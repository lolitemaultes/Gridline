// Gridline tests - K-19: plugin editor sizes (core/EditorZoom.h) and K-15: Loupe's dock
// layout (packs/edison/edison_dock.h).
//  * the automatic zoom follows the work area (a 1440p screen at 130 % opens a small editor
//    larger, a 1440x900 screen keeps the design size, big editors never grow);
//  * a picked zoom is capped so the window fits the work area, never under 100 % (or the
//    smaller size asked for); K-136: one global size (K-147: 60 % to 150 %, 75 % by default);
//  * the remembered zooms round-trip through the setting string (and junk is ignored);
//  * the magnified picture and the pointer use one mapping;
//  * Loupe's layout in every dock spot: wide / compact form, the waveform takes the rest,
//    controls never stretch, the panel never asks for less than its minimum.
#include <cmath>
#include <string>
#include "Check.h"
#include "core/EditorZoom.h"
#include "core/Settings.h"
#include "packs/edison/edison_dock.h"

using namespace gl;

int main() {
  // ---- automatic zoom (window sizes at 100 %, work areas in logical px as EditorArea gives them)
  // Reverb 914x355 and Compressor 754x334, Parametric EQ 934x517, Multiband 1074x291.
  const float a1440p130W = 1969 - 16, a1440p130H = 1108 - 48 - 26 - 16;
  expect(edzoom::autoZoom(914, 355, a1440p130W, a1440p130H) == 1.25f, "2560x1440 at 130 %: Reverb opens at 125 %");
  expect(edzoom::autoZoom(754, 334, a1440p130W, a1440p130H) == 1.25f, "2560x1440 at 130 %: Compressor opens at 125 %");
  expect(edzoom::autoZoom(934, 517, a1440p130W, a1440p130H) == 1.0f, "2560x1440 at 130 %: the EQ (a tall editor) keeps 100 %");
  expect(edzoom::autoZoom(914, 355, 2560 - 16, 1440 - 90) == 1.5f, "2560x1440 at 100 %: Reverb at 150 %");
  expect(edzoom::autoZoom(914, 355, 3840 - 16, 2160 - 90) == 2.0f, "3840x2160 at 100 %: Reverb at 200 %");
  expect(edzoom::autoZoom(914, 355, 1440 - 16, 900 - 90) == 1.0f, "1440x900: design size");
  expect(edzoom::autoZoom(1074, 291, 1920 - 16, 1080 - 90) == 1.0f, "1920x1080: a wide editor keeps 100 %");
  expect(edzoom::autoZoom(0, 0, 1000, 1000) == 1.0f && edzoom::autoZoom(500, 300, 0, 0) == 1.0f, "unknown sizes: 100 %");
  for (float aw : {900.0f, 1400.0f, 2500.0f, 3800.0f})
    for (float ah : {500.0f, 800.0f, 1300.0f, 2000.0f}) {
      const float z = edzoom::autoZoom(700, 400, aw, ah);
      if (z > 1.0f && (700 * z > aw * edzoom::kAutoWidth + 0.5f || 400 * z > ah * edzoom::kAutoHeight + 0.5f)) {
        expect(false, "automatic zoom stays within its share of the work area");
        return finish();
      }
    }
  expect(true, "automatic zoom stays within its share of the work area (16 areas)");

  // ---- capped to the work area
  expect(edzoom::fitZoom(2.0f, 914, 355, 1067 - 16, 600 - 90) == 1.14f, "200 % in a 1067x600 workspace: capped to 114 %");
  expect(edzoom::fitZoom(1.5f, 914, 355, 1953, 1018) == 1.5f, "150 % fits a 1440p workspace: kept");
  expect(edzoom::fitZoom(1.5f, 1200, 700, 1000, 600) == 0.83f, "K-147: an editor larger than the area shrinks to fit (83 %)");
  expect(edzoom::fitZoom(1.0f, 4000, 700, 1000, 600) == edzoom::kFitMin, "K-147: never under the fit floor (then it scrolls)");
  expect(edzoom::fitZoom(5.0f, 100, 100, 5000, 5000) == edzoom::kMax, "never above 200 %");
  expect(edzoom::fitZoom(0.5f, 100, 100, 5000, 5000) == edzoom::kMin, "never below 60 %");
  // K-136: sizes under 100 %
  expect(edzoom::fitZoom(0.85f, 754, 334, 1904, 990) == 0.85f && edzoom::fitZoom(0.75f, 754, 334, 1904, 990) == 0.75f,
         "85 % and 75 % are kept as asked");
  expect(edzoom::fitZoom(0.85f, 1200, 700, 1000, 600) == 0.83f, "K-147: a reduced editor larger than the area shrinks to fit");
  expect(edzoom::fitZoom(1.25f, 1200, 700, 1000, 600) == 0.83f, "K-147: an enlarged editor larger than the area shrinks to fit");
  expect(edzoom::globalSize(0) == edzoom::kGlobalDefault && edzoom::globalSize(-1) == edzoom::kGlobalDefault &&
             edzoom::globalSize(std::nan("")) == edzoom::kGlobalDefault,
         "global size: unset or junk is the default");
  expect(edzoom::globalSize(0.5) == 0.6f && edzoom::globalSize(3) == 1.5f, "global size: 60 % to 150 %");
  expect(edzoom::globalSize(0.87) == 0.85f && edzoom::globalSize(1.13) == 1.15f, "global size: 5 % steps");
  {
    Settings st;
    expect(st.getNumber("ui.pluginWindowSize") == 0.75 && edzoom::globalSize(st.getNumber("ui.pluginWindowSize")) == edzoom::kGlobalDefault,
           "setting ui.pluginWindowSize: 75 % by default");
    expect(!st.getBool("ui.pluginWindowFit"), "setting ui.pluginWindowFit: off by default (editors do not grow on large screens)");
  }
  {
    bool ok = true;
    for (float want : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f})
      for (float aw : {800.0f, 1200.0f, 2000.0f})
        for (float ah : {500.0f, 900.0f, 1400.0f}) {
          const float z = edzoom::fitZoom(want, 600, 400, aw, ah);
          if (z > edzoom::kFitMin && (600 * z > aw + 0.01f || 400 * z > ah + 0.01f)) ok = false;
          if (z > want + 1e-4f) ok = false;
        }
    expect(ok, "a zoomed window always fits the work area and is never larger than asked");
  }

  // ---- remembered per type
  std::string s;
  s = edzoom::withZoom(s, "gl.eq", 1.5f);
  s = edzoom::withZoom(s, "gl.reverb", 2.0f);
  expect(edzoom::zoomFor(s, "gl.eq") == 1.5f && edzoom::zoomFor(s, "gl.reverb") == 2.0f, "two types remembered");
  expect(edzoom::zoomFor(s, "gl.comp") == 0.0f, "a type never sized: the global size");
  s = edzoom::withZoom(s, "gl.eq", 0);
  expect(edzoom::zoomFor(s, "gl.eq") == 0.0f && edzoom::zoomFor(s, "gl.reverb") == 2.0f, "back to the global size: forgotten, the others kept");
  s = edzoom::withZoom(s, "gl.comp", 1.337f);
  expect(edzoom::zoomFor(s, "gl.comp") == 1.34f, "a dragged size is kept to whole percent");
  s = edzoom::withZoom(s, "gl.dist", 9.0f);
  expect(edzoom::zoomFor(s, "gl.dist") == 2.0f, "clamped when stored");
  expect(edzoom::parse("junk;=2;gl.a=;gl.b=x;gl.c=1.75;;gl.d=-3").size() == 1 && edzoom::zoomFor("junk;gl.c=1.75", "gl.c") == 1.75f,
         "junk in the setting is ignored");
  expect(edzoom::format(edzoom::parse(s)) == s, "the setting string round-trips");
  {
    Settings st;
    expect(st.getString("ui.editorZoom").empty(), "setting ui.editorZoom: empty by default (the global size)");
    expect(st.set("ui.editorZoom", s) && st.getString("ui.editorZoom") == s, "setting ui.editorZoom: stores the string");
    expect(!st.getBool("ui.loupePinned") && st.set("ui.loupePinned", true), "setting ui.loupePinned: off by default, settable");
  }

  // ---- one mapping for the picture and the pointer
  {
    bool ok = true;
    for (float z : edzoom::kSteps)
      for (float p : {100.0f, 123.5f, 480.0f}) {
        const float q = edzoom::toScreen(100, p, z);
        if (std::fabs(edzoom::toEditor(100, q, z) - p) > 1e-3f) ok = false;
      }
    expect(ok, "screen <-> editor mapping round-trips at every step");
    expect(edzoom::toScreen(50, 50, 2) == 50 && edzoom::toScreen(50, 150, 2) == 250, "the region's corner stays, the rest is magnified");
  }

  // ---- K-15: Loupe in each dock spot
  using packs::edison::dock::Layout;
  using packs::edison::dock::layoutFor;
  {
    const Layout main = layoutFor(1500, 700);  // a tab next to the playlist
    expect(!main.compact && main.scale == 1.0f && main.formW == 1500 && main.waveH == 700 - main.headH - main.pad,
           "main area: controls at their design size (not stretched), the form and waveform fill the space");
    const Layout floating = packs::edison::dock::floatingLayout(1100, 560);
    expect(!floating.compact && floating.formW == 1100 && floating.headH == 112 && floating.waveH == 560 - 112 - 8,
           "floating: the J-207 form, unchanged");
    const Layout bottom = layoutFor(1800, 260);  // a strip under the playlist
    expect(!bottom.compact && bottom.waveH >= bottom.minWaveH && bottom.formW <= 1800, "bottom strip: wide form, the waveform keeps its minimum");
    const Layout side = layoutFor(340, 900);  // the browser's column
    expect(side.compact && side.formW == 340 && side.waveH > 400, "side column: the compact form, the waveform takes the height");
    const Layout tiny = layoutFor(120, 80);
    expect(tiny.formW >= packs::edison::dock::kMinW && tiny.waveH >= tiny.minWaveH, "never laid out below its minimum (the panel scrolls)");
    bool ok = true;
    for (float w = 200; w <= 3000; w += 37)
      for (float h = 150; h <= 1600; h += 53) {
        const Layout l = layoutFor(w, h);
        if (l.scale > 1.0f + 1e-4f || l.scale < 0.6f - 1e-4f) ok = false;  // controls never stretch, never shrink past legible
        if (l.formW > std::max(w, packs::edison::dock::kMinW) + 0.5f) ok = false;
        if (l.headH + l.waveH + l.pad > std::max(h, l.headH + l.minWaveH + l.pad) + 0.5f) ok = false;
      }
    expect(ok, "every dock size: controls at 60..100 %, the form within the space (or its minimum)");
  }
  return finish();
}
