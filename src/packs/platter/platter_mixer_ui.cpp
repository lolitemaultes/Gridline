// Gridline — "platter" pack: the mixer between the decks. One label column in
// the middle (TRIM, HI, MID, LOW, FILTER, RES) with deck A's knobs to its left
// and deck B's to its right, the EQ kills on the outside; the channel faders
// with their pre-fader level (what a cue button is mostly used for: seeing a
// channel's level before it comes in), the master level and meter between
// them; each channel's crossfader assign; the crossfader with its curve, cut
// and reverse; where synced decks take their tempo; the channel fader curve.
// In the one-deck layout it is deck A's channel strip.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../app/Look.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/PluginInfo.h"
#include "platter_ui.h"

namespace gl::packs::platter::pv {

namespace {

using theme::pal;

float dbOf(float level) { return level > 1e-6f ? 20.0f * std::log10(level) : -90.0f; }
float meterNorm(float level) { return look::dbNorm(dbOf(level), 48.0f); }

const ParamDesc* desc(int k) {
  const PluginInfo* info = findPlugin(kType);
  return info && k < static_cast<int>(info->params.size()) ? &info->params[static_cast<size_t>(k)] : nullptr;
}

// A knob (dial only) centred at c; the value shows while hovered or dragged.
void dialAt(App& a, const PluginTarget& t, int k, ImVec2 c, float r) {
  ImGui::SetCursorScreenPos(ImVec2(std::round(c.x - r), std::round(c.y - r)));
  pui::dial(a, t, k, r);
}

// EQ kill: drops the band out, a second click brings it back.
void kill(App& a, const PluginTarget& t, int k, ImVec2 p0, ImVec2 size) {
  const bool killed = param(a, t, k) <= kEqKillDb + 0.01;
  char id[16];
  std::snprintf(id, sizeof id, "##pk%d", k);
  if (button(id, p0, size, i18n::N_("KILL"), killed, i18n::N_("Kill the band (click again to bring it back)")))
    pui::setParamValue(a, t, k, killed ? 0.0 : kEqKillDb, false);
}

// A channel fader: slot, ruler, cap (drag, wheel, double-click resets like any slider).
void channelFader(App& a, const PluginTarget& t, int k, ImVec2 p0, ImVec2 p1, ImU32 tint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ParamDesc* d = desc(k);
  if (!d) return;
  const float cx = std::round((p0.x + p1.x) * 0.5f);
  const float t0 = p0.y + 8, t1 = p1.y - 8, len = t1 - t0;
  for (int i = 0; i <= 10; ++i) {
    const float y = std::round(t0 + len * i / 10.0f);
    const float wv = i % 5 == 0 ? 7.0f : 4.0f;
    dl->AddLine(ImVec2(cx - 9 - wv, y), ImVec2(cx - 9, y), i % 5 == 0 ? P.textDim : P.textFaint, 1);
  }
  skin::insetField(dl, ImVec2(cx - 3, t0 - 3), ImVec2(cx + 3, t1 + 3), P.surface950, 3);
  double v = param(a, t, k);
  const float yv = t1 - static_cast<float>(d->toNormalized(v)) * len;
  if (t1 - yv > 1) dl->AddRectFilled(ImVec2(cx - 1.5f, yv), ImVec2(cx + 1.5f, t1), theme::withAlpha(tint, 0.85f), 1);
  ImGui::SetCursorScreenPos(ImVec2(cx - 16, t0 - 10));
  char id[16];
  std::snprintf(id, sizeof id, "##pfd%d", k);
  ImGui::InvisibleButton(id, ImVec2(32, len + 20));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  pui::dragParamTrack(a, t.pid(k), *d, v, t0, len, false);
  const float y2 = t1 - static_cast<float>(d->toNormalized(v)) * len;
  const ImVec2 q0(cx - 14, y2 - 9), q1(cx + 14, y2 + 9);
  skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? theme::mix(P.surface700, P.paper, 0.06f) : P.surface700, 3);
  dl->AddRect(q0, q1, P.line, 3);
  dl->AddLine(ImVec2(q0.x + 3, y2), ImVec2(q1.x - 3, y2), P.paper, 2);
}

// The crossfader: A left, B right.
void crossfader(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ParamDesc* d = desc(PXfader);
  if (!d) return;
  const float cy = std::round((p0.y + p1.y) * 0.5f);
  const float tx0 = p0.x + 22, tx1 = p1.x - 22;
  textC(dl, a.fontBold, theme::kFontCaption, ImVec2(p0.x + 7, cy), deckColor(0), "A");
  textC(dl, a.fontBold, theme::kFontCaption, ImVec2(p1.x - 7, cy), deckColor(1), "B");
  for (int i = 0; i <= 8; ++i) {
    const float x = std::round(tx0 + (tx1 - tx0) * i / 8.0f);
    dl->AddLine(ImVec2(x, cy - 12), ImVec2(x, cy - (i % 4 == 0 ? 7.0f : 9.0f)), i == 4 ? P.textDim : P.textFaint, 1);
  }
  skin::insetField(dl, ImVec2(tx0 - 3, cy - 3), ImVec2(tx1 + 3, cy + 3), P.surface950, 3);
  double v = param(a, t, PXfader);
  ImGui::SetCursorScreenPos(ImVec2(tx0 - 12, cy - 13));
  traceRect("xfader", ImVec2(tx0, cy - 13), ImVec2(tx1, cy + 13));
  ImGui::InvisibleButton("##pxf", ImVec2(tx1 - tx0 + 24, 26));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  pui::dragParamTrack(a, t.pid(PXfader), *d, v, tx0, tx1 - tx0, true);
  const float xv = tx0 + static_cast<float>(d->toNormalized(v)) * (tx1 - tx0);
  const ImVec2 q0(xv - 9, cy - 12), q1(xv + 9, cy + 12);
  skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? theme::mix(P.surface700, P.paper, 0.06f) : P.surface700, 3);
  dl->AddRect(q0, q1, P.line, 3);
  dl->AddLine(ImVec2(xv, q0.y + 3), ImVec2(xv, q1.y - 3), P.paper, 2);
}

// Small switch bound to a toggle parameter.
void toggleParam(App& a, const PluginTarget& t, int k, ImVec2 p0, ImVec2 size, const char* label, const char* hint) {
  char id[16];
  std::snprintf(id, sizeof id, "##ptg%d", k);
  if (button(id, p0, size, label, on(a, t, k), hint)) flip(a, t, k);
}

}  // namespace

void drawMixer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView* lv, const MixView& mv, bool two) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  (void)e;
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("MIXER"), nullptr);
  const float x0 = c.x, x1 = p1.x - 12, W = x1 - x0;
  const float cx = std::round((x0 + x1) * 0.5f);
  const float r = 13, killW = 34, killH = 18;
  struct Row {
    const char* label;
    int param;
    bool kill;
  };
  static const Row kRows[6] = {{i18n::N_("TRIM"), DTrim, false},   {i18n::N_("HI"), DEqHigh, true},       {i18n::N_("MID"), DEqMid, true},
                               {i18n::N_("LOW"), DEqLow, true},    {i18n::N_("FILTER"), DFilter, false}, {i18n::N_("RES"), DFilterRes, false}};
  const float rowH = 28;
  float y = c.y;
  // knob columns
  const float ax = two ? std::round(x0 + killW + 6 + r + 4) : std::round(x0 + 54 + r);
  const float bx = std::round(x1 - killW - 6 - r - 4);
  const float lx = two ? cx : std::round(x0 + 22);
  for (int i = 0; i < 6; ++i) {
    const Row& rw = kRows[i];
    const float ry = y + i * rowH, cy = ry + rowH * 0.5f;
    if (i == 1 || i == 4) dl->AddLine(ImVec2(x0 + 2, ry - 0.5f), ImVec2(x1 - 2, ry - 0.5f), theme::withAlpha(P.line, 0.6f));
    textC(dl, a.fontMono, theme::kFontLabel, ImVec2(lx, cy), P.textDim, i18n::tr(rw.label));
    dialAt(a, t, deckParam(0, rw.param), ImVec2(ax, cy), r);
    if (two) dialAt(a, t, deckParam(1, rw.param), ImVec2(bx, cy), r);
    if (rw.kill) {
      if (two) {
        kill(a, t, deckParam(0, rw.param), ImVec2(x0, cy - killH * 0.5f), ImVec2(killW, killH));
        kill(a, t, deckParam(1, rw.param), ImVec2(x1 - killW, cy - killH * 0.5f), ImVec2(killW, killH));
      } else {
        kill(a, t, deckParam(0, rw.param), ImVec2(x1 - killW, cy - killH * 0.5f), ImVec2(killW, killH));
      }
    }
  }
  y += 6 * rowH + 6;
  dl->AddLine(ImVec2(x0 + 2, y - 3.5f), ImVec2(x1 - 2, y - 3.5f), theme::withAlpha(P.line, 0.6f));
  // faders, pre-fader meters, master
  const float fh = two ? 112 : 150;
  {
    const float mW = 6;
    // deck A: fader, its meter on the inside
    const float fa = two ? ax : std::round(x0 + 26);
    channelFader(a, t, deckParam(0, DVolume), ImVec2(fa - 16, y), ImVec2(fa + 16, y + fh), deckColor(0));
    look::Meter ma;
    ma.l = meterNorm(lv[0].pre);
    look::meter(dl, ImVec2(fa + 20, y + 8), ImVec2(fa + 20 + mW, y + fh - 8), ma);
    if (two) {
      const float fb = bx;
      channelFader(a, t, deckParam(1, DVolume), ImVec2(fb - 16, y), ImVec2(fb + 16, y + fh), deckColor(1));
      look::Meter mb;
      mb.l = meterNorm(lv[1].pre);
      look::meter(dl, ImVec2(fb - 20 - mW, y + 8), ImVec2(fb - 20, y + fh - 8), mb);
    }
    // master: stereo meter and its level
    const float mx = two ? cx : std::round(x1 - 34);
    look::Meter mm;
    mm.l = meterNorm(mv.masterL), mm.r = meterNorm(mv.masterR);
    mm.clip = mv.masterL > 0.999f || mv.masterR > 0.999f;
    look::meter(dl, ImVec2(mx - 7, y + 8), ImVec2(mx + 7, y + fh - 50), mm);
    dialAt(a, t, PMaster, ImVec2(mx, y + fh - 28), 12);
    textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2(mx, y + fh - 6), P.textFaint, i18n::tr("MASTER"));
  }
  y += fh + 6;
  if (two) {
    // crossfader assign under each fader
    static const char* const kAssign[3] = {i18n::N_("THRU"), "A", "B"};
    static const char* const kAssignHints[3] = {i18n::N_("The crossfader does not touch this channel"), i18n::N_("This channel is on the crossfader's A side"),
                                                i18n::N_("This channel is on the crossfader's B side")};
    const float pw = std::min(96.0f, cx - x0 - 30);
    paramPills(a, t, deckParam(0, DXfSide), ImVec2(std::round(ax - pw * 0.5f + 8), y), pw, 20, kAssign, 3, kAssignHints);
    paramPills(a, t, deckParam(1, DXfSide), ImVec2(std::round(bx - pw * 0.5f - 8), y), pw, 20, kAssign, 3, kAssignHints);
    y += 28;
    crossfader(a, t, ImVec2(x0, y), ImVec2(x1, y + 30));
    y += 36;
    // curve, cut, reverse
    caps(dl, a, ImVec2(x0, y + 5), P.textDim, i18n::tr("CURVE"));
    ImGui::SetCursorScreenPos(ImVec2(x0 + 46, y + 1));
    const float sw = W - 46 - 2 * 44 - 12;
    pui::hslider(a, t, PXfCurve, "", sw);
    toggleParam(a, t, PXfCut, ImVec2(x1 - 2 * 44 - 4, y), ImVec2(44, 24), i18n::N_("CUT"),
                i18n::N_("Cut: the crossfader opens the other side at once, whatever the curve (scratching)"));
    toggleParam(a, t, PXfReverse, ImVec2(x1 - 44, y), ImVec2(44, 24), i18n::N_("REV"), i18n::N_("Reverse the crossfader (B left, A right)"));
    y += 30;
    // where synced decks take their tempo
    caps(dl, a, ImVec2(x0, y + 5), P.textDim, i18n::tr("SYNC TO"));
    static const char* const kSync[2] = {i18n::N_("Project"), i18n::N_("Other deck")};
    static const char* const kSyncHints[2] = {i18n::N_("Synced decks follow the project's tempo and beat"),
                                              i18n::N_("A synced deck follows the other deck (when both are synced, B follows A and A the project)")};
    paramPills(a, t, PSyncTo, ImVec2(x0 + 66, y), W - 66, 22, kSync, 2, kSyncHints);
    y += 28;
  }
  // channel fader curve
  caps(dl, a, ImVec2(x0, y + 5), P.textDim, i18n::tr("FADERS"));
  static const char* const kCurves[3] = {i18n::N_("Soft"), i18n::N_("Linear"), i18n::N_("Sharp")};
  static const char* const kCurveHints[3] = {i18n::N_("Soft start: fine control at the bottom"), i18n::N_("Straight"),
                                             i18n::N_("Full level a sixth of the way up: fast cuts")};
  if (two) paramPills(a, t, PFaderCurve, ImVec2(x0 + 66, y), W - 66, 22, kCurves, 3, kCurveHints);
  else paramPills(a, t, PFaderCurve, ImVec2(x0, y + 18), W, 22, kCurves, 3, kCurveHints);
}

}  // namespace gl::packs::platter::pv
