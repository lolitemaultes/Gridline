// Gridline - J-55 theme page: mode (dark / light / follow system) and the
// user's tweaks (accent and role colours, contrast, brightness) persist in the
// settings, apply to the palette and the ImGui style, keep the role colours
// distinct, and reset. J-129: the control style (theme.controls) defaults to
// slate, reads unknown text as slate, round-trips and applies. Headless Dear ImGui (no window, no renderer).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include "Check.h"
#include "../third_party/imgui/imgui.h"
#include "app/Theme.h"
#include "app/ThemeSettings.h"
#include "core/Settings.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

uint32_t rgbOf(ImU32 c) { return ((c >> IM_COL32_R_SHIFT) & 0xFF) << 16 | ((c >> IM_COL32_G_SHIFT) & 0xFF) << 8 | ((c >> IM_COL32_B_SHIFT) & 0xFF); }

float lum(ImU32 c) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}

}  // namespace

int main() {
  ImGui::CreateContext();
  ImGui::GetIO().Fonts->AddFontDefault();
  ImGui::GetIO().Fonts->Build();

  // Colour text.
  expect(theme::colourText(0x3D63DD) == "#3D63DD" && theme::parseColour("#3d63dd") == 0x3D63DD, "colour text round trip");
  expect(theme::parseColour("") == theme::Tweaks::kKeep && theme::parseColour("#12345") == theme::Tweaks::kKeep &&
             theme::parseColour("zzzzzz") == theme::Tweaks::kKeep,
         "empty / invalid colour = theme default");

  // Mode: older settings (ui.darkTheme only) keep working.
  Settings st;
  expect(theme::modeFromSettings(st) == theme::Mode::Dark, "default mode: dark");
  st.set("ui.darkTheme", false);
  expect(theme::modeFromSettings(st) == theme::Mode::Light, "legacy ui.darkTheme=false reads as light");
  theme::modeToSettings(theme::Mode::System, st);
  expect(theme::modeFromSettings(st) == theme::Mode::System && st.getString("theme.mode") == "system", "follow system stored");
  theme::modeToSettings(theme::Mode::Dark, st);
  expect(st.getBool("ui.darkTheme") && theme::modeFromSettings(st) == theme::Mode::Dark, "dark keeps ui.darkTheme in step");

  // Default tweaks change nothing.
  const theme::Palette darkBase = theme::makePalette(true, theme::Tweaks{});
  theme::applyFromSettings(st);
  expect(theme::isDark() && theme::pal().accent == darkBase.accent && theme::pal().surface900 == darkBase.surface900,
         "defaults: the stock dark palette");

  // Tweaks persist through a save / load.
  theme::Tweaks t;
  t.accent = 0x3D63DD;
  t.led = 0x4ECCFF;
  t.contrast = 0.5f;
  t.brightness = -0.25f;
  theme::tweaksToSettings(t, st);
  theme::modeToSettings(theme::Mode::Light, st);
  const fs::path dir = fs::temp_directory_path() / "gridline_j55_theme";
  fs::create_directories(dir);
  const std::string path = (dir / "settings.json").string();
  expect(st.save(path), "settings saved");
  Settings st2;
  expect(st2.load(path), "settings loaded");
  const theme::Tweaks t2 = theme::tweaksFromSettings(st2);
  expect(t2.accent == 0x3D63DD && t2.led == 0x4ECCFF && t2.hot == theme::Tweaks::kKeep && t2.playhead == theme::Tweaks::kKeep,
         "persisted: accent, LED, untouched roles stay default");
  expect(std::fabs(t2.contrast - 0.5f) < 1e-6f && std::fabs(t2.brightness + 0.25f) < 1e-6f, "persisted: contrast and brightness");
  expect(theme::modeFromSettings(st2) == theme::Mode::Light, "persisted: light mode");

  // Applying: palette and ImGui style follow.
  theme::applyFromSettings(st2);
  const theme::Palette& p = theme::pal();
  const theme::Palette lightBase = theme::makePalette(false, theme::Tweaks{});
  expect(!theme::isDark() && !p.dark, "light applied");
  expect(rgbOf(p.accent) == 0x3D63DD, "accent override applied");
  expect(rgbOf(p.led) == 0x4ECCFF && rgbOf(p.meterLow) == 0x4ECCFF, "LED override applied (and the low meter range)");
  expect(p.hot == lightBase.hot && p.playhead == lightBase.playhead, "roles without overrides keep the theme colours");
  expect(lum(p.surface900) < lum(lightBase.surface900), "negative brightness darkens the panels");
  expect(std::fabs(lum(p.text) - lum(p.surface900)) > std::fabs(lum(lightBase.text) - lum(lightBase.surface900)) - 0.05f,
         "positive contrast keeps text at least as far from the panels");
  expect(lum(p.text) <= lum(lightBase.text), "contrast moves the text toward ink on the light theme");
  const ImVec4 nav = ImGui::GetStyle().Colors[ImGuiCol_NavCursor];
  expect(std::fabs(nav.z - (0xDD / 255.0f)) < 0.01f && std::fabs(nav.x - (0x3D / 255.0f)) < 0.01f, "ImGui style uses the new accent");
  expect(rgbOf(p.hot) != rgbOf(p.led) && rgbOf(p.accent) != rgbOf(p.danger), "role colours stay distinct");

  // Lower contrast pulls the text toward the panels.
  theme::Tweaks low;
  low.contrast = -1.0f;
  const theme::Palette lowP = theme::makePalette(true, low);
  expect(std::fabs(lum(lowP.text) - lum(lowP.surface900)) < std::fabs(lum(darkBase.text) - lum(darkBase.surface900)), "negative contrast softens text");
  theme::Tweaks hi;
  hi.contrast = 1.0f;
  const theme::Palette hiP = theme::makePalette(true, hi);
  expect(std::fabs(lum(hiP.text) - lum(hiP.surface900)) > std::fabs(lum(darkBase.text) - lum(darkBase.surface900)), "positive contrast sharpens text");
  theme::Tweaks bright;
  bright.brightness = 1.0f;
  expect(lum(theme::makePalette(true, bright).surface900) > lum(darkBase.surface900), "positive brightness lightens the panels");

  // Reset: tweaks gone, the light / dark choice stays.
  theme::resetThemeSettings(st2);
  expect(theme::tweaksFromSettings(st2) == theme::Tweaks{} && theme::modeFromSettings(st2) == theme::Mode::Light, "reset theme keeps the mode");
  theme::applyFromSettings(st2);
  expect(theme::pal().accent == lightBase.accent && theme::pal().surface900 == lightBase.surface900, "reset restores the stock light palette");

  // Follow system resolves to one of the two (whatever this machine says).
  theme::modeToSettings(theme::Mode::System, st2);
  theme::applyFromSettings(st2, true);
  expect(theme::isDark() == theme::systemPrefersDark(), "follow system applies the desktop's choice");

  // J-129: control style.
  {
    Settings cs;
    expect(cs.getString("theme.controls") == "slate" && theme::controlStyleFromSettings(cs) == ui::ControlStyle::Slate,
           "controls: default slate");
    const ui::ControlStyle all[] = {ui::ControlStyle::Classic, ui::ControlStyle::Graphite, ui::ControlStyle::Hairline,
                                    ui::ControlStyle::Lumen, ui::ControlStyle::Slate};
    const std::string cpath = (dir / "controls.json").string();
    for (ui::ControlStyle c : all) {
      theme::controlStyleToSettings(c, cs);
      Settings back;
      expect(cs.save(cpath) && back.load(cpath) && theme::controlStyleFromSettings(back) == c &&
                 back.getString("theme.controls") == ui::controlStyleName(c),
             (std::string("controls round trip: ") + ui::controlStyleName(c)).c_str());
      theme::applyFromSettings(back);
      expect(ui::controlStyle() == c && ui::styledControls() == (c != ui::ControlStyle::Classic),
             (std::string("controls applied: ") + ui::controlStyleName(c)).c_str());
    }
    cs.set("theme.controls", "chrome");
    expect(theme::controlStyleFromSettings(cs) == ui::ControlStyle::Slate, "controls: unknown value = slate");
    cs.set("theme.controls", "");
    expect(theme::controlStyleFromSettings(cs) == ui::ControlStyle::Slate, "controls: empty = slate");
    {
      const ui::ControlStyleScope scope(ui::ControlStyle::Lumen);
      expect(ui::controlStyle() == ui::ControlStyle::Lumen, "controls: a preview scope draws its own style");
    }
    expect(ui::controlStyle() == ui::ControlStyle::Slate, "controls: back to the setting after the preview");
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
  ImGui::DestroyContext();
  return finish();
}
