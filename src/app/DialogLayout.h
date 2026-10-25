// Gridline — J-54 dialog layout helpers: footers that never push a needless
// scrollbar onto their window, form label columns sized to their labels, and
// text that is fitted with an ellipsis (full text in a tooltip) instead of
// running into the next column. Depends only on Dear ImGui and Theme, so the
// layout tests (tests/test_dialog_layout.cpp) run it headless.
#pragma once
#include <cstdint>
#include "../../third_party/imgui/imgui.h"

struct ImGuiWindow;

namespace gl::ui {

// Height of what follows a scrolling child at the bottom of a dialog: two kS1
// spacers and `rows` rows of frame height (fields / buttons), with the item
// spacing between all of them (including the gap under the child). Reserve it
// with BeginChild(id, ImVec2(0, -DialogFooterHeight(rows))): the window then
// fits its content exactly and shows no scrollbar.
float DialogFooterHeight(int rows = 1);
// Dialog footer: kS1, hairline, kS1, then the cursor right-aligned for
// `buttonsWidth` px of buttons (one row; DialogFooterHeight(1) reserves it).
void DialogFooter(float buttonsWidth);
// Width of `count` buttons of `width` px each, kS2 apart (for DialogFooter).
float ButtonRowWidth(int count, float width);

// Tallest a dialog may be: the work area between the top bar and the status bar.
float DialogMaxHeight();
// Room a dialog may take on screen (work area between the top bar and the
// status bar, less a kS2 margin all round).
ImVec2 DialogArea();
// `want` clamped to DialogArea() (0 components stay 0 = auto).
ImVec2 FitDialogSize(ImVec2 want);
// Before Begin(): centre the next dialog in the work area with size `want`
// clamped to the screen (`cond` for both), and cap its size at DialogArea()
// (pass the dialog's own minimum; it is clamped to the area too).
void PlaceNextDialog(ImVec2 want, ImGuiCond cond = ImGuiCond_Appearing, ImVec2 minSize = ImVec2(0, 0));
// Body of an auto-sizing dialog (ImGuiWindowFlags_AlwaysAutoResize): as large
// as its content, but never taller than leaves room for the footer
// (DialogFooterHeight(footerRows)) inside DialogMaxHeight(); beyond that the
// body scrolls and the footer buttons stay on screen. Always pair with
// EndDialogBody().
void BeginDialogBody(const char* id, int footerRows = 1);
void EndDialogBody();

// J-91: floating windows (channel and plugin windows, dialogs) stay inside the
// work area between the top bar and the status bar. The clamp must move a
// window before it is laid out: moving it after Begin() (0.6.0) left its
// title bar where the drag put it and pushed only its content down, an empty
// band under the title bar while a window was dragged against the top
// (GitHub issue #1).
// The position of a window of `size` kept in [areaMin, areaMax]: fully
// inside, or while the user drags it only its title bar (80 x 32 px).
ImVec2 ClampWindowPos(ImVec2 pos, ImVec2 size, ImVec2 areaMin, ImVec2 areaMax, bool dragging);
// The work area of the main viewport (between the top bar and the status bar).
void WorkArea(ImVec2& areaMin, ImVec2& areaMax);
// Right after Begin(): the current window is clamped from now on, before each
// frame's layout (and at once on the frame it appears).
void TrackWindowInViewport();
// Right after ImGui::NewFrame(), before any Begin(): keeps the tracked windows and
// every floating panel or editor (the channel rack, the mixer or piano roll undocked,
// plugin editors, floating dock nodes) in the work area (J-114: not the detached ones,
// the OS moves those). J-117: an elastic boundary (core/WindowRubber.h): a window
// dragged past an edge follows with growing resistance and springs back inside once
// it is let go or the work area shrinks; one that just opened is placed at once.
// Popups, menus, tooltips and docked windows are never touched.
void ClampTrackedWindows();
// J-117: the boundary stretches and springs (Settings > Interface "Elastic edges",
// off under reduced motion) or is a hard edge. On by default.
void SetWindowRubber(bool elastic);
// J-114: right after Begin() of a window that never docks (a plugin or effect editor): it still
// gets the pop-out / dock-back title-bar button.
void MarkDetachable();
bool IsMarkedDetachable(const ImGuiWindow* w);

// Two-column form: the x (from the window's content start) where controls
// begin, so the widest label fits with `gap` to spare, never less than `minX`
// and never more than `maxX`. Labels wider than that are fitted by FormLabelFit.
float FormColumn(const char* const* labels, int count, float minX, float maxX, float gap = 12.0f);
// Dim form label fitted to end `gap` before column `x` (ellipsis + full-text
// tooltip when cut), then SameLine(x) so the control starts on the column.
void FormLabelFit(const char* label, float x, ImU32 col, float gap = 12.0f);
// Text fitted to `maxW` with an ellipsis; when cut, hovering shows the full
// text. Uses the frame-padding baseline when `framed`. Returns true when cut.
bool TextFit(const char* text, float maxW, ImU32 col, bool framed = false);
// TextFit for paths: the ellipsis goes in the middle, so the root and the last
// folders stay readable; when cut, hovering shows the whole path.
bool PathFit(const char* text, float maxW, ImU32 col, bool framed = false);

}  // namespace gl::ui
