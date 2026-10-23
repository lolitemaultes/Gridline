// Gridline — "utility" pack: editor-side helpers shared by the pack's UI files
// (live data access, spectrum analysis, small drawing helpers).
#pragma once
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "utility_rt.h"

namespace gl::packs::utility {

inline float val(const PluginState& s, int k, float def = 0.f) {
  audit::stateRead(k);  // J-92 binding audit
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : def;
}

// The running instance behind a mixer slot (nullptr while the engine has none).
UtilityFx* liveFx(App& a, const PluginTarget& t);

// Magnitude spectrum of the ring (dB re a full-scale sine) per FFT bin.
// channel: 0 = mid, 1 = left, 2 = right. end = 0 reads the newest frames.
struct SpectrumAnalyzer {
  void analyse(const StereoRing* ring, int fftSize, bool window, int channel, uint64_t end, std::vector<float>& outDb);

 private:
  Fft fft_;
  std::vector<float> re_, im_, win_, l_, r_;
  int n_ = 0;
  bool hann_ = true;
  float norm_ = 1;
};

// dB value of the band [f0, f1] (max of the bins inside, or interpolated).
float bandDb(const std::vector<float>& binsDb, double f0, double f1, double sr);

// Frequency axis with a log / linear blend (scale 1 = fully logarithmic).
double axisToHz(double u, double scale, double nyquist);
double hzToAxis(double hz, double scale, double nyquist);

// Scrolling sonogram image (columns of normalised intensities), fed by hops of the ring.
struct Sonogram {
  int cols = 0, rows = 0, head = 0;
  uint64_t lastPos = 0;
  std::vector<float> img;  // cols * rows, 0..1
  SpectrumAnalyzer an;
  std::vector<float> bins;
  // Adds the columns for audio written since the last call (hop in frames).
  void update(const StereoRing* ring, double sr, int fftSize, int hop, double rangeDb, double gainDb, double tiltDbOct,
              double scale, int columns, int rowCount);
  void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1) const;
};

// Intensity colour for displays (monochrome ramp; the very top uses the accent).
ImU32 heat(float v);

// Small helpers.
void panelBg(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
void textAt(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text);
void textCentred(ImDrawList* dl, ImFont* font, float size, ImVec2 centre, ImU32 col, const char* text);
std::string fmtDb(float db, const char* suffix = "");

// ---- reference-family skin helpers (J-19) ----
// The grey reference family (panel / well / wheel rail / cap / labels) with the
// plugin's own wheel colour (#RRGGBB).
const skin::Skin& fruitySkin(uint32_t fill);
// Wheel module whose value row is a rail slider (reference "value slider").
bool sliderBox(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
               const char* caption);
// Wheel module whose value row is a drop-down list (choice parameters).
bool menuParamBox(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
                  const char* caption);
// Section panel with a caption above it (reference "sober" panels).
void soberPanel(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption);

// Editors in utility_view_ui.cpp.
void editorScope(App& a, const PluginTarget& t, float width);
void editorSpectro(App& a, const PluginTarget& t, float width);
void editorMeter(App& a, const PluginTarget& t, float width);
void editorClock(App& a, const PluginTarget& t, float width);
void editorNotes(App& a, const PluginTarget& t, float width);
// utility_bankeq_ui.cpp
void editorBankEq(App& a, const PluginTarget& t, float width);

}  // namespace gl::packs::utility
