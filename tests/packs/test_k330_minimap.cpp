// Gridline - K-330: Junction's minimap drags in proportion to the minimap's scale.
// A frame-by-frame model of the map: every frame the minimap is fitted to the patch plus the
// view box (as the editor draws it), and the pointer, held still or moved, drives the view.
//   1. the old way (re-fit every frame, centre on the pointer) runs away with a still pointer;
//   2. the new drag (mapping frozen at the press) keeps the view still while the pointer is;
//   3. a drag of N minimap pixels moves the view box N minimap pixels, for a small and a very
//      large patch and at three zooms;
//   4. pressing inside the view box does not jump it; pressing outside centres it there.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../Check.h"
#include "../../src/packs/modulation/junction_minimap.h"

using namespace gl::packs::modulation;

namespace {

constexpr float kW = 900, kH = 430, kSize = 136;

struct Patch {
  float x0, y0, x1, y1;  // modules' bounds in world units
};

// The minimap as the editor fits it this frame (bounds grown by the view box).
MiniMapping fit(const Patch& p, float panX, float panY, float zoom) {
  float vx0, vy0, vx1, vy1;
  miniViewRect(panX, panY, zoom, kW, kH, vx0, vy0, vx1, vy1);
  float w, h;
  MiniMapping m = miniFit(kSize, std::min(p.x0, vx0), std::min(p.y0, vy0), std::max(p.x1, vx1), std::max(p.y1, vy1), w, h);
  m.ox += kW - w - 8, m.oy += kH - h - 8;  // the default corner: bottom right
  return m;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  const Patch small{0, 0, 700, 300};
  const Patch large{-4000, -2500, 26000, 9000};  // about 256 modules

  // 1. old behaviour: re-fit every frame, pan = centre on the pointer, pointer held still
  {
    const float zoom = 1.0f;
    float panX = 0, panY = 0;
    MiniMapping m = fit(small, panX, panY, zoom);
    const float mx = m.ox + 600 * m.z, my = m.oy + 250 * m.z;  // towards the patch's right end
    float firstX = 0;
    for (int f = 0; f < 30; ++f) {
      m = fit(small, panX, panY, zoom);
      const float cx = m.worldX(mx), cy = m.worldY(my);
      panX = kW * 0.5f - cx * zoom, panY = kH * 0.5f - cy * zoom;
      if (f == 0) firstX = panX;
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "old minimap drag drifts with a still pointer (%.0f -> %.0f)", static_cast<double>(firstX), static_cast<double>(panX));
    std::printf("# %s\n", msg);
    expect(std::fabs(panX - firstX) > 50, msg);
  }

  // 2 + 3. new drag: frozen mapping
  for (const Patch* p : {&small, &large}) {
    for (const float zoom : {0.3f, 1.0f, 2.0f}) {
      float panX = -100 * zoom, panY = -50 * zoom;
      const MiniMapping m = fit(*p, panX, panY, zoom);
      float vx0, vy0, vx1, vy1;
      miniViewRect(panX, panY, zoom, kW, kH, vx0, vy0, vx1, vy1);
      // Press in the middle of the view box.
      const float sx = m.ox + (vx0 + vx1) * 0.5f * m.z, sy = m.oy + (vy0 + vy1) * 0.5f * m.z;
      const MiniDrag d = miniBegin(m, sx + 2, sy - 1, panX, panY, zoom, kW, kH);
      float qx = panX, qy = panY;
      miniPan(d, sx + 2, sy - 1, zoom, kW, kH, qx, qy);
      char msg[160];
      std::snprintf(msg, sizeof msg, "%s patch, zoom %.1f: pressing on the view box does not move it", p == &small ? "small" : "large", static_cast<double>(zoom));
      expect(near(qx, panX, 0.01f) && near(qy, panY, 0.01f), msg);
      // Hold still for 30 frames.
      for (int f = 0; f < 30; ++f) miniPan(d, sx + 2, sy - 1, zoom, kW, kH, qx, qy);
      std::snprintf(msg, sizeof msg, "%s patch, zoom %.1f: a still pointer keeps the view still", p == &small ? "small" : "large", static_cast<double>(zoom));
      expect(near(qx, panX, 0.01f) && near(qy, panY, 0.01f), msg);
      // Drag 10 minimap pixels right and 6 down, in small steps.
      for (int k = 1; k <= 10; ++k) miniPan(d, sx + 2 + k, sy - 1 + k * 0.6f, zoom, kW, kH, qx, qy);
      float nx0, ny0, nx1, ny1;
      miniViewRect(qx, qy, zoom, kW, kH, nx0, ny0, nx1, ny1);
      const float movedX = (nx0 - vx0) * m.z, movedY = (ny0 - vy0) * m.z;  // in minimap pixels
      std::snprintf(msg, sizeof msg, "%s patch, zoom %.1f: a 10 x 6 pixel drag moves the view box %.2f x %.2f minimap pixels", p == &small ? "small" : "large",
                    static_cast<double>(zoom), static_cast<double>(movedX), static_cast<double>(movedY));
      expect(near(movedX, 10, 0.05f) && near(movedY, 6, 0.05f), msg);
    }
  }

  // 4. pressing outside the view box centres the view on that point
  {
    const float zoom = 1.0f;
    const float panX = 0, panY = 0;
    const MiniMapping m = fit(large, panX, panY, zoom);
    const float wx = 20000, wy = 5000;
    const float sx = m.ox + wx * m.z, sy = m.oy + wy * m.z;
    const MiniDrag d = miniBegin(m, sx, sy, panX, panY, zoom, kW, kH);
    float qx = panX, qy = panY;
    miniPan(d, sx, sy, zoom, kW, kH, qx, qy);
    float x0, y0, x1, y1;
    miniViewRect(qx, qy, zoom, kW, kH, x0, y0, x1, y1);
    expect(near((x0 + x1) * 0.5f, wx, 0.5f) && near((y0 + y1) * 0.5f, wy, 0.5f), "a press outside the view box centres the view there");
  }
  return finish();
}
