// Gridline — "plasma" pack: the Synth tab's block bodies in Simple (see plasma_ui.h, "blocks").
// Each block is a live picture and its main dials: the layers a large picture over four dials,
// the rest a picture beside two or four. Pictures move with the sound (the playing frame, the
// cutoff, the envelope's stage, the LFO's phase) and a double-click opens the large editor.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include "../../app/Theme.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
// A dial of the design: s = its diameter (design px), top = the top of its cell.
void dialAt(Ctx& c, int k, float cx, float top, float s, const char* label, int flags = 0) {
  knob(c, k, cx, top + M(s * 0.5f), M(s * 0.5f), label, KnobValue | flags);
}
// Four dials in two rows of two at the right of a picture (60 px cells), centred in h.
void dials4(Ctx& c, float x, float y, float h, const int k[4], const char* const l[4], int flags = 0) {
  const float cellH = M(36 + 2 + 13 + 12), gap = M(4), tot = cellH * 2 + gap;
  const float top = y + std::max(0.f, (h - tot) * 0.5f);
  for (int i = 0; i < 4; ++i)
    dialAt(c, k[i], x + M(30) + (i % 2) * M(62), top + (i / 2) * (cellH + gap), 36, l[i], flags);
}
void dials2(Ctx& c, float x, float y, float h, const int k[2], const char* const l[2]) {
  const float cellH = M(34 + 2 + 13 + 12), top = y + std::max(0.f, (h - cellH) * 0.5f);
  for (int i = 0; i < 2; ++i) dialAt(c, k[i], x + M(30) + i * M(62), top, 34, l[i]);
}
constexpr float kDials4W = M(124), kDials2W = M(124), kHzGap = M(10);

std::vector<ImVec2> sampled(const Ctx& c, float x, float y, float w, float h, int n, const std::function<float(float)>& f) {
  std::vector<ImVec2> v(static_cast<size_t>(n + 1));
  for (int i = 0; i <= n; ++i) {
    const float u = i / static_cast<float>(n);
    v[static_cast<size_t>(i)] = c.p(x + u * w, y + h - std::clamp(f(u), -0.05f, 1.05f) * h);
  }
  return v;
}
float nowSec() { return static_cast<float>(ImGui::GetTime()); }
bool playing(const Ctx& c) { return c.hasLive && c.live.voices > 0; }

// An envelope as the design draws it: attack, decay, a held sustain, release; the live stage as a
// lit dot on the curve.
void adsrPicture(Ctx& c, int e, float x, float y, float w, float h, ImU32 col, int nx) {
  wellLook(c, x, y, w, h);
  gridLook(c, x, y, w, h, nx, 4, false);
  const float a = 0.03f + 0.9f * valf(c, envP(e, E_Attack)), d = 0.05f + 1.2f * valf(c, envP(e, E_Decay));
  const float s = std::clamp(valf(c, envP(e, E_Sustain)), 0.f, 1.f), r = 0.05f + 1.2f * valf(c, envP(e, E_Release));
  const float hold = 0.9f, tot = a + d + hold + r, pad = M(6);
  const float sx = (w - pad * 2) / tot, top = y + pad + M(4), bot = y + h - pad;
  auto Y = [&](float v) { return bot - (bot - top) * v; };
  std::vector<ImVec2> pts;
  pts.push_back(c.p(x + pad, bot));
  for (int i = 1; i <= 16; ++i) { const float u = i / 16.f; pts.push_back(c.p(x + pad + a * sx * u, Y(1 - std::pow(1 - u, 2.2f)))); }
  for (int i = 1; i <= 20; ++i) { const float u = i / 20.f; pts.push_back(c.p(x + pad + (a + d * u) * sx, Y(s + (1 - s) * std::pow(1 - u, 3.f)))); }
  pts.push_back(c.p(x + pad + (a + d + hold) * sx, Y(s)));
  for (int i = 1; i <= 20; ++i) { const float u = i / 20.f; pts.push_back(c.p(x + pad + (a + d + hold + r * u) * sx, Y(s * std::pow(1 - u, 2.6f)))); }
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + w - 1, y + h - 1), true);
  fillUnder(c, pts, c.p(0, bot).y, col, 0.25f, 0.02f, c.p(0, y).y);
  glowLine(c, pts, col, M(2));
  if (playing(c) && c.live.envStage[e] > 0) {  // where the lead note is now
    const float lv = std::clamp(c.live.env[e], 0.f, 1.f);
    float tx = 0, ty = lv;
    switch (c.live.envStage[e]) {
      case 1: tx = a * (1 - std::pow(1 - lv, 1 / 2.2f)); break;
      case 2: tx = a; break;
      case 3: tx = a + d * (1 - std::pow(std::clamp((lv - s) / std::max(1e-3f, 1 - s), 0.f, 1.f), 1 / 3.f)); break;
      case 4: tx = a + d + hold * 0.5f, ty = s; break;
      default: tx = a + d + hold + r * (1 - std::pow(std::clamp(lv / std::max(1e-3f, s), 0.f, 1.f), 1 / 2.6f)); break;
    }
    glowDot(c, c.p(x + pad + tx * sx, Y(ty)), M(3.4f), col);
  }
  c.dl->PopClipRect();
}
}  // namespace

// ---- Layers: the picture over Position, Warp, Unison and Level
void layerBlockBody(Ctx& c, int o, float x, float y, float w, float h) {
  ImGui::PushID(o + 8100);
  const float kh = M(64), ph = std::max(M(40), h - kh - M(6));
  layerPicture(c, o, x, y, w, ph);
  const int type = vali(c, oscP(o, O_Type));
  const int kPos = type == OscGranular || type == OscSpectral ? oscP(o, O_Position) : type == OscMultisample ? oscP(o, O_MTimbre)
                   : type == OscSample                        ? oscP(o, O_ScanRate) : oscP(o, O_WtPos);
  const char* posName = type == OscMultisample ? "Timbre" : type == OscSample ? "Speed" : "Position";
  const int ks[4] = {kPos, oscP(o, O_Warp1), oscP(o, O_Unison), oscP(o, O_Level)};
  const char* ls[4] = {posName, "Warp", "Unison", "Level"};
  const float top = y + h - kh;
  for (int i = 0; i < 4; ++i) dialAt(c, ks[i], x + w * (i + 0.5f) / 4, top, 40, ls[i]);
  ImGui::PopID();
}

// ---- Root: one cycle of its shape (moving while it plays) beside Octave and Level
void rootBlockBody(Ctx& c, float x, float y, float w, float h) {
  ImGui::PushID("rootb");
  const float pw = w - kDials2W - kHzGap;
  wellLook(c, x, y, pw, h);
  gridLook(c, x, y, pw, h, 4, 2, true);
  const int shape = vali(c, subP(S_Shape));
  const ImU32 col = val(c, subP(S_Enable)) >= 0.5 ? partCol(c, 3) : c.P.textFaint;
  const float ph = playing(c) ? nowSec() * 0.4f : 0.f;
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + pw - 1, y + h - 1), true);
  auto pts = sampled(c, x, y, pw, h, 80, [&](float u) {
    float t = u * 1.5f - ph;
    t -= std::floor(t);
    return 0.5f + rootWaveAt(shape, t) * 0.3f;
  });
  fillUnder(c, pts, c.p(0, y + h * 0.5f).y, col, 0.22f, 0.02f, c.p(0, y + h * 0.2f).y);
  glowLine(c, pts, col, M(1.8f));
  c.dl->PopClipRect();
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##pic", c.F.sz(pw, h));
  markPicture();
  const int ks[2] = {subP(S_Octave), subP(S_Level)};
  const char* const ls[2] = {"Octave", "Level"};
  dials2(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

// ---- Texture: a soft band of its noise beside Colour and Level
void textureBlockBody(Ctx& c, float x, float y, float w, float h) {
  ImGui::PushID("texb");
  const float pw = w - kDials2W - kHzGap;
  wellLook(c, x, y, pw, h);
  const ImU32 col = val(c, noiseP(N_Enable)) >= 0.5 ? partCol(c, 4) : c.P.textFaint;
  const int step = playing(c) ? static_cast<int>(nowSec() * 14) : 0;
  const float colour = valf(c, noiseP(N_Color));
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + pw - 1, y + h - 1), true);
  auto pts = sampled(c, x, y, pw, h, 110, [&](float u) {
    const int i = static_cast<int>(u * 110);
    const float n = std::sin(i * 12.9898f + step * 78.233f) * 43758.5453f;
    const float r = (n - std::floor(n)) * 2 - 1;
    return 0.5f + r * 0.26f * (0.5f + 0.5f * std::sin(u * 3 + 1)) * (0.6f + 0.6f * colour);
  });
  glowLine(c, pts, theme::withAlpha(col, 0.8f), M(1), 0.4f);
  c.dl->PopClipRect();
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##pic", c.F.sz(pw, h));
  markPicture();
  const int ks[2] = {noiseP(N_Color), noiseP(N_Level)};
  const char* const ls[2] = {"Colour", "Level"};
  dials2(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

// ---- Filters: the response (live cutoff), drag it; Cutoff, Resonance, Drive, Key track
void filterBlockBody(Ctx& c, int f, float x, float y, float w, float h) {
  ImGui::PushID(f + 8200);
  const float pw = w - kDials4W - kHzGap;
  wellLook(c, x, y, pw, h);
  gridLook(c, x, y, pw, h, 6, 4, false);
  const ImU32 col = val(c, filtP(f, F_Enable)) >= 0.5 ? partCol(c, 10 + f) : c.P.textFaint;
  constexpr int N = 120;
  float hz[N], mag[N];
  for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
  double cut = cutoffHz(val(c, filtP(f, F_Freq)));
  if (playing(c) && c.live.filterCut[f] > 0) cut = c.live.filterCut[f];
  Filter::responseCurve(vali(c, filtP(f, F_Type)), 48000, cut, val(c, filtP(f, F_Res)), val(c, filtP(f, F_Var)), hz, mag, N);
  std::vector<ImVec2> pts(N);
  const float top = h * 0.2f;
  for (int i = 0; i < N; ++i) {
    const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
    pts[static_cast<size_t>(i)] = c.p(x + pw * i / (N - 1), y + std::clamp(top + (-db / 30.f) * (h - top), 2.f, h + 4));
  }
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + pw - 1, y + h - 1), true);
  fillUnder(c, pts, c.p(0, y + h).y, col, 0.26f, 0.02f, c.p(0, y + top).y);
  glowLine(c, pts, col, M(2));
  const float cx = static_cast<float>(std::clamp(std::log(cut / 20.0) / std::log(1000.0), 0.0, 1.0));
  glowDot(c, pts[static_cast<size_t>(std::lround(cx * (N - 1)))], M(3.2f), col);
  c.dl->PopClipRect();
  // drag the picture: cutoff across, resonance up and down
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##fg", c.F.sz(pw, h));
  markPicture();
  if (ImGui::IsItemActive() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
    const ImVec2 m = ImGui::GetIO().MousePos, a = c.p(x, y), b = c.p(x + pw, y + h);
    const double fx = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 1.f), fy = std::clamp(1.0 - (m.y - a.y) / (b.y - a.y), 0.0, 1.0);
    setv(c, filtP(f, F_Freq), std::clamp(cutoffNorm(20.0 * std::pow(1000.0, fx)), 0.0, 1.0), true);
    setv(c, filtP(f, F_Res), fy, true);
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  if (ImGui::IsItemHovered()) c.a.setHint("Drag: cutoff (left / right) and resonance (up / down) \xC2\xB7 double-click: the filter page");
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Dest)) == filtP(f, F_Freq) + 1 && vali(c, modP(s, M_Source)) > SrcNone) {
      const int src = vali(c, modP(s, M_Source));
      modChip(c, x + pw - M(7), y + M(5), dockName(src).c_str(), c.P.fam[familyOf(std::max(src, 0))]);
      break;
    }
  const int ks[4] = {filtP(f, F_Freq), filtP(f, F_Res), filtP(f, F_Drive), filtP(f, F_KeyTrack)};
  const char* const ls[4] = {"Cutoff", "Resonance", "Drive", "Key track"};
  dials4(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

// ---- Amp and the envelopes: the shape with the live stage; Attack, Decay, Sustain, Release
void envBlockBodyDesign(Ctx& c, int e, float x, float y, float w, float h, int nx) {
  ImGui::PushID(e + 8300);
  const float pw = w - kDials4W - kHzGap;
  adsrPicture(c, e, x, y, pw, h, e == 0 ? partCol(c, 20) : partCol(c, 200 + e), nx);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##pic", c.F.sz(pw, h));
  markPicture();
  if (ImGui::IsItemHovered()) c.a.setHint("Envelope " + std::to_string(e + 1) + ": double-click to shape it in the envelope editor");
  const int ks[4] = {envP(e, E_Attack), envP(e, E_Decay), envP(e, E_Sustain), envP(e, E_Release)};
  const char* const ls[4] = {"Attack", "Decay", "Sustain", "Release"};
  dials4(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

// ---- LFOs: the shape, its phase as a moving head with a lit trail; Rate, Rise, Delay, Smooth
void lfoBlockBodyDesign(Ctx& c, int l, float x, float y, float w, float h) {
  ImGui::PushID(l + 8400);
  const float pw = w - kDials4W - kHzGap;
  wellLook(c, x, y, pw, h);
  gridLook(c, x, y, pw, h, 4, 4, true);
  const ImU32 col = partCol(c, 100 + l);
  const Curve cv = lfoShapePublic(c, l);
  const float pad = M(6);
  auto shapeAt = [&](float u) { return static_cast<float>(std::clamp(curveAt(cv, u), 0.0, 1.0)); };
  auto Y = [&](float v) { return y + h - pad - v * (h - pad * 2); };
  std::vector<ImVec2> pts(121);
  for (int i = 0; i <= 120; ++i) pts[static_cast<size_t>(i)] = c.p(x + pw * i / 120.f, Y(shapeAt(i / 120.f)));
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + pw - 1, y + h - 1), true);
  fillUnder(c, pts, c.p(0, y + h).y, col, 0.2f, 0.01f, c.p(0, y).y);
  c.dl->AddPolyline(pts.data(), 121, theme::withAlpha(col, 0.55f), 0, c.s(M(1.6f)));
  if (playing(c)) {
    const float ph = c.live.lfoPhase[l] - std::floor(c.live.lfoPhase[l]);
    std::vector<ImVec2> tr;
    for (int i = 0; i <= 30; ++i) {
      const float u = ph - 0.28f + 0.28f * i / 30.f;
      if (u >= 0) tr.push_back(c.p(x + u * pw, Y(shapeAt(u))));
    }
    if (tr.size() > 1) glowLine(c, tr, col, M(2.4f));
    vline(c, x + ph * pw, y, y + h, theme::withAlpha(col, 0.35f));
    glowDot(c, c.p(x + ph * pw, Y(shapeAt(ph))), M(3.6f), col);
  }
  c.dl->PopClipRect();
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##pic", c.F.sz(pw, h));
  markPicture();
  if (ImGui::IsItemHovered()) c.a.setHint("LFO " + std::to_string(l + 1) + ": double-click to draw its shape in the LFO editor");
  const int ks[4] = {lfoP(l, L_Rate), lfoP(l, L_Rise), lfoP(l, L_Delay), lfoP(l, L_Smooth)};
  const char* const ls[4] = {"Rate", "Rise", "Delay", "Smooth"};
  dials4(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

// ---- Macros: four as columns, then their dials (the page of four shown)
void macroBlockBodyDesign(Ctx& c, int first, float x, float y, float w, float h) {
  ImGui::PushID("macb");
  const float pw = w - kDials4W - kHzGap;
  wellLook(c, x, y, pw, h);
  const ImU32 col = partCol(c, 300);
  const float bw = pw / 4;
  for (int i = 0; i < 4; ++i) {
    const int m = first + i;
    const float q = c.hasLive ? std::clamp(c.live.macro[m], 0.f, 1.f) : valf(c, macroP(m));
    const float X = x + i * bw + bw * 0.28f, W = bw * 0.44f, top = y + M(10), bot = y + h - M(18), Yv = bot - (bot - top) * q;
    c.dl->AddRectFilled(c.p(X, top), c.p(X + W, bot), theme::withAlpha(c.P.text, c.P.dark ? 0.045f : 0.06f));
    c.dl->AddRectFilledMultiColor(c.p(X, Yv), c.p(X + W, bot), theme::withAlpha(col, 0.85f), theme::withAlpha(col, 0.85f), theme::withAlpha(col, 0.15f),
                                  theme::withAlpha(col, 0.15f));
    const float g = glowK(c);
    if (g > 0.01f) c.dl->AddRectFilled(c.p(X, Yv - M(1) - 2 * g), c.p(X + W, Yv + M(1) + 2 * g), theme::withAlpha(col, 0.2f * g));
    c.dl->AddRectFilled(c.p(X, Yv - M(1)), c.p(X + W, Yv + M(1)), col);
    char b[16];
    std::snprintf(b, sizeof b, "%d", m + 1);
    MonoText mono;
    textMid(c, X, X + W, y + h - M(9), b, c.P.textFaint, T(10), false);
  }
  int ks[4];
  std::string names[4];
  const char* ls[4];
  for (int i = 0; i < 4; ++i) {
    const int m = first + i;
    ks[i] = macroP(m);
    names[i] = c.m.macroName[static_cast<size_t>(m)].empty() ? "Macro " + std::to_string(m + 1) : c.m.macroName[static_cast<size_t>(m)];
    ls[i] = names[i].c_str();
  }
  dials4(c, x + pw + kHzGap, y, h, ks, ls);
  ImGui::PopID();
}

}  // namespace gl::packs::plasma::ui
