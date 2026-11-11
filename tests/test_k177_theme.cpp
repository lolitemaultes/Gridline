// Gridline - K-177 (GitHub issue 42): theme brightness reaches pitch black, the white text and
// glyphs option, and the roles the top bar, browser folders, waveforms and plugin window bars
// take from the theme. The stock dark and light palettes stay exactly as they were.
// Headless Dear ImGui (no window, no renderer).
#include <cmath>
#include <cstdio>
#include <string>
#include "Check.h"
#include "../third_party/imgui/imgui.h"
#include "app/Theme.h"
#include "app/ThemeSettings.h"
#include "core/Settings.h"

using namespace gl;

namespace {
uint32_t rgbOf(ImU32 c) { return ((c >> IM_COL32_R_SHIFT) & 0xFF) << 16 | ((c >> IM_COL32_G_SHIFT) & 0xFF) << 8 | ((c >> IM_COL32_B_SHIFT) & 0xFF); }
float lum(ImU32 c) {
  const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}
std::string hexs(ImU32 c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", static_cast<unsigned>(rgbOf(c)));
  return b;
}
}  // namespace

int main() {
  ImGui::CreateContext();
  ImGui::GetIO().Fonts->AddFontDefault();
  ImGui::GetIO().Fonts->Build();

  // ---- the brightness mapping
  expect(theme::brightnessAmount(0.0f) == 0.0f, "brightness 0: no change");
  expect(std::fabs(theme::brightnessAmount(1.0f) - 0.22f) < 1e-6f, "brightness +100%: as before (0.22 toward white)");
  expect(std::fabs(theme::brightnessAmount(-1.0f) + 1.0f) < 1e-6f, "brightness -100%: all the way to the pitch-black faces");
  expect(std::fabs(theme::brightnessAmount(-0.1f) + 0.022f) < 0.001f, "near 0 the darker side moves as before (0.22 per unit)");
  bool mono = true;
  for (int i = -100; i < 100; ++i) mono = mono && theme::brightnessAmount(i / 100.0f) < theme::brightnessAmount((i + 1) / 100.0f);
  expect(mono, "brightness mapping rises steadily from -100% to +100%");

  // ---- defaults look exactly as before
  const theme::Palette d = theme::makePalette(true, theme::Tweaks{}), l = theme::makePalette(false, theme::Tweaks{});
  expect(rgbOf(d.surface950) == 0x262C30 && rgbOf(d.surface900) == 0x353D42 && rgbOf(d.chrome) == 0x262C30 && rgbOf(d.text) == 0xD5DBDF &&
             rgbOf(d.textDim) == 0xA2ABB0 && rgbOf(d.field) == 0x23282B && rgbOf(d.well) == 0x30373B && rgbOf(d.gridBar) == 0x1D2225,
         "default dark palette unchanged");
  expect(rgbOf(l.surface950) == 0xC9D1D6 && rgbOf(l.surface900) == 0xE0E5E8 && rgbOf(l.chrome) == 0xC9D1D6 && rgbOf(l.text) == 0x1B2226 &&
             rgbOf(l.field) == 0xFAFBFC && rgbOf(l.well) == 0xD8DEE2 && l.dark == false,
         "default light palette unchanged");
  expect(!d.whiteGlyphs && !l.whiteGlyphs, "white glyphs off by default");
  // the clip content colour is the same formula as before
  theme::apply(true);
  const ImU32 base = theme::rgb(0x5A8FD0);
  expect(theme::clipInk(base) == theme::mix(base, d.paper, 0.45f) && theme::clipInk(base, true) == base, "dark: clip ink as before");
  theme::apply(false);
  expect(theme::clipInk(base) == theme::mix(base, l.ink, 0.42f) && theme::clipInk(base, true) == theme::mix(base, l.ink, 0.15f),
         "light: clip ink as before");
  expect(theme::tint(theme::rgb(0xFB9244)) == theme::lit(theme::rgb(0xFB9244)), "light: folder tint is the lit role as before");

  // ---- pitch black
  theme::Tweaks black;
  black.brightness = -1.0f;
  for (int mode = 0; mode < 2; ++mode) {
    const theme::Palette p = theme::makePalette(mode == 0, black);
    const char* m = mode == 0 ? "dark" : "light";
    expect(rgbOf(p.chrome) == 0x000000 && rgbOf(p.surface950) == 0x000000, (std::string(m) + ": -100% chrome (top bar) is pitch black").c_str());
    expect(lum(p.surface900) < 0.03f && lum(p.field) < 0.01f && lum(p.well) < 0.1f, (std::string(m) + ": panels, fields and grids near black").c_str());
    expect(p.dark, (std::string(m) + ": pitch-black faces are styled as dark faces").c_str());
    expect(theme::contrastRatio(p.text, p.surface900) >= 4.5f && theme::contrastRatio(p.textDim, p.surface900) >= 3.0f,
           (std::string(m) + ": text stays readable on black (" + hexs(p.text) + " on " + hexs(p.surface900) + ")").c_str());
    expect(lum(p.surface950) <= lum(p.surface900) && lum(p.surface900) < lum(p.surface800) && lum(p.surface800) < lum(p.surface700) &&
               lum(p.surface700) < lum(p.surface600),
           (std::string(m) + ": surface steps keep their order").c_str());
    expect(lum(p.gridBar) <= lum(p.gridBeat) && lum(p.gridBeat) <= lum(p.gridStep) && lum(p.gridStep) < lum(p.well),
           (std::string(m) + ": grid lines stay apart from the playlist well").c_str());
  }
  // every step on the way down stays readable, both modes
  bool readable = true;
  std::string worst;
  for (int mode = 0; mode < 2; ++mode)
    for (int i = 0; i <= 100; i += 5) {
      theme::Tweaks t;
      t.brightness = -i / 100.0f;
      const theme::Palette p = theme::makePalette(mode == 0, t);
      if (theme::contrastRatio(p.text, p.surface900) < 4.5f) readable = false, worst = std::to_string(i);
    }
  expect(readable, ("text readable at every brightness from 0 to -100% " + worst).c_str());

  // ---- white text and glyphs
  theme::Tweaks wt;
  wt.whiteText = true;
  {
    const theme::Palette p = theme::makePalette(true, wt);
    expect(rgbOf(p.text) == 0xFFFFFF, "white text: text is pure white");
    expect(lum(p.textDim) > lum(d.textDim) && lum(p.textDim) < 1.0f && lum(p.textFaint) < lum(p.textDim), "white text: dim and faint are steps of white");
    expect(p.whiteGlyphs && rgbOf(p.logo) == 0xFFFFFF, "white text: glyphs and logo white");
    expect(p.surface900 == d.surface900 && p.chrome == d.chrome, "white text: panels untouched");
    theme::setTweaks(wt);
    theme::apply(true);
    expect(rgbOf(theme::tint(theme::rgb(0xFB9244))) == 0xFFFFFF, "white text: folder glyph tint is white");
    expect(lum(theme::clipInk(base)) > lum(theme::mix(base, d.paper, 0.45f)), "white text: waveforms lighter");
    const ImVec4 tx = ImGui::GetStyle().Colors[ImGuiCol_Text];
    expect(tx.x == 1.0f && tx.y == 1.0f && tx.z == 1.0f, "white text: ImGui text colour is white");
  }
  {
    const theme::Palette p = theme::makePalette(false, wt);
    expect(p.text == l.text && !p.whiteGlyphs, "white text on light faces is left off (would not read)");
    theme::Tweaks both = wt;
    both.brightness = -1.0f;
    const theme::Palette q = theme::makePalette(false, both);
    expect(rgbOf(q.text) == 0xFFFFFF && q.whiteGlyphs, "white text on light mode taken to pitch black: white");
  }

  // ---- parts that stay dark in both modes follow the tweaks
  theme::setTweaks(theme::Tweaks{});
  theme::apply(false);
  expect(theme::darkFaces().surface800 == d.surface800 && theme::darkFaces().text == d.text, "light mode: dark faces are the stock dark slate");
  theme::setTweaks(black);
  theme::apply(false);
  expect(rgbOf(theme::darkFaces().surface950) == 0x000000, "brightness -100%: the dark faces (plugin window bars) go black too");
  theme::setTweaks(theme::Tweaks{});

  // ---- settings: white text persists, applies, and the reset clears it
  Settings st;
  expect(!st.getBool("theme.whiteText"), "setting off by default");
  theme::modeToSettings(theme::Mode::Dark, st);
  theme::Tweaks t;
  t.brightness = -1.0f;
  t.whiteText = true;
  theme::tweaksToSettings(t, st);
  const theme::Tweaks r = theme::tweaksFromSettings(st);
  expect(r.whiteText && r.brightness == -1.0f && r == t, "white text and brightness read back");
  theme::applyFromSettings(st);
  expect(rgbOf(theme::pal().chrome) == 0x000000 && rgbOf(theme::pal().text) == 0xFFFFFF, "applied: black chrome, white text");
  theme::resetThemeSettings(st);
  expect(!theme::tweaksFromSettings(st).whiteText, "reset colours clears white text");
  theme::applyFromSettings(st);
  expect(theme::pal().chrome == d.chrome && theme::pal().text == d.text, "reset restores the stock dark palette");

  // ---- dev notes (used by the app test)
  theme::devNote("folder", theme::rgb(0x123456));
  expect(rgbOf(theme::devUsed("folder")) == 0x123456 && theme::devUsed("nothing") == 0, "dev notes record the colour drawn");
  char buf[1024];
  theme::devDump(buf, sizeof buf);
  expect(std::string(buf).find("used.folder=#123456") != std::string::npos && std::string(buf).find("chrome=#262C30") != std::string::npos,
         "dev dump lists the palette and the noted colours");

  ImGui::DestroyContext();
  return finish();
}
