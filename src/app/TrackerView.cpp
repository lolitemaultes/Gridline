// Gridline - the tracker pattern editor (K-106). See TrackerView.h.
#include "TrackerView.h"
#include <SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#include "MenuParityUi.h"
#include "PixelGrid.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "TrackerLook.h"
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
};

struct State {
  Cursor cur;
  Selection sel;
  bool edit = false;
  int step = 1;
  bool follow = true;
  bool hasKb = false;      // the editor had the keyboard on the last frame
  bool wantFocus = false;  // dev / page switch: focus the editor next frame
  std::vector<KeyEv> keys;
  std::map<ObjectId, int> extraCols;  // note columns added by hand (view only)
  trk::Block clip;
  std::string status;
  double statusT = -10;
  std::map<int, Held> held;  // scan code -> the note it auditions
  int playRow = -1;
  ImVec2 area0, area1, cell0, cell1;  // last frame (dev geometry)
  float rowH = 14;
  ObjectId pattern = kNoId;
  ObjectId cursorChannel = kNoId;  // the channel the cursor was on (selection follows a move)
  int drawnFrame = -10;  // the ImGui frame the editor was last drawn in (a closed editor has no keyboard)
  std::string pendingPage;  // a centre page to bring to the front (bringPage)
  int pendingFrames = 0, pendingWait = 0;  // frames docked and waiting, frames in all
  int pendingHold = 0;  // frames to wait before the request looks (the layout re-docks its pages)
  int jumps[4] = {-1, -1, -1, -1};  // F9 to F12 rows stored with Shift (-1: 0, a quarter, half, three quarters)
  float wheel = 0;          // wheel movement not yet a whole row (trackpads send small steps)
  float scrollX = 0;        // the tracks' sideways scroll (always a track's left edge)
  float wheelX = 0;         // sideways wheel movement not yet a whole track
  ImVec2 hbar0, hbar1;      // the tracks' scrollbar last frame (none: both zero)
  long long scrolledFor = -1;  // the cursor track (and the tracks' sizes) the sideways scroll last brought into view
  int offLeft = 0, offRight = 0;  // tracks out of view with notes in the rows shown (last frame)
  int stacked = 0;  // song mode: other patterns playing at the position (shown on the Song page, counted in the bar)
};
State& st() {
  static State s;
  return s;
}

// The sub-columns shown (View > Columns; the note and instrument always are).
bool gShow[static_cast<int>(Field::Count)] = {true, true, true, true, true};
bool fieldShown(Field f) { return gShow[static_cast<int>(f)]; }
void loadColumns(App& a) {
  gShow[static_cast<int>(Field::Volume)] = a.settings.getBool("tracker.volumeColumn");
  gShow[static_cast<int>(Field::Pan)] = a.settings.getBool("tracker.panColumn");
  gShow[static_cast<int>(Field::Effect)] = a.settings.getBool("tracker.effectColumn");
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

View build(App& a) {
  View v;
  const Song& s = a.doc.song();
  v.p = s.pattern(a.curPattern);
  if (!v.p) return v;
  v.g = trk::gridOf(s, *v.p);
  for (const Channel& c : s.channels) {
    // An audio clip's source channel is no note track (its clips are on the Song page), unless
    // this pattern has notes for it.
    const bool audioSource = std::any_of(s.playlist.begin(), s.playlist.end(), [&](const Clip& k) { return k.kind == ClipKind::Audio && k.source == c.id; });
    if (audioSource && !(v.p->notes.count(c.id) && !v.p->notes.at(c.id).empty())) continue;
    const int extra = st().extraCols.count(c.id) ? st().extraCols.at(c.id) : 0;
    trk::Track t = trk::buildTrack(s, *v.p, c.id, v.g, 1);
    if (extra > 0) t = trk::buildTrack(s, *v.p, c.id, v.g, std::max(1, t.voices) + extra);
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

void releaseAll(App& a) {
  for (const auto& [sc, h] : st().held) a.engine.noteOff(h.channel, h.pitch);
  st().held.clear();
}

// ---- block operations ----
void copyBlock(const View& v, bool cut, App& a) {
  int t0, t1, r0, r1;
  const State& s = st();
  if (s.sel.on) blockRange(&t0, &t1, &r0, &r1);
  else t0 = t1 = s.cur.track, r0 = 0, r1 = v.g.rows - 1;  // no block: the whole track
  trk::Block b;
  b.rows = r1 - r0 + 1;
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t) {
    const Rows r = rowsOf(*v.p, v.tracks[static_cast<size_t>(t)].channel);
    b.tracks.push_back(trk::copyRows(r.notes, r.steps, r.props, v.g, r0, r1));
  }
  st().clip = b;
  if (cut) {
    a.doc.beginGroup("cut rows");
    for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
      applyEdit(a, v, t, "cut rows", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::cutRows(x.notes, x.steps, g, r0, r1); });
    a.doc.endGroup(false);
  }
  say(cut ? i18n::tr("Cut to the tracker clipboard") : i18n::tr("Copied to the tracker clipboard"));
}

void pasteBlock(const View& v, App& a) {
  const State& s = st();
  if (s.clip.empty()) {
    say(i18n::tr("Nothing copied yet: select rows with Shift and the arrows, then Ctrl+C"));
    return;
  }
  const trk::Block b = s.clip;
  const int row = s.cur.row;
  a.doc.beginGroup("paste rows");
  for (size_t i = 0; i < b.tracks.size(); ++i) {
    const int t = s.cur.track + static_cast<int>(i);
    if (t >= static_cast<int>(v.tracks.size())) break;
    applyEdit(a, v, t, "paste rows", [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::pasteRows(x.notes, x.steps, g, row, b.rows, b.tracks[i]); });
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

// A row inserted / deleted at the cursor's row: its track, or every track.
void rowEdit(App& a, const View& v, bool ins, bool everyTrack) {
  const int row = st().cur.row;
  const int lo = everyTrack ? 0 : st().cur.track, hi = everyTrack ? static_cast<int>(v.tracks.size()) - 1 : st().cur.track;
  a.doc.beginGroup(ins ? "insert row" : "delete row");
  for (int x = lo; x <= hi; ++x)
    applyEdit(a, v, x, ins ? "insert row" : "delete row", [&](Rows& r, const trk::Track&, const trk::Grid& g) {
      return ins ? trk::insertRow(r.notes, r.steps, r.props, g, row) : trk::deleteRow(r.notes, r.steps, r.props, g, row);
    });
  a.doc.endGroup(false);
}

// A note moved to another channel (the instrument typed): out of one, into the other, one undo step.
void moveToChannel(App& a, const View& v, int ti, int noteIndex, int target) {
  const Song& s = a.doc.song();
  if (target < 0 || target >= static_cast<int>(s.channels.size()) || target == ti) return;
  const ObjectId from = v.tracks[static_cast<size_t>(ti)].channel, to = s.channels[static_cast<size_t>(target)].id;
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
  say(std::string(i18n::tr("Moved to")) + " " + s.channels[static_cast<size_t>(target)].name);
}

// ---- the Tracker key set's pattern keys (core/Keymap Profile::Tracker; FT2-MAP section 9) ----
bool trackerKeySet() { return keys::keymap().profile() == keys::Profile::Tracker; }

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
  if (ctrl && alt) return false;
  if (sc >= SDL_SCANCODE_F9 && sc <= SDL_SCANCODE_F12 && !alt) return true;  // jump rows (Shift: store, Ctrl: play from); Alt+F12 closes the plugin windows
  const bool mod = shift || ctrl || alt;
  if ((sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F2 || sc == SDL_SCANCODE_F7 || sc == SDL_SCANCODE_F8) && mod) return true;  // transpose
  if ((sc == SDL_SCANCODE_F3 || sc == SDL_SCANCODE_F4 || sc == SDL_SCANCODE_F5) && (shift || ctrl)) return true;  // track / pattern clipboard
  if (sc == SDL_SCANCODE_GRAVE && !ctrl && !alt) return true;  // edit step
  if (sc == SDL_SCANCODE_CAPSLOCK) return true;                // note off
  if (alt && (trackJump(sc) >= 0 || sc == SDL_SCANCODE_C || sc == SDL_SCANCODE_DELETE || (sc >= SDL_SCANCODE_RIGHT && sc <= SDL_SCANCODE_UP)))
    return true;
  if (ctrl && (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_RETURN || (sc == SDL_SCANCODE_DELETE && !shift))) return true;  // Ctrl+Shift+Del deletes the pattern
  return false;
}

// One of those keys: 0 handled (no song change), 1 handled (the song may have changed), -1 not one.
int trackerKey(App& a, const View& v, const KeyEv& k) {
  if (!trackerKeySet()) return -1;
  State& s = st();
  Cursor& c = s.cur;
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, shift = (k.mods & KMOD_SHIFT) != 0, alt = (k.mods & KMOD_ALT) != 0;
  const int tracks = static_cast<int>(v.tracks.size());
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
    if (k.sc == SDL_SCANCODE_F5) {
      if (ctrl) c.track = 0;
      c.row = 0;
      if (s.edit) pasteBlock(v, a);
      c = keep;
      if (!s.edit) say(i18n::tr(noEdit));
      return 1;
    }
    s.sel = {true, ctrl ? 0 : c.track, 0, ctrl ? tracks - 1 : c.track, std::max(0, v.g.rows - 1)};
    const bool cut = k.sc == SDL_SCANCODE_F3;
    if (cut && !s.edit) say(i18n::tr(noEdit));
    else copyBlock(v, cut, a);
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
    if (k.sc == SDL_SCANCODE_LEFT || k.sc == SDL_SCANCODE_RIGHT) {  // the previous / next pattern
      const Song& song = a.doc.song();
      const int i = song.patternIndex(a.curPattern) + (k.sc == SDL_SCANCODE_LEFT ? -1 : 1);
      if (i >= 0 && i < static_cast<int>(song.patterns.size())) a.selectPattern(song.patterns[static_cast<size_t>(i)].id);
      return 1;
    }
    if (k.sc == SDL_SCANCODE_RETURN) {  // insert a row (Shift: in every track), as Insert
      if (!s.edit) return say(i18n::tr(noEdit)), 0;
      const int row = c.row, lo = shift ? 0 : c.track, hi = shift ? tracks - 1 : c.track;
      a.doc.beginGroup("insert row");
      for (int x = lo; x <= hi; ++x)
        applyEdit(a, v, x, "insert row", [&](Rows& r, const trk::Track&, const trk::Grid& g) { return trk::insertRow(r.notes, r.steps, r.props, g, row); });
      a.doc.endGroup(false);
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
  const Cell& cell = t.at(c.row, c.column);
  const Channel* ch = v.channels[static_cast<size_t>(ti)];
  const char* noEdit = "Edit mode is off: Esc turns it on (the keys only play)";
  switch (k.sc) {
    case SDL_SCANCODE_DELETE:
      if (!s.edit) return say(i18n::tr(noEdit)), false;
      if (s.sel.on) return clearBlock(v, a), true;
      applyEdit(a, v, ti, "clear", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::clearField(x.notes, x.steps, x.props, tr, g, c.row, c.column, c.field);
      });
      moveRow(v, s.step, true);
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
      if (k.repeat) return false;
      audition(a, k.sc, ch->id, pitch, a.typing.velocity);
      // Record pattern / Record song: the note lands on the row playing (edit mode or not).
      if (a.recordArmed && a.engine.playing() && s.playRow >= 0) {
        const int row = s.playRow;
        applyEdit(a, v, ti, "record note", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
          return trk::putNote(x.notes, tr, g, row, c.column, pitch, a.typing.velocity);
        });
        return true;
      }
      if (!s.edit) return false;
      // A song without pattern clips: the pattern typed in becomes its first order entry (as a new
      // classic song starts with entry 00), so Play song plays it. One undo step with the note.
      const bool first = !trk::hasPatternEntries(a.doc.song());
      if (first) a.doc.beginGroup("type note");
      applyEdit(a, v, ti, "type note", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) {
        return trk::putNote(x.notes, tr, g, c.row, c.column, pitch, a.typing.velocity);
      });
      if (first) {
        a.doc.addClip(a.curPattern, 0, 0);
        a.doc.endGroup(false);
      }
      moveRow(v, s.step, true);
      return true;
    }
    if (isOffKey(k.sc)) {
      if (!s.edit) return say(i18n::tr(noEdit)), false;
      applyEdit(a, v, ti, "note off", [&](Rows& x, const trk::Track& tr, const trk::Grid& g) { return trk::putOff(x.notes, tr, g, c.row, c.column); });
      moveRow(v, s.step, true);
      return true;
    }
    return false;
  }
  if (!s.edit) {
    if (hexDigit(k.kc) >= 0) say(i18n::tr(noEdit));
    return false;
  }
  auto advance = [&] {
    if (c.digit + 1 < trk::fieldDigits(c.field)) {
      ++c.digit;
      return;
    }
    c.digit = 0;
    moveRow(v, s.step, true);
  };
  const Note* note = cell.kind == Cell::On ? &notesRef(*v.p, t.channel)[static_cast<size_t>(cell.note)] : nullptr;
  if (c.field == Field::Effect) {
    const std::vector<trk::Effect> fx = note ? trk::effectsOf(*note) : std::vector<trk::Effect>{};
    const char shown = fx.empty() ? 0 : fx.front().letter;
    int value = fx.empty() ? 0 : fx.front().value;
    char letter = shown;
    if (c.digit == 0) {
      const int up = k.kc >= 'a' && k.kc <= 'z' ? k.kc - 'a' + 'A' : k.kc;
      if (!std::strchr(trk::kEffectLetters, up) || up == 0) return false;
      letter = static_cast<char>(up);
      if (letter != shown) {  // the letter's own value as the note has it
        const Note probe = note ? *note : Note{};
        value = letter == 'G' || letter == 'P' ? 1 : letter == 'X' ? probe.modX : letter == 'Y' ? probe.modY : letter == 'R' ? probe.release : probe.fine & 0xFF;
      }
    } else {
      const int nib = hexDigit(k.kc);
      if (nib < 0) return false;
      if (!shown) return say(i18n::tr("Type the effect letter first: G P T X Y R")), false;
      value = setNibble(value, c.digit - 1, nib);
    }
    const trk::Result r = applyEdit(a, v, ti, "effect", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setEffectAt(x.notes, tr, c.row, c.column, shown, letter, value);
    });
    if (r.refused.empty()) {
      say(trk::effectHelp(letter, value));
      advance();
    }
    return true;
  }
  const int nib = hexDigit(k.kc);
  if (nib < 0) return false;
  if (c.field == Field::Instrument) {
    if (cell.kind != Cell::On) return say(i18n::tr(cell.kind == Cell::Step ? "A rack step belongs to its channel" : "No note here")), false;
    const int now = ti + 1;
    const int want = setNibble(now, c.digit, nib);
    advance();
    if (want - 1 != ti) moveToChannel(a, v, ti, cell.note, want - 1);
    return true;
  }
  int now = 0;
  if (c.field == Field::Volume) {
    now = note ? note->velocity : cell.kind == Cell::Step ? stepsRef(*v.p, t.channel)[static_cast<size_t>(cell.step)] : 100;
    const int want = setNibble(now, c.digit, nib);
    const trk::Result r = applyEdit(a, v, ti, "volume", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setVolume(x.notes, x.steps, tr, c.row, c.column, std::min(want, 127));
    });
    if (r.refused.empty()) advance();
    return true;
  }
  if (c.field == Field::Pan) {
    const std::vector<StepProps>& pr = propsRef(*v.p, t.channel);
    now = note ? note->pan + 64 : cell.kind == Cell::Step && static_cast<size_t>(cell.step) < pr.size() ? pr[static_cast<size_t>(cell.step)].pan + 64 : 64;
    const int want = setNibble(now, c.digit, nib);
    const trk::Result r = applyEdit(a, v, ti, "pan", [&](Rows& x, const trk::Track& tr, const trk::Grid&) {
      return trk::setPan(x.notes, x.props, tr, c.row, c.column, std::min(want, 128));
    });
    if (r.refused.empty()) advance();
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
// Whether a centre page is the one shown in its dock node this frame.
static bool windowFrontNow(const char* window) {
  const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + window).c_str());
  return w && w->DockIsActive && w->DockNode && w->DockNode->VisibleWindow == w;
}

void keysBackToPattern() {
  if (windowFrontNow("Pattern editor")) st().wantFocus = true;
}

void setExtended(App& a, bool on) {
  if (gExtended == on) return;
  gExtended = on;
  layoutui::setRegionVisible(a, "tracker.header", !on);
  // The band holds the page strip: without it the pattern page stays in front (its bar has the
  // button that brings the band back). T-05: the region change docks the pages again a frame or
  // two later (the first page docked comes to the front), so the request waits for that.
  if (on || windowFrontNow("Pattern editor")) {
    showPage(a, "Pattern editor");
    st().pendingHold = 4;
  }
}

void transposeScope(App& a, TransposeScope scope, int semis) {
  if (scope == TransposeScope::Block) return transposeBlock(a, semis);
  const View v = build(a);
  if (!v.p || v.tracks.empty()) return;
  clampCursor(v);
  const int t0 = scope == TransposeScope::Track ? st().cur.track : 0;
  const int t1 = scope == TransposeScope::Track ? st().cur.track : static_cast<int>(v.tracks.size()) - 1;
  int refused = 0;
  a.doc.beginGroup("transpose");
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
    refused += applyEdit(a, v, t, "transpose", [&](Rows& x, const trk::Track&, const trk::Grid& g) {
                 return trk::transpose(x.notes, g, 0, std::max(g.rows - 1, 0) + 1024, semis);
               }).refusedCount;
  a.doc.endGroup(false);
  if (refused > 0) say(std::to_string(refused) + " " + i18n::tr("notes would leave the note range and stay where they are"));
}

void scaleBlock(App& a, bool expand) {
  const View v = build(a);
  if (!v.p || v.tracks.empty()) return;
  clampCursor(v);
  int t0, t1, r0, r1;
  if (st().sel.on) blockRange(&t0, &t1, &r0, &r1);
  else t0 = t1 = st().cur.track, r0 = 0, r1 = v.g.rows - 1;
  a.doc.beginGroup(expand ? "expand rows" : "shrink rows");
  for (int t = t0; t <= t1 && t < static_cast<int>(v.tracks.size()); ++t)
    applyEdit(a, v, t, expand ? "expand rows" : "shrink rows",
              [&](Rows& x, const trk::Track&, const trk::Grid& g) { return trk::scaleRows(x.notes, g, r0, r1, expand); });
  a.doc.endGroup(false);
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

void addColumn(App& a, int delta) {
  View v;
  if (!menuView(a, &v)) return;
  const trk::Track& t = v.tracks[static_cast<size_t>(st().cur.track)];
  int& extra = st().extraCols[t.channel];
  extra = std::clamp(extra + delta, 0, trk::kMaxColumns - std::max(1, t.voices));
  if (delta < 0) st().cur.column = std::min(st().cur.column, std::max(1, t.voices) + extra - 1);
}
bool follow() { return st().follow; }
void setFollow(bool on) { st().follow = on; }
int cursorField() { return static_cast<int>(st().cur.field); }

bool sdlKey(App& a, int scancode, int keycode, int mods, bool down, bool repeat) {
  State& s = st();
  // Caps Lock comes as a key down when it locks and a key up when it unlocks (macOS): each is a press.
  if (scancode == SDL_SCANCODE_CAPSLOCK && !down && trackerKeySet() && hasKeyboard()) down = true, repeat = false;
  if (down && hasKeyboard() && trackerTakes(scancode, mods)) {
    s.keys.push_back({scancode, keycode, mods, repeat});
    return true;
  }
  if (!down) {
    if (const auto it = s.held.find(scancode); it != s.held.end()) {
      a.engine.noteOff(it->second.channel, it->second.pitch);
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
    if (a.selChannel != kNoId) a.settingsChannel = a.selChannel;
    return;
  }
  if (std::strcmp(window, "Loupe") == 0) {  // T-11: the pinned panel docks with the centre's pages: it comes to the front
    a.settings.set("ui.loupePinned", true);
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

void bringPage(const char* window) {
  st().pendingPage = window;
  st().pendingFrames = st().pendingWait = st().pendingHold = 0;
}

void pumpPage(App& a) {
  State& s = st();
  // Once a frame, from whichever Tracker part is drawn (T-05: with the header band hidden the
  // order list that used to pump it is not drawn, so the pages and the editor pump it too).
  static int pumped = -1;
  if (pumped == ImGui::GetFrameCount()) return;
  pumped = ImGui::GetFrameCount();
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
    if (w && s.pendingFrames <= 60) ImGui::FocusWindow(w);
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
  State& s = st();
  const auto& P = pal();
  ImGuiIO& io = ImGui::GetIO();
  const bool hadKb = s.hasKb;
  s.drawnFrame = ImGui::GetFrameCount();
  pumpPage(a);
  loadColumns(a);
  s.hasKb = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
  if (hadKb && !s.hasKb) releaseAll(a);  // the keyboard went elsewhere: no note keeps sounding
  if (s.wantFocus) {
    ImGui::SetWindowFocus("###Pattern editor");  // the part window ("Label###English"; focusing the child would pick no dock tab)
    s.wantFocus = false;
  }
  View v = build(a);
  if (s.pattern != a.curPattern) s.pattern = a.curPattern, s.sel.on = false;
  if (!v.p || v.tracks.empty()) {  // no pattern, or no channel to give the pattern a track
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    // A song of audio clips only has no pattern: the Song page shows its clips; a pattern made here
    // starts the rows (one undo step, as Pattern > New pattern).
    const bool audioOnly = !v.p && std::any_of(a.doc.song().playlist.begin(), a.doc.song().playlist.end(), [](const Clip& c) { return c.kind != ClipKind::Pattern; });
    // The message in the space above the button row; the button under its plate (never on it).
    const float fs = ImGui::GetFontSize();
    const float bw = std::round(fs * 9), bh = std::round(fs + 10), gapB = 12;
    const float fullH = std::max(av.y, 90.0f), msgH = !v.p ? fullH - (bh + gapB) * 2 : fullH;
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + msgH), !v.p ? "No pattern" : "No channels",
                   !v.p ? (audioOnly ? "Audio and automation clips only (the Song page shows them). A new pattern starts rows of notes"
                                     : "Make a pattern to write notes in rows")
                        : "Add a track (the header's Add track) to write notes in rows");
    if (!v.p) {
      // (the plate's height as ui::EmptyState lays it out: the mark when there is room, the title, the line)
      const bool mark = msgH >= 190 && av.x >= 160;
      const float total = (mark ? 80 + theme::kS4 : 0) + theme::kFontBody + 2 + theme::kS1 + theme::kFontCaption + 2;
      const ImVec2 b0(std::round(p0.x + (av.x - bw) * 0.5f), std::round(p0.y + (msgH + total) * 0.5f + theme::kS2 + gapB));
      if (trackerview::bevelButton(a, "##newpat", "New pattern", b0, ImVec2(b0.x + bw, b0.y + bh), false, "A new, empty pattern to write in")) {
        // The new pattern goes in the song too when it has no pattern clips (entry 00).
        const bool first = !trk::hasPatternEntries(a.doc.song());
        a.doc.beginGroup("new pattern");
        const ObjectId id = a.doc.addPattern(ops::nextPatternName(a.doc.song()));
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
    if (tick >= 0 && v.g.rowTicks > 0) s.playRow = static_cast<int>((tick % std::max<Tick>(1, v.g.length())) / v.g.rowTicks);
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
    m.rowH = std::round(pf * 1.12f + 2);
    return m;
  };
  auto tracksW = [&](const Metrics& m, size_t n) {
    float w = 0;
    for (size_t i = 0; i < n && i < v.tracks.size(); ++i) w += m.pad * 2 + m.colW * static_cast<float>(v.tracks[i].columns) + 1;
    return w;
  };
  const float areaW = a1.x - a0.x;
  size_t shown = v.tracks.size();
  {
    const Metrics m0 = metricsAt(fs);
    while (shown > 1 && tracksW(m0, shown) + m0.numW * 2 > areaW) --shown;
  }
  const float minRows = 27;  // T-20: the classic view's 27 rows (13 each side of the edit row) before the glyphs grow
  const float pfMax = std::max(fs, std::min(std::floor(fs * 2.2f), std::floor(((a1.y - a0.y - headH) / minRows - 2) / 1.12f)));
  float pf = fs;
  for (float t = pfMax; t > fs; t -= 1) {
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
  const float extra = std::max(0.0f, std::floor((viewW - tracksW(M, shown)) / static_cast<float>(std::max<size_t>(1, shown))));
  std::vector<float> trackX, trackW, inset;  // each track's left edge (from the area's left, before scroll), width, cell inset
  float x = 0;
  for (const trk::Track& t : v.tracks) {
    trackX.push_back(x);
    const float w = M.pad * 2 + colW * static_cast<float>(t.columns) + extra;
    trackW.push_back(w);
    inset.push_back(std::floor(M.pad + extra * 0.5f));
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
  // T-19: a scrollbar under the tracks when they do not all fit.
  const bool hbar = lastFirst > 0;
  const float sbH = hbar ? std::max(9.0f, std::round(fs * 0.75f)) : 0.0f;
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
  const float wheelX = io.MouseWheelH != 0 ? io.MouseWheelH : io.KeyShift ? io.MouseWheel : 0.0f;  // (> 0: to the left)
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
  if (wheelHov && io.MouseWheel != 0 && !io.KeyShift && !(s.follow && s.playRow >= 0)) {
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
  {  // before the first row and after the last: shaded
    const float yTop = body0 + static_cast<float>(-firstRow) * rowH, yEnd = body0 + static_cast<float>(v.g.rows - firstRow) * rowH;
    const ImU32 shade = theme::mix(P.well, P.surface950, 0.75f);
    if (yTop > body0) band(body0, std::min(yTop, body1), a0.x, a1.x, shade);
    if (yEnd < body1) band(std::max(yEnd, body0), body1, a0.x, a1.x, shade);
  }
  for (int i = 0; i < visRows; ++i) {
    const int row = firstRow + i;
    if (row < 0 || row >= v.g.rows) continue;
    const float y = body0 + static_cast<float>(i) * rowH;
    if (row % v.g.rowsPerBar == 0) band(y, y + rowH, a0.x, a1.x, theme::withAlpha(P.gridBar, 0.55f));
    else if (row % v.g.rowsPerBeat == 0) band(y, y + rowH, a0.x, a1.x, theme::withAlpha(P.gridBeat, 0.45f));
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
    const ImU32 col = row % v.g.rowsPerBeat == 0 ? P.text : P.textFaint;
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
  dl->PushClipRect(ImVec2(gx0, a0.y), ImVec2(gx1, a1.y), true);
  const Song& sg = a.doc.song();
  const ops::ChannelSolo& solo = soloMemory();
  int hoverTrackHead = -1;
  for (size_t ti = 0; ti < v.tracks.size(); ++ti) {
    const trk::Track& t = v.tracks[ti];
    const Channel* ch = v.channels[ti];
    const float tx = gx0 + trackX[ti] - scrollX;
    const float tw = trackW[ti];
    if (tx > gx1 || tx + tw < gx0) continue;
    // Separator.
    const pixelgrid::Span sep = pixelgrid::line(tx + tw, dpp);
    dl->AddRectFilled(ImVec2(sep.x0, a0.y), ImVec2(sep.x1, a1.y), P.line);
    // Header: colour bar, number and name, the mute lamp, + and - columns.
    const bool muted = ch->muted, soloed = ops::channelSoloed(sg, ch->id, solo);
    const ImVec2 h0(tx, a0.y), h1(tx + tw, a0.y + headH - 1);
    trackerview::bevelBox(dl, h0, h1, false);  // a raised header, as the band above
    dl->AddRectFilled(h0, ImVec2(h1.x, h0.y + 3), theme::rgb(ch->color, muted ? 0.35f : 1.0f));
    const std::string name = std::to_string(ti + 1) + " " + ch->name;
    const float lampW = std::round(fs * 0.7f);
    const float btnW = std::round(cwH * 1.6f);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(h0.x + 6, std::round(h0.y + 3 + (headH - 4 - fs) * 0.5f)), tw - 12 - lampW - btnW * 2 - 8,
                        muted ? P.textFaint : static_cast<int>(ti) == s.cur.track ? P.text : P.textDim, name.c_str());
    const ImVec2 l0(h1.x - 6 - lampW, std::round(h0.y + 3 + (headH - 4 - lampW) * 0.5f)), l1(l0.x + lampW, l0.y + lampW);
    dl->AddRectFilled(l0, l1, soloed ? P.solo : muted ? P.surface600 : P.led);
    dl->AddRect(l0, l1, P.line);
    ImGui::PushID(static_cast<int>(ti));
    ImGui::SetCursorScreenPos(ImVec2(l0.x - 2, l0.y - 2));
    if (ImGui::InvisibleButton("##mute", ImVec2(lampW + 4, lampW + 4))) a.doc.setParam(channelParam(ch->id, ChannelParam::Mute), muted ? 0 : 1, false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ops::toggleChannelSolo(a.doc, ch->id, soloMemory());
    if (ImGui::IsItemHovered()) hoverTrackHead = static_cast<int>(ti);
    ui::Hint(soloed ? "Soloed: every other channel is muted \xC2\xB7 right-click: back to normal \xC2\xB7 click: mute"
             : muted ? "Muted \xC2\xB7 click: unmute \xC2\xB7 right-click: solo this channel"
                     : "Playing \xC2\xB7 click: mute \xC2\xB7 right-click: solo this channel");
    // + and - note columns.
    for (int b = 0; b < 2; ++b) {
      const ImVec2 b0(l0.x - 4 - btnW * static_cast<float>(2 - b), h0.y + 3), b1(b0.x + btnW, h1.y);
      ImGui::SetCursorScreenPos(b0);
      ImGui::PushID(b);
      const bool click = ImGui::InvisibleButton("##cols", ImVec2(btnW, std::max(1.0f, b1.y - b0.y)));
      const bool bh = ImGui::IsItemHovered();
      ImGui::PopID();
      theme::centreText(dl, mono, fs, b0, b1, bh ? P.text : P.textFaint, b == 0 ? "-" : "+");
      ui::Hint(b == 0 ? "One note column fewer (only empty columns go)" : "One more note column (for chords)");
      if (click) {
        int& extra = st().extraCols[ch->id];
        extra = std::clamp(extra + (b == 0 ? -1 : 1), 0, trk::kMaxColumns - std::max(1, t.voices));
      }
    }
    ImGui::PopID();
    // A track shown from another pattern playing here: a wash in that pattern's colour.
    if (v.src[ti] != v.p) dl->AddRectFilled(ImVec2(tx, body0), ImVec2(tx + tw, body1), theme::withAlpha(theme::rgb(v.src[ti]->color), 0.08f));
    // Cells.
    const std::vector<Note>& tNotes = notesRef(*v.src[ti], t.channel);
    const std::vector<uint8_t>& tSteps = stepsRef(*v.src[ti], t.channel);
    const std::vector<StepProps>& tProps = propsRef(*v.src[ti], t.channel);
    for (int i = 0; i < visRows; ++i) {
      const int row = firstRow + i;
      if (row < 0 || row >= v.g.rows) continue;
      const float y = body0 + static_cast<float>(i) * rowH + std::round((rowH - pf) * 0.5f);
      for (int c = 0; c < t.columns; ++c) {
        const float cx = std::round(tx + inset[ti] + colW * static_cast<float>(c) + 3);
        const Cell& ce = t.at(row + v.rowOff[ti], c);
        const Note* n = ce.kind == Cell::On || ce.kind == Cell::Off ? &tNotes[static_cast<size_t>(ce.note)] : nullptr;
        const bool dimmed = muted || (n && n->muted);
        auto put = [&](Field f, const std::string& text, ImU32 col) {
          if (!fieldShown(f)) return;
          dl->AddText(mono, pf, ImVec2(cx + cw * static_cast<float>(fieldOffset(f)), y), dimmed ? theme::withAlpha(col, 0.45f) : col, text.c_str());
        };
        const ImU32 faint = theme::withAlpha(P.textFaint, 0.55f);
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
            put(Field::Note, trk::noteText(n->pitch), fieldColour(Field::Note));
            put(Field::Instrument, trk::hex2(static_cast<int>(ti) + 1), fieldColour(Field::Instrument));
            put(Field::Volume, trk::hex2(n->velocity), fieldColour(Field::Volume));
            put(Field::Pan, n->pan == 0 ? std::string("\xC2\xB7\xC2\xB7") : trk::hex2(n->pan + 64), n->pan == 0 ? faint : fieldColour(Field::Pan));
            const std::vector<trk::Effect> fx = trk::effectsOf(*n);
            if (fx.empty()) put(Field::Effect, "\xC2\xB7\xC2\xB7\xC2\xB7", faint);
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
            put(Field::Note, trk::noteText(pitch), theme::mix(fieldColour(Field::Note), P.led, 0.35f));
            put(Field::Instrument, trk::hex2(static_cast<int>(ti) + 1), fieldColour(Field::Instrument));
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
      const bool left = tx + trackW[ti] <= gx0 + 1, right = tx >= gx1 - 1;
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
    if (extended()) {  // the header is hidden: a button brings it back
      const char* lbl = i18n::tr("Header band");
      const float w = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, lbl).x) + 20;
      if (trackerview::bevelButton(a, "##showhdr", "Header band", ImVec2(b1.x - w, b0.y), b1, false, "The header band back")) setExtended(a, false);
      b1.x -= w + 2;
    }
    trackerview::bevelBox(dl, b0, b1);
    const Pattern& p = *v.p;
    char info[200];
    std::snprintf(info, sizeof info, "%s %s \xC2\xB7 %s %s \xC2\xB7 %s %d \xC2\xB7 %s %d \xC2\xB7 %s %d \xC2\xB7 %s %s", i18n::tr("Pattern"),
                  trk::hex2(trk::patternNumber(song, p.id)).c_str(), i18n::tr("length"), trk::rowText(v.g.rows, hex).c_str(), i18n::tr("rows per beat"),
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
        std::snprintf(b, sizeof b, "%s \xC2\xB7 %s %lld \xC2\xB7 %s %lld %s", trk::noteText(n.pitch).c_str(), i18n::tr("starts at tick"),
                      static_cast<long long>(n.start), i18n::tr("lasts"), static_cast<long long>(n.length), i18n::tr("ticks"));
        right = b;
      }
    } else if (ce.kind == Cell::Step) {
      right = i18n::tr("A rack step: Delete clears it, the volume sets its velocity; the rest is edited in Steps");
    } else if (s.cur.field == Field::Effect) {
      right = trk::effectHelp(0, 0);
    }
    if (s.stacked > 0 && (ImGui::GetTime() - s.statusT >= 4.0 || s.status.empty()))
      right = std::to_string(s.stacked) + " " + i18n::tr(s.stacked == 1 ? "more pattern plays here (the Song page shows it)" : "more patterns play here (the Song page shows them)");
    const float ty = std::round(b0.y + (barH - 2 - fs) * 0.5f);
    dl->AddText(mono, fs, ImVec2(b0.x + 8, ty), P.textDim, info);
    const float iw = mono->CalcTextSizeA(fs, FLT_MAX, 0, info).x;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(b0.x + 8 + iw + 24, ty), b1.x - (b0.x + 8 + iw + 24) - 120, P.text, right.c_str());
    const char* mode = s.edit ? i18n::tr("Edit mode") : i18n::tr("Play keys");
    const float mw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, mode).x;
    dl->AddText(a.fontRegular, fs, ImVec2(b1.x - 8 - mw, ty), s.edit ? P.arm : P.textFaint, mode);
  }
  ImGui::SetCursorScreenPos(ImVec2(a0.x, a1.y + barH));
  ImGui::Dummy(ImVec2(1, 0));
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
  } else if (cmd == "page" && args.size() > 1) {
    std::string w = args[1];
    for (size_t i = 2; i < args.size(); ++i) w += " " + args[i];
    // The page strip's names too (Song is the Playlist window, Mix the Mixer, ...).
    static const std::pair<const char*, const char*> names[] = {{"Pattern", "Pattern editor"}, {"Song", "Playlist"},  {"Roll", "Piano roll"},
                                                                {"Steps", "Channel rack"},     {"Mix", "Mixer"},       {"Pads", "Live pads"},
                                                                {"Sample", "Loupe"}};
    for (const auto& [label, window] : names)
      if (w == label) w = window;
    showPage(a, w.c_str());
    std::printf("tracker page frame %d: %s\n", frame, w.c_str());
  } else if (cmd == "swap") {
    const std::string w = args.size() > 1 ? args[1] : "none";
    setHeaderSwap(w == "help" ? 1 : w == "files" ? 2 : w == "config" ? 3 : 0);
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
    for (const char* pg : {"Pattern editor", "Playlist", "Channel rack", "Mixer", "Live pads", "Piano roll", "Browser", "Device chain"}) {
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
    std::printf("tracker dump frame %d: pattern %d rows %d edit %d step %d octave %d keyboard %d cursor %d %d %d %d play %d undo %zu engine %d %u %u drawn %d off %d %d scroll %.0f stacked %d\n", frame,
                v.p ? trk::patternNumber(a.doc.song(), v.p->id) : 0, v.p ? v.g.rows : 0, s.edit ? 1 : 0, s.step, a.typing.octave, hasKeyboard() ? 1 : 0,
                s.cur.track, s.cur.row, s.cur.column, static_cast<int>(s.cur.field), s.playRow, a.doc.history().position(), a.engine.playing() ? static_cast<int>(a.engine.mode()) : -1, a.engine.pattern(), a.curPattern, s.drawnFrame, s.offLeft, s.offRight, static_cast<double>(s.scrollX), s.stacked);
    for (size_t ti = 0; v.p && ti < v.tracks.size(); ++ti) {
      const trk::Track& t = v.tracks[ti];
      const Rows r = rowsOf(*v.p, t.channel);
      for (int row = 0; row < v.g.rows; ++row)
        for (int c = 0; c < t.columns; ++c) {
          const Cell& ce = t.at(row, c);
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
