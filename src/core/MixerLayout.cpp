// Gridline — mixer layout for thin panes (J-38). See MixerLayout.h.
#include "MixerLayout.h"

#include <algorithm>
#include <cmath>

namespace gl::mixlayout {

float minRowHeight(const Input& in) { return in.minStripH + kComfortExtra; }

Layout compute(const Input& in) {
  Layout L;
  L.column = in.width < kColumnBelow;
  if (!L.column) return L;
  const int n = std::max(1, in.strips);
  const float gap = in.gap;
  L.contentW = std::max(1.0f, in.width - 2 * in.padding - in.scrollbarW);
  // Strips per row at the minimum width, then stretched so a row fills the pane.
  const int fit = std::max(1, static_cast<int>(std::floor((L.contentW + gap) / (kMinStripW + gap))));
  const int per = std::min(fit, std::max(1, n - 1));  // inserts per full row
  L.stripW = std::floor(std::clamp((L.contentW - static_cast<float>(per - 1) * gap) / static_cast<float>(per), std::min(kMinStripW, L.contentW),
                                   kMaxStripW));
  // The master is wider (plate + dB scale); the scale goes first when it doesn't fit.
  float extra = in.masterExtra;
  if (L.stripW + extra > L.contentW) extra -= in.masterScaleW, L.masterScale = false;
  L.masterW = std::min(L.contentW, L.stripW + std::max(0.0f, extra));
  // Flow: master, inserts; wrap like text.
  float x = 0;
  int row = 0;
  for (int i = 0; i < n; ++i) {
    const size_t k = static_cast<size_t>(i);
    const float w = i == 0 ? L.masterW : k < in.widths.size() && in.widths[k] > 0 ? std::min(in.widths[k], L.contentW) : L.stripW;
    if (x > 0 && x + w > L.contentW + 0.01f) x = 0, ++row;
    L.cells.push_back({i, row, x, w});
    x += w + gap;
  }
  L.rows = row + 1;
  L.perRow = 0;
  for (const Cell& c : L.cells) L.perRow += c.row == 0;
  if (x > 0 && x + in.addButtonW > L.contentW + 0.01f) L.addRow = row + 1, L.addX = 0;
  else L.addRow = row, L.addX = x;
  // Heights: the inspector stacks below when both it and one comfortable row fit.
  const float comfort = minRowHeight(in);
  L.showInspector = in.height >= comfort + in.bandH + 2 * in.padding + kInspectorMinH;
  L.inspectorH = L.showInspector ? std::floor(std::clamp(in.height * 0.40f, kInspectorMinH, std::min(kInspectorMaxH, in.height - comfort - in.bandH - 2 * in.padding)))
                                 : 0.0f;
  L.stripsH = in.height - L.inspectorH;
  const float area = L.stripsH - 2 * in.padding;
  const int rowsShown = L.rows;
  // Rows that fit at a comfortable height share the area; more rows scroll, one
  // screenful at a time, never below the minimum strip.
  const int rowsFit = std::max(1, static_cast<int>(std::floor((area + kRowGap) / (comfort + in.bandH + kRowGap))));
  const int share = std::min(rowsShown, rowsFit);
  L.rowH = std::max(in.minStripH, (area - static_cast<float>(share) * in.bandH - static_cast<float>(share - 1) * kRowGap) / static_cast<float>(share));
  L.rowH = std::floor(L.rowH);
  L.rowPitch = in.bandH + L.rowH + kRowGap;
  return L;
}

ToolbarFit toolbarFit(float room, float metersW, float holdW, float routingW) {
  ToolbarFit f;
  float left = room;
  f.meters = left >= metersW;
  if (!f.meters) return f;
  left -= metersW;
  f.hold = left >= holdW;
  if (!f.hold) return f;
  left -= holdW;
  f.routing = left >= routingW;
  return f;
}

int perRow(int strips, int rows) {
  rows = std::clamp(rows, 1, 4);
  return strips > 0 ? (strips + rows - 1) / rows : 0;
}

float middleRowW(const WideInput& in, int row) {
  const int n = perRow(in.middle, in.rows);
  if (n <= 0 || row < 0) return 0;
  const int from = row * n, to = std::min(in.middle, from + n);
  if (from >= to) return 0;
  float w = 0;
  for (int k = from; k < to; ++k) {
    const size_t i = static_cast<size_t>(k);
    w += (i < in.middleW.size() ? in.middleW[i] : in.stripW) + in.gap;
  }
  return w - in.gap;
}

float wideContentW(const WideInput& in) {
  float x = 0;
  if (in.left > 0) x += in.masterW + in.gap + static_cast<float>(in.left - 1) * (in.stripW + in.gap) + in.dividerW - in.gap - in.leftSaved;
  float mid = 0;
  for (int r = 0; r < std::clamp(in.rows, 1, 4); ++r) mid = std::max(mid, middleRowW(in, r));
  x += mid;
  x += in.gap + in.addW + static_cast<float>(std::max(0, in.right)) * (in.stripW + in.gap) - in.rightSaved;
  return x + in.tail + 2 * in.padding;
}

float wideInspectorW(float width, float contentW, float preferred) {
  const float room = std::max(kInspectorMinW, width - kStripsMinW);
  const float base = preferred > 0 ? std::clamp(preferred, kInspectorMinW, room) : std::clamp(width * 0.27f, kInspectorMinW, kInspectorMaxW);
  return std::floor(std::max(base, width - contentW));
}

float masterZoneH(int rows, float rowH, float bandH, float rowGap) {
  const float r = static_cast<float>(std::clamp(rows, 1, 4));
  return r * rowH + (r - 1) * (bandH + rowGap);
}

}  // namespace gl::mixlayout
