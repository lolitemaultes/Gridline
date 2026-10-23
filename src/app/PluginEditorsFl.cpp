// Gridline — J-19 reference-family editors for the built-in effects: each
// editor sits on its reference panel's grid (form units scaled to the window
// width by skin::Form) with the reference palette roles, drawn with the
// PluginSkin kit.
#include <map>
#include "../engine/Bitcrush.h"
#include "../engine/ClassicModFx.h"
#include "../engine/FlFilterRef.h"
#include "../engine/FreqShiftRef.h"
#include "../engine/Dsp.h"
#include "../engine/GraphModels.h"
#include "../engine/LiveGraph.h"
#include "../engine/Program.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

using namespace pp;

namespace {

using namespace ed;

// ---------------------------------------------------------------- Flanger
// Reference panel 500 × 205: four rows of three wheel modules (160 × 45).
const skin::Skin& flangerSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x3C4346, 0x464E51, 0xE5D4B3, 0x3F484D, 0xA2B0B7);
    s.pillBg = skin::hex(0x444D53);
    s.valueBg = skin::hex(0x656F74);
    s.valueText = skin::hex(0xE5D4B3);
    return s;
  }();
  return k;
}

void lfoPreview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, double rate, double depth, double shape, double stereo) {
  skin::graphWell(dl, p0, p1, k);
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const double t0 = ImGui::GetTime();
  for (int side = 0; side < 2; ++side) {
    ImVec2 pts[64];
    for (int i = 0; i < 64; ++i) {
      double ph = i / 63.0 * 2.0 + side * stereo + t0 * rate * 0.25;
      ph -= std::floor(ph);
      double tri = ph < 0.25 ? ph * 4 : ph < 0.75 ? 2 - ph * 4 : ph * 4 - 4;
      double y = std::sin(2 * dsp::kPi * ph) * (1 - shape) + tri * shape;
      pts[i] = ImVec2(p0.x + 3 + (w - 6) * i / 63.0f, p0.y + h * 0.5f - static_cast<float>(y * std::max(0.05, depth)) * (h * 0.5f - 4));
    }
    dl->AddPolyline(pts, 64, side ? k.accent2C() : k.graphCurve, 0, 1.4f);
  }
}

void editorFlangerFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = flangerSkin();
  constexpr float kW = 500, kH = 228;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const bool ref = ival(st, FlgModel) == 1;  // J-26 reference engine: knob positions
  const float c0 = 5, c1 = 170, c2 = 335, bw = 160, bh = 45;
  skin::paramBox(a, t, ref ? FlgDelayN : FlgDelay, F, c0, 5, bw, bh, K, "DELAY");
  skin::paramBox(a, t, ref ? FlgDepthN : FlgDepth, F, c1, 5, bw, bh, K, "DEPTH");
  skin::paramBox(a, t, ref ? FlgRateN : FlgRate, F, c2, 5, bw, bh, K, "RATE");
  skin::paramBox(a, t, FlgPhase, F, c0, 55, bw, bh, K, "PHASE");
  skin::paramBox(a, t, ref ? FlgDampN : FlgDamp, F, c1, 55, bw, bh, K, "DAMP");
  skin::paramBox(a, t, FlgShape, F, c2, 55, bw, bh, K, "SHAPE");
  skin::paramBox(a, t, ref ? FlgFeedN : FlgFeedback, F, c0, 105, bw, bh, K, "FEED");
  // Feedback polarity: the reference's separate invert-feedback module flips
  // the sign of the (bipolar) feedback here.
  {
    skin::captionWell(dl, F, c1, 105, bw, bh, K, "INV FEEDBACK", F.font(7.5f), 54);
    const double fb = ref ? (bval(st, FlgInvFb) ? -1.0 : 1.0) : val(st, FlgFeedback, 0.5f);
    ImGui::PushID("invfb");
    if (skin::button("##invfb", F.p(c1 + 6, 110), F.sz(40, 34), K, fb < 0 ? "-" : "+", fb < 0, F.font(12))) {
      if (ref) pui::setParamValue(a, t, FlgInvFb, fb < 0 ? 0 : 1);
      else pui::setParamValue(a, t, FlgFeedback, -fb);
    }
    if (ImGui::IsItemHovered()) a.setHint("Invert the feedback polarity (flips the sign of FEED)");
    ImGui::PopID();
    skin::valueStrip(dl, F.p(c1 + 55, 131), F.p(c1 + bw - 3, 145), K, fb < 0 ? "inverted" : "normal", F.font(8));
  }
  {
    skin::captionWell(dl, F, c2, 105, bw, bh, K, "INV WET", F.font(7.5f), 54);
    skin::toggle(a, t, FlgInvertWet, F.p(c2 + 6, 110), F.sz(40, 34), K, "INV", F.font(9));
    skin::valueStrip(dl, F.p(c2 + 55, 131), F.p(c2 + bw - 3, 145), K, bval(st, FlgInvertWet) ? "inverted" : "normal", F.font(8));
  }
  // Bottom row: dry (reference) or the LFO preview, wet, cross.
  if (ref) {
    skin::paramBox(a, t, FlgDryN, F, c0, 155, bw, bh, K, "DRY");
  } else {
    skin::well(dl, F.p(c0, 155), F.p(c0 + bw, 155 + bh), K, 4 * F.s);
    lfoPreview(dl, F.p(c0 + 4, 159), F.p(c0 + bw - 4, 155 + bh - 4), K, val(st, FlgRate, 0.2f), val(st, FlgDepth, 0.6f), val(st, FlgShape),
               val(st, FlgPhase, 90) / 360.0);
  }
  skin::paramBox(a, t, ref ? FlgWetN : FlgMix, F, c1, 155, bw, bh, K, "WET");
  skin::paramBox(a, t, ref ? FlgCrossN : FlgCross, F, c2, 155, bw, bh, K, "CROSS");
  {
    static const char* eng[2] = {"LEGACY", "REFERENCE"};
    skin::text(dl, F.p(c0 + 4, 209), K.textDim, "ENGINE", F.font(8));
    skin::switchSel(a, t, FlgModel, F.p(c0 + 52, 205), F.sz(150, 18), K, eng, 2, true);
    if (ref) {
      char b[96];
      std::snprintf(b, sizeof b, "%.2f ms + %.2f ms · %.3f Hz", classic::flangerMs(val(st, FlgDelayN)), classic::flangerMs(val(st, FlgDepthN, 0.4f)),
                    classic::flangerRateHz(val(st, FlgRateN, 0.4f)));
      skin::text(dl, F.p(c1 + 50, 209), K.textDim, b, F.font(8));
    }
  }
  F.end(kW, kH);
}

// ================================================================ shared helpers
// A reference panel placed at the cursor: scaled to the editor width but capped
// at `maxScale` (small reference panels would otherwise grow huge) and centred;
// the skin's panel fills the whole editor width behind it.
struct Frame {
  ImVec2 c;
  float w = 0, formW = 0, formH = 0;
  skin::Form F;
  Frame(float width, float fw, float fh, float maxScale, const skin::Skin& k) : c(ImGui::GetCursorScreenPos()), w(width), formW(fw), formH(fh) {
    float s = std::min(width / fw, maxScale);
    F = skin::Form{ImVec2(std::round(c.x + (width - fw * s) * 0.5f), c.y), s};
    skin::panel(ImGui::GetWindowDrawList(), c, ImVec2(c.x + w, c.y + fh * s), k, 4);
  }
  void end() const {
    ImGui::SetCursorScreenPos(c);
    ImGui::Dummy(ImVec2(w, formH * F.s));
  }
};

// Small-caps label centred on a form x.
void flabel(ImDrawList* dl, const skin::Form& F, float cx, float y, const skin::Skin& k, const char* text, float px = 8.0f) {
  skin::label(dl, F.p(cx, 0).x, F.p(0, y).y, text, k.textDim, F.font(px));
}

// Wheel module with a value slider under the caption (the reference filter's
// cells): well, wheel, caption pill carrying the live value, rail slider.
void sliderBox(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k,
               const char* caption) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const skin::Module m = skin::module(F, x, y, w, h);
  skin::well(dl, m.b0, m.b1, k, 4 * F.s);
  skin::wheel(a, t, param, m.wheelTl, m.wheelSize, k, skin::WheelBigLine);
  auto d = describeParam(a.doc.song(), t.pid(param));
  std::string cap = caption;
  if (d) cap += "  " + d->format(pui::paramValue(a, t, param));
  skin::pill(dl, m.pill0, m.pill1, k, cap.c_str(), F.font(8.0f));
  skin::hslider(a, t, param, m.row0, ImVec2(m.row1.x - m.row0.x, m.row1.y - m.row0.y), k);
}

// Narrow module (77 wide): caption pill on the right of the top, control below.
skin::Module miniWell(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption) {
  return skin::captionWell(dl, F, x, y, w, h, k, caption, F.font(8.0f), 17);
}

// Choice parameter laid out as a grid of cells (cols × rows), reference switch style.
bool gridSel(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* const* labels, int count, int cols) {
  auto d = describeParam(a.doc.song(), t.pid(param));
  if (!d || count <= 0) return false;
  int rows = (count + cols - 1) / cols;
  int cur = std::clamp(static_cast<int>(std::lround(pui::paramValue(a, t, param) - d->min)), 0, count - 1);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, k.lcdBg, 3);
  dl->AddRect(tl, p1, skin::darken(k.panel, 0.6f), 3, 0, 1.0f);
  float cw = size.x / cols, ch = size.y / rows;
  bool changed = false;
  ImGui::PushID(param + 91000);
  for (int i = 0; i < count; ++i) {
    ImVec2 c0(tl.x + (i % cols) * cw, tl.y + (i / cols) * ch), c1(c0.x + cw, c0.y + ch);
    ImGui::SetCursorScreenPos(c0);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##g", ImVec2(cw, ch)) && i != cur) {
      pui::setParamValue(a, t, param, d->min + i);
      cur = i, changed = true;
    }
    bool hov = ImGui::IsItemHovered();
    if (hov) a.setHint(d->name + ": " + d->format(d->min + i));
    ImGui::PopID();
    bool on = i == cur;
    if (on) dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), theme::withAlpha(k.select, 0.30f), 2);
    else if (hov) dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), theme::withAlpha(k.text, 0.08f), 2);
    ImGui::PushFont(app().fontMonoBold, std::clamp(ch * 0.55f, 8.0f, 12.0f));
    ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    dl->PushClipRect(c0, c1, true);
    dl->AddText(ImVec2(std::round((c0.x + c1.x - ts.x) * 0.5f), std::round((c0.y + c1.y - ts.y) * 0.5f)),
                on ? skin::lighten(k.select, 0.35f) : k.textDim, labels[i]);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  ImGui::PopID();
  return changed;
}

std::string fmtHz(double hz) {
  char b[32];
  if (std::fabs(hz) >= 1000) std::snprintf(b, sizeof b, "%.2fk", hz / 1000);
  else std::snprintf(b, sizeof b, "%.0f", hz);
  return b;
}

// ================================================================ Phaser
// Reference panel 500 × 155: three rows of three wheel modules; a fourth row
// holds the sweep display and the LFO options Gridline adds.
const skin::Skin& phaserSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x3C4346, 0x464E51, 0xC4E5B3, 0x3F484D, 0xA2B0B7);
    s.pillBg = skin::hex(0x444D53);
    s.valueBg = skin::hex(0x656F74);
    s.valueText = skin::hex(0xC4E5B3);
    return s;
  }();
  return k;
}

void editorPhaserFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = phaserSkin();
  constexpr float kW = 500, kH = 228;
  Frame fr(width, kW, kH, 1.6f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const float c0 = 5, c1 = 170, c2 = 335, bw = 160, bh = 45;
  const bool ref = ival(st, PhsModel) == 1;  // J-26 reference engine
  const bool mm = ival(st, PhsRangeMode) == 1;
  const double sr = a.engine.sampleRate();
  auto apHz = [sr](double d) { return sr / dsp::kPi * std::atan((1 - d) / (1 + d)); };  // all-pass corner of coefficient -d
  skin::paramBox(a, t, ref ? PhsSweepN : PhsRate, F, c0, 5, bw, bh, K, "SWEEP FREQ");
  skin::paramBox(a, t, ref ? PhsDepthMin : mm ? PhsMinFreq : PhsCenter, F, c1, 5, bw, bh, K, ref ? "MIN DEPTH" : mm ? "MIN FREQ" : "CENTRE");
  skin::paramBox(a, t, ref ? PhsDepthMax : mm ? PhsMaxFreq : PhsDepth, F, c2, 5, bw, bh, K, ref ? "MAX DEPTH" : mm ? "MAX FREQ" : "DEPTH");
  double lo = mm ? val(st, PhsMinFreq, 200) : val(st, PhsCenter, 800) * std::pow(2.0, -2.0 * val(st, PhsDepth, 0.7f));
  double hi = mm ? val(st, PhsMaxFreq, 3000) : val(st, PhsCenter, 800) * std::pow(2.0, 2.0 * val(st, PhsDepth, 0.7f));
  if (ref) {
    lo = apHz(val(st, PhsDepthMin, 0.1f)), hi = apHz(val(st, PhsDepthMax, 0.8f));
    if (lo > hi) std::swap(lo, hi);
  }
  {  // Frequency range module: the range mode switch in place of the wheel.
    skin::captionWell(dl, F, c0, 55, bw, bh, K, "FREQ RANGE", F.font(8.5f), 54);
    static const char* rm[2] = {"C+D", "MIN/MAX"};
    static const char* rl[2] = {"SMALL", "LARGE"};
    skin::switchSel(a, t, ref ? PhsRangeLarge : PhsRangeMode, F.p(c0 + 3, 58), F.sz(48, 39), K, ref ? rl : rm, 2);
    std::string r = ref ? (fmtHz(classic::phaserRateHz(val(st, PhsSweepN, 0.5f), ival(st, PhsRangeLarge) == 1) * 1000) + " mHz")
                        : fmtHz(lo) + " - " + fmtHz(hi) + " Hz";
    skin::valueStrip(dl, F.p(c0 + 55, 81), F.p(c0 + bw - 3, 95), K, r.c_str(), F.font(8.5f));
  }
  skin::paramBox(a, t, ref ? PhsStereoN : PhsStereo, F, c1, 55, bw, bh, K, "STEREO");
  skin::paramBox(a, t, ref ? PhsStagesN : PhsStages, F, c2, 55, bw, bh, K, "NR. STAGES");
  skin::paramBox(a, t, ref ? PhsFeedbackN : PhsFeedback, F, c0, 105, bw, bh, K, "FEEDBACK");
  skin::paramBox(a, t, ref ? PhsDryN : PhsMix, F, c1, 105, bw, bh, K, ref ? "DRY" : "DRY-WET");
  skin::paramBox(a, t, ref ? PhsGainN : PhsOutGain, F, c2, 105, bw, bh, K, "OUT GAIN");
  {  // Sweep display: the band the notches travel over, and the sweep position.
    ImVec2 p0 = F.p(c0, 155), p1 = F.p(c1 + bw, 200);
    skin::graphWell(dl, p0, p1, K);
    pui::drawFreqGrid(dl, p0, p1, 24, false);
    float x0 = pui::freqToX(lo, p0.x, p1.x - p0.x), x1 = pui::freqToX(hi, p0.x, p1.x - p0.x);
    dl->AddRectFilled(ImVec2(x0, p0.y + 4), ImVec2(std::max(x0 + 2, x1), p1.y - 4), theme::withAlpha(K.fill, 0.18f), 2);
    double ph = std::fmod(ImGui::GetTime() * val(st, PhsRate, 0.4f), 1.0);
    double f = bval(st, PhsManual) ? lo : lo * std::pow(hi / std::max(1.0, lo), 0.5 + 0.5 * std::sin(2 * dsp::kPi * ph));
    float xf = pui::freqToX(f, p0.x, p1.x - p0.x);
    dl->AddLine(ImVec2(xf, p0.y + 3), ImVec2(xf, p1.y - 3), K.fill, 2);
    skin::text(dl, ImVec2(p0.x + 5, p0.y + 3), K.graphText, "SWEEP", F.font(7.5f));
  }
  {
    skin::captionWell(dl, F, c2, 155, bw, 45, K, "LFO", F.font(8.5f), 0);
    static const char* shp[2] = {"SINE", "TRI"};
    if (ref) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
    skin::switchSel(a, t, PhsShape, F.p(c2 + 4, 174), F.sz(78, 22), K, shp, 2, true);
    skin::toggle(a, t, PhsManual, F.p(c2 + 86, 174), F.sz(70, 22), K, "MANUAL", F.font(8));
    if (ref) ImGui::PopStyleVar();
  }
  {
    static const char* eng[2] = {"LEGACY", "REFERENCE"};
    skin::text(dl, F.p(c0 + 4, 209), K.textDim, "ENGINE", F.font(8));
    skin::switchSel(a, t, PhsModel, F.p(c0 + 52, 205), F.sz(150, 18), K, eng, 2, true);
  }
  fr.end();
}

// ================================================================ Filter
// Reference panel 500 × 105 (cutoff / resonance / low-pass, band-pass /
// high-pass / x2 / centre); Gridline adds the mode row and a response graph.
const skin::Skin& filterSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x3C4346, 0x464E51, 0xB7E5B3, 0x3F484D, 0xA2B0B7);
    s.pillBg = skin::hex(0x444D53);
    s.valueBg = skin::hex(0x656F74);
    s.valueText = skin::hex(0xB7E5B3);
    s.handle = skin::hex(0x30373A);
    return s;
  }();
  return k;
}

void filterGraph(App& a, const PluginTarget& t, const PluginState& st, ImVec2 p, ImVec2 sz, const skin::Skin& K) {
  const double sr = a.engine.sampleRate();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##filtgraph", sz);
  const bool act = ImGui::IsItemActive();
  skin::graphWell(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), K);
  dl->PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
  pui::drawFreqGrid(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), 24);
  const int mode = ival(st, FltMode);
  const bool classic = ival(st, FltModel) == 1;  // J-26 classic model: approximate view at the host rate
  const double fc = classic ? flref::filterCutHz(val(st, FltCutK, 1), sr) : val(st, FltCutoff, 20000);
  const double reso = classic ? val(st, FltResK, 0.2f) * 0.8 : val(st, FltReso), mix = classic ? 1.0 : val(st, FltMix, 1), gain = val(st, FltGain);
  const bool steep = ival(st, FltSlope) == 1, blend = classic || bval(st, FltBlend);
  std::vector<ImVec2> pts;
  // J-68: the modern model's curve is the engine's own response (complex SVF
  // taps, steep stage, mix), cached until a parameter moves.
  const livegraph::Curve* modern = nullptr;
  if (!classic) {
    const int W = static_cast<int>(sz.x) / 2 + 1;
    modern = &livegraph::memo<livegraph::Curve>(t.key(), 0x464C5452).get(livegraph::Hasher().floats(st.params).d(sr).i(W).value(), [&](livegraph::Curve& o) {
      o.resize(static_cast<size_t>(W));
      for (int i = 0; i < W; ++i) o[static_cast<size_t>(i)] = static_cast<float>(graphmodel::filterResponseDb(st, sr, pui::xToFreq(p.x + 2 * i, p.x, sz.x)));
    });
  }
  for (int x = 0; x < static_cast<int>(sz.x); x += 2) {
    double hz = pui::xToFreq(p.x + x, p.x, sz.x), db;
    if (modern) {
      db = (*modern)[static_cast<size_t>(std::min(static_cast<int>(modern->size()) - 1, x / 2))];
      pts.push_back(ImVec2(p.x + x, p.y + sz.y * 0.5f - static_cast<float>(std::clamp(db, -30.0, 30.0) / 24) * sz.y * 0.5f));
      continue;
    }
    if (mode >= 4 && !blend) {
      dsp::Biquad bq;
      bq.set(mode == 4 ? dsp::Biquad::LowShelf : mode == 5 ? dsp::Biquad::Peak : dsp::Biquad::HighShelf, sr, fc, 0.5 + reso * 9.5, steep ? gain * 0.5 : gain);
      double m = bq.magnitude(hz, sr);
      db = dsp::gainToDb(steep ? m * m : m);
    } else if (blend) {
      double lo = dsp::dbToGain(svfResponseDb(0, fc, reso, hz, sr)), bp = dsp::dbToGain(svfResponseDb(1, fc, reso, hz, sr)),
             hp = dsp::dbToGain(svfResponseDb(2, fc, reso, hz, sr));
      double gl = val(st, FltLPLevel, 1), gb = val(st, FltBPLevel), gh = val(st, FltHPLevel);
      if (classic) gl = flref::filterLevel(gl), gb = flref::filterLevel(gb), gh = flref::filterLevel(gh);
      db = dsp::gainToDb(std::fabs(gl * lo + gb * bp + gh * hp));
      if (classic && steep) db *= 2;
    } else {
      db = svfResponseDb(mode, fc, steep ? reso * 0.75 : reso, hz, sr);
      if (steep) db += svfResponseDb(mode, fc, 0.0, hz, sr);
    }
    db = dsp::gainToDb(1 - mix + mix * dsp::dbToGain(db));
    pts.push_back(ImVec2(p.x + x, p.y + sz.y * 0.5f - static_cast<float>(std::clamp(db, -30.0, 30.0) / 24) * sz.y * 0.5f));
  }
  skin::curveArea(dl, pts.data(), static_cast<int>(pts.size()), p.y + sz.y, K.graphCurve, 2.0f, 0.20f);
  // Cutoff / resonance node, drawn inside the well (the drag maps the whole width).
  ImVec2 node(std::clamp(pui::freqToX(fc, p.x, sz.x), p.x + 9, std::max(p.x + 9, p.x + sz.x - 9)), p.y + sz.y * (0.85f - static_cast<float>(reso) * 0.7f));
  const bool nodeHot = act || ImGui::IsItemHovered();
  if (nodeHot) dl->AddCircleFilled(node, 11, theme::withAlpha(K.fill, 0.22f), 24);
  dl->AddCircleFilled(node, 7, IM_COL32(0, 0, 0, 120), 20);
  dl->AddCircleFilled(node, 6, act ? K.fillPressed : K.fill, 20);
  dl->AddCircle(node, 6, skin::lighten(K.fill, 0.4f), 20, 1.2f);
  if (!act)
    skin::graphHover(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), K, [&](float fx, float) {
      const double hz = pui::xToFreq(p.x + fx * sz.x, p.x, sz.x);
      const size_t i = std::min(pts.size() - 1, static_cast<size_t>(std::max(0.0f, fx * sz.x) / 2));
      const double db = (p.y + sz.y * 0.5f - pts[i].y) / (sz.y * 0.5f) * 24.0;
      char b[48];
      if (hz >= 1000) std::snprintf(b, sizeof b, "%.2f kHz  %+.1f dB", hz / 1000.0, db);
      else std::snprintf(b, sizeof b, "%.0f Hz  %+.1f dB", hz, db);
      return std::string(b);
    });
  dl->PopClipRect();
  if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    ImGuiIO& io = ImGui::GetIO();
    PluginState ns = st;
    const float ry = std::clamp((0.85f - (io.MousePos.y - p.y) / sz.y) / 0.7f, 0.0f, 1.0f);
    if (classic) {
      ns.params[FltCutK] = std::clamp((io.MousePos.x - p.x) / sz.x, 0.0f, 1.0f);
      ns.params[FltResK] = std::min(1.0f, ry / 0.8f);
    } else {
      ns.params[FltCutoff] = static_cast<float>(pui::xToFreq(io.MousePos.x, p.x, sz.x));
      ns.params[FltReso] = ry;
    }
    setPluginState(a, t, ns, "move filter", 0x464C540000000000ull ^ t.key());
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag: cutoff (left/right) and resonance (up/down)");
}

void editorFilterFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = filterSkin();
  constexpr float kW = 500, kH = 262;
  Frame fr(width, kW, kH, 1.6f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const bool classic = ival(st, FltModel) == 1;
  const bool blend = classic || bval(st, FltBlend);
  sliderBox(a, t, classic ? FltCutK : FltCutoff, F, 5, 5, 160, 45, K, "CUTOFF");
  sliderBox(a, t, classic ? FltResK : FltReso, F, 170, 5, 160, 45, K, "RESO");
  if (!blend) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
  sliderBox(a, t, FltLPLevel, F, 335, 5, 160, 45, K, "LOW");
  sliderBox(a, t, FltBPLevel, F, 5, 55, 160, 45, K, "BAND");
  sliderBox(a, t, FltHPLevel, F, 170, 55, 160, 45, K, "HIGH");
  if (!blend) ImGui::PopStyleVar();
  {
    miniWell(dl, F, 335, 55, 77, 45, K, "SLOPE");
    static const char* sl[2] = {"12", "24"};
    skin::switchSel(a, t, FltSlope, F.p(339, 73), F.sz(69, 25), K, sl, 2, true);
    miniWell(dl, F, 417, 55, 77, 45, K, "CENTER");
    skin::toggle(a, t, FltDcBlock, F.p(421, 73), F.sz(69, 25), K, "DC OFF", F.font(8));
  }
  {  // Mode row: filter type cells + blend switch, then gain and mix.
    skin::captionWell(dl, F, 5, 105, 325, 45, K, "MODE", F.font(8.5f), 0);
    static const char* modes[7] = {"LP", "BP", "HP", "NOTCH", "LO SH", "PEAK", "HI SH"};
    if (classic) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
    skin::switchSel(a, t, FltMode, F.p(9, 124), F.sz(214, 23), K, modes, 7, true);
    skin::toggle(a, t, FltBlend, F.p(227, 124), F.sz(47, 23), K, "BLEND", F.font(8));
    if (classic) ImGui::PopStyleVar();
    skin::toggle(a, t, FltModel, F.p(278, 124), F.sz(49, 23), K, "CLASSIC", F.font(7.5f));
    const skin::Module mg = miniWell(dl, F, 335, 105, 77, 45, K, "GAIN");
    skin::wheel(a, t, FltGain, mg.wheelTl, mg.wheelSize, K, skin::WheelBigLine);
    auto dg = describeParam(a.doc.song(), t.pid(FltGain));
    if (dg) skin::valueStrip(dl, ImVec2(mg.wheelTl.x + mg.wheelSize + 2 * F.s, mg.row0.y), mg.row1, K, dg->format(val(st, FltGain)).c_str(), F.font(7.5f));
    const skin::Module mm = miniWell(dl, F, 417, 105, 77, 45, K, "MIX");
    skin::wheel(a, t, FltMix, mm.wheelTl, mm.wheelSize, K, skin::WheelBigLine);
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", val(st, FltMix, 1) * 100);
    skin::valueStrip(dl, ImVec2(mm.wheelTl.x + mm.wheelSize + 2 * F.s, mm.row0.y), mm.row1, K, b, F.font(7.5f));
  }
  filterGraph(a, t, st, F.p(5, 156), F.sz(490, 101), K);
  fr.end();
}

// ================================================================ Freq Shifter
// Reference panel 460 × 220: big frequency wheel, carrier visual and read-out
// on top, range / type selectors, carrier shape and mix wheels along the bottom.
const skin::Skin& freqShiftSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3A4050, 0x333846, 0x363636, 0x7DD2C9, 0x8A8A8A, 0xA0ADB4);
    s.graphBg = skin::hex(0x2F2D2C);
    s.lcdBg = skin::hex(0x363636);
    s.lcdText = skin::hex(0x7DD2C9);
    s.pillBg = skin::hex(0x7DD2C9);
    s.pillText = skin::hex(0x292929);
    return s;
  }();
  return k;
}

void editorFreqShiftFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = freqShiftSkin();
  constexpr float kW = 460, kH = 220;
  Frame fr(width, kW, kH, 1.65f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const int range = ival(st, FsRange);
  const bool ref = ival(st, FsModel) == 1;  // J-26 reference engine: knob positions
  const int shiftParam = ref ? FsFreqPos : range == 1 ? FsShiftWide : range == 2 ? FsShiftBeats : FsShift;
  skin::wheel(a, t, shiftParam, F.p(31, 21), 88 * F.s, K, skin::WheelBigLine | skin::WheelCentered);
  flabel(dl, F, 75, 111, K, "FREQUENCY");
  const double hz = ref ? fsref::shiftHz(val(st, FsFreqPos, 0.5f), ival(st, FsType), range, a.doc.song().tempo)
                 : range == 1 ? val(st, FsShiftWide) : range == 2 ? val(st, FsShiftBeats, 1) * a.doc.song().tempo / 60.0 : val(st, FsShift, 50);
  {  // Carrier visual.
    ImVec2 p0 = F.p(128, 21), p1 = F.p(262, 82);
    skin::graphWell(dl, p0, p1, K);
    {  // J-68: the shifted output's live spectrum behind the carrier (log 20 Hz–20 kHz)
      constexpr int N = 64;
      float db[N];
      pui::spectrum(t.tap(a), t.key() ^ 0x4653ull, N, db, a.engine.sampleRate());
      const float w = (p1.x - p0.x) / N;
      for (int i = 0; i < N; ++i) {
        const float hh = std::clamp((db[i] + 84.0f) / 84.0f, 0.0f, 1.0f) * (p1.y - p0.y - 4);
        if (hh > 0.5f) dl->AddRectFilled(ImVec2(p0.x + w * i, p1.y - 2 - hh), ImVec2(p0.x + w * (i + 1) - 0.5f, p1.y - 2), theme::withAlpha(K.text, 0.13f));
      }
    }
    ImVec2 pts[120];
    const double shp = val(st, FsShapeL, 1), shr = val(st, FsShapeR, 1);
    for (int side = 0; side < 2; ++side) {
      for (int i = 0; i < 120; ++i) {
        double ph = i / 119.0 * 3 + ImGui::GetTime() * std::clamp(hz, -20.0, 20.0) * 0.2 + side * val(st, FsStereo, 0.25f);
        double c = std::cos(2 * dsp::kPi * ph), s = side ? shr : shp;
        if (ref) c *= 0.35 + 0.65 * std::fabs(2 * s - 1);  // sideband blend: 0.5 = both sidebands (beating)
        else c = s >= 0.999 ? c : (c < 0 ? -1 : 1) * std::pow(std::fabs(c), std::max(0.05, s));
        pts[i] = ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * i / 119.0f, (p0.y + p1.y) * 0.5f - static_cast<float>(c) * (p1.y - p0.y) * 0.4f);
      }
      dl->AddPolyline(pts, 120, side ? theme::withAlpha(K.accent2C(), 0.8f) : K.graphCurve, 0, 1.4f);
    }
  }
  {  // Read-out (filled pill) + plugin title.
    char buf[40];
    std::snprintf(buf, sizeof buf, "%+.2f Hz", hz);
    skin::pill(dl, F.p(272, 37), F.p(445, 65), K, buf, F.font(13));
    skin::text(dl, F.p(300, 12), K.textDim, "FREQ SHIFTER", F.font(9));
    static const char* md[2] = {"LEGACY", "REF"};
    skin::switchSel(a, t, FsModel, F.p(372, 8), F.sz(76, 16), K, md, 2, true);
  }
  {
    static const char* rg[3] = {"FINE", "WIDE", "TEMPO"};
    static const char* rgRef[3] = {"200 HZ", "20 KHZ", "TEMPO"};
    skin::switchSel(a, t, FsRange, F.p(146, 88), F.sz(70, 45), K, ref ? rgRef : rg, 3);
    flabel(dl, F, 181, 134, K, "RANGE");
    static const char* ty[3] = {"SHIFTER", "SHIFTER HQ", "RING MOD"};
    skin::switchSel(a, t, FsType, F.p(271, 80), F.sz(121, 51), K, ty, 3);
    flabel(dl, F, 331, 132, K, "TYPE");
  }
  skin::labeledWheel(a, t, FsShapeL, F, 13, 140, 48, K, "SHAPE L");
  skin::text(dl, F.p(70, 158), K.textDim, "L·R", F.font(8));
  skin::labeledWheel(a, t, FsShapeR, F, 98, 140, 48, K, "SHAPE R");
  skin::labeledWheel(a, t, ref ? FsStartRef : FsStartPhase, F, 168, 147, 38, K, "PHASE");
  skin::labeledWheel(a, t, ref ? FsLrPhase : FsStereo, F, 222, 147, 38, K, "L/R");
  skin::labeledWheel(a, t, ref ? FsFeedbackRef : FsFeedback, F, 285, 147, 38, K, "FEED");
  skin::labeledWheel(a, t, ref ? FsMixRef : FsMix, F, 349, 147, 38, K, "MIX");
  skin::labeledWheel(a, t, FsWidth, F, 405, 147, 38, K, "STEREO");
  fr.end();
}

// ================================================================ Graphic EQ
// Reference panel 340 × 150: seven level sliders with frequency tags; Gridline
// adds the response strip above and the layout / width / mix / output column.
const skin::Skin& geqSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x3C4346, 0x656F74, 0xB3C4E5, 0x3F484D, 0xA2B0B7);
    s.handle = skin::hex(0x30373A);
    s.pillBg = skin::hex(0x656F74);
    s.pillText = skin::hex(0x1B1B1B);
    return s;
  }();
  return k;
}

void editorGeqFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = geqSkin();
  constexpr float kW = 470, kH = 216;
  Frame fr(width, kW, kH, 1.6f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const bool classic = ival(st, GeqLayout) == 1;
  const double* freqs = classic ? kGeqFreqsClassic : kGeqFreqs;
  {  // Response strip.
    ImVec2 p0 = F.p(5, 5), p1 = F.p(465, 60);
    skin::graphWell(dl, p0, p1, K);
    pui::drawFreqGrid(dl, p0, p1, 18, false);
    const double sr = a.engine.sampleRate();
    double oct = val(st, GeqBandwidth, 1.27f), qPeak = std::sqrt(std::pow(2.0, oct)) / (std::pow(2.0, oct) - 1.0), mix = val(st, GeqMix, 1);
    dsp::Biquad f[kGeqBands];
    for (int b = 0; b < kGeqBands; ++b)
      f[b].set(b == 0 ? dsp::Biquad::LowShelf : b == kGeqBands - 1 ? dsp::Biquad::HighShelf : dsp::Biquad::Peak, sr, freqs[b],
               b == 0 || b == kGeqBands - 1 ? 0.707 : qPeak, val(st, b));
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    std::vector<ImVec2> pts;
    for (int x = 0; x < static_cast<int>(w); x += 2) {
      double hz = pui::xToFreq(p0.x + x, p0.x, w), m = 1;
      for (auto& bq : f) m *= bq.magnitude(hz, sr);
      double db = dsp::gainToDb(std::fabs(1 + (m - 1) * mix)) + val(st, GeqOutput);
      pts.push_back(ImVec2(p0.x + x, p0.y + h * 0.5f - static_cast<float>(std::clamp(db, -24.0, 24.0) / 18.0) * h * 0.45f));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), K.graphCurve, 0, 1.8f);
    for (int b = 0; b < kGeqBands; ++b)
      dl->AddCircleFilled(ImVec2(pui::freqToX(freqs[b], p0.x, w), p0.y + h * 0.5f - val(st, b) / 18.0f * h * 0.45f), 3.0f, K.fill);
  }
  static const float xs[kGeqBands] = {15, 62, 110, 157, 205, 252, 300};
  for (int b = 0; b < kGeqBands; ++b) {
    skin::vslider(a, t, b, F.p(xs[b], 68), F.sz(29, 123), K);
    char lbl[16];
    std::snprintf(lbl, sizeof lbl, "%g", freqs[b]);
    skin::pill(dl, F.p(xs[b] - 3, 195), F.p(xs[b] + 32, 209), K, lbl, F.font(7.5f));
  }
  {  // Options column.
    skin::well(dl, F.p(345, 66), F.p(465, 210), K, 4 * F.s);
    skin::Skin kp = K;
    kp.pillBg = skin::hex(0x444D53);
    kp.pillText = K.text;
    skin::pill(dl, F.p(345, 66), F.p(465, 82), kp, "BANDS", F.font(8.5f));
    static const char* lay[2] = {"WIDE", "CLASSIC"};
    skin::switchSel(a, t, GeqLayout, F.p(350, 86), F.sz(110, 20), K, lay, 2, true);
    skin::labeledWheel(a, t, GeqBandwidth, F, 353, 114, 34, K, "WIDTH");
    skin::labeledWheel(a, t, GeqMix, F, 405, 114, 34, K, "MIX");
    skin::labeledWheel(a, t, GeqOutput, F, 379, 162, 30, K, "OUTPUT");
  }
  fr.end();
}

// ================================================================ Tuner
// Reference panel 300 × 360: the 300 × 300 display, then reference and
// responsiveness wheels, the display-mode button and the window option.
const skin::Skin& tunerSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x222222, 0x2B2F33, 0x2E3438, 0x8D989E, 0x3F484D, 0xA9B2B7);
    s.graphBg = skin::hex(0x161819);
    s.accent2 = skin::hex(0xFF8000);
    return s;
  }();
  return k;
}

struct TunerState {
  double shown = 0, cents = 0;
};

void editorTunerFl(App& a, const PluginTarget& t, float width) {
  static std::map<uint64_t, TunerState> uiMap;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = tunerSkin();
  TunerState& ui = uiMap[t.key()];
  const bool bass = bval(st, TnBassMode);
  const size_t win = bass ? 8192 : 4096;
  static std::vector<float> buf(8192);
  double hz = 0;
  if (const ScopeTap* tap = t.tap(a); tap && tap->read(buf.data(), win)) {
    double sr = a.engine.sampleRate(), energy = 0;
    for (size_t i = 0; i < win; ++i) energy += buf[i] * buf[i];
    if (energy > 1e-3) {
      int minLag = static_cast<int>(sr / 2000), maxLag = std::min(static_cast<int>(win / 2), static_cast<int>(sr / (bass ? 20 : 50)));
      double best = 0;
      int bestLag = 0;
      for (int lag = minLag; lag < maxLag; ++lag) {
        double c = 0;
        for (size_t i = 0; i + static_cast<size_t>(lag) < win; i += 2) c += buf[i] * buf[i + static_cast<size_t>(lag)];
        if (c > best) best = c, bestLag = lag;
      }
      if (bestLag > 0) hz = sr / bestLag;
    }
  }
  const double k = 0.05 + 0.6 * val(st, TnReactivity, 0.5f);
  ui.shown = hz > 0 ? (ui.shown > 0 ? ui.shown * (1 - k) + hz * k : hz) : ui.shown * 0.98;
  const double ref = val(st, TnReference, 440);
  const int disp = ival(st, TnDisplay);
  constexpr float kW = 300, kH = 362;
  Frame fr(width, kW, kH, 1.3f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  ImVec2 p0 = F.p(4, 4), p1 = F.p(296, 296);
  skin::graphWell(dl, p0, p1, K);
  const float cx = (p0.x + p1.x) * 0.5f, W = p1.x - p0.x;
  const ImU32 good = K.fill, bad = K.accent2C();
  if (ui.shown > 20) {
    double note = 69 + 12 * std::log2(ui.shown / ref);
    int n = static_cast<int>(std::lround(note));
    ui.cents = ui.cents * (1 - k) + (note - n) * 100 * k;
    const bool inTune = std::fabs(ui.cents) < 5;
    std::string nm = noteName(n);
    ImGui::PushFont(a.fontBold, 44.0f * F.s);
    ImVec2 ts = ImGui::CalcTextSize(nm.c_str());
    if (disp == 2) {
      float r = 100 * F.s, cy = F.p(0, 140).y;
      dl->AddCircle(ImVec2(cx, cy), r, K.rail, 64, 4);
      float an = static_cast<float>(-dsp::kPi / 2 + ui.cents / 50.0 * dsp::kPi);
      dl->PathArcTo(ImVec2(cx, cy), r, static_cast<float>(-dsp::kPi / 2), an, 32);
      dl->PathStroke(inTune ? good : bad, ImDrawFlags_None, 5.0f);
      dl->AddText(ImVec2(cx - ts.x / 2, cy - ts.y / 2), inTune ? skin::lighten(K.text, 0.5f) : K.text, nm.c_str());
    } else {
      dl->AddText(ImVec2(cx - ts.x / 2, F.p(0, 30).y), inTune ? skin::lighten(K.text, 0.5f) : K.text, nm.c_str());
      float y = F.p(0, 170).y;
      if (disp == 1) {
        for (int c = -50; c <= 50; c += 5) {
          float x = cx + c / 50.0f * (W / 2 - 20);
          float hh = (c % 25 == 0 ? 16.0f : 8.0f) * F.s;
          dl->AddLine(ImVec2(x, y - hh), ImVec2(x, y + hh), c == 0 ? K.text : K.rail, c == 0 ? 2.f : 1.f);
        }
        float nx = cx + static_cast<float>(ui.cents / 50.0) * (W / 2 - 20);
        dl->AddRectFilled(ImVec2(nx - 4, y - 22 * F.s), ImVec2(nx + 4, y + 22 * F.s), inTune ? good : bad, 2);
      } else {
        for (int c = -50; c <= 50; c += 10) {
          float an = static_cast<float>(-dsp::kPi / 2 + c / 50.0 * 0.9);
          ImVec2 pc(cx, F.p(0, 250).y);
          float r0 = 150 * F.s, r1 = (c % 50 == 0 ? 165.0f : 158.0f) * F.s;
          dl->AddLine(ImVec2(pc.x + std::cos(an) * r0, pc.y + std::sin(an) * r0), ImVec2(pc.x + std::cos(an) * r1, pc.y + std::sin(an) * r1),
                      c == 0 ? K.text : K.textDim, c == 0 ? 2.f : 1.f);
        }
        ImVec2 pc(cx, F.p(0, 250).y);
        float an = static_cast<float>(-dsp::kPi / 2 + std::clamp(ui.cents, -50.0, 50.0) / 50.0 * 0.9);
        dl->AddLine(pc, ImVec2(pc.x + std::cos(an) * 160 * F.s, pc.y + std::sin(an) * 160 * F.s), inTune ? good : bad, 3);
        dl->AddCircleFilled(pc, 6, K.text);
      }
    }
    ImGui::PopFont();
    char b2[64];
    std::snprintf(b2, sizeof b2, "%+.0f cents  ·  %.1f Hz", ui.cents, ui.shown);
    ImGui::PushFont(a.fontMono, F.font(9));
    ImVec2 t2 = ImGui::CalcTextSize(b2);
    dl->AddText(ImVec2(cx - t2.x / 2, p1.y - t2.y - 8), K.textDim, b2);
    ImGui::PopFont();
  } else {
    ImGui::PushFont(a.fontMono, F.font(10));
    const char* msg = "listening...";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(cx - ts.x / 2, (p0.y + p1.y) / 2 - ts.y / 2), K.textDim, msg);
    ImGui::PopFont();
  }
  skin::labeledWheel(a, t, TnReference, F, 9, 302, 42, K, "REF");
  skin::labeledWheel(a, t, TnReactivity, F, 61, 302, 42, K, "REACT");
  static const char* dm[3] = {"NEEDLE", "STRIP", "RING"};
  skin::switchSel(a, t, TnDisplay, F.p(112, 302), F.sz(182, 22), K, dm, 3, true);
  skin::check(a, t, TnBassMode, F.p(112, 330), F.sz(80, 16), K, "BASS", K.accent2C());
  {
    char b[24];
    std::snprintf(b, sizeof b, "A4 = %.1f Hz", ref);
    skin::lcd(dl, F.p(196, 329), F.p(294, 347), K, b, F.font(8.5f));
  }
  fr.end();
}

// ================================================================ Gate
// The reference limiter's gate page on its 626 × 300 panel: history visual on
// top, caption bar, then the settings strip of 46 px wheels (threshold group,
// envelope group in its own bevel, sidechain-key group).
const skin::Skin& gateSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x5F686D, 0x566065, 0x3F484D, 0xCEA245, 0x3F484D, 0xC9D5D4);
    s.graphBg = skin::hex(0x2B3136);
    s.pillBg = skin::hex(0x2B3136);
    s.pillText = skin::hex(0xC9D5D4);
    s.lcdBg = skin::hex(0x31373B);
    s.lcdText = skin::hex(0xCEA245);
    s.accent2 = skin::hex(0x7D908C);
    return s;
  }();
  return k;
}

void editorGateFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = gateSkin();
  float gr = 0, in = -120, open = 0;
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) gr = rt->meter.load(), in = rt->input.load(), open = rt->bands[0].load();
  const uint64_t hk = t.key() ^ 0x4741544500000000ull;
  pui::historyPush(hk, 1, in);
  pui::historyPush(hk, 2, gr);
  constexpr float kW = 626, kH = 300;
  Frame fr(width, kW, kH, 1.5f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  dl->AddRectFilled(F.p(0, 0), F.p(kW, 160), K.graphBg, 3);
  {  // Left strip: gate state LED.
    dl->AddRectFilled(F.p(4, 3), F.p(25, 86), skin::hex(0x262626), 3, ImDrawFlags_RoundCornersBottom);
    bool isOpen = open > 0.5f;
    skin::ledDot(dl, F.p(14.5f, 20), 5 * F.s, isOpen, K);
    skin::text(dl, F.p(6, 32), K.textDim, isOpen ? "ON" : "--", F.font(7));
    skin::meter(dl, F.p(8, 92), F.p(21, 154), K, in, 80);
  }
  {  // History: input level (dim) and reduction; threshold line.
    ImVec2 i0 = F.p(31, 4), i1 = F.p(622, 156);
    pui::drawGrid(dl, i0, i1, 8, 4);
    pui::historyDraw(dl, i0, i1, hk, -80, 0);
    auto yOf = [&](double db) { return i1.y - static_cast<float>(std::clamp((db + 80) / 80, 0.0, 1.0)) * (i1.y - i0.y); };
    float yt = yOf(val(st, GtThreshold, -40));
    dl->AddLine(ImVec2(i0.x, yt), ImVec2(i1.x, yt), K.fill, 1.4f);
    if (val(st, GtHysteresis) > 0.05f) {
      float yh = yOf(val(st, GtThreshold, -40) - val(st, GtHysteresis));
      dl->AddLine(ImVec2(i0.x, yh), ImVec2(i1.x, yh), theme::withAlpha(K.fill, 0.4f), 1.0f);
    }
    skin::text(dl, ImVec2(i0.x + 4, i0.y + 2), K.graphText, "INPUT · THRESHOLD", F.font(7.5f));
  }
  // Caption bar.
  dl->AddRectFilled(F.p(0, 160), F.p(kW, 176), K.graphBg);
  skin::label(dl, F.p(142, 0).x, F.p(0, 162).y, "GATE", K.text, F.font(8.5f));
  skin::label(dl, F.p(372, 0).x, F.p(0, 162).y, "ENVELOPE", K.text, F.font(8.5f));
  skin::label(dl, F.p(544, 0).x, F.p(0, 162).y, "SIDECHAIN KEY", K.text, F.font(8.5f));
  skin::well(dl, F.p(284, 179), F.p(458, 296), K, 4 * F.s);
  dl->AddRectFilled(F.p(284, 179), F.p(458, 296), skin::hex(0x3E474D), 4 * F.s);
  auto w46 = [&](int param, float x, const char* lbl, ImU32 fill = 0) {
    skin::wheel(a, t, param, F.p(x, 184), 46 * F.s, K, skin::WheelBigLine, fill);
    skin::label(dl, F.p(x + 23, 0).x, F.p(0, 233).y, lbl, K.text, F.font(7.5f));
  };
  w46(GtThreshold, 8, "THRES");
  w46(GtRange, 62, "RANGE");
  w46(GtHysteresis, 122, "HYST");
  const ImU32 envFill = skin::hex(0x7D908C);
  w46(GtAttack, 290, "ATT", envFill);
  w46(GtHold, 346, "HOLD", envFill);
  w46(GtRelease, 402, "REL", envFill);
  w46(GtLookahead, 466, "LOOK", envFill);
  w46(GtKeyHpf, 520, "KEY HP", envFill);
  w46(GtKeyLpf, 574, "KEY LP", envFill);
  skin::toggle(a, t, GtFlip, F.p(178, 186), F.sz(98, 24), K, "DUCK", F.font(8.5f));
  skin::text(dl, F.p(180, 213), K.textDim, "inverse gate", F.font(7));
  {  // Level read-outs.
    char b[32];
    std::snprintf(b, sizeof b, "IN %5.1f dB", std::max(in, -99.0f));
    skin::lcd(dl, F.p(8, 254), F.p(170, 271), K, b, F.font(8.5f));
    std::snprintf(b, sizeof b, "GR %5.1f dB", gr);
    skin::lcd(dl, F.p(8, 275), F.p(170, 292), K, b, F.font(8.5f));
  }
  {  // Envelope sketch: attack / hold / release proportions.
    ImVec2 e0 = F.p(292, 250), e1 = F.p(450, 290);
    skin::graphWell(dl, e0, e1, K);
    auto d = [&](int p) { return static_cast<float>(describeParam(a.doc.song(), t.pid(p))->toNormalized(val(st, p))); };
    float w = e1.x - e0.x - 8, at = d(GtAttack), ho = d(GtHold), re = d(GtRelease), tot = at + ho + re + 0.6f;
    float x0 = e0.x + 4, yb = e1.y - 4, yt = e0.y + 5;
    float x1 = x0 + w * 0.15f, x2 = x1 + w * at / tot, x3 = x2 + w * ho / tot + w * 0.15f, x4 = x3 + w * re / tot;
    ImVec2 pts[6] = {ImVec2(x0, yb), ImVec2(x1, yb), ImVec2(x2, yt), ImVec2(x3, yt), ImVec2(x4, yb), ImVec2(e1.x - 4, yb)};
    dl->AddPolyline(pts, 6, K.graphCurve, 0, 1.6f);
  }
  fr.end();
}

// ================================================================ Distortion
// The reference overdrive's 420 × 180 panel (pre band / colour, big pre-amp
// wheel with the x100 pad, post filter / gain) plus a Gridline column for the
// shape modes, transfer curve, threshold, mix and oversampling.
const skin::Skin& distSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x222222, 0x2A2A2A, 0x333333, 0xE32000, 0x444444, 0xB5B5B5);
    s.graphBg = skin::hex(0x161616);
    s.lcdBg = skin::hex(0x181818);
    s.lcdText = skin::hex(0xFF5A3A);
    s.select = skin::hex(0xE32000);
    return s;
  }();
  return k;
}

void editorDistFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = distSkin();
  constexpr float kW = 600, kH = 180;
  Frame fr(width, kW, kH, 1.6f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  skin::wheel(a, t, DstPreBand, F.p(37, 7), 58 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 66, 67, K, "PRE BAND");
  skin::wheel(a, t, DstPreAmount, F.p(102, 14), 32 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 118, 48, K, "AMT");
  skin::wheel(a, t, DstColor, F.p(37, 95), 58 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 66, 155, K, "COLOR");
  skin::wheel(a, t, DstDrive, F.p(147, 27), 120 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 207, 150, K, "DRIVE");
  skin::toggle(a, t, DstExtreme, F.p(262, 153), F.sz(28, 18), K, "x100", F.font(7));
  skin::wheel(a, t, DstTone, F.p(329, 6), 58 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 358, 66, K, "TONE");
  skin::wheel(a, t, DstOutput, F.p(329, 95), 58 * F.s, K, skin::WheelBigLine);
  flabel(dl, F, 358, 155, K, "OUTPUT");
  // Gridline column.
  skin::well(dl, F.p(424, 4), F.p(596, 176), K, 4 * F.s);
  static const char* modes[8] = {"SOFT", "HARD", "FOLD", "TUBE", "FAST A", "FAST B", "DRIVE", "RECT"};
  gridSel(a, t, DstMode, F.p(430, 9), F.sz(160, 36), K, modes, 8, 4);
  const int mode = ival(st, DstMode);
  const double drive = dsp::dbToGain(val(st, DstDrive, 12) + (bval(st, DstExtreme) ? 40 : 0)), thr = dsp::dbToGain(val(st, DstThreshold));
  const double bias = val(st, DstColor) * 0.3;
  auto curve = [&](double x) {
    double v = x * std::min(drive, 40.0) + bias;
    switch (mode) {
      case 1: return std::clamp(v, -thr, thr);
      case 2: return thr * std::sin(v / thr);
      case 3: return v >= 0 ? 1 - std::exp(-v) : -(1 - std::exp(v * 1.4)) / 1.2;
      case 4: return thr * std::tanh(v / thr);
      case 5: {
        double av = std::fabs(v), f = std::fmod(av, 4 * thr), r = f < thr ? f : f < 3 * thr ? 2 * thr - f : f - 4 * thr;
        return v < 0 ? -r : r;
      }
      case 6: return v >= 0 ? std::tanh(v * 1.2) : std::tanh(v * 0.6) * 0.8;
      case 7: return std::fabs(v);
      default: return std::tanh(v);
    }
  };
  {
    ImVec2 p0 = F.p(430, 50), p1 = F.p(512, 132);
    skin::graphWell(dl, p0, p1, K);
    ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddLine(ImVec2(p0.x + 2, c.y), ImVec2(p1.x - 2, c.y), K.graphGrid);
    dl->AddLine(ImVec2(c.x, p0.y + 2), ImVec2(c.x, p1.y - 2), K.graphGrid);
    ImVec2 pts[81];
    for (int i = 0; i <= 80; ++i) {
      double x = -1.2 + 2.4 * i / 80.0, y = std::clamp(curve(x), -1.2, 1.2);
      pts[i] = ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * static_cast<float>((x + 1.2) / 2.4), c.y - static_cast<float>(y / 1.2) * ((p1.y - p0.y) * 0.5f - 3));
    }
    dl->AddPolyline(pts, 81, K.fill, 0, 1.6f);
  }
  skin::labeledWheel(a, t, DstThreshold, F, 532, 50, 28, K, "THRES");
  skin::labeledWheel(a, t, DstMix, F, 532, 92, 28, K, "MIX");
  static const char* os[4] = {"1x", "2x", "4x", "8x"};
  skin::switchSel(a, t, DstOversample, F.p(430, 140), F.sz(82, 30), K, os, 4, true);
  skin::check(a, t, DstDcRemove, F.p(520, 138), F.sz(72, 15), K, "DC");
  skin::check(a, t, DstSoftOut, F.p(520, 156), F.sz(72, 15), K, "SOFT OUT");
  fr.end();
}

// ================================================================ Bitcrusher
// J-104: Rate (hold rate in Hz) is the hero control; Bits second. The display
// runs the engine's own core (engine/Bitcrush.h) on a test wave, so the staircase
// shows exactly what Rate, Bits, the converter filters and dither do. Dragging
// the display sideways sets Rate, up / down sets Bits.
void crushPreview(App& a, const PluginTarget& t, const PluginState& st, ImVec2 p0, ImVec2 sz, double sr) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##crushview", sz);
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
  pui::displayFrame(dl, p0, p1);
  const double rate = val(st, BcRate, 11025), bits = std::clamp(static_cast<double>(val(st, BcBits, 8)), 1.0, 24.0);
  const double aa = val(st, BcAntiAlias), smooth = val(st, BcSmooth), dith = val(st, BcDither), sq = val(st, BcSquarize);
  const double period = crush::periodFor(rate, sr, kBcRateOff), steps = std::exp2(bits - 1);
  constexpr double kWindow = 0.006;  // 6 ms on screen: the number of steps shows the rate
  const int n = std::clamp(static_cast<int>(kWindow * sr), 32, 1200);
  const double f0 = 2.0 / kWindow;  // two cycles of the test wave
  auto wave = [&](int i) {
    const double ph = 2 * 3.14159265358979 * f0 * i / sr;
    return 0.78 * std::sin(ph) + 0.17 * std::sin(3 * ph + 0.6) + 0.05 * std::sin(9 * ph);
  };
  static crush::Crusher core;  // UI thread only
  core.reset();
  core.setFilters(sr, period, aa, smooth);
  static std::vector<float> out;
  out.assign(static_cast<size_t>(n), 0.f);
  for (int pass = 0; pass < 3; ++pass)  // settle the filters on the periodic wave, keep the last pass
    for (int i = 0; i < n; ++i) {
      double l = wave(i), r = l;
      core.frame(l, r, period, steps, sq, dith);
      out[static_cast<size_t>(i)] = static_cast<float>(l);
    }
  const float pad = 10, x0 = p0.x + pad, w = sz.x - 2 * pad, yc = (p0.y + p1.y) * 0.5f, hh = sz.y * 0.5f - 18;
  auto X = [&](double i) { return x0 + static_cast<float>(i / n) * w; };
  auto Y = [&](double v) { return yc - static_cast<float>(std::clamp(v, -1.2, 1.2)) * hh; };
  dl->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);
  // Quantiser levels while they are coarse enough to see.
  if (steps <= 40)
    for (int q = -static_cast<int>(steps); q <= static_cast<int>(steps); ++q) {
      const float y = std::round(Y(q / steps)) + 0.5f;
      dl->AddLine(ImVec2(p0.x + 1, y), ImVec2(p1.x - 1, y), q == 0 ? P.gridBar : theme::withAlpha(P.gridStep, 0.7f));
    }
  else
    dl->AddLine(ImVec2(p0.x + 1, yc), ImVec2(p1.x - 1, yc), P.gridBar);
  // Input wave (thin), then the crushed output as a filled staircase with a bright edge.
  std::vector<ImVec2> in(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = ImVec2(X(i), Y(wave(i)));
  dl->AddPolyline(in.data(), n, theme::withAlpha(P.text, 0.28f), 0, 1.2f);
  const bool stairs = period > 1.5 || steps <= 40;
  std::vector<ImVec2> edge;
  edge.reserve(static_cast<size_t>(n) * 2 + 2);
  for (int i = 0; i < n; ++i) {
    const float y = Y(out[static_cast<size_t>(i)]);
    if (stairs && smooth < 0.02) {
      edge.push_back(ImVec2(X(i), y));
      edge.push_back(ImVec2(X(i + 1), y));
    } else {
      edge.push_back(ImVec2(X(i + 0.5), y));
    }
  }
  {  // area under the output: unsmoothed fills so neighbouring columns leave no seams
    const ImDrawListFlags fl = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (size_t k = 0; k + 1 < edge.size(); ++k) {
      ImVec2 q[4] = {edge[k], edge[k + 1], ImVec2(edge[k + 1].x, yc), ImVec2(edge[k].x, yc)};
      if (std::fabs(q[0].x - q[1].x) > 0.01f) dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.accent, 0.18f));
    }
    dl->Flags = fl;
  }
  dl->AddPolyline(edge.data(), static_cast<int>(edge.size()), theme::withAlpha(P.accent, 0.35f), 0, 4.0f);
  dl->AddPolyline(edge.data(), static_cast<int>(edge.size()), P.text, 0, 1.6f);
  dl->PopClipRect();
  // Captions: holds in the window, levels, the time span.
  char b[64];
  const double holds = kWindow * sr / period;
  if (period <= 1.0) std::snprintf(b, sizeof b, "NO HOLD  \xC2\xB7  %.1f kHz HOST", sr / 1000.0);
  else std::snprintf(b, sizeof b, "%.1f HOLDS / 6 ms  \xC2\xB7  1 PER %.2f SAMPLES", holds, period);
  monoText(dl, ImVec2(p0.x + 10, p0.y + 7), P.textDim, b, 10.0f);
  if (bits < 16) std::snprintf(b, sizeof b, "%.0f LEVELS", std::min(2 * steps + 1, 1e9));
  else std::snprintf(b, sizeof b, "%.1f BITS", bits);
  ImGui::PushFont(app().fontMono, 10.0f);
  const float tw = ImGui::CalcTextSize(b).x;
  ImGui::PopFont();
  monoText(dl, ImVec2(p1.x - 10 - tw, p0.y + 7), P.textDim, b, 10.0f);
  monoText(dl, ImVec2(p0.x + 10, p1.y - 18), hot ? P.textDim : P.textFaint, hot ? "DRAG \xE2\x86\x94 RATE  \xE2\x86\x95 BITS" : "6 ms", 10.0f);
  // Drag: sideways = Rate (log), vertical = Bits.
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    const PluginInfo* info = findPlugin("gl.bitcrush");
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    const double fine = ImGui::GetIO().KeyShift ? 0.125 : 1.0;
    if (info && d.x != 0) {
      const ParamDesc& pd = info->params[BcRate];
      a.doc.setParam(t.pid(BcRate), pd.fromNormalized(std::clamp(pd.toNormalized(rate) + d.x / (sz.x * 1.2) * fine, 0.0, 1.0)), true);
    }
    if (info && d.y != 0) {
      const ParamDesc& pd = info->params[BcBits];
      a.doc.setParam(t.pid(BcBits), pd.fromNormalized(std::clamp(pd.toNormalized(bits) - d.y / (sz.y * 1.5) * fine, 0.0, 1.0)), true);
    }
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag sideways to set the rate, up or down to set the bits (Shift: finer)");
}

// Big centred value text.
void heroText(ImDrawList* dl, float cx, float y, const char* text, ImU32 col, float size) {
  ImGui::PushFont(app().fontMonoBold, size);
  const ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddText(ImVec2(std::round(cx - ts.x * 0.5f), y), col, text);
  ImGui::PopFont();
}

void editorBitcrush(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const double sr = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : 44100.0;
  const PluginInfo* info = findPlugin("gl.bitcrush");
  const float gap = 8, topH = 218;
  const float heroW = 196, bitsW = 150, viewW = width - heroW - bitsW - 2 * gap;
  ImVec2 o = ImGui::GetCursorScreenPos();
  // Hero: Rate.
  pui::beginPanel("RATE", heroW, topH, "HOLD");
  {
    const ImVec2 wp = ImGui::GetWindowPos();
    const float r = 50, cx = wp.x + heroW * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(cx - r, wp.y + 30));
    pui::dial(a, t, BcRate, r);
    const double rate = val(st, BcRate, 11025);
    const std::string txt = info ? info->params[BcRate].format(rate) : "";
    const double period = crush::periodFor(rate, sr, kBcRateOff);
    heroText(dl, cx, wp.y + 30 + 2 * r + 10, txt.c_str(), period > 1.0 ? P.text : P.textDim, 24.0f);
    char b[48];
    if (period > 1.0) std::snprintf(b, sizeof b, "HOST / %.2f", period);
    else std::snprintf(b, sizeof b, "FULL RATE");
    ImGui::PushFont(app().fontMono, 10.0f);
    const float tw = ImGui::CalcTextSize(b).x;
    ImGui::PopFont();
    monoText(dl, ImVec2(std::round(cx - tw * 0.5f), wp.y + 30 + 2 * r + 42), P.textFaint, b, 10.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10, wp.y + topH - 8));
    ImGui::Dummy(ImVec2(1, 1));
  }
  pui::endPanel();
  // Staircase display.
  crushPreview(a, t, st, ImVec2(o.x + heroW + gap, o.y), ImVec2(viewW, topH), sr);
  // Bits (second control) + dither.
  ImGui::SetCursorScreenPos(ImVec2(o.x + heroW + gap + viewW + gap, o.y));
  pui::beginPanel("BITS", bitsW, topH, "DEPTH");
  {
    const ImVec2 wp = ImGui::GetWindowPos();
    const float r = 32, cx = wp.x + bitsW * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(cx - r, wp.y + 30));
    pui::dial(a, t, BcBits, r);
    const double bits = val(st, BcBits, 8);
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", bits);
    heroText(dl, cx, wp.y + 30 + 2 * r + 6, b, P.text, 20.0f);
    const double levels = 2 * std::exp2(bits - 1) + 1;
    if (levels < 1e6) std::snprintf(b, sizeof b, "BITS \xC2\xB7 %.0f LEVELS", levels);
    else std::snprintf(b, sizeof b, "BITS");
    ImGui::PushFont(app().fontMono, 9.5f);
    const float tw = ImGui::CalcTextSize(b).x;
    ImGui::PopFont();
    monoText(dl, ImVec2(std::round(cx - tw * 0.5f), wp.y + 30 + 2 * r + 32), P.textFaint, b, 9.5f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10, wp.y + 30 + 2 * r + 50));
    pui::knobInline(a, t, BcDither, "Dither", bitsW - 20, 13);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10, wp.y + topH - 8));
    ImGui::Dummy(ImVec2(1, 1));
  }
  pui::endPanel();
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + topH + gap));
  // Secondary row: converter filters | character | tone filter | output.
  const float rowH = 132;
  const float wConv = std::floor((width - 3 * gap) * 0.24f), wChar = std::floor((width - 3 * gap) * 0.34f), wOut = std::floor((width - 3 * gap) * 0.16f);
  const float wTone = width - 3 * gap - wConv - wChar - wOut;
  pui::beginPanel("CONVERTERS", wConv, rowH);
  {
    const float c = (wConv - 20) / 2;
    pui::knob(a, t, BcAntiAlias, "Anti-alias", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcSmooth, "Smoothing", pui::KnobMedium, c);
    pui::caption("0% CRUNCH \xC2\xB7 100% CLEAN");
  }
  pui::endPanel();
  ImGui::SameLine(0, gap);
  pui::beginPanel("CHARACTER", wChar, rowH);
  {
    const float c = (wChar - 20) / 4;
    pui::knob(a, t, BcSquarize, "Squarize", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcPunch, "Punch", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcPreserve, "Preserve", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcImpact, "Impact", pui::KnobMedium, c);
    pui::caption("PUNCH KEEPS ONSETS CLEAN");
  }
  pui::endPanel();
  ImGui::SameLine(0, gap);
  pui::beginPanel("TONE", wTone, rowH);
  {
    pui::led(a, t, BcFilterOn, "Filter");
    ImGui::SameLine(0, 10);
    pui::combo(a, t, BcFilterType, wTone - 96);
    const float c = (wTone - 20) / 3;
    pui::knob(a, t, BcFilterFreq, "Freq", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcFilterReso, "Reso", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 8));
    static const char* pos[2] = {"Pre", "Post"};
    pui::segmented(a, t, BcFilterPos, c - 6, pos, 1);
    ImGui::EndGroup();
  }
  pui::endPanel();
  ImGui::SameLine(0, gap);
  pui::beginPanel("OUTPUT", wOut, rowH);
  {
    const float c = (wOut - 20) / 2;
    pui::knob(a, t, BcMix, "Mix", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, BcOutput, "Gain", pui::KnobSmall, c);
  }
  pui::endPanel();
}

// ================================================================ Low Lifter
// Reference panel 680 × 280: band visual on top, settings strip below (pre
// gain, multiband, slope + invert, diffuser, low band, crossover, high band,
// low cut, mix). The visual's right end hosts Gridline's harmonics / boost.
const skin::Skin& lowLiftSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3E4244, 0x2C3338, 0x5C6266, 0xC0C0C0, 0x4E5356, 0xB0A4AC);
    s.graphBg = skin::hex(0x23282C);
    s.lcdBg = skin::hex(0x23282C);
    s.accent2 = skin::hex(0x83EB43);
    return s;
  }();
  return k;
}

void editorLowLiftFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = lowLiftSkin();
  constexpr float kW = 680, kH = 280;
  Frame fr(width, kW, kH, 1.4f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const bool ref = ival(st, LlModel) == 1;  // J-26 reference engine
  const int pXo = ref ? LlXoverHz : LlCrossover, pLc = ref ? LlLowCutHz : LlLowCut;
  {  // Band visual: spectrum, low-cut shade, low band, draggable crossover.
    ImVec2 p = F.p(6, 6), q = F.p(560, 188);
    ImVec2 sz(q.x - p.x, q.y - p.y);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##llvis", sz);
    skin::graphWell(dl, p, q, K);
    const int N = 160;
    float db[N];
    pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
    for (int i = 0; i < N; ++i) {
      float x0 = p.x + sz.x * i / N, x1 = p.x + sz.x * (i + 1) / N;
      float hh = std::clamp((db[i] + 90.0f) / 90.0f, 0.0f, 1.0f) * (sz.y - 4);
      dl->AddRectFilled(ImVec2(x0, q.y - hh), ImVec2(x1, q.y), theme::withAlpha(K.text, 0.12f));
    }
    dl->PushClipRect(p, q, true);
    pui::drawFreqGrid(dl, p, q, 24, true);
    dl->PopClipRect();
    {  // J-68: band responses (low red, high green, total white) from the engine's filters, ±24 dB
      const double sr = a.engine.sampleRate();
      const int W = std::max(2, static_cast<int>(sz.x / 2));
      struct Resp {
        std::vector<float> lo, hi, tot;
      };
      const uint64_t in = livegraph::Hasher().floats(st.params).d(sr).i(W).value();
      const Resp& r = livegraph::memo<Resp>(t.key(), 0x4C4C5250).get(in, [&](Resp& o) {
        o.lo.resize(static_cast<size_t>(W)), o.hi.resize(static_cast<size_t>(W)), o.tot.resize(static_cast<size_t>(W));
        for (int i = 0; i < W; ++i) {
          graphmodel::LowLiftResponse v = graphmodel::lowLiftResponse(st, sr, pui::xToFreq(p.x + sz.x * i / (W - 1), p.x, sz.x));
          auto db = [](double g) { return static_cast<float>(20 * std::log10(std::max(1e-6, g))); };
          o.lo[static_cast<size_t>(i)] = db(v.low), o.hi[static_cast<size_t>(i)] = db(v.high), o.tot[static_cast<size_t>(i)] = db(v.total);
        }
      });
      auto yOf = [&](float db) { return p.y + sz.y * (0.5f - std::clamp(db, -24.0f, 24.0f) / 48.0f); };
      std::vector<ImVec2> pts(static_cast<size_t>(W));
      auto line = [&](const std::vector<float>& v, ImU32 col, float th) {
        for (int i = 0; i < W; ++i) pts[static_cast<size_t>(i)] = ImVec2(p.x + sz.x * i / (W - 1), yOf(v[static_cast<size_t>(i)]));
        dl->AddPolyline(pts.data(), W, col, 0, th);
      };
      dl->PushClipRect(p, q, true);
      line(r.lo, theme::withAlpha(skin::hex(0xD23A48), 0.85f), 1.3f);
      line(r.hi, theme::withAlpha(skin::hex(0x83EB43), 0.85f), 1.3f);
      line(r.tot, theme::withAlpha(K.text, 0.95f), 1.8f);
      dl->PopClipRect();
    }
    float xo = pui::freqToX(val(st, pXo, 120), p.x, sz.x), xc = pui::freqToX(val(st, pLc, 25), p.x, sz.x);
    dl->AddRectFilled(p, ImVec2(xc, q.y), theme::withAlpha(skin::hex(0x000000), 0.4f));
    if (bval(st, LlMultiband, true)) dl->AddRectFilled(ImVec2(xc, p.y), ImVec2(xo, q.y), theme::withAlpha(skin::hex(0x912C2E), 0.25f));
    dl->AddLine(ImVec2(xo, p.y), ImVec2(xo, q.y), K.text, 1.5f);
    skin::text(dl, ImVec2(xo + 5, p.y + 5), K.text, "CROSSOVER", F.font(7.5f));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
      a.doc.setParam(t.pid(pXo), std::clamp(pui::xToFreq(ImGui::GetIO().MousePos.x, p.x, sz.x), ref ? 20.0 : 40.0, ref ? 120.0 : 400.0), true);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Drag to move the crossover");
  }
  {  // Harmonics / bass boost column (reference engine: enable, look-ahead, engine switch).
    skin::well(dl, F.p(566, 6), F.p(674, 188), K, 4 * F.s);
    if (ref) {
      skin::check(a, t, LlOn, F.p(578, 16), F.sz(86, 16), K, "ENABLED", skin::hex(0x83EB43));
      skin::check(a, t, LlLookahead, F.p(578, 40), F.sz(86, 16), K, "LOOKAHEAD", skin::hex(0x83EB43));
    } else {
      skin::labeledWheel(a, t, LlHarmonics, F, 600, 12, 40, K, "HARMONICS");
      skin::labeledWheel(a, t, LlBoost, F, 600, 72, 40, K, "BASS BOOST");
      skin::labeledWheel(a, t, LlBoostFreq, F, 600, 132, 40, K, "BOOST FREQ");
    }
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, LlModel, F.p(574, ref ? 160 : 168), F.sz(92, 16), K, eng, 2, true);
  }
  const float y0 = 200;
  dl->AddRectFilled(F.p(0, y0), F.p(kW, kH), K.wellC());
  auto w = [&](int param, float x, float y, float s, const char* lbl, ImU32 fill = 0) {
    skin::wheel(a, t, param, F.p(x, y0 + y), s * F.s, K, skin::WheelBigLine, fill);
    skin::label(dl, F.p(x + s * 0.5f, 0).x, F.p(0, y0 + y + s + 1).y, lbl, K.textDim, F.font(7.5f));
  };
  w(ref ? LlPreDb : LlDrive, 15, 22, 46, "PRE GAIN");
  skin::check(a, t, LlMultiband, F.p(70, y0 + 30), F.sz(64, 15), K, "MULTI");
  w(ref ? LlSlopeDb : LlSlope, 136, 22, 46, "SLOPE");
  skin::check(a, t, LlInvertHarm, F.p(186, y0 + 40), F.sz(60, 15), K, "INV");
  w(ref ? LlDiffCoef : LlDiffuseAmt, 253, 26, 40, "DIFF");
  w(LlDiffuse, 305, 26, 40, "DIFF MIX");
  skin::check(a, t, LlLowOn, F.p(352, y0 + 8), F.sz(28, 14), K, "", skin::hex(0xFF4E60));
  w(ref ? LlLowDb : LlLowGain, 382, 26, 40, "LOW", skin::hex(0xD23A48));
  w(pXo, 434, 26, 40, "XOVER");
  w(ref ? LlHighDb : LlHighGain, 487, 26, 40, "HIGH", skin::hex(0x83EB43));
  skin::check(a, t, LlHighOn, F.p(529, y0 + 8), F.sz(28, 14), K, "", skin::hex(0x83EB43));
  w(pLc, 564, 26, 40, "LOW CUT");
  w(LlMix, 625, 26, 40, "MIX");
  skin::text(dl, F.p(348, y0 + 24), K.textDim, "ON", F.font(6.5f));
  skin::text(dl, F.p(530, y0 + 24), K.textDim, "ON", F.font(6.5f));
  fr.end();
}

// ================================================================ Analyzer
// Reference visualiser family (592 × 315 panel): the visual fills the top,
// settings sheets below — input (gain, hold pad), view (mode, update speed)
// and spectrum (tilt, range, peaks, stereo).
const skin::Skin& analyzerSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x363C40, 0x3B4144, 0x50A0FF, 0x42474A, 0xC1C7CB);
    s.graphBg = skin::hex(0x101214);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0x9FA8AD);
    s.accent2 = skin::hex(0xFFA600);
    s.graphCurve = skin::hex(0x7DB8FF);
    return s;
  }();
  return k;
}

struct AnzState {
  std::vector<float> sono, peaks, frozen;
  int col = 0;
};

void editorAnalyzerFl(App& a, const PluginTarget& t, float width) {
  static std::map<uint64_t, AnzState> uiMap;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = analyzerSkin();
  AnzState& ui = uiMap[t.key()];
  constexpr float kW = 592, kH = 292;
  Frame fr(width, kW, kH, 1.45f, K);
  const skin::Form& F = fr.F;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const int mode = ival(st, AnzMode, 2);
  const float gain = val(st, AnzGain), range = val(st, AnzRange, 96), tilt = val(st, AnzSlope);
  const bool hold = bval(st, AnzHold);
  constexpr int N = 200;
  float db[N];
  pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
  static const float smooth[3] = {0.0f, 0.5f, 0.85f};
  if (static_cast<int>(ui.frozen.size()) != N) ui.frozen.assign(N, -120), ui.peaks.assign(N, -120);
  const float sm = smooth[std::clamp(ival(st, AnzSmoothing, 1), 0, 2)];
  for (int k = 0; k < N; ++k) {
    double hz = 20.0 * std::pow(1000.0, (k + 0.5) / N);
    float v = db[k] + gain + tilt * static_cast<float>(std::log2(hz / 1000.0));
    size_t kk = static_cast<size_t>(k);
    if (!hold) ui.frozen[kk] = ui.frozen[kk] * sm + v * (1 - sm);
    ui.peaks[kk] = std::max(ui.peaks[kk] - 0.2f, ui.frozen[kk]);
  }
  const float top = 6;
  auto Ydb = [&](float v, float y0, float hh) { return y0 + hh - std::clamp((v - top + range) / range, 0.0f, 1.0f) * hh; };
  auto drawSpectrum = [&](ImVec2 p, ImVec2 sz) {
    skin::graphWell(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), K);
    pui::drawFreqGrid(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), range / 2, false);
    ImVec2 pts[N];
    for (int k = 0; k < N; ++k) pts[k] = ImVec2(p.x + sz.x * (k + 0.5f) / N, Ydb(ui.frozen[static_cast<size_t>(k)], p.y, sz.y));
    for (int k = 0; k + 1 < N; ++k) {
      ImVec2 q[4] = {pts[k], pts[k + 1], ImVec2(pts[k + 1].x, p.y + sz.y), ImVec2(pts[k].x, p.y + sz.y)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(K.fill, 0.16f));
    }
    dl->AddPolyline(pts, N, K.graphCurve, 0, 1.5f);
    if (bval(st, AnzPeaks, true)) {
      for (int k = 0; k < N; ++k) pts[k].y = Ydb(ui.peaks[static_cast<size_t>(k)], p.y, sz.y);
      dl->AddPolyline(pts, N, theme::withAlpha(K.accent2C(), 0.8f), 0, 1.0f);
    }
  };
  auto drawScope = [&](ImVec2 p, ImVec2 sz) {
    skin::graphWell(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), K);
    dl->AddLine(ImVec2(p.x, p.y + sz.y / 2), ImVec2(p.x + sz.x, p.y + sz.y / 2), K.graphGridStrong);
    static std::vector<float> buf(2048);
    if (const ScopeTap* tap = t.tap(a); tap && tap->read(buf.data(), buf.size())) {
      size_t start = 0;
      for (size_t i = 1; i < 1024; ++i)
        if (buf[i - 1] < 0 && buf[i] >= 0) {
          start = i;
          break;
        }
      ImVec2 pts[1024];
      float g = static_cast<float>(dsp::dbToGain(gain));
      for (size_t i = 0; i < 1024; ++i)
        pts[i] = ImVec2(p.x + sz.x * i / 1024.0f, p.y + sz.y / 2 - std::clamp(buf[start + i] * g, -1.0f, 1.0f) * sz.y * 0.45f);
      dl->AddPolyline(pts, 1024, K.graphCurve, 0, 1.5f);
    }
  };
  auto meterVal = [&](int k) {
    if (t.isChannel()) return -120.0f;
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) return rt->bands[k].load();
    return -120.0f;
  };
  const ImVec2 p0 = F.p(7, 7);
  const float dw = F.u(578), H = F.u(204);
  if (mode == 0) {
    drawSpectrum(p0, ImVec2(dw, H));
  } else if (mode == 1) {
    drawScope(p0, ImVec2(dw, H));
  } else if (mode == 2) {
    drawSpectrum(p0, ImVec2(dw, H * 0.62f));
    drawScope(ImVec2(p0.x, p0.y + H * 0.62f + 4), ImVec2(dw, H * 0.38f - 4));
  } else if (mode == 3) {
    const int cols = 120, rows = 64;
    if (static_cast<int>(ui.sono.size()) != cols * rows) ui.sono.assign(static_cast<size_t>(cols * rows), -120.f);
    if (!hold) {
      ui.col = (ui.col + 1) % cols;
      for (int r = 0; r < rows; ++r) ui.sono[static_cast<size_t>(ui.col * rows + r)] = ui.frozen[static_cast<size_t>(r * N / rows)];
    }
    skin::graphWell(dl, p0, ImVec2(p0.x + dw, p0.y + H), K);
    float cw = (dw - 8) / cols, rh = (H - 8) / rows;
    for (int c = 0; c < cols; ++c) {
      int src = (ui.col + 1 + c) % cols;
      for (int r = 0; r < rows; ++r) {
        float v = std::clamp((ui.sono[static_cast<size_t>(src * rows + r)] - top + range) / range, 0.0f, 1.0f);
        if (v < 0.05f) continue;
        ImU32 col = v > 0.75f ? theme::mix(K.fill, K.accent2C(), (v - 0.75f) * 4) : theme::withAlpha(K.fill, v * 0.9f);
        float x = p0.x + 4 + c * cw, y = p0.y + H - 4 - (r + 1) * rh;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw + 0.5f, y + rh + 0.5f), col);
      }
    }
  } else if (mode == 4) {
    skin::graphWell(dl, p0, ImVec2(p0.x + dw, p0.y + H), K);
    const char* labels[3] = {"PEAK L", "PEAK R", "RMS"};
    float vals[3] = {meterVal(0), meterVal(1), meterVal(3)};
    for (int k = 0; k < 3; ++k) {
      float x = p0.x + 40 + k * (dw - 80) / 3;
      skin::meter(dl, ImVec2(x, p0.y + 14), ImVec2(x + 34, p0.y + H - 40), K, vals[k] + gain, range);
      skin::text(dl, ImVec2(x, p0.y + H - 34), K.textDim, labels[k], 10);
      char b[24];
      std::snprintf(b, sizeof b, "%.1f dB", vals[k]);
      skin::text(dl, ImVec2(x, p0.y + H - 20), K.text, b, 10);
    }
  } else {
    skin::graphWell(dl, p0, ImVec2(p0.x + dw, p0.y + H), K);
    float corr = meterVal(2), pl = meterVal(0), pr = meterVal(1);
    float cx = p0.x + dw / 2, y = p0.y + H * 0.35f;
    dl->AddLine(ImVec2(p0.x + 30, y), ImVec2(p0.x + dw - 30, y), K.graphGridStrong, 2);
    dl->AddCircleFilled(ImVec2(cx + corr * (dw / 2 - 30), y), 8, corr < 0 ? skin::hex(0xE5484D) : K.fill);
    skin::text(dl, ImVec2(p0.x + 30, y + 14), K.textDim, "-1  OUT OF PHASE", 10);
    skin::text(dl, ImVec2(p0.x + dw - 110, y + 14), K.textDim, "MONO  +1", 10);
    skin::text(dl, ImVec2(cx - 40, y - 30), K.text, "CORRELATION", 10);
    float bal = std::clamp((pr - pl) / 12.0f, -1.0f, 1.0f), yb = p0.y + H * 0.72f;
    dl->AddLine(ImVec2(p0.x + 30, yb), ImVec2(p0.x + dw - 30, yb), K.graphGridStrong, 2);
    dl->AddCircleFilled(ImVec2(cx + bal * (dw / 2 - 30), yb), 7, K.accent2C());
    skin::text(dl, ImVec2(cx - 26, yb - 26), K.text, "BALANCE", 10);
  }
  // Settings sheets.
  auto sheet = [&](float x, float w, const char* cap) {
    skin::well(dl, F.p(x, 219), F.p(x + w, 287), K, 4 * F.s);
    skin::pill(dl, F.p(x + 3, 221), F.p(x + w - 3, 234), K, cap, F.font(7.5f));
  };
  sheet(7, 116, "INPUT");
  skin::labeledWheel(a, t, AnzGain, F, 13, 238, 34, K, "GAIN");
  skin::toggle(a, t, AnzHold, F.p(55, 239), F.sz(62, 42), K, "HOLD", F.font(9));
  sheet(131, 238, "VIEW");
  static const char* views[6] = {"SPEC", "SCOPE", "BOTH", "SONO", "METER", "STEREO"};
  skin::switchSel(a, t, AnzMode, F.p(135, 238), F.sz(230, 20), K, views, 6, true);
  static const char* sp[3] = {"FAST", "MED", "SLOW"};
  skin::switchSel(a, t, AnzSmoothing, F.p(135, 262), F.sz(150, 19), K, sp, 3, true);
  skin::text(dl, F.p(292, 266), K.textDim, "UPDATE", F.font(7.5f));
  sheet(377, 208, "SPECTRUM");
  skin::labeledWheel(a, t, AnzSlope, F, 383, 238, 32, K, "TILT");
  skin::labeledWheel(a, t, AnzRange, F, 427, 238, 32, K, "RANGE");
  skin::check(a, t, AnzPeaks, F.p(472, 242), F.sz(108, 15), K, "PEAKS", K.accent2C());
  skin::check(a, t, AnzStereo, F.p(472, 262), F.sz(108, 15), K, "STEREO");
  fr.end();
}

}  // namespace

// Entry point (called before the older built-in editors).
bool drawFlEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.flanger") editorFlangerFl(a, t, width);
  else if (ty == "gl.phaser") editorPhaserFl(a, t, width);
  else if (ty == "gl.filter") editorFilterFl(a, t, width);
  else if (ty == "gl.freqshift") editorFreqShiftFl(a, t, width);
  else if (ty == "gl.geq") editorGeqFl(a, t, width);
  else if (ty == "gl.tuner") editorTunerFl(a, t, width);
  else if (ty == "gl.gate") editorGateFl(a, t, width);
  else if (ty == "gl.dist") editorDistFl(a, t, width);
  else if (ty == "gl.bitcrush") editorBitcrush(a, t, width);
  else if (ty == "gl.lowlift") editorLowLiftFl(a, t, width);
  else if (ty == "gl.analyzer") editorAnalyzerFl(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
