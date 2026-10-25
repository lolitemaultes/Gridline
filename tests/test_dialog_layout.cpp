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

// J-91 (GitHub issue #1): a floating window dragged against the top bar.
// The drag is real mouse input on the title bar; the window must stop at the
// top bar with its title bar, background and content together (0.6.0 moved
// the window after Begin(): the title bar stayed where the mouse put it and
// only the content was pushed down, an empty band in between).
struct DragCase {
  float bandTop = 0;     // inner area top - (window top + title bar): 0 = no band
  float contentGap = 0;  // content start - inner area top - padding: 0 = attached
  float top = 0;         // window top while dragged
  float released = 0;    // window top a second after it was let go
};
DragCase dragAgainstTop(bool oldWay) {
  ImGuiIO& io = ImGui::GetIO();
  const char* name = oldWay ? "drag old" : "drag new";
  auto body = [&]() {
    if (!oldWay) ui::ClampTrackedWindows();
    ImGui::SetNextWindowPos(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (oldWay) {  // 0.6.0 KeepWindowInViewport: clamped after Begin()
      ImVec2 a0, a1;
      ui::WorkArea(a0, a1);
      const ImVec2 p = ui::ClampWindowPos(ImGui::GetWindowPos(), ImGui::GetWindowSize(), a0, a1, true);
      if (p.x != ImGui::GetWindowPos().x || p.y != ImGui::GetWindowPos().y) ImGui::SetWindowPos(p);
    } else {
      ui::TrackWindowInViewport();
    }
    ImGui::Button("content", ImVec2(300, 200));
    ImGui::End();
  };
  run(name, body, 30);  // half a second: the press below is not a double click (which would fold the window)
  ImGuiWindow* w = ImGui::FindWindowByName(name);
  // Press on the title bar and drag far above the top bar.
  const ImVec2 grab(w->Pos.x + 60, w->Pos.y + w->TitleBarHeight * 0.5f);
  io.AddMousePosEvent(grab.x, grab.y);
  run(name, body, 2);
  io.AddMouseButtonEvent(0, true);
  run(name, body, 2);
  for (int i = 1; i <= 6; ++i) {
    io.AddMousePosEvent(grab.x, grab.y - 60.0f * static_cast<float>(i));
    run(name, body, 1);
  }
  DragCase d;
  d.top = w->Pos.y;
  d.bandTop = w->InnerRect.Min.y - (w->Pos.y + w->TitleBarHeight + w->MenuBarHeight);
  d.contentGap = w->DC.CursorStartPos.y - w->InnerRect.Min.y - w->WindowPadding.y;
  io.AddMouseButtonEvent(0, false);
  run(name, body, 60);
  d.released = w->Pos.y;
  return d;
}

void testDragAgainstTop() {
  gDisplay = ImVec2(1440, 900);
  const DragCase old = dragAgainstTop(true);
  const DragCase now = dragAgainstTop(false);
  char label[160];
  std::snprintf(label, sizeof label, "0.6.0 clamp after Begin: the content is pushed %.1f px away from the title bar (the band)",
                static_cast<double>(old.contentGap));
  expect(old.contentGap > 20.0f, label);
  // J-117: dragged 360 px past the top bar it follows only a little way (elastic, damped) ...
  std::snprintf(label, sizeof label, "dragged against the top bar: the window stretches only a little past it (top %.1f)", static_cast<double>(now.top));
  expect(now.top < theme::kTopBarH && now.top > theme::kTopBarH - 48.0f, label);
  // ... and springs back onto it once let go.
  std::snprintf(label, sizeof label, "let go: it springs back to the top bar (top %.1f)", static_cast<double>(now.released));
  expect(near(now.released, theme::kTopBarH), label);
  std::snprintf(label, sizeof label, "no band under the title bar (%.1f px)", static_cast<double>(now.bandTop));
  expect(near(now.bandTop, 0.0f), label);
  std::snprintf(label, sizeof label, "content attached to the title bar (%.1f px)", static_cast<double>(now.contentGap));
  expect(near(now.contentGap, 0.0f), label);
  // At rest: a window left below the screen (the main window shrank) comes back inside, moved
  // before it is drawn (J-117: springing back over a few frames).
  ImGuiWindow* w = ImGui::FindWindowByName("drag new");
  auto rest = [&]() {
    ui::ClampTrackedWindows();
    ImGui::Begin("drag new", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ui::TrackWindowInViewport();
    ImGui::Button("content", ImVec2(300, 200));
    ImGui::End();
  };
  ImGui::SetWindowPos(w, ImVec2(1300, 880), ImGuiCond_Always);  // put outside by code: back inside at once
  run("drag new", rest, 1);
  expect(w->Pos.x + w->Size.x <= 1440.5f && w->Pos.y + w->Size.y <= 900 - theme::kStatusBarH + 0.5f, "a window put outside by code is placed inside before it is drawn");
  ImGui::SetWindowPos(w, ImVec2(1100, 600), ImGuiCond_Always);  // resting near the bottom-right corner ...
  run("drag new", rest, 3);
  gDisplay = ImVec2(1200, 800);  // ... when the screen shrinks
  run("drag new", rest, 2);
  const float midway = w->Pos.x;
  expect(midway < 1100 && midway > 1200 - w->Size.x, "a window off the shrunk screen springs back (on its way after two frames)");
  expect(near(w->InnerRect.Min.y, w->Pos.y + w->TitleBarHeight), "and is drawn in one piece meanwhile");
  run("drag new", rest, 30);
  expect(w->Pos.x + w->Size.x <= 1200.5f && w->Pos.y + w->Size.y <= 800 - theme::kStatusBarH + 0.5f, "half a second later it is fully inside");
  expect(near(w->InnerRect.Min.y, w->Pos.y + w->TitleBarHeight), "and drawn in one piece");
  ui::SetWindowRubber(false);  // elastic edges off: straight back, before it is drawn
  ImGui::SetWindowPos(w, ImVec2(1300, 880), ImGuiCond_Always);
  run("drag new", rest, 1);
  expect(w->Pos.x + w->Size.x <= 1200.5f && w->Pos.y + w->Size.y <= 800 - theme::kStatusBarH + 0.5f, "elastic edges off: back inside at once");
  ui::SetWindowRubber(true);
  // The pure clamp: at rest inside; while dragged only the title bar is kept reachable.
  const ImVec2 a0(0, 48), a1(1000, 700);
  const ImVec2 r = ui::ClampWindowPos(ImVec2(-50, 10), ImVec2(300, 200), a0, a1, false);
  expect(near(r.x, 0) && near(r.y, 48), "clamp at rest: inside the area");
  const ImVec2 d = ui::ClampWindowPos(ImVec2(-250, 680), ImVec2(300, 200), a0, a1, true);
  expect(near(d.x, -220) && near(d.y, 668), "clamp while dragging: 80 px of width and the 32 px title bar stay reachable");
  gDisplay = ImVec2(1440, 900);
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

// Paths are cut in the middle: the start and the last folder / file name stay.
void testPathFit() {
  const char* path = "/Users/somebody/Music/Gridline/Collaborative projects/A very long project name/Song";
  bool cut = false, whole = true;
  float w = 0;
  std::string shown;
  run("pathfit", [&]() {
    ImGui::Begin("pathfit", nullptr, ImGuiWindowFlags_NoSavedSettings);
    whole = !ui::PathFit("/tmp/short", 300, theme::pal().text);
    cut = ui::PathFit(path, 220, theme::pal().text);
    w = ImGui::GetItemRectSize().x;
    shown = theme::fitTextMiddle(path, 220);
    ImGui::End();
  });
  expect(whole, "a short path is shown whole");
  expect(cut, "a long path is cut");
  expect(w <= 220 + 0.5f, "the cut path fits its width");
  expect(shown.rfind("/Users", 0) == 0, "the cut path keeps its start");
  expect(shown.size() >= 5 && shown.compare(shown.size() - 5, 5, "/Song") == 0, "the cut path keeps its end");
  expect(shown.find("\xE2\x80\xA6") != std::string::npos, "the cut path has an ellipsis in the middle");
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
  testDragAgainstTop();
  testFormLabels();
  testTextFit();
  testPathFit();
  ImGui::DestroyContext();
  return finish();
}
