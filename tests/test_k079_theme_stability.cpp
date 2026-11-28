// Gridline - K-395 Part B headless regression test: theme colour stability & readability invariants.
//   * theme::mix() and theme::withAlpha() never wrap/corrupt RGBA bytes on out-of-range or NaN/Inf t;
//   * theme::apply() while temporary palettes are pushed (depth 1 and > 8) updates the canonical base
//     so unwinding all popPalette() calls lands on the new theme (never reverting to a stale dark/light palette);
//   * theme::resetPaletteStack() recovers cleanly from leaked pushes;
//   * theme::generation() increments monotonically on every theme change;
//   * corrupted NaN/Inf or extreme tweak values never produce unreadable text-on-panel contrast.
#include <cmath>
#include <limits>
#include "Check.h"
#include "../third_party/imgui/imgui.h"
#include "app/Theme.h"
#include "app/ThemeSettings.h"
#include "core/Settings.h"

using namespace gl;

int main() {
  ImGui::CreateContext();
  ImGui::GetIO().Fonts->AddFontDefault();
  ImGui::GetIO().Fonts->Build();

  // 1. mix() and withAlpha() clamping / NaN / Inf safety.
  {
    const ImU32 a = theme::rgb(0x203040), b = theme::rgb(0xC0D0E0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expect(theme::mix(a, b, -5.0f) == a, "mix: t < 0 clamps cleanly to a (no unsigned wrap)");
    expect(theme::mix(a, b, 5.0f) == b, "mix: t > 1 clamps cleanly to b (no byte overflow)");
    expect(theme::mix(a, b, nan) == a, "mix: NaN t yields finite a");
    expect(theme::mix(a, b, inf) == a && theme::mix(a, b, -inf) == a, "mix: Inf t yields finite a");
    expect(((theme::withAlpha(a, -1.0f) >> IM_COL32_A_SHIFT) & 0xFF) == 0, "withAlpha: negative alpha clamps to 0");
    expect(((theme::withAlpha(a, 5.0f) >> IM_COL32_A_SHIFT) & 0xFF) == 255, "withAlpha: alpha > 1 clamps to 255");
    expect(((theme::withAlpha(a, nan) >> IM_COL32_A_SHIFT) & 0xFF) == 255, "withAlpha: NaN alpha stays opaque");
  }

  // 2. theme::apply() while temporary palettes are pushed (nested > 8 deep) never leaves stale palette.
  {
    theme::apply(true);
    const theme::Palette darkStock = theme::pal();
    const uint64_t g0 = theme::generation();

    theme::Palette fakeSkin = darkStock;
    fakeSkin.surface900 = theme::rgb(0x111111);
    fakeSkin.text = theme::rgb(0xFF00FF);
    for (int i = 0; i < 12; ++i) theme::pushPalette(fakeSkin);
    expect(theme::pal().surface900 == fakeSkin.surface900, "deep pushPalette (>8) activates pushed palette");

    // Mid-frame theme switch from dark to light while scopes are open.
    theme::apply(false);
    expect(theme::generation() > g0 && !theme::isDark(), "apply(false) increments generation and switches mode");

    // Unwind all 12 scopes -> must land on LIGHT base palette, never reverting to old DARK palette!
    for (int i = 0; i < 12; ++i) theme::popPalette();
    const theme::Palette lightStock = theme::makePalette(false, theme::Tweaks{});
    expect(!theme::pal().dark && theme::pal().surface900 == lightStock.surface900 && theme::pal().text == lightStock.text,
           "unwinding pushed palettes after mid-frame apply lands on the new light palette");

    // Frame-boundary resetPaletteStack recovers cleanly from leaked pushes.
    theme::pushPalette(fakeSkin);
    theme::pushPalette(fakeSkin);
    theme::resetPaletteStack();
    expect(theme::pal().surface900 == lightStock.surface900 && theme::pal().text == lightStock.text,
           "resetPaletteStack restores canonical base palette after leaked pushes");
  }

  // 3. NaN / Inf / extreme tweak combinations never produce unreadable text on panel.
  {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    theme::Tweaks bad;
    bad.brightness = nan;
    bad.contrast = inf;
    bad.hue = -inf;
    bad.saturation = nan;
    const theme::Palette pDark = theme::makePalette(true, bad);
    const theme::Palette pLight = theme::makePalette(false, bad);
    expect(theme::contrastRatio(pDark.text, pDark.surface900) >= 4.5f, "NaN/Inf tweaks on dark stay readable");
    expect(theme::contrastRatio(pLight.text, pLight.surface900) >= 4.5f, "NaN/Inf tweaks on light stay readable");

    bool allReadable = true;
    for (bool dark : {true, false}) {
      for (float b : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        for (float c : {-1.0f, 0.0f, 1.0f}) {
          for (float h : {-1.0f, 0.0f, 0.5f}) {
            for (float s : {-1.0f, 0.0f, 1.0f}) {
              theme::Tweaks tw;
              tw.brightness = b;
              tw.contrast = c;
              tw.hue = h;
              tw.saturation = s;
              const theme::Palette p = theme::makePalette(dark, tw);
              const float minRatio = c < 0.0f ? 2.0f : 4.5f;
              if (theme::contrastRatio(p.text, p.surface900) < minRatio - 0.01f) allReadable = false;
            }
          }
        }
      }
    }
    expect(allReadable, "every extreme brightness/contrast/hue/saturation combo maintains readable text contrast");
  }

  // 4. applyFromSettings with NaN settings values sanitizes cleanly.
  {
    Settings st;
    theme::modeToSettings(theme::Mode::Dark, st);
    st.set("theme.brightness", std::numeric_limits<double>::quiet_NaN());
    st.set("theme.contrast", std::numeric_limits<double>::infinity());
    st.set("theme.accent", std::string("not-a-hex-color"));
    theme::applyFromSettings(st);
    expect(theme::isDark() && theme::contrastRatio(theme::pal().text, theme::pal().surface900) >= 4.5f,
           "corrupted settings apply cleanly with readable stock dark palette");
  }

  ImGui::DestroyContext();
  return finish();
}
