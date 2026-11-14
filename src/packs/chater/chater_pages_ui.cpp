// Gridline - "chater" pack: the editor's pages. The drawer under the compact tiles
// (Fracture, Glitter, Colour, Space, Motion), the preset browser and the settings sheet.
// Every graph is drawn from the parameters (and the running instance where it helps).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../app/App.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Presets.h"
#include "chater_widgets.h"

namespace gl::packs::chater::ui {

using i18n::tr;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTop = 314, kBottom = 666;

int vi(Ctx& c, int k) { return static_cast<int>(std::lround(c.v(k))); }

uint32_t h32(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}
float r01(uint32_t a, uint32_t b) { return static_cast<float>(h32(a * 0x9E3779B9U ^ h32(b)) & 0xFFFFFF) / 16777216.0f; }

// A column: its separator on the left, the dot, title and subtitle.
void section(Ctx& c, float x, float w, const char* title, const char* sub, ImU32 col, bool sep) {
  const Look& L = c.L;
  if (sep) c.dl->AddLine(c.p(x, kTop + 4), c.p(x, kBottom), L.line, c.s(1));
  rect(c, x + 14, kTop + 23, 9, 9, col, 2);
  text(c, fBold(), 13, ImVec2(x + 31, kTop + 19), L.text, title);
  const float tw = textSize(c, fBold(), 13, title).x;
  const float room = w - 28 - 17 - tw - 8 - (x + w > 1060 ? 30 : 0);  // the last column leaves room for the close cross
  if (room > 20 && sub) {
    std::string s = sub;
    while (!s.empty() && textSize(c, fReg(), 11, s.c_str()).x > room) {
      s.pop_back();
      while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
      if (!s.empty() && textSize(c, fReg(), 11, (s + "...").c_str()).x <= room) {
        s += "...";
        break;
      }
    }
    text(c, fReg(), 11, ImVec2(x + 31 + tw + 8, kTop + 21), L.text3, s.c_str());
  }
}

void graphBox(Ctx& c, float x, float y, float w, float h) { rect(c, x, y, w, h, c.L.tape, 6); }

// A small close cross (22 x 22 at x, y): the way back from a sheet or a page.
template <typename F>
void closeCross(Ctx& c, const char* id, float x, float y, const char* hint, F onClose) {
  bool hov = false;
  if (hit(c, id, x, y, 22, 22, &hov)) onClose();
  rect(c, x, y, 22, 22, hov ? theme::withAlpha(c.L.text, 0.12f) : theme::withAlpha(c.L.text, 0.05f), 5);
  const ImVec2 m = c.p(x + 11, y + 11);
  const float k = c.s(4.5f);
  const ImU32 col = hov ? c.L.text : c.L.text2;
  c.dl->AddLine(ImVec2(m.x - k, m.y - k), ImVec2(m.x + k, m.y + k), col, c.s(1.6f));
  c.dl->AddLine(ImVec2(m.x - k, m.y + k), ImVec2(m.x + k, m.y - k), col, c.s(1.6f));
  if (hov) c.a.setHint(hint);
}

void gridLines(Ctx& c, float x, float y, float w, float h, int nx, int ny) {
  const ImU32 g = theme::withAlpha(c.L.text, 0.05f);
  for (int i = 1; i < nx; ++i) c.dl->AddLine(c.p(x + w * i / nx, y), c.p(x + w * i / nx, y + h), g, c.s(1));
  for (int i = 1; i < ny; ++i) c.dl->AddLine(c.p(x, y + h * i / ny), c.p(x + w, y + h * i / ny), g, c.s(1));
}

// Knobs in a grid: `cols` per row, centred in the column, rows `rowH` apart.
void knobGrid(Ctx& c, const int* ks, int n, float x, float w, float y, int cols, float rowH, ImU32 col, float r = 17) {
  const float cw = (w - 28) / static_cast<float>(cols);
  for (int i = 0; i < n; ++i) {
    const float cx = x + 14 + cw * (static_cast<float>(i % cols) + 0.5f);
    const float cy = y + rowH * static_cast<float>(i / cols);
    knob(c, ks[i], cx, cy, r, col, KnobArc, tr(shortLabel(ks[i])));
  }
}

void footLabel(Ctx& c, float x, float y, const char* s) { text(c, fReg(), 11, ImVec2(x, y + 4), c.L.text3, s); }

void sparkle(Ctx& c, ImVec2 p, float r, ImU32 col) {
  const float k = r * 0.28f;
  const ImVec2 pts[8] = {{p.x, p.y - r}, {p.x + k, p.y - k}, {p.x + r, p.y}, {p.x + k, p.y + k},
                         {p.x, p.y + r}, {p.x - k, p.y + k}, {p.x - r, p.y}, {p.x - k, p.y - k}};
  c.dl->AddConvexPolyFilled(pts, 8, col);
}

// ---------------------------------------------------------------- Fracture

void fracture(Ctx& c) {
  const Look& L = c.L;
  const float xs[6] = {14, 232, 450, 668, 887, 1086};
  // Catch: the latest captured stretch of the input and where the loop sits
  {
    const float x = xs[0], w = xs[1] - xs[0];
    section(c, x, w, tr("Catch"), tr("Rolling capture"), L.b[0], false);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 190;
    graphBox(c, gx, gy, gw, gh);
    const double lenBeats = lengthBars(vi(c, PLength)) * (c.hasLive ? c.live.barBeats : 4.0);
    const int cols = std::clamp(static_cast<int>(lenBeats / kTapeBeats * kTapeCols), 8, kTapeCols);
    const double win = 4 * (c.hasLive ? std::max(0.25, c.live.barBeats) : 4.0);
    double inWin = c.hasLive ? std::fmod(c.live.beat - c.live.barOrigin, win) : 0;
    if (inWin < 0) inWin += win;
    const int head = static_cast<int>(inWin / win * kTapeCols);
    const int bars = 90;
    const float bw = gw / bars;
    for (int i = 0; i < bars; ++i) {
      const int col = ((head - cols + i * cols / bars) % kTapeCols + kTapeCols) % kTapeCols;
      const float pk = c.hasLive ? std::min(1.0f, c.live.peak[col] * 1.3f) : 0.0f;
      const float hh = std::max(0.5f, pk * (gh * 0.45f));
      c.dl->AddLine(c.p(gx + (i + 0.5f) * bw, gy + gh * 0.5f - hh), c.p(gx + (i + 0.5f) * bw, gy + gh * 0.5f + hh), L.wave, std::max(1.0f, c.s(bw * 0.55f)));
    }
    const bool hold = c.v(PHold) >= 0.5;
    const float px = gx + gw * (hold ? 0.5f : 0.33f);
    c.dl->AddLine(c.p(px, gy), c.p(px, gy + gh), L.accent, c.s(1.5f));
    if (hold) text(c, fMono(), 9.5f, ImVec2(gx + 6, gy + 6), L.accentInk, tr("held"));
    const int ks[3] = {PLength, PQuantise, PSmooth};
    knobGrid(c, ks, 3, x, w, kTop + 257, 3, 78, L.b[0]);
    ledToggle(c, PHold, x + 14, kTop + 316, tr("Hold"));
  }
  // Events: the odds of each break, as rows
  {
    const float x = xs[1], w = xs[2] - xs[1];
    section(c, x, w, tr("Events"), tr("Odds of each break"), L.ev[0], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 301;
    graphBox(c, gx, gy, gw, gh);
    const PluginInfo* in = findPlugin(kType);
    for (int i = 0; i < 6; ++i) {
      const int k = POddRepeat + i;
      const float ry = gy + 18 + i * 48.5f;
      text(c, fReg(), 11, ImVec2(gx + 10, ry + 3), L.text2, breakName(i + 1));
      const float bx = gx + 80, bw2 = gw - 80 - 42;
      double val = c.v(k);
      rect(c, bx, ry + 6, bw2, 6, L.cellLine, 3);
      rect(c, bx, ry + 6, bw2 * static_cast<float>(val / 100.0), 6, L.ev[i], 3);
      char n[8];
      std::snprintf(n, sizeof n, "%d", static_cast<int>(std::lround(val)));
      textR(c, fMono(), 10.5f, gx + gw - 10, ry + 2, L.text2, n);
      if (in) {
        ImGui::PushID(k);
        ImGui::PushID("ev");
        ImGui::SetCursorScreenPos(c.p(bx - 4, ry - 2));
        ImGui::InvisibleButton("##evbar", ImVec2(c.s(bw2 + 8), c.s(22)));
        pui::dragParamTrack(c.a, c.t.pid(k), in->params[static_cast<size_t>(k)], val, c.p(bx, 0).x, c.s(bw2), true);
        ImGui::PopID();
        ImGui::PopID();
      }
    }
  }
  // Repeat: the slices shrinking and rising toward the end of a break
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Repeat"), tr("Stutter and retrigger"), L.ev[0], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    const double accel = c.eff(PAccel), gate = c.eff(PGate), ramp = c.eff(PRamp);
    float sx = gx + 8;
    double len = 1;
    const int n = 6;
    double total = 0;
    for (int i = 0; i < n; ++i) total += std::pow(1 - 0.55 * std::max(0.0, accel), i) * (accel < 0 ? std::pow(1 + 0.5 * -accel, i) : 1.0);
    const float unit = (gw - 16 - 4 * (n - 1)) / static_cast<float>(total);
    for (int i = 0; i < n; ++i) {
      len = std::pow(1 - 0.55 * std::max(0.0, accel), i) * (accel < 0 ? std::pow(1 + 0.5 * -accel, i) : 1.0);
      const float sw = unit * static_cast<float>(len);
      const float lift = static_cast<float>(ramp / 24.0) * 10.0f * static_cast<float>(i) / n;
      const float top = gy + 10 + 6 * static_cast<float>(i) / n * 2 - lift, bot = gy + gh - 18;
      rect(c, sx, std::max(gy + 4, top), sw, bot - std::max(gy + 4, top), theme::withAlpha(L.ev[0], 0.22f), 2);
      const float on = sw * static_cast<float>(gate);
      const int lines = std::max(2, static_cast<int>(on / 3));
      for (int j = 0; j < lines; ++j) {
        const float t = static_cast<float>(j) / lines;
        const float amp = (0.25f + 0.75f * r01(static_cast<uint32_t>(i * 97 + j), 5)) * (1 - t * 0.5f) * (bot - top) * 0.45f;
        const float lx = sx + 1.5f + t * (on - 3);
        const float cy = (top + bot) * 0.5f;
        c.dl->AddLine(c.p(lx, cy - amp), c.p(lx, cy + amp), L.ev[0], c.s(1.2f));
      }
      sx += sw + 4;
    }
    text(c, fMono(), 9, ImVec2(gx + 8, gy + gh - 14), L.text3, display(PDivision, c.v(PDivision)).c_str());
    textR(c, fMono(), 9, gx + gw - 8, gy + gh - 14, L.text3, accel >= 0 ? tr("faster") : tr("slower"));
    const int ks[4] = {PDivision, PAccel, PRamp, PGate};
    knobGrid(c, ks, 4, x, w, kTop + 207, 2, 78, L.ev[0]);
  }
  // Tape and scrub: the speed through a tape break
  {
    const float x = xs[3], w = xs[4] - xs[3];
    section(c, x, w, tr("Tape and scrub"), tr("Stops, rises, scrubs"), L.ev[2], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    const double curve = c.eff(PTapeCurve);
    const float top = gy + 10, bot = gy + gh - 22;
    ImVec2 pts[96];
    for (int i = 0; i < 96; ++i) {
      const double t = i / 95.0;
      // the stop: speed falls to zero by the curve, then the rise back to speed
      double sp;
      if (t < 0.55) {
        const double u = t / 0.55;
        sp = 1 - std::pow(u, 0.5 + curve * 2.5);
      } else {
        const double u = (t - 0.55) / 0.45;
        sp = std::pow(u, 0.5 + (1 - curve) * 2.0);
      }
      const double wob = c.eff(PScrubDepth) * 0.12 * std::sin(t * 2 * kPi * 3);
      pts[i] = c.p(gx + 8 + static_cast<float>(t) * (gw - 16), bot - static_cast<float>(std::clamp(sp + wob * sp, 0.0, 1.0)) * (bot - top));
    }
    for (int i = 0; i < 95; ++i)
      c.dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, c.p(0, bot).y), ImVec2(pts[i].x, c.p(0, bot).y), theme::withAlpha(L.ev[2], 0.14f));
    c.dl->AddPolyline(pts, 96, L.ev[2], 0, c.s(2));
    text(c, fMono(), 9, ImVec2(gx + 8, gy + gh - 14), L.text3, tr("stop"));
    textR(c, fMono(), 9, gx + gw - 8, gy + gh - 14, L.text3, tr("rise"));
    const int ks[4] = {PTapeTime, PTapeCurve, PScrubDepth, PScrubRate};
    knobGrid(c, ks, 4, x, w, kTop + 207, 2, 78, L.ev[2]);
  }
  // Triggers: where breaks fire (the grid's chance and the input's hits)
  {
    const float x = xs[4], w = xs[5] - xs[4];
    const int mode = vi(c, PTrigMode);
    section(c, x, w, tr("Triggers"), mode == TrigHits ? tr("On the input's hits") : mode == TrigBoth ? tr("Grid and hits") : tr("By chance on the grid"), L.b[2], true);
    const int ks[6] = {PGrid, PDensity, PVariation, PSpan, PSensitivity, PHitChance};
    knobGrid(c, ks, 6, x, w, kTop + 56, 2, 72, L.b[2], 15);
    dropdown(c, PLean, x + 14, kTop + 266, w - 28, 24, tr("Lean"));
    static const char* modes[3] = {i18n::N_("Grid"), i18n::N_("Hits"), i18n::N_("Both")};
    const char* lab[3] = {tr(modes[0]), tr(modes[1]), tr(modes[2])};
    segmented(c, PTrigMode, x + 14, kTop + 306, 26, lab, 3, w - 28);
  }
}

// ---------------------------------------------------------------- Glitter

void glitter(Ctx& c) {
  const Look& L = c.L;
  const float xs[5] = {14, 332, 584, 835, 1086};
  // Grains: the live cloud (pan across, pitch up)
  {
    const float x = xs[0], w = xs[1] - xs[0];
    section(c, x, w, tr("Grains"), tr("The sparkle cloud"), L.g[2], false);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    gridLines(c, gx, gy, gw, gh, 8, 4);
    c.dl->PushClipRect(c.p(gx, gy), c.p(gx + gw, gy + gh), true);
    int alive = 0;
    if (c.hasLive)
      for (const auto& g : c.live.grains) {
        if (!g.alive) continue;
        ++alive;
        const float px = gx + gw * (0.5f + 0.48f * std::clamp(g.pan, -1.0f, 1.0f));
        const float py = gy + gh * (0.85f - 0.75f * std::clamp((g.semis + 12) / 43.0f, 0.0f, 1.0f));
        const float life = std::clamp(g.life, 0.0f, 1.0f);
        const float rr = 2 + 4 * std::sin(life * kPi) * std::min(1.0f, 0.4f + g.level);
        const ImU32 col = g.semis >= 18 ? L.g[4] : g.semis >= 6 ? L.g[1] : g.semis >= 0 ? L.g[2] : L.g[3];
        sparkle(c, c.p(px, py), c.s(rr), col);
      }
    if (!alive) {
      // at rest: where grains would land
      const int n = static_cast<int>(std::clamp(c.eff(PGrDensity) / 4.0, 3.0, 40.0));
      for (int i = 0; i < n; ++i) {
        const float px = gx + 10 + (gw - 20) * r01(static_cast<uint32_t>(i), 1);
        const float py = gy + 10 + (gh - 20) * r01(static_cast<uint32_t>(i), 2);
        sparkle(c, c.p(px, py), c.s(2 + 2 * r01(static_cast<uint32_t>(i), 3)), theme::withAlpha(L.g[i % 4 + 1], 0.35f + 0.35f * static_cast<float>(c.eff(PGrLevel))));
      }
    }
    c.dl->PopClipRect();
    const int ks[5] = {PGrDensity, PGrSize, PGrScatter, PGrReverse, PGrLevel};
    knobGrid(c, ks, 5, x, w, kTop + 177, 3, 78, L.g[2]);
    dropdown(c, PGrWindow, x + 14, kTop + 316, 104, 24, tr("Window"));
  }
  // Pitch: the intervals a grain may take
  {
    const float x = xs[1], w = xs[2] - xs[1];
    section(c, x, w, tr("Pitch"), tr("Intervals each grain may take"), L.g[3], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 222;
    graphBox(c, gx, gy, gw, gh);
    const int mask = vi(c, PIntervals);
    const float cw = (gw - 32 - 18) / 4, chh = 24;
    const float cy0 = gy + gh * 0.5f - chh - 3;
    for (int i = 0; i < kChips; ++i) {
      const float cx = gx + 16 + (i % 4) * (cw + 6), cy = cy0 + (i / 4) * (chh + 7);
      const bool on = mask & (1 << i);
      char id[12], lab[8];
      std::snprintf(id, sizeof id, "##chip%d", i);
      std::snprintf(lab, sizeof lab, kChipSemis[static_cast<size_t>(i)] > 0 ? "+%d" : "%d", kChipSemis[static_cast<size_t>(i)]);
      bool hov = false;
      if (hit(c, id, cx, cy, cw, chh, &hov)) setParam(c, PIntervals, static_cast<double>(mask ^ (1 << i)));
      rect(c, cx, cy, cw, chh, on ? theme::withAlpha(L.g[3], 0.22f) : (hov ? theme::withAlpha(L.text, 0.06f) : L.cell), 4);
      if (on) rectLine(c, cx, cy, cw, chh, L.g[3], 4);
      textC(c, fMono(), 10.5f, cx + cw * 0.5f, cy + 5, on ? L.text : L.text3, lab);
    }
    const int ks[2] = {PReach, PDetune};
    knobGrid(c, ks, 2, x, w, kTop + 286, 2, 78, L.g[3]);
  }
  // Scale: a keyboard of the notes grains snap to
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Scale"), tr("Snaps grains to a key"), L.g[1], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 190;
    graphBox(c, gx, gy, gw, gh);
    const int scale = vi(c, PScale), root = vi(c, PRoot);
    const int mask = scaleMask(scale);
    auto inScale = [&](int pc) { return scale != 0 && (mask >> (((pc - root) % 12 + 12) % 12) & 1); };
    static const int white[7] = {0, 2, 4, 5, 7, 9, 11};
    static const char* names[7] = {"C", "D", "E", "F", "G", "A", "B"};
    const float kx = gx + 6, ky = gy + 6, kw = (gw - 12) / 7, kh = gh - 46;
    for (int i = 0; i < 7; ++i) {
      const bool on = inScale(white[i]);
      rect(c, kx + i * kw + 1, ky, kw - 2, kh, on ? theme::mix(L.g[1], L.white, i == 0 || white[i] == root ? 0.0f : 0.35f) : theme::rgb(0xD9E1E5), 2);
      rectLine(c, kx + i * kw + 1, ky, kw - 2, kh, theme::withAlpha(L.ink, 0.25f), 2);
      textC(c, fBold(), 9.5f, kx + i * kw + kw * 0.5f, ky + kh - 15, L.ink, names[i]);
    }
    static const int black[5] = {1, 3, 6, 8, 10};
    static const float bpos[5] = {1, 2, 4, 5, 6};
    for (int i = 0; i < 5; ++i) {
      const bool on = inScale(black[i]);
      const float bx = kx + bpos[i] * kw - kw * 0.3f;
      rect(c, bx, ky, kw * 0.6f, kh * 0.6f, on ? theme::mix(L.g[1], L.ink, 0.35f) : L.ink, 2);
    }
    dropdown(c, PScale, gx + 6, gy + gh - 34, gw * 0.56f, 24);
    dropdown(c, PRoot, gx + 12 + gw * 0.56f, gy + gh - 34, gw * 0.44f - 18, 24, tr("Root"));
    const int ks[1] = {PSnap};
    knobGrid(c, ks, 1, x, w, kTop + 254, 1, 78, L.g[1]);
  }
  // Shimmer: the feedback climbing up the interval
  {
    const float x = xs[3], w = xs[4] - xs[3];
    section(c, x, w, tr("Shimmer"), tr("Feedback up an interval"), L.g[4], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 190;
    graphBox(c, gx, gy, gw, gh);
    const double fb = c.eff(PShFeedback);
    const int step = shimmerSemis(vi(c, PShInterval));
    const float bw = (gw - 20) / 6;
    double lvl = 1;
    for (int i = 0; i < 6; ++i) {
      const float bh = static_cast<float>(lvl) * (gh - 34);
      const float bx = gx + 10 + i * bw + bw * 0.3f;
      rect(c, bx, gy + gh - 10 - bh, bw * 0.4f, bh, theme::mix(L.g[4], L.tape, i * 0.1f), 1);
      char lab[8];
      std::snprintf(lab, sizeof lab, "+%d", step * i);
      textC(c, fMono(), 8.5f, bx + bw * 0.2f, gy + gh - 22 - bh, L.text3, lab);
      lvl *= std::max(0.04, fb);
    }
    const int ks[3] = {PShFeedback, PShTone, PShSpread};
    knobGrid(c, ks, 3, x, w, kTop + 254, 3, 78, L.g[4]);
    footLabel(c, x + 14, kTop + 316, tr("Interval"));
    const char* lab[3] = {"+7", "+12", "+19"};
    segmented(c, PShInterval, x + 14 + textSize(c, fReg(), 11, tr("Interval")).x + 8, kTop + 316, 22, lab, 3);
  }
}

// ---------------------------------------------------------------- Colour

void colour(Ctx& c) {
  const Look& L = c.L;
  const float xs[4] = {14, 372, 729, 1086};
  // Crystal: six resonator peaks
  {
    const float x = xs[0], w = xs[1] - xs[0];
    section(c, x, w, tr("Crystal"), tr("Six tuned resonators"), L.b[3], false);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    gridLines(c, gx, gy, gw, gh, 6, 3);
    const int chord = vi(c, PCrChord);
    const double dec = c.eff(PCrDecay), br = c.eff(PCrBright);
    const float base = gy + gh - 10;
    ImVec2 pts[160];
    for (int i = 0; i < 160; ++i) {
      const float t = i / 159.0f;
      float y = 0;
      for (int v = 0; v < 6; ++v) {
        const float semi = static_cast<float>(chordSemis(chord, v));
        const float pos = 0.08f + semi / 40.0f * 0.84f;
        const float live = c.hasLive ? std::min(1.0f, c.live.crystal[v] * 3) : 0.0f;
        const float amp = (0.55f + 0.4f * static_cast<float>(br) * (1 - v / 8.0f)) * (0.75f + 0.25f * live);
        const float wdt = 0.012f / static_cast<float>(std::sqrt(std::max(0.1, dec)));
        y = std::max(y, amp * std::exp(-(t - pos) * (t - pos) / (wdt * wdt)));
      }
      pts[i] = c.p(gx + 4 + t * (gw - 8), base - y * (gh - 22));
    }
    c.dl->AddPolyline(pts, 160, L.b[3], 0, c.s(2));
    const int ks[5] = {PCrDecay, PCrBright, PCrStrike, PCrSpread, PCrMix};
    knobGrid(c, ks, 5, x, w, kTop + 177, 3, 78, L.b[3]);
    footLabel(c, x + 14, kTop + 316, tr("Chord"));
    const char* lab[5] = {tr("Octaves"), tr("Fifths"), tr("Triad"), tr("Sus"), tr("Ninth")};
    segmented(c, PCrChord, x + 14 + textSize(c, fReg(), 11, tr("Chord")).x + 8, kTop + 316, 22, lab, 5);
  }
  // Halo: the lifted highs over a spectrum
  {
    const float x = xs[1], w = xs[2] - xs[1];
    section(c, x, w, tr("Halo"), tr("Lifts and twinkles the highs"), L.b[4], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    const double from = c.v(PHaFrom), lift = c.eff(PHaLift), twk = c.eff(PHaTwinkle);
    const float fx = static_cast<float>(std::log(from / 20.0) / std::log(1000.0));
    ImVec2 a[120], b[120];
    for (int i = 0; i < 120; ++i) {
      const float t = i / 119.0f;
      const float n = 0.5f * r01(static_cast<uint32_t>(i), 9) + 0.5f * r01(static_cast<uint32_t>(i / 3), 10);
      const float sp = 0.62f - 0.35f * t + 0.12f * n;
      const float up = t > fx ? static_cast<float>(lift) * 0.35f * std::min(1.0f, (t - fx) * 8) : 0.0f;
      b[i] = c.p(gx + t * gw, gy + gh * (1 - sp));
      a[i] = c.p(gx + t * gw, gy + gh * (1 - std::min(0.95f, sp + up)));
    }
    c.dl->AddPolyline(b, 120, theme::withAlpha(L.text3, 0.7f), 0, c.s(1));
    c.dl->AddPolyline(a, 120, L.b[4], 0, c.s(1.8f));
    for (float dy = gy; dy < gy + gh; dy += 6) c.dl->AddLine(c.p(gx + fx * gw, dy), c.p(gx + fx * gw, dy + 3), theme::withAlpha(L.b[4], 0.6f), c.s(1));
    const int stars = static_cast<int>(twk * 10);
    for (int i = 0; i < stars; ++i)
      sparkle(c, c.p(gx + (fx + (1 - fx) * r01(static_cast<uint32_t>(i), 11)) * gw - 4, gy + 10 + 30 * r01(static_cast<uint32_t>(i), 12)), c.s(3), L.b[4]);
    const int ks[5] = {PHaLift, PHaFrom, PHaTwinkle, PHaSmear, PHaMix};
    knobGrid(c, ks, 5, x, w, kTop + 177, 3, 78, L.b[4]);
    ledToggle(c, PHaFreeze, x + 14, kTop + 316, tr("Freeze"));
  }
  // Grit: a sine through the bits and the rate
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Grit"), tr("Bits and rate, cleanly"), L.b[5], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    const double bits = c.eff(PGtBits), rate = c.eff(PGtRate);
    const float levels = static_cast<float>(std::pow(2.0, std::min(bits, 6.0) - 1));
    const int holds = std::clamp(static_cast<int>(200 * rate / 48000.0), 6, 200);
    ImVec2 s[160], q[400];
    int nq = 0;
    for (int i = 0; i < 160; ++i) {
      const float t = i / 159.0f;
      s[i] = c.p(gx + t * gw, gy + gh * 0.5f - std::sin(t * 2 * kPi * 1.5f) * gh * 0.38f);
    }
    float prevY = 0;
    for (int i = 0; i < holds && nq < 398; ++i) {
      const float t0 = static_cast<float>(i) / holds, t1 = static_cast<float>(i + 1) / holds;
      const float v = std::round(std::sin(t0 * 2 * kPi * 1.5f) * levels) / levels;
      const float y = gy + gh * 0.5f - v * gh * 0.38f;
      if (i > 0) q[nq++] = c.p(gx + t0 * gw, prevY);
      q[nq++] = c.p(gx + t0 * gw, y);
      if (i == holds - 1) q[nq++] = c.p(gx + t1 * gw, y);
      prevY = y;
    }
    c.dl->AddPolyline(s, 160, theme::withAlpha(L.text3, 0.6f), 0, c.s(1));
    c.dl->AddPolyline(q, nq, L.b[5], 0, c.s(2));
    const int ks[5] = {PGtBits, PGtRate, PGtJitter, PGtDrive, PGtMix};
    knobGrid(c, ks, 5, x, w, kTop + 177, 3, 78, L.b[5]);
    footLabel(c, x + 14, kTop + 316, tr("Smooth"));
    const char* lab[3] = {tr("Raw"), tr("Soft"), tr("Clean")};
    segmented(c, PGtSmooth, x + 14 + textSize(c, fReg(), 11, tr("Smooth")).x + 8, kTop + 316, 22, lab, 3);
  }
}

// ---------------------------------------------------------------- Space

void space(Ctx& c) {
  const Look& L = c.L;
  const float xs[5] = {14, 282, 550, 818, 1086};
  {
    const float x = xs[0], w = xs[1] - xs[0];
    section(c, x, w, tr("Spray"), tr("Where sparkles land"), L.b[6], false);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    c.dl->AddLine(c.p(gx + gw * 0.5f, gy + 6), c.p(gx + gw * 0.5f, gy + gh - 6), theme::withAlpha(L.text, 0.08f), c.s(1));
    text(c, fReg(), 9.5f, ImVec2(gx + 6, gy + gh * 0.5f - 6), L.text3, "L");
    textR(c, fReg(), 9.5f, gx + gw - 6, gy + gh * 0.5f - 6, L.text3, "R");
    const float spread = static_cast<float>(std::min(2.0, c.eff(PWidth)) / 2 * (0.4 + 0.6 * c.eff(PPanScatter)));
    for (int i = 0; i < 40; ++i) {
      const float pan = (r01(static_cast<uint32_t>(i), 21) * 2 - 1) * spread;
      const float py = gy + 10 + (gh - 20) * r01(static_cast<uint32_t>(i), 22);
      sparkle(c, c.p(gx + gw * (0.5f + 0.45f * pan), py), c.s(2 + 2 * r01(static_cast<uint32_t>(i), 23)), L.g[i % 4 + 1]);
    }
    const int ks[4] = {PWidth, PPanScatter, PMicroDelay, POrbit};
    knobGrid(c, ks, 4, x, w, kTop + 207, 2, 78, L.b[6]);
  }
  {
    const float x = xs[1], w = xs[2] - xs[1];
    section(c, x, w, tr("Room"), tr("A long, soft hall"), L.b[7], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    const double dec = c.eff(PRmDecay), size = c.eff(PRmSize), damp = c.eff(PRmDamping);
    const float base = gy + gh - 6;
    for (int i = 0; i < 60; ++i) {
      const float t = i / 60.0f;
      const float env = static_cast<float>(std::exp(-t * 12 / std::max(0.2, dec)));
      const float hh = env * (gh - 20) * (0.35f + 0.65f * r01(static_cast<uint32_t>(i), 31)) * static_cast<float>(1 - damp * 0.4);
      const float bx = gx + 4 + t * (gw - 8) * static_cast<float>(0.5 + 0.5 * size) + 2;
      c.dl->AddLine(c.p(bx, base), c.p(bx, base - hh), theme::withAlpha(L.b[7], 0.55f), c.s(2));
    }
    ImVec2 pts[100];
    for (int i = 0; i < 100; ++i) {
      const float t = i / 99.0f;
      const float rise = std::min(1.0f, t * 30);
      const float env = rise * static_cast<float>(std::exp(-t * 12 / std::max(0.2, dec)));
      pts[i] = c.p(gx + 4 + t * (gw - 8), base - env * (gh - 18));
    }
    c.dl->AddPolyline(pts, 100, L.b[7], 0, c.s(2));
    const double shim = c.eff(PRmShimmer);
    for (int i = 0; i < static_cast<int>(shim * 12); ++i)  // shimmer: sparks rising out of the tail
      sparkle(c, c.p(gx + 18 + (gw - 36) * r01(static_cast<uint32_t>(i), 33), gy + 10 + (gh * 0.45f) * r01(static_cast<uint32_t>(i), 34)), c.s(2.5f), L.g[4]);
    if (c.v(PRmFreeze) >= 0.5) {  // frozen: the tail held as a band, glowing with the live level
      const float glow = c.hasLive ? std::min(1.0f, c.live.room * 4) : 0.5f;
      c.dl->AddRectFilled(c.p(gx + 2, gy + 2), c.p(gx + gw - 2, gy + gh - 2), theme::withAlpha(L.b[7], 0.08f + 0.12f * glow), c.s(5));
      textR(c, fBold(), 9.5f, gx + gw - 8, gy + 6, L.b[7], tr("FROZEN"));
    }
    const int ks[6] = {PRmSize, PRmDecay, PRmDamping, PRmShimmer, PRmMod, PRmMix};
    knobGrid(c, ks, 6, x, w, kTop + 177, 3, 78, L.b[7]);
    ledToggle(c, PRmFreeze, x + 14, kTop + 316, tr("Freeze"));
  }
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Echo"), tr("Tempo echo"), L.b[8], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    const float mid = gy + gh * 0.5f;
    c.dl->AddLine(c.p(gx + 4, mid), c.p(gx + gw - 4, mid), theme::withAlpha(L.text, 0.1f), c.s(1));
    textR(c, fReg(), 8.5f, gx + gw - 6, gy + 4, L.text3, "L");
    textR(c, fReg(), 8.5f, gx + gw - 6, gy + gh - 14, L.text3, "R");
    const double fb = c.eff(PEcFeedback), beats = echoBeats(vi(c, PEcTime));
    const bool pp = c.v(PEcPingPong) >= 0.5;
    const float stepX = std::max(10.0f, (gw - 24) * static_cast<float>(beats / 2.0));
    double lv = 1;
    for (int i = 0; gx + 12 + i * stepX < gx + gw - 12 && i < 40; ++i) {
      const float bx = gx + 12 + i * stepX;
      const float hh = static_cast<float>(lv) * (gh * 0.45f - 6);
      const bool down = pp && (i % 2 == 1);
      rect(c, bx - 3, down ? mid : mid - hh, 6, hh, i == 0 ? L.b[8] : theme::withAlpha(L.b[8], 0.75f), 1);
      lv *= fb;
    }
    const int ks[5] = {PEcTime, PEcFeedback, PEcTone, PEcDiffuse, PEcMix};
    knobGrid(c, ks, 5, x, w, kTop + 177, 3, 78, L.b[8]);
    ledToggle(c, PEcPingPong, x + 14, kTop + 316, tr("Ping-pong"));
  }
  {
    const float x = xs[3], w = xs[4] - xs[3];
    section(c, x, w, tr("Tone and output"), tr("Last stage"), L.b[9], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    gridLines(c, gx, gy, gw, gh, 5, 4);
    const double lc = c.eff(PLowCut), hc = c.eff(PHighCut), tilt = c.eff(PTilt);
    ImVec2 pts[120];
    for (int i = 0; i < 120; ++i) {
      const double t = i / 119.0;
      const double f = 20 * std::pow(1000.0, t);
      double db = -10 * std::log10(1 + std::pow(lc / f, 4)) - 10 * std::log10(1 + std::pow(f / hc, 4));
      db += tilt * std::log2(f / 1000.0) / 5.0;
      pts[i] = c.p(gx + static_cast<float>(t) * gw, gy + gh * 0.32f - static_cast<float>(std::clamp(db, -30.0, 9.0)) / 30.0f * gh * 0.62f);
    }
    c.dl->AddPolyline(pts, 120, L.b[9], 0, c.s(2));
    const int ks[6] = {PLowCut, PHighCut, PTilt, PDuck, PMix, POutput};
    knobGrid(c, ks, 6, x, w, kTop + 177, 3, 78, L.b[9]);
    ledToggle(c, PSafety, x + 14, kTop + 316, tr("Safety limit"));
  }
}

// ---------------------------------------------------------------- Haze

void haze(Ctx& c) {
  const Look& L = c.L;
  const float xs[5] = {14, 282, 550, 818, 1086};
  const ImU32 hz = L.tile[PHaze], mem = L.tile[PMemory];
  // Damp: the wet path's high cut and warmth over a soft spectrum
  {
    const float x = xs[0], w = xs[1] - xs[0];
    section(c, x, w, tr("Damp"), tr("Softer, warmer wet sound"), hz, false);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    gridLines(c, gx, gy, gw, gh, 5, 4);
    const double fc = c.eff(PDamp), warm = c.eff(PWarmth);
    ImVec2 pts[120];
    for (int i = 0; i < 120; ++i) {
      const double t = i / 119.0;
      const double f = 20 * std::pow(1000.0, t);
      double db = fc >= 19900 ? 0.0 : -10 * std::log10(1 + std::pow(f / fc, 4));
      db += warm * 2.5 * std::exp(-std::pow(std::log2(f / 250.0), 2) / 4);
      pts[i] = c.p(gx + static_cast<float>(t) * gw, gy + gh * 0.3f - static_cast<float>(std::clamp(db, -30.0, 9.0)) / 30.0f * gh * 0.62f);
    }
    for (int i = 0; i < 119; ++i)
      c.dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, c.p(0, gy + gh).y), ImVec2(pts[i].x, c.p(0, gy + gh).y), theme::withAlpha(hz, 0.10f));
    c.dl->AddPolyline(pts, 120, hz, 0, c.s(2));
    const int ks[2] = {PDamp, PWarmth};
    knobGrid(c, ks, 2, x, w, kTop + 207, 2, 78, hz);
  }
  // Soft breaks: a break's shape with its edges and its washed tail
  {
    const float x = xs[1], w = xs[2] - xs[1];
    section(c, x, w, tr("Soft breaks"), tr("Glitches that blur"), hz, true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    const float soft = static_cast<float>(c.eff(PSoftness)), wash = static_cast<float>(c.eff(PWash));
    const float base = gy + gh - 10, top = gy + 18;
    const float x0 = gx + 16, x1 = gx + gw * 0.62f;
    const float edge = 2 + soft * (x1 - x0) * 0.3f;
    // the repeats: four slices, each a little lower when soft
    const float sw = (x1 - x0) / 4;
    for (int r = 0; r < 4; ++r) {
      const float lv = std::pow(1.0f - soft * 0.15f, static_cast<float>(r));
      const float sx = x0 + r * sw, e = std::min(edge, sw * 0.45f);
      const float hy = base - (base - top) * lv;
      const ImVec2 q[4] = {c.p(sx, base), c.p(sx + e, hy), c.p(sx + sw - e, hy), c.p(sx + sw, base)};
      c.dl->AddConvexPolyFilled(q, 4, theme::withAlpha(L.ev[0], 0.35f + 0.15f * (1 - r / 4.0f)));
      c.dl->AddPolyline(q, 4, L.ev[0], 0, c.s(1.5f));
    }
    // the washed tail into the space
    if (wash > 0.01f) {
      ImVec2 t[40];
      for (int i = 0; i < 40; ++i) {
        const float u = i / 39.0f;
        const float a = wash * std::exp(-u * 3.0f) * (0.8f + 0.2f * r01(static_cast<uint32_t>(i), 41));
        t[i] = c.p(x1 + u * (gx + gw - 10 - x1), base - (base - top) * 0.75f * a);
      }
      c.dl->AddPolyline(t, 40, L.b[7], 0, c.s(1.8f));
      footLabel(c, x1 + 6, gy + 4, tr("into the space"));
    }
    const int ks[2] = {PSoftness, PWash};
    knobGrid(c, ks, 2, x, w, kTop + 207, 2, 78, hz);
  }
  // Wow and flutter: the tape's pitch line, live
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Wow and flutter"), tr("A tape that wavers"), mem, true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
    graphBox(c, gx, gy, gw, gh);
    const float mid = gy + gh * 0.5f;
    c.dl->AddLine(c.p(gx + 4, mid), c.p(gx + gw - 4, mid), theme::withAlpha(L.text, 0.1f), c.s(1));
    const double wow = c.eff(PWow), fl = c.eff(PFlutter), rate = c.eff(PWowRate), ens = c.eff(PEnsemble);
    const float ph = static_cast<float>(ImGui::GetTime() * 0.4);
    for (int v = 0; v < (ens > 0.01 ? 3 : 1); ++v) {
      ImVec2 pts[120];
      for (int i = 0; i < 120; ++i) {
        const float t = i / 119.0f;
        const float y = static_cast<float>(wow * 0.8 * std::sin((t * 4 * rate + ph) * 2 * kPi + v * 2.1f) + fl * 0.25 * std::sin(t * 60 + v));
        pts[i] = c.p(gx + 4 + t * (gw - 8), mid - y * (gh * 0.42f) + (v ? (v == 1 ? -8.f : 8.f) * static_cast<float>(ens) : 0.f));
      }
      c.dl->AddPolyline(pts, 120, v ? theme::withAlpha(mem, 0.45f) : mem, 0, c.s(v ? 1.2f : 2));
    }
    if (c.hasLive) {
      char s[24];
      std::snprintf(s, sizeof s, "%+.1f ct", static_cast<double>(c.live.wow));
      textR(c, fMono(), 9.5f, gx + gw - 6, gy + 4, L.text3, s);
    }
    const int ks[4] = {PWow, PWowRate, PFlutter, PEnsemble};
    knobGrid(c, ks, 4, x, w, kTop + 177, 2, 78, mem);
  }
  // Tape: age, hiss and crackle
  {
    const float x = xs[3], w = xs[4] - xs[3];
    section(c, x, w, tr("Tape"), tr("Worn, warm, a little noisy"), mem, true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    const double age = c.eff(PAge), hiss = c.eff(PHiss), crk = c.eff(PCrackle);
    // two reels and the tape between them, yellowing with age
    const ImU32 reel = theme::mix(L.text2, theme::rgb(0xD9B26A), static_cast<float>(age));
    const float ry = gy + gh * 0.42f, rr = gh * 0.26f;
    for (int s = 0; s < 2; ++s) {
      const ImVec2 ctr = c.p(gx + gw * (s ? 0.72f : 0.28f), ry);
      c.dl->AddCircle(ctr, c.s(rr), reel, 32, c.s(2));
      c.dl->AddCircle(ctr, c.s(rr * 0.3f), reel, 16, c.s(1.5f));
      const float a = static_cast<float>(ImGui::GetTime() * 0.8) + s;
      for (int k = 0; k < 3; ++k) {
        const float an = a + k * 2.094f;
        c.dl->AddLine(ImVec2(ctr.x + std::cos(an) * c.s(rr * 0.32f), ctr.y + std::sin(an) * c.s(rr * 0.32f)),
                      ImVec2(ctr.x + std::cos(an) * c.s(rr * 0.85f), ctr.y + std::sin(an) * c.s(rr * 0.85f)), reel, c.s(1.2f));
      }
    }
    c.dl->AddLine(c.p(gx + gw * 0.28f, ry + rr), c.p(gx + gw * 0.72f, ry + rr), reel, c.s(1.5f));
    // hiss as fine dust, crackle as specks along the bottom
    for (int i = 0; i < static_cast<int>(hiss * 60); ++i)
      c.dl->AddCircleFilled(c.p(gx + 6 + (gw - 12) * r01(static_cast<uint32_t>(i), 51), gy + gh - 22 + 16 * r01(static_cast<uint32_t>(i), 52)), c.s(0.8f),
                            theme::withAlpha(L.text2, 0.6f));
    for (int i = 0; i < static_cast<int>(crk * 14); ++i)
      c.dl->AddCircleFilled(c.p(gx + 6 + (gw - 12) * r01(static_cast<uint32_t>(i), 53), gy + gh - 22 + 16 * r01(static_cast<uint32_t>(i), 54)), c.s(1.8f), mem);
    const int ks[3] = {PAge, PHiss, PCrackle};
    knobGrid(c, ks, 3, x, w, kTop + 207, 3, 78, mem);
  }
}

// ---------------------------------------------------------------- Motion

float swayValue(int shape, double ph) {
  ph -= std::floor(ph);
  switch (shape) {
    case ShTri: return static_cast<float>(ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4);
    case ShRamp: return static_cast<float>(ph * 2 - 1);
    case ShSteps: {
      static const float s[8] = {0.2f, -0.6f, 0.9f, -0.1f, 0.5f, -0.9f, 0.7f, -0.3f};
      return s[static_cast<int>(ph * 8) & 7];
    }
    case ShDrift: return std::sin(static_cast<float>(ph) * 2 * kPi) * 0.6f + std::sin(static_cast<float>(ph) * 6 * kPi + 1) * 0.3f;
    default: return std::sin(static_cast<float>(ph) * 2 * kPi);
  }
}

void swayColumn(Ctx& c, int sw, float x, float w) {
  const Look& L = c.L;
  const ImU32 col = L.l[sw];
  char sub[64];
  std::snprintf(sub, sizeof sub, "%s, %s", tr("Tempo"), display(swayParam(sw, SwRate), c.v(swayParam(sw, SwRate))).c_str());
  section(c, x, w, sw == 0 ? tr("Sway A") : tr("Sway B"), sub, col, sw != 0);
  const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 112;
  graphBox(c, gx, gy, gw, gh);
  gridLines(c, gx, gy, gw, gh, 4, 2);
  const int shape = vi(c, swayParam(sw, SwShape));
  const double depth = c.eff(swayParam(sw, SwDepth)), phase = c.v(swayParam(sw, SwPhase)) / 360.0;
  ImVec2 pts[120];
  for (int i = 0; i < 120; ++i) {
    const double t = i / 119.0;
    const float v = swayValue(shape, t * 2 + phase) * static_cast<float>(std::max(0.15, depth));
    pts[i] = c.p(gx + static_cast<float>(t) * gw, gy + gh * 0.5f - v * gh * 0.42f);
  }
  c.dl->AddPolyline(pts, 120, col, 0, c.s(2));
  if (c.hasLive) {
    const float v = sw == 0 ? c.live.swayA : c.live.swayB;
    const double rb = swayBeats(vi(c, swayParam(sw, SwRate)));
    double ph = rb > 0 ? std::fmod(c.live.beat / rb, 2.0) / 2.0 : 0;
    if (ph < 0) ph += 1;
    c.dl->AddCircleFilled(c.p(gx + static_cast<float>(ph) * gw, gy + gh * 0.5f - std::clamp(v, -1.0f, 1.0f) * gh * 0.42f), c.s(4), col, 12);
  }
  const int ks[4] = {swayParam(sw, SwRate), swayParam(sw, SwDepth), swayParam(sw, SwPhase), swayParam(sw, SwSmooth)};
  knobGrid(c, ks, 4, x, w, kTop + 177, 2, 78, col);
  dropdown(c, swayParam(sw, SwShape), x + 14, kTop + 316, std::min(150.0f, w - 28), 24, tr("Shape"));
}

void motion(Ctx& c) {
  const Look& L = c.L;
  const float xs[5] = {14, 244, 474, 704, 1086};
  swayColumn(c, 0, xs[0], xs[1] - xs[0]);
  swayColumn(c, 1, xs[1], xs[2] - xs[1]);
  // Follower
  {
    const float x = xs[2], w = xs[3] - xs[2];
    section(c, x, w, tr("Follower"), tr("Input loudness"), L.l[2], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 144;
    graphBox(c, gx, gy, gw, gh);
    if (c.hasLive && c.now - c.u.foLast > 1.0 / 30) {
      c.u.foLast = c.now;
      c.u.foHist[c.u.foAt] = c.live.follower;
      c.u.foAt = (c.u.foAt + 1) % 120;
    }
    ImVec2 pts[120];
    for (int i = 0; i < 120; ++i) {
      const float v = std::clamp(c.u.foHist[(c.u.foAt + i) % 120], 0.0f, 1.0f);
      pts[i] = c.p(gx + 4 + (gw - 8) * i / 119.0f, gy + gh - 8 - v * (gh - 16));
      c.dl->AddLine(ImVec2(pts[i].x, c.p(0, gy + gh - 8).y), pts[i], theme::withAlpha(L.text3, 0.35f), c.s(1.0f));
    }
    c.dl->AddPolyline(pts, 120, L.l[2], 0, c.s(2));
    const int ks[4] = {PFoAttack, PFoRelease, PFoGain, PFoFloor};
    knobGrid(c, ks, 4, x, w, kTop + 207, 2, 78, L.l[2]);
  }
  // Links
  {
    const float x = xs[3], w = xs[4] - xs[3];
    int used = 0;
    for (int l = 0; l < kLinks; ++l) used += vi(c, linkParam(l, LkSource)) != SrcOff;
    char sub[32];
    std::snprintf(sub, sizeof sub, "%d %s %d %s", used, tr("of"), kLinks, tr("used"));
    section(c, x, w, tr("Links"), sub, L.l[3], true);
    const float gx = x + 14, gy = kTop + 39, gw = w - 28, gh = 301;
    graphBox(c, gx, gy, gw, gh);
    static const char* srcN[SrcCount] = {"", i18n::N_("Sway A"), i18n::N_("Sway B"), i18n::N_("Follower"), i18n::N_("Breaks")};
    const ImU32 srcC[SrcCount] = {L.text3, L.l[0], L.l[1], L.l[2], L.l[4]};
    const PluginInfo* in = findPlugin(kType);
    float ry = gy + 8;
    for (int l = 0; l < kLinks && in; ++l) {
      const int src = vi(c, linkParam(l, LkSource));
      if (src == SrcOff || src >= SrcCount) continue;
      const int tgt = vi(c, linkParam(l, LkTarget));
      const int dk = linkParam(l, LkDepth);
      c.dl->AddLine(c.p(gx + 8, ry + 27), c.p(gx + gw - 8, ry + 27), L.cellLine, c.s(1));
      c.dl->AddCircleFilled(c.p(gx + 14, ry + 13.5f), c.s(3.5f), srcC[src], 12);
      text(c, fReg(), 11, ImVec2(gx + 24, ry + 7), L.text2, tr(srcN[src]));
      text(c, fReg(), 11, ImVec2(gx + 100, ry + 7), L.text3, "\xE2\x86\x92");
      const char* tn = tgt >= 0 && tgt < PCount ? in->params[static_cast<size_t>(tgt)].name.c_str() : "";
      text(c, fReg(), 11, ImVec2(gx + 120, ry + 7), L.text, tr(tn));
      double dv = c.v(dk);
      char n[16];
      std::snprintf(n, sizeof n, "%+d%%", static_cast<int>(std::lround(dv * 100)));
      textR(c, fMono(), 10.5f, gx + gw - 32, ry + 8, dv >= 0 ? L.led : theme::lit(theme::rgb(0xFF6F6F)), n);
      if (!c.u.settings && !c.u.dice) {
        ImGui::PushID(dk);
        ImGui::SetCursorScreenPos(c.p(gx + gw - 90, ry + 2));
        ImGui::InvisibleButton("##depth", ImVec2(c.s(58), c.s(22)));
        pui::dragParamItem(c.a, c.t.pid(dk), in->params[static_cast<size_t>(dk)], dv, 160.0f, true);
        ImGui::PopID();
      }
      char id[16];
      std::snprintf(id, sizeof id, "##rmlink%d", l);
      bool hov = false;
      if (hit(c, id, gx + gw - 26, ry + 4, 18, 18, &hov)) {
        std::vector<float> p = c.st.params;
        p[static_cast<size_t>(linkParam(l, LkSource))] = SrcOff;
        p[static_cast<size_t>(dk)] = 0;
        setState(c, p, tr("Remove link"));
      }
      textC(c, fBold(), 12, gx + gw - 17, ry + 5, hov ? L.text : L.text3, "\xC3\x97");
      ry += 30;
    }
    if (used < kLinks) {
      for (float dx = gx + 8; dx < gx + gw - 8; dx += 8) {
        c.dl->AddLine(c.p(dx, ry + 6), c.p(std::min(dx + 4, gx + gw - 8), ry + 6), L.cellLine, c.s(1));
        c.dl->AddLine(c.p(dx, ry + 32), c.p(std::min(dx + 4, gx + gw - 8), ry + 32), L.cellLine, c.s(1));
      }
      text(c, fReg(), 11, ImVec2(gx + 16, ry + 12), L.text3, tr("+ Right-click any knob, then Link to"));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- the drawer

void drawDrawer(Ctx& c, float y0) {
  (void)y0;
  const Look& L = c.L;
  const int tile = shownTile(c.u);
  const ImU32 col = tile >= 0 ? L.tile[tile] : L.l[0];
  rect(c, 14, kTop, 1072, kBottom - kTop, L.panel, 10);
  c.dl->AddRectFilled(c.p(14, kTop), c.p(1086, kTop + 4), col, c.s(10), ImDrawFlags_RoundCornersTop);
  const float cx = tile >= 0 ? 14 + tile * 135 + 63.5f : 1010;
  c.dl->AddTriangleFilled(c.p(cx - 9, kTop), c.p(cx + 9, kTop), c.p(cx, kTop - 8), col);
  switch (c.u.page) {
    case PgFracture: fracture(c); break;
    case PgGlitter: glitter(c); break;
    case PgColour: colour(c); break;
    case PgSpace: space(c); break;
    case PgHaze: haze(c); break;
    case PgMotion: motion(c); break;
    default: break;
  }
  closeCross(c, "##pageclose", 1058, kTop + 14, tr("Back to the main page"), [&] { c.u.page = PgMain; });
}

// ---------------------------------------------------------------- settings

namespace {

float segLabels(Ctx& c, const char* id, float right, float y, const char* const* labels, int count, int cur, int* picked) {
  const Look& L = c.L;
  float widths[8], total = 4;
  for (int i = 0; i < count; ++i) widths[i] = textSize(c, fMono(), 10.5f, labels[i]).x + 16, total += widths[i] + 2;
  const float x = right - total, h = 24;
  rect(c, x, y, total, h, L.cell, 4);
  rectLine(c, x, y, total, h, L.line, 4);
  float cx = x + 2;
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    char bid[8];
    std::snprintf(bid, sizeof bid, "##%d", i);
    bool hov = false;
    if (hit(c, bid, cx, y + 2, widths[i], h - 4, &hov)) *picked = i;
    if (i == cur) {
      rect(c, cx, y + 2, widths[i], h - 4, L.accentBg, 3);
      rectLine(c, cx, y + 2, widths[i], h - 4, L.accent, 3);
    }
    textC(c, fMono(), 10.5f, cx + widths[i] * 0.5f, y + 5, i == cur ? L.accentInk : L.text2, labels[i]);
    cx += widths[i] + 2;
  }
  ImGui::PopID();
  return total;
}

void switchToggle(Ctx& c, int k, float right, float y) {
  const Look& L = c.L;
  const bool on = c.v(k) >= 0.5;
  bool hov = false;
  ImGui::PushID(k);
  if (hit(c, "##sw", right - 30, y, 30, 16, &hov)) setParam(c, k, on ? 0.0 : 1.0);
  ImGui::PopID();
  rect(c, right - 30, y, 30, 16, on ? L.accent : L.cellLine, 8);
  c.dl->AddCircleFilled(c.p(on ? right - 8 : right - 22, y + 8), c.s(5.5f), on ? L.white : L.text3, 16);
}

void rowLabel(Ctx& c, float y, const char* name, const char* sub) {
  text(c, fReg(), 12.5f, ImVec2(708, y), c.L.text, name);
  if (sub) text(c, fReg(), 10.5f, ImVec2(708, y + 16), c.L.text3, sub);
}

}  // namespace

void drawSettings(Ctx& c) {
  const Look& L = c.L;
  // dim everything else (a click there closes the sheet: drawEditor)
  c.dl->AddRectFilled(c.p(0, 46), c.p(690, kH), theme::withAlpha(theme::rgb(0x000000), 0.38f));
  rect(c, 690, 46, kW - 690, kH - 46, L.side, 0);
  c.dl->AddLine(c.p(690, 46), c.p(690, kH), L.line, c.s(1));
  text(c, fBold(), 16, ImVec2(708, 63), L.text, tr("Settings"));
  closeCross(c, "##setclose", 1058, 60, tr("Close settings"), [&] { c.u.settings = false; });
  const float R = 1082;
  auto head = [&](float y, const char* s) { text(c, fBold(), 10, ImVec2(708, y), L.text3, s); };
  int pick = -1;
  head(93, tr("SOUND"));
  {
    rowLabel(c, 109, tr("Quality"), tr("High adds 2x oversampling to Grit"));
    const char* l[3] = {tr("Light"), tr("Normal"), tr("High")};
    pick = -1;
    segLabels(c, "q", R, 112, l, 3, vi(c, PQuality), &pick);
    if (pick >= 0) setParam(c, PQuality, pick);
  }
  {
    char sub[80];
    std::snprintf(sub, sizeof sub, tr("Spectral stage, %d ms latency when on"), vi(c, PHaloEngine) == HaloFull ? 23 : 12);
    rowLabel(c, 141, tr("Halo engine"), sub);
    const char* l[3] = {tr("Off"), tr("Low"), tr("Full")};
    pick = -1;
    segLabels(c, "h", R, 144, l, 3, vi(c, PHaloEngine), &pick);
    if (pick >= 0) setParam(c, PHaloEngine, pick);
  }
  {
    rowLabel(c, 180, tr("Capture up to"), nullptr);
    const char* l[3] = {tr("2 bars"), tr("4 bars"), tr("8 bars")};
    pick = -1;
    segLabels(c, "cap", R, 176, l, 3, vi(c, PCaptureUpTo), &pick);
    if (pick >= 0) setParam(c, PCaptureUpTo, pick);
  }
  {
    rowLabel(c, 205, tr("Tempo"), tr("Free runs at its own BPM"));
    const char* l[2] = {tr("Song"), tr("Free")};
    pick = -1;
    const float sw = segLabels(c, "tm", R, 208, l, 2, vi(c, PTempoMode), &pick);
    if (pick >= 0) setParam(c, PTempoMode, pick);
    if (vi(c, PTempoMode) == 1) knob(c, PFreeBpm, R - sw - 22, 220, 10, L.accent, KnobArc);
  }
  head(245, tr("BREAKS"));
  {
    rowLabel(c, 262, tr("Hit gap"), tr("Shortest time between two hits"));
    knob(c, PHitGap, R - 12, 272, 11, L.b[2], KnobArc);
    textR(c, fMono(), 10.5f, R - 30, 266, L.text2, display(PHitGap, c.v(PHitGap)).c_str());
  }
  {
    rowLabel(c, 296, tr("Seed"), tr("Same seed, same breaks"));
    knob(c, PSeed, R - 12, 306, 11, L.b[2], KnobArc);
    textR(c, fMono(), 10.5f, R - 30, 300, L.text2, display(PSeed, c.v(PSeed)).c_str());
  }
  rowLabel(c, 332, tr("Restart breaks on play"), nullptr);
  switchToggle(c, PRestart, R, 333);
  rowLabel(c, 356, tr("Dice keeps the vibe"), tr("Rolls stay near the current sound"));
  switchToggle(c, PKeepVibe, R, 360);
  head(395, tr("LOOK"));
  Prefs& pr = prefs(c.a);
  {
    rowLabel(c, 411, tr("Visuals"), tr("Calm and Still for light-sensitive eyes"));
    const char* l[3] = {tr("Full"), tr("Calm"), tr("Still")};
    pick = -1;
    segLabels(c, "vis", R, 414, l, 3, pr.visuals, &pick);
    if (pick >= 0 && pick != pr.visuals) pr.visuals = pick, savePrefs(c.a);
  }
  {
    rowLabel(c, 450, tr("Knob drag"), nullptr);
    const char* l[2] = {tr("Circular"), tr("Up and down")};
    pick = -1;
    segLabels(c, "kd", R, 446, l, 2, pr.knobDrag, &pick);
    if (pick >= 0 && pick != pr.knobDrag) pr.knobDrag = pick, savePrefs(c.a);
  }
  c.dl->AddLine(c.p(708, 482), c.p(R, 482), L.line, c.s(1));
  {
    bool hov = false;
    const char* lab = tr("Reset all");
    const float bw = textSize(c, fReg(), 12, lab).x + 24;
    if (hit(c, "##resetall", 708, 494, bw, 28, &hov)) {
      const PluginInfo* in = findPlugin(kType);
      if (in) {
        PluginState s = presetState(*in, 0);
        s.original = c.st.original;
        setPluginState(c.a, c.t, s, tr("Reset all"));
        c.u.lastPreset = 0, c.u.presetName = in->presets[0].name;
      }
    }
    rect(c, 708, 494, bw, 28, hov ? theme::mix(L.panel2, L.text, 0.06f) : L.panel2, 4);
    rectLine(c, 708, 494, bw, 28, L.line, 4);
    textC(c, fReg(), 12, 708 + bw * 0.5f, 500, L.text, lab);
  }
  textR(c, fMono(), 10.5f, R, 502, L.text3, "Chater 1.0");
}

// ---------------------------------------------------------------- presets

namespace {

bool matches(const char* hay, const char* needle) {
  if (!needle[0]) return true;
  std::string h = hay, n = needle;
  for (auto& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return h.find(n) != std::string::npos;
}

void macroBars(Ctx& c, float x, float y, float w, const std::vector<float>& p, ImU32 col) {
  const float bw = (w - 7 * 4) / 8;
  for (int i = 0; i < 8; ++i) {
    const float v = i < static_cast<int>(p.size()) ? std::clamp(p[static_cast<size_t>(i)], 0.0f, 1.0f) : 0.0f;
    const float h = 3 + v * 10;
    rect(c, x + i * (bw + 4), y + 13 - h, bw, h, col, 1);
  }
}

}  // namespace

void drawPresets(Ctx& c, float y0) {
  const Look& L = c.L;
  const PluginInfo* in = findPlugin(kType);
  if (!in) return;
  Prefs& pr = prefs(c.a);
  rect(c, 14, y0, 1072, kBottom - y0, L.panel, 10);
  // search
  {
    const ImVec2 p0 = c.p(28, y0 + 14);
    rect(c, 28, y0 + 14, 294, 30, L.cell, 6);
    rectLine(c, 28, y0 + 14, 294, 30, L.line, 6);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + c.s(6), p0.y + c.s(4)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(L.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(L.text3));
    ImGui::SetNextItemWidth(c.s(282));
    char hint[64];
    std::snprintf(hint, sizeof hint, tr("Search %d sounds and tags"), static_cast<int>(in->presets.size()) - 1);
    ImGui::InputTextWithHint("##chsearch", hint, c.u.search, sizeof c.u.search);
    ImGui::PopStyleColor(3);
  }
  const std::string root = presetRoot(c.a.configDir());
  static std::vector<PresetEntry> users;
  static double usersAt = -100;
  if (c.now - usersAt > 2) users = listPresets(root, kType), usersAt = c.now;
  // tabs
  {
    char t1[32], t2[32];
    std::snprintf(t1, sizeof t1, "%s %d", tr("Yours"), static_cast<int>(users.size()));
    std::snprintf(t2, sizeof t2, "%s %d", tr("Favourites"), static_cast<int>(pr.favourites.size()));
    const char* tabs[3] = {tr("Factory"), t1, t2};
    float tx = 330;
    for (int i = 0; i < 3; ++i) {
      const float tw = textSize(c, fBold(), 12, tabs[i]).x + 20;
      char id[12];
      std::snprintf(id, sizeof id, "##ptab%d", i);
      if (button(c, id, tx, y0 + 14, tw, 30, tabs[i], c.u.presetTab == i)) c.u.presetTab = i;
      tx += tw + 8;
    }
  }
  // save and close
  {
    const float cw = textSize(c, fBold(), 12, tr("Close")).x + 26;
    if (button(c, "##pclose", 1072 - cw, y0 + 14, cw, 30, tr("Close"), false)) c.u.presets = false;
    const char* sl = c.u.saving ? tr("Save") : tr("Save preset");
    const float sw = textSize(c, fBold(), 12, sl).x + 24;
    const float sx = 1072 - cw - 8 - sw;
    if (c.u.saving) {
      rect(c, sx - 218, y0 + 14, 210, 30, L.cell, 6);
      rectLine(c, sx - 218, y0 + 14, 210, 30, L.accent, 6);
      ImGui::SetCursorScreenPos(c.p(sx - 212, y0 + 18));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
      ImGui::SetNextItemWidth(c.s(198));
      if (c.u.saveName[0] == 0 && ImGui::IsWindowAppearing()) std::snprintf(c.u.saveName, sizeof c.u.saveName, "%s", c.u.presetName.c_str());
      ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##chsave", c.u.saveName, sizeof c.u.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::PopStyleColor();
      if (enter || button(c, "##psave", sx, y0 + 14, sw, 30, sl, true)) {
        const std::string clean = safePresetName(c.u.saveName);
        if (!clean.empty() && savePreset(root, c.st, clean, "Chater")) {
          c.u.presetName = clean, c.u.lastPreset = -1, usersAt = -100;
          pui::presetLoaded(c.t, kType, clean);
        }
        c.u.saving = false;
      }
    } else if (button(c, "##psave", sx, y0 + 14, sw, 30, sl, false)) {
      c.u.saving = true;
      std::snprintf(c.u.saveName, sizeof c.u.saveName, "%s", c.u.presetName.c_str());
    }
  }
  static const int bankTile[kBanks] = {6, 1, 4, 0, 3, 7, 2, 5};
  const float colW = 124, gap = (1072 - 28 - colW * kBanks) / (kBanks - 1);
  if (c.u.presetTab == 1) {
    // the user's own presets, in columns
    float x = 28, y = y0 + 64;
    if (users.empty()) text(c, fReg(), 12, ImVec2(28, y0 + 70), L.text3, tr("Save a sound with Save preset and it shows here."));
    for (size_t i = 0; i < users.size(); ++i) {
      if (!matches(users[i].name.c_str(), c.u.search)) continue;
      if (y + 40 > kBottom - 10) y = y0 + 64, x += colW + gap;
      if (x + colW > 1086) break;
      char id[24];
      std::snprintf(id, sizeof id, "##up%d", static_cast<int>(i));
      bool hov = false;
      const bool cur = c.u.lastPreset < 0 && c.u.presetName == users[i].name;
      if (hit(c, id, x, y, colW, 34, &hov)) {
        PluginState s;
        if (loadPreset(users[i].file, kType, s)) {
          s.original = c.st.original;
          setPluginState(c.a, c.t, s, std::string(tr("Load preset")) + " " + users[i].name);
          c.u.presetName = users[i].name, c.u.lastPreset = -1;
          pui::presetLoaded(c.t, kType, users[i].name);
        }
      }
      rect(c, x, y, colW, 34, cur ? L.accent : (hov ? theme::mix(L.card, L.text, 0.05f) : L.card), 6);
      text(c, fBold(), 12.5f, ImVec2(x + 12, y + 9), cur ? L.ink : L.text, users[i].name.c_str());
      y += 40;
    }
    return;
  }
  for (int b = 0; b < kBanks; ++b) {
    const float x = 28 + b * (colW + gap);
    const ImU32 bc = L.tile[bankTile[b]];
    std::string bn = tr(bankName(b));
    for (auto& ch : bn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    text(c, fBold(), 12, ImVec2(x, y0 + 56), bc, bn.c_str());
    float y = y0 + 79;
    int count = 0;
    for (size_t i = 1; i < in->presets.size() && i < presetMeta().size(); ++i) {
      const PresetMeta& m = presetMeta()[i];
      if (m.bank != b) continue;
      if (c.u.presetTab == 2 && !pr.favourites.count(m.name)) continue;
      if (!matches(m.name, c.u.search) && !matches(m.tags, c.u.search) && !matches(m.description, c.u.search)) continue;
      ++count;
      const float h = 88;
      const bool cur = c.u.lastPreset == static_cast<int>(i);
      char id[24];
      std::snprintf(id, sizeof id, "##fp%d", static_cast<int>(i));
      bool hov = false;
      if (hit(c, id, x, y, colW - 22, h, &hov)) loadFactory(c, static_cast<int>(i));
      rect(c, x, y, colW, h, cur ? bc : (hov ? theme::mix(L.card, L.text, 0.05f) : L.card), 6);
      if (!cur) c.dl->AddRectFilled(c.p(x, y), c.p(x + 3, y + h), bc, c.s(6), ImDrawFlags_RoundCornersLeft);
      const ImU32 ink = cur ? L.ink : L.text, ink2 = cur ? theme::withAlpha(L.ink, 0.75f) : L.text3;
      text(c, fBold(), 12.5f, ImVec2(x + 14, y + 10), ink, m.name);
      c.dl->AddText(fReg(), c.s(10.5f * kTextScale), c.p(x + 14, y + 30), ink2, tr(m.description), nullptr, c.s(colW - 26));
      macroBars(c, x + 14, y + h - 21, colW - 28, presetState(*in, i).params, cur ? theme::withAlpha(L.ink, 0.7f) : theme::withAlpha(bc, 0.85f));
      // favourite star
      {
        const bool fav = pr.favourites.count(m.name) > 0;
        bool sh = false;
        std::snprintf(id, sizeof id, "##fav%d", static_cast<int>(i));
        if (hit(c, id, x + colW - 22, y + 6, 16, 16, &sh)) {
          if (fav) pr.favourites.erase(m.name);
          else pr.favourites.insert(m.name);
          savePrefs(c.a);
        }
        if (fav || hov || sh) sparkle(c, c.p(x + colW - 14, y + 14), c.s(5.5f), fav ? (cur ? L.ink : L.tile[4]) : theme::withAlpha(ink, 0.35f));
      }
      y += h + 6;
    }
    char n[8];
    std::snprintf(n, sizeof n, "%d", count);
    textR(c, fMono(), 10, x + colW, y0 + 57, L.text3, n);
  }
}

}  // namespace gl::packs::chater::ui
