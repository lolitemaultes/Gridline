// Gridline - J-147: plugin manager logic: one row per plugin across formats (grouping
// keys, vendor matching, Gridline Suite builds kept apart), clean categories from every
// format's category strings, the preferred format (global order, per-plugin pick), what
// the browser lists, and the settings file keeping old favourites / sandbox flags.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "host/HostedPlugins.h"
#include "host/PluginGroups.h"
#include "host/PluginPrefs.h"

using namespace gl;
using namespace gl::host;
namespace fs = std::filesystem;

namespace {

GroupItem item(const std::string& type, const std::string& name, const std::string& vendor, const std::string& sub, bool inst) {
  GroupItem g;
  g.type = type;
  g.format = type.substr(0, type.find(':'));
  g.name = name;
  g.vendor = vendor;
  g.subCategories = sub;
  g.instrument = inst;
  return g;
}

void categories() {
  // VST3 sub-categories
  expect(normaliseCategory("Instrument|Drum", true) == "Drums", "vst3 Instrument|Drum -> Drums");
  expect(normaliseCategory("Instrument|Synth", true) == "Synth", "vst3 Instrument|Synth -> Synth");
  expect(normaliseCategory("Instrument|Sampler", true) == "Sampler", "vst3 Instrument|Sampler -> Sampler");
  expect(normaliseCategory("Instrument|Piano", true) == "Keys", "vst3 Instrument|Piano -> Keys");
  expect(normaliseCategory("Fx|Dynamics", false) == "Dynamics", "vst3 Fx|Dynamics");
  expect(normaliseCategory("Fx|Analyzer", false) == "Analyzer", "vst3 Fx|Analyzer");
  expect(normaliseCategory("Fx|EQ", false) == "EQ", "vst3 Fx|EQ");
  expect(normaliseCategory("Fx|Pitch Shift", false) == "Pitch", "vst3 Fx|Pitch Shift");
  expect(normaliseCategory("Fx|Spatial", false) == "Spatial", "vst3 Fx|Spatial");
  expect(normaliseCategory("Fx|Tools", false) == "Utility", "vst3 Fx|Tools -> Utility");
  expect(normaliseCategory("Fx|Delay|Modulation", false) == "Delay", "first rule wins: delay before modulation");
  expect(normaliseCategory("Fx|Analyzer|Spatial", false) == "Analyzer", "analyzer before spatial");
  // CLAP features: channel layouts and kind words do not count
  expect(normaliseCategory("instrument|drum|stereo", true) == "Drums", "clap instrument|drum|stereo");
  expect(normaliseCategory("instrument|synthesizer|stereo", true) == "Synth", "clap synthesizer");
  expect(normaliseCategory("audio-effect|analyzer", false) == "Analyzer", "clap audio-effect|analyzer");
  expect(normaliseCategory("audio-effect|compressor|stereo", false) == "Dynamics", "clap compressor");
  expect(normaliseCategory("audio-effect|equalizer", false) == "EQ", "clap equalizer");
  expect(normaliseCategory("audio-effect|stereo", false) == "Effect", "stereo alone is a layout, not a category");
  expect(normaliseCategory("audio-effect|utility|stereo", false) == "Utility", "clap utility");
  expect(normaliseCategory("audio-effect|distortion|mastering", false) == "Distortion", "distortion before mastering");
  expect(normaliseCategory("note-effect", false) == "MIDI", "clap note-effect -> MIDI");
  // LV2 classes
  expect(normaliseCategory("ReverbPlugin", false) == "Reverb", "lv2 ReverbPlugin");
  expect(normaliseCategory("EQPlugin", false) == "EQ", "lv2 EQPlugin");
  expect(normaliseCategory("CompressorPlugin|DynamicsPlugin", false) == "Dynamics", "lv2 compressor");
  expect(normaliseCategory("AmplifierPlugin", false) == "Distortion", "lv2 amplifier");
  expect(normaliseCategory("InstrumentPlugin", true) == "Instrument", "lv2 InstrumentPlugin is generic");
  // AU / VST2: only the kind; the name helps
  expect(normaliseCategory("Fx", false) == "Effect", "au Fx alone -> Effect");
  expect(normaliseCategory("Instrument", true) == "Instrument", "au Instrument alone -> Instrument");
  expect(normaliseCategory("Fx", false, "AUMatrixReverb") == "Reverb", "camel-case name -> Reverb");
  expect(normaliseCategory("Fx", false, "AUBandpass") == "Filter", "name bandpass -> Filter");
  expect(normaliseCategory("Fx", false, "Peak Limiter") == "Dynamics", "name limiter -> Dynamics");
  expect(normaliseCategory("Fx", false, "Graphic Equalizer") == "EQ", "name equalizer -> EQ");
  expect(normaliseCategory("Fx", false, "Cheap Amp") == "Effect", "short words never match names");
  expect(normaliseCategory("Fx|Music effect", false) == "Effect", "au music effect");
  expect(normaliseCategory("", true, "Note Sampler") == "Sampler", "instrument name -> Sampler");
  expect(isGenericCategory("Effect") && isGenericCategory("Instrument") && isGenericCategory("") && !isGenericCategory("EQ"), "generic categories");
  bool listed = true;
  for (const char* s : {"Drums", "Synth", "Keys", "Sampler", "Instrument", "Reverb", "Delay", "EQ", "Filter", "Dynamics", "Effect"}) {
    bool found = false;
    for (const char* c : categoryNames()) found = found || std::string(c) == s;
    listed = listed && found;
  }
  expect(listed, "categoryNames lists every result");
}

void suiteTypes() {
  expect(isOwnSuiteType("clap:org.gridline.suite.abacus"), "suite CLAP id");
  expect(isOwnSuiteType("vst3:475249444C494E45535549541A2B3C4D"), "suite VST3 class");
  expect(isOwnSuiteType("vst3:47524944-4c494e45-53554954-1a2b3c4d"), "suite VST3 class, dashes and lower case");
  expect(isOwnSuiteType("au:aumu/abac/GrLn"), "suite AU manufacturer");
  expect(!isOwnSuiteType("au:aumu/abac/GrIn"), "Gridline's own app units are not suite builds");
  expect(!isOwnSuiteType("clap:org.gridline.other"), "other gridline CLAP id");
  expect(!isOwnSuiteType("vst3:0123456789ABCDEF0123456789ABCDEF"), "third-party VST3");
  expect(!isOwnSuiteType("gl.abacus"), "built-in type");
}

void groupKeys() {
  expect(groupNameKey("Copperverb") == groupNameKey("copperverb"), "name key ignores case");
  expect(groupNameKey("Copperverb (VST3)") == "copperverb", "name key drops (VST3)");
  expect(groupNameKey("Copperverb VST") == "copperverb", "name key drops VST");
  expect(groupNameKey("Copperverb x64") == "copperverb", "name key drops x64");
  expect(groupNameKey("Copperverb-x64 (VST3)") == "copperverb", "name key drops stacked tags");
  expect(groupNameKey("Phaseshift 4") == "phaseshift4", "digits kept");
  expect(groupNameKey("Copperverb (Mono)") != groupNameKey("Copperverb"), "variants stay apart");
  expect(groupNameKey("Au") == "au", "a name that is only a tag stays");
  expect(groupVendorKey("FabFilter Software Instruments") == groupVendorKey("FabFilter"), "vendor key: first word");
  expect(groupVendorKey("u-he") == "uhe", "vendor key: punctuation");
  expect(groupVendorKey("") == "", "empty vendor");
  auto o = formatOrder("");
  expect(o.size() == 5 && o[0] == "vst3" && o[1] == "clap" && o[2] == "au", "default order VST3, CLAP, AU");
  o = formatOrder("clap");
  expect(o[0] == "clap" && o[1] == "vst3" && o.size() == 5, "preferred CLAP first");
  o = formatOrder("bogus");
  expect(o[0] == "vst3", "unknown preference: default order");
}

void grouping() {
  std::vector<GroupItem> items = {
      item("au:aufx/copp/Hald", "Copperverb", "Halden Sound", "Fx", false),                        // 0
      item("clap:com.halden.copperverb", "Copperverb", "Halden Sound", "audio-effect|reverb", false),  // 1
      item("vst3:AAAA", "Copperverb", "Halden Sound", "Fx|Reverb", false),                         // 2
      item("vst2:12345678", "Copperverb", "", "Fx", false),                                        // 3: no vendor: joins
      item("vst3:BBBB", "Copperverb", "Other Maker", "Fx|Reverb", false),                          // 4: other vendor: apart
      item("clap:org.gridline.suite.abacus", "Abacus", "Gridline", "instrument|drum", true),       // 5
      item("vst3:475249444C494E4553554954AAAAAAAA", "Abacus", "Gridline", "Instrument|Drum", true),  // 6
      item("vst3:CCCC", "Abacus", "Gridline", "Instrument|Drum", true),                            // 7: not a suite id: apart
      item("au:aufx/mtrx/syst", "AUMatrixReverb", "System", "Fx", false),                          // 8
  };
  auto g = groupPlugins(items, "", {});
  expect(g.size() == 5, "nine entries -> five plugins");
  const PluginGroup& cv = g[0];
  expect(cv.members.size() == 4, "copperverb: four formats in one row");
  expect(cv.preferred == 2, "default preference: VST3");
  expect(cv.members[0] == 2 && cv.members[1] == 1 && cv.members[2] == 0 && cv.members[3] == 3, "members in format order");
  expect(cv.category == "Reverb", "group category from the most specific format");
  expect(cv.vendor == "Halden Sound" && !cv.instrument && !cv.ownSuite, "group vendor / kind");
  expect(g[1].members.size() == 1 && g[1].vendor == "Other Maker", "same name, other vendor: its own row");
  expect(g[2].ownSuite && g[2].members.size() == 2 && g[2].category == "Drums", "suite builds grouped and marked");
  expect(!g[3].ownSuite && g[3].members.size() == 1, "a non-suite plugin never merges with suite builds");
  expect(g[4].category == "Reverb", "AU-only plugin: category from its name");

  g = groupPlugins(items, "clap", {});
  expect(g[0].preferred == 1 && !g[0].userPicked, "global preference CLAP");
  g = groupPlugins(items, "lv2", {});
  expect(g[0].preferred == 2, "preferred format missing: next in order (VST3)");
  g = groupPlugins(items, "clap", {"au:aufx/copp/Hald"});
  expect(g[0].preferred == 0 && g[0].userPicked && g[0].members[0] == 0, "per-plugin pick wins and leads");
  expect(g[0].category == "Reverb", "category still from the specific format");
  g = groupPlugins({}, "", {});
  expect(g.empty(), "no items, no groups");
}

void prefsFile() {
  const fs::path dir = fs::temp_directory_path() / "gridline_j147_prefs";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  {  // a settings file from before J-147
    std::ofstream f(dir / "plugin-manager.json");
    f << R"({"version":1,"extraFolders":["/x"],"disabledFormats":["lv2"],"sandbox":["vst3:AAAA"],"favourites":["clap:com.halden.copperverb"],"categories":{"vst3:AAAA":"Verbs"},"options":{}})";
  }
  loadPluginPrefs(dir.string());
  PluginPrefs p = pluginPrefs();
  expect(p.favourites.count("clap:com.halden.copperverb") && p.sandbox.count("vst3:AAAA") && p.categories["vst3:AAAA"] == "Verbs",
         "old favourites, sandbox and categories load unchanged");
  expect(p.preferredFormat.empty() && p.preferTypes.empty() && !p.browserAllFormats, "new settings default");
  p.preferredFormat = "clap";
  p.preferTypes.insert("au:aufx/copp/Hald");
  p.browserAllFormats = true;
  setPluginPrefs(p);
  loadPluginPrefs(dir.string());
  PluginPrefs q = pluginPrefs();
  expect(q == p, "new settings round-trip");
  expect(q.favourites == p.favourites && q.sandbox == p.sandbox, "favourites / sandbox kept per type");
  fs::remove_all(dir, ec);
}

void snapshot() {
  const fs::path dir = fs::temp_directory_path() / "gridline_j147_snap";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  loadPluginPrefs(dir.string());
  auto reg = [](const std::string& fmt, const std::string& uid, const std::string& name, const std::string& vendor, const std::string& sub) {
    ScanResult r;
    r.format = fmt;
    r.bundle = "/plugins/" + name + "." + fmt;
    r.ok = true;
    ScannedClass c;
    c.uid = uid;
    c.name = name;
    c.vendor = vendor;
    c.subCategories = sub;
    r.classes.push_back(c);
    registerScanResult(r);
  };
  reg("vst3", "DDDD0000DDDD0000DDDD0000DDDD0000", "Kilnpress", "Ironhill DSP", "Fx|Dynamics");
  reg("clap", "com.ironhill.kilnpress", "Kilnpress", "Ironhill DSP", "audio-effect|compressor");
  reg("clap", "org.gridline.suite.chorus", "Chorus", "Gridline", "audio-effect|chorus");
  const std::string v3 = "vst3:DDDD0000DDDD0000DDDD0000DDDD0000", cl = "clap:com.ironhill.kilnpress";
  auto s = pluginGroups();
  expect(s->groupOf.count(v3) && s->groupOf.count(cl) && s->groupOf.at(v3) == s->groupOf.at(cl), "snapshot groups the registered formats");
  expect(listedInBrowser(v3) && !listedInBrowser(cl), "browser lists the preferred format only");
  expect(!listedInBrowser("clap:org.gridline.suite.chorus"), "browser never lists Gridline Suite builds");
  expect(listedInBrowser("gl.chorus"), "built-in types always listed");
  PluginPrefs p = pluginPrefs();
  p.preferredFormat = "clap";
  setPluginPrefs(p);
  expect(!listedInBrowser(v3) && listedInBrowser(cl), "preference change rebuilds the snapshot");
  p.preferTypes.insert(v3);
  setPluginPrefs(p);
  expect(listedInBrowser(v3) && !listedInBrowser(cl), "per-plugin pick");
  p.browserAllFormats = true;
  setPluginPrefs(p);
  expect(listedInBrowser(v3) && listedInBrowser(cl) && !listedInBrowser("clap:org.gridline.suite.chorus"), "every format listed, still no Suite builds");
  p.disabledFormats.insert("clap");
  p.browserAllFormats = false;
  p.preferTypes.clear();
  setPluginPrefs(p);
  s = pluginGroups();
  expect(!s->groupOf.count(cl) && listedInBrowser(v3), "a switched-off format leaves the group");
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  categories();
  suiteTypes();
  groupKeys();
  grouping();
  prefsFile();
  snapshot();
  return finish();
}
