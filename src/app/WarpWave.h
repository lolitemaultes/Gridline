// Gridline: ui::WarpWave, warp markers on any waveform. One widget draws and
// edits them everywhere a waveform can be warped: playlist audio clips (with
// PlaylistWarp.h's tick mapping), the audio clip editor, the sampler, Kerf,
// Pads, Loupe and the Tracker sample editor.
//
// It shows the markers (faint when warp mode is off, handles when it is on),
// the sections that play faster or slower (a teal tint and their %), and
// takes the warp mode's gestures on a sample's markers (core/SampleWarp.h):
// click adds a marker (on the nearest transient when snapping is on), drag
// moves it on the grid (Alt: free), Shift+drag slides the audio under it,
// double-click or right-click removes it. The host maps x to warped seconds.
//
// Also the warp-mode button every view puts at the left of its own toolbar
// (the same teal glyph everywhere) and the "W 6" badge of a warped sample.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/SampleWarp.h"
#include "../core/Types.h"
#include "WarpWaveGeom.h"

namespace gl {
class App;
struct Sample;

namespace warpwave {

// Draws the markers in the body [b0, b1] (clipped to visX0..visX1). `active`: warp mode is on
// (handles, tinted sections and their speed); else faint lines in `ink`. `held`: the dragged
// marker. Returns the marker under the pointer (warp mode on, pointer in the body; 0: none).
uint32_t draw(ImDrawList* dl, const std::vector<Mark>& v, ImVec2 b0, ImVec2 b1, float visX0, float visX1, bool active, ImU32 ink,
              uint32_t held = 0);

// The warp glyph (a tab on a line) centred at c, `size` px tall.
void glyph(ImDrawList* dl, ImVec2 c, float size, ImU32 col);
// The warp-mode toggle: the glyph and `label` ("Warp", "Flex"), lit teal while on. `height` 0: the frame height.
// The mode's name in the current layout: "Flex" in Smooth (its own word for it), else "Warp".
const char* modeWord();
bool modeButton(const char* id, bool on, const char* label = nullptr, float height = 0.0f, const char* hint = nullptr);
// The "W 6" badge of a clip or sample with markers, drawn at `pos` (top-left); returns its width.
float badge(ImDrawList* dl, ImVec2 pos, int count, float height = 16.0f);
float badgeWidth(int count);

// ---- editing a sample's markers (sampler, Kerf, Pads, Loupe, Tracker) ----
struct Map {
  std::function<float(double)> xOf;  // warped seconds -> x
  std::function<double(float)> wOf;  // x -> warped seconds
  double step = 0;                   // grid in warped seconds (0: free); Alt drags free
  double fileSec = 0;                // the file's length
  const std::vector<double>* onsets = nullptr;  // file seconds; new markers snap to them (when set)
};
struct EditResult {
  bool changed = false;  // the markers changed (the host stores them: one step per gesture)
  bool released = false; // a gesture ended this frame
  uint32_t hot = 0;      // the marker under the pointer
  std::string label;     // the undo label of the change
  bool menuClick = false; // a right-click ended over the rect that removed no marker (the host's menu may open)
};
// The warp mode's gestures over the item rect [b0, b1] (call after drawing the waveform; it draws
// the markers itself). `active` false: the markers show faint and nothing is taken.
EditResult edit(const char* id, std::vector<WarpMarker>& m, const Map& map, ImVec2 b0, ImVec2 b1, bool active, ImU32 ink);
// Dev checks: "warpwave [tag]" prints every Warp button and marker lane drawn in the last two frames
// (its id, rect, whether warp mode is on, its markers as warped seconds), each line with the tag.
void devCommand(App& a, const std::vector<std::string>& args);

// The markers of `m` as `draw` takes them.
std::vector<Mark> marks(const std::vector<WarpMarker>& m, const std::function<float(double)>& xOf);

// The Warp submenu's rows for a sample (the playlist clip menu's rows, plus the loop's tempo and
// Kerf's slicing): returns true when `w` changed. `bpmFallback`: the detected tempo.
struct MenuOpts {
  bool slicer = false;     // "Slice at warp markers"
  bool* sliceAtMarkers = nullptr;
  bool applyRow = false;   // Loupe and the Tracker: "Apply warp to a new file"
  bool* apply = nullptr;
  double gridStep = 0;     // quantize grid (warped seconds; 0: the loop's beats)
};
bool sampleMenu(App& a, SampleWarp& w, const Sample* smp, const MenuOpts& o);

// The sample instruments' warp on their waveform well (sampler, Kerf, pads; K-248): the teal Warp
// button at the well's top right, the markers on the waveform (edited while warp mode is on: one
// undo step per gesture, synced in collaboration with the channel), the warp mode key while the
// pointer is over the well, and the right-click Warp menu. `smp`: the channel's sample as loaded
// (not warped). Returns true while warp mode is on: the host leaves the pointer to the markers.
struct WellOpts {
  bool slicer = false;          // Kerf: "Slice at warp markers"
  bool* sliceAtMarkers = nullptr;
  bool menu = true;             // a right-click on the well opens the Warp menu
  // A zoomed view (Kerf): warped seconds <-> x (unset: the whole sample fills the well).
  std::function<float(double)> xOf;
  std::function<double(float)> wOf;
  float laneH = 0;              // > 0: the markers live in a lane this tall at the well's bottom (Kerf)
  float buttonRight = 44;       // the mode button's gap from the well's right edge
};
bool sampleWell(App& a, ObjectId channel, const char* id, ImVec2 p0, ImVec2 p1, const std::shared_ptr<const Sample>& smp, ImU32 ink,
                const WellOpts& o = {});
// The sample the well shows: warped while the warp plays, else `smp`.
std::shared_ptr<const Sample> shownSample(const SampleWarp& w, const std::shared_ptr<const Sample>& smp);

}  // namespace warpwave
}  // namespace gl
