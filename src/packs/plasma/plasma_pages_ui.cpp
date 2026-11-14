// Gridline — "plasma" pack: the Routing, Links and Setup views of the stage.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

void curveGlyph(const Ctx& c, float x, float y, float w, float h, double bend, const Curve* cv, ImU32 col) {
  panelRect(c, x, y, w, h, c.P.field, 2);
  ImVec2 prev;
  for (int i = 0; i <= 16; ++i) {
    const float t = i / 16.f;
    const float v = cv && !cv->empty() ? static_cast<float>(curveAt(*cv, t)) : static_cast<float>(bendCurve(t, bend));
    const ImVec2 q = c.p(x + 3 + t * (w - 6), y + h - 3 - std::clamp(v, 0.f, 1.f) * (h - 6));
    if (i) c.dl->AddLine(prev, q, col, std::max(1.2f, c.s(1.3f)));
    prev = q;
  }
}

// Curve popup: point editor of the slot's source (or aux) curve + its bend and smoothing.
void curvePopup(Ctx& c, int s, bool aux) {
  Curve& cv = aux ? c.m.auxCurve[static_cast<size_t>(s)] : c.m.modCurve[static_cast<size_t>(s)];
  ImGui::TextDisabled("%s", aux ? i18n::tr("Scale-by curve") : i18n::tr("Response curve"));
  const ImVec2 o = ImGui::GetCursorScreenPos();
  skin::Form F2 = c.F;
  F2.o = o;
  Ctx cc{c.a, c.t, F2, ImGui::GetWindowDrawList(), c.st, c.m, c.u, c.P, c.live, c.hasLive, c.graph};
  displayWellPublic(cc, 0, 0, 220, 140);
  gridLines(cc, 0, 0, 220, 140, 4, 4, false);
  Curve edit = cv.empty() ? Curve{} : cv;
  if (edit.empty()) {
    const double b = val(c, modP(s, aux ? M_AuxCurve : M_Curve));
    for (int i = 0; i <= 4; ++i) edit.push_back({i / 4.f, static_cast<float>(bendCurve(i / 4.0, b)), 0});
  }
  static int sel = -1;
  if (curveEditor(cc, aux ? "auxcv" : "modcv", edit, 0, 0, 220, 140, 4, 4, sel, true, c.P.fam[familyOf(vali(c, modP(s, M_Source)))])) {
    cv = edit;
    commit(c, "Edit link curve", 2000 + s);
  }
  ImGui::Dummy(F2.sz(220, 146));
  if (ImGui::Button(i18n::tr("Straight"))) {
    cv.clear();
    commitParams(c, "Reset link curve", {{modP(s, aux ? M_AuxCurve : M_Curve), 0}});
  }
  ImGui::Separator();
  paramRows(c, {modP(s, aux ? M_AuxCurve : M_Curve), modP(s, M_Rise), modP(s, M_Fall)});
}

void sourceField(Ctx& c, int k, float x, float y, float w, float h, int slot) {
  const int src = vali(c, k);
  const std::string nm = src > SrcNone ? sourceLabel(src) : "None";
  reach(k);
  dropField(
      c, "src", x, y, w, h, nm.c_str(),
      [&] {
        int out = -1;
        if (sourceMenuItems(c, out)) setv(c, k, out < 0 ? 0 : out);
      },
      false, nullptr, src <= SrcNone);
  // accept a source dragged from a tab / macro
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_SRC")) {
      int sv = 0;
      std::memcpy(&sv, p->Data, sizeof(int));
      setv(c, k, sv);
    }
    ImGui::EndDragDropTarget();
  }
  (void)slot;
}

void destField(Ctx& c, int s, float x, float y, float w, float h) {
  const int d = vali(c, modP(s, M_Dest));
  reach(modP(s, M_Dest));
  const std::string nm = d > 0 ? paramLongName(d - 1) : "Choose a target";
  dropField(
      c, "dst", x, y, w, h, nm.c_str(),
      [&] {
        int out = -2;
        if (destMenuItems(c, out)) setv(c, modP(s, M_Dest), out < 0 ? 0 : out + 1);
      },
      false, nullptr, d <= 0);
}
void clearSlot(Ctx& c, int s) {
  std::vector<std::pair<int, double>> ps;
  for (int q = 0; q < M_Count; ++q)
    if (const ParamDesc* d = pdesc(modP(s, q))) ps.push_back({modP(s, q), d->def});
  commitParams(c, "Remove link", ps);  // one undo step
}
std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
}  // namespace

// ================================================================== Routing
void routingView(Ctx& c) {
  stageHead(c, "Routing");
  ImGui::PushID("routing");
  struct Row {
    const char* name;
    int r, on, level, pan;
  };
  const Row rows[kRoutes] = {{"Layer 1", RtA, oscP(0, O_Enable), oscP(0, O_Level), oscP(0, O_Pan)},
                             {"Layer 2", RtB, oscP(1, O_Enable), oscP(1, O_Level), oscP(1, O_Pan)},
                             {"Layer 3", RtC, oscP(2, O_Enable), oscP(2, O_Level), oscP(2, O_Pan)},
                             {"Root", RtSub, subP(S_Enable), subP(S_Level), subP(S_Pan)},
                             {"Texture", RtNoise, noiseP(N_Enable), noiseP(N_Level), noiseP(N_Pan)},
                             {"Filter 1", RtF1, filtP(0, F_Enable), filtP(0, F_Level), -1},
                             {"Filter 2", RtF2, filtP(1, F_Enable), filtP(1, F_Level), -1}};
  const float cx[] = {16, 40, 124, 254, 364, 594, 712, 806, 900}, cw[] = {22, 80, 122, 102, 222, 110, 86, 86, 88};
  const char* heads[] = {"", "Source", "Level", "Pan", "Goes to", "Filter 1 / 2", "Send A", "Send B", "Envelope 1"};
  const float y0 = kStageY + 10;
  for (int i = 1; i < 9; ++i) text(c, cx[i] + 2, y0, heads[i], c.P.label, 11.0f, true);
  for (int i = 0; i < kRoutes; ++i) {
    const Row& rw = rows[i];
    const float y = y0 + 26 + i * 38.f;
    ImGui::PushID(i);
    panelRect(c, 12, y - 5, kStageW - 24, 36, (i & 1) ? c.P.panel : c.P.panelLo, 4);
    // on / off
    {
      reach(rw.on);
      const bool on = val(c, rw.on) >= 0.5;
      ImGui::SetCursorScreenPos(c.p(cx[0], y));
      if (ImGui::InvisibleButton("##on", c.F.sz(cw[0], 26))) setv(c, rw.on, on ? 0 : 1);
      if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(rw.on) + (on ? ": on (click: off)" : ": off (click: on)"));
      const ImVec2 dc = c.p(cx[0] + 10, y + 13);
      if (on) c.dl->AddCircleFilled(dc, c.s(4), c.P.good, 14);
      else c.dl->AddCircle(dc, c.s(4), c.P.boxEdge, 14, std::max(1.0f, c.s(1.2f)));
    }
    labelBeside(c, cx[1] + 2, y, 26, rw.name, val(c, rw.on) >= 0.5 ? c.P.text : c.P.textFaint, 12.0f, true);
    bar(c, rw.level, cx[2], y, cw[2], 26, "", BarNoLabel);
    if (rw.pan >= 0) bar(c, rw.pan, cx[3], y, cw[3], 26, "", BarNoLabel);
    else bar(c, filtP(rw.r == RtF1 ? 0 : 1, F_Stereo), cx[3], y, cw[3], 26, "Stereo", BarCentre);
    segments(c, routeP(rw.r, R_Dest), cx[4], y, cw[4], 26, {"Filters", "Out", "Direct", "Off"});
    const bool toF = vali(c, routeP(rw.r, R_Dest)) == RouteFilter;
    bar(c, routeP(rw.r, R_Balance), cx[5], y, cw[5], 26, "", BarNoLabel | BarCentre | (toF ? 0 : BarDim));
    bar(c, routeP(rw.r, R_Bus1), cx[6], y, cw[6], 26, "", BarNoLabel);
    bar(c, routeP(rw.r, R_Bus2), cx[7], y, cw[7], 26, "", BarNoLabel);
    chip(c, tailP(T_RawA + rw.r), cx[8], y, cw[8], 26, "Leaves out");
    ImGui::PopID();
  }
  const float by = y0 + 26 + kRoutes * 38.f + 8;
  const float bh = kStageB - 12 - by;
  {
    const float y = card(c, 9, by, 244, bh, "Send A");
    fieldGrid(c, 20, y, 222, 1, 34, {{G_Bus1Vol, "Level"}, {G_Bus1Dest, "Goes to"}}, 26);
    text(c, 20, y + 74, "Its effects: the Send A chain", c.P.textDim, 11.0f, false);
  }
  {
    const float y = card(c, 253, by, 244, bh, "Send B");
    fieldGrid(c, 264, y, 222, 1, 34, {{G_Bus2Vol, "Level"}, {G_Bus2Dest, "Goes to"}}, 26);
    text(c, 264, y + 74, "Its effects: the Send B chain", c.P.textDim, 11.0f, false);
  }
  {
    const float y = card(c, 497, by, 244, bh, "Send chaining");
    field(c, tailP(T_BusChain), 508, y, 222, 26, "Chain");
    text(c, 508, y + 40, "One send can feed the other", c.P.textDim, 11.0f, false);
    text(c, 508, y + 58, "instead of the output.", c.P.textDim, 11.0f, false);
  }
  {
    const float y = card(c, 741, by, 250, bh, "Out");
    fieldGrid(c, 752, y, 226, 1, 34, {{G_MasterVol, "Output"}, {G_DirectVol, "Direct out"}}, 26);
    drawMeter(c, 752, y + 76, 26, bh - 116);
    text(c, 786, y + 78, "Output meter", c.P.textDim, 11.0f, false);
  }
  ImGui::PopID();
}

// ================================================================== Links
// The editor of link slot s (source, target, response curve, depth and the rest, scale-by), from
// (x, y) w wide; the Links view and the Patch view's inspector. Returns the y under it.
float linkEditor(Ctx& c, int s, float x, float y, float w) {
  ImGui::PushID(s + 6000);
  sourceField(c, modP(s, M_Source), x + 14, y, 140, 26, s);
  iconArrow(c.dl, c.p(x + 163, y + 13), c.s(7), 1, c.P.textDim);
  destField(c, s, x + 172, y, w - 186, 26);
  y += 34;
  // response curve, large
  {
    Curve& cv = c.m.modCurve[static_cast<size_t>(s)];
    const float gx = x + 14, gw = w - 28, gh = 120;
    text(c, gx, y, "Response curve", c.P.label, 11.0f, false);
    display(c, gx, y + 16, gw, gh, 4, 4);
    Curve edit = cv;
    if (edit.empty()) {
      const double bnd = val(c, modP(s, M_Curve));
      for (int i = 0; i <= 4; ++i) edit.push_back({i / 4.f, static_cast<float>(bendCurve(i / 4.0, bnd)), 0});
    }
    static int sel = -1;
    if (curveEditor(c, "modcv", edit, gx, y + 16, gw, gh, 4, 4, sel, true, c.P.fam[familyOf(vali(c, modP(s, M_Source)))])) {
      cv = edit;
      commit(c, "Edit link curve", 2000 + s);
    }
    if (button(c, "straight", gx + gw - 80, y - 4, 80, 20, "Straight", false, 10.5f)) {
      cv.clear();
      commitParams(c, "Reset link curve", {{modP(s, M_Curve), 0}});
    }
    y += gh + 22;
  }
  y = fieldGrid(c, x + 14, y, w - 28, 2, 32,
                {{modP(s, M_Amount), "Depth"}, {modP(s, M_Bipolar), "Centred"}, {modP(s, M_Curve), "Curve bend"}, {modP(s, M_Out), "Trim"},
                 {modP(s, M_Rise), "Rise"},     {modP(s, M_Fall), "Fall"},       {modP(s, M_Bypass), "Bypass"}}, 26);
  y += 4;
  text(c, x + 14, y + 6, "Scale by", c.P.label, 11.0f, false);
  sourceField(c, modP(s, M_Aux), x + 76, y, 130, 26, s);
  chip(c, modP(s, M_AuxInv), x + 212, y, w - 226, 26, "Inverted");
  y += 32;
  {
    Curve& cv = c.m.auxCurve[static_cast<size_t>(s)];
    const float gx = x + 14, gw = w - 28, gh = 56;
    display(c, gx, y, gw, gh, 4, 2);
    Curve edit = cv;
    if (edit.empty()) {
      const double bnd = val(c, modP(s, M_AuxCurve));
      for (int i = 0; i <= 4; ++i) edit.push_back({i / 4.f, static_cast<float>(bendCurve(i / 4.0, bnd)), 0});
    }
    static int sel = -1;
    if (curveEditor(c, "auxcv", edit, gx, y, gw, gh, 4, 2, sel, true, c.P.textDim)) {
      cv = edit;
      commit(c, "Edit link curve", 2100 + s);
    }
    y += gh + 6;
  }
  bar(c, modP(s, M_AuxCurve), x + 14, y, w - 28, 26, "Scale-by bend", BarCentre);
  y += 32;
  if (button(c, "remove", x + 14, y, 120, 26, "Remove link", false, 11.0f)) clearSlot(c, s);
  ImGui::PopID();
  return y + 32;
}

namespace {
// Where a target lives (the section and part) and that part's colour.
std::pair<std::string, ImU32> targetInfo(const Ctx& c, int k) {
  if (k >= kOscBase && k < kSubBase) {
    const int o = (k - kOscBase) / O_Count;
    return {"Sources, Layer " + std::to_string(o + 1), partCol(c, o)};
  }
  if (k >= kSubBase && k < kNoiseBase) return {"Sources, Root", partCol(c, 3)};
  if (k >= kNoiseBase && k < kFiltBase) return {"Sources, Texture", partCol(c, 4)};
  if (k >= kFiltBase && k < kRouteBase) {
    const int f = (k - kFiltBase) / F_Count;
    return {"Shape, Filter " + std::to_string(f + 1), partCol(c, 10 + f)};
  }
  if (k >= kRouteBase && k < kEnvBase) return {"Routing", c.P.label};
  if (k >= kEnvBase && k < kLfoBase) return {"Movement, Envelope " + std::to_string((k - kEnvBase) / E_Count + 1), c.P.fam[FamEnv]};
  if (k >= kLfoBase && k < kMacroBase) return {"Movement, LFO " + std::to_string((k - kLfoBase) / L_Count + 1), c.P.fam[FamLfo]};
  if (k >= kMacroBase && k < kModBase) return {"Movement", c.P.fam[FamMacro]};
  if (k >= kModBase && k < kFxBase) return {"Links", c.P.label};
  if (k >= kFxBase && k < kTailBase) {
    const int s = (k - kFxBase) / X_Count;
    return {"Effects", fxCol(c, vali(c, fxP(s, X_Type)))};
  }
  return {"Sound", c.P.label};
}

// A source in a few words (an LFO's speed and start, a macro's name).
std::string sourceSub(const Ctx& c, int src) {
  if (src >= SrcLfo1 && src <= SrcLfo10) {
    const int l = src - SrcLfo1, mode = vali(c, lfoP(l, L_Mode));
    return std::string(val(c, lfoP(l, L_Bpm)) >= 0.5 ? "Tempo " : "Free ") + ptext(c, lfoP(l, L_Rate)) +
           (mode == 0 ? ", keeps going" : mode == 1 ? ", restarts each note" : ", plays once");
  }
  if (src >= SrcEnv1 && src <= SrcEnv4) return src == SrcEnv1 ? "the Amp's envelope" : "an envelope";
  if (src >= SrcMacro1 && src <= SrcMacro8) return "Macro " + std::to_string(src - SrcMacro1 + 1);
  if (src == SrcVelo || src == SrcModWheel || src == SrcNote || src == SrcAftertouch || src == SrcPitchBend) return "key and touch";
  return sourceGroup(src);
}
std::string sourceShown(const Ctx& c, int src) {
  if (src >= SrcMacro1 && src <= SrcMacro8) {
    const std::string& n = c.m.macroName[static_cast<size_t>(src - SrcMacro1)];
    if (!n.empty()) return n;
  }
  return sourceLabel(src);
}

// A little on / off switch.
bool toggleLook(Ctx& c, const char* id, float x, float y, bool on, ImU32 col) {
  const float w = M(32), h = M(18);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  const bool click = ImGui::InvisibleButton("##tg", c.F.sz(w, h));
  ImGui::PopID();
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), on ? theme::withAlpha(col, 0.3f) : c.P.field, c.s(h * 0.5f));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), on ? col : c.P.line, c.s(h * 0.5f));
  c.dl->AddCircleFilled(c.p(on ? x + w - h * 0.5f : x + h * 0.5f, y + h * 0.5f), c.s(h * 0.5f - M(3)), on ? col : c.P.textFaint, 16);
  return click;
}

// A chip that names a source or target and opens its menu.
void endChip(Ctx& c, const char* id, float x, float y, float w, const std::string& name, ImU32 col, bool source, int s) {
  const float h = M(28);
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##chip", c.F.sz(w, h))) ImGui::OpenPopup("##endmenu");
  const bool hv = ImGui::IsItemHovered();
  if (hv) c.a.setHint(source ? "Click to choose what moves it" : "Click to choose what it moves");
  layoutBox(c, x, y, w, h, name.c_str());
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), hv ? c.P.lineHi : c.P.line, c.s(h * 0.5f));
  if (source) c.dl->AddCircleFilled(c.p(x + M(14), y + h * 0.5f), c.s(M(3.5f)), col, 12);
  else c.dl->AddRectFilled(c.p(x + M(11), y + h * 0.5f - M(3.5f)), c.p(x + M(18), y + h * 0.5f + M(3.5f)), col, c.s(M(1.5f)));
  std::string shown = name;
  while (shown.size() > 3 && textW(c, shown.c_str(), T(12), false) > w - M(36)) shown.pop_back();
  text(c, x + M(26), y + h * 0.5f - T(12) * 0.68f, shown.c_str(), c.P.text, T(12), false);
  if (ImGui::BeginPopup("##endmenu")) {
    int out = source ? -1 : -2;
    if (source) {
      if (sourceMenuItems(c, out)) setv(c, modP(s, M_Source), out < 0 ? 0 : out);
    } else if (destMenuItems(c, out)) {
      setv(c, modP(s, M_Dest), out < 0 ? 0 : out + 1);
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}
}  // namespace

void linksView(Ctx& c) {
  Ui& u = c.u;
  const Pal& P = c.P;
  panelRect(c, 0, kTopH, kW, kPlayY - kTopH, P.frame);
  const float x0 = M(20), lw = M(930), px = M(966), pw = kW - M(20) - px, top = kTopH + M(16), bottom = kPlayY - M(16);
  int used = 0;
  for (int s = 0; s < kModSlots; ++s) used += vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0;
  if (u.linkSel < 0)  // the first link is the one shown at the right until another is picked
    for (int s = 0; s < kModSlots && u.linkSel < 0; ++s)
      if (vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0) u.linkSel = s;
  // the bar: grouping, search, how many, New link, more
  {
    const float h = M(28);
    const float gw = M(214);
    const int hit = segPick(c, "grp", x0, top, gw, h, {"By source", "By target", "By slot"}, u.linkGroup, false, T(11.5f));
    if (hit >= 0 && hit != u.linkGroup) u.linkGroup = hit, u.mxScroll = 0;
    const float sx = x0 + gw + M(12), sw = M(220);
    c.dl->AddRectFilled(c.p(sx, top), c.p(sx + sw, top + h), P.field, c.s(M(4)));
    c.dl->AddRect(c.p(sx, top), c.p(sx + sw, top + h), P.line, c.s(M(4)));
    {
      const ImVec2 m = c.p(sx + M(14), top + h * 0.5f - M(1));
      c.dl->AddCircle(m, c.s(M(3.8f)), P.textFaint, 14, std::max(1.f, c.s(M(1.3f))));
      c.dl->AddLine(ImVec2(m.x + c.s(M(2.9f)), m.y + c.s(M(2.9f))), ImVec2(m.x + c.s(M(6)), m.y + c.s(M(6))), P.textFaint, std::max(1.f, c.s(M(1.3f))));
    }
    char fb[64];
    std::snprintf(fb, sizeof fb, "%s", u.linkFilter.c_str());
    ImGui::SetCursorScreenPos(c.p(sx + M(26), top + M(2)));
    ImGui::PushItemWidth(c.s(sw - M(32)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(c.s(M(2)), (c.s(h - M(4)) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, P.textFaint);
    if (ImGui::InputTextWithHint("##lfind", i18n::tr("Find a link"), fb, sizeof fb)) u.linkFilter = fb;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    ImGui::PopItemWidth();
    float rx = x0 + lw;
    {
      const float w = h;
      rx -= w;
      if (btnLook(c, "more", rx, top, w, h, "...", false, false, T(12))) ImGui::OpenPopup("##linksmore");
      if (ImGui::IsItemHovered()) c.a.setHint("More: remove every link");
      if (ImGui::BeginPopup("##linksmore")) {
        if (ImGui::BeginMenu(i18n::tr("Remove every link"))) {
          if (ImGui::MenuItem(i18n::tr("Yes, remove them all")))
            later(c, [](App& a, const PluginTarget& t) {
              editState(a, t, "Remove every link", [](PluginState& st, Model&) {
                for (int s = 0; s < kModSlots; ++s)
                  for (int q = 0; q < M_Count; ++q)
                    if (const ParamDesc* d = pdesc(modP(s, q))) st.params[static_cast<size_t>(modP(s, q))] = d->def;
              });
            });
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      rx -= M(10);
    }
    {
      const float w = textW(c, "New link", T(12), false) + M(24);
      rx -= w;
      if (btnLook(c, "new", rx, top, w, h, "New link", false, false, T(12)))
        for (int s = 0; s < kModSlots; ++s)
          if (vali(c, modP(s, M_Source)) <= SrcNone && vali(c, modP(s, M_Dest)) <= 0) {
            u.linkSel = s;
            break;
          }
      if (ImGui::IsItemHovered()) c.a.setHint("New link: the first free slot; choose both ends at the right");
      rx -= M(10);
    }
    {
      char b[32];
      std::snprintf(b, sizeof b, "%d of %d used", used, kModSlots);
      const float w = textW(c, b, T(11.5f), false);
      rx -= w;
      text(c, rx, top + h * 0.5f - T(11.5f) * 0.68f, b, P.label, T(11.5f), false);
      rx -= M(10) + M(90);
      c.dl->AddRectFilled(c.p(rx, top + h * 0.5f - M(1)), c.p(rx + M(90), top + h * 0.5f + M(1)), P.field, c.s(M(1)));
      c.dl->AddRectFilled(c.p(rx, top + h * 0.5f - M(1)), c.p(rx + M(90) * used / kModSlots, top + h * 0.5f + M(1)), P.fam[FamLfo], c.s(M(1)));
      c.dl->AddCircleFilled(c.p(rx + M(90) * used / kModSlots, top + h * 0.5f), c.s(M(2.5f)), P.fam[FamLfo], 10);
    }
  }
  // the list and the cards under it
  const std::string q = lower(u.linkFilter);
  auto pass = [&](int s) {
    if (q.empty()) return true;
    const int d = vali(c, modP(s, M_Dest));
    return lower(sourceShown(c, vali(c, modP(s, M_Source)))).find(q) != std::string::npos ||
           (d > 0 && lower(paramLongName(d - 1)).find(q) != std::string::npos);
  };
  struct Group {
    int src = -1;  // -1: by target / slot order
    std::string head, sub;
    std::vector<int> slots;
  };
  std::vector<Group> groups;
  if (u.linkGroup == 2) {
    Group g;
    for (int s = 0; s < kModSlots; ++s)
      if (pass(s)) g.slots.push_back(s);
    groups.push_back(g);
  } else {
    std::map<int, size_t> at;
    std::vector<std::pair<int, int>> keyed;  // key, slot
    for (int s = 0; s < kModSlots; ++s) {
      const int src = vali(c, modP(s, M_Source)), d = vali(c, modP(s, M_Dest));
      if ((src <= SrcNone && d <= 0) || !pass(s)) continue;
      keyed.push_back({u.linkGroup == 0 ? src : d, s});
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](auto a, auto b) { return a.first < b.first; });
    for (const auto& [key, s] : keyed) {
      if (!at.count(key)) {
        Group g;
        if (u.linkGroup == 0) g.src = key, g.head = key > SrcNone ? sourceShown(c, key) : "No source", g.sub = key > SrcNone ? sourceSub(c, key) : "";
        else g.head = key > 0 ? paramLongName(key - 1) : "No target", g.sub = key > 0 ? targetInfo(c, key - 1).first : "";
        at[key] = groups.size();
        groups.push_back(g);
      }
      groups[at[key]].slots.push_back(s);
    }
  }
  // the sources with nothing to move yet
  std::vector<int> ready;
  {
    std::vector<int> cand = {SrcEnv2, SrcEnv3, SrcEnv4};
    for (int m = 0; m < 4; ++m) cand.push_back(SrcMacro1 + m);
    cand.push_back(SrcVelo), cand.push_back(SrcModWheel);
    for (int src : cand) {
      bool has = false;
      for (int s = 0; s < kModSlots && !has; ++s) has = vali(c, modP(s, M_Source)) == src && vali(c, modP(s, M_Dest)) > 0;
      if (!has) ready.push_back(src);
    }
  }
  const float helpH = M(96), helpY = bottom - helpH;
  const float listTop = top + M(28) + M(14);
  // ready to link: chips that wrap
  float readyH = 0;
  std::vector<std::pair<ImVec2, float>> chipAt;
  {
    float cx = x0 + M(16), cy = 0;
    for (int src : ready) {
      const std::string nm = sourceShown(c, src), sb = sourceSub(c, src);
      const float w = M(14 + 7 + 8) + textW(c, nm.c_str(), T(12), false) + M(8) + textW(c, sb.c_str(), T(11), false) + M(14);
      if (cx + w > x0 + lw - M(16)) cx = x0 + M(16), cy += M(38);
      chipAt.push_back({ImVec2(cx, cy), w});
      cx += w + M(8);
    }
    readyH = ready.empty() ? 0 : M(58) + cy + M(32) + M(16);
  }
  const float readyY = helpY - M(14) - readyH;
  // the link list, scrolled
  {
    const float lt = listTop, lb = (ready.empty() ? helpY : readyY) - M(14);
    float total = 0;
    for (const auto& g : groups) total += (g.src >= 0 || !g.head.empty() ? M(58) : 0) + g.slots.size() * M(56) + M(12);
    const float viewH = lb - lt;
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(c.p(x0, lt), c.p(x0 + lw, lb)) && ImGui::GetIO().MouseWheel != 0)
      u.mxScroll -= ImGui::GetIO().MouseWheel * 56;
    u.mxScroll = std::clamp(u.mxScroll, 0.f, std::max(0.f, total - viewH));
    ImGui::PushClipRect(c.p(x0 - M(2), lt), c.p(x0 + lw + M(2), lb), true);
    c.dl->PushClipRect(c.p(x0 - M(2), lt), c.p(x0 + lw + M(2), lb), true);
    layoutScroll(c);
    float y = lt - u.mxScroll;
    if (groups.empty() || (groups.size() == 1 && groups[0].slots.empty()))
      text(c, x0 + M(16), lt + M(16), q.empty() ? "No links yet: drag a movement block's dot onto any knob" : "No link has that in its name", P.textFaint, T(12), false);
    for (const auto& g : groups) {
      if (g.slots.empty()) continue;
      const bool hasHead = g.src >= 0 || !g.head.empty();
      const float gh = (hasHead ? M(58) : 0) + g.slots.size() * M(56);
      c.dl->AddRectFilled(c.p(x0, y), c.p(x0 + lw, y + gh), barBg(c), c.s(M(6)));
      c.dl->AddRect(c.p(x0, y), c.p(x0 + lw, y + gh), P.line, c.s(M(6)));
      float ry = y;
      if (hasHead) {
        const float cy = y + M(29);
        float tx = x0 + M(16);
        if (g.src > SrcNone) {
          sourceGrip(c, g.src, tx + M(8), cy, M(8));
          tx += M(30);
        }
        text(c, tx, cy - M(8) - T(13) * 0.68f, g.head.c_str(), P.text, T(13), true);
        if (!g.sub.empty()) text(c, tx, cy + M(9) - T(11.5f) * 0.68f, g.sub.c_str(), P.textFaint, T(11.5f), false);
        if (g.src >= SrcLfo1 && g.src <= SrcLfo10) {
          const float mx = x0 + lw - M(16) - M(120);
          wellLook(c, mx, cy - M(17), M(120), M(34));
          drawCurve(c, lfoShapePublic(c, g.src - SrcLfo1), mx + M(4), cy - M(13), M(112), M(26), P.fam[FamLfo], false);
        }
        ry += M(58);
      }
      for (int s : g.slots) {
        const float rh = M(56), rc = ry + rh * 0.5f;
        hline(c, x0, x0 + lw, ry + 0.5f, P.line);
        ImGui::PushID(s + 4000);
        const int src = vali(c, modP(s, M_Source)), d = vali(c, modP(s, M_Dest));
        const bool isUsed = src > SrcNone || d > 0, byp = val(c, modP(s, M_Bypass)) >= 0.5, sel = u.linkSel == s;
        const ImU32 sc = src > SrcNone ? P.fam[familyOf(src)] : P.textFaint;
        ImGui::SetCursorScreenPos(c.p(x0, ry));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", c.F.sz(lw, rh))) u.linkSel = s;
        const bool hv = ImGui::IsItemHovered();
        if (sel || hv) c.dl->AddRectFilled(c.p(x0 + 1, ry + 1), c.p(x0 + lw - 1, ry + rh), sel ? theme::withAlpha(sc, 0.12f) : P.panelHi);
        if (sel) c.dl->AddRectFilled(c.p(x0, ry), c.p(x0 + M(3), ry + rh), sc);
        {
          char num[16];
          std::snprintf(num, sizeof num, "%02d", s + 1);
          MonoText mono;
          text(c, x0 + M(16), rc - T(10.5f) * 0.68f, num, P.textFaint, T(10.5f), false);
        }
        const auto ti = d > 0 ? targetInfo(c, d - 1) : std::pair<std::string, ImU32>{"", P.textFaint};
        const std::string tn = d > 0 ? paramLongName(d - 1) : (isUsed ? "Choose a target" : "Empty");
        const std::string sn = u.linkGroup == 0 ? ti.first : (src > SrcNone ? "from " + sourceShown(c, src) : "");
        const float nx = x0 + M(56);
        c.dl->AddRectFilled(c.p(nx, rc - M(8) - M(3.5f)), c.p(nx + M(7), rc - M(8) + M(3.5f)), u.linkGroup == 0 ? ti.second : sc, c.s(M(1.5f)));
        text(c, nx + M(16), rc - M(8) - T(12.5f) * 0.68f, tn.c_str(), isUsed ? P.text : P.textFaint, T(12.5f), false);
        if (!sn.empty()) text(c, nx + M(16), rc + M(9) - T(11) * 0.68f, sn.c_str(), P.textFaint, T(11), false);
        if (isUsed) {
          // the amount: drag the bar
          const float bx = x0 + M(432), bw = M(260);
          ImGui::SetCursorScreenPos(c.p(bx, rc - M(9)));
          ImGui::InvisibleButton("##amt", c.F.sz(bw, M(18)));
          if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0)
            setv(c, modP(s, M_Amount), std::clamp(val(c, modP(s, M_Amount)) + ImGui::GetIO().MouseDelta.x / c.s(bw) * 2.0, -1.0, 1.0));
          if (ImGui::IsItemHovered()) c.a.setHint("Drag: how far it moves " + tn);
          reach(modP(s, M_Amount));
          const float a = std::clamp(valf(c, modP(s, M_Amount)), -1.f, 1.f), mid = bx + bw * 0.5f, ax = mid + a * bw * 0.5f;
          c.dl->AddRectFilled(c.p(bx, rc - M(3)), c.p(bx + bw, rc + M(3)), P.field, c.s(M(3)));
          c.dl->AddRectFilled(c.p(std::min(mid, ax), rc - M(3)), c.p(std::max(mid, ax), rc + M(3)), byp ? P.textFaint : sc, c.s(M(3)));
          c.dl->AddCircleFilled(c.p(ax, rc), c.s(M(6)), byp ? P.textFaint : sc, 16);
          c.dl->AddCircleFilled(c.p(ax, rc), c.s(M(3)), P.panel, 12);
          char b[16];
          std::snprintf(b, sizeof b, "%+d%%", static_cast<int>(std::lround(a * 100)));
          {
            MonoText mono;
            textR(c, x0 + M(790), rc - T(11) * 0.68f, b, P.text, T(11), false);
          }
          // the response, the switch, remove
          const float cx = x0 + M(800);
          ImGui::SetCursorScreenPos(c.p(cx, rc - M(14)));
          if (ImGui::InvisibleButton("##crv", c.F.sz(M(40), M(28)))) u.linkSel = s, ImGui::OpenPopup("##crvpop");
          if (ImGui::IsItemHovered()) c.a.setHint("Response: click to bend it");
          wellLook(c, cx, rc - M(14), M(40), M(28));
          curveGlyph(c, cx + M(2), rc - M(12), M(36), M(24), val(c, modP(s, M_Curve)), &c.m.modCurve[static_cast<size_t>(s)], sc);
          if (ImGui::BeginPopup("##crvpop")) {
            curvePopup(c, s, false);
            ImGui::EndPopup();
          }
          reach(modP(s, M_Bypass));
          if (toggleLook(c, "byp", x0 + M(852), rc - M(9), !byp, P.hot)) setv(c, modP(s, M_Bypass), byp ? 0 : 1);
          if (ImGui::IsItemHovered()) c.a.setHint(byp ? "Off (click: on)" : "On (click: off)");
          ImGui::SetCursorScreenPos(c.p(x0 + M(906), rc - M(10)));
          if (ImGui::InvisibleButton("##del", c.F.sz(M(20), M(20)))) clearSlot(c, s);
          const bool dh = ImGui::IsItemHovered();
          if (dh) c.a.setHint("Remove this link");
          iconClose(c.dl, c.p(x0 + M(916), rc), c.s(M(8)), dh ? P.danger : P.textFaint);
        }
        ImGui::PopID();
        ry += rh;
      }
      y += gh + M(12);
    }
    c.dl->PopClipRect();
    ImGui::PopClipRect();
    if (total > viewH) {
      const float th = std::max(M(24), viewH * viewH / total), ty = lt + (viewH - th) * u.mxScroll / (total - viewH);
      c.dl->AddRectFilled(c.p(x0 + lw + M(3), ty), c.p(x0 + lw + M(6), ty + th), P.lineHi, c.s(M(1.5f)));
    }
  }
  if (!ready.empty()) {  // Ready to link
    c.dl->AddRectFilled(c.p(x0, readyY), c.p(x0 + lw, readyY + readyH), barBg(c), c.s(M(6)));
    c.dl->AddRect(c.p(x0, readyY), c.p(x0 + lw, readyY + readyH), P.line, c.s(M(6)));
    text(c, x0 + M(16), readyY + M(23) - T(13) * 0.68f, "Ready to link", P.text, T(13), true);
    textR(c, x0 + lw - M(16), readyY + M(23) - T(11) * 0.68f, "movement with no links yet", P.textFaint, T(11), false);
    for (size_t i = 0; i < ready.size(); ++i) {
      const int src = ready[i];
      const float cx = chipAt[i].first.x, cy = readyY + M(44) + chipAt[i].first.y, w = chipAt[i].second, h = M(30);
      ImGui::PushID(src + 4600);
      ImGui::SetCursorScreenPos(c.p(cx, cy));
      if (ImGui::InvisibleButton("##ready", c.F.sz(w, h)))
        for (int s = 0; s < kModSlots; ++s)
          if (vali(c, modP(s, M_Source)) <= SrcNone && vali(c, modP(s, M_Dest)) <= 0) {
            setv(c, modP(s, M_Source), src);
            u.linkSel = s;
            break;
          }
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("PLASMA_SRC", &src, sizeof(int));
        ImGui::TextUnformatted(sourceShown(c, src).c_str());
        ImGui::EndDragDropSource();
      }
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.a.setHint("Drag onto any control to link it, or click to start a link at the right");
      layoutBox(c, cx, cy, w, h, "ready");
      c.dl->AddRectFilled(c.p(cx, cy), c.p(cx + w, cy + h), hv ? P.panelHi : P.panel, c.s(h * 0.5f));
      c.dl->AddRect(c.p(cx, cy), c.p(cx + w, cy + h), P.line, c.s(h * 0.5f));
      glowDot(c, c.p(cx + M(14) + M(3.5f), cy + h * 0.5f), M(3.5f), P.fam[familyOf(src)]);
      const std::string nm = sourceShown(c, src), sb = sourceSub(c, src);
      const float tx = cx + M(14 + 7 + 8);
      text(c, tx, cy + h * 0.5f - T(12) * 0.68f, nm.c_str(), P.text, T(12), false);
      text(c, tx + textW(c, nm.c_str(), T(12), false) + M(8), cy + h * 0.5f - T(11) * 0.68f, sb.c_str(), P.textFaint, T(11), false);
      ImGui::PopID();
    }
  }
  {  // how to make a link
    c.dl->AddRectFilled(c.p(x0, helpY), c.p(x0 + lw, bottom), barBg(c), c.s(M(6)));
    c.dl->AddRect(c.p(x0, helpY), c.p(x0 + lw, bottom), P.line, c.s(M(6)));
    const ImU32 lc = P.fam[FamLfo];
    const ImVec2 a = c.p(x0 + M(36), helpY + M(48)), b = c.p(x0 + M(146), helpY + M(56));
    c.dl->AddCircle(a, c.s(M(8)), lc, 16, std::max(1.f, c.s(M(1.5f))));
    c.dl->AddCircleFilled(a, c.s(M(3.5f)), lc, 12);
    for (int i = 0; i < 12; ++i) {
      const float t0 = i / 12.f, t1 = (i + 0.5f) / 12.f;
      auto bez = [&](float t) {
        const ImVec2 p0 = ImVec2(a.x + c.s(M(12)), a.y), p1 = ImVec2(a.x + c.s(M(50)), a.y - c.s(M(40))), p2 = ImVec2(b.x - c.s(M(14)), b.y - c.s(M(26)));
        const float u2 = 1 - t;
        return ImVec2(u2 * u2 * p0.x + 2 * u2 * t * p1.x + t * t * p2.x, u2 * u2 * p0.y + 2 * u2 * t * p1.y + t * t * p2.y);
      };
      c.dl->AddLine(bez(t0), bez(t1), theme::withAlpha(lc, 0.8f), std::max(1.f, c.s(M(1.3f))));
    }
    c.dl->AddCircleFilled(b, c.s(M(14)), P.knob, 24);
    c.dl->AddCircle(b, c.s(M(14)), IM_COL32(0, 0, 0, 120), 24, std::max(1.f, c.s(M(1))));
    c.dl->AddLine(b, ImVec2(b.x - c.s(M(6)), b.y + c.s(M(6))), P.panel, std::max(1.5f, c.s(M(2))));
    const float tx = x0 + M(188);
    text(c, tx, helpY + M(28) - T(13) * 0.68f, "Make a link", P.text, T(13), true);
    const char* d =
        "Drag the dot on any movement block onto a knob. The knob gets a coloured ring that shows how far it will move. Or press New link and pick both ends here.";
    ImGui::PushClipRect(c.p(tx, helpY), c.p(x0 + lw - M(16), bottom), true);
    const ImVec2 at = c.p(tx, helpY + M(42));
    c.dl->AddText(ImGui::GetFont(), c.s(T(12)) * kTypeScale, at, P.label, i18n::tr(d), nullptr, c.s(x0 + lw - M(32) - tx));
    layoutWindowText(at, "", 0);
    ImGui::PopClipRect();
  }
  // the selected link
  {
    c.dl->AddRectFilled(c.p(px, top), c.p(px + pw, bottom), barBg(c), c.s(M(6)));
    c.dl->AddRect(c.p(px, top), c.p(px + pw, bottom), P.line, c.s(M(6)));
    const float ix = px + M(18), iw = pw - M(36);
    float y = top + M(26);
    text(c, ix, y - T(13) * 0.68f, "Selected link", P.text, T(13), true);
    const int s = u.linkSel;
    if (s < 0 || s >= kModSlots) {
      text(c, ix, y + M(30), "Click a link at the left to change it here.", P.textFaint, T(11.5f), false);
      return;
    }
    {
      char b[16];
      std::snprintf(b, sizeof b, "slot %02d", s + 1);
      MonoText mono;
      textR(c, ix + iw, y - T(11) * 0.68f, b, P.textFaint, T(11), false);
    }
    ImGui::PushID(s + 6000);
    const int src = vali(c, modP(s, M_Source)), d = vali(c, modP(s, M_Dest));
    const ImU32 sc = src > SrcNone ? P.fam[familyOf(src)] : P.textFaint;
    reach(modP(s, M_Source)), reach(modP(s, M_Dest));
    y += M(22);
    const float sw = std::min(M(130), iw * 0.32f);
    endChip(c, "src", ix, y, sw, src > SrcNone ? sourceShown(c, src) : std::string("Choose"), sc, true, s);
    {
      const float ax = ix + sw + M(14), ay = y + M(14);
      c.dl->AddLine(c.p(ax, ay), c.p(ax + M(22), ay), P.textFaint, std::max(1.f, c.s(M(1.2f))));
      c.dl->AddLine(c.p(ax + M(22), ay), c.p(ax + M(18), ay - M(3)), P.textFaint, std::max(1.f, c.s(M(1.2f))));
      c.dl->AddLine(c.p(ax + M(22), ay), c.p(ax + M(18), ay + M(3)), P.textFaint, std::max(1.f, c.s(M(1.2f))));
    }
    const float dx = ix + sw + M(50);
    endChip(c, "dst", dx, y, ix + iw - dx, d > 0 ? paramLongName(d - 1) : std::string("Choose a target"), d > 0 ? targetInfo(c, d - 1).second : P.textFaint, false, s);
    y += M(28) + M(14);
    // the response: a curve to bend (straight by default)
    const float gh = M(200);
    {
      Curve& cv = c.m.modCurve[static_cast<size_t>(s)];
      wellLook(c, ix, y, iw, gh);
      {
        MonoText mono;
        capsText(c, ix + M(10), y + M(10), "Response: drag the points", P.textFaint, T(10), M(0.7f));
      }
      Curve edit = cv;
      if (edit.empty()) {
        const double bnd = val(c, modP(s, M_Curve));
        for (int i = 0; i <= 4; ++i) edit.push_back({i / 4.f, static_cast<float>(bendCurve(i / 4.0, bnd)), 0});
      }
      static int sel = -1;
      if (curveEditor(c, "modcv", edit, ix + M(8), y + M(28), iw - M(16), gh - M(36), 4, 4, sel, true, sc)) {
        cv = edit;
        commit(c, "Edit link curve", 2000 + s);
      }
      y += gh + M(18);
    }
    auto row = [&](const char* label, float h) {
      text(c, ix, y + h * 0.5f - T(12) * 0.68f, label, P.label, T(12), false);
    };
    {  // how far
      row("How far it moves", M(28));
      bar(c, modP(s, M_Amount), ix + iw * 0.42f, y, iw * 0.58f, M(28), "", BarCentre);
      y += M(28) + M(14);
    }
    {  // response presets
      text(c, ix, y, "Response", P.label, T(12), false);
      y += M(22);
      struct Rp {
        const char* name;
        int kind;
      };
      static const Rp rp[5] = {{"Straight", 0}, {"Ease in", 1}, {"Ease out", 2}, {"S curve", 3}, {"Steps", 4}};
      Curve& cv = c.m.modCurve[static_cast<size_t>(s)];
      reach(modP(s, M_Curve));
      const double bnd = val(c, modP(s, M_Curve));
      int cur = -1;
      if (cv.empty()) cur = std::fabs(bnd) < 0.01 ? 0 : bnd > 0 ? 1 : 2;
      else if (cv.size() == 5 && std::fabs(cv[1].y - 0.1f) < 0.01f) cur = 3;
      else if (cv.size() == 8) cur = 4;
      const float bw = (iw - M(6) * 4) / 5, bh = M(44);
      for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i + 6100);
        const float bx = ix + i * (bw + M(6));
        if (btnLook(c, "rp", bx, y, bw, bh, "", false, cur == i, T(10.5f))) {
          cv.clear();
          if (i == 3) cv = {{0, 0, 0}, {0.25f, 0.1f, 0}, {0.5f, 0.5f, 0}, {0.75f, 0.9f, 0}, {1, 1, 0}};
          if (i == 4)
            for (int k = 0; k < 4; ++k) cv.push_back({k / 4.f, k / 3.f, 0}), cv.push_back({(k + 1) / 4.f - 0.001f, k / 3.f, 0});
          commitParams(c, "Link response", {{modP(s, M_Curve), i == 1 ? 0.5 : i == 2 ? -0.5 : 0.0}});
        }
        Curve g;
        if (i == 3) g = {{0, 0, 0}, {0.25f, 0.1f, 0}, {0.5f, 0.5f, 0}, {0.75f, 0.9f, 0}, {1, 1, 0}};
        const double gb = i == 1 ? 0.5 : i == 2 ? -0.5 : 0.0;
        if (i == 4) {
          std::vector<ImVec2> pts;
          for (int k = 0; k < 4; ++k) {
            const float yy = y + M(22) - M(12) * k / 3.f;
            pts.push_back(c.p(bx + bw * 0.3f + bw * 0.1f * k, yy)), pts.push_back(c.p(bx + bw * 0.3f + bw * 0.1f * (k + 1), yy));
          }
          c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), cur == i ? sc : P.label, 0, std::max(1.f, c.s(M(1.3f))));
        } else {
          curveGlyph(c, bx + bw * 0.3f, y + M(6), bw * 0.4f, M(20), gb, g.empty() ? nullptr : &g, cur == i ? sc : P.label);
        }
        textMid(c, bx, bx + bw, y + bh - M(10), rp[i].name, cur == i ? P.text : P.label, T(10.5f), false);
        ImGui::PopID();
      }
      y += bh + M(16);
    }
    {  // centred
      reach(modP(s, M_Bipolar));
      const bool cen = val(c, modP(s, M_Bipolar)) >= 0.5;
      text(c, ix, y, "Centred on the knob", P.label, T(12), false);
      text(c, ix, y + M(17), "moves up and down from where the knob sits", P.textFaint, T(10.5f), false);
      if (toggleLook(c, "cen", ix + iw - M(32), y + M(4), cen, sc)) setv(c, modP(s, M_Bipolar), cen ? 0 : 1);
      y += M(44);
    }
    {  // scaled by
      row("Scaled by", M(28));
      sourceField(c, modP(s, M_Aux), ix + iw * 0.42f, y, iw * 0.58f, M(28), s);
      y += M(28) + M(10);
      if (vali(c, modP(s, M_Aux)) > SrcNone) {
        chip(c, modP(s, M_AuxInv), ix + iw * 0.42f, y, iw * 0.28f, M(26), "Inverted");
        bar(c, modP(s, M_AuxCurve), ix + iw * 0.72f, y, iw * 0.28f, M(26), "Bend", BarCentre);
        y += M(26) + M(10);
      }
    }
    {  // rise and fall, trim
      row("Rise and fall", M(28));
      const float fw = (iw * 0.58f - M(6)) / 2;
      bar(c, modP(s, M_Rise), ix + iw * 0.42f, y, fw, M(28), "Rise", BarField);
      bar(c, modP(s, M_Fall), ix + iw * 0.42f + fw + M(6), y, fw, M(28), "Fall", BarField);
      y += M(28) + M(10);
      row("Trim", M(28));
      bar(c, modP(s, M_Out), ix + iw * 0.42f, y, iw * 0.58f, M(28), "", 0);
      y += M(28) + M(10);
    }
    {  // turn off, remove
      const float by = bottom - M(18) - M(28), bw = (iw - M(8)) / 2;
      const bool byp = val(c, modP(s, M_Bypass)) >= 0.5;
      reach(modP(s, M_Bypass));
      if (btnLook(c, "off", ix, by, bw, M(28), byp ? "Turn on" : "Turn off", false, false, T(12))) setv(c, modP(s, M_Bypass), byp ? 0 : 1);
      if (btnLook(c, "remove", ix + bw + M(8), by, bw, M(28), "", false, false, T(12))) clearSlot(c, s);
      textMid(c, ix + bw + M(8), ix + iw, by + M(14), "Remove link", P.danger, T(12), false);
    }
    ImGui::PopID();
  }
}

// ================================================================== Setup
namespace {
// A choice as the design draws it: a dark field, the value, a chevron; the choices in a menu.
void selectLook(Ctx& c, int k, float x, float y, float w, float h) {
  const ParamDesc* d = pdesc(k);
  if (!d) return;
  reach(k);
  ImGui::PushID(k + 91000);
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##sel", c.F.sz(w, h))) ImGui::OpenPopup("##selm");
  const bool hv = ImGui::IsItemHovered();
  const std::string v = ptext(c, k);
  if (hv) c.a.setHint(paramLongName(k) + ": " + v);
  layoutBox(c, x, y, w, h, v.c_str());
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), hv ? c.P.lineHi : c.P.line, c.s(M(4)));
  std::string shown = v;
  while (shown.size() > 3 && textW(c, shown.c_str(), T(12), false) > w - M(36)) shown.pop_back();
  text(c, x + M(10), y + h * 0.5f - T(12) * 0.68f, shown.c_str(), c.P.text, T(12), false);
  const ImVec2 m = c.p(x + w - M(14), y + h * 0.5f);
  const float a = c.s(M(3.5f));
  c.dl->AddLine(ImVec2(m.x - a, m.y - a * 0.5f), ImVec2(m.x, m.y + a * 0.5f), c.P.label, std::max(1.f, c.s(M(1.2f))));
  c.dl->AddLine(ImVec2(m.x, m.y + a * 0.5f), ImVec2(m.x + a, m.y - a * 0.5f), c.P.label, std::max(1.f, c.s(M(1.2f))));
  if (ImGui::BeginPopup("##selm")) {
    const std::vector<std::string> ch = d->kind == ParamKind::Toggle ? std::vector<std::string>{"Off", "On"} : d->choices;
    const int cur = vali(c, k);
    for (int i = 0; i < static_cast<int>(ch.size()); ++i)
      if (ImGui::MenuItem(ch[static_cast<size_t>(i)].c_str(), nullptr, i == cur)) setv(c, k, i);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

// A Setup card: its glyph in a small square, its title; returns the content top.
float setupCard(Ctx& c, float x, float y, float w, float h, const char* title, int glyph) {
  const Pal& P = c.P;
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), barBg(c), c.s(M(6)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), P.line, c.s(M(6)));
  const float gx = x + M(18), gy = y + M(17), gs = M(26);
  c.dl->AddRectFilled(c.p(gx, gy), c.p(gx + gs, gy + gs), P.panel, c.s(M(4)));
  c.dl->AddRect(c.p(gx, gy), c.p(gx + gs, gy + gs), P.line, c.s(M(4)));
  const ImVec2 m = c.p(gx + gs * 0.5f, gy + gs * 0.5f);
  const float r = c.s(M(6)), th = std::max(1.f, c.s(M(1.3f)));
  const ImU32 col = P.label;
  switch (glyph) {
    case 0: {  // a wave
      ImVec2 pts[13];
      for (int i = 0; i <= 12; ++i) pts[i] = ImVec2(m.x - r + 2 * r * i / 12.f, m.y - r * 0.6f * std::sin(i / 12.f * 6.2832f));
      c.dl->AddPolyline(pts, 13, col, 0, th);
      break;
    }
    case 1:  // a page
      c.dl->AddRect(ImVec2(m.x - r * 0.75f, m.y - r), ImVec2(m.x + r * 0.75f, m.y + r), col, c.s(M(1.5f)), 0, th);
      for (int i = 0; i < 3; ++i) c.dl->AddLine(ImVec2(m.x - r * 0.4f, m.y - r * 0.45f + i * r * 0.45f), ImVec2(m.x + r * 0.4f, m.y - r * 0.45f + i * r * 0.45f), col, th);
      break;
    case 2:  // a clock
      c.dl->AddCircle(m, r, col, 18, th);
      c.dl->AddLine(m, ImVec2(m.x, m.y - r * 0.6f), col, th), c.dl->AddLine(m, ImVec2(m.x + r * 0.45f, m.y), col, th);
      break;
    case 3:  // a screen
      c.dl->AddRect(ImVec2(m.x - r, m.y - r * 0.7f), ImVec2(m.x + r, m.y + r * 0.45f), col, c.s(M(1.5f)), 0, th);
      c.dl->AddLine(ImVec2(m.x - r * 0.4f, m.y + r * 0.85f), ImVec2(m.x + r * 0.4f, m.y + r * 0.85f), col, th);
      break;
    case 4:  // a tuning fork
      c.dl->AddLine(ImVec2(m.x, m.y + r), ImVec2(m.x, m.y - r), col, th);
      c.dl->AddLine(ImVec2(m.x - r * 0.5f, m.y - r * 0.5f), ImVec2(m.x, m.y - r), col, th);
      c.dl->AddLine(ImVec2(m.x + r * 0.5f, m.y - r * 0.5f), ImVec2(m.x, m.y - r), col, th);
      break;
    case 5:  // notes
      c.dl->AddCircleFilled(ImVec2(m.x - r * 0.5f, m.y + r * 0.6f), r * 0.32f, col, 10);
      c.dl->AddCircleFilled(ImVec2(m.x + r * 0.55f, m.y + r * 0.4f), r * 0.32f, col, 10);
      c.dl->AddLine(ImVec2(m.x - r * 0.2f, m.y + r * 0.6f), ImVec2(m.x - r * 0.2f, m.y - r * 0.7f), col, th);
      c.dl->AddLine(ImVec2(m.x + r * 0.85f, m.y + r * 0.4f), ImVec2(m.x + r * 0.85f, m.y - r * 0.9f), col, th);
      c.dl->AddLine(ImVec2(m.x - r * 0.2f, m.y - r * 0.7f), ImVec2(m.x + r * 0.85f, m.y - r * 0.9f), col, th);
      break;
    default:  // an arrow in
      c.dl->AddLine(ImVec2(m.x - r, m.y), ImVec2(m.x + r, m.y), col, th);
      c.dl->AddLine(ImVec2(m.x + r, m.y), ImVec2(m.x + r * 0.4f, m.y - r * 0.6f), col, th);
      c.dl->AddLine(ImVec2(m.x + r, m.y), ImVec2(m.x + r * 0.4f, m.y + r * 0.6f), col, th);
      break;
  }
  text(c, gx + gs + M(10), gy + gs * 0.5f - T(13.5f) * 0.68f, title, P.text, T(13.5f), true);
  return y + M(58);
}
// A setting row: its name (and a line under it), the control at the right is the caller's.
void setupRow(const Ctx& c, float x, float y, float h, const char* name, const char* sub) {
  if (sub && *sub) {
    text(c, x, y + h * 0.5f - M(8) - T(12) * 0.68f, name, c.P.label, T(12), false);
    text(c, x, y + h * 0.5f + M(8) - T(10.5f) * 0.68f, sub, c.P.textFaint, T(10.5f), false);
  } else {
    text(c, x, y + h * 0.5f - T(12) * 0.68f, name, c.P.label, T(12), false);
  }
}
// Wrapped text inside a width.
void wrapped(const Ctx& c, float x, float y, float w, const char* s, ImU32 col, float px) {
  const ImVec2 at = c.p(x, y);
  c.dl->AddText(ImGui::GetFont(), c.s(px) * kTypeScale, at, col, i18n::tr(s), nullptr, c.s(w));
  layoutWindowText(at, "", 0);
}
}  // namespace

void setupView(Ctx& c) {
  const Pal& P = c.P;
  panelRect(c, 0, kTopH, kW, kPlayY - kTopH, P.frame);
  ImGui::PushID("setup");
  const float x0 = M(20), gap = M(12), cw = (kW - M(40) - gap * 3) / 4, top = kTopH + M(16), bottom = kPlayY - M(16);
  const float row1H = M(214), row2Y = top + row1H + gap, row2H = M(214), col = cw - M(36);
  const float c1 = x0, c2 = x0 + cw + gap, c3 = x0 + (cw + gap) * 2, c4 = x0 + (cw + gap) * 3;
  {  // Sound quality
    float y = setupCard(c, c1, top, cw, row1H, "Sound quality", 0);
    const float ix = c1 + M(18);
    wrapped(c, ix, y - M(4), col, "Higher quality is cleaner on bright sounds and costs more processor time.", P.textFaint, T(11.5f));
    y += M(36);
    reach(G_Quality);
    const int hit = segPick(c, "q", ix, y, M(168), M(28), {"Normal", "High", "Best"}, vali(c, G_Quality), false, T(11.5f));
    if (hit >= 0) setv(c, G_Quality, hit);
    y += M(28) + M(10);
    setupRow(c, ix, y, M(36), "Instant changes", "skip the short glide when a knob moves");
    reach(G_NoSmooth);
    const bool on = val(c, G_NoSmooth) >= 0.5;
    if (toggleLook(c, "ns", ix + col - M(32), y + M(9), on, P.hot)) setv(c, G_NoSmooth, on ? 0 : 1);
  }
  {  // Tuning
    float y = setupCard(c, c1, row2Y, cw, row2H, "Tuning", 4);
    const float ix = c1 + M(18);
    reach(G_ConcertA);
    char b[16];
    std::snprintf(b, sizeof b, "%.0f", val(c, G_ConcertA));
    float bw;
    {
      MonoText mono;
      bw = textW(c, b, T(24), false);
      text(c, ix, y + M(10) - T(24) * 0.68f, b, ImGui::IsMouseHoveringRect(c.p(ix, y - M(6)), c.p(ix + bw, y + M(26))) ? P.accentText : P.text, T(24), false);
      text(c, ix + bw + M(6), y + M(14) - T(10.5f) * 0.68f, "Hz", P.textFaint, T(10.5f), false);
    }
    text(c, ix + bw + M(32), y + M(14) - T(11) * 0.68f, "pitch of the A above middle C", P.textFaint, T(11), false);
    ImGui::SetCursorScreenPos(c.p(ix, y - M(6)));
    ImGui::InvisibleButton("##concert", c.F.sz(bw + M(24), M(32)));
    if (ImGui::IsItemHovered()) c.a.setHint("Concert A: drag up or down (double-click: 440)"), ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0) setv(c, G_ConcertA, std::clamp(val(c, G_ConcertA) - ImGui::GetIO().MouseDelta.y * 0.25, 400.0, 480.0));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) setv(c, G_ConcertA, 440);
    y += M(42);
    setupRow(c, ix, y, M(28), "Scale", "");
    const std::string tn = c.m.tuningName.empty() ? std::string("Equal temperament") : c.m.tuningName;
    dropField(c, "tun", ix + col * 0.45f, y, col * 0.55f, M(28), tn.c_str(), [&] {
      if (ImGui::MenuItem(i18n::tr("Load tuning (.tun)...")))
        openFile(c, i18n::tr("Load tuning"), {".tun"}, [](App& a, const PluginTarget& t, const std::string& p) {
          std::ifstream f(p, std::ios::binary);
          std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
          if (txt.size() > (1u << 20)) return;
          editState(a, t, "Load tuning", [&](PluginState&, Model& m) {
            m.tuning = txt;
            m.tuningName = fs_stem(p);
          });
        });
      if (ImGui::MenuItem(i18n::tr("Equal temperament (clear)"), nullptr, false, !c.m.tuning.empty())) {
        c.m.tuning.clear();
        c.m.tuningName.clear();
        commit(c, "Clear tuning");
      }
    });
  }
  {  // About this sound
    const float h = row1H + gap + row2H;
    float y = setupCard(c, c2, top, cw, h, "About this sound", 1);
    const float ix = c2 + M(18);
    auto fieldIn = [&](const char* id, float yy, float fh, std::string& s, const char* label, const char* hint, int flags) {
      text(c, ix, yy - M(4), label, P.textFaint, T(11), false);
      char buf[512];
      std::snprintf(buf, sizeof buf, "%s", s.c_str());
      if (inputField(c, id, ix, yy + M(12), col, fh, buf, sizeof buf, hint, flags, T(12))) {
        s = buf;
        commit(c, label, 900 + static_cast<int>(id[0]));
      }
    };
    y -= M(10);
    fieldIn("n", y, M(30), c.m.presetName, "Name", "Name", 0);
    fieldIn("a", y + M(65), M(30), c.m.author, "Made by", "Your name", 0);
    fieldIn("d", y + M(130), M(30), c.m.description, "Notes", "How to play it, which macros matter, anything worth knowing", 0);
    int links = 0, fx = 0, layers = 0;
    for (int s = 0; s < kModSlots; ++s) links += vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0;
    for (int s = 0; s < kFxSlots; ++s) fx += vali(c, fxP(s, X_Type)) > FxNone;
    for (int o = 0; o < kOscs; ++o) layers += val(c, oscP(o, O_Enable)) >= 0.5;
    char b1[32], b2[32], b3[32];
    std::snprintf(b1, sizeof b1, "%d of %d links", links, kModSlots);
    std::snprintf(b2, sizeof b2, fx == 1 ? "%d effect" : "%d effects", fx);
    std::snprintf(b3, sizeof b3, layers == 1 ? "%d layer" : "%d layers", layers);
    float px = ix;
    const float py = top + h - M(18) - M(24);
    for (const char* b : {b1, b2, b3}) {
      const float w = textW(c, b, T(11), false) + M(18);
      c.dl->AddRect(c.p(px, py), c.p(px + w, py + M(24)), P.line, c.s(M(12)));
      textMid(c, px, px + w, py + M(12), b, P.label, T(11), false);
      px += w + M(8);
    }
    if (!c.m.openFormat.empty() && py - M(30) > y + M(200))
      text(c, ix, py - M(26), "Brought in from another file: its notes are in the top bar.", P.textFaint, T(11), false);
  }
  {  // Timing
    float y = setupCard(c, c3, top, cw, row1H, "Timing", 2);
    const float ix = c3 + M(18);
    setupRow(c, ix, y, M(28), "Envelope times", "");
    bar(c, G_EnvScale, ix + col * 0.45f, y, col * 0.55f, M(28), "", 0);
    y += M(44);
    setupRow(c, ix, y, M(28), "LFO rates", "");
    bar(c, G_LfoScale, ix + col * 0.45f, y, col * 0.55f, M(28), "", 0);
    y += M(44);
    setupRow(c, ix, y, M(28), "Scale rates, not times", "");
    reach(G_LfoScaleRate);
    const bool on = val(c, G_LfoScaleRate) >= 0.5;
    if (toggleLook(c, "sr", ix + col - M(32), y + M(5), on, P.hot)) setv(c, G_LfoScaleRate, on ? 0 : 1);
  }
  {  // Send notes out
    float y = setupCard(c, c3, row2Y, cw, row2H, "Send notes out", 5);
    const float ix = c3 + M(18);
    setupRow(c, ix, y, M(28), "Send to other plugins", "");
    selectLook(c, G_MidiOut, ix + col * 0.55f, y, col * 0.45f, M(28));
    y += M(46);
    setupRow(c, ix, y, M(36), "Hold notes", "keep notes on until the next one");
    reach(G_NoteLatch);
    const bool on = val(c, G_NoteLatch) >= 0.5;
    if (toggleLook(c, "hold", ix + col - M(32), y + M(9), on, P.hot)) setv(c, G_NoteLatch, on ? 0 : 1);
  }
  {  // Editor
    const float h = row1H + gap + row2H;
    float y = setupCard(c, c4, top, cw, h, "Editor", 3);
    const float ix = c4 + M(18);
    wrapped(c, ix, y - M(4), col, "How this window looks and feels. These follow you to every sound.", P.textFaint, T(11.5f));
    y += M(40);
    setupRow(c, ix, y, M(36), "Value bubbles", "show the value while you drag");
    if (toggleLook(c, "bub", ix + col - M(32), y + M(9), c.u.prefBubbles, P.hot)) c.u.prefBubbles = !c.u.prefBubbles;
    y += M(46);
    setupRow(c, ix, y, M(36), "Live motion", "animate the pictures while it plays");
    if (toggleLook(c, "live", ix + col - M(32), y + M(9), c.u.prefLive, P.hot)) c.u.prefLive = !c.u.prefLive;
    y += M(46);
    setupRow(c, ix, y, M(36), "Detail", "Simple folds the extra controls");
    {
      MonoText mono;
      const int hit = segPick(c, "detail", ix + col - M(100), y + M(7), M(100), M(22), {"Simple", "Full"}, c.u.full ? 1 : 0, true, T(10.5f));
      if (hit >= 0 && (hit == 1) != c.u.full) c.u.full = hit == 1, c.a.settings.set("ui.plasmaFull", c.u.full);
    }
    y += M(46);
    setupRow(c, ix, y, M(36), "Window size", "use the plugin window's size menu");
  }
  {  // Compatibility
    const float y0 = row2Y + row2H + gap, h = bottom - y0;
    float y = setupCard(c, x0, y0, kW - M(40), h, "Compatibility", 6);
    const float ix = x0 + M(18), iw = kW - M(40) - M(36);
    static const std::pair<int, const char*> laws[12] = {
        {G_S1Mode, "Older morph phase"},   {tailP(T_OscLaw), "Table law"},          {tailP(T_ModLaw), "Link law"},           {tailP(T_EnvLaw), "Envelope curves"},
        {tailP(T_LfoLaw), "LFO law"},      {tailP(T_GranLaw), "Grains law"},        {tailP(T_MsLaw), "Multisample level"},   {tailP(T_NoiseLaw), "Texture pitch law"},
        {tailP(T_DriveMode), "Filter drive"}, {tailP(T_KeyCentre), "Filter key centre"}, {tailP(T_UniMix), "Stack voice pan"}, {tailP(T_BusChain), "Send chaining"}};
    bool allNew = true;
    for (const auto& [k, n] : laws)
      if (const ParamDesc* d = pdesc(k)) allNew = allNew && std::fabs(val(c, k) - d->def) < 1e-6;
    {  // a pill: how this sound is set
      const char* pill = allNew ? "New-sound settings" : "Matches how this sound was made";
      const float tx = x0 + M(18 + 26 + 10) + textW(c, "Compatibility", T(13.5f), true) + M(18), pw = textW(c, pill, T(11), false) + M(30), py = y0 + M(17) + M(1);
      c.dl->AddRectFilled(c.p(tx, py), c.p(tx + pw, py + M(24)), theme::withAlpha(P.good, 0.14f), c.s(M(12)));
      c.dl->AddCircleFilled(c.p(tx + M(12), py + M(12)), c.s(M(2.5f)), P.good, 10);
      text(c, tx + M(20), py + M(12) - T(11) * 0.68f, pill, P.text, T(11), false);
    }
    textR(c, ix + iw, y0 + M(30) - T(11) * 0.68f, "only for imported sounds", P.textFaint, T(11), false);
    text(c, ix, y - M(8), "Sounds brought in from other files can play slightly differently. These settings make them play the way they were made. New sounds leave them as they are.",
         P.textFaint, T(11.5f), false);
    y += M(22);
    const float lgap = M(28), lw = (iw - lgap * 3) / 4, rh = M(48);
    for (int i = 0; i < 12; ++i) {
      const float lx = ix + (i % 4) * (lw + lgap), ly = y + (i / 4) * rh;
      setupRow(c, lx, ly, M(30), laws[i].second, "");
      selectLook(c, laws[i].first, lx + lw - M(112), ly + M(2), M(112), M(26));
      hline(c, lx, lx + lw, ly + rh - M(10), P.line);
    }
    const float fy = y0 + h - M(18) - M(30);
    text(c, ix, fy + M(15) - T(11.5f) * 0.68f, "Changing these alters how this sound plays. Undo brings it back.", P.textFaint, T(11.5f), false);
    const float bw = textW(c, "Use new-sound settings", T(12), false) + M(26);
    if (btnLook(c, "newlaws", ix + iw - bw, fy, bw, M(30), "Use new-sound settings", false, false, T(12)))
      later(c, [](App& a, const PluginTarget& t) {
        editState(a, t, "Use new-sound settings", [](PluginState& st, Model&) {
          for (int k : {static_cast<int>(G_S1Mode), tailP(T_OscLaw), tailP(T_ModLaw), tailP(T_EnvLaw), tailP(T_LfoLaw), tailP(T_GranLaw), tailP(T_MsLaw),
                        tailP(T_NoiseLaw), tailP(T_DriveMode), tailP(T_KeyCentre), tailP(T_UniMix), tailP(T_BusChain)})
            if (const ParamDesc* d = pdesc(k)) st.params[static_cast<size_t>(k)] = d->def;
        });
      });
    if (ImGui::IsItemHovered()) c.a.setHint("Every compatibility setting back to what a new sound uses (one undo step)");
  }
  ImGui::PopID();
}

}  // namespace gl::packs::plasma::ui
