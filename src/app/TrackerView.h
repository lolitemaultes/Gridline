// Gridline - the tracker pattern editor (K-106, the Pattern editor part) and the Tracker layout's
// header band (the Order list part): rows of notes per rack channel, typed on the computer keyboard
// in the classic tracker way.
//
// The editor shows App::curPattern, the pattern the piano roll and the channel rack show, through
// core/TrackerGrid.h: one row per step, one track per rack channel (its overlapping notes in note
// columns), cells with note, instrument, volume, pan and an effect letter. Every edit is a Document
// command (setNotes, the steps through editPattern; several channels inside one group), so undo,
// saving and collaboration treat it as a piano roll edit and every layout sees the same notes.
// Cursor, edit mode, edit step and selection are this person's view state, never saved or sent.
//
// Keys: while the editor has the keyboard (its window focused, no text field), main.cpp hands it the
// SDL key events first (sdlKey). Note keys go by where the key sits (the typing keyboard's two rows,
// any keyboard layout), values by the character typed. The chords it uses run no other command
// (keys::setEditorKeys); Space, the function keys (F1 to F8 pick the octave in the Tracker key set)
// and the other shortcuts keep working.
#pragma once
#include <string>
#include <vector>

#include "../core/Types.h"

namespace gl {
class App;
}
namespace gl::trk {
struct Masks;
}

namespace gl::trackerview {

// The Pattern editor part's body.
void patternEditor(App& a);
// The Order list part's body: in the Tracker layout the classic header band (order list, song box,
// scopes, buttons, instrument and sample lists, page strip); elsewhere the order list alone.
void orderList(App& a);

// main.cpp, for every SDL key event: true when the pattern editor takes it (the typing keyboard and
// the other handlers then leave it). Key-ups end the notes a key auditioned and are never taken.
bool sdlKey(App& a, int scancode, int keycode, int mods, bool down, bool repeat);
// R4-01, R4-04: the song position (the order list's, Ins, Del and Enter act there): the engine's in
// song mode, kept apart while the engine is in pattern mode (a pattern play counts its own rows).
// setSongPosition moves it (the engine too, outside pattern mode); songMode switches the engine to
// song mode at it.
Tick songPosition(App& a);
void setSongPosition(App& a, Tick t);
void songMode(App& a);
// R4-09: the tracks multichannel edit writes to, by channel (view state kept per project, never
// saved; every track is on until a scope's right-click turns it off).
bool multiTrackOn(App& a, ObjectId channel);
void toggleMultiTrack(App& a, ObjectId channel);
// A message in the pattern editor's status line (where its own keys answer).
void status(const std::string& text);
// cp16: the message said in the last four seconds (nullptr: none), for the pages' title lines.
const std::string* freshStatus();
// The number the first track shows: 0 as the classic tracker counts channels, or 1 (Config).
int trackBase(const App& a);
// The editor had the keyboard on the last frame.
bool hasKeyboard();
// A header button was clicked: the keyboard goes back to the pattern editor when its page is the
// one shown (the classic tracker's buttons never take the keys from the pattern).
void keysBackToPattern();
// R11-04: a button that opens a popup in the band keeps the keys there (focusing the pattern would
// close the popup it just opened): cancels this frame's keysBackToPattern.
void keepKeysHere();
// R3-06: the cursor to the channel's track, the keys back to the pattern (an instrument picked).
void cursorToChannel(App& a, ObjectId channel);
// The editor was drawn on the last frame (it is the pattern page: PAT does not bring the rack forward over it).
bool shown();

// View state shared by the editor and the header band.
int editStep();
void setEditStep(int n);
bool editMode();
void setEditMode(bool on);
// Block tools on the selection (or the cursor's row on its track): transpose by semitones, and
// expand / shrink (rows spread twice as far apart, or every other row kept closer).
void transposeBlock(App& a, int semis);
void scaleBlock(App& a, bool expand);
// Transpose by scope: the cursor's track, every track of the pattern, every pattern of the song, or
// the block. `current`: only the current instrument's notes (the selected channel's track), as the
// classic Transpose box's "current instrument" half; otherwise every instrument's.
enum class TransposeScope { Track, Pattern, Song, Block };
void transposeScope(App& a, TransposeScope scope, int semis, bool current = false);
// The Advanced edit box: the old instrument's notes move to the new one's track, in the scope
// (Track: the cursor's track, which must be the old instrument's; Block: the marked rows).
void remapInstrument(App& a, TransposeScope scope, ObjectId from, ObjectId to);
// Its clipboard masks (view state, never saved): which parts a copy takes, a paste writes, and
// which parts paste transparently; on or off as a whole.
trk::Masks& masks();
bool masking();
void setMasking(bool on);
// Files shown in the band: Backspace goes to the folder above (true: done there).
bool diskParent(App& a);
// The Edit and Pattern menus' pattern commands (the same as the editor's keys; a menu item runs
// whether edit mode is on or not). Each edit is one undo step through the Document.
enum class BlockOp { Cut, Copy, Paste, Clear, SelectAll, Deselect };
void blockOp(App& a, BlockOp op);
bool hasBlock();           // a block is marked
bool hasClip();            // the tracker clipboard holds rows
// A row inserted / deleted at the cursor: the cursor's track, or every track.
void insertRow(App& a, bool everyTrack);
void deleteRow(App& a, bool everyTrack);
// Volume, pan or effect values on a straight line through the block (the field under the cursor;
// on the note field, the volume). The cursor's track from row to the pattern end with no block.
void interpolate(App& a);
// Note columns of the cursor's track: one more / one fewer (empty ones only; view only).
void addColumn(App& a, int delta);
bool follow();
void setFollow(bool on);
// The editor's cursor field: true on an effect or value field (the menu words Interpolate by it).
int cursorField();

// cp17 (user): the pattern's sideways zoom, a view preference in the settings (tracker.patternZoom,
// tracker.patternFit; never in the song). d > 0: wider tracks (fewer in view); d < 0: narrower
// (sub-columns hidden, then smaller glyphs; their data stays); 0: every track in view (Fit, kept
// as the window changes); 99: back to the plain view. patternZoom is the step drawn (0..6).
void zoomPattern(App& a, int d);
int patternZoom();
bool patternFit();
// The zoom keys (Ctrl+Alt+. , and /): the Song page's zoom when it is in front, else the pattern's.
void zoomFront(App& a, int d);
// The header band hidden (the pattern gets the room) and back.
void setExtended(App& a, bool on);
bool extended();
// The pattern's row height as last drawn (0 before the first frame).
float patternRowHeight();
// The centre page shown in the Tracker layout (a window homed in tracker.pages, or the instrument
// page, which opens the selected channel's instrument).
void showPage(App& a, const char* window);
// Brings a centre page's hidden tab to the front (over the next frames, until it shows). The
// centre has no tab bar, so focus alone does not pick the tab; the header band pumps this.
// K-297 (round 8): takeKeys false brings it without the keys (a layout rebuild: the window that
// opened in another region keeps them).
void bringPage(const char* window, bool takeKeys = true);
void pumpPage(App& a);
// K-297 (round 8): the page a restart brings back in front (the one in front at quit); the
// Tracker's first frame on screen shows it instead of the pattern page. takeStartPage() hands it
// over once ("" when none).
void setStartPage(const std::string& window);
std::string takeStartPage();
// What the header shows: 0 its boxes, 1 Help, 2 Files (the header-left swap).
void setHeaderSwap(int which);
// Ctrl+Alt+8, as the Audio editor button: the audio editor in front, or (in front already) the pattern.
void toggleAudioPage(App& a);
// K-297: Esc (when nothing else took it) or Cmd+W (Ctrl+W) on a page docked in the centre, other
// than the pattern: a page of the page strip does what its Exit does (the pattern in front); any
// other window docked there (Settings, the browser) closes, and the pattern comes to the front.
// True when it acted (the key is used).
bool closeFrontPageKey(App& a);
// K-297 on macOS: Cmd+W is the window menu's Close, which reaches Gridline as the main window's
// close request (with Cmd held). When a Tracker page other than the pattern is in front, the
// request closes that page on the next frame instead (true); otherwise it is the window's (false).
bool closePageOnCloseRequest(App& a);
// R8-02: the previous (-1) or next (+1) instrument, as Ctrl+Shift+Up / Down in the pattern, from
// anywhere in the Tracker (the instrument editor open and focused too); the keys go back to the pattern.
void stepInstrument(App& a, int d);
// The previous / next pattern (Ctrl+Left / Right, the pattern box's arrows); past the last one, a
// new empty pattern when the last holds something (R10-03).
void stepPattern(App& a, int d);
// R10-04: the previous / next song position (Ctrl+Shift+Left / Right, the order list's arrows): its
// pattern shows, as a click on the entry.
void stepSongPosition(App& a, int d);
// The scope box's tool (0 the scopes, 1 Transpose, 2 Clean up, 3 Advanced edit, 4 MIDI, 5 Sample tools): shown, or (shown
// already) back to the scopes. The keys Ctrl+Alt+T, R and A.
void toggleScopeTool(App& a, int which);
int scopeTool();
// N-05: back to the main screen (Ctrl+Alt+X, as the classic Ctrl+X): the header's swaps and scope
// tools close, the band comes back from Extend, the pattern page comes to the front.
void mainScreen(App& a);
int headerSwap();
// The settings screen's page (0 Layout, 1 Miscellaneous, 2 Audio, 3 MIDI input).
void setConfigPage(int page);
// The Files page's Samples open (a double-click on an audio file): into the current instrument.
void filesLoadSample(App& a, const std::string& path);

// cp16 (in-app CPU timers, no profiler): the wall time of the Tracker's own drawing per frame, by
// part: the header band, the pattern editor, the other pages (Song, Steps, Mix, Pads, Roll's row).
// "tracker perf" prints each part's mean and 95th percentile over the last 600 frames that drew it.
enum PerfPart { PerfBand, PerfPattern, PerfPage, PerfEditor, kPerfParts };  // (cp18: PerfEditor, the lower band's Tracker screens)
class PerfScope {
 public:
  explicit PerfScope(PerfPart p);
  ~PerfScope();
  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  PerfPart p_;
  unsigned long long t0_;
};

// Dev (--dev-input "tracker ..."):
//   tracker dump                 the shown pattern: one line per non-empty cell, the cursor and the play row
//   tracker focus                gives the editor the keyboard
//   tracker cursor <track> <row> [column] [field]
//   tracker edit on|off · tracker step <n> · tracker follow on|off · tracker select <t0> <r0> <t1> <r1>
//   tracker geometry             the editor's rectangle, the row height and the cursor cell's rectangle
//   tracker page <window>        a centre page to the front ("Pattern editor", "Playlist", "Channel rack", "Mixer", "Live pads", "Piano roll")
//   tracker swap none|help|files the header's swap
//   tracker extend on|off        the band as a strip (the classic Extend) or back
//   tracker songclip <index>     the Song page picks the playlist's clip at that index
//   tracker busiest              the song position to the pattern with the most tracks in use (photos)
//   tracker mididevs <n>         n stand-in MIDI input devices listed (nothing is opened)
//   tracker loadsample <path>    the Files page loading an audio file into the current instrument
//   tracker tool <n>             the scope box's tool (0 scopes .. 5 sample tools); swap config <page>: a settings page
void devCommand(App& a, const std::vector<std::string>& args, int frame);

// K-106: a menu label in the Tracker's own words (the shared menus say playlist, piano roll and
// channel rack; the Tracker's pages are Song, Roll and Steps). Returns `label` when nothing changes;
// otherwise a string kept for the program's life. The part after "##" (the item's id) is kept.
const char* menuWords(const char* label);

}  // namespace gl::trackerview
