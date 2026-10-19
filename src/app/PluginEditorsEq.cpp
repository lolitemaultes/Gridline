// Gridline — dispatch for the remaining pre-J-19 editors (Parametric EQ,
// Maximizer, legacy Delay, Reverb, Shaper) and the ed:: drawing helpers.
#include "../engine/Dsp.h"
#include "PluginEditorsCommon.h"

namespace gl {

namespace ed {

void waveIcon(ImDrawList* dl, int shape, ImVec2 c, float w, float h, ImU32 col, float thick) {
  ImVec2 pts[33];
  for (int i = 0; i <= 32; ++i) {
    float ph = i / 32.0f, y = 0;
    switch (shape) {
      case 0: y = std::sin(ph * 6.2831853f); break;
      case 1: y = ph < 0.25f ? ph * 4 : ph < 0.75f ? 2 - ph * 4 : ph * 4 - 4; break;
      case 2: y = 1 - 2 * ph; break;
      case 3: y = ph < 0.5f ? 1 : -1; break;
      case 5: y = 2 * ph - 1; break;
      case 6: y = ph < 0.25f ? 1 : -1; break;
      case 7: y = std::floor(ph * 4) == 0 ? 0.6f : std::floor(ph * 4) == 1 ? -0.2f : std::floor(ph * 4) == 2 ? 0.9f : -0.7f; break;
      case 8: y = ph < 0.464f ? -1 + 2 * (1 - std::exp(-22.8f * ph / 0.464f)) : ph < 0.833f ? 1 : -1; break;
      default: y = std::sin(ph * 91.0f) * std::cos(ph * 37.0f); break;
    }
    pts[i] = ImVec2(c.x - w / 2 + w * ph, c.y - y * h / 2);
  }
  dl->AddPolyline(pts, 33, col, 0, thick);
}

double svfResponseDb(int mode, double cutoff, double reso01, double hz, double sr) {
  // Analog prototype of the state-variable filter (k = 2 - 1.98 r).
  double k = 2.0 - 1.98 * std::clamp(reso01, 0.0, 1.0);
  double w = std::tan(dsp::kPi * std::min(hz, sr * 0.49) / sr) / std::tan(dsp::kPi * std::min(cutoff, sr * 0.49) / sr);
  double re = 1 - w * w, im = k * w;
  double den = re * re + im * im;
  double num;
  switch (mode) {
    case 1: num = (k * w) * (k * w); break;          // band (unity peak scaled by k)
    case 2: num = w * w * w * w; break;              // high
    case 3: num = (1 - w * w) * (1 - w * w); break;  // notch
    default: num = 1; break;                         // low
  }
  return dsp::gainToDb(std::sqrt(num / std::max(den, 1e-30)));
}

}  // namespace ed

void editorEqRef(App& a, const PluginTarget& t, float width);         // PluginEditorsEqRef.cpp
bool drawDynamicsEditor(App& a, const PluginTarget& t, float width);  // PluginEditorsDyn.cpp
bool drawFxEditor(App& a, const PluginTarget& t, float width);        // PluginEditorsFx.cpp
bool drawMiscEditor(App& a, const PluginTarget& t, float width);      // PluginEditorsMisc.cpp

bool drawCustomEditor3(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (drawDynamicsEditor(a, t, width)) return true;
  if (drawFxEditor(a, t, width)) return true;
  if (drawMiscEditor(a, t, width)) return true;
  const std::string& ty = st->type;
  if (ty == "gl.eq") editorEqRef(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
