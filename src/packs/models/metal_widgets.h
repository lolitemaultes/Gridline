// Gridline — "models" pack, J-22 group "metal": editor helpers shared by the
// Alloy / Spectra Morph / Grain Scanner / Spline Scratcher editors (UI thread).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../../engine/Sample.h"
#include "../flc_kit.h"
#include "../samplers/samplers.h"
#include "harmor.h"
#include "metal.h"
#include "../../core/Platform.h"

namespace gl::packs::models::metal::ui {

// Parameter value (default when absent).
inline float prmf(const PluginState& s, int i, float d = 0.f) {
  return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d;
}
inline int prmi(const PluginState& s, int i, int d = 0) { return static_cast<int>(std::lround(prmf(s, i, static_cast<float>(d)))); }

// Reference form colours are stored blue-green-red; → skin #RRGGBB.
inline uint32_t bgr(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// File dialog for a sample (the app's built-in browser dialog).
inline void openSampleDialog(App& a, const std::string& title, std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".aif", ".aiff"};
  a.fileDialog.onOk = std::move(ok);
}

// The sample of a path (cached; null when it cannot be decoded); no path = the built-in phrase.
inline SamplePtr sampleOf(const std::string& path) { return path.empty() ? samplers::scratchDefaultSample() : sampleCache().get(path); }

// Min/max waveform of `smp` across [a, b) of its frames in a rect (horizontal or vertical time axis).
inline void waveform(ImDrawList* dl, const Sample* smp, ImVec2 p0, ImVec2 p1, ImU32 col, bool vertical = false, double a = 0, double b = 1) {
  if (!smp || smp->frames() < 2) return;
  const float len = vertical ? p1.y - p0.y : p1.x - p0.x;
  const float span = vertical ? p1.x - p0.x : p1.y - p0.y;
  const int cols = std::max(1, static_cast<int>(len));
  const double f0 = std::clamp(a, 0.0, 1.0) * static_cast<double>(smp->frames()), f1 = std::clamp(b, 0.0, 1.0) * static_cast<double>(smp->frames());
  for (int i = 0; i < cols; ++i) {
    size_t s0 = static_cast<size_t>(f0 + (f1 - f0) * i / cols), s1 = static_cast<size_t>(f0 + (f1 - f0) * (i + 1) / cols);
    s1 = std::min(std::max(s1, s0 + 1), smp->frames());
    float lo = 0, hi = 0;
    const size_t stride = std::max<size_t>(1, (s1 - s0) / 64);
    for (size_t k = s0; k < s1; k += stride) lo = std::min(lo, smp->left[k]), hi = std::max(hi, smp->left[k]);
    const float mid = span * 0.5f;
    if (vertical)
      dl->AddLine(ImVec2(p0.x + mid - hi * mid * 0.92f, p1.y - static_cast<float>(i)), ImVec2(p0.x + mid - lo * mid * 0.92f + 1, p1.y - static_cast<float>(i)), col);  // bottom = start
    else
      dl->AddLine(ImVec2(p0.x + static_cast<float>(i), p0.y + mid - hi * mid * 0.92f), ImVec2(p0.x + static_cast<float>(i), p0.y + mid - lo * mid * 0.92f + 1), col);
  }
}

// Point editor for one articulator / map curve (hm::Curve: points as x steps).
// Drag points, double-click to add, right-click to delete. True when changed.
inline bool curveEdit(const flk::Ctx& c, const char* id, float x, float y, float w, float h, hm::Curve& cv, bool timeAxis, float neutral,
                      int& sel) {
  using hm::Pt;
  ImDrawList* dl = c.dl;
  const ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(dl, p0, p1, c.K);
  const ImVec2 q0(p0.x + 6, p0.y + 6), q1(p1.x - 6, p1.y - 6);
  const float W = q1.x - q0.x, H = q1.y - q0.y;
  for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(q0.x, q0.y + H * i / 4), ImVec2(q1.x, q0.y + H * i / 4), c.K.graphGrid, 1);
  const double len = cv.pts.empty() ? 1.0 : std::max(1e-6, hm::curveLength(cv));
  const double view = timeAxis ? std::max(0.05, len * 1.15) : len;
  auto sx = [&](double xv) { return q0.x + static_cast<float>(xv / view) * W; };
  auto sy = [&](double yv) { return q1.y - static_cast<float>(std::clamp(yv, 0.0, 1.0)) * H; };
  dl->AddLine(ImVec2(q0.x, sy(neutral)), ImVec2(q1.x, sy(neutral)), c.K.graphGridStrong, 1);
  ImGui::SetCursorScreenPos(q0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, W), std::max(1.0f, H)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 m = ImGui::GetIO().MousePos;
  std::vector<double> ax(cv.pts.size(), 0.0);
  for (size_t i = 1; i < cv.pts.size(); ++i) ax[i] = ax[i - 1] + cv.pts[i].dt;
  int hot = -1;
  // Dense (resynthesised) curves are shown but only edited through their first points.
  const bool dense = cv.pts.size() > 512;
  if (!dense)
    for (size_t i = 0; i < cv.pts.size(); ++i)
      if (std::fabs(m.x - sx(ax[i])) < 6 && std::fabs(m.y - sy(cv.pts[i].y)) < 6) hot = static_cast<int>(i);
  bool changed = false;
  if (cv.pts.empty() && hov && ImGui::IsMouseClicked(0)) {
    cv.pts = {{0, neutral, 0, 0}, {1, neutral, 0, 0}};
    changed = true;
  }
  if (hov && ImGui::IsMouseClicked(0)) sel = hot;
  if (!dense && hov && ImGui::IsMouseDoubleClicked(0) && hot < 0 && !cv.pts.empty()) {
    const double xv = std::clamp((m.x - q0.x) / W * view, 0.0, view);
    const float yv = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
    size_t i = 1;
    while (i < cv.pts.size() && ax[i] < xv) ++i;
    const double prevX = ax[i - 1];
    Pt p{static_cast<float>(xv - prevX), yv, 0, 0};
    if (i < cv.pts.size()) cv.pts[i].dt = static_cast<float>(std::max(0.0, ax[i] - xv));
    cv.pts.insert(cv.pts.begin() + static_cast<long>(i), p);
    sel = static_cast<int>(i);
    changed = true;
  }
  if (hov && ImGui::IsMouseClicked(1) && hot > 0 && cv.pts.size() > 2) {
    if (static_cast<size_t>(hot) + 1 < cv.pts.size()) cv.pts[static_cast<size_t>(hot) + 1].dt += cv.pts[static_cast<size_t>(hot)].dt;
    cv.pts.erase(cv.pts.begin() + hot);
    sel = -1;
    changed = true;
  }
  if (act && sel >= 0 && sel < static_cast<int>(cv.pts.size()) && ImGui::IsMouseDragging(0, 0)) {
    Pt& p = cv.pts[static_cast<size_t>(sel)];
    p.y = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
    if (sel > 0) {
      const double prevX = ax[static_cast<size_t>(sel) - 1];
      const double nx = std::clamp((m.x - q0.x) / W * view, prevX, view);
      const double old = p.dt;
      p.dt = static_cast<float>(nx - prevX);
      if (!timeAxis && static_cast<size_t>(sel) + 1 < cv.pts.size())
        cv.pts[static_cast<size_t>(sel) + 1].dt = static_cast<float>(std::max(0.0, cv.pts[static_cast<size_t>(sel) + 1].dt + old - p.dt));
    }
    changed = true;
  }
  ImVec2 prev;
  const int steps = 200;
  for (int i = 0; i <= steps; ++i) {
    const double xv = view * i / steps;
    const ImVec2 q(sx(xv), sy(cv.pts.empty() ? neutral : hm::curveAt(cv, std::min(xv, len))));
    if (i) dl->AddLine(prev, q, c.K.graphCurve, 2);
    prev = q;
  }
  if (!dense)
    for (size_t i = 0; i < cv.pts.size(); ++i) {
      const ImVec2 q(sx(ax[i]), sy(cv.pts[i].y));
      const bool s = static_cast<int>(i) == sel;
      dl->AddCircleFilled(q, s ? 5 : 3.5f, static_cast<int>(i) == cv.sustain ? c.K.accent2C() : s ? c.K.text : c.K.graphCurve);
    }
  if (hov) c.a.setHint("Drag points · double-click: add · right-click: delete");
  return changed;
}

// Per-editor UI state keyed by the plugin target.
template <class T>
T& stateFor(const PluginTarget& t) {
  static std::map<uint64_t, T> m;
  return m[t.key()];
}

}  // namespace gl::packs::models::metal::ui
