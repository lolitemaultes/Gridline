// Gridline tests: K-44, a zipped project holds every file the project uses. A project using every
// plugin type that keeps files is packed, its files are taken away, the package is unpacked somewhere
// else, and every reference must resolve inside the unpacked folder with the same bytes, the samples
// must load and the audio must match. Also: export project data files, bones, the FL package, the
// report of what could not be packed, and a failed write leaving the target alone.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "../src/core/Json.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/SfzText.h"
#include "../src/core/TrackReceipt.h"
#include "../src/core/ProjectArchive.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/Zip.h"
#include "../src/core/flp/FlpPackage.h"
#include "../src/core/AudioBuffer.h"
#include "../src/engine/Export.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/KerfMarks.h"
#include "../src/engine/Sample.h"
#include "../src/engine/Slicing.h"
#include "../src/engine/SlicexEngine.h"
#include "../src/packs/plasma/plasma.h"
#include "../src/packs/plasma/plasma_dsp.h"
#include "../src/packs/samplers/dwave.h"
#include "../src/packs/spacefx/spacefx.h"
#include "../src/packs/synths/gms.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string root;  // this run's scratch

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// A distinct tone per file, so swapping two files changes the sound and the bytes.
std::string tone(const fs::path& p, double hz, int frames = 22050) {
  fs::create_directories(p.parent_path());
  AudioBuffer b(2, static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const float v = 0.5f * std::sin(6.283185307 * hz * i / 44100.0) * (1.0f - static_cast<float>(i) / static_cast<float>(frames));
    b.at(0, static_cast<size_t>(i)) = b.at(1, static_cast<size_t>(i)) = v;
  }
  writeWavFile(p.u8string(), b, 44100, 16);
  return p.u8string();
}

std::vector<char> bytesOf(const std::string& p) {
  std::ifstream f(fs::u8path(p), std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& t) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << t;
}

// One RIFF chunk header + text: a soundfont only has to be a file here (its reading has its own tests).
std::string fakeBank(const fs::path& p) {
  writeText(p, "RIFF....sfbk bank bytes for the packing test");
  return p.u8string();
}

struct Project {
  Song song;
  std::map<std::string, std::vector<char>> bytes;  // original path -> content
  std::vector<std::string> gone;                   // references to files that do not exist
  std::vector<ObjectId> rendered;                  // channels that take part in the render
};

Song emptySong() {
  Song s = makeEmptySong();
  s.channels.clear();
  s.info.title = "K44";
  Pattern p;
  p.id = s.allocateId();
  s.patterns.clear();
  s.patterns.push_back(p);
  return s;
}

ObjectId addChannel(Song& s, const std::string& type, const std::string& data, bool playNotes) {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  if (playNotes) s.patterns.front().notes[c.id].push_back(Note(0, 96, 60, 100));
  return c.id;
}

void addFx(Song& s, const std::string& type, const std::string& data) {
  PluginState st = defaultPluginState(type);
  st.data = data;
  for (auto& slot : s.mixer.front().slots) {
    if (slot.plugin.empty()) {
      slot.plugin = st;
      return;
    }
  }
}

// Every plugin type that keeps files, all from one source folder.
Project buildProject(const fs::path& src) {
  Project pr;
  Song& s = pr.song;
  s = emptySong();
  int hz = 110;
  auto T = [&](const std::string& rel) {
    hz += 37;
    const std::string p = tone(src / fs::u8path(rel), hz);
    pr.bytes[p] = bytesOf(p);
    return p;
  };
  auto keep = [&](const std::string& p) {
    pr.bytes[p] = bytesOf(p);
    return p;
  };

  // Plain sample players: one path, or a first line with a model below it.
  addChannel(s, "gl.sampler", T("plain.wav"), true);
  addChannel(s, "gl.synth", T("cycle.wav"), true);
  addChannel(s, "gl.granular", T("gran.wav"), true);
  addChannel(s, "gl.scratch", T("scratch.wav"), true);
  addChannel(s, "gl.grainscan", T("grainscan.wav"), true);
  addChannel(s, "gl.splinescratch", T("spline.wav"), true);
  addChannel(s, "gl.seltmann", T("seltmann.wav"), true);
  addChannel(s, "gl.turntable", T("turntable.wav"), false);
  addChannel(s, "gl.imagesynth", keep(fakeBank(src / "picture.png")) + "\ncanvas line", false);

  // Kerf with names below the path, and a region model naming a file for each deck.
  addChannel(s, "gl.slicer", kerfmarks::withNames(T("kerf.wav"), {{3, "hit"}}), true);
  {
    slicex::Model m = slicex::defaultModel();
    m.decks[0].file = T("deckA.wav");
    m.decks[1].file = T("deckB.wav");
    addChannel(s, "gl.slicer", slicex::makeData(m), false);
  }
  // A DJ setup: the first line and "file " lines.
  addChannel(s, "gl.platter", T("platA.wav") + "\nfile " + T("platB.wav"), false);

  // Abacus: bank A pads, a layer, a bank B pad, the built-in kit and an empty pad, names and envelopes.
  {
    std::string d = T("pad01.wav") + "\n" + T("pad02.wav") + "\n\nnone\nbuiltin:kick\n" + T("pad06.wav");
    for (int i = 6; i < 16; ++i) d += "\n";
    d += "\nN1 Kick pad.wav\nL1.2 " + T("padlayer.wav") + "\nL1.7 " + T("padlayer7.wav") + "\nL17.1 " + T("padB.wav") + "\nL2.3 none";
    addChannel(s, "gl.pads", d, true);
  }
  // Multisampler: zones with sample, soundfont and generated sources, and a whole bank.
  {
    const std::string bank = keep(fakeBank(src / "bank.sf2")), zoneBank = keep(fakeBank(src / "zones.SF2"));
    std::string d = "#gl-zones 1\nsf2 0 3 Grand%20Piano | " + bank + "\n";
    d += "z lo=0 hi=59 vlo=0 vhi=127 root=48 name=Low.wav | " + T("zone1.wav") + "\n";
    d += "z lo=60 hi=119 vlo=0 vhi=127 root=72 | " + T("zone2.wav") + "\n";
    d += "z lo=120 hi=127 vlo=0 vhi=127 root=120 | sf2:" + zoneBank + "#12\n";
    d += "z lo=120 hi=127 vlo=0 vhi=127 root=120 | tone:piano:60:0\n";
    d += "z lo=120 hi=127 vlo=0 vhi=127 root=120 | builtin:hat\n";
    addChannel(s, "gl.multisampler", d, true);
    addChannel(s, "gl.multisampler", T("dropped.wav"), false);  // a bare path
  }
  // Wave program.
  {
    packs::samplers::dw::DwProgram p;
    packs::samplers::dw::DwZone a, b;
    a.name = "Named.wav";
    a.path = T("dwA.wav");
    b.path = "builtin:tomlo";
    p.zones = {a, b};
    addChannel(s, "gl.waveprogram", packs::samplers::dw::formatProgram(p), true);
  }
  // Morphing oscillators.
  {
    packs::synths::gms::WaveFiles w;
    w.path[0] = T("gmsA.wav");
    w.path[1] = T("gmsB.wav");
    addChannel(s, "gl.gms", packs::synths::gms::serializeWaveFiles(w), false);
  }
  // The additive model's analysis source (the first part), an unused second part, a picture.
  addChannel(s, "gl.harmor", "hmr 1\nx 0 " + T("harmor.wav") + "\nx 1 " + keep(fakeBank(src / "harmor picture.png")) + "\n", false);
  // Plasma: a table file, a sample, a multisample map, the noise file, a convolver slot, embedded and factory references.
  {
    namespace pl = packs::plasma;
    pl::Model m = pl::defaultModel();
    m.osc[0].table = "file:" + T("table.wav");
    m.osc[0].tableName = "Shown name.wav";
    m.osc[1].sample = T("plasmaSample.wav");
    // A map: relative through a folder, through default_path, an absolute one, a name with spaces.
    const std::string m1 = T("kit/samples/s1.wav"), m2 = T("kit/other/deep/s2.wav"), m3 = T("elsewhere/abs.wav"), m4 = T("kit/samples/s 4.wav");
    (void)m2;
    writeText(src / "kit" / "map" / "kit.sfz",
              "// a comment naming sample=not/this.wav\n<control>\ndefault_path=../samples\n<region> sample=s1.wav key=60\n<region> sample=s 4.wav key=61\n"
              "<control>\ndefault_path=..\\other\n<region> key=62 sample=deep\\s2.wav\n<control>\ndefault_path=\n<region> sample=" +
                  m3 + " key=63\n<region> sample=*sine key=64\n<region> sample=gone.wav key=65\n");
    m.osc[2].sfz = keep((src / "kit" / "map" / "kit.sfz").u8string());
    pr.gone.push_back((src / "kit" / "map" / "gone.wav").lexically_normal().u8string());
    (void)m1, (void)m4;
    m.noiseFile = T("noise.wav");
    m.fxExtra[0].ir = T("plasmaIr.wav");
    m.fxExtra[1].ir = "factory:Hall.wav";
    addChannel(s, "gl.plasma", pl::modelToData(m), true);
  }
  // Effects, in a mixer slot.
  addFx(s, "gl.convolver", T("ir.wav") + "\n#v 1\n#mode eq=smooth");
  for (auto& slot : s.mixer.front().slots)
    if (slot.plugin.type == "gl.convolver") slot.plugin.params[static_cast<size_t>(packs::spacefx::CvSource)] = static_cast<float>(packs::spacefx::SrcFile);
  addFx(s, "gl.turntablefx", T("turntablefx.wav"));
  // A patch: an effect inside it keeps its own format, and its labels are not files.
  {
    Json node = Json::makeObject();
    node.object["type"] = Json::makeString("gl.pads");
    node.object["data"] = Json::makeString(T("patchPad.wav") + "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\nL1.2 " + T("patchLayer.wav"));
    node.object["disp"] = Json::makeString("Label.wav");
    Json nodes = Json::makeArray();
    nodes.array.push_back(node);
    Json j = Json::makeObject();
    j.object["nodes"] = nodes;
    addChannel(s, "gl.patcher", jsonStringify(j, 0), false);
    // The newer patch format is recognised by its first bytes: they must survive.
    const std::string inner = jsonStringify(Json::makeString(T("patchV2.wav")), 0);
    addChannel(s, "gl.patcher", "{\"v\":2,\"ver\":4,\"items\":[],\"nodes\":[{\"type\":\"gl.sampler\",\"params\":[],\"data\":" + inner + ",\"disp\":\"Label2.wav\"}],\"slots\":[]}", false);
  }
  // A cabinet folder (found by name inside it).
  {
    const std::string a = tone(src / "cabinets" / "IMP Cabinet One.wv", 300), b = tone(src / "cabinets" / "board.wav", 320);
    pr.bytes[a] = bytesOf(a);
    pr.bytes[b] = bytesOf(b);
    addFx(s, "gl.amprack", "cabdir=" + (src / "cabinets").u8string() + "\n");
  }
  // The same file name from two folders, with different content, and a name outside ASCII.
  addChannel(s, "gl.sampler", T("one/same name.wav"), true);
  addChannel(s, "gl.sampler", T("two/same name.wav"), true);
  addChannel(s, "gl.sampler", T("two/SAME NAME.wav.bak.wav"), false);
  addChannel(s, "gl.sampler", T(u8"Jero\u0301nimo s\u00f8ren \u65e5\u672c.wav"), true);
  // Files that are gone: a plain one and a Windows path in a JSON state.
  {
    const std::string lost = (src / "lost" / "lost.wav").u8string();
    addChannel(s, "gl.sampler", lost, false);
    pr.gone.push_back(lost);
    namespace pl = packs::plasma;
    pl::Model m = pl::defaultModel();
    m.osc[0].sample = "C:\\Samples\\Windows Kick.wav";
    addChannel(s, "gl.plasma", pl::modelToData(m), false);
    pr.gone.push_back("C:\\Samples\\Windows Kick.wav");
  }
  // Values that name no file.
  addChannel(s, "gl.sampler", "builtin:kick", false);
  addChannel(s, "gl.pads", "none\nbuiltin:snare", false);
  addChannel(s, "gl.keyctl", "1 2 3", false);

  s.playlist.clear();
  return pr;
}

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.tail = 96;
  o.patternMode = true;
  o.pattern = s.patterns.front().id;
  return renderSong(s, o);
}

float peakDiff(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.frames != b.frames || a.channels != b.channels) return 1e9f;
  float d = 0;
  for (size_t i = 0; i < a.data.size(); ++i) d = std::max(d, std::fabs(a.data[i] - b.data[i]));
  return d;
}

float peak(const AudioBuffer& a) {
  float d = 0;
  for (float v : a.data) d = std::max(d, std::fabs(v));
  return d;
}

bool isInside(const std::string& p, const fs::path& dir) {
  const std::string d = dir.lexically_normal().generic_u8string() + "/", g = fs::u8path(p).lexically_normal().generic_u8string();
  return g.rfind(d, 0) == 0;
}

bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Every original file is present under `dir` with its own bytes, through the state that named it.
void checkResolved(const Song& original, const Song& opened, const Project& pr, const fs::path& dir, const std::string& what) {
  // Same walk order on both: reference by reference, old against new.
  std::vector<std::string> before = referencedFiles(original), after = referencedFiles(opened);
  expect(before.size() == after.size(), what + ": as many references as before (" + std::to_string(before.size()) + " / " + std::to_string(after.size()) + ")");
  size_t wrong = 0, outside = 0;
  for (size_t i = 0; i < std::min(before.size(), after.size()); ++i) {
    auto it = pr.bytes.find(before[i]);
    if (it == pr.bytes.end()) continue;  // a file that is gone
    if (!isInside(after[i], dir)) {
      ++outside;
      std::printf("  outside: %s -> %s\n", before[i].c_str(), after[i].c_str());
    }
    if (fs::u8path(before[i]).extension() != ".sfz" && bytesOf(after[i]) != it->second) {
      ++wrong;
      std::printf("  other bytes: %s -> %s\n", before[i].c_str(), after[i].c_str());
    }
  }
  expect(outside == 0, what + ": every reference resolves inside the unpacked folder");
  expect(wrong == 0, what + ": every reference has the original file's bytes");
}

void checkSfz(const fs::path& mapFile, const std::string& what) {
  std::vector<packs::plasma::Zone> zones;
  expect(packs::plasma::parseSfz(mapFile.u8string(), zones, nullptr), what + ": the map still parses");
  expect(zones.size() == 4, what + ": the map's four present samples load (" + std::to_string(zones.size()) + ")");
}

void testRoundTrip(bool withRender) {
  const fs::path base = fs::u8path(root) / "round";
  const fs::path src = base / "Original folder";
  Project pr = buildProject(src);
  const Song original = pr.song;
  const std::vector<std::string> refs = referencedFiles(original);

  // Nothing that names no file is listed, and everything that does is.
  for (const char* bad : {"builtin:kick", "builtin:snare", "builtin:hat", "tone:piano:60:0", "none", "Shown name.wav", "Label.wav", "factory:Hall.wav", "Hall.wav", "Named.wav", "-"})
    expect(!contains(refs, bad), std::string("not a file: ") + bad);
  for (const auto& [p, b] : pr.bytes) {
    (void)b;
    if (fs::u8path(p).parent_path() == src / "cabinets" || fs::u8path(p).parent_path() == src / "kit" / "samples" || fs::u8path(p).parent_path() == src / "kit" / "other" / "deep" ||
        fs::u8path(p).parent_path() == src / "elsewhere")
      continue;  // reached through their folder or map
    expect(contains(refs, p), "listed: " + fs::u8path(p).filename().u8string());
  }
  // A walk that changes nothing leaves every state byte for byte.
  {
    Song same = original;
    expect(forEachFileRef(same, [](std::string&) {}) == 0 && same == original, "a walk that changes nothing changes no state");
  }

  AudioBuffer before;
  if (withRender) before = render(original);
  if (withRender) expect(peak(before) > 0.01f, "the original project sounds");

  const std::string zip = (base / "Out" / "Song.gridline.zip").u8string();
  fs::create_directories(base / "Out");
  CollectReport rep = exportZippedProject(original, zip, "Song");
  expect(rep.error.empty(), "zip written (" + rep.error + ")");
  expect(rep.unpacked.empty(), "nothing left behind (" + std::to_string(rep.unpacked.size()) + ")");
  for (const auto& p : rep.unpacked) std::printf("  left behind: %s\n", p.c_str());
  for (const auto& g : pr.gone) expect(contains(rep.missing, g), "listed as missing: " + g);
  expect(rep.missing.size() == pr.gone.size(), "only the files that are gone are missing (" + std::to_string(rep.missing.size()) + ")");

  // The project text inside the archive holds no path to a real file.
  std::vector<ZipEntry> entries;
  expect(readZipDirectory(zip, entries), "the archive reads");
  std::string projectText;
  std::set<std::string> names;
  for (const auto& e : entries) {
    names.insert(e.name);
    if (e.name == "Song.gridline.json") {
      std::vector<uint8_t> d;
      expect(readZipEntry(zip, e, d), "the project reads");
      projectText.assign(d.begin(), d.end());
    }
  }
  expect(!projectText.empty() && absolutePathsInProject(projectText).empty(), "no absolute path to a real file in the project");
  for (const auto& [p, b] : pr.bytes) {
    (void)b;
    expect(projectText.find(p) == std::string::npos, "not named in the project: " + fs::u8path(p).filename().u8string());
  }
  expect(names.count("Samples/kit/kit.sfz") && names.count("Samples/kit/s1.wav") && names.count("Samples/kit/s2.wav") && names.count("Samples/kit/abs.wav") && names.count("Samples/kit/s 4.wav"),
         "a map travels with its samples");
  expect(names.count("Samples/cabinets/IMP Cabinet One.wv") && names.count("Samples/cabinets/board.wav"), "a cabinet folder travels whole");
  expect(names.count("Samples/same name.wav") && names.count("Samples/same name (2).wav"), "same name from two folders: both kept");

  // The originals go away; only the archive is left.
  const fs::path moved = base / "Taken away";
  fs::rename(src, moved);
  sampleCache().clear();

  const fs::path there = base / "Somewhere else" / u8"f\u00f8lder";
  std::string file, err;
  expect(unpackZippedProject(zip, there.u8string(), file, &err), "unpacks (" + err + ")");
  Song opened;
  expect(loadProject(file, opened, &err), "the project loads (" + err + ")");
  const fs::path real = fs::weakly_canonical(there);
  fs::rename(moved, src);  // (for the bytes of the original below only: nothing reads it through the project)
  checkResolved(original, opened, pr, real, "zip");
  fs::rename(src, moved);
  // What is gone is still named, as it was.
  const auto miss = missingFiles(opened);
  expect(miss.size() == 2 && contains(miss, pr.gone[1]) && contains(miss, pr.gone[2]), "after unpacking only the plainly missing files are missing (" + std::to_string(miss.size()) + ")");
  checkSfz(real / "Samples" / "kit" / "kit.sfz", "zip");

  // Formats kept: the newer patch is still that patch; a pad layer is a pad layer.
  for (const auto& c : opened.channels) {
    if (c.plugin.type == "gl.patcher" && c.plugin.data.find("Label2.wav") != std::string::npos)
      expect(c.plugin.data.rfind("{\"v\":2,", 0) == 0, "the newer patch format survives the rewrite");
    if (c.plugin.type == "gl.pads" && c.plugin.data.find("L1.2") != std::string::npos) {
      expect(fs::exists(fs::u8path(padSamplePath(c.plugin.data, 0, 1))) && fs::exists(fs::u8path(padSamplePath(c.plugin.data, 16, 0))) && fs::exists(fs::u8path(padSamplePath(c.plugin.data, 1, 0))),
             "pads: pad 2, a layer and bank B resolve");
      expect(padDataGet(c.plugin.data, "N1") == "Kick pad.wav", "pads: a pad's name is left alone");
      expect(padSamplePath(c.plugin.data, 3, 0) == "none" && padSamplePath(c.plugin.data, 4, 0) == "builtin:kick", "pads: an empty pad and the built-in kit are left alone");
      ChannelAux aux;
      prepareChannelAux(c.plugin, aux);
      int loaded = 0;
      for (const auto& smp : aux.samples) loaded += smp && smp->frames() > 0 ? 1 : 0;
      expect(loaded >= 7, "pads: the samples load after unpacking (" + std::to_string(loaded) + ")");
    }
    if (c.plugin.type == "gl.multisampler" && c.plugin.data.find("tone:piano") != std::string::npos) {
      expect(c.plugin.data.find("| Samples/") == std::string::npos, "zones: absolute after loading");
      expect(c.plugin.data.find("#12\n") != std::string::npos && c.plugin.data.find("| sf2:" + (real / "Samples" / "zones.SF2").u8string() + "#12\n") != std::string::npos, "zones: the sound font keeps its sample number");
      expect(c.plugin.data.find("Grand%20Piano | " + (real / "Samples" / "bank.sf2").u8string()) != std::string::npos, "zones: the whole bank line");
      expect(c.plugin.data.find("| tone:piano:60:0\n") != std::string::npos && c.plugin.data.find("| builtin:hat\n") != std::string::npos, "zones: generated sources are left alone");
    }
    if (c.plugin.type == "gl.plasma" && c.plugin.data.find("kit.sfz") != std::string::npos) {
      packs::plasma::Model m;
      expect(packs::plasma::modelFromData(c.plugin.data, m), "Plasma: the model still parses");
      expect(m.osc[0].table == "file:" + (real / "Samples" / "table.wav").u8string() && m.osc[0].tableName == "Shown name.wav", "Plasma: the table file, its label left alone");
      expect(fs::exists(fs::u8path(m.osc[1].sample)) && fs::exists(fs::u8path(m.noiseFile)) && fs::exists(fs::u8path(m.fxExtra[0].ir)), "Plasma: sample, noise and impulse resolve");
      expect(m.fxExtra[1].ir == "factory:Hall.wav", "Plasma: a factory impulse is left alone");
    }
  }
  for (const auto& slot : opened.mixer.front().slots)
    if (slot.plugin.type == "gl.amprack") expect(slot.plugin.data == "cabdir=" + (real / "Samples" / "cabinets").u8string() + "\n", "cabinet folder resolves after unpacking");

  if (withRender) {
    const float d = peakDiff(before, render(opened));
    expect(d < 1e-5f, "the unpacked project sounds the same (peak difference " + std::to_string(d) + ")");
  }

  // Saving again over the same archive: nothing is lost and the archive stays a package.
  {
    CollectReport again = exportZippedProject(opened, zip, "Song");
    expect(again.error.empty() && again.unpacked.empty(), "save over the archive it was opened from");
    expect(again.missing.size() == 3, "save again: what is gone is still reported (" + std::to_string(again.missing.size()) + ")");
    fs::rename(there.parent_path(), base / "Second move");
    const fs::path third = base / "Third";
    expect(unpackZippedProject(zip, third.u8string(), file, &err), "the saved archive unpacks");
    Song back;
    expect(loadProject(file, back, &err), "the saved archive loads");
    fs::rename(moved, src);
    checkResolved(original, back, pr, fs::weakly_canonical(third), "saved again");
    fs::rename(src, moved);
    checkSfz(fs::weakly_canonical(third) / "Samples" / "kit" / "kit.sfz", "saved again");
    if (withRender) {
      const float d = peakDiff(before, render(back));
      expect(d < 1e-5f, "saved again: the same sound (peak difference " + std::to_string(d) + ")");
    }
  }
  fs::rename(moved, src);
}

// A project opened out of the cache (K-42: a read-only folder here) packs from the cached files.
void testFromCache() {
  const fs::path base = fs::u8path(root) / "cache";
  const fs::path src = base / "src";
  Project pr = buildProject(src);
  const fs::path ro = base / "read only";
  fs::create_directories(ro);
  const std::string zip = (ro / "Song.gridline.zip").u8string();
  expect(exportZippedProject(pr.song, zip, "Song").error.empty(), "cache: the archive is written");
  fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);
  archive::UnpackRequest rq;
  rq.zipPath = zip;
  rq.cacheRoot = (base / "cacheRoot").u8string();
  archive::UnpackResult ur;
  std::string err;
  const bool unpacked = archive::unpackForOpening(rq, ur, &err);
  fs::permissions(ro, fs::perms::owner_all);
  expect(unpacked, "cache: the archive unpacks (" + err + ")");
#ifndef _WIN32  // (Windows has no read-only folders in this sense: it unpacks beside the archive)
  expect(ur.inCache, "cache: a read-only folder unpacks into the cache");
#endif
  Song opened;
  expect(loadProject(ur.projectFile, opened, &err), "cache: it loads");
  const std::string zip2 = (base / "Saved elsewhere.gridline.zip").u8string();
  CollectReport rep = exportZippedProject(opened, zip2, "Saved");
  expect(rep.error.empty() && rep.unpacked.empty() && rep.missing.size() == 3, "cache: Save as packs everything");
  std::vector<ZipEntry> a, b;
  readZipDirectory(zip, a);
  readZipDirectory(zip2, b);
  expect(a.size() == b.size(), "cache: as many entries as the first archive (" + std::to_string(a.size()) + " / " + std::to_string(b.size()) + ")");
}

void testDataFilesAndBones() {
  const fs::path base = fs::u8path(root) / "data";
  const fs::path src = base / "src";
  Project pr = buildProject(src);
  const Song original = pr.song;
  const fs::path out = base / "Export";
  CollectReport rep = exportProjectDataFiles(original, out.u8string(), "Song");
  expect(rep.error.empty() && rep.unpacked.empty() && rep.missing.size() == pr.gone.size(), "data files: everything copied, gone files listed");
  Song back;
  std::string err;
  expect(loadProject((out / "Song.gridline.json").u8string(), back, &err), "data files: the project loads");
  checkResolved(original, back, pr, out, "data files");
  checkSfz(out / "Samples" / "kit" / "kit.sfz", "data files");
  // Again into the same folder: reused, no second copies.
  const int first = rep.copied;
  CollectReport twice = exportProjectDataFiles(original, out.u8string(), "Song");
  expect(twice.error.empty() && twice.copied == 0 && twice.reused == first, "data files: a second export reuses the files (" + std::to_string(twice.copied) + " copied)");
  expect(!fs::exists(out / "Samples" / "kit (2)"), "data files: a map is not copied twice");

  const fs::path bones = base / "Bones";
  BonesReport br = exportProjectBones(original, bones.u8string(), "Song");
  expect(br.files.error.empty() && br.files.unpacked.empty(), "bones: everything copied");
  size_t outsideRefs = 0;
  for (auto it = fs::recursive_directory_iterator(bones / "Presets"); it != fs::recursive_directory_iterator(); ++it)
    if (it->is_regular_file() && it->path().extension() != ".wav") {
      std::ifstream f(it->path(), std::ios::binary);
      const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      outsideRefs += t.find(src.u8string()) != std::string::npos && t.find("lost") == std::string::npos ? 1 : 0;
    }
  expect(outsideRefs == 0, "bones: presets name the collected files (" + std::to_string(outsideRefs) + ")");
}

void testFlPackage() {
  const fs::path base = fs::u8path(root) / "fl";
  const fs::path src = base / "src";
  Song s = emptySong();
  const std::string a = tone(src / "one" / "same.wav", 200), b = tone(src / "two" / "same.wav", 300), l = tone(src / "layer.wav", 400), z = tone(src / "zone.wav", 500);
  addChannel(s, "gl.pads", a + "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\nL1.2 " + l, true);
  addChannel(s, "gl.multisampler", "#gl-zones 1\nz lo=0 hi=127 vlo=0 vhi=127 root=60 | " + b + "\nz lo=0 hi=127 vlo=0 vhi=127 root=60 | " + z + "\n", true);
  s.playlist.clear();
  flp::PackageOptions opt;
  opt.flp.factoryData.clear();
  opt.flp.renderInstruments = false;
  flp::PackageReport rep;
  std::string err;
  const std::string zip = (base / "pack.zip").u8string();
  fs::create_directories(base);
  expect(flp::exportFlpZip(s, zip, "pack", opt, rep, &err), "FL package written (" + err + ")");
  expect(rep.files.size() == 4 && rep.missing.empty(), "FL package: pad layers and zones travel (" + std::to_string(rep.files.size()) + ")");
  std::set<std::string> names;
  for (const auto& [from, entry] : rep.files) names.insert(entry);
  expect(names.count("same.wav") && names.count("same (2).wav") && names.count("layer.wav") && names.count("zone.wav"), "FL package: the entries beside the project");
}

// Whatever could not be packed is named; a failed write leaves the target as it was.
void testReport() {
  const fs::path base = fs::u8path(root) / "report";
  const fs::path src = base / "src";
  const std::string real = tone(src / "real.wav", 500), hidden = tone(src / "hidden.flac.wav", 600);
  Song s = emptySong();
  addChannel(s, "gl.sampler", real, true);
  // A state whose format the walk does not know, naming a real file on one of its lines.
  addChannel(s, "gl.keyctl", "some model\nsource=" + hidden + "\n", false);
  const std::string zip = (base / "x.gridline.zip").u8string();
  fs::create_directories(base);
  CollectReport rep = exportZippedProject(s, zip, "X");
  expect(rep.error.empty(), "report: written");
  expect(contains(rep.unpacked, hidden), "report: a path the walk does not know is named, not lost");
  expect(!contains(rep.unpacked, real), "report: a packed file is not named");
  std::string realInJson;  // (a Windows path's backslashes are escapes in a project's text)
  for (const char c : real) {
    if (c == '\\') realInJson += '\\';
    realInJson += c;
  }
  expect(contains(absolutePathsInProject("{\"a\":{\"data\":\"z lo=1 | " + realInJson + "\"},\"b\":\"sf2:" + realInJson + "#3\"}"), real), "the check reads lines and references");
  expect(absolutePathsInProject("{\"a\":\"/no/such/file.wav\",\"b\":\"Samples/x.wav\"}").empty(), "the check ignores what is not real");

  // A file that cannot be read: an error, the archive as it was, no temporary file.
  const std::string goodZip = (base / "good.gridline.zip").u8string();
  Song g = emptySong();
  addChannel(g, "gl.sampler", real, true);
  expect(exportZippedProject(g, goodZip, "G").error.empty(), "report: a good archive first");
  const auto goodBytes = bytesOf(goodZip);
  const std::string locked = tone(src / "locked.wav", 700);
  addChannel(g, "gl.sampler", locked, false);
  fs::permissions(fs::u8path(locked), fs::perms::none);
  std::ifstream probe(locked);
  const bool canStillRead = static_cast<bool>(probe);  // (running as root)
  probe.close();
  if (!canStillRead) {
    CollectReport bad = exportZippedProject(g, goodZip, "G");
    expect(!bad.error.empty() && bad.error.find("locked.wav") != std::string::npos, "report: an unreadable file is an error that names it");
    expect(bytesOf(goodZip) == goodBytes, "report: the existing archive stays as it was");
    size_t left = 0;
    for (const auto& e : fs::directory_iterator(base)) left += e.path().filename().u8string().find(".tmp.") != std::string::npos ? 1 : 0;
    expect(left == 0, "report: no temporary file left");
  }
  fs::permissions(fs::u8path(locked), fs::perms::owner_read | fs::perms::owner_write);

  // Saved Track Receipt fields follow the files and keep no path.
  Song r = emptySong();
  addChannel(r, "gl.sampler", real, true);
  Json files = Json::makeObject();
  Json f = Json::makeObject();
  f.object["ticked"] = Json::makeNumber(1);
  files.object[real] = f;
  files.object[hidden] = f;
  Json receipt = Json::makeObject();
  receipt.object["files"] = files;
  r.trackReceipt = jsonStringify(receipt, 0);
  const std::string zr = (base / "r.gridline.zip").u8string();
  CollectReport rr = exportZippedProject(r, zr, "R");
  expect(rr.error.empty() && rr.unpacked.empty(), "receipt: no path to a real file stays in the fields");
}

// The old walk's false alarms: values that are not files never count as missing.
void testNoFalseAlarms() {
  Song s = emptySong();
  slicex::Model m = slicex::defaultModel();
  addChannel(s, "gl.slicer", "builtin:kick\n" + slicex::writeText(m), false);
  addChannel(s, "gl.sampler", "tone:piano:60:0", false);
  addChannel(s, "gl.pads", "\n\n\nnone", false);
  addChannel(s, "gl.multisampler", "", false);
  {
    namespace pl = packs::plasma;
    pl::Model pm = pl::defaultModel();
    pm.presetName = "A preset.wav";
    pm.osc[0].tableName = "Named.wav";
    pm.fxExtra[0].ir = "factory:Hall.wav";
    addChannel(s, "gl.plasma", pl::modelToData(pm), false);
  }
  // A note processor's state is walked too.
  {
    Song n = s;
    n.channels.front().noteFx.push_back(PluginState{"gl.sampler", {}, "/somewhere/note.wav", "", ""});
    expect(contains(referencedFiles(n), "/somewhere/note.wav"), "a note processor's file is listed");
  }
  const auto refs = referencedFiles(s);
  std::string list;
  for (const auto& r : refs) list += " [" + r + "]";
  expect(refs.empty(), "no file: nothing listed:" + list);
  expect(missingFiles(s).empty(), "no file: nothing missing");
}

bool hasBytes(const std::vector<char>& d, uint32_t signature) {
  for (size_t i = 0; i + 4 <= d.size(); ++i)
    if (static_cast<uint8_t>(d[i]) == (signature & 0xFF) && static_cast<uint8_t>(d[i + 1]) == ((signature >> 8) & 0xFF) &&
        static_cast<uint8_t>(d[i + 2]) == ((signature >> 16) & 0xFF) && static_cast<uint8_t>(d[i + 3]) == (signature >> 24))
      return true;
  return false;
}

// The standard tool checks every entry when it is installed (1 = not there, nothing is claimed then).
int unzipTest(const std::string& zip) {
  if (std::system("unzip -v > /dev/null 2>&1") != 0) return -1;
  return std::system(("unzip -tqq \"" + zip + "\" > /dev/null 2>&1").c_str());
}

// The writer's limits. Under them: the classic layout and nothing else.
void testZipLayout() {
  const fs::path base = fs::u8path(root) / "zip64";
  fs::create_directories(base);
  const std::string small = (base / "small.zip").u8string();
  const std::string src = tone(base / "src.wav", 440);
  {
    ZipWriter z;
    expect(z.open(small) && z.add("a.txt", {'h', 'i'}) && z.addFile("Samples/src.wav", src), "small: written");
    expect(z.close(), "small: closed");
  }
  const auto d = bytesOf(small);
  const auto wav = bytesOf(src);
  expect(d.size() == (30 + 5 + 2) + (30 + 15 + wav.size()) + (46 + 5) + (46 + 15) + 22u, "small: exactly the classic records (" + std::to_string(d.size()) + ")");
  expect(!hasBytes(d, 0x06064b50) && !hasBytes(d, 0x07064b50), "small: no ZIP64 records");
  std::vector<ZipEntry> es;
  std::vector<uint8_t> out;
  expect(readZipDirectory(small, es) && es.size() == 2 && readZipEntry(small, es[1], out) && out.size() == wav.size() && std::equal(out.begin(), out.end(), reinterpret_cast<const uint8_t*>(wav.data())),
         "small: a streamed file reads back, checksum checked");
  const int t = unzipTest(small);
  expect(t <= 0, "small: unzip -t accepts it");

  // More entries than the classic record counts: nothing large, so this one is quick.
  const std::string many = (base / "many.zip").u8string();
  {
    ZipWriter z;
    bool ok = z.open(many);
    for (int i = 0; ok && i < 65600; ++i) ok = z.add("n/" + std::to_string(i), {static_cast<uint8_t>(i)});
    expect(ok && z.close(), "many entries: written");
  }
  es.clear();
  expect(readZipDirectory(many, es) && es.size() == 65600, "many entries: all listed (" + std::to_string(es.size()) + ")");
  out.clear();
  expect(!es.empty() && readZipEntry(many, es.back(), out) && out.size() == 1 && out[0] == static_cast<uint8_t>(65599), "many entries: the last one reads");
  expect(hasBytes(bytesOf(many), 0x06064b50), "many entries: ZIP64 records written");
  expect(unzipTest(many) <= 0, "many entries: unzip -t accepts it");
  fs::remove(many);
}

uint64_t diskUse(const fs::path& p) {  // bytes really on disk
#ifdef _WIN32
  std::error_code ec;
  return fs::file_size(p, ec);
#else
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512u : 0;
#endif
}

// Past 4 GB for real: an entry that large, an entry and the directory beyond it. The source is a
// sparse file and silence stays a hole in the archive, so this reads 4 GB of zeros and uses next to
// no disk. Where the file system cannot do that (or there is little room) it says so and stops.
void testOver4Gb() {
  const fs::path base = fs::u8path(root) / "big";
  fs::create_directories(base);
  std::error_code ec;
  const uint64_t bigSize = (4ull << 30) + 12345;
  const fs::path bigFile = base / "big.wav";
  { std::ofstream(bigFile, std::ios::binary).put('\0'); }
  fs::resize_file(bigFile, bigSize, ec);
  if (ec || diskUse(bigFile) > (64u << 20)) {
    std::printf("# skipped the 4 GB archive: this file system has no sparse files\n");
    fs::remove(bigFile, ec);
    return;
  }
  const std::string after = tone(base / "after.wav", 880);
  const std::string zip = (base / "big.zip").u8string();
  std::string err;
  {
    ZipWriter z;
    expect(z.open(zip, &err) && z.add("first.txt", {'o', 'k'}), "4 GB: first entry");
    expect(z.addFile("Samples/big.wav", bigFile.u8string(), &err), "4 GB: the large entry (" + err + ")");
    expect(z.addFile("Samples/after.wav", after, &err), "4 GB: an entry beyond 4 GB");
    expect(z.close(&err), "4 GB: closed (" + err + ")");
  }
  expect(fs::file_size(zip) > bigSize, "4 GB: the archive is past 4 GB");
  expect(diskUse(zip) < (64u << 20), "4 GB: the archive stays small on disk");
  std::vector<ZipEntry> es;
  expect(readZipDirectory(zip, es, &err) && es.size() == 3, "4 GB: the reader lists it (" + err + ")");
  if (es.size() == 3) {
    expect(es[1].size == bigSize && es[1].packed == bigSize, "4 GB: the 64-bit size");
    expect(es[2].offset > bigSize, "4 GB: the 64-bit offset");
    std::vector<uint8_t> out;
    const auto wav = bytesOf(after);
    expect(readZipEntry(zip, es[2], out, &err) && out.size() == wav.size() && std::equal(out.begin(), out.end(), reinterpret_cast<const uint8_t*>(wav.data())), "4 GB: the entry beyond it reads back (" + err + ")");
    out.clear();
    expect(readZipEntry(zip, es[0], out) && out.size() == 2, "4 GB: the first entry reads back");
  }
  if (std::getenv("GRIDLINE_K44_UNZIP_BIG")) expect(unzipTest(zip) <= 0, "4 GB: unzip -t accepts it");  // (reads 4 GB more)
  fs::remove(zip, ec);
  fs::remove(bigFile, ec);
}

// A source that ends early or changes while it is read: an error, the target as it was.
void testStreamFailure() {
  const fs::path base = fs::u8path(root) / "short";
  fs::create_directories(base);
  const std::string src = tone(base / "s.wav", 440), zip = (base / "x.zip").u8string();
  {
    ZipWriter z;
    z.open(zip);
    z.add("a", {1});
    z.close();
  }
  const auto good = bytesOf(zip);
  {
    ZipWriter z;
    std::string err;
    z.open(zip);
    expect(!z.addFile("gone.wav", (base / "gone.wav").u8string(), &err) && !err.empty(), "a file that is not there is an error");
#if !defined(_WIN32)
    // (A failed read marks the writer: a folder opens but cannot be read. Windows does not open one.)
    expect(!z.addFile("folder", base.u8string(), &err) && !err.empty(), "what cannot be read is an error");
    expect(!z.close(&err), "a failed read: the archive can no longer replace the target");
#endif
  }
#if !defined(_WIN32)
  expect(bytesOf(zip) == good, "the target stays as it was");
#endif
  size_t left = 0;
  for (const auto& e : fs::directory_iterator(base)) left += e.path().filename().u8string().find(".tmp.") != std::string::npos ? 1 : 0;
  expect(left == 0, "no temporary file left");
}

}  // namespace

// Hosted plugins inside a patch, at any depth: what they name is packed and named, and their state
// is never edited (it is the plugin's own data).
void testNestedHosted() {
  const fs::path base = fs::u8path(root) / "nested";
  const fs::path src = base / "src";
  const std::string one = tone(src / "insidePatch.wav", 210), two = tone(src / "insideDeeper.wav", 220);
  auto node = [](const std::string& type, const std::string& data) {
    Json n = Json::makeObject();
    n.object["type"] = Json::makeString(type);
    n.object["data"] = Json::makeString(data);
    return n;
  };
  auto patch = [](std::vector<Json> nodes) {
    Json a = Json::makeArray();
    a.array = std::move(nodes);
    Json j = Json::makeObject();
    j.object["nodes"] = a;
    return jsonStringify(j, 0);
  };
  const std::string inner = patch({node("vst3:deep", "{\"s\":\"" + two + "\"}")});
  const std::string outer = patch({node("vst3:top", "{\"s\":\"" + one + "\"}"), node("gl.patcher", inner)});
  Song s = emptySong();
  addChannel(s, "gl.patcher", outer, false);
  const std::string zip = (base / "n.gridline.zip").u8string();
  CollectReport rep = exportZippedProject(s, zip, "N");
  expect(rep.error.empty(), "nested: written");
  expect(contains(rep.insidePlugins, one), "nested: a hosted plugin in a patch is scanned");
  expect(contains(rep.insidePlugins, two), "nested: a hosted plugin in a patch in a patch is scanned");
  std::vector<ZipEntry> entries;
  readZipDirectory(zip, entries);
  std::set<std::string> names;
  for (const auto& e : entries) names.insert(e.name);
  expect(names.count("Samples/Inside plugins/insidePatch.wav") && names.count("Samples/Inside plugins/insideDeeper.wav"), "nested: their files are packed");
  std::string file, err;
  expect(unpackZippedProject(zip, (base / "out").u8string(), file, &err), "nested: unpacks");
  Song back;
  expect(loadProject(file, back, &err), "nested: loads");
  bool same = false;
  for (const auto& c : back.channels)
    if (c.plugin.type == "gl.patcher") {
      Json a, b;
      same = jsonParse(c.plugin.data, a) && jsonParse(outer, b) && jsonStringify(a, 0) == jsonStringify(b, 0);
    }
  expect(same, "nested: the plugins' own state is not edited");
}

// #include and #define: the player reads them, packing follows them, loops end.
void testSfzDirectives() {
  namespace pl = packs::plasma;
  const fs::path base = fs::u8path(root) / "sfz";
  const fs::path src = base / "src";
  tone(src / "samples" / "one.wav", 240);
  tone(src / "samples" / "two.wav", 250);
  tone(src / "samples" / "three.wav", 260);
  tone(src / "samples" / "a#define b.wav", 270);
  writeText(src / "inc" / "regions.sfz",
            "#include \"inc/self.sfz\"\n#include \"../main.sfz\"\n// #define $DIR wrong\n<region> sample=$DIR/one.wav key=40\n#define $KEYS 61\n<region> sample=$DIR/two.wav key=$KEYS\n");
  writeText(src / "inc" / "self.sfz", "#include \"inc/self.sfz\"\n<region> sample=samples/three.wav key=70\n");
  writeText(src / "main.sfz",
            "#define $DIR samples\n#define $KEY 60\n#include \"inc/regions.sfz\"\n<region> sample=$DIR/three.wav key=$KEY\n#include \"inc/not there.sfz\"\n");
  const std::string mainMap = (src / "main.sfz").u8string();

  sfz::Expanded ex;
  expect(sfz::expand(mainMap, ex), "sfz: the map reads");
  expect(ex.looped, "sfz: an include loop is seen");
  expect(ex.missing.size() == 1 && ex.missing[0].find("not there.sfz") != std::string::npos, "sfz: a missing include is named");
  auto keysOf = [](const std::string& map) {
    std::vector<pl::Zone> zones;
    std::vector<int> keys;
    pl::parseSfz(map, zones, nullptr);
    for (const auto& z : zones) keys.push_back(z.loKey);
    return keys;
  };
  // The longest name wins: $KEYS is not $KEY followed by an S.
  expect(keysOf(mainMap) == std::vector<int>({70, 40, 61, 60}), "sfz: the player follows includes and defines, and the loops end");

  // A map without directives plays as before: comments gone, the rest untouched.
  const std::string plainText = "<region> sample=samples/one.wav key=30\n<region> sample=samples/a#define b.wav key=31\n<region> sample=samples/$two.wav key=32\n";
  writeText(src / "plain.sfz", "// sample=samples/two.wav\n" + plainText);
  expect(sfz::expand((src / "plain.sfz").u8string(), ex) && ex.text == "\n" + plainText && !ex.looped && ex.included.empty(), "sfz: a plain map's text is unchanged");
  expect(keysOf((src / "plain.sfz").u8string()) == std::vector<int>({30, 31}), "sfz: a plain map's samples are unchanged");

  Song s = emptySong();
  addChannel(s, "gl.sampler", mainMap, true);
  const std::string zip = (base / "s.gridline.zip").u8string();
  CollectReport rep = exportZippedProject(s, zip, "S");
  expect(rep.error.empty(), "sfz: packed");
  expect(rep.missing.size() == 1 && rep.missing[0].find("not there.sfz") != std::string::npos, "sfz: packing names the missing include");
  std::string file, err;
  const fs::path out = base / "out";
  expect(unpackZippedProject(zip, out.u8string(), file, &err), "sfz: unpacks");
  const fs::path packed = fs::weakly_canonical(out) / "Samples" / "main" / "main.sfz";
  expect(fs::exists(packed), "sfz: the map travels");
  for (const char* n : {"one.wav", "two.wav", "three.wav"}) expect(fs::exists(packed.parent_path() / n), std::string("sfz: a sample named through a define or an include travels: ") + n);
  fs::rename(src, base / "moved");
  sampleCache().clear();
  expect(keysOf(packed.u8string()) == std::vector<int>({70, 40, 61, 60}), "sfz: the packed map plays the same, on its own");
  std::ifstream pf(packed, std::ios::binary);
  const std::string packedText((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
  expect(packedText.find("#include") == std::string::npos && packedText.find('$') == std::string::npos, "sfz: a packed map is flat");

  // A map whose samples were not there yet is read again once something arrives (a shared project).
  const fs::path late = base / "late";
  writeText(late / "late.sfz", "<region> sample=arrives.wav key=50\n");
  pl::Model m = pl::defaultModel();
  m.osc[0].sfz = (late / "late.sfz").u8string();
  PluginState st;
  st.type = "gl.plasma";
  st.data = pl::modelToData(m);
  auto first = pl::buildAux(st);
  expect(first && first->osc[0].zones.empty(), "sfz: no zone before the sample is there");
  expect(pl::buildAux(st) == first, "sfz: nothing new, nothing rebuilt");
  const std::string arrives = tone(late / "arrives.wav", 280);
  sampleCache().forget(arrives);
  auto second = pl::buildAux(st);
  expect(second && second->osc[0].zones.size() == 1, "sfz: the map fills in when the sample arrives");
}

// Origin and source notes travel with their files and the Track Receipt mapping follows.
void testSidecars() {
  namespace rc = receipt;
  const fs::path base = fs::u8path(root) / "side";
  const fs::path src = base / "src";
  const std::string original = tone(src / "orig" / "original.wav", 290), stem = tone(src / "stems" / "stem.wav", 300), edit = tone(src / "edit.wav", 310);
  const std::string base2 = tone(src / "orig" / "packed source.wav", 320), derived = tone(src / "derived.wav", 330);
  rc::Origin o;
  o.source = original, o.start = 1, o.end = 2, o.kind = "stem";
  expect(rc::writeOrigin(stem, o), "sidecars: written");
  o.source = stem, o.start = 0.25, o.end = 0.75, o.kind = "edit";
  expect(rc::writeOrigin(edit, o), "sidecars: written");
  o.source = base2, o.start = 0, o.end = 0.5, o.kind = "slice";
  o.sources = {base2, original};
  expect(rc::writeOrigin(derived, o), "sidecars: written");
  const std::string note = "{\"title\":\"A title\",\"uploader\":\"Someone\",\"site\":\"Example\",\"url\":\"https://example.org/x\"}\n";
  writeText(fs::u8path(edit + ".source.json"), note);
  Song s = emptySong();
  addChannel(s, "gl.sampler", edit, true);
  addChannel(s, "gl.sampler", derived, true);
  addChannel(s, "gl.sampler", base2, true);

  auto check = [&](const fs::path& place, const std::string& what) {
    const std::vector<rc::Origin> chain = rc::originChain((place / "edit.wav").u8string());
    expect(chain.size() == 2, what + ": the whole chain is there (" + std::to_string(chain.size()) + ")");
    if (chain.size() == 2) {
      expect(chain[0].start == 0.25 && chain[0].end == 0.75 && chain[1].start == 1 && chain[1].end == 2, what + ": the ranges are kept");
      expect(fs::u8path(chain[0].source).filename() == "stem.wav" && fs::u8path(chain[1].source).filename() == "original.wav", what + ": the chain names its files");
      expect(chain[0].source.find(src.u8string()) == std::string::npos && chain[1].source.find(src.u8string()) == std::string::npos, what + ": no path back to the old place");
    }
    rc::Origin d;
    expect(rc::readOrigin((place / "derived.wav").u8string(), d), what + ": a packed source's note reads");
    expect(fs::exists(fs::u8path(d.source)) && fs::u8path(d.source).filename() == "packed source.wav" && d.source.find(src.u8string()) == std::string::npos,
           what + ": the note leads to the packed source");
    expect(d.sources.size() == 2 && d.sources[0] == d.source && fs::u8path(d.sources[1]).filename() == "original.wav", what + ": every source of a note follows");
    std::ifstream f(place / "edit.wav.source.json", std::ios::binary);
    const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    expect(t == note, what + ": the source note travels as it is");
  };
  const std::string zip = (base / "z.gridline.zip").u8string();
  CollectReport rep = exportZippedProject(s, zip, "Z");
  expect(rep.error.empty() && rep.unpacked.empty(), "sidecars: nothing left unpacked");
  std::string file, err;
  const fs::path out = base / "out";
  expect(unpackZippedProject(zip, out.u8string(), file, &err), "sidecars: unpacks (" + err + ")");
  const fs::path dataOut = base / "Data";
  CollectReport dataRep = exportProjectDataFiles(s, dataOut.u8string(), "Z");
  expect(dataRep.error.empty(), "sidecars: data files");
  // The FL package carries them too.
  {
    Song fl = emptySong();
    addChannel(fl, "gl.sampler", derived, true);
    addChannel(fl, "gl.sampler", base2, true);
    fl.playlist.clear();
    flp::PackageOptions opt;
    opt.flp.factoryData.clear();
    opt.flp.renderInstruments = false;
    flp::PackageReport fr;
    const std::string fz = (base / "fl.zip").u8string();
    expect(flp::exportFlpZip(fl, fz, "fl", opt, fr, &err), "sidecars: FL package written (" + err + ")");
    std::vector<ZipEntry> entries;
    readZipDirectory(fz, entries);
    std::string text;
    for (const auto& e : entries)
      if (e.name == "derived.wav.origin.json") {
        std::vector<uint8_t> d;
        readZipEntry(fz, e, d);
        text.assign(d.begin(), d.end());
      }
    Json j;
    const Json* sourceField = jsonParse(text, j) ? j.find("source") : nullptr;
    expect(sourceField && sourceField->str == "packed source.wav", "sidecars: the FL package's note names the packed source");
  }
  fs::rename(src, base / "moved");
  check(fs::weakly_canonical(out) / "Samples", "zip");
  check(fs::weakly_canonical(dataOut) / "Samples", "data files");
}

int main() {
  root = fs::path(gl::test::hermeticTestEnv("k44")).lexically_normal().u8string();
  testNoFalseAlarms();
  testRoundTrip(true);
  testFromCache();
  testDataFilesAndBones();
  testFlPackage();
  testReport();
  testNestedHosted();
  testSfzDirectives();
  testSidecars();
  testZipLayout();
  testOver4Gb();
  testStreamFailure();
  return finish();
}
