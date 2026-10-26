// Gridline — Delay editor on the reference delay's 747 × 217 panel grid
// (input slider, time / stereo / feedback / filter / lo-fi / output rows on top,
// modulation / diffusion / distortion + transfer display underneath), drawn
// with the plugin's rack skin. Shown for the reference delay model.
#include "../core/DelayCurves.h"
#include "../engine/Delay3Ref.h"
#include "../core/I18n.h"
#include "../engine/Dsp.h"
#include "../engine/GraphModels.h"
#include "../engine/LiveGraph.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

const skin::Skin& delaySkin() {
  static skin::Skin k = [] {
    skin::Skin s = skin::Skin::make(0x566065, 0x5A656B, 0xB1E758, 0x9ADF26, 0x3F484D, 0xB9BFC3);
    s.panelHi = skin::hex(0x5E696E);
    s.panelLo = skin::hex(0x373F42);
    s.lcdBg = skin::hex(0x2B3235);
    s.lcdText = skin::hex(0xC8F07E);
    s.graphBg = skin::hex(0x2B3235);
    s.graphGrid = skin::hex(0x3B4448);
    s.graphGridStrong = skin::hex(0x4C575C);
    s.graphText = skin::hex(0xB9BFC3);
    s.handle = skin::hex(0x30373A);
    s.select = skin::hex(0xB1E758);
    return s;
  }();
  return k;
}

void wheelL(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k, const char* label,
            int flags = 0) {
  skin::wheel(a, t, param, F.p(x, y), size * F.s, k, flags);
  if (label)
    skin::label(ImGui::GetWindowDrawList(), F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 1.5f).y, label, k.textDim, std::clamp(8.0f * F.s, 10.0f, 13.0f));
}

// Musical divisions of the reference time menu (ticks of 1/192 beat).
struct Division {
  const char* name;
  int ticks;
};
const Division kDivisions[] = {{"1/64", 12},        {"1/32", 24},       {"1/16", 48},        {"1/8", 96},        {"1/4", 192},
                               {"1/2", 384},        {"1/16 dotted", 72}, {"1/8 dotted", 144}, {"1/4 dotted", 288}, {"1/2 dotted", 576},
                               {"1/16 triplet", 32}, {"1/8 triplet", 64}, {"1/4 triplet", 128}, {"1/2 triplet", 256}};

// Transfer curve of the loop distortion (limit / saturate): the feedback display.
void distortionVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const PluginState& st, const skin::Skin& k) {
  skin::graphWell(dl, p0, p1, k);
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const int type = ival(st, DlyDistType);
  const double thrN = std::clamp(val(st, DlyDistThresh) / -28.5, 0.0, 1.0);
  const double T = dsp::dbToGain(dly::distThreshDb(thrN)), makeup = dly::distMakeup(thrN);
  const double hard = val(st, DlyDistHardness), sym = val(st, DlyDistSymmetry);
  auto f = [&](double x) {
    if (type == 1) return std::clamp(x, -T * makeup, T * makeup);
    if (type == 2) {
      double top = T * (x >= 0 ? 1 + sym : 1 - sym), ax = std::fabs(x), lin = hard * top, kk = std::max(1e-11, (1 - hard) * top);
      double y = ax <= lin ? ax : (ax < lin + 2 * kk ? ax - (ax - lin) * (ax - lin) / (4 * kk) : lin + kk);
      return (x < 0 ? -y : y) * makeup;
    }
    return x;
  };
  double range = std::max(1.5, std::max(std::fabs(f(10.0)), std::fabs(f(-10.0))) * 1.25);
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  dl->AddLine(ImVec2(p0.x + 2, c.y), ImVec2(p1.x - 2, c.y), k.graphGrid);
  dl->AddLine(ImVec2(c.x, p0.y + 2), ImVec2(c.x, p1.y - 2), k.graphGrid);
  for (float u : {-1.0f, 1.0f}) {
    float px = c.x + u * static_cast<float>(w * 0.5 / range), py = c.y - u * static_cast<float>(h * 0.5 / range);
    dl->AddLine(ImVec2(px, c.y - 3), ImVec2(px, c.y + 3), k.graphGridStrong);
    dl->AddLine(ImVec2(c.x - 3, py), ImVec2(c.x + 3, py), k.graphGridStrong);
  }
  ImVec2 pts[64];
  for (int i = 0; i < 64; ++i) {
    double x = (i / 63.0 * 2 - 1) * range;
    pts[i] = ImVec2(p0.x + w * i / 63.0f, c.y - static_cast<float>(std::clamp(f(x) / range, -1.0, 1.0)) * h * 0.5f);
  }
  dl->AddPolyline(pts, 64, k.fill, 0, 1.6f);
}

}  // namespace

// J-26: the classic tempo delay (DlyModel = 2) on a compact three-row panel.
void editorDelayClassic(App& a, const PluginTarget& t, float width, const PluginState& st) {
  const skin::Skin& K = delaySkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kW = 500, kH = 182;
  skin::Form F = skin::Form::fit(width, kW);
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const float c0 = 5, c1 = 170, c2 = 335, bw = 160, bh = 45;
  skin::paramBox(a, t, DlyClsIn, F, c0, 5, bw, bh, K, "INPUT");
  skin::paramBox(a, t, DlyClsPan, F, c1, 5, bw, bh, K, "INPUT PAN", nullptr, skin::WheelBigLine | skin::WheelCentered);
  skin::paramBox(a, t, DlyClsDry, F, c2, 5, bw, bh, K, "DRY");
  skin::paramBox(a, t, DlyClsFb, F, c0, 55, bw, bh, K, "FEEDBACK");
  skin::paramBox(a, t, DlyClsCoef, F, c1, 55, bw, bh, K, "CUTOFF");
  {
    char b[24];
    int ticks = static_cast<int>(std::lround(val(st, DlyTicks, 144)));
    std::snprintf(b, sizeof b, "%d:%02d", ticks / 48, ticks % 48);
    skin::paramBox(a, t, DlyTicks, F, c2, 55, bw, bh, K, "TIME", b);
  }
  skin::paramBox(a, t, DlyClsOffset, F, c0, 105, bw, bh, K, "STEREO OFFSET", nullptr, skin::WheelBigLine | skin::WheelCentered);
  {
    skin::well(dl, F.p(c1, 105), F.p(c1 + bw, 105 + bh), K, 4 * F.s);
    static const char* md[3] = {"NORMAL", "SWAP", "P.PONG"};
    skin::switchSel(a, t, DlyClsMode, F.p(c1 + 4, 112), F.sz(bw - 8, 30), K, md, 3, true);
  }
  {
    char b[24];
    double bpm = val(st, DlyClsBpm);
    if (bpm <= 0) std::snprintf(b, sizeof b, "host");
    else std::snprintf(b, sizeof b, "%.0f BPM", bpm);
    skin::paramBox(a, t, DlyClsBpm, F, c2, 105, bw, bh, K, "TEMPO", b);
  }
  static const char* models[4] = {"LEGACY", "REF", "CLASSIC", "STEREO"};
  skin::text(dl, F.p(c0 + 4, 162), K.textDim, "MODEL", F.font(8));
  skin::switchSel(a, t, DlyModel, F.p(c0 + 46, 157), F.sz(220, 20), K, models, 4, true);
  skin::toggle(a, t, DlyClsStepRound, F.p(c1 + 110, 157), F.sz(70, 20), K, "1/16 GRID", F.font(7.5f));
  skin::toggle(a, t, DlyClsScaleSr, F.p(c2 + 85, 157), F.sz(75, 20), K, "RATE COMP", F.font(7.5f));
  F.end(kW, kH);
}

// J-35: the stereo delay reference (DlyModel = 3): the same grid, every control
// bound to the reference's normalised value (DlyD3Raw0 + index).
void editorDelayStereo(App& a, const PluginTarget& t, float width, const PluginState& st) {
  const skin::Skin& K = delaySkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kW = 747, kH = 242;
  skin::Form F{ImGui::GetCursorScreenPos(), width / kW};
  const float s = F.s;
  const float fsL = std::clamp(8.0f * s, 10.0f, 13.0f), fsV = std::clamp(8.5f * s, 10.0f, 13.0f);
  skin::panel(dl, F.o, ImVec2(F.o.x + width, F.o.y + kH * s), K, 4);
  auto R = [](int k) { return DlyD3Raw0 + k; };
  auto rv = [&](int k) { return val(st, R(k)); };
  auto cell = [&](int k, int count) { return std::clamp(static_cast<int>(rv(k) * (count - 1) + 0.5), 0, count - 1); };
  // A row (or column) of option buttons writing idx / (count - 1).
  auto opts = [&](const char* id, int k, const char* const* names, int count, float x, float y, float w, float h, bool vertical) {
    const int on = cell(k, count);
    for (int i = 0; i < count; ++i) {
      ImVec2 tl = vertical ? F.p(x, y + i * h / count) : F.p(x + i * w / count, y);
      ImVec2 sz = vertical ? ImVec2(w * s, (h / count - 1) * s) : ImVec2((w / count - 1) * s, h * s);
      char bid[48];
      std::snprintf(bid, sizeof bid, "%s%d", id, i);
      if (skin::button(bid, tl, sz, K, names[i], i == on, fsL)) a.doc.setParam(t.pid(R(k)), count > 1 ? static_cast<double>(i) / (count - 1) : 0, false);
    }
  };
  auto caption = [&](float x0, float x1, float y, const char* text) {
    dl->AddLine(F.p(x0, y + 4), F.p(x1, y + 4), skin::darken(K.panel, 0.25f), 1.0f);
    skin::label(dl, F.p((x0 + x1) * 0.5f, 0).x, F.p(0, y - 1).y, text, K.textDim, fsL);
  };
  caption(6, 54, 8, "IN");
  caption(62, 250, 8, "TIME");
  caption(256, 374, 8, "STEREO");
  caption(380, 588, 8, "FEEDBACK");
  caption(589, 684, 8, "LO-FI");
  caption(690, 742, 8, "OUT");
  caption(62, 248, 126, "MODULATION");
  caption(262, 370, 126, "DIFFUSION");
  caption(380, 684, 126, "DISTORTION");
  skin::vslider(a, t, R(d3ref::PInWet), F.p(11, 26), ImVec2(38 * s, 153 * s), K);
  skin::label(dl, F.p(30, 0).x, F.p(0, 182).y, "WET", K.textDim, fsL);
  const bool sync = rv(d3ref::PSync) >= 0.5;
  skin::wheel(a, t, R(d3ref::PTime), F.p(67, 30), 64 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    if (sync) {
      const int ticks = static_cast<int>(rv(d3ref::PTime) * 767 + 0.5) + 1;
      std::snprintf(buf, sizeof buf, "%d:%02d", ticks / 48, ticks % 48);
    } else {
      std::snprintf(buf, sizeof buf, "%.0f ms", d3ref::freeTimeSec(rv(d3ref::PTime)) * 1000);
    }
    skin::lcd(dl, F.p(73, 97), F.p(121, 112), K, buf, fsV);
  }
  static const char* onOff[2] = {"OFF", "ON"};
  skin::label(dl, F.p(170, 0).x, F.p(0, 30).y, "TEMPO SYNC", K.textDim, fsL);
  opts("##d3sync", d3ref::PSync, onOff, 2, 137, 40, 65, 13, false);
  skin::label(dl, F.p(170, 0).x, F.p(0, 56).y, "KEEP PITCH", K.textDim, fsL);
  opts("##d3kp", d3ref::PKeepPitch, onOff, 2, 137, 66, 65, 13, false);
  skin::hslider(a, t, R(d3ref::PSmooth), F.p(135, 86), ImVec2(71 * s, 13 * s), K);
  skin::label(dl, F.p(170, 0).x, F.p(0, 101).y, "SMOOTH", K.textDim, fsL);
  wheelL(a, t, R(d3ref::POffset), F, 207, 42, 40, K, "R/L OFS", skin::WheelCentered | skin::WheelBigLine);
  static const char* types[4] = {"FAKE ST", "STEREO", "PING PONG", "OFF"};
  opts("##d3type", d3ref::PType, types, 4, 258, 31, 66, 62, true);
  wheelL(a, t, R(d3ref::PSpread), F, 330, 42, 40, K, "SPREAD", skin::WheelBigLine);
  skin::wheel(a, t, R(d3ref::PFeedback), F.p(382, 30), 64 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.0f%%", rv(d3ref::PFeedback) * 125);
    skin::lcd(dl, F.p(390, 97), F.p(438, 112), K, buf, fsV);
  }
  wheelL(a, t, R(d3ref::PCutoff), F, 452, 42, 40, K, "CUTOFF", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PRes), F, 499, 42, 40, K, "RES", skin::WheelBigLine);
  static const char* modes[4] = {"LP", "HP", "BP", "OFF"};
  opts("##d3flt", d3ref::PFilter, modes, 4, 551, 31, 35, 62, true);
  wheelL(a, t, R(d3ref::PRate), F, 591, 42, 40, K, "RATE", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PBits), F, 639, 42, 40, K, "BITS", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::POutWet), F, 696, 31, 40, K, "WET", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PTone), F, 696, 93, 40, K, "TONE", skin::WheelCentered | skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PDry), F, 696, 155, 40, K, "DRY", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PModRate), F, 70, 150, 40, K, "RATE", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PModTime), F, 156, 150, 40, K, "TIME", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PModCut), F, 207, 150, 40, K, "CUTOFF", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PDifLevel), F, 269, 150, 40, K, "LEVEL", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PDifSpread), F, 321, 150, 40, K, "SPREAD", skin::WheelBigLine);
  static const char* dt[2] = {"LIMIT", "SAT"};
  opts("##d3dist", d3ref::PDistType, dt, 2, 384, 148, 50, 44, true);
  wheelL(a, t, R(d3ref::PThresh), F, 452, 150, 40, K, "THRES", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PHard), F, 499, 150, 40, K, "HARD", skin::WheelBigLine);
  wheelL(a, t, R(d3ref::PSym), F, 550, 150, 40, K, "SYM", skin::WheelBigLine);
  // J-68: the feedback visual (610,138 70×70): the loop distortion's transfer
  // curve from the engine's own limiter / knee law, redrawn when the
  // distortion controls move.
  {
    const int type = cell(d3ref::PDistType, 2);
    const double thr = rv(d3ref::PThresh), hard = rv(d3ref::PHard), sym = rv(d3ref::PSym);
    const uint64_t in = livegraph::Hasher().i(type).d(thr).d(hard).d(sym).value();
    constexpr int kPts = 65;
    const livegraph::Curve& c = livegraph::memo<livegraph::Curve>(t.key(), 0x44334656).get(in, [&](livegraph::Curve& o) {
      o.resize(kPts);
      for (int i = 0; i < kPts; ++i) o[static_cast<size_t>(i)] = static_cast<float>(graphmodel::delay3Transfer(type, thr, hard, sym, (i / (kPts - 1.0) * 2 - 1) * 1.5));
    });
    ImVec2 q0 = F.p(610, 138), q1 = F.p(680, 208);
    skin::graphWell(dl, q0, q1, K);
    const ImVec2 cc((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f);
    const float hw = (q1.x - q0.x) * 0.5f - 3, hh = (q1.y - q0.y) * 0.5f - 3;
    dl->AddLine(ImVec2(q0.x + 2, cc.y), ImVec2(q1.x - 2, cc.y), K.graphGrid);
    dl->AddLine(ImVec2(cc.x, q0.y + 2), ImVec2(cc.x, q1.y - 2), K.graphGrid);
    ImVec2 pts[kPts];
    float peak = 0;  // output axis follows the curve's own peak (the make-up gain can lift or lower it)
    for (float y : c) peak = std::max(peak, std::fabs(y));
    const float ys = std::max(0.25f, peak * 1.15f);
    for (int i = 0; i < kPts; ++i)
      pts[i] = ImVec2(cc.x + hw * (i / (kPts - 1.0f) * 2 - 1), cc.y - hh * std::clamp(c[static_cast<size_t>(i)] / ys, -1.0f, 1.0f));
    dl->AddPolyline(pts, kPts, K.fill, 0, 1.6f);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##d3fbvis", ImVec2(q1.x - q0.x, q1.y - q0.y));
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Feedback distortion: input level (x) to output level (y)"));
  }
  static const char* models[4] = {"LEGACY", "REFERENCE", "CLASSIC", "STEREO REF"};
  skin::text(dl, F.p(10, 222), K.textDim, "MODEL", F.font(8));
  skin::switchSel(a, t, DlyModel, F.p(56, 217), F.sz(300, 20), K, models, 4, true);
  ImGui::SetCursorScreenPos(F.o);
  ImGui::Dummy(ImVec2(width, kH * s));
}

void editorDelayRef(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  if (ival(st, DlyModel) == 2) {
    editorDelayClassic(a, t, width, st);
    return;
  }
  if (ival(st, DlyModel) == 3) {
    editorDelayStereo(a, t, width, st);
    return;
  }
  const skin::Skin& K = delaySkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kW = 747, kH = 217;
  skin::Form F{ImGui::GetCursorScreenPos(), width / kW};
  const float s = F.s;
  const float fsL = std::clamp(8.0f * s, 10.0f, 13.0f), fsV = std::clamp(8.5f * s, 10.0f, 13.0f);
  skin::panel(dl, F.o, ImVec2(F.o.x + width, F.o.y + kH * s), K, 4);
  auto caption = [&](float x0, float x1, float y, const char* text) {
    dl->AddLine(F.p(x0, y + 4), F.p(x1, y + 4), skin::darken(K.panel, 0.25f), 1.0f);
    ImGui::PushFont(a.fontMonoBold, fsL);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 c = F.p((x0 + x1) * 0.5f, y);
    dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 4, c.y - 1), ImVec2(c.x + ts.x * 0.5f + 4, c.y + ts.y), K.panel);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - 1)), K.textDim, text);
    ImGui::PopFont();
  };
  caption(6, 54, 8, "IN");
  caption(62, 250, 8, "TIME");
  caption(256, 374, 8, "STEREO");
  caption(380, 588, 8, "FEEDBACK");
  caption(589, 684, 8, "LO-FI");
  caption(690, 742, 8, "OUT");
  caption(62, 248, 126, "MODULATION");
  caption(262, 370, 126, "DIFFUSION");
  caption(380, 684, 126, "DISTORTION");

  // Input level slider (11,26) 38×153.
  skin::vslider(a, t, DlyInput, F.p(11, 26), ImVec2(38 * s, 153 * s), K);
  skin::label(dl, F.p(30, 0).x, F.p(0, 182).y, "WET", K.textDim, fsL);
  // Time (67,30) 64 + read-out (79,96) 40×15.
  const bool sync = bval(st, DlySync, true);
  skin::wheel(a, t, sync ? DlyTicks : DlyTimeMs, F.p(67, 30), 64 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    if (sync) {
      int ticks = ival(st, DlyTicks, 192);
      std::snprintf(buf, sizeof buf, "%d:%02d", ticks / 48, ticks % 48);
    } else {
      std::snprintf(buf, sizeof buf, "%.0f ms", val(st, DlyTimeMs, 300));
    }
    skin::lcd(dl, F.p(73, 97), F.p(121, 112), K, buf, fsV);
    // Divisions menu (the reference's wheel "Set" submenu).
    if (sync && skin::button("##divs", F.p(123, 97), ImVec2(11 * s, 15 * s), K, GL_ICON_LABEL("chevron-down", "v"), false, fsL)) ImGui::OpenPopup("##dlydivs");
    if (sync && ImGui::IsItemHovered()) a.setHint("Set the time to a musical division");
    if (ImGui::BeginPopup("##dlydivs")) {
      int cur = ival(st, DlyTicks, 192);
      for (const Division& d : kDivisions)
        if (ImGui::MenuItem(d.name, nullptr, d.ticks == cur)) a.doc.setParam(t.pid(DlyTicks), d.ticks, false);
      ImGui::EndPopup();
    }
  }
  skin::check(a, t, DlySync, F.p(137, 35), ImVec2(65 * s, 17 * s), K, "TEMPO SYNC");
  skin::check(a, t, DlyKeepPitch, F.p(137, 54), ImVec2(65 * s, 17 * s), K, "KEEP PITCH");
  skin::hslider(a, t, DlyTimeSmooth, F.p(135, 78), ImVec2(71 * s, 15 * s), K);
  skin::label(dl, F.p(170, 0).x, F.p(0, 95).y, "SMOOTH", K.textDim, fsL);
  wheelL(a, t, DlyTimeR, F, 207, 42, 40, K, "R/L OFS", skin::WheelCentered | skin::WheelBigLine);
  {
    static const char* types[4] = {"FAKE ST", "STEREO", "PING PONG", "OFF"};
    skin::switchSel(a, t, DlyType3, F.p(258, 31), ImVec2(66 * s, 62 * s), K, types, 4);
  }
  wheelL(a, t, DlySpread3, F, 330, 42, 40, K, "SPREAD", skin::WheelBigLine);
  // Feedback (382,30) 64, cutoff (452,42) + read-out, resonance (499,42), mode (551,31) 35×62.
  skin::wheel(a, t, DlyFeedback, F.p(382, 30), 64 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.0f%%", val(st, DlyFeedback, 0.625f) * 100);
    skin::lcd(dl, F.p(390, 97), F.p(438, 112), K, buf, fsV);
  }
  const int fmode = ival(st, DlyFilter3);
  skin::wheel(a, t, DlyCutoff3, F.p(452, 42), 40 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    double hz = val(st, DlyCutoff3, 22000);
    if (hz >= 1000) std::snprintf(buf, sizeof buf, "%.1fk", hz / 1000);
    else std::snprintf(buf, sizeof buf, "%.0f", hz);
    skin::lcd(dl, F.p(450, 97), F.p(494, 112), K, fmode == 3 ? "off" : buf, fsV);
  }
  wheelL(a, t, DlyResonance, F, 499, 42, 40, K, "RES", skin::WheelBigLine);
  {
    static const char* modes[4] = {"LP", "HP", "BP", "OFF"};
    skin::switchSel(a, t, DlyFilter3, F.p(551, 31), ImVec2(35 * s, 62 * s), K, modes, 4);
  }
  wheelL(a, t, DlyRate3, F, 591, 42, 40, K, "RATE", skin::WheelBigLine);
  wheelL(a, t, DlyBits, F, 639, 42, 40, K, "BITS", skin::WheelBigLine);
  wheelL(a, t, DlyWet, F, 696, 31, 40, K, "WET", skin::WheelBigLine);
  wheelL(a, t, DlyTone, F, 696, 93, 40, K, "TONE", skin::WheelCentered | skin::WheelBigLine);
  wheelL(a, t, DlyDry, F, 696, 155, 40, K, "DRY", skin::WheelBigLine);
  // Modulation: rate (66,146) 48 → arrow → time (156,150) + cutoff (207,150), read-outs underneath.
  skin::wheel(a, t, DlyModRate3, F.p(66, 146), 48 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.2f Hz", val(st, DlyModRate3, 1.14f));
    skin::lcd(dl, F.p(62, 197), F.p(118, 212), K, buf, fsL);
    ImVec2 c = F.p(136, 170);
    float r = 6 * s;
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), K.textDim, 1.5f);
    dl->AddTriangleFilled(ImVec2(c.x + r + 4, c.y), ImVec2(c.x + r - 1, c.y - 4), ImVec2(c.x + r - 1, c.y + 4), K.textDim);
  }
  skin::wheel(a, t, DlyModDepth, F.p(156, 150), 40 * s, K, skin::WheelBigLine);
  skin::wheel(a, t, DlyModCutoff, F.p(207, 150), 40 * s, K, skin::WheelBigLine);
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "TIME %.0f%%", val(st, DlyModDepth) * 100);
    skin::lcd(dl, F.p(150, 197), F.p(202, 212), K, buf, fsL);
    std::snprintf(buf, sizeof buf, "CUT %.0f%%", val(st, DlyModCutoff) * 100);
    skin::lcd(dl, F.p(204, 197), F.p(254, 212), K, buf, fsL);
  }
  wheelL(a, t, DlyDiffusion, F, 269, 150, 40, K, "LEVEL", skin::WheelBigLine);
  wheelL(a, t, DlyDiffSpread, F, 321, 150, 40, K, "SPREAD", skin::WheelBigLine);
  {
    static const char* dt[3] = {"OFF", "LIMIT", "SAT"};
    skin::switchSel(a, t, DlyDistType, F.p(384, 148), ImVec2(50 * s, 44 * s), K, dt, 3);
  }
  wheelL(a, t, DlyDistThresh, F, 452, 150, 40, K, "THRES", skin::WheelBigLine);
  wheelL(a, t, DlyDistHardness, F, 499, 150, 40, K, "HARD", skin::WheelBigLine);
  wheelL(a, t, DlyDistSymmetry, F, 550, 150, 40, K, "SYM", skin::WheelBigLine);
  distortionVisual(dl, F.p(610, 138), F.p(680, 208), st, K);
  ImGui::SetCursorScreenPos(F.o);
  ImGui::Dummy(ImVec2(width, kH * s));
}

}  // namespace gl
