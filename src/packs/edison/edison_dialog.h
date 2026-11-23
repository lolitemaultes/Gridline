// Gridline — "edison" pack: the frame every Loupe tool dialog shares (edison_tools_ui.cpp):
// a scope line, titled sections with a label column, Gridline's dialog sliders, preset
// buttons of one width and the house footer. Used by the tool and repair dialogs.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <vector>
#include "../../app/Theme.h"
#include "edison_ui.h"

namespace gl::packs::edison::dlg {

constexpr float kDlgW = 420;      // section width of the slider dialogs
constexpr float kDlgWideW = 540;  // graph dialogs
constexpr float kBtnW = 96;       // footer and preset buttons (the house dialogs' button width)
constexpr float kValueW = 96;     // a slider's value field (the house button width)

const char* toolTitle(Tool t);
// The label column of a dialog: wide enough for its widest label (translated).
float column(std::initializer_list<const char*> labels);
// A titled section of the given width (pair with ui::EndSection()).
void BeginSection(const char* caption, float width);
// One form row: the label, then the next item starts on the column and fills the section.
void row(const char* label, float col);
// Gridline's dialog slider (rail, handle, value field; double-click the value to type one).
bool slider(const char* label, float col, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0);
bool combo(const char* label, float col, int* v, const char* const* items, int n);
// A dim note line inside a section (wraps at the section's edge).
void note(const char* text);
// A status note that changes while a job runs ("Measuring... 42%", then its result): `rows` rows
// kept from the start, the text wrapped inside them, so the dialog never changes height.
void statusNote(const char* text, int rows = 2);
// Preset buttons: one width, one gap; returns the clicked index or -1.
int presets(std::initializer_list<const char*> labels);
// What the tool applies to, above its section.
void scopeLine(App& a, const Session& s, Range tr);
// The footer: Cancel and Accept (primary), right-aligned. True on Accept.
bool footer(bool enabled = true);

// Point editor in a graph: x / y mapping functions, drag points, double-click
// adds, right-click removes (first and last stay when `pinEnds`).
template <class P, class ToScreen, class FromScreen>
void pointGraph(const char* id, std::vector<P>& pts, ImVec2 size, ToScreen toS, FromScreen fromS, bool pinEnds, int& drag,
                const std::function<void(ImDrawList*, ImVec2, ImVec2)>& grid) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
  ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  skin::insetField(dl, p0, p1, C(col::WaveBg), theme::kRadius);
  grid(dl, p0, p1);
  const ImVec2 m = ImGui::GetIO().MousePos;
  int hot = -1;
  for (size_t i = 0; i < pts.size(); ++i) {
    ImVec2 q = toS(pts[i], p0, p1);
    if (std::fabs(q.x - m.x) < 7 && std::fabs(q.y - m.y) < 7) hot = static_cast<int>(i);
  }
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = hot;
  if (ImGui::IsItemActive() && drag >= 0 && drag < static_cast<int>(pts.size())) {
    P np = fromS(ImVec2(std::clamp(m.x, p0.x, p1.x), std::clamp(m.y, p0.y, p1.y)), p0, p1);
    if (pinEnds && (drag == 0 || drag + 1 == static_cast<int>(pts.size()))) np.first = pts[static_cast<size_t>(drag)].first;
    pts[static_cast<size_t>(drag)] = np;
  }
  if (ImGui::IsItemDeactivated()) drag = -1;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0)
    pts.push_back(fromS(m, p0, p1));
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hot >= 0 && pts.size() > 2 && !(pinEnds && (hot == 0 || hot + 1 == static_cast<int>(pts.size()))))
    pts.erase(pts.begin() + hot);
  std::stable_sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.first < b.first; });
  // Curve.
  std::vector<ImVec2> line;
  for (const auto& q : pts) line.push_back(toS(q, p0, p1));
  if (line.size() > 1) dl->AddPolyline(line.data(), static_cast<int>(line.size()), C(col::Wave), 0, 2.f);
  for (size_t i = 0; i < line.size(); ++i) dl->AddCircleFilled(line[i], static_cast<int>(i) == hot ? 6.f : 4.5f, static_cast<int>(i) == hot ? C(col::WaveSel) : C(col::Wave));
}

// Small grid label (mono caption size) at a point.
inline void gridText(ImDrawList* dl, ImVec2 p, const char* t) {
  ImFont* f = app().fontMono ? app().fontMono : ImGui::GetFont();
  dl->AddText(f, theme::kFontLabel, ImVec2(std::round(p.x), std::round(p.y)), C(col::TextDim), t);
}

struct EqPt {
  double first, second;  // hz, db
};

}  // namespace gl::packs::edison::dlg
