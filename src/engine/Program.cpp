#include "../core/ClipPlayback.h"
#include "../packs/Packs.h"
#include "../core/Groove.h"
#include "../core/Lanes.h"
#include "Program.h"
#include "ArmedRing.h"
#include "../core/StageRoomOps.h"
#include "../core/LayerPick.h"
#include "Freeze.h"
#include "ClipSource.h"
#include "ChannelIns.h"
#include "ClipOps.h"
#include "SampleTap.h"
#include "../packs/edison/edison.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include "../core/Arrangements.h"
#include "../core/TrackModes.h"
#include "../core/ClipGain.h"
#include "../core/ClipLoop.h"
#include "../core/StageRooms.h"
#include "../core/ClipRate.h"
#include "../core/Lfo.h"
#include "../core/PatternAutomation.h"
#include "../core/PluginParams.h"
#include "Delay3Ref.h"
#include "Dsp.h"
#include "Slicing.h"

namespace gl {

namespace {

// J-36: parameters that change while playing without a program build:
// automation clips, pattern automation and internal controller links.
using LiveTargets = std::set<std::pair<ObjectId, uint32_t>>;
LiveTargets liveTargets(const Song& song) {
  LiveTargets t;
  for (const auto& a : song.automations) t.insert({a.owner, a.index});
  for (const auto& p : song.patterns)
    for (const auto& l : p.automation) t.insert({l.owner, l.index});
  for (const auto& m : song.modLinks) t.insert({m.owner, m.index});
  return t;
}

// What prepareHost sees: automated parameters at their maximum, so a section
// that automation may switch on mid-song (Matrix FM delay / reverb / pluck,
// Multi Delay units: lazily allocated) has its memory before it plays.
// `base` = ParamId index of the plugin's parameter 0 on `owner`.
void prepareForHost(const PluginState& st, const LiveTargets& live, ObjectId owner, uint32_t base,
                    const std::function<void(const PluginState&)>& prepare) {
  const PluginInfo* info = live.empty() ? nullptr : findPlugin(st.type);
  if (!info) return prepare(st);
  auto lo = live.lower_bound({owner, base});
  if (lo == live.end() || lo->first != owner || lo->second >= base + static_cast<uint32_t>(st.params.size())) return prepare(st);
  PluginState s = st;
  for (auto it = lo; it != live.end() && it->first == owner; ++it) {
    if (it->second < base) continue;
    const size_t k = it->second - base;
    if (k >= s.params.size() || k >= info->params.size()) break;
    s.params[k] = static_cast<float>(info->params[k].max);
  }
  prepare(s);
}

// Effects that move audio between mixer tracks (see ProgSlot). Parameter 0
// of each selects the destination: a mixer track number, or a bus 0..7.
constexpr const char* kMixerSendType = "gl.send";
constexpr const char* kBusSendType = "gl.bussend";
constexpr const char* kBusReturnType = "gl.busreturn";

bool hasOutputs(const std::string& type) {
  const PluginInfo* p = findPlugin(type);
  return p && !p->outputs.empty();
}

// A new effect runtime of `type` (send buffers for the types that send); nullptr when the
// effect cannot be created.
std::shared_ptr<EffectRuntime> newEffectRuntime(const std::string& type) {
  auto fx = createEffect(type);
  if (!fx) return nullptr;
  auto rt = std::make_shared<EffectRuntime>();
  rt->type = type;
  rt->fx = std::move(fx);
  if (type == kMixerSendType || type == kBusSendType) {  // pulled by the destination
    rt->sendL.assign(kEngineMaxBlock, 0.f);
    rt->sendR.assign(kEngineMaxBlock, 0.f);
  }
  if (const PluginInfo* pi = findPlugin(type); pi && !pi->sendParams.empty())
    for (size_t k = 0; k < pi->sendParams.size() && k < static_cast<size_t>(kMaxSendOutputs); ++k) {
      rt->multiL[k].assign(kEngineMaxBlock, 0.f);
      rt->multiR[k].assign(kEngineMaxBlock, 0.f);
    }
  return rt;
}

// 0.7.8: the effect runtimes of a build, by (mixer track, slot). A runtime follows its effect: one
// that moved (a move up or down, a drag to another slot or track, the undo or redo of one, a
// collaborator's move) keeps its live instance, so a third-party plugin keeps everything it has,
// also what it never reported (before, the runtime stayed with the slot number and the instance
// there took the moved effect's saved state, or a new one was made: plugins that save lazily came
// back reset). Each effect is known by its uid (EffectSlot::uid, given by the document); for one
// without (or a uid this builder has not seen: a song from elsewhere) the previous Program says what
// state each runtime was built with:
//  0. the runtime built for the same uid;
//  1. the slot's own runtime when the slot holds the same effect as before (type and state);
//  2. else an unclaimed runtime of the same effect from another slot (it moved here): the same
//     track first, then the nearest slot;
//  3. else the slot's own runtime of the same type (it takes the new state, as before);
//  4. else none: the build makes a new one.
// An installed plugin (VST3, CLAP ...) that is known now: a silent runtime made while it was missing
// (a collaborator's project, a plugin not installed yet) is made again after a rescan finds it.
bool installedNow(const std::string& type) { return isHostedType(type) && findPlugin(type) != nullptr; }

using EffectKey = std::pair<ObjectId, int>;
std::map<EffectKey, std::shared_ptr<EffectRuntime>> matchEffectRuntimes(const Song& song, const std::map<EffectKey, std::shared_ptr<EffectRuntime>>& had,
                                                                       const std::unordered_map<uint64_t, std::shared_ptr<EffectRuntime>>& byUid,
                                                                       const Program* prev) {
  std::map<EffectKey, std::shared_ptr<EffectRuntime>> out;
  std::unordered_map<const EffectRuntime*, const PluginState*> builtWith;
  if (prev)
    for (const ProgMixer& pm : prev->mixer)
      for (const ProgSlot& ps : pm.slots)
        if (ps.rt) builtWith[ps.rt.get()] = &ps.plugin;
  const auto usable = [](const std::shared_ptr<EffectRuntime>& rt, const std::string& type) {
    // (a silent one of an installed plugin that was missing is made again once it is installed)
    return rt && rt->type == type && !(rt->fx && rt->fx->stale()) && (rt->fx || !installedNow(type));
  };
  const auto same = [&](const EffectRuntime* rt, const PluginState& st) {
    auto it = builtWith.find(rt);
    return it != builtWith.end() && *it->second == st;
  };
  struct Want {
    EffectKey key;
    const PluginState* st;
    uint64_t uid;
  };
  std::vector<Want> wants;
  for (const MixerTrack& m : song.mixer)
    for (size_t s = 0; s < m.slots.size(); ++s)
      if (!m.slots[s].plugin.empty()) wants.push_back({{m.id, static_cast<int>(s)}, &m.slots[s].plugin, m.slots[s].uid});
  std::unordered_set<const EffectRuntime*> claimed;
  std::vector<const Want*> byState;
  for (const Want& w : wants) {  // 0
    auto u = w.uid ? byUid.find(w.uid) : byUid.end();
    if (u != byUid.end() && usable(u->second, w.st->type) && !claimed.count(u->second.get())) {
      out[w.key] = u->second;
      claimed.insert(u->second.get());
    } else {
      byState.push_back(&w);
    }
  }
  std::vector<const Want*> open;
  for (const Want* wp : byState) {  // 1
    const Want& w = *wp;
    auto e = had.find(w.key);
    if (e != had.end() && usable(e->second, w.st->type) && !claimed.count(e->second.get()) && same(e->second.get(), *w.st)) {
      out[w.key] = e->second;
      claimed.insert(e->second.get());
    } else {
      open.push_back(wp);
    }
  }
  if (!open.empty() && !builtWith.empty()) {  // 2
    std::vector<const Want*> still;
    for (const Want* w : open) {
      const std::pair<const EffectKey, std::shared_ptr<EffectRuntime>>* best = nullptr;
      long bestCost = 0;
      for (const auto& kv : had) {
        if (claimed.count(kv.second.get()) || !usable(kv.second, w->st->type) || !same(kv.second.get(), *w->st)) continue;
        const long cost = (kv.first.first == w->key.first ? 0 : 1000) + std::labs(static_cast<long>(kv.first.second) - w->key.second);
        if (!best || cost < bestCost) best = &kv, bestCost = cost;
      }
      if (best) {
        out[w->key] = best->second;
        claimed.insert(best->second.get());
      } else {
        still.push_back(w);
      }
    }
    open = std::move(still);
  }
  for (const Want* w : open) {  // 3
    auto e = had.find(w->key);
    if (e != had.end() && usable(e->second, w->st->type) && !claimed.count(e->second.get())) {
      out[w->key] = e->second;
      claimed.insert(e->second.get());
    }
  }
  return out;
}

// A new channel runtime of `type` (gen is null when the instrument cannot be created).
// K-64: how long exact silence must have gone through an effect before realtime playback leaves it
// out (seconds, longer than anything it can still hold; 0: it always runs). Only effects whose whole
// state is their signal, so that leaving them out cannot change what comes later: no free-running
// modulation (its phase would differ from an export's), nothing that makes sound on its own.
double restSeconds(const PluginState& p) {
  using namespace pp;
  const auto is = [&](int i, float def) { return static_cast<size_t>(i) < p.params.size() ? p.params[static_cast<size_t>(i)] : def; };
  if (p.type == "gl.eq") return is(EqListen, 0) > 0.5f ? 0.0 : 3.0;  // (linear phase: its FIR is shorter)
  if (p.type == "gl.clipper") return 1.0;  // K-277: no state but its oversampling filters and a 64-sample delay
  if (p.type == "gl.delay") {
    const int model = static_cast<int>(std::lround(is(DlyModel, 0)));
    if (model == 0 && (is(DlyModDepth, 0) != 0 || is(DlyModCutoff, 0) != 0)) return 0.0;
    if (model == 3 && (is(DlyD3Raw0 + d3ref::PModTime, 0) != 0 || is(DlyD3Raw0 + d3ref::PModCut, 0) != 0)) return 0.0;
    return 14.0;  // one bar at 20 BPM
  }
  if (p.type == "gl.reverb") {
    const int model = static_cast<int>(std::lround(is(RvbModel, 0)));
    if (is(RvbFreeze, 0) > 0.5f) return 0.0;
    if (model == 0 && is(RvbModDepth, 0) != 0) return 0.0;
    if (model == 2 && is(RvbFModDepth, 0) != 0) return 0.0;
    return 6.0;  // (the longest pre-delay and the first reflections)
  }
  return 0.0;
}

// GitHub #66: built-in effects that may rest while the transport is stopped, nothing comes into them
// and what they put out has stayed below -90 dBFS for a while (the rule hosted effects follow). Only
// effects that make no sound of their own: no playback started from their window, no generator.
// Their free-running modulation (a chorus, a hall's movement) stands still while they rest, which
// only a stopped transport allows: Play wakes them, and renders never leave anything out.
bool restsWhenStopped(const std::string& type) {
  static const std::set<std::string> kTypes = {"gl.autopan", "gl.bitcrush", "gl.chorus",    "gl.clipper",  "gl.comp",    "gl.delay",
                                                "gl.dist",    "gl.eq",       "gl.filter",    "gl.flanger",  "gl.freqshift", "gl.gate",
                                                "gl.geq",     "gl.hall",     "gl.limiter",   "gl.maximizer", "gl.phaser",   "gl.reverb",
                                                "gl.scduck",  "gl.shaper",   "gl.stereo",    "gl.utility",  "gl.waveshaper"};
  return kTypes.count(type) != 0;
}

std::shared_ptr<ChannelRuntime> newChannelRuntime(const std::string& type) {
  auto rt = std::make_shared<ChannelRuntime>();
  rt->type = type;
  rt->gen = createGenerator(type);
  if (rt->gen) rt->gen->setMidiOut(&rt->midiOut);
  return rt;
}

// The built-in types that allocate in their first block (tests/test_q18.cpp
// fails on any other type that does). Patch hosts are not in the list: they can
// hold hosted plugins, which must not run here; the built-in modules inside get
// this warm-up when the host prepares them (K-59, warmUpBuiltin). A block of no frames on the builder's
// thread does that work where allocation is allowed, and advances no phase or
// random state, so the sound stays the same; the runtime is not shared yet.
bool allocatesInFirstBlock(const std::string& type) {
  static const std::set<std::string> kTypes = {"gl.chorus",      "gl.delay",    "gl.eq",         "gl.geq",
                                                "gl.reverb",      "gl.timegate", "gl.vocoder",    "gl.dcosynth",
                                                "gl.flex",        "gl.hexsynth", "gl.membranepad", "gl.pocketsynth",
                                                "gl.twinstring",  "gl.waveprogram"};
  return kTypes.count(type) != 0;
}
ProcessContext warmUpContext(double sampleRate) {
  ProcessContext ctx;
  ctx.sampleRate = sampleRate;
  return ctx;
}
void warmUp(EffectRuntime& rt, const PluginState& state, const ChannelAux& aux, double sampleRate) {
  if (rt.warmed) return;
  rt.warmed = true;
  if (!rt.fx || !allocatesInFirstBlock(rt.type)) return;
  float l = 0, r = 0;
  rt.fx->processAux(state, aux, warmUpContext(sampleRate), &l, &r, 0);
  rt.fx->reset();
}
void warmUp(ChannelRuntime& rt, const PluginState& state, const ChannelAux& aux, double sampleRate) {
  if (rt.warmed) return;
  rt.warmed = true;
  if (!rt.gen || !allocatesInFirstBlock(rt.type)) return;
  float l = 0, r = 0;
  rt.gen->process(state, aux, warmUpContext(sampleRate), &l, &r, 0);
  rt.gen->kill();
}

int paramInt(const PluginState& s, size_t k, int dflt) {
  return k < s.params.size() ? static_cast<int>(std::lround(s.params[k])) : dflt;
}

// J-45: the send parameters of this state hold FL route indexes (PluginInfo::routeModeParam).
bool routeIndexed(const PluginInfo* pi, const PluginState& s) {
  if (!pi || pi->routeModeParam == -1) return false;
  if (pi->routeModeParam == kRouteIndexAlways) return true;
  const size_t k = static_cast<size_t>(pi->routeModeParam);
  return k < s.params.size() && s.params[k] >= 0.5f;
}
// A send parameter's destination mixer index (-1 = none) for the effect on track `track`.
int sendDestination(const Song& song, const PluginInfo* pi, const PluginState& s, int track, size_t param, int dflt) {
  const int v = paramInt(s, param, dflt);
  return routeIndexed(pi, s) ? flRouteOutput(song, track, v) : v;
}

// Mixer processing order over the song's sends plus extra (from, to) edges
// (slot sends, bus send → return). Extra edges that would close a cycle are
// dropped and reported through `accepted`. Sources come before destinations;
// the master is last.
std::vector<int> orderWithEdges(const Song& song, const std::vector<std::pair<int, int>>& extra,
                                std::vector<bool>& accepted) {
  size_t n = song.mixer.size();
  std::vector<std::vector<int>> adj(n);
  for (size_t i = 0; i < n; ++i)
    for (const auto& s : song.mixer[i].sends) {
      int to = song.mixerIndex(s.to);
      if (to >= 0) adj[i].push_back(to);
    }
  auto reaches = [&](int from, int to) {  // is `to` reachable from `from`?
    std::vector<char> seen(n, 0);
    std::vector<int> stack{from};
    while (!stack.empty()) {
      int x = stack.back();
      stack.pop_back();
      if (x == to) return true;
      if (seen[static_cast<size_t>(x)]) continue;
      seen[static_cast<size_t>(x)] = 1;
      for (int y : adj[static_cast<size_t>(x)]) stack.push_back(y);
    }
    return false;
  };
  int master = std::max(0, song.mixerIndex(song.masterId()));
  accepted.assign(extra.size(), false);
  for (size_t e = 0; e < extra.size(); ++e) {
    auto [a, b] = extra[e];
    if (a < 0 || b < 0 || a == b || static_cast<size_t>(a) >= n || static_cast<size_t>(b) >= n) continue;
    if (a == master) continue;           // the master feeds the output only
    if (reaches(b, a)) continue;         // would close a cycle
    adj[static_cast<size_t>(a)].push_back(b);
    accepted[e] = true;
  }
  std::vector<int> out;
  std::vector<char> state(n, 0);  // 0 new, 1 active, 2 done
  std::function<void(int)> visit = [&](int x) {
    if (state[static_cast<size_t>(x)]) return;
    state[static_cast<size_t>(x)] = 1;
    for (int y : adj[static_cast<size_t>(x)]) visit(y);
    state[static_cast<size_t>(x)] = 2;
    out.push_back(x);
  };
  // Started in id order (see mixerOrder in core/Song.cpp): a reordered mixer sums in the same order.
  std::vector<int> roots;
  for (size_t i = 0; i < n; ++i) roots.push_back(static_cast<int>(i));
  std::stable_sort(roots.begin(), roots.end(), [&](int a, int b) { return song.mixer[static_cast<size_t>(a)].id < song.mixer[static_cast<size_t>(b)].id; });
  for (int i : roots) visit(i);
  std::reverse(out.begin(), out.end());
  auto it = std::find(out.begin(), out.end(), master);
  if (it != out.end()) {
    out.erase(it);
    out.push_back(master);
  }
  return out;
}

// Resolves a parameter that a controller link or an automation lane drives:
// fills the target fields of `pl` (kind, indices, descriptor, unlinked value)
// and marks the owner so the engine keeps live copies / recomputes strip gains.
// False for unresolved targets and song parameters.
bool resolveTarget(const Song& song, Program& prog, ParamId target, ProgLink& pl) {
  auto desc = describeParam(song, target);
  auto cur = getParam(song, target);
  if (!desc || !cur) return false;
  int ci = song.channelIndex(target.owner), mi = song.mixerIndex(target.owner);
  if (ci >= 0) {
    pl.chan = ci;
    ProgChannel& pc = prog.channels[static_cast<size_t>(ci)];
    if (target.index >= kNoteFxBase) {
      pl.kind = ProgLink::ChanNoteFx;
      pl.fx = static_cast<int>((target.index - kNoteFxBase) / kNoteFxStride);
      pl.param = static_cast<int>((target.index - kNoteFxBase) % kNoteFxStride);
      if (static_cast<size_t>(pl.fx) >= pc.noteFx.size()) return false;
      pc.noteFx[static_cast<size_t>(pl.fx)].modded = true;
    } else if (target.index >= kChannelPluginBase) {
      pl.kind = ProgLink::ChanPlugin;
      pl.param = static_cast<int>(target.index - kChannelPluginBase);
      pc.modded = true;
    } else if (target.index >= kLayerMacroBase && target.index < kLayerMacroBase + static_cast<uint32_t>(kLayerMacros)) {
      pl.kind = ProgLink::ChanMacro;  // K-169: a layer's macro
      pl.param = static_cast<int>(target.index - kLayerMacroBase);
    } else {
      switch (static_cast<ChannelParam>(target.index)) {
        case ChannelParam::Volume: pl.kind = ProgLink::ChanVolume; break;
        case ChannelParam::Pan: pl.kind = ProgLink::ChanPan; break;
        case ChannelParam::Pitch: pl.kind = ProgLink::ChanPitch; break;
        case ChannelParam::Mute: pl.kind = ProgLink::ChanMute; break;
        case ChannelParam::Sustain: pl.kind = ProgLink::ChanSustain; break;
        case ChannelParam::Transpose: pl.kind = ProgLink::ChanTranspose; break;
        default: return false;
      }
      if (pl.kind == ProgLink::ChanSustain) pc.sustainMoves = true;
      else if (pl.kind == ProgLink::ChanPitch) pc.pitchMoves = true;
      else if (pl.kind == ProgLink::ChanTranspose) pc.transposeMoves = true;
      else pc.stripLinked = true;
    }
  } else if (mi >= 0) {
    pl.mixer = mi;
    ProgMixer& pm = prog.mixer[static_cast<size_t>(mi)];
    if (target.index >= kSlotBase) {
      pl.slot = static_cast<int>((target.index - kSlotBase) / kSlotStride);
      uint32_t k = (target.index - kSlotBase) % kSlotStride;
      if (static_cast<size_t>(pl.slot) >= pm.slots.size()) return false;
      if (k == kSlotMix) {
        pl.kind = ProgLink::SlotMix;
      } else if (k == kSlotEnable) {
        pl.kind = ProgLink::SlotEnable;
      } else {
        pl.kind = ProgLink::SlotParam;
        pl.param = static_cast<int>(k);
        pm.slots[static_cast<size_t>(pl.slot)].modded = true;
      }
    } else if (target.index >= kLayerMacroBase && target.index < kLayerMacroBase + static_cast<uint32_t>(kLayerMacros)) {
      pl.kind = ProgLink::MixMacro;  // K-245: a mixer track's macro
      pl.param = static_cast<int>(target.index - kLayerMacroBase);
    } else if (target.index >= kSendBase) {
      int dest = static_cast<int>(target.index - kSendBase);
      pl.kind = ProgLink::MixSend;
      pl.param = -1;
      for (size_t i = 0; i < pm.sends.size(); ++i)
        if (pm.sends[i].first == dest) pl.param = static_cast<int>(i);
      if (pl.param < 0) return false;
    } else if (target.index >= static_cast<uint32_t>(MixerParam::EqLowGain) &&
               target.index <= static_cast<uint32_t>(MixerParam::EqHighQ)) {
      uint32_t r = target.index - static_cast<uint32_t>(MixerParam::EqLowGain);
      pl.kind = r < 3 ? ProgLink::MixEqGain : r < 6 ? ProgLink::MixEqFreq : ProgLink::MixEqQ;
      pl.param = static_cast<int>(r % 3);
    } else {
      switch (static_cast<MixerParam>(target.index)) {
        case MixerParam::Volume:
          pl.kind = ProgLink::MixVolume;
          pm.stripLinked = true;
          break;
        case MixerParam::Pan:
          pl.kind = ProgLink::MixPan;
          pm.stripLinked = true;
          break;
        case MixerParam::StereoSep:
          pl.kind = ProgLink::MixSep;
          if (prog.panLaw == 2) pm.stripLinked = true;  // Stage placement: the width is part of the strip gains
          break;
        case MixerParam::Depth:  // the distance gain is part of the strip gains
          pl.kind = ProgLink::MixDepth;
          pm.stripLinked = true;
          break;
        case MixerParam::Focus: pl.kind = ProgLink::MixFocus; break;
        case MixerParam::Mute:
          pl.kind = ProgLink::MixMute;
          pm.muteLinked = true;
          break;
        default: return false;
      }
    }
  } else if (const int li = song.lfoIndex(target.owner); li >= 0 && static_cast<size_t>(li) < prog.lfos.size()) {
    // K-247: a board LFO's own setting (Program::lfos follows the song's order).
    if (target.index >= static_cast<uint32_t>(lfo::LfoParam::Count)) return false;
    pl.kind = ProgLink::LfoSetting;
    pl.fx = li;
    pl.param = static_cast<int>(target.index);
  } else if (target.owner == kSongId) {
    // 0.7.9: live internal-controller links on song settings (tempo, swing, master pitch, groove accents).
    switch (static_cast<SongParam>(target.index)) {
      case SongParam::Tempo: pl.kind = ProgLink::SongTempo; prog.tempoLinked = true; break;
      case SongParam::Swing: pl.kind = ProgLink::SongSwing; prog.swingLinked = true; break;
      case SongParam::MasterPitch: pl.kind = ProgLink::SongMasterPitch; prog.masterPitchLinked = true; break;
      case SongParam::GrooveAccent: pl.kind = ProgLink::SongGrooveAccent; prog.grooveAccentLinked = true; break;
      default: return false;
    }
  } else {
    return false;
  }
  pl.desc = *desc;
  pl.base = desc->toNormalized(*cur);
  return true;
}

// Audio clip → playback description (arrangement). The channel's sampler
// settings give the sample, pitch shift, stretch mode and reverse; the clip
// adds its own window, gain, fades, pitch, stretch and tempo sync.
// J-87: how compiled clips get their offline-quality render.
struct ClipRenderCtx {
  double sampleRate = 44100;
  bool async = false;  // app builds: rendered in the background (realtime grains until ready)
  // J-196: an edit in progress (a knob turn): only renders already made are
  // used; the realtime stretch plays the rest until the edit settles.
  bool defer = false;
  bool* deferred = nullptr;  // set when a render was left for later
};

// J-196: the clip's levels (gain, fades, declick) as compiled; also what
// ProgramBuilder::patchClipLevels writes into a playing Program.
void setClipLevels(const ProgAudioClip& out, const Clip& c, const ClipFades* fades) {  // (the levels are mutable)
  out.gain = clipgain::playGain(c.gain);  // J-79: the bottom of FL's range is silence
  out.pan = std::clamp(c.pan, -1.0f, 1.0f);  // K-414: per-clip stereo pan
  if (fades) {  // J-38: resolved fades (automatic crossfades, curve shapes, tension)
    out.fadeIn = fades->in.len, out.fadeOut = fades->out.len;
    out.shapeIn = fades->in.shape, out.shapeOut = fades->out.shape;
    out.tensionIn = fades->in.tension, out.tensionOut = fades->out.tension;
  } else {  // the clip's own fades only (live clips play alone)
    out.fadeIn = c.fadeIn, out.fadeOut = c.fadeOut;
    out.shapeIn = c.fadeInShape == 1 ? FadeShape::Single : FadeShape::Smooth;
    out.shapeOut = c.fadeOutShape == 1 ? FadeShape::Single : FadeShape::Smooth;
    out.tensionIn = c.fadeInTension, out.tensionOut = c.fadeOutTension;
  }
  out.declickIn = c.fadeInDeclick, out.declickOut = c.fadeOutDeclick;  // J-43
  if (!out.head) out.fadeIn = 0;  // K-114: a later pass of a looping clip
  if (!out.tail) out.fadeOut = 0;
}

bool compileAudioClip(const Song& song, const Channel& ch, const Clip& c, const TempoMap& map, ProgAudioClip& out,
                      const ClipFades* fades = nullptr, const ClipRenderCtx* rctx = nullptr) {
  // K-85: the sound the channel's sample page makes (handles, precomputed effects, processing).
  const ClipSource src = clipSource(ch);
  if (!src.ok()) return false;
  const SamplePtr& smp = src.sample;
  auto par = [&](int k, double d) { return k < static_cast<int>(ch.plugin.params.size()) ? static_cast<double>(ch.plugin.params[static_cast<size_t>(k)]) : d; };
  const bool sampler = ch.plugin.type == "gl.sampler";
  // J-164: speed and pitch from the mapping the clip operations and the
  // waveform share (core/ClipRate.h): pitch never changes how the source
  // spans the clip (an FL clip's speed pitch aside).
  const ClipRate rate = clipRate(ch, c);
  const bool stretchMode = !rate.resample;
  // J-157: Rubber Band or the stretch keeps the pitch (also of tempo-synced clips).
  const bool rubber = sampler && stretch::modeOf(par(pp::SmpStretchMode, 0)) == stretch::Mode::RubberBand;
  out.engine = rubber ? stretch::Engine::RubberBand : stretch::Engine::Stretch;
  const stretch::Mode keepMode = rubber ? stretch::Mode::RubberBand : stretch::kStretch;
  const double timeFactor = rate.timeFactor;
  out.start = c.start;
  out.end = c.start + song.clipLength(c);
  out.startSec = map.secondsAt(static_cast<double>(out.start));
  out.durSec = map.secondsAt(static_cast<double>(out.end)) - out.startSec;
  out.sample = smp;
  out.reverse = c.reverse != (sampler && par(pp::SmpReverse, 0) >= 0.5);
  if (!clipWindow(src, out.reverse, c, out.srcStart, out.srcEnd)) return false;
  out.clip = c.id;
  setClipLevels(out, c, fades);
  // K-85: warp markers. The sections stretch through the stretch engine with
  // the clip's pitch kept, per clip, whatever the channel's time mode.
  const bool warped = !c.warp.empty() && !warpIsIdentity(c.warp);
  if (warped) {
    const double last = static_cast<double>(smp->frames() - 1) / smp->sampleRate;
    out.warp = warpPoints(c.warp, out.reverse, last);
    out.warpBase = warpTimeAt(out.warp, out.srcStart);
    uint64_t h = 1469598103934665603ull;
    for (const WarpPoint& p : out.warp)
      for (double v : {p.w, p.s}) {
        uint64_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        h = (h ^ b) * 1099511628211ull;
      }
    out.warpKey = h;
  }
  if (c.syncBpm > 0 && c.tempoLock && rate.resample && !warped) {
    // J-193: locked to the tempo and resampled: the pitch follows the speed
    // the tempo gives it (a record played faster or slower); `pitch` is the
    // pitch per unit of speed (the clip's pitch apart from its speed).
    out.secPerTick = 60.0 / (c.syncBpm * song.ppq) / timeFactor;
    out.resampleSync = true;
    out.pitch = rate.pitch / std::max(1e-9, rate.speed);
    out.granular = std::fabs(out.pitch - 1.0) > 1e-9;
  } else if (c.syncBpm > 0) {  // follows the tempo: always pitch-independent
    out.secPerTick = 60.0 / (c.syncBpm * song.ppq) / timeFactor;
    out.granular = true;
    out.pitch = rate.resample ? rate.pitch * timeFactor : rate.pitch;  // the pitch alone (the tempo fit keeps it)
  } else if (stretchMode || warped) {
    out.speed = rate.speed;
    out.pitch = rate.pitch;
    out.granular = warped || out.pitch != 1.0 || out.speed != 1.0;
  } else {  // resample: the stretch resamples; a pitch apart from the speed goes through the stretch
    out.speed = rate.speed;
    out.pitch = rate.pitch;
    out.granular = std::fabs(rate.pitch / rate.speed - 1.0) > 1e-9;
  }
  double played = out.secPerTick > 0 ? static_cast<double>(out.end - out.start) * out.secPerTick : out.durSec * out.speed;
  const double playedTo = warped ? warpSourceAt(out.warp, out.warpBase + played) : out.srcStart + played;
  out.cutEnd = playedTo < out.srcEnd - 1e-6;
  // J-87: offline-quality render of every pitched / stretched / synced clip.
  // The channel's own pitch (knob + fine + master pitch) is part of it; when
  // it moves at run time (automation), the realtime grains take over.
  if (rctx && out.end > out.start && out.srcEnd > out.srcStart) {
    const double chanSemis = static_cast<double>(ch.transpose) + (static_cast<double>(ch.pitchCents) + static_cast<double>(ch.fine)) / 100.0 + song.masterPitch;
    const double chanRatio = std::pow(2.0, chanSemis / 12.0);
    // J-153: every stored stretch mode is the one stretch (J-157: or Rubber Band)
    stretch::Mode mode = stretchMode ? keepMode : stretch::Mode::Resample;
    stretch::ClipRequest rq;
    rq.sample = smp;
    rq.srcStart = out.srcStart, rq.srcEnd = out.srcEnd;
    rq.reverse = out.reverse;
    rq.rate = rctx->sampleRate;
    const double rawFrames = (std::isfinite(out.durSec) && out.durSec > 0 && std::isfinite(rctx->sampleRate) && rctx->sampleRate > 0)
                                 ? std::ceil(out.durSec * rctx->sampleRate) + 2.0
                                 : 0.0;
    rq.frames = (rawFrames > 0.0 && rawFrames <= 1.0e12) ? static_cast<int64_t>(rawFrames) : 0;
    rq.atTick = static_cast<double>(out.start);
    bool need = false;
    if (out.resampleSync) {  // J-193: resampled through the tempo map (pitch follows the speed)
      rq.map = &map;
      rq.secPerTick = out.secPerTick;
      rq.startTick = static_cast<double>(out.start);
      rq.startSec = out.startSec;
      char sig[64];
      std::snprintf(sig, sizeof sig, "%llx", static_cast<unsigned long long>(map.signature()));
      rq.mapKey = sig;
      const double extra = out.pitch * chanRatio;  // pitch on top of the speed's
      const double spd = out.secPerTick * map.bpmAt(static_cast<double>(out.start)) * song.ppq / 60.0;
      if (std::fabs(extra - 1.0) <= 1e-9) {
        mode = stretch::Mode::Resample;
        rq.pitch = 1.0;
        need = !map.constant() || std::fabs(spd - 1.0) > 1e-9;
      } else if (map.constant()) {  // a constant speed: resampled, with the pitch on top through the stretch
        rq.map = nullptr;
        rq.secPerTick = 0, rq.startTick = 0, rq.startSec = 0;
        rq.mapKey.clear();
        mode = stretch::kStretch;
        rq.speed = spd;
        rq.pitch = spd * extra;
        need = true;
      }  // (a pitch on top of a changing tempo: the realtime path follows it)
    } else if (out.secPerTick > 0) {  // tempo sync: pitch-independent
      mode = keepMode;
      rq.map = &map;
      rq.secPerTick = out.secPerTick;
      rq.startTick = static_cast<double>(out.start);
      rq.startSec = out.startSec;
      char sig[64];
      std::snprintf(sig, sizeof sig, "%llx", static_cast<unsigned long long>(map.signature()));
      rq.mapKey = sig;
      rq.pitch = out.pitch * chanRatio;
      // At the clip's own tempo, unpitched, it is a plain read.
      need = std::fabs(rq.pitch - 1.0) > 1e-9 || !map.constant() || std::fabs(map.secondsAt(1.0) - map.secondsAt(0.0) - out.secPerTick) > 1e-12;
    } else if (out.granular || mode != stretch::Mode::Resample) {
      if (mode == stretch::Mode::Resample) mode = stretch::kStretch;  // J-164: resampled, pitched apart from its speed
      rq.speed = out.speed;
      rq.pitch = out.pitch * chanRatio;
      need = std::fabs(rq.pitch - 1.0) > 1e-9 || std::fabs(out.speed - 1.0) > 1e-9;
    } else {  // resample: speed carries the pitch; a channel pitch on top keeps the timing
      rq.speed = out.speed;
      rq.pitch = out.speed * chanRatio;  // J-164: the speed's pitch times the channel's, as the realtime path plays it
      if (std::fabs(chanRatio - 1.0) > 1e-9) mode = stretch::kStretch;
      need = std::fabs(out.speed - 1.0) > 1e-9 || std::fabs(chanRatio - 1.0) > 1e-9;
    }
    if (warped) {  // K-85: always rendered (pitch kept), the warp is part of the request
      if (mode == stretch::Mode::Resample) mode = keepMode;
      rq.warp = out.warp;
      rq.warpBase = out.warpBase;
      need = true;
    }
    rq.mode = mode;
    if (need && rctx->defer) {  // J-196: no new render while the edit goes on
      out.hq = stretch::findRenderedClip(rq);
      if (!out.hq && rctx->deferred) *rctx->deferred = true;
      out.hqSemis = chanSemis;
    } else if (need && !tap::readsOnly()) {  // K-319: a Track Receipt pass that only measures plays no clip sound
      out.hq = stretch::renderClip(rq, rctx->async);
      out.hqSemis = chanSemis;
    }
    out.hqNeeded = need;
  }
  return out.end > out.start && out.srcEnd > out.srcStart;
}

}  // namespace

void warmUpBuiltin(Generator& g, const std::string& type, const PluginState& state, const ChannelAux& aux, double sampleRate) {
  if (isHostedType(type) || !allocatesInFirstBlock(type)) return;
  float l = 0, r = 0;
  g.process(state, aux, warmUpContext(sampleRate), &l, &r, 0);
  g.kill();
}
void warmUpBuiltin(Effect& fx, const std::string& type, const PluginState& state, const ChannelAux& aux, double sampleRate) {
  if (isHostedType(type) || !allocatesInFirstBlock(type)) return;
  float l = 0, r = 0;
  fx.processAux(state, aux, warmUpContext(sampleRate), &l, &r, 0);
  fx.reset();
}
double effectRestSeconds(const PluginState& state) { return restSeconds(state); }
bool effectRestsWhenStopped(const std::string& type) { return restsWhenStopped(type); }

namespace {

NoteEv makeEv(Tick on, Tick off, float pitch, float velocity) {
  NoteEv e;
  e.on = on;
  e.off = off;
  e.pitch = pitch;
  e.velocity = velocity;
  return e;
}

// Start order: by tick; at the same tick ordinary notes come before slide
// notes, so a slide that starts with its note glides that note.
void sortEvents(std::vector<NoteEv>& v) {
  std::stable_sort(v.begin(), v.end(), [](const NoteEv& a, const NoteEv& b) {
    if (a.on != b.on) return a.on < b.on;
    if (a.onFrac != b.onFrac) return a.onFrac < b.onFrac;
    return (a.flags & kNoteSlide) < (b.flags & kNoteSlide);
  });
}

// GitHub #87: each 0% note gets the loudest velocity of the slide notes that start while it sounds
// (its colour group, or any with slidesAnyColour), the level a slide can fade it in to. `v` is sorted.
void markSlideVelRefs(std::vector<NoteEv>& v, bool anyColour) {
  for (size_t i = 0; i < v.size(); ++i) {
    NoteEv& e = v[i];
    if ((e.flags & kNoteSlide) || e.velocity > 0) continue;
    const double on = noteOnTick(e), off = noteOffTick(e);
    float peak = 0;
    for (size_t k = i + 1; k < v.size(); ++k) {
      const NoteEv& s = v[k];
      if (noteOnTick(s) >= off) break;
      if ((s.flags & kNoteSlide) && noteOnTick(s) >= on && (anyColour || (s.props.color & 15) == (e.props.color & 15)))
        peak = std::max(peak, s.velocity);
    }
    e.slideVelRef = std::clamp(peak, 0.0f, 1.0f);
  }
}

// Sets a start / end tick with its fraction (J-138).
void setOn(NoteEv& e, double t) {
  const double f = std::floor(t);
  e.on = static_cast<Tick>(f);
  e.onFrac = static_cast<float>(t - f);
  if (e.onFrac >= 1.0f) e.on += 1, e.onFrac = 0;
}
void setOff(NoteEv& e, double t) {
  const double f = std::floor(t);
  e.off = static_cast<Tick>(f);
  e.offFrac = static_cast<float>(t - f);
  if (e.offFrac >= 1.0f) e.off += 1, e.offFrac = 0;
}

// Deterministic per-note choice for random layers (same result on every build).
uint32_t layerHash(const NoteEv& e, ObjectId layer) {
  uint32_t h = static_cast<uint32_t>(e.on) * 2654435761u ^ static_cast<uint32_t>(e.pitch * 16.0f) * 40503u ^ layer * 97u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

// K-169: the child weights of one note of layer `l` (see layer::pick): zone weight, layer solo,
// the child's mute and own key range (after the transpose). Children beyond 32 never play.
void rackWeights(const Song& song, const Channel& l, int key, int vel127, float shift, const NoteProps& props, double pitch,
                 float* w, int* transpose) {
  bool solo = false;
  for (const auto& z : l.layerZones) solo = solo || (z.solo && std::find(l.layer.begin(), l.layer.end(), z.child) != l.layer.end());
  const int n = static_cast<int>(std::min<size_t>(l.layer.size(), 32));
  for (int i = 0; i < n; ++i) {
    const ObjectId kid = l.layer[static_cast<size_t>(i)];
    const Channel* c = song.channel(kid);
    const LayerZone* z = l.layerZone(kid);
    const LayerZone full;
    const LayerZone& zz = z ? *z : full;
    transpose[i] = zz.transpose;
    w[i] = 0;
    if (!c || c->muted || (solo && !zz.solo)) continue;
    const int own = noteKey(pitch + shift + zz.transpose, props);
    if (own < c->keyLo || own > c->keyHi) continue;
    w[i] = layerZoneWeight(zz, key, vel127);
  }
}

void addRackNotes(const Song& song, const Channel& l, size_t self, float shift, const std::vector<NoteEv>& src,
                  std::vector<NoteEv>& out) {
  if (self >= 32) return;
  std::vector<size_t> order(src.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const NoteEv &x = src[a], &y = src[b];
    if (x.on != y.on) return x.on < y.on;
    if (x.onFrac != y.onFrac) return x.onFrac < y.onFrac;
    return x.pitch < y.pitch;
  });
  layer::PickState st;
  float w[32];
  int tr[32];
  const int n = static_cast<int>(std::min<size_t>(l.layer.size(), 32));
  for (size_t i : order) {
    const NoteEv& e = src[i];
    const int key = noteKey(e.pitch, e.props);
    const int vel = static_cast<int>(std::lround(std::clamp(e.velocity, 0.0f, 1.0f) * 127.0f));
    rackWeights(song, l, key, vel, shift, e.props, e.pitch, w, tr);
    const uint32_t mask = layer::pick(l.layerMode, w, n, layerHash(e, l.id), st);
    if (!(mask & (1u << self))) continue;
    NoteEv x = e;
    x.pitch += shift + static_cast<float>(tr[self]);
    x.velocity = e.velocity * w[self];  // fades play softer (fade by velocity)
    out.push_back(x);
  }
}

// Notes that reach `target` through layer channels (nested up to 4 deep).
void addLayerNotes(const Song& song, const Pattern& p, const Channel& target, int depth, std::vector<NoteEv>& out) {
  if (depth > 4) return;
  for (const auto& l : song.channels) {
    if (l.id == target.id || l.layer.empty()) continue;
    auto pos = std::find(l.layer.begin(), l.layer.end(), target.id);
    if (pos == l.layer.end()) continue;
    std::vector<NoteEv> src = compileChannelNotes(song, p, l);
    addLayerNotes(song, p, l, depth + 1, src);
    const float shift = static_cast<float>(l.transpose) + (l.pitchCents + static_cast<float>(l.fine)) / 100.0f;
    const size_t self = static_cast<size_t>(pos - l.layer.begin());
    if (l.layerRack()) {  // K-169: zones, fades, transpose, layer solo, cycle / no-repeat modes
      addRackNotes(song, l, self, shift, src, out);
      continue;
    }
    for (NoteEv e : src) {
      if (l.layerMode == 1 && layerHash(e, l.id) % l.layer.size() != self) continue;
      e.pitch += shift;
      out.push_back(e);
    }
  }
}

// Channel MIDI output ports -> channels listening on the same input port: hosted plugins' MIDI
// output (played one block later) and MIDI Out channels (the same block, in Program::midiOrder).
// Links that would bring a channel's MIDI back to it are left out (midiPortListeners).
void linkMidiPorts(const Song& song, Program& prog) {
  const size_t n = std::min(song.channels.size(), prog.channels.size());
  std::vector<std::vector<int>> out = midiPortListeners(song, &prog.midiLoops);
  out.resize(n);
  for (auto& o : out) o.erase(std::remove_if(o.begin(), o.end(), [n](int t) { return static_cast<size_t>(t) >= n; }), o.end());
  std::vector<int> feeders(n, 0);
  for (size_t i = 0; i < n; ++i) {
    ProgChannel& pc = prog.channels[i];
    pc.midiTargets = out[i];
    pc.midiFirst = !out[i].empty() && song.channels[i].plugin.type == "gl.midiout" && pc.rt && pc.rt->gen;
    if (pc.midiFirst)
      for (int t : out[i]) ++feeders[static_cast<size_t>(t)];
  }
  // Delivery order (Kahn): a channel comes after every MIDI Out channel that feeds it.
  std::vector<int> ready;
  for (size_t i = 0; i < n; ++i) {
    prog.channels[i].midiFed = feeders[i] > 0;
    if (feeders[i] == 0 && prog.channels[i].midiFirst) ready.push_back(static_cast<int>(i));
  }
  for (size_t k = 0; k < ready.size(); ++k) {
    const int c = ready[k];
    prog.midiOrder.push_back(c);
    if (!prog.channels[static_cast<size_t>(c)].midiFirst) continue;
    for (int t : out[static_cast<size_t>(c)])
      if (--feeders[static_cast<size_t>(t)] == 0) ready.push_back(t);
  }
}

}  // namespace

double swingWarp(double x, double swing, Tick stepTicks, int stepsPerBar) {
  if (!(swing > 0) || stepTicks <= 0 || stepsPerBar <= 0) return x;
  const double st = static_cast<double>(stepTicks), pair = 2.0 * st;
  const double bar = st * static_cast<double>(stepsPerBar);
  const double barStart = std::floor(x / bar) * bar;
  const double u = x - barStart;
  const double pairStart = std::floor(u / pair) * pair;
  if (pairStart + pair > bar + 1e-9) return x;  // odd steps per bar: the last step has no partner
  const double v = u - pairStart;
  const double mid = st * (1.0 + 0.5 * std::min(swing, 1.0));  // where the off-beat step starts
  const double w = v < st ? v * (mid / st) : mid + (v - st) * ((pair - mid) / st);
  return barStart + pairStart + w;
}

namespace {
// K-152: the song swing a pattern is compiled with (< 0: Song::swing). Swing automation compiles a
// pattern clip's passes with the swing the automation has where each pass starts.
thread_local double gSwingOverride = -1.0;
thread_local double gAccentOverride = -1.0;  // the same for the groove's accents (< 0: SongGroove::accent)
thread_local bool gLiveSwing = false;        // 0.7.9: SongParam::Swing driven live on the audio thread
thread_local bool gLiveAccent = false;       // 0.7.9: SongParam::GrooveAccent driven live on the audio thread
// K-255: the pass of a looping clip a pattern is compiled for (lanes that run on take their phase from it).
thread_local int64_t gLanePass = 0;
// K-152 (0.7.8): swing that moves inside a pass. The swing and groove accent (thousandths, the
// song's values before the channel's swing mix) of every cell of the pattern's groove grid, read
// where each cell starts in the song: cell k of full cycle j of the pattern is [j * cycle + k]
// (cycles counted bar after bar, the partial cycle at a bar's end left out: it stays straight).
struct SwingCells {
  int cycle = 2;
  std::vector<int> swing, accent;
};
thread_local const SwingCells* gSwingCells = nullptr;

// Where the groove puts a note: one warp for the whole pattern, or (gSwingCells) one per cycle.
struct PlaceWarp {
  groove::Warp one;
  std::vector<groove::Warp> per;
  double cyc = 0;
  int64_t full = 0;  // full cycles per bar
  bool active = false, accents = false;
  const groove::Warp& at(double x) const {
    x = std::max(0.0, x);
    const double bi = std::floor(x / one.bar);
    const int64_t c = std::min<int64_t>(full - 1, static_cast<int64_t>(std::floor((x - bi * one.bar) / cyc)));
    const int64_t j = std::clamp<int64_t>(static_cast<int64_t>(bi) * full + c, 0, static_cast<int64_t>(per.size()) - 1);
    return per[static_cast<size_t>(j)];
  }
  double map(double x) const { return per.empty() ? one.map(x) : at(x).map(x); }
  // The cell a note belongs to is the nearest cell start, which may open the next cycle.
  float velocityAt(double x) const { return per.empty() ? one.velocityAt(x) : at(x + 0.5 * one.cell).velocityAt(x); }
};

// The groove geometry of pattern `p`: ticks per cell and bar, cells per cycle, full cycles per bar
// (0: no groove grid).
struct CellGrid {
  double cell = 0, bar = 0;
  int cycle = 2;
  int64_t full = 0;
};
CellGrid cellGrid(const Song& song, const Pattern& p) {
  CellGrid g;
  const groove::Warp w = groove::makeWarp(song, &p, song.groove.shape, 1.0, 1.0);
  if (!(w.cell > 0) || !(w.bar > 0)) return g;
  g.cell = w.cell, g.bar = w.bar, g.cycle = w.cycle;
  g.full = static_cast<int64_t>(std::floor((w.bar + 1e-9) / (w.cell * static_cast<double>(w.cycle))));
  return g;
}
}  // namespace

std::vector<NoteEv> compileChannelNotes(const Song& song, const Pattern& p, const Channel& c) {
  std::vector<NoteEv> out;
  const ObjectId channel = c.id;
  Tick stepTicks = song.ticksPerStep(p);
  const size_t total = static_cast<size_t>(p.bars) * static_cast<size_t>(p.stepsPerBar);
  // J-31: per-channel step loop: the first `loop` steps (and the notes that
  // start in them) repeat through the pattern.
  size_t loop = 0;
  if (auto lt = p.stepLoop.find(channel); lt != p.stepLoop.end() && lt->second > 0 && static_cast<size_t>(lt->second) < total)
    loop = static_cast<size_t>(lt->second);
  // Song swing x the channel's share (J-138: steps and piano-roll notes alike).
  // 0.7.9: when an internal controller drives swing or groove accents live, keep straight positions
  // / unaccented velocities here and tag eligible notes for realtime warping in AudioEngine::dispatch.
  const double songSwing = gLiveSwing ? 0.0 : (gSwingOverride >= 0.0 ? gSwingOverride : static_cast<double>(song.swing));  // K-152
  const double swing = std::clamp(songSwing * static_cast<double>(c.swingMix), 0.0, 1.0);
  // K-151: the song's swing shape (the classic step swing unless a preset or the groove editor
  // changed it; that one places every step and note exactly as swingWarp does).
  PlaceWarp gw;
  const double songAccent = gLiveAccent ? 0.0 : (gAccentOverride >= 0.0 ? gAccentOverride : static_cast<double>(song.groove.accent));
  gw.one = groove::makeWarp(song, &p, song.groove.shape, swing, std::clamp(songAccent * static_cast<double>(c.swingMix), 0.0, 1.0));
  gw.active = gw.one.active, gw.accents = gw.one.accents;
  // K-152 (0.7.8): swing automation that moves inside the pass: each cycle of the groove plays with
  // the swing and accent the automation has where each of its cells starts.
  if (gSwingCells) {
    const CellGrid g = cellGrid(song, p);
    const SwingCells& sc = *gSwingCells;
    const size_t cy = static_cast<size_t>(g.cycle);
    const size_t cycles = cy > 0 ? sc.swing.size() / cy : 0;
    if (g.full > 0 && cy == static_cast<size_t>(sc.cycle) && cycles > 0 && sc.accent.size() == sc.swing.size()) {
      const double mix = static_cast<double>(c.swingMix);
      gw.cyc = g.cell * static_cast<double>(g.cycle), gw.full = g.full;
      gw.per.reserve(cycles);
      gw.active = gw.accents = false;
      double am[groove::kMaxCycle], ac[groove::kMaxCycle];
      for (size_t j = 0; j < cycles; ++j) {
        for (size_t k = 0; k < cy; ++k) {
          am[k] = std::clamp(sc.swing[j * cy + k] / 1000.0 * mix, 0.0, 1.0);
          ac[k] = std::clamp(sc.accent[j * cy + k] / 1000.0 * mix, 0.0, 1.0);
        }
        const double next = std::clamp(sc.swing[(j + 1 < cycles ? j + 1 : j) * cy] / 1000.0 * mix, 0.0, 1.0);
        gw.per.push_back(groove::makeCycleWarp(song, &p, song.groove.shape, am, ac, next));
        gw.active = gw.active || gw.per.back().active;
        gw.accents = gw.accents || gw.per.back().accents;
      }
    }
  }
  auto warpAt = [&gw](double x) { return gw.active ? std::max(0.0, gw.map(x)) : x; };  // (K-152: one warp, or one per cycle)
  auto accent = [&gw](NoteEv& e, double written) {
    if (gw.accents) e.velocity = std::clamp(e.velocity * gw.velocityAt(written), 0.0f, 1.0f);
  };
  // K-255: a lane of its own (a grid of its own, or running on) plays through core/Lanes.h; every
  // other row keeps the 0.7.7 code below, so songs without lanes render exactly as before.
  const bool laneWay = !lanes::plain(p, channel);
  const bool ownGrid = lanes::ownGrid(p, channel);
  if (auto it = p.steps.find(channel); it != p.steps.end() && (stepTicks > 0 || ownGrid)) {
    const auto& row = it->second;
    // J-11: per-step pitch, properties and shift (graph / keyboard editor).
    const std::vector<StepProps>* props = nullptr;
    if (auto pt = p.stepProps.find(channel); pt != p.stepProps.end()) props = &pt->second;
    // K-255: the row's own steps ([s] the step in the pass, [src] the row step it plays).
    std::vector<lanes::StepHit> hits;
    if (laneWay) hits = lanes::playedSteps(song, p, channel, gLanePass);
    const double laneStep = ownGrid ? lanes::stepTicks(song, p, channel) : 0.0;
    const size_t count = laneWay ? hits.size() : total;
    // J-31 / J-138: a swung step keeps its length (or, truncated, still ends where the
    // unswung step ends); its start follows the swing warp, to a fraction of a tick.
    for (size_t k = 0; k < count; ++k) {
      const size_t s = laneWay ? static_cast<size_t>(hits[k].index) : k;
      const size_t src = laneWay ? static_cast<size_t>(hits[k].src) : loop ? s % loop : s;
      if (src >= row.size() || row[src] == 0) continue;
      if (ownGrid) {  // K-255: exact place on the row's grid (no swing: it belongs to the pattern's steps)
        const double at = hits[k].on;
        const StepProps sp = props && src < props->size() ? (*props)[src] : StepProps{};
        const double on = at + laneStep * static_cast<double>(sp.shift) / static_cast<double>(kStepShiftMax);
        NoteEv e = makeEv(0, 0, static_cast<float>(sp.pitch < 0 ? 60 : sp.pitch) + static_cast<float>(sp.fine) / 100.0f,
                          static_cast<float>(row[src]) / 127.0f);
        setOn(e, on);
        setOff(e, on + laneStep);
        if (!sp.isDefault()) {
          e.props.pan = static_cast<float>(sp.pan) / static_cast<float>(kNotePanMax);
          e.props.release = sp.release == kNoteReleaseDefault ? 0.5f : static_cast<float>(sp.release) / 127.0f;
          e.props.modX = sp.modX == kNoteModCentre ? 0.5f : static_cast<float>(sp.modX) / static_cast<float>(kNoteModMax + 1);
          e.props.modY = sp.modY == kNoteModCentre ? 0.5f : static_cast<float>(sp.modY) / static_cast<float>(kNoteModMax + 1);
          e.props.detune = static_cast<float>(sp.fine) / 100.0f;
        }
        accent(e, at);
        if (gLiveAccent) e.flags |= kNoteSwingAccent;
        out.push_back(e);
        continue;
      }
      const Tick grid = static_cast<Tick>(s) * stepTicks;
      double on = warpAt(static_cast<double>(grid));
      const double gridEnd = static_cast<double>(grid + stepTicks);
      auto stepEnd = [&](double start) {
        return c.swingTruncate ? std::max(start + 1.0, gridEnd) : start + static_cast<double>(stepTicks);
      };
      if (!props || src >= props->size() || (*props)[src].isDefault()) {
        NoteEv e = makeEv(0, 0, 60.0f, static_cast<float>(row[src]) / 127.0f);
        setOn(e, on);
        setOff(e, stepEnd(on));
        accent(e, static_cast<double>(grid));
        if (gLiveSwing) e.flags |= kNoteSwingTiming;
        if (gLiveAccent) e.flags |= kNoteSwingAccent;
        out.push_back(e);
        continue;
      }
      const StepProps& sp = (*props)[src];
      const Tick shift = stepTicks * sp.shift / kStepShiftMax;
      const double end = stepEnd(on);
      on += static_cast<double>(shift);
      NoteEv e = makeEv(0, 0, static_cast<float>(sp.pitch < 0 ? 60 : sp.pitch) + static_cast<float>(sp.fine) / 100.0f,
                        static_cast<float>(row[src]) / 127.0f);
      setOn(e, on);
      setOff(e, c.swingTruncate ? std::max(on + 1.0, end) : end + static_cast<double>(shift));
      e.props.pan = static_cast<float>(sp.pan) / static_cast<float>(kNotePanMax);
      e.props.release = sp.release == kNoteReleaseDefault ? 0.5f : static_cast<float>(sp.release) / 127.0f;
      e.props.modX = sp.modX == kNoteModCentre ? 0.5f : static_cast<float>(sp.modX) / static_cast<float>(kNoteModMax + 1);
      e.props.modY = sp.modY == kNoteModCentre ? 0.5f : static_cast<float>(sp.modY) / static_cast<float>(kNoteModMax + 1);
      e.props.detune = static_cast<float>(sp.fine) / 100.0f;  // J-180: the step keeps its key
      accent(e, static_cast<double>(grid));
      if (gLiveSwing) e.flags |= kNoteSwingTiming;
      if (gLiveAccent) e.flags |= kNoteSwingAccent;
      out.push_back(e);
    }
  }
  const Tick loopTicks = static_cast<Tick>(loop) * stepTicks;
  const Tick patLen = static_cast<Tick>(total) * stepTicks;
  std::shared_ptr<const std::vector<BendPoint>> bendShared;  // K-25: one copy per note (step loop repeats share it)
  const Note* bendOf = nullptr;
  // One played copy of note `n`, `rep` ticks later (a step loop's repeat).
  auto emitNote = [&](const Note& n, Tick rep) {
      NoteEv e = makeEv(n.start + rep, n.start + rep + n.length, static_cast<float>(n.pitch) + static_cast<float>(n.fine) / 100.0f,
                        static_cast<float>(n.velocity) / 127.0f);
      // K-22: a step note one step long plays exactly as the rack step it was: its step's swing
      // (whatever swingNotes says), its offset inside the step, one step long or truncated.
      const bool asStep = n.step && stepTicks > 0 && n.length == stepTicks;
      if (asStep) {
        const Tick at = n.start + rep;
        const Tick grid = at / stepTicks * stepTicks;
        const double warped = warpAt(static_cast<double>(grid));
        const double offset = static_cast<double>(at - grid) + n.frac;
        const double on = warped + offset;
        const double end = c.swingTruncate ? std::max(warped + 1.0, static_cast<double>(grid + stepTicks)) : warped + static_cast<double>(stepTicks);
        setOn(e, on);
        setOff(e, c.swingTruncate ? std::max(on + 1.0, end) : end + offset);
      } else if (n.frac != 0.f || n.endFrac != 0.f) {  // J-159: a sub-tick start / end
        const double t0 = static_cast<double>(n.start + rep) + n.frac;
        setOn(e, t0);
        setOff(e, std::max(t0 + 1e-3, static_cast<double>(n.start + rep + n.length) + n.endFrac));
      }
      if (asStep || song.swingNotes) accent(e, static_cast<double>(n.start + rep) + n.frac);  // K-151: the groove's accents
      if (!asStep && gw.active && stepTicks > 0 && song.swingNotes) {  // J-138: notes swing with the steps (written positions stay straight)
        const double on = warpAt(noteOnTick(e));
        const double end = warpAt(noteOffTick(e));
        // Truncated: the note ends where its warped end falls. Otherwise it is never shorter than written.
        const double minLen = std::min(1.0, static_cast<double>(n.length));
        setOn(e, on);
        setOff(e, c.swingTruncate ? std::max(end, on + minLen) : std::max(end, on + static_cast<double>(n.length)));
      }
      e.props.pan = static_cast<float>(n.pan) / static_cast<float>(kNotePanMax);
      e.props.release = static_cast<float>(n.release) / 127.0f;
      e.props.modX = static_cast<float>(n.modX) / static_cast<float>(kNoteModMax + 1);
      e.props.modY = static_cast<float>(n.modY) / static_cast<float>(kNoteModMax + 1);
      e.props.color = n.color;
      e.props.detune = static_cast<float>(n.fine) / 100.0f;  // J-180: key-mapped instruments keep the written key
      if (n.release == kNoteReleaseDefault) e.props.release = 0.5f;  // exact neutral values
      if (n.modX == kNoteModCentre) e.props.modX = 0.5f;
      if (n.modY == kNoteModCentre) e.props.modY = 0.5f;
      e.flags = static_cast<uint8_t>((n.slide ? kNoteSlide : 0) | (n.porta ? kNotePorta : 0));
      if (asStep || song.swingNotes) {
        if (gLiveSwing && stepTicks > 0) e.flags |= kNoteSwingTiming;
        if (gLiveAccent) e.flags |= kNoteSwingAccent;
      }
      e.program = static_cast<int16_t>(n.program);  // K-348
      if (!n.slide) e.vib = n.vib;  // K-262: the note's vibrato rides its pitch line
      if (!n.bend.empty() && !n.slide) {  // K-25: slide notes glide other notes, they have no line of their own
        if (!bendShared || bendOf != &n) bendShared = std::make_shared<const std::vector<BendPoint>>(n.bend), bendOf = &n;
        e.bend = bendShared;
      }
      if (!n.slide) {  // K-144: expression lanes; the note starts at their first values
        if (!n.pressure.empty()) {
          e.pressure = std::make_shared<const std::vector<BendPoint>>(n.pressure);
          e.props.pressure = static_cast<float>(std::clamp(bend::value(n.pressure, 0.0), 0.0, 1.0));
        }
        if (!n.timbre.empty()) {
          e.timbre = std::make_shared<const std::vector<BendPoint>>(n.timbre);
          e.props.timbre = static_cast<float>(std::clamp(0.5 + bend::value(n.timbre, 0.0), 0.0, 1.0));
        }
      }
      if (!n.tracker.empty()) {  // K-76
        e.tracker = std::make_shared<const std::vector<uint8_t>>(n.tracker);
        e.props.tracker = e.tracker->data();
        e.props.trackerLen = static_cast<uint32_t>(e.tracker->size());
      }
      out.push_back(e);
  };
  if (auto it = p.notes.find(channel); it != p.notes.end()) {
    if (!laneWay) {
      for (const auto& n : it->second)
        for (Tick rep = 0;; rep += loopTicks) {  // one pass, or the step loop's repeats (J-31)
          if (n.muted) break;  // J-11: muted notes stay in the pattern but do not play
          if (loopTicks > 0 && (n.start >= loopTicks || n.start + rep >= patLen)) break;
          emitNote(n, rep);
          if (loopTicks <= 0) break;
        }
    } else {  // K-255: the lane's repeats (a fraction of a tick apart on an odd grid; phased when it runs on)
      for (const lanes::NoteRepeat& r : lanes::playedNotes(song, p, channel, gLanePass)) {
        if (r.note->muted) continue;
        const double whole = std::floor(r.offset);
        emitNote(*r.note, static_cast<Tick>(whole));
        if (const double fr = r.offset - whole; fr > 0) {
          NoteEv& e = out.back();
          setOn(e, noteOnTick(e) + fr);
          setOff(e, noteOffTick(e) + fr);
        }
      }
    }
  }
  // Echo: repeats of every played note.
  if (c.echo.active()) {
    const size_t base = out.size();
    const double step = static_cast<double>(c.echo.time) * song.ppq;
    for (size_t i = 0; i < base; ++i) {
      if (out[i].flags & kNoteSlide) continue;
      float vel = out[i].velocity;
      for (int k = 1; k <= c.echo.count; ++k) {
        vel *= c.echo.feedback;
        if (vel < 0.005f) break;
        NoteEv e = out[i];
        Tick d = static_cast<Tick>(std::llround(step * k));
        e.on += d;
        e.off += d;
        e.velocity = vel;
        e.pitch += static_cast<float>(c.echo.pitch) * static_cast<float>(k) / 100.0f;
        e.props.detune += static_cast<float>(c.echo.pitch) * static_cast<float>(k) / 100.0f;  // J-180: echoes keep the key
        float pan = c.echo.pingPong && k % 2 == 0 ? -c.echo.pan : c.echo.pan;
        e.props.pan = std::clamp(e.props.pan + pan, -1.0f, 1.0f);
        e.props.modX = std::clamp(e.props.modX + c.echo.modX * 0.5f * static_cast<float>(k), 0.0f, 1.0f);
        e.props.modY = std::clamp(e.props.modY + c.echo.modY * 0.5f * static_cast<float>(k), 0.0f, 1.0f);
        e.flags = 0;
        out.push_back(e);
      }
    }
  }
  // Time shift: the whole channel starts later.
  if (c.timeShift > 0) {
    Tick d = static_cast<Tick>(std::llround(static_cast<double>(c.timeShift) * song.ppq));
    for (auto& e : out) e.on += d, e.off += d;
  }
  sortEvents(out);
  if (song.slidesVelocity) markSlideVelRefs(out, song.slidesAnyColour);
  return out;
}

std::vector<NoteEv> compilePatternChannel(const Song& song, const Pattern& p, ObjectId channel) {
  const Channel* c = song.channel(channel);
  if (!c) return {};
  std::vector<NoteEv> out = compileChannelNotes(song, p, *c);
  size_t own = out.size();
  addLayerNotes(song, p, *c, 0, out);
  if (out.size() != own) sortEvents(out);
  if (!c->fullKeyRange()) {  // J-31: the channel's key range (layer splits)
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const NoteEv& e) {
                               const int k = noteKey(e.pitch, e.props);  // J-180: the written key (fine pitch apart)
                               return k < c->keyLo || k > c->keyHi;
                             }),
              out.end());
  }
  return out;
}

void prepareChannelAux(const PluginState& state, ChannelAux& aux) {
  if (packs::prepareAux(state, aux)) return;  // pack instrument: aux prepared by its pack
  // Built-in sample instruments (sampler, drum pads with velocity layers, slicer).
  prepareBuiltinAux(state, aux);
}

std::shared_ptr<const Program> ProgramBuilder::build(const Song& song) {
  // J-30: asynchronous builds (the app) never wait for sample decoding: the
  // samples are queued for the background decoders and play once a later
  // build finds them decoded.
  std::optional<SampleCache::NonBlocking> nonBlocking;
  if (asyncSamples_) nonBlocking.emplace();
  // J-87: interpolation tables built here, never on the audio thread.
  stretch::sincTable(stretch::Quality::Realtime);
  stretch::sincTable(stretch::Quality::Offline);
  const LiveTargets live = liveTargets(song);
  {  // decode the channels' samples in parallel before they are prepared one by one (J-25)
    std::vector<std::string> paths;
    for (const auto& c : song.channels)
      if (const std::string p = c.plugin.data.substr(0, c.plugin.data.find('\n')); !p.empty() && isAudioFile(p)) paths.push_back(p);  // J-182: first line
    if (asyncSamples_)
      for (const auto& p : paths) sampleCache().request(p);
    else
      sampleCache().prefetch(paths);
  }
  auto prog = std::make_shared<Program>();
  prog->tempo = song.tempo;
  prog->masterPitch = song.masterPitch;
  prog->ppq = song.ppq;
  prog->slidesAnyColour = song.slidesAnyColour;  // K-5
  prog->slidesVelocity = song.slidesVelocity;    // GitHub #87
  prog->barTicks = song.barTicks();
  prog->beatTicks = song.beatTicks();
  prog->meter.build(song);
  if (!prog->meter.constant()) {  // K-193: plugins that read the meter get blocks split at its changes
    auto reads = [](const PluginState& s) {
      if (isHostedType(s.type)) return true;
      if (s.type != "gl.patcher" && s.type != "gl.patcherfx") return false;
      for (const char* k : {"vst3:", "clap:", "vst2:", "au:", "lv2:"})  // a patch that hosts a plugin
        if (s.data.find(k) != std::string::npos) return true;
      return false;
    };
    for (const auto& c : song.channels) prog->meterReaders = prog->meterReaders || reads(c.plugin);
    for (const auto& m : song.mixer)
      for (const auto& es : m.slots) prog->meterReaders = prog->meterReaders || reads(es.plugin);
  }
  prog->songEnd = song.lengthTicks();
  // K-415: Song mode end follows an explicit Project End marker when placed, otherwise the last clip
  // rounded up to whole bars. Offline export reads prog->songEnd and is unaffected.
  const Tick loopTarget = song.projectEnd > 0 ? song.projectEnd : prog->songEnd;
  Tick bar = std::max<Tick>(1, prog->barTicks);
  prog->songLoopEnd = std::max(bar, (loopTarget + bar - 1) / bar * bar);
  if (!prog->meter.constant()) {  // K-179: rounded up to a bar line of the meter map
    const MeterMap& mm = prog->meter;
    const Tick b = mm.barStartAt(loopTarget);
    prog->songLoopEnd = std::max(mm.nextBar(0), b == loopTarget ? b : mm.nextBar(loopTarget));
  }
  {  // J-85: marker actions (FL's marker skip / pause / marker loop, Gridline's stop; FL's loop marker)
    std::vector<Tick> at;
    for (const auto& m : song.markers) at.push_back(m.pos);
    std::sort(at.begin(), at.end());
    auto nextAfter = [&](Tick t) -> Tick {
      auto it = std::upper_bound(at.begin(), at.end(), t);
      return it == at.end() ? -1 : *it;
    };
    for (const auto& m : song.markers) {
      ProgMarkerEvent e;
      e.tick = static_cast<double>(m.pos);
      switch (m.action) {
        case MarkerAction::Skip: {
          const Tick n = nextAfter(m.pos);
          if (n < 0) continue;
          e.kind = ProgMarkerEvent::Skip;
          e.target = static_cast<double>(n);
          break;
        }
        case MarkerAction::Pause: e.kind = ProgMarkerEvent::Pause; break;
        case MarkerAction::Stop: e.kind = ProgMarkerEvent::Stop; break;
        case MarkerAction::Loop: {  // repeats [marker, next marker) (or to the song end; as ops::markerLoopRange)
          Tick n = nextAfter(m.pos);
          if (n < 0) n = std::max(prog->songEnd, m.pos + bar);
          e.kind = ProgMarkerEvent::LoopBack;
          e.tick = static_cast<double>(n);
          e.from = e.target = static_cast<double>(m.pos);
          break;
        }
        case MarkerAction::SongLoop:
          if (m.pos < prog->songLoopEnd) prog->songLoopStart = std::max(prog->songLoopStart, static_cast<double>(m.pos));
          continue;
        default: continue;
      }
      if (m.pos <= 0 && e.kind != ProgMarkerEvent::LoopBack) continue;  // playback never reaches tick 0 from earlier
      prog->markerEvents.push_back(e);
    }
    std::stable_sort(prog->markerEvents.begin(), prog->markerEvents.end(),
                     [](const ProgMarkerEvent& a, const ProgMarkerEvent& b) { return a.tick < b.tick; });
  }

  // --- mixer ---
  std::set<ObjectId> soloSet;
  for (const auto& m : song.mixer)
    if (m.solo) soloSet.insert(m.id);
  // A soloed track keeps its sources and destinations audible.
  std::set<ObjectId> audible;
  if (!soloSet.empty()) {
    audible = soloSet;
    bool grew = true;
    while (grew) {
      grew = false;
      for (const auto& m : song.mixer)
        for (const auto& s : m.sends) {
          bool from = audible.count(m.id), to = audible.count(s.to);
          // downstream of a solo track, or upstream into one
          if (from && !to && soloSet.count(m.id)) grew |= audible.insert(s.to).second;
          const MixerTrack* dst = song.mixerTrack(s.to);
          const bool alone = dst && dst->soloAlone;  // J-31 alt solo: sources stay silent
          if (!from && to && soloSet.count(s.to) && !alone) grew |= audible.insert(m.id).second;
        }
    }
    audible.insert(song.masterId());
  }
  std::map<ObjectId, std::shared_ptr<MixerRuntime>> mixers;
  std::map<std::pair<ObjectId, int>, std::shared_ptr<EffectRuntime>> effects;
  const std::shared_ptr<const Program> prevProg = last_.lock();
  const std::map<std::pair<ObjectId, int>, std::shared_ptr<EffectRuntime>> matched = matchEffectRuntimes(song, effects_, effectByUid_, prevProg.get());
  prog->panLaw = static_cast<int>(song.panLaw);
  prog->rooms.clear();
  for (int r = 0; r < roomops::roomCount(song); ++r) {
    const StageSettings& st = roomops::roomAt(song, r);
    ProgRoom pr;
    pr.depthK = depth::strengthK(st.depthStrength);
    pr.front = st.frontQuiet;
    pr.preDelay = st.preDelay;
    pr.earlyOn = st.early;
    pr.on = st.hasRoom();
    const rooms::Room& rm = rooms::get(st.roomType);
    pr.halfW = rm.halfWidth, pr.depth = rm.depth, pr.wall = rm.wall;
    prog->rooms.push_back(pr);
  }
  double armedBytes = 0;  // K-123 (0.7.7): capture-while-armed rings so far
  for (const auto& m : song.mixer) {
    ProgMixer pm;
    pm.id = m.id;
    auto it = mixers_.find(m.id);
    pm.rt = it != mixers_.end() ? it->second : std::make_shared<MixerRuntime>();
    mixers[m.id] = pm.rt;
    pm.room = static_cast<size_t>(roomops::trackRoom(song, m));
    pm.roomW = m.id == song.masterId() ? 1.0f : roomops::roomAt(song, static_cast<int>(pm.room)).width;
    const ProgRoom& room = prog->roomOf(pm);
    double g = dsp::faderGain(m.volume, 0.8);
    // Stage distance (none at 0 or with no room: old projects render as before)
    if (room.on && m.depth > 0) g *= depth::gain(m.depth, room.depthK);
    const depth::PanGains pg = depth::panGains(prog->panLaw, g, roomPan(m.pan, pm.roomW), roomSep(m.stereoSep, pm.roomW));
    pm.gainL = static_cast<float>(pg.gl);
    pm.gainR = static_cast<float>(pg.gr);
    pm.xlr = static_cast<float>(pg.xlr);
    pm.xrl = static_cast<float>(pg.xrl);
    pm.depth = room.on ? m.depth : 0.0f;  // kept on the track, not heard without a room
    pm.focus = room.on ? m.focus : 0.0f;
    pm.volume = pm.modVolume = m.volume;
    pm.pan = pm.modPan = m.pan;
    pm.sep = pm.sepBase = m.stereoSep;
    for (int k = 0; k < kLayerMacros; ++k) pm.macroLive[k] = m.macros[static_cast<size_t>(k)].value;  // K-245
    pm.invert = m.invertPolarity;
    pm.swap = m.swapLR;
    pm.inFirst = m.input;
    pm.inStereo = m.inputStereo;
    pm.inGain = std::pow(10.0f, std::clamp(m.inputTrim, kInputTrimMin, kInputTrimMax) / 20.0f) * (m.inputInvert ? -1.0f : 1.0f);
    pm.inProcessed = m.input >= 0 && pm.inGain != 1.0f;
    if (pm.inProcessed && pm.rt->inL.size() < static_cast<size_t>(kEngineMaxBlock)) {
      pm.rt->inL.assign(static_cast<size_t>(kEngineMaxBlock), 0.f);
      pm.rt->inR.assign(static_cast<size_t>(kEngineMaxBlock), 0.f);
    }
    pm.monitor = m.monitor;
    pm.armed = m.records();
    pm.recPoint = m.recPoint;
    if (pm.armed && m.recPoint == static_cast<uint8_t>(RecordPoint::PostEffects)) pm.hasPreSend = true;  // J-31: keeps the pre-fader copy
    if (pm.armed && !pm.rt->rec) pm.rt->rec = std::make_shared<packs::edison::CaptureRing>();
    // K-123 (0.7.7): capture while armed, for tracks recording their input.
    if (pm.armed && m.recPoint == static_cast<uint8_t>(RecordPoint::Input) && m.input >= 0) {
      const double full = std::max(0.0, armedSeconds_) * std::max(1.0, sampleRate_);
      const bool fits = armedBytes + full * 8.0 <= kArmedCaptureBudget;
      const auto want = static_cast<size_t>(fits ? full : 0.0);
      if (fits) armedBytes += full * 8.0;
      if (!pm.rt->armRing || pm.rt->armRing->capacity() != ArmedRing::capacityFor(want)) {
        pm.rt->armRing = std::make_shared<ArmedRing>(want);
      }
      pm.armRing = pm.rt->armRing;
    } else {
      pm.rt->armRing.reset();
    }
    pm.eq.gain = m.eqGain;
    pm.eq.freq = m.eqFreq;
    pm.eq.q = m.eqQ;
    pm.disabled = m.disabled;
    pm.threaded = m.threaded;
    pm.latencyManual = std::clamp(m.latencyOffset, 0, kMaxManualLatency);
    if (auto fz = frozenMixers_.find(m.id); fz != frozenMixers_.end() && fz->second && m.freeze.latency > 0 && m.freeze.sampleRate > 0)
      // K-87: the render plays as late as its effects did, so the track is compensated as before.
      pm.latencyManual = std::clamp(m.latencyOffset + static_cast<int>(std::lround(static_cast<double>(m.freeze.latency) * sampleRate_ /
                                                                                     m.freeze.sampleRate)),
                                    0, kMaxManualLatency);
    pm.soloSilent = !audible.empty() && !audible.count(m.id);
    pm.silent = m.muted || pm.soloSilent;
    pm.muteMod = m.muted;
    for (size_t s = 0; s < m.slots.size(); ++s) {
      const EffectSlot& es = m.slots[s];
      ProgSlot ps;
      ps.plugin = es.plugin;
      ps.enabled = es.enabled && !m.disabled;  // a disabled track runs none of its effects
      ps.mix = es.mix;
      if (!es.plugin.empty()) {
        auto key = std::make_pair(m.id, static_cast<int>(s));
        auto e = matched.find(key);  // 0.7.8: the runtime that follows this effect (matchEffectRuntimes)
        ps.rt = e != matched.end() ? e->second : newEffectRuntime(es.plugin.type);
        if (ps.rt) {
          effects[key] = ps.rt;
          prepareForHost(es.plugin, live, m.id, slotParam(m.id, static_cast<int>(s), 0).index,
                         [&](const PluginState& st) { ps.rt->fx->prepareHost(st, sampleRate_); });
        }
        packs::prepareAux(es.plugin, ps.aux);
        if (ps.rt && warmUp_) warmUp(*ps.rt, es.plugin, ps.aux, sampleRate_);
        ps.controller = hasOutputs(es.plugin.type);
        ps.restFrames = static_cast<int>(restSeconds(es.plugin) * sampleRate_);
        ps.builtinRest = restsWhenStopped(es.plugin.type);  // GitHub #66
        // K-142 (0.7.8): every built-in effect too; one not in that list rests only while it says
        // restsWhenFaint(), which the engine asks every block (the answer may change as it plays).
        ps.hostedRest = isHostedType(es.plugin.type) || ps.builtinRest || (ps.rt && ps.rt->fx);
        if (const PluginInfo* pi = findPlugin(es.plugin.type); pi && pi->sideInParam >= 0) {  // J-45 side input
          const int v = paramInt(es.plugin, static_cast<size_t>(pi->sideInParam), -1);
          if (v >= 0) {
            ps.sideOwn = true;
            ps.sideSrc = flRouteInput(song, static_cast<int>(prog->mixer.size()), v);
          }
        }
      }
      pm.slots.push_back(std::move(ps));
    }
    for (size_t s = 0; s < pm.slots.size(); ++s)  // K-142 (follow-up)
      if (pm.slots[s].rt) pm.filledSlots.push_back(static_cast<uint16_t>(s));
    prog->mixer.push_back(std::move(pm));
  }
  std::map<ObjectId, std::shared_ptr<depth::RoomTap>> roomTaps;
  std::map<ObjectId, std::shared_ptr<depth::RoomEarly>> roomEarly;
  for (size_t i = 0; i < song.mixer.size(); ++i)
    for (const auto& s : song.mixer[i].sends) {
      int to = song.mixerIndex(s.to);
      if (to < 0) continue;
      if (s.sidechain) {
        prog->mixer[i].sidechain.push_back({to, s.level});
        prog->mixer[i].sidechainPre.push_back(s.preFader ? 1 : 0);
        prog->mixer[static_cast<size_t>(to)].hasSidechain = true;
      } else {
        prog->mixer[i].sends.push_back({to, s.level});
        prog->mixer[i].sendPre.push_back(s.preFader ? 1 : 0);
        prog->mixer[i].sendRoom.push_back(s.stageRoom ? 1 : 0);
        const ProgRoom& sroom = prog->roomOf(prog->mixer[i]);
        if (s.stageRoom && sroom.on && (sroom.preDelay || sroom.earlyOn)) {  // the room tap (pre-delay, reflections)
          const ObjectId src = song.mixer[i].id, dst = s.to;
          auto t = roomTaps_.find(src);
          prog->mixer[i].roomTap = roomTaps.emplace(src, t != roomTaps_.end() ? t->second : std::make_shared<depth::RoomTap>()).first->second;
          if (sroom.earlyOn) {
            auto e = roomEarly_.find(dst);
            prog->mixer[static_cast<size_t>(to)].roomEarly =
                roomEarly.emplace(dst, e != roomEarly_.end() ? e->second : std::make_shared<depth::RoomEarly>()).first->second;
          }
        }
      }
      prog->mixer[i].hasPreSend = prog->mixer[i].hasPreSend || s.preFader;
    }
  // Slot sends and audio buses add ordering edges (source track before destination).
  {
    struct Ref {
      size_t mixer, slot;
      bool bus;
      int out = -1;  // PluginInfo::sendParams output, -1 = the slot's single send
    };
    std::vector<std::pair<int, int>> extra;
    std::vector<Ref> refs;
    std::vector<std::vector<size_t>> busSenders(kAudioBuses), busReceivers(kAudioBuses);
    for (size_t i = 0; i < prog->mixer.size(); ++i)
      for (size_t s = 0; s < prog->mixer[i].slots.size(); ++s) {
        const ProgSlot& ps = prog->mixer[i].slots[s];
        if (!ps.rt) continue;
        if (ps.plugin.type == kMixerSendType) {
          int to = sendDestination(song, findPlugin(ps.plugin.type), ps.plugin, static_cast<int>(i), 0, 0);
          if (to >= 0 && static_cast<size_t>(to) < prog->mixer.size()) {
            extra.push_back({static_cast<int>(i), to});
            refs.push_back({i, s, false});
          }
        } else if (const PluginInfo* pi = findPlugin(ps.plugin.type); pi && !pi->sendParams.empty()) {
          for (size_t k = 0; k < pi->sendParams.size() && k < static_cast<size_t>(kMaxSendOutputs); ++k) {
            int to = sendDestination(song, pi, ps.plugin, static_cast<int>(i), static_cast<size_t>(pi->sendParams[k]), -1);
            if (to >= 0 && static_cast<size_t>(to) < prog->mixer.size() && static_cast<size_t>(to) != i) {
              extra.push_back({static_cast<int>(i), to});
              refs.push_back({i, s, false, static_cast<int>(k)});
            }
          }
        } else if (ps.plugin.type == kBusSendType || ps.plugin.type == kBusReturnType) {
          int bus = std::clamp(paramInt(ps.plugin, 0, 0), 0, kAudioBuses - 1);
          (ps.plugin.type == kBusSendType ? busSenders : busReceivers)[static_cast<size_t>(bus)].push_back(i);
          if (ps.plugin.type == kBusSendType)
            prog->mixer[i].slots[s].busSend = bus;
          else
            prog->mixer[i].slots[s].busRecv = bus;
        }
      }
    size_t slotEdges = extra.size();
    auto byId = [&](size_t x, size_t y) { return song.mixer[x].id < song.mixer[y].id; };  // independent of the mixer's order
    for (auto& v : busSenders) std::stable_sort(v.begin(), v.end(), byId);
    for (auto& v : busReceivers) std::stable_sort(v.begin(), v.end(), byId);
    for (int b = 0; b < kAudioBuses; ++b)
      for (size_t a : busSenders[static_cast<size_t>(b)])
        for (size_t r : busReceivers[static_cast<size_t>(b)])
          if (a != r) extra.push_back({static_cast<int>(a), static_cast<int>(r)});
    std::vector<bool> accepted;
    if (extra.empty()) {
      for (ObjectId id : mixerOrder(song)) prog->order.push_back(song.mixerIndex(id));
    } else {
      prog->order = orderWithEdges(song, extra, accepted);
      for (size_t e = 0; e < slotEdges; ++e)
        if (accepted[e]) {
          ProgSlot& sl = prog->mixer[refs[e].mixer].slots[refs[e].slot];
          (refs[e].out >= 0 ? sl.multiSend[refs[e].out] : sl.sendTo) = extra[e].second;
        }
    }
  }
  prog->master = std::max(0, song.mixerIndex(song.masterId()));
  if (prog->mixer.empty()) {  // degenerate song: a bare master
    ProgMixer pm;
    pm.rt = std::make_shared<MixerRuntime>();
    pm.gainL = pm.gainR = 1;
    prog->mixer.push_back(std::move(pm));
    prog->order = {0};
  }

  // --- channels ---
  std::map<ObjectId, std::shared_ptr<ChannelRuntime>> chans;
  std::map<std::pair<ObjectId, int>, std::shared_ptr<NoteFxRuntime>> noteFx;
  // J-87: channels that feed audio clips, channels with notes or steps.
  std::set<ObjectId> clipSources, noteChannels;
  for (const auto& cl : song.playlist)
    if (cl.kind == ClipKind::Audio) clipSources.insert(cl.source);
  for (const auto& p : song.patterns) {
    for (const auto& [id, v] : p.notes)
      if (!v.empty()) noteChannels.insert(id);
    for (const auto& [id, v] : p.steps)
      for (uint8_t x : v)
        if (x) noteChannels.insert(id);
  }
  for (const auto& c : song.channels) {
    ProgChannel pc;
    pc.id = c.id;
    auto it = channels_.find(c.id);
    if (it != channels_.end() && it->second->type == c.plugin.type && !(it->second->gen && it->second->gen->stale()) &&
        (it->second->gen || !installedNow(c.plugin.type))) {  // (a missing plugin installed since: made again)
      pc.rt = it->second;
    } else {
      pc.rt = newChannelRuntime(c.plugin.type);
    }
    chans[c.id] = pc.rt;
    pc.plugin = c.plugin;
    pc.restable = c.plugin.type == "gl.sampler" && c.plugin.ins.empty();
    if (pc.rt->gen && c.plugin.type == "gl.sampler")  // J-87: no stretch voices for clip-only channels
      pc.rt->gen->setClipOnly((c.audioClip || clipSources.count(c.id)) && !noteChannels.count(c.id));
    if (pc.rt->gen)
      prepareForHost(c.plugin, live, c.id, channelPluginParam(c.id, 0).index,
                     [&](const PluginState& st) { pc.rt->gen->prepareHost(st, sampleRate_); });
    prepareChannelAux(c.plugin, pc.aux);
    if (!c.plugin.ins.empty()) {  // channel instrument envelopes / filter (J-33)
      auto ins = std::make_shared<InsSettings>();
      if (parseIns(c.plugin.ins, *ins) && ins->active()) pc.aux.ins = ins;
    }
    if (pc.rt && warmUp_ && !pc.rt->warmed) {
      warmUp(*pc.rt, c.plugin, pc.aux, sampleRate_);
      if (pc.rt->gen && allocatesInFirstBlock(pc.rt->type)) pc.rt->configuredIn.store(prog.get());  // K-288: it took this aux (not shared yet)
    }
    pc.controller = hasOutputs(c.plugin.type);
    double g = c.muted ? 0.0 : dsp::faderGain(c.gain, 0.78), l, r;
    dsp::panGains(c.pan, l, r);
    pc.gainL = static_cast<float>(g * l);
    pc.gainR = static_cast<float>(g * r);
    pc.volume = pc.modVolume = c.gain;
    pc.pan = pc.modPan = c.pan;
    pc.muted = pc.modMuted = c.muted;
    pc.pitchCents = pc.pitchCentsLive = c.pitchCents;
    pc.transpose = pc.transposeLive = c.transpose;
    pc.hosted = isHostedType(c.plugin.type);
    pc.bendRange = c.bendRange;
    pc.finePitch = static_cast<float>(c.fine) / 100.0f;
    pc.swingMix = c.swingMix;
    pc.swingTruncate = c.swingTruncate;
    pc.masterPitch = song.masterPitch + static_cast<float>(c.fine) / 100.0f;
    pc.pitchOffset = pc.pitchTarget = static_cast<float>(c.transpose) + (c.pitchCents + static_cast<float>(c.fine)) / 100.0f + song.masterPitch;
    int mi = song.mixerIndex(c.mixerTrack);
    pc.mixer = mi >= 0 ? mi : prog->master;
    for (ObjectId t : c.auxOuts) pc.auxMixer.push_back(t == kNoId ? -1 : song.mixerIndex(t));
    pc.polyphony = c.polyphony;
    pc.mono = c.mono;
    pc.porta = c.porta;
    pc.portaTime = c.portaTime;
    pc.expr = c.expr;
    pc.cutGroup = c.cutGroup;
    pc.cutBy = c.cutBy;
    pc.layerMode = c.layerMode;
    pc.keyLo = c.keyLo;
    pc.keyHi = c.keyHi;
    pc.sustain = pc.sustainLive = c.sustainPedal;
    if (c.plugin.type == "gl.midiout")  // J-165: the port its notes go out on (note triggers)
      pc.midiPort = std::clamp(static_cast<int>(std::lround(c.plugin.params.size() > static_cast<size_t>(pp::MoPort)
                                                                  ? c.plugin.params[static_cast<size_t>(pp::MoPort)]
                                                                  : 0.0f)),
                               0, MidiPortBus::kPorts - 1);
    for (ObjectId k : c.layer) {
      int ki = song.channelIndex(k);
      if (ki >= 0 && k != c.id) {
        pc.layer.push_back(ki);
        const LayerZone* z = c.layerZone(k);  // K-169
        pc.layerZones.push_back(z ? *z : LayerZone{});
        pc.layerSolo = pc.layerSolo || (z && z->solo);
      }
    }
    pc.layerRack = c.layerRack();
    for (int k = 0; k < kLayerMacros; ++k) pc.macroLive[k] = c.macros[static_cast<size_t>(k)].value;
    prog->hasCutGroups = prog->hasCutGroups || c.cutGroup > 0;
    for (size_t k = 0; k < c.noteFx.size(); ++k) {
      const PluginState& st = c.noteFx[k];
      ProgNoteFx nf;
      nf.state = st;
      auto key = std::make_pair(c.id, static_cast<int>(k));
      auto f = noteFx_.find(key);
      if (f != noteFx_.end() && f->second->type == st.type) {
        nf.rt = f->second;
      } else if (auto fx = createNoteFx(st.type)) {
        nf.rt = std::make_shared<NoteFxRuntime>();
        nf.rt->type = st.type;
        nf.rt->fx = std::move(fx);
      }
      if (nf.rt) noteFx[key] = nf.rt;
      std::vector<ObjectId> targets = noteFxTargets(st.data);
      for (size_t t = 0; t < targets.size() && t < static_cast<size_t>(NoteFxAux::kTargets); ++t)
        nf.aux.targets[t] = targets[t] == c.id ? -1 : song.channelIndex(targets[t]);
      prepareNoteFxAux(st, nf.aux);
      prog->hasNoteFx = prog->hasNoteFx || nf.rt != nullptr;
      pc.noteFx.push_back(std::move(nf));
    }
    if (auto fz = frozenChannels_.find(c.id); fz != frozenChannels_.end() && fz->second) {
      // K-87: the render holds the instrument, its notes and the channel pitch; the strip stays live.
      pc.frozen = fz->second;
      pc.restable = false;
      pc.pitchOffset = pc.pitchTarget = 0;
      pc.pitchCents = pc.pitchCentsLive = 0;
      pc.transpose = pc.transposeLive = 0;
      pc.finePitch = 0;
      pc.masterPitch = 0;
      pc.pitchMoves = false;
      pc.transposeMoves = false;
      if (pc.rt->gen) pc.rt->gen->setClipOnly(true);
    }
    pc.unloaded = unloadedChannels_.count(c.id) > 0;  // K-87
    pc.asleep = pc.unloaded && asleepChannels_.count(c.id) > 0;
    // K-277: Rest silent instruments (completed below, once links and MIDI routes are known).
    if (!c.keepRunning && pc.rt->gen && !pc.frozen && !pc.controller && !pc.aux.ins) {
      const Generator& g = *pc.rt->gen;
      bool extraOut = false;
      for (int k : pc.auxMixer) extraOut = extraOut || k >= 0;
      pc.restSilent = g.mayRest() && !g.wantsLanes() && !g.rawMidi() && !extraOut && c.layer.empty();
    }
    prog->channels.push_back(std::move(pc));
  }
  linkMidiPorts(song, *prog);
  // K-87: frozen mixer tracks: their render plays into them on a channel of its own, at unity.
  for (const auto& [mid, sample] : frozenMixers_) {
    const int mi = song.mixerIndex(mid);
    if (mi < 0 || !sample || song.channel(mid)) continue;
    ProgChannel pc;
    pc.id = mid;
    auto it = channels_.find(mid);
    pc.rt = it != channels_.end() && it->second->type == "gl.sampler" ? it->second : newChannelRuntime("gl.sampler");
    if (!pc.rt) continue;
    chans[mid] = pc.rt;
    pc.plugin = defaultPluginState("gl.sampler");
    pc.plugin.data.clear();
    if (pc.rt->gen) pc.rt->gen->setClipOnly(true);
    pc.gainL = pc.gainR = 1.0f;
    pc.volume = pc.modVolume = 0.78f;
    pc.mixer = mi;
    pc.frozen = sample;
    if (const MixerTrack& mt = song.mixer[static_cast<size_t>(mi)]; mt.freeze.latency > 0 && mt.freeze.sampleRate > 0)
      pc.frozenDelay = std::lround(static_cast<double>(mt.freeze.latency) * sampleRate_ / mt.freeze.sampleRate);
    prog->channels.push_back(std::move(pc));
  }

  // --- K-247: the board's LFOs (before the links: their settings are link targets) ---
  std::map<ObjectId, std::shared_ptr<LfoRuntime>> lfoRts;
  std::map<std::pair<ObjectId, uint64_t>, std::shared_ptr<LfoLinkRuntime>> lfoLinkRts;
  for (const SongLfo& l : song.lfos) {
    ProgLfo pf;
    pf.id = l.id;
    pf.on = l.on;
    pf.shape = l.shape;
    pf.pulseWidth = l.pulseWidth;
    pf.randomSteps = l.randomSteps;
    pf.seed = l.seed;
    if (l.shape == LfoShape::Drawn) {
      auto t = std::make_shared<lfo::DrawnTable>();
      lfo::buildTable(l, *t);
      pf.table = std::move(t);
    }
    pf.tempoSync = l.tempoSync;
    pf.rateKind = l.rateKind;
    pf.fadeIn = l.fadeIn;
    pf.unipolar = l.unipolar;
    pf.restart = l.restart;
    pf.noteChannel = song.channelIndex(l.noteChannel);
    pf.live[0] = l.depth, pf.live[1] = l.offset, pf.live[2] = l.phase, pf.live[3] = l.smoothMs;
    pf.live[4] = l.rateHz, pf.live[5] = static_cast<float>(l.rateNote), pf.live[6] = l.character;
    pf.kind = l.kind;
    pf.trigger = l.trigger;
    pf.centred = l.centred;
    pf.quantise = l.quantise;
    pf.roll = l.roll;
    if (l.kind == LfoKind::Random) {  // the trigger as the clock the pass runs
      if (l.trigger == RandomTrigger::Tempo) pf.tempoSync = true, pf.restart = LfoRestart::OnPlay;
      else if (l.trigger == RandomTrigger::Hz) pf.tempoSync = false, pf.restart = LfoRestart::Free;
      else pf.restart = l.trigger == RandomTrigger::Note ? LfoRestart::EachNote : LfoRestart::Free;
    } else if (l.kind == LfoKind::Lfo && pf.restart == LfoRestart::EachBar) {
      pf.restart = LfoRestart::OnPlay;  // (a bar restart is Chaos's)
    }
    auto it = lfos_.find(l.id);
    pf.rt = it != lfos_.end() ? it->second : std::make_shared<LfoRuntime>();
    lfoRts[l.id] = pf.rt;
    prog->lfos.push_back(std::move(pf));
  }

  // --- internal controller links ---
  std::map<uint64_t, std::shared_ptr<LinkRuntime>> linkRts;
  for (const auto& ml : song.modLinks) {
    if (!ml.enabled || ml.isLfo()) continue;  // (K-247: LFO links: the LFO pass below)
    ProgLink pl;
    if (ml.sourceSlot == kLayerMacroSlot) {  // K-169: a channel's macro; K-245: or a mixer track's
      pl.srcMacro = std::clamp(ml.sourceOutput, 0, kLayerMacros - 1);
      pl.srcChannel = song.channelIndex(ml.sourceOwner);
      if (pl.srcChannel < 0) pl.srcMixer = song.mixerIndex(ml.sourceOwner);
      if (pl.srcChannel < 0 && pl.srcMixer < 0) continue;
      if (pl.srcChannel >= 0 && static_cast<size_t>(pl.srcChannel) >= prog->channels.size()) continue;
      if (pl.srcMixer >= 0 && static_cast<size_t>(pl.srcMixer) >= prog->mixer.size()) continue;
    } else if (ml.sourceSlot < 0) {
      pl.srcChannel = song.channelIndex(ml.sourceOwner);
      if (pl.srcChannel < 0 || !prog->channels[static_cast<size_t>(pl.srcChannel)].controller) continue;
    } else {
      pl.srcMixer = song.mixerIndex(ml.sourceOwner);
      pl.srcSlot = ml.sourceSlot;
      if (pl.srcMixer < 0 || static_cast<size_t>(pl.srcSlot) >= prog->mixer[static_cast<size_t>(pl.srcMixer)].slots.size() ||
          !prog->mixer[static_cast<size_t>(pl.srcMixer)].slots[static_cast<size_t>(pl.srcSlot)].controller)
        continue;
    }
    pl.output = std::clamp(ml.sourceOutput, 0, kMaxControllerOutputs - 1);
    ParamId target{ml.owner, ml.index};
    if (!resolveTarget(song, *prog, target, pl)) continue;
    pl.min = ml.min;
    pl.max = ml.max;
    pl.smoothMs = ml.smoothMs;
    pl.curve = ml.curve;
    pl.invert = ml.invert;
    if (ml.curve == ModCurve::Formula && !pl.formula.compile(ml.formula.empty() ? "input" : ml.formula, {"input", "base", "t", "beat"}))
      pl.curve = ModCurve::Linear;  // invalid formula: plain mapping
    uint64_t key = target.packed();
    auto lr = links_.find(key);
    pl.rt = lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
    linkRts[key] = pl.rt;
    prog->links.push_back(std::move(pl));
  }
  compileLfoPass(song, *prog, linkRts, lfoLinkRts);
  lfos_ = std::move(lfoRts);
  lfoLinks_ = std::move(lfoLinkRts);
  // 0.7.9: initialise live song-parameter state (zero allocation on the audio thread).
  prog->modTempo = song.tempo;
  prog->modMasterPitch = song.masterPitch;
  prog->modSwing = song.swing;
  prog->modGrooveAccent = song.groove.accent;
  if (prog->masterPitchLinked) {
    for (ProgChannel& pc : prog->channels)
      if (!pc.frozen) pc.pitchMoves = true;
  }
  if (prog->swingLinked || prog->grooveAccentLinked) {
    prog->liveGroove = groove::makeWarp(song, nullptr, song.groove.shape, 1.0, 1.0);
  }
  // --- PH-09: host macros (plugin build) ---
  prog->macros.assign(std::min(macroTargets_.size(), static_cast<size_t>(kHostMacros)), ProgMacro{});
  for (size_t k = 0; k < macroTargets_.size() && k < static_cast<size_t>(kHostMacros); ++k) {
    const ParamId target = macroTargets_[k];
    ProgMacro& m = prog->macros[k];
    if (target.owner == kNoId || !resolveTarget(song, *prog, target, m.target)) continue;
    const uint64_t key = target.packed();
    auto lr = links_.find(key);
    m.target.rt = linkRts.count(key) ? linkRts[key] : lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
    linkRts[key] = m.target.rt;
    m.valid = true;
    prog->hasMacros = true;
  }
  // --- J-131: host parameter bank (single-plugin build) ---
  if (bankTarget_.owner != kNoId && resolveTarget(song, *prog, bankTarget_, prog->hostBank) &&
      (prog->hostBank.kind == ProgLink::ChanPlugin || prog->hostBank.kind == ProgLink::SlotParam)) {
    const PluginState* st = prog->hostBank.kind == ProgLink::ChanPlugin
                                ? &prog->channels[static_cast<size_t>(prog->hostBank.chan)].plugin
                                : &prog->mixer[static_cast<size_t>(prog->hostBank.mixer)].slots[static_cast<size_t>(prog->hostBank.slot)].plugin;
    prog->hostBankInfo = findPlugin(st->type);
    prog->hasHostBank = prog->hostBankInfo != nullptr;
  }
  // --- automation clips (arrangement): one lane per automated parameter ---
  // Tempo automation becomes the tempo map instead (song mode).
  prog->tempoMap.build(song);
  prog->flatMap.setConstant(song.tempo, song.ppq);
  // Compiles one lane (false: target not automatable, e.g. song parameters).
  auto compileLane = [&](AutomationLane&& lane, std::vector<ProgAutoLane>& out) {
    if (lane.owner == kSongId) return false;  // tempo: tempo map; swing, groove accents: pattern clips (K-152); master pitch: not automated
    ProgAutoLane al;
    ParamId target{lane.owner, lane.index};
    if (!resolveTarget(song, *prog, target, al.target)) return false;
    uint64_t key = target.packed();
    auto lr = links_.find(key);
    al.target.rt = linkRts.count(key) ? linkRts[key] : lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
    linkRts[key] = al.target.rt;
    al.lane = std::move(lane);
    al.events = al.lane.hasEvents();  // J-210
    prog->hasEventLanes |= al.events;
    out.push_back(std::move(al));
    return true;
  };
  // Pattern automation of placed patterns first: automation clips are applied
  // after them, so a clip wins where both drive a parameter.
  for (auto& lane : patternAutomationLanes(song)) compileLane(std::move(lane), prog->autos);
  for (auto& lane : automationLanes(song)) compileLane(std::move(lane), prog->autos);
  // Pattern mode: each pattern's own lanes (attached to ProgPattern below).
  std::map<ObjectId, std::vector<ProgAutoLane>> patternAutos;
  for (const auto& p : song.patterns)
    for (auto& lane : singlePatternLanes(p, std::max<Tick>(1, song.patternContentLength(p))))
      prog->hasPatternAutos |= compileLane(std::move(lane), patternAutos[p.id]);
  // Live mode: the clips of the live area (compiled further below; their
  // automation targets are resolved here so the live copies include them).
  std::vector<std::vector<ProgAutoLane>> liveAutos;
  std::vector<int> liveIdx;  // playlist index of each live clip
  for (size_t i = 0; i < song.playlist.size(); ++i) {
    const Clip& c = song.playlist[i];
    if (!inLiveArea(song, c) || c.muted) continue;
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    std::vector<ProgAutoLane> lanes;
    const Tick len = std::max<Tick>(1, song.clipLength(c));
    if (c.kind == ClipKind::Pattern) {
      if (const Pattern* p = song.pattern(c.pattern))
        for (auto& lane : singlePatternLanes(*p, len, c.offset, clipLoopWindow(song, c).len, clipLoopWindow(song, c).from))
          prog->hasPatternAutos |= compileLane(std::move(lane), lanes);
    } else if (c.kind == ClipKind::Automation) {
      if (const AutomationClip* a = song.automation(c.source); a && !a->points.empty() && a->owner != kNoId) {
        AutomationLane l;
        l.owner = a->owner;
        l.index = a->index;
        AutomationClip k = *a;
        if (!c.loop || c.loopLength > 0) k.loop = 0;  // K-114: only a placement that loops repeats the curve
        auto cv = std::make_shared<const AutomationClip>(std::move(k));
        if (c.loop && c.loopLength > 0)  // K-140: a cut clip repeats its part, pass by pass
          for (const ClipPass& ps : clipPasses(song, c)) l.inst.push_back({ps.at, ps.at + ps.len, ps.content, cv});
        else
          l.inst.push_back({0, len, c.offset, cv});
        prog->hasPatternAutos |= compileLane(std::move(l), lanes);
      }
    }
    liveAutos.push_back(std::move(lanes));
    liveIdx.push_back(static_cast<int>(i));
  }
  // Live copies for the states that links rewrite.
  for (auto& pc : prog->channels) {
    // K-277: an instrument whose MIDI output other channels play keeps running. K-142 (0.7.8): one
    // whose parameters links or automation move may rest while they hold still (a stopped
    // transport): a value that changes wakes it in that block (ProgChannel::liveMoved), so it
    // follows every change as it did when it never rested.
    if (!pc.midiTargets.empty()) pc.restSilent = false;
    if (pc.modded) pc.live = pc.plugin;
    for (auto& nf : pc.noteFx)
      if (nf.modded) nf.live = nf.state;
  }
  for (auto& pm : prog->mixer)
    for (auto& ps : pm.slots) {
      if (ps.modded) ps.live = ps.plugin;
      // K-64: an effect taking sound from elsewhere or passing it on keeps running.
      if (ps.controller || ps.sideOwn || ps.sendTo >= 0 || ps.busSend >= 0 || ps.busRecv >= 0) ps.restFrames = 0, ps.hostedRest = false;
      for (int k : ps.multiSend)
        if (k >= 0) ps.restFrames = 0, ps.hostedRest = false;
      // GitHub #66: what a hosted effect does with a side input is its own. K-142 (0.7.8): a built-in
      // one rests only while its side input is silent too (AudioEngine::runMixer).
      if (pm.hasSidechain && !ps.builtinRest) ps.hostedRest = false;
    }

  // --- patterns (pattern mode) and song events ---
  // 0.7.9: when an internal controller drives SongParam::Swing or SongParam::GrooveAccent live,
  // compile straight timing / unaccented velocities and tag eligible notes for audio-thread warping.
  gLiveSwing = prog->swingLinked;
  gLiveAccent = prog->grooveAccentLinked;
  struct LiveGrooveGuard {
    ~LiveGrooveGuard() { gLiveSwing = gLiveAccent = false; }
  } liveGrooveGuard;
  for (const auto& p : song.patterns) {
    ProgPattern pp;
    pp.id = p.id;
    pp.length = std::max<Tick>(1, song.patternContentLength(p));
    pp.bar = std::max<Tick>(1, song.patternBarTicks(p));
    pp.beat = std::max<Tick>(1, song.patternBeatTicks(p));
    pp.group = meterGroup(song.patternMeterNum(p), song.patternMeterDen(p));
    for (const auto& c : song.channels) pp.perChannel.push_back(compilePatternChannel(song, p, c.id));
    if (auto it = patternAutos.find(p.id); it != patternAutos.end()) pp.autos = std::move(it->second);
    prog->patterns.push_back(std::move(pp));
  }
  const std::vector<ClipFades> clipFades = resolveAllFades(song, prog->tempoMap);  // J-38
  bool deferred = false;  // J-196: some clip's render waits for the edit to settle
  // K-152: the song's swing and groove accent automation (song mode), if any: pattern clips follow
  // them pass by pass (skipped for a setting driven live by an internal controller).
  std::vector<AutomationLane> songLanes;
  const AutomationLane* swingLane = nullptr;
  const AutomationLane* accentLane = nullptr;
  for (const auto& am : song.automations)
    if (am.owner == kSongId && (am.index == static_cast<uint32_t>(SongParam::Swing) || am.index == static_cast<uint32_t>(SongParam::GrooveAccent))) {
      songLanes = automationLanes(song);
      for (const auto& l : songLanes) {
        if (!prog->swingLinked && l.targets(kSongId, static_cast<uint32_t>(SongParam::Swing))) swingLane = &l;
        if (!prog->grooveAccentLinked && l.targets(kSongId, static_cast<uint32_t>(SongParam::GrooveAccent))) accentLane = &l;
      }
      break;
    }
  // Swing and accent in thousandths: the project's, and what a lane has at tick t.
  auto milli = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 1000.0)); };
  const int baseSwing = milli(static_cast<double>(song.swing)), baseAccent = milli(static_cast<double>(song.groove.accent));
  std::map<std::tuple<ObjectId, int, int, int64_t>, std::vector<std::vector<NoteEv>>> swingVariants;
  // K-255: which pass of the clip's lanes that run on a pass plays (0: as the pattern itself).
  // Only a clip that loops over its whole pattern runs them on; a cut clip's own loop restarts them.
  auto lanePassOf = [&song](const Clip& clip, const ClipPass& ps) -> int64_t {
    if (ps.index <= 0) return 0;
    const Pattern* pat = song.pattern(clip.pattern);
    if (!pat || !lanes::anyRunOn(*pat)) return 0;
    const LoopWindow w = clipLoopWindow(song, clip);
    if (w.from != 0 || w.len != song.patternContentLength(*pat)) return 0;
    return ps.index % lanes::lineUpPasses(song, *pat, 4096);
  };
  // Events of a pattern compiled for swing / accent (thousandths; the project's: as it is) and a lane pass.
  auto variant = [&](ObjectId pattern, int qs, int qa, int64_t lp) -> const std::vector<std::vector<NoteEv>>* {
    auto& v = swingVariants[{pattern, qs, qa, lp}];
    if (v.empty())
      if (const Pattern* pat = song.pattern(pattern)) {
        if (qs != baseSwing || qa != baseAccent) gSwingOverride = qs / 1000.0, gAccentOverride = qa / 1000.0;
        gLanePass = lp;
        for (const auto& c : song.channels) v.push_back(compilePatternChannel(song, *pat, c.id));
        gSwingOverride = gAccentOverride = -1.0;
        gLanePass = 0;
      }
    return &v;
  };
  // K-152 (0.7.8): passes whose swing moves inside them, compiled per pattern, lane pass and cell values.
  std::map<std::pair<ObjectId, int64_t>, std::map<std::vector<int>, std::vector<std::vector<NoteEv>>>> movingVariants;
  std::unordered_map<ObjectId, CellGrid> cellGrids;
  SwingCells cells;
  std::unordered_set<ObjectId> mutedClips;  // K-64: they keep what they had (clipRenders_, clipVoices_)
  for (size_t clipIdx = 0; clipIdx < song.playlist.size(); ++clipIdx) {
    const Clip& clip = song.playlist[clipIdx];
    if (clip.track >= 0 && static_cast<size_t>(clip.track) < song.tracks.size() &&
        song.tracks[static_cast<size_t>(clip.track)].muted) {
      mutedClips.insert(clip.id);
      continue;
    }
    if (clip.muted) mutedClips.insert(clip.id);
    if (clip.muted || inLiveArea(song, clip)) continue;  // live area: launched by hand (below)
    if (clip.kind == ClipKind::Audio) {  // arrangement: audio clip through its channel
      int ci = song.channelIndex(clip.source);
      const ClipRenderCtx rctx{sampleRate_, asyncSamples_, deferRenders_, &deferred};
      if (ci < 0) continue;
      // K-114: a looping clip plays pass by pass (one window each).
      const std::vector<Clip> passes = clip.loop ? audioLoopPasses(song, clip, prog->tempoMap) : std::vector<Clip>{clip};
      for (size_t k = 0; k < passes.size(); ++k) {
        ProgAudioClip ac;
        ac.pass = static_cast<int>(k), ac.head = k == 0, ac.tail = k + 1 == passes.size();
        ac.track = clip.track;  // grid-1118
        if (compileAudioClip(song, song.channels[static_cast<size_t>(ci)], passes[k], prog->tempoMap, ac, &clipFades[clipIdx], &rctx))
          prog->channels[static_cast<size_t>(ci)].audio.push_back(std::move(ac));
      }
      continue;
    }
    if (clip.kind != ClipKind::Pattern) continue;
    const ProgPattern* pp = prog->pattern(clip.pattern);
    if (!pp) continue;
    // The clip shows pattern ticks [offset, offset + len); a looping
    // pattern repeats them pass by pass (Q-1, core/ClipLoop.h).
    const Tick len = song.clipLength(clip);
    const std::vector<ClipPass> passes = clipPasses(song, clip);
    const bool play = clipplay::active(clip);
    for (const ClipPass& ps : passes) {
      // K-152: swing automation: each cell of the groove grid in this pass plays with the swing (and
      // accent) the automation has where the cell starts; a pass the automation holds still is the
      // pattern compiled once for that value.
      const std::vector<std::vector<NoteEv>>* per = &pp->perChannel;
      const int64_t lp = lanePassOf(clip, ps);  // K-255
      int qs = baseSwing, qa = baseAccent;
      bool moving = false;
      if (swingLane || accentLane) {
        const double at = static_cast<double>(clip.start + ps.at);
        qs = swingLane ? milli(swingLane->valueAt(at)) : baseSwing, qa = accentLane ? milli(accentLane->valueAt(at)) : baseAccent;
        if (const Pattern* pat = song.pattern(clip.pattern)) {
          auto gi = cellGrids.find(pat->id);
          if (gi == cellGrids.end()) gi = cellGrids.emplace(pat->id, cellGrid(song, *pat)).first;
          const CellGrid& g = gi->second;
          if (g.full > 0) {
            // Cells of every bar the pattern's content covers, read inside this pass's window.
            const int64_t bars = static_cast<int64_t>(std::ceil(static_cast<double>(std::max<Tick>(1, song.patternContentLength(*pat))) / g.bar)) + 1;
            const double lo = static_cast<double>(ps.content), hi = std::max(lo, static_cast<double>(ps.content + ps.len) - 1e-3);
            const double cyc = g.cell * static_cast<double>(g.cycle);
            cells.cycle = g.cycle;
            cells.swing.clear(), cells.accent.clear();
            for (int64_t b = 0; b < bars; ++b)
              for (int64_t j = 0; j < g.full; ++j)
                for (int k = 0; k < g.cycle; ++k) {
                  const double pt = std::clamp(static_cast<double>(b) * g.bar + static_cast<double>(j) * cyc + static_cast<double>(k) * g.cell, lo, hi);
                  const double st = static_cast<double>(clip.start + ps.at) + (pt - lo);
                  cells.swing.push_back(swingLane ? milli(swingLane->valueAt(st)) : baseSwing);
                  cells.accent.push_back(accentLane ? milli(accentLane->valueAt(st)) : baseAccent);
                }
            for (size_t i = 0; i < cells.swing.size() && !moving; ++i) moving = cells.swing[i] != cells.swing[0] || cells.accent[i] != cells.accent[0];
            if (!moving && !cells.swing.empty()) qs = cells.swing[0], qa = cells.accent[0];
          }
        }
      }
      if (moving) {
        std::vector<int> key(cells.swing);
        key.insert(key.end(), cells.accent.begin(), cells.accent.end());
        auto& v = movingVariants[{clip.pattern, lp}][key];
        if (v.empty())
          if (const Pattern* pat = song.pattern(clip.pattern)) {
            gSwingCells = &cells;
            gLanePass = lp;
            for (const auto& c : song.channels) v.push_back(compilePatternChannel(song, *pat, c.id));
            gSwingCells = nullptr;
            gLanePass = 0;
          }
        if (v.size() == pp->perChannel.size()) per = &v;
      } else if (qs != baseSwing || qa != baseAccent || lp != 0) {
        const auto* v = variant(clip.pattern, qs, qa, lp);
        if (v->size() == pp->perChannel.size()) per = v;
      }
      for (size_t ci = 0; ci < per->size(); ++ci)
        for (const auto& e : (*per)[ci]) {
          if (e.on < ps.content || e.on >= ps.content + ps.len) continue;
          NoteEv x = e;
          x.on = clip.start + ps.at + (e.on - ps.content);
          x.off = clip.start + ps.at + (e.off - ps.content);
          if (play) clipplay::apply(clip, clip.start, x.on, x.off, x.onFrac, x.offFrac, x.pitch, x.velocity);  // K-112b3
          if (noteOffTick(x) > static_cast<double>(clip.start + len)) x.off = clip.start + len, x.offFrac = 0;
          x.track = static_cast<int16_t>(clip.track);  // grid-1118
          prog->channels[ci].song.push_back(x);
        }
    }
  }
  // --- live area: clips compiled for launching (content ticks from 0) ---
  for (size_t k = 0; k < liveIdx.size(); ++k) {
    const Clip& clip = song.playlist[static_cast<size_t>(liveIdx[k])];
    ProgLiveClip lc;
    lc.clip = clip.id;
    lc.track = clip.track;
    lc.length = std::max<Tick>(1, song.clipLength(clip));
    lc.once = clip.launchOnce;
    lc.pad = clip.launchSet.mode;  // grid2-1118: the clip's own launch settings
    lc.quant = clip.launchSet.quant;
    lc.legato = clip.launchSet.legato;
    lc.follow = clip.launchSet.follow;
    lc.scene = trackmodes::sceneOf(song, clip.start);
    lc.autos = std::move(liveAutos[k]);
    if (clip.kind == ClipKind::Pattern) {
      if (const ProgPattern* pp = prog->pattern(clip.pattern))
        for (size_t ci = 0; ci < pp->perChannel.size(); ++ci)
          for (const ClipPass& ps : clipPasses(song, clip))  // Q-1: repeats of a looping pattern
            for (const auto& e : [&]() -> const std::vector<NoteEv>& {  // K-255: lanes that run on, from the launch
                   const int64_t lp = lanePassOf(clip, ps);
                   if (lp == 0) return pp->perChannel[ci];
                   const auto* v = variant(clip.pattern, baseSwing, baseAccent, lp);
                   return ci < v->size() ? (*v)[ci] : pp->perChannel[ci];
                 }()) {
              if (e.on < ps.content || e.on >= ps.content + ps.len) continue;
              ProgLiveClip::Ev x;
              x.channel = static_cast<int>(ci);
              x.e = e;
              x.e.on = ps.at + (e.on - ps.content);
              x.e.off = ps.at + (e.off - ps.content);
              if (clipplay::active(clip)) clipplay::apply(clip, 0, x.e.on, x.e.off, x.e.onFrac, x.e.offFrac, x.e.pitch, x.e.velocity);  // K-112b3
              if (noteOffTick(x.e) > static_cast<double>(lc.length)) x.e.off = lc.length, x.e.offFrac = 0;
              lc.notes.push_back(x);
            }
      std::stable_sort(lc.notes.begin(), lc.notes.end(), [](const ProgLiveClip::Ev& a, const ProgLiveClip::Ev& b) { return noteOnTick(a.e) < noteOnTick(b.e); });
    } else if (clip.kind == ClipKind::Audio) {
      int ci = song.channelIndex(clip.source);
      Clip at0 = clip;
      at0.start = 0;
      const ClipRenderCtx rctx{sampleRate_, asyncSamples_, deferRenders_, &deferred};
      const std::vector<Clip> passes = at0.loop ? audioLoopPasses(song, at0, prog->flatMap) : std::vector<Clip>{at0};  // K-114
      for (size_t k = 0; ci >= 0 && k < passes.size(); ++k) {
        ProgAudioClip ac;
        ac.pass = static_cast<int>(k), ac.head = k == 0, ac.tail = k + 1 == passes.size();
        if (compileAudioClip(song, song.channels[static_cast<size_t>(ci)], passes[k], prog->flatMap, ac, nullptr, &rctx)) {
          lc.audioChannel = ci;
          lc.audio.push_back(std::move(ac));
        }
      }
    }
    int ti = prog->liveTrackIndex(clip.track);
    if (ti < 0) {
      ProgLiveTrack lt;
      lt.track = clip.track;
      if (clip.track >= 0 && static_cast<size_t>(clip.track) < song.tracks.size()) {
        const PlaylistTrack& t = song.tracks[static_cast<size_t>(clip.track)];
        lt.launch = t.launch;
        lt.pad = t.pad;
        lt.follow = t.follow;
        lt.syncStart = t.syncStart;
      }
      prog->liveTracks.push_back(lt);
      ti = static_cast<int>(prog->liveTracks.size()) - 1;
    }
    prog->liveTracks[static_cast<size_t>(ti)].clips.push_back(static_cast<int>(prog->liveClips.size()));
    prog->liveClips.push_back(std::move(lc));
  }
  for (auto& lt : prog->liveTracks)  // by playlist position
    std::stable_sort(lt.clips.begin(), lt.clips.end(), [&](int a, int b) {
      return song.clip(prog->liveClips[static_cast<size_t>(a)].clip)->start < song.clip(prog->liveClips[static_cast<size_t>(b)].clip)->start;
    });
  // grid2-1118: the Clips grid's scenes: each track's slot (its first clip; a deactivated one is no clip),
  // its stop button, and the scene's own tempo, time signature and follow actions.
  prog->liveOrigin = song.live.areaEnd;
  prog->originEpoch = originEpoch_;
  if (song.live.areaEnd > 0) {
    const int nTracks = static_cast<int>(std::min<size_t>(song.tracks.size(), static_cast<size_t>(kMaxPlaylistTracks)));
    const int nScenes = std::max(trackmodes::sceneCount(song), static_cast<int>(song.live.scenes.size()));
    prog->scenes.assign(static_cast<size_t>(std::max(0, nScenes)), ProgScene{});
    for (int k = 0; k < nScenes; ++k) {
      ProgScene& sc = prog->scenes[static_cast<size_t>(k)];
      sc.clips.assign(static_cast<size_t>(nTracks), kNoId);
      sc.stop.assign(static_cast<size_t>(nTracks), 1);
      for (int t = 0; t < nTracks; ++t) {
        sc.stop[static_cast<size_t>(t)] = trackmodes::hasStopButton(song, t, k) ? 1 : 0;
      }
      if (const SceneInfo* info = trackmodes::sceneInfo(song, k)) {
        sc.tempo = trackmodes::sceneTempo(*info);
        int n = 0, d = 0;
        if (trackmodes::sceneSignature(*info, n, d)) {
          sc.sigNum = n, sc.sigDen = d;
        }
        sc.follow = info->follow;
      }
    }
    for (const ProgLiveTrack& lt : prog->liveTracks) {
      for (int li : lt.clips) {
        const ProgLiveClip& lc = prog->liveClips[static_cast<size_t>(li)];
        if (lc.scene < 0 || lc.scene >= nScenes || lc.track < 0 || lc.track >= nTracks) {
          continue;
        }
        ObjectId& slot = prog->scenes[static_cast<size_t>(lc.scene)].clips[static_cast<size_t>(lc.track)];
        if (slot == kNoId) {  // (the first one in time: the one the slot shows)
          slot = lc.clip;
        }
      }
    }
  }
  for (auto& pc : prog->channels) {
    std::stable_sort(pc.song.begin(), pc.song.end(), [](const NoteEv& a, const NoteEv& b) { return noteOnTick(a) < noteOnTick(b); });
    std::stable_sort(pc.audio.begin(), pc.audio.end(), [](const ProgAudioClip& a, const ProgAudioClip& b) { return a.start < b.start; });
  }

  // J-153: the realtime stretch for the clips that may need it: while their
  // render is on its way (app builds) or when the channel pitch moves.
  // J-157: a Rubber Band voice (about 6 MB) only where it can play: while the
  // clip's render is still on its way, or when the channel pitch moves.
  // J-196: a deferred render (an edit in progress) plays through the realtime
  // stretch too. A clip keeps its voice from build to build while it plays the
  // same source (a pitch or level change: the stream goes on, gliding to the
  // new pitch; a new timing restarts the stream in the same voice): preparing
  // one costs milliseconds (Rubber Band: about 100 ms and megabytes), and a
  // new voice would restart the sound every frame of a knob turn.
  // While an edit goes on, a clip that needs a new voice gets Gridline's own
  // stretch (its render, started when the edit is over, plays Rubber Band),
  // at most a few new voices per build (voicesPending(): the caller builds
  // again for the rest; a clip waiting for one plays its plain read).
  constexpr int kNewVoicesWhileEditing = 4;
  int newVoices = 0;
  bool voicesPending = false;
  std::map<std::pair<ObjectId, int>, ClipVoiceSlot> voices;
  auto giveVoice = [&](ProgAudioClip& ac, const ProgChannel& pc, bool live) {
    if (tap::readsOnly()) {  // K-319: a Track Receipt pass that only measures plays no clip sound
      return;
    }
    const bool rubber = ac.engine == stretch::Engine::RubberBand;
    if (!((asyncSamples_ && ac.hq && !(rubber && ac.hq->ready.load(std::memory_order_acquire))) || pc.pitchMoves ||
          (ac.hqNeeded && !ac.hq)))
      return;
    ClipVoiceSlot slot;
    slot.sample = ac.sample.get();
    slot.engine = ac.engine;
    slot.reverse = ac.reverse;
    slot.start = ac.start;
    slot.srcStart = ac.srcStart, slot.srcEnd = ac.srcEnd, slot.speed = ac.speed, slot.secPerTick = ac.secPerTick;
    slot.resampleSync = ac.resampleSync;
    slot.warpKey = ac.warpKey;
    const auto key = std::make_pair(ac.clip, live ? -1 - ac.pass : ac.pass);
    auto old = clipVoices_.find(key);
    // A preview voice (the stretch standing in for Rubber Band) plays on
    // until the clip's render is ready, unless the channel pitch moves at run
    // time (then the clip keeps a voice of its own engine).
    const bool preview = !pc.pitchMoves && rubber && old != clipVoices_.end() && old->second.engine == stretch::Engine::Stretch;
    ClipVoiceSlot want = slot;
    if (preview) want.engine = stretch::Engine::Stretch;
    if (ac.clip != kNoId && old != clipVoices_.end() && old->second.same(want) && !voices.count(key)) {
      slot.voice = old->second.voice;  // (a clip that was muted: its gen moved on, so its stream restarts)
      slot.engine = want.engine;
      // A new timing (a time stretch knob, a trim, a move): the stream starts
      // again where the clip now is (the audio thread restarts it).
      slot.gen = old->second.gen + (old->second.inPlace(slot) ? 0u : 1u);
      ++voicesReused_;
    } else if (deferRenders_ && newVoices >= kNewVoicesWhileEditing) {
      voicesPending = true;
      return;
    } else {
      if (deferRenders_) slot.engine = stretch::Engine::Stretch, ++newVoices;
      // K-64: in realtime playback its steps are shared out over the blocks (an export has no deadline).
      slot.voice = makeClipVoice(sampleRate_, slot.engine, ac.sample ? ac.sample->sampleRate : 0.0, asyncSamples_);
      ++voicesMade_;
    }
    ac.rt = slot.voice;
    ac.rtGen = slot.gen;
    ac.engine = slot.engine;  // the voice's engine (J-170's priming is Rubber Band's)
    if (ac.clip != kNoId) voices[key] = std::move(slot);
  };
  for (auto& pc : prog->channels)
    for (auto& ac : pc.audio) giveVoice(ac, pc, false);
  for (auto& lc : prog->liveClips)
    if (lc.audioChannel >= 0 && static_cast<size_t>(lc.audioChannel) < prog->channels.size())
      for (auto& ac : lc.audio) giveVoice(ac, prog->channels[static_cast<size_t>(lc.audioChannel)], true);
  // While editing, voices a value made unnecessary for now (a pitch crossing
  // zero) are kept for when it comes back.
  if (deferRenders_)
    for (auto& kv : clipVoices_) voices.emplace(kv.first, std::move(kv.second));
  // K-64: the voices and renders of clips muted since stay for when they come back.
  for (auto& kv : clipVoices_)
    if (mutedClips.count(kv.first.first) && kv.second.voice) {
      ++kv.second.gen;
      voices.emplace(kv.first, std::move(kv.second));
    }
  clipVoices_ = std::move(voices);
  {
    std::unordered_map<ObjectId, stretch::RenderedClipPtr> renders;
    renders.reserve(clipRenders_.size());
    for (const auto& pc : prog->channels)
      for (const auto& ac : pc.audio)
        if (ac.hq && ac.clip != kNoId) renders.emplace(ac.clip, ac.hq);
    for (auto& kv : clipRenders_)
      if (mutedClips.count(kv.first)) renders.emplace(kv.first, std::move(kv.second));
    clipRenders_ = std::move(renders);
  }
  deferred_ = deferred;
  voicesPending_ = voicesPending;

  compileGraph(*prog);

  // Keep only live runtimes so deleted channels/tracks free their DSP state
  // once the last Program that references them is retired.
  channels_ = std::move(chans);
  mixers_ = std::move(mixers);
  roomTaps_ = std::move(roomTaps);
  roomEarly_ = std::move(roomEarly);
  effectByUid_.clear();  // 0.7.8 (matchEffectRuntimes)
  for (const MixerTrack& m : song.mixer)
    for (size_t k = 0; k < m.slots.size(); ++k)
      if (m.slots[k].uid)
        if (auto e = effects.find({m.id, static_cast<int>(k)}); e != effects.end()) effectByUid_[m.slots[k].uid] = e->second;
  effects_ = std::move(effects);
  noteFx_ = std::move(noteFx);
  links_ = std::move(linkRts);
  last_ = prog;
  ++builds_;
  return prog;
}

bool ProgramBuilder::ClipVoiceSlot::same(const ClipVoiceSlot& o) const {
  return sample == o.sample && engine == o.engine && reverse == o.reverse;
}

bool ProgramBuilder::ClipVoiceSlot::inPlace(const ClipVoiceSlot& o) const {
  // K-85: a moved warp marker keeps the stream (it glides onto the new time
  // map, AudioClips.cpp); warping or unwarping a clip restarts it.
  return start == o.start && srcStart == o.srcStart && srcEnd == o.srcEnd && speed == o.speed && secPerTick == o.secPerTick &&
         resampleSync == o.resampleSync && (warpKey == o.warpKey || (warpKey != 0 && o.warpKey != 0));
}

bool ProgramBuilder::patchClipLevels(const Song& song) {
  const std::shared_ptr<const Program> prog = last_.lock();
  if (!prog) return false;
  std::unordered_map<ObjectId, size_t> at;
  at.reserve(song.playlist.size());
  for (size_t i = 0; i < song.playlist.size(); ++i) at.emplace(song.playlist[i].id, i);
  const std::vector<ClipFades> fades = resolveAllFades(song, prog->tempoMap);  // (as build: crossfades follow)
  auto clipOf = [&](const ProgAudioClip& ac) -> int64_t {
    auto it = at.find(ac.clip);
    return it == at.end() ? -1 : static_cast<int64_t>(it->second);
  };
  for (const ProgChannel& pc : prog->channels)
    for (const ProgAudioClip& ac : pc.audio)
      if (clipOf(ac) < 0) return false;
  for (const ProgLiveClip& lc : prog->liveClips)
    for (const ProgAudioClip& ac : lc.audio)
      if (clipOf(ac) < 0) return false;
  for (const ProgChannel& pc : prog->channels)
    for (const ProgAudioClip& ac : pc.audio) {
      const size_t i = static_cast<size_t>(clipOf(ac));
      setClipLevels(ac, song.playlist[i], &fades[i]);
    }
  for (const ProgLiveClip& lc : prog->liveClips)
    for (const ProgAudioClip& ac : lc.audio) setClipLevels(ac, song.playlist[static_cast<size_t>(clipOf(ac))], nullptr);
  ++patches_;
  return true;
}

std::vector<ProgramBuilder::PreloadItem> ProgramBuilder::preloadItems(const Song& song) {
  std::vector<PreloadItem> out;
  const freeze::Plan plan = freeze::makePlan(song);  // K-87: frozen and hibernating units load nothing
  for (const auto& m : song.mixer)
    for (size_t s = 0; s < m.slots.size(); ++s)
      if (!m.slots[s].plugin.empty() && !plan.unloadMixers.count(m.id)) out.push_back({true, m.id, static_cast<int>(s), m.slots[s].plugin.type});
  for (const auto& c : song.channels)
    if (!c.plugin.type.empty() && !plan.unloadChannels.count(c.id)) out.push_back({false, c.id, -1, c.plugin.type});
  return out;
}

bool ProgramBuilder::preload(const Song& song, const PreloadItem& item) {
  // As build(): samples are never waited for here (the loader decodes them meanwhile).
  std::optional<SampleCache::NonBlocking> nonBlocking;
  if (asyncSamples_) nonBlocking.emplace();
  const LiveTargets live = liveTargets(song);
  if (item.effect) {
    const MixerTrack* m = song.mixerTrack(item.owner);
    if (!m || item.slot < 0 || static_cast<size_t>(item.slot) >= m->slots.size()) return false;
    const EffectSlot& es = m->slots[static_cast<size_t>(item.slot)];
    if (es.plugin.empty()) return false;
    std::shared_ptr<EffectRuntime> rt = newEffectRuntime(es.plugin.type);
    if (!rt) return false;
    prepareForHost(es.plugin, live, m->id, slotParam(m->id, item.slot, 0).index,
                   [&](const PluginState& st) { rt->fx->prepareHost(st, sampleRate_); });
    effects_[{m->id, item.slot}] = std::move(rt);
    return true;
  }
  const Channel* c = song.channel(item.owner);
  if (!c) return false;
  std::shared_ptr<ChannelRuntime> rt = newChannelRuntime(c->plugin.type);
  if (rt->gen && c->plugin.type == "gl.sampler") {  // J-87: as build(), before prepareHost
    bool clipSource = c->audioClip, notes = false;
    for (const auto& cl : song.playlist) clipSource = clipSource || (cl.kind == ClipKind::Audio && cl.source == c->id);
    for (const auto& p : song.patterns) {
      if (auto n = p.notes.find(c->id); n != p.notes.end() && !n->second.empty()) notes = true;
      if (auto st = p.steps.find(c->id); st != p.steps.end())
        for (uint8_t x : st->second) notes = notes || x != 0;
    }
    rt->gen->setClipOnly(clipSource && !notes);
  }
  if (rt->gen)
    prepareForHost(c->plugin, live, c->id, channelPluginParam(c->id, 0).index,
                   [&](const PluginState& st) { rt->gen->prepareHost(st, sampleRate_); });
  const bool ok = rt->gen != nullptr;
  channels_[c->id] = std::move(rt);
  return ok;
}

void ProgramBuilder::adoptPreloaded(ProgramBuilder&& from) {
  for (auto& [id, rt] : from.channels_) channels_[id] = std::move(rt);
  for (auto& [key, rt] : from.effects_) effects_[key] = std::move(rt);
  from.channels_.clear();
  from.effects_.clear();
}

std::shared_ptr<ChannelRuntime> ProgramBuilder::channelRuntime(ObjectId id) const {
  auto it = channels_.find(id);
  return it == channels_.end() ? nullptr : it->second;
}
std::shared_ptr<MixerRuntime> ProgramBuilder::mixerRuntime(ObjectId id) const {
  auto it = mixers_.find(id);
  return it == mixers_.end() ? nullptr : it->second;
}
std::shared_ptr<EffectRuntime> ProgramBuilder::effectRuntime(ObjectId track, int slot) const {
  auto it = effects_.find({track, slot});
  return it == effects_.end() ? nullptr : it->second;
}
std::shared_ptr<NoteFxRuntime> ProgramBuilder::noteFxRuntime(ObjectId channel, int index) const {
  auto it = noteFx_.find({channel, index});
  return it == noteFx_.end() ? nullptr : it->second;
}

void ProgramBuilder::holdAutomation(ParamId pid, double normalized) {
  std::shared_ptr<LinkRuntime>& rt = links_[pid.packed()];
  if (!rt) rt = std::make_shared<LinkRuntime>();  // (the next build's lane takes it: compileLane)
  rt->heldValue.store(static_cast<float>(std::clamp(normalized, 0.0, 1.0)), std::memory_order_relaxed);
  rt->held.store(true, std::memory_order_relaxed);
  automationHoldChanges().fetch_add(1, std::memory_order_release);  // K-142 (0.7.8 close)
}

void ProgramBuilder::releaseAutomation(ParamId pid) {
  auto it = links_.find(pid.packed());
  if (it != links_.end() && it->second) it->second->held.store(false, std::memory_order_relaxed);
  automationHoldChanges().fetch_add(1, std::memory_order_release);  // K-142 (0.7.8 close)
}

void ProgramBuilder::releaseAllAutomation() {
  for (auto& [key, rt] : links_)
    if (rt) rt->held.store(false, std::memory_order_relaxed);
  automationHoldChanges().fetch_add(1, std::memory_order_release);  // K-142 (0.7.8 close)
}

bool ProgramBuilder::automationHeld(ParamId pid) const {
  auto it = links_.find(pid.packed());
  return it != links_.end() && it->second && it->second->held.load(std::memory_order_relaxed);
}

bool ProgramBuilder::modulatedValue(ParamId id, double& value) const {
  auto it = links_.find(id.packed());
  if (it == links_.end() || !it->second->active.load(std::memory_order_relaxed)) return false;
  // K-247: under the board's LFOs, the value they move around (the dot shows where they took it).
  value = it->second->lfoMoved.load(std::memory_order_relaxed) ? it->second->baseDisplay.load(std::memory_order_relaxed)
                                                                 : it->second->display.load(std::memory_order_relaxed);
  return true;
}

bool ProgramBuilder::lfoValue(ParamId id, double& value) const {
  auto it = links_.find(id.packed());
  if (it == links_.end() || !it->second->lfoMoved.load(std::memory_order_relaxed)) return false;
  value = it->second->display.load(std::memory_order_relaxed);
  return true;
}

std::shared_ptr<LfoRuntime> ProgramBuilder::lfoRuntime(ObjectId lfo) const {
  auto it = lfos_.find(lfo);
  return it == lfos_.end() ? nullptr : it->second;
}

void ProgramBuilder::compileLfoPass(const Song& song, Program& prog, std::map<uint64_t, std::shared_ptr<LinkRuntime>>& linkRts,
                                    std::map<std::pair<ObjectId, uint64_t>, std::shared_ptr<LfoLinkRuntime>>& lfoLinkRts) {
  if (prog.lfos.empty()) return;
  // The LFO links, one group per target.
  std::map<uint64_t, size_t> groupOf;
  for (const auto& ml : song.modLinks) {
    if (!ml.enabled || !ml.isLfo()) continue;
    const int li = song.lfoIndex(ml.sourceOwner);
    if (li < 0) continue;
    const ParamId target{ml.owner, ml.index};
    const uint64_t key = target.packed();
    auto g = groupOf.find(key);
    if (g == groupOf.end()) {
      ProgLfoTarget t;
      if (!resolveTarget(song, prog, target, t.target)) continue;  // song parameters are not modulated
      auto lr = links_.find(key);
      t.target.rt = linkRts.count(key) ? linkRts[key] : lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
      linkRts[key] = t.target.rt;
      g = groupOf.emplace(key, prog.lfoTargets.size()).first;
      prog.lfoTargets.push_back(std::move(t));
    }
    ProgLfoLink k;
    k.lfo = li;
    k.amount = ml.amount;
    k.min = ml.min;
    k.max = ml.max;
    k.smoothMs = ml.smoothMs;
    k.mode = ml.mode;
    k.direction = ml.direction;
    k.invert = ml.invert;
    k.curve = ml.curve == ModCurve::Formula ? ModCurve::Linear : ml.curve;
    const auto rk = std::make_pair(ml.sourceOwner, key);
    auto old = lfoLinks_.find(rk);
    k.rt = old != lfoLinks_.end() ? old->second : std::make_shared<LfoLinkRuntime>();
    lfoLinkRts[rk] = k.rt;
    prog.lfoTargets[g->second].links.push_back(std::move(k));
  }
  // A macro's identity (channel or mixer track, macro) as a number; -1: not a macro.
  auto targetMacro = [](const ProgLink& t) -> int64_t {
    if (t.kind == ProgLink::ChanMacro) return (int64_t{t.chan} << 8) | t.param;
    if (t.kind == ProgLink::MixMacro) return (int64_t{1} << 40) | (int64_t{t.mixer} << 8) | t.param;
    return -1;
  };
  auto sourceMacro = [](const ProgLink& l) -> int64_t {
    if (l.srcMacro < 0) return -1;
    if (l.srcChannel >= 0) return (int64_t{l.srcChannel} << 8) | l.srcMacro;
    return (int64_t{1} << 40) | (int64_t{l.srcMixer} << 8) | l.srcMacro;
  };
  // Links whose source is a macro an LFO moves (directly, or through such links) run after it.
  std::set<int64_t> moved;
  for (const auto& t : prog.lfoTargets)
    if (int64_t m = targetMacro(t.target); m >= 0) moved.insert(m);
  for (bool grew = true; grew;) {
    grew = false;
    for (auto& l : prog.links) {
      if (l.late || !moved.count(sourceMacro(l))) continue;
      l.late = true;
      grew = true;
      if (int64_t m = targetMacro(l); m >= 0) moved.insert(m);
    }
  }
  // The pass's order: every LFO before the targets it moves, a target before the LFO whose setting
  // it is and before the links its macro drives (a topological order; what a loop leaves over runs
  // last, in the song's order: the document refuses loops, older files may still hold one).
  const int nl = static_cast<int>(prog.lfos.size()), nt = static_cast<int>(prog.lfoTargets.size());
  std::vector<int> late;
  for (int k = 0; k < static_cast<int>(prog.links.size()); ++k)
    if (prog.links[static_cast<size_t>(k)].late) late.push_back(k);
  const int n = nl + nt + static_cast<int>(late.size());
  std::vector<std::vector<int>> out(static_cast<size_t>(n));
  std::vector<int> indeg(static_cast<size_t>(n), 0);
  auto edge = [&](int a, int b) {
    out[static_cast<size_t>(a)].push_back(b);
    ++indeg[static_cast<size_t>(b)];
  };
  auto targetEdges = [&](int from, const ProgLink& t) {  // what follows a node that sets target t
    if (t.kind == ProgLink::LfoSetting) edge(from, t.fx);
    const int64_t m = targetMacro(t);
    for (size_t j = 0; j < late.size(); ++j) {
      const ProgLink& l = prog.links[static_cast<size_t>(late[j])];
      if (m >= 0 && sourceMacro(l) == m) edge(from, nl + nt + static_cast<int>(j));
    }
  };
  auto sameTarget = [](const ProgLink& a, const ProgLink& b) {
    return a.kind == b.kind && a.chan == b.chan && a.mixer == b.mixer && a.slot == b.slot && a.fx == b.fx && a.param == b.param;
  };
  for (int g = 0; g < nt; ++g) {
    for (const auto& k : prog.lfoTargets[static_cast<size_t>(g)].links) edge(k.lfo, nl + g);
    targetEdges(nl + g, prog.lfoTargets[static_cast<size_t>(g)].target);
  }
  for (size_t j = 0; j < late.size(); ++j) {
    const ProgLink& l = prog.links[static_cast<size_t>(late[j])];
    const int node = nl + nt + static_cast<int>(j);
    targetEdges(node, l);
    for (int g = 0; g < nt; ++g)  // the LFOs move around what the link set
      if (sameTarget(l, prog.lfoTargets[static_cast<size_t>(g)].target)) edge(node, nl + g);
  }
  std::vector<int> order, ready;
  std::vector<char> done(static_cast<size_t>(n), 0);
  for (int i = 0; i < n; ++i)
    if (indeg[static_cast<size_t>(i)] == 0) ready.push_back(i);
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), std::greater<int>());
    const int i = ready.back();
    ready.pop_back();
    order.push_back(i);
    done[static_cast<size_t>(i)] = 1;
    for (int b : out[static_cast<size_t>(i)])
      if (--indeg[static_cast<size_t>(b)] == 0) ready.push_back(b);
  }
  for (int i = 0; i < n; ++i)
    if (!done[static_cast<size_t>(i)]) order.push_back(i);
  // 0.7.8 (K-142): a Chaos or Random source that moves nothing (no enabled link) is left out of the
  // pass: it costs nothing and a song with only such sources rests as one without them.
  std::vector<char> used(static_cast<size_t>(nl), 0);
  for (const auto& t : prog.lfoTargets)
    for (const auto& k : t.links) used[static_cast<size_t>(k.lfo)] = 1;
  for (int i : order) {
    ProgLfoOp op;
    if (i < nl && !used[static_cast<size_t>(i)] && prog.lfos[static_cast<size_t>(i)].kind != LfoKind::Lfo) continue;
    if (i < nl) op.kind = ProgLfoOp::Eval, op.index = i;
    else if (i < nl + nt) op.kind = ProgLfoOp::Target, op.index = i - nl;
    else op.kind = ProgLfoOp::Late, op.index = late[static_cast<size_t>(i - nl - nt)];
    prog.lfoOps.push_back(op);
  }
}

float ProgramBuilder::controllerOutput(ObjectId owner, int slot, int output) const {
  if (output < 0 || output >= kMaxControllerOutputs) return 0;
  if (slot < 0) {
    auto rt = channelRuntime(owner);
    return rt ? rt->ctlUi[output].load(std::memory_order_relaxed) : 0.f;
  }
  auto rt = effectRuntime(owner, slot);
  return rt ? rt->ctlUi[output].load(std::memory_order_relaxed) : 0.f;
}

MetronomeBeat metronomeBeat(const Program* p, const ProgPattern* pat, bool song, Tick first) {
  if (song) {
    const MeterMap& m = p->meter;
    Tick t = m.beatStartAt(first);
    if (t < first) t = m.nextBeat(first);
    return {t, m.accentAt(t)};
  }
  const Tick bar = std::max<Tick>(1, pat ? pat->bar : p->barTicks), beat = std::max<Tick>(1, pat ? pat->beat : p->beatTicks);
  const int group = pat ? pat->group : meterGroup(static_cast<int>(bar / beat), static_cast<int>(static_cast<Tick>(p->ppq) * 4 / beat));
  const Tick t = (std::max<Tick>(0, first) + beat - 1) / beat * beat;
  const Tick into = t % bar;
  return {t, into == 0 ? 2 : group > 0 && (into / beat) % group == 0 ? 1 : 0};
}

}  // namespace gl
