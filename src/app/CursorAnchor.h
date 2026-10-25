// Gridline - J-111d: where a collaborator's pointer is, in terms both computers share.
//
// A pointer position in window pixels means little on another computer: its windows
// have other sizes, another interface scale, other scroll positions and zoom. So the
// pointer is described by what it is over (an "anchor"), and each computer maps the
// anchor back through its own layout:
//   - editor surfaces the editors publish while drawing: a playlist track (time in
//     ticks, fraction of the track's height), its header and the ruler (ticks); the
//     piano roll's grid (ticks, key rows) for a pattern and channel, its keyboard,
//     ruler and velocity lane; a channel rack row (logical pixels from the row's
//     start); a mixer strip (fraction of the strip);
//   - an ImGui item (button, knob, step...) by its ID, which is a hash of the window
//     name after "###" and the item's ID path (labels and PushID values), the same
//     on both computers as long as the IDs are not pointers or translated labels;
//     with the pointer's fraction inside the item's rectangle;
//   - else the fraction of the window (window key, CollabUi).
// The smallest surface or item under the pointer wins (a surface wins a tie).
// Resolving an anchor here says whether that spot is shown: a surface or item that
// was not drawn this frame (closed or hidden window, other tab, other pattern,
// scrolled away) or a point outside its visible part is "not shown".
// UI thread only. Everything is in ImGui's logical coordinates (after the interface
// scale; the framebuffer density never enters).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"

struct ImGuiWindow;

namespace gl::canchor {

enum Kind : int {
  Window = 0,          // fraction of the window (cx, cy unused: Presence x, y)
  Item = 1,            // a: ImGui item ID; c: fraction of the item
  PlaylistTrack = 2,   // a: track index; c: tick, fraction of the track height
  PlaylistHeader = 3,  // a: track index; c: fraction of the header column, of the track height
  PlaylistRuler = 4,   // c: tick, fraction of the ruler height
  PianoGrid = 5,       // a: pattern, b: channel; c: tick, key row (0 = top of pitch 127)
  PianoKeys = 6,       // a, b as above; c: fraction of the keyboard width, key row
  PianoRuler = 7,      // a, b as above; c: tick, fraction of the ruler height
  PianoVelocity = 8,   // a, b as above; c: tick, fraction of the lane height
  RackRow = 9,         // a: channel; c: logical px from the row's left edge, fraction of the row height
  MixerStrip = 10,     // a: mixer track; c: fraction of the strip
  KindCount
};
const char* kindName(int kind);
int kindByName(const std::string& name);  // -1: unknown

struct Anchor {
  int kind = Window;
  uint32_t a = 0, b = 0;
  float cx = 0, cy = 0;
};

// The outermost window a window belongs to: child regions belong to their window;
// a docked window is a window of its own.
ImGuiWindow* rootOf(ImGuiWindow* w);

// Per frame, right after ImGui::NewFrame. `on`: collect this frame (a session runs).
// probes[0] should be the mouse (its root: the hovered window's); more probes find
// the anchor at other points (dev reports). `wanted`: item IDs to locate.
struct Probe {
  ImVec2 p{0, 0};
  ImGuiWindow* root = nullptr;  // only items of this window (nullptr: none)
};
void beginFrame(bool on, const std::vector<Probe>& probes, const std::vector<uint32_t>& wanted);
bool collecting();

// An editor surface drawn this frame in the current window: content (cx, cy) is at
// origin + c * scale on screen; shown inside [clipMin, clipMax] (intersected with the
// window's clip rectangle). No-op unless collecting.
void publish(int kind, uint32_t a, uint32_t b, ImVec2 origin, ImVec2 scale, ImVec2 clipMin, ImVec2 clipMax);
// A surface addressed by fractions of its rectangle (both axes).
inline void publishRect(int kind, uint32_t a, uint32_t b, ImVec2 mn, ImVec2 mx) {
  publish(kind, a, b, mn, ImVec2(mx.x - mn.x, mx.y - mn.y), mn, mx);
}

// After all windows: the anchor under probe `probe` (false: nothing but the window).
bool anchorAt(size_t probe, Anchor& out);
// Where `an` (not Window) is on this screen this frame. False: not drawn here.
// inView: the point is inside the visible part of its surface / item.
bool resolve(const Anchor& an, ImVec2& out, bool& inView, ImGuiWindow** root = nullptr);

}  // namespace gl::canchor
