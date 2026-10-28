#include "../packs/Packs.h"
#include "Program.h"
#include "ChannelIns.h"
#include "../packs/edison/edison.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include "../core/Arrangements.h"
#include "../core/ClipGain.h"
#include "../core/ClipLoop.h"
#include "../core/PatternAutomation.h"
#include "../core/PluginParams.h"
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
  for (size_t i = 0; i < n; ++i) visit(static_cast<int>(i));
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
    } else {
      switch (static_cast<ChannelParam>(target.index)) {
        case ChannelParam::Volume: pl.kind = ProgLink::ChanVolume; break;
        case ChannelParam::Pan: pl.kind = ProgLink::ChanPan; break;
        case ChannelParam::Pitch: pl.kind = ProgLink::ChanPitch; break;
        case ChannelParam::Mute: pl.kind = ProgLink::ChanMute; break;
        default: return false;
      }
      if (pl.kind != ProgLink::ChanPitch) pc.stripLinked = true;
      else pc.pitchMoves = true;
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
    } else if (target.index >= kSendBase) {
      int dest = static_cast<int>(target.index - kSendBase);
      pl.kind = ProgLink::MixSend;
      pl.param = -1;
      for (size_t i = 0; i < pm.sends.size(); ++i)
        if (pm.sends[i].first == dest) pl.param = static_cast<int>(i);
      if (pl.param < 0) return false;
    } else if (target.index >= static_cast<uint32_t>(MixerParam::EqLowGain) &&
               target.index < static_cast<uint32_t>(MixerParam::Count)) {
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
        case MixerParam::StereoSep: pl.kind = ProgLink::MixSep; break;
        case MixerParam::Mute:
          pl.kind = ProgLink::MixMute;
          pm.muteLinked = true;
          break;
        default: return false;
      }
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
};

bool compileAudioClip(const Song& song, const Channel& ch, const Clip& c, const TempoMap& map, ProgAudioClip& out,
                      const ClipFades* fades = nullptr, const ClipRenderCtx* rctx = nullptr) {
  SamplePtr smp = sampleCache().get(ch.plugin.data);
  if (!smp || smp->frames() < 2 || smp->sampleRate <= 0) return false;
  auto par = [&](int k, double d) { return k < static_cast<int>(ch.plugin.params.size()) ? static_cast<double>(ch.plugin.params[static_cast<size_t>(k)]) : d; };
  const bool sampler = ch.plugin.type == "gl.sampler";
  const double cents = (sampler ? par(pp::SmpPitchShift, 0) : 0.0) + c.pitch;
  const bool stretchMode = sampler && par(pp::SmpStretchMode, 0) >= 0.5;
  // J-157: Rubber Band or the stretch keeps the pitch (also of tempo-synced clips).
  const bool rubber = sampler && stretch::modeOf(par(pp::SmpStretchMode, 0)) == stretch::Mode::RubberBand;
  out.engine = rubber ? stretch::Engine::RubberBand : stretch::Engine::Stretch;
  const stretch::Mode keepMode = rubber ? stretch::Mode::RubberBand : stretch::kStretch;
  const double timeFactor = std::max(0.01, c.stretch * (sampler ? par(pp::SmpTimeStretch, 1) : 1.0));
  const double ratio = std::pow(2.0, cents / 1200.0);
  out.start = c.start;
  out.end = c.start + song.clipLength(c);
  out.startSec = map.secondsAt(static_cast<double>(out.start));
  out.durSec = map.secondsAt(static_cast<double>(out.end)) - out.startSec;
  out.sample = smp;
  out.srcStart = std::max(0.0, c.audioStart);
  out.srcEnd = c.audioEnd > out.srcStart ? std::min(c.audioEnd, smp->seconds()) : smp->seconds();
  out.reverse = c.reverse != (sampler && par(pp::SmpReverse, 0) >= 0.5);
  out.gain = clipgain::playGain(c.gain);  // J-79: the bottom of FL's range is silence
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
  if (c.syncBpm > 0) {  // follows the tempo: always pitch-independent
    out.secPerTick = 60.0 / (c.syncBpm * song.ppq) / timeFactor;
    out.granular = true;
    out.pitch = ratio;
  } else if (stretchMode) {
    out.speed = 1.0 / timeFactor;
    out.pitch = ratio;
    out.granular = ratio != 1.0 || out.speed != 1.0;
  } else {  // resample: pitch and speed together
    out.speed = ratio / timeFactor;
  }
  double played = out.secPerTick > 0 ? static_cast<double>(out.end - out.start) * out.secPerTick : out.durSec * out.speed;
  out.cutEnd = out.srcStart + played < out.srcEnd - 1e-6;
  // J-87: offline-quality render of every pitched / stretched / synced clip.
  // The channel's own pitch (knob + fine + master pitch) is part of it; when
  // it moves at run time (automation), the realtime grains take over.
  if (rctx && out.end > out.start && out.srcEnd > out.srcStart) {
    const double chanSemis = static_cast<double>(ch.pitch) + static_cast<double>(ch.fine) / 100.0 + song.masterPitch;
    const double chanRatio = std::pow(2.0, chanSemis / 12.0);
    // J-153: every stored stretch mode is the one stretch (J-157: or Rubber Band)
    stretch::Mode mode = stretchMode ? keepMode : stretch::Mode::Resample;
    stretch::ClipRequest rq;
    rq.sample = smp;
    rq.srcStart = out.srcStart, rq.srcEnd = out.srcEnd;
    rq.reverse = out.reverse;
    rq.rate = rctx->sampleRate;
    rq.frames = static_cast<int64_t>(std::ceil(out.durSec * rctx->sampleRate)) + 2;
    bool need = false;
    if (out.secPerTick > 0) {  // tempo sync: pitch-independent
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
      rq.speed = out.speed;
      rq.pitch = out.pitch * chanRatio;
      need = std::fabs(rq.pitch - 1.0) > 1e-9 || std::fabs(out.speed - 1.0) > 1e-9;
    } else {  // resample: speed carries the pitch; a channel pitch on top keeps the timing
      rq.speed = out.speed;
      rq.pitch = chanRatio;
      if (std::fabs(chanRatio - 1.0) > 1e-9) mode = stretch::kStretch;
      need = std::fabs(out.speed - 1.0) > 1e-9 || std::fabs(chanRatio - 1.0) > 1e-9;
    }
    rq.mode = mode;
    if (need) {
      out.hq = stretch::renderClip(rq, rctx->async);
      out.hqSemis = chanSemis;
    }
  }
  return out.end > out.start && out.srcEnd > out.srcStart;
}

}  // namespace

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

// Notes that reach `target` through layer channels (nested up to 4 deep).
void addLayerNotes(const Song& song, const Pattern& p, const Channel& target, int depth, std::vector<NoteEv>& out) {
  if (depth > 4) return;
  for (const auto& l : song.channels) {
    if (l.id == target.id || l.layer.empty()) continue;
    auto pos = std::find(l.layer.begin(), l.layer.end(), target.id);
    if (pos == l.layer.end()) continue;
    std::vector<NoteEv> src = compileChannelNotes(song, p, l);
    addLayerNotes(song, p, l, depth + 1, src);
    const float shift = static_cast<float>(l.pitch) + static_cast<float>(l.fine) / 100.0f;
    const size_t self = static_cast<size_t>(pos - l.layer.begin());
    for (NoteEv e : src) {
      if (l.layerMode == 1 && layerHash(e, l.id) % l.layer.size() != self) continue;
      e.pitch += shift;
      out.push_back(e);
    }
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
  const double swing = std::clamp(static_cast<double>(song.swing) * static_cast<double>(c.swingMix), 0.0, 1.0);
  if (auto it = p.steps.find(channel); it != p.steps.end() && stepTicks > 0) {
    const auto& row = it->second;
    // J-11: per-step pitch, properties and shift (graph / keyboard editor).
    const std::vector<StepProps>* props = nullptr;
    if (auto pt = p.stepProps.find(channel); pt != p.stepProps.end()) props = &pt->second;
    // J-31 / J-138: a swung step keeps its length (or, truncated, still ends where the
    // unswung step ends); its start follows the swing warp, to a fraction of a tick.
    for (size_t s = 0; s < total; ++s) {
      const size_t src = loop ? s % loop : s;
      if (src >= row.size() || row[src] == 0) continue;
      const Tick grid = static_cast<Tick>(s) * stepTicks;
      double on = swingWarp(static_cast<double>(grid), swing, stepTicks, p.stepsPerBar);
      const double gridEnd = static_cast<double>(grid + stepTicks);
      auto stepEnd = [&](double start) {
        return c.swingTruncate ? std::max(start + 1.0, gridEnd) : start + static_cast<double>(stepTicks);
      };
      if (!props || src >= props->size() || (*props)[src].isDefault()) {
        NoteEv e = makeEv(0, 0, 60.0f, static_cast<float>(row[src]) / 127.0f);
        setOn(e, on);
        setOff(e, stepEnd(on));
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
      out.push_back(e);
    }
  }
  const Tick loopTicks = static_cast<Tick>(loop) * stepTicks;
  const Tick patLen = static_cast<Tick>(total) * stepTicks;
  if (auto it = p.notes.find(channel); it != p.notes.end())
    for (const auto& n : it->second)
      for (Tick rep = 0;; rep += loopTicks) {  // one pass, or the step loop's repeats (J-31)
      if (n.muted) break;  // J-11: muted notes stay in the pattern but do not play
      if (loopTicks > 0 && (n.start >= loopTicks || n.start + rep >= patLen)) break;
      NoteEv e = makeEv(n.start + rep, n.start + rep + n.length, static_cast<float>(n.pitch) + static_cast<float>(n.fine) / 100.0f,
                        static_cast<float>(n.velocity) / 127.0f);
      if (swing > 0 && stepTicks > 0 && song.swingNotes) {  // J-138: notes swing with the steps (written positions stay straight)
        const double on = swingWarp(static_cast<double>(e.on), swing, stepTicks, p.stepsPerBar);
        const double end = swingWarp(static_cast<double>(e.off), swing, stepTicks, p.stepsPerBar);
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
      if (n.release == kNoteReleaseDefault) e.props.release = 0.5f;  // exact neutral values
      if (n.modX == kNoteModCentre) e.props.modX = 0.5f;
      if (n.modY == kNoteModCentre) e.props.modY = 0.5f;
      e.flags = static_cast<uint8_t>((n.slide ? kNoteSlide : 0) | (n.porta ? kNotePorta : 0));
      out.push_back(e);
      if (loopTicks <= 0) break;
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
    const float lo = static_cast<float>(c->keyLo) - 0.5f, hi = static_cast<float>(c->keyHi) + 0.5f;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const NoteEv& e) { return e.pitch < lo || e.pitch >= hi; }), out.end());
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
      if (!c.plugin.data.empty() && isAudioFile(c.plugin.data)) paths.push_back(c.plugin.data);
    if (asyncSamples_)
      for (const auto& p : paths) sampleCache().request(p);
    else
      sampleCache().prefetch(paths);
  }
  auto prog = std::make_shared<Program>();
  prog->tempo = song.tempo;
  prog->ppq = song.ppq;
  prog->barTicks = song.barTicks();
  prog->beatTicks = song.beatTicks();
  prog->songEnd = song.lengthTicks();
  Tick bar = std::max<Tick>(1, prog->barTicks);
  prog->songLoopEnd = std::max(bar, (prog->songEnd + bar - 1) / bar * bar);
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
  for (const auto& m : song.mixer) {
    ProgMixer pm;
    pm.id = m.id;
    auto it = mixers_.find(m.id);
    pm.rt = it != mixers_.end() ? it->second : std::make_shared<MixerRuntime>();
    mixers[m.id] = pm.rt;
    double g = dsp::faderGain(m.volume, 0.8), l, r;
    dsp::panGains(m.pan, l, r);
    pm.gainL = static_cast<float>(g * l);
    pm.gainR = static_cast<float>(g * r);
    pm.volume = pm.modVolume = m.volume;
    pm.pan = pm.modPan = m.pan;
    pm.sep = pm.sepBase = m.stereoSep;
    pm.invert = m.invertPolarity;
    pm.swap = m.swapLR;
    pm.inFirst = m.input;
    pm.inStereo = m.inputStereo;
    pm.monitor = m.monitor;
    pm.armed = m.records();
    pm.recPoint = m.recPoint;
    if (pm.armed && m.recPoint == static_cast<uint8_t>(RecordPoint::PostEffects)) pm.hasPreSend = true;  // J-31: keeps the pre-fader copy
    if (pm.armed && !pm.rt->rec) pm.rt->rec = std::make_shared<packs::edison::CaptureRing>();
    pm.eq.gain = m.eqGain;
    pm.eq.freq = m.eqFreq;
    pm.eq.q = m.eqQ;
    pm.disabled = m.disabled;
    pm.threaded = m.threaded;
    pm.latencyManual = std::clamp(m.latencyOffset, 0, kMaxManualLatency);
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
        auto e = effects_.find(key);
        if (e != effects_.end() && e->second->type == es.plugin.type && !(e->second->fx && e->second->fx->stale())) {
          ps.rt = e->second;
        } else if (auto fx = createEffect(es.plugin.type)) {
          ps.rt = std::make_shared<EffectRuntime>();
          ps.rt->type = es.plugin.type;
          ps.rt->fx = std::move(fx);
          if (es.plugin.type == kMixerSendType || es.plugin.type == kBusSendType) {  // pulled by the destination
            ps.rt->sendL.assign(kEngineMaxBlock, 0.f);
            ps.rt->sendR.assign(kEngineMaxBlock, 0.f);
          }
          if (const PluginInfo* pi = findPlugin(es.plugin.type); pi && !pi->sendParams.empty())
            for (size_t k = 0; k < pi->sendParams.size() && k < static_cast<size_t>(kMaxSendOutputs); ++k) {
              ps.rt->multiL[k].assign(kEngineMaxBlock, 0.f);
              ps.rt->multiR[k].assign(kEngineMaxBlock, 0.f);
            }
        }
        if (ps.rt) {
          effects[key] = ps.rt;
          prepareForHost(es.plugin, live, m.id, slotParam(m.id, static_cast<int>(s), 0).index,
                         [&](const PluginState& st) { ps.rt->fx->prepareHost(st, sampleRate_); });
        }
        packs::prepareAux(es.plugin, ps.aux);
        ps.controller = hasOutputs(es.plugin.type);
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
    prog->mixer.push_back(std::move(pm));
  }
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
    if (it != channels_.end() && it->second->type == c.plugin.type && !(it->second->gen && it->second->gen->stale())) {
      pc.rt = it->second;
    } else {
      pc.rt = std::make_shared<ChannelRuntime>();
      pc.rt->type = c.plugin.type;
      pc.rt->gen = createGenerator(c.plugin.type);
      if (pc.rt->gen) pc.rt->gen->setMidiOut(&pc.rt->midiOut);
    }
    chans[c.id] = pc.rt;
    pc.plugin = c.plugin;
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
    pc.controller = hasOutputs(c.plugin.type);
    double g = c.muted ? 0.0 : dsp::faderGain(c.gain, 0.78), l, r;
    dsp::panGains(c.pan, l, r);
    pc.gainL = static_cast<float>(g * l);
    pc.gainR = static_cast<float>(g * r);
    pc.volume = pc.modVolume = c.gain;
    pc.pan = pc.modPan = c.pan;
    pc.muted = pc.modMuted = c.muted;
    pc.pitch = c.pitch;
    pc.masterPitch = song.masterPitch + static_cast<float>(c.fine) / 100.0f;
    pc.pitchOffset = static_cast<float>(c.pitch) + static_cast<float>(c.fine) / 100.0f + song.masterPitch;
    int mi = song.mixerIndex(c.mixerTrack);
    pc.mixer = mi >= 0 ? mi : prog->master;
    for (ObjectId t : c.auxOuts) pc.auxMixer.push_back(t == kNoId ? -1 : song.mixerIndex(t));
    pc.polyphony = c.polyphony;
    pc.mono = c.mono;
    pc.porta = c.porta;
    pc.portaTime = c.portaTime;
    pc.cutGroup = c.cutGroup;
    pc.cutBy = c.cutBy;
    pc.layerMode = c.layerMode;
    pc.keyLo = c.keyLo;
    pc.keyHi = c.keyHi;
    for (ObjectId k : c.layer) {
      int ki = song.channelIndex(k);
      if (ki >= 0 && k != c.id) pc.layer.push_back(ki);
    }
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
    prog->channels.push_back(std::move(pc));
  }
  // Plugin MIDI output ports → channels listening on the same input port.
  for (size_t i = 0; i < song.channels.size() && i < prog->channels.size(); ++i) {
    const int port = song.channels[i].midiOutPort;
    if (port < 0) continue;
    for (size_t j = 0; j < song.channels.size() && j < prog->channels.size(); ++j)
      if (j != i && song.channels[j].midiInPort == port) prog->channels[i].midiTargets.push_back(static_cast<int>(j));
  }

  // --- internal controller links ---
  std::map<uint64_t, std::shared_ptr<LinkRuntime>> linkRts;
  for (const auto& ml : song.modLinks) {
    if (!ml.enabled) continue;
    ProgLink pl;
    if (ml.sourceSlot < 0) {
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
    if (!resolveTarget(song, *prog, target, pl)) continue;  // song parameters are not modulated
    pl.min = ml.min;
    pl.max = ml.max;
    pl.smoothMs = ml.smoothMs;
    pl.curve = ml.curve;
    if (ml.curve == ModCurve::Formula && !pl.formula.compile(ml.formula.empty() ? "input" : ml.formula, {"input", "base", "t", "beat"}))
      pl.curve = ModCurve::Linear;  // invalid formula: plain mapping
    uint64_t key = target.packed();
    auto lr = links_.find(key);
    pl.rt = lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
    linkRts[key] = pl.rt;
    prog->links.push_back(std::move(pl));
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
    if (lane.owner == kSongId) return false;  // tempo: tempo map; swing / master pitch: not automated
    ProgAutoLane al;
    ParamId target{lane.owner, lane.index};
    if (!resolveTarget(song, *prog, target, al.target)) return false;
    uint64_t key = target.packed();
    auto lr = links_.find(key);
    al.target.rt = linkRts.count(key) ? linkRts[key] : lr != links_.end() ? lr->second : std::make_shared<LinkRuntime>();
    linkRts[key] = al.target.rt;
    al.lane = std::move(lane);
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
        for (auto& lane : singlePatternLanes(*p, len, c.offset, clipLoopLength(song, c))) prog->hasPatternAutos |= compileLane(std::move(lane), lanes);
    } else if (c.kind == ClipKind::Automation) {
      if (const AutomationClip* a = song.automation(c.source); a && !a->points.empty() && a->owner != kNoId) {
        AutomationLane l;
        l.owner = a->owner;
        l.index = a->index;
        l.inst.push_back({0, len, c.offset, std::make_shared<const AutomationClip>(*a)});
        prog->hasPatternAutos |= compileLane(std::move(l), lanes);
      }
    }
    liveAutos.push_back(std::move(lanes));
    liveIdx.push_back(static_cast<int>(i));
  }
  // Live copies for the states that links rewrite.
  for (auto& pc : prog->channels) {
    if (pc.modded) pc.live = pc.plugin;
    for (auto& nf : pc.noteFx)
      if (nf.modded) nf.live = nf.state;
  }
  for (auto& pm : prog->mixer)
    for (auto& ps : pm.slots)
      if (ps.modded) ps.live = ps.plugin;

  // --- patterns (pattern mode) and song events ---
  for (const auto& p : song.patterns) {
    ProgPattern pp;
    pp.id = p.id;
    pp.length = std::max<Tick>(1, song.patternContentLength(p));
    for (const auto& c : song.channels) pp.perChannel.push_back(compilePatternChannel(song, p, c.id));
    if (auto it = patternAutos.find(p.id); it != patternAutos.end()) pp.autos = std::move(it->second);
    prog->patterns.push_back(std::move(pp));
  }
  const std::vector<ClipFades> clipFades = resolveAllFades(song, prog->tempoMap);  // J-38
  for (size_t clipIdx = 0; clipIdx < song.playlist.size(); ++clipIdx) {
    const Clip& clip = song.playlist[clipIdx];
    if (clip.track >= 0 && static_cast<size_t>(clip.track) < song.tracks.size() &&
        song.tracks[static_cast<size_t>(clip.track)].muted)
      continue;
    if (clip.muted || inLiveArea(song, clip)) continue;  // live area: launched by hand (below)
    if (clip.kind == ClipKind::Audio) {  // arrangement: audio clip through its channel
      int ci = song.channelIndex(clip.source);
      ProgAudioClip ac;
      const ClipRenderCtx rctx{sampleRate_, asyncSamples_};
      if (ci >= 0 && compileAudioClip(song, song.channels[static_cast<size_t>(ci)], clip, prog->tempoMap, ac, &clipFades[clipIdx], &rctx))
        prog->channels[static_cast<size_t>(ci)].audio.push_back(std::move(ac));
      continue;
    }
    if (clip.kind != ClipKind::Pattern) continue;
    const ProgPattern* pp = prog->pattern(clip.pattern);
    if (!pp) continue;
    // The clip shows pattern ticks [offset, offset + len); a looping
    // pattern repeats them pass by pass (Q-1, core/ClipLoop.h).
    const Tick len = song.clipLength(clip);
    const std::vector<ClipPass> passes = clipPasses(song, clip);
    for (size_t ci = 0; ci < pp->perChannel.size(); ++ci)
      for (const ClipPass& ps : passes)
        for (const auto& e : pp->perChannel[ci]) {
          if (e.on < ps.content || e.on >= ps.content + ps.len) continue;
          NoteEv x = e;
          x.on = clip.start + ps.at + (e.on - ps.content);
          x.off = clip.start + ps.at + (e.off - ps.content);
          if (noteOffTick(x) > static_cast<double>(clip.start + len)) x.off = clip.start + len, x.offFrac = 0;
          prog->channels[ci].song.push_back(x);
        }
  }
  // --- live area: clips compiled for launching (content ticks from 0) ---
  for (size_t k = 0; k < liveIdx.size(); ++k) {
    const Clip& clip = song.playlist[static_cast<size_t>(liveIdx[k])];
    ProgLiveClip lc;
    lc.clip = clip.id;
    lc.track = clip.track;
    lc.length = std::max<Tick>(1, song.clipLength(clip));
    lc.autos = std::move(liveAutos[k]);
    if (clip.kind == ClipKind::Pattern) {
      if (const ProgPattern* pp = prog->pattern(clip.pattern))
        for (size_t ci = 0; ci < pp->perChannel.size(); ++ci)
          for (const ClipPass& ps : clipPasses(song, clip))  // Q-1: repeats of a looping pattern
            for (const auto& e : pp->perChannel[ci]) {
              if (e.on < ps.content || e.on >= ps.content + ps.len) continue;
              ProgLiveClip::Ev x;
              x.channel = static_cast<int>(ci);
              x.e = e;
              x.e.on = ps.at + (e.on - ps.content);
              x.e.off = ps.at + (e.off - ps.content);
              if (noteOffTick(x.e) > static_cast<double>(lc.length)) x.e.off = lc.length, x.e.offFrac = 0;
              lc.notes.push_back(x);
            }
      std::stable_sort(lc.notes.begin(), lc.notes.end(), [](const ProgLiveClip::Ev& a, const ProgLiveClip::Ev& b) { return noteOnTick(a.e) < noteOnTick(b.e); });
    } else if (clip.kind == ClipKind::Audio) {
      int ci = song.channelIndex(clip.source);
      Clip at0 = clip;
      at0.start = 0;
      ProgAudioClip ac;
      const ClipRenderCtx rctx{sampleRate_, asyncSamples_};
      if (ci >= 0 && compileAudioClip(song, song.channels[static_cast<size_t>(ci)], at0, prog->flatMap, ac, nullptr, &rctx)) {
        lc.audioChannel = ci;
        lc.audio.push_back(std::move(ac));
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
  for (auto& pc : prog->channels) {
    std::stable_sort(pc.song.begin(), pc.song.end(), [](const NoteEv& a, const NoteEv& b) { return noteOnTick(a) < noteOnTick(b); });
    std::stable_sort(pc.audio.begin(), pc.audio.end(), [](const ProgAudioClip& a, const ProgAudioClip& b) { return a.start < b.start; });
  }

  // J-153: the realtime stretch for the clips that may need it: while their
  // render is on its way (app builds) or when the channel pitch moves.
  // J-157: a Rubber Band voice (about 6 MB) only where it can play: while the
  // clip's render is still on its way, or when the channel pitch moves.
  auto giveVoice = [&](ProgAudioClip& ac, const ProgChannel& pc) {
    const bool rubber = ac.engine == stretch::Engine::RubberBand;
    if ((asyncSamples_ && ac.hq && !(rubber && ac.hq->ready.load(std::memory_order_acquire))) || pc.pitchMoves)
      ac.rt = makeClipVoice(sampleRate_, ac.engine);
  };
  for (auto& pc : prog->channels)
    for (auto& ac : pc.audio) giveVoice(ac, pc);
  for (auto& lc : prog->liveClips)
    if (lc.audioChannel >= 0 && static_cast<size_t>(lc.audioChannel) < prog->channels.size())
      for (auto& ac : lc.audio) giveVoice(ac, prog->channels[static_cast<size_t>(lc.audioChannel)]);

  compileGraph(*prog);

  // Keep only live runtimes so deleted channels/tracks free their DSP state
  // once the last Program that references them is retired.
  channels_ = std::move(chans);
  mixers_ = std::move(mixers);
  effects_ = std::move(effects);
  noteFx_ = std::move(noteFx);
  links_ = std::move(linkRts);
  return prog;
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

bool ProgramBuilder::modulatedValue(ParamId id, double& value) const {
  auto it = links_.find(id.packed());
  if (it == links_.end() || !it->second->active.load(std::memory_order_relaxed)) return false;
  value = it->second->display.load(std::memory_order_relaxed);
  return true;
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

}  // namespace gl
