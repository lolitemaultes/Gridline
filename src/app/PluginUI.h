// Gridline — plugin windows: shared header (presets, A/B, bypass, mix), custom
// editors per plugin type, and building blocks (sections, knob rows, ADSR,
// response graphs, spectrum).
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"
#include "../core/PluginBrand.h"
#include "../engine/Scope.h"

namespace gl {

class App;

namespace audit {
void bindTouch(const ParamId& pid);  // LayoutAudit.cpp
void stateRead(int k);               // an editor read parameter k from the state (graphs, grids)
}

// Where a plugin lives: a channel's instrument (slot < 0) or a mixer slot.
struct PluginTarget {
  ObjectId owner = kNoId;
  int slot = -1;
  bool isChannel() const { return slot < 0; }
  ParamId pid(int k) const {
    const ParamId p = isChannel() ? channelPluginParam(owner, k) : slotParam(owner, slot, k);
    audit::bindTouch(p);  // J-92 binding audit (one branch when off)
    return p;
  }
  const PluginState* state(const Song& s) const;
  const PluginInfo* info(const Song& s) const;
  uint64_t key() const { return (static_cast<uint64_t>(owner) << 8) ^ static_cast<uint64_t>(slot + 1); }
  const ScopeTap* tap(App& a) const;       // live output for displays
  const ScopeTap* inputTap(App& a) const;  // live input (effects only; nullptr otherwise)
};

// Replaces the whole plugin state as one undo step (mergeKey folds drags).
void setPluginState(App& a, const PluginTarget& t, const PluginState& st, const std::string& label, uint64_t mergeKey = 0);

// Header bar: preset navigation/menu, A/B compare, reset (+ bypass/mix for effects).
void drawPluginHeader(App& a, const PluginTarget& t);
// Editor body: the custom editor for the type, or a grouped knob grid.
void drawPluginBody(App& a, const PluginTarget& t, float width);

namespace pui {
// J-94: accent role (0xRRGGBB) of a brand family; 0 for descriptive tools.
uint32_t brandRole(BrandFamily f);
// Flagship wordmark at the cursor (accent bar + lower-case name, tagline on
// hover). Returns false, drawing nothing, for types without one.
bool wordmark(App& a, const std::string& type, float size);
// Panel with a small-caps title; call end() after the contents.
void beginSection(const char* title, float width, float height = 0);
void endSection();
// Knob with value + label underneath, fixed cell width.
void knobCell(App& a, const PluginTarget& t, int param, const char* label = nullptr, float cell = 64, float radius = 16);
// Choice parameter as segmented buttons.
void choiceButtons(App& a, const PluginTarget& t, int param, float width = 0);
// Toggle parameter as a pill button.
void toggleButton(App& a, const PluginTarget& t, int param, const char* label = nullptr);
// Interactive ADSR graph bound to four parameters (seconds / 0..1 sustain).
void adsrGraph(App& a, const PluginTarget& t, int att, int dec, int sus, int rel, ImVec2 size);
// Log-frequency helpers for graphs (20 Hz – 20 kHz).
float freqToX(double hz, float x0, float w);
double xToFreq(float x, float x0, float w);
// Draws the frequency grid (Hz lines/labels) and dB lines.
void drawFreqGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double dbRange, bool labels = true);
// Smoothed magnitude spectrum of a tap (dB per log-spaced point), keyed for smoothing.
void spectrum(const ScopeTap* tap, uint64_t key, int points, float* outDb, double sampleRate);

// ---- rack-panel kit (0.5) -------------------------------------------------
// Knob sizes for knob()/knobAt().
enum KnobStyle { KnobSmall = 0, KnobMedium = 1, KnobLarge = 2 };
// Knob bound to a plugin parameter, drawn in a fixed-width cell with its value
// and label underneath. Accent value arc (bipolar ranges arc from the centre),
// value tooltip while dragging, wheel steps, Shift = fine, Ctrl/Cmd+click
// resets, double-click types a value, right-click opens a menu (reset, type,
// MIDI link). cell <= 0 picks the style's default width. Returns true on change.
bool knob(App& a, const PluginTarget& t, int param, const char* label = nullptr, int style = KnobMedium, float cell = 0);
// Same, with the cell's top-left at an absolute screen position (canvas layouts).
bool knobAt(App& a, const PluginTarget& t, int param, ImVec2 pos, const char* label = nullptr, int style = KnobMedium,
            float cell = 0);
// Knob without label/value text (just the dial), e.g. inside dense strips.
bool dial(App& a, const PluginTarget& t, int param, float radius);
float knobCellWidth(int style);
float knobCellHeight(int style);
// Rack panel: rounded panel with a hairline border and a title strip (title
// left, optional caption right). Call endPanel() after the contents.
void beginPanel(const char* title, float width, float height = 0, const char* caption = nullptr);
void endPanel();
// Horizontal slider bound to a parameter (label left, value right).
bool hslider(App& a, const PluginTarget& t, int param, const char* label, float width);
// Vertical slider bound to a parameter (value + label below when label != nullptr).
bool vslider(App& a, const PluginTarget& t, int param, const char* label, ImVec2 size);
// LED toggle for a toggle parameter: indicator + label, click flips.
bool led(App& a, const PluginTarget& t, int param, const char* label = nullptr);
// Segmented selector for a choice parameter; labels override the choice names,
// columns > 0 wraps into rows. Returns true on change.
bool segmented(App& a, const PluginTarget& t, int param, float width, const char* const* labels = nullptr, int columns = 0);
// Compact dropdown for a choice parameter (optional label to its left).
bool combo(App& a, const PluginTarget& t, int param, float width, const char* label = nullptr);
// Small stepper "< value >" for a choice or integer parameter.
bool stepper(App& a, const PluginTarget& t, int param, float width, const char* label = nullptr);
// Display well (graphs): dark rounded background with a hairline border.
void displayFrame(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
// Evenly spaced grid lines inside a rect (optional emphasised centre lines).
void drawGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows, bool centreLines = false);
// Vertical level meter: value in dB (0 = full scale). Reduction meters grow
// down from the top in the danger colour.
void levelMeter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float db, bool reduction = false, float rangeDb = 48.f);
// Small-caps caption (mono, faint) at the cursor.
void caption(const char* text);
// Knob with its label and value to the right (compact grid cells). Returns true on change.
bool knobInline(App& a, const PluginTarget& t, int param, const char* label, float width, float radius = 14);
// Rolling history (e.g. levels / gain reduction) sampled once per UI frame per key.
void historyPush(uint64_t key, int lane, float value);
// Draws up to 4 lanes of history as lines inside a display (values in dB over [lo, hi]).
// lane colours: 0 paper, 1 dim text, 2 danger (reduction, drawn from the top), 3 accent.
void historyDraw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, uint64_t key, float lo, float hi);
// Parses typed text in a parameter's units ("1.2k", "-6", "250ms", "50%", a choice name).
bool parseValue(const ParamDesc& d, const std::string& text, double& out);
// Writes one parameter as an undoable edit (merge = fold into the current gesture).
void setParamValue(App& a, const PluginTarget& t, int param, double value, bool merge = false);
// Current parameter value (0 when missing).
double paramValue(App& a, const PluginTarget& t, int param);
// Standard parameter interaction for the last ImGui item (drag over `pixels`,
// wheel, Shift fine, Ctrl+click reset, double-click type, right-click menu,
// hint + value tooltip). Custom-drawn controls (PluginSkin) build on it.
bool dragParamItem(App& a, ParamId pid, const ParamDesc& d, double& val, float pixels, bool horizontal);
// A/B compare shared with the header: store the current state in the spare
// slot, swap current and spare, and which slot is showing.
void compareStore(App& a, const PluginTarget& t);
void compareFlip(App& a, const PluginTarget& t);
bool compareOnB(const PluginTarget& t);
// True when the parameter has a MIDI controller link.
bool paramLinked(App& a, ParamId pid);
}  // namespace pui

}  // namespace gl
