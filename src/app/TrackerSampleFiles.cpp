// Gridline - K-435: a Tracker Sampler's sounds out of the project. See TrackerSampleFiles.h.
#include "TrackerSampleFiles.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/Presets.h"
#include "../packs/tracker/tracker.h"
#include "App.h"
#include "ContextMenus.h"
#include "PluginUI.h"
#include "TrackerEditors.h"
#include "Widgets.h"

namespace gl::trackerfiles {

namespace fs = std::filesystem;
namespace tp = gl::packs::trackerpack;
namespace trk = gl::tracker;

namespace {

const Channel* trackerChannel(App& a, ObjectId id) {
  const Channel* c = id == kNoId ? nullptr : a.doc.song().channel(id);
  return c && c->plugin.type == trk::kInstrumentType ? c : nullptr;
}

std::string root(App& a) { return presetRoot(a.configDir()); }

std::string twoDigits(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

// A menu row whose place a dev script can find by its English label (TrackerEditors.h devMenuRow).
bool row(const char* label, bool enabled = true) {
  const bool hit = ImGui::MenuItem(i18n::tr(label), nullptr, false, enabled);
  const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
  trackereditors::devMenuRow(label, r0.x, r0.y, r1.x, r1.y);
  return hit;
}

}  // namespace

std::string songPresetFolder(App& a) {
  std::string t = safePresetName(a.doc.song().info.title);
  return t.empty() ? std::string("Tracker instruments") : t;
}

bool savePreset(App& a, ObjectId channel, const std::string& nameIn) {
  const Channel* c = trackerChannel(a, channel);
  std::string name = safePresetName(nameIn);
  if (!c || name.empty()) return false;
  const bool ok = gl::savePreset(root(a), c->plugin, name);
  a.notify(ok ? std::string(i18n::tr("Saved the preset ")) + name + " " + i18n::tr("(with its samples)") : std::string(i18n::tr("Could not save the preset")), !ok);
  return ok;
}

int saveAllPresets(App& a) {
  const Song& s = a.doc.song();
  const std::string folder = songPresetFolder(a);
  int n = 0, tried = 0;
  for (size_t i = 0; i < s.channels.size(); ++i) {
    const Channel& c = s.channels[i];
    if (c.plugin.type != trk::kInstrumentType) continue;
    ++tried;
    const std::string nm = safePresetName(c.name);
    n += gl::savePreset(root(a), c.plugin, folder + "/" + twoDigits(static_cast<int>(i) + 1) + (nm.empty() ? std::string() : " " + nm)) ? 1 : 0;
  }
  if (tried == 0) a.notify(i18n::tr("The song has no Tracker Sampler instruments"));
  else if (n == tried) a.notify(std::string(i18n::tr("Saved every instrument as a preset in the folder ")) + folder);
  else a.notify(std::string(i18n::tr("Some instruments could not be saved as presets in ")) + folder, true);
  return n;
}

bool loadPreset(App& a, ObjectId channel, const std::string& file) {
  const Channel* c = trackerChannel(a, channel);
  PluginState ns;
  if (!c || !gl::loadPreset(file, trk::kInstrumentType, ns)) {
    a.notify(i18n::tr("That file is not a Tracker Sampler preset"), true);
    return false;
  }
  if (ns.data.empty()) ns.data = c->plugin.data;  // (a preset with no samples of its own keeps the current ones)
  setPluginState(a, PluginTarget{channel, -1}, ns, "load preset");
  pui::presetLoaded(PluginTarget{channel, -1}, trk::kInstrumentType, fs::u8path(file).stem().u8string());
  return true;
}

int exportSamples(App& a, ObjectId channel, const std::string& path) {
  const Channel* c = trackerChannel(a, channel);
  if (!c) return 0;
  std::vector<std::string> out;
  const int n = tp::exportSampleFiles(c->plugin, path, &out);
  if (n == 0) a.notify(i18n::tr("No sample could be written (its file is missing, or the instrument has none)"), true);
  else if (n == 1) a.notify(std::string(i18n::tr("Saved the sample as ")) + fs::u8path(out[0]).filename().u8string());
  else a.notify(std::to_string(n) + " " + i18n::tr("samples saved in") + " " + fs::u8path(out[0]).parent_path().filename().u8string());
  return n;
}

void menuItems(App& a, ObjectId channel) {
  const Channel* c = trackerChannel(a, channel);
  bool anyTracker = false;
  for (const Channel& ch : a.doc.song().channels) anyTracker = anyTracker || ch.plugin.type == trk::kInstrumentType;
  if (!c && !anyTracker) return;
  ImGui::Separator();
  if (c) {
    const std::vector<PresetEntry> lib = listPresets(root(a), trk::kInstrumentType);
    if (ImGui::BeginMenu(i18n::tr("Load preset"), !lib.empty())) {
      std::string folder = "\x01";
      for (const PresetEntry& e : lib) {
        if (e.folder != folder) {
          folder = e.folder;
          if (!folder.empty()) ImGui::SeparatorText(folder.c_str());
        }
        if (ImGui::MenuItem(e.name.c_str())) {
          const std::string f = e.file;
          a.later([&a, channel, f] { loadPreset(a, channel, f); });
        }
      }
      ImGui::EndMenu();
    }
    if (lib.empty()) ui::Hint("Saved Tracker Sampler presets show here");
    if (row("Save as preset...")) {
      const ObjectId id = channel;
      menus::openPrompt(i18n::tr("Save the instrument as a preset"), c->name.empty() ? std::string("Instrument") : c->name,
                        [&a, id](const std::string& n) { savePreset(a, id, n); });
    }
    ui::Hint("The instrument with its samples, envelopes and key map in your presets (its samples are copied with it)");
    if (row("Export samples as WAV...")) {
      const ObjectId id = channel;
      menus::fileDialog(a, i18n::tr("Export the samples"), true, {".wav"}, safePresetName(c->name.empty() ? std::string("Instrument") : c->name) + ".wav",
                        [&a, id](const std::string& p) { exportSamples(a, id, fs::u8path(p).extension().empty() ? p + ".wav" : p); });
    }
    ui::Hint("Each sample as a WAV file (several samples: one file each, numbered after the name you give)");
  }
  if (row("Save every instrument as a preset", anyTracker)) a.later([&a] { saveAllPresets(a); });
  ui::Hint("Every Tracker Sampler of the song in your presets, in a folder named after the song");
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string sub = args.size() > 2 ? args[2] : "dump";
  const Song& s = a.doc.song();
  auto chanAt = [&](size_t i) {
    const long idx = args.size() > i ? std::atol(args[i].c_str()) : -1;
    return idx >= 0 && idx < static_cast<long>(s.channels.size()) ? s.channels[static_cast<size_t>(idx)].id : a.selChannel;
  };
  auto rest = [&](size_t from) {
    std::string t;
    for (size_t i = from; i < args.size(); ++i) t += (t.empty() ? "" : " ") + args[i];
    return t;
  };
  if (sub == "save") {
    std::printf("tracker files frame %d: save %s\n", frame, savePreset(a, chanAt(3), rest(4)) ? "ok" : "failed");
  } else if (sub == "all") {
    std::printf("tracker files frame %d: all %d\n", frame, saveAllPresets(a));
  } else if (sub == "load") {
    const std::string name = rest(4);
    std::string file;
    for (const PresetEntry& e : listPresets(root(a), trk::kInstrumentType))
      if (e.name == name || (e.folder.empty() ? e.name : e.folder + "/" + e.name) == name) file = e.file;
    std::printf("tracker files frame %d: load %s\n", frame, !file.empty() && loadPreset(a, chanAt(3), file) ? "ok" : "failed");
  } else if (sub == "export") {
    std::printf("tracker files frame %d: export %d\n", frame, exportSamples(a, chanAt(3), rest(4)));
  } else {
    std::printf("tracker files frame %d: dump\n", frame);
    for (size_t i = 0; i < s.channels.size(); ++i) {
      const Channel& c = s.channels[i];
      if (c.plugin.type != trk::kInstrumentType) continue;
      const auto inst = tp::instrumentFor(c.plugin);
      int play = 0;
      if (inst)
        for (const auto& f : inst->files) play += f && f->frames() > 0 ? 1 : 0;
      const tp::Edit e = tp::readEdit(c.plugin.data, 0);
      std::printf("  channel %zu \"%s\" playable %d files", i, c.name.c_str(), play);
      for (const auto& f : e.text.files) std::printf(" |%s", f.c_str());
      std::printf("\n");
    }
  }
  std::fflush(stdout);
}

}  // namespace gl::trackerfiles
