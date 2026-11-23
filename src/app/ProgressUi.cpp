// Gridline — steady progress windows (see ProgressUi.h).
#include "ProgressUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "DialogLayout.h"
#include "Theme.h"

namespace gl::progui {

namespace {

// The row of the current line reaches `x` (screen): an auto-sizing window is as wide as its
// widest row, so a row that always reaches the same x keeps the window's width.
void reach(float x) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  w->DC.CursorMaxPos.x = std::max(w->DC.CursorMaxPos.x, x);
}

Fake gFake;
bool gFaking = false;
bool gTrace = false;
struct Noted {
  ImVec2 pos, size;
  int frame = -1;
};
std::map<std::string, Noted>& noted() {
  static std::map<std::string, Noted> m;
  return m;
}

}  // namespace

namespace {
std::string percentOf(int pc) {
  char b[32];
  std::snprintf(b, sizeof b, i18n::tr("%d%%"), pc);
  return b;
}
}  // namespace

float PercentWidth() { return ImGui::CalcTextSize(percentOf(100).c_str()).x; }

std::string PercentText(double frac) { return percentOf(static_cast<int>(std::floor(std::clamp(frac, 0.0, 1.0) * 100.0 + 1e-9))); }

void RightField(const char* text, float w, ImU32 col) {
  const float x0 = ImGui::GetCursorPosX(), sx = ImGui::GetCursorScreenPos().x;
  const char* shown = theme::fitText(text, w);
  const float tw = ImGui::CalcTextSize(shown).x;
  ImGui::SetCursorPosX(x0 + std::max(0.0f, w - tw));
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(shown);
  ImGui::PopStyleColor();
  reach(sx + w);
}

void Percent(double frac, ImU32 col) { RightField(PercentText(frac).c_str(), PercentWidth(), col); }

void Line(const char* text, float w, ImU32 col, bool path) {
  const float sx = ImGui::GetCursorScreenPos().x;
  if (!text || !*text) ImGui::TextUnformatted("");
  else if (path) ui::PathFit(text, w, col);
  else ui::TextFit(text, w, col);
  reach(sx + w);
}

float RowsHeight(int rows) { return static_cast<float>(std::max(1, rows)) * ImGui::GetTextLineHeight(); }

void WrappedRows(const char* text, float w, int rows, ImU32 col) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = RowsHeight(rows);
  if (text && *text) {
    const ImVec4 clip(p.x, p.y, p.x + w, p.y + h);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), p, col, text, nullptr, w, &clip);
  }
  ImGui::Dummy(ImVec2(w, h));
}

void note(const char* name) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w) return;
  Noted& n = noted()[name];
  n.pos = w->Pos;
  n.size = w->Size;
  n.frame = ImGui::GetFrameCount();
  if (gTrace) {
    // (a window that is not shown yet, its first frame, reports it: the test skips those)
    std::printf("progsize frame %d %s %.0f %.0f %.0f %.0f%s\n", n.frame, name, n.pos.x, n.pos.y, n.size.x, n.size.y,
                w->Hidden || w->Appearing ? " appearing" : "");
    std::fflush(stdout);
  }
}

const Fake* fake(const char* name) { return gFaking && gFake.popup == name ? &gFake : nullptr; }

const std::vector<std::string>& popups() {
  static const std::vector<std::string> v = {"filejob", "render",       "projectload", "flpexport", "samplerrender",
                                             "stems",   "trackreceipt", "urlimport",   "urlsetup",  "update"};
  return v;
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  (void)a;
  const std::string what = args.empty() ? "rects" : args[0];
  if (gTrace && (what == "show" || what == "off")) {  // (the trace counts the interface's frames)
    std::printf("progsize at %d %s %s\n", ImGui::GetFrameCount(), what.c_str(), what == "show" && args.size() >= 2 ? args[1].c_str() : gFake.popup.c_str());
    std::fflush(stdout);
  }
  if (what == "show" && args.size() >= 2) {
    gFaking = true;
    gFake.popup = args[1];
    gFake.frac = args.size() > 2 ? std::atof(args[2].c_str()) : 0.0;
    gFake.stalled = args.size() > 3 && args[3] == "1";
    gFake.text = args.size() > 4 ? args[4] : std::string();
  } else if (what == "off") {
    gFaking = false;
    gFake = Fake{};
  } else if (what == "trace") {
    gTrace = args.size() < 2 || args[1] != "off";
  } else {
    for (const auto& [name, n] : noted())
      std::printf("progsize rect frame %d %s noted %d %.0f %.0f %.0f %.0f\n", frame, name.c_str(), n.frame, n.pos.x, n.pos.y, n.size.x, n.size.y);
    std::fflush(stdout);
  }
}

}  // namespace gl::progui
