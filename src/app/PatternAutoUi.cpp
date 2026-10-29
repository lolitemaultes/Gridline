// Gridline — J-173: pattern automation clips in the channel rack and menus (see PatternAutoUi.h).
#include "PatternAutoUi.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../core/Automation.h"
#include "../core/I18n.h"
#include "../core/MenuParity.h"
#include "../core/PatternAutoOps.h"
#include "../core/PatternAutomation.h"
#include "App.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::patautoui {

using theme::pal;

uint32_t curveRgb(const AutomationClip& c, size_t k) {
  static const Rgb kAuto[] = {0xF2C14E, 0x6FD3E8, 0xF07FC4, 0xA5E86A, 0xC8A2F5, 0xF29B5E};  // unset colours: told apart by order
  return c.color == AutomationClip{}.color ? kAuto[k % (sizeof kAuto / sizeof kAuto[0])] : c.color;
}

void clipMenu(App& a, ObjectId pattern, ParamId target) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  const AutomationClip* c = p ? findPatternLane(*p, target.owner, target.index) : nullptr;
  if (!c) return;
  const std::string name = patternAutoName(s, *c);
  const std::string current = c->name;
  ImGui::TextDisabled("%s", name.c_str());
  if (ImGui::MenuItem(i18n::tr("Edit in the automation editor"))) openPatternAutomation(a, pattern, target);
  if (ImGui::MenuItem(i18n::tr("Show in the piano roll"))) {
    pianoAutomationLane() = target;
    a.selectPattern(pattern);
    a.openPianoRoll(s.channel(target.owner) ? target.owner : a.selChannel);
    a.showPiano = true;
    a.focusWindow = "Piano roll";
  }
  ui::Hint("The clip's points in the piano roll's lower lane (drawn as steps there)");
  if (ImGui::BeginMenu(i18n::tr("Rename..."))) {
    menus::renameField(current.empty() ? name : current, [&a, pattern, target](const std::string& n) {
      a.doc.editPattern(pattern, "rename automation clip", [&](Pattern& x) {
        if (AutomationClip* q = findPatternLane(x, target.owner, target.index)) q->name = n;
      });
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col = 0;
    if (menus::colorGrid(&col))
      a.doc.editPattern(pattern, "automation clip colour", [&](Pattern& x) {
        if (AutomationClip* q = findPatternLane(x, target.owner, target.index)) q->color = col;
      });
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Move to the playlist"))) {
    if (ops::laneToAutomationClip(a.doc, pattern, target.owner, target.index) != kNoId)
      a.notify("The automation is a playlist clip now (one under each clip of the pattern)");
    return;
  }
  ui::Hint("Turns it into a playlist automation clip placed under every clip of this pattern (the song sounds the same)");
  if (ImGui::MenuItem(i18n::tr("Delete"))) {
    a.doc.editPattern(pattern, "delete automation clip", [&](Pattern& x) { removePatternClip(x, target.owner, target.index); });
    if (pianoAutomationLane() == target) pianoAutomationLane() = ParamId{};
  }
}

void addMenu(App& a, ObjectId pattern) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  ParamId pick{kNoId, 0};
  auto add = [&](ParamId pid) {
    const bool has = findPatternLane(*p, pid.owner, pid.index) != nullptr;
    if (ImGui::MenuItem(paramDisplayName(s, pid).c_str(), nullptr, has)) pick = pid;
  };
  const std::vector<ParamId> params = listParams(s);
  const Channel* ch = s.channel(a.selChannel);
  if (ch) {
    ImGui::TextDisabled("%s", ch->name.c_str());
    int shown = 0;
    for (ParamId pid : params)
      if (pid.owner == ch->id && ++shown < 120) add(pid);
    ImGui::Separator();
  }
  ImGui::TextDisabled("%s", i18n::tr("Song"));
  for (ParamId pid : params)
    if (pid.owner == kSongId) add(pid);
  if (ch && ch->mixerTrack != kNoId) {
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("Mixer track"));
    int n = 0;
    for (ParamId pid : params)
      if (pid.owner == ch->mixerTrack && ++n < 60) add(pid);
  }
  if (pick.owner != kNoId) createPatternAutomationClip(a, pick, pattern);
}

void listMenu(App& a, ObjectId pattern) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  std::vector<std::pair<ParamId, std::string>> items;
  for (const auto& c : p->automation) items.push_back({ParamId{c.owner, c.index}, patternAutoName(s, c)});
  // (items are copies: an edit below replaces the pattern)
  for (const auto& [pid, name] : items) {
    ImGui::PushID(static_cast<int>(pid.packed() & 0x7FFFFFFF));
    if (ImGui::BeginMenu(name.c_str())) {
      clipMenu(a, pattern, pid);
      ImGui::EndMenu();
    }
    ImGui::PopID();
  }
  if (!items.empty()) ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Add automation clip for"))) {
    addMenu(a, pattern);
    ImGui::EndMenu();
  }
}

void drawRackRows(App& a, ObjectId pattern, const RackRows& g) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(pattern);
  if (!pat || pat->automation.empty()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Tick len = std::max<Tick>(1, s.patternContentLength(*pat));
  // Copies: a menu item may replace the pattern while the rows draw.
  const std::vector<AutomationClip> clips = pat->automation;
  ParamId open{kNoId, 0};
  for (size_t k = 0; k < clips.size(); ++k) {
    const AutomationClip& ac = clips[k];
    const ParamId tgt{ac.owner, ac.index};
    ImGui::PushID(static_cast<int>(tgt.packed() & 0x7FFFFFFF) ^ 0x2A000000);
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float cy = rp.y + g.rowH * 0.5f;
    const ImU32 col = theme::rgb(curveRgb(ac, k));
    const std::string name = patternAutoName(s, ac);
    // Automation glyph where a channel's lamp is.
    ui::drawIcon(dl, ui::Icon::ClipAutomation, ImVec2(rp.x + g.xLed + 8, cy), 13, theme::mix(col, P.paper, 0.2f));
    // Name button over the knob / mixer / name cells (a clip has none of those).
    const float nx = rp.x + g.xLed + 20, nw = g.nameX + g.nameW - (g.xLed + 20);
    const ImVec2 np(nx, cy - g.cellH * 0.5f);
    ImGui::SetCursorScreenPos(np);
    ImGui::InvisibleButton("aname", ImVec2(nw, g.cellH));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) open = tgt;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pamenu");
    const ImU32 bg = hov ? P.surface700 : P.surface800;
    dl->AddRectFilled(np, ImVec2(np.x + nw, np.y + g.cellH), theme::mix(bg, P.ink, 0.3f), theme::kRadiusSm);
    dl->AddRectFilled(np, ImVec2(np.x + nw, np.y + g.cellH - 1), bg, theme::kRadiusSm);
    dl->AddRectFilled(np, ImVec2(np.x + 4, np.y + g.cellH), col, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(np.x + 10, std::round(np.y + (g.cellH - 13) * 0.5f) - 1), nw - 14,
                        P.text, name.c_str());
    if (ui::HintHovered()) ui::Hint(name + "   " + i18n::tr("(automation in this pattern \xC2\xB7 click: edit \xC2\xB7 right-click: menu)"));
    if (ImGui::BeginPopup("##pamenu")) {
      clipMenu(a, pattern, tgt);
      ImGui::EndPopup();
    }
    // The curve across the step area (the pattern's length fills the steps, like a note preview).
    const ImVec2 sp(rp.x + g.stepsX - g.scrollX, cy - g.cellH * 0.5f);
    ImGui::PushClipRect(ImVec2(g.viewX0, rp.y), ImVec2(g.viewX1, rp.y + g.rowH), true);
    ImGui::SetCursorScreenPos(ImVec2(g.viewX0, sp.y));
    ImGui::InvisibleButton("acurve", ImVec2(std::max(1.0f, g.viewX1 - g.viewX0), g.cellH));
    const bool chov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) open = tgt;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pamenu2");
    if (chov) ui::Hint(name + "   " + i18n::tr("(click: edit in the automation editor)"));
    if (ImGui::BeginPopup("##pamenu2")) {
      clipMenu(a, pattern, tgt);
      ImGui::EndPopup();
    }
    dl->AddRectFilled(sp, ImVec2(sp.x + g.stepsW, sp.y + g.cellH), chov ? theme::mix(P.field, P.surface700, 0.4f) : P.field, theme::kRadiusSm);
    for (Tick b = s.barTicks(); b < len; b += std::max<Tick>(1, s.barTicks())) {
      const float x = std::round(sp.x + g.stepsW * static_cast<float>(b) / static_cast<float>(len));
      dl->AddLine(ImVec2(x, sp.y + 2), ImVec2(x, sp.y + g.cellH - 2), theme::withAlpha(P.lineStrong, 0.6f));
    }
    {
      const float y0 = sp.y + 2, y1 = sp.y + g.cellH - 2;
      std::vector<ImVec2> pts;
      const float xa = std::max(g.viewX0, sp.x), xb = std::min(g.viewX1, sp.x + g.stepsW);
      for (float x = std::floor(xa); x <= xb; x += 1.0f) {
        const double t = static_cast<double>(x - sp.x) / static_cast<double>(g.stepsW) * static_cast<double>(len);
        const double v = std::clamp(automationValue(ac, t), 0.0, 1.0);
        pts.emplace_back(x + 0.5f, y1 - static_cast<float>(v) * (y1 - y0));
      }
      if (pts.size() >= 2) {
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, 1.5f);
      }
    }
    if (g.playTick >= 0) {
      const float x = sp.x + g.stepsW * static_cast<float>(g.playTick) / static_cast<float>(len);
      dl->AddLine(ImVec2(x, sp.y), ImVec2(x, sp.y + g.cellH), P.playhead);
    }
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + g.rowH + g.rowGap));
    ImGui::Dummy(ImVec2(g.rowW, 0));
    ImGui::PopID();
  }
  if (open.owner != kNoId) openPatternAutomation(a, pattern, open);
}

void rackDropRow(App& a, ObjectId pattern, float rowW, float rowH) {
  const ImGuiPayload* drag = ImGui::GetDragDropPayload();
  if (!drag || !drag->IsDataType("GL_CLIPSRC")) return;
  const auto* src = static_cast<const browserui::ClipSrcPayload*>(drag->Data);
  if (!src || static_cast<ClipKind>(src->kind) != ClipKind::Automation || !a.doc.song().pattern(pattern)) return;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 rp = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##padrop", ImVec2(std::max(40.0f, rowW), rowH));
  const bool over = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), theme::withAlpha(P.hot, over ? 0.22f : 0.08f), theme::kRadius);
  dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + rowH), theme::withAlpha(P.hot, 0.7f), theme::kRadius, 0, 1.5f);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(rp.x + 10, std::round(rp.y + (rowH - 13) * 0.5f)), rowW - 20, P.text,
                      i18n::tr("Drop here: the automation clip goes into this pattern"));
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CLIPSRC")) {
      const browserui::ClipSrcPayload ps = *static_cast<const browserui::ClipSrcPayload*>(pl->Data);
      const patauto::MoveResult r = patauto::automationIntoPattern(a.doc, ps.id, pattern, 0);
      if (r != patauto::MoveResult::Done) a.notify(patauto::moveResultText(r), true);
      else a.notify("Automation copied into the pattern");
    }
    ImGui::EndDragDropTarget();
  }
}

}  // namespace gl::patautoui
