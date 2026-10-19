// Gridline — FL Studio preset import (.fst): converts the presets of the
// user's own FL Studio installation into Gridline presets on their machine,
// using the same decoders as project import. Nothing is bundled with Gridline.
//
//   plugin presets   (Plugin presets/…, plugin folders)  one plugin's events
//   channel presets  (Channel presets/…, Packs/…)         a whole channel (64 …)
//   mixer presets    (Mixer presets/…)                     an effect chain
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include "../Presets.h"
#include "FlpPresets.h"

namespace gl::flp {

namespace fs = std::filesystem;

namespace {

bool hasEvent(const File& f, uint8_t id) {
  for (const auto& e : f.events)
    if (e.id == id) return true;
  return false;
}

// Mixer presets (an insert's effect chain): plugin event runs, each starting
// at 201, plus 225 records for slot mix / enable.
bool importChain(const File& f, Context& c, PresetResult& out) {
  std::vector<std::vector<Event>> slots;
  for (const auto& e : f.events) {
    if (e.id == 201) slots.emplace_back();
    if (!slots.empty() && (e.id == 201 || e.id == 212 || e.id == 213 || e.id == 203 || e.id == 155 || e.id == 128))
      slots.back().push_back(e);
  }
  out.kind = PresetResult::Chain;
  if (slots.empty()) {  // an empty mixer preset: loading it clears the effect slots
    out.chain.slots.assign(10, PluginState{});
    out.chain.mix.assign(10, 1.0f);
    out.exact = true;
    return true;
  }
  out.chain.slots.assign(std::min<size_t>(slots.size(), 10), PluginState{});
  out.chain.mix.assign(out.chain.slots.size(), 1.0f);
  bool any = false, exact = true;
  for (size_t k = 0; k < out.chain.slots.size(); ++k) {
    PluginMapping m;
    const Event* nm = find(slots[k], 201);
    m.flName = nm ? nm->text() : "";
    const PluginDecoder* dec = findDecoder(m.flName);
    if (m.flName.empty() || !dec || !(*dec)(c, slots[k], m) || m.glType.empty()) {
      out.unmapped.push_back(m.flName.empty() ? "(empty)" : m.flName);
      continue;
    }
    if (m.state.type.empty()) m.state.type = m.glType;
    normalizePluginState(m.state);
    out.chain.slots[k] = m.state;
    exact = exact && m.exact;
    any = true;
  }
  // 225 records: u32 0, u8 id, u8 kind, u16 where (… << 6 | slot), i32 value.
  for (const auto& e : f.events) {
    if (e.id != 225) continue;
    for (uint32_t o = 0; o + 12 <= e.size; o += 12) {
      uint8_t id = e.u8(o + 4);
      size_t slot = e.u16(o + 6) & 0x3F;
      int32_t v = e.i32(o + 8);
      if (slot >= out.chain.mix.size()) continue;
      if (id == 1) out.chain.mix[slot] = std::clamp(static_cast<float>(v) / 12800.0f, 0.0f, 1.0f);
    }
  }
  out.exact = exact;
  return any;
}

// Channel presets: the whole channel goes through the project importer so
// channel-level settings (envelopes, filter, levels) land in the instrument
// exactly as they do for projects.
bool importChannelPreset(const File& f, Context& c, PresetResult& out) {
  c.sec = split(f);
  if (c.sec.channels.empty()) return false;
  ensureMixer(c.song, 0);
  c.insertIds[0] = c.song.masterId();
  importChannels(c);
  importPlugins(c);
  finishChannels(c);
  if (c.song.channels.empty()) return false;
  const int idx = c.sec.channels.begin()->first;
  out.kind = PresetResult::Plugin;
  out.state = c.song.channels.front().plugin;
  auto pm = c.channelPlugins.find(idx);
  if (pm != c.channelPlugins.end()) {
    out.flPlugin = pm->second.flName;
    out.exact = pm->second.exact;
  } else {
    const Event* nm = nullptr;
    for (const auto& e : c.sec.channels.begin()->second)
      if (e.id == 201) nm = &e;
    out.flPlugin = nm && !nm->text().empty() ? nm->text() : "Sampler";
    out.exact = out.state.type == "gl.sampler";
    if (nm && !nm->text().empty() && out.state.type == "gl.synth") {  // placeholder: no decoder
      out.unmapped.push_back(nm->text());
      return false;
    }
  }
  return !out.state.type.empty();
}

// Automation-clip channel presets (channel type 5): the clip's points become
// an automation shape. The target link is not part of the preset.
bool isAutomationPreset(const File& f) {
  for (const auto& e : f.events)
    if (e.id == 21 && e.value() == 5) return hasEvent(f, 234);
  return false;
}

bool importShapePreset(const File& f, PresetResult& out) {
  for (const auto& e : f.events)
    if (e.id == 234) {
      out.kind = PresetResult::Shape;
      out.flPlugin = "Automation clip";
      out.shape = AutomationShape{};
      out.shape.source = "FL Studio automation clip preset";
      if (!decodeAutomationPoints(e, kShapePpq, out.shape.points)) return false;
      out.exact = true;
      return true;
    }
  return false;
}

// Plugin presets: a single plugin's events.
bool importPluginPreset(const File& f, Context& c, PresetResult& out) {
  const Event* nm = nullptr;
  for (const auto& e : f.events)
    if (e.id == 201) {
      nm = &e;
      break;
    }
  if (!nm) return false;
  PluginMapping m;
  m.flName = nm->text();
  out.flPlugin = m.flName;
  const PluginDecoder* dec = findDecoder(m.flName);
  if (!dec || !(*dec)(c, f.events, m) || m.glType.empty()) {
    out.unmapped.push_back(m.flName);
    return false;
  }
  if (m.state.type.empty()) m.state.type = m.glType;
  normalizePluginState(m.state);
  out.kind = PresetResult::Plugin;
  out.state = m.state;
  out.exact = m.exact;
  return true;
}

}  // namespace

// Native preset files that hold a plugin's state payload alone: .gmsynth =
// groove-machine synth, .mrp = spectral morpher (J-22).
static bool importNativePreset(const std::string& path, PresetResult& out, const std::string& factoryData, const char* flName);
bool importGmsPreset(const std::string& path, PresetResult& out, const std::string& factoryData) {
  return importNativePreset(path, out, factoryData, "GMS");
}
static bool importNativePreset(const std::string& path, PresetResult& out, const std::string& factoryData, const char* flName) {
  std::vector<uint8_t> bytes;
  {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
      out.error = "cannot open file";
      return false;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0 && bytes.size() < (8u << 20)) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(fp);
  }
  File f;
  Song song;
  Report rep;
  Context c(f, song, rep);
  c.factoryData = factoryData.empty() ? defaultFactoryData() : factoryData;
  Event e;
  e.id = 213;
  e.data = bytes.data();
  e.size = static_cast<uint32_t>(bytes.size());
  PluginMapping m;
  m.flName = flName;
  out.flPlugin = flName;
  const PluginDecoder* dec = findDecoder(flName);
  if (!dec || !(*dec)(c, {e}, m) || m.glType.empty()) {
    out.error = std::string("not a ") + flName + " preset";
    return false;
  }
  if (m.state.type.empty()) m.state.type = m.glType;
  normalizePluginState(m.state);
  out.kind = PresetResult::Plugin;
  out.state = m.state;
  out.exact = m.exact;
  for (const auto& w : rep.warnings) out.notes.push_back(w);
  return true;
}

bool importFlPreset(const std::string& path, PresetResult& out, const std::string& factoryData) {
  out = PresetResult{};
  {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext == ".gmsynth") return importGmsPreset(path, out, factoryData);
    if (ext == ".mrp") return importNativePreset(path, out, factoryData, "Morphine");
  }
  File f;
  if (!readFile(path, f, &out.error)) return false;
  Song song;
  Report rep;
  Context c(f, song, rep);
  c.factoryData = factoryData.empty() ? defaultFactoryData() : factoryData;
  c.projectDir = fs::path(path).parent_path().string();
  bool ok;
  if (isAutomationPreset(f)) ok = importShapePreset(f, out);
  else if (hasEvent(f, 64)) ok = importChannelPreset(f, c, out);
  else if (hasEvent(f, 236) || hasEvent(f, 225)) ok = importChain(f, c, out);
  else ok = importPluginPreset(f, c, out);
  for (const auto& w : rep.warnings) out.notes.push_back(w);
  if (!ok && out.error.empty()) out.error = out.unmapped.empty() ? "nothing to import" : "no Gridline equivalent for " + out.unmapped.front();
  return ok;
}

namespace {

std::string joinSafe(const fs::path& rel) {
  std::string s;
  for (const auto& part : rel) {
    if (!s.empty()) s += "/";
    s += safePresetName(part.string());
  }
  return s;
}

}  // namespace

PresetLibraryReport importPresetLibrary(const std::string& factoryData, const std::string& presetRootDir,
                                        const std::function<bool(size_t done, size_t total, const std::string& file)>& progress) {
  PresetLibraryReport rep;
  const std::string fl = factoryData.empty() ? defaultFactoryData() : factoryData;
  if (fl.empty()) {
    rep.error = "FL Studio data folder not found";
    return rep;
  }
  struct Source {
    fs::path file;
    std::string folder;  // destination folder below "FL Studio/"
    bool chain = false;
  };
  std::vector<Source> files;
  std::error_code ec;
  auto walk = [&](const fs::path& root, const std::string& prefix, bool chain, int stripDirs) {
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::string ext = it->path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (ext != ".fst" && ext != ".gmsynth" && ext != ".mrp") continue;
      fs::path rel = fs::relative(it->path().parent_path(), root, ec);
      fs::path kept;
      int i = 0;
      for (const auto& part : rel)
        if (i++ >= stripDirs && part != ".") kept /= part;
      std::string folder = prefix;
      std::string sub = joinSafe(kept);
      if (!sub.empty()) folder += (folder.empty() ? "" : "/") + sub;
      files.push_back({it->path(), folder, chain});
    }
  };
  const fs::path data = fs::path(fl) / "Data" / "Patches";
  walk(data / "Plugin presets" / "Generators", "", false, 0);  // <Plugin>/<sub…>
  walk(data / "Plugin presets" / "Effects", "", false, 0);
  walk(data / "Channel presets", "Channel presets", false, 0);
  walk(data / "Packs", "Packs", false, 0);
  walk(data / "Mixer presets", "", true, 0);
  for (const char* kind : {"Effects", "Generators"}) {
    fs::path pr = fs::path(fl) / "Plugins" / "Fruity" / kind;
    if (!fs::is_directory(pr, ec)) continue;
    for (fs::directory_iterator it(pr, ec), end; !ec && it != end; it.increment(ec)) {
      std::string plugin = it->path().filename().string();
      if (plugin == "ZGameEditor Visualizer" || plugin == "FL Studio Mobile" || plugin == "FL Studio Mobile Rack FX") continue;
      walk(it->path(), safePresetName(plugin), false, 0);
    }
  }
  rep.total = files.size();
  const std::string base = "FL Studio";
  for (size_t i = 0; i < files.size(); ++i) {
    if (progress && !progress(i, files.size(), files[i].file.string())) {
      rep.cancelled = true;
      break;
    }
    PresetResult r;
    std::string name = safePresetName(files[i].file.stem().string());
    if (!importFlPreset(files[i].file.string(), r, fl)) {
      ++rep.failed;
      for (const auto& u : r.unmapped) rep.unmappedPlugins[u]++;
      if (r.unmapped.empty()) rep.errors[r.error]++;
      continue;
    }
    if (r.kind == PresetResult::Shape) {
      std::string folder = files[i].folder;
      const std::string strip = "Channel presets/Automation clips";
      if (folder.compare(0, strip.size(), strip) == 0) folder = folder.substr(std::min(folder.size(), strip.size() + 1));
      if (saveAutomationShape(presetRootDir, r.shape, kShapePpq, base + "/" + (folder.empty() ? "" : folder + "/") + name)) {
        ++rep.shapes;
      } else {
        ++rep.failed;
        rep.errors["could not write shape file"]++;
      }
      continue;
    }
    if (r.kind == PresetResult::Chain) {
      r.chain.source = "FL Studio mixer preset";
      if (saveChain(presetRootDir, r.chain, base + "/" + (files[i].folder.empty() ? "" : files[i].folder + "/") + name)) ++rep.chains;
      continue;
    }
    // Plugin presets: under the Gridline type, in "FL Studio/<folder>". Plugin
    // presets' first folder is already the FL plugin name.
    std::string folder = base + (files[i].folder.empty() ? "" : "/" + files[i].folder);
    if (savePreset(presetRootDir, r.state, folder + "/" + name, "FL Studio: " + r.flPlugin, r.exact)) {
      ++rep.converted;
      if (r.exact) ++rep.exact;
      rep.perPlugin[r.flPlugin]++;
    } else {
      ++rep.failed;
      rep.errors["could not write preset file"]++;
    }
  }
  if (progress) progress(files.size(), files.size(), "");
  return rep;
}

}  // namespace gl::flp
