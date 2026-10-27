// Gridline — "seltmann" pack: the sequence lane (J-149). After a drop the
// regions play in order, A B C D ...; the lane shows every step as a block as
// long as it plays, in its region's colour. Drag a block to a new place (a
// caret shows where it lands; Alt+Left / Alt+Right move the selected one);
// right-click a block, a slice in the waveform or a pad to split its region
// (1/2, 1/3, ... : the first piece keeps the letter, later letters move on) or
// to delete it (it joins its neighbour, never past the chopped region). Every
// edit is one undo step through the plugin state.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;

namespace {
const int kSplitParts[7] = {2, 3, 4, 5, 6, 7, 8};
}

// ------------------------------------------------------------------ region edits
void markEdited(Model& m) {
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
}

void splitRegionUi(App& a, const PluginTarget& t, Ed& e, int region, int parts) {
  Model nm = e.m;
  if (!splitRegion(nm, region, parts)) {
    a.notify(i18n::tr("This region is too short to split that many times"), true);
    return;
  }
  char lab[64];
  std::snprintf(lab, sizeof lab, "%s %s 1/%d", i18n::tr("Split"), regionName(region).c_str(), parts);
  commit(a, t, e, nm, lab);
  e.selSlice = region;
  e.selStep = -1;
}

void deleteRegionUi(App& a, const PluginTarget& t, Ed& e, int region) {
  Model nm = e.m;
  if (!mergeRegion(nm, region)) return;
  char lab[64];
  std::snprintf(lab, sizeof lab, "%s %s", i18n::tr("Delete region"), regionName(region).c_str());
  commit(a, t, e, nm, lab);
  e.selSlice = std::clamp(region == 0 ? 0 : region - 1, 0, std::max(0, static_cast<int>(nm.slices.size()) - 1));
  e.selStep = -1;
}

void regionMenuItems(App& a, const PluginTarget& t, Ed& e, int region) {
  const int n = static_cast<int>(e.m.slices.size());
  if (region < 0 || region >= n) return;
  const std::string L = regionName(region);
  char b[96];
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Split"), L.c_str());
  if (ImGui::BeginMenu(b)) {
    for (int parts : kSplitParts) {
      Model probe = e.m;
      const bool ok = splitRegion(probe, region, parts);
      char it[64];
      std::snprintf(it, sizeof it, "1/%d   %s %d %s", parts, i18n::tr("into"), parts, i18n::tr("equal parts"));
      if (ImGui::MenuItem(it, nullptr, false, ok)) splitRegionUi(a, t, e, region, parts);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string tip = L + " " + i18n::tr("keeps the first part; the next parts become") + " ";
        for (int p = 1; p < parts; ++p) tip += regionName(region + p) + (p + 1 < parts ? " " : "");
        tip += std::string(".\n") + i18n::tr("Later letters move on.");
        if (!ok) tip += std::string("\n") + i18n::tr("Too short for that many parts.");
        ui::Tooltip(tip.c_str());
      }
    }
    ImGui::EndMenu();
  }
  if (n > 1) std::snprintf(b, sizeof b, "%s %s   (%s %s)", i18n::tr("Delete"), L.c_str(), i18n::tr("joins"), regionName(region == 0 ? 1 : region - 1).c_str());
  else std::snprintf(b, sizeof b, "%s %s", i18n::tr("Delete"), L.c_str());
  if (ImGui::MenuItem(b, "Del", false, n > 1)) deleteRegionUi(a, t, e, region);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ui::Tooltip(n > 1 ? i18n::tr("The region joins its neighbour (the first one joins the next). The chopped region stays the same.")
                      : i18n::tr("The last region cannot be deleted."));
}

void openRegionMenu(Ed& e, int region, int step) {
  e.menuRegion = region;
  e.menuStep = step;
  e.menuWant = true;
}

void drawRegionMenu(App& a, const PluginTarget& t, Ed& e) {
  if (e.menuWant) {
    ImGui::OpenPopup("##sregion");
    e.menuWant = false;
  }
  if (!ImGui::BeginPopup("##sregion")) return;
  const int n = static_cast<int>(e.m.slices.size());
  const int ns = static_cast<int>(e.m.pattern.steps.size());
  const int region = e.menuRegion;
  if (region < 0 || region >= n) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  {
    char b[96];
    const double secs = static_cast<double>(e.m.slices[static_cast<size_t>(region)].end - e.m.slices[static_cast<size_t>(region)].start) /
                        std::max(1.0, e.m.srcRate);
    if (e.menuStep >= 0 && e.menuStep < ns)
      std::snprintf(b, sizeof b, "%s %d  \xC2\xB7  %s %s  \xC2\xB7  %.3f s", i18n::tr("Step"), e.menuStep + 1, i18n::tr("region"), regionName(region).c_str(), secs);
    else std::snprintf(b, sizeof b, "%s %s  \xC2\xB7  %.3f s", i18n::tr("Region"), regionName(region).c_str(), secs);
    ImGui::TextDisabled("%s", b);
  }
  ImGui::Separator();
  regionMenuItems(a, t, e, region);
  const int st = e.menuStep;
  if (st >= 0 && st < ns) {
    ImGui::Separator();
    Model nm = e.m;
    Step& s = nm.pattern.steps[static_cast<size_t>(st)];
    if (ImGui::MenuItem(i18n::tr("Play backwards"), nullptr, s.reverse)) {
      s.reverse = !s.reverse;
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Reverse a step"));
    }
    if (ImGui::MenuItem(i18n::tr("Duplicate step")) && ns < kMaxSteps) {
      nm.pattern.steps.insert(nm.pattern.steps.begin() + st + 1, s);
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Duplicate a step"));
      e.selStep = st + 1;
    }
    if (ImGui::MenuItem(i18n::tr("Remove from the sequence"), nullptr, false, ns > 1)) {
      nm.pattern.steps.erase(nm.pattern.steps.begin() + st);
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Remove a step"));
      e.selStep = -1;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(i18n::tr("Only this step leaves the sequence; the region stays (use Delete to join regions)."));
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Reset to A B C D order"), nullptr, false, !isNaturalOrder(e.m))) {
    Model nm = e.m;
    nm.preset.clear();
    naturalPattern(nm);
    commit(a, t, e, nm, i18n::N_("Reset the order"));
    e.selStep = -1;
  }
  ImGui::EndPopup();
}

// ------------------------------------------------------------------ the lane
void drawSequence(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Model& m = e.m;
  const int ns = static_cast<int>(m.pattern.steps.size());
  const int nSl = static_cast<int>(m.slices.size());
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double beats = std::max(1e-9, m.pattern.beats());
  const bool natural = isNaturalOrder(m);
  char cap[96];
  {
    const double bars = beats / bpb;
    char bb[24];
    std::snprintf(bb, sizeof bb, std::fabs(bars - std::round(bars)) < 1e-6 ? "%.0f" : "%.2f", bars);
    std::snprintf(cap, sizeof cap, "%s  \xC2\xB7  %d %s  \xC2\xB7  %s %s", natural ? i18n::tr("IN ORDER") : i18n::tr("YOUR ORDER"), ns,
                  ns == 1 ? i18n::tr("STEP") : i18n::tr("STEPS"), bb, std::fabs(bars - 1) < 1e-9 ? i18n::tr("BAR") : i18n::tr("BARS"));
  }
  // J-152: the lane sits right under the waveform and shares its inner width (in order, A plays
  // under A). A 24 px strip between the two carries the title, the caption, the hint and Reset.
  const float stripH = 24;
  {
    const float cy = std::round(p0.y + stripH * 0.5f);
    dl->AddRectFilled(ImVec2(p0.x + 8, cy - 5), ImVec2(p0.x + 10, cy + 4), accent(), 1);
    ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
    const char* title = i18n::tr("SEQUENCE");
    dl->AddText(f, 10.5f, ImVec2(p0.x + 16, cy - 7), P.text, title);
    const float tw = f->CalcTextSizeA(10.5f, 1e9f, 0, title).x;
    dl->AddText(a.fontMono, 10.5f, ImVec2(std::round(p0.x + 16 + tw + 12), cy - 7), P.textFaint, cap);
    const float btnW = 116, btnH = 20;
    const float bx = p1.x - btnW;
    if (button("##sseqreset", ImVec2(bx, cy - btnH * 0.5f), ImVec2(btnW, btnH), i18n::N_("Reset order"), false,
               natural ? i18n::N_("The sequence already plays every region in order")
                       : i18n::N_("Play every region once, in order (A B C D ...), each as long as its region"),
               static_cast<int>(ui::Icon::Loop)) &&
        !natural) {
      Model nm = m;
      nm.preset.clear();
      naturalPattern(nm);
      commit(a, t, e, nm, i18n::N_("Reset the order"));
      e.selStep = -1;
    }
    textR(dl, a.fontRegular, theme::kFontLabel, ImVec2(bx - 12, cy - 7), P.textFaint,
          i18n::tr("Drag a block to move it. Right-click: split, delete"));
  }
  // the well
  const float lx0 = p0.x, lx1 = p1.x, ly0 = p0.y + stripH, rulerH = 13, ly1 = p1.y - rulerH;
  const ImVec2 w0(lx0, ly0), w1(lx1, p1.y);
  pui::displayFrame(dl, w0, w1);
  e.seq0 = ImVec2(lx0, ly0), e.seq1 = ImVec2(lx1, ly1);
  e.seqCentres.clear();
  if (ns == 0) return;
  const float bx0 = lx0 + 8, bx1 = lx1 - 8, by0 = ly0 + 5, by1 = ly1 - 1;
  const float bw = bx1 - bx0;
  auto X = [&](double beat) { return bx0 + static_cast<float>(beat / beats) * bw; };
  // block edges (beats)
  std::vector<float> edge(static_cast<size_t>(ns) + 1);
  {
    double u = 0;
    for (int i = 0; i < ns; ++i) {
      edge[static_cast<size_t>(i)] = X(u);
      u += stepBeats(m.pattern, i);
    }
    edge[static_cast<size_t>(ns)] = bx1;
  }
  // ruler: bars and beats under the blocks
  {
    const float ry = ly1 + 1;
    const float beatPx = bw / static_cast<float>(beats);
    for (int b = 0; b <= static_cast<int>(std::floor(beats + 1e-9)); ++b) {
      const bool bar = b % bpb == 0;
      if (!bar && beatPx < 6) continue;
      const float x = std::round(X(b)) + 0.5f;
      dl->AddLine(ImVec2(x, ry), ImVec2(x, ry + (bar ? 5.0f : 3.0f)), theme::withAlpha(P.textDim, bar ? 0.7f : 0.35f), 1);
      if (bar && b < beats - 1e-9 && bpb * beatPx >= 18) {
        char nb[16];
        std::snprintf(nb, sizeof nb, "%d", b / bpb + 1);
        dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(x + 3, ry + 1), P.textFaint, nb);
      }
    }
  }
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mp = io.MousePos;
  bool laneHovered = ImGui::IsMouseHoveringRect(w0, w1, true) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  // blocks
  for (int i = 0; i < ns; ++i) {
    const Step& s = m.pattern.steps[static_cast<size_t>(i)];
    const float x0 = edge[static_cast<size_t>(i)], x1 = edge[static_cast<size_t>(i) + 1];
    const ImVec2 q0(std::round(x0) + 1, by0), q1(std::max(std::round(x0) + 3, std::round(x1) - 1), by1);
    const int k = stepSlice(m.pattern, s, nSl);
    const bool playing = i == lv.step && lv.patternRunning, sel = i == e.selStep, dragged = i == e.seqDrag;
    ImGui::PushID(0x5E0000 + i);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##blk", ImVec2(q1.x - q0.x, q1.y - q0.y));
    const bool hv = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
      e.seqPress = i;
      e.selStep = i;
      if (k >= 0) {
        e.selSlice = k;
        auditionStart(a, t, e, k);
      }
    }
    if (ImGui::IsItemActive() && e.seqPress == i && e.seqDrag < 0 && ImGui::IsMouseDragging(0, 5.0f) && ns > 1) {
      e.seqDrag = i;
      auditionStop(a, t, e);
    }
    if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
    if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && k >= 0) {
      e.selStep = i;
      e.selSlice = k;
      openRegionMenu(e, k, i);
    }
    if (hv && !ImGui::IsItemActive() && e.seqDrag < 0) {
      char b[200];
      std::snprintf(b, sizeof b, "%s %d: %s %s  \xC2\xB7  %.4g %s\n%s", i18n::tr("Step"), i + 1, i18n::tr("region"), k >= 0 ? regionName(k).c_str() : "-",
                    stepBeats(m.pattern, i), i18n::tr("beats"), i18n::tr("Click to hear it. Drag to move it. Right-click: split or delete the region."));
      ui::Tooltip(b);
    }
    if (hv && e.seqDrag < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();
    // paint
    // J-152: a quiet tinted face with the hue along its foot; only the playing step lights up fully
    if (s.src == kRest || k < 0) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 1.0f), theme::kRadius);
    } else {
      const ImU32 face = playing ? regionColor(k) : regionFace(k, hv ? 0.42f : sel ? 0.36f : 0.28f);
      skin::raisedFace(dl, q0, q1, dragged ? theme::withAlpha(face, 0.3f) : face, theme::kRadius);
      if (!playing && !dragged)
        dl->AddRectFilled(ImVec2(q0.x + 1, q1.y - 3), ImVec2(q1.x - 1, q1.y - 1), regionColor(k, 0.95f), 1.0f, ImDrawFlags_RoundCornersBottom);
      if (s.reverse)
        dl->AddRectFilledMultiColor(q0, q1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.28f), theme::withAlpha(P.ink, 0.28f),
                                    theme::withAlpha(P.ink, 0.0f));
      const ImU32 ink = theme::withAlpha(playing ? theme::contrastText(regionColor(k)) : P.text, dragged ? 0.4f : 1.0f);
      const float cw = q1.x - q0.x;
      const std::string L = regionName(k);
      if (cw >= 11) {
        const float fs = cw >= 26 ? 15.0f : 12.0f;
        textC(dl, a.fontBold, fs, ImVec2((q0.x + q1.x) * 0.5f, q0.y + (q1.y - q0.y) * (cw >= 26 ? 0.42f : 0.5f)), ink, L.c_str());
      }
      if (cw >= 30) {
        std::string marks;
        if (s.reverse) marks += "R ";
        if (s.pitch != 0) {
          char pb[16];
          std::snprintf(pb, sizeof pb, "%+d ", static_cast<int>(std::lround(s.pitch)));
          marks += pb;
        }
        if (s.stutter > 1) marks += "x" + std::to_string(s.stutter) + " ";
        if (!marks.empty()) {
          marks.pop_back();
          textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2((q0.x + q1.x) * 0.5f, q1.y - 8), theme::withAlpha(ink, 0.85f), marks.c_str());
        }
      }
    }
    if (playing && !dragged) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), P.paper, theme::kRadius + 1, 0, 2.0f);
    else if (sel && !dragged) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), accent(), theme::kRadius + 1, 0, 2.0f);
    e.seqCentres.push_back(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f));
  }
  // drag to a new place: the caret at the nearest edge, the block under the pointer
  if (e.seqDrag >= 0 && e.seqDrag < ns) {
    int best = 0;
    float bd = 1e9f;
    for (int j = 0; j <= ns; ++j)
      if (std::fabs(edge[static_cast<size_t>(j)] - mp.x) < bd) bd = std::fabs(edge[static_cast<size_t>(j)] - mp.x), best = j;
    e.seqDrop = best;
    const bool noop = best == e.seqDrag || best == e.seqDrag + 1;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    {  // the block, lifted under the pointer
      const Step& s = m.pattern.steps[static_cast<size_t>(e.seqDrag)];
      const int k = stepSlice(m.pattern, s, nSl);
      const float gw = std::clamp(edge[static_cast<size_t>(e.seqDrag) + 1] - edge[static_cast<size_t>(e.seqDrag)], 22.0f, 80.0f);
      const ImVec2 g0(std::round(mp.x - gw * 0.5f), std::round(by0 - 14)), g1(std::round(mp.x + gw * 0.5f), std::round(by0 + (by1 - by0) * 0.5f - 6));
      dl->AddRectFilled(ImVec2(g0.x + 2, g0.y + 4), ImVec2(g1.x + 2, g1.y + 5), IM_COL32(0, 0, 0, 80), theme::kRadius + 1);
      skin::raisedFace(dl, g0, g1, regionColor(k, 0.9f), theme::kRadius);
      dl->AddRect(g0, g1, P.paper, theme::kRadius, 0, 1.5f);
      textC(dl, a.fontBold, 14, ImVec2((g0.x + g1.x) * 0.5f, (g0.y + g1.y) * 0.5f), theme::contrastText(regionColor(k)), regionName(k).c_str());
    }
    if (!noop) {  // where it lands (drawn over the lifted block)
      const float cx = std::round(std::clamp(edge[static_cast<size_t>(best)], bx0 + 1, bx1 - 1));
      dl->AddLine(ImVec2(cx, by0 - 3), ImVec2(cx, by1 + 3), accent(), 2.5f);
      dl->AddTriangleFilled(ImVec2(cx - 5, by1 + 5), ImVec2(cx + 5, by1 + 5), ImVec2(cx, by1 - 1), accent());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) e.seqDrag = -1, e.seqDrop = -1;
    if (!io.MouseDown[0] && e.seqDrag >= 0) {
      Model nm = m;
      const int from = e.seqDrag;
      if (moveStep(nm, from, best)) {
        commit(a, t, e, nm, i18n::N_("Move a step"));
        e.selStep = best > from ? best - 1 : best;
      }
      e.seqDrag = -1, e.seqDrop = -1;
    }
  } else if (!io.MouseDown[0]) {
    e.seqDrag = -1, e.seqPress = -1;
  }
  // playhead
  if (lv.patternPos >= 0 && lv.patternRunning) {
    const float xx = bx0 + static_cast<float>(lv.patternPos) * bw;
    dl->AddLine(ImVec2(xx, by0 - 2), ImVec2(xx, by1 + 2), P.playhead, 1.5f);
  }
  // keys: Alt+Left / Alt+Right move the selected step, Delete deletes the region under the pointer (or the selected one)
  if (!io.WantTextInput && e.seqDrag < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    const int s = e.selStep;
    if (s >= 0 && s < ns && io.KeyAlt) {
      int to = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && s > 0) to = s - 1;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && s + 1 < ns) to = s + 2;
      Model nm = m;
      if (to >= 0 && moveStep(nm, s, to)) {
        commit(a, t, e, nm, i18n::N_("Move a step"));
        e.selStep = to > s ? to - 1 : to;
      }
    }
    if (laneHovered && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
      int target = -1;
      for (int i = 0; i < ns; ++i)
        if (mp.x >= edge[static_cast<size_t>(i)] && mp.x < edge[static_cast<size_t>(i) + 1]) target = i;
      if (target < 0) target = e.selStep;
      if (target >= 0 && target < ns) {
        const int k = stepSlice(m.pattern, m.pattern.steps[static_cast<size_t>(target)], nSl);
        if (k >= 0) deleteRegionUi(a, t, e, k);
      }
    }
  }
}

}  // namespace gl::packs::seltmann::sui
