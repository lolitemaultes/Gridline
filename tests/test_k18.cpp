// Gridline: K-16 / K-18: the browser's file operations (core/FileOps.h) and favourite plugins.
//   K-18: names checked; rename (sidecars along, collisions, a change of letter case), new folder,
//   move into a folder (into itself refused), the trash (a freedesktop-style trash in a test
//   folder, and the native one under a test HOME), a read-only folder refused; the open project's
//   references follow a rename or a folder move as one undo step that moves the files back on
//   undo and again on redo; the browser index and its tags follow.
//   K-32: paths that differ only in letter case, by the rule (scripted) and on the disk this runs on;
//   GRIDLINE_TEST_OTHER_VOLUME names a folder on a volume with the other rule (a disk image).
//   K-16: favourites round trip through the settings file (a restart), favourites-first ordering.
// Every file operation runs in this test's own temp folder (Hermetic.h).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/Document.h"
#include "core/FileOps.h"
#include "core/Json.h"
#include "core/Platform.h"
#include "core/ProjectInterop.h"
#include "core/Song.h"
#include "host/PluginPrefs.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace gl;
namespace fs = std::filesystem;

namespace {

fs::path root;

void touch(const fs::path& p, const std::string& body = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << body;
}
bool has(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

void testNames() {
  expect(fileops::checkName("Kick 01").empty(), "a plain name is fine");
  expect(fileops::checkName("kick.wav").empty(), "a name with an extension is fine");
  expect(!fileops::checkName("").empty() && !fileops::checkName("   ").empty(), "empty names are refused");
  expect(!fileops::checkName("a/b").empty() && !fileops::checkName("a:b").empty() && !fileops::checkName("a?").empty(), "reserved characters are refused");
  expect(!fileops::checkName("..").empty() && !fileops::checkName(".hidden").empty(), "dot names are refused");
  expect(!fileops::checkName("CON").empty() && !fileops::checkName("com1.wav").empty(), "names Windows reserves are refused");
  expect(!fileops::checkName("name.").empty() && !fileops::checkName("name ").empty(), "a trailing dot or space is refused");
  expect(!fileops::checkName(std::string(300, 'a')).empty(), "too long is refused");
}

void testPaths() {
  expect(fileops::isInside("/a/b/c.wav", "/a/b") && fileops::isInside("/a/b", "/a/b"), "isInside: a file in a folder, the folder itself");
  expect(!fileops::isInside("/a/bc/d.wav", "/a/b"), "isInside: a sibling with the same prefix is not inside");
  // Absolute on this system: "/a/b" has no drive letter, so on Windows it is a relative path.
#ifdef _WIN32
  const std::string drive = "C:";
#else
  const std::string drive;
#endif
  std::string p = drive + "/a/b/c.wav";
  expect(fileops::retargetPath(p, drive + "/a/b/c.wav", drive + "/a/b/d.wav") && p == drive + "/a/b/d.wav", "retarget: the file itself");
  p = drive + "/a/b/sub/c.wav";
  expect(fileops::retargetPath(p, drive + "/a/b", drive + "/a/x") && fs::path(p) == fs::path(drive + "/a/x/sub/c.wav"), "retarget: a file inside a moved folder");
  p = drive + "/a/bc/c.wav";
  expect(!fileops::retargetPath(p, drive + "/a/b", drive + "/a/x") && p == drive + "/a/bc/c.wav", "retarget: a sibling folder stays");
  p = "rel/c.wav";
  expect(!fileops::retargetPath(p, "rel/c.wav", drive + "/x.wav"), "retarget: relative references are left alone");
#ifdef _WIN32
  // Both spellings of one path meet: an FL import writes forward slashes, the browser backslashes.
  p = "C:/a/b/sub/c.wav";
  expect(fileops::retargetPath(p, "C:\\a\\b", "C:\\a\\x") && p == "C:\\a\\x\\sub\\c.wav", "retarget: forward slashes in the reference, backslashes in the move");
  p = "C:\\a\\b\\c.wav";
  expect(fileops::retargetPath(p, "C:/a/b/c.wav", "C:/a/b/d.wav") && p == "C:/a/b/d.wav", "retarget: backslashes in the reference, forward slashes in the move");
  // Mixed separators in the move (a folder path joined from two spellings) do not reach the reference.
  p = "C:\\t/g\\old\\c.wav";
  expect(fileops::retargetPath(p, "C:\\t/g\\old", "C:\\t/g\\new\\old") && p == "C:\\t\\g\\new\\old\\c.wav", "retarget: a move with mixed separators writes one spelling");
#endif
}

// A Windows path in a plugin's JSON state is one reference on every system: its backslashes
// are escaped, none starts an escape (the "\t" of "\take.wav"), and a rewrite keeps them.
void testBackslashesInJsonState() {
  const std::string from = "C:\\Music\\Edited audio\\take.wav", to = "D:\\new\\Edited audio\\take.wav";
  Json j = Json::makeObject();
  j.object["path"] = Json::makeString(from);
  PluginState st;
  st.type = "gl.edison";
  st.data = jsonStringify(j, 0);
  std::vector<std::string> seen;
  const bool changed = forEachStateFileRef(st, [&](std::string& p) {
    seen.push_back(p);
    p = to;
  });
  expect(seen == std::vector<std::string>{from}, "JSON state: a Windows path reaches the reference walk whole");
  Json back;
  expect(changed && jsonParse(st.data, back) && back.text("path") == to, "JSON state: a rewritten Windows path reads back whole");
}

void testRename() {
  const fs::path d = root / "rename";
  touch(d / "Snap.wav", "snap");
  touch(d / "Snap.wav.origin.json", "{}");
  touch(d / "Snap.wav.source.json", "{}");
  touch(d / "Tick.wav", "tick");
  auto r = fileops::rename((d / "Snap.wav").string(), "Kick.wav");
  expect(r.ok && has(d / "Kick.wav") && !has(d / "Snap.wav"), "rename: the file has its new name");
  expect(has(d / "Kick.wav.origin.json") && has(d / "Kick.wav.source.json") && !has(d / "Snap.wav.origin.json"), "rename: its sidecars follow");
  expect(r.moves.size() == 3 && r.moves[0].second == (d / "Kick.wav").string(), "rename: the moves list the file first, then the sidecars");
  r = fileops::rename((d / "Tick.wav").string(), "Kick.wav");
  expect(!r.ok && r.error.find("already exists") != std::string::npos && has(d / "Tick.wav"), "rename onto an existing name is refused, nothing moves");
  r = fileops::rename((d / "Tick.wav").string(), "a/b.wav");
  expect(!r.ok && has(d / "Tick.wav"), "rename to an invalid name is refused");
  r = fileops::rename((d / "Kick.wav").string(), "kick.wav");
  bool listed = false;
  for (const auto& e : fs::directory_iterator(d)) listed = listed || e.path().filename() == "kick.wav";
  expect(r.ok && listed, "a change of letter case only (case-insensitive file systems too)");
  r = fileops::rename((d / "nothing.wav").string(), "x.wav");
  expect(!r.ok && !r.error.empty(), "a missing file is reported");
  // Folders.
  touch(d / "Loops" / "a.wav");
  r = fileops::rename((d / "Loops").string(), "Grooves");
  expect(r.ok && has(d / "Grooves" / "a.wav") && !has(d / "Loops"), "rename a folder: its contents go with it");
}

void testMoveAndFolders() {
  const fs::path d = root / "move";
  touch(d / "a.wav");
  touch(d / "a.wav.origin.json", "{}");
  touch(d / "Sub" / "a.wav");
  auto f = fileops::makeFolder(d.string());
  expect(f.ok && fs::is_directory(f.path) && fs::path(f.path).filename() == "New folder", "new folder: \"New folder\"");
  auto f2 = fileops::makeFolder(d.string());
  expect(f2.ok && fs::path(f2.path).filename() == "New folder 2", "a second one: \"New folder 2\"");
  auto r = fileops::moveInto((d / "a.wav").string(), f.path);
  expect(r.ok && has(fs::path(f.path) / "a.wav") && has(fs::path(f.path) / "a.wav.origin.json") && !has(d / "a.wav"), "move into a folder, sidecar along");
  r = fileops::moveInto((fs::path(f.path) / "a.wav").string(), (d / "Sub").string());
  expect(!r.ok && r.error.find("already exists") != std::string::npos, "a move onto an existing name is refused");
  r = fileops::moveInto(f.path, (fs::path(f.path)).string());
  expect(!r.ok, "a folder cannot go into itself");
  fs::create_directories(fs::path(f.path) / "Inner");
  r = fileops::moveInto(f.path, (fs::path(f.path) / "Inner").string());
  expect(!r.ok, "... nor into one of its subfolders");
  r = fileops::moveInto((d / "Sub").string(), f2.path);
  expect(r.ok && has(fs::path(f2.path) / "Sub" / "a.wav"), "a folder moves into another folder");
  std::string err;
  expect(fileops::moveBack(r.moves, &err) && has(d / "Sub" / "a.wav"), "moveBack undoes it");
  expect(fileops::moveAgain(r.moves, &err) && has(fs::path(f2.path) / "Sub" / "a.wav"), "moveAgain redoes it");
}

void testTrash() {
  const fs::path d = root / "trash";
  touch(d / "gone.wav", "1");
  touch(d / "gone.wav.origin.json", "{}");
  touch(d / "again" / "gone.wav", "2");
  const fs::path t = root / "testtrash";
  auto r = fileops::moveToTrash((d / "gone.wav").string(), t.string());
  expect(r.ok && !has(d / "gone.wav") && !has(d / "gone.wav.origin.json"), "trash: the file and its sidecar leave the folder");
  expect(has(t / "files" / "gone.wav") && has(t / "files" / "gone.wav.origin.json"), "trash: they are in the trash's files");
#if !defined(_WIN32)
  expect(has(t / "info" / "gone.wav.trashinfo"), "trash: with a .trashinfo (freedesktop)");
  std::ifstream info(t / "info" / "gone.wav.trashinfo");
  std::string body((std::istreambuf_iterator<char>(info)), std::istreambuf_iterator<char>());
  expect(body.rfind("[Trash Info]\nPath=/", 0) == 0 && body.find("DeletionDate=") != std::string::npos, "trash: the .trashinfo names the place and date");
#endif
  r = fileops::moveToTrash((d / "again" / "gone.wav").string(), t.string());
  expect(r.ok && has(t / "files" / "gone 2.wav"), "trash: a second item of that name gets its own name");
  r = fileops::moveToTrash((d / "again").string(), t.string());
  expect(r.ok && !has(d / "again"), "trash: a folder");
#if !defined(_WIN32)
  // The native trash under a test HOME / XDG_DATA_HOME (never the user's).
  const fs::path home = root / "home";
  fs::create_directories(home);
  platform::setEnv("HOME", home.string().c_str());
  platform::setEnv("XDG_DATA_HOME", (home / "xdg").string().c_str());
  touch(d / "native.wav");
  r = fileops::moveToTrash((d / "native.wav").string());
#if defined(__APPLE__)
  expect(r.ok && has(home / ".Trash" / "native.wav"), "native trash: ~/.Trash (macOS)");
#else
  expect(r.ok && has(home / "xdg" / "Trash" / "files" / "native.wav"), "native trash: the freedesktop trash in XDG_DATA_HOME");
#endif
  // A read-only folder: nothing can be renamed in it.
  const fs::path ro = root / "readonly";
  touch(ro / "locked.wav");
  ::chmod(ro.string().c_str(), 0555);
  if (::access(ro.string().c_str(), W_OK) != 0) {  // (root ignores permissions)
    r = fileops::rename((ro / "locked.wav").string(), "free.wav");
    expect(!r.ok && has(ro / "locked.wav"), "a read-only folder: rename refused with a message");
    expect(!platform::writable(ro.string()), "platform::writable says so (the browser shows the reason)");
  }
  ::chmod(ro.string().c_str(), 0755);
#endif
}

Song songUsing(const std::string& sample, const std::string& loupeTake) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Kick";
  c.plugin.type = "gl.sampler";
  c.plugin.data = sample;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  if (s.mixer.size() > 1) {
    s.mixer[1].slots[0].plugin.type = "gl.edison";
    Json state = Json::makeObject();  // as the plugin writes it (edison_io_dsp.cpp): backslashes escaped
    state.object["path"] = Json::makeString(loupeTake);
    state.object["v"] = Json::makeNumber(1);
    s.mixer[1].slots[0].plugin.data = jsonStringify(state, 0);
  }
  return s;
}

void testProjectSide() {
  const fs::path d = root / "project";
  touch(d / "Drums" / "kick.wav");
  touch(d / "Edited audio" / "take.wav");
  Document doc;
  doc.reset(songUsing((d / "Drums" / "kick.wav").string(), (d / "Edited audio" / "take.wav").string()));
  expect(fileops::referencesTo(doc.song(), (d / "Drums").string()) == 1, "references into a folder are counted");
  // Rename the file: the channel follows, one undo step.
  auto r = fileops::rename((d / "Drums" / "kick.wav").string(), "Kick 01.wav");
  std::vector<std::string> heard;
  doc.execute(std::make_unique<fileops::FileMoveCommand>("Renamed", r.moves, std::vector<fileops::Move>{r.moves.front()},
                                                         [&heard](const std::string& from, const std::string& to) { heard.push_back(from + ">" + to); }));
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "Kick 01.wav").string(), "rename: the channel's sample follows");
  expect(doc.history().position() == 1 && doc.history().undoLabel() == "Renamed", "... one undo step");
  doc.undo();
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "kick.wav").string() && has(d / "Drums" / "kick.wav") &&
             !has(d / "Drums" / "Kick 01.wav"),
         "undo: the reference and the file go back");
  doc.redo();
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "Kick 01.wav").string() && has(d / "Drums" / "Kick 01.wav"),
         "redo: renamed again (file and reference)");
  expect(heard.size() == 2, "the index hears undo and redo");
  // Move a folder: the Loupe take inside its JSON state follows.
  touch(d / "Archive" / ".keep");
  r = fileops::moveInto((d / "Edited audio").string(), (d / "Archive").string());
  doc.execute(std::make_unique<fileops::FileMoveCommand>("Moved", r.moves, std::vector<fileops::Move>{r.moves.front()}));
  // Compared as paths, not as strings: the test's own root holds a '/' on Windows (Hermetic.h joins
  // "<temp>/gridline-...") and a retargeted reference comes out in one spelling, all backslashes.
  const auto refs = referencedFiles(doc.song());
  const fs::path movedTake = d / "Archive" / "Edited audio" / "take.wav";
  const auto followed = std::find_if(refs.begin(), refs.end(), [&](const std::string& f) { return fs::path(f) == movedTake; });
  if (followed == refs.end() || !has(fs::path(*followed)))
    for (const auto& f : refs) std::printf("# wanted %s, reference [%s]\n", movedTake.string().c_str(), f.c_str());
  expect(followed != refs.end(), "a moved folder: the references inside a plugin's JSON state follow");
  expect(followed != refs.end() && has(fs::path(*followed)), "a moved folder: the reference it wrote opens the moved file");
  doc.undo();
  expect(has(d / "Edited audio" / "take.wav") && fileops::referencesTo(doc.song(), (d / "Edited audio").string()) == 1, "undo: folder and reference back");
}

void testIndex() {
  browse::Index ix;
  ix.setCacheFile((root / "index.json").string());
  const std::string a = (root / "ix" / "a.wav").string(), b = (root / "ix" / "sub" / "b.wav").string();
  ix.setTags(a, {"starred"});
  ix.setTags(b, {"kick"});
  ix.movePath(a, (root / "ix" / "renamed.wav").string());
  expect(ix.tagsOf(a).empty() && ix.tagsOf((root / "ix" / "renamed.wav").string()) == std::vector<std::string>{"starred"}, "index: a renamed file keeps its tags");
  ix.movePath((root / "ix" / "sub").string(), (root / "ix" / "moved").string());
  expect(ix.tagsOf((root / "ix" / "moved" / "b.wav").string()) == std::vector<std::string>{"kick"}, "index: entries inside a moved folder follow");
  ix.movePath((root / "ix" / "moved").string(), "");
  expect(ix.tagsOf((root / "ix" / "moved" / "b.wav").string()).empty(), "index: a trashed folder is forgotten");
}

void testFavourites() {
  const std::string cfg = (root / "cfg").string();
  fs::create_directories(cfg);
  host::loadPluginPrefs(cfg);
  expect(!host::isFavourite("gl.reverb"), "favourites: none at first");
  expect(host::setFavourite("gl.reverb", true) && host::setFavourite("vst3:ABCDEF", true), "favourites: a built-in and a hosted type");
  expect(!host::setFavourite("gl.reverb", true), "favourites: setting it again changes nothing");
  host::loadPluginPrefs((root / "empty").string());  // another config: nothing
  expect(!host::isFavourite("gl.reverb"), "favourites: another user's settings do not have them");
  host::loadPluginPrefs(cfg);  // a restart
  expect(host::isFavourite("gl.reverb") && host::isFavourite("vst3:ABCDEF"), "favourites: kept across a restart (settings round trip)");
  expect(host::setFavourite("vst3:ABCDEF", false) && !host::isFavourite("vst3:ABCDEF"), "favourites: removed");
  host::loadPluginPrefs(cfg);
  expect(!host::isFavourite("vst3:ABCDEF") && host::isFavourite("gl.reverb"), "favourites: the removal is saved too");
  // Picker order: favourites first, each part in its own order.
  std::vector<std::string> list = {"gl.chorus", "gl.delay", "gl.eq", "gl.reverb", "vst3:X"};
  const std::set<std::string> favs = {"gl.reverb", "gl.delay"};
  host::favouritesFirst(list, favs, [](const std::string& t) { return t; });
  expect((list == std::vector<std::string>{"gl.delay", "gl.reverb", "gl.chorus", "gl.eq", "vst3:X"}), "picker order: favourites first, both parts keep their order");
}

}  // namespace

// K-32, the rule itself: a scripted answer to "does this folder ignore case?", on every system.
void testCaseRule() {
#ifdef _WIN32
  const std::string drive = "C:";
#else
  const std::string drive;
#endif
  int asked = 0;
  fileops::setCaseRuleForTests([&asked](const std::string&) {
    ++asked;
    return true;
  });
  std::string p = drive + "/Proj/DRUMS/Sub Dir/Kick.WAV";
  expect(fileops::isInside(p, drive + "/proj/drums"), "ignoring case: a folder spelled differently holds the file");
  expect(!fileops::isInside(drive + "/proj/drumsets/a.wav", drive + "/PROJ/DRUMS"), "ignoring case: a sibling with the same prefix is still not inside");
  expect(!fileops::isInside(drive + "/proj", drive + "/PROJ/DRUMS"), "ignoring case: a parent is not inside its child");
  expect(fileops::retargetPath(p, drive + "/proj/drums", drive + "/proj/moved"), "ignoring case: the reference follows a moved folder");
  expect(fs::path(p) == fs::path(drive + "/proj/moved/Sub Dir/Kick.WAV"), "ignoring case: below the move it keeps its own spelling");
  p = drive + "/Proj/KICK.wav";
  expect(fileops::retargetPath(p, drive + "/proj/kick.WAV", drive + "/proj/Snare.wav") && p == drive + "/proj/Snare.wav", "ignoring case: the file itself follows");
  p = drive + "/Proj/snare.wav";
  expect(!fileops::retargetPath(p, drive + "/proj/kick.wav", drive + "/proj/Snare.wav") && p == drive + "/Proj/snare.wav", "ignoring case: an unrelated reference keeps its spelling");
  p = drive + "/a/Loops/x.wav";
  expect(fileops::retargetPath(p, drive + "/a/Loops", drive + "/a/loops") && fs::path(p) == fs::path(drive + "/a/loops/x.wav"), "a folder's change of case reaches what is inside it");
  expect(!fileops::retargetPath(p, drive + "/a/loops", drive + "/a/loops"), "... and a reference already spelled so does not count as changed");
#ifdef _WIN32
  p = "c:\\Samples\\Kick.wav";
  expect(fileops::retargetPath(p, "C:\\samples", "D:\\Moved") && p == "D:\\Moved\\Kick.wav", "ignoring case: another drive-letter case follows too");
  expect(fileops::isInside("c:/Samples/Kick.wav", "C:\\"), "ignoring case: a drive root holds its files");
#else
  expect(fileops::isInside("c:/Samples/Kick.wav", "C:/samples"), "ignoring case: drive letters are compared the same way");
#endif
  // The volume is only asked when spellings differ, and not once per call.
  fileops::setCaseRuleForTests([&asked](const std::string&) {
    ++asked;
    return true;
  });
  asked = 0;
  for (int k = 0; k < 2000; ++k) {
    fileops::isInside("/Same/Spelling/x.wav", "/Same/Spelling");
    fileops::isInside("/Same/Spelling/x.wav", "/Other/Folder");
    fileops::isInside("/Same/Spelling/x.wav", "/Same/Spellings");
  }
  expect(asked == 0, "the same spelling, or another name, never asks the volume");
  for (int k = 0; k < 2000; ++k) fileops::isInside("/Same/Spelling/x.wav", "/same/spelling");
  expect(asked == 1, "another spelling asks once for 2000 frames");
  // A file system that does not ignore case.
  fileops::setCaseRuleForTests([](const std::string&) { return false; });
  p = drive + "/Proj/DRUMS/Kick.wav";
  expect(!fileops::isInside(p, drive + "/proj/drums"), "case-sensitive: another spelling is another folder");
  expect(!fileops::retargetPath(p, drive + "/proj/drums", drive + "/proj/moved") && p == drive + "/Proj/DRUMS/Kick.wav", "case-sensitive: the other spelling stays");
  expect(fileops::retargetPath(p, drive + "/Proj/DRUMS", drive + "/Proj/moved"), "case-sensitive: the exact spelling follows");
  fileops::setCaseRuleForTests({});
#if !defined(_WIN32) && !defined(__APPLE__)
  expect(!fileops::isInside("/Proj/DRUMS/Kick.wav", "/proj/drums"), "Linux stays case-sensitive");
  p = "/Proj/DRUMS/Kick.wav";
  expect(!fileops::retargetPath(p, "/proj/drums", "/x") && p == "/Proj/DRUMS/Kick.wav", "Linux stays case-sensitive: the reference stays");
#endif
#ifdef _WIN32
  expect(fileops::isInside("C:/Proj/DRUMS/Kick.wav", "c:\\proj\\drums"), "Windows ignores case without asking anything");
#endif
}

// `dir` is a scratch folder on the volume under test; what the disk really does decides what is expected.
void testCaseOnDisk(const fs::path& d, const char* tag) {
  auto check = [tag](bool ok, const char* name) { expect(ok, (std::string(tag) + ": " + name).c_str()); };
  touch(d / "CaseProbe");
  const bool ignores = has(d / "caseprobe");
  std::printf("# %s: this volume %s case\n", tag, ignores ? "ignores" : "keeps");
  touch(d / "Drums" / "Sub" / "Kick.wav");
  touch(d / "Drums" / "Sub" / "Kick.wav.origin.json", "{}");
  touch(d / "Archive" / ".keep");
  // A reference with the folder spelled otherwise: what an FL import or an edited project can hold.
  const std::string spelled = (d / "DRUMS" / "Sub" / "Kick.wav").string();
  Document doc;
  doc.reset(songUsing(ignores ? spelled : (d / "Drums" / "Sub" / "Kick.wav").string(), (d / "unused.wav").string()));
  check(fileops::referencesTo(doc.song(), (d / "Drums").string()) == 1, "references are counted through the other spelling");
  auto r = fileops::moveInto((d / "Drums").string(), (d / "Archive").string());
  check(r.ok && has(d / "Archive" / "Drums" / "Sub" / "Kick.wav"), "the folder moved");
  doc.execute(std::make_unique<fileops::FileMoveCommand>("Moved", r.moves, std::vector<fileops::Move>{r.moves.front()}));
  check(fs::path(doc.song().channels.back().plugin.data) == d / "Archive" / "Drums" / "Sub" / "Kick.wav", "the reference follows the move");
  check(has(fs::path(doc.song().channels.back().plugin.data)), "the reference opens the moved file");
  doc.undo();
  check(has(d / "Drums" / "Sub" / "Kick.wav") && fileops::referencesTo(doc.song(), (d / "Drums").string()) == 1, "undo: file and reference back");
  check(fileops::isInside(spelled, (d / "Drums").string()) == ignores, "the other spelling is the same folder only where the volume says so");
  // Moved "into" the folder it is in, spelled otherwise.
  r = fileops::moveInto((d / "Drums" / "Sub" / "Kick.wav").string(), (d / "Drums" / "SUB").string());
  if (ignores) check(r.ok && r.moves.empty() && has(d / "Drums" / "Sub" / "Kick.wav"), "moving into its own folder, spelled otherwise, does nothing");
  else check(!r.ok, "the other spelling is no folder here");
  // A change of letter case only: rename, undo, redo, with its sidecar, on disk and in the project.
  Document doc2;
  doc2.reset(songUsing((d / "Drums" / "Sub" / "Kick.wav").string(), (d / "unused.wav").string()));
  r = fileops::rename((d / "Drums" / "Sub" / "Kick.wav").string(), "KICK.wav");
  auto listed = [&](const char* name) {
    for (const auto& e : fs::directory_iterator(d / "Drums" / "Sub"))
      if (e.path().filename() == name) return true;
    return false;
  };
  check(r.ok && listed("KICK.wav") && !listed("Kick.wav"), "a change of case: the disk has the new spelling");
  check(listed("KICK.wav.origin.json") && !listed("Kick.wav.origin.json") && r.moves.size() == 2, "a change of case: the sidecar goes along");
  doc2.execute(std::make_unique<fileops::FileMoveCommand>("Renamed", r.moves, std::vector<fileops::Move>{r.moves.front()}));
  check(doc2.song().channels.back().plugin.data == (d / "Drums" / "Sub" / "KICK.wav").string(), "a change of case: the reference follows");
  doc2.undo();
  check(listed("Kick.wav") && !listed("KICK.wav") && listed("Kick.wav.origin.json") && !listed("KICK.wav.origin.json"), "a change of case: undo restores the spelling on disk");
  check(doc2.song().channels.back().plugin.data == (d / "Drums" / "Sub" / "Kick.wav").string(), "a change of case: undo restores the reference");
  doc2.redo();
  check(listed("KICK.wav") && !listed("Kick.wav"), "a change of case: redo");
  // A different item at the target still blocks an undo, on every file system.
  std::string err;
  touch(d / "Other" / "a.wav");
  touch(d / "Other" / "b.wav");
  check(!fileops::moveBack({{(d / "Other" / "b.wav").string(), (d / "Other" / "a.wav").string()}}, &err) && has(d / "Other" / "a.wav"), "another item at the target blocks moving back");
  // The browser index: entries stored in the other spelling follow too.
  const fs::path cache = d / "ix.json";
  browse::Index ix;
  ix.setCacheFile(cache.string());
  ix.setTags((d / "Tree" / "One.wav").string(), {"keep"});
  ix.movePath((d / "TREE").string(), (d / "Moved").string());
  const std::string wanted = ((ignores ? d / "Moved" : d / "Tree") / "One.wav").string();
  check(!ix.tagsOf(wanted).empty(), "the index follows the other spelling where the volume ignores case");
}

#if defined(__APPLE__)
// What folding says against what this volume does, for names beyond ASCII. A name the volume keeps
// apart must not be taken for one: a reference would go to the wrong file.
void testFoldingAgainstVolume() {
  const fs::path d = root / "fold";
  fs::create_directories(d);
  touch(d / "CaseProbe");
  if (!has(d / "caseprobe")) return;
  struct Pair {
    const char* a;
    const char* b;
  };
  const Pair pairs[] = {
      {"\xC3\x84rger", "\xC3\xA4RGER"},        // A diaeresis, both cases
      {"caf\xC3\xA9", "cafe\xCC\x81"},         // composed, decomposed
      {"CAF\xC3\x89", "cafe\xCC\x81"},         // and both at once
      {"\xCE\xA3\xCE\xA1", "\xCF\x83\xCF\x81"},  // Greek
      {"\xD0\x96", "\xD0\xB6"},                // Cyrillic
      {"stra\xC3\x9F" "e", "STRASSE"},         // sharp s
      {"\xC4\xB1", "I"},                       // dotless i
      {"\xC4\xB0", "i"},                       // dotted capital I
      {"\xE2\x84\xAA", "k"},                   // Kelvin sign
      {"\xEF\xAC\x81", "FI"},                  // ligature
      {"\xCF\x82", "\xCF\x83"},                // final sigma
      {"a", "b"},
  };
  int k = 0;
  for (const Pair& pr : pairs) {
    const fs::path cell = d / std::to_string(k++);
    touch(cell / pr.a);
    const bool onDisk = has(cell / pr.b);
    const bool ours = fileops::isInside((cell / pr.b).string(), (cell / pr.a).string());
    std::printf("# fold pair %d: volume %d, compare %d\n", k, onDisk ? 1 : 0, ours ? 1 : 0);
    expect(onDisk == ours, ("the comparison agrees with the volume: pair " + std::to_string(k)).c_str());
  }
}
#endif

int main() {
  root = fs::path(test::hermeticTestEnv("k18")) / "files";
  fs::create_directories(root);
  testNames();
  testPaths();
  testBackslashesInJsonState();
  testRename();
  testMoveAndFolders();
  testTrash();
  testProjectSide();
  testIndex();
  testFavourites();
  testCaseRule();
  testCaseOnDisk(root / "case", "here");
#if defined(__APPLE__)
  testFoldingAgainstVolume();
#endif
  // A folder on a volume with the other rule, when a run provides one (scripts: a disk image).
  if (const char* other = std::getenv("GRIDLINE_TEST_OTHER_VOLUME"); other && *other) testCaseOnDisk(fs::path(other) / "k32", "other volume");
  return finish();
}
