// Gridline - the layouts' chrome control kinds (K-112). See LookChrome.h.
#include "LookChrome.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "LookPaint.h"

namespace gl::look::chrome {

using layout::ButtonKind;
using layout::FaderKind;
using layout::FieldKind;
using layout::KnobKind;
using layout::MeterKind;

namespace {
const Tokens& tk() { return painter(current()).tk; }
}  // namespace

void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k, KnobKind kind, const char* valueText) {
  switch (kind) {
    case KnobKind::OpenArc: {  // a small knob, the style's least detail
      Knob s = k;
      s.size = 0;
      look::knob(dl, c, r, s);
      break;
    }
    case KnobKind::SmallRing: {  // a smaller face inside the same hit circle
      Knob s = k;
      s.size = 0;
      look::knob(dl, c, std::round(r * 0.82f), s);
      break;
    }
    case KnobKind::Stepper: {  // the value as "< v >" in the knob's cell
      char b[32];
      if (!valueText) std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(k.norm * 100.0f)));
      const float h = std::min(2.0f * r, 22.0f);
      look::stepper(dl, ImVec2(c.x - r * 1.6f, c.y - h * 0.5f), ImVec2(c.x + r * 1.6f, c.y + h * 0.5f), valueText ? valueText : b, k.state, 0);
      break;
    }
    default: look::knob(dl, c, r, k);
  }
}

void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f, FaderKind kind) {
  if (kind == FaderKind::Studio) {
    look::fader(dl, p0, p1, f);
    return;
  }
  Slider s;
  s.norm = f.norm;
  s.vertical = true;
  s.state = f.state;
  const float w = p1.x - p0.x, cx = std::round((p0.x + p1.x) * 0.5f);
  ImVec2 a = p0, b = p1;
  if (kind == FaderKind::LineCap) a.x = cx - std::max(6.0f, w * 0.22f), b.x = cx + std::max(6.0f, w * 0.22f);
  else if (kind == FaderKind::RoundCap) a.x = cx - std::max(8.0f, w * 0.34f), b.x = cx + std::max(8.0f, w * 0.34f);
  look::slider(dl, a, b, s);
  if (f.meterL >= 0) {  // the level beside it, as the Studio fader shows it
    Meter m;
    m.l = f.meterL;
    m.r = f.meterR;
    m.peakL = f.peakL;
    m.peakR = f.peakR;
    look::meter(dl, ImVec2(b.x + 3, p0.y), ImVec2(std::min(p1.x, b.x + 11), p1.y), m);
  }
}

void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, ButtonKind kind, ImU32 role) {
  switch (kind) {
    case ButtonKind::Pill: {  // the style's segmented pill, one segment
      const char* labels[1] = {label ? label : ""};
      look::segmented(dl, p0, p1, labels, 1, (state & On) ? 0 : -1, (state & Hover) ? 0 : -1, state);
      break;
    }
    case ButtonKind::Bevel: {  // the style's face with a light top-left and a dark bottom-right edge
      look::button(dl, p0, p1, label, state, role);
      const Tokens& t = tk();
      const bool in = (state & (On | Active)) != 0;
      const ImU32 hi = in ? t.faceLo : t.faceHi, lo = in ? t.faceHi : t.faceLo;
      dl->AddLine(ImVec2(p0.x + 1, p0.y + 0.5f), ImVec2(p1.x - 1, p0.y + 0.5f), hi);
      dl->AddLine(ImVec2(p0.x + 0.5f, p0.y + 1), ImVec2(p0.x + 0.5f, p1.y - 1), hi);
      dl->AddLine(ImVec2(p0.x + 1, p1.y - 0.5f), ImVec2(p1.x - 1, p1.y - 0.5f), lo);
      dl->AddLine(ImVec2(p1.x - 0.5f, p0.y + 1), ImVec2(p1.x - 0.5f, p1.y - 1), lo);
      break;
    }
    default: look::button(dl, p0, p1, label, state, role);  // Studio and Flat: the style's own button
  }
}

void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m, MeterKind kind) {
  Meter v = m;
  if (kind == MeterKind::Twin && v.r < 0) v.r = v.l;
  look::meter(dl, p0, p1, v);
  const Tokens& t = tk();
  if (kind == MeterKind::Segmented) {  // gaps in the panel colour every 4 px
    if (v.vertical)
      for (float y = p1.y - 4; y > p0.y; y -= 4) dl->AddLine(ImVec2(p0.x, std::floor(y) + 0.5f), ImVec2(p1.x, std::floor(y) + 0.5f), t.panelBot);
    else
      for (float x = p0.x + 4; x < p1.x; x += 4) dl->AddLine(ImVec2(std::floor(x) + 0.5f, p0.y), ImVec2(std::floor(x) + 0.5f, p1.y), t.panelBot);
  } else if (kind == MeterKind::Scaled) {  // a dB scale beside it: 0, -6, -12, -24, -48
    for (float db : {0.0f, -6.0f, -12.0f, -24.0f, -48.0f}) {
      const float k = std::clamp(1.0f + db / 60.0f, 0.0f, 1.0f);
      if (v.vertical) {
        const float y = std::floor(p1.y - k * (p1.y - p0.y)) + 0.5f;
        dl->AddLine(ImVec2(p1.x + 1, y), ImVec2(p1.x + 4, y), t.textFaint);
      } else {
        const float x = std::floor(p0.x + k * (p1.x - p0.x)) + 0.5f;
        dl->AddLine(ImVec2(x, p1.y + 1), ImVec2(x, p1.y + 4), t.textFaint);
      }
    }
  }
}

void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, float norm, int state, FieldKind kind) {
  switch (kind) {
    case FieldKind::FillSlider: {  // the field, its value as a fill from the left, the text over it
      look::field(dl, p0, p1, "", state);
      const Tokens& t = tk();
      const float x = p0.x + 1 + std::clamp(norm, 0.0f, 1.0f) * (p1.x - p0.x - 2);
      dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(x, p1.y - 1), (t.value & 0x00FFFFFFu) | 0x50000000u, t.radiusSm);
      look::valueText(dl, p0, p1, text, state);
      break;
    }
    case FieldKind::Stepper: look::stepper(dl, p0, p1, text, state, 0); break;
    default: look::field(dl, p0, p1, text, state);
  }
}

void drawGalleryPage(Dir d, double t) {
  const ui::ControlStyleScope scope(d);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
  Painter& P = painter(d);
  const ImVec2 p0 = vp->Pos, p1(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
  dl->AddRectFilled(p0, p1, P.tk.panelBot);
  P.panel(dl, ImVec2(p0.x + 8, p0.y + 8), ImVec2(p1.x - 8, p1.y - 8));
  char title[96];
  std::snprintf(title, sizeof title, "%s  /  layout control kinds", dirTitle(d));
  P.header(dl, ImVec2(p0.x + 8, p0.y + 8), ImVec2(p1.x - 8, p0.y + 52), title, "Each layout picks the kind; this control style draws it.");
  const float colW = (vp->Size.x - 48) / static_cast<float>(layout::kLayoutCount);
  for (int k = 0; k < layout::kLayoutCount; ++k) {
    const layout::Style& s = layout::style(static_cast<layout::Preset>(k));
    const float x = p0.x + 24 + colW * static_cast<float>(k);
    float y = p0.y + 70;
    P.label(dl, ImVec2(x, y), ImVec2(x + colW - 16, y + 18), layout::displayName(s.preset), false);
    y += 28;
    for (int i = 0; i < 3; ++i) {  // knobs: idle, hover, bipolar
      Knob kn;
      kn.norm = 0.25f + 0.25f * static_cast<float>(i);
      kn.state = i == 1 ? Hover : 0;
      kn.bipolar = i == 2;
      knob(dl, ImVec2(x + 22 + 56.0f * static_cast<float>(i), y + 22), 18, kn, s.knob);
    }
    y += 60;
    for (int i = 0; i < 3; ++i) {  // buttons: idle, hover, on
      const int st = i == 0 ? 0 : i == 1 ? Hover : On;
      button(dl, ImVec2(x + 60.0f * static_cast<float>(i), y), ImVec2(x + 54 + 60.0f * static_cast<float>(i), y + 22), i == 2 ? "On" : "Off", st, s.button);
    }
    y += 38;
    field(dl, ImVec2(x, y), ImVec2(x + 120, y + 22), "120.000", 0.6f, 0, s.field);
    field(dl, ImVec2(x + 128, y), ImVec2(x + 180, y + 22), "-6.0", 0.4f, Hover, s.field);
    y += 38;
    const float lv = 0.55f + 0.3f * static_cast<float>(std::sin(t * 2.0));
    for (int i = 0; i < 2; ++i) {
      Fader f;
      f.norm = 0.8f - 0.3f * static_cast<float>(i);
      f.meterL = lv;
      f.meterR = lv * 0.9f;
      fader(dl, ImVec2(x + 70.0f * static_cast<float>(i), y), ImVec2(x + 40 + 70.0f * static_cast<float>(i), y + 150), f, s.fader);
    }
    Meter m;
    m.l = lv;
    m.r = lv * 0.85f;
    m.peakL = 0.9f;
    meter(dl, ImVec2(x + 150, y), ImVec2(x + 162, y + 150), m, s.meter);
  }
}

}  // namespace gl::look::chrome
