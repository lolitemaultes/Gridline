// Gridline — one built-in plugin on its own (J-131, Gridline Suite): the
// minimal project around it, its host parameters and its state.
//
// The project: an instrument plays on one channel routed to the master; an
// effect sits in the first slot of insert 1, which takes the host's main input
// (engine input channels 1-2, monitoring on), and insert 2 takes the sidechain
// (channels 3-4) and feeds insert 1's sidechain input. MIDI-input effects get a
// MIDI Out channel on port 0 that plays the host's notes onto the port bus.
//
// Host parameters: parameter k of the plugin is host parameter k (id k); the
// engine applies host values through its host parameter bank (AudioEngine::
// setHostParam). Effects add Mix and Bypass (host macros 1 and 2 linked to the
// slot's mix and on / off). The project and the host agree on every value:
// host values are written into the project (no undo step) by syncParams(),
// and edits made in the editor are reported to the host.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../app/App.h"
#include "../core/Json.h"
#include "../core/PluginParams.h"
#include "../core/Project.h"
#include "PluginCore.h"
#include "PluginView.h"

namespace gl::plugin {

namespace {

constexpr const char* kSuiteStateFormat = "gridline-suite";
constexpr int kSuiteStateVersion = 1;
constexpr float kMinZoom = 0.5f, kMaxZoom = 3.0f;

// Every instance numbers its objects from its own base so that editor
// state kept per object id (preset headers, A/B compare) never mixes two
// instances in one host process.
ObjectId nextIdBase() {
  static std::atomic<uint32_t> serial{0};
  const uint32_t s = serial.fetch_add(1) % 32768u;
  return kFirstObjectId + static_cast<ObjectId>(s) * 65536u;
}

double editorWidthOf(const PluginInfo& info) {
  return info.editorWidth > 0 ? info.editorWidth : (info.type == "gl.eq" ? 780.0 : 600.0);
}

// A number typed by the user: the first number in the text, with k / % hints.
bool parseNumber(const std::string& text, double& v, bool& kilo, bool& percent) {
  const char* s = text.c_str();
  while (*s == ' ' || *s == '\t') ++s;
  char* end = nullptr;
  v = std::strtod(s, &end);
  if (end == s || !std::isfinite(v)) return false;
  const std::string rest(end);
  kilo = rest.find('k') != std::string::npos || rest.find('K') != std::string::npos;
  percent = rest.find('%') != std::string::npos;
  return true;
}

}  // namespace

// ---------------------------------------------------------------- project

void Instance::setupSingle() {
  App& a = *app_;
  a.singlePlugin = true;
  const PluginInfo& info = *info_;
  nParams_ = static_cast<int>(std::min(info.params.size(), static_cast<size_t>(kMaxHostParams)));
  Song s;
  s.nextId = nextIdBase();
  ensureMixer(s, 2);
  Pattern p;
  p.id = s.allocateId();
  p.name = "Pattern 1";
  s.patterns.push_back(p);
  ObjectId midiChannel = kNoId;
  if (variant_ == Variant::Instrument) {
    Channel c;
    c.id = s.allocateId();
    c.name = info.name;
    c.plugin = defaultPluginState(info.type);
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    owner_ = c.id;
    midiChannel = c.id;
  } else {
    MixerTrack& t = s.mixer[1];
    t.name = "Input";
    t.input = 0;
    t.inputStereo = true;
    t.monitor = static_cast<uint8_t>(InputMonitor::On);
    t.slots[0].plugin = defaultPluginState(info.type);
    owner_ = t.id;
    MixerTrack& sc = s.mixer[2];
    sc.name = "Sidechain";
    sc.input = 2;
    sc.inputStereo = true;
    sc.monitor = static_cast<uint8_t>(InputMonitor::On);
    Send send;
    send.to = t.id;
    send.level = 1.0f;
    send.sidechain = true;
    sc.sends = {send};
    if (info.midiInput) {  // the host's notes reach the effect through MIDI port 0
      Channel c;
      c.id = s.allocateId();
      c.name = "MIDI";
      c.plugin = defaultPluginState("gl.midiout");
      if (c.plugin.params.size() > static_cast<size_t>(pp::MoPort)) c.plugin.params[pp::MoPort] = 0;
      c.mixerTrack = s.masterId();
      s.channels.push_back(c);
      midiChannel = c.id;
    }
  }
  a.startProject(std::move(s));
  a.selChannel = midiChannel;
  a.selChannels = {midiChannel};
  a.selMixer = variant_ == Variant::Effect ? owner_ : a.doc.song().masterId();
  if (variant_ == Variant::Effect)
    a.fxEditor = {owner_, 0};
  else
    a.settingsChannel = owner_;
  a.builder.setHostParamBank(variant_ == Variant::Instrument ? channelPluginParam(owner_, 0) : slotParam(owner_, 0, 0));
  if (variant_ == Variant::Effect) a.builder.setHostMacros({slotMixParam(owner_, 0), slotEnableParam(owner_, 0)});
  const int total = paramCount();
  hostNorm_.reset(new std::atomic<float>[static_cast<size_t>(total)]);
  hostDirty_.reset(new std::atomic<uint64_t>[static_cast<size_t>((total + 63) / 64)]);
  for (int w = 0; w < (total + 63) / 64; ++w) hostDirty_[static_cast<size_t>(w)].store(0);
  synced_.assign(static_cast<size_t>(total), 0.0);
  gesture_.assign(static_cast<size_t>(total), 0);
  zoom_ = static_cast<float>(std::clamp(a.settings.getNumber("ui.scale"), 0.75, 2.0));
  resetHostValues();
  a.forceRebuild();
}

const PluginState* Instance::singleState() const {
  const Song& s = app_->doc.song();
  if (variant_ == Variant::Instrument) {
    const Channel* c = s.channel(owner_);
    return c ? &c->plugin : nullptr;
  }
  const MixerTrack* t = s.mixerTrack(owner_);
  return t ? &t->slots[0].plugin : nullptr;
}

// The project's value of host parameter k, normalized.
static double projectValue(const App& a, const PluginInfo& info, Variant v, ObjectId owner, int nParams, int k) {
  const Song& s = a.doc.song();
  const PluginState* st = nullptr;
  const EffectSlot* slot = nullptr;
  if (v == Variant::Instrument) {
    if (const Channel* c = s.channel(owner)) st = &c->plugin;
  } else if (const MixerTrack* t = s.mixerTrack(owner)) {
    slot = &t->slots[0];
    st = &slot->plugin;
  }
  if (k < nParams) {
    if (!st || static_cast<size_t>(k) >= st->params.size()) return info.params[static_cast<size_t>(k)].toNormalized(info.params[static_cast<size_t>(k)].def);
    const ParamDesc& d = info.params[static_cast<size_t>(k)];
    return std::clamp(d.toNormalized(d.clamp(st->params[static_cast<size_t>(k)])), 0.0, 1.0);
  }
  if (!slot) return k == nParams ? 1.0 : 0.0;
  return k == nParams ? std::clamp(static_cast<double>(slot->mix), 0.0, 1.0) : (slot->enabled ? 0.0 : 1.0);
}

void Instance::resetHostValues() {
  App& a = *app_;
  const int total = paramCount();
  for (int k = 0; k < total; ++k) {
    const double v = projectValue(a, *info_, variant_, owner_, nParams_, k);
    hostNorm_[static_cast<size_t>(k)].store(static_cast<float>(v), std::memory_order_relaxed);
    synced_[static_cast<size_t>(k)] = v;
    if (k < nParams_) a.engine.setHostParam(k, -1.f);  // the project's values play
  }
  if (variant_ == Variant::Effect)
    for (int m = 0; m < 2; ++m) a.engine.setHostMacro(m, -1.f);
  syncedRevision_ = a.doc.revision();
}

// ---------------------------------------------------------------- host parameters

int Instance::paramCount() const {
  if (!info_) return kMacros;
  return nParams_ + (variant_ == Variant::Effect ? 2 : 0);
}

uint32_t Instance::paramId(int index) const {
  if (!info_ || index < nParams_) return static_cast<uint32_t>(index);
  return index == nParams_ ? kMixParamId : kBypassParamId;
}

int Instance::paramIndex(uint32_t id) const {
  if (!info_) return id < static_cast<uint32_t>(kMacros) ? static_cast<int>(id) : -1;
  if (id < static_cast<uint32_t>(nParams_)) return static_cast<int>(id);
  if (variant_ == Variant::Effect && id == kMixParamId) return nParams_;
  if (variant_ == Variant::Effect && id == kBypassParamId) return nParams_ + 1;
  return -1;
}

HostParam Instance::paramInfo(int index) const {
  HostParam h;
  if (!info_) {
    h.name = macroName(index);
    return h;
  }
  if (index < nParams_) {
    const ParamDesc& d = info_->params[static_cast<size_t>(index)];
    h.name = d.name;
    h.unit = d.unit;
    h.min = d.min;
    h.max = d.max;
    h.def = std::clamp(d.toNormalized(d.clamp(d.def)), 0.0, 1.0);
    h.logScale = d.scale == ParamScale::Log;
    if (d.kind == ParamKind::Toggle) {
      h.toggle = true;
      h.steps = 1;
      h.min = 0;
      h.max = 1;
    } else if (d.kind == ParamKind::Choice) {
      h.choice = true;
      h.choices = d.choices;
      h.min = 0;
      h.max = d.choices.empty() ? std::max(0.0, d.max) : static_cast<double>(d.choices.size() - 1);
      h.steps = static_cast<int>(h.max);
    }
    return h;
  }
  if (index == nParams_) {
    h.name = "Mix";
    h.unit = "%";
    h.def = 1.0;
    return h;
  }
  h.name = "Bypass";
  h.bypass = h.toggle = true;
  h.steps = 1;
  return h;
}

int Instance::paramSteps(int index) const {
  if (!info_ || index < 0 || index > nParams_ + 1) return 0;
  if (index == nParams_) return 0;
  if (index == nParams_ + 1) return 1;
  const ParamDesc& d = info_->params[static_cast<size_t>(index)];
  if (d.kind == ParamKind::Toggle) return 1;
  if (d.kind == ParamKind::Choice) return d.choices.empty() ? std::max(0, static_cast<int>(d.max)) : static_cast<int>(d.choices.size()) - 1;
  return 0;
}

double Instance::paramValue(int index) const {
  if (!info_) return macroValue(index);
  if (index < 0 || index >= paramCount()) return 0;
  return hostNorm_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void Instance::setParamValue(int index, double normalized) {
  if (!info_) {
    setMacroValue(index, normalized);
    return;
  }
  if (index < 0 || index >= paramCount()) return;
  const float v = static_cast<float>(std::isfinite(normalized) ? std::clamp(normalized, 0.0, 1.0) : 0.0);
  hostNorm_[static_cast<size_t>(index)].store(v, std::memory_order_relaxed);
  hostDirty_[static_cast<size_t>(index >> 6)].fetch_or(uint64_t{1} << (index & 63), std::memory_order_release);
  AudioEngine& eng = app_->engine;
  if (index < nParams_)
    eng.setHostParam(index, v);
  else if (index == nParams_)
    eng.setHostMacro(0, v);  // slot mix
  else
    eng.setHostMacro(1, v >= 0.5f ? 0.f : 1.f);  // bypass on = slot off
}

double Instance::paramToPlain(int index, double normalized) const {
  normalized = std::clamp(normalized, 0.0, 1.0);
  if (!info_ || index >= nParams_ || index < 0) return index == nParams_ + 1 ? (normalized >= 0.5 ? 1.0 : 0.0) : normalized;
  const ParamDesc& d = info_->params[static_cast<size_t>(index)];
  return d.clamp(d.fromNormalized(normalized));
}

double Instance::paramFromPlain(int index, double plain) const {
  if (!info_ || index >= nParams_ || index < 0) return std::clamp(plain, 0.0, 1.0);
  const ParamDesc& d = info_->params[static_cast<size_t>(index)];
  return std::clamp(d.toNormalized(d.clamp(plain)), 0.0, 1.0);
}

std::string Instance::paramText(int index, double normalized) const {
  if (!info_) return macroText(index, normalized);
  normalized = std::clamp(normalized, 0.0, 1.0);
  if (index >= 0 && index < nParams_) {
    const ParamDesc& d = info_->params[static_cast<size_t>(index)];
    return d.format(d.clamp(d.fromNormalized(normalized)));
  }
  if (index == nParams_ + 1) return normalized >= 0.5 ? "On" : "Off";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f%%", normalized * 100.0);
  return buf;
}

bool Instance::paramFromText(int index, const std::string& text, double& normalized) const {
  if (info_ && index >= 0 && index < nParams_) {
    const ParamDesc& d = info_->params[static_cast<size_t>(index)];
    for (size_t c = 0; c < d.choices.size(); ++c)  // a choice by its label
      if (d.choices[c] == text) {
        normalized = std::clamp(d.toNormalized(static_cast<double>(c)), 0.0, 1.0);
        return true;
      }
    if (d.kind == ParamKind::Toggle) {
      if (text == "On" || text == "on") return normalized = 1.0, true;
      if (text == "Off" || text == "off") return normalized = 0.0, true;
    }
    double x = 0;
    bool kilo = false, percent = false;
    if (!parseNumber(text, x, kilo, percent)) return false;
    // The value as the display shows it may be scaled (kHz, percent of 0..1):
    // take the reading whose own display shows the typed number best.
    const double cands[] = {kilo ? x * 1000.0 : x, x / 100.0, x / 1000.0, x * 1000.0};
    double best = d.clamp(cands[0]), bestErr = 1e300;
    for (double c : cands) {
      const double v = d.clamp(c);
      double shown = 0;
      bool k2 = false, p2 = false;
      if (!parseNumber(d.format(v), shown, k2, p2)) continue;
      if (k2 && !kilo) shown *= 1000.0;
      const double err = std::fabs(shown - (kilo ? x * 1000.0 : x)) / std::max(1e-9, std::fabs(x) + 1e-9);
      if (err < bestErr - 1e-12) {
        bestErr = err;
        best = v;
      }
    }
    normalized = std::clamp(d.toNormalized(best), 0.0, 1.0);
    return true;
  }
  if (info_ && index == nParams_ + 1) {
    if (text == "On" || text == "on" || text == "1") return normalized = 1.0, true;
    if (text == "Off" || text == "off" || text == "0") return normalized = 0.0, true;
    return false;
  }
  double x = 0;
  bool kilo = false, percent = false;
  if (!parseNumber(text, x, kilo, percent)) return false;
  normalized = std::clamp(percent || (info_ && x > 1.0) ? x / 100.0 : x, 0.0, 1.0);  // Mix: "50" = 50 %
  return true;
}

void Instance::syncParams() {
  if (!info_) return;
  App& a = *app_;
  const int total = paramCount();
  // Host -> project: values the host set since the last call.
  std::vector<std::pair<int, double>> fromHost;
  for (int w = 0; w * 64 < total; ++w) {
    uint64_t bits = hostDirty_[static_cast<size_t>(w)].exchange(0, std::memory_order_acquire);
    for (int b = 0; bits; ++b, bits >>= 1)
      if (bits & 1) {
        const int k = w * 64 + b;
        if (k >= total) break;
        if (gesture_[static_cast<size_t>(k)]) continue;  // the editor is moving it: the editor wins
        fromHost.push_back({k, hostNorm_[static_cast<size_t>(k)].load(std::memory_order_relaxed)});
      }
  }
  if (!fromHost.empty()) {
    bool differs = false;
    for (const auto& [k, v] : fromHost)
      if (std::fabs(projectValue(a, *info_, variant_, owner_, nParams_, k) - v) > 1e-6) differs = true;
    if (differs) {
      const bool inst = variant_ == Variant::Instrument;
      const ObjectId owner = owner_;
      const PluginInfo* info = info_;
      const int n = nParams_;
      a.doc.patchSong([&](Song& s) {
        PluginState* st = nullptr;
        EffectSlot* slot = nullptr;
        if (inst) {
          if (Channel* c = s.channel(owner)) st = &c->plugin;
        } else if (MixerTrack* t = s.mixerTrack(owner)) {
          slot = &t->slots[0];
          st = &slot->plugin;
        }
        if (!st) return;
        for (const auto& [k, v] : fromHost) {
          if (k < n) {
            if (static_cast<size_t>(k) >= st->params.size()) continue;
            const ParamDesc& d = info->params[static_cast<size_t>(k)];
            st->params[static_cast<size_t>(k)] = static_cast<float>(d.clamp(d.fromNormalized(v)));
          } else if (slot && k == n) {
            slot->mix = static_cast<float>(v);
          } else if (slot) {
            slot->enabled = v < 0.5;
          }
        }
      });
    }
    for (const auto& [k, v] : fromHost) synced_[static_cast<size_t>(k)] = projectValue(a, *info_, variant_, owner_, nParams_, k);
    syncedRevision_ = a.doc.revision();
  }
  // Project -> host: edits made in the editor (or undo, presets, ...).
  const bool pointer = a.doc.inPointerGesture();
  if (a.doc.revision() != syncedRevision_) {
    syncedRevision_ = a.doc.revision();
    std::vector<int> changed;
    for (int k = 0; k < total; ++k) {
      const double v = projectValue(a, *info_, variant_, owner_, nParams_, k);
      if (std::fabs(v - synced_[static_cast<size_t>(k)]) <= 1e-7) continue;
      synced_[static_cast<size_t>(k)] = v;
      hostNorm_[static_cast<size_t>(k)].store(static_cast<float>(v), std::memory_order_relaxed);
      // The engine plays the project's value (host values set earlier stay in step).
      if (k < nParams_)
        a.engine.setHostParam(k, static_cast<float>(v));
      else if (k == nParams_)
        a.engine.setHostMacro(0, static_cast<float>(v));
      else
        a.engine.setHostMacro(1, v >= 0.5 ? 0.f : 1.f);
      changed.push_back(k);
    }
    if (!changed.empty()) {
      if (pointer || changed.size() <= 8) {
        for (int k : changed) {
          const bool open = gesture_[static_cast<size_t>(k)] != 0;
          if (!open && onParamGesture) onParamGesture(k, true);
          if (onMacroEdited) onMacroEdited(k, synced_[static_cast<size_t>(k)]);
          if (pointer)
            gesture_[static_cast<size_t>(k)] = 1;
          else if (!open && onParamGesture)
            onParamGesture(k, false);
        }
      } else if (onMacrosChanged) {
        onMacrosChanged();  // a preset, undo or reset: the host reads every value again
      }
    }
  }
  if (!pointer)  // the pointer let go: the edits' gestures end
    for (int k = 0; k < total; ++k)
      if (gesture_[static_cast<size_t>(k)]) {
        gesture_[static_cast<size_t>(k)] = 0;
        if (onParamGesture) onParamGesture(k, false);
      }
}

// ---------------------------------------------------------------- editor size

void Instance::preferredViewSize(int& w, int& h) const {
  if (!info_) {
    w = kDefaultViewW;
    h = kDefaultViewH;
    return;
  }
  int cw = editorW_, ch = editorH_;
  if (cw <= 0 || ch <= 0) {  // not drawn yet: the editor's width and a typical height
    cw = static_cast<int>(editorWidthOf(*info_)) + 16;
    ch = variant_ == Variant::Instrument ? 520 : 420;
  }
  w = static_cast<int>(std::lround(cw * zoom_));
  h = static_cast<int>(std::lround(ch * zoom_));
}

void Instance::minViewSize(int& w, int& h) const {
  if (!info_) {
    w = kMinViewW;
    h = kMinViewH;
    return;
  }
  int pw = 0, ph = 0;
  preferredViewSize(pw, ph);
  w = std::max(160, static_cast<int>(std::lround(pw / zoom_ * kMinZoom)));
  h = std::max(120, static_cast<int>(std::lround(ph / zoom_ * kMinZoom)));
}

bool Instance::setEditorContent(int w, int h) {
  if (w <= 0 || h <= 0 || (w == editorW_ && h == editorH_)) return false;
  editorW_ = w;
  editorH_ = h;
  return true;
}

void Instance::editorContent(int& w, int& h) const {
  w = editorW_;
  h = editorH_;
}

void Instance::setEditorZoom(float z) { zoom_ = std::clamp(std::isfinite(z) ? z : 1.0f, kMinZoom, kMaxZoom); }

// ---------------------------------------------------------------- state

std::string Instance::saveSingle() {
  const PluginState* st = singleState();
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString(kSuiteStateFormat);
  j.object["version"] = Json::makeNumber(kSuiteStateVersion);
  j.object["plugin"] = pluginStateToJson(st ? *st : defaultPluginState(info_->type));
  if (variant_ == Variant::Effect)
    if (const MixerTrack* t = app_->doc.song().mixerTrack(owner_)) {
      j.object["mix"] = Json::makeNumber(t->slots[0].mix);
      Json on;
      on.type = Json::Type::Bool;
      on.boolean = t->slots[0].enabled;
      j.object["enabled"] = on;
    }
  Json ed = Json::makeObject();
  ed.object["zoom"] = Json::makeNumber(zoom_);
  ed.object["tab"] = Json::makeNumber(editorTab_);
  j.object["editor"] = std::move(ed);
  return jsonStringify(j, 0);
}

bool Instance::loadFactoryPreset(int i) {
  if (!info_ || i < 0 || static_cast<size_t>(i) >= info_->presets.size()) return false;
  makeCurrent();
  App& a = *app_;
  PluginState st = presetState(*info_, static_cast<size_t>(i));
  if (const PluginState* cur = singleState()) st.data = cur->data;
  const bool inst = variant_ == Variant::Instrument;
  const ObjectId owner = owner_;
  a.doc.patchSong([&](Song& s) {
    if (inst) {
      if (Channel* c = s.channel(owner)) c->plugin = st;
    } else if (MixerTrack* t = s.mixerTrack(owner)) {
      t->slots[0].plugin = st;
    }
  });
  for (char& g : gesture_) g = 0;
  resetHostValues();
  a.forceRebuild();
  idle();
  if (onMacrosChanged) onMacrosChanged();
  return true;
}

bool Instance::loadSingle(const std::string& data, std::string* err) {
  App& a = *app_;
  Json j;
  if (!jsonParse(data, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "not a Gridline Suite state";
    return false;
  }
  // The suite's own state, or a plugin state / preset file as Gridline writes them.
  const Json* pj = j.text("format") == kSuiteStateFormat ? j.find("plugin") : &j;
  if (!pj || !pj->is(Json::Type::Object) || pj->text("type") != info_->type) {
    if (err) *err = "not a state of " + info_->name;
    return false;
  }
  PluginState st = pluginStateFromJson(*pj);
  const PluginState* cur = singleState();
  if (!pj->find("data") && cur && j.text("format") != kSuiteStateFormat) st.data = cur->data;  // a preset keeps the loaded sample
  double mix = 1.0;
  bool enabled = true;
  if (const Json* m = j.find("mix"); m && m->is(Json::Type::Number)) mix = std::clamp(m->number, 0.0, 1.0);
  if (const Json* e = j.find("enabled"); e && e->is(Json::Type::Bool)) enabled = e->boolean;
  const bool inst = variant_ == Variant::Instrument;
  const ObjectId owner = owner_;
  a.doc.patchSong([&](Song& s) {
    if (inst) {
      if (Channel* c = s.channel(owner)) c->plugin = st;
    } else if (MixerTrack* t = s.mixerTrack(owner)) {
      t->slots[0].plugin = st;
      t->slots[0].mix = static_cast<float>(mix);
      t->slots[0].enabled = enabled;
    }
  });
  if (const Json* ed = j.find("editor"); ed && ed->is(Json::Type::Object)) {
    if (const Json* z = ed->find("zoom"); z && z->is(Json::Type::Number)) setEditorZoom(static_cast<float>(z->number));
    editorTab_ = std::clamp(jsonInt<int>(ed->num("tab")), 0, 2);
  }
  for (char& g : gesture_) g = 0;
  for (int w = 0; w * 64 < paramCount(); ++w) hostDirty_[static_cast<size_t>(w)].store(0);
  resetHostValues();
  a.forceRebuild();
  idle();
  if (onMacrosChanged) onMacrosChanged();
  return true;
}

}  // namespace gl::plugin
