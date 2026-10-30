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
#include "../engine/KerfGrid.h"
#include "../engine/KerfMarks.h"
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
#include "DevSignal.h"
#include "PlayLights.h"
#include "SmoothScrollUi.h"
#include "../engine/RegionActivity.h"
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
  int kbMenuKey = -1;                // key under the keyboard's context menu
  // J-182: marker editing (snap, ruler and grid are the instance's parameters).
  int64_t follow = -1;               // a marker just moved: select the region starting there
  int64_t lastAdd = -1;              // frame of the marker added last (dev trace)
  int flagMenu = -1;                 // region under the flag menu
  int edit = -1;                     // region whose name / position the popup edits
  bool openRename = false, openPos = false;
  int posUnit = -1;                  // position entry unit (-1: the ruler's)
  bool posBad = false;               // the typed position was not understood
  char nameBuf[80] = {};
  char posBuf[48] = {};
};
std::map<uint64_t, SlicerUi> slicerUi;

// J-186: the sounding regions of each editor instance (lit regions, flags, pads, read positions).
struct KerfLights {
  playlight::RegionLights lights;
  int frame = -1;
  double time = 0;
  std::vector<int> reported;  // regions whose "kerf-lit:<region>" dev signal was sent while lit
};
std::map<uint64_t, KerfLights> kerfLights;
const playlight::RegionLights& regionLights(App& a, const PluginTarget& t, int regions) {
  KerfLights& e = kerfLights[t.key()];
  const int frame = ImGui::GetFrameCount();
  if (e.frame == frame) return e.lights;
  const double now = ImGui::GetTime();
  const float dt = e.frame < 0 ? 0.0f : static_cast<float>(std::clamp(now - e.time, 0.0, 0.25));
  if (e.frame != frame - 1) e.lights.primed = false;  // the editor was hidden: no stale flashes
  e.frame = frame, e.time = now;
  const RegionActivity* ra = nullptr;
  if (t.isChannel())
    if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen) ra = rt->gen->regionActivity();
  if (ra) {
    e.lights.update(*ra, regions, dt, scrollui::animations());
  } else {
    static const RegionActivity idle;
    e.lights.update(idle, regions, dt, scrollui::animations());
  }
  e.reported.resize(static_cast<size_t>(std::max(0, regions)), 0);
  for (int r = 0; r < regions; ++r) {
    const bool lit = e.lights.lit(r);
    if (lit && !e.reported[static_cast<size_t>(r)]) devsig::reach("kerf-lit:" + std::to_string(r + 1));  // dev checks (waitfor; numbered as the flags)
    e.reported[static_cast<size_t>(r)] = lit;
  }
  return e.lights;
}

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

// The state with the current slices as manual markers (so they can be edited):
// slice k on marker slot k. J-182: names of earlier hand-set markers go (the slots are new).
PluginState manualState(const PluginState& st, const std::vector<int64_t>& starts, const Sample* smp) {
  PluginState ns = st;
  if (!smp || ed::ival(st, SlMode) == SlModeManual) return ns;
  ns.params[static_cast<size_t>(SlMode)] = SlModeManual;
  for (int k = 0; k < kMaxSlices; ++k)
    ns.params[static_cast<size_t>(slMarker(k))] =
        k < static_cast<int>(starts.size()) ? static_cast<float>(static_cast<double>(starts[static_cast<size_t>(k)]) / static_cast<double>(smp->frames())) : -1.f;
  ns.data = kerfmarks::pathOf(st.data);
  return ns;
}
void makeManual(App& a, const PluginTarget& t, const PluginState& st, const std::vector<int64_t>& starts, const Sample* smp) {
  if (!smp || ed::ival(st, SlMode) == SlModeManual) return;
  setPluginState(a, t, manualState(st, starts, smp), "edit slices by hand");
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

std::string fmtTime(double s) {
  char b[32];
  if (s < 10) std::snprintf(b, sizeof b, "%.3f s", s);
  else std::snprintf(b, sizeof b, "%.2f s", s);
  return b;
}

// Layout (J-183, on the reference's band order): the bands, each drawn on its own
// form coordinates and placed under the bands shown before it.
//   master (0..99): three cards: sliders 22/46/70/94 with icon tiles (8..126), MODULATION
//     pad 136 + X / Y wheels 220 (130..336), PLAYBACK trigger 424,10 / fit tempo + beats
//     row 38 / options + mono + declick row 66 (340..562); logo 590,10, keyboard
//     button 574,62, display mode selector 604,62;
//   region panel (99..296): REGION card 8..170 (menu, selector 130, key/cut row 156,
//     AMP / FILTER / PITCH / START rows 184..268), SLICES card 174..334 (mode 130,
//     FILTER inset 158), view column 338..706 (tabs 106, part row 124, view 142..258, row 264);
//   deck bar (296..362): row 1 (298, 32 high): loop 8, play 38, stop 68, menus 160..392,
//     tools 428..708; row 2: captions 333 over values 346, view switches 622..712;
//   wave view (362..540): overview 362, flags 376, wave 390..504, ruler 504, switch row 517;
//   keyboard (540..594): base note 4, two rows of pads 30..710.
enum Band { BMaster, BRegion, BDeck, BWave, BKeys, BCount };
constexpr float kBandY[BCount] = {0, 99, 296, 362, 540}, kBandH[BCount] = {99, 197, 66, 178, 54};
constexpr float kMinScale = 0.86f;  // smallest scale of the form (below it the window scrolls)

void editorSlicex(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  SlicerUi& ui = slicerUi[t.key()];
  const std::string file = slicex::sampleOf(st.data);  // J-41: region-model data starts with the sample
  SamplePtr smp = file.empty() ? nullptr : sampleCache().get(file);
  const RegionView rv = regionView(st, smp.get());
  // J-190: slices on the beat grid (the detected beats; the loop-length grid while the analysis runs).
  const bool gridMode = !rv.on && ed::ival(st, SlMode) == SlModeGrid;
  const kerfgrid::ResultPtr kg = smp && gridMode ? kerfgrid::resultFor(st, smp, false) : nullptr;
  const std::vector<int64_t> starts = rv.on ? rv.starts : gridMode && kg ? kg->cuts : sliceStarts(st, smp.get());
  if (gridMode && kg && !kg->analysing) devsig::reach("kerf-grid-ready");  // dev checks (waitfor): the analysis is in
  const int nSl = static_cast<int>(starts.size());
  if (ui.follow >= 0) {  // J-182: a moved marker keeps its region selected (markers may have changed order)
    for (int s = 0; s < nSl; ++s)
      if (std::llabs(starts[static_cast<size_t>(s)] - ui.follow) <= 1) {
        ui.slice = s;
        break;
      }
    ui.follow = -1;
  }
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
  const playlight::RegionLights& play = regionLights(a, t, nSl);  // J-186: the regions sounding now
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
  // J-182: classic engine names belong to hand-set markers (marker slot -> name, kept after the sample path).
  const std::map<int, std::string> classicNames = manual ? kerfmarks::namesOf(st.data) : std::map<int, std::string>();
  auto regionName = [&](int s) {
    if (rv.on) return static_cast<size_t>(s) < rv.md.decks[0].regions.size() ? rv.md.decks[0].regions[static_cast<size_t>(s)].name : std::string();
    if (classicNames.empty() || s < 0 || s >= nSl) return std::string();
    const auto it = classicNames.find(nearestMarkerParam(st, static_cast<double>(starts[static_cast<size_t>(s)]) / frames));
    return it == classicNames.end() ? std::string() : it->second;
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
  // J-190: the beat grid's cell (choosing one slices on the grid), meter and options.
  auto setGridDiv = [&](int div) {
    PluginState ns = st;
    ns.params[static_cast<size_t>(SlGridDiv)] = static_cast<float>(div);
    ns.params[static_cast<size_t>(SlMode)] = static_cast<float>(SlModeGrid);
    setPluginState(a, t, ns, "slice on the beat grid");
  };
  auto clearMarkers = [&]() {
    PluginState ns = st;
    for (int m = 0; m < kMaxSlices; ++m) ns.params[static_cast<size_t>(slMarker(m))] = m == 0 ? 0.f : -1.f;
    ns.data = kerfmarks::pathOf(st.data);  // J-182: their names go with them
    setPluginState(a, t, ns, "clear slice markers");
  };
  auto addMarkerAt = [&](int64_t frame) {  // classic engine: a manual marker at a frame (one undo step)
    if (!smp) return;
    for (int s = 0; s < nSl; ++s)
      if (std::llabs(starts[static_cast<size_t>(s)] - frame) <= 1) {  // J-182: a marker is there already (snapping lands on it): select it
        ui.slice = s;
        return;
      }
    PluginState ns = manualState(st, starts, smp.get());
    auto names = kerfmarks::namesOf(ns.data);
    for (int q = 0; q < kMaxSlices; ++q)
      if (ns.params[static_cast<size_t>(slMarker(q))] < 0) {
        ns.params[static_cast<size_t>(slMarker(q))] = static_cast<float>(static_cast<double>(frame) / frames);
        names.erase(q);  // a free slot keeps no old name
        ns.data = kerfmarks::withNames(ns.data, names);
        ui.lastAdd = frame;
        ui.follow = frame;
        setPluginState(a, t, ns, "add slice marker");
        return;
      }
    a.notify(i18n::tr("Every marker is in use (64)"), true);
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
  const double gridBeats = smp ? kerfmarks::loopBeats(smp->seconds(), classic && ed::bval(st, SlFitTempo), loopBeats) : 0.0;
  // J-190: on the beat grid, the grid it cuts on (detected beats, or the loop-length grid in its meter).
  const bool detectedGrid = gridMode && kg && kg->source == kerfgrid::SourceDetected;
  const double tempoEst = gridMode && kg ? kg->bpm : smp && smp->seconds() > 0 ? gridBeats * 60.0 / smp->seconds() : 0.0;
  // ---- J-182: the loop's grid (4/4 over its beats), snapping, ruler and positions.
  const kerfmarks::Grid tg = gridMode && kg ? kg->timeGrid() : kerfmarks::straightGrid(smp ? kerfmarks::framesPerBeat(frames, gridBeats) : 0.0);
  const double fpb = tg.fpb;
  const int bpb = std::max(1, tg.beatsPerBar);
  const int snap = std::clamp(ed::ival(st, SlSnap), 0, kerfmarks::SnapCount - 1);
  const bool snapZero = ed::bval(st, SlSnapZero);
  const bool barsRuler = ed::ival(st, SlRuler) == kerfmarks::RulerBars;
  const bool gridShown = ed::bval(st, SlGridLines, true);
  const int rulerUnit = barsRuler ? kerfmarks::UnitBars : kerfmarks::UnitSeconds;
  auto posText = [&](double f) { return kerfmarks::formatPosition(f, rulerUnit, rate, tg); };
  auto snapped = [&](double f, bool bypass) {
    return kerfmarks::snapFrame(smp.get(), std::llround(f), bypass ? static_cast<int>(kerfmarks::SnapOff) : snap, snapZero, tg);
  };
  // Markers that can move: every region start (region engine), the cuts after the first (classic).
  auto canMove = [&](int s) { return smp && s >= 0 && s < nSl && (rv.on || (s > 0 && s < kMaxSlices)); };
  // Moves region s's start marker to a frame (one undo step) and keeps the region selected.
  auto moveMarker = [&](int s, int64_t to) {
    if (!canMove(s)) return;
    to = std::clamp<int64_t>(to, 0, static_cast<int64_t>(frames) - 1);
    if (to == starts[static_cast<size_t>(s)]) return;
    if (rv.on) {
      moveRegionStart(a, t, st, rv, s, to, smp.get());
    } else if (manual) {
      const int mp = nearestMarkerParam(st, static_cast<double>(starts[static_cast<size_t>(s)]) / frames);
      if (mp < 0) return;
      a.doc.setParam(t.pid(slMarker(mp)), static_cast<float>(static_cast<double>(to) / frames), false);
    } else {
      PluginState ns = manualState(st, starts, smp.get());
      ns.params[static_cast<size_t>(slMarker(s))] = static_cast<float>(static_cast<double>(to) / frames);
      setPluginState(a, t, ns, "move slice marker");
    }
    ui.follow = to;
  };
  auto nudge = [&](int s, int dir) {
    if (canMove(s)) moveMarker(s, kerfmarks::nudgeFrame(smp.get(), starts[static_cast<size_t>(s)], dir, snap, snapZero, tg));
  };
  auto canRename = [&](int s) { return smp && s >= 0 && s < nSl && (rv.on || s < kMaxSlices); };
  auto renameRegion = [&](int s, const std::string& typed) {
    if (!canRename(s)) return;
    const std::string nm = kerfmarks::cleanName(typed);
    if (nm == regionName(s)) return;
    if (rv.on) {
      slicex::Model md = rv.md;
      md.decks[0].regions[static_cast<size_t>(s)].name = nm;
      PluginState ns = st;
      ns.data = slicex::sampleOf(st.data) + "\n" + slicex::writeText(md);
      setPluginState(a, t, ns, "rename region");
      return;
    }
    // Classic: the name belongs to the region's marker (hand-set markers first, one undo step).
    PluginState ns = manualState(st, starts, smp.get());
    const int slot = manual ? nearestMarkerParam(ns, static_cast<double>(starts[static_cast<size_t>(s)]) / frames) : s;
    if (slot < 0) return;
    auto names = kerfmarks::namesOf(ns.data);
    if (nm.empty()) names.erase(slot);
    else names[slot] = nm;
    ns.data = kerfmarks::withNames(ns.data, names);
    setPluginState(a, t, ns, "rename region");
  };
  auto removeMarker = [&](int s) {  // classic hand-set marker (and its name)
    if (!manual || !canMove(s)) return;
    const int mp = nearestMarkerParam(st, static_cast<double>(starts[static_cast<size_t>(s)]) / frames);
    if (mp < 0) return;
    PluginState ns = st;
    ns.params[static_cast<size_t>(slMarker(mp))] = -1.f;
    auto names = kerfmarks::namesOf(ns.data);
    names.erase(mp);
    ns.data = kerfmarks::withNames(ns.data, names);
    setPluginState(a, t, ns, "remove slice marker");
  };
  auto beginRename = [&](int s) {
    if (!canRename(s)) return;
    ui.edit = s, ui.openRename = true;
    std::snprintf(ui.nameBuf, sizeof ui.nameBuf, "%s", regionName(s).c_str());
  };
  auto beginPosition = [&](int s) {
    if (!canMove(s)) return;
    ui.edit = s, ui.openPos = true, ui.posBad = false;
    ui.posUnit = -1;
    std::snprintf(ui.posBuf, sizeof ui.posBuf, "%s", posText(static_cast<double>(starts[static_cast<size_t>(s)])).c_str());
  };
  auto setSnap = [&](int m) { a.doc.setParam(t.pid(SlSnap), m, false); };

  // ---- bands shown and the form height; scaled down to fit a short window (J-181),
  // never below kMinScale (J-183: under that the channel window scrolls, text stays readable).
  const bool shown[BCount] = {true, ui.display == 0 || ui.display == 1, ui.display == 0 || ui.display == 2, ui.display == 0 || ui.display == 2, ui.keys};
  float placed[BCount] = {}, formH = 0;
  for (int b = 0; b < BCount; ++b) placed[b] = formH, formH += shown[b] ? kBandH[b] : 0;
  float w = width;
  if (!browserui::thumbMode()) {
    const float avail = ui::EditorArea().y - ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y - 2;
    if (formH * width / kSxW > avail && avail > 0) {
      w = std::max(width * kMinScale, std::floor(avail * kSxW / formH));
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
  static const bool trace = std::getenv("GRIDLINE_KERF_TRACE") != nullptr;
  kerf::auditBegin(trace && !browserui::thumbMode());
  // A control's box for the layout audit: controls with the same top share one row (and its height).
  auto ctl = [](const iskin::Skin& s, const char* id, float x, float y, float bw, float bh) { kerf::auditBox(id, s, x, y, bw, bh, 9000 + static_cast<int>(y)); };
  // Form ground and the accent strip.
  iskin::background(root, 0, 0, kSxW, formH, rgb(0x1A2633), rgb(0x0F171F), 4);
  {
    const ImVec2 p = root.at(0, 0), q = root.at(kSxW, 4);
    dl->AddRectFilledMultiColor(p, q, rgb(0xFF8A10), rgb(kOrange), rgb(kOrange), rgb(0xFF8A10));
    dl->AddLine(ImVec2(p.x, q.y), ImVec2(q.x, q.y), IM_COL32(0, 0, 0, 120));
  }
  std::string traceLine;
  auto traceRect = [&](const char* name, ImVec2 p, ImVec2 q) {
    if (!trace) return;
    char b[96];
    std::snprintf(b, sizeof b, " %s=%.0f,%.0f,%.0f,%.0f", name, static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(q.x),
                  static_cast<double>(q.y));
    traceLine += b;
  };
  using kerf::Role;
  // The detected meter as text ("4/4", "3/4", "6/8"; "" when the grid is not a detected one).
  auto detectedMeter = [&]() -> std::string {
    if (!kg || kg->source != kerfgrid::SourceDetected || !kg->grid) return std::string();
    return kg->compound && kg->beatsPerBar == 2 ? std::string("6/8") : std::to_string(kg->beatsPerBar) + "/4";
  };
  // The beat grid menu (the SLICES card's grid field and the Regions menu): cells, meter, options, status.
  auto gridMenuItems = [&]() {
    const kerfgrid::Settings gs = kerfgrid::settingsOf(st);
    ImGui::TextDisabled("%s", i18n::tr("Slice on the beat grid"));
    for (int d = 0; d < kerfgrid::DivCount; ++d) {
      if (d == kerfgrid::Div4T) ImGui::Separator();
      char lb[48];
      std::snprintf(lb, sizeof lb, "%s##griddiv%d", kerfgrid::divisionName(d), d);
      if (ImGui::MenuItem(lb, nullptr, gridMode && gs.division == d, classic && smp)) setGridDiv(d);
      char id[24];
      std::snprintf(id, sizeof id, "griddiv%d", d);
      traceRect(id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::Separator();
    if (ImGui::BeginMenu(i18n::tr("Meter"), classic)) {
      for (int m = 0; m < kerfgrid::MeterCount; ++m) {
        std::string lb = m == kerfgrid::MeterDetected ? std::string(i18n::tr("As detected")) : std::string(kerfgrid::meterName(m));
        if (m == kerfgrid::MeterDetected && gs.meter == kerfgrid::MeterDetected && !detectedMeter().empty()) lb += " (" + detectedMeter() + ")";
        lb += "##gridmeter" + std::to_string(m);
        if (ImGui::MenuItem(lb.c_str(), nullptr, gs.meter == m)) a.doc.setParam(t.pid(SlGridMeter), m, false);
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Move the cuts onto the nearest hits"), nullptr, gs.hits, classic)) a.doc.setParam(t.pid(SlGridHits), gs.hits ? 0 : 1, false);
    if (ImGui::MenuItem(i18n::tr("Then the nearest zero crossing"), nullptr, gs.zero, classic)) a.doc.setParam(t.pid(SlGridZero), gs.zero ? 0 : 1, false);
    if (ImGui::MenuItem(i18n::tr("Follow the detected beats"), nullptr, gs.follow, classic)) a.doc.setParam(t.pid(SlGridFollow), gs.follow ? 0 : 1, false);
    ImGui::Separator();
    if (!smp) {
      ImGui::TextDisabled("%s", i18n::tr("No loop loaded"));
    } else if (kg && kg->analysing) {
      ImGui::TextDisabled("%s %.0f%%", i18n::tr("Finding the beat..."), static_cast<double>(kg->progress() * 100.0f));
    } else if (kg && kg->source == kerfgrid::SourceDetected) {
      ImGui::TextDisabled("%s: %.1f BPM", i18n::tr("Detected beat"), kg->bpm);
    } else if (kg) {
      ImGui::TextDisabled("%s: %.1f BPM", kg->failed ? i18n::tr("No beat found, the loop length") : i18n::tr("The loop length"), kg->bpm);
    }
    if (kg && kg->found > kMaxSlices) ImGui::TextDisabled("%s", i18n::tr("Only the first 64 slices are kept"));
  };
  const ImU32 capLight = rgb(0xA7B0B9), titleLight = rgb(0xC6D0D9), capDarkCol = rgb(0x22282D), titleDarkCol = rgb(0x2B3237);
  // A field of the wave view's switch row: dark well, the value (or a dim placeholder), a pen glyph when it can be edited.
  auto field = [&](const iskin::Skin& sk, const char* id, float x, float y, float fw, float fh, const std::string& value, const char* placeholder, bool mono,
                   bool enabled, const char* hint) {
    const ImVec2 q0 = sk.at(x, y), q1 = sk.at(x + fw, y + fh);
    ImGui::SetCursorScreenPos(q0);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(q1.x - q0.x, q1.y - q0.y));
    const bool hv = enabled && ImGui::IsItemHovered();
    ImGui::EndDisabled();
    iskin::bevel(sk, x, y, fw, fh, hv ? rgb(0x2C353C) : rgb(0x1E252B), hv ? rgb(kOrange, 0.8f) : rgb(0x0F1418), 3);
    const bool empty = value.empty();
    const ImU32 tc = !enabled ? rgb(0x5C666E) : empty ? rgb(0x7D8892) : mono ? rgb(0xFFD49A) : sk.c.lcdText;
    const Role r = empty || !mono ? Role::Body : Role::Value;
    const std::string shownText = empty ? (*placeholder ? std::string(i18n::tr(placeholder)) : std::string()) : value;
    kerf::text(sk, x + 6, y, fw - 22, fh, kerf::fit(sk, shownText, r, fw - 22).c_str(), r, tc, 0);
    if (enabled) ui::drawIconFit(dl, ui::Icon::ToolDraw, sk.at(x + fw - 13, y + fh * 0.5f - 4.5f), sk.at(x + fw - 4, y + fh * 0.5f + 4.5f), hv ? rgb(kOrange) : rgb(0x6F7B85));
    if (hv) a.setHint(i18n::tr(hint));
    traceRect(id + 2, q0, q1);
    ctl(sk, id, x, y, fw, fh);
    return hit && enabled;
  };

  // ======================================================================= master band
  // Three cards like the reference: sliders (with icon tiles), MODULATION (pad, X / Y), PLAYBACK
  // (trigger, fit tempo + beats, options + mono + declick); logo, keyboard switch and display mode.
  {
    const iskin::Skin sk = bandSkin(BMaster);
    kerf::card(sk, 8, 8, 118, 86, false);
    kerf::card(sk, 130, 8, 206, 86, false);
    kerf::card(sk, 340, 8, 222, 86, false);
    struct Sl {
      int param;
      float x;
      ui::Icon icon;  // Count: the pan glyph
      bool centered, classicOnly;
      const char* id;
      const char* tile;
      const char* hint;
    };
    const Sl sliders[4] = {{rv.on ? SlMaster : SlVolume, 22, ui::Icon::SpeakerSmall, false, false, "##slv", "##tlv", "Master level"},
                           {SlFine, 46, ui::Icon::Detune, true, true, "##sfn", "##tfn", "Fine tune (cents)"},
                           {SlPan, 70, ui::Icon::Count, true, true, "##spn", "##tpn", "Master pan"},
                           {SlPitch, 94, ui::Icon::Piano, true, false, "##spt", "##tpt", "Master pitch (semitones)"}};
    for (const Sl& s : sliders) {
      const bool off = rv.on && s.classicOnly;
      ImGui::BeginDisabled(off);
      iskin::slider(a, t, sk, s.param, s.x, 12, 20, 62, sk.c.fill, s.centered);
      ImGui::EndDisabled();
      ctl(sk, s.id, s.x, 12, 20, 62);
      // The tile under the slider names it (the reference's icon tiles; the hint gives the name).
      const bool hv = ImGui::IsMouseHoveringRect(sk.at(s.x, 78), sk.at(s.x + 20, 92));
      iskin::bevel(sk, s.x, 78, 20, 14, hv ? rgb(0x323D48) : rgb(0x26313C), rgb(0x0C1217), 2);
      const ImU32 ic = off ? rgb(0x4E5A64) : hv ? rgb(kOrange) : rgb(0x9AA6B0);
      const ImVec2 c = sk.at(s.x + 10, 85);
      if (s.icon == ui::Icon::Count) iskin::glyphPan(dl, c, sk.px(4.5f), ic);
      else ui::drawIconFit(dl, s.icon, ImVec2(c.x - sk.px(5.5f), c.y - sk.px(5.5f)), ImVec2(c.x + sk.px(5.5f), c.y + sk.px(5.5f)), ic);
      if (hv) a.setHint(i18n::tr(s.hint));
      ctl(sk, s.tile, s.x, 78, 20, 14);
    }
    {  // X/Y modulation pad and the X / Y wheels
      kerf::text(sk, 220, 12, 110, 16, "MODULATION", Role::Title, titleLight);
      darkWell(sk, 136, 14, 74, 74, 3);
      const ImVec2 p0 = sk.at(136, 14), p1 = sk.at(210, 88);
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
      ctl(sk, "##xy", 136, 14, 74, 74);
      iskin::wheel(a, t, sk, SlModX, 220, 36, 24, rgb(0x959EA6));
      ctl(sk, "##modx", 220, 36, 24, 24);
      kerf::text(sk, 250, 36, 14, 24, "X", Role::Title, rgb(0xB4BEC7));
      kerf::text(sk, 266, 36, 64, 24, "CUTOFF", Role::Label, capLight);
      iskin::wheel(a, t, sk, SlModY, 220, 64, 24, rgb(0x959EA6));
      ctl(sk, "##mody", 220, 64, 24, 24);
      kerf::text(sk, 250, 64, 14, 24, "Y", Role::Title, rgb(0xB4BEC7));
      kerf::text(sk, 266, 64, 64, 24, "RES", Role::Label, capLight);
    }
    // PLAYBACK: how a region plays (one choice over Hold and Play to end), fit to tempo and its beats, options, mono, declick.
    kerf::text(sk, 346, 12, 74, 16, "PLAYBACK", Role::Title, titleLight);
    const int trig = (ed::bval(st, SlHold) ? 1 : 0) + (ed::bval(st, SlPlayToEnd) ? 2 : 0);
    static const char* const trigShort[4] = {i18n::N_("Region, gated"), i18n::N_("Region, one shot"), i18n::N_("To end, gated"), i18n::N_("To end, one shot")};
    static const char* const trigLong[4] = {i18n::N_("Play the region, stop when the key is let go"), i18n::N_("Play the whole region (one shot)"),
                                            i18n::N_("Play on to the end of the loop, stop when the key is let go"),
                                            i18n::N_("Play on to the end of the loop (one shot)")};
    ImGui::BeginDisabled(rv.on);
    {
      iskin::lcd(sk, 424, 10, 132, 20, "", sk.c.lcdBg, sk.c.lcdText, 10);
      const std::string tl = rv.on ? std::string(i18n::tr("Regions of the loop")) : std::string(i18n::tr(trigShort[trig]));
      kerf::text(sk, 430, 10, 112, 20, kerf::fit(sk, tl, Role::Value, 112).c_str(), Role::Value, sk.c.lcdText, 0);
      iskin::glyphArrow(dl, sk.at(549, 20), sk.px(3.5f), sk.c.lcdText, 3);
      ImGui::SetCursorScreenPos(sk.at(424, 10));
      if (ImGui::InvisibleButton("##trigger", ImVec2(sk.px(132), sk.px(20)))) ImGui::OpenPopup("##ktrigger");
      if (ImGui::IsItemHovered())
        a.setHint(rv.on ? i18n::tr("The loop brought its own regions: they play as they are stored") : i18n::tr("How a region plays: click to choose"));
      ctl(sk, "##trigger", 424, 10, 132, 20);
      ImGui::SetNextWindowPos(sk.at(424, 30));
      if (ImGui::BeginPopup("##ktrigger")) {
        for (int m = 0; m < 4; ++m)
          if (ImGui::MenuItem(i18n::tr(trigLong[m]), nullptr, trig == m) && trig != m) {
            PluginState ns = st;  // both switches in one undo step
            ns.params[static_cast<size_t>(SlHold)] = (m & 1) ? 1.f : 0.f;
            ns.params[static_cast<size_t>(SlPlayToEnd)] = (m & 2) ? 1.f : 0.f;
            setPluginState(a, t, ns, "region play mode");
          }
        ImGui::EndPopup();
      }
    }
    auto togParam = [&](const char* id, int param, float x, float y, float tw, float th, const char* caption) {
      const bool on = ed::bval(st, param);
      const PluginInfo* info = findPlugin("gl.slicer");
      const std::string hint = info && static_cast<size_t>(param) < info->params.size() ? info->params[static_cast<size_t>(param)].name + ": click to switch" : "";
      if (kerf::ledToggle(a, sk, id, x, y, tw, th, caption, on, nullptr)) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
      if (ImGui::IsItemHovered() && !hint.empty()) a.setHint(hint);
    };
    togParam("##fittempo", SlFitTempo, 346, 38, 82, 20, "FIT TEMPO");
    kerf::text(sk, 430, 38, 42, 20, "BEATS", Role::Label, capLight, 2);
    iskin::digi(a, t, sk, SlLoopBeats, 476, 38, 80, 20, [](double v) {
      char b[24];
      std::snprintf(b, sizeof b, "%.0f beats", v);
      return std::string(b);
    }, 10);
    ctl(sk, "##beats", 476, 38, 80, 20);
    togParam("##mono", SlMono, 374, 66, 70, 20, "MONO");
    togParam("##declick", SlDeclick, 476, 66, 80, 20, "DECLICK");
    ImGui::EndDisabled();
    if (rv.on) dimArea(sk, 344, 32, 214, 58, 0x1C2733);
    // Options menu (macros and settings).
    if (kerf::deckButton(a, sk, "##options", 346, 66, 20, 20, ui::Icon::ChevronDown, "Options: key macros and playback settings")) ImGui::OpenPopup("##kerfoptions");
    ctl(sk, "##options", 346, 66, 20, 20);
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
    if (!pui::titleWordmarkIn(sk.dl(), "gl.slicer", sk.at(590, 10), sk.at(706, 54), sk.px(26), 2, sk.c.text))  // J-136: the wordmark
      iskin::text(sk, 590, 10, 116, 44, pluginTitle("gl.slicer").c_str(), 26, rgb(kOrangeDeep), 2, false, true);
    // Keyboard switch and display mode.
    if (kerf::deckButton(a, sk, "##kbbtn", 574, 62, 26, 30, ui::Icon::Piano, "Show or hide the keyboard", ui.keys ? 2 : 0)) ui.keys = !ui.keys;
    ctl(sk, "##kbbtn", 574, 62, 26, 30);
    {
      const char* hints[4] = {"Display: everything", "Display: no wave editor", "Display: no region panel", "Display: compact (master band only)"};
      iskin::bevel(sk, 604, 62, 108, 30, rgb(0x0D1318), rgb(0x05080B), 3);
      for (int m = 0; m < 4; ++m) {
        const float x = 606 + m * 26.0f;
        const ImVec2 p = sk.at(x, 64), q = sk.at(x + 24, 90);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(m);
        if (ImGui::InvisibleButton("##disp", ImVec2(q.x - p.x, q.y - p.y))) ui.display = m;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) a.setHint(i18n::tr(hints[m]));
        const bool on = ui.display == m;
        if (on) dl->AddRectFilled(p, q, rgb(kOrange, 0.16f), sk.px(2));
        else if (hov) dl->AddRectFilled(p, q, theme::withAlpha(IM_COL32_WHITE, 0.05f), sk.px(2));
        kerf::bandGlyph(dl, sk.at(x + 6, 68), sk.at(x + 18, 86), m, on ? rgb(kOrange) : hov ? rgb(0xC8D0D7) : rgb(0x7C8892), rgb(0x46515A));
      }
      ctl(sk, "##display", 604, 62, 108, 30);
      traceRect("display", sk.at(604, 62), sk.at(712, 92));
    }
  }

  // ======================================================================= region panel
  // REGION card (menu, title, count; selector; KEY / CUT; AMP, FILTER, PITCH, START rows of
  // two wheels with the per-region switches beside them), SLICES card (slicing mode, FILTER
  // inset) and the view column (target tabs, part row, view, knob row).
  if (shown[BRegion]) {
    const iskin::Skin sk = bandSkin(BRegion);
    {
      const ImVec2 p = sk.at(0, 99), q = sk.at(kSxW, 296);
      iskin::bevel(sk, 0, 99, kSxW, 197, rgb(0x5F676C), 0, 0);
      dl->AddRectFilledMultiColor(p, q, rgb(0x687075), rgb(0x687075), rgb(0x585F64), rgb(0x585F64));
      dl->AddLine(p, ImVec2(q.x, p.y), theme::withAlpha(IM_COL32_WHITE, 0.16f));
      dl->AddLine(ImVec2(p.x, q.y - 1), ImVec2(q.x, q.y - 1), IM_COL32(0, 0, 0, 90));
    }
    // ---- REGION card.
    kerf::card(sk, 8, 104, 162, 190, true);
    if (!smp) {  // nothing loaded: the wordmark and a load button
      const ImVec2 p = sk.at(9, 105), q = sk.at(169, 293);
      dl->AddRectFilledMultiColor(p, q, rgb(0x4C5358), rgb(0x4C5358), rgb(0x40464B), rgb(0x40464B));
      if (!pui::titleWordmarkIn(dl, "gl.slicer", sk.at(24, 146), sk.at(154, 186), sk.px(30), 1, rgb(0xDCE2E7)))
        iskin::text(sk, 24, 146, 130, 40, pluginTitle("gl.slicer").c_str(), 26, rgb(0xDCE2E7), 1, false, true);
      kerf::text(sk, 14, 192, 150, 18, i18n::tr("No loop loaded"), Role::Body, rgb(0xB5BDC4), 1);
      if (kerf::deckButton(a, sk, "##loadbig", 39, 222, 100, 26, ui::Icon::Open, "Load a loop", false, false, i18n::tr("Load a loop"))) loadLoop();
      ctl(sk, "##loadbig", 39, 222, 100, 26);
      traceRect("loadbig", sk.at(39, 222), sk.at(139, 248));
    } else {
      if (kerf::deckButton(a, sk, "##rmenu", 12, 108, 18, 18, ui::Icon::Menu, "Region menu")) ImGui::OpenPopup("##regionmenu");
      ctl(sk, "##rmenu", 12, 108, 18, 18);
      if (ImGui::BeginPopup("##regionmenu")) {
        if (ImGui::MenuItem(i18n::tr("Previous region"), nullptr, false, k > 0)) selectRegion(k - 1);
        if (ImGui::MenuItem(i18n::tr("Next region"), nullptr, false, k + 1 < nSl)) selectRegion(k + 1);
        if (ImGui::MenuItem(i18n::tr("Play region"), nullptr, false, nSl > 0 && sliceKey(k) >= 0)) audition(k);
        if (ImGui::MenuItem(i18n::tr("Show region"), nullptr, false, nSl > 0)) fitRegion(k);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Rename the region..."), nullptr, false, canRename(k))) beginRename(k);
        if (ImGui::MenuItem(i18n::tr("Reset this region's settings"), nullptr, false, classic && nSl > 0 && hasParams)) resetRegion(kp, false);
        if (ImGui::MenuItem(i18n::tr("Back to its automatic key"), nullptr, false, classic && nSl > 0 && hasParams)) a.doc.setParam(t.pid(slParam2(kp, SliceKey)), -1, false);
        ImGui::EndPopup();
      }
      kerf::text(sk, 34, 108, 70, 18, "REGION", Role::Title, titleDarkCol);
      {
        char cnt[24];
        std::snprintf(cnt, sizeof cnt, "%d / %d", nSl > 0 ? k + 1 : 0, nSl);
        kerf::text(sk, 106, 108, 60, 18, cnt, Role::Eyebrow, rgb(0x30373C), 2);
      }
      const std::string cur = nSl > 0 ? regionLabel(k) : std::string(i18n::tr("(no regions)"));
      iskin::lcd(sk, 12, 130, 154, 20, "", sk.c.lcdBg, sk.c.lcdText, 10);
      kerf::text(sk, 17, 130, 134, 20, kerf::fit(sk, cur, Role::Value, 134).c_str(), Role::Value, sk.c.lcdText, 0);
      iskin::glyphArrow(dl, sk.at(158, 140), sk.px(3.5f), sk.c.lcdText, 3);
      ImGui::SetCursorScreenPos(sk.at(12, 130));
      ImGui::InvisibleButton("##rsel", ImVec2(sk.px(154), sk.px(20)));
      if (ImGui::IsItemClicked() && nSl > 0) ImGui::OpenPopup("##regionlist");
      if (ImGui::IsItemHovered()) {
        a.setHint(i18n::tr("Current region: click for the list, mouse wheel to step"));
        const float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0 && nSl > 0) selectRegion(k + (wh > 0 ? -1 : 1));
      }
      ctl(sk, "##rsel", 12, 130, 154, 20);
      ImGui::SetNextWindowPos(sk.at(12, 150));
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
      // Rows: label column 12..56, wheels at 58 and 86 (24), switch / value column 114..168; 28 apart.
      auto rowLabel = [&](float y, float h, const char* s) { kerf::text(sk, 12, y, 44, h, s, Role::Label, capDarkCol, 0); };
      auto rw = [&](const char* id, int param, float x, float y) {
        iskin::wheel(a, t, sk, param, x, y, 24, sk.c.fill);
        ctl(sk, id, x, y, 24, 24);
      };
      auto regionToggle = [&](const char* id, int param, float y, const char* caption) {
        const bool on = ed::bval(st, param);
        if (kerf::ledToggle(a, sk, id, 116, y, 52, 24, caption, on, nullptr, true)) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
        if (ImGui::IsItemHovered()) a.setHint(std::string(i18n::tr(caption)) + ": " + (on ? i18n::tr("on") : i18n::tr("off")) + "   " + i18n::tr("(click to switch)"));
      };
      if (hasParams || nSl == 0) {
        ImGui::BeginDisabled(nSl == 0);
        rowLabel(156, 22, "KEY");
        iskin::digi(a, t, sk, slParam2(kp, SliceKey), 58, 156, 52, 22, [&](double v) {
          int n = static_cast<int>(std::lround(v));
          if (n < 0) return keyName(k) + "*";  // automatic: the key the region gets
          return ed::noteName(n);
        }, 10);
        ctl(sk, "##key", 58, 156, 52, 22);
        kerf::text(sk, 114, 156, 26, 22, "CUT", Role::Label, capDarkCol, 2);
        iskin::digi(a, t, sk, slParam2(kp, SliceCut), 142, 156, 26, 22, [](double v) {
          int n = static_cast<int>(std::lround(v));
          return n <= 0 ? std::string("-") : std::to_string(n);
        }, 10);
        ctl(sk, "##cut", 142, 156, 26, 22);
        rowLabel(184, 24, "AMP");
        rw("##rpan", slParam(kp, SlicePan), 58, 184);
        rw("##rvol", slParam(kp, SliceGain), 86, 184);
        rowLabel(212, 24, "FILTER");
        rowLabel(240, 24, "PITCH");
        rw("##rpitch", slParam(kp, SlicePitch), 86, 240);
        kerf::text(sk, 114, 240, 26, 24, "END", Role::Label, capDarkCol, 2);
        iskin::digi(a, t, sk, slParam2(kp, SliceEnd), 142, 240, 26, 24, [](double v) {
          if (v < 0) return std::string("next");
          char b[16];
          std::snprintf(b, sizeof b, "%.0f%%", v * 100);
          return std::string(b);
        }, 9.5f);
        ctl(sk, "##end", 142, 240, 26, 24);
        rowLabel(268, 24, "START");
        rw("##rdelay", slParam2(kp, SliceDelay), 58, 268);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(nSl == 0 || rv.on);  // classic engine only
        rw("##rcut", slParam(kp, SliceCutoff), 58, 212);
        rw("##rres", slParam2(kp, SliceReso), 86, 212);
        rw("##rstart", slParam(kp, SliceStart), 86, 268);
        regionToggle("##rrev", slParam(kp, SliceReverse), 184, "REV");
        regionToggle("##rmute", slParam(kp, SliceMute), 212, "MUTE");
        ImGui::EndDisabled();
        if (rv.on) {
          dimArea(sk, 56, 210, 56, 28, 0x5A6166);
          dimArea(sk, 84, 266, 28, 28, 0x5A6166);
          dimArea(sk, 114, 182, 56, 56, 0x5A6166);
        }
      } else {  // regions past the 64 slice parameters: their imported settings play as they are
        kerf::text(sk, 14, 172, 150, 16, i18n::tr("This region plays with"), Role::Body, capDarkCol, 1);
        kerf::text(sk, 14, 188, 150, 16, i18n::tr("its imported settings."), Role::Body, capDarkCol, 1);
        kerf::text(sk, 14, 214, 150, 16, i18n::tr("Regions 1 to 64 can"), Role::Body, capDarkCol, 1);
        kerf::text(sk, 14, 230, 150, 16, i18n::tr("be edited here."), Role::Body, capDarkCol, 1);
      }
    }
    // ---- SLICES card: slicing mode, FILTER inset (type, response, cutoff / resonance).
    {
      kerf::card(sk, 174, 104, 160, 190, true);
      kerf::text(sk, 180, 108, 62, 18, "SLICES", Role::Title, titleDarkCol);
      ImGui::BeginDisabled(rv.on);
      {
        // Cells: hits, 4, 8, 16, 32, the beat grid (J-190), by hand. Cell 5 is mode 6 and cell 6 mode 5.
        static const ui::Icon icons[7] = {ui::Icon::SliceWave, ui::Icon::Count, ui::Icon::Count, ui::Icon::Count, ui::Icon::Count, ui::Icon::Metronome,
                                          ui::Icon::ToolDraw};
        static const char* const labels[7] = {nullptr, "4", "8", "16", "32", nullptr, nullptr};
        static const char* const hints[7] = {"Slice at the hits (transients)", "Four even slices", "Eight even slices", "Sixteen even slices",
                                             "Thirty-two even slices", "Slice on the beat grid (the size: the field above)",
                                             "Slice at the markers set by hand"};
        const int md = ed::ival(st, SlMode);
        const int cur = !classic ? -1 : md == SlModeGrid ? 5 : md == SlModeManual ? 6 : md;
        const int mc = kerf::segmented(a, sk, "##slmode", 178, 130, 152, 20, labels, icons, 7, cur, hints, false);
        if (mc >= 0) {
          const int mode = mc == 5 ? SlModeGrid : mc == 6 ? SlModeManual : mc;
          if (mode == SlModeManual) makeManual(a, t, st, starts, smp.get());
          if (mode != SlModeManual || !smp) setMode(mode);
        }
        traceRect("slmode", sk.at(178, 130), sk.at(330, 150));
        traceRect("slgrid", sk.at(178 + 152.0f * 5 / 7, 130), sk.at(178 + 152.0f * 6 / 7, 150));
      }
      {  // J-190: the beat grid's cell and meter (a field on the title row; click: the grid menu)
        const kerfgrid::Settings gs = kerfgrid::settingsOf(st);
        const bool on = classic && gridMode;
        iskin::lcd(sk, 246, 108, 84, 18, "", on ? sk.c.lcdBg : rgb(0x4C5257), on ? sk.c.lcdText : rgb(0x8C949A), 10);
        const std::string meter = gs.meter != kerfgrid::MeterDetected ? std::string(kerfgrid::meterName(gs.meter)) : detectedMeter();
        const std::string val = std::string(kerfgrid::divisionName(gs.division)) + (meter.empty() ? "" : "  " + meter);
        kerf::text(sk, 250, 108, 66, 18, kerf::fit(sk, val, Role::Value, 66).c_str(), Role::Value, on ? sk.c.lcdText : rgb(0x8C949A), 0);
        iskin::glyphArrow(dl, sk.at(322, 117), sk.px(3.0f), on ? sk.c.lcdText : rgb(0x8C949A), 3);
        if (on && kg && kg->analysing) {  // busy: a sweep along the field's foot while the beat is found
          const float ph = static_cast<float>(std::fmod(ImGui::GetTime() * 0.8, 1.0));
          const ImVec2 f0 = sk.at(248, 124), f1 = sk.at(328, 125.5f);
          const float w = (f1.x - f0.x) * 0.3f, x0 = f0.x + (f1.x - f0.x - w) * ph;
          dl->AddRectFilled(ImVec2(x0, f0.y), ImVec2(x0 + w, f1.y), rgb(kOrange, 0.9f));
        }
        ImGui::SetCursorScreenPos(sk.at(246, 108));
        ImGui::InvisibleButton("##gridfield", ImVec2(sk.px(84), sk.px(18)));
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##gridmenu");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          a.setHint(on && kg && kg->analysing ? i18n::tr("Finding the beat of the loop...")
                                               : i18n::tr("Beat grid: slice size and meter (click to choose)"));
        ctl(sk, "##gridfield", 246, 108, 84, 18);
        traceRect("gridfield", sk.at(246, 108), sk.at(330, 126));
        if (ImGui::BeginPopup("##gridmenu")) {
          gridMenuItems();
          ImGui::EndPopup();
        }
      }
      iskin::bevel(sk, 178, 158, 152, 132, rgb(0x50575C), rgb(0x3F454A), 3);
      kerf::text(sk, 184, 160, 70, 18, "FILTER", Role::Title, titleDarkCol);
      {
        static const char* const types[4] = {"OFF", "LP", "HP", "BP"};
        static const char* const typeHints[4] = {"No filter", "Low pass", "High pass", "Band pass"};
        const int tc = kerf::segmented(a, sk, "##ftype", 184, 180, 140, 20, types, nullptr, 4, ed::ival(st, SlFilterType), typeHints, true);
        if (tc >= 0) a.doc.setParam(t.pid(SlFilterType), tc, false);
      }
      // Response curve (vector sketch of the chosen type at the cutoff and resonance).
      darkWell(sk, 184, 204, 140, 28, 3);
      {
        const ImVec2 p0 = sk.at(186, 206), p1 = sk.at(322, 230);
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
      ctl(sk, "##fresp", 184, 204, 140, 28);
      iskin::wheel(a, t, sk, SlCutoff, 204, 236, 34, sk.c.fill2);
      ctl(sk, "##fcut", 204, 236, 34, 34);
      iskin::wheel(a, t, sk, SlReso, 270, 236, 34, sk.c.fill2);
      ctl(sk, "##fres", 270, 236, 34, 34);
      ImGui::EndDisabled();
      kerf::text(sk, 188, 272, 66, 14, "CUTOFF", Role::Label, capDarkCol, 1);
      kerf::text(sk, 254, 272, 66, 14, "RES", Role::Label, capDarkCol, 1);
      if (rv.on) dimArea(sk, 175, 128, 158, 165, 0x5A6166);
    }
    // ---- view column: target tabs, part row, the view and its knob row.
    {
      const float ex = 338, ew = 368;
      const ImU32 acc = rgb(kOrange);
      const char* targets[3] = {"AMP", "REGION", "DETECT"};
      const char* targetHints[3] = {"Amp envelope and velocity", "The selected region", "How the loop is sliced"};
      const bool targetOn[3] = {classic, true, classic};
      const int tc = kerf::tabStrip(a, sk, "##target", ex, 106, ew, 16, targets, 3, ui.tab, targetOn, targetHints, acc);
      if (tc >= 0) ui.tab = tc;
      ctl(sk, "##target", ex, 106, ew, 16);
      traceRect("tab_amp", sk.at(ex, 106), sk.at(ex + ew / 3, 122));
      traceRect("tab_region", sk.at(ex + ew / 3, 106), sk.at(ex + ew * 2 / 3, 122));
      traceRect("tab_detect", sk.at(ex + ew * 2 / 3, 106), sk.at(ex + ew, 122));
      darkWell(sk, ex, 142, ew, 116, 3);
      ctl(sk, "##view", ex, 142, ew, 116);
      const ImVec2 p0 = sk.at(ex + 5, 148), p1 = sk.at(ex + ew - 5, 252);
      // A knob of the row under the view with its caption to the right.
      auto knobCell = [&](const char* id, int param, float x, float capW, const char* cap) {
        iskin::wheel(a, t, sk, param, x, 264, 24, sk.c.fill);
        ctl(sk, id, x, 264, 24, 24);
        kerf::text(sk, x + 28, 264, capW, 24, cap, Role::Label, capDarkCol, 0);
      };
      auto infoStrip = [&](const std::string& s) {
        iskin::bevel(sk, ex, 124, ew, 16, rgb(0x262D33), rgb(0x161B20), 2);
        kerf::text(sk, ex + 6, 124, ew - 12, 16, kerf::fit(sk, s, Role::Body, ew - 12).c_str(), Role::Body, rgb(0xB9C3CC), 0);
        ctl(sk, "##part", ex, 124, ew, 16);
      };
      if (ui.tab == 1) {  // ---- the selected region
        char info[160] = "-";
        if (nSl > 0 && smp) {
          const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]), s1 = sliceEnd(k);
          std::snprintf(info, sizeof info, "%s %d / %d  \xC2\xB7  %s %s  \xC2\xB7  %s %s  \xC2\xB7  %s %s", i18n::tr("Region"), k + 1, nSl, i18n::tr("from"),
                        posText(s0).c_str(), fmtTime((s1 - s0) / rate).c_str(), i18n::tr("long"), i18n::tr("key"), keyName(k).c_str());
        }
        infoStrip(info);
        if (nSl > 0 && smp) {
          const auto [s0, s1] = regionSpan(k);
          kerf::WaveStyle ws;
          ws.peak = rgb(0xF0B060, 0.85f), ws.rms = rgb(0xFFD9A0), ws.edge = rgb(0xFFE2B8, 0.55f), ws.centre = theme::withAlpha(IM_COL32_WHITE, 0.08f);
          faintGrid(dl, p0, p1, 8, 2, 0.04f);
          dl->PushClipRect(p0, p1, true);  // (a very short region draws its samples past the view)
          kerf::drawWave(dl, smp.get(), p0, p1, s0 * frames, std::max(s0 * frames + 1, s1 * frames), ws);
          dl->PopClipRect();
          ImGui::SetCursorScreenPos(p0);
          ImGui::InvisibleButton("##rprev", ImVec2(p1.x - p0.x, p1.y - p0.y));
          if (ImGui::IsItemClicked()) audition(k);
          if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Click: play the region"));
        } else {
          kerf::text(sk, ex, 142, ew, 116, smp ? i18n::tr("No regions") : i18n::tr("Load a loop to see its regions"), Role::Body, rgb(0x6E7A85), 1);
        }
        // Row: step through the regions, play it, show it; its name (click: rename, also on the wave's switch row).
        if (kerf::deckButton(a, sk, "##rpv", ex + 2, 264, 24, 24, ui::Icon::ChevronLeft, "Previous region") && k > 0) selectRegion(k - 1);
        if (kerf::deckButton(a, sk, "##rnx", ex + 28, 264, 24, 24, ui::Icon::Chevron, "Next region") && k + 1 < nSl) selectRegion(k + 1);
        if (kerf::deckButton(a, sk, "##rplay", ex + 54, 264, 24, 24, ui::Icon::Play, "Play the region") && nSl > 0) audition(k);
        if (kerf::deckButton(a, sk, "##rshow", ex + 80, 264, 24, 24, ui::Icon::ScrollTo, "Show the whole region on the wave view") && nSl > 0) fitRegion(k);
        ctl(sk, "##rpv", ex + 2, 264, 24, 24), ctl(sk, "##rnx", ex + 28, 264, 24, 24), ctl(sk, "##rplay", ex + 54, 264, 24, 24),
            ctl(sk, "##rshow", ex + 80, 264, 24, 24);
        const std::string nm = nSl > 0 ? regionName(k) : std::string();
        kerf::text(sk, ex + 112, 264, ew - 116, 24, kerf::fit(sk, nm.empty() ? std::string(nSl > 0 ? i18n::tr("(unnamed region)") : "") : nm, Role::Body, ew - 116).c_str(),
                   Role::Body, nm.empty() ? rgb(0x30373C) : capDarkCol, 0);
      } else if (ui.tab == 0) {  // ---- amp: shape or velocity
        const char* parts[2] = {"SHAPE", "VELOCITY"};
        const char* partHints[2] = {"Attack, release and the fades of every region", "How strongly velocity sets the level"};
        const int pc = kerf::tabStrip(a, sk, "##amppart", ex, 124, ew, 16, parts, 2, ui.ampPart, nullptr, partHints, acc);
        if (pc >= 0) ui.ampPart = pc;
        ctl(sk, "##part", ex, 124, ew, 16);
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
            pts.push_back(ImVec2(X(u), Y(std::clamp(v, 0.0, 1.0) * 0.86)));
          }
          kerf::dashedLine(dl, ImVec2(X(fa), p0.y), ImVec2(X(fa), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.16f), 3, 3);
          kerf::dashedLine(dl, ImVec2(X(fa + fb), p0.y), ImVec2(X(fa + fb), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.16f), 3, 3);
          kerf::text(sk, ex + 8, 146, 80, 14, "ATTACK", Role::Eyebrow, rgb(0x6F7B85), 0);
          kerf::text(sk, ex + ew - 88, 146, 80, 14, "RELEASE", Role::Eyebrow, rgb(0x6F7B85), 2);
          knobCell("##katt", SlAttack, ex + 2, 62, "ATTACK");
          knobCell("##krel", SlRelease, ex + 94, 62, "RELEASE");
          knobCell("##kfin", SlFadeIn, ex + 186, 62, "FADE IN");
          knobCell("##kfout", SlFadeOut, ex + 278, 62, "FADE OUT");
        } else {
          const double amt = ed::val(st, SlVelocity, 1);
          for (int i = 0; i <= 60; ++i) {
            const double u = i / 60.0;
            pts.push_back(ImVec2(X(u), Y((1.0 - amt + amt * u) * 0.86)));
          }
          kerf::text(sk, ex + 8, 236, 80, 14, "SOFT", Role::Eyebrow, rgb(0x6F7B85), 0);
          kerf::text(sk, ex + ew - 88, 236, 80, 14, "HARD", Role::Eyebrow, rgb(0x6F7B85), 2);
          knobCell("##kvel", SlVelocity, ex + 2, 62, "VELOCITY");
          knobCell("##klev", SlVolume, ex + 94, 62, "LEVEL");
          knobCell("##kchr", SlChromatic, ex + 186, 120, "CHROMATIC");
        }
        for (size_t i = 1; i < pts.size(); ++i) {
          const ImU32 tc0 = rgb(kOrange, 0.20f), tc1 = rgb(kOrange, 0.02f);
          dl->AddRectFilledMultiColor(ImVec2(pts[i - 1].x, std::min(pts[i - 1].y, pts[i].y)), ImVec2(pts[i].x, p1.y), tc0, tc0, tc1, tc1);
        }
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), rgb(kOrange), 0, std::max(1.4f, sk.px(1.8f)));
        traceRect("part_vel", sk.at(ex + ew / 2, 124), sk.at(ex + ew, 140));
      } else {  // ---- detection: the loop with its cuts, sensitivity / gap (the mode is the SLICES card's)
        char b[96];
        std::snprintf(b, sizeof b, "%d %s  \xC2\xB7  %.1f BPM  \xC2\xB7  %s", nSl, i18n::tr("slices"), tempoEst,
                      ed::ival(st, SlMode) == 0 ? i18n::tr("cut at the hits") : manual ? i18n::tr("cut at the markers")
                      : !gridMode                ? i18n::tr("even slices")
                      : kg && kg->analysing      ? i18n::tr("finding the beat...")
                      : detectedGrid             ? i18n::tr("on the detected beat")
                                                 : i18n::tr("on the loop-length grid"));
        infoStrip(smp ? b : i18n::tr("No loop loaded"));
        if (smp) {
          kerf::WaveStyle ws;
          ws.peak = rgb(0x5D7C99, 0.9f), ws.rms = rgb(0x86A9C8), ws.centre = theme::withAlpha(IM_COL32_WHITE, 0.06f);
          kerf::drawWave(dl, smp.get(), p0, p1, 0, frames, ws);
          if (gridMode && tg.valid()) {  // J-190: the bars of the grid it cuts on
            for (double bt = std::ceil(tg.beatAt(0) / bpb) * bpb; tg.frameAt(bt) < frames; bt += bpb) {
              const float x = std::round(p0.x + static_cast<float>(tg.frameAt(bt) / frames) * (p1.x - p0.x)) + 0.5f;
              dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), rgb(0xBFD6EA, 0.22f));
            }
          }
          for (int s = 0; s < nSl; ++s) {
            const float x = std::round(p0.x + static_cast<float>(static_cast<double>(starts[static_cast<size_t>(s)]) / frames) * (p1.x - p0.x)) + 0.5f;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), s == k ? rgb(kOrange) : rgb(kOrangeDeep, 0.6f));
          }
        } else {
          kerf::text(sk, ex, 142, ew, 116, i18n::tr("Load a loop to see where it is cut"), Role::Body, rgb(0x6E7A85), 1);
        }
        const bool detect = ed::ival(st, SlMode) == 0;
        ImGui::BeginDisabled(!detect);
        knobCell("##ksens", SlSensitivity, ex + 2, 80, "SENSITIVITY");
        knobCell("##kgap", SlMinGap, ex + 114, 62, "MIN GAP");
        ImGui::EndDisabled();
        if (!detect) dimArea(sk, ex, 262, 206, 28, 0x5F676C);
        if (kerf::deckButton(a, sk, "##detedit", ex + ew - 122, 264, 120, 24, ui::Icon::ToolDraw, "Edit the cuts by hand on the wave view", manual, false,
                             i18n::tr("Edit by hand")))
          makeManual(a, t, st, starts, smp.get());
        ctl(sk, "##detedit", ex + ew - 122, 264, 120, 24);
      }
    }
  }

  // ======================================================================= deck bar
  // Row 1 (298, 32 high): flat transport; menus File, Sample | Edit, Tools, Regions | View, Snap,
  // Select, Zoom; tools Undo, Redo, nudge left / right, marker | hand, add, auto, dump | clear, drag.
  // Row 2: captions over values (rate, channels, tempo, ruler unit, title, length) and view switches.
  if (shown[BDeck]) {
    const iskin::Skin sk = bandSkin(BDeck);
    {
      const ImVec2 p = sk.at(0, 296), q = sk.at(kSxW, 362);
      iskin::bevel(sk, 0, 296, kSxW, 66, rgb(0x293136), 0, 0);
      dl->AddRectFilledMultiColor(p, q, rgb(0x2E373D), rgb(0x2E373D), rgb(0x232A2F), rgb(0x232A2F));
      dl->AddLine(p, ImVec2(q.x, p.y), theme::withAlpha(IM_COL32_WHITE, 0.07f));
      dl->AddLine(sk.at(0, 332), sk.at(kSxW, 332), IM_COL32(0, 0, 0, 60));
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
    if (kerf::flatButton(a, sk, "##loop", 8, 298, 28, 32, ui::Icon::LoopArrows, "Loop: play the region again each time it ends", pl.loop)) pl.loop = !pl.loop;
    if (kerf::flatButton(a, sk, "##play", 38, 298, 28, 32, ui::Icon::Play, "Play the selected region / stop", playing) && t.isChannel() && nSl > 0 &&
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
      const ImVec2 pc = sk.at(52, 314);
      char b[96];
      std::snprintf(b, sizeof b, "kerf: play %.0f %.0f on %d", static_cast<double>(pc.x), static_cast<double>(pc.y), pl.latch.on && pl.ch == t.owner ? 1 : 0);
      if (last != b) {
        last = b;
        std::printf("%s\n", b);
        std::fflush(stdout);
      }
    }
    if (kerf::flatButton(a, sk, "##stop", 68, 298, 28, 32, ui::Icon::Stop, "Stop", false) && playing && t.isChannel()) stopPlay();
    // A tool tile of row 1 (24 x 32; `menu`: the small caret under the glyph).
    auto tile = [&](const char* id, float x, ui::Icon icon, const char* hint, int lit = 0, bool menu = false) {
      const bool hit = kerf::deckButton(a, sk, id, x, 298, 24, 32, icon, hint, lit, menu);
      ctl(sk, id, x, 298, 24, 32);
      return hit;
    };
    // Menus: file, sample | edit, tools, regions | view, snap, select, zoom.
    if (tile("##mfile", 160, ui::Icon::Folder, "File", 0, true)) ImGui::OpenPopup("##kfile");
    if (ImGui::BeginPopup("##kfile")) {
      if (ImGui::MenuItem(i18n::tr("Load a loop..."))) loadLoop();
      if (ImGui::MenuItem(i18n::tr("Remove the loop"), nullptr, false, !file.empty())) {
        PluginState ns = st;
        ns.data.clear();
        setPluginState(a, t, ns, "remove loop");
      }
      ImGui::EndPopup();
    }
    if (tile("##msample", 184, ui::Icon::Info, "Sample", 0, true)) ImGui::OpenPopup("##ksample");
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
    if (tile("##medit", 216, ui::Icon::Clone, "Edit", 0, true)) ImGui::OpenPopup("##kedit");
    if (ImGui::BeginPopup("##kedit")) {
      if (ImGui::MenuItem(i18n::tr("Undo"))) a.doc.undo();
      if (ImGui::MenuItem(i18n::tr("Redo"))) a.doc.redo();
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit the cuts by hand"), nullptr, manual, classic && smp)) makeManual(a, t, st, starts, smp.get());
      if (ImGui::MenuItem(i18n::tr("Clear the markers"), nullptr, false, manual)) clearMarkers();
      ImGui::EndPopup();
    }
    if (tile("##mtools", 240, ui::Icon::Settings, "Tools", 0, true)) ImGui::OpenPopup("##ktools");
    if (ImGui::BeginPopup("##ktools")) {
      if (ImGui::MenuItem(i18n::tr("Write the regions to the piano roll"), nullptr, false, nSl > 0 && t.isChannel())) dumpScore();
      if (ImGui::MenuItem(i18n::tr("Map regions upwards from the base note"), nullptr, false, classic && nSl > 0)) resetKeys();
      if (ImGui::MenuItem(i18n::tr("Reset every region's settings"), nullptr, false, classic && nSl > 0)) resetRegion(0, true);
      ImGui::EndPopup();
    }
    if (tile("##mregions", 264, ui::Icon::ToolSlice, "Regions: slicing", 0, true)) ImGui::OpenPopup("##regionsmenu");
    if (ImGui::BeginPopup("##regionsmenu")) {
      const int md = ed::ival(st, SlMode);
      if (ImGui::MenuItem(i18n::tr("Slice at the hits"), nullptr, classic && md == 0, classic)) setMode(0);
      if (ImGui::MenuItem(i18n::tr("Even slices: 4"), nullptr, classic && md == 1, classic)) setMode(1);
      if (ImGui::MenuItem(i18n::tr("Even slices: 8"), nullptr, classic && md == 2, classic)) setMode(2);
      if (ImGui::MenuItem(i18n::tr("Even slices: 16"), nullptr, classic && md == 3, classic)) setMode(3);
      if (ImGui::MenuItem(i18n::tr("Even slices: 32"), nullptr, classic && md == 4, classic)) setMode(4);
      if (ImGui::BeginMenu(i18n::tr("On the beat grid"), classic)) {  // J-190
        gridMenuItems();
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit markers by hand"), nullptr, manual, classic)) makeManual(a, t, st, starts, smp.get());
      if (ImGui::MenuItem(i18n::tr("Add a marker in the selected region"), nullptr, false, classic && nSl > 0 && smp)) {
        addMarkerAt(snapped((static_cast<double>(starts[static_cast<size_t>(k)]) + sliceEnd(k)) * 0.5, false));
      }
      if (ImGui::MenuItem(i18n::tr("Clear markers"), nullptr, false, manual)) clearMarkers();
      ImGui::Separator();
      // J-182: the selected region's marker: name, exact position, one snap step either way.
      if (ImGui::MenuItem(i18n::tr("Rename the region..."), nullptr, false, canRename(k))) beginRename(k);
      if (ImGui::MenuItem(i18n::tr("Type the marker's position..."), nullptr, false, canMove(k))) beginPosition(k);
      if (ImGui::MenuItem(i18n::tr("Nudge the marker left"), nullptr, false, canMove(k))) nudge(k, -1);
      if (ImGui::MenuItem(i18n::tr("Nudge the marker right"), nullptr, false, canMove(k))) nudge(k, 1);
      ImGui::EndPopup();
    }
    if (tile("##mview", 296, ui::Icon::Eye, "View", 0, true)) ImGui::OpenPopup("##kview");
    if (ImGui::BeginPopup("##kview")) {
      const char* names[4] = {"Everything", "No wave editor", "No region panel", "Compact"};
      for (int m = 0; m < 4; ++m)
        if (ImGui::MenuItem(i18n::tr(names[m]), nullptr, ui.display == m)) ui.display = m;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Keyboard"), nullptr, ui.keys)) ui.keys = !ui.keys;
      ImGui::Separator();  // J-182: time units of the ruler, the beat grid
      if (ImGui::MenuItem(i18n::tr("Ruler in seconds"), nullptr, !barsRuler)) a.doc.setParam(t.pid(SlRuler), kerfmarks::RulerSeconds, false);
      if (ImGui::MenuItem(i18n::tr("Ruler in bars and beats"), nullptr, barsRuler)) a.doc.setParam(t.pid(SlRuler), kerfmarks::RulerBars, false);
      if (ImGui::MenuItem(i18n::tr("Beat grid lines"), nullptr, gridShown)) a.doc.setParam(t.pid(SlGridLines), gridShown ? 0 : 1, false);
      ImGui::EndPopup();
    }
    if (tile("##msnap", 320, ui::Icon::Magnet, "Snap: where moved and added markers land", snap != 0 ? 2 : 0, true)) ImGui::OpenPopup("##ksnap");
    traceRect("snapbtn", sk.at(320, 298), sk.at(344, 330));
    if (ImGui::BeginPopup("##ksnap")) {
      // J-182: grid sizes of the loop (4/4 over its beats), zero crossings, and grid then zero crossing.
      static const char* const names[kerfmarks::SnapCount] = {i18n::N_("No snapping"), i18n::N_("Zero crossings"), i18n::N_("Bar"),
                                                              i18n::N_("Half bar"), i18n::N_("Beat"), i18n::N_("1/2 beat"),
                                                              i18n::N_("1/3 beat (triplets)"), i18n::N_("1/4 beat"),
                                                              i18n::N_("1/6 beat (triplets)"), i18n::N_("1/8 beat")};
      for (int m = 0; m < kerfmarks::SnapCount; ++m) {
        if (m == kerfmarks::SnapBar) ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr(names[m]), nullptr, snap == m, m < kerfmarks::SnapBar || fpb > 0)) setSnap(m);
        char id[16];
        std::snprintf(id, sizeof id, "snapitem%d", m);
        traceRect(id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Then the nearest zero crossing"), nullptr, snapZero, kerfmarks::isGrid(snap)))
        a.doc.setParam(t.pid(SlSnapZero), snapZero ? 0 : 1, false);
      ImGui::Separator();
      ImGui::TextDisabled("%s", i18n::tr("Hold Alt while dragging: no snapping"));
      ImGui::EndPopup();
    }
    if (tile("##mselect", 344, ui::Icon::Select, "Select", 0, true)) ImGui::OpenPopup("##kselect");
    if (ImGui::BeginPopup("##kselect")) {
      if (ImGui::MenuItem(i18n::tr("First region"), nullptr, false, nSl > 0)) selectRegion(0);
      if (ImGui::MenuItem(i18n::tr("Previous region"), nullptr, false, k > 0)) selectRegion(k - 1);
      if (ImGui::MenuItem(i18n::tr("Next region"), nullptr, false, k + 1 < nSl)) selectRegion(k + 1);
      if (ImGui::MenuItem(i18n::tr("Last region"), nullptr, false, nSl > 0)) selectRegion(nSl - 1);
      ImGui::EndPopup();
    }
    if (tile("##mzoom", 368, ui::Icon::Zoom, "Zoom", 0, true)) ImGui::OpenPopup("##viewmenu");
    if (ImGui::BeginPopup("##viewmenu")) {
      const double mid = (ui.goFrom + ui.goTo) * 0.5;
      if (ImGui::MenuItem(i18n::tr("Zoom in"))) zoom(0.5, mid);
      if (ImGui::MenuItem(i18n::tr("Zoom out"))) zoom(2.0, mid);
      if (ImGui::MenuItem(i18n::tr("Show the whole loop"))) setView(0, 1);
      if (ImGui::MenuItem(i18n::tr("Show the selected region"), nullptr, false, nSl > 0 && smp)) fitRegion(k);
      ImGui::EndPopup();
    }
    traceRect("menu_regions", sk.at(264, 298), sk.at(288, 330));
    // Tools: undo, redo, the marker one snap step left / right, the marker's name and position.
    if (tile("##undo", 428, ui::Icon::Undo, "Undo")) a.doc.undo();
    if (tile("##redo", 452, ui::Icon::Redo, "Redo")) a.doc.redo();
    ImGui::BeginDisabled(!canMove(k));
    if (tile("##nudgel", 476, ui::Icon::Left, "Nudge the marker left by one snap step (Left arrow over the wave)")) nudge(k, -1);
    if (tile("##nudger", 500, ui::Icon::Right, "Nudge the marker right by one snap step (Right arrow over the wave)")) nudge(k, 1);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!canRename(k) && !canMove(k));
    if (tile("##marker", 524, ui::Icon::ListBullets, "The selected region's marker: name, position", 0, true)) ImGui::OpenPopup("##kmarker");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##kmarker")) {
      if (nSl > 0) ImGui::TextDisabled("%s", regionLabel(k).c_str());
      if (ImGui::MenuItem(i18n::tr("Rename the region..."), nullptr, false, canRename(k))) beginRename(k);
      if (ImGui::MenuItem(i18n::tr("Type the marker's position..."), nullptr, false, canMove(k))) beginPosition(k);
      if (ImGui::MenuItem(i18n::tr("Snap the marker"), nullptr, false, canMove(k) && snap != kerfmarks::SnapOff))
        moveMarker(k, snapped(static_cast<double>(starts[static_cast<size_t>(k)]), false));
      if (ImGui::MenuItem(i18n::tr("Remove the marker"), nullptr, false, manual && canMove(k))) removeMarker(k);
      ImGui::EndPopup();
    }
    // Hand, add, auto (right-click: even slices), dump | clear, drag out.
    ImGui::BeginDisabled(rv.on);
    if (tile("##hand", 556, ui::Icon::ToolDraw, "Edit the markers by hand (double-click the wave: add, right-click a marker: remove)", manual))
      makeManual(a, t, st, starts, smp.get());
    if (tile("##add", 580, ui::Icon::Flag, "Add a marker in the middle of the selected region") && smp && nSl > 0)
      addMarkerAt(snapped((static_cast<double>(starts[static_cast<size_t>(k)]) + sliceEnd(k)) * 0.5, false));
    if (tile("##auto", 604, ui::Icon::SliceWave, "Slice at the hits (right-click: even slices)", classic && ed::ival(st, SlMode) == 0)) setMode(0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##kevens");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##kevens")) {
      const int md = ed::ival(st, SlMode);
      for (int m = 1; m <= 4; ++m) {
        char lb[48];
        std::snprintf(lb, sizeof lb, "%s: %d", i18n::tr("Even slices"), 2 << m);
        if (ImGui::MenuItem(lb, nullptr, classic && md == m, classic)) setMode(m);
      }
      ImGui::EndPopup();
    }
    ImGui::BeginDisabled(nSl == 0 || !t.isChannel());
    if (tile("##dump", 628, ui::Icon::Piano, "Write the regions to the piano roll (current pattern)")) dumpScore();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!manual);
    if (tile("##clear", 660, ui::Icon::Trash, "Clear the markers")) clearMarkers();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(file.empty());
    tile("##drag", 684, ui::Icon::GripDots, "Drag the loop out (to the playlist, a channel or the browser)");
    if (!file.empty()) browserui::fileDragSource(file);
    ImGui::EndDisabled();
    traceRect("dump", sk.at(628, 298), sk.at(652, 330));
    // Row 2: a caption over each value, hairlines between the fields.
    const ImU32 dim = rgb(0x7F8B95), lit = rgb(0xD8DFE5);
    auto infoField = [&](float x, float fw, const char* caption, const std::string& value, int align, ImU32 col) {
      kerf::text(sk, x, 333, fw, 12, caption, Role::Eyebrow, dim, align);
      kerf::text(sk, x, 346, fw, 14, kerf::fit(sk, value, Role::Value, fw).c_str(), Role::Value, col, align);
    };
    auto hairline = [&](float x) { dl->AddLine(sk.at(x, 336), sk.at(x, 359), theme::withAlpha(IM_COL32_WHITE, 0.08f)); };
    char info[96];
    std::snprintf(info, sizeof info, "%d Hz", static_cast<int>(rate));
    infoField(8, 56, "RATE", smp ? info : "--", 0, lit);
    hairline(68);
    infoField(72, 56, "CHANNELS", smp ? (smp->mono() ? "mono" : "stereo") : "--", 0, lit);
    hairline(132);
    std::snprintf(info, sizeof info, "%.1f BPM", tempoEst);
    infoField(136, 62, "TEMPO", smp ? info : "--", 0, lit);
    if (ImGui::IsMouseHoveringRect(sk.at(136, 333), sk.at(198, 360))) a.setHint(i18n::tr("Tempo of the loop at its length in beats (Fit tempo)"));
    hairline(202);
    {  // J-182: the ruler's unit (seconds / bars and beats), also a click on the ruler and the View menu
      kerf::text(sk, 206, 333, 64, 12, "RULER", Role::Eyebrow, dim, 0);
      static const char* const units[2] = {"SEC", "BAR"};
      static const char* const unitHints[2] = {"Ruler and positions in seconds", "Ruler and positions in bars and beats"};
      const int uc = kerf::segmented(a, sk, "##rulerunit", 206, 345, 64, 15, units, nullptr, 2, barsRuler ? 1 : 0, unitHints, true);
      if (uc >= 0) a.doc.setParam(t.pid(SlRuler), uc == 1 ? kerfmarks::RulerBars : kerfmarks::RulerSeconds, false);
    }
    hairline(274);
    const std::string nm = smp ? fs::path(file).filename().string() : std::string(i18n::tr("No loop: drop one on the wave view"));
    infoField(278, 262, "TITLE", nm, 0, smp ? lit : dim);
    hairline(544);
    std::string len = "--";
    if (nSl > 0 && smp) {
      const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]), s1 = sliceEnd(k);
      kerfmarks::Grid dur = kerfmarks::straightGrid(fpb);  // a length: in mean beats of the grid
      dur.beatsPerBar = bpb;
      len = barsRuler ? kerfmarks::barsBeats(s1 - s0, dur, true) : fmtTime((s1 - s0) / rate);
    }
    infoField(548, 68, "LENGTH / SEL", len, 2, rgb(kOrange, 0.95f));
    const double mid = (ui.goFrom + ui.goTo) * 0.5;
    auto vbtn = [&](const char* id, float x, ui::Icon icon, const char* hint) {
      const bool hit = kerf::deckButton(a, sk, id, x, 338, 18, 20, icon, hint);
      ctl(sk, id, x, 338, 18, 20);
      return hit;
    };
    if (vbtn("##zout", 622, ui::Icon::Minus, "Zoom out")) zoom(2.0, mid);
    if (vbtn("##zin", 640, ui::Icon::Plus, "Zoom in")) zoom(0.5, mid);
    if (vbtn("##zall", 658, ui::Icon::Zoom, "Show the whole loop")) setView(0, 1);
    if (vbtn("##zreg", 676, ui::Icon::ScrollTo, "Show the selected region") && nSl > 0) fitRegion(k);
    if (vbtn("##zplay", 694, ui::Icon::Speaker, "Play the selected region once") && nSl > 0) audition(k);
  }

  // ======================================================================= wave view
  // Overview scroller (362, on top as in the reference), region flags (376), the wave (390..504),
  // the ruler (504) and the switch row (517: options, hand edit, snap, name, position, view switches).
  if (shown[BWave]) {
    const iskin::Skin sk = bandSkin(BWave);
    const float wx = 0, wy = 362, ww = kSxW, scrollH = 14, flagH = 14, waveY = 390, waveB = 504, rulerH = 13, rowY = 517;
    iskin::bevel(sk, wx, wy, ww, 178, rgb(0x0B1014), 0, 0);
    {
      const ImVec2 p = sk.at(wx, waveY), q = sk.at(wx + ww, waveB);
      dl->AddRectFilledMultiColor(p, q, rgb(0x0F161D), rgb(0x0F161D), rgb(0x0A1015), rgb(0x0A1015));
    }
    iskin::bevel(sk, wx, wy + scrollH, ww, flagH, rgb(0x161E26), 0, 0);
    sk.dl()->AddLine(sk.at(wx, waveY), sk.at(wx + ww, waveY), IM_COL32(0, 0, 0, 140));
    const ImVec2 top = sk.at(wx + 2, wy + scrollH), p0 = sk.at(wx + 2, waveY), p1 = sk.at(wx + ww - 2, waveB);
    const double f0 = ui.viewFrom * frames, f1 = ui.viewTo * frames;
    auto fx = [&](double f) { return p0.x + static_cast<float>((f - f0) / std::max(1e-9, f1 - f0)) * (p1.x - p0.x); };
    auto frameAt = [&](float x) { return f0 + std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * (f1 - f0); };
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    if (!smp) {  // ---- empty: the drop zone
      if (kerf::dropZone(a, sk, "##drop", 24, waveY + 6, ww - 48, waveB - waveY - 12, rgb(theme::role::kSlice))) loadLoop();
      traceRect("dropzone", sk.at(24, waveY + 6), sk.at(ww - 24, waveB - 6));
    }
    const float labelPx = std::max(kerf::kMinTextPx, sk.px(10));
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
    struct FlagRect {
      float x0, x1;
      int s;
    };
    std::vector<FlagRect> flagRects;  // J-182: the flags as drawn (right-click: the region's menu)
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
        if (const float pl = play.light(s); pl > 0.003f) {  // J-186: sounding: lit from the top, a bright rim
          dl->AddRectFilledMultiColor(r0, r1, rgb(0xFFD08A, 0.26f * pl), rgb(0xFFD08A, 0.26f * pl), rgb(0xFFD08A, 0.07f * pl), rgb(0xFFD08A, 0.07f * pl));
          dl->AddLine(ImVec2(r0.x, p0.y + 1.0f), ImVec2(r1.x, p0.y + 1.0f), rgb(0xFFE6BF, 0.95f * pl), 2.0f);
        }
      }
      // J-182: the beat grid at the snap size (or beats under a bars:beats ruler): bars over beats over
      // subdivisions, each level fading out as its lines close up.
      // J-190: on the beat grid, the lines follow the detected beats (and show its cell when no snap is set).
      const double gStep = kerfmarks::isGrid(snap) ? kerfmarks::stepBeats(snap, bpb)
                           : gridMode && kg ? kerfgrid::cellBeats(kerfgrid::settingsOf(st).division, bpb, kg->compound)
                                            : (barsRuler ? 1.0 : 0.0);
      if (gridShown && gStep > 0 && fpb > 0 && f1 > f0) {
        const double pxPerBeat = static_cast<double>(p1.x - p0.x) / ((f1 - f0) / fpb);
        auto fade = [](double px) { return static_cast<float>(std::clamp((px - 4.0) / 10.0, 0.0, 1.0)); };
        const float aBar = 0.24f * fade(pxPerBeat * bpb), aBeat = 0.13f * fade(pxPerBeat), aSub = 0.075f * fade(pxPerBeat * gStep);
        double each = gStep;  // the finest level still shown
        if (each < 1 && aSub <= 0) each = 1;
        if (each <= 1 && aBeat <= 0) each = bpb;
        if (each < bpb && fade(pxPerBeat * each) <= 0) each = bpb;
        if (aBar > 0 || each < bpb) {
          const double b0 = tg.beatAt(f0), b1 = tg.beatAt(f1);
          for (long long i = static_cast<long long>(std::ceil(b0 / each - 1e-9)); static_cast<double>(i) * each <= b1 + 1e-9; ++i) {
            const double beat = static_cast<double>(i) * each;
            const bool onBeat = std::fabs(beat - std::round(beat)) < 1e-6;
            const bool onBar = onBeat && ((std::llround(beat) % bpb) + bpb) % bpb == 0;
            const float al = onBar ? aBar : onBeat ? aBeat : aSub;
            if (al < 0.004f) continue;
            const float x = std::round(fx(tg.frameAt(beat))) + 0.5f;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), onBar ? rgb(0xBFD6EA, al) : theme::withAlpha(IM_COL32_WHITE, al));
          }
        }
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
      for (int h = 0; h < play.headCount; ++h) {  // J-186: the read position of each voice, in its region
        const auto& hd = play.heads[h];
        if (hd.region >= nSl || static_cast<double>(hd.frame) < f0 || static_cast<double>(hd.frame) > f1) continue;
        const float x = std::round(fx(static_cast<double>(hd.frame))) + 0.5f, pl = play.light(hd.region);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), rgb(0xFFF1DA, 0.35f + 0.55f * pl), 1.0f);
      }
      // Flags: an orange handle on the marker with the number and name (number only, or
      // nothing, when there is no room before the next region's flag).
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
        auto fits = [&](const std::string& tx) { return a.fontMono && a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, tx.c_str()).x + sk.px(10) <= room; };
        if (!rn.empty() && fits(lbl + " " + rn)) lbl += " " + rn;
        if (!fits(lbl)) lbl.clear();
        const bool sel = s == k, hv = s == hovRegion;
        const float tw = lbl.empty() ? std::clamp(spacing * 0.35f, 1.0f, sk.px(5))
                                     : a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, lbl.c_str()).x + sk.px(10);
        const ImVec2 t0(x, top.y + sk.px(1)), t1(x + tw, p0.y);
        flagRects.push_back({x - 2, std::max(x + tw, x + 3), s});
        const float pl = play.light(s);  // J-186: a sounding region's flag lights up
        ImU32 fc = sel ? rgb(kOrange) : hv ? rgb(kOrangeDeep, 0.85f) : rgb(kOrangeDeep, lbl.empty() ? std::min(0.8f, markerAlpha * 1.6f) : 0.62f);
        if (pl > 0.003f) {
          fc = lerp(fc, rgb(0xFFE6BF), pl);
          dl->AddRectFilled(ImVec2(x - sk.px(1.5f), t0.y - sk.px(1)), ImVec2(x + std::max(tw, sk.px(5)) + sk.px(1.5f), t1.y), rgb(0xFFB54A, 0.30f * pl), sk.px(3),
                            ImDrawFlags_RoundCornersTop);  // glow
        }
        dl->AddRectFilled(t0, ImVec2(x + std::min(tw, sk.px(5)), t1.y), fc, sk.px(1.5f), ImDrawFlags_RoundCornersTop);  // the handle
        if (!lbl.empty()) {
          const bool bright = sel || pl > 0.5f;
          dl->AddRectFilled(ImVec2(x + sk.px(5), t0.y), t1, lerp(sel ? rgb(kOrange, 0.9f) : rgb(0x2A1D10, 0.85f), rgb(0xFFC873), pl), sk.px(2),
                            ImDrawFlags_RoundCornersTopRight);
          dl->AddText(a.fontMono, labelPx, ImVec2(x + sk.px(7), t0.y + (t1.y - t0.y - labelPx) * 0.5f - 0.5f), bright ? rgb(0x1E160A) : rgb(0xF4D2A6), lbl.c_str());
        }
      }
      dl->PopClipRect();
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##wave", ImVec2(p1.x - top.x, p1.y - top.y));
    traceRect("wave", p0, p1);
    ctl(sk, "##wave", 2, wy + scrollH, ww - 4, waveB - wy - scrollH);
    const bool hov = ImGui::IsItemHovered() && smp;
    int hoverMarker = -1;
    // J-182: a dragged marker is shown where it will land (snapped unless Alt is held) and moves on release (one undo step).
    const bool dragging = ui.drag >= 0 && ui.drag < nSl && ImGui::IsItemActive();
    const int64_t dragTo = dragging ? snapped(frameAt(m.x), io.KeyAlt) : -1;
    if (smp) {
      dl->PushClipRect(top, ImVec2(p1.x, p1.y), true);
      for (int s = 0; s < nSl; ++s) {
        float x = std::round(fx(static_cast<double>(starts[static_cast<size_t>(s)]))) + 0.5f;
        if (dragging && s == ui.drag) x = std::round(fx(static_cast<double>(dragTo))) + 0.5f;
        if (x < p0.x - 1 || x > p1.x + 1) continue;
        if (s == 1) traceRect("mk1", ImVec2(x, p0.y), ImVec2(x, p1.y));
        const bool near = grabMarkers && hov && std::fabs(m.x - x) < 4 && canMove(s);
        if (near) hoverMarker = s;
        if (s != k) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), near ? rgb(0xFFFFFF, 0.9f) : rgb(kOrangeDeep, markerAlpha), near ? 2.0f : 1.0f);
      }
      if (k < nSl) {  // the selected region's marker on top
        float x = std::round(fx(static_cast<double>(starts[static_cast<size_t>(k)]))) + 0.5f;
        if (dragging && k == ui.drag) x = std::round(fx(static_cast<double>(dragTo))) + 0.5f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), rgb(kOrange), hoverMarker == k ? 2.0f : 1.4f);
      }
      if (dragging) {  // where it lands, in the ruler's unit
        const float x = std::round(fx(static_cast<double>(dragTo))) + 0.5f;
        const std::string lt = posText(static_cast<double>(dragTo)) + (io.KeyAlt && snap != kerfmarks::SnapOff ? "  (free)" : "");
        const ImVec2 ts = a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, lt.c_str());
        const float bx = std::clamp(x + sk.px(5), p0.x + 2, p1.x - ts.x - sk.px(8));
        dl->AddRectFilled(ImVec2(bx, p0.y + sk.px(4)), ImVec2(bx + ts.x + sk.px(8), p0.y + sk.px(4) + ts.y + sk.px(4)), IM_COL32(8, 12, 16, 220), sk.px(3));
        dl->AddText(a.fontMono, labelPx, ImVec2(bx + sk.px(4), p0.y + sk.px(6)), rgb(0xFFD49A), lt.c_str());
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
      }
    }
    if (ui.drag >= 0 && ImGui::IsItemDeactivated() && ui.drag < nSl && smp) {
      // Released: the marker moves (one undo step; classic slicing becomes hand-set markers); a click without moving selects.
      const int64_t to = snapped(frameAt(m.x), io.KeyAlt);
      if (std::fabs(fx(static_cast<double>(to)) - fx(static_cast<double>(starts[static_cast<size_t>(ui.drag)]))) >= 2.0f)
        moveMarker(ui.drag, to);
      else
        ui.slice = ui.drag;
    }
    if (ImGui::IsItemDeactivated()) ui.drag = -1;
    if (hov && classic && m.y >= p0.y && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addMarkerAt(snapped(frameAt(m.x), io.KeyAlt));
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (m.y < p0.y) {  // a flag: the region's menu
        for (auto it = flagRects.rbegin(); it != flagRects.rend(); ++it)
          if (m.x >= it->x0 && m.x < it->x1) {
            ui.flagMenu = it->s;
            ImGui::OpenPopup("##kflag");
            break;
          }
      } else if (hoverMarker > 0 && manual) {
        removeMarker(hoverMarker);
      }
    }
    if (ImGui::BeginPopup("##kflag")) {
      const int s = ui.flagMenu;
      if (s >= 0 && s < nSl) {
        ImGui::TextDisabled("%s", regionLabel(s).c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Rename..."), nullptr, false, canRename(s))) beginRename(s);
        if (ImGui::MenuItem(i18n::tr("Type the position..."), nullptr, false, canMove(s))) beginPosition(s);
        if (ImGui::MenuItem(i18n::tr("Nudge left by one snap step"), nullptr, false, canMove(s))) nudge(s, -1);
        if (ImGui::MenuItem(i18n::tr("Nudge right by one snap step"), nullptr, false, canMove(s))) nudge(s, 1);
        if (ImGui::MenuItem(i18n::tr("Snap the marker"), nullptr, false, canMove(s) && snap != kerfmarks::SnapOff))
          moveMarker(s, snapped(static_cast<double>(starts[static_cast<size_t>(s)]), false));
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Select"), nullptr, s == k)) selectRegion(s);
        if (ImGui::MenuItem(i18n::tr("Play"), nullptr, false, sliceKey(s) >= 0)) audition(s);
        if (ImGui::MenuItem(i18n::tr("Show the whole region"))) fitRegion(s);
        if (ImGui::MenuItem(i18n::tr("Remove the marker"), nullptr, false, manual && canMove(s))) removeMarker(s);
      }
      ImGui::EndPopup();
    }
    // Arrow keys over the wave view: the selected region's marker by one snap step.
    if (hovWave && nSl > 0 && !io.WantTextInput && !ImGui::IsAnyItemActive()) {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge(k, -1);
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge(k, 1);
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
      a.setHint(std::to_string(nSl) + " " +
                i18n::tr("regions \xC2\xB7 click: select + play \xC2\xB7 drag a marker: move the region's start (Alt: no snap) \xC2\xB7 right-click a flag: rename, position \xC2\xB7 "
                         "arrows: nudge \xC2\xB7 wheel: zoom, Shift+wheel: scroll"));
    else if (hov)
      a.setHint(std::to_string(nSl) + " " +
                i18n::tr("regions \xC2\xB7 click: select + play \xC2\xB7 drag a marker (Alt: no snap) \xC2\xB7 double-click: add marker \xC2\xB7 right-click a marker: remove, a flag: "
                         "rename, position \xC2\xB7 arrows: nudge \xC2\xB7 wheel: zoom"));
    // Ruler: time ticks under the waveform (J-182: seconds or bars:beats; a click switches).
    {
      const float ry = waveB;
      iskin::bevel(sk, wx, ry, ww, rulerH, rgb(0x121A21), 0, 0);
      dl->AddLine(sk.at(wx, ry), sk.at(wx + ww, ry), theme::withAlpha(IM_COL32_WHITE, 0.06f));
      const ImVec2 r0 = sk.at(wx + 2, ry), r1 = sk.at(wx + ww - 2, ry + rulerH);
      traceRect("ruler", r0, r1);
      ctl(sk, "##ruler", wx + 2, ry, ww - 4, rulerH);
      if (smp) {
        const float fsz = std::max(kerf::kMinTextPx, sk.px(10));
        const double pxPerFrame = static_cast<double>(p1.x - p0.x) / std::max(1e-9, f1 - f0);
        dl->PushClipRect(r0, r1, true);
        auto tick = [&](double f, const char* label, bool major) {
          const float x = std::round(fx(f)) + 0.5f;
          dl->AddLine(ImVec2(x, r0.y), ImVec2(x, r0.y + sk.px(major ? 5 : 3)), major ? rgb(0x8A97A2) : rgb(0x5A6670));
          if (label) dl->AddText(a.fontMono, fsz, ImVec2(x + sk.px(3), r0.y + (r1.y - r0.y - fsz) * 0.5f), major ? rgb(0x9AA7B2) : rgb(0x7E8B96), label);
        };
        if (barsRuler && fpb > 0) {  // J-182: bars:beats (over the loop's beats; J-190: or its detected beats); ticks between the labels
          const double pxPerBeat = pxPerFrame * fpb;
          const double step = kerfmarks::rulerStepBeats(pxPerBeat, sk.px(54));
          // J-190: label steps of a bar or more are whole bars of the meter.
          const double barStep = step >= kerfmarks::kBeatsPerBar ? std::ceil(step / bpb) * bpb : step;
          const double minor = barStep >= bpb ? (pxPerBeat * bpb >= sk.px(6) ? static_cast<double>(bpb) : barStep) : step / 2;
          const double b0 = tg.beatAt(f0), b1 = tg.beatAt(f1);
          for (long long i = static_cast<long long>(std::ceil(b0 / minor - 1e-9)); static_cast<double>(i) * minor <= b1 + 1e-9; ++i) {
            const double beat = static_cast<double>(i) * minor;
            const bool labelled = std::fabs(beat / barStep - std::round(beat / barStep)) < 1e-6;
            const bool onBar = std::fabs(beat / bpb - std::round(beat / bpb)) < 1e-6;
            std::string lb;
            if (labelled) lb = barStep >= bpb ? std::to_string(static_cast<long long>(std::floor(beat / bpb + 1e-6)) + 1)
                               : kerfmarks::barsBeats(tg.frameAt(beat), tg, step < 1);
            tick(tg.frameAt(beat), labelled ? lb.c_str() : nullptr, onBar);
          }
        } else {
          const double t0 = f0 / rate, t1 = f1 / rate;
          const double step = kerfmarks::rulerStepSeconds(pxPerFrame * rate, sk.px(54));
          for (long long i = static_cast<long long>(std::ceil(t0 / step - 1e-9)); static_cast<double>(i) * step <= t1 + 1e-9; ++i) {
            const double tt = static_cast<double>(i) * step;
            char b[24];
            if (step < 0.01) std::snprintf(b, sizeof b, "%.3f", tt);
            else if (step < 0.1) std::snprintf(b, sizeof b, "%.2f", tt);
            else if (step < 1) std::snprintf(b, sizeof b, "%.1f", tt);
            else std::snprintf(b, sizeof b, "%.0f s", tt);
            tick(tt * rate, b, false);
          }
        }
        dl->PopClipRect();
        // A click on the ruler switches its unit (also in the View menu and the RULER field).
        ImGui::SetCursorScreenPos(r0);
        if (ImGui::InvisibleButton("##ruler", ImVec2(r1.x - r0.x, r1.y - r0.y)))
          a.doc.setParam(t.pid(SlRuler), barsRuler ? kerfmarks::RulerSeconds : kerfmarks::RulerBars, false);
        if (ImGui::IsItemHovered())
          a.setHint(barsRuler ? i18n::tr("Ruler in bars and beats (4/4 over the loop's beats): click for seconds")
                              : i18n::tr("Ruler in seconds: click for bars and beats"));
      }
    }
    // Overview scroller on top: the whole loop with the visible part; drag to scroll, arrows step.
    {
      iskin::bevel(sk, wx, wy, ww, scrollH, rgb(0x1B232A), 0, 0);
      dl->AddLine(sk.at(wx, wy + scrollH), sk.at(wx + ww, wy + scrollH), IM_COL32(0, 0, 0, 140));
      const double wv = ui.goTo - ui.goFrom;
      if (kerf::flatButton(a, sk, "##ovl", 0, wy, 12, scrollH, ui::Icon::ChevronLeft, "Scroll left", false) && smp) setView(ui.goFrom - wv * 0.25, ui.goTo - wv * 0.25);
      if (kerf::flatButton(a, sk, "##ovr", ww - 12, wy, 12, scrollH, ui::Icon::Chevron, "Scroll right", false) && smp) setView(ui.goFrom + wv * 0.25, ui.goTo + wv * 0.25);
      const ImVec2 q0 = sk.at(14, wy + 1), q1 = sk.at(ww - 14, wy + scrollH - 1);
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
        const double c = (io.MousePos.x - q0.x) / std::max(1.0f, q1.x - q0.x);
        jumpView(c - wv * 0.5, c + wv * 0.5);
      }
      if (ImGui::IsItemHovered() && smp) {
        a.setHint(i18n::tr("Drag to scroll the wave view, wheel to zoom"));
        if (io.MouseWheel != 0) zoom(io.MouseWheel > 0 ? 0.8 : 1.25, (ui.goFrom + ui.goTo) * 0.5);
      }
      ctl(sk, "##scroll", 14, wy, ww - 28, scrollH);
    }
    // Switch row: options, hand edit | SNAP size, then-zero-crossing | NAME, POS of the selected marker | grid lines, show region.
    {
      iskin::bevel(sk, wx, rowY, ww, 540 - rowY, rgb(0x161E26), 0, 0);
      dl->AddLine(sk.at(wx, rowY), sk.at(wx + ww, rowY), IM_COL32(0, 0, 0, 160));
      const float ry = rowY + 2, rh = 20;
      if (kerf::deckButton(a, sk, "##wopts", 4, ry, 22, rh, ui::Icon::ChevronDown, "Wave view options")) ImGui::OpenPopup("##kwopts");
      ctl(sk, "##wopts", 4, ry, 22, rh);
      if (ImGui::BeginPopup("##kwopts")) {
        if (ImGui::MenuItem(i18n::tr("Ruler in seconds"), nullptr, !barsRuler)) a.doc.setParam(t.pid(SlRuler), kerfmarks::RulerSeconds, false);
        if (ImGui::MenuItem(i18n::tr("Ruler in bars and beats"), nullptr, barsRuler)) a.doc.setParam(t.pid(SlRuler), kerfmarks::RulerBars, false);
        if (ImGui::MenuItem(i18n::tr("Beat grid lines"), nullptr, gridShown)) a.doc.setParam(t.pid(SlGridLines), gridShown ? 0 : 1, false);
        ImGui::Separator();
        const double mid = (ui.goFrom + ui.goTo) * 0.5;
        if (ImGui::MenuItem(i18n::tr("Zoom in"))) zoom(0.5, mid);
        if (ImGui::MenuItem(i18n::tr("Zoom out"))) zoom(2.0, mid);
        if (ImGui::MenuItem(i18n::tr("Show the whole loop"))) setView(0, 1);
        if (ImGui::MenuItem(i18n::tr("Show the selected region"), nullptr, false, nSl > 0 && smp)) fitRegion(k);
        ImGui::EndPopup();
      }
      ImGui::BeginDisabled(rv.on || !smp);
      if (kerf::iconSwitch(a, sk, "##whand", 30, ry, 22, rh, ui::Icon::ToolDraw, "Edit the markers by hand", manual)) makeManual(a, t, st, starts, smp.get());
      ImGui::EndDisabled();
      kerf::text(sk, 58, ry, 30, rh, "SNAP", Role::Eyebrow, rgb(0x7F8B95), 0);
      {
        static const char* const shortNames[kerfmarks::SnapCount] = {i18n::N_("Off"), i18n::N_("Zero crossing"), i18n::N_("Bar"), i18n::N_("1/2 bar"),
                                                                     i18n::N_("Beat"), "1/2", "1/3", "1/4", "1/6", "1/8"};
        iskin::lcd(sk, 90, ry, 100, rh, "", rgb(0x262D33), sk.c.lcdText, 10);
        const std::string sn = std::string(i18n::tr(shortNames[snap])) + (snap >= kerfmarks::SnapBeat + 1 ? " " + std::string(i18n::tr("beat")) : std::string());
        kerf::text(sk, 95, ry, 82, rh, kerf::fit(sk, sn, Role::Value, 82).c_str(), Role::Value, snap ? rgb(0xFFD49A) : rgb(0xA4AEB7), 0);
        iskin::glyphArrow(dl, sk.at(183, ry + rh * 0.5f), sk.px(3.2f), rgb(0xA4AEB7), 3);
        ImGui::SetCursorScreenPos(sk.at(90, ry));
        if (ImGui::InvisibleButton("##snaplcd", ImVec2(sk.px(100), sk.px(rh)))) ImGui::OpenPopup("##ksnap");
        if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Snap: where moved and added markers land (hold Alt while dragging: no snapping)"));
        ctl(sk, "##snaplcd", 90, ry, 100, rh);
      }
      ImGui::BeginDisabled(!kerfmarks::isGrid(snap));
      if (kerf::iconSwitch(a, sk, "##snapzero", 194, ry, 22, rh, ui::Icon::ZeroCross, "Then the nearest zero crossing", snapZero))
        a.doc.setParam(t.pid(SlSnapZero), snapZero ? 0 : 1, false);
      ImGui::EndDisabled();
      kerf::text(sk, 226, ry, 32, rh, "NAME", Role::Eyebrow, rgb(0x7F8B95), 0);
      if (field(sk, "##namefld", 260, ry, 140, rh, nSl > 0 ? regionName(k) : std::string(), nSl > 0 ? i18n::N_("Name this region") : "", false, canRename(k),
                i18n::N_("Click: name the region (shown on its flag)")))
        beginRename(k);
      kerf::text(sk, 408, ry, 28, rh, "POS", Role::Eyebrow, rgb(0x7F8B95), 0);
      if (field(sk, "##posfld", 438, ry, 108, rh, canMove(k) ? posText(static_cast<double>(starts[static_cast<size_t>(k)])) : std::string(),
                nSl > 0 ? i18n::N_("Fixed at the start") : "", true, canMove(k), i18n::N_("Click: type the marker's exact position")))
        beginPosition(k);
      if (kerf::iconSwitch(a, sk, "##wgrid", 664, ry, 22, rh, ui::Icon::DisplayBars, "Beat grid lines", gridShown))
        a.doc.setParam(t.pid(SlGridLines), gridShown ? 0 : 1, false);
      if (kerf::deckButton(a, sk, "##wshow", 688, ry, 22, rh, ui::Icon::ScrollTo, "Show the selected region") && nSl > 0) fitRegion(k);
      ctl(sk, "##wshow", 688, ry, 22, rh);
    }
  }

  // ======================================================================= keyboard
  // Base note (left) and two rows of pads like the reference: black-key pads over white-key pads.
  if (shown[BKeys]) {
    const iskin::Skin sk = bandSkin(BKeys);
    iskin::bevel(sk, 0, 540, kSxW, 54, rgb(0x12191E), 0, 0);
    sk.dl()->AddLine(sk.at(0, 540), sk.at(kSxW, 540), IM_COL32(0, 0, 0, 160));
    ImGui::BeginDisabled(rv.on);
    iskin::digi(a, t, sk, SlBaseNote, 4, 543, 22, 48, [](double v) { return ed::noteName(static_cast<int>(std::lround(v))); }, 9.5f);
    ImGui::EndDisabled();
    ctl(sk, "##basenote", 4, 543, 22, 48);
    bool litKeys[128] = {};
    for (int s = 0; s < nSl; ++s) {
      int kk = sliceKey(s);
      if (kk >= 0 && kk < 128) litKeys[kk] = true;
    }
    // The pads start an octave under the selected region's key (or the base note)
    // (moved only when that key is out of sight; 35 white pads = 60 keys).
    const int focus = nSl > 0 && sliceKey(k) >= 0 ? sliceKey(k) : base;
    if (ui.kbLow < 0 || focus < ui.kbLow || focus >= ui.kbLow + 60) ui.kbLow = std::clamp((focus / 12) * 12 - 12, 0, 60);
    int hoverNote = -1;
    float glow[128] = {};  // J-186: the pads of the regions sounding now
    for (int s = 0; s < nSl; ++s) {
      const int kk = sliceKey(s);
      if (kk >= 0 && kk < 128) glow[kk] = std::max(glow[kk], play.light(s));
    }
    const int clicked = kerf::padKeyboard(a, t, sk, 30, 543, 680, 48, ui.kbLow, 35, litKeys, sliceKey(k), &hoverNote, glow);
    if (clicked >= 0)
      for (int s = 0; s < nSl; ++s)
        if (sliceKey(s) == clicked) {
          selectRegion(s);
          break;
        }
    if (hoverNote >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ui.kbMenuKey = hoverNote;
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
  // ======================================================================= J-182: name and position entry
  if (ui.openRename) ImGui::OpenPopup("##kname"), ui.openRename = false;
  if (ui.openPos) ImGui::OpenPopup("##kpos"), ui.openPos = false;
  if (ImGui::BeginPopup("##kname")) {
    const int s = ui.edit;
    if (!canRename(s)) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextDisabled("%s %d", i18n::tr("Name of region"), s + 1);
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(std::max(220.0f, root.px(240)));
      const bool enter = ImGui::InputText("##nm", ui.nameBuf, sizeof ui.nameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      traceRect("nameinput", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      if (classic && !manual) ImGui::TextDisabled("%s", i18n::tr("Naming keeps the cuts as hand-set markers"));
      if (enter || ImGui::Button(i18n::tr("Rename"))) {
        renameRegion(s, ui.nameBuf);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(i18n::tr("Cancel"))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##kpos")) {
    const int s = ui.edit;
    if (!canMove(s)) {
      ImGui::CloseCurrentPopup();
    } else {
      const int unit = ui.posUnit < 0 ? rulerUnit : ui.posUnit;
      const double cur = static_cast<double>(starts[static_cast<size_t>(s)]);
      ImGui::TextDisabled("%s %d", i18n::tr("Marker of region"), s + 1);
      static const char* const units[kerfmarks::UnitCount] = {i18n::N_("Seconds"), i18n::N_("Samples"), i18n::N_("Bars:beats:ticks")};
      for (int u = 0; u < kerfmarks::UnitCount; ++u) {
        if (u) ImGui::SameLine();
        ImGui::BeginDisabled(u == kerfmarks::UnitBars && !(fpb > 0));
        if (ImGui::RadioButton(i18n::tr(units[u]), unit == u) && unit != u) {
          ui.posUnit = u, ui.posBad = false;
          std::snprintf(ui.posBuf, sizeof ui.posBuf, "%s", kerfmarks::formatPosition(cur, u, rate, tg).c_str());
        }
        ImGui::EndDisabled();
      }
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(std::max(220.0f, root.px(240)));
      const bool enter = ImGui::InputText("##pos", ui.posBuf, sizeof ui.posBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      traceRect("posinput", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      if (ImGui::IsItemEdited()) ui.posBad = false;
      if (ui.posBad) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", i18n::tr("Not a position in this unit"));
      else ImGui::TextDisabled("%s", unit == kerfmarks::UnitBars ? i18n::tr("e.g. 2:3:048 (bar, beat, tick of 96)") : unit == kerfmarks::UnitSamples ? i18n::tr("e.g. 22050") : i18n::tr("e.g. 1.250"));
      if (enter || ImGui::Button(i18n::tr("Move"))) {
        double f = 0;
        if (kerfmarks::parsePosition(ui.posBuf, unit, rate, tg, f) && f <= frames) {
          moveMarker(s, std::llround(f));  // exactly there (typed positions are not snapped)
          ImGui::CloseCurrentPopup();
        } else {
          ui.posBad = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(i18n::tr("Cancel"))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (trace && !traceLine.empty()) {  // J-181 dev check: control places (printed when they change)
    static std::string last;
    char b[640];
    const std::string selName = nSl > 0 ? regionName(k) : std::string();
    std::string litList;  // J-186: the regions lit now (1-based, as numbered on their flags)
    for (int s = 0; s < nSl; ++s)
      if (play.lit(s)) litList += (litList.empty() ? "" : ",") + std::to_string(s + 1);
    if (litList.empty()) litList = "-";
    long long startSum = 0;  // J-182: the markers as a whole (an undone edit brings the sum back)
    for (int64_t f : starts) startSum += static_cast<long long>(f);
    std::snprintf(b, sizeof b,
                  " form=%.0f,%.0f,%.0f,%.0f tab=%d display=%d regions=%d snap=%d snapzero=%d rulerunit=%d grid=%d fpb=%.3f sel=%d selframe=%lld lastadd=%lld "
                  "startsum=%lld name=[%s] lit=%s",
                  static_cast<double>(root.o.x), static_cast<double>(root.o.y), static_cast<double>(root.at(kSxW, formH).x),
                  static_cast<double>(root.at(kSxW, formH).y), ui.tab, ui.display, nSl, snap, snapZero ? 1 : 0, barsRuler ? 1 : 0, gridShown ? 1 : 0, fpb, k,
                  static_cast<long long>(nSl > 0 ? starts[static_cast<size_t>(k)] : -1), static_cast<long long>(ui.lastAdd), startSum, selName.c_str(), litList.c_str());
    // J-190: the beat grid (mode, source 0 detected / 1 loop length, analysing, tempo, meter) and every slice start.
    char g[160];
    std::snprintf(g, sizeof g, " slmode=%d gridsrc=%d analysing=%d gridbpm=%.3f gridbpb=%d gridcell=%d starts=", ed::ival(st, SlMode),
                  kg ? kg->source : -1, kg && kg->analysing ? 1 : 0, kg ? kg->bpm : 0.0, kg ? kg->beatsPerBar : 0, ed::ival(st, SlGridDiv));
    std::string startList;
    for (int64_t f : starts) startList += (startList.empty() ? "" : ",") + std::to_string(static_cast<long long>(f));
    const std::string line = "kerf-ui:" + traceLine + b + g + (startList.empty() ? std::string("-") : startList);
    if (line != last) {
      last = line;
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
    }
  }
  if (kerf::auditOn()) {  // J-183 dev check: the layout audit (printed when it changes)
    static std::string last;
    char b[96];
    std::snprintf(b, sizeof b, " scale=%.3f display=%d keys=%d tab=%d ", static_cast<double>(root.s), ui.display, ui.keys ? 1 : 0, ui.tab);
    const std::string line = kerf::auditReport() + b;
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
