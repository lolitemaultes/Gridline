// Gridline - Junction map: the minimap's mapping and its drag (K-330).
//
// The minimap shows the whole patch (and the view box) shrunk by a scale `z`: a point of the
// patch at world (x, y) is drawn at (ox + x * z, oy + y * z). Its bounds include the view box,
// so the mapping changes whenever the view moves. A drag therefore freezes the mapping it
// started with: the view box follows the pointer one minimap pixel per pixel, whatever the
// patch size or zoom, instead of chasing a mapping that moves under it every frame.
//
// Pressing inside the view box grabs it where it was pressed (no jump); pressing elsewhere
// centres the view on that point, and the drag continues from there.
#pragma once

namespace gl::packs::modulation {

inline constexpr float kMiniGrabPx = 5;

struct MiniMapping {
  float z = 1, ox = 0, oy = 0;  // screen = o + world * z
  float worldX(float sx) const { return (sx - ox) / z; }
  float worldY(float sy) const { return (sy - oy) / z; }
};

struct MiniDrag {
  MiniMapping map;          // frozen at the press
  float grabX = 0, grabY = 0;  // world offset from the view centre to the grabbed point
};

// The view's world rectangle from its pan (relative to the canvas origin), zoom and canvas size.
inline void miniViewRect(float panX, float panY, float zoom, float W, float H, float& x0, float& y0, float& x1, float& y1) {
  x0 = -panX / zoom, y0 = -panY / zoom, x1 = x0 + W / zoom, y1 = y0 + H / zoom;
}

// The mapping that fits world bounds (x0, y0)-(x1, y1) into a minimap of about `size` pixels,
// relative to its frame's top left, with a 4 pixel margin; frameW / frameH return the frame's size.
inline MiniMapping miniFit(float size, float x0, float y0, float x1, float y1, float& frameW, float& frameH) {
  MiniMapping m;
  const float w = x1 - x0, h = y1 - y0;
  m.z = size / (w > h * 1.4f ? (w > 1 ? w : 1) : (h * 1.4f > 1 ? h * 1.4f : 1));
  frameW = w * m.z + 8, frameH = h * m.z + 8;
  m.ox = 4 - x0 * m.z, m.oy = 4 - y0 * m.z;
  return m;
}

// Starts a drag at screen point (mx, my) in the minimap.
inline MiniDrag miniBegin(const MiniMapping& map, float mx, float my, float panX, float panY, float zoom, float W, float H) {
  MiniDrag d;
  d.map = map;
  float x0, y0, x1, y1;
  miniViewRect(panX, panY, zoom, W, H, x0, y0, x1, y1);
  const float wx = map.worldX(mx), wy = map.worldY(my);
  // A view box only a few pixels across (a large patch) is grabbed within 5 pixels of its centre.
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, reach = kMiniGrabPx / map.z;
  const float hx = (x1 - x0) * 0.5f > reach ? (x1 - x0) * 0.5f : reach, hy = (y1 - y0) * 0.5f > reach ? (y1 - y0) * 0.5f : reach;
  if (wx >= cx - hx && wx <= cx + hx && wy >= cy - hy && wy <= cy + hy) {
    d.grabX = wx - cx;
    d.grabY = wy - cy;
  }
  return d;
}

// The pan that puts the grabbed point under the pointer at (mx, my).
inline void miniPan(const MiniDrag& d, float mx, float my, float zoom, float W, float H, float& panX, float& panY) {
  const float cx = d.map.worldX(mx) - d.grabX, cy = d.map.worldY(my) - d.grabY;
  panX = W * 0.5f - cx * zoom;
  panY = H * 0.5f - cy * zoom;
}

}  // namespace gl::packs::modulation
