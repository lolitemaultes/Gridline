// Gridline: the silence tools' rows (K-253). See SilenceUi.h.
#include "SilenceUi.h"
#include <set>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "Widgets.h"

namespace gl::silenceui {

silence::Options options(const App& a) {
  silence::Options o;
  const Settings& s = a.settings;
  o.autoThreshold = s.getBool("silence.autoThreshold");
  o.thresholdDb = s.getNumber("silence.thresholdDb");
  o.minGapMs = s.getNumber("silence.minGapMs");
  o.marginMs = s.getNumber("silence.marginMs");
  o.fadeMs = s.getNumber("silence.fadeMs");
  o.keepBreaths = s.getBool("silence.keepBreaths");
  o.ripple = s.getBool("silence.ripple");
  return o;
}

void settingsRows(App& a, bool ripple) {
  Settings& s = a.settings;
  bool autoT = s.getBool("silence.autoThreshold");
  if (ImGui::MenuItem(i18n::tr("Automatic threshold"), nullptr, autoT)) s.set("silence.autoThreshold", !autoT);
  ui::Hint("The threshold sits just above the audio's own noise floor");
  auto slider = [&](const char* key, const char* label, float lo, float hi, const char* fmt, bool enabled) {
    float v = static_cast<float>(s.getNumber(key));
    ImGui::BeginDisabled(!enabled);
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) s.set(key, static_cast<double>(v));
    ImGui::SameLine();
    ImGui::TextUnformatted(i18n::tr(label));
    ImGui::EndDisabled();
  };
  slider("silence.thresholdDb", "Threshold", -90.0f, -20.0f, "%.0f dB", !autoT);
  ui::Hint("Quieter than this counts as silence");
  slider("silence.minGapMs", "Shortest gap", 50.0f, 2000.0f, "%.0f ms", true);
  ui::Hint("Pauses shorter than this stay (they belong to the phrasing)");
  slider("silence.marginMs", "Margin", 0.0f, 250.0f, "%.0f ms", true);
  ui::Hint("Audio kept before and after every part with sound");
  slider("silence.fadeMs", "Fades", 0.0f, 50.0f, "%.0f ms", true);
  ui::Hint("A short fade at every cut, so no cut clicks");
  bool br = s.getBool("silence.keepBreaths");
  if (ImGui::MenuItem(i18n::tr("Keep breaths"), nullptr, br)) s.set("silence.keepBreaths", !br);
  ui::Hint("Quiet short sounds between the silences (breaths) stay");
  if (ripple) {
    bool rp = s.getBool("silence.ripple");
    if (ImGui::MenuItem(i18n::tr("Close the gaps"), nullptr, rp)) s.set("silence.ripple", !rp);
    ui::Hint("Remove silence moves the parts together, and the clips after them on the track");
  }
}

int applyToClips(App& a, const std::set<ObjectId>& clips, bool remove) {
  const silence::Options o = options(a);
  std::vector<ObjectId> ids;
  std::string problem;
  for (const auto& c : a.doc.song().playlist)
    if (clips.count(c.id) && c.kind == ClipKind::Audio) {
      const char* why = silence::clipProblem(a.doc.song(), c);
      if (*why) problem = why;
      else ids.push_back(c.id);
    }
  int changed = 0;
  if (!ids.empty())
    a.doc.editSong(remove ? "remove silence" : "trim silence", [&](Song& x) {
      for (const ObjectId id : ids) {
        if (remove) changed += silence::removeClipSilence(x, id, o).empty() ? 0 : 1;
        else changed += silence::trimClip(x, id, o) ? 1 : 0;
      }
    });
  if (changed == 0) a.notify(!problem.empty() ? problem : remove ? std::string("No silence to remove") : std::string("No silence at the ends"));
  return changed;
}

bool clipItems(App& a, ObjectId clip) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c || c->kind != ClipKind::Audio) return false;
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(clip)) sel = {clip};
  if (ImGui::MenuItem(i18n::tr("Trim silence at the ends"))) {
    a.later([&a, sel] { applyToClips(a, sel, false); });
    return true;
  }
  ui::Hint("The clip starts at its first sound and ends after its last one");
  if (ImGui::MenuItem(i18n::tr("Remove silence"))) {
    a.later([&a, sel] { applyToClips(a, sel, true); });
    return true;
  }
  ui::Hint("The clip is cut at its silences and the silent parts go (Silence settings: Close the gaps)");
  if (ImGui::BeginMenu(i18n::tr("Silence settings"))) {
    settingsRows(a, true);
    ImGui::EndMenu();
  }
  return false;
}

bool applyToChannel(App& a, ObjectId channel, bool remove) {
  const silence::Options o = options(a);
  const std::string dir = editedDir(a);
  std::string out, why;
  Song next = a.doc.song();
  out = silence::applyToChannel(next, channel, remove, o, dir, &why);
  if (out.empty()) {
    a.notify(why.empty() ? std::string("Nothing changed") : why);
    return false;
  }
  const Channel* nc = next.channel(channel);
  if (!nc) return false;
  const Channel after = *nc;
  a.doc.editChannel(channel, remove ? "remove silence" : "trim silence", [&](Channel& x) {
    x.plugin.data = after.plugin.data;
    x.plugin.params = after.plugin.params;
  });
  return true;
}

bool channelItems(App& a, ObjectId channel) {
  const Channel* ch = a.doc.song().channel(channel);
  if (!ch || !silence::channelHasSample(*ch)) return false;
  if (ImGui::MenuItem(i18n::tr("Trim silence at the ends"))) {
    a.later([&a, channel] { applyToChannel(a, channel, false); });
    return true;
  }
  ui::Hint("The sample starts at its first sound and ends after its last one (a new file; the old one stays)");
  if (ImGui::MenuItem(i18n::tr("Remove silence"))) {
    a.later([&a, channel] { applyToChannel(a, channel, true); });
    return true;
  }
  ui::Hint("Every silence in the sample goes, the parts joined with short fades (a new file; the old one stays)");
  if (ImGui::BeginMenu(i18n::tr("Silence settings"))) {
    settingsRows(a, false);
    ImGui::EndMenu();
  }
  return false;
}

}  // namespace gl::silenceui
