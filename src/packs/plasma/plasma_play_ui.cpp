// Gridline — "plasma" pack: the Play tab. How the notes play, in five cards: Voices (one note or
// many, how many sound now), Glide, Wheels (bend and mod, with their ranges), the Arpeggiator (its
// pattern live, the common orders and its main dials), Phrases (one key plays a stored pattern)
// and Key zones (which layer plays which keys or velocities). Each card's "..." opens the full
// page with every setting.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

// A card: the block frame, its light (or none), the name and what it does; returns the body top.
float playCard(Ctx& c, float x, float y, float w, float h, ImU32 fam, int enable, const char* name, const char* sub) {
  blockFrame(c, x, y, w, h, fam, false, false);
  float tx = x + M(16);
  const float hy = y + M(24);
  if (enable >= 0) led(c, enable, tx, hy - M(3.5f), M(7)), tx += M(16);
  text(c, tx, hy - T(13.5f) * 0.68f, name, c.P.text, T(13.5f), true);
  text(c, tx + textW(c, name, T(13.5f), true) + M(9), hy - T(11.5f) * 0.68f, sub, c.P.textFaint, T(11.5f), false);
  return y + M(44);
}
// The "..." at a card's right: opens the full page.
void moreButton(Ctx& c, const char* id, float rx, float y, int view, const char* hint) {
  const float w = M(24), h = M(20);
  if (btnLook(c, id, rx - w, y + M(14), w, h, "...", true, false, T(11.5f))) openView(c.u, view);
  if (ImGui::IsItemHovered()) c.a.setHint(hint);
}
// A data knob (a float of the model), one undo step a drag.
bool modelKnob(Ctx& c, const char* id, float& v, double lo, double hi, double def, float cx, float cy, const char* label, const std::string& shown,
               int what) {
  ParamDesc d;
  d.name = label, d.min = lo, d.max = hi, d.def = def;
  float n = static_cast<float>(d.toNormalized(v));
  {
    MonoText mono;
    textMid(c, cx - M(34), cx + M(34), cy + M(18) + M(23), shown.c_str(), c.P.textDim, T(10), false);
  }
  if (knobFree(c, id, n, cx, cy, M(18), label, false, shown.c_str(), d)) {
    v = static_cast<float>(d.fromNormalized(std::clamp(n, 0.f, 1.f)));
    commit(c, label, what);
    return true;
  }
  return false;
}
std::string noteName(int k) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(n[((k % 12) + 12) % 12]) + std::to_string(k / 12 - 2);
}

// ---------------------------------------------------------------- Voices
void voicesCard(Ctx& c, float x, float y, float w, float h) {
  float by = playCard(c, x, y, w, h, c.P.textDim, -1, "Voices", "how notes share the sound");
  moreButton(c, "vmore", x + w - M(14), y, VKeys, "Key and touch: steals, one voice per key, velocity, the keys' own settings");
  const bool mono = val(c, G_Mono) >= 0.5, legato = val(c, G_Legato) >= 0.5;
  const int cur = !mono ? 0 : legato ? 2 : 1;
  reach(G_Mono), reach(G_Legato);
  const int hit = segPick(c, "pml", x + M(16), by, M(168), M(28), {"Poly", "Mono", "Legato"}, cur, false, T(12));
  if (hit == 0) setv(c, G_Mono, 0), setv(c, G_Legato, 0);
  if (hit == 1) setv(c, G_Mono, 1), setv(c, G_Legato, 0);
  if (hit == 2) setv(c, G_Mono, 1), setv(c, G_Legato, 1);
  if (ImGui::IsItemHovered()) c.a.setHint("Poly: every key its own voice. Mono: one note at a time. Legato: one note, and joined notes glide without starting over");
  by += M(28) + M(10);
  // the voices: one bar each, lit while it sounds
  const float kx = x + w - M(48), ww = kx - M(38) - (x + M(16)), wh = y + h - M(14) - by;
  wellLook(c, x + M(16), by, ww, wh);
  const int n = mono ? 1 : std::clamp(vali(c, G_Poly), 1, 32), playing = c.hasLive ? std::min(c.live.voices, n) : 0;
  {
    char b[32];
    std::snprintf(b, sizeof b, "%d of %d playing", playing, n);
    MonoText mono2;
    capsText(c, x + M(24), by + M(8), b, c.P.textFaint, T(9.5f), M(0.6f));
  }
  const int shown = std::min(n, 16);
  const float bx0 = x + M(24), bw = (ww - M(16)) / static_cast<float>(shown), top = by + M(24), bot = by + wh - M(14);
  for (int i = 0; i < shown; ++i) {
    const float bx = bx0 + bw * i;
    c.dl->AddRectFilled(c.p(bx + M(1.5f), top), c.p(bx + bw - M(1.5f), bot), theme::withAlpha(c.P.line, 0.5f), c.s(M(2)));
    if (i < playing) {
      const float lv = c.live.env[0] > 0 ? std::clamp(c.live.env[0], 0.f, 1.f) : 0.6f;
      c.dl->AddRectFilled(c.p(bx + M(1.5f), bot - (bot - top) * lv), c.p(bx + bw - M(1.5f), bot), theme::withAlpha(c.P.textDim, 0.85f), c.s(M(2)));
    }
    if (shown <= 16) {
      MonoText mono2;
      char b[16];
      std::snprintf(b, sizeof b, "%d", i + 1);
      textMid(c, bx, bx + bw, bot + M(7), b, c.P.textFaint, T(9), false);
    }
  }
  KnobColour kc(c.P.textDim);
  knob(c, G_Poly, kx, y + M(108), M(18), "Voices", KnobValue);
  knob(c, G_RndDetune, kx, y + M(186), M(18), "Variation", KnobValue);
}

// ---------------------------------------------------------------- Glide
void glideCard(Ctx& c, float x, float y, float w, float h) {
  const ImU32 fam = c.P.fam[FamKey];
  float by = playCard(c, x, y, w, h, fam, -1, "Glide", "slide from note to note");
  const float t = static_cast<float>(valf(c, G_PortaTime));
  const bool always = val(c, G_PortaAlways) >= 0.5;
  const int cur = t <= 0.0001f ? 2 : always ? 0 : 1;
  reach(G_PortaAlways);
  const int hit = segPick(c, "gl", x + M(16), by, M(212), M(28), {"Always", "Only when held", "Off"}, cur, false, T(12));
  if (hit >= 0 && hit != cur) {
    if (hit == 2) setv(c, G_PortaTime, 0);
    else {
      if (t <= 0.0001f) setv(c, G_PortaTime, 0.2);
      setv(c, G_PortaAlways, hit == 0 ? 1 : 0);
    }
  }
  if (ImGui::IsItemHovered()) c.a.setHint("Always: every note slides from the last one. Only when held: only notes played while another is held");
  by += M(28) + M(10);
  const float kx = x + w - M(48), gx = x + M(16), gw = kx - M(38) - gx, gh = y + h - M(14) - by;
  wellLook(c, gx, by, gw, gh);
  gridLook(c, gx, by, gw, gh, 6, 3, false);
  {  // the glide from C2 up to G2: flat, the slide (its time and curve), flat
    const float lo = by + gh * 0.72f, hi = by + gh * 0.24f, span = std::max(0.02f, t) * 0.55f, x0 = gx + gw * 0.3f, x1 = x0 + gw * span;
    const float cv = static_cast<float>(valf(c, G_PortaCurve));
    std::vector<ImVec2> pts;
    pts.push_back(c.p(gx, lo));
    for (int i = 0; i <= 32; ++i) {
      const float u = i / 32.f;
      const float e = std::pow(u, std::exp(-cv * 1.6f));
      const float s = e * e * (3 - 2 * e);
      pts.push_back(c.p(x0 + (x1 - x0) * u, lo + (hi - lo) * (t <= 0.0001f ? (u > 0 ? 1.f : 0.f) : s)));
    }
    pts.push_back(c.p(gx + gw, hi));
    glowLine(c, pts, t <= 0.0001f ? c.P.textFaint : theme::mix(fam, c.P.text, 0.4f), M(1.6f));
    glowDot(c, pts[pts.size() - 2], M(2.6f), theme::mix(fam, c.P.text, 0.4f));
    MonoText mono;
    text(c, gx + M(6), hi - M(10), "G2", c.P.textFaint, T(9.5f), false);
    text(c, gx + M(6), lo - M(10), "C2", c.P.textFaint, T(9.5f), false);
  }
  KnobColour kc(fam);
  knob(c, G_PortaTime, kx, y + M(108), M(18), "Time", KnobValue);
  knob(c, G_PortaCurve, kx, y + M(186), M(18), "Curve", KnobValue | KnobBipolar);
}

// ---------------------------------------------------------------- Wheels
void wheel(Ctx& c, int k, bool bend, float x, float y, float w, float h) {
  reach(k);
  const ParamDesc* d = pdesc(k);
  const float v = static_cast<float>(valf(c, k)), lo = d ? static_cast<float>(d->min) : 0.f, hi = d ? static_cast<float>(d->max) : 1.f;
  const float u = (v - lo) / std::max(1e-6f, hi - lo);
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), theme::mix(c.P.well, c.P.frame, 0.4f), c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(4)));
  for (int i = 1; i < 14; ++i) hline(c, x + w * 0.3f, x + w * 0.7f, y + h * i / 14.f, theme::withAlpha(c.P.text, 0.12f));
  const float my = y + h - M(8) - (h - M(16)) * u;
  c.dl->AddRectFilled(c.p(x + w * 0.2f, my - M(3)), c.p(x + w * 0.8f, my + M(3)), bend ? theme::mix(c.P.text, c.P.knob, 0.4f) : c.P.good, c.s(M(3)));
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##wheel", c.F.sz(w, h));
  if (ImGui::IsItemActive()) {
    const float f = std::clamp((c.p(0, y + h - M(8)).y - ImGui::GetIO().MousePos.y) / c.s(h - M(16)), 0.f, 1.f);
    setv(c, k, lo + (hi - lo) * f, true);
  }
  if (ImGui::IsItemDeactivated()) {
    if (bend) setv(c, k, 0, true);  // the bend wheel springs back
    c.a.doc.endGesture();
  }
  if (ImGui::IsItemHovered()) c.a.setHint(bend ? "Bend: drag up or down, it springs back" : "Mod wheel: drag up or down, it stays");
  ImGui::PopID();
}
void wheelsCard(Ctx& c, float x, float y, float w, float h) {
  const ImU32 fam = c.P.fam[FamKey];
  const float by = playCard(c, x, y, w, h, fam, -1, "Wheels", "bend and mod, bend springs back");
  const float ww = M(150), wh = y + h - M(14) - by;
  wellLook(c, x + M(16), by, ww, wh);
  wheel(c, G_PitchWheel, true, x + M(16) + M(28), by + M(14), M(30), wh - M(40));
  wheel(c, G_ModWheel, false, x + M(16) + ww - M(58), by + M(14), M(30), wh - M(40));
  {
    MonoText mono;
    capsText(c, x + M(16) + M(30), by + wh - M(18), "Bend", c.P.textFaint, T(9.5f), M(0.6f));
    capsText(c, x + M(16) + ww - M(54), by + wh - M(18), "Mod", c.P.textFaint, T(9.5f), M(0.6f));
  }
  const float kx0 = x + M(16) + ww + M(46), kx1 = x + w - M(46);
  KnobColour kc(fam);
  knob(c, G_BendUp, kx0, y + M(84), M(18), "Bend up", KnobValue | KnobBipolar);
  knob(c, G_BendDown, kx1, y + M(84), M(18), "Bend down", KnobValue | KnobBipolar);
  knob(c, G_Transpose, kx0, y + M(162), M(18), "Transpose", KnobValue | KnobBipolar);
  knob(c, G_MasterTune, kx1, y + M(162), M(18), "Fine", KnobValue | KnobBipolar);
}

// ---------------------------------------------------------------- Arpeggiator
void arpCard(Ctx& c, float x, float y, float w, float h) {
  const ImU32 fam = c.P.accent;
  ArpSlot& a = c.m.arp[static_cast<size_t>(std::clamp(c.m.arpActive, 0, kArpSlots - 1))];
  const float by = playCard(c, x, y, w, h, fam, G_ArpOn, "Arpeggiator", "plays held notes one at a time");
  {  // On / Off, the full page
    const bool on = val(c, G_ArpOn) >= 0.5;
    const int hit = segPick(c, "arpon", x + w - M(24) - M(14) - M(80), y + M(14), M(70), M(20), {"On", "Off"}, on ? 0 : 1, true, T(10.5f));
    if (hit >= 0 && (hit == 0) != on) setv(c, G_ArpOn, hit == 0 ? 1 : 0);
    moreButton(c, "arpmore", x + w - M(14), y, VArp, "Every arpeggiator setting: twelve patterns, climb, velocity, chance, start over");
  }
  // the pattern: a 16 step preview of the order over four held notes (a drawn pattern as drawn)
  const float gx = x + M(16), gw = w * 0.58f, gh = y + h - M(14) - by;
  wellLook(c, gx, by, gw, gh);
  constexpr int cols = 16, rows = 12;
  const float cw = gw / cols, rh = (gh - M(10)) / rows;
  for (int i = 1; i < cols; ++i) vline(c, gx + cw * i, by, by + gh - M(10), theme::withAlpha(c.P.line, 0.6f));
  std::vector<std::pair<int, int>> cells;  // column, row (0 = lowest)
  if (a.shape == ArpPattern) {
    const float beats = std::max(0.25f, a.patternBeats);
    for (const auto& st : a.steps) {
      const int col = static_cast<int>(st.time / beats * cols);
      if (col < cols) cells.push_back({col, std::clamp(st.row, 0, rows - 1)});
    }
  } else {
    std::vector<int> order, played = {0, 2, 1, 3};
    uint32_t rng = 12345;
    const int oct = std::clamp(a.range, 0, 2) + 1;
    while (static_cast<int>(cells.size()) < cols) {
      arpOrder(a.shape, 4, played, rng, order);
      if (order.empty()) break;
      for (int o = 0; o < oct && static_cast<int>(cells.size()) < cols; ++o)
        for (int n : order) {
          if (static_cast<int>(cells.size()) >= cols) break;
          cells.push_back({static_cast<int>(cells.size()), std::clamp(n + o * 4, 0, rows - 1)});
        }
    }
  }
  const int now = c.hasLive && c.live.arpStep >= 0 && val(c, G_ArpOn) >= 0.5 ? c.live.arpStep % cols : -1;
  if (now >= 0) c.dl->AddRectFilled(c.p(gx + cw * now, by), c.p(gx + cw * (now + 1), by + gh - M(10)), theme::withAlpha(fam, 0.16f));
  for (const auto& [col, row] : cells) {
    const float cx = gx + cw * (col + 0.5f), cy = by + gh - M(10) - rh * (row + 0.5f), s = std::min(cw, rh) * 0.6f;
    c.dl->AddRectFilled(c.p(cx - s * 0.5f, cy - s * 0.5f), c.p(cx + s * 0.5f, cy + s * 0.5f), theme::withAlpha(fam, col == now ? 1.f : 0.65f), c.s(M(2)));
  }
  for (int i = 0; i < cols; ++i)
    c.dl->AddRectFilled(c.p(gx + cw * i + M(2), by + gh - M(6)), c.p(gx + cw * (i + 1) - M(2), by + gh - M(4)), i == now ? fam : theme::withAlpha(c.P.line, 0.8f));
  ImGui::SetCursorScreenPos(c.p(gx, by));
  if (ImGui::InvisibleButton("##arpgrid", c.F.sz(gw, gh))) openView(c.u, VArp);
  if (ImGui::IsItemHovered()) c.a.setHint("The order the held notes play in (click: edit the pattern)");
  // the common orders, then the main dials
  const float rx = gx + gw + M(12), rw = x + w - M(16) - rx;
  struct O {
    int shape;
    const char* name;
  };
  const O os[5] = {{ArpUp, "Up"}, {ArpDown, "Down"}, {ArpUpDown, "Both ways"}, {ArpPlayed, "As played"}, {ArpRand, "Random"}};
  const float ow = (rw - M(6) * 4) / 5, oh = M(44);
  for (int i = 0; i < 5; ++i) {
    const float ox = rx + (ow + M(6)) * i;
    const bool on = a.shape == os[i].shape;
    ImGui::PushID(i + 5300);
    if (btnLook(c, "ord", ox, by, ow, oh, "", false, on, T(10.5f))) {
      a.shape = os[i].shape;
      commit(c, "Arp order");
    }
    // a little picture of the order: dots climbing, falling, both, as played, scattered
    static const float ys[5][6] = {{0.2f, 0.4f, 0.6f, 0.8f, 0.2f, 0.4f}, {0.8f, 0.6f, 0.4f, 0.2f, 0.8f, 0.6f}, {0.2f, 0.5f, 0.8f, 0.5f, 0.2f, 0.5f},
                                   {0.3f, 0.7f, 0.5f, 0.2f, 0.3f, 0.7f}, {0.6f, 0.2f, 0.8f, 0.4f, 0.7f, 0.3f}};
    for (int k = 0; k < 6; ++k)
      c.dl->AddRectFilled(c.p(ox + ow * 0.2f + k * ow * 0.12f, by + M(6) + M(12) * (1 - ys[i][k])), c.p(ox + ow * 0.2f + k * ow * 0.12f + M(4), by + M(8) + M(12) * (1 - ys[i][k])),
                          on ? fam : c.P.label);
    textMid(c, ox, ox + ow, by + oh - M(12), os[i].name, on ? c.P.text : c.P.label, T(10.5f), false);
    ImGui::PopID();
  }
  const float ky = by + oh + M(40), kw = rw / 5;
  KnobColour kc(fam);
  {
    const auto& dn = divisionNames();
    const int nd = static_cast<int>(dn.size()) - 1;
    float rn = static_cast<float>(nd - a.rate);
    if (a.bpm) {
      if (modelKnob(c, "rate", rn, 0, nd, nd - ArpSlot{}.rate, rx + kw * 0.5f, ky, "Rate", dn[static_cast<size_t>(std::clamp(a.rate, 0, nd))], 8300))
        a.rate = std::clamp(nd - static_cast<int>(std::lround(rn)), 0, nd);
    } else {
      char b[24];
      std::snprintf(b, sizeof b, "%.2f Hz", static_cast<double>(a.hz));
      modelKnob(c, "hz", a.hz, 0.05, 20.0, ArpSlot{}.hz, rx + kw * 0.5f, ky, "Rate", b, 8301);
    }
  }
  {
    float r = static_cast<float>(a.range + 1);
    if (modelKnob(c, "oct", r, 1, 9, 1, rx + kw * 1.5f, ky, "Octaves", std::to_string(a.range + 1), 8302)) a.range = std::clamp(static_cast<int>(std::lround(r)) - 1, 0, 8);
  }
  {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", a.gate * 100.0);
    modelKnob(c, "len", a.gate, 0.05, 2.0, ArpSlot{}.gate, rx + kw * 2.5f, ky, "Length", b, 8303);
  }
  knob(c, G_Swing, rx + kw * 3.5f, ky, M(18), "Swing", KnobValue);
  {  // hold: a switch shown as a dial cell
    const float hx = rx + kw * 4.5f;
    if (btnLook(c, "hold", hx - M(26), ky - M(14), M(52), M(28), a.latch ? "On" : "Off", false, a.latch, T(11))) {
      a.latch = !a.latch;
      commit(c, "Arp hold");
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Hold: the notes keep playing after the keys are let go");
    textMid(c, hx - M(30), hx + M(30), ky + M(28), "Hold", c.P.label, T(11), false);
  }
}

// ---------------------------------------------------------------- Phrases
void phrasesCard(Ctx& c, float x, float y, float w, float h) {
  const ImU32 fam = c.P.accent;
  const float by = playCard(c, x, y, w, h, fam, G_ClipOn, "Phrases", "one key plays a whole pattern");
  moreButton(c, "phmore", x + w - M(14), y, VPhrases, "Every phrase: the note grid, lanes, how each plays");
  std::vector<int> list;
  for (int i = 0; i < kClipSlots; ++i)
    if (!c.m.clip[static_cast<size_t>(i)].notes.empty() || i == c.m.clipActive) list.push_back(i);
  const float rh = M(40), gap = M(6), bh = M(30), lx = x + M(16), lw = w - M(32);
  const int fit = std::max(1, static_cast<int>((y + h - M(14) - bh - M(14) - by + gap) / (rh + gap)));
  for (int j = 0; j < std::min(fit, static_cast<int>(list.size())); ++j) {
    const int i = list[static_cast<size_t>(j)];
    const ClipSlot& cs = c.m.clip[static_cast<size_t>(i)];
    const float ry = by + j * (rh + gap);
    const bool act = c.m.clipActive == i;
    ImGui::PushID(i + 5400);
    ImGui::SetCursorScreenPos(c.p(lx, ry));
    if (ImGui::InvisibleButton("##ph", c.F.sz(lw, rh))) {
      c.m.clipActive = i;
      commit(c, "Choose phrase");
    }
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.a.setHint("Play this phrase (the trigger octave on the keyboard also picks them)");
    c.dl->AddRectFilled(c.p(lx, ry), c.p(lx + lw, ry + rh), act ? theme::mix(c.P.panel, fam, 0.18f) : hv ? c.P.panelHi : theme::mix(c.P.panel, c.P.frame, 0.5f), c.s(M(4)));
    c.dl->AddRect(c.p(lx, ry), c.p(lx + lw, ry + rh), act ? theme::withAlpha(fam, 0.7f) : c.P.line, c.s(M(4)));
    // its notes, small
    const float px = lx + M(10), pw = M(92), py = ry + M(8), ph = rh - M(16);
    c.dl->AddRectFilled(c.p(px, py), c.p(px + pw, py + ph), c.P.well, c.s(M(2)));
    if (!cs.notes.empty()) {
      int lo = 127, hi = 0;
      for (const auto& n : cs.notes) lo = std::min(lo, n.key), hi = std::max(hi, n.key);
      const float span = std::max(1, hi - lo);
      for (const auto& n : cs.notes) {
        const float nx = px + pw * n.start / std::max(0.25f, cs.lengthBeats), nw = std::max(M(2), pw * n.len / std::max(0.25f, cs.lengthBeats));
        const float ny = py + ph - M(3) - (ph - M(6)) * (n.key - lo) / span;
        c.dl->AddRectFilled(c.p(nx, ny - M(1)), c.p(std::min(px + pw, nx + nw), ny + M(1)), act ? fam : c.P.label);
      }
    }
    std::string name = cs.name.empty() ? (cs.notes.empty() ? std::string("Empty") : "Phrase " + std::to_string(i + 1)) : cs.name;
    text(c, px + pw + M(12), ry + rh * 0.5f - T(12) * 0.68f, name.c_str(), c.P.text, T(12), false);
    char len[24];
    const float bars = cs.lengthBeats / 4.f;
    std::snprintf(len, sizeof len, bars == 1.f ? "%g bar" : "%g bars", static_cast<double>(bars));
    MonoText mono;
    textR(c, lx + lw - M(10), ry + rh * 0.5f - T(11) * 0.68f, len, c.P.textFaint, T(11), false);
    ImGui::PopID();
  }
  if (list.empty()) text(c, lx, by + M(8), "No phrases yet: record one or draw it", c.P.textFaint, T(11.5f), false);
  const float bw = (lw - M(8)) * 0.5f, yb = y + h - M(14) - bh;
  if (btnLook(c, "rec", lx, yb, bw, bh, "Record a phrase", false, false, T(12))) openView(c.u, VPhrases);
  if (ImGui::IsItemHovered()) c.a.setHint("Open the phrase editor to record or draw notes");
  if (btnLook(c, "edit", lx + bw + M(8), yb, bw, bh, "Edit phrases", false, false, T(12))) openView(c.u, VPhrases);
  if (ImGui::IsItemHovered()) c.a.setHint("Open the phrase editor");
}

// ---------------------------------------------------------------- Key zones
void zoneRow(Ctx& c, int pMin, int pMax, bool vel, bool on, float x0, float x1, float y0, float h, ImU32 col, const char* who) {
  reach(pMin), reach(pMax);
  const int lo = vel ? 1 : 0, hi = 127;
  const float bw = x1 - x0;
  auto xOf = [&](float v) { return x0 + bw * (v - lo) / static_cast<float>(hi - lo + 1); };
  const float vmin = std::round(static_cast<float>(valf(c, pMin))), vmax = std::round(static_cast<float>(valf(c, pMax)));
  c.dl->AddRectFilled(c.p(x0, y0), c.p(x1, y0 + h), theme::withAlpha(c.P.line, 0.45f), c.s(M(3)));
  const float ax = xOf(vmin), axe = xOf(vmax + 1);
  c.dl->AddRectFilled(c.p(ax, y0), c.p(axe, y0 + h), theme::withAlpha(col, on ? 0.72f : 0.25f), c.s(M(3)));
  c.dl->AddRect(c.p(ax, y0), c.p(axe, y0 + h), theme::withAlpha(col, on ? 1.f : 0.4f), c.s(M(3)));
  {
    MonoText mono;
    const std::string a = vel ? std::to_string(static_cast<int>(vmin)) : noteName(static_cast<int>(vmin));
    const std::string b = vel ? std::to_string(static_cast<int>(vmax)) : noteName(static_cast<int>(vmax));
    const ImU32 tc = c.P.dark ? IM_COL32(20, 22, 24, 230) : IM_COL32(20, 22, 24, 220);
    text(c, ax + M(6), y0 + h * 0.5f - T(10) * 0.68f, a.c_str(), tc, T(10), true);
    if (axe - ax > M(60)) textR(c, axe - M(6), y0 + h * 0.5f - T(10) * 0.68f, b.c_str(), tc, T(10), true);
  }
  ImGui::PushID(pMin);
  ImGui::SetCursorScreenPos(c.p(x0, y0));
  ImGui::InvisibleButton("##zone", c.F.sz(bw, h));
  static int mode = 0;
  static float grab = 0;
  const float mx = ImGui::GetIO().MousePos.x;
  const float v = std::clamp(lo + (mx - c.p(x0, 0).x) / c.s(bw) * static_cast<float>(hi - lo + 1), static_cast<float>(lo), static_cast<float>(hi) + 0.999f);
  const bool nearMin = std::fabs(mx - c.p(ax, 0).x) < c.s(M(6)), nearMax = std::fabs(mx - c.p(axe, 0).x) < c.s(M(6));
  if (ImGui::IsItemHovered() && (nearMin || nearMax)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActivated()) {
    mode = nearMin ? 1 : nearMax ? 2 : (v >= vmin && v <= vmax + 1) ? 3 : 0;
    grab = v - vmin;
    if (mode == 0) mode = std::fabs(v - vmin) < std::fabs(v - vmax) ? 1 : 2;
  }
  if (ImGui::IsItemActive()) {
    const float f = std::floor(v);
    if (mode == 1) setv(c, pMin, std::min(f, vmax), true);
    if (mode == 2) setv(c, pMax, std::max(f, vmin), true);
    if (mode == 3) {
      const float span = vmax - vmin, nmin = std::clamp(std::floor(v - grab), static_cast<float>(lo), static_cast<float>(hi) - span);
      setv(c, pMin, nmin, true), setv(c, pMax, nmin + span, true);
    }
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) setv(c, pMin, lo), setv(c, pMax, hi);
  if (ImGui::IsItemHovered()) c.a.setHint(std::string(vel ? "Velocities " : "Keys ") + who + " plays: drag an end or the middle \xC2\xB7 double-click: all");
  ImGui::PopID();
}
void zonesCard(Ctx& c, float x, float y, float w, float h) {
  const float by = playCard(c, x, y, w, h, c.P.accent, -1, "Key zones", "which layer plays on which keys and how hard");
  static bool vel = false;
  const int hit = segPick(c, "kv", x + w - M(24) - M(14) - M(124), y + M(14), M(112), M(20), {"Keys", "Velocity"}, vel ? 1 : 0, true, T(10.5f));
  if (hit >= 0) vel = hit == 1;
  moreButton(c, "zmore", x + w - M(14), y, VZones, "Every zone setting: fold, keep feeding the warps");
  const float lx = x + M(16), bx0 = x + M(146), bx1 = x + w - M(16);
  const char* names[4] = {"Layer 1", "Layer 2", "Layer 3", "Texture"};
  const int rowsN = vel ? 3 : 4;
  float ry = by + M(4);
  for (int i = 0; i < rowsN; ++i) {
    const bool noise = i == 3;
    const ImU32 col = partCol(c, noise ? 4 : i);
    const bool on = val(c, noise ? noiseP(N_Enable) : oscP(i, O_Enable)) >= 0.5;
    c.dl->AddRectFilled(c.p(lx, ry + M(6)), c.p(lx + M(8), ry + M(14)), col, c.s(M(2)));
    text(c, lx + M(16), ry + M(10) - T(12) * 0.68f, names[i], on ? c.P.textDim : c.P.textFaint, T(12), false);
    const int pMin = noise ? tailP(T_NKeyMin) : oscP(i, vel ? O_VelMin : O_KeyMin), pMax = noise ? tailP(T_NKeyMax) : oscP(i, vel ? O_VelMax : O_KeyMax);
    zoneRow(c, pMin, pMax, vel, on, bx0, bx1, ry, M(20), col, names[i]);
    ry += M(30);
  }
  // the keyboard under the key zones (C0 .. C8 across the same width as the bars)
  if (!vel) {
    const float ky = ry + M(2), kh = y + h - M(14) - ky, lo = 0, hi = 128;
    const float kw = (bx1 - bx0) / (hi - lo);
    const ImU32 white = c.P.dark ? IM_COL32(214, 220, 224, 255) : IM_COL32(236, 240, 242, 255), black = IM_COL32(28, 31, 34, 255);
    c.dl->AddRectFilled(c.p(bx0, ky), c.p(bx1, ky + kh), white, c.s(M(2)));
    // white key edges, then the black keys
    for (int k = 0; k < 128; ++k) {
      const int pc = k % 12;
      if (pc == 0 || pc == 5) vline(c, bx0 + kw * k, ky, ky + kh, IM_COL32(150, 156, 160, 255));
    }
    for (int k = 0; k < 128; ++k) {
      const int pc = k % 12;
      const bool blk = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
      if (blk) c.dl->AddRectFilled(c.p(bx0 + kw * k, ky), c.p(bx0 + kw * (k + 1), ky + kh * 0.6f), black, c.s(M(1)));
      else if (pc == 2 || pc == 4 || pc == 7 || pc == 9 || pc == 11) vline(c, bx0 + kw * k, ky + kh * 0.6f, ky + kh, IM_COL32(170, 176, 180, 255));
    }
    MonoText mono;
    for (int o = 0; o <= 9; ++o) {
      const int k = o * 12 + 24;
      if (k < 128) text(c, bx0 + kw * k + M(3), ky + kh - M(12), noteName(k).c_str(), IM_COL32(110, 116, 120, 255), T(9), false);
    }
  }
}

}  // namespace

void playPage(Ctx& c) {
  const float x0 = M(20), x1 = kW - M(20), gap = M(12), y0 = kTopH + M(16), yB = kPlayY - M(16) - drawerH(c.u);
  const float w3 = (x1 - x0 - gap * 2) / 3.f;
  const float h1 = M(240), h2 = M(252), h3 = std::max(M(160), yB - y0 - h1 - h2 - gap * 2);
  voicesCard(c, x0, y0, w3, h1);
  glideCard(c, x0 + w3 + gap, y0, w3, h1);
  wheelsCard(c, x0 + (w3 + gap) * 2, y0, w3, h1);
  const float y2 = y0 + h1 + gap;
  arpCard(c, x0, y2, w3 * 2 + gap, h2);
  phrasesCard(c, x0 + (w3 + gap) * 2, y2, w3, h2);
  zonesCard(c, x0, y2 + h2 + gap, x1 - x0, h3);
}

}  // namespace gl::packs::plasma::ui
