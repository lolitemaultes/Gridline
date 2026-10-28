// Gridline — "platter" pack: Platter editor internals shared by its parts.
//   platter_ui.cpp        frame: the controller question, the top bar, the
//                         mapping drawer, shared helpers, the waveform bands
//   platter_deck_ui.cpp   the waveform panel (both decks) and a deck: header,
//                         pitch fader, platter, sync / key / modes, transport,
//                         hot cue pads, loops
//   platter_mixer_ui.cpp  the mixer between the decks: trim, EQ with kills,
//                         filter, channel faders with meters, master, assign,
//                         crossfader (curve, cut, reverse), sync source
//   platter_tick_ui.cpp   per frame for every setup, open or not: controller
//                         values into parameters (and automation), cues the
//                         decks set, recorded takes into the project, LEDs
// UI thread only.
#pragma once
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../third_party/imgui/imgui.h"
#include "platter.h"

namespace gl::packs::platter::pv {

// Three-band peaks of a track (low / mid / high, 0..255 per block), made once per
// file on a worker thread; the waveforms colour by them.
inline constexpr int kBandBlock = 256;
struct Bands {
  std::atomic<bool> ready{false};
  int64_t frames = 0;
  double sampleRate = 44100;
  std::vector<uint8_t> lo, mid, hi;
};
std::shared_ptr<const Bands> bandsFor(const SamplePtr& s, const std::string& path);

// Per deck, UI only.
struct DeckUi {
  std::string path;
  seltmann::JobPtr job;               // analysis + peaks of `path`
  std::shared_ptr<const Bands> bands;
  bool platterDrag = false;
  double dragTurns = 0, lastAngle = 0;
  bool waveDrag = false;
  float waveGrabX = 0;
  double strobe = 0;  // strobe dots' drift (turns): stands still at 1.0x
  double rimTurn = 0; // last platter angle (turns), for the strobe
  double padGlow[kHotCues] = {};
};

// Per editor (one per setup), UI only.
struct Ed {
  std::string data;  // PluginState::data the model was parsed from
  bool parsed = false;
  Model m;
  std::array<DeckUi, kDecks> d;
  // controller question
  bool setup = false;     // shown (first open, or "Controller...")
  int setupProfile = 0;   // index into profiles()
  int setupLayout = LayoutTwo;
  int setupSide = 0;      // one-deck layout: the controller side that plays it
  std::string setupDevice;
  bool setupInit = false;
  // mapping drawer
  bool drawer = false;
  int drawerTab = 0;      // 0 deck A, 1 deck B, 2 mixer
  int learn = -1;         // control waiting for a MIDI message
  int learnDeck = 0;
  uint32_t learnSeq = 0;
  char presetName[64] = {};
  double zoomSeconds = 8;  // the scrolling waveforms' width in seconds
  bool demoDone = false;
  int demoPlay = 0;        // dev hook: decks to start once ready (bit d)
  int demoHots = 0;        // dev hook: decks that get a few hot cues once analysed (bit d)
  std::string lastTrace;
};
Ed& ed(const PluginTarget& t);

// ---- colours / text
ImU32 accent();
ImU32 hot();                       // lit states (SYNC, KEY, modes, kills)
ImU32 deckColor(int d, float alpha = 1.0f);
ImU32 hotColor(int k, float alpha = 1.0f);
ImU32 cueColor();
void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s);
void textR(ImDrawList* dl, ImFont* f, float size, ImVec2 rt, ImU32 col, const char* s);
std::string fmtTime(double seconds);
std::string keyName(int key, bool minor);
// Section frame at absolute coordinates with a small caps title; returns the content origin.
ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption = nullptr);
// Flat button (icon and / or text); `on` lights it (the hot role), `primary` fills it with the accent.
bool button(const char* id, ImVec2 p0, ImVec2 size, const char* text, bool on, const char* hint, int icon = -1, bool primary = false);
// Pills (segmented choice).
bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur, const char* const* hints = nullptr);
bool paramPills(App& a, const PluginTarget& t, int param, ImVec2 p0, float w, float h, const char* const* labels, int count,
                const char* const* hints = nullptr);
// A padlock (open when `open`), centred.
void padlock(ImDrawList* dl, ImVec2 c, float size, ImU32 col, bool open);
// Small caps label.
void caps(ImDrawList* dl, App& a, ImVec2 p, ImU32 col, const char* s);
// Small caps label centred on a row of height h at y0 (the baseline of pills and buttons there).
void capsRow(ImDrawList* dl, App& a, float x, float y0, float h, ImU32 col, const char* s);
// Top of caption text centred on a row of height h at y0 (the font buttons use).
float captionY(float y0, float h);

// ---- parameters
double param(App& a, const PluginTarget& t, int k);
bool on(App& a, const PluginTarget& t, int k);
void flip(App& a, const PluginTarget& t, int k);

// ---- model edits / the runtime
void commit(App& a, const PluginTarget& t, Ed& e, const Model& m, const char* label, uint64_t mergeKey = 0);
LiveView live(App& a, const PluginTarget& t, int deck);
MixView mixLive(App& a, const PluginTarget& t);
Generator* deckGen(App& a, const PluginTarget& t);
void send(App& a, const PluginTarget& t, int deck, uint8_t kind, uint8_t control = 0, double v = 0, double w = 0);
void press(App& a, const PluginTarget& t, int deck, int control, bool down);
void openLoadDialog(App& a, const PluginTarget& t, int deck);
// Puts `path` on deck `deck` (deck A: the first line of the data).
void loadOnDeck(App& a, const PluginTarget& t, int deck, const std::string& path);
std::string mappingDir(App& a);
bool deckReady(const Ed& e, int d);
// Dev hook (GRIDLINE_PLATTER_TRACE): prints where a control is ("platter: rect <name> x0 y0 x1 y1") when it moves.
void traceRect(const char* name, ImVec2 p0, ImVec2 p1);

// ---- parts
void drawSetup(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1);
void drawDrawer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv);
// J-158: how far the editor gives way to a short window, 0 (natural size) .. 1 (most compact):
// shorter waveform lanes, platter body and pads row (the mixer follows the deck's height). Set
// before anything is measured or drawn this frame.
void setSqueeze(float f);
float squeeze();
// The waveform panel (both decks' lanes and the phase meter); its height for a layout.
float wavesHeight(bool two);
void drawWaves(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float W, const LiveView* lv);
// One deck in [p0, p1]; `mirror` puts its pitch fader on the right (deck B).
void drawDeck(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv, bool mirror);
float deckPanelHeight();
// The mixer in [p0, p1] (two channels, or deck A's only).
void drawMixer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView* lv, const MixView& mv, bool two);

}  // namespace gl::packs::platter::pv
