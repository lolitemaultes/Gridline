// Gridline — "utility" pack: Morph EQ (gl.bankeq) editor.
//
// Reference layout 565 × 240: band canvas group on the left (tool strip, per-
// band mode strip, 31-bar canvas, bank / curve selectors underneath), the
// motion wheels and the setup controls in the column on the right.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include "../../core/I18n.h"
#include "../../core/Params.h"
#include "../../app/DrawSmoothUi.h"
#include "utility_widgets.h"

namespace gl::packs::utility {


namespace {

const skin::Skin& bankEqSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3E484F, 0x46515A, 0x3F484D, 0xFFA830, 0x42474A, 0xB9BFC3);
    s.graphBg = skin::hex(0x262E33);
    s.graphGrid = skin::hex(0x313B41);
    s.graphGridStrong = skin::hex(0x44505A);
    s.graphCurve = skin::hex(0xFFC060);
    s.lcdBg = skin::hex(0x526061);
    s.lcdText = skin::hex(0xFFFFFF);
    s.select = skin::hex(0x49B5F5);
    s.accent2 = skin::hex(0x49B5F5);
    return s;
  }();
  return k;
}

struct BankUi {
  ImVec2 last{0, 0}, lineStart{0, 0};
  bool erasing = false, analysing = false;
  uint64_t gesture = 0;
};
std::map<uint64_t, BankUi> bankUis;

uint64_t gestureKey(const PluginTarget& t, uint64_t tag) {
  return tag ^ (t.key() << 4) ^ static_cast<uint64_t>(ImGui::GetFrameCount());
}

int ival(const PluginState& s, int k, int def = 0) { return static_cast<int>(std::lround(val(s, k, static_cast<float>(def)))); }

// Bank index the canvas edits (master = 8).
int editBank(const PluginState& s) { return val(s, BqEditMaster) >= 0.5f ? kBqMaster : std::clamp(ival(s, BqEditBank), 0, kBqBanks - 1); }

float& bandVal(PluginState& s, int bank, int target, int band) { return s.params[static_cast<size_t>(bqValue(bank, target, band))]; }

void drawToolIcon(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  float r = std::min(w, h) * 0.3f;
  if (i == 0) {
    dl->AddCircleFilled(c, r * 0.45f, col, 12);
  } else if (i == 1) {
    dl->AddLine(ImVec2(c.x - r, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.7f), col, 1.6f);
  } else {
    dl->PathArcTo(ImVec2(c.x, c.y + r * 0.8f), r * 1.1f, 3.14159265f * 1.15f, 3.14159265f * 1.85f, 12);
    dl->PathStroke(col, 0, 1.6f);
  }
}

}  // namespace

void editorBankEq(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  if (st.params.size() < static_cast<size_t>(kBqCount)) return;
  const skin::Skin& K = bankEqSkin();
  constexpr float kW = 565, kH = 240;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  BankUi& ui = bankUis[t.key()];
  ImGuiIO& io = ImGui::GetIO();
  const float fs = F.font(8.0f);
  const int target = std::clamp(ival(st, BqEditTarget), 0, kBqTargets - 1);
  const int bank = editBank(st);
  const int tool = std::clamp(ival(st, BqDrawMode, 2), 0, 2);
  static const char* targetNames[kBqTargets] = {"LEVEL", "PAN", "SEND"};

  // ---- canvas group ----
  skin::well(dl, F.p(4, 4), F.p(396, 236), K, 4 * F.s);
  {
    char cap[48];
    if (bank == kBqMaster) std::snprintf(cap, sizeof cap, "MASTER · %s", targetNames[target]);
    else std::snprintf(cap, sizeof cap, "BANK %d · %s", bank + 1, targetNames[target]);
    skin::text(dl, F.p(17, 28), K.textDim, cap, fs);
  }
  // Options menu.
  if (skin::button("##bqopt", F.p(231, 23), F.sz(38, 19), K, "...", false, fs)) ImGui::OpenPopup("##bqopt");
  if (ImGui::IsItemHovered()) a.setHint("Curve options: random, flat, interpolate banks 1 to 8, copy");
  if (ImGui::BeginPopup("##bqopt")) {
    PluginState ns = st;
    const char* label = nullptr;
    if (ImGui::MenuItem(i18n::tr("Random curve"))) {
      std::mt19937 rng(static_cast<uint32_t>(ImGui::GetFrameCount()) * 2654435761u);
      std::uniform_real_distribution<float> d(-1.f, 1.f);
      float v = 0;
      for (int b = 0; b < kBqBands; ++b) {
        v = std::clamp(v * 0.75f + d(rng) * 0.3f, -0.8f, 0.8f);
        bandVal(ns, bank, target, b) = 1.0f + v;
      }
      label = "random curve";
    }
    if (ImGui::MenuItem(i18n::tr("Flat curve"))) {
      for (int b = 0; b < kBqBands; ++b) bandVal(ns, bank, target, b) = 1.0f;
      label = "flatten curve";
    }
    if (ImGui::MenuItem(i18n::tr("Invert curve"))) {
      for (int b = 0; b < kBqBands; ++b) bandVal(ns, bank, target, b) = 2.0f - bandVal(ns, bank, target, b);
      label = "invert curve";
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Interpolate banks 1 to 8"))) {
      for (int k = 1; k < kBqBanks - 1; ++k)
        for (int tt = 0; tt < kBqTargets; ++tt)
          for (int b = 0; b < kBqBands; ++b) {
            float x = static_cast<float>(k) / (kBqBanks - 1);
            bandVal(ns, k, tt, b) = bandVal(ns, 0, tt, b) * (1 - x) + bandVal(ns, kBqBanks - 1, tt, b) * x;
          }
      label = "interpolate banks";
    }
    if (bank != kBqMaster && ImGui::MenuItem(i18n::tr("Copy this bank to all banks"))) {
      for (int k = 0; k < kBqBanks; ++k)
        for (int tt = 0; tt < kBqTargets; ++tt)
          for (int b = 0; b < kBqBands; ++b) bandVal(ns, k, tt, b) = bandVal(ns, bank, tt, b);
      label = "copy bank";
    }
    if (ImGui::MenuItem(i18n::tr("All bands on"))) {
      for (int tt = 0; tt < kBqTargets; ++tt)
        for (int b = 0; b < kBqBands; ++b) ns.params[static_cast<size_t>(bqMode(tt, b))] = BmOn;
      label = "bands on";
    }
    if (label) setPluginState(a, t, ns, label);
    ImGui::EndPopup();
  }
  skin::iconSel(a, t, BqDrawMode, F.p(271, 24), F.sz(48, 18), K, 3, drawToolIcon, true);
  // Analyse (hold): accumulate the input per band, then flatten on release.
  {
    UtilityFx* fx = liveFx(a, t);
    BandTelemetry* tel = fx ? fx->bandTelemetry() : nullptr;
    bool on = ui.analysing;
    skin::button("##bqana", F.p(322, 24), F.sz(66, 18), K, "ANALYSE", on, fs);
    if (ImGui::IsItemHovered()) a.setHint("Hold while audio plays: on release the curve flattens the measured spectrum");
    if (ImGui::IsItemActivated() && tel) {
      tel->reset.store(true);
      tel->capture.store(true);
      ui.analysing = true;
    }
    if (ImGui::IsItemDeactivated() && ui.analysing) {
      ui.analysing = false;
      if (tel) {
        tel->capture.store(false);
        if (tel->frames.load() > 0 && target != BtPan) {
          double rms[kBqBands], avg = 0;
          int cnt = 0;
          for (int b = 0; b < kBqBands; ++b) rms[b] = std::sqrt(std::max(0.0f, tel->energy[b].load()) / static_cast<double>(tel->frames.load()));
          for (int b = 4; b < kBqBands; ++b)
            if (bqBandHz(b) < a.engine.sampleRate() * 0.5) avg += rms[b], ++cnt;
          avg /= std::max(1, cnt);
          if (avg > 1e-7) {
            PluginState ns = st;
            for (int b = 0; b < kBqBands; ++b) {
              double g = b < 4 || rms[b] <= 0 ? 1.0 : std::min(4.0, avg / rms[b]);
              bandVal(ns, bank, target, b) = static_cast<float>(bqValueOfGain(g));
            }
            setPluginState(a, t, ns, "analyse curve");
          }
        }
      }
    }
  }

  // ---- per-band mode strip ----
  const ImVec2 m0 = F.p(17, 50), m1 = F.p(387, 60);
  const float cellW = (m1.x - m0.x) / kBqBands;
  ImGui::SetCursorScreenPos(m0);
  ImGui::InvisibleButton("##bqmodes", ImVec2(m1.x - m0.x, m1.y - m0.y));
  {
    bool hov = ImGui::IsItemHovered();
    int hb = hov ? std::clamp(static_cast<int>((io.MousePos.x - m0.x) / cellW), 0, kBqBands - 1) : 0;  // no mouse: -FLT_MAX
    for (int b = 0; b < kBqBands; ++b) {
      int mode = std::clamp(ival(st, bqMode(target, b)), 0, 2);
      ImVec2 c0(m0.x + b * cellW + 1, m0.y), c1(m0.x + (b + 1) * cellW - 1, m1.y);
      ImU32 col = mode == BmOff ? K.graphBg : mode == BmLocked ? K.select : theme::withAlpha(K.fill, 0.75f);
      dl->AddRectFilled(c0, c1, col, 1.5f);
      if (hov && b == hb) dl->AddRect(c0, c1, K.text, 1.5f);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      int mode = std::clamp(ival(st, bqMode(target, hb)), 0, 2);
      int next = ImGui::IsItemClicked(ImGuiMouseButton_Right) ? (mode == BmLocked ? BmOn : BmLocked) : (mode == BmOff ? BmOn : BmOff);
      a.doc.setParam(t.pid(bqMode(target, hb)), static_cast<float>(next), false);
    }
    if (hov) {
      static const char* mn[3] = {"on", "off", "locked"};
      char buf[128];
      std::snprintf(buf, sizeof buf, "Band %d mode: %s   (click: on / off · right-click: lock against morph, shift and mix)", hb + 1,
                    mn[std::clamp(ival(st, bqMode(target, hb)), 0, 2)]);
      a.setHint(buf);
    }
  }

  // ---- band canvas ----
  const ImVec2 g0 = F.p(17, 62), g1 = F.p(387, 200);
  const float gw = g1.x - g0.x, gh = g1.y - g0.y;
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##bqcanvas", ImVec2(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  skin::graphWell(dl, g0, g1, K);
  auto vY = [&](double v) { return g1.y - static_cast<float>(std::clamp(v, 0.0, 2.0) / 2.0) * gh; };
  auto yV = [&](float y) { return std::clamp(static_cast<double>(g1.y - y) / gh * 2.0, 0.0, 2.0); };
  auto bandAt = [&](float x) { return std::clamp(static_cast<int>((x - g0.x) / cellW), 0, kBqBands - 1); };
  // Grid: unity line and the 0 / ±6 dB gains of the level law.
  dl->AddLine(ImVec2(g0.x, vY(1.0)), ImVec2(g1.x, vY(1.0)), K.graphGridStrong);
  if (target != BtPan)
    for (double db : {-6.0, 6.0}) {
      float y = vY(bqValueOfGain(std::pow(10.0, db / 20.0)));
      dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), K.graphGrid);
    }
  for (int b = 0; b < kBqBands; b += 3) {
    float x = g0.x + (b + 0.5f) * cellW;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), theme::withAlpha(K.graphGrid, 0.6f));
  }
  // Bars of the edited bank.
  for (int b = 0; b < kBqBands; ++b) {
    double v = val(st, bqValue(bank, target, b), 1);
    float x0 = g0.x + b * cellW + 1.5f, x1 = g0.x + (b + 1) * cellW - 1.5f;
    float y = vY(v), yb = target == BtPan ? vY(1.0) : g1.y;
    bool off = ival(st, bqMode(target, b)) == BmOff;
    ImU32 col = off ? theme::withAlpha(K.text, 0.15f) : theme::withAlpha(K.fill, 0.85f);
    dl->AddRectFilled(ImVec2(x0, std::min(y, yb)), ImVec2(x1, std::max(y, yb) + (y == yb ? 1.0f : 0.0f)), col, 1.0f);
  }
  // Effective result (morph / shift / mix / master) as a line.
  {
    BankFrame fr;
    bankEffective(st.params.data(), val(st, BqShift), val(st, BqMix, 1), val(st, BqMorph), fr);
    ImVec2 pts[kBqBands];
    for (int b = 0; b < kBqBands; ++b) {
      double v = target == BtPan ? 1.0 + fr.pan[b] : bqValueOfGain(fr.gain[target][b]);
      pts[b] = ImVec2(g0.x + (b + 0.5f) * cellW, vY(v));
    }
    dl->AddPolyline(pts, kBqBands, theme::withAlpha(K.graphCurve, 0.9f), 0, 1.6f);
  }
  // Drawing.
  auto paint = [&](PluginState& ns, float xa, double va, float xb, double vb) {
    if (xb < xa) std::swap(xa, xb), std::swap(va, vb);
    int b0 = bandAt(xa), b1 = bandAt(xb);
    for (int b = b0; b <= b1; ++b) {
      float cx = g0.x + (b + 0.5f) * cellW;
      double tt = xb - xa > 1e-3f ? std::clamp((cx - xa) / (xb - xa), 0.0f, 1.0f) : 0.0;
      bandVal(ns, bank, target, b) = static_cast<float>(std::clamp(va + (vb - va) * tt, 0.0, 2.0));
    }
  };
  auto brush = [&](PluginState& ns, float x, double v) {  // smooth tool: bell-shaped brush
    double centre = (x - g0.x) / cellW - 0.5;
    for (int b = 0; b < kBqBands; ++b) {
      double d = (b - centre) / 1.6, w = std::exp(-d * d);
      if (w < 0.02) continue;
      float& cur = bandVal(ns, bank, target, b);
      cur = static_cast<float>(std::clamp(cur + (v - cur) * w, 0.0, 2.0));
    }
  };
  // J-99: the pencil (tool 0) goes through the shared smooth stroke: every band
  // between samples is filled, the pointer is filtered and the painted bands
  // get a light pass (Alt: raw). The line tool stays exact; the brush is
  // stamped along the path so fast moves leave no gaps. One undo step per stroke.
  const bool pencil = tool == 0 && act && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
  {
    float cur[kBqBands];
    for (int b = 0; b < kBqBands; ++b) cur[b] = static_cast<float>(std::clamp(val(st, bqValue(bank, target, b), 1), 0.0f, 2.0f));
    drawui::Spec spec;
    spec.surface = "bankeq";
    spec.lo = 0.0f;
    spec.hi = 2.0f;
    spec.p0 = g0;
    spec.p1 = g1;
    const double mx = (io.MousePos.x - g0.x) / gw;
    if (const std::vector<float>* res = drawui::stroke(a, ImGui::GetItemID(), pencil, mx, yV(io.MousePos.y), cur, kBqBands, spec)) {
      PluginState ns = st;
      for (int b = 0; b < kBqBands; ++b) bandVal(ns, bank, target, b) = (*res)[static_cast<size_t>(b)];
      if (ns.params != st.params) setPluginState(a, t, ns, "draw EQ bands", (0x42514550ull << 32) ^ (t.key() << 4));
    }
  }
  if (ImGui::IsItemActivated()) {
    ui.gesture = gestureKey(t, 0x42514555ull << 32);
    ui.erasing = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    ui.last = ui.lineStart = io.MousePos;
    if (tool == 2 || ui.erasing) {
      PluginState ns = st;
      double v = ui.erasing ? 1.0 : yV(io.MousePos.y);
      if (ui.erasing) paint(ns, io.MousePos.x, v, io.MousePos.x, v);
      else brush(ns, io.MousePos.x, v);
      setPluginState(a, t, ns, "draw EQ bands", ui.gesture);
    }
  }
  if (act && (tool == 2 || ui.erasing)) {
    if (io.MousePos.x != ui.last.x || io.MousePos.y != ui.last.y) {
      PluginState ns = st;
      double va = ui.erasing ? 1.0 : yV(ui.last.y), vb = ui.erasing ? 1.0 : yV(io.MousePos.y);
      if (ui.erasing) {
        paint(ns, ui.last.x, va, io.MousePos.x, vb);
      } else {  // stamp every half band along the path
        const int steps = std::max(1, static_cast<int>(std::ceil(std::fabs(io.MousePos.x - ui.last.x) / (cellW * 0.5f))));
        for (int k = 1; k <= steps; ++k) {
          const float f = static_cast<float>(k) / steps;
          brush(ns, ui.last.x + (io.MousePos.x - ui.last.x) * f, va + (vb - va) * f);
        }
      }
      ui.last = io.MousePos;
      setPluginState(a, t, ns, "draw EQ bands", ui.gesture);
    }
  } else if (act && tool == 1) {
    dl->AddLine(ui.lineStart, io.MousePos, K.select, 1.5f);
  }
  if (ImGui::IsItemDeactivated()) {
    if (tool == 1 && !ui.erasing) {
      PluginState ns = st;
      paint(ns, ui.lineStart.x, yV(ui.lineStart.y), io.MousePos.x, yV(io.MousePos.y));
      setPluginState(a, t, ns, "draw EQ line", ui.gesture);
    }
    a.doc.endGesture();
  }
  if (hov && io.MouseWheel != 0) {
    int b = bandAt(io.MousePos.x);
    PluginState ns = st;
    float& v = bandVal(ns, bank, target, b);
    v = std::clamp(v + io.MouseWheel * 0.02f, 0.f, 2.f);
    setPluginState(a, t, ns, "nudge EQ band", 0x42515757ull ^ (t.key() << 4) ^ static_cast<uint64_t>(b));
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    PluginState ns = st;
    bandVal(ns, bank, target, bandAt(io.MousePos.x)) = 1.0f;
    setPluginState(a, t, ns, "reset EQ band");
  }
  if (hov || act) {
    int b = bandAt(io.MousePos.x);
    double v = val(st, bqValue(bank, target, b), 1), hz = bqBandHz(b);
    char rd[80], f[24], buf[200];
    if (hz >= 1000) std::snprintf(f, sizeof f, "%.2f kHz", hz / 1000);
    else std::snprintf(f, sizeof f, "%.0f Hz", hz);
    if (target == BtPan)
      std::snprintf(rd, sizeof rd, "%d · %s · %+.0f%%", b + 1, f, (v - 1) * 100);
    else
      std::snprintf(rd, sizeof rd, "%d · %s · %s", b + 1, f, fmtDb(static_cast<float>(20 * std::log10(std::max(1e-9, bqGain(v)))), " dB").c_str());
    std::snprintf(buf, sizeof buf, "Band %s   (drag to draw · right-drag resets · wheel nudges · double-click resets)", rd);
    a.setHint(buf);
    skin::text(dl, F.p(108, 28), K.text, rd, fs);
  }
  if (tool == 0) drawui::chip(a, "bankeq", g0, g1);

  // ---- bank strip ----
  skin::text(dl, F.p(18, 209), K.textDim, "BANK", fs);
  skin::toggle(a, t, BqEditMaster, F.p(53, 205), F.sz(23, 18), K, "M", fs);
  if (ImGui::IsItemHovered()) a.setHint("Draw the master curve (applied on top of the morphed banks)");
  {
    static const char* banks[kBqBanks] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    skin::switchSel(a, t, BqEditBank, F.p(76, 205), F.sz(144, 18), K, banks, kBqBanks, true);
    // Morph position marker over the bank cells.
    float mx = F.p(76, 0).x + static_cast<float>((val(st, BqMorph) + 0.5) / kBqBanks) * F.u(144);
    dl->AddTriangleFilled(ImVec2(mx - 3 * F.s, F.p(0, 203).y), ImVec2(mx + 3 * F.s, F.p(0, 203).y), ImVec2(mx, F.p(0, 206).y), K.graphCurve);
  }
  skin::switchSel(a, t, BqEditTarget, F.p(262, 205), F.sz(126, 18), K, targetNames, kBqTargets, true);

  // ---- right column ----
  skin::text(dl, F.p(406, 8), K.textDim, "MOTION", fs);
  skin::labeledWheel(a, t, BqShift, F, 404, 24, 40, K, "SHIFT", skin::WheelBigLine | skin::WheelCentered);
  skin::labeledWheel(a, t, BqMix, F, 462, 24, 40, K, "MIX", skin::WheelBigLine | skin::WheelCentered);
  skin::labeledWheel(a, t, BqMorph, F, 519, 24, 40, K, "MORPH");
  skin::labeledWheel(a, t, BqSmooth, F, 434, 79, 40, K, "SMOOTH");
  skin::labeledWheel(a, t, BqOutput, F, 492, 79, 40, K, "VOL", skin::WheelBigLine | skin::WheelCentered);
  skin::text(dl, F.p(406, 139), K.textDim, "SETUP", fs);
  skin::check(a, t, BqSendOnly, F.p(401, 158), F.sz(81, 15), K, "SEND ONLY");
  skin::text(dl, F.p(491, 160), K.textDim, "SEND", fs);
  {
    int to = ival(st, BqSendTo, -1);
    char buf[16];
    if (to < 0) std::snprintf(buf, sizeof buf, "--");
    else std::snprintf(buf, sizeof buf, "%d", to);
    skin::digi(a, t, BqSendTo, F.p(526, 154), F.sz(34, 22), K, buf);
    if (ImGui::IsItemHovered()) a.setHint("Mixer insert that receives the send curve (drag; -- = none)");
  }
  skin::labeledWheel(a, t, BqBandwidth, F, 519, 182, 40, K, "BW");
  skin::check(a, t, BqAddMode, F.p(401, 202), F.sz(78, 16), K, "ADD MODE");
  skin::check(a, t, BqOversample, F.p(401, 222), F.sz(92, 15), K, "OVERSAMPLE");
  F.end(kW, kH);
}

}  // namespace gl::packs::utility
