// Gridline - J-129 dev gallery (`--gallery <look>[:page]`): every element of a
// look in every state, drawn over the main viewport for offscreen captures.
// Page 1: controls. Page 2: displays and animation filmstrips (six frames of
// each live element, so a still capture shows the motion).
#include <cmath>
#include <cstdio>

#include "LookPaint.h"
#include "Widgets.h"

namespace gl::look {
namespace {

constexpr int kStates = 7;
const char* const kStateNames[kStates] = {"Idle", "Hover", "Dragging", "Disabled", "Modulated", "Bipolar", "Live"};

const char* blurb(Dir d) {
  switch (d) {
    case Dir::Graphite: return "Machined graphite, recessed Paper light. The app icon's material.";
    case Dir::Hairline: return "Flat precision: hairlines, type and one accent. No material.";
    case Dir::Lumen: return "Dark glass; every value is emitted light with a soft bloom.";
    case Dir::Slate: return "Today's studio slate roles, rebuilt with one consistent finish.";
    default: return "The interface as drawn today (shared skin functions).";
  }
}

struct Ctx {
  Painter& P;
  ImDrawList* dl;
  double t;
};

void rowLabel(Ctx& c, float x, float y, float w, const char* s) { c.P.label(c.dl, ImVec2(x, y), ImVec2(x + w, y + 16), s, true); }

Knob knobState(int s, float t, int size) {
  Knob k;
  k.size = size;
  k.norm = 0.62f;
  switch (s) {
    case 1: k.state = Hover; break;
    case 2: k.state = Active, k.norm = 0.78f; break;
    case 3: k.state = Disabled, k.norm = 0.4f; break;
    case 4: k.norm = 0.45f, k.modDepth = 0.3f, k.modNow = 0.45f + 0.3f * (0.5f + 0.5f * std::sin(t * 2.2f)); break;
    case 5: k.bipolar = true, k.norm = 0.3f; break;
    case 6: k.norm = 0.55f, k.level = 0.5f + 0.5f * std::fabs(std::sin(t * 3.1f)), k.modDepth = -0.2f, k.modNow = 0.55f - 0.2f * (0.5f + 0.5f * std::sin(t * 1.3f)); break;
    default: break;
  }
  return k;
}

// ------------------------------------------------------------------ page 1
void page1(Ctx& c, ImVec2 o, float W) {
  Painter& P = c.P;
  ImDrawList* dl = c.dl;
  const float colW = 88, x0 = o.x + 140;
  // ---- knobs
  P.section(dl, ImVec2(o.x, o.y), ImVec2(o.x + 780, o.y + 300), "Knobs");
  for (int s = 0; s < kStates; ++s) P.label(dl, ImVec2(x0 + colW * static_cast<float>(s), o.y + 22), ImVec2(x0 + colW * static_cast<float>(s + 1), o.y + 38), kStateNames[s], true);
  const float rads[3] = {26, 18, 11};
  const char* const sizes[3] = {"Large 52", "Medium 36", "Small 22"};
  float y = o.y + 50;
  for (int z = 0; z < 3; ++z) {
    const float r = rads[z];
    rowLabel(c, o.x + 10, y + r - 8, 130, sizes[z]);
    for (int s = 0; s < kStates; ++s) {
      const ImVec2 cc(x0 + colW * (static_cast<float>(s) + 0.5f), y + r);
      P.knob(dl, cc, r, knobState(s, static_cast<float>(c.t), 2 - z));
      if (z == 0) {
        char b[16];
        std::snprintf(b, sizeof b, s == 5 ? "-40%%" : "%d%%", static_cast<int>(knobState(s, 0, 2).norm * 100));
        P.valueText(dl, ImVec2(cc.x - 40, cc.y + r + 6), ImVec2(cc.x + 40, cc.y + r + 20), b, s == 2 ? Active : 0);
      }
    }
    y += r * 2 + (z == 0 ? 34 : 18);
  }
  // ---- sliders and fader
  P.section(dl, ImVec2(o.x + 792, o.y), ImVec2(o.x + W, o.y + 300), "Sliders, fader, meters");
  {
    const float sx = o.x + 806;
    const int st[4] = {0, Hover, Active, Disabled};
    for (int i = 0; i < 4; ++i) {
      Slider s;
      s.norm = 0.35f + 0.12f * static_cast<float>(i);
      s.state = st[i];
      P.slider(dl, ImVec2(sx, o.y + 26 + 26 * static_cast<float>(i)), ImVec2(sx + 170, o.y + 48 + 26 * static_cast<float>(i)), s);
    }
    Slider sb;
    sb.norm = 0.3f, sb.bipolar = true;
    P.slider(dl, ImVec2(sx, o.y + 130), ImVec2(sx + 170, o.y + 152), sb);
    Slider sm;
    sm.norm = 0.5f, sm.modDepth = 0.25f, sm.modNow = 0.5f + 0.25f * (0.5f + 0.5f * static_cast<float>(std::sin(c.t * 2)));
    P.slider(dl, ImVec2(sx, o.y + 160), ImVec2(sx + 170, o.y + 182), sm);
    rowLabel(c, sx, o.y + 186, 170, "H: idle hover drag off bip mod");
    for (int i = 0; i < 4; ++i) {
      Slider s;
      s.vertical = true, s.norm = 0.25f + 0.18f * static_cast<float>(i), s.state = i == 1 ? Hover : i == 2 ? Active : 0;
      if (i == 3) s.bipolar = true, s.norm = 0.7f, s.modDepth = -0.3f, s.modNow = 0.55f;
      P.slider(dl, ImVec2(sx + 186 + 26 * static_cast<float>(i), o.y + 24), ImVec2(sx + 208 + 26 * static_cast<float>(i), o.y + 200), s);
    }
    // Fader with live meters.
    Fader f;
    f.norm = 0.8f;
    const float lv = 0.55f + 0.3f * std::fabs(static_cast<float>(std::sin(c.t * 5.0)));
    f.meterL = lv, f.meterR = lv * 0.9f, f.peakL = 0.9f, f.peakR = 0.86f;
    P.fader(dl, ImVec2(sx + 300, o.y + 20), ImVec2(sx + 360, o.y + 290), f);
    Fader f2 = f;
    f2.norm = 0.55f, f2.state = Active;
    P.fader(dl, ImVec2(sx + 370, o.y + 20), ImVec2(sx + 430, o.y + 290), f2);
    // Meters.
    Meter m;
    m.l = 0.72f, m.r = 0.66f, m.peakL = 0.84f, m.peakR = 0.8f;
    P.meter(dl, ImVec2(sx + 440, o.y + 30), ImVec2(sx + 452, o.y + 220), m);
    Meter mr = m;
    mr.l = 0.85f, mr.r = 0.8f, mr.rmsL = 0.6f, mr.rmsR = 0.55f, mr.peakL = 0.97f, mr.clip = true;
    P.meter(dl, ImVec2(sx + 462, o.y + 30), ImVec2(sx + 474, o.y + 220), mr);
    Meter g;
    g.l = 0.3f, g.reduction = true;
    P.meter(dl, ImVec2(sx + 484, o.y + 30), ImVec2(sx + 490, o.y + 220), g);
    Meter h;
    h.l = 0.6f, h.r = 0.52f, h.peakL = 0.75f, h.peakR = 0.7f, h.vertical = false;
    P.meter(dl, ImVec2(sx, o.y + 214), ImVec2(sx + 280, o.y + 226), h);
    Meter h2;
    h2.l = 0.96f, h2.vertical = false, h2.clip = true, h2.peakL = 0.98f;
    P.meter(dl, ImVec2(sx, o.y + 234), ImVec2(sx + 280, o.y + 240), h2);
    rowLabel(c, sx, o.y + 248, 280, "Meters: stereo, mono, clip");
    rowLabel(c, sx + 430, o.y + 228, 80, "pk rms gr");
  }
  // ---- buttons
  y = o.y + 312;
  P.section(dl, ImVec2(o.x, y), ImVec2(o.x + 780, y + 196), "Buttons, toggles, lamps");
  const char* const bst[5] = {"Idle", "Hover", "Pressed", "On", "Disabled"};
  const int bfl[5] = {0, Hover, Active, On, Disabled};
  for (int i = 0; i < 5; ++i) {
    const float bx = x0 + 110 * static_cast<float>(i);
    P.label(dl, ImVec2(bx, y + 22), ImVec2(bx + 96, y + 36), bst[i], true);
    P.button(dl, ImVec2(bx, y + 42), ImVec2(bx + 96, y + 66), "Retrigger", bfl[i], 0);
    P.iconButton(dl, ImVec2(bx, y + 76), ImVec2(bx + 26, y + 102), static_cast<int>(ui::Icon::Play), bfl[i], rgb(0xB1E758));
    P.iconButton(dl, ImVec2(bx + 34, y + 76), ImVec2(bx + 60, y + 102), static_cast<int>(ui::Icon::Loop), bfl[i], 0);
    P.iconButton(dl, ImVec2(bx + 68, y + 76), ImVec2(bx + 94, y + 102), static_cast<int>(ui::Icon::Metronome), bfl[i], 0);
    P.check(dl, ImVec2(bx, y + 112), ImVec2(bx + 96, y + 128), "Legato", bfl[i], 0);
    P.toggle(dl, ImVec2(bx, y + 138), ImVec2(bx + 34, y + 156), i == 3 ? 1.0f : 0.0f, bfl[i], 0);
  }
  rowLabel(c, o.x + 10, y + 46, 130, "Text button");
  rowLabel(c, o.x + 10, y + 82, 130, "Icon buttons");
  rowLabel(c, o.x + 10, y + 113, 130, "Check box");
  rowLabel(c, o.x + 10, y + 140, 130, "Switch");
  // LEDs.
  rowLabel(c, o.x + 10, y + 166, 130, "Lamps");
  const ImU32 roles[4] = {0, rgb(0xFFB547), rgb(0xFF5B53), rgb(0x4ECCFF)};
  for (int i = 0; i < 8; ++i) {
    const float lx = x0 + 10 + 36 * static_cast<float>(i);
    const float lev = i == 6 ? 0.5f + 0.5f * static_cast<float>(std::sin(c.t * 6)) : 0.0f;
    P.led(dl, ImVec2(lx, y + 174), 5, i % 2 == 1, i == 4 ? 0.35f : lev, roles[(i / 2) % 4], 0);
  }
  P.led(dl, ImVec2(x0 + 330, y + 174), 3.5f, true, 0, 0, 0);
  P.led(dl, ImVec2(x0 + 350, y + 174), 7, true, 0, roles[1], 0);
  P.led(dl, ImVec2(x0 + 376, y + 174), 7, false, 0, roles[1], 0);
  // ---- selectors / fields
  P.section(dl, ImVec2(o.x + 792, y), ImVec2(o.x + W, y + 196), "Selectors and fields");
  {
    const float sx = o.x + 806;
    const char* const seg[4] = {"Sine", "Tri", "Saw", "Pulse"};
    P.segmented(dl, ImVec2(sx, y + 24), ImVec2(sx + 230, y + 48), seg, 4, 2, 1, 0);
    const char* const tb[3] = {"Oscillator", "Filter", "Matrix"};
    P.tabs(dl, ImVec2(sx + 246, y + 22), ImVec2(sx + 540, y + 48), tb, 3, 0, 2);
    P.dropdown(dl, ImVec2(sx, y + 58), ImVec2(sx + 150, y + 82), "Warm Low 12", 0);
    P.dropdown(dl, ImVec2(sx + 160, y + 58), ImVec2(sx + 310, y + 82), "Ladder 24", Hover | Active);
    const char* const items[4] = {"Warm Low 12", "Ladder 24", "Band Pass", "Comb"};
    P.menu(dl, ImVec2(sx + 160, y + 86), ImVec2(sx + 310, y + 186), items, 4, 1, 1);
    P.field(dl, ImVec2(sx, y + 92), ImVec2(sx + 70, y + 114), "-6.0 dB", 0, -1, false);
    P.field(dl, ImVec2(sx + 78, y + 92), ImVec2(sx + 148, y + 114), "250 ms", Active, 3, false);
    P.field(dl, ImVec2(sx, y + 124), ImVec2(sx + 148, y + 152), "140.000", 0, -1, true);
    P.stepper(dl, ImVec2(sx, y + 160), ImVec2(sx + 148, y + 184), "1/16", 0, 1);
    P.field(dl, ImVec2(sx + 330, y + 58), ImVec2(sx + 540, y + 90), "001:03:192", Hover, -1, true);
    P.caption(dl, ImVec2(sx + 330, y + 100), ImVec2(sx + 420, y + 116), "Cutoff");
    P.valueText(dl, ImVec2(sx + 430, y + 100), ImVec2(sx + 540, y + 116), "1.25 kHz", 0);
    P.tooltip(dl, ImVec2(sx + 330, y + 126), "Cutoff", "1.25 kHz");
    P.scrollbar(dl, ImVec2(sx + 470, y + 126), ImVec2(sx + 480, y + 186), 0.3f, 0.35f, 0);
    P.scrollbar(dl, ImVec2(sx + 490, y + 126), ImVec2(sx + 500, y + 186), 0.6f, 0.35f, Hover);
    P.scrollbar(dl, ImVec2(sx + 510, y + 126), ImVec2(sx + 520, y + 186), 0.8f, 0.35f, Active);
  }
  // ---- steps and pads
  y += 208;
  P.section(dl, ImVec2(o.x, y), ImVec2(o.x + W, y + 150), "Step sequencer and pads");
  {
    const int playing = static_cast<int>(c.t * 8) % 16;
    for (int i = 0; i < 16; ++i) {
      const float sx = o.x + 14 + 30 * static_cast<float>(i) + 8 * static_cast<float>(i / 4);
      const bool on = (i % 4 == 0) || i == 6 || i == 11 || i == 14;
      const float vel = 0.35f + 0.65f * static_cast<float>((i * 37) % 10) / 9.0f;
      P.stepPad(dl, ImVec2(sx, y + 30), ImVec2(sx + 26, y + 64), on, (i / 4) % 2 == 1, vel, i == playing, i == 9 ? Hover : 0, 0);
      P.stepPad(dl, ImVec2(sx, y + 72), ImVec2(sx + 26, y + 106), i % 3 == 0, (i / 4) % 2 == 1, 1.0f, false, 0, rgb(0x5FA8F0));
    }
    rowLabel(c, o.x + 14, y + 114, 540, "Steps: velocity as brightness, alternate groups, playing step, channel tint");
    const char* const pl[4] = {"Kick", "Snare", "Hat", "Clap"};
    for (int i = 0; i < 4; ++i) {
      const float px = o.x + 580 + 92 * static_cast<float>(i);
      float g = i == 1 ? 1.0f : i == 2 ? 0.45f : 0.0f;
      if (i == 3) g = static_cast<float>(std::fmod(c.t * 1.5, 1.0));
      P.pad(dl, ImVec2(px, y + 28), ImVec2(px + 82, y + 110), pl[i], i == 3 ? 1.0f - g : g, i == 0 ? Hover : 0, i == 2 ? rgb(0x5FA8F0) : 0);
    }
    rowLabel(c, o.x + 580, y + 114, 360, "Pads: idle, hit, decaying, tinted");
    P.modChip(dl, ImVec2(o.x + 970, y + 30), ImVec2(o.x + 1090, y + 54), "LFO 1", 0.5f + 0.5f * static_cast<float>(std::sin(c.t * 3)), 0);
    P.modChip(dl, ImVec2(o.x + 970, y + 62), ImVec2(o.x + 1090, y + 86), "Env 2", 0.8f, Hover);
    P.modAmount(dl, ImVec2(o.x + 1100, y + 34), ImVec2(o.x + 1250, y + 50), 0.6f, static_cast<float>(std::sin(c.t * 3)), 0);
    P.modAmount(dl, ImVec2(o.x + 1100, y + 66), ImVec2(o.x + 1250, y + 82), -0.35f, 0.5f, Hover);
    rowLabel(c, o.x + 970, y + 114, 280, "Mod sources and matrix amounts (live)");
  }
}

// ------------------------------------------------------------------ page 2
void synthData(float* scope, int ns, float* spec, int nf, float* mn, float* mx, int nw, double t) {
  for (int i = 0; i < ns; ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(ns);
    scope[i] = 0.7f * std::sin(x * 6.2832f * 3 + static_cast<float>(t)) + 0.2f * std::sin(x * 6.2832f * 11) * std::sin(static_cast<float>(t) * 0.7f);
  }
  for (int i = 0; i < nf; ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(nf);
    spec[i] = -18 - 50 * x - 8 * std::sin(x * 40) * (1 - x) + 12 * std::exp(-std::pow((x - 0.18f) * 14, 2.0f)) - 4 * std::sin(x * 9 + static_cast<float>(t));
  }
  for (int i = 0; i < nw; ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(nw);
    const float env = std::exp(-std::fmod(x * 4, 1.0f) * 3.5f) * (0.5f + 0.5f * std::sin(x * 3));
    const float a = env * (0.6f + 0.4f * std::sin(static_cast<float>(i) * 1.7f));
    mx[i] = a, mn[i] = -a * 0.92f;
  }
}

void envPts(ImVec2* p, ImVec2 a, ImVec2 b) {
  const float w = b.x - a.x, h = b.y - a.y;
  p[0] = ImVec2(a.x + 6, b.y - 6);
  p[1] = ImVec2(a.x + w * 0.12f, a.y + 8);
  p[2] = ImVec2(a.x + w * 0.34f, a.y + h * 0.45f);
  p[3] = ImVec2(a.x + w * 0.7f, a.y + h * 0.45f);
  p[4] = ImVec2(b.x - 6, b.y - 6);
}

void page2(Ctx& c, ImVec2 o, float W) {
  Painter& P = c.P;
  ImDrawList* dl = c.dl;
  float scope[256], spec[200], mn[300], mx[300];
  synthData(scope, 256, spec, 200, mn, mx, 300, c.t);
  P.section(dl, ImVec2(o.x, o.y), ImVec2(o.x + W, o.y + 384), "Displays");
  const float gx = o.x + 14, gy = o.y + 28, gw = 330, gh = 108, sp = 16;
  ImVec2 e[5];
  {
    const ImVec2 a(gx, gy), b(gx + gw, gy + gh);
    envPts(e, a, b);
    const float ph = static_cast<float>(std::fmod(c.t * 0.4, 1.0));
    P.envelope(dl, a, b, e, 5, 2, a.x + 6 + (b.x - a.x - 12) * ph, a.y + 8 + (b.y - a.y) * 0.35f);
    rowLabel(c, a.x, b.y + 4, gw, "Envelope editor: handle, live stage dot");
  }
  {
    const ImVec2 a(gx + gw + sp, gy), b(a.x + gw, gy + gh);
    P.lfo(dl, a, b, 1, static_cast<float>(std::fmod(c.t * 0.5, 1.0)), 0.9f);
    rowLabel(c, a.x, b.y + 4, gw, "LFO: shape, live phase");
  }
  {
    const ImVec2 a(gx + (gw + sp) * 2, gy), b(a.x + gw, gy + gh);
    P.scope(dl, a, b, scope, 256);
    rowLabel(c, a.x, b.y + 4, gw, "Oscilloscope (live)");
  }
  {
    const ImVec2 a(gx + (gw + sp) * 3, gy), b(o.x + W - 14, gy + gh);
    P.xyPad(dl, a, b, 0.35f, 0.62f, 0.35f + 0.2f * static_cast<float>(std::cos(c.t)), 0.62f + 0.2f * static_cast<float>(std::sin(c.t)), Active);
    rowLabel(c, a.x, b.y + 4, b.x - a.x, "XY pad: point + live modulated point");
  }
  const float gy2 = gy + gh + 26;
  {
    const ImVec2 a(gx, gy2), b(gx + gw, gy2 + gh);
    P.spectrum(dl, a, b, spec, 200, 0);
    rowLabel(c, a.x, b.y + 4, gw, "Spectrum (live)");
  }
  {
    const ImVec2 a(gx + gw + sp, gy2), b(a.x + gw, gy2 + gh);
    P.waveform(dl, a, b, mn, mx, 300, static_cast<float>(std::fmod(c.t * 0.2, 1.0)), 0.2f, 0.38f);
    rowLabel(c, a.x, b.y + 4, gw, "Waveform: selection, playhead");
  }
  {
    const ImVec2 a(gx + (gw + sp) * 2, gy2), b(a.x + gw, gy2 + gh);
    P.wavetable(dl, a, b, 16, 5.0f + 4.0f * static_cast<float>(std::sin(c.t * 0.5)), 1.0f);
    rowLabel(c, a.x, b.y + 4, gw, "Wavetable frames, live position");
  }
  {
    const ImVec2 a(gx + (gw + sp) * 3, gy2), b(o.x + W - 14, gy2 + gh);
    P.grNeedle(dl, a, b, -6.5f - 3.0f * static_cast<float>(std::sin(c.t * 2)), -12, -18);
    rowLabel(c, a.x, b.y + 4, b.x - a.x, "Gain reduction gauge");
  }
  {
    bool held[128] = {};
    held[60] = held[64] = held[67] = held[70] = true;
    P.keyboard(dl, ImVec2(gx, gy2 + gh + 28), ImVec2(gx + 700, gy2 + gh + 76), 48, 84, held, 55, 72);
    rowLabel(c, gx + 720, gy2 + gh + 44, 400, "Keyboard: held keys, mapped range");
  }

  // ---- filmstrips
  const float fy = o.y + 396;
  P.section(dl, ImVec2(o.x, fy), ImVec2(o.x + W, fy + 420), "Live elements, six frames each");
  const int N = 6;
  const float fw = 150;
  const float fx0 = o.x + 160;
  for (int f = 0; f < N; ++f) {
    char b[16];
    std::snprintf(b, sizeof b, "t%d", f);
    P.label(dl, ImVec2(fx0 + fw * static_cast<float>(f), fy + 22), ImVec2(fx0 + fw * static_cast<float>(f + 1), fy + 36), b, true);
  }
  for (int f = 0; f < N; ++f) {
    const float ph = static_cast<float>(f) / static_cast<float>(N);
    const float cx = fx0 + fw * (static_cast<float>(f) + 0.5f);
    // Modulated knob.
    Knob k;
    k.norm = 0.5f, k.modDepth = 0.35f, k.modNow = 0.5f + 0.35f * (0.5f + 0.5f * std::sin(ph * 6.2832f)), k.size = 2;
    P.knob(dl, ImVec2(cx - 30, fy + 70), 22, k);
    // Knob with live level inside.
    Knob kl;
    kl.norm = 0.7f, kl.level = 0.5f + 0.5f * std::sin(ph * 6.2832f), kl.size = 2;
    P.knob(dl, ImVec2(cx + 30, fy + 70), 22, kl);
    // Meter.
    Meter m;
    m.l = 0.45f + 0.5f * std::fabs(std::sin(ph * 6.2832f * 0.75f)), m.r = m.l * 0.92f, m.peakL = 0.92f, m.peakR = 0.88f, m.vertical = false;
    m.clip = f == 2;
    P.meter(dl, ImVec2(cx - 62, fy + 112), ImVec2(cx + 62, fy + 124), m);
    // LFO phase.
    P.lfo(dl, ImVec2(cx - 66, fy + 136), ImVec2(cx + 66, fy + 196), 0, ph, 0.9f);
    // Step playhead.
    for (int s = 0; s < 4; ++s) {
      const float sx = cx - 62 + 32 * static_cast<float>(s);
      P.stepPad(dl, ImVec2(sx, fy + 206), ImVec2(sx + 28, fy + 230), s != 2, false, 0.6f + 0.13f * static_cast<float>(s), s == f % 4, 0, 0);
    }
    // Pad hit decay.
    P.pad(dl, ImVec2(cx - 62, fy + 240), ImVec2(cx + 62, fy + 300), "Kick", std::max(0.0f, 1.0f - ph * 1.6f), 0, 0);
    // Switch animation.
    P.toggle(dl, ImVec2(cx - 62, fy + 312), ImVec2(cx - 28, fy + 330), std::min(1.0f, ph * 1.4f), 0, 0);
    // LED pulse.
    P.led(dl, ImVec2(cx - 8, fy + 321), 5, false, 0.5f + 0.5f * std::cos(ph * 6.2832f), 0, 0);
    // XY live point.
    P.xyPad(dl, ImVec2(cx + 10, fy + 306), ImVec2(cx + 66, fy + 362), 0.4f, 0.5f, 0.4f + 0.3f * std::cos(ph * 6.2832f), 0.5f + 0.3f * std::sin(ph * 6.2832f), 0);
  }
  rowLabel(c, o.x + 10, fy + 62, 140, "Mod ring / level");
  rowLabel(c, o.x + 10, fy + 110, 140, "Meter + clip");
  rowLabel(c, o.x + 10, fy + 158, 140, "LFO phase");
  rowLabel(c, o.x + 10, fy + 210, 140, "Step playhead");
  rowLabel(c, o.x + 10, fy + 262, 140, "Pad hit decay");
  rowLabel(c, o.x + 10, fy + 322, 140, "Switch, lamp, XY");
}

}  // namespace

void drawGallery(Dir d, double t, int page) {
  const ui::ControlStyleScope scope(d);  // Classic draws through the shared drawers as they are
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
  Painter& P = painter(d);
  const ImVec2 p0 = vp->Pos, p1(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
  dl->AddRectFilled(p0, p1, P.tk.panelBot);
  P.panel(dl, ImVec2(p0.x + 8, p0.y + 8), ImVec2(p1.x - 8, p1.y - 8));
  char title[96];
  std::snprintf(title, sizeof title, "%s  /  %s", dirTitle(d), page == 2 ? "displays and motion" : "controls");
  P.header(dl, ImVec2(p0.x + 8, p0.y + 8), ImVec2(p1.x - 8, p0.y + 52), title, blurb(d));
  Ctx c{P, dl, t};
  const ImVec2 o(p0.x + 24, p0.y + 66);
  const float W = vp->Size.x - 48;
  if (page == 2) page2(c, o, W);
  else page1(c, o, W);
}

}  // namespace gl::look
