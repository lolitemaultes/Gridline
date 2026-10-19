// Gridline — plugin editor dispatch. The built-in editors live in
// PluginEditors{Instr*,Fl,FlSynth*,K3*}.cpp, with the remaining pre-J-19 ones
// reached via PluginEditorsEq.cpp; pack editors come from the pack registry;
// anything else gets a generic grouped knob grid.
#include "../packs/PacksUi.h"
#include <cctype>
#include "PluginEditorsCommon.h"
#include "PluginEditorsK3.h"

namespace gl {

namespace {

void editorKnobRow(App& a, const PluginTarget& t, float width, const char* title) {
  const PluginState st = *t.state(a.doc.song());
  const PluginInfo* info = findPlugin(st.type);
  pui::beginPanel(title, width);
  int perRow = std::max(1, static_cast<int>((width - 24) / 76));
  int col = 0;
  for (size_t k = 0; k < info->params.size(); ++k) {
    const ParamDesc& d = info->params[k];
    if (col) ImGui::SameLine(0, 0);
    if (d.kind == ParamKind::Toggle) {
      ImGui::BeginGroup();
      ImGui::Dummy(ImVec2(0, 10));
      pui::led(a, t, static_cast<int>(k));
      ImGui::EndGroup();
    } else if (d.kind == ParamKind::Choice) {
      ImGui::BeginGroup();
      ImGui::TextDisabled("%s", d.name.c_str());
      pui::choiceButtons(a, t, static_cast<int>(k));
      ImGui::EndGroup();
      col = perRow - 1;
    } else {
      pui::knob(a, t, static_cast<int>(k), nullptr, pui::KnobMedium, 76);
    }
    col = (col + 1) % perRow;
  }
  pui::endPanel();
}

}  // namespace

bool drawCustomEditor3(App& a, const PluginTarget& t, float width);  // PluginEditorsEq.cpp (0.5 editors)
bool drawInstrumentEditorJ2(App& a, const PluginTarget& t, float width);  // PluginEditorsInstr.cpp (J-2 instruments)
bool drawFlEditor(App& a, const PluginTarget& t, float width);       // PluginEditorsFl.cpp (J-19 effects)
bool drawFlSynthEditor(App& a, const PluginTarget& t, float width);  // PluginEditorsFlSynth.cpp (J-19 instruments)

bool drawCustomEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (drawInstrumentEditorJ2(a, t, width)) return true;
  if (drawFlEditor(a, t, width)) return true;
  if (drawFlSynthEditor(a, t, width)) return true;
  if (drawK3Editor(a, t, width)) return true;
  if (drawCustomEditor3(a, t, width)) return true;
  if (packs::drawEditor(a, t, width)) return true;
  if (const PluginInfo* info = findPlugin(st->type)) {
    std::string title = info->name;
    for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    editorKnobRow(a, t, width, title.c_str());
    return true;
  }
  return false;
}

}  // namespace gl
