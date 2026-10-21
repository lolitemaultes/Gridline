// Gridline — "spectral" pack: editors.
// Layouts follow the proportions of the classic stock plugins (band display
// under the vocoder controls, XY pad + sliders for the pitch shifter, pitch
// meter + keyboard for the corrector, platter for the scratcher, band graph for
// the splitter, stereo field for the spreader), drawn in Gridline's style.
#include "../PacksUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>
#include "../../app/App.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../engine/Program.h"
#include "spectral.h"
#include "spectral_blocks.h"
#include "../spacefx/sfx_skin.h"

namespace gl::packs::spectral {

namespace {

using theme::pal;

float val(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }

// Live effect instance of a mixer slot (keeps the runtime alive while drawing).
struct Live {
  std::shared_ptr<EffectRuntime> rt;
  const SpectralFx* fx = nullptr;
  float v(int slot) const { return fx ? fx->tel.v[slot].load(std::memory_order_relaxed) : 0.f; }
};
Live live(App& a, const PluginTarget& t) {
  Live l;
  if (t.isChannel()) return l;
  l.rt = a.builder.effectRuntime(t.owner, t.slot);
  if (l.rt) l.fx = dynamic_cast<const SpectralFx*>(l.rt->fx.get());
  return l;
}

void setP(App& a, const PluginTarget& t, int k, double v, bool merge) { a.doc.setParam(t.pid(k), v, merge); }

void mono(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text, float size = 10.0f) {
  ImGui::PushFont(app().fontMono, size);
  dl->AddText(p, col, text);
  ImGui::PopFont();
}
float monoWidth(const char* text, float size = 10.0f) {
  ImGui::PushFont(app().fontMono, size);
  float w = ImGui::CalcTextSize(text).x;
  ImGui::PopFont();
  return w;
}

// Choice parameter as a dropdown.
void choiceCombo(App& a, const PluginTarget& t, int k, float width) {
  const Song& s = a.doc.song();
  auto d = describeParam(s, t.pid(k));
  if (!d) return;
  int cur = static_cast<int>(getParam(s, t.pid(k)).value_or(0) + 0.5);
  ImGui::PushID(k);
  ImGui::SetNextItemWidth(width);
  const char* label = cur >= 0 && static_cast<size_t>(cur) < d->choices.size() ? d->choices[static_cast<size_t>(cur)].c_str() : "";
  if (ImGui::BeginCombo("##c", label)) {
    for (size_t i = 0; i < d->choices.size(); ++i)
      if (ImGui::Selectable(d->choices[i].c_str(), static_cast<int>(i) == cur)) setP(a, t, k, static_cast<double>(i), false);
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) a.setHint(d->name + ": " + d->format(cur));
  ImGui::PopID();
}

void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) { dl->AddRectFilled(p0, p1, pal().surface950, 8); }

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
void noteText(char* buf, size_t n, double midi) {
  int k = static_cast<int>(std::floor(midi + 0.5));
  int cents = static_cast<int>(std::lround((midi - k) * 100));
  std::snprintf(buf, n, "%s%d %+d ct", kNoteNames[((k % 12) + 12) % 12], k / 12 - 1, cents);
}

// ================================================================== Vocoder
double bandFreqT(double t, int n, double lo, double hi, double spacing) {
  // Continuous version of vocBandFreq (t = 0..1 across the bands).
  (void)n;
  if (hi < lo * 1.05) hi = lo * 1.05;
  double logF = lo * std::pow(hi / lo, t);
  if (spacing >= 0) return logF + (lo + (hi - lo) * t - logF) * spacing;
  double t2 = std::pow(t, 1.0 - spacing);
  return logF + (lo * std::pow(hi / lo, t2) - logF) * (-spacing);
}

// Reference vocoder panel 593 × 603 (Gridline: 593 × 532): level wheel and
// sliders + routing on top; envelope, band and carrier groups in the middle;
// the band display where the reference has its envelope editor; the carrier
// keyboard at the bottom.
const skin::Skin& vocSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2B3D3D, 0x243333, 0x2D3437, 0x62C1DC, 0x3F484D, 0xB1BEC0);
    s.fillPressed = skin::hex(0x72C8E0);
    s.handle = skin::hex(0x35373C);
    s.select = skin::hex(0x61D2F3);
    s.lcdBg = skin::hex(0x162222);
    s.lcdText = skin::hex(0xDAF0FF);
    s.graphBg = skin::hex(0x172424);
    s.graphGrid = skin::hex(0x223333);
    s.graphGridStrong = skin::hex(0x355050);
    s.pillBg = skin::hex(0x293F41);
    s.accent2 = skin::hex(0xFE8025);
    return s;
  }();
  return k;
}

void editorVocoderLegacy(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const skin::Skin& K = vocSkin();
  constexpr float kW = 593, kH = 532;
  skin::Form F = skin::Form::fit(width, kW);
  F.o.y -= F.u(24);  // the reference's top strip (title / options) is not reproduced
  ImDrawList* dlt = ImGui::GetWindowDrawList();
  skin::panel(dlt, F.p(0, 24), F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  const ImU32 orange = skin::hex(0xFFA830), env = skin::hex(0x963D63), band = skin::hex(0x32A67C), car = skin::hex(0x33A9C0);
  // Levels.
  box(dlt, F, 4, 30, 236, 122, K, "LEVELS", 16);
  W(a, t, VcWetLevel, F, 12, 52, 76, K, "WET");
  V(a, t, VcModLevel, F, 104, 52, 16, 70, K, "MOD", orange);
  V(a, t, VcCarLevel, F, 134, 52, 16, 70, K, "CAR", orange);
  V(a, t, VcFlatten, F, 164, 52, 16, 70, K, "FLAT", orange);
  V(a, t, VcSibilance, F, 194, 52, 16, 70, K, "SIB", orange);
  // Routing.
  box(dlt, F, 244, 30, 345, 122, K, "ROUTING", 16);
  text(dlt, F, 252, 52, "MODULATOR", K.textDim);
  {
    static const char* ms[2] = {"PAIR", "SIDECHAIN"};
    sel(a, t, VcModSource, F, 252, 64, 150, 20, K, true, ms);
  }
  text(dlt, F, 252, 94, "MOD CHANNEL", K.textDim);
  {
    static const char* sw[2] = {"MOD L", "MOD R"};
    skin::switchSel(a, t, VcSwap, F.p(252, 106), F.sz(150, 20), K, sw, 2, true);
  }
  text(dlt, F, 418, 52, "LISTEN", K.textDim);
  {
    static const char* li[3] = {"WET", "MOD", "CAR"};
    sel(a, t, VcListen, F, 418, 64, 162, 20, K, true, li);
  }
  chk(a, t, VcInvert, F, 418, 100, 80, 16, K, "INVERT", K.select);
  chk(a, t, VcHold, F, 500, 100, 80, 16, K, "HOLD", K.select);
  {
    static const char* md[2] = {"LEGACY", "REFERENCE"};
    sel(a, t, VxModel, F, 418, 126, 162, 18, K, true, md);
  }
  // Envelope.
  box(dlt, F, 4, 156, 150, 134, K, "ENVELOPE", 16);
  W(a, t, VcAttack, F, 16, 186, 48, K, "ATTACK", skin::WheelBigLine, env);
  W(a, t, VcRelease, F, 86, 186, 48, K, "RELEASE", skin::WheelBigLine, env);
  // Bands.
  box(dlt, F, 158, 156, 214, 134, K, "BANDS", 16);
  menu(a, t, VcBands, F, 166, 182, 62, 20, K);
  cap(dlt, F, 197, 204, "COUNT", K.textDim);
  {
    static const char* ol[3] = {"12", "24", "36"};
    sel(a, t, VcOrder, F, 166, 230, 62, 20, K, true, ol);
    cap(dlt, F, 197, 252, "SLOPE", K.textDim);
  }
  W(a, t, VcLowFreq, F, 238, 178, 38, K, "LOW", skin::WheelBigLine, band);
  W(a, t, VcHighFreq, F, 282, 178, 38, K, "HIGH", skin::WheelBigLine, band);
  W(a, t, VcSpacing, F, 326, 178, 38, K, "SPACE", skin::WheelBigLine, band);
  W(a, t, VcBandwidth, F, 260, 234, 38, K, "WIDTH", skin::WheelBigLine, band);
  W(a, t, VcFormant, F, 306, 234, 38, K, "FORMANT", skin::WheelBigLine, band);
  // Carrier.
  box(dlt, F, 376, 156, 213, 134, K, "CARRIER", 16);
  const bool internal = val(st, VcCarrier) >= 0.5f;
  menu(a, t, VcCarrier, F, 384, 180, 118, 20, K);
  ImGui::BeginDisabled(!internal);
  menu(a, t, VcChord, F, 384, 206, 118, 20, K);
  W(a, t, VcRoot, F, 518, 178, 44, K, "NOTE", skin::WheelBigLine, car);
  ImGui::EndDisabled();
  W(a, t, VcSpread, F, 400, 236, 34, K, "SPREAD", skin::WheelBigLine, car);
  if (!internal) cap(dlt, F, 520, 252, "INPUT CARRIER", K.textDim);
  // Carrier keyboard (built-in carrier root note).
  {
    bool lit[128] = {};
    const int root = static_cast<int>(std::lround(val(st, VcRoot)));
    if (root >= 0 && root < 128) lit[root] = true;
    int n = skin::keyboard("##vockb", F.p(4, 474), F.sz(585, 54), 24, 84, K, lit, -1, -1, root);
    if (n >= 0) setP(a, t, VcRoot, n, false);
    if (ImGui::IsItemHovered()) a.setHint("Carrier root note for the built-in carrier (click a key)");
  }
  (void)P;
  // ---------------- band display ----------------
  ImVec2 p = F.p(4, 296);
  ImVec2 sz = F.sz(585, 170);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##bands", sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p.x + sz.x, p.y + sz.y);
  panel(dl, p, p1);
  const float padL = 8, padR = 34, top = p.y + 10, bot = p.y + sz.y - 18;
  const float gx = p.x + padL, gw = sz.x - padL - padR;
  for (double dec : {100.0, 1000.0, 10000.0})
    for (int m = 1; m <= 9; ++m) {
      double f = dec * m;
      if (f < 20 || f > 20000) continue;
      float x = pui::freqToX(f, gx, gw);
      dl->AddLine(ImVec2(x, top), ImVec2(x, bot), m == 1 ? P.gridBar : P.gridStep);
      if (m == 1 || m == 2 || m == 5) {
        char buf[16];
        std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
        mono(dl, ImVec2(x + 2, bot + 2), P.textFaint, buf);
      }
    }
  const int nb = vocBandCount(val(st, VcBands));
  const double lo = val(st, VcLowFreq), hi = val(st, VcHighFreq), spc = val(st, VcSpacing);
  const int count = lv.fx ? std::min(nb, lv.fx->tel.count.load(std::memory_order_relaxed)) : 0;
  auto levelY = [&](float db) { return bot - std::clamp((db + 72.0f) / 72.0f, 0.0f, 1.0f) * (bot - top); };
  for (int b = 0; b < nb; ++b) {
    double f = vocBandFreq(b, nb, lo, hi, spc);
    float x = pui::freqToX(f, gx, gw);
    double fn = vocBandFreq(std::min(nb - 1, b + 1), nb, lo, hi, spc), fp = vocBandFreq(std::max(0, b - 1), nb, lo, hi, spc);
    float half = std::clamp((pui::freqToX(fn, gx, gw) - pui::freqToX(fp, gx, gw)) * 0.2f, 1.0f, 9.0f);
    dl->AddLine(ImVec2(x, bot), ImVec2(x, bot - 4), P.lineStrong);
    if (b < count) {
      float ym = levelY(lv.fx->tel.a[b].load(std::memory_order_relaxed));
      float yo = levelY(lv.fx->tel.b[b].load(std::memory_order_relaxed));
      dl->AddRectFilled(ImVec2(x - half, ym), ImVec2(x + half, bot), theme::withAlpha(P.text, 0.14f), 2);
      dl->AddRectFilled(ImVec2(x - half * 0.55f, yo), ImVec2(x + half * 0.55f, bot), theme::withAlpha(P.paper, 0.85f), 1);
    }
  }
  // Contour curve (dB, right axis -24..+12) with 8 draggable points.
  auto contourY = [&](double db) { return top + static_cast<float>((12.0 - db) / 36.0) * (bot - top); };
  for (double db : {0.0, -12.0}) {
    float y = contourY(db);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%+.0f", db);
    mono(dl, ImVec2(p1.x - padR + 6, y - 6), P.textFaint, buf);
  }
  float pts[kVocContour];
  for (int k = 0; k < kVocContour; ++k) pts[k] = val(st, VcContour0 + k);
  std::vector<ImVec2> line;
  for (int i = 0; i <= 96; ++i) {
    double tt = i / 96.0;
    line.push_back(ImVec2(pui::freqToX(bandFreqT(tt, nb, lo, hi, spc), gx, gw), contourY(vocContourDb(pts, tt))));
  }
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), theme::withAlpha(P.accent, 0.9f), 0, 1.6f);
  ImVec2 handle[kVocContour];
  for (int k = 0; k < kVocContour; ++k) {
    double tt = static_cast<double>(k) / (kVocContour - 1);
    handle[k] = ImVec2(pui::freqToX(bandFreqT(tt, nb, lo, hi, spc), gx, gw), contourY(pts[k]));
  }
  static int drag = -1;
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  float bestD = 14;
  for (int k = 0; k < kVocContour; ++k) {
    float d = std::fabs(io.MousePos.x - handle[k].x);
    if (ImGui::IsItemHovered() && d < bestD) {
      bestD = d;
      hover = k;
    }
  }
  if (ImGui::IsItemActivated()) drag = hover;
  for (int k = 0; k < kVocContour; ++k) {
    bool hot = k == hover || (drag == k && ImGui::IsItemActive());
    dl->AddCircleFilled(handle[k], hot ? 5.5f : 4.0f, hot ? P.paper : P.accent);
  }
  if (ImGui::IsItemActive() && drag >= 0) {
    double db = 12.0 - (io.MousePos.y - top) / (bot - top) * 36.0;
    setP(a, t, VcContour0 + drag, std::clamp(db, -24.0, 12.0), true);
    char buf[64];
    std::snprintf(buf, sizeof buf, "Contour %d: %+.1f dB", drag + 1, std::clamp(db, -24.0, 12.0));
    a.setHint(buf);
  }
  if (ImGui::IsItemDeactivated()) {
    drag = -1;
    a.doc.endGesture();
  }
  if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
    a.setHint("Bars: modulator (grey) and vocoded output (white) per band. Drag the points to shape the band levels.");
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hover >= 0) setP(a, t, VcContour0 + hover, 0.0, false);
  // Legend + unvoiced indicator.
  char buf[48];
  std::snprintf(buf, sizeof buf, "%d BANDS", nb);
  mono(dl, ImVec2(p.x + 10, p.y + 6), P.textDim, buf);
  float unv = lv.v(TvUnvoiced);
  const char* uv = "UNVOICED";
  float uw = monoWidth(uv);
  mono(dl, ImVec2(p1.x - padR - uw - 6, p.y + 6), unv > 0.5f ? P.accent : P.textFaint, uv);

  F.end(kW, kH);
}

// Reference model (J-1) on the reference panel (593 x 603): inputs and
// options on top, level wheel + sliders + pass-through filters, contour
// switches and a band meter; envelope and band groups; the per-band curve
// editor (13 targets: points or per-band drawing, pitch follow); carrier
// tone, voices and keyboard at the bottom.
void vxValueText(char* buf, size_t n, int m, double y) {
  switch (m) {
    case VmGain: std::snprintf(buf, n, "%+.1f dB", 20 * std::log10(std::max(1e-6, vxMapGain(y)))); break;
    case VmPass: std::snprintf(buf, n, "%+.1f dB", 20 * std::log10(std::max(1e-6, 0.5 * vxMapGain(y)))); break;
    case VmPan: std::snprintf(buf, n, "%+.0f%%", (2 * y - 1) * 100); break;
    case VmOffset: std::snprintf(buf, n, "%+.4f", vxMapOffset(y)); break;
    case VmNoise: std::snprintf(buf, n, "%.4f", vxMapNoise(y)); break;
    case VmHold: case VmAttack: case VmRelease: std::snprintf(buf, n, "x%.3f", vxMapTime(y)); break;
    case VmDist: std::snprintf(buf, n, "%.0f Hz", vxScaleHz(y)); break;
    case VmWidth: std::snprintf(buf, n, "x%.2f", vxMapWidth(y)); break;
    case VmPitch: std::snprintf(buf, n, "%+.2f oct", -2 * (y - 0.5)); break;
    default: std::snprintf(buf, n, "%.0f%%", y * 100); break;
  }
}

// Curve editor over band position (x 0..1) for one target.
void vxCurveEditor(App& a, const PluginTarget& t, const PluginState& st, const Live& lv, ImVec2 p, ImVec2 sz, const skin::Skin& K, int mode) {
  const auto& P = pal();
  const int m = std::clamp(static_cast<int>(val(st, VxEditTarget) + 0.5f), 0, kVxMaps - 1);
  const int nb = std::clamp(static_cast<int>(std::lround(val(st, VxBands))), 5, kVxMaxBands);
  VxData d;
  vxParse(st.data, d);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##vxcurve", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p.x + sz.x, p.y + sz.y);
  dl->AddRectFilled(p, p1, K.graphBg, 3);
  const float pad = 8, gx = p.x + pad, gw = sz.x - 2 * pad, top = p.y + 8, bot = p1.y - 14;
  auto X = [&](double x) { return gx + static_cast<float>(x) * gw; };
  auto Y = [&](double y) { return bot - static_cast<float>(y) * (bot - top); };
  for (double g : {0.25, 0.5, 0.75}) dl->AddLine(ImVec2(gx, Y(g)), ImVec2(gx + gw, Y(g)), g == 0.5 ? K.graphGridStrong : K.graphGrid);
  // Band positions + live levels (modulator grey, output light).
  const int count = lv.fx ? std::min(nb, lv.fx->tel.count.load(std::memory_order_relaxed)) : 0;
  const float bwid = std::max(1.0f, gw / static_cast<float>(nb) * 0.35f);
  for (int b = 0; b < nb; ++b) {
    float x = X(static_cast<double>(b) / (nb - 1));
    dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4), K.graphGridStrong);
    if (b < count) {
      float ym = std::clamp((lv.fx->tel.a[b].load(std::memory_order_relaxed) + 72.0f) / 72.0f, 0.0f, 1.0f);
      float yo = std::clamp((lv.fx->tel.b[b].load(std::memory_order_relaxed) + 72.0f) / 72.0f, 0.0f, 1.0f);
      dl->AddRectFilled(ImVec2(x - bwid, Y(ym)), ImVec2(x + bwid, bot), theme::withAlpha(P.text, 0.10f), 1);
      dl->AddRectFilled(ImVec2(x - bwid * 0.5f, Y(yo)), ImVec2(x + bwid * 0.5f, bot), theme::withAlpha(K.fill, 0.45f), 1);
    }
  }
  // Absolute point positions.
  std::vector<VxPoint>& pts = d.maps[m];
  double total = 0;
  for (size_t i = 1; i < pts.size(); ++i) total += pts[i].dx;
  if (total <= 0) total = 1;
  std::vector<double> ax(pts.size());
  {
    double acc = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (i) acc += pts[i].dx;
      ax[i] = acc / total;
    }
  }
  std::vector<ImVec2> line;
  for (int i = 0; i <= 256; ++i) line.push_back(ImVec2(X(i / 256.0), Y(vxEval(pts, i / 256.0))));
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), K.fill, 0, 2.0f);
  ImGuiIO& io = ImGui::GetIO();
  const double mx = std::clamp(static_cast<double>((io.MousePos.x - gx) / gw), 0.0, 1.0), my = std::clamp(static_cast<double>((bot - io.MousePos.y) / (bot - top)), 0.0, 1.0);
  int hover = -1;
  float best = 9;
  for (size_t i = 0; i < pts.size() && pts.size() <= 128; ++i) {
    float dd = std::hypot(io.MousePos.x - X(ax[i]), io.MousePos.y - Y(pts[i].y));
    if (dd < best) best = dd, hover = static_cast<int>(i);
  }
  static int drag = -1;
  auto commit = [&](const char* label, bool merge) {
    // Back to relative distances over a unit total.
    std::vector<VxPoint> out = pts;
    for (size_t i = 0; i < out.size(); ++i) out[i].dx = i ? std::max(0.0, ax[i] - ax[i - 1]) : 0.0;
    d.maps[m] = out;
    PluginState ns = st;
    ns.data = vxSerialize(d);
    setPluginState(a, t, ns, label, merge ? (0x5643ull << 32) + static_cast<uint64_t>(m) : 0);
  };
  const bool hovered = ImGui::IsItemHovered();
  if (mode == 1) {  // draw: one point per band, set the band under the mouse
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (static_cast<int>(pts.size()) != nb) {
        std::vector<VxPoint> np(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) np[static_cast<size_t>(b)] = {b ? 1.0 / (nb - 1) : 0.0, vxEval(pts, static_cast<double>(b) / (nb - 1)), 0};
        pts = np;
        ax.resize(pts.size());
        for (int b = 0; b < nb; ++b) ax[static_cast<size_t>(b)] = static_cast<double>(b) / (nb - 1);
      }
      int b = std::clamp(static_cast<int>(std::lround(mx * (nb - 1))), 0, nb - 1);
      pts[static_cast<size_t>(b)].y = my;
      commit("Draw vocoder curve", true);
    }
  } else {
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = hover;
    if (ImGui::IsItemActive() && drag >= 0 && drag < static_cast<int>(pts.size()) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
      size_t i = static_cast<size_t>(drag);
      if (i > 0 && i + 1 < pts.size()) ax[i] = std::clamp(mx, ax[i - 1], ax[i + 1]);
      pts[i].y = my;
      commit("Edit vocoder curve", true);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hover < 0) {
      size_t at = 1;
      while (at < ax.size() && ax[at] < mx) ++at;
      pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(at), VxPoint{0, my, 0});
      ax.insert(ax.begin() + static_cast<std::ptrdiff_t>(at), mx);
      commit("Add vocoder curve point", false);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hover > 0 && hover + 1 < static_cast<int>(pts.size())) {
      pts.erase(pts.begin() + hover);
      ax.erase(ax.begin() + hover);
      commit("Delete vocoder curve point", false);
    }
  }
  if (ImGui::IsItemDeactivated()) {
    drag = -1;
    a.doc.endGesture();
  }
  if (pts.size() <= 128)
    for (size_t i = 0; i < pts.size(); ++i) {
      bool hot = static_cast<int>(i) == hover || static_cast<int>(i) == drag;
      dl->AddCircleFilled(ImVec2(X(ax[i]), Y(pts[i].y)), hot ? 4.5f : 3.0f, hot ? P.paper : K.fill);
    }
  char vb[48], hb[192];
  vxValueText(vb, sizeof vb, m, hovered ? my : vxEval(pts, 0.5));
  mono(dl, ImVec2(p1.x - monoWidth(vb) - 8, p.y + 4), K.lcdText, vb);
  if (hovered) {
    int b = std::clamp(static_cast<int>(std::lround(mx * (nb - 1))), 0, nb - 1);
    std::snprintf(hb, sizeof hb, "Band %d: %s  (%s)", b + 1, vb,
                  mode == 1 ? "drag to draw per band" : "drag points, double-click adds, right-click deletes");
    a.setHint(hb);
  }
}

void editorVocoderRef(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const skin::Skin& K = vocSkin();
  constexpr float kW = 593, kH = 603;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 orange = skin::hex(0xFFA830), env = skin::hex(0x963D63), band = skin::hex(0x32A67C), car = skin::hex(0x33A9C0);
  // ---- top row: inputs, L-R encoding, model, MIDI port ----
  {
    const int ms = static_cast<int>(val(st, VcModSource) + 0.5f), cs = static_cast<int>(val(st, VxCarSource) + 0.5f);
    static const char* mn[2] = {"MOD:IN", "MOD:SC"};
    static const char* cn[3] = {"CAR:SYN", "CAR:IN", "CAR:SC"};
    skin::digi(a, t, VcModSource, F.p(100, 8), F.sz(52, 18), K, mn[std::clamp(ms, 0, 1)], F.font(7.5f));
    if (ImGui::IsItemHovered()) a.setHint("Modulator input: main input or sidechain (drag)");
    skin::digi(a, t, VxCarSource, F.p(156, 8), F.sz(54, 18), K, cn[std::clamp(cs, 0, 2)], F.font(7.5f));
    if (ImGui::IsItemHovered()) a.setHint("Carrier input: built-in synth, main input or sidechain (drag)");
    chk(a, t, VxLrEncoding, F, 214, 6, 92, 20, K, "L-R ENCODING", K.select);
    static const char* md[2] = {"LEGACY", "REFERENCE"};
    sel(a, t, VxModel, F, 420, 6, 160, 20, K, true, md);
  }
  // ---- levels ----
  W(a, t, VcWetLevel, F, 9, 44, 84, K, "WET");
  V(a, t, VxEnhance, F, 104, 41, 16, 76, K, "ENH", orange);
  V(a, t, VcModLevel, F, 148, 41, 16, 76, K, "", orange);
  V(a, t, VcCarLevel, F, 177, 41, 16, 76, K, "", orange);
  V(a, t, VxNoise, F, 206, 53, 16, 64, K, "", orange);
  cap(dl, F, 156, 30, "MOD", K.textDim, 6.5f);
  cap(dl, F, 185, 30, "CAR", K.textDim, 6.5f);
  cap(dl, F, 214, 42, "NOISE", K.textDim, 6.5f);
  W(a, t, VxModHp, F, 144, 120, 24, K, "HP");
  W(a, t, VxCarLp, F, 173, 120, 24, K, "LP");
  W(a, t, VxNoiseHp, F, 202, 120, 24, K, "HP");
  // ---- contour ----
  text(dl, F, 279, 58, "CONTOUR", K.textDim);
  text(dl, F, 279, 72, "FROM", K.textDim, 6.5f);
  chk(a, t, VxContourCar, F, 279, 85, 88, 12, K, "CAR", K.select);
  chk(a, t, VxContourCons, F, 279, 99, 92, 12, K, "CONSONANTS", K.select);
  text(dl, F, 324, 72, "TO", K.textDim, 6.5f);
  chk(a, t, VxContourMod, F, 279, 113, 88, 12, K, "MOD", K.select);
  chk(a, t, VxContourNoise, F, 279, 127, 88, 12, K, "NOISE", K.select);
  chk(a, t, VxDenoise, F, 279, 141, 100, 12, K, "DENOISE", K.select);
  // ---- band meter (reference logo area) ----
  {
    ImVec2 q0 = F.p(391, 34), q1 = F.p(580, 111);
    dl->AddRectFilled(q0, q1, K.lcdBg, 3 * F.s);
    const int count = lv.fx ? lv.fx->tel.count.load(std::memory_order_relaxed) : 0;
    const float w = (q1.x - q0.x - 8) / std::max(1, count);
    for (int b = 0; b < count; ++b) {
      float lvl = std::clamp((lv.fx->tel.b[b].load(std::memory_order_relaxed) + 72.0f) / 72.0f, 0.0f, 1.0f);
      float x = q0.x + 4 + w * b;
      dl->AddRectFilled(ImVec2(x, q1.y - 4 - lvl * (q1.y - q0.y - 8)), ImVec2(x + std::max(1.0f, w - 1), q1.y - 4), theme::withAlpha(K.lcdText, 0.8f));
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%d BANDS", static_cast<int>(std::lround(val(st, VxBands))));
    mono(dl, ImVec2(q0.x + 6, q0.y + 4), K.lcdText, buf);
    if (lv.v(TvUnvoiced) > 0.5f) mono(dl, ImVec2(q1.x - monoWidth("CONS") - 6, q0.y + 4), orange, "CONS");
  }
  // ---- envelope ----
  W(a, t, VxHoldMs, F, 27, 166, 38, K, "HOLD", skin::WheelBigLine, env);
  W(a, t, VxAttackMs, F, 81, 158, 46, K, "ATTACK", skin::WheelBigLine, env);
  W(a, t, VxReleaseMs, F, 140, 158, 46, K, "RELEASE", skin::WheelBigLine, env);
  chk(a, t, VxMinTimes, F, 192, 169, 76, 20, K, "MIN TIMES", K.select);
  {
    static const char* hm[2] = {"PEAK", "RMS"};
    sel(a, t, VxHoldMode, F, 13, 222, 45, 30, K, false, hm);
  }
  W(a, t, VxAttCurve, F, 93, 224, 24, K, "CURVE");
  W(a, t, VxRelCurve, F, 151, 224, 24, K, "CURVE");
  tog(a, t, VcHold, F, 140, 262, 46, 24, K, "FREEZE");
  // ---- bands ----
  W(a, t, VxWidth, F, 357, 158, 46, K, "WIDTH", skin::WheelBigLine, band);
  W(a, t, VxModWidth, F, 416, 166, 38, K, "MOD W", skin::WheelBigLine, band);
  W(a, t, VcFormant, F, 477, 158, 46, K, "PITCH", skin::WheelBigLine, band);
  W(a, t, VxUnisonShift, F, 536, 158, 46, K, "UNISON", skin::WheelBigLine, band);
  W(a, t, VxFlatness, F, 368, 224, 24, K, "FLAT");
  W(a, t, VxUnisonPan, F, 547, 224, 24, K, "PAN");
  {
    static const char* ol[4] = {"1", "2", "3", "4"};
    sel(a, t, VxOrder, F, 270, 214, 74, 14, K, true, ol);
    cap(dl, F, 307, 230, "ORDER", K.textDim, 6.5f);
  }
  skin::digi(a, t, VxBands, F.p(283, 258), F.sz(42, 21), K, nullptr, F.font(9));
  cap(dl, F, 304, 281, "BANDS", K.textDim, 6.5f);
  skin::digi(a, t, VxUnison, F.p(556, 258), F.sz(26, 21), K, nullptr, F.font(9));
  cap(dl, F, 569, 281, "COPIES", K.textDim, 6.5f);
  // ---- curve editor ----
  static std::map<uint64_t, int> toolOf;
  int& tool = toolOf[t.key()];
  {
    at(F, 16, 297);
    choiceCombo(a, t, VxEditTarget, 159 * F.s);
    const int m = std::clamp(static_cast<int>(val(st, VxEditTarget) + 0.5f), 0, kVxMaps - 1);
    VxData d;
    vxParse(st.data, d);
    if (btn("##vxpf", F, 181, 299, 30, 19, K, "PF", d.pf[m])) {
      d.pf[m] = !d.pf[m];
      PluginState ns = st;
      ns.data = vxSerialize(d);
      setPluginState(a, t, ns, "Vocoder curve pitch follow");
    }
    if (ImGui::IsItemHovered()) a.setHint("Curve follows the modulator pitch shift");
    if (btn("##vxpts", F, 380, 299, 50, 19, K, "POINTS", tool == 0)) tool = 0;
    if (btn("##vxdraw", F, 434, 299, 50, 19, K, "DRAW", tool == 1)) tool = 1;
    if (btn("##vxreset", F, 488, 299, 50, 19, K, "RESET", false)) {
      VxData def = vxDefaultData();
      d.maps[m] = def.maps[m];
      PluginState ns = st;
      ns.data = vxSerialize(d);
      setPluginState(a, t, ns, "Reset vocoder curve");
    }
    if (btn("##vxflat", F, 542, 299, 42, 19, K, "FLIP", false)) {
      for (auto& q : d.maps[m]) q.y = 1.0 - q.y;
      PluginState ns = st;
      ns.data = vxSerialize(d);
      setPluginState(a, t, ns, "Flip vocoder curve");
    }
  }
  vxCurveEditor(a, t, st, lv, F.p(9, 321), F.sz(575, 208), K, tool);
  // ---- carrier ----
  text(dl, F, 9, 536, "CARRIER", K.textDim);
  {
    char vb[8];
    std::snprintf(vb, sizeof vb, "%d", static_cast<int>(std::lround(val(st, VxCarVoices))));
    skin::digi(a, t, VxCarVoices, F.p(9, 548), F.sz(22, 18), K, vb, F.font(8));
  }
  cap(dl, F, 20, 567, "VOI", K.textDim, 6.0f);
  W(a, t, VxCarDetune, F, 40, 544, 20, K, "DET", skin::WheelBigLine, car);
  {
    int port = static_cast<int>(std::lround(val(st, VxMidiPort)));
    char pb[8];
    std::snprintf(pb, sizeof pb, port < 0 ? "--" : "%d", port);
    skin::digi(a, t, VxMidiPort, F.p(70, 548), F.sz(28, 18), K, pb, F.font(8));
    if (ImGui::IsItemHovered()) a.setHint("MIDI port that plays the built-in carrier (-- = held keys only)");
    cap(dl, F, 84, 567, "PORT", K.textDim, 6.0f);
  }
  menu(a, t, VxTone, F, 9, 578, 160, 20, K);
  {
    VxData d;
    vxParse(st.data, d);
    bool lit[128] = {};
    for (int k : d.keys) lit[k] = true;
    if (d.keys.empty()) lit[std::clamp(static_cast<int>(std::lround(val(st, VcRoot))), 0, 127)] = true;
    int n = skin::keyboard("##vxkb", F.p(176, 535), F.sz(408, 64), 36, 96, K, lit, -1, -1, -1);
    if (n >= 0) {
      if (d.keys.empty() && st.data.empty()) d = vxDefaultData();
      auto it = std::find(d.keys.begin(), d.keys.end(), n);
      if (it != d.keys.end())
        d.keys.erase(it);
      else if (d.keys.size() < 16)
        d.keys.push_back(n);
      std::sort(d.keys.begin(), d.keys.end());
      PluginState ns = st;
      ns.data = vxSerialize(d);
      setPluginState(a, t, ns, "Vocoder carrier keys");
    }
    if (ImGui::IsItemHovered()) a.setHint("Held keys of the built-in carrier (click to toggle)");
  }
  F.end(kW, kH);
}

void editorVocoder(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (st && val(*st, VxModel) >= 0.5f)
    editorVocoderRef(a, t, width);
  else
    editorVocoderLegacy(a, t, width);
}


// ============================================================ Pitch shifter
// Reference pitch-shifter panel 747 × 280: engine selector column (its colour
// tints the accents), pitch / formant (or grain length) pad, pitch and
// formant / duration sliders, the engine's wheel (formant correction or
// density), pitch-analysis drop-downs (voice) or jitter / randomize wheels
// (music), delay + feedback sliders with tempo sync and the 2 × 2 delay-mode
// cells, and the mix slider. Gridline's extras (fine, stereo detune, output,
// the classic engines) sit in a strip underneath (747 × 330).
const skin::Skin& pitchSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x323E44, 0x2B363B, 0x6C777D, 0x42A2C8, 0x3F484D, 0xA0ADB4);
    s.fillPressed = skin::hex(0x5FB6D8);
    s.handle = skin::hex(0x30373A);
    s.valueBg = skin::hex(0x42A2C8);
    s.valueText = skin::hex(0x292929);
    s.select = skin::hex(0x42A2C8);
    s.lcdBg = skin::hex(0x242D31);
    s.graphBg = skin::hex(0x31373B);
    s.graphGrid = skin::hex(0x3C444A);
    s.graphGridStrong = skin::hex(0x56616A);
    return s;
  }();
  return k;
}
// Same family, music engine tint.
const skin::Skin& pitchMusicSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = pitchSkin();
    s.fill = skin::hex(0x5FB88A);
    s.fillPressed = skin::hex(0x7CCFA3);
    s.valueBg = skin::hex(0x5FB88A);
    s.select = skin::hex(0x5FB88A);
    return s;
  }();
  return k;
}

void editorPitch(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const int engine = std::clamp(static_cast<int>(val(st, PsEngine) + 0.5f), 0, 2);
  const bool classic = engine == kPsClassic, voice = engine == kPsVoice, music = engine == kPsMusic;
  const bool spectral = val(st, PsMode) >= 0.5f;
  const bool formantOn = voice || (classic && spectral);
  const skin::Skin& K = music ? pitchMusicSkin() : pitchSkin();
  constexpr float kW = 747, kH = 330;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  {
    static const char* engines[3] = {"CLS", "VOICE", "MUSIC"};
    sel(a, t, PsEngine, F, 5, 5, 32, 270, K, false, engines);
  }
  // Pitch (x) / formant or grain length (y) pad.
  const int yParam = music ? PsDuration : PsFormant;
  {
    ImVec2 p = F.p(43, 12), sz = F.sz(256, 256);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##xy", sz);
    ImVec2 p1(p.x + sz.x, p.y + sz.y);
    skin::graphWell(dl, p, p1, K);
    const float pad = 14;
    const ParamDesc* yd = pdesc(a, t, yParam);
    const double y0 = yd ? yd->min : -12, y1 = yd ? yd->max : 12;
    auto X = [&](double v) { return p.x + pad + static_cast<float>((v + 24) / 48) * (sz.x - 2 * pad); };
    auto Y = [&](double v) { return p.y + pad + static_cast<float>((y1 - v) / (y1 - y0)) * (sz.y - 2 * pad - 14); };
    for (int k = -24; k <= 24; k += 12) dl->AddLine(ImVec2(X(k), Y(y1)), ImVec2(X(k), Y(y0)), k == 0 ? P.lineStrong : P.gridBar);
    for (int k = -21; k <= 21; k += 3)
      if (k % 12) dl->AddLine(ImVec2(X(k), Y(y1)), ImVec2(X(k), Y(y0)), P.gridStep);
    for (int g = 0; g <= 4; ++g) {
      double v = y0 + (y1 - y0) * g / 4.0;
      dl->AddLine(ImVec2(X(-24), Y(v)), ImVec2(X(24), Y(v)), g == 2 && !music ? P.lineStrong : P.gridStep);
    }
    // Live analysis: detected input pitch (voice engine) as a faint column.
    float det = lv.v(TvDetected);
    if (voice && det > 0) {
      char nb[32];
      noteText(nb, sizeof nb, det);
      mono(dl, ImVec2(p1.x - monoWidth(nb) - 10, p.y + 4), K.graphText, nb);
    }
    double pitch = val(st, PsPitch) + val(st, PsFine) / 100.0, yv = val(st, yParam);
    ImVec2 dot(X(pitch), Y(yv));
    dl->AddLine(ImVec2(dot.x, Y(y1)), ImVec2(dot.x, Y(y0)), theme::withAlpha(K.fill, 0.5f));
    dl->AddCircleFilled(dot, 9, theme::withAlpha(K.fill, 0.25f));
    dl->AddCircleFilled(dot, 6, ImGui::IsItemActive() ? K.fillPressed : K.fill);
    mono(dl, ImVec2(p.x + 10, p1.y - 18), K.graphText, "PITCH  -24 .. +24 st");
    mono(dl, ImVec2(p.x + 10, p.y + 4), formantOn || music ? K.graphText : theme::withAlpha(K.graphText, 0.4f), music ? "GRAIN" : "FORMANT");
    if (ImGui::IsItemActive()) {
      ImGuiIO& io = ImGui::GetIO();
      double nx = std::clamp((io.MousePos.x - p.x - pad) / (sz.x - 2 * pad) * 48.0 - 24.0, -24.0, 24.0);
      double ny = std::clamp(y1 - (io.MousePos.y - p.y - pad) / (sz.y - 2 * pad - 14) * (y1 - y0), y0, y1);
      if (!io.KeyShift) nx = std::round(nx);
      PluginState ns = st;
      ns.params[PsPitch] = static_cast<float>(nx);
      if (!io.KeyShift) ns.params[PsFine] = 0;
      ns.params[static_cast<size_t>(yParam)] = static_cast<float>(ny);
      setPluginState(a, t, ns, music ? "pitch / grain length" : "pitch / formant", 0x5053585900000000ull ^ t.key());
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      char buf[112];
      std::snprintf(buf, sizeof buf, "Pitch %+.2f st, %s %s (drag; Shift = no semitone snap)", pitch, music ? "grain length" : "formant",
                    pfmt(a, t, yParam).c_str());
      a.setHint(buf);
    }
  }
  text(dl, F, 47, 271, music ? "GRAIN CLOUD" : (voice ? "PITCH-SYNCHRONOUS" : (spectral ? "SPECTRAL" : "DELAY LINE")), K.textDim);
  // Pitch and formant / duration sliders.
  VV(a, t, PsPitch, F, 319, 44, 26, 200, K, "PITCH");
  if (music) {
    VV(a, t, PsDuration, F, 389, 44, 26, 200, K, "DURATION");
  } else {
    ImGui::BeginDisabled(!formantOn);
    VV(a, t, PsFormant, F, 377, 44, 26, 200, K, "FORMANT");
    ImGui::EndDisabled();
    if (!formantOn) dim(dl, F, 369, 24, 42, 240, K);
  }
  // Engine wheels / analysis.
  if (voice) {
    WV(a, t, PsCorrection, F, 455, 48, 60, K, "FORMANT CORR.");
    static const char* caps[3] = {"MODE", "DETECTOR", "CHANNEL"};
    const int ps[3] = {PsPitchMode, PsDetector, PsAnalysis};
    for (int i = 0; i < 3; ++i) {
      text(dl, F, 445, 142 + 33 * i, caps[i], K.textDim, 6.5f);
      menu(a, t, ps[i], F, 445, 153 + 33 * i, 80, 20, K);
    }
    cap(dl, F, 485, 254, "PITCH ANALYSIS", K.textDim);
  } else if (music) {
    WV(a, t, PsDensity, F, 455, 47, 60, K, "DENSITY");
    WV(a, t, PsJitter, F, 435, 199, 42, K, "JITTER");
    WV(a, t, PsRand, F, 493, 199, 42, K, "RANDOM");
  } else {
    static const char* modes[2] = {"FAST", "SPECTRAL"};
    sel(a, t, PsMode, F, 438, 28, 94, 34, K, false, modes);
    if (spectral) chk(a, t, PsPreserve, F, 438, 74, 110, 18, K, "KEEP FORMANTS");
    else WV(a, t, PsGrain, F, 455, 92, 52, K, "WINDOW");
    cap(dl, F, 485, 254, "CLASSIC ENGINE", K.textDim);
  }
  // Delay section.
  const bool sync = val(st, PsDelaySync) >= 0.5f;
  const int dmode = std::clamp(static_cast<int>(val(st, PsDelayMode) + 0.5f), 0, 3);
  VV(a, t, sync ? PsDelaySteps : PsDelay, F, 567, 44, 26, 106, K, "DELAY");
  VV(a, t, PsFeedback, F, 625, 44, 26, 106, K, "FEEDBACK");
  if (classic) dim(dl, F, 560, 24, 100, 156, K);
  chk(a, t, PsDelaySync, F, 597, 168, 50, 12, K, "SYNC");
  {
    static const char* cells[4] = {"OFF", "POST", "PRE", "DRY"};
    static const char* hints[4] = {"Delay off", "Echoes after the shifter (they shift again on every repeat)",
                                   "Echoes before the shifter", "Echoes of the unshifted input, mixed with the shifted signal"};
    for (int i = 0; i < 4; ++i) {
      float x = 575 + (i % 2) * 40, y = 184 + (i / 2) * 31;
      ImGui::PushID(i);
      if (btn("##dm", F, x, y, 38, 29, K, cells[i], dmode == i)) setP(a, t, PsDelayMode, i, false);
      if (ImGui::IsItemHovered()) a.setHint(hints[i]);
      ImGui::PopID();
    }
    if (classic) dim(dl, F, 572, 182, 84, 64, K);
    cap(dl, F, 615, 250, "DELAY MODE", K.textDim);
  }
  VV(a, t, PsMix, F, 691, 44, 26, 200, K, "MIX");
  // Gridline strip.
  box(dl, F, 43, 284, 698, 40, K, nullptr);
  W(a, t, PsFine, F, 60, 288, 24, K, "FINE", skin::WheelBigLine | skin::WheelCentered);
  W(a, t, PsDetune, F, 120, 288, 24, K, "DETUNE");
  W(a, t, PsOutput, F, 180, 288, 24, K, "OUTPUT", skin::WheelBigLine | skin::WheelCentered);
  {
    char buf[48];
    float lat = lv.v(TvLatency);
    double sr = a.engine.sampleRate();
    std::snprintf(buf, sizeof buf, "LATENCY %.1f ms", lv.fx ? lat / sr * 1000.0 : 0.0);
    skin::lcd(dl, F.p(240, 292), F.p(380, 312), K, buf, F.font(8.0f));
  }
  if (classic) text(dl, F, 400, 297, "CLASSIC: DELAY AFTER THE SHIFTER, FEEDBACK INTO IT", K.textDim, 6.5f);
  else text(dl, F, 400, 297, voice ? "VOICE: PSOLA, FORMANTS KEPT BY CORRECTION" : "MUSIC: GRANULAR, ANY MATERIAL", K.textDim, 6.5f);
  F.end(kW, kH);
}

// ========================================================== Pitch corrector
// Reference pitch-corrector window (523 × 354, bitmap skin there): title
// strip with the pitch-reference switch, a correction readout, the range /
// scale / key / A4 strip and twelve note keys between the input- and
// output-pitch rails, the key-bypass LED row, and a lower panel that is either
// the correction view (speed and fine levers, gender knob, formant button) or
// the harmonize view (four voice-pan faders, stereo knob, mix button,
// velocity LED, held-note keyboard), with the MIDI / octaves / harmonize
// tabs and the MIDI port box. Gridline extras (engine, amount, lead and
// interval voices) in a strip underneath (523 × 420).
const skin::Skin& pitcherSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x262626, 0x1B1B1B, 0x4A4A4A, 0x3D63DD, 0x343434, 0xC4C4C4);
    s.fillPressed = skin::hex(0x5E7FE6);
    s.select = skin::hex(0x3D63DD);
    s.valueBg = skin::hex(0x3D63DD);
    s.valueText = skin::hex(0xF2F2F2);
    s.lcdBg = skin::hex(0x141414);
    s.graphBg = skin::hex(0x161616);
    s.graphGrid = skin::hex(0x262626);
    s.graphGridStrong = skin::hex(0x3A3A3A);
    s.accent2 = skin::hex(0xE5484D);
    return s;
  }();
  return k;
}

void editorPitcher(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const skin::Skin& K = pitcherSkin();
  constexpr float kW = 523, kH = 420;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const bool ref = val(st, PcModel) >= 0.5f;
  const bool harm = ref && val(st, PcHarmonize) >= 0.5f, midiT = ref && val(st, PcMidiTargets) >= 0.5f;
  float pv[PcCount];
  for (int k = 0; k < PcCount; ++k) pv[k] = val(st, k);
  const uint16_t mask = pitcherMask(pv);
  const float det = lv.v(TvDetected), tgt = lv.v(TvTarget), outp = lv.v(TvOutput);
  bool held[128] = {};
  uint16_t heldPc = 0;
  for (int k = 0; k < 128; ++k)
    if (lv.fx && lv.fx->tel.wave[static_cast<size_t>(k)].load(std::memory_order_relaxed) > 0) {
      held[k] = true;
      heldPc = static_cast<uint16_t>(heldPc | (1 << (k % 12)));
    }

  // ---- title strip: name, engine, pitch reference.
  text(dl, F, 14, 10, "PITCH CORRECTOR", K.text, 10.0f);
  {
    static const char* models[2] = {"LEGACY", "REFERENCE"};
    sel(a, t, PcModel, F, 300, 8, 120, 16, K, true, models);
    static const char* refs[3] = {"L", "L+R", "R"};
    text(dl, F, 427, 45, "REF", K.textDim, 6.5f);
    ImGui::BeginDisabled(!ref);
    sel(a, t, PcRefChannel, F, 447, 42, 66, 17, K, true, refs);
    ImGui::EndDisabled();
  }
  // Correction readout (input note, output note, applied correction).
  {
    ImVec2 p0 = F.p(124, 40), p1 = F.p(420, 80);
    skin::graphWell(dl, p0, p1, K);
    char buf[48];
    if (det > 0) noteText(buf, sizeof buf, det);
    else std::snprintf(buf, sizeof buf, "--");
    skin::text(dl, F.p(132, 44), K.graphText, "IN", F.font(6.5f));
    skin::text(dl, F.p(132, 54), K.text, buf, F.font(9.0f));
    if (det > 0) noteText(buf, sizeof buf, outp);
    else std::snprintf(buf, sizeof buf, "--");
    skin::text(dl, F.p(228, 44), K.graphText, "OUT", F.font(6.5f));
    skin::text(dl, F.p(228, 54), K.fill, buf, F.font(9.0f));
    float corr = det > 0 ? outp - det : 0.f;
    std::snprintf(buf, sizeof buf, "%+.0f ct", corr * 100.0f);
    skin::text(dl, F.p(330, 44), K.graphText, "CORRECTION", F.font(6.5f));
    skin::text(dl, F.p(330, 54), K.text, buf, F.font(9.0f));
    // Cents bar (±100 ct).
    float bx0 = F.p(132, 0).x, bx1 = F.p(412, 0).x, by = F.p(0, 72).y;
    dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + F.u(4)), K.rail, 2);
    float cx = (bx0 + bx1) * 0.5f, vx = cx + std::clamp(corr, -1.0f, 1.0f) * (bx1 - bx0) * 0.5f;
    dl->AddRectFilled(ImVec2(std::min(cx, vx), by), ImVec2(std::max(cx, vx) + 1, by + F.u(4)), K.fill, 2);
    dl->AddLine(ImVec2(cx, by - F.u(2)), ImVec2(cx, by + F.u(6)), K.text);
  }
  // ---- scale strip: range, scale, A4, key.
  if (ref) menu(a, t, PcRange6, F, 28, 86, 90, 16, K);
  else menu(a, t, PcLowest, F, 28, 86, 90, 16, K);
  if (ImGui::IsItemHovered()) a.setHint("Lowest pitch the detector accepts (also its analysis window)");
  menu(a, t, PcScale, F, 28, 104, 94, 14, K);
  skin::digi(a, t, PcReference, F.p(28, 120), F.sz(46, 14), K, nullptr, F.font(7.0f));
  if (ImGui::IsItemHovered()) a.setHint("Tuning reference: A4 in Hz (drag)");
  menu(a, t, PcKey, F, 77, 120, 46, 14, K);
  // ---- 12 note keys with the input / output pitch rails.
  {
    const float kx = 124, ky = 103, kw = 31;
    ImVec2 r0 = F.p(kx, 92), r1 = F.p(kx + 12 * kw, 100), o0 = F.p(kx, 136), o1 = F.p(kx + 12 * kw, 144);
    dl->AddRectFilled(r0, r1, K.lcdBg, 2);
    dl->AddRectFilled(o0, o1, K.lcdBg, 2);
    skin::text(dl, F.p(kx + 12 * kw + 4, 92), K.textDim, "IN", F.font(6.0f));
    skin::text(dl, F.p(kx + 12 * kw + 4, 136), K.textDim, "OUT", F.font(6.0f));
    auto pcX = [&](double midi) {
      double pc = std::fmod(midi, 12.0);
      if (pc < 0) pc += 12;
      return F.p(kx + static_cast<float>(pc + 0.5) * kw, 0).x;
    };
    if (det > 0) {
      float x = pcX(det);
      dl->AddTriangleFilled(ImVec2(x - F.u(4), r0.y), ImVec2(x + F.u(4), r0.y), ImVec2(x, r1.y), K.text);
      float xo = pcX(outp);
      dl->AddTriangleFilled(ImVec2(xo, o0.y), ImVec2(xo - F.u(4), o1.y), ImVec2(xo + F.u(4), o1.y), K.fill);
    }
    const uint16_t targets = midiT ? heldPc : mask;
    const int tpc = tgt > 0 ? ((static_cast<int>(tgt) % 12) + 12) % 12 : -1;
    for (int k = 0; k < 12; ++k) {
      const bool black = (0x54A >> k) & 1;  // C# D# F# G# A#
      const bool on = (targets >> k) & 1;
      ImVec2 p0 = F.p(kx + k * kw + 1.5f, ky), p1 = F.p(kx + (k + 1) * kw - 1.5f, ky + 31);
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID(400 + k);
      ImGui::InvisibleButton("##key", ImVec2(p1.x - p0.x, p1.y - p0.y));
      bool hov = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) {
        PluginState ns = st;
        for (int q = 0; q < 12; ++q) ns.params[static_cast<size_t>(PcNote0 + q)] = (mask & (1 << q)) ? 1.f : 0.f;
        size_t idx = static_cast<size_t>(PcNote0 + k);
        ns.params[idx] = ns.params[idx] >= 0.5f ? 0.f : 1.f;
        ns.params[PcScale] = static_cast<float>(kScaleCount - 1);
        setPluginState(a, t, ns, "edit scale notes");
      }
      if (hov)
        a.setHint(midiT ? "MIDI targets: the lit keys are the held notes (click edits the scale used without MIDI)"
                        : "Click a key to add it to / remove it from the target notes (switches the scale to Custom)");
      ImGui::PopID();
      ImU32 bg = black ? (on ? skin::darken(K.fill, 0.25f) : skin::hex(0x101010)) : (on ? K.fill : skin::hex(0xBDBDBD));
      dl->AddRectFilled(p0, p1, bg, F.u(3));
      dl->AddRect(p0, p1, hov ? K.text : skin::hex(0x0A0A0A), F.u(3), 0, hov ? 1.5f : 1.0f);
      if (k == tpc && det > 0) dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, p1.y - F.u(7)), F.u(3), on ? skin::hex(0xF2F2F2) : K.fill);
      skin::text(dl, ImVec2(p0.x + F.u(3), p0.y + F.u(2)), black || on ? skin::hex(0xE8E8E8) : skin::hex(0x202020), kNoteNames[k], F.font(6.0f));
    }
  }
  // ---- key-bypass LEDs (input pass-through while the sung note is that key).
  {
    ImGui::BeginDisabled(!ref);
    auto led = [&](int param, float cx, float cy, const char* lbl, const char* hint) {
      const bool on = val(st, param) >= 0.5f;
      ImVec2 c = F.p(cx, cy);
      float r = F.u(6);
      ImGui::SetCursorScreenPos(ImVec2(c.x - r - 2, c.y - r - 2));
      ImGui::PushID(param);
      ImGui::InvisibleButton("##led", ImVec2(2 * r + 4, 2 * r + 4));
      if (ImGui::IsItemClicked()) setP(a, t, param, on ? 0.0 : 1.0, false);
      if (ImGui::IsItemHovered()) a.setHint(hint);
      ImGui::PopID();
      skin::ledDot(dl, c, r, on, K, K.accent2C());
      if (lbl) skin::text(dl, ImVec2(c.x + r + F.u(3), c.y - F.u(4)), K.textDim, lbl, F.font(6.5f));
    };
    led(PcBypassAll, 39, 164, "BYPASS ALL", "Bypass all keys: no correction at all");
    for (int k = 0; k < 12; ++k) {
      char hint[96];
      std::snprintf(hint, sizeof hint, "Bypass %s: no correction while the sung note is %s", kNoteNames[k], kNoteNames[k]);
      led(PcKeyBypass0 + k, 124 + 31 * k + 15.5f, 164, nullptr, hint);
    }
    ImGui::EndDisabled();
    if (!ref) dim(dl, F, 26, 154, 490, 22, K);
  }
  // ---- lower panel.
  box(dl, F, 8, 186, 507, 164, K, nullptr);
  if (!harm) {
    // Correction view.
    VV(a, t, ref ? PcCorrSpeed : PcSpeed, F, 39, 214, 26, 96, K, "SPEED");
    if (ref) {
      char b[32];
      double ms = pitcherGlideMs(val(st, PcCorrSpeed));
      if (ms <= 0) std::snprintf(b, sizeof b, "instant");
      else std::snprintf(b, sizeof b, "%.1f ms", ms);
      text(dl, F, 26, 330, b, K.textDim, 6.5f);
    }
    // Detection confidence + correction display.
    {
      ImVec2 p0 = F.p(92, 206), p1 = F.p(196, 282);
      skin::graphWell(dl, p0, p1, K);
      skin::text(dl, F.p(98, 210), K.graphText, "DETECTION", F.font(6.0f));
      float conf = lv.v(TvConfidence);
      float bx0 = F.p(98, 0).x, bx1 = F.p(190, 0).x;
      dl->AddRectFilled(ImVec2(bx0, F.p(0, 224).y), ImVec2(bx1, F.p(0, 230).y), K.rail, 2);
      dl->AddRectFilled(ImVec2(bx0, F.p(0, 224).y), ImVec2(bx0 + (bx1 - bx0) * std::clamp(conf, 0.f, 1.f), F.p(0, 230).y), K.fill, 2);
      char b[40];
      if (det > 0) std::snprintf(b, sizeof b, "%.1f Hz", 440.0 * std::pow(2.0, (det - 69.0) / 12.0));
      else std::snprintf(b, sizeof b, "unvoiced");
      skin::text(dl, F.p(98, 238), K.text, b, F.font(7.5f));
      if (tgt > 0) {
        noteText(b, sizeof b, tgt);
        skin::text(dl, F.p(98, 256), K.textDim, "TARGET", F.font(6.0f));
        skin::text(dl, F.p(132, 255), K.fill, b, F.font(7.0f));
      }
    }
    W(a, t, PcFormant, F, 222, 206, 54, K, "GENDER", skin::WheelBigLine | skin::WheelCentered);
    ImGui::BeginDisabled(!ref);
    VV(a, t, PcFine, F, 383, 214, 26, 96, K, "FINE");
    ImGui::EndDisabled();
    tog(a, t, PcPreserve, F, 318, 240, 52, 22, K, "FORMANT");
    if (ImGui::IsItemHovered()) a.setHint("Formant: pitch-synchronous resynthesis with the gender control (off: delay-line shifter)");
    W(a, t, PcAmount, F, 446, 214, 34, K, "AMOUNT");
  } else {
    // Harmonize view: voice pans, stereo, mix / velocity, held notes.
    for (int k = 0; k < 4; ++k) {
      char lb[8];
      std::snprintf(lb, sizeof lb, "V%d", k + 1);
      text(dl, F, 34, 196 + 20 * k, lb, K.textDim, 7.0f);
      skin::hslider(a, t, PcHarmPan1 + k, F.p(62, 195 + 20 * k), F.sz(80, 12), K);
    }
    W(a, t, PcStereo, F, 158, 202, 40, K, "STEREO");
    tog(a, t, PcHarmMix, F, 28, 282, 60, 22, K, "MIX");
    if (ImGui::IsItemHovered()) a.setHint("Mix: add the dry input to the voices (off: the voices replace it)");
    chk(a, t, PcHarmVel, F, 100, 286, 54, 14, K, "VEL");
    if (ImGui::IsItemHovered()) a.setHint("Voice level follows the note velocity (2 v²)");
    // Held notes and sounding voices.
    {
      ImVec2 p0 = F.p(212, 196), p1 = F.p(506, 284);
      skin::graphWell(dl, p0, p1, K);
      skin::text(dl, F.p(218, 199), K.graphText, "HELD NOTES / VOICES", F.font(6.0f));
      bool lit[128] = {};
      int lo = 48, hi = 83;
      for (int k = 0; k < 128; ++k) lit[k] = held[k];
      for (int k = 0; k < 128; ++k)
        if (held[k]) {
          while (k < lo) lo -= 12;
          while (k > hi) hi += 12;
        }
      skin::keyboard("##heldkeys", F.p(218, 212), F.sz(282, 52), std::max(0, lo), std::min(127, hi), K, lit);
      char b[96] = "";
      size_t used = 0;
      for (int k = 0; k < 4; ++k) {
        float vp = lv.fx ? lv.fx->tel.wave[static_cast<size_t>(128 + k)].load(std::memory_order_relaxed) : 0.f;
        if (vp <= 0) continue;
        char nb[24];
        int n = static_cast<int>(std::lround(vp));
        std::snprintf(nb, sizeof nb, "%s%s%d", used ? "  " : "", kNoteNames[((n % 12) + 12) % 12], n / 12 - 1);
        used += static_cast<size_t>(std::snprintf(b + used, sizeof b - used, "%s", nb));
        if (used >= sizeof b - 1) break;
      }
      skin::text(dl, F.p(218, 268), K.fill, used ? b : "no voices", F.font(7.0f));
    }
  }
  // Mode tabs + MIDI port.
  {
    ImGui::BeginDisabled(!ref);
    tog(a, t, PcMidiTargets, F, 170, 295, 58, 18, K, "MIDI");
    if (ImGui::IsItemHovered()) a.setHint("Correct to the notes held on the MIDI port instead of the scale");
    tog(a, t, PcMidiOctaves, F, 233, 295, 56, 18, K, "OCTAVES");
    if (ImGui::IsItemHovered()) a.setHint("MIDI notes keep their octave (exact notes instead of pitch classes)");
    tog(a, t, PcHarmonize, F, 293, 295, 76, 18, K, "HARMONIZE");
    if (ImGui::IsItemHovered()) a.setHint("Voices at the held MIDI notes (up to four)");
    text(dl, F, 108, 338, "MIDI PORT", K.textDim, 6.5f);
    char pb[16];
    int port = static_cast<int>(std::lround(val(st, PcMidiPort)));
    if (port < 0) std::snprintf(pb, sizeof pb, "ALL");
    else std::snprintf(pb, sizeof pb, "%d", port);
    skin::digi(a, t, PcMidiPort, F.p(150, 334), F.sz(34, 15), K, pb, F.font(7.0f));
    if (ImGui::IsItemHovered()) a.setHint("MIDI port whose held notes are read (a MIDI Out channel set to this port plays them); ALL = every port");
    ImGui::EndDisabled();
  }
  // ---- Gridline strip: lead and interval voices.
  box(dl, F, 8, 356, 507, 60, K, nullptr);
  {
    struct Cell {
      int param;
      const char* label;
      int flags;
    };
    const int cf = skin::WheelBigLine | skin::WheelCentered;
    const Cell cells[] = {{PcLeadLevel, "LEAD", skin::WheelBigLine}, {PcLeadPan, "PAN", cf},
                          {PcVoice1, "V2 ST", cf},  {PcVoice1Level, "V2 LVL", skin::WheelBigLine}, {PcVoice1Pan, "V2 PAN", cf},
                          {PcVoice2, "V3 ST", cf},  {PcVoice2Level, "V3 LVL", skin::WheelBigLine}, {PcVoice2Pan, "V3 PAN", cf},
                          {PcVoice3, "V4 ST", cf},  {PcVoice3Level, "V4 LVL", skin::WheelBigLine}, {PcVoice3Pan, "V4 PAN", cf}};
    ImGui::BeginDisabled(harm);
    float x = 18;
    for (const Cell& c : cells) {
      W(a, t, c.param, F, x, 362, 26, K, c.label, c.flags);
      x += 38;
    }
    chk(a, t, PcDiatonic, F, 438, 372, 72, 14, K, "IN SCALE");
    if (ImGui::IsItemHovered()) a.setHint("Snap the interval voices into the scale");
    ImGui::EndDisabled();
  }
  F.end(kW, kH);
}

// ================================================================ Scratcher
// Reference scratcher panel (247 × 303): level wheels and switches on the
// left, the platter in the middle, motor wheels on the right, transport /
// pattern row and the recorded-input scope at the bottom — widened here to
// 560 × 330 for Gridline's pattern and crossfader controls.
const skin::Skin& scratchSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x2A3033, 0x5F696D, 0xEDEFF1, 0x667279, 0xB9BFC3);
    s.fillPressed = skin::hex(0xFFA830);
    s.select = skin::hex(0xF8895A);
    s.lcdBg = skin::hex(0x232829);
    s.graphBg = skin::hex(0x232829);
    s.graphGrid = skin::hex(0x30373A);
    s.handle = skin::hex(0x6A7579);
    s.accent2 = skin::hex(0xFFA830);
    return s;
  }();
  return k;
}

void editorScratcher(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const skin::Skin& K = scratchSkin();
  constexpr float kW = 560, kH = 330;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 orange = skin::hex(0xFFA830), check = skin::hex(0xF8895A);
  // Left column.
  W(a, t, ScVolume, F, 22, 14, 50, K, "VOLUME");
  W(a, t, ScPan, F, 22, 88, 50, K, "PAN");
  chk(a, t, ScMotor, F, 10, 162, 84, 18, K, "MOTOR", check);
  if (ImGui::IsItemHovered()) a.setHint("Motor on/off: start-up and spin-down follow the start and stop times");
  chk(a, t, ScGrab, F, 10, 186, 84, 18, K, "HOLD", check);
  if (ImGui::IsItemHovered()) a.setHint("Hold the platter: the platter control then scratches the record");
  // Platter.
  {
    ImVec2 p = F.p(104, 4), p1 = F.p(456, 210);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##platter", ImVec2(p1.x - p.x, p1.y - p.y));
    ImVec2 c((p.x + p1.x) * 0.5f, (p.y + p1.y) * 0.5f);
    float R = (p1.y - p.y) * 0.5f - 16;
    dl->AddCircleFilled(c, R + 6, skin::darken(K.panel, 0.35f), 96);
    dl->AddCircleFilled(c, R, skin::hex(0x141617), 96);
    for (float r = R * 0.42f; r < R - 3; r += 5) dl->AddCircle(c, r, theme::withAlpha(K.text, 0.07f), 96);
    dl->AddCircleFilled(c, R * 0.34f, orange, 64);
    dl->AddCircleFilled(c, 4, skin::hex(0x141617), 16);
    float ang = lv.v(TvAngle) * 6.2831853f - 1.5707963f;
    ImVec2 m0(c.x + std::cos(ang) * R * 0.38f, c.y + std::sin(ang) * R * 0.38f), m1(c.x + std::cos(ang) * (R - 4), c.y + std::sin(ang) * (R - 4));
    dl->AddLine(m0, m1, K.fill, 3);
    float hand = std::clamp(lv.fx ? lv.v(TvHandPos) : val(st, ScHand), 0.0f, 1.0f);
    dl->PathArcTo(c, R + 11, -1.5707963f - 2.4f, -1.5707963f, 48);
    dl->PathStroke(K.rail, 0, 3.0f);
    float ha = -1.5707963f - 2.4f * hand;
    dl->AddCircleFilled(ImVec2(c.x + std::cos(ha) * (R + 11), c.y + std::sin(ha) * (R + 11)), 5, orange);
    char buf[64];
    std::snprintf(buf, sizeof buf, "SPEED %+4.0f%%", lv.v(TvVelocity) * 100.0f);
    mono(dl, ImVec2(p.x + 4, p1.y - 14), K.textDim, buf);
    std::snprintf(buf, sizeof buf, "BEHIND %4.0f ms", lv.v(TvLag) * 1000.0f);
    mono(dl, ImVec2(p1.x - 4 - monoWidth(buf), p1.y - 14), K.textDim, buf);
    static float lastAng = 0;
    static bool grabbedByDrag = false;
    ImGuiIO& io = ImGui::GetIO();
    float ma = std::atan2(io.MousePos.y - c.y, io.MousePos.x - c.x);
    if (ImGui::IsItemActivated()) {
      lastAng = ma;
      grabbedByDrag = val(st, ScGrab) < 0.5f;
      if (grabbedByDrag) setP(a, t, ScGrab, 1, true);
    }
    if (ImGui::IsItemActive()) {
      float d = ma - lastAng;
      if (d > 3.14159f) d -= 6.2831853f;
      if (d < -3.14159f) d += 6.2831853f;
      lastAng = ma;
      double turns = d / 6.2831853;
      double rangeS = std::max(0.1f, val(st, ScRange));
      double nh = std::clamp(val(*t.state(a.doc.song()), ScHand) - turns * 1.8 / rangeS, 0.0, 1.0);
      if (d != 0) setP(a, t, ScHand, nh, true);
      a.setHint("Scratching: clockwise plays forward, anticlockwise pulls the record back");
    }
    if (ImGui::IsItemDeactivated()) {
      if (grabbedByDrag) setP(a, t, ScGrab, 0, true);
      grabbedByDrag = false;
      a.doc.endGesture();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) a.setHint("Drag around the platter to scratch");
  }
  // Right column: motor + hand.
  text(dl, F, 470, 4, "MOTOR", K.textDim);
  W(a, t, ScStart, F, 468, 18, 38, K, "START");
  W(a, t, ScStop, F, 514, 18, 38, K, "STOP");
  text(dl, F, 470, 92, "HAND", K.textDim);
  W(a, t, ScRange, F, 468, 106, 38, K, "RANGE");
  W(a, t, ScSmooth, F, 514, 106, 38, K, "SMOOTH");
  // Transport row: auto-scratch pattern / rate, crossfader + curve.
  {
    static const char* pats[6] = {"OFF", "BABY", "FWD", "CHIRP", "TRANS", "TEAR"};
    sel(a, t, ScPattern, F, 6, 216, 300, 20, K, true, pats);
    static const char* rates[4] = {"1/2", "1/4", "1/8", "1/16"};
    sel(a, t, ScRate, F, 312, 216, 136, 20, K, true, rates);
    static const char* curves[3] = {"SMOOTH", "SHARP", "CUT"};
    sel(a, t, ScCurve, F, 454, 216, 100, 20, K, true, curves);
  }
  text(dl, F, 6, 244, "LIVE", K.textDim);
  text(dl, F, 524, 244, "DECK", K.textDim);
  skin::hslider(a, t, ScFader, F.p(36, 242), F.sz(484, 16), K, orange);
  // Recorded input scope.
  {
    ImVec2 w0 = F.p(6, 266), w1 = F.p(554, 324);
    skin::graphWell(dl, w0, w1, K);
    if (lv.fx) {
      int wp = lv.fx->tel.wavePos.load(std::memory_order_relaxed);
      float mid = (w0.y + w1.y) * 0.5f, hh0 = (w1.y - w0.y) * 0.5f - 5;
      for (int i = 0; i < 256; ++i) {
        float v = lv.fx->tel.wave[static_cast<size_t>((wp + i) & 255)].load(std::memory_order_relaxed);
        float x = w0.x + 4 + (w1.x - w0.x - 8) * i / 255.0f;
        float hh = std::min(1.0f, v) * hh0;
        dl->AddLine(ImVec2(x, mid - hh), ImVec2(x, mid + hh), theme::withAlpha(K.fill, 0.45f), 2.0f);
      }
      float span = std::max(0.1f, lv.v(TvLatency));
      float lx = w0.x + 4 + (w1.x - w0.x - 8) * (1.0f - std::clamp(lv.v(TvLag) / span, 0.0f, 1.0f));
      dl->AddLine(ImVec2(lx, w0.y + 4), ImVec2(lx, w1.y - 4), orange, 2);
    }
    mono(dl, ImVec2(w0.x + 6, w0.y + 3), K.graphText, "RECORDED INPUT  (last 6 s)");
  }
  F.end(kW, kH);
}

// ============================================================ Band splitter
// Reference splitter panel 770 × 340: band display on top, crossover wheels
// left and per-band strips right (band colours red / yellow / lime / green).
const skin::Skin& splitSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2F3233, 0x272A2B, 0x2B2E2F, 0xFF4E60, 0x383A3F, 0x9CA2A5);
    s.select = skin::hex(0x83EB43);
    s.lcdBg = skin::hex(0x282A2B);
    s.lcdText = skin::hex(0xC9CDD0);
    s.graphBg = skin::hex(0x1E2021);
    s.graphGrid = skin::hex(0x2C2F30);
    s.graphGridStrong = skin::hex(0x434748);
    s.pillBg = skin::hex(0x383B3C);
    return s;
  }();
  return k;
}

void editorSplitterLegacy(App& a, const PluginTarget& t, float width) {
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  const auto& P = pal();
  Live lv = live(a, t);
  const double sr = a.engine.sampleRate();
  const int nb = std::clamp(static_cast<int>(val(st, SpBands) + 0.5f), 0, 2) + 2;
  const int slope = std::clamp(static_cast<int>(val(st, SpSlope) + 0.5f), 0, 2);
  double f[3] = {val(st, SpFreq1), val(st, SpFreq2), val(st, SpFreq3)};
  for (int k = 1; k < 3; ++k) f[k] = std::max(f[k], f[k - 1] * 1.01);

  const skin::Skin& K = splitSkin();
  constexpr float kW = 770, kH = 340;
  skin::Form F = skin::Form::fit(width, kW);
  skin::panel(ImGui::GetWindowDrawList(), F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  ImVec2 p = F.p(7, 7);
  ImVec2 sz = F.sz(755, 212);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##split", sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p.x + sz.x, p.y + sz.y);
  panel(dl, p, p1);
  {
    const int N = 160;
    float db[N];
    pui::spectrum(t.tap(a), t.key(), N, db, sr);
    for (int k = 0; k < N; ++k) {
      float x0 = p.x + sz.x * k / N, x1 = p.x + sz.x * (k + 1) / N;
      float hh = std::clamp((db[k] + 90.0f) / 90.0f, 0.0f, 1.0f) * sz.y;
      dl->AddRectFilled(ImVec2(x0, p1.y - hh), ImVec2(x1, p1.y), theme::withAlpha(P.text, 0.06f));
    }
  }
  for (double dec : {10.0, 100.0, 1000.0, 10000.0})
    for (int m = 1; m <= 9; ++m) {
      double fq = dec * m;
      if (fq < 20 || fq > 20000) continue;
      float x = pui::freqToX(fq, p.x, sz.x);
      dl->AddLine(ImVec2(x, p.y), ImVec2(x, p1.y), m == 1 ? P.gridBar : P.gridStep);
      if (m == 1 || m == 2 || m == 5) {
        char buf[16];
        std::snprintf(buf, sizeof buf, fq >= 1000 ? "%gk" : "%g", fq >= 1000 ? fq / 1000 : fq);
        mono(dl, ImVec2(x + 3, p1.y - 14), P.textFaint, buf);
      }
    }
  for (int dbv = -18; dbv <= 18; dbv += 6) {
    float y = p.y + sz.y * 0.5f - dbv / 24.0f * sz.y * 0.5f;
    dl->AddLine(ImVec2(p.x, y), ImVec2(p1.x, y), dbv == 0 ? P.lineStrong : P.gridStep);
    if (dbv != 0) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "%+d", dbv);
      mono(dl, ImVec2(p1.x - 24, y - 12), P.textFaint, buf);
    }
  }
  // Band shading and per-band responses.
  float xs[5] = {p.x, 0, 0, 0, 0};
  for (int k = 0; k < nb - 1; ++k) xs[k + 1] = pui::freqToX(f[k], p.x, sz.x);
  xs[nb] = p1.x;
  bool anySolo = false;
  for (int b = 0; b < nb; ++b) anySolo = anySolo || val(st, splitParam(b, SbSolo)) >= 0.5f;
  for (int b = 0; b < nb; ++b)
    dl->AddRectFilled(ImVec2(xs[b], p.y), ImVec2(xs[b + 1], p1.y), theme::withAlpha(P.text, b % 2 ? 0.035f : 0.0f));
  blk::LrCrossover xo[3];
  for (int k = 0; k < nb - 1; ++k) xo[k].design(sr, f[k], slope);
  auto bandMag = [&](int b, double hz) {
    double m = 1;
    for (int k = 0; k < nb - 1 && k <= b; ++k) {
      const blk::LrCrossover& x = xo[k];
      for (int i = 0; i < x.sections; ++i) m *= k == b ? x.lp[i].magnitude(hz, sr) : x.hp[i].magnitude(hz, sr);
    }
    return m;
  };
  auto yOf = [&](double dbv) { return p.y + sz.y * 0.5f - static_cast<float>(std::clamp(dbv, -24.0, 24.0) / 24.0) * sz.y * 0.5f; };
  std::vector<ImVec2> pts;
  for (int b = 0; b < nb; ++b) {
    bool audible = val(st, splitParam(b, SbMute)) < 0.5f && (!anySolo || val(st, splitParam(b, SbSolo)) >= 0.5f);
    double g = audible ? blk::dbGain(val(st, splitParam(b, SbGain))) : 0.0;
    pts.clear();
    for (int i = 0; i <= 200; ++i) {
      double hz = 20.0 * std::pow(1000.0, i / 200.0);
      double m = bandMag(b, hz) * g;
      pts.push_back(ImVec2(pui::freqToX(hz, p.x, sz.x), yOf(blk::toDb(m))));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), audible ? theme::withAlpha(P.text, 0.55f) : theme::withAlpha(P.text, 0.15f), 0, 1.4f);
    // Band peak meter at the top.
    float pk = lv.fx ? lv.fx->tel.a[b].load(std::memory_order_relaxed) : -120.f;
    float mw = std::clamp((pk + 60.0f) / 60.0f, 0.0f, 1.0f) * (xs[b + 1] - xs[b] - 16);
    if (mw > 1) dl->AddRectFilled(ImVec2(xs[b] + 8, p.y + 6), ImVec2(xs[b] + 8 + mw, p.y + 10), pk > -0.5f ? P.danger : P.textDim, 2);
  }
  // Summed response (flat unless band gains differ).
  pts.clear();
  for (int i = 0; i <= 200; ++i) {
    double hz = 20.0 * std::pow(1000.0, i / 200.0);
    // Magnitude of the complex sum: the bands are phase-aligned, so add magnitudes with their LR polarity.
    double re = 0;
    for (int b = 0; b < nb; ++b) {
      bool audible = val(st, splitParam(b, SbMute)) < 0.5f && (!anySolo || val(st, splitParam(b, SbSolo)) >= 0.5f);
      if (audible) re += bandMag(b, hz) * blk::dbGain(val(st, splitParam(b, SbGain)));
    }
    pts.push_back(ImVec2(pui::freqToX(hz, p.x, sz.x), yOf(blk::toDb(re))));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), P.paper, 0, 2.0f);
  // Crossover handles.
  ImGuiIO& io = ImGui::GetIO();
  static int dragX = -1;
  if (ImGui::IsItemActivated()) {
    dragX = -1;
    float best = 12;
    for (int k = 0; k < nb - 1; ++k)
      if (std::fabs(io.MousePos.x - xs[k + 1]) < best) {
        best = std::fabs(io.MousePos.x - xs[k + 1]);
        dragX = k;
      }
  }
  for (int k = 0; k < nb - 1; ++k) {
    bool hot = dragX == k && ImGui::IsItemActive();
    dl->AddLine(ImVec2(xs[k + 1], p.y + 14), ImVec2(xs[k + 1], p1.y), hot ? P.accent : P.paper, hot ? 2.5f : 1.5f);
    char buf[24];
    std::snprintf(buf, sizeof buf, f[k] >= 1000 ? "%.2fk" : "%.0f", f[k] >= 1000 ? f[k] / 1000 : f[k]);
    float tw = monoWidth(buf);
    float tx = std::clamp(xs[k + 1] - tw * 0.5f, p.x + 4, p1.x - tw - 4);
    dl->AddRectFilled(ImVec2(tx - 3, p.y + 14), ImVec2(tx + tw + 3, p.y + 28), P.surface800, 3);
    mono(dl, ImVec2(tx, p.y + 15), P.text, buf);
  }
  if (ImGui::IsItemActive() && dragX >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
    setP(a, t, SpFreq1 + dragX, pui::xToFreq(io.MousePos.x, p.x, sz.x), true);
  if (ImGui::IsItemDeactivated()) {
    dragX = -1;
    a.doc.endGesture();
  }
  if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) a.setHint("Drag the split lines to move the crossovers; curves show each band's response");

  // Controls (reference row under the band display): band count + split
  // wheels + slope on the left, one strip per band on the right.
  using namespace flskin;
  static const uint32_t bandCol[4] = {0xFF4E60, 0xF5CC47, 0xCED671, 0x83EB43};
  {
    static const char* bands[3] = {"2", "3", "4"};
    sel(a, t, SpBands, F, 10, 228, 40, 54, K, false, bands);
    cap(dl, F, 30, 285, "BANDS", K.textDim);
    for (int k = 0; k < 3; ++k) {
      ImGui::BeginDisabled(k >= nb - 1);
      char lb[12];
      std::snprintf(lb, sizeof lb, "SPLIT %d", k + 1);
      W(a, t, SpFreq1 + k, F, 60 + k * 58, 228, 46, K, lb, skin::WheelBigLine, skin::hex(bandCol[k == 0 ? 0 : k == 1 ? 1 : 3]));
      ImGui::EndDisabled();
      if (k >= nb - 1) dim(dl, F, 58 + k * 58, 226, 50, 70, K);
    }
    static const char* slopes[3] = {"12", "24", "48"};
    sel(a, t, SpSlope, F, 240, 228, 44, 54, K, false, slopes);
    cap(dl, F, 262, 285, "dB/OCT", K.textDim);
  }
  static const char* names[3][4] = {{"LOW", "HIGH", "", ""}, {"LOW", "MID", "HIGH", ""}, {"LOW", "LO MID", "HI MID", "HIGH"}};
  for (int b = 0; b < 4; ++b) {
    ImGui::PushID(b);
    const bool on = b < nb;
    const float x0 = 296 + b * 118;
    const char* title = on ? names[nb - 2][b] : "-";
    box(dl, F, x0, 224, 112, 110, K, title, 16);
    if (on) dl->AddRectFilled(F.p(x0 + 4, 237), F.p(x0 + 108, 239), skin::hex(bandCol[nb == 2 ? (b ? 3 : 0) : nb == 3 ? (b == 2 ? 3 : b) : b]));
    ImGui::BeginDisabled(!on);
    W(a, t, splitParam(b, SbGain), F, x0 + 4, 246, 36, K, "GAIN", skin::WheelBigLine,
      skin::hex(bandCol[nb == 2 ? (b ? 3 : 0) : nb == 3 ? (b == 2 ? 3 : b) : b]));
    W(a, t, splitParam(b, SbPan), F, x0 + 42, 250, 30, K, "PAN");
    W(a, t, splitParam(b, SbWidth), F, x0 + 77, 250, 30, K, "WIDTH");
    chk(a, t, splitParam(b, SbMute), F, x0 + 8, 304, 44, 16, K, "MUTE", skin::hex(0xA3A3A3));
    chk(a, t, splitParam(b, SbSolo), F, x0 + 58, 304, 44, 16, K, "SOLO", skin::hex(0xF5CC47));
    ImGui::EndDisabled();
    if (!on) dim(dl, F, x0, 224, 112, 110, K);
    ImGui::PopID();
  }
  {
    static const char* md[2] = {"LEGACY", "REFERENCE"};
    sel(a, t, SpModel, F, 10, 306, 140, 20, K, true, md);
  }
  F.end(kW, kH);
}

// Reference model (J-1): the reference panel's crossover strip (slope menus,
// crossover wheels, band count, linked mode), MAIN OUT and SENDS groups
// (level wheel + mute per band, send target digits) and the bottom row
// (linear phase, precision, speed, graph monitor). Right-click a mute box =
// solo that band (main group).
void editorSplitterRef(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  const auto& P = pal();
  Live lv = live(a, t);
  const double sr = a.engine.sampleRate();
  const int nb = std::clamp(static_cast<int>(val(st, SpBands) + 0.5f), 0, 2) + 2;
  double f[3] = {val(st, SpFreq1), val(st, SpFreq2), val(st, SpFreq3)};
  if (nb >= 4)
    std::sort(f, f + 3);
  else if (nb == 3)
    std::sort(f, f + 2);
  int sl[3];
  for (int k = 0; k < 3; ++k) sl[k] = std::clamp(static_cast<int>(val(st, SpSlopeA + k) + 0.5f), 0, kSplitSlopes - 1);
  const bool sends = val(st, SpMonitor) >= 0.5f;
  const bool sumView = val(st, sends ? SpViewSend : SpViewMain) >= 0.5f;

  const skin::Skin& K = splitSkin();
  constexpr float kW = 770, kH = 340;
  skin::Form F = skin::Form::fit(width, kW);
  skin::panel(ImGui::GetWindowDrawList(), F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  static const uint32_t bandCol[4] = {0xFF4E60, 0xF5CC47, 0xCED671, 0x83EB43};
  auto colOf = [&](int b) { return skin::hex(bandCol[nb == 2 ? (b ? 3 : 0) : nb == 3 ? (b == 2 ? 3 : b) : b]); };

  // ---------------- band graph ----------------
  ImVec2 p = F.p(7, 7), sz = F.sz(755, 212);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##splitref", sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p.x + sz.x, p.y + sz.y);
  panel(dl, p, p1);
  {
    const int N = 160;
    float db[N];
    pui::spectrum(t.tap(a), t.key(), N, db, sr);
    for (int k = 0; k < N; ++k) {
      float x0 = p.x + sz.x * k / N, x1 = p.x + sz.x * (k + 1) / N;
      float hh = std::clamp((db[k] + 90.0f) / 90.0f, 0.0f, 1.0f) * sz.y;
      dl->AddRectFilled(ImVec2(x0, p1.y - hh), ImVec2(x1, p1.y), theme::withAlpha(P.text, 0.06f));
    }
  }
  for (double dec : {10.0, 100.0, 1000.0, 10000.0})
    for (int m = 1; m <= 9; ++m) {
      double fq = dec * m;
      if (fq < 20 || fq > 20000) continue;
      float x = pui::freqToX(fq, p.x, sz.x);
      dl->AddLine(ImVec2(x, p.y), ImVec2(x, p1.y), m == 1 ? P.gridBar : P.gridStep);
      if (m == 1 || m == 2 || m == 5) {
        char buf[16];
        std::snprintf(buf, sizeof buf, fq >= 1000 ? "%gk" : "%g", fq >= 1000 ? fq / 1000 : fq);
        mono(dl, ImVec2(x + 3, p1.y - 14), P.textFaint, buf);
      }
    }
  auto yOf = [&](double dbv) { return p.y + sz.y * 0.5f - static_cast<float>(std::clamp(dbv, -24.0, 24.0) / 24.0) * sz.y * 0.5f; };
  for (int dbv = -18; dbv <= 18; dbv += 6) {
    float y = yOf(dbv);
    dl->AddLine(ImVec2(p.x, y), ImVec2(p1.x, y), dbv == 0 ? P.lineStrong : P.gridStep);
    if (dbv != 0) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "%+d", dbv);
      mono(dl, ImVec2(p1.x - 24, y - 12), P.textFaint, buf);
    }
  }
  float xs[5] = {p.x, 0, 0, 0, 0};
  for (int k = 0; k < nb - 1; ++k) xs[k + 1] = pui::freqToX(f[k], p.x, sz.x);
  xs[nb] = p1.x;
  for (int b = 0; b < nb; ++b) dl->AddRectFilled(ImVec2(xs[b], p.y), ImVec2(xs[b + 1], p1.y), theme::withAlpha(colOf(b), 0.05f));
  bool anySolo = false;
  for (int b = 0; b < nb; ++b) anySolo = anySolo || val(st, splitParam(b, SbSolo)) >= 0.5f;
  auto bandGain = [&](int b) {
    if (sends) return val(st, splitParam2(b, SxSendMute)) >= 0.5f ? 0.0 : splitLevelGain(val(st, splitParam2(b, SxSend)));
    bool audible = val(st, splitParam(b, SbMute)) < 0.5f && (!anySolo || val(st, splitParam(b, SbSolo)) >= 0.5f);
    return audible ? splitLevelGain(val(st, splitParam2(b, SxMain))) * blk::dbGain(val(st, splitParam(b, SbGain))) : 0.0;
  };
  auto bandMag = [&](int b, double hz) {
    double m = 1;
    for (int x = 0; x < nb - 1 && x <= b; ++x) {
      double r = std::tan(blk::kPi * std::min(hz, sr * 0.499) / sr) / std::tan(blk::kPi * std::min(f[x], sr * 0.49) / sr), lp, hp;
      blk::slopeMag(sl[x], r, lp, hp);
      m *= x == b ? lp : hp;
    }
    return m;
  };
  std::vector<ImVec2> pts;
  if (!sumView)
    for (int b = 0; b < nb; ++b) {
      double g = bandGain(b);
      pts.clear();
      for (int i = 0; i <= 200; ++i) {
        double hz = 20.0 * std::pow(1000.0, i / 200.0);
        pts.push_back(ImVec2(pui::freqToX(hz, p.x, sz.x), yOf(blk::toDb(bandMag(b, hz) * g))));
      }
      dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(colOf(b), g > 0 ? 0.9f : 0.25f), 0, 1.6f);
    }
  // Summed (magnitude, LR-phase-aligned) response of the shown group.
  pts.clear();
  for (int i = 0; i <= 200; ++i) {
    double hz = 20.0 * std::pow(1000.0, i / 200.0), re = 0;
    for (int b = 0; b < nb; ++b) re += bandMag(b, hz) * bandGain(b);
    pts.push_back(ImVec2(pui::freqToX(hz, p.x, sz.x), yOf(blk::toDb(re))));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), sumView ? P.paper : theme::withAlpha(P.paper, 0.45f), 0, sumView ? 2.0f : 1.2f);
  for (int b = 0; b < nb && lv.fx; ++b) {
    float pk = lv.fx->tel.a[b].load(std::memory_order_relaxed);
    float mw = std::clamp((pk + 60.0f) / 60.0f, 0.0f, 1.0f) * (xs[b + 1] - xs[b] - 16);
    if (mw > 1) dl->AddRectFilled(ImVec2(xs[b] + 8, p.y + 6), ImVec2(xs[b] + 8 + mw, p.y + 10), pk > -0.5f ? P.danger : colOf(b), 2);
  }
  // Crossover handles (linked mode keeps the ratio between them).
  ImGuiIO& io = ImGui::GetIO();
  static int dragX = -1;
  if (ImGui::IsItemActivated()) {
    dragX = -1;
    float best = 12;
    for (int k = 0; k < nb - 1; ++k)
      if (std::fabs(io.MousePos.x - xs[k + 1]) < best) {
        best = std::fabs(io.MousePos.x - xs[k + 1]);
        dragX = k;
      }
  }
  for (int k = 0; k < nb - 1; ++k) {
    bool hot = dragX == k && ImGui::IsItemActive();
    dl->AddLine(ImVec2(xs[k + 1], p.y + 14), ImVec2(xs[k + 1], p1.y), hot ? P.accent : P.paper, hot ? 2.5f : 1.5f);
    char buf[24];
    std::snprintf(buf, sizeof buf, f[k] >= 1000 ? "%.2fk" : "%.0f", f[k] >= 1000 ? f[k] / 1000 : f[k]);
    float tw = monoWidth(buf);
    float tx = std::clamp(xs[k + 1] - tw * 0.5f, p.x + 4, p1.x - tw - 4);
    dl->AddRectFilled(ImVec2(tx - 3, p.y + 14), ImVec2(tx + tw + 3, p.y + 28), P.surface800, 3);
    mono(dl, ImVec2(tx, p.y + 15), P.text, buf);
  }
  if (ImGui::IsItemActive() && dragX >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
    double nf = std::clamp(pui::xToFreq(io.MousePos.x, p.x, sz.x), 20.0, 20000.0);
    if (val(st, SpLinked) >= 0.5f) {
      double ratio = nf / std::max(1.0, f[dragX]);
      for (int k = 0; k < nb - 1; ++k) setP(a, t, SpFreq1 + k, std::clamp(f[k] * ratio, 20.0, 20000.0), true);
    } else {
      setP(a, t, SpFreq1 + dragX, nf, true);
    }
  }
  if (ImGui::IsItemDeactivated()) {
    dragX = -1;
    a.doc.endGesture();
  }
  if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
    a.setHint(sends ? "Send levels per band (curves); drag the split lines to move the crossovers"
                    : "Main output per band (curves); drag the split lines to move the crossovers");
  mono(dl, ImVec2(p.x + 10, p.y + 18), P.textFaint, sends ? "SENDS" : "MAIN");

  // ---------------- crossover strip ----------------
  menu(a, t, SpSlopeA, F, 12, 241, 48, 23, K);
  cap(dl, F, 36, 267, "SLOPE", K.textDim);
  W(a, t, SpFreq1, F, 69, 228, 48, K, nb == 2 ? "LOW/HIGH" : "LOW/MID", skin::WheelBigLine, colOf(0));
  {
    static const char* bc[3] = {"2", "3", "4"};
    sel(a, t, SpBands, F, 124, 240, 44, 18, K, true, bc);
  }
  chk(a, t, SpLinked, F, 126, 274, 44, 14, K, "LINK", K.select);
  ImGui::BeginDisabled(nb < 3);
  W(a, t, SpFreq2, F, 174, 228, 48, K, "MID/HIGH", skin::WheelBigLine, colOf(nb - 1));
  menu(a, t, SpSlopeB, F, 232, 241, 48, 23, K);
  ImGui::EndDisabled();
  cap(dl, F, 256, 267, "SLOPE", K.textDim);
  if (nb < 3) dim(dl, F, 170, 226, 114, 60, K);
  if (nb == 4) {
    W(a, t, SpFreq3, F, 248, 280, 22, K, nullptr, skin::WheelBigLine, colOf(3));
    menu(a, t, SpSlopeC, F, 196, 282, 48, 18, K);
  }

  // ---------------- MAIN OUT / SENDS groups ----------------
  static const char* names[3][4] = {{"LOW", "HIGH", "", ""}, {"LOW", "MID", "HIGH", ""}, {"LOW", "LO MID", "HI MID", "HIGH"}};
  const int ncol = nb == 4 ? 4 : 3;
  auto bandOfCol = [&](int c) { return nb == 2 ? (c == 0 ? 0 : (c == 2 ? 1 : -1)) : c; };
  box(dl, F, 292, 222, 194, 76, K, nullptr);
  box(dl, F, 488, 222, 278, 76, K, nullptr);
  text(dl, F, 298, 288, "MAIN OUT", K.textDim);
  text(dl, F, 494, 288, "SENDS", K.textDim);
  for (int c = 0; c < ncol; ++c) {
    const int b = bandOfCol(c);
    ImGui::PushID(c);
    const float mx = 300 + c * (ncol == 4 ? 44.f : 59.f), sx = 492 + c * (ncol == 4 ? 68.f : 89.f);
    if (b < 0) {
      dim(dl, F, mx - 4, 226, 52, 56, K);
      dim(dl, F, sx - 2, 226, 82, 56, K);
      ImGui::PopID();
      continue;
    }
    cap(dl, F, mx + 18, 224, names[nb - 2][b], K.textDim, 6.5f);
    W(a, t, splitParam2(b, SxMain), F, mx + 2, 232, 32, K, nullptr, skin::WheelBigLine, colOf(b));
    chk(a, t, splitParam(b, SbMute), F, mx, 268, 36, 12, K, val(st, splitParam(b, SbSolo)) >= 0.5f ? "SOLO" : "", colOf(b));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) setP(a, t, splitParam(b, SbSolo), val(st, splitParam(b, SbSolo)) >= 0.5f ? 0 : 1, false);
    if (ImGui::IsItemHovered()) a.setHint("Band mute (right-click: solo)");
    {
      int to = static_cast<int>(std::lround(val(st, splitParam2(b, SxSendTo))));
      char tb[12];
      if (to < 0)
        std::snprintf(tb, sizeof tb, "--");
      else
        std::snprintf(tb, sizeof tb, "%d", to);
      skin::digi(a, t, splitParam2(b, SxSendTo), F.p(sx, 237), F.sz(ncol == 4 ? 30 : 37, 20), K, tb, F.font(9));
      if (ImGui::IsItemHovered()) a.setHint("Send this band to mixer insert (drag; -- = none)");
    }
    const float wx = sx + (ncol == 4 ? 33 : 42);
    W(a, t, splitParam2(b, SxSend), F, wx, 232, 32, K, nullptr, skin::WheelBigLine, colOf(b));
    chk(a, t, splitParam2(b, SxSendMute), F, wx - 2, 268, 36, 12, K, "", colOf(b));
    ImGui::PopID();
  }
  {
    static const char* vw[2] = {"BANDS", "SUM"};
    sel(a, t, SpViewMain, F, 400, 284, 82, 12, K, true, vw);
    sel(a, t, SpViewSend, F, 680, 284, 82, 12, K, true, vw);
  }

  // ---------------- bottom row ----------------
  tog(a, t, SpLinear, F, 4, 304, 96, 30, K, "LINEAR PHASE");
  ImGui::BeginDisabled(val(st, SpLinear) < 0.5f);
  menu(a, t, SpQuality, F, 110, 309, 92, 22, K);
  cap(dl, F, 156, 333, "PRECISION", K.textDim, 6.5f);
  skin::hslider(a, t, SpLinSpeed, F.p(214, 309), F.sz(110, 22), K);
  cap(dl, F, 269, 333, "SPEED", K.textDim, 6.5f);
  ImGui::EndDisabled();
  if (val(st, SpLinear) >= 0.5f && lv.fx) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d smp", static_cast<int>(lv.v(TvLatency)));
    text(dl, F, 330, 316, buf, K.textDim);
  }
  {
    static const char* mo[2] = {"MAIN", "SENDS"};
    sel(a, t, SpMonitor, F, 379, 309, 96, 20, K, true, mo);
    cap(dl, F, 427, 331, "GRAPH", K.textDim, 6.5f);
  }
  {
    static const char* md[2] = {"LEGACY", "REFERENCE"};
    sel(a, t, SpModel, F, 640, 309, 124, 20, K, true, md);
  }
  F.end(kW, kH);
}

void editorSplitter(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (st && val(*st, SpModel) >= 0.5f)
    editorSplitterRef(a, t, width);
  else
    editorSplitterLegacy(a, t, width);
}


// ================================================================= Spreader
// Reference spreader panel 540 × 255: enable switch, the spread /
// separation / low-bypass wheels and the mono switch in the left column, the
// vectorscope (input trace dimmed, output trace in the accent) with the pre-
// and post-correlation bars and their monitor switches filling the rest.
// Gridline's stage options (method, L/R offset, pan, volume, polarity, order,
// mono check, DC removal, engine) in a strip underneath (540 × 300).
const skin::Skin& spreadSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3E4244, 0x35393B, 0x959595, 0xFFFFFF, 0x42474A, 0xC4C4C4);
    s.fillPressed = skin::hex(0xFFA830);
    s.select = skin::hex(0xDB592A);
    s.lcdBg = skin::hex(0x26292A);
    s.graphBg = skin::hex(0x222527);
    s.graphGrid = skin::hex(0x33373A);
    s.graphGridStrong = skin::hex(0x4A4F52);
    s.accent2 = skin::hex(0xC0A74E);
    return s;
  }();
  return k;
}

void editorSpread(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  Live lv = live(a, t);
  const skin::Skin& K = spreadSkin();
  constexpr float kW = 540, kH = 300;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  const ImU32 orange = skin::hex(0xDB592A);
  const bool ref = val(st, SrModel) >= 0.5f;
  const bool enabled = !ref || val(st, SrEnabled) >= 0.5f;
  const bool split = ref ? (val(st, SrLowBypass) >= 0.5f || val(st, SrBassMono) >= 0.5f) : val(st, SrBassMono) >= 0.5f;
  const bool showPre = ref && val(st, SrPreMonitor) >= 0.5f, showPost = !ref || val(st, SrPostMonitor) >= 0.5f;
  // Left column.
  ImGui::BeginDisabled(!ref);
  chk(a, t, SrEnabled, F, 12, 6, 60, 16, K, "ON", orange);
  ImGui::EndDisabled();
  W(a, t, SrSpread, F, 16, 33, 44, K, "SPREAD");
  W(a, t, SrSeparation, F, 16, 101, 44, K, "SEPARATE", skin::WheelBigLine | skin::WheelCentered);
  ImGui::BeginDisabled(!split);
  W(a, t, ref ? SrLowFreq : SrBassFreq, F, 16, 167, 44, K, ref ? "LOW BYPASS" : "BASS HZ");
  ImGui::EndDisabled();
  if (ref) {
    chk(a, t, SrLowBypass, F, 6, 230, 66, 14, K, "LOW", orange);
    if (ImGui::IsItemHovered()) a.setHint("Lows below the split keep their original stereo (they bypass the widener)");
  }
  chk(a, t, SrBassMono, F, 6, ref ? 248 : 232, 66, 14, K, "MONO", orange);
  if (ImGui::IsItemHovered()) a.setHint("Mono below the split frequency");
  // Vectorscope + correlation bars.
  {
    ImVec2 p = F.p(80, 6), p1 = F.p(534, 214);
    skin::graphWell(dl, p, p1, K);
    const float w = p1.x - p.x, h = p1.y - p.y;
    const float R = std::min(w * 0.5f - 14, h - 44);
    ImVec2 c(p.x + w * 0.5f, p1.y - 10);
    // Half-disc scope: mid up, sides left/right.
    dl->PathArcTo(c, R, static_cast<float>(blk::kPi), static_cast<float>(2 * blk::kPi), 48);
    dl->PathStroke(P.gridBar, 0, 1.0f);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), P.gridBar);
    dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y - R), P.gridStep);
    dl->AddLine(c, ImVec2(c.x - R * 0.7071f, c.y - R * 0.7071f), P.gridStep);
    dl->AddLine(c, ImVec2(c.x + R * 0.7071f, c.y - R * 0.7071f), P.gridStep);
    mono(dl, ImVec2(c.x - R * 0.7071f - 12, c.y - R * 0.7071f - 12), K.graphText, "L");
    mono(dl, ImVec2(c.x + R * 0.7071f + 4, c.y - R * 0.7071f - 12), K.graphText, "R");
    mono(dl, ImVec2(c.x - 3, c.y - R - 13), K.graphText, "M");
    if (lv.fx) {
      float l[128], r[128], il[128], ir[128];
      float mx = 1e-4f;
      for (int i = 0; i < 128; ++i) {
        l[i] = lv.fx->tel.wave[static_cast<size_t>(i * 2)].load(std::memory_order_relaxed);
        r[i] = lv.fx->tel.wave[static_cast<size_t>(i * 2 + 1)].load(std::memory_order_relaxed);
        il[i] = showPre ? lv.fx->tel.a[static_cast<size_t>(i)].load(std::memory_order_relaxed) : 0.f;
        ir[i] = showPre ? lv.fx->tel.b[static_cast<size_t>(i)].load(std::memory_order_relaxed) : 0.f;
        if (std::fabs(il[i]) > 50) il[i] = 0;  // unwritten slots
        if (std::fabs(ir[i]) > 50) ir[i] = 0;
        mx = std::max({mx, std::fabs(l[i]), std::fabs(r[i]), std::fabs(il[i]), std::fabs(ir[i])});
      }
      const float k = R * 0.9f / std::max(0.05f, mx);
      auto dot = [&](float a1, float b1, ImU32 col) {
        float m = (a1 + b1) * 0.7071f, sd = (b1 - a1) * 0.7071f;
        dl->AddCircleFilled(ImVec2(c.x + sd * k, c.y - std::fabs(m) * k), 1.6f, col);
      };
      if (showPre)
        for (int i = 0; i < 128; ++i) dot(il[i], ir[i], theme::withAlpha(K.textDim, 0.45f));
      if (showPost)
        for (int i = 0; i < 128; ++i) dot(l[i], r[i], theme::withAlpha(K.fillPressed, 0.8f));
    }
    auto bar = [&](float x0, float x1, float corr, bool on, const char* label) {
      float y = F.p(0, 30).y, xa = F.p(x0, 0).x, xb = F.p(x1, 0).x;
      dl->AddRectFilled(ImVec2(xa, y), ImVec2(xb, y + F.u(6)), K.lcdBg, 2);
      float cx = (xa + xb) * 0.5f;
      dl->AddLine(ImVec2(cx, y - F.u(2)), ImVec2(cx, y + F.u(8)), P.lineStrong);
      if (on) {
        float vx = cx + std::clamp(corr, -1.0f, 1.0f) * (xb - xa) * 0.5f;
        dl->AddRectFilled(ImVec2(std::min(cx, vx), y + 1), ImVec2(std::max(cx, vx), y + F.u(6) - 1), corr < 0 ? P.danger : K.accent2C(), 2);
      }
      skin::text(dl, F.p(x0, 38), K.graphText, label, F.font(6.0f));
    };
    float post = lv.fx ? lv.v(TvCorrelation) : 1.0f, pre = lv.fx ? lv.v(TvSrPreCorr) : 1.0f;
    bar(94, 274, pre, showPre, "PRE");
    bar(290, 470, post, showPost, "POST");
    if (ref) {
      chk(a, t, SrPreMonitor, F, 86, 10, 50, 13, K, "IN", orange);
      chk(a, t, SrPostMonitor, F, 476, 10, 54, 13, K, "OUT", orange);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "CORR %+.2f   WIDTH %.2f", post, lv.v(TvWidth));
    mono(dl, ImVec2(p.x + (w - monoWidth(buf)) * 0.5f, p.y + F.u(46)), K.graphText, buf);
    if (!enabled) mono(dl, ImVec2(p.x + (w - monoWidth("BYPASSED")) * 0.5f, p.y + F.u(60)), orange, "BYPASSED");
  }
  // Gridline strip: method, offsets, output, polarity, order, checks, engine.
  box(dl, F, 80, 220, 454, 74, K, nullptr);
  {
    static const char* modes[4] = {"COMPLEMENT", "HAAS", "CHORUS", "MID/SIDE"};
    sel(a, t, SrMode, F, 86, 226, 92, 62, K, false, modes);
  }
  W(a, t, SrOffset, F, 186, 228, 32, K, "OFFSET", skin::WheelBigLine | skin::WheelCentered);
  W(a, t, SrPan, F, 226, 228, 32, K, "PAN", skin::WheelBigLine | skin::WheelCentered);
  W(a, t, SrVolume, F, 266, 228, 32, K, "VOLUME", skin::WheelBigLine | skin::WheelCentered);
  text(dl, F, 308, 224, "POLARITY", K.textDim, 6.5f);
  {
    static const char* inv[4] = {"NORM", "L", "R", "BOTH"};
    sel(a, t, SrInvert, F, 306, 234, 120, 16, K, true, inv);
  }
  text(dl, F, 308, 256, "SEPARATION", K.textDim, 6.5f);
  {
    static const char* pos[2] = {"BEFORE", "AFTER"};
    sel(a, t, SrPosition, F, 306, 266, 120, 16, K, true, pos);
  }
  chk(a, t, SrMonoCheck, F, 436, 226, 92, 14, K, "MONO CHECK", orange);
  chk(a, t, SrDcBlock, F, 436, 244, 92, 14, K, "NO DC", orange);
  {
    static const char* models[2] = {"LEGACY", "REF"};
    sel(a, t, SrModel, F, 436, 266, 92, 16, K, true, models);
  }
  F.end(kW, kH);
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.vocoder") editorVocoder(a, t, width);
  else if (ty == "gl.pitch") editorPitch(a, t, width);
  else if (ty == "gl.pitcher") editorPitcher(a, t, width);
  else if (ty == "gl.scratcher") editorScratcher(a, t, width);
  else if (ty == "gl.splitter") editorSplitter(a, t, width);
  else if (ty == "gl.spread") editorSpread(a, t, width);
  else return false;
  return true;
}

}  // namespace gl::packs::spectral
