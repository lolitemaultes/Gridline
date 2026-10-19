// Gridline — default workspace geometry (J-28). See Workspace.h.
#include "Workspace.h"
#include <algorithm>
#include <cmath>

namespace gl::workspace {

float rackHeightFor(int channels) {
  // Tab bar + toolbar + step header, one 35 px row per channel, then the
  // add-channel row and a little air.
  return 128.0f + 35.0f * static_cast<float>(std::max(1, channels));
}

Layout defaultLayout(float w, float h, int channels) {
  Layout L;
  const float W = std::max(w, 400.0f), H = std::max(h, 300.0f);
  L.area = {0, 0, W, H};
  // Browser: a sidebar; mixer: a thin column of compact strips (they wrap
  // into rows and scroll vertically, so every strip stays readable).
  const float browserW = std::floor(std::clamp(W * 0.15f, 200.0f, 280.0f));
  const float mixerW = std::floor(std::clamp(W * 0.235f, 330.0f, 470.0f));
  L.browser = {0, 0, browserW, H};
  L.mixer = {W - mixerW, 0, mixerW, H};
  L.playlist = {browserW, 0, W - browserW - mixerW, H};
  L.browserFrac = browserW / W;
  L.mixerFrac = mixerW / (W - browserW);
  // Rack: floating over the playlist, left of its centre, never taller than
  // most of the area and never wider than the playlist.
  const float margin = 16.0f;
  const float rackW = std::floor(std::min(700.0f, L.playlist.w - 2 * margin));
  const float rackH = std::floor(std::min(rackHeightFor(channels), H * 0.72f));
  float rx = L.playlist.x + std::max(margin, std::min(L.playlist.w * 0.10f, (L.playlist.w - rackW) * 0.5f));
  float ry = std::floor(L.playlist.y + std::clamp(H * 0.16f, margin, std::max(margin, H - rackH - margin)));
  L.rack = {std::floor(rx), ry, rackW, rackH};
  return L;
}

}  // namespace gl::workspace
