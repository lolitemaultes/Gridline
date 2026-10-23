// Gridline — envelope editor widget for menv::Env (time / volume slots, filter
// unit envelopes, waveshaper curve). Reference behaviour:
//   left-click empty space   add a point (and drag it)
//   drag a point             move it (x between its neighbours; Slide moves all later points too)
//   drag a tension handle    (small ring mid-segment) bend the segment up/down
//   right-click a point      point menu: curve mode, delete
//   double-click a point     delete it (first point excepted)
//   Step mode                drag across the graph to draw flat steps of the snap width
//   Freeze                   no edits
//   Snap                     x snaps to the grid (snapDiv cells per x unit)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <functional>
#include <string>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../engine/MEnv.h"
#include "Theme.h"

namespace gl::envui {

struct Style {
  ImU32 bg, grid, gridStrong, line, fill, point, pointHot, handle, text;
};

struct Opts {
  double xMax = 1;        // x range shown (0..xMax)
  int gridDiv = 4;        // grid lines per x unit (e.g. 4 per beat)
  int gridEvery = 4;      // every n-th grid line is strong
  int snapDiv = 0;        // snap cells per x unit (0 = off)
  int yLines = 4;         // horizontal grid divisions
  bool freeze = false, step = false, slide = false;
  bool lockEnds = false;  // first point stays at x = 0 and last at x = xMax (waveshaper)
  bool centreLine = false;
  double playX = -1;      // playhead (x units), < 0 = none
  const char* const* modeNames = nullptr;  // menu names for menv modes (ModeCount entries)
  int modeCount = menv::ModeCount;
  // Extra point-menu items (called inside the popup with the point index; return true when the envelope changed).
  std::function<bool(int)> extraMenu;
  // J-93: readout text for a position (x units, 0..1 level), shown next to the
  // hovered / dragged point or the mouse; empty = none.
  std::function<std::string(double, double)> readout;
  // J-93: x-unit numbers along the top (every gridEvery lines when > 0).
  bool xLabels = false;
};

struct State {
  int drag = -1, bend = -1, menuPt = -1;
  double dragX0 = 0;
  bool stepping = false;
};

enum Result { None = 0, Changed = 1, Released = 2 };

inline const char* const* defaultModeNames() {
  static const char* n[menv::ModeCount] = {"Single curve", "Double curve", "Hold", "Stairs", "Smooth stairs", "Pulse", "Wave",
                                           "Single curve 2", "Double curve 2", "Half sine", "Smooth", "Single curve 3", "Double curve 3"};
  return n;
}

// Draws and edits an envelope in the rect p0..p1. Returns Changed while editing
// (envelope modified, prepared) and Released when a gesture ends.
template <int N>
int edit(const char* id, ImVec2 p0, ImVec2 p1, menv::Env<N>& e, State& st, const Opts& o, const Style& sty) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  auto X = [&](double x) { return p0.x + static_cast<float>(x / o.xMax) * W; };
  auto Y = [&](double y) { return p1.y - static_cast<float>(std::clamp(y, 0.0, 1.0)) * H; };
  auto toX = [&](float sx) { return std::clamp(static_cast<double>((sx - p0.x) / W) * o.xMax, 0.0, o.xMax); };
  auto toY = [&](float sy) { return std::clamp(static_cast<double>((p1.y - sy) / H), 0.0, 1.0); };
  auto snap = [&](double x) {
    if (o.snapDiv <= 0) return x;
    double c = 1.0 / o.snapDiv;
    return std::round(x / c) * c;
  };
  // Background and grid.
  dl->AddRectFilled(p0, p1, sty.bg, 2);
  dl->PushClipRect(p0, p1, true);
  int gl = static_cast<int>(std::ceil(o.xMax * o.gridDiv));
  for (int k = 1; k < gl; ++k) {
    float x = X(static_cast<double>(k) / o.gridDiv);
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), (o.gridEvery > 0 && k % o.gridEvery == 0) ? sty.gridStrong : sty.grid, 1.0f);
  }
  for (int r = 1; r < o.yLines; ++r) {
    float y = p0.y + H * r / o.yLines;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), (o.centreLine && r * 2 == o.yLines) ? sty.gridStrong : sty.grid, 1.0f);
  }
  // Curve + fill (J-93: one anti-aliased polyline over a seamless gradient fill).
  const int S = std::max(64, static_cast<int>(W));
  {
    static thread_local std::vector<ImVec2> pts;
    pts.resize(static_cast<size_t>(S) + 1);
    for (int i = 0; i <= S; ++i) {
      double x = o.xMax * i / S;
      pts[static_cast<size_t>(i)] = ImVec2(X(x), Y(e.eval(x)));
    }
    const ImDrawListFlags keep = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // adjacent quads: no seams
    const int v0 = dl->VtxBuffer.Size;
    for (int i = 1; i <= S; ++i) {
      const ImVec2 a = pts[static_cast<size_t>(i - 1)], b = pts[static_cast<size_t>(i)];
      ImVec2 quad[4] = {a, b, ImVec2(b.x, p1.y), ImVec2(a.x, p1.y)};
      dl->AddConvexPolyFilled(quad, 4, sty.fill);
    }
    dl->Flags = keep;
    // Fade towards the background at the bottom (the shade keeps the fill's alpha).
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p0, ImVec2(p0.x, p1.y), theme::mix(sty.fill, sty.line, 0.35f),
                                                  theme::mix(sty.fill, sty.bg, 0.85f));
    dl->AddPolyline(pts.data(), S + 1, theme::withAlpha(sty.line, 0.28f), 0, 4.0f);  // soft glow
    dl->AddPolyline(pts.data(), S + 1, sty.line, 0, 1.8f);
  }
  if (o.xLabels && o.gridDiv > 0) {
    const int every = o.gridEvery > 0 ? o.gridEvery : o.gridDiv;
    const float fsz = ImGui::GetFontSize() * 0.82f;
    for (int k = 1; k * every < gl + every; ++k) {  // the first beat's number would sit on the y-axis labels
      const double xv = static_cast<double>(k * every) / o.gridDiv;
      if (xv >= o.xMax - 1e-9) break;
      char lb[16];
      std::snprintf(lb, sizeof lb, "%g", xv + 1.0);
      dl->AddText(ImGui::GetFont(), fsz, ImVec2(std::round(X(xv) + 4), p0.y + 3), theme::withAlpha(sty.text, 0.5f), lb);
    }
  }
  if (o.playX >= 0) {
    const float px = std::round(X(o.playX)) + 0.5f;
    dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p1.y), theme::withAlpha(sty.text, 0.85f), 1.0f);
    dl->AddTriangleFilled(ImVec2(px - 4, p0.y), ImVec2(px + 4, p0.y), ImVec2(px, p0.y + 5), sty.text);
  }
  // Interaction.
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##env", ImVec2(W, H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImVec2 m = io.MousePos;
  int hotPt = -1, hotBend = -1;
  if (hov && !act) {
    float best = 7.0f;
    for (int i = 0; i < e.n; ++i) {
      float d = std::hypot(m.x - X(e.x[i]), m.y - Y(e.y[i]));
      if (d < best) best = d, hotPt = i;
    }
    if (hotPt < 0)
      for (int i = 1; i < e.n; ++i) {
        if (e.x[i] - e.x[i - 1] <= 0) continue;
        double xm = (e.x[i] + e.x[i - 1]) * 0.5;
        float d = std::hypot(m.x - X(xm), m.y - Y(e.eval(xm)));
        if (d < 6.5f) hotBend = i;
      }
  }
  int res = None;
  if (!o.freeze) {
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      st.drag = -1, st.bend = -1, st.stepping = false;
      if (hotPt >= 0) {
        st.drag = hotPt;
      } else if (hotBend >= 0) {
        st.bend = hotBend;
      } else if (o.step) {
        st.stepping = true;
      } else if (e.n < N) {  // add a point at the click
        double x = snap(toX(m.x));
        int at = e.segmentAt(x);
        if (o.lockEnds) at = std::clamp(at, 1, std::max(1, e.n - 1));
        e.insert(at, x, static_cast<float>(toY(m.y)), 0.f, at < e.n && at > 0 ? e.mode[std::min(at + 1, e.n - 1)] : static_cast<uint8_t>(menv::SingleCurve));
        st.drag = at;
        res = Changed;
      }
      st.dragX0 = st.drag >= 0 ? e.x[st.drag] : 0;
    }
    if (act && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (st.drag >= 0 && st.drag < e.n && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        int i = st.drag;
        double nx = snap(toX(m.x));
        double lo = i > 0 ? e.x[i - 1] : 0.0, hi = (i + 1 < e.n && !o.slide) ? e.x[i + 1] : o.xMax;
        if (o.lockEnds && (i == 0 || i == e.n - 1)) nx = i == 0 ? 0.0 : o.xMax;
        nx = std::clamp(nx, lo, hi);
        if (o.slide) {  // later points follow
          double dx = nx - e.x[i];
          double last = e.x[e.n - 1] + dx;
          if (last > o.xMax) dx -= last - o.xMax;
          for (int k = i; k < e.n; ++k) e.x[k] = std::clamp(e.x[k] + dx, 0.0, o.xMax);
        } else {
          e.x[i] = nx;
        }
        e.y[i] = static_cast<float>(toY(m.y));
        res = Changed;
      } else if (st.bend > 0 && st.bend < e.n && io.MouseDelta.y != 0) {
        int i = st.bend;
        float dir = e.y[i] >= e.y[i - 1] ? 1.f : -1.f;  // dragging up bends towards a fast start for rising segments
        e.t[i] = std::clamp(e.t[i] - io.MouseDelta.y * 0.006f * dir, -1.0f, 1.0f);
        if (std::fabs(e.t[i]) < 0.004f) e.t[i] = 0;
        res = Changed;
      } else if (st.stepping && o.snapDiv > 0) {  // draw a flat step at the cell under the mouse
        double c = 1.0 / o.snapDiv;
        double x0 = std::floor(toX(m.x) / c) * c, x1 = std::min(o.xMax, x0 + c);
        float y = static_cast<float>(toY(m.y));
        // Remove points inside [x0, x1], then add hold points at x0 and x1.
        for (int k = e.n - 1; k >= 0; --k)
          if (e.x[k] >= x0 - 1e-9 && e.x[k] <= x1 + 1e-9) e.erase(k);
        int at = e.segmentAt(x0);
        e.insert(at, x0, y, 0.f, menv::SingleCurve);
        e.insert(at + 1, x1 - 1e-6, y, 0.f, menv::Hold);
        res = Changed;
      }
    }
    if (ImGui::IsItemDeactivated()) {
      st.drag = st.bend = -1;
      st.stepping = false;
      res |= Released;
    }
    if (hov && hotPt > 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !(o.lockEnds && hotPt == e.n - 1) && e.n > 2) {
      e.erase(hotPt);
      st.drag = -1;
      res |= Changed | Released;
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hotPt >= 0) {
      st.menuPt = hotPt;
      ImGui::OpenPopup("##ptmenu");
    }
    if (ImGui::BeginPopup("##ptmenu")) {
      int i = st.menuPt;
      if (i >= 0 && i < e.n) {
        const char* const* names = o.modeNames ? o.modeNames : defaultModeNames();
        if (i > 0) {
          ImGui::TextDisabled("%s", i18n::tr("Segment shape"));
          for (int k = 0; k < o.modeCount; ++k)
            if (ImGui::MenuItem(names[k], nullptr, e.mode[i] == k)) {
              e.mode[i] = static_cast<uint8_t>(k);
              res |= Changed | Released;
            }
          if (ImGui::MenuItem(i18n::tr("Reset tension"))) e.t[i] = 0, res |= Changed | Released;
          ImGui::Separator();
        }
        if (o.extraMenu && o.extraMenu(i)) res |= Changed | Released;
        bool canDel = i > 0 && e.n > 2 && !(o.lockEnds && i == e.n - 1);
        if (ImGui::MenuItem(i18n::tr("Delete point"), nullptr, false, canDel)) {
          e.erase(i);
          res |= Changed | Released;
        }
      }
      ImGui::EndPopup();
    }
  }
  if (res & Changed) e.prepare();
  // Points and tension handles.
  for (int i = 1; i < e.n; ++i) {
    if (e.x[i] - e.x[i - 1] <= 0 || e.mode[i] == menv::Hold) continue;
    double xm = (e.x[i] + e.x[i - 1]) * 0.5;
    ImVec2 c(X(xm), Y(e.eval(xm)));
    bool hot = i == hotBend || i == st.bend;
    dl->AddCircle(c, hot ? 4.5f : 3.2f, hot ? sty.pointHot : sty.handle, 12, 1.3f);
  }
  for (int i = 0; i < e.n; ++i) {
    ImVec2 c(X(e.x[i]), Y(e.y[i]));
    bool hot = i == hotPt || i == st.drag;
    const float r = hot ? 5.2f : 4.0f;
    if (hot) dl->AddCircleFilled(c, r + 4.0f, theme::withAlpha(sty.pointHot, 0.25f), 20);
    dl->AddCircleFilled(c, r + 1.2f, IM_COL32(0, 0, 0, 150), 18);
    dl->AddCircleFilled(c, r, hot ? sty.pointHot : sty.point, 18);
    dl->AddCircleFilled(c, r * 0.42f, theme::withAlpha(sty.line, hot ? 0.0f : 0.9f), 10);
  }
  // J-93 readout: the point being dragged / hovered, else the mouse position.
  if (o.readout && (hov || act)) {
    const int ri = st.drag >= 0 && st.drag < e.n ? st.drag : hotPt;
    const double rx = ri >= 0 ? e.x[ri] : toX(m.x), ry = ri >= 0 ? static_cast<double>(e.y[ri]) : toY(m.y);
    const std::string txt = o.readout(rx, ry);
    if (!txt.empty()) {
      const ImVec2 at = ri >= 0 ? ImVec2(X(rx), Y(ry)) : m;
      const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
      ImVec2 b0(at.x + 12, at.y - ts.y - 14);
      if (b0.x + ts.x + 12 > p1.x - 2) b0.x = at.x - ts.x - 24;
      if (b0.y < p0.y + 2) b0.y = at.y + 12;
      const ImVec2 b1(b0.x + ts.x + 12, b0.y + ts.y + 6);
      dl->AddRectFilled(b0, b1, theme::withAlpha(theme::mix(sty.bg, IM_COL32(0, 0, 0, 255), 0.55f), 0.94f), 3);
      dl->AddRect(b0, b1, theme::withAlpha(sty.line, 0.6f), 3, 0, 1.0f);
      dl->AddText(ImVec2(std::round(b0.x + 6), std::round(b0.y + 3)), sty.text, txt.c_str());
    }
  }
  dl->PopClipRect();
  dl->AddRect(p0, p1, theme::mix(sty.bg, IM_COL32(0, 0, 0, 255), 0.5f), 2, 0, 1.0f);
  ImGui::PopID();
  return res;
}

}  // namespace gl::envui
