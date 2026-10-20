// Gridline — custom controls: knobs, faders, meters, icon buttons, and the
// design-system building blocks shared by every window (see Theme.h).
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"
#include "DialogLayout.h"  // DialogFooter, DialogFooterHeight, form columns, fitted text (J-54)

namespace gl::ui {

enum class Icon {
  Play, Pause, Stop, Record, Plus, Minus, Close, Pattern, Song, Draw, Paint, Erase, Select, Slice, Mute,
  Metronome, Loop, Undo, Redo, Save, Open, NewFile, Keyboard, Browser, Rack, Piano, Playlist, Mixer, Settings,
  Folder, File, Audio, Plug, Up, Down, Left, Right, Magnet, Menu, Zoom, Help, Wave, Clone, Trash, More, Search,
  Chevron, ChevronDown,
  Slip, Detune, Stamp, Lock  // J-11 tool glyphs
};

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);
// Square icon button; `active` draws it highlighted (Paper fill, or activeCol).
// Dark theme: the active glyph lights in `activeCol` (tool role colour) or the
// lit-glyph orange; `idleCol` tints the glyph at rest (e.g. menu buttons).
bool IconButton(const char* id, Icon icon, bool active = false, float size = 26.0f, const char* hint = nullptr,
                ImU32 activeCol = 0, ImU32 idleCol = 0);
// Icon toggle for "show/enable" states: quiet when off, bright icon + accent
// underline when on (window toggles, metronome, typing keyboard).
bool ToggleIcon(const char* id, Icon icon, bool on, float size = 28.0f, const char* hint = nullptr);
// Text button with fixed width (0 = fit).
bool TextButton(const char* label, bool active = false, float width = 0.0f, const char* hint = nullptr);
// Segmented control (one of n). Returns true when *index changed.
bool Segmented(const char* id, const char* const* labels, int count, int* index, float segWidth = 0.0f,
               float height = 0.0f, const char* const* hints = nullptr);

// Knob bound to a document parameter: drag vertically (Shift = fine), wheel,
// double-click resets, Alt+click / right-click type a value. One undo step per gesture.
bool ParamKnob(const char* id, ParamId pid, float radius = 14.0f, const char* label = nullptr, bool bipolar = false);
// Vertical fader bound to a parameter.
bool ParamFader(const char* id, ParamId pid, ImVec2 size);
// Free knob over a value with descriptor (no document); returns true while changing.
bool Knob(const char* id, double* value, const ParamDesc& d, float radius, const char* label, bool bipolar,
          bool* released = nullptr, const std::function<void()>& extraMenu = nullptr);

// Stereo peak meter with hold; `key` keeps the per-meter decay state.
void Meter(ImGuiID key, float peakL, float peakR, ImVec2 size, bool vertical = true);

// Small coloured swatch that opens a palette popup; returns true when changed.
bool ColorSwatch(const char* id, uint32_t* rgb, ImVec2 size);

// Sets the hint bar text when the last item is hovered. The const char*
// overload builds no string unless the item is hovered (cheap per frame).
void Hint(const std::string& text);
void Hint(const char* text);
// True when the last item is hovered for hint purposes: guard hints whose
// text is concatenated, so the string is built only when it is shown.
bool HintHovered();
// Section label in small caps (mono).
void Label(const char* text);
// Delayed tooltip for the last item (design-system tooltip style).
void Tooltip(const char* text);
// 1.5 px accent focus ring around the last item while it is active/focused.
void FocusRing();

// Combo box with a slim chevron instead of the square arrow button. Same
// contract as ImGui::BeginCombo (call ImGui::EndCombo when it returns true).
bool BeginCombo(const char* id, const char* preview, float width);

// ---- floating windows ----
// Fixed-width floating editor (instrument / effect windows): the height grows
// with the content up to the space between the top bar and the status bar,
// then the window scrolls. Call before Begin().
void NextFloatingWindowSize(float width);
// Call right after Begin(): pulls the window back inside the work area if a
// resize (of the window or the app) left it partly off screen.
void KeepWindowInViewport();

// ---- layout building blocks ----
// Dock window with zero outer padding (toolbars run edge to edge). Always call
// EndDockWindow() (even when it returns false).
bool BeginDockWindow(const char* name, bool* open, ImVec2 firstSize);
void EndDockWindow();
// Full-width toolbar strip (theme::kToolbarH) with a hairline under it. Controls
// are vertically centred; text should use ImGui::AlignTextToFramePadding().
void BeginToolbar(const char* id);
void EndToolbar();
// Thin vertical divider between toolbar groups.
void ToolbarDivider();
// Empty state: muted 80 px mark (guide §08 C) with a title and one line of
// help, centred in [min, max]; shrinks gracefully (mark hides when cramped).
void EmptyState(ImDrawList* dl, ImVec2 min, ImVec2 max, const char* title, const char* body);
// Dialog group panel (studio forms group their controls in recessed boxes with
// a small caption): contents are laid out inside with kS2 padding. `width` 0 =
// the remaining width of the window. Nestable; always pair with EndSection().
void BeginSection(const char* caption, float width = 0.0f);
void EndSection();
// Studio tab strip (settings / channel windows): text tabs on a baseline, the
// selected one raised with a lit-orange overline. Returns true when changed.
bool TabStrip(const char* id, const char* const* labels, int count, int* index, const char* const* hints = nullptr);
// Drag-and-drop preview chip (call between BeginDragDropSource/End): a tint
// bar (content colour, or the lit orange), the kind icon and the item name.
void DragPreview(Icon icon, const char* text, ImU32 tint = 0);
// Round LED (mute/enable lamps). `level` 0..1 brightens it.
void Led(ImDrawList* dl, ImVec2 center, float radius, bool on, float level = 0.0f);
// Step button (channel rack): rounded pad with a top bevel; `alt` = the
// alternate 4-step group tint; `on` lights it (tinted by `tint`), `vel` 0..1.
void StepPad(ImDrawList* dl, ImVec2 a, ImVec2 b, bool on, bool alt, bool hovered, ImU32 tint, float vel);
// Number LCD (tempo, song position): mono bold, centred in a sunken field.
void Lcd(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* text, bool hovered, bool active);

const uint32_t* contentPalette(int* count);  // colours offered for channels/patterns/clips

}  // namespace gl::ui
