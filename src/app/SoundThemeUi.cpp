// Gridline - J-133: Settings > Theme > Sounds (see SoundThemeUi.h).
#include "SoundThemeUi.h"
#include <algorithm>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../engine/UiSounds.h"
#include "App.h"
#include "DialogLayout.h"
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
  // J-137: the columns are measured from where the controls start and end on
  // this page (screen positions): the Preview button never runs past the
  // section, a description that does not fit is shortened (whole on hover).
  const std::string cur = resolveTheme(st.getString(kThemeKey));
  const ImGuiStyle& gs = ImGui::GetStyle();
  float nameW = 0, lineW = 0;
  for (int i = 0; i < kThemeCount; ++i) {
    nameW = std::max(nameW, ImGui::CalcTextSize(themeName(i)).x);
    lineW = std::max(lineW, ImGui::CalcTextSize(themeLine(i)).x);
  }
  const float previewW = std::max(64.0f, ImGui::CalcTextSize(tr("Preview")).x + 2 * gs.FramePadding.x);
  for (int i = 0; i < kThemeCount; ++i) {
    label(i == 0 ? "Sound theme" : "");
    const float x0 = ImGui::GetCursorScreenPos().x, right = x0 + ImGui::GetContentRegionAvail().x;
    const float descX = x0 + ImGui::GetFrameHeight() + gs.ItemInnerSpacing.x + nameW + theme::kS3;
    const float buttonX = std::max(x0, std::min(descX + lineW + theme::kS3, right - previewW));
    ImGui::PushID(i);
    if (ImGui::RadioButton(themeName(i), cur == themeId(i))) st.set(kThemeKey, std::string(themeId(i)));  // App::tick loads it
    const float y = ImGui::GetItemRectMin().y;
    if (buttonX - theme::kS3 - descX >= 40.0f) {  // room for a description
      ImGui::SameLine(0, 0);
      ImGui::SetCursorScreenPos(ImVec2(descX, y));
      ui::TextFit(themeLine(i), buttonX - theme::kS3 - descX, P.textFaint, true);
    }
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(std::max(buttonX, ImGui::GetCursorScreenPos().x + theme::kS2), y));
    ImGui::BeginDisabled(a.uiSoundsDisabled);
    if (ui::TextButton(tr("Preview"), false, previewW, tr("Play this theme's startup sound"))) a.previewSoundTheme(themeId(i));
    ImGui::EndDisabled();
    ImGui::PopID();
  }

  label("Volume");
  float vol = static_cast<float>(st.getNumber("sounds.volume")) * 100.0f;
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##uisndvol", &vol, 0.0f, 100.0f, "%.0f%%")) st.set("sounds.volume", static_cast<double>(vol) / 100.0);

  // Per-event switches; the Play column clears the widest name (checkbox square + gap included)
  // and stays inside the section (J-137: screen positions, like the theme rows above).
  float eventW = 0;
  for (int i = 0; i < kEventCount; ++i) eventW = std::max(eventW, ImGui::CalcTextSize(eventName(static_cast<Event>(i))).x);
  const float playW = std::max(48.0f, ImGui::CalcTextSize(tr("Play")).x + 2 * gs.FramePadding.x);
  for (int i = 0; i < kEventCount; ++i) {
    const auto e = static_cast<Event>(i);
    bool on = st.getBool(settingKey(e));
    label(i == 0 ? "Sounds" : "");
    const float x0 = ImGui::GetCursorScreenPos().x, right = x0 + ImGui::GetContentRegionAvail().x;
    const float playX = std::min(x0 + std::max(fieldW - playW, ImGui::GetFrameHeight() + gs.ItemInnerSpacing.x + eventW + theme::kS3), right - playW);
    ImGui::PushID(100 + i);
    if (ImGui::Checkbox(eventName(e), &on)) st.set(settingKey(e), on);
    const float y = ImGui::GetItemRectMin().y;
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(std::max(playX, ImGui::GetCursorScreenPos().x + theme::kS2), y));
    ImGui::BeginDisabled(a.uiSoundsDisabled || !a.uiSounds.hasClip(e));
    if (ui::TextButton(tr("Play"), false, playW, tr("Preview this sound"))) {
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
