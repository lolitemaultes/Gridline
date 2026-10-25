// Gridline — "plasma" pack: editor internals shared by the plasma_*_ui.cpp
// files. The editor is a fixed 1190 x 740 form (scaled with the window):
//   header 0..68      wordmark, page tabs (OSC / MIX / FX / MATRIX / GLOBAL),
//                     preset row (name, menu, browse, < >), MAIN volume + meter
//   page   68..388    the selected page
//   lower  388..655   MACROS | ENV 1-4 | LFO 1-10 | VELO / NOTE + VOICING
//                     (or the ARP panel; the CLIP editor and the wavetable
//                     editor take 68..655)
//   strip  655..740   wheels, CLIP / ARP toggles, transpose / key / scale /
//                     swing, keyboard, portamento
// Everything is drawn as vectors in Plasma's own palette (dark and light).
// Parameter edits go through pui (undo, automation, MIDI link); data edits
// (tables, shapes, racks, clips) through commit() as one undo step.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "plasma.h"

namespace gl::packs::plasma {
struct Aux;
}
namespace gl::packs::plasma::ui {

constexpr float kW = 1190, kH = 740;
constexpr float kHeadH = 68, kPageY = 68, kLowerY = 388, kStripY = 655;

enum Page : int { PgOsc = 0, PgMix, PgFx, PgMatrix, PgGlobal, PgCount };

// Plasma's palette (resolved for the current theme each frame).
struct Pal {
  bool dark = true;
  ImU32 frame, panel, panelHi, panelLo, head, headOn, well, wellHi, grid, gridStrong, line, lineHi;
  ImU32 text, textDim, textFaint, label, accent, accentDim, accentSoft, mod, modSoft, warn, good, danger;
  ImU32 capTop, capBot, rail, pointer, keyWhite, keyBlack, keyDown;
};
const Pal& pal();
const skin::Skin& skin();

// Editor state per instance (pages, tabs, selections).
struct Ui {
  int page = PgOsc;
  int envTab = 0;
  int lfoTab = 0, lfoFirst = 0;
  int scalarTab = 0;     // 0 velo, 1 note
  int filterTab = 0;
  bool arp = false;      // lower panel shows the ARP editor
  bool clip = false;     // the CLIP editor replaces page + lower panel
  bool wtEdit = false;   // the wavetable editor replaces page + lower panel
  int wtOsc = 0, wtFrame = 0, wtBrush = 0;
  int oscPanel[kOscs] = {};  // oscillator display: 0 waveform, 1 unison settings, 2 options
  int fxRack = 0, fxSel = -1;
  float mxScroll = 0;
  bool mxExpanded = false;
  int arpSlot = 0, clipSlot = 0;
  int clipLane = 0;      // 0 velocity, 1.. automation lanes
  float clipZoom = 1, clipScrollY = 48;
  int lfoSel = -1;       // selected LFO point
  int lfoTool = 0;       // 0 points, 1 steps, 2 ramp up, 3 ramp down
  int envZoom = 0;
  int browser = 0;       // preset browser open (0 = closed)
  std::string browserDir;
  std::string browserFilter;
  std::string importMsg;  // last import report (shown in a popup)
  bool importOpen = false;
  int heldKey = -1;
  std::string devPreset;  // dev (screenshots): factory preset applied on the first frame
  bool prefBubbles = true, prefLive = true, wt3d = false;  // editor preferences (GLOBAL page)
  int modDragSlot = -1;  // matrix slot being dragged on a knob ring
  bool arpEditAll = false, arpPattern = false, clipEditAll = false;
  int clipTopKey = 84, clipSel = -1;
  float clipLastLen = 0.25f;
  int wtScroll = 0, wtGrid = 0, wtGridY = 0, wtZoom = 1;
  // clip editor: snap grid (index), fold, audition, macros, recording
  bool mapping = false;  // oscillator mapping editor (key / velocity zones)
  bool mapVel = false;
  int clipGrid = 2;
  bool clipFold = false, clipAudition = true, clipMacros = false;
  bool clipRec = false, clipOverdub = true, clipExtend = false, clipQuant = false;
  uint32_t clipRecRead = 0;
  double clipRecStart = -1;
  bool clipRecHost = false, clipRecCleared = false;
  float clipRecOn[128] = {}, clipRecVel[128] = {};  // recorded keys held: start + 1 (beats; 0 = none) and velocity
  std::string wtFormula, wtFormulaErr;
};

// Per-frame context.
struct Ctx {
  App& a;
  const PluginTarget& t;
  const skin::Form& F;
  ImDrawList* dl;
  const PluginState& st;
  Model& m;
  Ui& u;
  const Pal& P;
  LiveView live;
  bool hasLive = false;
  ImVec2 p(float x, float y) const { return F.p(x, y); }
  float s(float v) const { return F.u(v); }
};

// ---- values
const ParamDesc* pdesc(int k);  // description of parameter k (null when out of range)
double val(const Ctx& c, int k);
float valf(const Ctx& c, int k);
int vali(const Ctx& c, int k);
void setv(Ctx& c, int k, double v, bool merge = false);
// Writes the data model back (one undo step; `what` folds repeated edits such as drags).
void commit(Ctx& c, const char* label, int what = 0);
std::string ptext(const Ctx& c, int k);  // display text of parameter k

// File name without folder and extension.
std::string fs_stem(const std::string& path);

// ---- text
float font(const Ctx& c, float px);  // scaled font size (form px)
void text(const Ctx& c, float x, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textC(const Ctx& c, float cx, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textR(const Ctx& c, float rx, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textMid(const Ctx& c, float x0, float x1, float cy, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
float textW(const Ctx& c, const char* s, float px = 9.0f, bool bold = false);  // form units

// ---- surfaces
void panelRect(const Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0);
// Graph well (gradient + border).
void displayWellPublic(const Ctx& c, float x, float y, float w, float h);
void wellRect(const Ctx& c, float x, float y, float w, float h, float r = 3);
void hline(const Ctx& c, float x0, float x1, float y, ImU32 col);
void vline(const Ctx& c, float x, float y0, float y1, ImU32 col);
// Module header strip: LED (toggle `enable`, -1 = none), title, optional right text.
void moduleHead(Ctx& c, float x, float y, float w, float h, const char* title, int enable, bool lit = true);
// Small-caps group caption with hairlines on both sides.
void caption(const Ctx& c, float x0, float x1, float y, const char* s);
// Module framing (Serum-like visual language, drawn in Plasma's style):
// a raised container inset by the gutter, so the frame colour shows between neighbours.
constexpr float kGutter = 1.5f;  // half the gap between neighbouring cells
void frameBox(const Ctx& c, float x, float y, float w, float h, ImU32 fill = 0);
// frameBox + a header strip (LED toggle when enable >= 0, title). Returns the body top (y + headH).
float moduleFrame(Ctx& c, float x, float y, float w, float h, float headH, const char* title, int enable, bool lit = true, ImU32 fill = 0,
                  bool centre = false);
// The lighter knob deck under a display (top divider).
void deck(const Ctx& c, float x, float y, float w, float h);
// Inset display: well fill, border, inner top shade, grid (cols x rows, centre line).
void insetFrame(const Ctx& c, float x, float y, float w, float h, int cols = 4, int rows = 2, bool centre = true);
// A label centred at cx that fits maxW (shrinks to 7 px, then clips).
void fitLabel(const Ctx& c, float cx, float y, float maxW, const char* s, ImU32 col, float px = 9.0f, bool bold = true);
// The label of a knob-like cell (same font, colour and fit as knob labels), centred at cx.
void cellLabel(const Ctx& c, float cx, float y, const char* s, float maxW = 52);
// Layout check (GL_PLASMA_LAYOUT_CHECK=1): text and knob rects of the frame, intersections reported.
void layoutLayer(int layer);

// ---- controls (form units; return true on change)
enum KnobFlags { KnobBipolar = 1, KnobNoLabel = 2, KnobSmall = 4, KnobValue = 8 };
bool knob(Ctx& c, int k, float cx, float cy, float r, const char* label, int flags = 0);
// Free (unbound) knob over a float 0..1 (or -1..1 when bipolar); for data edits.
bool knobFree(Ctx& c, const char* id, float& v, float cx, float cy, float r, const char* label, bool bipolar, const char* valueText);
bool hfader(Ctx& c, int k, float x, float y, float w, float h);
bool vfader(Ctx& c, int k, float x, float y, float w, float h);
bool led(Ctx& c, int k, float x, float y, float size = 9);
bool checkBox(Ctx& c, int k, float x, float y, const char* label, float px = 8.5f);
bool toggleBtn(Ctx& c, int k, float x, float y, float w, float h, const char* label, float px = 8.5f);
bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on = false, float px = 8.5f);
// "LABEL value" box (drag vertically / wheel / double-click type).
bool valueBox(Ctx& c, int k, float x, float y, float w, float h, const char* label, const char* override = nullptr);
// Choice parameter as a drop-down field with optional < > arrows.
bool choiceBox(Ctx& c, int k, float x, float y, float w, float h, bool arrows = false, const char* override = nullptr,
               const std::vector<std::string>* names = nullptr);
// Free drop-down: shows `shown`, calls items() inside the open popup.
bool dropField(Ctx& c, const char* id, float x, float y, float w, float h, const char* shown, const std::function<void()>& items,
               bool arrows = false, int* step = nullptr);
// Integer stepper "value ^v".
bool stepper(Ctx& c, int k, float x, float y, float w, float h, const char* override = nullptr);
bool stepperFree(Ctx& c, const char* id, int& v, int lo, int hi, float x, float y, float w, float h, const char* shown = nullptr);
// Tab strip (returns the clicked tab, or -1); `srcBase` >= 0 adds a drag handle per tab (mod source).
int tabs(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<std::string>& names, int cur, int srcBase = -1,
         float px = 9.5f);

// Popup rows (inside BeginPopup / BeginMenu): an ImGui control per parameter.
void paramRows(Ctx& c, const std::vector<int>& ks);
// Small "more" button (three dots) that opens a popup with paramRows(ks) plus extra().
void moreButton(Ctx& c, const char* id, float x, float y, const std::vector<int>& ks, const std::function<void()>& extra = {},
                const char* hint = nullptr);

// ---- modulation
// Drag handle that drops a modulation source on knobs. It draws as a small live
// meter of the source's current output (no button look).
void sourceHandle(Ctx& c, int src, float cx, float cy, float r);
// Accepts a dropped source on the last item for parameter k.
void dropTarget(Ctx& c, int k);
// Adds a matrix slot src -> k (returns the slot or -1 when full).
int addModulation(Ctx& c, int src, int k, float amount);
// Slots that target k (not bypassed first).
std::vector<int> modSlotsFor(const Ctx& c, int k);
// Menus listing sources / destinations; return the chosen id (-1 = none).
bool sourceMenuItems(Ctx& c, int& out);
bool destMenuItems(Ctx& c, int& out);
std::string sourceLabel(int src);
// Warp mode menu tree for oscillator `o` (sets O_WarpNMode).
void warpMenuItems(Ctx& c, int o, int slot);
// Filter type tree for parameter k.
void filterMenuItems(Ctx& c, int k);

// ---- graphs
void drawCurve(const Ctx& c, const Curve& cv, float x, float y, float w, float h, ImU32 col, bool fill, bool bipolar = false);
void gridLines(const Ctx& c, float x, float y, float w, float h, int cols, int rows, bool centre = false);
// Point editor over a curve (x 0..1, y 0..1); returns true when it changed (drag / add / delete).
bool curveEditor(Ctx& c, const char* id, Curve& cv, float x, float y, float w, float h, int gridX, int gridY, int& sel, bool lockEnds,
                 ImU32 col, int* loopPoint = nullptr);

// Styled single-line text field (editor font); true when edited (or Enter with ImGuiInputTextFlags_EnterReturnsTrue).
bool inputField(Ctx& c, const char* id, float x, float y, float w, float h, char* buf, size_t size, const char* hint = nullptr, int flags = 0,
                float px = 9.5f);

// ---- icons
void iconArrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 col);  // 0 left 1 right 2 up 3 down
void iconGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void iconPencil(ImDrawList* dl, ImVec2 c, float s, ImU32 col);
void iconPower(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void iconSave(ImDrawList* dl, ImVec2 c, float s, ImU32 col);
void iconBrowse(ImDrawList* dl, ImVec2 c, float s, ImU32 col);
void iconClose(ImDrawList* dl, ImVec2 c, float s, ImU32 col);
// Source meter: a 270 degree gauge (gap at the bottom, like the knobs). v is
// -1..1 from the top centre when bipolar, else 0..1 from the lower left; idle
// draws the track only.
void iconSourceMeter(ImDrawList* dl, ImVec2 c, float r, float v, bool bipolar, bool idle, ImU32 track, ImU32 lit);

// ---- pages / panels (plasma_*_ui.cpp)
void oscPage(Ctx& c);
void mixPage(Ctx& c);
void fxPage(Ctx& c);
void matrixPage(Ctx& c);
void globalPage(Ctx& c);
void lowerPanel(Ctx& c);
void arpPanel(Ctx& c);
void clipEditor(Ctx& c);
void wtEditor(Ctx& c);
void presetBrowser(Ctx& c);
void mappingPanel(Ctx& c);

// Keyboard preview (channel instruments).
void previewKey(Ctx& c, int key, bool on);
// Opens the Serum 2 import file dialog; the report is shown in a popup.
void importDialog(Ctx& c);
bool importFile(Ctx& c, const std::string& path);
// Edits the current state outside a frame (file dialogs): fn changes the
// parameters and / or the model; one undo step.
void editState(App& a, const PluginTarget& t, const char* label, const std::function<void(PluginState&, Model&)>& fn);
// Opens the file dialog; ok runs with the chosen path.
void openFile(Ctx& c, const char* title, const std::vector<std::string>& exts,
              const std::function<void(App&, const PluginTarget&, const std::string&)>& ok);
// Prepared data of the current state (tables, samples), cached by data.
std::shared_ptr<const Aux> auxOf(const Ctx& c);
// Applies a factory / data preset.
void applyPreset(Ctx& c, const std::vector<std::pair<int, float>>& values, const std::string& data, const std::string& name);

}  // namespace gl::packs::plasma::ui
