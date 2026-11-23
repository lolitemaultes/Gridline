// Gridline — "seltmann" pack: editor internals shared by the editor's parts.
// 0.7.8 Ribbon: the waveform, its lettered regions and the sequence share one time axis (the
// ribbon); the layouts run in a strip of cards under it (Standard) or a library list beside it
// (Full); a region's or a step's settings open as a card where it was clicked; Sound, Grid, Loops
// and the chop modes open as cards from the header and the action row. Three heights: Compact (a
// device band), Standard, Full (with the key map), picked by the room the host gives.
//   seltmann_ui.cpp        frame, empty / analysing state, header, action row, compact row, shared
//                          helpers, auditions, renders, linked "send to project", editor keys
//   seltmann_wave_ui.cpp   the waveform (bar grid, regions, tools), the whole-song strip, chop settings
//   seltmann_seq_ui.cpp    the sequence blocks under the waveform, the region menu
//   seltmann_parts_ui.cpp  the layout cards, the library list, the compact picker, previews, the keys strip
//   seltmann_cards_ui.cpp  the cards: region / step, sound, grid, loops, chop, steps
//   seltmann_rec_ui.cpp    the recorder (an empty instance, or a new take into a loaded one)
//   seltmann_pane_ui.cpp   Smooth's lower-pane tab
// UI thread only.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../app/RunLatch.h"
#include "../../third_party/imgui/imgui.h"
#include "seltmann.h"
#include "seltmann_editor.h"

namespace gl::packs::seltmann::sui {

// Background render of loops into the project's "Seltmann loops" folder.
struct LoopJob {
  std::atomic<int> done{0}, total{0};
  std::atomic<bool> finished{false};
  std::vector<std::string> files;  // written when finished
};

// The cards that open over the editor (one at a time).
enum Card { CardNone = 0, CardRegion, CardStep, CardSound, CardGrid, CardLoops, CardChop, CardSteps, CardTakes };

// A layout preview being heard: the layout rendered on the current chop, played as a loop.
struct PreviewJob {
  std::atomic<bool> done{false};
  SamplePtr sample;  // the loop (valid once done; null when it could not be rendered)
};

// A recorded take this instance has used (this session): the file and its length.
struct Take {
  std::string path;
  int64_t frames = 0;
  double rate = 0;
};

// Per editor (one per plugin instance), UI-only.
struct Ed {
  // parsed state
  std::string data;           // PluginState::data the model was parsed from
  std::string path;
  bool hasModel = false;
  Model m;
  JobPtr job;                 // analysis + peaks of `path`
  bool reanalyse = false;     // the next finished job replaces the grid
  // view
  int selSlice = 0, selStep = -1;
  int presetTab = 2;          // the layouts' tab: 0 Starred, 1 Recent, 2 + PresetTab
  char search[64] = {};
  int keyPage = 0;            // the keys strip's page of regions
  int cardPage = 0;           // the first layout card shown
  bool stripHidden = false;   // songs: the whole-song strip is hidden
  int tool = ToolPlay;        // Play / Cut / Grid
  int height = HeightFull;    // the form drawn this frame
  bool hosted = false;        // drawn in a layout's region (not its own window)
  // cards
  int card = CardNone;
  ImVec2 cardAnchor{0, 0};    // where it was opened (its arrow points there)
  bool cardBelow = true;      // opens under the anchor (else above)
  bool cardTouched = false;   // this frame's click opened or retargeted it (do not close it)
  ImVec2 card0{0, 0}, card1{0, 0};  // its rect (trace)
  ImVec2 win0{0, 0}, win1{0, 0};    // the room it may use (the host window's visible rect)
  // layouts: preview, stars, recent
  LayoutPrefs prefs;
  bool prefsLoaded = false;
  std::string previewName;    // the layout being previewed ("" none)
  std::shared_ptr<PreviewJob> previewJob;
  bool previewPlaying = false;
  bool previewResume = false; // the pattern played before the preview: it plays again after
  bool previewHeld = false;   // the preview follows a held card (else the pointer resting on it)
  double pressAt = -1;        // when the layout under the pointer was pressed
  std::string hoverName;      // the layout the pointer rests on (hover preview) and since when
  double hoverAt = -1;
  bool previewSeen = false;   // this frame drew the item being previewed under the pointer or held
  bool savingLayout = false;  // the Save layout field is shown (Standard: in a small pop-up)
  bool hasCustom = false;     // "Custom (yours)": the hand-edited sequence a layout replaced
  Model custom;
  std::map<std::string, std::vector<int>> chips;  // layout name -> regions it plays on this chop (cache)
  std::string chipsFor;       // the state data the chips were made for
  bool libOpen[12] = {true, true, true, false, false, false, false, false, false, false, false, false};  // library groups
  float libScroll = 0;
  // takes
  std::vector<Take> takes;
  // interaction
  int dragCut = -1;
  int dragRegion = 0;         // 0 none, 1 left edge, 2 right edge, 3 move, 4 new
  double dragA = 0, dragB = 0, dragGrab = 0;
  int dragBeat = -1;          // grid stretch (Alt+drag a bar line, Grid tool)
  int auditionKey = -1;       // key held for an audition
  // J-110: the pattern's Play button. It follows the engine (Stop, panic,
  // focus loss: RunLatch) and the instrument's own running flag
  // (LiveView::patternRunning); patternKey = the key that latched it.
  RunLatch pattern;
  int patternKey = -1;
  bool autoPlayed = false;    // dev hook GRIDLINE_SELTMANN_PLAY: started once
  std::vector<double> taps;
  uint32_t seed = 1;
  std::vector<Preset> user;
  bool userLoaded = false;
  char dsl[1024] = {};
  std::string dslFor;         // steps text the field was filled from
  char presetName[64] = {};
  bool typingBpm = false;
  char bpmText[16] = {};
  // renders
  std::map<std::string, std::string> rendered;  // item key -> file
  std::shared_ptr<LoopJob> loopJob;
  bool demoDone = false;
  // dev trace (GRIDLINE_SELTMANN_TRACE): the last state line printed, geometry for scripted input
  std::string lastTrace;
  ImVec2 zone0, zone1, playBtn;
  ImVec2 recEntry{-1, -1};             // J-218: the start screen's Record button (trace)
  std::vector<std::pair<std::string, ImVec2>> cards;  // visible layout cards (name, centre)
  std::vector<std::pair<std::string, ImVec2>> btns;   // 0.7.8: named controls for scripted runs
  ImVec2 tracePos{-1e9f, -1e9f};  // (trace) the window's place last frame
  std::string crowd;  // (trace) rows whose controls ran into each other this frame
  // J-149 sequence lane
  int seqDrag = -1;           // step being dragged to a new place (-1 none)
  int seqDrop = -1;           // insertion point under the pointer while dragging
  int seqPress = -1;          // step pressed (click or the start of a drag)
  int menuStep = -1, menuRegion = -1;  // what the open region menu acts on
  bool menuWant = false;               // open it this frame
  std::vector<ImVec2> seqCentres;      // block centres (scripted input, trace)
  ImVec2 seq0, seq1;                   // the lane
  ImVec2 body;                         // J-152: the editor body's size (trace: measured heights)
  // J-185 slice pitch, the sequence's start flag, direct slice resizing
  uint32_t gesture = 0;                // counts drags (one undo step each)
  int pitchDrag = -1;                  // slice whose pitch chip is dragged
  float pitchFrom = 0, pitchAcc = 0;   // pitch at the press, pointer travel (coarse drags)
  float wheelAcc = 0;                  // wheel travel not yet turned into pitch steps
  double wheelLast = -1;               // J-208: time of the last pitch notch (a pause starts a new undo step)
  int wheelSlice = -1;                 // J-208: the slice the last notches pitched
  int cutDrag = -1;                    // cut (slice index) being dragged in the waveform
  Model cutBase;                       // the model when the cut drag began
  bool startDrag = false;              // the START flag is being dragged
  ImVec2 startFlag{-1, -1};            // its centre (trace)
  std::vector<std::pair<std::string, ImVec2>> menuPts;  // items of the open region menu (trace)
  std::vector<ImVec2> cutPts, laneLenPts, padChips;     // cut handles, step length handles, pitch readouts (trace)
  // J-214 step lengths: the right edge of a block in the sequence sets that step's own length
  int lenDrag = -1;                    // step whose length is dragged (-1 none)
  double lenFrom = 0, lenStart = 0;    // its length and (unswung) start at the press (beats)
  double lenShown = 0;                 // the lane's length at the press (the scale holds while shortening)
  float lenPx = 1, lenX0 = 0;          // pixels per beat and the pointer at the press
};
Ed& ed(const PluginTarget& t);

// ---- colours / text
ImU32 accent();                       // Sample family accent
ImU32 slotColor(int slot, float alpha = 1.0f);
int sliceSlot(const Model& m, int slice);  // first slot letter that plays the slice (-1 none)
ImU32 regionColor(int region, float alpha = 1.0f);  // J-149: a region's colour (by its letter)
ImU32 regionFace(int region, float amount);         // J-152: the panel surface tinted with a region's hue
void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s);
void textR(ImDrawList* dl, ImFont* f, float size, ImVec2 rightTop, ImU32 col, const char* s);
std::string fmtTime(double seconds);
std::string noteName(int midi);
const char* keyName(int key, bool minor);
const char* kindLabel(int kind);      // "Break", "Loop", ... (translatable)

// ---- small controls (UI-only state)
// Rounded panel with a mono caption; returns the content origin.
// J-152: `strip` = the title strip's height (the tool panels host their tabs in a 30 px strip).
ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption = nullptr, float strip = 22);
// J-152: a mono group label left of its control, centred in a row of height h; returns its width plus the gap.
float inlineLabel(ImDrawList* dl, App& a, ImVec2 p, float h, const char* text);
// Small mono caption above a control group.
void groupLabel(ImDrawList* dl, App& a, ImVec2 p, const char* text);
// Pill selector over an int; returns true when changed. hints may be null.
bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur,
           const char* const* hints = nullptr, bool mono = false);
// Same, bound to a choice parameter.
bool paramPills(App& a, const PluginTarget& t, int param, ImVec2 p0, float w, float h, const char* const* labels, int count,
                const char* const* hints = nullptr);
// Raised button with text or icon; returns true when clicked.
bool button(const char* id, ImVec2 p0, ImVec2 size, const char* text, bool on = false, const char* hint = nullptr, int icon = -1,
            bool primary = false);
// Knob over a model value (not a parameter): drag, wheel, double-click resets.
// `released` is set when a gesture ends (commit then). Returns true while changing.
bool fieldKnob(App& a, const char* id, ImVec2 tl, float& v, float mn, float mx, float def, const char* label, const char* fmt,
               bool* released, bool bipolar = false, float step = 0);
// Drag grip (six dots).
void grip(ImDrawList* dl, ImVec2 c, ImU32 col);
// A labelled drag handle chip (grip + text) that carries a rendered item; returns its width.
float dragChip(App& a, const PluginTarget& t, Ed& e, const char* id, ImVec2 p0, float h, const char* text, const std::string& item,
               const std::string& label, const char* hint);

// ---- model edits
// Writes the model into the plugin state (one undo step; mergeKey folds drags).
void commit(App& a, const PluginTarget& t, Ed& e, const Model& m, const char* label, uint64_t mergeKey = 0);
// Same, with parameter overrides in the same step.
void commitWith(App& a, const PluginTarget& t, Ed& e, const Model& m, const std::vector<std::pair<int, float>>& params, const char* label);
SamplePtr sampleOf(Ed& e);
double paramOf(App& a, const PluginTarget& t, int k);
int baseNote(App& a, const PluginTarget& t);
// Applies a layout (one undo step) and starts the pattern when it is not playing.
void applyLayout(App& a, const PluginTarget& t, Ed& e, const Preset& p, bool play = true);
// Opens the file dialog that loads audio into this instance.
void openLoadDialog(App& a, const PluginTarget& t, Ed& e);
// Loads an audio file into this instance (one undo step; the analysis follows) and names the
// channel `name` (empty: after the file). The file dialog and the recorder both load this way.
void loadAudio(App& a, const PluginTarget& t, const std::string& path, const std::string& name = "");

// ---- J-218 the recorder on the start screen (seltmann_rec_ui.cpp)
bool recorderOpenFor(const PluginTarget& t);  // this instance shows the recorder
void openRecorder(App& a, const PluginTarget& t);
// K-145 dev checks: GRIDLINE_SELTMANN_AUTOREC="<pause at> <resume after> <finish at> [<fitted frames>]"
// (seconds of the take; 0 = no pause) opens the recorder on an empty instance and records, pauses,
// resumes and finishes by itself, wherever the window sits; with <fitted frames>, it finishes only
// once that many frames have shown the take fitted to the width (the take is real time: on a slow
// computer few frames fall between the edge and the finish).
void devAutoRecord(App& a, const PluginTarget& t);
// In place of the drop zone: input, meter, the take, Record / Pause / Resume, Finish, Clear, Load.
void drawRecorder(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);
// Per frame (packs::drawWindows): drains the input tap into the take, watches the device.
void tick(App& a);

// ---- audition / renders / project
void auditionStart(App& a, const PluginTarget& t, Ed& e, int slice, float vel = 0.9f);
void auditionStop(App& a, const PluginTarget& t, Ed& e);
void togglePattern(App& a, const PluginTarget& t, Ed& e);
// J-110: each frame, the Play button follows the engine and the instrument.
void syncPattern(App& a, const PluginTarget& t, Ed& e, const LiveView& lv);
std::string loopsDir(App& a);
// Renders an item to a file (cached); kinds: "pattern", "slice:<k>", "span:<b0>:<b1>:<name>", "midi".
std::string renderItem(App& a, const PluginTarget& t, Ed& e, const std::string& item, const std::string& label);
void makeLoops(App& a, const PluginTarget& t, Ed& e);
void sendToProject(App& a, const PluginTarget& t, Ed& e);
// Drag source for the last item: renders on the first frame of the drag and carries the file.
void dragOut(App& a, const PluginTarget& t, Ed& e, const std::string& item, const std::string& label);
LiveView live(App& a, const PluginTarget& t);

// ---- parts
void drawStrip(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);    // songs: whole file + region
void drawSections(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w);                      // songs: a button per section
void drawWave(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);     // the region, slices, grid, tools
void drawChopSettings(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w);                  // chop card: chop modes
void drawGridTools(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);                   // grid card
void drawSequence(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);  // J-149: the sequence blocks
// Layouts: the strip of cards (Standard), the library list (Full), the picker button (Compact).
void drawLayoutStrip(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w);
void drawLibrary(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);
void drawLayoutPicker(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w, float h);
// The keys strip (Full): the pattern octave and a key per region (play, pitch, drag onto a step).
void drawKeys(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);
// Layout previews: started by a held card (or a hovered one, setting), stopped when let go.
void previewStart(App& a, const PluginTarget& t, Ed& e, const Preset& p);
void previewStop(App& a, const PluginTarget& t, Ed& e);
void previewTick(App& a, const PluginTarget& t, Ed& e);
// Handles the press / hover of a layout item (a card, a library row, its play button): a quick click
// keeps the layout, a held press (or the pointer resting, setting) previews it. Call right after the
// item's InvisibleButton.
void layoutGesture(App& a, const PluginTarget& t, Ed& e, const Preset& p, bool clicked, bool previewOnly = false);
// Applies a layout the person keeps: the hand-edited sequence it replaces stays under Recent.
void keepLayout(App& a, const PluginTarget& t, Ed& e, const Preset& p);
void restoreCustom(App& a, const PluginTarget& t, Ed& e);
LayoutPrefs& prefs(App& a, Ed& e);
void savePrefs(App& a, Ed& e);
// The layouts (factory and the person's own), by name.
const Preset* findLayout(App& a, Ed& e, const std::string& name);
std::string userLayoutDir(App& a);
// The regions a layout plays on the current chop (cached per chop).
const std::vector<int>& layoutChips(Ed& e, const Preset& p);
// Region letters as small coloured chips; returns the width used.
float regionChips(ImDrawList* dl, App& a, ImVec2 p0, float maxW, float h, const std::vector<int>& regions, int total);
// Cards: open one (anchor = the point its arrow shows), draw the open one (last, over the editor).
void openCard(Ed& e, int card, ImVec2 anchor, bool below = true);
void toggleCard(Ed& e, int card, ImVec2 anchor, bool below = true);
void closeCard(Ed& e);
void drawCard(App& a, const PluginTarget& t, Ed& e, const LiveView& lv);
// A named control for scripted runs (trace "seltmann: btn <name> x y"): the last item's centre.
void noteBtn(Ed& e, const char* name);
// 0.7.8 a new take into a loaded instance: keep the chop (same tempo) or analyse it fresh.
void useTake(App& a, const PluginTarget& t, const std::string& path, bool keepChop, int64_t frames, double rate);
// Velocity of a click on a key by its height (P12): 0.45 at the top .. 1 at the foot.
float clickVelocity(float y0, float y1, float y);
// The linked patterns: each frame, a following pattern takes its channel's sequence (folded into the
// edit that changed it). Called from tick().
void linkTick(App& a);
// The pattern of this channel Send to project linked (newest; kNoId none) and its state.
ObjectId linkedPatternOf(App& a, const PluginTarget& t, LinkState* st = nullptr);
void relink(App& a, const PluginTarget& t);
void unlink(App& a, const PluginTarget& t);
// Editor keys (only those free in every key set): while the editor has focus and nothing is typed.
void editorKeys(App& a, const PluginTarget& t, Ed& e);
// The editor's commands as a menu (the keys shown where they are free): inside an open menu.
void editorKeyMenu(App& a, const PluginTarget& t, Ed& e);
// Chop sizes of the action row (0 Auto, 1 bar, 1/2, 1/4, 1/8, 1/16, Hits).
void setChopSize(App& a, const PluginTarget& t, Ed& e, int which);
int chopSizeOf(const Model& m);

// ---- J-149 region edits (one undo step each)
void splitRegionUi(App& a, const PluginTarget& t, Ed& e, int region, int parts);
void deleteRegionUi(App& a, const PluginTarget& t, Ed& e, int region);
// Menu items for a region: Split into 2..8, Delete (joins the neighbour). Inside an open popup.
void regionMenuItems(App& a, const PluginTarget& t, Ed& e, int region);
// Opens the region menu for `region` (step = the sequence step it came from, -1 none); drawn by drawRegionMenu.
// 0.7.8: region -1 with a blank step opens the blank's menu (Fill with, the step's own items).
void openRegionMenu(Ed& e, int region, int step);
// ---- 0.7.8 blank steps (one undo step each)
// The step is a blank (a rest, or nothing it could play).
bool stepIsBlank(const Model& m, int step);
// Remove from the sequence: a blank takes the step's place (length and later steps stay).
void clearStepUi(App& a, const PluginTarget& t, Ed& e, int step);
// A step (a blank or not) plays `region`.
void fillStepUi(App& a, const PluginTarget& t, Ed& e, int step, int region);
// Delete step: the step leaves the sequence, the later steps move up.
void deleteStepUi(App& a, const PluginTarget& t, Ed& e, int step);
// "Fill with" and the regions as coloured buttons (inside an open popup).
void fillMenu(App& a, const PluginTarget& t, Ed& e, int step);
void drawRegionMenu(App& a, const PluginTarget& t, Ed& e);
// Marks the sequence as edited by hand (re-chops that keep the regions keep it).
void markEdited(Model& m);

// ---- J-185 slice pitch, the sequence's start, resizing (one undo step each; drags fold)
// Sets a slice's pitch (semitones). mergeKey folds a drag or a run of wheel steps.
void slicePitchUi(App& a, const PluginTarget& t, Ed& e, int slice, float semis, uint64_t mergeKey = 0);
// The mouse wheel over the last item steps the slice's pitch (1 semitone, Shift 0.1) and keeps the wheel from scrolling.
void wheelPitch(App& a, const PluginTarget& t, Ed& e, int slice);
// A step's own pitch (the sequence: only that step changes, other steps of its region keep theirs).
void stepPitchUi(App& a, const PluginTarget& t, Ed& e, int step, float semis, uint64_t mergeKey = 0);
void wheelStepPitch(App& a, const PluginTarget& t, Ed& e, int step);
// "+3", "-12", "+0.5", "0".
std::string pitchText(float semis);
// A small pitch readout of a slice: drag up / down (semitones; Shift fine), wheel, double-click
// back to 0, right-click the region menu. `units` adds " st" when it fits `maxW`. Returns its width.
float pitchChip(App& a, const PluginTarget& t, Ed& e, const char* id, ImVec2 p0, float h, float maxW, int slice, bool units);
// The sequence starts at `step` (keepRest: the steps before move to the end; else they are left out).
void startHereUi(App& a, const PluginTarget& t, Ed& e, int step, bool keepRest);
// J-214: a step's own length in the sequence (beats; <= 0: its default). mergeKey folds a drag.
void stepBeatsUi(App& a, const PluginTarget& t, Ed& e, int step, double beats, uint64_t mergeKey = 0);
// Cut drags (waveform): begin stores the model; dragging moves cut `cut` (the start of that
// slice) to `frame` (the chop's snap, Alt: none), both neighbours resize; one undo step per drag.
void cutDragBegin(Ed& e, int cut);
void cutDragTo(App& a, const PluginTarget& t, Ed& e, int cut, double frame);
// ---- K-347 the Tracker's own screens (seltmann_tracker_ui.cpp) reuse these
// The per-frame work before drawing: the model from the state, the analysis, the first chop, the dev
// hooks, finished renders. False when the state is gone.
bool prepare(App& a, const PluginTarget& t);
// A card's body (no frame or title) in [c0, c1]: the Tracker opens Grid, Loops, Chop, Steps and
// Takes in its own modal boxes.
void cardBody(App& a, const PluginTarget& t, Ed& e, int card, ImVec2 c0, ImVec2 c1);
ImVec2 cardSize(const Ed& e, int card, float maxW);
// The layout groups (0 Starred, 1 Recent, then the PresetTab kinds: e.presetTab's numbering).
int layoutGroupCount();
const char* layoutGroupName(int g);  // English (translate with i18n::tr)
const char* layoutGroupHint(int g);
std::vector<const Preset*> layoutGroup(App& a, Ed& e, int g, const std::string& query);
const Preset& customLayout();  // "Custom (yours)": the hand-made sequence the last layout replaced
// Saves the chop and the sequence as the person's own layout (under Mine). False when it failed.
bool saveLayoutAs(App& a, const PluginTarget& t, Ed& e, const std::string& name);
// Deletes one of the person's own layouts (its file). False when it is not theirs.
bool deleteUserLayout(App& a, Ed& e, const std::string& name);
// The chop's summary ("1/8 on the hits").
const char* chopSummaryText(const Model& m);
// Loads an audio file dropped on [p0, p1] (the browser, the desktop): one undo step.
void wellDropTarget(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1);

// Merge key of a drag gesture on an item.
inline uint64_t gestureKey(uint64_t base, const Ed& e, int item) {
  return base + (static_cast<uint64_t>(e.gesture & 0xFFFFu) << 16) + static_cast<uint64_t>(item & 0xFFFF);
}

}  // namespace gl::packs::seltmann::sui
