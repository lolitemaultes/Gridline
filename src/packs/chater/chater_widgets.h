// Gridline - "chater" pack: the editor's look and drawing helpers (pack-local).
// The editor is drawn at 1100 x 680 form units, scaled to the window (skin::Form).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "chater.h"

namespace gl {
class App;
}

namespace gl::packs::chater::ui {

inline constexpr float kW = 1100, kH = 680;
inline constexpr float kTextScale = 1.12f;  // the design's type sizes, as Gridline's fonts draw them

// Colours: the tiles keep their colours in every theme; the surfaces follow it (dark: the
// design's own deep slate, light: the theme's faces).
struct Look {
  bool dark = true;
  ImU32 win, top, tape, panel, panel2, line, cell, cellLine, card, menu, side, knob, ink;
  ImU32 text, text2, text3, wave, accent, accentInk, accentBg, led, white;
  ImU32 tile[8];
  ImU32 ev[6];          // break colours: Repeat, Reverse, Tape, Scrub, Jump, Skip
  ImU32 g[5];           // glitter lanes
  ImU32 b[10];          // blocks
  ImU32 l[5];           // motion
};
const Look& look();

enum Page : int { PgMain = 0, PgFracture, PgGlitter, PgColour, PgSpace, PgMotion, PgHaze };

// Editor state per plugin window.
struct Ui {
  int page = PgMain;
  int openTile = -1;                        // the tile whose chevron opened the page
  bool presets = false, settings = false, dice = false;
  int diceAmount = DiceShake;
  int diceScopes = ScMacros | ScBreaks | ScGlitter;
  std::vector<std::vector<float>> history;  // states before and after each roll
  int historyAt = -1;
  uint32_t rollSeed = 1;
  uint32_t rolledMask = 0;                  // macro tiles the last roll moved
  double rolledAt = -10;
  int menuParam = -1;                       // the knob whose menu is open
  bool wantMenu = false;
  ImVec2 menuPos{0, 0};
  bool devDrives = false;
  int typeParam = -1;
  char search[64] = {};
  int presetTab = 0;                        // 0 factory, 1 yours, 2 favourites
  int lastPreset = 0;                       // factory index shown in the bar (-1 = a user preset)
  std::string presetName = "Init";
  bool saving = false;
  char saveName[64] = {};
  double openedAt = 0;
  int devFrames = 0;
  int lastFrame = -100;                     // the frame the editor was last drawn (a gap: the window opened again)
  float foHist[120] = {};                   // follower history for the Motion page
  int foAt = 0;
  double foLast = 0;
};

struct Ctx {
  App& a;
  const PluginTarget& t;
  skin::Form F;
  ImDrawList* dl;
  const PluginState& st;
  Ui& u;
  const Look& L;
  LiveView live;
  bool hasLive = false;
  double now = 0;
  ImVec2 p(float x, float y) const { return F.p(x, y); }
  float s(float v) const { return F.u(v); }
  double v(int k) const;            // page value
  double eff(int k) const;          // with the current macros applied
};

// App-wide preferences (Settings > Look), kept in <config>/chater.json.
struct Prefs {
  int visuals = 0;       // 0 full, 1 calm, 2 still
  int knobDrag = 1;      // 0 circular, 1 up and down
  std::set<std::string> favourites;
  bool loaded = false;
};
Prefs& prefs(App& a);
void savePrefs(App& a);

// ---- text
ImFont* fBold();
ImFont* fReg();
ImFont* fMono();
ImFont* fMonoBold();
void text(Ctx& c, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* s);  // pos in form units
ImVec2 textSize(Ctx& c, ImFont* f, float size, const char* s);                     // in form units
void textR(Ctx& c, ImFont* f, float size, float right, float y, ImU32 col, const char* s);
void textC(Ctx& c, ImFont* f, float size, float cx, float y, ImU32 col, const char* s);

// ---- shapes (form units)
void rect(Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0);
void rectLine(Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0, float th = 1);

// ---- controls (form units; true on change)
enum KnobKind { KnobTile = 0, KnobArc };
// A parameter knob centred at (cx, cy) with radius r. Arc knobs draw the label and value
// under them when `label` is set. Right-click opens Chater's knob menu.
bool knob(Ctx& c, int param, float cx, float cy, float r, ImU32 col, int kind, const char* label = nullptr, bool valueText = true);
// A clickable box; returns true when clicked (hover state through *hov).
bool hit(Ctx& c, const char* id, float x, float y, float w, float h, bool* hov = nullptr);
// A dark button with text (top bar style); `on` = filled white.
bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on, float fontSize = 12);
// Segmented choice for a choice parameter (labels override; mono). Width 0: fit the labels.
float segmented(Ctx& c, int param, float x, float y, float h, const char* const* labels, int count, float w = 0);
// LED pill toggle bound to a toggle parameter.
float ledToggle(Ctx& c, int param, float x, float y, const char* label);
// Dropdown for a choice parameter (opens a menu).
void dropdown(Ctx& c, int param, float x, float y, float w, float h, const char* label = nullptr);
// The knob menu (drawn once per frame by the editor).
void knobMenu(Ctx& c);
void openKnobMenu(Ctx& c, int param);

// ---- pages
void drawDrawer(Ctx& c, float y0);          // the open page under the compact tiles
void drawSettings(Ctx& c);
void drawPresets(Ctx& c, float y0);
void drawDice(Ctx& c);
// Colour of a parameter's group (the knob arc colour on the pages).
ImU32 groupColour(const Look& L, int param);
// The tile a page opens from (-1: Motion, from its button).
int pageTile(int page);
int tilePage(int tile);
int shownTile(const Ui& u);  // the tile the open page belongs to (-1: none)
const char* breakName(int type);
ImU32 breakColour(const Look& L, int type);

// Edits (one undo step each).
void setParam(Ctx& c, int k, double v, bool merge = false);
void setState(Ctx& c, const std::vector<float>& params, const char* label);
void loadFactory(Ctx& c, int index);

}  // namespace gl::packs::chater::ui
