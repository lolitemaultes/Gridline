// Gridline — scripting in the app (see ScriptUi.h).
#include "../core/I18n.h"
#include "ScriptUi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../core/ControllerScript.h"
#include "../core/ProjectScript.h"
#include "../core/Log.h"
#include "../core/MenuOps.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ContextMenus.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

std::string userScriptsDir(const App& a, const char* kind) { return (fs::path(a.configDir()) / "scripts" / kind).string(); }
std::string shippedScriptsDir(const App& a, const char* kind) {
  return a.assetsDir.empty() ? std::string() : (fs::path(a.assetsDir) / "scripts" / kind).string();
}

namespace {

script::Limits appLimits(const App& a) {
  script::Limits l;
  l.seconds = std::clamp(a.settings.getNumber("scripts.maxSeconds"), 0.05, 30.0);
  l.instructions = static_cast<uint64_t>(std::clamp(a.settings.getNumber("scripts.maxMillionInstructions"), 1.0, 2000.0) * 1e6);
  return l;
}

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ================================================================ piano roll ====

struct Session {
  bool open = false, focus = false;
  script::PianoRollScript script;
  std::string id;
  ObjectId pat = kNoId, ch = kNoId;
  std::vector<Note> base;
  std::vector<bool> baseSel;
  std::vector<Marker> baseMarkers;
  std::vector<script::FormParam> values;
  uint32_t seed = 1;
  uint64_t key = 0;
  bool changed = false;  // the document holds a preview
  script::PreviewScheduler sched;  // J-40: small-budget previews while dragging, full runs otherwise
  std::string error;
  script::PianoRollResult last;
  double runMs = 0;
  bool lastLimitHit = false;  // the last run (preview or full) hit its limit
};
Session S;
uint64_t sessionCounter = 1;
std::string lastScriptPath;  // the last piano roll script opened (J-27)
std::map<std::string, std::vector<script::FormParam>> remembered;  // form values per script id
std::map<std::string, uint32_t> rememberedSeed;
std::vector<script::ScriptInfo> prList;
bool prScanned = false;

const std::vector<Note>* targetNotes(const Song& s) {
  const Pattern* p = s.pattern(S.pat);
  if (!p) return nullptr;
  static const std::vector<Note> empty;
  auto it = p->notes.find(S.ch);
  return it == p->notes.end() ? &empty : &it->second;
}

void setSelection(App& a, const std::vector<bool>& sel) {
  a.piano.selPattern = S.pat;
  a.piano.selChannel = S.ch;
  a.piano.selected.clear();
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) a.piano.selected.insert(i);
}

std::string label() { return "script: " + S.script.name(); }

// `full`: the configured limits; otherwise the small live-preview budget
// (J-40). A preview that runs out of budget leaves the last result in place.
void runPreview(App& a, bool full) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(S.pat);
  const Channel* c = s.channel(S.ch);
  if (!p || !c) return;
  script::PianoRollContext ctx;
  ctx.notes = S.base;
  ctx.sel = S.baseSel;
  ctx.markers = S.baseMarkers;
  ctx.ppq = s.ppq;
  ctx.tsNum = s.timeSigNum;
  ctx.tsDen = s.timeSigDen;
  ctx.length = s.patternContentLength(*p);
  ctx.root = a.piano.scaleRoot;
  ctx.scale = a.piano.scale;
  ctx.selStart = a.playlist.selStart;
  ctx.selEnd = a.playlist.selEnd;
  ctx.seed = S.seed;
  ctx.channel = c->name;
  ctx.pattern = p->name;
  const double t0 = wallNow();
  std::string root = S.script.path().empty() ? std::string() : fs::path(S.script.path()).parent_path().string();
  const script::Limits fullLimits = appLimits(a);
  script::PianoRollResult r = S.script.run(ctx, S.values, full ? fullLimits : script::previewLimits(fullLimits), root);
  S.runMs = (wallNow() - t0) * 1000.0;
  S.lastLimitHit = r.limitHit;
  if (!full && r.limitHit) return;  // too slow for a live preview: keep showing the last result
  S.last = std::move(r);
  for (const auto& l : S.last.log) logging::info("script", S.script.name() + ": " + l);
  if (!S.last.ok) {
    S.error = S.last.error;
    logging::error("script", S.script.name() + ": " + S.error);
    return;
  }
  S.error.clear();
  const std::vector<Note>* cur = targetNotes(s);
  if (cur && S.last.notes != *cur) {
    a.doc.setNotes(S.pat, S.ch, S.last.notes, label(), S.key);
    S.changed = true;
  }
  setSelection(a, S.last.sel);
}

void closeSession(App& a, bool keep) {
  remembered[S.id] = S.values;
  rememberedSeed[S.id] = S.seed;
  if (keep && S.last.ok && S.last.markersChanged) {
    // Notes and markers as one undo step: drop the preview step, then record both.
    if (S.changed) {
      a.doc.setNotes(S.pat, S.ch, S.base, label(), S.key);
      a.doc.undo();
    }
    a.doc.endGesture();
    a.doc.beginGroup(label());
    a.doc.setNotes(S.pat, S.ch, S.last.notes, label());
    std::vector<Marker> mk = S.last.markers;
    a.doc.editSong(label(), [&mk](Song& s) { s.markers = mk; });
    a.doc.endGroup();
    setSelection(a, S.last.sel);
  } else if (!keep && S.changed) {
    a.doc.setNotes(S.pat, S.ch, S.base, "cancel", S.key);
    setSelection(a, S.baseSel);
  } else if (!keep) {
    setSelection(a, S.baseSel);
  }
  a.doc.endGesture();
  S.open = false;
  S.changed = false;
}

// Knob cell: knob, name and value under it.
bool knobCell(const char* id, script::FormParam& p) {
  const auto& P = pal();
  const float cell = 76;
  ImGui::BeginGroup();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  const float r = 16;
  ImGui::SetCursorScreenPos(ImVec2(pos.x + (cell - 2 * r) * 0.5f, pos.y));
  ParamDesc d;
  d.min = p.min;
  d.max = p.max;
  d.def = p.value;
  d.name = p.name;
  double v = p.value;
  const bool changed = ui::Knob(id, &v, d, r, nullptr, false);
  if (changed) p.value = std::clamp(p.kind == script::FormKind::KnobInt ? std::round(v) : v, p.min, p.max);
  if (!p.hint.empty()) ui::Hint(p.name + ": " + p.hint);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y = pos.y + 2 * r + 4;
  char val[48];
  if (p.kind == script::FormKind::KnobInt)
    std::snprintf(val, sizeof val, "%lld", static_cast<long long>(std::llround(p.value)));
  else
    std::snprintf(val, sizeof val, std::fabs(p.max - p.min) <= 2 ? "%.2f" : "%.1f", p.value);
  auto centred = [&](ImFont* f, float size, ImU32 col, const char* t) {
    float tw = f->CalcTextSizeA(size, 1e9f, 0, t).x;
    if (tw > cell - 2)
      theme::textEllipsis(dl, f, size, ImVec2(pos.x + 1, y), cell - 2, col, t);
    else
      dl->AddText(f, size, ImVec2(std::round(pos.x + (cell - tw) * 0.5f), y), col, t);
    y += size + 3;
  };
  centred(app().fontRegular, theme::kFontLabel, P.textDim, p.name.c_str());
  centred(app().fontMono, theme::kFontLabel, P.text, val);
  ImGui::SetCursorScreenPos(pos);
  ImGui::Dummy(ImVec2(cell, y - pos.y));
  ImGui::EndGroup();
  return changed;
}

bool drawForm(std::vector<script::FormParam>& vals) {
  bool changed = false;
  std::string group = "\x01";
  int knobsInRow = 0;
  const int perRow = 6;
  auto newRow = [&]() {
    knobsInRow = 0;
  };
  for (size_t i = 0; i < vals.size(); ++i) {
    script::FormParam& p = vals[i];
    ImGui::PushID(static_cast<int>(i));
    if (p.group != group) {
      group = p.group;
      newRow();
      ImGui::Dummy(ImVec2(0, 2));
      if (!group.empty()) ui::Label(group.c_str());
    }
    const bool isKnob = p.kind == script::FormKind::Knob || p.kind == script::FormKind::KnobInt;
    if (isKnob) {
      if (knobsInRow > 0 && knobsInRow < perRow) ImGui::SameLine(0, 2);
      if (knobsInRow >= perRow) knobsInRow = 0;
      changed |= knobCell("##k", p);
      ++knobsInRow;
    } else {
      newRow();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(pal().textDim), "%s", p.name.c_str());
      ImGui::SameLine(150);
      if (p.kind == script::FormKind::Combo) {
        int idx = std::clamp(static_cast<int>(std::lround(p.value)) - 1, 0, static_cast<int>(p.items.size()) - 1);
        if (ui::BeginCombo("##c", p.items[static_cast<size_t>(idx)].c_str(), 220)) {
          for (size_t k = 0; k < p.items.size(); ++k)
            if (ImGui::Selectable(p.items[k].c_str(), static_cast<int>(k) == idx)) p.value = static_cast<double>(k + 1), changed = true;
          ImGui::EndCombo();
        }
      } else if (p.kind == script::FormKind::Check) {
        bool b = p.value >= 0.5;
        if (ImGui::Checkbox("##b", &b)) p.value = b ? 1 : 0, changed = true;
      } else {
        char buf[1024];
        std::snprintf(buf, sizeof buf, "%s", p.text.c_str());
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("##t", buf, sizeof buf)) p.text = buf, changed = true;
        ui::FocusRing();
      }
      if (!p.hint.empty()) ui::Hint(p.name + ": " + p.hint);
    }
    ImGui::PopID();
  }
  return changed;
}

// ============================================================ controllers ====

class AppHost final : public script::ControllerHost {
 public:
  AppHost(App& a, std::string device) : ControllerHost(a.doc), a_(a), device_(std::move(device)) {}
  void action(RemoteAction act) override { a_.runAction(act); }
  bool playing() const override { return a_.engine.playing(); }
  bool recording() const override { return a_.recordArmed; }
  bool songMode() const override { return a_.engine.mode() == PlayMode::Song; }
  Tick position() const override { return static_cast<Tick>(a_.engine.position()); }
  void setPosition(Tick t) override { a_.engine.setPosition(t); }
  int selectedMixer() const override {
    const auto& m = a_.doc.song().mixer;
    for (size_t i = 0; i < m.size(); ++i)
      if (m[i].id == a_.selMixer) return static_cast<int>(i);
    return 0;
  }
  void selectMixer(int index) override {
    const auto& m = a_.doc.song().mixer;
    if (index >= 0 && static_cast<size_t>(index) < m.size()) a_.selMixer = m[static_cast<size_t>(index)].id;
  }
  bool armed(int index) const override {
    const auto& m = a_.doc.song().mixer;
    return index >= 0 && static_cast<size_t>(index) < m.size() && m[static_cast<size_t>(index)].armed;
  }
  void setArmed(int index, bool on) override {
    if (armed(index) != on && index > 0) toggleArm(a_, a_.doc.song().mixer[static_cast<size_t>(index)].id);
  }
  int selectedChannel() const override {
    const auto& c = a_.doc.song().channels;
    for (size_t i = 0; i < c.size(); ++i)
      if (c[i].id == a_.selChannel) return static_cast<int>(i);
    return -1;
  }
  void selectChannel(int index) override {
    const auto& c = a_.doc.song().channels;
    if (index >= 0 && static_cast<size_t>(index) < c.size()) a_.selectChannel(c[static_cast<size_t>(index)].id);
  }
  std::pair<ObjectId, int> focusedPlugin() const override {
    if (a_.focusPluginOwner != kNoId) return {a_.focusPluginOwner, a_.focusPluginSlot};
    return {a_.selChannel, -1};
  }
  void midiOut(uint8_t s, uint8_t d1, uint8_t d2) override {
    if (a_.midi) a_.midi->send(s, d1, d2);
  }
  void sysexOut(const std::vector<uint8_t>& bytes) override {
    if (a_.midi) a_.midi->sendRaw(std::vector<unsigned char>(bytes.begin(), bytes.end()));
  }
  void hint(const std::string& text) override {
    hintText = text;
    hintUntil = wallNow() + 2.0;
  }
  std::string deviceName() const override { return device_; }
  std::string hintText;
  double hintUntil = 0;

 private:
  App& a_;
  std::string device_;
};

struct DeviceScript {
  std::string id;
  std::unique_ptr<AppHost> host;
  std::unique_ptr<script::ControllerScript> script;
  bool reported = false;
  double lastIdle = 0;
};
std::map<std::string, DeviceScript> devices;
std::vector<script::ScriptInfo> ctlList;
bool ctlScanned = false;

// Snapshot of what onRefresh reports.
struct Watch {
  uint64_t revision = ~0ull;
  bool playing = false, rec = false, song = false;
  ObjectId selMixer = kNoId, selChannel = kNoId, plugOwner = kNoId;
  int plugSlot = -2;
} watch;

const script::ScriptInfo* findCtl(App& a, const std::string& id) {
  for (const auto& s : controllerScripts(a))
    if (s.id == id) return &s;
  return nullptr;
}

DeviceScript* ensure(App& a, const std::string& device, const std::string& id) {
  auto it = devices.find(device);
  if (it != devices.end() && it->second.id == id) return it->second.script && it->second.script->active() ? &it->second : nullptr;
  if (it != devices.end()) devices.erase(it);
  const script::ScriptInfo* info = findCtl(a, id);
  DeviceScript& d = devices[device];
  d.id = id;
  d.host = std::make_unique<AppHost>(a, device);
  d.script = std::make_unique<script::ControllerScript>();
  d.script->limits = appLimits(a);
  d.script->limits.seconds = std::min(d.script->limits.seconds, 0.25);  // hooks run on the UI thread
  std::string err = info ? "" : "script not found: " + id;
  if (info) d.script->loadFile(info->path, *d.host, &err);
  if (!d.script->active()) {
    if (err.empty()) err = d.script->error();
    a.notify("Controller script " + id + ": " + err, true);
    logging::error("midi", device + ": " + err);
    d.reported = true;
    return nullptr;
  }
  if (auto* vm = d.script->vm()) {
    std::string src = device;
    vm->logSink = [src](const std::string& l) { logging::info("script", src + ": " + l); };
  }
  logging::info("midi", device + ": controller script " + d.script->name() + " loaded");
  return &d;
}

void reportFailure(App& a, const std::string& device, DeviceScript& d) {
  if (d.reported) return;
  d.reported = true;
  a.notify("Controller script stopped: " + d.script->error(), true);
  logging::error("midi", device + ": " + d.script->error());
}

std::string scriptIdOf(const std::string& controllerType) {
  return controllerType.rfind("script:", 0) == 0 ? controllerType.substr(7) : std::string();
}

}  // namespace

// ============================================================== project ====
// SC-02: project scripts run against the whole project; a run is one undo
// step. Selection and transport come from the app.

class AppProjectHost final : public script::ProjectHost {
 public:
  explicit AppProjectHost(App& a) : ProjectHost(a.doc), a_(a) { allowTransport = a.settings.getBool("scripts.allowTransport"); }
  bool playing() const override { return a_.engine.playing(); }
  bool recording() const override { return a_.recordArmed; }
  bool songMode() const override { return a_.engine.mode() == PlayMode::Song; }
  Tick position() const override { return static_cast<Tick>(a_.engine.position()); }
  void play(bool on) override {
    if (on != a_.engine.playing()) a_.runAction(RemoteAction::PlayPause);
  }
  void setPosition(Tick t) override { a_.engine.setPosition(t); }
  std::vector<ObjectId> selectedChannels() const override {
    std::vector<ObjectId> v(a_.selChannels.begin(), a_.selChannels.end());
    if (v.empty() && a_.selChannel != kNoId) v.push_back(a_.selChannel);
    return v;
  }
  void selectChannels(const std::vector<ObjectId>& ids) override {
    a_.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
    if (!ids.empty()) a_.selChannel = ids.front();
  }
  std::vector<ObjectId> selectedClips() const override { return {a_.playlist.selected.begin(), a_.playlist.selected.end()}; }
  void selectClips(const std::vector<ObjectId>& ids) override { a_.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end()); }
  ObjectId currentPattern() const override { return a_.curPattern; }
  void setCurrentPattern(ObjectId id) override { a_.selectPattern(id); }
  ObjectId selectedMixer() const override { return a_.selMixer; }
  void selectMixer(ObjectId id) override { a_.selMixer = id; }
  std::pair<Tick, Tick> timeSelection() const override { return {a_.playlist.selStart, a_.playlist.selEnd}; }
  void setTimeSelection(Tick start, Tick end) override {
    a_.playlist.selStart = start;
    a_.playlist.selEnd = end;
  }

 private:
  App& a_;
};

struct ProjectWindow {
  bool open = false, focus = false;
  script::ProjectScript script;
  std::string id;
  std::vector<script::FormParam> values;
  uint32_t seed = 1;
  std::string status;
  bool statusError = false;
  std::vector<std::string> log;
  double runMs = 0;
};
ProjectWindow PW;
std::vector<script::ScriptInfo> projList;
bool projScanned = false;

void runProjectScript(App& a) {
  AppProjectHost host(a);
  const std::string root = PW.script.path().empty() ? std::string() : fs::path(PW.script.path()).parent_path().string();
  const double t0 = wallNow();
  script::ProjectRunResult r = PW.script.run(host, PW.values, appLimits(a), root, PW.seed);
  PW.runMs = (wallNow() - t0) * 1000.0;
  PW.log = r.log;
  for (const auto& l : r.log) logging::info("script", PW.script.name() + ": " + l);
  if (!r.ok) {
    PW.status = r.error + " (nothing was changed)";
    PW.statusError = true;
    logging::error("script", PW.script.name() + ": " + r.error);
    return;
  }
  PW.statusError = false;
  PW.status = r.changed ? "Done: one undo step (" + a.doc.history().undoLabel() + ")" : "Done: nothing changed";
}

// ================================================================= public ====

std::unique_ptr<script::ProjectHost> makeProjectHost(App& a) { return std::make_unique<AppProjectHost>(a); }
script::Limits projectScriptLimits(const App& a) { return appLimits(a); }

const std::vector<script::ScriptInfo>& pianoRollScripts(App& a, bool rescan) {
  if (!prScanned || rescan) {
    std::vector<std::pair<std::string, bool>> dirs;
    std::string ship = shippedScriptsDir(a, "pianoroll");
    if (!ship.empty()) dirs.push_back({ship, false});
    dirs.push_back({userScriptsDir(a, "pianoroll"), true});
    prList = script::scanScripts(dirs);
    prScanned = true;
  }
  return prList;
}

void scriptsMenuItems(App& a) {
  const bool ok = menus::pianoHasTarget(a) && a.settings.getBool("scripts.enabled");
  const auto& list = pianoRollScripts(a);
  std::string cat = "\x01";
  for (const auto& s : list) {
    if (s.category != cat) {
      cat = s.category;
      ImGui::SeparatorText(cat.c_str());
    }
    std::string l = s.name + (s.user ? "  (yours)" : "") + "##" + s.path;
    if (ImGui::MenuItem(l.c_str(), nullptr, false, ok)) openPianoRollScript(a, s.path);
    if (!s.description.empty()) ui::Hint(s.description);
  }
  if (list.empty()) ImGui::TextDisabled("%s", i18n::tr("No scripts found"));
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rescan scripts"))) pianoRollScripts(a, true);
  if (ImGui::MenuItem(i18n::tr("Open your scripts folder"))) {
    std::error_code ec;
    fs::create_directories(userScriptsDir(a, "pianoroll"), ec);
    a.notify("Your piano roll scripts (*.lua): " + userScriptsDir(a, "pianoroll"));
  }
  if (!a.settings.getBool("scripts.enabled")) ImGui::TextDisabled("%s", i18n::tr("Scripts are switched off (Settings \xE2\x86\x92 Files)"));
}

const std::string& lastPianoRollScript() { return lastScriptPath; }

void openPianoRollScript(App& a, const std::string& path) {
  if (S.open) closeSession(a, true);
  const Song& s = a.doc.song();
  if (!s.pattern(a.curPattern) || !s.channel(a.selChannel)) {
    a.notify("Select a pattern and a channel first");
    return;
  }
  Session n;
  std::string err;
  if (!n.script.loadFile(path, &err)) {
    a.notify("Script error: " + err, true);
    logging::error("script", err);
    return;
  }
  n.id = fs::path(path).stem().string();
  lastScriptPath = path;  // J-27: "run the last script again"
  n.open = n.focus = true;
  n.pat = a.curPattern;
  n.ch = a.selChannel;
  n.key = 0x4A32345300000000ull ^ (sessionCounter++ << 4);
  S = std::move(n);
  if (const auto* notes = targetNotes(s)) S.base = *notes;
  S.baseSel.assign(S.base.size(), false);
  if (a.piano.selPattern == S.pat && a.piano.selChannel == S.ch)
    for (size_t i : a.piano.selected)
      if (i < S.baseSel.size()) S.baseSel[i] = true;
  S.baseMarkers = s.markers;
  S.values = S.script.form().params;
  // Restore the values used last time (when the form still has those controls).
  if (auto it = remembered.find(S.id); it != remembered.end())
    for (auto& v : S.values)
      for (const auto& r : it->second)
        if (r.name == v.name && r.kind == v.kind) v.value = std::clamp(r.value, v.min, v.max), v.text = r.text;
  if (auto it = rememberedSeed.find(S.id); it != rememberedSeed.end()) S.seed = it->second;
  a.showPiano = true;
}

bool scriptToolOpen() { return S.open; }

void drawScriptTool(App& a) {
  if (!S.open) return;
  const Song& s = a.doc.song();
  if (!s.pattern(S.pat) || !s.channel(S.ch)) {
    S.open = false;
    return;
  }
  // Runs the script when the scheduler says so (J-40: coalesced previews while
  // a control is held, one full run on release).
  auto runNow = [&a](script::PreviewScheduler::Run kind) {
    if (kind == script::PreviewScheduler::Run::None) return;
    runPreview(a, kind == script::PreviewScheduler::Run::Full);
    S.sched.ran(kind, wallNow(), S.lastLimitHit);
  };
  runNow(S.sched.poll(wallNow()));
  const auto& P = pal();
  std::string title = "Script \xE2\x80\x94 " + S.script.name() + " \xE2\x80\x94 " + s.channel(S.ch)->name + "###j24script";
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 40), ImGuiCond_Appearing,
                          ImVec2(0.5f, 1.0f));
  if (S.focus) ImGui::SetNextWindowFocus(), S.focus = false;
  bool open = true;
  if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) closeSession(a, false);
    return;
  }
  size_t k = 0;
  for (bool b : S.baseSel) k += b;
  ImGui::TextColored(theme::vec(P.textDim), "%s  \xC2\xB7  %s", s.channel(S.ch)->name.c_str(),
                     k ? (std::to_string(k) + " of " + std::to_string(S.base.size()) + " notes selected").c_str()
                       : ("all " + std::to_string(S.base.size()) + " notes").c_str());
  const std::string& desc = S.script.form().description;
  if (!desc.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", desc.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::BeginGroup();
  const bool formChanged = drawForm(S.values);
  ImGui::EndGroup();
  const bool held = ImGui::IsItemActive();  // a knob drag / text edit in the form
  if (formChanged) S.sched.changed(wallNow(), held);
  if (S.sched.dragging() && !held) S.sched.released();
  if (S.script.form().params.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("This script has no settings."));
  ImGui::Dummy(ImVec2(0, 4));
  if (!S.error.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(theme::vec(P.danger), "%s", S.error.c_str());
    ImGui::PopTextWrapPos();
  } else if (S.sched.tooSlow()) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Preview too slow \xE2\x80\x94 showing the last result"));
    ui::Hint("The live preview has a small time budget; the script runs in full when you release the control or accept");
  } else if (!S.last.log.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    size_t from = S.last.log.size() > 3 ? S.last.log.size() - 3 : 0;
    for (size_t i = from; i < S.last.log.size(); ++i) ImGui::TextColored(theme::vec(P.textFaint), "%s", S.last.log[i].c_str());
    ImGui::PopTextWrapPos();
  }
  if (ui::TextButton("New variation", false, 0, "Run again with new random numbers")) {
    S.seed = S.seed * 1664525u + 1013904223u + 7u;
    S.sched.changed(wallNow(), false);
  }
  ImGui::SameLine();
  bool accept = ui::TextButton("Accept", true, 90, "Keep the result (Enter)");
  ImGui::SameLine();
  if (ui::TextButton("Reset", false, 90, "Back to the script's default settings")) {
    S.values = S.script.form().params;
    S.sched.changed(wallNow(), false);
  }
  ImGui::SameLine();
  bool cancel = ui::TextButton("Cancel", false, 90, "Close and restore the notes (Esc)");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%.1f ms"), S.runMs);
  ui::Hint("Time the last run took");
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
  }
  ui::KeepWindowInViewport();
  ImGui::End();
  // Accept keeps the result of a run with the full limits (J-40).
  if (accept && S.sched.needsFull()) runNow(script::PreviewScheduler::Run::Full);
  if (accept && S.error.empty()) {
    if (S.changed || S.last.markersChanged) a.notify(S.script.name() + " applied");
    closeSession(a, true);
  } else if (accept) {
    a.notify("The script has an error; nothing was applied", true);
  } else if (cancel || !open) {
    closeSession(a, false);
  }
}

const std::vector<script::ScriptInfo>& controllerScripts(App& a, bool rescan) {
  if (!ctlScanned || rescan) {
    std::vector<std::pair<std::string, bool>> dirs;
    std::string ship = shippedScriptsDir(a, "controllers");
    if (!ship.empty()) dirs.push_back({ship, false});
    dirs.push_back({userScriptsDir(a, "controllers"), true});
    ctlList = script::scanScripts(dirs);
    ctlScanned = true;
  }
  return ctlList;
}

bool controllerScriptMidi(App& a, const std::string& device, const MidiMessage& m) {
  if (!a.midi || !a.settings.getBool("scripts.enabled")) return false;
  const MidiInputConfig* cfg = a.midi->inputConfig(device);
  std::string id = cfg ? scriptIdOf(cfg->controllerType) : std::string();
  if (id.empty()) return false;
  DeviceScript* d = ensure(a, device, id);
  if (!d) return false;
  bool handled = false;
  if (!d->script->onMidiMsg(m, &handled)) {
    reportFailure(a, device, *d);
    return false;
  }
  return handled;
}

void controllerScriptsTick(App& a) {
  if (!a.midi) return;
  const bool enabled = a.settings.getBool("scripts.enabled");
  // Load scripts of configured devices, drop the others.
  std::map<std::string, std::string> want;
  if (enabled)
    for (const auto& name : a.midi->inputs)
      if (const MidiInputConfig* c = a.midi->inputConfig(name); c && c->enabled)
        if (std::string id = scriptIdOf(c->controllerType); !id.empty()) want[name] = id;
  for (auto it = devices.begin(); it != devices.end();) {
    auto w = want.find(it->first);
    if (w == want.end() || w->second != it->second.id)
      it = devices.erase(it);
    else
      ++it;
  }
  for (const auto& [dev, id] : want)
    if (!devices.count(dev)) ensure(a, dev, id);
  if (devices.empty()) return;
  // What changed since the last frame.
  script::RefreshFlags f;
  const Song& s = a.doc.song();
  if (a.doc.revision() != watch.revision) f.mixer = f.channels = true, watch.revision = a.doc.revision();
  const bool playing = a.engine.playing(), song = a.engine.mode() == PlayMode::Song;
  if (playing != watch.playing || a.recordArmed != watch.rec || song != watch.song) f.transport = true;
  watch.playing = playing, watch.rec = a.recordArmed, watch.song = song;
  if (a.selMixer != watch.selMixer || a.selChannel != watch.selChannel) f.selection = true;
  watch.selMixer = a.selMixer, watch.selChannel = a.selChannel;
  if (a.focusPluginOwner != watch.plugOwner || a.focusPluginSlot != watch.plugSlot) f.plugin = true;
  watch.plugOwner = a.focusPluginOwner, watch.plugSlot = a.focusPluginSlot;
  (void)s;
  const double t = wallNow();
  for (auto& [dev, d] : devices) {
    if (!d.script || !d.script->active()) {
      if (d.script) reportFailure(a, dev, d);
      continue;
    }
    bool ok = true;
    if (f.any()) ok = d.script->onRefresh(f);
    if (ok && t - d.lastIdle >= 0.02) {
      d.lastIdle = t;
      ok = d.script->onIdle();
    }
    if (!ok) reportFailure(a, dev, d);
    if (d.host && d.host->hintUntil > t && a.hint.empty()) a.setHint(d.host->hintText);
  }
}

void shutdownControllerScripts() {
  devices.clear();
  ctlScanned = false;
}

std::string controllerScriptStatus(const std::string& device) {
  auto it = devices.find(device);
  if (it == devices.end() || !it->second.script) return "";
  return it->second.script->active() ? "running" : it->second.script->error();
}

void drawScriptConsole(App& a, float height) {
  static std::unique_ptr<script::Vm> vm;
  static std::unique_ptr<AppHost> host;
  static std::unique_ptr<AppProjectHost> projHost;
  static std::vector<std::pair<bool, std::string>> lines;  // (is input, text)
  static char input[1024] = "";
  if (!vm) {
    host = std::make_unique<AppHost>(a, "Console");
    script::Limits l = appLimits(a);
    std::string root = userScriptsDir(a, "console");
    std::error_code ec;
    fs::create_directories(root, ec);
    vm = std::make_unique<script::Vm>(l, root, true);
    script::openControllerApi(*vm, *host);
    projHost = std::make_unique<AppProjectHost>(a);
    script::openProjectApi(*vm, *projHost);
    lines.push_back({false, "Lua 5.4 console \xC2\xB7 transport, mixer, channels, plugin, device, ui, files, project"});
  }
  const auto& P = pal();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##console", ImVec2(0, height), ImGuiChildFlags_Borders);
  ImGui::PushFont(a.fontMono, theme::kFontNumber);
  for (const auto& [in, t] : lines)
    ImGui::TextColored(theme::vec(in ? P.accent : t.rfind("error: ", 0) == 0 ? P.danger : P.text), "%s%s", in ? "> " : "", t.c_str());
  ImGui::PopFont();
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SetNextItemWidth(-90);
  bool run = ImGui::InputTextWithHint("##cin", i18n::tr("mixer.count()   transport.tempo()   channels.name(1)"), input, sizeof input,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
  ui::FocusRing();
  ImGui::SameLine();
  run |= ui::TextButton("Run", true, 80);
  if (run && input[0]) {
    std::string code = input, err;
    lines.push_back({true, code});
    // One console line = one undo step for its project edits (SC-02).
    projHost->allowTransport = a.settings.getBool("scripts.allowTransport");
    a.doc.endGesture();
    a.doc.beginGroup("console: " + code.substr(0, 40));
    std::vector<std::string> out = script::runConsole(*vm, code, &err);
    if (err.empty())
      a.doc.endGroup();
    else
      a.doc.cancelGroup();
    for (auto& l : out) lines.push_back({false, l});
    if (!err.empty()) lines.push_back({false, "error: " + err});
    while (lines.size() > 400) lines.erase(lines.begin());
    input[0] = 0;
    ImGui::SetKeyboardFocusHere(-1);
  }
}

const std::vector<script::ScriptInfo>& projectScripts(App& a, bool rescan) {
  if (!projScanned || rescan) {
    std::vector<std::pair<std::string, bool>> dirs;
    std::string ship = shippedScriptsDir(a, "project");
    if (!ship.empty()) dirs.push_back({ship, false});
    dirs.push_back({userScriptsDir(a, "project"), true});
    projList = script::scanScripts(dirs);
    projScanned = true;
  }
  return projList;
}

void projectScriptsMenuItems(App& a) {
  const bool ok = a.settings.getBool("scripts.enabled");
  const auto& list = projectScripts(a);
  std::string cat = "\x01";
  for (const auto& s : list) {
    if (s.category != cat) {
      cat = s.category;
      ImGui::SeparatorText(cat.c_str());
    }
    std::string l = s.name + (s.user ? "  (yours)" : "") + "...##" + s.path;
    if (ImGui::MenuItem(l.c_str(), nullptr, false, ok)) openProjectScript(a, s.path);
    if (!s.description.empty()) ui::Hint(s.description);
  }
  if (list.empty()) ImGui::TextDisabled("No scripts found");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rescan scripts"))) projectScripts(a, true);
  if (ImGui::MenuItem(i18n::tr("Open your scripts folder"))) {
    std::error_code ec;
    fs::create_directories(userScriptsDir(a, "project"), ec);
    a.notify("Your project scripts (*.lua): " + userScriptsDir(a, "project"));
  }
  if (!ok) ImGui::TextDisabled("Scripts are switched off (Settings \xE2\x86\x92 Files)");
}

void openProjectScript(App& a, const std::string& path) {
  ProjectWindow n;
  std::string err;
  if (!n.script.loadFile(path, &err)) {
    a.notify("Script error: " + err, true);
    logging::error("script", err);
    return;
  }
  n.id = fs::path(path).stem().string();
  n.values = n.script.form().params;
  if (auto it = remembered.find("project:" + n.id); it != remembered.end())
    for (auto& v : n.values)
      for (const auto& r : it->second)
        if (r.name == v.name && r.kind == v.kind) v.value = std::clamp(r.value, v.min, v.max), v.text = r.text;
  n.open = n.focus = true;
  // A script without settings runs straight away.
  const bool direct = n.values.empty();
  PW = std::move(n);
  if (direct) {
    runProjectScript(a);
    if (!PW.statusError) {
      a.notify(PW.script.name() + ": " + PW.status);
      PW.open = false;
    }
  }
}

void drawProjectScriptWindow(App& a) {
  if (!PW.open) return;
  const auto& P = pal();
  std::string title = "Project script \xE2\x80\x94 " + PW.script.name() + "###j57script";
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  if (PW.focus) ImGui::SetNextWindowFocus(), PW.focus = false;
  bool open = true;
  if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) PW.open = false;
    return;
  }
  const std::string& desc = PW.script.form().description;
  if (!desc.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", desc.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::BeginGroup();
  drawForm(PW.values);
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(0, 4));
  if (!PW.status.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(theme::vec(PW.statusError ? P.danger : P.textDim), "%s", PW.status.c_str());
    size_t from = PW.log.size() > 3 ? PW.log.size() - 3 : 0;
    for (size_t i = from; i < PW.log.size(); ++i) ImGui::TextColored(theme::vec(P.textFaint), "%s", PW.log[i].c_str());
    ImGui::PopTextWrapPos();
  }
  bool run = ui::TextButton("Run", true, 90, "Apply the script to the project (one undo step, Enter)");
  ImGui::SameLine();
  if (ui::TextButton("Undo", false, 90, "Undo the last step")) a.doc.undo();
  ImGui::SameLine();
  if (ui::TextButton("Reset", false, 90, "Back to the script's default settings")) PW.values = PW.script.form().params;
  ImGui::SameLine();
  bool close = ui::TextButton("Close", false, 90, "Close (Esc)");
  if (PW.runMs > 0) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textFaint), "%.1f ms", PW.runMs);
  }
  if (a.settings.getBool("scripts.allowTransport")) ImGui::TextColored(theme::vec(P.textFaint), "Scripts may control the transport");
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) run = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
  }
  ui::KeepWindowInViewport();
  ImGui::End();
  if (run) {
    PW.seed = PW.seed * 1664525u + 1013904223u;
    runProjectScript(a);
  }
  if (close || !open) {
    remembered["project:" + PW.id] = PW.values;
    PW.open = false;
  }
}

bool drawScriptForm(std::vector<script::FormParam>& values) { return drawForm(values); }

void devOpenScript(App& a, const std::string& id) {
  if (id.rfind("project:", 0) == 0) {  // --script project:<id>: J-57 project script window
    for (const auto& s : projectScripts(a))
      if (s.id == id.substr(8)) openProjectScript(a, s.path);
    return;
  }
  for (const auto& s : pianoRollScripts(a))
    if (s.id == id) {
      openPianoRollScript(a, s.path);
      return;
    }
}

}  // namespace gl
