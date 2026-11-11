// Gridline — "plasma" pack: editor internals shared by the plasma_*_ui.cpp
// files. The editor is a fixed 1280 x 800 form (scaled with the window), laid
// out to follow the signal and to show one module at a time:
//   top bar    0..44      Library, Save as, the import note, Undo / Redo, Setup,
//                         Output and its meter (the sound's name is in the plugin
//                         header above, kept in step)
//   panels     44..764    Panels: four numbered columns (Sources, Tone, Volume, Effects,
//                         x 0..1000); Patch: the same sound as boxes and wires
//   stage      48..764    a full-width view (Links, envelope and LFO editors, ...) over
//                         the columns, header 48..82 with Back; cards on a 12 unit grid
//   dock       x 1000..1280, y 44..764: the macros, the other motion families as
//                         rows that open one at a time, the links of the control
//                         under the pointer
//   play bar   764..800   voices, glide, transpose, arpeggiator, phrases, More...
//                         (bend, key, groove, key zones), keyboard drawer
// Overlays (Library, Table editor, Phrases) cover the stage and the dock.
// Everything is drawn as vectors in Gridline's roles (dark and light).
// Parameter edits go through pui (undo, automation, MIDI link); data edits
// (tables, shapes, racks, phrases) through commit() as one undo step.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
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

constexpr float kW = 1280, kH = 800;
// The full-width views sit right under the top bar (the panels' columns use kColY..kColB).
constexpr float kTopH = 46.2f, kRailY = 46.2f, kRailB = 46.2f, kStageHeadY = 50, kStageY = 84, kStageB = 759.1f;  // the design: a 52 px top bar, a 46 px play bar
// The full-width views' width: kW on the tabs, 1000 in the Patch view (its inspector at the right).
inline float kStageW = 1000;
constexpr float kDockX = 1000, kDockW = 280, kPlayY = 759.1f;
// overlays (Library, Table editor, Phrases): x 0..kW, y kPageY..kStripY
constexpr float kPageY = kRailB, kStripY = kPlayY;

// The views of the stage. The values are not saved.
enum View : int {
  VLayer = 0,  // Layer 1-3 (Ui::layer)
  VRootTex,    // Root and Texture
  VFilters,
  VAmp,        // Amp and voices
  VRouting,
  VEffects,
  VEnv,        // Envelope 1-4 (Ui::envTab)
  VLfo,        // LFO 1-10 (Ui::lfoTab)
  VKeys,       // Key and touch
  VLinks,
  VArp,
  VZones,      // Key zones
  VSetup,
  VVoices,     // Play: notes, glide, bend
  VPhrases,    // overlays from here: they cover the stage and the dock
  VTable,      // Table editor (Ui::wtOsc)
  VLibrary,
  VCount
};
constexpr bool isOverlay(int v) { return v >= VPhrases; }

// Modulation source families: each has its own role colour (dock rows, link bands, Links rows).
enum Family : int { FamEnv = 0, FamLfo, FamMacro, FamKey, FamChance, FamCount };
int familyOf(int src);

// Plasma's palette (resolved for the current theme each frame).
struct Pal {
  bool dark = true;
  ImU32 frame, panel, panelHi, panelLo, head, headOn, well, wellHi, field, grid, gridStrong, line, lineHi;
  ImU32 text, textDim, textFaint, label, accent, accentDim, accentSoft, mod, modSoft, warn, good, danger;
  ImU32 capTop, capBot, rail, pointer, keyWhite, keyBlack, keyDown, boxEdge;
  ImU32 accentText, modText;  // J-202: the accent / mod colours as text (4.5:1 on the surfaces they sit on)
  ImU32 hot, knob;             // lit marks (orange), the value ring of dials and bars
  ImU32 fam[FamCount], famText[FamCount];  // modulation families (graphics, text)
};
const Pal& pal();
const skin::Skin& skin();

// Editor state per instance (view, selections). Never saved.
struct Ui {
  int view = VLayer, back = VLayer;  // the view, and where Esc / Close return from an overlay
  int layer = 0;                     // Layer view: 0..2
  int envTab = 0;
  int lfoTab = 0;
  int wtOsc = 0, wtFrame = 0, wtBrush = 0;
  int layerCard = 0;     // Grains / Spectral: 0 scatter or band, 1 region
  int fxRack = 0, fxSel = -1;  // Effects: the lane and the selected slot
  float mxScroll = 0;
  int linkGroup = 0;     // Links: 0 by source, 1 by target, 2 slot order
  int linkSel = -1;      // Links: the selected slot
  std::string linkFilter;
  int linkSrc = -1;      // Link mode: the armed source (-1 = off)
  int focusK = -1;       // the control last under the pointer (its links show in the dock)
  int hoverSrc = -1, hoverSrcNext = -1;
  int hoverK = -1, hoverKNext = -1;
  unsigned moreOpen = 0;  // the card turned to its details (cardMore), 0 = none
  bool patch = false;     // the Patch view (a graph of the same sound) instead of the panels
  bool full = false;      // Full: every control of each column in place; Simple folds the details
  bool fullSet = false;   // full taken from the setting (once per editor)
  int src = 0;            // Sources column: 0..2 Layer 1-3, 3 Root, 4 Texture
  float colScroll[4] = {}, colContent[4] = {};  // the columns' scroll offsets and content heights (last frame)
  std::vector<unsigned> secOpen;  // Simple: sections opened to their details
  std::vector<unsigned> blockClosed, blockMore;  // blocks folded / showing their details
  std::map<unsigned, float> blockH;   // each block's height (last frame), for its card
  // Patch view (editor state only, never saved in the sound)
  float pzoom = -1, ppanX = 0, ppanY = 0;  // zoom -1: fit the patch on the next frame
  int pSel = -1;          // selected node (PatchNode id), -1 none
  int pLink = -1;         // selected link slot, -1 none
  int pHover = -1;        // node under the pointer (last frame): follow the sound
  bool pShowSound = true, pShowMove = false, pShowFx = true, pShowNotes = false;  // Patch: what to show (pShowFx: the sends)
  bool pAll = false;      // Patch inspector: every setting of the box unfolded
  int pDragPin = -1, pDragNode = -1;  // a wire being dragged out of a pin or a box
  int pAddFrom = -1;      // the box a wire was dropped from onto empty canvas (the search menu)
  std::string pWhy;       // why the last connection was refused (shown for a moment)
  float pWhyT = 0;
  float pScroll = 0, pContent = 0;  // the inspector's scroll
  int pFrames = 0;        // frames the Patch view has drawn
  bool devMenu = false;  // dev (screenshots): open the first block's menu
  int devPSel = -1;      // dev (reach driver): the Patch box to select
  int devPatch = 0;       // dev (screenshots): 1 first link, 2 add menu, 3 LFO 1 hovered, 4 link search, 5 Filter 1 selected
  bool drawer = false;   // the keyboard drawer is open
  float drawerT = 0;      // its slide, 0 (in) .. 1 (out)
  float stageScroll = 0;  // a full-width view's scroll while the drawer shortens it
  // Synth and Effects tabs: the parts sidebar at the right, the block grid
  float sbW = 264, sbCur = -1;  // the sidebar's width (resizable) and its drawn width as it slides
  bool sbOpen = true;
  int sbLib = 0;                  // Library: 0 tables, 1 LFO shapes, 2 sounds
  std::string sbFilter;
  std::string libGroup[2];         // Library: the folder shown (tables, shapes); "" = the first
  int libSel[2] = {-1, -1};        // the chosen item (tables: catalog index, shapes: 100 LFO, 200 Root, 300 Texture + n)
  int libInto = 0;                 // Load into: layer (tables) or LFO (shapes)
  float libScroll = 0, libContent = 0;
  int libChip = 0;                 // the quick filter chip (0 = all)
  float sbScroll = 0, sbContent = 0, sbModScroll = 0, sbModContent = 0;
  float synthScroll = 0, synthContent = 0, modY = 0;  // the Synth tab's scroll, the Modulation section's top
  float fxScroll[3] = {}, fxContent[3] = {};
  std::vector<int> order;         // the blocks' visual order (ids, see plasma_synth_ui.cpp); never the sound's
  std::map<int, ImVec2> blockPos; // each block's drawn place as it glides to its slot
  uint32_t added = 0;             // motion blocks added in this editor (shown though not linked yet)
  int macrosShown = 4;
  std::vector<int> moreIds;       // blocks showing their details (More): wider, like Full
  std::map<int, float> contentH;  // each block's content height (last frame): its row grows to fit
  std::map<int, float> picFit;    // each block's picture slack in its section row (fitPic)
  int movePage = 0;               // Movement: the page of blocks shown
  ImVec2 dropAt{-1, -1};          // where a part was dropped (content units): its block glides from there
  // the advanced panel a block opens (double-click): it grows from the block, Back shrinks it
  bool large = false;             // a block's own page over the Synth or Effects tab (Root and Texture, Filters, an effect)
  bool closing = false;
  float expandT = 1;              // 0 at the block .. 1 the whole area
  ImVec4 expandFrom{0, 0, 0, 0};  // the block's screen rectangle (x0, y0, x1, y1), zero when none
  bool devPlay = false;           // dev: hold a note, LFO 1 moving Layer 1's position (live displays)
  std::map<int, bool> wt3d;       // a layer's display shows the frames in depth
  int devDrag = -1;               // dev (screenshots): draw a part being dragged from the sidebar
  int devLayers = 0;              // dev: the Synth tab with this many layers on
  bool devScrollMod = false;      // dev: scroll the Synth tab to the Modulation section
  bool reachAll = false;          // dev (reach driver): every block shown, used or not
  int dockOpen = FamEnv; // the motion family opened in the dock (-1 = none)
  int arpSlot = 0, clipSlot = 0;
  int clipLane = 0;      // 0 velocity, 1.. automation lanes
  float clipZoom = 1, clipScrollY = 48;
  int lfoSel = -1;       // selected LFO point
  int lfoTool = 0;       // 0 points, 1 steps, 2 ramp up, 3 ramp down
  bool lfoAdv = false;   // LFO page: the Advanced row open
  bool envAdv = false;   // Envelope page: the Advanced row open
  // the table editor, per editor: a factory or file table shown from a copy (re-read when the
  // table or the undo position changes) and a drawn edit waiting for the gesture to end
  std::vector<std::vector<float>> wtView;
  std::string wtViewKey;
  bool wtPending = false;
  std::map<int, std::pair<float, float>> pOffsets;  // Patch view: the user's moves of boxes (Tidy clears them)
  std::set<int> pShownMoves;                        // Patch view: movement sources added by hand
  int envZoom = 0;
  std::string browserDir;
  std::string browserFilter;
  int heldKey = -1;
  std::string devPreset;  // dev (screenshots): factory preset applied on the first frame
  bool prefBubbles = true, prefLive = true;  // editor preferences (Setup)
  int modDragSlot = -1;  // link slot being dragged on a dial ring
  bool arpEditAll = false, arpPattern = false, clipEditAll = false;
  int clipTopKey = 84, clipSel = -1;
  float clipLastLen = 0.25f;
  int wtScroll = 0, wtGrid = 0, wtGridY = 0, wtZoom = 1;
  bool wt3dEdit = false, wtPhases = false;  // the table editor: the whole table in depth; the harmonics as phases
  bool mapVel = false;   // Key zones: velocity axis
  int clipGrid = 2;
  bool clipFold = false, clipAudition = true, clipMacros = false;
  bool clipRec = false, clipOverdub = true, clipExtend = false, clipQuant = false;
  uint32_t clipRecRead = 0;
  double clipRecStart = -1;
  bool clipRecHost = false, clipRecCleared = false;
  float clipRecOn[128] = {}, clipRecVel[128] = {};  // recorded keys held: start + 1 (beats; 0 = none) and velocity
  std::string wtFormula, wtFormulaErr;
  int reachStep = -1;    // dev (GL_PLASMA_REACH): the driver's step
  int devLinkArm = -1, devFrames = 0;  // dev (GL_PLASMA_VIEW=linkmode): arm Link mode after a few frames
};
// dev (screenshots, tests): puts the editor on a named view ("layer2", "env0", "linkmode", ...).
void devView(Ui& u, const std::string& name);
// Opens view v (an overlay remembers where to return).
void openView(Ui& u, int v);
void closeOverlay(Ui& u);

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
// The block views that live in the panel columns (the others open full width over them).
inline bool isPanelsView(int v) { return v == VLayer || v == VRootTex || v == VFilters || v == VAmp || v == VEffects; }
// Panels: four numbered columns over x 0..kStageW, y kColY..kColB (the Movement column is the dock).
constexpr float kColY = 52, kColB = 756;
constexpr float kColX[4] = {8, 368, 600, 792}, kColW[4] = {344, 216, 176, 200};
// A column's section: its title row (title, a short summary, More / Less in Simple when it has
// details). Moves y below the row. True when its details show (always in Full).
bool section(Ctx& c, const char* id, float x, float& y, float w, const char* title, const std::string& summary, bool hasDetails);
void sectionEnd(Ctx& c, float x, float& y, float w);
// A block: one synth part as a card with a title bar (family strip, light, name, its live picture
// when folded, More, open large, fold). blockBegin returns true when the body shows; draw the
// body from y, then blockEnd. Shared by the panels and the Patch view.
struct BlockOpts {
  int enable = -1;        // its on / off parameter
  ImU32 fam = 0;          // family colour (the strip)
  int view = -1, viewTab = 0;  // the large editor a click on the name opens
  bool canMore = true;    // has details for More
  bool drag = false;      // the name is a drag handle (the caller makes it a drag source)
  bool selected = false;
  bool forceOpen = false, forceClosed = false;
  bool canRemove = false; // an x in the title bar (Block::removeClicked)
  int grip = -1;          // a modulation source: its link dot sits in the title bar
  const char* hint = nullptr;
  float frameH = 0;       // a set frame height (a section row): at least this tall
  bool ownOpen = false;   // the caller opens the advanced panel on a double-click (not the name)
  const char* sub = nullptr;  // what it is (dim, after the name): a table, a type, a shape
  std::function<void()> subMenu;  // a click on `sub` opens these menu items
  std::function<float(float, float)> tools;  // draws its tools right-aligned at (rx, y), returns the new rx
  // the block menu: its parameters (Reset, Copy, Paste between blocks of one kind), the Patch box it
  // shows, its section (Remove from ...), what it does, and its own items (a layer's wavetable)
  int pLo = -1, pN = 0, pKind = 0;
  int patchNode = -1;
  const char* section = nullptr;
  const char* about = nullptr;
  std::function<void()> extraMenu;
};
// Block kinds for Copy / Paste (blocks of one kind share their parameter layout).
enum BlockKind : int { BkNone = 0, BkLayer, BkRoot, BkTexture, BkFilter, BkEnv, BkLfo };
struct Block {
  float x = 0, y0 = 0, w = 0, dimFrom = 0, frameH = 0;
  unsigned key = 0;
  bool open = true, more = false, prevMore = false, titleClicked = false, removeClicked = false, openClicked = false;
};
bool blockBegin(Ctx& c, Block& b, const char* id, float x, float& y, float w, const char* title, const BlockOpts& o,
                const std::function<void(float, float, float, float)>& mini = {});
void blockEnd(Ctx& c, Block& b, float& y);
void blockFrame(const Ctx& c, float x, float y, float w, float h, ImU32 fam, bool sel, bool dim);
// True while a block shows its details (its More, or Full).
bool showDetails(const Ctx& c);
// ---- the look (plasma_look_ui.cpp). Sizes in design pixels (a 1440 x 900 window): M() gives form
// units, T() a text size for text() (which scales by 1.25).
constexpr float kMk = 8.0f / 9.0f;
constexpr float M(float px) { return px * kMk; }
constexpr float T(float px) { return px * kMk / 1.25f; }
void layoutBox(const Ctx& c, float x, float y, float w, float h, const char* what);
float glowK(const Ctx& c);              // how much glow (lower than the design, less in light)
ImU32 partCol(const Ctx& c, int id);    // a block's family colour (block ids, effects 400+)
ImU32 fxCol(const Ctx& c, int type);      // an effect's colour, by what it does
ImU32 barBg(const Ctx& c);              // the top and play bars, the sidebar
void wellLook(const Ctx& c, float x, float y, float w, float h);
void gridLook(const Ctx& c, float x, float y, float w, float h, int nx, int ny, bool mid);
void glowLine(const Ctx& c, const std::vector<ImVec2>& pts, ImU32 col, float lw, float glow = 1.0f);  // screen points, lw in form units
void fillUnder(const Ctx& c, const std::vector<ImVec2>& pts, float baseY, ImU32 col, float a0, float a1, float topY);  // screen
void glowDot(const Ctx& c, ImVec2 at, float r, ImU32 col);  // screen centre, r in form units
void capsText(const Ctx& c, float x, float y, const char* s, ImU32 col, float px, float track);
float capsW(const Ctx& c, const char* s, float px, float track);
// A section head (spaced caps, a hint, a note at the right); returns the x left of the note.
float sectionHead(const Ctx& c, float x, float y, float w, const char* title, const char* hint, const char* right);
int segPick(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<const char*>& labels, int cur, bool mono, float px);
bool btnLook(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool quiet, bool on, float px);
// The block menu's look (inside an ImGui popup): a caps head, a rule, an item with a glyph (1 open,
// 2 power, 3 fold, 4 more, 5 patch, 6 question, 7 bin), its name and its shortcut; true on a click.
void menuHead(const Ctx& c, const char* title);
void menuRule(const Ctx& c);
bool menuItemLook(const Ctx& c, const char* id, const char* label, const char* keys, int glyph, bool danger = false, bool enabled = true);
void chipLook(const Ctx& c, float x, float y, float w, float h, const char* label, bool dotOn);
void modChip(const Ctx& c, float rx, float y, const char* label, ImU32 col);  // right-aligned at rx
extern ImU32 gKnobCol;  // the value colour of the dials being drawn (a block's family), 0 = accent
struct KnobColour {
  ImU32 was;
  explicit KnobColour(ImU32 c) : was(gKnobCol) { gKnobCol = c; }
  ~KnobColour() { gKnobCol = was; }
  KnobColour(const KnobColour&) = delete;
  KnobColour& operator=(const KnobColour&) = delete;
};
// Layout check: text ImGui draws in a window (a menu) that runs past the window's right edge.
void layoutWindowText(ImVec2 at, const char* s, float wrap);
// No-scroll sizing: a block in a section row of set height lays its picture out at base + the
// row's slack (Simple only), so the block fills its row exactly; 0 elsewhere.
extern float gPicFit;
extern bool gLayoutSettling;  // a block's picture fit moved this frame: the layout check skips it
extern bool gPicUsed;         // the block being drawn has a picture that takes the slack
inline float fitPic(const Ctx& c, float base) {
  if (showDetails(c)) return base;
  gPicUsed = true;
  return std::max(36.f, base + gPicFit);
}
// The main dial row of a block body (Simple, in a section row): every block's row sits at the same
// distance from its bottom, so the dials of a row share a baseline. dialRow returns its top.
constexpr float kDialRowH = 72;
extern float gDialBase, gDialPushed;
inline float dialRow(float y) {
  if (gDialBase <= y) return y;
  gDialPushed += gDialBase - y;
  return gDialBase;
}
// Double-click targets: the block's title and pictures register here each frame.
void markPicture();
bool isPicture(unsigned id);
void clearPictures();
// Closes the open advanced panel (or overlay), shrinking it back into its block.
void beginClose(Ui& u);
void finishClose(Ui& u);
// Shows every detail while it lives (the Patch inspector).
struct DetailsOn {
  bool prev;
  DetailsOn();
  ~DetailsOn();
};
// Block bodies (Simple: the main controls; details with More or in Full). Each returns the y under it.
float layerColumn(Ctx& c, int o, float x, float y, float w);
float rootColumn(Ctx& c, float x, float y, float w);
float textureColumn(Ctx& c, float x, float y, float w);
float filterColumn(Ctx& c, int f, float x, float y, float w);
float ampColumn(Ctx& c, float x, float y, float w);
float lfoBlockBody(Ctx& c, int l, float x, float y, float w);
float envBlockBody(Ctx& c, int e, float x, float y, float w);
void fxLaneFoot(Ctx& c, int r, float x, float y, float w);
float fxLane(Ctx& c, int rack, float x, float y, float w);
void cycleMiniPublic(Ctx& c, int o, float x, float y, float w, float h);
void rootWavePublic(Ctx& c, int shape, float x, float y, float w, float h, ImU32 col);
void textureMiniPublic(Ctx& c, float x, float y, float w, float h);
void filterCurvePublic(Ctx& c, int f, float x, float y, float w, float h);
void tableMenuPublic(Ctx& c, int o);
void exportTablePublic(Ctx& c, int o);  // saves layer o's table as a WAV (a save dialog)
// The theme button: the switch to light (or dark) is made at the start of Plasma's next frame,
// outside its skin scope, as the Options menu makes it (with Keep / Revert).
void requestThemeSwitch(bool toLight);
void applyThemeSwitch(App& a);
bool devThemePress();  // dev (GL_PLASMA_THEME_AT): the button pressed on these editor frames
void devThemeDump(App& a);  // dev (GL_PLASMA_THEME_DUMP): the palettes and ImGui colours at one editor frame
const TableCatalog& tableCatalogUi(const Ctx& c, bool rescan = false);  // every table (built once, again on request)
void chooseTableEntry(Ctx& c, int o, const TableEntry& e);              // loads it into layer o (one undo step)
std::string layerSourceName(const Ctx& c, int o);
bool layerHasTable(const Ctx& c, int o);
void layerPicture(Ctx& c, int o, float x, float y, float w, float h);
void tableDepthAt(Ctx& c, int o, float x, float y, float w, float h);  // layer o's table in depth (no well)
float rootWaveAt(int shape, float t);
Curve lfoShapePublic(const Ctx& c, int l);
// ---- blocks (plasma_blocks_ui.cpp): the Synth tab's bodies in Simple, in a body rect
void layerBlockBody(Ctx& c, int o, float x, float y, float w, float h);
void rootBlockBody(Ctx& c, float x, float y, float w, float h);
void textureBlockBody(Ctx& c, float x, float y, float w, float h);
void filterBlockBody(Ctx& c, int f, float x, float y, float w, float h);
void envBlockBodyDesign(Ctx& c, int e, float x, float y, float w, float h, int nx);
void lfoBlockBodyDesign(Ctx& c, int l, float x, float y, float w, float h);
void macroBlockBodyDesign(Ctx& c, int first, float x, float y, float w, float h);
void fxTypeGlyphPublic(Ctx& c, int type, float x, float y, float w, float h, ImU32 col);
bool addFxAt(Ctx& c, int type, int rack, int beforeSlot);
const char* fxName(int type);  // an effect type's display name (untranslated)  // false when every effect place is in use
// The tabs: Synth (the block grid), Effects, Links, Play, Setup.
enum Tab : int { TabSynth = 0, TabEffects, TabLinks, TabPlay, TabSetup, TabCount };
int tabOf(int view);
void synthTab(Ctx& c);
void effectsTab(Ctx& c);
void sidebar(Ctx& c);
float sidebarW(const Ui& u);  // the sidebar's drawn width (0 on the other tabs)
void libraryPage(Ctx& c);  // the Library (tables, shapes, sounds) as a full panel
void playPage(Ctx& c);  // the Play tab (voices, glide, wheels, arpeggiator, phrases, key zones)
void voicesView(Ctx& c);
std::string dockName(int src);
int linksOf(const Ctx& c, int src);
int linksUsed(const Ctx& c);
void sourceRow(Ctx& c, int src, float x, float y, float w, bool sel, int view, const char* name, float h = 26);
void macroRenamePopup(Ctx& c, int i);
// The four columns' contents from (x, y), w wide; each returns the y under its last control.
float sourcesColumn(Ctx& c, float x, float y, float w);
float toneColumn(Ctx& c, float x, float y, float w);
float volumeColumn(Ctx& c, float x, float y, float w);
float effectsColumn(Ctx& c, float x, float y, float w);
// A scrolling body x..x+w, y0..y1: fn draws the content from (x, y) w wide in a scrolled context
// and returns the y under it; scroll and content keep the state between frames.
void scrollBody(Ctx& c, float x, float y0, float w, float y1, float& scroll, float& content,
                const std::function<float(Ctx&, float, float, float)>& fn);
// Small live pictures and chain edits shared by the panels and the Patch view.
void sourceMini(Ctx& c, int i, float x, float y, float w, float h);  // 0..2 layers, 3 Root, 4 Texture
void lfoMini(Ctx& c, int l, float x, float y, float w, float h);
void envMini(Ctx& c, int e, float x, float y, float w, float h);
bool routeFeeds(const Ctx& c, int r, int f);
void routeToggleFeed(Ctx& c, int r, int f);
void fxAddMenu(Ctx& c, int rack);
void fxRemove(Ctx& c, int slot);
void fxDropOn(Ctx& c, int slot);
void fxToRack(Ctx& c, int slot, int rack);
bool fxAdd(Ctx& c, int type, int rack);  // false when every effect place is in use
void patchView(Ctx& c);
void stageView(Ctx& c);
// The keyboard drawer's height as it slides (form units, 0 when in).
constexpr float kDrawerH = 112;
inline float drawerH(const Ui& u) { return kDrawerH * u.drawerT; }
// The editor of link slot s (the Links view's, reused by the Patch inspector); returns the y under it.
float linkEditor(Ctx& c, int s, float x, float y, float w);
// dev (reach driver): selects in the Patch view the block that panel view u.view shows.
void patchSelectFor(Ui& u);

// ---- values
const ParamDesc* pdesc(int k);  // description of parameter k (null when out of range)
double val(const Ctx& c, int k);
float valf(const Ctx& c, int k);
int vali(const Ctx& c, int k);
void setv(Ctx& c, int k, double v, bool merge = false);
// The model (c.m) and parameters ps written together as one undo step (adding or removing an effect).
void commitParams(Ctx& c, const char* label, const std::vector<std::pair<int, double>>& ps);
// Writes the data model back (one undo step; `what` folds repeated edits such as drags).
void commit(Ctx& c, const char* label, int what = 0);
std::string ptext(const Ctx& c, int k);  // display text of parameter k

// File name without folder and extension.
std::string fs_stem(const std::string& path);
// Display name of a file reference: an embedded entry shows the name it had in the preset.
std::string refStem(const Model& m, const std::string& ref);

// J-202: the stereo output meter (header, MIX page)
void drawMeter(Ctx& c, float x, float y, float w, float h);
// J-202: a two-way segmented switch; returns true when clicked (flip it).
bool seg2(Ctx& c, const char* id, float x, float y, float w, float h, const char* a, const char* b, bool second, const char* hint, float px = 8.0f);  // px: ty::kCap
// ---- text
// J-202 type scale (form px; 8 px is the floor at 100 %):
//   kCap   captions: group / field / axis labels (bold, label colour)
//   kLabel control labels under knobs and on buttons (bold)
//   kValue values: fields, steppers, readouts (mono face, text colour)
//   kTitle module titles in header strips (bold)
//   kName  the preset name
namespace ty {
constexpr float kCap = 8.0f, kLabel = 8.5f, kValue = 9.0f, kTitle = 9.5f, kName = 10.5f;
}
float font(const Ctx& c, float px);  // scaled font size (form px)
float ctlPx(float h);                // the text size of a control h form units tall (10..12)
// While one is alive, text is set in the mono face (values).
struct MonoText {
  MonoText();
  ~MonoText();
  MonoText(const MonoText&) = delete;
  MonoText& operator=(const MonoText&) = delete;
};
void text(const Ctx& c, float x, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textC(const Ctx& c, float cx, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textR(const Ctx& c, float rx, float y, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
void textMid(const Ctx& c, float x0, float x1, float cy, const char* s, ImU32 col, float px = 9.0f, bool bold = false);
// J-128: bold text at form x beside a field box (boxY, boxH), on the baseline
// of the field's own value (9 px bold, centred in the box).
void labelBeside(const Ctx& c, float x, float boxY, float boxH, const char* s, ImU32 col, float px = 8.5f, bool bold = true);
float textW(const Ctx& c, const char* s, float px = 9.0f, bool bold = false);  // form units

// ---- surfaces
void panelRect(const Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0);
// Graph well (gradient + border).
void displayWellPublic(const Ctx& c, float x, float y, float w, float h);
void wellRect(const Ctx& c, float x, float y, float w, float h, float r = 3);
void hline(const Ctx& c, float x0, float x1, float y, ImU32 col);
void textHeavy(const Ctx& c, float x, float y, const char* s, ImU32 col, float px);  // bold, a step heavier (block titles)
void vline(const Ctx& c, float x, float y0, float y1, ImU32 col);
// Module header strip: LED (toggle `enable`, -1 = none), title, optional right text.
void moduleHead(Ctx& c, float x, float y, float w, float h, const char* title, int enable, bool lit = true);
// Small-caps group caption with hairlines on both sides.
void caption(const Ctx& c, float x0, float x1, float y, const char* s);
// A raised cell inset by the gutter, so the ground shows between neighbours.
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
// layout check: the current clip rect is a scrolling area (text cut at its edges is scrolled
// out of view, not cut); a control's body box (boxes must not overlap).
void layoutScroll(const Ctx& c);
void layoutBox(const Ctx& c, float x, float y, float w, float h, const char* what);

// ---- controls (form units; return true on change)
enum KnobFlags { KnobBipolar = 1, KnobNoLabel = 2, KnobSmall = 4, KnobValue = 8 };
bool knob(Ctx& c, int k, float cx, float cy, float r, const char* label, int flags = 0);
// Free (unbound) knob over a float 0..1 (or -1..1 when bipolar); for data edits.
// `d` describes the value the knob shows (v = d.toNormalized(value)): its
// default for the double-click reset and the type-in (J-136).
bool knobFree(Ctx& c, const char* id, float& v, float cx, float cy, float r, const char* label, bool bipolar, const char* valueText,
              const ParamDesc& d);
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
// `placeholder`: `shown` is an empty-state text (set in the faint tone).
bool dropField(Ctx& c, const char* id, float x, float y, float w, float h, const char* shown, const std::function<void()>& items,
               bool arrows = false, int* step = nullptr, bool placeholder = false);
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
// A labelled button ("More") that opens the same popup; returns true while the popup is open.
bool moreLink(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, const std::vector<int>& ks,
              const std::function<void()>& extra = {}, const char* hint = nullptr);

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

// ---- the new controls (K-164)
// A value bar: label at the left (Inter), value at the right (mono), fill from the left
// (or the centre for a bipolar value), link bands along the lower edge in the source's
// family colour. Drag sideways (Shift: fine), wheel, double-click to type, right-click menu.
enum BarFlags { BarCentre = 1, BarNoLabel = 2, BarDim = 4, BarField = 8 };  // BarField: a plain value field (drag it), no fill
bool bar(Ctx& c, int k, float x, float y, float w, float h, const char* label, int flags = 0);
// Any parameter by its kind: a toggle is a chip with a lit dot, a choice a labelled field,
// anything else a value bar.
bool field(Ctx& c, int k, float x, float y, float w, float h, const char* label);
// A column of fields (rowH apart); `cols` columns over w. Returns the y below the last row.
struct FieldItem {
  int k;
  const char* label;
};
float fieldGrid(Ctx& c, float x, float y, float w, int cols, float rowH, const std::vector<FieldItem>& items, float h = 20);
// A card's details, in place: a More... button in the card's title row turns the card (x, y, w, h)
// to its secondary settings and back. True while turned (the caller then skips its main content).
// One plain sentence saying what control k does (empty when none is written).
std::string paramSentence(int k);
bool cardMore(Ctx& c, const char* id, float x, float y, float w, float h, const std::vector<FieldItem>& items, const char* hint);
// A toggle parameter as a chip (lit dot + label).
bool chip(Ctx& c, int k, float x, float y, float w, float h, const char* label);
// A choice parameter as a segmented switch (labels in order of the stored index).
bool segments(Ctx& c, int k, float x, float y, float w, float h, const std::vector<const char*>& labels);
// A card: a raised cell with its title (sentence case) and an optional on / off dot.
// Returns the content top.
float card(Ctx& c, float x, float y, float w, float h, const char* title, int enable = -1);
// The stage header: the view title at the left; returns the x where the header controls start.
// The advanced panel (a block grown to the page): its frame and head; content from kAdvBodyY.
constexpr float kAdvX0 = M(20), kAdvX1 = kW - M(20), kAdvIX0 = M(38), kAdvIX1 = kW - M(38);
// The Library: a full panel (a header with the search and the kind, then folders | items | the chosen one)
constexpr float kLibX0 = M(20), kLibX1 = kW - M(20), kLibY0 = kTopH + M(16), kLibY1 = kPlayY - M(16), kLibBodyY = kLibY0 + M(56),
                kLibSideW = M(236), kLibInfoX = kLibX1 - M(320);
constexpr float kAdvHeadY = kTopH + M(44), kAdvLineY = kTopH + M(72), kAdvBodyY = kTopH + M(89);
float advHead(Ctx& c, const char* title, const char* sub, ImU32 fam, int src);
float stageHead(Ctx& c, const char* title);
// The flat display: a well with a hairline grid (cols x rows).
void display(const Ctx& c, float x, float y, float w, float h, int cols = 8, int rows = 4);
// A family coloured source grip (drag it onto a control; click it: Link mode).
void sourceGrip(Ctx& c, int src, float cx, float cy, float r);
// Link routes: lines from a source to its linked controls on screen (the armed source in Link mode,
// else the hovered one). routesBegin at the frame's start, routeFamily for a closed family's row,
// drawRoutes at its end.
void routesBegin(Ctx& c);
void routeFamily(int fam, float cx, float cy, const Ctx& c);
void drawRoutes(Ctx& c);
// A picture of what effect slot s does, from its settings (big: the editor's display).
void fxPicture(Ctx& c, int s, float x, float y, float w, float h, bool big);
// Records that a control for parameter k was drawn (the reach check, GL_PLASMA_REACH).
void reach(int k);
const std::vector<char>& reachRecord();
void reachClear();
int& macroRenameRequest();  // a macro's menu asked to rename it (macro index, -1 = none)
// Runs fn at the start of the next frame (edits that replace the sound from inside a menu).
void later(Ctx& c, std::function<void(App&, const PluginTarget&)> fn);
void runLater(App& a, const PluginTarget& t);  // (the entry point, before reading the state)
// The output routing of route r: destination field, filter balance, sends, skip Env 1.
void routeFields(Ctx& c, int r, float x, float y, float w, float rowH);
// The same, compact: destination segments, then the filter balance and a Sends... popup (60 tall). Returns the y below.
float routeCompact(Ctx& c, int r, float x, float y, float w);
// Filter type picker (popup content): Gridline's groups and names, stored index unchanged.
void filterPicker(Ctx& c, int k);
// Morph (warp) picker for layer o, slot 0 / 1 (popup content).
void morphPicker(Ctx& c, int o, int slot);
// The display order of the filter types and morph modes: groups, each with its stored indices.
struct MenuGroup {
  const char* name;
  std::vector<int> items;
};
const std::vector<MenuGroup>& filterMenuGroups();
const std::vector<MenuGroup>& morphMenuGroups();
std::string filterDisplayName(int type);
std::string morphDisplayName(int mode);

// ---- views (plasma_*_ui.cpp)
void topBar(Ctx& c);

void playBar(Ctx& c);
void drawer(Ctx& c);
void layerView(Ctx& c);
void rootTextureView(Ctx& c);
void filtersView(Ctx& c);
void ampView(Ctx& c);
void routingView(Ctx& c);
void effectsView(Ctx& c);
const char* fxDisplayName(int type);  // Gridline's effect names (the stored type unchanged)
void envView(Ctx& c);
void lfoView(Ctx& c);
void keysView(Ctx& c);
void linksView(Ctx& c);
void setupView(Ctx& c);
void arpView(Ctx& c);
void clipEditor(Ctx& c);
void wtEditor(Ctx& c);
void presetBrowser(Ctx& c);
void mappingPanel(Ctx& c);
// The envelope editor (graph + values) of envelope e in the rectangle (Envelope and Amp views).
void envEditor(Ctx& c, int e, float x, float y, float w, float h, bool compact);
// A wheel (pitch: springs back) and the audition keyboard.
void wheel(Ctx& c, int k, float x, float y, float w, float h, bool spring);
void keyboard(Ctx& c, float x, float y, float w, float h);
// The live mini display of a rail tile.
void cycleMini(Ctx& c, int o, float x, float y, float w, float h);
void filterCurve(Ctx& c, int f, float x, float y, float w, float h, bool interactive);

// Keyboard preview (channel instruments).
void previewKey(Ctx& c, int key, bool on);
// The one Open path for preset files: Plasma presets, Serum 2 (.SerumPreset)
// and Serum 1 (.fxp) presets, detected by content. What Serum presets only
// approximate stays with the sound (the info mark next to the preset name).
void openPresetDialog(Ctx& c);
bool openPreset(Ctx& c, const std::string& path);
// Report text of the current sound's Serum origin ("" when none).
std::string openReportText(const Model& m);
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
