// Gridline — editor layout audit (Q-7). While a collection is open the
// editor building blocks (skin panels / wells / graph boxes, pui panels and
// sections, every bound control through pui::dragParamItem, skin labels and
// buttons) report their screen rectangles here; end() checks them with
// inset::check. Off (the normal case) every call is one branch.
//
// `gridline-app --layout-audit <dir>` draws every built-in editor at three
// widths (form scale 1, 1.5, 2), writes <dir>/layout_audit.tsv and a contact
// sheet <dir>/sheet.png with the problem rectangles outlined, and exits with
// the number of editors that have issues (0 = clean).
#pragma once
#include <map>
#include <string>
#include <vector>
#include "../core/Params.h"
#include "../../third_party/imgui/imgui.h"
#include "AlignCheck.h"
#include "Inset.h"

struct ImGuiWindow;

namespace gl::audit {

bool on();
void begin();
void frame(ImVec2 p0, ImVec2 p1, const char* what);
void control(ImVec2 p0, ImVec2 p1, const char* what);
// The last ImGui item as a control.
void lastItem(const char* what);
// J-128: a panel / section that groups controls for the alignment check only
// (rows, baselines and columns are compared inside the innermost group); not
// part of the Q-7 inset check.
void group(ImVec2 p0, ImVec2 p1, const char* what);
// J-128: every ImGui item drawn while the collection is open (item hook): the
// controls no helper registered (custom buttons, selectors) join the alignment
// check. Child windows are skipped.
void item(unsigned id, ImVec2 p0, ImVec2 p1);
// J-128: the kind (align::Kind) of the next control registered through
// lastItem / control (knob helpers mark their dial, sliders their track).
void nextKind(uint8_t kind);

struct Report {
  std::vector<inset::Rect> frames, controls;
  std::vector<std::string> frameWhat, controlWhat;
  std::vector<inset::Issue> issues;
  // J-128 alignment: controls with their kind and clip rectangle, text runs
  // read back from the draw lists (collectText), and the findings.
  std::vector<uint8_t> controlKind;
  std::vector<inset::Rect> controlClip;
  align::Scene scene;
  std::vector<align::Finding> findings;
};
// J-128: every text run drawn in `root` and its child windows this frame
// (glyph quads of the font atlas grouped into runs: layout box, baseline,
// size, clipped), appended to the open collection. Call before end().
void collectText(ImGuiWindow* root);
// Same for any window: the runs as a list (panel audit, --align-audit).
std::vector<align::Text> textRuns(ImGuiWindow* root);
// Scene of a collection (controls that are not text holders, frames, texts).
align::Scene sceneOf(const Report& r);
// Closes the collection and checks it (min inset in px).
Report end(float min = inset::kMinInset);

// J-92 parameter binding audit: while a binding collection is open, every
// parameter the editor reads (describeParam / getParam, through the core read
// hook) and every bound control (pui / skin drag controls, ui::ParamKnob /
// ParamFader) is counted per ParamId.
struct Bindings {
  std::map<uint64_t, int> reads;     // ParamId::packed() -> reads
  std::map<uint64_t, int> controls;  // ParamId::packed() -> bound drag controls
  std::map<int, int> stateReads;     // parameter index -> reads straight from the state (graphs, grids)
};
void bindBegin();
void bindControl(ParamId pid);
Bindings bindEnd();

}  // namespace gl::audit
