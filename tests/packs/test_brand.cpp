// Gridline — plugin names and brand system (J-94): unique, original catalogue
// names, flagship families and taglines, former names still searchable, and
// the FL Studio import / export reports naming the Gridline plugins by their
// display names.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/core/BrowserIndex.h"
#include "../fixtures/TestSong.h"
#include "../../src/core/PluginBrand.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Project.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpExport.h"
#include "../Check.h"

using namespace gl;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// True when `word` occurs in `text` as a whole word (case-insensitive).
bool hasWord(const std::string& text, const std::string& word) {
  const std::string t = lower(text), w = lower(word);
  for (size_t at = t.find(w); at != std::string::npos; at = t.find(w, at + 1)) {
    const bool l = at == 0 || !std::isalnum(static_cast<unsigned char>(t[at - 1]));
    const size_t e = at + w.size();
    const bool r = e >= t.size() || !std::isalnum(static_cast<unsigned char>(t[e]));
    if (l && r) return true;
  }
  return false;
}

// Names of FL Studio's own plugins (and their distinctive parts) that no
// Gridline display name or search keyword may use. Plain industry terms FL
// also uses descriptively (Delay, Compressor, Parametric EQ, Vocoder, ...)
// are not on the list.
const std::vector<std::string>& flNames() {
  static const std::vector<std::string> v = {
      "Fruity", "Image-Line", "Edison", "Gross Beat", "Sytrus", "Harmor", "Harmless", "Maximus", "Slicex", "FPC",
      "Vocodex", "Soundgoodizer", "Newtone", "Newtime", "Patcher", "Pitcher", "Transistor Bass", "Toxic Biohazard",
      "Sakura", "Ogun", "Autogun", "Morphine", "Kepler", "PoiZone", "Sawer", "SimSynth", "BooBass", "Drumaxx",
      "Drumpad", "BeepMap", "DirectWave", "Wasp", "FLEX", "GMS", "Groove Machine", "Plucked", "Distructor", "EQUO",
      "Effector", "Emphasis", "Emphasizer", "Transmitter", "Transporter", "Hardcore", "LuxeVerb", "Spreader",
      "Low Lifter", "Wave Candy", "Vintage Chorus", "Vintage Phaser", "Hyper Chorus", "Love Philter", "Flangus",
      "Spectroman", "Reeverb", "PanOMatic", "Blood Overdrive", "Granulizer", "Slayer", "Wave Traveller", "Fruit Kick",
      "BassDrum", "DrumSynth", "Dashboard", "Control Surface", "Frequency Splitter", "Delay Bank", "Squeeze",
      "Scratcher", "Transient Processor", "Peak Controller", "Formula Controller", "Envelope Controller",
      "Keyboard Controller", "NoteBook", "Big Clock", "Fast Dist", "Fast LP", "Free Filter", "Level Scaler",
      "Key Mapper", "Color Mapper", "Keyboard Splitter", "Wrapper", "Minihost", "ZGameEditor", "Stereo Shaper",
      "Stereo Enhancer", "Soft Clipper", "Mute 2", "Balance", "Phase Inverter", "Video Player", "Razer Chroma",
      "VFX"};
  return v;
}

// The renames (type → former name, new name): new names are the catalogue's,
// former names stay searchable.
struct Rename {
  const char* type;
  const char* before;
  const char* after;
};
const std::vector<Rename>& renames() {
  static const std::vector<Rename> v = {
      {"gl.synth", "Trio Synth", "Tercet"},          {"gl.pads", "Drum Pads", "Abacus"},
      {"gl.slicer", "Slicer", "Kerf"},               {"gl.wavetable", "Wavetable", "Isoline"},
      {"gl.matrixfm", "Matrix FM", "Lattice"},       {"gl.waveprogram", "Wave Program", "Folio"},
      {"gl.additive", "Additive", "Facet"},          {"gl.harmor", "Resynth", "Halftone"},
      {"gl.patcher", "Patch Host", "Junction"},      {"gl.patcherfx", "Patch Host FX", "Junction FX"},
      {"gl.delay", "Delay", "Tracer"},               {"gl.hall", "Hall", "Cloister"},
      {"gl.timegate", "Time Gate", "Shuttle"},       {"gl.master", "Mastering Dynamics", "Crest"},
      {"gl.vocoder", "Vocoder", "Cipher"},           {"gl.pitcher", "Pitch Corrector", "Plumb"},
      {"gl.scope", "Scope", "Graticule"},            {"gl.edison", "Audio Editor", "Loupe"},
      {"gl.analog", "Analog", "Analog Synth"},       {"gl.gms", "Groove Synth", "Hybrid Synth"},
      {"gl.plucker", "Plucker", "Pluck Model"},      {"gl.envctl", "Envelope Controller", "Envelope Mod"},
      {"gl.keyctl", "Keyboard Controller", "Keyboard Mod"}, {"gl.formulactl", "Formula Controller", "Formula Mod"},
      {"gl.surface", "Control Surface", "Control Deck"}, {"gl.clipper", "Soft Clipper", "Clipper"},
      {"gl.stereo", "Stereo Shaper", "Stereo Width"}, {"gl.lowlift", "Low Lifter", "Sub Focus"},
      {"gl.scratcher", "Scratcher", "Turntable"},    {"gl.spread", "Spreader", "Stereo Spread"},
      {"gl.hitloop", "Hit Looper", "Hit Repeater"},  {"gl.amprack", "Amp Rack", "Pedal Rack"},
      {"gl.squeeze", "Squeeze", "Pinch"},            {"gl.pocketrack", "Pocket Rack", "Pocket FX"},
      {"gl.keymap", "Key Mapper", "Key Remap"},      {"gl.split", "Key Splitter", "Key Zones"},
      {"gl.velscale", "Level Scaler", "Velocity Scale"}, {"gl.freefilter", "Free Filter", "Biquad Filter"},
      {"gl.splinescratch", "Spline Scratcher", "Spline Scratch"},
  };
  return v;
}

bool ascii(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= 32 && c < 127; });
}

void testCatalogueNames() {
  std::set<std::string> seen;
  bool unique = true, plain = true, shortEnough = true;
  for (const PluginInfo& p : pluginCatalog()) {
    if (!seen.insert(lower(p.name)).second) {
      unique = false;
      std::printf("# duplicate name: %s (%s)\n", p.name.c_str(), p.type.c_str());
    }
    if (p.name.empty() || !ascii(p.name)) plain = false;
    if (p.name.size() > 20) {  // fits a picker tile and a mixer slot label at 100%
      shortEnough = false;
      std::printf("# long name: %s\n", p.name.c_str());
    }
  }
  expect(unique, "catalogue display names are unique (case-insensitive)");
  expect(plain, "catalogue display names are non-empty ASCII");
  expect(shortEnough, "catalogue display names are at most 20 characters");
}

void testNoFlNames() {
  bool clean = true;
  for (const PluginInfo& p : pluginCatalog())
    for (const std::string& fl : flNames()) {
      if (hasWord(p.name, fl)) {
        clean = false;
        std::printf("# %s: display name \"%s\" uses FL's \"%s\"\n", p.type.c_str(), p.name.c_str(), fl.c_str());
      }
      if (hasWord(pluginKeywords(p.type), fl) || hasWord(pluginTagline(p.type), fl)) {
        clean = false;
        std::printf("# %s: keywords / tagline use FL's \"%s\"\n", p.type.c_str(), fl.c_str());
      }
    }
  expect(clean, "no display name, keyword or tagline uses an FL Studio plugin name");
}

void testRenames() {
  bool named = true, findable = true;
  for (const Rename& r : renames()) {
    const PluginInfo* p = findPlugin(r.type);
    if (!p || p->name != r.after) {
      named = false;
      std::printf("# %s is \"%s\", expected \"%s\"\n", r.type, p ? p->name.c_str() : "?", r.after);
      continue;
    }
    // A former name that was also an FL plugin's name is not kept (clean room).
    const bool flName = std::any_of(flNames().begin(), flNames().end(), [&](const std::string& f) { return hasWord(r.before, f); });
    if (!flName && !browse::matchesPlugin(*p, browse::parseQuery(lower(r.before)))) {
      findable = false;
      std::printf("# %s not found by its former name \"%s\"\n", r.type, r.before);
    }
    if (!browse::matchesPlugin(*p, browse::parseQuery(lower(r.after)))) findable = false;
  }
  expect(named, "renamed plugins carry their new catalogue names");
  expect(findable, "every renamed plugin is found by its new and its former name");
  // Everyday words still lead to the plugin.
  auto finds = [](const char* q, const char* type) {
    const PluginInfo* p = findPlugin(type);
    return p && browse::matchesPlugin(*p, browse::parseQuery(q));
  };
  expect(finds("reverb", "gl.hall") && finds("reverb", "gl.reverb"), "\"reverb\" finds Cloister and Reverb");
  expect(finds("time gate", "gl.timegate") && finds("stutter", "gl.timegate"), "\"time gate\" / \"stutter\" find Shuttle");
  expect(finds("slicer", "gl.slicer") && finds("audio editor", "gl.edison"), "\"slicer\" finds Kerf, \"audio editor\" finds Loupe");
  expect(!finds("shuttle", "gl.reverb"), "keywords do not leak to other plugins");
}

void testBrands() {
  bool known = true, taglines = true, families = true;
  int flagships = 0;
  std::set<std::string> types;
  for (const PluginBrand& b : pluginBrands()) {
    if (!types.insert(b.type).second || !findPlugin(b.type)) known = false;
    const PluginInfo* p = findPlugin(b.type);
    if (b.family != BrandFamily::None) {
      ++flagships;
      const std::string tl = b.tagline;
      if (tl.empty() || !ascii(tl) || tl.size() > 64) taglines = false;
      // Flagship names: one short word (the FX twin of an instrument adds "FX").
      if (p) {
        std::string n = p->name;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, " FX") == 0) n.resize(n.size() - 3);
        if (n.find(' ') != std::string::npos || n.size() > 10) families = false;
      }
    } else if (b.tagline[0]) {
      taglines = false;
    }
    if (!ascii(b.keywords) || lower(b.keywords) != b.keywords) known = false;
  }
  std::printf("# %d flagships, %zu brand entries\n", flagships, pluginBrands().size());
  expect(known, "brand entries name catalogue types once, keywords lower-case ASCII");
  expect(taglines, "every flagship has a short ASCII tagline, tools none");
  expect(families, "flagship names are one word of at most 10 letters");
  expect(flagships >= 15, "at least 15 flagships");
  expect(isFlagship("gl.timegate") && !isFlagship("gl.comp"), "Shuttle is a flagship, Compressor a tool");
  expect(pluginTitle("gl.timegate") == "SHUTTLE" && pluginTitle("gl.matrixfm") == "LATTICE", "skin titles come from the catalogue");
  expect(pluginDisplayName("gl.nope") == "gl.nope", "unknown types show their id");
}

// The FL reports name the Gridline side by its display name.
void testFlReports() {
  flp::ExportReport rep;
  rep.plugins.push_back({"insert 1 slot 1", "gl.timegate", "Gross Beat", "approx", "test"});
  const std::string text = flp::reportText(rep);
  expect(text.find("Shuttle (gl.timegate) -> Gross Beat") != std::string::npos, "export report: Gridline display name and type");

  Song s = makeTestSong();
  s.channels.resize(1);
  s.channels[0].name = "Probe";
  s.channels[0].mixerTrack = s.mixer[1].id;
  s.channels[0].plugin = defaultPluginState("gl.matrixfm");
  for (auto& t : s.mixer) t.slots.clear();
  s.mixer[1].slots.resize(1);
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.timegate");
  s.automations.clear(), s.links.clear(), s.modLinks.clear();
  flp::ExportOptions o;
  o.useOrigin = false;
  flp::ExportReport er;
  flp::Bytes b;
  const bool exported = flp::exportFlp(s, b, er, o);
  expect(exported, "FL export of a Lattice channel and a Shuttle slot");
  Song back;
  flp::Report ir;
  std::string err;
  const bool imported = exported && importFlpBytes(std::vector<uint8_t>(b.begin(), b.end()), back, ir, &err, "", "", false);
  expect(imported, "re-imported");
  bool lattice = false, shuttle = false;
  for (const auto& [k, v] : ir.mapped) {
    std::printf("# mapped: %s (%d)\n", k.c_str(), v);
    if (k.find("\xE2\x86\x92 Lattice") != std::string::npos) lattice = true;
    if (k.find("\xE2\x86\x92 Shuttle") != std::string::npos) shuttle = true;
  }
  expect(lattice && shuttle, "import report: FL plugin -> Lattice / Shuttle");
}

}  // namespace

int main() {
  testCatalogueNames();
  testNoFlNames();
  testRenames();
  testBrands();
  testFlReports();
  return finish();
}
