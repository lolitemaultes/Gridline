// Gridline - J-133: Settings > Theme > Sounds (see SoundThemeUi.h).
#include "SoundThemeUi.h"
#include <algorithm>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../engine/UiSounds.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::uisound {

namespace {
using i18n::tr;
using theme::pal;

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }

// The translated strings of the theme and event tables (extracted by tools/i18n_extract.py).
const char* themeName(int i) {
  static const char* const kNames[kThemeCount] = {"Graphite", "Prism", "Tine", "Signal", "Classic"};
  return tr(kNames[std::clamp(i, 0, kThemeCount - 1)]);
}
const char* themeLine(int i) {
  static const char* const kLines[kThemeCount] = {
      "Warm wood: a tuned bar under a felt mallet, close and dry",
      "Struck crystal with a short sparkle and a small bright hall",
      "Electric-piano tine: a warm bell with a gentle bark",
      "Precise electronics: clean tones that snap into place",
      "The original Gridline glass sounds",
  };
  return tr(kLines[std::clamp(i, 0, kThemeCount - 1)]);
}
const char* eventName(Event e) {
  static const char* const kNames[kEventCount] = {"Startup", "Project opened", "Project saved", "Export finished",
                                                   "Import finished", "Background task finished", "Needs attention",
                                                   "Error", "Recording started", "Recording stopped"};
  return tr(kNames[static_cast<int>(e)]);
}
}  // namespace

void drawThemeSettings(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  bool snd = st.getBool("sounds.enabled");
  label("Interface sounds");
  if (ImGui::Checkbox(tr("Play short sounds for startup, saving, exports, imports and errors"), &snd)) st.set("sounds.enabled", snd);
  ui::Hint(tr("Quiet while the song plays or records, except errors. Never recorded or exported."));
  ImGui::BeginDisabled(!snd);

  // Theme: one row per theme, a radio button, its description and a preview.
  const std::string cur = resolveTheme(st.getString(kThemeKey));
  float nameW = 0;
  for (int i = 0; i < kThemeCount; ++i) nameW = std::max(nameW, ImGui::CalcTextSize(themeName(i)).x);
  const float descX = 168 + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + nameW + theme::kS3;
  float previewX = descX + theme::kS3;
  for (int i = 0; i < kThemeCount; ++i) previewX = std::max(previewX, descX + ImGui::CalcTextSize(themeLine(i)).x + theme::kS3);
  for (int i = 0; i < kThemeCount; ++i) {
    label(i == 0 ? "Sound theme" : "");
    ImGui::PushID(i);
    if (ImGui::RadioButton(themeName(i), cur == themeId(i))) st.set(kThemeKey, std::string(themeId(i)));  // App::tick loads it
    ImGui::SameLine(descX);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", themeLine(i));
    ImGui::SameLine(previewX);
    ImGui::BeginDisabled(a.uiSoundsDisabled);
    if (ui::TextButton(tr("Preview"), false, 64, tr("Play this theme's startup sound"))) a.previewSoundTheme(themeId(i));
    ImGui::EndDisabled();
    ImGui::PopID();
  }

  label("Volume");
  float vol = static_cast<float>(st.getNumber("sounds.volume")) * 100.0f;
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##uisndvol", &vol, 0.0f, 100.0f, "%.0f%%")) st.set("sounds.volume", static_cast<double>(vol) / 100.0);

  // Per-event switches; the Play column clears the widest name (checkbox square + gap included).
  float playX = fieldW + 140;
  for (int i = 0; i < kEventCount; ++i)
    playX = std::max(playX, 168 + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                ImGui::CalcTextSize(eventName(static_cast<Event>(i))).x + theme::kS3);
  for (int i = 0; i < kEventCount; ++i) {
    const auto e = static_cast<Event>(i);
    bool on = st.getBool(settingKey(e));
    label(i == 0 ? "Sounds" : "");
    ImGui::PushID(100 + i);
    if (ImGui::Checkbox(eventName(e), &on)) st.set(settingKey(e), on);
    ImGui::SameLine(playX);
    ImGui::BeginDisabled(a.uiSoundsDisabled || !a.uiSounds.hasClip(e));
    if (ui::TextButton(tr("Play"), false, 48, tr("Preview this sound"))) {
      a.uiSounds.setVolume(static_cast<float>(st.getNumber("sounds.volume")));
      a.uiSounds.trigger(e);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::EndDisabled();
  if (a.uiSoundsDisabled) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), "%s",
                       tr("Interface sounds are turned off for this session (--no-ui-sounds or GRIDLINE_NO_UI_SOUNDS)."));
    ImGui::PopTextWrapPos();
  }
}

}  // namespace gl::uisound
