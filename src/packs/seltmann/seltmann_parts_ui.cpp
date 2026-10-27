// Gridline — "seltmann" pack: the editor's lower parts. The layout picker (a
// tool: tabs and search in its header, cards that show the steps and what the layout does;
// a click applies it and plays it), the pads (two rows of eight, tinted by region, lit
// while playing, a drag handle on the selected one) and the Edit drawer with
// everything advanced: the slice inspector, the chop modes and manual cuts,
// the step editor and step language, the beat grid tools, the sound and
// playback settings and the list of loops to drag out.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string userDir(App& a) { return (fs::path(a.configDir()) / "seltmann" / "layouts").string(); }

// The layout's steps as letter chips (slot colours); generated layouts show dice dots.
void stepChips(ImDrawList* dl, App& a, ImVec2 p0, float w, const Preset& p) {
  const auto& P = pal();
  Pattern tmp;
  const float ch = 18, cw = 20, gap = 3;
  if (p.gen || p.steps.empty() || !patternFromText(p.steps, tmp)) {
    for (int i = 0; i < 8; ++i) {
      const ImVec2 q0(p0.x + i * (cw + gap), p0.y), q1(q0.x + cw, q0.y + ch);
      dl->AddRectFilled(q0, q1, slotColor((i * 5 + 3) % 12, 0.35f), 4);
      dl->AddCircleFilled(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), 2.2f, theme::withAlpha(P.text, 0.8f), 8);
    }
    textR(dl, a.fontRegular, theme::kFontLabel, ImVec2(p0.x + w, p0.y + 3), P.textDim, i18n::tr("a new take each time"));
    return;
  }
  const int fit = std::max(1, static_cast<int>((w - 28) / (cw + gap)));
  int shown = 0;
  float x = p0.x;
  for (size_t i = 0; i < tmp.steps.size(); ++i) {
    const Step& s = tmp.steps[i];
    if (shown >= fit) {
      char more[24];
      std::snprintf(more, sizeof more, "+%zu", tmp.steps.size() - i);
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 2, p0.y + 3), P.textDim, more);
      break;
    }
    const ImVec2 q0(x, p0.y), q1(x + cw, p0.y + ch);
    if (s.src == kRest) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 1.0f), 4);
    } else {
      const int c = s.slot ? s.src : s.src % 12;
      const ImU32 fill = slotColor(c, s.reverse ? 0.45f : 0.9f);
      dl->AddRectFilled(q0, q1, fill, 4);
      char lb[16];
      std::snprintf(lb, sizeof lb, "%s", s.slot ? slotName(s.src) : regionName(s.src).c_str());  // (layouts: letters = regions in order)
      textC(dl, a.fontBold, s.slot || s.src < 26 ? 12.0f : 10.0f, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), theme::contrastText(slotColor(c)), lb);
      if (s.stutter > 1) dl->AddLine(ImVec2(q0.x + 3, q1.y - 2), ImVec2(q1.x - 3, q1.y - 2), theme::withAlpha(theme::contrastText(slotColor(c)), 0.7f), 1);
    }
    x += cw + gap;
    for (int k = 0; k < s.ties && shown + 1 < fit; ++k) {  // a held step: a bar after the chip
      dl->AddRectFilled(ImVec2(x - gap, p0.y + ch * 0.4f), ImVec2(x + cw * 0.6f, p0.y + ch * 0.6f), theme::withAlpha(P.textDim, 0.5f), 2);
      x += cw * 0.6f + gap;
      ++shown;
    }
    ++shown;
  }
}

}  // namespace

// ------------------------------------------------------------------ layout picker
void drawPresets(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // J-152: the tool header (30 px, shared with the pads) hosts the title, the tabs and the search
  const float strip = kToolStrip;
  panel(dl, a, p0, p1, i18n::tr("LAYOUTS"), nullptr, strip);
  e.cards.clear();
  ImFont* tf = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
  const float titleW = std::round(tf->CalcTextSizeA(10.5f, 1e9f, 0, i18n::tr("LAYOUTS")).x);
  const float x = p0.x + 18 + titleW + 12, w = p1.x - 6 - x;
  float y = p0.y + 4;
  // tabs + search
  {
    static const char* const tabs[TabCount] = {i18n::N_("Flip"), i18n::N_("Phrase"), i18n::N_("Breaks"), i18n::N_("Micro"), i18n::N_("Creative"),
                                               i18n::N_("Mine")};
    static const char* const tabHints[TabCount] = {i18n::N_("Instant flips that work on anything"),
                                                   i18n::N_("Bars and phrases re-ordered (A B A C), and loops"),
                                                   i18n::N_("Breaks re-cut in eighths and sixteenths, and fills"),
                                                   i18n::N_("Tiny grains in new rhythms and melodies"),
                                                   i18n::N_("Shuffles, reverses, octave jumps and gates"),
                                                   i18n::N_("Your own saved layouts")};
    const float searchW = std::min(120.0f, w * 0.26f);
    const float tabsW = w - searchW - 6;
    int cur = e.search[0] ? -1 : e.presetTab;
    if (pills(a, "##sptab", ImVec2(x, y), tabsW, strip - 8, tabs, TabCount, cur, tabHints) && cur >= 0) {
      e.presetTab = cur;
      e.search[0] = 0;
    }
    ImGui::SetCursorScreenPos(ImVec2(x + w - searchW, y));
    ImGui::SetNextItemWidth(searchW);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, std::max(0.0f, (strip - 8 - ImGui::GetFontSize()) * 0.5f)));
    ImGui::InputTextWithHint("##ssearch", i18n::tr("Search"), e.search, sizeof e.search);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("Search layouts by name or what they do"));
    y = p0.y + strip + 8;
  }
  const float cx = p0.x + 10, cw = p1.x - p0.x - 20;  // the cards' column
  if (!e.userLoaded) {
    e.user = loadUserPresets(userDir(a));
    e.userLoaded = true;
  }
  const std::string q = lower(e.search);
  std::vector<const Preset*> list;
  for (const auto& p : factoryPresets())
    if (q.empty() ? presetTabOf(p.category) == e.presetTab : lower(p.name + " " + p.hint).find(q) != std::string::npos) list.push_back(&p);
  for (const auto& p : e.user)
    if (q.empty() ? e.presetTab == TabMine : lower(p.name).find(q) != std::string::npos) list.push_back(&p);
  const bool mine = q.empty() && e.presetTab == TabMine;
  const float saveH = mine ? 36.0f : 0.0f;
  const float listH = p1.y - 8 - saveH - y;
  ImGui::SetCursorScreenPos(ImVec2(cx - 4, y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##scards", ImVec2(cw + 8, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(2);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const ImVec2 c0 = ImGui::GetCursorScreenPos();
  const float scrollW = ImGui::GetScrollMaxY() > 0 ? 10.0f : 0.0f;
  const int cols = cw >= 480 ? 3 : 2;
  const float gap = 8, cardH = std::max(64.0f, std::floor((listH - gap) * 0.5f));  // two rows show, more scroll
  const float cardW = std::floor((cw - scrollW - gap * (cols - 1)) / cols);
  const ImVec2 clipMin = ImGui::GetWindowPos(), clipMax(clipMin.x + ImGui::GetWindowWidth(), clipMin.y + ImGui::GetWindowHeight());
  for (size_t i = 0; i < list.size(); ++i) {
    const Preset& p = *list[i];
    const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
    const ImVec2 q0(c0.x + 4 + c * (cardW + gap), c0.y + r * (cardH + gap)), q1(q0.x + cardW, q0.y + cardH);
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetCursorScreenPos(q0);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
    const bool hv = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool cur = p.name == e.m.preset;
    const ImU32 bg = cur ? theme::mix(P.surface700, accent(), 0.14f) : hv ? P.surface700 : P.surface900;
    skin::raisedFace(cl, q0, q1, bg, theme::kRadiusLg);
    cl->AddRect(q0, q1, cur ? accent() : hv ? theme::withAlpha(accent(), 0.5f) : P.line, theme::kRadiusLg, 0, cur ? 1.5f : 1.0f);
    theme::textEllipsis(cl, a.fontBold, theme::kFontBody, ImVec2(q0.x + 10, q0.y + 8), cardW - 44, P.text, p.name.c_str());
    if (cur) {
      // playing marker: three small bars
      for (int k = 0; k < 3; ++k) {
        const float hh = 4.0f + 3.0f * ((k + static_cast<int>(a.now * 6)) % 3);
        cl->AddRectFilled(ImVec2(q1.x - 26 + k * 6, q0.y + 20 - hh), ImVec2(q1.x - 23 + k * 6, q0.y + 20), accent(), 1);
      }
    }
    stepChips(cl, a, ImVec2(q0.x + 10, q0.y + 29), cardW - 20, p);
    theme::textEllipsis(cl, a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 10, q1.y - 21), cardW - 20, P.textDim, i18n::tr(p.hint.c_str()));
    if (hv) {
      std::string tip = i18n::tr(p.hint.c_str());
      tip += std::string("\n") + presetSummary(p, 32) + "\n" + i18n::tr("Click to use it; it plays at once.");
      if (cur && p.gen) tip += std::string("\n") + i18n::tr("Click again for another take.");
      ui::Tooltip(tip.c_str());
    }
    if (clicked) {
      if (cur && p.gen) ++e.seed;  // generated layouts: another take
      applyLayout(a, t, e, p, true);
    }
    const ImVec2 cc((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f);
    if (cc.y > clipMin.y && cc.y < clipMax.y) e.cards.push_back({p.name, cc});
  }
  const int rows = (static_cast<int>(list.size()) + cols - 1) / cols;
  ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + rows * (cardH + gap) - gap - 1));
  ImGui::Dummy(ImVec2(1, 1));
  if (list.empty())
    cl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(c0.x + 6, c0.y + 6), P.textDim,
                mine ? i18n::tr("Your layouts appear here. Save the current one below.") : i18n::tr("No layout matches"));
  ImGui::EndChild();
  if (mine) {
    const float sy = p1.y - 8 - 28;
    ImGui::SetCursorScreenPos(ImVec2(cx, sy));
    ImGui::SetNextItemWidth(cw - 104);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, std::max(0.0f, (28 - ImGui::GetFontSize()) * 0.5f)));
    ImGui::InputTextWithHint("##spname", i18n::tr("Name for the current layout"), e.presetName, sizeof e.presetName);
    ImGui::PopStyleVar();
    if (button("##spsave", ImVec2(cx + cw - 96, sy), ImVec2(96, 28), i18n::N_("Save"), false,
               i18n::N_("Save the chop settings and the pattern as your own layout"), static_cast<int>(ui::Icon::Save), true) &&
        e.presetName[0]) {
      if (saveUserPreset(userDir(a), e.presetName, e.m, static_cast<float>(paramOf(a, t, SmSwing)), static_cast<int>(std::lround(paramOf(a, t, SmSpeed))))) {
        e.userLoaded = false;
        a.notify(std::string(i18n::tr("Saved the layout ")) + e.presetName);
        e.presetName[0] = 0;
      }
    }
  }
}

// ------------------------------------------------------------------ pads
void drawPads(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int n = e.hasModel ? static_cast<int>(e.m.slices.size()) : 0;
  char cap[48];
  std::snprintf(cap, sizeof cap, "%d %s", n, n == 1 ? i18n::tr("REGION") : i18n::tr("REGIONS"));
  panel(dl, a, p0, p1, i18n::tr("PADS"), cap, kToolStrip);
  if (!e.hasModel) return;
  // J-152: two rows of eight (A..H over I..P), sixteen to a page as before
  const int cols = 8, rows = 2, per = cols * rows;
  const int pages = std::max(1, (n + per - 1) / per);
  e.padPage = std::clamp(e.padPage, 0, pages - 1);
  if (pages > 1) {
    ImFont* tf = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
    const float dx = p0.x + 18 + std::round(tf->CalcTextSizeA(10.5f, 1e9f, 0, i18n::tr("PADS")).x) + 16;
    for (int pg = 0; pg < pages && pg < 16; ++pg) {
      const ImVec2 c(dx + pg * 14.0f, std::round(p0.y + kToolStrip * 0.5f));
      ImGui::SetCursorScreenPos(ImVec2(c.x - 6, c.y - 6));
      ImGui::PushID(pg);
      if (ImGui::InvisibleButton("##pg", ImVec2(12, 12))) e.padPage = pg;
      if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("Page of pads"));
      ImGui::PopID();
      dl->AddCircleFilled(c, pg == e.padPage ? 4.0f : 3.0f, pg == e.padPage ? accent() : P.surface600, 10);
    }
  }
  const float gx0 = p0.x + 10, gy0 = p0.y + kToolStrip + 8, gw = p1.x - p0.x - 20, gh = p1.y - 8 - gy0;
  const float gap = 6, cw = std::floor((gw - gap * (cols - 1)) / cols), ch = std::floor((gh - gap * (rows - 1)) / rows);
  SamplePtr smp = sampleOf(e);
  const int base = baseNote(a, t);
  const bool slotsMap = static_cast<int>(std::lround(paramOf(a, t, SmKeyMap))) == KeySlots;
  for (int i = 0; i < per; ++i) {
    const int k = e.padPage * per + i;
    const int r = i / cols, c = i % cols;
    const ImVec2 q0(gx0 + c * (cw + gap), gy0 + r * (ch + gap)), q1(q0.x + cw, q0.y + ch);
    if (k >= n) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 0.5f), theme::kRadiusLg);
      continue;
    }
    const auto& s = e.m.slices[static_cast<size_t>(k)];
    const bool sel = k == e.selSlice, playing = k == lv.slice;
    const std::string L = regionName(k);
    ImGui::PushID(k);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##pad", ImVec2(cw, ch));
    const bool hv = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
      e.selSlice = k;
      auditionStart(a, t, e, k);
    }
    if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
    // drag a pad onto a pattern step to place it there
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      auditionStop(a, t, e);
      ImGui::SetDragDropPayload("GL_SELT_SLICE", &k, sizeof k);
      char b[48];
      std::snprintf(b, sizeof b, "%s %s", i18n::tr("Region"), L.c_str());
      ui::DragPreview(ui::Icon::Wave, b, regionColor(k));
      ImGui::EndDragDropSource();
    }
    if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      e.selSlice = k;
      openRegionMenu(e, k, -1);
    }
    if (hv && !ImGui::IsItemActive()) {
      char b[240];
      const int slot = sliceSlot(e.m, k);
      const int key = slotsMap ? (slot >= 0 ? base + slot : -1) : base + k;
      std::snprintf(b, sizeof b, "%s %s  \xC2\xB7  %s %s  \xC2\xB7  %.3f s\n%s", i18n::tr("Region"), L.c_str(), i18n::tr("key"),
                    key >= 0 && key <= 127 ? noteName(key).c_str() : "-", static_cast<double>(s.end - s.start) / e.m.srcRate,
                    i18n::tr("Click to play. Right-click: split or delete. Drag the grip to take it out as audio."));
      ui::Tooltip(b);
    }
    // J-152: a quiet tinted pad with its hue along the top; it lights up in full while it plays
    const ImU32 face = playing ? regionColor(k) : regionFace(k, hv ? 0.34f : sel ? 0.28f : 0.2f);
    skin::raisedFace(dl, q0, q1, face, theme::kRadiusLg);
    if (!playing) dl->AddRectFilled(ImVec2(q0.x + 1, q0.y + 1), ImVec2(q1.x - 1, q0.y + 4), regionColor(k, 0.95f), theme::kRadiusLg, ImDrawFlags_RoundCornersTop);
    if (playing) dl->AddRect(ImVec2(q0.x - 2, q0.y - 2), ImVec2(q1.x + 2, q1.y + 2), theme::withAlpha(P.paper, 0.9f), theme::kRadiusLg + 2, 0, 2.0f);
    else if (sel) dl->AddRect(ImVec2(q0.x - 1.5f, q0.y - 1.5f), ImVec2(q1.x + 1.5f, q1.y + 1.5f), accent(), theme::kRadiusLg + 1, 0, 2.0f);
    const ImU32 ink = playing ? theme::contrastText(regionColor(k)) : P.text;
    const ImU32 waveInk = playing ? theme::withAlpha(ink, 0.6f) : regionColor(k, 0.85f);
    if (smp) {
      const float wy0 = q1.y - ch * 0.45f, wy1 = q1.y - 6;
      const float mid = (wy0 + wy1) * 0.5f, half = (wy1 - wy0) * 0.5f;
      const int cols2 = std::max(4, static_cast<int>(cw - 14) / 2);
      const int64_t len = std::max<int64_t>(1, s.end - s.start);
      const int64_t nn = static_cast<int64_t>(smp->frames());
      for (int j = 0; j < cols2; ++j) {
        const int64_t f0 = s.start + len * j / cols2, f1 = s.start + len * (j + 1) / cols2;
        float mx = 0;
        const int64_t step = std::max<int64_t>(1, (f1 - f0) / 64);
        for (int64_t f = f0; f < f1 && f < nn; f += step) mx = std::max(mx, std::fabs(smp->left[static_cast<size_t>(f)]));
        const float xx = q0.x + 7 + j * 2.0f;
        const float hh = std::max(0.5f, std::min(1.0f, mx * 1.2f) * half);
        dl->AddLine(ImVec2(xx, mid - hh), ImVec2(xx, mid + hh), waveInk, 1.2f);
      }
    }
    dl->AddText(a.fontBold, 16, ImVec2(q0.x + 8, q0.y + 8), ink, L.c_str());
    if (s.reverse) dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q0.x + 12 + 10 * static_cast<float>(L.size()), q0.y + 11), theme::withAlpha(ink, 0.8f), "R");
    ImGui::PopID();
  }
  // drag handle on the selected pad: the slice out as audio
  if (e.selSlice >= 0 && e.selSlice < n) {
    const int i = e.selSlice - e.padPage * per;
    if (i >= 0 && i < per) {
      const int r = i / cols, c = i % cols;
      const ImVec2 q0(gx0 + c * (cw + gap), gy0 + r * (ch + gap));
      const ImVec2 g(q0.x + cw - 10, q0.y + 16);
      ImGui::SetCursorScreenPos(ImVec2(g.x - 8, g.y - 10));
      ImGui::InvisibleButton("##sgrip", ImVec2(16, 20));
      const bool h = ImGui::IsItemHovered();
      if (h && !ImGui::IsItemActive()) ui::Tooltip(i18n::tr("Drag the slice out as audio"));
      if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      char lab[32];
      std::snprintf(lab, sizeof lab, "region %s", regionName(e.selSlice).c_str());
      dragOut(a, t, e, "slice:" + std::to_string(e.selSlice), lab);
      grip(dl, g, theme::withAlpha(e.selSlice == lv.slice ? theme::contrastText(regionColor(e.selSlice)) : P.text, h ? 1.0f : 0.6f));
    }
  }
}

namespace {

// ------------------------------------------------------------------ drawer: slice inspector
void drawSlice(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (e.m.slices.empty()) return;
  e.selSlice = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
  Slice s = e.m.slices[static_cast<size_t>(e.selSlice)];
  const std::string L = regionName(e.selSlice);
  float x = p0.x;
  const float y = p0.y;
  // badge, name, length, play + drag
  skin::raisedFace(dl, ImVec2(x, y), ImVec2(x + 40, y + 40), regionColor(e.selSlice), theme::kRadiusLg);
  textC(dl, a.fontBold, 20, ImVec2(x + 20, y + 20), theme::contrastText(regionColor(e.selSlice)), L.c_str());
  char b[96];
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Region"), L.c_str());
  dl->AddText(a.fontBold, theme::kFontBody + 1, ImVec2(x + 52, y + 1), P.text, b);
  std::snprintf(b, sizeof b, "%.3f s  ·  %s %s", static_cast<double>(s.end - s.start) / e.m.srcRate, i18n::tr("at"),
                fmtTime(static_cast<double>(s.start) / e.m.srcRate).c_str());
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x + 52, y + 22), P.textDim, b);
  if (button("##splay1", ImVec2(x, y + 52), ImVec2(40, 30), "", false, i18n::N_("Play the slice"), static_cast<int>(ui::Icon::Play)))
    auditionStart(a, t, e, e.selSlice);
  if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
  char lab[32];
  std::snprintf(lab, sizeof lab, "region %s", L.c_str());
  dragChip(a, t, e, "##sdragslice", ImVec2(x + 48, y + 52), 30, "WAV", "slice:" + std::to_string(e.selSlice), lab,
           i18n::N_("Drag the slice out as audio: into the playlist, the channel rack or the browser"));
  // J-149: split / delete the region (letters follow)
  groupLabel(dl, a, ImVec2(x, y + 94), i18n::N_("REGION"));
  {
    static const int parts[3] = {2, 3, 4};
    static const char* const txt[3] = {"1/2", "1/3", "1/4"};
    static const char* const hint[3] = {i18n::N_("Split the region in half (the next letters move on)"), i18n::N_("Split the region in three"),
                                        i18n::N_("Split the region in four")};
    for (int i = 0; i < 3; ++i) {
      char id[16];
      std::snprintf(id, sizeof id, "##ssplit%d", parts[i]);
      if (button(id, ImVec2(x + i * 44, y + 110), ImVec2(40, 24), txt[i], false, hint[i])) splitRegionUi(a, t, e, e.selSlice, parts[i]);
    }
    const bool can = e.m.slices.size() > 1;
    if (button("##sdelreg", ImVec2(x + 136, y + 110), ImVec2(64, 24), i18n::N_("Delete"), false,
               can ? i18n::N_("Delete the region: it joins its neighbour (the first one joins the next)") : i18n::N_("The last region cannot be deleted")) &&
        can)
      deleteRegionUi(a, t, e, e.selSlice);
  }
  // knobs
  x = p0.x + 240;
  bool rel = false, changed = false;
  const float kw = 74;
  float off = s.offset * 100, len = s.length * 100, choke = static_cast<float>(s.choke);
  changed |= fieldKnob(a, "##kp", ImVec2(x + 0 * kw, y + 6), s.pitch, -24, 24, 0, i18n::N_("Pitch"), "%+.1f st", &rel, true);
  changed |= fieldKnob(a, "##kg", ImVec2(x + 1 * kw, y + 6), s.gain, -24, 12, 0, i18n::N_("Gain"), "%+.1f dB", &rel, true);
  changed |= fieldKnob(a, "##kfi", ImVec2(x + 2 * kw, y + 6), s.fadeIn, 0, 200, 0, i18n::N_("Fade in"), "%.0f ms", &rel);
  changed |= fieldKnob(a, "##kfo", ImVec2(x + 3 * kw, y + 6), s.fadeOut, 0, 400, 0, i18n::N_("Fade out"), "%.0f ms", &rel);
  changed |= fieldKnob(a, "##ko", ImVec2(x + 4 * kw, y + 6), off, 0, 90, 0, i18n::N_("Start"), "%.0f%%", &rel);
  changed |= fieldKnob(a, "##kl", ImVec2(x + 5 * kw, y + 6), len, 5, 100, 100, i18n::N_("Length"), "%.0f%%", &rel);
  changed |= fieldKnob(a, "##kc", ImVec2(x + 6 * kw, y + 6), choke, 0, 8, 0, i18n::N_("Choke"), "%.0f", &rel);
  s.offset = off / 100, s.length = len / 100, s.choke = static_cast<int>(std::lround(choke));
  if (button("##krev", ImVec2(x + 7 * kw + 4, y + 14), ImVec2(70, 28), i18n::N_("Reverse"), s.reverse, i18n::N_("Play this slice backwards"))) {
    s.reverse = !s.reverse;
    changed = true;
    rel = true;
  }
  if (changed || rel) {
    Model nm = e.m;
    nm.slices[static_cast<size_t>(e.selSlice)] = s;
    if (rel) commit(a, t, e, nm, i18n::N_("Slice settings"), 0x5E17D000ull + static_cast<uint64_t>(e.selSlice));
    else e.m = nm;  // live while dragging; written on release
  }
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, y + 94), P.textDim,
              i18n::tr("Pick a region on the pads, in the waveform or in the sequence. Choke: regions in the same group cut each other off."));
  (void)p1;
}

// ------------------------------------------------------------------ drawer: chop
void drawChop(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  drawChopSettings(a, t, e, p0, p1.x - p0.x);
  const float y = p0.y + 44;
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y), P.textDim,
              i18n::tr("Manual cuts in the waveform: double-click to add a cut, drag a cut to move it, right-click a cut to remove it."));
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 20), P.textDim,
              i18n::tr("Grid chops stay on the bars; cuts never make a slice shorter than half a step (tiny pieces join their neighbour)."));
  if (e.m.grid.kind == KindSong || barCount(e.m) > 16)
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 40), P.textDim,
                i18n::tr("Songs: pick the bars to chop in the strip above the waveform (drag the box, or double-click a section)."));
}

// ------------------------------------------------------------------ drawer: steps
void drawSteps(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Model& m = e.m;
  const float W = p1.x - p0.x;
  float x = p0.x;
  // name, step length, add / remove
  groupLabel(dl, a, ImVec2(x, p0.y), i18n::N_("PATTERN"));
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(x + 2, p0.y + 19), 150, P.text, m.pattern.name.c_str());
  x += 170;
  groupLabel(dl, a, ImVec2(x, p0.y), i18n::N_("STEP LENGTH"));
  static const char* const unitLabels[8] = {"1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
  static const int unitDiv[8] = {Div1Bar, DivHalf, DivBeat, Div8th, Div16th, Div32nd, Div8T, Div16T};
  static const char* const unitH[8] = {i18n::N_("Steps of one bar"),     i18n::N_("Steps of half a bar"),  i18n::N_("Steps of a beat"),
                                       i18n::N_("Steps of an eighth"),   i18n::N_("Steps of a sixteenth"), i18n::N_("Steps of a thirty-second"),
                                       i18n::N_("Eighth-triplet steps"), i18n::N_("Sixteenth-triplet steps")};
  int cu = -1;
  for (int i = 0; i < 8; ++i)
    if (std::fabs(divisionBeats(unitDiv[i], m.grid.beatsPerBar) - m.pattern.unitBeats) < 1e-6) cu = i;
  if (pills(a, "##sunit", ImVec2(x, p0.y + 16), 340, 26, unitLabels, 8, cu, unitH, true) && cu >= 0) {
    Model nm = m;
    nm.pattern.unitBeats = divisionBeats(unitDiv[cu], nm.grid.beatsPerBar);
    commit(a, t, e, nm, i18n::N_("Step length"));
  }
  x += 352;
  if (button("##sminus", ImVec2(x, p0.y + 16), ImVec2(30, 26), "", false, i18n::N_("Remove the last step"), static_cast<int>(ui::Icon::Minus)) &&
      m.pattern.steps.size() > 1) {
    Model nm = m;
    nm.pattern.steps.pop_back();
    markEdited(nm);
    commit(a, t, e, nm, i18n::N_("Remove a step"));
  }
  if (button("##splus", ImVec2(x + 34, p0.y + 16), ImVec2(30, 26), "", false, i18n::N_("Add a step (the next slice)"), static_cast<int>(ui::Icon::Plus)) &&
      m.pattern.steps.size() < static_cast<size_t>(kMaxSteps)) {
    Model nm = m;
    Step st;
    st.slot = false;
    st.src = static_cast<int>(nm.pattern.steps.size()) % std::max(1, static_cast<int>(nm.slices.size()));
    nm.pattern.steps.push_back(st);
    markEdited(nm);
    commit(a, t, e, nm, i18n::N_("Add a step"));
  }
  // ---- steps
  const float sy0 = p0.y + 54, sy1 = p1.y - 44;
  const float sx0 = p0.x, sw = W;
  const int ns = static_cast<int>(m.pattern.steps.size());
  double units = 0;
  for (const auto& s : m.pattern.steps) units += 1 + s.ties;
  const int nSl = static_cast<int>(m.slices.size());
  double u = 0;
  for (int i = 0; i < ns; ++i) {
    const Step& s = m.pattern.steps[static_cast<size_t>(i)];
    const float x0 = sx0 + static_cast<float>(u / units) * sw, x1 = sx0 + static_cast<float>((u + 1 + s.ties) / units) * sw;
    u += 1 + s.ties;
    const ImVec2 q0(x0 + 1.5f, sy0), q1(x1 - 1.5f, sy1);
    const int k = stepSlice(m.pattern, s, nSl);
    const int colorSlot = s.src == kRest ? -1 : k;
    const bool playing = i == lv.step, sel = i == e.selStep;
    ImGui::PushID(1000 + i);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##step", ImVec2(std::max(2.0f, q1.x - q0.x), q1.y - q0.y));
    const bool hv = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(0)) e.selStep = sel ? -1 : i;
    if (hv && ImGui::GetIO().MouseWheel != 0 && s.src != kRest) {
      Model nm = m;
      Step& st = nm.pattern.steps[static_cast<size_t>(i)];
      const int dir = ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
      const int lim = st.slot ? std::min(kSlotCount, std::max(1, nSl)) : std::max(1, nSl);
      st.src = ((st.src + dir) % lim + lim) % lim;
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Change a step"), 0x5E17E000ull + static_cast<uint64_t>(i));
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SELT_SLICE")) {
        const int slice = *static_cast<const int*>(pl->Data);
        Model nm = m;
        Step& st = nm.pattern.steps[static_cast<size_t>(i)];
        const int let = sliceSlot(nm, slice);
        if (let >= 0) st.slot = true, st.src = let;
        else st.slot = false, st.src = slice;
        markEdited(nm);
        commit(a, t, e, nm, i18n::N_("Place a slice on a step"));
      }
      ImGui::EndDragDropTarget();
    }
    if (hv && ImGui::IsMouseClicked(1)) {
      e.selStep = i;
      ImGui::OpenPopup("##stepmenu");
    }
    if (hv && !ImGui::IsItemActive()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s %d: %s\n%s", i18n::tr("Step"), i + 1, stepToText(s).c_str(),
                    i18n::tr("Wheel: another letter. Right-click: options. Drop a pad here to place its slice."));
      ui::Tooltip(b);
    }
    if (s.src == kRest) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 0.9f), theme::kRadius);
      textC(dl, a.fontBold, 14, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), P.textFaint, ".");
    } else {
      // J-152: the same quiet face as the sequence lane; the playing step lights up
      const ImU32 fc = playing ? slotColor(colorSlot) : regionFace(colorSlot, hv ? 0.42f : sel ? 0.36f : 0.28f);
      skin::raisedFace(dl, q0, q1, fc, theme::kRadius);
      if (!playing) dl->AddRectFilled(ImVec2(q0.x + 1, q1.y - 3), ImVec2(q1.x - 1, q1.y - 1), slotColor(colorSlot, 0.95f), 1.0f, ImDrawFlags_RoundCornersBottom);
      if (s.reverse)
        dl->AddRectFilledMultiColor(q0, q1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.25f), theme::withAlpha(P.ink, 0.25f),
                                    theme::withAlpha(P.ink, 0.0f));
      const ImU32 ink = playing ? theme::contrastText(slotColor(colorSlot)) : P.text;
      const float cw = q1.x - q0.x;
      char lb[16];
      std::snprintf(lb, sizeof lb, "%s", regionName(k).c_str());
      if (cw >= 12) textC(dl, a.fontBold, cw >= 26 ? 16.0f : 12.0f, ImVec2((q0.x + q1.x) * 0.5f, q0.y + (q1.y - q0.y) * 0.4f), ink, lb);
      if (cw >= 26) {
        std::string marks;
        if (s.reverse) marks += "R ";
        if (s.pitch != 0) {
          char pb[16];
          std::snprintf(pb, sizeof pb, "%+d ", static_cast<int>(std::lround(s.pitch)));
          marks += pb;
        }
        if (s.stutter > 1) marks += "x" + std::to_string(s.stutter) + " ";
        if (s.gain < -0.05f) marks += "q ";
        if (!marks.empty()) {
          marks.pop_back();
          textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2((q0.x + q1.x) * 0.5f, q1.y - 9), theme::withAlpha(ink, 0.85f), marks.c_str());
        }
      }
      if (s.stutter > 1)
        for (int r = 1; r < s.stutter; ++r) {
          const float xx = q0.x + (q1.x - q0.x) * r / s.stutter;
          dl->AddLine(ImVec2(xx, q0.y + 2), ImVec2(xx, q0.y + 6), theme::withAlpha(ink, 0.6f), 1);
        }
    }
    if (playing) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), P.paper, theme::kRadius + 1, 0, 2.0f);
    else if (sel) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), accent(), theme::kRadius + 1, 0, 2.0f);
    if (ImGui::BeginPopup("##stepmenu")) {
      Model nm = m;
      Step& st = nm.pattern.steps[static_cast<size_t>(i)];
      bool ch = false;
      ImGui::TextDisabled("%s %d", i18n::tr("Step"), i + 1);
      if (ImGui::BeginMenu(i18n::tr("Play slot"))) {
        for (int l = 0; l < std::min(kSlotCount, std::max(1, nSl)); ++l) {
          char lb[32];
          std::snprintf(lb, sizeof lb, "%s  (%s %d)", slotName(l), i18n::tr("slice"), stepSlice(nm.pattern, Step{l, true, 0, false, 0, 0, 1, 1, 0}, nSl) + 1);
          if (ImGui::MenuItem(lb, nullptr, st.slot && st.src == l)) st.slot = true, st.src = l, ch = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("Play region"))) {
        for (int l = 0; l < nSl && l < 128; ++l) {
          char lb[16];
          std::snprintf(lb, sizeof lb, "%s", regionName(l).c_str());
          if (ImGui::MenuItem(lb, nullptr, !st.slot && st.src == l)) st.slot = false, st.src = l, ch = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(i18n::tr("Rest"), nullptr, st.src == kRest)) st.src = kRest, ch = true;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, st.reverse)) st.reverse = !st.reverse, ch = true;
      int stut = st.stutter;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderInt(i18n::tr("Stutter"), &stut, 1, 16)) st.stutter = stut, ch = true;
      float pitch = st.pitch;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderFloat(i18n::tr("Pitch"), &pitch, -24, 24, "%+.0f st")) st.pitch = std::round(pitch), ch = true;
      float gain = st.gain;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderFloat(i18n::tr("Gain"), &gain, -30, 6, "%+.1f dB")) st.gain = gain, ch = true;
      float len = st.length * 100;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderFloat(i18n::tr("Length"), &len, 5, 100, "%.0f%%")) st.length = len / 100, ch = true;
      float off = st.offset * 100;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderFloat(i18n::tr("Start"), &off, 0, 90, "%.0f%%")) st.offset = off / 100, ch = true;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Longer (tie the next unit)"))) ++st.ties, ch = true;
      if (st.ties > 0 && ImGui::MenuItem(i18n::tr("Shorter"))) --st.ties, ch = true;
      bool structural = false;
      if (ImGui::MenuItem(i18n::tr("Duplicate step"))) {
        nm.pattern.steps.insert(nm.pattern.steps.begin() + i + 1, st);
        structural = true;
      }
      if (ns > 1 && ImGui::MenuItem(i18n::tr("Delete step"))) {
        nm.pattern.steps.erase(nm.pattern.steps.begin() + i);
        structural = true;
        e.selStep = -1;
      }
      if (ch || structural) markEdited(nm);
      if (ch || structural) commit(a, t, e, nm, i18n::N_("Edit a step"), structural ? 0 : 0x5E17F000ull + static_cast<uint64_t>(i));
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (lv.patternPos >= 0) {
    const float xx = sx0 + static_cast<float>(lv.patternPos) * sw;
    dl->AddLine(ImVec2(xx, sy0 - 3), ImVec2(xx, sy1 + 3), P.playhead, 1.5f);
  }
  // ---- step language
  {
    const std::string cur = patternToText(m.pattern);
    if (cur != e.dslFor) {
      std::snprintf(e.dsl, sizeof e.dsl, "%s", cur.c_str());
      e.dslFor = cur;
    }
    groupLabel(dl, a, ImVec2(p0.x, p1.y - 30), i18n::N_("STEPS"));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 56, p1.y - 34));
    ImGui::SetNextItemWidth(W - 56);
    ImGui::PushFont(a.fontMono);
    const bool enter = ImGui::InputText("##sdsl", e.dsl, sizeof e.dsl, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopFont();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
      ui::Tooltip(i18n::tr("Type the steps: 1, 2, 3 ... = the regions A, B, C ... in order; A..Z = layout letters; . = rest, ~ = hold. Options after a colon: r reverse, "
                           "p+7 pitch, g-6 gain dB, l50 length %, x4 stutter, o25 start %. Enter applies."));
    if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
      Pattern p = m.pattern;
      std::string err;
      if (patternFromText(e.dsl, p, &err)) {
        Model nm = m;
        nm.pattern.steps = p.steps;
        markEdited(nm);
        commit(a, t, e, nm, i18n::N_("Type the steps"));
      } else {
        a.notify(std::string(i18n::tr("Steps not changed: ")) + err, true);
        e.dslFor.clear();
      }
    }
  }
}

// ------------------------------------------------------------------ drawer: sound
void drawSound(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float x = p0.x;
  const float y = p0.y;
  // playback
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("FOLLOW THE PROJECT BY"));
  {
    static const char* const l[2] = {i18n::N_("Stretch"), i18n::N_("Resample")};
    static const char* const h[2] = {i18n::N_("Time-stretched: the pitch stays"), i18n::N_("Sped up or slowed down: the pitch follows (vinyl)")};
    const int sync = static_cast<int>(std::lround(paramOf(a, t, SmSync)));
    int cur = sync == SyncResample ? 1 : sync == SyncStretch ? 0 : -1;
    if (pills(a, "##sfollow", ImVec2(x, y + 16), 180, 26, l, 2, cur, h)) pui::setParamValue(a, t, SmSync, cur == 1 ? SyncResample : SyncStretch, false);
  }
  x += 196;
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("STRETCH"));
  {
    static const char* const l[3] = {i18n::N_("Auto"), i18n::N_("Drums"), i18n::N_("Mono")};
    static const char* const h[3] = {i18n::N_("General stretch for full mixes"), i18n::N_("Keeps drum hits sharp"), i18n::N_("For vocals, bass and single lines")};
    paramPills(a, t, SmStretchMode, ImVec2(x, y + 16), 186, 26, l, 3, h);
  }
  x += 202;
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("KEYS PLAY"));
  {
    static const char* const l[2] = {i18n::N_("Slices"), i18n::N_("Letters")};
    static const char* const h[2] = {i18n::N_("Keys from the base note play the slices in order"),
                                     i18n::N_("Keys from the base note play the slot letters A, B, C...")};
    paramPills(a, t, SmKeyMap, ImVec2(x, y + 16), 150, 26, l, 2, h);
  }
  x += 166;
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("SPEED"));
  {
    static const char* const l[3] = {"1/2", "1x", "2x"};
    static const char* const h[3] = {i18n::N_("Pattern at half speed"), i18n::N_("Pattern at normal speed"), i18n::N_("Pattern at double speed")};
    paramPills(a, t, SmSpeed, ImVec2(x, y + 16), 120, 26, l, 3, h);
  }
  x += 136;
  ImGui::SetCursorScreenPos(ImVec2(x, y + 18));
  pui::led(a, t, SmLock, i18n::tr("Lock to song"));
  ImGui::SetCursorScreenPos(ImVec2(x + 120, y + 18));
  pui::led(a, t, SmMono, i18n::tr("Mono"));
  ImGui::SetCursorScreenPos(ImVec2(x, y + 44));
  pui::stepper(a, t, SmBaseNote, 120, i18n::tr("Base"));
  {
    char b[48];
    std::snprintf(b, sizeof b, "%s %s", i18n::tr("pattern key"), noteName(baseNote(a, t) - 12).c_str());
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(ImGui::GetItemRectMax().x + 10, y + 50), P.textFaint, b);
  }
  // knobs
  const float ky = y + 78;
  const float kw = pui::knobCellWidth(pui::KnobSmall) + 4;
  const int params[13] = {SmSwing, SmGate, SmVelocity, SmPitch, SmAttack, SmDecay, SmSustain, SmRelease, SmDeclick, SmCutoff, SmReso, SmPan, SmVolume};
  const char* names[13] = {i18n::N_("Swing"),   i18n::N_("Gate"),    i18n::N_("Velocity"), i18n::N_("Pitch"),     i18n::N_("Attack"),
                           i18n::N_("Decay"),   i18n::N_("Sustain"), i18n::N_("Release"),  i18n::N_("Declick"),   i18n::N_("Cutoff"),
                           i18n::N_("Resonance"), i18n::N_("Pan"),   i18n::N_("Volume")};
  for (int k = 0; k < 13; ++k) pui::knobAt(a, t, params[k], ImVec2(p0.x + k * kw, ky), i18n::tr(names[k]), pui::KnobSmall);
  ImGui::SetCursorScreenPos(ImVec2(p0.x, std::min(p1.y - 22, ky + pui::knobCellHeight(pui::KnobSmall) + 10)));
  pui::led(a, t, SmReverse, i18n::tr("Reverse every slice"));
}

// ------------------------------------------------------------------ drawer: loops
void drawLoops(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const float w = p1.x - p0.x;
  ImGui::SetCursorScreenPos(p0);
  ImGui::BeginChild("##sloops", ImVec2(w, p1.y - p0.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const int bpb = std::max(1, e.m.grid.beatsPerBar);
  struct Row {
    std::string item, label, info;
    int color;
    float score;
  };
  std::vector<Row> rows;
  {
    char b[96];
    const PluginState* sp = t.state(a.doc.song());
    const double pb = patternBars(e.m, sp ? sp->params : std::vector<float>{}, bpb);
    std::snprintf(b, sizeof b, "%.4g %s", pb, std::fabs(pb - 1) < 1e-9 ? i18n::tr("bar") : i18n::tr("bars"));
    rows.push_back({"pattern", std::string(i18n::tr("Pattern")) + ": " + e.m.pattern.name, b, -1, -1});
  }
  for (size_t i = 0; i < e.m.grid.loops.size(); ++i) {
    const auto& l = e.m.grid.loops[i];
    const int bar = static_cast<int>(std::floor(static_cast<double>(l.firstBeat - e.m.grid.bar1) / bpb)) + 1;
    char it[64], lab[64], info[64];
    std::snprintf(it, sizeof it, "span:%d:%d", l.firstBeat, l.firstBeat + l.bars * bpb);
    std::snprintf(lab, sizeof lab, "%s %d", i18n::tr("Phrase at bar"), bar);
    std::snprintf(info, sizeof info, "%d %s", l.bars, l.bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
    int sec = 0;
    for (const auto& s : e.m.grid.sections)
      if (l.firstBeat >= s.firstBeat && l.firstBeat < s.endBeat) sec = s.label;
    rows.push_back({it, lab, info, sec, l.score});
  }
  for (size_t i = 0; i < e.m.grid.sections.size(); ++i) {
    const auto& s = e.m.grid.sections[i];
    char it[64], lab[64], info[64];
    std::snprintf(it, sizeof it, "span:%d:%d", s.firstBeat, s.endBeat);
    std::snprintf(lab, sizeof lab, "%s %s%zu", i18n::tr("Section"), slotName(s.label), i + 1);
    std::snprintf(info, sizeof info, "%d %s", (s.endBeat - s.firstBeat) / bpb, i18n::tr("bars"));
    rows.push_back({it, lab, info, s.label, -1});
  }
  const float rowH = 34, colGap = 12;
  const float colW = std::floor((w - colGap - 10) / 2);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    const int col = static_cast<int>(i % 2), line = static_cast<int>(i / 2);
    const ImVec2 r0(o.x + col * (colW + colGap), o.y + line * rowH), r1(r0.x + colW, r0.y + rowH - 4);
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetCursorScreenPos(r0);
    ImGui::InvisibleButton("##lrow", ImVec2(colW - 34, rowH - 4));
    const bool hv = ImGui::IsItemHovered();
    if (hv && !ImGui::IsItemActive()) ui::Tooltip(i18n::tr("Drag into the playlist (audio clip), the channel rack (sampler) or the browser"));
    if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dragOut(a, t, e, r.item, r.label);
    cl->AddRectFilled(r0, r1, hv ? P.surface700 : P.surface900, theme::kRadius);
    cl->AddRectFilled(r0, ImVec2(r0.x + 4, r1.y), r.color >= 0 ? slotColor(r.color) : accent(), 1);
    grip(cl, ImVec2(r0.x + 15, (r0.y + r1.y) * 0.5f), hv ? P.text : P.textFaint);
    theme::textEllipsis(cl, a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 26, r0.y + 8), colW - 150, P.text, r.label.c_str());
    textR(cl, a.fontMono, theme::kFontLabel, ImVec2(r1.x - 38, r0.y + 9), P.textDim, r.info.c_str());
    if (r.score >= 0)
      for (int s = 0; s < 3; ++s)
        cl->AddCircleFilled(ImVec2(r1.x - 100 + s * 6.0f, (r0.y + r1.y) * 0.5f), 2, r.score > 0.55f + s * 0.12f ? accent() : P.surface600, 8);
    if (button("##lplay", ImVec2(r1.x - 30, r0.y + 3), ImVec2(26, rowH - 10), "", false, i18n::N_("Listen to this loop"), static_cast<int>(ui::Icon::Play))) {
      const std::string f = renderItem(a, t, e, r.item, r.label);
      if (!f.empty()) a.previewSample(f);
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + ((rows.size() + 1) / 2) * rowH));
  ImGui::Dummy(ImVec2(1, 1));
  ImGui::EndChild();
}

}  // namespace

// ------------------------------------------------------------------ Edit drawer
float drawerHeight(const Ed& e) {
  // J-152: each tab takes the height it needs (the tabs stay where they are)
  switch (e.drawerTab) {
    case DrawSlice: return 200;
    case DrawChop:
    case DrawGrid: return 170;
    case DrawLoops: {
      const size_t rows = 1 + e.m.grid.loops.size() + e.m.grid.sections.size();
      return std::clamp(62.0f + static_cast<float>((rows + 1) / 2) * 34.0f, 132.0f, 236.0f);
    }
    default: return 236;  // steps, sound
  }
}

void drawDrawer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  panel(dl, a, p0, p1, nullptr);
  static const char* const tabs[DrawCount] = {i18n::N_("Slice"), i18n::N_("Chop"), i18n::N_("Steps"), i18n::N_("Grid"), i18n::N_("Sound"), i18n::N_("Loops")};
  static const char* const hints[DrawCount] = {i18n::N_("The selected slice: pitch, gain, fades, start, length, choke, reverse"),
                                               i18n::N_("Chop modes, cutting by hand, snapping"),
                                               i18n::N_("The pattern's steps and the step language"),
                                               i18n::N_("Tap the tempo, nudge the grid, meter, bar 1, analyse again"),
                                               i18n::N_("Playback and the envelope, filter and mix"),
                                               i18n::N_("Phrases and sections to drag out as loops")};
  pills(a, "##sdtab", ImVec2(p0.x + 10, p0.y + 10), 540, 28, tabs, DrawCount, e.drawerTab, hints);
  textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 16, p0.y + 18), P.textFaint, i18n::tr("EDIT"));
  const ImVec2 c0(p0.x + 16, p0.y + 50), c1(p1.x - 16, p1.y - 12);
  switch (e.drawerTab) {
    case DrawSlice: drawSlice(a, t, e, c0, c1); break;
    case DrawChop: drawChop(a, t, e, c0, c1); break;
    case DrawSteps: drawSteps(a, t, e, c0, c1, lv); break;
    case DrawGrid: drawGridTools(a, t, e, c0, c1); break;
    case DrawSound: drawSound(a, t, c0, c1); break;
    default: drawLoops(a, t, e, c0, c1); break;
  }
}

}  // namespace gl::packs::seltmann::sui
