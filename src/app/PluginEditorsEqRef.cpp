// Gridline — Parametric EQ editor, laid out on the reference EQ's 600 × 285
// panel grid (graph left, band strip right, tool bar under the graph) and
// drawn with a per-plugin rack skin. Every position below is in panel units
// and scaled to the window width.
//
// Graph: log 20 Hz–20 kHz × ±18 dB; octave and region rows above, frequency
// labels below; histogram / heat map of the monitored signal; per-band
// response fills in the band colours; numbered band tokens.
//   drag token          frequency (x) + level (y)
//   Shift + drag        solo the band while held (when "band solo on Shift" is
//                       on): frequency (x) + width (y); otherwise width (x) + level (y)
//   middle drag         width (x) + level (y)
//   wheel over token    width (Shift: level); elsewhere: main level; Ctrl = 5× steps
//   drag empty graph    main level
//   Alt + click         reset the band's level, frequency and width
//   double-click token  band on / off; double-click empty graph: enable a free band there
//   right-click token   band menu (reset, type, order, key = snap to a note)
#include <map>
#include "../engine/Dsp.h"
#include "../engine/EqDesign.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

// Band colours (reference wheel fills) and the panel palette.
const uint32_t kBandHex[kEqBands] = {0x8550C7, 0xC550A2, 0xC56451, 0xC3C751, 0x5CC651, 0x4FC6A9, 0x4F7FC7};

const skin::Skin& eqSkin() {
  static skin::Skin k = [] {
    skin::Skin s = skin::Skin::make(0x29353A, 0x1C2D37, 0x8550C7, 0x986AD0, 0x1C2D37, 0x9AA8B3);
    s.panelHi = skin::hex(0x354043);
    s.panelLo = skin::hex(0x1A2932);
    s.graphBg = skin::hex(0x152027);
    s.graphGrid = skin::hex(0x26343C);
    s.graphGridStrong = skin::hex(0x34434D);
    s.graphText = skin::hex(0x84939F);
    s.handle = skin::hex(0x8B8E97);
    s.lcdBg = skin::hex(0x141E24);
    s.lcdText = skin::hex(0xC9D3DA);
    s.select = skin::hex(0x7FB2D9);
    return s;
  }();
  return k;
}

bool hasGain(int type) { return type == EqLowShelf || type == EqPeak || type == EqHighShelf; }
bool isCut(int type) { return type == EqLowCut || type == EqHighCut; }

const char* kTypeNames[8] = {"Off", "Low cut", "Band pass", "High cut", "Notch", "Low shelf", "Peak", "High shelf"};
const char* kOrderShort[7] = {"8\xC2\xB7", "6\xC2\xB7", "4\xC2\xB7", "2", "4", "6", "8"};  // steep marked with a dot
const char* kOrderNames[7] = {"Steep 8", "Steep 6", "Steep 4", "Normal (2)", "Gentle 4", "Gentle 6", "Gentle 8"};

// Filter-shape glyph for a band type.
void typeGlyph(ImDrawList* dl, int type, ImVec2 c, float w, float h, ImU32 col, float th = 1.3f) {
  ImVec2 pts[12];
  int n = 0;
  float x0 = c.x - w / 2, y0 = c.y - h / 2;
  auto P = [&](float x, float y) { pts[n++] = ImVec2(x0 + x * w, y0 + y * h); };
  switch (type) {
    case EqLowCut: P(0.05f, 1); P(0.35f, 0.35f); P(0.5f, 0.3f); P(1, 0.3f); break;
    case EqHighCut: P(0, 0.3f); P(0.5f, 0.3f); P(0.65f, 0.35f); P(0.95f, 1); break;
    case EqBandPass: P(0.05f, 1); P(0.35f, 0.4f); P(0.5f, 0.05f); P(0.65f, 0.4f); P(0.95f, 1); break;
    case EqNotch: P(0, 0.2f); P(0.38f, 0.2f); P(0.5f, 1); P(0.62f, 0.2f); P(1, 0.2f); break;
    case EqLowShelf: P(0, 0.1f); P(0.3f, 0.1f); P(0.6f, 0.65f); P(1, 0.65f); break;
    case EqHighShelf: P(0, 0.65f); P(0.4f, 0.65f); P(0.7f, 0.1f); P(1, 0.1f); break;
    case EqPeak: P(0, 0.75f); P(0.3f, 0.7f); P(0.5f, 0.05f); P(0.7f, 0.7f); P(1, 0.75f); break;
    default:
      dl->AddLine(ImVec2(x0 + w * 0.25f, y0 + h * 0.5f), ImVec2(x0 + w * 0.75f, y0 + h * 0.5f), col, th);
      return;
  }
  dl->AddPolyline(pts, n, col, 0, th);
}

struct EqUi {
  int selected = -1;  // band whose readout / menu is current
  int hover = -1;
  int drag = -1;      // band being dragged
  int dragMode = 0;   // 0 freq+level, 1 freq+width (solo), 2 width+level
  bool soloHeld = false;
  int listenBefore = 0;
  uint64_t gesture = 0;
  std::vector<float> hist, hold;           // smoothed histogram (dB) + peak hold
  std::vector<std::vector<float>> heat;    // heat-map rows (newest first), dB
  ImVec2 dragAcc{0, 0};
};
std::map<uint64_t, EqUi> eqUis;

constexpr float kFormW = 600, kFormH = 285;
// Graph rect in panel units.
constexpr float kGx = 7, kGy = 33, kGw = 400, kGh = 181;

// Band readout text: "Band 2 (Peak)  632 Hz  39%  +3.00 dB".
std::string bandReadout(const PluginState& st, int b) {
  int type = std::clamp(ival(st, eqParam(b, EqType)), 0, 7);
  char buf[160];
  std::snprintf(buf, sizeof buf, "Band %d (%s)  %.0f Hz  %d%%  %+.2f dB", b + 1, kTypeNames[type], val(st, eqParam(b, EqFreq)),
                static_cast<int>(std::lround(val(st, eqFl(b, EqWidth)) * 100)), hasGain(type) ? val(st, eqParam(b, EqGain)) : 0.0f);
  return buf;
}

// Resets a band's level / frequency / width to its defaults (keeps the type).
void resetBandValues(PluginState& ns, int b) {
  PluginState def = defaultPluginState(ns.type);
  for (int idx : {eqParam(b, EqGain), eqParam(b, EqFreq), eqFl(b, EqWidth)}) ns.params[static_cast<size_t>(idx)] = def.params[static_cast<size_t>(idx)];
}

// Changing a band's type resets its width to the type's natural default
// (cuts: Butterworth, band / notch / peak: 2 octaves, shelves: slope 1).
float defaultWidthFor(int type) {
  if (isCut(type)) return static_cast<float>(1.0 - std::cbrt(1.0 / 11.0));
  if (type == EqLowShelf || type == EqHighShelf) return 40124.0f / 65536.0f;
  return 25412.0f / 65536.0f;
}
void setBandType(App& a, const PluginTarget& t, const PluginState& st, int b, int type) {
  PluginState ns = st;
  ns.params[static_cast<size_t>(eqParam(b, EqType))] = static_cast<float>(type);
  ns.params[static_cast<size_t>(eqFl(b, EqWidth))] = defaultWidthFor(type);
  setPluginState(a, t, ns, "EQ band type");
}

void bandMenu(App& a, const PluginTarget& t, const PluginState& st, int b) {
  ImGui::TextDisabled("Band %d (%s)", b + 1, kTypeNames[std::clamp(ival(st, eqParam(b, EqType)), 0, 7)]);
  ImGui::Separator();
  if (ImGui::MenuItem("Reset")) {
    PluginState ns = st, def = defaultPluginState(st.type);
    for (int idx : {eqParam(b, EqType), eqParam(b, EqGain), eqParam(b, EqFreq), eqFl(b, EqWidth), eqFl(b, EqFlOrder), eqExt(b, EqEnabled)})
      ns.params[static_cast<size_t>(idx)] = def.params[static_cast<size_t>(idx)];
    setPluginState(a, t, ns, "reset EQ band");
  }
  int cur = ival(st, eqParam(b, EqType));
  if (ImGui::BeginMenu("Type")) {
    static const int order[8] = {EqOff, EqHighCut, EqBandPass, EqLowCut, EqNotch, EqLowShelf, EqPeak, EqHighShelf};
    for (int i : order) {
      ImVec2 ip = ImGui::GetCursorScreenPos();
      if (ImGui::MenuItem((std::string("      ") + kTypeNames[i]).c_str(), nullptr, i == cur) && i != cur) setBandType(a, t, st, b, i);
      typeGlyph(ImGui::GetWindowDrawList(), i, ImVec2(ip.x + 12, ip.y + 8), 18, 9, pal().text);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Order")) {
    int o = ival(st, eqFl(b, EqFlOrder), EqOrd2);
    for (int k = 0; k < 7; ++k)
      if (ImGui::MenuItem(kOrderNames[k], nullptr, k == o)) a.doc.setParam(t.pid(eqFl(b, EqFlOrder)), k, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Key")) {
    // Notes between 20 Hz and 20 kHz, grouped by note name (octaves inside).
    static const char* nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    double hz = val(st, eqParam(b, EqFreq));
    int curNote = static_cast<int>(std::lround(69 + 12 * std::log2(std::max(hz, 1.0) / 440.0)));
    for (int pc = 0; pc < 12; ++pc)
      if (ImGui::BeginMenu(nm[pc])) {
        for (int oct = -1; oct <= 9; ++oct) {
          int note = (oct + 1) * 12 + pc;
          double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
          if (f < 20.0 || f > 20000.0) continue;
          char buf[48];
          std::snprintf(buf, sizeof buf, "%s%d - %.1f Hz", nm[pc], oct, f);
          if (ImGui::MenuItem(buf, nullptr, note == curNote)) a.doc.setParam(t.pid(eqParam(b, EqFreq)), f, false);
        }
        ImGui::EndMenu();
      }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  bool en = bval(st, eqExt(b, EqEnabled), true);
  if (ImGui::MenuItem("Enabled", nullptr, en)) a.doc.setParam(t.pid(eqExt(b, EqEnabled)), en ? 0 : 1, false);
  int lis = ival(st, EqListen);
  if (ImGui::MenuItem("Solo (listen)", nullptr, lis == b + 1)) a.doc.setParam(t.pid(EqListen), lis == b + 1 ? 0 : b + 1, false);
}

void choiceMenu(App& a, const PluginTarget& t, const PluginState& st, int param, const char* label) {
  auto d = describeParam(a.doc.song(), t.pid(param));
  if (!d) return;
  if (ImGui::BeginMenu(label)) {
    int cur = ival(st, param);
    for (size_t k = 0; k < d->choices.size(); ++k)
      if (ImGui::MenuItem(d->choices[k].c_str(), nullptr, static_cast<int>(k) == cur)) a.doc.setParam(t.pid(param), static_cast<double>(k), false);
    ImGui::EndMenu();
  }
}

void optionsMenu(App& a, const PluginTarget& t, const PluginState& st) {
  ImGui::TextDisabled("Options");
  ImGui::Separator();
  choiceMenu(a, t, st, EqSoloShift, "Band solo on Shift");
  choiceMenu(a, t, st, EqHQ, "Quality");
  choiceMenu(a, t, st, EqLinear, "Linear phase");
  ImGui::Separator();
  ImGui::TextDisabled("Visualization");
  choiceMenu(a, t, st, EqMonitor, "Monitor");
  bool tok = bval(st, EqTokens, true);
  if (ImGui::MenuItem("Band tokens", nullptr, tok)) a.doc.setParam(t.pid(EqTokens), tok ? 0 : 1, false);
  bool ph = bval(st, EqPhaseView);
  if (ImGui::MenuItem("Phase rotation", nullptr, ph)) a.doc.setParam(t.pid(EqPhaseView), ph ? 0 : 1, false);
  ImGui::Separator();
  ImGui::TextDisabled("Histogram");
  choiceMenu(a, t, st, EqSpectrum, "Enabled");
  choiceMenu(a, t, st, EqHistRange, "Range");
  choiceMenu(a, t, st, EqPivot, "Pivot slope");
  choiceMenu(a, t, st, EqSmoothing, "Time smoothing");
  bool avg = bval(st, EqHistAvg);
  if (ImGui::MenuItem("Average mode", nullptr, avg)) a.doc.setParam(t.pid(EqHistAvg), avg ? 0 : 1, false);
  ImGui::Separator();
  ImGui::TextDisabled("Heat map");
  choiceMenu(a, t, st, EqHeatmap, "Position");
}

// A small framed caption cell (graph header rows).
void headerCell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, const skin::Skin& k, float fs) {
  dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(k.panelLo, 0.55f), 2);
  if (!text || !*text) return;
  ImGui::PushFont(app().fontMonoBold, fs);
  ImVec2 ts = ImGui::CalcTextSize(text);
  dl->PushClipRect(p0, p1, true);
  dl->AddText(ImVec2(std::round((p0.x + p1.x - ts.x) * 0.5f), std::round((p0.y + p1.y - ts.y) * 0.5f)), k.graphText, text);
  dl->PopClipRect();
  ImGui::PopFont();
}

}  // namespace

void editorEqRef(App& a, const PluginTarget& t, float width) {
  const Song& song = a.doc.song();
  const PluginState* sp = t.state(song);
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = eqSkin();
  EqUi& ui = eqUis[t.key()];
  ImGuiIO& io = ImGui::GetIO();
  const double sr = a.engine.sampleRate();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  skin::Form F{ImGui::GetCursorScreenPos(), width / kFormW};
  const float s = F.s;
  const float H = kFormH * s;
  skin::panel(dl, F.o, ImVec2(F.o.x + width, F.o.y + H), K, 4);
  const int listen = ival(st, EqListen);
  const float fsSmall = std::clamp(7.0f * s, 8.0f, 11.0f), fsMid = std::clamp(8.0f * s, 9.0f, 12.0f);

  // ================================================================ graph
  const ImVec2 g0 = F.p(kGx, kGy), g1 = F.p(kGx + kGw, kGy + kGh);
  const float gw = g1.x - g0.x, gh = g1.y - g0.y;
  auto fx = [&](double hz) { return g0.x + gw * static_cast<float>(std::log(std::max(hz, 1.0) / 20.0) / std::log(1000.0)); };
  auto xf = [&](float x) { return 20.0 * std::pow(1000.0, std::clamp((x - g0.x) / gw, 0.0f, 1.0f)); };
  auto dy = [&](double db) { return g0.y + gh * (0.5f - static_cast<float>(db / 36.0)); };

  // Header rows: octave cells (C1..C9) and frequency regions.
  {
    const float r1 = F.p(0, kGy - 26).y, r2 = F.p(0, kGy - 13).y, r3 = g0.y - 1;
    double c1 = 440.0 * std::pow(2.0, (24 - 69) / 12.0);  // C1
    headerCell(dl, ImVec2(g0.x, r1), ImVec2(fx(c1), r2), "", K, fsSmall);
    for (int o = 1; o <= 9; ++o) {
      double lo = c1 * std::pow(2.0, o - 1), hi = std::min(20000.0, lo * 2);
      char buf[8];
      std::snprintf(buf, sizeof buf, "C%d", o);
      headerCell(dl, ImVec2(fx(lo), r1), ImVec2(fx(hi), r2), buf, K, fsSmall);
    }
    struct Region { float x0, x1; const char* name; };
    static const Region regs[] = {{0, 39, ""}, {41, 79, "SUB"}, {81, 145, "BASS"}, {147, 185, "LOW MID"}, {187, 265, "MID"},
                                  {267, 305, "HIGH MID"}, {307, 328, "PRS"}, {330, 400, "TREBLE"}};
    for (const Region& r : regs) headerCell(dl, ImVec2(g0.x + r.x0 * s, r2), ImVec2(g0.x + r.x1 * s, r3), r.name, K, fsSmall);
  }
  skin::graphWell(dl, g0, g1, K);
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##eqgraph", ImVec2(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool gHov = ImGui::IsItemHovered(), gAct = ImGui::IsItemActive();
  dl->PushClipRect(g0, g1, true);

  // Histogram / heat map of the monitored signal.
  const int monitor = ival(st, EqMonitor, 2), histMode = ival(st, EqSpectrum, 1), heatPos = ival(st, EqHeatmap);
  if (monitor > 0 && (histMode > 0 || heatPos > 0)) {
    const int N = 160;
    float db[N];
    pui::spectrum(monitor == 1 && t.inputTap(a) ? t.inputTap(a) : t.tap(a), t.key() ^ (monitor == 1 ? 0x1D00ull : 0), N, db, sr);
    static const float kSmooth[5] = {1.0f, 0.6f, 0.35f, 0.18f, 0.08f};
    float k = kSmooth[std::clamp(ival(st, EqSmoothing, 2), 0, 4)];
    if (bval(st, EqHistAvg)) k *= 0.05f;  // average mode: long-term mean
    static const float kPivot[5] = {0, 1.5f, 3, 4.5f, 6};
    float pivot = kPivot[std::clamp(ival(st, EqPivot, 2), 0, 4)];
    if (static_cast<int>(ui.hist.size()) != N) ui.hist.assign(N, -140.f), ui.hold.assign(N, -140.f);
    for (int i = 0; i < N; ++i) {
      double hz = 20.0 * std::pow(1000.0, (i + 0.5) / N);
      float v = db[i] + pivot * static_cast<float>(std::log2(hz / 1000.0));
      ui.hist[static_cast<size_t>(i)] += (v - ui.hist[static_cast<size_t>(i)]) * k;
      ui.hold[static_cast<size_t>(i)] = std::max(ui.hold[static_cast<size_t>(i)] - 0.2f, ui.hist[static_cast<size_t>(i)]);
    }
    int range = ival(st, EqHistRange, 1);
    float top = 0, floorDb = range == 0 ? -60.f : range == 1 ? -90.f : range == 2 ? -120.f : -90.f;
    if (range == 3) {  // matched: follow the loudest bin
      float mx = -140;
      for (float v : ui.hist) mx = std::max(mx, v);
      top = std::max(-60.f, mx + 3), floorDb = top - 60;
    }
    auto ly = [&](float v) { return g1.y - std::clamp((v - floorDb) / (top - floorDb), 0.0f, 1.0f) * gh; };
    if (heatPos > 0) {
      ui.heat.insert(ui.heat.begin(), ui.hist);
      if (ui.heat.size() > 48) ui.heat.pop_back();
      float hy0 = heatPos == 2 ? g0.y + gh * 0.6f : g0.y, hy1 = heatPos == 1 ? g0.y + gh * 0.4f : g1.y;
      float rowH = (hy1 - hy0) / 48.0f;
      for (size_t r = 0; r < ui.heat.size(); ++r)
        for (int i = 0; i < N; ++i) {
          float v = std::clamp((ui.heat[r][static_cast<size_t>(i)] - floorDb) / (top - floorDb), 0.0f, 1.0f);
          if (v < 0.05f) continue;
          float x0 = g0.x + gw * i / N, x1 = g0.x + gw * (i + 1) / N;
          float y = heatPos == 2 ? hy1 - (r + 1) * rowH : hy0 + r * rowH;
          dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowH + 0.5f), theme::withAlpha(skin::lighten(K.graphGridStrong, v * 0.6f), v * 0.55f));
        }
    }
    if (histMode > 0) {
      ImVec2 poly[N];
      for (int i = 0; i < N; ++i) {
        float x0 = g0.x + gw * i / N, x1 = g0.x + gw * (i + 1) / N, y = ly(ui.hist[static_cast<size_t>(i)]);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1 - 0.5f, g1.y), theme::withAlpha(K.graphGridStrong, 0.85f));
        poly[i] = ImVec2((x0 + x1) * 0.5f, y);
      }
      dl->AddPolyline(poly, N, skin::lighten(K.graphGridStrong, 0.25f), 0, 1.0f);
      if (histMode == 2) {
        for (int i = 0; i < N; ++i) poly[i].y = ly(ui.hold[static_cast<size_t>(i)]);
        dl->AddPolyline(poly, N, theme::withAlpha(K.graphText, 0.6f), 0, 1.0f);
      }
    }
  }
  // Grid: 10 / 100 / 1000 Hz lines (40, 80, 250, 500, 2k, 4k, 6k emphasised), 6 dB majors with 1 dB minors.
  for (int i = 0; i <= 8; ++i) dl->AddLine(ImVec2(fx(20 + i * 10), g0.y), ImVec2(fx(20 + i * 10), g1.y), K.graphGrid);
  for (int i = 0; i <= 9; ++i) dl->AddLine(ImVec2(fx(100 + i * 100), g0.y), ImVec2(fx(100 + i * 100), g1.y), K.graphGrid);
  for (int i = 0; i <= 9; ++i) dl->AddLine(ImVec2(fx(1000 + i * 1000), g0.y), ImVec2(fx(1000 + i * 1000), g1.y), K.graphGrid);
  for (double hz : {40.0, 80.0, 250.0, 500.0, 2000.0, 4000.0, 6000.0, 100.0, 1000.0, 10000.0})
    dl->AddLine(ImVec2(fx(hz), g0.y), ImVec2(fx(hz), g1.y), K.graphGridStrong);
  for (int m = 0; m < 6; ++m) {
    float y = g0.y + gh * m / 6.0f;
    if (m > 0) dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), m == 3 ? skin::lighten(K.graphGridStrong, 0.15f) : K.graphGridStrong);
    for (int k = 1; k < 6; ++k) {
      float yy = y + gh / 36.0f * k;
      dl->AddLine(ImVec2(g0.x, yy), ImVec2(g0.x + 4 * s, yy), K.graphGrid);
      dl->AddLine(ImVec2(g1.x - 4 * s, yy), ImVec2(g1.x, yy), K.graphGrid);
    }
  }
  ImGui::PushFont(a.fontMono, fsSmall);
  for (int db = 12; db >= -12; db -= 6) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%+d", db);
    if (db == 0) std::snprintf(buf, sizeof buf, "0");
    dl->AddText(ImVec2(g0.x + 4 * s, dy(db) - fsSmall - 1), K.graphText, buf);
  }
  ImGui::PopFont();

  // Band responses.
  EqBandFilters f[kEqBands];
  for (int b = 0; b < kEqBands; ++b) designEqBand(st, b, sr, f[b]);
  const int W = std::max(2, static_cast<int>(gw / 2));
  std::vector<ImVec2> pts(static_cast<size_t>(W));
  std::vector<double> total(static_cast<size_t>(W), 0.0);
  const float zy = dy(0);
  for (int b = 0; b < kEqBands; ++b) {
    if (f[b].count == 0) continue;
    ImU32 bc = skin::hex(kBandHex[b]);
    for (int i = 0; i < W; ++i) {
      float x = g0.x + gw * i / (W - 1);
      double v = dsp::gainToDb(eqBandMagnitude(f[b], xf(x), sr));
      total[static_cast<size_t>(i)] += v;
      pts[static_cast<size_t>(i)] = ImVec2(x, dy(std::clamp(v, -40.0, 40.0)));
    }
    bool sel = b == ui.selected || b == ui.hover || b == ui.drag;
    for (int i = 0; i + 1 < W; ++i) {
      ImVec2 q[4] = {pts[static_cast<size_t>(i)], pts[static_cast<size_t>(i + 1)], ImVec2(pts[static_cast<size_t>(i + 1)].x, zy),
                     ImVec2(pts[static_cast<size_t>(i)].x, zy)};
      if (std::fabs(q[0].y - zy) > 0.3f || std::fabs(q[1].y - zy) > 0.3f) dl->AddConvexPolyFilled(q, 4, theme::withAlpha(bc, sel ? 0.30f : 0.16f));
    }
    dl->AddPolyline(pts.data(), W, theme::withAlpha(bc, sel ? 1.0f : 0.6f), 0, sel ? 1.6f : 1.0f);
  }
  if (listen > 0) {
    dsp::Biquad lb;
    designEqListen(st, listen - 1, sr, lb, ival(st, EqSoloShift, 1));
    for (int i = 0; i < W; ++i) {
      float x = g0.x + gw * i / (W - 1);
      pts[static_cast<size_t>(i)] = ImVec2(x, dy(std::clamp(dsp::gainToDb(lb.magnitude(xf(x), sr)), -40.0, 40.0)));
    }
    dl->AddPolyline(pts.data(), W, skin::hex(kBandHex[listen - 1]), 0, 2.2f);
  } else {
    const bool classicMain = val(st, EqModel, 1) >= 1.5f;  // J-26 classic model: linear output gain
    double mainDb = classicMain ? dsp::gainToDb(std::max(1e-6, static_cast<double>(val(st, EqMainLin, 1)))) : val(st, EqMain);
    for (int i = 0; i < W; ++i) pts[static_cast<size_t>(i)] = ImVec2(g0.x + gw * i / (W - 1), dy(std::clamp(total[static_cast<size_t>(i)] + mainDb, -40.0, 40.0)));
    dl->AddPolyline(pts.data(), W, IM_COL32(236, 242, 246, 235), 0, 1.8f);
  }

  // Phase rotation: the minimum-phase response's phase (±180° over the graph height).
  if (bval(st, EqPhaseView) && ival(st, EqLinear) == 0) {
    for (int i = 0; i < W; ++i) {
      double hz = xf(g0.x + gw * i / (W - 1)), w = dsp::kTwoPi * hz / sr, phase = 0;
      double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
      for (int b = 0; b < kEqBands; ++b)
        for (int k = 0; k < f[b].count; ++k) {
          const dsp::Biquad& q = f[b].stage[k];
          phase += std::atan2(-(q.b1 * s1 + q.b2 * s2), q.b0 + q.b1 * c1 + q.b2 * c2) - std::atan2(-(q.a1 * s1 + q.a2 * s2), 1 + q.a1 * c1 + q.a2 * c2);
        }
      phase = std::remainder(phase, dsp::kTwoPi);
      pts[static_cast<size_t>(i)] = ImVec2(g0.x + gw * i / (W - 1), g0.y + gh * (0.5f - static_cast<float>(phase / dsp::kTwoPi)));
    }
    for (int i = 0; i + 1 < W; ++i)
      if (std::fabs(pts[static_cast<size_t>(i + 1)].y - pts[static_cast<size_t>(i)].y) < gh * 0.5f && (i / 3) % 2 == 0)
        dl->AddLine(pts[static_cast<size_t>(i)], pts[static_cast<size_t>(i + 1)], IM_COL32(255, 196, 120, 200), 1.2f);
  }
  // Tokens: frequency on x, level on y (gain types) or 0 dB.
  ImVec2 tok[kEqBands];
  for (int b = 0; b < kEqBands; ++b) {
    int type = ival(st, eqParam(b, EqType));
    tok[b] = ImVec2(fx(val(st, eqParam(b, EqFreq))), hasGain(type) ? dy(val(st, eqParam(b, EqGain))) : zy);
  }
  const bool showTokens = bval(st, EqTokens, true);
  const float tr = std::clamp(6.5f * s, 7.0f, 12.0f);
  if (!gAct) {
    ui.hover = -1;
    float best = tr * 1.6f;
    for (int b = 0; b < kEqBands; ++b) {
      if (ival(st, eqParam(b, EqType)) == EqOff) continue;
      float d = std::hypot(io.MousePos.x - tok[b].x, io.MousePos.y - tok[b].y);
      if (gHov && d < best) best = d, ui.hover = b;
    }
  }
  if (showTokens)
    for (int b = kEqBands - 1; b >= 0; --b) {
      int type = ival(st, eqParam(b, EqType));
      if (type == EqOff) continue;
      bool off = !bval(st, eqExt(b, EqEnabled), true);
      bool hot = b == ui.hover || b == ui.drag || b == ui.selected;
      ImU32 bc = skin::hex(kBandHex[b]);
      dl->AddCircleFilled(ImVec2(tok[b].x, tok[b].y + 1), tr + 1, IM_COL32(0, 0, 0, 90), 24);
      dl->AddCircleFilled(tok[b], tr, off ? theme::withAlpha(bc, 0.25f) : hot ? skin::lighten(bc, 0.2f) : bc, 24);
      dl->AddCircle(tok[b], tr, hot ? IM_COL32(255, 255, 255, 230) : skin::darken(bc, 0.4f), 24, hot ? 1.6f : 1.0f);
      char num[4];
      std::snprintf(num, sizeof num, "%d", b + 1);
      ImGui::PushFont(a.fontMonoBold, std::clamp(tr * 1.25f, 9.0f, 14.0f));
      ImVec2 ts = ImGui::CalcTextSize(num);
      dl->AddText(ImVec2(std::round(tok[b].x - ts.x * 0.5f), std::round(tok[b].y - ts.y * 0.5f)), off ? skin::darken(bc, 0.3f) : IM_COL32(18, 24, 28, 255), num);
      ImGui::PopFont();
    }
  // Hover / drag readout at the top of the graph.
  if (gHov || gAct) {
    std::string txt;
    int b = ui.drag >= 0 ? ui.drag : ui.hover;
    if (b >= 0) {
      txt = bandReadout(st, b);
    } else {
      double hz = xf(io.MousePos.x);
      int note = static_cast<int>(std::lround(69 + 12 * std::log2(hz / 440.0)));
      double db = (0.5 - (io.MousePos.y - g0.y) / gh) * 36.0;
      char buf[80];
      std::snprintf(buf, sizeof buf, "%.0f Hz  %s  %+.2f dB", hz, noteName(note).c_str(), db);
      txt = buf;
    }
    ImGui::PushFont(a.fontMono, fsMid);
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    float x = std::clamp(io.MousePos.x + 12, g0.x + 4, g1.x - ts.x - 6), y = std::clamp(io.MousePos.y - ts.y - 12, g0.y + 3, g1.y - ts.y - 3);
    dl->AddRectFilled(ImVec2(x - 4, y - 2), ImVec2(x + ts.x + 4, y + ts.y + 2), IM_COL32(10, 16, 20, 210), 3);
    dl->AddText(ImVec2(x, y), b >= 0 ? skin::lighten(skin::hex(kBandHex[b]), 0.35f) : K.lcdText, txt.c_str());
    ImGui::PopFont();
    a.setHint(b >= 0 ? "drag: freq/level · Shift+drag: solo, freq/width · middle drag: width/level · wheel: width (Shift: order) · Alt+click: reset · right-click: band menu"
                     : "double-click: enable a free band here");
  }
  if (listen > 0) monoText(dl, ImVec2(g0.x + 30 * s, g0.y + 4 * s), skin::hex(kBandHex[listen - 1]), ("SOLO · BAND " + std::to_string(listen)).c_str(), fsMid);
  dl->PopClipRect();

  // ---- graph interaction
  const int soloMode = ival(st, EqSoloShift, 1);
  if (ImGui::IsItemActivated() && ui.hover < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    ui.drag = -2;  // empty graph: the main level follows vertical drags
    ui.gesture = 0x4551F00000000000ull ^ (t.key() << 4) ^ static_cast<uint64_t>(ImGui::GetFrameCount());
  }
  if (gAct && ui.drag == -2 && io.MouseDelta.y != 0) {
    PluginState ns = *t.state(a.doc.song());
    const float dDb = -io.MouseDelta.y * (io.KeyCtrl ? 0.2f : 1.0f) * 36.0f / gh;
    if (val(st, EqModel, 1) >= 1.5f) {
      float& g = ns.params[static_cast<size_t>(EqMainLin)];
      g = std::clamp(g * std::pow(10.0f, dDb / 20.0f), 0.0f, 12.0f);
    } else {
      float& g = ns.params[static_cast<size_t>(EqMain)];
      g = std::clamp(g + dDb, -18.0f, 18.0f);
    }
    setPluginState(a, t, ns, "EQ main level", ui.gesture);
  }
  if (ImGui::IsItemActivated() && ui.hover >= 0) {
    int b = ui.hover;
    ui.selected = b;
    ui.gesture = 0x4551E00000000000ull ^ (t.key() << 4) ^ static_cast<uint64_t>(ImGui::GetFrameCount());
    if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      PluginState ns = st;
      resetBandValues(ns, b);
      setPluginState(a, t, ns, "reset EQ band");
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ImGui::OpenPopup("##eqbandmenu");
    } else {
      ui.drag = b;
      bool middle = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
      ui.dragMode = middle ? 2 : io.KeyShift ? (soloMode > 0 ? 1 : 2) : 0;
      if (ui.dragMode == 1) {
        ui.soloHeld = true;
        ui.listenBefore = listen;
        a.doc.setParam(t.pid(EqListen), b + 1, false);
      }
    }
  }
  if (gAct && ui.drag >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
    int b = ui.drag;
    PluginState ns = *t.state(a.doc.song());
    float fine = io.KeyCtrl ? 0.2f : 1.0f;
    auto& P = ns.params;
    auto freqMove = [&] {
      double x = fx(P[static_cast<size_t>(eqParam(b, EqFreq))]) + io.MouseDelta.x * fine;
      P[static_cast<size_t>(eqParam(b, EqFreq))] = static_cast<float>(std::clamp(xf(static_cast<float>(x)), 20.0, 20000.0));
    };
    auto levelMove = [&] {
      if (!hasGain(ival(ns, eqParam(b, EqType)))) return;
      float& g = P[static_cast<size_t>(eqParam(b, EqGain))];
      g = std::clamp(g - io.MouseDelta.y * fine * 36.0f / gh, -18.0f, 18.0f);
    };
    auto widthMove = [&](float d) {
      float& w = P[static_cast<size_t>(eqFl(b, EqWidth))];
      w = std::clamp(w + d * fine / std::max(80.0f, gh), 0.0f, 1.0f);
    };
    if (ui.dragMode == 0) freqMove(), levelMove();
    else if (ui.dragMode == 1) freqMove(), widthMove(-io.MouseDelta.y);
    else widthMove(io.MouseDelta.x), levelMove();
    setPluginState(a, t, ns, "move EQ band", ui.gesture);
  }
  if (ImGui::IsItemDeactivated()) {
    a.doc.endGesture();
    if (ui.soloHeld) a.doc.setParam(t.pid(EqListen), ui.listenBefore, false);
    ui.soloHeld = false;
    ui.drag = -1;
  }
  // Wheel: over a token the width (Shift: level), elsewhere the main level; Ctrl = 5× steps.
  if (gHov && io.MouseWheel != 0 && !gAct) {
    float step = (io.KeyCtrl ? 5.0f : 1.0f) * io.MouseWheel;
    if (ui.hover >= 0) {
      int b = ui.hover;
      if (io.KeyShift) {
        if (hasGain(ival(st, eqParam(b, EqType))))
          a.doc.setParam(t.pid(eqParam(b, EqGain)), std::clamp(val(st, eqParam(b, EqGain)) + step * 0.36, -18.0, 18.0), true);
      } else {
        a.doc.setParam(t.pid(eqFl(b, EqWidth)), std::clamp(val(st, eqFl(b, EqWidth)) + step / 54.6, 0.0, 1.0), true);
      }
    } else {
      if (val(st, EqModel, 1) >= 1.5f)
        a.doc.setParam(t.pid(EqMainLin), std::clamp(val(st, EqMainLin, 1) * std::pow(10.0, step * 0.018), 0.0, 12.0), true);
      else
        a.doc.setParam(t.pid(EqMain), std::clamp(val(st, EqMain) + step * 0.36, -18.0, 18.0), true);
    }
  }
  if (gHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
    if (ui.hover >= 0) {
      bool en = bval(st, eqExt(ui.hover, EqEnabled), true);
      a.doc.setParam(t.pid(eqExt(ui.hover, EqEnabled)), en ? 0 : 1, false);
    } else {
      for (int b = 0; b < kEqBands; ++b)
        if (ival(st, eqParam(b, EqType)) == EqOff || !bval(st, eqExt(b, EqEnabled), true)) {
          PluginState ns = st;
          if (ival(st, eqParam(b, EqType)) == EqOff) {
            ns.params[static_cast<size_t>(eqParam(b, EqType))] = EqPeak;
            ns.params[static_cast<size_t>(eqFl(b, EqWidth))] = defaultWidthFor(EqPeak);
          }
          ns.params[static_cast<size_t>(eqExt(b, EqEnabled))] = 1;
          ns.params[static_cast<size_t>(eqParam(b, EqFreq))] = static_cast<float>(xf(io.MousePos.x));
          ns.params[static_cast<size_t>(eqParam(b, EqGain))] = std::clamp(static_cast<float>((0.5 - (io.MousePos.y - g0.y) / gh) * 36.0), -18.f, 18.f);
          setPluginState(a, t, ns, "add EQ band");
          ui.selected = b;
          break;
        }
    }
  }
  if (ImGui::BeginPopup("##eqbandmenu")) {
    if (ui.selected >= 0) bandMenu(a, t, st, ui.selected);
    ImGui::EndPopup();
  }
  // Frequency labels under the graph (centred on their grid lines).
  {
    static const std::pair<double, const char*> labels[] = {{20, "20"}, {50, "50"}, {100, "100"}, {200, "200"}, {500, "500"},
                                                            {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}};
    ImGui::PushFont(a.fontMono, fsSmall);
    float y = g1.y + 3 * s;
    for (const auto& [hz, txt] : labels) {
      ImVec2 ts = ImGui::CalcTextSize(txt);
      float x = hz == 20 ? g0.x + 1 : fx(hz) - ts.x * 0.5f;
      dl->AddText(ImVec2(std::round(x), std::round(y)), K.graphText, txt);
    }
    ImGui::PopFont();
  }

  // ================================================================ right strip
  // Main level.
  {
    ImVec2 p = F.p(425, 28);
    skin::vslider(a, t, val(st, EqModel, 1) >= 1.5f ? EqMainLin : EqMain, p, ImVec2(15 * s, 191 * s), K, skin::hex(0xB8C2C9));
    // Band solo on Shift toggle (right-click: mode menu).
    ImVec2 bp = F.p(425, 224);
    bool on = soloMode > 0;
    if (skin::button("##soloshift", bp, ImVec2(16 * s, 14 * s), K, "S", on, fsSmall)) a.doc.setParam(t.pid(EqSoloShift), on ? 0 : 1, false);
    if (ImGui::IsItemHovered()) {
      a.setHint("Band solo on Shift (right-click: mode)");
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##solomode");
    }
    if (ImGui::BeginPopup("##solomode")) {
      static const char* modes[4] = {"Off", "Adaptive", "Band pass", "Inherit width"};
      for (int k = 0; k < 4; ++k)
        if (ImGui::MenuItem(modes[k], nullptr, k == soloMode)) a.doc.setParam(t.pid(EqSoloShift), k, false);
      ImGui::EndPopup();
    }
  }
  // Wheel row captions.
  {
    ImGui::PushFont(a.fontMonoBold, fsSmall);
    dl->AddText(F.p(419 - 2, 245), K.graphText, "FREQ");
    dl->AddText(F.p(427 - 2, 266), K.graphText, "BW");
    ImGui::PopFont();
  }
  for (int b = 0; b < kEqBands; ++b) {
    const float bx = 21.0f * b;
    const ImU32 bc = skin::hex(kBandHex[b]);
    const int type = ival(st, eqParam(b, EqType));
    const bool en = bval(st, eqExt(b, EqEnabled), true);
    ImGui::PushID(b);
    // Type selector (glyph LCD): click → type menu, drag / wheel steps.
    {
      ImVec2 p0 = F.p(450 + bx, 7), sz(15 * s, 9 * s);
      ImGui::SetCursorScreenPos(p0);
      ImGui::InvisibleButton("type", sz);
      bool hov = ImGui::IsItemHovered();
      dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), hov ? skin::lighten(K.lcdBg, 0.08f) : K.lcdBg, 2);
      typeGlyph(dl, type, ImVec2(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f), sz.x * 0.8f, sz.y * 0.7f, type == EqOff ? K.textDim : bc, 1.2f);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##typemenu");
      if (hov) {
        a.setHint(std::string("Band ") + std::to_string(b + 1) + " type: " + kTypeNames[std::clamp(type, 0, 7)] + "   (click: menu · wheel: step)");
        if (io.MouseWheel != 0) setBandType(a, t, st, b, std::clamp(type - static_cast<int>(io.MouseWheel), 0, 7));
      }
      if (ImGui::BeginPopup("##typemenu")) {
        static const int order[8] = {EqOff, EqHighCut, EqBandPass, EqLowCut, EqNotch, EqLowShelf, EqPeak, EqHighShelf};
        for (int i : order) {
          ImVec2 ip = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable((std::string("      ") + kTypeNames[i]).c_str(), i == type) && i != type) setBandType(a, t, st, b, i);
          typeGlyph(ImGui::GetWindowDrawList(), i, ImVec2(ip.x + 12, ip.y + 8), 18, 9, pal().text);
        }
        ImGui::EndPopup();
      }
    }
    // Order digit.
    {
      int o = std::clamp(ival(st, eqFl(b, EqFlOrder), EqOrd2), 0, 6);
      skin::digi(a, t, eqFl(b, EqFlOrder), F.p(450 + bx, 16.5f), ImVec2(15 * s, 10.5f * s), K, kOrderShort[o], fsSmall);
    }
    // Level slider (dimmed for types without gain).
    {
      if (!hasGain(type)) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
      skin::Skin kk = K;
      if (!hasGain(type)) kk.handle = skin::darken(K.handle, 0.4f);
      skin::vslider(a, t, eqParam(b, EqGain), F.p(449 + bx, 28), ImVec2(15 * s, 191 * s), kk, bc);
      if (ImGui::IsItemActivated()) ui.selected = b;
      if (!hasGain(type)) ImGui::PopStyleVar();
    }
    // Mute / solo LED (reference behaviour): click toggles the band; right-click
    // or Ctrl+click solos it (only this band on); soloing the only active band
    // again turns every band back on.
    {
      ImVec2 p0 = F.p(448 + bx, 226), sz(14 * s, 11 * s);
      ImGui::SetCursorScreenPos(p0);
      ImGui::InvisibleButton("ms", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      bool hov = ImGui::IsItemHovered();
      bool left = ImGui::IsItemClicked(ImGuiMouseButton_Left), right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      if (left || right) {
        bool exclusive = en;
        for (int o = 0; o < kEqBands; ++o)
          if (o != b && bval(st, eqExt(o, EqEnabled), true)) exclusive = false;
        PluginState ns = st;
        bool soloGesture = right || io.KeyCtrl;
        for (int o = 0; o < kEqBands; ++o) {
          float& e = ns.params[static_cast<size_t>(eqExt(o, EqEnabled))];
          if (!exclusive) {
            if (soloGesture) e = o == b ? 1.f : 0.f;
            else if (o == b) e = en ? 0.f : 1.f;
          } else {
            if (soloGesture) e = 1.f;
            else if (o == b) e = 0.f;
          }
        }
        setPluginState(a, t, ns, soloGesture ? "solo EQ band" : "mute EQ band");
      }
      ImVec2 c(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f);
      float r = std::min(sz.x, sz.y) * 0.38f;
      dl->AddCircleFilled(c, r + 1.5f, K.lcdBg, 16);
      if (en) dl->AddCircleFilled(c, r, bc, 16);
      if (hov) {
        dl->AddCircle(c, r + 1.5f, K.text, 16, 1.0f);
        a.setHint(std::string("Band ") + std::to_string(b + 1) + ": click mute · right-click / Ctrl+click solo");
      }
    }
    // Frequency + width wheels.
    skin::wheel(a, t, eqParam(b, EqFreq), F.p(446 + bx, 239), 20 * s, K, 0, en ? bc : skin::darken(bc, 0.45f));
    if (ImGui::IsItemActivated()) ui.selected = b;
    skin::wheel(a, t, eqFl(b, EqWidth), F.p(447 + bx, 262), 18 * s, K, 0, en ? bc : skin::darken(bc, 0.45f));
    if (ImGui::IsItemActivated()) ui.selected = b;
    ImGui::PopID();
  }

  // ================================================================ bottom bar
  {
    // Wordmark.
    ImGui::PushFont(a.fontMonoBold, std::clamp(11.0f * s, 11.0f, 18.0f));
    dl->AddText(F.p(10, 250), skin::hex(0x3F515D), "PARAMETRIC");
    ImGui::PopFont();
    ImGui::PushFont(a.fontMonoBold, std::clamp(9.0f * s, 10.0f, 15.0f));
    dl->AddText(F.p(10, 264), skin::hex(0x4C6170), "EQ");
    ImGui::PopFont();
    // Options (gear).
    ImVec2 op = F.p(150, 247);
    ImVec2 osz(24 * s, 22 * s);
    if (skin::button("##eqopt", op, osz, K, "", false)) ImGui::OpenPopup("##eqoptions");
    {
      ImVec2 c(op.x + osz.x * 0.5f, op.y + osz.y * 0.5f);
      float r = std::min(osz.x, osz.y) * 0.26f;
      for (int k = 0; k < 8; ++k) {
        float an = k * 3.14159265f / 4;
        dl->AddLine(ImVec2(c.x + std::cos(an) * r, c.y + std::sin(an) * r), ImVec2(c.x + std::cos(an) * r * 1.45f, c.y + std::sin(an) * r * 1.45f), K.textDim, 2.0f);
      }
      dl->AddCircle(c, r, K.textDim, 16, 2.0f);
    }
    if (ImGui::IsItemHovered()) a.setHint("Options");
    if (ImGui::BeginPopup("##eqoptions")) {
      optionsMenu(a, t, st);
      ImGui::EndPopup();
    }
    // LIN (linear phase, right-click: quality), HQ (decramped bands), band tokens.
    ImVec2 bs(33 * s, 33 * s);
    bool hq = ival(st, EqHQ) > 0;
    {
      static std::map<uint64_t, int> lastQuality;
      int lin = ival(st, EqLinear);
      if (lin > 0) lastQuality[t.key()] = lin;
      if (skin::button("##lin", F.p(182, 247), bs, K, "LIN", lin > 0, fsMid))
        a.doc.setParam(t.pid(EqLinear), lin > 0 ? 0 : (lastQuality.count(t.key()) ? lastQuality[t.key()] : 3), false);
      if (ImGui::IsItemHovered()) {
        char buf[160];
        int taps = eqLinearTaps(lin > 0 ? lin : 3);
        std::snprintf(buf, sizeof buf, "Linear phase (%d taps, latency %.0f ms) · right-click: quality", taps, 1.5 * taps / std::max(1.0, sr) * 1000.0);
        a.setHint(buf);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##linq");
      }
      if (ImGui::BeginPopup("##linq")) {
        static const char* q[6] = {"Lowest", "Low", "Medium", "High", "Higher", "Highest"};
        for (int k = 0; k < 6; ++k)
          if (ImGui::MenuItem(q[k], nullptr, lin == k + 1)) a.doc.setParam(t.pid(EqLinear), k + 1, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Off", nullptr, lin == 0)) a.doc.setParam(t.pid(EqLinear), 0, false);
        ImGui::EndPopup();
      }
    }
    if (skin::button("##hq", F.p(221, 247), bs, K, "HQ", hq, fsMid)) a.doc.setParam(t.pid(EqHQ), hq ? 0 : 1, false);
    if (ImGui::IsItemHovered()) a.setHint("High quality: decramped bands (response matches the analog curve up to Nyquist)");
    bool tokens = bval(st, EqTokens, true);
    if (skin::button("##tok", F.p(261, 247), bs, K, "", tokens, fsMid)) a.doc.setParam(t.pid(EqTokens), tokens ? 0 : 1, false);
    {
      ImVec2 c = F.p(261 + 16.5f, 247 + 16.5f);
      dl->AddCircleFilled(c, 7 * s, tokens ? skin::hex(kBandHex[0]) : K.textDim, 20);
      ImGui::PushFont(a.fontMonoBold, std::clamp(8 * s, 9.0f, 13.0f));
      ImVec2 ts = ImGui::CalcTextSize("1");
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(18, 24, 28, 255), "1");
      ImGui::PopFont();
    }
    if (ImGui::IsItemHovered()) a.setHint("View band tokens");
    // Monitor selector.
    {
      ImGui::PushFont(a.fontMonoBold, fsSmall);
      dl->AddText(F.p(304, 238), K.graphText, "MONITOR");
      ImGui::PopFont();
      static const char* mon[3] = {"OFF", "IN", "OUT"};
      skin::switchSel(a, t, EqMonitor, F.p(302, 249), ImVec2(48 * s, 14 * s), K, mon, 3, true);
    }
    // Compare (A/B): store current into the spare slot, flip with it.
    {
      ImGui::PushFont(a.fontMonoBold, fsSmall);
      dl->AddText(F.p(358, 234), K.graphText, "COMPARE");
      ImGui::PopFont();
      if (skin::button("##cstore", F.p(355, 244), ImVec2(24 * s, 22 * s), K, "STO", false, fsSmall)) {
        pui::compareStore(a, t);
        a.notify("Stored in the spare state");
      }
      if (ImGui::IsItemHovered()) a.setHint("Store in spare state");
      bool onB = pui::compareOnB(t);
      if (skin::button("##cflip", F.p(380, 244), ImVec2(24 * s, 22 * s), K, onB ? "B" : "A", onB, fsMid)) pui::compareFlip(a, t);
      if (ImGui::IsItemHovered()) a.setHint("Flip with spare state (A/B compare)");
    }
  }

  ImGui::SetCursorScreenPos(F.o);
  ImGui::Dummy(ImVec2(width, H));
}

}  // namespace gl
