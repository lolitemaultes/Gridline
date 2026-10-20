// Gridline — J-54 dialog layout checks, run headless (Dear ImGui without a
// renderer): footers reserved under a scrolling child leave no needless
// scrollbar, auto-sizing dialog bodies keep the footer on short screens,
// dialogs are clamped to the screen, and form labels / fitted text never run
// into the next column (with the real Inter font at the body size).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include "Check.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app/DialogLayout.h"
#include "app/Theme.h"

using namespace gl;

namespace {

ImVec2 gDisplay(1440, 900);

// Runs `frames` frames of `body` at the current display size and returns the
// ImGui window `name` as left by the last frame.
ImGuiWindow* run(const char* name, const std::function<void()>& body, int frames = 4) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = gDisplay;
  for (int f = 0; f < frames; ++f) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    body();
    ImGui::Render();
  }
  return ImGui::FindWindowByName(name);
}

bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

// The pattern every list dialog uses: a child that takes all but the footer,
// then DialogFooter and one row of buttons (file dialog: field row + buttons).
void footerDialog(const char* name, ImVec2 size, ImVec2 pad, int rows) {
  ImGui::SetNextWindowPos(ImVec2(20, 60));
  ImGui::SetNextWindowSize(size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);  // pushed only around Begin, like the app's dialogs
  ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  ImGui::BeginChild("##list", ImVec2(0, -ui::DialogFooterHeight(rows)), ImGuiChildFlags_Borders);
  for (int i = 0; i < 80; ++i) ImGui::Text("row %d", i);  // the list itself scrolls
  ImGui::EndChild();
  if (rows == 1) {
    ui::DialogFooter(ui::ButtonRowWidth(2, 96));
  } else {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::SetNextItemWidth(-1);
    static char buf[32] = "name";
    ImGui::InputText("##name", buf, sizeof buf);
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  ImGui::Button("Cancel", ImVec2(96, 0));
  ImGui::SameLine(0, theme::kS2);
  ImGui::Button("OK", ImVec2(96, 0));
  ImGui::End();
}

void testFooters() {
  const ImVec2 pads[] = {ImVec2(theme::kS2, theme::kS2), ImVec2(theme::kS4, theme::kS3), ImVec2(theme::kS4, theme::kS4)};
  const float heights[] = {300, 420, 480, 611};
  for (int rows = 1; rows <= 2; ++rows)
    for (const ImVec2& pad : pads)
      for (float h : heights) {
        char name[64];
        std::snprintf(name, sizeof name, "footer r%d p%.0f h%.0f", rows, pad.y, h);
        ImGuiWindow* w = run(name, [&]() { footerDialog(name, ImVec2(560, h), pad, rows); });
        char label[160];
        std::snprintf(label, sizeof label, "%s: no window scrollbar (scroll max %.1f)", name, w ? w->ScrollMax.y : -1.0f);
        expect(w && !w->ScrollbarY && w->ScrollMax.y <= 0.0f, label);
        // The content ends exactly at the bottom padding (no gap, no overflow).
        const float contentBottom = w->ContentSize.y + w->WindowPadding.y + w->TitleBarHeight + w->MenuBarHeight;
        std::snprintf(label, sizeof label, "%s: footer flush with the padding (%.1f vs %.1f)", name, contentBottom + w->WindowPadding.y, w->Size.y);
        expect(near(contentBottom + w->WindowPadding.y, w->Size.y, 1.0f), label);
      }
}

// An auto-sizing dialog whose body can be much taller than the screen.
void bodyDialog(const char* name, float bodyH, ImRect* footerButton) {
  ui::PlaceNextDialog(ImVec2(780, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleVar();
  ui::BeginDialogBody("##body");
  ImGui::Dummy(ImVec2(700, bodyH));
  ui::EndDialogBody();
  ui::DialogFooter(ui::ButtonRowWidth(2, 96));
  ImGui::Button("Cancel", ImVec2(96, 0));
  ImGui::SameLine(0, theme::kS2);
  ImGui::Button("Start", ImVec2(96, 0));
  *footerButton = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::End();
}

void testDialogBody() {
  const ImVec2 displays[] = {ImVec2(1440, 900), ImVec2(960, 600), ImVec2(1280, 720)};
  for (const ImVec2& d : displays) {
    gDisplay = d;
    for (float bodyH : {200.0f, 900.0f, 2400.0f}) {
      char name[64];
      std::snprintf(name, sizeof name, "body %.0fx%.0f %.0f", d.x, d.y, bodyH);
      ImRect button;
      ImGuiWindow* w = run(name, [&]() { bodyDialog(name, bodyH, &button); }, 6);
      const ImVec2 area = ui::DialogArea();
      char label[160];
      std::snprintf(label, sizeof label, "%s: dialog within the screen area (%.0f <= %.0f)", name, w ? w->Size.y : -1.0f, area.y);
      expect(w && w->Size.y <= area.y + 0.5f && w->Size.x <= area.x + 0.5f, label);
      ImGuiWindow* body = nullptr;
      for (ImGuiWindow* c : GImGui->Windows)
        if (w && c->ParentWindow == w && c->ChildId == w->GetID("##body")) body = c;
      std::snprintf(label, sizeof label, "%s: body child found", name);
      expect(body != nullptr, label);
      if (body && body->ScrollbarY) {  // the body's scrollbar sits beside the content, not over it
        std::snprintf(label, sizeof label, "%s: body scrollbar clear of the content", name);
        expect(body->Size.x >= 700 + ImGui::GetStyle().ScrollbarSize - 0.5f, label);
      }
      std::snprintf(label, sizeof label, "%s: dialog itself never scrolls", name);
      expect(w && !w->ScrollbarY, label);
      std::snprintf(label, sizeof label, "%s: footer button on screen and inside the dialog (button %.0f-%.0f, window %.0f+%.0f)", name, button.Min.y, button.Max.y, w ? w->Pos.y : 0.f, w ? w->Size.y : 0.f);
      expect(w && button.Max.y <= w->Pos.y + w->Size.y + 0.5f && button.Max.y <= d.y - theme::kStatusBarH + 0.5f &&
                 button.Min.y >= theme::kTopBarH - 0.5f,
             label);
      if (bodyH <= 200) {
        std::snprintf(label, sizeof label, "%s: a short body is not stretched (window %.0f)", name, w ? w->Size.y : -1.0f);
        expect(w && w->Size.y < 200 + 160, label);
      }
      // The dialog hugs its body: 700 px of content + padding (+ a scrollbar when it scrolls).
      const float extra = bodyH > 400 && d.y < 1000 ? ImGui::GetStyle().ScrollbarSize : 0.0f;
      std::snprintf(label, sizeof label, "%s: dialog width hugs the body (%.0f vs %.0f)", name, w ? w->Size.x : -1.0f, 700 + 2 * theme::kS4 + extra);
      expect(w && w->Size.x <= 700 + 2 * theme::kS4 + extra + 0.5f, label);
    }
  }
  gDisplay = ImVec2(1440, 900);
}

void testPlacement() {
  gDisplay = ImVec2(960, 600);
  ImGuiWindow* w = run("place big", []() {
    ui::PlaceNextDialog(ImVec2(1060, 640), ImGuiCond_Always, ImVec2(720, 400));
    ImGui::Begin("place big", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
  });
  const ImVec2 area = ui::DialogArea();
  expect(w && w->Size.x <= area.x + 0.5f && w->Size.y <= area.y + 0.5f, "a 1060x640 dialog is clamped to a 960x600 screen");
  expect(w && w->Pos.x >= 0 && w->Pos.x + w->Size.x <= 960 + 0.5f, "clamped dialog stays inside the screen horizontally");
  expect(w && w->Pos.y >= theme::kTopBarH - 0.5f && w->Pos.y + w->Size.y <= 600 - theme::kStatusBarH + 0.5f,
         "clamped dialog sits between the top bar and the status bar");
  const ImVec2 fit = ui::FitDialogSize(ImVec2(400, 0));
  expect(fit.x == 400 && fit.y == 0, "a size that fits is kept (0 = auto stays 0)");
  gDisplay = ImVec2(1440, 900);
  w = run("place small", []() {
    ui::PlaceNextDialog(ImVec2(560, 520), ImGuiCond_Always);
    ImGui::Begin("place small", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
  });
  expect(w && near(w->Size.x, 560) && near(w->Size.y, 520), "a dialog that fits keeps its size");
  expect(w && near(w->Pos.x + w->Size.x * 0.5f, 720, 1.0f), "and is centred");
}

void testFormLabels() {
  const char* labels[] = {"Channels", "Patterns", "Playlist clips", "Third-party plugin (no installed match)"};
  float col = 0;
  run("form col", [&]() {
    ImGui::Begin("form col", nullptr, ImGuiWindowFlags_NoSavedSettings);
    col = ui::FormColumn(labels, 4, 168, 600, theme::kS6);
    ImGui::End();
  });
  const float widest = ImGui::CalcTextSize(labels[3]).x;
  expect(col >= widest + theme::kS6 - 0.5f, "form column clears the widest label plus the gap");
  run("form col2", [&]() {
    ImGui::Begin("form col2", nullptr, ImGuiWindowFlags_NoSavedSettings);
    col = ui::FormColumn(labels, 3, 168, 600);
    ImGui::End();
  });
  expect(near(col, 168), "short labels keep the minimum column");
  run("form col3", [&]() {
    ImGui::Begin("form col3", nullptr, ImGuiWindowFlags_NoSavedSettings);
    col = ui::FormColumn(labels, 4, 168, 200);
    ImGui::End();
  });
  expect(near(col, 200), "the column never passes its maximum");

  // A label longer than its column is fitted: it ends before the gap and the
  // control starts exactly on the column, also inside indented groups.
  const char* longLabel = "Wrapped third-party instrument whose plugin file is not installed on this computer";
  for (float indent : {0.0f, theme::kS2}) {
    float textRight = 0, controlX = 0, colScreen = 0;
    bool cut = false;
    run("fit label", [&]() {
      ImGui::SetNextWindowPos(ImVec2(10, 60));
      ImGui::SetNextWindowSize(ImVec2(520, 300));
      ImGui::Begin("fit label", nullptr, ImGuiWindowFlags_NoSavedSettings);
      ImGui::BeginGroup();
      if (indent > 0) ImGui::Indent(indent);
      const float groupX = ImGui::GetCursorScreenPos().x - indent;
      ui::FormLabelFit(longLabel, 260, theme::pal().textDim);
      textRight = 0;
      controlX = ImGui::GetCursorScreenPos().x;
      ImGui::Text("14");
      if (indent > 0) ImGui::Unindent(indent);
      ImGui::EndGroup();
      colScreen = groupX + 260;
      // The fitted label is the item before the number: measure it again.
      const char* shown = theme::fitText(longLabel, 260 - indent - 12.0f);
      cut = std::string(shown) != longLabel;
      textRight = groupX + indent + ImGui::CalcTextSize(shown).x;
      ImGui::End();
    });
    char label[128];
    std::snprintf(label, sizeof label, "indent %.0f: long label is cut with an ellipsis", indent);
    expect(cut, label);
    std::snprintf(label, sizeof label, "indent %.0f: fitted label ends before the number column (%.1f < %.1f)", indent, textRight, colScreen);
    expect(textRight <= colScreen - 12.0f + 0.5f, label);
    std::snprintf(label, sizeof label, "indent %.0f: the number starts on the column", indent);
    expect(near(controlX, colScreen), label);
  }
}

void testTextFit() {
  bool cut = true, cut2 = false;
  float w1 = 0;
  run("textfit", [&]() {
    ImGui::Begin("textfit", nullptr, ImGuiWindowFlags_NoSavedSettings);
    cut = ui::TextFit("Short", 200, theme::pal().text);
    cut2 = ui::TextFit("/Users/somebody/.config/gridline/scripts/controllers/a/very/long/path", 160, theme::pal().text);
    w1 = ImGui::GetItemRectSize().x;
    ImGui::End();
  });
  expect(!cut, "text that fits is shown whole");
  expect(cut2, "a long path is cut");
  expect(w1 <= 160 + 0.5f, "the cut path fits its width");
}

}  // namespace

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // headless: textures are never uploaded
  ImFontConfig cfg;
  ImFont* inter = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf", theme::kFontBody, &cfg);
  expect(inter != nullptr, "Inter loads from assets/fonts");
  theme::apply(true);
  if (inter) {
    ImGui::GetStyle().FontSizeBase = theme::kFontBody;
    io.FontDefault = inter;
  }
  testFooters();
  testDialogBody();
  testPlacement();
  testFormLabels();
  testTextFit();
  ImGui::DestroyContext();
  return finish();
}
