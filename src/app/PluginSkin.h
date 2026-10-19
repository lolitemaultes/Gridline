// Gridline — "rack skin" for plugin editors that reproduce a reference
// plugin's panel: a per-plugin palette (panel, rail, fill, cap, graph colour
// roles) plus vector-drawn control styles — wheel knobs with a filled value
// ring and indicator line, rail sliders, stacked switch selectors, LCD-style
// numeric readouts, check boxes, bevels and graph wells. Everything is drawn
// here as vectors; positions are given in "form units" (the reference panel's
// pixel grid) and scaled by a Form so editors keep the panel's proportions.
//
// All bound controls go through pui::dragParamItem, so they share Gridline's
// interaction (drag, wheel, Shift fine, Ctrl+click reset, double-click type,
// right-click menu with MIDI link, undo).
#pragma once
#include <cstdint>
#include "../../third_party/imgui/imgui.h"
#include <functional>
#include "PluginUI.h"

namespace gl {
class App;
namespace skin {

// #RRGGBB → ImU32.
ImU32 hex(uint32_t rrggbb, float alpha = 1.0f);
ImU32 lighten(ImU32 c, float t);  // towards white
ImU32 darken(ImU32 c, float t);   // towards black

struct Skin {
  ImU32 panel = 0;       // form background
  ImU32 panelHi = 0;     // top of the background gradient / raised areas
  ImU32 panelLo = 0;     // bottom of the gradient, recessed areas
  ImU32 bevel = 0;       // section frames
  ImU32 rail = 0;        // knob / slider track
  ImU32 fill = 0;        // value ring / slider fill
  ImU32 fillPressed = 0;
  ImU32 cap = 0;         // knob cap
  ImU32 handle = 0;      // slider handle
  ImU32 text = 0;        // labels
  ImU32 textDim = 0;     // secondary labels
  ImU32 lcdBg = 0, lcdText = 0;
  ImU32 graphBg = 0, graphGrid = 0, graphGridStrong = 0, graphText = 0, graphCurve = 0;
  ImU32 select = 0;      // selector highlight
  // Optional explicit roles (0 = derived from the ones above).
  ImU32 well = 0;                  // section bevel fill (reference "plain rounded" bevels)
  ImU32 pillBg = 0, pillText = 0;  // caption pills ("rounded subtle" labels)
  ImU32 valueBg = 0, valueText = 0;  // value strips under captions
  ImU32 accent2 = 0;               // secondary curve / alternate LED colour
  // Reference-form kit roles (κ3): raised group panels, knob pointer, rounded caption plates.
  ImU32 raised = 0, pointer = 0, labelBg = 0;
  // Builds the derived roles from the reference panel's colour properties.
  static Skin make(uint32_t panel, uint32_t rail, uint32_t fill, uint32_t fillPressed, uint32_t cap, uint32_t text);
  // Reference-family builder: panel, section well, wheel rail, value fill,
  // knob cap and label colours (all #RRGGBB, as read from the form).
  static Skin fl(uint32_t panel, uint32_t well, uint32_t rail, uint32_t fill, uint32_t cap, uint32_t text);
  // Resolved optional roles.
  ImU32 wellC() const;
  ImU32 pillBgC() const;
  ImU32 pillTextC() const;
  ImU32 valueBgC() const;
  ImU32 valueTextC() const;
  ImU32 accent2C() const;
};

// Maps reference-form coordinates onto the screen: p = o + v * s.
struct Form {
  ImVec2 o{0, 0};
  float s = 1;
  float w = 0, h = 0;  // scaled form size (set by formAt; 0 when built by hand)
  ImVec2 p(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
  float u(float v) const { return v * s; }
  ImVec2 sz(float w, float h) const { return ImVec2(w * s, h * s); }
  // Form rooted at the cursor, scaled so the reference width `formW` fills `width`.
  static Form fit(float width, float formW);
  // Readable font size for a reference font of `px` form pixels (clamped 9..15).
  float font(float px = 8.0f) const;
  // Reserves the form's area (formW × formH reference px) in the ImGui layout.
  void end(float formW, float formH) const;
  float d(float v) const { return v * s; }  // same as u()
};
// A form of formW x formH units at the cursor, scaled to `width` (scale capped
// at maxScale); reserve() then claims its space in the layout.
Form formAt(float formW, float formH, float width, float maxScale = 2.0f);
void reserve(const Form& f);

enum WheelFlags {
  WheelCentered = 1,  // bipolar: ring grows from the top centre
  WheelBigLine = 2,   // thick indicator line
  WheelNoValue = 4,   // no value readout on hover
};

// Background of the whole form (vertical gradient + hairline frame).
void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding = 4);
// Recessed section frame, optional caption strip above it (inside p0.y - h).
void bevel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding = 3);
// Small-caps label centred on x at y (top).
void label(ImDrawList* dl, float cx, float y, const char* text, ImU32 col, float size);
// LCD-style readout (dark well, mono digits).
void lcd(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* text, float size, bool center = true);
// Graph well: background + border.
void graphWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k);

// Wheel knob bound to a parameter; tl/size in screen px (square).
bool wheel(App& a, const PluginTarget& t, int param, ImVec2 tl, float size, const Skin& k, int flags = 0, ImU32 fill = 0);
// Vertical rail slider (fill from bottom, or from the centre for bipolar ranges).
bool vslider(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, ImU32 fill = 0, bool ruler = true);
// Horizontal rail slider.
bool hslider(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, ImU32 fill = 0);
// Stacked switch selector over a choice parameter: one cell per label (rows,
// or columns when horizontal). Click selects, drag/wheel steps.
bool switchSel(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* const* labels, int count,
               bool horizontal = false);
// Check box bound to a toggle parameter: square LED + label to the right.
bool check(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* text, ImU32 on = 0);
// LCD digit selector over a choice / integer parameter (drag or wheel steps);
// `text` overrides the shown value.
bool digi(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* text = nullptr, float font = 0);
// Free (unbound) button in the skin; returns true when clicked.
bool button(const char* id, ImVec2 tl, ImVec2 size, const Skin& k, const char* text, bool on, float font = 0);
// Draws a wheel's face without interaction (for previews / custom bindings).
void drawWheel(ImDrawList* dl, ImVec2 c, float r, double norm, bool centered, bool bigLine, bool hot, const Skin& k, ImU32 fill);

// ---- J-19 additions: skin scope + reference-family controls -----------------

// Active skin (innermost Scope), or nullptr. pui:: widgets draw in this skin
// when one is active (wheels, panels, selectors, sliders, LEDs).
const Skin* active();
// Makes `k` the active skin and maps it onto theme::pal() and the ImGui style
// colours for the scope's lifetime, so every pui/ImGui widget inside an editor
// takes the plugin's palette.
class Scope {
 public:
  explicit Scope(const Skin& k);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};
// Paints the skin's panel behind a flow-layout editor (the height is measured
// on the previous frame, keyed per editor) and opens a Scope. Construct before
// the editor's contents; the destructor records the height.
class Backdrop {
 public:
  Backdrop(const Skin& k, float width, uint64_t key, float pad = 0);
  ~Backdrop();
  Backdrop(const Backdrop&) = delete;
  Backdrop& operator=(const Backdrop&) = delete;

 private:
  Scope scope_;
  ImVec2 o_;
  float w_ = 0, pad_ = 0;
  uint64_t key_ = 0;
};

// Section well (reference "plain rounded" bevel) in the skin's well colour.
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float rounding = 4);
// Caption pill ("rounded subtle" label): text centred in a rounded strip.
void pill(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* text, float font = 0);
// Value strip: flat rectangle with the value text in the fill colour.
void valueStrip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, const char* text, float font = 0);
// Group caption across a section: hairline from x0 to x1 (form units) with the
// caption centred on it, at form y.
void groupCaption(ImDrawList* dl, const Form& F, float x0, float x1, float y, const Skin& k, const char* text);
// Wheel with a small-caps label underneath (form units).
bool labeledWheel(App& a, const PluginTarget& t, int param, const Form& F, float x, float y, float size, const Skin& k, const char* label,
                  int flags = WheelBigLine);
// Wheel module (form units): well at (x,y,w,h), wheel on the left, caption pill
// at the top right and the live value underneath. `caption` defaults to the
// parameter name; `value` overrides the formatted value.
bool paramBox(App& a, const PluginTarget& t, int param, const Form& F, float x, float y, float w, float h, const Skin& k,
              const char* caption = nullptr, const char* value = nullptr, int flags = WheelBigLine);
// Push button bound to a toggle parameter (lit when on).
bool toggle(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, const char* text = nullptr, float font = 0);
// Drop-down box bound to a choice parameter (LCD with an arrow; opens a menu).
bool menuBox(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, float font = 0);
// Selector of `count` choice cells drawn with icons by `drawIcon(dl, i, centre, w, h, col)`.
bool iconSel(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const Skin& k, int count,
             void (*drawIcon)(ImDrawList*, int, ImVec2, float, float, ImU32), bool horizontal = true);
// Step grid over `count` parameters (param = first + i * stride). Toggle
// parameters are switched (click / paint by dragging); continuous ones are
// level bars drawn by dragging. `current` highlights the playing step.
bool stepGrid(App& a, const PluginTarget& t, int first, int count, ImVec2 tl, ImVec2 size, const Skin& k, int stride = 1, int current = -1,
              int groupOf = 4);
// X/Y pad bound to two parameters (x → px, y → py, up = high).
bool xyPad(App& a, const PluginTarget& t, int px, int py, ImVec2 tl, ImVec2 size, const Skin& k, const char* label = nullptr);
// Envelope editor in the reference style: delay / attack / hold / decay /
// sustain / release segments laid out by each time knob's position, with
// draggable handles. Any index may be -1 (segment absent). Sustain is a level.
struct EnvParams {
  int del = -1, att = -1, hold = -1, dec = -1, sus = -1, rel = -1;
  int amount = -1;  // optional depth knob drawn as the curve's vertical scale
};
bool envelope(App& a, const PluginTarget& t, const EnvParams& e, ImVec2 tl, ImVec2 size, const Skin& k);
// Keyboard strip from note lo to hi (inclusive). `lit` marks held notes (128
// entries or nullptr); [markLo, markHi] is tinted (key ranges); root is
// outlined. Returns the note clicked this frame, or -1.
int keyboard(const char* id, ImVec2 tl, ImVec2 size, int lo, int hi, const Skin& k, const bool* lit = nullptr, int markLo = -1, int markHi = -1,
             int root = -1);
// Segmented LED meter (value in dB, 0 = full scale); reduction grows down.
void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Skin& k, float db, float rangeDb = 48.f, bool reduction = false);
// Round indicator LED.
void ledDot(ImDrawList* dl, ImVec2 c, float r, bool on, const Skin& k, ImU32 onCol = 0);
// Text in the skin (mono bold, small caps style) at a position; returns width.
float text(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s, float size);
// ===================================================================== form-unit controls (κ2)
// Additions used by the dynamics editors: every control placed by form units
// (x, y, w, h) on a Form, with per-control colour roles taken from the
// reference form (each knob there has its own Rail/Fill/Cap colours). Bound
// controls use pui::dragParamItem like the ones above.

// Wheel variant: per-control colours plus the reference's other wheel shapes.
struct WheelLook {
  ImU32 rail = hex(0x3F484D);
  ImU32 fill = hex(0x7D908C);
  ImU32 fillPressed = hex(0xA7C1BC);
  ImU32 cap = hex(0x3F484D);
  ImU32 pointer = hex(0xE4ECEE);
  bool centred = false;     // fill grows from the 12 o'clock position (bipolar)
  float railWidth = 0.17f;  // rail thickness as a share of the radius
  float gap = 1.5708f;      // unused arc at the bottom (radians): 90 deg = 270 deg sweep
  bool ruler = false;       // ruler ticks outside the cap instead of a flat gauge rail
  int dents = 0;            // notches around the cap rim (cog / dented caps)
};
struct DigiLook {
  ImU32 bg = hex(0x6A787B);
  ImU32 text = hex(0xFFFFFF);
};
struct TabLook {
  ImU32 btn = hex(0x3E474D);
  ImU32 btnPressed = hex(0x2F383D);
  ImU32 text = hex(0x9FA8AD);
  ImU32 textPressed = hex(0xFFFFFF);
};
struct MeterLook {
  ImU32 bg = hex(0x1A1E21);
  ImU32 low = hex(0x5FBF4A);
  ImU32 mid = hex(0xD7C94A);
  ImU32 high = hex(0xE5484D);
  float rangeDb = 60;
};
enum BevelStyle { BevelPlain = 0, BevelRounded, BevelRoundedBottom, BevelRoundedTop };

// Wheel face in a WheelLook (n = normalized value).
void drawWheel(ImDrawList* dl, ImVec2 c, float r, double n, bool centred, bool hot, bool pressed, const WheelLook& look);

// Static parts (form units).
void bevel(ImDrawList* dl, const Form& f, float x, float y, float w, float h, ImU32 col, int style = BevelRounded);
void gradient(ImDrawList* dl, const Form& f, float x, float y, float w, float h, ImU32 top, ImU32 bottom);
// Boxed text: align 0 left, 1 centre, 2 right; bg 0 = transparent; px = font size at 100 %.
void label(ImDrawList* dl, const Form& f, float x, float y, float w, float h, const char* text, ImU32 col, ImU32 bg = 0,
           float px = 10.0f, int align = 1, bool bold = true, bool rounded = true);
// Rounded section caption pill.
void caption(ImDrawList* dl, const Form& f, float x, float y, float w, float h, const char* text, ImU32 col, ImU32 bg);
// Thin bracket under a pair of controls.
void bracket(ImDrawList* dl, const Form& f, float x0, float x1, float y, ImU32 col);
// Level meter (dB, 0 = full scale), vertical; reduction meters grow from the top.
void meter(ImDrawList* dl, const Form& f, float x, float y, float w, float h, float db, const MeterLook& look = {}, bool reduction = false);

// Bound controls (form units); true when the parameter changed.
bool wheel(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const WheelLook& look);
// LCD numeric box (choice / integer params): drag or wheel steps; text overrides the shown value.
bool digi(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const DigiLook& look,
          const char* text = nullptr);
// Tab selector bound to a choice param (labels override the choice names).
bool tabs(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const char* const* labels,
          int count, const TabLook& look, bool vertical = false);
// Same for UI-only state.
bool tabsUi(const Form& f, const char* id, float x, float y, float w, float h, const char* const* labels, int count, int& sel,
            const TabLook& look, bool vertical = false);
// Switch selector (the one above, placed in form units; vertical when h > w;
// labels default to the choice names).
bool switchSel(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, ImU32 slot, ImU32 lit,
               const char* const* labels = nullptr);
// LED push button bound to a toggle param, with caption.
bool ledToggle(App& a, const PluginTarget& t, int param, const Form& f, float x, float y, float w, float h, const char* text,
               ImU32 bg, ImU32 on, ImU32 textCol);

// UI-only controls (form units).
bool slider(const Form& f, const char* id, float x, float y, float w, float h, float& v, ImU32 rail, ImU32 handle, const char* hint = nullptr);
// Glyph toggle (display options); draw(dl, centre, size, colour) paints the glyph.
bool glyphToggle(const Form& f, const char* id, float x, float y, float w, float h, bool& on, ImU32 col,
                 void (*draw)(ImDrawList*, ImVec2, float, ImU32), const char* hint = nullptr);
bool button(const Form& f, const char* id, float x, float y, float w, float h, const char* text, ImU32 bg, ImU32 textCol,
            const char* hint = nullptr);
// Options (three-bar) button.
bool menuButton(const Form& f, const char* id, float x, float y, float w, float h, ImU32 col);
// Hint bar text while the last item is hovered.
void hint(App& a, const char* text);
// =============================================================== reference-form kit (κ3)
// Places controls at a reference form's geometry (SkinForms.h tables, reference
// pixels scaled to the window) and draws them in the styles the form names:
// gauge / ruler / inter-fill rails, bevelled / chrome / gear caps, carved
// sliders with mixer / flat / chromed handles, cell selectors, digit spinners,
// LED check boxes, combos, panels and bevels.


// ------------------------------------------------------------- form data
enum class Ctl : uint8_t { Panel, Bevel, Wheel, Slider, Selector, Digi, Button, Check, Label, Paint, Env, Combo, Meter, XY, Memo, PaintWheel, Gradient, Group };
// Rail styles (index into the generator's list).
enum RailStyle : uint8_t { RailNone, RailGauge, RailInterFill, RailInterFillIn, RailRulerOut, RailCarved };
// Cap / handle styles.
enum CapStyle : uint8_t { CapNone, CapBevelled, CapChrome, CapChrome2, CapGearChrome, CapSoftBump, HandleClassic, HandleFlat, HandleMixer };

struct FormCtl {
  const char* name;
  Ctl kind;
  int x, y, w, h;   // reference pixels, relative to the form's client area
  int tag;          // reference parameter tag (-1 none)
  int min, max, def;
  int color, fill, rail, cap;  // 0xRRGGBB, -1 = unset
  float a0, a1;     // wheel gap angles (radians, maths convention): min at a0, max at a1, clockwise over the top
  int cols, rows;   // selector cells
  int railStyle, capStyle, style;
  int anchors;      // 1 left, 2 top, 4 right, 8 bottom (reference resize anchors)
};
struct FormDef {
  const char* id;
  int w, h;
  int bg;
  const FormCtl* ctls;
  int count;
  const FormCtl* find(const char* name) const;
};

// ------------------------------------------------------------- palette
// The kit uses Skin; paletteFrom() fills it from a form's colour roles (form bg,
// panel colour, first wheel fill / rail / cap, first label background).
using Palette = Skin;
Palette paletteFrom(const FormDef& f);
ImU32 col(int rrggbb, float alpha = 1.0f);

// ------------------------------------------------------------- placement
struct View {
  const FormDef* f = nullptr;
  ImVec2 o;          // screen position of the form's (0,0)
  float s = 1;       // reference px → screen px
  float growH = 0;   // extra form height (reference px): bottom-anchored controls move, top+bottom ones stretch
  ImVec2 p(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
  float dy0(const FormCtl& c) const { return (c.anchors & 8) && !(c.anchors & 2) ? growH : 0.f; }
  float dy1(const FormCtl& c) const { return (c.anchors & 8) ? growH : 0.f; }
  ImVec2 p0(const FormCtl& c) const { return p(static_cast<float>(c.x), static_cast<float>(c.y) + dy0(c)); }
  ImVec2 p1(const FormCtl& c) const { return p(static_cast<float>(c.x + c.w), static_cast<float>(c.y + c.h) + dy1(c)); }
  float height() const { return f ? (static_cast<float>(f->h) + growH) * s : 0.f; }
  const FormCtl& c(const char* name) const;  // asserts in debug; returns a dummy when missing
  float px(float v) const { return v * s; }
};
// Starts a form at the cursor, scaled to `width` (capped by maxScale); fills the background.
// growH adds reference height (resizable forms: anchors decide what moves).
View begin(const FormDef& f, float width, float maxScale, const Palette& pal, float growH = 0);
// Reserves the form's space in the layout.
void end(const View& v);
// Scale that fits a form into width (capped).
float fitScale(const FormDef& f, float width, float maxScale);

// ------------------------------------------------------------- static drawing
void panel(const View& v, const char* name, const Palette& pal, const char* title = nullptr);
void bevel(const View& v, const char* name, const Palette& pal);
void label(const View& v, const char* name, const Palette& pal, const char* text, int align = 0 /* -1 left, 0 centre, 1 right */,
           bool rounded = false);
void textAt(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, const char* text, float size, int align = 0, bool mono = false);
// LCD readout (dark well, light mono digits).
void lcd(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Palette& pal, const char* text, int align = 0);
// Knob drawing (no interaction): norm 0..1, bipolar arcs from the centre.
void drawWheel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const FormCtl& c, const Palette& pal, double norm, bool bipolar, bool hot, float scale);
// Slider drawing (vertical when taller than wide).
void drawSlider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const FormCtl& c, const Palette& pal, double norm, bool bipolar, bool hot, float scale);

// ------------------------------------------------------------- parameter controls
// Knob at the control's rect bound to a Gridline parameter.
bool wheel(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal);
// Knob at an explicit rect (reference px), styled like control `like`.
bool wheelAt(App& a, const PluginTarget& t, const View& v, const char* like, float x, float y, float w, float h, int param, const Palette& pal);
bool slider(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal);
// Cell selector bound to a choice/toggle parameter; `cells` = number of values
// (defaults to cols × rows); `draw` paints cell content (index, rect, selected).
using CellFn = std::function<void(ImDrawList*, int, ImVec2, ImVec2, bool)>;
bool selector(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const CellFn& draw,
              int cells = 0, int valueOffset = 0);
// Text-cell selector (labels per cell).
bool textSelector(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal,
                  const char* const* labels, int cells, int valueOffset = 0);
// LED check box bound to a toggle parameter (text right of the LED).
bool check(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal, const char* text);
// Digit spinner (LCD digits; drag / wheel to change) bound to a parameter; fmt formats the value.
bool digi(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal,
          const std::function<std::string(double)>& fmt = {});
// Flat combo bound to a choice parameter.
bool combo(App& a, const PluginTarget& t, const View& v, const char* name, int param, const Palette& pal);

// ------------------------------------------------------------- free controls (no parameter)
// Flat button; returns true when clicked. `on` draws the latched state; glyph paints the face.
bool button(const View& v, const char* name, const Palette& pal, bool on, const char* text = nullptr,
            const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>& glyph = {}, const char* hint = nullptr);
bool buttonAt(const View& v, const char* id, float x, float y, float w, float h, const Palette& pal, bool on, const char* text = nullptr,
              const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>& glyph = {}, const char* hint = nullptr);
// Flat combo over a plain value (no parameter); returns true when changed.
bool comboFree(const View& v, const char* name, const Palette& pal, const char* const* items, int count, int& value, const char* hint = nullptr);
// LED check box not bound to a parameter.
bool checkFree(const View& v, const char* name, const Palette& pal, bool on, const char* text, const char* hint = nullptr);

// Small glyphs used by several editors.
void glyphMenu(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c);    // three bars
void glyphSnow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c);    // freeze
void glyphMagnet(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c);  // snap
void glyphSteps(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c);   // step editing
void glyphSlide(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c);   // slide points
void glyphWave(ImDrawList* dl, int shape, ImVec2 a, ImVec2 b, ImU32 c, float th = 1.5f);  // 0 sine 1 tri 2 square 3 saw


}  // namespace skin
}  // namespace gl
