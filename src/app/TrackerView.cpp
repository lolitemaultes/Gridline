// Gridline - the tracker pattern editor (K-106). See TrackerView.h.
#include "TrackerView.h"
#include <SDL.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/MidiRecord.h"
#include "../core/Params.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "CollabUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MidiArm.h"
#include "MenuParityUi.h"
#include "PixelGrid.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "TrackerEditors.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "WindowRaise.h"
#include "Widgets.h"

namespace gl::trackerview {

using theme::pal;
using trk::Cell;
using trk::Field;

namespace {

struct Cursor {
  int track = 0, column = 0;
  Field field = Field::Note;
  int digit = 0;
  int row = 0;
};
struct Selection {
  bool on = false;
  int t0 = 0, r0 = 0, t1 = 0, r1 = 0;  // anchor (t0, r0), moving end (t1, r1)
  int tLo() const { return std::min(t0, t1); }
  int tHi() const { return std::max(t0, t1); }
  int rLo() const { return std::min(r0, r1); }
  int rHi() const { return std::max(r0, r1); }
};
struct KeyEv {
  int sc = 0, kc = 0, mods = 0;
  bool repeat = false;
};
struct Held {
  ObjectId channel = kNoId;
  int pitch = 0;
  int track = -1, column = 0;  // record key-off: where the note was written (-1: only played)
};
// The classic tracker's three clipboards: the block's, the track's and the pattern's (each keeps
// its own rows, so a track copied with Shift+F4 pastes again after a block was copied).
enum class Buf { Block, Track, Pattern };

struct State {
  Cursor cur;
  Selection sel;
  bool edit = false;
  int step = 1;
  bool follow = true;
  bool hasKb = false;      // the editor had the keyboard on the last frame
  int chordRow = -1;       // multichannel edit: the row the notes held together go to
  bool chordGroup = false; // R5-08: the notes typed while note keys are held are one undo step
  bool wantFocus = false;  // dev / page switch: focus the editor next frame
  std::vector<KeyEv> keys;
  std::map<ObjectId, int> extraCols;  // note columns added by hand (view only)
  trk::Block clip, clipTrack, clipPattern;
  std::string status;
  double statusT = -10;
  uint64_t statusSerial = 0;  // R11-10: the undo position the message belongs to (0: taken on the next frame)
  std::map<int, Held> held;  // scan code -> the note it auditions
  int playRow = -1;
  float playFrac = 0;  // how far the play position is through playRow (0..1)
  ImVec2 area0, area1, cell0, cell1;  // last frame (dev geometry)
  float rowH = 14;
  ObjectId pattern = kNoId;
  ObjectId cursorChannel = kNoId;  // the channel the cursor was on (selection follows a move)
  int drawnFrame = -10;  // the ImGui frame the editor was last drawn in (a closed editor has no keyboard)
  std::string pendingPage;  // a centre page to bring to the front (bringPage)
  int pendingFrames = 0, pendingWait = 0;  // frames docked and waiting, frames in all
  bool pendingQuiet = false;  // K-297 (round 8): the page comes to the front without taking the keys
  int pendingHold = 0;  // frames to wait before the request looks (the layout re-docks its pages)
  int jumps[4] = {-1, -1, -1, -1};  // F9 to F12 rows stored with Shift (-1: 0, a quarter, half, three quarters)
  float wheel = 0;          // wheel movement not yet a whole row (trackpads send small steps)
  float scrollX = 0;        // the tracks' sideways scroll (always a track's left edge)
  float wheelX = 0;         // sideways wheel movement not yet a whole track
  float zoomWheel = 0;      // cp17: Ctrl+wheel movement not yet a zoom step
  ImVec2 hbar0, hbar1;      // the tracks' scrollbar last frame (none: both zero)
  long long scrolledFor = -1;  // the cursor track (and the tracks' sizes) the sideways scroll last brought into view
  int offLeft = 0, offRight = 0;  // tracks out of view with notes in the rows shown (last frame)
  int firstShown = 0, lastShown = -1;  // cp16: the tracks in view (last frame; "tracker tracks")
  int stacked = 0;  // song mode: other patterns playing at the position (shown on the Song page, counted in the bar)
};
State& st() {
  static State s;
  return s;
}

// The sub-columns shown (View > Columns; the note and instrument always are).
bool gShow[static_cast<int>(Field::Count)] = {true, true, true, true, true};
bool fieldShown(Field f) { return gShow[static_cast<int>(f)]; }
bool gDots = true;    // empty fields as dots (Settings: off, blank)
bool gFlats = false;  // note names with flats
bool gMulti = false;  // multichannel edit: each typed note moves the cursor to the next track
bool gMultiRec = false;   // multichannel record: each recorded note goes to the next enabled track
bool gMultiJazz = false;  // multichannel key jazz: notes only played go to the enabled tracks in turn
int gRecSnap = 0;         // cp15: record quantize (the classic "Quantize"): rows a recorded note snaps to (0: off)
bool gRecOff = false;     // record key-off notes: a key released while recording ends its note there
bool gCutCopies = true;   // a cut also copies (off: it only deletes)
bool gInsDelLength = false;  // R7-15: inserting / deleting a line on every track changes the pattern length
bool gTall = false;       // Settings, Layout: taller rows
bool gBeats = true;       // ... beat and bar rows lit
bool gFrames = true;      // ... lines between the tracks
bool gNumbers = true;     // ... track numbers in the headers
trk::Masks gMasks;        // the Advanced edit box's clipboard masks
bool gMasking = false;    // ... turned on
int gVisible = 0;     // tracks in view at most (0: as many as fit), the classic "maximum visible channels"
int gTrackWidth = 0;  // cp16: characters every track is widened by (a header's edge dragged; settings)
// cp17 (user): the pattern's sideways zoom, a view preference kept in the settings (never in the
// song: saves stay the same in every layout). Each step narrows the tracks as a classic tracker's
// narrower track modes do: 0 as set (the glyphs grow with the room, a hand-widened width kept);
// 1 the interface's glyph size; 2 the effect and pan sub-columns hidden; 3 the note and instrument
// only; 4 the note only; 5 and 6 the note in smaller glyphs. Hidden sub-columns keep their data
// and are edited again by zooming back in. "Fit" picks the widest step that shows every track.
constexpr int kZoomMax = 6;
bool gShowCfg[static_cast<int>(Field::Count)] = {true, true, true, true, true};  // the settings' sub-columns
int gZoom = 0;        // the step chosen (settings tracker.patternZoom)
bool gZoomFit = false;  // the step follows "every track in view" (settings tracker.patternFit)
int gZoomEff = 0;     // the step drawn (the fitted one with Fit)
// The sub-columns step `z` shows.
void zoomColumns(int z) {
  for (int f = 0; f < static_cast<int>(Field::Count); ++f) gShow[f] = gShowCfg[f];
  if (z >= 2) gShow[static_cast<int>(Field::Effect)] = gShow[static_cast<int>(Field::Pan)] = false;
  if (z >= 3) gShow[static_cast<int>(Field::Volume)] = false;
  if (z >= 4) gShow[static_cast<int>(Field::Instrument)] = false;
}
// The glyph size step `z` draws at (`fs` the interface's).
float zoomGlyph(int z, float fs) { return z >= 6 ? std::max(7.0f, std::floor(fs * 0.66f)) : z >= 5 ? std::max(8.0f, std::floor(fs * 0.82f)) : fs; }
void loadColumns(App& a) {
  gVisible = static_cast<int>(a.settings.getNumber("tracker.visibleTracks"));
  gTrackWidth = std::clamp(static_cast<int>(a.settings.getNumber("tracker.trackWidth")), 0, 40);
  gMulti = a.settings.getBool("tracker.multiEdit");
  gMultiRec = a.settings.getBool("tracker.multiRecord");
  gMultiJazz = a.settings.getBool("tracker.multiJazz");
  gRecOff = a.settings.getBool("tracker.recordKeyOff");
  gRecSnap = std::clamp(static_cast<int>(a.settings.getNumber("tracker.recSnap")), 0, 16);
  gCutCopies = a.settings.getBool("tracker.cutCopies");
  gInsDelLength = a.settings.getBool("tracker.insDelLength");
  gTall = a.settings.getBool("tracker.tallRows");
  gBeats = a.settings.getBool("tracker.beatHighlight");
  gFrames = a.settings.getBool("tracker.trackFrames");
  gNumbers = a.settings.getBool("tracker.trackNumbers");
  gShowCfg[static_cast<int>(Field::Volume)] = a.settings.getBool("tracker.volumeColumn");
  gShowCfg[static_cast<int>(Field::Pan)] = a.settings.getBool("tracker.panColumn");
  gShowCfg[static_cast<int>(Field::Effect)] = a.settings.getBool("tracker.effectColumn");
  gZoom = std::clamp(static_cast<int>(a.settings.getNumber("tracker.patternZoom")), 0, kZoomMax);
  gZoomFit = a.settings.getBool("tracker.patternFit");
  if (!gZoomFit) gZoomEff = gZoom;
  zoomColumns(gZoomEff);
  gDots = a.settings.getBool("tracker.emptyDots");
  gFlats = a.settings.getBool("tracker.flats");
}
// The next track after `ti` that multichannel edit writes to (the enabled ones; none other: `ti`).
int nextMultiTrack(App& a, const std::vector<trk::Track>& tracks, int ti) {
  const int n = static_cast<int>(tracks.size());
  for (int d = 1; d < n; ++d) {
    const int t = (ti + d) % n;
    if (multiTrackOn(a, tracks[static_cast<size_t>(t)].channel)) return t;
  }
  return ti;
}

// The next shown field from `f` in direction `dir` (Field::Count, or -1 as int, past the ends).
int nextShown(int f, int dir) {
  while (f >= 0 && f < static_cast<int>(Field::Count) && !gShow[f]) f += dir;
  return f;
}

ops::ChannelSolo& soloMemory() {
  static ops::ChannelSolo m;
  return m;
}

void say(const std::string& text) {
  st().status = text;
  st().statusT = ImGui::GetTime();
  st().statusSerial = 0;
}

// The tracks of the shown pattern.
struct View {
  const Pattern* p = nullptr;
  trk::Grid g;
  std::vector<trk::Track> tracks;
  std::vector<const Channel*> channels;
  // Per track: the pattern its cells come from (the shown one, or one stacked under the song
  // position while the song plays) and the row of that pattern at a shown row's place.
  std::vector<const Pattern*> src;
  std::vector<int> rowOff;
};

// cp16 (user, fixed columns): the note columns each channel needs over the whole song (the most
// voices any pattern gives it), so a track keeps its width from pattern to pattern. Worked out
// again only when the song changes.
int songVoices(const Song& s, uint64_t revision, ObjectId channel) {
  static std::map<ObjectId, int> voices;
  static uint64_t at = ~uint64_t{0};
  static const Song* of = nullptr;
  if (at != revision || of != &s) {
    voices.clear();
    for (const Pattern& p : s.patterns) {
      const trk::Grid g = trk::gridOf(s, p);
      for (const auto& [ch, notes] : p.notes)
        if (!notes.empty()) voices[ch] = std::max(voices[ch], trk::buildTrack(s, p, ch, g, 1).voices);
      for (const auto& [ch, steps] : p.steps)
        if (std::any_of(steps.begin(), steps.end(), [](uint8_t x) { return x > 0; }))
          voices[ch] = std::max(voices[ch], trk::buildTrack(s, p, ch, g, 1).voices);
    }
    at = revision, of = &s;
  }
  const auto it = voices.find(channel);
  return it == voices.end() ? 1 : std::max(1, it->second);
}

View build(App& a) {
  View v;
  const Song& s = a.doc.song();
  v.p = s.pattern(a.curPattern);
  if (!v.p) return v;
  v.g = trk::gridOf(s, *v.p);
  // cp16 (user): every channel of the rack has its track, always, in the rack's order (as a
  // tracker shows all its channels): none comes or goes with the pattern or its notes, and each
  // keeps its width over the song (its most voices in any pattern, and the columns added by hand).
  for (const Channel& c : s.channels) {
    const int extra = st().extraCols.count(c.id) ? st().extraCols.at(c.id) : 0;
    trk::Track t = trk::buildTrack(s, *v.p, c.id, v.g, songVoices(s, a.doc.revision(), c.id) + extra);
    v.tracks.push_back(std::move(t));
    v.channels.push_back(&c);
    v.src.push_back(v.p);
    v.rowOff.push_back(0);
  }
  return v;
}

// The other patterns playing at song position `pos` beside `shown` (clips stacked on other playlist
// tracks). The editor shows one pattern, as the classic editor shows the one pattern of the order
// position; the others are counted in the pattern bar, never drawn into this pattern's rows.
int stackedAt(const Song& s, ObjectId shown, Tick pos) {
  std::vector<ObjectId> others;
  for (const Clip& c : s.playlist) {
    if (c.kind != ClipKind::Pattern || c.pattern == shown || pos < c.start || pos >= c.start + s.clipLength(c)) continue;
    if (std::find(others.begin(), others.end(), c.pattern) == others.end()) others.push_back(c.pattern);
  }
  return static_cast<int>(others.size());
}

void clampCursor(const View& v) {
  Cursor& c = st().cur;
  if (v.tracks.empty()) {
    c = Cursor{};
    return;
  }
  c.track = std::clamp(c.track, 0, static_cast<int>(v.tracks.size()) - 1);
  c.column = std::clamp(c.column, 0, v.tracks[static_cast<size_t>(c.track)].columns - 1);
  c.row = std::clamp(c.row, 0, v.g.rows - 1);
  if (!fieldShown(c.field)) c.field = Field::Note;
  c.digit = std::clamp(c.digit, 0, trk::fieldDigits(c.field) - 1);
  // A block left from before a channel went or the pattern got shorter stays inside the pattern.
  Selection& sel = st().sel;
  const int lastT = static_cast<int>(v.tracks.size()) - 1, lastR = std::max(0, v.g.rows - 1);
  sel.t0 = std::clamp(sel.t0, 0, lastT), sel.t1 = std::clamp(sel.t1, 0, lastT);
  sel.r0 = std::clamp(sel.r0, 0, lastR), sel.r1 = std::clamp(sel.r1, 0, lastR);
}

// ---- edits through the Document ----
struct Rows {
  std::vector<Note> notes;
  std::vector<uint8_t> steps;
  std::vector<StepProps> props;
};

const std::vector<Note>& notesRef(const Pattern& p, ObjectId ch) {
  static const std::vector<Note> none;
  const auto it = p.notes.find(ch);
  return it == p.notes.end() ? none : it->second;
}
const std::vector<uint8_t>& stepsRef(const Pattern& p, ObjectId ch) {
  static const std::vector<uint8_t> none;
  const auto it = p.steps.find(ch);
  return it == p.steps.end() ? none : it->second;
}
const std::vector<StepProps>& propsRef(const Pattern& p, ObjectId ch) {
  static const std::vector<StepProps> none;
  const auto it = p.stepProps.find(ch);
  return it == p.stepProps.end() ? none : it->second;
}

Rows rowsOf(const Pattern& p, ObjectId ch) {
  Rows r;
  if (const auto it = p.notes.find(ch); it != p.notes.end()) r.notes = it->second;
  if (const auto it = p.steps.find(ch); it != p.steps.end()) r.steps = it->second;
  if (const auto it = p.stepProps.find(ch); it != p.stepProps.end()) r.props = it->second;
  return r;
}

void commit(App& a, ObjectId pattern, ObjectId ch, const Rows& r, const Rows& before, const std::string& label) {
  const bool notes = r.notes != before.notes, steps = r.steps != before.steps || r.props != before.props;
  if (!notes && !steps) return;
  st().status.clear();  // R10-10: an edit made ends the last message (one about this edit is said after it)
  a.doc.beginGroup(label);
  if (notes) a.doc.setNotes(pattern, ch, r.notes, label);
  if (steps)
    a.doc.editPattern(pattern, label, [&](Pattern& q) {
      q.steps[ch] = r.steps;
      if (r.props.empty()) q.stepProps.erase(ch);
      else q.stepProps[ch] = r.props;
    });
  a.doc.endGroup(false);
}

template <class F>
trk::Result applyEdit(App& a, const View& v, int ti, const char* label, F&& fn) {
  trk::Result res;
  if (!v.p || ti < 0 || ti >= static_cast<int>(v.tracks.size())) return res;
  const ObjectId ch = v.tracks[static_cast<size_t>(ti)].channel;
  Rows r = rowsOf(*v.p, ch);
  const Rows before = r;
  res = fn(r, v.tracks[static_cast<size_t>(ti)], v.g);
  if (!res.refused.empty()) say(i18n::tr(res.refused.c_str()));
  if (res.changed) commit(a, v.p->id, ch, r, before, label);
  return res;
}

// The tracks and rows a block tool acts on: the selection, else the cursor's row on its track.
void blockRange(int* t0, int* t1, int* r0, int* r1) {
  const State& s = st();
  if (s.sel.on) {
    *t0 = s.sel.tLo(), *t1 = s.sel.tHi(), *r0 = s.sel.rLo(), *r1 = s.sel.rHi();
  } else {
    *t0 = *t1 = s.cur.track;
    *r0 = *r1 = s.cur.row;
  }
}

void moveRow(const View& v, int d, bool wrap) {
  Cursor& c = st().cur;
  if (v.g.rows <= 0) return;
  if (wrap) c.row = ((c.row + d) % v.g.rows + v.g.rows) % v.g.rows;
  else c.row = std::clamp(c.row + d, 0, v.g.rows - 1);
}

// The cursor's place along a track: note column, field, digit.
void moveDigit(const View& v, int d) {
  Cursor& c = st().cur;
  if (v.tracks.empty()) return;
  for (int k = 0; k < std::abs(d); ++k) {
    const int dir = d > 0 ? 1 : -1;
    int digit = c.digit + dir;
    int field = static_cast<int>(c.field);
    if (digit >= 0 && digit < trk::fieldDigits(c.field)) {
      c.digit = digit;
      continue;
    }
    field = nextShown(field + dir, dir);
    if (field >= 0 && field < static_cast<int>(Field::Count)) {
      c.field = static_cast<Field>(field);
      c.digit = dir > 0 ? 0 : trk::fieldDigits(c.field) - 1;
      continue;
    }
    int col = c.column + dir;
    if (col < 0 || col >= v.tracks[static_cast<size_t>(c.track)].columns) {
      const int n = static_cast<int>(v.tracks.size());
      const int t = (c.track + dir + n) % n;  // past the last track: the first (and back), as the classic editor wraps
      c.track = t;
      col = dir > 0 ? 0 : v.tracks[static_cast<size_t>(t)].columns - 1;
    }
    c.column = col;
    c.field = dir > 0 ? Field::Note : static_cast<Field>(nextShown(static_cast<int>(Field::Effect), -1));
    c.digit = dir > 0 ? 0 : trk::fieldDigits(c.field) - 1;
  }
}

int hexDigit(int kc) {
  if (kc >= '0' && kc <= '9') return kc - '0';
  if (kc >= 'a' && kc <= 'f') return kc - 'a' + 10;
  if (kc >= 'A' && kc <= 'F') return kc - 'A' + 10;
  return -1;
}

int setNibble(int value, int digit, int nib) { return digit == 0 ? ((nib << 4) | (value & 0x0F)) : ((value & 0xF0) | nib); }

// The pitch a note key types: Z on the lower row is C of the octave picked (F1 to F8), the upper
// row an octave higher. -1 when the key is not a note key.
int notePitch(App& a, int scancode) {
  const int key = TypingKeyboard::keyAtPosition(scancode);
  const int off = key > 0 ? TypingKeyboard::keyOffset(key) : -1;
  if (off < 0) return -1;
  return 12 * (a.typing.octave + 1) + off;
}

bool isOffKey(int sc) { return sc == SDL_SCANCODE_NONUSBACKSLASH || sc == SDL_SCANCODE_GRAVE || sc == SDL_SCANCODE_1; }

void audition(App& a, int sc, ObjectId ch, int pitch, int velocity) {
  if (ch == kNoId || pitch < 0 || pitch > 127) return;
  State& s = st();
  if (const auto it = s.held.find(sc); it != s.held.end()) a.engine.noteOff(it->second.channel, it->second.pitch);
  a.engine.noteOn(ch, pitch, static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f);
  s.held[sc] = {ch, pitch};
}

// Multichannel edit: a note key other than `sc` still held (the notes are a chord).
bool otherNoteHeld(int sc) {
  for (const auto& [k, h] : st().held)
    if (k != sc) return true;
  return false;
}

void releaseAll(App& a) {
  for (const auto& [sc, h] : st().held) a.engine.noteOff(h.channel, h.pitch);
  st().held.clear();
}

// ---- block operations ----
trk::Block& buffer(Buf which) {
  State& s = st();
  return which == Buf::Track ? s.clipTrack : which == Buf::Pattern ? s.clipPattern : s.clip;
}

void copyBlock(const View& v, bool cut, App& a, Buf which = Buf::Block) {
  int t0, t1, r0, r1;
  const State& s = st();
  if (s.sel.on) blockRange(&t0, &t1, &r0, &r1);
  else t0 = t1 = s.cur.track, r0 = 0, r1 = v.g.rows - 1;  // no block: the whole track
  // Cut without copying (Settings: a cut only deletes): the clipboard keeps what it had.
  if (!cut || gCutCopies) {
    trk::Block b;
    b.rows = r1 - r0 + 1;
    for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t) {
      const Rows r = rowsOf(*v.p, v.tracks[static_cast<size_t>(t)].channel);
      b.tracks.push_back(trk::copyRows(r.notes, r.steps, r.props, v.g, r0, r1));
      if (gMasking) trk::maskCopied(b.tracks.back(), gMasks);  // the copy mask: the parts left out are not taken
    }
    for (int k = 0; k < trk::kParts; ++k) b.carried[k] = !gMasking || gMasks.copy[k];
    buffer(which) = b;
  }
  if (cut) {
    a.doc.beginGroup("cut rows");
    for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
      applyEdit(a, v, t, "cut rows", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::cutRows(x.notes, x.steps, g, r0, r1); });
    a.doc.endGroup(false);
  }
  const char* what = which == Buf::Track ? (cut ? "Track cut" : "Track copied") : which == Buf::Pattern ? (cut ? "Pattern cut" : "Pattern copied")
                                                                                                    : (cut ? "Block cut" : "Block copied");
  say(std::string(i18n::tr(what)) + (cut && !gCutCopies ? std::string(" (") + i18n::tr("not kept: Settings, Miscellaneous") + ")" : std::string()) +
      (gMasking ? std::string(" \xC2\xB7 ") + i18n::tr("masks on") : std::string()));
}

void pasteBlock(const View& v, App& a, Buf which = Buf::Block) {
  const State& s = st();
  const trk::Block& src = buffer(which);
  if (src.empty()) {
    say(i18n::tr(which == Buf::Track ? "No track copied yet: Shift+F4 copies the cursor's track"
                 : which == Buf::Pattern ? "No pattern copied yet: Ctrl+F4 copies the pattern"
                                         : "Nothing copied yet: mark a block (Alt or Shift and the arrows), then Alt+F4 or Ctrl+C"));
    return;
  }
  trk::Block b = src;
  const int row = s.cur.row;
  // R7-09: a track or a pattern pastes over the whole track (the classic editor's buffer holds
  // empty rows past the copy): the rows past a shorter copy are cleared, through the masks.
  if (which != Buf::Block) b.rows = std::max(b.rows, v.g.rows - row);
  a.doc.beginGroup("paste rows");
  for (size_t i = 0; i < b.tracks.size(); ++i) {
    const int t = s.cur.track + static_cast<int>(i);
    if (t >= static_cast<int>(v.tracks.size())) break;
    applyEdit(a, v, t, "paste rows", [&](Rows& x, const trk::Track&, const trk::Grid& g) {
      return gMasking ? trk::pasteRowsMasked(x.notes, x.steps, g, row, b.rows, b.tracks[i], b.carried, gMasks)
                      : trk::pasteRows(x.notes, x.steps, g, row, b.rows, b.tracks[i]);
    });
  }
  a.doc.endGroup(false);
}

// R7-09: the pattern clipboard pasted over pattern `pid` from its top, after the prompt (the view is
// built again: the prompt closes frames later). `bars` > 0: the pattern takes that length first, in
// the same undo step (the song's later entries move along, R6-02).
void pasteWhole(App& a, Buf which, ObjectId pid, int bars) {
  State& s = st();
  View v = build(a);
  if (!v.p || v.p->id != pid) return;
  a.doc.beginGroup("paste pattern");
  if (bars > 0 && bars != v.p->bars) {
    trk::setPatternLengthInSong(a.doc, pid, bars, v.p->stepsPerBar);
    v = build(a);
  }
  if (v.p && v.p->id == pid) {
    const Cursor keep = s.cur;
    s.cur.track = 0, s.cur.row = 0;
    pasteBlock(v, a, which);
    s.cur = keep;
    clampCursor(v);
  }
  a.doc.endGroup(false);
}

void clearBlock(const View& v, App& a) {
  int t0, t1, r0, r1;
  blockRange(&t0, &t1, &r0, &r1);
  a.doc.beginGroup("clear rows");
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
    applyEdit(a, v, t, "clear rows", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::cutRows(x.notes, x.steps, g, r0, r1); });
  a.doc.endGroup(false);
}

// Whether any track has a note starting, or a step set, at or after row `r0` (past the end too).
bool rowsUsed(const View& v, int r0) {
  if (!v.p) return false;
  const Tick from = v.g.tickOf(r0);
  for (const auto& t : v.tracks) {
    const Rows r = rowsOf(*v.p, t.channel);
    for (const Note& n : r.notes)
      if (n.start >= from) return true;
    for (size_t i = static_cast<size_t>(std::max(0, r0)); i < r.steps.size(); ++i)
      if (r.steps[i] != 0) return true;
  }
  return false;
}

// A row inserted / deleted at the cursor's row: its track, or every track.
// R7-15 (Settings, Miscellaneous): with "Insert and delete change the length", a line inserted or
// deleted on every track changes the pattern's length, as the classic option does. Gridline's
// patterns are whole bars, so the length follows by bars: an insert that would push a note or a
// step off the last row first adds a bar; a delete that empties the last bar (it held something
// before) takes that bar away. One undo step; the song's later entries move along (R6-02).
void rowEdit(App& a, const View& v0, bool ins, bool everyTrack) {
  View v = v0;
  const int row = st().cur.row;
  const int lo = everyTrack ? 0 : st().cur.track, hi = everyTrack ? static_cast<int>(v.tracks.size()) - 1 : st().cur.track;
  a.doc.beginGroup(ins ? "insert row" : "delete row");
  const bool stretch = gInsDelLength && everyTrack && v.p;
  const ObjectId pid = v.p ? v.p->id : kNoId;
  const int spb = v.p ? std::max(1, v.p->stepsPerBar) : 1;
  bool lastBarHad = false;
  if (stretch && ins && rowsUsed(v, v.g.rows - 1) && v.g.rows + spb <= 256 && v.p->bars < kMaxPatternBars) {
    trk::setPatternLengthInSong(a.doc, pid, v.p->bars + 1, v.p->stepsPerBar);
    v = build(a);
  }
  if (stretch && !ins) lastBarHad = v.p->bars > 1 && rowsUsed(v, v.g.rows - spb);
  // A line on every track moves every track or none (R9-04, R9-06: a track that refuses, an imported
  // voice across the row or a note that would fall off the end, keeps the others as they are too).
  if (v.p && v.p->id == pid && hi > lo)
    for (int x = lo; x <= hi; ++x) {
      Rows r = rowsOf(*v.p, v.tracks[static_cast<size_t>(x)].channel);
      const trk::Result t = ins ? trk::insertRow(r.notes, r.steps, r.props, v.g, row) : trk::deleteRow(r.notes, r.steps, r.props, v.g, row);
      if (!t.refused.empty()) {
        say(i18n::tr(t.refused.c_str()));
        a.doc.cancelGroup();
        return;
      }
    }
  int dropped = 0;
  if (v.p && v.p->id == pid)
    for (int x = lo; x <= hi; ++x)
      dropped += applyEdit(a, v, x, ins ? "insert row" : "delete row", [&](Rows& r, const trk::Track&, const trk::Grid& g) {
                   return ins ? trk::insertRow(r.notes, r.steps, r.props, g, row) : trk::deleteRow(r.notes, r.steps, r.props, g, row);
                 }).dropped;
  // R10-02: what the insert pushed off the last row is gone, as in the classic tracker; the bar says
  // so (the same undo step brings it back).
  if (dropped > 0 && v.p) {
    char b[160];
    std::snprintf(b, sizeof b, i18n::tr("Row %s's %s dropped off the end (Undo brings it back)"), trk::rowText(v.g.rows - 1, a.settings.getString("tracker.rowBase") != "dec").c_str(),
                  dropped == 1 ? i18n::tr("note") : i18n::tr("notes"));
    say(b);
  }
  if (stretch && !ins && lastBarHad) {
    v = build(a);
    if (v.p && v.p->id == pid && v.p->bars > 1 && !rowsUsed(v, v.g.rows - spb)) {
      trk::setPatternLengthInSong(a.doc, pid, v.p->bars - 1, v.p->stepsPerBar);
      clampCursor(build(a));
    }
  }
  a.doc.endGroup(false);
}

// A note moved to another channel (the instrument typed): out of one, into the other, one undo step.
void moveToChannel(App& a, const View& v, int ti, int noteIndex, int target) {
  const Song& s = a.doc.song();
  if (target < 0 || target >= static_cast<int>(s.channels.size())) return;
  const ObjectId from = v.tracks[static_cast<size_t>(ti)].channel, to = s.channels[static_cast<size_t>(target)].id;
  if (from == to) return;  // (compared by channel: tracks skip audio clips' source channels, so a track's place is not its number)
  std::vector<Note> src = rowsOf(*v.p, from).notes, dst = rowsOf(*v.p, to).notes;
  if (noteIndex < 0 || noteIndex >= static_cast<int>(src.size())) return;
  const Note n = src[static_cast<size_t>(noteIndex)];
  src.erase(src.begin() + noteIndex);
  dst.push_back(n);
  trk::normalize(dst);
  a.doc.beginGroup("move note to instrument");
  a.doc.setNotes(v.p->id, from, src, "move note to instrument");
  a.doc.setNotes(v.p->id, to, dst, "move note to instrument");
  a.doc.endGroup(false);
  // R10-09: the note, and the track it went to (number and name, as its header shows them).
  say(trk::noteText(n.pitch, gFlats) + " " + i18n::tr("moved to track") + " " + std::to_string(target + 1) + " " + s.channels[static_cast<size_t>(target)].name);
}

// ---- the Tracker key set's pattern keys (core/Keymap Profile::Tracker; FT2-MAP section 9) ----
bool trackerKeySet() { return keys::keymap().profile() == keys::Profile::Tracker; }

// R6-01: an effect letter typed at its default value (T00, X80, Y80, R40) changes nothing in the note,
// so the letter waits on the note (drawn "T..") until its digits come; any edit lets it go.
struct PendingFx {
  ObjectId pattern = kNoId, channel = kNoId;
  Tick start = -1;
  int pitch = -1;
  char letter = 0;
  uint64_t revision = 0;
};
PendingFx gPendFx;
char pendingEffect(const App& a, ObjectId pattern, ObjectId channel, const Note* n) {
  const PendingFx& f = gPendFx;
  return n && f.letter && f.pattern == pattern && f.channel == channel && f.start == n->start && f.pitch == n->pitch && f.revision == a.doc.revision()
             ? f.letter
             : 0;
}

// Alt+Q..I: tracks 0 to 7, Alt+A..K: tracks 8 to 15 (-1: not one of them).
int trackJump(int sc) {
  static const int row1[8] = {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I};
  static const int row2[8] = {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K};
  for (int i = 0; i < 8; ++i) {
    if (sc == row1[i]) return i;
    if (sc == row2[i]) return 8 + i;
  }
  return -1;
}

// The chords the Tracker key set gives the pattern editor on top of the common ones.
bool trackerTakes(int sc, int mods) {
  if (!trackerKeySet()) return false;
  const bool ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (mods & KMOD_ALT) != 0, shift = (mods & KMOD_SHIFT) != 0;
  // N-04: the play keys. Right Ctrl and Enter play the song, Right Alt and Ctrl+Enter the pattern,
  // Shift+Enter the pattern from the cursor's row.
  if (sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_RALT) return !shift;
  if (sc == SDL_SCANCODE_RETURN && !alt) return true;
  if (ctrl && alt) return false;
  if (sc >= SDL_SCANCODE_F9 && sc <= SDL_SCANCODE_F12 && !alt) return true;  // jump rows (Shift: store, Ctrl: play from); Alt+F12 closes the plugin windows
  const bool mod = shift || ctrl || alt;
  if ((sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F2 || sc == SDL_SCANCODE_F7 || sc == SDL_SCANCODE_F8) && mod) return true;  // transpose
  if ((sc == SDL_SCANCODE_F3 || sc == SDL_SCANCODE_F4 || sc == SDL_SCANCODE_F5) && (shift || ctrl)) return true;  // track / pattern clipboard
  if (sc == SDL_SCANCODE_GRAVE && !ctrl && !alt) return true;  // edit step
  if (sc == SDL_SCANCODE_CAPSLOCK) return true;                // note off
  if (alt && !ctrl && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return true;  // macros: Alt writes one, Shift+Alt stores it
  if (!ctrl && !alt && ((sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_8) || sc == SDL_SCANCODE_KP_ENTER)) return true;  // keypad instruments
  if (ctrl && !alt && (sc == SDL_SCANCODE_KP_PLUS || sc == SDL_SCANCODE_KP_MINUS)) return true;  // the master volume
  if (alt && (trackJump(sc) >= 0 || sc == SDL_SCANCODE_C || sc == SDL_SCANCODE_DELETE || (sc >= SDL_SCANCODE_RIGHT && sc <= SDL_SCANCODE_UP)))
    return true;
  if (ctrl && (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT || (sc == SDL_SCANCODE_DELETE && !shift))) return true;  // Ctrl+Shift+Del deletes the pattern
  return false;
}

// The macros (the classic volume and effect macros, R4-06): two banks of ten, one for volumes and
// one for effects. The field the cursor is on picks the bank: Shift+Alt+1 to 0 stores the cell's
// volume or effect in its slot, Alt+1 to 0 writes the slot into the note at the cursor and moves on
// by the edit step. Both banks are kept in the settings (tracker.volumeMacros, tracker.effectMacros:
// ten comma separated slots, "20" or "P01", empty when unused).
struct Macro {
  bool set = false;
  int value = 0;
  char letter = 0;  // the effect bank's letter
};
struct MacroBanks {
  Macro vol[10], fx[10];
};

MacroBanks& macros(App& a) {
  static MacroBanks m;
  static bool loaded = false;
  if (loaded) return m;
  loaded = true;
  auto load = [&](const char* key, Macro* bank, bool effect) {
    const std::string t = a.settings.getString(key);
    size_t at = 0;
    for (int i = 0; i < 10 && at <= t.size(); ++i) {
      const size_t e = std::min(t.find(',', at), t.size());
      const std::string v = t.substr(at, e - at);
      at = e + 1;
      if (v.empty() || (effect && v.size() < 2)) continue;
      char* end = nullptr;
      const long n = std::strtol(v.c_str() + (effect ? 1 : 0), &end, 16);
      if (!end || *end || n < 0 || n > 255) continue;
      bank[i] = {true, static_cast<int>(n), effect ? v[0] : static_cast<char>(0)};
    }
  };
  load("tracker.volumeMacros", m.vol, false);
  load("tracker.effectMacros", m.fx, true);
  return m;
}

void saveMacros(App& a) {
  const MacroBanks& m = macros(a);
  auto text = [](const Macro* bank, bool effect) {
    std::string t;
    for (int i = 0; i < 10; ++i) {
      if (i) t += ',';
      if (bank[i].set) t += (effect ? std::string(1, bank[i].letter) : std::string()) + trk::hex2(bank[i].value);
    }
    return t;
  };
  a.settings.set("tracker.volumeMacros", text(m.vol, false));
  a.settings.set("tracker.effectMacros", text(m.fx, true));
}

int macroKey(App& a, const View& v, int slot, bool store) {
  State& s = st();
  Cursor& c = s.cur;
  const trk::Track& t = v.tracks[static_cast<size_t>(c.track)];
  const Cell& cell = t.at(c.row, c.column);
  const Note* note = cell.kind == Cell::On ? &notesRef(*v.p, t.channel)[static_cast<size_t>(cell.note)] : nullptr;
  const std::string n = std::to_string(slot + 1 == 10 ? 0 : slot + 1);
  if (c.field != Field::Volume && c.field != Field::Effect) {
    say(i18n::tr("The macros store and write a volume or an effect: put the cursor on that field"));
    return 0;
  }
  const bool vol = c.field == Field::Volume;
  Macro& m = vol ? macros(a).vol[slot] : macros(a).fx[slot];
  const std::string kind = i18n::tr(vol ? "Volume macro" : "Effect macro");
  if (store) {
    if (vol && note) {
      m = {true, std::min(note->velocity, 255), 0};
    } else if (!vol && note && !trk::effectsOf(*note).empty()) {
      const trk::Effect e = trk::effectsOf(*note).front();
      m = {true, e.value, e.letter};
    } else {
      say(i18n::tr(vol ? "No note here: a volume macro stores a note's volume" : "No effect here: an effect macro stores a note's effect"));
      return 0;
    }
    saveMacros(a);
    say(kind + " " + n + ": " + (vol ? trk::hex2(m.value) : std::string(1, m.letter) + trk::hex2(m.value)));
    return 0;
  }
  if (!m.set) return say(kind + " " + n + " " + i18n::tr("is empty: Shift+Alt+") + n + " " + i18n::tr("stores the field at the cursor")), 0;
  if (!s.edit) return say(i18n::tr("Edit mode is off: Esc turns it on (the keys only play)")), 0;
  if (!note) return say(i18n::tr("No note here")), 0;
  trk::Result r;
  if (vol) {
    r = applyEdit(a, v, c.track, "macro", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setVolume(x.notes, x.steps, tr, c.row, c.column, std::min(m.value, 127));
    });
  } else {
    const std::vector<trk::Effect> fx = trk::effectsOf(*note);
    const char shown = fx.empty() ? 0 : fx.front().letter;
    r = applyEdit(a, v, c.track, "macro", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setEffectAt(x.notes, tr, c.row, c.column, shown, m.letter, m.value);
    });
  }
  if (r.refused.empty()) moveRow(v, s.step, true);
  return 1;
}

// One of those keys: 0 handled (no song change), 1 handled (the song may have changed), -1 not one.
int trackerKey(App& a, const View& v, const KeyEv& k) {
  if (!trackerKeySet()) return -1;
  State& s = st();
  Cursor& c = s.cur;
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, shift = (k.mods & KMOD_SHIFT) != 0, alt = (k.mods & KMOD_ALT) != 0;
  const int tracks = static_cast<int>(v.tracks.size());
  // N-04: the play keys (the classic tracker's).
  {
    const bool rctrl = k.sc == SDL_SCANCODE_RCTRL, ralt = k.sc == SDL_SCANCODE_RALT, enter = k.sc == SDL_SCANCODE_RETURN;
    if ((rctrl && !alt) || (enter && !ctrl && !shift)) {  // the song, from the current order entry
      if (k.repeat) return 0;
      const Tick pos = songPosition(a);
      Tick from = 0;
      for (const trk::OrderEntry& e : trk::orderList(a.doc.song()))
        if (pos >= e.start && pos < e.end) from = e.start;
      a.engine.setMode(PlayMode::Song);
      setSongPosition(a, from);
      if (!a.engine.playing()) menuparity::playPressed(a);
      return 0;
    }
    if (k.sc == SDL_SCANCODE_RSHIFT) {  // Right Shift tapped: record the pattern (as the header's Record pattern)
      a.engine.setMode(PlayMode::Pattern);
      a.engine.setPattern(a.curPattern);
      a.recordArmed = true;
      if (!a.engine.playing()) menuparity::playPressed(a);
      return 0;
    }
    if (ralt || (enter && (ctrl || shift))) {  // the pattern, from its start (Shift+Enter: from the cursor's row)
      if (k.repeat) return 0;
      a.engine.setMode(PlayMode::Pattern);
      a.engine.setPattern(a.curPattern);
      a.engine.setPosition(enter && shift ? static_cast<double>(static_cast<Tick>(s.cur.row) * v.g.rowTicks) : 0.0, true);
      if (!a.engine.playing()) menuparity::playPressed(a);
      return 0;
    }
  }
  const char* noEdit = "Edit mode is off: Esc turns it on (the keys only play)";
  // F9 to F12: jump rows.
  if (k.sc >= SDL_SCANCODE_F9 && k.sc <= SDL_SCANCODE_F12) {
    const int j = k.sc - SDL_SCANCODE_F9;
    if (shift) {
      s.jumps[j] = c.row;
      say(std::string(i18n::tr("Jump")) + " " + std::to_string(j + 1) + ": " + i18n::tr("row") + " " + trk::rowText(c.row, true));
      return 0;
    }
    const int row = std::clamp(s.jumps[j] >= 0 ? s.jumps[j] : v.g.rows * j / 4, 0, std::max(0, v.g.rows - 1));
    s.sel.on = false;
    c.row = row;
    if (ctrl) {  // play the pattern from there
      a.engine.setMode(PlayMode::Pattern);
      a.engine.setPattern(a.curPattern);
      a.engine.setPosition(static_cast<double>(static_cast<Tick>(row) * v.g.rowTicks), true);
      if (!a.engine.playing()) menuparity::playPressed(a);
    }
    return 0;
  }
  // Transpose: F1 / F2 a semitone down / up, F7 / F8 the same for the cursor's instrument (its
  // track: every track is one instrument here). Shift: the track, Ctrl: the pattern, Alt: the block.
  if (k.sc == SDL_SCANCODE_F1 || k.sc == SDL_SCANCODE_F2 || k.sc == SDL_SCANCODE_F7 || k.sc == SDL_SCANCODE_F8) {
    const int d = k.sc == SDL_SCANCODE_F1 || k.sc == SDL_SCANCODE_F7 ? -1 : 1;
    const bool all = k.sc == SDL_SCANCODE_F1 || k.sc == SDL_SCANCODE_F2;
    const TransposeScope scope = alt ? TransposeScope::Block : ctrl && all ? TransposeScope::Pattern : TransposeScope::Track;
    transposeScope(a, scope, d);
    return 1;
  }
  // Shift+F3 / F4 / F5: cut / copy / paste the track; Ctrl: the pattern (every track).
  if ((k.sc == SDL_SCANCODE_F3 || k.sc == SDL_SCANCODE_F4 || k.sc == SDL_SCANCODE_F5) && (shift || ctrl) && !alt) {  // (Alt: the block, below)
    const Cursor keep = c;
    const Selection keepSel = s.sel;
    const Buf which = ctrl ? Buf::Pattern : Buf::Track;
    if (k.sc == SDL_SCANCODE_F5) {
      if (!s.edit) return say(i18n::tr(noEdit)), 1;
      const trk::Block& src = buffer(which);
      // R7-09: a pattern of another length: asked first, as the classic editor does, whether this
      // pattern takes the copy's length (Gridline's lengths are whole bars: asked when it can).
      const int spb = std::max(1, v.p->stepsPerBar);
      if (ctrl && !src.empty() && src.rows != v.g.rows && src.rows % spb == 0 && src.rows / spb <= kMaxPatternBars) {
        const ObjectId pid = v.p->id;
        const int bars = src.rows / spb, rows = src.rows;
        a.confirm.open = true;
        a.confirm.title = i18n::tr("Make this pattern as long as the copy?");
        a.confirm.subtitle = std::to_string(v.g.rows) + " " + i18n::tr("rows here") + ", " + std::to_string(rows) + " " +
                             i18n::tr("copied. Either way the whole pattern is replaced; rows past the copy are cleared.");
        a.confirm.okLabel = i18n::N_("Match length");
        a.confirm.noLabel = i18n::N_("Keep length");
        a.confirm.action = [&a, pid, bars] { pasteWhole(a, Buf::Pattern, pid, bars); };
        a.confirm.noAction = [&a, pid] { pasteWhole(a, Buf::Pattern, pid, 0); };
        return 1;
      }
      if (ctrl) c.track = 0;
      c.row = 0;
      pasteBlock(v, a, which);
      c = keep;
      if (ctrl && !src.empty() && src.rows != v.g.rows)
        say(std::to_string(src.rows) + " " + i18n::tr("rows pasted; the pattern keeps its length (a whole number of bars)"));
      return 1;
    }
    s.sel = {true, ctrl ? 0 : c.track, 0, ctrl ? tracks - 1 : c.track, std::max(0, v.g.rows - 1)};
    const bool cut = k.sc == SDL_SCANCODE_F3;
    if (cut && !s.edit) say(i18n::tr(noEdit));
    else copyBlock(v, cut, a, which);
    s.sel = keepSel;
    return cut ? 1 : 0;
  }
  if (k.sc == SDL_SCANCODE_GRAVE) {  // the edit step, one more (wraps after 16) or, with Shift, one fewer
    setEditStep(shift ? (s.step == 0 ? 16 : s.step - 1) : (s.step >= 16 ? 0 : s.step + 1));
    say(std::string(i18n::tr("Edit step")) + " " + std::to_string(s.step));
    return 0;
  }
  if (k.sc == SDL_SCANCODE_DELETE && shift && !ctrl && !alt && !s.sel.on) {  // the whole cell: note, volume and effect
    if (!s.edit) return say(i18n::tr(noEdit)), 0;
    applyEdit(a, v, c.track, "clear", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
      return trk::clearField(x.notes, x.steps, x.props, tr, g, c.row, c.column, Field::Note);
    });
    moveRow(v, s.step, true);
    return 1;
  }
  if (k.sc == SDL_SCANCODE_CAPSLOCK) {  // a note off, as the key left of Z
    if (!s.edit) return say(i18n::tr(noEdit)), 0;
    applyEdit(a, v, c.track, "note off", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) { return trk::putOff(x.notes, tr, g, c.row, c.column); });
    moveRow(v, s.step, true);
    return 1;
  }
  if (alt && !ctrl && k.sc >= SDL_SCANCODE_1 && k.sc <= SDL_SCANCODE_0) return macroKey(a, v, k.sc - SDL_SCANCODE_1, shift);
  // Ctrl+keypad plus and minus: the master volume up or down (about a sixteenth a press, as the classic).
  if (ctrl && !alt && (k.sc == SDL_SCANCODE_KP_PLUS || k.sc == SDL_SCANCODE_KP_MINUS)) {
    const Song& song = a.doc.song();
    if (song.mixer.empty()) return 0;
    const ParamId id = mixerParam(song.masterId(), MixerParam::Volume);
    const double now = getParam(song, id).value_or(0.8);
    const double to = std::clamp(std::round((now + (k.sc == SDL_SCANCODE_KP_PLUS ? 0.0625 : -0.0625)) * 1000.0) / 1000.0, 0.0, 1.0);
    a.doc.setParam(id, to, true);
    say(std::string(i18n::tr("Master volume")) + " " + std::to_string(static_cast<int>(std::lround(to * 100))) + "%");
    return 1;
  }
  // The keypad picks instruments as the classic one: 1 to 8 the instruments of the keypad's group
  // of eight, Enter the next group (wraps); the cursor goes to the instrument's track.
  if (!ctrl && !alt && ((k.sc >= SDL_SCANCODE_KP_1 && k.sc <= SDL_SCANCODE_KP_8) || k.sc == SDL_SCANCODE_KP_ENTER)) {
    static int group = 0;
    const Song& song = a.doc.song();
    const int n = static_cast<int>(song.channels.size()), groups = std::max(1, (n + 7) / 8);
    group = std::clamp(group, 0, groups - 1);
    if (k.sc == SDL_SCANCODE_KP_ENTER) {
      group = (group + 1) % groups;
      say(std::string(i18n::tr("Keypad instruments")) + " " + trk::hex2(group * 8 + 1) + "-" + trk::hex2(std::min(n, group * 8 + 8)));
      return 0;
    }
    const int i = group * 8 + (k.sc - SDL_SCANCODE_KP_1);
    if (i >= n) return say(std::string(i18n::tr("No instrument")) + " " + trk::hex2(i + 1)), 0;
    const ObjectId id = song.channels[static_cast<size_t>(i)].id;
    a.selectChannel(id);
    cursorToChannel(a, id);
    return 0;
  }
  if (alt && !ctrl) {
    if (const int j = trackJump(k.sc); j >= 0) {
      if (j < tracks) s.sel.on = false, c.track = j, c.column = 0, c.field = Field::Note, c.digit = 0;
      return 0;
    }
    if (k.sc == SDL_SCANCODE_C) {  // mark the cursor's track
      s.sel = {true, c.track, 0, c.track, std::max(0, v.g.rows - 1)};
      return 0;
    }
    if (k.sc == SDL_SCANCODE_DELETE) {  // the effect
      if (!s.edit) return say(i18n::tr(noEdit)), 0;
      applyEdit(a, v, c.track, "clear effect", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::clearField(x.notes, x.steps, x.props, tr, g, c.row, c.column, Field::Effect);
      });
      moveRow(v, s.step, true);
      return 1;
    }
    // Alt+arrows mark a block, as Shift+arrows do. (Alt+F3 to F5, the block's cut, copy and paste,
    // go on to the common keys: T-02.)
    if (k.sc != SDL_SCANCODE_UP && k.sc != SDL_SCANCODE_DOWN && k.sc != SDL_SCANCODE_LEFT && k.sc != SDL_SCANCODE_RIGHT) return -1;
    if (!s.sel.on) s.sel = {true, c.track, c.row, c.track, c.row};
    if (k.sc == SDL_SCANCODE_UP || k.sc == SDL_SCANCODE_DOWN) moveRow(v, k.sc == SDL_SCANCODE_UP ? -1 : 1, false);
    else c.track = std::clamp(c.track + (k.sc == SDL_SCANCODE_LEFT ? -1 : 1), 0, tracks - 1);
    s.sel.t1 = c.track, s.sel.r1 = c.row;
    return 0;
  }
  if (ctrl) {
    if (shift && (k.sc == SDL_SCANCODE_UP || k.sc == SDL_SCANCODE_DOWN)) {  // the previous / next instrument (its track)
      s.sel.on = false;
      c.track = std::clamp(c.track + (k.sc == SDL_SCANCODE_UP ? -1 : 1), 0, tracks - 1);
      c.column = 0, c.field = Field::Note, c.digit = 0;
      return 0;
    }
    if (shift && (k.sc == SDL_SCANCODE_LEFT || k.sc == SDL_SCANCODE_RIGHT)) {  // R10-04: the previous / next song position
      stepSongPosition(a, k.sc == SDL_SCANCODE_LEFT ? -1 : 1);
      return 1;
    }
    if (k.sc == SDL_SCANCODE_LEFT || k.sc == SDL_SCANCODE_RIGHT) {  // the previous / next pattern
      stepPattern(a, k.sc == SDL_SCANCODE_LEFT ? -1 : 1);
      return 1;
    }
    if (k.sc == SDL_SCANCODE_DELETE) {  // volume and effect
      if (!s.edit) return say(i18n::tr(noEdit)), 0;
      a.doc.beginGroup("clear volume and effect");
      for (Field f : {Field::Volume, Field::Effect})
        applyEdit(a, v, c.track, "clear volume and effect", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
          return trk::clearField(x.notes, x.steps, x.props, tr, g, c.row, c.column, f);
        });
      a.doc.endGroup(false);
      moveRow(v, s.step, true);
      return 1;
    }
  }
  return -1;
}

// One key. True when it may have changed the song (the view is built again).
bool handleKey(App& a, const View& v, const KeyEv& k) {
  State& s = st();
  Cursor& c = s.cur;
  if (k.sc < 0) {  // a note key let go while recording (record key-off): its note ends on the row playing
    const int ti = -1 - k.sc;
    if (!v.p || ti >= static_cast<int>(v.tracks.size()) || s.playRow < 0 || s.playRow >= v.g.rows) return false;
    const int row = s.playRow, col = k.kc;
    applyEdit(a, v, ti, "record key-off", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) { return trk::putOff(x.notes, tr, g, row, col); });
    return true;
  }
  if (!v.tracks.empty() && v.p)
    if (const int r = trackerKey(a, v, k); r >= 0) return r == 1;
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, shift = (k.mods & KMOD_SHIFT) != 0, alt = (k.mods & KMOD_ALT) != 0;
  if (v.tracks.empty() || !v.p) return false;
  auto extend = [&](auto&& move) {
    if (shift) {
      if (!s.sel.on) s.sel = {true, c.track, c.row, c.track, c.row};
      move();
      s.sel.t1 = c.track, s.sel.r1 = c.row;
    } else {
      s.sel.on = false;
      move();
    }
  };
  switch (k.sc) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN: {
      const int d = k.sc == SDL_SCANCODE_UP ? -1 : 1;
      if (ctrl) {
        transposeBlock(a, -d * (shift ? 12 : 1));  // Up: higher (d is the row direction)
        return true;
      }
      extend([&] { moveRow(v, d, !shift); });
      return false;
    }
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: {
      const int d = k.sc == SDL_SCANCODE_LEFT ? -1 : 1;
      if (shift) extend([&] { c.track = std::clamp(c.track + d, 0, static_cast<int>(v.tracks.size()) - 1); });
      else s.sel.on = false, moveDigit(v, d);
      return false;
    }
    case SDL_SCANCODE_PAGEUP: extend([&] { moveRow(v, -16, false); }); return false;
    case SDL_SCANCODE_PAGEDOWN: extend([&] { moveRow(v, 16, false); }); return false;
    case SDL_SCANCODE_HOME: extend([&] { c.row = 0; }); return false;
    case SDL_SCANCODE_END: extend([&] { c.row = v.g.rows - 1; }); return false;
    case SDL_SCANCODE_TAB:
      s.sel.on = false;
      c.track = (c.track + (shift ? -1 : 1) + static_cast<int>(v.tracks.size())) % static_cast<int>(v.tracks.size());  // wraps
      c.column = 0, c.field = Field::Note, c.digit = 0;
      return false;
    case SDL_SCANCODE_ESCAPE:
      if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
      if (s.sel.on) s.sel.on = false;
      else setEditMode(!s.edit);
      return false;
    case SDL_SCANCODE_KP_DIVIDE:
    case SDL_SCANCODE_KP_MULTIPLY:
      a.typing.octave = std::clamp(a.typing.octave + (k.sc == SDL_SCANCODE_KP_DIVIDE ? -1 : 1), 0, 8);
      say(std::string(i18n::tr("Octave")) + " " + std::to_string(a.typing.octave));
      return false;
    default: break;
  }
  if (alt && !ctrl) {
    if (k.sc == SDL_SCANCODE_F3) return copyBlock(v, true, a), true;
    if (k.sc == SDL_SCANCODE_F4) return copyBlock(v, false, a), false;
    if (k.sc == SDL_SCANCODE_F5) return pasteBlock(v, a), true;
    return false;
  }
  if (ctrl) {
    if (k.kc == 'c') return copyBlock(v, false, a), false;
    if (k.kc == 'x') return copyBlock(v, true, a), true;
    if (k.kc == 'v') return pasteBlock(v, a), true;
    if (k.sc >= SDL_SCANCODE_1 && k.sc <= SDL_SCANCODE_0) {  // Ctrl+0..9: the edit step
      setEditStep(k.sc == SDL_SCANCODE_0 ? 0 : k.sc - SDL_SCANCODE_1 + 1);
      return false;
    }
    if (k.sc == SDL_SCANCODE_MINUS) return setEditStep(s.step - 1), false;
    if (k.sc == SDL_SCANCODE_EQUALS) return setEditStep(s.step + 1), false;
    return false;
  }
  const int ti = c.track;
  const trk::Track& t = v.tracks[static_cast<size_t>(ti)];
  const Channel* ch = v.channels[static_cast<size_t>(ti)];
  const char* noEdit = "Edit mode is off: Esc turns it on (the keys only play)";
  // Record pattern / Record song (the classic record modes): the keys edit the row playing and the
  // cursor stays with it; in edit mode they edit the cursor's row and the cursor moves on by the
  // edit step. Neither: the keys only play.
  const bool rec = a.recordArmed && a.engine.playing() && s.playRow >= 0 && s.playRow < v.g.rows;
  const bool canEdit = s.edit || rec;
  int row = rec ? s.playRow : c.row;
  // cp15: record quantize (the classic Quantize): a recorded note goes to the nearest row that is a
  // multiple of the snap (1: the nearest row), wrapping to the pattern's start; off, the row playing.
  if (rec && gRecSnap > 0 && v.g.rows > 0) row = trk::snapRecordRow(s.playRow, s.playFrac, gRecSnap, v.g.rows);
  auto stepOn = [&] {
    if (!rec) moveRow(v, s.step, true);
  };
  const Cell& cell = t.at(row, c.column);
  // K-106: a row an imported module's voice sounds through: its volume column and effect are the
  // module's own (trk::setModuleVolume / setModuleEffect say which edits are stored exactly).
  int modK = -1;
  const int modNote = c.field == Field::Volume || c.field == Field::Effect ? trk::moduleNoteAt(notesRef(*v.p, t.channel), t, v.g, row, c.column, &modK) : -1;
  switch (k.sc) {
    case SDL_SCANCODE_DELETE:
      if (!canEdit) return say(i18n::tr(noEdit)), false;
      if (s.sel.on) return clearBlock(v, a), true;
      if (modNote >= 0) {
        const trk::Result r = applyEdit(a, v, ti, "clear", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
          return c.field == Field::Volume ? trk::setModuleVolume(x.notes, tr, g, row, c.column, -1) : trk::setModuleEffect(x.notes, tr, g, row, c.column, -1, 0);
        });
        if (r.refused.empty()) stepOn();
        return true;
      }
      applyEdit(a, v, ti, "clear", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::clearField(x.notes, x.steps, x.props, tr, g, row, c.column, c.field);
      });
      stepOn();
      return true;
    case SDL_SCANCODE_INSERT:
    case SDL_SCANCODE_BACKSPACE: {
      if (!s.edit) return say(i18n::tr(noEdit)), false;
      // Backspace takes the row above the cursor and pulls the rest up, the cursor with them (on
      // the first row there is nothing above: nothing happens).
      if (k.sc == SDL_SCANCODE_BACKSPACE) {
        if (c.row == 0) return false;
        --c.row;
      }
      rowEdit(a, v, k.sc == SDL_SCANCODE_INSERT, shift);
      return true;
    }
    default: break;
  }
  // ---- entry ----
  if (c.field == Field::Note) {
    const int pitch = notePitch(a, k.sc);
    if (pitch >= 0) {
      // A held key repeats only in edit mode (the note goes on down the rows by the edit step), as
      // the classic editor's key repeat; playing or recording, a held key is one note.
      if (k.repeat && (!s.edit || rec || gMulti)) return false;
      // Multichannel key jazz (Settings): notes only played go to the enabled tracks in turn, so
      // a chord sounds on several instruments.
      ObjectId playOn = ch->id;
      if (!canEdit && gMultiJazz && v.tracks.size() > 1) {
        static int jazz = -1;
        jazz = nextMultiTrack(a, v.tracks, jazz < 0 || jazz >= static_cast<int>(v.tracks.size()) ? ti : jazz);
        playOn = v.tracks[static_cast<size_t>(jazz)].channel;
      }
      if (!k.repeat) audition(a, k.sc, playOn, pitch, a.typing.velocity);
      // Record pattern / Record song: the note lands on the row playing (edit mode or not).
      if (rec) {
        applyEdit(a, v, ti, "record note", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
          return trk::putNote(x.notes, tr, g, row, c.column, pitch, a.typing.velocity);
        });
        if (auto it = s.held.find(k.sc); it != s.held.end()) it->second.track = ti, it->second.column = c.column;  // record key-off
        // Multichannel record: the next note goes to the next enabled track.
        if (gMultiRec && v.tracks.size() > 1) c.track = nextMultiTrack(a, v.tracks, ti), c.column = 0, c.digit = 0;
        return true;
      }
      if (!s.edit) return false;
      // A song without pattern clips: the pattern typed in becomes its first order entry (as a new
      // classic song starts with entry 00), so Play song plays it. One undo step with the note.
      const bool first = !trk::hasPatternEntries(a.doc.song());
      // Multichannel edit (Settings; R4-09): each note goes to the cursor's track and the cursor to
      // the next enabled track (a scope's right-click enables it). Notes held together are a chord:
      // they land on the row the first of them went to, the row moving on once (by the edit step).
      const bool multi = gMulti && v.tracks.size() > 1;
      const bool chord = multi && s.chordRow >= 0 && s.chordRow < v.g.rows && otherNoteHeld(k.sc);
      const int at = chord ? s.chordRow : c.row;
      // R5-08: notes typed together (multichannel edit) are one undo step: a group opens with the
      // first and closes when no note key is held any more (pumpPage, every frame).
      if (multi && !s.chordGroup) {
        a.doc.beginGroup("type chord");
        s.chordGroup = true;
      }
      if (first) a.doc.beginGroup("type note");
      applyEdit(a, v, ti, "type note", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::putNote(x.notes, tr, g, at, c.column, pitch, a.typing.velocity);
      });
      if (first) {
        a.doc.addClip(a.curPattern, 0, 0);
        a.doc.endGroup(false);
      }
      if (!chord) {
        s.chordRow = c.row;
        moveRow(v, s.step, true);
      }
      if (multi) c.track = nextMultiTrack(a, v.tracks, ti), c.column = 0, c.digit = 0;
      return true;
    }
    if (isOffKey(k.sc)) {
      if (!canEdit) return say(i18n::tr(noEdit)), false;
      applyEdit(a, v, ti, "note off", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) { return trk::putOff(x.notes, tr, g, row, c.column); });
      stepOn();
      return true;
    }
    return false;
  }
  if (!canEdit) {
    if (hexDigit(k.kc) >= 0) say(i18n::tr(noEdit));
    return false;
  }
  // Each digit or letter typed into a value goes in and the cursor moves down by the edit step,
  // staying on the same digit (the classic editor: a column of values is typed straight down;
  // Right moves on to the next digit, an edit step of 0 keeps the row).
  auto advance = [&] { stepOn(); };
  if (modNote >= 0) {
    const std::vector<tracker::Cell> mcs = trk::moduleRows(notesRef(*v.p, t.channel)[static_cast<size_t>(modNote)], modK + 1);
    if (modK >= static_cast<int>(mcs.size())) return false;
    const tracker::Cell mc = mcs[static_cast<size_t>(modK)];
    if (c.field == Field::Volume) {
      const int nib = hexDigit(k.kc);
      if (nib < 0) return false;
      const int now = mc.vol >= 0x10 && mc.vol <= 0x50 ? mc.vol - 0x10 : 0;
      const int want = std::min(setNibble(now, c.digit, nib), 0x40);
      const trk::Result r = applyEdit(a, v, ti, "volume", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::setModuleVolume(x.notes, tr, g, row, c.column, want);
      });
      if (r.refused.empty()) {
        say(std::string(i18n::tr("Module volume")) + " " + trk::hex2(want) + " " + i18n::tr("of 40"));
        advance();
      }
      return true;
    }
    int efx = mc.efx, param = mc.param;
    if (c.digit == 0) {
      const int up = k.kc >= 'a' && k.kc <= 'z' ? k.kc - 'a' + 'A' : k.kc;
      efx = up >= '0' && up <= '9' ? up - '0' : up >= 'A' && up <= 'Z' ? up - 'A' + 10 : -1;
      if (efx < 0) return false;
      if (efx != mc.efx || (mc.efx == 0 && mc.param == 0)) param = efx == 0x0C ? 0x40 : 0x80;  // a new effect starts at full volume, centre pan
    } else {
      const int nib = hexDigit(k.kc);
      if (nib < 0) return false;
      if (mc.efx == 0 && mc.param == 0) return say(i18n::tr("Type the effect first: 8 (pan) or C (volume)")), false;
      param = setNibble(mc.param, c.digit - 1, nib);
    }
    const trk::Result r = applyEdit(a, v, ti, "effect", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
      return trk::setModuleEffect(x.notes, tr, g, row, c.column, efx, param);
    });
    if (r.refused.empty()) {
      say(efx == 0x0C ? std::string(i18n::tr("Module effect C: set volume")) + " " + trk::hex2(std::min(param, 0x40))
                      : std::string(i18n::tr("Module effect 8: set pan")) + " " + trk::hex2(param));
      if (c.digit >= 2) advance();
    }
    return true;
  }
  const Note* note = cell.kind == Cell::On ? &notesRef(*v.p, t.channel)[static_cast<size_t>(cell.note)] : nullptr;
  if (c.field == Field::Effect) {
    const std::vector<trk::Effect> fx = note ? trk::effectsOf(*note) : std::vector<trk::Effect>{};
    const char shown = fx.empty() ? 0 : fx.front().letter;
    int value = fx.empty() ? 0 : fx.front().value;
    char letter = shown;
    char offDigit = 0;  // G or P given a digit other than 0 or 1
    static bool rHigh = false;  // R's high digit was past 7 (the value stops at 7F)
    // The letter's own value as the note has it (its default when the note has none).
    auto valueOf = [note](char l) {
      const Note probe = note ? *note : Note{};
      return l == 'G' || l == 'P' ? 1 : l == 'X' ? probe.modX : l == 'Y' ? probe.modY : l == 'R' ? probe.release : probe.fine & 0xFF;
    };
    if (c.digit == 0) {
      const int up = k.kc >= 'a' && k.kc <= 'z' ? k.kc - 'a' + 'A' : k.kc;
      if (!std::strchr(trk::kEffectLetters, up) || up == 0) return false;
      letter = static_cast<char>(up);
      if (letter != shown) value = valueOf(letter);
    } else {
      const int nib = hexDigit(k.kc);
      if (nib < 0) return false;
      if (!shown) {
        // R6-01: a letter typed at its default value (T00, X80 ...) changes nothing in the note, so it
        // waits on the note ("T.."): the digits then set it.
        const char held = pendingEffect(a, v.p->id, t.channel, note);
        if (!held) return say(i18n::tr("Type the effect letter first: G P T X Y R")), false;
        letter = held;
        value = valueOf(held);
      }
      value = setNibble(value, c.digit - 1, nib);
      // R7-10: G and P are on or off: a digit that is neither 0 nor 1 (or a high digit past 0) says so.
      if ((letter == 'G' || letter == 'P') && (c.digit == 1 ? nib != 0 : nib > 1)) offDigit = letter;
      if (letter == 'R' && c.digit == 1) rHigh = nib > 7;
      if (letter == 'R' && (value > 0x7F || (c.digit == 2 && rHigh))) offDigit = 'R';
    }
    const Tick nStart = note ? note->start : 0;  // (the edit may move the notes: kept by value)
    const int nPitch = note ? note->pitch : 0;
    const trk::Result r = applyEdit(a, v, ti, "effect", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setEffectAt(x.notes, tr, row, c.column, shown, letter, value);
    });
    if (r.refused.empty()) {
      // Nothing changed (the letter at its default): the letter waits on this note for its digits.
      // The letter still at its default value (T00 typed, or its first digit 0): it keeps waiting.
      Note probe;
      trk::setEffect(probe, letter, value);
      gPendFx = {};
      if (note && letter && trk::effectsOf(probe).empty()) gPendFx = {v.p->id, t.channel, nStart, nPitch, letter, a.doc.revision()};
      if (offDigit == 'R')  // (round 08) the release velocity stops at 7F
        say(i18n::tr("R is a release velocity, 00 to 7F: it stays at 7F"));
      else if (offDigit)
        say(std::string(1, offDigit) + i18n::tr(" is on or off: it shows 01 whatever the digits (Delete turns it off)"));
      else
        say(trk::effectHelp(letter, value));
      // R7-04: the edit step applies once the effect is complete (its last digit): the letter and
      // the high digit keep the row, so the digits after them land on the same note.
      if (c.digit >= 2) advance();
    }
    return true;
  }
  const int nib = hexDigit(k.kc);
  if (nib < 0) return false;
  if (c.field == Field::Instrument) {
    if (cell.kind != Cell::On) return say(i18n::tr(cell.kind == Cell::Step ? "A rack step belongs to its channel" : "No note here")), false;
    const int now = a.doc.song().channelIndex(t.channel) + 1;
    const int want = setNibble(now, c.digit, nib);
    advance();
    if (want != now) moveToChannel(a, v, ti, cell.note, want - 1);
    return true;
  }
  int now = 0;
  if (c.field == Field::Volume) {
    now = note ? note->velocity : cell.kind == Cell::Step ? stepsRef(*v.p, t.channel)[static_cast<size_t>(cell.step)] : 100;
    const int want = setNibble(now, c.digit, nib);
    const trk::Result r = applyEdit(a, v, ti, "volume", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setVolume(x.notes, x.steps, tr, row, c.column, std::min(want, 127));
    });
    if (r.refused.empty()) {
      // cp24 (usability): the value as it now is, and how to reach the other digit (a first session
      // typed "2 0" for 20 and got 24 on this row and "no note here" on the next).
      say(std::string(i18n::tr("Volume")) + " " + trk::hex2(std::min(want, 127)) + " " + i18n::tr("of 7F") + "   " +
          i18n::tr(c.digit == 0 ? "Right: the low digit (each digit moves down by the edit step)" : "Left: the high digit"));
      advance();
    }
    return true;
  }
  if (c.field == Field::Pan) {
    const std::vector<StepProps>& pr = propsRef(*v.p, t.channel);
    now = note ? note->pan + 64 : cell.kind == Cell::Step && static_cast<size_t>(cell.step) < pr.size() ? pr[static_cast<size_t>(cell.step)].pan + 64 : 64;
    const int want = setNibble(now, c.digit, nib);
    const trk::Result r = applyEdit(a, v, ti, "pan", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setPan(x.notes, x.props, tr, row, c.column, std::min(want, 128));
    });
    if (r.refused.empty()) {
      say(std::string(i18n::tr("Pan")) + " " + trk::hex2(std::min(want, 128)) + " " + i18n::tr("(40 centre)") + "   " +
          i18n::tr(c.digit == 0 ? "Right: the low digit (each digit moves down by the edit step)" : "Left: the high digit"));
      advance();
    }
    return true;
  }
  return false;
}

// ---- drawing ----
// Characters a note column takes: the shown fields (note ins vol pan fx), a space between.
int noteChars() {
  int n = -1;
  for (int f = 0; f < static_cast<int>(Field::Count); ++f)
    if (gShow[f]) n += trk::fieldChars(static_cast<Field>(f)) + 1;
  return n;
}

// Character offset of a field inside a note column.
int fieldOffset(Field f) {
  int x = 0;
  for (int k = 0; k < static_cast<int>(f); ++k)
    if (gShow[k]) x += trk::fieldChars(static_cast<Field>(k)) + 1;
  return x;
}

ImU32 fieldColour(Field f) {
  const auto& P = pal();
  switch (f) {
    case Field::Note: return P.text;
    case Field::Instrument: return P.hot;
    case Field::Volume: return P.led;
    case Field::Pan: return P.textDim;
    case Field::Effect: return P.accent;
    default: return P.text;
  }
}

}  // namespace

// ---- shared view state ----
int editStep() { return st().step; }
void setEditStep(int n) {
  st().step = std::clamp(n, 0, 16);
  say(std::string(i18n::tr("Edit step")) + " " + std::to_string(st().step));
}
bool editMode() { return st().edit; }
void setEditMode(bool on) {
  st().edit = on;
  say(on ? i18n::tr("Edit mode: the keys write notes") : i18n::tr("Edit mode off: the keys play"));
}
bool shown() { return ImGui::GetFrameCount() - st().drawnFrame <= 1; }
bool hasKeyboard() { return st().hasKb && shown(); }

void transposeBlock(App& a, int semis) {
  const View v = build(a);
  if (!v.p || v.tracks.empty()) return;
  clampCursor(v);
  int t0, t1, r0, r1;
  blockRange(&t0, &t1, &r0, &r1);
  int refused = 0;
  a.doc.beginGroup("transpose");
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
    refused += applyEdit(a, v, t, "transpose", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::transpose(x.notes, g, r0, r1, semis); }).refusedCount;
  a.doc.endGroup(false);
  if (refused > 0) say(std::to_string(refused) + " " + i18n::tr("notes would leave the note range and stay where they are"));
}

namespace {
bool gExtended = false;
}
bool extended() { return gExtended; }
float patternRowHeight() { return st().rowH; }
// Whether a centre page is the one shown in its dock node this frame.
static bool windowFrontNow(const char* window) {
  const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + window).c_str());
  return w && w->DockIsActive && w->DockNode && w->DockNode->VisibleWindow == w;
}

void zoomPattern(App& a, int d) {
  loadColumns(a);
  int z = gZoomEff;
  bool fit = false;
  if (d == 0) fit = true;                        // every track in view (the step is found as drawn)
  else if (d == 99) z = 0;                       // back to the plain view
  else z = std::clamp(z - (d > 0 ? 1 : -1), 0, kZoomMax);  // wider: a step down; narrower: a step up
  gZoom = z, gZoomFit = fit;
  if (!fit) gZoomEff = z;
  a.settings.set("tracker.patternZoom", static_cast<double>(z));
  a.settings.set("tracker.patternFit", fit);
  zoomColumns(gZoomEff);
  st().scrolledFor = -1;  // (the cursor's track brought into view again at the new widths)
}
int patternZoom() { return gZoomEff; }
bool patternFit() { return gZoomFit; }
void zoomFront(App& a, int d) {
  if (windowFrontNow("Playlist")) trackerpages::zoomSong(a, d);
  else zoomPattern(a, d);
}

namespace {
// cp16: the per-part frame timers (PerfScope): this frame's sums, and a ring of the last 600 frames'.
constexpr int kPerfFrames = 600;
struct Perf {
  double acc[kPerfParts] = {};
  bool drew[kPerfParts] = {};
  int frame = -1;
  float ring[kPerfParts][kPerfFrames] = {};
  int n[kPerfParts] = {}, at[kPerfParts] = {};
};
Perf& perf() {
  static Perf p;
  return p;
}
void perfRoll() {  // a new frame: the last one's sums into the rings
  Perf& p = perf();
  const int f = ImGui::GetFrameCount();
  if (f == p.frame) return;
  for (int k = 0; k < kPerfParts; ++k) {
    if (p.drew[k]) {
      p.ring[k][p.at[k]] = static_cast<float>(p.acc[k]);
      p.at[k] = (p.at[k] + 1) % kPerfFrames;
      p.n[k] = std::min(kPerfFrames, p.n[k] + 1);
    }
    p.acc[k] = 0;
    p.drew[k] = false;
  }
  p.frame = f;
}
}  // namespace

// (std::chrono, not SDL's counter: this file is also linked into the plugin bundles, which have no SDL.)
namespace {
unsigned long long perfNowNs() {
  return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace
PerfScope::PerfScope(PerfPart p) : p_(p), t0_(perfNowNs()) { perfRoll(); }
PerfScope::~PerfScope() {
  Perf& p = perf();
  p.acc[p_] += 1e-6 * static_cast<double>(perfNowNs() - t0_);
  p.drew[p_] = true;
}

void status(const std::string& text) { say(text); }
const std::string* freshStatus() { return !st().status.empty() && ImGui::GetTime() - st().statusT < 4.0 ? &st().status : nullptr; }
int trackBase(const App& a) { return a.settings.getNumber("tracker.trackBase") >= 1 ? 1 : 0; }

// R4-09, R5-07: the tracks multichannel edit leaves out, by channel, kept per project as this
// person's view state: never saved in the song, never in the app's settings (another project's
// channels are its own), and a moved track keeps its mark. The open project's set belongs to its
// session (the Document's project epoch: a new, opened or recovered project starts a new one), so
// two unsaved projects never share one; saving it under a name keeps the set, and the set is filed
// under that name for when the project is opened again in this run.
namespace {
std::set<ObjectId>& multiOff(const App& a) {
  static std::map<std::string, std::set<ObjectId>> byFile;  // saved projects' sets (this run)
  static std::set<ObjectId> current;
  static uint64_t epoch = ~uint64_t{0};
  static std::string file;
  const uint64_t now = a.doc.projectEpoch();
  const std::string& f = a.projectFile();
  if (now != epoch) {  // another project: file the old set, take the new one's (none: all on)
    if (!file.empty()) byFile[file] = current;
    epoch = now;
    file = f;
    const auto it = f.empty() ? byFile.end() : byFile.find(f);
    current = it == byFile.end() ? std::set<ObjectId>{} : it->second;
  } else if (f != file) {  // the same project saved (as): its set goes with it
    if (file.empty() && current.empty())
      if (const auto it = byFile.find(f); it != byFile.end()) current = it->second;  // (opened, then named this frame)
    file = f;
  }
  if (!file.empty()) byFile[file] = current;
  return current;
}
}  // namespace
bool multiTrackOn(App& a, ObjectId channel) {
  if (a.doc.song().channelIndex(channel) < 0) return false;
  return multiOff(a).count(channel) == 0;
}
void toggleMultiTrack(App& a, ObjectId channel) {
  if (a.doc.song().channelIndex(channel) < 0) return;
  std::set<ObjectId>& off = multiOff(a);
  if (!off.erase(channel)) off.insert(channel);
}

void keysBackToPattern() {
  if (windowFrontNow("Pattern editor")) st().wantFocus = true;
}
void keepKeysHere() { st().wantFocus = false; }

void setExtended(App& a, bool on) {
  if (gExtended == on) return;
  gExtended = on;
  // The band shrinks to one strip (the header draws it; the classic Extend): the pattern page comes
  // to the front with about 40 rows; the strip's own button brings the band back.
  if (on) {
    layoutui::setRegionVisible(a, "tracker.header", true);  // (a band hidden from View > Panels comes back as the strip)
    showPage(a, "Pattern editor");
    st().pendingHold = 4;
  }
}

namespace {
// The shown pattern's view with the cursor kept inside it (for the menus, outside the editor's frame).
bool menuView(App& a, View* v) {
  loadColumns(a);
  *v = build(a);
  if (!v->p || v->tracks.empty()) return false;
  clampCursor(*v);
  return true;
}
}  // namespace

void transposeScope(App& a, TransposeScope scope, int semis, bool current) {
  const Song& song = a.doc.song();
  const ObjectId inst = current ? a.selChannel : kNoId;
  if (current && song.channelIndex(inst) < 0) {
    say(i18n::tr("No instrument selected"));
    return;
  }
  int refused = 0;
  if (scope == TransposeScope::Song) {
    // Every pattern of the song (one undo step), the instrument's notes or all of them.
    std::vector<std::pair<ObjectId, ObjectId>> work;  // pattern, channel
    for (const Pattern& p : song.patterns)
      for (const auto& [ch, notes] : p.notes)
        if (!notes.empty() && (!current || ch == inst)) work.emplace_back(p.id, ch);
    a.doc.beginGroup("transpose");
    for (const auto& [pid, ch] : work) {
      const Pattern* p = a.doc.song().pattern(pid);
      if (!p) continue;
      const trk::Grid g = trk::gridOf(a.doc.song(), *p);
      Rows r = rowsOf(*p, ch);
      const Rows before = r;
      const trk::Result res = trk::transpose(r.notes, g, 0, std::max(g.rows - 1, 0) + 1024, semis);
      refused += res.refusedCount;
      if (res.changed) commit(a, pid, ch, r, before, "transpose");
    }
    a.doc.endGroup(false);
  } else {
    const View v = build(a);
    if (!v.p || v.tracks.empty()) return;
    clampCursor(v);
    int t0 = 0, t1 = static_cast<int>(v.tracks.size()) - 1, r0 = 0, r1 = std::max(v.g.rows - 1, 0) + 1024;
    if (scope == TransposeScope::Track) t0 = t1 = st().cur.track;
    if (scope == TransposeScope::Block) blockRange(&t0, &t1, &r0, &r1);
    bool any = false;
    a.doc.beginGroup("transpose");
    for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t) {
      if (current && v.tracks[static_cast<size_t>(t)].channel != inst) continue;
      any = true;
      refused += applyEdit(a, v, t, "transpose", [&](Rows& x, const trk::Track&, const trk::Grid& g) {
                   return trk::transpose(x.notes, g, r0, r1, semis);
                 }).refusedCount;
    }
    a.doc.endGroup(false);
    if (current && !any) {
      say(i18n::tr(scope == TransposeScope::Pattern ? "The current instrument has no track in this pattern"
                                                    : "The current instrument's notes are not in this track or block"));
      return;
    }
  }
  if (refused > 0) say(std::to_string(refused) + " " + i18n::tr("notes would leave the note range and stay where they are"));
}

// The Advanced edit box's instrument remap: the old instrument's notes (and rack steps, as notes)
// move to the new instrument's track, in the cursor's track, the pattern, every pattern or the
// block. One undo step; the notes keep their place, pitch and properties.
void remapInstrument(App& a, TransposeScope scope, ObjectId from, ObjectId to) {
  const Song& song = a.doc.song();
  if (song.channelIndex(from) < 0 || song.channelIndex(to) < 0) {
    say(i18n::tr("Pick the old and the new instrument first"));
    return;
  }
  if (from == to) {
    say(i18n::tr("The old and the new instrument are the same"));
    return;
  }
  std::vector<ObjectId> pats;
  int lo = 0, hi = 1 << 20;  // every row (and notes past the end)
  if (scope == TransposeScope::Song) {
    for (const Pattern& p : song.patterns) pats.push_back(p.id);
  } else {
    View v;
    if (!menuView(a, &v)) return;
    const State& s = st();
    if (scope == TransposeScope::Track && v.tracks[static_cast<size_t>(s.cur.track)].channel != from) {
      say(i18n::tr("The cursor's track is not the old instrument's: Pattern remaps wherever it is"));
      return;
    }
    if (scope == TransposeScope::Block) {
      int t0, t1;
      if (!s.sel.on) {
        say(i18n::tr("No block marked"));
        return;
      }
      blockRange(&t0, &t1, &lo, &hi);
      bool inside = false;
      for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t) inside = inside || v.tracks[static_cast<size_t>(t)].channel == from;
      if (!inside) {
        say(i18n::tr("The block does not hold the old instrument's track"));
        return;
      }
    }
    pats.push_back(v.p->id);
  }
  int moved = 0;
  a.doc.beginGroup("remap instrument");
  for (ObjectId pid : pats) {
    const Pattern* p = a.doc.song().pattern(pid);
    if (!p) continue;
    const trk::Grid g = trk::gridOf(a.doc.song(), *p);
    Rows src = rowsOf(*p, from), dst = rowsOf(*p, to);
    const Rows srcBefore = src, dstBefore = dst;
    std::vector<Note> take = trk::copyRows(src.notes, src.steps, src.props, g, lo, hi);
    if (take.empty()) continue;
    trk::cutRows(src.notes, src.steps, g, lo, hi);
    for (Note& n : take) {
      n.start += g.tickOf(lo);
      dst.notes.push_back(n);
    }
    trk::normalize(dst.notes);
    moved += static_cast<int>(take.size());
    commit(a, pid, from, src, srcBefore, "remap instrument");
    commit(a, pid, to, dst, dstBefore, "remap instrument");
  }
  a.doc.endGroup(false);
  const Channel* nc = a.doc.song().channel(to);
  say(moved ? std::to_string(moved) + " " + i18n::tr(moved == 1 ? "note moved to" : "notes moved to") + " " + (nc ? nc->name : std::string())
            : std::string(i18n::tr("The old instrument has no notes there")));
}

trk::Masks& masks() { return gMasks; }
bool masking() { return gMasking; }
void setMasking(bool on) { gMasking = on; }

void scaleBlock(App& a, bool expand) {
  View v = build(a);
  if (!v.p || v.tracks.empty()) return;
  clampCursor(v);
  const char* label = expand ? "expand rows" : "shrink rows";
  // N-03: with a block, the block (refused when a note would be pushed past the block's end); with
  // none, the whole pattern as the classic Expand and Shrink: Expand doubles the pattern's length
  // first, so nothing falls off; Shrink halves it after (an odd number of bars keeps its length).
  if (st().sel.on) {
    int t0, t1, r0, r1;
    blockRange(&t0, &t1, &r0, &r1);
    if (expand) {
      // R3-04: the notes stay inside the block (below it they would land on the notes there).
      const Tick a0 = v.g.tickOf(r0), b0 = v.g.tickOf(r1 + 1);
      for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
        for (const Note& n : notesRef(*v.src[static_cast<size_t>(t)], v.tracks[static_cast<size_t>(t)].channel))
          if (n.start >= a0 && n.start < b0 && a0 + (n.start - a0) * 2 >= b0) {
            say(i18n::tr("Expand would push notes past the block's end: mark a block twice as long (its second half empty), or expand without a block"));
            return;
          }
    }
    a.doc.beginGroup(label);
    for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
      applyEdit(a, v, t, label, [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::scaleRows(x.notes, g, r0, r1, expand); });
    a.doc.endGroup(false);
    return;
  }
  const ObjectId pid = v.p->id;
  const int bars = v.p->bars, spb = v.p->stepsPerBar, rows = v.g.rows;
  // R3-05: at most 256 rows (as the classic Expand), and never past the longest pattern.
  if (expand && (bars * 2 > kMaxPatternBars || rows * 2 > 256)) {
    say(i18n::tr("A pattern is at most 256 rows long: Expand would double it past that"));
    return;
  }
  a.doc.beginGroup(label);
  if (expand) {
    trk::setPatternLengthInSong(a.doc, pid, bars * 2, spb);  // R6-02: the song's later entries move along
    v = build(a);
  }
  if (v.p && v.p->id == pid)
    for (int t = 0; t < static_cast<int>(v.tracks.size()); ++t)
      applyEdit(a, v, t, label, [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::scaleRows(x.notes, g, 0, rows - 1, expand); });
  if (!expand && bars % 2 == 0) trk::setPatternLengthInSong(a.doc, pid, bars / 2, spb);
  a.doc.endGroup(false);
  say(i18n::tr(expand ? "The pattern is twice as long, its notes twice as far apart" : "The pattern's notes twice as close"));
}


void blockOp(App& a, BlockOp op) {
  View v;
  if (!menuView(a, &v)) return;
  State& s = st();
  switch (op) {
    case BlockOp::Cut: copyBlock(v, true, a); break;
    case BlockOp::Copy: copyBlock(v, false, a); break;
    case BlockOp::Paste: pasteBlock(v, a); break;
    case BlockOp::Clear:
      if (s.sel.on) clearBlock(v, a);
      else
        applyEdit(a, v, s.cur.track, "clear", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
          return trk::clearField(x.notes, x.steps, x.props, tr, g, s.cur.row, s.cur.column, s.cur.field);
        });
      break;
    case BlockOp::SelectAll: s.sel = {true, 0, 0, static_cast<int>(v.tracks.size()) - 1, std::max(0, v.g.rows - 1)}; break;
    case BlockOp::Deselect: s.sel.on = false; break;
  }
}
bool hasBlock() { return st().sel.on; }
bool hasClip() { return !st().clip.empty(); }

void insertRow(App& a, bool everyTrack) {
  View v;
  if (menuView(a, &v)) rowEdit(a, v, true, everyTrack);
}
void deleteRow(App& a, bool everyTrack) {
  View v;
  if (menuView(a, &v)) rowEdit(a, v, false, everyTrack);
}

void interpolate(App& a) {
  View v;
  if (!menuView(a, &v)) return;
  const State& s = st();
  int t0, t1, r0, r1;
  if (s.sel.on) blockRange(&t0, &t1, &r0, &r1);
  else t0 = t1 = s.cur.track, r0 = s.cur.row, r1 = v.g.rows - 1;
  const Field f = s.cur.field == Field::Pan || s.cur.field == Field::Effect ? s.cur.field : Field::Volume;
  std::string refused;
  bool changed = false;
  a.doc.beginGroup("interpolate");
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t) {
    const trk::Result r = applyEdit(a, v, t, "interpolate", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::interpolate(x.notes, g, r0, r1, f); });
    changed |= r.changed;
    if (!r.refused.empty()) refused = r.refused;
  }
  a.doc.endGroup(false);
  if (!changed && !refused.empty()) say(i18n::tr(refused.c_str()));
  else if (changed) say(i18n::tr(f == Field::Pan ? "Pan interpolated" : f == Field::Effect ? "Effect interpolated" : "Volume interpolated"));
}

// R3-06: an instrument picked in the header's list: the cursor goes to its track (the notes typed
// next are its), and the pattern keeps the keys.
void cursorToChannel(App& a, ObjectId channel) {
  View v;
  if (menuView(a, &v))
    for (int t = 0; t < static_cast<int>(v.tracks.size()); ++t)
      if (v.tracks[static_cast<size_t>(t)].channel == channel && st().cur.track != t) {
        st().cur.track = t, st().cur.column = 0, st().cur.field = Field::Note, st().cur.digit = 0;
        break;
      }
  keysBackToPattern();
}

void addColumn(App& a, int delta) {
  View v;
  if (!menuView(a, &v)) return;
  const trk::Track& t = v.tracks[static_cast<size_t>(st().cur.track)];
  int& extra = st().extraCols[t.channel];
  const int base = std::max(1, t.columns - extra);  // (the song's voices for the channel: songVoices)
  extra = std::clamp(extra + delta, 0, trk::kMaxColumns - base);
  if (delta < 0) st().cur.column = std::min(st().cur.column, base + extra - 1);
}
bool follow() { return st().follow; }
void setFollow(bool on) { st().follow = on; }
int cursorField() { return static_cast<int>(st().cur.field); }

bool sdlKey(App& a, int scancode, int keycode, int mods, bool down, bool repeat) {
  State& s = st();
  // cp18: the lower band's Sample editor or Channel frame with the keys takes them first.
  if (trackereditors::sdlKey(a, scancode, keycode, mods, down, repeat)) return true;
  // Caps Lock comes as a key down when it locks and a key up when it unlocks (macOS): each is a press.
#ifdef __APPLE__
  // (N-02: only macOS; elsewhere every press sends a down and an up, and the up is not a press.)
  if (scancode == SDL_SCANCODE_CAPSLOCK && !down && trackerKeySet() && hasKeyboard()) down = true, repeat = false;
#endif
  // Right Shift alone (pressed and let go with no other key) records the pattern, as the classic
  // Right Shift; held with another key it is an ordinary Shift.
  static bool rshift = false, rshiftUsed = false;
  if (scancode == SDL_SCANCODE_RSHIFT) {
    if (down && !repeat) rshift = true, rshiftUsed = false;
    if (!down && rshift && !rshiftUsed && trackerKeySet() && hasKeyboard()) s.keys.push_back({SDL_SCANCODE_RSHIFT, 0, 0, false});
    if (!down) rshift = false;
    return false;
  }
  if (down && rshift) rshiftUsed = true;
  // Files shown in the band: Backspace is its parent folder (the classic Disk op.), not a row edit.
  if (down && scancode == SDL_SCANCODE_BACKSPACE && (mods & (KMOD_CTRL | KMOD_GUI | KMOD_ALT | KMOD_SHIFT)) == 0 && hasKeyboard() && diskParent(a)) return true;
  if (down && hasKeyboard() && trackerTakes(scancode, mods)) {
    s.keys.push_back({scancode, keycode, mods, repeat});
    return true;
  }
  if (!down) {
    if (const auto it = s.held.find(scancode); it != s.held.end()) {
      a.engine.noteOff(it->second.channel, it->second.pitch);
      // Record key-off notes (Settings): a key let go while recording ends its note on the row playing.
      if (gRecOff && it->second.track >= 0 && a.recordArmed && a.engine.playing() && s.playRow >= 0)
        s.keys.push_back({-1 - it->second.track, it->second.column, 0, false});
      s.held.erase(it);
    }
    return false;
  }
  if (!hasKeyboard()) return false;
  // The chords keys::editorTakes leaves to their commands are not taken here either.
  const bool ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (mods & KMOD_ALT) != 0;
  if (scancode == SDL_SCANCODE_SPACE || (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12 && !alt)) return false;
  if (scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI) return false;  // the modifiers themselves
  if (ctrl && alt) return false;
  if (ctrl) {
    const bool mine = keycode == 'c' || keycode == 'x' || keycode == 'v' || scancode == SDL_SCANCODE_UP || scancode == SDL_SCANCODE_DOWN ||
                      scancode == SDL_SCANCODE_MINUS || scancode == SDL_SCANCODE_EQUALS || (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0);
    if (!mine) return false;
  }
  if (alt && !ctrl && !(scancode == SDL_SCANCODE_F3 || scancode == SDL_SCANCODE_F4 || scancode == SDL_SCANCODE_F5)) return false;
  s.keys.push_back({scancode, keycode, mods, repeat});
  return true;
}

void showPage(App& a, const char* window) {
  // Another page asked for (a key, a menu): the band comes back with its page strip.
  if (gExtended && std::strcmp(window, "Pattern editor") != 0) setExtended(a, false);
  if (std::strcmp(window, "Instrument") == 0) {  // the selected channel's instrument (hosted in the layout's chain)
    if (a.selChannel != kNoId) trackereditors::open(a, trackereditors::Editor::Instrument, a.selChannel);
    return;
  }
  if (std::strcmp(window, "Loupe") == 0) {  // T-11: the pinned panel docks with the centre's pages: it comes to the front
    a.settings.set("ui.loupePinned", true);
    // The instrument editor gives the lower band to the audio editor (the classic sample editor
    // replaces the instrument editor; both edit the current instrument).
    a.settingsChannel = kNoId;
    if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
    // The selected channel's sample, as the classic sample editor edits the current instrument's.
    if (const Channel* ch = a.doc.song().channel(a.selChannel); ch && ch->plugin.type == "gl.sampler" && !ch->plugin.data.empty())
      packs::openAudioEditor(a, ch->plugin.data, ch->id);
    bringPage(window);
    return;
  }
  bool* want = layoutui::windowOpenFlag(a, window);
  if (!want) return;
  // The centre's pages are tabs without a tab bar: the page opens (if closed) and comes to the front;
  // the others stay open behind it.
  *want = true;
  bringPage(window);
  if (std::strcmp(window, "Pattern editor") == 0) st().wantFocus = true;
}

namespace {
std::string gStartPage;
int gCloseAsked = -10;  // the frame of the main window's close request a page took (closePageOnCloseRequest)

// The focused window when it is a page docked in the centre other than the pattern (else null).
ImGuiWindow* frontPage() {
  if (!GImGui) return nullptr;
  ImGuiWindow* nav = GImGui->NavWindow;
  ImGuiWindow* w = nav ? nav->RootWindow : nullptr;
  const ImGuiWindow* pe = ImGui::FindWindowByName("###Pattern editor");
  if (!w || !pe || w == pe || !w->DockIsActive || !w->DockNode || w->DockNode != pe->DockNode) return nullptr;
  return w;
}
}  // namespace
void setStartPage(const std::string& window) { gStartPage = window; }
std::string takeStartPage() {
  std::string w;
  w.swap(gStartPage);
  return w;
}

void stepPattern(App& a, int d) {
  const Song& song = a.doc.song();
  const int cur = song.patternIndex(a.curPattern), n = static_cast<int>(song.patterns.size());
  const int i = cur + d;
  if (i >= 0 && i < n) {
    a.selectPattern(song.patterns[static_cast<size_t>(i)].id);
    return;
  }
  // R10-03: past the last pattern comes a new, empty one, as the classic tracker's next number is
  // always there to type in. Only after a pattern that holds something: an empty last one stays the
  // last (stepping on never makes a row of empty patterns).
  if (d <= 0 || cur != n - 1 || n == 0) return;
  const Pattern& last = song.patterns.back();
  bool used = !last.automation.empty();
  for (const auto& [ch, v] : last.notes) used = used || !v.empty();
  for (const auto& [ch, v] : last.steps)
    for (uint8_t x : v) used = used || x != 0;
  if (!used) {
    say(i18n::tr("This is the last pattern, and it is empty: write in it first"));
    return;
  }
  // cp21: the classic 64 rows (before: the length of the pattern before it).
  const ObjectId id = a.doc.addPattern(ops::nextPatternName(song), kNewPatternBars, kNewPatternSteps);
  if (id) {
    a.selectPattern(id);
    say(i18n::tr("A new, empty pattern (Ins puts it in the song)"));
  }
}

void stepInstrument(App& a, int d) {
  const View v = build(a);
  if (!v.p || v.tracks.empty()) return;
  State& s = st();
  s.sel.on = false;
  s.cur.track = std::clamp(s.cur.track + d, 0, static_cast<int>(v.tracks.size()) - 1);
  s.cur.column = 0, s.cur.field = Field::Note, s.cur.digit = 0;
  a.selChannel = s.cursorChannel = v.tracks[static_cast<size_t>(s.cur.track)].channel;
  s.wantFocus = true;
}

bool closePageOnCloseRequest(App& a) {
  if (trackereditors::closeRequest(a)) return true;  // cp18: Cmd+W with the lower band's screen: it closes
  if (!(layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker)) || !frontPage()) return false;
  gCloseAsked = ImGui::GetFrameCount();
  return true;
}

bool closeFrontPageKey(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  if (trackereditors::hasKeys()) return false;  // cp18: Esc and Cmd+W close the lower band's screen (its own keys)
  const bool asked = ImGui::GetFrameCount() - gCloseAsked <= 2;  // (asked between frames: taken on the next)
  gCloseAsked = -10;
  if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
  const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !keys::escapeBusy();
  const bool cmdW = asked || (ImGui::IsKeyPressed(ImGuiKey_W, false) && (io.KeySuper || io.KeyCtrl) && !io.KeyAlt && !io.KeyShift);
  if (!esc && !cmdW) return false;
  ImGuiWindow* w = frontPage();
  if (!w) return false;
  const char* id = ui::stableWindowName(w->Name);
  static const char* const kStrip[] = {"Playlist", "Piano roll", "Channel rack", "Mixer", "Live pads", "Loupe"};
  bool strip = false;
  for (const char* n : kStrip) strip = strip || std::strcmp(id, n) == 0;
  if (!strip)
    if (bool* f = layoutui::windowOpenFlag(a, id)) *f = false;  // closed, not left behind as a hidden tab
  if (esc) keys::claimEscape();
  showPage(a, "Pattern editor");
  return true;
}

void bringPage(const char* window, bool takeKeys) {
  st().pendingPage = window;
  st().pendingQuiet = !takeKeys;
  st().pendingFrames = st().pendingWait = st().pendingHold = 0;
}

void pumpPage(App& a) {
  State& s = st();
  // Once a frame, from whichever Tracker part is drawn (T-05: with the header band hidden the
  // order list that used to pump it is not drawn, so the pages and the editor pump it too).
  static int pumped = -1;
  if (pumped == ImGui::GetFrameCount()) return;
  pumped = ImGui::GetFrameCount();
  if (s.chordGroup && s.held.empty()) {  // R5-08: the chord's keys are all up: its notes are one step
    a.doc.endGroup(false);
    s.chordGroup = false;
  }
  if (s.pendingPage.empty()) return;
  if (s.pendingHold > 0) {  // the layout docks its pages again first
    --s.pendingHold;
    return;
  }
  ImGuiWindow* w = ImGui::FindWindowByName(("###" + s.pendingPage).c_str());  // windows are "Label###English"
  const bool* open = layoutui::windowOpenFlag(a, s.pendingPage);
  // Done once it shows in its dock node (or gives up: closed again, or not in front after a second
  // of being docked). While the layout is still docking its windows the request waits: a window
  // drawn on its own for a frame before it docks is not yet the page in front.
  // DockTabIsVisible says nothing while the tab bar is hidden: the node's visible window does.
  const bool docked = w && w->DockIsActive && w->DockNode;
  const bool front = docked && w->DockNode->VisibleWindow == w;
  // A page the person floated out never docks: after three seconds it is simply focused.
  s.pendingFrames += docked ? 1 : 0;
  s.pendingWait += 1;
  if (front || (open && !*open) || s.pendingFrames > 60 || s.pendingWait > 180) {
    s.pendingWait = 0;
    if (w && s.pendingFrames <= 60 && !s.pendingQuiet) ui::focusWindowKeepPopups(w);  // (a prompt stays open)
    s.pendingPage.clear();
    return;
  }
  if (!w || !w->DockNode) return;
  ImGuiDockNode* n = w->DockNode;
  if (n->IsNoTabBar() || n->IsHiddenTabBar()) {
    // A node without a tab bar shows its first window: that page goes first.
    if (ImGuiWindow** it = n->Windows.find(w); it != n->Windows.end() && it != n->Windows.begin()) {
      n->Windows.erase(it);
      n->Windows.push_front(w);
    }
  } else if (n->TabBar) {
    n->TabBar->NextSelectedTabId = w->TabId;
  }
}

void patternEditor(App& a) {
  const PerfScope perfScope(PerfPattern);
  State& s = st();
  // R11-10: an undo or redo (or any edit) after a message ends it, as an edit made here does.
  if (!s.status.empty()) {
    const uint64_t now = a.doc.history().stateSerial();
    if (s.statusSerial == 0) s.statusSerial = now;
    else if (s.statusSerial != now) s.status.clear();
  }
  const auto& P = pal();
  ImGuiIO& io = ImGui::GetIO();
  const bool hadKb = s.hasKb;
  s.drawnFrame = ImGui::GetFrameCount();
  pumpPage(a);
  loadColumns(a);
  s.hasKb = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
  if (hadKb && !s.hasKb) releaseAll(a);  // the keyboard went elsewhere: no note keeps sounding
  if (s.wantFocus) {
    // The part window ("Label###English"; focusing the child would pick no dock tab). R7-01, K-297:
    // never over a prompt, dialog or menu a header button just opened (App::confirm's Clean up,
    // Replace; Files > Save as): a plain focus closed every popup above the pattern.
    ui::focusWindowKeepPopups(ImGui::FindWindowByName("###Pattern editor"));
    s.wantFocus = false;
  }
  View v = build(a);
  if (s.pattern != a.curPattern) s.pattern = a.curPattern, s.sel.on = false;
  if (!v.p || v.tracks.empty()) {  // no pattern, or no channel to give the pattern a track
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    // A song of audio clips only has no pattern: the Song page shows its clips; a pattern made here
    // starts the rows (one undo step, as Pattern > New pattern).
    const bool audioOnly = !v.p && std::any_of(a.doc.song().playlist.begin(), a.doc.song().playlist.end(), [](const Clip& c) { return c.kind != ClipKind::Pattern; });
    // cp21 (R21-09): the classic centred message box (a raised bevel frame, its title in the bold
    // face, the line under it, New pattern inside the box) in place of the shared empty-state plate
    // with its large mark, the one modern panel left on the Pattern page.
    const auto& EP = pal();
    ImDrawList* edl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float bw = std::round(fs * 9), bh = std::round(fs + 10);
    const char* title = i18n::tr(!v.p ? "No pattern" : "No channels");
    const char* line = i18n::tr(!v.p ? (audioOnly ? "Audio and automation clips only (the Song page shows them). A new pattern starts rows of notes"
                                                  : "Make a pattern to write notes in rows")
                                     : "Add a track (the header's Add track) to write notes in rows");
    const float boxW = std::round(std::min(std::max(120.0f, av.x - 16), fs * 34));
    const float wrapW = boxW - 24;
    const float lineH = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, wrapW, line).y;
    const float boxH = std::round(10 + fs + 8 + lineH + (!v.p ? 10 + bh : 0) + 12);
    const ImVec2 q0(std::round(p0.x + (av.x - boxW) * 0.5f), std::round(p0.y + std::max(8.0f, (std::max(av.y, 90.0f) - boxH) * 0.45f)));
    const ImVec2 q1(q0.x + boxW, q0.y + boxH);
    bevelBox(edl, q0, q1, false);
    edl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(q0.x + 12, q0.y + 10), EP.text, title);
    edl->AddText(a.fontRegular, fs, ImVec2(q0.x + 12, q0.y + 10 + fs + 8), EP.textDim, line, nullptr, wrapW);
    if (!v.p) {
      const ImVec2 b0(std::round(q0.x + (boxW - bw) * 0.5f), std::round(q1.y - 12 - bh));
      if (trackerview::bevelButton(a, "##newpat", "New pattern", b0, ImVec2(b0.x + bw, b0.y + bh), false, "A new, empty pattern of 64 rows to write in")) {
        // The new pattern goes in the song too when it has no pattern clips (entry 00).
        const bool first = !trk::hasPatternEntries(a.doc.song());
        a.doc.beginGroup("new pattern");
        const ObjectId id = a.doc.addPattern(ops::nextPatternName(a.doc.song()), kNewPatternBars, kNewPatternSteps);  // cp21: 64 rows
        if (id && first) a.doc.addClip(id, 0, 0);
        a.doc.endGroup(false);
        if (id) a.selectPattern(id);
      }
    }
    s.keys.clear();
    return;
  }
  clampCursor(v);
  // Keys first, in the order they came; the view is built again after an edit.
  if (!s.keys.empty()) {
    const std::vector<KeyEv> keys = std::move(s.keys);
    s.keys.clear();
    for (const KeyEv& k : keys) {
      if (handleKey(a, v, k)) v = build(a);
      if (!v.p || v.tracks.empty()) return;
      clampCursor(v);
    }
  }
  // ---- playback row ----
  const Song& song = a.doc.song();
  s.playRow = -1;
  s.stacked = 0;
  if (a.engine.playing()) {
    Tick tick = -1;
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == a.curPattern) {
      tick = pos;
    } else if (a.engine.mode() == PlayMode::Song) {
      // Follow on: the editor shows the pattern of the order entry under the position (its top
      // clip's pattern), one pattern at a time as the classic editor does; the patterns stacked
      // with it are counted in the pattern bar. Follow off: the pattern picked stays.
      if (s.follow) {
        const ObjectId under = trk::patternAt(song, pos);
        if (under != kNoId && under != a.curPattern) {
          a.curPattern = under;  // the shown pattern follows the song (the engine plays the arrangement)
          v = build(a);
          if (!v.p || v.tracks.empty()) return;
          clampCursor(v);
        }
      }
      tick = trk::patternTickAt(song, a.curPattern, pos);
    }
    if (tick >= 0 && v.g.rowTicks > 0) {
      const Tick inPat = tick % std::max<Tick>(1, v.g.length());
      s.playRow = static_cast<int>(inPat / v.g.rowTicks);
      s.playFrac = static_cast<float>(inPat % v.g.rowTicks) / static_cast<float>(v.g.rowTicks);
    }
    if (s.follow && s.playRow >= 0) s.cur.row = std::clamp(s.playRow, 0, v.g.rows - 1);
    if (a.engine.mode() == PlayMode::Song) s.stacked = stackedAt(song, a.curPattern, static_cast<Tick>(a.engine.position()));
  }
  // The cursor's track is the selected channel (the piano roll, the rack and the typing keyboard
  // follow it), only when the cursor moves to another track, so a rack click still selects.
  if (s.cursorChannel == kNoId)  // first frame: the cursor starts on the selected channel
    for (size_t ti = 0; ti < v.tracks.size(); ++ti)
      if (v.tracks[ti].channel == a.selChannel) s.cur.track = static_cast<int>(ti), s.cursorChannel = a.selChannel;
  if (s.cur.track >= 0 && s.cur.track < static_cast<int>(v.tracks.size())) {
    const ObjectId ch = v.tracks[static_cast<size_t>(s.cur.track)].channel;
    if (ch != s.cursorChannel) {
      s.cursorChannel = ch;
      a.selChannel = ch;
    }
  }

  // ---- geometry ----
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dpp = pixelgrid::frameScale();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = a.fontMono ? a.fontMono : ImGui::GetFont();
  const float cwH = mono->CalcTextSizeA(fs, FLT_MAX, 0, "0").x;  // the track headers' size
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const float barH = std::round(fs + 10);
  const float headH = std::round(fs + 10);
  const ImVec2 a0 = o, a1(o.x + av.x, o.y + std::max(av.y - barH, headH + fs * 6));
  s.area0 = a0, s.area1 = a1;
  // The pattern's glyph size grows with the room: the largest size (whole pixels, up to a bit over
  // twice the interface's) at which the tracks share the width and the view still holds a good run
  // of rows. Tracks that do not fit at the interface's size scroll sideways.
  struct Metrics {
    float pf, cw, colW, pad, numW, rowH;
  };
  auto metricsAt = [&](float pf) {
    Metrics m{};
    const float k = pf / fs;
    m.pf = pf;
    m.cw = mono->CalcTextSizeA(pf, FLT_MAX, 0, "0").x;
    m.colW = std::round(m.cw * static_cast<float>(noteChars()) + 6 * k);
    m.pad = std::round(4 * k);
    m.numW = std::round(m.cw * 3 + 10 * k);
    m.rowH = std::round(pf * (gTall ? 1.5f : 1.12f) + 2);
    return m;
  };
  int handW = gTrackWidth;  // (cp17: a zoomed-out view drops the hand widening)
  auto tracksW = [&](const Metrics& m, size_t n) {
    float w = 0;
    for (size_t i = 0; i < n && i < v.tracks.size(); ++i)
      w += m.pad * 2 + m.colW * static_cast<float>(v.tracks[i].columns) + m.cw * static_cast<float>(handW) + 1;
    return w;
  };
  const float areaW = a1.x - a0.x;
  // cp17: Fit: the widest zoom step at which every track is in view (the narrowest when none is).
  if (gZoomFit) {
    int z = 0;
    for (; z < kZoomMax; ++z) {
      zoomColumns(z);
      handW = z == 0 ? gTrackWidth : 0;
      const Metrics m = metricsAt(zoomGlyph(z, fs));
      if (tracksW(m, v.tracks.size()) + m.numW * 2 <= areaW) break;
    }
    gZoomEff = z;
    zoomColumns(z);
  }
  handW = gZoomEff == 0 ? gTrackWidth : 0;
  // A sub-column the zoom hides takes the cursor to the note (its data stays; zoom in to edit it).
  if (!gShow[static_cast<int>(s.cur.field)]) s.cur.field = Field::Note, s.cur.digit = 0;
  const float fz = zoomGlyph(gZoomEff, fs);  // the zoom step's glyph size
  size_t shown = v.tracks.size();
  {
    const Metrics m0 = metricsAt(fz);
    while (shown > 1 && tracksW(m0, shown) + m0.numW * 2 > areaW) --shown;
  }
  // Settings' "Visible tracks": at most that many share the width (wider tracks, the rest scroll).
  if (gVisible > 0 && gZoomEff == 0) shown = std::min(shown, static_cast<size_t>(gVisible));
  // T-20: the classic view's 27 rows (13 each side of the edit row) before the glyphs grow; R3-09:
  // extended, about 40 (the room goes to rows, not to bigger glyphs).
  const float minRows = gExtended ? 41 : 27;
  const float pfMax = std::max(fs, std::min(std::floor(fs * 2.2f), std::floor(((a1.y - a0.y - headH) / minRows - 2) / 1.12f)));
  float pf = fz;
  for (float t = pfMax; t > fs && handW == 0 && gZoomEff == 0; t -= 1) {  // (widened by hand or zoomed: the glyphs stay, so the edge follows the pointer)
    const Metrics m = metricsAt(t);
    if (tracksW(m, shown) + m.numW * 2 <= areaW) {
      pf = t;
      break;
    }
  }
  const Metrics M = metricsAt(pf);
  const float cw = M.cw, colW = M.colW, numW = M.numW, rowH = M.rowH;
  s.rowH = rowH;
  // What width is left goes to the shown tracks evenly (their cells centred), so the channels fill
  // the view edge to edge.
  const float viewW = std::max(40.0f, areaW - numW * 2);
  const float extra = handW > 0 ? 0.0f : std::max(0.0f, std::floor((viewW - tracksW(M, shown)) / static_cast<float>(std::max<size_t>(1, shown))));
  // (cp21: kept between frames, so drawing allocates nothing once they have grown to the tracks)
  static std::vector<float> trackX, trackW, inset;  // each track's left edge (from the area's left, before scroll), width, cell inset
  trackX.clear(), trackW.clear(), inset.clear();
  float x = 0;
  for (const trk::Track& t : v.tracks) {
    trackX.push_back(x);
    // (cp16: widened by hand, gTrackWidth, every track by as much)
    const float wide = extra + cw * static_cast<float>(handW);
    const float w = M.pad * 2 + colW * static_cast<float>(t.columns) + wide;
    trackW.push_back(w);
    inset.push_back(std::floor(M.pad + wide * 0.5f));
    x += w + 1;
  }
  const float contentW = x;
  // Horizontal scroll, in whole tracks (T-19: the view always starts on a track's left edge, so no
  // sliver of the previous one stays at the left): it brings the cursor's track into view when the
  // cursor goes to another track; Shift+wheel and the scrollbar move it a track at a time between.
  const int nTracks = static_cast<int>(v.tracks.size());
  int lastFirst = std::max(0, nTracks - 1);  // the furthest first track: from there the rest fits
  while (lastFirst > 0 && contentW - trackX[static_cast<size_t>(lastFirst - 1)] <= viewW + 1) --lastFirst;
  auto firstAt = [&](float sx) {  // the track the view starts with at that scroll (the one under its left edge)
    int f = 0;
    while (f + 1 < nTracks && trackX[static_cast<size_t>(f + 1)] <= sx + 0.5f) ++f;
    return std::clamp(f, 0, lastFirst);
  };
  float& scrollX = s.scrollX;
  int first = firstAt(scrollX);
  // (Again when the tracks' sizes change: another pattern, the glyphs grown or shrunk, a column
  // more; T-19, the cursor's track went off screen when the song moved to a pattern of another size.)
  const long long revealKey = (static_cast<long long>(s.cur.track) << 32) ^ static_cast<long long>(std::lround(contentW)) ^
                              (static_cast<long long>(std::lround(viewW)) << 16);
  if (revealKey != s.scrolledFor && nTracks > 0) {
    const int ct = std::clamp(s.cur.track, 0, nTracks - 1);
    if (ct < first) first = ct;
    while (first < ct && trackX[static_cast<size_t>(ct)] + trackW[static_cast<size_t>(ct)] - trackX[static_cast<size_t>(first)] > viewW + 1) ++first;
    s.scrolledFor = revealKey;
  }
  auto scrollTo = [&](int f) {
    first = std::clamp(f, 0, lastFirst);
    scrollX = nTracks > 0 ? trackX[static_cast<size_t>(first)] : 0.0f;
  };
  scrollTo(first);
  const float gx0 = a0.x + numW, gx1 = a1.x - numW;  // the tracks' area
  // The whole tracks in view (the cursor's is kept one of them).
  int lastShown = std::clamp(first, 0, std::max(0, nTracks - 1));
  while (lastShown + 1 < nTracks && trackX[static_cast<size_t>(lastShown + 1)] + trackW[static_cast<size_t>(lastShown + 1)] - scrollX <= viewW + 1) ++lastShown;
  // cp16 (user): the next track shows in part at the right edge, cut by it, as the Playlist's do
  // (no empty gap); the view still starts on a whole track, and the cursor's track is always whole.
  const float gxEnd = gx1;
  // T-19: a scrollbar under the tracks when they do not all fit.
  const bool hbar = lastFirst > 0;
  const float sbH = hbar ? std::max(12.0f, std::round(fs * 0.95f)) : 0.0f;  // (cp16: a bar the size of the Playlist's, easy to grab)
  const float body0 = a0.y + headH, body1 = a1.y - sbH;
  const int visRows = std::max(1, static_cast<int>((body1 - body0) / rowH));
  // The edit row stays fixed in the middle and the rows scroll past it; above the first row and
  // below the last the view is shaded.
  const int centre = visRows / 2;
  const float centreY = body0 + static_cast<float>(centre) * rowH;
  const int firstRow = s.cur.row - centre;

  // ---- mouse: click a cell to put the cursor there, drag to select, wheel to move ----
  // (The rows only: the track headers' mute lamps and column buttons take their own clicks.)
  ImGui::SetCursorScreenPos(ImVec2(a0.x, body0));
  ImGui::InvisibleButton("##trackergrid", ImVec2(std::max(1.0f, a1.x - a0.x), std::max(1.0f, body1 - body0)));
  const bool hov = ImGui::IsItemHovered();
  auto hit = [&](ImVec2 m, int* track, int* column, Field* field, int* digit, int* row) {
    *row = firstRow + static_cast<int>(std::floor((m.y - body0) / rowH));
    const float lx = m.x - gx0 + scrollX;
    *track = -1;
    if (m.x >= gxEnd) return;  // the empty gap after the last whole track
    for (size_t i = 0; i < v.tracks.size(); ++i) {
      const float l = trackX[i] + inset[i];
      if (lx >= trackX[i] && lx < trackX[i] + trackW[i] + 1) {
        *track = static_cast<int>(i);
        *column = std::clamp(static_cast<int>((lx - l) / colW), 0, v.tracks[i].columns - 1);
        const int ch = std::max(0, static_cast<int>((lx - l - static_cast<float>(*column) * colW) / cw));
        *field = Field::Note;
        for (int f = 0; f < static_cast<int>(Field::Count); ++f)
          if (gShow[f] && ch >= fieldOffset(static_cast<Field>(f))) *field = static_cast<Field>(f);
        *digit = std::clamp(ch - fieldOffset(*field), 0, trk::fieldDigits(*field) - 1);
        return;
      }
    }
  };
  // A click moves the cursor to the track and field under the pointer and keeps its row (the rows
  // stay where they are under the mouse, as the classic tracker); a drag from there marks a block
  // from the cell pressed to the cell under the pointer. Shift+click extends the block.
  static int pressTrack = -1, pressRow = -1;
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.y >= body0) {
    int t, col, d, row;
    Field f;
    hit(io.MousePos, &t, &col, &f, &d, &row);
    pressTrack = -1;
    if (t >= 0 && row >= 0 && row < v.g.rows) {
      if (io.KeyShift) {
        if (!s.sel.on) s.sel = {true, s.cur.track, s.cur.row, t, row};
        s.sel.t1 = t, s.sel.r1 = row;
      } else {
        s.sel.on = false;
        pressTrack = t, pressRow = row;
      }
      s.cur.track = t, s.cur.column = col, s.cur.field = f, s.cur.digit = d;
      a.selectChannel(v.tracks[static_cast<size_t>(t)].channel);
    }
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4) && io.MousePos.y >= body0 && pressTrack >= 0) {
    int t, col, d, row;
    Field f;
    hit(io.MousePos, &t, &col, &f, &d, &row);
    if (t >= 0) {
      s.sel = {true, pressTrack, pressRow, t, std::clamp(row, 0, v.g.rows - 1)};
      s.cur.track = t;
    }
  }
  const bool wheelHov = hov || (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MousePos.y >= a0.y && io.MousePos.y < a1.y);
  // cp17: Ctrl+wheel (Cmd+wheel on macOS) zooms the tracks: up wider, down narrower (more in view).
  const bool zoomWheel = io.KeyCtrl || io.KeySuper;
  if (wheelHov && zoomWheel && io.MouseWheel != 0) {
    s.zoomWheel += io.MouseWheel;
    const int n = static_cast<int>(s.zoomWheel);
    if (n != 0) s.zoomWheel -= static_cast<float>(n), zoomPattern(a, n > 0 ? 1 : -1);
  }
  const float wheelX = zoomWheel ? 0.0f : io.MouseWheelH != 0 ? io.MouseWheelH : io.KeyShift ? io.MouseWheel : 0.0f;  // (> 0: to the left)
  if (wheelHov && wheelX != 0) {  // a track a notch (a trackpad's small steps add up)
    s.wheelX -= wheelX;
    const int n = static_cast<int>(s.wheelX);
    if (n != 0) s.wheelX -= static_cast<float>(n), scrollTo(first + n);
  }
  // The scrollbar: the thumb shows the tracks in view; dragging it scrolls track by track, a click
  // beside it pages by the tracks in view.
  if (hbar) {
    const float tx0 = gx0, tx1 = gx1, span = std::max(1.0f, tx1 - tx0);
    const float th0 = tx0 + span * scrollX / contentW, th1 = std::min(tx1, th0 + std::max(span * viewW / contentW, sbH * 2));
    ImGui::SetCursorScreenPos(ImVec2(tx0, body1));
    ImGui::InvisibleButton("##trackscroll", ImVec2(span, sbH));
    static float grab = -1;
    if (ImGui::IsItemActivated()) {
      const float mx = io.MousePos.x;
      if (mx >= th0 && mx < th1) {
        grab = mx - th0;
      } else {
        grab = -1;
        int shownNow = 1;
        while (first + shownNow < nTracks &&
               trackX[static_cast<size_t>(first + shownNow)] + trackW[static_cast<size_t>(first + shownNow)] - scrollX <= viewW + 1)
          ++shownNow;
        scrollTo(mx < th0 ? first - shownNow : first + shownNow);
      }
    }
    if (ImGui::IsItemActive() && grab >= 0) scrollTo(firstAt((io.MousePos.x - grab - tx0) / span * contentW + trackW[0] * 0.5f));
    if (!ImGui::IsItemActive()) grab = -1;
    s.hbar0 = ImVec2(tx0, body1), s.hbar1 = ImVec2(tx1, body1 + sbH);
    ui::Hint("The tracks: drag to scroll them (Shift+wheel too); a click beside the bar shows the next tracks");
  } else {
    s.hbar0 = s.hbar1 = ImVec2(0, 0);
  }
  if (wheelHov && io.MouseWheel != 0 && !io.KeyShift && !zoomWheel && !(s.follow && s.playRow >= 0)) {
    s.wheel -= io.MouseWheel * 4;  // four rows a notch; a trackpad's small steps add up
    const int rows = static_cast<int>(s.wheel);
    if (rows != 0) {
      s.wheel -= static_cast<float>(rows);
      s.sel.on = s.sel.on && io.KeyShift;
      moveRow(v, rows, false);
    }
  }

  // ---- background, row bands ----
  dl->PushClipRect(a0, a1, true);
  dl->AddRectFilled(a0, a1, P.well);
  auto band = [&](float y0, float y1, float l, float r, ImU32 col) {
    const pixelgrid::Span sy = pixelgrid::block(y0, y1, dpp, 0, 0);
    dl->AddRectFilled(ImVec2(pixelgrid::edge(l, dpp), sy.x0), ImVec2(pixelgrid::edge(r, dpp), sy.x1), col);
  };
  // Before the first row and after the last (shaded below; the track lines go on through them).
  const float yTop = body0 + static_cast<float>(-firstRow) * rowH, yEnd = body0 + static_cast<float>(v.g.rows - firstRow) * rowH;
  {
    const ImU32 shade = theme::mix(P.well, P.surface950, 0.75f);
    if (yTop > body0) band(body0, std::min(yTop, body1), a0.x, a1.x, shade);
    if (yEnd < body1) band(std::max(yEnd, body0), body1, a0.x, a1.x, shade);
    // R8-03: an imported pattern's rows past what the song plays (whole bars round it up): shaded
    // lighter, so the module's own length reads at a glance.
    if (const int played = trk::playedRows(a.doc.song(), *v.p, v.g); played < v.g.rows) {
      const float yPlay = body0 + static_cast<float>(played - firstRow) * rowH;
      if (yPlay < std::min(yEnd, body1)) band(std::max(yPlay, body0), std::min(yEnd, body1), a0.x, a1.x, theme::withAlpha(shade, 0.6f));
    }
  }
  for (int i = 0; i < visRows; ++i) {
    const int row = firstRow + i;
    if (row < 0 || row >= v.g.rows) continue;
    const float y = body0 + static_cast<float>(i) * rowH;
    if (!gBeats) continue;
    // cp16: beat rows a step brighter, bar rows brighter still (before they were faint), so the
    // pattern's beats read at a glance as the classic highlight rows do.
    if (row % v.g.rowsPerBar == 0) band(y, y + rowH, a0.x, a1.x, theme::withAlpha(P.gridBar, 0.85f));
    else if (row % v.g.rowsPerBeat == 0) band(y, y + rowH, a0.x, a1.x, theme::withAlpha(P.gridBeat, 0.7f));
  }
  // The cursor row (fixed in the middle) and the play row.
  band(centreY, centreY + rowH, a0.x, a1.x, theme::withAlpha(s.edit ? P.arm : P.accent, 0.22f));
  if (s.playRow >= 0 && s.playRow != s.cur.row) {
    const int i = s.playRow - firstRow;
    if (i >= 0 && i < visRows) band(body0 + static_cast<float>(i) * rowH, body0 + static_cast<float>(i + 1) * rowH, a0.x, a1.x, theme::withAlpha(P.playhead, 0.25f));
  }
  if (s.playRow >= 0 && s.playRow == s.cur.row) band(centreY, centreY + rowH, a0.x, a0.x + 3, P.playhead);

  // ---- row numbers both sides ----
  const bool hex = a.settings.getString("tracker.rowBase") != "dec";
  for (int i = 0; i < visRows; ++i) {
    const int row = firstRow + i;
    if (row < 0 || row >= v.g.rows) continue;
    const float y = body0 + static_cast<float>(i) * rowH + std::round((rowH - pf) * 0.5f);
    const std::string t = trk::rowText(row, hex);
    // cp21 (R21-06): the edit row's number in the values' ink, whatever the beat (before, an off-beat
    // edit row, the last row among them, kept the faint number and the eye lost the row).
    const ImU32 col = row == s.cur.row ? valueInk() : !gBeats || row % v.g.rowsPerBeat == 0 ? P.text : P.textFaint;
    const float tw = mono->CalcTextSizeA(pf, FLT_MAX, 0, t.c_str()).x;
    dl->AddText(mono, pf, ImVec2(std::round(a0.x + numW - 6 - tw), y), col, t.c_str());
    dl->AddText(mono, pf, ImVec2(std::round(a1.x - numW + 6), y), col, t.c_str());
  }
  {
    const pixelgrid::Span l = pixelgrid::line(gx0 - 1, dpp), r = pixelgrid::line(gx1, dpp);
    dl->AddRectFilled(ImVec2(l.x0, a0.y), ImVec2(l.x1, a1.y), P.line);
    dl->AddRectFilled(ImVec2(r.x0, a0.y), ImVec2(r.x1, a1.y), P.line);
  }

  // ---- tracks ----
  if (gxEnd < gx1 - 1) dl->AddRectFilled(ImVec2(gxEnd, a0.y), ImVec2(gx1, a1.y), theme::mix(P.well, P.surface950, 0.75f));
  dl->PushClipRect(ImVec2(gx0, a0.y), ImVec2(gxEnd, a1.y), true);
  const Song& sg = a.doc.song();
  const ops::ChannelSolo& solo = soloMemory();
  int hoverTrackHead = -1;
  for (size_t ti = 0; ti < v.tracks.size(); ++ti) {
    const trk::Track& t = v.tracks[ti];
    const Channel* ch = v.channels[ti];
    const float tx = gx0 + trackX[ti] - scrollX;
    const float tw = trackW[ti];
    if (tx >= gxEnd || tx + tw < gx0) continue;
    // Separator.
    const pixelgrid::Span sep = pixelgrid::line(tx + tw, dpp);
    dl->AddRectFilled(ImVec2(sep.x0, a0.y), ImVec2(sep.x1, gFrames ? a1.y : a0.y + headH), P.line);  // (no frames: the headers only)
    // K-106 (refs 100, 140): the track columns run on through the empty space above the first row and
    // below the last, as the classic screen's channel boxes do; on the darker shade there the line
    // is drawn lighter (the rows' own ground) so it still reads.
    if (gFrames) {
      if (yTop > body0) dl->AddRectFilled(ImVec2(sep.x0, body0), ImVec2(sep.x1, std::min(yTop, body1)), P.well);
      if (yEnd < body1) dl->AddRectFilled(ImVec2(sep.x0, std::max(yEnd, body0)), ImVec2(sep.x1, body1), P.well);
    }
    // Header: colour bar, number and name, the mute lamp, + and - columns.
    const bool muted = ch->muted, soloed = ops::channelSoloed(sg, ch->id, solo);
    const ImVec2 h0(tx, a0.y), h1(tx + tw, a0.y + headH - 1);
    trackerview::bevelBox(dl, h0, h1, false);  // a raised header, as the band above
    dl->AddRectFilled(h0, ImVec2(h1.x, h0.y + 3), theme::rgb(ch->color, muted ? 0.35f : 1.0f));
    const std::string number = std::to_string(sg.channelIndex(ch->id) + trackBase(a));
    const std::string name = gNumbers ? number + " " + ch->name : ch->name;
    float lampW = std::round(fs * 0.7f);
    // cp17: a track zoomed narrow keeps its number, name start and lamp; the column buttons go.
    const bool narrowHead = tw < cwH * 11;
    const float btnW = narrowHead ? 0.0f : std::round(cwH * 1.6f);
    // K-123 (4.5): the record dot before the column buttons: armed (red), or on the header's hover.
    const bool noteArmed = midiarm::armed(a, ch->id);
    const bool armShown = midiarm::applies(a) && tw >= cwH * 8 &&
                          (noteArmed || (ImGui::IsMouseHoveringRect(h0, h1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)));
    const float armW = armShown ? std::round(fs * 0.9f) + 4 : 0.0f;
    const ImU32 nameInk = muted ? P.textFaint : static_cast<int>(ti) == s.cur.track ? P.text : P.textDim;
    // cp21 (review R21-01): a narrow header (Fit, the far zoom steps) always shows the track's number
    // in the monospaced face, the name after it only while a few letters fit. Before, Fit left most
    // headers with the lamp alone, and nothing told the tracks apart.
    const float edge = narrowHead ? 3.0f : 6.0f;
    const float numFs = narrowHead ? std::min(fs, std::max(9.0f, pf)) : fs;
    const float numTw = mono->CalcTextSizeA(numFs, FLT_MAX, 0, number.c_str()).x;
    if (narrowHead && tw - edge * 2 - numTw - 3 < lampW) lampW = 0;  // too narrow for both: the number wins
    const float textY = std::round(h0.y + 3 + (headH - 4 - fs) * 0.5f);
    if (narrowHead) {
      dl->AddText(mono, numFs, ImVec2(h0.x + edge, std::round(h0.y + 3 + (headH - 4 - numFs) * 0.5f)), nameInk, number.c_str());
      const float room = tw - edge * 2 - numTw - 4 - (lampW > 0 ? lampW + 3 : 0) - armW;
      if (room >= a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, "Abc").x) {
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(h0.x + edge + numTw + 4, textY), room, nameInk, ch->name.c_str());
      }
    } else {
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(h0.x + 6, textY), tw - 12 - lampW - btnW * 2 - 8 - armW, nameInk, name.c_str());
    }
    const ImVec2 l0(h1.x - edge - lampW, std::round(h0.y + 3 + (headH - 4 - lampW) * 0.5f)), l1(l0.x + lampW, l0.y + lampW);
    // cp16: a lamp in a sunken bevel (lit green while it plays, dark when muted, the solo colour).
    if (lampW > 0) {
      trackerview::bevelBox(dl, ImVec2(l0.x - 1, l0.y - 1), ImVec2(l1.x + 1, l1.y + 1), true);
      dl->AddRectFilled(ImVec2(l0.x + 1, l0.y + 1), ImVec2(l1.x - 1, l1.y - 1), soloed ? P.solo : muted ? theme::withAlpha(P.led, 0.15f) : P.led);
    }
    // The column buttons show while the pointer is over the header (they are rarely used).
    const bool headHov = ImGui::IsMouseHoveringRect(h0, h1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::PushID(static_cast<int>(ti));
    ImGui::SetCursorScreenPos(ImVec2(l0.x - 2, l0.y - 2));
    if (ImGui::InvisibleButton("##mute", ImVec2(lampW + 4, lampW + 4))) a.doc.setParam(channelParam(ch->id, ChannelParam::Mute), muted ? 0 : 1, false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ops::toggleChannelSolo(a.doc, ch->id, soloMemory());
    if (ImGui::IsItemHovered()) hoverTrackHead = static_cast<int>(ti);
    ui::Hint(soloed ? "Soloed: every other channel is muted \xC2\xB7 right-click: back to normal \xC2\xB7 click: mute"
             : muted ? "Muted \xC2\xB7 click: unmute \xC2\xB7 right-click: solo this channel"
                     : "Playing \xC2\xB7 click: mute \xC2\xB7 right-click: solo this channel");
    if (armShown) {  // K-123: the record dot
      const float r = std::round(fs * 0.3f);
      const ImVec2 c(std::round(l0.x - 4 - btnW * 2 - armW * 0.5f - 1), std::round((l0.y + l1.y) * 0.5f));
      ImGui::SetCursorScreenPos(ImVec2(c.x - armW * 0.5f, l0.y - 2));
      if (ImGui::InvisibleButton("##notearm", ImVec2(armW, lampW + 4))) midiarm::toggle(a, ch->id, io.KeyAlt);
      const bool armHot = ImGui::IsItemHovered();
      midiarm::drawDot(dl, c, r, noteArmed, armHot);
      if (armHot) hoverTrackHead = static_cast<int>(ti);
      ui::Hint(midiarm::dotHint(noteArmed));
    }
    // + and - note columns.
    for (int b = 0; b < (narrowHead ? 0 : 2); ++b) {
      const ImVec2 b0(l0.x - 4 - btnW * static_cast<float>(2 - b), h0.y + 3), b1(b0.x + btnW, h1.y);
      ImGui::SetCursorScreenPos(b0);
      ImGui::PushID(b);
      const bool click = ImGui::InvisibleButton("##cols", ImVec2(btnW, std::max(1.0f, b1.y - b0.y)));
      const bool bh = ImGui::IsItemHovered();
      ImGui::PopID();
      if (headHov) theme::centreText(dl, mono, fs, b0, b1, bh ? P.text : P.textFaint, b == 0 ? "-" : "+");
      ui::Hint(b == 0 ? "One note column fewer (only empty columns go)" : "One more note column (for chords)");
      if (click) {
        int& extra = st().extraCols[ch->id];
        extra = std::clamp(extra + (b == 0 ? -1 : 1), 0, trk::kMaxColumns - std::max(1, t.columns - extra));
      }
    }
    // cp16 (user): the header's right edge drags every track wider or narrower (in characters; a
    // double-click: back to as narrow as they go). Kept in the settings, a view preference that
    // never touches the song (saves stay the same in every layout, and collab peers keep theirs).
    {
      static float grabX = 0;
      static float grabTw = 0;
      ImGui::SetCursorScreenPos(ImVec2(tx + tw - 3, a0.y));
      ImGui::InvisibleButton("##trwidth", ImVec2(6, std::max(1.0f, headH - 1)));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsItemActivated()) {
        grabX = io.MousePos.x, grabTw = tw;
        if (gZoomEff != 0 || gZoomFit) zoomPattern(a, 99);  // (cp17: widening by hand starts from the plain zoom)
      }
      if (ImGui::IsItemActive() && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // The edge follows the pointer: the width it asks for, in characters past the cells
        // (at the plain glyph size, which a track widened by hand keeps).
        const Metrics m = metricsAt(fs);
        const float cells = m.pad * 2 + m.colW * static_cast<float>(t.columns);
        const int w = std::clamp(static_cast<int>(std::lround((grabTw + io.MousePos.x - grabX - cells) / std::max(1.0f, m.cw))), 0, 40);
        if (w != gTrackWidth && std::fabs(io.MousePos.x - grabX) >= 2) gTrackWidth = w, a.settings.set("tracker.trackWidth", static_cast<double>(w));
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        gTrackWidth = 0;
        a.settings.set("tracker.trackWidth", 0.0);
      }
      ui::Hint("Drag: every track wider or narrower (the ones that do not fit scroll) \xC2\xB7 double-click: back to fitting the view");
    }
    ImGui::PopID();
    // A track shown from another pattern playing here: a wash in that pattern's colour.
    if (v.src[ti] != v.p) dl->AddRectFilled(ImVec2(tx, body0), ImVec2(tx + tw, body1), theme::withAlpha(theme::rgb(v.src[ti]->color), 0.08f));
    // Cells.
    const std::vector<Note>& tNotes = notesRef(*v.src[ti], t.channel);
    const std::vector<uint8_t>& tSteps = stepsRef(*v.src[ti], t.channel);
    const std::vector<StepProps>& tProps = propsRef(*v.src[ti], t.channel);
    // K-106: an imported module's cells, in its own notation, on the rows each voice sounds through
    // (read from the notes' cells, nothing converted or stored).
    std::vector<tracker::Cell> mod;
    std::vector<uint8_t> modSet;
    for (size_t i = 0; i < tNotes.size() && i < t.col.size(); ++i) {
      const Note& n = tNotes[i];
      if (n.tracker.empty() || t.col[i] >= t.columns || v.g.rowTicks <= 0 || n.start < 0) continue;
      if (mod.empty()) mod.assign(static_cast<size_t>(std::max(0, t.rows)) * static_cast<size_t>(t.columns), {}), modSet.assign(mod.size(), 0);
      const int startRow = static_cast<int>(n.start / v.g.rowTicks);
      // The rows the note covers (trk::moduleNoteAt's rule, so an edit finds the cell shown).
      const Tick end = n.start + std::max<Tick>(1, n.length);
      const int cover = std::max(1, std::min(static_cast<int>((end + v.g.rowTicks - 1) / v.g.rowTicks), v.g.rows) - startRow);
      const std::vector<tracker::Cell> cells = trk::moduleRows(n, std::min(cover, std::max(0, t.rows - startRow)));
      for (size_t k = 0; k < cells.size(); ++k) {
        const int r = startRow + static_cast<int>(k);
        if (r >= t.rows) break;
        // R10-08: a note on a later row that is not a glide's target (3xx, 5xx, Mx) starts a voice of
        // its own (a note delay, ED3, makes it a separate note): that row and the rest are its voice's,
        // shown with it, not again as this one's.
        const tracker::Cell& kc = cells[k];
        if (k > 0 && kc.note >= 1 && kc.note <= 96 && kc.efx != 3 && kc.efx != 5 && kc.vol < 0xF0) break;
        const size_t at = static_cast<size_t>(r) * static_cast<size_t>(t.columns) + static_cast<size_t>(t.col[i]);
        mod[at] = cells[k];
        modSet[at] = 1;
      }
    }
    for (int i = 0; i < visRows; ++i) {
      const int row = firstRow + i;
      if (row < 0 || row >= v.g.rows) continue;
      const float y = body0 + static_cast<float>(i) * rowH + std::round((rowH - pf) * 0.5f);
      for (int c = 0; c < t.columns; ++c) {
        const float cx = std::round(tx + inset[ti] + colW * static_cast<float>(c) + 3);
        const Cell& ce = t.at(row + v.rowOff[ti], c);
        const Note* n = ce.kind == Cell::On || ce.kind == Cell::Off ? &tNotes[static_cast<size_t>(ce.note)] : nullptr;
        const bool dimmed = muted || (n && n->muted);
        const ImU32 faint = theme::withAlpha(P.textFaint, 0.55f);
        auto put = [&](Field f, const std::string& text, ImU32 col) {
          if (!fieldShown(f)) return;
          if (!gDots && col == faint) return;  // empty fields blank
          dl->AddText(mono, pf, ImVec2(cx + cw * static_cast<float>(fieldOffset(f)), y), dimmed ? theme::withAlpha(col, 0.45f) : col, text.c_str());
        };
        const int mr = row + v.rowOff[ti];
        const size_t mAt = static_cast<size_t>(std::max(0, mr)) * static_cast<size_t>(t.columns) + static_cast<size_t>(c);
        const tracker::Cell* mc = mr >= 0 && mAt < modSet.size() && modSet[mAt] ? &mod[mAt] : nullptr;
        if (mc && ce.kind != Cell::On) {
          // A row the module's voice sounds through: its volume column and effect (a key off as OFF).
          if (ce.kind == Cell::Off) put(Field::Note, "OFF", P.textDim);
          else if (mc->note == 97) put(Field::Note, "OFF", P.textDim);
          // R9-03: a note inside a voice is a glide's target (3xx, 5xx, Mx: the voice slides to it, no
          // new note starts): shown as the module wrote it, read only (module note 49 is C-4).
          else if (mc->note >= 1 && mc->note <= 96) put(Field::Note, trk::noteText(mc->note + 11, gFlats), P.textDim);
          else put(Field::Note, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
          if (ce.offGrid && ce.kind == Cell::Off) dl->AddText(mono, pf, ImVec2(cx + cw * 3, y), P.hot, "~");
          if (mc->instr) put(Field::Instrument, trk::hex2(mc->instr), theme::withAlpha(fieldColour(Field::Instrument), 0.7f));
          else put(Field::Instrument, "\xC2\xB7\xC2\xB7", faint);
          const std::string vt = trk::moduleVolText(mc->vol), et = trk::moduleEffectText(mc->efx, mc->param);
          put(Field::Volume, vt.empty() ? std::string("\xC2\xB7\xC2\xB7") : vt, vt.empty() ? faint : fieldColour(Field::Volume));
          put(Field::Pan, "\xC2\xB7\xC2\xB7", faint);
          put(Field::Effect, et.empty() ? std::string("\xC2\xB7\xC2\xB7\xC2\xB7") : et, et.empty() ? faint : fieldColour(Field::Effect));
          continue;
        }
        switch (ce.kind) {
          case Cell::Empty:
            put(Field::Note, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
            put(Field::Instrument, "\xC2\xB7\xC2\xB7", faint);
            put(Field::Volume, "\xC2\xB7\xC2\xB7", faint);
            put(Field::Pan, "\xC2\xB7\xC2\xB7", faint);
            put(Field::Effect, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
            break;
          case Cell::Off:
            put(Field::Note, "OFF", P.textDim);
            if (ce.offGrid) dl->AddText(mono, pf, ImVec2(cx + cw * 3, y), P.hot, "~");
            put(Field::Volume, "\xC2\xB7\xC2\xB7", faint);
            put(Field::Pan, "\xC2\xB7\xC2\xB7", faint);
            put(Field::Effect, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
            break;
          case Cell::On: {
            put(Field::Note, trk::noteText(n->pitch, gFlats), fieldColour(Field::Note));
            put(Field::Instrument, trk::hex2(sg.channelIndex(ch->id) + 1), fieldColour(Field::Instrument));
            const std::string mvt = mc ? trk::moduleVolText(mc->vol) : std::string(), met = mc ? trk::moduleEffectText(mc->efx, mc->param) : std::string();
            if (mc) put(Field::Volume, mvt.empty() ? std::string("\xC2\xB7\xC2\xB7") : mvt, mvt.empty() ? faint : fieldColour(Field::Volume));
            else put(Field::Volume, trk::hex2(n->velocity), fieldColour(Field::Volume));
            put(Field::Pan, n->pan == 0 ? std::string("\xC2\xB7\xC2\xB7") : trk::hex2(n->pan + 64), n->pan == 0 ? faint : fieldColour(Field::Pan));
            const std::vector<trk::Effect> fx = trk::effectsOf(*n);
            if (!met.empty()) put(Field::Effect, met, fieldColour(Field::Effect));  // the module's effect on its own row
            else if (const char held = fx.empty() ? pendingEffect(a, v.src[ti]->id, t.channel, n) : 0) put(Field::Effect, std::string(1, held) + "..", fieldColour(Field::Effect));
            else if (fx.empty()) put(Field::Effect, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
            else put(Field::Effect, std::string(1, fx[0].letter) + (fx[0].exact ? trk::hex2(fx[0].value) : std::string("!!")), fieldColour(Field::Effect));
            // Marks in the gaps: off the row grid (~), more letters (+), expression or a curved bend (e).
            if (ce.offGrid) dl->AddText(mono, pf, ImVec2(cx + cw * 3, y), P.hot, "~");
            if (fx.size() > 1 && fieldShown(Field::Effect))
              dl->AddText(mono, pf, ImVec2(cx + cw * static_cast<float>(fieldOffset(Field::Effect) - 1), y), P.accent, "+");
            if (!n->pressure.empty() || !n->timbre.empty() || !n->bend.empty()) {
              const int at = fieldShown(Field::Pan) ? fieldOffset(Field::Pan) - 1 : fieldShown(Field::Volume) ? fieldOffset(Field::Volume) + 2 : -1;
              if (at > 0) dl->AddText(mono, pf, ImVec2(cx + cw * static_cast<float>(at), y), P.textDim, "e");
            }
            break;
          }
          case Cell::Step: {
            const size_t si = static_cast<size_t>(ce.step);
            const int pitch = si < tProps.size() && tProps[si].pitch >= 0 ? tProps[si].pitch : 60;
            put(Field::Note, trk::noteText(pitch, gFlats), theme::mix(fieldColour(Field::Note), P.led, 0.35f));
            put(Field::Instrument, trk::hex2(sg.channelIndex(ch->id) + 1), fieldColour(Field::Instrument));
            put(Field::Volume, trk::hex2(si < tSteps.size() ? tSteps[si] : 0), fieldColour(Field::Volume));
            const int pan = si < tProps.size() ? tProps[si].pan : 0;
            put(Field::Pan, pan == 0 ? std::string("\xC2\xB7\xC2\xB7") : trk::hex2(pan + 64), pan == 0 ? faint : fieldColour(Field::Pan));
            put(Field::Effect, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
            // The step mark: a thin lit bar at the cell's left edge (the text keeps its spacing).
            const float bw = std::max(1.0f, std::round(2 * dpp) / dpp);
            const ImVec2 m0(std::round(cx - 3), std::round(y + 1));
            dl->AddRectFilled(m0, ImVec2(m0.x + bw, std::round(y + pf - 1)), P.led);
            if (ce.offGrid) dl->AddText(mono, pf, ImVec2(cx + cw * 3, y), P.hot, "~");
            break;
          }
        }
      }
      if (row == v.g.rows - 1 && v.rowOff[ti] == 0 && t.pastEnd > 0) {
        const std::string past = "+" + std::to_string(t.pastEnd);
        dl->AddText(a.fontRegular, fs * 0.85f, ImVec2(tx + tw - 4 - cw * static_cast<float>(past.size()), y), P.hot, past.c_str());
      }
    }
    if (t.voices > trk::kMaxColumns - 1) dl->AddText(a.fontRegular, fs * 0.85f, ImVec2(tx + 6, a0.y + headH), P.hot, "+");
  }
  // Selection.
  if (s.sel.on) {
    const int tl = s.sel.tLo(), th = std::min(s.sel.tHi(), static_cast<int>(v.tracks.size()) - 1);
    const float sx0 = gx0 + trackX[static_cast<size_t>(tl)] - scrollX;
    const float sx1 = gx0 + trackX[static_cast<size_t>(th)] - scrollX + trackW[static_cast<size_t>(th)];
    const float sy0 = body0 + static_cast<float>(s.sel.rLo() - firstRow) * rowH, sy1 = body0 + static_cast<float>(s.sel.rHi() - firstRow + 1) * rowH;
    dl->AddRectFilled(ImVec2(sx0, std::max(sy0, body0)), ImVec2(sx1, std::min(sy1, body1)), P.selection);
  }
  // Collaborators on this pattern: their block (a wash in their colour) and their cursor (the
  // row of their track outlined, their name beside it). Ticks map to rows here, so a cursor from
  // any layout (a piano roll pointer too) lands on the row it is in.
  {
    auto trackOf = [&](uint32_t ch) {
      for (size_t i = 0; i < v.tracks.size(); ++i)
        if (v.tracks[i].channel == ch) return static_cast<int>(i);
      return -1;
    };
    auto rowOf = [&](double tick) { return static_cast<int>(std::floor(tick / static_cast<double>(std::max<Tick>(1, v.g.rowTicks)))); };
    auto trackR = [&](int ti) { return gx0 + trackX[static_cast<size_t>(ti)] - scrollX + trackW[static_cast<size_t>(ti)]; };
    for (const collabui::PeerEdit& e : collabui::peerEdits(a)) {
      if (e.at.pattern != v.p->id) continue;
      const ImU32 col = theme::rgb(e.color);
      if (e.at.selTick1 > e.at.selTick0) {
        int t0 = trackOf(e.at.selChannel0), t1 = trackOf(e.at.selChannel1);
        if (t0 > t1) std::swap(t0, t1);
        const int r0 = rowOf(e.at.selTick0), r1 = rowOf(e.at.selTick1 - 1);
        if (t0 >= 0 && r1 >= firstRow) {
          const float sy0 = body0 + static_cast<float>(r0 - firstRow) * rowH, sy1 = body0 + static_cast<float>(r1 - firstRow + 1) * rowH;
          dl->AddRectFilled(ImVec2(gx0 + trackX[static_cast<size_t>(t0)] - scrollX, std::max(sy0, body0)), ImVec2(trackR(t1), std::min(sy1, body1)),
                            theme::withAlpha(col, 0.18f));
        }
      }
      const int ti = trackOf(e.at.channel), row = rowOf(e.at.tick);
      if (ti < 0 || row < firstRow || row >= firstRow + visRows || row >= v.g.rows) continue;
      const float tx = gx0 + trackX[static_cast<size_t>(ti)] - scrollX;
      const float y = body0 + static_cast<float>(row - firstRow) * rowH;
      const float th = pixelgrid::whole(e.pointer ? 1.0f : 2.0f, dpp), in = pixelgrid::strokeIn(th);
      dl->AddRect(ImVec2(tx + 1 + in, y + in), ImVec2(trackR(ti) - 1 - in, y + rowH - in), col, 0, 0, th);
      const float nfs = fs * 0.8f;
      const float nw = a.fontRegular->CalcTextSizeA(nfs, FLT_MAX, 0, e.name.c_str()).x;
      const ImVec2 n0(std::round(trackR(ti) - 1 - nw - 6), std::round(y - nfs - 2));
      dl->AddRectFilled(n0, ImVec2(n0.x + nw + 6, n0.y + nfs + 2), col);
      dl->AddText(a.fontRegular, nfs, ImVec2(n0.x + 3, n0.y + 1), theme::contrastText(col), e.name.c_str());
    }
  }
  // This user's edit cursor and block, for collaborators (by channel ids and pattern ticks).
  {
    collabui::EditCursor ec;
    ec.pattern = v.p->id;
    ec.channel = v.tracks[static_cast<size_t>(s.cur.track)].channel;
    ec.tick = static_cast<double>(static_cast<Tick>(s.cur.row) * v.g.rowTicks);
    if (s.sel.on) {
      const int th = std::min(s.sel.tHi(), static_cast<int>(v.tracks.size()) - 1);
      ec.selChannel0 = v.tracks[static_cast<size_t>(std::clamp(s.sel.tLo(), 0, th))].channel;
      ec.selChannel1 = v.tracks[static_cast<size_t>(th)].channel;
      ec.selTick0 = static_cast<double>(static_cast<Tick>(s.sel.rLo()) * v.g.rowTicks);
      ec.selTick1 = static_cast<double>(static_cast<Tick>(s.sel.rHi() + 1) * v.g.rowTicks);
    }
    collabui::setEditCursor(ec);
  }
  // The cursor: the digit under it, outlined.
  {
    const trk::Track& t = v.tracks[static_cast<size_t>(s.cur.track)];
    (void)t;
    const float tx = gx0 + trackX[static_cast<size_t>(s.cur.track)] - scrollX;
    const float cx = std::round(tx + inset[static_cast<size_t>(s.cur.track)] + colW * static_cast<float>(s.cur.column) + 3);
    const int chars = s.cur.field == Field::Note ? 3 : 1;
    const float dx = cx + cw * static_cast<float>(fieldOffset(s.cur.field) + (s.cur.field == Field::Note ? 0 : s.cur.digit));
    const ImVec2 c0(std::round(dx - 1), centreY), c1(std::round(dx + cw * static_cast<float>(chars) + 1), centreY + rowH);
    s.cell0 = c0, s.cell1 = c1;
    const float in = pixelgrid::strokeIn(1.0f);
    dl->AddRectFilled(c0, c1, theme::withAlpha(s.edit ? P.arm : P.accent, 0.30f));
    dl->AddRect(ImVec2(c0.x + in, c0.y + in), ImVec2(c1.x - in, c1.y - in), s.hasKb ? P.text : P.textDim);
  }
  dl->PopClipRect();
  // Edit mode: a frame round the pattern in the record role. Keyboard focus: corner marks.
  if (s.edit) {
    const float t = pixelgrid::whole(2.0f, dpp);
    const float in = pixelgrid::strokeIn(t);
    dl->AddRect(ImVec2(a0.x + in, a0.y + in), ImVec2(a1.x - in, a1.y - in), P.arm, 0, 0, t);
  } else if (s.hasKb) {
    const float L = 10;
    const ImU32 col = P.accent;
    dl->AddLine(a0, ImVec2(a0.x + L, a0.y), col, 2), dl->AddLine(a0, ImVec2(a0.x, a0.y + L), col, 2);
    dl->AddLine(ImVec2(a1.x, a0.y), ImVec2(a1.x - L, a0.y), col, 2), dl->AddLine(ImVec2(a1.x, a0.y), ImVec2(a1.x, a0.y + L), col, 2);
    dl->AddLine(ImVec2(a0.x, a1.y), ImVec2(a0.x + L, a1.y), col, 2), dl->AddLine(ImVec2(a0.x, a1.y), ImVec2(a0.x, a1.y - L), col, 2);
    dl->AddLine(a1, ImVec2(a1.x - L, a1.y), col, 2), dl->AddLine(a1, ImVec2(a1.x, a1.y - L), col, 2);
  }
  dl->PopClipRect();
  (void)hoverTrackHead;

  // The tracks' scrollbar (T-19): a sunken trough, the raised thumb over the tracks in view.
  if (hbar) {
    const float span = std::max(1.0f, gx1 - gx0);
    trackerview::bevelBox(dl, ImVec2(gx0, body1), ImVec2(gx1, a1.y - 2), true);  // (above the edit frame's bottom edge)
    float th0 = gx0 + span * scrollX / contentW, th1 = std::min(gx1, th0 + std::max(span * viewW / contentW, sbH * 2));
    th0 = std::round(std::min(th0, th1 - sbH * 2)), th1 = std::round(th1);
    trackerview::bevelBox(dl, ImVec2(th0, body1 + 1), ImVec2(th1, a1.y - 3), false);
  }
  // Tracks scrolled out of view that hold notes in the rows shown (a stacked pattern playing on a
  // channel off to the right, say): a lit arrow with their count in the row number column's head,
  // on that side; a click scrolls to the nearest of them.
  {
    int nL = 0, nR = 0, nearL = -1, nearR = -1;
    for (size_t ti = 0; ti < v.tracks.size(); ++ti) {
      const float tx = gx0 + trackX[ti] - scrollX;
      const bool left = tx + trackW[ti] <= gx0 + 1, right = tx + trackW[ti] > gxEnd + 1;
      if (!left && !right) continue;
      const trk::Track& t = v.tracks[ti];
      bool has = false;
      for (int i = 0; i < visRows && !has; ++i)
        for (int c = 0; c < t.columns && !has; ++c) has = t.at(firstRow + i + v.rowOff[ti], c).kind != Cell::Empty;
      if (!has) continue;
      if (left) ++nL, nearL = static_cast<int>(ti);
      else if (nR++ == 0) nearR = static_cast<int>(ti);
    }
    s.offLeft = nL, s.offRight = nR;
    s.firstShown = first, s.lastShown = lastShown;
    for (int side = 0; side < 2; ++side) {
      const int n = side ? nR : nL;
      if (n == 0) continue;
      const ImVec2 b0(side ? gx1 + 1 : a0.x, a0.y), b1(side ? a1.x : gx0 - 1, a0.y + headH - 1);
      trackerview::bevelBox(dl, b0, b1, false);
      const std::string t = side ? std::to_string(n) + ">" : "<" + std::to_string(n);
      theme::centreText(dl, mono, fs, b0, b1, P.hot, t.c_str());
      ImGui::SetCursorScreenPos(b0);
      ImGui::PushID(side);
      if (ImGui::InvisibleButton("##offscreen", ImVec2(std::max(1.0f, b1.x - b0.x), std::max(1.0f, b1.y - b0.y)))) {
        const int to = side ? nearR : nearL;
        int f = to;
        if (side)
          while (f > 0 && trackX[static_cast<size_t>(to)] + trackW[static_cast<size_t>(to)] - trackX[static_cast<size_t>(f - 1)] <= viewW + 1) --f;
        scrollTo(f);
      }
      ImGui::PopID();
      ui::Hint(side ? "Tracks to the right have notes in these rows: click to scroll to them (Shift+wheel scrolls the tracks)"
                    : "Tracks to the left have notes in these rows: click to scroll to them (Shift+wheel scrolls the tracks)");
    }
  }

  // ---- the pattern bar: what is shown, the cell under the cursor, the last message ----
  {
    ImVec2 b0(a0.x, a1.y + 2), b1(a1.x, a1.y + barH);
    trackerview::bevelBox(dl, b0, b1);
    const Pattern& p = *v.p;
    char info[200];
    std::snprintf(info, sizeof info, "%s %s \xC2\xB7 %s %s \xC2\xB7 %s %d \xC2\xB7 %s %d \xC2\xB7 %s %d \xC2\xB7 %s %s", i18n::tr("Pattern"),
                  trk::hex2(trk::patternNumber(song, p.id)).c_str(), i18n::tr("length"), trk::rowText(trk::playedRows(song, p, v.g), hex).c_str(), i18n::tr("rows per beat"),
                  v.g.rowsPerBeat, i18n::tr("edit step"), s.step, i18n::tr("octave"), a.typing.octave, i18n::tr("follow"),
                  s.follow ? i18n::tr("on") : i18n::tr("off"));
    std::string right;
    const trk::Track& t = v.tracks[static_cast<size_t>(s.cur.track)];
    const Cell& ce = t.at(s.cur.row + v.rowOff[static_cast<size_t>(s.cur.track)], s.cur.column);
    if (ImGui::GetTime() - s.statusT < 4.0 && !s.status.empty()) {
      right = s.status;
    } else if (ce.kind == Cell::On) {
      const Note& n = notesRef(*v.src[static_cast<size_t>(s.cur.track)], t.channel)[static_cast<size_t>(ce.note)];
      const std::vector<trk::Effect> fx = trk::effectsOf(n);
      if (s.cur.field == Field::Effect && !fx.empty()) right = trk::effectHelp(fx[0].letter, fx[0].value);
      else {
        char b[160];
        std::snprintf(b, sizeof b, "%s \xC2\xB7 %s %lld \xC2\xB7 %s %lld %s", trk::noteText(n.pitch, gFlats).c_str(), i18n::tr("starts at tick"),
                      static_cast<long long>(n.start), i18n::tr("lasts"), static_cast<long long>(n.length), i18n::tr("ticks"));
        right = b;
      }
    } else if (ce.kind == Cell::Step) {
      right = i18n::tr("A rack step: Delete clears it, the volume sets its velocity; the rest is edited in Steps");
    } else if (s.cur.field == Field::Effect) {
      right = trk::effectHelp(0, 0);
    }
    // cp25: audio and automation clips playing with this pattern's entry: the pattern cannot show them,
    // so the bar names them (read-only; the Song page's lanes show and edit them). Worked out again
    // only when the song or the pattern shown changes.
    {
      static uint64_t clipsRev = ~0ull;
      static ObjectId clipsPat = kNoId;
      static std::string clipsNote;
      if (clipsRev != a.doc.revision() || clipsPat != p.id) {
        clipsRev = a.doc.revision();
        clipsPat = p.id;
        clipsNote.clear();
        int audio = 0, autos = 0;
        for (const trk::OrderEntry& e : trk::orderList(song)) {
          if (e.pattern != p.id) {
            continue;
          }
          for (const Clip& c : song.playlist) {
            if (c.kind == ClipKind::Pattern || c.start >= e.end || c.start + std::max<Tick>(song.clipLength(c), 1) <= e.start) {
              continue;
            }
            (c.kind == ClipKind::Audio ? audio : autos) += 1;
          }
          break;
        }
        if (audio + autos > 0) {
          clipsNote = std::string(i18n::tr("Also in this entry:")) + " ";
          if (audio > 0) {
            clipsNote += std::to_string(audio) + " " + (audio == 1 ? i18n::tr("audio clip") : i18n::tr("audio clips"));
          }
          if (autos > 0) {
            clipsNote += std::string(audio > 0 ? ", " : "") + std::to_string(autos) + " " + (autos == 1 ? i18n::tr("automation clip") : i18n::tr("automation clips"));
          }
          clipsNote += std::string(" (") + i18n::tr("the Song page's lanes") + ")";
        }
      }
      if (right.empty()) {
        right = clipsNote;
      }
    }
    // N-09: the song plays an audio part (no pattern there): the editor keeps the pattern picked,
    // dimmed, and the bar says so.
    const Tick songPos = static_cast<Tick>(a.engine.position());
    const bool auPart = a.engine.playing() && a.engine.mode() == PlayMode::Song && songPos < song.lengthTicks() && trk::patternAt(song, songPos) == kNoId;
    if (auPart) {
      dl->AddRectFilled(ImVec2(a0.x, a0.y + headH), a1, theme::withAlpha(P.surface950, 0.45f));
      right = i18n::tr("No pattern plays here (an audio part, on the Song page): this is the pattern picked");
    } else if (s.stacked > 0 && (ImGui::GetTime() - s.statusT >= 4.0 || s.status.empty()))
      right = std::to_string(s.stacked) + " " + i18n::tr(s.stacked == 1 ? "more pattern plays here (the Song page shows it)" : "more patterns play here (the Song page shows them)");
    const float ty = std::round(b0.y + (barH - 2 - fs) * 0.5f);
    // R10-06: a message just said (a refusal, what an edit did) takes the whole bar, the info giving
    // way, so it reads from its start; one still too long has its full text in a hint.
    const bool fresh = ImGui::GetTime() - s.statusT < 4.0 && !s.status.empty() && right == s.status;
    const float rx = fresh ? b0.x + 8 : b0.x + 8 + mono->CalcTextSizeA(fs, FLT_MAX, 0, info).x + 24;
    if (!fresh) dl->AddText(mono, fs, ImVec2(b0.x + 8, ty), P.textDim, info);
    const char* mode = s.edit ? i18n::tr("Edit mode") : i18n::tr("Play keys");
    const float mw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, mode).x;
    // cp17 (user): the tracks' zoom at the bar's right, beside the mode: narrower, wider, every
    // track in view, and the step shown (a view preference kept in the settings).
    float zx = b1.x - 8 - std::max(mw, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Play keys")).x) - 12;
    {
      const float bh = barH - 6, by = b0.y + 2;
      const float fitW = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Fit")).x) + 14, pmW = std::round(bh * 1.3f);
      zx -= fitW;
      if (bevelButton(a, "##zfit", "Fit", ImVec2(zx, by), ImVec2(zx + fitW, by + bh), gZoomFit, "Every track in view (Ctrl+Alt+/): the tracks narrow as far as they need"))
        zoomPattern(a, 0);
      zx -= pmW + 2;
      if (bevelButton(a, "##zin", "+", ImVec2(zx, by), ImVec2(zx + pmW, by + bh), false, "Wider tracks, fewer in view (Ctrl+Alt+. or Ctrl+wheel up)"))
        zoomPattern(a, 1);
      zx -= pmW + 2;
      if (bevelButton(a, "##zout", "-", ImVec2(zx, by), ImVec2(zx + pmW, by + bh), false,
                      "Narrower tracks, more in view (Ctrl+Alt+, or Ctrl+wheel down): first the effect and pan go, then the volume, the instrument, then smaller glyphs; zoom back in to edit them"))
        zoomPattern(a, -1);
      char zt[32];
      std::snprintf(zt, sizeof zt, "%s %d", i18n::tr("Zoom"), gZoomEff);
      const float zw = mono->CalcTextSizeA(fs, FLT_MAX, 0, zt).x;
      zx -= zw + 8;
      dl->AddText(mono, fs, ImVec2(zx, ty), gZoomEff > 0 ? P.text : P.textDim, zt);
    }
    const float rw = zx - rx - 12;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(rx, ty), rw, P.text, right.c_str());
    if (!right.empty() && a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str()).x > rw && ImGui::IsMouseHoveringRect(ImVec2(rx, b0.y), ImVec2(rx + rw, b1.y)))
      ImGui::SetTooltip("%s", right.c_str());
    dl->AddText(a.fontRegular, fs, ImVec2(b1.x - 8 - mw, ty), s.edit ? P.arm : P.textFaint, mode);
  }
  ImGui::SetCursorScreenPos(ImVec2(a0.x, a1.y + barH));
  ImGui::Dummy(ImVec2(1, 0));
}

const char* menuWords(const char* label) {
  if (!label || !*label) return label;
  static const std::pair<const char*, const char*> kWords[] = {
      {"Piano roll", "Roll"},     {"piano roll", "roll"},     {"Playlist", "Song"},           {"playlist", "song"},
      {"Channel rack", "Steps"},  {"channel rack", "steps"},  {"Step sequencer", "Steps"},    {"step sequencer", "steps"},
      {"Mixer", "Mix"},           {"mixer", "mix"}};  // R12-06: the page strip says Mix
  // Most labels hold none of the words: returned as they are.
  const char* idPos = std::strstr(label, "##");
  const size_t textLen = idPos ? static_cast<size_t>(idPos - label) : std::strlen(label);
  bool any = false;
  for (const auto& w : kWords) {
    const char* f = std::strstr(label, w.first);
    if (f && static_cast<size_t>(f - label) < textLen) any = true;
  }
  if (!any) return label;
  // cp22: the labels made live in two generations, so a pointer handed out is never freed while it
  // may still be read. Labels made on the fly (file names) never pile up: when the current
  // generation is full it becomes the old one and the one before it is freed, at most once a frame.
  // A label asked for again moves (its node, so its storage stays put) from the old generation to
  // the current one; so every label returned in a frame is in the current generation for the rest
  // of that frame and survives at least until a later frame's swap, and labels used every frame stay.
  static std::map<std::string, std::string> gens[2];
  static int curGen = 0, swappedFrame = -1;
  constexpr size_t kGeneration = 2048;
  std::map<std::string, std::string>& cur = gens[curGen];
  if (const auto it = cur.find(label); it != cur.end()) {
    return it->second.c_str();
  }
  std::map<std::string, std::string>& old = gens[curGen ^ 1];
  if (const auto it = old.find(label); it != old.end()) {
    return cur.insert(old.extract(it)).position->second.c_str();
  }
  std::string text(label, textLen);
  for (const auto& w : kWords) {
    for (size_t at = text.find(w.first); at != std::string::npos; at = text.find(w.first, at + std::strlen(w.second))) {
      text.replace(at, std::strlen(w.first), w.second);
    }
  }
  if (idPos) {
    text += idPos;
  }
  const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
  if (cur.size() >= kGeneration && frame != swappedFrame) {
    swappedFrame = frame;
    old.clear();  // nothing returned this frame lives there (it moved to the current generation)
    curGen ^= 1;
  }
  return gens[curGen].emplace(label, std::move(text)).first->second.c_str();
}

// cp22 (dev): menuWords under load (the label-invalidation fix): many labels, the right words, and
// a label in use keeps its text.
static bool menuWordsSelfCheck() {
  const char* keep = menuWords("Piano roll##keep");
  const std::string keepText = keep;
  bool ok = keepText == "Roll##keep";
  for (int i = 0; i < 6000 && ok; ++i) {
    const std::string l = "Playlist " + std::to_string(i);
    const char* m = menuWords(l.c_str());
    ok = m && std::string(m) == "Song " + std::to_string(i);
    if (i % 500 == 0) {
      ok = ok && std::string(menuWords("Piano roll##keep")) == keepText;  // used now and then: kept
    }
  }
  return ok;
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  State& s = st();
  const std::string cmd = args.empty() ? "" : args[0];
  auto arg = [&](size_t i, int d) { return i < args.size() ? std::atoi(args[i].c_str()) : d; };
  if (cmd == "focus") {
    s.wantFocus = true;
    bringPage("Pattern editor");
  } else if (cmd == "cursor") {  // the track by number, or by its channel's name
    s.cur.track = arg(1, 0), s.cur.row = arg(2, 0), s.cur.column = arg(3, 0);
    if (args.size() > 1 && !args[1].empty() && (args[1][0] < '0' || args[1][0] > '9')) {
      const Song& song = a.doc.song();
      for (size_t i = 0; i < song.channels.size(); ++i)
        if (song.channels[i].name == args[1]) s.cur.track = static_cast<int>(i);
    }
    s.cur.field = static_cast<Field>(std::clamp(arg(4, 0), 0, static_cast<int>(Field::Count) - 1));
    s.cur.digit = 0;
    s.sel.on = false;
  } else if (cmd == "busiest") {
    // K-106 (dev, showcase photos): the song position moves to the pattern with the most notes
    // and steps that the song plays (its first place in the song), which then shows.
    // (Among the order list's entries, so the pattern is the one its entry shows.)
    // cp14: the most tracks in use first (a photo of three busy tracks among twenty empty ones reads
    // as an empty pattern), then the most notes; the cursor goes to the pattern's middle row, so the
    // rows fill the editor above and below it.
    const Song& song = a.doc.song();
    const Pattern* best = nullptr;
    Tick at = 0;
    size_t most = 0, bestTracks = 0;
    for (const trk::OrderEntry& e : trk::orderList(song)) {
      const Pattern* p = song.pattern(e.pattern);
      if (!p) continue;
      size_t n = 0, tracks = 0;
      std::set<ObjectId> used;
      for (const auto& [ch, v] : p->notes)
        if (!v.empty()) n += v.size(), used.insert(ch);
      for (const auto& [ch, v] : p->steps)
        for (uint8_t x : v)
          if (x != 0) ++n, used.insert(ch);
      tracks = used.size();
      if (tracks > bestTracks || (tracks == bestTracks && n > most)) best = p, most = n, bestTracks = tracks, at = e.start;
    }
    if (best) {
      a.selectPattern(best->id);
      setSongPosition(a, at);
      s.cur.row = std::max(0, trk::gridOf(song, *best).rows / 2);
      s.sel.on = false;
    }
    std::printf("tracker busiest frame %d: %s (%zu notes and steps)\n", frame, best ? best->name.c_str() : "none", most);
  } else if (cmd == "mididevs") {
    // K-106 (dev): N stand-in MIDI input devices listed (nothing is opened), for the MIDI boxes'
    // long lists in hidden runs.
    if (a.midi) {
      const int n = std::clamp(arg(1, 8), 0, 64);
      for (int i = 1; i <= n; ++i) {
        const std::string name = "Test keyboard controller MIDI port " + std::to_string(i);
        if (std::find(a.midi->inputs.begin(), a.midi->inputs.end(), name) == a.midi->inputs.end()) a.midi->inputs.push_back(name);
        if (!a.midi->inputConfig(name)) {
          MidiInputConfig c;
          c.name = name;
          c.enabled = i % 3 != 0;
          a.midi->inputCfg.push_back(c);
        }
      }
    }
    std::printf("tracker mididevs frame %d: %zu inputs\n", frame, a.midi ? a.midi->inputs.size() : static_cast<size_t>(0));
  } else if (cmd == "page" && args.size() > 1) {
    std::string w = args[1];
    for (size_t i = 2; i < args.size(); ++i) w += " " + args[i];
    // The page strip's names too (Song is the Playlist window, Mix the Mixer, ...).
    static const std::pair<const char*, const char*> names[] = {{"Pattern", "Pattern editor"}, {"Song", "Playlist"},  {"Roll", "Piano roll"},
                                                                {"Steps", "Channel rack"},     {"Mix", "Mixer"},       {"Pads", "Live pads"},
                                                                {"Sample", "Loupe"},           {"Audio", "Loupe"}};
    for (const auto& [label, window] : names)
      if (w == label) w = window;
    showPage(a, w.c_str());
    std::printf("tracker page frame %d: %s\n", frame, w.c_str());
  } else if (cmd == "swap") {  // none|help|files|config|about
    const std::string w = args.size() > 1 ? args[1] : "none";
    setHeaderSwap(w == "help" ? 1 : w == "files" ? 2 : w == "config" ? 3 : w == "about" ? 4 : 0);
    if (w == "config" && args.size() > 2) setConfigPage(std::atoi(args[2].c_str()));
    if (w == "help" && args.size() > 2) setHelpSubject(std::atoi(args[2].c_str()));
  } else if (cmd == "loadsample" && args.size() > 1) {  // as a double-click on the file in Files, Samples
    filesLoadSample(a, args[1]);
    const Channel* c = a.doc.song().channel(a.selChannel);
    std::printf("tracker loadsample frame %d: %s\n", frame, c ? c->plugin.type.c_str() : "none");
  } else if (cmd == "tool" && args.size() > 1) {  // the scope box's tool (0 scopes .. 5 sample tools)
    const int t = std::atoi(args[1].c_str());
    if (scopeTool() != t) toggleScopeTool(a, t);
    std::printf("tracker tool frame %d: %d\n", frame, scopeTool());
  } else if (cmd == "extend") {
    setExtended(a, args.size() > 1 && args[1] == "on");
  } else if (cmd == "tracks") {  // cp16: the pattern's tracks (channel names, rack order) and those in view
    View v = build(a);
    std::printf("tracker tracks frame %d: n %zu rack %zu first %d last %d cursor %d width %d |", frame, v.tracks.size(), a.doc.song().channels.size(),
                s.firstShown, s.lastShown, s.cur.track, gTrackWidth);
    for (size_t i = 0; i < v.tracks.size(); ++i) std::printf(" %s(%d)", v.channels[i]->name.c_str(), v.tracks[i].columns);
    std::printf("\n");
    std::fflush(stdout);
  } else if (cmd == "songzoom") {  // cp17: the Song page's lanes and timeline zoom (TrackerPages)
    std::vector<std::string> sa = {"tracker"};
    sa.insert(sa.end(), args.begin(), args.end());
    trackerpages::devSongZoom(a, sa, frame);
  } else if (cmd == "editors") {  // cp18: the lower band's screens (TrackerEditors.h)
    trackereditors::devCommand(a, args, frame);
  } else if (cmd == "zoom") {  // cp17: "tracker zoom in|out|fit|plain|show": the pattern's sideways zoom
    const std::string w = args.size() > 1 ? args[1] : "show";
    if (w == "in") zoomPattern(a, 1);
    else if (w == "out") zoomPattern(a, -1);
    else if (w == "fit") zoomPattern(a, 0);
    else if (w == "plain") zoomPattern(a, 99);
    const View v = build(a);
    // (The view as last drawn: the tracks wholly in view, the cursor's track and its field.)
    std::printf("tracker zoom frame %d: step %d fit %d tracks %zu first %d last %d cursor %d field %d inview %d settings %d %d\n", frame, gZoomEff,
                gZoomFit ? 1 : 0, v.tracks.size(), s.firstShown, s.lastShown, s.cur.track, static_cast<int>(s.cur.field),
                s.cur.track >= s.firstShown && s.cur.track <= s.lastShown ? 1 : 0, static_cast<int>(a.settings.getNumber("tracker.patternZoom")),
                a.settings.getBool("tracker.patternFit") ? 1 : 0);
    std::fflush(stdout);
  } else if (cmd == "rack") {  // cp16: "rack move <i> <to>": the rack's own channel move (Document::moveChannel), any layout
    const auto& chs = a.doc.song().channels;
    const int i = arg(2, 0);
    if (args.size() > 1 && args[1] == "move" && i >= 0 && i < static_cast<int>(chs.size())) a.doc.moveChannel(chs[static_cast<size_t>(i)].id, arg(3, 0));
  } else if (cmd == "menuwords") {  // cp22: menuWords' label generations checked under load
    std::printf("tracker menuwords frame %d: %s\n", frame, menuWordsSelfCheck() ? "ok" : "FAIL");
    std::fflush(stdout);
  } else if (cmd == "perf") {  // cp16: the Tracker's frame timers (PerfScope), mean and 95th percentile per part
    perfRoll();
    const Perf& p = perf();
    static const char* const names[] = {"band", "pattern", "page", "editor"};
    std::printf("tracker perf frame %d:", frame);
    for (int k = 0; k < kPerfParts; ++k) {
      std::vector<float> v(p.ring[k], p.ring[k] + p.n[k]);
      double mean = 0;
      for (float x : v) mean += x;
      mean = v.empty() ? 0 : mean / static_cast<double>(v.size());
      std::sort(v.begin(), v.end());
      const float p95 = v.empty() ? 0.0f : v[std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.95))];
      std::printf(" %s n %d mean %.3f p95 %.3f ms |", names[k], p.n[k], mean, static_cast<double>(p95));
    }
    std::printf("\n");
    std::fflush(stdout);
  } else if (cmd == "order") {  // cp16: the Song page's order edits (dump, ins, del, move, set, clone, bars, loop)
    trackerpages::devOrder(a, args, frame);
  } else if (cmd == "songclip") {  // the Song page's picked clip: by its index in the playlist
    const int i = arg(1, -1);
    const auto& pl = a.doc.song().playlist;
    trackerpages::gSongClip = i >= 0 && i < static_cast<int>(pl.size()) ? pl[static_cast<size_t>(i)].id : kNoId;
  } else if (cmd == "edit") {
    s.edit = args.size() > 1 && args[1] == "on";
  } else if (cmd == "step") {
    s.step = std::clamp(arg(1, 1), 0, 16);
  } else if (cmd == "follow") {
    s.follow = args.size() > 1 && args[1] == "on";
  } else if (cmd == "select") {
    s.sel = {true, arg(1, 0), arg(2, 0), arg(3, 0), arg(4, 0)};
  } else if (cmd == "geometry") {
    std::printf("tracker geometry frame %d: area %.1f %.1f %.1f %.1f row %.1f cursor %.2f %.2f %.2f %.2f\n", frame, static_cast<double>(s.area0.x),
                static_cast<double>(s.area0.y), static_cast<double>(s.area1.x), static_cast<double>(s.area1.y), static_cast<double>(s.rowH),
                static_cast<double>(s.cell0.x), static_cast<double>(s.cell0.y), static_cast<double>(s.cell1.x), static_cast<double>(s.cell1.y));
    std::printf("tracker geometry frame %d: scrollbar %.1f %.1f %.1f %.1f scroll %.0f\n", frame, static_cast<double>(s.hbar0.x), static_cast<double>(s.hbar0.y),
                static_cast<double>(s.hbar1.x), static_cast<double>(s.hbar1.y), static_cast<double>(s.scrollX));
    // The centre pages: each window's rectangle, its dock node's, and whether it is the one shown.
    for (const char* pg : {"Pattern editor", "Playlist", "Channel rack", "Mixer", "Live pads", "Piano roll", "Browser", "Device chain", "Settings"}) {
      const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + pg).c_str());
      if (!w) continue;
      const ImGuiDockNode* n = w->DockNode;
      std::printf("tracker page %s: window %.0f %.0f %.0f %.0f node %.0f %.0f %.0f %.0f shown %d active %d hidden %d imgui frame %d last %d\n", pg,
                  static_cast<double>(w->Pos.x), static_cast<double>(w->Pos.y), static_cast<double>(w->Pos.x + w->Size.x), static_cast<double>(w->Pos.y + w->Size.y),
                  n ? static_cast<double>(n->Pos.x) : 0.0, n ? static_cast<double>(n->Pos.y) : 0.0, n ? static_cast<double>(n->Pos.x + n->Size.x) : 0.0,
                  n ? static_cast<double>(n->Pos.y + n->Size.y) : 0.0, n && n->VisibleWindow == w ? 1 : 0, w->Active ? 1 : 0, w->Hidden ? 1 : 0,
                  ImGui::GetFrameCount(), w->LastFrameActive);
      // Its child windows (a page's scrolled body): rectangle and clip.
      for (const ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
        if (c->ParentWindow == w && (c->Flags & ImGuiWindowFlags_ChildWindow))
          std::printf("tracker page %s child %s: window %.0f %.0f %.0f %.0f clip %.0f %.0f last %d hidden %d\n", pg, c->Name, static_cast<double>(c->Pos.x),
                      static_cast<double>(c->Pos.y), static_cast<double>(c->Pos.x + c->Size.x), static_cast<double>(c->Pos.y + c->Size.y),
                      static_cast<double>(c->ClipRect.Min.y), static_cast<double>(c->ClipRect.Max.y), c->LastFrameActive, c->Hidden ? 1 : 0);
    }
  } else if (cmd == "dump") {
    const View v = build(a);
    std::printf("tracker dump frame %d: pattern %d rows %d edit %d step %d octave %d keyboard %d cursor %d %d %d %d play %d undo %zu engine %d %u %u drawn %d off %d %d scroll %.0f stacked %d songpos %lld\n", frame,
                v.p ? trk::patternNumber(a.doc.song(), v.p->id) : 0, v.p ? v.g.rows : 0, s.edit ? 1 : 0, s.step, a.typing.octave, hasKeyboard() ? 1 : 0,
                s.cur.track, s.cur.row, s.cur.column, static_cast<int>(s.cur.field), s.playRow, a.doc.history().position(), a.engine.playing() ? static_cast<int>(a.engine.mode()) : -1, a.engine.pattern(), a.curPattern, s.drawnFrame, s.offLeft, s.offRight, static_cast<double>(s.scrollX), s.stacked, static_cast<long long>(songPosition(a)));
    std::printf("tracker status: %s\n", s.status.c_str());
    for (size_t ti = 0; v.p && ti < v.tracks.size(); ++ti) {
      const trk::Track& t = v.tracks[ti];
      const Rows r = rowsOf(*v.p, t.channel);
      for (int row = 0; row < v.g.rows; ++row)
        for (int c = 0; c < t.columns; ++c) {
          const Cell& ce = t.at(row, c);
          int mk = -1;  // K-106: a module cell on this row (its volume column and effect, as shown)
          if (const int mi = trk::moduleNoteAt(r.notes, t, v.g, row, c, &mk); mi >= 0) {
            const std::vector<tracker::Cell> mcs = trk::moduleRows(r.notes[static_cast<size_t>(mi)], mk + 1);
            const tracker::Cell& m = mcs[static_cast<size_t>(mk)];
            const std::string vt = trk::moduleVolText(m.vol), et = trk::moduleEffectText(m.efx, m.param);
            std::printf("  module %zu %d %d note %d instr %d vol %s fx %s\n", ti, row, c, m.note, m.instr, vt.empty() ? ".." : vt.c_str(), et.empty() ? "..." : et.c_str());
          }
          if (ce.kind == Cell::Empty) continue;
          if (ce.kind == Cell::On) {
            const Note& n = r.notes[static_cast<size_t>(ce.note)];
            const std::vector<trk::Effect> fx = trk::effectsOf(n);
            std::printf("  cell %zu %d %d %s %02X %02X %02X %s\n", ti, row, c, trk::noteText(n.pitch).c_str(), static_cast<int>(ti) + 1, n.velocity,
                        n.pan + 64, fx.empty() ? "..." : (std::string(1, fx[0].letter) + trk::hex2(fx[0].value)).c_str());
          } else if (ce.kind == Cell::Off) {
            std::printf("  cell %zu %d %d OFF\n", ti, row, c);
          } else {
            std::printf("  cell %zu %d %d step %02X\n", ti, row, c, r.steps[static_cast<size_t>(ce.step)]);
          }
        }
    }
    // Collaborators' cursors on this pattern, as the editor places them (track, row).
    for (const collabui::PeerEdit& e : collabui::peerEdits(a)) {
      if (!v.p || e.at.pattern != v.p->id) continue;
      int ti = -1;
      for (size_t i = 0; i < v.tracks.size(); ++i)
        if (v.tracks[i].channel == e.at.channel) ti = static_cast<int>(i);
      std::printf("  peer %s track %d row %d%s\n", e.name.c_str(), ti, static_cast<int>(e.at.tick / static_cast<double>(std::max<Tick>(1, v.g.rowTicks))),
                  e.pointer ? " pointer" : "");
    }
    std::printf("tracker dump end\n");
  }
  std::fflush(stdout);
}

}  // namespace gl::trackerview
