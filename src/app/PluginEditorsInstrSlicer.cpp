// Gridline — Slicer editor in the reference region-slicer layout (J-2, J-176).
// Reference form 714 × 625, laid out as that form's bands (see editorSlicex):
// top band (master sliders, X/Y pad, options, logo), region panel (region
// selector, per-region wheels, filter, envelope / selected region), deck bar
// (transport, menus, tools, sample info, view buttons), the wave view with the
// region flags and a time scroller, and the keyboard.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include "../engine/SlicexEngine.h"
#include "../core/I18n.h"
#include "../core/PluginBrand.h"
#include "../core/PluginParams.h"
#include "../engine/Slicing.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"
#include "RunLatch.h"
#include "../core/Platform.h"

namespace gl {

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width);

namespace {

using namespace pp;
using iskin::lerp;
using iskin::rgb;
namespace fs = std::filesystem;

constexpr float kSxW = 714, kSxH = 625;

// J-110: the deck's Play (one region played at a time, whichever instance).
struct SlicerPlay {
  RunLatch latch;
  ObjectId ch = kNoId;
  int key = -1;
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
// region came to its end: off, and its key is let go.
void syncSlicerPlay(App& a) {
  SlicerPlay& pl = slicerPlay();
  const RunLatch::End end = pl.latch.sync(a.engine, slicerSounding(a, pl.ch));
  if (end == RunLatch::End::Finished && pl.key >= 0) a.engine.noteOff(pl.ch, pl.key);
  if (end != RunLatch::End::None) pl.key = -1;
}

iskin::Palette slicexPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x18232F);
  p.bgBottom = rgb(0x111A23);
  p.panel = rgb(0x41474B);
  p.panelLine = rgb(0x0E1419);
  p.rail = rgb(0x3F484D);
  p.fill = rgb(0xFFA830);
  p.fill2 = rgb(0x989EA7);
  p.fill3 = rgb(0xF5781B);
  p.cap = rgb(0x4A5257);
  p.handle = rgb(0x30373B);
  p.label = rgb(0xA7B0B9);
  p.text = rgb(0xE9EDFA);
  p.textDim = rgb(0xACB4BA);
  p.lcdBg = rgb(0x6E7186);
  p.lcdText = rgb(0xE9EDFA);
  p.led = rgb(0xFFA830);
  p.ledOff = rgb(0x2A3136);
  return p;
}

struct SlicerUi {
  int slice = 0;
  int drag = -1;   // marker being dragged (slice index)
  double viewFrom = 0, viewTo = 1;  // J-176: the wave view (fractions of the sample)
  int kbLow = -1;                   // J-176: the keyboard's lowest key (follows the selected region)
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

// J-176: a part of the form that does nothing in the current engine: dimmed
// (its controls are disabled by the caller with ImGui::BeginDisabled).
void dimArea(const iskin::Skin& sk, float x, float y, float w, float h, uint32_t under) {
  sk.dl()->AddRectFilled(sk.at(x, y), sk.at(x + w, y + h), rgb(under, 0.62f));  // `under`: the colour beneath
}

// J-176: a deck-bar tool button (24 × 34) with a glyph; returns true when clicked.
bool toolBtn(App& a, const iskin::Skin& sk, const char* id, float x, ui::Icon icon, const char* hint, bool lit = false) {
  const bool hit = iskin::button(sk, id, x, 298, 24, 34, "", lit, 8);
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  ui::drawIconFit(sk.dl(), icon, sk.at(x + 5, 308), sk.at(x + 19, 322), lit ? rgb(0x293136) : ImGui::GetStyle().Alpha < 0.99f ? sk.c.textDim : sk.c.text);
  if (hov) a.setHint(hint);
  return hit;
}

// J-176: a small view button (18 × 21) on the deck bar's lower row.
bool viewBtn(App& a, const iskin::Skin& sk, const char* id, float x, ui::Icon icon, const char* hint) {
  const bool hit = iskin::button(sk, id, x, 339, 18, 21, "", false, 8);
  if (ImGui::IsItemHovered()) a.setHint(hint);
  ui::drawIconFit(sk.dl(), icon, sk.at(x + 3, 343), sk.at(x + 15, 356), sk.c.text);
  return hit;
}

// Layout (J-176, the reference region slicer's form, 714 × 625):
//   top band (0..99): master sliders, X/Y pad + wheels, slicing mode and options,
//     detection wheels, logo;
//   region panel (99..296): region menu button and region selector (14, 131),
//     key / cut group row (157), region wheels (46 / 72 on rows 185..269) with
//     the toggles beside them (121), filter (175..331), envelope or region
//     view (338, 107, 367 × 185: two selector strips, the view, its row);
//   deck bar (296..362): transport (38 / 67), menu buttons (160..), tools
//     (427..707), sample info row (345) with the view buttons (621..707);
//   wave view with the region flags (362..571): flags strip, waveform, time
//     scroller (19 px);
//   keyboard (571..625) with the base note selector.
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
  // The view (fractions of the sample) keeps a sane width.
  if (!(ui.viewTo - ui.viewFrom > 1e-6) || ui.viewFrom < 0 || ui.viewTo > 1) ui.viewFrom = 0, ui.viewTo = 1;
  auto setView = [&](double from, double to) {
    const double minW = smp && smp->frames() > 64 ? 64.0 / frames : 0.001;
    double w = std::clamp(to - from, minW, 1.0);
    from = std::clamp(from, 0.0, 1.0 - w);
    ui.viewFrom = from, ui.viewTo = from + w;
  };
  auto zoom = [&](double factor, double around) {  // around: fraction of the sample kept in place
    const double w = (ui.viewTo - ui.viewFrom) * factor;
    const double rel = (around - ui.viewFrom) / std::max(1e-12, ui.viewTo - ui.viewFrom);
    setView(around - rel * w, around - rel * w + w);
  };
  auto showRegion = [&](int s) {  // scrolls the region into view (zoom kept unless it does not fit)
    if (s < 0 || s >= nSl || !smp) return;
    const double a0 = static_cast<double>(starts[static_cast<size_t>(s)]) / frames, a1 = sliceEnd(s) / frames;
    if (a0 >= ui.viewFrom && a1 <= ui.viewTo) return;
    const double w = std::max(ui.viewTo - ui.viewFrom, (a1 - a0) * 1.2);
    setView((a0 + a1) * 0.5 - w * 0.5, (a0 + a1) * 0.5 + w * 0.5);
  };
  auto selectRegion = [&](int s) {
    ui.slice = std::clamp(s, 0, std::max(0, nSl - 1));
    showRegion(ui.slice);
  };

  iskin::Skin sk = iskin::begin(width, kSxW, kSxH, slicexPalette());
  ImDrawList* dl = sk.dl();
  iskin::background(sk, 0, 0, kSxW, kSxH, sk.c.bgTop, sk.c.bgBottom, 4);
  iskin::bevel(sk, 0, 0, kSxW, 4, rgb(0xFF8000), 0, 0);  // divider strip
  const ImU32 capDark = rgb(0x2C3236);

  // ---- top band: master sliders, X/Y pad, options, logo.
  struct Sl {
    int param;
    float x;
    const char* cap;
    bool centered, classicOnly;
  };
  const Sl sliders[4] = {{rv.on ? SlMaster : SlVolume, 22, "LEVEL", false, false},
                         {SlFine, 46, "FINE", true, true},
                         {SlPan, 71, "PAN", true, true},
                         {SlPitch, 95, "PITCH", true, false}};
  for (const Sl& s : sliders) {
    ImGui::BeginDisabled(rv.on && s.classicOnly);
    iskin::slider(a, t, sk, s.param, s.x, 7, 20, 73, sk.c.fill, s.centered);
    ImGui::EndDisabled();
    iskin::text(sk, s.x - 6, 82, 32, 10, s.cap, 7.5f, sk.c.label, 1, false, true);
    if (rv.on && s.classicOnly) dimArea(sk, s.x - 2, 6, 24, 87, 0x18232F);
  }
  {  // X/Y modulation pad
    iskin::bevel(sk, 132, 9, 77, 77, rgb(0x33393E), sk.c.panelLine, 2);
    const ImVec2 p0 = sk.at(132, 9), p1 = sk.at(209, 86);
    for (int i = 1; i < 4; ++i) {
      float fx = p0.x + (p1.x - p0.x) * i / 4.0f, fy = p0.y + (p1.y - p0.y) * i / 4.0f;
      dl->AddLine(ImVec2(fx, p0.y), ImVec2(fx, p1.y), theme::withAlpha(IM_COL32_WHITE, 0.05f));
      dl->AddLine(ImVec2(p0.x, fy), ImVec2(p1.x, fy), theme::withAlpha(IM_COL32_WHITE, 0.05f));
    }
    const float mx = ed::val(st, SlModX, 0.5f), my = ed::val(st, SlModY, 0.5f);
    ImVec2 c(p0.x + (p1.x - p0.x) * mx, p1.y - (p1.y - p0.y) * my);
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), rgb(0xB9BFC3, 0.5f));
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), rgb(0xB9BFC3, 0.5f));
    dl->AddCircleFilled(c, sk.px(4), rgb(0xB9BFC3));
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##xy", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemActive()) {
      ImVec2 m = ImGui::GetIO().MousePos;
      a.doc.setParam(t.pid(SlModX), std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f), true);
      a.doc.setParam(t.pid(SlModY), std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.0f, 1.0f), true);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Modulation X (filter cutoff) / Y (resonance): drag");
    iskin::wheel(a, t, sk, SlModX, 215, 33, 24, rgb(0x959EA6));
    iskin::text(sk, 241, 37, 17, 15, "X", 11, rgb(0x959EA6), 0, false, true);
    iskin::wheel(a, t, sk, SlModY, 215, 62, 24, rgb(0x959EA6));
    iskin::text(sk, 241, 67, 17, 15, "Y", 11, rgb(0x959EA6), 0, false, true);
  }
  // Slicing mode + options (classic engine); the region engine plays the loop's own regions.
  iskin::text(sk, 345, 11, 76, 15, "SLICING", 8.5f, sk.c.label, 2, false, true);
  if (rv.on) {
    iskin::lcd(sk, 424, 9, 132, 19, "Regions of the loop", sk.c.lcdBg, sk.c.lcdText, 9);
    if (ImGui::IsMouseHoveringRect(sk.at(424, 9), sk.at(556, 28))) a.setHint("The loop brought its own regions: they play as they are stored");
  } else {
    iskin::digi(a, t, sk, SlMode, 424, 9, 132, 19, {}, 10);
  }
  ImGui::BeginDisabled(rv.on);
  iskin::check(a, t, sk, SlMono, 345, 35, 70, 16, "MONO", 8.5f);
  iskin::check(a, t, sk, SlDeclick, 420, 35, 70, 16, "DECLICK", 8.5f);
  iskin::check(a, t, sk, SlHold, 495, 35, 62, 16, "HOLD", 8.5f);
  iskin::check(a, t, sk, SlPlayToEnd, 345, 53, 90, 16, "PLAY TO END", 8.5f);
  iskin::check(a, t, sk, SlReverse, 440, 53, 80, 16, "REVERSE", 8.5f);
  iskin::check(a, t, sk, SlFitTempo, 345, 71, 90, 16, "FIT TEMPO", 8.5f);
  iskin::digi(a, t, sk, SlLoopBeats, 440, 71, 54, 16, [](double v) {
    char b[24];
    std::snprintf(b, sizeof b, "%.0f beats", v);
    return std::string(b);
  }, 8.5f);
  ImGui::EndDisabled();
  if (rv.on) dimArea(sk, 343, 32, 222, 58, 0x18232F);
  // Detection (automatic slicing) and velocity.
  {
    const bool detect = !rv.on && ed::ival(st, SlMode) != SlModeManual;
    ImGui::BeginDisabled(!detect);
    iskin::wheel(a, t, sk, SlSensitivity, 576, 61, 26, sk.c.fill);
    iskin::wheel(a, t, sk, SlMinGap, 608, 61, 26, sk.c.fill);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(rv.on);
    iskin::wheel(a, t, sk, SlVelocity, 670, 61, 26, sk.c.fill);
    ImGui::EndDisabled();
    iskin::text(sk, 570, 88, 38, 9, "SENS", 7.5f, sk.c.label, 1, false, true);
    iskin::text(sk, 602, 88, 38, 9, "GAP", 7.5f, sk.c.label, 1, false, true);
    iskin::text(sk, 664, 88, 38, 9, "VEL", 7.5f, sk.c.label, 1, false, true);
    if (!detect) dimArea(sk, 572, 59, 66, 40, 0x18232F);
    if (rv.on) dimArea(sk, 664, 59, 40, 40, 0x18232F);
  }
  if (!pui::titleWordmarkIn(sk.dl(), "gl.slicer", sk.at(589, 12), sk.at(589 + 118, 12 + 40), sk.px(26), 2, sk.c.text))  // J-136: the wordmark
    iskin::text(sk, 589, 12, 118, 40, pluginTitle("gl.slicer").c_str(), 26, rgb(0xF5781B), 2, false, true);

  // ---- region panel.
  iskin::bevel(sk, 0, 99, kSxW, 198, rgb(0x60686C), 0, 0);
  iskin::bevel(sk, 10, 106, 158, 189, rgb(0x5A6166), rgb(0x4A5156), 3);
  {
    // Region menu button and the region selector (a list of every region).
    if (iskin::button(sk, "##rmenu", 13, 110, 18, 19, "", false, 8)) ImGui::OpenPopup("##regionmenu");
    if (ImGui::IsItemHovered()) a.setHint("Region menu");
    ui::drawIconFit(dl, ui::Icon::Menu, sk.at(16, 113), sk.at(28, 126), sk.c.text);
    if (ImGui::BeginPopup("##regionmenu")) {
      if (ImGui::MenuItem(i18n::tr("Previous region"), nullptr, false, k > 0)) selectRegion(k - 1);
      if (ImGui::MenuItem(i18n::tr("Next region"), nullptr, false, k + 1 < nSl)) selectRegion(k + 1);
      if (ImGui::MenuItem(i18n::tr("Play region"), nullptr, false, nSl > 0 && sliceKey(k) >= 0)) audition(k);
      if (ImGui::MenuItem(i18n::tr("Show region"), nullptr, false, nSl > 0 && smp)) {
        const double a0 = static_cast<double>(starts[static_cast<size_t>(k)]) / frames, a1 = sliceEnd(k) / frames;
        const double pad = (a1 - a0) * 0.15;
        setView(a0 - pad, a1 + pad);
      }
      ImGui::EndPopup();
    }
    iskin::text(sk, 35, 110, 130, 19, "REGION", 7.5f, capDark, 0, false, true);
    std::string cur = nSl > 0 ? regionLabel(k) : std::string("(no regions)");
    iskin::lcd(sk, 14, 131, 150, 19, "", sk.c.lcdBg, sk.c.lcdText, 9);
    {  // text clipped to the field, arrow on the right
      const ImVec2 c0 = sk.at(18, 131), c1 = sk.at(152, 150);
      dl->PushClipRect(c0, c1, true);
      const float fs = std::max(8.0f, sk.px(9));
      const ImVec2 ts = a.fontMono ? a.fontMono->CalcTextSizeA(fs, FLT_MAX, 0, cur.c_str()) : ImVec2(0, fs);
      dl->AddText(a.fontMono, fs, ImVec2(c0.x, (c0.y + c1.y - ts.y) * 0.5f), sk.c.lcdText, cur.c_str());
      dl->PopClipRect();
      iskin::glyphArrow(dl, sk.at(158, 140.5f), sk.px(3.5f), sk.c.lcdText, 3);
    }
    ImGui::SetCursorScreenPos(sk.at(14, 131));
    ImGui::InvisibleButton("##rsel", ImVec2(sk.px(150), sk.px(19)));
    if (ImGui::IsItemClicked() && nSl > 0) ImGui::OpenPopup("##regionlist");
    if (ImGui::IsItemHovered()) {
      a.setHint("Current region: click for the list, mouse wheel to step");
      const float w = ImGui::GetIO().MouseWheel;
      if (w != 0 && nSl > 0) selectRegion(k + (w > 0 ? -1 : 1));
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
  }
  // Key and cut group (row 157), region wheels (rows 185 / 213 / 241 / 269), toggles beside them.
  auto rw = [&](int param, float x, float y, const char* cap, bool capLeft) {
    iskin::wheel(a, t, sk, param, x, y, 23, sk.c.fill);
    if (capLeft) iskin::text(sk, 12, y, x - 14, 23, cap, 7.5f, capDark, 2, false, true);
    else iskin::text(sk, x + 25, y, 26, 23, cap, 7.5f, capDark, 0, false, true);
  };
  if (hasParams || nSl == 0) {
  ImGui::BeginDisabled(nSl == 0);
  iskin::text(sk, 12, 157, 24, 23, "KEY", 7.5f, capDark, 2, false, true);
  iskin::digi(a, t, sk, slParam2(kp, SliceKey), 40, 158, 52, 21, [&](double v) {
    int n = static_cast<int>(std::lround(v));
    if (n < 0) return keyName(k) + "*";  // automatic: the key the region gets
    return ed::noteName(n);
  }, 9);
  iskin::text(sk, 96, 157, 23, 23, "CUT", 7.5f, capDark, 2, false, true);
  iskin::digi(a, t, sk, slParam2(kp, SliceCut), 121, 158, 27, 21, [](double v) {
    int n = static_cast<int>(std::lround(v));
    return n <= 0 ? std::string("-") : std::to_string(n);
  }, 9);
  rw(slParam(kp, SlicePan), 46, 185, "PAN", true);
  rw(slParam(kp, SliceGain), 72, 185, "VOL", false);
  rw(slParam(kp, SlicePitch), 72, 241, "PITCH", false);
  rw(slParam2(kp, SliceDelay), 46, 269, "DELAY", true);
  iskin::text(sk, 121, 232, 44, 9, "END", 7.5f, capDark, 0, false, true);
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
    iskin::text(sk, 14, 175, 150, 14, "This region plays with", 8, capDark, 1, false, true);
    iskin::text(sk, 14, 189, 150, 14, "its imported settings.", 8, capDark, 1, false, true);
    iskin::text(sk, 14, 215, 150, 14, "Regions 1 to 64 can", 8, capDark, 1, false, true);
    iskin::text(sk, 14, 229, 150, 14, "be edited here.", 8, capDark, 1, false, true);
  }
  // Filter (classic engine).
  ImGui::BeginDisabled(rv.on);
  iskin::bevel(sk, 175, 131, 156, 24, rgb(0x4E555A), 0, 3);
  iskin::text(sk, 175, 131, 156, 24, "FILTER", 11, rgb(0xA7B0B9), 1, false, true);
  iskin::text(sk, 180, 183, 60, 18, "TYPE", 8, capDark, 0, false, true);
  iskin::digi(a, t, sk, SlFilterType, 244, 183, 84, 18, {}, 9);
  iskin::wheel(a, t, sk, SlAttack, 191, 242, 36, sk.c.fill2);
  iskin::wheel(a, t, sk, SlCutoff, 231, 232, 46, sk.c.fill2);
  iskin::wheel(a, t, sk, SlReso, 281, 242, 36, sk.c.fill2);
  ImGui::EndDisabled();
  iskin::text(sk, 185, 280, 48, 10, "ATTACK", 7.5f, capDark, 1, false, true);
  iskin::text(sk, 226, 280, 56, 10, "CUTOFF", 7.5f, capDark, 1, false, true);
  iskin::text(sk, 275, 280, 48, 10, "RES", 7.5f, capDark, 1, false, true);
  if (rv.on) dimArea(sk, 173, 129, 160, 164, 0x60686C);
  // Envelope (classic) or the selected region (region engine): two strips, the view, a row.
  {
    const float ex = 338, ew = 367;
    iskin::bevel(sk, ex, 107, ew, 15, rgb(0x3A4146), 0, 2);
    iskin::bevel(sk, ex, 123, ew, 15, rgb(0x343B40), 0, 2);
    iskin::bevel(sk, ex, 139, ew, 120, rgb(0x0E1215), sk.c.panelLine, 2);
    const ImVec2 p0 = sk.at(ex + 5, 145), p1 = sk.at(ex + ew - 5, 253);
    if (rv.on) {
      iskin::text(sk, ex + 5, 107, ew - 10, 15, "SELECTED REGION", 8.5f, sk.c.text, 0, false, true);
      char info[128] = "-";
      if (nSl > 0 && smp && smp->sampleRate > 0) {
        const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]), s1 = sliceEnd(k);
        std::snprintf(info, sizeof info, "%d of %d  \xC2\xB7  from %.3f s  \xC2\xB7  %.3f s long  \xC2\xB7  key %s", k + 1, nSl, s0 / smp->sampleRate,
                      (s1 - s0) / smp->sampleRate, keyName(k).c_str());
      }
      iskin::text(sk, ex + 5, 123, ew - 10, 15, info, 8, sk.c.textDim, 0, true, false);
      if (nSl > 0 && smp) {
        const double s0 = static_cast<double>(starts[static_cast<size_t>(k)]) / frames, s1 = sliceEnd(k) / frames;
        dl->AddRectFilled(p0, p1, rgb(0x3A2A12, 0.55f));
        iskin::wave(dl, smp.get(), p0, p1, rgb(0xC9D6E2), s0, std::max(s0 + 1e-9, s1), theme::withAlpha(IM_COL32_WHITE, 0.08f));
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##rprev", ImVec2(p1.x - p0.x, p1.y - p0.y));
        if (ImGui::IsItemClicked()) audition(k);
        if (ImGui::IsItemHovered()) a.setHint("Click: play the region");
      }
      // Row: step through the regions, the region's name.
      if (iskin::button(sk, "##rpv", ex, 263, 24, 26, "", false, 8) && k > 0) selectRegion(k - 1);
      iskin::glyphArrow(dl, sk.at(ex + 12, 276), sk.px(5), sk.c.text, 0);
      if (iskin::button(sk, "##rnx", ex + 26, 263, 24, 26, "", false, 8) && k + 1 < nSl) selectRegion(k + 1);
      iskin::glyphArrow(dl, sk.at(ex + 38, 276), sk.px(5), sk.c.text, 1);
      const std::string nm = nSl > 0 ? regionName(k) : std::string();
      iskin::text(sk, ex + 58, 263, ew - 62, 26, nm.empty() ? "(unnamed region)" : nm.c_str(), 9, nm.empty() ? sk.c.textDim : sk.c.text, 0, false, true);
    } else {
      iskin::text(sk, ex + 5, 107, ew - 10, 15, "AMP ENVELOPE", 8.5f, sk.c.text, 0, false, true);
      iskin::text(sk, ex + 5, 123, ew - 10, 15, "attack  \xC2\xB7  slice (fade in / out)  \xC2\xB7  release", 8, sk.c.textDim, 0, false, false);
      const double at = ed::val(st, SlAttack, 0.001f), re = ed::val(st, SlRelease, 0.01f), fi = ed::val(st, SlFadeIn), fo = ed::val(st, SlFadeOut);
      // Width shares: attack and release grow with their times (so short ones stay visible).
      const double fa = 0.06 + 0.22 * std::sqrt(std::clamp(at / 0.5, 0.0, 1.0));
      const double fr = 0.06 + 0.22 * std::sqrt(std::clamp(re / 1.0, 0.0, 1.0));
      const double fb = 1.0 - fa - fr;
      auto X = [&](double u) { return p0.x + static_cast<float>(u) * (p1.x - p0.x); };
      auto Y = [&](double v) { return p1.y - static_cast<float>(v) * (p1.y - p0.y); };
      std::vector<ImVec2> pts;
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
        pts.push_back(ImVec2(X(u), Y(std::clamp(v, 0.0, 1.0))));
      }
      for (size_t i = 1; i < pts.size(); ++i)
        dl->AddQuadFilled(ImVec2(pts[i - 1].x, p1.y), pts[i - 1], pts[i], ImVec2(pts[i].x, p1.y), rgb(0xFFA830, 0.12f));
      dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), sk.c.fill, 0, std::max(1.2f, sk.px(1.6f)));
      dl->AddLine(ImVec2(X(fa), p0.y), ImVec2(X(fa), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.14f));
      dl->AddLine(ImVec2(X(fa + fb), p0.y), ImVec2(X(fa + fb), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.14f));
      const struct {
        int p;
        const char* c;
      } ws[4] = {{SlRelease, "RELEASE"}, {SlFadeIn, "FADE IN"}, {SlFadeOut, "FADE OUT"}, {SlChromatic, "CHROMATIC"}};
      for (int i = 0; i < 4; ++i) {
        iskin::wheel(a, t, sk, ws[i].p, ex + 4 + i * 92, 265, 22, sk.c.fill);
        iskin::text(sk, ex + 29 + i * 92, 265, 62, 22, ws[i].c, 7.5f, capDark, 0, false, true);
      }
    }
  }

  // ---- deck bar: transport, menus, tools, sample info and view buttons.
  iskin::bevel(sk, 0, 296, kSxW, 66, rgb(0x293136), 0, 0);
  {
    // Transport: play the selected region / stop. J-110: the button follows
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
    if (iskin::button(sk, "##play", 38, 297, 31, 30, "", playing, 9) && t.isChannel() && nSl > 0 && (playing || sliceKey(k) >= 0)) {
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
    if (ImGui::IsItemHovered()) a.setHint("Play the selected region / stop");
    {
      ImVec2 c = sk.at(53.5f, 312);
      float r = sk.px(6);
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x + r, c.y + r), playing ? rgb(0x293136) : sk.c.text);
    }
    if (iskin::button(sk, "##stop", 69, 297, 31, 30, "", false, 9) && playing && t.isChannel()) stopPlay();
    static const bool trace = std::getenv("GRIDLINE_KERF_TRACE") != nullptr;
    if (trace) {  // J-110 dev check: the play button's place and state (printed when it changes)
      static std::string last;
      const ImVec2 pc = sk.at(53.5f, 312);
      char b[96];
      std::snprintf(b, sizeof b, "kerf: play %.0f %.0f on %d", static_cast<double>(pc.x), static_cast<double>(pc.y), pl.latch.on && pl.ch == t.owner ? 1 : 0);
      if (last != b) {
        last = b;
        std::printf("%s\n", b);
        std::fflush(stdout);
      }
    }
    ui::drawIconFit(dl, ui::Icon::Stop, sk.at(79, 307), sk.at(89, 317), sk.c.text);
    // Menus: file (load), regions (slicing), view (zoom).
    auto loadLoop = [&]() {
      PluginTarget tt = t;
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.title = "Load loop";
      const char* h = gl::platform::homeEnv();
      a.fileDialog.dir = st.data.empty() ? (h ? h : ".") : fs::path(slicex::sampleOf(st.data)).parent_path().string();
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
    const bool manual = !rv.on && ed::ival(st, SlMode) == SlModeManual;
    if (toolBtn(a, sk, "##mfile", 160, ui::Icon::Open, "Load a loop (or drop one on the wave view)")) loadLoop();
    if (toolBtn(a, sk, "##mregions", 192, ui::Icon::ToolSlice, "Regions: slicing")) ImGui::OpenPopup("##regionsmenu");
    if (ImGui::BeginPopup("##regionsmenu")) {
      const bool cl = !rv.on;
      if (ImGui::MenuItem(i18n::tr("Detect slices"), nullptr, cl && ed::ival(st, SlMode) == 0, cl)) setMode(0);
      if (ImGui::MenuItem(i18n::tr("Even slices: 8"), nullptr, cl && ed::ival(st, SlMode) == 2, cl)) setMode(2);
      if (ImGui::MenuItem(i18n::tr("Even slices: 16"), nullptr, cl && ed::ival(st, SlMode) == 3, cl)) setMode(3);
      if (ImGui::MenuItem(i18n::tr("Even slices: 32"), nullptr, cl && ed::ival(st, SlMode) == 4, cl)) setMode(4);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Edit markers by hand"), nullptr, manual, cl)) makeManual(a, t, st, starts, smp.get());
      if (ImGui::MenuItem(i18n::tr("Clear markers"), nullptr, false, manual)) clearMarkers();
      ImGui::EndPopup();
    }
    if (toolBtn(a, sk, "##mview", 224, ui::Icon::Zoom, "View: zoom")) ImGui::OpenPopup("##viewmenu");
    if (ImGui::BeginPopup("##viewmenu")) {
      const double mid = (ui.viewFrom + ui.viewTo) * 0.5;
      if (ImGui::MenuItem(i18n::tr("Zoom in"))) zoom(0.5, mid);
      if (ImGui::MenuItem(i18n::tr("Zoom out"))) zoom(2.0, mid);
      if (ImGui::MenuItem(i18n::tr("Show the whole loop"))) setView(0, 1);
      if (ImGui::MenuItem(i18n::tr("Show the selected region"), nullptr, false, nSl > 0 && smp)) {
        const double a0 = static_cast<double>(starts[static_cast<size_t>(k)]) / frames, a1 = sliceEnd(k) / frames;
        setView(a0 - (a1 - a0) * 0.15, a1 + (a1 - a0) * 0.15);
      }
      ImGui::EndPopup();
    }
    // Tools: hand editing, even slices, add marker, detection, clear (classic engine).
    if (rv.on) {
      iskin::text(sk, 427, 298, 280, 34, "Regions come from the loop: drag a marker to move a region", 8, sk.c.textDim, 1, false, false);
    } else {
      if (toolBtn(a, sk, "##hand", 427, ui::Icon::ToolDraw, "Edit the markers by hand (double-click the wave: add, right-click a marker: remove)", manual))
        makeManual(a, t, st, starts, smp.get());
      const struct {
        const char* id;
        const char* cap;
        float x;
        int mode;
      } ev[3] = {{"##ev8", "8", 459, 2}, {"##ev16", "16", 483, 3}, {"##ev32", "32", 507, 4}};
      for (const auto& e : ev) {
        if (iskin::button(sk, e.id, e.x, 298, 24, 34, e.cap, ed::ival(st, SlMode) == e.mode, 8.5f)) setMode(e.mode);
        if (ImGui::IsItemHovered()) a.setHint("Even slices");
      }
      if (toolBtn(a, sk, "##add", 579, ui::Icon::Flag, "Add a marker in the middle of the selected slice", false) && smp && nSl > 0) {
        if (!manual) makeManual(a, t, st, starts, smp.get());
        const double mid = (static_cast<double>(starts[static_cast<size_t>(k)]) + sliceEnd(k)) * 0.5 / frames;
        const PluginState* cur = t.state(a.doc.song());
        for (int q = 0; cur && q < kMaxSlices; ++q)
          if (ed::val(*cur, slMarker(q), -1) < 0) {
            a.doc.setParam(t.pid(slMarker(q)), static_cast<float>(mid), false);
            break;
          }
      }
      if (toolBtn(a, sk, "##auto", 603, ui::Icon::SliceWave, "Detect slices from the hits", ed::ival(st, SlMode) == 0)) setMode(0);
      ImGui::BeginDisabled(!manual);
      if (toolBtn(a, sk, "##clear", 659, ui::Icon::Trash, "Clear the markers")) clearMarkers();
      ImGui::EndDisabled();
    }
    // Lower row: rate, channels, length, name, region count; view buttons.
    char info[96];
    std::snprintf(info, sizeof info, "%s", smp ? (std::to_string(static_cast<int>(smp->sampleRate)) + "Hz").c_str() : "--");
    iskin::text(sk, 5, 345, 64, 15, info, 8.5f, sk.c.textDim, 0);
    iskin::text(sk, 70, 345, 46, 15, smp ? (smp->mono() ? "mono" : "stereo") : "", 8.5f, sk.c.textDim, 0);
    if (smp) std::snprintf(info, sizeof info, "%.2f s", smp->seconds());
    else std::snprintf(info, sizeof info, "--");
    iskin::text(sk, 118, 345, 60, 15, info, 8.5f, sk.c.textDim, 0);
    std::string nm = smp ? fs::path(slicex::sampleOf(st.data)).filename().string() : std::string("Drop a loop here or load one");
    dl->PushClipRect(sk.at(184, 340), sk.at(470, 362), true);
    iskin::text(sk, 184, 345, 286, 15, nm.c_str(), 9, sk.c.text, 0);
    dl->PopClipRect();
    std::snprintf(info, sizeof info, "%d regions", nSl);
    iskin::text(sk, 470, 345, 146, 15, info, 8.5f, sk.c.textDim, 2);
    const double mid = (ui.viewFrom + ui.viewTo) * 0.5;
    if (viewBtn(a, sk, "##zout", 639, ui::Icon::Minus, "Zoom out")) zoom(2.0, mid);
    if (viewBtn(a, sk, "##zin", 657, ui::Icon::Plus, "Zoom in")) zoom(0.5, mid);
    if (viewBtn(a, sk, "##zall", 675, ui::Icon::Zoom, "Show the whole loop")) setView(0, 1);
    if (viewBtn(a, sk, "##zreg", 693, ui::Icon::ScrollTo, "Show the selected region") && nSl > 0 && smp) {
      const double a0 = static_cast<double>(starts[static_cast<size_t>(k)]) / frames, a1 = sliceEnd(k) / frames;
      setView(a0 - (a1 - a0) * 0.15, a1 + (a1 - a0) * 0.15);
    }
  }

  // ---- wave view: region flags, waveform with the markers, time scroller.
  {
    const float wx = 0, wy = 362, ww = kSxW, wh = 209, flagH = 14, scrollH = 19;
    iskin::bevel(sk, wx, wy, ww, wh, rgb(0x0B1014), 0, 0);
    iskin::bevel(sk, wx, wy, ww, flagH, rgb(0x161E26), 0, 0);
    const ImVec2 top = sk.at(wx + 2, wy), p0 = sk.at(wx + 2, wy + flagH), p1 = sk.at(wx + ww - 2, wy + wh - scrollH - 1);
    const double f0 = ui.viewFrom * frames, f1 = ui.viewTo * frames;
    auto fx = [&](double f) { return p0.x + static_cast<float>((f - f0) / std::max(1e-9, f1 - f0)) * (p1.x - p0.x); };
    auto frameAt = [&](float x) { return f0 + std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * (f1 - f0); };
    const float labelPx = std::max(8.0f, sk.px(9));
    // Dense markers (hundreds of regions at once): drawn faint so the audio shows,
    // and grabbed for moving only once zoomed in far enough.
    int visible = 0;
    for (int s = 0; s < nSl; ++s) visible += static_cast<double>(starts[static_cast<size_t>(s)]) >= f0 && static_cast<double>(starts[static_cast<size_t>(s)]) <= f1;
    const float spacing = (p1.x - p0.x) / static_cast<float>(std::max(1, visible));
    const float markerAlpha = std::clamp(spacing / sk.px(12), 0.18f, 0.7f);
    const bool grabMarkers = spacing >= sk.px(6);
    dl->PushClipRect(top, ImVec2(p1.x, p1.y), true);
    for (int s = 0; s < nSl; ++s) {  // region shading (J-166: region engine: the region's own end)
      const double s0 = static_cast<double>(starts[static_cast<size_t>(s)]), s1 = sliceEnd(s);
      if (s1 < f0 || s0 > f1) continue;
      const bool muted = !rv.on && s < kMaxSlices && ed::bval(st, slParam(s, SliceMute));
      ImU32 c = s == k ? rgb(0x3A2A12) : muted ? rgb(0x2A1A1A) : (s % 2 ? rgb(0x141C24) : rgb(0x101820));
      dl->AddRectFilled(ImVec2(fx(s0), p0.y), ImVec2(fx(s1), p1.y), c);
    }
    iskin::wave(dl, smp.get(), p0, p1, rgb(0x7FA6C7), ui.viewFrom, ui.viewTo, theme::withAlpha(IM_COL32_WHITE, 0.08f));
    // Flags: number (and name) when they fit before the next region's flag.
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
      const float x = fx(s0), room = fx(next) - std::max(x, p0.x) - 5;
      std::string lbl = std::to_string(s + 1);
      const std::string nm = regionName(s);
      auto fits = [&](const std::string& tx) { return a.fontMono && a.fontMono->CalcTextSizeA(labelPx, FLT_MAX, 0, tx.c_str()).x <= room; };
      if (!nm.empty() && fits(lbl + " " + nm)) lbl += " " + nm;
      if (!fits(lbl)) lbl.clear();
      const ImU32 fc = s == k ? sk.c.fill : rgb(0xF5781B, std::min(1.0f, markerAlpha * 1.4f));
      dl->AddRectFilled(ImVec2(x, top.y + 1), ImVec2(x + std::min(sk.px(4), std::max(1.0f, spacing - 1)), p0.y - 1), fc);
      if (!lbl.empty()) dl->AddText(a.fontMono, labelPx, ImVec2(std::max(x, p0.x) + sk.px(6), top.y + (sk.px(flagH) - labelPx) * 0.5f), s == k ? sk.c.fill : sk.c.textDim, lbl.c_str());
    }
    const bool manual = ed::ival(st, SlMode) == SlModeManual && !rv.on;
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##wave", ImVec2(p1.x - top.x, p1.y - top.y));
    const bool hov = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    int hoverMarker = -1;
    const bool regionDrag = rv.on && ui.drag >= 0 && ImGui::IsItemActive();
    for (int s = 0; s < nSl; ++s) {
      float x = fx(static_cast<double>(starts[static_cast<size_t>(s)]));
      if (regionDrag && s == ui.drag) x = std::clamp(m.x, p0.x, p1.x);  // region engine: the marker follows the mouse
      if (x < p0.x - 1 || x > p1.x + 1) continue;
      const bool near = grabMarkers && hov && std::fabs(m.x - x) < 4 && (s > 0 || rv.on);
      if (near) hoverMarker = s;
      if (s != k) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), near ? sk.c.text : rgb(0xF5781B, markerAlpha), near ? 2.0f : 1.0f);
    }
    if (k < nSl) {  // the selected region's marker on top
      float x = fx(static_cast<double>(starts[static_cast<size_t>(k)]));
      if (regionDrag && k == ui.drag) x = std::clamp(m.x, p0.x, p1.x);
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), sk.c.fill, hoverMarker == k ? 2.0f : 1.0f);
    }
    dl->PopClipRect();
    if (hov && io.MouseWheel != 0 && smp) zoom(io.MouseWheel > 0 ? 0.8 : 1.25, frameAt(m.x) / frames);
    if (ImGui::IsItemActivated() && smp) {
      ui.drag = hoverMarker;
      if (ui.drag < 0) {
        const double f = frameAt(m.x);
        int s = -1;
        if (rv.on) {  // regions may overlap or be out of order: the latest start whose region holds the point
          for (int q = 0; q < nSl; ++q)
            if (static_cast<double>(starts[static_cast<size_t>(q)]) <= f && f < sliceEnd(q) &&
                (s < 0 || starts[static_cast<size_t>(q)] >= starts[static_cast<size_t>(s)]))
              s = q;
        } else {
          s = 0;
          while (s + 1 < nSl && static_cast<double>(starts[static_cast<size_t>(s + 1)]) <= f) ++s;
        }
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
      const int64_t to = std::llround(frameAt(m.x));
      if (std::fabs(fx(static_cast<double>(to)) - fx(static_cast<double>(starts[static_cast<size_t>(ui.drag)]))) >= 2.0f)
        moveRegionStart(a, t, st, rv, ui.drag, to, smp.get());
      else
        ui.slice = ui.drag;
    }
    if (ui.drag > 0 && manual && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1) && ui.drag < nSl) {
      int mp = markerParamAt(st, static_cast<double>(starts[static_cast<size_t>(ui.drag)]) / frames);
      if (mp < 0) {  // positions are stored as fractions: match the nearest marker
        double best = 1e9;
        for (int q = 0; q < kMaxSlices; ++q) {
          double v = ed::val(st, slMarker(q), -1);
          double d = std::fabs(v - static_cast<double>(starts[static_cast<size_t>(ui.drag)]) / frames);
          if (v >= 0 && d < best) best = d, mp = q;
        }
      }
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), static_cast<float>(frameAt(m.x) / frames), true);
    }
    if (ImGui::IsItemDeactivated()) {
      ui.drag = -1;
      a.doc.endGesture();
    }
    if (hov && !rv.on && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && smp) {
      if (!manual) makeManual(a, t, st, starts, smp.get());
      for (int q = 0; q < kMaxSlices; ++q)
        if (ed::val(st, slMarker(q), -1) < 0 || (!manual && q >= nSl)) {
          a.doc.setParam(t.pid(slMarker(q)), static_cast<float>(frameAt(m.x) / frames), false);
          break;
        }
    }
    if (hoverMarker > 0 && manual && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      int mp = -1;
      double best = 1e9;
      for (int q = 0; q < kMaxSlices; ++q) {
        double v = ed::val(st, slMarker(q), -1);
        double d = std::fabs(v - static_cast<double>(starts[static_cast<size_t>(hoverMarker)]) / frames);
        if (v >= 0 && d < best) best = d, mp = q;
      }
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), -1, false);
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          PluginState ns = st;
          ns.data = path;
          setPluginState(a, t, ns, "load loop");
          ui.viewFrom = 0, ui.viewTo = 1;
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (hov && rv.on)
      a.setHint(std::to_string(nSl) + " regions · click: select + play · drag a marker: move the region's start · wheel: zoom · drop a loop");
    else if (hov)
      a.setHint(std::to_string(nSl) + " regions · click: select + play · drag a marker · double-click: add marker · right-click a marker: remove · wheel: zoom · drop a loop");
    // Time scroller: the whole loop with the visible part; drag to scroll.
    {
      const float sy = wy + wh - scrollH;
      iskin::bevel(sk, wx, sy, ww, scrollH, rgb(0x1B232A), 0, 0);
      const ImVec2 q0 = sk.at(wx + 2, sy + 2), q1 = sk.at(wx + ww - 2, sy + scrollH - 2);
      iskin::wave(dl, smp.get(), q0, q1, rgb(0x55728A), 0, 1, 0);
      for (int s = 0; s < nSl; ++s) {
        const float x = q0.x + static_cast<float>(static_cast<double>(starts[static_cast<size_t>(s)]) / frames) * (q1.x - q0.x);
        dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q0.y + sk.px(4)), s == k ? sk.c.fill : rgb(0xF5781B, 0.6f));
      }
      const float v0 = q0.x + static_cast<float>(ui.viewFrom) * (q1.x - q0.x), v1 = q0.x + static_cast<float>(ui.viewTo) * (q1.x - q0.x);
      dl->AddRectFilled(ImVec2(v0, q0.y), ImVec2(std::max(v1, v0 + 3), q1.y), rgb(0xB9BFC3, 0.16f), sk.px(2));
      dl->AddRect(ImVec2(v0, q0.y), ImVec2(std::max(v1, v0 + 3), q1.y), rgb(0xB9BFC3, 0.55f), sk.px(2));
      ImGui::SetCursorScreenPos(q0);
      ImGui::InvisibleButton("##scroll", ImVec2(q1.x - q0.x, q1.y - q0.y));
      if (ImGui::IsItemActive()) {
        const double w = ui.viewTo - ui.viewFrom;
        const double c = (io.MousePos.x - q0.x) / std::max(1.0f, q1.x - q0.x);
        setView(c - w * 0.5, c + w * 0.5);
      }
      if (ImGui::IsItemHovered()) {
        a.setHint("Drag to scroll the wave view, wheel to zoom");
        if (io.MouseWheel != 0) zoom(io.MouseWheel > 0 ? 0.8 : 1.25, (ui.viewFrom + ui.viewTo) * 0.5);
      }
    }
  }
  // ---- keyboard with the base note selector.
  {
    iskin::bevel(sk, 0, 571, kSxW, 54, rgb(0x12191E), 0, 0);
    ImGui::BeginDisabled(rv.on);
    iskin::digi(a, t, sk, SlBaseNote, 3, 574, 22, 48, [](double v) { return ed::noteName(static_cast<int>(std::lround(v))); }, 7.5f);
    ImGui::EndDisabled();
    bool lit[128] = {};
    for (int s = 0; s < nSl; ++s) {
      int kk = sliceKey(s);
      if (kk >= 0 && kk < 128) lit[kk] = true;
    }
    iskin::KeyColors kc;
    kc.white = rgb(0xD9DEE2), kc.black = rgb(0x242C32), kc.pressed = rgb(0xFFA830), kc.mark = rgb(0xF5781B), kc.sep = rgb(0x0E1419);
    // The keyboard starts an octave under the selected region's key (or the base note).
    // (moved only when that key is out of sight; 35 white keys = 60 keys).
    const int focus = nSl > 0 && sliceKey(k) >= 0 ? sliceKey(k) : base;
    if (ui.kbLow < 0 || focus < ui.kbLow || focus >= ui.kbLow + 60) ui.kbLow = std::clamp((focus / 12) * 12 - 12, 0, 60);
    const int low = ui.kbLow;
    int clicked = iskin::keyboard(a, t, sk, 27, 571, 676, 54, low, 35, kc, sliceKey(k), lit);
    if (clicked >= 0)
      for (int s = 0; s < nSl; ++s)
        if (sliceKey(s) == clicked) {
          selectRegion(s);
          break;
        }
  }
  iskin::end(sk, kSxH);
}

}  // namespace

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != "gl.slicer") return false;
  editorSlicex(a, t, width);
  return true;
}

}  // namespace gl
