// Gridline — control bar, settings panel and routing of installed plugins'
// windows (see PluginWindowBar.h). Window-system independent.
#include "PluginWindowBar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include "../core/I18n.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "App.h"
#include "HostedUi.h"
#include "HostedUiPages.h"
#include "Theme.h"
#include "Widgets.h"
#include "../../third_party/imgui/imgui.h"

namespace gl {

namespace lay = host::layout;

namespace {

// Control bar look (slate wrapper roles, drawn by Gridline).
struct BarLook {
  ImU32 top = theme::rgb(0x434C52), bottom = theme::rgb(0x323A3F), edge = theme::rgb(0x23292D);
  ImU32 text = theme::rgb(0xE4ECEE), dim = theme::rgb(0x9FA8AD), faint = theme::rgb(0x6C767C);
  ImU32 btn = theme::rgb(0x2A3237), btnHov = theme::rgb(0x36404A), btnOn = theme::rgb(0x3D63DD);
  ImU32 lcd = theme::rgb(0x161A1D), lcdText = theme::rgb(0x9FE0A8);
  ImU32 led = theme::rgb(0x4CD964), ledOff = theme::rgb(0x55402F), warn = theme::rgb(0xE5484D);
  ImU32 panel = theme::rgb(0x262C30);
};

using Key = std::pair<const App*, uint64_t>;

std::map<Key, bool>& channelBypass() {  // instruments bypassed from the bar (plugin's own bypass)
  static std::map<Key, bool> m;
  return m;
}
std::set<Key>& panelRequested() {  // targets whose Gridline panel was asked for explicitly
  static std::set<Key> s;
  return s;
}
std::map<Key, double>& reopen() {  // targets to show again once their new instance runs
  static std::map<Key, double> m;
  return m;
}

bool barButton(const char* id, const lay::Rect& r, float y0) {
  ImGui::SetCursorPos(ImVec2(static_cast<float>(r.x), y0 + static_cast<float>(r.y)));
  return ImGui::InvisibleButton(id, ImVec2(static_cast<float>(r.w), static_cast<float>(r.h)));
}

void boxAt(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 fill, ImU32 edge) {
  dl->AddRectFilled(p0, p1, fill, 3.0f);
  dl->AddRect(p0, p1, edge, 3.0f);
}

void centredText(ImDrawList* dl, ImFont* f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* text) {
  ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0, text);
  dl->AddText(f, size, ImVec2(std::round((p0.x + p1.x - ts.x) * 0.5f), std::round((p0.y + p1.y - ts.y) * 0.5f)), col, text);
}

void drawGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  for (int k = 0; k < 8; ++k) {
    const float a = static_cast<float>(k) * 3.14159265f / 4.0f;
    const ImVec2 d(std::cos(a), std::sin(a));
    dl->AddLine(ImVec2(c.x + d.x * r * 0.55f, c.y + d.y * r * 0.55f), ImVec2(c.x + d.x * r, c.y + d.y * r), col, 2.6f);
  }
  dl->AddCircle(c, r * 0.62f, col, 16, 2.0f);
  dl->AddCircleFilled(c, r * 0.22f, col, 10);
}

void drawPower(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->PathArcTo(c, r, -3.14159265f * 0.5f + 0.75f, 3.14159265f * 1.5f - 0.75f, 18);
  dl->PathStroke(col, 0, 1.8f);
  dl->AddLine(ImVec2(c.x, c.y - r - 1.5f), ImVec2(c.x, c.y - 1.0f), col, 1.8f);
}

void drawBar(pluginbar::Bar& w, host::HostedInstance* hi) {
  App& a = *w.app;
  const BarLook K;
  const PluginTarget t = w.target;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  const float by = o.y;  // bar top in screen space (the ImGui window starts at the bar)
  const float W = static_cast<float>(w.L.w), H = static_cast<float>(w.L.bar.h);
  dl->AddRectFilledMultiColor(ImVec2(o.x, by), ImVec2(o.x + W, by + H), K.top, K.top, K.bottom, K.bottom);
  dl->AddLine(ImVec2(o.x, by + 0.5f), ImVec2(o.x + W, by + 0.5f), K.edge, 1.0f);
  const lay::BarLayout B = lay::layoutBar(w.L.w);
  auto R = [&](lay::BarItem it, ImVec2& p0, ImVec2& p1) {
    const lay::Rect& r = B.items[it];
    p0 = ImVec2(o.x + static_cast<float>(r.x), by + static_cast<float>(r.y));
    p1 = ImVec2(p0.x + static_cast<float>(r.w), p0.y + static_cast<float>(r.h));
  };
  ImFont* fr = a.fontRegular ? a.fontRegular : ImGui::GetFont();
  ImFont* fm = a.fontMono ? a.fontMono : fr;
  ImVec2 p0, p1;

  // Presets.
  const int np = hi ? hostedui::presetCount(hi) : 0;
  int& cur = hostedui::presetIndex(t);
  R(lay::kBarPrev, p0, p1);
  if (barButton("##prev", B.items[lay::kBarPrev], 0) && np > 0) hostedui::selectPreset(a, t, hi, cur <= 0 ? np - 1 : cur - 1);
  ui::Hint("Previous preset of the plugin");
  boxAt(dl, p0, p1, ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x + 3, c.y - 5), ImVec2(c.x + 3, c.y + 5), ImVec2(c.x - 4, c.y), np > 0 ? K.text : K.faint);
  }
  R(lay::kBarPreset, p0, p1);
  if (barButton("##preset", B.items[lay::kBarPreset], 0) && np > 0) ImGui::OpenPopup("##presets");
  ui::Hint("The plugin's own programs and preset library");
  boxAt(dl, p0, p1, K.lcd, ImGui::IsItemHovered() ? K.btnOn : K.edge);
  {
    std::string pname = cur >= 0 && cur < np ? hostedui::presetName(hi, cur) : (np > 0 ? std::string("Presets") : std::string("No presets"));
    const float maxW = p1.x - p0.x - 12;
    const char* fitted = theme::fitText(pname.c_str(), maxW);
    ImVec2 ts = fm->CalcTextSizeA(11.0f, 1e9f, 0, fitted);
    dl->AddText(fm, 11.0f, ImVec2(p0.x + 6, std::round((p0.y + p1.y - ts.y) * 0.5f)), np > 0 ? K.lcdText : K.faint, fitted);
  }
  R(lay::kBarNext, p0, p1);
  if (barButton("##next", B.items[lay::kBarNext], 0) && np > 0) hostedui::selectPreset(a, t, hi, cur < 0 ? 0 : (cur + 1) % np);
  ui::Hint("Next preset of the plugin");
  boxAt(dl, p0, p1, ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 5), ImVec2(c.x - 3, c.y + 5), ImVec2(c.x + 4, c.y), np > 0 ? K.text : K.faint);
  }
  if (ImGui::BeginPopup("##presets")) {
    if (hi->programCount() > 0) ImGui::TextDisabled("%s", i18n::tr("Programs"));
    for (int k = 0; k < hi->programCount(); ++k)
      if (ImGui::Selectable((hi->programName(k) + "##p" + std::to_string(k)).c_str(), k == cur)) hostedui::selectPreset(a, t, hi, k);
    std::string cat = "\x01";
    const auto& lib = hi->libraryPresets();
    for (size_t k = 0; k < lib.size(); ++k) {
      if (lib[k].category != cat) {
        cat = lib[k].category;
        ImGui::TextDisabled("%s", cat.empty() ? "Library" : cat.c_str());
      }
      const int idx = hi->programCount() + static_cast<int>(k);
      if (ImGui::Selectable((lib[k].name + "##l" + std::to_string(k)).c_str(), idx == cur)) hostedui::selectPreset(a, t, hi, idx);
    }
    ImGui::EndPopup();
  }

  // Bypass: effects switch their mixer slot; instruments use the plugin's own bypass.
  {
    bool avail = true, on = true;
    const Key bk{&a, t.key()};
    if (!t.isChannel()) {
      on = getParam(a.doc.song(), slotEnableParam(t.owner, t.slot)).value_or(1.0) >= 0.5;
    } else {
      avail = hi && hi->backend()->hasBypass();
      on = !channelBypass()[bk];
    }
    R(lay::kBarBypass, p0, p1);
    if (barButton("##bypass", B.items[lay::kBarBypass], 0) && avail) {
      if (!t.isChannel()) {
        a.doc.setParam(slotEnableParam(t.owner, t.slot), on ? 0.0 : 1.0, false);
      } else if (hi->backend()->setBypass(on)) {
        channelBypass()[bk] = on;
      }
    }
    ui::Hint(!avail ? "This instrument has no bypass of its own" : on ? "Plugin on: click to bypass it" : "Plugin bypassed: click to switch it on");
    boxAt(dl, p0, p1, ImGui::IsItemHovered() && avail ? K.btnHov : K.btn, K.edge);
    drawPower(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f + 1), 5.5f, !avail ? K.faint : on ? K.led : K.warn);
  }

  // Sandbox / generic switches.
  auto toggle = [&](lay::BarItem it, const char* id, const char* label, bool lit, const char* hint) {
    R(it, p0, p1);
    const bool clicked = barButton(id, B.items[it], 0);
    ui::Hint(hint);
    boxAt(dl, p0, p1, lit ? K.btnOn : ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
    centredText(dl, fr, 10.5f, p0, p1, lit ? theme::rgb(0xFFFFFF) : K.text, label);
    return clicked;
  };
  const bool sandboxed = hi && hi->sandboxed();
  if (toggle(lay::kBarSandbox, "##sandbox", "SANDBOX", sandboxed,
             sandboxed ? "Runs in its own process (a crash only silences it). Click to run it inside Gridline"
                       : "Run this plugin in its own process: if it crashes, Gridline keeps running"))
    w.action = pluginbar::Action::Sandbox;
  if (toggle(lay::kBarGeneric, "##generic", "GENERIC", false, "Show Gridline's parameter list instead of the plugin's interface"))
    w.action = pluginbar::Action::Generic;

  // CPU / latency readout.
  if (B.visible[lay::kBarCpu] && hi) {
    R(lay::kBarCpu, p0, p1);
    float avg = 0, pk = 0;
    hostedui::cpuLoad(a, t, hi, avg, pk);
    dl->AddRectFilled(p0, p1, K.lcd, 3.0f);
    const float fillW = std::clamp(avg, 0.0f, 1.0f) * (p1.x - p0.x);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 3), ImVec2(p0.x + fillW, p1.y), avg > 0.5f ? K.warn : K.btnOn, 1.0f);
    char line[64];
    const double sr = hi->backend()->sampleRate() > 0 ? hi->backend()->sampleRate() : 48000.0;
    std::snprintf(line, sizeof line, "CPU %.1f%%  %.1fms", 100.0 * avg, 1000.0 * hi->totalLatency() / sr);
    centredText(dl, fm, 10.0f, p0, ImVec2(p1.x, p1.y - 2), K.dim, line);
    ImGui::SetCursorPos(ImVec2(static_cast<float>(B.items[lay::kBarCpu].x), static_cast<float>(B.items[lay::kBarCpu].y)));
    ImGui::Dummy(ImVec2(static_cast<float>(B.items[lay::kBarCpu].w), static_cast<float>(B.items[lay::kBarCpu].h)));
    char tip[160];
    std::snprintf(tip, sizeof tip, "Processing load %.1f%% (peak %.0f%%), latency %d samples (compensated)", 100.0 * avg, 100.0 * pk, hi->totalLatency());
    ui::Hint(tip);
  }

  // Settings panel.
  R(lay::kBarSettings, p0, p1);
  if (barButton("##settings", B.items[lay::kBarSettings], 0)) w.wantPanel = w.panelPage >= 0 ? -1 : 0;
  ui::Hint(w.panelPage >= 0 ? "Fold the settings away" : "Settings: processing, MIDI, sidechain, outputs, parameters");
  boxAt(dl, p0, p1, w.panelPage >= 0 ? K.btnOn : ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  drawGear(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), 6.5f, K.text);
}

void drawPanel(pluginbar::Bar& w) {
  const BarLook K;
  App& a = *w.app;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  const float top = static_cast<float>(w.L.bar.h);
  dl->AddRectFilled(ImVec2(o.x, o.y + top), ImVec2(o.x + static_cast<float>(w.L.w), o.y + top + static_cast<float>(w.L.panel.h)), K.panel);
  ImGui::SetCursorPos(ImVec2(8, top + 6));
  static const char* const kTabs[] = {"Processing", "Settings", "Parameters"};
  static const char* const kHints[] = {"Buffers, rendering mode, sandbox, latency", "MIDI ports, sidechain input, extra outputs, plugin facts",
                                       "Every parameter: automate, link to a controller, type a value"};
  int page = w.panelPage;
  ui::TabStrip("##ptabs", kTabs, 3, &page, kHints);
  if (page != w.panelPage) w.panelPage = page;
  if (w.target.isChannel()) {
    ImGui::SameLine(0, 12);
    if (ui::TextButton("Channel\xE2\x80\xA6", false, 0, "Gridline's channel window: tuning, polyphony, note effects")) w.action = pluginbar::Action::ChannelPanel;
  }
  const float y = ImGui::GetCursorPosY() + 2;
  ImGui::SetCursorPos(ImVec2(8, y));
  const float width = static_cast<float>(w.L.w) - 16;
  ImGui::BeginChild("##page", ImVec2(width, top + static_cast<float>(w.L.panel.h) - y - 6), ImGuiChildFlags_None);
  hostedui::drawPage(a, w.target, w.panelPage + 1, width - 12);  // tabs: processing, settings, parameters (compact)
  ImGui::EndChild();
}

bool panelShowing(App& a, const PluginTarget& t) {
  return t.isChannel() ? a.settingsChannel == t.owner : a.fxEditor == std::make_pair(t.owner, t.slot);
}
void hidePanel(App& a, const PluginTarget& t) {
  if (t.isChannel()) {
    if (a.settingsChannel == t.owner) a.settingsChannel = kNoId;
  } else if (a.fxEditor == std::make_pair(t.owner, t.slot)) {
    a.fxEditor = {kNoId, -1};
  }
}
// The window to show for an installed plugin: its own interface unless it
// has none, the generic editor is chosen, or it is not running.
bool wantsNative(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || !isHostedType(st->type)) return false;
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) return false;  // Gridline's panel explains (missing / crashed)
  return lay::chooseEditorMode(hi->backend()->hasEditor(), host::hostOptions(hi->hostedClass().type).genericEditor) == lay::EditorMode::Native;
}

}  // namespace

namespace pluginbar {

void draw(Bar& b, host::HostedInstance* hi, float top) {
  ImGui::SetNextWindowPos(ImVec2(0, top));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(b.L.w), static_cast<float>(b.L.bar.h + b.L.panel.h)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##pluginbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(3);
  if (hi && !hi->crashed()) {
    drawBar(b, hi);
    if (b.panelPage >= 0) drawPanel(b);
  }
  ImGui::End();
}

void background(float rgba[4]) {
  const ImVec4 bg = theme::vec(BarLook().panel);
  rgba[0] = bg.x;
  rgba[1] = bg.y;
  rgba[2] = bg.z;
  rgba[3] = 1.0f;
}

void runAction(App& a, const PluginTarget& t, const std::string& type, Action act) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  switch (act) {
    case Action::Generic:
      hostedui::setGenericEditor(a, type, true);
      break;
    case Action::Sandbox:
      if (hi) setHostedSandbox(a, type, !hi->sandboxed());  // reopens on the new instance
      break;
    case Action::ChannelPanel:
      showHostedPanel(a, t);
      break;
    case Action::Close:
      closeHostedEditor(a, t);
      break;
    case Action::None:
      break;
  }
}

void reopenPending(App& a) {
  for (auto it = reopen().begin(); it != reopen().end();) {
    if (it->first.first != &a) {
      ++it;
      continue;
    }
    if (it->second == 0) it->second = a.now + 5.0;  // give up after a few seconds
    bool done = a.now > it->second;
    // Find the target by key among the song's hosted plugins.
    const Song& s = a.doc.song();
    PluginTarget found{kNoId, -1};
    for (const auto& c : s.channels)
      if (PluginTarget{c.id, -1}.key() == it->first.second) found = {c.id, -1};
    for (const auto& m : s.mixer)
      for (int k = 0; k < kEffectSlots && k < static_cast<int>(m.slots.size()); ++k)
        if (PluginTarget{m.id, k}.key() == it->first.second) found = {m.id, k};
    if (found.owner == kNoId) done = true;
    if (!done) {
      host::HostedInstance* hi = hostedInstanceFor(a, found);
      if (hi && !hi->crashed()) {
        if (hi->backend()->hasEditor() && !host::hostOptions(hi->hostedClass().type).genericEditor) openHostedEditor(a, found);
        done = true;
      }
    }
    it = done ? reopen().erase(it) : std::next(it);
  }
}

}  // namespace pluginbar

// ---------------------------------------------------------------- routing

void hostedui::reopenLater(const PluginTarget& t) { reopen()[Key{&app(), t.key()}] = 0; }

void routePluginWindows(App& a) {
  // Explicit panel requests end with the panel.
  for (auto it = panelRequested().begin(); it != panelRequested().end();) {
    if (it->first != &a) {
      ++it;
      continue;
    }
    const bool still = (a.settingsChannel != kNoId && PluginTarget{a.settingsChannel, -1}.key() == it->second) ||
                       (a.fxEditor.first != kNoId && PluginTarget{a.fxEditor.first, a.fxEditor.second}.key() == it->second);
    it = still ? std::next(it) : panelRequested().erase(it);
  }
  auto route = [&a](const PluginTarget& t) {
    if (panelRequested().count(Key{&a, t.key()}) || !wantsNative(a, t)) return;
    if (openHostedEditor(a, t))
      hidePanel(a, t);
    else
      panelRequested().insert(Key{&a, t.key()});  // could not open: Gridline's panel stays
  };
  if (a.settingsChannel != kNoId) route(PluginTarget{a.settingsChannel, -1});
  if (a.fxEditor.first != kNoId) route(PluginTarget{a.fxEditor.first, a.fxEditor.second});
}

bool pluginWindowShown(App& a, const PluginTarget& t) { return hostedEditorOpen(t) || panelShowing(a, t); }

void togglePluginWindow(App& a, const PluginTarget& t) {
  if (hostedEditorOpen(t)) {
    closeHostedEditor(a, t);
    return;
  }
  if (panelShowing(a, t)) {
    hidePanel(a, t);
    return;
  }
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
  routePluginWindows(a);
}

void showHostedPanel(App& a, const PluginTarget& t) {
  panelRequested().insert(Key{&a, t.key()});
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
}

}  // namespace gl
