// Gridline — "platter" pack: Platter editor internals shared by its parts.
//   platter_ui.cpp        frame: the controller question, the drop zone, the top
//                         bar, the mapping drawer, shared helpers
//   platter_deck_ui.cpp   the deck: waveforms, platter, transport, pitch, mixer,
//                         hot cues, loops, crossfader, feel, takes
//   platter_tick_ui.cpp   per frame for every deck, open or not: controller
//                         values into parameters (and automation), cues the
//                         deck set, recorded takes into the project, LEDs
// UI thread only.
#pragma once
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../third_party/imgui/imgui.h"
#include "platter.h"

namespace gl::packs::platter::pv {

// Per editor (one per deck), UI only.
struct Ed {
  std::string data;  // PluginState::data the model was parsed from
  std::string path;
  bool hasModel = false;
  Model m;
  seltmann::JobPtr job;  // analysis + peaks of `path`
  // controller question
  bool setup = false;     // shown (first open, or "Controller...")
  int setupProfile = 0;   // index into profiles()
  int setupSide = 0;
  std::string setupDevice;
  bool setupInit = false;
  // mapping drawer
  bool drawer = false;
  int learn = -1;         // control waiting for a MIDI message
  uint32_t learnSeq = 0;
  char presetName[64] = {};
  // gestures
  bool platterDrag = false;
  double dragTurns = 0, lastAngle = 0;
  bool waveDrag = false;
  float waveGrabX = 0;
  double zoomSeconds = 6;  // the scrolling waveform's width in seconds
  bool demoDone = false;
  bool parsed = false;  // the state was parsed once
  std::string lastTrace;
};
Ed& ed(const PluginTarget& t);

// ---- colours / text
ImU32 accent();
ImU32 hotColor(int k, float alpha = 1.0f);
void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s);
void textR(ImDrawList* dl, ImFont* f, float size, ImVec2 rt, ImU32 col, const char* s);
std::string fmtTime(double seconds);
// Rack panel at absolute coordinates: returns the content origin.
ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption = nullptr);
// Flat button (icon and / or text); `on` lights it in the accent.
bool button(const char* id, ImVec2 p0, ImVec2 size, const char* text, bool on, const char* hint, int icon = -1, bool primary = false);
// Pills (segmented choice).
bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur, const char* const* hints = nullptr);
bool paramPills(App& a, const PluginTarget& t, int param, ImVec2 p0, float w, float h, const char* const* labels, int count,
                const char* const* hints = nullptr);
// A padlock (open when `open`), centred.
void padlock(ImDrawList* dl, ImVec2 c, float size, ImU32 col, bool open);

// ---- model edits
void commit(App& a, const PluginTarget& t, Ed& e, const Model& m, const char* label, uint64_t mergeKey = 0);
LiveView live(App& a, const PluginTarget& t);
Generator* deckGen(App& a, const PluginTarget& t);
void send(App& a, const PluginTarget& t, uint8_t kind, uint8_t control = 0, double v = 0, double w = 0);
void press(App& a, const PluginTarget& t, int control, bool down);
void openLoadDialog(App& a, const PluginTarget& t);
std::string mappingDir(App& a);

// ---- parts
void drawSetup(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);
void drawDrawer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);
// The deck under the top bar; returns its height.
float drawDeck(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, const LiveView& lv);
// Heights of the deck's rows (the frame reserves them for the drop zone too).
float deckHeight();

}  // namespace gl::packs::platter::pv
