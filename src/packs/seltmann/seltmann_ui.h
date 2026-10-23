// Gridline — "seltmann" pack: editor internals shared by the editor's parts.
// One flow: drop audio -> it is analysed -> pick a layout -> play / drag loops out.
//   seltmann_ui.cpp        frame, empty / analysing state, top bar, key controls,
//                          shared helpers, auditions, renders, "send to project"
//   seltmann_wave_ui.cpp   the waveform (bar grid, slices by letter), the slim
//                          region strip for songs, the chop settings (drawer)
//   seltmann_parts_ui.cpp  the layout picker (cards), the pads and the Edit
//                          drawer (slice, chop, steps, grid, sound, loops)
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
  bool patternOn = false;     // pattern audition running
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
};
Ed& ed(const PluginTarget& t);

// ---- colours / text
ImU32 accent();                       // Sample family accent
ImU32 slotColor(int slot, float alpha = 1.0f);
int sliceSlot(const Model& m, int slice);  // first slot letter that plays the slice (-1 none)
void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s);
void textR(ImDrawList* dl, ImFont* f, float size, ImVec2 rightTop, ImU32 col, const char* s);
std::string fmtTime(double seconds);
std::string noteName(int midi);
const char* keyName(int key, bool minor);
const char* kindLabel(int kind);      // "Break", "Loop", ... (translatable)

// ---- small controls (UI-only state)
// Rounded panel with a mono caption; returns the content origin.
ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption = nullptr);
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
void drawGridTools(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);                   // drawer: grid tab

}  // namespace gl::packs::seltmann::sui
