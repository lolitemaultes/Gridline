// Gridline — "seltmann" pack: the editor's layout pickers and the keys strip.
//   layout strip   Standard: tabs (Starred, Recent, the kinds, Mine), search, a row of cards that
//                  show the regions each layout plays on this chop; paged with the arrows
//   library        Full: the same layouts as a list in groups beside the ribbon, with counts, stars,
//                  a play button per row, and the field that saves the current layout
//   picker         Compact: one button with the layout's name and a list under it
// A quick click keeps a layout (one undo step) and plays it; holding a card (or the play button)
// plays it on the current chop without changing anything (the setting "play while the pointer
// rests" plays on hover). "Custom (yours)" keeps the hand-made sequence a layout replaced.
//   keys strip     Full: the pattern octave and a key per region, sixteen to a page in the order a
//                  controller plays them; a click plays with the velocity of its height, the wheel sets
//                  the pitch, right-click opens the region menu, a drag puts the region on a step
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Settings.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The groups of the strip's tabs and the library: Starred, Recent, then the PresetTab kinds.
enum Group { GroupStarred = 0, GroupRecent, GroupKinds };
constexpr int kGroups = GroupKinds + TabCount;

const char* groupName(int g) {
  static const char* const n[kGroups] = {i18n::N_("Starred"), i18n::N_("Recent"), i18n::N_("Flip"),     i18n::N_("Phrase"),
                                         i18n::N_("Breaks"),  i18n::N_("Micro"),  i18n::N_("Creative"), i18n::N_("Mine")};
  return g >= 0 && g < kGroups ? n[g] : "";
}

const char* groupHint(int g) {
  static const char* const n[kGroups] = {i18n::N_("The layouts you starred"),
                                         i18n::N_("The layouts you used last, and your own sequence a layout replaced"),
                                         i18n::N_("Instant flips that work on anything"),
                                         i18n::N_("Bars and phrases re-ordered (A B A C), and loops"),
                                         i18n::N_("Breaks re-cut in eighths and sixteenths, and fills"),
                                         i18n::N_("Tiny grains in new rhythms and melodies"),
                                         i18n::N_("Shuffles, reverses, octave jumps and gates"),
                                         i18n::N_("Your own saved layouts")};
  return g >= 0 && g < kGroups ? n[g] : "";
}

void loadUser(App& a, Ed& e) {
  if (e.userLoaded) return;
  e.user = loadUserPresets(userLayoutDir(a));
  e.userLoaded = true;
}

// The layouts of a group (search: every layout whose name or hint matches).
std::vector<const Preset*> groupList(App& a, Ed& e, int g, const std::string& q) {
  loadUser(a, e);
  std::vector<const Preset*> v;
  if (!q.empty()) {
    for (const auto& p : factoryPresets())
      if (lower(p.name + " " + p.hint).find(q) != std::string::npos) v.push_back(&p);
    for (const auto& p : e.user)
      if (lower(p.name).find(q) != std::string::npos) v.push_back(&p);
    return v;
  }
  const LayoutPrefs& pr = prefs(a, e);
  if (g == GroupStarred) {
    for (const auto& n : pr.stars)
      if (const Preset* p = findLayout(a, e, n)) v.push_back(p);
  } else if (g == GroupRecent) {
    for (const auto& n : pr.recent)
      if (const Preset* p = findLayout(a, e, n)) v.push_back(p);
  } else if (g - GroupKinds == TabMine) {
    for (const auto& p : e.user) v.push_back(&p);
  } else {
    for (const auto& p : factoryPresets())
      if (presetTabOf(p.category) == g - GroupKinds) v.push_back(&p);
  }
  return v;
}

// A small star button at p (16 px). Returns true when clicked.
bool starButton(App& a, Ed& e, const Preset& p, ImVec2 c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  ImGui::SetCursorScreenPos(ImVec2(c.x - 9, c.y - 9));
  const bool clicked = ImGui::InvisibleButton("##star", ImVec2(18, 18));
  const bool hv = ImGui::IsItemHovered();
  const bool on = prefs(a, e).starred(p.name);
  if (hv) ui::Tooltip(on ? i18n::tr("Take the star away") : i18n::tr("Star this layout (it shows under Starred)"));
  ui::drawIcon(dl, on ? ui::Icon::StarFilled : ui::Icon::Star, c, 13, on ? accent() : hv ? P.text : P.textFaint);
  if (clicked) {
    prefs(a, e).toggleStar(p.name);
    savePrefs(a, e);
  }
  return clicked;
}

// The "Custom (yours)" layout: the hand-made sequence the last layout replaced.
const Preset& customPreset() {
  static Preset p;
  p.name = i18n::tr("Custom (yours)");
  p.hint = i18n::tr("Your own sequence from before the last layout");
  return p;
}

void saveLayoutField(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w, float h) {
  ImGui::SetCursorScreenPos(p0);
  ImGui::SetNextItemWidth(w - 70);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, std::max(0.0f, (h - ImGui::GetFontSize()) * 0.5f)));
  const bool enter = ImGui::InputTextWithHint("##spname", i18n::tr("Name for this layout"), e.presetName, sizeof e.presetName,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopStyleVar();
  if ((button("##spsave", ImVec2(p0.x + w - 64, p0.y), ImVec2(64, h), i18n::N_("Save"), false,
              i18n::N_("Save the chop settings and the sequence as your own layout (under Mine)"), -1, true) ||
       enter) &&
      e.presetName[0]) {
    if (saveUserPreset(userLayoutDir(a), e.presetName, e.m, static_cast<float>(paramOf(a, t, SmSwing)), static_cast<int>(std::lround(paramOf(a, t, SmSpeed))))) {
      e.userLoaded = false;
      a.notify(std::string(i18n::tr("Saved the layout ")) + e.presetName);
      e.presetName[0] = 0;
      e.savingLayout = false;
    }
  }
}

}  // namespace

// ------------------------------------------------------------------ K-347 (the Tracker's screens)
int layoutGroupCount() { return kGroups; }
const char* layoutGroupName(int g) { return groupName(g); }
const char* layoutGroupHint(int g) { return groupHint(g); }
std::vector<const Preset*> layoutGroup(App& a, Ed& e, int g, const std::string& query) { return groupList(a, e, g, lower(query)); }
const Preset& customLayout() { return customPreset(); }

bool saveLayoutAs(App& a, const PluginTarget& t, Ed& e, const std::string& name) {
  if (name.empty()) return false;
  if (!saveUserPreset(userLayoutDir(a), name, e.m, static_cast<float>(paramOf(a, t, SmSwing)), static_cast<int>(std::lround(paramOf(a, t, SmSpeed))))) {
    a.notify(i18n::tr("Could not save the layout"), true);
    return false;
  }
  e.userLoaded = false;
  a.notify(std::string(i18n::tr("Saved the layout ")) + name);
  return true;
}

bool deleteUserLayout(App& a, Ed& e, const std::string& name) {
  loadUser(a, e);
  for (const auto& p : e.user)
    if (p.name == name) {
      std::string file;  // (the file name saveUserPreset gave it)
      for (char c : name) file += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
      std::error_code ec;
      const bool gone = fs::remove(fs::path(userLayoutDir(a)) / (file + ".seltmann"), ec);
      e.userLoaded = false;
      LayoutPrefs& pr = prefs(a, e);
      pr.stars.erase(std::remove(pr.stars.begin(), pr.stars.end(), name), pr.stars.end());
      pr.recent.erase(std::remove(pr.recent.begin(), pr.recent.end(), name), pr.recent.end());
      savePrefs(a, e);
      if (gone) a.notify(std::string(i18n::tr("Deleted the layout ")) + name);
      return gone;
    }
  return false;
}

// ------------------------------------------------------------------ shared
std::string userLayoutDir(App& a) { return (fs::path(a.configDir()) / "seltmann" / "layouts").string(); }

LayoutPrefs& prefs(App& a, Ed& e) {
  if (!e.prefsLoaded) {
    e.prefs = loadLayoutPrefs((fs::path(a.configDir()) / "seltmann" / "layout-prefs.txt").string());
    e.prefsLoaded = true;
  }
  return e.prefs;
}

void savePrefs(App& a, Ed& e) {
  if (!saveLayoutPrefs((fs::path(a.configDir()) / "seltmann" / "layout-prefs.txt").string(), e.prefs))
    a.notify(i18n::tr("Could not save the starred layouts"), true);
}

const Preset* findLayout(App& a, Ed& e, const std::string& name) {
  for (const auto& p : factoryPresets())
    if (p.name == name) return &p;
  loadUser(a, e);
  for (const auto& p : e.user)
    if (p.name == name) return &p;
  return nullptr;
}

const std::vector<int>& layoutChips(Ed& e, const Preset& p) {
  if (e.chipsFor != e.data) {
    e.chips.clear();
    e.chipsFor = e.data;
  }
  auto it = e.chips.find(p.name);
  if (it != e.chips.end()) return it->second;
  const Model pv = previewModel(e.m, sampleOf(e).get(), p, e.seed);
  return e.chips[p.name] = stepRegions(pv, 16);
}

float regionChips(ImDrawList* dl, App& a, ImVec2 p0, float maxW, float h, const std::vector<int>& regions, int total) {
  const auto& P = pal();
  const float cw = h, gap = 3;
  float x = p0.x;
  for (size_t i = 0; i < regions.size(); ++i) {
    if (x + cw > p0.x + maxW - (i + 1 < regions.size() ? 18 : 0)) {
      char more[24];
      std::snprintf(more, sizeof more, "+%zu", regions.size() - i);
      dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(x + 1, p0.y + (h - theme::kFontLabel) * 0.5f), P.textFaint, more);
      x += 18;
      break;
    }
    const int k = regions[i];
    const ImVec2 q0(x, p0.y), q1(x + cw, p0.y + h);
    if (k < 0 || k >= std::max(1, total)) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 1.0f), 3);
    } else {
      dl->AddRectFilled(q0, q1, regionColor(k, 0.92f), 3);
      const std::string L = regionName(k);
      textC(dl, a.fontBold, L.size() > 1 ? h * 0.5f : h * 0.68f, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), theme::contrastText(regionColor(k)), L.c_str());
    }
    x += cw + gap;
  }
  return x - p0.x;
}

void layoutGesture(App& a, const PluginTarget& t, Ed& e, const Preset& p, bool clicked, bool previewOnly) {
  constexpr double kHold = 0.3, kRest = 0.45;
  // Click, hold and rest are timed on the interface's clock, as its own clicks and double-clicks
  // are: the wall clock for a person, and a scripted click stays a click on a slow machine (a
  // frame of software OpenGL can take a few hundred ms).
  const double now = ImGui::GetTime();
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (ImGui::IsItemActivated()) {
    e.pressAt = now;
    e.previewHeld = true;
  }
  if (ac) {
    if ((previewOnly || now - e.pressAt >= kHold) && e.previewName != p.name) previewStart(a, t, e, p);
    if (e.previewName == p.name) e.previewSeen = true;
  }
  if (ImGui::IsItemDeactivated()) {
    const bool quick = now - e.pressAt < kHold;
    e.previewHeld = false;
    if (e.previewName == p.name) previewStop(a, t, e);
    if (clicked && quick && !previewOnly) keepLayout(a, t, e, p);
  }
  // the setting: the pointer resting on a card plays it
  static bool hoverPlays = false;
  static double hoverRead = -1;
  if (a.now - hoverRead > 1.0) {  // (read once a second: the setting rarely changes)
    hoverPlays = a.settings.getBool("seltmann.hoverPreview");
    hoverRead = a.now;
  }
  if (hoverPlays && hv && !ac && !e.previewHeld) {
    if (e.hoverName != p.name) e.hoverName = p.name, e.hoverAt = now;
    if (now - e.hoverAt >= kRest && e.previewName != p.name) previewStart(a, t, e, p);
    if (e.previewName == p.name) e.previewSeen = true;
  }
  if (hv && !ac) {
    std::string tip = p.name + "\n" + i18n::tr(p.hint.c_str());
    tip += std::string("\n") + (previewOnly ? i18n::tr("Hold to hear it on your chop.") : i18n::tr("Click to use it. Hold to hear it on your chop first."));
    ui::Tooltip(tip.c_str());
  }
}

// Right-click on the layouts: whether resting the pointer on a card plays it (a setting, for you).
void layoutsMenu(App& a, ImVec2 p0, ImVec2 p1) {
  if (ImGui::IsMouseClicked(1) && ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    ImGui::OpenPopup("##slayoutsmenu");
  if (ImGui::BeginPopup("##slayoutsmenu")) {
    bool on = a.settings.getBool("seltmann.hoverPreview");
    if (ImGui::MenuItem(i18n::tr("Play on hover"), nullptr, &on)) {
      a.settings.set("seltmann.hoverPreview", on);
      a.settings.save(Settings::defaultPath());
    }
    if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("Hear a layout while the pointer rests on it (else hold the mouse on it)"));
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ layout strip (Standard)
void drawLayoutStrip(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  e.cards.clear();
  layoutsMenu(a, p0, ImVec2(p0.x + w, p0.y + 84));
  // tabs, search, caption, paging
  const float tabH = 24;
  float x = p0.x;
  x += inlineLabel(dl, a, ImVec2(x, p0.y), tabH, i18n::N_("LAYOUTS"));
  const float tabsW = std::min(520.0f, w * 0.46f);
  {
    static const char* labels[kGroups];
    static const char* hints[kGroups];
    for (int g = 0; g < kGroups; ++g) labels[g] = groupName(g), hints[g] = groupHint(g);
    int cur = e.search[0] ? -1 : e.presetTab;
    if (pills(a, "##sptab", ImVec2(x, p0.y), tabsW, tabH, labels, kGroups, cur, hints) && cur >= 0) {
      e.presetTab = cur;
      e.search[0] = 0;
      e.cardPage = 0;
    }
    x += tabsW + 8;
  }
  const float searchW = 120;
  ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
  ImGui::SetNextItemWidth(searchW);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, std::max(0.0f, (tabH - ImGui::GetFontSize()) * 0.5f)));
  if (ImGui::InputTextWithHint("##ssearch", i18n::tr("Search"), e.search, sizeof e.search)) e.cardPage = 0;
  ImGui::PopStyleVar();
  if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("Search layouts by name or what they do"));
  x += searchW + 10;
  const float rx = p0.x + w;
  const bool prevBtn = button("##spgprev", ImVec2(rx - 52, p0.y), ImVec2(24, tabH), "", false, i18n::N_("Earlier layouts"), static_cast<int>(ui::Icon::ChevronLeft));
  const bool nextBtn = button("##spgnext", ImVec2(rx - 24, p0.y), ImVec2(24, tabH), "", false, i18n::N_("More layouts"), static_cast<int>(ui::Icon::Right));
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x, p0.y + 5), rx - 60 - x, P.textFaint,
                      i18n::tr("Hold a card to hear it on your chop; click to keep it. Your own sequence stays under Recent."));
  // cards
  const std::string q = lower(e.search);
  std::vector<const Preset*> list = groupList(a, e, e.presetTab, q);
  const bool custom = e.hasCustom && q.empty() && e.presetTab == GroupRecent;
  if (custom) list.insert(list.begin(), &customPreset());
  const bool mine = q.empty() && e.presetTab == GroupKinds + TabMine;
  const float cy = p0.y + tabH + 6, ch = 54, cardW = 176, gap = 8;
  const int fit = std::max(1, static_cast<int>((w + gap) / (cardW + gap)));
  const int total = static_cast<int>(list.size()) + (mine ? 1 : 0);
  const int maxPage = std::max(0, total - fit);
  if (prevBtn) e.cardPage = std::max(0, e.cardPage - fit);
  if (nextBtn) e.cardPage = std::min(maxPage, e.cardPage + fit);
  e.cardPage = std::clamp(e.cardPage, 0, maxPage);
  const float cw = std::floor((w - gap * (fit - 1)) / fit);
  const int nSl = static_cast<int>(e.m.slices.size());
  for (int i = 0; i < fit && e.cardPage + i < total; ++i) {
    const int idx = e.cardPage + i;
    const ImVec2 q0(std::round(p0.x + i * (cw + gap)), cy), q1(std::round(q0.x + cw), cy + ch);
    ImGui::PushID(idx);
    if (idx >= static_cast<int>(list.size())) {  // Mine: save the current layout
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##savecard", ImVec2(cw, ch))) {
        e.savingLayout = true;
        ImGui::OpenPopup("##ssavepop");
      }
      const bool hv = ImGui::IsItemHovered();
      dl->AddRect(q0, q1, hv ? accent() : P.line, theme::kRadiusLg, 0, 1.0f);
      textC(dl, a.fontRegular, theme::kFontCaption, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), hv ? P.text : P.textDim, i18n::tr("+ Save this layout"));
      if (ImGui::BeginPopup("##ssavepop")) {
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        saveLayoutField(a, t, e, sp, 300, 26);
        ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + 30));
        ImGui::Dummy(ImVec2(300, 1));
        if (!e.savingLayout) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
      continue;
    }
    const Preset& p = *list[static_cast<size_t>(idx)];
    const bool isCustom = custom && idx == 0;
    ImGui::SetCursorScreenPos(q0);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
    const bool hv = ImGui::IsItemHovered();
    if (isCustom) {
      if (clicked) restoreCustom(a, t, e);
      if (hv) ui::Tooltip(i18n::tr("Your own sequence from before the last layout. Click to have it back."));
    } else {
      layoutGesture(a, t, e, p, clicked);
    }
    const bool cur = !isCustom && p.name == e.m.preset;
    const bool previewing = !isCustom && p.name == e.previewName;
    const ImU32 bg = cur ? theme::mix(P.surface700, accent(), 0.14f) : hv ? P.surface700 : P.surface800;
    skin::raisedFace(dl, q0, q1, bg, theme::kRadiusLg);
    dl->AddRect(q0, q1, cur || previewing ? accent() : hv ? theme::withAlpha(accent(), 0.5f) : P.line, theme::kRadiusLg, 0, cur || previewing ? 1.5f : 1.0f);
    theme::textEllipsis(dl, a.fontBold, theme::kFontCaption + 1, ImVec2(q0.x + 10, q0.y + 7), cw - 40, isCustom ? P.text : P.text, p.name.c_str());
    if (previewing) {
      // what is heard: three small bars
      for (int k = 0; k < 3; ++k) {
        const float hh = 4.0f + 3.0f * ((k + static_cast<int>(a.now * 6)) % 3);
        dl->AddRectFilled(ImVec2(q1.x - 42 + k * 5, q0.y + 18 - hh), ImVec2(q1.x - 39 + k * 5, q0.y + 18), accent(), 1);
      }
    }
    if (!isCustom) starButton(a, e, p, ImVec2(q1.x - 14, q0.y + 14));
    const std::vector<int> chips = isCustom ? stepRegions(e.custom, 16) : layoutChips(e, p);
    regionChips(dl, a, ImVec2(q0.x + 10, q0.y + 28), cw - 20, 16, chips, isCustom ? static_cast<int>(e.custom.slices.size()) : nSl);
    const ImVec2 cc((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f);
    if (!isCustom) e.cards.push_back({p.name, cc});
    ImGui::PopID();
  }
  if (list.empty() && !mine)
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 4, cy + 18), P.textDim,
                e.presetTab == GroupStarred && q.empty() ? i18n::tr("Star a layout and it shows here.") : i18n::tr("No layout matches"));
}

// ------------------------------------------------------------------ library (Full)
void drawLibrary(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  e.cards.clear();
  layoutsMenu(a, p0, p1);
  skin::insetField(dl, p0, p1, P.surface900, theme::kRadiusLg);
  const float w = p1.x - p0.x;
  // search
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 8));
  ImGui::SetNextItemWidth(w - 16);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
  ImGui::InputTextWithHint("##slsearch", i18n::tr("Search layouts"), e.search, sizeof e.search);
  ImGui::PopStyleVar();
  const float saveH = 30;
  const float ly0 = p0.y + 42, ly1 = p1.y - saveH - 14;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 2, ly0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##slib", ImVec2(w - 4, ly1 - ly0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(2);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float cw = ImGui::GetContentRegionAvail().x;
  const ImVec2 clip0 = ImGui::GetWindowPos(), clip1(clip0.x + ImGui::GetWindowWidth(), clip0.y + ImGui::GetWindowHeight());
  float y = o.y;
  const std::string q = lower(e.search);
  const int nSl = static_cast<int>(e.m.slices.size());
  const float rowH = 30, headH = 24;
  auto row = [&](const Preset& p, int id, bool isCustom) {
    const ImVec2 r0(o.x + 4, y), r1(o.x + cw - 4, y + rowH - 2);
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##lrow", ImVec2(r1.x - r0.x, r1.y - r0.y));
    const bool hv = ImGui::IsItemHovered();
    if (isCustom) {
      if (clicked) restoreCustom(a, t, e);
      if (hv) ui::Tooltip(i18n::tr("Your own sequence from before the last layout. Click to have it back."));
    } else {
      layoutGesture(a, t, e, p, clicked);
    }
    const bool cur = !isCustom && p.name == e.m.preset;
    const bool previewing = !isCustom && p.name == e.previewName;
    if (cur || hv || previewing) cl->AddRectFilled(r0, r1, cur ? theme::mix(P.surface700, accent(), 0.18f) : P.surface700, theme::kRadius);
    if (cur || previewing) cl->AddRectFilled(ImVec2(r0.x, r0.y + 3), ImVec2(r0.x + 3, r1.y - 3), accent(), 1);
    const float nameW = std::min(110.0f, (r1.x - r0.x) * 0.45f);
    theme::textEllipsis(cl, cur ? a.fontBold : a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 10, r0.y + 7), nameW, P.text, p.name.c_str());
    const std::vector<int> chips = isCustom ? stepRegions(e.custom, 8) : layoutChips(e, p);
    const float chipX = r0.x + 14 + nameW;
    regionChips(cl, a, ImVec2(chipX, r0.y + 7), r1.x - 52 - chipX, 14, chips, isCustom ? static_cast<int>(e.custom.slices.size()) : nSl);
    if (!isCustom) {
      starButton(a, e, p, ImVec2(r1.x - 38, (r0.y + r1.y) * 0.5f));
      // play: hold to hear it (keeps nothing)
      ImGui::SetCursorScreenPos(ImVec2(r1.x - 24, r0.y + 3));
      ImGui::InvisibleButton("##lplay", ImVec2(20, r1.y - r0.y - 6));
      const bool ph = ImGui::IsItemHovered();
      layoutGesture(a, t, e, p, false, true);
      ui::drawIcon(cl, previewing ? ui::Icon::SpeakerWaves : ui::Icon::Play, ImVec2(r1.x - 14, (r0.y + r1.y) * 0.5f), 11, previewing ? accent() : ph ? P.text : P.textFaint);
    }
    const ImVec2 cc((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
    if (!isCustom && cc.y > clip0.y && cc.y < clip1.y) e.cards.push_back({p.name, ImVec2(r0.x + 40, cc.y)});
    ImGui::PopID();
    y += rowH;
  };
  int id = 0;
  if (!q.empty()) {
    const auto list = groupList(a, e, 0, q);
    for (const Preset* p : list) row(*p, id++, false);
    if (list.empty()) cl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(o.x + 8, y + 6), P.textDim, i18n::tr("No layout matches"));
    if (list.empty()) y += rowH;
  } else {
    for (int g = 0; g < kGroups; ++g) {
      std::vector<const Preset*> list = groupList(a, e, g, q);
      const bool withCustom = g == GroupRecent && e.hasCustom;
      const int count = static_cast<int>(list.size()) + (withCustom ? 1 : 0);
      // group header: chevron, name, count
      ImGui::PushID(1000 + g);
      ImGui::SetCursorScreenPos(ImVec2(o.x, y));
      if (ImGui::InvisibleButton("##lgroup", ImVec2(cw, headH))) e.libOpen[g] = !e.libOpen[g];
      const bool hv = ImGui::IsItemHovered();
      if (hv) ui::Tooltip(i18n::tr(groupHint(g)));
      ImGui::PopID();
      ui::drawIcon(cl, e.libOpen[g] ? ui::Icon::ChevronDown : ui::Icon::Right, ImVec2(o.x + 12, y + headH * 0.5f), 10, hv ? P.text : P.textDim);
      ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
      std::string up = i18n::tr(groupName(g));
      for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      cl->AddText(f, theme::kFontLabel, ImVec2(o.x + 22, y + 6), hv ? P.text : P.textDim, up.c_str());
      char cb[16];
      std::snprintf(cb, sizeof cb, "%d", count);
      textR(cl, a.fontMono, theme::kFontLabel, ImVec2(o.x + cw - 10, y + 6), P.textFaint, cb);
      y += headH;
      if (!e.libOpen[g]) continue;
      if (withCustom) row(customPreset(), id++, true);
      for (const Preset* p : list) row(*p, id++, false);
      if (count == 0) {
        cl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(o.x + 22, y + 2), P.textFaint,
                    g == GroupStarred ? i18n::tr("Star a layout and it shows here") : g == GroupRecent ? i18n::tr("The layouts you use show here") : i18n::tr("Save a layout below"));
        y += 20;
      }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, y));
  ImGui::Dummy(ImVec2(1, 4));
  ImGui::EndChild();
  // save the current layout
  saveLayoutField(a, t, e, ImVec2(p0.x + 8, p1.y - saveH - 8), w - 16, saveH);
}

// ------------------------------------------------------------------ picker (Compact)
void drawLayoutPicker(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  e.cards.clear();
  ImGui::SetCursorScreenPos(p0);
  if (ImGui::InvisibleButton("##spick", ImVec2(w, h))) ImGui::OpenPopup("##spickpop");
  const bool hv = ImGui::IsItemHovered();
  if (hv) ui::Tooltip(i18n::tr("The layout: pick another one (hold the play button of a row to hear it first)"));
  const ImVec2 p1(p0.x + w, p0.y + h);
  skin::raisedFace(dl, p0, p1, hv ? P.surface700 : P.surface800, theme::kRadius);
  dl->AddRect(p0, p1, P.line, theme::kRadius);
  const std::string cur = e.m.preset.empty() ? std::string(i18n::tr("Your sequence")) : e.m.preset;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 9, p0.y + (h - theme::kFontCaption) * 0.5f - 1), w - 30, P.text, cur.c_str());
  ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p1.x - 12, (p0.y + p1.y) * 0.5f), 10, P.textDim);
  e.cards.push_back({cur, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f)});
  ImGui::SetNextWindowSizeConstraints(ImVec2(260, 0), ImVec2(320, 420));
  if (ImGui::BeginPopup("##spickpop")) {
    const int nSl = static_cast<int>(e.m.slices.size());
    int id = 0;
    for (int g = 0; g < kGroups; ++g) {
      const auto list = groupList(a, e, g, "");
      if (list.empty() && !(g == GroupRecent && e.hasCustom)) continue;
      ImGui::TextDisabled("%s", i18n::tr(groupName(g)));
      if (g == GroupRecent && e.hasCustom && ImGui::Selectable(customPreset().name.c_str())) restoreCustom(a, t, e);
      for (const Preset* p : list) {
        ImGui::PushID(id++);
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const bool sel = ImGui::Selectable("##pickrow", p->name == e.m.preset, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 22));
        const float rw = ImGui::GetItemRectSize().x;
        ImDrawList* pl = ImGui::GetWindowDrawList();
        pl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 2, r0.y + 4), P.text, p->name.c_str());
        regionChips(pl, a, ImVec2(r0.x + 120, r0.y + 4), rw - 150, 14, layoutChips(e, *p), nSl);
        ImGui::SetCursorScreenPos(ImVec2(r0.x + rw - 22, r0.y + 1));
        ImGui::InvisibleButton("##pickplay", ImVec2(20, 20));
        layoutGesture(a, t, e, *p, false, true);
        ui::drawIcon(pl, p->name == e.previewName ? ui::Icon::SpeakerWaves : ui::Icon::Play, ImVec2(r0.x + rw - 12, r0.y + 11), 10,
                     p->name == e.previewName ? accent() : P.textDim);
        if (sel) {
          keepLayout(a, t, e, *p);
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
      }
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ keys strip (Full)
float clickVelocity(float y0, float y1, float y) {
  const float f = std::clamp((y - y0) / std::max(1.0f, y1 - y0), 0.0f, 1.0f);
  return 0.45f + 0.55f * f;
}

void drawKeys(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int n = static_cast<int>(e.m.slices.size());
  const int base = baseNote(a, t);
  const bool slotsMap = static_cast<int>(std::lround(paramOf(a, t, SmKeyMap))) == KeySlots;
  const float h = p1.y - p0.y;
  float x = p0.x;
  x += inlineLabel(dl, a, ImVec2(x, p0.y), h, i18n::N_("KEYS"));
  // the pattern octave: the pattern key, then +1 .. +11 (the pattern transposed)
  const float patW = 64, plusW = 24, gap = 2;
  for (int k = 0; k < 12; ++k) {
    const float w = k == 0 ? patW : plusW;
    const ImVec2 q0(x, p0.y), q1(x + w, p1.y);
    const int key = base - 12 + k;
    ImGui::PushID(0x5E0300 + k);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##pk", ImVec2(w, h));
    const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
    if (ImGui::IsItemActivated() && t.isChannel() && key >= 0) {
      if (k == 0) togglePattern(a, t, e);
      else a.engine.ownNoteOn(t.owner, key, clickVelocity(q0.y, q1.y, ImGui::GetIO().MousePos.y));
    }
    if (ImGui::IsItemDeactivated() && k > 0 && t.isChannel() && key >= 0) a.engine.noteOff(t.owner, key);
    if (hv) {
      char b[160];
      if (k == 0) std::snprintf(b, sizeof b, "%s (%s)\n%s", i18n::tr("The pattern"), noteName(key).c_str(), i18n::tr("Click to play or stop the sequence"));
      else std::snprintf(b, sizeof b, "%s +%d (%s)\n%s", i18n::tr("The pattern"), k, noteName(key).c_str(), i18n::tr("Hold to play the sequence this many semitones up"));
      ui::Tooltip(b);
    }
    const bool on = k == 0 ? e.pattern.on : ac;
    skin::raisedFace(dl, q0, q1, on ? accent() : hv ? P.surface600 : P.surface700, 3);
    const ImU32 ink = on ? theme::contrastText(accent()) : P.textDim;
    if (k == 0) {
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q0.x + 5, q0.y + 4), ink, i18n::tr("PATTERN"));
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q0.x + 5, q1.y - 15), ink, noteName(key).c_str());
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "+%d", k);
      textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2((q0.x + q1.x) * 0.5f, q1.y - 9), ink, b);
    }
    ImGui::PopID();
    x += w + gap;
  }
  x += 6;
  // the regions, sixteen to a page (controller order: the first page from the base note up)
  constexpr int per = 16;
  const int pages = std::max(1, (n + per - 1) / per);
  e.keyPage = std::clamp(e.keyPage, 0, pages - 1);
  const float pagerW = pages > 1 ? 52.0f : 0.0f;
  const float kw = std::floor((p1.x - x - pagerW - gap * (per - 1)) / per);
  e.padChips.clear();
  for (int i = 0; i < per; ++i) {
    const int k = e.keyPage * per + i;
    const ImVec2 q0(x + i * (kw + gap), p0.y), q1(q0.x + kw, p1.y);
    if (k >= n) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 0.5f), 3);
      continue;
    }
    const auto& s = e.m.slices[static_cast<size_t>(k)];
    const bool sel = k == e.selSlice, playing = k == lv.slice;
    const std::string L = regionName(k);
    ImGui::PushID(k);
    ImGui::SetCursorScreenPos(q0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##key", ImVec2(kw, h));
    const bool hv = ImGui::IsItemHovered();
    wheelPitch(a, t, e, k);
    if (ImGui::IsItemActivated()) {
      e.selSlice = k;
      auditionStart(a, t, e, k, clickVelocity(q0.y, q1.y, ImGui::GetIO().MousePos.y));
    }
    if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
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
    const int slot = sliceSlot(e.m, k);
    const int key = slotsMap ? (slot >= 0 ? base + slot : -1) : base + k;
    if (hv && !ImGui::IsItemActive()) {
      char b[240];
      std::snprintf(b, sizeof b, "%s %s  \xC2\xB7  %s %s  \xC2\xB7  %.3f s\n%s", i18n::tr("Region"), L.c_str(), i18n::tr("key"),
                    key >= 0 && key <= 127 ? noteName(key).c_str() : "-", static_cast<double>(s.end - s.start) / std::max(1.0, e.m.srcRate),
                    i18n::tr("Click to play (lower is louder). Wheel: pitch. Right-click: more. Drag onto a step to play it there."));
      ui::Tooltip(b);
    }
    const ImU32 face = playing ? regionColor(k) : regionFace(k, hv ? 0.42f : sel ? 0.36f : 0.28f);
    skin::raisedFace(dl, q0, q1, face, 3);
    if (!playing) dl->AddRectFilled(ImVec2(q0.x + 1, q1.y - 3), ImVec2(q1.x - 1, q1.y - 1), regionColor(k, 0.95f), 1.0f, ImDrawFlags_RoundCornersBottom);
    if (sel && !playing) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), accent(), 4, 0, 1.5f);
    const ImU32 ink = playing ? theme::contrastText(regionColor(k)) : P.text;
    textC(dl, a.fontBold, 13, ImVec2((q0.x + q1.x) * 0.5f, q0.y + 11), ink, L.c_str());
    if (key >= 0 && key <= 127 && kw >= 26)
      textC(dl, a.fontMono, theme::kFontLabel - 2, ImVec2((q0.x + q1.x) * 0.5f, q1.y - 10), theme::withAlpha(ink, 0.7f), noteName(key).c_str());
    // the pitch readout of a pitched key (and the key under the pointer): drag it up or down
    const bool over = ImGui::IsMouseHoveringRect(q0, q1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const float chipY = q0.y + 19, chipH = 13;
    if (kw >= 30 && (std::fabs(s.pitch) >= 0.005f || over || e.pitchDrag == k))
      pitchChip(a, t, e, "##kpitch", ImVec2(q0.x + 3, chipY), chipH, kw - 6, k, false);
    if (e.padChips.size() < 16) e.padChips.push_back(ImVec2(q0.x + 10, chipY + chipH * 0.5f));
    ImGui::PopID();
  }
  if (pages > 1) {
    const float bx = p1.x - pagerW + 4;
    if (button("##kprev", ImVec2(bx, p0.y), ImVec2(22, h), "", false, i18n::N_("The regions before"), static_cast<int>(ui::Icon::ChevronLeft)))
      e.keyPage = std::max(0, e.keyPage - 1);
    if (button("##knext", ImVec2(bx + 24, p0.y), ImVec2(22, h), "", false, i18n::N_("The next sixteen regions"), static_cast<int>(ui::Icon::Right)))
      e.keyPage = std::min(pages - 1, e.keyPage + 1);
  }
}

}  // namespace gl::packs::seltmann::sui
