// Gridline — "seltmann" pack: editor internals shared by the editor's parts.
// One flow: drop audio -> it is analysed -> pick a layout -> play / drag loops out.
//   seltmann_ui.cpp        frame, empty / analysing state, top bar, key controls,
//                          shared helpers, auditions, renders, "send to project"
//   seltmann_wave_ui.cpp   the waveform (bar grid, slices by letter), the slim
//                          region strip for songs, the chop settings (drawer)
//   seltmann_parts_ui.cpp  the layout picker (cards), the pads and the Edit
//                          drawer (slice, chop, steps, grid, sound, loops)
//   seltmann_seq_ui.cpp    J-149: the sequence (A B C D ...): drag to reorder,
//                          right-click a region to split it or delete it
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

namespace gl::packs::seltmann::sui {

// Background render of loops into the project's "Seltmann loops" folder.
struct LoopJob {
  std::atomic<int> done{0}, total{0};
  std::atomic<bool> finished{false};
  std::vector<std::string> files;  // written when finished
};

// Edit drawer tabs (advanced controls, collapsed by default).
enum DrawerTab { DrawSlice = 0, DrawChop, DrawSteps, DrawGrid, DrawSound, DrawLoops, DrawCount };

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
  int presetTab = TabFlip;
  char search[64] = {};
  int padPage = 0;
  bool drawer = false;        // the Edit drawer is open
  int drawerTab = DrawSlice;
  bool stripHidden = false;   // songs: the region strip is hidden
  // interaction
  int dragCut = -1;
  int dragRegion = 0;         // 0 none, 1 left edge, 2 right edge, 3 move, 4 new
  double dragA = 0, dragB = 0, dragGrab = 0;
  int dragBeat = -1;          // grid stretch (Alt+drag a bar line)
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
  std::vector<std::pair<std::string, ImVec2>> cards;  // visible layout cards (name, centre)
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
  int cutDrag = -1;                    // cut (slice index) being dragged in the waveform or the lane
  Model cutBase;                       // the model when the cut drag began
  bool startDrag = false;              // the START flag is being dragged
  ImVec2 startFlag{-1, -1};            // its centre (trace)
  std::vector<std::pair<std::string, ImVec2>> menuPts;  // items of the open region menu (trace)
  std::vector<ImVec2> cutPts, laneCutPts, padChips;     // cut handles, lane edges, pad pitch chips (trace)
};
Ed& ed(const PluginTarget& t);

// J-152: the tool panels' (layouts, pads) header strip: title, tabs / page dots, caption.
constexpr float kToolStrip = 30;

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
               bool* released, bool bipolar = false);
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

// ---- audition / renders / project
void auditionStart(App& a, const PluginTarget& t, Ed& e, int slice);
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
void drawWave(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);     // the region, slices, grid
void drawChopSettings(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w);                  // drawer: chop modes
void drawPresets(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);                     // layout cards
void drawPads(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);
void drawDrawer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);   // Edit drawer
float drawerHeight(const Ed& e);                                                                   // J-152: the open tab's height
void drawGridTools(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);                   // drawer: grid tab
void drawSequence(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);  // J-149: the sequence lane

// ---- J-149 region edits (one undo step each)
void splitRegionUi(App& a, const PluginTarget& t, Ed& e, int region, int parts);
void deleteRegionUi(App& a, const PluginTarget& t, Ed& e, int region);
// Menu items for a region: Split into 2..8, Delete (joins the neighbour). Inside an open popup.
void regionMenuItems(App& a, const PluginTarget& t, Ed& e, int region);
// Opens the region menu for `region` (step = the sequence step it came from, -1 none); drawn by drawRegionMenu.
void openRegionMenu(Ed& e, int region, int step);
void drawRegionMenu(App& a, const PluginTarget& t, Ed& e);
// Marks the sequence as edited by hand (re-chops that keep the regions keep it).
void markEdited(Model& m);

// ---- J-185 slice pitch, the sequence's start, resizing (one undo step each; drags fold)
// Sets a slice's pitch (semitones). mergeKey folds a drag or a run of wheel steps.
void slicePitchUi(App& a, const PluginTarget& t, Ed& e, int slice, float semis, uint64_t mergeKey = 0);
// The mouse wheel over the last item steps the slice's pitch (1 semitone, Shift 0.1) and keeps the wheel from scrolling.
void wheelPitch(App& a, const PluginTarget& t, Ed& e, int slice);
// "+3", "-12", "+0.5", "0".
std::string pitchText(float semis);
// A small pitch readout of a slice: drag up / down (semitones; Shift fine), wheel, double-click
// back to 0, right-click the region menu. `units` adds " st" when it fits `maxW`. Returns its width.
float pitchChip(App& a, const PluginTarget& t, Ed& e, const char* id, ImVec2 p0, float h, float maxW, int slice, bool units);
// The sequence starts at `step` (keepRest: the steps before move to the end; else they are left out).
void startHereUi(App& a, const PluginTarget& t, Ed& e, int step, bool keepRest);
// Cut drags (waveform, lane): begin stores the model; dragging moves cut `cut` (the start of that
// slice) to `frame` (the chop's snap, Alt: none), both neighbours resize; one undo step per drag.
void cutDragBegin(Ed& e, int cut);
void cutDragTo(App& a, const PluginTarget& t, Ed& e, int cut, double frame);
// Merge key of a drag gesture on an item.
inline uint64_t gestureKey(uint64_t base, const Ed& e, int item) {
  return base + (static_cast<uint64_t>(e.gesture & 0xFFFFu) << 16) + static_cast<uint64_t>(item & 0xFFFF);
}

}  // namespace gl::packs::seltmann::sui
