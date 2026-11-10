// Gridline — hosted (VST3 / CLAP / VST2 / AU) plugins in the app.
#include "../core/I18n.h"
#include "HostedUi.h"
#include "../core/Crash.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include "../host/CrossFormat.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/vst3/Vst3Editor.h"
#include "../packs/modulation/junction.h"
#include "HostedUiPages.h"
#include "App.h"
#include "PluginSkin.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "UndoUi.h"

namespace gl {

using theme::pal;

host::HostedInstance* hostedInstanceFor(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || !isHostedType(st->type)) return nullptr;
  if (t.node) {  // K-7: a module in a Junction
    if (t.isChannel()) {
      auto rt = a.builder.channelRuntime(t.owner);
      packs::modulation::JRuntime* j = rt && rt->gen ? packs::modulation::junctionRuntime(rt->gen.get()) : nullptr;
      if (!j) return nullptr;
      if (Generator* g = j->hostedGenerator(t.node)) return host::hostedInstance(g);
      Effect* f = j->hostedEffect(t.node);
      return f ? host::hostedInstance(f) : nullptr;
    }
    auto rt = a.builder.effectRuntime(t.owner, t.slot);
    packs::modulation::JRuntime* j = rt && rt->fx ? packs::modulation::junctionRuntime(rt->fx.get()) : nullptr;
    if (!j) return nullptr;
    if (Generator* g = j->hostedGenerator(t.node)) return host::hostedInstance(g);
    Effect* f = j->hostedEffect(t.node);
    return f ? host::hostedInstance(f) : nullptr;
  }
  if (t.isChannel()) {
    auto rt = a.builder.channelRuntime(t.owner);
    return rt && rt->gen && rt->type == st->type ? host::hostedInstance(rt->gen.get()) : nullptr;
  }
  auto rt = a.builder.effectRuntime(t.owner, t.slot);
  return rt && rt->fx && rt->type == st->type ? host::hostedInstance(rt->fx.get()) : nullptr;
}

namespace {

std::string targetName(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  if (t.node) {
    const PluginState* st = t.state(s);
    const PluginInfo* info = st ? findPlugin(st->type) : nullptr;
    return targetName(a, PluginTarget{t.owner, t.slot}) + " \xC2\xB7 " + (info ? info->name : std::string("Junction"));
  }
  if (t.isChannel()) {
    const Channel* c = s.channel(t.owner);
    return c ? c->name : "Channel";
  }
  int mi = s.mixerIndex(t.owner);
  return (mi >= 0 ? mixerTrackName(s, static_cast<size_t>(mi)) : std::string("Mixer")) + " \xC2\xB7 slot " + std::to_string(t.slot + 1);
}

// Every hosted plugin target in the song.
std::vector<PluginTarget> hostedTargets(const Song& s) {
  std::vector<PluginTarget> v;
  // K-7: installed plugins inside a Junction are targets of their own.
  const auto inside = [&v](const PluginState& st, ObjectId owner, int slot) {
    if (st.type != "gl.patcher" && st.type != "gl.patcherfx") return;
    // (Runs every frame: only a graph that names an installed plugin is worth reading.)
    bool any = false;
    for (const char* prefix : {"vst3:", "clap:", "vst2:", "au:", "lv2:"}) any = any || st.data.find(prefix) != std::string::npos;
    if (!any) return;
    for (const packs::modulation::JNode& n : packs::modulation::junctionOf(st).nodes)
      if (isHostedType(n.plugin.type)) v.push_back({owner, slot, n.id});
  };
  for (const auto& c : s.channels) {
    if (isHostedType(c.plugin.type)) v.push_back({c.id, -1});
    inside(c.plugin, c.id, -1);
  }
  for (const auto& m : s.mixer)
    for (size_t k = 0; k < m.slots.size(); ++k) {
      if (isHostedType(m.slots[k].plugin.type)) v.push_back({m.id, static_cast<int>(k)});
      inside(m.slots[k].plugin, m.id, static_cast<int>(k));
    }
  return v;
}

// Target key -> state serial of the history step the plugin's own latest edits made (its
// interface's parameter gestures, read-back values, captured state).
std::map<uint64_t, uint64_t> gOwnStep;

// Takes a change the plugin reported (its state, its values read back) into the song:
//  - right after the plugin's own edits (nothing else edited, undone or redone since): into
//    that same undo step, so one gesture in its interface stays one undo step;
//  - while nothing was edited since an undo / redo, and only shortly after it (K-141): without a
//    step of its own (the plugin only follows what the undo / redo restored; a new step would
//    throw the redo history away). A change made in its window later is the user's: a step;
//  - otherwise as an undo step of its own.
void takeOver(App& a, const PluginTarget& t, const PluginState& ns, const char* label, uint64_t mergeKey) {
  const uint64_t serial = a.doc.history().stateSerial();
  auto own = gOwnStep.find(t.key());
  if (own != gOwnStep.end() && own->second == serial) {
    a.doc.amendStep(serial, [&] { setPluginState(a, t, ns, label); });
  } else if (a.doc.moveSerial() != 0 && a.doc.moveSerial() == serial && a.now - undoui::lastMoveAt() < undoui::kFollowSeconds) {
    a.doc.patchSong([&](Song& s) {
      if (t.isChannel()) {
        if (Channel* c = s.channel(t.owner)) {
          if (t.node) packs::modulation::junctionSetNodeState(c->plugin, t.node, ns);
          else c->plugin = ns;
        }
      } else if (MixerTrack* m = s.mixerTrack(t.owner); m && t.slot < static_cast<int>(m->slots.size())) {
        if (t.node) packs::modulation::junctionSetNodeState(m->slots[static_cast<size_t>(t.slot)].plugin, t.node, ns);
        else m->slots[static_cast<size_t>(t.slot)].plugin = ns;
      }
    });
    return;
  } else {
    setPluginState(a, t, ns, label, mergeKey);
  }
  gOwnStep[t.key()] = a.doc.history().stateSerial();
}

// Stores captured plugin state in the song (see takeOver).
void storeState(App& a, const PluginTarget& t, const std::string& data) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->data == data) return;
  PluginState ns = *st;
  ns.data = data;
  takeOver(a, t, ns, "plugin state", t.key() ^ 0x7374617465ull);
}

std::map<uint64_t, double> gCaptureDue;  // target key → time of the pending state capture

}  // namespace

std::string hostedTargetName(App& a, const PluginTarget& t) { return targetName(a, t); }

// ---------------------------------------------------------------- per-frame sync

void pumpHostedPlugins(App& a) {
#if !defined(__APPLE__) && !defined(_WIN32)  // X11 (Linux, the BSDs)
  vst3::runLoop()->pump();
#endif
  hostedui::pluginWindowsIdle(a);
  // Hosted plugin slots, listed again only when the project changes (J-25:
  // projects with hundreds of mixer slots).
  static std::vector<PluginTarget> targetCache;
  static uint64_t targetRev = ~0ull;
  if (targetRev != a.doc.revision()) {
    targetRev = a.doc.revision();
    targetCache = hostedTargets(a.doc.song());
  }
  const std::vector<PluginTarget> targets = targetCache;  // copy: the loop may edit the song
  for (const PluginTarget& t : targets) {
    host::HostedInstance* hi = hostedInstanceFor(a, t);
    if (!hi) continue;
    host::UiUpdate u;
    hi->pumpUi(u);
    if (u.crashed) {
      closeHostedEditor(a, t);
      a.notify(hi->hostedClass().name + " crashed (" + hi->crashReason() + "). It is silenced; reload it from its plugin window.", true);
      continue;
    }
    if (t.node) {  // K-7: a module's values live in the Junction's data
      if (const PluginState* st = !u.edits.empty() ? t.state(a.doc.song()) : nullptr) {
        PluginState ns = *st;
        for (const auto& e : u.edits)
          if (e.index >= 0 && static_cast<size_t>(e.index) < ns.params.size()) ns.params[static_cast<size_t>(e.index)] = static_cast<float>(e.value);
        setPluginState(a, t, ns, "plugin parameters", t.key() ^ 0x706172616d73ull);
      }
    } else {
      for (const auto& e : u.edits) a.doc.setParam(t.pid(e.index), e.value, true);
    }
    if (!u.edits.empty()) gOwnStep[t.key()] = a.doc.history().stateSerial();
    if (u.gestureEnd) a.doc.endGesture();
    if (u.readback) {
      const PluginState* st = t.state(a.doc.song());
      if (st && st->params != u.values) {
        PluginState ns = *st;
        ns.params = u.values;
        normalizePluginState(ns);
        takeOver(a, t, ns, "plugin parameters", t.key() ^ 0x706172616d73ull);
      }
    }
    if (u.captureState && !gCaptureDue.count(t.key())) gCaptureDue[t.key()] = a.now + 0.4;
  }
  for (auto it = gCaptureDue.begin(); it != gCaptureDue.end();) {
    if (a.now < it->second) {
      ++it;
      continue;
    }
    for (const PluginTarget& t : targets)
      if (t.key() == it->first)
        if (host::HostedInstance* hi = hostedInstanceFor(a, t)) {
          std::string data;
          if (hi->captureState(data)) storeState(a, t, data);
        }
    it = gCaptureDue.erase(it);
  }
}

void captureHostedStates(App& a) {
  crash::BusyScope busy("reading plugin states", 120);
  for (const PluginTarget& t : hostedTargets(a.doc.song()))
    if (host::HostedInstance* hi = hostedInstanceFor(a, t)) {
      std::string data;
      if (hi->captureState(data)) storeState(a, t, data);
    }
  gCaptureDue.clear();
}

bool selectHostedProgram(App& a, const PluginTarget& t, int index) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || index < 0 || index >= hi->programCount()) return false;
  return hi->selectProgram(index);
}

void reloadHostedPlugin(App& a, const PluginTarget& t) {
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi) return;
  closeHostedEditor(a, t);
  if (!hi->crashed()) {  // keep what it has now
    std::string data;
    if (hi->captureState(data)) storeState(a, t, data);
  }
  hi->requestReload();
  a.syncEngine();
}

void setHostedSandbox(App& a, const std::string& typeRef, bool on) {
  const std::string type = typeRef;  // the caller's string may belong to a window this closes
  if (host::runsInSandbox(type) == on) return;
  captureHostedStates(a);  // the new instance starts from the current state
  for (const PluginTarget& t : hostedTargets(a.doc.song())) {
    const PluginState* st = t.state(a.doc.song());
    if (st && st->type == type && hostedEditorOpen(t)) {
      closeHostedEditor(a, t);
      hostedui::reopenLater(t);  // the new instance shows its interface again
    }
  }
  host::setRunInSandbox(type, on);
  a.syncEngine();
}

// ---------------------------------------------------------------- generic editor

namespace {

// One parameter row: name left, value bar with the plugin's own value text.
void paramRow(App& a, const PluginTarget& t, host::HostedInstance* hi, int k, const ParamDesc& d, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ParamId pid = t.pid(k);
  double val = getParam(s, pid).value_or(d.def);
  ImGui::PushID(k);
  const float h = 22, nameW = std::min(220.0f, width * 0.38f), barW = width - nameW - 8;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  theme::textEllipsis(dl, a.fontRegular, 12.0f, ImVec2(p.x + 2, p.y + 4), nameW - 6, P.textDim, d.name.c_str());
  ImVec2 b0(p.x + nameW, p.y), b1(p.x + nameW + barW, p.y + h);
  ImGui::SetCursorScreenPos(b0);
  ImGui::InvisibleButton("##bar", ImVec2(barW, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const double n0 = d.toNormalized(val);
  if (ImGui::IsItemClicked() && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)) {
    a.doc.setParam(pid, d.def, false);
  } else if (d.kind == ParamKind::Toggle) {
    if (ImGui::IsItemClicked()) a.doc.setParam(pid, val >= 0.5 ? 0.0 : 1.0, false);
  } else if (d.kind == ParamKind::Choice) {
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##choices");
  } else if (act) {
    float fine = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
    float dx = ImGui::GetIO().MouseDelta.x;
    if (dx != 0) a.doc.setParam(pid, d.fromNormalized(std::clamp(n0 + dx / barW * fine, 0.0, 1.0)), true);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::BeginPopupContextItem("##ctx")) {  // reset, type a value, MIDI link, automation clip…
    knobContextMenu(a, pid);
    ImGui::EndPopup();
  }
  if (hov && ImGui::GetIO().MouseWheel != 0 && d.kind == ParamKind::Continuous) {
    a.doc.setParam(pid, d.fromNormalized(std::clamp(n0 + ImGui::GetIO().MouseWheel * 0.01, 0.0, 1.0)), true);
    a.doc.endGesture();
  }
  if (ImGui::BeginPopup("##choices")) {
    for (size_t c = 0; c < d.choices.size(); ++c)
      if (ImGui::Selectable(d.choices[c].c_str(), static_cast<int>(std::lround(val)) == static_cast<int>(c)))
        a.doc.setParam(pid, static_cast<double>(c), false);
    ImGui::EndPopup();
  }
  dl->AddRectFilled(b0, b1, hov || act ? P.surface700 : theme::mix(P.surface800, P.surface700, 0.5f), 4);
  const float n = static_cast<float>(d.toNormalized(val));
  if (d.kind == ParamKind::Toggle) {
    ui::Led(dl, ImVec2(b0.x + 10, b0.y + h * 0.5f), 4, val >= 0.5);
  } else {
    float xv = b0.x + barW * n;
    dl->AddRectFilled(ImVec2(b0.x, b1.y - 3), ImVec2(std::max(b0.x + 1, xv), b1.y - 1), P.accent, 1);
    if (d.kind == ParamKind::Continuous) dl->AddLine(ImVec2(xv, b0.y + 3), ImVec2(xv, b1.y - 1), P.paper, 2);
  }
  std::string text = hi ? hi->paramText(k, static_cast<float>(val)) : std::string();
  if (text.empty()) text = d.format(val);
  if (!d.unit.empty() && d.kind == ParamKind::Continuous && text.find(d.unit) == std::string::npos) text += " " + d.unit;
  theme::textEllipsis(dl, a.fontMono, 10.5f, ImVec2(b0.x + (d.kind == ParamKind::Toggle ? 20 : 7), b0.y + 5), barW - 14, P.text,
                      text.c_str());
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 3));
  ImGui::Dummy(ImVec2(width, 0));
  ImGui::PopID();
}

// Mixer tracks no channel output and no effect uses yet.
bool trackFree(const Song& s, const MixerTrack& m) {
  if (m.id == s.masterId()) return false;
  for (const auto& c : s.channels) {
    if (c.mixerTrack == m.id) return false;
    for (ObjectId x : c.auxOuts)
      if (x == m.id) return false;
  }
  for (const auto& sl : m.slots)
    if (!sl.plugin.empty()) return false;
  return true;
}

// Extra outputs of a multi-output instrument → mixer tracks.
void outputsSection(App& a, const PluginTarget& t, const host::HostedClass& cls, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Channel* ch = s.channel(t.owner);
  if (!ch) return;
  pui::beginSection("OUTPUTS", width);
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Output 1 plays through the channel. Send the other outputs to mixer tracks:"));
  const size_t n = cls.auxOutputs.size();
  for (size_t k = 0; k < n; ++k) {
    ImGui::PushID(static_cast<int>(k));
    ObjectId cur = k < ch->auxOuts.size() ? ch->auxOuts[k] : kNoId;
    const int mi = cur == kNoId ? -1 : s.mixerIndex(cur);
    std::string label = mi < 0 ? std::string("(not used)") : mixerTrackName(s, static_cast<size_t>(mi));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(theme::fitText(cls.auxOutputs[k].c_str(), 160));
    ImGui::SameLine(170);
    if (ui::BeginCombo("##out", label.c_str(), std::min(220.0f, width - 190))) {
      auto pick = [&](ObjectId id) {
        a.doc.editChannel(t.owner, "route plugin output", [k, id](Channel& c) {
          if (c.auxOuts.size() <= k) c.auxOuts.resize(k + 1, kNoId);
          c.auxOuts[k] = id;
          while (!c.auxOuts.empty() && c.auxOuts.back() == kNoId) c.auxOuts.pop_back();
        });
      };
      if (ImGui::Selectable(i18n::tr("(not used)"), cur == kNoId)) pick(kNoId);
      for (size_t m = 1; m < s.mixer.size(); ++m)
        if (ImGui::Selectable((mixerTrackName(s, m) + "##m" + std::to_string(m)).c_str(), s.mixer[m].id == cur)) pick(s.mixer[m].id);
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
  if (ui::TextButton("Send outputs to free mixer tracks", false, 0, "Routes every extra output to its own unused mixer track and names the track")) {
    a.doc.editSong("route plugin outputs", [&](Song& song) {
      Channel* c = song.channel(t.owner);
      if (!c) return;
      c->auxOuts.assign(n, kNoId);
      size_t next = 1;
      for (size_t k = 0; k < n; ++k) {
        while (next < song.mixer.size() && !trackFree(song, song.mixer[next])) ++next;
        if (next >= song.mixer.size()) break;
        MixerTrack& m = song.mixer[next];
        c->auxOuts[k] = m.id;
        m.name = c->name + " " + cls.auxOutputs[k];
        ++next;
      }
    });
  }
  pui::endSection();
}

}  // namespace

// ---------------------------------------------------------------- plugin wrapper window
//
// The window body of a hosted plugin follows the layout of a DAW plugin
// wrapper: a header strip (plugin name, editor / generic / sandbox switches,
// preset browser, CPU and latency readouts) over three pages — the generic
// parameter list, the processing options and the wrapper settings (MIDI
// ports with detection, sidechain input, extra outputs, plugin facts).

namespace {

struct WrapperUi {
  int tab = 0;
  float cpuAvg = 0, cpuPeak = 0;
  double cpuAt = 0;
  int preset = -1;  // index into the combined program + library list
  uint64_t midiAtDetect = 0;
  double detectUntil = 0;
};
int gWrapperPage = 0;
std::map<uint64_t, WrapperUi>& wrapperUi() {
  static std::map<uint64_t, WrapperUi> m;
  return m;
}
// J-213: the picked preset belongs to the plugin it was picked on: another
// plugin in the same channel or slot (a replace, an undo) starts with none.
std::map<uint64_t, std::string>& wrapperTypes() {
  static std::map<uint64_t, std::string> m;
  return m;
}
WrapperUi& wrapperFor(const PluginTarget& t, host::HostedInstance* hi) {
  WrapperUi& w = wrapperUi()[t.key()];
  if (hi) {
    std::string& type = wrapperTypes()[t.key()];
    if (type != hi->hostedClass().type) {
      type = hi->hostedClass().type;
      w.preset = -1;
    }
  }
  return w;
}

// Slate wrapper palette (panel roles of a plugin wrapper; drawn by Gridline).
struct WrapLook {
  ImU32 top = skin::hex(0x4A545A), bottom = skin::hex(0x353D42);
  ImU32 text = skin::hex(0xE4ECEE), dim = skin::hex(0x9FA8AD);
  ImU32 btn = skin::hex(0x2F383D), btnOn = skin::hex(0x3D63DD), btnText = skin::hex(0xDDE4E7);
  ImU32 lcd = skin::hex(0x1A1E21), lcdText = skin::hex(0x9FE0A8);
  ImU32 warn = skin::hex(0xE5484D);
};

// Processing options changed: structural changes need a new instance (the
// current state is captured first, editors of that type close).
void applyOptions(App& a, const std::string& type, const host::HostOptions& o) {
  const host::HostOptions cur = host::hostOptions(type);
  if (cur == o) return;
  if (!cur.sameStructure(o)) {
    captureHostedStates(a);
    for (const PluginTarget& t : hostedTargets(a.doc.song())) {
      const PluginState* st = t.state(a.doc.song());
      if (st && st->type == type && hostedEditorOpen(t)) {
        closeHostedEditor(a, t);
        if (!o.genericEditor) hostedui::reopenLater(t);
      }
    }
  }
  host::setHostOptions(type, o);
  a.syncEngine();
}

// Programs first, then the plugin's library presets.
int presetCount(host::HostedInstance* hi) { return hi->programCount() + static_cast<int>(hi->libraryPresets().size()); }
std::string presetName(host::HostedInstance* hi, int i) {
  if (i < hi->programCount()) return hi->programName(i);
  const auto& lib = hi->libraryPresets();
  const size_t k = static_cast<size_t>(i - hi->programCount());
  return k < lib.size() ? lib[k].name : std::string();
}
bool selectPreset(App& a, const PluginTarget& t, host::HostedInstance* hi, int i) {
  if (i < 0 || i >= presetCount(hi)) return false;
  if (i < hi->programCount()) return selectHostedProgram(a, t, i);
  return hi->loadLibraryPreset(static_cast<size_t>(i - hi->programCount()));
}

void header(App& a, const PluginTarget& t, host::HostedInstance* hi, const PluginInfo& info, WrapperUi& w, float width) {
  const WrapLook L;
  const host::HostedClass& cls = hi->hostedClass();
  const host::HostOptions opts = host::hostOptions(cls.type);  // as chosen now (the instance may predate it)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Form F = skin::formAt(640, 94, width, 1.6f);
  skin::gradient(dl, F, 0, 0, 640, 94, L.top, L.bottom);
  char line[256];
  skin::label(dl, F, 10, 5, 330, 18, cls.name.c_str(), L.text, 0, 14.5f, 0, true, false);
  std::snprintf(line, sizeof line, "%s  \xC2\xB7  %s%s  \xC2\xB7  %s", cls.vendor.empty() ? "Unknown vendor" : cls.vendor.c_str(), cls.formatLabel(),
                hi->sandboxed() ? " (sandbox)" : "", cls.instrument ? "instrument" : "effect");
  skin::label(dl, F, 10, 23, 330, 14, line, L.dim, 0, 10.5f, 0, false, false);

  // Generic / sandbox. The plugin's own interface is its window: this
  // panel only shows for plugins without one, when the generic editor is
  // chosen, or when asked for from the plugin window's settings.
  const bool hasUi = hi->backend()->editorAvailable();
  if (skin::button(F, "##gen", 10, 44, 82, 20, "GENERIC", opts.genericEditor || !hasUi ? L.btnOn : L.btn, hasUi ? L.btnText : L.dim,
                   hasUi ? (opts.genericEditor ? "Switch back to the plugin's own interface"
                                               : "Use Gridline's parameter list as this plugin's editor instead of its own interface")
                         : "This plugin has no interface of its own: Gridline shows its parameters")) {
    if (hasUi) {
      hostedui::setGenericEditor(a, cls.type, !opts.genericEditor);
      w.tab = 0;
      return;
    }
  }
  if (skin::button(F, "##sb", 96, 44, 76, 20, "SANDBOX", hi->sandboxed() ? L.btnOn : L.btn, L.btnText,
                   "Runs this plugin in its own process: if it crashes, Gridline keeps running and the plugin is silenced")) {
    setHostedSandbox(a, cls.type, !hi->sandboxed());
    return;  // the instance is being replaced
  }
  if (hasUi && !opts.genericEditor && !hostedEditorOpen(t) &&
      skin::button(F, "##show", 176, 44, 96, 20, "INTERFACE", L.btn, L.btnText, "Shows the plugin's own interface")) {
    openHostedEditor(a, t);
  }

  // Presets: previous / next / browser.
  const int np = presetCount(hi);
  if (skin::button(F, "##pp", 280, 44, 18, 20, GL_ICON_LABEL("chevron-left", "<"), L.btn, L.btnText, "Previous preset") && np > 0) {
    w.preset = w.preset <= 0 ? np - 1 : w.preset - 1;
    selectPreset(a, t, hi, w.preset);
  }
  std::string pname = w.preset >= 0 && w.preset < np ? presetName(hi, w.preset) : (np > 0 ? "Plugin presets" : "No plugin presets");
  if (skin::button(F, "##pn", 300, 44, 196, 20, theme::fitText(pname.c_str(), F.u(186)), L.lcd, L.lcdText,
                   "The plugin's own programs and preset library") &&
      np > 0)
    ImGui::OpenPopup("##wrapperPresets");
  if (skin::button(F, "##pnx", 498, 44, 18, 20, GL_ICON_LABEL("chevron-right", ">"), L.btn, L.btnText, "Next preset") && np > 0) {
    w.preset = (w.preset + 1) % np;
    selectPreset(a, t, hi, w.preset);
  }
  if (ImGui::BeginPopup("##wrapperPresets")) {
    if (hi->programCount() > 0) ImGui::TextDisabled("%s", i18n::tr("Programs"));
    for (int k = 0; k < hi->programCount(); ++k)
      if (ImGui::Selectable((hi->programName(k) + "##p" + std::to_string(k)).c_str(), k == w.preset) && selectPreset(a, t, hi, k)) w.preset = k;
    std::string cat = "\x01";
    const auto& lib = hi->libraryPresets();
    for (size_t k = 0; k < lib.size(); ++k) {
      if (lib[k].category != cat) {
        cat = lib[k].category;
        ImGui::TextDisabled("%s", cat.empty() ? "Library" : cat.c_str());
      }
      const int idx = hi->programCount() + static_cast<int>(k);
      if (ImGui::Selectable((lib[k].name + "##l" + std::to_string(k)).c_str(), idx == w.preset) && selectPreset(a, t, hi, idx)) w.preset = idx;
    }
    ImGui::EndPopup();
  }

  // CPU (refreshed four times a second) and latency.
  if (a.now - w.cpuAt > 0.25) {
    float avg = 0, pk = 0;
    hi->takeCpu(avg, pk);
    w.cpuAvg = avg;
    w.cpuPeak = pk;
    w.cpuAt = a.now;
  }
  ImVec2 b0 = F.p(530, 8), b1 = F.p(630, 18);
  dl->AddRectFilled(b0, b1, L.lcd, F.u(2));
  const float fillW = std::clamp(w.cpuAvg, 0.0f, 1.0f) * (b1.x - b0.x);
  dl->AddRectFilled(b0, ImVec2(b0.x + fillW, b1.y), w.cpuAvg > 0.5f ? L.warn : L.btnOn, F.u(2));
  const float pkX = b0.x + std::clamp(w.cpuPeak, 0.0f, 1.0f) * (b1.x - b0.x);
  dl->AddLine(ImVec2(pkX, b0.y), ImVec2(pkX, b1.y), L.text, 1.0f);
  std::snprintf(line, sizeof line, "CPU %.1f%%  peak %.0f%%", 100.0 * w.cpuAvg, 100.0 * w.cpuPeak);
  skin::label(dl, F, 520, 20, 112, 12, line, L.dim, 0, 9.5f, 2, false, false);
  const int lat = hi->totalLatency();
  const double sr = hi->backend()->sampleRate() > 0 ? hi->backend()->sampleRate() : 48000.0;
  std::snprintf(line, sizeof line, "LAT %d smp  %.1f ms", lat, 1000.0 * lat / sr);
  skin::label(dl, F, 520, 32, 112, 12, line, L.dim, 0, 9.5f, 2, false, false);
  std::snprintf(line, sizeof line, "%zu params", info.params.size());
  skin::label(dl, F, 520, 46, 112, 12, line, L.dim, 0, 9.5f, 2, false, false);

  // Pages.
  static const char* const kTabs[] = {"PARAMETERS", "PROCESSING", "SETTINGS"};
  skin::TabLook tl;
  skin::tabsUi(F, "##wtabs", 10, 70, 360, 20, kTabs, 3, w.tab, tl);
  skin::reserve(F);
}

void processingPage(App& a, host::HostedInstance* hi, float width) {
  const host::HostedClass& cls = hi->hostedClass();
  host::HostOptions o = hi->options();
  const host::HostOptions before = o;
  pui::beginSection("PROCESSING", width);
  auto row = [&](const char* label, bool& v, const char* hint) {
    if (ui::TextButton(label, v, 230, hint)) v = !v;
  };
  row("Use fixed size buffers", o.fixedBuffers,
      "The plugin always processes the same number of samples (adds that many samples of latency, compensated). For plugins that misbehave with "
      "varying buffer sizes.");
  if (o.fixedBuffers) {
    ImGui::SameLine(0, 8);
    char sz[32];
    std::snprintf(sz, sizeof sz, "%d samples", o.fixedSize);
    if (ui::BeginCombo("##fixed", sz, 130)) {
      for (int n : {64, 128, 256, 512, 1024})
        if (ImGui::Selectable((std::to_string(n) + " samples").c_str(), n == o.fixedSize)) o.fixedSize = n;
      ImGui::EndCombo();
    }
  }
  row("Process maximum size buffers", o.maxBuffers,
      "One process call per audio block with every note placed inside it at its exact sample. Off: the block is cut at each note (smaller, "
      "varying buffers).");
  row("Notify about rendering mode", o.notifyRender, "Tells the plugin when a song is exported (offline), so it can use its highest quality mode.");
  {  // K-144: how slide notes, porta and bend lines reach the plugin
    static const char* const kPitch[] = {i18n::N_("Automatic"), i18n::N_("MPE (a channel per note)"), i18n::N_("Channel pitch bend"), i18n::N_("Off")};
    const char* const kPitchHint[] = {
        i18n::N_("Slide notes and bend lines tune each note where the plugin can, otherwise they bend the note's channel."),
        i18n::N_("Every note plays on its own MIDI channel with its own pitch bend, for plugins that understand MPE."),
        i18n::N_("Slide notes and bend lines move the pitch bend of the note's channel (one bend per channel)."),
        i18n::N_("Notes keep their pitch: slide notes and bend lines are not sent.")};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Per-note pitch"));
    ImGui::SameLine(0, 8);
    const int cur = std::clamp(o.notePitch, 0, 3);
    if (ui::BeginCombo("##notepitch", i18n::tr(kPitch[cur]), 200)) {
      for (int k = 0; k < 4; ++k) {
        if (ImGui::Selectable(i18n::tr(kPitch[k]), k == cur)) o.notePitch = k;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr(kPitchHint[k]));
      }
      ImGui::EndCombo();
    }
    if (o.notePitch == host::HostOptions::PitchMpe) {
      ImGui::SameLine(0, 8);
      char rng[32];
      std::snprintf(rng, sizeof rng, i18n::tr("Bend range %d st"), o.mpeBendRange);
      if (ui::BeginCombo("##mperange", rng, 130)) {
        for (int n : {2, 12, 24, 48, 96}) {
          std::snprintf(rng, sizeof rng, i18n::tr("%d semitones"), n);
          if (ImGui::Selectable(rng, n == o.mpeBendRange)) o.mpeBendRange = n;
        }
        ImGui::EndCombo();
      }
    }
  }
  if (cls.format == "clap")
    row("Links modulate parameters", o.modulation,
        "Controller links and automation move modulatable parameters as modulation: the plugin keeps the song's value and shows the movement "
        "on top.");
  bool sandbox = hi->sandboxed();
  if (ui::TextButton("Make bridged (run in sandbox)", sandbox, 230,
                     "Runs this plugin in its own process: a crash silences it instead of closing Gridline (slightly more CPU)")) {
    setHostedSandbox(a, cls.type, !sandbox);
    pui::endSection();
    return;
  }
  ImGui::Spacing();
  const double sr = hi->backend()->sampleRate() > 0 ? hi->backend()->sampleRate() : 48000.0;
  const int tail = hi->backend()->tailSamples();
  const std::string tailText = tail >= (1 << 28) ? std::string("infinite") : std::to_string(tail) + " samples";
  ImGui::TextColored(theme::vec(theme::pal().textDim), i18n::tr("Latency: plugin %d + buffer %d = %d samples (%.1f ms), compensated."),
                     hi->backend()->latency(), hi->totalLatency() - hi->backend()->latency(), hi->totalLatency(), 1000.0 * hi->totalLatency() / sr);
  ImGui::TextColored(theme::vec(theme::pal().textDim), i18n::tr("Tail: %s."), tailText.c_str());
  pui::endSection();
  if (o != before) applyOptions(a, cls.type, o);
}

void settingsPage(App& a, const PluginTarget& t, host::HostedInstance* hi, WrapperUi& w, float width) {
  const auto& P = theme::pal();
  const host::HostedClass& cls = hi->hostedClass();
  const Song& s = a.doc.song();
  // MIDI ports and detection.
  pui::beginSection("MIDI", width);
  const bool midiIn = hi->backend()->hasEventInput(), midiOut = hi->hasMidiOutput();
  ImGui::TextColored(theme::vec(P.textDim), i18n::tr("Input: %s  \xC2\xB7  Output: %s"), midiIn ? "notes / MIDI" : "none", midiOut ? "notes / MIDI" : "none");
  ImGui::SameLine(0, 12);
  if (ui::TextButton("Detect", a.now < w.detectUntil, 0, "Watches the plugin's MIDI output for a few seconds (play some notes)")) {
    w.midiAtDetect = hi->midiOutCount();
    w.detectUntil = a.now + 6.0;
  }
  if (a.now < w.detectUntil || w.detectUntil > 0) {
    const uint64_t sent = hi->midiOutCount() - w.midiAtDetect;
    ImGui::SameLine(0, 8);
    ImGui::TextColored(theme::vec(sent > 0 ? P.accent : P.textFaint), "%s",
                       sent > 0 ? ("sending MIDI (" + std::to_string(sent) + " messages)").c_str()
                                : (a.now < w.detectUntil ? "listening\xE2\x80\xA6" : "no MIDI output seen"));
  }
  if (const Channel* ch = t.isChannel() ? s.channel(t.owner) : nullptr) {
    auto portCombo = [&](const char* id, const char* label, int cur, bool out) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(150);
      std::string prev = cur < 0 ? std::string("(none)") : "Port " + std::to_string(cur);
      if (ui::BeginCombo(id, prev.c_str(), 140)) {
        for (int p = -1; p < 64; ++p) {
          std::string nm = p < 0 ? std::string("(none)") : "Port " + std::to_string(p);
          if (ImGui::Selectable(nm.c_str(), p == cur))
            a.doc.editChannel(t.owner, out ? "MIDI output port" : "MIDI input port", [p, out](Channel& c) { (out ? c.midiOutPort : c.midiInPort) = p; });
        }
        ImGui::EndCombo();
      }
    };
    portCombo("##min", "Input port", ch->midiInPort, false);
    ui::Hint("This channel also plays the notes plugins send on this port");
    portCombo("##mout", "Output port", ch->midiOutPort, true);
    ui::Hint("Notes this plugin sends (arpeggiators, chord tools) play every channel listening on this port");
    if (ch->midiOutPort >= 0) {
      int listeners = 0;
      for (const auto& c : s.channels) listeners += c.id != ch->id && c.midiInPort == ch->midiOutPort;
      ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%d channel%s listening on port %d"), listeners, listeners == 1 ? "" : "s", ch->midiOutPort);
    }
  } else {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("MIDI ports apply to plugins on channels."));
  }
  pui::endSection();

  // Sidechain input (effects): fed by sidechain sends of mixer tracks.
  if (!t.isChannel() && hi->backend()->sidechainChannels() > 0) {
    pui::beginSection("SIDECHAIN", width);
    std::vector<std::string> from;
    for (size_t m = 0; m < s.mixer.size(); ++m)
      for (const Send& snd : s.mixer[m].sends)
        if (snd.sidechain && snd.to == t.owner) from.push_back(mixerTrackName(s, m));
    std::string list;
    for (const auto& f : from) list += (list.empty() ? "" : ", ") + f;
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%d-channel sidechain input. Fed by: %s"), hi->backend()->sidechainChannels(),
                       from.empty() ? "nothing yet" : list.c_str());
    if (ui::BeginCombo("##scadd", "Add sidechain from\xE2\x80\xA6", 220)) {
      for (size_t m = 0; m < s.mixer.size(); ++m) {
        if (s.mixer[m].id == t.owner || s.mixer[m].id == s.masterId()) continue;
        if (ImGui::Selectable((mixerTrackName(s, m) + "##sc" + std::to_string(m)).c_str())) {
          const ObjectId src = s.mixer[m].id, dst = t.owner;
          a.doc.editSong("sidechain send", [src, dst](Song& song) {
            MixerTrack* mt = song.mixerTrack(src);
            if (!mt) return;
            for (const Send& x : mt->sends)
              if (x.to == dst && x.sidechain) return;
            Send sc;
            sc.to = dst;
            sc.sidechain = true;
            mt->sends.push_back(sc);
          });
        }
      }
      ImGui::EndCombo();
    }
    ui::Hint("Sends that track's audio to this track's sidechain input (it is not mixed into the track)");
    pui::endSection();
  }

  if (t.isChannel() && !cls.auxOutputs.empty()) outputsSection(a, t, cls, width);

  pui::beginSection("PLUGIN", width);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(cls.type.c_str(), width - 24));
  ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(cls.bundle.c_str(), width - 24));
  ImGui::TextColored(theme::vec(P.textDim), i18n::tr("version %s  \xC2\xB7  %s"), cls.version.empty() ? "?" : cls.version.c_str(),
                     cls.subCategories.empty() ? "-" : cls.subCategories.c_str());
  ImGui::PopFont();
  pui::endSection();
}

// K-78: a plugin that is not installed in its saved format: the installed plugins that may be it, in
// other formats, best first. Nothing is replaced until one is picked (a low-confidence match never
// loads on its own when the project opens).
void otherFormatsSection(App& a, const PluginTarget& t, const PluginState& st, float width) {
  const auto& P = pal();
  const host::ident::Identity id = host::xformat::savedIdentity(st);
  static uint64_t gen = ~uint64_t(0);
  static std::string key;
  static std::vector<host::ident::Candidate> ranked;
  if (gen != host::hostedClassesGeneration() || key != st.type + "|" + st.ident) {  // ranked once per plugin and class set
    gen = host::hostedClassesGeneration();
    key = st.type + "|" + st.ident;
    ranked = host::xformat::candidatesFor(id);
  }
  pui::beginSection("OTHER FORMATS", width);
  if (!id.name.empty())
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("Saved as %s%s%s"), id.name.c_str(), id.vendor.empty() ? "" : " by ", id.vendor.c_str());
  if (ranked.empty()) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No installed format of this plugin was found."));
    pui::endSection();
    return;
  }
  ImGui::TextWrapped("%s", i18n::tr("Installed plugins that may be the same plugin. Pick one to use it here: its settings are carried over where they can be."));
  ImGui::PushID("xformat");
  for (size_t i = 0; i < ranked.size() && i < 6; ++i) {
    const auto& c = ranked[i];
    ImGui::PushID(static_cast<int>(i));
    const char* conf = c.confidence >= host::ident::Confidence::High ? i18n::tr("sure match")
                       : c.confidence == host::ident::Confidence::Medium ? i18n::tr("likely match")
                                                                          : i18n::tr("possible match");
    std::string label = c.cls->name + " (" + c.cls->formatLabel() + ")";
    if (!c.cls->vendor.empty()) label += "  " + c.cls->vendor;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(theme::fitText(label.c_str(), std::max(60.0f, width - 190)));
    if (ImGui::IsItemHovered()) {
      std::string tip;
      for (const auto& r : c.reasons) tip += (tip.empty() ? "" : "\n") + r;
      if (c.paramsTotal > 0) tip += "\n" + std::to_string(c.paramsMatched) + " / " + std::to_string(c.paramsTotal) + " parameters";
      ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::vec(c.confidence >= host::ident::Confidence::Medium ? P.textDim : P.meterMid), "%s", conf);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), width - 60));
    if (ImGui::SmallButton(i18n::tr("Use"))) {
      host::xformat::Conversion cv;
      if (host::xformat::convert(st, id, *c.cls, c.confidence, cv)) {
        setPluginState(a, t, cv.state, std::string("Use ") + label);
        a.syncEngine();
        a.notify(host::xformat::describe(cv.note));
      }
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  pui::endSection();
}

}  // namespace

void drawHostedEditor(App& a, const PluginTarget& t, float width) {
  const auto& P = pal();
  const PluginState* st = t.state(a.doc.song());
  const PluginInfo* info = st ? findPlugin(st->type) : nullptr;
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!info || !hi) {
    pui::beginSection("HOSTED PLUGIN", width);
    ImGui::TextColored(theme::vec(P.danger), "%s", info ? "This plugin could not be loaded." : "This plugin is not installed or not scanned yet.");
    ImGui::TextColored(theme::vec(P.textFaint), "%s", st ? st->type.c_str() : "");
    std::string err = host::lastError();
    if (!err.empty()) ImGui::TextWrapped("%s", err.c_str());
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Its parameters and state stay in the project."));
    if (info && ui::TextButton("Try again", false, 0, "Loads the plugin again")) a.syncEngine();
    pui::endSection();
    if (!info && st) otherFormatsSection(a, t, *st, width);
    return;
  }
  const host::HostedClass& cls = hi->hostedClass();
  if (hi->crashed()) {
    pui::beginSection("HOSTED PLUGIN", width);
    ImGui::TextColored(theme::vec(P.danger), i18n::tr("%s crashed and is silenced."), cls.name.c_str());
    ImGui::TextColored(theme::vec(P.textDim), "%s", hi->crashReason().c_str());
    if (ui::TextButton("Reload plugin", false, 0, "Starts the plugin again with the state saved in the project")) reloadHostedPlugin(a, t);
    pui::endSection();
    return;
  }
  const bool fresh = wrapperUi().try_emplace(t.key()).second;
  WrapperUi& w = wrapperFor(t, hi);
  if (fresh) w.tab = gWrapperPage;
  header(a, t, hi, *info, w, width);
  hostedui::drawPage(a, t, w.tab, width);
}

void hostedui::drawPage(App& a, const PluginTarget& t, int page, float width) {
  const auto& P = pal();
  const PluginState* st = t.state(a.doc.song());
  const PluginInfo* info = st ? findPlugin(st->type) : nullptr;
  host::HostedInstance* hi = hostedInstanceFor(a, t);  // (a switch may have replaced it)
  if (!info || !hi || hi->crashed()) return;
  WrapperUi& w = wrapperFor(t, hi);
  if (page == 1) {
    processingPage(a, hi, width);
    return;
  }
  if (page == 2) {
    settingsPage(a, t, hi, w, width);
    return;
  }

  static std::map<uint64_t, std::string> filters;
  std::string& filter = filters[t.key()];
  char buf[128];
  std::snprintf(buf, sizeof buf, "%s", filter.c_str());
  pui::beginSection("PARAMETERS", width);
  ImGui::SetNextItemWidth(std::min(260.0f, width - 24));
  if (ImGui::InputTextWithHint("##find", i18n::tr("Find parameter"), buf, sizeof buf)) filter = buf;
  std::string f = filter;
  for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::vector<int> rows;
  for (size_t k = 0; k < info->params.size(); ++k) {
    if (!f.empty()) {
      std::string nm = info->params[k].name;
      for (char& c : nm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (nm.find(f) == std::string::npos) continue;
    }
    rows.push_back(static_cast<int>(k));
  }
  const float rowH = 25.0f, listH = std::min(page == 3 ? 230.0f : host::hostOptions(hi->hostedClass().type).genericEditor ? 560.0f : 380.0f, std::max(rowH, rowH * static_cast<float>(rows.size())));
  ImGui::BeginChild("##params", ImVec2(width - 16, listH), ImGuiChildFlags_None);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(rows.size()), rowH);
  const float rw = ImGui::GetContentRegionAvail().x - 4;
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
      paramRow(a, t, hi, rows[static_cast<size_t>(r)], info->params[static_cast<size_t>(rows[static_cast<size_t>(r)])], rw);
  if (rows.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No parameter matches."));
  ImGui::PopStyleVar();
  ImGui::EndChild();
  pui::endSection();
}

}  // namespace gl

namespace gl {
void setHostedWrapperPage(int page) { gWrapperPage = std::clamp(page, 0, 2); }

namespace hostedui {
int presetCount(host::HostedInstance* hi) { return gl::presetCount(hi); }
std::string presetName(host::HostedInstance* hi, int index) { return gl::presetName(hi, index); }
bool selectPreset(App& a, const PluginTarget& t, host::HostedInstance* hi, int index) {
  if (!gl::selectPreset(a, t, hi, index)) return false;
  wrapperFor(t, hi).preset = index;
  return true;
}
int& presetIndex(const PluginTarget& t, host::HostedInstance* hi) { return wrapperFor(t, hi).preset; }
void cpuLoad(App& a, const PluginTarget& t, host::HostedInstance* hi, float& average, float& peak) {
  WrapperUi& w = wrapperUi()[t.key()];
  if (a.now - w.cpuAt > 0.25) {
    float avg = 0, pk = 0;
    hi->takeCpu(avg, pk);
    w.cpuAvg = avg;
    w.cpuPeak = pk;
    w.cpuAt = a.now;
  }
  average = w.cpuAvg;
  peak = w.cpuPeak;
}
void setGenericEditor(App& a, std::string type, bool on) {
  host::HostOptions o = host::hostOptions(type);
  if (o.genericEditor == on) return;
  o.genericEditor = on;
  // Windows of this type switch over: the plugin's interface closes and
  // Gridline's parameter list opens, or the other way round.
  std::vector<PluginTarget> shown;
  for (const PluginTarget& t : hostedTargets(a.doc.song())) {
    const PluginState* st = t.state(a.doc.song());
    if (!st || st->type != type) continue;
    const bool panel = !t.node && (t.isChannel() ? a.settingsChannel == t.owner : a.fxEditor == std::make_pair(t.owner, t.slot));
    if (hostedEditorOpen(t) || panel) shown.push_back(t);
    closeHostedEditor(a, t);
    if (panel) {
      if (t.isChannel())
        a.settingsChannel = kNoId;
      else
        a.fxEditor = {kNoId, -1};
    }
  }
  applyOptions(a, type, o);
  for (const PluginTarget& t : shown) {
    if (t.node) continue;  // (a module in a Junction opens from the Junction)
    if (t.isChannel())
      a.settingsChannel = t.owner;  // routePluginWindows opens the right editor
    else
      a.fxEditor = {t.owner, t.slot};
  }
}
}  // namespace hostedui
}  // namespace gl
