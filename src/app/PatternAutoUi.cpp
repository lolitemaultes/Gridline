// Gridline — J-173: pattern automation clips in the channel rack and menus (see PatternAutoUi.h).
#include "PatternAutoUi.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../core/Macros.h"
#include "../core/Automation.h"
#include "../core/I18n.h"
#include "../core/MenuParity.h"
#include "../core/PatternAutoOps.h"
#include "../core/PatternAutomation.h"
#include "../core/RackLayout.h"
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
  if (!c) {
    // K-413: lane persisted across patterns, empty in this pattern.
    const std::string name = paramDisplayName(s, target);
    ImGui::TextDisabled("%s", name.c_str());
    if (ImGui::MenuItem(i18n::tr("Create & edit in this pattern"))) createPatternAutomationClip(a, target, pattern);
    if (ImGui::MenuItem(i18n::tr("Show in the piano roll"))) {
      pianoAutomationLane() = target;
      a.selectPattern(pattern);
      a.openPianoRoll(s.channel(target.owner) ? target.owner : a.selChannel);
      a.showPiano = true;
      a.focusWindow = "Piano roll";
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Remove lane from rack"))) {
      a.doc.editSong("remove automation lane", [&](Song& x) { removeRackAutoLane(x, target.owner, target.index); });
      if (pianoAutomationLane() == target) pianoAutomationLane() = ParamId{};
    }
    return;
  }
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
      a.doc.editSong("rename automation clip", [&](Song& x) {
        if (Pattern* px = x.pattern(pattern))
          if (AutomationClip* q = findPatternLane(*px, target.owner, target.index)) q->name = n;
        updateRackAutoLaneMeta(x, target.owner, target.index, &n, nullptr);  // K-413
      });
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col = 0;
    if (menus::colorGrid(&col))
      a.doc.editSong("automation clip colour", [&](Song& x) {
        if (Pattern* px = x.pattern(pattern))
          if (AutomationClip* q = findPatternLane(*px, target.owner, target.index)) q->color = col;
        updateRackAutoLaneMeta(x, target.owner, target.index, nullptr, &col);  // K-413
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
  if (ImGui::MenuItem(i18n::tr("Clear in this pattern"))) {
    a.doc.editPattern(pattern, "clear automation in pattern", [&](Pattern& x) { removePatternClip(x, target.owner, target.index); });
  }
  ui::Hint("Removes the events in this pattern while keeping the automation lane in the rack");
  if (ImGui::MenuItem(i18n::tr("Remove lane from rack"))) {
    a.doc.editSong("remove automation lane", [&](Song& x) {
      if (Pattern* px = x.pattern(pattern)) removePatternClip(*px, target.owner, target.index);
      removeRackAutoLane(x, target.owner, target.index);
    });
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
    // K-459: one ID per parameter (two can share a name, e.g. the channel's Volume and its
    // instrument's own Volume: "Take › Volume" twice)
    ImGui::PushID(static_cast<int>(pid.owner));
    ImGui::PushID(static_cast<int>(pid.index));
    if (ImGui::MenuItem(paramDisplayName(s, pid).c_str(), nullptr, has)) pick = pid;
    ImGui::PopID();
    ImGui::PopID();
  };
  const std::vector<ParamId> params = listParams(s);
  // K-246: every assigned macro of the project, first (a macro moves what it drives, so one clip
  // automates them all)
  if (const std::vector<ParamId> ms = macro::assignedMacros(s); !ms.empty()) {
    ImGui::TextDisabled("%s", i18n::tr("Macros"));
    for (ParamId pid : ms) add(pid);
    ImGui::Separator();
  }
  const Channel* ch = s.channel(a.selChannel);
  if (ch) {
    ImGui::TextDisabled("%s", ch->name.c_str());
    int shown = 0;
    for (ParamId pid : params)
      if (pid.owner == ch->id && !macro::isMacroParam(pid) && ++shown < 120) add(pid);
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
      if (pid.owner == ch->mixerTrack && !macro::isMacroParam(pid) && ++n < 60) add(pid);
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
  if (!pat) return;
  // K-413: show every channel/song persistent automation lane across patterns,
  // even when the current pattern has no events yet (immediately drawable on click).
  const std::vector<SongAutoLane> lanes = effectiveRackAutoLanes(s, pat);
  if (lanes.empty()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Tick len = std::max<Tick>(1, s.patternContentLength(*pat));
  const Tick barLen = std::max<Tick>(1, s.patternBarTicks(*pat));  // K-179: its own bars
  ParamId open{kNoId, 0};
  bool openCreate = false;
  for (size_t k = 0; k < lanes.size(); ++k) {
    const SongAutoLane& lane = lanes[k];
    const ParamId tgt{lane.owner, lane.index};
    const Song& curSong = a.doc.song();
    const Pattern* curPat = curSong.pattern(pattern);
    const AutomationClip* acPtr = curPat ? findPatternLane(*curPat, tgt.owner, tgt.index) : nullptr;
    const bool hasEvents = acPtr != nullptr;
    AutomationClip acCopy;
    if (acPtr) {
      acCopy = *acPtr;
    } else {
      acCopy.owner = lane.owner;
      acCopy.index = lane.index;
      acCopy.name = lane.name;
      acCopy.color = lane.color;
    }
    ImGui::PushID(static_cast<int>(tgt.packed() & 0x7FFFFFFF) ^ 0x2A000000);
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float cy = rp.y + g.rowH * 0.5f;
    const ImU32 col = theme::rgb(curveRgb(acCopy, k));
    const std::string name = patternAutoName(curSong, acCopy);
    // Automation glyph where a channel's headphones button is (K-186).
    ui::drawIcon(dl, ui::Icon::ClipAutomation, ImVec2(rp.x + g.xLeft + racklayout::kPhonesW * 0.5f, cy), 13,
                 theme::withAlpha(theme::mix(col, P.paper, 0.2f), hasEvents ? 1.0f : 0.65f));
    // Name button over the knob / mixer / name cells (a clip has none of those), from where the pan knob starts.
    const float nx = rp.x + racklayout::kXPan, nw = g.nameX + g.nameW - racklayout::kXPan;
    const ImVec2 np(nx, cy - g.cellH * 0.5f);
    ImGui::SetCursorScreenPos(np);
    ImGui::InvisibleButton("aname", ImVec2(nw, g.cellH));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) open = tgt, openCreate = !hasEvents;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pamenu");
    const ImU32 bg = hov ? P.surface700 : P.surface800;
    dl->AddRectFilled(np, ImVec2(np.x + nw, np.y + g.cellH), theme::mix(bg, P.ink, 0.3f), theme::kRadiusSm);
    dl->AddRectFilled(np, ImVec2(np.x + nw, np.y + g.cellH - 1), bg, theme::kRadiusSm);
    dl->AddRectFilled(np, ImVec2(np.x + 4, np.y + g.cellH), hasEvents ? col : theme::withAlpha(col, 0.55f), theme::kRadiusSm,
                      ImDrawFlags_RoundCornersLeft);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(np.x + 10, std::round(np.y + (g.cellH - 13) * 0.5f) - 1), nw - 14,
                        hasEvents ? P.text : P.textDim, name.c_str());
    if (ui::HintHovered())
      ui::Hint(name + "   " +
               i18n::tr(hasEvents ? "(automation in this pattern \xC2\xB7 click: edit \xC2\xB7 right-click: menu)"
                                  : "(empty in this pattern \xC2\xB7 click: draw automation \xC2\xB7 right-click: menu)"));
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
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) open = tgt, openCreate = !hasEvents;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pamenu2");
    if (chov)
      ui::Hint(name + "   " +
               i18n::tr(hasEvents ? "(click: edit in the automation editor)" : "(empty in this pattern \xC2\xB7 click: draw automation)"));
    if (ImGui::BeginPopup("##pamenu2")) {
      clipMenu(a, pattern, tgt);
      ImGui::EndPopup();
    }
    dl->AddRectFilled(sp, ImVec2(sp.x + g.stepsW, sp.y + g.cellH), chov ? theme::mix(P.field, P.surface700, 0.4f) : P.field, theme::kRadiusSm);
    for (Tick b = barLen; b < len; b += barLen) {  // K-179: its own bars
      const float x = std::round(sp.x + g.stepsW * static_cast<float>(b) / static_cast<float>(len));
      dl->AddLine(ImVec2(x, sp.y + 2), ImVec2(x, sp.y + g.cellH - 2), theme::withAlpha(P.lineStrong, 0.6f));
    }
    if (hasEvents) {
      const float y0 = sp.y + 2, y1 = sp.y + g.cellH - 2;
      std::vector<ImVec2> pts;
      const float xa = std::max(g.viewX0, sp.x), xb = std::min(g.viewX1, sp.x + g.stepsW);
      for (float x = std::floor(xa); x <= xb; x += 1.0f) {
        const double t = static_cast<double>(x - sp.x) / static_cast<double>(g.stepsW) * static_cast<double>(len);
        const double v = std::clamp(automationValue(acCopy, t), 0.0, 1.0);
        pts.emplace_back(x + 0.5f, y1 - static_cast<float>(v) * (y1 - y0));
      }
      if (pts.size() >= 2) {
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, 1.5f);
      }
    } else {
      // K-413: empty in this pattern: a quiet dashed baseline and prompt so it reads as ready to draw.
      const float midY = std::round(sp.y + g.cellH * 0.5f) + 0.5f;
      const float xa = std::max(g.viewX0, sp.x + 4), xb = std::min(g.viewX1, sp.x + g.stepsW - 4);
      const ImU32 dashCol = theme::withAlpha(col, chov ? 0.55f : 0.30f);
      for (float x = xa; x < xb; x += 8.0f) dl->AddLine(ImVec2(x, midY), ImVec2(std::min(x + 4.0f, xb), midY), dashCol, 1.0f);
      const char* emptyTxt = i18n::tr("Click to draw in this pattern");
      theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(std::max(g.viewX0 + 6, sp.x + 8), std::round(sp.y + (g.cellH - theme::kFontLabel) * 0.5f)),
                          std::max(0.0f, xb - std::max(g.viewX0 + 6, sp.x + 8)), chov ? P.textDim : P.textFaint, emptyTxt);
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
  if (open.owner != kNoId) {
    if (openCreate) createPatternAutomationClip(a, open, pattern);
    else openPatternAutomation(a, pattern, open);
  }
}

bool rackDropRowShown(App& a, ObjectId pattern) {
  const ImGuiPayload* drag = ImGui::GetDragDropPayload();
  if (!drag || !drag->IsDataType("GL_CLIPSRC")) return false;
  const auto* src = static_cast<const browserui::ClipSrcPayload*>(drag->Data);
  return src && static_cast<ClipKind>(src->kind) == ClipKind::Automation && a.doc.song().pattern(pattern);
}

void rackDropRow(App& a, ObjectId pattern, float rowW, float rowH) {
  if (!rackDropRowShown(a, pattern)) return;
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
