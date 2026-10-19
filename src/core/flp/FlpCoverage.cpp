// Gridline — FL Studio import coverage (event catalog + file tally).
//
// The catalog records, for every event id found in FL Studio's own shipped
// projects and presets (analysis/reference/format-flp.md), whether the
// importer decodes it and, when it does not, why. Keep it in step with the
// area importers (FlpProject / FlpChannels / FlpPatterns / FlpMixer /
// FlpArrangement / FlpPlugins); tests/test_coverage.cpp checks it is complete
// and ordered.
#include "FlpCoverage.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include "../Json.h"
#include "../PluginInfo.h"
#include "FlpPresets.h"

namespace gl::flp {

namespace {
constexpr auto D = EventInfo::Decoded;
constexpr auto P = EventInfo::Partial;
constexpr auto I = EventInfo::Ignored;
constexpr int64_t kNone = 4294967295LL;  // DWORD -1
}  // namespace

const std::vector<EventInfo>& eventCatalog() {
  static const std::vector<EventInfo> cat = {
      {0, "channel switch (on / off)", D, "", 1},
      {9, "project: loop switch", I, "transport state at save time; Gridline loops by time selection", -1},
      {10, "project: show info on open", I, "user-interface preference", -1},
      {11, "main swing (0..128)", D, "", 0},
      {15, "channel zipped in the rack", I, "rack display state (Gridline has no zipped rows)", 0},
      {17, "time signature numerator", D, "", 4},
      {18, "time signature beat", D, "", 4},
      {19, "channel: legacy (preset files)", I, "only in 5 channel presets; no observable effect", -1},
      {20, "sampler ping-pong loop", D, "", 0},
      {21, "channel type", D, "", -1},
      {22, "channel → mixer insert (FL ≤ 24.2)", D, "", -1},
      {23, "project pan law", I, "Gridline uses one pan law (FL's default, used by 163 of 164 shipped projects)", 0},
      {25, "pattern: legacy", I, "2 files saved by FL 11–12; no observable effect", -1},
      {26, "pattern looped (performance)", I, "2 files; Gridline loops pattern clips by their length", -1},
      {27, "mixer: legacy insert property", I, "FL ≤ 11 files; constant value", 12},
      {28, "project: licence state", I, "licensing, never imported", -1},
      {29, "mixer: automatic delay compensation", I, "Gridline always compensates plugin delay", 1},
      {30, "play truncated notes in patterns", I, "switch on in every project that stores it; Gridline cuts notes at the clip end", 1},
      {31, "project: unknown switch", I, "no observable effect identified", -1},
      {32, "channel locked", I, "rack edit lock; Gridline has no channel lock", 0},
      {33, "marker: time signature numerator", D, "", -1},
      {34, "marker: time signature beat", D, "", -1},
      {35, "project: unknown switch (FL 12.9+)", I, "no observable effect identified", -1},
      {36, "arrangement: unknown switch", I, "constant 0", 0},
      {37, "project / preset: unknown switch", I, "no observable effect identified", -1},
      {38, "project: unknown switch (FL 20.5+)", I, "no observable effect identified", -1},
      {39, "project: unknown switch (FL 20.7+)", I, "no observable effect identified", -1},
      {40, "project: unknown switch (FL 20.8+)", I, "constant 0", 0},
      {41, "plugin: unknown switch (FL 21+)", I, "no observable effect identified", -1},
      {42, "mixer insert: unknown switch (FL 21+)", I, "used only to find where inserts start", -1},
      {43, "track: unknown switch (FL 21+)", I, "no observable effect identified", -1},
      {44, "project: unknown switch (FL 21+)", I, "no observable effect identified", -1},
      {45, "marker: unknown (FL 21.2+)", I, "constant 0", 0},
      {46, "marker: unknown (FL 21.2+)", I, "constant 0", 0},
      {47, "project: unknown (FL 21.2+)", I, "constant 0", 0},
      {48, "channel: unknown (FL 24+)", I, "constant 0", 0},
      {49, "mixer insert: unknown (FL 24.2+)", I, "constant 0", 0},
      {50, "channel: unknown (FL 24.2+)", I, "no observable effect identified", -1},
      {51, "channel: unknown (FL 24.2.99+)", I, "constant 0", 0},
      {52, "pattern: unknown (FL 25.2+)", I, "constant 0", 0},
      {64, "channel (opens a channel)", D, "", -1},
      {65, "pattern (opens a pattern)", D, "", -1},
      {66, "tempo (legacy, coarse)", D, "", -1},
      {67, "current pattern", I, "selection state at save time", -1},
      {69, "sampler precomputed effect: frequency tilt", D, "", 128},
      {70, "sampler effect flags (stereo fade, reverse, boost clip, swap stereo)", D, "", 0},
      {71, "sampler precomputed effect: cutoff", D, "", 1024},
      {72, "channel volume (legacy word)", I, "4 files saved by FL 11–20; superseded by 219", -1},
      {74, "sampler precomputed effect: boost", D, "", 0},
      {75, "sampler fade out", D, "", 0},
      {76, "sampler fade in", D, "", 0},
      {80, "master pitch (cents)", D, "", 0},
      {83, "sampler precomputed effect: resonance", D, "", 0},
      {85, "sampler precomputed effect: stereo delay", D, "", 2048},
      {86, "sampler precomputed effect: time warp (pogo)", D, "", 256},
      {89, "channel time shift", D, "", 0},
      {91, "pattern: legacy (FL 11)", I, "1 file", -1},
      {92, "pattern: legacy (FL 11)", I, "1 file", -1},
      {93, "tempo (legacy, fine)", D, "", -1},
      {94, "layer child", D, "", -1},
      {95, "mixer insert icon", I, "FL's icon set is artwork; Gridline shows names and colours", 0},
      {97, "channel swing mix", D, "", 128},
      {98, "mixer slot index", D, "", -1},
      {99, "arrangement (opens an arrangement)", D, "", -1},
      {100, "current arrangement", D, "", -1},
      {101, "marker: unknown (FL 21.2+)", I, "constant 0", 0},
      {103, "mixer insert count (FL 25)", D, "", -1},
      {104, "channel → mixer insert (FL 24.2.99+)", D, "", -1},
      {128, "plugin / channel colour", D, "", -1},
      {131, "sampler precomputed effect: ring modulation", D, "", 8388608},
      {132, "channel cut groups", D, "", -1},
      {133, "channel rack window height", I, "window layout", -1},
      {135, "sampler root note", D, "", -1},
      {138, "channel echo: mod X / Y per echo", P, "decoded; the per-echo modulation law is approximated", 8388736},
      {139, "sampler precomputed effect: reverb", P, "mix and type decoded; Gridline's own reverb tail stands in for the reference algorithm", 65536},
      {140, "preset: unknown (channel presets)", I, "no observable effect identified", -1},
      {142, "channel fine tune (legacy)", D, "", -1},
      {143, "sampler option bits", I, "sampler loading options (keep on disk, resample, …); no Gridline equivalent", -1},
      {144, "layer flags (random, crossfade)", P, "random decoded; crossfade plays every child", 0},
      {145, "channel display group", D, "", -1},
      {146, "channel rack: current display group", I, "view filter at save time", kNone},
      {147, "mixer insert output (closes an insert)", D, "", kNone},
      {148, "marker position (+ action bits)", D, "", -1},
      {149, "mixer insert colour", D, "", -1},
      {150, "pattern colour", D, "", -1},
      {154, "mixer insert audio input", I, "audio inputs are not modelled yet (recording, J-8)", kNone},
      {155, "plugin icon", I, "FL's icon set is artwork", 0},
      {156, "tempo × 1000", D, "", -1},
      {157, "pattern: unknown (FL 12.5+)", I, "no observable effect identified", kNone},
      {158, "pattern: unknown (FL 12.9+)", I, "no observable effect identified", kNone},
      {159, "FL build number", D, "", -1},
      {160, "pattern: performance channel id", I, "7 performance templates; no observable effect identified", -1},
      {161, "pattern: unknown (performance)", I, "no observable effect identified", -1},
      {162, "pattern: unknown", I, "2 files", -1},
      {164, "pattern fixed length (ticks)", D, "", 0},
      {165, "mixer insert: unknown (FL 21+)", I, "no observable effect identified", -1},
      {166, "mixer insert: unknown (FL 21+)", I, "no observable effect identified", -1},
      {167, "project: unknown (FL 21.2+)", I, "no observable effect identified", -1},
      {168, "marker: unknown (FL 21.2+)", I, "constant 0", 0},
      {169, "project / preset: unknown (FL 24.2+)", I, "no observable effect identified", -1},
      {170, "channel: unknown (FL 24.2.99+)", I, "no observable effect identified", -1},
      {171, "pattern: unknown (FL 25.2+)", I, "constant 0", 0},
      {172, "project: unknown (FL 25.2.3+, variable size)", I, "read with its variable size; no observable effect identified", -1},
      {192, "channel name (legacy)", D, "", -1},
      {193, "pattern name", D, "", -1},
      {194, "project title", D, "", -1},
      {195, "project comments", D, "", -1},
      {196, "sample path", D, "", -1},
      {197, "project URL", D, "", -1},
      {198, "project comments (RTF)", D, "", -1},
      {199, "FL version", D, "", -1},
      {200, "licensee", I, "personal data, never imported", -1},
      {201, "plugin internal name", D, "", -1},
      {202, "project data folder", I, "Gridline references samples where they are", -1},
      {203, "plugin / channel display name", D, "", -1},
      {204, "mixer insert name", D, "", -1},
      {205, "marker name", D, "", -1},
      {206, "project genre", D, "", -1},
      {207, "project author", D, "", -1},
      {209, "channel echo delay", D, "", -1},
      {210, "preset: unknown data", I, "2 presets", -1},
      {212, "plugin wrapper (window, flags)", P, "used for third-party plugin hosting; window position ignored", -1},
      {213, "plugin state", D, "", -1},
      {215, "channel parameters", P, "arpeggiator, time stretch, ping-pong echo decoded; other sampler fields ignored", -1},
      {216, "playlist selection", I, "selection state at save time", -1},
      {217, "unknown data", I, "1 file", -1},
      {218, "channel envelope / LFO", P, "sampler channels and native synths' volume envelope; other targets approximated", -1},
      {219, "channel levels (pan, volume, pitch, filter)", D, "", -1},
      {220, "preset: unknown data", I, "channel presets only; no observable effect identified", -1},
      {221, "channel polyphony / porta", D, "", -1},
      {223, "pattern controller events", D, "", -1},
      {224, "pattern notes", D, "", -1},
      {225, "mixer parameters", D, "", -1},
      {226, "MIDI remote-control links", P, "links to a fixed target decoded; links that follow the focused channel / track (the default templates' links) are not imported", -1},
      {227, "automation / controller targets", D, "", -1},
      {228, "channel key tracking", I, "filter / pan key tracking is not modelled", -1},
      {229, "channel level adjustments", D, "", -1},
      {230, "unknown data", I, "25 files; no observable effect identified", -1},
      {231, "channel display group name", D, "", -1},
      {233, "playlist items", D, "", -1},
      {234, "automation points", D, "", -1},
      {235, "mixer insert routing", D, "", -1},
      {236, "mixer insert flags", P, "polarity, swap, mute, solo, effects on decoded; dock side, separator, lock ignored (layout)", -1},
      {237, "project creation date and time spent", D, "", -1},
      {238, "playlist track", D, "", -1},
      {239, "playlist track name", D, "", -1},
      {240, "unknown data", I, "1 file", -1},
      {241, "arrangement name", D, "", -1},
      {242, "track: unknown data (FL 21+)", I, "no observable effect identified", -1},
      {243, "project: unknown data (FL 21+)", I, "no observable effect identified", -1},
      {251, "channel: unknown data (FL 25.2+)", I, "2 files", -1},
      {252, "pattern: unknown data (FL 25.2+)", I, "constant", -1},
      {253, "pattern: unknown data (FL 25.2+)", I, "no observable effect identified", -1},
  };
  return cat;
}

const EventInfo* eventInfo(uint8_t id) {
  const auto& cat = eventCatalog();
  auto it = std::lower_bound(cat.begin(), cat.end(), id, [](const EventInfo& e, uint8_t v) { return e.id < v; });
  return it != cat.end() && it->id == id ? &*it : nullptr;
}

const char* statusName(EventInfo::Status s) {
  switch (s) {
    case EventInfo::Decoded: return "decoded";
    case EventInfo::Partial: return "partial";
    default: return "ignored";
  }
}

namespace {

void tallyPlugin(Context& c, const std::vector<Event>& evs, std::map<std::string, PluginTally>& out) {
  const Event* nm = find(evs, 201);
  if (!nm) return;
  std::string name = nm->text();
  if (name.empty()) return;  // samplers / audio clips
  PluginTally& t = out[name];
  ++t.uses;
  PluginMapping m;
  m.flName = name;
  const PluginDecoder* dec = findDecoder(name);
  if (!dec || !(*dec)(c, evs, m) || m.glType.empty()) return;
  ++t.mapped;
  t.exact += m.exact;
  t.types[m.glType]++;
}

}  // namespace

void tallyFile(const std::string& path, FileKind kind, CoverageTally& t, const std::string& factoryData) {
  const bool preset = kind != FileKind::Project;
  File f;
  std::string err;
  if (!readFile(path, f, &err)) {
    ++t.unreadable;
    return;
  }
  (kind == FileKind::Project ? t.projects : kind == FileKind::Preset ? t.presets : t.database)++;
  std::map<int, std::array<int, 2>> seen;  // id → count, non-default
  for (const Event& e : f.events) {
    auto& s = seen[e.id];
    ++s[0];
    if (e.id < 192)
      if (const EventInfo* inf = eventInfo(e.id); inf && inf->dflt >= 0 && static_cast<int64_t>(e.value()) != inf->dflt) ++s[1];
  }
  for (const auto& [id, s] : seen) {
    auto& ec = t.events[id];
    if (preset) ++ec.presetFiles, ec.presetCount += s[0], ec.presetNonDefault += s[1];
    else ++ec.projectFiles, ec.projectCount += s[0], ec.projectNonDefault += s[1];
  }
  Sections sec = split(f);
  Song song;
  Report rep;
  Context c(f, song, rep);
  c.factoryData = factoryData.empty() ? defaultFactoryData() : factoryData;
  c.projectDir = std::filesystem::path(path).parent_path().string();
  auto& plugins = kind == FileKind::Project ? t.projectPlugins : kind == FileKind::Preset ? t.presetPlugins : t.databasePlugins;
  for (const auto& [k, evs] : sec.channels) tallyPlugin(c, evs, plugins);
  for (const auto& ins : sec.inserts)
    for (const auto& sl : ins.slots) tallyPlugin(c, sl.events, plugins);
  if (sec.channels.empty() && sec.inserts.empty()) tallyPlugin(c, f.events, plugins);  // plugin presets
  if (kind != FileKind::Preset) return;
  // The preset file as the preset library converts it.
  PresetResult r;
  bool ok = importFlPreset(path, r, factoryData);
  std::string key = r.kind == PresetResult::Chain ? "(effect chain)" : r.kind == PresetResult::Shape ? "(automation shape)" : r.flPlugin;
  if (!ok && !r.unmapped.empty()) key = r.unmapped.front();
  if (key.empty()) key = "(" + (r.error.empty() ? std::string("unknown") : r.error) + ")";
  PluginTally& pt = t.presetFiles[key];
  ++pt.uses;
  if (ok) {
    ++pt.mapped;
    pt.exact += r.exact;
    if (r.kind == PresetResult::Plugin) pt.types[r.state.type]++;
  }
}

CoverageFiles coverageFiles(const std::string& flRoot) {
  namespace fs = std::filesystem;
  CoverageFiles out;
  std::error_code ec;
  auto walk = [&](const fs::path& root, const char* ext, std::vector<std::string>& v, bool skipSpecial) {
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::string e = it->path().extension().string();
      std::transform(e.begin(), e.end(), e.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (e != ext) continue;
      if (skipSpecial) {  // as importPresetLibrary: these plugin folders hold no convertible presets
        const std::string p = it->path().string();
        if (p.find("/ZGameEditor Visualizer/") != std::string::npos || p.find("/FL Studio Mobile/") != std::string::npos ||
            p.find("/FL Studio Mobile Rack FX/") != std::string::npos)
          continue;
      }
      v.push_back(it->path().string());
    }
  };
  const fs::path data = fs::path(flRoot) / "Data";
  walk(data, ".flp", out.projects, false);
  const fs::path patches = data / "Patches";
  for (const char* sub : {"Plugin presets", "Channel presets", "Packs", "Mixer presets"}) walk(patches / sub, ".fst", out.presets, false);
  walk(fs::path(flRoot) / "Plugins" / "Fruity", ".fst", out.presets, true);
  walk(patches / "Plugin database", ".fst", out.database, false);
  for (auto* v : {&out.projects, &out.presets, &out.database}) std::sort(v->begin(), v->end());
  return out;
}

std::string defaultMapping(const std::string& flName, bool* exact) {
  if (exact) *exact = false;
  const PluginDecoder* dec = findDecoder(flName);
  if (!dec) return "";
  // A plugin event list with just the name (FL writes this for a fresh,
  // never-edited instance of some plugins).
  std::vector<uint8_t> bytes;
  auto put = [&](const std::vector<uint8_t>& v) { bytes.insert(bytes.end(), v.begin(), v.end()); };
  std::vector<uint8_t> nameEv = {201};
  std::vector<uint8_t> txt;
  for (char ch : flName) txt.push_back(static_cast<uint8_t>(ch)), txt.push_back(0);
  txt.push_back(0), txt.push_back(0);
  nameEv.push_back(static_cast<uint8_t>(txt.size()));  // names are short (< 128 bytes)
  put(nameEv);
  put(txt);
  std::vector<uint8_t> file = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 0, 0, 96, 0, 'F', 'L', 'd', 't'};
  uint32_t n = static_cast<uint32_t>(bytes.size());
  for (int k = 0; k < 4; ++k) file.push_back(static_cast<uint8_t>(n >> (8 * k)));
  file.insert(file.end(), bytes.begin(), bytes.end());
  File f;
  if (!readBytes(file, f, nullptr)) return "";
  Song song;
  Report rep;
  Context c(f, song, rep);
  PluginMapping m;
  m.flName = flName;
  if (!(*dec)(c, f.events, m)) return "";
  if (exact) *exact = m.exact;
  return m.glType;
}

std::string coverageJson(const CoverageTally& t) {
  auto num = [](double v) { return Json::makeNumber(v); };
  auto boolean = [](bool v) {
    Json j;
    j.type = Json::Type::Bool;
    j.boolean = v;
    return j;
  };
  auto str = [](const std::string& v) { return Json::makeString(v); };
  auto tallies = [&](const std::map<std::string, PluginTally>& m) {
    Json o = Json::makeObject();
    for (const auto& [name, pt] : m) {
      Json j = Json::makeObject();
      j.object["uses"] = num(pt.uses);
      j.object["mapped"] = num(pt.mapped);
      j.object["exact"] = num(pt.exact);
      Json ty = Json::makeObject();
      for (const auto& [k, v] : pt.types) ty.object[k] = num(v);
      j.object["types"] = std::move(ty);
      o.object[name] = std::move(j);
    }
    return o;
  };
  Json root = Json::makeObject();
  root.object["projects"] = num(t.projects);
  root.object["presets"] = num(t.presets);
  root.object["unreadable"] = num(t.unreadable);
  Json cat = Json::makeArray();
  for (const PluginInfo& p : pluginCatalog()) {
    Json j = Json::makeObject();
    j.object["type"] = str(p.type);
    j.object["name"] = str(p.name);
    j.object["category"] = str(p.category);
    j.object["kind"] = str(p.kind == PluginKind::Generator ? "generator" : p.kind == PluginKind::Effect ? "effect" : "notefx");
    j.object["params"] = num(static_cast<double>(p.params.size()));
    j.object["presets"] = num(static_cast<double>(p.presets.size()));
    cat.array.push_back(std::move(j));
  }
  root.object["catalog"] = std::move(cat);
  Json decs = Json::makeObject();
  for (const auto& [name, dec] : decoders()) {
    (void)dec;
    bool exact = false;
    Json j = Json::makeObject();
    j.object["default"] = str(defaultMapping(name, &exact));
    j.object["defaultExact"] = boolean(exact);
    decs.object[name] = std::move(j);
  }
  root.object["decoders"] = std::move(decs);
  root.object["projectPlugins"] = tallies(t.projectPlugins);
  root.object["presetPlugins"] = tallies(t.presetPlugins);
  root.object["presetFiles"] = tallies(t.presetFiles);
  root.object["databasePlugins"] = tallies(t.databasePlugins);
  root.object["database"] = num(t.database);
  Json evs = Json::makeObject();
  std::map<int, CoverageTally::EventCount> all = t.events;
  for (const EventInfo& e : eventCatalog()) all[e.id];
  for (const auto& [id, ec] : all) {
    Json j = Json::makeObject();
    const EventInfo* inf = eventInfo(static_cast<uint8_t>(id));
    j.object["name"] = str(inf ? inf->name : "");
    j.object["status"] = str(inf ? statusName(inf->status) : "unknown");
    j.object["note"] = str(inf ? inf->note : "not in the event catalog");
    j.object["projectFiles"] = num(ec.projectFiles);
    j.object["projectCount"] = num(ec.projectCount);
    j.object["projectNonDefault"] = num(ec.projectNonDefault);
    j.object["presetFiles"] = num(ec.presetFiles);
    j.object["presetCount"] = num(ec.presetCount);
    j.object["presetNonDefault"] = num(ec.presetNonDefault);
    j.object["hasDefault"] = boolean(inf && inf->dflt >= 0);
    evs.object[std::to_string(id)] = std::move(j);
  }
  root.object["events"] = std::move(evs);
  return jsonStringify(root, 1);
}

}  // namespace gl::flp
