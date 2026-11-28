// Gridline - a Tracker Sampler's samples kept with it (presets, files, moved folders) and the Sample
// editor's Effects. Every module, sample and folder is made here (no user files).
//   * a preset of an imported module's instrument keeps its sound after the unpack cache is cleared
//     (its samples are carried beside the preset, named relative to it), in the library and in a file
//     of the person's choice; a sample from the person's own folder is only referred to;
//   * presets and projects saved with the samples in Gridline's own folders at an older place (another
//     HOME or settings folder, a module unpacked again under another version) find them where they are;
//   * a sample file that was missing when the instrument was built plays once it is there again;
//   * the instrument's samples written as WAV files;
//   * effects run over a sample: every offered effect gives finite sound, a delay's tail grows the
//     sample, a range takes effect only there, a mono sample turns stereo through a stereo effect.
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../../src/core/Json.h"
#include "../../src/core/Presets.h"
#include "../../src/core/Project.h"
#include "../../src/core/ProjectArchive.h"
#include "../../src/core/ProjectInterop.h"
#include "../../src/core/TrackerImport.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/tracker/tracker.h"
#include "../../src/packs/tracker/tracker_session.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;

namespace {

// A four-channel module with two samples (a square and a sine), each played once.
std::vector<uint8_t> makeMod() {
  std::vector<uint8_t> b(1084, 0);
  std::memcpy(b.data(), "k435 test", 9);
  const int lens[2] = {800, 1200};
  const char* names[2] = {"square", "sine"};
  for (int k = 0; k < 2; ++k) {
    uint8_t* sh = b.data() + 20 + k * 30;
    std::memcpy(sh, names[k], std::strlen(names[k]));
    sh[22] = static_cast<uint8_t>((lens[k] / 2) >> 8);
    sh[23] = static_cast<uint8_t>((lens[k] / 2) & 255);
    sh[25] = 64;
  }
  b[950] = 1;
  b[951] = 127;
  std::memcpy(b.data() + 1080, "M.K.", 4);
  std::vector<uint8_t> pat(64 * 4 * 4, 0);
  auto cell = [&](int row, int ch, int smp) {
    uint8_t* c = pat.data() + (row * 4 + ch) * 4;
    c[0] = static_cast<uint8_t>((smp & 0xF0) | (428 >> 8));
    c[1] = 428 & 255;
    c[2] = static_cast<uint8_t>((smp & 15) << 4);
  };
  cell(0, 0, 1);
  cell(8, 1, 2);
  b.insert(b.end(), pat.begin(), pat.end());
  for (int i = 0; i < lens[0]; ++i) b.push_back(static_cast<uint8_t>((i / 16) % 2 ? 100 : 156));
  for (int i = 0; i < lens[1]; ++i) b.push_back(static_cast<uint8_t>(static_cast<int8_t>(std::lround(100 * std::sin(2 * M_PI * i / 40.0)))));
  return b;
}

void writeWav(const fs::path& p, int frames, double freq, int rate = 22050) {
  std::ofstream o(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
  o.write("RIFF", 4);
  u32(36 + static_cast<uint32_t>(frames) * 2);
  o.write("WAVEfmt ", 8);
  u32(16), u16(1), u16(1), u32(static_cast<uint32_t>(rate)), u32(static_cast<uint32_t>(rate) * 2), u16(2), u16(16);
  o.write("data", 4);
  u32(static_cast<uint32_t>(frames) * 2);
  for (int i = 0; i < frames; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(9000 * std::sin(2 * M_PI * freq * i / rate))));
}

std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// How many of the instrument's samples have their audio.
int playable(const PluginState& st) {
  const auto inst = tp::instrumentFor(st);
  int n = 0;
  if (inst)
    for (const auto& f : inst->files) n += f && f->frames() > 0 ? 1 : 0;
  return n;
}

// The instrument's file names (its text's sample files).
std::vector<std::string> filesOf(const PluginState& st) { return tp::readEdit(st.data, 0).text.files; }
// The same file by name (the temporary folder's name may hold a doubled separator).
bool sameName(const std::string& a, const fs::path& b) { return fs::u8path(a).lexically_normal() == b.lexically_normal(); }

}  // namespace

int main() {
  const std::string home = gl::test::hermeticTestEnv("tracker_presets");
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", (home + "/cache").c_str());
  const fs::path config = fs::u8path(gl::platform::configRoot()) / "gridline";
  const fs::path cache = fs::u8path(archive::defaultCacheRoot());
  std::error_code ec;
  fs::create_directories(config, ec);
  expect(cache == fs::u8path(home) / "cache" / "unpacked", "the test's cache is its own");

  // ---- an imported module, its samples in the unpack cache (as the app opens one) ----
  trk::Module mod;
  std::string err;
  expect(trk::parseModule(makeMod(), mod, &err), "the test module parses");
  const fs::path copy = cache / "k435 test-0123abcd";
  std::vector<std::string> names;
  expect(trk::writeSamples(mod, copy.u8string(), names, &err) && names.size() == 2, "its samples are written into the cache");
  Song song;
  trk::ImportReport rep;
  trk::ImportOptions o;
  o.sampleFolder = copy.u8string();
  o.write = false;
  expect(trk::buildSong(mod, "k435 test", o, song, rep, &err) && song.channels.size() == 2, "the module becomes two instruments");
  const PluginState ins = song.channels[0].plugin;
  expect(ins.type == trk::kInstrumentType && playable(ins) == 1, "the first instrument plays its sample from the cache");
  const auto origInst = tp::instrumentFor(ins);
  const size_t origFrames = origInst && origInst->files[0] ? origInst->files[0]->frames() : 0;

  // ---- a library preset keeps the sound once the cache copy is gone ----
  const std::string root = presetRoot(config.u8string());
  expect(savePreset(root, ins, "Square lead"), "save the instrument as a library preset");
  const fs::path pj = fs::u8path(root) / trk::kInstrumentType / "Square lead.json";
  const fs::path carried = fs::u8path(root) / trk::kInstrumentType / "Square lead samples" / "instrument001-sample00.wav";
  expect(fs::is_regular_file(carried, ec), "the preset carries its sample beside it");
  {
    Json j;
    expect(jsonParse(readText(pj), j) && j.text("data").rfind("Square lead samples/instrument001-sample00.wav\n", 0) == 0,
           "the preset names its sample relative to itself");
  }
  // The project closes: its unpacked copy is removed (as sweepCache does for a local module).
  fs::remove_all(copy, ec);
  sampleCache().clear();
  expect(!fs::exists(copy / "instrument001-sample00.wav", ec), "the instrument's cached samples are gone");
  PluginState back;
  expect(loadPreset(pj.u8string(), trk::kInstrumentType, back), "the preset loads");
  const auto backInst = tp::instrumentFor(back);
  expect(backInst && backInst->files[0] && backInst->files[0]->frames() == origFrames && origFrames > 0,
         "the loaded preset plays the original sample (same length) after the cache is cleared");
  expect(!filesOf(back).empty() && fs::u8path(filesOf(back)[0]).is_absolute(), "a loaded preset names its sample by its full path");
  // Saved again under its own name: nothing is copied twice.
  expect(savePreset(root, back, "Square lead"), "save it again");
  size_t inFolder = 0;
  for (fs::directory_iterator it(carried.parent_path(), ec), end; !ec && it != end; it.increment(ec)) ++inFolder;
  expect(inFolder == 1, "saving the same preset again reuses its sample");
  {
    Json j;
    expect(jsonParse(readText(pj), j) && j.text("data").rfind("Square lead samples/", 0) == 0, "and still names it relative to itself");
  }

  // ---- Save preset to file: the same, beside the file chosen ----
  {
    expect(trk::writeSamples(mod, copy.u8string(), names, &err), "the module is opened again (its copy back in the cache)");
    const fs::path f = fs::u8path(home) / "elsewhere" / "My lead.json";
    int n = 0;
    expect(savePresetFile(f.u8string(), song.channels[1].plugin, "", true, &n) && n == 1, "a preset file carries its sample");
    fs::remove_all(copy, ec);
    sampleCache().clear();
    PluginState fb;
    expect(loadPreset(f.u8string(), trk::kInstrumentType, fb) && playable(fb) == 1 &&
               fs::is_regular_file(fs::u8path(home) / "elsewhere" / "My lead samples" / "instrument002-sample00.wav", ec),
           "the preset file plays from the folder beside it");
  }

  // ---- a sample from the person's own folder is only referred to ----
  {
    const fs::path lib = fs::u8path(home) / "my samples";
    fs::create_directories(lib, ec);
    writeWav(lib / "kick.wav", 4000, 60);
    tp::Edit e = tp::readEdit("", 0);
    expect(tp::addSampleFiles(e, {(lib / "kick.wav").u8string()}) == 0, "a file of the person's own is added");
    PluginState st = defaultPluginState(trk::kInstrumentType);
    st.data = tp::writeEdit(e);
    expect(savePreset(root, st, "Kick"), "save it");
    PluginState kb;
    expect(loadPreset((fs::u8path(root) / trk::kInstrumentType / "Kick.json").u8string(), trk::kInstrumentType, kb) && filesOf(kb).size() == 1 &&
               filesOf(kb)[0] == (lib / "kick.wav").u8string() && !fs::exists(fs::u8path(root) / trk::kInstrumentType / "Kick samples", ec),
           "the preset refers to it where it is (no copy)");
  }

  // ---- saved under an older place of Gridline's own folders ----
  {
    // A preset saved before the fix, on another HOME: its sample in the cache there, by full path.
    expect(trk::writeSamples(mod, copy.u8string(), names, &err), "the module's copy is in the cache");
    sampleCache().clear();
    PluginState old = ins;
    const std::string was = "/Users/someone else/.cache/gridline/unpacked/k435 test-0123abcd/instrument001-sample00.wav";
    old.data.replace(0, old.data.find('\n'), was);
    const fs::path oldp = fs::u8path(root) / trk::kInstrumentType / "Old.json";
    {
      Json j = Json::makeObject();
      j.object["format"] = Json::makeString("gridline-preset");
      j.object["type"] = Json::makeString(old.type);
      Json pa = Json::makeArray();
      for (float v : old.params) pa.array.push_back(Json::makeNumber(v));
      j.object["params"] = pa;
      j.object["data"] = Json::makeString(old.data);
      std::ofstream(oldp) << jsonStringify(j);
    }
    PluginState ob;
    expect(loadPreset(oldp.u8string(), trk::kInstrumentType, ob) && playable(ob) == 1 && sameName(filesOf(ob)[0], copy / "instrument001-sample00.wav"),
           (std::string("an older preset finds its unpacked sample under this computer's cache (") + (filesOf(ob).empty() ? std::string() : filesOf(ob)[0]) + ")").c_str());
    // The module changed since: unpacked again under another version's name.
    const fs::path other = cache / "k435 test-feedbeef";
    fs::rename(copy, other, ec);
    sampleCache().clear();
    PluginState ob2;
    expect(loadPreset(oldp.u8string(), trk::kInstrumentType, ob2) && playable(ob2) == 1 && sameName(filesOf(ob2)[0], other / "instrument001-sample00.wav"),
           (std::string("it finds the copy of the module made again under another version's name (") + (filesOf(ob2).empty() ? std::string() : filesOf(ob2)[0]) + ")").c_str());
    fs::rename(other, copy, ec);
    // Edited audio of a project not saved yet, in the settings folder of another HOME.
    fs::create_directories(config / "Edited audio", ec);
    writeWav(config / "Edited audio" / "drawn-1a2b.wav", 3000, 220);
    const std::string drawn = "/home/me/.config/gridline/Edited audio/drawn-1a2b.wav";
    expect(sameName(relinkGridlineFile(drawn, gridlineFileRoots()), config / "Edited audio" / "drawn-1a2b.wav"),
           "edited audio saved under another settings folder is found in this one");
    expect(relinkGridlineFile("/home/me/Music/other.wav", gridlineFileRoots()).empty(), "a file outside Gridline's folders is never guessed");
    // A project saved that way opens with the sample found.
    Song ps;
    ps.channels.push_back(song.channels[0]);
    ps.channels[0].plugin.data = old.data;
    const fs::path proj = fs::u8path(home) / "proj" / "Song.gridline.json";
    fs::create_directories(proj.parent_path(), ec);
    expect(saveProject(proj.u8string(), ps, &err), "a project naming the older place is saved");
    Song pl;
    expect(loadProject(proj.u8string(), pl, &err) && pl.channels.size() == 1 && sameName(filesOf(pl.channels[0].plugin)[0], copy / "instrument001-sample00.wav"),
           "the project opens with the sample found where Gridline's cache is now");
  }

  // ---- a missing file put back plays (the sample cache does not keep it missing) ----
  {
    const fs::path late = fs::u8path(home) / "late" / "arrives.wav";
    tp::Edit e = tp::readEdit("", 0);
    e.text.files = {late.u8string()};
    e.text.fileOf[0] = 0;
    e.text.def.samples[0].length = 2000;
    e.text.def.keymap.fill(0);
    PluginState st = defaultPluginState(trk::kInstrumentType);
    st.data = tp::writeEdit(e);
    expect(playable(st) == 0, "a sample whose file is not there yet is missing");
    fs::create_directories(late.parent_path(), ec);
    writeWav(late, 2000, 330);
    // (the app asks the sample cache to look at its failed files each frame, at most twice a second)
    expect(sampleCache().recheckFailed(0) == 1, "the sample cache notices the file is there now");
    expect(playable(st) == 1, "once the file is there, the instrument plays it (no reopening)");
  }

  // ---- the samples as WAV files ----
  {
    expect(trk::writeSamples(mod, copy.u8string(), names, &err), "the module's copy is in the cache");
    PluginState two = ins;
    tp::Edit e = tp::readEdit(two.data, 0);
    expect(tp::copySampleInto(e, 1, tp::readEdit(song.channels[1].plugin.data, 0), 0) == 1, "a second sample in the instrument");
    two.data = tp::writeEdit(e);
    std::vector<std::string> out;
    const fs::path base = fs::u8path(home) / "export" / "Lead.wav";
    expect(tp::exportSampleFiles(two, base.u8string(), &out) == 2 && out.size() == 2, "two samples, two files");
    expect(out.size() == 2 && fs::u8path(out[0]).filename().u8string().rfind("Lead 01", 0) == 0 && fs::u8path(out[1]).filename().u8string().rfind("Lead 02", 0) == 0,
           "named after the file chosen and their entries");
    const SamplePtr s0 = out.empty() ? nullptr : loadSample(out[0]);
    expect(s0 && s0->frames() == origFrames, "a written sample holds the whole played part");
    std::vector<std::string> one;
    expect(tp::exportSampleFiles(ins, (fs::u8path(home) / "export" / "Only.wav").u8string(), &one) == 1 && one.size() == 1 &&
               fs::u8path(one[0]).filename() == "Only.wav",
           "one sample: the file chosen");
  }

  // ---- effects over a sample ----
  {
    const auto list = tp::sampleEffects();
    expect(list.size() >= 10, "Gridline's own effects are offered for samples");
    bool delay = false;
    for (const PluginInfo* p : list) delay = delay || p->type == "gl.delay";
    expect(delay, "the delay is among them");
    tp::Wave base;
    base.rate = 22050;
    for (int i = 0; i < 4000; ++i) base.L.push_back(static_cast<float>(0.5 * std::sin(2 * M_PI * 220 * i / 22050.0)));
    int finite = 0;
    for (const PluginInfo* p : list) {
      tp::Wave w = base;
      if (!tp::waveEffects(w, tp::Span{}, {tp::FxStep{defaultPluginState(p->type), 1.0}}, 0.0)) continue;
      bool ok = w.frames() == base.frames();
      for (float v : w.L) ok = ok && std::isfinite(v);
      for (float v : w.R) ok = ok && std::isfinite(v);
      if (ok) ++finite;
      else std::printf("  not finite or resized: %s\n", p->type.c_str());
    }
    expect(finite == static_cast<int>(list.size()), "every offered effect gives finite sound of the same length without a tail");
    // A delay's echoes ring out after the sample's end: it grows; dry (mix 0) leaves it as it was.
    tp::Wave w = base;
    PluginState dl = defaultPluginState("gl.delay");
    expect(tp::waveEffects(w, tp::Span{}, {tp::FxStep{dl, 1.0}}, 2.0) && w.frames() > base.frames(), "the delay's tail grows the sample");
    tp::Wave dry = base;
    expect(tp::waveEffects(dry, tp::Span{}, {tp::FxStep{dl, 0.0}}, 0.0) && dry.L == base.L, "mix 0: the sample stays as it was");
    // Only a range: what precedes it is untouched.
    tp::Wave part = base;
    expect(tp::waveEffects(part, tp::Span{2000, 3000}, {tp::FxStep{defaultPluginState("gl.filter"), 1.0}}, 0.0), "an effect over a range");
    bool before = true, changed = false;
    for (int i = 0; i < 2000; ++i) before = before && part.L[static_cast<size_t>(i)] == base.L[static_cast<size_t>(i)];
    for (int i = 2000; i < 3000; ++i) changed = changed || std::fabs(part.L[static_cast<size_t>(i)] - base.L[static_cast<size_t>(i)]) > 1e-4f;
    expect(before && part.frames() == base.frames(), "frames before the range are untouched");
    expect(changed, "the range itself went through the filter");
    // Silence in: silence out from a reverb with no tail asked for.
    tp::Wave quiet;
    quiet.rate = 22050;
    quiet.L.assign(3000, 0.0f);
    expect(tp::waveEffects(quiet, tp::Span{}, {tp::FxStep{defaultPluginState("gl.reverb"), 1.0}}, 1.0), "a reverb over silence");
    float peak = 0;
    for (float v : quiet.L) peak = std::max(peak, std::fabs(v));
    expect(peak < 1e-4f && quiet.frames() == 3000, "silence stays silence (no tail added)");
    // A stereo effect makes a mono sample stereo.
    tp::Wave mono = base;
    expect(tp::waveEffects(mono, tp::Span{}, {tp::FxStep{defaultPluginState("gl.chorus"), 1.0}}, 0.0) && mono.stereo && mono.R.size() == mono.L.size(),
           "a chorus turns a mono sample stereo");
    expect(!tp::waveEffects(w, tp::Span{}, {}, 0.0), "no effect: nothing changes");
  }
  return finish();
}
