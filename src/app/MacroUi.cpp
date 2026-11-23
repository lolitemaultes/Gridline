// Gridline - K-245: macro knobs in the plugin windows (see MacroUi.h).
#include "MacroUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Macros.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::macroui {

using theme::pal;

namespace {

DevState gDev;

struct Learn {
  ObjectId owner = kNoId;
  int k = -1;
  uint64_t serial = 0;
};
Learn gLearn;

struct PanelUi {
  int selected = 0;
  bool picked = false;  // the person chose a macro (until then the panel shows the first one in use)
  int renaming = -1;
  char name[32] = {};
  char find[64] = {};
};
std::map<ObjectId, PanelUi>& panels() {
  static std::map<ObjectId, PanelUi> m;
  return m;
}
std::map<ObjectId, bool>& stripsOpen() {
  static std::map<ObjectId, bool> m;
  return m;
}

std::string label(const Song& s, ObjectId owner, int k) {
  const LayerMacro* m = macro::find(s, owner, k);
  if (m && !m->name.empty()) return m->name;
  return std::string(i18n::tr("Macro")) + " " + std::to_string(k + 1);
}

std::string ownerName(const Song& s, ObjectId owner) {
  if (const Channel* c = s.channel(owner)) return c->name;
  const int mi = s.mixerIndex(owner);
  return mi >= 0 ? mixerTrackName(s, static_cast<size_t>(mi)) : std::string();
}

// The parameter's name without its owner ("Plasma › Cutoff" for "Lead › Plasma › Cutoff").
std::string shortName(const Song& s, ParamId t) {
  std::string n = paramDisplayName(s, t);
  const std::string own = ownerName(s, t.owner);
  const std::string sep = " \xE2\x80\xBA ";
  if (!own.empty() && n.compare(0, own.size() + sep.size(), own + sep) == 0) n = n.substr(own.size() + sep.size());
  return n;
}

void linkTo(App& a, ParamId target, ObjectId owner, int k) {
  const Song& s = a.doc.song();
  if (macro::link(a.doc, target, owner, k)) a.notify(label(a.doc.song(), owner, k) + " \xE2\x86\x92 " + paramDisplayName(a.doc.song(), target));
  else a.notify(macro::wouldLoop(s, owner, k, target) ? i18n::tr("A macro cannot drive itself") : i18n::tr("This control cannot be linked to a macro"), true);
}

// The macro items of one owner in the link menu.
void ownerItems(App& a, ParamId pid, ObjectId owner, const char* heading) {
  const Song& s = a.doc.song();
  ObjectId curOwner = kNoId;
  int curK = -1;
  macro::linkedMacro(s, pid, &curOwner, &curK);
  ImGui::TextDisabled("%s", heading);
  for (int k = 0; k < kLayerMacros; ++k) {
    const size_t n = macro::links(s, owner, k).size();
    std::string item = label(s, owner, k);
    if (n > 0) item += "  (" + std::to_string(n) + ")";
    item += "##m" + std::to_string(owner) + "." + std::to_string(k);
    const bool on = curOwner == owner && curK == k;
    const bool ok = !macro::wouldLoop(s, owner, k, pid);
    if (ImGui::MenuItem(item.c_str(), nullptr, on, ok)) {
      if (on) macro::unlink(a.doc, pid);
      else linkTo(a, pid, owner, k);
    }
  }
}

bool learnStep(App& a) {
  if (gLearn.k < 0) return false;
  if (a.doc.touchSerial() == gLearn.serial) return false;
  const ParamId t = a.doc.lastTouchedParam();
  gLearn.serial = a.doc.touchSerial();
  if (t.owner == kNoId || t.owner == kSongId || (macro::isMacroParam(t) && t.owner == gLearn.owner)) return false;  // its own knobs: keep waiting
  linkTo(a, t, gLearn.owner, gLearn.k);
  gLearn = {};
  return true;
}

void caption(ImDrawList* dl, App& a, ImVec2 p, ImU32 col, const std::string& s) {
  dl->AddText(a.fontMono, theme::kFontLabel, p, col, s.c_str());
}

// The candidates of the Map list: the owner's own parameters (its instrument, note processors and
// strip for a channel; its effects and strip for a mixer track), without its macros.
std::vector<ParamId> candidates(const Song& s, ObjectId owner, const std::string& find) {
  std::vector<ParamId> out;
  std::string f = find;
  for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (ParamId p : listParams(s)) {
    if (p.owner != owner || macro::isMacroParam(p)) continue;
    if (!f.empty()) {
      std::string n = shortName(s, p);
      for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (n.find(f) == std::string::npos) continue;
    }
    out.push_back(p);
  }
  return out;
}

void mapPopup(App& a, ObjectId owner, int k, PanelUi& ui) {
  const Song& s = a.doc.song();
  if (ImGui::MenuItem(i18n::tr("Learn: move a control now"))) devLearn(a, owner, k);
  ui::Hint("The next control you move, here or in a plugin's own interface, is linked to this macro");
  ImGui::Separator();
  ImGui::SetNextItemWidth(240);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##find", i18n::tr("Find parameter"), ui.find, sizeof ui.find);
  const std::vector<ParamId> list = candidates(s, owner, ui.find);
  ImGui::BeginChild("##cands", ImVec2(320, std::min(320.0f, 22.0f * static_cast<float>(std::max<size_t>(1, list.size())) + 4)), ImGuiChildFlags_None);
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(list.size()));
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
      const ParamId t = list[static_cast<size_t>(r)];
      ObjectId o = kNoId;
      int mk = -1;
      const bool on = macro::linkedMacro(s, t, &o, &mk) && o == owner && mk == k;
      if (ImGui::Selectable((shortName(s, t) + "##c" + std::to_string(t.packed())).c_str(), on, ImGuiSelectableFlags_DontClosePopups)) {
        if (on) macro::unlink(a.doc, t);
        else linkTo(a, t, owner, k);
      }
    }
  if (list.empty()) ImGui::TextDisabled("%s", i18n::tr("No parameter matches."));
  ImGui::EndChild();
}

// The selected macro's targets: name, range (from, to), curve, remove.
void targetRows(App& a, ObjectId owner, int k, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const std::vector<const ModLink*> ls = macro::links(s, owner, k);
  const ImVec2 lp = ImGui::GetCursorScreenPos();
  caption(dl, a, lp, P.textFaint, label(s, owner, k) + "  \xE2\x86\x92");
  ImGui::Dummy(ImVec2(width, theme::kFontLabel + 4));
  if (ls.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
    ImGui::TextUnformatted(i18n::tr("No targets yet: right-click any control and pick Link to macro, or press Map"));
    ImGui::PopStyleColor();
    return;
  }
  // Copies: an edit below replaces the song's link list.
  std::vector<ModLink> rows;
  for (const ModLink* l : ls) rows.push_back(*l);
  const float h = ImGui::GetFrameHeight();
  const float sliderW = 104, curveW = 112, ctrlW = sliderW * 2 + curveW + h + theme::kS2 * 3;
  const float nameW = std::max(80.0f, width - ctrlW - theme::kS2);
  std::vector<ParamId> remove;
  for (const ModLink& ml : rows) {
    const ParamId t{ml.owner, ml.index};
    ImGui::PushID(static_cast<int>(t.packed() & 0x7FFFFFFF));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const std::string nm = t.owner == owner ? shortName(s, t) : paramDisplayName(s, t);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 4, q.y + std::round((h - 13) * 0.5f)), nameW - 8, P.text, nm.c_str());
    float mn = ml.min * 100, mx = ml.max * 100;
    const uint64_t key = t.packed() ^ 0x4D4143524F000000ull;
    ImGui::SetCursorScreenPos(ImVec2(q.x + nameW + theme::kS2, q.y));
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::DragFloat("##min", &mn, 0.25f, 0, 100, i18n::tr("from %.0f%%"), ImGuiSliderFlags_AlwaysClamp)) a.doc.editModLink(t, "change macro range", [&](ModLink& x) { x.min = mn / 100; }, key ^ 1);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ui::Hint("Where the control is with the macro at 0");
    ImGui::SameLine(0, theme::kS2);
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::DragFloat("##max", &mx, 0.25f, 0, 100, i18n::tr("to %.0f%%"), ImGuiSliderFlags_AlwaysClamp)) a.doc.editModLink(t, "change macro range", [&](ModLink& x) { x.max = mx / 100; }, key ^ 2);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ui::Hint("Where the control is with the macro at its top (lower than the start: the control moves the other way)");
    ImGui::SameLine(0, theme::kS2);
    ImGui::SetNextItemWidth(curveW);
    if (ImGui::BeginCombo("##curve", i18n::tr(modCurveName(ml.curve)))) {
      for (int c = 0; c < static_cast<int>(ModCurve::Count); ++c) {
        if (static_cast<ModCurve>(c) == ModCurve::Formula) continue;  // (the link settings menu edits formulas)
        if (ImGui::Selectable(i18n::tr(modCurveName(static_cast<ModCurve>(c))), c == static_cast<int>(ml.curve)))
          a.doc.editModLink(t, "change macro curve", [&](ModLink& x) { x.curve = static_cast<ModCurve>(c); });
      }
      ImGui::EndCombo();
    }
    ui::Hint("How the macro's travel maps onto the control's range");
    ImGui::SameLine(0, theme::kS2);
    if (ui::IconButton("##rm", ui::Icon::Close, false, h, "Unlink this control from the macro")) remove.push_back(t);
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + h + 4));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::PopID();
  }
  for (const ParamId& t : remove) macro::unlink(a.doc, t);
}

}  // namespace

const DevState& devState() { return gDev; }

void devLearn(App& a, ObjectId owner, int k) {
  if (k < 0 || !macro::hasMacros(a.doc.song(), owner)) {
    gLearn = {};
    return;
  }
  gLearn = {owner, k, a.doc.touchSerial()};
}

void frame(App& a) { learnStep(a); }

namespace {
// Layers whose child this channel is: their macros too (K-169).
std::vector<ObjectId> layersOf(const Song& s, ObjectId owner) {
  std::vector<ObjectId> layers;
  if (s.channel(owner))
    for (const auto& c : s.channels)
      if (c.id != owner && std::find(c.layer.begin(), c.layer.end(), owner) != c.layer.end()) layers.push_back(c.id);
  return layers;
}
}  // namespace

bool linkMenuAvailable(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  const ObjectId owner = macro::ownerFor(s, pid);
  if (owner == kNoId) return false;
  return !macro::isMacroParam(pid) || !layersOf(s, owner).empty();  // a macro knob is not linked to its own set
}

bool linkedToMacro(App& a, ParamId pid) { return macro::linkedMacro(a.doc.song(), pid, nullptr, nullptr); }

void linkMenu(App& a, ParamId pid) {
  if (!linkMenuAvailable(a, pid)) return;
  if (!ImGui::BeginMenu(i18n::tr("Link to macro"))) return;
  linkMenuItems(a, pid);
  ImGui::EndMenu();
}

void linkMenuItems(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  const ObjectId owner = macro::ownerFor(s, pid);
  if (owner == kNoId) return;
  const std::vector<ObjectId> layers = layersOf(s, owner);
  const bool ownMacro = macro::isMacroParam(pid);
  ObjectId curOwner = kNoId;
  int curK = -1;
  const bool linked = macro::linkedMacro(s, pid, &curOwner, &curK);
  if (!ownMacro) {
    const int free = macro::firstFree(s, owner);
    if (ImGui::MenuItem(i18n::tr("New macro"), nullptr, false, free >= 0)) linkTo(a, pid, owner, free);
    ui::Hint(free >= 0 ? "Links this control to the next macro that drives nothing yet" : "All eight macros are in use: pick one below");
    ImGui::Separator();
    ownerItems(a, pid, owner, (ownerName(s, owner) + " \xC2\xB7 " + i18n::tr("macros")).c_str());
  }
  for (ObjectId l : layers) {
    ImGui::Separator();
    ImGui::PushID(static_cast<int>(l));
    ownerItems(a, pid, l, (ownerName(s, l) + " \xC2\xB7 " + i18n::tr("layer macros")).c_str());
    ImGui::PopID();
  }
  if (linked) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Unlink from macro"))) macro::unlink(a.doc, pid);
    ui::Hint((std::string(i18n::tr("Driven by ")) + controllerOutputName(s, curOwner, kLayerMacroSlot, curK)).c_str());
  }
}

void drawPanel(App& a, ObjectId owner, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  if (!macro::hasMacros(s, owner)) return;
  learnStep(a);
  PanelUi& ui = panels()[owner];
  if (!ui.picked && macro::links(a.doc.song(), owner, ui.selected).empty())
    for (int k = 0; k < kLayerMacros; ++k)
      if (!macro::links(a.doc.song(), owner, k).empty()) {
        ui.selected = k;
        break;
      }
  ui.selected = std::clamp(ui.selected, 0, kLayerMacros - 1);
  width = std::floor(std::max(420.0f, std::min(width, ImGui::GetContentRegionAvail().x - 4)));  // inside the page, clear of its scroll bar
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID("macropanel");
  ImGui::PushID(static_cast<int>(owner));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  caption(dl, a, p0, P.textFaint, i18n::tr("MACROS"));
  {
    const char* tip = i18n::tr("Right-click any control of this plugin > Link to macro");
    const float tw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, tip).x;
    if (tw + 80 < width) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x + width - tw, p0.y - 1), P.textFaint, tip);
  }
  ImGui::Dummy(ImVec2(width, theme::kFontLabel + 6));
  const float cellW = std::floor(width / kLayerMacros);
  const ImVec2 row = ImGui::GetCursorScreenPos();
  const float r = 15;
  for (int k = 0; k < kLayerMacros; ++k) {
    ImGui::PushID(k);
    const float cx = std::round(row.x + k * cellW + cellW * 0.5f);
    const bool sel = ui.selected == k;
    if (sel) dl->AddRectFilled(ImVec2(row.x + k * cellW + 2, row.y), ImVec2(row.x + (k + 1) * cellW - 2, row.y + 92), P.surface800, theme::kRadiusLg);
    ImGui::SetCursorScreenPos(ImVec2(cx - r, row.y + 6));
    ui::ParamKnob("##macro", layerMacroParam(owner, k), r);
    if (ImGui::IsItemClicked()) ui.selected = k, ui.picked = true;
    gDev.knobX[k] = cx, gDev.knobY[k] = row.y + 6 + r;
    const int n = static_cast<int>(macro::links(s, owner, k).size());
    if (n > 0) {  // the count of targets
      const std::string cnt = std::to_string(n);
      dl->AddCircleFilled(ImVec2(cx + r + 5, row.y + 8), 7, P.accent);
      const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, cnt.c_str());
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(cx + r + 5 - ts.x * 0.5f), std::round(row.y + 8 - ts.y * 0.5f)), P.paper, cnt.c_str());
    }
    // Name (double-click renames).
    const float maxW = cellW - 10;
    const ImVec2 np(row.x + k * cellW + 5, row.y + 6 + 2 * r + 4);
    if (ui.renaming == k) {
      ImGui::SetCursorScreenPos(np);
      ImGui::SetNextItemWidth(maxW);
      if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##mname", ui.name, sizeof ui.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
          ImGui::IsItemDeactivated()) {
        macro::rename(a.doc, owner, k, ui.name);
        ui.renaming = -1;
      }
    } else {
      const std::string nm = label(s, owner, k);
      const float tw = std::min(maxW, a.fontRegular->CalcTextSizeA(theme::kFontCaption + 1, 1e9f, 0, nm.c_str()).x);
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(std::round(cx - tw * 0.5f), np.y + 2), maxW, sel ? P.text : P.textDim,
                          nm.c_str());
      ImGui::SetCursorScreenPos(np);
      ImGui::InvisibleButton("##mnamebtn", ImVec2(maxW, ImGui::GetFontSize() + 4));
      if (ImGui::IsItemClicked()) ui.selected = k, ui.picked = true;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ui.renaming = k;
        const LayerMacro* m = macro::find(s, owner, k);
        std::snprintf(ui.name, sizeof ui.name, "%s", m ? m->name.c_str() : "");
      }
      if (ImGui::BeginPopupContextItem("##namemenu")) {
        if (ImGui::MenuItem(i18n::tr("Rename..."))) {
          ui.renaming = k;
          const LayerMacro* m = macro::find(s, owner, k);
          std::snprintf(ui.name, sizeof ui.name, "%s", m ? m->name.c_str() : "");
        }
        if (ImGui::MenuItem(i18n::tr("Unlink every control"), nullptr, false, n > 0)) macro::clear(a.doc, owner, k);
        ImGui::EndPopup();
      }
      ui::Hint("Click: show the macro's targets below. Double-click: rename. Right-click: menu");
    }
    // Map: learn from the next moved control, or pick one from the list.
    const bool learning = gLearn.owner == owner && gLearn.k == k;
    const char* mapLabel = learning ? "Move a control##map" : "Map##map";
    const float bw = std::min(maxW, ui::TextButtonWidth(learning ? "Move a control" : "Map"));
    ImGui::SetCursorScreenPos(ImVec2(std::round(cx - bw * 0.5f), np.y + ImGui::GetFontSize() + 8));
    if (ui::TextButton(mapLabel, learning, bw,
                       learning ? "Move any control now, here or in a plugin's own interface; click again to stop"
                                : "Link a control: move it, or pick it from the list")) {
      ui.selected = k, ui.picked = true;
      if (learning) gLearn = {};
      else ImGui::OpenPopup("##mapmenu");
    }
    if (ImGui::BeginPopup("##mapmenu")) {
      mapPopup(a, owner, k, ui);
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 96));
  targetRows(a, owner, ui.selected, width);
  const ImVec2 p1 = ImGui::GetCursorScreenPos();
  gDev.owner = owner;
  gDev.frame = ImGui::GetFrameCount();
  gDev.selected = ui.selected;
  gDev.learning = gLearn.owner == owner ? gLearn.k : -1;
  gDev.x0 = p0.x, gDev.y0 = p0.y, gDev.x1 = p0.x + width, gDev.y1 = p1.y;
  ImGui::PopID();
  ImGui::PopID();
}

void drawEffectStrip(App& a, ObjectId mixerTrack, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  if (!s.mixerTrack(mixerTrack)) return;
  int used = 0;
  for (int k = 0; k < kLayerMacros; ++k) used += macro::assigned(s, mixerTrack, k) ? 1 : 0;
  auto [it, fresh] = stripsOpen().try_emplace(mixerTrack, used > 0);
  bool& open = it->second;
  (void)fresh;
  ImGui::PushID("macrostrip");
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 20;
  ImGui::InvisibleButton("##fold", ImVec2(width, h));
  const bool hov = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) open = !open;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + width, p.y), P.surface700, 1);
  ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(p.x + 8, p.y + h * 0.5f + 1), 11, hov ? P.text : P.textDim);
  std::string t = std::string(i18n::tr("TRACK MACROS")) + "  \xC2\xB7  " + ownerName(s, mixerTrack);
  if (used > 0) t += "  \xC2\xB7  " + std::to_string(used) + " " + i18n::tr("in use");
  caption(dl, a, ImVec2(p.x + 20, p.y + 5), hov ? P.text : P.textDim, t);
  ui::Hint("The eight macro knobs of this mixer track: each can drive controls of any effect on it");
  if (open) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    drawPanel(a, mixerTrack, width);
  }
  ImGui::PopID();
}

}  // namespace gl::macroui
