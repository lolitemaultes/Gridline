// Gridline — plugin windows: shared header (presets, A/B, bypass, mix), custom
// editors per plugin type, and building blocks (sections, knob rows, ADSR,
// response graphs, spectrum).
#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>
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
// A stand-in (core/PluginStandIn.h) keeps the plugin it replaces while its
// type stays the same: presets and edits of the stand-in keep the original.
void setPluginState(App& a, const PluginTarget& t, const PluginState& st, const std::string& label, uint64_t mergeKey = 0);
// Stand-ins: keeps the built-in plugin for good (drops the replaced plugin and its state).
void keepStandIn(App& a, const PluginTarget& t);
// Stand-ins: a slim note above the editor ("Serum 2 is not installed: Plasma
// plays its sound"), what is approximated on hover, and "Replace with ...".
void drawStandInNote(App& a, const PluginTarget& t, float width);

// Header bar: preset navigation/menu, A/B compare, reset (+ bypass/mix for effects).
void drawPluginHeader(App& a, const PluginTarget& t);
// Editor body: the custom editor for the type, or a grouped knob grid.
void drawPluginBody(App& a, const PluginTarget& t, float width);

namespace pui {
// J-94: accent role (0xRRGGBB) of a brand family; 0 for descriptive tools.
uint32_t brandRole(BrandFamily f);
// J-130: the plugin's wordmark (Tile logo, PluginWordmark.cpp) at the cursor, as
// one item of max(frame height, size + 4) px; the name (and tagline) shows on
// hover. Returns false, drawing nothing, for types without one (hosted plugins).
bool wordmark(App& a, const std::string& type, float size);
// J-130: the "Gridline Suite" / "Gridline Suite Mini" logo, `height` px tall
// with its top-left at `pos` (drawn only); returns its width, 0 when missing.
float suiteLogo(ImDrawList* dl, ImVec2 pos, float height, bool mini);
// Its size at `height` (0, 0 when missing).
ImVec2 suiteLogoSize(float height, bool mini);
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
// J-98: the same interaction for a slider whose handle is drawn along a track
// (start .. start + len in screen pixels; vertical tracks run bottom = min):
// dragging keeps the handle under the pointer (grab offset taken at the click,
// so a click never jumps; no drift after passing an end), Shift is 8x finer.
bool dragParamTrack(App& a, ParamId pid, const ParamDesc& d, double& val, float trackStart, float trackLen, bool horizontal);
// Dev / test hook (--dev-input "ctl" / "sdlctl"): the last drawn track slider of a
// parameter index (or plugin parameter index of a mixer slot): track (logical px), the cross-axis centre, the normalized value.
struct DevTrack {
  float start = 0, len = 0, cross = 0;
  bool horizontal = true;
  double norm = 0, value = 0;
  int frame = -1;
};
bool devTrack(uint32_t paramIndex, DevTrack& out);
// Dev / test hook (--dev-input "atparam" / "plug"): the last drawn control of any kind (knob,
// slider, custom dragParam item) bound to a parameter: its rectangle in ImGui screen coordinates.
struct DevItem {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int frame = -1;
};
bool devItem(ParamId pid, DevItem& out);
// J-105 dev / test hook (--dev-input "sdlpt" / "pts"): the last drawn point editor of a
// name ("conv.eq", "conv.env"): its plot rectangle (logical px; x0..x1 = 0..span, y1..y0 =
// value 0..1), its points (t, v, tension), the point being dragged and the hovered one.
struct DevPoints {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double span = 1;
  std::vector<std::array<double, 3>> pts;
  int drag = -1, hot = -1, frame = -1;
};
void devPointsSet(const std::string& name, const DevPoints& p);
bool devPoints(const std::string& name, DevPoints& out);
// A/B compare shared with the header: store the current state in the spare
// slot, swap current and spare, and which slot is showing.
void compareStore(App& a, const PluginTarget& t);
void compareFlip(App& a, const PluginTarget& t);
bool compareOnB(const PluginTarget& t);
// True when the parameter has a MIDI controller link.
bool paramLinked(App& a, ParamId pid);
}  // namespace pui

}  // namespace gl
