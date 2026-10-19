// Gridline — FLP import: automation channel point data (event 234).
//
//   u32 1, u32 0x40, u8 ?, u32 4, u32 format (@13), u32 count (@17), then count records:
//     format 1 (FL 4–8): 16 bytes — f32 beats after the previous point, f32 value,
//                         f32 tension of the segment ending here, u8 curve mode, 3 bytes
//     format 2+:         24 bytes — f64 beats after the previous point, f64 value,
//                         f32 tension, u8 curve mode, u8, u8, u8 tension sign
//   then a trailer (LFO / clip settings, not used).
// Observed on the shipped demo projects (format 3) and FL's automation-clip
// presets (formats 1 and 2); see analysis/demos/notes_alpha.md.
#include <algorithm>
#include <cmath>
#include "../AutomationEdit.h"
#include "Flp.h"

namespace gl::flp {

bool decodeAutomationPoints(const Event& e, int ppq, std::vector<AutoPoint>& out) {
  out.clear();
  if (e.size < 21) return false;
  const uint32_t format = e.u32(13), n = e.u32(17);
  const size_t stride = format == 1 ? 16 : 24;
  double beats = 0;
  for (uint32_t i = 0; i < n && 21 + stride * (i + 1) <= e.size; ++i) {
    const size_t o = 21 + stride * static_cast<size_t>(i);
    double d = stride == 16 ? static_cast<double>(e.f32(o)) : e.f64(o);
    double v = stride == 16 ? static_cast<double>(e.f32(o + 4)) : e.f64(o + 8);
    float t = stride == 16 ? e.f32(o + 8) : e.f32(o + 16);
    int mode = e.u8(stride == 16 ? o + 12 : o + 20);
    if (!std::isfinite(d) || d < 0) d = 0;
    beats += d;
    AutoPoint p;
    p.pos = static_cast<Tick>(std::llround(beats * std::max(1, ppq)));
    p.value = static_cast<float>(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0));
    p.tension = autoedit::tensionFromFl(std::clamp(std::isfinite(t) ? t : 0.0f, -1.0f, 1.0f));
    p.shape = autoedit::shapeFromFlMode(mode);
    out.push_back(p);
  }
  return !out.empty();
}

}  // namespace gl::flp
