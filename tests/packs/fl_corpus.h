// Gridline — test helper: decodes every state of one FL plugin found in FL
// Studio's own shipped content (plugin presets + the projects / templates /
// render tests under Data), when FL Studio is installed. Nothing is bundled.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpPresets.h"

namespace gl::testfl {

inline const std::string& flData() {
  static const std::string d = "/Applications/FL Studio 2026.app/Contents/Resources/FL";
  return d;
}
inline bool flInstalled() {
  std::error_code ec;
  return std::filesystem::is_directory(flData() + "/Data", ec);
}

struct Decoded {
  std::string where;    // file (and channel position for projects)
  bool ok = false;      // decoder accepted the state
  PluginState state;    // decoded Gridline state
  bool exact = false;
  std::vector<std::string> notes;  // importer notes / warnings for this state
};

// Every shipped preset file of `plugin` (extensions: .fst plus `extra`).
inline std::vector<std::string> presetFiles(const std::string& plugin, const std::vector<std::string>& extra = {}) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path dir = fs::path(flData()) / "Data/Patches/Plugin presets/Generators" / plugin;
  if (!fs::is_directory(dir, ec)) return out;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = it->path().extension().string();
    bool want = ext == ".fst";
    for (const auto& e : extra) want = want || ext == e;
    if (want) out.push_back(it->path().string());
  }
  return out;
}

// Decodes each shipped preset of `plugin` through the preset importer.
inline std::vector<Decoded> decodePresets(const std::string& plugin, const std::vector<std::string>& extra = {}) {
  std::vector<Decoded> v;
  for (const auto& f : presetFiles(plugin, extra)) {
    Decoded d;
    d.where = std::filesystem::path(f).filename().string();
    flp::PresetResult r;
    d.ok = flp::importFlPreset(f, r, flData()) && r.kind == flp::PresetResult::Plugin;
    d.state = r.state;
    d.exact = r.exact;
    d.notes = r.notes;
    v.push_back(std::move(d));
  }
  return v;
}

// Decodes every `plugin` state in the shipped projects (.flp under Data).
inline std::vector<Decoded> decodeProjects(const std::string& plugin) {
  namespace fs = std::filesystem;
  std::vector<Decoded> v;
  std::error_code ec;
  const fs::path root = fs::path(flData()) / "Data";
  if (!fs::is_directory(root, ec)) return v;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".flp") continue;
    flp::File file;
    if (!flp::readFile(it->path().string(), file, nullptr)) continue;
    // Plugin event runs: from the name (201) to the state (213).
    for (size_t i = 0; i < file.events.size(); ++i) {
      const flp::Event& e = file.events[i];
      if (e.id != 201 || e.text() != plugin) continue;
      std::vector<flp::Event> run;
      for (size_t j = i; j < file.events.size(); ++j) {
        const flp::Event& x = file.events[j];
        if (j > i && (x.id == 201 || x.id == 64)) break;
        run.push_back(x);
        if (x.id == 213) break;
      }
      bool hasState = false;
      for (const auto& x : run) hasState = hasState || x.id == 213;
      if (!hasState) continue;
      Song song;
      flp::Report rep;
      flp::Context c(file, song, rep);
      c.factoryData = flData();
      c.projectDir = it->path().parent_path().string();
      flp::PluginMapping m;
      m.flName = plugin;
      Decoded d;
      d.where = it->path().filename().string() + " @" + std::to_string(e.offset);
      const flp::PluginDecoder* dec = flp::findDecoder(plugin);
      d.ok = dec && (*dec)(c, run, m) && !m.glType.empty();
      if (d.ok) {
        d.state = m.state;
        if (d.state.type.empty()) d.state.type = m.glType;
        normalizePluginState(d.state);
        d.exact = m.exact;
      }
      d.notes = rep.warnings;
      v.push_back(std::move(d));
    }
  }
  return v;
}

// Debug: GL_J4_DUMP=<file name> prints that decoded state's parameters.
inline void maybeDump(const Decoded& d) {
  const char* e = std::getenv("GL_J4_DUMP");
  if (!e || d.where.find(e) == std::string::npos) return;
  const PluginInfo* info = findPlugin(d.state.type);
  for (size_t i = 0; i < d.state.params.size(); ++i)
    if (info && i < info->params.size() && d.state.params[i] != static_cast<float>(info->params[i].def))
      std::printf("#   %s = %g\n", info->params[i].name.c_str(), static_cast<double>(d.state.params[i]));
  size_t at = 0;
  for (int line = 0; line < 80 && at < d.state.data.size(); ++line) {
    size_t nl = d.state.data.find('\n', at);
    if (nl == std::string::npos) nl = d.state.data.size();
    std::printf("#   | %.160s\n", d.state.data.substr(at, nl - at).c_str());
    at = nl + 1;
  }
}

}  // namespace gl::testfl
