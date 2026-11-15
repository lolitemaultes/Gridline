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
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {
class App;
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
// App::capture into the current pattern (how many, with a notification). Session record: the
// record button in pattern mode, playback started; sessionRecording: it is armed in pattern mode.
// Draw mode: the arrangement's and the notes editor's tool, the pencil or select.
int captureNow(App& a);
bool sessionRecording(const App& a);
void toggleSessionRecord(App& a);
bool drawMode(const App& a);
void setDrawMode(App& a, bool on);

// ---- the main view: clip grid or arrangement (one at a time, Tab swaps) ----
bool gridView(const App& a);
void showView(App& a, bool grid);
// Per frame while Live is active: keeps exactly one of the two views open (a view closed from a
// menu brings the other one back), and Tab swaps them when no text field has the keyboard.
void viewFrame(App& a);

// ---- the arrangement's track headers (right of the lanes) ----
constexpr float kHeaderW = 186.0f;  // the header column at 100 %
// What part of a header is under the pointer.
enum class HeaderPart : int { None = 0, Name, Activator, Solo, Arm, Volume, BackToArrangement, Pan };
struct HeaderDraw {
  int track = -1;
  ImVec2 h0, h1;          // the header's rectangle
  bool hovered = false;   // the pointer is over this header
  bool selected = false;  // in the header selection
  bool lit = false;       // moving with a header drag
  ImVec2 mouse;
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
// K-287 (dev input "livegrid"): the next drawn clip grid prints each slot's centre and clip.
void devGridAsk();

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
