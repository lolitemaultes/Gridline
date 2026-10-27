// Gridline — custom controls: knobs, faders, meters, icon buttons, and the
// design-system building blocks shared by every window (see Theme.h).
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "DialogLayout.h"  // DialogFooter, DialogFooterHeight, form columns, fitted text (J-54)

namespace gl::ui {

enum class Icon {
  Play, Pause, Stop, Record, Plus, Minus, Close, Pattern, Song, Draw, Paint, Erase, Select, Slice, Mute,
  Metronome, Loop, Undo, Redo, Save, Open, NewFile, Keyboard, Browser, Rack, Piano, Playlist, Mixer, Settings,
  Folder, File, Audio, Plug, Up, Down, Left, Right, Magnet, Menu, Zoom, Help, Wave, Clone, Trash, More, Search,
  Chevron, ChevronDown,
  Slip, Detune, Stamp, Lock,  // J-11 tool glyphs
  Eye, EyeOff,                // Q-6: window shown / hidden
  // Q-12: editing tool family (Q-13: SVG icons, see iconSvgName):
  // playlist / piano roll / automation editor tool strips.
  ToolDraw, ToolPaint, ToolDelete, ToolMute, ToolSlip, ToolSlice, ToolSelect, ToolZoom, ToolPlay, ToolDetune, ToolSnap,
  // J-79: playlist audio clip editing toggles.
  StretchMode, FadeHandles, ZeroCross,
  // J-100: Shuttle envelope toolbar (step editing, linked slots).
  StepEdit, LinkSlots,
  // J-106: Convolver curve mode (smooth curve / straight segments).
  CurveSmooth, CurveStraight,
  // Gridline originals replacing hand-drawn glyphs (assets/icons/ui, NOTICE.md):
  // generic, windows, track header, playlist, mixer track icons, plugin
  // categories, plugin editor glyphs, Edison.
  ChevronLeft, ChevronUp, Check, Slash, Ring, Power, Question, Info, PlusMinus, ListBullets, GripDots, Star,
  StarFilled, WindowPopout, WindowDockin, Speaker, SpeakerWaves, InputJack, LoopArrows, ClipAutomation, ClipPattern,
  TrackKick, TrackSnare, TrackHats, TrackPercussion, TrackBass, TrackKeys, TrackSynth, TrackPad, TrackGuitar,
  TrackVocal, TrackEffects, TrackReverb, TrackDelay, TrackBus, TrackSidechain, TrackOutput, CatSynth, CatDrums,
  CatSampler, CatController, CatMidi, CatDelay, CatDynamics, CatFilter, CatEq, CatDistortion, CatModulation,
  CatSpectral, CatStereo, CatPitch, CatUtility, StoreTray, SwapArrows, DisplayBar, DisplayPeaks, DisplayEnvelope,
  DisplayLevels, DisplayDots, DisplayBars, DisplayLine, Snowflake, SlideArrow, Sine, DiceTwo, CirclesApart,
  CirclesMerged, EdisonLoopSet, EdisonLoop, Flag, Trim, SliceWave, FadeIn, FadeOut, Normalize, ReverbTail, Blur,
  EqCurve, EnvelopePoints, Stretch, Denoise, SendOut, Scrub, Follow, SpeakerSmall, SpeakerSmallOff, ScrollTo,
  Headphones,
  Heart, HeartFilled,  // J-148: favourite projects (welcome screen)
  People,  // J-146: collaborators (the welcome screen's Collaborative view)
  Count  // number of icons (not an icon)
};
// Q-12: true for the tool family (playlist / piano roll / automation editor strips).
inline bool isToolIcon(Icon i) { return i >= Icon::ToolDraw && i <= Icon::ToolSnap; }

// Q-13: every icon is an SVG on the 24-unit grid (assets/icons/ui/<name>.svg,
// Lucide / Tabler geometry, see NOTICE.md there), rasterised once per size into
// the icon atlas (src/app/icons) and drawn as a tinted, pixel-snapped quad.
// File stem of the icon's SVG (nullptr only for Icon::Count).
const char* iconSvgName(Icon icon);
// `size`: the glyph's visible box (the SVG box is 1.2x, snapped to 10/12/14/16/20/24/... px).
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);
// Same, `box` = the SVG's 24-unit box (snapped to a standard size).
void drawIconBox(ImDrawList* dl, Icon icon, ImVec2 center, float box, ImU32 col);
// The icon fitted to an older glyph's footprint: drawn as large as its ink (the
// pixels it covers, stroke included) fits in `inkMin`..`inkMax`, the ink centred
// there. Where an SVG replaced a hand-drawn glyph, pass the old glyph's ink
// bounds, so the icon never draws bigger than the glyph it replaced.
void drawIconFit(ImDrawList* dl, Icon icon, ImVec2 inkMin, ImVec2 inkMax, ImU32 col);
// Same, the ink box given by its centre and size.
inline void drawIconFitC(ImDrawList* dl, Icon icon, ImVec2 c, ImVec2 ink, ImU32 col) {
  drawIconFit(dl, icon, ImVec2(c.x - ink.x * 0.5f, c.y - ink.y * 0.5f), ImVec2(c.x + ink.x * 0.5f, c.y + ink.y * 0.5f), col);
}
// An icon drawn on an older glyph's unit grid (icons::drawGlyph): one old unit
// of `unitPx` logical px is 10 SVG units around the centre `c`; fitted to the
// old glyph's ink (its geometry plus half `oldStroke`), never bigger.
void drawIconGlyph(ImDrawList* dl, Icon icon, ImVec2 c, float unitPx, float oldStroke, ImU32 col);
// Icon labels: a text button caption that is an icon. GL_ICON_LABEL("chevron-left", "<")
// is the SVG chevron-left, laid out and sized like the text "<" in the button's font
// (it fits the character's ink bounds). Button helpers that draw a caption take
// these: they measure labelText(s) and draw it with drawLabel().
#define GL_ICON_LABEL(svg, text) "\x1b" svg "\x1f" text
// The text an icon label stands for ("<" above), or `s` itself.
const char* labelText(const char* s);
// J-128: size of `s` in the current font without ImGui::CalcTextSize's rounding
// up of the width (which pulls every centred label up to half a pixel left):
// for centring text on a control.
ImVec2 TextSizeExact(const char* s);
// Draws the caption `s` at `pos` (as ImDrawList::AddText with `font` / `size`):
// an icon label draws its SVG over the ink of its text, anything else draws as text.
void drawLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* s);
// Same with the current font and size.
void drawLabel(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* s);
// Installs the current ImGui context's glyph hook (ImGui's own arrows, check
// marks, close crosses and step buttons as SVG icons); call once per context.
void installGlyphHook();
// Square icon button; `active` draws it highlighted (Paper fill, or activeCol).
// Dark theme: the active glyph lights in `activeCol` (tool role colour) or the
// lit-glyph orange; `idleCol` tints the glyph at rest (e.g. menu buttons).
// `onPress`: acts when the button goes down (transport, J-91), not on release.
bool IconButton(const char* id, Icon icon, bool active = false, float size = 26.0f, const char* hint = nullptr,
                ImU32 activeCol = 0, ImU32 idleCol = 0, bool onPress = false);
// Icon toggle for "show/enable" states: quiet when off, bright icon + accent
// underline when on (window toggles, metronome, typing keyboard).
// `mirror` scales the glyph horizontally about its centre (-1 .. 1: the
// metronome's tick-tock: +1 / -1, J-91 / J-97).
bool ToggleIcon(const char* id, Icon icon, bool on, float size = 28.0f, const char* hint = nullptr, float mirror = 1.0f);
// Q-6: show / hide toggle for a plugin's window: an open eye while the window
// is shown (lit), an eye with a slash while it is hidden; the tooltip says
// what a click does. Returns true when clicked.
inline Icon eyeIcon(bool shown) { return shown ? Icon::Eye : Icon::EyeOff; }
// English (i18n::N_), translated by the tooltip: what a click does.
inline const char* eyeHint(bool shown) { return shown ? i18n::N_("Hide the plugin's window") : i18n::N_("Show the plugin's window"); }
bool EyeToggle(const char* id, bool shown, float size = 22.0f);
// Q-12: a tool strip button. The glyph is the SVG icon (Q-13, 20 px box in a
// 24 px control, pixel-snapped), lit in `roleCol` when active (FL-style lit glyph on a
// tinted face with an underline), hover / pressed faces from the theme. The
// tooltip reads "Name (key): description" with the key from the keymap
// command `keyId` (no key part when unbound). English name / description
// (i18n::N_ literals) are translated here. Returns true when clicked.
bool ToolButton(const char* id, Icon icon, bool active, float size, const char* name, const char* desc, const char* keyId, ImU32 roleCol = 0);
// Tooltip text of a tool button (translated, key from the keymap).
std::string toolTip(const char* name, const char* desc, const char* keyId);
// Q-12: call once a frame per tool strip with the active tool: for a moment
// after the tool changes (click or shortcut) the status hint names it.
void noteActiveTool(const char* strip, int tool, const char* name, const char* keyId);
// Text button with fixed width (0 = fit).
bool TextButton(const char* label, bool active = false, float width = 0.0f, const char* hint = nullptr);
// Destructive action (leave, stop sharing, reset): the label in the danger
// colour on the normal face; `armed` (the confirming click) fills the face with
// the danger colour. Same size and translation rules as TextButton.
bool DangerButton(const char* label, bool armed = false, float width = 0.0f, const char* hint = nullptr);
// Width a TextButton / DangerButton with this English label takes (0 = fit).
float TextButtonWidth(const char* label);
// Small status lamp (8 px) on the text line, then SameLine: "online", "offline" rows.
void StatusDot(ImU32 col, bool pulse = false);
// Segmented control (one of n). Returns true when *index changed.
bool Segmented(const char* id, const char* const* labels, int count, int* index, float segWidth = 0.0f,
               float height = 0.0f, const char* const* hints = nullptr);

// Knob bound to a document parameter: drag vertically (Shift = fine), wheel,
// double-click resets (types at the default, J-136), Alt+click types, right-click
// menu (reset, type, links, automation). One undo step per gesture.
bool ParamKnob(const char* id, ParamId pid, float radius = 14.0f, const char* label = nullptr, bool bipolar = false);
// Vertical fader bound to a parameter.
bool ParamFader(const char* id, ParamId pid, ImVec2 size);
// J-83: wheel routing (core/WheelRoute.h). Inside a scope whose pane scrolls
// (the thin mixer, or Settings > Interface "Mouse wheel over mixer controls:
// scroll") the plain wheel over a knob / fader / slot scrolls the pane; the
// control takes it with the wheel-adjust modifier (Ctrl / Cmd, fine steps) or
// after a click on it (wheel focus); a press while the pane is being scrolled
// starts no drag. Outside a scope, or in a pane that adjusts, nothing changes.
class WheelPolicyScope {
 public:
  explicit WheelPolicyScope(bool paneScrolls);
  ~WheelPolicyScope();
  WheelPolicyScope(const WheelPolicyScope&) = delete;
  WheelPolicyScope& operator=(const WheelPolicyScope&) = delete;

 private:
  bool prev_;
};
// The innermost scope scrolls its pane with the plain wheel.
bool wheelPaneScrolls();
// A pane scroll gesture (or its momentum) is running.
bool wheelScrolling();
// A control has wheel focus and the pointer is on it.
bool wheelFocusActive();
// The wheel-adjust modifier (keymap "mixer.wheelAdjust") is held.
bool wheelModifierHeld();
// A control used this frame's wheel (the gesture is not a pane scroll).
void wheelClaim();
// Per wheel-sensitive control, right after its item. `takesWheel`: the
// control's own rule in an adjusting pane (e.g. a modifier). `scale` > 0: the
// control applies this frame's wheel times `scale`; `dragOk`: a drag may move
// it; `hint`: the wheel scrolls the pane here (show "Drag to change, ...").
struct WheelGate {
  float scale = 0;
  bool dragOk = true;
  bool hint = false;
};
WheelGate wheelGate(bool takesWheel = true);
// Translated "Drag to change, Ctrl+wheel to fine-tune".
const char* wheelHintText();
// Around every frame (after ImGui::NewFrame / before ImGui::Render).
void wheelFrameBegin();
void wheelFrameEnd();

// Free knob over a value with descriptor (no document); returns true while changing.
bool Knob(const char* id, double* value, const ParamDesc& d, float radius, const char* label, bool bipolar,
          bool* released = nullptr, const std::function<void()>& extraMenu = nullptr);

// ---- J-136: reset and type-in, the same on every parameter knob and slider ----
// Double-click: back to the default when the value differs from it (one undo
// step), else the type-in popup. Right-click menu: "Reset to default" first,
// greyed when the value already is the default. Ctrl+click reset stays.
//
// The value when the last item's double-click began: call right after the
// control's item, before the control changes the value this frame (a press
// that nudges the value does not turn the double-click into a reset).
double ValueAtPress(double current);
// The double-click on the last item: Reset (when `pressed` differs from the
// default) or Type (when it is the default); false without a double-click.
bool DoubleClicked(const ParamDesc& d, double pressed, ParamDoubleClick* action);
// "Reset to default" for a control's right-click menu (greyed at the default).
bool ResetMenuItem(const ParamDesc& d, double value);
// Bound to a document parameter: the double-click resets pid (one undo step;
// returns true, `val` = the default) or opens the popup "##type" (false).
bool ParamDoubleClickReset(ParamId pid, const ParamDesc& d, double& val, double pressed);
// The menu item above for pid; true (and `val` = default) when chosen.
bool ParamResetMenuItem(ParamId pid, const ParamDesc& d, double& val);
// The type-in popup "##type" for pid (open it with ImGui::OpenPopup("##type")).
void ParamTypePopup(ParamId pid, const ParamDesc& d);
// The same popup over a free value: true when a value was entered.
bool ValueTypePopup(const ParamDesc& d, double* value);
// Hint text of the mouse gestures of a parameter control.
const char* ParamControlHint();

// Stereo peak meter with hold; `key` keeps the per-meter decay state.
void Meter(ImGuiID key, float peakL, float peakR, ImVec2 size, bool vertical = true);

// J-137: the drawing alone of Knob / ParamKnob, ParamFader and Meter, in the
// control style in use (a ControlStyleScope picks another). The Theme page's
// style previews draw with these, so a preview is the real control.
void DrawKnob(ImDrawList* dl, ImVec2 centre, float radius, float norm, bool bipolar, bool hov, bool act);
void DrawFader(ImDrawList* dl, ImVec2 p, ImVec2 size, float norm, float defNorm, bool hov, bool act);
void DrawMeter(ImDrawList* dl, ImVec2 p, ImVec2 size, float gainL, float gainR, float holdL, float holdR, bool vertical = true);

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
// `name` is the English title: the visible title is its translation, the
// ImGui id stays the English name ("Mixer" -> "Mischpult###Mixer", J-61), so
// saved layouts, DockBuilderDockWindow / FindWindowByName / SetWindowFocus
// with the English name keep working in every language.
bool BeginDockWindow(const char* name, bool* open, ImVec2 firstSize);
void EndDockWindow();
// "<translated title>###<english title>" for ImGui::Begin (J-61).
std::string windowTitle(const char* english);
// The stable part of a window name: after "###" when present (compare this,
// not ImGuiWindow::Name, with the English title).
const char* stableWindowName(const char* name);
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
// J-137: the song-position outline of a step pad (the step playing now), drawn
// inside the pad's own rectangle: the step viewport's clip and the controls
// beside the first step never cut or cover it.
void StepPlayOutline(ImDrawList* dl, ImVec2 a, ImVec2 b);
// Number LCD (tempo, song position): mono bold, centred in a sunken field.
void Lcd(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* text, bool hovered, bool active);

const uint32_t* contentPalette(int* count);  // colours offered for channels/patterns/clips

}  // namespace gl::ui
