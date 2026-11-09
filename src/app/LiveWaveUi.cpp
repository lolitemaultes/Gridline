// Gridline — drawing a recording in progress (K-145). See LiveWaveUi.h.
#include "LiveWaveUi.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::livewaveui {

float drawLane(ImDrawList* dl, const LiveWave& w, ImVec2 p0, ImVec2 p1, LiveWaveView v, const Style& st) {
  const float W = p1.x - p0.x, mid = std::round((p0.y + p1.y) * 0.5f);
  if (W <= 1 || p1.y <= p0.y) return p0.x;
  const float half = (p1.y - p0.y) * st.fill * st.gain;
  if (st.centre) dl->AddLine(ImVec2(p0.x, mid + 0.5f), ImVec2(p1.x, mid + 0.5f), st.centre, 1.0f);
  v.width = W;
  if (v.pxScale <= 0) v.pxScale = 1;
  static std::vector<LiveWavePoint> pts;  // UI thread
  traceLiveWave(w, st.channel, v, pts);
  float headX = p0.x;
  if (pts.size() >= 2) {
    const float minH = 1.0f;  // silence still shows as a line
    auto ys = [&](const LiveWavePoint& q, float& ya, float& yb) {
      ya = mid - std::clamp(q.hi, -1.0f, 1.0f) * half;
      yb = mid - std::clamp(q.lo, -1.0f, 1.0f) * half;
      if (yb - ya < minH) {
        const float c = (ya + yb) * 0.5f;
        ya = c - minH * 0.5f, yb = c + minH * 0.5f;
      }
      ya = std::max(p0.y, ya), yb = std::min(p1.y, yb);
    };
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const size_t quads = pts.size() - 1;
    constexpr size_t kChunk = 4096;  // stays inside 16-bit indices
    float ya0, yb0;
    ys(pts[0], ya0, yb0);
    for (size_t q0 = 0; q0 < quads; q0 += kChunk) {
      const size_t n = std::min(kChunk, quads - q0);
      dl->PrimReserve(static_cast<int>(n * 6), static_cast<int>(n * 4));
      for (size_t i = q0; i < q0 + n; ++i) {
        float ya1, yb1;
        ys(pts[i + 1], ya1, yb1);
        const float xa = p0.x + pts[i].x, xb = p0.x + std::max(pts[i + 1].x, pts[i].x);
        dl->PrimQuadUV(ImVec2(xa, ya0), ImVec2(xb, ya1), ImVec2(xb, yb1), ImVec2(xa, yb0), uv, uv, uv, uv, st.wave);
        ya0 = ya1, yb0 = yb1;
      }
    }
    headX = p0.x + pts.back().x;
  }
  if (st.head && v.end >= 0) {
    headX = p0.x + static_cast<float>((std::min(v.end, static_cast<double>(w.frames())) - v.start) * W / v.span);
    if (headX >= p0.x && headX <= p1.x) dl->AddLine(ImVec2(headX, p0.y), ImVec2(headX, p1.y), st.head, st.headWidth);
  }
  return headX;
}

}  // namespace gl::livewaveui
