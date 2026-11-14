// Gridline — "plasma" pack: the editor's look (see plasma_ui.h, "look"). Live pictures in wells
// (a curve with a soft fill and a low glow, a lit dot), section heads, the parts' colours from the
// theme's roles, and the small controls of the bars (segments, buttons, chips, value fields).
// Sizes are given in design pixels (a 1440 x 900 window) and drawn on the 1280 x 800 form.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "../../app/Theme.h"
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

float glowK(const Ctx& c) { return c.P.dark ? 0.6f : 0.22f; }

ImU32 partCol(const Ctx& c, int id) {
  uint32_t r = theme::role::kMenu;
  bool tex = false;
  if (id == 1) r = theme::role::kDelete;
  else if (id == 2) r = theme::role::kMute;
  else if (id == 3) r = theme::role::kSelect;
  else if (id == 4) r = theme::role::kSelect, tex = true;
  else if (id == 10) r = theme::role::kGraph;
  else if (id == 11) r = theme::role::kSlice;
  else if (id == 20) r = theme::role::kSnap;
  else if (id >= 100 && id < 200) return c.P.fam[FamLfo];
  else if (id >= 200 && id < 300) return c.P.fam[FamEnv];
  else if (id == 300) return c.P.fam[FamMacro];
  else if (id == 310) return c.P.fam[FamKey];
  else if (id == 320) return c.P.fam[FamChance];
  else if (id >= 400) r = theme::role::kMenu;  // effects
  ImU32 col = c.P.dark ? theme::rgb(r) : theme::lit(theme::rgb(r));
  if (tex) col = theme::mix(col, c.P.text, c.P.dark ? 0.45f : 0.35f);
  return col;
}

ImU32 fxCol(const Ctx& c, int type) {
  uint32_t r = theme::role::kMenu;  // tone
  switch (type) {
    case FxChorus: case FxFlanger: case FxPhaser: case FxHyper: case FxBode: r = theme::role::kWarp; break;
    case FxDelay: case FxReverb: case FxConvolve: r = theme::role::kPaint; break;
    case FxComp: case FxUtility: r = theme::role::kPlay; break;
    case FxSplitLH: case FxSplitLMH: case FxSplitMS: r = theme::role::kSlice; break;
    default: break;
  }
  return c.P.dark ? theme::rgb(r) : theme::lit(theme::rgb(r));
}

ImU32 barBg(const Ctx& c) { return theme::mix(c.P.frame, c.P.well, c.P.dark ? 0.35f : 0.18f); }

void wellLook(const Ctx& c, float x, float y, float w, float h) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.well, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(4)));
}

void gridLook(const Ctx& c, float x, float y, float w, float h, int nx, int ny, bool mid) {
  const ImU32 g = theme::withAlpha(c.P.text, c.P.dark ? 0.045f : 0.06f);
  for (int i = 1; i < nx; ++i) vline(c, x + w * i / nx, y, y + h, g);
  for (int j = 1; j < ny; ++j) hline(c, x, x + w, y + h * j / ny, g);
  if (mid) hline(c, x, x + w, y + h * 0.5f, theme::withAlpha(c.P.text, c.P.dark ? 0.08f : 0.11f));
}

void glowLine(const Ctx& c, const std::vector<ImVec2>& pts, ImU32 col, float lw, float glow) {
  if (pts.size() < 2) return;
  const float g = glowK(c) * glow, w = std::max(1.0f, c.s(lw));
  if (g > 0.01f) {
    c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(col, 0.07f * g), 0, w + c.s(7) * g);
    c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(col, 0.14f * g), 0, w + c.s(3.5f) * g);
  }
  c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, w);
}

void fillUnder(const Ctx& c, const std::vector<ImVec2>& pts, float baseY, ImU32 col, float a0, float a1, float topY) {
  if (pts.size() < 2) return;
  ImDrawList* dl = c.dl;
  const ImVec2 uv = dl->_Data->TexUvWhitePixel;
  const float span = std::max(1.0f, baseY - topY);
  auto colAt = [&](float y) {
    const float t = std::clamp((y - topY) / span, 0.f, 1.f);
    return theme::withAlpha(col, a0 + (a1 - a0) * t);
  };
  const int n = static_cast<int>(pts.size()) - 1;
  dl->PrimReserve(n * 6, n * 4);
  for (int i = 0; i < n; ++i) {
    const ImVec2 p0 = pts[static_cast<size_t>(i)], p1 = pts[static_cast<size_t>(i) + 1];
    const ImDrawIdx b = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(p0, uv, colAt(p0.y));
    dl->PrimWriteVtx(p1, uv, colAt(p1.y));
    dl->PrimWriteVtx(ImVec2(p1.x, baseY), uv, colAt(baseY));
    dl->PrimWriteVtx(ImVec2(p0.x, baseY), uv, colAt(baseY));
    dl->PrimWriteIdx(b), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 2));
    dl->PrimWriteIdx(b), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 2)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 3));
  }
}

void glowDot(const Ctx& c, ImVec2 at, float r, ImU32 col) {
  const float g = glowK(c);
  if (g > 0.01f) c.dl->AddCircleFilled(at, c.s(r) + c.s(4) * g, theme::withAlpha(col, 0.16f * g), 20);
  c.dl->AddCircleFilled(at, c.s(r), col, 20);
  c.dl->AddCircleFilled(at, c.s(r) * 0.45f, c.P.dark ? IM_COL32(255, 255, 255, 216) : IM_COL32(255, 255, 255, 255), 12);
}

void capsText(const Ctx& c, float x, float y, const char* s, ImU32 col, float px, float track) {
  MonoText mono;
  float cx = x;
  char ch[2] = {0, 0};
  for (const char* p = s; *p; ++p) {
    ch[0] = static_cast<char>(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    text(c, cx, y, ch, col, px, true);
    cx += textW(c, ch, px, true) + track;
  }
}
float capsW(const Ctx& c, const char* s, float px, float track) {
  MonoText mono;
  float w = 0;
  char ch[2] = {0, 0};
  for (const char* p = s; *p; ++p) {
    ch[0] = static_cast<char>(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    w += textW(c, ch, px, true) + track;
  }
  return w;
}

float sectionHead(const Ctx& c, float x, float y, float w, const char* title, const char* hint, const char* right) {
  const float h = M(22), cy = y + h * 0.5f;
  const float tp = T(10.5f);
  capsText(c, x, cy - tp * 0.68f, title, c.P.label, tp, M(1.0f));
  const float tw = capsW(c, title, tp, M(1.0f));
  float rx = x + w;
  if (right && *right) {
    const float rw = textW(c, right, T(11), false);
    text(c, rx - rw, cy - T(11) * 0.68f, right, c.P.textFaint, T(11), false);
    rx -= rw + M(10);
  }
  if (hint && *hint && textW(c, hint, T(11.5f), false) < rx - (x + tw + M(10)) - M(8))
    text(c, x + tw + M(10), cy - T(11.5f) * 0.68f, hint, c.P.textFaint, T(11.5f), false);
  return rx;
}

int segPick(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<const char*>& labels, int cur, bool mono, float px) {
  ImGui::PushID(id);
  const int n = static_cast<int>(labels.size());
  layoutBox(c, x, y, w, h, id);
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(4)));
  const float pad = h > M(24) ? M(2) : M(1), gap = M(2);
  float tot = 0;
  std::vector<float> ws(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (mono) {
      MonoText m;
      ws[static_cast<size_t>(i)] = textW(c, labels[static_cast<size_t>(i)], px, false);
    } else {
      ws[static_cast<size_t>(i)] = textW(c, labels[static_cast<size_t>(i)], px, false);
    }
    tot += ws[static_cast<size_t>(i)];
  }
  const float spare = std::max(0.f, (w - pad * 2 - gap * (n - 1)) - tot) / std::max(1, n);
  int hit = -1;
  float sx = x + pad;
  for (int i = 0; i < n; ++i) {
    const float sw = ws[static_cast<size_t>(i)] + spare;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(c.p(sx, y + pad));
    if (ImGui::InvisibleButton("##sg", c.F.sz(sw, h - pad * 2))) hit = i;
    const bool hov = ImGui::IsItemHovered(), on = i == cur;
    if (on) {
      c.dl->AddRectFilled(c.p(sx, y + pad), c.p(sx + sw, y + h - pad), c.P.accentSoft, c.s(M(3)));
      c.dl->AddRect(c.p(sx, y + pad), c.p(sx + sw, y + h - pad), c.P.accent, c.s(M(3)));
    } else if (hov) {
      c.dl->AddRectFilled(c.p(sx, y + pad), c.p(sx + sw, y + h - pad), c.P.panelHi, c.s(M(3)));
    }
    {
      std::unique_ptr<MonoText> m;
      if (mono) m = std::make_unique<MonoText>();
      textMid(c, sx, sx + sw, y + h * 0.5f, labels[static_cast<size_t>(i)], on ? c.P.accentText : (hov ? c.P.text : c.P.label), px, false);
    }
    ImGui::PopID();
    sx += sw + gap;
  }
  ImGui::PopID();
  return hit;
}

bool btnLook(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool quiet, bool on, float px) {
  layoutBox(c, x, y, w, h, label && *label ? label : id);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##b", c.F.sz(w, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  if (on) {
    c.dl->AddRectFilled(a, b, c.P.accentSoft, c.s(M(4)));
    c.dl->AddRect(a, b, c.P.accent, c.s(M(4)));
  } else if (!quiet || hov) {
    c.dl->AddRectFilled(a, b, act ? c.P.panelLo : (hov ? c.P.headOn : c.P.panelHi), c.s(M(4)));
    c.dl->AddRect(a, b, hov ? c.P.lineHi : c.P.line, c.s(M(4)));
  }
  if (label && *label) textMid(c, x, x + w, y + h * 0.5f, label, on ? c.P.accentText : (quiet && !hov ? c.P.label : c.P.text), px, false);
  ImGui::PopID();
  return click;
}

// ---- the block menu: a caps head, rows with a glyph, the name and its shortcut, rules between groups
void menuHead(const Ctx& c, const char* title) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = c.s(M(24));
  ImGui::Dummy(ImVec2(c.s(M(250)), h));
  ImFont* f = c.a.fontMono ? c.a.fontMono : ImGui::GetFont();
  const float px = font(c, T(9.5f));
  std::string up = i18n::tr(title);
  for (char& ch : up) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
  dl->AddText(f, px, ImVec2(at.x + c.s(M(8)), at.y + (h - px) * 0.5f), c.P.textFaint, up.c_str());
}
void menuRule(const Ctx& c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(w, c.s(M(9))));
  dl->AddLine(ImVec2(at.x + c.s(M(4)), at.y + c.s(M(4.5f))), ImVec2(at.x + w - c.s(M(4)), at.y + c.s(M(4.5f))), c.P.line);
}
bool menuItemLook(const Ctx& c, const char* id, const char* label, const char* keys, int glyph, bool danger, bool enabled) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  // as wide as the label, a gap and the keys need (the menu takes its widest row)
  const float fs0 = ImGui::GetFontSize();
  ImFont* mf0 = c.a.fontMono ? c.a.fontMono : ImGui::GetFont();
  const float need = c.s(M(34)) + ImGui::CalcTextSize(i18n::tr(label)).x + c.s(M(28)) +
                     (keys && *keys ? mf0->CalcTextSizeA(fs0 * 0.85f, 1e9f, 0, i18n::tr(keys)).x + c.s(M(10)) : 0.f);
  const float w = std::max({c.s(M(260)), need, ImGui::GetContentRegionAvail().x}), h = c.s(M(30));
  ImGui::PushID(id);
  const bool hit = ImGui::InvisibleButton("##mi", ImVec2(w, h)) && enabled;
  const bool hv = ImGui::IsItemHovered() && enabled;
  ImGui::PopID();
  if (hv) {
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), danger ? theme::withAlpha(c.P.danger, 0.14f) : c.P.accentSoft, c.s(M(4)));
    if (!danger) dl->AddRect(at, ImVec2(at.x + w, at.y + h), theme::withAlpha(c.P.accent, 0.7f), c.s(M(4)));
  }
  const ImU32 col = !enabled ? c.P.textFaint : danger ? c.P.danger : c.P.text, gc = !enabled ? c.P.textFaint : danger ? c.P.danger : c.P.textDim;
  // the glyph: 1 open large, 2 power, 3 fold, 4 more, 5 patch, 6 question, 7 bin, 8 reset, 9 copy,
  // 10 paste, 11 folder, 12 save
  const ImVec2 g(at.x + c.s(M(16)), at.y + h * 0.5f);
  const float r = c.s(M(5)), lw = std::max(1.f, c.s(M(1.2f)));
  switch (glyph) {
    case 1:
      dl->AddRect(ImVec2(g.x - r, g.y - r), ImVec2(g.x + r, g.y + r), gc, c.s(M(1.5f)), 0, lw);
      dl->AddLine(ImVec2(g.x - r * 0.3f, g.y + r * 0.3f), ImVec2(g.x + r * 0.5f, g.y - r * 0.5f), gc, lw);
      break;
    case 2:
      dl->PathArcTo(g, r, -1.1f, 4.24f, 14);
      dl->PathStroke(gc, 0, lw);
      dl->AddLine(ImVec2(g.x, g.y - r * 1.2f), ImVec2(g.x, g.y - r * 0.2f), gc, lw);
      break;
    case 3: dl->AddLine(ImVec2(g.x - r, g.y), ImVec2(g.x + r, g.y), gc, lw), dl->AddLine(ImVec2(g.x - r, g.y - r * 0.7f), ImVec2(g.x + r, g.y - r * 0.7f), gc, lw); break;
    case 4:
      for (int i = 0; i < 3; ++i) dl->AddLine(ImVec2(g.x - r, g.y - r * 0.7f + i * r * 0.7f), ImVec2(g.x + r, g.y - r * 0.7f + i * r * 0.7f), gc, lw);
      break;
    case 5:
      dl->AddCircle(ImVec2(g.x - r * 0.8f, g.y), r * 0.35f, gc, 10, lw), dl->AddCircle(ImVec2(g.x + r * 0.8f, g.y - r * 0.6f), r * 0.35f, gc, 10, lw);
      dl->AddCircle(ImVec2(g.x + r * 0.8f, g.y + r * 0.6f), r * 0.35f, gc, 10, lw);
      dl->AddLine(ImVec2(g.x - r * 0.45f, g.y), ImVec2(g.x + r * 0.45f, g.y - r * 0.5f), gc, lw), dl->AddLine(ImVec2(g.x - r * 0.45f, g.y), ImVec2(g.x + r * 0.45f, g.y + r * 0.5f), gc, lw);
      break;
    case 6: dl->AddCircle(g, r, gc, 16, lw), dl->AddCircleFilled(ImVec2(g.x, g.y + r * 0.45f), lw * 0.8f, gc, 8); break;
    case 7:
      dl->AddRect(ImVec2(g.x - r * 0.7f, g.y - r * 0.5f), ImVec2(g.x + r * 0.7f, g.y + r), gc, c.s(M(1)), 0, lw);
      dl->AddLine(ImVec2(g.x - r, g.y - r * 0.7f), ImVec2(g.x + r, g.y - r * 0.7f), gc, lw);
      break;
    case 8:  // reset: a turning arrow
      dl->PathArcTo(g, r * 0.85f, -2.6f, 2.4f, 16);
      dl->PathStroke(gc, 0, lw);
      dl->AddTriangleFilled(ImVec2(g.x - r * 1.05f, g.y - r * 0.95f), ImVec2(g.x - r * 0.15f, g.y - r * 0.6f), ImVec2(g.x - r * 0.95f, g.y - r * 0.05f), gc);
      break;
    case 9:  // copy: two sheets
      dl->AddRect(ImVec2(g.x - r * 0.9f, g.y - r * 0.9f), ImVec2(g.x + r * 0.4f, g.y + r * 0.4f), gc, c.s(M(1)), 0, lw);
      dl->AddRect(ImVec2(g.x - r * 0.4f, g.y - r * 0.4f), ImVec2(g.x + r * 0.9f, g.y + r * 0.9f), gc, c.s(M(1)), 0, lw);
      break;
    case 10:  // paste: a board with a clip
      dl->AddRect(ImVec2(g.x - r * 0.75f, g.y - r * 0.75f), ImVec2(g.x + r * 0.75f, g.y + r), gc, c.s(M(1)), 0, lw);
      dl->AddRectFilled(ImVec2(g.x - r * 0.35f, g.y - r), ImVec2(g.x + r * 0.35f, g.y - r * 0.55f), gc, c.s(M(0.5f)));
      break;
    case 11:  // load: a folder
      dl->PathLineTo(ImVec2(g.x - r, g.y + r * 0.7f)), dl->PathLineTo(ImVec2(g.x - r, g.y - r * 0.7f));
      dl->PathLineTo(ImVec2(g.x - r * 0.25f, g.y - r * 0.7f)), dl->PathLineTo(ImVec2(g.x + r * 0.05f, g.y - r * 0.4f));
      dl->PathLineTo(ImVec2(g.x + r, g.y - r * 0.4f)), dl->PathLineTo(ImVec2(g.x + r, g.y + r * 0.7f));
      dl->PathStroke(gc, ImDrawFlags_Closed, lw);
      break;
    case 12:  // save: an arrow down onto a line
      dl->AddLine(ImVec2(g.x, g.y - r), ImVec2(g.x, g.y + r * 0.35f), gc, lw);
      dl->AddLine(ImVec2(g.x - r * 0.5f, g.y - r * 0.1f), ImVec2(g.x, g.y + r * 0.4f), gc, lw), dl->AddLine(ImVec2(g.x + r * 0.5f, g.y - r * 0.1f), ImVec2(g.x, g.y + r * 0.4f), gc, lw);
      dl->AddLine(ImVec2(g.x - r, g.y + r), ImVec2(g.x + r, g.y + r), gc, lw);
      break;
    default: break;
  }
  ImFont* f = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const char* tl = i18n::tr(label);
  dl->AddText(f, fs, ImVec2(at.x + c.s(M(34)), at.y + (h - fs) * 0.5f), col, tl);
  layoutWindowText(ImVec2(at.x + c.s(M(34)), at.y), tl, 0);
  if (keys && *keys) {
    ImFont* mf = c.a.fontMono ? c.a.fontMono : f;
    const float ks = fs * 0.85f;
    const char* tk = i18n::tr(keys);
    const ImVec2 sz = mf->CalcTextSizeA(ks, 1e9f, 0, tk);
    dl->AddText(mf, ks, ImVec2(at.x + w - c.s(M(10)) - sz.x, at.y + (h - ks) * 0.5f), c.P.textFaint, tk);
  }
  return hit;
}

void chipLook(const Ctx& c, float x, float y, float w, float h, const char* label, bool dotOn) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.panelHi, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(4)));
  const float dx = x + M(11) + M(3.5f);
  if (dotOn) glowDot(c, c.p(dx, y + h * 0.5f), M(3.5f), c.P.good);
  else c.dl->AddCircleFilled(c.p(dx, y + h * 0.5f), c.s(M(3.5f)), c.P.textFaint, 12);
  labelBeside(c, x + M(11 + 7 + 7), y, h, label, c.P.label, T(11.5f), false);  // on the row's baseline
}

void modChip(const Ctx& c, float rx, float y, const char* label, ImU32 col) {
  MonoText mono;
  const float tw = textW(c, label, T(10), false), w = tw + M(7 + 5 + 5 + 7), h = M(18);
  const float x = rx - w;
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), theme::withAlpha(col, 0.45f), c.s(M(9)));
  c.dl->AddCircleFilled(c.p(x + M(7 + 2.5f), y + h * 0.5f), c.s(M(2.5f)), col, 10);
  text(c, x + M(7 + 5 + 5), y + h * 0.5f - T(10) * 0.68f, label, c.P.label, T(10), false);
}

}  // namespace gl::packs::plasma::ui
