// Gridline — dynamics editor dispatch: the FL-form editors in
// PluginEditorsDynFl.cpp, plus the 0.5 Maximizer editor (rack-panel kit).
#include <functional>
#include "PluginEditorsCommon.h"
#include "PluginEditorsDynFl.h"

namespace gl {

using theme::pal;
using namespace pp;

const char* const* names3();

namespace {

using namespace ed;

struct Meters {
  float gr = 0, in = -120, b[4] = {0, 0, 0, 0};
};
Meters meters(App& a, const PluginTarget& t) {
  Meters m;
  if (t.isChannel()) return m;
  if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
    m.gr = rt->meter.load();
    m.in = rt->input.load();
    for (int k = 0; k < 4; ++k) m.b[k] = rt->bands[k].load();
  }
  return m;
}

double compCurveDb(double db, double th, double ratio, double knee) {
  double over = db - th;
  if (knee > 0 && 2 * std::fabs(over) <= knee) return (1.0 / ratio - 1.0) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
  if (2 * over < -knee) return 0;
  return (1.0 / ratio - 1.0) * over;
}

// Square in/out transfer plot (dB), with an optional live input dot.
void transferPlot(ImDrawList* dl, ImVec2 p, float size, double lo, double hi, const std::function<double(double)>& f, float liveIn,
                  const char* caption = nullptr) {
  const auto& P = pal();
  pui::displayFrame(dl, p, ImVec2(p.x + size, p.y + size));
  const float pad = 8;
  auto X = [&](double db) { return p.x + pad + static_cast<float>((db - lo) / (hi - lo)) * (size - pad * 2); };
  auto Y = [&](double db) { return p.y + size - pad - static_cast<float>((std::clamp(db, lo, hi) - lo) / (hi - lo)) * (size - pad * 2); };
  for (double db = std::ceil(lo / 12) * 12; db <= hi; db += 12) {
    dl->AddLine(ImVec2(X(db), p.y + pad), ImVec2(X(db), p.y + size - pad), P.gridStep);
    dl->AddLine(ImVec2(p.x + pad, Y(db)), ImVec2(p.x + size - pad, Y(db)), P.gridStep);
  }
  dl->AddLine(ImVec2(X(lo), Y(lo)), ImVec2(X(hi), Y(hi)), P.lineStrong);
  ImVec2 pts[161];
  for (int i = 0; i <= 160; ++i) {
    double in = lo + (hi - lo) * i / 160.0;
    pts[i] = ImVec2(X(in), Y(f(in)));
  }
  dl->AddPolyline(pts, 161, P.paper, 0, 2.0f);
  if (liveIn > lo) {
    double in = std::min(static_cast<double>(liveIn), hi);
    dl->AddCircleFilled(ImVec2(X(in), Y(f(in))), 5, P.accent);
  }
  monoText(dl, ImVec2(p.x + 10, p.y + 8), P.textFaint, caption ? caption : "OUT");
  monoText(dl, ImVec2(p.x + size - 24, p.y + size - 20), P.textFaint, "IN");
}

// Vertical meter with a caption under it.
void meterCol(ImDrawList* dl, ImVec2 p, float h, float db, bool reduction, const char* cap, float range = 24) {
  pui::levelMeter(dl, p, ImVec2(p.x + 12, p.y + h), db, reduction, range);
  if (cap) monoText(dl, ImVec2(p.x - 2, p.y + h + 3), pal().textFaint, cap, 9.5f);
}

// ======================================================================= Maximizer
void editorMaximizer(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Meters m = meters(a, t);
  int stage = std::clamp(ival(st, MxStage, 3), 0, 3);
  const float topH = 180;
  ImVec2 o = ImGui::GetCursorScreenPos();
  // Top-left: transfer curve of the edited stage.
  if (stage < 3) {
    double th = val(st, MxLowThresh + stage), ratio = val(st, MxLowRatio + stage, 3), g = val(st, MxLowGain + stage);
    int mode = ival(st, MxLowMode + stage);
    transferPlot(dl, o, topH, -48, 6, [&](double in) { return mode == 2 ? -120.0 : in + g + (mode == 0 ? compCurveDb(in + g, th, ratio, 6) : 0.0); }, -120,
                 names3()[stage]);
  } else {
    double ceil = val(st, MxCeiling, -0.3f), drive = val(st, MxDrive, 4);
    transferPlot(dl, o, topH, -48, 6, [&](double in) { return std::min(in + drive, ceil); }, -120, "MASTER");
  }
  // Top-right: spectrum with the band split (drag the split lines).
  ImVec2 sp(o.x + topH + 8, o.y);
  ImVec2 ssz(width - topH - 8, topH);
  ImGui::SetCursorScreenPos(sp);
  ImGui::InvisibleButton("##mxspec", ssz);
  pui::displayFrame(dl, sp, ImVec2(sp.x + ssz.x, sp.y + ssz.y));
  {
    const int N = 160;
    float db[N];
    pui::spectrum(t.tap(a), t.key(), N, db, a.engine.sampleRate());
    ImVec2 pts[N];
    for (int k = 0; k < N; ++k) pts[k] = ImVec2(sp.x + ssz.x * (k + 0.5f) / N, sp.y + ssz.y - std::clamp((db[k] + 90.0f) / 96.0f, 0.0f, 1.0f) * ssz.y);
    for (int k = 0; k + 1 < N; ++k) {
      ImVec2 q[4] = {pts[k], pts[k + 1], ImVec2(pts[k + 1].x, sp.y + ssz.y), ImVec2(pts[k].x, sp.y + ssz.y)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.08f));
    }
    dl->AddPolyline(pts, N, theme::withAlpha(P.text, 0.35f), 0, 1.0f);
  }
  pui::drawFreqGrid(dl, sp, ImVec2(sp.x + ssz.x, sp.y + ssz.y), 24, true);
  float x1 = pui::freqToX(val(st, MxLowFreq, 140), sp.x, ssz.x), x2 = pui::freqToX(val(st, MxHighFreq, 4000), sp.x, ssz.x);
  float xs[4] = {sp.x, x1, x2, sp.x + ssz.x};
  for (int b = 0; b < 3; ++b) {
    if (b == stage) dl->AddRectFilled(ImVec2(xs[b], sp.y + 1), ImVec2(xs[b + 1], sp.y + ssz.y - 1), theme::withAlpha(P.accent, 0.08f));
    float hh = std::min(1.0f, -m.b[b] / 24.0f) * (ssz.y - 30);
    if (hh > 1) dl->AddRectFilled(ImVec2(xs[b] + 6, sp.y + 6), ImVec2(xs[b + 1] - 6, sp.y + 6 + hh), theme::withAlpha(P.danger, 0.35f), 2);
    monoText(dl, ImVec2(xs[b] + 8, sp.y + ssz.y - 30), b == stage ? P.accent : P.textFaint, names3()[b]);
  }
  ImGuiIO& io = ImGui::GetIO();
  static int dragX = -1;
  if (ImGui::IsItemActivated()) {
    dragX = std::fabs(io.MousePos.x - x1) < 8 ? 0 : std::fabs(io.MousePos.x - x2) < 8 ? 1 : -1;
    if (dragX < 0) a.doc.setParam(t.pid(MxStage), io.MousePos.x < x1 ? 0 : io.MousePos.x < x2 ? 1 : 2, false);
  }
  for (int k = 0; k < 2; ++k) dl->AddLine(ImVec2(k ? x2 : x1, sp.y), ImVec2(k ? x2 : x1, sp.y + ssz.y), P.paper, 1.5f);
  if (ImGui::IsItemActive() && dragX >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
    a.doc.setParam(t.pid(dragX ? MxHighFreq : MxLowFreq), pui::xToFreq(io.MousePos.x, sp.x, ssz.x), true);
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Click a band to edit it · drag the split lines to move the crossovers");
  meterCol(dl, ImVec2(sp.x + ssz.x - 22, sp.y + 22), ssz.y - 60, m.gr, true, "GR");
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + topH + 8));
  // Bottom: stage list | stage controls | envelope | global.
  pui::beginPanel("STAGE", 110, 184);
  static const char* stages[4] = {"Low", "Mid", "High", "Master"};
  pui::segmented(a, t, MxStage, 88, stages, 1);
  ImGui::Dummy(ImVec2(0, 2));
  static const char* solo[4] = {"-", "L", "M", "H"};
  pui::caption("SOLO");
  pui::segmented(a, t, MxSolo, 88, solo);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  float rest = width - 110 - 24;
  float w1 = rest * 0.36f, w2 = rest * 0.28f, w3 = rest - w1 - w2;
  pui::beginPanel(stage < 3 ? "BAND" : "MASTER", w1, 184);
  float c = (w1 - 20) / 3;
  if (stage < 3) {
    static const char* modes[3] = {"Compress", "Off", "Mute"};
    pui::segmented(a, t, MxLowMode + stage, w1 - 22, modes);
    pui::knob(a, t, MxLowGain + stage, "Gain", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxLowThresh + stage, "Threshold", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxLowRatio + stage, "Ratio", pui::KnobSmall, c);
    pui::knob(a, t, MxLowWidth + stage, "Width", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxLowSat + stage, "Saturate", pui::KnobSmall, c);
  } else {
    pui::knob(a, t, MxDrive, "Drive", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxCeiling, "Ceiling", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxWidth, "Width", pui::KnobMedium, c);
    pui::knob(a, t, MxAmount, "Amount", pui::KnobSmall, c);
  }
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("ENVELOPE", w2, 184);
  c = (w2 - 20) / 2;
  if (stage < 3) {
    pui::knob(a, t, MxLowAttack + stage, "Attack", pui::KnobMedium, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxLowRel + stage, "Release", pui::KnobMedium, c);
  } else {
    pui::knob(a, t, MxAttack, "Attack", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, MxRelease, "Release", pui::KnobSmall, c);
    pui::knob(a, t, MxRelease2, "Release 2", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 10));
    pui::stepper(a, t, MxRelCurve, c - 8, nullptr);
    pui::caption("CURVE");
    ImGui::EndGroup();
  }
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("BANDS", w3, 184);
  c = (w3 - 20) / 3;
  pui::knob(a, t, MxLowFreq, "Low/Mid", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, MxHighFreq, "Mid/High", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, MxLowCut, "Low cut", pui::KnobSmall, c);
  pui::knob(a, t, MxLookahead, "Look-ahead", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, MxBandMix, "Band mix", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(0, 8));
  static const char* slope[2] = {"12", "24"};
  pui::segmented(a, t, MxSlope, c - 6, slope);
  pui::caption("dB/OCT");
  ImGui::EndGroup();
  pui::endPanel();
}

}  // namespace

const char* const* names3() {
  static const char* n[3] = {"LOW", "MID", "HIGH"};
  return n;
}

bool drawDynamicsEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (drawDynamicsFlEditor(a, t, width)) return true;
  if (ty != "gl.maximizer") return false;
  editorMaximizer(a, t, width);
  return true;
}

}  // namespace gl
