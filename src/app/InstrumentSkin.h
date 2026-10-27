// Gridline — instrument editor skin (J-2): reference-representative control
// kit for the instrument editors. Every editor lays its controls out on the
// reference form's own coordinate grid (form pixels) and the skin maps that
// grid onto the editor width, so proportions match the reference exactly at
// any size. Each plugin passes its own palette (colour roles: background,
// rail, fill, cap, label, LCD …). Everything is drawn here as vectors.
//
// Kept small and self-contained so it can merge with the shared effect skin
// (`PluginSkin` / pui::) — the control interaction (drag, wheel, reset, type,
// MIDI link, undo) is the shared pui:: one.
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../engine/Sample.h"
#include "PluginSkin.h"
#include "PluginUI.h"

namespace gl::iskin {

// Colour from 0xRRGGBB.
ImU32 rgb(uint32_t rrggbb, float alpha = 1.0f);
ImU32 lerp(ImU32 a, ImU32 b, float t);

// Palette roles (per plugin).
struct Palette {
  ImU32 bgTop = rgb(0x5D686E), bgBottom = rgb(0x4A545A);  // form background (gradient)
  ImU32 panel = rgb(0x2D383E);                             // inset panels / selectors
  ImU32 panelLine = rgb(0x1D282E);                         // panel outlines
  ImU32 rail = rgb(0x5A656B);                              // wheel / slider rail
  ImU32 fill = rgb(0xB1E758);                              // primary value colour
  ImU32 fill2 = rgb(0xFFA830);                             // secondary value colour
  ImU32 fill3 = rgb(0xF4462D);                             // tertiary value colour
  ImU32 cap = rgb(0x62676A);                               // wheel cap
  ImU32 handle = rgb(0x273035);                            // slider handle
  ImU32 label = rgb(0x1D282E);                             // section captions
  ImU32 text = rgb(0xD8DEE2);                              // light text
  ImU32 textDim = rgb(0x8D989E);                           // dim text / icons
  ImU32 lcdBg = rgb(0x1A1F22), lcdText = rgb(0xB1E758);    // numeric readouts
  ImU32 led = rgb(0xB1E758), ledOff = rgb(0x394043);       // check boxes / switches
  ImU32 pointer = 0;                                       // wheel pointer (0 = light on the cap)
};

// A form-coordinate canvas: origin + scale (screen px per form px).
struct Skin {
  ImVec2 o{0, 0};
  float s = 1.0f;
  Palette c;
  ImVec2 at(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
  float px(float v) const { return v * s; }
  ImDrawList* dl() const { return ImGui::GetWindowDrawList(); }
  // The shared rack skin (skin::, PluginSkin.h) with this palette's roles: bound
  // wheels and sliders are drawn by the shared helpers; this header only adds
  // the instrument-specific pieces (form canvas, pads, keyboard, free values).
  skin::Skin shared(ImU32 capOverride = 0) const;
};

// J-128: a row of wheels of mixed sizes on one centre line (see skin::RowScope):
// the tallest (`tallest` form px) starts at form y `top`, labels drawn with
// labelY() sit `gap` under it.
struct Row : skin::RowScope {
  Row(const Skin& k, float top, float tallest, float gap = 1.0f)
      : skin::RowScope(k.at(0, top + tallest * 0.5f).y, k.at(0, top + tallest + gap).y) {}
};
// The label line (form px) of the active row, or `fallback` outside a row.
inline float labelY(const Skin& k, float fallback) { return skin::rowActive() ? (skin::rowLabelY() - k.o.y) / k.s : fallback; }

// Starts a canvas at the cursor that maps a formW × formH form onto `width`
// screen pixels; reserves the space (call end() to move the cursor below).
Skin begin(float width, float formW, float formH, const Palette& pal);
void end(const Skin& k, float formH);

// ---- static drawing ------------------------------------------------------------
// Form background: vertical gradient over the rect (form px), optional rounding.
void background(const Skin& k, float x, float y, float w, float h, ImU32 top, ImU32 bottom, float round = 0);
// Inset / raised panel (bevel): filled rect with a hairline edge.
void bevel(const Skin& k, float x, float y, float w, float h, ImU32 fill, ImU32 edge, float round = 3);
// Caption text inside a rect; align 0 left, 1 centre, 2 right. size in form px.
void text(const Skin& k, float x, float y, float w, float h, const char* s, float size, ImU32 col, int align = 0,
          bool mono = false, bool bold = false);
// Numeric LCD readout (dark well, mono digits).
void lcd(const Skin& k, float x, float y, float w, float h, const char* s, ImU32 bg, ImU32 fg, float size = 0);

// ---- controls bound to plugin parameters ---------------------------------------
enum WheelFlags { WheelCentered = 1, WheelBigLine = 2, WheelNoArc = 4, WheelSteps = 8 };
// Rotary wheel (rail ring, value arc in `fill`, cap, pointer). (x, y, size) in form px.
bool wheel(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float size, ImU32 fill,
           int flags = 0, ImU32 cap = 0);
// Vertical slider (centered ones fill from the middle). Rect in form px.
bool slider(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, ImU32 fill,
            bool centered = true);
// Horizontal slider.
bool hslider(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, ImU32 fill,
             bool centered = false);
// Check box: LED square + caption (toggle param).
bool check(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h, const char* caption,
           float size = 11);
// Switch button drawn by `icon(dl, rect min, rect max, on)` (toggle param).
bool switchBtn(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
               const std::function<void(ImDrawList*, ImVec2, ImVec2, bool)>& icon);
// Selector over a choice param: draws the current item with `item(dl, min, max, index)`;
// click right/left half or wheel steps, drag slides; endless wraps.
bool selector(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
              const std::function<void(ImDrawList*, ImVec2, ImVec2, int)>& item, bool endless = true);
// LCD that is dragged like a wheel (numeric value or choice name; `fmt` overrides the text).
bool digi(App& a, const PluginTarget& t, const Skin& k, int param, float x, float y, float w, float h,
          const std::function<std::string(double)>& fmt = {}, float size = 0);
// The same controls over a free value (zone settings and other data that is not
// a plugin parameter): drag, wheel, Ctrl+click resets to d.def, double-click
// resets (types at the default), right-click menu (J-136). True on change.
bool wheelValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float size, ImU32 fill,
                int flags = 0, ImU32 cap = 0);
bool sliderValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float w, float h, ImU32 fill,
                 bool centered = false);
bool digiValue(App& a, const Skin& k, const char* id, double& v, const ParamDesc& d, float x, float y, float w, float h,
               const std::function<std::string(double)>& fmt = {}, float size = 0);
// Text button with a lit state (actions, not params). Returns true when clicked.
bool button(const Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool lit,
            float size = 10, ImU32 litCol = 0);
// Drum pad (velocity from the click height, lit while sounding / selected).
// Returns 1 on press (velocity in *vel), 2 on release, 0 otherwise.
int pad(const Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool selected, float glow,
        float* vel);

// Clickable keyboard (audition): `whites` white keys from `lowNote` (a C).
// mark >= 0 highlights a key (root / base note); lit[] (128 entries, optional)
// lights keys (mapped zones / pads). Returns the clicked note or -1; with
// `pickOnly` the click only returns the note (no audition).
struct KeyColors {
  ImU32 white = rgb(0xCFD3DA), black = rgb(0x2F3031), pressed = rgb(0xFE8025), mark = rgb(0x7FF0AA), sep = rgb(0x9FA8AD);
};
int keyboard(App& a, const PluginTarget& t, const Skin& k, float x, float y, float w, float h, int lowNote, int whites,
             const KeyColors& kc, int mark = -1, const bool* lit = nullptr, bool pickOnly = false);

// Waveform of a sample (mono min/max columns) inside a screen rect, frames
// from..to as fractions; `centre` draws the zero line.
void wave(ImDrawList* dl, const Sample* smp, ImVec2 p0, ImVec2 p1, ImU32 col, double from = 0, double to = 1, ImU32 centre = 0);

// Small vector glyphs used by the editors.
void glyphPan(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void glyphInvert(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void glyphArrow(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int dir);  // 0 left 1 right 2 up 3 down
void glyphFolder(ImDrawList* dl, ImVec2 c, float r, ImU32 col);

}  // namespace gl::iskin
