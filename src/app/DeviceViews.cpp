// Gridline - compact device views (K-112c). See DeviceViews.h.
#include "DeviceViews.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "../engine/Dsp.h"
#include "../engine/EqDesign.h"
#include "../engine/GraphModels.h"
#include "../engine/LiveGraph.h"
#include "App.h"
#include "PluginEditorsDynFl.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "../packs/samplers/patchplayer.h"

namespace gl::devview {

namespace {

using namespace pp;

enum class Kind { None, Eq, Filter, Comp, Limiter, Clipper, Dist, Crush, Gate, Reverb, Delay, Chorus, Flanger, Phaser, Envelope, Decay };

// A model or mode choice's value (0 when the state is short).
float model(const PluginState& st, int k) { return k >= 0 && static_cast<size_t>(k) < st.params.size() ? st.params[static_cast<size_t>(k)] : 0.0f; }

float pv(const PluginState& st, int k, float d = 0) { return k >= 0 && static_cast<size_t>(k) < st.params.size() ? st.params[static_cast<size_t>(k)] : d; }

// The first parameter named `name` (an instrument's envelope), or -1.
int paramNamed(const PluginInfo& info, const char* name) {
  for (size_t k = 0; k < info.params.size(); ++k)
    if (info.params[k].name == name) return static_cast<int>(k);
  return -1;
}

struct Env {
  int a = -1, d = -1, s = -1, r = -1;
  bool ok() const { return a >= 0 && d >= 0 && s >= 0 && r >= 0; }
};
Env envelopeOf(const PluginInfo& info) { return Env{paramNamed(info, "Attack"), paramNamed(info, "Decay"), paramNamed(info, "Sustain"), paramNamed(info, "Release")}; }

// Instruments with one decay (drums, plucked and struck sounds): its parameter and the time the picture spans.
struct DecayOf {
  int param = -1;
  double span = 1;
};
DecayOf decayOf(const std::string& ty) {
  if (ty == "gl.kick") return {KickDecay, 1.0};
  if (ty == "gl.snare") return {SnDecay, 0.6};
  if (ty == "gl.hat") return {HatDecay, 0.6};
  if (ty == "gl.clap") return {ClapDecay, 0.6};
  if (ty == "gl.pluck") return {PlkDecay, 8.0};
  if (ty == "gl.keys") return {KyDecay, 8.0};
  return {};
}

// The EQ band the device row works on: the one selected in the EQ's editor, else the band that
// moves the sound most, else the first band that is on.
bool eqBandOn(const PluginState& st, int b) {
  return static_cast<int>(std::lround(pv(st, eqParam(b, EqType)))) != EqOff && pv(st, eqExt(b, EqEnabled), 1) >= 0.5f;
}
int eqBand(const PluginTarget& t, const PluginState& st) {
  const int sel = eqSelectedBand(t);
  if (sel >= 0 && sel < kEqBands) return sel;
  int best = -1;
  float most = 0.05f;
  for (int b = 0; b < kEqBands; ++b)
    if (eqBandOn(st, b) && std::fabs(pv(st, eqParam(b, EqGain))) > most) most = std::fabs(pv(st, eqParam(b, EqGain))), best = b;
  if (best >= 0) return best;
  for (int b = 0; b < kEqBands; ++b)
    if (eqBandOn(st, b)) return b;
  return 0;
}

Kind kindOf(const PluginInfo* info, const PluginTarget& t) {
  if (!info) return Kind::None;
  const std::string& ty = info->type;
  if (ty == "gl.eq") return Kind::Eq;
  if (ty == "gl.filter") return Kind::Filter;
  if (ty == "gl.comp") return Kind::Comp;
  if (ty == "gl.limiter") return Kind::Limiter;
  if (ty == "gl.clipper") return Kind::Clipper;
  if (ty == "gl.dist") return Kind::Dist;
  if (ty == "gl.reverb") return Kind::Reverb;
  if (ty == "gl.delay") return Kind::Delay;
  if (ty == "gl.chorus") return Kind::Chorus;
  if (ty == "gl.flanger") return Kind::Flanger;
  if (ty == "gl.phaser") return Kind::Phaser;
  if (ty == "gl.bitcrush") return Kind::Crush;
  if (ty == "gl.gate") return Kind::Gate;
  if (t.isChannel() && envelopeOf(*info).ok()) return Kind::Envelope;
  if (t.isChannel() && decayOf(ty).param >= 0) return Kind::Decay;
  return Kind::None;
}

// The pointer on a picture this frame (its invisible button).
struct Ptr {
  bool hov = false, pressed = false, active = false, released = false, dbl = false;
};
// The picture handle being dragged: which device, which handle (an EQ band), the grab offset.
struct PicDrag {
  uint64_t key = 0;
  int what = -1;
  ImVec2 off;
};
PicDrag& picDrag() {
  static PicDrag d;
  return d;
}

// Parameter edits from a picture drag (merged into the drag's one undo step).
void setAbs(App& a, const PluginTarget& t, const PluginInfo& info, int k, double v) {
  if (k < 0 || static_cast<size_t>(k) >= info.params.size()) return;
  a.doc.setParam(t.pid(k), info.params[static_cast<size_t>(k)].clamp(v), true);
}
void nudge(App& a, const PluginTarget& t, const PluginInfo& info, const PluginState& st, int k, double dNorm) {
  if (k < 0 || static_cast<size_t>(k) >= info.params.size() || dNorm == 0) return;
  const ParamDesc& d = info.params[static_cast<size_t>(k)];
  const double cur = pv(st, k, static_cast<float>(d.def));
  a.doc.setParam(t.pid(k), d.clamp(d.fromNormalized(std::clamp(d.toNormalized(cur) + dNorm, 0.0, 1.0))), true);
}

// The dark inset every view sits in, with a faint grid.
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows) {
  const auto& P = theme::pal();
  dl->AddRectFilled(p0, p1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
  const ImU32 g = theme::withAlpha(P.textFaint, 0.18f);
  for (int i = 1; i < cols; ++i) {
    const float x = std::round(p0.x + (p1.x - p0.x) * i / cols) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), g);
  }
  for (int i = 1; i < rows; ++i) {
    const float y = std::round(p0.y + (p1.y - p0.y) * i / rows) + 0.5f;
    dl->AddLine(ImVec2(p0.x + 1, y), ImVec2(p1.x - 1, y), g);
  }
}

ImU32 ink() { return theme::pal().hot; }

// A curve and the area between it and a base line.
void curve(ImDrawList* dl, const std::vector<ImVec2>& pts, float baseY, bool fill) {
  if (pts.size() < 2) return;
  const ImU32 c = ink();
  if (fill)
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
      ImVec2 q[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, baseY), ImVec2(pts[i].x, baseY)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(c, 0.16f));
    }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c, 1.5f, 0);
}

// ---- frequency responses ----
bool eqView(App& a, const PluginTarget& t, const PluginInfo& info, const PluginState& st, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Ptr& ptr) {
  const double sr = a.engine.sampleRate();
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const int W = std::max(2, static_cast<int>(w / 2));
  EqBandFilters f[kEqBands];
  for (int b = 0; b < kEqBands; ++b) designEqBand(st, b, sr, f[b]);
  const livegraph::Curve& total = livegraph::memo<livegraph::Curve>(t.key(), 0x4B313132).get(livegraph::Hasher().floats(st.params).d(sr).i(W).value(), [&](livegraph::Curve& o) {
    o.assign(static_cast<size_t>(W), 0.0f);
    for (int b = 0; b < kEqBands; ++b)
      if (f[b].count > 0)
        for (int i = 0; i < W; ++i) o[static_cast<size_t>(i)] += static_cast<float>(dsp::gainToDb(eqBandMagnitude(f[b], pui::xToFreq(p0.x + w * i / (W - 1), p0.x, w), sr)));
  });
  constexpr float kRange = 18;
  auto y = [&](double db) { return p0.y + h * 0.5f - static_cast<float>(std::clamp(db, -24.0, 24.0) / kRange) * h * 0.5f; };
  std::vector<ImVec2> pts(static_cast<size_t>(W));
  for (int i = 0; i < W; ++i) pts[static_cast<size_t>(i)] = ImVec2(p0.x + w * i / (W - 1), y(total[static_cast<size_t>(i)]));
  dl->PushClipRect(p0, p1, true);
  curve(dl, pts, y(0), true);
  // The bands: a dot at each band's frequency and gain (every band that is switched on, flat or not).
  // The row's knobs work on the ringed band; a press on another dot picks it (the editor's selection
  // too) and drags it: sideways the frequency, up and down the gain, with Alt the band's width.
  // A double-click on a dot flattens the band.
  const int cur = eqBand(t, st);
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse = io.MousePos;
  int nearest = -1;
  float nearD = 64.0f;  // within 8 px
  ImVec2 nearC;
  bool nearGain = false;
  for (int b = 0; b < kEqBands; ++b) {
    if (!eqBandOn(st, b)) continue;
    const int type = static_cast<int>(std::lround(pv(st, eqParam(b, EqType))));
    const bool gainType = type == EqLowShelf || type == EqPeak || type == EqHighShelf;
    const ImVec2 c(pui::freqToX(pv(st, eqParam(b, EqFreq), 1000), p0.x, w), gainType ? y(pv(st, eqParam(b, EqGain))) : y(0));
    const float dx = mouse.x - c.x, dy = mouse.y - c.y;
    if (dx * dx + dy * dy < nearD) nearD = dx * dx + dy * dy, nearest = b, nearC = c, nearGain = gainType;
    dl->AddCircleFilled(c, 3.0f, b == cur ? theme::pal().hot : theme::pal().text, 12);
    if (b == cur) dl->AddCircle(c, 5.5f, theme::pal().hot, 16, 1.2f);
  }
  dl->PopClipRect();
  PicDrag& pd = picDrag();
  if (ptr.pressed) {
    pd.key = t.key(), pd.what = nearest;
    if (nearest >= 0) eqSelectBand(t, nearest), pd.off = ImVec2(nearC.x - mouse.x, nearC.y - mouse.y);
  }
  if (ptr.dbl && nearest >= 0) {
    if (nearGain) a.doc.setParam(t.pid(eqParam(nearest, EqGain)), 0.0, false);
    pd.what = -1;
    return false;
  }
  if (ptr.active && pd.key == t.key() && pd.what >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
    const int b = pd.what;
    const int type = static_cast<int>(std::lround(pv(st, eqParam(b, EqType))));
    const bool gainType = type == EqLowShelf || type == EqPeak || type == EqHighShelf;
    if (io.KeyAlt) {
      const bool legacy = model(st, EqModel) < 0.5f;
      nudge(a, t, info, st, legacy ? eqParam(b, EqQ) : eqFl(b, EqWidth), -io.MouseDelta.y / std::max(20.0f, h));
    } else {
      const float tx = std::clamp(mouse.x + pd.off.x, p0.x, p1.x), ty = std::clamp(mouse.y + pd.off.y, p0.y, p1.y);
      setAbs(a, t, info, eqParam(b, EqFreq), pui::xToFreq(tx, p0.x, w));
      if (gainType) setAbs(a, t, info, eqParam(b, EqGain), (p0.y + h * 0.5f - ty) / (h * 0.5f) * kRange);
    }
  }
  return ptr.dbl;
}

void filterView(App& a, const PluginTarget& t, const PluginState& st, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const double sr = a.engine.sampleRate();
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const int W = std::max(2, static_cast<int>(w / 2));
  const livegraph::Curve& c = livegraph::memo<livegraph::Curve>(t.key(), 0x4B313133).get(livegraph::Hasher().floats(st.params).d(sr).i(W).value(), [&](livegraph::Curve& o) {
    o.resize(static_cast<size_t>(W));
    for (int i = 0; i < W; ++i) o[static_cast<size_t>(i)] = static_cast<float>(graphmodel::filterResponseDb(st, sr, pui::xToFreq(p0.x + w * i / (W - 1), p0.x, w)));
  });
  auto y = [&](double db) { return p0.y + h * 0.35f - static_cast<float>(std::clamp(db, -48.0, 24.0) / 48.0) * h * 0.6f; };
  std::vector<ImVec2> pts(static_cast<size_t>(W));
  for (int i = 0; i < W; ++i) pts[static_cast<size_t>(i)] = ImVec2(p0.x + w * i / (W - 1), y(c[static_cast<size_t>(i)]));
  dl->PushClipRect(p0, p1, true);
  curve(dl, pts, p1.y, true);
  dl->PopClipRect();
}

// ---- transfer curves (input across, output up) ----
template <class F>
void transfer(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double lo, double hi, F&& out, double markIn = 1e9) {
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  auto X = [&](double v) { return p0.x + static_cast<float>((v - lo) / (hi - lo)) * w; };
  auto Y = [&](double v) { return p1.y - static_cast<float>((std::clamp(v, lo, hi) - lo) / (hi - lo)) * h; };
  dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), theme::withAlpha(theme::pal().textFaint, 0.35f));  // unity
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 64; ++i) {
    const double x = lo + (hi - lo) * i / 64.0;
    pts.push_back(ImVec2(X(x), Y(out(x))));
  }
  dl->PushClipRect(p0, p1, true);
  curve(dl, pts, p1.y, false);
  if (markIn > lo && markIn < hi) dl->AddCircleFilled(ImVec2(X(markIn), Y(out(markIn))), 3.0f, theme::pal().text, 12);
  dl->PopClipRect();
}

// Gain reduction as a bar down from the top at the right edge.
void grBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float grDb) {
  const float h = p1.y - p0.y, d = std::clamp(-grDb / 24.0f, 0.0f, 1.0f) * h;
  dl->AddRectFilled(ImVec2(p1.x - 5, p0.y), ImVec2(p1.x - 1, p0.y + h), theme::withAlpha(theme::pal().textFaint, 0.25f));
  if (d > 0.5f) dl->AddRectFilled(ImVec2(p1.x - 5, p0.y), ImVec2(p1.x - 1, p0.y + d), theme::pal().hot);
}

// ---- time pictures ----
void reverbView(App& a, const PluginTarget& t, const PluginState& st, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const double sr = a.engine.sampleRate(), tempo = a.doc.song().tempo;
  const graphmodel::ReverbView& rv = livegraph::memo<graphmodel::ReverbView>(t.key(), 0x4B313134).get(
      livegraph::Hasher().floats(st.params).d(sr).d(tempo).value(), [&](graphmodel::ReverbView& o) { o = graphmodel::reverbView(st, sr, tempo); });
  const double span = std::max(0.5, rv.preSec + std::min(rv.rt60, 12.0) * 1.1);
  const float w = p1.x - p0.x, h = p1.y - p0.y - 4;
  auto X = [&](double sec) { return p0.x + static_cast<float>(sec / span) * w; };
  dl->PushClipRect(p0, p1, true);
  for (int i = 0; i < rv.erCount && i < static_cast<int>(rv.erSec.size()); ++i) {  // early reflections
    const float x = X(rv.erSec[static_cast<size_t>(i)]), g = static_cast<float>(std::clamp(rv.erGain[static_cast<size_t>(i)], 0.0, 1.0));
    dl->AddLine(ImVec2(x, p1.y), ImVec2(x, p1.y - g * h * 0.8f), theme::withAlpha(theme::pal().text, 0.6f), 1.0f);
  }
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 64; ++i) {
    const double sec = rv.preSec + (span - rv.preSec) * i / 64.0;
    pts.push_back(ImVec2(X(sec), p1.y - static_cast<float>(graphmodel::decayAmp(sec - rv.preSec, std::max(0.05, rv.rt60))) * h * 0.9f));
  }
  pts.insert(pts.begin(), ImVec2(X(rv.preSec), p1.y));
  curve(dl, pts, p1.y, true);
  dl->PopClipRect();
}

void delayView(const PluginState& st, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const bool classic = std::lround(model(st, DlyModel)) == 2;  // the classic model has its own feedback
  const float fb = std::clamp(pv(st, classic ? DlyClsFb : DlyFeedback, 0.4f), 0.0f, 1.0f), wet = std::clamp(pv(st, DlyWet, 1), 0.0f, 1.0f);
  const float w = p1.x - p0.x, h = p1.y - p0.y - 6;
  const ImU32 c = ink();
  dl->AddLine(ImVec2(p0.x + 6, p1.y - 3), ImVec2(p0.x + 6, p1.y - 3 - h), theme::pal().text, 2.0f);  // the input
  float g = std::max(0.15f, wet);
  for (int n = 1; n <= 10 && g > 0.02f; ++n) {
    const float x = p0.x + 6 + (w - 12) * n / 10.0f;
    dl->AddLine(ImVec2(x, p1.y - 3), ImVec2(x, p1.y - 3 - h * g), c, 2.0f);
    g *= fb;
  }
}

void chorusView(const PluginState& st, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const float depth = std::clamp(pv(st, ChoDepth, 0.5f), 0.0f, 1.0f);
  const float w = p1.x - p0.x, cy = (p0.y + p1.y) * 0.5f, amp = (p1.y - p0.y) * 0.42f * std::max(0.08f, depth);
  for (int v = 0; v < 3; ++v) {  // three voices, a third of a cycle apart
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 48; ++i) {
      const float u = i / 48.0f;
      pts.push_back(ImVec2(p0.x + w * u, cy - amp * std::sin(u * 6.2831853f * 2.0f + v * 2.0944f)));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(ink(), v == 0 ? 1.0f : 0.5f), 1.4f, 0);
  }
}

// A modulation sweep: the swept delay or frequency over two seconds, as fast and as deep as set.
void sweepView(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float cycles, float depth) {
  const float w = p1.x - p0.x, cy = (p0.y + p1.y) * 0.5f, amp = (p1.y - p0.y) * 0.42f * std::clamp(depth, 0.06f, 1.0f);
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 64; ++i) {
    const float u = static_cast<float>(i) / 64.0f;
    pts.push_back(ImVec2(p0.x + w * u, cy - amp * std::sin(u * 6.2831853f * std::clamp(cycles, 0.25f, 12.0f))));
  }
  curve(dl, pts, cy, true);
}

// An instrument's single decay: the level falling away over the picture's span.
void decayView(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double decaySec, double span) {
  const float w = p1.x - p0.x, h = p1.y - p0.y - 4;
  std::vector<ImVec2> pts;
  pts.push_back(ImVec2(p0.x, p1.y));
  for (int i = 0; i <= 64; ++i) {
    const double sec = span * i / 64.0;
    pts.push_back(ImVec2(p0.x + w * static_cast<float>(i) / 64.0f, p1.y - static_cast<float>(graphmodel::decayAmp(sec, std::max(0.01, decaySec))) * h));
  }
  curve(dl, pts, p1.y, true);
}

const char* hintOf(Kind k) {
  switch (k) {
    case Kind::Eq: return i18n::N_("The EQ curve: drag a dot for its frequency and gain (Alt: its width), double-click it to flatten it");
    case Kind::Filter: return i18n::N_("The filter's response: drag sideways for the cutoff, up or down for the resonance");
    case Kind::Comp: return i18n::N_("The transfer curve: drag sideways for the threshold, up or down for the ratio");
    case Kind::Limiter: return i18n::N_("The transfer curve: drag sideways for the gain, up or down for the ceiling");
    case Kind::Clipper: return i18n::N_("The transfer curve: drag sideways for the threshold");
    case Kind::Dist: return i18n::N_("The transfer curve: drag up or down for the drive, sideways for the tone");
    case Kind::Reverb: return i18n::N_("The reverb's decay: early reflections, then the tail (drag sideways for the decay)");
    case Kind::Delay: return i18n::N_("The echoes: drag sideways for the time, up or down for the feedback");
    case Kind::Chorus: return i18n::N_("The chorus voices moving against each other");
    case Kind::Flanger: case Kind::Phaser: return i18n::N_("The sweep: as fast and as deep as the rate and depth set");
    case Kind::Crush: return i18n::N_("A test wave as the bitcrusher steps it (drag sideways for the rate, up or down for the bits)");
    case Kind::Gate: return i18n::N_("The gate: drag sideways for the threshold, up or down for the range");
    case Kind::Decay: return i18n::N_("The sound's decay: how fast it fades");
    case Kind::Envelope: return i18n::N_("The amplitude envelope: attack, decay, sustain and release (drag the points)");
    default: return "";
  }
}

}  // namespace

float width(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  const PluginState* st = t.state(s);
  if (!st) return 0;
  switch (kindOf(t.info(s), t)) {
    case Kind::None: return 0;
    case Kind::Eq: case Kind::Filter: case Kind::Envelope: case Kind::Crush: return 176;
    default: return 120;
  }
}

namespace {

// What dragging a picture sideways (x) and up or down (y) changes (-1: nothing).
struct Axes {
  int x = -1, y = -1;
};
Axes axesOf(Kind k, const PluginState& st) {
  switch (k) {
    case Kind::Filter: return model(st, FltModel) >= 0.5f ? Axes{FltCutK, FltResK} : Axes{FltCutoff, FltReso};
    case Kind::Comp: return Axes{CmpThreshold, CmpRatio};
    case Kind::Limiter: return Axes{LimGain, LimCeiling};
    case Kind::Clipper: return Axes{ClpThreshold, -1};
    case Kind::Gate: return Axes{GtThreshold, GtRange};
    case Kind::Dist: return Axes{DstTone, DstDrive};
    case Kind::Reverb: return Axes{std::lround(model(st, RvbModel)) == 2 ? RvbFDecay : RvbDecayTime, -1};
    case Kind::Delay:
      switch (std::lround(model(st, DlyModel))) {
        case 0: return Axes{DlyTime, DlyFeedback};
        case 1: return Axes{DlyTicks, DlyFeedback};
        case 2: return Axes{DlyTicks, DlyClsFb};
        default: return Axes{};
      }
    default: return Axes{};
  }
}

}  // namespace

bool draw(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const Song& s = a.doc.song();
  const PluginState* sp = t.state(s);
  const PluginInfo* info = t.info(s);
  const Kind k = kindOf(info, t);
  if (!sp || k == Kind::None) return false;
  const PluginState st = *sp;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool dblHere = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered();
  if (k == Kind::Envelope) {  // the editors' own envelope widget (draggable points)
    const Env e = envelopeOf(*info);
    ImGui::SetCursorScreenPos(p0);
    pui::adsrGraph(a, t, e.a, e.d, e.s, e.r, ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr(hintOf(k)));
    return dblHere;
  }
  if (k == Kind::Crush) {  // the editor's own preview (it frames itself and takes the drag)
    crushView(a, t, p0, p1);
    return dblHere;
  }
  // The picture is one invisible button: a press grabs a handle, a drag moves it (one undo step).
  Ptr ptr;
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##picture", ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  ptr.hov = ImGui::IsItemHovered();
  ptr.pressed = ImGui::IsItemActivated();
  ptr.active = ImGui::IsItemActive();
  ptr.released = ImGui::IsItemDeactivated();
  ptr.dbl = ptr.hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  if (ptr.released) a.doc.endGesture();
  const Axes ax = k == Kind::Eq ? Axes{} : axesOf(k, st);
  if (ptr.active && (ax.x >= 0 || ax.y >= 0)) {
    const ImGuiIO& io = ImGui::GetIO();
    const double fine = io.KeyShift ? 0.1 : 1.0;
    nudge(a, t, *info, st, ax.x, io.MouseDelta.x / std::max(20.0f, p1.x - p0.x) * fine);
    nudge(a, t, *info, st, ax.y, -io.MouseDelta.y / std::max(20.0f, p1.y - p0.y) * fine);
  }
  if (ptr.hov || ptr.active) ImGui::SetMouseCursor(k == Kind::Eq ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
  well(dl, p0, p1, k == Kind::Eq || k == Kind::Filter ? 6 : 4, 4);
  const ImVec2 q0(p0.x + 3, p0.y + 3), q1(p1.x - 3, p1.y - 3);
  const dynfl::LiveMeters m = t.isChannel() ? dynfl::LiveMeters{} : dynfl::liveMeters(a, t);
  bool dbl = ptr.dbl;
  switch (k) {
    case Kind::Eq: dbl = eqView(a, t, *info, st, dl, q0, q1, ptr); break;
    case Kind::Filter: filterView(a, t, st, dl, q0, q1); break;
    case Kind::Comp:
      transfer(dl, q0, ImVec2(q1.x - 7, q1.y), -60, 0, [&](double in) { return in + graphmodel::compGainDb(st, in); }, m.in);
      grBar(dl, q0, q1, m.gr);
      break;
    case Kind::Limiter: {
      const double ceil = pv(st, LimCeiling), gain = pv(st, LimGain);
      const bool comp = graphmodel::limiterCompOn(st);
      transfer(dl, q0, ImVec2(q1.x - 7, q1.y), -36, 6,
               [&](double in) {
                 const double x = in + gain;
                 return std::min(ceil, x + (comp ? graphmodel::limiterCompGainDb(st, x) : 0.0));
               },
               m.in);
      grBar(dl, q0, q1, m.gr);
      break;
    }
    case Kind::Clipper: transfer(dl, q0, q1, 0, 1.5, [&](double x) { return clipperTransfer(st, x); }); break;
    case Kind::Dist: {
      const float cx = (q0.x + q1.x) * 0.5f, cy = (q0.y + q1.y) * 0.5f;
      dl->AddLine(ImVec2(q0.x, cy), ImVec2(q1.x, cy), theme::withAlpha(theme::pal().textFaint, 0.35f));
      dl->AddLine(ImVec2(cx, q0.y), ImVec2(cx, q1.y), theme::withAlpha(theme::pal().textFaint, 0.35f));
      transfer(dl, q0, q1, -1.2, 1.2, [&](double x) { return distTransfer(st, x); });
      break;
    }
    case Kind::Reverb: reverbView(a, t, st, dl, q0, q1); break;
    case Kind::Delay: delayView(st, dl, q0, q1); break;
    case Kind::Chorus: chorusView(st, dl, q0, q1); break;
    case Kind::Flanger: {
      const bool ref = std::lround(model(st, FlgModel)) == 1;  // the reference engine's controls are 0..1
      sweepView(dl, q0, q1, ref ? 1.0f + pv(st, FlgRateN, 0.4f) * 6.0f : pv(st, FlgRate, 0.2f) * 2.0f, pv(st, ref ? FlgDepthN : FlgDepth, 0.5f));
      break;
    }
    case Kind::Phaser: {
      const bool ref = std::lround(model(st, PhsModel)) == 1;
      sweepView(dl, q0, q1, ref ? 1.0f + pv(st, PhsSweepN, 0.5f) * 6.0f : pv(st, PhsRate, 0.4f) * 2.0f, pv(st, ref ? PhsDepthMax : PhsDepth, 0.7f));
      break;
    }
    case Kind::Gate: {
      const double thr = pv(st, GtThreshold, -40), range = pv(st, GtRange, -80);
      const bool duck = pv(st, GtFlip) >= 0.5f;  // ducking: the range applies above the threshold
      transfer(dl, q0, q1, -80, 0, [&](double in) { return (in < thr) != duck ? in + range : in; });
      break;
    }
    case Kind::Decay: {
      const DecayOf d = decayOf(info->type);
      decayView(dl, q0, q1, pv(st, d.param, 0.3f), d.span);
      break;
    }
    default: break;
  }
  if (ptr.hov) a.setHint(i18n::tr(hintOf(k)));
  return dbl;
}

std::vector<Main> mainParams(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  const PluginState* sp = t.state(s);
  const PluginInfo* info = t.info(s);
  if (!sp || !info) return {};
  const PluginState& st = *sp;
  const std::string& ty = info->type;
  auto list = [](std::initializer_list<int> ps) {
    std::vector<Main> v;
    for (int p : ps) v.push_back(Main{p, std::string()});
    return v;
  };
  if (ty == "gl.eq") {  // the selected band: frequency, gain and its width (Q in the legacy model), then its type
    const int b = eqBand(t, st);
    const std::string n = std::string(i18n::tr("Band")) + " " + std::to_string(b + 1) + " ";
    const bool legacy = model(st, EqModel) < 0.5f;
    return {Main{eqParam(b, EqFreq), n + i18n::tr("freq")}, Main{eqParam(b, EqGain), n + i18n::tr("gain")},
            Main{legacy ? eqParam(b, EqQ) : eqFl(b, EqWidth), n + (legacy ? std::string("Q") : std::string(i18n::tr("width")))},
            Main{eqParam(b, EqType), n + i18n::tr("type")}};
  }
  if (ty == packs::samplers::kPatchPlayerType)  // K-333: the patch, its level and sends, the filter offsets
    return list({packs::samplers::PpPatch, packs::samplers::PpVolume, packs::samplers::PpPan, packs::samplers::PpReverb, packs::samplers::PpChorus,
                 packs::samplers::PpCutoff, packs::samplers::PpResonance, packs::samplers::PpTranspose});
  if (ty == "gl.filter") return model(st, FltModel) >= 0.5f ? list({FltCutK, FltResK, FltMode, FltMix}) : list({FltCutoff, FltReso, FltMode, FltMix});
  if (ty == "gl.comp") return list({CmpThreshold, CmpRatio, CmpAttack, CmpRelease, CmpMakeup, CmpMix});
  if (ty == "gl.limiter") return list({LimGain, LimCeiling, LimRelease, LimCompThresh});
  if (ty == "gl.gate") return list({GtThreshold, GtRange, GtAttack, GtRelease});
  if (ty == "gl.dist") return list({DstDrive, DstTone, DstMix, DstOutput});
  if (ty == "gl.bitcrush") return list({BcBits, BcRate, BcMix, BcOutput});
  if (ty == "gl.chorus") return list({ChoRate, ChoDepth, ChoDelay, ChoMix});
  if (ty == "gl.flanger")
    return std::lround(model(st, FlgModel)) == 1 ? list({FlgRateN, FlgDepthN, FlgFeedN, FlgWetN}) : list({FlgRate, FlgDepth, FlgFeedback, FlgMix});
  if (ty == "gl.phaser")
    return std::lround(model(st, PhsModel)) == 1 ? list({PhsSweepN, PhsDepthMax, PhsFeedbackN, PhsDryN}) : list({PhsRate, PhsDepth, PhsFeedback, PhsMix});
  if (ty == "gl.reverb")
    return std::lround(model(st, RvbModel)) == 2 ? list({RvbFDecay, RvbFRoom, RvbFPredelay, RvbFWet}) : list({RvbDecayTime, RvbSize, RvbPreDelay, RvbWet});
  if (ty == "gl.delay") {
    switch (std::lround(model(st, DlyModel))) {
      case 0: return list({DlyTime, DlyFeedback, DlyCutoff, DlyWet});
      case 1: return list({DlyTicks, DlyFeedback, DlyCutoff3, DlyWet});
      case 2: return list({DlyTicks, DlyClsFb, DlyClsCoef, DlyWet});
      default: return {};
    }
  }
  if (ty == "gl.synth") return list({SynCutoff, SynResonance, SynOsc2Level, SynOsc3Level, SynGlide, SynVolume});
  if (ty == "gl.sampler") return list({SmpCutoff, SmpReso, SmpStart, SmpFine, SmpPan, SmpVolume});
  if (ty == "gl.acid") return list({AcCutoff, AcReso, AcEnvMod, AcDecay, AcAccent, AcDrive});
  if (ty == "gl.kick") return list({KickPitch, KickEndPitch, KickSweep, KickClick, KickDrive, KickVolume});
  return {};
}

}  // namespace gl::devview
