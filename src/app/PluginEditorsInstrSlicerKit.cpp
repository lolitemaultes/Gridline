// Gridline — Kerf's own drawing kit (see PluginEditorsInstrSlicerKit.h).
#include "PluginEditorsInstrSlicerKit.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include "../core/I18n.h"
#include "App.h"
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
             const bool* enabled, const char* const* hints, ImU32 accent, float size) {
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
    iskin::text(k, tx + 2, y, tw - 4, h, labels[i], size, tc, 1, false, true);
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

int64_t snapFrame(const Sample* smp, int64_t f, int mode, double beats) {
  if (!smp || smp->frames() == 0 || mode == 0) return f;
  const int64_t n = static_cast<int64_t>(smp->frames());
  f = std::clamp<int64_t>(f, 0, n - 1);
  if (mode == 2) {
    const double step = static_cast<double>(n) / std::max(1.0, beats * 4.0);
    return std::clamp<int64_t>(std::llround(std::round(static_cast<double>(f) / step) * step), 0, n - 1);
  }
  const int64_t reach = std::max<int64_t>(1, static_cast<int64_t>(smp->sampleRate * 0.005));
  for (int64_t d = 0; d <= reach; ++d)
    for (int64_t s : {f - d, f + d}) {
      if (s <= 0 || s >= n) continue;
      const float x0 = mono(*smp, static_cast<size_t>(s - 1)), x1 = mono(*smp, static_cast<size_t>(s));
      if ((x0 <= 0 && x1 > 0) || (x0 >= 0 && x1 < 0) || x1 == 0) return s;
    }
  return f;
}

}  // namespace gl::kerf
