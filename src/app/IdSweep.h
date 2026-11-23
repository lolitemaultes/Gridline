// Gridline — 0.7.8: Dear ImGui ID-conflict checks for scripted runs (--dev-input).
// Dear ImGui (1.92) finds conflicting IDs on hover: while an item is hovered, every item
// submitted next frame with the same ID counts (ItemHoverable: HoveredIdPreviousFrameItemCount);
// more than one sets ImGuiContext::DebugDrawIdConflictsId at the next NewFrame and EndFrame
// draws the "Programmer error: N visible items with conflicting ID!" tooltip. This module
// reads that same state after every frame (the dev counter) and can sweep the pointer over
// every item of some windows so each one is hovered once.
//   idconflicts                 the counter since the start: frames with the tooltip, distinct IDs
//   idsweep <filter> [max] [dwell]  the pointer to the centre of every visible item, [dwell]
//                               frames each (default 1), in
//                               the windows whose name (or a parent's) contains <filter> ('|'
//                               separates alternatives, '*' is every window); items that appear
//                               during the sweep (submenus, popups) are visited next; at most
//                               [max] items (default 3000); a swept window that scrolls is
//                               scrolled on and swept again until its end. Ends with "idsweep done ..."
//                               and the signal idsweep-done ("waitfor idsweep-done <s>")
//   idsweep stop                ends the sweep now
//   idsweep demo [frames]       a small window with two buttons of one ID, for [frames] frames
//                               (default 600): the check's own check
#pragma once
#include <string>
#include <vector>

struct ImGuiIO;

namespace gl::idsweep {

// After the dev input of the frame (logical pixels): the sweep's pointer for this frame.
void frame(ImGuiIO& io, int frameNo);
// With the frame's windows, before ImGui::Render(): "idsweep demo"'s window.
void drawDemo();
// Right after ImGui::Render(): reads Dear ImGui's conflict state (prints when `dev`).
void afterRender(bool dev, int frameNo);
// "idconflicts" / "idsweep ..."; false for other commands.
bool devCommand(const std::string& cmd, const std::vector<std::string>& args, int frameNo);

}  // namespace gl::idsweep
