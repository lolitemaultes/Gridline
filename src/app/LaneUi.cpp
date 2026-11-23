// Gridline - K-255: the lane controls (see LaneUi.h).
#include "LaneUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Grouping.h"
#include "../core/I18n.h"
#include "../core/Lanes.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::laneui {

using theme::pal;

namespace {

ImGuiID gGridId = 0, gLenId = 0;
ObjectId gGridChannel = kNoId, gLenChannel = kNoId;
bool gOpenGrid = false, gOpenLen = false;
int gUnit = 0;      // 0 steps per bar, 1 per beat, 2 per pattern
int gRegrid = 0;    // 0 keep where they sound, 1 keep their order
int gOther = 0;     // the Other field (0: not edited yet)
int gLenField = 0;  // the length field
constexpr uint64_t kLaneDragKey = 0x4C414E4500000000ull;  // "LANE"

int beatsPerBar(const Song& s, const Pattern& p) {
  const Tick bar = s.patternBarTicks(p), beat = s.patternBeatTicks(p);
  return beat > 0 && bar % beat == 0 ? static_cast<int>(std::max<Tick>(1, bar / beat)) : 0;
}

// The unit value shown for a grid (0: the unit cannot show it).
int unitOf(const Song& s, const Pattern& p, int grid, int unit) {
  if (unit == 1) {
    const int b = beatsPerBar(s, p);
    return b > 0 && grid % b == 0 ? grid / b : 0;
  }
  if (unit == 2) return grid * std::max(1, p.bars);
  return grid;
}
// The grid (steps per bar) of a value in a unit (0: none).
int gridOf(const Song& s, const Pattern& p, int v, int unit) {
  if (v <= 0) return 0;
  if (unit == 1) {
    const int b = beatsPerBar(s, p);
    return b > 0 ? v * b : 0;
  }
  if (unit == 2) {
    const int bars = std::max(1, p.bars);
    return v % bars == 0 ? v / bars : 0;
  }
  return v;
}

void title(App& a, const std::string& main, const std::string& rest) {
  const auto& P = pal();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(main.c_str());
  ImGui::PopFont();
  ImGui::SameLine(0, 8);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
  ImGui::TextUnformatted(rest.c_str());
  ImGui::PopStyleColor();
}

void caption(const char* text) {
  const auto& P = pal();
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
  std::string up = i18n::tr(text);
  for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
  ImGui::TextUnformatted(up.c_str());
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

// A one-of-n choice drawn as a round radio row.
bool radio(const char* label, bool on) {
  return ImGui::RadioButton(label, on);
}

void gridPopover(App& a) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  const Channel* c = s.channel(gGridChannel);
  if (!p || !c) {
    ImGui::CloseCurrentPopup();
    return;
  }
  const auto& P = pal();
  const ObjectId pid = p->id, cid = c->id;
  const int cur = lanes::grid(*p, cid);
  const bool own = lanes::ownGrid(*p, cid);
  title(a, i18n::tr("Grid of") + std::string(" ") + c->name, i18n::tr("in this pattern"));
  ImGui::Dummy(ImVec2(0, 2));
  const char* units[] = {i18n::tr("Steps per bar"), i18n::tr("Per beat"), i18n::tr("Per pattern")};
  const char* unitHints[] = {i18n::tr("How many steps the row has in one bar"), i18n::tr("How many steps the row has in one beat"),
                             i18n::tr("How many steps the row has over the whole pattern")};
  ui::Segmented("##laneunit", units, 3, &gUnit, 0.0f, 0.0f, unitHints);
  ImGui::Dummy(ImVec2(0, 2));
  const lanes::Regrid how = gRegrid == 1 ? lanes::Regrid::Order : lanes::Regrid::Nearest;
  auto apply = [&a, pid, cid, how](int g) { a.later([&a, pid, cid, g, how] { lanes::setGrid(a.doc, pid, {cid}, g, how); }); };
  // Presets in the unit: per bar and per pattern the common counts, per beat 1 to 8.
  static const int perBar[] = {3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 24, 32};
  static const int perBeat[] = {1, 2, 3, 4, 5, 6, 7, 8};
  const int* vals = gUnit == 1 ? perBeat : perBar;
  const int nVals = gUnit == 1 ? 8 : 13;
  const float bw = 34.0f;
  for (int i = 0; i < nVals; ++i) {
    int v = vals[i];
    if (gUnit == 2) v *= std::max(1, p->bars);  // per pattern: the bar counts over the pattern's bars
    const int g = gridOf(s, *p, v, gUnit);
    const bool ok = g > 0 && g <= kMaxLaneGrid;
    if (i > 0 && i % 8 != 0) ImGui::SameLine(0, 4);
    ImGui::PushID(i);
    ImGui::BeginDisabled(!ok);
    char lab[16];
    std::snprintf(lab, sizeof lab, "%d", v);
    if (ui::TextButton(lab, ok && g == cur, bw) && ok) apply(g);
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  // Other: any count of the unit.
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(i18n::tr("Other"));
  ImGui::SameLine(0, 8);
  if (gOther <= 0) gOther = std::max(1, unitOf(s, *p, cur, gUnit));
  ImGui::SetNextItemWidth(56);
  if (ImGui::InputInt("##laneother", &gOther, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
    const int g = gridOf(s, *p, std::max(1, gOther), gUnit);
    if (g > 0 && g <= kMaxLaneGrid) apply(g);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    const int g = gridOf(s, *p, std::max(1, gOther), gUnit);
    if (g > 0 && g <= kMaxLaneGrid) apply(g);
  }
  ImGui::SameLine(0, 8);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
  char range[48];
  std::snprintf(range, sizeof range, "%s", gUnit == 0 ? i18n::tr("1 to 64") : gUnit == 1 ? i18n::tr("a whole number") : i18n::tr("a whole number per bar"));
  ImGui::TextUnformatted(range);
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 12);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::litRole(theme::role::kLoop)));
  const std::string exact = gridText(s, *p, cur) + ", " + i18n::tr("exact");
  ImGui::TextUnformatted(exact.c_str());
  ImGui::PopStyleColor();
  ui::Hint("Steps play at their true place in the bar, also between ticks (5 or 7 a bar)");
  ImGui::Separator();
  caption("The steps already set");
  if (radio(i18n::tr("Keep them where they sound (nearest new step)"), gRegrid == 0)) gRegrid = 0;
  if (radio(i18n::tr("Keep their order (step 3 stays step 3)"), gRegrid == 1)) gRegrid = 1;
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::BeginDisabled(!own);
  if (ui::TextButton(i18n::tr("Back to the pattern grid"), false, 0.0f, "The row takes the pattern's steps per bar again")) apply(0);
  ImGui::EndDisabled();
  ImGui::SameLine(0, 6);
  if (ui::TextButton(i18n::tr("Every row like this"), false, 0.0f, "Every row of this pattern takes this row's grid"))
    a.later([&a, pid, cid, how] { lanes::gridLikeAll(a.doc, pid, cid, how); });
}

void lengthPopover(App& a) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  const Channel* c = s.channel(gLenChannel);
  if (!p || !c) {
    ImGui::CloseCurrentPopup();
    return;
  }
  const auto& P = pal();
  const ObjectId pid = p->id, cid = c->id;
  const int total = lanes::steps(*p, cid), len = lanes::length(*p, cid);
  const LaneMode mode = lanes::mode(*p, cid);
  title(a, i18n::tr("Loop of") + std::string(" ") + c->name, i18n::tr("in") + std::string(" ") + p->name);
  ImGui::Dummy(ImVec2(0, 2));
  // A lane given a length here runs on unless it was set to restart.
  auto setLen = [&a, pid, cid](int n) {
    a.later([&a, pid, cid, n] {
      const Pattern* q = a.doc.song().pattern(pid);
      const bool fresh = q && lanes::length(*q, cid) == 0 && !q->lanes.count(cid);
      const LaneMode on = LaneMode::RunOn;
      lanes::setLength(a.doc, pid, {cid}, n, fresh ? &on : nullptr);
    });
  };
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(i18n::tr("Length"));
  ImGui::SameLine(0, 8);
  if (gLenField <= 0) gLenField = len > 0 ? len : total;
  if (ui::IconButton("lendn", ui::Icon::Minus, false, 22, "One step shorter") && gLenField > 1) setLen(--gLenField);
  ImGui::SameLine(0, 4);
  ImGui::SetNextItemWidth(48);
  if (ImGui::InputInt("##lanelen", &gLenField, 0, 0)) gLenField = std::clamp(gLenField, 1, total);
  if (ImGui::IsItemDeactivatedAfterEdit()) setLen(gLenField);
  ImGui::SameLine(0, 4);
  if (ui::IconButton("lenup", ui::Icon::Plus, false, 22, "One step longer") && gLenField < total) setLen(++gLenField);
  ImGui::SameLine(0, 8);
  ImGui::TextUnformatted(i18n::tr("steps"));
  ImGui::SameLine(0, 12);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
  const std::string rng = "1 " + std::string(i18n::tr("to")) + " " + std::to_string(std::max(1, total - 1));
  ImGui::TextUnformatted(rng.c_str());
  ImGui::PopStyleColor();
  // Quick lengths: beats and a bar of the row's own steps, and from the notes.
  const int g = lanes::grid(*p, cid);
  const int b = beatsPerBar(s, *p);
  const int perBeat = b > 0 && g % b == 0 ? g / b : 0;
  struct Q {
    const char* name;
    int steps;
  };
  const Q quick[] = {{i18n::tr("1 beat"), perBeat}, {i18n::tr("2 beats"), perBeat * 2}, {i18n::tr("3 beats"), perBeat * 3}, {i18n::tr("1 bar"), g}};
  bool first = true;
  for (const Q& q : quick) {
    if (!first) ImGui::SameLine(0, 4);
    first = false;
    const bool ok = q.steps > 0 && q.steps < total;
    ImGui::BeginDisabled(!ok);
    if (ui::TextButton(q.name, ok && len == q.steps)) setLen(q.steps), gLenField = q.steps;
    ImGui::EndDisabled();
  }
  ImGui::SameLine(0, 4);
  if (ui::TextButton(i18n::tr("From the notes"), false, 0.0f, "As long as the row's steps and notes reach, rounded up to a beat"))
    a.later([&a, pid, cid, perBeat] {
      ops::loopToContent(a.doc, pid, {cid}, std::max(1, perBeat));
      gLenField = 0;
    });
  ImGui::Separator();
  caption("When the pattern starts again");
  if (radio(i18n::tr("Keep running (true polymeter through the clip)"), mode == LaneMode::RunOn))
    a.later([&a, pid, cid] { lanes::setMode(a.doc, pid, {cid}, LaneMode::RunOn); });
  if (radio(i18n::tr("Start again with the pattern (as before)"), mode == LaneMode::Restart))
    a.later([&a, pid, cid] { lanes::setMode(a.doc, pid, {cid}, LaneMode::Restart); });
  if (const std::string up = lineUpText(s, *p); !up.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::litRole(theme::role::kLoop)));
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Separator();
  ImGui::BeginDisabled(len == 0);
  if (ui::TextButton(i18n::tr("Don't loop"), false, 0.0f, "The row plays its whole length again")) {
    a.later([&a, pid, cid] { lanes::setLength(a.doc, pid, {cid}, 0); });
    gLenField = 0;
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 6);
  ImGui::BeginDisabled(len == 0 && !lanes::ownGrid(*p, cid));
  if (ui::TextButton(i18n::tr("Burn into the pattern"), false, 0.0f,
                     "Writes the lane out as plain steps or notes (a lane that runs on lengthens the pattern to where the lanes line up)"))
    a.later([&a, pid, cid] {
      bool cut = false;
      if (lanes::burn(a.doc, pid, {cid}, &cut) > 0)
        a.notify(cut ? i18n::tr("Burnt into the pattern (cut at 64 bars: the lanes line up later than that)") : i18n::tr("Burnt into the pattern"));
      gLenField = 0;
    });
  ImGui::EndDisabled();
}

}  // namespace

bool columnShown(App& a, const Pattern* p) {
  if (!p) return false;
  if (a.settings.getBool("rack.laneColumn") || lanes::any(*p)) return true;
  return false;
}

std::string gridText(const Song& s, const Pattern& p, int grid) {
  const int b = beatsPerBar(s, p);
  char buf[48];
  if (b > 1 && grid % b == 0)
    std::snprintf(buf, sizeof buf, "%d %s", grid / b, i18n::tr("per beat"));
  else
    std::snprintf(buf, sizeof buf, "%d %s", grid, i18n::tr("per bar"));
  return buf;
}

std::string lineUpText(const Song& s, const Pattern& p) {
  if (!lanes::anyRunOn(p)) return {};
  const int64_t bars = lanes::lineUpBars(s, p, 100000);
  if (bars >= 100000) return i18n::tr("The lanes line up again only after a very long time");
  return std::string(i18n::tr("Every lane lines up again after")) + " " + std::to_string(bars) + " " + (bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
}

void beginRows() {
  gGridId = ImGui::GetID("##lanegridpop");
  gLenId = ImGui::GetID("##lanelenpop");
}

void openGrid(ObjectId channel) {
  gGridChannel = channel;
  gOpenGrid = true;
  gOther = 0;
}

void openLength(ObjectId channel) {
  gLenChannel = channel;
  gOpenLen = true;
  gLenField = 0;
}

void chip(App& a, const Pattern& p, const Channel& c, ImVec2 p0, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  ImGui::PushID("lanechip");
  const bool click = ImGui::InvisibleButton("c", size);
  const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  const bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  const bool own = lanes::ownGrid(p, c.id);
  const int g = lanes::grid(p, c.id);
  const int len = lanes::length(p, c.id);
  const ImU32 hot = P.hot;
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  const float r = size.y * 0.5f;
  dl->AddRectFilled(p0, p1, hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, r);
  if (own) dl->AddRect(ImVec2(p0.x + 0.5f, p0.y + 0.5f), ImVec2(p1.x - 0.5f, p1.y - 0.5f), theme::withAlpha(hot, 0.85f), r, 0, 1.2f);
  char buf[8];
  std::snprintf(buf, sizeof buf, "%d", g);
  const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontNumber, 1e9f, 0, buf);
  const float tx = len > 0 ? p0.x + (size.x - 6 - ts.x) * 0.5f : p0.x + (size.x - ts.x) * 0.5f;
  dl->AddText(a.fontMono, theme::kFontNumber, ImVec2(std::round(tx), std::round(p0.y + (size.y - ts.y) * 0.5f)), own ? hot : P.textDim, buf);
  if (len > 0) {  // the lane loops: a small loop mark, lit while it runs on
    const bool on = lanes::runsOn(p, c.id);
    const ImU32 lc = on ? theme::litRole(theme::role::kLoop) : P.textFaint;
    const ImVec2 cc(p1.x - 7, p0.y + size.y * 0.5f);
    dl->PathArcTo(cc, 3.2f, 0.6f, 5.6f, 10);
    dl->PathStroke(lc, 0, 1.3f);
    dl->AddTriangleFilled(ImVec2(cc.x + 1.6f, cc.y - 4.6f), ImVec2(cc.x + 4.4f, cc.y - 1.6f), ImVec2(cc.x + 0.6f, cc.y - 0.8f), lc);
  }
  if (hov) {
    std::string h = c.name + ": " + gridText(a.doc.song(), p, g) + (own ? std::string(" (") + i18n::tr("its own grid") + ")" : std::string(" (") + i18n::tr("the pattern's grid") + ")");
    if (len > 0) h += std::string(" \xC2\xB7 ") + i18n::tr("length") + " " + std::to_string(len) + " \xC2\xB7 " + (lanes::runsOn(p, c.id) ? i18n::tr("runs on") : i18n::tr("restarts"));
    h += std::string("   (") + i18n::tr("click: grid \xC2\xB7 right-click: length") + ")";
    ui::Hint(h);
  }
  if (click) openGrid(c.id);
  if (rclick) openLength(c.id);
}

int handleInput(App& a, const Pattern& p, const Channel& c, Handle& h, int steps, const std::function<float(int)>& posOf, bool* clicked) {
  if (clicked) *clicked = false;
  ImGui::SetCursorScreenPos(ImVec2(h.x - 4, h.y0));
  ImGui::PushID("lanehandle");
  ImGui::InvisibleButton("h", ImVec2(8, std::max(1.0f, h.y1 - h.y0)));
  ImGui::PopID();
  h.hot = ImGui::IsItemHovered();
  h.held = ImGui::IsItemActive();
  if (h.hot || h.held) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (h.hot && !h.held)
    ui::Hint(h.looping ? ("Lane length: drag to change it \xC2\xB7 click: length and what it does when the pattern starts again")
                       : i18n::N_("Drag to give this row a length of its own (it runs on through longer clips) \xC2\xB7 click for more"));
  int to = -1;
  static float pressX = 0;
  static bool dragged = false;
  if (ImGui::IsItemActivated()) pressX = ImGui::GetMousePos().x, dragged = false;
  if (h.held && std::fabs(ImGui::GetMousePos().x - pressX) > 3.0f) dragged = true;
  if (h.held && dragged) {
    const float mx = ImGui::GetMousePos().x;
    int best = 1;
    float bd = 1e9f;
    for (int k = 1; k <= steps; ++k) {
      const float d = std::fabs(mx - posOf(k));
      if (d < bd) bd = d, best = k;
    }
    if (best != h.at) {
      to = best;
      const Pattern* q = a.doc.song().pattern(p.id);
      const bool fresh = q && lanes::length(*q, c.id) == 0 && !q->lanes.count(c.id);
      const LaneMode on = LaneMode::RunOn;
      lanes::setLength(a.doc, p.id, {c.id}, best, fresh ? &on : nullptr, kLaneDragKey ^ (p.id * 131 + c.id));
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (!dragged && clicked) *clicked = true;
    a.doc.endGesture();
  }
  return to;
}

void handleDraw(ImDrawList* dl, const Handle& h, LaneMode mode) {
  const auto& P = pal();
  const ImU32 hot = P.hot;
  const ImU32 col = h.looping ? hot : theme::withAlpha(P.textFaint, h.hot || h.held ? 0.9f : 0.45f);
  const float x = std::round(h.x);
  dl->AddRectFilled(ImVec2(x - 1.5f, h.y0), ImVec2(x + 1.5f, h.y1), h.hot || h.held ? theme::mix(col, P.paper, 0.25f) : col, 1.0f);
  if (h.looping) {
    // A tab at the top with the length (over the row's top edge); dashed when the lane starts again
    // with the pattern.
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d", h.at);
    ImFont* f = ImGui::GetFont();
    const float fs = theme::kFontLabel - 1;
    const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, buf);
    const ImVec2 t0(x - 1.5f, h.y0 - 4), t1(x + ts.x + 5, h.y0 - 4 + ts.y + 1);
    dl->AddRectFilled(t0, t1, hot, 2.0f, ImDrawFlags_RoundCornersRight);
    dl->AddText(f, fs, ImVec2(std::round(t0.x + 3), std::round(t0.y)), theme::contrastText(hot), buf);
    if (mode == LaneMode::Restart) {
      for (float y = t1.y + 2; y < h.y1 - 2; y += 4) dl->AddLine(ImVec2(x, y), ImVec2(x, y + 1.5f), theme::withAlpha(P.surface950, 0.8f), 1.0f);
    }
  }
}

void popovers(App& a) {
  if (gOpenGrid) {
    ImGui::OpenPopup(gGridId);
    gOpenGrid = false;
  }
  if (gOpenLen) {
    ImGui::OpenPopup(gLenId);
    gOpenLen = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS3));
  if (ImGui::BeginPopupEx(gGridId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    gridPopover(a);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupEx(gLenId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    lengthPopover(a);
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void menuItems(App& a) {
  const bool on = a.settings.getBool("rack.laneColumn");
  if (ImGui::MenuItem(i18n::tr("Show row grids and lengths"), nullptr, on)) a.settings.set("rack.laneColumn", !on);
  ui::Hint("Each row shows its grid (steps per bar) and a handle for its length; rows with their own always show them");
}

}  // namespace gl::laneui
