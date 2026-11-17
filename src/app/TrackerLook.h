// Gridline - the Tracker layout's drawing pieces (K-106), shared by its header band
// (TrackerFrame.cpp), the pattern editor and its pages (TrackerPages.cpp): bevelled boxes and
// buttons in the theme's roles, numbers in the monospaced face, two-digit hex numbering.
#pragma once
#include <initializer_list>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::trackerview {

ImFont* monoFont(App& a);
// K-106 cp23 (polish): the band's one spacing scale, in pixels at the interface size. Every screen
// of the Tracker spaces its parts from these (before: 1, 2, 3, 4, 6, 8 and 10 px chosen place by place).
namespace space {
constexpr float kLine = 1;   // between the rows of a stacked button column
constexpr float kGap = 2;    // between peer controls (a row of buttons, a field and its arrows, two boxes)
constexpr float kPad = 4;    // inside a box: its edge to its controls
constexpr float kGroup = 6;  // between groups in one row or column (a label and its controls, two sets)
}  // namespace space
// cp23: a label column wide enough for the widest of `labels` (translated) plus `pad`, so the fields
// of a group of rows (steppers, readouts) start at one edge.
float labelColumn(App& a, std::initializer_list<const char*> labels, float pad = space::kGroup);
// cp21 (look audit): hard pixel edges for the lines and shapes drawn while it lives (check ticks,
// radio diamonds, arrows, envelopes, wave and shape glyphs), as the classic screens draw them: the
// draw list's anti-aliasing is off in its scope and back as it was after. Text is not affected.
class Aliased {
 public:
  explicit Aliased(ImDrawList* dl) : dl_(dl), flags_(dl ? dl->Flags : 0) {
    if (dl_) dl_->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex | ImDrawListFlags_AntiAliasedFill);
  }
  ~Aliased() {
    if (dl_) dl_->Flags = flags_;
  }
  Aliased(const Aliased&) = delete;
  Aliased& operator=(const Aliased&) = delete;

 private:
  ImDrawList* dl_;
  ImDrawListFlags flags_;
};
// The values' ink (the hot colour, darkened until it reads on the sunken fields).
ImU32 valueInk();
// cp18 (were file-local): a radio row (a small diamond, lit when on, then the label) and a check
// row (a sunken square, ticked when on, then the label). The whole row takes the click.
bool radioRow(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint = nullptr);
bool checkRow(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint = nullptr);
// A bevelled box: a light edge top and left, a dark one bottom and right (sunken: the other way).
void bevelBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool sunken = false);
// A bevel button: raised; pressed (inset) while held; lit when `on`; a lighter face on hover.
bool bevelButton(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on = false, const char* hint = nullptr);
// A box title in the bold face.
void boxTitle(App& a, ImVec2 p, const char* text);
// A small arrow button (a filled triangle on a bevel face); repeats while held.
bool arrowButton(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool up, const char* hint);
// cp17: a sideways scroll bar (bevel trough, raised thumb, an arrow button at each end). `start`
// and `view` are in the content's units (of `total`); the arrows step by `step`, a click beside
// the thumb pages. True when `start` changed (it stays inside 0 .. total - view).
bool scrollBar(App& a, const char* id, ImVec2 p0, ImVec2 p1, double* start, double view, double total, double step, const char* hint = nullptr);
// cp24: the same bar standing up (the arrows at the top and the foot), for the pages' lists.
bool vScrollBar(App& a, const char* id, ImVec2 p0, ImVec2 p1, double* start, double view, double total, double step, const char* hint = nullptr);
// cp17 (moved from TrackerPages.cpp): a horizontal value bar (volume, pan) in a sunken box, a raised
// knob at the value; dragging sets it (*out), a double-click the default. True while it changed.
bool valueBar(const char* id, ImVec2 p0, ImVec2 p1, double v, double lo, double hi, bool centred, double* out, const char* hint = nullptr);
// cp17: a modal box in the band's look (the classic centred dialog: a raised bevel frame, its title
// in the bold face, the rest of the screen dimmed and inert). Open it with ImGui::OpenPopup(id);
// between beginModalBox (true) and endModalBox the caller draws its rows. Esc closes it.
bool beginModalBox(App& a, const char* id, const char* title, ImVec2 size);
void endModalBox();
// K-325 (were Dialogs.cpp's own): the classic dialog pieces for the shared windows when they show in
// the Tracker. dialogFrame: inside a window with no title bar (a question box, the Recording card), a
// raised bevel frame round a sunken well over the whole window; drawn first, under its rows.
// windowFrame: a window that keeps its title bar (the Record window, the latency check): only the
// raised bevel edges round it (nothing filled), called last, before End(). dialogButton: a bevel button `w` wide at the cursor
// (font height + 10 tall), the cursor moved past it as an ImGui button does; `on` lights it.
void dialogFrame();
void windowFrame();
float dialogButtonHeight();
bool dialogButton(App& a, const char* label, float w, bool on = false, const char* hint = nullptr);
// One stepper row of width `w`: the label, the value in a sunken field after it (at `labelW` from
// the left; < 0: 42 percent), the up and down arrows at the right. Returns the step asked for: +1 or
// -1 from an arrow or the wheel, more while the value is dragged (right or up: more). `fill`: the
// value's field runs from the label column to the arrows. `typed` (optional, K-297 round 7): a click
// on the value (no drag) opens it for typing; Enter or a click elsewhere stores the number typed there
// (Esc keeps the old value). While it is open, keys (Space too) go to the field.
int stepper(App& a, const char* id, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint, float labelW = -1,
            bool fill = false, double* typed = nullptr);

}  // namespace gl::trackerview
