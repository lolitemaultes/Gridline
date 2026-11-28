// Gridline - the Tracker layout's own pages (K-106): the Song, Steps and Mix pages in the tracker
// look (bevelled boxes, two-digit hex numbering in the monospaced face, the header's row height and
// colour roles). They draw inside the Playlist, Channel rack and Mixer windows while the Tracker
// layout is built from its shell, so those windows keep their identity (menus, keys, the page strip)
// and every other layout keeps its own views. Same data, same Document commands: a step toggled
// here, a clip placed or a volume dragged is one undo step and syncs to collaborators as anywhere.
#include "PluginIcons.h"
#include "TrackerPages.h"
#include "../core/Lanes.h"
#include "SnapMenu.h"
#include "ClipEditor.h"
#include "Keys.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Arrangements.h"
#include "../core/ClipGain.h"
#include "../engine/ClipSource.h"
#include "../engine/PlaylistTools.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/MenuParity.h"
#include "../core/Params.h"
#include "../core/TrackerData.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "ContextMenus.h"
#include "MidiUi.h"
#include "PluginUI.h"
#include "TrackerEditors.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MenuParityUi.h"
#include "MixerMeters.h"
#include "MixerHideUi.h"
#include "PixelGrid.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerOrder.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"
#include "RenderSheetUi.h"

namespace gl::trackerpages {

using theme::pal;
using trackerview::bevelBox;
using trackerview::bevelButton;
using trackerview::boxTitle;
using trackerview::monoFont;
using trackerview::scrollBar;
using trackerview::valueBar;
using trackerview::stepper;

bool active(const App& a) {
  return layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker);
}

bool enteringFrame() {
  static int seen = -10, entered = -10;
  if (layoutui::active() != layout::Preset::Tracker) return false;
  const int f = ImGui::GetFrameCount();
  if (seen != f && seen != f - 1) entered = f;  // not the Tracker on the frame before
  seen = f;
  if (f != entered) return false;
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  return w && !w->DockIsActive;
}


namespace {

constexpr float kGap = 2;

// cp17 (user): the Song page's sideways views. The lanes: their width (settings
// tracker.songLaneWidth) and scroll; the arranger's timeline: its zoom (tracker.timelineZoom, 1 =
// the whole song) and the first tick in view. Scroll positions are view state, never saved.
constexpr float kMinLaneW = 28, kMaxLaneW = 480;
constexpr float kFitLaneW = 12;  // Fit: every lane in view, down to a narrow strip each (zoom back in for the names)
float gLaneW = 70, gLaneFitW = 70;  // as last drawn
float gLaneScroll = 0;
float gZoomWheel = 0;               // Ctrl+wheel movement not yet a zoom step
int gLaneReveal = -1;               // a lane to bring into view on the next frame
int gLanesShown = 0, gLaneFirst = 0, gLaneLast = -1;
double gTlStart = 0, gTlSpan = 0, gTlLen = 0;  // the timeline's first tick in view, ticks in view, the song's length (last drawn)
int gTlCur = -2;                    // the entry the timeline last brought into view
bool gTlToEnd = false;              // "tracker songzoom end": the timeline to the song's end

float rowHeight() { return std::round(ImGui::GetFontSize() + 4); }

// The page's title bar: a raised box with the title left, `info` right (monospaced), and at the
// right end Exit, back to the pattern (S-03: the pages are screens swapped into the centre, as the
// classic tracker's; each has its Exit, and the page strip switches between them. They do not
// close or float: the centre always shows one page and nothing gets lost behind another window).
float titleBar(App& a, ImVec2 o, float w, const char* title, const std::string& info, float reserve = 0, float leftReserve = 0) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), h = std::round(fs + 8);
  bevelBox(dl, o, ImVec2(o.x + w, o.y + h));
  boxTitle(a, ImVec2(o.x + 6, std::round(o.y + (h - fs) * 0.5f)), title);
  const float ew = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Exit")).x) + 24;
  if (bevelButton(a, "##pageexit", "Exit", ImVec2(o.x + w - 2 - ew, o.y + 2), ImVec2(o.x + w - 2, o.y + h - 2), false, "Back to the pattern"))
    trackerview::exitPage(a, ui::stableWindowName(ImGui::GetCurrentWindowRead()->RootWindow->Name));  // as Esc: a page off the strip (LFO) closes
  // cp16: a message just said (an edit refused, a file saved: the app's notices in the Tracker) takes
  // the info's place for a few seconds, as the pattern bar shows it on the pattern page.
  const float tx0 = std::max(o.x + 6 + std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(title)).x) + fs * 12,
                             leftReserve > 0 ? o.x + leftReserve + 12 : 0.0f);
  const float rightLim = o.x + w - ew - reserve - 10;
  const float room = rightLim - tx0;
  if (const std::string* msg = trackerview::freshStatus()) {
    if (room > 24.0f) {
      const float tw = std::min(room, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, msg->c_str()).x);
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(std::round(rightLim - tw), std::round(o.y + (h - fs) * 0.5f)), room, pal().hot, msg->c_str());
    }
  } else if (!info.empty() && room > 32.0f) {
    ImFont* mono = monoFont(a);
    const float tw = std::min(room, mono->CalcTextSizeA(fs, FLT_MAX, 0, info.c_str()).x);
    theme::textEllipsis(dl, mono, fs, ImVec2(std::round(rightLim - tw), std::round(o.y + (h - fs) * 0.5f)), room, pal().textDim, info.c_str());
  }
  return h;
}

// A sunken name field (the instrument list's look): number, colour mark, name.
void nameField(App& a, ImVec2 p0, ImVec2 p1, int number, unsigned colour, const std::string& name, bool selected, bool dim) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  bevelBox(dl, p0, p1, true);
  if (selected) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.surface600, 0.9f));
  dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 3), ImVec2(p0.x + 5, p1.y - 3), theme::rgb(colour));
  const float ty = std::round(p0.y + (p1.y - p0.y - fs) * 0.5f);
  const std::string num = trk::hex2(number);
  dl->AddText(mono, fs, ImVec2(p0.x + 9, ty), selected ? P.hot : P.textDim, num.c_str());
  const float nx = p0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), p1.x - nx - 4, dim ? P.textFaint : selected ? P.text : P.textDim, name.c_str());
}

// A small square lamp (mute: lit while the sound plays).
bool lamp(App& a, const char* id, ImVec2 p0, float size, bool lit, ImU32 colour, const char* hint) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hov = ImGui::IsItemHovered();
  bevelBox(dl, p0, ImVec2(p0.x + size, p0.y + size), true);
  const ImVec2 i0(p0.x + 3, p0.y + 3), i1(p0.x + size - 3, p0.y + size - 3);
  dl->AddRectFilled(i0, i1, lit ? colour : theme::withAlpha(colour, hov ? 0.35f : 0.18f));
  if (hint) ui::Hint(hint);
  (void)a;
  return click;
}

// The scrolled body under a title bar: a sunken box and a child window clipped to it.
// cp24: a body taller than its box scrolls with the band's own bevel bar inside the box's right edge
// (it was ImGui's thin rounded bar, a modern element); the wheel scrolls it as before.
struct Body {
  ImVec2 p0, p1;
  float sbW = 0;  // the bar's width (0: no bar)
  ImGuiWindow* win = nullptr;
  float scroll = 0, scrollMax = 0, viewH = 1;
};
Body gBody;
float bodyBarW() { return std::round(std::max(12.0f, ImGui::GetFontSize() + 1)); }
bool beginBody(const char* id, ImVec2 p0, ImVec2 p1, float contentH, bool noWheel = false) {
  bevelBox(ImGui::GetWindowDrawList(), p0, p1, true);
  gBody = Body{};
  gBody.p0 = p0, gBody.p1 = p1;
  gBody.sbW = contentH > p1.y - p0.y ? bodyBarW() : 0.0f;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 1, p0.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  // (noWheel, cp17: Ctrl+wheel zooms the body sideways and Shift+wheel scrolls it, not the rows)
  const float w = p1.x - p0.x - 2 - (gBody.sbW > 0 ? gBody.sbW + trackerview::space::kLine : 0.0f);
  const bool open = ImGui::BeginChild(id, ImVec2(std::max(1.0f, w), std::max(1.0f, p1.y - p0.y - 2)), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_NoScrollbar | (noWheel ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None));
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  gBody.win = ImGui::GetCurrentWindow();
  return open;
}
void endBody(App& a, float contentH) {
  ImGui::SetCursorPos(ImVec2(0, contentH));
  ImGui::Dummy(ImVec2(1, 0));
  gBody.scroll = ImGui::GetScrollY(), gBody.scrollMax = ImGui::GetScrollMaxY(), gBody.viewH = ImGui::GetWindowHeight();
  ImGui::EndChild();
  if (gBody.sbW > 0 && gBody.win) {
    double at = gBody.scroll;
    const ImVec2 s0(gBody.p1.x - 1 - gBody.sbW, gBody.p0.y + 1), s1(gBody.p1.x - 1, gBody.p1.y - 1);
    if (trackerview::vScrollBar(a, gBody.win->Name, s0, s1, &at, gBody.viewH, gBody.scrollMax + gBody.viewH, std::round(ImGui::GetFontSize() + 4) * 3)) {
      ImGui::SetScrollY(gBody.win, static_cast<float>(std::clamp(at, 0.0, static_cast<double>(gBody.scrollMax))));
    }
  }
}

// K-306: clang-cl 20.1.8 (Windows CI) crashed in its x86 instruction selection on steps(), and
// again after the step cells moved into a function of their own, which it inlined back. The Steps
// page's parts are kept out of line, and clang-cl builds them without optimisation (a page drawn
// once a frame; every other compiler optimises them as usual).
#if defined(__clang__) && defined(_MSC_VER)
#define GL_STEPS_FN __attribute__((noinline, optnone))
#elif defined(_MSC_VER)
#define GL_STEPS_FN __declspec(noinline)
#else
#define GL_STEPS_FN __attribute__((noinline))
#endif

// One channel's row of step cells.
GL_STEPS_FN void stepCells(App& a, ImDrawList* dl, const Pattern* pat, ObjectId channel, const std::vector<uint8_t>* row, int first, int nSteps, int playStep,
                           float gx, float y, float rh, float cw, float group) {
  const auto& P = pal();
  // K-255: the row's own steps (a grid of its own sits stretched under the pattern's steps, a bar as
  // wide as every row's) and its length (the steps past it show, dim, what the loop plays there).
  const bool own = pat && lanes::ownGrid(*pat, channel);
  const int len = pat ? lanes::length(*pat, channel) : 0;
  const int spb = pat ? std::max(1, pat->stepsPerBar) : 16, g = pat ? lanes::grid(*pat, channel) : 16;
  const int laneSteps = pat ? lanes::steps(*pat, channel) : nSteps;
  auto xAt = [&](double u) {  // screen x of pattern step position u (fractional) in the part shown
    const double r = u - first;
    const int i = static_cast<int>(std::floor(r));
    auto X = [&](int k) { return gx + static_cast<float>(k) * (cw + 1) + group * static_cast<float>(k >= 0 ? k / 4 : (k - 3) / 4); };
    return X(i) + static_cast<float>(r - i) * (X(i + 1) - X(i));
  };
  int k0 = first, k1 = first + nSteps;
  if (own) {  // the lane steps that start in the part shown
    k0 = static_cast<int>(std::ceil(static_cast<double>(first) * g / spb - 1e-9));
    k1 = std::min(laneSteps, static_cast<int>(std::ceil(static_cast<double>(first + nSteps) * g / spb - 1e-9)));
  }
  int lanePlay = playStep;
  if (own && playStep >= 0 && pat) {
    const double st = lanes::stepTicks(a.doc.song(), *pat, channel);
    lanePlay = -1;
    if (a.engine.playing() && st > 0) lanePlay = static_cast<int>(static_cast<int64_t>(std::floor(static_cast<double>(playStep) * a.doc.song().ticksPerStep(*pat) / st + 1e-9)) % std::max(1, laneSteps));
  }
  // cp24: the steps first .. first + nSteps - 1 (a long pattern shows a part at a time).
  for (int k = k0; k < k1; ++k) {
    float x, w;
    if (own) {
      const double u = static_cast<double>(k) * spb / g, u1 = static_cast<double>(k + 1) * spb / g;
      x = std::round(xAt(u));
      w = std::max(3.0f, std::round(xAt(u1)) - x - 1);
    } else {
      const int i = k - first;
      x = std::round(gx + static_cast<float>(i) * (cw + 1) + group * static_cast<float>(i / 4));
      w = cw;
    }
    const bool looped = len > 0 && k >= len;
    const int ek = looped ? k % len : k;  // a looped step edits the step it repeats
    const ImVec2 s0(x, y + 1), s1(x + w, y + rh - 2);
    const uint8_t v = row && static_cast<size_t>(ek) < row->size() ? (*row)[static_cast<size_t>(ek)] : 0;
    ImGui::SetCursorScreenPos(s0);
    ImGui::PushID(k);
    const bool click = ImGui::InvisibleButton("##st", ImVec2(w, s1.y - s0.y));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool beat = own ? (static_cast<int>(std::floor(static_cast<double>(k) * spb / g + 1e-9)) / 4) % 2 == 0 : (k / 4) % 2 == 0;
    bevelBox(dl, s0, s1, v == 0);
    if (v > 0) {
      dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), theme::mix(P.surface700, P.hot, 0.35f + 0.65f * static_cast<float>(v) / 127.0f));
    } else {
      dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), beat ? P.surface900 : P.surface950);
    }
    if (looped) dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), theme::withAlpha(P.surface950, 0.55f));
    if (len > 0 && k == len) dl->AddRectFilled(ImVec2(s0.x - 2, s0.y - 1), ImVec2(s0.x, s1.y + 1), P.hot);  // where the loop starts again
    if (hov) dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), theme::withAlpha(P.text, 0.12f));
    if (k == lanePlay) dl->AddRectFilled(ImVec2(s0.x + 1, s1.y - 3), ImVec2(s1.x - 1, s1.y - 1), P.playhead);
    if (hov && looped) ui::Hint("A step the loop plays again: a click sets the step it repeats");
    if (click && pat) a.doc.setStep(pat->id, channel, ek, v > 0 ? 0 : 100);
  }
}

// One channel's row on the Steps page: its name, mute lamp, and step cells (or its notes' starts).
GL_STEPS_FN void stepRow(App& a, const Pattern* pat, size_t ci, ImVec2 c0, float nameW, float lampW, float gx, float avail, float rh,
                         float cw, float group, int first, int view, int nSteps, int playStep) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Channel& c = s.channels[ci];
  const float y = std::round(c0.y + 2 + static_cast<float>(ci) * rh);
  ImGui::PushID(static_cast<int>(ci));
  const bool sel = a.selChannel == c.id;
  nameField(a, ImVec2(c0.x + 2, y), ImVec2(c0.x + 2 + nameW, y + rh - 1), static_cast<int>(ci) + 1, c.color, c.name, sel, c.muted);
  ImGui::SetCursorScreenPos(ImVec2(c0.x + 2, y));
  if (ImGui::InvisibleButton("##name", ImVec2(nameW, rh - 1))) a.selectChannel(c.id);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.settingsChannel = c.id;
  ui::Hint("Click: select \xC2\xB7 double-click: its instrument");
  if (lamp(a, "##mute", ImVec2(c0.x + 2 + nameW + kGap, std::round(y + (rh - 1 - lampW) * 0.5f)), lampW, !c.muted, P.led, "Lit: the channel plays \xC2\xB7 click: mute")) {
    a.doc.setParam(channelParam(c.id, ChannelParam::Mute), c.muted ? 0 : 1, false);
  }
  const std::vector<uint8_t>* row = pat && pat->steps.count(c.id) ? &pat->steps.at(c.id) : nullptr;
  const bool hasNotes = pat && pat->notes.count(c.id) && !pat->notes.at(c.id).empty();
  if (hasNotes && (!row || std::none_of(row->begin(), row->end(), [](uint8_t v) { return v > 0; }))) {
    // A channel playing notes: the piano roll edits them; here a quiet line of their starts.
    const std::vector<Note>& ns = pat->notes.at(c.id);
    const Tick len = std::max<Tick>(1, s.patternLength(*pat));
    // (cp24: the part of the pattern the steps show)
    const Tick t0 = len * first / std::max(1, nSteps), t1 = std::max<Tick>(t0 + 1, len * (first + view) / std::max(1, nSteps));
    const float span = static_cast<float>(t1 - t0);
    dl->AddRectFilled(ImVec2(gx, y + 1), ImVec2(gx + avail, y + rh - 2), theme::withAlpha(P.surface800, 0.6f));  // (cp24: as wide as the cells)
    for (const Note& n : ns) {
      const Tick ns0 = n.start % len, ns1 = std::min<Tick>(len, ns0 + n.length);
      if (ns1 <= t0 || ns0 >= t1) {
        continue;
      }
      const float nx0 = std::round(gx + avail * static_cast<float>(std::max(ns0, t0) - t0) / span);
      const float nx1 = std::max(nx0 + 2, std::round(gx + avail * static_cast<float>(std::min(ns1, t1) - t0) / span));
      const float ny = std::round(y + 3 + (rh - 8) * (1.0f - static_cast<float>(n.pitch % 24) / 24.0f));
      dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + 2), theme::rgb(c.color));
    }
    ImGui::SetCursorScreenPos(ImVec2(gx, y));
    if (ImGui::InvisibleButton("##notes", ImVec2(std::max(1.0f, avail), rh - 1))) a.selectChannel(c.id);
    ui::Hint("This channel plays notes: the pattern editor and the piano roll edit them");
  } else {
    stepCells(a, dl, pat, c.id, row, first, view, playStep, gx, y, rh, cw, group);
  }
  ImGui::PopID();
}

// The Steps page's title text: the pattern's number and its length in steps.
std::string stepsInfo(const Song& s, const Pattern* pat, int nSteps) {
  const std::string num = pat ? trk::hexNum(trk::patternNumber(s, pat->id)) : std::string("--");
  char info[96];
  std::snprintf(info, sizeof info, "%s %s  %d %s", i18n::tr("Pattern"), num.c_str(), nSteps, i18n::tr("steps"));
  return info;
}

// ---- the Song page as an arranger ----
// The order list (the song, one entry per row), the pattern bank, the song box and a timeline strip.
// Every edit goes through the core order-list edits, one undo step each. Songs whose entries layer
// several patterns or audio show those entries read only; the lane view stays behind "Lanes".

// The entry under the song position (-1: none), as the header's order list.
int entryAt(App& a, const std::vector<trk::OrderEntry>& ol) {
  const Tick pos = trackerview::songPosition(a);
  for (size_t i = 0; i < ol.size(); ++i)
    if (pos >= ol[i].start && pos < ol[i].end) return static_cast<int>(i);
  return ol.empty() ? -1 : (pos >= ol.back().end ? static_cast<int>(ol.size()) - 1 : 0);
}

// A block of entries marked with Shift (Shift+Up/Down, Shift+click): its other end, -1 none. The
// block is what Loop plays over and over (the playlist's time selection: the same loop in every layout).
int gMark = -1;

// The marked block [lo, hi] around the current entry (lo > hi: none).
void markedBlock(int cur, int n, int* lo, int* hi) {
  if (gMark < 0 || gMark >= n || cur < 0) {
    *lo = 1, *hi = 0;
    return;
  }
  *lo = std::min(gMark, cur), *hi = std::max(gMark, cur);
}

// The loop the engine plays (the playlist's time selection, else the song's saved loop: playLoop), as
// entries: [lo, hi], or lo > hi none.
void loopedEntries(const App& a, const std::vector<trk::OrderEntry>& ol, int* lo, int* hi) {
  *lo = 1, *hi = 0;
  Tick from = 0, to = 0;
  if (!playLoop(a, from, to)) {
    return;
  }
  for (size_t i = 0; i < ol.size(); ++i) {
    if (ol[i].end > from && ol[i].start < to) {
      if (*lo > *hi) *lo = static_cast<int>(i);
      *hi = static_cast<int>(i);
    }
  }
}

// Loops [from, to) of the song (to <= from: no loop): the playlist's time selection, sent to the engine.
// K-314 (A-06): it is the song's saved loop too (Song::cycleStart / cycleEnd / cycleOn, as Smooth's cycle
// and Clips' loop brace), so it is saved and loops in every layout; off keeps the range, as theirs do.
void setSongLoop(App& a, Tick from, Tick to) {
  if (to <= from) from = to = 0;
  a.playlist.selStart = from;
  a.playlist.selEnd = to;
  a.engine.setLoopRange(from, to);
  const Song& s = a.doc.song();
  if (to > from ? !(s.cycleOn && s.cycleStart == from && s.cycleEnd == to) : s.cycleOn) {
    a.doc.patchSong([from, to](Song& x) {
      x.cycleOn = to > from;
      if (to > from) {
        x.cycleStart = from, x.cycleEnd = to;
      }
    });
  }
}

void goEntry(App& a, const std::vector<trk::OrderEntry>& ol, int i) {
  if (i < 0 || i >= static_cast<int>(ol.size())) return;
  const trk::OrderEntry& e = ol[static_cast<size_t>(i)];
  if (e.pattern != kNoId) a.selectPattern(e.pattern);
  trackerview::setSongPosition(a, e.start);
}

void sayOrder(trk::OrderEdit r) {
  if (r == trk::OrderEdit::Spans) trackerview::status(i18n::tr("That entry's clips run into the next entries: edit it in Lanes"));
  else if (r == trk::OrderEdit::OtherClips) trackerview::status(i18n::tr("That entry holds audio or automation clips: edit it in Lanes"));
}

// Entry `i` and its neighbour `i + d` trade patterns (moving the entry up or down): one undo step.
void moveEntry(App& a, const std::vector<trk::OrderEntry>& ol, int i, int d) {
  const int j = i + d;
  if (i < 0 || j < 0 || i >= static_cast<int>(ol.size()) || j >= static_cast<int>(ol.size())) return;
  const ObjectId pi = ol[static_cast<size_t>(i)].pattern, pj = ol[static_cast<size_t>(j)].pattern;
  if (pi == kNoId || pj == kNoId || ol[static_cast<size_t>(i)].patterns != 1 || ol[static_cast<size_t>(j)].patterns != 1) {
    sayOrder(trk::OrderEdit::OtherClips);
    return;
  }
  if (pi == pj) {
    goEntry(a, ol, j);
    return;
  }
  // The earlier entry first: its length change moves the later one, whose index stays.
  const int lo = std::min(i, j), hi = std::max(i, j);
  const ObjectId pLo = ol[static_cast<size_t>(lo)].pattern, pHi = ol[static_cast<size_t>(hi)].pattern;
  a.doc.beginGroup("move order entry");
  trk::OrderEdit r = trk::orderSetPattern(a.doc, lo, pHi);
  if (r == trk::OrderEdit::Done) r = trk::orderSetPattern(a.doc, hi, pLo);
  a.doc.endGroup(false);
  if (r != trk::OrderEdit::Done) {
    sayOrder(r);
    return;
  }
  goEntry(a, *trackerorder::of(a), j);
}

void setEntryPattern(App& a, const std::vector<trk::OrderEntry>& ol, int i, ObjectId p) {
  if (i < 0 || i >= static_cast<int>(ol.size()) || p == kNoId) return;
  const trk::OrderEdit r = trk::orderSetPattern(a.doc, i, p);
  sayOrder(r);
  if (r == trk::OrderEdit::Done) goEntry(a, *trackerorder::of(a), i);
}

void stepEntryPattern(App& a, const std::vector<trk::OrderEntry>& ol, int i, int d) {
  const Song& s = a.doc.song();
  if (i < 0 || i >= static_cast<int>(ol.size()) || ol[static_cast<size_t>(i)].pattern == kNoId) return;
  const int pi = s.patternIndex(ol[static_cast<size_t>(i)].pattern) + d;
  if (pi < 0 || pi >= static_cast<int>(s.patterns.size())) return;
  setEntryPattern(a, ol, i, s.patterns[static_cast<size_t>(pi)].id);
}

void insertEntry(App& a, const std::vector<trk::OrderEntry>& /*ol*/, int at, ObjectId p) {
  if (p == kNoId) return;
  gMark = -1;
  const trk::OrderEdit r = trk::orderInsert(a.doc, at, p);
  sayOrder(r);
  if (r == trk::OrderEdit::Done) goEntry(a, *trackerorder::of(a), at);
}

void removeEntry(App& a, const std::vector<trk::OrderEntry>& ol, int at) {
  if (at < 0 || at >= static_cast<int>(ol.size())) return;
  gMark = -1;
  const trk::OrderEdit r = trk::orderRemove(a.doc, at);
  sayOrder(r);
  if (r == trk::OrderEdit::Done) {
    const trackerorder::List nlHeld = trackerorder::of(a, a.doc.song());  // K-142: cached per song change
    const std::vector<trk::OrderEntry>& nl = *nlHeld;
    goEntry(a, nl, std::min(at, static_cast<int>(nl.size()) - 1));
  }
}

int loopEntry(const Song& s, const std::vector<trk::OrderEntry>& ol) {
  for (const Marker& m : s.markers)
    if (m.action == MarkerAction::SongLoop)
      for (size_t i = 0; i < ol.size(); ++i)
        if (m.pos < ol[i].end) return static_cast<int>(i);
  return -1;
}

// cp22: the Song page's counts (rows, bars) in one number base, the Tracker's own (Settings, Layout,
// Hexadecimal rows): two hex digits, or decimal.
std::string countText(App& a, int v) {
  char b[16];
  std::snprintf(b, sizeof b, a.settings.getString("tracker.rowBase") != "dec" ? "%02X" : "%02d", std::max(0, v));
  return b;
}

std::string timeText(const Song& s, Tick t) {
  const double secs = static_cast<double>(t) / std::max(1, s.ppq) * 60.0 / std::max(1.0, s.tempo);
  char b[32];
  std::snprintf(b, sizeof b, "%d:%02d", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
  return b;
}

void arranger(App& a, ImVec2 o, ImVec2 av, float th) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize(), rh = rowHeight(), pad = 4;
  const trackerorder::List olHeld = trackerorder::of(a, s);  // K-142: cached per song change
  const std::vector<trk::OrderEntry>& ol = *olHeld;
  const int cur = entryAt(a, ol), loopAt = loopEntry(s, ol);
  int mLo, mHi, lLo, lHi;
  markedBlock(cur, static_cast<int>(ol.size()), &mLo, &mHi);
  loopedEntries(a, ol, &lLo, &lHi);
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const bool playing = a.engine.playing();
  const float bh = std::round(rh + 4);                 // button height
  const float tlSb = std::max(12.0f, std::round(fs * 0.95f));  // cp17: the timeline's scroll bar
  const float tlH = std::round(rh * 2 + pad * 2) + tlSb;      // the timeline strip
  const float top = o.y + th + kGap, bottom = o.y + av.y - tlH - kGap;
  const float W = av.x;
  const float wOrder = std::floor(W * 0.44f), wPat = std::floor(W * 0.32f), wSong = W - wOrder - wPat - 2 * kGap;
  const float xO = o.x, xP = xO + wOrder + kGap, xS = xP + wPat + kGap;
  const float digitW = mono->CalcTextSizeA(fs, FLT_MAX, 0, "00").x;
  // ---- the order list ----
  {
    bevelBox(dl, ImVec2(xO, top), ImVec2(xO + wOrder, bottom));
    boxTitle(a, ImVec2(xO + 6, top + 3), "Order");
    const float hy = top + rh + 2;
    const float cPos = xO + pad + 10, cPtn = cPos + digitW + 14, cName = cPtn + digitW + 14;
    // Each right column as wide as its heading or its digits (a wide heading never runs off the box).
    auto colW = [&](const char* head) { return std::max(digitW, std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(head)).x)); };
    const float cRows = xO + wOrder - pad - colW("Rows") - 14, cBars = cRows - colW("Bars") - 14;
    // cp22: the entry's start time where the box has room (the space between the name and the counts).
    const float startW = std::max(mono->CalcTextSizeA(fs, FLT_MAX, 0, "00:00").x, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Start")).x);
    const bool showStart = cBars - startW - 14 - cName >= fs * 9.0f;
    const float cStart = showStart ? cBars - startW - 14 : cBars;
    auto head = [&](float x, const char* t) { dl->AddText(a.fontRegular, fs, ImVec2(x, hy), P.textFaint, i18n::tr(t)); };
    head(cPos, "Pos");
    head(cPtn, "Ptn");
    head(cName, "Name");
    if (showStart) {
      head(cStart, i18n::N_("Start"));
    }
    head(cBars, "Bars");
    head(cRows, "Rows");
    const float l0y = hy + rh, l1y = bottom - bh - pad * 2;
    const float contentH = static_cast<float>(ol.size()) * rh + 2;
    if (beginBody("##orderlist", ImVec2(xO + pad, l0y), ImVec2(xO + wOrder - pad, l1y), contentH)) {
      ImDrawList* cl = ImGui::GetWindowDrawList();
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      const float cw = ImGui::GetContentRegionAvail().x;
      static int lastCur = -2;
      if (cur != lastCur && cur >= 0) {  // the current entry in view
        const float y = static_cast<float>(cur) * rh, sy = ImGui::GetScrollY(), vh = ImGui::GetWindowHeight();
        if (y < sy) ImGui::SetScrollY(y);
        else if (y + rh > sy + vh) ImGui::SetScrollY(y + rh - vh);
        lastCur = cur;
      }
      const float dx = c0.x - (xO + pad);  // the columns measured from the box
      for (size_t i = 0; i < ol.size(); ++i) {
        const trk::OrderEntry& e = ol[i];
        const float y = std::round(c0.y + static_cast<float>(i) * rh);
        ImGui::SetCursorScreenPos(ImVec2(c0.x, y));
        ImGui::PushID(static_cast<int>(i));
        const bool click = ImGui::InvisibleButton("##ent", ImVec2(std::max(1.0f, cw), rh));
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ImGui::PopID();
        const int ii = static_cast<int>(i);
        const bool marked = ii >= mLo && ii <= mHi;
        if (ii == cur) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.surface600, 0.95f));
        else if (marked) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.hot, 0.16f));
        else if (hov) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.surface700, 0.6f));
        if (i % 4 == 0 && ii != cur && !marked) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.surface800, 0.35f));
        // The looped entries: a bar at the left edge (the song's Loop button).
        if (ii >= lLo && ii <= lHi) cl->AddRectFilled(ImVec2(c0.x + cw - 8, y + (ii == lLo ? 2.0f : 0.0f)), ImVec2(c0.x + cw - 6, y + rh - (ii == lHi ? 2.0f : 0.0f)), P.led);
        const float ty = std::round(y + (rh - fs) * 0.5f);
        if (ii == loopAt) cl->AddText(mono, fs, ImVec2(c0.x + 2, ty), P.hot, "L");
        const std::string pos = trk::hexNum(static_cast<int>(i));
        cl->AddText(mono, fs, ImVec2(cPos + dx, ty), static_cast<int>(i) == cur ? P.hot : P.textDim, pos.c_str());
        const Pattern* p = s.pattern(e.pattern);
        if (p) {
          const std::string num = trk::hexNum(trk::patternNumber(s, p->id));
          cl->AddRectFilled(ImVec2(cPtn + dx - 5, y + 3), ImVec2(cPtn + dx - 2, y + rh - 3), theme::rgb(p->color));
          cl->AddText(mono, fs, ImVec2(cPtn + dx, ty), P.text, num.c_str());
          std::string name = p->name;
          if (e.patterns > 1) name += " +" + std::to_string(e.patterns - 1);
          theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(cName + dx, ty), cStart - cName - 8, static_cast<int>(i) == cur ? P.text : P.textDim, name.c_str());
          const trk::Grid g = trk::gridOf(s, *p);
          const std::string bars = countText(a, static_cast<int>((e.end - e.start + bar - 1) / bar)), rows = countText(a, std::min(255, g.rows));
          cl->AddText(mono, fs, ImVec2(cBars + dx, ty), P.textDim, bars.c_str());
          cl->AddText(mono, fs, ImVec2(cRows + dx, ty), P.textDim, rows.c_str());
        }
        if (showStart) {
          cl->AddText(mono, fs, ImVec2(cStart + dx, ty), P.textFaint, timeText(s, e.start).c_str());
        }
        if (!p) {
          cl->AddText(mono, fs, ImVec2(cPtn + dx, ty), P.textFaint, "AU");
          cl->AddText(a.fontRegular, fs, ImVec2(cName + dx, ty), P.textFaint, i18n::tr("Audio part"));
        }
        if (playing && a.engine.mode() == PlayMode::Song && static_cast<int>(i) == cur)
          cl->AddRectFilled(ImVec2(c0.x + cw - 4, y + 2), ImVec2(c0.x + cw - 1, y + rh - 2), P.playhead);
        if (click) {  // Shift: the block from the current entry to this one
          if (ImGui::GetIO().KeyShift && cur >= 0) {
            if (gMark < 0) gMark = cur;
          } else {
            gMark = -1;
          }
          goEntry(a, ol, static_cast<int>(i));
        }
        if (dbl && e.pattern != kNoId) trackerview::showPage(a, "Pattern editor");
      }
      endBody(a, contentH);
    }
    if (ol.empty()) {
      dl->AddText(a.fontRegular, fs, ImVec2(xO + pad + 10, l0y + 6), P.textFaint, i18n::tr("The song is empty: pick a pattern, then Ins or Append"), nullptr,
                  wOrder - pad * 2 - 20);
    }
    // The order's buttons.
    const float by = bottom - bh - pad;
    const char* const labels[] = {i18n::N_("Ins"), i18n::N_("Del"), i18n::N_("Up"), i18n::N_("Down"), i18n::N_("Ptn -"), i18n::N_("Ptn +")};
    const char* const hints[] = {i18n::N_("Inserts the current entry's pattern after it (Ins); in an empty song, the pattern shown"),
                                 i18n::N_("Removes the current entry; the song after it moves up (Del)"),
                                 i18n::N_("Moves the entry up: it trades places with the one above (Ctrl+Up)"),
                                 i18n::N_("Moves the entry down (Ctrl+Down)"),
                                 i18n::N_("The entry plays the previous pattern (Left)"),
                                 i18n::N_("The entry plays the next pattern (Right)")};
    const float bw = std::floor((wOrder - pad * 2 - kGap * 5) / 6);
    for (int k = 0; k < 6; ++k) {
      const float bx = xO + pad + static_cast<float>(k) * (bw + kGap), bx1 = k == 5 ? xO + wOrder - pad : bx + bw;
      const bool need = k > 0;
      const bool off = need && cur < 0;
      ImGui::PushID(700 + k);
      if (off) ImGui::BeginDisabled();
      if (bevelButton(a, "##ordb", labels[k], ImVec2(bx, by), ImVec2(bx1, by + bh), false, hints[k])) {
        switch (k) {
          case 0: insertEntry(a, ol, cur < 0 ? 0 : cur + 1, cur >= 0 && ol[static_cast<size_t>(cur)].pattern != kNoId ? ol[static_cast<size_t>(cur)].pattern : a.curPattern); break;
          case 1: removeEntry(a, ol, cur); break;
          case 2: moveEntry(a, ol, cur, -1); break;
          case 3: moveEntry(a, ol, cur, 1); break;
          case 4: stepEntryPattern(a, ol, cur, -1); break;
          default: stepEntryPattern(a, ol, cur, 1); break;
        }
      }
      if (off) ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  // ---- the pattern bank ----
  {
    bevelBox(dl, ImVec2(xP, top), ImVec2(xP + wPat, bottom));
    boxTitle(a, ImVec2(xP + 6, top + 3), "Patterns");
    const float hy = top + rh + 2;
    const float cNo = xP + pad + 10, cName = cNo + digitW + 14;
    auto colW = [&](const char* head) { return std::max(digitW, std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(head)).x)); };
    const float cUses = xP + wPat - pad - colW("Uses") - 14, cBars = cUses - colW("Bars") - 14;
    dl->AddText(a.fontRegular, fs, ImVec2(cNo, hy), P.textFaint, i18n::tr("No"));
    dl->AddText(a.fontRegular, fs, ImVec2(cName, hy), P.textFaint, i18n::tr("Name"));
    dl->AddText(a.fontRegular, fs, ImVec2(cBars, hy), P.textFaint, i18n::tr("Bars"));
    dl->AddText(a.fontRegular, fs, ImVec2(cUses, hy), P.textFaint, i18n::tr("Uses"));
    const float l0y = hy + rh, l1y = bottom - bh * 2 - pad * 2 - kGap;
    std::vector<int> uses(s.patterns.size(), 0);
    for (const trk::OrderEntry& e : ol)
      if (const int pi = s.patternIndex(e.pattern); pi >= 0) ++uses[static_cast<size_t>(pi)];
    const float contentH = static_cast<float>(s.patterns.size()) * rh + 2;
    if (beginBody("##patbank", ImVec2(xP + pad, l0y), ImVec2(xP + wPat - pad, l1y), contentH)) {
      ImDrawList* cl = ImGui::GetWindowDrawList();
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      const float cw = ImGui::GetContentRegionAvail().x, dx = c0.x - (xP + pad);
      for (size_t i = 0; i < s.patterns.size(); ++i) {
        const Pattern& p = s.patterns[i];
        const ObjectId pid = p.id;
        const Rgb color = p.color;
        const std::string name = p.name;
        const int barsCount = p.bars;
        const float y = std::round(c0.y + static_cast<float>(i) * rh);
        ImGui::SetCursorScreenPos(ImVec2(c0.x, y));
        ImGui::PushID(static_cast<int>(i));
        const bool click = ImGui::InvisibleButton("##pat", ImVec2(std::max(1.0f, cw), rh));
        if (i == 0) devNoteRect("trpat0");
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ui::Hint("Click: select \xC2\xB7 double-click: pattern editor \xC2\xB7 right-click: rename & options");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.selectPattern(pid);
        const uint64_t rev = a.doc.revision();
        if (ImGui::BeginPopupContextItem("##trpatctx")) {
          patternContextMenu(a, pid);
          ImGui::EndPopup();
        }
        ImGui::PopID();
        const bool sel = pid == a.curPattern;
        if (sel) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.surface600, 0.95f));
        else if (hov) cl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + cw, y + rh), theme::withAlpha(P.surface700, 0.6f));
        const float ty = std::round(y + (rh - fs) * 0.5f);
        cl->AddRectFilled(ImVec2(cNo + dx - 5, y + 3), ImVec2(cNo + dx - 2, y + rh - 3), theme::rgb(color));
        const std::string num = trk::hex2(static_cast<int>(i) + 1);
        cl->AddText(mono, fs, ImVec2(cNo + dx, ty), sel ? P.hot : P.textDim, num.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(cName + dx, ty), cBars - cName - 8, sel ? P.text : P.textDim, name.c_str());
        char u[16];
        const std::string bars = countText(a, barsCount);
        std::snprintf(u, sizeof u, "%d", uses[i]);
        cl->AddText(mono, fs, ImVec2(cBars + dx, ty), P.textDim, bars.c_str());
        cl->AddText(mono, fs, ImVec2(cUses + dx, ty), uses[i] ? P.textDim : P.textFaint, u);
        if (click) a.selectPattern(pid);
        if (dbl) trackerview::showPage(a, "Pattern editor");
        if (a.doc.revision() != rev) break;
      }
      endBody(a, contentH);
    }
    const Pattern* sp = s.pattern(a.curPattern);
    const float bw = std::floor((wPat - pad * 2 - kGap * 2) / 3);
    const float by0 = bottom - bh * 2 - pad - kGap, by1 = bottom - bh - pad;
    struct B {
      const char* label;
      const char* hint;
      bool on;
    };
    const B bs[6] = {{i18n::N_("New"), i18n::N_("A new empty pattern after the last, as long as the current one"), true},
                     {i18n::N_("Duplicate"), i18n::N_("A copy of the picked pattern after it, shown (one undo step)"), sp != nullptr},
                     {i18n::N_("Set"), i18n::N_("The current entry plays the picked pattern"), sp != nullptr && cur >= 0},
                     {i18n::N_("Append"), i18n::N_("Adds the picked pattern at the end of the song"), sp != nullptr},
                     {i18n::N_("Bars -"), i18n::N_("The picked pattern a bar shorter (the song after it moves up)"), sp && sp->bars > 1},
                     {i18n::N_("Bars +"), i18n::N_("The picked pattern a bar longer (the song after it moves down)"), sp && sp->bars < 64}};
    for (int k = 0; k < 6; ++k) {
      const float bx = xP + pad + static_cast<float>(k % 3) * (bw + kGap), bx1 = k % 3 == 2 ? xP + wPat - pad : bx + bw;
      const float by = k < 3 ? by0 : by1;
      ImGui::PushID(720 + k);
      if (!bs[k].on) ImGui::BeginDisabled();
      if (bevelButton(a, "##patb", bs[k].label, ImVec2(bx, by), ImVec2(bx1, by + bh), false, bs[k].hint)) {
        switch (k) {
          case 0: {
            // HUNT2-1119: as long as the current pattern (cp21: always 64 rows).
            const trackerview::NewPatternSize ns = trackerview::newPatternSize(a);
            const ObjectId id = a.doc.addPattern(std::string(i18n::tr("Pattern")) + " " + std::to_string(s.patterns.size() + 1), ns.bars, ns.steps);
            if (id) a.selectPattern(id);
            break;
          }
          case 1:
            if (const ObjectId id = a.doc.clonePattern(sp->id)) a.selectPattern(id);
            break;
          case 2: setEntryPattern(a, ol, cur, sp->id); break;
          case 3: insertEntry(a, ol, static_cast<int>(ol.size()), sp->id); break;
          case 4: trk::setPatternLengthInSong(a.doc, sp->id, sp->bars - 1, sp->stepsPerBar); break;
          default: trk::setPatternLengthInSong(a.doc, sp->id, sp->bars + 1, sp->stepsPerBar); break;
        }
      }
      if (!bs[k].on) ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  // ---- the song box ----
  {
    bevelBox(dl, ImVec2(xS, top), ImVec2(xS + wSong, bottom));
    boxTitle(a, ImVec2(xS + 6, top + 3), "Song");
    // cp23 (polish): the box on the band's spacing scale. Its first line on the neighbours' heading
    // line ("Pos Ptn Name", "No Name": it stood 4 px lower), lines a row apart as the lists' rows (were a
    // row and 2 px), the steppers as tall as the buttons under them and their fields against their
    // arrows (were 2 px shorter, the fields 38 px left of the arrows), 2 px between peers and 6 px
    // between groups (were 4, 6, 10, 2 and 8 px).
    namespace tsp = trackerview::space;
    const float x = xS + pad + 4, w = wSong - pad * 2 - 8;
    float y = top + rh + 2;
    auto line = [&](const char* label, const std::string& v) {
      dl->AddText(a.fontRegular, fs, ImVec2(x, y), P.textDim, i18n::tr(label));
      // (cp22: a long value, a pattern's name, is cut before it reaches the label)
      const float lw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x + 10;
      const float vw = std::min(mono->CalcTextSizeA(fs, FLT_MAX, 0, v.c_str()).x, std::max(0.0f, w - lw));
      theme::textEllipsis(dl, mono, fs, ImVec2(x + w - vw, y), vw + 1, P.text, v.c_str());
      y += rh;
    };
    const Pattern* sp = s.pattern(a.curPattern);
    ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
    const Tick len = ol.empty() ? 0 : ol.back().end;
    line("Entries", trk::hexNum(static_cast<int>(ol.size())));
    line("Bars", countText(a, static_cast<int>((len + bar - 1) / bar)));
    line("Time", timeText(s, len));
    line("Position", cur >= 0 ? trk::hexNum(cur) : std::string("--"));
    y += tsp::kGap;
    char bpm[24];
    {  // K-409: preserve decimals up to 3 places, trimming trailing zeros
      std::snprintf(bpm, sizeof bpm, "%.3f", s.tempo);
      char* dot = std::strchr(bpm, '.');
      if (dot) {
        char* end = bpm + std::strlen(bpm);
        while (end > dot + 1 && end[-1] == '0') *--end = '\0';
        if (end == dot + 1) *dot = '\0';
      }
    }
    double typedBpm = std::nan("");
    if (const int d = stepper(a, "sgbpm", "Tempo", bpm, ImVec2(x, y), w, bh,
                              "The song tempo in beats per minute: click, double-click or right-click to type (Shift: steps of 0.1)", w, false, &typedBpm)) {
      const double t = ImGui::GetIO().KeyShift ? std::round(s.tempo * 10 + d) / 10.0 : std::round(s.tempo) + d;
      a.doc.setParam(songParam(SongParam::Tempo), std::clamp(t, 10.0, 522.0), true);
    }
    if (!std::isnan(typedBpm)) {
      const double v = std::clamp(std::round(typedBpm * 1000.0) / 1000.0, 10.0, 522.0);
      if (std::fabs(v - s.tempo) > 1e-6) {
        a.doc.endGesture();
        a.doc.setParam(songParam(SongParam::Tempo), v, false);
        a.doc.endGesture();
      }
    }
    {  // K-409: right-click > Type a value... on the Song page's Tempo stepper
      static bool openSgBpmType = false;
      const ImVec2 tp0(x, y), tp1(x + w, y + bh);
      if (ImGui::IsMouseHoveringRect(tp0, tp1) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##sgbpmmenu");
      if (ImGui::BeginPopup("##sgbpmmenu")) {
        if (ImGui::MenuItem(i18n::tr("Type a value..."))) openSgBpmType = true;
        ImGui::Separator();
        knobContextMenu(a, songParam(SongParam::Tempo));
        actionLinkMenuItems(a, RemoteAction::TapTempo);
        ImGui::EndPopup();
      }
      if (openSgBpmType) {
        openSgBpmType = false;
        ImGui::OpenPopup("##type");
      }
      if (const auto d = describeParam(a.doc.song(), songParam(SongParam::Tempo)))
        ui::ParamTypePopup(songParam(SongParam::Tempo), *d);
    }
    y += bh + tsp::kGap;
    if (const int d = stepper(a, "sgloop", "Loop to", loopAt < 0 ? std::string("--") : trk::hexNum(loopAt), ImVec2(x, y), w, bh,
                              "The entry the song goes back to when it ends (none: it stops)", w)) {
      const int to = loopAt + d;
      Tick loopPos = 0;
      for (const Marker& m : s.markers)
        if (m.action == MarkerAction::SongLoop) loopPos = m.pos;
      if (to < 0 && loopAt >= 0) ops::addSongLoopMarker(a.doc, loopPos);  // placed where it is: removed
      else if (to >= 0 && to < static_cast<int>(ol.size())) ops::addSongLoopMarker(a.doc, ol[static_cast<size_t>(to)].start);
    }
    y += bh + tsp::kGroup;
    const float hw = std::floor((w - kGap) / 2);
    const bool songPlaying = playing && a.engine.mode() == PlayMode::Song;
    if (bevelButton(a, "##sgplay", "Play song", ImVec2(x, y), ImVec2(x + hw, y + bh), songPlaying, "Plays the song from the current entry (Enter)")) {
      trackerview::songMode(a);
      if (!playing) menuparity::playPressed(a);
    }
    if (bevelButton(a, "##sgstop", "Stop", ImVec2(x + hw + kGap, y), ImVec2(x + w, y + bh), false, "Stops playing") && playing)
      menuparity::stopPressed(a);
    y += bh + kGap;
    // Loop: the marked block (Shift+Up/Down, Shift+click), or the current entry, plays over and over;
    // lit while a loop is set (again: off). The same loop as the playlist's time selection.
    {
      const bool looping = lLo <= lHi;
      if (bevelButton(a, "##sgloopblk", looping ? "Loop off" : "Loop block", ImVec2(x, y), ImVec2(x + w, y + bh), looping,
                      keys::withKeys(i18n::N_("Plays the marked entries (Shift+Up/Down) or the current one over and over; again: the whole song (%s)"), {keys::fixedKey("Ctrl+L")})) ||
          (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) &&
           ImGui::IsKeyPressed(ImGuiKey_L, false))) {
        if (looping) {
          setSongLoop(a, 0, 0);
        } else if (cur >= 0) {
          const int lo = mLo <= mHi ? mLo : cur, hi = mLo <= mHi ? mHi : cur;
          setSongLoop(a, ol[static_cast<size_t>(lo)].start, ol[static_cast<size_t>(hi)].end);
          if (playing && a.engine.mode() == PlayMode::Song && (trackerview::songPosition(a) < ol[static_cast<size_t>(lo)].start ||
                                                                trackerview::songPosition(a) >= ol[static_cast<size_t>(hi)].end))
            goEntry(a, ol, lo);
        }
      }
    }
    y += bh + tsp::kGroup;
    // cp22: the box's empty lower part tells about the current entry and the picked pattern (what the
    // order and pattern lists hold in their narrow columns), section by section while they fit.
    auto section = [&](const char* title) {
      dl->AddRectFilled(ImVec2(x, std::round(y + rh * 0.5f)), ImVec2(x + w, std::round(y + rh * 0.5f) + 1), theme::withAlpha(P.textFaint, 0.35f));
      const float tw = bold->CalcTextSizeA(fs, FLT_MAX, 0, title).x;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tw + 8, y + rh), P.surface700);
      dl->AddText(bold, fs, ImVec2(x, y + std::round((rh - fs) * 0.5f)), P.text, title);
      y += rh;
    };
    const float lineH = rh;
    if (cur >= 0 && cur < static_cast<int>(ol.size()) && y + lineH * 6 < bottom) {
      const trk::OrderEntry& e = ol[static_cast<size_t>(cur)];
      const std::string title = std::string(i18n::tr("Entry")) + " " + trk::hexNum(cur);
      section(title.c_str());
      const Pattern* ep = s.pattern(e.pattern);
      line(i18n::N_("Pattern"), ep ? trk::hexNum(trk::patternNumber(s, ep->id)) + " " + ep->name : std::string(i18n::tr("audio only")));
      line(i18n::N_("Starts"), timeText(s, e.start));
      line(i18n::N_("Bars"), countText(a, static_cast<int>((e.end - e.start + bar - 1) / bar)));
      line(i18n::N_("Rows"), ep ? countText(a, std::min(255, trk::gridOf(s, *ep).rows)) : std::string("--"));
      if (y + lineH * 2 < bottom) {
        line(i18n::N_("Layers"), std::to_string(std::max(1, e.patterns)) + (e.audio.empty() ? std::string() : " + " + std::to_string(e.audio.size()) + " " + i18n::tr("audio")));
      }
      y += tsp::kGroup;
    }
    if (sp && y + lineH * 5 < bottom) {
      const std::string title = std::string(i18n::tr("Pattern")) + " " + trk::hexNum(trk::patternNumber(s, sp->id));
      section(title.c_str());
      int used = 0, firstUse = -1;
      for (size_t k = 0; k < ol.size(); ++k) {
        if (ol[k].pattern == sp->id) {
          ++used;
          if (firstUse < 0) {
            firstUse = static_cast<int>(k);
          }
        }
      }
      line(i18n::N_("Bars"), countText(a, sp->bars));
      line(i18n::N_("Rows"), countText(a, std::min(255, trk::gridOf(s, *sp).rows)));
      line(i18n::N_("Used"), firstUse < 0 ? std::string(i18n::tr("not in the song")) : std::to_string(used) + "x, " + i18n::tr("first at") + " " + trk::hexNum(firstUse));
      y += tsp::kGroup;
    }
    bool layered = false;
    for (const trk::OrderEntry& e : ol) layered = layered || e.patterns > 1 || e.pattern == kNoId;
    if (layered && y + rh * 2 < bottom)
      dl->AddText(a.fontRegular, fs, ImVec2(x, y), P.textDim,
                  i18n::tr("Some entries layer several patterns or audio: the arranger shows their top pattern, and Lanes edits them."), nullptr, w);
  }
  // ---- the timeline ----
  {
    // cp17 (user): the timeline zooms (Ctrl+wheel at the pointer, the zoom keys, - + Fit in the
    // order list's title) and scrolls (the wheel, the scroll bar under it); it brings the entry
    // picked into view, and while the song plays, the play position.
    const float y0 = bottom + kGap, y1 = o.y + av.y - tlSb;
    bevelBox(dl, ImVec2(o.x, y0), ImVec2(o.x + W, y1), true);
    const Tick len = ol.empty() ? 0 : ol.back().end;
    const float x0 = o.x + pad, x1 = o.x + W - pad, by0 = y0 + pad, by1 = y1 - pad;
    const double zoom = std::clamp(a.settings.getNumber("tracker.timelineZoom"), 1.0, 512.0);
    const double span = len > 0 ? static_cast<double>(len) / zoom : 1.0;
    gTlLen = static_cast<double>(len), gTlSpan = span;
    if (len > 0) {
      auto reveal = [&](double t0, double t1) {
        if (t0 < gTlStart) gTlStart = t0;
        if (t1 > gTlStart + span) gTlStart = std::min(t0, t1 - span);
      };
      if (cur != gTlCur && cur >= 0) reveal(static_cast<double>(ol[static_cast<size_t>(cur)].start), static_cast<double>(ol[static_cast<size_t>(cur)].end));
      gTlCur = cur;
      if (playing) {
        const double p = static_cast<double>(trackerview::songPosition(a));
        if (p < gTlStart || p > gTlStart + span) gTlStart = p - span * 0.1;
      }
      if (gTlToEnd) gTlStart = static_cast<double>(len) - span, gTlToEnd = false;
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsMouseHoveringRect(ImVec2(o.x, y0), ImVec2(o.x + W, y1 + tlSb)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if ((io.KeyCtrl || io.KeySuper) && io.MouseWheel != 0) {
          // The tick under the pointer stays under it.
          const float mx = std::clamp(io.MousePos.x, x0, x1);
          const double at = gTlStart + (mx - x0) / std::max(1.0f, x1 - x0) * span;
          const double nz = std::clamp(zoom * std::pow(1.25, static_cast<double>(io.MouseWheel)), 1.0, 512.0);
          a.settings.set("tracker.timelineZoom", nz);
          gTlStart = at - (mx - x0) / std::max(1.0f, x1 - x0) * (static_cast<double>(len) / nz);
        } else if (io.MouseWheel != 0 || io.MouseWheelH != 0) {
          gTlStart -= static_cast<double>(io.MouseWheelH != 0 ? io.MouseWheelH : io.MouseWheel) * span * 0.1;
        }
      }
      gTlStart = std::clamp(gTlStart, 0.0, std::max(0.0, static_cast<double>(len) - span));
      const double k = (x1 - x0) / span;
      const double t0v = gTlStart;
      dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
      for (size_t i = 0; i < ol.size(); ++i) {
        const trk::OrderEntry& e = ol[i];
        if (static_cast<double>(e.end) < t0v || static_cast<double>(e.start) > t0v + span) continue;  // (only the entries in view)
        const float bx0 = std::round(x0 + static_cast<float>((static_cast<double>(e.start) - t0v) * k)),
                    bx1 = std::max(bx0 + 2, std::round(x0 + static_cast<float>((static_cast<double>(e.end) - t0v) * k)) - 1);
        const Pattern* p = s.pattern(e.pattern);
        const ImU32 c = p ? theme::rgb(p->color) : P.surface600;
        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), theme::withAlpha(c, static_cast<int>(i) == cur ? 0.85f : 0.45f));
        if (static_cast<int>(i) == cur) dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), P.hot);
        if (bx1 - bx0 > fs * 2) {  // (cp25: an audio part is named too, as the order list names it)
          dl->PushClipRect(ImVec2(bx0 + 2, by0), ImVec2(bx1 - 2, by1), true);
          dl->AddText(a.fontRegular, fs, ImVec2(bx0 + 4, std::round(by0 + (by1 - by0 - fs) * 0.5f)), p ? P.text : P.textDim,
                      p ? p->name.c_str() : i18n::tr("Audio part"));
          dl->PopClipRect();
        }
        const float cx0 = std::max(bx0, x0), cx1 = std::min(bx1, x1);  // (the part inside the strip takes the click)
        ImGui::SetCursorScreenPos(ImVec2(cx0, by0));
        ImGui::PushID(static_cast<int>(800 + i));
        if (ImGui::InvisibleButton("##tl", ImVec2(std::max(1.0f, cx1 - cx0), std::max(1.0f, by1 - by0)))) goEntry(a, ol, static_cast<int>(i));
        ImGui::PopID();
      }
      auto xAt = [&](Tick t) { return std::round(x0 + static_cast<float>((static_cast<double>(t) - t0v) * k)); };
      if (lLo <= lHi) {  // the loop: a bar over its entries
        const float lx0 = xAt(ol[static_cast<size_t>(lLo)].start);
        const float lx1 = xAt(ol[static_cast<size_t>(lHi)].end) - 1;
        dl->AddRectFilled(ImVec2(lx0, y0 + 1), ImVec2(lx1, y0 + 3), P.led);
        dl->AddRectFilled(ImVec2(lx0, y1 - 3), ImVec2(lx1, y1 - 1), P.led);
      }
      const Tick pos = trackerview::songPosition(a);
      const float px = xAt(std::min(pos, len));
      dl->AddLine(ImVec2(px, y0 + 1), ImVec2(px, y1 - 1), P.playhead, 2.0f);
      dl->PopClipRect();
      // The scroll bar (a bar's worth an arrow step; the whole song in view: the thumb fills it).
      const double barT = static_cast<double>(bar);
      scrollBar(a, "##tlsb", ImVec2(o.x, y1), ImVec2(o.x + W, y1 + tlSb), &gTlStart, span, static_cast<double>(len), std::max(barT, span * 0.1),
                "The song's timeline: drag to scroll it (the wheel too) \xC2\xB7 Ctrl+wheel: zoom in or out at the pointer");
    }
  }
  // ---- keys (the page in front, no text typed) ----
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper, shift = ImGui::GetIO().KeyShift;  // (Ctrl, or Cmd on macOS)
    // Up/Down: the entry; Shift: and mark the block; Ctrl: move the entry.
    for (int d : {-1, 1}) {
      if (!ImGui::IsKeyPressed(d < 0 ? ImGuiKey_UpArrow : ImGuiKey_DownArrow)) continue;
      if (ctrl) {
        gMark = -1;
        moveEntry(a, ol, cur, d);
        continue;
      }
      if (shift && cur >= 0 && gMark < 0) gMark = cur;
      if (!shift) gMark = -1;
      goEntry(a, ol, std::clamp(cur + d, 0, std::max(0, static_cast<int>(ol.size()) - 1)));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) gMark = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) gMark = -1, goEntry(a, ol, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) gMark = -1, goEntry(a, ol, static_cast<int>(ol.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) stepEntryPattern(a, ol, cur, -1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) stepEntryPattern(a, ol, cur, 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Insert, false))
      insertEntry(a, ol, cur < 0 ? 0 : cur + 1, cur >= 0 && ol[static_cast<size_t>(cur)].pattern != kNoId ? ol[static_cast<size_t>(cur)].pattern : a.curPattern);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) removeEntry(a, ol, cur);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      trackerview::songMode(a);
      if (!playing) menuparity::playPressed(a);
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y - 1));
  ImGui::Dummy(ImVec2(1, 1));
}

}  // namespace

void pageTitle(App& a, const char* title, const std::string& info) {
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = titleBar(a, o, w, title, info);
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + kGap));
}

// ---- cp16: the Roll page's tool row (the band's bevel buttons, no icons or drop-down boxes) ----
void rollToolbar(App& a) {
  const trackerview::PerfScope perfScope(trackerview::PerfPage);  // cp16: "tracker perf"
  const auto& P = pal();
  PianoRollState& st = a.piano;
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float W = ImGui::GetContentRegionAvail().x, h = std::round(fs + 8);
  bevelBox(dl, o, ImVec2(o.x + W, o.y + h));
  const float y0 = o.y + 2, y1 = o.y + h - 2;
  float x = o.x + 2;
  auto width = [&](const char* label) { return std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x) + 16; };
  auto btn = [&](const char* id, const char* label, bool on, const char* hint) {
    const float w = width(label);
    const bool c = bevelButton(a, id, label, ImVec2(x, y0), ImVec2(x + w, y1), on, hint);
    x += w + kGap;
    return c;
  };
  auto label = [&](const char* text) {
    x += 8;
    dl->AddText(a.fontRegular, fs, ImVec2(x, std::round(o.y + (h - fs) * 0.5f)), P.textDim, i18n::tr(text));
    x += std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(text)).x) + 6;
  };
  if (btn("##trprmenu", "Menu", ImGui::IsPopupOpen("##trprmenupop"),
          "Everything the piano roll offers: file, edit, select, quantize, chords, tools, snap and view"))
    ImGui::OpenPopup("##trprmenupop");
  if (ImGui::BeginPopup("##trprmenupop")) {
    pianoRollMenu(a);
    ImGui::EndPopup();
  }
  label("Tool");
  struct T {
    Tool tool;
    const char* label;
    const char* hint;
  };
  static const T tools[] = {{Tool::Draw, i18n::N_("Draw"), i18n::N_("Draw notes (click: a note, drag: its length)")},
                            {Tool::Paint, i18n::N_("Paint"), i18n::N_("Paint notes along a drag")},
                            {Tool::Select, i18n::N_("Select"), i18n::N_("Select notes with a box")},
                            {Tool::Erase, i18n::N_("Erase"), i18n::N_("Erase the notes clicked or dragged over")},
                            {Tool::Slice, i18n::N_("Slice"), i18n::N_("Cut notes where the drag crosses them")},
                            {Tool::Vibrato, i18n::N_("Vibrato"), i18n::N_("Drag up on a note: vibrato depth; sideways or the wheel: its rate; right-click: shape and presets")}};
  for (const T& t : tools) {
    ImGui::PushID(static_cast<int>(t.tool));
    if (btn("##trprtool", t.label, st.tool == t.tool, t.hint)) st.tool = t.tool;
    ImGui::PopID();
  }
  label("Snap");
  struct S {
    Snap snap;
    const char* label;
    const char* hint;
  };
  static const S snaps[] = {{Snap::Line, i18n::N_("Grid"), i18n::N_("Notes snap to the grid lines shown (they follow the zoom)")},
                            {Snap::Step, i18n::N_("Row"), i18n::N_("Notes snap to the pattern's rows (its steps)")},
                            {Snap::Beat, i18n::N_("Beat"), i18n::N_("Notes snap to beats")},
                            {Snap::Bar, i18n::N_("Bar"), i18n::N_("Notes snap to bars")},
                            {Snap::None, i18n::N_("Off"), i18n::N_("No snap: notes go where they are put")}};
  bool listed = false;
  for (const S& sn : snaps) {
    ImGui::PushID(100 + static_cast<int>(sn.snap));
    if (btn("##trprsnap", sn.label, st.snap == sn.snap, sn.hint)) st.snap = sn.snap;
    ImGui::PopID();
    listed = listed || st.snap == sn.snap;
  }
  // GitHub #58: every other grid (step and beat fractions, dotted values, tuplets, Main) from one
  // more button; it is lit, and reads as the grid, while one of those is chosen.
  {
    const char* more = listed ? i18n::N_("More") : snapmenu::label(st.snap);
    const float w = width(more);
    const ImVec2 at(x, y1);
    if (bevelButton(a, "##trprsnapmore", more, ImVec2(x, y0), ImVec2(x + w, y1), !listed || ImGui::IsPopupOpen("##trprsnapmorepop"),
                    "More grids: step and beat fractions, dotted values, quintuplets and septuplets, or Main (the playlist's snap)"))
      ImGui::OpenPopup("##trprsnapmorepop");
    x += w + kGap;
    ImGui::SetNextWindowPos(at);
    if (ImGui::BeginPopup("##trprsnapmorepop")) {
      snapmenu::items(st.snap, true, true);
      ImGui::EndPopup();
    }
  }
  // The instrument: arrows and its number and name (the notes edited are its notes).
  const float zw = width("-") + width("+") + kGap + 8;
  const float right = o.x + W - 2 - zw;
  label("Instrument");
  {
    const float aw = std::round(h * 1.1f);
    const int ci = s.channelIndex(a.selChannel), n = static_cast<int>(s.channels.size());
    if (trackerview::arrowButton(a, "##trprinsprev", ImVec2(x, y0), ImVec2(x + aw, y1), true, "The previous instrument") && ci > 0)
      a.selectChannel(s.channels[static_cast<size_t>(ci - 1)].id);
    x += aw + kGap;
    if (trackerview::arrowButton(a, "##trprinsnext", ImVec2(x, y0), ImVec2(x + aw, y1), false, "The next instrument") && ci >= 0 && ci + 1 < n)
      a.selectChannel(s.channels[static_cast<size_t>(ci + 1)].id);
    x += aw + kGap;
    const float fx1 = std::max(x + 40, std::min(right - 6, x + fs * 16));
    bevelBox(dl, ImVec2(x, y0), ImVec2(fx1, y1), true);
    if (const Channel* c = s.channel(a.selChannel)) {
      const std::string num = trk::hex2(ci + 1);
      const float ty = std::round(o.y + (h - fs) * 0.5f);
      dl->AddText(mono, fs, ImVec2(x + 5, ty), P.hot, num.c_str());
      float nx = x + 5 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
      // K-450: its instrument's own icon before the name (a built-in instrument).
      if (fx1 - nx > 80 && ui::drawPluginIcon(dl, c->plugin.type, ImVec2(std::round(nx + 6), std::round((y0 + y1) * 0.5f)), std::min(14.0f, std::floor(y1 - y0 - 4)),
                                             ui::PluginIconSurface::Tracker))
        nx += 18;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), fx1 - nx - 4, P.text, c->name.c_str());
    }
    x = fx1 + kGap;
  }
  // K-262: with the Vibrato tool, its defaults (preset, depth, rate) in a popup.
  if (st.tool == Tool::Vibrato) {
    const char* vl = i18n::N_("Vibrato defaults");
    if (x + width(vl) < right) {
      const ImVec2 at(x, y1);
      if (btn("##trprvib", vl, ImGui::IsPopupOpen("##trprvibpop"), "The vibrato a note gets with the Vibrato tool: preset, depth and rate"))
        ImGui::OpenPopup("##trprvibpop");
      ImGui::SetNextWindowPos(at);
      if (ImGui::BeginPopup("##trprvibpop")) {
        pianoVibratoStrip(a);
        ImGui::EndPopup();
      }
    }
  }
  // Zoom at the right end.
  x = std::max(x, right + 8);
  if (btn("##trprzout", "-", false, "Zoom out (PgDn, Ctrl+wheel)")) st.zoomRequest /= 1.5f;
  if (btn("##trprzin", "+", false, "Zoom in (PgUp, Ctrl+wheel)")) st.zoomRequest *= 1.5f;
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + kGap));
  ImGui::Dummy(ImVec2(1, 0));
}

void devOrder(App& a, const std::vector<std::string>& args, int frame) {
  const std::string c = args.size() > 1 ? args[1] : "dump";
  auto num = [&](size_t i, int def) { return i < args.size() ? std::atoi(args[i].c_str()) : def; };
  auto ptn = [&](size_t i) -> ObjectId {  // a pattern by its number (from 1)
    const Song& s = a.doc.song();
    const int n = num(i, 0);
    return n >= 1 && n <= static_cast<int>(s.patterns.size()) ? s.patterns[static_cast<size_t>(n - 1)].id : kNoId;
  };
  const trackerorder::List olHeld = trackerorder::of(a, a.doc.song());  // K-142: cached per song change
  const std::vector<trk::OrderEntry>& ol = *olHeld;
  if (c == "go") {
    gMark = -1;
    goEntry(a, ol, num(2, 0));
  } else if (c == "ins") {
    const int i = num(2, entryAt(a, ol));
    insertEntry(a, ol, i < 0 ? 0 : i + 1, i >= 0 && i < static_cast<int>(ol.size()) && ol[static_cast<size_t>(i)].pattern != kNoId ? ol[static_cast<size_t>(i)].pattern : a.curPattern);
  } else if (c == "del") {
    removeEntry(a, ol, num(2, entryAt(a, ol)));
  } else if (c == "move") {
    moveEntry(a, ol, num(2, 0), num(3, 1) < 0 ? -1 : 1);
  } else if (c == "set") {
    setEntryPattern(a, ol, num(2, 0), ptn(3));
  } else if (c == "append") {
    insertEntry(a, ol, static_cast<int>(ol.size()), ptn(2));
  } else if (c == "new") {
    const Song& s = a.doc.song();
    const trackerview::NewPatternSize ns = trackerview::newPatternSize(a);  // HUNT2-1119: as long as the current pattern
    if (const ObjectId id = a.doc.addPattern(std::string(i18n::tr("Pattern")) + " " + std::to_string(s.patterns.size() + 1), ns.bars, ns.steps)) {
      a.selectPattern(id);
    }
  } else if (c == "clone") {
    if (const ObjectId p = ptn(2))
      if (const ObjectId id = a.doc.clonePattern(p)) a.selectPattern(id);
  } else if (c == "bars") {
    if (const Pattern* p = a.doc.song().pattern(ptn(2)))
      trk::setPatternLengthInSong(a.doc, p->id, std::clamp(p->bars + (num(3, 1) < 0 ? -1 : 1), 1, 64), p->stepsPerBar);
  } else if (c == "loop") {
    if (args.size() > 2 && args[2] == "off") setSongLoop(a, 0, 0);
    else {
      const int i0 = std::clamp(num(2, 0), 0, std::max(0, static_cast<int>(ol.size()) - 1)), i1 = std::clamp(num(3, i0), i0, std::max(0, static_cast<int>(ol.size()) - 1));
      if (!ol.empty()) setSongLoop(a, ol[static_cast<size_t>(i0)].start, ol[static_cast<size_t>(i1)].end);
    }
  } else if (c == "play") {
    goEntry(a, ol, num(2, 0));
    trackerview::songMode(a);
    if (!a.engine.playing()) menuparity::playPressed(a);
  }
  const Song& s = a.doc.song();
  const trackerorder::List nowHeld = trackerorder::of(a, s);  // K-142: cached per song change
  const std::vector<trk::OrderEntry>& now = *nowHeld;
  std::string line;
  for (const trk::OrderEntry& e : now) line += " " + (e.pattern != kNoId ? trk::hexNum(trk::patternNumber(s, e.pattern)) : std::string("AU"));
  int lLo, lHi;
  loopedEntries(a, now, &lLo, &lHi);
  std::printf("order frame %d (%s): n %zu |%s | patterns %zu | loop %d-%d | pos %d | playing %d\n", frame, c.c_str(), now.size(), line.c_str(), s.patterns.size(), lLo,
              lHi, entryAt(a, now), a.engine.playing() ? 1 : 0);
  std::fflush(stdout);
}

// ---- cp16: the Audio page with nothing open: what it edits and the way to a sample ----
void audioEmpty(App& a, float w, float h) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), rh = rowHeight();
  const ImVec2 t0 = ImGui::GetCursorScreenPos();
  const float th = titleBar(a, t0, w, i18n::tr("Audio"), "");
  const ImVec2 o(t0.x, t0.y + th + kGap);
  const ImVec2 p1(o.x + w, t0.y + std::max(h, th + rh * 6));
  bevelBox(dl, o, p1, true);
  const Channel* c = s.channel(a.selChannel);
  const float x = o.x + 14, tw = std::max(40.0f, w - 28);
  float y = o.y + 12;
  boxTitle(a, ImVec2(x, y), "No sample open");
  y += rh + 4;
  std::string what;
  bool hasFile = false;
  if (!c) {
    what = i18n::tr("No instrument is selected: pick one in the header's list.");
  } else {
    const std::string head = trk::hex2(s.channelIndex(c->id) + 1) + " " + c->name + ": ";
    hasFile = c->plugin.type == "gl.sampler" && !c->plugin.data.empty();
    if (hasFile) what = head + i18n::tr("its sample is ready to edit.");
    else if (c->plugin.type == tracker::kInstrumentType) what = head + i18n::tr("a Tracker Sampler: its samples are edited in its instrument editor and the Sample tools.");
    else what = head + i18n::tr("no sample (its sound is synthesized).");
  }
  dl->AddText(a.fontRegular, fs, ImVec2(x, y), P.text, what.c_str(), nullptr, tw);
  y += a.fontRegular->CalcTextSizeA(fs, FLT_MAX, tw, what.c_str()).y + 6;
  dl->AddText(a.fontRegular, fs, ImVec2(x, y), P.textDim, i18n::tr("The audio editor edits the current instrument's sample; Files loads one into it."), nullptr, tw);
  y += a.fontRegular->CalcTextSizeA(fs, FLT_MAX, tw, i18n::tr("The audio editor edits the current instrument's sample; Files loads one into it.")).y + 10;
  float bx = x;
  const float bh = std::round(rh + 4);
  auto btn = [&](const char* id, const char* label, const char* hint) {
    const float bw = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x) + 28;
    const bool clicked = bevelButton(a, id, label, ImVec2(bx, y), ImVec2(bx + bw, y + bh), false, hint);
    bx += bw + 4;
    return clicked;
  };
  if (hasFile && btn("##aued", "Edit its sample", "Opens the instrument's sample here")) trackerview::showPage(a, "Loupe");
  if (btn("##aufiles", "Files", "The header shows the files: a double-click on an audio file loads it into the instrument")) trackerview::setHeaderSwap(2);
  if (c && btn("##auinst", "Instrument", "The instrument's editor (Channel settings)")) trackerview::showPage(a, "Instrument");
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(w, p1.y - o.y));
}

// ---- Steps: the channels and their steps ----
GL_STEPS_FN void steps(App& a) {
  const trackerview::PerfScope perfScope(trackerview::PerfPage);  // cp16: "tracker perf"
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const int nSteps = pat ? std::max(1, pat->stepsPerBar * pat->bars) : 16;
  const float th = titleBar(a, o, av.x, i18n::tr("Steps"), stepsInfo(s, pat, nSteps));
  const float rh0 = rowHeight(), y0 = o.y + th + kGap;
  const ImVec2 b0(o.x, y0);
  ImVec2 b1(o.x + av.x, o.y + std::max(av.y, th + rh0 * 3));
  // cp24 (usability): a pattern with more steps than fit at the least step width (8 px) shows a part
  // of them, whole bars, with the band's scroll bar under the rows (before, the steps ran on past the
  // page's edge: a 256-step pattern showed about 120 and the rest could not be reached).
  static double stepFirst = 0;
  const float fs = ImGui::GetFontSize();
  const float sbH = std::max(12.0f, std::round(fs * 0.95f));
  const float group = 3;  // extra gap every 4 steps
  int view = nSteps;
  {
    const float Wg = av.x - 2 - bodyBarW() - trackerview::space::kLine;  // (as the body measures it, with room for its own bar)
    const float nameW = std::min(200.0f, std::round(Wg * 0.22f));
    const float availG = Wg - 4 - nameW - kGap - (rh0 - 1) - trackerview::space::kGroup;
    const int fit = static_cast<int>(std::floor((availG + group) / (9.0f + group / 4.0f)));
    const int bar = pat ? std::max(1, pat->stepsPerBar) : 16;
    if (fit < nSteps) {
      // Paged, the steps get a comfortable width (12 px), whole bars in view.
      const int fitWide = static_cast<int>(std::floor((availG + group) / (13.0f + group / 4.0f)));
      view = std::max(bar, fitWide / bar * bar);
      b1.y -= sbH + kGap;
    }
  }
  int first = view < nSteps ? std::clamp(static_cast<int>(std::lround(stepFirst)), 0, nSteps - view) : 0;
  // cp24 (usability): a few channels: the rows grow (up to half again) so the page has no empty lower
  // part, as the Mix page's.
  const float rh = std::max(rh0, std::min(std::round(rh0 * 1.5f), std::floor((b1.y - b0.y - 4) / static_cast<float>(std::max<size_t>(1, s.channels.size())))));
  const float contentH = static_cast<float>(s.channels.size()) * rh + 4;
  // The step being played (pattern mode on this pattern, or this pattern under the song position).
  int playStep = -1;
  if (pat && a.engine.playing()) {
    Tick t = -1;
    if (a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == pat->id) t = static_cast<Tick>(a.engine.position());
    else if (a.engine.mode() == PlayMode::Song) t = trk::patternTickAt(s, pat->id, static_cast<Tick>(a.engine.position()));
    const Tick st = s.ticksPerStep(*pat);
    if (t >= 0 && st > 0) playStep = static_cast<int>((t / st) % nSteps);
  }
  // (cp24: while it plays, the part in view follows the step played)
  if (view < nSteps && playStep >= 0 && (playStep < first || playStep >= first + view)) {
    first = std::min(nSteps - view, playStep / view * view);
    stepFirst = first;
  }
  float stepsX = o.x;  // (cp24: where the steps start: their scroll bar starts there too)
  if (beginBody("##steps", b0, b1, contentH)) {
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    // cp23: the lamp as tall as the name field beside it; cp24: a square of the plain row height, centred
    // in a grown row (a 23 px green block read heavy).
    const float nameW = std::min(200.0f, std::round(W * 0.22f)), lampW = rh0 - 1;
    const float gx = c0.x + 2 + nameW + kGap + lampW + trackerview::space::kGroup;
    stepsX = gx;
    const float avail = c0.x + W - 2 - gx;  // cp23: 2 px from the edge, as the names (was 4)
    const float cw = std::max(8.0f, std::floor((avail - group * static_cast<float>(view / 4)) / static_cast<float>(view)) - 1);
    // cp24: what the whole cells leave goes into the gaps between the beats (evenly, whole pixels), so the
    // last step ends at the row's right edge (it ended up to 110 px short at 1440x900).
    const int beats = std::max(1, view / 4);
    const float spare = avail - static_cast<float>(view) * (cw + 1) + 1 - group * static_cast<float>(beats - 1);
    const float gap = beats > 1 ? group + std::max(0.0f, std::floor(spare / static_cast<float>(beats - 1))) : group;
    const float cellsW = static_cast<float>(view) * (cw + 1) - 1 + gap * static_cast<float>(beats - 1);
    for (size_t ci = 0; ci < s.channels.size(); ++ci) {
      stepRow(a, pat, ci, c0, nameW, lampW, gx, cellsW, rh, cw, gap, first, view, nSteps, playStep);
    }
    endBody(a, contentH);
  }
  if (view < nSteps) {
    double at = first;
    const int bar = pat ? std::max(1, pat->stepsPerBar) : 16;
    if (scrollBar(a, "##stepsb", ImVec2(stepsX, b1.y + kGap), ImVec2(o.x + av.x, b1.y + kGap + sbH), &at, view, nSteps, bar,
                  "The steps in view: a bar at a time (the pattern is longer than the page)")) {
      stepFirst = std::round(at / bar) * bar;
    }
    b1.y += sbH + kGap;
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

// ---- Song: the arrangement as rows of bars, a column per playlist track ----
// The clip picked on the Song page (a click), its properties in the clip box under the rows.
ObjectId gSongClip = kNoId;
bool gSongLanes = false;  // the Song page shows the lanes (else the arranger); view state, never saved

namespace {
std::string clipTitle(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    const Pattern* p = s.pattern(c.pattern);
    return p ? trk::hexNum(trk::patternNumber(s, p->id)) + " " + p->name : std::string(i18n::tr("Pattern"));
  }
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->name : std::string(i18n::tr("Audio clip"));
  }
  const AutomationClip* au = s.automation(c.source);
  return au ? au->name : std::string(i18n::tr("Automation"));
}

// The clip box: the picked clip's properties, edited in place, each change one Document edit (the
// same edits the Studio playlist's clip menu and edge drags make, so every layout shows the same
// clip; nothing is stored that the other layouts lack). T-13: where it starts, how long it is and
// where its content starts inside it (the offset), its mute; an audio clip's gain, fades, pitch and
// direction; a pattern clip's playback transpose. Each value is a stepper (the arrows, the wheel, or
// a drag on the value): positions in rows of the song grid, Shift a whole bar. "More" opens the
// clip's whole menu (warp, audio tools, curves, export, as in the playlist).
// A note the clip box shows in its title row for a moment (instead of a toast over its fields).
std::string gBoxNote;
double gBoxNoteUntil = 0;

float clipBoxHeight() { return std::round(ImGui::GetFontSize() + 4) * 4 + 4 * 5 + 2; }

// bars:beats:rows of a length or an offset, from 0 ("+": and some ticks more).
std::string barsText(Tick t, Tick bar, Tick beat, Tick row) {
  char b[96];  // (room for three 64-bit numbers: GCC -Wformat-truncation)
  const Tick bars = t / bar, beats = (t % bar) / beat, rows = (t % beat) / row, rest = t % row;
  std::snprintf(b, sizeof b, "%lld:%lld:%02lld%s", static_cast<long long>(bars), static_cast<long long>(beats), static_cast<long long>(rows), rest ? "+" : "");
  return b;
}

void clipBox(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Clip* c = s.clip(gSongClip);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bevelBox(dl, p0, p1);
  if (!c) return;
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ObjectId id = c->id;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick beat = std::max<Tick>(1, s.ppq);
  const Tick row = std::max<Tick>(1, s.ppq / 4);  // one row of the song grid (four to the beat)
  static TempoMap tm;
  static uint64_t tmRev = ~0ull;
  if (tmRev != a.doc.revision()) tm.build(s), tmRev = a.doc.revision();
  const bool shift = ImGui::GetIO().KeyShift;
  const Tick unit = shift ? bar : row;
  static uint64_t key = 0x54524b00;
  const float rh = std::round(fs + 4), pad = 4;
  const float x0 = p0.x + 6, y0 = p0.y + pad;
  // Title: kind, name; the track at the right.
  const char* kind = c->kind == ClipKind::Audio ? i18n::tr("Audio clip") : c->kind == ClipKind::Pattern ? i18n::tr("Pattern clip") : i18n::tr("Automation clip");
  const float ty = std::round(y0 + (rh - fs) * 0.5f);
  boxTitle(a, ImVec2(x0, ty), kind);
  const float kw = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, kind).x) + 10;
  char where[48];
  std::snprintf(where, sizeof where, "%s %s", i18n::tr("Track"), trk::hex2(c->track + 1).c_str());
  const float ww = mono->CalcTextSizeA(fs, FLT_MAX, 0, where).x;
  const bool note = !gBoxNote.empty() && a.now < gBoxNoteUntil;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + kw, ty), p1.x - 12 - ww - (x0 + kw) - 10, note ? P.hot : P.text, note ? gBoxNote.c_str() : clipTitle(s, *c).c_str());
  dl->AddText(mono, fs, ImVec2(std::round(p1.x - 6 - ww), ty), P.textDim, where);
  ImGui::PushID("##songclipbox");
  // The fields: a grid of steppers, four to a row (a narrow page: as many as fit).
  const float inner = p1.x - p0.x - 12;
  const int perRow = std::clamp(static_cast<int>(inner / std::max(fs * 13.0f, 160.0f)), 2, 4);
  const float cellW = std::floor((inner - static_cast<float>(perRow - 1) * 10) / static_cast<float>(perRow));
  int n = 0;
  auto cell = [&](ImVec2* at) {
    const int r = 1 + n / perRow, k = n % perRow;
    *at = ImVec2(std::round(x0 + static_cast<float>(k) * (cellW + 10)), std::round(y0 + static_cast<float>(r) * (rh + pad)));
    ++n;
    return at->y + rh <= p1.y - 2;  // (a short box shows the fields that fit)
  };
  const float lw = std::round(std::min(cellW * 0.42f, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Fade out")).x + 10));
  auto field = [&](const char* sid, const char* label, const std::string& value, const char* hint) {
    ImVec2 at;
    if (!cell(&at)) return 0;
    return trackerview::stepper(a, sid, label, value, at, cellW, rh, hint, lw, true);
  };
  auto toggle = [&](const char* sid, const char* label, bool on, const char* hint) {
    ImVec2 at;
    if (!cell(&at)) return false;
    return bevelButton(a, sid, label, at, ImVec2(at.x + std::min(cellW, std::round(fs * 6)), at.y + rh), on, hint);
  };
  // Start: the whole clip moves (its content with it).
  if (const int d = field("start", "Start", menuparity::positionText(a, static_cast<double>(c->start)),
                          "Where the clip starts in the song (a row a step, Shift: a bar; drag the value for more)")) {
    const Tick to = std::max<Tick>(0, c->start + unit * d);
    if (to != c->start) a.doc.editClip(id, "move clip", [to](Clip& x) { x.start = to; }, key + 10);
  }
  // Length: the right edge (audio stops at the end of its audio, as the playlist's edge drag).
  const Tick len = s.clipLength(*c);
  if (const int d = field("len", "Length", barsText(len, bar, beat, row), "How long the clip plays, in bars:beats:rows (a row a step, Shift: a bar)")) {
    const Tick by = std::max<Tick>(row, len + unit * d) - len;
    if (by != 0) {
      const std::vector<Clip> after = pltools::resizeEnds(s, s.playlist, {id}, by, false, &tm);
      // R3-08: an audio clip at the end of its audio grows no further: say so.
      if (by > 0 && c->kind == ClipKind::Audio)
        for (const Clip& x : after)
          if (x.id == id && s.clipLength(x) <= len) {
            // R4-08: said in the box's title row (a toast would cover its fields); the Offset hint
            // only when an earlier offset has audio to give.
            gBoxNote = i18n::tr(c->audioStart > 0.0005 ? "The audio ends there: the clip plays no longer (an earlier Offset gives it more)"
                                                       : "The audio ends there: the clip plays no longer");
            gBoxNoteUntil = a.now + 4.0;
          }
      a.doc.editClips("resize clips", [&after](std::vector<Clip>& cs) {
        if (cs.size() == after.size()) cs = after;
      }, key + 11);
    }
  }
  // Offset: where the content starts inside the clip (the playlist's slip edit).
  {
    std::string off;
    if (c->kind == ClipKind::Audio) {
      char b[32];
      std::snprintf(b, sizeof b, "%.3f s", c->audioStart);
      off = b;
    } else {
      off = barsText(c->offset, bar, beat, row);
    }
    if (const int d = field("off", "Offset", off,
                            c->kind == ClipKind::Audio ? "Where the clip's audio starts in its sample (a row a step, Shift: a bar)"
                                                       : "Where the clip's content starts inside it (a row a step, Shift: a bar)")) {
      const Clip after = pltools::slipClip(s, tm, *c, -unit * d);
      a.doc.editClip(id, "slip clip", [after](Clip& x) { x = after; }, key + 12);
    }
  }
  if (toggle("##mute", "Mute", c->muted, "The clip stays in place, silent")) {
    const bool m = !c->muted;
    a.doc.editClip(id, "mute clip", [m](Clip& x) { x.muted = m; });
  }
  if (c->kind == ClipKind::Audio) {
    n = std::max(n, perRow);  // the audio's own values from the next row (R3-07: n counts from the first field row)
    char g[32];
    const double db = clipgain::toDb(c->gain);
    if (db < clipgain::kMinDb) std::snprintf(g, sizeof g, "-inf dB");
    else std::snprintf(g, sizeof g, "%+.1f dB", db);
    if (const int d = field("gain", "Gain", g, "The clip's volume (0.5 dB a step, Shift: 6 dB)")) {
      const double base = db < clipgain::kMinDb ? clipgain::kMinDb - 0.1 : db;
      const double to = std::clamp(std::round((base + (shift ? 6.0 : 0.5) * d) * 10.0) / 10.0, clipgain::kMinDb - 0.1, clipgain::kMaxDb);
      a.doc.editClip(id, "clip gain", [to](Clip& x) { x.gain = clipgain::fromDb(to); }, key);
    }
    auto ms = [](double sec) {
      char b[32];
      std::snprintf(b, sizeof b, "%.0f ms", sec * 1000.0);
      return std::string(b);
    };
    const double fstep = shift ? 0.1 : 0.01;
    if (const int d = field("fi", "Fade in", ms(c->fadeIn), "The fade at the clip's start (10 ms a step, Shift: 100 ms)")) {
      const double to = std::max(0.0, std::round((c->fadeIn + fstep * d) * 1000.0) / 1000.0);
      a.doc.editClip(id, "clip fade in", [to](Clip& x) { x.fadeIn = to, x.fadeInManual = true; }, key + 2);
    }
    if (const int d = field("fo", "Fade out", ms(c->fadeOut), "The fade at the clip's end (10 ms a step, Shift: 100 ms)")) {
      const double to = std::max(0.0, std::round((c->fadeOut + fstep * d) * 1000.0) / 1000.0);
      a.doc.editClip(id, "clip fade out", [to](Clip& x) { x.fadeOut = to, x.fadeOutManual = true; }, key + 3);
    }
    char pc[32];
    std::snprintf(pc, sizeof pc, "%+.0f ct", static_cast<double>(c->pitch));
    if (const int d = field("pitch", "Pitch", pc, "The clip's pitch in cents (10 a step, Shift: a semitone)")) {
      const float to = std::clamp(std::round(c->pitch + static_cast<float>((shift ? 100 : 10) * d)), -2400.0f, 2400.0f);
      a.doc.editClip(id, "clip pitch", [to](Clip& x) { x.pitch = to; }, key + 1);
    }
    n = std::max(n, perRow * 2);
    if (toggle("##rev", "Reverse", c->reverse, "Plays the clip backwards (keeps the same audio)"))
      a.doc.editClip(id, "reverse clip", [&a](Clip& x) { toggleClipReverse(a.doc.song(), x); });
  } else if (c->kind == ClipKind::Pattern) {
    n = std::max(n, perRow);
    char t[16];
    std::snprintf(t, sizeof t, "%+d st", c->playTranspose);
    if (const int d = field("tr", "Transpose", t, "Plays the pattern's notes this many semitones higher or lower (the pattern stays as written; Shift: an octave)")) {
      const int to = std::clamp(c->playTranspose + d * (shift ? 12 : 1), -48, 48);
      a.doc.editClip(id, "clip transpose", [to](Clip& x) { x.playTranspose = to; }, key + 4);
    }
  } else {
    n = std::max(n, perRow);
  }
  {
    ImVec2 at;
    if (cell(&at) && bevelButton(a, "##more", "More...", at, ImVec2(at.x + std::min(cellW, std::round(fs * 6)), at.y + rh), false,
                                 "The clip's whole menu, as in the playlist: warp markers, audio tools, curves, export"))
      ImGui::OpenPopup("##songclipmore");
  }
  if (c->kind == ClipKind::Audio) {  // K-249: the audio clip editor on the Audio page
    ImVec2 at;
    if (cell(&at) && bevelButton(a, "##editclip", "Edit...", at, ImVec2(at.x + std::min(cellW, std::round(fs * 6)), at.y + rh), false,
                                 "Opens the sample clip in the audio clip editor (on the Audio page)"))
      clipeditor::open(a, id);
  }
  if (ImGui::BeginPopup("##songclipmore")) {
    devArrangementClipMenu(a, id);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  // A held arrow or a dragged value is one undo step; it ends when the mouse lets go.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) a.doc.endGesture();
}
}  // namespace

void song(App& a) {
  const trackerview::PerfScope perfScope(trackerview::PerfPage);  // cp16: "tracker perf"
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const int usedBars = static_cast<int>((s.lengthTicks() + bar - 1) / bar);
  const int bars = std::max(16, usedBars + 8);
  int usedTracks = 0;
  for (const Clip& c : s.playlist) usedTracks = std::max(usedTracks, c.track + 1);
  const int tracks = std::clamp(usedTracks + 1, 4, static_cast<int>(s.tracks.size()));
  char info[128];
  // T-27: the columns past the used tracks are free ones to put clips on; the count says so.
  std::snprintf(info, sizeof info, "%s %s  %s %s  %s %s", i18n::tr("Bars"), trk::hex2(usedBars).c_str(), i18n::tr("Tracks"), trk::hex2(usedTracks).c_str(),
                i18n::tr("Free"), trk::hex2(std::max(0, tracks - usedTracks)).c_str());
  // The arranger's title says its keys (a keyboard musician's reminder).
  // cp17: the sideways zoom's buttons sit before Exit (the lanes' width, or the timeline's zoom).
  const float zh = std::round(fs + 8) - 4, pmW = std::round(zh * 1.3f);
  const float fitBw = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Fit")).x) + 14;
  const float zoomW = pmW * 2 + fitBw + 4 + 8;
  const float bx0 = std::round(6 + a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Song")).x + fs * 1.5f);
  const float arrBtnW = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Arranger")).x) + 20;
  const float lanesBtnW = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Lanes")).x) + 20;
  const std::string curArrName = arrangementName(s, s.arrangements.empty() ? 0 : s.currentArrangement);
  const float arrDropW = std::clamp(std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, curArrName.c_str()).x) + 34.0f, 108.0f, 180.0f);
  const float leftReserve = bx0 + arrBtnW + 2 + lanesBtnW + 8 + arrDropW;
  const float th = titleBar(a, o, av.x, i18n::tr("Song"),
                            gSongLanes ? std::string(info) : std::string(i18n::tr("Up/Down: entry  Shift: mark  Ctrl: move  Left/Right: pattern  Ins Del  Enter: play")),
                            zoomW, leftReserve);
  {
    const float ew = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Exit")).x) + 24;
    float zx = o.x + av.x - 2 - ew - 8 - fitBw;
    const bool fitOn = gSongLanes ? a.settings.getNumber("tracker.songLaneWidth") < 0 : a.settings.getNumber("tracker.timelineZoom") <= 1.0;
    if (bevelButton(a, "##sgfit", "Fit", ImVec2(zx, o.y + 2), ImVec2(zx + fitBw, o.y + 2 + zh), fitOn,
                    gSongLanes ? keys::withKeys(i18n::N_("Every lane in view (%s)"), {keys::cmdKey("view.zoomTracksFit", "Ctrl+Alt+/")}) : keys::withKeys(i18n::N_("The whole song in the timeline (%s)"), {keys::cmdKey("view.zoomTracksFit", "Ctrl+Alt+/")})))
      zoomSong(a, 0);
    zx -= pmW + 2;
    if (bevelButton(a, "##sgzin", "+", ImVec2(zx, o.y + 2), ImVec2(zx + pmW, o.y + 2 + zh), false,
                    gSongLanes ? keys::withKeys(i18n::N_("Wider lanes (%s or Ctrl+wheel up)"), {keys::cmdKey("view.zoomTracksIn", "Ctrl+Alt+.")}) : keys::withKeys(i18n::N_("The timeline zoomed in (%s or Ctrl+wheel up over it)"), {keys::cmdKey("view.zoomTracksIn", "Ctrl+Alt+.")})))
      zoomSong(a, 1);
    zx -= pmW + 2;
    if (bevelButton(a, "##sgzout", "-", ImVec2(zx, o.y + 2), ImVec2(zx + pmW, o.y + 2 + zh), false,
                    gSongLanes ? keys::withKeys(i18n::N_("Narrower lanes, more in view (%s or Ctrl+wheel down)"), {keys::cmdKey("view.zoomTracksOut", "Ctrl+Alt+,")}) : keys::withKeys(i18n::N_("The timeline zoomed out (%s or Ctrl+wheel down over it)"), {keys::cmdKey("view.zoomTracksOut", "Ctrl+Alt+,")})))
      zoomSong(a, -1);
  }
  {  // the arranger or the lanes (the clips on the song's tracks, as they lie), plus the K-406 arrangement switcher
    const float bh = th - 4, bx = o.x + bx0;
    if (bevelButton(a, "##sgarr", "Arranger", ImVec2(bx, o.y + 2), ImVec2(bx + arrBtnW, o.y + 2 + bh), !gSongLanes,
                    "The song as a list of entries, one pattern each, with the pattern bank"))
      gSongLanes = false;
    if (bevelButton(a, "##sglanes", "Lanes", ImVec2(bx + arrBtnW + 2, o.y + 2), ImVec2(bx + arrBtnW + 2 + lanesBtnW, o.y + 2 + bh), gSongLanes,
                    "The clips on the song's tracks as they lie (layered patterns, audio, automation)"))
      gSongLanes = true;
    const float ax = bx + arrBtnW + 2 + lanesBtnW + 8;
    const float ew = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Exit")).x) + 24;
    const float maxDropW = o.x + av.x - 2 - ew - zoomW - 8 - ax;
    if (maxDropW >= 72.0f) {
      ImGui::SetCursorScreenPos(ImVec2(ax, o.y + 2));
      arrangementDropdown(a, "##trkArrDrop", std::min(arrDropW, maxDropW), bh, ArrangementStyle::Tracker);
    }
  }
  if (!gSongLanes) {
    arranger(a, o, av, th);
    return;
  }
  const float rh = rowHeight();
  // Track headers (bevelled), then the scrolled rows.
  const float numW = std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000").x + 10);
  // cp17 (user): every lane is reachable: the lanes scroll sideways (a scroll bar under them,
  // Shift+wheel) and zoom (Ctrl+wheel, the zoom keys, - + Fit in the title): the lane width is a
  // view preference in the settings (0: the lanes share the width, at least 70 px; -1: every lane
  // in view; else px).
  const float fitW = std::max(kFitLaneW, std::floor((av.x - numW - 6) / static_cast<float>(tracks)) - kGap);
  const double lw = a.settings.getNumber("tracker.songLaneWidth");
  const float colW = lw < 0 ? fitW : lw > 0 ? std::clamp(static_cast<float>(lw), kMinLaneW, kMaxLaneW) : std::max(70.0f, fitW);
  gLaneFitW = fitW, gLaneW = colW;
  const float lanesW = static_cast<float>(tracks) * (colW + kGap);
  const float laneView = std::max(1.0f, av.x - numW - 4);
  const bool hbar = lanesW > laneView + 1;
  const float sbH = hbar ? std::max(12.0f, std::round(fs * 0.95f)) : 0.0f;
  double scroll = std::clamp(static_cast<double>(gLaneScroll), 0.0, std::max(0.0, static_cast<double>(lanesW - laneView)));
  const float hy = o.y + th + kGap;
  ImDrawList* wdl = ImGui::GetWindowDrawList();
  wdl->PushClipRect(ImVec2(o.x + numW + 2, hy), ImVec2(o.x + av.x, hy + rh), true);
  for (int t = 0; t < tracks; ++t) {
    const float x = std::round(o.x + numW + 2 + static_cast<float>(t) * (colW + kGap) - static_cast<float>(scroll));
    if (x + colW < o.x + numW || x > o.x + av.x) continue;  // (only the lanes in view are drawn)
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    const std::string name = tr.name.empty() ? std::string(i18n::tr("Track")) + " " + std::to_string(t + 1) : tr.name;
    nameField(a, ImVec2(x, hy), ImVec2(x + colW, hy + rh), t + 1, tr.color, name, false, tr.muted);
  }
  wdl->PopClipRect();
  if (!s.clip(gSongClip)) gSongClip = kNoId;
  const float boxH = gSongClip != kNoId ? clipBoxHeight() : 0.0f;  // the clip box under the rows
  const ImVec2 b0(o.x, hy + rh + kGap), b1(o.x + av.x, o.y + std::max(av.y - (boxH > 0 ? boxH + kGap : 0.0f) - sbH, th + rh * 4));
  // Ctrl+wheel over the lanes zooms (up: wider), Shift+wheel or a sideways wheel scrolls a lane a notch.
  {
    ImGuiIO& io = ImGui::GetIO();
    const bool over = ImGui::IsMouseHoveringRect(ImVec2(o.x, hy), ImVec2(b1.x, b1.y + sbH)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (over && (io.KeyCtrl || io.KeySuper) && io.MouseWheel != 0) {
      gZoomWheel += io.MouseWheel;
      const int n = static_cast<int>(gZoomWheel);
      if (n != 0) gZoomWheel -= static_cast<float>(n), zoomSong(a, n > 0 ? 1 : -1);
    } else if (over && (io.MouseWheelH != 0 || (io.KeyShift && io.MouseWheel != 0))) {
      scroll -= static_cast<double>((io.MouseWheelH != 0 ? io.MouseWheelH : io.MouseWheel) * (colW + kGap));
    }
  }
  if (gLaneReveal >= 0) {  // (a lane asked for: "tracker songzoom lane <n>")
    const double lx = static_cast<double>(gLaneReveal) * (colW + kGap);
    if (lx < scroll) scroll = lx;
    if (lx + colW > scroll + laneView) scroll = lx + colW - laneView;
    gLaneReveal = -1;
  }
  if (hbar) scrollBar(a, "##lanesb", ImVec2(o.x + numW + 2, b1.y), ImVec2(o.x + av.x, b1.y + sbH), &scroll, laneView, lanesW, colW + kGap,
                      "The lanes: drag to scroll them (Shift+wheel too) \xC2\xB7 Ctrl+wheel: wider or narrower lanes");
  scroll = std::clamp(scroll, 0.0, std::max(0.0, static_cast<double>(lanesW - laneView)));
  gLaneScroll = static_cast<float>(scroll);
  gLanesShown = tracks;
  gLaneFirst = std::clamp(static_cast<int>(std::ceil(scroll / (colW + kGap) - 0.01)), 0, tracks - 1);
  gLaneLast = std::clamp(static_cast<int>(std::floor((scroll + laneView + kGap) / (colW + kGap) + 0.01)) - 1, gLaneFirst, tracks - 1);
  const float contentH = static_cast<float>(bars) * rh + 2;
  const Tick pos = a.engine.playing() && a.engine.mode() == PlayMode::Pattern ? trackerview::songPosition(a) : static_cast<Tick>(a.engine.position());
  const int playBar = static_cast<int>(pos / bar);
  (void)wdl;
  static ObjectId menuClip = kNoId;
  if (beginBody("##song", b0, b1, contentH, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper || ImGui::GetIO().KeyShift)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    // Which clip covers (track, bar): the one starting there first, else one running through it.
    // The rows the page really shows: the body's clip, and the page window's own inner rectangle.
    const ImGuiWindow* page = ImGui::GetCurrentWindow()->ParentWindow;
    const float clipY0 = std::max(dl->GetClipRectMin().y, page ? page->InnerClipRect.Min.y : -FLT_MAX);
    const float clipY1 = std::min(dl->GetClipRectMax().y, page ? page->InnerClipRect.Max.y : FLT_MAX);
    // The shown tracks' clips with their ends, once (a big song has thousands; each cell looks only at its track's).
    struct Span {
      const Clip* clip;
      Tick end;
    };
    std::vector<std::vector<Span>> byTrack(static_cast<size_t>(tracks));
    for (const Clip& c : s.playlist)
      if (c.track >= 0 && c.track < tracks) byTrack[static_cast<size_t>(c.track)].push_back({&c, c.start + s.clipLength(c)});
    for (int r = 0; r < bars; ++r) {
      const float y = std::round(c0.y + static_cast<float>(r) * rh);
      if (y >= clipY1 || y + rh <= clipY0) continue;  // scrolled out: nothing drawn (nor laid under the next panel)
      // A row the body's edge cuts keeps its stripes but not its texts (no half letters at the edge).
      const bool textOk = y + (rh + fs) * 0.5f <= clipY1 && y + (rh - fs) * 0.5f >= clipY0;
      if (r % 4 == 0) dl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + W, y + rh), theme::withAlpha(P.surface800, 0.55f));
      if (r == playBar && a.engine.playing()) dl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + W, y + rh), theme::withAlpha(P.playhead, 0.22f));
      const std::string num = trk::hex2(r);
      if (textOk) dl->AddText(mono, fs, ImVec2(c0.x + 4, std::round(y + (rh - fs) * 0.5f)), r % 4 == 0 ? P.text : P.textDim, num.c_str());
      dl->PushClipRect(ImVec2(c0.x + numW, std::max(c0.y, clipY0)), ImVec2(c0.x + W + 1, clipY1), true);  // (lanes scrolled under the numbers are cut there)
      for (int t = gLaneFirst > 0 ? gLaneFirst - 1 : 0; t < tracks; ++t) {
        const float x = std::round(c0.x + numW + 1 + static_cast<float>(t) * (colW + kGap) - static_cast<float>(scroll));
        if (x + colW <= c0.x + numW) continue;   // scrolled away to the left
        if (x >= dl->GetClipRectMax().x) break;  // the tracks past the page's right edge
        const Tick b0t = static_cast<Tick>(r) * bar, b1t = b0t + bar;
        // R3-08, R4-05: the clip drawn in a bar is the longest of those starting in it, else the
        // longest of those running through it; every other clip covering the bar is counted ("+n").
        // A bar whose drawn clip is not the one drawn in the bar above names it (a clip that goes on
        // after the drawn one ended is named where it shows).
        struct Pick {
          const Clip* clip = nullptr;
          bool starts = false;
          int covering = 0;
        };
        auto pick = [&](Tick from, Tick to) {
          Pick k;
          Tick sEnd = 0, rEnd = 0;
          const Clip* runs = nullptr;
          for (const Span& sp : byTrack[static_cast<size_t>(t)]) {
            const Clip& c = *sp.clip;
            if (c.start >= from && c.start < to) {
              ++k.covering;
              if (!k.starts || sp.end > sEnd) k.clip = &c, k.starts = true, sEnd = sp.end;
            } else if (c.start < from && sp.end > from) {
              ++k.covering;
              if (sp.end > rEnd) runs = &c, rEnd = sp.end;
            }
          }
          if (!k.starts) k.clip = runs;
          return k;
        };
        const Pick here = pick(b0t, b1t);
        const Clip* starts = here.starts ? here.clip : nullptr;
        const int stackCount = here.covering;  // the drawn clip and the others under it
        const bool named = here.clip && (here.starts || r == 0 || pick(b0t - bar, b0t).clip != here.clip);
        const float ty = std::round(y + (rh - fs) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(r * 1024 + t);
        const bool click = ImGui::InvisibleButton("##cell", ImVec2(colW, rh));
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();
        if (hov) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rh), theme::withAlpha(P.text, 0.08f));
        const Clip* c = here.clip;
        if (c) {
          const Pattern* cp = c->kind == ClipKind::Pattern ? s.pattern(c->pattern) : nullptr;
          // An audio or automation clip: its channel's or automation's name and colour (no pattern number).
          unsigned col = cp ? cp->color : 0x808080u;
          std::string other;
          if (c->kind == ClipKind::Audio) {
            if (const Channel* ch = s.channel(c->source)) col = ch->color, other = ch->name;
          } else if (!cp) {
            if (const AutomationClip* au = s.automation(c->source)) col = au->color, other = au->name;
          }
          // The clip's bars in a faint wash of its colour, the stripe at the left, the picked clip
          // outlined as one box over all its bars (top on the first, bottom on the last).
          const Tick cEnd = c->start + s.clipLength(*c);
          const bool first = starts != nullptr, last = cEnd <= b1t;
          // T-13: the clip's box starts where it starts inside its first bar and ends where it ends
          // in its last (a clip half a bar in is drawn from the middle of the bar).
          const float yT = first ? std::round(y + rh * static_cast<float>(c->start - b0t) / static_cast<float>(bar)) : y;
          const float yB = last ? std::max(yT + 2, std::round(y + rh * static_cast<float>(cEnd - b0t) / static_cast<float>(bar))) : y + rh;
          dl->AddRectFilled(ImVec2(x + 4, yT), ImVec2(x + colW, yB), theme::withAlpha(theme::rgb(col), 0.12f));
          dl->AddRectFilled(ImVec2(x + 1, yT), ImVec2(x + 4, yB), theme::rgb(col));
          if (c->muted) dl->AddRectFilled(ImVec2(x + 4, yT), ImVec2(x + colW, yB), theme::withAlpha(P.surface950, 0.45f));
          if (c->id == gSongClip) {
            dl->AddRectFilled(ImVec2(x, yT), ImVec2(x + 1, yB), P.hot);
            dl->AddRectFilled(ImVec2(x + colW - 1, yT), ImVec2(x + colW, yB), P.hot);
            if (first) dl->AddRectFilled(ImVec2(x, yT), ImVec2(x + colW, yT + 1), P.hot);
            if (last) dl->AddRectFilled(ImVec2(x, yB - 1), ImVec2(x + colW, yB), P.hot);
          }
          // An audio clip: its fades as ramps over the bars they cover, its gain in the first bar.
          if (c->kind == ClipKind::Audio) {
            const Tick cend = c->start + s.clipLength(*c);
            const double tps = std::max(1e-9, static_cast<double>(s.ppq) * s.tempo / 60.0);  // (the fades are seconds; the song tempo maps them for the eye)
            const Tick fin = c->start + static_cast<Tick>(c->fadeIn * tps), fout = cend - static_cast<Tick>(c->fadeOut * tps);
            const ImU32 ramp = theme::withAlpha(P.led, 0.35f);
            auto frac = [&](Tick t) { return std::clamp(static_cast<float>(t - b0t) / static_cast<float>(bar), 0.0f, 1.0f); };
            if (c->fadeIn > 0 && fin > b0t && c->start < b1t) {  // the rising part inside this bar
              const float f0 = frac(std::max(c->start, b0t)), f1 = frac(std::min(fin, b1t));
              dl->AddTriangleFilled(ImVec2(x + colW - 2, y + rh * f0), ImVec2(x + colW - 2, y + rh * f1), ImVec2(x + colW - 2 - (colW * 0.25f) * f1, y + rh * f1), ramp);
            }
            if (c->fadeOut > 0 && cend > b0t && fout < b1t) {
              const float f0 = frac(std::max(fout, b0t)), f1 = frac(std::min(cend, b1t));
              dl->AddTriangleFilled(ImVec2(x + colW - 2 - colW * 0.25f * (1 - f0), y + rh * f0), ImVec2(x + colW - 2, y + rh * f0), ImVec2(x + colW - 2, y + rh * f1), ramp);
            }
            if (starts && textOk && std::fabs(c->gain - 1.0f) > 1e-4f) {
              char g[24];
              const double db = clipgain::toDb(c->gain);
              if (db < clipgain::kMinDb) std::snprintf(g, sizeof g, "-inf");
              else std::snprintf(g, sizeof g, "%+.1f", db);
              const float gw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, g).x;
              const float mutedW = c->muted ? mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, "M").x + 10 : 0.0f;
              dl->AddText(mono, fs * 0.85f, ImVec2(std::round(x + colW - 6 - gw - colW * 0.12f - mutedW), std::round(y + (rh - fs * 0.85f) * 0.5f)), P.textDim, g);
            }
          }
          if (named && textOk) {
            // T-13: content that starts later than the clip (an offset) shows after the name: ">" and
            // where it starts (seconds of audio, or bars:beats:rows of the pattern).
            std::string off;
            if (!starts) {
              // (a clip named where it goes on: no offset, it is not its start)
            } else if (c->kind == ClipKind::Audio && c->audioStart > 0.0005) {
              char b[32];
              std::snprintf(b, sizeof b, "  >%.3fs", c->audioStart);
              off = b;
            } else if (c->kind != ClipKind::Audio && c->offset > 0) {
              off = "  >" + barsText(c->offset, bar, std::max<Tick>(1, s.ppq), std::max<Tick>(1, s.ppq / 4));
            }
            if (cp) {
              const std::string label = trk::hexNum(trk::patternNumber(s, cp->id));
              dl->AddText(mono, fs, ImVec2(x + 8, ty), cp->id == a.curPattern ? P.hot : P.text, label.c_str());
              const float lx = x + 8 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx, ty), x + colW - lx - 4, P.textDim, (cp->name + off).c_str());
            } else {
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 8, ty), colW - 12, P.textDim, (other + off).c_str());
            }
          }
          // T-13: a muted clip says so in its first bar ("M" in a dark tag at the right).
          if (starts && textOk && c->muted) {
            const float mw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, "M").x;
            const ImVec2 m0(std::round(x + colW - mw - 9), y + 2), m1(x + colW - 3, y + rh - 2);
            dl->AddRectFilled(m0, m1, theme::withAlpha(P.surface950, 0.9f));
            dl->AddText(mono, fs * 0.85f, ImVec2(m0.x + 3, std::round(y + (rh - fs * 0.85f) * 0.5f)), P.textFaint, "M");
          }
          // A clip starting inside the bar: a tick where it starts; more clips in the bar: "+n".
          if (starts && starts->start > b0t) {
            const float sy = std::round(y + rh * static_cast<float>(starts->start - b0t) / static_cast<float>(bar));
            dl->AddRectFilled(ImVec2(x + 4, sy), ImVec2(x + 10, sy + 1), P.text);
          }
          if (stackCount > 1 && textOk) {
            const std::string more = "+" + std::to_string(stackCount - 1);
            const float mw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, more.c_str()).x;
            dl->AddRectFilled(ImVec2(x + colW - mw - 8, y + 1), ImVec2(x + colW - 1, y + rh - 1), theme::withAlpha(P.surface900, 0.9f));
            dl->AddText(mono, fs * 0.85f, ImVec2(x + colW - mw - 4, std::round(y + (rh - fs * 0.85f) * 0.5f)), P.hot, more.c_str());
          }
        } else {
          if (textOk) dl->AddText(mono, fs, ImVec2(x + 8, ty), theme::withAlpha(P.textFaint, 0.55f), "\xC2\xB7\xC2\xB7");
        }
        if (click) {
          trackerview::setSongPosition(a, b0t);  // (kept while a pattern plays)
          gSongClip = c ? c->id : kNoId;
          if (c && c->kind == ClipKind::Pattern) a.selectPattern(c->pattern);
        }
        if (dbl && !c && a.curPattern != kNoId) a.doc.addClip(a.curPattern, t, b0t);
        if (dbl && c && c->kind == ClipKind::Audio) clipeditor::openFromDoubleClick(a, c->id);  // K-249: the Audio page edits it
        if (rclick && c) {
          gSongClip = c->id;
          menuClip = c->id;
          ImGui::OpenPopup("##songclipmenu");
        }
      }
      dl->PopClipRect();
    }
    if (ImGui::BeginPopup("##songclipmenu")) {
      if (s.clip(menuClip)) {
        // K-456: the arrangement's clip menu as it is (its name, render rows and Delete included; a
        // second Delete and render block here doubled them).
        devArrangementClipMenu(a, menuClip);
        if (!a.doc.song().clip(menuClip) && gSongClip == menuClip) gSongClip = kNoId;
      }
      ImGui::EndPopup();
    }
    endBody(a, contentH);
  }
  ui::Hint("Click: go there and pick the clip (its pattern shows; the box below edits it) \xC2\xB7 double-click an empty bar: place the pattern shown \xC2\xB7 right-click: the clip's menu");
  if (boxH > 0) clipBox(a, ImVec2(o.x, b1.y + sbH + kGap), ImVec2(o.x + av.x, b1.y + sbH + kGap + boxH));
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y + sbH + (boxH > 0 ? boxH + kGap : 0.0f)));
  ImGui::Dummy(ImVec2(1, 0));
}

void zoomSong(App& a, int d) {
  if (gSongLanes) {
    // The lane width: wider / narrower by a quarter from the width drawn, or every lane in view.
    const double w = d == 0 ? -1.0 : std::clamp(static_cast<double>(gLaneW) * (d > 0 ? 1.25 : 0.8), static_cast<double>(std::min(kMinLaneW, gLaneFitW)), static_cast<double>(kMaxLaneW));
    a.settings.set("tracker.songLaneWidth", d != 0 && std::fabs(w - static_cast<double>(gLaneFitW)) < 0.5 ? -1.0 : std::round(w));
  } else {
    // The timeline: half again as close / as far, or the whole song; the middle of the view stays.
    const double z = std::clamp(a.settings.getNumber("tracker.timelineZoom"), 1.0, 512.0);
    const double nz = d == 0 ? 1.0 : std::clamp(d > 0 ? z * 1.5 : z / 1.5, 1.0, 512.0);
    if (gTlLen > 0) gTlStart = gTlStart + gTlSpan * 0.5 - gTlLen / nz * 0.5;
    a.settings.set("tracker.timelineZoom", nz);
  }
}

void devSongZoom(App& a, const std::vector<std::string>& args, int frame) {
  const std::string w = args.size() > 2 ? args[2] : "show";
  if (w == "in") zoomSong(a, 1);
  else if (w == "out") zoomSong(a, -1);
  else if (w == "fit") zoomSong(a, 0);
  else if (w == "lanes") gSongLanes = true;
  else if (w == "arranger") gSongLanes = false;
  else if (w == "end") gTlToEnd = true;
  else if (w == "lane" && args.size() > 3) gLaneReveal = std::atoi(args[3].c_str());
  std::printf("tracker songzoom frame %d: view %s lanes %d lanew %.0f first %d last %d scroll %.0f | timeline zoom %.3f start %.0f end %.0f len %.0f\n", frame,
              gSongLanes ? "lanes" : "arranger", gLanesShown, static_cast<double>(gLaneW), gLaneFirst, gLaneLast, static_cast<double>(gLaneScroll),
              a.settings.getNumber("tracker.timelineZoom"), gTlStart, gTlStart + gTlSpan, gTlLen);
  std::fflush(stdout);
}

// ---- Mix: one row per mixer track ----
namespace {
// TRK-1118: a mixer track's effects on the Mix page: the effect slots' names in a sunken field, and
// its menu (a click or right-click on the field): each effect's editor, replace, bypass and remove,
// and Add effect (the first free slot; Studio's effect picker, built-in and installed). An editor
// opens where the Tracker homes it: a built-in's in the lower band, an installed plugin's own window.
std::string effectNames(const MixerTrack& m) {
  std::string out;
  for (const EffectSlot& sl : m.slots) {
    if (sl.plugin.empty()) {
      continue;
    }
    const PluginInfo* fx = findPlugin(sl.plugin.type);
    out += (out.empty() ? "" : ", ") + (fx ? fx->name : sl.plugin.type);
  }
  return out;
}

void openEffect(App& a, ObjectId track, int slot) {
  a.selMixer = track;
  a.fxEditor = {track, slot};
}

// An effect in the track's first free slot, its editor open (false: no free slot).
bool addEffect(App& a, ObjectId track, const std::string& type) {
  const MixerTrack* mt = a.doc.song().mixerTrack(track);
  for (int k = 0; mt && k < kEffectSlots; ++k) {
    if (mt->slots[static_cast<size_t>(k)].plugin.empty()) {
      if (!a.doc.setSlotPlugin(track, k, type)) {
        return false;
      }
      openEffect(a, track, k);
      return true;
    }
  }
  return false;
}

void replaceEffect(App& a, ObjectId track, int slot, const std::string& type) {
  if (a.doc.setSlotPlugin(track, slot, type)) {
    pui::pluginReplaced(PluginTarget{track, slot});
    openEffect(a, track, slot);
  }
}

void removeEffect(App& a, ObjectId track, int slot) {
  if (a.fxEditor.first == track && a.fxEditor.second == slot) {
    a.fxEditor = {kNoId, -1};
  }
  a.doc.setSlotPlugin(track, slot, "");
}

void effectMenu(App& a, ObjectId track) {
  const MixerTrack* m = a.doc.song().mixerTrack(track);
  if (!m) {
    return;
  }
  int used = 0;
  for (int k = 0; k < kEffectSlots; ++k) {
    const EffectSlot& sl = m->slots[static_cast<size_t>(k)];
    if (sl.plugin.empty()) {
      continue;
    }
    ++used;
    const PluginInfo* fx = findPlugin(sl.plugin.type);
    // K-450: the effect's icon after the slot number (room kept in the label, drawn on the menu's
    // own draw list after the item, so it stays in front of the item's highlight).
    const std::string num = std::to_string(k + 1) + "  ";
    const bool own = ui::hasPluginIcon(sl.plugin.type);
    const float isz = std::min(13.0f, std::floor(ImGui::GetTextLineHeight()));
    std::string pad;  // spaces as wide as the icon and a 5 px gap before the name
    while (own && ImGui::CalcTextSize(pad.c_str()).x < isz + 5.0f) pad += ' ';
    const std::string label = num + pad + (fx ? fx->name : sl.plugin.type) +
                              (sl.enabled ? "" : std::string("  (") + i18n::tr("bypassed") + ")") + "##fxslot" + std::to_string(k);
    ImDrawList* menuDl = ImGui::GetWindowDrawList();
    // Where BeginMenu writes its label: the cursor plus the menu's label column.
    const ImVec2 at(ImGui::GetCursorScreenPos().x + static_cast<float>(ImGui::GetCurrentWindow()->DC.MenuColumns.OffsetLabel), ImGui::GetCursorScreenPos().y);
    const float lineH = ImGui::GetTextLineHeight();
    const bool opened = ImGui::BeginMenu(label.c_str());
    if (!own) ui::notePluginIconFallback(sl.plugin.type, ui::PluginIconSurface::Menu);
    else ui::drawPluginIcon(menuDl, sl.plugin.type, ImVec2(std::round(at.x + ImGui::CalcTextSize(num.c_str()).x + isz * 0.5f), std::round(at.y + lineH * 0.5f)), isz,
                            ui::PluginIconSurface::Menu, sl.enabled ? 1.0f : 0.45f);
    if (opened) {
      if (ImGui::MenuItem(i18n::tr("Open editor"))) {
        openEffect(a, track, k);
      }
      if (ImGui::BeginMenu(i18n::tr("Replace with"))) {
        const std::string cur = sl.plugin.type;
        menus::effectItems([&a, track, k](const std::string& t) { a.later([&a, track, k, t] { replaceEffect(a, track, k, t); }); }, cur);
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(i18n::tr("Bypass"), nullptr, !sl.enabled)) {
        const bool on = !sl.enabled;
        a.later([&a, track, k, on] { ops::setSlotEnabled(a.doc, track, k, on); });
      }
      if (ImGui::MenuItem(i18n::tr("Remove"))) {
        a.later([&a, track, k] { removeEffect(a, track, k); });
      }
      ImGui::EndMenu();
    }
  }
  if (used > 0) {
    ImGui::Separator();
  }
  if (used >= kEffectSlots) {
    ImGui::BeginDisabled();
  }
  if (ImGui::BeginMenu(i18n::tr("Add effect"))) {
    menus::effectItems([&a, track](const std::string& t) {
      a.later([&a, track, t] {
        if (!addEffect(a, track, t)) {
          a.notify(i18n::tr("No free effect slot on this track"), true);
        }
      });
    });
    ImGui::EndMenu();
  }
  if (used >= kEffectSlots) {
    ImGui::EndDisabled();
  }
}
}  // namespace

void devEffect(App& a, const std::vector<std::string>& args, int frame) {
  const Song& s = a.doc.song();
  const std::string op = args.size() > 1 ? args[1] : "dump";
  const int ti = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
  const MixerTrack* m = ti >= 0 && ti < static_cast<int>(s.mixer.size()) ? &s.mixer[static_cast<size_t>(ti)] : nullptr;
  const int slot = args.size() > 3 ? std::atoi(args[3].c_str()) : 0;
  const bool slotOk = slot >= 0 && slot < kEffectSlots;
  if (m && op == "add" && args.size() > 3) {
    addEffect(a, m->id, args[3]);
  } else if (m && op == "replace" && args.size() > 4 && slotOk) {
    replaceEffect(a, m->id, slot, args[4]);
  } else if (m && op == "bypass" && slotOk) {
    ops::setSlotEnabled(a.doc, m->id, slot, !m->slots[static_cast<size_t>(slot)].enabled);
  } else if (m && op == "remove" && slotOk) {
    removeEffect(a, m->id, slot);
  } else if (m && op == "open" && slotOk) {
    openEffect(a, m->id, slot);
  }
  const Song& now = a.doc.song();
  const MixerTrack* t = ti >= 0 && ti < static_cast<int>(now.mixer.size()) ? &now.mixer[static_cast<size_t>(ti)] : nullptr;
  std::string slots;
  for (int k = 0; t && k < kEffectSlots; ++k) {
    const EffectSlot& sl = t->slots[static_cast<size_t>(k)];
    if (!sl.plugin.empty()) {
      slots += " " + std::to_string(k) + ":" + sl.plugin.type + (sl.enabled ? "" : "(off)");
    }
  }
  const int fxTrack = a.fxEditor.first != kNoId ? now.mixerIndex(a.fxEditor.first) : -1;
  std::printf("tracker fx frame %d (%s): track %d slots%s | editor %d %d | undo %zu \"%s\"\n", frame, op.c_str(), ti, slots.empty() ? " -" : slots.c_str(), fxTrack,
              a.fxEditor.second, a.doc.history().position(), a.doc.history().canUndo() ? a.doc.history().undoLabel().c_str() : "");
  std::fflush(stdout);
}

void mixer(App& a) {
  const trackerview::PerfScope perfScope(trackerview::PerfPage);  // cp16: "tracker perf"
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  char info[64];
  std::snprintf(info, sizeof info, "%s %s", i18n::tr("Tracks"), trk::hex2(static_cast<int>(s.mixer.size())).c_str());
  // GitHub #58: hidden tracks have no row, a folded group one row, a narrow track a short row (name,
  // mute and meter); the title bar's button lists what is hidden.
  std::vector<size_t> all(s.mixer.size());
  for (size_t i = 0; i < all.size(); ++i) all[i] = i;
  const std::vector<mixhide::Entry> entries = mixhide::arrange(s, all);
  const std::string hiddenText = mixhideui::hiddenLabel(s);
  const float hiddenW = hiddenText.empty() ? 0.0f : std::ceil(a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, hiddenText.c_str()).x) + 24;
  const float th = titleBar(a, o, av.x, i18n::tr("Mix"), info, hiddenW > 0 ? hiddenW + 4 : 0.0f);
  if (hiddenW > 0) {
    const float ew = std::ceil(a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, i18n::tr("Exit")).x) + 24;
    const float x1 = o.x + av.x - 2 - ew - 4;
    if (bevelButton(a, "##mixhidden", hiddenText.c_str(), ImVec2(x1 - hiddenW, o.y + 2), ImVec2(x1, o.y + th - 2), false,
                    "Tracks hidden from the mixer (they still play): click to show them"))
      ImGui::OpenPopup("##trkhidden");
    devNoteRect("trkmixhidden");
    if (ImGui::BeginPopup("##trkhidden")) {
      mixhideui::hiddenMenu(a);
      ImGui::EndPopup();
    }
  }
  const float rh0 = rowHeight();
  const ImVec2 b0(o.x, o.y + th + kGap), b1(o.x + av.x, o.y + std::max(av.y, th + rh0 * 3));
  // Few tracks: the rows grow (up to twice their height) so the page has no empty lower part.
  const float rh = std::max(rh0, std::min(rh0 * 2, std::floor((b1.y - b0.y - 4) / static_cast<float>(std::max<size_t>(1, entries.size())))));
  const float contentH = static_cast<float>(entries.size()) * rh + 4;
  if (beginBody("##mix", b0, b1, contentH)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float nameW = std::min(200.0f, std::round(W * 0.22f)), lampW = rh0 - 1;  // cp23: as the Steps page
    const float volW = std::round(W * 0.24f), panW = std::round(W * 0.14f);
    // cp24 (usability): the channels playing into each track, in a column before its meter (the page
    // named tracks only, "Insert 3", and which instrument went where could be found only in each
    // instrument's Channel frame). The meter keeps what is left (it ran 560 px wide at 1440x900).
    const float fromW = std::round(W * 0.16f);
    const float fxW = std::round(W * 0.17f);  // TRK-1118: the track's effects
    std::vector<std::string> from(s.mixer.size());
    for (const Channel& ch : s.channels) {
      for (size_t i = 0; i < s.mixer.size(); ++i) {
        if (s.mixer[i].id == ch.mixerTrack) {
          from[i] += (from[i].empty() ? "" : ", ") + ch.name;
          break;
        }
      }
    }
    for (size_t row = 0; row < entries.size(); ++row) {
      const mixhide::Entry& e = entries[row];
      const size_t i = e.index;
      const MixerTrack& m = s.mixer[i];
      const bool group = e.show == mixhide::Show::Group;
      const float y = std::round(c0.y + 2 + static_cast<float>(row) * rh);
      ImGui::PushID(static_cast<int>(i));
      std::string name = m.name.empty() ? (i == 0 ? std::string(i18n::tr("Master")) : std::string(i18n::tr("Insert")) + " " + std::to_string(i)) : m.name;
      if (group) name = "> " + e.group + "  (" + std::to_string(e.members) + " " + i18n::tr("tracks") + ")";  // a folded group
      const bool muted = group ? mixhide::groupMuted(s, e.group) : m.muted;
      float x = c0.x + 2;
      nameField(a, ImVec2(x, y), ImVec2(x + nameW, y + rh - 1), static_cast<int>(i), m.color, name, a.selMixer == m.id, muted);
      ImGui::SetCursorScreenPos(ImVec2(x, y));
      if (ImGui::InvisibleButton("##name", ImVec2(nameW, rh - 1)) && !group) a.selMixer = m.id;
      if (group && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) mixhide::setGroupFolded(a.doc, e.group, false);
      if (ImGui::IsItemHovered()) {
        if (group) ui::Hint("A collapsed group: double-click to expand it \xC2\xB7 right-click: its menu");
        else if (i > 0) ui::Hint("Click: select \xC2\xB7 right-click: hide the track, a short row, its group");
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && (group || i > 0)) {
        if (!group) a.selMixer = m.id;
        ImGui::OpenPopup("##trkmixrow");
      }
      if (ImGui::BeginPopup("##trkmixrow")) {
        if (group) {
          mixhideui::groupMenu(a, e.group);
        } else {
          mixhideui::trackItems(a, m.id, {m.id});
          if (!a.doc.song().mixer[i].group.empty()) mixhideui::groupItem(a, a.doc.song().mixer[i].group);
        }
        ImGui::EndPopup();
      }
      x += nameW + kGap;
      if (lamp(a, "##mute", ImVec2(x, std::round(y + (rh - rh0) * 0.5f)), lampW, !muted, P.led,
               group ? i18n::tr("Lit: the group plays \xC2\xB7 click: mute every track in it") : i18n::tr("Lit: the track plays \xC2\xB7 click: mute"))) {
        if (group) mixhide::setGroupMute(a.doc, e.group, !muted);
        else a.doc.setParam(mixerParam(m.id, MixerParam::Mute), m.muted ? 0 : 1, false);
      }
      x += lampW + trackerview::space::kGroup;
      if (e.show != mixhide::Show::Strip) {  // a short row: the meter right after the lamp (a group: its loudest track)
        float lvl = 0;
        for (const MixerTrack& t2 : s.mixer)
          if (group ? t2.group == e.group : t2.id == m.id) lvl = std::max(lvl, mixmeters::get(a, t2.id).peak());
        lvl = std::clamp(lvl, 0.0f, 1.0f);
        const float mx1 = c0.x + W - 2;
        if (mx1 - x > 20) {
          bevelBox(dl, ImVec2(x, y), ImVec2(mx1, y + rh - 1), true);
          const int segs = std::max(4, static_cast<int>((mx1 - x - 4) / 5));
          const int lit = static_cast<int>(std::round(lvl * static_cast<float>(segs)));
          for (int k = 0; k < segs; ++k) {
            const float sx = std::round(x + 2 + static_cast<float>(k) * 5);
            const ImU32 col = k >= segs * 9 / 10 ? P.danger : k >= segs * 7 / 10 ? P.hot : P.led;
            dl->AddRectFilled(ImVec2(sx, y + 3), ImVec2(sx + 3, y + rh - 4), k < lit ? col : theme::withAlpha(col, 0.12f));
          }
        }
        ImGui::PopID();
        continue;
      }

      double nv = 0;
      char txt[16];
      std::snprintf(txt, sizeof txt, "%3d", static_cast<int>(std::lround(m.volume * 100)));
      dl->AddText(mono, fs, ImVec2(x, std::round(y + (rh - fs) * 0.5f)), P.textDim, txt);
      x += std::ceil(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000").x) + trackerview::space::kGroup;  // cp23: 6 px to the bar, as between the other parts
      if (valueBar("##vol", ImVec2(x, y), ImVec2(x + volW, y + rh - 1), m.volume, 0.0, 1.25, false, &nv, "Volume (drag; double-click: the default)"))
        a.doc.setParam(mixerParam(m.id, MixerParam::Volume), nv, true);
      x += volW + trackerview::space::kGroup;
      if (valueBar("##pan", ImVec2(x, y), ImVec2(x + panW, y + rh - 1), m.pan, -1.0, 1.0, true, &nv, "Pan (drag; double-click: centre)"))
        a.doc.setParam(mixerParam(m.id, MixerParam::Pan), nv, true);
      x += panW + trackerview::space::kGroup;  // cp23: the parts 6 px apart (were 6, a space, 8 and 8)
      {
        const ImVec2 f0(x, y), f1(x + fromW, y + rh - 1);
        bevelBox(dl, f0, f1, true);
        const float ty = std::round(y + (rh - fs) * 0.5f);
        if (from[i].empty()) {
          dl->AddText(mono, fs, ImVec2(f0.x + 6, ty), P.textFaint, "--");
        } else {
          theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(f0.x + 6, ty), f1.x - f0.x - 10, P.textDim, from[i].c_str());
        }
        ImGui::SetCursorScreenPos(f0);
        ImGui::InvisibleButton("##from", ImVec2(fromW, rh - 1));
        ui::Hint(from[i].empty() ? "No channel plays into this track" : "The channels that play into this track (each one's Output, in its Channel frame)");
        x += fromW + trackerview::space::kGroup;
      }
      {  // TRK-1118: the track's effects; a click opens their menu (add, open, replace, bypass, remove)
        const ImVec2 f0(x, y), f1(x + fxW, y + rh - 1);
        ImGui::SetCursorScreenPos(f0);
        const bool click = ImGui::InvisibleButton("##fx", ImVec2(fxW, rh - 1));
        const bool hov = ImGui::IsItemHovered();
        if (click || ImGui::IsItemClicked(ImGuiMouseButton_Right) || devMenuTake(("trackerfx" + std::to_string(i)).c_str())) {  // dev: "menu open trackerfx<n>"
          a.selMixer = m.id;
          ImGui::OpenPopup("##fxmenu");
        }
        bevelBox(dl, f0, f1, true);
        if (hov) {
          dl->AddRectFilled(ImVec2(f0.x + 1, f0.y + 1), ImVec2(f1.x - 1, f1.y - 1), theme::withAlpha(P.surface600, 0.5f));
        }
        const std::string names = effectNames(m);
        const float ty = std::round(y + (rh - fs) * 0.5f);
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(f0.x + 6, ty), f1.x - f0.x - 10, names.empty() ? (hov ? P.textDim : P.textFaint) : P.textDim,
                            names.empty() ? i18n::tr("+ Add effect") : names.c_str());
        ui::Hint("The track's effects: click to add one, or to open, replace, bypass or remove one");
        if (ImGui::BeginPopup("##fxmenu")) {
          effectMenu(a, m.id);
          ImGui::EndPopup();
        }
        x += fxW + trackerview::space::kGroup;
      }
      // A segmented level meter to the row's end.
      const float lvl = std::clamp(mixmeters::get(a, m.id).peak(), 0.0f, 1.0f);
      const float mx1 = c0.x + W - 2;  // cp23: 2 px from the body's edge, as the names at the left (was 4)
      if (mx1 - x > 20) {
        bevelBox(dl, ImVec2(x, y), ImVec2(mx1, y + rh - 1), true);
        const int segs = std::max(4, static_cast<int>((mx1 - x - 4) / 5));
        const int lit = static_cast<int>(std::round(lvl * static_cast<float>(segs)));
        for (int k = 0; k < segs; ++k) {
          const float sx = std::round(x + 2 + static_cast<float>(k) * 5);
          const ImU32 col = k >= segs * 9 / 10 ? P.danger : k >= segs * 7 / 10 ? P.hot : P.led;
          dl->AddRectFilled(ImVec2(sx, y + 3), ImVec2(sx + 3, y + rh - 4), k < lit ? col : theme::withAlpha(col, 0.12f));
        }
      }
      ImGui::PopID();
    }
    endBody(a, contentH);
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

void pads(App& a) {
  const trackerview::PerfScope perfScope(trackerview::PerfPage);  // cp16: "tracker perf"
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const bool live = a.engine.mode() == PlayMode::Live;
  // Title bar with the page's two controls at its right (Live mode is this page's own switch).
  const float th = titleBar(a, o, av.x, "Pads", "");
  {
    const float bw = std::ceil(std::max(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Live mode")).x,
                                        a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Stop all")).x)) + 24;
    const float x1 = o.x + av.x - 3, y0 = o.y + 2, y1 = o.y + th - 2;
    if (bevelButton(a, "##stopall", "Stop all", ImVec2(x1 - bw, y0), ImVec2(x1, y1), false, "Stops every track at its launch grid")) a.engine.stopAllLive();
    if (bevelButton(a, "##livemode", "Live mode", ImVec2(x1 - bw * 2 - 2, y0), ImVec2(x1 - bw - 2, y1), live,
                    "Live mode: the transport runs freely and plays only the clips launched here"))
      a.engine.setMode(live ? PlayMode::Song : PlayMode::Live);
  }
  const ImVec2 b0(o.x, o.y + th + kGap), b1(o.x + av.x, o.y + std::max(av.y, th + 60));
  if (s.live.areaEnd <= 0) {
    // cp16: no live area: the song's patterns as pads (before: an empty card). A click plays the
    // pattern, looped (another click on a playing pad stops); a double-click edits it.
    bevelBox(dl, b0, b1, true);
    const float rh0 = rowHeight();
    const float padW = std::round(std::max(fs * 9, 120.0f)), padH = std::round(rh0 * 2.6f);
    const float inner = b1.x - b0.x - 8;
    const int perRow = std::max(1, static_cast<int>((inner + kGap) / (padW + kGap)));
    const float pw = std::floor((inner - kGap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow));
    const bool patPlaying = a.engine.playing() && a.engine.mode() == PlayMode::Pattern;
    const float noteH = rh0 + 6;
    const int rows = (static_cast<int>(s.patterns.size()) + perRow - 1) / perRow;
    const float contentH = static_cast<float>(rows) * (padH + kGap) + 4;
    if (beginBody("##patpads", ImVec2(b0.x, b0.y), ImVec2(b1.x, b1.y - noteH), contentH)) {
      ImDrawList* cl = ImGui::GetWindowDrawList();
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      for (size_t i = 0; i < s.patterns.size(); ++i) {
        const Pattern& p = s.patterns[i];
        const int r = static_cast<int>(i) / perRow, k = static_cast<int>(i) % perRow;
        const ImVec2 q(std::round(c0.x + 3 + static_cast<float>(k) * (pw + kGap)), std::round(c0.y + 3 + static_cast<float>(r) * (padH + kGap)));
        const ImVec2 q1(q.x + pw, q.y + padH);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(static_cast<int>(p.id));
        const bool click = ImGui::InvisibleButton("##patpad", ImVec2(pw, padH));
        const bool hov = ImGui::IsItemHovered(), dbl = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();
        const bool on = patPlaying && a.engine.pattern() == p.id;
        bevelBox(cl, q, q1, on || ImGui::IsItemActive());
        if (on) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.hot, 0.85f));
        else if (p.id == a.curPattern) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.surface600, 0.7f));
        else if (hov) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.surface600, 0.4f));
        cl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q1.x - 2, q.y + 5), theme::rgb(p.color));
        const ImU32 tc = on ? theme::contrastText(P.hot) : P.text;
        const std::string num = trk::hex2(static_cast<int>(i) + 1);
        cl->AddText(mono, fs, ImVec2(q.x + 6, q.y + 8), on ? tc : P.textDim, num.c_str());
        char bars[24];
        std::snprintf(bars, sizeof bars, "%d %s", p.bars, i18n::tr(p.bars == 1 ? "bar" : "bars"));
        const float bw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, bars).x;
        cl->AddText(a.fontRegular, fs, ImVec2(std::round(q1.x - 6 - bw), q.y + 8), on ? tc : P.textFaint, bars);
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(q.x + 6, std::round(q.y + 8 + fs + 3)), pw - 12, tc, p.name.c_str());
        if (hov) ui::Hint("Click: plays the pattern, looped (again: stops) \xC2\xB7 right-click: edit it");
        if (dbl) {
          a.selectPattern(p.id);
          trackerview::showPage(a, "Pattern editor");
        } else if (click) {
          if (on) {
            menuparity::stopPressed(a);
          } else {
            a.selectPattern(p.id);
            a.engine.setMode(PlayMode::Pattern);
            a.engine.setPattern(p.id);
            if (!a.engine.playing()) menuparity::playPressed(a);
          }
        }
      }
      endBody(a, contentH);
    }
    dl->AddText(a.fontRegular, fs, ImVec2(b0.x + 8, std::round(b1.y - noteH + (noteH - fs) * 0.5f)), P.textFaint,
                i18n::tr("The song's patterns. Clips placed before the live area's end (Song menu, Live area) become launch pads here."), nullptr,
                b1.x - b0.x - 16);
    ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
    ImGui::Dummy(ImVec2(1, 0));
    return;
  }
  const float rh = std::round(rowHeight() * 2 + 2), nameW = std::round(std::max(150.0f, fs * 12)), padW = std::round(fs * 8);
  std::vector<int> rows;
  for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr)
    if (!liveClips(s, tr).empty()) rows.push_back(tr);
  const float contentH = static_cast<float>(rows.size()) * (rh + kGap) + kGap;
  if (beginBody("##padsbody", b0, b1, contentH)) {
    static ObjectId held = kNoId;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f);
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImDrawList* cl = ImGui::GetWindowDrawList();
    for (size_t r = 0; r < rows.size(); ++r) {
      const int tr = rows[r];
      const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
      const float y = c0.y + kGap + static_cast<float>(r) * (rh + kGap), x = c0.x + kGap;
      ImGui::PushID(tr);
      nameField(a, ImVec2(x, y), ImVec2(x + nameW, y + rowHeight()), tr + 1, t.color, playlistTrackName(s, static_cast<size_t>(tr)), false, false);
      const std::string mode = std::string(padModeName(t.pad)) + " \xC2\xB7 " + followModeName(t.follow);
      theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(x + 9, std::round(y + rowHeight() + 2 + (rh - rowHeight() - 2 - fs) * 0.5f)), nameW - 60, P.textFaint,
                          mode.c_str());
      if (bevelButton(a, "##stop", "Stop", ImVec2(x + nameW - 48, y + rowHeight() + 2), ImVec2(x + nameW, y + rh), false,
                      "Stops this track (right-click: its live settings)"))
        a.engine.stopLiveTrack(tr);
      if (ImGui::BeginPopupContextItem("##trlive")) {
        liveTrackMenu(a, tr);
        ImGui::EndPopup();
      }
      const std::vector<int> clips = liveClips(s, tr);
      const ObjectId act = a.engine.liveActive(tr), que = a.engine.liveQueued(tr);
      for (size_t k = 0; k < clips.size(); ++k) {
        const Clip& c = s.playlist[static_cast<size_t>(clips[k])];
        ImGui::PushID(static_cast<int>(c.id));
        const ImVec2 q(x + nameW + kGap + static_cast<float>(k) * (padW + kGap), y), q1(q.x + padW, y + rh);
        ImGui::SetCursorScreenPos(q);
        ImGui::InvisibleButton("##pad", ImVec2(padW, rh));
        if (ImGui::IsItemActivated()) {
          if (!live) a.engine.setMode(PlayMode::Live);
          a.engine.launchClip(c.id);
          held = c.id;
        }
        const bool hov = ImGui::IsItemHovered(), on = act == c.id, waiting = que == c.id;
        Rgb rgb = 0x50565E;
        std::string nm;
        int num = 0;
        if (c.kind == ClipKind::Pattern) {
          if (const Pattern* pt = s.pattern(c.pattern)) rgb = pt->color, nm = pt->name, num = trk::patternNumber(s, pt->id);
        } else if (c.kind == ClipKind::Audio) {
          if (const Channel* ch = s.channel(c.source)) rgb = ch->color, nm = ch->name;
        } else if (const AutomationClip* au = s.automation(c.source)) {
          rgb = au->color, nm = au->name;
        }
        bevelBox(cl, q, q1, on || ImGui::IsItemActive());
        if (on) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.hot, 0.85f));
        else if (hov) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.surface600, 0.5f));
        cl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q1.x - 2, q.y + 5), theme::rgb(rgb));
        if (waiting) cl->AddRect(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.hot, 0.35f + 0.65f * pulse), 0, 0, 2.0f);
        const ImU32 tc = on ? theme::contrastText(P.hot) : P.text;
        const std::string n = num ? trk::hex2(num) : std::string("--");
        cl->AddText(mono, fs, ImVec2(q.x + 5, q.y + 7), on ? tc : P.textDim, n.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(q.x + 5, q.y + 7 + fs + 1), padW - 10, tc, nm.c_str());
        ui::Hint("Press to launch (on the track's launch grid)");
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    if (held != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      a.engine.releaseClip(held);
      held = kNoId;
    }
    endBody(a, contentH);
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

}  // namespace gl::trackerpages
