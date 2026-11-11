// Gridline — "plasma" pack: the Effects view. The left column lists the three
// chains (Main chain, Send A, Send B) top to bottom, one row per effect in
// signal order; a splitter's bands follow it as indented rows. Drag a row to
// reorder it or move it to another chain. The selected effect fills the rest
// of the stage: its display and main dials first, the other settings below.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../app/LayoutAudit.h"
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

const char* fxDisplayName(int type) {
  static const char* n[FxTypeCount] = {"Empty",  "Drive",           "Flanger", "Phaser",          "Chorus",         "Echo",
                                       "Dynamics", "Space",         "EQ",      "Filter",          "Widen",          "Frequency shift",
                                       "Impulse space", "Utility",  "Split low / high", "Split low / mid / high", "Split mid / side"};
  return type >= 0 && type < FxTypeCount ? n[type] : "Empty";
}

namespace {

constexpr float kListX = 12, kListW = 300, kEdX = kListX + kListW + 12, kEdW = 1000 - 12 - kEdX;
constexpr float kTop = kStageY + 10, kBottom = kStageB - 12;
// The parameter shown as the editor's Mix bar (-1: none).
int mixIndex(int type) {
  const auto& defs = fxParams(type);
  for (int q = 0; q < static_cast<int>(defs.size()); ++q)
    if (defs[static_cast<size_t>(q)].kind == 0 && std::strcmp(defs[static_cast<size_t>(q)].key, "kParamWet") == 0) return q;
  return -1;
}

int bandsOf(int type) { return type == FxSplitLMH ? 3 : (type == FxSplitLH || type == FxSplitMS) ? 2 : 0; }
const char* bandName(int type, int b) {
  if (type == FxSplitMS) return b == 0 ? "Mid" : "Side";
  if (type == FxSplitLH) return b == 0 ? "Low" : "High";
  return b == 0 ? "Low" : b == 1 ? "Mid" : "High";
}
const char* laneName(int r) { return r == 0 ? "Main chain" : r == 1 ? "Send A" : "Send B"; }

// ------------------------------------------------------------------ rack edits
int freeSlot(const Ctx& c) {
  for (int s = 0; s < kFxSlots; ++s)
    if (vali(c, fxP(s, X_Type)) == FxNone) return s;
  return -1;
}
void addFx(Ctx& c, int type, int rack, int parent, int band, int beforeIndex = -1) {
  const int s = freeSlot(c);
  if (s < 0) return;
  // the current layout first (slots without a node are appended by resolvedFx)
  c.m.fx = resolvedFx(c.m, c.st.params);
  FxNode n{s, rack, parent, band};
  if (beforeIndex >= 0 && beforeIndex <= static_cast<int>(c.m.fx.size())) c.m.fx.insert(c.m.fx.begin() + beforeIndex, n);
  else c.m.fx.push_back(n);
  c.m.fxExtra[static_cast<size_t>(s)] = FxExtra{};
  std::vector<std::pair<int, double>> ps = {{fxP(s, X_Type), type}, {fxP(s, X_On), 1}};
  const auto& defs = fxParams(type);
  for (int q = 0; q < kFxParams; ++q) ps.push_back({fxP(s, X_P0 + q), q < static_cast<int>(defs.size()) ? fxDefaultNorm(type, q) : 0.0});
  commitParams(c, "Add effect", ps);  // one undo step
  c.u.fxSel = s;
}
void removeFx(Ctx& c, int slot) {
  auto nodes = resolvedFx(c.m, c.st.params);
  std::vector<int> gone = {slot};
  for (const auto& n : nodes)
    if (n.parent == slot) gone.push_back(n.slot);
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const FxNode& n) { return std::find(gone.begin(), gone.end(), n.slot) != gone.end(); }),
              nodes.end());
  c.m.fx = nodes;
  for (int s : gone) c.m.fxExtra[static_cast<size_t>(s)] = FxExtra{};
  std::vector<std::pair<int, double>> ps;
  for (int s : gone) ps.push_back({fxP(s, X_Type), FxNone});
  commitParams(c, "Remove effect", ps);  // one undo step
  if (c.u.fxSel == slot) c.u.fxSel = -1;
}
// Moves node `slot` to sit before node `before` (-1: to the end of its group).
void moveFx(Ctx& c, int slot, int before) {
  auto nodes = resolvedFx(c.m, c.st.params);
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const FxNode& n) { return n.slot == slot; });
  if (it == nodes.end() || slot == before) return;
  FxNode n = *it;
  // a splitter moves with its band modules
  std::vector<FxNode> moving = {n};
  nodes.erase(it);
  if (n.parent < 0) {
    for (auto j = nodes.begin(); j != nodes.end();)
      if (j->parent == slot) moving.push_back(*j), j = nodes.erase(j);
      else ++j;
  }
  auto pos = before < 0 ? nodes.end() : std::find_if(nodes.begin(), nodes.end(), [&](const FxNode& q) { return q.slot == before; });
  if (pos != nodes.end()) {
    // take the target's place (rack / parent / band follow the target)
    moving[0].rack = pos->rack, moving[0].parent = pos->parent, moving[0].band = pos->band;
    for (size_t i = 1; i < moving.size(); ++i) moving[i].rack = pos->rack;
  }
  nodes.insert(pos, moving.begin(), moving.end());
  c.m.fx = nodes;
  commit(c, "Move effect");
}

// The add menu: Gridline's own grouping, by what the effect does.
void addMenu(Ctx& c, int rack, int parent, int band) {
  struct G {
    const char* name;
    std::vector<int> types;
  };
  static const std::vector<G> groups = {{"Tone", {FxDistortion, FxFilter, FxEq}},
                                        {"Movement", {FxChorus, FxFlanger, FxPhaser, FxHyper, FxBode}},
                                        {"Room and echo", {FxDelay, FxReverb, FxConvolve}},
                                        {"Level", {FxComp, FxUtility}},
                                        {"Split into bands", {FxSplitLH, FxSplitLMH, FxSplitMS}}};
  const bool full = freeSlot(c) < 0;
  if (full) ImGui::TextDisabled("%s", i18n::tr("All 24 effect places are in use"));
  for (const auto& g : groups) {
    if (parent >= 0 && bandsOf(g.types.front())) continue;  // no splitter inside a splitter
    ImGui::SeparatorText(i18n::tr(g.name));
    for (int t : g.types)
      if (ImGui::MenuItem(i18n::tr(fxDisplayName(t)), nullptr, false, !full)) addFx(c, t, rack, parent, band);
  }
}

// Sentence case for a control label ("FILTER TYPE" -> "Filter type").
std::string niceLabel(const char* s) {
  std::string o = s ? s : "";
  if (o == "Q") return o;
  if (o == "BPM") return "Tempo sync";
  if (o == "FREQ") return "Frequency";
  if (o == "MIX") return "Mix";
  for (size_t i = 1; i < o.size(); ++i) o[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[i])));
  return o;
}

// ------------------------------------------------------------------ module displays
void distDisplay(Ctx& c, int s, float x, float y, float w, float h) {
  displayWellPublic(c, x, y, w, h);
  const int mi = fxParamIndex(FxDistortion, "kParamMode"), di = fxParamIndex(FxDistortion, "kParamDrive");
  const int mode = static_cast<int>(std::lround(fxPlain(FxDistortion, mi, val(c, fxP(s, X_P0 + mi)))));
  const float drive = static_cast<float>(val(c, fxP(s, X_P0 + di)));
  FxExtra& ex = c.m.fxExtra[static_cast<size_t>(s)];
  if (mode == 11 || mode == 12) {
    // X-Shaper: editable transfer curve(s)
    Curve& cv = ex.shapeA;
    Curve edit = cv.empty() ? linearCurve() : cv;
    static int sel = -1;
    if (curveEditor(c, "xs", edit, x + 2, y + 2, w - 4, h - 4, 8, 8, sel, false, c.P.accent)) {
      cv = edit;
      commit(c, "Edit X-Shaper", 6000 + s);
    }
    return;
  }
  float ca[257], cb[257];
  for (int i = 0; i < 257; ++i) ca[i] = cb[i] = i / 256.f;
  float st = 0;
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 48; ++i) {
    const float xin = -1.f + 2.f * i / 48.f;
    const float yo = std::clamp(shapeSample(mode, xin, drive, 0.f, ca, cb, st), -1.2f, 1.2f);
    pts.push_back(c.p(x + w * i / 48.f, y + h * 0.5f - yo * h * 0.42f));
  }
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  c.dl->AddLine(c.p(x, y + h * 0.92f), c.p(x + w, y + h * 0.08f), theme::withAlpha(c.P.textDim, 0.45f), std::max(1.0f, c.s(1.2f)));  // before
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), c.p(0, y + h * 0.5f).y, c.P.accent, std::max(1.5f, c.s(1.6f)), 0.2f);
  c.dl->PopClipRect();
}
void filterDisplay(Ctx& c, int s, float x, float y, float w, float h) {
  displayWellPublic(c, x, y, w, h);
  auto P = [&](const char* key) {
    const int i = fxParamIndex(FxFilter, key);
    return i < 0 ? 0.0 : fxPlain(FxFilter, i, val(c, fxP(s, X_P0 + i)));
  };
  constexpr int N = 64;
  float hz[N], mag[N];
  for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
  const int ti = fxParamIndex(FxFilter, "kParamType"), fi = fxParamIndex(FxFilter, "kParamFreq");
  Filter::responseCurve(static_cast<int>(std::lround(P("kParamType"))), 48000, cutoffHz(val(c, fxP(s, X_P0 + fi))), P("kParamReso") / 100.0,
                        P("kParamVar") / 100.0, hz, mag, N);
  (void)ti;
  std::vector<ImVec2> pts(N);
  for (int i = 0; i < N; ++i) {
    const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
    pts[static_cast<size_t>(i)] = c.p(x + w * i / (N - 1), y + h - std::clamp((db + 36.f) / 60.f, 0.f, 1.f) * h);
  }
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  skin::curveArea(c.dl, pts.data(), N, c.p(0, y + h).y, c.P.accent, std::max(1.5f, c.s(1.6f)), 0.2f);
  c.dl->PopClipRect();
}

}  // namespace

// A picture of what an effect does, drawn from its own settings: the bend of a drive (straight
// input faint behind it), a filter or EQ curve, echo taps fading by the feedback, a room's decay,
// a dynamics knee, sweeping copies for the moving effects, a fork for a splitter. Used at full
// size in the editor and small in the chain rows and the Effects tile.
void fxPicture(Ctx& c, int s, float x, float y, float w, float h, bool big) {
  const int type = vali(c, fxP(s, X_Type));
  const bool on = val(c, fxP(s, X_On)) >= 0.5;
  const ImU32 col = on ? c.P.accent : c.P.textFaint, faint = theme::withAlpha(c.P.textDim, 0.45f);
  const float lw = big ? std::max(1.5f, c.s(1.8f)) : std::max(1.1f, c.s(1.2f));
  auto P = [&](const char* key, double dflt) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? dflt : fxPlain(type, i, val(c, fxP(s, X_P0 + i)));
  };
  auto pt = [&](float fx, float fy) { return c.p(x + fx * w, y + (1.f - fy) * h); };  // 0..1 box, y up
  std::vector<ImVec2> v;
  auto line = [&](ImU32 cc, float th) {
    if (v.size() > 1) c.dl->AddPolyline(v.data(), static_cast<int>(v.size()), cc, 0, th);
    v.clear();
  };
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  const int N = big ? 96 : 32;
  switch (type) {
    case FxDistortion: {
      const int mode = static_cast<int>(std::lround(P("kParamMode", 0)));
      const float drive = static_cast<float>(val(c, fxP(s, X_P0 + std::max(0, fxParamIndex(type, "kParamDrive")))));
      float ca[257], cb[257];
      for (int i = 0; i < 257; ++i) ca[i] = cb[i] = i / 256.f;
      float st = 0;
      v.push_back(pt(0.06f, 0.06f)), v.push_back(pt(0.94f, 0.94f));
      line(faint, lw * 0.8f);  // before: straight through
      for (int i = 0; i <= N; ++i) {
        const float xin = -1.f + 2.f * i / N;
        const float yo = std::clamp(shapeSample(mode, xin, drive, 0.f, ca, cb, st), -1.1f, 1.1f);
        v.push_back(pt(0.06f + 0.88f * i / N, 0.5f + yo * 0.44f));
      }
      line(col, lw);
      break;
    }
    case FxFilter: {
      constexpr int M = 48;
      float hz[M], mag[M];
      for (int i = 0; i < M; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (M - 1));
      const int fi = fxParamIndex(FxFilter, "kParamFreq");
      Filter::responseCurve(static_cast<int>(std::lround(P("kParamType", 0))), 48000, cutoffHz(val(c, fxP(s, X_P0 + fi))),
                            P("kParamReso", 10) / 100.0, P("kParamVar", 0) / 100.0, hz, mag, M);
      for (int i = 0; i < M; ++i) {
        const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
        v.push_back(pt(static_cast<float>(i) / (M - 1), 0.1f + 0.8f * std::clamp((db + 36.f) / 54.f, 0.f, 1.f)));
      }
      line(col, lw);
      break;
    }
    case FxEq: {
      const double f1 = P("kParamFreq1", 210), f2 = P("kParamFreq2", 2041), g1 = P("kParamGain1", 0), g2 = P("kParamGain2", 0);
      const double q1 = 0.4 + P("kParamReso1", 60) / 40.0, q2 = 0.4 + P("kParamReso2", 60) / 40.0;
      const int t1 = static_cast<int>(std::lround(P("kParamType1", 0))), t2 = static_cast<int>(std::lround(P("kParamType2", 0)));
      v.push_back(pt(0, 0.5f)), v.push_back(pt(1, 0.5f));
      line(faint, lw * 0.7f);
      for (int i = 0; i <= N; ++i) {
        const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / N);
        const double o1 = std::log2(f / f1), o2 = std::log2(f / f2);
        double db = 0;
        db += t1 == 1 ? g1 * std::exp(-o1 * o1 * q1) : t1 == 2 ? (o1 < 0 ? std::max(-36.0, 24.0 * o1) : 0.0) : g1 / (1 + std::exp(4 * o1));
        db += t2 == 1 ? g2 * std::exp(-o2 * o2 * q2) : t2 == 2 ? (o2 > 0 ? std::max(-36.0, -24.0 * o2) : 0.0) : g2 / (1 + std::exp(-4 * o2));
        v.push_back(pt(static_cast<float>(i) / N, 0.5f + static_cast<float>(std::clamp(db / 30.0, -0.45, 0.45))));
      }
      line(col, lw);
      break;
    }
    case FxDelay: {  // taps fading by the feedback; ping-pong taps alternate high and low
      const double fb = P("kParamFeedback", 40) / 100.0, tl = P("kParamTimeL", 0.25);
      const bool pp = std::lround(P("kParamMode", 0)) == 1;
      const float gap = static_cast<float>(0.1 + 0.3 * std::clamp(tl, 0.0, 1.0));
      double a = 1;
      for (int k = 0; k < 24; ++k) {
        const float fx = 0.06f + gap * k;
        if (fx > 0.97f || a < 0.03) break;
        const float hh = static_cast<float>(a) * 0.8f;
        const float y0 = pp && (k % 2) ? 0.5f : 0.08f;
        c.dl->AddRectFilled(pt(fx - 0.012f, y0 + hh * (pp ? 0.5f : 1.f)), pt(fx + 0.012f, y0), k ? col : c.P.text, c.s(1));
        if (k) a *= fb;
      }
      break;
    }
    case FxReverb: case FxConvolve: {  // pre-delay, then a dense tail fading by the decay
      const double decay = type == FxReverb ? 0.15 + 0.85 * (P("kParamFeedback", 50) / 100.0) * (0.4 + 0.6 * P("kParamSize", 50) / 100.0) : 0.5;
      const float pre = type == FxReverb ? static_cast<float>(0.04 + 0.12 * std::clamp(P("kParamPreDelay", 0) / 2.5, 0.0, 1.0)) : 0.04f;
      c.dl->AddRectFilled(pt(0.03f, 0.92f), pt(0.045f, 0.06f), c.P.text, c.s(1));
      uint32_t rng = 7u + static_cast<uint32_t>(s);
      const int lines = big ? 120 : 30;
      for (int i = 0; i < lines; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const float t = static_cast<float>(i) / lines, fx = pre + 0.03f + t * (0.95f - pre);
        const float env = static_cast<float>(std::exp(-t * 3.0 / decay));
        const float hh = env * (0.45f + 0.4f * ((rng >> 8) / 16777216.f)) * 0.86f;
        c.dl->AddLine(pt(fx, 0.06f), pt(fx, 0.06f + hh), theme::withAlpha(col, 0.8f), std::max(1.0f, c.s(big ? 1.4f : 1.f)));
      }
      break;
    }
    case FxComp: {  // level in > level out: straight to the threshold, flatter above it
      const double thr = std::clamp(P("kParamThresh", 0.15), 0.0, 1.0), ratio = std::max(1.0, P("kParamRatio", 4));
      const float tx = static_cast<float>(0.15 + 0.75 * (1.0 - thr));
      v.push_back(pt(0.06f, 0.06f)), v.push_back(pt(0.94f, 0.94f));
      line(faint, lw * 0.8f);
      v.push_back(pt(0.06f, 0.06f)), v.push_back(pt(tx, tx));
      v.push_back(pt(0.94f, tx + (0.94f - tx) / static_cast<float>(std::min(ratio, 50.0))));
      line(col, lw);
      c.dl->AddCircleFilled(pt(tx, tx), c.s(big ? 3.5f : 2.f), c.P.text);
      break;
    }
    case FxChorus: case FxFlanger: case FxPhaser: case FxHyper: {  // copies drifting against each other
      const int copies = type == FxHyper ? 4 : type == FxChorus ? 3 : 2;
      for (int k = 0; k < copies; ++k) {
        for (int i = 0; i <= N; ++i) {
          const float t = static_cast<float>(i) / N;
          const float ph = t * 6.2832f * (type == FxPhaser ? 3.f : 2.f) + k * (type == FxFlanger ? 0.6f : 1.3f);
          v.push_back(pt(t, 0.5f + 0.32f * std::sin(ph) * (1.f - 0.15f * k)));
        }
        line(k == 0 ? col : theme::withAlpha(col, 0.5f), lw);
      }
      break;
    }
    case FxBode: {  // a set of partials moved up by a fixed number of Hz
      for (int k = 1; k <= 5; ++k) {
        const float fx = 0.1f + 0.16f * (k - 1);
        c.dl->AddLine(pt(fx, 0.1f), pt(fx, 0.1f + 0.7f / k), faint, lw);
        c.dl->AddLine(pt(fx + 0.06f, 0.1f), pt(fx + 0.06f, 0.1f + 0.7f / k), col, lw);
      }
      break;
    }
    case FxUtility: {
      c.dl->AddRectFilled(pt(0.3f, 0.75f), pt(0.45f, 0.1f), col, c.s(1));
      c.dl->AddRectFilled(pt(0.55f, 0.75f), pt(0.7f, 0.1f), col, c.s(1));
      break;
    }
    case FxSplitLH: case FxSplitLMH: case FxSplitMS: {  // one path forking into its bands
      const int n = bandsOf(type);
      for (int b = 0; b < n; ++b) {
        const float ty = n == 2 ? (b == 0 ? 0.72f : 0.28f) : 0.8f - 0.3f * b;
        v.push_back(pt(0.05f, 0.5f)), v.push_back(pt(0.4f, 0.5f)), v.push_back(pt(0.6f, ty)), v.push_back(pt(0.95f, ty));
        line(col, lw);
      }
      break;
    }
    default: break;
  }
  c.dl->PopClipRect();
}

namespace {
// ------------------------------------------------------------------ lane cards
// Moves the dragged node (and its bands) to the end of lane r.
void moveToLane(Ctx& c, int from, int r) {
  auto nodes = resolvedFx(c.m, c.st.params);
  std::vector<FxNode> moving, rest;
  for (const auto& q : nodes) (q.slot == from || q.parent == from ? moving : rest).push_back(q);
  if (moving.empty()) return;
  for (auto& q : moving) {
    q.rack = r;
    if (q.slot == from) q.parent = -1, q.band = 0;
  }
  // the moved node first, its bands after it
  std::stable_partition(moving.begin(), moving.end(), [&](const FxNode& q) { return q.slot == from; });
  rest.insert(rest.end(), moving.begin(), moving.end());
  c.m.fx = rest;
  commit(c, "Move effect to another lane");
}

void acceptFxDrop(Ctx& c, int target) {
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_FX")) {
      int from = 0;
      std::memcpy(&from, p->Data, sizeof(int));
      if (from != target) moveFx(c, from, target);
    }
    ImGui::EndDragDropTarget();
  }
}

// One effect as a row of its chain: on / off dot, name (a band's tag first), mix, output dot.
void laneRow(Ctx& c, const FxNode& n, float x, float y, float w, float h, int bandType) {
  const int s = n.slot, type = vali(c, fxP(s, X_Type));
  const int kOn = fxP(s, X_On);
  reach(kOn);
  reach(fxP(s, X_Type));
  const bool on = val(c, kOn) >= 0.5, sel = c.u.fxSel == s;
  ImGui::PushID(s + 6400);
  // power dot first (the row button below would take the hover otherwise)
  ImGui::SetCursorScreenPos(c.p(x + 2, y));
  if (ImGui::InvisibleButton("##on", c.F.sz(24, h))) setv(c, kOn, on ? 0 : 1);
  const bool hvOn = ImGui::IsItemHovered();
  if (hvOn) c.a.setHint(on ? "On (click: off)" : "Off (click: on)");
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##card", c.F.sz(w, h))) c.u.fxSel = s;
  const bool hv = ImGui::IsItemHovered();
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_FX", &s, sizeof(int));
    ImGui::TextUnformatted(i18n::tr(fxDisplayName(type)));
    ImGui::EndDragDropSource();
  }
  acceptFxDrop(c, s);
  if (hv) c.a.setHint("Click: show its controls. Drag: reorder or move to another chain. Right-click: more");
  if (ImGui::BeginPopupContextItem("##cm")) {
    auto nodes = resolvedFx(c.m, c.st.params);
    std::vector<int> group;
    for (const auto& q : nodes)
      if (q.rack == n.rack && q.parent == n.parent && (n.parent < 0 || q.band == n.band)) group.push_back(q.slot);
    const long idx = std::find(group.begin(), group.end(), s) - group.begin();
    if (ImGui::MenuItem(i18n::tr("Move earlier"), nullptr, false, idx > 0)) moveFx(c, s, group[static_cast<size_t>(idx - 1)]);
    if (ImGui::MenuItem(i18n::tr("Move later"), nullptr, false, idx + 1 < static_cast<long>(group.size())))
      moveFx(c, group[static_cast<size_t>(idx + 1)], s);
    ImGui::Separator();
    if (ImGui::MenuItem(on ? i18n::tr("Turn off") : i18n::tr("Turn on"))) setv(c, kOn, on ? 0 : 1);
    if (ImGui::MenuItem(i18n::tr("Remove"))) removeFx(c, s);
    ImGui::EndPopup();
  }
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  gl::audit::group(a, b, "cell");
  c.dl->AddRectFilled(a, b, sel ? c.P.headOn : (hv ? c.P.panelHi : c.P.field), c.s(4));
  if (sel) {
    c.dl->AddRect(a, b, c.P.accent, c.s(4), 0, std::max(1.f, c.s(1.5f)));
    c.dl->AddRectFilled(c.p(x, y + 5), c.p(x + 3, y + h - 5), c.P.accent, c.s(1));
  } else {
    c.dl->AddRect(a, b, hv ? c.P.lineHi : c.P.line, c.s(4));
  }
  const ImVec2 dc = c.p(x + 14, y + h * 0.5f);
  if (on) c.dl->AddCircleFilled(dc, c.s(4.5f), c.P.good, 14);
  else c.dl->AddCircle(dc, c.s(4.5f), hvOn ? c.P.text : c.P.boxEdge, 14, std::max(1.0f, c.s(1.2f)));
  float tx = x + 28;
  if (bandType >= 0) {
    const char* tag = bandName(bandType, n.band);
    const float tw = textW(c, i18n::tr(tag), 10.5f, false) + 10;
    c.dl->AddRectFilled(c.p(tx, y + h * 0.5f - 9), c.p(tx + tw, y + h * 0.5f + 9), theme::withAlpha(c.P.accent, 0.16f), c.s(3));
    textMid(c, tx, tx + tw, y + h * 0.5f, i18n::tr(tag), c.P.accentText, 10.5f, false);
    tx += tw + 8;
  }
  // the mix as a short meter at the right, the output level as a dot
  const int mq = mixIndex(type);
  const bool narrow = w < 240;  // a column row: no mix meter
  const float lx1 = x + w - 26, lx0 = narrow ? lx1 : lx1 - 54, ly = y + h * 0.5f;
  {  // a small picture of what it does
    const float pw = 36, ph = h - 10;
    c.dl->AddRectFilled(c.p(tx, y + 5), c.p(tx + pw, y + 5 + ph), c.P.well, c.s(3));
    fxPicture(c, s, tx + 3, y + 7, pw - 6, ph - 4, false);
    tx += pw + 8;
  }
  c.dl->PushClipRect(c.p(tx, y), c.p(lx0 - 8, y + h), true);
  labelBeside(c, tx, y, h, i18n::tr(fxDisplayName(type)), on ? c.P.text : c.P.textFaint, 12.0f, false);
  c.dl->PopClipRect();
  if (!narrow) c.dl->AddLine(c.p(lx0, ly), c.p(lx1, ly), c.P.line, std::max(1.f, c.s(3)));
  const float mixN = mq >= 0 ? static_cast<float>(val(c, fxP(s, X_P0 + mq))) : 1.f;
  if (!narrow) c.dl->AddLine(c.p(lx0, ly), c.p(lx0 + (lx1 - lx0) * mixN, ly), on ? c.P.knob : c.P.textFaint, std::max(1.f, c.s(3)));
  const float lv = c.hasLive ? std::clamp(c.live.fxMeter[s], 0.f, 1.f) : 0.f;
  c.dl->AddCircleFilled(c.p(x + w - 13, ly), c.s(3.5f), theme::withAlpha(c.P.good, 0.25f + 0.75f * lv), 12);
  ImGui::PopID();
}

void laneMenu(Ctx& c, int r) {
  static std::vector<std::pair<FxNode, std::vector<float>>> clip;
  static std::vector<FxExtra> clipExtra;
  if (ImGui::MenuItem(i18n::tr("Copy this lane"))) {
    clip.clear();
    clipExtra.clear();
    for (const auto& n : resolvedFx(c.m, c.st.params))
      if (n.rack == r) {
        std::vector<float> p;
        for (int q = 0; q < X_Count; ++q) p.push_back(valf(c, fxP(n.slot, q)));
        clip.push_back({n, p});
        clipExtra.push_back(c.m.fxExtra[static_cast<size_t>(n.slot)]);
      }
  }
  if (ImGui::MenuItem(i18n::tr("Paste at the end of this lane"), nullptr, false, !clip.empty())) {
    std::map<int, int> remap;
    auto nodes = resolvedFx(c.m, c.st.params);
    std::vector<std::pair<int, std::vector<float>>> writes;
    std::vector<bool> taken(kFxSlots, false);
    for (const auto& n : nodes) taken[static_cast<size_t>(n.slot)] = true;
    for (size_t i = 0; i < clip.size(); ++i) {
      int s = -1;
      for (int q = 0; q < kFxSlots; ++q)
        if (!taken[static_cast<size_t>(q)] && vali(c, fxP(q, X_Type)) == FxNone) {
          s = q;
          break;
        }
      if (s < 0) break;
      taken[static_cast<size_t>(s)] = true;
      remap[clip[i].first.slot] = s;
      FxNode n = clip[i].first;
      n.slot = s, n.rack = r;
      if (n.parent >= 0) n.parent = remap.count(n.parent) ? remap[n.parent] : -1;
      nodes.push_back(n);
      c.m.fxExtra[static_cast<size_t>(s)] = clipExtra[i];
      writes.push_back({s, clip[i].second});
    }
    c.m.fx = nodes;
    std::vector<std::pair<int, double>> ps;
    for (const auto& [s, p] : writes)
      for (int q = 0; q < X_Count; ++q) ps.push_back({fxP(s, q), p[static_cast<size_t>(q)]});
    commitParams(c, "Paste effects", ps);
  }
  if (ImGui::MenuItem(i18n::tr("Empty this lane"))) {
    std::vector<int> gone;
    for (const auto& n : resolvedFx(c.m, c.st.params))
      if (n.rack == r && n.parent < 0) gone.push_back(n.slot);
    for (int s : gone) removeFx(c, s);
  }
}

constexpr float kRowH = 34, kHeadH = 40;
// Rows chain r needs (effects, bands, the band "+" rows; at least one for the empty note).
int laneRows(const Ctx& c, int r, const std::vector<FxNode>& nodes) {
  int rows = 0;
  for (const auto& n : nodes) {
    if (n.rack != r || n.parent >= 0) continue;
    ++rows;
    if (bandsOf(vali(c, fxP(n.slot, X_Type)))) {
      for (const auto& ch : nodes)
        if (ch.parent == n.slot && ch.rack == r) ++rows;
      ++rows;  // the band "+" buttons
    }
  }
  return std::max(1, rows);
}

// Chain r as a card from y (height h): its name, + Add and its menu, then one row per effect.
void lane(Ctx& c, int r, const std::vector<FxNode>& nodes, float y, float h, float rowH) {
  const float x = kListX, w = kListW;
  ImGui::PushID(r + 6600);
  card(c, x - 3, y, w + 6, h, i18n::tr(laneName(r)));
  // the card's free space is a drop target: move to the end of this chain
  {
    ImGui::SetCursorScreenPos(c.p(x, y + kHeadH));
    ImGui::InvisibleButton("##lane", c.F.sz(w, std::max(4.f, h - kHeadH - 4)));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_FX")) {
        int from = 0;
        std::memcpy(&from, p->Data, sizeof(int));
        moveToLane(c, from, r);
      }
      ImGui::EndDragDropTarget();
    }
  }
  if (button(c, "add", x + w - 122, y + 7, 76, 24, "+ Add", false, 11.0f)) ImGui::OpenPopup("##addfx");
  if (ImGui::IsItemHovered()) c.a.setHint("Add an effect at the end of this chain");
  if (ImGui::BeginPopup("##addfx")) {
    addMenu(c, r, -1, 0);
    ImGui::EndPopup();
  }
  dropField(c, "lm", x + w - 42, y + 7, 34, 24, "...", [&] { laneMenu(c, r); });
  if (ImGui::IsItemHovered()) c.a.setHint("Copy, paste or empty this chain");
  float ry = y + kHeadH;
  int drawn = 0;
  for (const auto& n : nodes) {
    if (n.rack != r || n.parent >= 0) continue;
    const int t = vali(c, fxP(n.slot, X_Type));
    laneRow(c, n, x + 8, ry, w - 16, rowH - 6, -1);
    ry += rowH;
    ++drawn;
    if (bandsOf(t)) {
      const float by0 = ry - 4;
      for (const auto& ch : nodes)
        if (ch.parent == n.slot && ch.rack == r) {
          laneRow(c, ch, x + 30, ry, w - 38, rowH - 6, t);
          ry += rowH;
        }
      ImGui::PushID(n.slot + 6700);
      const int nb = bandsOf(t);
      const float bw = (w - 38 - 6 * (nb - 1)) / static_cast<float>(nb);
      for (int bnd = 0; bnd < nb; ++bnd) {
        ImGui::PushID(bnd);
        const std::string lab = std::string("+ ") + i18n::tr(bandName(t, bnd));
        if (button(c, "addb", x + 30 + bnd * (bw + 6), ry, bw, rowH - 10, lab.c_str(), false, 10.5f)) ImGui::OpenPopup("##addband");
        if (ImGui::IsItemHovered()) c.a.setHint("Add an effect to this band");
        if (ImGui::BeginPopup("##addband")) {
          addMenu(c, r, n.slot, bnd);
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
      ImGui::PopID();
      ry += rowH;
      c.dl->AddLine(c.p(x + 19, by0), c.p(x + 19, ry - 10), theme::withAlpha(c.P.accent, 0.6f), std::max(1.f, c.s(1.5f)));
    }
  }
  if (!drawn) {
    const ImVec2 a = c.p(x + 8, ry), b = c.p(x + w - 8, ry + rowH - 6);
    c.dl->AddRect(a, b, c.P.line, c.s(4));
    textMid(c, x + 8, x + w - 8, ry + (rowH - 6) * 0.5f, i18n::tr("Empty: add an effect or drag one here"), c.P.textDim, 11.0f, false);
  }
  ImGui::PopID();
}

// ------------------------------------------------------------------ the selected effect's controls
// A choice of effect slot s, parameter q, as a drop-down field.
void fxChoice(Ctx& c, int s, int type, int q, float x, float y, float w, float h) {
  const auto& d = fxParams(type)[static_cast<size_t>(q)];
  const int k = fxP(s, X_P0 + q);
  reach(k);
  const int cnt = static_cast<int>(d.choices.size());
  const int cur = std::clamp(static_cast<int>(std::lround(fxPlain(type, q, val(c, k)))), 0, std::max(0, cnt - 1));
  int step = 0;
  // the filter effect's types read with the editor's filter names (the stored index unchanged)
  const bool filterType = type == FxFilter && std::strcmp(d.key, "kParamType") == 0;
  const auto choiceName = [&](int j) { return filterType ? filterDisplayName(j) : d.choices[static_cast<size_t>(j)]; };
  const std::string shown = niceLabel(d.label) + ": " + (cnt ? choiceName(cur) : ptext(c, k));
  dropField(c, "ch", x, y, w, h, shown.c_str(),
            [&] {
              if (type == FxConvolve && std::strcmp(d.key, "kParamIr") == 0) {
                if (ImGui::MenuItem(i18n::tr("Load impulse response..."))) {
                  const int slot = s;
                  openFile(c, i18n::tr("Load impulse response"), {".wav", ".flac", ".aif", ".aiff"},
                           [slot, type, q](App& a, const PluginTarget& t, const std::string& p) {
                             editState(a, t, "Load impulse", [&](PluginState& st, Model& m) {
                               m.fxExtra[static_cast<size_t>(slot)].ir = p;
                               st.params[static_cast<size_t>(fxP(slot, X_P0 + q))] = static_cast<float>(fxNorm(type, q, 0));
                             });
                           });
                }
                ImGui::Separator();
              }
              for (int j = 0; j < cnt; ++j)
                if (ImGui::Selectable(choiceName(j).c_str(), j == cur)) setv(c, k, fxNorm(type, q, j));
            },
            true, &step);
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(k) + ": " + (cnt ? choiceName(cur) : ptext(c, k)) + "   (click: choose, arrows: step)");
  if (step && cnt) setv(c, k, fxNorm(type, q, std::clamp(cur + step, 0, cnt - 1)));
}

// The selected effect: header (on / off, name, Mix, meter, Remove), then its display and up to six
// main dials, then every other setting as bars, switches and choices in three columns.
void fxEditor(Ctx& c, const FxNode& n, int bandType, float x, float y, float w, float h) {
  const int s = n.slot, type = vali(c, fxP(s, X_Type));
  ImGui::PushID(s + 6100);
  std::string title = i18n::tr(fxDisplayName(type));
  if (bandType >= 0) title += std::string(" (") + i18n::tr(bandName(bandType, n.band)) + ")";
  const float top = card(c, x, y, w, h, title.c_str(), fxP(s, X_On));
  const int mixQ = mixIndex(type);
  {
    const float hx = x + w - 384;
    if (mixQ >= 0) {
      ImGui::PushID(mixQ + 200);
      bar(c, fxP(s, X_P0 + mixQ), hx, y + 6, 220, 24, "Mix");
      ImGui::PopID();
    }
    const float mx = hx + 230, mw = 50;
    c.dl->AddRectFilled(c.p(mx, y + 15), c.p(mx + mw, y + 21), c.P.field, c.s(2));
    if (c.hasLive)
      c.dl->AddRectFilled(c.p(mx, y + 15), c.p(mx + mw * std::clamp(c.live.fxMeter[s], 0.f, 1.f), y + 21), c.P.good, c.s(2));
    if (button(c, "rm", x + w - 96, y + 6, 84, 24, "Remove", false, 11.0f)) removeFx(c, s);
  }
  const auto& defs = fxParams(type);
  std::vector<int> dials, rest;
  for (int q = 0; q < static_cast<int>(defs.size()); ++q) {
    if (q == mixQ) continue;
    if (defs[static_cast<size_t>(q)].kind == 0 && dials.size() < 6) dials.push_back(q);
    else rest.push_back(q);
  }
  const float L = x + 14, W = w - 28;
  float yy = top + 8;
  const bool hasDisplay = type != FxNone && type != FxUtility;
  auto dial = [&](int q, float cx, float cy, float r) {
    ImGui::PushID(q + 200);
    const auto& d = defs[static_cast<size_t>(q)];
    const std::string lab = niceLabel(d.label);
    knob(c, fxP(s, X_P0 + q), cx, cy, r, lab.c_str(), (d.min < 0 && d.max > 0) ? KnobBipolar : 0);
    ImGui::PopID();
  };
  const float restH = rest.empty() ? 0.f : 30.f + 32.f * static_cast<float>((rest.size() + 2) / 3);
  if (hasDisplay) {  // display at the left, the main dials beside it (three per row); it takes the free height
    const float dw = std::max(300.f, W - 3 * 92 - 16), dh = std::clamp(y + h - 16 - yy - 18 - restH, 196.f, 400.f);
    if (type == FxDistortion) distDisplay(c, s, L, yy, dw, dh);
    else if (type == FxFilter) filterDisplay(c, s, L, yy, dw, dh);
    else {
      displayWellPublic(c, L, yy, dw, dh);
      fxPicture(c, s, L + 10, yy + 10, dw - 20, dh - 20, true);
    }
    const float ax = L + dw + 16, aw = L + W - ax, cw = aw / 3;
    const float rowGap = std::max(104.f, dh * 0.5f);
    for (size_t i = 0; i < dials.size(); ++i) dial(dials[i], ax + cw * (i % 3 + 0.5f), yy + rowGap * 0.5f - 16 + (i / 3) * rowGap, 30);
    yy += dh + 18;
  } else if (!dials.empty()) {  // the main dials across
    const float cw = W / 6;
    for (size_t i = 0; i < dials.size(); ++i) dial(dials[i], L + cw * (i + 0.5f), yy + 38, 30);
    yy += 104;
  }
  if (!rest.empty()) {
    hline(c, L, L + W, yy, c.P.line);
    text(c, L, yy + 8, "More settings", c.P.label, 11.0f, true);
    yy += 30;
    const float gap = 10, cw = (W - gap * 2) / 3, rowH = 32;
    int i = 0;
    for (int q : rest) {
      const float fx = L + (cw + gap) * (i % 3), fy = yy + rowH * (i / 3);
      ++i;
      const auto& d = defs[static_cast<size_t>(q)];
      const int k = fxP(s, X_P0 + q);
      ImGui::PushID(q + 100);
      const std::string lab = niceLabel(d.label);
      if (d.kind == 1) chip(c, k, fx, fy, cw, 26, lab.c_str());
      else if (d.kind == 2) fxChoice(c, s, type, q, fx, fy, cw, 26);
      else bar(c, k, fx, fy, cw, 26, lab.c_str(), (d.min < 0 && d.max > 0) ? BarCentre : 0);
      ImGui::PopID();
    }
    yy += rowH * ((i + 2) / 3);
  }
  if (type == FxConvolve) {
    const std::string& ir = c.m.fxExtra[static_cast<size_t>(s)].ir;
    text(c, L, yy + 6, ir.empty() ? i18n::tr("Built-in room") : refStem(c.m, ir).c_str(), c.P.textDim, 11.0f, false);
  }
  ImGui::PopID();
}

}  // namespace

void effectsView(Ctx& c) {
  const float hx = stageHead(c, "Effects");
  text(c, hx, kStageHeadY + 8, i18n::tr("Main chain first, then the two sends"), c.P.textDim, 11.0f, false);
  if (button(c, "lock", kStageW - 212, kStageHeadY + 3, 200, 24, c.m.lockFx ? "Kept on preset load" : "Keep on preset load", c.m.lockFx,
             11.0f)) {
    c.m.lockFx = !c.m.lockFx;
    commit(c, "Keep effects on preset load");
  }
  if (ImGui::IsItemHovered()) c.a.setHint("When on, loading another sound keeps these effects");
  const auto nodes = resolvedFx(c.m, c.st.params);
  // the chains share the column's height: each gets its rows, the free space is shared out
  {
    int rows[3], all = 0;
    for (int r = 0; r < 3; ++r) all += rows[r] = laneRows(c, r, nodes);
    const float avail = kBottom - kTop - 2 * 12;
    const float rowH = std::clamp((avail - 3 * (kHeadH + 8)) / static_cast<float>(all), 24.f, kRowH);
    const float spare = std::max(0.f, avail - 3 * (kHeadH + 8) - all * rowH) / 3.f;
    float y = kTop;
    for (int r = 0; r < 3; ++r) {
      const float h = kHeadH + 8 + rows[r] * rowH + spare;
      lane(c, r, nodes, y, h, rowH);
      y += h + 12;
    }
  }
  // the selected effect (or the first one)
  const FxNode* sel = nullptr;
  for (const auto& n : nodes)
    if (n.slot == c.u.fxSel && vali(c, fxP(n.slot, X_Type)) != FxNone) sel = &n;
  if (!sel)
    for (const auto& n : nodes)
      if (vali(c, fxP(n.slot, X_Type)) != FxNone) {
        sel = &n;
        break;
      }
  const float ex = kEdX - 3, ew = kEdW + 6, ey = kTop, eh = kBottom - kTop;
  if (!sel) {
    card(c, ex, ey, ew, eh, "");
    textC(c, ex + ew * 0.5f, ey + eh * 0.5f - 8, i18n::tr("No effects yet. Use + Add on a chain."), c.P.textDim, 12.0f, false);
    return;
  }
  int bandType = -1;
  if (sel->parent >= 0) bandType = vali(c, fxP(sel->parent, X_Type));
  fxEditor(c, *sel, bandType, ex, ey, ew, eh);
}


// ================================================================== Panels: 4 Effects
float effectsColumn(Ctx& c, float x, float y, float w) {
  ImGui::PushID("fxcol");
  const auto nodes = resolvedFx(c.m, c.st.params);
  int& r = c.u.fxRack;
  r = std::clamp(r, 0, 2);
  {  // which chain: the main one or a send
    static const char* const names[3] = {"Main", "Send A", "Send B"};
    const float bw = (w - 8) / 3;
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      int n = 0;
      for (const auto& q : nodes) n += q.rack == i && q.parent < 0 && vali(c, fxP(q.slot, X_Type)) != FxNone;
      // the chain's name, and its effect count as a small badge at the right
      const float bx = x + i * (bw + 4);
      if (button(c, "rk", bx, y, bw, 26, n ? "" : names[i], r == i, 11.0f)) r = i;
      if (n) {
        char b[16];
        std::snprintf(b, sizeof b, "%d", n);
        const float nw = textW(c, names[i], 11.0f, false), bdw = std::max(16.f, textW(c, b, 9.5f, true) + 8);
        const float tx = bx + (bw - nw - bdw - 4) * 0.5f;
        text(c, tx, y + 6, names[i], r == i ? c.P.text : c.P.label, 11.0f, false);
        const float cx = tx + nw + 4;
        c.dl->AddRectFilled(c.p(cx, y + 6), c.p(cx + bdw, y + 20), r == i ? c.P.accent : c.P.headOn, c.s(7));
        textMid(c, cx, cx + bdw, y + 13, b, r == i ? IM_COL32(24, 24, 28, 255) : c.P.text, 9.5f, true);  // dark on the lit badge, both themes
      }
      if (ImGui::IsItemHovered()) c.a.setHint(i == 0 ? "The main chain: every sound goes through it" : "A send chain: sources feed it by their send level");
      ImGui::PopID();
    }
    y += 34;
  }
  int shown = 0;
  for (const auto& n : nodes) {
    if (n.rack != r || n.parent >= 0 || vali(c, fxP(n.slot, X_Type)) == FxNone) continue;
    laneRow(c, n, x, y, w, 30, -1);
    y += 34;
    ++shown;
    const int t = vali(c, fxP(n.slot, X_Type));
    if (bandsOf(t))
      for (const auto& ch : nodes)
        if (ch.parent == n.slot && ch.rack == r && vali(c, fxP(ch.slot, X_Type)) != FxNone) {
          laneRow(c, ch, x + 14, y, w - 14, 30, t);
          y += 34;
        }
  }
  if (!shown) {
    c.dl->AddRect(c.p(x, y), c.p(x + w, y + 30), c.P.line, c.s(4));
    textMid(c, x, x + w, y + 15, i18n::tr("No effects in this chain"), c.P.textDim, 11.0f, false);
    y += 34;
  }
  if (button(c, "add", x, y, w, 26, "+ Add effect", false, 11.5f)) ImGui::OpenPopup("##addfx");
  if (ImGui::IsItemHovered()) c.a.setHint("Add an effect at the end of this chain");
  if (ImGui::BeginPopup("##addfx")) {
    addMenu(c, r, -1, 0);
    ImGui::EndPopup();
  }
  y += 32;
  if (r > 0) {
    y = fieldGrid(c, x, y, w, 1, 32, {{r == 1 ? G_Bus1Vol : G_Bus2Vol, "Send level"}, {r == 1 ? G_Bus1Dest : G_Bus2Dest, "Goes to"}}, 26);
    if (r == 1) y = fieldGrid(c, x, y, w, 1, 32, {{tailP(T_BusChain), "Feeds Send B"}}, 26);
  }
  sectionEnd(c, x, y, w);
  // the selected effect of this chain (or its first)
  const FxNode* sel = nullptr;
  for (const auto& n : nodes)
    if (n.slot == c.u.fxSel && n.rack == r && vali(c, fxP(n.slot, X_Type)) != FxNone) sel = &n;
  if (!sel)
    for (const auto& n : nodes)
      if (n.rack == r && vali(c, fxP(n.slot, X_Type)) != FxNone) {
        sel = &n;
        break;
      }
  if (sel) {
    const int s = sel->slot, type = vali(c, fxP(s, X_Type));
    ImGui::PushID(s + 6100);
    const auto& defs = fxParams(type);
    const int mixQ = mixIndex(type);
    std::vector<int> dials, rest;
    for (int q = 0; q < static_cast<int>(defs.size()); ++q) {
      if (q == mixQ) continue;
      if (defs[static_cast<size_t>(q)].kind == 0 && dials.size() < 6) dials.push_back(q);
      else rest.push_back(q);
    }
    led(c, fxP(s, X_On), x, y + 6, 10);
    const bool det = section(c, "sel", x + 18, y, w - 18, i18n::tr(fxDisplayName(type)), "", !rest.empty());
    const float dh = 84;
    if (type == FxDistortion) distDisplay(c, s, x, y, w, dh);
    else if (type == FxFilter) filterDisplay(c, s, x, y, w, dh);
    else {
      displayWellPublic(c, x, y, w, dh);
      fxPicture(c, s, x + 6, y + 6, w - 12, dh - 12, true);
    }
    y += dh + 8;
    if (mixQ >= 0) {
      ImGui::PushID(mixQ + 200);
      bar(c, fxP(s, X_P0 + mixQ), x, y, w, 26, "Mix");
      ImGui::PopID();
      y += 34;
    }
    const float dx = w / 3;
    for (size_t i = 0; i < dials.size(); ++i) {
      ImGui::PushID(dials[i] + 200);
      const auto& d = defs[static_cast<size_t>(dials[i])];
      const std::string lab = niceLabel(d.label);
      knob(c, fxP(s, X_P0 + dials[i]), x + dx * (i % 3 + 0.5f), y + 20 + 80.f * (i / 3), 18, lab.c_str(),
           KnobValue | ((d.min < 0 && d.max > 0) ? KnobBipolar : 0));
      ImGui::PopID();
    }
    y += 80.f * ((dials.size() + 2) / 3);
    if (det)
      for (int q : rest) {
        const auto& d = defs[static_cast<size_t>(q)];
        const int k = fxP(s, X_P0 + q);
        ImGui::PushID(q + 100);
        const std::string lab = niceLabel(d.label);
        if (d.kind == 1) chip(c, k, x, y, w, 26, lab.c_str());
        else if (d.kind == 2) fxChoice(c, s, type, q, x, y, w, 26);
        else bar(c, k, x, y, w, 26, lab.c_str(), (d.min < 0 && d.max > 0) ? BarCentre : 0);
        ImGui::PopID();
        y += 32;
      }
    if (button(c, "rm", x, y, w, 24, "Remove this effect", false, 11.0f)) removeFx(c, s);
    y += 30;
    ImGui::PopID();
  }
  if (button(c, "lock", x, y, w, 24, c.m.lockFx ? "Kept on preset load" : "Keep on preset load", c.m.lockFx, 11.0f)) {
    c.m.lockFx = !c.m.lockFx;
    commit(c, "Keep effects on preset load");
  }
  if (ImGui::IsItemHovered()) c.a.setHint("When on, loading another sound keeps these effects");
  y += 30;
  ImGui::PopID();
  return y;
}


void fxAddMenu(Ctx& c, int rack) { addMenu(c, rack, -1, 0); }
void fxRemove(Ctx& c, int slot) { removeFx(c, slot); }
void fxDropOn(Ctx& c, int slot) { acceptFxDrop(c, slot); }
void fxToRack(Ctx& c, int slot, int rack) { moveToLane(c, slot, rack); }
bool fxAdd(Ctx& c, int type, int rack) {
  if (freeSlot(c) < 0) return false;
  addFx(c, type, rack, -1, 0);
  return true;
}


// ---- the Effects tab: one lane per chain, each effect a block (drag its name to move it)
namespace {
// The design's effect block: its picture at the left, four dials beside it (three of its own and
// the mix), the rest of its settings under them in Full (or More).
float fxBody(Ctx& c0, int s, float x, float y, float w) {
  const int type = vali(c0, fxP(s, X_Type));
  Pal pal2 = c0.P;  // the picture in the effect's colour
  pal2.accent = fxCol(c0, type);
  Ctx c{c0.a, c0.t, c0.F, c0.dl, c0.st, c0.m, c0.u, pal2, c0.live, c0.hasLive};
  const auto& defs = fxParams(type);
  const int mixQ = mixIndex(type);
  std::vector<int> dials, rest;
  for (int q = 0; q < static_cast<int>(defs.size()); ++q) {
    if (q == mixQ) continue;
    if (defs[static_cast<size_t>(q)].kind == 0 && static_cast<int>(dials.size()) < (mixQ >= 0 ? 3 : 4)) dials.push_back(q);
    else rest.push_back(q);
  }
  if (mixQ >= 0) dials.push_back(mixQ);
  const float ph = M(130), dw = M(124), pw = std::max(M(60), w - dw - M(10));
  if (type == FxDistortion) distDisplay(c, s, x, y, pw, ph);
  else if (type == FxFilter) filterDisplay(c, s, x, y, pw, ph);
  else {
    wellLook(c, x, y, pw, ph);
    gridLook(c, x, y, pw, ph, 4, 4, false);
    fxPicture(c, s, x + M(4), y + M(4), pw - M(8), ph - M(8), true);
  }
  {
    const float cellH = M(36 + 2 + 15 + 14), gap = M(6), tot = cellH * 2 + gap, top = y + std::max(0.f, (ph - tot) * 0.5f);
    for (size_t i = 0; i < dials.size() && i < 4; ++i) {
      ImGui::PushID(dials[i] + 200);
      const auto& d = defs[static_cast<size_t>(dials[i])];
      const std::string lab = dials[i] == mixQ ? std::string("Mix") : niceLabel(d.label);
      knob(c0, fxP(s, X_P0 + dials[i]), x + pw + M(10) + M(30) + (i % 2) * M(62), top + (i / 2) * (cellH + gap) + M(18), M(18), lab.c_str(),
           KnobValue | ((d.min < 0 && d.max > 0) ? KnobBipolar : 0));
      ImGui::PopID();
    }
  }
  y += ph;
  if (showDetails(c0)) {
    y += M(10);
    for (int q : rest) {
      const auto& d = defs[static_cast<size_t>(q)];
      const int k = fxP(s, X_P0 + q);
      ImGui::PushID(q + 100);
      const std::string lab = niceLabel(d.label);
      if (d.kind == 1) chip(c0, k, x, y, w, 26, lab.c_str());
      else if (d.kind == 2) fxChoice(c0, s, type, q, x, y, w, 26);
      else bar(c0, k, x, y, w, 26, lab.c_str(), (d.min < 0 && d.max > 0) ? BarCentre : 0);
      ImGui::PopID();
      y += 32;
    }
  }
  return y;
}
// What an effect is set to, in a few words: its first choice (a mode, a type), else nothing.
std::string fxSub(const Ctx& c, int s) {
  const int type = vali(c, fxP(s, X_Type));
  const auto& defs = fxParams(type);
  for (int q = 0; q < static_cast<int>(defs.size()); ++q)
    if (defs[static_cast<size_t>(q)].kind == 2) {
      std::string t = paramText(fxP(s, X_P0 + q), val(c, fxP(s, X_P0 + q)), &c.st.params);
      if (const size_t br = t.find(" ["); br != std::string::npos) t.resize(br);
      return t;
    }
  return {};
}
// A part dragged from the sidebar (an effect type) or an effect block dropped at slot `before`
// (-1: the end) of lane r.
void fxLaneDrop(Ctx& c, int r, int before, const ImRect& at, ImGuiID id) {
  if (!ImGui::BeginDragDropTargetCustom(at, id)) return;
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_FX")) {
    int from = 0;
    std::memcpy(&from, p->Data, sizeof(int));
    if (before >= 0 && from != before) moveFx(c, from, before);
    else if (before < 0) moveToLane(c, from, r);
  }
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_PART")) {
    int part = 0;
    std::memcpy(&part, p->Data, sizeof(int));
    if (part >= 100) addFxAt(c, part - 100, r, before);
  }
  ImGui::EndDragDropTarget();
}
float fxBlock(Ctx& c, const FxNode& n, int r, float x, float y, float w, int bandType) {
  const int s = n.slot, type = vali(c, fxP(s, X_Type));
  reach(fxP(s, X_Type));
  ImGui::PushID(s + 6900);
  std::string title = i18n::tr(fxDisplayName(type));
  if (bandType > 0) title = std::string(bandName(bandType, n.band)) + ": " + title;
  Block b;
  BlockOpts o;
  const ImU32 fam = fxCol(c, type);
  const std::string sub = fxSub(c, s);
  o.enable = fxP(s, X_On), o.fam = fam, o.drag = true, o.canRemove = true, o.selected = c.u.fxSel == s;
  if (!sub.empty()) o.sub = sub.c_str();
  KnobColour kc(fam);
  const float y0 = y;
  const bool open = blockBegin(c, b, "fx", x, y, w, title.c_str(), o, [&](float mx, float my, float mw, float mh) {
    fxTypeGlyphPublic(c, type, mx, my, mw, mh, fam);
  });
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_FX", &s, sizeof(int));
    ImGui::TextUnformatted(title.c_str());
    ImGui::EndDragDropSource();
  }
  if (b.titleClicked) c.u.fxSel = s;
  if (open) {
    y = fxBody(c, s, x + M(12), y, w - M(24));
  }
  blockEnd(c, b, y);
  if (b.removeClicked) removeFx(c, s);
  fxLaneDrop(c, r, s, ImRect(c.p(x, y0), c.p(x + w, y)), ImGui::GetID("drop"));
  ImGui::PopID();
  return y;
}
}  // namespace

float fxLane(Ctx& c, int r, float x, float y, float w) {
  ImGui::PushID(r + 6800);
  const auto nodes = resolvedFx(c.m, c.st.params);
  for (const auto& n : nodes) {
    if (n.rack != r || n.parent >= 0 || vali(c, fxP(n.slot, X_Type)) == FxNone) continue;
    y = fxBlock(c, n, r, x, y, w, -1);
    const int t = vali(c, fxP(n.slot, X_Type));
    if (bandsOf(t))
      for (const auto& ch : nodes)
        if (ch.parent == n.slot && ch.rack == r && vali(c, fxP(ch.slot, X_Type)) != FxNone) y = fxBlock(c, ch, r, x + 16, y, w - 16, t);
  }
  {  // the end of the lane: drop an effect here, or click to pick one
    const float h = M(40);
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    const bool dragging = (pl && (pl->IsDataType("PLASMA_FX") || pl->IsDataType("PLASMA_PART"))) || c.u.devDrag >= 100;
    ImGui::SetCursorScreenPos(c.p(x, y));
    if (ImGui::InvisibleButton("##addfx", c.F.sz(w, h))) ImGui::OpenPopup("##addfx");
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.a.setHint("Drag an effect here from the right, or click to pick one");
    layoutBox(c, x, y, w, h, "drop");
    const ImU32 col = dragging ? c.P.accent : (hv ? c.P.lineHi : theme::withAlpha(c.P.line, 1.f));
    if (dragging) c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), theme::withAlpha(c.P.accent, 0.06f), c.s(M(6)));
    for (float dx = x + M(6); dx < x + w - M(6); dx += M(8)) {
      c.dl->AddLine(c.p(dx, y), c.p(std::min(x + w - M(6), dx + M(4)), y), col);
      c.dl->AddLine(c.p(dx, y + h), c.p(std::min(x + w - M(6), dx + M(4)), y + h), col);
    }
    for (float dy = y + M(6); dy < y + h - M(6); dy += M(8)) {
      c.dl->AddLine(c.p(x, dy), c.p(x, std::min(y + h - M(6), dy + M(4))), col);
      c.dl->AddLine(c.p(x + w, dy), c.p(x + w, std::min(y + h - M(6), dy + M(4))), col);
    }
    const char* msg = "Drag an effect here";
    const float tw = textW(c, msg, T(11.5f), false), cx = x + w * 0.5f - (tw + M(24)) * 0.5f;
    const ImU32 tc = dragging ? c.P.accentText : c.P.textFaint;
    const ImVec2 pc = c.p(cx + M(8), y + h * 0.5f);
    c.dl->AddCircle(pc, c.s(M(8)), tc, 18, std::max(1.f, c.s(M(1))));
    c.dl->AddLine(ImVec2(pc.x - c.s(M(3.5f)), pc.y), ImVec2(pc.x + c.s(M(3.5f)), pc.y), tc, std::max(1.f, c.s(M(1.2f))));
    c.dl->AddLine(ImVec2(pc.x, pc.y - c.s(M(3.5f))), ImVec2(pc.x, pc.y + c.s(M(3.5f))), tc, std::max(1.f, c.s(M(1.2f))));
    text(c, cx + M(24), y + h * 0.5f - T(11.5f) * 0.68f, msg, tc, T(11.5f), false);
    fxLaneDrop(c, r, -1, ImRect(c.p(x, y), c.p(x + w, y + h)), ImGui::GetID("end"));
    if (ImGui::BeginPopup("##addfx")) {
      addMenu(c, r, -1, 0);
      ImGui::EndPopup();
    }
    y += h + M(8);
  }
  ImGui::PopID();
  return y;
}

// The foot of a lane: where it goes and how loud (Main: the output; a send: its return).
void fxLaneFoot(Ctx& c, int r, float x, float y, float w) {
  ImGui::PushID(r + 6850);
  const float h = M(28);
  {
    MonoText mono;
    capsText(c, x, y + h * 0.5f - T(10) * 0.62f, r == 0 ? "Out" : "Return", c.P.textFaint, T(10), M(0.8f));
  }
  float fx = x + capsW(c, r == 0 ? "Out" : "Return", T(10), M(0.8f)) + M(12);
  const int lk = r == 0 ? G_MasterVol : (r == 1 ? G_Bus1Vol : G_Bus2Vol);
  float vw;
  {
    MonoText mono;
    vw = std::max(textW(c, ptext(c, lk).c_str(), T(11.5f), false), textW(c, "-12.0 dB", T(11.5f), false));
  }
  const float lw = textW(c, "Level", T(11.5f), false) + vw + M(32);
  bar(c, lk, fx, y, lw, h, "Level", BarField);
  fx += lw + M(8);
  // Goes to, at the right
  // where it goes, as the engine mixes it: a chained send feeds the other one instead of returning
  // (both ways: Send B feeds Send A and neither returns)
  const int chain = vali(c, tailP(T_BusChain));
  const bool chained = (r == 1 && (chain & 1)) || (r == 2 && chain >= 2);
  const char* dest = "Plasma out";
  if (r > 0) dest = chained ? (r == 1 ? (chain == 3 ? "Nothing" : "Send B") : "Send A") : vali(c, r == 1 ? G_Bus1Dest : G_Bus2Dest) == 0 ? "Main" : "Plasma out";
  float dw;
  {
    MonoText mono;
    dw = textW(c, dest, T(11.5f), false);
  }
  const float gw = textW(c, "Goes to", T(11.5f), false) + dw + M(30), gx = x + w - gw;
  ImGui::SetCursorScreenPos(c.p(gx, y));
  if (ImGui::InvisibleButton("##goes", c.F.sz(gw, h)) && r > 0) {
    if (chained) setv(c, tailP(T_BusChain), chain & ~(r == 1 ? 1 : 2));  // back to returning
    else setv(c, r == 1 ? G_Bus1Dest : G_Bus2Dest, vali(c, r == 1 ? G_Bus1Dest : G_Bus2Dest) == 0 ? 1 : 0);
  }
  const bool hv = ImGui::IsItemHovered();
  if (hv)
    c.a.setHint(r == 0     ? "Main goes to Plasma's output"
                : chained ? (chain == 3 && r == 1 ? "Both sends feed each other: Send B goes into Send A and Send A returns nothing (click: Send A returns again)"
                                                  : "This send feeds the other send instead of returning (click: it returns again)")
                          : "Where this send returns: Main (into the main chain) or straight to Plasma's output (click to switch)");
  layoutBox(c, gx, y, gw, h, "Goes to");
  c.dl->AddRectFilled(c.p(gx, y), c.p(gx + gw, y + h), c.P.field, c.s(M(4)));
  c.dl->AddRect(c.p(gx, y), c.p(gx + gw, y + h), hv && r > 0 ? c.P.lineHi : c.P.line, c.s(M(4)));
  text(c, gx + M(10), y + h * 0.5f - T(11.5f) * 0.68f, "Goes to", c.P.label, T(11.5f), false);
  {
    MonoText mono;
    text(c, gx + gw - M(10) - dw, y + h * 0.5f - T(11.5f) * 0.68f, dest, c.P.text, T(11.5f), false);
  }
  // in between: Main keeps its effects on a preset load; Send A can feed Send B
  const float mw = gx - M(8) - fx;
  if (r == 0 && mw > M(60)) {
    if (btnLook(c, "lock", fx, y, mw, h, mw > M(120) ? "Keep on load" : "Keep", true, c.m.lockFx, T(11.5f))) {
      c.m.lockFx = !c.m.lockFx;
      commit(c, "Keep effects on preset load");
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Keep on load: loading another sound keeps these effects");
  } else if (r > 0 && mw > M(60)) {  // feed the other send (both: Send B into Send A, Send A returns nothing)
    const int bit = r == 1 ? 1 : 2;
    const char* lb = r == 1 ? (mw > M(110) ? "To Send B" : "To B") : (mw > M(110) ? "To Send A" : "To A");
    reach(tailP(T_BusChain));
    if (btnLook(c, "chain", fx, y, mw, h, lb, true, (chain & bit) != 0, T(11.5f))) setv(c, tailP(T_BusChain), chain ^ bit);
    if (ImGui::IsItemHovered()) c.a.setHint(r == 1 ? "Send A feeds Send B instead of returning" : "Send B feeds Send A instead of returning");
  }
  ImGui::PopID();
}

const char* fxName(int type) { return fxDisplayName(type); }

bool addFxAt(Ctx& c, int type, int rack, int beforeSlot) {
  if (freeSlot(c) < 0) return false;
  int idx = -1;
  if (beforeSlot >= 0) {
    const auto nodes = resolvedFx(c.m, c.st.params);
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].slot == beforeSlot) idx = static_cast<int>(i);
  }
  addFx(c, type, rack, -1, 0, idx);
  return true;
}

}  // namespace gl::packs::plasma::ui
