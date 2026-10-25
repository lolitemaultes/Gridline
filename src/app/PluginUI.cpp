#include "../core/I18n.h"
#include "HostedUi.h"
#include "MidiUi.h"
#include "PluginUI.h"
#include "../core/PluginBrand.h"
#include "../core/PluginStandIn.h"
#include "../core/Presets.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <complex>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Json.h"
#include "../core/Project.h"
#include "App.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "LayoutAudit.h"
#include "PluginSkin.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

bool drawCustomEditor(App& a, const PluginTarget& t, float width);  // PluginEditors.cpp

const PluginState* PluginTarget::state(const Song& s) const {
  if (isChannel()) {
    const Channel* c = s.channel(owner);
    return c ? &c->plugin : nullptr;
  }
  const MixerTrack* m = s.mixerTrack(owner);
  if (!m || slot >= static_cast<int>(m->slots.size())) return nullptr;
  const PluginState& st = m->slots[static_cast<size_t>(slot)].plugin;
  return st.empty() ? nullptr : &st;
}

const PluginInfo* PluginTarget::info(const Song& s) const {
  const PluginState* st = state(s);
  return st ? findPlugin(st->type) : nullptr;
}

const ScopeTap* PluginTarget::tap(App& a) const {
  if (isChannel()) {
    auto rt = a.builder.channelRuntime(owner);
    return rt ? &rt->tap : nullptr;
  }
  auto rt = a.builder.effectRuntime(owner, slot);
  return rt ? &rt->tap : nullptr;
}

const ScopeTap* PluginTarget::inputTap(App& a) const {
  if (isChannel()) return nullptr;
  auto rt = a.builder.effectRuntime(owner, slot);
  return rt ? &rt->inTap : nullptr;
}

void setPluginState(App& a, const PluginTarget& t, const PluginState& st0, const std::string& label, uint64_t mergeKey) {
  PluginState st = st0;
  if (const PluginState* cur = t.state(a.doc.song()); cur && st.original.empty() && !cur->original.empty() && cur->type == st.type)
    st.original = cur->original;  // a stand-in stays one (PluginStandIn.h)
  if (t.isChannel())
    a.doc.editChannel(t.owner, label, [&](Channel& c) { c.plugin = st; }, mergeKey);
  else
    a.doc.editMixerTrack(t.owner, label, [&](MixerTrack& m) { m.slots[static_cast<size_t>(t.slot)].plugin = st; }, mergeKey);
}

void keepStandIn(App& a, const PluginTarget& t) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur || cur->original.empty()) return;
  const std::string label = std::string("Replace with ") + (findPlugin(cur->type) ? findPlugin(cur->type)->name : cur->type);
  if (t.isChannel())
    a.doc.editChannel(t.owner, label, [&](Channel& c) { c.plugin.original.clear(); });
  else
    a.doc.editMixerTrack(t.owner, label, [&](MixerTrack& m) { m.slots[static_cast<size_t>(t.slot)].plugin.original.clear(); });
}

void drawStandInNote(App& a, const PluginTarget& t, float width) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur || cur->original.empty()) return;
  const auto& P = pal();
  const PluginState st = *cur;
  const std::string what = standin::describe(st);
  const PluginInfo* info = findPlugin(st.type);
  const std::string keep = std::string(i18n::tr("Replace with")) + " " + (info ? info->name : st.type);
  ImGui::PushID("standin");
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
  ImGui::TextUnformatted(what.c_str());
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    standin::Original o;
    standin::fromText(st.original, o);
    std::string tip = i18n::tr("The plugin and its own settings are kept unchanged: the project opens with it again once it is installed.");
    if (!o.format.empty()) tip += std::string("\n\n") + i18n::tr("Read from") + ": " + o.format;
    if (!o.notes.empty()) {
      tip += std::string("\n\n") + i18n::tr("Approximated") + ":";
      for (const auto& n : o.notes) tip += "\n  - " + n;
    }
    ImGui::SetTooltip("%s", tip.c_str());
  }
  const float bw = ImGui::CalcTextSize(keep.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), width - bw - 4));
  if (ImGui::SmallButton(keep.c_str())) keepStandIn(a, t);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::tr("Keep the built-in plugin for good: the missing plugin and its saved settings are removed from the project"));
  ImGui::PopID();
}

// ------------------------------------------------------------------ presets

namespace {

// Presets on disk (core/Presets.h): "<config>/presets/<type>/<folders…>/<name>.json".
std::string presetsRoot(App& a) { return presetRoot(a.configDir()); }

// Listing is cached per type and refreshed whenever a preset menu opens.
const std::vector<PresetEntry>& cachedPresets(App& a, const std::string& type, bool refresh) {
  static std::map<std::string, std::vector<PresetEntry>> cache;
  auto it = cache.find(type);
  if (refresh || it == cache.end()) it = cache.insert_or_assign(type, listPresets(presetsRoot(a), type)).first;
  return it->second;
}

bool saveUserPreset(App& a, const PluginState& st, const std::string& name) {
  bool ok = savePreset(presetsRoot(a), st, name);
  cachedPresets(a, st.type, true);
  return ok;
}

// Folder tree of presets for nested menus.
struct PresetNode {
  std::map<std::string, PresetNode> kids;
  std::vector<int> items;  // indices into the combined list
};

struct HeaderState {
  std::string presetName;
  int presetIndex = -1;  // index into factory + user list
  PluginState spare;     // A/B compare
  bool onB = false;
};
std::map<uint64_t, HeaderState> headers;

}  // namespace

namespace pui {
void compareStore(App& a, const PluginTarget& t) {
  if (const PluginState* st = t.state(a.doc.song())) headers[t.key()].spare = *st;
}
void compareFlip(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return;
  HeaderState& h = headers[t.key()];
  if (h.spare.type != st->type) h.spare = *st;
  PluginState cur = *st;
  setPluginState(a, t, h.spare, "compare A/B");
  h.spare = cur;
  h.onB = !h.onB;
}
bool compareOnB(const PluginTarget& t) {
  auto it = headers.find(t.key());
  return it != headers.end() && it->second.onB;
}
}  // namespace pui

namespace pui {
uint32_t brandRole(BrandFamily f) {
  switch (f) {
    case BrandFamily::Synth: return theme::role::kStamp;
    case BrandFamily::Sample: return theme::role::kSlice;
    case BrandFamily::Space: return theme::role::kGraph;
    case BrandFamily::Dynamics: return theme::role::kMenu;
    case BrandFamily::Voice: return theme::role::kSnap;
    case BrandFamily::Structure: return theme::role::kPlay;
    case BrandFamily::None: break;
  }
  return 0;
}

bool wordmark(App& a, const std::string& type, float size) {
  const PluginBrand* b = pluginBrand(type);
  const PluginInfo* info = findPlugin(type);
  if (!b || b->family == BrandFamily::None || !info || !a.fontBold) return false;
  // Lower-case name in Inter Bold, tracked like the Gridline lockup
  // (-3 units at 104), after a family accent bar.
  std::string word = info->name;
  for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const float track = -size * 3.0f / 104.0f, barW = 3.0f, gap = 7.0f;
  ImFontBaked* fb = a.fontBold->GetFontBaked(size);
  float w = 0;
  for (size_t i = 0; i < word.size(); ++i) w += fb->GetCharAdvance(static_cast<ImWchar>(word[i])) + (i + 1 < word.size() ? track : 0.0f);
  const float h = std::max(ImGui::GetFrameHeight(), size + 4.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##wordmark", ImVec2(barW + gap + w, h));
  if (ImGui::IsItemHovered() && b->tagline[0]) ui::Hint(info->name + "  \xC2\xB7  " + b->tagline);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 accent = theme::litRole(brandRole(b->family));
  dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + barW, p.y + h * 0.8f), accent);
  float x = p.x + barW + gap;
  const float top = p.y + (h - size) * 0.5f;
  for (char c : word) {
    const char one[2] = {c, 0};
    dl->AddText(a.fontBold, size, ImVec2(x, top), pal().text, one);
    x += fb->GetCharAdvance(static_cast<ImWchar>(c)) + track;
  }
  return true;
}
}  // namespace pui

void drawPluginHeader(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  const PluginState* st = t.state(s);
  const PluginInfo* info = t.info(s);
  if (!st || !info) return;
  const auto& P = pal();
  HeaderState& h = headers[t.key()];
  ImGui::PushID(static_cast<int>(t.key()));

  // Combined preset list: factory, then the preset library (your presets at
  // the top level, then folders such as "FL Studio/<plugin>/…").
  std::vector<std::string> names;
  for (const auto& p : info->presets) names.push_back(p.name);
  const size_t factoryCount = names.size();
  const bool menuOpening = ImGui::IsPopupOpen("##preset");
  const auto& lib = cachedPresets(a, info->type, false);
  for (const auto& e : lib) names.push_back(e.folder.empty() ? e.name : e.folder + "/" + e.name);
  auto apply = [&](int idx) {
    if (idx < 0 || idx >= static_cast<int>(names.size())) return;
    if (static_cast<size_t>(idx) < factoryCount && isHostedType(info->type)) {  // hosted: the plugin's own programs
      if (!selectHostedProgram(a, t, idx)) return;
      h.presetIndex = idx;
      h.presetName = names[static_cast<size_t>(idx)];
      return;
    }
    PluginState ns;
    if (static_cast<size_t>(idx) < factoryCount) {
      ns = presetState(*info, static_cast<size_t>(idx));
      ns.data = st->data;  // factory presets keep the loaded sample
    } else {
      if (!loadPreset(lib[static_cast<size_t>(idx) - factoryCount].file, info->type, ns)) return;
      if (ns.data.empty()) ns.data = st->data;  // presets without their own data keep the current one
    }
    setPluginState(a, t, ns, "load preset");
    h.presetIndex = idx;
    h.presetName = static_cast<size_t>(idx) < factoryCount ? names[static_cast<size_t>(idx)] : lib[static_cast<size_t>(idx) - factoryCount].name;
  };
  (void)menuOpening;

  if (!pui::wordmark(a, info->type, 18.0f)) {  // J-94: flagships show their wordmark
    ImGui::PushFont(a.fontBold, 15.0f);
    ImGui::TextUnformatted(info->name.c_str());
    ImGui::PopFont();
  }
  ImGui::SameLine(0, 14);
  if (ui::IconButton("prev", ui::Icon::Left, false, 22, "Previous preset") && !names.empty())
    apply(h.presetIndex <= 0 ? static_cast<int>(names.size()) - 1 : h.presetIndex - 1);
  ImGui::SameLine(0, 2);
  std::string label = h.presetName.empty() ? "Default" : h.presetName;
  if (ui::BeginCombo("##preset", label.c_str(), 170)) {
    static char search[96] = "";
    if (ImGui::IsWindowAppearing()) {
      cachedPresets(a, info->type, true);  // pick up presets saved or imported meanwhile
      search[0] = 0;
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##psearch", i18n::tr("Search presets"), search, sizeof search);
    std::string q = search;
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto selectable = [&](int i, const std::string& text) {
      if (ImGui::Selectable((text + "##p" + std::to_string(i)).c_str(), i == h.presetIndex)) apply(i);
    };
    if (!q.empty()) {  // flat, filtered
      int shown = 0;
      for (size_t i = 0; i < names.size() && shown < 400; ++i) {
        std::string l = names[i];
        for (char& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (l.find(q) == std::string::npos) continue;
        selectable(static_cast<int>(i), names[i]);
        ++shown;
      }
      if (!shown) ImGui::TextDisabled("%s", i18n::tr("No presets match"));
    } else {
      if (ImGui::Selectable(i18n::tr("Default"), h.presetName.empty())) {
        PluginState ns = defaultPluginState(info->type);
        ns.data = st->data;
        setPluginState(a, t, ns, "reset plugin");
        h.presetIndex = -1;
        h.presetName.clear();
      }
      if (factoryCount) ImGui::SeparatorText(i18n::tr("Factory"));
      for (size_t i = 0; i < factoryCount; ++i) selectable(static_cast<int>(i), names[i]);
      PresetNode root;
      for (size_t k = 0; k < lib.size(); ++k) {
        PresetNode* n = &root;
        std::string f = lib[k].folder;
        size_t st0 = 0;
        while (!f.empty() && st0 <= f.size()) {
          size_t e = f.find('/', st0);
          n = &n->kids[f.substr(st0, e == std::string::npos ? std::string::npos : e - st0)];
          if (e == std::string::npos) break;
          st0 = e + 1;
        }
        n->items.push_back(static_cast<int>(factoryCount + k));
      }
      if (!root.items.empty() || !root.kids.empty()) ImGui::SeparatorText(i18n::tr("Library"));
      std::function<void(const PresetNode&)> tree = [&](const PresetNode& n) {
        for (const auto& [name, kid] : n.kids)
          if (ImGui::BeginMenu(name.c_str())) {
            tree(kid);
            ImGui::EndMenu();
          }
        for (int i : n.items) selectable(i, lib[static_cast<size_t>(i) - factoryCount].name);
      };
      tree(root);
    }
    ImGui::EndCombo();
  }
  ui::Hint("Presets: factory sounds, your saved presets and imported libraries (type to search)");
  ImGui::SameLine(0, 2);
  if (ui::IconButton("next", ui::Icon::Right, false, 22, "Next preset") && !names.empty())
    apply(h.presetIndex + 1 >= static_cast<int>(names.size()) ? 0 : h.presetIndex + 1);
  ImGui::SameLine(0, 2);
  if (ui::IconButton("save", ui::Icon::Save, false, 22, "Save as a user preset")) ImGui::OpenPopup("##savepreset");
  if (ImGui::BeginPopup("##savepreset")) {
    static char buf[96];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof buf, "%s", h.presetName.empty() ? "My preset" : h.presetName.c_str());
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##pn", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) && buf[0]) {
      std::string n = buf;
      for (char& c : n)
        if (c == '/' || c == '\\' || c == ':') c = '-';
      if (saveUserPreset(a, *st, n)) {
        h.presetName = n;
        a.notify("Saved preset " + n);
      } else {
        a.notify("Could not save preset", true);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("%s", i18n::tr("Enter to save"));
    ImGui::EndPopup();
  }

  // A/B compare.
  ImGui::SameLine(0, 12);
  if (h.spare.type != st->type) h.spare = *st;
  if (ui::TextButton("A", !h.onB, 26, "Compare: state A") && h.onB) {
    PluginState cur = *st;
    setPluginState(a, t, h.spare, "compare A/B");
    h.spare = cur;
    h.onB = false;
  }
  ImGui::SameLine(0, 2);
  if (ui::TextButton("B", h.onB, 26, "Compare: state B") && !h.onB) {
    PluginState cur = *st;
    setPluginState(a, t, h.spare, "compare A/B");
    h.spare = cur;
    h.onB = true;
  }
  ImGui::SameLine(0, 2);
  if (ui::IconButton("copyab", ui::Icon::Clone, false, 22, "Copy the current state to the other compare slot")) h.spare = *st;

  // Effects: bypass + mix on the right.
  if (!t.isChannel()) {
    const MixerTrack* m = s.mixerTrack(t.owner);
    bool en = m && m->slots[static_cast<size_t>(t.slot)].enabled;
    float right = ImGui::GetWindowContentRegionMax().x;
    // Narrow windows: wrap bypass + mix onto their own row instead of overlapping A/B.
    float lastX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    if (lastX + 12 <= right - 128) ImGui::SameLine(right - 128);
    else ImGui::SetCursorPosX(right - 128);
    if (ui::TextButton(en ? "On" : "Bypassed", en, 76, "Bypass this effect")) {
      int k = t.slot;
      a.doc.editMixerTrack(t.owner, en ? "bypass effect" : "enable effect",
                           [k](MixerTrack& mt) { mt.slots[static_cast<size_t>(k)].enabled = !mt.slots[static_cast<size_t>(k)].enabled; });
    }
    ImGui::SameLine(0, 6);
    float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y - 2);
    ui::ParamKnob("mix", slotMixParam(t.owner, t.slot), 11.0f);
  }
  browserui::pluginWindowDropTarget(a, t);  // presets / samples from the browser (J-16)
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, 2));
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), P.line);
  ImGui::Dummy(ImVec2(0, 6));
}

// --------------------------------------------------------------- building blocks

namespace pui {

void beginSection(const char* title, float width, float height) {
  const auto& P = pal();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(theme::mix(P.surface800, P.text, 0.07f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::BeginChild(title, ImVec2(width, height),
                    ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders | (height <= 0 ? ImGuiChildFlags_AutoResizeY : 0),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  audit::frame(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y), "section");
  if (const skin::Skin* k = skin::active()) {  // plugin skin: section well + caption pill
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    skin::well(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), *k, 4);
    std::string tt = title;
    if (size_t h = tt.find("##"); h != std::string::npos) tt.resize(h);
    ImGui::PushFont(app().fontMonoBold, 10.5f);
    float tw = ImGui::CalcTextSize(tt.c_str()).x;
    ImGui::PopFont();
    ImVec2 c = ImGui::GetCursorScreenPos();
    skin::pill(dl, ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + std::min(ws.x - 16, tw + 16), c.y + 14), *k, tt.c_str(), 10.5f);
    ImGui::Dummy(ImVec2(0, 14));
    return;
  }
  ImGui::PushFont(app().fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", title);
  ImGui::PopFont();
}

void endSection() {
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

bool knobImpl(App& a, const PluginTarget& t, int param, const char* label, float radius, float cell, bool text);

void knobCell(App& a, const PluginTarget& t, int param, const char* label, float cell, float radius) {
  knobImpl(a, t, param, label, radius, cell, true);
}

void choiceButtons(App& a, const PluginTarget& t, int param, float width) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return;
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  ImGui::PushID(param);
  float w = width > 0 ? (width - (d->choices.size() - 1) * 2.0f) / static_cast<float>(d->choices.size()) : 0;
  for (size_t i = 0; i < d->choices.size(); ++i) {
    if (i) ImGui::SameLine(0, 2);
    if (ui::TextButton(d->choices[i].c_str(), static_cast<int>(i) == cur, w)) a.doc.setParam(pid, static_cast<double>(i), false);
  }
  ImGui::PopID();
}

void toggleButton(App& a, const PluginTarget& t, int param, const char* label) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return;
  bool on = getParam(s, pid).value_or(0) >= 0.5;
  ImGui::PushID(param);
  if (ui::TextButton(label ? label : d->name.c_str(), on)) a.doc.setParam(pid, on ? 0 : 1, false);
  ImGui::PopID();
}

void adsrGraph(App& a, const PluginTarget& t, int att, int dec, int sus, int rel, ImVec2 size) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  auto dA = describeParam(s, t.pid(att)), dD = describeParam(s, t.pid(dec)), dS = describeParam(s, t.pid(sus)), dR = describeParam(s, t.pid(rel));
  if (!dA || !dD || !dS || !dR) return;
  double vA = *getParam(s, t.pid(att)), vD = *getParam(s, t.pid(dec)), vS = *getParam(s, t.pid(sus)), vR = *getParam(s, t.pid(rel));
  ImGui::PushID(att);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##adsr", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.surface950, 6);
  const float pad = 8, seg = (size.x - pad * 2) / 4.0f, top = p.y + pad, bot = p.y + size.y - pad;
  float xA = p.x + pad + static_cast<float>(dA->toNormalized(vA)) * seg;
  float xD = xA + static_cast<float>(dD->toNormalized(vD)) * seg;
  float yS = bot - static_cast<float>(vS) * (bot - top);
  float xS = xD + seg * 0.8f;
  float xR = xS + static_cast<float>(dR->toNormalized(vR)) * seg;
  ImVec2 pts[5] = {ImVec2(p.x + pad, bot), ImVec2(xA, top), ImVec2(xD, yS), ImVec2(xS, yS), ImVec2(xR, bot)};
  for (int i = 0; i < 4; ++i) {
    ImVec2 q[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, bot), ImVec2(pts[i].x, bot)};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.08f));
  }
  dl->AddPolyline(pts, 5, P.text, 0, 2.0f);
  // Handles: 0 attack, 1 decay/sustain, 2 release.
  ImVec2 handles[3] = {pts[1], pts[2], pts[4]};
  static int dragging = -1;
  static ImGuiID dragId = 0;
  ImGuiIO& io = ImGui::GetIO();
  ImGuiID me = ImGui::GetItemID();
  int hover = -1;
  for (int i = 0; i < 3; ++i)
    if (std::hypot(io.MousePos.x - handles[i].x, io.MousePos.y - handles[i].y) < 9) hover = i;
  if (ImGui::IsItemActivated()) {
    dragging = hover;
    dragId = me;
  }
  for (int i = 0; i < 3; ++i)
    dl->AddCircleFilled(handles[i], (i == hover || (dragging == i && dragId == me)) ? 6.0f : 4.5f, i == hover ? P.paper : P.text);
  if (ImGui::IsItemActive() && dragId == me && dragging >= 0) {
    uint64_t key = 0x4144535200000000ull ^ t.key() ^ static_cast<uint64_t>(att);
    PluginState st = *t.state(s);
    auto setN = [&](int k, const ParamDesc& d, double n) { st.params[static_cast<size_t>(k)] = static_cast<float>(d.fromNormalized(std::clamp(n, 0.0, 1.0))); };
    if (dragging == 0) setN(att, *dA, (io.MousePos.x - (p.x + pad)) / seg);
    if (dragging == 1) {
      setN(dec, *dD, (io.MousePos.x - xA) / seg);
      st.params[static_cast<size_t>(sus)] = static_cast<float>(std::clamp((bot - io.MousePos.y) / (bot - top), 0.0f, 1.0f));
    }
    if (dragging == 2) setN(rel, *dR, (io.MousePos.x - xS) / seg);
    setPluginState(a, t, st, "edit envelope", key);
  }
  if (ImGui::IsItemDeactivated()) {
    dragging = -1;
    a.doc.endGesture();
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "A %s · D %s · S %s · R %s   (drag the points)", dA->format(vA).c_str(), dD->format(vD).c_str(),
                  dS->format(vS).c_str(), dR->format(vR).c_str());
    a.setHint(buf);
  }
  ImGui::PopID();
}

float freqToX(double hz, float x0, float w) {
  return x0 + w * static_cast<float>(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(1000.0));
}
double xToFreq(float x, float x0, float w) { return 20.0 * std::pow(1000.0, std::clamp((x - x0) / w, 0.0f, 1.0f)); }

void drawFreqGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double dbRange, bool labels) {
  const auto& P = pal();
  float w = p1.x - p0.x, h = p1.y - p0.y;
  for (double dec : {10.0, 100.0, 1000.0, 10000.0})
    for (int m = 1; m <= 9; ++m) {
      double f = dec * m;
      if (f < 20 || f > 20000) continue;
      float x = freqToX(f, p0.x, w);
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), m == 1 ? P.gridBar : P.gridStep);
      if (labels && (m == 1 || m == 2 || m == 5) && x > p0.x + 30 && x < p1.x - 28) {  // J-93: never past the right edge
        char buf[16];
        if (f >= 1000)
          std::snprintf(buf, sizeof buf, "%gk", f / 1000);
        else
          std::snprintf(buf, sizeof buf, "%g", f);
        ImGui::PushFont(app().fontMono, 10.0f);
        dl->AddText(ImVec2(x + 3, p1.y - 14), P.textFaint, buf);
        ImGui::PopFont();
      }
    }
  for (double db = -dbRange; db <= dbRange + 0.01; db += dbRange / 3) {
    float y = p0.y + h * 0.5f - static_cast<float>(db / dbRange) * h * 0.5f;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), std::fabs(db) < 0.01 ? P.lineStrong : P.gridStep);
    if (labels && std::fabs(db) > 0.01 && std::fabs(db) < dbRange - 0.01) {  // J-93: edge values would sit outside the well
      char buf[16];
      std::snprintf(buf, sizeof buf, "%+.0f", db);
      ImGui::PushFont(app().fontMono, 10.0f);
      dl->AddText(ImVec2(p0.x + 4, std::max(p0.y + 2, y - 12)), P.textFaint, buf);
      ImGui::PopFont();
    }
  }
}

// Iterative radix-2 FFT (n power of two).
static void fft(std::vector<std::complex<float>>& a) {
  size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * 3.14159265f / static_cast<float>(len);
    std::complex<float> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1, 0);
      for (size_t j = 0; j < len / 2; ++j) {
        auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

void spectrum(const ScopeTap* tap, uint64_t key, int points, float* outDb, double sampleRate) {
  static std::map<uint64_t, std::vector<float>> smooth;
  // J-68: the FFT runs only when the tap holds new audio (the write counter
  // moved) or the layout changed; otherwise the last analysis is reused.
  struct Raw {
    size_t written = static_cast<size_t>(-1);
    double sr = 0;
    std::vector<float> db;
  };
  static std::map<uint64_t, Raw> raws;
  auto& sm = smooth[key];
  if (static_cast<int>(sm.size()) != points) sm.assign(static_cast<size_t>(points), -120.f);
  Raw& raw = raws[key];
  const size_t written = tap ? tap->written() : 0;
  if (raw.written != written || raw.sr != sampleRate || static_cast<int>(raw.db.size()) != points) {
    raw.written = written, raw.sr = sampleRate;
    raw.db.assign(static_cast<size_t>(points), -120.f);
    const size_t N = 4096;
    static std::vector<float> buf(N);
    static std::vector<std::complex<float>> c(N);
    if (tap && tap->read(buf.data(), N)) {
      for (size_t i = 0; i < N; ++i) {
        float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(N - 1));  // Hann
        c[i] = std::complex<float>(buf[i] * w, 0);
      }
      fft(c);
      for (int k = 0; k < points; ++k) {
        double f0 = 20.0 * std::pow(1000.0, static_cast<double>(k) / points), f1 = 20.0 * std::pow(1000.0, static_cast<double>(k + 1) / points);
        size_t b0 = static_cast<size_t>(f0 / sampleRate * N), b1 = std::max(b0 + 1, static_cast<size_t>(f1 / sampleRate * N));
        float mag = 0;
        for (size_t b = b0; b < b1 && b < N / 2; ++b) mag = std::max(mag, std::abs(c[b]));
        raw.db[static_cast<size_t>(k)] = 20.0f * std::log10(std::max(mag * 4.0f / static_cast<float>(N), 1e-7f));
      }
    }
  }
  for (int k = 0; k < points; ++k) {
    const float db = raw.db[static_cast<size_t>(k)];
    float& s = sm[static_cast<size_t>(k)];
    s = db > s ? s + (db - s) * 0.6f : s + (db - s) * 0.12f;  // fast rise, slow fall
    outDb[k] = s;
  }
}

// ============================================================ rack-panel kit

namespace {

constexpr float kKnobPi = 3.14159265f;
const float kKnobRadius[3] = {11.0f, 16.0f, 23.0f};
const float kKnobCell[3] = {54.0f, 68.0f, 86.0f};


bool isLinked(App& a, ParamId pid) {
  for (const auto& l : a.doc.song().links)
    if (l.owner == pid.owner && l.index == pid.index) return true;
  return false;
}

// Shared right-click menu for parameter controls.
void paramMenu(App& a, ParamId pid, const ParamDesc& d, bool* wantType) {
  ImGui::TextDisabled("%s", d.name.c_str());
  ImGui::Separator();
  std::string reset = "Reset  (" + d.format(d.def) + ")";
  if (ImGui::MenuItem(reset.c_str())) a.doc.setParam(pid, d.def, false);
  if (ImGui::MenuItem(i18n::tr("Type a value..."))) *wantType = true;
  ImGui::Separator();
  automationMenuItems(a, pid);  // J-5: create / edit automation, edit events, record
  ImGui::Separator();
  remoteLinkMenuItems(a, pid);  // MIDI remote control (MidiUi.cpp)
}

// Type-in popup (opened by the caller with OpenPopup("##type")).
void typePopup(App& a, ParamId pid, const ParamDesc& d) {
  static char buf[64];
  if (ImGui::BeginPopup("##type")) {
    if (ImGui::IsWindowAppearing()) {
      std::string cur = d.format(*getParam(a.doc.song(), pid));
      std::snprintf(buf, sizeof buf, "%s", cur.c_str());
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::TextDisabled("%s", d.name.c_str());
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
      double v;
      if (pui::parseValue(d, buf, v)) a.doc.setParam(pid, v, false);
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("%s", i18n::tr("Enter to apply · Esc to cancel"));
    ImGui::EndPopup();
  }
}

// J-98: a slider's drawn track: the drag maps the pointer along it (see dragParamTrack).
struct Track {
  float start = 0, len = 0;
};
std::map<uint64_t, pui::DevItem>& devItems() {
  static std::map<uint64_t, pui::DevItem> m;
  return m;
}
std::map<uint32_t, pui::DevTrack>& devTracks() {
  static std::map<uint32_t, pui::DevTrack> m;
  return m;
}

// Interaction shared by knobs and sliders: drag (vertical or horizontal), wheel,
// Ctrl+click reset, double-click type, right-click menu. Item must be the last item.
bool dragParam(App& a, ParamId pid, const ParamDesc& d, double& val, float pixels, bool horizontal, const Track* track = nullptr) {
  audit::lastItem("param");
  audit::bindControl(pid);  // J-92 binding audit
  {
    pui::DevItem& di = devItems()[pid.packed()];
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    di = {r0.x, r0.y, r1.x, r1.y, ImGui::GetFrameCount()};
  }
  ImGuiIO& io = ImGui::GetIO();
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  double n = d.toNormalized(val);
  ImGuiID id = ImGui::GetItemID();
  static std::map<ImGuiID, double> accum;  // choice params step through accumulated motion
  // Track sliders: the pointer position and the normalized value at the grab (re-anchored when Shift changes).
  struct Anchor {
    float pos = 0;
    double n = 0;
    bool fine = false;
  };
  static Anchor anchor;
  const float along = horizontal ? io.MousePos.x : io.MousePos.y;
  if (ImGui::IsItemActivated()) {
    accum[id] = n;
    anchor = {along, n, io.KeyShift};
    if (io.KeyCtrl || io.KeySuper) {
      val = d.def;
      a.doc.setParam(pid, val, false);
      return true;
    }
  }
  if (track && track->len > 1 && act && !(io.KeyCtrl || io.KeySuper)) {
    if (io.KeyShift != anchor.fine) anchor = {along, n, io.KeyShift};
    if (along != anchor.pos) {
      const double dir = horizontal ? 1.0 : -1.0;  // vertical: up raises the value
      const double nn = std::clamp(anchor.n + dir * (along - anchor.pos) / track->len * (anchor.fine ? 0.125 : 1.0), 0.0, 1.0);
      const double v = d.fromNormalized(nn);
      if (v != val) {
        val = v;
        a.doc.setParam(pid, val, true);
        changed = true;
      }
    }
  } else if (act && !(io.KeyCtrl || io.KeySuper)) {
    float delta = horizontal ? io.MouseDelta.x : -io.MouseDelta.y;
    if (delta != 0) {
      double speed = (io.KeyShift ? 0.12 : 1.0) / std::max(40.0f, pixels);
      double& acc = accum[id];
      acc = std::clamp(acc + delta * speed, 0.0, 1.0);
      double v = d.fromNormalized(acc);
      if (v != val) {
        val = v;
        a.doc.setParam(pid, val, true);
        changed = true;
      }
    }
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (hov && !act && io.MouseWheel != 0) {
    double step = d.kind == ParamKind::Choice ? 1.0 / std::max<size_t>(1, d.choices.empty() ? 1 : d.choices.size() - 1)
                                              : (io.KeyShift ? 0.005 : 0.025);
    double v = d.fromNormalized(std::clamp(n + io.MouseWheel * step, 0.0, 1.0));
    if (v != val) {
      val = v;
      a.doc.setParam(pid, val, false);
      changed = true;
    }
  }
  bool wantType = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pmenu");
  if (ImGui::BeginPopup("##pmenu")) {
    paramMenu(a, pid, d, &wantType);
    ImGui::EndPopup();
  }
  if (wantType) ImGui::OpenPopup("##type");
  typePopup(a, pid, d);
  if (hov || act) {
    a.setHint(d.name + ": " + d.format(val) + "   (drag · Shift fine · Ctrl+click reset · double-click type · right-click menu)");
    if (act || ImGui::GetCurrentContext()->HoveredIdTimer > 0.5f) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7, 4));
      ImGui::BeginTooltip();
      ImGui::PushFont(a.fontMono, 11.0f);
      ImGui::TextUnformatted(d.format(val).c_str());
      ImGui::PopFont();
      ImGui::EndTooltip();
      ImGui::PopStyleVar();
    }
  }
  return changed;
}

void drawDial(ImDrawList* dl, ImVec2 c, float r, double norm, bool bipolar, bool hov, bool act, bool linked, bool learning) {
  const auto& P = pal();
  // J-93: one knob face for every editor (skin roles inside plugin skins,
  // theme roles elsewhere).
  skin::KnobPaint kp;
  kp.a0 = kKnobPi * 0.75f, kp.a1 = kKnobPi * 2.25f;
  kp.an = kp.a0 + static_cast<float>(std::clamp(norm, 0.0, 1.0)) * (kp.a1 - kp.a0);
  kp.from = bipolar ? (kp.a0 + kp.a1) * 0.5f : kp.a0;
  kp.hot = hov, kp.active = act;
  kp.railTh = std::max(2.0f, r * 0.16f);
  ImU32 mark = P.accent;
  if (const skin::Skin* k = skin::active()) {
    kp.rail = k->rail, kp.fill = act ? k->fillPressed : k->fill, kp.cap = k->cap;
    kp.pointer = k->pointer ? k->pointer : skin::lighten(k->text, 0.25f);
    mark = k->fill;
  } else {
    kp.rail = P.surface900, kp.fill = hov || act ? P.accent : P.paper, kp.cap = P.surface700, kp.pointer = P.text;
  }
  skin::knobFace(dl, c, r, kp);
  if (linked || learning) dl->AddCircleFilled(ImVec2(c.x + r * 0.8f, c.y - r * 0.8f), 2.5f, learning ? P.danger : mark);
}

}  // namespace

bool dragParamItem(App& a, ParamId pid, const ParamDesc& d, double& val, float pixels, bool horizontal) {
  return dragParam(a, pid, d, val, pixels, horizontal);
}
bool dragParamTrack(App& a, ParamId pid, const ParamDesc& d, double& val, float trackStart, float trackLen, bool horizontal) {
  const Track tr{trackStart, trackLen};
  const bool changed = dragParam(a, pid, d, val, trackLen, horizontal, &tr);
  DevTrack& dv = devTracks()[pid.index];
  const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
  dv.start = trackStart, dv.len = trackLen, dv.horizontal = horizontal;
  dv.cross = horizontal ? (r0.y + r1.y) * 0.5f : (r0.x + r1.x) * 0.5f;
  dv.norm = d.toNormalized(val), dv.value = val, dv.frame = ImGui::GetFrameCount();
  return changed;
}
bool devTrack(uint32_t paramIndex, DevTrack& out) {
  // Exact ParamId index, else a plugin parameter index inside a mixer slot (stride 1000): the latest drawn.
  bool found = false;
  for (const auto& [k, v] : devTracks())
    if ((k == paramIndex || (k >= 1000 && k % 1000 == paramIndex)) && (!found || v.frame > out.frame)) out = v, found = true;
  return found;
}
std::map<std::string, DevPoints>& devPointMap() {
  static std::map<std::string, DevPoints> m;
  return m;
}
void devPointsSet(const std::string& name, const DevPoints& p) {
  DevPoints& d = devPointMap()[name];
  d = p;
  d.frame = ImGui::GetFrameCount();
}
bool devPoints(const std::string& name, DevPoints& out) {
  auto it = devPointMap().find(name);
  if (it == devPointMap().end()) return false;
  out = it->second;
  return true;
}
bool devItem(ParamId pid, DevItem& out) {
  auto it = devItems().find(pid.packed());
  if (it == devItems().end()) return false;
  out = it->second;
  return true;
}
bool paramLinked(App& a, ParamId pid) { return isLinked(a, pid); }

float knobCellWidth(int style) { return kKnobCell[std::clamp(style, 0, 2)]; }
float knobCellHeight(int style) { return kKnobRadius[std::clamp(style, 0, 2)] * 2 + 32; }

bool knobImpl(App& a, const PluginTarget& t, int param, const char* label, float radius, float cell, bool text) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return false;
  double val = *v;
  ImGui::PushID(param);
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  if (cell <= 0) cell = radius * 2 + 8;
  ImGui::SetCursorScreenPos(ImVec2(p.x + (cell - radius * 2) * 0.5f, p.y));
  ImGui::InvisibleButton("##k", ImVec2(radius * 2, radius * 2));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = dragParam(a, pid, *d, val, 160.0f, false);
  bool bip = d->min < 0 && d->max > 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  drawDial(dl, ImVec2(p.x + cell * 0.5f, p.y + radius), radius, d->toNormalized(val), bip, hov, act, isLinked(a, pid),
           a.midiLearning && a.midiLearnTarget == pid);
  if (text) {
    std::string vs = d->format(val);
    ImGui::PushFont(a.fontMono, 10.5f);
    ImVec2 ts = ImGui::CalcTextSize(vs.c_str());
    float y = p.y + radius * 2 + 3;
    dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, y), act ? pal().paper : pal().text, vs.c_str());
    ImGui::PopFont();
    const char* lbl = label ? label : d->name.c_str();
    ImGui::PushFont(nullptr, 12.0f);
    ts = ImGui::CalcTextSize(lbl);
    float lx = p.x + std::max(0.0f, (cell - ts.x) * 0.5f);
    dl->PushClipRect(ImVec2(p.x - 2, y), ImVec2(p.x + cell + 2, y + 40), true);
    dl->AddText(ImVec2(lx, y + 14), pal().textDim, lbl);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(cell, radius * 2 + (text ? 32.0f : 0.0f)));
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

bool knob(App& a, const PluginTarget& t, int param, const char* label, int style, float cell) {
  style = std::clamp(style, 0, 2);
  return knobImpl(a, t, param, label, kKnobRadius[style], cell > 0 ? cell : kKnobCell[style], true);
}

bool knobAt(App& a, const PluginTarget& t, int param, ImVec2 pos, const char* label, int style, float cell) {
  ImVec2 keep = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(pos);
  bool r = knob(a, t, param, label, style, cell);
  ImGui::SetCursorScreenPos(keep);
  return r;
}

bool dial(App& a, const PluginTarget& t, int param, float radius) { return knobImpl(a, t, param, nullptr, radius, radius * 2, false); }

void beginPanel(const char* title, float width, float height, const char* cap) {
  const auto& P = pal();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(theme::mix(P.surface800, P.text, 0.08f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
  ImGui::BeginChild(title, ImVec2(width, height),
                    ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders | (height <= 0 ? ImGuiChildFlags_AutoResizeY : 0),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  float w = ImGui::GetWindowSize().x;
  audit::frame(wp, ImVec2(wp.x + w, wp.y + ImGui::GetWindowSize().y), "section");
  if (const skin::Skin* k = skin::active()) {  // plugin skin: section well + caption pill
    ImVec2 ws = ImGui::GetWindowSize();
    skin::well(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), *k, 4);
    std::string tt = title;
    if (size_t h = tt.find("##"); h != std::string::npos) tt.resize(h);
    ImGui::PushFont(app().fontMonoBold, 10.5f);
    float tw = ImGui::CalcTextSize(tt.c_str()).x;
    ImGui::PopFont();
    skin::pill(dl, ImVec2(wp.x + 4, wp.y + 4), ImVec2(wp.x + std::min(w - 4, tw + 24), wp.y + 20), *k, tt.c_str(), 10.5f);
    if (cap && *cap) {
      ImGui::PushFont(app().fontMonoBold, 10.5f);
      ImVec2 cs = ImGui::CalcTextSize(cap);
      dl->AddText(ImVec2(wp.x + w - 10 - cs.x, wp.y + 5), k->textDim, cap);
      ImGui::PopFont();
    }
    ImGui::SetCursorPosY(28);
    return;
  }
  // Title strip (J-93: soft shade, accent tick, title, hairline).
  skin::gradientRect(dl, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + w - 1, wp.y + 22), theme::mix(P.surface800, P.surface700, 0.75f),
                     theme::mix(P.surface800, P.surface700, 0.35f), 6, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(wp.x + 1, wp.y + 22), ImVec2(wp.x + w - 1, wp.y + 22), theme::mix(P.surface800, P.text, 0.06f));
  dl->AddRectFilled(ImVec2(wp.x + 9, wp.y + 7), ImVec2(wp.x + 11, wp.y + 16), P.accent, 1);
  // Title text: strip the "##id" suffix.
  std::string tt = title;
  if (size_t h = tt.find("##"); h != std::string::npos) tt.resize(h);
  ImGui::PushFont(app().fontMonoBold, 10.5f);
  dl->AddText(ImVec2(wp.x + 16, wp.y + 5), P.text, tt.c_str());
  if (cap && *cap) {
    ImVec2 cs = ImGui::CalcTextSize(cap);
    dl->AddText(ImVec2(wp.x + w - 10 - cs.x, wp.y + 5), P.textFaint, cap);
  }
  ImGui::PopFont();
  ImGui::SetCursorPosY(28);
}

void endPanel() {
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

bool hslider(App& a, const PluginTarget& t, int param, const char* label, float width) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return false;
  double val = *v;
  ImGui::PushID(param);
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22;
  ImGui::InvisibleButton("##hs", ImVec2(width, h));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = dragParamTrack(a, pid, *d, val, p.x, width, true);  // J-98: the handle line is drawn at p.x + width * n
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), hov || act ? P.surface700 : theme::mix(P.surface800, P.surface700, 0.5f), 4);
  float n = static_cast<float>(d->toNormalized(val));
  bool bip = d->min < 0 && d->max > 0;
  float x0 = bip ? p.x + width * static_cast<float>(d->toNormalized(0)) : p.x;
  float xv = p.x + width * n;
  dl->AddRectFilled(ImVec2(std::min(x0, xv), p.y + h - 3), ImVec2(std::max(x0, xv) + 1, p.y + h - 1), P.accent, 1);
  dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(std::max(p.x + 4, xv), p.y + h - 3), theme::withAlpha(P.text, act ? 0.10f : 0.06f), 4,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddLine(ImVec2(xv, p.y + 3), ImVec2(xv, p.y + h - 1), P.paper, 2);
  ImGui::PushFont(nullptr, 12.0f);
  dl->AddText(ImVec2(p.x + 7, p.y + 4), P.textDim, label ? label : d->name.c_str());
  ImGui::PopFont();
  std::string vs = d->format(val);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImVec2 ts = ImGui::CalcTextSize(vs.c_str());
  dl->AddText(ImVec2(p.x + width - ts.x - 7, p.y + 5), P.text, vs.c_str());
  ImGui::PopFont();
  ImGui::PopID();
  return changed;
}

bool vslider(App& a, const PluginTarget& t, int param, const char* label, ImVec2 size) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return false;
  double val = *v;
  ImGui::PushID(param);
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##vs", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  float cx = p.x + size.x * 0.5f, capH = std::min(14.0f, size.x * 0.7f), travel = size.y - capH;
  bool changed = dragParamTrack(a, pid, *d, val, p.y + capH * 0.5f, travel, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(cx - 2, p.y + capH * 0.5f), ImVec2(cx + 2, p.y + size.y - capH * 0.5f), P.surface950, 2);
  float n = static_cast<float>(d->toNormalized(val));
  bool bip = d->min < 0 && d->max > 0;
  float y0 = p.y + capH * 0.5f + (1.0f - (bip ? static_cast<float>(d->toNormalized(0)) : 0.0f)) * travel;
  float yv = p.y + capH * 0.5f + (1.0f - n) * travel;
  dl->AddRectFilled(ImVec2(cx - 2, std::min(y0, yv)), ImVec2(cx + 2, std::max(y0, yv)), P.accent, 2);
  if (bip) dl->AddLine(ImVec2(p.x + 2, y0), ImVec2(p.x + size.x - 2, y0), P.lineStrong);
  float yc = p.y + (1.0f - n) * travel;
  dl->AddRectFilled(ImVec2(p.x + 1, yc), ImVec2(p.x + size.x - 1, yc + capH), act ? P.paper : hov ? P.text : P.textDim, 3);
  dl->AddLine(ImVec2(p.x + 4, yc + capH * 0.5f), ImVec2(p.x + size.x - 4, yc + capH * 0.5f), P.surface900, 2);
  if (label) {
    std::string vs = d->format(val);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(vs.c_str());
    dl->AddText(ImVec2(cx - ts.x * 0.5f, p.y + size.y + 3), P.text, vs.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.5f);
    ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(cx - ts.x * 0.5f, p.y + size.y + 16), P.textDim, label);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(size.x, 30));
  }
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

bool led(App& a, const PluginTarget& t, int param, const char* label) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return false;
  bool on = getParam(s, pid).value_or(0) >= 0.5;
  const char* lbl = label ? label : d->name.c_str();
  ImGui::PushID(param);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 ts = ImGui::CalcTextSize(lbl);
  ImVec2 sz(ts.x + 26, 22);
  bool pressed = ImGui::InvisibleButton("##led", sz);
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), P.surface700, 4);
  ImVec2 c(p.x + 10, p.y + sz.y * 0.5f);
  if (on) dl->AddCircleFilled(c, 7.0f, theme::withAlpha(P.accent, 0.25f), 16);
  dl->AddCircleFilled(c, 4.5f, on ? P.accent : P.surface950, 16);
  dl->AddCircle(c, 4.5f, on ? theme::mix(P.accent, P.paper, 0.3f) : P.lineStrong, 16, 1.0f);
  dl->AddText(ImVec2(p.x + 20, p.y + (sz.y - ts.y) * 0.5f), on ? P.text : P.textDim, lbl);
  if (pressed) a.doc.setParam(pid, on ? 0 : 1, false);
  if (hov) a.setHint(d->name + ": " + (on ? "on" : "off") + "   (click to switch)");
  ImGui::PopID();
  return pressed;
}

bool segmented(App& a, const PluginTarget& t, int param, float width, const char* const* labels, int columns) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d || d->choices.empty()) return false;
  int n = static_cast<int>(d->choices.size());
  int cols = columns > 0 ? std::min(columns, n) : n;
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  if (width <= 0) {
    float w = 0;
    for (int i = 0; i < n; ++i) w = std::max(w, ImGui::CalcTextSize(labels ? labels[i] : d->choices[static_cast<size_t>(i)].c_str()).x + 16);
    width = w * cols;
  }
  float cw = std::floor(width / cols);
  const float h = 22;
  bool changed = false;
  ImGui::PushID(param);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  int rows = (n + cols - 1) / cols;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + cw * cols, p0.y + h * rows), P.surface950, 5);
  for (int i = 0; i < n; ++i) {
    int r = i / cols, c = i % cols;
    ImVec2 q(p0.x + c * cw, p0.y + r * h);
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##seg", ImVec2(cw, h)) && i != cur) {
      a.doc.setParam(pid, static_cast<double>(i), false);
      changed = true;
    }
    bool hov = ImGui::IsItemHovered();
    bool on = i == cur;
    const skin::Skin* sk = skin::active();
    if (on)
      dl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + cw - 2, q.y + h - 2), sk ? theme::withAlpha(sk->select, 0.32f) : P.text, sk ? 2.0f : 4.0f);
    else if (hov)
      dl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + cw - 2, q.y + h - 2), P.surface700, 4);
    if (on && sk) dl->AddRectFilled(ImVec2(q.x + 4, q.y + h * 0.5f - 2), ImVec2(q.x + 7, q.y + h * 0.5f + 2), sk->select, 1);
    const char* lbl = labels ? labels[i] : d->choices[static_cast<size_t>(i)].c_str();
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    dl->PushClipRect(q, ImVec2(q.x + cw, q.y + h), true);
    ImU32 onText = sk ? skin::lighten(sk->select, 0.4f) : P.ink;
    dl->AddText(ImVec2(q.x + std::max(3.0f, (cw - ts.x) * 0.5f), q.y + (h - ts.y) * 0.5f), on ? onText : hov ? P.text : P.textDim, lbl);
    dl->PopClipRect();
    if (hov) a.setHint(d->name + ": " + d->choices[static_cast<size_t>(i)]);
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(cw * cols, h * rows));
  ImGui::PopID();
  return changed;
}

bool combo(App& a, const PluginTarget& t, int param, float width, const char* label) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d || d->choices.empty()) return false;
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  bool changed = false;
  ImGui::PushID(param);
  if (label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
    ImGui::SameLine(0, 6);
  }
  if (ui::BeginCombo("##c", d->choices[static_cast<size_t>(std::clamp(cur, 0, static_cast<int>(d->choices.size()) - 1))].c_str(), width)) {
    for (size_t i = 0; i < d->choices.size(); ++i)
      if (ImGui::Selectable(d->choices[i].c_str(), static_cast<int>(i) == cur)) {
        a.doc.setParam(pid, static_cast<double>(i), false);
        changed = true;
      }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) a.setHint(d->name + ": " + d->format(cur));
  ImGui::PopID();
  return changed;
}

bool stepper(App& a, const PluginTarget& t, int param, float width, const char* label) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return false;
  double val = *v;
  bool changed = false;
  ImGui::PushID(param);
  if (label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", label);
    ImGui::SameLine(0, 6);
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22;
  ImGui::InvisibleButton("##st", ImVec2(width, h));
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  double step = d->kind == ParamKind::Choice || d->max - d->min >= 4 ? 1.0 : (d->max - d->min) / 20.0;
  if (ImGui::IsItemClicked()) {
    double nv = d->clamp(val + (io.MousePos.x < p.x + width * 0.5f ? -step : step));
    if (nv != val) a.doc.setParam(pid, nv, false), changed = true;
  }
  if (hov && io.MouseWheel != 0) {
    double nv = d->clamp(val + (io.MouseWheel > 0 ? step : -step));
    if (nv != val) a.doc.setParam(pid, nv, false), changed = true;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), hov ? P.surface700 : P.surface950, 4);
  ui::drawIcon(dl, ui::Icon::Left, ImVec2(p.x + 9, p.y + h * 0.5f), 9, P.textDim);
  ui::drawIcon(dl, ui::Icon::Right, ImVec2(p.x + width - 9, p.y + h * 0.5f), 9, P.textDim);
  std::string vs = d->format(val);
  ImGui::PushFont(a.fontMono, 11.0f);
  ImVec2 ts = ImGui::CalcTextSize(vs.c_str());
  dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), P.text, vs.c_str());
  ImGui::PopFont();
  if (hov) a.setHint(d->name + ": " + vs + "   (click left/right or wheel)");
  ImGui::PopID();
  return changed;
}

void displayFrame(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  audit::frame(p0, p1, "display");
  const auto& P = pal();
  skin::gradientRect(dl, p0, p1, theme::mix(P.surface950, P.surface900, 0.15f), P.surface950, 6);
  dl->AddLine(ImVec2(p0.x + 6, p0.y + 1), ImVec2(p1.x - 6, p0.y + 1), IM_COL32(0, 0, 0, 50), 1.0f);
  dl->AddRect(p0, p1, theme::mix(P.surface950, P.text, 0.10f), 6, 0, 1.0f);
}

void drawGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows, bool centreLines) {
  const auto& P = pal();
  for (int c = 1; c < cols; ++c) {
    float x = p0.x + (p1.x - p0.x) * c / cols;
    bool mid = centreLines && c * 2 == cols;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), mid ? P.gridBar : P.gridStep);
  }
  for (int r = 1; r < rows; ++r) {
    float y = p0.y + (p1.y - p0.y) * r / rows;
    bool mid = centreLines && r * 2 == rows;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), mid ? P.gridBar : P.gridStep);
  }
}

void levelMeter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float db, bool reduction, float rangeDb) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface950, 3);
  float h = p1.y - p0.y - 2;
  if (reduction) {
    float f = std::clamp(-db / rangeDb, 0.0f, 1.0f);
    if (f > 0) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 1 + f * h), P.danger, 2);
  } else {
    float f = std::clamp((db + rangeDb) / rangeDb, 0.0f, 1.0f);
    ImU32 c = db > -0.5f ? P.danger : db > -9 ? P.meterHigh : P.meterLow;
    if (f > 0) dl->AddRectFilled(ImVec2(p0.x + 1, p1.y - 1 - f * h), ImVec2(p1.x - 1, p1.y - 1), c, 2);
  }
}

void caption(const char* text) {
  ImGui::PushFont(app().fontMono, 10.5f);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", text);
  ImGui::PopFont();
}

bool parseValue(const ParamDesc& d, const std::string& text, double& out) {
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s.empty()) return false;
  if (d.kind == ParamKind::Choice) {
    for (size_t i = 0; i < d.choices.size(); ++i) {
      std::string c;
      for (char ch : d.choices[i])
        if (!std::isspace(static_cast<unsigned char>(ch))) c += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (c == s) {
        out = static_cast<double>(i);
        return true;
      }
    }
  }
  if (d.kind == ParamKind::Toggle) {
    if (s == "on" || s == "1" || s == "yes" || s == "true") return out = 1, true;
    if (s == "off" || s == "0" || s == "no" || s == "false") return out = 0, true;
  }
  if (d.unit == "note") {  // "c4", "a#3"
    static const char* names[] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
    for (int k = 11; k >= 0; --k) {
      size_t len = std::strlen(names[k]);
      if (s.compare(0, len, names[k]) == 0 && s.size() > len) {
        char* end = nullptr;
        long oct = std::strtol(s.c_str() + len, &end, 10);
        if (end && *end == 0) {
          out = d.clamp(static_cast<double>((oct + 1) * 12 + k));
          return true;
        }
      }
    }
  }
  if (d.unit == "rate" && (s == "off" || s == "max")) return out = d.max, true;  // J-104: Rate "Off" is the top of the travel
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  std::string suf = end ? std::string(end) : std::string();
  if (d.unit == "Hz" || d.unit == "rate") {
    if (suf == "k" || suf == "khz") v *= 1000.0;
  } else if (d.unit == "s") {
    if (suf == "ms") v *= 0.001;
    else if (suf.empty() && v > 20) v *= 0.001;  // bare numbers above 20 read as ms
  } else if (d.unit == "%") {
    if (suf != "x") v *= 0.01;  // typed as percent
  }
  if (d.kind == ParamKind::Choice) v = std::round(v);
  out = d.clamp(v);
  return true;
}

bool knobInline(App& a, const PluginTarget& t, int param, const char* label, float width, float radius) {
  const Song& s = a.doc.song();
  auto d = describeParam(s, t.pid(param));
  auto v = getParam(s, t.pid(param));
  if (!d || !v) return false;
  ImGui::PushID(param + 50000);
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool ch = knobImpl(a, t, param, nullptr, radius, radius * 2, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float tx = p.x + radius * 2 + 8;
  dl->PushClipRect(ImVec2(tx, p.y - 2), ImVec2(p.x + width, p.y + radius * 2 + 4), true);
  ImGui::PushFont(nullptr, 12.0f);
  dl->AddText(ImVec2(tx, p.y + radius - 15), pal().textDim, label ? label : d->name.c_str());
  ImGui::PopFont();
  std::string vs = d->format(*getParam(s, t.pid(param)));
  ImGui::PushFont(a.fontMono, 11.0f);
  dl->AddText(ImVec2(tx, p.y + radius + 1), pal().text, vs.c_str());
  ImGui::PopFont();
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(width, radius * 2 + 2));
  ImGui::EndGroup();
  ImGui::PopID();
  return ch;
}

namespace {
struct HistoryBuf {
  static constexpr int kLen = 300;
  float v[4][kLen];
  int head = 0, lastFrame = -1;
  bool used[4] = {false, false, false, false};
  HistoryBuf() {
    for (auto& lane : v)
      for (float& x : lane) x = -120.f;
  }
};
std::map<uint64_t, HistoryBuf> histories;
}  // namespace

void historyPush(uint64_t key, int lane, float value) {
  HistoryBuf& h = histories[key];
  int frame = ImGui::GetFrameCount();
  if (frame != h.lastFrame) {
    h.lastFrame = frame;
    h.head = (h.head + 1) % HistoryBuf::kLen;
    for (auto& l : h.v) l[h.head] = -120.f;
  }
  lane = std::clamp(lane, 0, 3);
  h.used[lane] = true;
  h.v[lane][h.head] = value;
}

void historyDraw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, uint64_t key, float lo, float hi) {
  const auto& P = pal();
  auto it = histories.find(key);
  if (it == histories.end()) return;
  const HistoryBuf& h = it->second;
  const int L = HistoryBuf::kLen;
  float w = p1.x - p0.x, ht = p1.y - p0.y;
  ImU32 cols[4] = {P.paper, theme::withAlpha(P.text, 0.45f), P.danger, P.accent};
  ImVec2 pts[HistoryBuf::kLen];
  for (int lane = 0; lane < 4; ++lane) {
    if (!h.used[lane]) continue;
    for (int i = 0; i < L; ++i) {
      int idx = (h.head + 1 + i) % L;
      float v = h.v[lane][idx];
      if (lane == 2 && v <= -119.f) v = 0;  // no data yet: no reduction
      float y;
      if (lane == 2)  // reduction: grows down from the top
        y = p0.y + std::clamp(-v / (hi - lo), 0.0f, 1.0f) * ht;
      else
        y = p1.y - std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * ht;
      pts[i] = ImVec2(p0.x + w * i / (L - 1), y);
    }
    if (lane == 0 || lane == 1) {
      for (int i = 0; i + 1 < L; ++i) {
        ImVec2 q[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, p1.y), ImVec2(pts[i].x, p1.y)};
        dl->AddConvexPolyFilled(q, 4, theme::withAlpha(cols[lane], lane == 0 ? 0.10f : 0.06f));
      }
    }
    dl->AddPolyline(pts, L, cols[lane], 0, lane == 2 ? 1.5f : 1.2f);
  }
}

void setParamValue(App& a, const PluginTarget& t, int param, double value, bool merge) { a.doc.setParam(t.pid(param), value, merge); }

double paramValue(App& a, const PluginTarget& t, int param) { return getParam(a.doc.song(), t.pid(param)).value_or(0.0); }


}  // namespace pui

// -------------------------------------------------------------------- body

void drawPluginBody(App& a, const PluginTarget& t, float width) {
  const PluginInfo* info = t.info(a.doc.song());
  if (!info) return;
  skin::resetFrames();  // Q-7: panels of this editor only
  if (isHostedType(info->type)) {  // third-party plugin: editor button + parameter list
    drawHostedEditor(a, t, width);
    return;
  }
  if (drawCustomEditor(a, t, width)) return;
  // Generic: choices first, then a knob grid inside one section.
  pui::beginSection("PARAMETERS", width);
  float x0 = ImGui::GetCursorPosX();
  float used = 0;
  for (size_t k = 0; k < info->params.size(); ++k) {
    const ParamDesc& d = info->params[k];
    if (d.kind == ParamKind::Choice) {
      ImGui::TextDisabled("%s", d.name.c_str());
      pui::choiceButtons(a, t, static_cast<int>(k));
    }
  }
  for (size_t k = 0; k < info->params.size(); ++k) {
    const ParamDesc& d = info->params[k];
    if (d.kind == ParamKind::Choice) continue;
    if (used > 0 && used + 70 > width - 20) {
      used = 0;
    } else if (used > 0) {
      ImGui::SameLine(0, 0);
    }
    if (d.kind == ParamKind::Toggle) {
      ImGui::BeginGroup();
      ImGui::Dummy(ImVec2(0, 8));
      pui::toggleButton(a, t, static_cast<int>(k));
      ImGui::EndGroup();
      used += ImGui::GetItemRectSize().x + 6;
      continue;
    }
    pui::knobCell(a, t, static_cast<int>(k), nullptr, 70);
    used += 70;
  }
  (void)x0;
  pui::endSection();
}

}  // namespace gl
