// Gridline - K-404: Studio inline Device chain panel. See DeviceChainUi.h.
#include "DeviceChainUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/DeviceChainLayout.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/Project.h"
#include "../host/HostedPlugins.h"
#include "App.h"
#include "BrowserUi.h"
#include "EditorZoomUi.h"
#include "HostedUi.h"
#include "LayerEditor.h"
#include "PluginFavsUi.h"
#include "PluginIcons.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::devchainui {

using theme::pal;

namespace {

constexpr const char* kDragPayload = "GL_STUDIODEV";

struct DragItem {
  ObjectId owner = kNoId;
  int slot = -1;
};

struct Entry {
  PluginTarget target;
  const PluginInfo* info = nullptr;
  std::string type;
  std::string name;
  std::string sub;
  bool isInstrument = false;
  bool hosted = false;
  bool enabled = true;
  float designW = 600.0f;
  float designH = 0.0f;
  std::string zoomKey;
};

struct DrawnInfo {
  std::string name, type;
  int slot = -1;
  bool collapsed = false;
  bool visible = false;
  bool drawn = false;
  float scale = 1.0f;
  float x = 0.0f, w = 0.0f, h = 0.0f;
};

struct ChainStats {
  int frame = -1;
  int devices = 0;
  int drawnEditors = 0;
  int drawnHosted = 0;
  int skippedCollapsed = 0;
  int skippedOffscreen = 0;
  float panelH = 0.0f;
  float scrollX = 0.0f;
  std::vector<DrawnInfo> items;
};

struct State {
  uint64_t selected = 0;
  std::set<uint64_t> collapsed;
  std::set<uint64_t> configuring;
  std::map<uint64_t, int> seenTouched;
  PluginState clipboard;
  // Track focus memory: switching channel shows that channel + its mixer track; switching to an
  // unrelated mixer track (bus / Master) shows that mixer track's inserts.
  ObjectId lastSelChannel = kNoId;
  ObjectId lastSelMixer = kNoId;
  ObjectId busFocus = kNoId;
  // One-shot horizontal scroll request (e.g. after adding a new effect or via dev command).
  uint64_t scrollToKey = 0;
  bool hasDevScroll = false;
  float devScrollX = 0.0f;
  // Pending dev actions applied on the next draw frame.
  int devCollapseMode = -2;  // -2 none, -1 all, -3 none(expand all), >= 0 index
  bool devCollapseTo = true;
  bool devPrintStats = false;
  ImVec2 devResize{0, 0};
  ChainStats lastStats;
};

State& state() {
  static State s;
  return s;
}

// Keep collapsed / selected UI state attached to effects after a reorder.
void followMove(ObjectId owner, int from, int to) {
  State& st = state();
  auto remap = [&](uint64_t k) {
    for (int sl = 0; sl < kEffectSlots; ++sl)
      if (PluginTarget{owner, sl}.key() == k) return PluginTarget{owner, devchain::remapSlotOnMove(sl, from, to)}.key();
    return k;
  };
  for (std::set<uint64_t>* set : {&st.collapsed, &st.configuring}) {
    std::set<uint64_t> next;
    for (uint64_t k : *set) next.insert(remap(k));
    *set = std::move(next);
  }
  st.selected = remap(st.selected);
}

std::string fitText(ImFont* f, float fs, const std::string& text, float maxW) {
  if (!f || maxW <= 6.0f) return "";
  if (f->CalcTextSizeA(fs, 1e9f, 0, text.c_str()).x <= maxW) return text;
  std::string s = text;
  while (s.size() > 1 && f->CalcTextSizeA(fs, 1e9f, 0, (s + "\xE2\x80\xA6").c_str()).x > maxW) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "\xE2\x80\xA6";
}

// Upward-rotated vertical text for slim collapsed strips (drawn flat, then vertices rotated 90 deg CCW).
void upwardText(ImDrawList* dl, ImFont* f, float fs, ImVec2 origin, ImU32 col, const std::string& text) {
  if (!dl || !f || text.empty()) return;
  const int v0 = dl->VtxBuffer.Size;
  dl->PushClipRectFullScreen();
  dl->AddText(f, fs, origin, col, text.c_str());
  dl->PopClipRect();
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - origin.x, dy = v.pos.y - origin.y;
    v.pos = ImVec2(origin.x + dy, origin.y - dx);
  }
}

// Crisp Studio LED dot (green when on, subtle hollow ring when bypassed/muted).
void drawPowerLed(ImDrawList* dl, ImVec2 c, bool on, bool hovered) {
  const auto& P = pal();
  if (on) {
    dl->AddCircleFilled(c, 5.5f, theme::withAlpha(P.led, hovered ? 0.34f : 0.20f), 16);
    dl->AddCircleFilled(c, 3.6f, P.led, 16);
  } else {
    dl->AddCircleFilled(c, 3.6f, hovered ? P.surface600 : P.surface900, 16);
    dl->AddCircle(c, 3.6f, hovered ? P.textDim : P.textFaint, 16, 1.1f);
  }
}

// Compact square icon button for the 26 px device header.
bool headerIconBtn(ImDrawList* dl, const char* id, ui::Icon icon, ImVec2 p0, float w, float h, bool lit, const char* hint,
                   bool dangerHover = false) {
  const auto& P = pal();
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const ImVec2 c(std::round((p0.x + p1.x) * 0.5f), std::round((p0.y + p1.y) * 0.5f));
  if (lit || hov || held) {
    const ImU32 bg = held ? P.surface600 : lit ? theme::withAlpha(P.hot, 0.22f) : theme::withAlpha(P.surface600, 0.7f);
    dl->AddRectFilled(p0, p1, bg, theme::kRadiusSm);
  }
  const ImU32 col = lit ? P.hot : (hov && dangerHover) ? P.danger : hov ? P.text : P.textDim;
  ui::drawIcon(dl, icon, c, 11.0f, col);
  if (hint) ui::Hint(hint);
  return clicked;
}

bool floatingWindowOpen(App& a, const Entry& e) {
  if (e.hosted) return pluginWindowShown(a, e.target);
  if (e.isInstrument) return a.settingsChannel == e.target.owner;
  return a.fxEditor.first == e.target.owner && a.fxEditor.second == e.target.slot;
}

void toggleFloatingWindow(App& a, const Entry& e) {
  if (e.hosted) {
    togglePluginWindow(a, e.target);
    return;
  }
  const bool open = floatingWindowOpen(a, e);
  if (e.isInstrument) {
    a.settingsChannel = open ? kNoId : e.target.owner;
    if (!open) a.focusWindow = "###channelsettings";
  } else {
    a.fxEditor = open ? std::pair<ObjectId, int>{kNoId, -1} : std::pair<ObjectId, int>{e.target.owner, e.target.slot};
    if (!open) a.focusWindow = "###fxeditor";
  }
}

void toggleEnabled(App& a, const Entry& e) {
  if (e.isInstrument) {
    const ObjectId cid = e.target.owner;
    a.doc.editChannel(cid, e.enabled ? "mute channel" : "unmute channel", [](Channel& c) { c.muted = !c.muted; });
  } else {
    a.doc.setParam(slotEnableParam(e.target.owner, e.target.slot), e.enabled ? 0.0 : 1.0, false);
  }
}

int firstFreeSlot(const MixerTrack* mt) {
  if (!mt) return -1;
  for (int k = 0; k < kEffectSlots; ++k)
    if (mt->slots[static_cast<size_t>(k)].plugin.type.empty()) return k;
  return -1;
}

// Adds an effect into `(track, slot)` WITHOUT forcing a floating window open.
bool addEffectInline(App& a, ObjectId track, int slot, const std::string& type) {
  if (track == kNoId || slot < 0 || slot >= kEffectSlots || type.empty()) return false;
  if (!a.doc.setSlotPlugin(track, slot, type)) return false;
  const PluginTarget t{track, slot};
  pui::pluginReplaced(t);
  State& st = state();
  st.selected = t.key();
  st.collapsed.erase(t.key());
  st.scrollToKey = t.key();
  return true;
}

void copyEffect(App& a, const Entry& e) {
  if (e.isInstrument) return;
  if (const PluginState* ps = e.target.state(a.doc.song())) state().clipboard = *ps;
}

void pasteEffectAfter(App& a, const Entry& e) {
  State& st = state();
  if (st.clipboard.type.empty()) return;
  const Song& s = a.doc.song();
  ObjectId owner = e.target.owner;
  int after = e.target.slot;
  if (e.isInstrument) {
    const Channel* c = s.channel(e.target.owner);
    if (!c) return;
    owner = c->mixerTrack != kNoId ? c->mixerTrack : s.masterId();
    after = -1;
  }
  const MixerTrack* mt = s.mixerTrack(owner);
  const int freeSl = firstFreeSlot(mt);
  if (freeSl < 0) {
    a.notify(i18n::tr("No free effect slot on this mixer track"), true);
    return;
  }
  const int to = freeSl > after ? after + 1 : after;
  const PluginState copy = st.clipboard;
  a.doc.beginGroup("paste effect");
  if (a.doc.setSlotPlugin(owner, freeSl, copy.type)) {
    setPluginState(a, PluginTarget{owner, freeSl}, copy, "paste effect");
    if (to != freeSl && to >= 0) {
      a.doc.moveSlot(owner, freeSl, to);
      followMove(owner, freeSl, to);
    }
    st.scrollToKey = PluginTarget{owner, std::max(0, to)}.key();
  }
  a.doc.endGroup();
}

void acceptDropOnSlot(App& a, ObjectId track, int targetSlot, int browserFreeSlot) {
  if (!ImGui::BeginDragDropTarget()) return;
  if (browserFreeSlot >= 0) {
    browserui::DropPlace place;
    place.mixer = track;
    place.slot = browserFreeSlot;
    browserui::acceptPluginDrop(a, browse::DropTarget::MixerSlot, place);
    browserui::acceptFileDrop(a, browse::DropTarget::MixerSlot, place);
  }
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kDragPayload)) {
    DragItem dd;
    std::memcpy(&dd, pl->Data, sizeof dd);
    const Song& s = a.doc.song();
    const MixerTrack* mt = s.mixerTrack(track);
    if (dd.owner == track && mt && dd.slot >= 0 && dd.slot < kEffectSlots && targetSlot >= 0 && targetSlot < kEffectSlots) {
      if (ImGui::GetIO().KeyAlt) {
        const int freeSl = firstFreeSlot(mt);
        if (freeSl >= 0) {
          const PluginState copy = mt->slots[static_cast<size_t>(dd.slot)].plugin;
          a.doc.beginGroup("copy effect");
          if (a.doc.setSlotPlugin(track, freeSl, copy.type)) {
            setPluginState(a, PluginTarget{track, freeSl}, copy, "copy effect");
            if (targetSlot != freeSl) {
              a.doc.moveSlot(track, freeSl, targetSlot);
              followMove(track, freeSl, targetSlot);
            }
          }
          a.doc.endGroup();
        } else {
          a.notify(i18n::tr("No free effect slot on this mixer track"), true);
        }
      } else if (dd.slot != targetSlot) {
        a.doc.moveSlot(track, dd.slot, targetSlot);
        followMove(track, dd.slot, targetSlot);
      }
    }
  }
  ImGui::EndDragDropTarget();
}

void presetPopupBody(App& a, const Entry& e) {
  const PluginState* ps = e.target.state(a.doc.song());
  if (!ps || !e.info) {
    ImGui::TextDisabled("%s", i18n::tr("No presets"));
    return;
  }
  if (ImGui::MenuItem(i18n::tr("Default"))) {
    PluginState ns = defaultPluginState(e.info->type);
    ns.data = ps->data;
    ns.shown = ps->shown;
    setPluginState(a, e.target, ns, "reset plugin");
    pui::presetLoaded(e.target, e.info->type, "");
  }
  if (!e.info->presets.empty()) ImGui::Separator();
  for (size_t i = 0; i < e.info->presets.size(); ++i) {
    if (ImGui::MenuItem(e.info->presets[i].name.c_str())) {
      PluginState ns = presetStateKeepingData(*e.info, i, ps->data);
      ns.shown = ps->shown;
      setPluginState(a, e.target, ns, "load preset");
      pui::presetLoaded(e.target, e.info->type, e.info->presets[i].name, static_cast<int>(i));
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Save preset to file..."))) savePresetDialog(a, e.target.owner, e.target.slot);
}

void deviceContextMenu(App& a, const Entry& e) {
  State& st = state();
  const uint64_t key = e.target.key();
  const bool collapsed = st.collapsed.count(key) > 0;
  const bool fx = !e.isInstrument;
  if (ImGui::MenuItem(e.enabled ? (fx ? i18n::tr("Bypass") : i18n::tr("Mute channel")) : (fx ? i18n::tr("Enable") : i18n::tr("Unmute channel"))))
    toggleEnabled(a, e);
  if (ImGui::MenuItem(collapsed ? i18n::tr("Expand") : i18n::tr("Collapse"))) {
    if (collapsed) st.collapsed.erase(key);
    else st.collapsed.insert(key);
  }
  if (ImGui::MenuItem(i18n::tr("Show its window"), nullptr, floatingWindowOpen(a, e))) toggleFloatingWindow(a, e);
  ImGui::Separator();
  if (fx && ImGui::MenuItem(i18n::tr("Cut"))) {
    copyEffect(a, e);
    a.doc.setSlotPlugin(e.target.owner, e.target.slot, "");
  }
  if (fx && ImGui::MenuItem(i18n::tr("Copy"))) copyEffect(a, e);
  if (ImGui::MenuItem(i18n::tr("Paste effect after"), nullptr, false, !st.clipboard.type.empty())) pasteEffectAfter(a, e);
  if (fx && ImGui::MenuItem(i18n::tr("Duplicate"))) {
    const PluginState keep = st.clipboard;
    copyEffect(a, e);
    pasteEffectAfter(a, e);
    st.clipboard = keep;
  }
  if (ImGui::BeginMenu(i18n::tr("Replace"))) {
    swapDeviceItems(a, e.target.owner, e.target.slot);
    ImGui::EndMenu();
  }
  if (fx) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Remove effect"))) a.doc.setSlotPlugin(e.target.owner, e.target.slot, "");
  }
  ImGui::Separator();
  if (ImGui::MenuItem(pui::compareOnB(e.target) ? i18n::tr("Compare: switch to A") : i18n::tr("Compare: switch to B")))
    pui::compareFlip(a, e.target);
  if (ImGui::MenuItem(i18n::tr("Compare: copy to the other side"))) pui::compareStore(a, e.target);
  ImGui::Separator();
  pluginWindowMenu(a, e.target.owner, e.target.slot, true);
}

// Searchable inline effect picker popup (never pops open a floating effect window).
void drawAddEffectPopup(App& a, ObjectId track, int slot) {
  auto pick = [&](const std::string& type) {
    addEffectInline(a, track, slot, type);
    ImGui::CloseCurrentPopup();
  };
  if (favui::searchField(PluginKind::Effect, pick, "")) return;
  const auto favs = favui::favouritesOfKind(PluginKind::Effect);
  if (!favs.empty()) {
    ui::Label(i18n::tr("Favourites"));
    ImGui::PushID("favs");
    for (const PluginInfo* p : favs)
      if (favui::pickerRow(*p, false)) pick(p->type);
    ImGui::PopID();
    ImGui::Spacing();
  }
  for (const CatalogGroup& g : catalogGroups(PluginKind::Effect)) {
    ui::Label(g.category.c_str());
    for (const PluginInfo* p : g.items)
      if (favui::pickerRow(*p, false)) pick(p->type);
    ImGui::Spacing();
  }
  const auto inst = favui::installedOfKind(PluginKind::Effect);
  if (!inst.empty()) {
    ui::Label(i18n::tr("Installed"));
    for (const PluginInfo* p : inst) {
      std::string fmt, uid;
      const host::Format* f = host::splitType(p->type, fmt, uid) ? host::findFormat(fmt) : nullptr;
      if (favui::pickerRow(*p, false, f ? f->label : nullptr)) pick(p->type);
    }
  }
}

std::vector<int> hostedShownParams(const PluginState* ps, const PluginInfo* info) {
  std::vector<int> out;
  const int n = info ? static_cast<int>(info->params.size()) : 0;
  // Only what the person pinned: an installed plugin keeps its own interface (Show its window),
  // so the card never lists its parameters on its own.
  if (ps)
    for (int k : ps->shown)
      if (k >= 0 && k < n) out.push_back(k);
  return out;
}

void setHostedShown(App& a, const PluginTarget& t, const std::vector<int>& v) {
  if (t.isChannel())
    a.doc.editChannel(t.owner, "device parameters", [&](Channel& c) { c.plugin.shown = v; });
  else
    a.doc.editMixerTrack(t.owner, "device parameters", [&](MixerTrack& m) { m.slots[static_cast<size_t>(t.slot)].plugin.shown = v; });
}

// 0.7.9 compact card body for an installed third-party plugin (native embedding is 0.8.0).
void drawHostedCardBody(App& a, const Entry& e, ImVec2 b0, ImVec2 b1) {
  const auto& P = pal();
  State& st = state();
  const uint64_t key = e.target.key();
  const float w = std::max(40.0f, b1.x - b0.x);
  const float h = std::max(40.0f, b1.y - b0.y);
  ImGui::SetCursorScreenPos(b0);
  ImGui::BeginChild("##hostedbody", ImVec2(w, h), ImGuiChildFlags_None);
  std::string fmt, uid;
  const host::Format* f = host::splitType(e.type, fmt, uid) ? host::findFormat(fmt) : nullptr;
  const bool loading = hostedLoading(a, e.target);
  const bool winOpen = pluginWindowShown(a, e.target);
  const bool conf = st.configuring.count(key) > 0;
  const float btnW = std::floor((w - 6.0f) * 0.5f);
  if (ui::TextButton(winOpen ? "Hide window" : "Show its window", winOpen, btnW, "Show or hide this plugin's native window"))
    toggleFloatingWindow(a, e);
  ImGui::SameLine(0, 4);
  if (ui::TextButton(conf ? "Done" : "Configure", conf, btnW,
                     conf ? "Stop capturing touched controls" : "Touch controls in the plugin's window to pin them here")) {
    if (!st.configuring.erase(key)) {
      st.configuring.insert(key);
      st.seenTouched[key] = hostedLastTouched(e.target);
      if (!winOpen) toggleFloatingWindow(a, e);
    }
  }
  ImGui::Spacing();
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  if (loading) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Loading plugin..."));
  } else {
    const char* badge = f && f->label ? f->label : "Plugin";
    ImGui::TextColored(theme::vec(P.textFaint), "%s  \xC2\xB7  %s", badge, e.sub.c_str());
  }
  ImGui::PopFont();
  const PluginState* ps = e.target.state(a.doc.song());
  std::vector<int> rows = hostedShownParams(ps, e.info);
  const int touched = hostedLastTouched(e.target);
  if (conf && touched >= 0 && touched != st.seenTouched[key]) {
    st.seenTouched[key] = touched;
    if (std::find(rows.begin(), rows.end(), touched) == rows.end()) {
      rows.push_back(touched);
      setHostedShown(a, e.target, rows);
    }
  }
  const bool showTemp = !conf && touched >= 0 && std::find(rows.begin(), rows.end(), touched) == rows.end() && e.info &&
                        touched < static_cast<int>(e.info->params.size());
  if (rows.empty() && !showTemp) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(40.0f, w - 8.0f));
    ImGui::TextColored(theme::vec(P.textDim), "%s",
                       conf ? i18n::tr("Move a control in the plugin's window to add it to this card.")
                            : i18n::tr("Click Show its window for the native interface, or Configure to pin key parameters here."));
    ImGui::PopTextWrapPos();
  } else {
    const float remH = std::max(24.0f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##hparams", ImVec2(0, remH), ImGuiChildFlags_None);
    const float rw = std::max(40.0f, ImGui::GetContentRegionAvail().x - 2.0f);
    for (size_t i = 0; i < rows.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      hostedParamRow(a, e.target, rows[i], rw);
      ImGui::PopID();
    }
    if (showTemp) {
      ImGui::PushID("temp");
      hostedParamRow(a, e.target, touched, rw);
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
  ImGui::EndChild();
}

ImU32 accentFor(const Entry& e) {
  const auto& P = pal();
  if (!e.enabled) return P.textFaint;
  if (e.info) {
    if (const PluginBrand* b = pluginBrand(e.info->type)) {
      const uint32_t role = pui::brandRole(b->family);
      if (role != 0) return theme::litRole(role);
    }
  }
  return e.isInstrument ? theme::litRole(theme::role::kStamp) : P.accent;
}

void drawCollapsedCard(App& a, ImDrawList* dl, const Entry& e, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  State& st = state();
  const uint64_t key = e.target.key();
  const bool selected = st.selected == key;
  const ImVec2 p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, selected ? theme::mix(P.surface800, P.surface700, 0.55f) : P.surface800, theme::kRadius);
  // Subtle top brand accent bar
  dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 3), accentFor(e), theme::kRadiusSm, ImDrawFlags_RoundCornersTop);
  // Power LED
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 3, p0.y + 5));
  if (ImGui::InvisibleButton("##led", ImVec2(w - 6, 16))) toggleEnabled(a, e);
  const bool ledHov = ImGui::IsItemHovered();
  ui::Hint(e.isInstrument ? "Mute or unmute this channel" : "Turn this effect on or off");
  drawPowerLed(dl, ImVec2(std::round(p0.x + w * 0.5f), p0.y + 13), e.enabled, ledHov);
  // Expand chevron
  if (headerIconBtn(dl, "##expand", ui::Icon::Right, ImVec2(p0.x + 5, p0.y + 23), w - 10, 18, false, "Expand this device editor"))
    st.collapsed.erase(key);
  // Clickable/draggable vertical strip body
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 1, p0.y + 43));
  ImGui::InvisibleButton("##strip", ImVec2(w - 2, std::max(12.0f, h - 44)));
  if (ImGui::IsItemClicked()) st.selected = key;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) st.collapsed.erase(key);
  ui::Hint(e.isInstrument ? "Double-click to expand" : "Double-click to expand; drag to reorder (Alt: copy)");
  if (!e.isInstrument && ImGui::BeginDragDropSource()) {
    const DragItem dd{e.target.owner, e.target.slot};
    ImGui::SetDragDropPayload(kDragPayload, &dd, sizeof dd);
    ImGui::TextUnformatted(e.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (!e.isInstrument) acceptDropOnSlot(a, e.target.owner, e.target.slot, -1);
  if (ImGui::BeginPopupContextItem("##devmenu")) {
    deviceContextMenu(a, e);
    ImGui::EndPopup();
  }
  const float fs = theme::kFontCaption + 0.5f;
  // K-450: a built-in device's own icon under the strip's top buttons, the name reading up below it.
  float iconRoom = 0.0f;
  if (e.info && h > 90.0f &&
      ui::drawPluginIcon(dl, e.info->type, ImVec2(p0.x + w * 0.5f, p0.y + 56.0f), 14, ui::PluginIconSurface::DeviceBand, e.enabled ? 1.0f : 0.45f))
    iconRoom = 26.0f;  // (icon 49..63, 8 px under the chevron; the name 9 px below it)
  else if (e.info)
    ui::notePluginIconFallback(e.info->type, ui::PluginIconSurface::DeviceBand);
  const std::string label = fitText(a.fontBold, fs, e.name, std::max(12.0f, h - 54.0f - iconRoom));
  upwardText(dl, a.fontBold, fs, ImVec2(std::round(p0.x + (w - fs) * 0.5f), p1.y - 8.0f), e.enabled ? P.text : P.textDim, label);
  dl->AddRect(p0, p1, selected ? P.hot : P.line, theme::kRadius, 0, selected ? 1.5f : 1.0f);
}

void drawExpandedCard(App& a, ImDrawList* dl, const Entry& e, const devchain::DeviceBox& box, ImVec2 p0, ImRect scrollClip) {
  const auto& P = pal();
  State& st = state();
  const uint64_t key = e.target.key();
  const bool selected = st.selected == key;
  const float w = box.w, h = box.h;
  const ImVec2 p1(p0.x + w, p0.y + h);
  const float hdrH = devchain::kHeaderH;

  // Card frame & header background
  dl->AddRectFilled(p0, p1, theme::mix(P.surface900, P.surface800, 0.5f), theme::kRadius);
  const ImU32 hdrBg = selected ? theme::mix(P.surface700, P.surface600, 0.35f) : theme::mix(P.surface800, P.surface700, 0.5f);
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + hdrH), hdrBg, theme::kRadius, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 1), ImVec2(p1.x - 2, p0.y + 3), accentFor(e));
  dl->AddLine(ImVec2(p0.x + 1, p0.y + hdrH), ImVec2(p1.x - 1, p0.y + hdrH), P.line);

  // ---- Header bar: left controls (LED, collapse, drag handle) ----
  const float by = p0.y + 4.0f, bh = hdrH - 6.0f;
  float lx = p0.x + 4.0f;
  ImGui::SetCursorScreenPos(ImVec2(lx, by));
  if (ImGui::InvisibleButton("##led", ImVec2(16, bh))) toggleEnabled(a, e);
  const bool ledHov = ImGui::IsItemHovered();
  ui::Hint(e.isInstrument ? "Mute or unmute this channel" : "Turn this effect on or off");
  drawPowerLed(dl, ImVec2(lx + 8.0f, p0.y + hdrH * 0.5f + 0.5f), e.enabled, ledHov);
  lx += 17.0f;

  if (headerIconBtn(dl, "##collapse", ui::Icon::ChevronDown, ImVec2(lx, by), 18, bh, false, "Collapse to a slim strip"))
    st.collapsed.insert(key);
  lx += 19.0f;

  if (!e.isInstrument) {
    const ImVec2 g0(lx, by);
    ImGui::SetCursorScreenPos(g0);
    ImGui::InvisibleButton("##grip", ImVec2(12, bh));
    const bool gHov = ImGui::IsItemHovered();
    ui::drawIcon(dl, ui::Icon::GripDots, ImVec2(g0.x + 6.0f, p0.y + hdrH * 0.5f + 0.5f), 10.0f, gHov ? P.text : P.textFaint);
    ui::Hint("Drag to reorder this effect (hold Alt to copy)");
    if (ImGui::BeginDragDropSource()) {
      const DragItem dd{e.target.owner, e.target.slot};
      ImGui::SetDragDropPayload(kDragPayload, &dd, sizeof dd);
      ImGui::TextUnformatted(e.name.c_str());
      ImGui::EndDragDropSource();
    }
    lx += 13.0f;
  }

  // ---- Header bar: right controls (laid out right-to-left so zero overlap is guaranteed) ----
  float rx = p1.x - 4.0f;
  if (!e.isInstrument) {
    rx -= 18.0f;
    if (headerIconBtn(dl, "##remove", ui::Icon::Close, ImVec2(rx, by), 18, bh, false, "Remove this effect", true)) {
      a.doc.setSlotPlugin(e.target.owner, e.target.slot, "");
      dl->AddRect(p0, p1, selected ? P.hot : P.line, theme::kRadius, 0, selected ? 1.5f : 1.0f);
      return;
    }
    rx -= 2.0f;
  }
  rx -= 18.0f;
  if (headerIconBtn(dl, "##menubtn", ui::Icon::More, ImVec2(rx, by), 18, bh, false, "Device menu")) ImGui::OpenPopup("##devmenu");
  rx -= 2.0f;

  rx -= 18.0f;
  const bool winOpen = floatingWindowOpen(a, e);
  if (headerIconBtn(dl, "##popout", ui::Icon::WindowPopout, ImVec2(rx, by), 18, bh, winOpen,
                    winOpen ? "Close floating window" : "Show its window"))
    toggleFloatingWindow(a, e);
  rx -= 3.0f;

  // Effects: compact wet/dry mix dial right in the header
  if (!e.isInstrument) {
    rx -= 20.0f;
    ImGui::SetCursorScreenPos(ImVec2(rx + 1.0f, p0.y + std::floor((hdrH - 18.0f) * 0.5f) + 1.0f));
    ui::ParamKnob("##mix", slotMixParam(e.target.owner, e.target.slot), 8.5f);
    rx -= 3.0f;
  }

  // Preset controls (<, menu, >) when name still has >= 52 px
  const bool hasPresets = e.info != nullptr;
  if (hasPresets && rx - 52.0f - lx >= 52.0f) {
    rx -= 16.0f;
    if (headerIconBtn(dl, "##pnext", ui::Icon::Right, ImVec2(rx, by), 16, bh, false, "Next preset")) stepPluginPreset(a, e.target, 1);
    rx -= 17.0f;
    if (headerIconBtn(dl, "##plist", ui::Icon::ListBullets, ImVec2(rx, by), 16, bh, false, "Presets")) ImGui::OpenPopup("##presets");
    rx -= 17.0f;
    if (headerIconBtn(dl, "##pprev", ui::Icon::Left, ImVec2(rx, by), 16, bh, false, "Previous preset")) stepPluginPreset(a, e.target, -1);
    rx -= 4.0f;
  }
  if (ImGui::BeginPopup("##presets")) {
    presetPopupBody(a, e);
    ImGui::EndPopup();
  }

  // ---- Header title (fills remaining [lx, rx]) ----
  const float nameW = std::max(10.0f, rx - lx);
  ImGui::SetCursorScreenPos(ImVec2(lx, p0.y + 2.0f));
  ImGui::InvisibleButton("##title", ImVec2(nameW, hdrH - 3.0f));
  if (ImGui::IsItemClicked()) st.selected = key;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) st.collapsed.insert(key);
  ui::Hint(e.isInstrument ? "Click to select, double-click to collapse"
                          : "Click to select, double-click to collapse; drag to reorder (Alt: copy)");
  if (!e.isInstrument && ImGui::BeginDragDropSource()) {
    const DragItem dd{e.target.owner, e.target.slot};
    ImGui::SetDragDropPayload(kDragPayload, &dd, sizeof dd);
    ImGui::TextUnformatted(e.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (!e.isInstrument) acceptDropOnSlot(a, e.target.owner, e.target.slot, -1);
  if (ImGui::BeginPopupContextItem("##devmenu")) {
    deviceContextMenu(a, e);
    ImGui::EndPopup();
  }
  const float fs = theme::kFontCaption + 1.0f;
  const float ty = theme::textTop(a.fontBold, fs, p0.y + 1.0f, hdrH - 1.0f);
  dl->PushClipRect(ImVec2(lx, p0.y + 1.0f), ImVec2(lx + nameW, p0.y + hdrH), true);
  // K-450: a built-in device's own icon before its name, as on the Clips band's cards.
  float iconW = 0.0f;
  if (e.info && nameW > 40.0f &&
      ui::drawPluginIcon(dl, e.info->type, ImVec2(lx + 8.0f, p0.y + 1.0f + (hdrH - 1.0f) * 0.5f), 14, ui::PluginIconSurface::DeviceBand,
                         e.enabled ? 1.0f : 0.45f))
    iconW = 22.0f;
  else if (e.info)
    ui::notePluginIconFallback(e.info->type, ui::PluginIconSurface::DeviceBand);
  const std::string titleStr = fitText(a.fontBold, fs, e.name, nameW - 4.0f - iconW);
  dl->AddText(a.fontBold, fs, ImVec2(lx + 1.0f + iconW, ty), e.enabled ? P.text : P.textDim, titleStr.c_str());
  dl->PopClipRect();

  // ---- Card body ----
  const ImVec2 b0(p0.x + devchain::kPadX, p0.y + hdrH + devchain::kPadY);
  const ImVec2 b1(p1.x - devchain::kPadX, p1.y - devchain::kPadY);
  if (e.hosted) {
    drawHostedCardBody(a, e, b0, b1);
  } else {
    // Centre horizontally & vertically inside the snapped wrapper body.
    const float offX = std::max(0.0f, std::floor((box.bodyW - box.scaledW) * 0.5f));
    const float offY = std::max(0.0f, std::floor((box.bodyH - box.scaledH) * 0.5f));
    ImRect clip(b0, b1);
    clip.ClipWithFull(scrollClip);
    ImGui::SetCursorScreenPos(ImVec2(b0.x + offX, b0.y + offY));
    edzoomui::beginInline(e.zoomKey, e.type, e.designW, box.scale, clip.Min, clip.Max);
    if (e.info) {
      drawPluginBody(a, e.target, e.designW);
    } else if (e.isInstrument) {
      layerui::drawEditor(a, e.target.owner, e.designW);
    }
    edzoomui::endInline(e.zoomKey);
    if (!e.enabled) {
      // Subtle visual bypass veil over the body (drawn on the parent list behind/with crisp border).
      dl->AddRectFilled(b0, b1, theme::withAlpha(P.surface950, 0.30f), theme::kRadiusSm);
    }
  }
  // Drop presets / samples from browser onto the card
  ImGui::SetCursorScreenPos(p0);
  browserui::pluginWindowDropTarget(a, e.target);

  // Outer crisp border
  dl->AddRect(p0, p1, selected ? P.hot : P.line, theme::kRadius, 0, selected ? 1.5f : 1.0f);
}

void drawAddCard(App& a, ImDrawList* dl, const MixerTrack* mt, int freeSl, bool onMaster, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const bool dragging = ImGui::GetDragDropPayload() != nullptr;
  dl->AddRectFilled(p0, p1, theme::withAlpha(P.surface800, 0.55f), theme::kRadius);
  dl->AddRect(p0, p1, dragging ? P.hot : theme::withAlpha(P.lineStrong, 0.7f), theme::kRadius, 0, dragging ? 1.5f : 1.0f);

  // Whole card acts as drop target (browser plugin/file or reorder-to-end).
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##adddrop", ImVec2(w, h));
  if (mt && freeSl >= 0) {
    int lastUsed = -1;
    for (int k = 0; k < kEffectSlots; ++k)
      if (!mt->slots[static_cast<size_t>(k)].plugin.type.empty()) lastUsed = k;
    acceptDropOnSlot(a, mt->id, std::max(0, lastUsed), freeSl);
  }

  const float pad = 10.0f;
  const float btnW = std::max(40.0f, w - 2.0f * pad);
  const float btnH = ImGui::GetFrameHeight();
  const bool showMasterBtn = onMaster && h >= 136.0f;
  const float blockH = (freeSl >= 0 ? (btnH * 2.0f + 8.0f) : btnH) + (showMasterBtn ? btnH + 8.0f : 0.0f) + 18.0f;
  float y = std::round(p0.y + std::max(8.0f, (h - blockH) * 0.5f));

  // Track caption
  if (mt) {
    const Song& s = a.doc.song();
    const int idx = s.mixerIndex(mt->id);
    const std::string trk = idx == 0 ? std::string(i18n::tr("Master")) : mixerTrackName(s, static_cast<size_t>(idx));
    const std::string fit = fitText(a.fontMono, theme::kFontLabel, trk, btnW);
    const float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, fit.c_str()).x;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p0.x + (w - tw) * 0.5f), y), P.textDim, fit.c_str());
    y += 18.0f;
  }

  if (mt && freeSl >= 0) {
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, y));
    if (ui::TextButton("+ Add effect", false, btnW, "Add an effect to this mixer track inline")) ImGui::OpenPopup("##addfx");
    if (ImGui::BeginPopup("##addfx")) {
      ImGui::BeginChild("##addfxlist", ImVec2(240, 340), ImGuiChildFlags_None);
      drawAddEffectPopup(a, mt->id, freeSl);
      ImGui::EndChild();
      ImGui::EndPopup();
    }
    y += btnH + 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, y));
    if (ui::TextButton("+ Junction", false, btnW, "Insert a modular Junction FX patch into this device chain"))
      addEffectInline(a, mt->id, freeSl, "gl.patcherfx");
    y += btnH + 6.0f;
  } else {
    const std::string full = fitText(a.fontRegular, theme::kFontLabel, i18n::tr("All slots full"), btnW);
    const float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, full.c_str()).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(p0.x + (w - tw) * 0.5f), y + 4.0f), P.textDim, full.c_str());
    y += btnH + 6.0f;
  }

  if (showMasterBtn) {
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, y));
    if (ui::TextButton("Master chain", a.showMaster, btnW, "Open the Master chain window")) {
      a.showMaster = true;
      a.focusWindow = "Master chain";
    }
  }
}

void resolveTargets(App& a, const Channel*& outCh, const MixerTrack*& outMt) {
  const Song& s = a.doc.song();
  State& st = state();
  const ObjectId curCh = a.selChannel;
  const ObjectId curMt = a.selMixer;

  if (curCh != st.lastSelChannel) {
    st.lastSelChannel = curCh;
    st.busFocus = kNoId;
  }
  if (curMt != st.lastSelMixer) {
    st.lastSelMixer = curMt;
    const Channel* ch = s.channel(curCh);
    const ObjectId chMt = ch ? (ch->mixerTrack != kNoId ? ch->mixerTrack : s.masterId()) : kNoId;
    if (curMt != kNoId && curMt != chMt) st.busFocus = curMt;
    else st.busFocus = kNoId;
  }

  if (st.busFocus != kNoId) {
    if (const MixerTrack* mt = s.mixerTrack(st.busFocus)) {
      outCh = nullptr;
      outMt = mt;
      return;
    }
    st.busFocus = kNoId;
  }

  const Channel* ch = s.channel(curCh);
  if (!ch && !s.channels.empty()) ch = &s.channels.front();
  outCh = ch;
  if (ch) {
    outMt = s.mixerTrack(ch->mixerTrack);
    if (!outMt && !s.mixer.empty()) outMt = &s.mixer[0];
  } else {
    outMt = s.mixerTrack(curMt);
    if (!outMt && !s.mixer.empty()) outMt = &s.mixer[0];
  }
}

}  // namespace

void draw(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  State& st = state();

  if (st.devResize.x > 0.0f && st.devResize.y > 0.0f) {
    if (ImGuiWindow* root = ImGui::GetCurrentWindow() ? ImGui::GetCurrentWindow()->RootWindow : nullptr)
      ImGui::SetWindowSize(root, st.devResize, ImGuiCond_Always);
    st.devResize = ImVec2(0, 0);
  }

  const Channel* ch = nullptr;
  const MixerTrack* mt = nullptr;
  resolveTargets(a, ch, mt);

  if (!ch && !mt) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), "No device",
                   "Select a channel or mixer track: its instrument and effects show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 60.0f)));
    return;
  }

  // Build signal-order entries: instrument first, then mixer track's occupied effect slots.
  std::vector<Entry> entries;
  if (ch) {
    Entry e;
    e.target = PluginTarget{ch->id, -1};
    e.info = findPlugin(ch->plugin.type);
    e.type = e.info ? e.info->type : std::string("(layer)");
    e.name = e.info ? e.info->name : ch->name;
    e.sub = i18n::tr("Instrument");
    e.isInstrument = true;
    e.hosted = isHostedType(ch->plugin.type);
    e.enabled = !ch->muted;
    e.designW = e.info && e.info->editorWidth > 0 ? e.info->editorWidth : (!e.info ? layerui::editorWidth() : 600.0f);
    e.zoomKey = "dc_" + std::to_string(e.target.key());
    e.designH = edzoomui::inlineDesignSize(e.zoomKey, e.type, e.designW).y;
    entries.push_back(std::move(e));
  }
  const int freeSl = firstFreeSlot(mt);
  if (mt) {
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& sl = mt->slots[static_cast<size_t>(k)];
      if (sl.plugin.type.empty()) continue;
      const PluginInfo* fx = findPlugin(sl.plugin.type);
      const bool hosted = isHostedType(sl.plugin.type);
      if (!fx && !hosted) continue;
      Entry e;
      e.target = PluginTarget{mt->id, k};
      e.info = fx;
      e.type = sl.plugin.type;
      e.name = fx ? fx->name : sl.plugin.type;
      e.sub = std::string(i18n::tr("Slot")) + " " + std::to_string(k + 1);
      e.isInstrument = false;
      e.hosted = hosted;
      e.enabled = sl.enabled;
      e.designW = fx && fx->editorWidth > 0 ? fx->editorWidth : (sl.plugin.type == "gl.eq" ? 780.0f : 600.0f);
      e.zoomKey = "dc_" + std::to_string(e.target.key());
      e.designH = edzoomui::inlineDesignSize(e.zoomKey, e.type, e.designW).y;
      entries.push_back(std::move(e));
    }
  }

  // Apply pending dev collapse command before laying out this frame.
  if (st.devCollapseMode != -2) {
    const int mode = std::exchange(st.devCollapseMode, -2);
    if (mode == -1) {
      for (const Entry& e : entries) st.collapsed.insert(e.target.key());
    } else if (mode == -3) {
      for (const Entry& e : entries) st.collapsed.erase(e.target.key());
    } else if (mode >= 0 && mode < static_cast<int>(entries.size())) {
      const uint64_t k = entries[static_cast<size_t>(mode)].target.key();
      if (st.devCollapseTo) st.collapsed.insert(k);
      else st.collapsed.erase(k);
    }
  }

  // Build pure layout specs.
  std::vector<devchain::DeviceSpec> specs;
  specs.reserve(entries.size());
  for (const Entry& e : entries) {
    devchain::DeviceSpec sp;
    sp.designW = e.designW;
    sp.designH = e.designH;
    sp.collapsed = st.collapsed.count(e.target.key()) > 0;
    sp.hosted = e.hosted;
    sp.isInstrument = e.isInstrument;
    specs.push_back(sp);
  }

  // Horizontal scrollable chain container (always crisp edge-to-edge height fit).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##studio_devchain", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleVar();

  if (st.hasDevScroll) {
    ImGui::SetScrollX(std::max(0.0f, st.devScrollX));
    st.hasDevScroll = false;
  }

  ImGuiWindow* win = ImGui::GetCurrentWindow();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float scrollX = ImGui::GetScrollX();
  const float viewW = win->InnerRect.GetWidth();
  const float sbH = win->ScrollbarX ? ImGui::GetStyle().ScrollbarSize : 0.0f;
  const float availH = std::max(devchain::kMinCardH, win->InnerRect.GetHeight() - sbH - 2.0f);
  const bool hasAddCard = mt != nullptr;
  const bool onMaster = mt && !s.mixer.empty() && mt->id == s.mixer[0].id;

  const devchain::RowLayout row = devchain::computeRow(specs, availH, scrollX, viewW, hasAddCard);

  // One-shot scroll reveal for a newly added device.
  if (st.scrollToKey != 0) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].target.key() == st.scrollToKey) {
        const auto& b = row.boxes[i];
        if (b.x + b.w > scrollX + viewW) ImGui::SetScrollX(std::max(0.0f, b.x + b.w - viewW + 8.0f));
        else if (b.x < scrollX) ImGui::SetScrollX(std::max(0.0f, b.x));
        break;
      }
    }
    st.scrollToKey = 0;
  }

  ChainStats stats;
  stats.frame = ImGui::GetFrameCount();
  stats.devices = static_cast<int>(entries.size());
  stats.panelH = row.cardH;
  stats.scrollX = scrollX;
  stats.items.reserve(entries.size());

  const ImRect scrollClip = win->InnerClipRect;

  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    const devchain::DeviceBox& box = row.boxes[i];
    const bool collapsed = specs[i].collapsed;
    const ImVec2 p0(origin.x + box.x, origin.y);

    // Subtle signal-flow chevron between instrument and first effect.
    if (i > 0 && entries[i - 1].isInstrument) {
      const float cx = p0.x - devchain::kSignalGap * 0.5f;
      const float cy = origin.y + row.cardH * 0.5f;
      ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(cx, cy), 10.0f, P.textFaint);
    }

    DrawnInfo di;
    di.name = e.name;
    di.type = e.type;
    di.slot = e.target.slot;
    di.collapsed = collapsed;
    di.visible = box.visible;
    di.scale = box.scale;
    di.x = box.x;
    di.w = box.w;
    di.h = box.h;

    if (collapsed) {
      // K-404 CPU rule: collapsed devices draw ONLY the slim vertical strip (zero editor/scope work).
      ++stats.skippedCollapsed;
      if (box.visible) {
        ImGui::PushID(static_cast<int>(i));
        drawCollapsedCard(a, dl, e, p0, box.w, box.h);
        ImGui::PopID();
      }
    } else if (!box.visible) {
      // K-404 CPU rule: off-screen devices skip editor draw calls and scope taps completely.
      ++stats.skippedOffscreen;
    } else {
      di.drawn = true;
      if (e.hosted) ++stats.drawnHosted;
      else ++stats.drawnEditors;
      ImGui::PushID(static_cast<int>(i));
      drawExpandedCard(a, dl, e, box, p0, scrollClip);
      ImGui::PopID();
    }
    stats.items.push_back(std::move(di));
  }

  // Signal-flow chevron into "+ Add effect" when only the instrument is present.
  if (hasAddCard && !entries.empty() && entries.back().isInstrument) {
    const float cx = origin.x + row.addX - devchain::kSignalGap * 0.5f;
    const float cy = origin.y + row.cardH * 0.5f;
    ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(cx, cy), 10.0f, P.textFaint);
  }
  if (hasAddCard) {
    const bool addVis = (viewW <= 0.0f) || (row.addX + devchain::kAddCardW > scrollX + 1.0f && row.addX < scrollX + viewW - 1.0f);
    if (addVis) {
      ImGui::PushID("addcard");
      drawAddCard(a, dl, mt, freeSl, onMaster, ImVec2(origin.x + row.addX, origin.y), devchain::kAddCardW, row.cardH);
      ImGui::PopID();
    }
  }

  // Reserve total scrollable row extent cleanly (keeps scrollbar rock-steady when devices cull).
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(std::max(1.0f, row.totalW), row.cardH));

  // Shift+wheel (or horizontal wheel) scrolls the chain sideways smoothly.
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheelH != 0.0f) {
      ImGui::SetScrollX(std::clamp(scrollX - io.MouseWheelH * 48.0f, 0.0f, ImGui::GetScrollMaxX()));
    } else if (io.KeyShift && io.MouseWheel != 0.0f) {
      ImGui::SetScrollX(std::clamp(scrollX - io.MouseWheel * 64.0f, 0.0f, ImGui::GetScrollMaxX()));
    }
  }

  ImGui::EndChild();

  st.lastStats = std::move(stats);
  if (st.devPrintStats) {
    st.devPrintStats = false;
    const ChainStats& cs = st.lastStats;
    std::printf("devchain frame %d: devices %d drawn %d hosted %d collapsed %d offscreen %d panelH %.0f scrollX %.0f\n",
                cs.frame, cs.devices, cs.drawnEditors, cs.drawnHosted, cs.skippedCollapsed, cs.skippedOffscreen,
                static_cast<double>(cs.panelH), static_cast<double>(cs.scrollX));
    for (size_t i = 0; i < cs.items.size(); ++i) {
      const DrawnInfo& di = cs.items[i];
      std::printf("  dev %zu \"%s\" type %s slot %d collapsed %d visible %d drawn %d scale %.3f rect %.0f %.0f %.0f\n",
                  i, di.name.c_str(), di.type.c_str(), di.slot, di.collapsed ? 1 : 0, di.visible ? 1 : 0, di.drawn ? 1 : 0,
                  static_cast<double>(di.scale), static_cast<double>(di.x), static_cast<double>(di.w), static_cast<double>(di.h));
    }
    std::fflush(stdout);
  }
}

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.size() < 2 || args[0] != "chain") return false;
  const std::string& op = args[1];
  State& st = state();
  if (op == "stats") {
    st.devPrintStats = true;
    return true;
  }
  if (op == "collapse" && args.size() > 2) {
    st.devCollapseTo = true;
    if (args[2] == "all") st.devCollapseMode = -1;
    else if (args[2] == "none") st.devCollapseMode = -3;
    else st.devCollapseMode = std::atoi(args[2].c_str());
    return true;
  }
  if (op == "expand" && args.size() > 2) {
    st.devCollapseTo = false;
    st.devCollapseMode = args[2] == "all" ? -3 : std::atoi(args[2].c_str());
    return true;
  }
  if (op == "scroll" && args.size() > 2) {
    st.hasDevScroll = true;
    st.devScrollX = static_cast<float>(std::atof(args[2].c_str()));
    return true;
  }
  if (op == "resize" && args.size() > 3) {
    st.devResize = ImVec2(static_cast<float>(std::atof(args[2].c_str())), static_cast<float>(std::atof(args[3].c_str())));
    return true;
  }
  const Channel* ch = nullptr;
  const MixerTrack* mt = nullptr;
  resolveTargets(a, ch, mt);
  if (op == "add" && args.size() > 2 && mt) {
    const int sl = firstFreeSlot(mt);
    if (sl >= 0) addEffectInline(a, mt->id, sl, args[2]);
    return true;
  }
  if (op == "remove" && args.size() > 2 && mt) {
    const int sl = std::atoi(args[2].c_str());
    if (sl >= 0 && sl < kEffectSlots) a.doc.setSlotPlugin(mt->id, sl, "");
    return true;
  }
  if (op == "move" && args.size() > 3 && mt) {
    const int from = std::atoi(args[2].c_str()), to = std::atoi(args[3].c_str());
    if (from >= 0 && from < kEffectSlots && to >= 0 && to < kEffectSlots && from != to) {
      a.doc.moveSlot(mt->id, from, to);
      followMove(mt->id, from, to);
    }
    return true;
  }
  if (op == "bypass" && args.size() > 2 && mt) {
    const int sl = std::atoi(args[2].c_str());
    if (sl >= 0 && sl < kEffectSlots) {
      const bool en = mt->slots[static_cast<size_t>(sl)].enabled;
      a.doc.setParam(slotEnableParam(mt->id, sl), en ? 0.0 : 1.0, false);
    }
    return true;
  }
  return false;
}

}  // namespace gl::devchainui
