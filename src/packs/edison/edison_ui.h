// Gridline — "edison" pack: editor internals shared by the edison_*_ui.cpp
// files. One Session per open editor (a mixer slot, or a standalone window on
// a channel's sample); it owns the undo history, view state, caches and the
// worker job that runs edits off the UI thread.
#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../core/SmoothScroll.h"
#include "../../app/PluginSkin.h"
#include "../PacksUi.h"
#include "../../core/PianoRollScript.h"
#include "../../engine/LiveWave.h"
#include "../../core/TimeWarp.h"
#include "edison.h"
#include "edison_repair.h"
#include "edison_specimage.h"
#include "edison_transport.h"

namespace gl::packs::edison {

// ------------------------------------------------------------------ palette (Edison roles)
namespace col {
inline constexpr uint32_t Panel = 0x3B4247, PanelHi = 0x454D53, PanelLo = 0x30363A;
inline constexpr uint32_t Btn = 0x4A5359, BtnHi = 0x58636A, BtnLo = 0x2C3236, Glyph = 0xD5DCE0, GlyphDim = 0x8E999F;
inline constexpr uint32_t Lit = 0xFF9637;       // lit glyphs / active toggles (orange role)
inline constexpr uint32_t Led = 0x7AE4A3;       // LED "on" (check boxes)
inline constexpr uint32_t Rec = 0xE5484D;
inline constexpr uint32_t WaveBg = 0x1C2124, WaveGrid = 0x2A3135, WaveGridHi = 0x39434A;
inline constexpr uint32_t Wave = 0x8FD3A0, WaveDim = 0x4E7E5C, WaveSel = 0xD3F5DB;
inline constexpr uint32_t SelBg = 0x34495A, SelEdge = 0x6F9BC2;
inline constexpr uint32_t RulerBg = 0x272D31, RulerText = 0xA7B3B9;
inline constexpr uint32_t Marker = 0xE8C46A, Region = 0x6A8FD6, Loop = 0xD9895A;
inline constexpr uint32_t Playhead = 0x3D63DD, Cursor = 0xF2F2F2;
inline constexpr uint32_t Text = 0xD6DDE1, TextDim = 0x8F9AA0, Lcd = 0x1A1F22, LcdText = 0xB8F0C6;
inline constexpr uint32_t Brush = 0xF0A040;
}  // namespace col

ImU32 C(uint32_t rgb, float a = 1.f);

// ------------------------------------------------------------------ tools (dialogs)
enum class Tool {
  None, Stretch, Equalize, Reverb, Blur, Denoise, Gate, Declick, Resample, Amplify, Normalize, Envelope, Slice, Tune, Silence,
  Spectral, Rename, Fade, Script,
  Deverb, Declip, DrumStretch,  // the repair tools (edison_repair_ui.cpp)
  Count
};

struct Job {
  std::future<std::function<void()>> fut;  // the worker returns the UI-thread completion
  std::string label;
  double started = 0;
  std::shared_ptr<Progress> progress;  // an edit that reports how far it is (and can be stopped)
  bool active() const { return fut.valid(); }
};

// (SpecLayer, the spectrogram image: edison_specimage.h)

struct Session {
  uint64_t key = 0;
  // Owner: a mixer slot (gl.edison), or a channel's sample (standalone).
  bool slot = false;
  PluginTarget target;
  ObjectId channel = kNoId;  // standalone: the channel whose sample is edited
  ObjectId sourceClip = kNoId;             // K-419: linked Playlist audio clip for warp marker sync
  std::vector<WarpMarker> clipWarpOrig;    // K-419: baseline warp markers from sourceClip
  std::vector<WarpMarker> clipWarpSynced;  // K-419: last warp markers synced with sourceClip
  bool pendingClipWarpImport = false;      // K-419: import sourceClip warp markers once audio finishes loading
  std::string file;          // a file opened on its own (the browser's Edit in Loupe): no owner in the project
  bool open = true;          // standalone window shown
  std::string dragOut;       // K-17: the file the drag out of the editor carries (made once per drag)
  bool dragOutUsed = false;  // K-17: the drag handle was dragged since it was pressed
  std::string title;

  History hist;
  std::string loadedFrom;  // file the history was loaded from ("" = empty / generated)
  std::string committed;   // file the current version is saved in
  uint64_t committedSerial = 0;
  bool pendingCommit = false;
  std::string commitLabel;

  // view
  double viewStart = 0, viewLen = 0;  // frames
  smoothscroll::Axis viewAx;          // J-67: wheel scrolling glides (no elastic edges: frames outside the take)
  gesture::SmartZoomRange smartZoom;  // K-410: trackpad smart-zoom toggle state
  float vZoom = 1.f;
  Range sel;
  int64_t cursor = 0;
  int64_t anchor = -1;  // selection drag anchor
  bool dragging = false;
  int dragMarker = -1;  // marker being moved
  bool spectral = false;
  bool brushMode = false;
  // K-248: warp as an edit: markers on the audio (file seconds), then Apply writes the warped audio
  // (one undo step). The markers are saved with the project on the owner's plugin state
  // (PluginState::loupeWarp: the channel's instrument, or the Loupe slot): each change is one step
  // of the song's undo and reaches collaborators; the session follows the owner (song undo, a
  // collaborator, a project load). A file opened on its own keeps them in the session only.
  bool warpOn = false;
  std::vector<WarpMarker> warp;
  std::string warpSynced;  // the owner's markers (PluginState::loupeWarp) the session last took or wrote
  // Each warp marker gesture (add, move, slide, remove, a Warp menu row) is one step of Loupe's
  // undo, taken back while it is the latest thing done (`serial`: the history's version it was
  // made on; a later audio edit or undo leaves the markers as they are). `docSerial`: the song's
  // undo state the step left (Loupe's undo of it is the song's undo while nothing came after).
  struct WarpStep {
    std::string label;
    std::vector<WarpMarker> before, after;
    uint64_t serial = 0;
    uint64_t docSerial = 0;
  };
  std::vector<WarpStep> warpUndo, warpRedo;
  bool warpGesture = false;               // a marker gesture is under way
  std::vector<WarpMarker> warpBefore;     // the markers when it began
  std::string warpLabel;
  int snap = 1;          // 0 none, 1 zero crossings, 2 markers, 3 beat grid
  bool barsTime = false; // ruler in bars:beats
  bool scrubMode = false;
  bool scrubbing = false;
  transport::ScrubFollow scrub;  // scrubbing outside a mixer slot (grains through the preview)
  double scrubT = 0;             // time of the last scrub step
  int edgeDrag = 0;              // the selection edge being dragged (1 start, 2 end)
  // Dev checks ("loupe state"): the lanes and ruler last frame (screen px) and the view (frames).
  float devLx0 = 0, devLx1 = 0, devLy0 = 0, devLy1 = 0, devRy0 = 0, devRy1 = 0;
  double devViewStart = 0, devViewLen = 0;
  float brushHz = 0;     // brush height (octaves fraction) — set on first use
  std::vector<SpectralRect> brush;

  // caches
  PeakCache peaks;
  uint64_t peaksSerial = 0;
  SpecLayer specBase;        // the whole file (shown while the view's own image is made)
  SpecLayer specView;        // the frames in view, one column per screen pixel
  SpecViewRequest specWant;  // what the view asked for last frame
  double specWantSince = 0;  // when that changed (a new image waits until the view rests)
  Levels levels;
  PitchInfo pitch;
  uint64_t levelsKey = ~0ull;
  std::future<std::pair<Levels, PitchInfo>> levelsJob;

  // playback (standalone: engine preview of a temporary sample)
  bool pvPlaying = false;
  double pvT0 = 0;
  int64_t pvFrom = 0, pvTo = 0;     // the preview starts at pvFrom and plays to pvTo
  int64_t pvLoopFrom = 0;           // ... then, when pvLoop, [pvLoopFrom, pvTo) over and over
  bool pvLoop = false;
  SamplePtr pvSample;
  int64_t playFrom = -1, playTo = -1;  // the range playing (a new selection while playing changes it)
  bool playLoop = false;
  int64_t playStart = -1;   // where the last play or jump started
  uint32_t settleWant = 0;  // the slot player's handled() count that carries it out (0: at once)
  bool settleSaid = true;   // reported to scripted runs ("loupe-settled")
  bool loop = false;  // Loop outside a mixer slot (in a slot it is the plugin's Loop parameter)

  // recording (slot)
  Recorder rec;
  size_t ringPos = 0;
  bool ringStarted = false;
  float inMeter = 0, outMeter = 0;
  // K-145: the take drawn live while it records (fed from `rec` every frame; the view fits
  // the whole take into the lanes once it reaches the right edge).
  LiveWave live;
  LiveFit fit;
  size_t liveFed = 0;     // take frames added to `live`
  size_t liveSeed = 0;    // frames of existing audio before the take (add after the existing audio)
  bool simArmed = false;  // dev: GRIDLINE_EDISON_SIM feeds a generated input
  int64_t simPos = 0;

  NoiseProfile noise;
  Job job;
  std::string status;  // last tool message (hint bar line)
  double statusUntil = 0;

  // tool dialog state
  Tool tool = Tool::None;
  bool toolOpenReq = false;
  float tStretch = 1.f, tSemis = 0.f;
  int tMode = 0, tQuality = 1;
  std::vector<EqPoint> eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}};
  ReverbSettings rv;
  float blurMs = 40, blurMix = 0.5f;
  float dnReduce = 24, dnSens = 0.5f;
  float gThr = -45, gAtt = 1, gHold = 30, gRel = 80, gFloor = -90;
  float dcSens = 0.5f;
  int rsRate = 48000, rsQuality = 2;
  float ampDb = 3, normDb = 0;
  std::vector<std::pair<double, double>> env = {{0, 1}, {1, 1}};
  float sliceSens = 0.5f;
  int tuneNote = -1;
  float silenceSec = 1.f;
  float spectralDb = -30;
  int fadeCurve = 0;
  int renameMarker = -1, renameRegion = -1;
  int devMenu = -1;  // J-207 dev: menu to open on the next frame (screenshots)
  char renameBuf[64] = {};

  const Content& cur() const { return hist.cur(); }
  int64_t frames() const { return hist.cur().frames(); }
  double rate() const { return hist.cur().rate(); }
};

// Sessions (edison_ui.cpp).
Session& sessionFor(App& a, const PluginTarget& t);
EdisonFx* slotFx(App& a, const Session& s);
const PluginState* slotState(App& a, const Session& s);
double param(App& a, const Session& s, int k, double dflt);
// Loop playback: the slot's Loop parameter, or the session's own Loop outside a slot.
bool loopOn(App& a, const Session& s);
void setLoop(App& a, Session& s, bool on);
void setParam(App& a, const Session& s, int k, double v);

// Edits: run `op` on the worker; its result becomes one undo step. `map` (GitHub #69): how the
// edit moved the audio (a stretch, paste, cut, crop, trim, resample, insert); afterwards the
// selection and the cursor cover what they covered before (mapSelection), unless they were
// changed while the edit ran. Undo brings back the selection the edit started from, redo the one
// it left. Without a map they stay where they were (clamped to the new length).
void runEdit(App& a, Session& s, const std::string& label, std::function<Content(const Content&)> op, EditMap map = {});
// Same, for a long edit: `op` reports its progress (shown on the busy plate and in the tool's
// dialog) and returns early when asked to stop (then nothing changes: no undo step).
void runEditP(App& a, Session& s, const std::string& label, std::function<Content(const Content&, Progress&)> op, EditMap map = {});
// Same, immediately on the UI thread (cheap edits: markers, loop points).
void applyNow(App& a, Session& s, const std::string& label, const Content& next);
void undo(App& a, Session& s);
void redo(App& a, Session& s);
// Undo / redo has a step to take (an audio edit, or a warp marker gesture made last).
bool canUndo(const Session& s);
bool canRedo(const Session& s);
// K-248: a warp marker change made from `before` to the session's markers, as one undo step.
void pushWarpStep(App& a, Session& s, const std::string& label, const std::vector<WarpMarker>& before);
// The session's warp markers onto the owner's plugin state (one song undo step; nothing for a file
// on its own or markers already there).
void saveWarp(App& a, Session& s, const std::string& label, uint64_t mergeKey = 0);
void say(Session& s, const std::string& text);
Range target(const Session& s);  // selection or whole file

// Spectrogram view (edison_specview_ui.cpp). Frequencies run from kSpecMinHz at the bottom to
// the Nyquist frequency at the top on a log axis.
// Draws frames [start, start + len) into p0..p1 (images are made on a worker for the screen's
// pixels; until one is ready the last one, or the whole file's, stands in).
void drawSpecView(App& a, Session& s, ImDrawList* dl, ImVec2 p0, ImVec2 p1, double start, double len);
// The level shown at a frame and frequency (dBFS); false when no image covers it.
bool specLevelAt(const Session& s, double frame, double hz, float& db);
// (kSpecMinHz, specRange and specResolution: edison_specimage.h)
void setSpecRange(App& a, int db);
void setSpecResolution(App& a, int r);
// Clipboard shared by every editor.
Audio& clipboard();

// Transport.
void play(App& a, Session& s);
void stopPlay(App& a, Session& s);
bool isPlaying(App& a, Session& s);
double playPos(App& a, Session& s);  // frames, -1 when stopped
// What Play plays now (selection, loop points, cursor; edison_transport.h).
transport::PlayRange playRange(App& a, const Session& s);
// Per frame: a new selection (or loop) while playing changes the playing range at once; the
// playback keeps its place when it is inside, else it goes to the new start. Stopped, a mixer
// slot keeps the range a following song transport starts from (the cursor).
void syncPlayRange(App& a, Session& s);
// Scrubbing (the Scrub button, Alt, or a drag along the ruler): in a mixer slot the slot's
// player follows the pointer; elsewhere grains through the preview do. The cursor stays where
// the scrub ends.
void scrubBegin(App& a, Session& s, double frame);
void scrubMove(App& a, Session& s, double frame);
void scrubEnd(App& a, Session& s);
// Loupe follows the playhead (setting loupe.autoScroll, also the editor's own toggle).
bool autoScroll(App& a);
void setAutoScroll(App& a, bool on);
// K-421: Loupe UI scale (0.75 .. 2.0) and remembered floating window size (persisted in Settings).
float uiScale(App& a);
void setUiScale(App& a, float scale, bool resizeWindow = true);
void resetWindowSize(App& a);
void rememberWindowSize(App& a, float w, float h);
void preferredWindowSize(App& a, float extraChromeW, float extraChromeH, ImVec2 workSize, float& outW, float& outH, bool& forceApply);
void drawSizeMenu(App& a);
void toggleRecord(App& a, Session& s);

// Actions shared by the toolbar and the menus.
void saveAs(App& a, Session& s, FileFormat f);
void exportSelection(App& a, Session& s);
void openFile(App& a, Session& s);
void sendToPlaylist(App& a, Session& s);
// K-17: the file a drag out of the editor carries: the selection, saved as its own file in
// the edited audio folder (named by its content), or with no selection the whole current
// version, saved the way Send to playlist saves it. "" while busy or empty.
std::string dragOutFile(App& a, Session& s);
// After the item that starts the drag (the toolbar's drag handle): the drag source. The
// payload is a browser audio file ("GL_FILE"), so the playlist (at the drop position, with
// snap), the channel rack, sample slots and samplers take it as they take a browser file.
void dragOutSource(App& a, Session& s);
// J-191: the current version into the Split stems window (files in the Stems folder); K-447: only the
// selection when there is one.
void splitStems(App& a, Session& s);
void sendToSampler(App& a, Session& s, bool slicer);
// K-248: the warp markers applied: the warped audio replaces the content (one undo step), the markers cleared.
void applyWarp(App& a, Session& s);
void detectSlices(App& a, Session& s, double sensitivity);
void addMarkerAtCursor(App& a, Session& s);

// K-15: Loupe pinned into the main layout (edison_ui.cpp). The "Loupe" panel is a dock
// window like the playlist; while pinned (setting ui.loupePinned) every Loupe that opens
// (a sample, a Loupe effect's editor) shows in it instead of a window of its own.
enum class DockSpot { Tab, Below, Left, Right, Floating };
bool pinned(App& a);
bool inPanel(const Session& s);  // this session is the one the panel shows
void pinTo(App& a, Session& s, DockSpot spot);
void unpin(App& a);              // the panel closes; its session goes back to its own window
void closeOwnWindows(App& a);     // every Loupe window of its own closes (the panel stays)
void dockMenuItems(App& a, Session& s);
// An audio file on its own (the browser's Edit in Loupe): its own window, no owner.
bool openFileInLoupe(App& a, const std::string& path);  // View menu: Pin to the layout / Unpin
bool claimEffectEditor(App& a, const PluginTarget& t);
// K-112: pinned and `t` holds Loupe: the panel's session becomes that slot's (nothing else changes).
bool showInPanel(App& a, const PluginTarget& t);
// K-112 (R3): the Loupe panel's content in `avail` (its session, or the empty state).
void drawPanelBody(App& a, ImVec2 avail);
// Dev checks (--dev-input "loupe [pin <spot> | unpin]"): the panel's room and layout last frame.
struct DockDev {
  float w = 0, h = 0;
  bool compact = false;
  float scale = 0, formW = 0, headH = 0, waveH = 0;
};
DockDev& dockDev();
bool dockDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);

// Drawing (edison_form_ui.cpp / edison_wave_ui.cpp / edison_tools_ui.cpp).
void drawForm(App& a, Session& s, float width, float maxHeight);
// K-15: the form pinned into the main layout, laid out for the dock's space `avail`.
void drawFormDocked(App& a, Session& s, ImVec2 avail);
namespace dock {
struct Layout;
}
void drawFormLayout(App& a, Session& s, const dock::Layout& layout);
namespace sty {
struct Metrics;
}
// The nine menu words from tl, cells cellW x h, perRow to a row (edison_form_ui.cpp).
void drawMenus(App& a, Session& s, ImVec2 tl, float cellW, float h, const sty::Metrics& M, int perRow = 9);
void drawWaveArea(App& a, Session& s, ImVec2 p0, ImVec2 p1, const sty::Metrics& M);
void drawToolDialogs(App& a, Session& s);
void openTool(Session& s, Tool t);
void handleKeys(App& a, Session& s);

// Audio editor scripts (edison_script_ui.cpp, SC-05 / ED-09).
const std::vector<script::ScriptInfo>& audioScripts(App& a, bool rescan = false);
void audioScriptMenuItems(App& a, Session& s);  // body of Tools > Scripts
void openAudioScript(App& a, Session& s, const std::string& path);
void drawScriptDialog(App& a, Session& s);      // inside the tool dialog

// View helpers.
void zoomAll(Session& s);
void zoomTo(Session& s, Range r);
void zoomBy(Session& s, double factor, double centreFrame);
std::string timeText(App& a, const Session& s, int64_t frame, bool precise = true);
// K-145: the rate the view's times are in (the live take's while it records).
double viewRate(const Session& s);

// Import from URL: a file into the open Loupe window with this key (false when it has closed).
bool loadIntoSession(App& a, uint64_t key, const std::string& path);

}  // namespace gl::packs::edison
