// Gridline — FLP import: channel rack (names, colours, instrument kind,
// routing, samples) and the channel settings every FL channel carries:
// levels, fine tune, root note, swing / time shift, polyphony / mono / porta,
// cut groups, echo, arpeggiator, layers, display groups, and for sampler
// channels the sample settings (region, loop, fades, reverse, filter,
// envelopes, LFO, time stretch).
//
// Field layouts come from analysis/reference/format-flp.md and were checked
// against the shipped demo projects (see analysis/demos/notes_beta.md).
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../PluginParams.h"
#include "Flp.h"

namespace gl::flp {

namespace {
Rgb flColor(uint32_t v) { return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF); }

void setParam(PluginState& s, int k, double v) {
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) s.params[static_cast<size_t>(k)] = static_cast<float>(v);
}

// FL envelope / LFO knob values are 100..65536 (0..100 %). The time curve is
// an approximation (cubic, 0..10 s) — FL shows these knobs as times without
// storing the curve in the project.
double envTime(int32_t v) {
  double x = std::clamp((static_cast<double>(v) - 100.0) / 65436.0, 0.0, 1.0);
  return 10.0 * x * x * x;
}

}  // namespace

ChannelIns decodeChannelIns(const std::vector<Event>& evs) {
  ChannelIns ins;
  int k = 0;
  for (const auto& e : evs) {
    if (e.id != 218 || e.size < 68 || k >= 5) continue;
    ChannelIns::Env& v = ins.env[k++];
    const int32_t flags = e.i32(0);
    v.enabled = e.i32(4) != 0;
    v.tempoSync = flags & 1;
    v.predelay = envTime(e.i32(8));
    v.attack = envTime(e.i32(12));
    v.hold = envTime(e.i32(16));
    v.decay = envTime(e.i32(20));
    v.sustain = std::clamp(e.i32(24) / 128.0, 0.0, 1.0);
    v.release = envTime(e.i32(28));
    v.amount = std::clamp(e.i32(32) / 128.0, -1.0, 1.0);
    v.lfoDelay = envTime(e.i32(36));
    v.lfoAmount = std::clamp(e.i32(44) / 128.0, -1.0, 1.0);
    // LFO speed 200..65536, logarithmic: ~0.05 .. 20 Hz (approximation).
    double x = std::clamp((e.i32(48) - 200.0) / 65336.0, 0.0, 1.0);
    v.lfoRate = 0.05 * std::pow(400.0, x);
    v.lfoShape = std::clamp(e.i32(52), 0, 2);
  }
  if (const Event* e = find(evs, 219); e && e->size >= 24) {  // Levels: pan, volume, pitch, filter mod X / Y / type
    ins.cutoff = std::clamp(static_cast<int>(e->u32(12)), 0, 256);
    ins.resonance = std::clamp(static_cast<int>(e->u32(16)), 0, 256);
    ins.filterType = static_cast<int>(e->u32(20));
  }
  ins.present = k > 0;
  return ins;
}

void applyInsToSampler(const ChannelIns& ins, PluginState& s) {
  using namespace pp;
  // Channel filter (Mod X = cutoff, Mod Y = resonance): active once moved from fully open.
  if (ins.cutoff < 256 || ins.resonance > 0) {
    static const int kType[8] = {1, 1, 3, 4, 5, 2, 1, 2};  // fast LP, LP, BP, HP, notch, LP x2, SVF LP, SVF LP x2
    setParam(s, SmpFilterType, kType[std::clamp(ins.filterType, 0, 7)]);
    setParam(s, SmpCutoff, 20.0 * std::pow(1000.0, ins.cutoff / 256.0));
    setParam(s, SmpReso, ins.resonance / 256.0);
  }
  const ChannelIns::Env& vol = ins.env[ChannelIns::Volume];
  if (vol.enabled) {  // volume envelope: the note length gates the sample
    setParam(s, SmpOneShot, 0);
    setParam(s, SmpEnvDelay, std::min(vol.predelay, 5.0));
    setParam(s, SmpAttack, std::clamp(vol.attack, 0.0005, 5.0));
    setParam(s, SmpEnvHold, std::min(vol.hold, 10.0));
    setParam(s, SmpDecay, std::clamp(vol.decay, 0.005, 10.0));
    setParam(s, SmpSustain, vol.sustain);
    setParam(s, SmpRelease, std::clamp(vol.release, 0.002, 8.0));
  }
  const ChannelIns::Env& cut = ins.env[ChannelIns::ModX];
  if (cut.enabled && cut.amount != 0) {
    if (s.params.size() > SmpFilterType && s.params[SmpFilterType] < 0.5f) {
      setParam(s, SmpFilterType, 1);
      setParam(s, SmpCutoff, 20.0 * std::pow(1000.0, ins.cutoff / 256.0));
    }
    setParam(s, SmpFiltEnv, cut.amount);
    setParam(s, SmpFiltAttack, std::clamp(cut.attack, 0.001, 10.0));
    setParam(s, SmpFiltDecay, std::clamp(cut.decay, 0.005, 30.0));
    setParam(s, SmpFiltSustain, cut.sustain);
    setParam(s, SmpFiltRelease, std::clamp(cut.release, 0.005, 10.0));
  }
  // One LFO: the first target with an amount (volume, pan, cutoff, pitch).
  static const std::pair<int, int> kLfo[4] = {{ChannelIns::Volume, 1}, {ChannelIns::Pan, 2}, {ChannelIns::ModX, 3}, {ChannelIns::Pitch, 4}};
  for (auto [env, target] : kLfo) {
    const ChannelIns::Env& e = ins.env[env];
    if (e.lfoAmount == 0) continue;
    setParam(s, SmpLfoTarget, target);
    setParam(s, SmpLfoShape, e.lfoShape);
    setParam(s, SmpLfoRate, std::clamp(e.lfoRate, 0.01, 20.0));
    setParam(s, SmpLfoAmount, e.lfoAmount);
    setParam(s, SmpLfoDelay, std::min(e.lfoDelay, 5.0));
    if (s.params.size() > SmpFilterType && target == 3 && s.params[SmpFilterType] < 0.5f) setParam(s, SmpFilterType, 1);
    break;
  }
}

void importChannels(Context& c) {
  Song& s = c.song;
  // Display groups: names (231, project-wide, in order) indexed by 145 per channel.
  for (const auto& e : c.sec.project)
    if (e.id == 231) s.channelGroups.push_back(e.text());
  int switchedOff = 0;
  int cutGroups = 0, selfCutIgnored = 0, echoes = 0, arps = 0, monos = 0, portas = 0, timeShifts = 0;
  std::map<int, std::vector<int>> layerKids;  // FL layer channel → FL child indices
  std::map<int, int> layerFlags;
  for (const auto& [idx, evs] : c.sec.channels) {
    int type = 0;
    if (const Event* e = find(evs, 21)) type = static_cast<int>(e->value());
    c.channelTypes[idx] = type;
    // Automation channels (5) become automation clips (arrangement importer).
    // Audio-clip channels (4) are rack channels too: their sample plays from
    // the playlist, and patterns can hold notes for them (FL plays the sample
    // like a sampler channel). The arrangement importer adds their clip settings.
    if (type == 5) continue;
    Channel ch;
    ch.id = s.allocateId();
    ch.name.clear();
    if (const Event* e = find(evs, 203)) ch.name = e->text();
    if (ch.name.empty())
      if (const Event* e = find(evs, 192)) ch.name = e->text();
    if (ch.name.empty()) ch.name = "Channel " + std::to_string(idx + 1);
    if (const Event* e = find(evs, 128)) ch.color = flColor(e->value());
    // Channel switch (0): off = the channel is muted.
    if (const Event* e = find(evs, 0); e && e->value() == 0) ch.muted = true, ++switchedOff;
    std::string sample;
    if (const Event* e = find(evs, 196)) sample = c.resolvePath(e->text());
    if (type == 0 || type == 4) {
      ch.plugin = defaultPluginState("gl.sampler");
      ch.plugin.data = sample;
    } else if (type == 3) {
      ch.plugin = PluginState{};  // layer: no instrument of its own
    } else {
      ch.plugin = defaultPluginState("gl.synth");  // replaced by importPlugins when the plugin is known
    }
    const Event* insEv = find(evs, 22);
    if (!insEv && type == 4) insEv = find(evs, 104);  // FL 24.2.99+ audio-clip channels store the insert here
    if (const Event* e = insEv) {  // mixer insert
      auto it = c.insertIds.find(e->id == 104 ? static_cast<int>(static_cast<int16_t>(e->value())) : static_cast<int>(e->ivalue()));
      ch.mixerTrack = it != c.insertIds.end() ? it->second : s.masterId();
    } else {
      ch.mixerTrack = s.masterId();
    }

    // --- Levels (219): pan 0..12800, volume 0..12800 (10000 = default), pitch in cents.
    double cents = 0;
    if (const Event* e = find(evs, 219); e && e->size >= 12) {
      ch.pan = static_cast<float>(std::clamp((static_cast<double>(e->i32(0)) - 6400.0) / 6400.0, -1.0, 1.0));
      ch.gain = static_cast<float>(std::clamp(e->u32(4) / 12800.0, 0.0, 1.25));
      cents = e->i32(8);
    }
    // Level adjustments (229): pan offset, volume multiplier (12800 = 100 %).
    if (const Event* e = find(evs, 229); e && e->size >= 8) {
      ch.pan = static_cast<float>(std::clamp(ch.pan + e->i32(0) / 6400.0, -1.0, 1.0));
      ch.gain = static_cast<float>(std::clamp(ch.gain * (e->u32(4) / 12800.0), 0.0, 1.25));
    }
    if (const Event* e = find(evs, 142)) cents += e->ivalue();  // fine tune (cents)
    const Event* params = find(evs, 215);
    // Resample-mode speed change (sampler): the pitch follows the speed.
    double stretchMul = 1;
    int stretchMode = 0;
    if (params && params->size >= 112) {
      stretchMul = std::pow(2.0, params->i32(104) / 10000.0);
      stretchMode = params->i32(108);
      if (type == 0 && stretchMode == 0 && stretchMul != 1) cents -= 1200.0 * std::log2(stretchMul);
    }
    {
      long total = std::lround(cents);
      long semis = std::clamp<long>((total >= 0 ? total + 50 : total - 50) / 100, -24, 24);
      ch.pitch = static_cast<int>(semis);
      ch.fine = static_cast<int>(std::clamp<long>(total - semis * 100, -200, 200));
    }
    // Audio-clip channels: the pitch knob moves each clip (arrangement importer).
    if (type == 4) ch.pitch = 0, ch.fine = 0;
    // --- Time: swing mix (0..128 → 0..1), fine time shift (0..1024 = up to one step).
    if (const Event* e = find(evs, 97)) ch.swingMix = static_cast<float>(std::clamp(e->value() / 128.0, 0.0, 1.0));
    if (const Event* e = find(evs, 89); e && e->value() > 0) {
      ch.timeShift = static_cast<float>(std::clamp(e->value() / 1024.0, 0.0, 1.0) * 0.25);
      ++timeShifts;
    }
    // --- Polyphony (221): max voices, porta (slide) time 0..1024, flags (1 mono, 2 porta).
    if (const Event* e = find(evs, 221); e && e->size >= 9) {
      ch.polyphony = std::clamp(static_cast<int>(e->u32(0)), 0, 256);
      double t = std::clamp(e->u32(4) / 1024.0, 0.0, 2.0);
      ch.portaTime = static_cast<float>(0.5 * t * t);  // approximation of FL's time curve
      ch.mono = e->u8(8) & 1;
      ch.porta = (e->u8(8) & 2) != 0;
      monos += ch.mono;
      portas += ch.porta;
    }
    // --- Cut groups (132): low word = cut, high word = cut by. FL gives every
    // plugin channel its own self-cut group (cut == cut by) without it acting
    // on the notes (the demos have overlapping notes on such channels that
    // ring), so self-cut is applied to sampler channels only.
    if (const Event* e = find(evs, 132)) {
      int cut = static_cast<int>(e->value() & 0xFFFF), by = static_cast<int>(e->value() >> 16);
      if (cut != by || type == 0) {
        ch.cutGroup = std::clamp(cut, 0, kMaxCutGroups);
        ch.cutBy = std::clamp(by, 0, kMaxCutGroups);
        cutGroups += ch.cutGroup > 0 || ch.cutBy > 0;
      } else if (cut > 0) {
        ++selfCutIgnored;
      }
    }
    // --- Echo delay (209): feedback (12800 = 100 %), pan, pitch (cents), echoes, time (ticks).
    if (const Event* e = find(evs, 209); e && e->size >= 20 && e->u32(0) > 0) {
      ch.echo.count = std::clamp(static_cast<int>(e->u32(12)), 1, 10);
      ch.echo.feedback = static_cast<float>(std::clamp(e->u32(0) / 12800.0, 0.0, 1.0));
      ch.echo.pan = static_cast<float>(std::clamp((e->i32(4) - 6400.0) / 6400.0, -1.0, 1.0));
      ch.echo.pitch = std::clamp(e->i32(8), -1200, 1200);
      ch.echo.time = static_cast<float>(std::clamp(e->u32(16) / static_cast<double>(std::max(1, c.file.ppq)), 0.01, 16.0));
      ch.echo.pingPong = params && params->size > 10 && (params->u8(10) & 2);
      // 138: per-echo mod X (low word) / mod Y (high word), 0..256, 128 = none.
      if (const Event* m = find(evs, 138)) {
        ch.echo.modX = static_cast<float>(std::clamp((static_cast<int>(m->value() & 0xFFFF) - 128) / 128.0, -1.0, 1.0));
        ch.echo.modY = static_cast<float>(std::clamp((static_cast<int>(m->value() >> 16) - 128) / 128.0, -1.0, 1.0));
      }
      ++echoes;
    }
    // --- Arpeggiator (215 @40): direction, range, chord, time, gate, repeat → the arpeggiator note processor.
    if (params && params->size >= 96 && params->u32(40) > 0 && type != 3) {
      PluginState arp = defaultPluginState("gl.arp");
      static const int kDir[6] = {0, 0, 1, 2, 2, 4};  // off, up, down, up-down bounce, up-down sticky, random
      setParam(arp, 0, kDir[std::min<uint32_t>(params->u32(40), 5)]);              // direction
      setParam(arp, 1, std::clamp(static_cast<int>(params->u32(44)) - 1, 0, 3));  // range (octaves)
      // Time: 1024 = one step (a 16th); pick the nearest rate.
      static const double kRateBeats[12] = {4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
      double beats = params->u32(52) / 1024.0 * 0.25;
      int best = 4;
      for (int k = 0; k < 12; ++k)
        if (std::fabs(std::log2(kRateBeats[k] / std::max(1e-3, beats))) < std::fabs(std::log2(kRateBeats[best] / std::max(1e-3, beats)))) best = k;
      setParam(arp, 2, best);
      setParam(arp, 3, std::clamp(params->u32(56) / 100.0, 0.05, 1.0));        // gate
      setParam(arp, 4, std::clamp(static_cast<int>(params->u32(92)), 1, 8));   // repeat
      ch.noteFx.push_back(arp);
      ++arps;
    }
    // --- Display group (145).
    if (const Event* e = find(evs, 145)) {
      int g = e->ivalue();
      if (g >= 0 && static_cast<size_t>(g) < s.channelGroups.size()) ch.group = g;
    }
    // --- Layer children (94, one event per child) and flags (144: 1 random, 2 crossfade).
    if (type == 3) {
      for (const auto& e : evs)
        if (e.id == 94) layerKids[idx].push_back(static_cast<int>(e.value()));
      if (const Event* e = find(evs, 144)) layerFlags[idx] = static_cast<int>(e->value());
    }

    // --- Sampler: sample settings.
    if (type == 0) {
      using namespace pp;
      PluginState& sp = ch.plugin;
      if (const Event* e = find(evs, 135)) setParam(sp, SmpRoot, std::clamp(e->ivalue(), 0, 127));
      if (const Event* e = find(evs, 70)) {  // FX flags: 1 stereo fade, 2 reverse, 4 boost clip, 256 swap stereo
        if (e->value() & 1) setParam(sp, SmpPreFadeStereo, 1);
        if (e->value() & 2) setParam(sp, SmpReverse, 1);
        if (e->value() & 4) setParam(sp, SmpPreClip, 1);
        if (e->value() & 256) setParam(sp, SmpSwapStereo, 1);
      }
      // Precomputed effects (J-23, laws in engine/SamplePrecompute.cpp): tilt
      // (69, 0..256, 128 flat), cutoff (71, 16..1024, 1024 off), boost (74,
      // 0..256), resonance (83, 0..640), stereo delay (85, 0..4096, 2048 off),
      // time warp (86, 0..512, 256 off), ring modulation (131: mix low word,
      // frequency high word, 0..256), reverb (139: mix low word 0..256, type
      // high word).
      if (const Event* e = find(evs, 69)) setParam(sp, SmpPreTilt, std::clamp((e->ivalue() - 128) / 128.0, -1.0, 1.0));
      if (const Event* e = find(evs, 71)) setParam(sp, SmpPreCutoff, std::clamp(e->ivalue() / 1024.0, 1.0 / 64, 1.0));
      if (const Event* e = find(evs, 74)) setParam(sp, SmpPreBoost, std::clamp(e->ivalue() / 256.0, 0.0, 1.0));
      if (const Event* e = find(evs, 83)) setParam(sp, SmpPreReso, std::clamp(e->ivalue() / 640.0, 0.0, 1.0));
      if (const Event* e = find(evs, 85)) setParam(sp, SmpPreStereoDelay, std::clamp((e->ivalue() - 2048) / 44.1, -46.44, 46.44));
      if (const Event* e = find(evs, 86)) setParam(sp, SmpPreWarp, std::clamp((e->ivalue() - 256) / 256.0, -1.0, 1.0));
      if (const Event* e = find(evs, 131)) {
        setParam(sp, SmpPreRingMix, std::clamp((e->value() & 0xFFFF) / 256.0, 0.0, 1.0));
        setParam(sp, SmpPreRingFreq, std::clamp(((e->value() >> 16) + 8) * 44100.0 / 5760.0, 61.25, 2021.25));
      }
      if (const Event* e = find(evs, 139)) {
        setParam(sp, SmpPreReverbMix, std::clamp((e->value() & 0xFFFF) / 256.0, 0.0, 1.0));
        setParam(sp, SmpPreReverbType, (e->value() >> 16) ? 1 : 0);
      }
      // Sampler flags (143) bit 8 = "use the file's loop points": FL sets it by
      // default and it only loops samples that carry loop markers, which the
      // sample loader does not read — so it does not switch looping on here.
      if (const Event* e = find(evs, 20); e && e->value()) {  // ping-pong loop
        setParam(sp, SmpLoop, 1);
        setParam(sp, SmpLoopMode, 1);
      }
      // Quick fades (76 in, 75 out, 0..1024): linear ramps across the whole
      // sample whose slope is value / 256 (they reach full level only at the
      // end / start), applied with the precomputed effects.
      if (const Event* e = find(evs, 76)) setParam(sp, SmpPreRampIn, std::clamp(e->ivalue() / 256.0, 0.0, 4.0));
      if (const Event* e = find(evs, 75)) setParam(sp, SmpPreRampOut, std::clamp(e->ivalue() / 256.0, 0.0, 4.0));
      if (params && params->size >= 144) {
        if (params->u8(9)) setParam(sp, SmpRemoveDc, 1);
        if (params->u8(80)) setParam(sp, SmpNormalize, 1);
        if (params->u8(81)) setParam(sp, SmpPolarity, 1);
        static const int kDeclick[6] = {1, 2, 2, 3, 4, 4};
        setParam(sp, SmpDeclick, kDeclick[std::min<int>(params->u8(83), 5)]);
        setParam(sp, SmpLoopXfade, std::clamp(params->u32(84) / 256.0, 0.0, 1.0) * 0.5);
        double start = params->f64(128), end = params->f64(136);
        if (std::isfinite(start) && start > 0 && start < 1) setParam(sp, SmpStart, start);
        if (std::isfinite(end) && end > 0 && end < 1) setParam(sp, SmpEnd, end);
        if (stretchMode != 0 && (stretchMul != 1 || params->i32(100) != 0)) {  // time stretch (pitch kept)
          setParam(sp, SmpStretchMode, 1);
          setParam(sp, SmpTimeStretch, std::clamp(stretchMul, 0.25, 4.0));
          setParam(sp, SmpPitchShift, std::clamp(params->i32(100), -1200, 1200));
        }
        if (params->u32(96) != 0) c.report.counts["sampler stretch-to-time (approximated)"]++;
      }
      ChannelIns ins = decodeChannelIns(evs);
      // FL plays a sampler voice to the end of the sample unless the volume
      // envelope is on, which then follows the note length.
      setParam(sp, SmpOneShot, 1);
      applyInsToSampler(ins, sp);
      if (ins.env[ChannelIns::Pitch].enabled && ins.env[ChannelIns::Pitch].amount != 0)
        c.report.warn("Channel '" + ch.name + "': pitch envelope not imported");
      normalizePluginState(sp);
    }
    s.channels.push_back(ch);
    c.channelIds[idx] = ch.id;
  }
  // Layers: children by FL index → Gridline channels.
  int layers = 0, layerLinks = 0;
  for (const auto& [idx, kids] : layerKids) {
    auto lid = c.channelIds.find(idx);
    Channel* l = lid == c.channelIds.end() ? nullptr : s.channel(lid->second);
    if (!l) continue;
    for (int k : kids) {
      auto it = c.channelIds.find(k);
      if (it != c.channelIds.end() && it->second != l->id && l->layer.size() < static_cast<size_t>(kMaxLayerChildren)) {
        l->layer.push_back(it->second);
        ++layerLinks;
      }
    }
    int fl = layerFlags.count(idx) ? layerFlags[idx] : 0;
    l->layerMode = (fl & 1) ? 1 : 0;
    if (fl & 2) c.report.warn("Layer '" + l->name + "': crossfade layering plays all children");
    ++layers;
  }
  c.report.counts["channels"] = static_cast<int>(s.channels.size());
  auto count = [&](const char* k, int v) {
    if (v) c.report.counts[k] = v;
  };
  count("channels switched off (muted)", switchedOff);
  count("layer channels", layers);
  count("layer children", layerLinks);
  count("channels with cut groups", cutGroups);
  count("plugin self-cut groups ignored", selfCutIgnored);
  count("channels with echo", echoes);
  count("arpeggiators", arps);
  count("mono channels", monos);
  count("porta channels", portas);
  count("channels with time shift", timeShifts);
  count("display groups", static_cast<int>(s.channelGroups.size()));
  int grouped = 0;
  for (const auto& ch : s.channels) grouped += ch.group >= 0;
  count("channels in display groups", grouped);
}

void finishChannels(Context& c) {
  // Plugin channels whose instrument became the Trio Synth: FL's channel volume
  // envelope (the instrument page) shapes those native instruments, so it
  // becomes the synth's amplitude envelope.
  for (const auto& [idx, evs] : c.sec.channels) {
    auto id = c.channelIds.find(idx);
    if (id == c.channelIds.end()) continue;
    Channel* ch = c.song.channel(id->second);
    if (!ch || ch->plugin.type != "gl.synth" || !c.channelPlugins.count(idx)) continue;
    ChannelIns ins = decodeChannelIns(evs);
    const ChannelIns::Env& v = ins.env[ChannelIns::Volume];
    if (!v.enabled) continue;
    using namespace pp;
    setParam(ch->plugin, SynAttack, std::clamp(v.attack, 0.001, 5.0));
    setParam(ch->plugin, SynDecay, std::clamp(v.decay + v.hold, 0.005, 10.0));
    setParam(ch->plugin, SynSustain, v.sustain);
    setParam(ch->plugin, SynRelease, std::clamp(v.release, 0.005, 10.0));
    normalizePluginState(ch->plugin);
    c.report.counts["plugin channels with channel envelope"]++;
  }
}

}  // namespace gl::flp
