// Gridline - J-99: smooth freehand drawing in editors (see DrawSmoothUi.h).
#include "DrawSmoothUi.h"

#include <algorithm>
#include <cmath>
#include <string>
#include "../core/I18n.h"
#include "App.h"
#include "Theme.h"

namespace gl::drawui {

namespace {

struct Live {
  ImGuiID id = 0;
  bool blocked = false;  // the press started on the chip: no stroke until release
  drawsmooth::Stroke s;
};
Live gLive;

std::string key(const char* surface) { return std::string("draw.smooth.") + (surface ? surface : ""); }

bool inRect(ImVec2 m, ImVec2 a, ImVec2 b) { return m.x >= a.x && m.y >= a.y && m.x < b.x && m.y < b.y; }

}  // namespace

bool enabled(const App& a, const char* surface) {
  const std::string k = key(surface);
  // Unregistered ids read as "on" (getBool of an unknown key is false).
  bool known = false;
  for (const char* s : drawsmooth::kSurfaces) known = known || std::string(s) == (surface ? surface : "");
  return known ? a.settings.getBool(k) : true;
}

void setEnabled(App& a, const char* surface, bool on) { a.settings.set(key(surface), on); }

double amount(const App& a) { return std::clamp(a.settings.getNumber("draw.smoothAmount"), 0.0, 1.0); }

bool rawModifier() { return ImGui::GetIO().KeyAlt; }

void chipRect(ImVec2 p0, ImVec2 p1, ImVec2& c0, ImVec2& c1, int corner) {
  const float w = 22.0f, h = 14.0f, pad = 3.0f;
  const float y0 = corner == 1 ? p1.y - pad - h : p0.y + pad;
  const float x0 = corner == 2 ? p0.x + pad : p1.x - pad - w;
  c0 = ImVec2(x0, y0);
  c1 = ImVec2(x0 + w, y0 + h);
}

const std::vector<uint8_t>& strokeTouched() {
  static const std::vector<uint8_t> none;
  return gLive.s.active() ? gLive.s.touched() : none;
}

bool strokeActive(ImGuiID id) { return gLive.id == id && (gLive.s.active() || gLive.blocked); }

const std::vector<float>* stroke(App& a, ImGuiID id, bool drawing, double x, double v, const float* current, int n, const Spec& spec) {
  if (!drawing) {
    if (gLive.id == id) {
      if (gLive.s.active()) a.doc.endGesture();  // the stroke is one undo step
      gLive = Live{};
    }
    return nullptr;
  }
  if (gLive.id != id) {  // a new stroke
    gLive = Live{};
    gLive.id = id;
    const bool hasRect = spec.p1.x > spec.p0.x && spec.p1.y > spec.p0.y;
    if (hasRect) {
      ImVec2 c0, c1;
      chipRect(spec.p0, spec.p1, c0, c1, spec.chipCorner);
      gLive.blocked = inRect(ImGui::GetIO().MouseClickedPos[0], c0, c1);
    }
    if (gLive.blocked || n <= 0 || !current) return nullptr;
    drawsmooth::Options o;
    o.amount = enabled(a, spec.surface) ? amount(a) : 0.0;
    o.cyclic = spec.cyclic;
    o.raw = rawModifier() || spec.raw;
    o.lo = spec.lo;
    o.hi = spec.hi;
    gLive.s.begin(current, n, o, spec.axis);
    a.doc.endGesture();  // never fold into the previous stroke
    if (spec.seedPress) gLive.s.add(spec.pressX, spec.pressV, ImGui::GetTime() - 1.0 / 60.0);
  }
  if (gLive.blocked || !gLive.s.active()) return nullptr;
  gLive.s.setRaw(rawModifier() || spec.raw);
  gLive.s.add(x, v, ImGui::GetTime());
  return &gLive.s.result();
}

void chip(App& a, const char* surface, ImVec2 p0, ImVec2 p1, int corner) {
  ImVec2 c0, c1;
  chipRect(p0, p1, c0, c1, corner);
  if (p1.x - p0.x < 240.0f || p1.y - p0.y < 40.0f) return;  // small surfaces: the bigger surface of the editor carries the switch
  const bool on = enabled(a, surface);
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool hov = inRect(m, c0, c1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && inRect(ImGui::GetIO().MouseClickedPos[0], c0, c1)) setEnabled(a, surface, !on);
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 lit = theme::litRole(theme::role::kGraph, 1.0f);
  dl->AddRectFilled(c0, c1, theme::withAlpha(P.surface950, hov ? 0.95f : 0.75f), 3.0f);
  dl->AddRect(c0, c1, on ? theme::withAlpha(lit, 0.8f) : theme::withAlpha(P.textFaint, 0.6f), 3.0f, 0, 1.0f);
  // A small wave glyph: smooth when on, stepped when off.
  const float gx0 = c0.x + 4, gx1 = c1.x - 4, gy = (c0.y + c1.y) * 0.5f, amp = (c1.y - c0.y) * 0.25f;
  const ImU32 g = on ? lit : P.textFaint;
  if (on) {
    ImVec2 pts[13];
    for (int i = 0; i <= 12; ++i) {
      const float t = i / 12.0f;
      pts[i] = ImVec2(gx0 + (gx1 - gx0) * t, gy - amp * std::sin(t * 6.2831853f));
    }
    dl->AddPolyline(pts, 13, g, 0, 1.4f);
  } else {
    const float q = (gx1 - gx0) / 4;
    const ImVec2 pts[] = {{gx0, gy}, {gx0 + q, gy}, {gx0 + q, gy - amp}, {gx0 + 2 * q, gy - amp}, {gx0 + 2 * q, gy + amp},
                          {gx0 + 3 * q, gy + amp}, {gx0 + 3 * q, gy}, {gx1, gy}};
    dl->AddPolyline(pts, 8, g, 0, 1.2f);
  }
  if (hov)
    a.setHint(on ? i18n::tr("Smooth drawing: on (click to draw raw here; hold Alt while drawing for one raw stroke)")
                 : i18n::tr("Smooth drawing: off (click to smooth strokes drawn here)"));
}

}  // namespace gl::drawui
