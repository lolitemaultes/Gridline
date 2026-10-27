// Gridline - J-129 element design system: helpers, the token-driven base
// painter and the public entry points (see Look.h).
#include "Look.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "LookPaint.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::look {

// ================================================================ names / override
namespace {
const char* const kTitles[] = {"Classic", "Graphite", "Hairline", "Lumen", "Slate"};
}  // namespace

const char* dirName(Dir d) { return ui::controlStyleName(d); }
const char* dirTitle(Dir d) { return kTitles[static_cast<int>(d)]; }
bool parseDir(const char* s, Dir& out) {
  if (s && std::strcmp(s, "current") == 0) return out = Dir::Classic, true;  // Phase 1 name
  return ui::parseControlStyle(s, out);
}

// ================================================================ colour
ImU32 rgb(uint32_t c, float a) {
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}
ImU32 alpha(ImU32 c, float a) {
  const float ca = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
  return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(ca + 0.5f) << IM_COL32_A_SHIFT);
}
ImU32 mix(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int s) {
    const float x = static_cast<float>((a >> s) & 0xFF), y = static_cast<float>((b >> s) & 0xFF);
    return static_cast<ImU32>(x + (y - x) * t + 0.5f) << s;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
ImU32 lighten(ImU32 c, float t) { return mix(c, (c & IM_COL32_A_MASK) | 0x00FFFFFFu, t); }
ImU32 darken(ImU32 c, float t) { return mix(c, c & IM_COL32_A_MASK, t); }
float luma(ImU32 c) {
  return (0.2126f * static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) + 0.7152f * static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) +
          0.0722f * static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF)) /
         255.0f;
}

// ================================================================ shapes
int segs(float r, float sweep) { return std::clamp(static_cast<int>(sweep * std::max(r, 1.0f) * 0.5f), 8, 96); }

void shadeV(ImDrawList* dl, int start, float y0, float y1, ImU32 top, ImU32 bottom) { shadeAlong(dl, start, ImVec2(0, y0), ImVec2(0, y1), top, bottom); }
void shadeH(ImDrawList* dl, int start, float x0, float x1, ImU32 left, ImU32 right) { shadeAlong(dl, start, ImVec2(x0, 0), ImVec2(x1, 0), left, right); }
// Unlike ImGui's KeepAlpha shading this also runs the alpha from c0 to c1;
// anti-aliasing fringe vertices keep their share of the shape's alpha.
void shadeAlong(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1) {
  ImDrawVert* v0 = dl->VtxBuffer.Data + start;
  ImDrawVert* v1 = dl->VtxBuffer.Data + dl->VtxBuffer.Size;
  int maxA = 1;
  for (ImDrawVert* v = v0; v < v1; ++v) maxA = std::max(maxA, static_cast<int>((v->col >> IM_COL32_A_SHIFT) & 0xFF));
  const ImVec2 d(b.x - a.x, b.y - a.y);
  const float len2 = std::max(1e-6f, d.x * d.x + d.y * d.y);
  for (ImDrawVert* v = v0; v < v1; ++v) {
    const float t = std::clamp(((v->pos.x - a.x) * d.x + (v->pos.y - a.y) * d.y) / len2, 0.0f, 1.0f);
    const ImU32 c = mix(c0, c1, t);
    const int oa = static_cast<int>((v->col >> IM_COL32_A_SHIFT) & 0xFF);
    const int ca = static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF);
    v->col = (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(ca * oa / maxA) << IM_COL32_A_SHIFT);
  }
}
void gradRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 top, ImU32 bottom, float r, ImDrawFlags f) {
  const int s = dl->VtxBuffer.Size;
  // KeepAlpha shading keeps the fill's alpha: fill with the top colour's alpha.
  dl->AddRectFilled(p0, p1, IM_COL32_WHITE, r, f);
  shadeV(dl, s, p0.y, p1.y, top, bottom);
}
void gradCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 top, ImU32 bottom) {
  const int s = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, r, IM_COL32_WHITE, segs(r));
  shadeV(dl, s, c.y - r, c.y + r, top, bottom);
}
void shadowRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, float spread, float dy, float strength) {
  if (strength <= 0) return;
  const int n = std::clamp(static_cast<int>(spread * 0.5f), 2, 3);
  for (int i = n; i >= 1; --i) {
    const float e = spread * static_cast<float>(i) / static_cast<float>(n);
    const float a = strength * 0.2f * (1.0f - static_cast<float>(i - 1) / static_cast<float>(n));
    dl->AddRectFilled(ImVec2(p0.x - e * 0.6f, p0.y - e * 0.35f + dy), ImVec2(p1.x + e * 0.6f, p1.y + e + dy), IM_COL32(0, 0, 0, static_cast<int>(a * 255)),
                      r + e);
  }
}
void shadowCircle(ImDrawList* dl, ImVec2 c, float r, float spread, float dy, float strength) {
  if (strength <= 0) return;
  const int n = std::clamp(static_cast<int>(spread * 0.5f), 2, 3);
  for (int i = n; i >= 1; --i) {
    const float e = spread * static_cast<float>(i) / static_cast<float>(n);
    const float a = strength * 0.21f * (1.0f - static_cast<float>(i - 1) / static_cast<float>(n));
    dl->AddCircleFilled(ImVec2(c.x, c.y + dy + e * 0.3f), r + e * 0.7f, IM_COL32(0, 0, 0, static_cast<int>(a * 255)), segs(r + e));
  }
}
void innerShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, float depth, float strength) {
  if (strength <= 0) return;
  dl->PushClipRect(p0, p1, true);
  const int n = std::clamp(static_cast<int>(depth), 2, 3);
  for (int i = 0; i < n; ++i) {
    const float k = static_cast<float>(i) / static_cast<float>(n);
    const float a = strength * 0.5f * (1.0f - k) * (1.0f - k);
    const float o = depth * k;
    dl->AddRect(ImVec2(p0.x - 0.5f + o * 0.35f, p0.y + 0.5f + o), ImVec2(p1.x + 0.5f - o * 0.35f, p1.y + depth * 2.0f), IM_COL32(0, 0, 0, static_cast<int>(a * 255)),
                r, 0, 1.0f + depth / static_cast<float>(n));
  }
  dl->PopClipRect();
}
void innerShadowCircle(ImDrawList* dl, ImVec2 c, float r, float depth, float strength) {
  if (strength <= 0) return;
  const int n = std::clamp(static_cast<int>(depth), 2, 3);
  for (int i = 0; i < n; ++i) {
    const float k = static_cast<float>(i) / static_cast<float>(n);
    const float a = strength * 0.5f * (1.0f - k) * (1.0f - k);
    const float o = depth * k;
    // Upper arc only: light comes from above.
    dl->PathArcTo(ImVec2(c.x, c.y + o * 0.6f), r - 0.6f - o * 0.4f, kPi * 1.05f, kPi * 1.95f, segs(r, kPi));
    dl->PathStroke(IM_COL32(0, 0, 0, static_cast<int>(a * 255)), 0, 1.0f + depth / static_cast<float>(n));
  }
}
void fillUnder(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 ct, ImU32 cb) {
  if (n < 2) return;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve((n - 1) * 6, (n - 1) * 4);
  for (int i = 0; i + 1 < n; ++i) {
    const ImDrawIdx v0 = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(pts[i], uv, ct);
    dl->PrimWriteVtx(pts[i + 1], uv, ct);
    dl->PrimWriteVtx(ImVec2(pts[i + 1].x, baseY), uv, cb);
    dl->PrimWriteVtx(ImVec2(pts[i].x, baseY), uv, cb);
    dl->PrimWriteIdx(v0), dl->PrimWriteIdx(static_cast<ImDrawIdx>(v0 + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(v0 + 2));
    dl->PrimWriteIdx(v0), dl->PrimWriteIdx(static_cast<ImDrawIdx>(v0 + 2)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(v0 + 3));
  }
}
void arc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th, bool roundCaps) {
  if (a1 < a0) std::swap(a0, a1);
  if (a1 - a0 < 0.0005f) return;
  dl->PathArcTo(c, r, a0, a1, std::max(3, segs(r, a1 - a0)));
  dl->PathStroke(col, 0, th);
  if (roundCaps && th >= 2.0f) {
    const float rr = th * 0.5f;
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r), rr, col, 12);
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r), rr, col, 12);
  }
}
void arcGrad(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 c0, ImU32 c1, float th) {
  if (a1 < a0) std::swap(a0, a1), std::swap(c0, c1);
  const float sweep = a1 - a0;
  if (sweep < 0.0005f) return;
  const int n = std::max(2, static_cast<int>(sweep / 0.06f));
  for (int i = 0; i < n; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(n), t1 = static_cast<float>(i + 1) / static_cast<float>(n);
    // A hair of overlap hides the seams between segments.
    dl->PathArcTo(c, r, a0 + sweep * t0 - (i ? 0.004f : 0.0f), a0 + sweep * t1, 3);
    dl->PathStroke(mix(c0, c1, (t0 + t1) * 0.5f), 0, th);
  }
  if (th >= 2.0f) {
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r), th * 0.5f, c0, 12);
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r), th * 0.5f, c1, 12);
  }
}
void glowArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th, float glow) {
  if (glow > 0) {
    arc(dl, c, r, a0, a1, alpha(col, 0.07f * glow), th * 4.2f);
    arc(dl, c, r, a0, a1, alpha(col, 0.12f * glow), th * 2.6f);
    arc(dl, c, r, a0, a1, alpha(col, 0.22f * glow), th * 1.7f);
  }
  arc(dl, c, r, a0, a1, col, th);
}
void glowLine(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float th, float glow) {
  if (n < 2) return;
  if (glow > 0) {
    dl->AddPolyline(pts, n, alpha(col, 0.06f * glow), th * 5.0f, 0);
    dl->AddPolyline(pts, n, alpha(col, 0.12f * glow), th * 2.8f, 0);
    dl->AddPolyline(pts, n, alpha(col, 0.22f * glow), th * 1.7f, 0);
  }
  dl->AddPolyline(pts, n, col, th, 0);
}
void glowDot(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float glow) {
  if (glow > 0) {
    dl->AddCircleFilled(c, r * 3.4f, alpha(col, 0.06f * glow), 24);
    dl->AddCircleFilled(c, r * 2.3f, alpha(col, 0.13f * glow), 20);
    dl->AddCircleFilled(c, r * 1.55f, alpha(col, 0.25f * glow), 16);
  }
  dl->AddCircleFilled(c, r, col, 16);
}

ImFont* font(Font f) {
  App& a = app();
  ImFont* r = nullptr;
  switch (f) {
    case Font::Body: r = a.fontRegular; break;
    case Font::Bold: r = a.fontBold; break;
    case Font::Mono: r = a.fontMono; break;
    case Font::MonoBold: r = a.fontMonoBold; break;
  }
  return r ? r : ImGui::GetFont();
}
ImVec2 textSize(Font f, float size, const char* s) { return font(f)->CalcTextSizeA(size, FLT_MAX, 0.0f, s); }
void text(ImDrawList* dl, Font f, float size, ImVec2 pos, ImU32 col, const char* s) {
  dl->AddText(font(f), size, ImVec2(std::round(pos.x), std::round(pos.y)), col, s);
}
void textIn(ImDrawList* dl, Font f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* s, int align) {
  const ImVec2 ts = textSize(f, size, s);
  float x = p0.x;
  if (align == 1) x = (p0.x + p1.x - ts.x) * 0.5f;
  else if (align == 2) x = p1.x - ts.x;
  text(dl, f, size, ImVec2(x, (p0.y + p1.y - ts.y) * 0.5f), col, s);
}
void caps(ImDrawList* dl, Font f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* s, float tracking, int align) {
  char up[128];
  size_t n = 0;
  for (; s[n] && n < sizeof(up) - 1; ++n) up[n] = (s[n] >= 'a' && s[n] <= 'z') ? static_cast<char>(s[n] - 32) : s[n];
  up[n] = 0;
  float w = 0;
  for (size_t i = 0; i < n; ++i) {
    char ch[2] = {up[i], 0};
    w += textSize(f, size, ch).x + (i + 1 < n ? tracking : 0);
  }
  float x = p0.x;
  if (align == 1) x = (p0.x + p1.x - w) * 0.5f;
  else if (align == 2) x = p1.x - w;
  const float y = (p0.y + p1.y - size) * 0.5f;
  for (size_t i = 0; i < n; ++i) {
    char ch[2] = {up[i], 0};
    text(dl, f, size, ImVec2(x, y), col, ch);
    x += textSize(f, size, ch).x + tracking;
  }
}

// ================================================================ base painter
ImU32 Painter::meterCol(float v) const {
  if (v > 0.94f) return tk.meterHigh;
  if (v > 0.78f) return mix(tk.meterMid, tk.meterHigh, (v - 0.78f) / 0.16f);
  if (v > 0.6f) return mix(tk.meterLow, tk.meterMid, (v - 0.6f) / 0.18f);
  return tk.meterLow;
}

void Painter::raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state) {
  const bool dis = state & Disabled;
  if (!(state & Active)) shadowRect(dl, p0, p1, r, 2.5f, 1.0f, tk.shadow * (dis ? 0.4f : 1.0f));
  ImU32 hi = tk.faceHi, lo = tk.faceLo;
  if (state & Hover) hi = lighten(hi, 0.07f), lo = lighten(lo, 0.05f);
  if (state & Active) std::swap(hi, lo), hi = darken(hi, 0.08f);
  if (dis) hi = alpha(hi, 0.5f), lo = alpha(lo, 0.5f);
  gradRect(dl, p0, p1, hi, lo, r);
  if (!(state & Active)) dl->AddLine(ImVec2(p0.x + r, p0.y + 0.5f), ImVec2(p1.x - r, p0.y + 0.5f), alpha(IM_COL32_WHITE, dis ? 0.04f : 0.12f), 1.0f);
  dl->AddRect(p0, p1, alpha(tk.faceLine, dis ? 0.5f : 1.0f), r, 0, 1.0f);
}
void Painter::hookFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role, bool rest) {
  const bool on = state & On, act = state & Active, hov = state & Hover;
  if (!rest && !on && !act && !hov) return;  // flat buttons have no face at rest
  const float rr = std::min(r, tk.radius);
  if (act) return sunken(dl, p0, p1, rr, mix(tk.field, tk.faceLo, 0.5f));
  if (on) {
    const ImU32 lit = roleOr(role, tk.hot);
    sunken(dl, p0, p1, rr, mix(tk.field, lit, hov ? 0.2f : 0.15f));
    dl->AddRect(p0, p1, alpha(lit, 0.4f), rr);
    return;
  }
  raised(dl, p0, p1, rr, state & (Hover | Disabled));
}
void Painter::sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill) {
  dl->AddRectFilled(p0, p1, fill, r);
  innerShadow(dl, p0, p1, r, 2.0f, tk.shadow * 0.8f);
  dl->AddLine(ImVec2(p0.x + r, p1.y - 0.5f), ImVec2(p1.x - r, p1.y - 0.5f), alpha(IM_COL32_WHITE, 0.07f), 1.0f);
}

void Painter::knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) {
  const bool dis = k.state & Disabled, lit = k.state & (Hover | Active);
  const float th = std::max(2.0f, r * 0.15f);
  const float rr = r - th * 0.5f;
  const ImU32 val = dis ? alpha(tk.textFaint, 0.6f) : roleOr(k.tint.value, tk.value);
  const float from = k.bipolar ? angleOf(0.5f) : kA0, an = angleOf(k.norm);
  shadowCircle(dl, c, r * 0.8f, r * 0.25f, r * 0.08f, tk.shadow);
  arc(dl, c, rr, kA0, kA1, tk.rail, th);
  if (std::fabs(an - from) > 0.002f) arc(dl, c, rr, std::min(from, an), std::max(from, an), (k.state & Active) ? lighten(val, 0.2f) : val, th);
  if (k.modDepth != 0) {
    const float m0 = angleOf(k.norm), m1 = angleOf(k.norm + k.modDepth);
    arc(dl, c, r + th * 0.9f, std::min(m0, m1), std::max(m0, m1), alpha(roleOr(k.tint.mod, tk.mod), 0.85f), std::max(1.5f, th * 0.45f));
  }
  if (k.modNow >= 0) {
    const float a = angleOf(k.modNow);
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr), th * 0.55f, roleOr(k.tint.mod, tk.mod), 12);
  }
  const float capR = r - th * 1.6f;
  ImU32 hi = tk.faceHi, lo = tk.faceLo;
  if (lit) hi = lighten(hi, 0.06f);
  shadowCircle(dl, c, capR, 2.0f, 1.0f, tk.shadow);
  gradCircle(dl, c, capR, hi, lo);
  dl->AddCircle(c, capR, tk.faceLine, segs(capR), 1.0f);
  if (k.level >= 0) dl->AddCircle(c, capR * 0.55f, alpha(tk.meterLow, 0.25f + 0.75f * k.level), segs(capR), 2.0f);
  const ImVec2 d(std::cos(an), std::sin(an));
  dl->AddLine(ImVec2(c.x + d.x * capR * 0.3f, c.y + d.y * capR * 0.3f), ImVec2(c.x + d.x * capR * 0.85f, c.y + d.y * capR * 0.85f), dis ? tk.textFaint : tk.text,
              std::max(1.5f, capR * 0.12f));
}

void Painter::slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s) {
  const bool v = s.vertical;
  const float th = 4.0f;
  // J-139: the slot's centre line on the pixel grid, so the slot, fill and handle share it.
  const ImVec2 c(v ? geom::sliderCentre(p0.x, p1.x, th) : (p0.x + p1.x) * 0.5f, v ? (p0.y + p1.y) * 0.5f : geom::sliderCentre(p0.y, p1.y, th));
  const ImVec2 a = v ? ImVec2(c.x - th * 0.5f, p0.y + 6) : ImVec2(p0.x + 6, c.y - th * 0.5f);
  const ImVec2 b = v ? ImVec2(c.x + th * 0.5f, p1.y - 6) : ImVec2(p1.x - 6, c.y + th * 0.5f);
  sunken(dl, a, b, th * 0.5f, tk.well);
  const float len = v ? b.y - a.y : b.x - a.x;
  auto pos = [&](float n) { return v ? b.y - len * n : a.x + len * n; };
  const float o = pos(s.bipolar ? 0.5f : 0.0f), q = pos(s.norm);
  const ImU32 val = (s.state & Disabled) ? tk.textFaint : roleOr(s.tint.value, tk.value);
  if (v) dl->AddRectFilled(ImVec2(a.x, std::min(o, q)), ImVec2(b.x, std::max(o, q)), val, th * 0.5f);
  else dl->AddRectFilled(ImVec2(std::min(o, q), a.y), ImVec2(std::max(o, q), b.y), val, th * 0.5f);
  if (s.modDepth != 0) {
    const float m = pos(s.norm + s.modDepth);
    if (v) dl->AddRectFilled(ImVec2(b.x + 2, std::min(q, m)), ImVec2(b.x + 4, std::max(q, m)), tk.mod);
    else dl->AddRectFilled(ImVec2(std::min(q, m), b.y + 2), ImVec2(std::max(q, m), b.y + 4), tk.mod);
  }
  const ImVec2 h0 = v ? ImVec2(c.x - 9, q - 4) : ImVec2(q - 4, c.y - 9), h1 = v ? ImVec2(c.x + 9, q + 4) : ImVec2(q + 4, c.y + 9);
  raised(dl, h0, h1, 2, s.state);
  if (s.modNow >= 0) {
    const float m = pos(s.modNow);
    dl->AddCircleFilled(v ? ImVec2(c.x, m) : ImVec2(m, c.y), 2.5f, tk.mod, 10);
  }
}

void Painter::fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f) {
  // J-139: cap, slot and ruler share one centre line (LookGeom.h); the cap used to sit 3 px left of the slot.
  const bool hasMeter = f.meterL >= 0;
  const geom::FaderGeom g = geom::fader(p0.x, p0.y, p1.x, p1.y, f.norm, geom::kSlateFader, hasMeter);
  const float tx = g.tx;
  // dB ruler (both sides of the slot; left only with a meter beside it). 0.8 = unity.
  const float marks[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
  const float ti = geom::kSlateFader.tickIn, to = g.tickOut(geom::kSlateFader.tickOut);
  for (float m : marks) {
    const float y = g.tickY(m);
    const ImU32 c = m == 0.8f ? tk.text : tk.textFaint;
    dl->AddLine(ImVec2(tx - to, y), ImVec2(tx - ti, y), c, 1.0f);
    if (g.ticksRight) dl->AddLine(ImVec2(tx + ti, y), ImVec2(tx + to, y), c, 1.0f);
  }
  sunken(dl, ImVec2(g.trackX0, g.y0), ImVec2(g.trackX1, g.y1), 2, tk.well);
  // Meters right of the track.
  if (hasMeter) {
    Meter m;
    m.l = f.meterL, m.r = f.meterR, m.peakL = f.peakL, m.peakR = f.peakR;
    meter(dl, ImVec2(tx + 9, g.y0), ImVec2(tx + 19, g.y1), m);
  }
  const ImVec2 h0(g.capX0, g.capY0), h1(g.capX1, g.capY1);
  raised(dl, h0, h1, 2, f.state);
  dl->AddLine(ImVec2(h0.x + 2, g.y), ImVec2(h1.x - 2, g.y), tk.text, 1.0f);
}

void Painter::meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) {
  const bool stereo = m.r >= 0;
  const float gap = 1.0f;
  const int nch = stereo ? 2 : 1;
  const float W = m.vertical ? p1.x - p0.x : p1.y - p0.y;
  const float cw = (W - gap * static_cast<float>(nch - 1)) / static_cast<float>(nch);
  for (int ch = 0; ch < nch; ++ch) {
    const float lv = std::clamp(ch ? m.r : m.l, 0.0f, 1.0f);
    const float pk = ch ? m.peakR : m.peakL;
    ImVec2 a, b;
    if (m.vertical) a = ImVec2(p0.x + (cw + gap) * static_cast<float>(ch), p0.y), b = ImVec2(a.x + cw, p1.y);
    else a = ImVec2(p0.x, p0.y + (cw + gap) * static_cast<float>(ch)), b = ImVec2(p1.x, a.y + cw);
    sunken(dl, a, b, 1.5f, tk.field);
    const float L = m.vertical ? b.y - a.y : b.x - a.x;
    if (m.reduction) {
      const float e = L * lv;
      if (m.vertical) dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 1 + e), tk.danger, 1);
      else dl->AddRectFilled(ImVec2(b.x - 1 - e, a.y + 1), ImVec2(b.x - 1, b.y - 1), tk.danger, 1);
      continue;
    }
    const int segN = std::max(4, static_cast<int>(L / 4.0f));
    for (int i = 0; i < segN; ++i) {
      const float t0 = static_cast<float>(i) / static_cast<float>(segN), t1 = static_cast<float>(i + 1) / static_cast<float>(segN);
      const bool onSeg = t1 <= lv + 0.5f / static_cast<float>(segN);
      const ImU32 col = onSeg ? meterCol(t1) : alpha(meterCol(t1), 0.08f);
      if (m.vertical) dl->AddRectFilled(ImVec2(a.x + 1, b.y - 1 - L * t1 + 1), ImVec2(b.x - 1, b.y - 1 - L * t0), col);
      else dl->AddRectFilled(ImVec2(a.x + 1 + L * t0, a.y + 1), ImVec2(a.x + L * t1, b.y - 1), col);
    }
    if (pk >= 0) {
      const float pp = L * std::clamp(pk, 0.0f, 1.0f);
      if (m.vertical) dl->AddRectFilled(ImVec2(a.x + 1, b.y - 1 - pp), ImVec2(b.x - 1, b.y - pp + 1), meterCol(pk));
      else dl->AddRectFilled(ImVec2(a.x + pp - 1, a.y + 1), ImVec2(a.x + pp + 1, b.y - 1), meterCol(pk));
    }
  }
  if (m.clip) {
    if (m.vertical) dl->AddRectFilled(ImVec2(p0.x, p0.y - 5), ImVec2(p1.x, p0.y - 2), tk.danger, 1);
    else dl->AddRectFilled(ImVec2(p1.x + 2, p0.y), ImVec2(p1.x + 5, p1.y), tk.danger, 1);
  }
}

void Painter::grNeedle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float grDb, float inDb, float outDb) {
  well(dl, p0, p1);
  const ImVec2 c((p0.x + p1.x) * 0.5f, p1.y - 10);
  const float r = std::min((p1.x - p0.x) * 0.42f, p1.y - p0.y - 30);
  const float a0 = kPi * 1.2f, a1 = kPi * 1.8f;
  arc(dl, c, r, a0, a1, tk.rail, 2.0f);
  for (int i = 0; i <= 6; ++i) {
    const float a = a0 + (a1 - a0) * static_cast<float>(i) / 6.0f;
    dl->AddLine(ImVec2(c.x + std::cos(a) * (r - 5), c.y + std::sin(a) * (r - 5)), ImVec2(c.x + std::cos(a) * (r + 1), c.y + std::sin(a) * (r + 1)), tk.textFaint, 1.0f);
  }
  const float t = std::clamp(-grDb / 24.0f, 0.0f, 1.0f);
  const float a = a1 - (a1 - a0) * t;
  arc(dl, c, r, a, a1, tk.danger, 2.0f);
  dl->AddLine(c, ImVec2(c.x + std::cos(a) * (r - 3), c.y + std::sin(a) * (r - 3)), tk.text, 1.5f);
  dl->AddCircleFilled(c, 3, tk.text, 10);
  char b[32];
  std::snprintf(b, sizeof b, "%.1f dB", grDb);
  textIn(dl, Font::Mono, 10, ImVec2(p0.x, p0.y + 3), ImVec2(p1.x, p0.y + 15), tk.textDim, b);
  (void)inDb, (void)outDb;
}

void Painter::led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) {
  const ImU32 col = roleOr(role, tk.led);
  innerShadowCircle(dl, c, r + 1, 1, tk.shadow);
  dl->AddCircleFilled(c, r + 1, darken(tk.field, 0.2f), 16);
  if (on || level > 0) {
    const float k = on ? 1.0f : level;
    glowDot(dl, c, r, alpha(col, 0.35f + 0.65f * k), tk.glow * k);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.35f), r * 0.35f, alpha(IM_COL32_WHITE, 0.5f * k), 10);
  } else {
    gradCircle(dl, c, r, mix(tk.field, col, 0.15f), mix(tk.field, col, 0.05f));
  }
  (void)state;
}

void Painter::button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) {
  raised(dl, p0, p1, tk.radius, state & ~On);
  const bool on = state & On;
  if (on) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), alpha(roleOr(role, tk.hot), 0.22f), tk.radius);
  const ImU32 tc = (state & Disabled) ? tk.textFaint : on ? roleOr(role, tk.hot) : tk.text;
  textIn(dl, Font::Body, 13, p0, ImVec2(p1.x, p1.y + ((state & Active) ? 2 : 0)), tc, lbl);
}
void Painter::iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role) {
  const bool on = state & On;
  if (state & (Hover | Active | On)) raised(dl, p0, p1, tk.radius, state & ~On);
  const ImU32 col = (state & Disabled) ? tk.textFaint : on ? roleOr(role, tk.hot) : (state & Hover) ? tk.text : tk.textDim;
  ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), (p1.y - p0.y) * 0.62f, col);
}
void Painter::toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role) {
  const float h = p1.y - p0.y, r = h * 0.5f;
  const ImU32 on = roleOr(role, tk.hot);
  sunken(dl, p0, p1, r, mix(tk.field, on, 0.75f * t));
  const ImVec2 c(p0.x + r + (p1.x - p0.x - h) * t, p0.y + r);
  shadowCircle(dl, c, r - 2, 2, 1, tk.shadow);
  gradCircle(dl, c, r - 2, (state & Hover) ? lighten(tk.faceHi, 0.1f) : tk.faceHi, tk.faceLo);
}
void Painter::check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, int state, ImU32 role) {
  const float s = p1.y - p0.y;
  const ImVec2 b0(p0.x, p0.y), b1(p0.x + s, p1.y);
  sunken(dl, b0, b1, tk.radiusSm, tk.field);
  if (state & On) {
    const ImU32 c = roleOr(role, tk.hot);
    dl->AddRectFilled(ImVec2(b0.x + 2, b0.y + 2), ImVec2(b1.x - 2, b1.y - 2), c, tk.radiusSm - 1);
    const ImVec2 q[3] = {ImVec2(b0.x + s * 0.27f, b0.y + s * 0.52f), ImVec2(b0.x + s * 0.43f, b0.y + s * 0.68f), ImVec2(b0.x + s * 0.74f, b0.y + s * 0.33f)};
    dl->AddPolyline(q, 3, darken(c, 0.75f), 1.8f, 0);
  }
  if (state & Hover) dl->AddRect(b0, b1, alpha(tk.text, 0.3f), tk.radiusSm);
  if (lbl) textIn(dl, Font::Body, 13, ImVec2(b1.x + 7, p0.y), p1, (state & Disabled) ? tk.textFaint : tk.text, lbl, 0);
}
void Painter::segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state) {
  sunken(dl, p0, p1, tk.radius, tk.field);
  const float w = (p1.x - p0.x) / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    const ImVec2 a(p0.x + w * static_cast<float>(i), p0.y), b(a.x + w, p1.y);
    if (i == sel) raised(dl, ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), tk.radiusSm, (state & Disabled) ? Disabled : 0);
    else if (i == hover) dl->AddRectFilled(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), alpha(IM_COL32_WHITE, 0.05f), tk.radiusSm);
    textIn(dl, Font::Body, 12, a, b, i == sel ? tk.hot : i == hover ? tk.text : tk.textDim, labels[i]);
  }
}
void Painter::tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover) {
  float x = p0.x;
  dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), tk.sectionLine, 1.0f);
  for (int i = 0; i < n; ++i) {
    const float w = textSize(Font::Body, 13, labels[i]).x + 24;
    const ImVec2 a(x, p0.y), b(x + w, p1.y);
    if (i == sel) {
      gradRect(dl, a, b, tk.faceHi, tk.section, tk.radius, ImDrawFlags_RoundCornersTop);
      dl->AddLine(ImVec2(a.x + 3, a.y + 1), ImVec2(b.x - 3, a.y + 1), tk.hot, 2.0f);
    }
    textIn(dl, Font::Body, 13, a, b, i == sel ? tk.text : i == hover ? tk.text : tk.textDim, labels[i]);
    x += w + 2;
  }
}
void Painter::dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state) {
  sunken(dl, p0, p1, tk.radius, (state & Hover) ? lighten(tk.field, 0.05f) : tk.field);
  if (state & (Active | Focus)) dl->AddRect(p0, p1, alpha(tk.hot, 0.8f), tk.radius, 0, 1.0f);
  textIn(dl, Font::Body, 13, ImVec2(p0.x + 8, p0.y), ImVec2(p1.x - 20, p1.y), (state & Disabled) ? tk.textFaint : tk.text, txt, 0);
  ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - 11, (p0.y + p1.y) * 0.5f), 10, tk.textDim);
}
void Painter::menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked) {
  shadowRect(dl, p0, p1, tk.radius, 10, 4, tk.shadow * 1.2f);
  dl->AddRectFilled(p0, p1, lighten(tk.section, 0.04f), tk.radius);
  dl->AddRect(p0, p1, tk.sectionLine, tk.radius);
  const float rh = (p1.y - p0.y - 8) / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    const ImVec2 a(p0.x + 4, p0.y + 4 + rh * static_cast<float>(i)), b(p1.x - 4, a.y + rh);
    if (i == hover) dl->AddRectFilled(a, b, alpha(tk.hot, 0.18f), tk.radiusSm);
    if (i == checked) ui::drawIcon(dl, ui::Icon::Check, ImVec2(a.x + 10, (a.y + b.y) * 0.5f), 10, tk.hot);
    textIn(dl, Font::Body, 13, ImVec2(a.x + 22, a.y), b, i == hover ? tk.text : tk.textDim, items[i], 0);
  }
}
void Painter::field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int caret, bool lcd) {
  sunken(dl, p0, p1, tk.radiusSm, lcd ? darken(tk.field, 0.3f) : tk.field);
  if (state & (Active | Focus)) dl->AddRect(p0, p1, alpha(tk.hot, 0.8f), tk.radiusSm, 0, 1.0f);
  const ImU32 tc = (state & Disabled) ? tk.textFaint : lcd ? tk.fieldText : tk.text;
  const float fs = lcd ? std::min(18.0f, (p1.y - p0.y) * 0.62f) : 13;
  textIn(dl, lcd ? Font::MonoBold : Font::Mono, fs, p0, p1, tc, txt);
  if (caret >= 0) {
    const ImVec2 ts = textSize(lcd ? Font::MonoBold : Font::Mono, fs, txt);
    char pre[128];
    std::snprintf(pre, sizeof pre, "%.*s", caret, txt);
    const float x = (p0.x + p1.x - ts.x) * 0.5f + textSize(lcd ? Font::MonoBold : Font::Mono, fs, pre).x + 1;
    dl->AddLine(ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4), tk.hot, 1.5f);
  }
}
void Painter::stepper(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* txt, int state, int hoverSide) {
  field(dl, p0, p1, txt, state & ~(Active | Focus), -1, false);
  const float h = p1.y - p0.y;
  ui::drawIcon(dl, ui::Icon::ChevronLeft, ImVec2(p0.x + h * 0.5f, (p0.y + p1.y) * 0.5f), 10, hoverSide < 0 ? tk.hot : tk.textDim);
  ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(p1.x - h * 0.5f, (p0.y + p1.y) * 0.5f), 10, hoverSide > 0 ? tk.hot : tk.textDim);
}
void Painter::stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint) {
  const ImU32 base = alt ? mix(tk.faceLo, tk.danger, 0.10f) : tk.faceLo;
  if (on) {
    const ImU32 c = roleOr(tint, tk.text);
    shadowRect(dl, p0, p1, tk.radiusSm, 2, 1, tk.shadow);
    gradRect(dl, p0, p1, lighten(c, 0.1f), mix(c, base, 0.25f + 0.4f * (1.0f - vel)), tk.radiusSm);
  } else {
    gradRect(dl, p0, p1, lighten(base, (state & Hover) ? 0.12f : 0.05f), base, tk.radiusSm);
  }
  dl->AddRect(p0, p1, tk.faceLine, tk.radiusSm);
  // J-137: highlights stay inside the pad (a step viewport or a grid's clip never cuts them, neighbours never cover them).
  if (playing) dl->AddRect(ImVec2(p0.x + 0.75f, p0.y + 0.75f), ImVec2(p1.x - 0.75f, p1.y - 0.75f), tk.hot, tk.radiusSm, 0, 1.5f);
}
void Painter::pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float glow, int state, ImU32 tint) {
  raised(dl, p0, p1, tk.radius, state);
  const ImU32 c = roleOr(tint, tk.hot);
  if (glow > 0) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), alpha(c, 0.55f * glow), tk.radius);
  textIn(dl, Font::Body, 12, ImVec2(p0.x + 6, p1.y - 18), ImVec2(p1.x - 6, p1.y - 4), tk.textDim, lbl, 0);
}
void Painter::panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  gradRect(dl, p0, p1, tk.panelTop, tk.panelBot, tk.radiusLg);
  dl->AddRect(p0, p1, tk.panelLine, tk.radiusLg);
}
void Painter::section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title) {
  dl->AddRectFilled(p0, p1, tk.section, tk.radius);
  dl->AddRect(p0, p1, tk.sectionLine, tk.radius);
  if (title) caps(dl, Font::MonoBold, 10, ImVec2(p0.x + 8, p0.y + 4), ImVec2(p1.x, p0.y + 18), tk.sectionTitle, title, 0.8f, 0);
}
void Painter::header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub) {
  gradRect(dl, p0, p1, lighten(tk.panelTop, 0.05f), tk.panelTop, tk.radiusLg, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), tk.sectionLine, 1.0f);
  text(dl, Font::Bold, 15, ImVec2(p0.x + 12, (p0.y + p1.y) * 0.5f - 9), tk.text, title);
  if (sub) textIn(dl, Font::Body, 12, ImVec2(p0.x, p0.y), ImVec2(p1.x - 12, p1.y), tk.textDim, sub, 2);
}
void Painter::well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) { sunken(dl, p0, p1, tk.radius, tk.well); }
void Painter::caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) {
  dl->AddRectFilled(p0, p1, tk.field, (p1.y - p0.y) * 0.5f);
  caps(dl, Font::MonoBold, 9.5f, p0, p1, tk.textDim, t, 0.6f);
}
void Painter::tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value) {
  const ImVec2 ts = textSize(Font::Body, 12, title), vs = textSize(Font::MonoBold, 13, value);
  const ImVec2 p1(pos.x + std::max(ts.x, vs.x) + 20, pos.y + 42);
  shadowRect(dl, pos, p1, tk.radius, 8, 3, tk.shadow * 1.2f);
  dl->AddRectFilled(pos, p1, lighten(tk.section, 0.06f), tk.radius);
  dl->AddRect(pos, p1, tk.sectionLine, tk.radius);
  text(dl, Font::Body, 12, ImVec2(pos.x + 10, pos.y + 6), tk.textDim, title);
  text(dl, Font::MonoBold, 13, ImVec2(pos.x + 10, pos.y + 22), tk.text, value);
}
void Painter::scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state) {
  const bool v = (p1.y - p0.y) > (p1.x - p0.x);
  const float L = v ? p1.y - p0.y : p1.x - p0.x;
  const float a = L * pos * (1 - size), b = a + L * size;
  const ImU32 c = (state & Active) ? tk.text : (state & Hover) ? tk.textDim : tk.textFaint;
  dl->AddRectFilled(p0, p1, alpha(tk.field, 0.5f), 3);
  if (v) dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + a + 2), ImVec2(p1.x - 2, p0.y + b - 2), alpha(c, 0.7f), 3);
  else dl->AddRectFilled(ImVec2(p0.x + a + 2, p0.y + 2), ImVec2(p0.x + b - 2, p1.y - 2), alpha(c, 0.7f), 3);
}
void Painter::label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, bool dim) {
  textIn(dl, Font::Body, 12, p0, p1, dim ? tk.textDim : tk.text, t);
}
void Painter::valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int state) {
  textIn(dl, Font::Mono, 11, p0, p1, (state & Active) ? tk.hot : tk.textDim, t);
}
void Painter::graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows) {
  for (int i = 1; i < cols; ++i) {
    const float x = std::round(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(cols)) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), i * 2 == cols ? tk.gridStrong : tk.grid, 1.0f);
  }
  for (int i = 1; i < rows; ++i) {
    const float y = std::round(p0.y + (p1.y - p0.y) * static_cast<float>(i) / static_cast<float>(rows)) + 0.5f;
    dl->AddLine(ImVec2(p0.x + 1, y), ImVec2(p1.x - 1, y), i * 2 == rows ? tk.gridStrong : tk.grid, 1.0f);
  }
}
void Painter::curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill) {
  if (n < 2) return;
  const ImU32 c = roleOr(col, tk.value);
  if (fill) fillUnder(dl, pts, n, baseY, alpha(c, 0.28f), alpha(c, 0.0f));
  glowLine(dl, pts, n, c, 1.8f, tk.glow);
}
void Painter::envelope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ImVec2* pts, int n, int sel, float playX, float playY) {
  well(dl, p0, p1);
  graphGrid(dl, p0, p1, 8, 4);
  curve(dl, pts, n, p1.y - 1, 0, true);
  for (int i = 0; i < n; ++i) {
    const bool s = i == sel;
    dl->AddCircleFilled(pts[i], s ? 5.0f : 3.5f, s ? tk.hot : tk.well, 12);
    dl->AddCircle(pts[i], s ? 5.0f : 3.5f, s ? tk.hot : tk.value, 12, 1.5f);
  }
  if (playX >= 0) {
    dl->AddLine(ImVec2(playX, p0.y + 2), ImVec2(playX, p1.y - 2), alpha(tk.hot, 0.5f), 1.0f);
    glowDot(dl, ImVec2(playX, playY), 3.5f, tk.hot, std::max(0.5f, tk.glow));
  }
}
namespace {
float lfoShape(int shape, float ph) {
  ph -= std::floor(ph);
  switch (shape) {
    case 0: return std::sin(ph * 2 * kPi);
    case 1: return ph < 0.5f ? ph * 4 - 1 : 3 - ph * 4;
    case 2: return ph < 0.5f ? 1.0f : -1.0f;
    default: return 1 - ph * 2;
  }
}
}  // namespace
void Painter::lfo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, float phase, float depth) {
  well(dl, p0, p1);
  graphGrid(dl, p0, p1, 4, 2);
  const float cy = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.4f * depth;
  ImVec2 pts[97];
  for (int i = 0; i <= 96; ++i) {
    const float t = static_cast<float>(i) / 96.0f;
    pts[i] = ImVec2(p0.x + 4 + (p1.x - p0.x - 8) * t, cy - lfoShape(shape, t) * h);
  }
  curve(dl, pts, 97, cy, 0, true);
  const ImVec2 q(p0.x + 4 + (p1.x - p0.x - 8) * phase, cy - lfoShape(shape, phase) * h);
  dl->AddLine(ImVec2(q.x, p0.y + 2), ImVec2(q.x, p1.y - 2), alpha(tk.mod, 0.35f), 1.0f);
  glowDot(dl, q, 3.5f, tk.mod, std::max(0.6f, tk.glow));
}
void Painter::scope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* s, int n) {
  well(dl, p0, p1);
  graphGrid(dl, p0, p1, 8, 4);
  ImVec2 pts[512];
  n = std::min(n, 512);
  const float cy = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.45f;
  for (int i = 0; i < n; ++i) pts[i] = ImVec2(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(n - 1), cy - s[i] * h);
  glowLine(dl, pts, n, tk.value, 1.5f, tk.glow);
}
void Painter::spectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* db, int n, float peakLine) {
  well(dl, p0, p1);
  graphGrid(dl, p0, p1, 10, 4);
  ImVec2 pts[512];
  n = std::min(n, 512);
  for (int i = 0; i < n; ++i)
    pts[i] = ImVec2(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(n - 1), p0.y + (p1.y - p0.y) * std::clamp(-db[i] / 90.0f, 0.0f, 1.0f));
  curve(dl, pts, n, p1.y - 1, 0, true);
  (void)peakLine;
}
void Painter::waveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* mn, const float* mx, int n, float playPos, float selA, float selB) {
  well(dl, p0, p1);
  const float cy = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.45f;
  if (selB > selA) dl->AddRectFilled(ImVec2(p0.x + (p1.x - p0.x) * selA, p0.y + 1), ImVec2(p0.x + (p1.x - p0.x) * selB, p1.y - 1), alpha(tk.hot, 0.14f));
  dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), tk.grid, 1.0f);
  for (int i = 0; i < n; ++i) {
    const float x = p0.x + 1 + (p1.x - p0.x - 2) * static_cast<float>(i) / static_cast<float>(n);
    dl->AddLine(ImVec2(x, cy - mx[i] * h), ImVec2(x, cy - mn[i] * h + 1), alpha(tk.value, 0.85f), 1.0f);
  }
  if (playPos >= 0) {
    const float x = p0.x + (p1.x - p0.x) * playPos;
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), tk.hot, 1.5f);
  }
}
void Painter::wavetable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int frames, float current, float morph) {
  well(dl, p0, p1);
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  const float dx = W * 0.22f / static_cast<float>(frames), dy = H * 0.42f / static_cast<float>(frames);
  for (int f = frames - 1; f >= 0; --f) {
    const float ft = static_cast<float>(f) / static_cast<float>(frames - 1);
    const float ox = p0.x + 10 + dx * static_cast<float>(f), oy = p1.y - 16 - dy * static_cast<float>(f);
    ImVec2 pts[65];
    for (int i = 0; i <= 64; ++i) {
      const float t = static_cast<float>(i) / 64.0f;
      const float v = std::sin(t * 2 * kPi) * (1 - ft * morph) + std::sin(t * 6 * kPi) * 0.33f * ft * morph;
      pts[i] = ImVec2(ox + (W * 0.7f) * t, oy - v * H * 0.2f);
    }
    const bool cur = std::fabs(static_cast<float>(f) - current) < 0.5f;
    if (cur) glowLine(dl, pts, 65, tk.value, 2.0f, tk.glow);
    else dl->AddPolyline(pts, 65, alpha(tk.value, 0.08f + 0.18f * (1 - ft)), 1.0f, 0);
  }
}
void Painter::xyPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float x, float y, float mx, float my, int state) {
  well(dl, p0, p1);
  graphGrid(dl, p0, p1, 4, 4);
  const ImVec2 q(p0.x + (p1.x - p0.x) * x, p1.y - (p1.y - p0.y) * y);
  dl->AddLine(ImVec2(q.x, p0.y + 1), ImVec2(q.x, p1.y - 1), alpha(tk.value, 0.3f), 1.0f);
  dl->AddLine(ImVec2(p0.x + 1, q.y), ImVec2(p1.x - 1, q.y), alpha(tk.value, 0.3f), 1.0f);
  if (mx >= 0) {
    const ImVec2 m(p0.x + (p1.x - p0.x) * mx, p1.y - (p1.y - p0.y) * my);
    dl->AddLine(q, m, alpha(tk.mod, 0.5f), 1.0f);
    glowDot(dl, m, 3, tk.mod, std::max(0.5f, tk.glow));
  }
  glowDot(dl, q, (state & Active) ? 6.0f : 5.0f, tk.value, tk.glow);
}
void Painter::keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi) {
  static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  int whites = 0;
  for (int n = lo; n <= hi; ++n) whites += kBlack[n % 12] ? 0 : 1;
  const float ww = (p1.x - p0.x) / static_cast<float>(whites);
  float x = p0.x;
  for (int n = lo; n <= hi; ++n) {
    if (kBlack[n % 12]) continue;
    const ImVec2 a(x, p0.y), b(x + ww - 1, p1.y);
    ImU32 top = rgb(0xE9ECEE), bot = rgb(0xC9CED2);
    if (n >= rangeLo && n <= rangeHi) top = mix(top, tk.value, 0.18f), bot = mix(bot, tk.value, 0.18f);
    if (held && held[n]) top = mix(top, tk.hot, 0.65f), bot = mix(bot, tk.hot, 0.8f);
    gradRect(dl, a, b, top, bot, 2, ImDrawFlags_RoundCornersBottom);
    x += ww;
  }
  x = p0.x;
  for (int n = lo; n <= hi; ++n) {
    if (kBlack[n % 12]) {
      const ImVec2 a(x - ww * 0.32f, p0.y), b(x + ww * 0.32f - 1, p0.y + (p1.y - p0.y) * 0.6f);
      ImU32 top = rgb(0x3A3F43), bot = rgb(0x16191B);
      if (held && held[n]) top = tk.hot, bot = darken(tk.hot, 0.3f);
      shadowRect(dl, a, b, 2, 2, 1, tk.shadow);
      gradRect(dl, a, b, top, bot, 2, ImDrawFlags_RoundCornersBottom);
    } else {
      x += ww;
    }
  }
}
void Painter::modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* lbl, float live, int state) {
  raised(dl, p0, p1, (p1.y - p0.y) * 0.5f, state);
  const float h = p1.y - p0.y;
  led(dl, ImVec2(p0.x + h * 0.5f, (p0.y + p1.y) * 0.5f), h * 0.18f, false, live, tk.mod, 0);
  textIn(dl, Font::Body, 12, ImVec2(p0.x + h, p0.y), ImVec2(p1.x - 6, p1.y), tk.text, lbl, 0);
}
void Painter::modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int state) {
  sunken(dl, p0, p1, 2, tk.field);
  const float cx = (p0.x + p1.x) * 0.5f, w = (p1.x - p0.x) * 0.5f - 2;
  const float e = cx + w * amount;
  dl->AddRectFilled(ImVec2(std::min(cx, e), p0.y + 2), ImVec2(std::max(cx, e), p1.y - 2), (state & Hover) ? lighten(tk.mod, 0.2f) : tk.mod, 1);
  dl->AddLine(ImVec2(cx, p0.y + 1), ImVec2(cx, p1.y - 1), tk.textFaint, 1.0f);
  if (live > -2) {
    const float lx = cx + w * live * amount;
    dl->AddLine(ImVec2(lx, p0.y), ImVec2(lx, p1.y), tk.text, 1.5f);
  }
}

// ================================================================ registry / entry points
namespace {
// Painters are built from the palette in force (the app theme, or a plugin
// skin's palette pushed while its editor draws). A few palettes alternate
// within one frame, so the built painters are kept per palette: built once,
// then reused (no allocation per frame).
struct PainterSet {
  uint64_t key = 0;
  uint64_t used = 0;
  std::unique_ptr<Painter> ps[static_cast<int>(Dir::Count)];
};
uint64_t paletteKey(const theme::Palette& P) {
  const ImU32 roles[] = {P.surface950, P.surface900, P.surface800, P.surface700, P.surface600, P.line,      P.lineStrong,
                         P.text,       P.textDim,    P.textFaint,  P.paper,      P.accent,     P.danger,    P.meterLow,
                         P.meterMid,   P.meterHigh,  P.hot,        P.led,        P.well,       P.field,     P.knobFill};
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (ImU32 c : roles) h = (h ^ c) * 1099511628211ull;
  return (h ^ (P.dark ? 1u : 0u)) * 1099511628211ull;
}
}  // namespace

Painter& painter(Dir d) {
  static PainterSet sets[6];
  static uint64_t tick = 0;
  const uint64_t key = paletteKey(theme::pal());
  PainterSet* set = nullptr;
  for (auto& s : sets)
    if (s.used && s.key == key) set = &s;
  if (!set) {  // a palette not seen yet: take the least recently used slot
    set = &sets[0];
    for (auto& s : sets)
      if (s.used < set->used) set = &s;
    for (auto& p : set->ps) p.reset();
    set->key = key;
  }
  set->used = ++tick;
  const int i = std::clamp(static_cast<int>(d), 0, static_cast<int>(Dir::Count) - 1);
  std::unique_ptr<Painter>& p = set->ps[i];
  if (!p) {
    switch (static_cast<Dir>(i)) {
      case Dir::Graphite: p.reset(makeGraphite()); break;
      case Dir::Hairline: p.reset(makeHairline()); break;
      case Dir::Lumen: p.reset(makeLumen()); break;
      case Dir::Slate: p.reset(makeSlate()); break;
      default: p.reset(makeClassic()); break;
    }
  }
  return *p;
}
namespace {
Painter& cur() { return painter(current()); }
// Surfaces (panels, sections, wells, faces, fields) handed over by the shared
// drawers: in the light theme every style keeps the caller's own colours with
// Slate's finish, so the dark materials stay on the controls.
Painter& surf() { return theme::isDark() ? cur() : painter(Dir::Slate); }
}  // namespace

void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k) { cur().knob(dl, c, r, k); }
void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s) { cur().slider(dl, p0, p1, s); }
void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f) { cur().fader(dl, p0, p1, f); }
void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m) { cur().meter(dl, p0, p1, m); }
void grNeedle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float g, float i, float o) { cur().grNeedle(dl, p0, p1, g, i, o); }
void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state) { cur().led(dl, c, r, on, level, role, state); }
void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* l, int s, ImU32 role) { cur().button(dl, p0, p1, l, s, role); }
void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int s, ImU32 role) { cur().iconButton(dl, p0, p1, icon, s, role); }
void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int s, ImU32 role) { cur().toggle(dl, p0, p1, t, s, role); }
void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* l, int s, ImU32 role) { cur().check(dl, p0, p1, l, s, role); }
void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* l, int n, int sel, int hover, int s) {
  cur().segmented(dl, p0, p1, l, n, sel, hover, s);
}
void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* l, int n, int sel, int hover) { cur().tabs(dl, p0, p1, l, n, sel, hover); }
void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int s) { cur().dropdown(dl, p0, p1, t, s); }
void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked) { cur().menu(dl, p0, p1, items, n, hover, checked); }
void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int s, int caret, bool lcd) { cur().field(dl, p0, p1, t, s, caret, lcd); }
void stepper(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int s, int side) { cur().stepper(dl, p0, p1, t, s, side); }
void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int s, ImU32 tint) {
  cur().stepPad(dl, p0, p1, on, alt, vel, playing, s, tint);
}
void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* l, float glow, int s, ImU32 tint) { cur().pad(dl, p0, p1, l, glow, s, tint); }
void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) { cur().panel(dl, p0, p1); }
void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) { cur().section(dl, p0, p1, t); }
void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, const char* sub) { cur().header(dl, p0, p1, t, sub); }
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) { cur().well(dl, p0, p1); }
void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t) { cur().caption(dl, p0, p1, t); }
void tooltip(ImDrawList* dl, ImVec2 pos, const char* t, const char* v) { cur().tooltip(dl, pos, t, v); }
void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int s) { cur().scrollbar(dl, p0, p1, pos, size, s); }
void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, bool dim) { cur().label(dl, p0, p1, t, dim); }
void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* t, int s) { cur().valueText(dl, p0, p1, t, s); }
void graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int c, int r) { cur().graphGrid(dl, p0, p1, c, r); }
void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill) { cur().curve(dl, pts, n, baseY, col, fill); }
void envelope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ImVec2* pts, int n, int sel, float px, float py) { cur().envelope(dl, p0, p1, pts, n, sel, px, py); }
void lfo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, float phase, float depth) { cur().lfo(dl, p0, p1, shape, phase, depth); }
void scope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* s, int n) { cur().scope(dl, p0, p1, s, n); }
void spectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* db, int n, float pk) { cur().spectrum(dl, p0, p1, db, n, pk); }
void waveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* mn, const float* mx, int n, float pp, float a, float b) {
  cur().waveform(dl, p0, p1, mn, mx, n, pp, a, b);
}
void wavetable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int frames, float current, float morph) { cur().wavetable(dl, p0, p1, frames, current, morph); }
void xyPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float x, float y, float mx, float my, int s) { cur().xyPad(dl, p0, p1, x, y, mx, my, s); }
void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int a, int b) { cur().keyboard(dl, p0, p1, lo, hi, held, a, b); }
void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* l, float live, int s) { cur().modChip(dl, p0, p1, l, live, s); }
void modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int s) { cur().modAmount(dl, p0, p1, amount, live, s); }
void hookPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { surf().hookPanel(dl, p0, p1, hint); }
void hookSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { surf().hookSection(dl, p0, p1, hint); }
void hookWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { surf().hookWell(dl, p0, p1, hint); }
void hookRaised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint, int state) { surf().hookRaised(dl, p0, p1, r, hint, state); }
void hookInset(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint) { surf().hookInset(dl, p0, p1, r, hint); }
ImU32 hookTint(ImU32 fill) { return cur().hookTint(fill); }
void hookFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role, bool rest) { surf().hookFace(dl, p0, p1, r, state, role, rest); }

}  // namespace gl::look
