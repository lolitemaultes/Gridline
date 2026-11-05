// Gridline — piano roll: notes of (selected pattern, selected channel).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "../core/MenuOpsExtra.h"
#include "../core/I18n.h"
#include "CursorAnchor.h"
#include "Keys.h"
#include "KeyLight.h"
#include "J31Ui.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "SmoothScrollUi.h"
#include "AutomationEditor.h"
#include "EditTools.h"
#include "ScriptUi.h"
#include "../core/Grouping.h"
#include "../core/MenuParity.h"
#include "../core/NoteBend.h"
#include "../core/NoteLaneEdit.h"
#include "../core/NoteTools.h"
#include "../core/SmoothSnap.h"
#include "../core/StepNotes.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"

namespace gl {

using theme::pal;

namespace {

// K-24: the key rows' height range and one Alt+wheel notch's factor.
constexpr float kRowHMin = 7.0f, kRowHMax = 28.0f;
constexpr double kRowZoomStep = 1.1;

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int pitch) {
  int k = pitch % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
std::string noteName(int pitch) { return std::string(kNoteNames[pitch % 12]) + std::to_string(pitch / 12 - 1); }

struct Gesture {
  enum Kind { None, Move, Resize, Select, Paint, Erase, Velocity, Slice, Scrub, Mute, Play, Zoom, Detune, Keys, Bend, BendCurve } kind = None;
  int bendPt = -1;  // K-25: the bend point dragged (index in the note's line at gesture start), -1 = a new one
                    // (BendCurve, K-28: the point ending the segment whose curve handle is dragged)
  double bendT = 0, bendV = 0;  // K-25: where it is now (ticks from the note start, semitones)
  int muteTo = -1;          // mute tool: state applied to every touched note
  std::vector<int> playing; // play tool: pitches sounding
  Tick lastScrub = -1;  // last position sent while scrubbing
  ImVec2 start;
  Tick startTick = 0;
  int startPitch = 0;
  std::vector<Note> base;   // notes at gesture start (sorted)
  std::vector<bool> sel;    // selection at gesture start
  int anchor = -1;          // note index the gesture started on
  uint64_t key = 0;
  bool moved = false;
  int previewPitch = -1;
  // Q-3: Shift+drag duplicates (copies made when the pointer first moves);
  // the drag's edits form one undo step (grouped) that Esc / right-click cancels.
  bool copyPending = false;
  bool grouped = false;
  bool drawResize = false;  // K-20: a Shift+click drew this note; its length follows the drag once it starts
  bool drawn = false;       // the click just drew this note: it follows the pointer at once
  std::set<size_t> selBefore;
  // A delete sweep takes only the topmost note at each point: where it already deleted one, the note
  // that was under it stays (a later frame of the same press would otherwise take that one too).
  struct ErasedSpan {
    int pitch;
    Tick start, end;
  };
  std::vector<ErasedSpan> erased;
};
Gesture g;
uint64_t gestureCounter = 1;

// Q-4: event lane drag state (Gesture::Velocity). Left drag paints along the
// pointer path (segment sweep from the previous frame's position); right drag
// is FL's line tool: a straight line from the press point, applied on release.
struct LaneDrag {
  bool line = false;     // right drag: line tool
  bool reset = false;    // paint with each note's default value (FL: Alt + drag)
  bool select = false;   // select notes by time (FL: Ctrl + drag in the lane)
  bool hasPrev = false;  // paint: previous pointer position known
  double prevTick = 0;   // paint: previous pointer position (ticks, fractional)
  float prevFrac = 0;    // paint: previous value (0..1)
  double tickA = 0;      // line: start point
  float fracA = 0;
};
LaneDrag ld;
PianoLaneGeometry laneGeo;  // dev check: last drawn lane geometry

// J-37: smooth snapping of a note drag. Every selected note moves by the same
// snapped delta (start ticks, pitch rows, length ticks); the drawing eases
// that delta, the document keeps the exact one.
smooth::Tracker<3> noteAnim;
struct NoteAnimTarget {
  ObjectId pat = kNoId, ch = kNoId;
  smooth::Tracker<3>::Vec d{};
} noteAnimT;

// ---- note properties: the property lane, new-note colour and slide mode
enum class Lane { Velocity, Pan, Release, Fine, ModX, ModY, Count };
const char* const kLaneNames[] = {"Velocity", "Pan", "Release velocity", "Fine pitch", "Mod X", "Mod Y"};
const char* const kLaneShort[] = {"VEL", "PAN", "REL", "FINE", "MOD X", "MOD Y"};
Lane lane = Lane::Velocity;
int newColor = 0;       // colour group of new notes
bool newSlide = false;  // new notes are slide notes

bool laneBipolar(Lane l) { return l == Lane::Pan || l == Lane::Fine; }
// Property value as 0..1 (0.5 = centre for bipolar lanes).
float laneValue(const Note& n, Lane l) {
  switch (l) {
    case Lane::Pan: return 0.5f + 0.5f * static_cast<float>(n.pan) / kNotePanMax;
    case Lane::Release: return static_cast<float>(n.release) / 127.0f;
    case Lane::Fine: return 0.5f + 0.5f * static_cast<float>(n.fine) / kNoteFineMax;
    case Lane::ModX: return static_cast<float>(n.modX) / kNoteModMax;
    case Lane::ModY: return static_cast<float>(n.modY) / kNoteModMax;
    default: return static_cast<float>(n.velocity) / 127.0f;
  }
}
void setLaneValue(Note& n, Lane l, float f) {
  f = std::clamp(f, 0.0f, 1.0f);
  switch (l) {
    case Lane::Pan: n.pan = static_cast<int>(std::lround((f * 2 - 1) * kNotePanMax)); break;
    case Lane::Release: n.release = static_cast<int>(std::lround(f * 127)); break;
    case Lane::Fine: n.fine = static_cast<int>(std::lround((f * 2 - 1) * kNoteFineMax / 10.0)) * 10; break;
    case Lane::ModX: n.modX = static_cast<int>(std::lround(f * kNoteModMax)); break;
    case Lane::ModY: n.modY = static_cast<int>(std::lround(f * kNoteModMax)); break;
    default: n.velocity = std::clamp(static_cast<int>(std::lround(f * 127)), 1, 127); break;
  }
}
// Q-4: lane edits go through setLaneValue (its rounding, e.g. fine pitch in 10 cent steps).
void laneSetFn(Note& n, tools::NoteProp p, double v) { setLaneValue(n, static_cast<Lane>(p), static_cast<float>(v)); }
// Line tool: half a pixel either side, so a stem drawn on the pixel where the
// line starts or ends is inside it.
constexpr float kLaneLineSlackPx = 0.5f;
std::string laneText(const Note& n, Lane l) {
  char b[48];
  switch (l) {
    case Lane::Pan:
      if (n.pan == 0) return "Pan centre";
      std::snprintf(b, sizeof b, "Pan %d%% %s", std::abs(n.pan) * 100 / kNotePanMax, n.pan < 0 ? "left" : "right");
      break;
    case Lane::Release: std::snprintf(b, sizeof b, "Release velocity %d", n.release); break;
    case Lane::Fine: std::snprintf(b, sizeof b, "Fine pitch %+d cents", n.fine); break;
    case Lane::ModX: std::snprintf(b, sizeof b, "Mod X %d%%", n.modX * 100 / kNoteModMax); break;
    case Lane::ModY: std::snprintf(b, sizeof b, "Mod Y %d%%", n.modY * 100 / kNoteModMax); break;
    default: std::snprintf(b, sizeof b, "Velocity %d", n.velocity); break;
  }
  return b;
}
// Colour groups: group 0 draws in the channel colour, 1..15 in muted hues.
ImU32 groupColor(int group, ImU32 chanCol) {
  if (group <= 0) return chanCol;
  float r, gg, b;
  ImGui::ColorConvertHSVtoRGB(static_cast<float>(group - 1) / 15.0f, 0.42f, 0.78f, r, gg, b);
  return ImGui::GetColorU32(ImVec4(r, gg, b, 1.0f));
}

// Sorts notes exactly like Document::setNotes and keeps the selection aligned.
void sortWithSelection(std::vector<Note>& notes, std::vector<bool>& sel) {
  std::vector<size_t> idx(notes.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].pitch < notes[b].pitch;
  });
  std::vector<Note> n2;
  std::vector<bool> s2;
  for (size_t i : idx) {
    n2.push_back(notes[i]);
    s2.push_back(i < sel.size() && sel[i]);
  }
  notes.swap(n2);
  sel.swap(s2);
}

void commit(App& a, ObjectId pat, ObjectId ch, std::vector<Note> notes, std::vector<bool> sel, const char* label,
            uint64_t key) {
  for (auto& n : notes) {
    n.pitch = std::clamp(n.pitch, 0, 127);
    n.velocity = std::clamp(n.velocity, 1, 127);
    n.start = std::max<Tick>(n.start, 0);
    n.length = std::max<Tick>(n.length, 1);
  }
  sel.resize(notes.size(), false);
  sortWithSelection(notes, sel);
  stepnotes::write(a.doc, pat, ch, notes, label, key);  // K-22: the first edit moves rack steps in
  a.piano.selected.clear();
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) a.piano.selected.insert(i);
}

// K-22: the channel's notes as the piano roll shows them (its rack steps as step notes too).
std::vector<Note> rollNotes(const App& a, ObjectId pat, ObjectId ch) {
  const Pattern* p = a.doc.song().pattern(pat);
  return p ? stepnotes::view(a.doc.song(), *p, ch) : std::vector<Note>{};
}

std::vector<bool> selVector(const App& a, size_t n) {
  std::vector<bool> v(n, false);
  for (size_t i : a.piano.selected)
    if (i < n) v[i] = true;
  return v;
}

void previewOn(App& a, ObjectId ch, int pitch, int vel) {
  if (g.previewPitch == pitch) return;
  if (g.previewPitch >= 0) a.engine.noteOff(ch, g.previewPitch);
  a.engine.noteOn(ch, pitch, vel / 127.0f);
  g.previewPitch = pitch;
}
void previewOff(App& a, ObjectId ch) {
  if (g.previewPitch >= 0) a.engine.noteOff(ch, g.previewPitch);
  g.previewPitch = -1;
}

// ---- K-25: pitch bend lines ----
// A note's line in screen space: x0 = the note's start, cy = its row's centre, one row per
// semitone. Straight and step segments need only their ends; curved ones (K-28: smooth, eases,
// a bent straight) are sampled every few pixels through bend::value, the function playback uses.
// Drawn to the note's end (points past it wait there, out of play). K-29: `seg[i]` names the
// bend point whose glide the piece ending at out[i] belongs to (-1: the hold after the last point).
void bendPolyline(const Note& n, float x0, float cy, float pxPerTick, float rowH, std::vector<ImVec2>& out, std::vector<int>& seg) {
  out.clear();
  seg.clear();
  const double len = static_cast<double>(n.length);
  int cur = -1;
  auto at = [&](double t, double v) {
    out.push_back(ImVec2(x0 + static_cast<float>(t) * pxPerTick, cy - static_cast<float>(v) * rowH));
    seg.push_back(cur);
  };
  at(0.0, bend::value(n.bend, 0.0));
  double ta = 0.0;
  for (size_t k = 0; k <= n.bend.size(); ++k) {
    cur = k < n.bend.size() ? static_cast<int>(k) : -1;
    const bool last = k == n.bend.size() || n.bend[k].tick >= len;
    const double tb = last ? len : static_cast<double>(n.bend[k].tick);
    const uint8_t curve = k < n.bend.size() ? n.bend[k].curve : static_cast<uint8_t>(bend::Straight);
    if (tb > ta) {
      if (k < n.bend.size() && bend::curved(n.bend[k])) {  // K-28: every curved shape, sampled (the same value() the engine plays)
        const int steps = std::clamp(static_cast<int>((tb - ta) * pxPerTick / 3.0), 2, 96);
        for (int s = 1; s < steps; ++s) {
          const double t = ta + (tb - ta) * s / steps;
          at(t, bend::value(n.bend, t));
        }
      } else if (curve == bend::Step && k < n.bend.size()) {
        at(tb, bend::value(n.bend, tb - 1e-4));
      }
    }
    if (last) {
      at(len, bend::value(n.bend, std::max(0.0, len - 1e-4)));
      break;
    }
    at(tb, n.bend[k].semis);
    ta = tb;
  }
}
float segDist(ImVec2 p, ImVec2 a, ImVec2 b) {
  const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
  const float u = l2 > 1e-6f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
  const float ex = a.x + u * dx - p.x, ey = a.y + u * dy - p.y;
  return std::sqrt(ex * ex + ey * ey);
}
// "+2.00 st  B2": the bend in semitones and the note it sounds (cents when between keys).
std::string bendText(int pitch, double semis) {
  const double target = pitch + semis;
  const int key = std::clamp(static_cast<int>(std::lround(target)), 0, 127);
  const int cents = static_cast<int>(std::lround((target - key) * 100.0));
  char b[64];
  if (cents != 0)
    std::snprintf(b, sizeof b, "%+.2f st  %s %+d ct", semis, noteName(key).c_str(), cents);
  else
    std::snprintf(b, sizeof b, "%+.2f st  %s", semis, noteName(key).c_str());
  return b;
}
// What the pointer is over with the bend tool (K-25): a point, a curve handle (K-28), a line, or
// a note body. `point` is the point (kind 4) or the point ending the handle's segment (kind 3),
// or (K-29) the point ending the line's segment under the pointer (kind 2; -1 = the hold after
// the last point).
struct BendHit {
  enum Kind { None = 0, Body = 1, Line = 2, Handle = 3, Point = 4 };
  int note = -1, point = -1;
  int kind = None;
  float dist = 1e9f;
  bool selected = false;
};
BendHit bendHover;
// K-29: the last press of the bend tool: what it hit, the notes before it and the history state
// its drag left (0: it changed nothing). A double-click acts on what its first click hit, on the
// line as it was before that click: a first click that moved a point by a few pixels (or added
// one) is undone first, so a double-click is one undo step and leaves no stray point.
struct BendClick {
  bool valid = false;
  BendHit hit;
  std::vector<Note> before;
  ObjectId pat = kNoId, ch = kNoId;
  uint64_t serial = 0;
  uint64_t press = 0;  // the history state at the press (a drag that changed nothing leaves it)
};
BendClick bendClick;
int bendFrontNote = -1;  // K-28: the note whose line was hovered last frame (drawn on top)
int bendMenuNote = -1, bendMenuPoint = -1;  // K-28: the point whose curve menu is open (-1 = none)
uint64_t bendMenuKey = 0;                   // K-28: undo key of the menu's curve amount drag

// K-28: the curve handle of the segment ending at point k: half way in time, on the curve. None
// for steps, holds and segments narrower than 16 px (too small to grab between their points).
bool bendHandle(const Note& n, size_t k, float x0, float cy, float pxPerTick, float rowH, ImVec2& out) {
  if (k >= n.bend.size() || n.bend[k].curve == bend::Step || n.bend[k].tick > static_cast<float>(n.length)) return false;
  const double ta = k > 0 ? n.bend[k - 1].tick : 0.0, va = k > 0 ? n.bend[k - 1].semis : 0.0, tb = n.bend[k].tick;
  if ((tb - ta) * pxPerTick < 16.0 || std::fabs(n.bend[k].semis - va) < 1e-4) return false;
  const double tm = 0.5 * (ta + tb);
  out = ImVec2(x0 + static_cast<float>(tm) * pxPerTick, cy - static_cast<float>(bend::value(n.bend, tm)) * rowH);
  return true;
}
// Does the segment a-b pass through the rectangle r0-r1? (Liang-Barsky clipping)
bool segHitsRect(ImVec2 a, ImVec2 b, ImVec2 r0, ImVec2 r1) {
  double u0 = 0, u1 = 1;
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double p[4] = {-dx, dx, -dy, dy}, q[4] = {a.x - r0.x, r1.x - a.x, a.y - r0.y, r1.y - a.y};
  for (int i = 0; i < 4; ++i) {
    if (std::fabs(p[i]) < 1e-12) {
      if (q[i] < 0) return false;
      continue;
    }
    const double r = q[i] / p[i];
    if (p[i] < 0) u0 = std::max(u0, r);
    else u1 = std::min(u1, r);
    if (u0 > u1) return false;
  }
  return true;
}

// K-28: the curve shapes of the bend point menu, in menu order (names are i18n keys).
struct BendCurveItem { uint8_t curve; const char* name; };
const BendCurveItem kBendCurves[] = {{bend::Straight, i18n::N_("Straight")}, {bend::Smooth, i18n::N_("Smooth")},
                                     {bend::EaseIn, i18n::N_("Ease in")},    {bend::EaseOut, i18n::N_("Ease out")},
                                     {bend::Step, i18n::N_("Step (hold, then jump)")}};
const char* bendCurveName(uint8_t c) {
  for (const auto& k : kBendCurves)
    if (k.curve == c) return k.name;
  return kBendCurves[0].name;
}
// A small drawing of a segment shape (rising) in the box p0-p1, the same function the line uses.
void drawCurveGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, uint8_t curve, float tension, ImU32 col, ImU32 faint) {
  dl->AddLine(ImVec2(p0.x, p1.y + 0.5f), ImVec2(p1.x, p1.y + 0.5f), faint, 1.0f);
  ImVec2 pts[34];
  int n = 0;
  if (curve == bend::Step) {
    pts[n++] = ImVec2(p0.x, p1.y), pts[n++] = ImVec2(p1.x - 1, p1.y), pts[n++] = ImVec2(p1.x - 1, p0.y);
  } else {
    for (int i = 0; i <= 32; ++i) {
      const double x = i / 32.0;
      pts[n++] = ImVec2(p0.x + static_cast<float>(x) * (p1.x - p0.x), p1.y - static_cast<float>(bend::shape(curve, tension, x)) * (p1.y - p0.y));
    }
  }
  dl->AddPolyline(pts, n, col, ImDrawFlags_None, 1.6f);
  dl->AddCircleFilled(ImVec2(p0.x, p1.y), 1.8f, col, 8);
  dl->AddCircleFilled(pts[n - 1], 1.8f, col, 8);
}
// One curve choice: its glyph at the left, then its name; a lit dot marks the current one.
bool curveMenuItem(const char* id, const char* label, uint8_t curve, float tension, bool on, float labelW) {
  const auto& P = pal();
  const float fs = ImGui::GetFontSize(), gw = std::round(fs * 1.9f), h = std::round(fs + 8.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool hit = ImGui::Selectable(id, false, ImGuiSelectableFlags_None, ImVec2(gw + 10.0f + labelW + fs + 4.0f, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 lit = theme::litRole(theme::role::kGraph);
  drawCurveGlyph(dl, ImVec2(p.x + 2, p.y + 4), ImVec2(p.x + gw, p.y + h - 5), curve, tension, on ? lit : P.textDim, theme::withAlpha(P.textFaint, 0.5f));
  dl->AddText(ImVec2(p.x + gw + 10.0f, std::round(p.y + (h - fs) * 0.5f)), on ? P.text : P.textDim, label);
  if (on) dl->AddCircleFilled(ImVec2(p.x + gw + 10.0f + labelW + fs * 0.5f + 4.0f, p.y + h * 0.5f), 3.0f, lit, 12);
  return hit;
}

}  // namespace

const PianoLaneGeometry& pianoLaneGeometry() { return laneGeo; }

// Notes added by Draw / Paint: one note, or a chord when a stamp is chosen; on
// the scale when snapping to it.
std::vector<Note> notesToAdd(const PianoRollState& st, Note n) {
  if (st.snapToScale && st.scale > 0) n.pitch = tools::snapPitchToScale(n.pitch, st.scaleRoot, st.scale);
  if (st.stampAuto > 0 && st.scale > 0) {  // J-27: a chord from the scale
    std::vector<Note> out;
    for (int p : ops::scaleChord(n.pitch, st.scaleRoot, st.scale, 3, st.stampAuto == 2)) {
      Note x = n;
      x.pitch = p;
      out.push_back(x);
    }
    return out;
  }
  if (st.stampChord < 0) return {n};
  return tools::stampChord(n, st.stampChord, st.stampInversion);
}

void drawPianoRoll(App& a) {
  drawEditTools(a);  // J-11 tool windows (quantize, chop, strum, …)
  drawScriptTool(a);  // J-24 piano roll script window
  if (!a.showPiano) return;
  if (!ui::BeginDockWindow("Piano roll", &a.showPiano, ImVec2(900, 520))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  PianoRollState& st = a.piano;
  const Pattern* pat = s.pattern(a.curPattern);
  const Channel* chan = s.channel(a.selChannel);
  // J-159: an instrument that names its keys (a deck setup's performance keys) shows the names
  const PluginInfo* chanInfo = chan ? findPlugin(chan->plugin.type) : nullptr;
  auto keyLabel = [&](int p) -> const std::string* {
    if (!chanInfo || p < 0 || p >= static_cast<int>(chanInfo->keyNames.size())) return nullptr;
    const std::string& n = chanInfo->keyNames[static_cast<size_t>(p)];
    return n.empty() ? nullptr : &n;
  };
  if (st.selPattern != a.curPattern || st.selChannel != a.selChannel) {
    st.selected.clear();
    st.selPattern = a.curPattern;
    st.selChannel = a.selChannel;
  }
  j31::pianoKey(a, st.scaleRoot, st.scale);  // J-31: scale highlighting follows key markers

  // ---------------- toolbar ----------------
  ui::BeginToolbar("##prtb");
  {
    const float fh = ImGui::GetFrameHeight();
    const float W = ImGui::GetContentRegionAvail().x;
    menuButton("prmenu", "Piano roll menu: tools, selection, chords, snap, view", [&a]() { pianoRollMenu(a); });
    ImGui::SameLine(0, 4);
    // Channel picker (colour chip + name).
    const float chanW = std::clamp(W * 0.22f, 110.0f, 180.0f);
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##chanbtn", ImVec2(chanW, fh));
      bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##chanpop");
      if (ImGui::IsItemClicked()) ImGui::OpenPopup("##chanpop");
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      tdl->AddRectFilled(p, ImVec2(p.x + chanW, p.y + fh), open || hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, theme::kRadius);
      if (chan) tdl->AddRectFilled(ImVec2(p.x + 6, p.y + 6), ImVec2(p.x + 10, p.y + fh - 6), theme::rgb(chan->color), 2);
      theme::textEllipsis(tdl, a.fontRegular, theme::kFontBody, ImVec2(p.x + 16, std::round(p.y + (fh - theme::kFontBody) * 0.5f)),
                          chanW - 36, P.text, chan ? chan->name.c_str() : "No channel");
      ui::drawIcon(tdl, ui::Icon::ChevronDown, ImVec2(p.x + chanW - 12, p.y + fh * 0.5f), 11, P.textDim);
      ui::Hint("Channel being edited");
      ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2));
      if (ImGui::BeginPopup("##chanpop")) {
        for (const auto& c : s.channels) {
          ImGui::PushID(static_cast<int>(c.id));
          ImVec2 q = ImGui::GetCursorScreenPos();
          float lh = ImGui::GetTextLineHeight();
          if (ImGui::Selectable("##c", c.id == a.selChannel, 0, ImVec2(std::max(160.0f, chanW), 0))) a.selectChannel(c.id);
          ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + 6, q.y + lh - 2), theme::rgb(c.color), 2);
          ImGui::GetWindowDrawList()->AddText(ImVec2(q.x + 14, q.y), P.text, c.name.c_str());
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
    }
    ui::ToolbarDivider();
    // Q-12: the shared tool family; tooltips "Name (key): what it does" from the keymap.
    struct T { ui::Icon icon; Tool tool; const char* name; const char* desc; const char* key; uint32_t role; };
    const T tools[] = {{ui::Icon::ToolDraw, Tool::Draw, i18n::N_("Draw"), i18n::N_("click to add, drag to move, drag the end to resize, right-click deletes"), "piano.toolDraw", theme::role::kDraw},
                       {ui::Icon::ToolBend, Tool::Bend, i18n::N_("Pitch bend"), i18n::N_("drag anywhere on a note to bend its pitch like an automation line (semitone steps, Alt: free); right-click a point: its curve menu, Shift+click: its next curve; double-click a point to remove it, a curve to straighten it, the note to reset its line"), "piano.toolBend", theme::role::kGraph},
                       {ui::Icon::ToolPaint, Tool::Paint, i18n::N_("Paint"), i18n::N_("paint notes while dragging"), "piano.toolPaint", theme::role::kPaint},
                       {ui::Icon::ToolDelete, Tool::Erase, i18n::N_("Delete"), i18n::N_("click or drag over notes to delete them"), "piano.toolDelete", theme::role::kDelete},
                       {ui::Icon::ToolSelect, Tool::Select, i18n::N_("Select"), i18n::N_("drag a box; Ctrl+drag selects with any tool"), "piano.toolSelect", theme::role::kSelect},
                       {ui::Icon::ToolSlice, Tool::Slice, i18n::N_("Slice"), i18n::N_("drag across notes to cut them"), "piano.toolSlice", theme::role::kSlice},
                       {ui::Icon::ToolMute, Tool::Mute, i18n::N_("Mute"), i18n::N_("click or drag over notes to mute / unmute them"), "piano.toolMute", theme::role::kMute},
                       {ui::Icon::ToolPlay, Tool::Play, i18n::N_("Playback"), i18n::N_("hold and move to hear the notes under the pointer"), "piano.toolPlay", theme::role::kPlay},
                       {ui::Icon::ToolZoom, Tool::Zoom, i18n::N_("Zoom"), i18n::N_("drag a range to fill the view, click zooms in, right-click zooms out"), "piano.toolZoom", theme::role::kZoom},
                       {ui::Icon::ToolDetune, Tool::Detune, i18n::N_("Detune"), i18n::N_("drag a note up / down to change its fine pitch (Shift: finer)"), "piano.toolDetune", theme::role::kSlip}};
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
      if (i) ImGui::SameLine(0, 2);
      if (ui::ToolButton(tools[i].key, tools[i].icon, st.tool == tools[i].tool, fh, tools[i].name, tools[i].desc, tools[i].key,
                         theme::rgb(tools[i].role)))
        st.tool = tools[i].tool;
      if (st.tool == tools[i].tool) ui::noteActiveTool("piano", static_cast<int>(tools[i].tool), tools[i].name, tools[i].key);
    }
    // Groups to the right of the tools appear only when they fit (no clipping).
    auto roomLeft = []() { return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetItemRectMax().x; };
    if (roomLeft() > fh + 6) {  // K-4: auto scrolling, the pattern editors' switch (beside the tools, as in the playlist)
      ImGui::SameLine(0, 4);
      const bool follow = a.settings.getBool("ui.autoScrollPattern");
      if (ui::IconButton("prfollow", ui::Icon::ScrollTo, follow, fh,
                         follow ? "Auto scrolling is on: the piano roll and channel rack follow the pattern position while it plays (click to keep the view still)"
                                : "Auto scrolling is off: the view stays where you put it while the pattern plays (click to follow the pattern position)",
                         theme::rgb(theme::role::kView)))
        a.settings.set("ui.autoScrollPattern", !follow);
    }
    if (roomLeft() > fh + 2 + 128 + 18) {
      ui::ToolbarDivider();
      ui::ToolButton("snapicon", ui::Icon::ToolSnap, st.snap != Snap::None, fh, i18n::N_("Snap"),
                     i18n::N_("snap to the grid chosen beside it (hold Alt while dragging to bypass)"), nullptr, theme::rgb(theme::role::kSnap));
      ImGui::SameLine(0, 2);
      std::string snapLabel = snapName(st.snap);
      if ((st.snap == Snap::Line || st.snap == Snap::Cell) && st.effGrid > 0)
        snapLabel += " \xC2\xB7 " + timeline::gridName(s, st.effGrid);
      if (ui::BeginCombo("##snap", snapLabel.c_str(), 128)) {
        for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
          if (ImGui::Selectable(snapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) st.snap = static_cast<Snap>(i);
        ImGui::EndCombo();
      }
      ui::Hint("Snap: Line and Cell follow the zoom (free at the deepest zoom); hold Alt while dragging to invert snapping");
    }
    float room = roomLeft();
    if (room > 120 + 60) {
      ui::ToolbarDivider();
      if (ui::TextButton("Quantize", false, 0, "Quantize note starts to the snap grid (Ctrl+Q)") && pat && chan) {
        if (std::vector<Note> ns = rollNotes(a, pat->id, chan->id); !ns.empty()) {
          Tick gsz = st.effGrid > 1 ? st.effGrid : snapTicks(s, st.snap);
          auto sel = selVector(a, ns.size());
          bool any = !st.selected.empty();
          for (size_t i = 0; i < ns.size(); ++i)
            if (!any || sel[i]) ns[i].start = snapNearest(ns[i].start, gsz);
          commit(a, pat->id, chan->id, ns, sel, "quantize notes", 0);
        }
      }
      ImGui::SameLine(0, 4);
      if (ui::IconButton("ghosts", ui::Icon::Clone, st.ghostNotes, fh, "Ghost notes: show other channels' notes (double-click a ghost note to edit its channel)",
                         theme::rgb(theme::role::kGhost)))
        st.ghostNotes = !st.ghostNotes;
    }
    if (roomLeft() > 110 + 120 + 30) {  // J-11: chord stamp + scale helper
      ui::ToolbarDivider();
      if (ui::IconButton("stampicon", ui::Icon::Stamp, st.stampChord >= 0, fh, "Chord stamp: Draw and Paint place this chord",
                         theme::rgb(theme::role::kStamp)))
        st.stampChord = st.stampChord >= 0 ? -1 : 0;
      ImGui::SameLine(0, 2);
      const auto& ch = tools::chords();
      if (ui::BeginCombo("##stamp", st.stampChord >= 0 ? ch[static_cast<size_t>(st.stampChord)].name : "No stamp", 110)) {
        if (ImGui::Selectable(i18n::tr("No stamp"), st.stampChord < 0)) st.stampChord = -1;
        for (size_t i = 0; i < ch.size(); ++i)
          if (ImGui::Selectable(ch[i].name, st.stampChord == static_cast<int>(i))) st.stampChord = static_cast<int>(i);
        ImGui::Separator();
        static const char* const inv[] = {"Root position", "1st inversion", "2nd inversion", "3rd inversion"};
        for (int k = 0; k < 4; ++k)
          if (ImGui::Selectable(inv[k], st.stampInversion == k)) st.stampInversion = k;
        ImGui::EndCombo();
      }
      ui::Hint("Chord stamp: the chord Draw and Paint place (root = the clicked key)");
      ImGui::SameLine(0, 4);
      static const char* const keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const auto& sc = tools::scales();
      std::string sl = st.scale > 0 ? std::string(keys[st.scaleRoot]) + " " + sc[static_cast<size_t>(st.scale)].name : "No scale";
      if (ui::BeginCombo("##scale", sl.c_str(), 120)) {
        if (ImGui::Selectable(i18n::tr("No scale"), st.scale == 0)) st.scale = 0;
        for (size_t i = 1; i < sc.size(); ++i)
          if (ImGui::Selectable(sc[i].name, st.scale == static_cast<int>(i))) st.scale = static_cast<int>(i);
        ImGui::Separator();
        for (int k = 0; k < 12; ++k) {
          if (k % 6) ImGui::SameLine(0, 2);
          ImGui::PushID(k);
          if (ui::TextButton(keys[k], st.scaleRoot == k, 26)) st.scaleRoot = k;
          ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::Checkbox(i18n::tr("Snap notes to the scale"), &st.snapToScale);
        ImGui::EndCombo();
      }
      ui::Hint("Scale highlighting: rows outside the scale are shaded; optionally drawn notes snap to it");
    }
    // Slide button and note colour: shown when both fit (divider, button, swatch).
    const float slideNeed = 2 * theme::kS2 + 1 + ImGui::CalcTextSize(i18n::tr("Slide")).x + 2 * ImGui::GetStyle().FramePadding.x + 4 + fh + 2;
    if (roomLeft() > slideNeed) {
      ui::ToolbarDivider();
      if (ui::TextButton("Slide", newSlide, 0, "Slide mode: new notes glide the channel's sounding notes to their pitch instead of starting a note"))
        newSlide = !newSlide;
      ImGui::SameLine(0, 4);
      const ImU32 chanCol = chan ? theme::rgb(chan->color) : P.textDim;
      ImVec2 sp = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##notecolor", ImVec2(fh, fh))) ImGui::OpenPopup("##colorpop");
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sp.x + 4, sp.y + 4), ImVec2(sp.x + fh - 4, sp.y + fh - 4),
                                                groupColor(newColor, chanCol), 3);
      if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRect(sp, ImVec2(sp.x + fh, sp.y + fh), P.lineStrong, theme::kRadius);
      ui::Hint("Note colour group for new notes; picking a colour also recolours the selected notes");
      if (ImGui::BeginPopup("##colorpop")) {
        for (int k = 0; k < kNoteColors; ++k) {
          if (k % 8) ImGui::SameLine(0, 4);
          ImVec2 q = ImGui::GetCursorScreenPos();
          ImGui::PushID(k);
          if (ImGui::InvisibleButton("##c", ImVec2(20, 20))) {
            newColor = k;
            if (pat && chan && !st.selected.empty()) {
              if (std::vector<Note> ns = rollNotes(a, pat->id, chan->id); !ns.empty()) {
                auto sel = selVector(a, ns.size());
                for (size_t i = 0; i < ns.size(); ++i)
                  if (sel[i]) ns[i].color = k;
                commit(a, pat->id, chan->id, ns, sel, "colour notes", 0);
              }
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::PopID();
          ImDrawList* pdl = ImGui::GetWindowDrawList();
          pdl->AddRectFilled(q, ImVec2(q.x + 20, q.y + 20), groupColor(k, chanCol), 3);
          if (k == newColor) pdl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q.x + 21, q.y + 21), P.accent, 3.0f, 0, 2.0f);
        }
        ImGui::EndPopup();
      }
    }
    if (roomLeft() > 76) {
      ui::ToolbarDivider();
      if (ui::IconButton("zout", ui::Icon::Minus, false, fh, "Zoom out (PgDn, Ctrl+wheel)")) st.zoomRequest /= 1.5f;
      ImGui::SameLine(0, 2);
      if (ui::IconButton("zin", ui::Icon::Plus, false, fh, "Zoom in (PgUp, Ctrl+wheel)")) st.zoomRequest *= 1.5f;
    }
  }
  ui::EndToolbar();

  if (!pat || !chan) {
    ImVec2 mn = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ImGui::Dummy(av);
    ui::EmptyState(ImGui::GetWindowDrawList(), mn, ImVec2(mn.x + av.x, mn.y + av.y), "Nothing to edit",
                   "Select a pattern and a channel to edit its notes");
    ui::EndDockWindow();
    return;
  }

  const ObjectId patId = pat->id, chId = chan->id;
  std::vector<Note> notes = rollNotes(a, patId, chId);  // K-22: rack steps show as step notes
  const bool rackSteps = stepnotes::hasSteps(s, *pat, chId);
  for (auto it = st.selected.begin(); it != st.selected.end();)
    it = *it < notes.size() ? std::next(it) : st.selected.erase(it);

  // ---------------- canvas geometry ----------------
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 160.0f);
  avail.y = std::max(avail.y, 140.0f);
  const float keyW = 64.0f, rulerH = 26.0f, velH = std::clamp(avail.y * 0.18f, 48.0f, 96.0f);
  ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::BeginDragDropTarget()) {  // MIDI files from the browser (J-16)
    browserui::acceptFileDrop(a, browse::DropTarget::PianoRoll, {});
    ImGui::EndDragDropTarget();
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();

  const ImVec2 gridMin(origin.x + keyW, origin.y + rulerH);
  const ImVec2 gridMax(origin.x + avail.x, origin.y + avail.y - velH);
  const float gridW = gridMax.x - gridMin.x, gridH = gridMax.y - gridMin.y;
  // ---- K-24: row height zoom (Alt+wheel), anchored at the pointer ----
  // The key row under the pointer and the point inside it stay still, gliding
  // like J-188's horizontal zoom (a trackpad's fractional Alt+scroll 1:1); a
  // height written from outside (View menu) glides with the top row held.
  std::array<float, 129> keyTop{};
  auto layoutKeys = [&](float h) {
    for (size_t i = 0; i < keyTop.size(); ++i) keyTop[i] = static_cast<float>(i) * h;
  };
  {
    smoothzoom::Params zp;
    zp.smooth = scrollui::smoothZoom();
    smoothzoom::RowGlide& vg = st.rowGlide;
    auto anchorAt = [&](float h, float px) {
      layoutKeys(h);
      return smoothzoom::rowAnchorAt(keyTop.data(), 128, h, static_cast<double>(px + st.scrollY), px);
    };
    const float held = st.rowHeight;
    const float drawn = vg.outside(held) ? vg.output() : held;
    if (vg.outside(held)) vg.zoomTo(drawn, std::clamp(held, kRowHMin, kRowHMax), anchorAt(drawn, 0.0f), true, zp);
    else if (vg.active() && st.scrollY != vg.scrollOut) vg.setAnchor(anchorAt(drawn, static_cast<float>(vg.anchor().px)));
    if (hovered && ((io.MouseWheel != 0 && !io.KeyCtrl && !io.KeyAlt) || io.MouseWheelH != 0 || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
      vg.finish();
    if (hovered && io.MouseWheel != 0 && io.KeyAlt && !io.KeyCtrl && st.centred) {
      const bool notch = smoothscroll::isNotch(io.MouseWheel);
      double to = std::clamp(vg.target(drawn) * std::pow(kRowZoomStep, static_cast<double>(io.MouseWheel)), static_cast<double>(kRowHMin),
                             static_cast<double>(kRowHMax));
      if (notch) to = std::round(to);  // a notch lands on whole pixels: crisp key rows (a trackpad moves freely)
      vg.zoomTo(drawn, to, anchorAt(drawn, std::clamp(io.MousePos.y - gridMin.y, 0.0f, gridH)), notch, zp);
    }
    st.rowHeight = static_cast<float>(vg.step(drawn, io.DeltaTime, zp));
  }
  const float rowH = st.rowHeight;
  layoutKeys(rowH);
  const Tick patLen = s.patternContentLength(*pat);
  const Tick viewTicks = std::max<Tick>(patLen + s.barTicks() * 4, static_cast<Tick>(gridW / st.pxPerTick) + 1);
  // First open: centre on the notes (C5-ish at the top when there are none).
  // Re-centred for a few frames while the dock layout settles the window size.
  static int settleFrames = 0;
  if (!st.centred) settleFrames = 8, st.centred = true;  // J-67: not "scrollY < 0": that is an elastic overscroll now
  if (settleFrames > 0) {
    --settleFrames;
    st.scrollY = (127 - 72) * rowH;
    if (!notes.empty()) {
      int lo = 127, hi = 0;
      for (const auto& n : notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
      st.scrollY = (127 - (lo + hi) * 0.5f) * rowH - gridH * 0.5f;
    }
  }
  if (st.rowGlide.anchored()) {  // K-24: the anchored point back under the pointer (the range's edges hold)
    const smoothzoom::RowAnchor& an = st.rowGlide.anchor();
    st.scrollY = std::clamp(static_cast<float>(smoothzoom::rowAnchorPos(keyTop.data(), 128, rowH, an) - an.px), 0.0f, std::max(0.0f, 128 * rowH - gridH));
    st.rowGlide.scrollOut = st.scrollY;
  }
  // J-67: outside writes are clamped here; the view's own glide / elastic
  // overscroll is left to scrollui::step below.
  scrollui::clampOuter(st.axY, st.scrollY, 0.0f, std::max(0.0f, 128 * rowH - gridH));
  scrollui::clampOuter(st.axX, st.scrollX, 0.0f, std::max(0.0f, viewTicks * st.pxPerTick - gridW * 0.5f));

  auto xOf = [&](Tick t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto yOf = [&](int pitch) { return gridMin.y + (127 - pitch) * rowH - st.scrollY; };
  auto tickAt = [&](float x) { return timeline::tickAtPx(x - gridMin.x + st.scrollX, st.pxPerTick); };
  // J-46: clamped before the conversion (the pointer is -FLT_MAX outside the window).
  auto pitchAt = [&](float y) { return 127 - static_cast<int>(std::clamp(std::floor((y - gridMin.y + st.scrollY) / rowH), 0.0f, 127.0f)); };
  // ---- zoom: Ctrl+wheel at the pointer, PgUp/PgDn, Home = fit the pattern ----
  // J-188: on the logical zoom and scroll; zoomEnd below makes them glide.
  scrollui::zoomBegin(st.zoomGlide, st.pxPerTick, st.scrollX);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (keys::pressed("piano.zoomIn")) st.zoomRequest *= 1.5f;
    if (keys::pressed("piano.zoomOut")) st.zoomRequest /= 1.5f;
    if (keys::pressed("piano.zoomFit")) st.fitRequest = true;
  }
  if (!st.zoomInit) {
    // Default zoom: a 16th note ~20 px wide (readable note blocks and step
    // columns at any timebase), unless the canvas can't show one bar then.
    const float sixteenth = static_cast<float>(std::max(1, s.ppq / 4));
    st.pxPerTick = std::min(20.0f / sixteenth, std::max(8.0f / sixteenth, gridW / static_cast<float>(std::max<Tick>(1, s.barTicks()))));
    st.zoomInit = true;
  }
  if (st.fitRequest) {
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, std::max<Tick>(patLen, s.barTicks()) + s.beatTicks(), gridW));
    st.scrollX = 0;
    st.fitRequest = false;
  }
  if (st.zoomRequest != 1.0f) {
    float anchor = hovered && io.MousePos.x >= gridMin.x ? io.MousePos.x - gridMin.x : gridW * 0.5f;
    timeline::zoomAt(s, st.pxPerTick, st.scrollX, st.zoomRequest, anchor);
    st.zoomRequest = 1.0f;
  }
  const bool wheelZoom = hovered && io.MouseWheel != 0 && io.KeyCtrl;
  if (wheelZoom) timeline::zoomAt(s, st.pxPerTick, st.scrollX, std::pow(1.2, static_cast<double>(io.MouseWheel)), io.MousePos.x - gridMin.x);
  st.pxPerTick = static_cast<float>(timeline::clampZoom(s, st.pxPerTick));
  scrollui::zoomEnd(st.zoomGlide, st.pxPerTick, st.scrollX, wheelZoom && !smoothscroll::isNotch(io.MouseWheel));
  // K-4: auto scrolling: page along with the pattern position while this pattern plays
  // (not while the user drags or scrolls the view; off: the view stays where it is).
  if (a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == patId && a.settings.getBool("ui.autoScrollPattern") &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    st.scrollX = ops::followScroll(st.scrollX, static_cast<float>(a.engine.position()) * st.pxPerTick, gridW);
  // ---- snapping: the visible grid (Line/Cell) or a fixed size; Alt inverts ----
  const Tick visGrid = timeline::visibleGrid(s, st.pxPerTick);
  st.effGrid = timeline::snapGrid(s, st.snap, st.pxPerTick, false);
  const Tick grid = std::max<Tick>(1, st.effGrid);  // keyboard nudges / quantize
  const Tick snapG = timeline::snapGrid(s, st.snap, st.pxPerTick, io.KeyAlt);
  const Tick defLen = st.lastLength > 0 ? st.lastLength : std::max<Tick>(1, s.ppq / 4);

  // Scroll & zoom.
  // J-67: wheel / trackpad input goes through the smooth scroller: a notch
  // glides three keys (or 60 px) and lands on them, trackpad deltas stay 1:1,
  // the edges stretch a little and spring back instead of jumping.
  float wheelX = 0, wheelY = 0;
  bool notchX = false, notchY = false;
  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) {
      // zoomed above (J-188)
    } else if (io.KeyAlt) {
      // the row height, zoomed above (K-24)
    } else if (io.KeyShift) {
      wheelX -= io.MouseWheel * 60;
      notchX = smoothscroll::isNotch(io.MouseWheel);
    } else {
      wheelY -= io.MouseWheel * rowH * 3;
      notchY = smoothscroll::isNotch(io.MouseWheel);
    }
  }
  if (hovered && io.MouseWheelH != 0) {
    wheelX -= io.MouseWheelH * 60;
    notchX = smoothscroll::isNotch(io.MouseWheelH);
  }
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
    wheelX -= io.MouseDelta.x;
    wheelY -= io.MouseDelta.y;
    notchX = notchY = false;
  }
  scrollui::step(st.axX, st.scrollX, wheelX, notchX, 0.0f, std::max(0.0f, viewTicks * st.pxPerTick - gridW * 0.5f));
  scrollui::step(st.axY, st.scrollY, wheelY, notchY, 0.0f, std::max(0.0f, 128 * st.rowHeight - gridH));
  if (canchor::collecting()) {  // J-111d: collaborators' pointers in pattern time and keys (this pattern and channel)
    const float pxT = std::max(1e-6f, st.pxPerTick), bottom = origin.y + avail.y;
    const ImVec2 o(gridMin.x - st.scrollX, gridMin.y - st.scrollY);
    canchor::publish(canchor::PianoGrid, patId, chId, o, ImVec2(pxT, rowH), gridMin, gridMax);
    canchor::publish(canchor::PianoKeys, patId, chId, ImVec2(origin.x, o.y), ImVec2(keyW, rowH), ImVec2(origin.x, gridMin.y),
                     ImVec2(gridMin.x, gridMax.y));
    canchor::publish(canchor::PianoRuler, patId, chId, ImVec2(o.x, origin.y), ImVec2(pxT, rulerH), ImVec2(gridMin.x, origin.y),
                     ImVec2(gridMax.x, gridMin.y));
    canchor::publish(canchor::PianoVelocity, patId, chId, ImVec2(o.x, gridMax.y), ImVec2(pxT, std::max(1.0f, bottom - gridMax.y)),
                     ImVec2(gridMin.x, gridMax.y), ImVec2(gridMax.x, bottom));
  }

  // ---------------- draw: grid ----------------
  dl->PushClipRect(gridMin, gridMax, true);
  // Grid well: black-key rows a step darker, every other bar a step lighter.
  dl->AddRectFilled(gridMin, gridMax, P.well);
  {
    const Tick bar = std::max<Tick>(1, s.barTicks());
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gridMin.x)), bar); xOf(t) < gridMax.x; t += bar)
      if ((t / bar) % 2 == 1)
        dl->AddRectFilled(ImVec2(xOf(t), gridMin.y), ImVec2(xOf(t + bar), gridMax.y), theme::withAlpha(P.paper, P.dark ? 0.03f : 0.015f));
  }
  for (int p = 0; p < 128; ++p) {
    float y = yOf(p);
    if (y > gridMax.y || y + rowH < gridMin.y) continue;
    if (isBlack(p)) dl->AddRectFilled(ImVec2(gridMin.x, y), ImVec2(gridMax.x, y + rowH), theme::withAlpha(P.ink, P.dark ? 0.16f : 0.05f));
    if (st.scale > 0) {  // J-11 scale highlighting: out-of-scale rows shaded, root rows tinted
      if (!tools::inScale(p, st.scaleRoot, st.scale))
        dl->AddRectFilled(ImVec2(gridMin.x, y), ImVec2(gridMax.x, y + rowH), theme::withAlpha(P.ink, P.dark ? 0.22f : 0.08f));
      else if ((p - st.scaleRoot) % 12 == 0)
        dl->AddRectFilled(ImVec2(gridMin.x, y), ImVec2(gridMax.x, y + rowH), theme::withAlpha(theme::rgb(theme::role::kStamp), 0.07f));
    }
    float ly = std::round(y + rowH);
    if (p % 12 == 0)
      dl->AddLine(ImVec2(gridMin.x, ly), ImVec2(gridMax.x, ly), P.gridBar);
    else if (rowH >= 10)
      dl->AddLine(ImVec2(gridMin.x, ly), ImVec2(gridMax.x, ly), P.gridStep);
  }
  {
    Tick step = std::max<Tick>(1, s.ppq / 4), beat = s.beatTicks(), bar = s.barTicks();
    // Lines at the visible grid (finer as you zoom in, down to single ticks).
    const ImU32 fine = theme::withAlpha(P.gridStep, 0.55f);
    Tick t0 = snapDown(tickAt(gridMin.x), visGrid);
    for (Tick t = std::max<Tick>(0, t0); xOf(t) < gridMax.x; t += visGrid) {
      ImU32 c = t % bar == 0 ? P.gridBar : t % beat == 0 ? P.gridBeat : t % step == 0 ? P.gridStep : fine;
      dl->AddLine(ImVec2(std::round(xOf(t)), gridMin.y), ImVec2(std::round(xOf(t)), gridMax.y), c);
    }
    // Pattern end shading.
    float xe = xOf(patLen);
    if (xe < gridMax.x) dl->AddRectFilled(ImVec2(std::max(xe, gridMin.x), gridMin.y), gridMax, theme::withAlpha(P.surface950, 0.55f));
  }
  j31::drawPianoBackground(a, gridMin.x, gridMin.y, gridMax.x, gridMax.y, static_cast<double>(tickAt(gridMin.x)),
                           1.0 / std::max(1e-6, static_cast<double>(st.pxPerTick)));  // J-31
  // Ghost notes from other channels (all others, or the chosen ones).
  struct GhostHit { ObjectId ch; int pitch; };
  GhostHit ghostHover{kNoId, -1};
  if (st.ghostNotes)
    for (const auto& [cid, ns] : pat->notes) {
      if (cid == chId || (!st.ghostChannels.empty() && !st.ghostChannels.count(cid))) continue;
      const Channel* oc = s.channel(cid);
      const ImU32 gcol = oc ? theme::rgb(oc->color) : P.text;
      for (const auto& n : ns) {
        float y = yOf(n.pitch);
        ImVec2 g0(xOf(n.start), y + 1), g1(xOf(n.start + n.length) - 1, y + rowH - 1);
        dl->AddRectFilled(g0, g1, theme::withAlpha(gcol, 0.13f), 2);
        dl->AddRect(g0, g1, theme::withAlpha(gcol, 0.32f), 2);
        if (hovered && io.MousePos.x >= g0.x && io.MousePos.x < g1.x && io.MousePos.y >= g0.y && io.MousePos.y < g1.y) ghostHover = {cid, n.pitch};
      }
    }
  // Notes.
  const ImU32 noteCol = theme::rgb(chan->color);
  int hoverNote = -1;
  bool hoverEdge = false;
  smooth::Tracker<3>::Vec noteOff{};  // J-37: drawn minus logical delta of the selected notes
  {
    const double now = ImGui::GetTime();
    if (!noteAnim.empty() && noteAnimT.pat == patId && noteAnimT.ch == chId) {
      const double epsT = 0.25 / std::max(1e-6, static_cast<double>(st.pxPerTick));
      const auto v = noteAnim.follow(0, noteAnimT.d, now, scrollui::smoothSnap() && snapG > 1, {epsT, 0.01, epsT});
      for (size_t k = 0; k < 3; ++k) noteOff[k] = v[k] - noteAnimT.d[k];
    }
    noteAnim.prune(now, g.kind == Gesture::Move || g.kind == Gesture::Resize);
  }
  const bool noteAnimOn = noteOff[0] != 0 || noteOff[1] != 0 || noteOff[2] != 0;
  struct NoteBox { float x0 = 0, x1 = 0, y = 0; };
  std::vector<NoteBox> noteBoxes(notes.size());
  struct NoteLabel { size_t note; float x0, x1, y; ImU32 fill; std::string text; };
  std::vector<NoteLabel> labels;  // K-28
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    bool sel = st.selected.count(i) > 0;
    float x0 = xOf(n.start), x1 = std::max(x0 + 3, xOf(n.start + n.length)), y = yOf(n.pitch);
    if (sel && noteAnimOn) {
      const float px = static_cast<float>(noteOff[0]) * st.pxPerTick;
      x0 += px;
      x1 = std::max(x0 + 3, xOf(n.start + n.length) + px + static_cast<float>(noteOff[2]) * st.pxPerTick);
      y -= static_cast<float>(noteOff[1]) * rowH;
    }
    noteBoxes[i] = {x0, x1, y};  // K-25: the bend pass draws on the note as drawn (gliding with a drag)
    if (x1 < gridMin.x || x0 > gridMax.x || y + rowH < gridMin.y || y > gridMax.y) continue;
    float vel = n.velocity / 127.0f;
    const ImU32 col = groupColor(n.color, noteCol);
    // Note: colour-group body whose brightness follows velocity, a darker
    // "title" cap on the left (name lives there), lit rim when selected.
    ImU32 fill = theme::mix(theme::mix(col, P.well, 0.55f), theme::mix(col, P.paper, 0.12f), vel);
    if (sel) fill = theme::mix(fill, P.paper, 0.45f);
    if (n.slide) fill = theme::withAlpha(fill, 0.55f);  // slide notes: lighter body + glide mark
    if (n.muted) fill = theme::withAlpha(theme::mix(fill, P.well, 0.6f), 0.6f);  // muted: dim, hollow look
    const ImVec2 n0(x0, y + 1), n1(x1 - 1, y + rowH - 1);
    dl->AddRectFilled(n0, n1, fill, theme::kRadiusSm);
    if (n.muted && x1 - x0 > 6) dl->AddLine(ImVec2(n0.x + 2, n1.y - 2), ImVec2(n1.x - 2, n0.y + 2), theme::withAlpha(theme::rgb(theme::role::kMute), 0.7f), 1.2f);
    if (x1 - x0 > 10 && rowH >= 9)
      dl->AddRectFilled(n0, ImVec2(std::min(n1.x, n0.x + 4), n1.y), theme::mix(fill, P.ink, 0.25f), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(n0.x + 1, n0.y + 0.5f), ImVec2(n1.x - 1, n0.y + 0.5f), theme::withAlpha(P.paper, 0.35f));
    dl->AddRect(n0, n1, sel ? P.hot : theme::withAlpha(P.ink, 0.55f), theme::kRadiusSm, 0, sel ? 1.5f : 1.0f);
    if (n.slide && x1 - x0 > 8) {
      float h = (rowH - 2) * 0.5f, cy = y + rowH * 0.5f, tx = x1 - 3;
      dl->AddTriangleFilled(ImVec2(tx - h, cy - h * 0.8f), ImVec2(tx, cy), ImVec2(tx - h, cy + h * 0.8f), theme::contrastText(fill));
    }
    if (n.porta) dl->AddCircleFilled(ImVec2(x0 + 4, y + rowH * 0.5f), std::max(1.5f, rowH * 0.14f), theme::contrastText(fill), 8);
    if (x1 - x0 > 28 && rowH >= 11) {  // K-28: drawn after the bend lines (labelPass below), so a line never hides a name
      const std::string* kn = keyLabel(n.pitch);
      labels.push_back({i, x0, x1, y, fill, kn ? *kn : noteName(n.pitch)});
    }
    if (hovered && io.MousePos.x >= x0 && io.MousePos.x < x1 && io.MousePos.y >= y && io.MousePos.y < y + rowH &&
        io.MousePos.y < gridMax.y && io.MousePos.x > gridMin.x) {
      hoverNote = static_cast<int>(i);
      hoverEdge = io.MousePos.x > x1 - std::min(8.0f, (x1 - x0) * 0.35f);
    }
  }
  // K-25 / K-28: pitch bend lines. The drawing order keeps lines and note names readable over
  // any note, in both themes: note bodies, then the bend lines, then the note names (a name a line
  // crosses gets a chip of its own note's colour behind it, so the line passes behind the name
  // with a clean edge), then the points and curve handles, then the line being dragged (or the
  // one under the pointer) with its points, on top of everything.
  // With the bend tool: every note's line, its points and the hover; otherwise only the lines that
  // bend, dimmed (still readable, never hidden). A dark halo under a light stroke (the reverse on
  // the light theme) keeps them readable over any note colour and the grid.
  const bool bendTool = st.tool == Tool::Bend;
  const int prevFront = bendFrontNote;
  bendHover = BendHit{};
  {
    static std::vector<std::vector<ImVec2>> pls;  // the lines drawn this frame (buffers reused)
    static std::vector<std::vector<int>> plSeg;   // K-29: the bend segment of each drawn piece
    static std::vector<size_t> plNote;
    size_t plCount = 0;
    const ImVec2 m = io.MousePos;
    const bool probe = bendTool && hovered && m.x >= gridMin.x && m.x < gridMax.x && m.y >= gridMin.y && m.y < gridMax.y &&
                       g.kind != Gesture::Bend && g.kind != Gesture::BendCurve &&
                       (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    auto better = [](const BendHit& h, const BendHit& best) {
      if (h.kind != best.kind) return h.kind > best.kind;
      if (h.selected != best.selected) return h.selected;
      return h.dist <= best.dist;  // ties: the note drawn later (on top)
    };
    const bool dragging = (g.kind == Gesture::Bend || g.kind == Gesture::BendCurve) && g.anchor >= 0;
    const int front = dragging ? g.anchor : bendTool ? prevFront : -1;
    for (size_t i = 0; i < notes.size(); ++i) {
      const Note& n = notes[i];
      if (n.slide || (!bendTool && n.bend.empty())) continue;
      const NoteBox& bx = noteBoxes[i];
      if (bx.x1 < gridMin.x - 8 || bx.x0 > gridMax.x + 8) continue;
      const float cy = bx.y + rowH * 0.5f;
      if (plCount >= pls.size()) pls.emplace_back(), plSeg.emplace_back(), plNote.emplace_back();
      std::vector<ImVec2>& pl = pls[plCount];
      std::vector<int>& ps = plSeg[plCount];
      plNote[plCount++] = i;
      bendPolyline(n, bx.x0, cy, st.pxPerTick, rowH, pl, ps);
      if (!probe) continue;
      BendHit h;
      h.note = static_cast<int>(i);
      h.selected = st.selected.count(i) > 0;
      for (size_t k = 0; k < n.bend.size(); ++k) {
        if (n.bend[k].tick > static_cast<float>(n.length)) break;
        const ImVec2 q(bx.x0 + n.bend[k].tick * st.pxPerTick, cy - n.bend[k].semis * rowH);
        const float d = std::hypot(q.x - m.x, q.y - m.y);
        if (d <= 7.0f && (h.kind < BendHit::Point || d < h.dist)) h.kind = BendHit::Point, h.point = static_cast<int>(k), h.dist = d;
      }
      if (h.kind < BendHit::Point)
        for (size_t k = 0; k < n.bend.size(); ++k) {
          ImVec2 q;
          if (!bendHandle(n, k, bx.x0, cy, st.pxPerTick, rowH, q)) continue;
          const float d = std::hypot(q.x - m.x, q.y - m.y);
          if (d <= 6.0f && (h.kind < BendHit::Handle || d < h.dist)) h.kind = BendHit::Handle, h.point = static_cast<int>(k), h.dist = d;
        }
      if (h.kind < BendHit::Handle)
        for (size_t k = 1; k < pl.size(); ++k) {
          const float d = segDist(m, pl[k - 1], pl[k]);
          if (d <= 5.0f && (h.kind < BendHit::Line || d < h.dist)) h.kind = BendHit::Line, h.dist = d, h.point = ps[k];  // K-29: its segment
        }
      if (h.kind < BendHit::Line && m.x >= bx.x0 && m.x < bx.x1 && m.y >= bx.y && m.y < bx.y + rowH) h.kind = BendHit::Body, h.dist = 0;
      if (h.kind > BendHit::None && better(h, bendHover)) bendHover = h;
    }
    bendFrontNote = bendHover.kind >= BendHit::Line ? bendHover.note : -1;
    // Colours of note i's line: `dim` 0 = not drawn.
    struct LineLook {
      float dim;
      ImU32 line, halo;
    };
    auto look = [&](size_t i) {
      const Note& n = notes[i];
      const bool sel = st.selected.count(i) > 0;
      const ImU32 base = groupColor(n.color, noteCol);
      LineLook L;
      L.dim = bendTool ? (n.muted ? 0.55f : 1.0f) : (n.bend.empty() ? 0.0f : 0.5f);
      L.line = P.dark ? theme::mix(base, theme::rgb(0xFFFFFF), sel ? 0.86f : 0.74f) : theme::mix(base, P.ink, 0.62f);
      L.halo = P.dark ? theme::withAlpha(P.ink, 0.6f * L.dim) : theme::withAlpha(P.paper, 0.8f * L.dim);
      return L;
    };
    auto drawLine = [&](size_t k) {
      const LineLook L = look(plNote[k]);
      if (L.dim <= 0) return;
      const std::vector<ImVec2>& pl = pls[k];
      dl->AddPolyline(pl.data(), static_cast<int>(pl.size()), L.halo, ImDrawFlags_None, bendTool ? 3.6f : 3.0f);
      dl->AddPolyline(pl.data(), static_cast<int>(pl.size()), theme::withAlpha(L.line, 0.95f * L.dim), ImDrawFlags_None, bendTool ? 1.6f : 1.25f);
    };
    auto drawPoints = [&](size_t k) {
      const size_t i = plNote[k];
      const Note& n = notes[i];
      const LineLook L = look(i);
      if (L.dim <= 0) return;
      const NoteBox& bx = noteBoxes[i];
      const float cy = bx.y + rowH * 0.5f;
      for (size_t j = 0; j < n.bend.size(); ++j) {  // K-28: curve handles (small rings half way along a segment)
        ImVec2 q;
        if (!bendHandle(n, j, bx.x0, cy, st.pxPerTick, rowH, q)) continue;
        const bool hot = (bendHover.kind == BendHit::Handle && bendHover.note == static_cast<int>(i) && bendHover.point == static_cast<int>(j)) ||
                         (g.kind == Gesture::BendCurve && g.anchor == static_cast<int>(i) && g.bendPt == static_cast<int>(j));
        const float r = hot ? 3.5f : 2.6f;
        dl->AddCircleFilled(q, r + 1.4f, L.halo, 12);
        dl->AddCircleFilled(q, r, hot ? L.line : (P.dark ? P.well : P.paper), 12);
        dl->AddCircle(q, r, theme::withAlpha(L.line, 0.8f), 12, 1.2f);
      }
      for (size_t j = 0; j < n.bend.size(); ++j) {
        if (n.bend[j].tick > static_cast<float>(n.length)) break;
        const ImVec2 q(bx.x0 + n.bend[j].tick * st.pxPerTick, cy - n.bend[j].semis * rowH);
        const bool hot = (bendHover.kind == BendHit::Point && bendHover.note == static_cast<int>(i) && bendHover.point == static_cast<int>(j)) ||
                         (bendMenuNote == static_cast<int>(i) && bendMenuPoint == static_cast<int>(j)) ||  // K-28: its menu is open
                         (g.kind == Gesture::Bend && g.anchor == static_cast<int>(i) && std::fabs(n.bend[j].tick - g.bendT) < 1e-3 &&
                          std::fabs(n.bend[j].semis - g.bendV) < 1e-3);
        const float r = hot ? 4.25f : 3.25f;
        if (n.bend[j].curve == bend::Step) {  // a step (hold, then jump) ends in a square: the shape reads at a glance
          dl->AddRectFilled(ImVec2(q.x - r - 1.5f, q.y - r - 1.5f), ImVec2(q.x + r + 1.5f, q.y + r + 1.5f), L.halo, 2.0f);
          dl->AddRectFilled(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), hot ? L.line : (P.dark ? P.well : P.paper), 1.5f);
          dl->AddRect(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), L.line, 1.5f, 0, 1.5f);
          continue;
        }
        dl->AddCircleFilled(q, r + 1.5f, L.halo, 16);
        dl->AddCircleFilled(q, r, hot ? L.line : (P.dark ? P.well : P.paper), 16);
        dl->AddCircle(q, r, L.line, 16, 1.5f);
      }
    };
    for (size_t k = 0; k < plCount; ++k)
      if (static_cast<int>(plNote[k]) != front) drawLine(k);
    // Note names, over the lines. A name is clipped where a later note on its row starts (that
    // note is drawn over it) and left out when a later note covers its start.
    if (!labels.empty()) {
      ImGui::PushFont(a.fontMono, std::min(11.0f, rowH - 3));
      const float fs = ImGui::GetFontSize();
      std::unordered_map<int, std::vector<size_t>> rows;  // notes in view by row
      for (size_t i = 0; i < notes.size(); ++i) {
        const NoteBox& bx = noteBoxes[i];
        if (bx.x1 >= gridMin.x && bx.x0 <= gridMax.x && bx.y + rowH >= gridMin.y && bx.y <= gridMax.y)
          rows[static_cast<int>(std::lround(bx.y * 4))].push_back(i);
      }
      for (const NoteLabel& lb : labels) {
        float clipR = lb.x1 - 2;
        bool covered = false;
        for (size_t j : rows[static_cast<int>(std::lround(lb.y * 4))]) {
          if (j <= lb.note) continue;
          const NoteBox& o = noteBoxes[j];
          if (o.x0 <= lb.x0 + 6 && o.x1 > lb.x0 + 6) covered = true;
          else if (o.x0 > lb.x0 && o.x0 < clipR) clipR = o.x0;
        }
        if (covered) continue;
        const ImVec2 tp(lb.x0 + 6, lb.y + (rowH - fs) * 0.5f);
        const ImVec2 ts = ImGui::CalcTextSize(lb.text.c_str());
        const ImVec2 c0(tp.x - 2, lb.y + 2), c1(std::min(tp.x + ts.x + 2, clipR), lb.y + rowH - 2);
        bool crossed = false;
        for (size_t k = 0; k < plCount && !crossed; ++k) {
          if (look(plNote[k]).dim <= 0) continue;
          const std::vector<ImVec2>& pl = pls[k];
          for (size_t q = 1; q < pl.size() && !crossed; ++q) crossed = segHitsRect(pl[q - 1], pl[q], ImVec2(c0.x - 2, c0.y - 2), ImVec2(c1.x + 2, c1.y + 2));
        }
        dl->PushClipRect(ImVec2(lb.x0, lb.y), ImVec2(clipR, lb.y + rowH), true);
        if (crossed) dl->AddRectFilled(c0, c1, lb.fill, theme::kRadiusSm);
        dl->AddText(tp, theme::withAlpha(theme::contrastText(lb.fill), 0.85f), lb.text.c_str());
        dl->PopClipRect();
      }
      ImGui::PopFont();
    }
    if (bendTool)
      for (size_t k = 0; k < plCount; ++k)
        if (static_cast<int>(plNote[k]) != front) drawPoints(k);
    for (size_t k = 0; k < plCount; ++k)
      if (static_cast<int>(plNote[k]) == front) {
        drawLine(k);
        if (bendTool) drawPoints(k);
      }
  }
  // Playhead.
  if (a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == patId) {
    float x = xOf(static_cast<Tick>(a.engine.position()));
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), P.playhead, 1.5f);
  }
  // Step record cursor: where the next played chord goes (Alt+click in the grid moves it).
  if (a.stepRecord && !a.engine.playing()) {
    // Record-red cursor: the step the next chord fills is tinted, a 2 px line
    // marks its start and a flag hangs from the top edge.
    float x = std::round(xOf(a.stepRecorder.cursor));
    const Tick stepG = std::max<Tick>(1, a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(a.doc.song(), a.piano.snap));
    const float xs = std::round(xOf(a.stepRecorder.cursor + stepG));
    dl->AddRectFilled(ImVec2(x, gridMin.y), ImVec2(std::max(x + 2, xs), gridMax.y), theme::withAlpha(P.danger, 0.10f));
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), theme::withAlpha(P.danger, 0.9f), 2.0f);
    dl->AddRectFilled(ImVec2(x, gridMin.y), ImVec2(x + 9, gridMin.y + 8), P.danger, 1.0f);
    dl->AddTriangleFilled(ImVec2(x, gridMin.y + 8), ImVec2(x + 9, gridMin.y + 8), ImVec2(x, gridMin.y + 13), P.danger);
    if (ImGui::IsWindowHovered() && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= gridMin.x) {
      Tick g = a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(a.doc.song(), a.piano.snap);
      double t = (io.MousePos.x - xOf(0)) / std::max(1e-6f, xOf(1) - xOf(0));
      a.stepRecorder.cursor = std::max<Tick>(0, snapDown(static_cast<Tick>(t), std::max<Tick>(1, g)));
    }
  }
  // Selection rectangle / slice line.
  if (g.kind == Gesture::Select && active)
    dl->AddRectFilled(g.start, io.MousePos, theme::withAlpha(P.accent, 0.12f)),
        dl->AddRect(g.start, io.MousePos, P.accent, 0, 0, 1.5f);
  if (g.kind == Gesture::Slice && active) {  // J-67: drawn on the snapped cut, gliding between grid lines (smooth motion)
    const Tick cut = std::max<Tick>(0, snapNearest(tickAt(io.MousePos.x), snapG));
    const float x = std::round(gridMin.x + static_cast<float>(scrollui::glide(0x50525343ull, static_cast<double>(cut), snapG > 1, 0.5)) * st.pxPerTick - st.scrollX);
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), P.danger, 1.5f);
  }
  if (g.kind == Gesture::Zoom && active) {
    const ImU32 zc = theme::rgb(theme::role::kZoom);
    dl->AddRectFilled(ImVec2(g.start.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), theme::withAlpha(zc, 0.10f));
    dl->AddRect(ImVec2(g.start.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), zc, 0, 0, 1.2f);
  }
  if (g.kind == Gesture::Play && active) dl->AddLine(ImVec2(io.MousePos.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), theme::rgb(theme::role::kPlay), 1.5f);
  dl->PopClipRect();

  // ---------------- keyboard ----------------
  {
    dl->PushClipRect(ImVec2(origin.x, gridMin.y), ImVec2(gridMin.x, gridMax.y), true);
    const ImU32 white = P.keyWhite;
    const ImU32 whiteEdge = theme::mix(P.keyWhite, P.ink, 0.28f);
    const ImU32 black = P.keyBlack;
    const float kx0 = origin.x, kx1 = gridMin.x - 1, blackW = std::round((kx1 - kx0) * 0.6f);
    const bool gliss = g.kind == Gesture::Keys;  // K-21
    const int hoverPitch = !gliss && hovered && io.MousePos.x < gridMin.x && io.MousePos.y >= gridMin.y && io.MousePos.y < gridMax.y
                               ? std::clamp(127 - static_cast<int>(std::floor((io.MousePos.y - gridMin.y + st.scrollY) / rowH)), 0, 127)
                               : -1;
    // White keys: light face shading to a darker tip (left), like a keyboard seen from above.
    dl->AddRectFilledMultiColor(ImVec2(kx0, gridMin.y), ImVec2(kx1, gridMax.y), theme::mix(white, P.ink, 0.12f), white, white,
                                theme::mix(white, P.ink, 0.12f));
    // J-77: keys sounding on this channel (playback, MIDI, typing keyboard, previews) light in the
    // channel colour while held and fade out smoothly on release.
    const keylight::ChannelLights* lit = keylight::lights(a, chId);
    const ImU32 litCol = theme::rgb(chan->color);
    // How lit key p is: sounding (fading on release), or the key being auditioned.
    auto keyLevel = [&](int p) {
      if (g.previewPitch == p) return 1.0f;
      return lit && (!gliss || lit->down[p]) ? lit->key(p) : 0.0f;  // K-21: no fading trail mid-glissando
    };
    // A lit key takes the channel colour, keeping a little of the key's own colour at full level so
    // a light channel colour doesn't turn into a flat glare.
    auto litTint = [&](ImU32 key, float lv) { return theme::mix(key, litCol, 0.8f * lv); };
    // A white key is wider than its row: it also owns the strip right of each neighbouring black key,
    // up to the seam through the middle of that black key's row. Lit and hovered white keys fill that
    // whole shape, shaded like the key face (darker towards the tip on the left). The strips start
    // part way along that shading, so they read as one surface with the key's main band.
    auto fillWhiteKey = [&](int p, float y, ImU32 face, ImU32 tip) {
      const ImU32 stripTip = theme::mix(tip, face, blackW / std::max(1.0f, kx1 - kx0));
      dl->AddRectFilledMultiColor(ImVec2(kx0, y), ImVec2(kx1, y + rowH), tip, face, face, tip);
      auto strip = [&](float y0, float y1) { dl->AddRectFilledMultiColor(ImVec2(kx0 + blackW, y0), ImVec2(kx1, y1), stripTip, face, face, stripTip); };
      if (p < 127 && isBlack(p + 1)) strip(y - rowH * 0.5f, y);         // lower half of the black row above
      if (p > 0 && isBlack(p - 1)) strip(y + rowH, y + rowH * 1.5f);    // upper half of the black row below
    };
    for (int p = 0; p < 128; ++p) {
      const float y = yOf(p);
      if (isBlack(p) || y - rowH > gridMax.y || y + rowH * 2 < gridMin.y) continue;
      const float lv = keyLevel(p);
      if (lv > 0.004f) {
        const ImU32 face = litTint(white, lv);
        fillWhiteKey(p, y, face, theme::mix(face, P.ink, 0.12f));
      } else if (hoverPitch == p) {
        fillWhiteKey(p, y, P.accent, P.accent);  // the accent chosen in Settings, as it is
      }
    }
    for (int p = 0; p < 128; ++p) {
      float y = yOf(p);
      if (y > gridMax.y || y + rowH < gridMin.y) continue;
      bool isB = isBlack(p);
      const float lv = keyLevel(p);
      ImU32 tint = !isB              ? 0  // white keys were filled above, over their whole shape
                   : lv > 0.004f     ? litTint(black, lv)
                   : hoverPitch == p ? P.accent
                                     : 0;
      if (!isB) {
        // Boundary between two adjacent white keys (E|F, B|C) gets a full line.
        if (p % 12 == 0 || p % 12 == 5) dl->AddLine(ImVec2(kx0, std::round(y + rowH)), ImVec2(kx1, std::round(y + rowH)), whiteEdge);
      } else {
        // The white-key seam continues to the right of the black key.
        dl->AddLine(ImVec2(kx0 + blackW, std::round(y + rowH * 0.5f)), ImVec2(kx1, std::round(y + rowH * 0.5f)), whiteEdge);
        dl->AddRectFilled(ImVec2(kx0, y), ImVec2(kx0 + blackW, y + rowH), tint ? tint : black, 2.0f, ImDrawFlags_RoundCornersRight);
        if (!tint && rowH >= 6)  // highlight along the black key's top face
          dl->AddRectFilled(ImVec2(kx0 + 2, y + 1.5f), ImVec2(kx0 + blackW - 3, y + 3), theme::withAlpha(P.paper, 0.14f), 1.0f);
      }
      if ((p % 12 == 0 || st.keyNames) && rowH >= 9 && !keyLabel(p)) {
        float fs = std::min(theme::kFontLabel, rowH - 1);
        std::string nn = noteName(p);
        ImVec2 ts = a.fontMono->CalcTextSizeA(fs, 1e9f, 0, nn.c_str());
        dl->AddText(a.fontMono, fs, ImVec2(kx1 - ts.x - 5, std::round(y + (rowH - ts.y) * 0.5f)),
                    lv <= 0.004f && hoverPitch == p ? theme::contrastText(P.accent) : theme::mix(P.keyWhite, P.ink, 0.6f), nn.c_str());
      }
    }
    // J-159: named keys are flat name plates over the keyboard (a drum-map look)
    if (chanInfo && !chanInfo->keyNames.empty())
      for (int p = 0; p < 128; ++p) {
        const std::string* kn = keyLabel(p);
        const float y = yOf(p);
        if (!kn || y > gridMax.y || y + rowH < gridMin.y) continue;
        const float lv = keyLevel(p);
        ImU32 plate = isBlack(p) ? P.surface800 : P.surface700;
        if (lv > 0.004f) plate = litTint(plate, lv);
        else if (hoverPitch == p) plate = P.accent;
        dl->AddRectFilled(ImVec2(kx0, y), ImVec2(kx1, y + rowH), plate);
        dl->AddLine(ImVec2(kx0, std::round(y + rowH)), ImVec2(kx1, std::round(y + rowH)), P.line);
        if (rowH < 7) continue;
        const float fs = std::min(theme::kFontLabel, rowH - 1);
        dl->PushClipRect(ImVec2(kx0, y), ImVec2(kx1 - 3, y + rowH), true);
        dl->AddText(a.fontMono, fs, ImVec2(kx0 + 4, std::round(y + (rowH - fs) * 0.5f)), lv <= 0.004f && hoverPitch == p ? theme::contrastText(plate) : P.text, kn->c_str());
        dl->PopClipRect();
      }
    dl->AddLine(ImVec2(kx1, gridMin.y), ImVec2(kx1, gridMax.y), P.lineStrong);
    dl->PopClipRect();
  }

  // ---------------- ruler ----------------
  {
    ImVec2 r0(gridMin.x, origin.y), r1(gridMax.x, origin.y + rulerH);
    dl->AddRectFilled(ImVec2(origin.x, origin.y), r1, P.dark ? P.field : P.surface950);
    dl->AddLine(ImVec2(origin.x, r1.y - 1), ImVec2(r1.x, r1.y - 1), P.dark ? P.ink : P.lineStrong);
    dl->PushClipRect(r0, r1, true);
    const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
    const Tick labelGrid = timeline::visibleGrid(s, st.pxPerTick, 64.0);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(r0.x)), visGrid); xOf(t) < r1.x; t += visGrid) {
      float x = std::round(xOf(t));
      bool major = t % labelGrid == 0;
      float len = major ? 10.0f : t % beat == 0 ? 6.0f : 3.0f;
      dl->AddLine(ImVec2(x, r1.y - len), ImVec2(x, r1.y - 1), major ? P.lineStrong : P.line);
      if (major) {
        std::string lbl = timeline::rulerLabel(s, t, labelGrid);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 4, origin.y + 4), t % bar == 0 ? P.text : P.textFaint, lbl.c_str());
      }
    }
    if (a.engine.mode() == PlayMode::Pattern) {  // position marker cap
      float px = std::round(xOf(static_cast<Tick>(a.engine.position())));
      dl->AddTriangleFilled(ImVec2(px - 5, r1.y - 9), ImVec2(px + 5, r1.y - 9), ImVec2(px, r1.y - 1), P.playhead);
    }
    float xe = std::round(xOf(patLen));
    dl->AddTriangleFilled(ImVec2(xe - 5, r1.y - 9), ImVec2(xe + 5, r1.y - 9), ImVec2(xe, r1.y - 2), P.textFaint);
    dl->PopClipRect();
    // Corner above the keyboard: pattern name.
    dl->AddRectFilled(origin, ImVec2(gridMin.x, r1.y), P.dark ? P.field : P.surface950);
    dl->AddLine(ImVec2(origin.x, r1.y - 1), ImVec2(gridMin.x, r1.y - 1), P.dark ? P.ink : P.lineStrong);
    theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(origin.x + 8, origin.y + 4), keyW - 12, P.textFaint, "NOTES");  // J-128: on the bar numbers' line
  }

  // ---------------- velocity lane ----------------
  const ImVec2 vMin(gridMin.x, gridMax.y + 8), vMax(gridMax.x, origin.y + avail.y - 4);
  // Event lane: dark well under the grid, separated by a raised splitter bar.
  dl->AddRectFilled(ImVec2(origin.x, gridMax.y), ImVec2(gridMax.x, origin.y + avail.y), P.dark ? P.field : P.surface950);
  dl->AddRectFilled(ImVec2(origin.x, gridMax.y), ImVec2(gridMax.x, gridMax.y + 3), P.dark ? P.surface800 : P.lineStrong);
  dl->AddLine(ImVec2(origin.x, gridMax.y + 3), ImVec2(gridMax.x, gridMax.y + 3), P.dark ? P.ink : P.lineStrong);
  {  // lane selector: the property shown and edited in the lane
    const ImVec2 l0(origin.x + 4, gridMax.y + 5), l1(gridMin.x - 4, gridMax.y + 5 + theme::kFontLabel + 6);
    const bool lh = hovered && io.MousePos.x >= l0.x && io.MousePos.x < l1.x && io.MousePos.y >= l0.y && io.MousePos.y < l1.y;
    if (lh) dl->AddRectFilled(l0, l1, P.surface800, theme::kRadius);
    const std::string autoLabel = patternLaneLabel(a);  // J-5: pattern automation lane shown instead
    theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(l0.x + 4, l0.y + 3), l1.x - l0.x - 16, lh ? P.text : P.textFaint,
                        autoLabel.empty() ? kLaneShort[static_cast<int>(lane)] : autoLabel.c_str());
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(l1.x - 6, (l0.y + l1.y) * 0.5f), 9, lh ? P.text : P.textFaint);
  }
  const bool bip = laneBipolar(lane);
  for (float f : {0.25f, 0.5f, 0.75f, 1.0f}) {
    float y = std::round(vMax.y - (vMax.y - vMin.y) * f);
    dl->AddLine(ImVec2(vMin.x, y), ImVec2(vMax.x, y), f == 1.0f || (bip && f == 0.5f) ? P.line : P.gridStep);
  }
  const bool autoLane = pianoAutomationLane().owner != kNoId;
  if (autoLane)
    drawPatternAutomationLane(
        a, dl, vMin, vMax, patId, [&](double t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; },
        [&](float x) { return static_cast<double>((x - gridMin.x + st.scrollX) / st.pxPerTick); }, snapG, hovered, active);
  dl->PushClipRect(ImVec2(vMin.x, vMin.y - 4), vMax, true);
  for (size_t i = 0; i < notes.size() && !autoLane; ++i) {
    const Note& n = notes[i];
    // Q-10: the bar glides to its new height (display only; the note already has the value).
    const uint64_t barKey = (static_cast<uint64_t>(chId) << 40) ^ (static_cast<uint64_t>(n.start) << 12) ^ (static_cast<uint64_t>(n.pitch) << 4) ^
                            static_cast<uint64_t>(lane);
    const float span = vMax.y - vMin.y, v = static_cast<float>(scrollui::glideBar(barKey, laneValue(n, lane)));
    float x = std::round(xOf(n.start)), yv = vMax.y - span * v, y0 = bip ? vMax.y - span * 0.5f : vMax.y;
    const bool lsel = st.selected.count(i) > 0;
    ImU32 c = lsel ? P.hot : theme::mix(groupColor(n.color, noteCol), P.paper, 0.15f);
    // Lollipop stem: a translucent bar to the note's length, a solid stem and a round head.
    float xe = std::round(xOf(n.start + n.length));
    if (xe - x > 4) dl->AddRectFilled(ImVec2(x + 2, std::min(yv, y0)), ImVec2(xe - 1, std::max(yv, y0)), theme::withAlpha(c, 0.12f));
    dl->AddRectFilled(ImVec2(x, std::min(yv, y0)), ImVec2(x + 2, std::max(yv, y0)), c);
    dl->AddCircleFilled(ImVec2(x + 1, yv), 3.5f, c, 12);
    dl->AddCircle(ImVec2(x + 1, yv), 3.5f, theme::withAlpha(P.ink, 0.5f), 12);
  }
  dl->PopClipRect();

  // Q-4: lane pointer mapping. Positions stay fractional (no tick rounding) so
  // the sweep and the line tool are exact at every zoom level.
  auto laneTickAt = [&](float x) { return static_cast<double>(x - gridMin.x + st.scrollX) / std::max(1e-6f, st.pxPerTick); };
  auto laneXOf = [&](double t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto laneFracAt = [&](float y) {
    float f = std::clamp((vMax.y - y) / std::max(1.0f, vMax.y - vMin.y), 0.0f, 1.0f);
    if (laneBipolar(lane) && std::fabs(f - 0.5f) < 0.03f) f = 0.5f;  // easy return to centre
    return f;
  };
  laneGeo = PianoLaneGeometry{gridMin.x - st.scrollX, st.pxPerTick, vMin.y, vMax.y, !autoLane, gridMin.y - st.scrollY, rowH, g.previewPitch};

  // ---------------- interaction ----------------
  const ImVec2 m = io.MousePos;
  const bool inGrid = m.x >= gridMin.x && m.x < gridMax.x && m.y >= gridMin.y && m.y < gridMax.y;
  const bool inKeys = m.x >= origin.x && m.x < gridMin.x && m.y >= gridMin.y && m.y < gridMax.y;
  const bool inRuler = m.y >= origin.y && m.y < gridMin.y && m.x >= gridMin.x;
  const bool inVel = m.y >= gridMax.y && m.x >= gridMin.x;
  const bool inLaneLabel = m.y >= gridMax.y && m.x >= origin.x && m.x < gridMin.x;
  // K-25: the other notes a bend edit applies to: a selected note's edit gives every selected
  // note the same line (a chord bends as one); an unselected note bends alone.
  auto bendTargets = [&](size_t ni) {
    std::vector<size_t> out;
    if (!st.selected.count(ni)) return out;
    for (size_t j : st.selected)
      if (j != ni && j < notes.size() && !notes[j].slide) out.push_back(j);
    return out;
  };

  if (hovered) {
    if (inGrid) {
      if (hoverNote >= 0 && st.tool == Tool::Draw)
        ImGui::SetMouseCursor(hoverEdge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
      char buf[96];
      std::snprintf(buf, sizeof buf, "%s  ·  bar %lld", noteName(pitchAt(m.y)).c_str(),
                    static_cast<long long>(tickAt(m.x) / std::max<Tick>(1, s.barTicks()) + 1));
      if (st.tool == Tool::Bend) {  // K-25
        if (bendHover.kind == BendHit::Point) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (bendHover.kind > BendHit::None) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (g.kind != Gesture::Bend && g.kind != Gesture::BendCurve) {
          // K-28: the snapping in force, named: the piano roll's snap for time, semitones for the
          // value; Alt inverts both (frees them, or with Snap off, snaps to the grid in view).
          char snapHint[160];
          if (st.snap != Snap::None)
            std::snprintf(snapHint, sizeof snapHint, i18n::tr("snap: %s and semitones, Alt: free"), snapName(st.snap));
          else
            std::snprintf(snapHint, sizeof snapHint, "%s", i18n::tr("no snap, Alt: the grid in view and semitones"));
          std::string h = std::string(buf) + "   (";
          if (bendHover.kind == BendHit::Point)
            h += i18n::tr("drag: move the point · right-click: curve menu · double-click or Alt+right-click: remove this point");
          else if (bendHover.kind == BendHit::Handle)
            h += i18n::tr("drag up / down: curve amount · right-click: curve menu · double-click: a straight ramp again");
          else if (bendHover.kind == BendHit::Line && bendHover.point >= 0)  // K-29
            h += i18n::tr("drag: a new point · double-click: this glide a straight ramp again · Ctrl+drag: select");
          else if (bendHover.kind == BendHit::Body)  // K-29
            h += i18n::tr("drag: bend the note · double-click: reset the note's bend · Ctrl+drag: select");
          else
            h += i18n::tr("drag on a note or its line: bend it · Ctrl+drag: select · a note under another note's line keeps its own pitch");
          a.setHint(h + " · " + snapHint + ")");
        }
      } else if (hoverNote >= 0 && st.tool == Tool::Draw)
        a.setHint(std::string(buf) + "   " + i18n::tr("(drag: move · edge: resize · Shift+drag: copy · Esc / right-click while dragging: cancel · Alt: no snap)"));
      else if (st.tool == Tool::Draw)  // K-20
        a.setHint(std::string(buf) + "   " + i18n::tr("(click: draw · Shift+click: draw and drag its length · wheel: scroll · Ctrl+wheel: zoom · Alt: no snap)"));
      else
        a.setHint(std::string(buf) + "   (wheel: scroll · Shift+wheel: horizontal · Ctrl+wheel: zoom · Alt: no snap)");
    } else if (inVel && !autoLane) {
      a.setHint(std::string(kLaneNames[static_cast<int>(lane)]) + ": " +
                i18n::tr("drag over notes to set it, right-drag to draw a line (selected notes only when there is a selection)"));
    } else if (inLaneLabel) {
      a.setHint("Choose the note property shown in the lane (wheel cycles)");
      if (io.MouseWheel != 0) {
        int k = (static_cast<int>(lane) + (io.MouseWheel < 0 ? 1 : static_cast<int>(Lane::Count) - 1)) % static_cast<int>(Lane::Count);
        lane = static_cast<Lane>(k);
      }
    } else if (inKeys) {
      a.setHint("Click keys to audition");
    } else if (inRuler) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      a.setHint("Click or drag to move the playback position (snaps to the grid, Alt = free)");
    }
  }

  bool lClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool rClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  static int propsNote = -1;
  // Q-3: Esc or a right-click during a note drag (move, Shift+drag copies,
  // resize) cancels it: notes back, copies gone, selection restored.
  if ((g.kind == Gesture::Move || g.kind == Gesture::Resize) && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      (keys::pressedHeld("piano.cancelDrag") || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    if (g.grouped) a.doc.cancelGroup();
    st.selected = g.selBefore;
    previewOff(a, chId);
    noteAnim.clear();
    g = Gesture{};
    lClick = rClick = false;
    a.setHint(i18n::tr("Drag cancelled"));
    notes = rollNotes(a, patId, chId);
  }

  if (lClick || rClick) {
    g = Gesture{};
    g.selBefore = st.selected;
    g.start = m;
    g.startTick = tickAt(m.x);
    g.startPitch = pitchAt(m.y);
    g.base = notes;
    g.sel = selVector(a, notes.size());
    g.key = 0x50520000000000ull ^ (gestureCounter++ << 8);
    g.anchor = hoverNote;
    if (inKeys && lClick) {
      g.kind = Gesture::Keys;  // K-21: a glissando while held: the key under the pointer sounds, one at a time
      previewOn(a, chId, pitchAt(m.y), 100);
    } else if (inRuler && lClick) {
      if (a.engine.mode() != PlayMode::Pattern) {  // J-49 (J-77: scrubbing here keeps the rack where it is)
        a.engine.setMode(PlayMode::Pattern);
        a.rackReveal.quietSwitch(PlayMode::Pattern);
      }
      g.kind = Gesture::Scrub;  // the position follows the pointer until release
    } else if (inLaneLabel && lClick) {
      ImGui::OpenPopup("##lanepop");
    } else if (inVel && (lClick || rClick)) {
      if (!autoLane) {  // automation lane: edited by drawPatternAutomationLane
        g.kind = Gesture::Velocity;
        ld = LaneDrag{};
        ld.line = rClick;  // Q-4: right drag = line tool (FL)
        ld.select = lClick && keys::modifierHeld("piano.laneSelectDrag");
        ld.reset = lClick && keys::modifierHeld("piano.laneDefaultDrag");
        ld.tickA = laneTickAt(m.x);
        ld.fracA = laneFracAt(m.y);
      }
    } else if (inGrid && rClick && st.tool == Tool::Zoom) {
      st.zoomRequest /= 1.5f;  // zoom tool: right-click zooms out
    } else if (inGrid && rClick && st.tool == Tool::Bend) {
      // K-25 / K-28: right-click a point or a curve handle: the curve menu of the segment that ends
      // at that point (Alt+right-click a point removes it at once). Never a note: this tool edits lines.
      if (bendHover.kind == BendHit::Point && io.KeyAlt) {
        std::vector<Note> ns = notes;
        const size_t ni = static_cast<size_t>(bendHover.note);
        bend::removePoint(ns[ni].bend, static_cast<size_t>(bendHover.point));
        for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
        commit(a, patId, chId, ns, selVector(a, ns.size()), "remove pitch bend point", 0);
      } else if (bendHover.kind == BendHit::Point || bendHover.kind == BendHit::Handle) {
        bendMenuNote = bendHover.note;
        bendMenuPoint = bendHover.point;
        ImGui::OpenPopup("##bendpoint");
      }
    } else if (inGrid && rClick) {
      g.kind = Gesture::Erase;
    } else if (inGrid && lClick) {
      bool ctrl = io.KeyCtrl;
      if (st.tool == Tool::Bend && !ctrl) {
        // K-25: the bend tool edits lines only (an empty spot does nothing; Ctrl+drag still selects).
        BendClick first = bendClick;
        const uint64_t now = a.doc.history().stateSerial();
        if (dbl && !(first.valid && first.pat == patId && first.ch == chId)) first = BendClick{true, bendHover, notes, patId, chId, 0, now};
        bendClick = BendClick{};
        if (!dbl) bendClick = BendClick{true, bendHover, notes, patId, chId, 0, now};
        if (dbl && first.hit.kind > BendHit::None) {
          // K-29: a double-click: on a point removes that point only, on a segment of the line or
          // its ring makes that glide a straight ramp again, on the note itself resets its line.
          BendHit hit = first.hit;
          if (first.serial != 0 && first.serial == a.doc.history().stateSerial()) a.doc.undo();  // the first click's drag
          const Pattern* cp = a.doc.song().pattern(patId);
          std::vector<Note> ns = cp && cp->notes.count(chId) ? cp->notes.at(chId) : std::vector<Note>{};
          if (ns != first.before) hit = bendHover;  // something else changed in between: what is under the pointer now
          const size_t ni = static_cast<size_t>(std::max(0, hit.note));
          if (hit.kind > BendHit::None && ni < ns.size()) {
            std::vector<BendPoint>& line = ns[ni].bend;
            const bool inLine = hit.point >= 0 && static_cast<size_t>(hit.point) < line.size();
            bool changed = false;
            const char* label = "reset pitch bend";
            if (hit.kind == BendHit::Point && inLine) {
              changed = bend::removePoint(line, static_cast<size_t>(hit.point));
              label = "remove pitch bend point";
            } else if ((hit.kind == BendHit::Handle || hit.kind == BendHit::Line) && inLine) {
              changed = bend::resetCurve(line, static_cast<size_t>(hit.point));
              label = "reset pitch bend curve";
              a.setHint(i18n::tr(changed ? "The glide is a straight ramp again" : "The glide already is a straight ramp"));
            } else if (hit.kind == BendHit::Body) {
              changed = !line.empty();
              line.clear();
            } else {
              a.setHint(i18n::tr("The line holds after its last point: there is no curve to reset"));
            }
            if (changed) {
              for (size_t j : bendTargets(ni)) ns[j].bend = line;
              commit(a, patId, chId, ns, selVector(a, ns.size()), label, 0);
            }
          }
        } else if (bendHover.kind > BendHit::None && !dbl) {
          const size_t ni = static_cast<size_t>(bendHover.note);
          if (io.KeyShift && bendHover.kind == BendHit::Point) {  // Shift+click a point: its segment's next curve
            std::vector<Note> ns = notes;
            BendPoint& bp = ns[ni].bend[static_cast<size_t>(bendHover.point)];
            size_t at = 0;
            const size_t count = sizeof kBendCurves / sizeof kBendCurves[0];
            while (at < count && kBendCurves[at].curve != bp.curve) ++at;
            bp.curve = kBendCurves[(at + 1) % count].curve;
            bp.tension = 0;
            for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
            commit(a, patId, chId, ns, selVector(a, ns.size()), "change pitch bend curve", 0);
            a.setHint(std::string(i18n::tr("Glide into the point:")) + " " + i18n::tr(bendCurveName(bp.curve)));
          } else if (bendHover.kind == BendHit::Handle) {  // K-28: drag a handle: the curve amount
            g.kind = Gesture::BendCurve;
            g.anchor = static_cast<int>(ni);
            g.bendPt = bendHover.point;
            g.bendV = notes[ni].bend[static_cast<size_t>(bendHover.point)].tension;
          } else {
            // Drag a point, or a new point at the pointer on the line (it moves only once dragged).
            g.kind = Gesture::Bend;
            g.anchor = static_cast<int>(ni);
            std::vector<BendPoint>& line = g.base[ni].bend;
            if (bendHover.kind == BendHit::Point) {
              g.bendPt = bendHover.point;
            } else {
              // K-28: a new point lands on the snap grid (whole ticks when free) and splits the
              // segment it falls in, both halves keeping its shape.
              const Note& n = notes[ni];
              double t = std::clamp(static_cast<double>(m.x - xOf(n.start)) / std::max(1e-6f, st.pxPerTick), 0.0, static_cast<double>(n.length));
              t = snapG > 1 ? static_cast<double>(snapNearest(n.start + static_cast<Tick>(std::lround(t)), snapG) - n.start) : std::round(t);
              t = std::clamp(t, 0.0, static_cast<double>(n.length));
              auto pos = std::upper_bound(line.begin(), line.end(), static_cast<float>(t), [](float x, const BendPoint& b) { return x < b.tick; });
              BendPoint np(static_cast<float>(t), static_cast<float>(bend::value(line, t)));
              if (pos != line.end() && pos->curve != bend::Step) np.curve = pos->curve, np.tension = pos->tension;
              g.bendPt = static_cast<int>(pos - line.begin());
              line.insert(pos, np);
            }
            const BendPoint& bp = line[static_cast<size_t>(g.bendPt)];
            g.bendT = bp.tick, g.bendV = bp.semis;
          }
        }
      } else if (!dbl && hoverNote < 0 && ghostHover.ch != kNoId && j31::editGhostAt(a, tickAt(m.x), ghostHover.pitch)) {
        // J-31 editable ghosts: the click switched to the ghost's channel with the note selected.
      } else if (dbl && hoverNote < 0 && ghostHover.ch != kNoId) {
        // Multi-channel editing: double-clicking a ghost note edits its channel.
        a.selectChannel(ghostHover.ch);
        a.notify("Editing " + std::string(s.channel(ghostHover.ch) ? s.channel(ghostHover.ch)->name : "channel"));
      } else if (dbl && hoverNote >= 0) {
        propsNote = hoverNote;
        ImGui::OpenPopup("##noteprops");
      } else if (ctrl || st.tool == Tool::Select) {
        g.kind = Gesture::Select;
        if (!io.KeyShift) st.selected.clear();
      } else if (st.tool == Tool::Erase) {
        g.kind = Gesture::Erase;
      } else if (st.tool == Tool::Slice) {
        g.kind = Gesture::Slice;
      } else if (st.tool == Tool::Paint) {
        g.kind = Gesture::Paint;
      } else if (st.tool == Tool::Mute) {
        g.kind = Gesture::Mute;
        if (hoverNote >= 0) g.muteTo = notes[static_cast<size_t>(hoverNote)].muted ? 0 : 1;
      } else if (st.tool == Tool::Play) {
        g.kind = Gesture::Play;
      } else if (st.tool == Tool::Zoom) {
        g.kind = Gesture::Zoom;
      } else if (st.tool == Tool::Detune) {
        if (hoverNote >= 0) {
          if (!st.selected.count(static_cast<size_t>(hoverNote))) {
            st.selected = {static_cast<size_t>(hoverNote)};
            g.sel = selVector(a, notes.size());
          }
          g.kind = Gesture::Detune;
        }
      } else {  // Draw
        if (hoverNote >= 0) {
          // Q-3: Shift+drag on a note copies the selection (or just that
          // note when it is not selected), like FL; the copies follow the pointer.
          const bool shiftCopy = io.KeyShift && !io.KeyCtrl && !hoverEdge;
          if (!st.selected.count(static_cast<size_t>(hoverNote))) {
            if (!io.KeyShift || shiftCopy) st.selected.clear();
            st.selected.insert(static_cast<size_t>(hoverNote));
            j31::expandNoteGroups(a);  // J-31: grouped notes come along
            g.sel = selVector(a, notes.size());
          }
          g.kind = hoverEdge ? Gesture::Resize : Gesture::Move;
          g.copyPending = shiftCopy;
          a.doc.beginGroup(shiftCopy ? "copy notes" : hoverEdge ? "resize notes" : "move notes");  // one undo step (empty: none)
          g.grouped = true;
          const Note& hn = notes[static_cast<size_t>(hoverNote)];
          previewOn(a, chId, hn.pitch, hn.velocity);
          st.lastLength = hn.length;
          st.lastVelocity = hn.velocity;
        } else {
          // Add a note and start moving it.
          Note n;
          n.start = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
          n.length = defLen;
          n.pitch = pitchAt(m.y);
          n.velocity = st.lastVelocity;
          n.color = newColor;
          n.slide = newSlide;
          std::vector<Note> ns = notes;
          std::vector<bool> sel(ns.size(), false);
          for (const Note& x : notesToAdd(st, n)) ns.push_back(x), sel.push_back(true);  // one note or a chord stamp
          if (st.snapToScale && st.scale > 0) n.pitch = tools::snapPitchToScale(n.pitch, st.scaleRoot, st.scale);
          commit(a, patId, chId, ns, sel, st.stampChord >= 0 ? "add chord" : "add note", g.key);
          if (st.stampOnce && (st.stampChord >= 0 || st.stampAuto > 0)) st.stampChord = -1, st.stampAuto = 0, st.stampOnce = false;
          notes = rollNotes(a, patId, chId);
          g.base = notes;
          g.sel = selVector(a, notes.size());
          g.anchor = static_cast<int>(*st.selected.begin());
          g.startTick = n.start;
          // K-20: Shift+click draws the note and the drag sets its length (FL-familiar); a click
          // without a drag keeps the default length.
          g.kind = io.KeyShift ? Gesture::Resize : Gesture::Move;
          g.drawResize = io.KeyShift;
          g.drawn = true;
          previewOn(a, chId, n.pitch, n.velocity);
        }
      }
    }
  }

  if (lClick || rClick) {  // J-37: a new drag starts its own glide
    noteAnim.clear();
    noteAnimT = NoteAnimTarget{patId, chId, {}};
    // J-188: whatever the setting (no lag while it is off), so switching it on mid-drag applies at once.
    if (g.kind == Gesture::Move || g.kind == Gesture::Resize) noteAnim.seed(0, {0, 0, 0}, ImGui::GetTime());
  }
  if (active && g.kind != Gesture::None) {
    // Dragging past the left/right edge scrolls the view.
    if (g.kind != Gesture::Select && g.kind != Gesture::Velocity && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
      const float edge = 24.0f;
      if (m.x > gridMax.x - edge) st.scrollX += (m.x - (gridMax.x - edge)) * 0.35f;
      if (m.x < gridMin.x + edge) st.scrollX = std::max(0.0f, st.scrollX - ((gridMin.x + edge) - m.x) * 0.35f);
    }
    const Tick curTick = tickAt(m.x);
    const int curPitch = pitchAt(m.y);
    switch (g.kind) {
      case Gesture::Keys:  // K-21: the previous key stops as the pointer reaches the next
        previewOn(a, chId, curPitch, 100);
        break;
      case Gesture::Scrub: {
        // Smooth: follows the pointer every frame, landing on the snap grid (Alt = free).
        Tick t = std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(std::clamp(m.x, gridMin.x, gridMax.x))), snapG, Snap::Line));
        if (t != g.lastScrub) {
          a.engine.setPosition(t);
          g.lastScrub = t;
        }
        break;
      }
      case Gesture::Move: {
        if (g.anchor < 0 || static_cast<size_t>(g.anchor) >= g.base.size()) break;
        if (!g.moved && !g.drawn && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;  // a click moves nothing
        const Note& an = g.base[static_cast<size_t>(g.anchor)];
        Tick earliest = an.start;
        for (size_t i = 0; i < g.base.size(); ++i)
          if (g.sel[i]) earliest = std::min(earliest, g.base[i].start);
        const Tick dt = timeline::moveDistance(curTick - g.startTick, snapG, st.snap, earliest);
        int dp = curPitch - g.startPitch;
        if (st.snapToScale && st.scale > 0) dp = tools::snapPitchToScale(an.pitch + dp, st.scaleRoot, st.scale) - an.pitch;
        // Keep the whole selection in range.
        for (size_t i = 0; i < g.base.size(); ++i)
          if (g.sel[i]) dp = std::clamp(dp, -g.base[i].pitch, 127 - g.base[i].pitch);
        noteAnimT.d = {static_cast<double>(dt), static_cast<double>(dp), 0.0};
        if (dt == 0 && dp == 0 && !g.moved) break;
        if (g.copyPending) {  // Q-3: the copies (every property kept, new groups) are what moves from here
          g.copyPending = false;
          const std::vector<int> map = tools::cloneNotes(g.base, g.sel);
          if (g.anchor >= 0 && static_cast<size_t>(g.anchor) < map.size() && map[static_cast<size_t>(g.anchor)] >= 0)
            g.anchor = map[static_cast<size_t>(g.anchor)];
        }
        g.moved = true;
        std::vector<Note> ns = g.base;
        for (size_t i = 0; i < ns.size(); ++i)
          if (g.sel[i]) {
            ns[i].start += dt;
            ns[i].pitch += dp;
          }
        commit(a, patId, chId, ns, g.sel, "move notes", g.key);
        previewOn(a, chId, an.pitch + dp, an.velocity);
        break;
      }
      case Gesture::Resize: {
        if (g.anchor < 0 || static_cast<size_t>(g.anchor) >= g.base.size()) break;
        if (!g.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) break;  // K-20: not dragged yet; a click changes nothing
        g.moved = true;
        const Note& an = g.base[static_cast<size_t>(g.anchor)];
        Tick end = timeline::snapTick(curTick, snapG, Snap::Line);
        Tick dl2 = std::max<Tick>(end, an.start + std::max<Tick>(1, snapG)) - (an.start + an.length);
        std::vector<Note> ns = g.base;
        for (size_t i = 0; i < ns.size(); ++i)
          if (g.sel[i]) ns[i].length = std::max<Tick>(1, ns[i].length + dl2);
        st.lastLength = std::max<Tick>(1, an.length + dl2);
        noteAnimT.d = {0.0, 0.0, static_cast<double>(dl2)};
        commit(a, patId, chId, ns, g.sel, "resize notes", g.key);
        break;
      }
      case Gesture::Select: {
        ImVec2 lo(std::min(g.start.x, m.x), std::min(g.start.y, m.y)), hi(std::max(g.start.x, m.x), std::max(g.start.y, m.y));
        std::set<size_t> sel;
        if (io.KeyShift)
          for (size_t i = 0; i < g.sel.size(); ++i)
            if (g.sel[i]) sel.insert(i);
        for (size_t i = 0; i < notes.size(); ++i) {
          float x0 = xOf(notes[i].start), x1 = xOf(notes[i].start + notes[i].length), y = yOf(notes[i].pitch);
          if (x1 >= lo.x && x0 <= hi.x && y + rowH >= lo.y && y <= hi.y) sel.insert(i);
        }
        st.selected = sel;
        break;
      }
      case Gesture::Paint: {
        Tick t = std::max<Tick>(0, snapDown(curTick, snapG));
        bool exists = false;
        for (const auto& n : notes) exists = exists || (n.pitch == curPitch && t < n.start + n.length && t + defLen > n.start);
        if (!exists) {
          std::vector<Note> ns = notes;
          Note pn;
          pn.start = t;
          pn.length = defLen;
          pn.pitch = curPitch;
          pn.velocity = st.lastVelocity;
          pn.color = newColor;
          pn.slide = newSlide;
          for (const Note& x : notesToAdd(st, pn)) ns.push_back(x);
          commit(a, patId, chId, ns, std::vector<bool>(ns.size(), false), "paint notes", g.key);
          previewOn(a, chId, curPitch, st.lastVelocity);
        }
        break;
      }
      case Gesture::Erase: {
        const Tick at = tickAt(m.x);
        const bool swept = hoverNote >= 0 && std::any_of(g.erased.begin(), g.erased.end(), [&](const Gesture::ErasedSpan& e) {
                             return e.pitch == notes[static_cast<size_t>(hoverNote)].pitch && at >= e.start && at < e.end;
                           });
        if (hoverNote >= 0 && !swept) {
          const Note& gone = notes[static_cast<size_t>(hoverNote)];
          // Up to the note's drawn end: a very short note is drawn wider than its length.
          g.erased.push_back({gone.pitch, std::min(at, gone.start), std::max({at + 1, gone.start + gone.length, tickAt(noteBoxes[static_cast<size_t>(hoverNote)].x1)})});
          std::vector<Note> ns = notes;
          auto sel = selVector(a, ns.size());
          ns.erase(ns.begin() + hoverNote);
          sel.erase(sel.begin() + hoverNote);
          commit(a, patId, chId, ns, sel, "delete notes", g.key);
        }
        break;
      }
      case Gesture::Mute: {  // mute tool: every touched note gets the first note's new state
        if (hoverNote < 0) break;
        if (g.muteTo < 0) g.muteTo = notes[static_cast<size_t>(hoverNote)].muted ? 0 : 1;
        if (notes[static_cast<size_t>(hoverNote)].muted == (g.muteTo == 1)) break;
        std::vector<Note> ns = notes;
        ns[static_cast<size_t>(hoverNote)].muted = g.muteTo == 1;
        commit(a, patId, chId, ns, selVector(a, ns.size()), g.muteTo ? "mute notes" : "unmute notes", g.key);
        break;
      }
      case Gesture::Play: {  // play tool: the notes under the pointer sound while held
        std::vector<int> now;
        for (size_t i : tools::notesAt(notes, std::max<Tick>(0, tickAt(m.x))))
          if (!notes[i].muted) now.push_back(notes[i].pitch);
        for (int p : g.playing)
          if (std::find(now.begin(), now.end(), p) == now.end()) a.engine.noteOff(chId, p);
        for (int p : now)
          if (std::find(g.playing.begin(), g.playing.end(), p) == g.playing.end()) a.engine.noteOn(chId, p, 0.8f);
        g.playing = now;
        a.setHint("Playing " + std::to_string(now.size()) + " note(s)");
        break;
      }
      case Gesture::Zoom: break;  // drawn below, applied on release
      case Gesture::Bend: {  // K-25: the point follows the pointer (the snap grid and semitone steps; Alt inverts)
        if (g.anchor < 0 || static_cast<size_t>(g.anchor) >= g.base.size() || g.bendPt < 0) break;
        if (!g.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) break;  // a click alone changes nothing
        g.moved = true;
        const size_t ni = static_cast<size_t>(g.anchor);
        const Note& bn = g.base[ni];
        const auto& line = bn.bend;
        const size_t k = static_cast<size_t>(g.bendPt);
        if (k >= line.size()) break;
        // K-28: snapping follows the piano roll's: time on its snap grid (snapG: the grid in view
        // for Line, the fixed size otherwise), the value in semitones; Alt inverts both, as for
        // notes (free with a snap set; the grid in view and semitones with Snap off).
        const bool snapOn = (st.snap != Snap::None) != io.KeyAlt;
        const double lo = k > 0 ? line[k - 1].tick : 0.0;
        const double hi = k + 1 < line.size() ? line[k + 1].tick : static_cast<double>(bn.length);
        double t = static_cast<double>(m.x - xOf(bn.start)) / std::max(1e-6f, st.pxPerTick);
        t = snapG > 1 ? static_cast<double>(snapNearest(bn.start + static_cast<Tick>(std::lround(t)), snapG) - bn.start) : std::round(t);
        t = std::clamp(t, lo, std::max(lo, hi));
        double v = static_cast<double>(yOf(bn.pitch) + rowH * 0.5f - m.y) / std::max(1.0f, rowH);
        v = snapOn ? std::round(v) : std::round(v * 100.0) / 100.0;
        v = std::clamp(v, std::max(-static_cast<double>(bend::kMaxSemis), -static_cast<double>(bn.pitch)),
                       std::min(static_cast<double>(bend::kMaxSemis), 127.0 - bn.pitch));
        g.bendT = t, g.bendV = v;
        std::vector<Note> ns = g.base;
        BendPoint& bp = ns[ni].bend[k];
        bp.tick = static_cast<float>(t);
        bp.semis = static_cast<float>(v);
        bend::normalize(ns[ni].bend);
        for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
        if (ns != notes) commit(a, patId, chId, ns, g.sel, "bend notes", g.key);
        a.setHint(bendText(bn.pitch, v) + "   " +
                  (st.snap != Snap::None ? i18n::tr("(Alt: free · right-click a point: curve menu · double-click a point: remove it)")
                                         : i18n::tr("(Alt: the grid in view and semitones · right-click a point: curve menu · double-click a point: remove it)")));
        break;
      }
      case Gesture::BendCurve: {  // K-28: one segment's curve amount follows the vertical drag (the curve follows the pointer)
        if (g.anchor < 0 || static_cast<size_t>(g.anchor) >= g.base.size() || g.bendPt < 0) break;
        if (!g.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) break;
        g.moved = true;
        const size_t ni = static_cast<size_t>(g.anchor), k = static_cast<size_t>(g.bendPt);
        const auto& line = g.base[ni].bend;
        if (k >= line.size()) break;
        const double rise = line[k].semis >= (k > 0 ? line[k - 1].semis : 0.0f) ? 1.0 : -1.0;
        const double up = static_cast<double>(g.start.y - m.y) / 120.0;  // 120 px: the whole range, as in automation clips
        // Up raises the curve's middle: a straight segment or an ease in bends towards a fast
        // start, an ease out gets stronger, a smooth one steeper.
        const uint8_t c = line[k].curve;
        const double d = c == bend::Smooth ? up : c == bend::EaseOut ? up * rise : -up * rise;
        const float ten = static_cast<float>(std::clamp(std::round((g.bendV + d) * 100.0) / 100.0, -1.0, 1.0));
        std::vector<Note> ns = g.base;
        ns[ni].bend[k].tension = ten;
        for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
        if (ns != notes) commit(a, patId, chId, ns, g.sel, "bend curve", g.key);
        char b[128];
        std::snprintf(b, sizeof b, i18n::tr("%s, curve amount %+.0f%%"), i18n::tr(bendCurveName(c)), static_cast<double>(ten) * 100.0);
        a.setHint(std::string(b) + "   " + i18n::tr("(double-click the ring: a straight ramp again)"));
        break;
      }
      case Gesture::Detune: {  // fine pitch of the selection follows the vertical drag
        const float dy = g.start.y - m.y;
        const int cents = static_cast<int>(std::lround(dy * (io.KeyShift ? 1.0f : 5.0f)));
        std::vector<Note> ns = g.base;
        for (size_t i = 0; i < ns.size(); ++i)
          if (g.sel[i]) ns[i].fine = std::clamp(g.base[i].fine + cents, -kNoteFineMax, kNoteFineMax);
        commit(a, patId, chId, ns, g.sel, "detune notes", g.key);
        if (g.anchor >= 0 && static_cast<size_t>(g.anchor) < ns.size())
          a.setHint("Fine pitch " + std::to_string(ns[static_cast<size_t>(g.anchor)].fine) + " cents (Shift: finer)");
        break;
      }
      case Gesture::Velocity: {  // property lane (velocity, pan, release, fine pitch, mod X / Y)
        const float frac = laneFracAt(m.y);
        const double t = laneTickAt(m.x);
        const bool anySel = std::find(g.sel.begin(), g.sel.end(), true) != g.sel.end();
        const std::vector<bool> only = anySel ? g.sel : std::vector<bool>{};  // FL: selected notes only
        const auto prop = static_cast<tools::NoteProp>(lane);
        Note shown;
        setLaneValue(shown, lane, frac);
        const char* label = lane == Lane::Velocity ? "change velocity" : "change note property";
        if (ld.select) {
          // Ctrl + drag: select the notes starting inside the dragged time span.
          std::set<size_t> sel;
          for (size_t i : notelane::notesBetween(notes, {}, ld.tickA, t)) sel.insert(i);
          st.selected = sel;
          a.setHint(std::to_string(sel.size()) + " " + i18n::tr("notes selected"));
        } else if (ld.line) {
          // Line tool: the notes between A and the pointer follow the line live
          // (from the gesture's start state), so release leaves exactly the line.
          const double slack = kLaneLineSlackPx / std::max(1e-6f, st.pxPerTick);
          std::vector<Note> ns = g.base;
          notelane::applyLine(ns, only, prop, ld.tickA, ld.fracA, t, frac, laneSetFn, slack);
          if (ns != notes) commit(a, patId, chId, ns, g.sel, label, g.key);
          Note from;
          setLaneValue(from, lane, ld.fracA);
          a.setHint(laneText(from, lane) + "  \xE2\x86\x92  " + laneText(shown, lane));
        } else {
          // Paint: sweep the segment from the previous pointer position to this
          // one; every position edits its nearest stem (FL), so nothing is skipped.
          std::vector<Note> ns = notes;
          const double t0 = ld.hasPrev ? ld.prevTick : t;
          const float f0 = ld.hasPrev ? ld.prevFrac : frac;
          bool changed = false;
          if (ld.reset) {  // each note back to its property's default
            for (size_t i : notelane::nearestInSweep(ns, only, t0, t)) {
              const Note before = ns[i];
              setLaneValue(ns[i], lane, static_cast<float>(tools::notePropNorm(Note{}, prop)));
              changed = changed || !(before == ns[i]);
            }
            setLaneValue(shown, lane, static_cast<float>(tools::notePropNorm(Note{}, prop)));
          } else {
            changed = notelane::paintSegment(ns, only, prop, t0, f0, t, frac, laneSetFn);
          }
          if (changed) commit(a, patId, chId, ns, g.sel, label, g.key);
          ld.hasPrev = true;
          ld.prevTick = t;
          ld.prevFrac = frac;
          a.setHint(laneText(shown, lane));
        }
        break;
      }
      default: break;
    }
  }
  if (active && g.kind == Gesture::Velocity && ld.select) {  // Q-4: Ctrl + drag selection span
    const float xa = laneXOf(ld.tickA);
    dl->AddRectFilled(ImVec2(std::min(xa, m.x), vMin.y), ImVec2(std::max(xa, m.x), vMax.y), theme::withAlpha(P.hot, 0.15f));
    dl->AddRect(ImVec2(std::min(xa, m.x), vMin.y), ImVec2(std::max(xa, m.x), vMax.y), theme::withAlpha(P.hot, 0.6f));
  }
  if (active && g.kind == Gesture::Velocity && ld.line) {  // Q-4: line tool preview, anchored to the start tick
    const float span = vMax.y - vMin.y;
    const ImVec2 pa(laneXOf(ld.tickA), vMax.y - span * ld.fracA), pb(m.x, vMax.y - span * laneFracAt(m.y));
    const ImU32 lc = theme::litRole(theme::role::kGraph);
    dl->PushClipRect(ImVec2(vMin.x, vMin.y - 4), ImVec2(vMax.x, vMax.y + 4), true);
    dl->AddLine(pa, pb, theme::withAlpha(P.ink, 0.6f), 3.0f);
    dl->AddLine(pa, pb, lc, 1.5f);
    dl->AddCircleFilled(pa, 3.0f, lc, 10);
    dl->AddCircleFilled(pb, 3.0f, lc, 10);
    dl->PopClipRect();
  }
  if (bendTool) {  // K-25: the value of the point being dragged or hovered, in a pill beside it
    int ni = -1, curveOf = -1;  // curveOf (K-28): the curve handle shown (point index), -1 = a point's value
    double bt = 0, bv = 0;
    if (g.kind == Gesture::Bend && active && g.moved) ni = g.anchor, bt = g.bendT, bv = g.bendV;
    else if (g.kind == Gesture::BendCurve && active) ni = g.anchor, curveOf = g.bendPt;
    else if (bendHover.kind == BendHit::Handle && g.kind == Gesture::None) ni = bendHover.note, curveOf = bendHover.point;
    else if (bendHover.kind == BendHit::Point && g.kind == Gesture::None) {
      ni = bendHover.note;
      const BendPoint& bp = notes[static_cast<size_t>(ni)].bend[static_cast<size_t>(bendHover.point)];
      bt = bp.tick, bv = bp.semis;
    }
    if (ni >= 0 && static_cast<size_t>(ni) < notes.size() &&
        (curveOf < 0 || static_cast<size_t>(curveOf) < notes[static_cast<size_t>(ni)].bend.size())) {
      const Note& n = notes[static_cast<size_t>(ni)];
      ImVec2 q(xOf(n.start) + static_cast<float>(bt) * st.pxPerTick, yOf(n.pitch) + rowH * 0.5f - static_cast<float>(bv) * rowH);
      std::string tip = bendText(n.pitch, bv);
      if (curveOf >= 0) {  // K-28: the segment's shape and curve amount, beside its handle
        const BendPoint& bp = n.bend[static_cast<size_t>(curveOf)];
        if (!bendHandle(n, static_cast<size_t>(curveOf), xOf(n.start), yOf(n.pitch) + rowH * 0.5f, st.pxPerTick, rowH, q))
          q = ImVec2(xOf(n.start) + bp.tick * st.pxPerTick, yOf(n.pitch) + rowH * 0.5f - bp.semis * rowH);
        char b[96];
        std::snprintf(b, sizeof b, "%s %+.0f%%", i18n::tr(bendCurveName(bp.curve)), static_cast<double>(bp.tension) * 100.0);
        tip = b;
      }
      const float fs = theme::kFontLabel;
      const ImVec2 ts = a.fontMono->CalcTextSizeA(fs, 1e9f, 0, tip.c_str());
      const float w = std::round(ts.x + 12), h = std::round(fs + 8);
      float x = std::round(q.x + 10), y = std::round(q.y - h - 8);
      if (x + w > gridMax.x - 4) x = std::round(q.x - 10 - w);
      if (y < gridMin.y + 4) y = std::round(q.y + 10);
      dl->PushClipRect(gridMin, gridMax, true);
      dl->AddRectFilled(ImVec2(x + 1, y + 2), ImVec2(x + w + 1, y + h + 2), theme::withAlpha(P.ink, 0.25f), theme::kRadius);  // soft drop
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), theme::popupBg(P), theme::kRadius);
      dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), P.lineStrong, theme::kRadius);
      dl->AddText(a.fontMono, fs, ImVec2(x + 6, y + 4), P.text, tip.c_str());
      dl->PopClipRect();
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (g.kind == Gesture::Play)
      for (int p : g.playing) a.engine.noteOff(chId, p);
    if (g.kind == Gesture::Zoom) {
      if (std::fabs(m.x - g.start.x) < 4) {
        st.zoomRequest *= 1.5f;  // click: zoom in at the pointer
      } else {
        Tick t0 = std::max<Tick>(0, tickAt(std::min(g.start.x, m.x))), t1 = tickAt(std::max(g.start.x, m.x));
        if (t1 > t0) {
          st.pxPerTick = static_cast<float>(timeline::clampZoom(s, gridW / static_cast<double>(t1 - t0)));
          st.scrollX = static_cast<float>(t0) * st.pxPerTick;
        }
      }
    }
    if (g.kind == Gesture::Slice) {
      Tick cut = std::max<Tick>(0, snapNearest(tickAt(m.x), snapG));
      int p0 = pitchAt(g.start.y), p1 = pitchAt(m.y);
      if (p0 > p1) std::swap(p0, p1);
      std::vector<Note> ns;
      bool changed = false;
      for (const auto& n : notes) {
        if (n.pitch >= p0 && n.pitch <= p1 && cut > n.start && cut < n.start + n.length) {
          Note left = n, right = n;
          left.length = cut - n.start;
          right.start = cut;
          right.length = n.start + n.length - cut;
          if (!n.bend.empty()) bend::split(n, cut - n.start, left, right);  // K-25: each part keeps its piece of the line
          ns.push_back(left);
          ns.push_back(right);
          changed = true;
        } else {
          ns.push_back(n);
        }
      }
      if (changed) commit(a, patId, chId, ns, std::vector<bool>(ns.size(), false), "slice notes", 0);
    }
    if (g.kind == Gesture::Move && !g.moved && g.anchor >= 0 && !io.KeyShift) {
      st.selected = {static_cast<size_t>(std::min<int>(g.anchor, static_cast<int>(notes.size()) - 1))};
    }
    if (g.grouped) a.doc.endGroup();
    a.doc.endGesture();
    // K-29: the history state a bend drag left (a double-click whose first click moved undoes it)
    if ((g.kind == Gesture::Bend || g.kind == Gesture::BendCurve) && g.moved && bendClick.valid && a.doc.history().stateSerial() != bendClick.press)
      bendClick.serial = a.doc.history().stateSerial();
    previewOff(a, chId);
    g = Gesture{};
  }
  if (!active && g.previewPitch >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) previewOff(a, chId);

  if (ImGui::BeginPopup("##lanepop")) {
    for (int k = 0; k < static_cast<int>(Lane::Count); ++k)
      if (ImGui::Selectable(kLaneNames[k], !autoLane && static_cast<int>(lane) == k)) lane = static_cast<Lane>(k), pianoAutomationLane() = ParamId{};
    patternLaneMenu(a, patId, chId);
    ImGui::EndPopup();
  }

  // K-28: a bend point's menu (right-click a point or its curve handle): the shape of the glide
  // into the point, each with a drawing of it, its curve amount, removing the point. The line on
  // the canvas follows every change as it is made. Menu choices are one undo step each, a drag of
  // the curve amount one step.
  if (ImGui::BeginPopup("##bendpoint")) {
    const std::vector<Note> curNotes = rollNotes(a, patId, chId);
    const std::vector<Note>* cns = curNotes.empty() ? nullptr : &curNotes;
    const bool ok = cns && bendMenuNote >= 0 && static_cast<size_t>(bendMenuNote) < cns->size() && bendMenuPoint >= 0 &&
                    static_cast<size_t>(bendMenuPoint) < (*cns)[static_cast<size_t>(bendMenuNote)].bend.size();
    if (!ok) {
      ImGui::CloseCurrentPopup();
    } else {
      const size_t ni = static_cast<size_t>(bendMenuNote), k = static_cast<size_t>(bendMenuPoint);
      const Note& n = (*cns)[ni];
      const BendPoint bp = n.bend[k];
      auto edit = [&](const char* label, uint64_t key, auto fn) {
        std::vector<Note> ns = *cns;
        fn(ns[ni].bend[k]);
        bend::normalize(ns[ni].bend);
        for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
        commit(a, patId, chId, ns, selVector(a, ns.size()), label, key);
      };
      ImGui::PushFont(a.fontMono, 0.0f);
      ImGui::TextDisabled("%s", bendText(n.pitch, bp.semis).c_str());
      ImGui::PopFont();
      ImGui::Separator();
      ImGui::TextDisabled("%s", i18n::tr("Glide into this point"));
      float labelW = 0;
      for (const auto& c : kBendCurves) labelW = std::max(labelW, ImGui::CalcTextSize(i18n::tr(c.name)).x);
      int id = 0;
      for (const auto& c : kBendCurves) {
        const bool on = c.curve == bp.curve;
        char sid[16];
        std::snprintf(sid, sizeof sid, "##curve%d", id++);
        if (curveMenuItem(sid, i18n::tr(c.name), c.curve, on ? bp.tension : 0.0f, on, labelW) && !on)
          edit("pitch bend curve", 0, [&](BendPoint& p) { p.curve = c.curve, p.tension = 0; });
      }
      const bool hold = std::fabs(bp.semis - (k > 0 ? n.bend[k - 1].semis : 0.0f)) < 1e-4f;
      ImGui::BeginDisabled(bp.curve == bend::Step || hold);
      float pct = bp.tension * 100.0f;
      // A drag field (no slider grab over the text): drag sideways, or Ctrl+click to type.
      ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, std::round(ImGui::GetFontSize() * 2.9f + labelW + 4.0f)));
      if (bendMenuKey == 0) bendMenuKey = 0x4B28000000000000ull ^ (gestureCounter++ << 8);
      if (ImGui::DragFloat("##bendtension", &pct, 1.0f, -100.0f, 100.0f, i18n::tr("Curve amount %+.0f%%"), ImGuiSliderFlags_AlwaysClamp))
        edit("bend curve", bendMenuKey, [&](BendPoint& p) { p.tension = std::clamp(std::round(pct) / 100.0f, -1.0f, 1.0f); });
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        a.doc.endGesture();
        bendMenuKey = 0;
      }
      ui::Hint(bp.curve == bend::Step ? "A step holds, then jumps: it has no curve" : hold ? "The line holds here: there is no glide to shape"
                                                                                          : "Bends the glide (drag the ring on the line the same way)");
      ImGui::EndDisabled();
      if (ImGui::MenuItem(i18n::tr("Reset curve amount"), nullptr, false, bp.tension != 0.0f))
        edit("reset pitch bend curve", 0, [](BendPoint& p) { p.tension = 0; });
      // K-29: the whole curve back to a straight ramp (what a double-click on the glide does)
      if (ImGui::MenuItem(i18n::tr("Reset the curve (straight)"), i18n::tr("double-click the glide"), false, bp.curve != bend::Straight || bp.tension != 0.0f))
        edit("reset pitch bend curve", 0, [](BendPoint& p) { p.curve = bend::Straight, p.tension = 0; });
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Remove point"), i18n::tr("double-click"))) {
        std::vector<Note> ns = *cns;
        bend::removePoint(ns[ni].bend, k);
        for (size_t j : bendTargets(ni)) ns[j].bend = ns[ni].bend;
        commit(a, patId, chId, ns, selVector(a, ns.size()), "remove pitch bend point", 0);
      }
      if (ImGui::MenuItem(i18n::tr("Reset the note's bend"), i18n::tr("double-click the note"))) {
        std::vector<Note> ns = *cns;
        ns[ni].bend.clear();
        for (size_t j : bendTargets(ni)) ns[j].bend.clear();
        commit(a, patId, chId, ns, selVector(a, ns.size()), "reset pitch bend", 0);
      }
    }
    ImGui::EndPopup();
  } else if (bendMenuNote >= 0) {
    bendMenuNote = bendMenuPoint = -1;
    if (bendMenuKey) a.doc.endGesture();
    bendMenuKey = 0;
  }

  // Note properties popup.
  if (ImGui::BeginPopup("##noteprops")) {
    const std::vector<Note> curNotes = rollNotes(a, patId, chId);  // K-22: step notes too
    if (propsNote >= 0 && static_cast<size_t>(propsNote) < curNotes.size()) {
      Note n = curNotes[static_cast<size_t>(propsNote)];
      Note orig = n;
      ImGui::PushFont(a.fontBold, 0.0f);
      ImGui::Text(i18n::tr("Note %s"), noteName(n.pitch).c_str());
      ImGui::PopFont();
      int pitch = n.pitch, vel = n.velocity;
      long long start = n.start, len = n.length;
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Pitch"), &pitch, 0, 127, noteName(pitch).c_str());
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Velocity"), &vel, 1, 127);
      ImGui::SetNextItemWidth(160);
      ImGui::InputScalar("Start (ticks)", ImGuiDataType_S64, &start);
      ImGui::SetNextItemWidth(160);
      ImGui::InputScalar("Length (ticks)", ImGuiDataType_S64, &len);
      int pan = n.pan, fineP = n.fine, rel = n.release, mx = n.modX, my = n.modY, colorG = n.color;
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Pan"), &pan, -kNotePanMax, kNotePanMax, pan == 0 ? "centre" : "%d");
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Fine pitch (cents)"), &fineP, -kNoteFineMax, kNoteFineMax);
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Release velocity"), &rel, 0, 127);
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Mod X"), &mx, 0, kNoteModMax);
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Mod Y"), &my, 0, kNoteModMax);
      ImGui::SetNextItemWidth(160);
      ImGui::SliderInt(i18n::tr("Colour group"), &colorG, 0, kNoteColors - 1);
      bool slide = n.slide, porta = n.porta;
      ImGui::Checkbox(i18n::tr("Slide"), &slide);
      ui::Hint("Glides the channel's sounding notes to this pitch over the note's length instead of starting a note");
      ImGui::SameLine();
      ImGui::Checkbox(i18n::tr("Porta"), &porta);
      ui::Hint("Starts at the previous note's pitch and glides to its own (channel porta time)");
      n.pitch = pitch;
      n.velocity = vel;
      n.start = std::max<long long>(0, start);
      n.length = std::max<long long>(1, len);
      n.pan = pan;
      n.fine = fineP;
      n.release = rel;
      n.modX = mx;
      n.modY = my;
      n.color = colorG;
      n.slide = slide;
      n.porta = porta;
      if (!(n == orig)) {
        std::vector<Note> ns = curNotes;
        std::vector<bool> sel(ns.size(), false);
        ns[static_cast<size_t>(propsNote)] = n;
        sel[static_cast<size_t>(propsNote)] = true;
        commit(a, patId, chId, ns, sel, "edit note properties", 0x4E50ull << 48);
        propsNote = static_cast<int>(*a.piano.selected.begin());
      }
    } else {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else if (propsNote >= 0) {
    propsNote = -1;
    a.doc.endGesture();
  }

  // K-22: while the channel's rack steps show here as notes, a quiet chip in the grid's top right
  // corner says so; the first edit moves them into the piano roll (one undo step).
  if (rackSteps && gridW > 220 && gridH > 60) {
    const char* label = i18n::tr("Rack steps");
    const float fs = theme::kFontLabel;
    const ImVec2 ts = a.fontMono->CalcTextSizeA(fs, 1e9f, 0, label);
    const float cell = std::round(fs * 0.5f), gap = std::max(2.0f, std::round(fs * 0.18f));
    const float glyphW = cell * 3 + gap * 2;
    const float h = std::round(fs + 10), w = std::round(glyphW + ts.x + 26);
    const float x = std::round(gridMax.x - w - 10), y = std::round(gridMin.y + 8);
    const bool over = hovered && io.MousePos.x >= x && io.MousePos.x < x + w && io.MousePos.y >= y && io.MousePos.y < y + h;
    dl->PushClipRect(gridMin, gridMax, true);
    dl->AddRectFilled(ImVec2(x + 1, y + 2), ImVec2(x + w + 1, y + h + 2), theme::withAlpha(P.ink, 0.22f), theme::kRadius);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), theme::withAlpha(theme::popupBg(P), over ? 1.0f : 0.92f), theme::kRadius);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), over ? P.lineStrong : P.line, theme::kRadius);
    const ImU32 lit = theme::mix(noteCol, P.paper, P.dark ? 0.15f : 0.0f);
    for (int k = 0; k < 3; ++k) {  // three rack step buttons, the middle one off
      const float gx = x + 9 + static_cast<float>(k) * (cell + gap), gy = std::round(y + (h - cell) * 0.5f);
      if (k == 1) dl->AddRect(ImVec2(gx, gy), ImVec2(gx + cell, gy + cell), theme::withAlpha(P.textDim, 0.8f), 1.5f);
      else dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + cell, gy + cell), lit, 1.5f);
    }
    dl->AddText(a.fontMono, fs, ImVec2(std::round(x + 9 + glyphW + 8), std::round(y + (h - ts.y) * 0.5f)), over ? P.text : P.textDim, label);
    dl->PopClipRect();
    if (over) a.setHint(i18n::tr("The channel's rack steps show as notes: moving, resizing or editing one turns them into piano roll notes (one undo step)"));
  }

  // ---------------- keyboard shortcuts (window focused) ----------------
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    std::vector<Note> ns = notes;
    auto sel = selVector(a, ns.size());
    bool anySel = !st.selected.empty();
    auto applyToSel = [&](auto fn, const char* label) {
      if (!anySel) return;
      for (size_t i = 0; i < ns.size(); ++i)
        if (sel[i]) fn(ns[i]);
      commit(a, patId, chId, ns, sel, label, 0);
    };
    if (keys::pressed("piano.delete")) {
      if (anySel) {
        std::vector<Note> keep;
        for (size_t i = 0; i < ns.size(); ++i)
          if (!sel[i]) keep.push_back(ns[i]);
        commit(a, patId, chId, keep, {}, "delete notes", 0);
      }
    }
    if (keys::pressed("piano.selectAll")) {
      st.selected.clear();
      for (size_t i = 0; i < ns.size(); ++i) st.selected.insert(i);
    }
    if (keys::pressed("piano.deselect")) st.selected.clear();
    if (keys::pressed("piano.copy") && anySel) {
      st.clipboard.clear();
      for (size_t i = 0; i < ns.size(); ++i)
        if (sel[i]) st.clipboard.push_back(ns[i]);
      a.notify(std::to_string(st.clipboard.size()) + " notes copied");
    }
    if (keys::pressed("piano.cut") && anySel) {
      st.clipboard.clear();
      std::vector<Note> keep;
      for (size_t i = 0; i < ns.size(); ++i) (sel[i] ? st.clipboard : keep).push_back(ns[i]);
      commit(a, patId, chId, keep, {}, "cut notes", 0);
    }
    if (keys::pressed("piano.paste") && !st.clipboard.empty()) {
      // Paste after the end of the copied block.
      Tick lo = st.clipboard[0].start, hi = 0;
      for (const auto& n : st.clipboard) lo = std::min(lo, n.start), hi = std::max(hi, n.start + n.length);
      Tick shift = snapNearest(hi - lo, std::max<Tick>(grid, 1));
      std::vector<bool> nsel(ns.size(), false);
      for (auto n : st.clipboard) {
        n.start += shift;
        ns.push_back(n);
        nsel.push_back(true);
      }
      for (auto& n : st.clipboard) n.start += shift;  // repeated pastes keep going
      commit(a, patId, chId, ns, nsel, "paste notes", 0);
    }
    if (keys::pressed("piano.quickQuantize")) {
      for (size_t i = 0; i < ns.size(); ++i)
        if (!anySel || sel[i]) ns[i].start = snapNearest(ns[i].start, grid);
      commit(a, patId, chId, ns, sel, "quantize notes", 0);
    }
    if (keys::pressed("piano.transposeUp")) applyToSel([&](Note& n) { n.pitch += 1; }, "transpose notes");
    if (keys::pressed("piano.transposeDown")) applyToSel([&](Note& n) { n.pitch -= 1; }, "transpose notes");
    if (keys::pressed("piano.octaveUp")) applyToSel([&](Note& n) { n.pitch += 12; }, "transpose notes");
    if (keys::pressed("piano.octaveDown")) applyToSel([&](Note& n) { n.pitch -= 12; }, "transpose notes");
    if (keys::pressed("piano.moveRight")) applyToSel([&](Note& n) { n.start += grid; }, "move notes");
    if (keys::pressed("piano.moveLeft")) applyToSel([&](Note& n) { n.start = std::max<Tick>(0, n.start - grid); }, "move notes");
    if (!a.typingKeyboard) {
      if (keys::pressed("piano.toolDraw")) st.tool = Tool::Draw;
      if (keys::pressed("piano.toolPaint")) st.tool = Tool::Paint;
      if (keys::pressed("piano.toolDelete")) st.tool = Tool::Erase;
      if (keys::pressed("piano.toolSelect")) st.tool = Tool::Select;
      if (keys::pressed("piano.toolSlice")) st.tool = Tool::Slice;
      if (keys::pressed("piano.toolMute")) st.tool = Tool::Mute;
      if (keys::pressed("piano.toolPlay")) st.tool = Tool::Play;
      if (keys::pressed("piano.toolZoom")) st.tool = Tool::Zoom;
      if (keys::pressed("piano.toolDetune")) st.tool = Tool::Detune;
      if (keys::pressed("piano.toolBend")) st.tool = Tool::Bend;  // K-25
    }
  }
  ui::EndDockWindow();
}

}  // namespace gl
