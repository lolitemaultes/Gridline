// Gridline — Shaper editor (drawn envelopes + filter), 0.5 rack-panel style.
#include <map>
#include "PluginEditorsCommon.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

void knobs(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, const char*>> ks, float width, int style = pui::KnobSmall) {
  float c = width / static_cast<float>(ks.size());
  bool first = true;
  for (auto [k, l] : ks) {
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    pui::knob(a, t, k, l, style, c);
  }
}

// ======================================================================= Shaper (drawn envelopes + filter)
struct ShaperUi {
  int lane = 0;  // 0 volume, 1 cutoff, 2 pan
};
std::map<uint64_t, ShaperUi> shaperUi;

void stepLane(App& a, const PluginTarget& t, const PluginState& st, int base, bool bipolar, ImVec2 sz, float playPh) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(base);
  ImGui::InvisibleButton("##steps", sz);
  pui::displayFrame(dl, p, ImVec2(p.x + sz.x, p.y + sz.y));
  float cw = sz.x / kShaperPoints;
  float mid = bipolar ? p.y + sz.y / 2 : p.y + sz.y;
  double tension = val(st, ShTension);
  for (int k = 0; k < kShaperPoints; ++k) {
    float v = val(st, base + k, bipolar ? 0.f : 1.f);
    float x0 = p.x + k * cw;
    float y = bipolar ? p.y + sz.y / 2 - v * (sz.y / 2 - 3) : p.y + sz.y - v * (sz.y - 4);
    dl->AddRectFilled(ImVec2(x0 + 1.5f, std::min(y, mid)), ImVec2(x0 + cw - 1.5f, std::max(y, mid)), theme::withAlpha(P.text, 0.25f + 0.35f * std::fabs(v)), 2);
    if (k % 4 == 0) dl->AddLine(ImVec2(x0, p.y), ImVec2(x0, p.y + sz.y), P.gridBar);
  }
  // The curve as played (tension interpolates between steps).
  ImVec2 pts[161];
  for (int i = 0; i <= 160; ++i) {
    double f = i / 160.0 * kShaperPoints;
    int k = std::min(kShaperPoints - 1, static_cast<int>(f));
    double a0 = val(st, base + k, bipolar ? 0.f : 1.f), v = a0;
    if (std::fabs(tension) > 1e-4) {
      double b = val(st, base + (k + 1) % kShaperPoints, bipolar ? 0.f : 1.f);
      v = a0 + (b - a0) * std::pow(f - k, std::pow(2.0, -tension * 3.0));
    }
    float y = bipolar ? p.y + sz.y / 2 - static_cast<float>(v) * (sz.y / 2 - 3) : p.y + sz.y - static_cast<float>(v) * (sz.y - 4);
    pts[i] = ImVec2(p.x + sz.x * i / 160.0f, y);
  }
  dl->AddPolyline(pts, 161, P.paper, 0, 1.6f);
  if (playPh >= 0) dl->AddLine(ImVec2(p.x + playPh * sz.x, p.y), ImVec2(p.x + playPh * sz.x, p.y + sz.y), P.accent, 1.5f);
  if (ImGui::IsItemActive()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    int k = std::clamp(static_cast<int>((m.x - p.x) / cw), 0, kShaperPoints - 1);
    float v = bipolar ? std::clamp(1 - (m.y - p.y) / sz.y * 2, -1.0f, 1.0f) : std::clamp(1 - (m.y - p.y) / sz.y, 0.0f, 1.0f);
    PluginState ns = st;
    ns.params[static_cast<size_t>(base + k)] = v;
    setPluginState(a, t, ns, "draw envelope", 0x5348000000000000ull ^ t.key() ^ static_cast<uint64_t>(base));
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Draw: drag across the steps");
  ImGui::PopID();
}

void editorShaper(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ShaperUi& ui = shaperUi[t.key()];
  float right = 178, main = width - right - 8;
  ImGui::BeginGroup();
  // Unit strip: IN | FILTER | DRIVE | OUT (the reference's per-unit row).
  float wIn = main * 0.12f, wF = main * 0.46f, wD = main * 0.2f, wO = main - wIn - wF - wD - 24;
  const float uh = 150;
  pui::beginPanel("IN", wIn, uh);
  pui::knob(a, t, ShInput, "Input", pui::KnobSmall, wIn - 20);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("FILTER", wF, uh);
  static const char* ft[9] = {"Off", "LP", "BP", "HP", "Nch", "LSh", "Pk", "HSh", "AP"};
  pui::segmented(a, t, ShFilter, wF - 22, ft);
  float c = (wF - 20) / 5;
  pui::knob(a, t, ShCutMin, "Min", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShCutMax, "Max", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShReso, "Res / BW", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShShelfGain, "Gain", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShCutDepth, "Env amt", pui::KnobSmall, c);
  static const char* sl[3] = {"Single", "Dual", "Triple"};
  pui::segmented(a, t, ShSlope, wF - 22, sl);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("DRIVE", wD, uh);
  knobs(a, t, {{ShDrive, "Drive"}, {ShDriveMix, "Mix"}}, wD - 20);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("OUT", wO, uh);
  knobs(a, t, {{ShOutput, "Output"}, {ShMix, "Mix"}}, wO - 20);
  pui::endPanel();
  // Envelope area: target tabs + step editor.
  pui::beginPanel("ARTICULATOR", main);
  const char* tabs[3] = {"Volume", "Cutoff", "Pan"};
  for (int k = 0; k < 3; ++k) {
    if (k) ImGui::SameLine(0, 2);
    ImGui::PushID(k);
    if (ui::TextButton(tabs[k], ui.lane == k, 90)) ui.lane = k;
    ImGui::PopID();
  }
  ImGui::SameLine(0, 16);
  static const char* len[5] = {"1/4", "1/2", "1 bar", "2 bars", "4 bars"};
  pui::segmented(a, t, ShLength, 250, len);
  ImGui::SameLine(0, 10);
  pui::knobInline(a, t, ShTension, "Curve", 110, 11);
  float playPh = -1;
  if (a.engine.playing()) {
    static const double beats[] = {1, 2, 4, 8, 16};
    double lenB = beats[std::clamp(ival(st, ShLength, 2), 0, 4)];
    double beat = a.engine.position() / std::max(1, a.doc.song().ppq);
    playPh = static_cast<float>(std::fmod(beat, lenB) / lenB);
  }
  int base = ui.lane == 0 ? ShVol : ui.lane == 1 ? ShCut : ShPan;
  stepLane(a, t, st, base, ui.lane == 2, ImVec2(main - 22, 150), playPh);
  pui::endPanel();
  ImGui::EndGroup();
  ImGui::SameLine(0, 8);
  pui::beginPanel("MODULATION", right, uh + 8 + 230);
  pui::caption("FOLLOWER");
  knobs(a, t, {{ShFollow, "Amount"}, {ShFollowAtt, "Attack"}}, right - 20);
  pui::knob(a, t, ShFollowRel, "Release", pui::KnobSmall, (right - 20) / 2);
  ImGui::Dummy(ImVec2(0, 4));
  pui::caption("LFO TO CUTOFF");
  knobs(a, t, {{ShLfoAmt, "Amount"}, {ShLfoRate, "Rate"}}, right - 20);
  static const char* lsh[4] = {"Sin", "Tri", "Saw", "Sq"};
  pui::segmented(a, t, ShLfoShape, right - 22, lsh);
  pui::knob(a, t, ShSmooth, "Smooth", pui::KnobSmall, (right - 20) / 2);
  pui::endPanel();
}

}  // namespace

bool drawMiscEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type != "gl.shaper") return false;
  editorShaper(a, t, width);
  return true;
}

}  // namespace gl
