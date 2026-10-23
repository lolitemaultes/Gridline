// Gridline — Gridline itself as a plugin (PH-09): the format-independent instance.
#include "PluginCore.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../app/App.h"
#include "../app/HostedUi.h"
#include "../core/I18n.h"
#include "../core/Json.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../host/Dylib.h"
#include "../host/HostedPlugins.h"
#include "../core/Platform.h"

namespace fs = std::filesystem;

namespace gl::plugin {

namespace {

constexpr const char* kStateFormat = "gridline-plugin";
constexpr int kStateVersion = 1;

double nowSeconds() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// The default project of the effect: the host's audio enters mixer insert 1
// (input monitoring on), which sends to the master as usual.
Song effectSong(const App& a) {
  Song s = makeEmptySong();
  a.applyProjectDefaults(s);
  if (s.mixer.size() > 1) {
    MixerTrack& t = s.mixer[1];
    t.name = i18n::tr("Host input");
    t.input = 0;
    t.inputStereo = true;
    t.monitor = static_cast<uint8_t>(InputMonitor::On);
  }
  return s;
}

}  // namespace

std::string findPluginAssets() {
  std::vector<fs::path> cands;
  if (const char* env = std::getenv("GRIDLINE_ASSETS")) cands.emplace_back(env);
  if (const std::string self = host::dl::pathOf(reinterpret_cast<const void*>(&findPluginAssets)); !self.empty()) {
    const fs::path bin = fs::u8path(self).parent_path();  // <bundle>/Contents/<arch or MacOS>
    cands.push_back(bin / ".." / "Resources" / "assets");
    cands.push_back(bin / "assets");
    cands.push_back(bin / "Gridline-assets");
  }
#ifdef GRIDLINE_SOURCE_ASSETS
  cands.emplace_back(GRIDLINE_SOURCE_ASSETS);
#endif
  if (const char* home = gl::platform::homeEnv()) cands.push_back(fs::path(home) / ".local" / "share" / "gridline" / "assets");
  cands.emplace_back("/usr/local/share/gridline/assets");
  cands.emplace_back("/usr/share/gridline/assets");
  for (const auto& c : cands) {
    std::error_code ec;
    if (fs::exists(c / "fonts" / "Inter-Regular.ttf", ec)) return fs::weakly_canonical(c, ec).string();
  }
  return {};
}

namespace {
std::vector<Instance*>& instances() {  // main thread
  static std::vector<Instance*> v;
  return v;
}
}  // namespace

Instance* Instance::of(const App* a) {
  for (Instance* i : instances())
    if (i->app_.get() == a) return i;
  return nullptr;
}

Instance::Instance(Variant v) : variant_(v) {
  instances().push_back(this);
  for (auto& m : macroValues_) m.store(-1.f);
  App* prev = setCurrentApp(nullptr);
  app_ = std::make_unique<App>();
  App& a = *app_;
  a.embedded = true;
  a.uiSoundsDisabled = true;
  a.assetsDir = findPluginAssets();
  a.applySettingsHooks();  // language from the assets folder
  // Installed plugins the application scanned (its plugin cache and
  // per-plugin options) load inside the Gridline plugin too.
  static bool hosting = false;
  if (!hosting) {
    hosting = true;
    host::initHosting(a.configDir());
  }
  if (variant_ == Variant::Effect) {
    a.startProject(effectSong(a));
    a.selMixer = a.doc.song().mixer.size() > 1 ? a.doc.song().mixer[1].id : a.doc.song().masterId();
  }
  a.engine.setInputChannels(variant_ == Variant::Effect ? 4 : 0);  // main input + sidechain
  a.engine.setInputPrefill(0);
  applyMidiRouting();
  if (prev) setCurrentApp(prev);
}

Instance::~Instance() {
  makeCurrent();
  closeAllHostedEditors(*app_);
  app_.reset();
  auto& v = instances();
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

void Instance::makeCurrent() { setCurrentApp(app_.get()); }

bool Instance::activate(double sampleRate, int maxFrames) {
  makeCurrent();
  App& a = *app_;
  sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
  maxFrames_ = std::max(1, maxFrames);
  a.engine.setSampleRate(sampleRate_);
  a.audioSampleRate = static_cast<int>(std::lround(sampleRate_));
  a.audioBuffer = maxFrames_;
  a.engine.setInputChannels(variant_ == Variant::Effect ? 4 : 0);
  interleaved_.assign(static_cast<size_t>(kChunk) * 4, 0.f);
  a.syncEngine();  // hosted plugins and samples prepare for the new rate
  active_ = true;
  idle();
  return true;
}

void Instance::deactivate() { active_ = false; }

void Instance::setOffline(bool on) { app_->engine.setOffline(on); }

void Instance::applyMidiRouting() {
  App& a = *app_;
  const Song& s = a.doc.song();
  if (a.doc.revision() == routedRevision_ && a.selChannel == routedSel_) return;
  routedRevision_ = a.doc.revision();
  routedSel_ = a.selChannel;
  a.engine.setMidiTarget(a.selChannel);
  for (int k = 0; k < 16; ++k)
    a.engine.setMidiChannelRoute(k, midiMode_ == MidiMode::PerChannel && static_cast<size_t>(k) < s.channels.size()
                                        ? s.channels[static_cast<size_t>(k)].id
                                        : kNoId);
}

void Instance::setMidiMode(MidiMode m) {
  midiMode_ = m;
  routedRevision_ = ~0ull;
  applyMidiRouting();
}

void Instance::idle() {
  makeCurrent();
  App& a = *app_;
  a.tick(nowSeconds());
  pumpHostedPlugins(a);
  a.syncEngine();
  applyMidiRouting();
  // Linked parameters that disappeared (channel or effect deleted) unlink.
  bool changed = false;
  for (int k = 0; k < kMacros; ++k)
    if (links_[k].owner != kNoId && !describeParam(a.doc.song(), links_[k])) {
      links_[k] = ParamId{};
      changed = true;
    }
  if (changed) publishMacros();
  const int lat = a.engine.outputLatency();
  if (lat != latency_.load(std::memory_order_relaxed)) {
    latency_.store(lat, std::memory_order_relaxed);
    if (onLatencyChanged) onLatencyChanged();
  }
}

// ---------------------------------------------------------------- macros

std::string Instance::macroName(int k) const {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%s %d", i18n::tr("Macro"), k + 1);
  return buf;
}

std::string Instance::macroTarget(int k) const {
  if (k < 0 || k >= kMacros || links_[k].owner == kNoId) return {};
  return paramDisplayName(app_->doc.song(), links_[k]);
}

ParamId Instance::macroLink(int k) const { return k >= 0 && k < kMacros ? links_[k] : ParamId{}; }

void Instance::publishMacros() {
  App& a = *app_;
  std::vector<ParamId> t(links_, links_ + kMacros);
  a.builder.setHostMacros(t);
  a.forceRebuild();
  a.syncEngine();
  if (onMacrosChanged) onMacrosChanged();
}

void Instance::linkMacro(int k, ParamId target) {
  if (k < 0 || k >= kMacros) return;
  makeCurrent();
  const Song& s = app_->doc.song();
  if (target.owner != kNoId) {
    auto desc = describeParam(s, target);
    auto cur = getParam(s, target);
    if (!desc || !cur) return;
    setMacroValue(k, desc->toNormalized(*cur));
    if (onMacroEdited) onMacroEdited(k, macroValue(k));
  }
  links_[k] = target;
  publishMacros();
}

double Instance::macroValue(int k) const {
  if (k < 0 || k >= kMacros) return 0;
  const float v = macroValues_[k].load(std::memory_order_relaxed);
  return v < 0 ? 0.0 : v;
}

void Instance::setMacroValue(int k, double normalized) {
  if (k < 0 || k >= kMacros) return;
  const float v = static_cast<float>(std::clamp(normalized, 0.0, 1.0));
  macroValues_[k].store(v, std::memory_order_relaxed);
  app_->engine.setHostMacro(k, v);
}

std::string Instance::macroText(int k, double normalized) const {
  if (k >= 0 && k < kMacros && links_[k].owner != kNoId)
    if (auto desc = describeParam(app_->doc.song(), links_[k])) return desc->format(desc->fromNormalized(std::clamp(normalized, 0.0, 1.0)));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f%%", std::clamp(normalized, 0.0, 1.0) * 100.0);
  return buf;
}

// ---------------------------------------------------------------- state

std::string Instance::saveState() {
  makeCurrent();
  App& a = *app_;
  captureHostedStates(a);  // hosted plugins' own states into the song
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString(kStateFormat);
  j.object["version"] = Json::makeNumber(kStateVersion);
  j.object["variant"] = Json::makeString(variant_ == Variant::Effect ? "effect" : "instrument");
  j.object["project"] = songToJson(a.doc.song());
  Json samples = Json::makeArray();
  for (const auto& f : referencedFiles(a.doc.song())) samples.array.push_back(Json::makeString(f));
  j.object["samples"] = std::move(samples);
  Json macros = Json::makeArray();
  for (int k = 0; k < kMacros; ++k) {
    Json m = Json::makeObject();
    m.object["owner"] = Json::makeNumber(links_[k].owner);
    m.object["index"] = Json::makeNumber(links_[k].index);
    m.object["value"] = Json::makeNumber(macroValues_[k].load(std::memory_order_relaxed));
    macros.array.push_back(std::move(m));
  }
  j.object["macros"] = std::move(macros);
  Json set = Json::makeObject();
  Json follow;
  follow.type = Json::Type::Bool;
  follow.boolean = followHost();
  set.object["followHost"] = follow;
  set.object["midiMode"] = Json::makeNumber(static_cast<int>(midiMode_));
  set.object["mode"] = Json::makeNumber(static_cast<int>(a.engine.mode()));
  set.object["pattern"] = Json::makeNumber(a.curPattern);
  set.object["channel"] = Json::makeNumber(a.selChannel);
  j.object["settings"] = std::move(set);
  return jsonStringify(j, 0);
}

bool Instance::loadState(const std::string& data, std::string* err) {
  makeCurrent();
  App& a = *app_;
  Json j;
  if (!jsonParse(data, j) || !j.is(Json::Type::Object) || j.text("format") != kStateFormat) {
    if (err) *err = "not a Gridline plugin state";
    return false;
  }
  const Json* pj = j.find("project");
  Song s;
  std::string e;
  if (!pj || !songFromJson(*pj, s, &e)) {
    if (err) *err = "project: " + e;
    return false;
  }
  a.startProject(std::move(s));
  const Song& song = a.doc.song();
  if (const Json* set = j.find("settings")) {
    if (const Json* f = set->find("followHost"); f && f->is(Json::Type::Bool)) setFollowHost(f->boolean);
    midiMode_ = set->num("midiMode") >= 0.5 ? MidiMode::PerChannel : MidiMode::Selected;
    const ObjectId pat = jsonInt<ObjectId>(set->num("pattern"));
    const ObjectId ch = jsonInt<ObjectId>(set->num("channel"));
    if (song.pattern(pat)) a.selectPattern(pat);
    if (song.channel(ch)) {
      a.selChannel = ch;
      a.selChannels = {ch};
    }
    const int mode = jsonInt<int>(set->num("mode"));
    a.engine.setMode(mode == 1 ? PlayMode::Song : mode == 2 ? PlayMode::Live : PlayMode::Pattern);
  }
  for (int k = 0; k < kMacros; ++k) {
    links_[k] = ParamId{};
    macroValues_[k].store(-1.f);
    a.engine.setHostMacro(k, -1.f);
  }
  if (const Json* ms = j.find("macros"); ms && ms->is(Json::Type::Array))
    for (size_t k = 0; k < ms->array.size() && k < static_cast<size_t>(kMacros); ++k) {
      const Json& m = ms->array[k];
      ParamId t{jsonInt<ObjectId>(m.num("owner")), jsonInt<uint32_t>(m.num("index"))};
      if (t.owner != kNoId && describeParam(song, t)) links_[k] = t;
      double v = m.num("value", -1);
      if (v < 0 && links_[k].owner != kNoId) {  // no value saved: the parameter's own value
        auto desc = describeParam(song, t);
        auto cur = getParam(song, t);
        if (desc && cur) v = desc->toNormalized(*cur);
      }
      if (v >= 0) setMacroValue(static_cast<int>(k), v);
    }
  routedRevision_ = ~0ull;
  publishMacros();
  applyMidiRouting();
  idle();
  return true;
}

// ---------------------------------------------------------------- audio thread

bool Instance::followingHost() const { return app_->engine.followingHost(); }

void Instance::queueMidi(const uint8_t m[3]) { app_->engine.midiEvent(m[0], m[1], m[2]); }

void Instance::runSlice(const ProcessArgs& a, int from, int len, bool first) {
  AudioEngine& eng = app_->engine;
  if (first) {
    AudioEngine::HostTransport t;
    t.follow = followHost() && a.transport.valid;
    t.playing = a.transport.playing;
    t.hasPosition = a.transport.hasPosition;
    t.beats = a.transport.beats;
    t.bpm = a.transport.hasTempo ? a.transport.bpm : 0.0;
    eng.setHostTransport(t);
  }
  float* tl[kExtraOutputs];
  float* tr[kExtraOutputs];
  static constexpr int kTapMixer[kExtraOutputs] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const int taps = std::clamp(a.extraCount, 0, kExtraOutputs);
  for (int pos = 0; pos < len;) {
    const int n = std::min(kChunk, len - pos);
    const int at = from + pos;
    if (variant_ == Variant::Effect) {  // host input → engine input channels 1-2 (main), 3-4 (sidechain)
      const float* src[4] = {a.inL, a.inR, a.scL, a.scR};
      float* x = interleaved_.data();
      for (int i = 0; i < n; ++i)
        for (int c = 0; c < 4; ++c) x[i * 4 + c] = src[c] ? src[c][at + i] : 0.f;
      eng.pushInput(x, n);
    }
    for (int k = 0; k < taps; ++k) {
      tl[k] = a.extraL[k] + at;
      tr[k] = a.extraR[k] + at;
    }
    AudioEngine::OutputTaps ot;
    ot.count = taps;
    ot.mixer = kTapMixer;
    ot.L = tl;
    ot.R = tr;
    eng.process(a.outL + at, a.outR + at, n, taps > 0 ? &ot : nullptr);
    pos += n;
  }
}

void Instance::process(const ProcessArgs& a) {
  if (a.frames <= 0 || !a.outL || !a.outR) return;
  if (!active_ || interleaved_.empty()) {
    std::fill(a.outL, a.outL + a.frames, 0.f);
    std::fill(a.outR, a.outR + a.frames, 0.f);
    for (int k = 0; k < a.extraCount; ++k) {
      std::fill(a.extraL[k], a.extraL[k] + a.frames, 0.f);
      std::fill(a.extraR[k], a.extraR[k] + a.frames, 0.f);
    }
    return;
  }
  AudioEngine& eng = app_->engine;
  int pos = 0, e = 0;
  bool first = true;
  while (pos < a.frames) {
    // Events due now take effect before the frames after them.
    while (e < a.eventCount && a.events[e].offset <= pos) {
      const Event& ev = a.events[e++];
      if (ev.kind == Event::Midi)
        eng.midiEvent(ev.midi[0], ev.midi[1], ev.midi[2]);
      else if (ev.macro >= 0 && ev.macro < kMacros) {
        const float v = std::clamp(ev.value, 0.f, 1.f);
        macroValues_[ev.macro].store(v, std::memory_order_relaxed);
        eng.setHostMacro(ev.macro, v);
      }
    }
    int end = a.frames;
    if (e < a.eventCount) end = std::clamp(a.events[e].offset, std::min(pos + 1, a.frames), a.frames);
    runSlice(a, pos, end - pos, first);
    first = false;
    pos = end;
  }
  // Events at or after the end of the block (malformed offsets) still count.
  for (; e < a.eventCount; ++e) {
    const Event& ev = a.events[e];
    if (ev.kind == Event::Midi) eng.midiEvent(ev.midi[0], ev.midi[1], ev.midi[2]);
  }
}

}  // namespace gl::plugin
