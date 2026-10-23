// Gridline — J-67: smooth scrolling and elastic edges in the interface.
//
// Custom-scrolled views (playlist, piano roll, automation editor, audio
// editor) keep a smoothscroll::Axis per scroll axis and call step() once a
// frame where they used to add the wheel delta to their scroll value.
// Every ImGui scroll region (channel rack, mixer, browser, plugin picker,
// dialogs, lists, menus) gets the same feel through the hooks around
// ImGui::NewFrame(): ImGui's own wheel jump is replaced by the eased target
// and an elastic overscroll (ImGuiWindow::ScrollOverscroll, GRIDLINE PATCH).
#pragma once
#include <cstdint>
#include "../core/SmoothScroll.h"

namespace gl {
class Settings;
}

namespace gl::scrollui {

// The parameters for this frame: Settings > Interface "Smooth scrolling" and
// "Elastic edges", both off while the system asks for reduced motion.
void configure(const Settings& s);
const smoothscroll::Params& params();
// The system's reduced-motion preference (macOS accessibility, GNOME
// animations; GRIDLINE_REDUCED_MOTION=0/1 overrides). Queried once.
bool systemReducedMotion();

// J-67 "smooth motion" family: snapped drags glide like J-37's clip moves
// (Settings > Interface "Smooth snapping" = ui.smoothSnap; off under
// reduced motion).
bool smoothSnap();
// J-91: Settings > Interface > Motion "Interface animations" (ui.animations;
// off under reduced motion): decorative motion such as the metronome tick
// and the key / pad light fades. Off also turns the three above off.
bool animations();
// Drawn position of a snapped value under a drag: glides to `target` while
// `dragging` (and smooth snapping is enabled), exactly `target` otherwise
// (pass dragging = false when the grid is off: no lag). `key`: a stable id.
double glide(uint64_t key, double target, bool dragging, double eps = 1e-3);

// Q-10: drawn height of an event-lane bar (0..1 value): glides to `target`
// with smooth::kBarTau whenever smooth snapping is on (the stored value has
// already changed: playback, undo and saving never wait), exactly `target`
// when it is off or the system asks for reduced motion. `key`: stable per bar.
double glideBar(uint64_t key, double target);

// ImGui scroll regions: call right before and right after ImGui::NewFrame().
void beforeNewFrame();
void afterNewFrame();

// One axis of a custom view, once per frame: adopts outside writes to `v`
// (zoom, fit, follow playback, scrollbar drag), applies `delta` px of input
// (`notch`: a wheel step, eased; else 1:1), eases and writes the drawn
// position (in [lo, hi] plus the elastic overscroll) back to `v`.
void step(smoothscroll::Axis& ax, float& v, float delta, bool notch, float lo, float hi);
// A view's own range clamp at the top of its frame: clamps `v` unless it is
// this axis' own drawn (possibly overscrolled) position.
void clampOuter(const smoothscroll::Axis& ax, float& v, float lo, float hi);
// Same for double positions in other units (audio editor: frames);
// `unitsPerPx` converts (the elastic bound and settle threshold stay in px).
// `elastic` false: hard edges for views that cannot draw past their content.
void stepUnits(smoothscroll::Axis& ax, double& v, double delta, bool notch, double lo, double hi, double unitsPerPx, bool elastic = true);

}  // namespace gl::scrollui
