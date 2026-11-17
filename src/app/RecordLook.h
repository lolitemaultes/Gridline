// Gridline - K-325: the recording windows' rows, drawn with the app's own form controls so the
// Record window and the latency check look like Settings and Render in every layout:
//   labels   ui::FormLabelFit on the Settings column (168; Clips moves it with its settings column,
//            Smooth right-aligns the labels to it)
//   groups   ui::BeginSection / EndSection (Studio and Tracker: recessed panels with a small
//            caption; Clips: a heading and a rule, switches as On / Off fields; Smooth: headings)
//   choices  ui::OptionRow (buttons, or a pop-up menu in Smooth); switches ImGui::Checkbox
//   buttons  ui::TextButton / PrimaryButton / DangerButton
// The Recording card and strip are not forms: there the Tracker keeps its bevel buttons and frame
// (TrackerLook.h). A FormScope marks the form windows.
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::reclook {

enum class Look { Studio, Clips, Smooth, Tracker };
Look look();
inline bool tracker() { return look() == Look::Tracker; }

constexpr float kG = 8.0f;          // the grid of the card and the strip
constexpr float kColumn = 168.0f;   // the Settings form's label column (ui::SettingsColumn's 168)
constexpr float kShortW = 56.0f;    // a short-value choice (1, 2, 4 / 5 ms / 30 s): its pop-up menu is narrow

// While one lives, the rows are a form (the Record window, the latency check): shared buttons in
// every layout. `fieldW`: the width of a pop-up menu choice (settingsFormBegin's).
struct FormScope {
  explicit FormScope(float fieldW);
  ~FormScope();
  FormScope(const FormScope&) = delete;
  FormScope& operator=(const FormScope&) = delete;
  float prevW;
  bool prev;
};
bool inForm();

// The card: square in the Tracker. Push before Begin, pop after it.
void pushWindowStyle();
void popWindowStyle();
// Right after Begin of the card: the Tracker's frame and well.
void cardFrame();

// A row's label on the form column (fitted, full text on hover). "" leaves the cell empty.
void rlLabel(const char* text, ImU32 col = 0);
// A value as text in the control column (`col` 0: the text colour).
void rlValue(const char* text, ImU32 col = 0);

// One of `count` (labels untranslated), as Settings draws a choice. `segW` kShortW: a short list
// (its pop-up menu narrow in Smooth). True when changed.
bool rlChoice(App& a, const char* id, const char* const* labels, int count, int* index, float segW, const char* hint = nullptr);
// A switch, as Settings draws one (a check box; Clips' sections show it as an On / Off field).
bool rlToggle(App& a, const char* id, bool* v, const char* hint = nullptr);

// Buttons. `w` 0: fit the label.
enum class Kind { Normal, Primary, Danger };
bool rlButton(App& a, const char* text, Kind kind = Kind::Normal, float w = 0, const char* hint = nullptr, bool on = false);
float buttonWidth(const char* text);
float buttonHeight();
// A footer button's width: Render's 96, or the label's when longer.
float footerButtonWidth(const char* text);

// Groups: rlSection ends the open group (if any) and starts the next, as a Settings section
// (caption in capitals, as Settings passes them); rlSectionsEnd closes the last one.
void rlSection(const char* caption);
void rlSectionsEnd();

}  // namespace gl::reclook
