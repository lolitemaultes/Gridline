// Gridline — "plasma" pack: the MIX, MATRIX and GLOBAL pages (y 68..388).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

// ================================================================== MIX
constexpr float kMixHeadY = 70, kMixColW = 110;

enum GainLaw { LawLevel, LawBus, LawMaster };
double lawNorm(int law, double db) {
  const double g = std::pow(10.0, db / 20.0);
  switch (law) {
    case LawBus: return std::sqrt(g / 4.0);
    case LawMaster: return std::sqrt(g / 1.41253754);
    default: return std::sqrt(g);
  }
}
// Level fader with a dB scale on its right.
void levelFader(Ctx& c, int k, float x, float y, float h, int law) {
  vfader(c, k, x, y, 18, h);
  const float knobL = 7;
  static const int lvl[] = {0, -6, -12, -24, -48};
  static const int bus[] = {12, 6, 0, -12, -36};
  static const int mst[] = {3, 0, -6, -12, -24, -48};
  const int* t = law == LawBus ? bus : law == LawMaster ? mst : lvl;
  const int n = law == LawMaster ? 6 : 5;
  for (int i = 0; i < n; ++i) {
    const double nv = lawNorm(law, t[i]);
    if (nv > 1.0) continue;
    const float ty = y + h - knobL * 0.5f - static_cast<float>(nv) * (h - knobL);
    hline(c, x + 21, x + 25, ty, c.P.textFaint);
    char b[8];
    std::snprintf(b, sizeof b, "%d", t[i]);
    text(c, x + 28, ty - 5, b, c.P.textFaint, 7.5f, true);
  }
}

void destDrop(Ctx& c, int r, float x, float y, float w) {
  const int d = vali(c, routeP(r, R_Dest));
  const bool isFilter = r == RtF1 || r == RtF2;
  const char* names[] = {isFilter ? (r == RtF1 ? "FILTER 2" : "FILTER 1") : "FILTER", "MAIN", "DIRECT", "OFF"};
  dropField(c, "dest", x, y, w, 18, names[std::clamp(d, 0, 3)], [&] {
    for (int i = 0; i < RouteDestCount; ++i) {
      if (r == RtF2 && i == RouteFilter) continue;  // filter 2 can not feed back into filter 1
      if (ImGui::MenuItem(names[i], nullptr, i == d)) setv(c, routeP(r, R_Dest), i);
    }
  });
  if (ImGui::IsItemHovered()) c.a.setHint(isFilter ? "Output of the filter: into the other filter (serial), main, direct out or off" : "Output: into the filters, main, direct out or off");
}

void mixSource(Ctx& c, int col, const char* name, int enable, int r, int level, int pan) {
  const float x0 = col * (kMixColW + 0.f), w = kMixColW;
  ImGui::PushID(col + 3000);
  moduleFrame(c, x0, kPageY, w, kLowerY - kPageY, kMixHeadY + 20 - kPageY, name, enable);
  destDrop(c, r, x0 + 8, kMixHeadY + 26, w - 16);
  hline(c, x0 + 8, x0 + w - 8, kMixHeadY + 106, c.P.line);
  const bool toFilter = vali(c, routeP(r, R_Dest)) == RouteFilter && r != RtF1 && r != RtF2;
  if (toFilter) {
    knob(c, routeP(r, R_Balance), x0 + w * 0.5f, kMixHeadY + 70, 14, nullptr, KnobNoLabel | KnobBipolar);
    cellLabel(c, x0 + w * 0.5f - 28, kMixHeadY + 78, "1", 12);
    cellLabel(c, x0 + w * 0.5f + 28, kMixHeadY + 78, "2", 12);
    cellLabel(c, x0 + w * 0.5f, kMixHeadY + 89, "BALANCE", 90);
  } else if (r == RtF1 || r == RtF2) {
    // small response of the filter
    const int f = r == RtF1 ? 0 : 1;
    const float gx = x0 + 8, gy = kMixHeadY + 50, gw = w - 16, gh = 48;
    displayWellPublic(c, gx, gy, gw, gh);
    constexpr int N = 48;
    float hz[N], mag[N];
    for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
    Filter::responseCurve(vali(c, filtP(f, F_Type)), 48000, cutoffHz(val(c, filtP(f, F_Freq))), val(c, filtP(f, F_Res)), val(c, filtP(f, F_Var)), hz, mag, N);
    std::vector<ImVec2> pts(N);
    for (int i = 0; i < N; ++i) {
      const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
      pts[static_cast<size_t>(i)] = c.p(gx + gw * i / (N - 1), gy + gh - std::clamp((db + 36.f) / 60.f, 0.f, 1.f) * gh);
    }
    c.dl->PushClipRect(c.p(gx, gy), c.p(gx + gw, gy + gh), true);
    c.dl->AddPolyline(pts.data(), N, c.P.accent, 0, std::max(1.3f, c.s(1.4f)));
    c.dl->PopClipRect();
  }
  // bus sends
  knob(c, routeP(r, R_Bus1), x0 + 30, kMixHeadY + 132, 14, nullptr, KnobNoLabel);
  knob(c, routeP(r, R_Bus2), x0 + 80, kMixHeadY + 132, 14, nullptr, KnobNoLabel);
  cellLabel(c, x0 + 30, kMixHeadY + 150.5f, "1", 12);
  cellLabel(c, x0 + 55, kMixHeadY + 150.5f, "BUS", 30);
  cellLabel(c, x0 + 80, kMixHeadY + 150.5f, "2", 12);
  hline(c, x0 + 8, x0 + w - 8, kMixHeadY + 166, c.P.line);
  // pan + level
  if (pan >= 0) knob(c, pan, x0 + 26, kMixHeadY + 198, 14, "PAN", KnobBipolar);
  if (r == RtF1 || r == RtF2) knob(c, filtP(r == RtF1 ? 0 : 1, F_Wet), x0 + 26, kMixHeadY + 258, 14, "MIX");
  levelFader(c, level, x0 + 56, kMixHeadY + 176, 128, (r == RtF1 || r == RtF2) ? LawBus : LawLevel);
  if (enable >= 0 && val(c, enable) < 0.5)
    c.dl->AddRectFilled(c.p(x0 + kGutter + 1, kMixHeadY + 20), c.p(x0 + w - kGutter - 1, kLowerY - kGutter - 1), theme::withAlpha(c.P.frame, 0.4f), c.s(2.0f),
                        ImDrawFlags_RoundCornersBottom);
  ImGui::PopID();
}

void mixBus(Ctx& c, int col, const char* name, int destK, int vol, int law, float w = kMixColW) {
  const float x0 = col * kMixColW;
  ImGui::PushID(col + 3100);
  moduleFrame(c, x0, kPageY, w, kLowerY - kPageY, kMixHeadY + 20 - kPageY, name, -1, false, c.P.panelHi, true);
  if (destK >= 0) choiceBox(c, destK, x0 + 8, kMixHeadY + 26, w - 16, 18);
  // live meter for the main output
  if (vol == G_MasterVol && c.hasLive) {
    const float lv = std::clamp(std::max(c.live.outL, c.live.outR), 0.f, 1.5f);
    const float db = lv > 1e-5f ? 20.f * std::log10(lv) : -90.f;
    const float n = std::clamp((db + 48.f) / 51.f, 0.f, 1.f);
    wellRect(c, x0 + w * 0.5f - 34, kMixHeadY + 176, 8, 128, 2);
    if (n > 0) c.dl->AddRectFilled(c.p(x0 + w * 0.5f - 32, kMixHeadY + 302 - n * 124), c.p(x0 + w * 0.5f - 28, kMixHeadY + 302), c.P.good);
  }
  levelFader(c, vol, x0 + w * 0.5f - 18, kMixHeadY + 176, 128, law);
  ImGui::PopID();
}

}  // namespace

void mixPage(Ctx& c) {
  mixSource(c, 0, "SUB", subP(S_Enable), RtSub, subP(S_Level), subP(S_Pan));
  for (int o = 0; o < kOscs; ++o)
    mixSource(c, 1 + o, o == 0 ? "OSC A" : o == 1 ? "OSC B" : "OSC C", oscP(o, O_Enable), o, oscP(o, O_Level), oscP(o, O_Pan));
  mixSource(c, 4, "NOISE", noiseP(N_Enable), RtNoise, noiseP(N_Level), noiseP(N_Pan));
  mixSource(c, 5, "FILTER 1", filtP(0, F_Enable), RtF1, filtP(0, F_Level), filtP(0, F_Stereo));
  mixSource(c, 6, "FILTER 2", filtP(1, F_Enable), RtF2, filtP(1, F_Level), filtP(1, F_Stereo));
  mixBus(c, 7, "BUS 1", G_Bus1Dest, G_Bus1Vol, LawBus);
  mixBus(c, 8, "BUS 2", G_Bus2Dest, G_Bus2Vol, LawBus);
  mixBus(c, 9, "MAIN", -1, G_MasterVol, LawMaster);
  {
    // the main column also holds the voice amp (ENV 1 depth) and the tuning
    knob(c, G_Amp, 9 * kMixColW + 30, kMixHeadY + 62, 14, "AMP");
    knob(c, G_MasterTune, 9 * kMixColW + 80, kMixHeadY + 62, 14, "TUNE", KnobBipolar);
  }
  mixBus(c, 10, "DIRECT", -1, G_DirectVol, LawBus, kW - 10 * kMixColW);
}

// ================================================================== MATRIX
namespace {
constexpr float kMxHeadY = 69, kMxRowH = 35;
struct MxCol {
  const char* name;
  float x, w;
};
const MxCol kMx[] = {{"", 0, 30},           {"SOURCE", 30, 145},  {"CRV", 175, 34},     {"AMOUNT", 209, 241}, {"POL", 450, 37},
                     {"DESTINATION", 487, 145}, {"OUT", 632, 35}, {"AUX SOURCE", 667, 145}, {"INV", 812, 36}, {"CRV", 848, 34},
                     {"OUTPUT", 882, 236},  {"", 1118, 72}};

void curveGlyph(const Ctx& c, float x, float y, float w, float h, double bend, const Curve* cv, ImU32 col) {
  panelRect(c, x, y, w, h, c.P.well, 2);
  ImVec2 prev;
  for (int i = 0; i <= 16; ++i) {
    const float t = i / 16.f;
    const float v = cv && !cv->empty() ? static_cast<float>(curveAt(*cv, t)) : static_cast<float>(bendCurve(t, bend));
    const ImVec2 q = c.p(x + 3 + t * (w - 6), y + h - 3 - std::clamp(v, 0.f, 1.f) * (h - 6));
    if (i) c.dl->AddLine(prev, q, col, std::max(1.2f, c.s(1.3f)));
    prev = q;
  }
}

// Curve popup: point editor of the slot's source (or aux) curve + its bend and smoothing.
void curvePopup(Ctx& c, int s, bool aux) {
  Curve& cv = aux ? c.m.auxCurve[static_cast<size_t>(s)] : c.m.modCurve[static_cast<size_t>(s)];
  ImGui::TextDisabled("%s", aux ? i18n::tr("Aux source curve") : i18n::tr("Source curve"));
  const ImVec2 o = ImGui::GetCursorScreenPos();
  skin::Form F2 = c.F;
  F2.o = o;
  Ctx cc{c.a, c.t, F2, ImGui::GetWindowDrawList(), c.st, c.m, c.u, c.P, c.live, c.hasLive};
  displayWellPublic(cc, 0, 0, 220, 140);
  gridLines(cc, 0, 0, 220, 140, 4, 4, false);
  Curve edit = cv.empty() ? Curve{} : cv;
  if (edit.empty()) {
    const double b = val(c, modP(s, aux ? M_AuxCurve : M_Curve));
    for (int i = 0; i <= 4; ++i) edit.push_back({i / 4.f, static_cast<float>(bendCurve(i / 4.0, b)), 0});
  }
  static int sel = -1;
  if (curveEditor(cc, aux ? "auxcv" : "modcv", edit, 0, 0, 220, 140, 4, 4, sel, true, c.P.mod)) {
    cv = edit;
    commit(c, "Edit matrix curve", 2000 + s);
  }
  ImGui::Dummy(F2.sz(220, 146));
  if (ImGui::Button(i18n::tr("Straight"))) {
    cv.clear();
    commit(c, "Reset matrix curve");
    setv(c, modP(s, aux ? M_AuxCurve : M_Curve), 0);
  }
  ImGui::Separator();
  paramRows(c, {modP(s, aux ? M_AuxCurve : M_Curve), modP(s, M_Rise), modP(s, M_Fall)});
}

void sourceField(Ctx& c, int k, float x, float y, float w, float h, int slot) {
  const int src = vali(c, k);
  const std::string nm = src > SrcNone ? sourceLabel(src) : "\xE2\x80\x94";
  dropField(c, "src", x, y, w, h, nm.c_str(), [&] {
    int out = -1;
    if (sourceMenuItems(c, out)) setv(c, k, out < 0 ? 0 : out);
  });
  // accept a source dragged from a tab / macro
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int sv = 0;
      std::memcpy(&sv, p->Data, sizeof(int));
      setv(c, k, sv);
    }
    ImGui::EndDragDropTarget();
  }
  (void)slot;
}

void matrixRow(Ctx& c, int s, float y) {
  ImGui::PushID(s + 4000);
  const bool used = vali(c, modP(s, M_Source)) > SrcNone || vali(c, modP(s, M_Dest)) > 0;
  const bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
  panelRect(c, kGutter + 1, y, kW - 2 * kGutter - 2, kMxRowH - 1, (s & 1) ? c.P.panel : c.P.panelLo);
  const float cy = y + kMxRowH * 0.5f;
  // slot number + drag grip
  char num[16];
  std::snprintf(num, sizeof num, "%d", s + 1);
  textC(c, 15, cy - 5, num, c.P.textFaint, 8, true);
  const float fh = 20, fy = cy - fh * 0.5f;
  sourceField(c, modP(s, M_Source), kMx[1].x + 4, fy, kMx[1].w - 8, fh, s);
  // source curve
  {
    ImGui::SetCursorScreenPos(c.p(kMx[2].x + 4, fy));
    if (ImGui::InvisibleButton("##crv", c.F.sz(26, fh))) ImGui::OpenPopup("##crvpop");
    curveGlyph(c, kMx[2].x + 4, fy, 26, fh, val(c, modP(s, M_Curve)), &c.m.modCurve[static_cast<size_t>(s)], ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint("Source curve and smoothing (click)");
    if (ImGui::BeginPopup("##crvpop")) {
      curvePopup(c, s, false);
      ImGui::EndPopup();
    }
  }
  // amount (bipolar slider)
  hfader(c, modP(s, M_Amount), kMx[3].x + 8, fy + 2, kMx[3].w - 16, fh - 4);
  // polarity
  {
    const bool bip = val(c, modP(s, M_Bipolar)) >= 0.5;
    ImGui::SetCursorScreenPos(c.p(kMx[4].x + 6, fy));
    if (ImGui::InvisibleButton("##pol", c.F.sz(26, fh))) setv(c, modP(s, M_Bipolar), bip ? 0 : 1);
    const ImVec2 q = c.p(kMx[4].x + 19, cy);
    const ImU32 col = ImGui::IsItemHovered() ? c.P.text : c.P.mod;
    if (bip) {
      c.dl->AddLine(ImVec2(q.x - c.s(7), q.y), ImVec2(q.x + c.s(7), q.y), col, std::max(1.3f, c.s(1.5f)));
      c.dl->AddLine(ImVec2(q.x, q.y - c.s(6)), ImVec2(q.x, q.y + c.s(6)), col, std::max(1.3f, c.s(1.5f)));
    } else {
      c.dl->PathArcTo(q, c.s(6), -2.8f, 0.6f, 14);
      c.dl->PathStroke(col, 0, std::max(1.3f, c.s(1.5f)));
    }
    if (ImGui::IsItemHovered()) c.a.setHint(bip ? "Bipolar: the source swings both ways around the value (click: unipolar)" : "Unipolar (click: bipolar)");
  }
  // destination
  {
    const int d = vali(c, modP(s, M_Dest));
    const std::string nm = d > 0 ? paramLongName(d - 1) : "\xE2\x80\x94";
    dropField(c, "dst", kMx[5].x + 4, fy, kMx[5].w - 8, fh, nm.c_str(), [&] {
      int out = -2;
      if (destMenuItems(c, out)) setv(c, modP(s, M_Dest), out < 0 ? 0 : out + 1);
    });
  }
  // live output
  {
    wellRect(c, kMx[6].x + 4, fy, kMx[6].w - 8, fh, 2);
    if (c.hasLive && used && !byp) {
      const float v = std::clamp(c.live.mod[s], -1.f, 1.f);
      const float mid = kMx[6].x + kMx[6].w * 0.5f;
      c.dl->AddRectFilled(c.p(std::min(mid, mid + v * 12), fy + 3), c.p(std::max(mid, mid + v * 12), fy + fh - 3), c.P.mod);
    }
  }
  sourceField(c, modP(s, M_Aux), kMx[7].x + 4, fy, kMx[7].w - 8, fh, s);
  // aux invert
  {
    const bool inv = val(c, modP(s, M_AuxInv)) >= 0.5;
    ImGui::SetCursorScreenPos(c.p(kMx[8].x + 6, fy));
    if (ImGui::InvisibleButton("##inv", c.F.sz(24, fh))) setv(c, modP(s, M_AuxInv), inv ? 0 : 1);
    const ImVec2 q = c.p(kMx[8].x + 18, cy);
    const ImU32 col = inv ? c.P.mod : (ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
    iconArrow(c.dl, ImVec2(q.x - c.s(3), q.y - c.s(2)), c.s(7), 2, col);
    iconArrow(c.dl, ImVec2(q.x + c.s(3), q.y + c.s(2)), c.s(7), 3, col);
    if (ImGui::IsItemHovered()) c.a.setHint("Invert the aux source (the aux scales the amount)");
  }
  {
    ImGui::SetCursorScreenPos(c.p(kMx[9].x + 4, fy));
    if (ImGui::InvisibleButton("##acrv", c.F.sz(26, fh))) ImGui::OpenPopup("##acrvpop");
    curveGlyph(c, kMx[9].x + 4, fy, 26, fh, val(c, modP(s, M_AuxCurve)), &c.m.auxCurve[static_cast<size_t>(s)], ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
    if (ImGui::BeginPopup("##acrvpop")) {
      curvePopup(c, s, true);
      ImGui::EndPopup();
    }
  }
  hfader(c, modP(s, M_Out), kMx[10].x + 8, fy + 2, kMx[10].w - 16, fh - 4);
  // bypass + remove
  {
    const float bx = kMx[11].x + 8;
    ImGui::SetCursorScreenPos(c.p(bx, fy));
    if (ImGui::InvisibleButton("##byp", c.F.sz(24, fh))) setv(c, modP(s, M_Bypass), byp ? 0 : 1);
    iconPower(c.dl, c.p(bx + 12, cy + 1), c.s(5), byp ? c.P.textFaint : (used ? c.P.accent : c.P.textDim));
    if (ImGui::IsItemHovered()) c.a.setHint(byp ? "Bypassed (click: on)" : "Bypass this slot");
    ImGui::SetCursorScreenPos(c.p(bx + 30, fy));
    if (ImGui::InvisibleButton("##del", c.F.sz(24, fh)))
      for (int q = 0; q < M_Count; ++q)
        if (const ParamDesc* d = pdesc(modP(s, q))) setv(c, modP(s, q), d->def);
    iconClose(c.dl, c.p(bx + 42, cy), c.s(10), ImGui::IsItemHovered() ? c.P.danger : c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint("Clear this slot");
  }
  if (byp && used) c.dl->AddRectFilled(c.p(30, y), c.p(kMx[11].x, y + kMxRowH - 1), theme::withAlpha(c.P.frame, 0.45f));
  ImGui::PopID();
}
}  // namespace

void matrixPage(Ctx& c) {
  const bool big = c.u.mxExpanded;
  const float y1 = big ? kStripY : kLowerY;
  panelRect(c, 0, kPageY, kW, y1 - kPageY, c.P.frame);
  // the table sits in one frame; the column titles form its header strip
  moduleFrame(c, 0, kPageY, kW, y1 - kPageY, kMxHeadY + 27 - kPageY, nullptr, -1, false, c.P.panelLo);
  for (const auto& col : kMx)
    if (*col.name) textC(c, col.x + col.w * 0.5f, kMxHeadY + 8, col.name, c.P.label, 8.5f, true);
  for (size_t i = 1; i < std::size(kMx); ++i) vline(c, kMx[i].x, kMxHeadY + 4, kMxHeadY + 23, c.P.line);
  ImGui::SetCursorScreenPos(c.p(4, kMxHeadY + 3));
  if (ImGui::InvisibleButton("##exp", c.F.sz(22, 21))) c.u.mxExpanded = !c.u.mxExpanded;
  {
    const ImVec2 q = c.p(15, kMxHeadY + 13.5f);
    const ImU32 col = ImGui::IsItemHovered() ? c.P.text : c.P.textDim;
    const float r = c.s(6);
    for (int i = 0; i < 4; ++i) {
      const float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
      c.dl->AddLine(ImVec2(q.x + sx * r, q.y + sy * r), ImVec2(q.x + sx * r * 0.35f, q.y + sy * r), col, std::max(1.2f, c.s(1.3f)));
      c.dl->AddLine(ImVec2(q.x + sx * r, q.y + sy * r), ImVec2(q.x + sx * r, q.y + sy * r * 0.35f), col, std::max(1.2f, c.s(1.3f)));
    }
    if (ImGui::IsItemHovered()) c.a.setHint(big ? "Smaller matrix (show the envelopes and LFOs)" : "Bigger matrix");
  }
  moreButton(c, "mxmore", kW - 24, kMxHeadY + 5, {}, [&] {
    if (ImGui::MenuItem(i18n::tr("Clear every slot")))
      for (int s = 0; s < kModSlots; ++s)
        for (int q = 0; q < M_Count; ++q)
          if (const ParamDesc* d = pdesc(modP(s, q))) setv(c, modP(s, q), d->def);
    if (ImGui::MenuItem(i18n::tr("Sort: used slots first"))) {
      // move used slots up (keeps their order)
      std::vector<std::vector<double>> used, empty;
      for (int s = 0; s < kModSlots; ++s) {
        std::vector<double> row;
        for (int q = 0; q < M_Count; ++q) row.push_back(val(c, modP(s, q)));
        const bool u = row[M_Source] > 0 || row[M_Dest] > 0;
        (u ? used : empty).push_back(row);
      }
      used.insert(used.end(), empty.begin(), empty.end());
      for (int s = 0; s < kModSlots; ++s)
        for (int q = 0; q < M_Count; ++q)
          if (val(c, modP(s, q)) != used[static_cast<size_t>(s)][static_cast<size_t>(q)]) setv(c, modP(s, q), used[static_cast<size_t>(s)][static_cast<size_t>(q)]);
    }
  }, "Matrix menu");
  // rows (scroll)
  const float top = kMxHeadY + 28, h = y1 - kGutter - 1 - top;
  const float total = kModSlots * kMxRowH;
  float& sc = c.u.mxScroll;
  const ImVec2 a = c.p(0, top), b = c.p(kW, top + h);
  if (ImGui::IsMouseHoveringRect(a, b) && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsAnyItemActive())
    sc -= ImGui::GetIO().MouseWheel * kMxRowH;
  sc = std::clamp(sc, 0.f, std::max(0.f, total - h));
  c.dl->PushClipRect(a, b, true);
  ImGui::PushClipRect(a, b, true);
  const int firstRow = static_cast<int>(sc / kMxRowH);
  for (int s = firstRow; s < kModSlots; ++s) {
    const float y = top + s * kMxRowH - sc;
    if (y > y1) break;
    matrixRow(c, s, y);
  }
  ImGui::PopClipRect();
  c.dl->PopClipRect();
  // scroll bar
  {
    const float th = h * h / total, ty = top + (h - th) * (sc / std::max(1.f, total - h));
    c.dl->AddRectFilled(c.p(kW - 8, ty), c.p(kW - 5, ty + th), c.P.lineHi, c.s(1));
  }
}

// ================================================================== GLOBAL
namespace {
void sectionBox(Ctx& c, float x, float y, float w, float h, const char* title) { moduleFrame(c, x, y, w, h, 22, title, -1, false); }
void twoWay(Ctx& c, const char* id, float x, float y, const char* a, const char* b, bool& v) {
  ImGui::PushID(id);
  if (button(c, "a", x, y, 76, 18, a, !v, 8)) v = false;
  if (button(c, "b", x + 80, y, 76, 18, b, v, 8)) v = true;
  ImGui::PopID();
}
}  // namespace

void globalPage(Ctx& c) {
  // left: editor and MPE settings
  {
    const float x = 0, y = kPageY, w = 414, h = kLowerY - kPageY;
    sectionBox(c, x, y, w, h, "EDITOR");
    float ry = y + 30;
    auto row = [&](const char* label, const char* a, const char* b, bool& v, const char* hint) {
      text(c, x + 14, ry + 4, label, c.P.text, 9, false);
      ImGui::PushID(label);
      twoWay(c, "tw", x + w - 172, ry, a, b, v);
      if (ImGui::IsItemHovered()) c.a.setHint(hint);
      ImGui::PopID();
      ry += 24;
    };
    row("Value bubbles", "HIDE", "SHOW", c.u.prefBubbles, "Value bubble while you drag or hover a control");
    row("Live modulation", "HIDE", "SHOW", c.u.prefLive, "Moving modulation dots and playheads");
    row("Waveform view", "2D", "3D", c.u.wt3d, "Oscillator display: one cycle (2D) or the whole table (3D)");
    ry += 6;
    text(c, x + 14, ry, "MPE", c.P.label, 9.5f, true);
    hline(c, x + 44, x + w - 14, ry + 6, c.P.line);
    ry += 18;
    checkBox(c, G_MpeOn, x + 14, ry, "MPE on", 9);
    ry += 20;
    text(c, x + 14, ry + 3, "Per-note bend range", c.P.text, 9, false);
    valueBox(c, G_MpeBend, x + w - 172, ry, 156, 17, "");
    ry += 22;
    checkBox(c, G_MpeYBipolar, x + 14, ry, "Expression Y is bipolar", 9);
    ry += 20;
    auto flag = [&](const char* id, bool& f, const char* label) {
      ImGui::PushID(id);
      ImGui::SetCursorScreenPos(c.p(x + 14, ry - 1));
      if (ImGui::InvisibleButton("##f", c.F.sz(w - 28, 13))) {
        f = !f;
        commit(c, "MPE routing");
      }
      const ImVec2 a = c.p(x + 14, ry + 0.5f), b = c.p(x + 22.5f, ry + 9);
      if (f) skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.2f), c.P.accent, c.s(2));
      else c.dl->AddRectFilled(a, b, c.P.well, c.s(2)), c.dl->AddRect(a, b, c.P.lineHi, c.s(2));
      text(c, x + 27, ry, label, f ? c.P.text : c.P.textDim, 9, false);
      ImGui::PopID();
      ry += 18;
    };
    flag("m1", c.m.mpeBendToX, "Per-note bend also drives expression X");
    flag("m2", c.m.mpeXYZtoMacros, "Expression X / Y / Z drive macros 1-3");
    flag("m3", c.m.mpeYZtoMacros, "Expression Y / Z drive macros 1-2");
    flag("m4", c.m.mpeYtoModWheel, "Expression Y drives the mod wheel");
  }
  // right: voice control, quality, tuning, scaling
  const float rx = 414;
  {
    const float x = rx, y = kPageY, w = 396, h = kLowerY - kPageY;
    sectionBox(c, x, y, w, h, "VOICE CONTROL");
    // oscillators it applies to
    const char* on[5] = {"S", "A", "B", "C", "N"};
    const int ok[5] = {G_VcS, G_VcA, G_VcB, G_VcC, G_VcN};
    text(c, x + 196, y + 34, "OSC", c.P.textDim, 8.5f, true);
    for (int i = 0; i < 5; ++i) {
      ImGui::PushID(i + 5000);
      toggleBtn(c, ok[i], x + 222 + i * 30.f, y + 29, 26, 18, on[i], 9);
      ImGui::PopID();
    }
    text(c, x + 14, y + 34, "STEPS", c.P.textDim, 8.5f, true);
    stepper(c, G_VcSteps, x + 52, y + 29, 44, 18);
    // random + sequence rows
    text(c, x + 40, y + 60, "RANDOM", c.P.textDim, 8.5f, true);
    text(c, x + 128, y + 60, "SEQ", c.P.textDim, 8.5f, true);
    const char* rows[6] = {"PAN", "DETUNE", "CUTOFF", "ENVS", "MOD 1", "MOD 2"};
    const int rnd[4] = {G_RndPan, G_RndDetune, G_RndCutoff, G_RndEnv};
    const float gx = x + 176, gw = 196, gy = y + 76, rh = 22;
    for (int i = 0; i < 8; ++i) {
      char b[4];
      std::snprintf(b, sizeof b, "%d", i + 1);
      textC(c, gx + gw * (i + 0.5f) / 8, gy - 14, b, i < vali(c, G_VcSteps) ? c.P.text : c.P.textFaint, 8.5f, true);
    }
    static int seqRow = 0;
    for (int r = 0; r < 6; ++r) {
      const float ry = gy + r * rh;
      if (r < 4) valueBox(c, rnd[r], x + 14, ry, 92, rh - 4, "");
      ImGui::PushID(r + 5100);
      if (button(c, "row", x + 112, ry, 58, rh - 4, rows[r], seqRow == r, 8)) seqRow = r;
      ImGui::PopID();
    }
    // the step grid edits the selected row: bars from the centre (bipolar)
    wellRect(c, gx, gy, gw, rh * 6 - 4, 2);
    for (int i = 1; i < 8; ++i) vline(c, gx + gw * i / 8, gy, gy + rh * 6 - 4, c.P.grid);
    hline(c, gx, gx + gw, gy + (rh * 6 - 4) * 0.5f, c.P.gridStrong);
    auto field = [](VoiceStep& v, int r) -> float& {
      switch (r) {
        case 0: return v.pan;
        case 1: return v.detune;
        case 2: return v.cutoff;
        case 3: return v.env;
        case 4: return v.mod1;
        default: return v.mod2;
      }
    };
    const float gh = rh * 6 - 4;
    for (int i = 0; i < 8; ++i) {
      const float v = field(c.m.voiceSeq[static_cast<size_t>(i)], seqRow);
      const float cx0 = gx + gw * i / 8 + 3, cx1 = gx + gw * (i + 1) / 8 - 3, mid = gy + gh * 0.5f;
      const bool live = i < vali(c, G_VcSteps);
      c.dl->AddRectFilled(c.p(cx0, std::min(mid, mid - v * gh * 0.5f)), c.p(cx1, std::max(mid, mid - v * gh * 0.5f)),
                          theme::withAlpha(c.P.accent, live ? 0.75f : 0.25f), c.s(1));
      hline(c, cx0, cx1, mid - v * gh * 0.5f, live ? c.P.good : c.P.textFaint);
    }
    ImGui::SetCursorScreenPos(c.p(gx, gy));
    ImGui::InvisibleButton("##seq", c.F.sz(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive() || (ImGui::IsItemHovered() && ImGui::IsMouseDown(1))) {
      const ImVec2 m = ImGui::GetIO().MousePos, a = c.p(gx, gy), b = c.p(gx + gw, gy + gh);
      const int i = std::clamp(static_cast<int>((m.x - a.x) / (b.x - a.x) * 8), 0, 7);
      const float v = ImGui::IsMouseDown(1) ? 0.f : std::clamp(1.f - 2.f * (m.y - a.y) / (b.y - a.y), -1.f, 1.f);
      float& f = field(c.m.voiceSeq[static_cast<size_t>(i)], seqRow);
      if (f != v) {
        f = v;
        commit(c, "Edit voice sequence", 5200 + seqRow);
      }
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Per-voice sequence (each new note takes the next step): drag to draw, right-drag to clear");
    // scaling
    const float sy = y + h - 32;
    hline(c, x + 14, x + w - 14, sy - 7.5f, c.P.line);
    text(c, x + 14, sy + 5, "SCALING", c.P.label, 9, true);
    valueBox(c, G_EnvScale, x + 90, sy, 120, 20, "ENVS");
    valueBox(c, G_LfoScale, x + 216, sy, 110, 20, "LFOS");
    toggleBtn(c, G_LfoScaleRate, x + 330, sy, 42, 20, "RATE", 8);
  }
  {
    const float x = rx + 396, y = kPageY, w = kW - x;
    sectionBox(c, x, y, w, 84, "QUALITY");
    choiceBox(c, G_Quality, x + 14, y + 26, 110, 20);
    checkBox(c, G_NoSmooth, x + 14, y + 54, "DISABLE SMOOTHING", 8.5f);
    if (ImGui::IsItemHovered()) c.a.setHint("Gain changes apply at once instead of gliding (sharper, may click)");
    const float ty = y + 84;
    sectionBox(c, x, ty, w, 84, "TUNING");
    valueBox(c, G_ConcertA, x + 14, ty + 26, 120, 20, "A =");
    const std::string tn = c.m.tuningName.empty() ? std::string("(12-TET)") : c.m.tuningName;
    dropField(c, "tun", x + 140, ty + 26, w - 154, 20, tn.c_str(), [&] {
      if (ImGui::MenuItem(i18n::tr("Load tuning (.tun)...")))
        openFile(c, i18n::tr("Load tuning"), {".tun"}, [](App& a, const PluginTarget& t, const std::string& p) {
          std::ifstream f(p, std::ios::binary);
          std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
          if (txt.size() > (1u << 20)) return;
          editState(a, t, "Load tuning", [&](PluginState&, Model& m) {
            m.tuning = txt;
            m.tuningName = fs_stem(p);
          });
        });
      if (ImGui::MenuItem(i18n::tr("12-TET (clear)"), nullptr, false, !c.m.tuning.empty())) {
        c.m.tuning.clear();
        c.m.tuningName.clear();
        commit(c, "Clear tuning");
      }
    });
    const float iy = ty + 84;
    sectionBox(c, x, iy, w, kLowerY - iy, "PRESET");
    char b[160];
    std::snprintf(b, sizeof b, "%s", c.m.presetName.empty() ? "Init" : c.m.presetName.c_str());
    text(c, x + 14, iy + 28, b, c.P.accent, 10, true);
    std::snprintf(b, sizeof b, "%d of %d matrix slots used", [&] {
      int n = 0;
      for (int s = 0; s < kModSlots; ++s) n += vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0;
      return n;
    }(), kModSlots);
    text(c, x + 14, iy + 48, b, c.P.textDim, 8.5f, false);
    int fx = 0;
    for (int s = 0; s < kFxSlots; ++s) fx += vali(c, fxP(s, X_Type)) > FxNone;
    std::snprintf(b, sizeof b, "%d effect modules", fx);
    text(c, x + 14, iy + 64, b, c.P.textDim, 8.5f, false);
  }
}

}  // namespace gl::packs::plasma::ui
