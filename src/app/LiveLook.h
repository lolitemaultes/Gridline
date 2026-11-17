// Gridline - the Live layout's own chrome (K-112 phase C): its control bar pieces, status line,
// the arrangement's track headers on the right, the device band and the info corner.
//
// Everything here draws the same data and runs the same commands as Studio: a header's activator
// is the track's mute, S is its solo, the arm dot is the linked mixer track's record arm, the device
// band's knobs are the plugins' own parameters (undo, automation and controller links as anywhere
// else). Nothing here writes a project field of its own, so a collaborator in any other layout sees
// every edit as their own view shows it. Colours come from the theme roles and the person's track
// and channel colours only.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {
class App;
struct Song;
struct Clip;
}

namespace gl::livelook {

// The Live layout is on screen, built from its region shell (the K-79 arrangement keeps Studio's chrome).
bool active(const App& a);
// K-112c: the face numbers are set in (rulers, readouts): the sans face in Live, as the refs set
// them; the monospaced one otherwise.
ImFont* numFont(const App& a);
// K-112c: the envelope line drawn over the notes (and the curve glyph of its chooser).
ImU32 envelopeColour();
// K-112c: the control bar's Capture, Session record and Draw mode. captureNow: the notes kept by
// App::capture into the current pattern (how many, with a notification); in Clips (checkpoint 48)
// into the clip the selected MIDI track plays, else a new clip in its slot that then plays. Session record: the
// record button in pattern mode, playback started; sessionRecording: it is armed in pattern mode.
// Draw mode: the arrangement's and the notes editor's tool, the pencil or select.
int captureNow(App& a);
bool sessionRecording(const App& a);
void toggleSessionRecord(App& a);
// Checkpoint 48 (Clips): Session record as the reference's, with the other clips playing on. Started
// from the control bar (the selected MIDI track: into the clip it plays, else a new clip in its
// selected or first empty slot) or an empty slot's record button (that slot). A new clip is launched
// and grows by bars while it records (up to a scene); a stop keeps its whole bars and it plays on.
// sessionRecordTarget: while the clip plays, what NoteRecorder records into (App::processMidi): its
// pattern, its length and the transport tick of its content's start. sessionRecordFrame: per Clips
// frame (launch, start, growth, the end when the transport stops or the track plays another clip).
void startSessionRecord(App& a, int track, int slot);
void stopSessionRecord(App& a);
ObjectId sessionRecordClip(const App& a);
bool sessionRecordTarget(const App& a, ObjectId& pattern, Tick& loopLength, double& origin);
void sessionRecordFrame(App& a);
// N47-01: Session record of an armed audio track (its input records while the clips play on), and the
// takes written when it stops: placed in the grid slot (true: they were Clips' and were handled).
bool sessionRecordAudio(const App& a);
bool audioTakesCommitted(App& a, const std::vector<ObjectId>& clips);
// Checkpoint 49: a Clips Session take is waiting to be written (its writing and its move into the slot
// are then one undo step).
bool audioTakePending(const App& a);
// N47-10: a track records when armed: an audio track by its mixer track's arm, a MIDI track by its arm
// button (this session) or by being the selected track (as the reference arms it by itself).
bool trackArmed(const App& a, int track);
bool midiArmedByHand(int track);
void toggleMidiArm(App& a, int track, bool exclusive);
// N47-06: the notes editor's play position for `pattern` while a launched grid clip plays it (the
// selected clip first): its tick in the pattern. False when no grid clip of it plays.
bool clipPlayhead(const App& a, ObjectId pattern, double& tick);
// N47-03 / N47-15: an instrument dropped in Clips: its track takes its name (when still the default),
// is selected, and the device band shows it (no editor window for built-in devices). False outside Clips.
bool instrumentDropped(App& a, ObjectId channel, const std::string& name);
bool drawMode(const App& a);
// N48-11: the Arrangement record switch (F9, the record button, the menu row). In Clips turning it on
// starts the transport, and the clip grid's launches are recorded into the arrangement.
void toggleArrangementRecord(App& a);
// N49-09: Main's title (grid, mixer band): Main selected, its chain shown.
void selectMain(App& a);
void setDrawMode(App& a, bool on);

// K-112c (C44-20, the user's "Hide it"): the live area stays where every layout stores it (the start
// of the playlist), but in Clips it is the clip grid's storage only: the arrangement view, its song
// length, its ruler numbers and song playback all begin where the live area ends, so the grid and the
// arrangement behave as two separate sets of clips. 0 outside Clips (or with no live area).
Tick arrangementOrigin(const App& a);
// Checkpoint 55 (gap 5): an Arrangement row's height in Clips, as a scale of the row height: the one the
// person dragged it to (kept per person and set, by the track's mixer track or instrument, so it follows
// a move), else the track's own (`fallback`). setRowScale: a drag (`done`: its end, which remembers it).
// Never written into the song.
float rowScale(const App& a, int track, float fallback);
void setRowScale(App& a, int track, float scale, bool done);
// Checkpoint 54 (N53-01): whether an arrangement edit (Select all, Delete) may reach this clip: in
// Clips the clip grid's clips (the live area) are never part of the Arrangement's selection.
bool arrangementClip(const App& a, const Clip& c);
// Checkpoint 54 (N53-02): runs an arrangement edit of `s` (time ranges, recording) with the clip grid's
// clips set aside: a grid clip longer than its scene reaches past the grid's storage end, and a cut
// there would shorten it and leave its tail in the arrangement. They go back where they stood.
void withoutGridClips(const App& a, Song& s, const std::function<void(Song&)>& fn);  // (outside Clips: fn(s) as it is)

// ---- the main view: clip grid or arrangement (one at a time, Tab swaps) ----
bool gridView(const App& a);
void showView(App& a, bool grid);
// Per frame while Live is active: keeps exactly one of the two views open (a view closed from a
// menu brings the other one back), and Tab swaps them when no text field has the keyboard.
void viewFrame(App& a);
// N45-10: a set opened in Clips shows the view it was last saved in here (the Arrangement when it was
// saved with the Arrangement in front, else the clip grid, as a new set); saving remembers which.
void projectOpened(App& a, const std::string& file);
// N47-17: a new set: its first track is selected on Clips' next frame.
void setOpenedNew();
// N47-12: the clip menu's Rename: the clip band shows the clip and its Name field takes the typing.
void renameClip(App& a, ObjectId clip);
// Checkpoint 49: true on the frame the clip band's Name field (MIDI or audio clip) takes the typing.
bool renameFocusNow();
// Checkpoint 49: the track menu's Rename (and Cmd+R on a selected track): in the clip grid the track's
// title becomes a name field in place, as the reference's; in the arrangement a name prompt.
void renameTrack(App& a, int track);
void projectSaved(App& a, const std::string& file);
// Per frame while Live is not active (puts back what viewFrame changes in Dear ImGui).
void leftFrame();

// ---- the arrangement's track headers (right of the lanes) ----
constexpr float kHeaderW = 216.0f;  // the header column at 100 % (wide enough for the demo song's track names)
// What part of a header is under the pointer.
// Io: checkpoint 55, the In/Out column (it acts on its own click).
enum class HeaderPart : int { None = 0, Name, Activator, Solo, Arm, Volume, BackToArrangement, Pan, Fold, Io };  // Fold: checkpoint 51, a group's
// Checkpoint 55 (gap 2): the header column's width (wider by the In/Out column while it shows).
float headerWidth(const App& a);
struct HeaderDraw {
  int track = -1;
  ImVec2 h0, h1;          // the header's rectangle
  bool hovered = false;   // the pointer is over this header
  bool selected = false;  // in the header selection
  bool lit = false;       // moving with a header drag
  ImVec2 mouse;
  int number = 0;         // checkpoint 54: the number Clips shows it by (trackmodes::trackNumbers)
  bool lane = false;      // checkpoint 54 (N53-03): an automation lane: its parameter's name only, no controls
};
// Draws one header and returns the part under the pointer (None when not hovered).
HeaderPart drawHeader(App& a, ImDrawList* dl, const HeaderDraw& h);
// The rows under the last track: the return tracks (mixer tracks other tracks send to), each
// with its name, volume and meter. Drawn from `top` down, between x0 and x1, while above `bottom`
// (Main's row, pinned there, excluded); a click selects the mixer track. Returns their height.
float drawBusRows(App& a, ImDrawList* dl, float x0, float x1, float top, float bottom);
// Main's row, pinned at the foot of the header column (its top at `y`), over the tracks.
void drawMainRow(App& a, ImDrawList* dl, float x0, float x1, float y);
constexpr float kBusRowH = 22.0f;
// Runs a click on a header part (mute, solo, arm). True when it was handled.
bool clickHeader(App& a, int track, HeaderPart part, bool right);
const char* headerHint(HeaderPart part);

// ---- the arrangement's extras: the overview strip and the minutes ruler under the lanes ----
constexpr float kOverviewH = 16.0f, kTimeRulerH = 18.0f;

// ---- the status line ----
void drawStatusBar(App& a);

// ---- the clip grid (the Live pads window's body in the Live layout) ----
// Tracks as columns, scenes as rows: scene k is the k-th span of kSceneBars bars of the live
// area, and a track's slot in it holds the clips that start there (Studio's live area, unchanged).
// Under the slots each column has its mixer controls, aligned with the column.
constexpr int kSceneBars = 4;
void drawClipGrid(App& a);
// The Mixer window's body in the Clips layout (its mixer band): the clip grid's titles and strips,
// returns beside Main at the right; edits go to the same mixer parameters.
void drawMixerBand(App& a);
// K-287 (dev input "livegrid"): the next drawn clip grid prints each slot's centre and clip.
void devGridAsk();
// C44-16: the Create menu's grid items: an empty scene under the selected slot's (else at the end), and
// a one-bar MIDI clip in the selected slot (else the selected track's first empty one). Run next frame.
void gridInsertScene(App& a);
void gridInsertMidiClip(App& a);
// N45-08: the track a new track goes after: the selected title or header, else the selected slot's
// track, else a selected clip's track; -1 when nothing is selected (it then goes after the tracks in
// use). newTrack makes an audio or MIDI track there and selects it (title and slot).
int selectedTrack(App& a);
int newTrack(App& a, bool audio);
// N47-11: Create's New return track (one undo step), and the detail bands shown or hidden together
// (both hidden: the clip band comes back).
void newReturnTrack(App& a);
void toggleDetail(App& a);
int newTrackAfter(App& a, bool audio, int after);
// Checkpoint 51: Cmd+G groups the selected tracks (`track` >= 0: that track's menu: the selection
// when it holds that track, else the track alone) into a group track; Cmd+Shift+G ungroups the
// selected group (a member selected: its group). trackmodes::groupTracks / ungroupTracks.
// N50-02: whether a clip of this kind may land on the track in Clips: MIDI clips on MIDI tracks,
// audio clips on audio tracks, nothing on a group track (automation clips anywhere).
bool clipFitsTrack(const Song& s, ClipKind kind, int track);
// Checkpoint 52: Clips' Delete / Duplicate of the selected tracks (or `track`): a group brings its tracks.
void deleteTracks(App& a, int track);
void duplicateTracks(App& a, int track);
void groupTracks(App& a, int track = -1);
void ungroupTracks(App& a, int track = -1);
// Checkpoint 53 (N52-10): A shows or hides the automation lanes in the Arrangement (the reference's
// automation mode); shown by default.
void toggleAutomationLanes(App& a);
bool automationLanesShown();

// ---- parts drawn the Live way (LayoutParts calls these while Live is active) ----
// The device chain as a row of devices. `pick` is the chain's device (the one opened last: from the
// mixer, the rack or View); `open` says it was opened since the last frame: the band selects it and
// shows its whole editor in the panel above the chain. A click on a device's title selects it, a
// double-click folds it to a strip; its expand arrow (or a double-click on its picture) shows its
// whole editor above the band, several side by side; its window button opens the editor in the
// floating window Studio uses instead (`fxWindow` / `insWindow`: the band then leaves the effect
// editor / channel settings body to its window). An installed plugin shows Gridline's parameter
// rows (all of them up to 64, else the ones chosen with Configure) and its window button shows the
// plugin's own window.
struct BandPick {
  int kind = 0;  // 0 instrument, 1 effect slot, 2 master chain (its own window)
  int slot = -1;
  ObjectId fxTrack = kNoId;
  bool open = false;
  bool fxWindow = false, insWindow = false;
};
void deviceBand(App& a, BandPick& pick);
void infoCorner(App& a);  // the hovered thing's name and what it does

// ---- the clip band's left column (the piano roll draws it while Live is active) ----
// The edited clip's properties (the pattern's name, length and meter) and its track's launch
// settings, at the cursor, as tall as the space left. Returns its width (0 when the window is too
// narrow or short for it); the cursor stays where it was, so the caller indents its own content
// by the width. The column is a child of the caller's window, and the caller's content stays in its
// own window (indented, not wrapped in a child), so focus, hover and popups work as before.
float clipColumn(App& a);
// K-112 (dev, --menu device): the device band's menu of the first effect on the selected channel's
// mixer track (none: of its instrument), as its title bar opens it.
void devDeviceMenu(App& a);
// The column's pieces, shared with the audio clip view (Playlist.cpp drawLiveAudioClip): a section's
// title bar (`fill`: the clip's colour, a tinted band with a swatch; a `foldId` folds the section;
// returns whether it is open), a field's label above it (`cols` fields side by side, this one at
// `col`; the cursor is left at the field, whose width it returns), and the fades with an arrow at an
// edge past which the column scrolls (call before EndChild).
bool columnSection(App& a, const char* title, ImU32 fill, const char* foldId);
float columnRow(App& a, const char* label, int col, int cols);
// N48-12: a bars. beats. sixteenths field of the clip view (drag, wheel, arrows once picked,
// double-click to type); `duration` counts from 0. 0. 0. True when `v` changed.
bool positionField(App& a, const char* id, Tick& v, Tick lo, Tick hi, float w, const char* hint, bool duration = false);
// N48-12: the one pattern clip selected that plays `pattern` (kNoId: none) and its region in the
// notes, [Start, End): its offset and its end. The clip view's fields and the notes editor's brace
// both show and change it (setClipRegion: one undo step per drag with `mergeKey`).
ObjectId clipRegion(const App& a, ObjectId pattern, Tick& start, Tick& end);
void setClipRegion(App& a, ObjectId clip, Tick start, Tick end, const char* label, uint64_t mergeKey = 0);
// K-112c (round 23): a read-only value in a box the field's colour with its 1 px rim, `w` wide and a
// frame tall, its text where a field's text starts (the refs box every value in the clip column).
void valueBox(App& a, const char* text, float w);
void columnScrollEdges();
// The clip the clip band shows as an audio clip: the one clip selected in the playlist or the
// launch grid when it is an audio clip, else kNoId (the band then shows the notes editor).
ObjectId focusedAudioClip(App& a);

// K-112 (dev): the text centring check. devCentreAsk() (dev input "centre") asks for one report;
// devCentreFrame() prints it during the frame: per font and size, the capital height and baseline
// from the font, and how far the older "(h - size) / 2 - 1" formula sits from the true centre.
void devCentreAsk();
void devCentreFrame(App& a);

}  // namespace gl::livelook
