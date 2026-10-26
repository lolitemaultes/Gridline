// Gridline - J-129 control styles: every control, meter and display that
// Gridline and its plugins draw, as one drawing function per element kind, in
// the styles of ui::ControlStyle (ControlStyle.h; Settings > Theme > Controls).
// The shared drawers (skin::, iskin::, pui::, ui::) hand their drawing to this
// layer while look::styled(); Classic keeps their own drawing. A style draws in
// the rectangles it is given: sizes and hit boxes never change. The dev gallery
// (`--gallery <style>[:page]`) draws every element in every state; `--look
// <style>` forces a style for one run.
//
// Everything is vectors on an ImDrawList (crisp at any scale). Each element
// takes a small state struct that carries its static value AND its live parts
// (modulation depth and the live modulated value, levels, phases, play
// positions), so the same function draws a still frame or an animated one.
#pragma once
#include <cstdint>
#include "../../third_party/imgui/imgui.h"
#include "ControlStyle.h"

namespace gl::look {

using Dir = ui::ControlStyle;  // Classic, Graphite, Hairline, Lumen, Slate
const char* dirName(Dir d);   // setting value: "graphite", ...
const char* dirTitle(Dir d);  // "Graphite", ... (English; the settings page translates)
bool parseDir(const char* s, Dir& out);

// The style in use (setting, dev --look, or a ControlStyleScope). The shared
// drawers (skin::, iskin::, pui::, ui::) hand drawing to this layer while
// styled(); Classic keeps their own drawing.
inline Dir current() { return ui::controlStyle(); }
inline bool styled() { return ui::styledControls(); }

// Element state flags.
enum : int { Hover = 1, Active = 2, Disabled = 4, On = 8, Focus = 16 };

// Colour roles an element may be tinted with (0 = the direction's default).
struct Tint {
  ImU32 value = 0;  // value arc / fill (plugin role colour)
  ImU32 mod = 0;    // modulation
};

// ------------------------------------------------------------------ rotary
struct Knob {
  float norm = 0;          // 0..1
  bool bipolar = false;    // arc from the centre
  int state = 0;
  float modDepth = 0;      // -1..1 (share of the range) - 0: no modulation ring
  float modNow = -1;       // live modulated value 0..1 (<0: none)
  float level = -1;        // live level 0..1 shown inside the knob (<0: none)
  int size = 1;            // 0 small, 1 medium, 2 large (detail level)
  int dents = 0;           // grip notches (0 = the direction's default)
  Tint tint;
};
void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k);

// ------------------------------------------------------------------ linear
struct Slider {
  float norm = 0;
  bool bipolar = false;
  bool vertical = false;
  int state = 0;
  float modDepth = 0, modNow = -1;
  Tint tint;
};
void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s);
// Mixer fader: long throw, dB ruler, a cap; meterL/R (<0 none) run beside it.
struct Fader {
  float norm = 0;  // 0..1 (0.8 = unity)
  int state = 0;
  float meterL = -1, meterR = -1, peakL = -1, peakR = -1;
};
void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f);

// ------------------------------------------------------------------ meters
struct Meter {
  float l = 0, r = -1;          // 0..1 levels (r < 0: mono)
  float peakL = -1, peakR = -1; // peak hold (<0 none)
  float rmsL = -1, rmsR = -1;   // RMS bodies (<0 none)
  bool clip = false;
  bool reduction = false;       // gain reduction: grows from the top
  bool vertical = true;
};
void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m);
// Gain-reduction needle / arc readout (compressors): db 0..-24.
void grNeedle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float grDb, float inDb, float outDb);

// ------------------------------------------------------------------ lamps
void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level = 0, ImU32 role = 0, int state = 0);

// ------------------------------------------------------------------ buttons
void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, ImU32 role = 0);
void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role = 0);  // icon = ui::Icon
// On / off switch (pill with a thumb); t = 0..1 animated position.
void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role = 0);
// Check box with caption to the right.
void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, ImU32 role = 0);
// Segmented selector / tab strip.
void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state = 0);
void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover);
// Drop-down box (closed), and its open menu under it.
void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state);
void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked);
// Value field / LCD (mono digits); `caret` >= 0 draws an edit caret after that many chars.
void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state, int caret = -1, bool lcd = false);
// "< value >" stepper.
void stepper(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state, int hoverSide);
// Sequencer step: vel 0..1, `playing` = the current step, alt = second 4-group.
void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint = 0);
// Drum / performance pad; glow 0..1 (decays after a hit).
void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, float glow, int state, ImU32 tint = 0);

// ------------------------------------------------------------------ frames
void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1);                       // editor background
void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title);  // grouped controls
void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub);  // plugin header strip
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1);                        // display well (graphs)
void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text);   // caption plate / pill
void tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value);
void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state);
// Text label in the direction's caption style (centred in the box).
void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, bool dim = false);
void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state = 0);

// ------------------------------------------------------------------ displays (inside a well)
void graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows);
// Curve with the direction's fill and glow; pts in screen px; baseY for the fill.
void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col = 0, bool fill = true);
// Envelope editor: n points (screen), sel = handle being dragged, playX < 0 = no play head; playY the live level.
void envelope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ImVec2* pts, int n, int sel, float playX, float playY);
// LFO shape with the live phase (0..1) dot.
void lfo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, float phase, float depth);
void scope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* s, int n);           // oscilloscope trace (-1..1)
void spectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* db, int n, float peakLine = 0);  // dB, -90..0
void waveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* mn, const float* mx, int n, float playPos, float selA, float selB);
void wavetable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int frames, float current, float morph);  // stacked 3D frames
void xyPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float x, float y, float mx, float my, int state);  // mx/my: live modulated point (<0 none)
void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi);
// Mod source chip (drag to a knob to modulate) and a matrix amount bar (-1..1).
void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, float live, int state);
void modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int state);

// ------------------------------------------------------------------ override hooks
// Called by the shared primitives (skin::, iskin::, pui::, ui::) while a dev
// override is set: surfaces keep their caller's colour as a hint (a look may
// use it, e.g. Slate keeps each plugin's palette; others use their own).
void hookPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint);
void hookSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint);
void hookWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint);
void hookRaised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint, int state);
void hookInset(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint);
// Button face under a caller's glyph or text (flat icon / tool / text buttons,
// segments): state Hover / Active (pressed) / On (lit) / Disabled; role = the
// lit colour (0: the style's); rest = the button has a face at rest too.
void hookFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role, bool rest);
// Value-colour hint for a knob / slider from a plugin role colour (looks may drop it).
ImU32 hookTint(ImU32 fill);
// dB (0 = full scale) to a meter level 0..1 over rangeDb.
inline float dbNorm(float db, float rangeDb) { return db <= -rangeDb ? 0.0f : db >= 0 ? 1.0f : 1.0f + db / rangeDb; }

// ------------------------------------------------------------------ gallery
// Draws the full gallery of `d` (every element in every state) over the main
// viewport; t = UI time in seconds (animated rows); page 1 controls, 2 displays + motion.
void drawGallery(Dir d, double t, int page);

}  // namespace gl::look
