// Gridline - K-325: the recording windows' rows with the app's own form controls. See RecordLook.h.
#include "RecordLook.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "../core/I18n.h"
#include "../core/LayoutPreset.h"
#include "App.h"
#include "DialogLayout.h"
#include "LayoutPresets.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "Widgets.h"

namespace gl::reclook {

using theme::pal;

Look look() {
  switch (layoutui::active()) {
    case layout::Preset::Live: return Look::Clips;
    case layout::Preset::Logic: return Look::Smooth;
    case layout::Preset::Tracker: return Look::Tracker;
    default: return Look::Studio;
  }
}

namespace {
int gPushed = 0;
bool gForm = false;
float gFieldW = 200.0f;
int gSections = 0;
}  // namespace

FormScope::FormScope(float fieldW) : prevW(gFieldW), prev(gForm) {
  gForm = true;
  gFieldW = fieldW;
}
FormScope::~FormScope() {
  gForm = prev;
  gFieldW = prevW;
}
bool inForm() { return gForm; }

void pushWindowStyle() {
  gPushed = 0;
  if (tracker()) {  // the classic dialog: square, its own bevel edges instead of the border
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    gPushed = 2;
  }
}
void popWindowStyle() {
  if (gPushed > 0) ImGui::PopStyleVar(gPushed);
  gPushed = 0;
}
void cardFrame() {
  if (tracker()) trackerview::dialogFrame();
}

void rlLabel(const char* text, ImU32 col) {
  ui::FormLabelFit(text && *text ? i18n::tr(text) : "", kColumn, col ? col : pal().textDim);
}

void rlValue(const char* text, ImU32 col) {
  ImGui::AlignTextToFramePadding();
  ui::TextFit(text, std::max(40.0f, ImGui::GetContentRegionAvail().x), col ? col : pal().text, true);
}

bool rlChoice(App&, const char* id, const char* const* labels, int count, int* index, float segW, const char* hint) {
  std::vector<const char*> tr(static_cast<size_t>(std::max(0, count)));
  for (int i = 0; i < count; ++i) {
    tr[static_cast<size_t>(i)] = i18n::tr(labels[i]);
  }
  // Settings' choices: a row of buttons, or in Smooth a pop-up menu (sample rate's 160 for short lists).
  const float menuW = segW > 0 && segW <= kShortW ? 160.0f : std::max(160.0f, gFieldW);
  const bool changed = ui::OptionRow(id, tr.data(), count, index, menuW);
  if (hint) ui::Hint(hint);
  return changed;
}

bool rlToggle(App&, const char* id, bool* v, const char* hint) {
  const bool ch = ImGui::Checkbox(id, v);
  if (hint) ui::Hint(hint);
  return ch;
}

float buttonWidth(const char* text) {
  if (tracker() && !gForm) return std::max(64.0f, std::ceil(ImGui::CalcTextSize(i18n::tr(text)).x + 3 * kG));
  return ui::TextButtonWidth(text);
}
float buttonHeight() { return tracker() && !gForm ? trackerview::dialogButtonHeight() : ImGui::GetFrameHeight(); }
float footerButtonWidth(const char* text) { return std::max(96.0f, ui::TextButtonWidth(text)); }

bool rlButton(App& a, const char* text, Kind kind, float w, const char* hint, bool on) {
  if (tracker() && !gForm) return trackerview::dialogButton(a, text, w > 0 ? w : buttonWidth(text), on, hint);
  switch (kind) {
    case Kind::Primary: return ui::PrimaryButton(text, w, hint);
    case Kind::Danger: return ui::DangerButton(text, false, w, hint);
    default: return ui::TextButton(text, on, w, hint);
  }
}

void rlSection(const char* caption) {
  rlSectionsEnd();
  ui::BeginSection(i18n::tr(caption));
  ++gSections;
}
void rlSectionsEnd() {
  for (; gSections > 0; --gSections) {
    ui::EndSection();
  }
}

}  // namespace gl::reclook
