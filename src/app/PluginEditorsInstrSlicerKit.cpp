// Gridline — Kerf's own drawing kit (see PluginEditorsInstrSlicerKit.h).
#include "PluginEditorsInstrSlicerKit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include "../core/I18n.h"
#include "App.h"
#include "PluginUI.h"
#include "Theme.h"

namespace gl::kerf {

namespace {
using iskin::lerp;
using iskin::rgb;

struct PeakEntry {
  std::string path;
  size_t frames = 0;
  Peaks peaks;
  uint64_t used = 0;
};
std::map<const Sample*, PeakEntry>& peakCache() {
  static std::map<const Sample*, PeakEntry> c;
  return c;
}
uint64_t& peakClock() {
  static uint64_t t = 0;
  return t;
}

float mono(const Sample& s, size_t i) { return (s.left[i] + s.right[i]) * 0.5f; }
}  // namespace

const Peaks* peaksOf(const Sample* smp) {
  if (!smp || smp->frames() == 0) return nullptr;
  auto& cache = peakCache();
  const uint64_t now = ++peakClock();
  auto it = cache.find(smp);
  if (it != cache.end() && it->second.path == smp->path && it->second.frames == smp->frames()) {
    it->second.used = now;
    return &it->second.peaks;
  }
  if (cache.size() >= 6) {  // the least recently drawn goes
    auto old = cache.begin();
    for (auto q = cache.begin(); q != cache.end(); ++q)
      if (q->second.used < old->second.used) old = q;
    cache.erase(old);
  }
  PeakEntry& e = cache[smp];
  e.path = smp->path, e.frames = smp->frames(), e.used = now;
  Peaks& p = e.peaks;
  p.frames = smp->frames();
  const size_t blocks = (p.frames + Peaks::kBlock - 1) / Peaks::kBlock;
  p.lo.assign(blocks, 0.0f), p.hi.assign(blocks, 0.0f), p.sq.assign(blocks, 0.0f);
  for (size_t b = 0; b < blocks; ++b) {
    float lo = 1e9f, hi = -1e9f, sq = 0;
    const size_t i1 = std::min(p.frames, (b + 1) * Peaks::kBlock);
    for (size_t i = b * Peaks::kBlock; i < i1; ++i) {
      const float v = mono(*smp, i);
      lo = std::min(lo, v), hi = std::max(hi, v), sq += v * v;
    }
    p.lo[b] = lo, p.hi[b] = hi, p.sq[b] = sq;
  }
  return &p;
}

void drawWave(ImDrawList* dl, const Sample* smp, ImVec2 p0, ImVec2 p1, double f0, double f1, const WaveStyle& ws) {
  const float mid = std::round((p0.y + p1.y) * 0.5f), half = (p1.y - p0.y) * ws.gain;
  if (ws.centre) dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), ws.centre);
  const Peaks* pk = peaksOf(smp);
  if (!pk || !(f1 > f0)) return;
  const int64_t n = static_cast<int64_t>(smp->frames());
  const int w = std::max(1, static_cast<int>(p1.x - p0.x));
  const double perPx = (f1 - f0) / w;
  auto Y = [&](float v) { return mid - std::clamp(v, -1.0f, 1.0f) * half; };
  if (perPx < 0.5) {  // zoomed in past one frame per two pixels: the samples as a line with dots
    const int64_t a = std::max<int64_t>(0, static_cast<int64_t>(std::floor(f0)) - 1), b = std::min<int64_t>(n - 1, static_cast<int64_t>(std::ceil(f1)) + 1);
    std::vector<ImVec2> pts;
    for (int64_t i = a; i <= b; ++i) pts.push_back(ImVec2(p0.x + static_cast<float>((static_cast<double>(i) - f0) / perPx), Y(mono(*smp, static_cast<size_t>(i)))));
    for (const ImVec2& q : pts) dl->AddLine(ImVec2(q.x, mid), q, ws.peak);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ws.edge ? ws.edge : ws.rms, 0, 1.5f);
    if (perPx < 0.125)
      for (const ImVec2& q : pts) dl->AddCircleFilled(q, 2.0f, ws.edge ? ws.edge : ws.rms);
    return;
  }
  std::vector<ImVec2> top(static_cast<size_t>(w)), bot(static_cast<size_t>(w));
  const ImU32 core = ws.peak, rim = theme::withAlpha(ws.peak, 0.45f);
  for (int x = 0; x < w; ++x) {
    const int64_t a0 = std::clamp<int64_t>(static_cast<int64_t>(f0 + perPx * x), 0, n - 1);
    const int64_t a1 = std::clamp<int64_t>(static_cast<int64_t>(f0 + perPx * (x + 1)), a0 + 1, n);
    float lo = 0, hi = 0, sq = 0;
    int64_t cnt = 0;
    if (a1 - a0 >= 2 * Peaks::kBlock) {
      const int64_t b0 = a0 / Peaks::kBlock, b1 = (a1 + Peaks::kBlock - 1) / Peaks::kBlock;
      lo = 1e9f, hi = -1e9f;
      for (int64_t b = b0; b < b1; ++b) {
        lo = std::min(lo, pk->lo[static_cast<size_t>(b)]), hi = std::max(hi, pk->hi[static_cast<size_t>(b)]);
        sq += pk->sq[static_cast<size_t>(b)];
      }
      cnt = std::min<int64_t>(n, b1 * Peaks::kBlock) - b0 * Peaks::kBlock;
    } else {
      lo = 1e9f, hi = -1e9f;
      for (int64_t i = a0; i < a1; ++i) {
        const float v = mono(*smp, static_cast<size_t>(i));
        lo = std::min(lo, v), hi = std::max(hi, v), sq += v * v;
      }
      cnt = a1 - a0;
    }
    const float rms = std::sqrt(sq / static_cast<float>(std::max<int64_t>(1, cnt)));
    const float X = p0.x + static_cast<float>(x);
    float yt = Y(hi), yb = Y(lo);
    if (yb - yt < 1.0f) yt = mid - 0.5f, yb = mid + 0.5f;
    // Body: brighter towards the centre line (the same gradient for every column).
    const float tt = std::clamp((mid - yt) / std::max(1.0f, half), 0.0f, 1.0f), tb = std::clamp((yb - mid) / std::max(1.0f, half), 0.0f, 1.0f);
    const ImU32 ct = lerp(core, rim, tt), cb = lerp(core, rim, tb);
    dl->AddRectFilledMultiColor(ImVec2(X, yt), ImVec2(X + 1, mid), ct, ct, core, core);
    dl->AddRectFilledMultiColor(ImVec2(X, mid), ImVec2(X + 1, yb), core, core, cb, cb);
    const float r = std::min(rms * half, std::min(mid - yt, yb - mid));
    if (r > 0.5f) dl->AddRectFilled(ImVec2(X, mid - r), ImVec2(X + 1, mid + r), ws.rms);
    top[static_cast<size_t>(x)] = ImVec2(X + 0.5f, yt), bot[static_cast<size_t>(x)] = ImVec2(X + 0.5f, yb);
  }
  if (ws.edge && w > 1) {
    dl->AddPolyline(top.data(), w, ws.edge, 0, 1.0f);
    dl->AddPolyline(bot.data(), w, ws.edge, 0, 1.0f);
  }
}

int tabStrip(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* const* labels, int n, int cur,
             const bool* enabled, const char* const* hints, ImU32 accent) {
  ImDrawList* dl = k.dl();
  iskin::bevel(k, x, y, w, h, rgb(0x262D33), rgb(0x161B20), 2);
  int clicked = -1;
  const float tw = w / static_cast<float>(std::max(1, n));
  ImGui::PushID(id);
  for (int i = 0; i < n; ++i) {
    const bool en = !enabled || enabled[i];
    const float tx = x + tw * static_cast<float>(i);
    const ImVec2 q0 = k.at(tx, y), q1 = k.at(tx + tw, y + h);
    ImGui::SetCursorScreenPos(q0);
    ImGui::PushID(i);
    ImGui::BeginDisabled(!en);
    if (ImGui::InvisibleButton("##tab", ImVec2(q1.x - q0.x, q1.y - q0.y))) clicked = i;
    const bool hov = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImGui::PopID();
    if (hov && hints && hints[i]) a.setHint(i18n::tr(hints[i]));
    const bool on = i == cur;
    if (on) {
      dl->AddRectFilled(ImVec2(q0.x + 1, q0.y + 1), ImVec2(q1.x - 1, q1.y - 1), lerp(rgb(0x262D33), accent, 0.22f), k.px(2));
      dl->AddRectFilled(ImVec2(q0.x + k.px(6), q1.y - std::max(1.5f, k.px(2))), ImVec2(q1.x - k.px(6), q1.y - 1), accent, 1.0f);
    } else if (hov) {
      dl->AddRectFilled(ImVec2(q0.x + 1, q0.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(IM_COL32_WHITE, 0.05f), k.px(2));
    }
    if (i > 0 && !on && i - 1 != cur) dl->AddLine(ImVec2(q0.x, q0.y + k.px(3)), ImVec2(q0.x, q1.y - k.px(3)), rgb(0x3A434A));
    const ImU32 tc = !en ? rgb(0x5C666E) : on ? lerp(accent, IM_COL32_WHITE, 0.55f) : hov ? rgb(0xD5DCE2) : rgb(0x98A3AC);
    text(k, tx + 2, y, tw - 4, h, labels[i], Role::Tab, tc, 1);
  }
  ImGui::PopID();
  return clicked;
}

bool deckButton(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint, int litMode,
                bool menu, const char* caption) {
  const bool lit = litMode == 1, glow = litMode == 2;
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  const bool hit = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled), act = ImGui::IsItemActive();
  const bool dis = ImGui::GetStyle().Alpha < 0.99f;  // inside BeginDisabled
  if (hov && hint) a.setHint(i18n::tr(hint));
  const ImU32 accent = rgb(0xFFA830);
  ImU32 top = rgb(0x48505A), bot = rgb(0x3A4148);
  if (lit) top = lerp(accent, IM_COL32_WHITE, 0.12f), bot = accent;
  else if (act) top = rgb(0x2E353B), bot = rgb(0x353C43);
  else if (hov && !dis) top = rgb(0x535C66), bot = rgb(0x434B53);
  const float r = k.px(3);
  dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(q.x, q.y + 1), IM_COL32(0, 0, 0, 70), r);  // drop shadow
  dl->AddRectFilled(p, q, bot, r);
  dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, (p.y + q.y) * 0.5f), top, top, lerp(top, bot, 0.5f), lerp(top, bot, 0.5f));
  dl->AddLine(ImVec2(p.x + r, p.y + 0.5f), ImVec2(q.x - r, p.y + 0.5f), theme::withAlpha(IM_COL32_WHITE, lit ? 0.35f : 0.10f));
  dl->AddRect(p, q, rgb(0x1A1F24), r);
  const ImU32 ic = lit ? rgb(0x221A0E) : dis ? rgb(0x6E7880) : glow ? (hov ? lerp(accent, IM_COL32_WHITE, 0.3f) : accent) : hov ? rgb(0xF2F5F8) : rgb(0xCDD4DA);
  if (glow) dl->AddLine(ImVec2(p.x + k.px(5), q.y - k.px(2.5f)), ImVec2(q.x - k.px(5), q.y - k.px(2.5f)), accent, std::max(1.0f, k.px(1.5f)));
  const float cy = menu ? (p.y + q.y) * 0.5f - k.px(3) : (p.y + q.y) * 0.5f;
  const float s = std::min(q.x - p.x, q.y - p.y) * 0.56f;
  if (caption && *caption) {
    ImFont* f = app().fontBold;
    const float fs = std::max(8.0f, k.px(9.5f));
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, caption);
    dl->AddText(f, fs, ImVec2(std::round((p.x + q.x - ts.x) * 0.5f), std::round(cy - ts.y * 0.5f)), ic, caption);
  } else {
    ui::drawIconFit(dl, icon, ImVec2((p.x + q.x) * 0.5f - s * 0.5f, cy - s * 0.5f), ImVec2((p.x + q.x) * 0.5f + s * 0.5f, cy + s * 0.5f), ic);
  }
  if (menu) {  // a small caret under the glyph: this button opens a menu
    const float cx = (p.x + q.x) * 0.5f, by = q.y - k.px(6);
    const float cw = k.px(3);
    dl->AddTriangleFilled(ImVec2(cx - cw, by - cw * 0.6f), ImVec2(cx + cw, by - cw * 0.6f), ImVec2(cx, by + cw * 0.5f), theme::withAlpha(ic, 0.8f));
  }
  return hit;
}

void bandGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int mode, ImU32 on, ImU32 off) {
  // Five bands (master, region panel, deck bar + wave, keyboard) at the form's proportions.
  const float H = p1.y - p0.y;
  const float ys[4] = {0.0f, 0.2f, 0.52f, 0.86f}, ye[4] = {0.17f, 0.49f, 0.83f, 1.0f};
  const bool shown[4][4] = {{true, true, true, true}, {true, true, false, true}, {true, false, true, true}, {true, false, false, true}};
  for (int b = 0; b < 4; ++b) {
    const ImVec2 a(p0.x, std::round(p0.y + H * ys[b])), c(p1.x, std::round(p0.y + H * ye[b]));
    if (shown[std::clamp(mode, 0, 3)][b]) dl->AddRectFilled(a, ImVec2(c.x, std::max(c.y, a.y + 1)), on, 1.0f);
    else dl->AddRect(a, ImVec2(c.x, std::max(c.y, a.y + 2)), off, 1.0f);
  }
}

void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float th) {
  const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
  if (len < 1) return;
  const float ux = dx / len, uy = dy / len;
  for (float t = 0; t < len; t += dash + gap) {
    const float e = std::min(len, t + dash);
    dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * e, a.y + uy * e), col, th);
  }
}

bool dropZone(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ImU32 accent) {
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  const bool hit = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  const bool dragging = pl && pl->IsDataType("GL_FILE");
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || (dragging && ImGui::IsMouseHoveringRect(p, q));
  if (hov && !dragging) a.setHint(i18n::tr("Drop a loop here, or click to choose one"));
  const ImU32 line = hov || dragging ? accent : rgb(0x3C4A57);
  const float r = k.px(8);
  dl->AddRectFilled(p, q, theme::withAlpha(accent, hov || dragging ? 0.07f : 0.025f), r);
  const float d = k.px(7), g = k.px(5);
  dashedLine(dl, ImVec2(p.x + r, p.y), ImVec2(q.x - r, p.y), line, d, g, 1.2f);
  dashedLine(dl, ImVec2(p.x + r, q.y), ImVec2(q.x - r, q.y), line, d, g, 1.2f);
  dashedLine(dl, ImVec2(p.x, p.y + r), ImVec2(p.x, q.y - r), line, d, g, 1.2f);
  dashedLine(dl, ImVec2(q.x, p.y + r), ImVec2(q.x, q.y - r), line, d, g, 1.2f);
  for (int c = 0; c < 4; ++c) {  // rounded corners
    const ImVec2 cc((c & 1) ? q.x - r : p.x + r, (c & 2) ? q.y - r : p.y + r);
    const float a0 = c == 0 ? 3.14159f : c == 1 ? 4.71239f : c == 2 ? 1.5708f : 0.0f;
    dl->PathArcTo(cc, r, a0, a0 + 1.5708f, 8);
    dl->PathStroke(line, 0, 1.2f);
  }
  const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
  const float is = k.px(26);
  dl->AddCircleFilled(ImVec2(c.x, c.y - k.px(18)), is * 0.95f, theme::withAlpha(accent, 0.12f), 40);
  ui::drawIconFit(dl, ui::Icon::Wave, ImVec2(c.x - is * 0.5f, c.y - k.px(18) - is * 0.5f), ImVec2(c.x + is * 0.5f, c.y - k.px(18) + is * 0.5f),
                  hov || dragging ? accent : rgb(0x8FA9C4));
  ImFont* fb = app().fontBold;
  ImFont* fr = app().fontRegular;
  const char* t1 = i18n::tr("Drop a loop here");
  const char* t2 = i18n::tr("or click to load one: WAV, FLAC, MP3, Ogg, WavPack");
  const float s1 = std::max(9.0f, k.px(13)), s2 = std::max(8.0f, k.px(9));
  const ImVec2 z1 = fb->CalcTextSizeA(s1, FLT_MAX, 0, t1), z2 = fr->CalcTextSizeA(s2, FLT_MAX, 0, t2);
  dl->AddText(fb, s1, ImVec2(std::round(c.x - z1.x * 0.5f), std::round(c.y + k.px(14))), rgb(0xE3E9EF), t1);
  dl->AddText(fr, s2, ImVec2(std::round(c.x - z2.x * 0.5f), std::round(c.y + k.px(14) + z1.y + k.px(3))), rgb(0x8C98A3), t2);
  return hit;
}

// ============================================================================ J-183
namespace {
struct AuditBox {
  std::string id;
  ImVec2 p, q;
  int row;
};
struct AuditText {
  std::string s;
  ImVec2 p, q;  // its box (screen)
  float ink, px;
};
struct AuditState {
  bool on = false;
  std::vector<AuditBox> boxes;
  std::vector<AuditText> texts;
};
AuditState& auditState() {
  static AuditState a;
  return a;
}
ImFont* roleFont(Role r) {
  App& a = app();
  if (r == Role::Value) return a.fontMono;
  if (r == Role::Body) return a.fontRegular;
  return a.fontBold;
}
}  // namespace

float roleSize(Role r) {
  switch (r) {
    case Role::Title: return 12.0f;
    case Role::Label: return 11.0f;
    case Role::Tab: return 11.5f;
    case Role::Value: return 11.0f;
    case Role::Eyebrow: return 10.0f;
    case Role::Body: return 10.5f;
  }
  return 11.0f;
}

float text(const iskin::Skin& k, float x, float y, float w, float h, const char* s, Role r, ImU32 col, int align) {
  if (!s || !*s) return 0;
  ImFont* f = roleFont(r);
  if (!f) return 0;
  const float fs = std::max(kMinTextPx, k.px(roleSize(r)));
  const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, s);
  const ImVec2 a = k.at(x, y), b = k.at(x + w, y + h);
  float tx = align == 1 ? (a.x + b.x - ts.x) * 0.5f : align == 2 ? b.x - ts.x : a.x;
  tx = std::max(tx, a.x);
  ImDrawList* dl = k.dl();
  // Clipped across only: a small form keeps the text at its minimum size, taller than its box.
  const float ty = std::round((a.y + b.y - ts.y) * 0.5f);
  dl->PushClipRect(ImVec2(a.x - 1, std::min(a.y, ty - 1)), ImVec2(b.x + 1, std::max(b.y, ty + ts.y + 1)), true);
  dl->AddText(f, fs, ImVec2(align == 2 ? std::floor(tx) : std::round(tx), ty), col, s);
  dl->PopClipRect();
  AuditState& au = auditState();
  if (au.on) au.texts.push_back({s, a, b, ts.x, fs});
  return ts.x;
}

std::string fit(const iskin::Skin& k, const std::string& s, Role r, float w) {
  ImFont* f = roleFont(r);
  if (!f) return s;
  const float fs = std::max(kMinTextPx, k.px(roleSize(r))), room = k.px(w);
  if (f->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str()).x <= room) return s;
  std::string t = s;
  while (!t.empty()) {
    do t.pop_back();
    while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);  // whole UTF-8 characters
    if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0x80)) t.pop_back();
    if (f->CalcTextSizeA(fs, FLT_MAX, 0, (t + "...").c_str()).x <= room) return t + "...";
  }
  return std::string();
}

void auditBegin(bool on) {
  AuditState& au = auditState();
  au.on = on;
  au.boxes.clear();
  au.texts.clear();
}
bool auditOn() { return auditState().on; }

void auditBox(const char* id, const iskin::Skin& k, float x, float y, float w, float h, int row) {
  AuditState& au = auditState();
  if (au.on) au.boxes.push_back({id, k.at(x, y), k.at(x + w, y + h), row});
}

std::string auditReport() {
  const AuditState& au = auditState();
  int overlaps = 0, rows = 0, clipped = 0, small = 0;
  float minPx = 1e9f;
  std::string detail;
  auto note = [&](const std::string& d) {
    if (std::count(detail.begin(), detail.end(), ';') < 12) detail += d + ";";
  };
  auto cross = [](ImVec2 p0, ImVec2 q0, ImVec2 p1, ImVec2 q1) {
    const float ox = std::min(q0.x, q1.x) - std::max(p0.x, p1.x), oy = std::min(q0.y, q1.y) - std::max(p0.y, p1.y);
    return ox > 0.75f && oy > 0.75f;
  };
  for (size_t i = 0; i < au.boxes.size(); ++i)
    for (size_t j = i + 1; j < au.boxes.size(); ++j) {
      const AuditBox &a = au.boxes[i], &b = au.boxes[j];
      if (cross(a.p, a.q, b.p, b.q)) ++overlaps, note("overlap " + a.id + "/" + b.id);
      if (a.row >= 0 && a.row == b.row && (std::fabs(a.p.y - b.p.y) > 0.75f || std::fabs(a.q.y - b.q.y) > 0.75f))
        ++rows, note("row " + std::to_string(a.row) + " " + a.id + "/" + b.id);
    }
  for (const AuditText& t : au.texts) {
    minPx = std::min(minPx, t.px);
    if (t.px < kMinTextPx - 0.01f) ++small, note("small " + t.s);
    if (t.ink > t.q.x - t.p.x + 0.75f) ++clipped, note("clipped " + t.s);
    // A caption may sit inside one control (its own); it must not reach into another one.
    for (const AuditBox& b : au.boxes) {
      const bool inside = t.p.x >= b.p.x - 0.75f && t.q.x <= b.q.x + 0.75f && t.p.y >= b.p.y - 0.75f && t.q.y <= b.q.y + 0.75f;
      if (!inside && cross(t.p, t.q, b.p, b.q)) ++overlaps, note("text " + t.s + "/" + b.id);
    }
  }
  char b[200];
  std::snprintf(b, sizeof b, "kerf-audit: boxes=%zu texts=%zu overlaps=%d rows=%d clipped=%d small=%d minpx=%.1f", au.boxes.size(), au.texts.size(), overlaps,
                rows, clipped, small, static_cast<double>(au.texts.empty() ? 0.0f : minPx));
  return std::string(b) + (detail.empty() ? std::string() : " [" + detail + "]");
}

void card(const iskin::Skin& k, float x, float y, float w, float h, bool light) {
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  const float r = k.px(4);
  const ImU32 fill = light ? rgb(0x5A6166) : rgb(0x1C2733), edge = light ? rgb(0x444A4F) : rgb(0x0A1016);
  dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(q.x, q.y + 1), IM_COL32(0, 0, 0, light ? 40 : 70), r);
  dl->AddRectFilled(p, q, fill, r);
  dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, p.y + k.px(16)), theme::withAlpha(IM_COL32_WHITE, light ? 0.07f : 0.04f),
                              theme::withAlpha(IM_COL32_WHITE, light ? 0.07f : 0.04f), theme::withAlpha(IM_COL32_WHITE, 0), theme::withAlpha(IM_COL32_WHITE, 0));
  dl->AddRect(p, q, edge, r);
  // Two small notches on the top edge (the reference cards' joins), drawn as short raised tabs.
  const ImU32 nc = light ? rgb(0x6A7176) : rgb(0x26323E);
  for (int i = 0; i < 2; ++i) {
    const float nx = i == 0 ? x + 8 : x + w - 26;
    dl->AddRectFilled(k.at(nx, y - 2), k.at(nx + 18, y + 1), nc, k.px(2), ImDrawFlags_RoundCornersTop);
  }
}

bool flatButton(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint, bool lit) {
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  const bool hit = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled), act = ImGui::IsItemActive();
  if (hov && hint) a.setHint(i18n::tr(hint));
  const ImU32 accent = rgb(0xFFA830);
  if (act) dl->AddRectFilled(p, q, theme::withAlpha(IM_COL32_WHITE, 0.10f), k.px(4));
  else if (hov) dl->AddRectFilled(p, q, theme::withAlpha(IM_COL32_WHITE, 0.06f), k.px(4));
  if (lit) dl->AddRectFilled(p, q, theme::withAlpha(accent, 0.12f), k.px(4));
  const float s = std::min(q.x - p.x, q.y - p.y) * 0.58f;
  const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
  ui::drawIconFit(dl, icon, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), lit ? accent : hov ? rgb(0xF2F5F8) : rgb(0xC3CBD2));
  auditBox(id, k, x, y, w, h, 9000 + static_cast<int>(y));
  return hit;
}

bool ledToggle(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool on, const char* hint, bool dark) {
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  const bool hit = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  const bool dis = ImGui::GetStyle().Alpha < 0.99f;
  if (hov && hint) a.setHint(i18n::tr(hint));
  const ImU32 accent = rgb(0xFFA830);
  const float ls = 10, lx = x + 2, ly = y + (h - ls) * 0.5f;
  const ImVec2 lc = k.at(lx + ls * 0.5f, ly + ls * 0.5f);
  if (on && !dis) dl->AddCircleFilled(lc, k.px(ls * 0.95f), theme::withAlpha(accent, 0.20f), 20);
  dl->AddCircleFilled(lc, k.px(ls * 0.5f), on ? (dis ? lerp(accent, rgb(0x2A3136), 0.55f) : accent) : dark ? rgb(0x3E464C) : rgb(0x2A3136), 20);
  dl->AddCircle(lc, k.px(ls * 0.5f), on ? lerp(accent, IM_COL32_BLACK, 0.35f) : rgb(0x10161B), 20, 1.0f);
  if (on) dl->AddCircleFilled(ImVec2(lc.x - k.px(1.5f), lc.y - k.px(1.5f)), k.px(1.4f), theme::withAlpha(IM_COL32_WHITE, 0.6f), 8);
  const ImU32 tc = dis ? (dark ? rgb(0x3C4247) : rgb(0x5C666E)) : dark ? (on || hov ? rgb(0x15191C) : rgb(0x262C31)) : (on || hov ? rgb(0xE9EDFA) : rgb(0xACB4BA));
  text(k, lx + ls + 5, y, w - (lx + ls + 5 - x), h, caption, Role::Label, tc, 0);
  auditBox(id, k, x, y, w, h, 9000 + static_cast<int>(y));
  return hit;
}

bool iconSwitch(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint, bool on) {
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  const bool hit = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  const bool dis = ImGui::GetStyle().Alpha < 0.99f;
  if (hov && hint) a.setHint(i18n::tr(hint));
  const ImU32 accent = rgb(0xFFA830);
  dl->AddRectFilled(p, q, on ? lerp(rgb(0x262D33), accent, 0.20f) : hov ? rgb(0x2E363D) : rgb(0x232A30), k.px(3));
  dl->AddRect(p, q, on ? lerp(accent, IM_COL32_BLACK, 0.3f) : rgb(0x12171B), k.px(3));
  const float s = std::min(q.x - p.x, q.y - p.y) * 0.66f;
  const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
  ui::drawIconFit(dl, icon, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f),
                  dis ? rgb(0x5C666E) : on ? accent : hov ? rgb(0xF2F5F8) : rgb(0x98A3AC));
  auditBox(id, k, x, y, w, h, 9000 + static_cast<int>(y));
  return hit;
}

int segmented(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* const* labels, const ui::Icon* icons, int n,
              int cur, const char* const* hints, bool dark) {
  ImDrawList* dl = k.dl();
  const ImU32 accent = rgb(0xFFA830), ground = dark ? rgb(0x232A30) : rgb(0x4C5358);
  iskin::bevel(k, x, y, w, h, ground, dark ? rgb(0x12171B) : rgb(0x3A4045), 3);
  const float cw = w / static_cast<float>(std::max(1, n));
  const bool dis = ImGui::GetStyle().Alpha < 0.99f;
  int clicked = -1;
  ImGui::PushID(id);
  for (int i = 0; i < n; ++i) {
    const float cx = x + cw * static_cast<float>(i);
    const ImVec2 p = k.at(cx, y), q = k.at(cx + cw, y + h);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##seg", ImVec2(q.x - p.x, q.y - p.y))) clicked = i;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov && hints && hints[i]) a.setHint(i18n::tr(hints[i]));
    const bool on = i == cur;
    if (on) dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), dis ? lerp(ground, accent, 0.25f) : lerp(ground, accent, 0.85f), k.px(2));
    else if (hov) dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), theme::withAlpha(IM_COL32_WHITE, 0.07f), k.px(2));
    if (i > 0 && !on && i - 1 != cur) dl->AddLine(ImVec2(p.x, p.y + k.px(3)), ImVec2(p.x, q.y - k.px(3)), dark ? rgb(0x3A434A) : rgb(0x3E4449));
    const ImU32 tc = dis ? rgb(0x6E777E) : on ? rgb(0x1E160A) : hov ? rgb(0xF2F5F8) : dark ? rgb(0xA4AEB7) : rgb(0xD8DEE3);
    if (icons && icons[i] != ui::Icon::Count) {
      const float s = std::min(q.x - p.x, q.y - p.y) * 0.62f;
      const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
      ui::drawIconFit(dl, icons[i], ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), tc);
    } else if (labels && labels[i]) {
      text(k, cx + 1, y, cw - 2, h, labels[i], Role::Tab, tc, 1);
    }
  }
  ImGui::PopID();
  auditBox(id, k, x, y, w, h, 9000 + static_cast<int>(y));
  return clicked;
}

int padKeyboard(App& a, const PluginTarget& t, const iskin::Skin& k, float x, float y, float w, float h, int low, int whites, const bool* lit, int mark,
                int* hover) {
  static const int wOff[7] = {0, 2, 4, 5, 7, 9, 11};
  static const int bAfter[7] = {1, 1, 0, 1, 1, 1, 0};
  ImDrawList* dl = k.dl();
  const ImVec2 p = k.at(x, y), q = k.at(x + w, y + h);
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("kpads");
  ImGui::InvisibleButton("##pads", ImVec2(q.x - p.x, q.y - p.y));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float pitch = (q.x - p.x) / static_cast<float>(whites);
  const float rowB = (q.y - p.y) * 0.44f, gapY = std::max(2.0f, k.px(3));  // black row on top, white row under it
  const float padW = std::max(2.0f, pitch - std::max(2.0f, k.px(3))), blackW = padW * 0.86f;
  auto whiteNote = [&](int i) { return low + (i / 7) * 12 + wOff[i % 7]; };
  auto whiteRect = [&](int i, ImVec2& a0, ImVec2& a1) {
    a0 = ImVec2(std::round(p.x + pitch * static_cast<float>(i) + (pitch - padW) * 0.5f), std::round(p.y + rowB + gapY));
    a1 = ImVec2(a0.x + std::round(padW), q.y);
  };
  auto blackRect = [&](int i, ImVec2& a0, ImVec2& a1) {  // the black key after white i
    const float cx = p.x + pitch * static_cast<float>(i + 1);
    a0 = ImVec2(std::round(cx - blackW * 0.5f), p.y);
    a1 = ImVec2(a0.x + std::round(blackW), std::round(p.y + rowB));
  };
  int hv = -1;
  if (hov) {
    for (int i = 0; i + 1 < whites && hv < 0; ++i) {
      if (!bAfter[i % 7]) continue;
      ImVec2 a0, a1;
      blackRect(i, a0, a1);
      if (m.x >= a0.x && m.x < a1.x && m.y < a1.y + gapY * 0.5f) hv = whiteNote(i) + 1;
    }
    if (hv < 0 && m.y >= p.y + rowB) hv = whiteNote(std::clamp(static_cast<int>((m.x - p.x) / pitch), 0, whites - 1));
  }
  static int playing = -1;
  static ImGuiID playingId = 0;
  int clicked = -1;
  if (ImGui::IsItemActivated() && hv >= 0) {
    clicked = hv;
    if (t.isChannel()) {
      a.engine.noteOn(t.owner, hv, 0.85f);
      playing = hv, playingId = ImGui::GetItemID();
    }
  }
  if (ImGui::IsItemDeactivated() && playing >= 0 && playingId == ImGui::GetItemID()) {
    a.engine.noteOff(t.owner, playing);
    playing = -1;
  }
  const ImU32 accent = rgb(0xFFA830);
  auto pad = [&](ImVec2 a0, ImVec2 a1, int note, bool black) {
    ImU32 c = black ? rgb(0x2C353C) : rgb(0xC9D0D6);
    const bool isLit = lit && note >= 0 && note < 128 && lit[note];
    if (isLit) c = black ? lerp(c, accent, 0.55f) : lerp(c, accent, 0.45f);
    if (note == mark) c = accent;
    if (act && note == playing) c = rgb(0xFFE2B0);
    else if (note == hv) c = lerp(c, IM_COL32_WHITE, 0.22f);
    const float r = k.px(2.5f);
    dl->AddRectFilled(ImVec2(a0.x, a0.y + 1), ImVec2(a1.x, a1.y + 1), IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(a0, a1, c, r);
    dl->AddLine(ImVec2(a0.x + r, a0.y + 0.5f), ImVec2(a1.x - r, a0.y + 0.5f), theme::withAlpha(IM_COL32_WHITE, black ? 0.10f : 0.45f));
  };
  for (int i = 0; i < whites; ++i) {
    ImVec2 a0, a1;
    whiteRect(i, a0, a1);
    const int note = whiteNote(i);
    pad(a0, a1, note, false);
    if (note % 12 == 0) {  // the octave's name at the foot of its C (when it fits)
      char nm[16];
      std::snprintf(nm, sizeof nm, "C%d", note / 12 - 1);
      ImFont* f = app().fontMono;
      const float fs = std::max(kMinTextPx, k.px(roleSize(Role::Eyebrow)));
      const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, nm);
      if (ts.x <= a1.x - a0.x - 2) {
        const ImVec2 tp(std::round((a0.x + a1.x - ts.x) * 0.5f), std::round(a1.y - ts.y - k.px(1)));
        dl->AddText(f, fs, tp, note == mark ? rgb(0x1E160A) : rgb(0x3A434A), nm);
        AuditState& au = auditState();
        if (au.on) au.texts.push_back({nm, a0, a1, ts.x, fs});
      }
    }
  }
  for (int i = 0; i + 1 < whites; ++i) {
    if (!bAfter[i % 7]) continue;
    ImVec2 a0, a1;
    blackRect(i, a0, a1);
    pad(a0, a1, whiteNote(i) + 1, true);
  }
  if (hov && hv >= 0) {
    static const char* nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    a.setHint(std::string(nn[hv % 12]) + std::to_string(hv / 12 - 1) + "   " + i18n::tr("click: play, right-click: assign"));
  }
  ImGui::PopID();
  auditBox("##pads", k, x, y, w, h);
  if (hover) *hover = hv;
  return clicked;
}

}  // namespace gl::kerf
