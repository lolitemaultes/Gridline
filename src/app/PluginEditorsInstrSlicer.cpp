// Gridline — Kerf, the region slicer editor (J-2, J-176, J-181).
// Laid out on the reference region slicer's form (714 × 625) as its bands:
// master band (sliders, X/Y pad, slicing mode, options, display mode), region
// panel (region selector and wheels, filter, a tabbed view: amp envelope /
// region / detection), deck bar (transport, menus, tools, sample info), the
// wave view (region flags, layered waveform, ruler, scroller) and the keyboard.
// Display modes hide bands (the form height follows) and the form scales down
// to fit a short channel window. Kerf's own drawing pieces: PluginEditorsInstrSlicerKit.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <tuple>
#include "../engine/SlicexEngine.h"
#include "../core/I18n.h"
#include "../core/PluginBrand.h"
#include "../core/PluginParams.h"
#include "../engine/Slicing.h"
#include "BrowserUi.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"
#include "PluginEditorsInstrSlicerKit.h"
#include "RunLatch.h"
#include "Widgets.h"
#include "../core/Platform.h"

namespace gl {

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width);

namespace {

using namespace pp;
using iskin::lerp;
using iskin::rgb;
namespace fs = std::filesystem;

constexpr float kSxW = 714;  // form width (the full form is 625 high: the bands below)
constexpr uint32_t kOrange = 0xFFA830, kOrangeDeep = 0xF5781B;

// J-110: the deck's Play (one region played at a time, whichever instance);
// J-181: Loop plays it again each time it ends.
struct SlicerPlay {
  RunLatch latch;
  ObjectId ch = kNoId;
  int key = -1;
  bool loop = false;
};
SlicerPlay& slicerPlay() {
  static SlicerPlay p;
  return p;
}
bool slicerSounding(App& a, ObjectId ch) {
  const auto rt = a.builder.channelRuntime(ch);
  return rt && rt->sounding.load(std::memory_order_relaxed);
}
// Stop / panic / focus loss: off at once (the engine released the key); the
// region came to its end: off and its key let go, or (Loop) played again.
void syncSlicerPlay(App& a) {
  SlicerPlay& pl = slicerPlay();
  const RunLatch::End end = pl.latch.sync(a.engine, slicerSounding(a, pl.ch));
  if (end == RunLatch::End::Finished && pl.key >= 0) {
    a.engine.noteOff(pl.ch, pl.key);
    if (pl.loop && pl.latch.seen) {
      a.engine.ownNoteOn(pl.ch, pl.key, 0.9f);
      pl.latch.start(a.engine);
      return;
    }
  }
  if (end != RunLatch::End::None) pl.key = -1;
}

iskin::Palette slicexPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x1A2633);
  p.bgBottom = rgb(0x111A23);
  p.panel = rgb(0x41474B);
  p.panelLine = rgb(0x0E1419);
  p.rail = rgb(0x3F484D);
  p.fill = rgb(kOrange);
  p.fill2 = rgb(0x989EA7);
  p.fill3 = rgb(kOrangeDeep);
  p.cap = rgb(0x4A5257);
  p.handle = rgb(0x30373B);
  p.label = rgb(0xA7B0B9);
  p.text = rgb(0xE9EDFA);
  p.textDim = rgb(0xACB4BA);
  p.lcdBg = rgb(0x6E7186);
  p.lcdText = rgb(0xF2F4FB);
  p.led = rgb(kOrange);
  p.ledOff = rgb(0x2A3136);
  return p;
}

// Per editor instance (UI only, not saved).
struct SlicerUi {
  int slice = 0;
  int drag = -1;                     // marker being dragged (slice index)
  double viewFrom = 0, viewTo = 1;   // the wave view shown (fractions of the sample)
  double goFrom = 0, goTo = 1;       // where it glides to (smooth zoom / scroll)
  int kbLow = -1;                    // the keyboard's lowest key (follows the selected region)
  int tab = -1;                      // region panel view: 0 amp, 1 region, 2 detection (-1: by engine)
  int ampPart = 0;                   // amp view: 0 envelope, 1 velocity
  int display = 0;                   // 0 full, 1 no wave editor, 2 no region panel, 3 compact
  bool keys = true;                  // keyboard band shown
  int snap = 0;                      // marker snapping: 0 off, 1 zero crossings, 2 sixteenths
  int kbMenuKey = -1;                // key under the keyboard's context menu
};
std::map<uint64_t, SlicerUi> slicerUi;

// Slice starts (frames) as the engine computes them.
std::vector<int64_t> sliceStarts(const PluginState& st, const Sample* smp) {
  std::vector<int64_t> out;
  if (!smp || smp->frames() == 0) return out;
  if (ed::ival(st, SlMode) == SlModeManual) {
    for (int k = 0; k < kMaxSlices; ++k) {
      float m = ed::val(st, slMarker(k), -1);
      if (m >= 0) out.push_back(std::llround(static_cast<double>(m) * static_cast<double>(smp->frames())));  // J-166: nearest frame
    }
    std::sort(out.begin(), out.end());
  } else {
    static std::map<std::string, std::pair<std::array<float, 3>, std::vector<int64_t>>> cache;
    auto& c = cache[smp->path];
    const std::array<float, 3> key{static_cast<float>(ed::ival(st, SlMode)), ed::val(st, SlSensitivity, 0.5f), ed::val(st, SlMinGap, 0.06f)};
    if (c.first != key || c.second.empty()) c.first = key, c.second = computeSlices(*smp, static_cast<int>(key[0]), key[1], key[2]);
    out = c.second;
    if (out.size() > static_cast<size_t>(kMaxSlices)) out.resize(kMaxSlices);
  }
  return out;
}

// Converts the current slices to manual markers (so they can be edited).
void makeManual(App& a, const PluginTarget& t, const PluginState& st, const std::vector<int64_t>& starts, const Sample* smp) {
  if (!smp || ed::ival(st, SlMode) == SlModeManual) return;
  PluginState ns = st;
  ns.params[static_cast<size_t>(SlMode)] = SlModeManual;
  for (int k = 0; k < kMaxSlices; ++k)
    ns.params[static_cast<size_t>(slMarker(k))] =
        k < static_cast<int>(starts.size()) ? static_cast<float>(static_cast<double>(starts[static_cast<size_t>(k)]) / static_cast<double>(smp->frames())) : -1.f;
  setPluginState(a, t, ns, "edit slices by hand");
}

// J-166: the region engine (SlEngine 1, loops brought in from project files):
// the regions exactly as they play: model starts in list order (not sorted),
// ends (deck A end parameter, else the region length, else the next region /
// the end of the loop), names and keys (explicit, else automatic).
struct RegionView {
  bool on = false;
  slicex::Model md;
  slicex::KeyMap km;
  std::vector<int64_t> starts, ends;
};
RegionView regionView(const PluginState& st, const Sample* smp) {
  RegionView v;
  const float eng = ed::val(st, SlEngine, 0);
  if (eng < 0.5f || eng >= SlEngineProcessed - 0.5f || !slicex::modelOf(st.data, v.md)) return v;
  v.on = true;
  v.km = slicex::buildKeyMap(v.md, &st);
  const auto& regs = v.md.decks[0].regions;
  const int64_t frames = smp ? static_cast<int64_t>(smp->frames()) : 0;
  for (size_t k = 0; k < regs.size(); ++k) {
    const int64_t s = regs[k].start;
    int64_t e = regs[k].length >= 0 ? s + regs[k].length : (k + 1 < regs.size() ? regs[k + 1].start : frames);
    const float pe = k < static_cast<size_t>(kMaxSlices) ? ed::val(st, slParam2(static_cast<int>(k), SliceEnd), -1) : -1.0f;
    if (pe >= 0 && frames > 0) e = std::llround(static_cast<double>(pe) * static_cast<double>(frames));
    v.starts.push_back(s);
    v.ends.push_back(std::clamp<int64_t>(e, s + 1, std::max<int64_t>(s + 1, frames)));
  }
  return v;
}

// Moves region `r`'s start (region engine): the model's region and, for the
// first 64 regions, the marker parameter (one undo step).
void moveRegionStart(App& a, const PluginTarget& t, const PluginState& st, const RegionView& rv, int r, int64_t start, const Sample* smp) {
  if (!smp || r < 0 || static_cast<size_t>(r) >= rv.md.decks[0].regions.size()) return;
  slicex::Model md = rv.md;
  auto& rg = md.decks[0].regions[static_cast<size_t>(r)];
  const int64_t frames = static_cast<int64_t>(smp->frames());
  start = std::clamp<int64_t>(start, 0, std::max<int64_t>(0, frames - 1));
  if (rg.length >= 0) rg.length = std::max<int64_t>(1, rg.start + rg.length - start);  // keep its end
  rg.start = start;
  PluginState ns = st;
  ns.data = slicex::sampleOf(st.data) + "\n" + slicex::writeText(md);
  if (r < kMaxSlices && ns.params.size() > static_cast<size_t>(slMarker(r)))
    ns.params[static_cast<size_t>(slMarker(r))] = static_cast<float>(static_cast<double>(start) / static_cast<double>(std::max<int64_t>(1, frames)));
  setPluginState(a, t, ns, "move region start");
}

int markerParamAt(const PluginState& st, double pos) {
  for (int m = 0; m < kMaxSlices; ++m)
    if (std::fabs(ed::val(st, slMarker(m), -1) - pos) < 1e-6) return m;
  return -1;
}
// The marker parameter nearest to a position (fractions are stored, starts are frames).
int nearestMarkerParam(const PluginState& st, double pos) {
  int mp = markerParamAt(st, pos);
  double best = 1e9;
  for (int q = 0; mp < 0 && q < kMaxSlices; ++q) {
    const double v = ed::val(st, slMarker(q), -1);
    if (v >= 0 && std::fabs(v - pos) < best) best = std::fabs(v - pos), mp = q;
  }
  return mp;
}

// J-176: a part of the form that does nothing in the current engine: dimmed
// (its controls are disabled by the caller with ImGui::BeginDisabled).
void dimArea(const iskin::Skin& sk, float x, float y, float w, float h, uint32_t under) {
  sk.dl()->AddRectFilled(sk.at(x, y), sk.at(x + w, y + h), rgb(under, 0.62f));  // `under`: the colour beneath
}

// J-181: a raised panel with a soft top light and a hairline (the region panel's columns).
void panelWell(const iskin::Skin& sk, float x, float y, float w, float h, uint32_t fill, float lift = 0.06f) {
  ImDrawList* dl = sk.dl();
  const ImVec2 p = sk.at(x, y), q = sk.at(x + w, y + h);
  iskin::bevel(sk, x, y, w, h, rgb(fill), lerp(rgb(fill), IM_COL32_BLACK, 0.28f), 4);
  dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, p.y + sk.px(14)), theme::withAlpha(IM_COL32_WHITE, lift),
                              theme::withAlpha(IM_COL32_WHITE, lift), theme::withAlpha(IM_COL32_WHITE, 0), theme::withAlpha(IM_COL32_WHITE, 0));
}

// Small caption on the light region panel.
void capDark(const iskin::Skin& sk, float x, float y, float w, float h, const char* s, int align = 0) {
  iskin::text(sk, x, y, w, h, s, 7.5f, rgb(0x262C31), align, false, true);
}

// A dark display well (envelope / region / detection view, wave view): gradient ground, inner shadow.
void darkWell(const iskin::Skin& sk, float x, float y, float w, float h, float round = 3) {
  ImDrawList* dl = sk.dl();
  const ImVec2 p = sk.at(x, y), q = sk.at(x + w, y + h);
  iskin::bevel(sk, x, y, w, h, rgb(0x0C1116), rgb(0x05080B), round);
  dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), rgb(0x111921), rgb(0x111921), rgb(0x0A0F14), rgb(0x0A0F14));
  dl->AddLine(ImVec2(p.x + 1, p.y + 1.5f), ImVec2(q.x - 1, p.y + 1.5f), IM_COL32(0, 0, 0, 120));
  dl->AddLine(ImVec2(p.x + 2, q.y - 1.0f), ImVec2(q.x - 2, q.y - 1.0f), theme::withAlpha(IM_COL32_WHITE, 0.05f));
}

// Faint grid inside a screen rect (n columns × m rows).
void faintGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows, float alpha = 0.045f) {
  for (int i = 1; i < cols; ++i) {
    const float x = std::round(p0.x + (p1.x - p0.x) * static_cast<float>(i) / static_cast<float>(cols)) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), theme::withAlpha(IM_COL32_WHITE, alpha));
  }
  for (int i = 1; i < rows; ++i) {
    const float y = std::round(p0.y + (p1.y - p0.y) * static_cast<float>(i) / static_cast<float>(rows)) + 0.5f;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), theme::withAlpha(IM_COL32_WHITE, alpha));
  }
}

// Text in a screen rect clipped to it (mono or regular), vertically centred.
void clipText(ImDrawList* dl, ImFont* f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const std::string& s, int align = 0) {
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str());
  float x = align == 1 ? (p0.x + p1.x - ts.x) * 0.5f : align == 2 ? p1.x - ts.x : p0.x;
  x = std::max(x, p0.x);
  dl->PushClipRect(p0, p1, true);
  dl->AddText(f, size, ImVec2(std::round(x), std::round((p0.y + p1.y - ts.y) * 0.5f)), col, s.c_str());
  dl->PopClipRect();
}

std::string fmtTime(double s) {
  char b[32];
  if (s < 10) std::snprintf(b, sizeof b, "%.3f s", s);
  else std::snprintf(b, sizeof b, "%.2f s", s);
  return b;
}

// Layout (J-181): the reference form's bands, each drawn on its own form
// coordinates and placed under the bands shown before it.
//   master (0..99): sliders 22/46/71/95, X/Y pad 132 + wheels 215, slicing mode
//     424,9, toggles (345..565 × 33..85) with the options button 343,67, logo
//     589,12, keyboard button 574,61, display mode selector 604,61;
//   region panel (99..296): region column (10..168: menu, selector, key/cut
//     row 157, wheels 46/72 × 185..269, switches 121), filter column (175..331),
//     view column (338..705: target tabs 107, part tabs 123, view 139..259, row 263);
//   deck bar (296..362): loop 9, play 38, stop 67, menus 160..368, tools 427..683,
//     info row 345, view buttons 639..693;
//   wave view (362..571): flags 362..377, waveform, ruler, scroller (19);
//   keyboard (571..625): base note 3, keys 27..703.
enum Band { BMaster, BRegion, BDeck, BWave, BKeys, BCount };
constexpr float kBandY[BCount] = {0, 99, 296, 362, 571}, kBandH[BCount] = {99, 197, 66, 209, 54};

void editorSlicex(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  SlicerUi& ui = slicerUi[t.key()];
  const std::string file = slicex::sampleOf(st.data);  // J-41: region-model data starts with the sample
  SamplePtr smp = file.empty() ? nullptr : sampleCache().get(file);
  const RegionView rv = regionView(st, smp.get());
  const std::vector<int64_t> starts = rv.on ? rv.starts : sliceStarts(st, smp.get());
  const int nSl = static_cast<int>(starts.size());
  // Region engine: every region can be selected (the per-region parameters cover the first 64).
  ui.slice = std::clamp(ui.slice, 0, std::max(0, (rv.on ? nSl : std::min(nSl, kMaxSlices)) - 1));
  const int k = ui.slice;
  const bool hasParams = k < kMaxSlices;
  const int kp = std::min(k, kMaxSlices - 1);  // parameter slot of the selected region
  const int base = ed::ival(st, SlBaseNote, 60);
  const bool classic = !rv.on;
  const bool manual = classic && ed::ival(st, SlMode) == SlModeManual;
  if (ui.tab < 0 || (rv.on && ui.tab != 1)) ui.tab = rv.on ? 1 : 0;
  // Keys of every region (region engine: from the key map, one pass).
  std::vector<int> keyOf(static_cast<size_t>(nSl), -1);
  if (rv.on) {
    for (int kk = 127; kk >= 0; --kk)
      for (int e : rv.km[0][static_cast<size_t>(kk)])
        if (e >= 0 && e < nSl) keyOf[static_cast<size_t>(e)] = kk;
  } else {
    for (int s = 0; s < nSl; ++s) {
      const int kk = ed::ival(st, slParam2(s, SliceKey), -1);
      keyOf[static_cast<size_t>(s)] = kk >= 0 ? kk : base + s;
    }
  }
  auto sliceKey = [&](int s) { return s >= 0 && s < nSl ? keyOf[static_cast<size_t>(s)] : -1; };
  const double frames = smp ? static_cast<double>(smp->frames()) : 1.0;
  const double rate = smp && smp->sampleRate > 0 ? smp->sampleRate : 44100.0;
  // Region end (frames) for the shading.
  auto sliceEnd = [&](int s) {
    if (rv.on) return static_cast<double>(rv.ends[static_cast<size_t>(s)]);
    double s1 = s + 1 < nSl ? static_cast<double>(starts[static_cast<size_t>(s + 1)]) : frames;
    float e = ed::val(st, slParam2(s, SliceEnd), -1);
    if (e >= 0) s1 = std::min(s1, static_cast<double>(e) * frames);
    return s1;
  };
  auto keyName = [&](int s) {
    const int kk = sliceKey(s);
    return kk >= 0 ? ed::noteName(kk) : std::string("-");
  };
  auto regionName = [&](int s) {
    return rv.on && static_cast<size_t>(s) < rv.md.decks[0].regions.size() ? rv.md.decks[0].regions[static_cast<size_t>(s)].name : std::string();
  };
  auto regionLabel = [&](int s) {  // "12  Kick  ·  D5"
    std::string l = std::to_string(s + 1);
    const std::string nm = regionName(s);
    if (!nm.empty()) l += "  " + nm;
    return l + "  \xC2\xB7  " + keyName(s);
  };
  auto audition = [&](int s) {
    const int key = sliceKey(s);
    if (t.isChannel() && key >= 0) a.engine.noteOn(t.owner, key, 0.9f), a.engine.noteOff(t.owner, key);
  };
  // ---- the wave view: target (goFrom..goTo) and the shown view gliding to it.
  auto sane = [](double& f, double& to) {
    if (!(to - f > 1e-9) || f < 0 || to > 1) f = 0, to = 1;
  };
  sane(ui.viewFrom, ui.viewTo), sane(ui.goFrom, ui.goTo);
  {
    const double e = 1.0 - std::exp(-static_cast<double>(ImGui::GetIO().DeltaTime) * 22.0);
    ui.viewFrom += (ui.goFrom - ui.viewFrom) * e, ui.viewTo += (ui.goTo - ui.viewTo) * e;
    if (std::fabs(ui.viewFrom - ui.goFrom) + std::fabs(ui.viewTo - ui.goTo) < 1e-7 * (ui.goTo - ui.goFrom) + 1e-12) ui.viewFrom = ui.goFrom, ui.viewTo = ui.goTo;
  }
  auto setView = [&](double from, double to) {
    const double minW = smp && smp->frames() > 64 ? 32.0 / frames : 0.001;
    double w = std::clamp(to - from, minW, 1.0);
    from = std::clamp(from, 0.0, 1.0 - w);
    ui.goFrom = from, ui.goTo = from + w;
  };
  auto jumpView = [&](double from, double to) {
    setView(from, to);
    ui.viewFrom = ui.goFrom, ui.viewTo = ui.goTo;
  };
  auto zoom = [&](double factor, double around) {  // around: fraction of the sample kept in place
    const double w = (ui.goTo - ui.goFrom) * factor;
    const double rel = (around - ui.goFrom) / std::max(1e-12, ui.goTo - ui.goFrom);
    setView(around - rel * w, around - rel * w + w);
  };
  auto regionSpan = [&](int s) {
    return std::make_pair(static_cast<double>(starts[static_cast<size_t>(s)]) / frames, sliceEnd(s) / frames);
  };
  auto showRegion = [&](int s) {  // scrolls the region into view (zoom kept unless it does not fit)
    if (s < 0 || s >= nSl || !smp) return;
    const auto [a0, a1] = regionSpan(s);
    if (a0 >= ui.goFrom && a1 <= ui.goTo) return;
    const double w = std::max(ui.goTo - ui.goFrom, (a1 - a0) * 1.2);
    setView((a0 + a1) * 0.5 - w * 0.5, (a0 + a1) * 0.5 + w * 0.5);
  };
  auto fitRegion = [&](int s) {
    if (s < 0 || s >= nSl || !smp) return;
    const auto [a0, a1] = regionSpan(s);
    setView(a0 - (a1 - a0) * 0.15, a1 + (a1 - a0) * 0.15);
  };
  auto selectRegion = [&](int s) {
    ui.slice = std::clamp(s, 0, std::max(0, nSl - 1));
    showRegion(ui.slice);
  };
  // ---- actions shared by buttons and menus.
  auto loadLoop = [&]() {
    PluginTarget tt = t;
    a.fileDialog = {};
    a.fileDialog.open = true;
    a.fileDialog.title = "Load loop";
    const char* h = gl::platform::homeEnv();
    a.fileDialog.dir = file.empty() ? (h ? h : ".") : fs::path(file).parent_path().string();
    a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv"};
    a.fileDialog.onOk = [&a, tt](const std::string& p) {
      if (const PluginState* cur = tt.state(a.doc.song())) {
        PluginState ns = *cur;
        ns.data = p;
        setPluginState(a, tt, ns, "load loop");
      }
    };
  };
  auto setMode = [&](int mode) { a.doc.setParam(t.pid(SlMode), mode, false); };
  auto clearMarkers = [&]() {
    PluginState ns = st;
    for (int m = 0; m < kMaxSlices; ++m) ns.params[static_cast<size_t>(slMarker(m))] = m == 0 ? 0.f : -1.f;
    setPluginState(a, t, ns, "clear slice markers");
  };
  auto addMarkerAt = [&](double frac) {  // classic engine: a manual marker at a position
    if (!smp) return;
    if (!manual) makeManual(a, t, st, starts, smp.get());
    const PluginState* cur = t.state(a.doc.song());
    for (int q = 0; cur && q < kMaxSlices; ++q)
      if (ed::val(*cur, slMarker(q), -1) < 0) {
        a.doc.setParam(t.pid(slMarker(q)), static_cast<float>(frac), false);
        break;
      }
  };
  auto resetKeys = [&]() {  // every region back on its automatic key (base note upwards)
    PluginState ns = st;
    for (int s = 0; s < kMaxSlices; ++s) ns.params[static_cast<size_t>(slParam2(s, SliceKey))] = -1.f;
    setPluginState(a, t, ns, "map regions from the base note");
  };
  auto resetRegion = [&](int s, bool all) {  // per-region settings back to their defaults
    const PluginInfo* info = findPlugin("gl.slicer");
    if (!info) return;
    PluginState ns = st;
    for (int q = all ? 0 : s; q <= (all ? kMaxSlices - 1 : s); ++q) {
      for (int f = 0; f < SliceFields; ++f)
        ns.params[static_cast<size_t>(slParam(q, f))] = static_cast<float>(info->params[static_cast<size_t>(slParam(q, f))].def);
      for (int f = 0; f < SliceFields2; ++f)
        ns.params[static_cast<size_t>(slParam2(q, f))] = static_cast<float>(info->params[static_cast<size_t>(slParam2(q, f))].def);
    }
    setPluginState(a, t, ns, all ? "reset every region" : "reset region");
  };
  auto assignKey = [&](int s, int key) {
    if (s < 0 || s >= kMaxSlices || key < 0 || key > 127) return;
    a.doc.setParam(t.pid(slParam2(s, SliceKey)), key, false);
  };
  // Region s in song ticks (start, length): at the loop's own speed, or stretched to its beats (Fit tempo).
  auto regionTicks = [&](int s) {
    const Song& song = a.doc.song();
    const double s0 = static_cast<double>(starts[static_cast<size_t>(s)]), s1 = sliceEnd(s);
    const double beatsPerFrame = classic && ed::bval(st, SlFitTempo) ? ed::val(st, SlLoopBeats, 4) / frames : song.tempo / 60.0 / rate;
    const double ppq = static_cast<double>(song.ppq);
    return std::make_pair(static_cast<Tick>(std::llround(s0 * beatsPerFrame * ppq)), static_cast<Tick>(std::max(1.0, std::round((s1 - s0) * beatsPerFrame * ppq))));
  };
  auto dumpScore = [&]() {  // every region as a note at its place in the loop, into the current pattern
    if (!t.isChannel() || nSl == 0) return;
    if (a.curPattern == kNoId) {
      a.notify(i18n::tr("Select a pattern first"), true);
      return;
    }
    std::vector<Note> notes;
    for (int s = 0; s < nSl; ++s) {
      const int key = sliceKey(s);
      if (key < 0 || key > 127) continue;
      Note n;
      std::tie(n.start, n.length) = regionTicks(s);
      n.pitch = key;
      notes.push_back(n);
    }
    a.doc.setNotes(a.curPattern, t.owner, notes, "dump regions to the piano roll");
    a.notify(i18n::tr("Regions written to the piano roll"));
  };
  const double loopBeats = ed::val(st, SlLoopBeats, 4);
  // Tempo of the loop: at its length in beats (Fit tempo), else the power-of-two
  // beat count that lands between 80 and 160 BPM.
  double tempoEst = 0;
  if (smp && smp->seconds() > 0) {
    double beats = loopBeats;
    if (!(classic && ed::bval(st, SlFitTempo))) {
      beats = 1;
      while (beats * 60.0 / smp->seconds() < 80 && beats < 256) beats *= 2;
    }
    tempoEst = beats * 60.0 / smp->seconds();
  }

  // ---- bands shown and the form height; scaled down to fit a short window (J-181).
  const bool shown[BCount] = {true, ui.display == 0 || ui.display == 1, ui.display == 0 || ui.display == 2, ui.display == 0 || ui.display == 2, ui.keys};
  float placed[BCount] = {}, formH = 0;
  for (int b = 0; b < BCount; ++b) placed[b] = formH, formH += shown[b] ? kBandH[b] : 0;
  float w = width;
  if (!browserui::thumbMode()) {
    const float avail = ui::EditorArea().y - ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y - 2;
    if (formH * width / kSxW > avail && avail > 0) {
      w = std::max(width * 0.62f, std::floor(avail * kSxW / formH));
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + std::floor((width - w) * 0.5f), ImGui::GetCursorScreenPos().y));
    }
  }
  const iskin::Skin root = iskin::begin(w, kSxW, formH, slicexPalette());
  auto bandSkin = [&](int b) {
    iskin::Skin s = root;
    s.o.y = root.o.y + (placed[b] - kBandY[b]) * root.s;
    return s;
  };
  ImDrawList* dl = root.dl();
  // Form ground and the accent strip.
  iskin::background(root, 0, 0, kSxW, formH, rgb(0x1A2633), rgb(0x0F171F), 4);
  {
    const ImVec2 p = root.at(0, 0), q = root.at(kSxW, 4);
    dl->AddRectFilledMultiColor(p, q, rgb(0xFF8A10), rgb(kOrange), rgb(kOrange), rgb(0xFF8A10));
    dl->AddLine(ImVec2(p.x, q.y), ImVec2(q.x, q.y), IM_COL32(0, 0, 0, 120));
  }
  static const bool trace = std::getenv("GRIDLINE_KERF_TRACE") != nullptr;
  std::string traceLine;
  auto traceRect = [&](const char* name, ImVec2 p, ImVec2 q) {
    if (!trace) return;
    char b[96];
    std::snprintf(b, sizeof b, " %s=%.0f,%.0f,%.0f,%.0f", name, static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(q.x),
                  static_cast<double>(q.y));
    traceLine += b;
  };

  // ======================================================================= master band
  {
    const iskin::Skin sk = bandSkin(BMaster);
    struct Sl {
      int param;
      float x;
      const char* cap;
      bool centered, classicOnly;
      const char* hint;
    };
    const Sl sliders[4] = {{rv.on ? SlMaster : SlVolume, 22, "LEVEL", false, false, "Master level"},
                           {SlFine, 46, "FINE", true, true, "Fine tune (cents)"},
                           {SlPan, 71, "PAN", true, true, "Master pan"},
                           {SlPitch, 95, "PITCH", true, false, "Master pitch (semitones)"}};
    for (const Sl& s : sliders) {
      ImGui::BeginDisabled(rv.on && s.classicOnly);
      iskin::slider(a, t, sk, s.param, s.x, 7, 20, 73, sk.c.fill, s.centered);
      ImGui::EndDisabled();
      iskin::text(sk, s.x - 6, 82, 32, 10, s.cap, 7.5f, rv.on && s.classicOnly ? rgb(0x56626C) : sk.c.label, 1, false, true);
    }
    {  // X/Y modulation pad
      darkWell(sk, 132, 9, 77, 77, 3);
      const ImVec2 p0 = sk.at(132, 9), p1 = sk.at(209, 86);
      faintGrid(dl, p0, p1, 4, 4, 0.06f);
      const float mx = ed::val(st, SlModX, 0.5f), my = ed::val(st, SlModY, 0.5f);
      const ImVec2 c(p0.x + (p1.x - p0.x) * mx, p1.y - (p1.y - p0.y) * my);
      dl->PushClipRect(p0, p1, true);
      dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), rgb(kOrange, 0.35f));
      dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), rgb(kOrange, 0.35f));
      dl->AddCircleFilled(c, sk.px(9), rgb(kOrange, 0.10f), 24);
      dl->AddCircleFilled(c, sk.px(5.5f), rgb(kOrange, 0.22f), 24);
      dl->AddCircleFilled(c, sk.px(3.2f), rgb(0xFFD38A), 20);
      dl->PopClipRect();
      ImGui::SetCursorScreenPos(p0);
      ImGui::InvisibleButton("##xy", ImVec2(p1.x - p0.x, p1.y - p0.y));
      if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        a.doc.setParam(t.pid(SlModX), std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f), true);
        a.doc.setParam(t.pid(SlModY), std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.0f, 1.0f), true);
      }
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Modulation X (filter cutoff) and Y (resonance): drag"));
      iskin::wheel(a, t, sk, SlModX, 215, 33, 24, rgb(0x959EA6));
      iskin::text(sk, 241, 37, 17, 15, "X", 11, rgb(0x959EA6), 0, false, true);
      iskin::wheel(a, t, sk, SlModY, 215, 62, 24, rgb(0x959EA6));
      iskin::text(sk, 241, 67, 17, 15, "Y", 11, rgb(0x959EA6), 0, false, true);
    }
    // Slicing mode (classic engine); the region engine plays the loop's own regions.
    iskin::text(sk, 345, 11, 76, 15, "SLICING", 8.5f, sk.c.label, 2, false, true);
    if (rv.on) {
      iskin::lcd(sk, 424, 9, 132, 19, "Regions of the loop", sk.c.lcdBg, sk.c.lcdText, 9);
      if (ImGui::IsMouseHoveringRect(sk.at(424, 9), sk.at(556, 28))) a.setHint(i18n::tr("The loop brought its own regions: they play as they are stored"));
    } else {
      iskin::digi(a, t, sk, SlMode, 424, 9, 132, 19, {}, 10);
    }
    // Playback toggles: 33 / 51 / 69, options button in front of the last row.
    ImGui::BeginDisabled(rv.on);
    iskin::check(a, t, sk, SlDeclick, 345, 33, 100, 16, "DECLICK", 8.5f);
    iskin::check(a, t, sk, SlReverse, 477, 33, 85, 16, "REVERSE", 8.5f);
    iskin::check(a, t, sk, SlMono, 345, 51, 62, 16, "MONO", 8.5f);
    iskin::check(a, t, sk, SlHold, 411, 51, 62, 16, "HOLD", 8.5f);
    iskin::check(a, t, sk, SlPlayToEnd, 477, 51, 88, 16, "PLAY TO END", 8.5f);
    iskin::check(a, t, sk, SlFitTempo, 367, 69, 100, 16, "FIT TEMPO", 8.5f);
    iskin::digi(a, t, sk, SlLoopBeats, 481, 69, 76, 16, [](double v) {
      char b[24];
      std::snprintf(b, sizeof b, "%.0f beats", v);
      return std::string(b);
    }, 8.5f);
    ImGui::EndDisabled();
    if (rv.on) dimArea(sk, 365, 31, 200, 56, 0x15202A);
    // Options menu (macros and settings).
    if (kerf::deckButton(a, sk, "##options", 343, 67, 20, 20, ui::Icon::ChevronDown, "Options: key macros and playback settings")) ImGui::OpenPopup("##kerfoptions");
    if (ImGui::BeginPopup("##kerfoptions")) {
      ImGui::TextDisabled("%s", i18n::tr("Macros"));
      if (ImGui::MenuItem(i18n::tr("Map regions upwards from the base note"), nullptr, false, classic && nSl > 0)) resetKeys();
      if (ImGui::MenuItem(i18n::tr("Reset every region's settings"), nullptr, false, classic && nSl > 0)) resetRegion(0, true);
      if (ImGui::MenuItem(i18n::tr("Write the regions to the piano roll"), nullptr, false, nSl > 0 && t.isChannel())) dumpScore();
      ImGui::Separator();
      ImGui::TextDisabled("%s", i18n::tr("Settings"));
      auto tog = [&](const char* label, int param) {
        const bool on = ed::bval(st, param);
        if (ImGui::MenuItem(i18n::tr(label), nullptr, on, classic)) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
      };
      tog("Monophonic", SlMono);
      tog("Declick region edges", SlDeclick);
      tog("Hold while the key is down", SlHold);
      tog("Play to the end of the loop", SlPlayToEnd);
      tog("Reverse every region", SlReverse);
      tog("Fit the loop to the tempo", SlFitTempo);
      ImGui::EndPopup();
    }
    if (!pui::titleWordmarkIn(sk.dl(), "gl.slicer", sk.at(589, 12), sk.at(589 + 118, 12 + 40), sk.px(26), 2, sk.c.text))  // J-136: the wordmark
      iskin::text(sk, 589, 12, 118, 40, pluginTitle("gl.slicer").c_str(), 26, rgb(kOrangeDeep), 2, false, true);
    // Keyboard toggle and display mode.
    if (kerf::deckButton(a, sk, "##kbbtn", 574, 61, 25, 30, ui::Icon::Piano, "Show or hide the keyboard", ui.keys ? 2 : 0)) ui.keys = !ui.keys;
    {
      const char* hints[4] = {"Display: everything", "Display: no wave editor", "Display: no region panel", "Display: compact (master band only)"};
      iskin::bevel(sk, 604, 61, 108, 30, rgb(0x0D1318), rgb(0x05080B), 3);
      for (int m = 0; m < 4; ++m) {
        const float x = 606 + m * 26.0f;
        const ImVec2 p = sk.at(x, 63), q = sk.at(x + 24, 89);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(m);
        if (ImGui::InvisibleButton("##disp", ImVec2(q.x - p.x, q.y - p.y))) ui.display = m;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) a.setHint(i18n::tr(hints[m]));
        const bool on = ui.display == m;
        if (on) dl->AddRectFilled(p, q, rgb(kOrange, 0.16f), sk.px(2));
        else if (hov) dl->AddRectFilled(p, q, theme::withAlpha(IM_COL32_WHITE, 0.05f), sk.px(2));
        kerf::bandGlyph(dl, sk.at(x + 6, 67), sk.at(x + 18, 85), m, on ? rgb(kOrange) : hov ? rgb(0xC8D0D7) : rgb(0x7C8892), rgb(0x46515A));
      }
      traceRect("display", sk.at(604, 61), sk.at(712, 91));
    }
  }

  // ======================================================================= region panel
  if (shown[BRegion]) {
    const iskin::Skin sk = bandSkin(BRegion);
    {
      const ImVec2 p = sk.at(0, 99), q = sk.at(kSxW, 296);
      iskin::bevel(sk, 0, 99, kSxW, 197, rgb(0x5F676C), 0, 0);
      dl->AddRectFilledMultiColor(p, q, rgb(0x687075), rgb(0x687075), rgb(0x585F64), rgb(0x585F64));
      dl->AddLine(p, ImVec2(q.x, p.y), theme::withAlpha(IM_COL32_WHITE, 0.16f));
      dl->AddLine(ImVec2(p.x, q.y - 1), ImVec2(q.x, q.y - 1), IM_COL32(0, 0, 0, 90));
    }
    // ---- left column: region menu, selector, key / cut, wheels.
    panelWell(sk, 10, 106, 158, 189, 0x5A6166);
    if (!smp) {  // nothing loaded: the wordmark and a load button (click anywhere in the column)
      const ImVec2 p = sk.at(11, 107), q = sk.at(167, 294);
      dl->AddRectFilledMultiColor(p, q, rgb(0x4C5358), rgb(0x4C5358), rgb(0x40464B), rgb(0x40464B));
      if (!pui::titleWordmarkIn(dl, "gl.slicer", sk.at(24, 150), sk.at(154, 190), sk.px(30), 1, rgb(0xDCE2E7)))
        iskin::text(sk, 24, 150, 130, 40, pluginTitle("gl.slicer").c_str(), 26, rgb(0xDCE2E7), 1, false, true);
      iskin::text(sk, 16, 196, 146, 14, "No loop loaded", 8.5f, rgb(0xB5BDC4), 1, false, false);
      if (kerf::deckButton(a, sk, "##loadbig", 39, 222, 100, 26, ui::Icon::Open, "Load a loop", false, false, i18n::tr("Load a loop"))) loadLoop();
      traceRect("loadbig", sk.at(39, 222), sk.at(139, 248));
    } else {
      if (kerf::deckButton(a, sk, "##rmenu", 13, 110, 18, 19, ui::Icon::Menu, "Region menu")) ImGui::OpenPopup("##regionmenu");
      if (ImGui::BeginPopup("##regionmenu")) {
        if (ImGui::MenuItem(i18n::tr("Previous region"), nullptr, false, k > 0)) selectRegion(k - 1);
        if (ImGui::MenuItem(i18n::tr("Next region"), nullptr, false, k + 1 < nSl)) selectRegion(k + 1);
        if (ImGui::MenuItem(i18n::tr("Play region"), nullptr, false, nSl > 0 && sliceKey(k) >= 0)) audition(k);
        if (ImGui::MenuItem(i18n::tr("Show region"), nullptr, false, nSl > 0)) fitRegion(k);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Reset this region's settings"), nullptr, false, classic && nSl > 0 && hasParams)) resetRegion(kp, false);
        if (ImGui::MenuItem(i18n::tr("Back to its automatic key"), nullptr, false, classic && nSl > 0 && hasParams)) a.doc.setParam(t.pid(slParam2(kp, SliceKey)), -1, false);
        ImGui::EndPopup();
      }
      iskin::text(sk, 36, 110, 128, 19, "REGION", 7.5f, rgb(0x262C31), 0, false, true);
      {
        char cnt[24];
        std::snprintf(cnt, sizeof cnt, "%d / %d", nSl > 0 ? k + 1 : 0, nSl);
        iskin::text(sk, 90, 110, 74, 19, cnt, 7.5f, rgb(0x30373C), 2, true, false);
      }
      const std::string cur = nSl > 0 ? regionLabel(k) : std::string("(no regions)");
      iskin::lcd(sk, 14, 131, 150, 19, "", sk.c.lcdBg, sk.c.lcdText, 9);
      clipText(dl, a.fontMono, std::max(8.0f, sk.px(9)), sk.at(19, 131), sk.at(150, 150), sk.c.lcdText, cur);
      iskin::glyphArrow(dl, sk.at(157, 140.5f), sk.px(3.5f), sk.c.lcdText, 3);
      ImGui::SetCursorScreenPos(sk.at(14, 131));
      ImGui::InvisibleButton("##rsel", ImVec2(sk.px(150), sk.px(19)));
      if (ImGui::IsItemClicked() && nSl > 0) ImGui::OpenPopup("##regionlist");
      if (ImGui::IsItemHovered()) {
        a.setHint(i18n::tr("Current region: click for the list, mouse wheel to step"));
        const float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0 && nSl > 0) selectRegion(k + (wh > 0 ? -1 : 1));
      }
      ImGui::SetNextWindowPos(sk.at(14, 150));
      if (ImGui::BeginPopup("##regionlist")) {
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##rlist", ImVec2(std::max(sk.px(220), 220.0f), std::min(16, std::max(1, nSl)) * rowH + 4), ImGuiChildFlags_None);
        ImGuiListClipper clip;
        clip.Begin(nSl, rowH);
        if (ImGui::IsWindowAppearing()) clip.IncludeItemByIndex(k);
        while (clip.Step())
          for (int s = clip.DisplayStart; s < clip.DisplayEnd; ++s) {
            ImGui::PushID(s);
            if (ImGui::Selectable(regionLabel(s).c_str(), s == k)) {
              selectRegion(s);
              ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsWindowAppearing() && s == k) ImGui::SetScrollHereY();
            ImGui::PopID();
          }
        ImGui::EndChild();
        ImGui::EndPopup();
      }
      // Key and cut group (row 157), region wheels (rows 185 / 213 / 241 / 269), switches beside them.
      auto rw = [&](int param, float x, float y, const char* cap, bool capLeft) {
        iskin::wheel(a, t, sk, param, x, y, 23, sk.c.fill);
        if (capLeft) capDark(sk, 12, y, x - 14, 23, cap, 2);
        else capDark(sk, x + 25, y, 26, 23, cap, 0);
      };
      if (hasParams || nSl == 0) {
        ImGui::BeginDisabled(nSl == 0);
        capDark(sk, 12, 157, 24, 23, "KEY", 2);
        iskin::digi(a, t, sk, slParam2(kp, SliceKey), 40, 158, 52, 21, [&](double v) {
          int n = static_cast<int>(std::lround(v));
          if (n < 0) return keyName(k) + "*";  // automatic: the key the region gets
          return ed::noteName(n);
        }, 9);
        capDark(sk, 96, 157, 23, 23, "CUT", 2);
        iskin::digi(a, t, sk, slParam2(kp, SliceCut), 121, 158, 27, 21, [](double v) {
          int n = static_cast<int>(std::lround(v));
          return n <= 0 ? std::string("-") : std::to_string(n);
        }, 9);
        rw(slParam(kp, SlicePan), 46, 185, "PAN", true);
        rw(slParam(kp, SliceGain), 72, 185, "VOL", false);
        rw(slParam(kp, SlicePitch), 72, 241, "PITCH", false);
        rw(slParam2(kp, SliceDelay), 46, 269, "DELAY", true);
        capDark(sk, 121, 232, 44, 9, "END");
        iskin::digi(a, t, sk, slParam2(kp, SliceEnd), 121, 243, 44, 18, [](double v) {
          if (v < 0) return std::string("next");
          char b[16];
          std::snprintf(b, sizeof b, "%.1f%%", v * 100);
          return std::string(b);
        }, 8);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(nSl == 0 || rv.on);  // classic engine only
        rw(slParam(kp, SliceCutoff), 46, 213, "CUT", true);
        rw(slParam2(kp, SliceReso), 72, 213, "RES", false);
        rw(slParam(kp, SliceStart), 72, 269, "START", false);
        iskin::check(a, t, sk, slParam(kp, SliceReverse), 121, 186, 46, 20, "REV", 8);
        iskin::check(a, t, sk, slParam(kp, SliceMute), 121, 208, 46, 20, "MUTE", 8);
        ImGui::EndDisabled();
        if (rv.on) {
          dimArea(sk, 12, 211, 100, 27, 0x5A6166);
          dimArea(sk, 70, 267, 52, 27, 0x5A6166);
          dimArea(sk, 119, 184, 48, 46, 0x5A6166);
        }
      } else {  // regions past the 64 slice parameters: their imported settings play as they are
        iskin::text(sk, 14, 175, 150, 14, "This region plays with", 8, rgb(0x262C31), 1, false, true);
        iskin::text(sk, 14, 189, 150, 14, "its imported settings.", 8, rgb(0x262C31), 1, false, true);
        iskin::text(sk, 14, 215, 150, 14, "Regions 1 to 64 can", 8, rgb(0x262C31), 1, false, true);
        iskin::text(sk, 14, 229, 150, 14, "be edited here.", 8, rgb(0x262C31), 1, false, true);
      }
    }
    // ---- filter column (classic engine): title, type, response, attack / cutoff / resonance.
    {
      panelWell(sk, 173, 106, 160, 189, 0x5A6166);
      ImGui::BeginDisabled(rv.on);
      iskin::bevel(sk, 177, 131, 152, 24, rgb(0x3B4247), rgb(0x2A3035), 3);
      iskin::text(sk, 177, 131, 152, 24, "FILTER", 10, rgb(0xC9D1D8), 1, false, true);
      capDark(sk, 180, 160, 60, 18, "TYPE");
      iskin::digi(a, t, sk, SlFilterType, 244, 160, 84, 18, {}, 9);
      // Response curve (vector sketch of the chosen type at the cutoff and resonance).
      darkWell(sk, 180, 183, 148, 44, 3);
      {
        const ImVec2 p0 = sk.at(182, 185), p1 = sk.at(326, 225);
        faintGrid(dl, p0, p1, 6, 2, 0.05f);
        const int ft = ed::ival(st, SlFilterType);
        const double fc = ed::val(st, SlCutoff, 20000), q = ed::val(st, SlReso, 0);
        const double lc = std::log(20.0), hc = std::log(20000.0);
        const double xc = (std::log(std::clamp(fc, 20.0, 20000.0)) - lc) / (hc - lc);
        std::vector<ImVec2> pts;
        for (int i = 0; i <= 72; ++i) {
          const double u = i / 72.0, r = std::exp((u - xc) * (hc - lc));  // f / fc
          double g = 1;
          const double peak = 1 + q * 3.0 / (1 + std::pow((r - 1) * 4.0, 2));
          if (ft == 1) g = 1 / std::sqrt(1 + std::pow(r, 4)) * peak;
          else if (ft == 2) g = 1 / std::sqrt(1 + std::pow(1 / std::max(1e-6, r), 4)) * peak;
          else if (ft == 3) g = 1 / std::sqrt(1 + std::pow((r - 1 / std::max(1e-6, r)) * (1.2 - q), 2)) * (1 + q);
          const double db = std::clamp(20 * std::log10(std::max(1e-6, g)), -30.0, 12.0);
          pts.push_back(ImVec2(p0.x + static_cast<float>(u) * (p1.x - p0.x), p0.y + static_cast<float>((12 - db) / 42.0) * (p1.y - p0.y)));
        }
        const ImU32 cc = ft == 0 ? rgb(0x6F7B85) : rgb(kOrange);
        for (size_t i = 1; i < pts.size(); ++i)
          dl->AddQuadFilled(ImVec2(pts[i - 1].x, p1.y), pts[i - 1], pts[i], ImVec2(pts[i].x, p1.y), theme::withAlpha(cc, 0.10f));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), cc, 0, std::max(1.2f, sk.px(1.4f)));
      }
      iskin::wheel(a, t, sk, SlAttack, 191, 236, 36, sk.c.fill2);
      iskin::wheel(a, t, sk, SlCutoff, 231, 231, 46, sk.c.fill2);
      iskin::wheel(a, t, sk, SlReso, 281, 236, 36, sk.c.fill2);
      ImGui::EndDisabled();
      capDark(sk, 185, 280, 48, 10, "ATTACK", 1);
      capDark(sk, 226, 280, 56, 10, "CUTOFF", 1);
      capDark(sk, 275, 280, 48, 10, "RES", 1);
      if (rv.on) dimArea(sk, 174, 107, 158, 187, 0x5A6166);
    }
    // ---- view column: target tabs, part tabs, the view and its row.
    {
      const float ex = 338, ew = 367;
      const ImU32 acc = rgb(kOrange);
      const char* targets[3] = {"AMP", "REGION", "DETECT"};
      const char* targetHints[3] = {"Amp envelope and velocity", "The selected region", "How the loop is sliced"};
      const bool targetOn[3] = {classic, true, classic};
      const int tc = kerf::tabStrip(a, sk, "##target", ex, 107, ew, 15, targets, 3, ui.tab, targetOn, targetHints, acc);
      if (tc >= 0) ui.tab = tc;
      traceRect("tab_amp", sk.at(ex, 107), sk.at(ex + ew / 3, 122));
      traceRect("tab_region", sk.at(ex + ew / 3, 107), sk.at(ex + ew * 2 / 3, 122));
      traceRect("tab_detect", sk.at(ex + ew * 2 / 3, 107), sk.at(ex + ew, 122));
      darkWell(sk, ex, 139, ew, 120, 3);
      const ImVec2 p0 = sk.at(ex + 5, 145), p1 = sk.at(ex + ew - 5, 253);
      if (ui.tab == 1) {  // ---- the selected region
        char info[128] = "-";
        if (nSl > 0 && smp) {
          const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]), s1 = sliceEnd(k);
          std::snprintf(info, sizeof info, "%d of %d  \xC2\xB7  from %s  \xC2\xB7  %s long  \xC2\xB7  key %s", k + 1, nSl, fmtTime(s0 / rate).c_str(),
                        fmtTime((s1 - s0) / rate).c_str(), keyName(k).c_str());
        }
        iskin::bevel(sk, ex, 123, ew, 15, rgb(0x262D33), rgb(0x161B20), 2);
        iskin::text(sk, ex + 6, 123, ew - 12, 15, info, 8, rgb(0xB9C3CC), 0, true, false);
        if (nSl > 0 && smp) {
          const auto [s0, s1] = regionSpan(k);
          kerf::WaveStyle ws;
          ws.peak = rgb(0xF0B060, 0.85f), ws.rms = rgb(0xFFD9A0), ws.edge = rgb(0xFFE2B8, 0.55f), ws.centre = theme::withAlpha(IM_COL32_WHITE, 0.08f);
          faintGrid(dl, p0, p1, 8, 2, 0.04f);
          kerf::drawWave(dl, smp.get(), p0, p1, s0 * frames, std::max(s0 * frames + 1, s1 * frames), ws);
          ImGui::SetCursorScreenPos(p0);
          ImGui::InvisibleButton("##rprev", ImVec2(p1.x - p0.x, p1.y - p0.y));
          if (ImGui::IsItemClicked()) audition(k);
          if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Click: play the region"));
        } else {
          iskin::text(sk, ex, 139, ew, 120, smp ? "No regions" : "Load a loop to see its regions", 9, rgb(0x6E7A85), 1, false, false);
        }
        // Row: step through the regions, the region's name.
        if (kerf::deckButton(a, sk, "##rpv", ex, 263, 24, 26, ui::Icon::ChevronLeft, "Previous region") && k > 0) selectRegion(k - 1);
        if (kerf::deckButton(a, sk, "##rnx", ex + 26, 263, 24, 26, ui::Icon::Right, "Next region") && k + 1 < nSl) selectRegion(k + 1);
        if (kerf::deckButton(a, sk, "##rplay", ex + 52, 263, 24, 26, ui::Icon::Play, "Play the region") && nSl > 0) audition(k);
        const std::string nm = nSl > 0 ? regionName(k) : std::string();
        iskin::text(sk, ex + 84, 263, ew - 88, 26, nm.empty() ? (nSl > 0 ? "(unnamed region)" : "") : nm.c_str(), 9, nm.empty() ? rgb(0xA9B2BA) : sk.c.text, 0, false, true);
      } else if (ui.tab == 0) {  // ---- amp: envelope or velocity
        const char* parts[2] = {"ENVELOPE", "VELOCITY"};
        const char* partHints[2] = {"Attack, release and the fades of every region", "How strongly velocity sets the level"};
        const int pc = kerf::tabStrip(a, sk, "##amppart", ex, 123, ew, 15, parts, 2, ui.ampPart, nullptr, partHints, acc);
        if (pc >= 0) ui.ampPart = pc;
        auto X = [&](double u) { return p0.x + static_cast<float>(u) * (p1.x - p0.x); };
        auto Y = [&](double v) { return p1.y - static_cast<float>(v) * (p1.y - p0.y); };
        faintGrid(dl, p0, p1, 8, 4, 0.04f);
        std::vector<ImVec2> pts;
        if (ui.ampPart == 0) {
          const double at = ed::val(st, SlAttack, 0.001f), re = ed::val(st, SlRelease, 0.01f), fi = ed::val(st, SlFadeIn), fo = ed::val(st, SlFadeOut);
          // Width shares: attack and release grow with their times (so short ones stay visible).
          const double fa = 0.06 + 0.22 * std::sqrt(std::clamp(at / 0.5, 0.0, 1.0));
          const double fr = 0.06 + 0.22 * std::sqrt(std::clamp(re / 1.0, 0.0, 1.0));
          const double fb = 1.0 - fa - fr;
          for (int i = 0; i <= 120; ++i) {
            const double u = i / 120.0;
            double v;
            if (u < fa) {
              v = u / fa;
            } else if (u < fa + fb) {
              const double rel = (u - fa) / fb;
              v = 1;
              if (fi > 0 && rel < fi) v *= rel / fi;
              if (fo > 0 && rel > 1 - fo) v *= (1 - rel) / fo;
            } else {
              v = fo > 0 ? 0.0 : 1.0 - (u - fa - fb) / fr;
            }
            pts.push_back(ImVec2(X(u), Y(std::clamp(v, 0.0, 1.0) * 0.92)));
          }
          kerf::dashedLine(dl, ImVec2(X(fa), p0.y), ImVec2(X(fa), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.16f), 3, 3);
          kerf::dashedLine(dl, ImVec2(X(fa + fb), p0.y), ImVec2(X(fa + fb), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.16f), 3, 3);
          iskin::text(sk, ex + 8, 143, 60, 10, "ATTACK", 6.5f, rgb(0x6F7B85), 0, false, true);
          iskin::text(sk, ex + ew - 68, 143, 60, 10, "RELEASE", 6.5f, rgb(0x6F7B85), 2, false, true);
          const struct {
            int p;
            const char* c;
          } ws[4] = {{SlRelease, "RELEASE"}, {SlFadeIn, "FADE IN"}, {SlFadeOut, "FADE OUT"}, {SlChromatic, "CHROMATIC"}};
          for (int i = 0; i < 4; ++i) {
            iskin::wheel(a, t, sk, ws[i].p, ex + 4 + i * 92, 265, 22, sk.c.fill);
            capDark(sk, ex + 29 + i * 92, 265, 62, 22, ws[i].c);
          }
        } else {
          const double amt = ed::val(st, SlVelocity, 1);
          for (int i = 0; i <= 60; ++i) {
            const double u = i / 60.0;
            pts.push_back(ImVec2(X(u), Y((1.0 - amt + amt * u) * 0.92)));
          }
          iskin::text(sk, ex + 8, 240, 120, 10, "SOFT", 6.5f, rgb(0x6F7B85), 0, false, true);
          iskin::text(sk, ex + ew - 128, 240, 120, 10, "HARD", 6.5f, rgb(0x6F7B85), 2, false, true);
          iskin::wheel(a, t, sk, SlVelocity, ex + 4, 265, 22, sk.c.fill);
          capDark(sk, ex + 29, 265, 120, 22, "VELOCITY TO LEVEL");
          iskin::wheel(a, t, sk, SlVolume, ex + 188, 265, 22, sk.c.fill);
          capDark(sk, ex + 213, 265, 80, 22, "LEVEL");
        }
        for (size_t i = 1; i < pts.size(); ++i) {
          const ImU32 tc0 = rgb(kOrange, 0.20f), tc1 = rgb(kOrange, 0.02f);
          dl->AddRectFilledMultiColor(ImVec2(pts[i - 1].x, std::min(pts[i - 1].y, pts[i].y)), ImVec2(pts[i].x, p1.y), tc0, tc0, tc1, tc1);
        }
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), rgb(kOrange), 0, std::max(1.4f, sk.px(1.8f)));
        traceRect("part_vel", sk.at(ex + ew / 2, 123), sk.at(ex + ew, 138));
      } else {  // ---- detection: methods, the loop with its cuts, sensitivity / gap
        const char* modes[6] = {"HITS", "4", "8", "16", "32", "HAND"};
        const char* modeHints[6] = {"Slice at the hits (transients)", "Four even slices", "Eight even slices", "Sixteen even slices",
                                    "Thirty-two even slices", "Slice at the markers set by hand"};
        const int mc = kerf::tabStrip(a, sk, "##detmode", ex, 123, ew, 15, modes, 6, ed::ival(st, SlMode), nullptr, modeHints, acc);
        if (mc >= 0) {
          if (mc == SlModeManual) makeManual(a, t, st, starts, smp.get());
          if (mc != SlModeManual || !smp) setMode(mc);
        }
        if (smp) {
          kerf::WaveStyle ws;
          ws.peak = rgb(0x5D7C99, 0.9f), ws.rms = rgb(0x86A9C8), ws.centre = theme::withAlpha(IM_COL32_WHITE, 0.06f);
          kerf::drawWave(dl, smp.get(), p0, p1, 0, frames, ws);
          for (int s = 0; s < nSl; ++s) {
            const float x = std::round(p0.x + static_cast<float>(static_cast<double>(starts[static_cast<size_t>(s)]) / frames) * (p1.x - p0.x)) + 0.5f;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), s == k ? rgb(kOrange) : rgb(kOrangeDeep, 0.6f));
          }
          char b[48];
          std::snprintf(b, sizeof b, "%d slices", nSl);
          iskin::text(sk, ex + ew - 108, 141, 100, 12, b, 7.5f, rgb(0xB9C3CC), 2, true, false);
        } else {
          iskin::text(sk, ex, 139, ew, 120, "Load a loop to see where it is cut", 9, rgb(0x6E7A85), 1, false, false);
        }
        const bool detect = ed::ival(st, SlMode) == 0;
        ImGui::BeginDisabled(!detect);
        iskin::wheel(a, t, sk, SlSensitivity, ex + 4, 265, 22, sk.c.fill);
        iskin::wheel(a, t, sk, SlMinGap, ex + 96, 265, 22, sk.c.fill);
        ImGui::EndDisabled();
        capDark(sk, ex + 29, 265, 62, 22, "SENSITIVITY");
        capDark(sk, ex + 121, 265, 62, 22, "MIN GAP");
        if (!detect) dimArea(sk, ex + 2, 263, 184, 26, 0x5F676C);
        if (kerf::deckButton(a, sk, "##detedit", ex + ew - 120, 263, 120, 26, ui::Icon::ToolDraw, "Edit the cuts by hand on the wave view", manual, false,
                             i18n::tr("Edit by hand")))
          makeManual(a, t, st, starts, smp.get());
      }
    }
  }

  // ======================================================================= deck bar
  if (shown[BDeck]) {
    const iskin::Skin sk = bandSkin(BDeck);
    {
      const ImVec2 p = sk.at(0, 296), q = sk.at(kSxW, 362);
      iskin::bevel(sk, 0, 296, kSxW, 66, rgb(0x293136), 0, 0);
      dl->AddRectFilledMultiColor(p, q, rgb(0x2E373D), rgb(0x2E373D), rgb(0x232A2F), rgb(0x232A2F));
      dl->AddLine(p, ImVec2(q.x, p.y), theme::withAlpha(IM_COL32_WHITE, 0.07f));
      dl->AddLine(sk.at(0, 337), sk.at(kSxW, 337), IM_COL32(0, 0, 0, 60));
    }
    // Transport: loop, play the selected region / stop. J-110: the button follows
    // what sounds: the engine's Stop / panic / focus loss (stop epoch) and the
    // region ending by itself (the channel falls silent).
    SlicerPlay& pl = slicerPlay();
    syncSlicerPlay(a);
    const bool playing = pl.latch.on && pl.ch == t.owner;
    auto stopPlay = [&]() {
      if (pl.key >= 0) a.engine.noteOff(pl.ch, pl.key);
      pl.latch.stop();
      pl.key = -1;
    };
    if (kerf::deckButton(a, sk, "##loop", 9, 300, 28, 30, ui::Icon::LoopArrows, "Loop: play the region again each time it ends", pl.loop)) pl.loop = !pl.loop;
    if (kerf::deckButton(a, sk, "##play", 38, 300, 28, 30, ui::Icon::Play, "Play the selected region / stop", playing) && t.isChannel() && nSl > 0 &&
        (playing || sliceKey(k) >= 0)) {
      if (playing) {
        stopPlay();
      } else {
        if (pl.latch.on) stopPlay();  // (another instance's region)
        pl.ch = t.owner;
        pl.key = sliceKey(k);
        a.engine.ownNoteOn(pl.ch, pl.key, 0.9f);  // J-112: the deck's own Play (plugin playback)
        pl.latch.start(a.engine);
      }
    }
    if (trace) {  // J-110 dev check: the play button's place and state (printed when it changes)
      static std::string last;
      const ImVec2 pc = sk.at(52, 315);
      char b[96];
      std::snprintf(b, sizeof b, "kerf: play %.0f %.0f on %d", static_cast<double>(pc.x), static_cast<double>(pc.y), pl.latch.on && pl.ch == t.owner ? 1 : 0);
      if (last != b) {
        last = b;
        std::printf("%s\n", b);
        std::fflush(stdout);
      }
    }
    if (kerf::deckButton(a, sk, "##stop", 67, 300, 28, 30, ui::Icon::Stop, "Stop") && playing && t.isChannel()) stopPlay();
    // Menus: file, sample | edit, tools, regions | view, snap, select, zoom.
    if (kerf::deckButton(a, sk, "##mfile", 160, 298, 24, 34, ui::Icon::Folder, "File", false, true)) ImGui::OpenPopup("##kfile");
    if (ImGui::BeginPopup("##kfile")) {
      if (ImGui::MenuItem(i18n::tr("Load a loop..."))) loadLoop();
      if (ImGui::MenuItem(i18n::tr("Remove the loop"), nullptr, false, !file.empty())) {
        PluginState ns = st;
        ns.data.clear();
        setPluginState(a, t, ns, "remove loop");
      }
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##msample", 184, 298, 24, 34, ui::Icon::Info, "Sample", false, true)) ImGui::OpenPopup("##ksample");
    if (ImGui::BeginPopup("##ksample")) {
      if (smp) {
        ImGui::TextDisabled("%s", fs::path(file).filename().string().c_str());
        ImGui::Text("%s: %.0f Hz", i18n::tr("Rate"), rate);
        ImGui::Text("%s: %s", i18n::tr("Channels"), smp->mono() ? i18n::tr("mono") : i18n::tr("stereo"));
        ImGui::Text("%s: %s", i18n::tr("Length"), fmtTime(smp->seconds()).c_str());
        ImGui::Text("%s: %.1f BPM (%.0f beats)", i18n::tr("Tempo"), tempoEst, loopBeats);
      } else {
        ImGui::TextDisabled("%s", i18n::tr("No loop loaded"));
      }
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##medit", 216, 298, 24, 34, ui::Icon::Undo, "Edit", false, true)) ImGui::OpenPopup("##kedit");
    if (ImGui::BeginPopup("##kedit")) {
      if (ImGui::MenuItem(i18n::tr("Undo"))) a.doc.undo();
      if (ImGui::MenuItem(i18n::tr("Redo"))) a.doc.redo();
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit the cuts by hand"), nullptr, manual, classic && smp)) makeManual(a, t, st, starts, smp.get());
      if (ImGui::MenuItem(i18n::tr("Clear the markers"), nullptr, false, manual)) clearMarkers();
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##mtools", 240, 298, 24, 34, ui::Icon::Settings, "Tools", false, true)) ImGui::OpenPopup("##ktools");
    if (ImGui::BeginPopup("##ktools")) {
      if (ImGui::MenuItem(i18n::tr("Write the regions to the piano roll"), nullptr, false, nSl > 0 && t.isChannel())) dumpScore();
      if (ImGui::MenuItem(i18n::tr("Map regions upwards from the base note"), nullptr, false, classic && nSl > 0)) resetKeys();
      if (ImGui::MenuItem(i18n::tr("Reset every region's settings"), nullptr, false, classic && nSl > 0)) resetRegion(0, true);
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##mregions", 264, 298, 24, 34, ui::Icon::ToolSlice, "Regions: slicing", false, true)) ImGui::OpenPopup("##regionsmenu");
    if (ImGui::BeginPopup("##regionsmenu")) {
      const int md = ed::ival(st, SlMode);
      if (ImGui::MenuItem(i18n::tr("Slice at the hits"), nullptr, classic && md == 0, classic)) setMode(0);
      if (ImGui::MenuItem(i18n::tr("Even slices: 4"), nullptr, classic && md == 1, classic)) setMode(1);
      if (ImGui::MenuItem(i18n::tr("Even slices: 8"), nullptr, classic && md == 2, classic)) setMode(2);
      if (ImGui::MenuItem(i18n::tr("Even slices: 16"), nullptr, classic && md == 3, classic)) setMode(3);
      if (ImGui::MenuItem(i18n::tr("Even slices: 32"), nullptr, classic && md == 4, classic)) setMode(4);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit markers by hand"), nullptr, manual, classic)) makeManual(a, t, st, starts, smp.get());
      if (ImGui::MenuItem(i18n::tr("Add a marker in the selected region"), nullptr, false, classic && nSl > 0 && smp)) {
        addMarkerAt((static_cast<double>(starts[static_cast<size_t>(k)]) + sliceEnd(k)) * 0.5 / frames);
      }
      if (ImGui::MenuItem(i18n::tr("Clear markers"), nullptr, false, manual)) clearMarkers();
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##mview", 296, 298, 24, 34, ui::Icon::Eye, "View", false, true)) ImGui::OpenPopup("##kview");
    if (ImGui::BeginPopup("##kview")) {
      const char* names[4] = {"Everything", "No wave editor", "No region panel", "Compact"};
      for (int m = 0; m < 4; ++m)
        if (ImGui::MenuItem(i18n::tr(names[m]), nullptr, ui.display == m)) ui.display = m;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Keyboard"), nullptr, ui.keys)) ui.keys = !ui.keys;
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##msnap", 320, 298, 24, 34, ui::Icon::Magnet, "Snap: where dragged markers land", ui.snap != 0 ? 2 : 0, true)) ImGui::OpenPopup("##ksnap");
    if (ImGui::BeginPopup("##ksnap")) {
      const char* names[3] = {"No snapping", "Zero crossings", "Sixteenths of the loop"};
      for (int m = 0; m < 3; ++m)
        if (ImGui::MenuItem(i18n::tr(names[m]), nullptr, ui.snap == m)) ui.snap = m;
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##mselect", 344, 298, 24, 34, ui::Icon::Select, "Select", false, true)) ImGui::OpenPopup("##kselect");
    if (ImGui::BeginPopup("##kselect")) {
      if (ImGui::MenuItem(i18n::tr("First region"), nullptr, false, nSl > 0)) selectRegion(0);
      if (ImGui::MenuItem(i18n::tr("Previous region"), nullptr, false, k > 0)) selectRegion(k - 1);
      if (ImGui::MenuItem(i18n::tr("Next region"), nullptr, false, k + 1 < nSl)) selectRegion(k + 1);
      if (ImGui::MenuItem(i18n::tr("Last region"), nullptr, false, nSl > 0)) selectRegion(nSl - 1);
      ImGui::EndPopup();
    }
    if (kerf::deckButton(a, sk, "##mzoom", 368, 298, 24, 34, ui::Icon::Zoom, "Zoom", false, true)) ImGui::OpenPopup("##viewmenu");
    if (ImGui::BeginPopup("##viewmenu")) {
      const double mid = (ui.goFrom + ui.goTo) * 0.5;
      if (ImGui::MenuItem(i18n::tr("Zoom in"))) zoom(0.5, mid);
      if (ImGui::MenuItem(i18n::tr("Zoom out"))) zoom(2.0, mid);
      if (ImGui::MenuItem(i18n::tr("Show the whole loop"))) setView(0, 1);
      if (ImGui::MenuItem(i18n::tr("Show the selected region"), nullptr, false, nSl > 0 && smp)) fitRegion(k);
      ImGui::EndPopup();
    }
    traceRect("menu_regions", sk.at(264, 298), sk.at(288, 332));
    // Tools: undo / redo | even slices, by hand | add, detect, write | clear, drag out.
    if (kerf::deckButton(a, sk, "##undo", 427, 298, 24, 34, ui::Icon::Undo, "Undo")) a.doc.undo();
    if (kerf::deckButton(a, sk, "##redo", 451, 298, 24, 34, ui::Icon::Redo, "Redo")) a.doc.redo();
    ImGui::BeginDisabled(rv.on);
    {
      const struct {
        const char* id;
        const char* cap;
        float x;
        int mode;
        const char* hint;
      } ev[3] = {{"##ev8", "8", 475, 2, "Eight even slices"}, {"##ev16", "16", 499, 3, "Sixteen even slices"}, {"##ev32", "32", 523, 4, "Thirty-two even slices"}};
      for (const auto& e : ev)
        if (kerf::deckButton(a, sk, e.id, e.x, 298, 24, 34, ui::Icon::Slice, e.hint, classic && ed::ival(st, SlMode) == e.mode, false, e.cap)) setMode(e.mode);
    }
    if (kerf::deckButton(a, sk, "##hand", 555, 298, 24, 34, ui::Icon::ToolDraw, "Edit the markers by hand (double-click the wave: add, right-click a marker: remove)", manual))
      makeManual(a, t, st, starts, smp.get());
    if (kerf::deckButton(a, sk, "##add", 579, 298, 24, 34, ui::Icon::Flag, "Add a marker in the middle of the selected region") && smp && nSl > 0)
      addMarkerAt((static_cast<double>(starts[static_cast<size_t>(k)]) + sliceEnd(k)) * 0.5 / frames);
    if (kerf::deckButton(a, sk, "##auto", 603, 298, 24, 34, ui::Icon::SliceWave, "Slice at the hits", classic && ed::ival(st, SlMode) == 0)) setMode(0);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(nSl == 0 || !t.isChannel());
    if (kerf::deckButton(a, sk, "##dump", 627, 298, 24, 34, ui::Icon::Piano, "Write the regions to the piano roll (current pattern)")) dumpScore();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!manual);
    if (kerf::deckButton(a, sk, "##clear", 659, 298, 24, 34, ui::Icon::Trash, "Clear the markers")) clearMarkers();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(file.empty());
    kerf::deckButton(a, sk, "##drag", 683, 298, 24, 34, ui::Icon::GripDots, "Drag the loop out (to the playlist, a channel or the browser)");
    if (!file.empty()) browserui::fileDragSource(file);
    ImGui::EndDisabled();
    traceRect("dump", sk.at(627, 298), sk.at(651, 332));
    // Lower row: rate, channels, length, tempo, name, selected region; view buttons.
    const ImU32 dim = rgb(0x8D99A3), lit = rgb(0xD8DFE5);
    ImFont* mono = a.fontMono;
    const float fs = std::max(8.0f, sk.px(8.5f));
    char info[96];
    std::snprintf(info, sizeof info, "%s", smp ? (std::to_string(static_cast<int>(rate)) + " Hz").c_str() : "-- Hz");
    clipText(dl, mono, fs, sk.at(6, 341), sk.at(68, 360), dim, info);
    clipText(dl, mono, fs, sk.at(70, 341), sk.at(116, 360), dim, smp ? (smp->mono() ? "mono" : "stereo") : "--");
    clipText(dl, mono, fs, sk.at(118, 341), sk.at(172, 360), dim, smp ? fmtTime(smp->seconds()) : "--");
    if (smp) std::snprintf(info, sizeof info, "%.1f BPM", tempoEst);
    else std::snprintf(info, sizeof info, "--");
    clipText(dl, mono, fs, sk.at(174, 341), sk.at(240, 360), dim, info);
    if (ImGui::IsMouseHoveringRect(sk.at(174, 341), sk.at(240, 360))) a.setHint(i18n::tr("Tempo of the loop at its length in beats (Fit tempo)"));
    const std::string nm = smp ? fs::path(file).filename().string() : std::string(i18n::tr("No loop: drop one on the wave view"));
    clipText(dl, a.fontMonoBold ? a.fontMonoBold : mono, fs, sk.at(244, 341), sk.at(450, 360), smp ? lit : dim, nm);
    if (nSl > 0 && smp) {
      const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]) / rate, s1 = sliceEnd(k) / rate;
      std::snprintf(info, sizeof info, "%d/%d  %.3f to %.3f", k + 1, nSl, s0, s1);
    } else {
      std::snprintf(info, sizeof info, "%d regions", nSl);
    }
    clipText(dl, mono, fs, sk.at(452, 341), sk.at(632, 360), rgb(kOrange, 0.9f), info, 2);
    const double mid = (ui.goFrom + ui.goTo) * 0.5;
    if (kerf::deckButton(a, sk, "##zout", 639, 339, 18, 21, ui::Icon::Minus, "Zoom out")) zoom(2.0, mid);
    if (kerf::deckButton(a, sk, "##zin", 657, 339, 18, 21, ui::Icon::Plus, "Zoom in")) zoom(0.5, mid);
    if (kerf::deckButton(a, sk, "##zall", 675, 339, 18, 21, ui::Icon::Zoom, "Show the whole loop")) setView(0, 1);
    if (kerf::deckButton(a, sk, "##zreg", 693, 339, 18, 21, ui::Icon::ScrollTo, "Show the selected region") && nSl > 0) fitRegion(k);
  }

  // ======================================================================= wave view
  if (shown[BWave]) {
    const iskin::Skin sk = bandSkin(BWave);
    const float wx = 0, wy = 362, ww = kSxW, wh = 209, flagH = 15, rulerH = 11, scrollH = 19;
    iskin::bevel(sk, wx, wy, ww, wh, rgb(0x0B1014), 0, 0);
    {
      const ImVec2 p = sk.at(wx, wy + flagH), q = sk.at(wx + ww, wy + wh - scrollH - rulerH);
      dl->AddRectFilledMultiColor(p, q, rgb(0x0F161D), rgb(0x0F161D), rgb(0x0A1015), rgb(0x0A1015));
    }
    iskin::bevel(sk, wx, wy, ww, flagH, rgb(0x161E26), 0, 0);
    sk.dl()->AddLine(sk.at(wx, wy + flagH), sk.at(wx + ww, wy + flagH), IM_COL32(0, 0, 0, 140));
    const ImVec2 top = sk.at(wx + 2, wy), p0 = sk.at(wx + 2, wy + flagH), p1 = sk.at(wx + ww - 2, wy + wh - scrollH - rulerH);
    const double f0 = ui.viewFrom * frames, f1 = ui.viewTo * frames;
    auto fx = [&](double f) { return p0.x + static_cast<float>((f - f0) / std::max(1e-9, f1 - f0)) * (p1.x - p0.x); };
    auto frameAt = [&](float x) { return f0 + std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * (f1 - f0); };
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    if (!smp) {  // ---- empty: the drop zone
      if (kerf::dropZone(a, sk, "##drop", 24, wy + flagH + 10, ww - 48, wh - flagH - scrollH - rulerH - 20, rgb(theme::role::kSlice))) loadLoop();
      traceRect("dropzone", sk.at(24, wy + flagH + 10), sk.at(ww - 24, wy + wh - scrollH - rulerH - 10));
    }
    const float labelPx = std::max(8.0f, sk.px(8.5f));
    // Dense markers (hundreds of regions at once): drawn faint so the audio shows,
    // and grabbed for moving only once zoomed in far enough.
    int visible = 0;
    for (int s = 0; s < nSl; ++s) visible += static_cast<double>(starts[static_cast<size_t>(s)]) >= f0 && static_cast<double>(starts[static_cast<size_t>(s)]) <= f1;
    const float spacing = (p1.x - p0.x) / static_cast<float>(std::max(1, visible));
    const float markerAlpha = std::clamp(spacing / sk.px(24), 0.10f, 0.7f);
    const bool grabMarkers = spacing >= sk.px(6);
    const bool hovWave = smp && ImGui::IsMouseHoveringRect(top, ImVec2(p1.x, p1.y)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // The region under the mouse (hover tint).
    auto regionAt = [&](double f) {
      int s = -1;
      if (rv.on) {  // regions may overlap or be out of order: the latest start whose region holds the point
        for (int q = 0; q < nSl; ++q)
          if (static_cast<double>(starts[static_cast<size_t>(q)]) <= f && f < sliceEnd(q) && (s < 0 || starts[static_cast<size_t>(q)] >= starts[static_cast<size_t>(s)]))
            s = q;
      } else if (nSl > 0) {
        s = 0;
        while (s + 1 < nSl && static_cast<double>(starts[static_cast<size_t>(s + 1)]) <= f) ++s;
      }
      return s;
    };
    const int hovRegion = hovWave && m.y >= p0.y ? regionAt(frameAt(m.x)) : -1;
    if (smp) {
      dl->PushClipRect(top, ImVec2(p1.x, p1.y), true);
      faintGrid(dl, p0, p1, 1, 4, 0.035f);
      for (int s = 0; s < nSl; ++s) {  // region shading (J-166: region engine: the region's own end)
        const double s0 = static_cast<double>(starts[static_cast<size_t>(s)]), s1 = sliceEnd(s);
        if (s1 < f0 || s0 > f1) continue;
        const bool muted = classic && s < kMaxSlices && ed::bval(st, slParam(s, SliceMute));
        const ImVec2 r0(fx(s0), p0.y), r1(fx(s1), p1.y);
        if (s == k) {
          dl->AddRectFilledMultiColor(r0, r1, rgb(kOrange, 0.16f), rgb(kOrange, 0.16f), rgb(kOrange, 0.06f), rgb(kOrange, 0.06f));
          dl->AddLine(ImVec2(r0.x, p0.y + 0.5f), ImVec2(r1.x, p0.y + 0.5f), rgb(kOrange, 0.8f), 2.0f);
        } else if (muted) {
          dl->AddRectFilled(r0, r1, rgb(0x3A1A1A, 0.55f));
        } else if (s % 2) {
          dl->AddRectFilled(r0, r1, theme::withAlpha(IM_COL32_WHITE, 0.018f));
        }
        if (s == hovRegion && s != k) dl->AddRectFilled(r0, r1, theme::withAlpha(IM_COL32_WHITE, 0.035f));
      }
      kerf::WaveStyle ws;
      const ImU32 blue = rgb(theme::role::kSlice);
      ws.peak = lerp(blue, rgb(0x0F161D), 0.28f), ws.rms = lerp(blue, IM_COL32_WHITE, 0.35f), ws.edge = theme::withAlpha(lerp(blue, IM_COL32_WHITE, 0.25f), 0.6f);
      ws.centre = theme::withAlpha(IM_COL32_WHITE, 0.07f);
      kerf::drawWave(dl, smp.get(), p0, p1, f0, f1, ws);
      if (nSl > 0) {  // the selected region's audio in the accent colour
        const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]), s1 = sliceEnd(k);
        if (s1 > f0 && s0 < f1) {
          dl->PushClipRect(ImVec2(std::max(p0.x, fx(s0)), p0.y), ImVec2(std::min(p1.x, fx(s1)), p1.y), true);
          kerf::WaveStyle wsel;
          wsel.peak = rgb(0xD99A4A, 0.85f), wsel.rms = rgb(0xFFD49A), wsel.edge = rgb(0xFFE0B0, 0.6f);
          kerf::drawWave(dl, smp.get(), p0, p1, f0, f1, wsel);
          dl->PopClipRect();
        }
      }
      // Flags: tabs with the number (and name) when they fit before the next region's flag.
      for (int s = 0; s < nSl; ++s) {
        const double s0 = static_cast<double>(starts[static_cast<size_t>(s)]);
        if (s0 > f1) continue;
        double next = f1;
        for (int q = s + 1; q < nSl && q < s + 8; ++q)
          if (static_cast<double>(starts[static_cast<size_t>(q)]) > s0) {
            next = std::min(next, static_cast<double>(starts[static_cast<size_t>(q)]));
            break;
          }
        if (next < f0) continue;
        const float x = std::round(fx(s0)), room = fx(next) - std::max(x, p0.x) - 4;
        std::string lbl = std::to_string(s + 1);
        const std::string rn = regionName(s);
        auto fits = [&](const std::string& tx) { return a.fontMono && a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, tx.c_str()).x + sk.px(8) <= room; };
        if (!rn.empty() && fits(lbl + " " + rn)) lbl += " " + rn;
        if (!fits(lbl)) lbl.clear();
        const bool sel = s == k, hv = s == hovRegion;
        const float tw = lbl.empty() ? std::clamp(spacing * 0.35f, 1.0f, sk.px(4))
                                     : a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, lbl.c_str()).x + sk.px(8);
        const ImVec2 t0(x, top.y + sk.px(2)), t1(x + tw, p0.y);
        const ImU32 fc = sel ? rgb(kOrange) : hv ? rgb(kOrangeDeep, 0.85f) : rgb(kOrangeDeep, lbl.empty() ? std::min(0.8f, markerAlpha * 1.6f) : 0.62f);
        dl->AddRectFilled(t0, t1, fc, sk.px(2.5f), ImDrawFlags_RoundCornersTopRight);
        if (!lbl.empty())
          dl->AddText(a.fontMono, labelPx, ImVec2(x + sk.px(4), top.y + sk.px(2) + (sk.px(flagH - 2) - labelPx) * 0.5f), sel ? rgb(0x1E160A) : rgb(0xF4E6D2), lbl.c_str());
      }
      dl->PopClipRect();
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##wave", ImVec2(p1.x - top.x, p1.y - top.y));
    const bool hov = ImGui::IsItemHovered() && smp;
    int hoverMarker = -1;
    const bool regionDrag = rv.on && ui.drag >= 0 && ImGui::IsItemActive();
    auto snapped = [&](double f) { return static_cast<double>(kerf::snapFrame(smp.get(), std::llround(f), ui.snap, loopBeats)); };
    if (smp) {
      dl->PushClipRect(top, ImVec2(p1.x, p1.y), true);
      for (int s = 0; s < nSl; ++s) {
        float x = std::round(fx(static_cast<double>(starts[static_cast<size_t>(s)]))) + 0.5f;
        if (regionDrag && s == ui.drag) x = fx(snapped(frameAt(m.x)));
        if (x < p0.x - 1 || x > p1.x + 1) continue;
        const bool near = grabMarkers && hov && std::fabs(m.x - x) < 4 && (s > 0 || rv.on);
        if (near) hoverMarker = s;
        if (s != k) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), near ? rgb(0xFFFFFF, 0.9f) : rgb(kOrangeDeep, markerAlpha), near ? 2.0f : 1.0f);
      }
      if (k < nSl) {  // the selected region's marker on top
        float x = std::round(fx(static_cast<double>(starts[static_cast<size_t>(k)]))) + 0.5f;
        if (regionDrag && k == ui.drag) x = fx(snapped(frameAt(m.x)));
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), rgb(kOrange), hoverMarker == k ? 2.0f : 1.4f);
      }
      if (hov && hoverMarker < 0 && m.y >= p0.y) dl->AddLine(ImVec2(m.x, p0.y), ImVec2(m.x, p1.y), theme::withAlpha(IM_COL32_WHITE, 0.18f));  // hover cursor
      dl->PopClipRect();
    }
    if (hov && io.MouseWheel != 0) {
      if (io.KeyShift) {  // Shift + wheel: scroll
        const double wv = ui.goTo - ui.goFrom;
        setView(ui.goFrom - io.MouseWheel * wv * 0.15, ui.goTo - io.MouseWheel * wv * 0.15);
      } else {
        zoom(io.MouseWheel > 0 ? 0.8 : 1.25, frameAt(m.x) / frames);
      }
    }
    if (hov && io.MouseWheelH != 0) {
      const double wv = ui.goTo - ui.goFrom;
      setView(ui.goFrom - io.MouseWheelH * wv * 0.15, ui.goTo - io.MouseWheelH * wv * 0.15);
    }
    if (ImGui::IsItemActivated() && smp) {
      ui.drag = hoverMarker;
      if (ui.drag < 0) {
        const int s = regionAt(frameAt(m.x));
        if (s >= 0 && s < nSl) {
          ui.slice = s;
          audition(s);
        }
      } else if (!manual && !rv.on) {
        makeManual(a, t, st, starts, smp.get());
      }
    }
    if (rv.on && ui.drag >= 0 && ImGui::IsItemDeactivated() && ui.drag < nSl) {
      // Released: the dragged region's start moves (one undo step); a click without moving leaves it.
      const int64_t to = std::llround(snapped(frameAt(m.x)));
      if (std::fabs(fx(static_cast<double>(to)) - fx(static_cast<double>(starts[static_cast<size_t>(ui.drag)]))) >= 2.0f)
        moveRegionStart(a, t, st, rv, ui.drag, to, smp.get());
      else
        ui.slice = ui.drag;
    }
    if (ui.drag > 0 && manual && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1) && ui.drag < nSl) {
      const int mp = nearestMarkerParam(st, static_cast<double>(starts[static_cast<size_t>(ui.drag)]) / frames);
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), static_cast<float>(snapped(frameAt(m.x)) / frames), true);
    }
    if (ImGui::IsItemDeactivated()) {
      ui.drag = -1;
      a.doc.endGesture();
    }
    if (hov && classic && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (!manual) makeManual(a, t, st, starts, smp.get());
      for (int q = 0; q < kMaxSlices; ++q)
        if (ed::val(st, slMarker(q), -1) < 0 || (!manual && q >= nSl)) {
          a.doc.setParam(t.pid(slMarker(q)), static_cast<float>(snapped(frameAt(m.x)) / frames), false);
          break;
        }
    }
    if (hoverMarker > 0 && manual && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const int mp = nearestMarkerParam(st, static_cast<double>(starts[static_cast<size_t>(hoverMarker)]) / frames);
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), -1, false);
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          PluginState ns = st;
          ns.data = path;
          setPluginState(a, t, ns, "load loop");
          ui.viewFrom = ui.goFrom = 0, ui.viewTo = ui.goTo = 1;
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (hov && rv.on)
      a.setHint(std::to_string(nSl) + " " + i18n::tr("regions \xC2\xB7 click: select + play \xC2\xB7 drag a marker: move the region's start \xC2\xB7 wheel: zoom, Shift+wheel: scroll \xC2\xB7 drop a loop"));
    else if (hov)
      a.setHint(std::to_string(nSl) + " " +
                i18n::tr("regions \xC2\xB7 click: select + play \xC2\xB7 drag a marker \xC2\xB7 double-click: add marker \xC2\xB7 right-click a marker: remove \xC2\xB7 wheel: zoom, Shift+wheel: scroll"));
    // Ruler: time ticks under the waveform.
    {
      const float ry = wy + wh - scrollH - rulerH;
      iskin::bevel(sk, wx, ry, ww, rulerH, rgb(0x121A21), 0, 0);
      dl->AddLine(sk.at(wx, ry), sk.at(wx + ww, ry), theme::withAlpha(IM_COL32_WHITE, 0.06f));
      if (smp) {
        const double t0 = f0 / rate, t1 = f1 / rate, span = std::max(1e-6, t1 - t0);
        const double steps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60};
        double step = steps[0];
        for (double s : steps)
          if (span / s * sk.px(46) <= (p1.x - p0.x)) {
            step = s;
            break;
          }
        const float fsz = std::max(8.0f, sk.px(7.5f));
        const ImVec2 r0 = sk.at(wx + 2, ry), r1 = sk.at(wx + ww - 2, ry + rulerH);
        dl->PushClipRect(r0, r1, true);
        for (double tt = std::ceil(t0 / step) * step; tt <= t1 + 1e-9; tt += step) {
          const float x = std::round(fx(tt * rate)) + 0.5f;
          dl->AddLine(ImVec2(x, r0.y), ImVec2(x, r0.y + sk.px(3)), rgb(0x6C7984));
          char b[24];
          if (step < 0.01) std::snprintf(b, sizeof b, "%.3f", tt);
          else if (step < 0.1) std::snprintf(b, sizeof b, "%.2f", tt);
          else if (step < 1) std::snprintf(b, sizeof b, "%.1f", tt);
          else std::snprintf(b, sizeof b, "%.0f s", tt);
          dl->AddText(a.fontMono, fsz, ImVec2(x + sk.px(2), r0.y + (r1.y - r0.y - fsz) * 0.5f + sk.px(1)), rgb(0x76838E), b);
        }
        dl->PopClipRect();
      }
    }
    // Time scroller: the whole loop with the visible part; drag to scroll.
    {
      const float sy = wy + wh - scrollH;
      iskin::bevel(sk, wx, sy, ww, scrollH, rgb(0x1B232A), 0, 0);
      dl->AddLine(sk.at(wx, sy), sk.at(wx + ww, sy), IM_COL32(0, 0, 0, 140));
      const ImVec2 q0 = sk.at(wx + 2, sy + 2), q1 = sk.at(wx + ww - 2, sy + scrollH - 2);
      if (smp) {
        kerf::WaveStyle ws;
        ws.peak = rgb(0x45607A), ws.rms = rgb(0x5E7F9C);
        kerf::drawWave(dl, smp.get(), q0, q1, 0, frames, ws);
        for (int s = 0; s < nSl; ++s) {
          const float x = std::round(q0.x + static_cast<float>(static_cast<double>(starts[static_cast<size_t>(s)]) / frames) * (q1.x - q0.x)) + 0.5f;
          dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q0.y + sk.px(4)), s == k ? rgb(kOrange) : rgb(kOrangeDeep, 0.6f));
        }
        const float v0 = q0.x + static_cast<float>(ui.viewFrom) * (q1.x - q0.x), v1 = q0.x + static_cast<float>(ui.viewTo) * (q1.x - q0.x);
        const bool full = ui.viewFrom <= 1e-6 && ui.viewTo >= 1 - 1e-6;
        dl->AddRectFilled(ImVec2(v0, q0.y), ImVec2(std::max(v1, v0 + 3), q1.y), rgb(0xB9BFC3, full ? 0.06f : 0.14f), sk.px(2));
        dl->AddRect(ImVec2(v0, q0.y), ImVec2(std::max(v1, v0 + 3), q1.y), rgb(0xB9BFC3, full ? 0.25f : 0.6f), sk.px(2));
      }
      ImGui::SetCursorScreenPos(q0);
      ImGui::InvisibleButton("##scroll", ImVec2(q1.x - q0.x, q1.y - q0.y));
      if (ImGui::IsItemActive() && smp) {
        const double wv = ui.goTo - ui.goFrom;
        const double c = (io.MousePos.x - q0.x) / std::max(1.0f, q1.x - q0.x);
        jumpView(c - wv * 0.5, c + wv * 0.5);
      }
      if (ImGui::IsItemHovered() && smp) {
        a.setHint(i18n::tr("Drag to scroll the wave view, wheel to zoom"));
        if (io.MouseWheel != 0) zoom(io.MouseWheel > 0 ? 0.8 : 1.25, (ui.goFrom + ui.goTo) * 0.5);
      }
    }
  }

  // ======================================================================= keyboard
  if (shown[BKeys]) {
    const iskin::Skin sk = bandSkin(BKeys);
    iskin::bevel(sk, 0, 571, kSxW, 54, rgb(0x12191E), 0, 0);
    sk.dl()->AddLine(sk.at(0, 571), sk.at(kSxW, 571), IM_COL32(0, 0, 0, 160));
    ImGui::BeginDisabled(rv.on);
    iskin::digi(a, t, sk, SlBaseNote, 3, 574, 22, 48, [](double v) { return ed::noteName(static_cast<int>(std::lround(v))); }, 7.5f);
    ImGui::EndDisabled();
    bool litKeys[128] = {};
    for (int s = 0; s < nSl; ++s) {
      int kk = sliceKey(s);
      if (kk >= 0 && kk < 128) litKeys[kk] = true;
    }
    iskin::KeyColors kc;
    kc.white = rgb(0xDCE1E5), kc.black = rgb(0x242C32), kc.pressed = rgb(kOrange), kc.mark = rgb(kOrangeDeep), kc.sep = rgb(0x0E1419);
    // The keyboard starts an octave under the selected region's key (or the base note)
    // (moved only when that key is out of sight; 35 white keys = 60 keys).
    const int focus = nSl > 0 && sliceKey(k) >= 0 ? sliceKey(k) : base;
    if (ui.kbLow < 0 || focus < ui.kbLow || focus >= ui.kbLow + 60) ui.kbLow = std::clamp((focus / 12) * 12 - 12, 0, 60);
    const int low = ui.kbLow;
    const int clicked = iskin::keyboard(a, t, sk, 27, 571, 676, 54, low, 35, kc, sliceKey(k), litKeys);
    const bool kbHov = ImGui::IsItemHovered();
    if (clicked >= 0)
      for (int s = 0; s < nSl; ++s)
        if (sliceKey(s) == clicked) {
          selectRegion(s);
          break;
        }
    if (kbHov) a.setHint(i18n::tr("Click: play the key (selects its region) \xC2\xB7 right-click: assign the region to the key"));
    // Right-click: the key under the mouse (same geometry as the keyboard).
    if (kbHov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      static const int wOff[7] = {0, 2, 4, 5, 7, 9, 11};
      static const int bAfter[7] = {1, 1, 0, 1, 1, 1, 0};
      const ImVec2 p = sk.at(27, 571), q = sk.at(703, 625), mm = ImGui::GetIO().MousePos;
      const float wwk = (q.x - p.x) / 35.0f, bh = (q.y - p.y) * 0.62f, bw = wwk * 0.62f;
      int note = -1;
      for (int i = 0; i < 35 && note < 0; ++i)
        if (bAfter[i % 7] && i + 1 < 35) {
          const float bx = p.x + (i + 1) * wwk - bw * 0.5f;
          if (mm.x >= bx && mm.x < bx + bw && mm.y < p.y + bh) note = low + (i / 7) * 12 + wOff[i % 7] + 1;
        }
      if (note < 0) {
        const int i = std::clamp(static_cast<int>((mm.x - p.x) / wwk), 0, 34);
        note = low + (i / 7) * 12 + wOff[i % 7];
      }
      ui.kbMenuKey = note;
      ImGui::OpenPopup("##kbmenu");
    }
    if (ImGui::BeginPopup("##kbmenu")) {
      ImGui::TextDisabled("%s", ed::noteName(ui.kbMenuKey).c_str());
      if (ImGui::MenuItem(i18n::tr("Assign the current region to this key"), nullptr, false, classic && nSl > 0 && hasParams)) assignKey(kp, ui.kbMenuKey);
      int owner = -1;
      for (int s = 0; s < nSl && owner < 0; ++s)
        if (sliceKey(s) == ui.kbMenuKey) owner = s;
      if (ImGui::MenuItem(i18n::tr("Select the region on this key"), nullptr, false, owner >= 0)) selectRegion(owner);
      ImGui::EndPopup();
    }
  }
  if (trace && !traceLine.empty()) {  // J-181 dev check: control places (printed when they change)
    static std::string last;
    char b[160];
    std::snprintf(b, sizeof b, " form=%.0f,%.0f,%.0f,%.0f tab=%d display=%d regions=%d snap=%d", static_cast<double>(root.o.x), static_cast<double>(root.o.y),
                  static_cast<double>(root.at(kSxW, formH).x), static_cast<double>(root.at(kSxW, formH).y), ui.tab, ui.display, nSl, ui.snap);
    const std::string line = "kerf-ui:" + traceLine + b;
    if (line != last) {
      last = line;
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
    }
  }
  iskin::end(root, formH);
}

}  // namespace

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != "gl.slicer") return false;
  editorSlicex(a, t, width);
  return true;
}

}  // namespace gl
