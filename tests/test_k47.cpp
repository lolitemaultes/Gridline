// Gridline - K-47: the exports that left the interface thread (progress, Cancel, no partial target
// after a Cancel or a failure, whatever was there before stays) and a clipboard that refers to the
// unpacked copy of a zipped project that has closed, with the real cache and real locks.
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectArchive.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/Zip.h"
#include "Check.h"

using namespace gl;
namespace {
void expect(bool ok, const std::string& what) { expect(ok, what.c_str()); }
}  // namespace
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void spit(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << text;
}

// Big enough for several chunks, and not all zeros.
std::string audioFile(const fs::path& p, char fill, size_t megabytes = 3) {
  spit(p, std::string(megabytes << 20, fill));
  return p.u8string();
}

Song songWith(const std::vector<std::string>& files) {
  Song s = makeEmptySong();
  s.channels.clear();
  int k = 0;
  for (const auto& file : files) {
    Channel c;
    c.id = s.allocateId();
    c.name = "Channel " + std::to_string(++k);
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = file;
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
  }
  return s;
}

// Everything under a folder, by relative name.
std::vector<std::string> listing(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return out;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(fs::relative(it->path(), dir, ec).generic_u8string());
  std::sort(out.begin(), out.end());
  return out;
}

bool anyNamed(const fs::path& dir, const std::string& part) {
  for (const auto& n : listing(dir))
    if (n.find(part) != std::string::npos) return true;
  return false;
}

// A control that stops the export after `after` progress calls, optionally doing something then.
struct Stopper {
  std::atomic<bool> flag{false};
  int calls = 0, after = 0;
  double last = -1;
  bool backwards = false;
  std::function<void(double, const std::string&)> also;
  ExportControl control() {
    ExportControl c;
    c.cancel = &flag;
    c.progress = [this](double p, const std::string& what) {
      backwards = backwards || p < last;
      last = p;
      if (also) also(p, what);
      if (after > 0 && ++calls >= after) flag = true;
    };
    return c;
  }
};

void testZip(const fs::path& base) {
  const fs::path dir = base / "zip";
  const Song song = songWith({audioFile(dir / "src" / "one.wav", 'a'), audioFile(dir / "src" / "two.wav", 'b')});
  const fs::path out = dir / "out" / "Tune.gridline.zip";
  fs::create_directories(out.parent_path());

  {  // Cancel on a new target: nothing at all stays.
    Stopper st;
    st.after = 2;
    const ExportControl c = st.control();
    const CollectReport r = exportZippedProject(song, out.u8string(), "Tune", &c);
    expect(r.cancelled && r.error.empty(), "a cancelled zip says so, and it is no error");
    expect(listing(out.parent_path()).empty(), "a cancelled zip leaves nothing behind");
  }
  std::string whole;
  {  // To its end, with progress.
    Stopper st;
    const ExportControl c = st.control();
    const CollectReport r = exportZippedProject(song, out.u8string(), "Tune", &c);
    expect(r.error.empty() && !r.cancelled && r.copied == 2, "a zip with a control is written (" + r.error + ")");
    expect(!st.backwards, "its progress only goes forward");
    expect(st.last >= 0.9 && st.last <= 1.0, "its progress gets to the end");
    whole = slurp(out);
    expect(!whole.empty(), "the archive is there");
    const fs::path plain = dir / "out2" / "Tune.gridline.zip";
    fs::create_directories(plain.parent_path());
    const CollectReport r2 = exportZippedProject(song, plain.u8string(), "Tune");
    expect(r2.error.empty() && slurp(plain) == whole, "the same bytes as without a control");
  }
  {  // Cancel over an archive that is there: it stays as it was.
    Stopper st;
    st.after = 3;
    const ExportControl c = st.control();
    const CollectReport r = exportZippedProject(song, out.u8string(), "Tune", &c);
    expect(r.cancelled, "a cancel over an existing archive");
    expect(slurp(out) == whole, "the archive that was there is untouched");
    expect(listing(out.parent_path()).size() == 1, "and no temporary file stays");
  }
  {  // A file that goes away in the middle: an error, the target stays.
    const fs::path gone = dir / "src" / "three.wav";
    const Song s3 = songWith({audioFile(dir / "src" / "first.wav", 'c'), audioFile(gone, 'd')});
    Stopper st;
    st.also = [&](double, const std::string&) {
      std::error_code ec;
      fs::remove(gone, ec);
    };
    const ExportControl c = st.control();
    const CollectReport r = exportZippedProject(s3, out.u8string(), "Tune", &c);
    expect(!r.error.empty() && !r.cancelled, "a file gone in the middle is an error");
    expect(slurp(out) == whole, "the archive that was there is untouched after the failure");
    expect(listing(out.parent_path()).size() == 1, "and no temporary file stays after it");
  }
  {  // A target that cannot be written.
    Stopper st;
    const ExportControl c = st.control();
    const fs::path folderTarget = dir / "a folder.zip";
    fs::create_directories(folderTarget);
    const CollectReport r = exportZippedProject(song, folderTarget.u8string(), "Tune", &c);
    expect(!r.error.empty(), "a target that cannot be written is an error");
  }
}

void testDataFiles(const fs::path& base) {
  const fs::path dir = base / "data";
  const std::string one = audioFile(dir / "src" / "one.wav", 'a'), two = audioFile(dir / "src" / "two.wav", 'b');
  const Song song = songWith({one, two});
  const fs::path out = dir / "out" / "Tune";

  {  // Cancel in a new folder: the folder goes.
    Stopper st;
    st.after = 2;
    const ExportControl c = st.control();
    const CollectReport r = exportProjectDataFiles(song, out.u8string(), "Tune", &c);
    expect(r.cancelled && r.error.empty(), "cancelled data files say so");
    expect(!fs::exists(out), "a cancelled export removes the folder it made");
  }
  {
    Stopper st;
    const ExportControl c = st.control();
    const CollectReport r = exportProjectDataFiles(song, out.u8string(), "Tune", &c);
    expect(r.error.empty() && r.copied == 2, "data files with a control (" + r.error + ")");
    expect(fs::exists(out / "Tune.gridline.json") && fs::file_size(out / "Samples" / "two.wav") == (3u << 20), "the project and whole samples are there");
    expect(!anyNamed(out, ".part"), "no temporary names stay");
    Song back;
    expect(loadProject((out / "Tune.gridline.json").u8string(), back, nullptr), "the project reads back");
    expect(missingFiles(back).empty(), "and finds its samples");
  }
  const std::vector<std::string> before = listing(out);
  const std::string projectBefore = slurp(out / "Tune.gridline.json");
  {  // Cancel over an earlier export: what was there stays, what this run added goes.
    const std::string three = audioFile(dir / "src" / "three.wav", 'c');
    const Song more = songWith({three, one, two});
    Stopper st;
    st.after = 1;
    const ExportControl c = st.control();
    const CollectReport r = exportProjectDataFiles(more, out.u8string(), "Tune", &c);
    expect(r.cancelled, "a cancel over an earlier export");
    expect(listing(out) == before, "that export's files are as they were, the new ones gone");
    expect(slurp(out / "Tune.gridline.json") == projectBefore, "its project too");
  }
  {  // A failure in the middle of a new folder: the folder goes, and it is an error.
    const fs::path out2 = dir / "out" / "Other";
    const fs::path gone = dir / "src" / "gone.wav";
    const Song s = songWith({one, audioFile(gone, 'e')});
#if !defined(_WIN32)
    // The source cannot be read once it is reached. (A test run as root reads anything: no check then.)
    Stopper st;
    st.also = [&](double, const std::string&) {
      std::error_code ec;
      fs::permissions(gone, fs::perms::none, ec);
    };
    const ExportControl c = st.control();
    const CollectReport r = exportProjectDataFiles(s, out2.u8string(), "Other", &c);
    if (std::ifstream(gone).good()) {
      expect(true, "(running as root: the file stayed readable)");
    } else {
      expect(!r.error.empty(), "an unreadable file in the middle is an error");
      expect(!fs::exists(out2), "and the folder it made is gone");
    }
#endif
    (void)out2;
    (void)s;
  }
  {  // collectFiles by itself (saving into a new folder).
    const fs::path out3 = dir / "out" / "Collected";
    Song s = songWith({one, two});
    Stopper st;
    st.after = 2;
    const ExportControl c = st.control();
    const CollectReport r = collectFiles(s, (out3 / "Samples").u8string(), "", &c);
    expect(r.cancelled, "a cancelled collect says so");
    expect(listing(out3 / "Samples").empty(), "and leaves no file");
  }
}

void testBones(const fs::path& base) {
  const fs::path dir = base / "bones";
  Song song = songWith({audioFile(dir / "src" / "one.wav", 'a')});
  Pattern p;
  p.id = song.allocateId();
  p.name = "Odd/Name";
  Note n;
  n.pitch = 60;
  n.length = 96;
  p.notes[song.channels[0].id].push_back(n);
  song.patterns.push_back(p);
  const fs::path out = dir / "out" / "Tune bones";
  {  // Cancel while the MIDI files and presets are written: it all goes.
    Stopper st;
    st.also = [&](double, const std::string& what) {
      if (what.find("MIDI") != std::string::npos) st.flag = true;
    };
    const ExportControl c = st.control();
    const BonesReport r = exportProjectBones(song, out.u8string(), "Tune", &c);
    expect(r.files.cancelled, "bones cancelled at the MIDI files");
    expect(!fs::exists(out), "and the folder it made is gone");
  }
  {
    Stopper st;
    const ExportControl c = st.control();
    const BonesReport r = exportProjectBones(song, out.u8string(), "Tune", &c);
    expect(r.files.error.empty() && r.midiFiles >= 1 && r.presets == 1, "bones with a control (" + r.files.error + ")");
    expect(anyNamed(out / "MIDI", "Odd_Name"), "a pattern name with a slash gets a file");
    expect(!anyNamed(out, ".part"), "no temporary names stay");
  }
  {  // MIDI cannot be written (a file stands where its folder should be): an error, what it added goes.
    const fs::path out2 = dir / "out" / "Blocked bones";
    spit(out2 / "MIDI", "in the way");
    Stopper st;
    const ExportControl c = st.control();
    const BonesReport r = exportProjectBones(song, out2.u8string(), "Tune", &c);
    expect(!r.files.error.empty(), "bones that cannot write their MIDI files report it");
    expect(listing(out2).size() == 1 && fs::is_regular_file(out2 / "MIDI"), "what was there stays, what it added is gone (" + std::to_string(listing(out2).size()) + ")");
  }
  {  // Without a control nothing changes: it runs to its end.
    const fs::path out3 = dir / "out" / "Plain bones";
    const BonesReport r = exportProjectBones(song, out3.u8string(), "Tune");
    expect(r.files.error.empty() && !r.files.cancelled && r.midiFiles >= 1, "bones without a control");
  }
}

void testSaveMarks() {
  Document d;
  d.reset(makeEmptySong());
  const uint64_t epoch = d.projectEpoch();
  expect(!d.modified(), "a new project is not modified");
  const ObjectId ch = d.addChannel("One", "gl.sampler", -1);
  expect(d.modified(), "an edit modifies it");
  const uint64_t atStart = d.saveSerial();  // a background save starts here
  d.renameChannel(ch, "Later");
  d.markSavedAt(atStart);
  expect(d.modified(), "an edit made during a background save stays unsaved");
  d.undo();
  expect(!d.modified(), "undoing it is the saved state");
  expect(d.projectEpoch() == epoch, "edits are the same project");
  d.reset(makeEmptySong());
  expect(d.projectEpoch() != epoch, "another project is another epoch");
}

// ---- the clipboard and the real cache ----------------------------------------------------------

struct CacheFixture {
  fs::path root, music, cache;
  archive::CacheRefs refs;
  archive::UnpackResult open(const fs::path& zip) const {
    archive::UnpackRequest u;
    u.zipPath = zip.u8string();
    u.cacheRoot = cache.u8string();
    u.refs = &refs;
    archive::UnpackResult r;
    std::string err;
    expect(archive::unpackForOpening(u, r, &err), "opens (" + err + ")");
    return r;
  }
};

CacheFixture cacheFixture(const fs::path& base, const char* name) {
  CacheFixture f;
  f.root = base / name;
  f.music = f.root / "My Music";
  f.cache = f.root / "cache" / "unpacked";
  std::error_code ec;
  fs::create_directories(f.music, ec);
  return f;
}

// A zipped project with an impulse response on the master track, by its name.
fs::path zipWithChain(const CacheFixture& f) {
  const fs::path zip = f.music / "Tune.zip";
  const std::string ir = audioFile(f.root / "impulse.wav", 'i', 1);
  Song s = makeEmptySong();
  s.channels.clear();
  s.mixer[0].slots[0].plugin = defaultPluginState("gl.convolver");
  s.mixer[0].slots[0].plugin.data = "impulse.wav";
  const std::string text = projectToText(s);
  ZipWriter w;
  expect(w.open(zip.u8string()), "the archive opens for writing");
  w.add("Tune.gridline.json", std::vector<uint8_t>(text.begin(), text.end()));
  w.addFile("impulse.wav", ir);
  expect(w.close(), "the archive is written");
  return zip;
}

// The chain as the clipboard keeps it: what Edit > Copy takes from the mixer track.
struct ChainClip {
  std::vector<EffectSlot> slots;
  std::vector<archive::CopyHold> holds;
  std::string file() const {
    MixerTrack t;
    t.slots = slots;
    Song refs;
    refs.mixer.push_back(t);
    const auto files = referencedFiles(refs);
    return files.empty() ? std::string() : files[0];
  }
};

ChainClip copyChain(const CacheFixture& f, const Song& s, bool hold) {
  ChainClip c;
  c.slots = s.mixer[0].slots;
  if (hold) {
    Song refs;
    MixerTrack t;
    t.slots = c.slots;
    refs.mixer.push_back(t);
    c.holds = archive::holdCopiesOf(refs, f.cache.u8string());
  }
  return c;
}

void testClipboard(const fs::path& base) {
  {  // Why this exists: without the hold, closing the project takes the file from the clip.
    CacheFixture f = cacheFixture(base, "nohold");
    const fs::path zip = zipWithChain(f);
    archive::UnpackResult r = f.open(zip);
    Song s;
    expect(loadProject(r.projectFile, s, nullptr), "the project loads");
    ChainClip clip = copyChain(f, s, false);
    expect(fs::exists(fs::u8path(clip.file())), "the clip's file is in the cached copy");
    r.use.reset();  // the project closes
    archive::sweepCache(f.cache.u8string(), archive::kCacheLimit, &f.refs);
    expect(!fs::exists(fs::u8path(clip.file())), "without a hold the sweep takes it");
  }
  CacheFixture f = cacheFixture(base, "hold");
  const fs::path zip = zipWithChain(f);
  archive::UnpackResult r = f.open(zip);
  Song s;
  expect(loadProject(r.projectFile, s, nullptr), "the project loads");
  ChainClip clip = copyChain(f, s, true);
  expect(clip.holds.size() == 1, "the clip holds the copy it refers to");
  const std::string file = clip.file();
  r.use.reset();  // the project closes
  const archive::CacheReport a = archive::sweepCache(f.cache.u8string(), archive::kCacheLimit, &f.refs);
  expect(a.removed == 0 && a.keptOpen == 1, "the closed project's copy stays while the clip refers to it");
  expect(fs::file_size(fs::u8path(file)) == (1u << 20), "with its file, whole");
  const archive::CacheReport cl = archive::clearCache(f.cache.u8string(), &f.refs);
  expect(cl.removed == 0 && fs::exists(fs::u8path(file)), "Clear cache leaves it too");

  // The paste: a new project takes the chain, and from here on the project claims the copy.
  Song other = makeEmptySong();
  other.mixer[0].slots = clip.slots;
  std::vector<archive::CopyHold> projectHolds = archive::holdCopiesOf(other, f.cache.u8string());
  expect(projectHolds.size() == 1 && missingFiles(other).empty(), "the pasted chain finds its file");
  clip.holds.clear();  // something else is copied
  clip.slots.clear();
  archive::sweepCache(f.cache.u8string(), archive::kCacheLimit, &f.refs);
  expect(missingFiles(other).empty(), "the file stays with the project that pasted it");
  // That project closes unsaved: nothing needs the copy any more.
  projectHolds.clear();
  const archive::CacheReport end = archive::sweepCache(f.cache.u8string(), archive::kCacheLimit, &f.refs);
  expect(end.removed == 1 && !fs::exists(fs::u8path(file)), "and goes when nothing refers to it");

  {  // A copy that is no longer there: nothing to hold, and it is not made up.
    Song ghost = makeEmptySong();
    ghost.mixer[0].slots[0].plugin = defaultPluginState("gl.convolver");
    ghost.mixer[0].slots[0].plugin.data = (f.cache / "Gone-0123" / "impulse.wav").u8string();
    expect(archive::holdCopiesOf(ghost, f.cache.u8string()).empty(), "a copy that went holds nothing");
    expect(!fs::exists(f.cache / "Gone-0123"), "and is not made again");
  }
  {  // Nothing in the cache: no work, no holds.
    Song plain = songWith({"/somewhere/else/a.wav"});
    expect(archive::holdCopiesOf(plain, f.cache.u8string()).empty(), "a song outside the cache holds nothing");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path base = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("k47_units"));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  testZip(base);
  testDataFiles(base);
  testBones(base);
  testSaveMarks();
  testClipboard(base);
  fs::remove_all(base, ec);
  return finish();
}
