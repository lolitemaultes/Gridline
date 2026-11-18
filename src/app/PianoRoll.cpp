// Gridline — piano roll: notes of (selected pattern, selected channel).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "../core/MenuOpsExtra.h"
#include "PixelGrid.h"
#include "Viewports.h"
#include "../core/I18n.h"
#include "CursorAnchor.h"
#include "Keys.h"
#include "KeyLight.h"
#include "J31Ui.h"
#include "MeterUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "LogicPlugins.h"
#include "ScrollBarUi.h"
#include "SmoothScrollUi.h"
#include "AutomationEditor.h"
#include "EditTools.h"
#include "ScriptUi.h"
#include "../core/Grouping.h"
#include "../core/SongTools.h"
#include "../core/MenuParity.h"
#include "../core/MeterEdit.h"
#include "../core/NoteBend.h"
#include "../core/NoteExprEdit.h"
#include "../core/NoteLaneEdit.h"
#include "../core/NoteTools.h"
#include "../core/SmoothSnap.h"
#include "../core/StepNotes.h"
#include "../core/TrackerGrid.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "TrackerPages.h"
#include "LiveLook.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"
#include "ChordStampUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"

namespace gl {

// GitHub #70: the play position inside the pattern shown (pattern ticks, -1: not playing there):
// in pattern mode while it is the pattern playing, in song mode wherever a clip of it plays.
static double rollPlayPos(App& a, ObjectId pat) {
  const PlayMode m = a.engine.mode();
  if (m == PlayMode::Live) return -1;
  return trk::patternPlayPos(a.doc.song(), pat, m == PlayMode::Song, a.engine.pattern(), a.engine.position());
}

using theme::pal;

namespace {

// K-24: the key rows' height range and one Alt+wheel notch's factor.
constexpr float kRowHMin = 7.0f, kRowHMax = 28.0f;

// K-112D (D3, G5): a zoom slider as the reference's local bar has (a glyph, then a thin track with a
// round thumb): t (0..1) is where the thumb is; returns the new position while dragged or clicked, else -1.
constexpr float kZoomSliderW = 54.0f;
float zoomSlider(const char* id, bool horizontal, float t, float h, const char* hint) {
  const auto& P = theme::pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float gw = 16.0f, w = gw + kZoomSliderW;
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = std::round(p.y + h * 0.5f), gx = p.x + 7;
  const ImU32 gc = hov || act ? P.text : P.textDim;
  if (horizontal) {  // a two-headed arrow across (144)
    dl->AddLine(ImVec2(gx - 4, cy + 0.5f), ImVec2(gx + 4, cy + 0.5f), gc, 1.2f);
    dl->AddTriangleFilled(ImVec2(gx - 7, cy + 0.5f), ImVec2(gx - 3, cy - 3), ImVec2(gx - 3, cy + 4), gc);
    dl->AddTriangleFilled(ImVec2(gx + 7, cy + 0.5f), ImVec2(gx + 3, cy - 3), ImVec2(gx + 3, cy + 4), gc);
  } else {  // a two-headed arrow up and down
    dl->AddLine(ImVec2(gx + 0.5f, cy - 4), ImVec2(gx + 0.5f, cy + 4), gc, 1.2f);
    dl->AddTriangleFilled(ImVec2(gx + 0.5f, cy - 7), ImVec2(gx - 3, cy - 3), ImVec2(gx + 4, cy - 3), gc);
    dl->AddTriangleFilled(ImVec2(gx + 0.5f, cy + 7), ImVec2(gx - 3, cy + 3), ImVec2(gx + 4, cy + 3), gc);
  }
  const float x0 = p.x + gw + 4, x1 = p.x + w - 4;
  const float tc = std::clamp(t, 0.0f, 1.0f), tx = std::round(x0 + (x1 - x0) * tc);
  dl->AddRectFilled(ImVec2(x0, cy - 1), ImVec2(x1, cy + 1), theme::mix(P.surface950, P.text, 0.18f), 1.0f);
  dl->AddRectFilled(ImVec2(x0, cy - 1), ImVec2(tx, cy + 1), theme::withAlpha(P.text, 0.55f), 1.0f);
  dl->AddCircleFilled(ImVec2(tx, cy + 0.5f), 5.5f, theme::withAlpha(P.ink, 0.35f), 16);
  dl->AddCircleFilled(ImVec2(tx, cy), 5.0f, act ? theme::mix(P.text, P.accent, 0.3f) : hov ? P.paper : P.text, 16);
  if (hov) ui::Hint(hint);
  if (act && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) || ImGui::IsItemActivated()))
    return std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
  return -1.0f;
}
constexpr double kRowZoomStep = 1.1;

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int pitch) {
  int k = pitch % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
// K-81: how opaque a piano roll note body is (dark and light themes).
float noteBodyAlpha(bool dark) { return dark ? 0.84f : 0.82f; }

// K-106 cp22: the roll's drawing style. The Tracker's Roll page draws square notes (no rounded
// corners, no cap, no top light) and plain velocity bars (a filled bar the note's length, no
// lollipop head), as the classic screens; every other layout keeps the default (all false).
struct RollStyle {
  bool squareNotes = false;
  bool plainVelocity = false;
};
RollStyle rollStyle() {
  RollStyle r;
  if (layoutui::active() == layout::Preset::Tracker) {
    r.squareNotes = true;
    r.plainVelocity = true;
  }
  return r;
}
std::string noteName(int pitch) { return std::string(kNoteNames[pitch % 12]) + std::to_string(pitch / 12 - 1); }
// K-112D (audit G5): the pitch and position under the pointer in the last frame, for Smooth's read-out
// in the local bar (-1: the pointer is not over the notes).
int gReadPitch = -1;
Tick gReadTick = 0;

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
  // K-198 follow-up: a drag of a selection spanning the edited channels (every channel moves together).
  bool multi = false;
  multinote::Lists mbase;     // the selection's channels' lists at the press
  multinote::Selection msel;  // the selection at the press (target included)
  multinote::Selection editSelBefore;
  int anchorPitch = 60, anchorVel = 100;  // the pressed note (preview, scale snap)
  Tick anchorStart = 0, anchorLen = 1;
  bool multiCopy = false;            // Shift+drag: copies move, the originals stay
  ObjectId anchorCh = kNoId;         // an edited channel's note pressed (not the target's)
  size_t anchorIdx = 0;
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
// K-171: the lane splitter being dragged (press y, lane height at the press).
struct LaneSplit {
  bool on = false;
  float y0 = 0, h0 = 0;
} laneSplit;
PianoLaneGeometry laneGeo;  // dev check: last drawn lane geometry
// The view scrollers (time above the ruler, keys left of the keyboard), held
// across frames, and the key row held at the opposite edge while an end of the
// vertical one zooms.
scrollbarui::Drag hBarDrag, vBarDrag;
smoothzoom::RowAnchor vBarRow;

// J-37: smooth snapping of a note drag. Every selected note moves by the same
// snapped delta (start ticks, pitch rows, length ticks); the drawing eases
// that delta, the document keeps the exact one.
smooth::Tracker<3> noteAnim;
struct NoteAnimTarget {
  ObjectId pat = kNoId, ch = kNoId;
  smooth::Tracker<3>::Vec d{};
} noteAnimT;

// ---- note properties: the property lane, new-note colour and slide mode
// K-99: Pressure and Timbre are the notes' expression lanes (MPE): a line over each note's length.
enum class Lane { Velocity, Pan, Release, Fine, ModX, ModY, Pressure, Timbre, Count };
const char* const kLaneNames[] = {"Velocity", "Pan", "Release velocity", "Fine pitch", "Mod X", "Mod Y", "Note pressure", "Note timbre"};
const char* const kLaneShort[] = {"VEL", "PAN", "REL", "FINE", "MOD X", "MOD Y", "PRESS", "TIMBRE"};
Lane lane = Lane::Velocity;
int newColor = 0;       // colour group of new notes
bool newSlide = false;  // new notes are slide notes

bool laneBipolar(Lane l) { return l == Lane::Pan || l == Lane::Fine || l == Lane::Timbre; }
bool exprLane(Lane l) { return l == Lane::Pressure || l == Lane::Timbre; }
ExprDim laneDim(Lane l) { return l == Lane::Timbre ? ExprDim::Timbre : ExprDim::Pressure; }
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

// K-198 follow-up: the selection over the target channel (`selected`) and the channels edited
// beside it (`editSel`). Edits of it are one undo step over every channel touched.
bool multiSel(const App& a) { return !a.piano.editSel.empty(); }
multinote::Selection unionSel(const App& a, ObjectId ch) {
  multinote::Selection s = a.piano.editSel;
  if (!a.piano.selected.empty()) s[ch] = a.piano.selected;
  return s;
}
void splitSel(App& a, ObjectId ch, multinote::Selection s) {
  auto it = s.find(ch);
  a.piano.selected = it != s.end() ? it->second : std::set<size_t>{};
  s.erase(ch);
  multinote::prune(s);
  a.piano.editSel = std::move(s);
}
void multiApply(App& a, ObjectId pat, ObjectId ch, const std::function<void(Note&)>& fn, const char* label) {
  const multinote::Selection sel = unionSel(a, ch);
  const multinote::Lists base = multinote::lists(a.doc.song(), pat, sel);
  a.doc.beginGroup(label);
  multinote::Selection out = multinote::transform(a.doc, pat, base, sel, fn, false, label);
  a.doc.endGroup();
  splitSel(a, ch, std::move(out));
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

// K-112 (Live): the editor's slim toolbar. Left: the menu, the tool (one dropdown, the keys as
// before), the channel, Fold, the scale and its highlighting; the middle: what the lower lane shows
// (Notes: velocity, Envelopes: the pattern's automation, MPE: each note's own expression and the
// pitch bend tool); right: More (follow, quantize, ghost notes, slide, note colour, meter, zoom)
// and the grid. Nothing the full toolbar offers is lost; Studio keeps the full toolbar.
static void liveToolbar(App& a, const Pattern*& pat, const Channel*& chan, const MeterMap& pm) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  PianoRollState& st = a.piano;
  const float fh = ImGui::GetFrameHeight();
  const float x0 = ImGui::GetCursorScreenPos().x, W = ImGui::GetContentRegionAvail().x;
  {
    const uint64_t rev = a.doc.revision();
    menuButton("prmenu", "Piano roll menu: tools, selection, chords, snap, view", [&a]() { pianoRollMenu(a); });
    if (a.doc.revision() != rev) {
      pat = s.pattern(a.curPattern);
      chan = s.channel(a.selChannel);
    }
  }
  // The tool: its icon lit in its colour, a chevron; the list names each tool and its key.
  struct T { ui::Icon icon; Tool tool; const char* name; const char* key; uint32_t role; };
  static const T tools[] = {{ui::Icon::ToolDraw, Tool::Draw, i18n::N_("Draw"), "piano.toolDraw", theme::role::kDraw},
                            {ui::Icon::ToolBend, Tool::Bend, i18n::N_("Pitch bend"), "piano.toolBend", theme::role::kGraph},
                            {ui::Icon::ToolPaint, Tool::Paint, i18n::N_("Paint"), "piano.toolPaint", theme::role::kPaint},
                            {ui::Icon::ToolDelete, Tool::Erase, i18n::N_("Delete"), "piano.toolDelete", theme::role::kDelete},
                            {ui::Icon::ToolSelect, Tool::Select, i18n::N_("Select"), "piano.toolSelect", theme::role::kSelect},
                            {ui::Icon::ToolSlice, Tool::Slice, i18n::N_("Slice"), "piano.toolSlice", theme::role::kSlice},
                            {ui::Icon::ToolMute, Tool::Mute, i18n::N_("Mute"), "piano.toolMute", theme::role::kMute},
                            {ui::Icon::ToolPlay, Tool::Play, i18n::N_("Playback"), "piano.toolPlay", theme::role::kPlay},
                            {ui::Icon::ToolZoom, Tool::Zoom, i18n::N_("Zoom"), "piano.toolZoom", theme::role::kZoom},
                            {ui::Icon::ToolDetune, Tool::Detune, i18n::N_("Detune"), "piano.toolDetune", theme::role::kSlip}};
  const T* cur = &tools[0];
  for (const T& t : tools)
    if (t.tool == st.tool) cur = &t;
  ImGui::SameLine(0, 4);
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float bw = fh + 14;
    const bool open = ImGui::IsPopupOpen("##livetools");
    if (ImGui::InvisibleButton("##livetool", ImVec2(bw, fh))) ImGui::OpenPopup("##livetools");
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + fh), open || hov ? P.surface700 : P.field, theme::kRadiusSm);
    ui::drawIconFit(dl, cur->icon, ImVec2(p.x + 4, p.y + 3), ImVec2(p.x + fh - 4, p.y + fh - 3), theme::rgb(cur->role));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + bw - 7, p.y + fh * 0.5f), 9, P.textDim);
    ui::Hint(ui::toolTip(cur->name, "the tool in use: click for the others (their keys work as before)", cur->key).c_str());
    ui::noteActiveTool("piano", static_cast<int>(cur->tool), cur->name, cur->key);
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2));
    if (ImGui::BeginPopup("##livetools")) {
      for (const T& t : tools)
        if (ImGui::MenuItem(i18n::tr(t.name), keys::label(t.key), st.tool == t.tool)) st.tool = t.tool;
      ImGui::EndPopup();
    }
  }
  // The channel whose notes are edited.
  ImGui::SameLine(0, 4);
  // Checkpoint 58 (N57-15): as wide as the device's name needs ("1 Analog Synth" read "1 Analog Syn..."
  // with room to spare), within 90 px and a fifth of the bar.
  const char* chanName = chan ? chan->name.c_str() : i18n::tr("No channel");
  const float chanW = std::clamp(std::ceil(ImGui::CalcTextSize(chanName).x) + fh + 16.0f, 90.0f, std::max(140.0f, std::floor(W * 0.2f)));
  if (ui::BeginCombo("##chanlive", chanName, chanW)) {
    for (const auto& c : s.channels)
      if (ImGui::Selectable((c.name + "##" + std::to_string(c.id)).c_str(), c.id == a.selChannel)) a.selectChannel(c.id);
    ImGui::EndCombo();
  }
  ui::Hint("Channel being edited");
  ui::ToolbarDivider();
  if (ui::TextButton("Fold", st.fold, 0, "Fold: only the rows that hold notes (click again for every key)")) {
    st.fold = !st.fold;
    st.scrollY = 0;
  }
  ImGui::SameLine(0, 4);
  {
    static const char* const keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const auto& sc = tools::scales();
    const std::string sl = st.scale > 0 ? std::string(keys[st.scaleRoot]) + " " + sc[static_cast<size_t>(st.scale)].name : i18n::tr("Scale");
    if (ui::BeginCombo("##scalelivepr", sl.c_str(), std::clamp(W * 0.1f, 80.0f, 120.0f))) {
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
    ui::Hint("The clip's scale: its key and scale (rows outside it can be shaded, drawn notes can snap to it)");
  }
  const float leftEnd = ImGui::GetItemRectMax().x;
  // Right: More, then the grid.
  // K-112c (round 24): the pop-out button at the far right (the clip band shows no tab bar).
  const bool pop = viewports::poppable();
  const float gridW = std::clamp(W * 0.1f, 84.0f, 120.0f), moreW = fh, popW = pop ? fh + 4 : 0.0f;
  const float rightX = x0 + W - gridW - moreW - 4 - popW;
  // Middle: the lane tabs, centred in the toolbar when they fit, else after the left group.
  static const char* const tabs[] = {i18n::N_("Notes"), i18n::N_("Envelopes"), i18n::N_("MPE")};
  const bool autoLane = pianoAutomationLane().owner != kNoId;
  int tab = autoLane ? 1 : lane != Lane::Velocity || st.tool == Tool::Bend ? 2 : 0;
  float segW = 76.0f;
  // Highlight scale sits after the scale when the row has room for it and the tabs.
  const float hlW = ImGui::CalcTextSize(i18n::tr("Highlight scale")).x + fh + 8;
  const bool hlHere = rightX - (leftEnd + 8 + hlW) >= segW * 3 + 4 + 16;
  float after = leftEnd;
  if (hlHere) {
    ImGui::SameLine(0, 8);
    ImGui::Checkbox(i18n::tr("Highlight scale"), &st.highlightScale);
    ui::Hint("Shade the rows outside the scale (the scale stays chosen either way)");
    after = ImGui::GetItemRectMax().x;
  }
  const float room = rightX - after - 16;
  segW = std::clamp(room / 3.0f - 2, 56.0f, 76.0f);
  const float tabsW = segW * 3 + 4;
  float tx = std::round(x0 + (W - tabsW) * 0.5f);
  tx = std::clamp(tx, after + 8, std::max(after + 8, rightX - 8 - tabsW));
  if (tx + tabsW <= rightX - 4) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(tx, ImGui::GetCursorScreenPos().y));
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ui::SetNextSegmentedTabs();  // K-112c (round 30): a tab strip, as the refs' clip view (058, 034, 417)
    if (ui::Segmented("##lanetabs", tabs, 3, &tab, segW)) {
      if (tab == 0) {
        lane = Lane::Velocity;
        pianoAutomationLane() = ParamId{};
        if (st.tool == Tool::Bend) st.tool = Tool::Draw;
      } else if (tab == 1) {
        ImGui::OpenPopup("##envlive");
      } else {
        ImGui::OpenPopup("##mpelive");
      }
    }
    ui::Hint("The lower lane: Notes (velocity), Envelopes (the pattern's automation), MPE (each note's own expression)");
    ImGui::SetNextWindowPos(ImVec2(tp.x + segW, tp.y + fh + 2));
    if (ImGui::BeginPopup("##envlive")) {
      if (pat && chan) patternLaneMenu(a, pat->id, chan->id);
      ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(ImVec2(tp.x + segW * 2, tp.y + fh + 2));
    if (ImGui::BeginPopup("##mpelive")) {
      for (int k = 1; k < static_cast<int>(Lane::Count); ++k)
        if (ImGui::MenuItem(i18n::tr(kLaneNames[k]), nullptr, !autoLane && static_cast<int>(lane) == k)) lane = static_cast<Lane>(k), pianoAutomationLane() = ParamId{};
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Pitch bend tool"), keys::label("piano.toolBend"), st.tool == Tool::Bend)) st.tool = Tool::Bend;
      ImGui::EndPopup();
    }
  }
  // More: the rest of the full toolbar.
  ImGui::SameLine(0, 0);
  ImGui::SetCursorScreenPos(ImVec2(rightX, ImGui::GetCursorScreenPos().y));
  if (ui::IconButton("prmorelive", ui::Icon::More, false, fh, "More: follow, quantize, ghost notes, slide, note colour, meter, zoom"))
    ImGui::OpenPopup("##morelive");
  if (ImGui::BeginPopup("##morelive")) {
    if (!hlHere) ImGui::MenuItem(i18n::tr("Highlight scale"), nullptr, &st.highlightScale);
    const bool follow = a.settings.getBool("ui.autoScrollPattern");
    if (ImGui::MenuItem(i18n::tr("Follow the pattern position"), nullptr, follow)) a.settings.set("ui.autoScrollPattern", !follow);
    if (ImGui::MenuItem(i18n::tr("Quantize"), keys::shownText("Ctrl+Q"), false, pat && chan)) {
      if (std::vector<Note> ns = rollNotes(a, pat->id, chan->id); !ns.empty()) {
        const Tick gsz = st.effGrid > 1 ? st.effGrid : snapTicks(pm, st.snap);
        auto sel = selVector(a, ns.size());
        const bool any = !st.selected.empty();
        for (size_t i = 0; i < ns.size(); ++i)
          if (!any || sel[i]) ns[i].start = snapNearest(ns[i].start, gsz);
        commit(a, pat->id, chan->id, ns, sel, "quantize notes", 0);
      }
    }
    if (ImGui::MenuItem(i18n::tr("Ghost notes"), nullptr, st.ghostNotes)) st.ghostNotes = !st.ghostNotes;
    if (ImGui::MenuItem(i18n::tr("Slide notes"), nullptr, newSlide)) newSlide = !newSlide;
    if (ImGui::BeginMenu(i18n::tr("Note colour"))) {
      const ImU32 chanCol = chan ? theme::rgb(chan->color) : P.textDim;
      for (int k = 0; k < kNoteColors; ++k) {
        if (k % 8) ImGui::SameLine(0, 4);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##c", ImVec2(20, 20))) {
          newColor = k;
          if (pat && chan && !st.selected.empty())
            if (std::vector<Note> ns = rollNotes(a, pat->id, chan->id); !ns.empty()) {
              auto sel = selVector(a, ns.size());
              for (size_t i = 0; i < ns.size(); ++i)
                if (sel[i]) ns[i].color = k;
              commit(a, pat->id, chan->id, ns, sel, "colour notes", 0);
            }
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        pdl->AddRectFilled(q, ImVec2(q.x + 20, q.y + 20), groupColor(k, chanCol), 3);
        if (k == newColor) pdl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q.x + 21, q.y + 21), P.accent, 3.0f, 2.0f, 0);
      }
      ImGui::EndMenu();
    }
    if (pat && ImGui::BeginMenu(i18n::tr("Meter"))) {
      meterui::patternMeterMenu(a, pat->id);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Zoom in"), "PgUp")) st.zoomRequest *= 1.5f;
    if (ImGui::MenuItem(i18n::tr("Zoom out"), "PgDn")) st.zoomRequest /= 1.5f;
    ImGui::EndPopup();
  }
  // The grid: the snap and its value.
  ImGui::SameLine(0, 4);
  std::string snapLabel = snapName(st.snap);
  if ((st.snap == Snap::Line || st.snap == Snap::Cell) && st.effGrid > 0) {
    // As a note value (1/16) when it is one, as the refs show the grid; else its name (bar, step...).
    const Tick whole = static_cast<Tick>(s.ppq) * 4;
    snapLabel = st.effGrid <= whole && whole % st.effGrid == 0 ? "1/" + std::to_string(whole / st.effGrid) : timeline::gridName(pm, st.effGrid);
  }
  if (ui::BeginCombo("##snaplive", snapLabel.c_str(), gridW)) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::Selectable(snapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) st.snap = static_cast<Snap>(i);
    ImGui::EndCombo();
  }
  ui::Hint("The grid: notes snap to it (Line and Cell follow the zoom); hold Alt while dragging to bypass");
  if (pop) {
    ImGui::SameLine(0, 4);
    const bool docked = ImGui::IsWindowDocked();
    if (ui::IconButton("prpopout", docked ? ui::Icon::WindowPopout : ui::Icon::WindowDockin, false, fh,
                       docked ? "Pop out: the piano roll in a window of its own" : "Dock back: the piano roll into the clip band"))
      viewports::togglePopOut("Piano roll");
  }
}

void drawPianoRoll(App& a) {
  drawEditTools(a);  // J-11 tool windows (quantize, chop, strum, …)
  drawScriptTool(a);  // J-24 piano roll script window
  // K-141: a drag whose window closed or hid before the button came up keeps its undo step open
  // (and undo waiting): close it once the button is up.
  if (g.grouped && !ImGui::IsAnyMouseDown() && !ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    a.doc.endGroup();
    a.doc.endGesture();
    g = Gesture{};
  }
  if (!a.showPiano) return;
  if (!ui::BeginDockWindow("Piano roll", &a.showPiano, ImVec2(900, 520)) || trackerpages::enteringFrame()) {  // K-106: not docked yet
    ui::EndDockWindow();
    return;
  }
  // K-112 (Live): an audio clip selected: the band shows that clip (its sample), not the notes.
  if (livelook::active(a))
    if (const ObjectId ac = livelook::focusedAudioClip(a); ac != kNoId) {
      drawLiveAudioClip(a, ac);
      ui::EndDockWindow();
      return;
    }
  // C44-26: in Clips the clip band shows the selected clip only; with none selected it is empty.
  if (livelook::active(a) && a.playlist.selected.empty()) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), i18n::tr("No clip selected"),
                   i18n::tr("Select a clip in the clip grid or the arrangement to edit it here"));
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 60.0f)));
    ui::EndDockWindow();
    return;
  }
  // K-112c: the Live layout's clip column on the left; the piano roll indents itself past it and
  // stays one window (no child), so its focus, hover, popups and shortcuts are as everywhere else.
  if (trackerpages::active(a)) {  // K-106: the Tracker's Roll page has the pages' title bar (pattern and channel as the tracker numbers them)
    const Song& ts = a.doc.song();
    const Channel* tc = ts.channel(a.selChannel);
    char info[160];
    std::snprintf(info, sizeof info, "%s %02X  %s %02X %s", i18n::tr("Pattern"), trk::patternNumber(ts, a.curPattern), i18n::tr("Instrument"),
                  ts.channelIndex(a.selChannel) + 1, tc ? tc->name.c_str() : "");
    trackerpages::pageTitle(a, i18n::tr("Roll"), info);
  }
  const float clipColW = livelook::active(a) ? livelook::clipColumn(a) : 0.0f;
  if (clipColW > 0) ImGui::Indent(clipColW);
  const auto& P = pal();
  const Song& s = a.doc.song();
  PianoRollState& st = a.piano;
  const Pattern* pat = s.pattern(a.curPattern);
  // K-179: pattern time follows the pattern's own meter (grid, bars, ruler, snap).
  const MeterMap pm = MeterMap::forPattern(s, pat);
  const Channel* chan = s.channel(a.selChannel);
  // J-159: an instrument that names its keys (a deck setup's performance keys) shows the names
  const PluginInfo* chanInfo = chan ? findPlugin(chan->plugin.type) : nullptr;
  auto keyLabel = [&](int p) -> const std::string* {
    if (!chanInfo || p < 0 || p >= static_cast<int>(chanInfo->keyNames.size())) return nullptr;
    const std::string& n = chanInfo->keyNames[static_cast<size_t>(p)];
    return n.empty() ? nullptr : &n;
  };
  if (st.selPattern != a.curPattern || st.selChannel != a.selChannel) {
    // K-198 follow-up: a new target from the picker keeps the selection over the edited channels.
    std::set<size_t> took;
    if (st.selPattern != a.curPattern) {
      st.editSel.clear();
    } else {
      if (auto it = st.editSel.find(a.selChannel); it != st.editSel.end()) took = it->second, st.editSel.erase(it);
      if (st.editChannels.count(st.selChannel) && !st.selected.empty()) st.editSel[st.selChannel] = st.selected;
    }
    st.selected = took;
    st.selPattern = a.curPattern;
    st.selChannel = a.selChannel;
  }
  for (auto it = st.editSel.begin(); it != st.editSel.end();)  // only channels still edited here
    it = st.editChannels.count(it->first) && it->first != a.selChannel && !it->second.empty() ? std::next(it) : st.editSel.erase(it);
  j31::pianoKey(a, st.scaleRoot, st.scale);  // J-31: scale highlighting follows key markers

  // ---------------- toolbar ----------------
  // K-112c (round 24): Live's toolbar is 20 px with 18 px controls, as the refs' clip view strip.
  const bool liveBar = livelook::active(a);
  // K-106 cp16: the Tracker's Roll page has a row of bevel buttons in the band's look instead (the
  // tools, the snap, the instrument, zoom; its Menu holds everything else the toolbar offers).
  const bool trackerBar = trackerpages::active(a);
  if (trackerBar) {
    trackerpages::rollToolbar(a);
    pat = s.pattern(a.curPattern);
    chan = s.channel(a.selChannel);
  } else {
  if (liveBar) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, std::max(0.0f, std::floor((18.0f - ImGui::GetFontSize()) * 0.5f))));
    ui::BeginToolbar("##prtb", 20.0f);
  } else {
    ui::BeginToolbar("##prtb");
  }
  if (liveBar) liveToolbar(a, pat, chan, pm);  // K-112: the slim toolbar
  else {
    const float fh = ImGui::GetFrameHeight();
    const float W = ImGui::GetContentRegionAvail().x;
    {
      // Some items are whole-song edits (events into a playlist automation clip replace the patterns):
      // look the pattern and channel up again rather than read the old ones.
      const uint64_t rev = a.doc.revision();
      // K-112b (checkpoint 24): the Tracks layout heads the piano roll with the same three text menus as
      // the tracks area (Edit, Functions, View), as 144, 152 and 127 do; the other layouts keep the menu button.
      const bool textMenus = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
      if (textMenus) {
        static const char* const names[] = {i18n::N_("Edit"), i18n::N_("Functions"), i18n::N_("View")};
        static const char* const hints[] = {i18n::N_("Edit and select notes"), i18n::N_("Import and export, note tools, chords, scripts and the stamp"),
                                            i18n::N_("Tool, snap, zoom and what the piano roll shows")};
        ui::MenuGroup("##prmenus", names, hints, 3, [&a](int k) { pianoRollMenu(a, k); });
      } else {
        menuButton("prmenu", "Piano roll menu: tools, selection, chords, snap, view", [&a]() { pianoRollMenu(a); });
      }
      if (a.doc.revision() != rev) {
        pat = s.pattern(a.curPattern);
        chan = s.channel(a.selChannel);
      }
    }
    ImGui::SameLine(0, layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic) ? 8.0f : 2.0f);  // D8: Smooth's bar: 8 between groups
    stampui::button(a, W >= 760.0f);  // the chord stamp, beside the menu (its chord's name shows on wide windows)
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
                          chanW - 36 - (st.editChannels.empty() ? 0.0f : 24.0f), P.text, chan ? chan->name.c_str() : "No channel");
      if (!st.editChannels.empty()) {  // K-198: how many channels are edited beside the target
        const std::string more = "+" + std::to_string(st.editChannels.size());
        tdl->AddText(ImVec2(p.x + chanW - 22 - ImGui::CalcTextSize(more.c_str()).x, std::round(p.y + (fh - theme::kFontBody) * 0.5f)), P.hot, more.c_str());
      }
      ui::drawIcon(tdl, ui::Icon::ChevronDown, ImVec2(p.x + chanW - 12, p.y + fh * 0.5f), 11, P.textDim);
      ui::Hint(st.editChannels.empty() ? "Target channel: the channel being edited (tick others in the list to edit them here too)"
                                       : "Target channel: new notes go here; the other ticked channels are edited here too (click one of their notes to make it the target)");
      ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2));
      if (ImGui::BeginPopup("##chanpop")) {
        // K-198: the row picks the target; the box at its end edits that channel here too.
        const float rowW = std::max(200.0f, chanW + 40);
        for (const auto& c : s.channels) {
          ImGui::PushID(static_cast<int>(c.id));
          ImVec2 q = ImGui::GetCursorScreenPos();
          float lh = ImGui::GetTextLineHeight();
          const bool target = c.id == a.selChannel;
          bool edited = target || st.editChannels.count(c.id) > 0;
          if (ImGui::Selectable("##c", target, ImGuiSelectableFlags_AllowOverlap, ImVec2(rowW - lh - 8, 0))) {
            if (!st.editChannels.empty()) ops::retarget(st.editChannels, a.selChannel, c.id);
            a.selectChannel(c.id);
          }
          ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + 6, q.y + lh - 2), theme::rgb(c.color), 2);
          ImGui::GetWindowDrawList()->AddText(ImVec2(q.x + 14, q.y), target ? P.hot : P.text, c.name.c_str());
          ImGui::SameLine(rowW - lh);
          ImGui::BeginDisabled(target);
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
          if (ImGui::Checkbox("##edit", &edited)) {
            if (edited) st.editChannels.insert(c.id);
            else st.editChannels.erase(c.id);
          }
          ImGui::PopStyleVar();
          ImGui::EndDisabled();
          ui::Hint(target ? "The target channel (new notes go here)" : "Edit this channel's notes here too");
          ImGui::PopID();
        }
        ImGui::Separator();
        if (pat && ImGui::MenuItem(i18n::tr("Edit every channel with notes here"))) {
          const std::vector<ObjectId> with = ops::channelsWithNotes(s, *pat, a.selChannel);
          st.editChannels = std::set<ObjectId>(with.begin(), with.end());
        }
        if (ImGui::MenuItem(i18n::tr("Edit the target channel only"), nullptr, false, !st.editChannels.empty())) st.editChannels.clear();
        ImGui::EndPopup();
      }
    }
    ui::ToolbarDivider();
    // Q-12: the shared tool family; tooltips "Name (key): what it does" from the keymap.
    struct T { ui::Icon icon; Tool tool; const char* name; const char* desc; const char* key; uint32_t role; };
    const T tools[] = {{ui::Icon::ToolDraw, Tool::Draw, i18n::N_("Draw"), i18n::N_("click to add, Shift+click and drag to set the new note's length, drag to move, drag the end to resize, right-click deletes"), "piano.toolDraw", theme::role::kDraw},
                       {ui::Icon::ToolBend, Tool::Bend, i18n::N_("Pitch bend"), i18n::N_("drag anywhere on a note to bend its pitch like an automation line (semitone steps, Alt: free); right-click a point: its curve menu, Shift+click: its next curve; double-click a point to remove it, a curve to straighten it, the note to reset its line"), "piano.toolBend", theme::role::kGraph},
                       {ui::Icon::ToolPaint, Tool::Paint, i18n::N_("Paint"), i18n::N_("paint notes while dragging"), "piano.toolPaint", theme::role::kPaint},
                       {ui::Icon::ToolDelete, Tool::Erase, i18n::N_("Delete"), i18n::N_("click or drag over notes to delete them"), "piano.toolDelete", theme::role::kDelete},
                       {ui::Icon::ToolSelect, Tool::Select, i18n::N_("Select"), i18n::N_("drag a box; Ctrl+drag selects with any tool"), "piano.toolSelect", theme::role::kSelect},
                       {ui::Icon::ToolSlice, Tool::Slice, i18n::N_("Slice"), i18n::N_("drag across notes to cut them"), "piano.toolSlice", theme::role::kSlice},
                       {ui::Icon::ToolMute, Tool::Mute, i18n::N_("Mute"), i18n::N_("click or drag over notes to mute / unmute them"), "piano.toolMute", theme::role::kMute},
                       {ui::Icon::ToolPlay, Tool::Play, i18n::N_("Playback"), i18n::N_("hold and move to hear the notes under the pointer"), "piano.toolPlay", theme::role::kPlay},
                       {ui::Icon::ToolZoom, Tool::Zoom, i18n::N_("Zoom"), i18n::N_("drag a range to fill the view, click zooms in, right-click zooms out"), "piano.toolZoom", theme::role::kZoom},
                       {ui::Icon::ToolDetune, Tool::Detune, i18n::N_("Detune"), i18n::N_("drag a note up / down to change its fine pitch (Shift: finer)"), "piano.toolDetune", theme::role::kSlip}};
    // K-112b: the Logic layout's calmer bar: one tool button (the tool in use, a click lists them
    // all) instead of the row, and only snap, quantise and zoom after it; the rest stay in the menu.
    const bool calm = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
    // K-112D (D3, G5): in Smooth the tool comes after the view switches (lane, catch), as 144's local bar
    // runs: menus, view switches, the tool, the position, Snap, then the zoom sliders. Its glyph is drawn
    // in the text colour (R09-09: the Studio layout's per-tool colours are gone from Smooth).
    auto calmTool = [&]() {
      size_t cur = 0;
      for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i)
        if (st.tool == tools[i].tool) cur = i;
      const T& t = tools[cur];
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float bw = fh + 16;
      ImGui::InvisibleButton("##prtoolmenu", ImVec2(bw, fh));
      const bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##prtoolpop");
      if (ImGui::IsItemClicked()) ImGui::OpenPopup("##prtoolpop");
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      tdl->AddRectFilled(p, ImVec2(p.x + bw, p.y + fh), open || hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, theme::kRadius);
      ui::drawIcon(tdl, t.icon, ImVec2(p.x + fh * 0.5f + 1, p.y + fh * 0.5f), fh - 10, P.text);
      ui::drawIcon(tdl, ui::Icon::ChevronDown, ImVec2(p.x + bw - 9, p.y + fh * 0.5f), 10, P.textDim);
      const char* k = keys::label(t.key);
      ui::Hint(std::string(i18n::tr(t.name)) + (k ? std::string(" (") + k + ")" : std::string()) + ": " + i18n::tr(t.desc) + "  " + i18n::tr("(click: all tools)"));
      ui::noteActiveTool("piano", static_cast<int>(t.tool), t.name, t.key);
      ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2));
      if (ImGui::BeginPopup("##prtoolpop")) {
        for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
          ImGui::PushID(static_cast<int>(i));
          const ImVec2 q = ImGui::GetCursorScreenPos();
          const float lh = ImGui::GetTextLineHeight();
          if (ImGui::Selectable("##t", st.tool == tools[i].tool, 0, ImVec2(200, 0))) st.tool = tools[i].tool;
          ImDrawList* pdl = ImGui::GetWindowDrawList();
          ui::drawIcon(pdl, tools[i].icon, ImVec2(q.x + 8, q.y + lh * 0.5f), lh - 2, st.tool == tools[i].tool ? P.text : P.textDim);
          pdl->AddText(ImVec2(q.x + 22, q.y), P.text, i18n::tr(tools[i].name));
          if (const char* kk = keys::label(tools[i].key)) {
            const float kw = ImGui::CalcTextSize(kk).x;
            pdl->AddText(ImVec2(q.x + 200 - kw, q.y), P.textDim, kk);
          }
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0] && !calm; ++i) {
      if (i) ImGui::SameLine(0, 2);
      if (ui::ToolButton(tools[i].key, tools[i].icon, st.tool == tools[i].tool, fh, tools[i].name, tools[i].desc, tools[i].key,
                         theme::rgb(tools[i].role)))
        st.tool = tools[i].tool;
      if (st.tool == tools[i].tool) ui::noteActiveTool("piano", static_cast<int>(tools[i].tool), tools[i].name, tools[i].key);
    }
    // Groups to the right of the tools appear only when they fit (no clipping).
    auto roomLeft = []() { return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetItemRectMax().x; };
    bool calmToolDrawn = false;
    if (roomLeft() > fh + 6) {  // K-4: auto scrolling, the pattern editors' switch (beside the tools, as in the playlist)
      ImGui::SameLine(0, 4);
      if (calm) {  // K-112D (audit G9): the lane under the notes shows on request, as the reference's MIDI Draw (144)
        const bool laneOn = a.settings.getBool("piano.smoothLane");
        if (ui::IconButton("prlane", ui::Icon::EnvelopePoints, laneOn, fh,
                           laneOn ? "The velocity lane is shown under the notes (click to hide it)" : "Show the velocity lane under the notes",
                           P.accent))
          a.settings.set("piano.smoothLane", !laneOn);
        ImGui::SameLine(0, 4);
      }
      const bool follow = a.settings.getBool("ui.autoScrollPattern");
      // K-112D (audit G5): Smooth draws auto scrolling as the reference's catch, lit in its blue.
      if (ui::IconButton("prfollow", calm ? ui::Icon::Follow : ui::Icon::ScrollTo, follow, fh,
                         follow ? "Auto scrolling is on: the piano roll and channel rack follow the pattern position while it plays (click to keep the view still)"
                                : "Auto scrolling is off: the view stays where you put it while the pattern plays (click to follow the pattern position)",
                         calm ? P.accent : theme::rgb(theme::role::kView)))
        a.settings.set("ui.autoScrollPattern", !follow);
      if (calm) {  // D8: 8 px between the bar's groups (10 and 6 before), 4 inside one
        ImGui::SameLine(0, 8);
        calmTool();
        calmToolDrawn = true;
        ImGui::SameLine(0, 8);
      }
      if (calm && roomLeft() > 120 + fh) {  // K-112D (audit G5): the pitch and position under the pointer (144: "B7 1.4.1.1")
        char rb[96] = "";  // four 64-bit tick fields can take 80 characters
        if (gReadPitch >= 0 && gReadPitch < 128) {
          const Tick bar = std::max<Tick>(1, pm.baseBar()), beat = std::max<Tick>(1, pm.baseBeat()), div = std::max<Tick>(1, beat / 4);
          const Tick t = gReadTick;
          std::snprintf(rb, sizeof rb, "%s%d   %lld.%lld.%lld.%lld", kNoteNames[gReadPitch % 12], gReadPitch / 12 - 1, static_cast<long long>(t / bar + 1),
                        static_cast<long long>(t % bar / beat + 1), static_cast<long long>(t % beat / div + 1), static_cast<long long>(t % div + 1));
        }
        // R09 (visual gap 8): off the grid the box keeps the last reading, as 144's always shows one
        static char lastRb[96] = "C3   1.1.1.1";
        if (rb[0]) std::snprintf(lastRb, sizeof lastRb, "%s", rb);
        else std::snprintf(rb, sizeof rb, "%s", lastRb);
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const float rw = 112.0f;
        ImGui::Dummy(ImVec2(rw, fh));
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        rdl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + fh), theme::mix(P.surface950, P.ink, 0.3f), 5.0f);
        const float rfs = theme::kFontLabel + 1;
        const float tw = a.fontRegular->CalcTextSizeA(rfs, 1e9f, 0, rb).x;
        // D8: centred as the bar's other text (it sat 1 px high)
        rdl->AddText(a.fontRegular, rfs, ImVec2(std::round(rp.x + (rw - tw) * 0.5f), std::round(rp.y + (fh - rfs) * 0.5f)), P.text, rb);
        ui::Hint("The pitch and the position (bar, beat, division, tick) under the pointer");
        ImGui::SameLine(0, 4);
      }
    }
    if (calm && !calmToolDrawn) {  // a narrow pane: the tool still shows
      ImGui::SameLine(0, 4);
      calmTool();
    }
    if (calm && roomLeft() > fh + 120) {
      // R09-09: Smooth's snap as the arrange's and 144's: "Snap:", a blue power switch, the grid ("Smart"
      // for the grid that follows the zoom: the reference's word for it).
      ImGui::SameLine(0, 8);
      {
        const char* lbl = i18n::tr("Snap:");
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(lp.x, std::round(lp.y + (fh - ImGui::GetFontSize()) * 0.5f)), P.textDim, lbl);
        ImGui::Dummy(ImVec2(ImGui::CalcTextSize(lbl).x, fh));
        ImGui::SameLine(0, 4);
      }
      static Snap lastSnap = Snap::Line;
      const bool snapOn = st.snap != Snap::None;
      if (ui::IconButton("##prsnapon", ui::Icon::Power, snapOn, fh, snapOn ? "Snap is on: click to place notes freely" : "Snap is off: click to snap to the grid again",
                         P.accent)) {
        if (snapOn) lastSnap = st.snap, st.snap = Snap::None;
        else st.snap = lastSnap != Snap::None ? lastSnap : Snap::Line;
      }
      ImGui::SameLine(0, 2);
      auto calmSnapName = [](Snap sn) -> const char* { return sn == Snap::Line ? i18n::tr("Smart") : snapName(sn); };
      if (ui::BeginCombo("##prcalmsnap", calmSnapName(st.snap), 96)) {
        for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
          if (ImGui::Selectable(calmSnapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) {
            st.snap = static_cast<Snap>(i);
            if (st.snap != Snap::None) lastSnap = st.snap;
          }
        ImGui::EndCombo();
      }
      ui::Hint("Snap: where notes land while drawing and dragging (Smart follows the zoom; hold Alt while dragging to bypass it)");
    } else if (!calm && roomLeft() > fh + 2 + 128 + 18) {
      ui::ToolbarDivider();
      ui::ToolButton("snapicon", ui::Icon::ToolSnap, st.snap != Snap::None, fh, i18n::N_("Snap"),
                     i18n::N_("snap to the grid chosen beside it (hold Alt while dragging to bypass)"), nullptr, theme::rgb(theme::role::kSnap));
      ImGui::SameLine(0, 2);
      std::string snapLabel = snapName(st.snap);
      if ((st.snap == Snap::Line || st.snap == Snap::Cell) && st.effGrid > 0)
        snapLabel += " \xC2\xB7 " + timeline::gridName(pm, st.effGrid);
      if (ui::BeginCombo("##snap", snapLabel.c_str(), 128)) {
        for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
          if (ImGui::Selectable(snapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) st.snap = static_cast<Snap>(i);
        ImGui::EndCombo();
      }
      ui::Hint("Snap: Line and Cell follow the zoom (free at the deepest zoom); hold Alt while dragging to invert snapping");
    }
    if (pat && !calm && roomLeft() > 56 + 8) {  // K-193: the pattern's meter, and whether it matches where it plays
      ImGui::SameLine(0, 6);
      const int pn = s.patternMeterNum(*pat), pd = s.patternMeterDen(*pat);
      // The pattern's first clip in the playlist, and the meter of the section it starts in.
      const Clip* first = nullptr;
      for (const auto& c : s.playlist)
        if (c.kind == ClipKind::Pattern && c.pattern == pat->id && (!first || c.start < first->start)) first = &c;
      int sn = pn, sd = pd;
      if (first && meter::changeCount(s) > 0) meter::meterAt(s, first->start, sn, sd);
      const bool differs = sn != pn || sd != pd;
      const std::string lbl = meter::text(pn, pd);
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(differs ? P.hot : P.text));
      const bool clicked = ui::TextButton((lbl + "##prmeter").c_str(), false, 0.0f);
      ImGui::PopStyleColor();
      char hint[160];
      if (differs)
        std::snprintf(hint, sizeof hint, i18n::tr("Meter of this pattern: %s. Its first clip starts in a %s section (click to change)"), lbl.c_str(),
                      meter::text(sn, sd).c_str());
      else if (pat->ownMeter())
        std::snprintf(hint, sizeof hint, i18n::tr("Meter of this pattern: %s, its own (click to change)"), lbl.c_str());
      else
        std::snprintf(hint, sizeof hint, i18n::tr("Meter of this pattern: %s, the song's meter at the start (click to change)"), lbl.c_str());
      ui::Hint(hint);
      if (clicked) ImGui::OpenPopup("##prmeterpop");
      if (ImGui::BeginPopup("##prmeterpop")) {
        meterui::patternMeterMenu(a, pat->id);
        if (first) meterui::clipMeterItems(a, first->id);
        ImGui::EndPopup();
      }
    }
    float room = roomLeft();
    if (!calm && room > 120 + 60) {  // K-112D (D3): Smooth quantizes from its local inspector (Q), as 144
      ui::ToolbarDivider();
      const bool quantize = ui::TextButton("Quantize", false, 0, "Quantize note starts to the snap grid (Ctrl+Q)") && pat && chan;
      if (quantize && multiSel(a)) {  // K-198 follow-up: the selection over every edited channel
        const Tick gsz = st.effGrid > 1 ? st.effGrid : snapTicks(pm, st.snap);
        multiApply(a, pat->id, chan->id, [&](Note& n) { n.start = gsz > 1 ? std::max<Tick>(0, ((n.start + gsz / 2) / gsz) * gsz) : n.start; },
                   "quantize notes");
        pat = s.pattern(a.curPattern), chan = s.channel(a.selChannel);
      } else if (quantize && !st.editChannels.empty() && st.selected.empty()) {
        // K-198: nothing selected and other channels edited here: every edited channel, one undo step.
        const Tick gsz = st.effGrid > 1 ? st.effGrid : snapTicks(pm, st.snap);
        std::vector<ObjectId> chans{chan->id};
        for (ObjectId c : st.editChannels)
          if (c != chan->id && s.channel(c)) chans.push_back(c);
        a.doc.beginGroup("quantize notes");
        for (ObjectId c : chans) {
          std::vector<Note> ns = rollNotes(a, pat->id, c);
          if (ns.empty()) continue;
          ops::quantizeStarts(ns, gsz);
          std::sort(ns.begin(), ns.end(), [](const Note& x, const Note& y) { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; });
          stepnotes::write(a.doc, pat->id, c, ns, "quantize notes", 0);
        }
        a.doc.endGroup(false);
        pat = s.pattern(a.curPattern), chan = s.channel(a.selChannel);
      } else if (quantize) {
        if (std::vector<Note> ns = rollNotes(a, pat->id, chan->id); !ns.empty()) {
          Tick gsz = st.effGrid > 1 ? st.effGrid : snapTicks(pm, st.snap);
          auto sel = selVector(a, ns.size());
          bool any = !st.selected.empty();
          for (size_t i = 0; i < ns.size(); ++i)
            if (!any || sel[i]) ns[i].start = snapNearest(ns[i].start, gsz);
          commit(a, pat->id, chan->id, ns, sel, "quantize notes", 0);
        }
      }
      ImGui::SameLine(0, 4);
      if (!calm && ui::IconButton("ghosts", ui::Icon::Clone, st.ghostNotes, fh, "Ghost notes: show other channels' notes (double-click a ghost note to edit its channel)",
                         theme::rgb(theme::role::kGhost)))
        st.ghostNotes = !st.ghostNotes;
    }
    if (!calm && roomLeft() > 120 + 30) {  // J-11: scale helper (the chord stamp has its own button beside the menu)
      ui::ToolbarDivider();
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
    if (!calm && roomLeft() > slideNeed) {
      ui::ToolbarDivider();
      if (ui::TextButton("Slide", newSlide, 0, "Slide mode: new notes glide the channel's sounding notes to their pitch instead of starting a note. Instruments that cannot glide bend their sounding notes instead"))
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
          if (k == newColor) pdl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q.x + 21, q.y + 21), P.accent, 3.0f, 2.0f, 0);
        }
        ImGui::EndPopup();
      }
    }
    if (calm && roomLeft() > 2 * (kZoomSliderW + 22) + 18) {
      // K-112D (D3, G5): the reference's two zoom sliders (144: key height, then time) in place of - and +.
      ImGui::SameLine(0, 10);
      const double zmin = timeline::minPxPerTick(s), zmax = timeline::maxPxPerTick(s);
      const float vt = (st.rowHeight - kRowHMin) / (kRowHMax - kRowHMin);
      const float nv = zoomSlider("##przv", false, vt, fh, "Key height: drag to zoom the rows (Alt+wheel over the notes)");
      if (nv >= 0) st.rowHeight = std::round(kRowHMin + nv * (kRowHMax - kRowHMin));
      ImGui::SameLine(0, 8);
      const float ht = static_cast<float>(std::log(st.pxPerTick / zmin) / std::log(zmax / zmin));
      const float nh = zoomSlider("##przh", true, ht, fh, "Time zoom: drag to zoom (Ctrl+wheel over the notes, PgUp / PgDn)");
      if (nh >= 0) st.zoomRequest = static_cast<float>(zmin * std::pow(zmax / zmin, static_cast<double>(nh)) / st.pxPerTick);
    } else if (!calm && roomLeft() > 76) {
      ui::ToolbarDivider();
      if (ui::IconButton("zout", ui::Icon::Minus, false, fh, "Zoom out (PgDn, Ctrl+wheel)")) st.zoomRequest /= 1.5f;
      ImGui::SameLine(0, 2);
      if (ui::IconButton("zin", ui::Icon::Plus, false, fh, "Zoom in (PgUp, Ctrl+wheel)")) st.zoomRequest *= 1.5f;
    }
  }
  ui::EndToolbar();
  if (liveBar) ImGui::PopStyleVar();
  }  // (not the Tracker's row)
  stampui::escapeTurnsOff(a);  // Esc over the piano roll turns a chord stamp off

  if (!pat || !chan) {
    ImVec2 mn = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ImGui::Dummy(av);
    ui::EmptyState(ImGui::GetWindowDrawList(), mn, ImVec2(mn.x + av.x, mn.y + av.y), "Nothing to edit",
                   "Select a pattern and a channel to edit its notes");
    if (clipColW > 0) ImGui::Unindent(clipColW);
    ui::EndDockWindow();
    return;
  }

  const ObjectId patId = pat->id, chId = chan->id;
  std::vector<Note> notes = rollNotes(a, patId, chId);  // K-22: rack steps show as step notes
  // K-112 (Live): Fold shows only the rows that hold notes (top to bottom, highest first): foldRow
  // gives each pitch its row (-1: folded away), foldPitch each row its pitch. Studio never folds.
  const bool fold = st.fold && livelook::active(a) && !notes.empty();
  std::array<int, 128> foldRow{};
  std::vector<int> foldPitch;
  if (fold) {
    std::array<bool, 128> used{};
    for (const auto& n : notes) used[static_cast<size_t>(std::clamp(n.pitch, 0, 127))] = true;
    for (int p = 127; p >= 0; --p) {
      foldRow[static_cast<size_t>(p)] = used[static_cast<size_t>(p)] ? static_cast<int>(foldPitch.size()) : -1;
      if (used[static_cast<size_t>(p)]) foldPitch.push_back(p);
    }
  }
  const float rowsN = fold ? static_cast<float>(foldPitch.size()) : 128.0f;  // rows the view scrolls through
  const bool rackSteps = stepnotes::hasSteps(s, *pat, chId);
  for (auto it = st.selected.begin(); it != st.selected.end();)
    it = *it < notes.size() ? std::next(it) : st.selected.erase(it);

  // K-112b: the Logic layout's editor inspector on the left: what is edited, quantise with strength
  // and swing, the scale, and the selected notes' velocity.
  if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic) && ImGui::GetContentRegionAvail().x >= 760) {
    const float iw = 196;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::BeginChild("##prinsp", ImVec2(iw, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImDrawList* idl = ImGui::GetWindowDrawList();
    {  // the pattern, and the channel that plays it
      const ImVec2 q = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      // K-112D (audit G6): the track's icon, then the region's name and "on" its track, as 144 heads it.
      const int ic = chan->icon > 0 ? chan->icon : trackicons::guess(chan->name, "Synth");
      if (ic > 0) trackicons::draw(idl, ic, ImVec2(q.x + 12, q.y + 18), 24, theme::mix(theme::content(chan->color), P.text, 0.3f));
      else idl->AddRectFilled(ImVec2(q.x, q.y + 2), ImVec2(q.x + 4, q.y + 34), theme::content(chan->color), 2.0f);
      const float tx = q.x + (ic > 0 ? 30.0f : 10.0f);
      theme::textEllipsis(idl, a.fontBold, theme::kFontBody, ImVec2(tx, q.y + 2), q.x + w - tx, P.text, pat->name.c_str());
      const std::string on = std::string(i18n::tr("on")) + " " + chan->name;
      theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(tx, q.y + 20), q.x + w - tx, P.textDim, on.c_str());
      ImGui::Dummy(ImVec2(w, 40));
    }
    auto section = [&](const char* title) {
      ImGui::Dummy(ImVec2(0, 4));
      const ImVec2 q = ImGui::GetCursorScreenPos();
      idl->AddLine(ImVec2(q.x - 10, q.y), ImVec2(q.x + ImGui::GetContentRegionAvail().x + 10, q.y), P.line);
      ImGui::Dummy(ImVec2(0, 6));
      if (title) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(title));
    };
    auto valueSlider = [&](const char* id, const char* label, int* v, int lo, int hi, const char* hint) {
      const float w = ImGui::GetContentRegionAvail().x;
      const ImVec2 q = ImGui::GetCursorScreenPos();
      const char* l = i18n::tr(label);
      char val[16];
      std::snprintf(val, sizeof val, "%d", *v);
      idl->AddText(q, P.text, l);
      idl->AddText(ImVec2(q.x + w - ImGui::CalcTextSize(val).x, q.y), P.text, val);
      ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + 2));
      const ImVec2 t0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton(id, ImVec2(w, 16));
      const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
      bool changed = false;
      if (act) {
        const float f = std::clamp((ImGui::GetIO().MousePos.x - t0.x - 6) / std::max(1.0f, w - 12), 0.0f, 1.0f);
        const int nv = lo + static_cast<int>(std::lround(f * static_cast<float>(hi - lo)));
        if (nv != *v) *v = nv, changed = true;
      }
      const float f = static_cast<float>(*v - lo) / static_cast<float>(std::max(1, hi - lo));
      const float tx = std::round(t0.x + 6 + (w - 12) * f), cy = t0.y + 8;
      idl->AddRectFilled(ImVec2(t0.x, cy - 2), ImVec2(t0.x + w, cy + 2), theme::mix(P.surface800, P.ink, 0.45f), 2.0f);
      idl->AddRectFilled(ImVec2(t0.x, cy - 2), ImVec2(tx, cy + 2), P.hot, 2.0f);
      idl->AddCircleFilled(ImVec2(tx, cy), 6.0f, hov || act ? P.text : theme::mix(P.text, P.surface800, 0.15f), 16);
      if (hint && hov) ui::Hint(hint);
      return changed;
    };
    // Quantise: the snap grid, with a strength (how far notes move) and a swing (every second
    // grid position later or earlier).
    static int strength = 100, swing = 50;
    // Checkpoint 30: the quantise section shows only when it fits whole (150 %, short panes): its
    // heading, two sliders and the button, never a button cut at the pane's foot.
    const float lh = ImGui::GetTextLineHeight(), sp = ImGui::GetStyle().ItemSpacing.y;
    const bool showQuant = ImGui::GetContentRegionAvail().y >= 10 + lh + sp + 2 * (lh + 2 + 16 + 2 * sp) + ImGui::GetFrameHeight() + sp;
    // R02-21: a short pane (960 x 600) keeps the Quantize button on its own (strength and swing as
    // last set): the reference's inspector always shows its time quantize.
    const bool quantButton = showQuant || ImGui::GetContentRegionAvail().y >= 10 + ImGui::GetFrameHeight() + sp + 10 + lh + 2 + 16 + 2 * sp + 4;
    if (showQuant) section("Quantize");
    if (!showQuant && quantButton) section(nullptr);
    // K-112D (audit G7): Smooth quantizes as the reference's inspector does: a pop-up for the grid
    // ("Snap grid" follows the editor's snap, or a fixed note value) beside a compact Q button, in one
    // row above Strength and Swing (144, 152).
    const bool lgQ = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
    static int qValue = 0;  // 0 the snap grid; else an entry of kQ below
    struct QVal {
      const char* name;
      int perQuarter;  // grid steps per quarter note (3 and 6: triplets)
    };
    static const QVal kQ[] = {{i18n::N_("Snap grid"), 0}, {i18n::N_("1/4 note"), 1},     {i18n::N_("1/8 note"), 2},     {i18n::N_("1/16 note"), 4},
                                {i18n::N_("1/32 note"), 8}, {i18n::N_("1/8 triplet"), 3}, {i18n::N_("1/16 triplet"), 6}};
    bool quantNow = false;
    if (lgQ && quantButton) {
      const float qw = 30.0f, cw = ImGui::GetContentRegionAvail().x - qw - 4;
      if (ui::BeginCombo("##prqgrid", i18n::tr(kQ[qValue].name), cw)) {
        for (int i = 0; i < static_cast<int>(sizeof kQ / sizeof kQ[0]); ++i)
          if (ImGui::Selectable(i18n::tr(kQ[i].name), qValue == i)) qValue = i;
        ImGui::EndCombo();
      }
      ui::Hint("The grid the notes are quantized to");
      ImGui::SameLine(0, 4);
      quantNow = ui::TextButton("Q##prq", false, qw, "Quantize the selected notes (none selected: every note) to the grid beside it (Ctrl+Q)");
    }
    if (showQuant) valueSlider("##prqstr", "Strength", &strength, 0, 100, "How far the notes move toward the grid, in percent");
    if (showQuant) valueSlider("##prqswing", "Swing", &swing, 0, 100, "50: straight; higher: every second grid position later, lower: earlier");
    if (!lgQ && quantButton)
      quantNow = ui::TextButton("Quantize notes", false, ImGui::GetContentRegionAvail().x, "Quantize the selected notes (none selected: every note) to the snap grid");
    if (quantNow) {
      std::vector<Note> ns = notes;
      Tick gsz = std::max<Tick>(1, st.effGrid > 1 ? st.effGrid : snapTicks(pm, st.snap == Snap::None ? Snap::Line : st.snap));
      if (lgQ && qValue > 0) gsz = std::max<Tick>(1, static_cast<Tick>(s.ppq) / kQ[qValue].perQuarter);
      auto sel = selVector(a, ns.size());
      const bool any = !st.selected.empty();
      for (size_t i = 0; i < ns.size(); ++i) {
        if (any && !sel[i]) continue;
        Tick target = snapNearest(ns[i].start, gsz);
        if ((target / gsz) % 2 == 1) target += static_cast<Tick>(std::lround(static_cast<double>(swing - 50) / 100.0 * static_cast<double>(gsz)));
        ns[i].start += static_cast<Tick>(std::lround(static_cast<double>(target - ns[i].start) * strength / 100.0));
      }
      commit(a, patId, chId, ns, sel, "quantize notes", 0);
      notes = rollNotes(a, patId, chId);
    }
    // Checkpoint 30: a short pane (1280 x 800) shows only the sections that fit whole, velocity first
    // (it acts on the selection), then the scale; nothing is cut off at the pane's foot.
    const float roomY = ImGui::GetContentRegionAvail().y;
    const float velNeed = 10 + lh + 2 + 16 + 2 * sp + 4, scaleNeed = 10 + lh + sp + 2 * (ImGui::GetFrameHeight() + sp) + 4;
    const bool showVel = roomY >= velNeed, showScale = roomY >= velNeed + scaleNeed;
    if (showScale) section("Scale");
    if (showScale) {
      static const char* const keyNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const auto& sc = tools::scales();
      const float w = ImGui::GetContentRegionAvail().x;
      if (ui::BeginCombo("##prroot", keyNames[std::clamp(st.scaleRoot, 0, 11)], 52)) {
        for (int k = 0; k < 12; ++k)
          if (ImGui::Selectable(keyNames[k], st.scaleRoot == k)) st.scaleRoot = k;
        ImGui::EndCombo();
      }
      ImGui::SameLine(0, 4);
      const char* scName = st.scale > 0 && st.scale < static_cast<int>(sc.size()) ? sc[static_cast<size_t>(st.scale)].name : i18n::tr("No scale");
      if (ui::BeginCombo("##prscale", scName, w - 56)) {
        if (ImGui::Selectable(i18n::tr("No scale"), st.scale == 0)) st.scale = 0;
        for (size_t i = 1; i < sc.size(); ++i)
          if (ImGui::Selectable(sc[i].name, st.scale == static_cast<int>(i))) st.scale = static_cast<int>(i);
        ImGui::EndCombo();
      }
      ui::Hint("Rows outside the scale are shaded");
      ImGui::BeginDisabled(st.scale == 0);
      ImGui::Checkbox(i18n::tr("Snap drawn notes to it"), &st.snapToScale);
      ImGui::EndDisabled();
    }
    if (showVel) section(nullptr);  // K-112b (second pass): the velocity row is its own heading (144), not "Velocity" twice
    if (showVel) {
      int vel = st.lastVelocity;
      if (!st.selected.empty()) {
        int sum = 0;
        for (size_t i : st.selected) sum += notes[i].velocity;
        vel = sum / static_cast<int>(st.selected.size());
      }
      const int before = vel;
      if (valueSlider("##prvel", "Velocity", &vel, 1, 127, "The selected notes' velocity (none selected: the velocity new notes get)") && vel != before) {
        if (st.selected.empty()) {
          st.lastVelocity = vel;
          st.lastSet = true;
        } else {
          std::vector<Note> ns = notes;
          auto sel = selVector(a, ns.size());
          for (size_t i = 0; i < ns.size(); ++i)
            if (sel[i]) ns[i].velocity = vel;
          commit(a, patId, chId, ns, sel, "note velocity", 0x112B0310);
          notes = rollNotes(a, patId, chId);
        }
      }
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
  }

  // ---------------- canvas geometry ----------------
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 160.0f);
  // K-112 (Live): the stacked band can be short (1280 x 800); a lower floor keeps the lane or the
  // envelope choosers inside the window instead of cut off at its foot.
  avail.y = std::max(avail.y, livelook::active(a) ? 96.0f : 140.0f);
  // K-171: the lower lane's height: dragged on its splitter (saved), 0 = automatic.
  const float laneMaxH = std::max(40.0f, std::round(avail.y * 0.6f));
  const float laneSetH = static_cast<float>(a.settings.getNumber("piano.laneHeight"));
  // K-112c (round 25): Live's lane is 32 px by default and can be dragged down to it (Studio: 40).
  constexpr float kLiveVelH = 32.0f;
  const float laneMinH = livelook::active(a) ? kLiveVelH : 40.0f;
  // Left to right: a slim vertical scroller, the keyboard, the grid; above the
  // grid a slim horizontal scroller with the notes' overview, then the ruler
  // (the playlist's arrangement).
  const float vbarW = scrollbarui::kBarW, hbarH = scrollbarui::kBarH;
  // K-112 (Live): an envelope (the Envelopes tab: the pattern's automation) is drawn across the
  // notes, as the refs draw it; the band under the grid then holds only its device and parameter
  // choosers (kEnvFootH).
  const bool liveEnvelope = livelook::active(a) && pianoAutomationLane().owner != kNoId;
  constexpr float kEnvFootH = 34.0f;
  // K-112 (Live): a loop brace row under the ruler (the pattern's length in its colour).
  // K-112c (round 24): Live's ruler is one 16 px row of bar numbers over a 10 px brace (the refs'
  // 35 to 40 px with the scroller).
  const float braceH = livelook::active(a) ? 10.0f : 0.0f, numDy = livelook::active(a) ? 2.0f : 4.0f;
  // K-112b (checkpoint 24): the Tracks layout's keyboard is 44 px, as 144, 127 and 152 draw it (the plan's
  // size; it was the other layouts' 64).
  const bool lgKeys = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
  const float keyW = lgKeys ? 44.0f : 64.0f, rulerH = hbarH + (livelook::active(a) ? 16.0f : 26.0f) + braceH,
              velH = lgKeys && !liveEnvelope && !a.settings.getBool("piano.smoothLane") ? 0.0f  // K-112D (audit G9): hidden until asked for
                     : liveEnvelope ? kEnvFootH
                     : laneSetH > 0 ? std::clamp(laneSetH, laneMinH, laneMaxH)
                     : livelook::active(a) ? std::min(kLiveVelH, std::round(avail.y * 0.3f))  // K-112c (round 25): 32 px, the refs' slim lane
                                           : std::clamp(avail.y * 0.18f, 48.0f, 96.0f);
  ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::BeginDragDropTarget()) {  // MIDI files from the browser (J-16)
    browserui::acceptFileDrop(a, browse::DropTarget::PianoRoll, {});
    ImGui::EndDragDropTarget();
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();

  const float keysX0 = origin.x + vbarW, rulerTop = origin.y + hbarH;
  const ImVec2 gridMin(keysX0 + keyW, origin.y + rulerH);
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
  // Checkpoint 59 (N58-08): in Clips a clip that opens, or gets its first notes (a recording into an
  // empty clip), shows at least 12 key rows and is centred on its notes.
  // Checkpoint 60 (N59-05): and while nobody scrolled it since, it follows the notes' range as notes
  // arrive (a recording's later, higher notes), so the top note stays in view; a range of up to 12
  // semitones that the rows cannot show fits by lower rows (a short panel at 1024 x 640).
  if (livelook::active(a) && gridH > 0) {
    static ObjectId lastPat = kNoId;
    static int lastLo = -1, lastHi = -1;
    static bool scrolled = false;
    int lo = 128, hi = -1;
    for (const auto& n : notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
    if (patId != lastPat) scrolled = false;
    if (hovered && (io.MouseWheel != 0 || io.MouseWheelH != 0 || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) scrolled = true;
    if (patId != lastPat || (!scrolled && (lo != lastLo || hi != lastHi))) {
      const int span = hi >= lo ? hi - lo + 1 : 0;
      const float fit = std::floor(gridH / static_cast<float>(span > 0 && span <= 12 ? std::max(span + 1, 4) : 12));
      if (fit < st.rowHeight) st.rowHeight = std::max(kRowHMin, fit);
      st.centred = false;
    }
    lastPat = patId, lastLo = lo, lastHi = hi;
  }
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
    // An end of the vertical scroller dragged: the thumb's length sets the key height, the opposite edge's row stays put (1:1).
    if (vBarDrag.zooming() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const double to = std::clamp(scrollbarui::endZoom(vBarDrag, io.MousePos, scrollbarui::kThumbMinV, gridH), static_cast<double>(kRowHMin),
                                   static_cast<double>(kRowHMax));
      if (std::fabs(to - vg.target(drawn)) > 1e-4) vg.zoomTo(drawn, to, vBarRow, false, zp);
    }
    st.rowHeight = static_cast<float>(vg.step(drawn, io.DeltaTime, zp));
  }
  const float rowH = st.rowHeight;
  layoutKeys(rowH);
  const Tick patLen = s.patternContentLength(*pat);
  // N48-12: in Clips the selected clip's Start and End bound the notes that play: the brace, the
  // shading and the clip view's fields are that one region (its ends drag in the brace row).
  Tick regA = 0, regB = patLen;
  const ObjectId regClip = livelook::clipRegion(a, pat->id, regA, regB);
  bool braceTook = false;
  // The scrollable time: the pattern and eight bars after it (as the playlist's
  // song), in proportion to the zoom so the scroller's thumb and ends track the
  // pointer exactly while they scroll and zoom.
  const Tick contentTicks = std::max(patLen, regB) + pm.baseBar() * 8;
  auto maxScrollAt = [&](float pxPerTick) { return std::max(0.0f, static_cast<float>(contentTicks) * pxPerTick - gridW); };
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
      // N59-05: the middle of the range's rows at the middle of the grid (row p spans (127 - p) * rowH
      // and one row more).
      st.scrollY = ((livelook::active(a) ? 127.5f : 127.0f) - (lo + hi) * 0.5f) * rowH - gridH * 0.5f;
    }
  }
  if (st.rowGlide.anchored()) {  // K-24: the anchored point back under the pointer (the range's edges hold)
    const smoothzoom::RowAnchor& an = st.rowGlide.anchor();
    st.scrollY = std::clamp(static_cast<float>(smoothzoom::rowAnchorPos(keyTop.data(), 128, rowH, an) - an.px), 0.0f, std::max(0.0f, rowsN * rowH - gridH));
    st.rowGlide.scrollOut = st.scrollY;
  }
  // J-67: outside writes are clamped here; the view's own glide / elastic
  // overscroll is left to scrollui::step below.
  scrollui::clampOuter(st.axY, st.scrollY, 0.0f, std::max(0.0f, rowsN * rowH - gridH));
  scrollui::clampOuter(st.axX, st.scrollX, 0.0f, maxScrollAt(st.pxPerTick));

  auto xOf = [&](Tick t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto yOf = [&](int pitch) {
    if (fold) {
      const int r = foldRow[static_cast<size_t>(std::clamp(pitch, 0, 127))];
      return r < 0 ? -1.0e6f : gridMin.y + static_cast<float>(r) * rowH - st.scrollY;  // folded away: off the view
    }
    return gridMin.y + (127 - pitch) * rowH - st.scrollY;
  };
  auto tickAt = [&](float x) { return timeline::tickAtPx(x - gridMin.x + st.scrollX, st.pxPerTick); };
  // J-46: clamped before the conversion (the pointer is -FLT_MAX outside the window).
  auto pitchAt = [&](float y) {
    if (fold)
      return foldPitch[static_cast<size_t>(std::clamp(std::floor((y - gridMin.y + st.scrollY) / rowH), 0.0f, rowsN - 1))];
    return 127 - static_cast<int>(std::clamp(std::floor((y - gridMin.y + st.scrollY) / rowH), 0.0f, 127.0f));
  };
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
    st.pxPerTick = std::min(20.0f / sixteenth, std::max(8.0f / sixteenth, gridW / static_cast<float>(std::max<Tick>(1, pm.baseBar()))));
    st.zoomInit = true;
  }
  {  // D4 (R10 visual gap 7): Smooth opens a region zoomed to it, as the reference's piano roll shows the
     // whole region across the editor (144, 304); a 2-bar region no longer sits in a wide empty grid.
    // The fit is asked again for a few frames: the pane settles its width after it opens.
    static ObjectId fitPat = kNoId;
    static int fitFrames = 0;
    if (logicplug::active(a) && patId != fitPat) fitFrames = 8;
    fitPat = patId;
    if (fitFrames > 0 && gridW > 120) st.fitRequest = true, --fitFrames;
  }
  if (st.fitRequest) {
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, std::max<Tick>(patLen, pm.baseBar()) + pm.baseBeat(), gridW));
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
  // An end of the horizontal scroller dragged: the thumb's length sets the zoom, the opposite edge's tick stays put (1:1).
  bool barZoom = false;
  if (hBarDrag.zooming() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const double z = timeline::clampZoom(s, scrollbarui::endZoom(hBarDrag, io.MousePos, scrollbarui::kThumbMinH, gridW));
    st.pxPerTick = static_cast<float>(z);
    st.scrollX = static_cast<float>(std::max(0.0, hBarDrag.edgeUnits * z - hBarDrag.edgePx));
    barZoom = true;
  }
  st.pxPerTick = static_cast<float>(timeline::clampZoom(s, st.pxPerTick));
  // A trackpad's Ctrl+scroll and a scroller end follow the pointer 1:1; wheel notches and every other zoom glide.
  scrollui::zoomEnd(st.zoomGlide, st.pxPerTick, st.scrollX, (wheelZoom && !smoothscroll::isNotch(io.MouseWheel)) || barZoom);
  // K-4: auto scrolling: page along with the pattern position while this pattern plays
  // (not while the user drags or scrolls the view; off: the view stays where it is).
  const double playPos = a.engine.playing() ? rollPlayPos(a, patId) : -1.0;  // #70: song mode too
  if (playPos >= 0 && a.settings.getBool("ui.autoScrollPattern") && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    st.scrollX = ops::followScroll(st.scrollX, static_cast<float>(playPos) * st.pxPerTick, gridW);
  // ---- snapping: the visible grid (Line/Cell) or a fixed size; Alt inverts ----
  Tick visGrid = timeline::visibleGrid(pm, st.pxPerTick);
  st.effGrid = timeline::snapGrid(pm, st.snap, st.pxPerTick, false);
  Tick snapG = timeline::snapGrid(pm, st.snap, st.pxPerTick, io.KeyAlt);
  livelook::editGrid(a, 1, st.snap, st.pxPerTick, visGrid, snapG, st.effGrid);  // grid2-1118: Clips' Cmd+1 .. 5
  const Tick grid = std::max<Tick>(1, st.effGrid);  // keyboard nudges / quantize
  // R02-17: the Tracks layout starts as the reference does (a 16th note at velocity 80) until a
  // note is touched; then new notes copy the last edited one.
  const bool tracksDefaults = !st.lastSet && logicplug::active(a);
  const Tick defLen = st.lastLength > 0 && !tracksDefaults ? st.lastLength : std::max<Tick>(1, s.ppq / 4);
  const int defVel = tracksDefaults ? 80 : st.lastVelocity;

  // Scroll & zoom.
  // J-67: wheel / trackpad input goes through the smooth scroller: a notch
  // glides three keys (or 60 px) and lands on them, trackpad deltas stay 1:1,
  // the edges stretch a little and spring back instead of jumping.
  // Over the time strip (the horizontal scroller and the ruler) the plain wheel
  // scrolls sideways: that strip has no keys to scroll through.
  float wheelX = 0, wheelY = 0;
  bool notchX = false, notchY = false;
  const bool overTime = hovered && io.MousePos.x >= gridMin.x && io.MousePos.x < gridMax.x && io.MousePos.y >= origin.y && io.MousePos.y < gridMin.y;
  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) {
      // zoomed above (J-188)
    } else if (io.KeyAlt) {
      // the row height, zoomed above (K-24)
    } else if (io.KeyShift || overTime) {
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
  const float maxScrollX = maxScrollAt(st.pxPerTick);
  scrollui::step(st.axX, st.scrollX, wheelX, notchX, 0.0f, maxScrollX);
  scrollui::step(st.axY, st.scrollY, wheelY, notchY, 0.0f, std::max(0.0f, rowsN * st.rowHeight - gridH));
  if (canchor::collecting()) {  // J-111d: collaborators' pointers in pattern time and keys (this pattern and channel)
    const float pxT = std::max(1e-6f, st.pxPerTick), bottom = origin.y + avail.y;
    const ImVec2 o(gridMin.x - st.scrollX, gridMin.y - st.scrollY);
    canchor::publish(canchor::PianoGrid, patId, chId, o, ImVec2(pxT, rowH), gridMin, gridMax);
    canchor::publish(canchor::PianoKeys, patId, chId, ImVec2(keysX0, o.y), ImVec2(keyW, rowH), ImVec2(keysX0, gridMin.y),
                     ImVec2(gridMin.x, gridMax.y));
    canchor::publish(canchor::PianoRuler, patId, chId, ImVec2(o.x, rulerTop), ImVec2(pxT, gridMin.y - rulerTop), ImVec2(gridMin.x, rulerTop),
                     ImVec2(gridMax.x, gridMin.y));
    canchor::publish(canchor::PianoVelocity, patId, chId, ImVec2(o.x, gridMax.y), ImVec2(pxT, std::max(1.0f, bottom - gridMax.y)),
                     ImVec2(gridMin.x, gridMax.y), ImVec2(gridMax.x, bottom));
  }

  // ---------------- draw: grid ----------------
  // K-276: note edges, grid lines and row lines on whole device pixels (PixelGrid.h): touching notes
  // show one gap at every zoom and interface scale, and a note on a beat starts on its line.
  const float dpp = pixelgrid::frameScale();
  auto snapX = [&](Tick t) { return pixelgrid::edge(xOf(t), dpp); };
  auto rowY0 = [&](int pitch) { return pixelgrid::edge(yOf(pitch), dpp); };
  auto rowY1 = [&](int pitch) { return pixelgrid::edge(yOf(pitch) + rowH, dpp); };
  dl->PushClipRect(gridMin, gridMax, true);
  // Grid well: black-key rows a step darker, every other bar a step lighter.
  // K-112b (checkpoint 21): in the Tracks layout (dark faces) the tracks area's near-black ground,
  // black-key rows clearly darker still and time lines lighter than the ground, as 144, 127 and 152
  // draw the piano roll (white-key rows #232323, black-key rows #0E0E0E, light lines).
  const bool lgDark = P.dark && layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
  const ImU32 well = lgDark ? P.surface950 : P.well;
  // K-112c (round 28): Live's black-key rows read as clearly as the refs' bands (058, 112, 198, 044).
  const ImU32 blackRow = lgDark ? theme::mix(well, P.ink, 0.5f)
                         : theme::withAlpha(P.ink, livelook::active(a) ? (P.dark ? 0.30f : 0.09f) : (P.dark ? 0.16f : 0.05f));
  const ImU32 lineBar = lgDark ? theme::mix(well, P.text, 0.15f) : P.gridBar, lineBeat = lgDark ? theme::mix(well, P.text, 0.08f) : P.gridBeat,
              lineStep = lgDark ? theme::mix(well, P.text, 0.04f) : P.gridStep;
  dl->AddRectFilled(gridMin, gridMax, well);
  {
    const Tick bar = std::max<Tick>(1, pm.baseBar());
    // K-112 (Live): the bar blocks read clearly (each ref shows them at a glance).
    const float barA = livelook::active(a) ? (P.dark ? 0.06f : 0.035f) : (P.dark ? 0.03f : 0.015f);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gridMin.x)), bar); xOf(t) < gridMax.x; t += bar)
      if ((t / bar) % 2 == 1)
        dl->AddRectFilled(ImVec2(snapX(t), gridMin.y), ImVec2(snapX(t + bar), gridMax.y), theme::withAlpha(P.paper, barA));
  }
  for (int p = 0; p < 128; ++p) {
    float y = yOf(p);
    if (y > gridMax.y || y + rowH < gridMin.y) continue;
    const ImVec2 r0(gridMin.x, rowY0(p)), r1(gridMax.x, rowY1(p));
    if (isBlack(p)) dl->AddRectFilled(r0, r1, blackRow);
    if (st.scale > 0 && (st.highlightScale || !livelook::active(a))) {  // J-11 scale highlighting (K-112: Live can turn it off): out-of-scale rows shaded, root rows tinted
      if (!tools::inScale(p, st.scaleRoot, st.scale))
        dl->AddRectFilled(r0, r1, theme::withAlpha(P.ink, P.dark ? 0.22f : 0.08f));
      else if ((p - st.scaleRoot) % 12 == 0)
        dl->AddRectFilled(r0, r1, theme::withAlpha(theme::rgb(theme::role::kStamp), 0.07f));
    }
    const pixelgrid::Span ly = pixelgrid::line(y + rowH, dpp);
    if (p % 12 == 0)
      dl->AddRectFilled(ImVec2(gridMin.x, ly.x0), ImVec2(gridMax.x, ly.x1), P.gridBar);
    else if (rowH >= 10)
      dl->AddRectFilled(ImVec2(gridMin.x, ly.x0), ImVec2(gridMax.x, ly.x1), P.gridStep);
  }
  {
    Tick step = std::max<Tick>(1, s.ppq / 4), beat = pm.baseBeat(), bar = pm.baseBar();
    // Lines at the visible grid (finer as you zoom in, down to single ticks).
    const ImU32 fine = theme::withAlpha(lineStep, 0.55f);
    Tick t0 = snapDown(tickAt(gridMin.x), visGrid);
    for (Tick t = std::max<Tick>(0, t0); xOf(t) < gridMax.x; t += visGrid) {
      ImU32 c = t % bar == 0 ? lineBar : t % beat == 0 ? lineBeat : t % step == 0 ? lineStep : fine;
      const pixelgrid::Span l = pixelgrid::line(xOf(t), dpp);
      dl->AddRectFilled(ImVec2(l.x0, gridMin.y), ImVec2(l.x1, gridMax.y), c);
    }
    // Pattern end shading (N48-12: in Clips, outside the clip's Start and End).
    float xe = snapX(regClip != kNoId ? regB : patLen);
    if (lgDark && xe < gridMax.x) {
      // K-112D (audit G8): past the region's end the reference darkens the grid and hatches it, so the
      // region's extent reads at a glance (144); a line in the region's colour marks the end.
      const float hx0 = std::max(xe, gridMin.x);
      dl->AddRectFilled(ImVec2(hx0, gridMin.y), gridMax, theme::withAlpha(P.ink, 0.42f));
      const ImU32 hatch = theme::withAlpha(P.text, 0.035f);
      const float hs = 9.0f, span = gridMax.y - gridMin.y;
      for (float x = std::floor((hx0 - span) / hs) * hs; x < gridMax.x; x += hs)
        if (x + span > hx0) dl->AddLine(ImVec2(x, gridMax.y), ImVec2(x + span, gridMin.y), hatch, 1.0f);
      if (xe >= gridMin.x) dl->AddRectFilled(ImVec2(xe - 1, gridMin.y), ImVec2(xe, gridMax.y), theme::withAlpha(theme::content(pat->color), 0.7f));
    } else if (xe < gridMax.x) {
      dl->AddRectFilled(ImVec2(std::max(xe, gridMin.x), gridMin.y), gridMax, theme::withAlpha(P.surface950, 0.55f));
    }
    if (regClip != kNoId && regA > 0) {
      const float xa = snapX(regA);
      if (xa > gridMin.x) dl->AddRectFilled(gridMin, ImVec2(std::min(xa, gridMax.x), gridMax.y), theme::withAlpha(P.surface950, 0.55f));
    }
  }
  j31::drawPianoBackground(a, gridMin.x, gridMin.y, gridMax.x, gridMax.y, static_cast<double>(tickAt(gridMin.x)),
                           1.0 / std::max(1e-6, static_cast<double>(st.pxPerTick)));  // J-31
  // Ghost notes from other channels (all others, or the chosen ones).
  struct GhostHit { ObjectId ch; int pitch; int index; };
  GhostHit ghostHover{kNoId, -1, -1};
  // K-198: channels edited beside the target draw solid in their colour (a click takes them); their
  // lists are the piano roll's (rack steps as step notes), so a selection of them lines up.
  // K-112D (D3, G1 / R09 visual gap 8): Smooth's editor shows the region's own track only, as the
  // reference's (a drum pattern's other sounds drew as faint outlined boxes over the kick's row); the
  // channels ticked to edit here still show.
  const bool smoothGhostsOff = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
  if ((st.ghostNotes && !smoothGhostsOff) || !st.editChannels.empty()) {
    std::vector<ObjectId> ghostChans;
    for (const auto& [cid, ns] : pat->notes) ghostChans.push_back(cid);
    for (ObjectId cid : st.editChannels)
      if (!pat->notes.count(cid)) ghostChans.push_back(cid);
    for (ObjectId cid : ghostChans) {
      const bool edited = st.editChannels.count(cid) > 0;
      if (cid == chId || (!edited && (smoothGhostsOff || !st.ghostNotes || (!st.ghostChannels.empty() && !st.ghostChannels.count(cid))))) continue;
      const Channel* oc = s.channel(cid);
      if (edited && !oc) continue;
      const ImU32 gcol = oc ? theme::rgb(oc->color) : P.text;
      const std::vector<Note> viewNs = edited ? rollNotes(a, patId, cid) : std::vector<Note>{};
      const std::vector<Note>& ns = edited ? viewNs : pat->notes.at(cid);
      const auto selIt = st.editSel.find(cid);
      for (size_t i = 0; i < ns.size(); ++i) {
        const Note& n = ns[i];
        float y = yOf(n.pitch);
        const pixelgrid::Span gsx = pixelgrid::block(xOf(n.start), xOf(n.start + n.length), dpp, 0.0f, 1.0f), gsy = pixelgrid::block(y, y + rowH, dpp);
        ImVec2 g0(gsx.x0, gsy.x0), g1(gsx.x1, gsy.x1);
        const bool picked = edited && selIt != st.editSel.end() && selIt->second.count(i) > 0;
        dl->AddRectFilled(g0, g1, picked ? theme::mix(gcol, P.paper, 0.45f) : theme::withAlpha(gcol, edited ? 0.62f : 0.13f), 2);
        if (picked) {  // K-198 follow-up: selected, with the lit rim of the target's selected notes
          const float rimIn = pixelgrid::strokeIn(1.5f);
          dl->AddRect(ImVec2(g0.x + rimIn, g0.y + rimIn), ImVec2(g1.x - rimIn, g1.y - rimIn), P.hot, 2, 0, 1.5f);
        } else {
          dl->AddRect(g0, g1, edited ? theme::withAlpha(P.ink, 0.6f) : theme::withAlpha(gcol, 0.32f), 2);
        }
        if (hovered && io.MousePos.x >= g0.x && io.MousePos.x < g1.x && io.MousePos.y >= g0.y && io.MousePos.y < g1.y)
          ghostHover = {cid, n.pitch, edited ? static_cast<int>(i) : -1};
      }
    }
  }
  // Notes.
  const ImU32 noteCol = theme::rgb(chan->color);
  // K-112b: the Logic layout colours notes by velocity (soft: cool, loud: warm), as all the
  // reference editors do; notes in a colour group keep the group's colour.
  // D4 (R10 gap 7): View > Note colours > By region colour (per person) draws them in the region's colour.
  const bool velColours = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic) &&
                          a.settings.getString("piano.smoothNoteColour") != "region";
  auto velocityColour = [](float v) {
    static const ImU32 stops[] = {theme::rgb(0x6A63E8), theme::rgb(0x3AA7D8), theme::rgb(0x3FBF6A), theme::rgb(0xD9C13A), theme::rgb(0xE07A32),
                                  theme::rgb(0xE0453A)};
    const float t = std::clamp(v, 0.0f, 1.0f) * 5.0f;
    const int k = std::min(4, static_cast<int>(t));
    return theme::mix(stops[k], stops[k + 1], t - static_cast<float>(k));
  };
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
  const bool liveNotes = livelook::active(a);
  const RollStyle style = rollStyle();
  const bool squareNotes = liveNotes || style.squareNotes;  // (Live's flat bars; the Tracker's square notes)
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
    ImU32 fill = velColours && n.color == 0 ? velocityColour(vel)
                                            : theme::mix(theme::mix(col, P.well, 0.55f), theme::mix(col, P.paper, 0.12f), vel);
    if (sel) fill = theme::mix(fill, P.paper, 0.45f);
    if (liveEnvelope && !sel) fill = theme::mix(P.ink, P.surface700, 0.35f);  // K-112c: under an envelope the notes go neutral and dark (the refs)
    if (n.slide) fill = theme::withAlpha(fill, 0.55f);  // slide notes: lighter body + glide mark
    if (n.muted) fill = theme::withAlpha(theme::mix(fill, P.well, 0.6f), 0.6f);  // muted: dim, hollow look
    // K-81: the body (and its cap) is slightly see-through, so a note lying under or over another
    // on the same key shows through, and the overlap reads denser. Rim, names and marks stay
    // opaque; slide and muted notes are lighter already.
    const ImU32 body = n.slide || n.muted ? fill : theme::withAlpha(fill, noteBodyAlpha(P.dark));
    // K-276: the drawn box on whole device pixels; a note ends 1 px before the next one starts.
    const pixelgrid::Span nsx = pixelgrid::block(x0, x1, dpp, 0.0f, 1.0f, 2.0f), nsy = pixelgrid::block(y, y + rowH, dpp);
    const ImVec2 n0(nsx.x0, nsy.x0), n1(nsx.x1, nsy.x1);
    // K-112c: Live's notes are flat square bars with a dark rim (no cap, no top light), as every ref.
    const float nr = squareNotes ? 0.0f : theme::kRadiusSm;
    dl->AddRectFilled(n0, n1, body, nr);
    if (n.muted && x1 - x0 > 6) dl->AddLine(ImVec2(n0.x + 2, n1.y - 2), ImVec2(n1.x - 2, n0.y + 2), theme::withAlpha(theme::rgb(theme::role::kMute), 0.7f), 1.2f);
    if (x1 - x0 > 10 && rowH >= 9 && !squareNotes && !velColours)
      dl->AddRectFilled(n0, ImVec2(std::min(n1.x, n0.x + 4), n1.y), theme::mix(body, P.ink, 0.25f), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    if (!squareNotes && !velColours) dl->AddLine(ImVec2(n0.x + 1, n0.y + 0.5f), ImVec2(n1.x - 1, n0.y + 0.5f), theme::withAlpha(P.paper, 0.35f));
    if (velColours && x1 - x0 > 6 && rowH >= 7) {  // K-112D (audit G1): the reference's velocity line, as long as the note is loud
      const float ly = std::round((n0.y + n1.y) * 0.5f);
      dl->AddLine(ImVec2(n0.x + 2, ly), ImVec2(n0.x + 2 + (n1.x - n0.x - 4) * vel, ly), theme::withAlpha(P.paper, 0.8f), 1.0f);
    }
    {  // K-276: the rim stroked wholly inside the note (a selected one reached into the gap)
      const float rimIn = pixelgrid::strokeIn(sel ? 1.5f : 1.0f);
      dl->AddRect(ImVec2(n0.x + rimIn, n0.y + rimIn), ImVec2(n1.x - rimIn, n1.y - rimIn), sel ? P.hot : theme::withAlpha(P.ink, liveNotes ? 0.75f : 0.55f), nr,
                  sel ? 1.5f : 1.0f, 0);
    }
    if (n.slide && x1 - x0 > 8) {
      float h = (rowH - 2) * 0.5f, cy = y + rowH * 0.5f, tx = x1 - 3;
      dl->AddTriangleFilled(ImVec2(tx - h, cy - h * 0.8f), ImVec2(tx, cy), ImVec2(tx - h, cy + h * 0.8f), theme::contrastText(fill));
    }
    if (n.porta) dl->AddCircleFilled(ImVec2(x0 + 4, y + rowH * 0.5f), std::max(1.5f, rowH * 0.14f), theme::contrastText(fill), 8);
    if (x1 - x0 > 28 && rowH >= (velColours ? 16.0f : 11.0f)) {  // K-112D: Smooth names notes only on tall rows (audit G1). K-28: drawn after the bend lines (labelPass below), so a line never hides a name
      const std::string* kn = keyLabel(n.pitch);
      labels.push_back({i, x0, x1, y, fill, kn ? *kn : noteName(n.pitch)});
    }
    if (hovered && io.MousePos.x >= x0 && io.MousePos.x < x1 && io.MousePos.y >= y && io.MousePos.y < y + rowH &&
        io.MousePos.y < gridMax.y && io.MousePos.x > gridMin.x && !liveEnvelope) {
      hoverNote = static_cast<int>(i);
      hoverEdge = io.MousePos.x > x1 - std::min(8.0f, (x1 - x0) * 0.35f);
    }
  }
  // Slide notes: a faint arrow from each note the slide moves (sounding at its start, same colour
  // unless slides take every colour) to the slide's pitch, so it is clear what a slide acts on.
  {
    const bool anyColour = a.doc.song().slidesAnyColour;
    const ImU32 arrowCol = theme::withAlpha(P.text, 0.35f);
    for (size_t i = 0; i < notes.size(); ++i) {
      const Note& s = notes[i];
      if (!s.slide || s.muted) continue;
      const NoteBox& sb = noteBoxes[i];
      if (sb.x0 < gridMin.x - 4 || sb.x0 > gridMax.x + 4) continue;
      const float sy = sb.y + rowH * 0.5f;
      for (size_t j = 0; j < notes.size(); ++j) {
        const Note& n = notes[j];
        if (j == i || n.slide || n.muted || n.pitch == s.pitch) continue;
        if (!anyColour && n.color != s.color) continue;
        if (n.start >= s.start || n.start + n.length <= s.start) continue;
        const float ny = noteBoxes[j].y + rowH * 0.5f, x = sb.x0;
        const float dir = sy < ny ? -1.0f : 1.0f, h = std::max(2.0f, std::min(4.0f, rowH * 0.3f));
        dl->AddLine(ImVec2(x, ny), ImVec2(x, sy - dir * h), arrowCol, 1.0f);
        dl->AddTriangleFilled(ImVec2(x - h * 0.7f, sy - dir * h), ImVec2(x + h * 0.7f, sy - dir * h), ImVec2(x, sy), arrowCol);
      }
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
      L.line = P.dark ? theme::mix(base, P.paper, sel ? 0.86f : 0.74f) : theme::mix(base, P.ink, 0.62f);
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
          dl->AddRect(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), L.line, 1.5f, 1.5f, 0);
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
      ImGui::PushFont(livelook::active(a) ? a.fontRegular : a.fontMono, std::min(11.0f, rowH - 3));  // K-112c (round 28): sans in Live, as the refs' labels
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
  // Playhead (#70: song mode too). N47-06: in Clips also a launched clip's place in its notes (the clip launch mode).
  double liveTick = 0;
  const bool livePh = a.engine.playing() && a.engine.mode() == PlayMode::Live && livelook::clipPlayhead(a, patId, liveTick);
  if (livePh) {
    const float x = xOf(static_cast<Tick>(liveTick));
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), P.playhead, 1.5f);
  }
  if (playPos >= 0) {
    float x = xOf(static_cast<Tick>(playPos));
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), P.playhead, 1.5f);
  }
  // Step record cursor: where the next played chord goes (a click on the ruler or Alt+click in the
  // grid moves it, Left / Right step it).
  if (a.stepRecord && !a.engine.playing()) {
    // Record-red cursor: the step the next chord fills is tinted, a 2 px line
    // marks its start and a flag hangs from the top edge. GitHub #71: a chord held while Right
    // lengthens it tints the length it will get.
    float x = std::round(xOf(a.stepRecorder.cursor));
    const Tick stepG = std::max<Tick>(1, a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(pm, a.piano.snap));
    const float xs = std::round(xOf(a.stepRecorder.cursor + stepG * (1 + a.stepRecorder.extraSteps())));
    dl->AddRectFilled(ImVec2(x, gridMin.y), ImVec2(std::max(x + 2, xs), gridMax.y), theme::withAlpha(P.danger, 0.10f));
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), theme::withAlpha(P.danger, 0.9f), 2.0f);
    dl->AddRectFilled(ImVec2(x, gridMin.y), ImVec2(x + 9, gridMin.y + 8), P.danger, 1.0f);
    dl->AddTriangleFilled(ImVec2(x, gridMin.y + 8), ImVec2(x + 9, gridMin.y + 8), ImVec2(x, gridMin.y + 13), P.danger);
    if (ImGui::IsWindowHovered() && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= gridMin.x) {
      Tick g = a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(pm, a.piano.snap);
      double t = (io.MousePos.x - xOf(0)) / std::max(1e-6f, xOf(1) - xOf(0));
      a.stepRecorder.cursor = std::max<Tick>(0, snapDown(static_cast<Tick>(t), std::max<Tick>(1, g)));
    }
  }
  // Selection rectangle / slice line.
  if (g.kind == Gesture::Select && active)
    dl->AddRectFilled(g.start, io.MousePos, theme::withAlpha(P.accent, 0.12f)),
        dl->AddRect(g.start, io.MousePos, P.accent, 0, 1.5f, 0);
  if (g.kind == Gesture::Slice && active) {  // J-67: drawn on the snapped cut, gliding between grid lines (smooth motion)
    const Tick cut = std::max<Tick>(0, snapNearest(tickAt(io.MousePos.x), snapG));
    const float x = std::round(gridMin.x + static_cast<float>(scrollui::glide(0x50525343ull, static_cast<double>(cut), snapG > 1, 0.5)) * st.pxPerTick - st.scrollX);
    dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), P.danger, 1.5f);
  }
  if (g.kind == Gesture::Zoom && active) {
    const ImU32 zc = theme::rgb(theme::role::kZoom);
    dl->AddRectFilled(ImVec2(g.start.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), theme::withAlpha(zc, 0.10f));
    dl->AddRect(ImVec2(g.start.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), zc, 0, 1.2f, 0);
  }
  if (g.kind == Gesture::Play && active) dl->AddLine(ImVec2(io.MousePos.x, gridMin.y), ImVec2(io.MousePos.x, gridMax.y), theme::rgb(theme::role::kPlay), 1.5f);
  dl->PopClipRect();

  // ---------------- keyboard ----------------
  {
    dl->PushClipRect(ImVec2(keysX0, gridMin.y), ImVec2(gridMin.x, gridMax.y), true);
    // K-112D (audit G2): Smooth's keyboard as the reference's: near-white keys with grey hairline
    // dividers, near-black keys at 60 % of the width with a soft sheen (144, 127, 152).
    const ImU32 white = lgKeys ? theme::rgb(0xE8E8E8) : P.keyWhite;
    const ImU32 whiteEdge = lgKeys ? theme::rgb(0x9A9A9A) : theme::mix(P.keyWhite, P.ink, 0.28f);
    const ImU32 black = lgKeys ? theme::rgb(0x1A1A1A) : P.keyBlack;
    // K-112 (Live): a column of octave names (C1, C2, ...) left of a narrow keyboard.
    const bool liveKeys = livelook::active(a);
    const float nameW = liveKeys ? 34.0f : 0.0f;
    if (liveKeys) {
      dl->AddRectFilled(ImVec2(keysX0, gridMin.y), ImVec2(keysX0 + nameW, gridMax.y), P.surface800);
      dl->AddLine(ImVec2(keysX0 + nameW - 1, gridMin.y), ImVec2(keysX0 + nameW - 1, gridMax.y), P.line);
      for (int p = 0; p < 128; p += fold ? 1 : 12) {  // folded: every row shows its note's name
        const float y = yOf(p);
        if (y > gridMax.y || y + rowH < gridMin.y) continue;
        const float by = std::round(y + rowH);  // the octave's (or row's) bottom edge, as the grid's line
        dl->AddLine(ImVec2(keysX0, by), ImVec2(keysX0 + nameW - 1, by), P.line);
        const float fs = theme::kFontLabel;
        // K-112c (round 28): the octave names in the sans face, as the refs (058, 198, 038).
        const float ty = fold ? theme::textTop(a.fontRegular, fs, y, rowH) : std::round(by - fs - 3);
        if (!fold || rowH >= fs) dl->AddText(a.fontRegular, fs, ImVec2(keysX0 + 5, ty), P.textDim, noteName(p).c_str());
      }
    }
    // K-112c: Live's keys are plates a row tall: black keys run the keyboard's whole width (every ref).
    const float kx0 = keysX0 + nameW, kx1 = gridMin.x - 1, blackW = liveKeys ? kx1 - kx0 : std::round((kx1 - kx0) * 0.6f);
    const bool gliss = g.kind == Gesture::Keys;  // K-21
    const int hoverPitch = !gliss && hovered && io.MousePos.x >= keysX0 && io.MousePos.x < gridMin.x && io.MousePos.y >= gridMin.y && io.MousePos.y < gridMax.y
                               ? pitchAt(io.MousePos.y)
                               : -1;
    // White keys: light face shading to a darker tip (left), like a keyboard seen from above.
    if (fold) dl->AddRectFilled(ImVec2(kx0, gridMin.y), ImVec2(kx1, gridMax.y), P.surface800);  // below the last folded row
    else if (liveKeys)
      dl->AddRectFilled(ImVec2(kx0, gridMin.y), ImVec2(kx1, gridMax.y), white);
    else
      dl->AddRectFilledMultiColor(ImVec2(kx0, gridMin.y), ImVec2(kx1, gridMax.y), theme::mix(white, P.ink, lgKeys ? 0.07f : 0.12f), white, white,
                                  theme::mix(white, P.ink, lgKeys ? 0.07f : 0.12f));
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
    if (fold)  // K-112 (Live): folded rows are flat key plates, white or black, lit and hovered as keys
      for (int p : foldPitch) {
        const float y = yOf(p);
        if (y > gridMax.y || y + rowH < gridMin.y) continue;
        const float lv = keyLevel(p);
        const ImU32 face = isBlack(p) ? black : white;
        dl->AddRectFilled(ImVec2(kx0, y), ImVec2(kx1, y + rowH), lv > 0.004f ? litTint(face, lv) : hoverPitch == p ? P.accent : face);
        dl->AddLine(ImVec2(kx0, std::round(y + rowH)), ImVec2(kx1, std::round(y + rowH)), whiteEdge);
      }
    for (int p = 0; p < 128 && !fold; ++p) {
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
    for (int p = 0; p < 128 && !fold; ++p) {
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
        if (lgKeys) {  // K-112D: a shadow under the key, then the key, lighter toward its front edge
          const float by0 = std::round(y) + 0.5f, by1 = std::round(y + rowH) - 0.5f;
          dl->AddRectFilled(ImVec2(kx0, by0 + 1), ImVec2(kx0 + blackW + 1, by1 + 1), theme::withAlpha(P.ink, 0.25f), 2.0f, ImDrawFlags_RoundCornersRight);
          if (tint) dl->AddRectFilled(ImVec2(kx0, by0), ImVec2(kx0 + blackW, by1), tint, 2.0f, ImDrawFlags_RoundCornersRight);
          else dl->AddRectFilledMultiColor(ImVec2(kx0, by0), ImVec2(kx0 + blackW, by1), black, theme::rgb(0x343434), theme::rgb(0x343434), black);
          if (!tint && rowH >= 6) dl->AddRectFilled(ImVec2(kx0 + blackW - 5, by0 + 1), ImVec2(kx0 + blackW - 1, by1 - 1), theme::withAlpha(P.paper, 0.10f), 1.0f);
        } else {
          dl->AddRectFilled(ImVec2(kx0, y), ImVec2(kx0 + blackW, y + rowH), tint ? tint : black, liveKeys ? 0.0f : 2.0f, ImDrawFlags_RoundCornersRight);
        }
        if (!tint && rowH >= 6 && !liveKeys && !lgKeys)  // highlight along the black key's top face
          dl->AddRectFilled(ImVec2(kx0 + 2, y + 1.5f), ImVec2(kx0 + blackW - 3, y + 3), theme::withAlpha(P.paper, 0.14f), 1.0f);
      }
      if (((p % 12 == 0 && !liveKeys) || st.keyNames) && rowH >= 9 && !keyLabel(p)) {
        float fs = std::min(theme::kFontLabel, rowH - 1);
        std::string nn = noteName(p);
        // The Tracks layout: the octave's name in the bold face, near the ink, at the key's right end (144, 127).
        ImFont* const kf = lgKeys ? a.fontBold : a.fontMono;
        ImVec2 ts = kf->CalcTextSizeA(fs, 1e9f, 0, nn.c_str());
        dl->AddText(kf, fs, ImVec2(kx1 - ts.x - (lgKeys ? 3 : 5), std::round(y + (rowH - ts.y) * 0.5f)),
                    lv <= 0.004f && hoverPitch == p ? theme::contrastText(P.accent) : lgKeys ? theme::rgb(0x5A5A5A) : theme::mix(P.keyWhite, P.ink, 0.6f), nn.c_str());
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
    ImVec2 r0(gridMin.x, rulerTop), r1(gridMax.x, origin.y + rulerH);
    dl->AddRectFilled(ImVec2(origin.x, origin.y), r1, P.dark ? P.field : P.surface950);
    dl->AddLine(ImVec2(origin.x, r1.y - 1), ImVec2(r1.x, r1.y - 1), P.dark ? P.ink : P.lineStrong);
    dl->PushClipRect(r0, r1, true);
    if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) {
      // K-112b: the edited pattern as a band along the ruler's foot, in its colour, with its name
      // (solid, as every reference editor shows the region: a coloured bar with its name)
      const float bx0 = snapX(0), bx1 = snapX(patLen), by0 = r1.y - 13, by1 = r1.y - 1;  // K-276: on the notes' edges
      // K-112D (audit G4): the reference's strip is a tint of the region's colour with a play glyph and
      // the name in white (144), not a solid bar with dark text.
      const ImU32 rc = theme::content(pat->color);
      // R09 (visual gap 8): a light tint of the region's colour with dark type, as 144 and 304 draw it.
      const ImU32 pc = lgDark ? theme::mix(rc, theme::rgb(0xFFFFFF), 0.42f) : theme::mix(P.surface700, rc, 0.85f);
      const ImU32 ptx = theme::contrastText(pc);
      dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), pc, 2.0f);
      const float nf = theme::kFontLabel - 1;
      float nx = std::max(bx0, r0.x) + 4;
      if (lgDark) {  // the play glyph in a small outlined box, as the reference's strip starts
        const float gy = std::round((by0 + by1) * 0.5f);
        dl->AddRect(ImVec2(nx, gy - 4), ImVec2(nx + 9, gy + 4), theme::withAlpha(ptx, 0.8f), 1.5f);
        dl->AddTriangleFilled(ImVec2(nx + 3, gy - 2), ImVec2(nx + 3, gy + 2), ImVec2(nx + 6.5f, gy), ptx);
        nx += 14;
      }
      theme::textEllipsis(dl, a.fontBold, nf, ImVec2(nx, std::round(by0 + (by1 - by0 - nf) * 0.5f) - 1),
                          std::max(0.0f, bx1 - nx - 4), ptx, pat->name.c_str());
    }
    const Tick bar = std::max<Tick>(1, pm.baseBar()), beat = std::max<Tick>(1, pm.baseBeat());
    const Tick labelGrid = timeline::visibleGrid(pm, st.pxPerTick, 64.0);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(r0.x)), visGrid); xOf(t) < r1.x; t += visGrid) {
      const pixelgrid::Span l = pixelgrid::line(xOf(t), dpp);  // K-276: on the grid line's pixel
      const float x = l.x0;
      bool major = t % labelGrid == 0;
      float len = major ? 10.0f : t % beat == 0 ? 6.0f : 3.0f;
      if (major && livelook::active(a))  // K-112c: the numbered tick at the number's left, from the top (the refs)
        dl->AddLine(ImVec2(x + 0.5f, rulerTop + numDy), ImVec2(x + 0.5f, rulerTop + numDy + 12), P.textFaint);
      else
        dl->AddRectFilled(ImVec2(l.x0, r1.y - braceH - len), ImVec2(l.x1, r1.y - braceH - 1), major ? P.lineStrong : P.line);
      if (major) {
        std::string lbl = timeline::rulerLabel(pm, t, labelGrid);
        if (lgKeys && t % bar == 0) lbl = std::to_string(t / bar + 1);  // R02-18: a bar reads as its number ("2", not "2.1"), as the reference's
        // K-112b (checkpoint 25): the Tracks layout's numbers in the interface face at caption size, as the
        // tracks area's ruler and 144, 152 set them.
        if (lgKeys) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 4, rulerTop + numDy - 1), t % bar == 0 ? P.text : P.textFaint, lbl.c_str());
        else dl->AddText(livelook::numFont(a), theme::kFontLabel, ImVec2(x + 4, rulerTop + numDy), t % bar == 0 ? P.text : P.textFaint, lbl.c_str());
      }
    }
    // position marker cap (pattern mode: the pattern position; #70 song mode: where a clip of this
    // pattern plays; N47-06 Clips: a launched clip's place)
    const double capPos = livePh ? liveTick : a.engine.mode() == PlayMode::Pattern ? a.engine.position() : rollPlayPos(a, patId);
    if (capPos >= 0) {
      float px = snapX(static_cast<Tick>(capPos));
      dl->AddTriangleFilled(ImVec2(px - 5, r1.y - braceH - 9), ImVec2(px + 5, r1.y - braceH - 9), ImVec2(px, r1.y - braceH - 1), P.playhead);
    }
    float xe = snapX(patLen);
    if (braceH > 0) {
      // K-112 (Live): the loop brace: a bar in the pattern's colour from its start to its end, with a
      // flag at each end, on its own row under the bar numbers.
      const float by0 = r1.y - braceH + 1, by1 = r1.y - 2, xs = snapX(regA);
      if (regClip != kNoId) xe = snapX(regB);
      // N48-12: the brace is the clip's Start to End: its ends drag them, its middle moves both.
      static int bDrag = 0;  // 1 the start, 2 the end, 3 the whole region
      static Tick bA = 0, bB = 0;
      static float bX = 0;
      if (regClip != kNoId) {
        const float mx = io.MousePos.x;
        const bool inBrace = hovered && io.MousePos.y >= by0 - 3 && io.MousePos.y < r1.y + 1 && mx >= xs - 6 && mx < xe + 6;
        const int part = std::fabs(mx - xs) <= 5.0f ? 1 : std::fabs(mx - xe) <= 5.0f ? 2 : 3;
        if (inBrace || bDrag) ImGui::SetMouseCursor((bDrag ? bDrag : part) == 3 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
        if (inBrace && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) bDrag = part, bA = regA, bB = regB, bX = mx;
        if (bDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          braceTook = true;
          const Tick six = std::max<Tick>(1, s.beatTicks() / 4);
          const double dt = static_cast<double>(mx - bX) / std::max(1e-6, static_cast<double>(st.pxPerTick));
          const Tick d = static_cast<Tick>(std::llround(dt / static_cast<double>(six))) * six;
          Tick na = bA, nb = bB;
          if (bDrag == 1) na = std::clamp<Tick>(bA + d, 0, bB - six);
          else if (bDrag == 2) nb = std::max<Tick>(bA + six, bB + d);
          else na = std::max<Tick>(0, bA + d), nb = na + (bB - bA);
          if (na != regA || nb != regB)
            livelook::setClipRegion(a, regClip, na, nb, bDrag == 3 ? "move clip loop" : bDrag == 1 ? "clip start" : "clip end",
                                    0x4252414345000000ull ^ static_cast<uint64_t>(regClip) ^ (static_cast<uint64_t>(bDrag) << 40));
        } else if (bDrag) {
          bDrag = 0;
        }
      }
      const ImU32 bc = theme::rgb(pat->color);
      dl->AddRectFilled(ImVec2(xs, by0), ImVec2(xe, by1), theme::withAlpha(bc, 0.85f));
      dl->AddTriangleFilled(ImVec2(xs, by0), ImVec2(xs + 5, (by0 + by1) * 0.5f), ImVec2(xs, by1), theme::contrastText(bc));
      dl->AddTriangleFilled(ImVec2(xe, by0), ImVec2(xe - 5, (by0 + by1) * 0.5f), ImVec2(xe, by1), theme::contrastText(bc));
      if (hovered && io.MousePos.y >= by0 - 2 && io.MousePos.y < by1 + 2 && io.MousePos.x >= xs && io.MousePos.x < xe)
        a.setHint(i18n::tr(regClip != kNoId ? "The clip's Start and End: drag an end to move it, the middle to move both"
                                             : "The pattern's loop: its length is set in the clip column"));
    } else {
      dl->AddTriangleFilled(ImVec2(xe - 5, r1.y - 9), ImVec2(xe + 5, r1.y - 9), ImVec2(xe, r1.y - 2), P.textFaint);
    }
    dl->PopClipRect();
    // Corner above the keyboard: pattern name.
    dl->AddRectFilled(origin, ImVec2(gridMin.x, r1.y), P.dark ? P.field : P.surface950);
    dl->AddLine(ImVec2(origin.x, r1.y - 1), ImVec2(gridMin.x, r1.y - 1), P.dark ? P.ink : P.lineStrong);
    if (!lgKeys)  // K-112D: the reference leaves the corner over its keyboard empty (144, 152)
      theme::textEllipsis(dl, livelook::numFont(a), theme::kFontLabel, ImVec2(keysX0 + 8, rulerTop + numDy), keyW - 12, P.textFaint,
                          livelook::active(a) ? i18n::tr("Notes") : "NOTES");  // K-112c: sans, sentence case in Live  // J-128: on the bar numbers' line
  }

  // ---------------- view scrollers ----------------
  // Above the ruler: the time, with every note of the pattern as a short mark at
  // its pitch (the lowest to the highest note fill the bar's height) and the
  // playback position. Left of the keyboard: the keys, with a mark at each
  // pitch in use. Thumbs and ends as in the playlist.
  using scrollbarui::Part;
  const scrollbarui::Layout hBar = scrollbarui::layout(ImVec2(gridMin.x, origin.y + 1), ImVec2(gridMax.x, rulerTop - 1), false, gridW,
                                                       maxScrollX + gridW, st.scrollX, scrollbarui::kThumbMinH);
  const scrollbarui::Layout vBar =
      scrollbarui::layout(ImVec2(origin.x, gridMin.y), ImVec2(keysX0 - 2, gridMax.y), true, gridH, rowsN * rowH, st.scrollY, scrollbarui::kThumbMinV);
  const bool barsFree = g.kind == Gesture::None || !active;
  const Part hOver = hovered && barsFree ? scrollbarui::hit(hBar, io.MousePos) : Part::None;
  const Part vOver = hovered && barsFree && hOver == Part::None ? scrollbarui::hit(vBar, io.MousePos) : Part::None;
  {
    auto lit = [](const scrollbarui::Drag& d, Part over) { return d.zooming() ? d.part : d.held() ? Part::None : over; };
    dl->AddRectFilled(ImVec2(origin.x, gridMin.y), ImVec2(keysX0, gridMax.y), P.dark ? P.well : P.surface950);
    scrollbarui::draw(dl, hBar, hOver != Part::None || hBarDrag.held(), lit(hBarDrag, hOver));
    scrollbarui::draw(dl, vBar, vOver != Part::None || vBarDrag.held(), lit(vBarDrag, vOver));
    const ImU32 markCol = theme::withAlpha(theme::rgb(chan->color), 0.9f), selCol = P.hot;
    int lo = 127, hi = 0;
    std::array<bool, 128> used{};
    for (const Note& n : notes) {
      const int p = std::clamp(n.pitch, 0, 127);
      lo = std::min(lo, p), hi = std::max(hi, p);
      used[static_cast<size_t>(p)] = true;
    }
    dl->PushClipRect(hBar.p0, hBar.p1, true);
    const float my0 = hBar.p0.y + 3, my1 = hBar.p1.y - 4;  // the marks' tops: lowest note at my1, highest at my0
    for (size_t i = 0; i < notes.size(); ++i) {
      const Note& n = notes[i];
      const float x0 = std::round(hBar.toRail(static_cast<float>(n.start) * st.pxPerTick));
      const float x1 = std::max(x0 + 1.0f, std::round(hBar.toRail(static_cast<float>(n.start + std::max<Tick>(1, n.length)) * st.pxPerTick)) - 1.0f);
      const float f = hi > lo ? static_cast<float>(std::clamp(n.pitch, 0, 127) - lo) / static_cast<float>(hi - lo) : 0.5f;
      const float y = std::round(my1 - f * (my1 - my0));
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 2), st.selected.count(i) ? selCol : markCol);
    }
    if (const double hp = rollPlayPos(a, patId); hp >= 0) {
      const float px = std::round(hBar.toRail(static_cast<float>(hp) * st.pxPerTick));
      dl->AddLine(ImVec2(px, hBar.p0.y), ImVec2(px, hBar.p1.y), P.playhead, 1.0f);
    }
    dl->PopClipRect();
    dl->PushClipRect(vBar.p0, ImVec2(vBar.p1.x + 2, vBar.p1.y), true);
    for (int p = 0; p < 128; ++p) {
      if (!used[static_cast<size_t>(p)]) continue;
      const float y = std::round(vBar.toRail((static_cast<float>(fold ? foldRow[static_cast<size_t>(p)] : 127 - p) + 0.5f) * rowH));
      dl->AddRectFilled(ImVec2(vBar.p0.x + 3, y), ImVec2(vBar.p1.x - 1, y + 1), markCol);
    }
    dl->PopClipRect();
  }

  // ---------------- velocity lane ----------------
  // K-112D: a hidden lane (Smooth) draws nothing: its drawing goes into an empty clip.
  const bool laneHidden = velH <= 0.0f;
  if (laneHidden) dl->PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
  const ImVec2 vMin(gridMin.x, gridMax.y + 8), vMax(gridMax.x, std::max(gridMax.y + 8, origin.y + avail.y - 4));
  // Event lane: dark well under the grid, separated by a raised splitter bar.
  dl->AddRectFilled(ImVec2(origin.x, gridMax.y), ImVec2(gridMax.x, origin.y + avail.y), P.dark ? P.field : P.surface950);
  dl->AddRectFilled(ImVec2(origin.x, gridMax.y), ImVec2(gridMax.x, gridMax.y + 3), P.dark ? P.surface800 : P.lineStrong);
  dl->AddLine(ImVec2(origin.x, gridMax.y + 3), ImVec2(gridMax.x, gridMax.y + 3), P.dark ? P.ink : P.lineStrong);
  // K-112 (Live): the velocity lane's scale (127, 64, 1) at the right of its header column.
  const bool liveScale = livelook::active(a) && lane == Lane::Velocity && pianoAutomationLane().owner == kNoId && vMax.y - vMin.y >= 2 * theme::kFontLabel - 2;  // round 25: kept in the 32 px lane
  if (liveScale) {
    const float fs = theme::kFontLabel;
    const struct { const char* t; float f; } marks[] = {{"127", 1.0f}, {"64", 0.5f}, {"1", 0.0f}};
    const bool slim = vMax.y - vMin.y < 3 * theme::kFontLabel;  // round 25: a slim lane shows 127 and 1 only (041, 112)
    for (const auto& m : marks) {
      if (slim && m.f == 0.5f) continue;
      const float y = vMax.y - (vMax.y - vMin.y) * m.f;
      const ImVec2 ts = livelook::numFont(a)->CalcTextSizeA(fs, 1e9f, 0, m.t);
      const float ty = std::clamp(std::round(y - ts.y * 0.5f), vMin.y - 4, vMax.y - ts.y + 2);
      dl->AddText(livelook::numFont(a), fs, ImVec2(gridMin.x - 6 - ts.x, ty), P.textFaint, m.t);
      dl->AddLine(ImVec2(gridMin.x - 4, std::round(y)), ImVec2(gridMin.x, std::round(y)), P.line);
    }
  }
  bool chipTook = false;
  if (liveEnvelope) {
    // K-112 (Live): the notes stay visible under a veil, the envelope is drawn and edited across
    // the whole grid, and the band below holds the choosers and the draw-curve chip.
    dl->AddRectFilled(gridMin, gridMax, theme::withAlpha(P.dark ? P.surface950 : P.paper, 0.45f));
    const bool envHover = hovered && !laneSplit.on && io.MousePos.y >= gridMin.y;
    drawPatternAutomationLane(
        a, dl, ImVec2(gridMin.x, gridMin.y + 6), ImVec2(gridMax.x, gridMax.y - 6), patId,
        [&](double t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; },
        [&](float x) { return static_cast<double>((x - gridMin.x + st.scrollX) / st.pxPerTick); }, snapG, envHover, active, st.tool, true);
    const float fy0 = gridMax.y + 4, ch = 22, cy0 = std::round(fy0 + (origin.y + avail.y - fy0 - ch) * 0.5f);
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    chipTook = drawPatternLaneChoosers(a, dl, ImVec2(origin.x + 6, cy0), ImVec2(std::min(gridMax.x - 110, origin.x + 6 + 366), cy0 + ch), patId, chId,
                                       hovered, click);
    const float chipX = std::min(gridMax.x - 104, origin.x + 6 + 366 + 6);
    chipTook = drawLaneCurveChip(a, dl, ImVec2(chipX, cy0), ImVec2(chipX + 96, cy0 + ch), patId, hovered, click) || chipTook;
  }
  if (!liveEnvelope) {  // lane selector: the property shown and edited in the lane
    const ImVec2 l0(origin.x + 4, gridMax.y + 5), l1(gridMin.x - (liveScale ? 30 : 4), gridMax.y + 5 + theme::kFontLabel + 6);
    const bool lh = hovered && io.MousePos.x >= l0.x && io.MousePos.x < l1.x && io.MousePos.y >= l0.y && io.MousePos.y < l1.y;
    if (lh) dl->AddRectFilled(l0, l1, P.surface800, theme::kRadius);
    const std::string autoLabel = patternLaneLabel(a);  // J-5: pattern automation lane shown instead
    // K-112c (Live): the lane's name in the sans face and sentence case, as the refs label it.
    const bool liveL = livelook::active(a);
    static const char* const kLaneLive[] = {"Velocity", "Pan", "Release", "Fine pitch", "Mod X", "Mod Y", "Pressure", "Timbre"};
    const char* laneName = autoLabel.empty() ? (liveL ? i18n::tr(kLaneLive[static_cast<int>(lane)]) : kLaneShort[static_cast<int>(lane)]) : autoLabel.c_str();
    theme::textEllipsis(dl, livelook::numFont(a), theme::kFontLabel, ImVec2(l0.x + 4, l0.y + 3), l1.x - l0.x - 16, lh ? P.text : P.textFaint, laneName);
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(l1.x - 6, (l0.y + l1.y) * 0.5f), 9, lh ? P.text : P.textFaint);
  }
  // K-171: the automation lane's draw-curve chip under the lane name (when there is room).
  if (pianoAutomationLane().owner != kNoId && !liveEnvelope) {
    const float cy0 = gridMax.y + 5 + theme::kFontLabel + 6 + 4, cy1 = cy0 + theme::kFontLabel + 6;
    if (cy1 <= origin.y + avail.y - 3)
      chipTook = drawLaneCurveChip(a, dl, ImVec2(origin.x + 4, cy0), ImVec2(gridMin.x - 4, cy1), patId, hovered, ImGui::IsMouseClicked(ImGuiMouseButton_Left));
  }
  const bool bip = laneBipolar(lane);
  for (float f : {0.25f, 0.5f, 0.75f, 1.0f}) {
    if (liveEnvelope) break;
    float y = std::round(vMax.y - (vMax.y - vMin.y) * f);
    dl->AddLine(ImVec2(vMin.x, y), ImVec2(vMax.x, y), f == 1.0f || (bip && f == 0.5f) ? P.line : P.gridStep);
  }
  const bool autoLane = pianoAutomationLane().owner != kNoId;
  if (autoLane && !liveEnvelope)
    drawPatternAutomationLane(
        a, dl, vMin, vMax, patId, [&](double t) { return gridMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; },
        [&](float x) { return static_cast<double>((x - gridMin.x + st.scrollX) / st.pxPerTick); }, snapG, hovered && !laneSplit.on, active, st.tool);
  dl->PushClipRect(ImVec2(vMin.x, vMin.y - 4), vMax, true);
  const bool liveVel = livelook::active(a);
  for (size_t i = 0; i < notes.size() && !autoLane; ++i) {
    const Note& n = notes[i];
    if (exprLane(lane)) {  // K-99: the note's pressure or timbre line over its length
      const ExprDim d = laneDim(lane);
      const float span = vMax.y - vMin.y;
      const float x0 = snapX(n.start), x1 = snapX(n.start + n.length);
      if (x1 < vMin.x - 2 || x0 > vMax.x + 2) continue;
      const bool lsel = st.selected.count(i) > 0;
      const ImU32 c = lsel ? P.hot : theme::mix(groupColor(n.color, noteCol), P.paper, 0.15f);
      const bool has = !exprlane::laneOf(n, d).empty();
      const int steps = std::clamp(static_cast<int>((x1 - x0) / 3.0f), 1, 400);
      ImVec2 pts[401];
      for (int k = 0; k <= steps; ++k) {
        const double t = static_cast<double>(n.length) * k / steps;
        pts[k] = ImVec2(x0 + (x1 - x0) * static_cast<float>(k) / static_cast<float>(steps),
                        std::round(vMax.y - span * exprlane::valueAt(n, d, t)) + 0.5f);
      }
      const float yBase = std::round(vMax.y - span * (d == ExprDim::Timbre ? 0.5f : 0.0f)) + 0.5f;
      if (has) {  // a soft fill between the line and its neutral value
        for (int k = 0; k < steps; ++k)
          dl->AddQuadFilled(ImVec2(pts[k].x, yBase), pts[k], pts[k + 1], ImVec2(pts[k + 1].x, yBase), theme::withAlpha(c, 0.12f));
      }
      dl->AddPolyline(pts, steps + 1, has ? c : theme::withAlpha(c, 0.35f), 0, has ? 1.5f : 1.0f);
      dl->AddRectFilled(ImVec2(x0, std::min(pts[0].y, yBase) - 0.5f), ImVec2(x0 + 2, std::max(pts[0].y, yBase) + 0.5f), c);  // the note's start
      continue;
    }
    // Q-10: the bar glides to its new height (display only; the note already has the value).
    const uint64_t barKey = (static_cast<uint64_t>(chId) << 40) ^ (static_cast<uint64_t>(n.start) << 12) ^ (static_cast<uint64_t>(n.pitch) << 4) ^
                            static_cast<uint64_t>(lane);
    const float span = vMax.y - vMin.y, v = static_cast<float>(scrollui::glideBar(barKey, laneValue(n, lane)));
    float x = snapX(n.start), yv = vMax.y - span * v, y0 = bip ? vMax.y - span * 0.5f : vMax.y;  // K-276: the note's own edge
    const bool lsel = st.selected.count(i) > 0;
    ImU32 c = lsel ? P.hot : theme::mix(groupColor(n.color, noteCol), P.paper, 0.15f);
    // Lollipop stem: a translucent bar to the note's length, a solid stem and a round head.
    float xe = snapX(n.start + n.length);
    if (liveVel) {
      // K-112c (round 21): Live draws a velocity as every ref's lane does (058, 038, 041, 112): a line at
      // its height for the note's length and a hollow ring at the note's start, no stem.
      const float ly = std::round(yv) + 0.5f;
      // Round 22: a note starting at the lane's left edge keeps a whole ring, drawn just inside the
      // edge (the lane never draws over its scale); its line still starts at the note, level with the roll.
      const float rx = x < vMin.x + 4.5f && x > vMin.x - 4.5f ? vMin.x + 4.5f : x + 0.5f;
      if (xe - x > 4) dl->AddLine(ImVec2(std::max(x + 4, rx + 3.5f), ly), ImVec2(xe - 1, ly), c, 1.5f);
      dl->AddCircleFilled(ImVec2(rx, ly), 3.5f, theme::mix(c, P.ink, 0.55f), 12);
      dl->AddCircle(ImVec2(rx, ly), 3.5f, c, 12, 1.5f);
      continue;
    }
    if (style.plainVelocity) {
      // K-106 cp22 (the Tracker): a plain bar, the note's length (1 px short of the next), filled to its
      // value with a solid edge at the value; no head.
      const float bx1 = std::max(x + 3, xe - 1), top = std::round(std::min(yv, y0)), bot = std::round(std::max(yv, y0));
      const float ey = std::clamp(std::round(yv), vMin.y, vMax.y - 1);
      dl->AddRectFilled(ImVec2(x, top), ImVec2(bx1, std::max(top + 1, bot)), theme::withAlpha(c, lsel ? 0.75f : 0.5f));
      dl->AddRectFilled(ImVec2(x, ey), ImVec2(bx1, ey + 1), c);
      continue;
    }
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
  laneGeo.scrollX = st.scrollX, laneGeo.scrollY = st.scrollY;
  laneGeo.gridX0 = gridMin.x, laneGeo.gridY0 = gridMin.y, laneGeo.gridX1 = gridMax.x, laneGeo.gridY1 = gridMax.y;
  laneGeo.hThumb0 = hBar.t0, laneGeo.hThumb1 = hBar.t1, laneGeo.hBarY = (hBar.p0.y + hBar.p1.y) * 0.5f;
  laneGeo.vThumb0 = vBar.t0, laneGeo.vThumb1 = vBar.t1, laneGeo.vBarX = (vBar.p0.x + 2 + vBar.p1.x) * 0.5f;
  if (laneHidden) dl->PopClipRect();

  // ---------------- interaction ----------------
  const ImVec2 m = io.MousePos;
  const bool overGrid = m.x >= gridMin.x && m.x < gridMax.x && m.y >= gridMin.y && m.y < gridMax.y;
  gReadPitch = hovered && overGrid ? pitchAt(m.y) : -1;  // K-112D
  if (gReadPitch >= 0) gReadTick = std::max<Tick>(0, tickAt(m.x));
  const bool inGrid = overGrid && !liveEnvelope;  // K-112 (Live): the envelope over the notes takes the grid
  const bool inKeys = m.x >= keysX0 && m.x < gridMin.x && m.y >= gridMin.y && m.y < gridMax.y;
  const bool inRuler = m.y >= rulerTop && m.y < gridMin.y && m.x >= gridMin.x;
  const bool inVel = m.y >= gridMax.y && m.x >= gridMin.x;
  const bool inLaneLabel = m.y >= gridMax.y && m.x >= origin.x && m.x < gridMin.x && !liveEnvelope;
  // K-25: the other notes a bend edit applies to: a selected note's edit gives every selected
  // note the same line (a chord bends as one); an unselected note bends alone.
  auto bendTargets = [&](size_t ni) {
    std::vector<size_t> out;
    if (!st.selected.count(ni)) return out;
    for (size_t j : st.selected)
      if (j != ni && j < notes.size() && !notes[j].slide) out.push_back(j);
    return out;
  };

  {  // the scrollers' hints: at once in the status bar; over an end also a tooltip after the hover delay
    const bool overBar = hOver != Part::None || vOver != Part::None;
    const Part end = hOver == Part::LowEnd || hOver == Part::HighEnd ? hOver : vOver == Part::LowEnd || vOver == Part::HighEnd ? vOver : Part::None;
    const bool v = vOver != Part::None;
    static double endSince = -1;
    static int endWas = 0;
    const int endNow = end == Part::None ? 0 : v ? 2 : 1;
    if (endNow != endWas) endWas = endNow, endSince = ImGui::GetTime();
    if (hBarDrag.zooming() || (end != Part::None && !v)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (vBarDrag.zooming() || end != Part::None) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (overBar && !hBarDrag.held() && !vBarDrag.held()) {
      if (end != Part::None) {
        a.setHint(v ? i18n::tr("Drag the end to zoom the key height (the other end stays put) \xC2\xB7 Alt+wheel: zoom at the pointer")
                    : i18n::tr("Drag the end to zoom the time (the other end stays put) \xC2\xB7 Ctrl+wheel: zoom at the pointer"));
        if (ImGui::GetTime() - endSince >= static_cast<double>(ImGui::GetStyle().HoverDelayNormal) && ImGui::BeginTooltip()) {
          ImGui::TextUnformatted(v ? i18n::tr("Drag to zoom the key height") : i18n::tr("Drag to zoom the time"));
          ImGui::EndTooltip();
        }
      } else {
        a.setHint(v ? i18n::tr("Drag to scroll through the keys \xC2\xB7 click beside the thumb to page \xC2\xB7 drag an end to zoom \xC2\xB7 "
                               "double-click: back to the notes")
                    : i18n::tr("Drag to scroll through the pattern \xC2\xB7 click beside the thumb to page \xC2\xB7 drag an end to zoom \xC2\xB7 "
                               "double-click: fit the pattern \xC2\xB7 wheel: scroll sideways"));
      }
    }
  }
  if (hovered && hOver == Part::None && vOver == Part::None) {
    if (inGrid) {
      if (hoverNote >= 0 && st.tool == Tool::Draw)
        ImGui::SetMouseCursor(hoverEdge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
      char buf[96];
      std::snprintf(buf, sizeof buf, "%s  ·  bar %lld", noteName(pitchAt(m.y)).c_str(),
                    static_cast<long long>(tickAt(m.x) / std::max<Tick>(1, pm.baseBar()) + 1));
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
      a.setHint(a.stepRecord && !a.engine.playing() ? "Click or drag to place the step record cursor (and the playback position)"
                                                    : "Click or drag to move the playback position (snaps to the grid, Alt = free)");
    }
  }

  bool lClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool rClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  {  // K-171: the splitter above the lane: drag to resize it, double-click for the automatic height
    const bool onSplit = hovered && !liveEnvelope && !laneHidden && m.y >= gridMax.y - 3 && m.y < gridMax.y + 4 && m.x >= origin.x && m.x < gridMax.x;
    if (onSplit || laneSplit.on) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (!laneSplit.on) a.setHint(i18n::tr("Drag to resize the lane \xC2\xB7 double-click: automatic height"));
    }
    if (onSplit && lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      a.settings.set("piano.laneHeight", 0.0);
      laneSplit.on = false;
      lClick = rClick = false;
    } else if (onSplit && lClick) {
      laneSplit.on = true;
      laneSplit.y0 = m.y;
      laneSplit.h0 = velH;
      lClick = rClick = false;
    }
    if (laneSplit.on) {
      if (active) {
        const float nh = std::round(std::clamp(laneSplit.h0 - (m.y - laneSplit.y0), laneMinH, laneMaxH));
        if (nh != velH) a.settings.set("piano.laneHeight", static_cast<double>(nh));
        lClick = rClick = false;
      } else {
        laneSplit.on = false;
      }
    }
  }
  {  // the view scrollers: drag a thumb, drag an end to zoom, click the rail to page, double-click to fit
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) hBarDrag = {}, vBarDrag = {};
    if (hBarDrag.part == Part::Thumb) st.scrollX = scrollbarui::thumbScroll(hBarDrag, hBar, m);
    if (vBarDrag.part == Part::Thumb) st.scrollY = scrollbarui::thumbScroll(vBarDrag, vBar, m);
    const bool barDbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (lClick && hOver != Part::None) {
      if (barDbl) {
        hBarDrag = {};
        st.fitRequest = true;
      } else if (const int page = scrollbarui::press(hBarDrag, hBar, hOver, m, st.pxPerTick, st.scrollX)) {
        st.scrollX = std::clamp(st.scrollX + static_cast<float>(page) * gridW * 0.9f, 0.0f, maxScrollX);
      }
      lClick = rClick = false;
    } else if (lClick && vOver != Part::None) {
      if (barDbl) {
        vBarDrag = {};
        st.centred = false;  // centred on the notes again (as at the first open)
      } else if (const int page = scrollbarui::press(vBarDrag, vBar, vOver, m, rowH, st.scrollY)) {
        st.scrollY = std::clamp(st.scrollY + static_cast<float>(page) * gridH * 0.9f, 0.0f, std::max(0.0f, rowsN * rowH - gridH));
      } else if (vBarDrag.zooming()) {
        vBarRow = smoothzoom::rowAnchorAt(keyTop.data(), 128, rowH, static_cast<double>(vBarDrag.edgePx + st.scrollY), vBarDrag.edgePx);
      }
      lClick = rClick = false;
    }
    if (hOver != Part::None || vOver != Part::None || hBarDrag.held() || vBarDrag.held()) lClick = rClick = false;
  }
  const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  static int propsNote = -1;
  // Q-3: Esc or a right-click during a note drag (move, Shift+drag copies,
  // resize) cancels it: notes back, copies gone, selection restored.
  if ((g.kind == Gesture::Move || g.kind == Gesture::Resize) && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      (keys::pressedHeld("piano.cancelDrag") || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    if (g.grouped) a.doc.cancelGroup();
    st.selected = g.selBefore;
    st.editSel = g.editSelBefore;
    previewOff(a, chId);
    noteAnim.clear();
    g = Gesture{};
    lClick = rClick = false;
    a.setHint(i18n::tr("Drag cancelled"));
    notes = rollNotes(a, patId, chId);
  }

  // N47-05: in Clips a double-click on an empty spot of the grid adds a note whatever the tool (as the
  // reference's pointer), by the Draw path below; the tool comes back after.
  const Tool toolKept = st.tool;
  // N48-09: did the first click of this double-click draw the note now under the pointer? (Then the
  // second click keeps it; a double-click on a note that was already there deletes it.)
  static bool clickDrewNote = false;
  const bool drewThisNote = clickDrewNote;
  if (lClick) clickDrewNote = false;
  const bool liveAdd = livelook::active(a) && lClick && dbl && inGrid && hoverNote < 0 && !io.KeyCtrl && st.tool != Tool::Draw && st.tool != Tool::Bend;
  if (liveAdd) st.tool = Tool::Draw;
  if (lClick || rClick) {
    if (!inVel && !(liveEnvelope && overGrid)) automationLaneBlur();  // K-171: the editing keys go back to the notes
    g = Gesture{};
    g.selBefore = st.selected;
    g.editSelBefore = st.editSel;
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
    } else if (inRuler && lClick && !braceTook) {
      if (a.engine.mode() != PlayMode::Pattern) {  // J-49 (J-77: scrubbing here keeps the rack where it is)
        a.engine.setMode(PlayMode::Pattern);
        a.rackReveal.quietSwitch(PlayMode::Pattern);
      }
      g.kind = Gesture::Scrub;  // the position follows the pointer until release
    } else if (inLaneLabel && lClick && !chipTook) {
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
      } else if (!dbl && !ctrl && hoverNote < 0 && ghostHover.index >= 0 && st.editChannels.count(ghostHover.ch) &&
                 (st.tool == Tool::Draw || st.tool == Tool::Select || st.tool == Tool::Paint) &&
                 (io.KeyShift || (st.editSel.count(ghostHover.ch) && st.editSel[ghostHover.ch].count(static_cast<size_t>(ghostHover.index))))) {
        // K-198 follow-up: Shift+click adds an edited channel's note to the selection (the target
        // stays); a press on a selected one drags the whole selection, every channel together
        // (Shift+drag copies it).
        const size_t gi = static_cast<size_t>(ghostHover.index);
        std::set<size_t>& es = st.editSel[ghostHover.ch];
        if (!es.count(gi)) {
          es.insert(gi);
        } else {
          const std::vector<Note> ens = rollNotes(a, patId, ghostHover.ch);
          if (gi < ens.size()) {
            g.kind = Gesture::Move;
            g.anchor = -1;
            g.anchorCh = ghostHover.ch, g.anchorIdx = gi;
            g.anchorPitch = ens[gi].pitch, g.anchorVel = ens[gi].velocity, g.anchorStart = ens[gi].start, g.anchorLen = ens[gi].length;
            g.multi = true;
            g.multiCopy = io.KeyShift;
            g.msel = unionSel(a, chId);
            g.mbase = multinote::lists(s, patId, g.msel);
            a.doc.beginGroup(g.multiCopy ? "copy notes" : "move notes");
            g.grouped = true;
          }
        }
      } else if (!dbl && hoverNote < 0 && ghostHover.ch != kNoId && st.editChannels.count(ghostHover.ch) &&
                 j31::takeEditedNoteAt(a, tickAt(m.x), ghostHover.pitch)) {
        // K-198: a note of an edited channel: its channel is the target now, the note selected.
      } else if (!dbl && hoverNote < 0 && ghostHover.ch != kNoId && j31::editGhostAt(a, tickAt(m.x), ghostHover.pitch)) {
        // J-31 editable ghosts: the click switched to the ghost's channel with the note selected.
      } else if (dbl && hoverNote < 0 && ghostHover.ch != kNoId) {
        // Multi-channel editing: double-clicking a ghost note edits its channel.
        a.selectChannel(ghostHover.ch);
        a.notify("Editing " + std::string(s.channel(ghostHover.ch) ? s.channel(ghostHover.ch)->name : "channel"));
      } else if (livelook::active(a) && dbl && toolKept == Tool::Draw && (hoverNote < 0 || drewThisNote)) {
        // N47-04: the first click of a double-click in Draw drew the note: the second adds nothing.
      } else if (livelook::active(a) && dbl && hoverNote >= 0) {
        // N47-05: a double-click on a note deletes it, as the reference's (its properties: right-click).
        std::vector<Note> ns = notes;
        ns.erase(ns.begin() + hoverNote);
        st.selected.clear();
        commit(a, patId, chId, ns, std::vector<bool>(ns.size(), false), "delete note", 0);
        notes = rollNotes(a, patId, chId);
        g.base = notes;
      } else if (dbl && hoverNote >= 0) {
        propsNote = hoverNote;
        ImGui::OpenPopup("##noteprops");
      } else if (ctrl || st.tool == Tool::Select) {
        g.kind = Gesture::Select;
        if (!io.KeyShift) st.selected.clear(), st.editSel.clear();
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
            if (!io.KeyShift || shiftCopy) st.selected.clear(), st.editSel.clear();
            st.selected.insert(static_cast<size_t>(hoverNote));
            j31::expandNoteGroups(a);  // J-31: grouped notes come along
            g.sel = selVector(a, notes.size());
          }
          g.kind = hoverEdge ? Gesture::Resize : Gesture::Move;
          g.copyPending = shiftCopy;
          if (multiSel(a)) {  // K-198 follow-up: the selection spans the edited channels: they all go along
            const Note& hn0 = notes[static_cast<size_t>(hoverNote)];
            g.multi = true;
            g.multiCopy = shiftCopy;
            g.copyPending = false;
            g.msel = unionSel(a, chId);
            g.mbase = multinote::lists(s, patId, g.msel);
            g.anchorPitch = hn0.pitch, g.anchorVel = hn0.velocity, g.anchorStart = hn0.start, g.anchorLen = hn0.length;
          }
          a.doc.beginGroup(shiftCopy ? "copy notes" : hoverEdge ? "resize notes" : "move notes");  // one undo step (empty: none)
          g.grouped = true;
          const Note& hn = notes[static_cast<size_t>(hoverNote)];
          previewOn(a, chId, hn.pitch, hn.velocity);
          st.lastLength = hn.length;
          st.lastVelocity = hn.velocity;
          st.lastSet = true;
        } else {
          // Add a note and start moving it.
          st.editSel.clear();  // K-198 follow-up: the new note alone is the selection
          Note n;
          n.start = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
          n.length = defLen;
          n.pitch = pitchAt(m.y);
          n.velocity = defVel;
          n.color = newColor;
          n.slide = newSlide;
          std::vector<Note> ns = notes;
          std::vector<bool> sel(ns.size(), false);
          for (const Note& x : notesToAdd(st, n)) {  // one note or a chord stamp
            // N47-04: in Clips never a second note on the same key at the same start (the reference keeps one)
            if (livelook::active(a) && std::any_of(ns.begin(), ns.end(), [&](const Note& y) { return y.pitch == x.pitch && y.start == x.start; })) continue;
            ns.push_back(x), sel.push_back(true);
          }
          if (st.snapToScale && st.scale > 0) n.pitch = tools::snapPitchToScale(n.pitch, st.scaleRoot, st.scale);
          if (ns.size() != notes.size()) {  // N47-04: already there: nothing added, nothing dragged
            clickDrewNote = true;
            commit(a, patId, chId, ns, sel, st.stampChord >= 0 ? "add chord" : "add note", g.key);
            if (st.stampOnce && (st.stampChord >= 0 || st.stampAuto > 0)) st.stampChord = -1, st.stampAuto = 0, st.stampOnce = false;
            notes = rollNotes(a, patId, chId);
            g.base = notes;
            g.sel = selVector(a, notes.size());
            g.anchor = static_cast<int>(*st.selected.begin());
            // N47-04: in Clips the drag counts from the click itself, not the snapped start: a click past
            // the middle of a step moved the new note one step on, out from under the pointer (so a
            // second click missed it and drew another).
            g.startTick = livelook::active(a) ? tickAt(m.x) : n.start;
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
  }

  st.tool = toolKept;
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
        // GitHub #71: step recording, the step cursor goes where the ruler is clicked (on the step grid).
        if (a.stepRecord && !a.engine.playing()) {
          const Tick sg = std::max<Tick>(1, a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(pm, a.piano.snap));
          a.stepRecorder.cursor = snapDown(std::max<Tick>(0, tickAt(std::clamp(m.x, gridMin.x, gridMax.x))), sg);
        }
        break;
      }
      case Gesture::Move: {
        if (g.multi) {  // K-198 follow-up: every selected note of every edited channel moves by one delta
          if (!g.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;
          const Tick earliest = multinote::earliestStart(g.mbase, g.msel, g.anchorStart);
          const Tick dt = timeline::moveDistance(curTick - g.startTick, snapG, st.snap, earliest);
          int dp = curPitch - g.startPitch;
          if (st.snapToScale && st.scale > 0) dp = tools::snapPitchToScale(g.anchorPitch + dp, st.scaleRoot, st.scale) - g.anchorPitch;
          dp = multinote::clampPitchDelta(g.mbase, g.msel, dp);
          noteAnimT.d = {static_cast<double>(dt), static_cast<double>(dp), 0.0};
          if (dt == 0 && dp == 0 && !g.moved) break;
          g.moved = true;
          splitSel(a, chId, multinote::transform(a.doc, patId, g.mbase, g.msel, [&](Note& n) { n.start += dt, n.pitch += dp; }, g.multiCopy,
                                                 g.multiCopy ? "copy notes" : "move notes", g.key));
          if (g.anchor >= 0) previewOn(a, chId, g.anchorPitch + dp, g.anchorVel);
          break;
        }
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
        if (g.multi) {  // K-198 follow-up: every selected note of every edited channel takes the same change
          st.lastLength = std::max<Tick>(1, an.length + dl2);
          noteAnimT.d = {0.0, 0.0, static_cast<double>(dl2)};
          splitSel(a, chId, multinote::transform(a.doc, patId, g.mbase, g.msel, [&](Note& n) { n.length = std::max<Tick>(1, n.length + dl2); },
                                                 false, "resize notes", g.key));
          break;
        }
        std::vector<Note> ns = g.base;
        for (size_t i = 0; i < ns.size(); ++i)
          if (g.sel[i]) ns[i].length = std::max<Tick>(1, ns[i].length + dl2);
        st.lastLength = std::max<Tick>(1, an.length + dl2);
        st.lastSet = true;
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
        // K-198 follow-up: the box takes the edited channels' notes too.
        multinote::Selection es = io.KeyShift ? g.editSelBefore : multinote::Selection{};
        for (ObjectId cid : st.editChannels) {
          if (cid == chId) continue;
          const std::vector<Note> ens = rollNotes(a, patId, cid);
          for (size_t i = 0; i < ens.size(); ++i) {
            float x0 = xOf(ens[i].start), x1 = xOf(ens[i].start + ens[i].length), y = yOf(ens[i].pitch);
            if (x1 >= lo.x && x0 <= hi.x && y + rowH >= lo.y && y <= hi.y) es[cid].insert(i);
          }
        }
        st.editSel = std::move(es);
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
        if (!ld.select && exprLane(lane)) {  // K-99: a pressure or timbre line painted over the notes
          const ExprDim d = laneDim(lane);
          const double grain = std::max(1.0, static_cast<double>(s.ppq) / 48.0);
          const char* elabel = d == ExprDim::Pressure ? "change note pressure" : "change note timbre";
          if (ld.line) {  // right drag: a straight line from the press, live from the gesture's start state
            std::vector<Note> ns = g.base;
            exprlane::paint(ns, only, d, ld.tickA, ld.fracA, t, frac, grain);
            if (ns != notes) commit(a, patId, chId, ns, g.sel, elabel, g.key);
          } else {  // paint along the pointer's path; Alt takes the lane back to neutral
            std::vector<Note> ns = notes;
            const double t0 = ld.hasPrev ? ld.prevTick : t;
            const float f0 = ld.hasPrev ? ld.prevFrac : frac;
            const bool changed = ld.reset ? exprlane::clear(ns, only, d, t0, t) : exprlane::paint(ns, only, d, t0, f0, t, frac, grain);
            if (changed) commit(a, patId, chId, ns, g.sel, elabel, g.key);
            ld.hasPrev = true;
            ld.prevTick = t;
            ld.prevFrac = frac;
          }
          char hb[48];
          if (d == ExprDim::Timbre)
            std::snprintf(hb, sizeof hb, "%s %+d%%", i18n::tr("Timbre"), static_cast<int>(std::lround((frac - 0.5f) * 200.0f)));
          else
            std::snprintf(hb, sizeof hb, "%s %d%%", i18n::tr("Pressure"), static_cast<int>(std::lround(frac * 100.0f)));
          a.setHint(ld.reset ? i18n::tr("Back to neutral") : hb);
          break;
        }
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
      st.editSel.clear();
    }
    if (g.kind == Gesture::Move && !g.moved && g.multi && g.anchor < 0 && g.anchorCh != kNoId && !io.KeyShift) {
      st.selected.clear();  // K-198 follow-up: a click on a selected edited note keeps just that note
      st.editSel = {{g.anchorCh, {g.anchorIdx}}};
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
    // K-171: after a click in the automation lane its points take the editing keys.
    const bool laneKeys = autoLane && automationLaneKeys(a, patId, std::max<Tick>(1, snapG));
    std::vector<Note> ns = notes;
    auto sel = selVector(a, ns.size());
    // K-198 follow-up: a selection spanning the edited channels: every edit below takes all of
    // them, one undo step.
    const bool multi = multiSel(a);
    bool anySel = !st.selected.empty() || multi;
    auto applyToSel = [&](auto fn, const char* label) {
      if (!anySel) return;
      if (multi) return multiApply(a, patId, chId, fn, label);
      for (size_t i = 0; i < ns.size(); ++i)
        if (sel[i]) fn(ns[i]);
      commit(a, patId, chId, ns, sel, label, 0);
    };
    auto multiErase = [&](const char* label) {  // deletes the selection over every channel
      const multinote::Selection all = unionSel(a, chId);
      const multinote::Lists base = multinote::lists(s, patId, all);
      a.doc.beginGroup(label);
      multinote::erase(a.doc, patId, base, all, label);
      a.doc.endGroup();
      st.selected.clear();
      st.editSel.clear();
    };
    if (!laneKeys && keys::pressed("piano.delete")) {
      if (multi) {
        multiErase("delete notes");
      } else if (anySel) {
        std::vector<Note> keep;
        for (size_t i = 0; i < ns.size(); ++i)
          if (!sel[i]) keep.push_back(ns[i]);
        commit(a, patId, chId, keep, {}, "delete notes", 0);
      }
    }
    if (!laneKeys && keys::pressed("piano.selectAll")) {
      st.selected.clear();
      for (size_t i = 0; i < ns.size(); ++i) st.selected.insert(i);
      st.editSel.clear();  // K-198 follow-up: the edited channels' notes too
      for (ObjectId cid : st.editChannels)
        if (cid != chId)
          if (const size_t n = rollNotes(a, patId, cid).size())
            for (size_t i = 0; i < n; ++i) st.editSel[cid].insert(i);
    }
    if (!laneKeys && keys::pressed("piano.deselect")) st.selected.clear(), st.editSel.clear();
    if (!laneKeys && (keys::pressed("piano.copy") || keys::pressed("piano.cut")) && multi) {
      const bool cut = keys::pressed("piano.cut");
      const multinote::Selection all = unionSel(a, chId);
      st.multiClipboard = multinote::copy(multinote::lists(s, patId, all), all);
      st.clipboard.clear();
      a.notify(std::to_string(st.multiClipboard.size()) + " notes copied from " + std::to_string(all.size()) + " channels");
      if (cut) multiErase("cut notes");
    } else {
      if (!laneKeys && keys::pressed("piano.copy") && anySel) {
        st.clipboard.clear();
        st.multiClipboard.clear();
        for (size_t i = 0; i < ns.size(); ++i)
          if (sel[i]) st.clipboard.push_back(ns[i]);
        a.notify(std::to_string(st.clipboard.size()) + " notes copied");
      }
      if (!laneKeys && keys::pressed("piano.cut") && anySel) {
        st.clipboard.clear();
        st.multiClipboard.clear();
        std::vector<Note> keep;
        for (size_t i = 0; i < ns.size(); ++i) (sel[i] ? st.clipboard : keep).push_back(ns[i]);
        commit(a, patId, chId, keep, {}, "cut notes", 0);
      }
    }
    if (!laneKeys && keys::pressed("piano.paste") && !st.multiClipboard.empty()) {
      // K-198 follow-up: each note back into its own channel, after the end of the copied block.
      const Tick shift = multinote::pasteShift(st.multiClipboard, grid);
      a.doc.beginGroup("paste notes");
      multinote::Selection out = multinote::paste(a.doc, patId, st.multiClipboard, shift, "paste notes");
      a.doc.endGroup();
      for (auto& cn : st.multiClipboard) cn.note.start += shift;  // repeated pastes keep going
      for (const auto& [cid, ix] : out)  // a pasted channel not edited here yet is edited now
        if (cid != chId) st.editChannels.insert(cid);
      splitSel(a, chId, std::move(out));
    } else if (!laneKeys && keys::pressed("piano.paste") && !st.clipboard.empty()) {
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
    if (!laneKeys && keys::pressed("piano.quickQuantize") && multi) {
      multiApply(a, patId, chId, [&](Note& n) { n.start = snapNearest(n.start, grid); }, "quantize notes");
    } else if (!laneKeys && keys::pressed("piano.quickQuantize")) {
      for (size_t i = 0; i < ns.size(); ++i)
        if (!anySel || sel[i]) ns[i].start = snapNearest(ns[i].start, grid);
      commit(a, patId, chId, ns, sel, "quantize notes", 0);
    }
    if (!laneKeys && keys::pressed("piano.transposeUp")) applyToSel([&](Note& n) { n.pitch += 1; }, "transpose notes");
    if (!laneKeys && keys::pressed("piano.transposeDown")) applyToSel([&](Note& n) { n.pitch -= 1; }, "transpose notes");
    if (!laneKeys && keys::pressed("piano.octaveUp")) applyToSel([&](Note& n) { n.pitch += 12; }, "transpose notes");
    if (!laneKeys && keys::pressed("piano.octaveDown")) applyToSel([&](Note& n) { n.pitch -= 12; }, "transpose notes");
    // GitHub #71: while step recording, Left and Right move the step cursor (main.cpp), not the notes.
    const bool stepKeys = a.stepRecord && !a.engine.playing();
    if (!laneKeys && !stepKeys && keys::pressed("piano.moveRight")) applyToSel([&](Note& n) { n.start += grid; }, "move notes");
    if (!laneKeys && !stepKeys && keys::pressed("piano.moveLeft")) applyToSel([&](Note& n) { n.start = std::max<Tick>(0, n.start - grid); }, "move notes");
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
      if (keys::pressed("piano.slideMode")) newSlide = !newSlide;
    }
  }
  if (clipColW > 0) ImGui::Unindent(clipColW);
  ui::EndDockWindow();
}

}  // namespace gl
