// Gridline — J-2 tests: deep parity of the most-used instruments.
//   Trio Synth (3x Osc): oscillator shapes, HQ / classic, custom wave, AM, decoder
//   Drum Pads (FPC): 32 pads, data-text layers, envelopes, cut groups, decoder
//   Slicer (Slicex): region ends, keys, cut groups, delay, decoder (ltxt regions)
//   Multisampler (DirectWave): zone key / velocity mapping
//   Keys (FL Keys): piano models
// When FL Studio is installed, its render tests and shipped presets are decoded
// as well (counts printed; skipped otherwise).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpPresets.h"
#include "engine/Dsp.h"
#include "engine/Generators.h"
#include "engine/Slicing.h"
#include "flp_gen2/Gen2Test.h"
#include "packs/Packs.h"
#include "packs/samplers/samplers.h"

using namespace gl;
using namespace gl::pp;
using gen2test::Bytes;

namespace {

constexpr double kSr = 44100.0;
namespace fs = std::filesystem;

struct Buf {
  std::vector<float> l, r;
};

ProcessContext ctx() {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  return c;
}

PluginState st(const std::string& type, std::initializer_list<std::pair<int, float>> set = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

Buf run(const PluginState& s, const ChannelAux& aux, std::vector<std::pair<double, float>> notes, size_t n, size_t offAt = SIZE_MAX) {
  auto g = createGenerator(s.type);
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  if (!g) return out;
  uint32_t id = 1;
  for (auto [p, v] : notes) g->noteOn(id++, p, v, 0);
  for (size_t pos = 0; pos < n; pos += 256) {
    int k = static_cast<int>(std::min<size_t>(256, n - pos));
    if (offAt >= pos && offAt < pos + static_cast<size_t>(k))
      for (uint32_t i = 1; i < id; ++i) g->noteOff(i, static_cast<int>(offAt - pos));
    g->process(s, aux, ctx(), out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

double rms(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  b = std::min(b, v.size());
  double acc = 0;
  for (size_t i = a; i < b; ++i) acc += static_cast<double>(v[i]) * v[i];
  return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
}
bool finite(const Buf& b) {
  for (size_t i = 0; i < b.l.size(); ++i)
    if (!std::isfinite(b.l[i]) || !std::isfinite(b.r[i])) return false;
  return true;
}

SamplePtr makeSample(size_t frames, double hz, double amp = 0.5, double dc = 0) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(frames), s->right.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    s->left[i] = static_cast<float>(amp * std::sin(2 * dsp::kPi * hz * static_cast<double>(i) / kSr) + dc);
    s->right[i] = s->left[i];
  }
  return s;
}

// High-frequency energy (first difference) relative to the signal: aliasing / brightness proxy.
double roughness(const std::vector<float>& v) {
  double d = 0, e = 0;
  for (size_t i = 1; i < v.size(); ++i) d += std::fabs(v[i] - v[i - 1]), e += std::fabs(v[i]);
  return e > 0 ? d / e : 0;
}

// ---------------------------------------------------------------- Trio Synth
void testTrio() {
  // Rounded saw: fast charge to +1, flat top from 0.464, low for the last sixth.
  {
    dsp::Noise nz;
    auto at = [&](double ph) {
      dsp::Osc o;
      o.phase = ph;
      return o.next(dsp::Shape::RoundSaw, 1e-6, nz);
    };
    expect(at(0.0) < -0.99 && at(0.2) > 0.99 && at(0.6) > 0.99 && at(0.9) < -0.99, "rounded saw: reference shape (fast rise, flat top, low last sixth)");
    expect(at(0.01) > -0.8, "rounded saw: steep capacitor-charge rise");
  }
  // A plain, unfiltered patch for oscillator checks.
  auto raw = [](std::initializer_list<std::pair<int, float>> extra) {
    PluginState s = st("gl.synth", {{SynOsc1Level, 1.f}, {SynOsc2Level, 0.f}, {SynOsc3Level, 0.f}, {SynCutoff, 20000.f}, {SynResonance, 0.f},
                                    {SynFilterEnv, 0.f}, {SynAttack, 0.001f}, {SynSustain, 1.f}, {SynDecay, 5.f}, {SynVolume, 1.f}});
    for (auto [k, v] : extra) s.params[static_cast<size_t>(k)] = v;
    return s;
  };
  ChannelAux none;
  // HQ (band-limited) vs classic (naive) saw at a high note: the classic one is rougher.
  {
    Buf hq = run(raw({{SynOsc1Shape, 2.f}, {SynHQ, 1.f}}), none, {{100, 1.f}}, 8192);
    Buf lq = run(raw({{SynOsc1Shape, 2.f}, {SynHQ, 0.f}}), none, {{100, 1.f}}, 8192);
    expect(rms(hq.l) > 0.05 && rms(lq.l) > 0.05, "trio: HQ and classic saws both sound");
    expect(roughness(lq.l) > roughness(hq.l) * 1.02, "trio: classic oscillators are not band-limited (rougher than HQ)");
  }
  // Custom wave: one cycle of the channel sample (a DC-shifted cycle shows up as DC).
  {
    ChannelAux aux;
    auto cyc = std::make_shared<Sample>();
    cyc->sampleRate = kSr;
    cyc->left.assign(64, 0.5f), cyc->right.assign(64, 0.5f);  // a flat +0.5 "cycle"
    aux.samples = {cyc};
    Buf b = run(raw({{SynOsc1Shape, 7.f}}), aux, {{60, 1.f}}, 4096);
    double mean = 0;
    for (size_t i = 1024; i < 4096; ++i) mean += b.l[i];
    mean /= 3072.0;
    expect(mean > 0.05, "trio: custom-wave oscillator plays the channel sample's cycle");
    Buf silent = run(raw({{SynOsc1Shape, 7.f}}), none, {{60, 1.f}}, 4096);
    expect(rms(silent.l) < 1e-6, "trio: custom wave without a sample is silent");
  }
  // AM: osc 3 multiplies osc 1+2 by (1 - level·osc3) — a slow osc 3 square gates the sound.
  {
    PluginState am = raw({{SynOsc1Shape, 0.f}, {SynOsc3Shape, 3.f}, {SynOsc3Level, 1.f}, {SynOsc3Coarse, -24.f}, {SynOsc3Mode, SynModeAM}});
    Buf b = run(am, none, {{60, 1.f}}, 16384);
    PluginState plain = raw({{SynOsc1Shape, 0.f}});
    Buf p = run(plain, none, {{60, 1.f}}, 16384);
    expect(finite(b) && rms(b.l) > 0.02 && std::fabs(rms(b.l) - rms(p.l)) > 0.01, "trio: osc 3 amplitude modulation changes the level envelope");
  }
  // Stereo detune / phase offsets make the sides differ; invert flips polarity.
  {
    Buf b = run(raw({{SynOsc1Detune, 30.f}}), none, {{60, 1.f}}, 8192);
    double diff = 0;
    for (size_t i = 0; i < b.l.size(); ++i) diff += std::fabs(b.l[i] - b.r[i]);
    expect(diff / 8192.0 > 0.01, "trio: stereo detune widens the oscillator");
    Buf a = run(raw({{SynOsc1Shape, 0.f}}), none, {{60, 1.f}}, 2048);
    Buf inv = run(raw({{SynOsc1Shape, 0.f}, {SynOsc1Invert, 1.f}}), none, {{60, 1.f}}, 2048);
    bool flipped = true;
    for (size_t i = 0; i < 2048; ++i) flipped = flipped && std::fabs(a.l[i] + inv.l[i]) < 1e-4;
    expect(flipped, "trio: invert flips the oscillator's polarity");
  }
  // Decoder: HQ byte (v13+) and custom-shape oscillators keep the channel sample.
  {
    Bytes b;
    b.i32(13);
    // 21 dwords in control-tag order (osc 1: pan, shape, coarse, fine, phase, detune; osc 2/3 mix + the same; phase rand).
    const int vals[21] = {0, 6, 0, 0, 0, 0, 64, 0, 0, -12, 0, 0, 0, 32, 0, 0, 12, 0, 0, 0, 10};
    for (int v : vals) b.i32(v);
    b.u8(0).u8(1).u8(0).u8(1);  // invert osc 2, osc 3 = AM
    b.u8(1);                    // HQ
    gen2test::Run r;
    gen2test::Bytes path;
    path.str("C:\\samples\\cycle.wav");
    r.add(196, path.b);
    gen2test::decode(r, "3x Osc", b.b);
    expect(r.ok && r.m.glType == "gl.synth", "3x Osc decode (v13)");
    expect(gen2test::param(r, SynHQ) == 1 && gen2test::param(r, SynOsc1Shape) == 7 && gen2test::param(r, SynOsc2Invert) == 1 &&
               gen2test::param(r, SynOsc3Mode) == SynModeAM,
           "3x Osc: HQ, custom shape, invert, AM");
    expect(r.m.state.data.find("cycle.wav") != std::string::npos, "3x Osc: custom shape keeps the channel sample");
  }
}

// ---------------------------------------------------------------- Drum Pads
void testPads() {
  ChannelAux aux;
  PluginState s = st("gl.pads");
  // Bank B pad 20 (index 19) on note 70 with a sample in layer 1, plus layers 5 / 6 in the data text.
  s.data = setPadSamplePath("", 19, 0, "/x/b1.wav");
  s.params[static_cast<size_t>(padE(19, PadNote))] = 70;
  s.params[static_cast<size_t>(padL(19, 0, LayVelHigh))] = 40;
  s.data = setPadSamplePath(s.data, 19, 5, "/x/b6.wav");
  PadLayerX x;
  x.lo = 41, x.hi = 127, x.level = 0.5f;
  s.data = padDataSet(s.data, "S20.6", formatPadLayer(x));
  expect(padLayerCount(s.data, 19) == 6, "pads: data-text layer count");
  // Aux by hand (the sample cache cannot load /x/..): soft + loud samples.
  aux.samples.assign(static_cast<size_t>(kPadSampleSlots), nullptr);
  aux.samples[static_cast<size_t>(padLayerSampleX(19, 0))] = makeSample(8000, 200, 0.5);
  aux.samples[static_cast<size_t>(padLayerSampleX(19, 5))] = makeSample(8000, 2000, 0.5);
  {
    ChannelAux prepared;
    prepareBuiltinAux(s, prepared);  // builds the pad envelope / layer tables
    aux.custom = prepared.custom;
  }
  Buf soft = run(s, aux, {{70, 0.2f}}, 4000), loud = run(s, aux, {{70, 0.9f}}, 4000);
  expect(rms(soft.l) > 0.01 && rms(loud.l) > 0.01, "pads: bank B pad plays (note 70)");
  expect(roughness(loud.l) > roughness(soft.l) * 2, "pads: layer 6 (data text) answers loud hits, layer 1 soft ones");
  // Volume envelope: 1 → 0 over 0.05 s silences the pad early.
  {
    PluginState e = s;
    PadEnvelope env;
    env.on = true, env.n = 2, env.v[0] = 1, env.t[1] = 0.05f, env.v[1] = 0, env.ten[1] = 0;
    e.data = padDataSet(e.data, "E20.v", formatPadEnvelope(env));
    ChannelAux p2;
    prepareBuiltinAux(e, p2);
    ChannelAux a2 = aux;
    a2.custom = p2.custom;
    Buf b = run(e, a2, {{70, 0.2f}}, 6000);
    expect(rms(b.l, 0, 1000) > 0.01 && rms(b.l, 3000, 6000) < 1e-4, "pads: multi-point volume envelope fades the pad out");
    PadEnvelope back;
    expect(parsePadEnvelope(formatPadEnvelope(env), back) && back.n == 2 && std::fabs(back.t[1] - 0.05f) < 1e-6, "pads: envelope text round trip");
  }
  // Pan envelope (0 = left) pans the pad.
  {
    PluginState e = s;
    PadEnvelope env;
    env.on = true, env.n = 1, env.v[0] = 0.0f;
    e.data = padDataSet(e.data, "E20.p", formatPadEnvelope(env));
    ChannelAux p2;
    prepareBuiltinAux(e, p2);
    ChannelAux a2 = aux;
    a2.custom = p2.custom;
    Buf b = run(e, a2, {{70, 0.2f}}, 4000);
    expect(rms(b.l, 500) > rms(b.r, 500) * 4, "pads: pan envelope pans the hit");
  }
  // FPC decoder: bank B pads, layers beyond 4, envelopes.
  {
    using gen2test::Run;
    auto chunk = [](Bytes& out, uint32_t id, const Bytes& payload) { out.u32(id).u32(static_cast<uint32_t>(payload.size())).u32(0).raw(payload.b); };
    Bytes stt;
    stt.u32(0x1E84E403);
    for (int p = 0; p < 18; ++p) {
      Bytes pad, settings;
      settings.i32(100).i32(0).i32(0).i32(40 + p).i32(0).i32(-1).i32(-1).i32(0).u32(0x4D91A1).zeros(28);
      chunk(pad, 0x64, settings);
      if (p == 17) {  // bank B pad 18: six layers + a volume envelope (on, sustain at point 1)
        for (int l = 0; l < 6; ++l) {
          Bytes layer, path, ls;
          path.utf16("/x/l" + std::to_string(l) + ".wav", false);
          chunk(layer, 0x12C, path);
          ls.i32(127).i32(0).i32(l * 21).i32(l * 21 + 20).i32(1).i32(0).i32(-1);
          chunk(layer, 0x12D, ls);
          chunk(pad, 0x69, layer);
        }
        Bytes kind, env;
        kind.i32(0).i32(0);
        chunk(pad, 0x67, kind);
        env.i32(1).i32(3).i32(3);
        env.f64(0).f64(0).f32(0).u32(0);
        env.f64(0.01).f64(1).f32(0.2f).u32(0x01000000);
        env.f64(0.5).f64(0).f32(0.2f).u32(0x01000000);
        env.i32(0).i32(-1).i32(1).i32(-1).i32(-1).i32(128).i32(128).i32(0).i32(128);
        chunk(pad, 0x68, env);
      }
      chunk(stt, 2, pad);
    }
    Run r;
    gen2test::decode(r, "FPC", stt.b);
    expect(r.ok && r.m.exact, "fpc: 18-pad kit (bank B) decodes exactly");
    expect(gen2test::param(r, padE(17, PadNote)) == 57, "fpc: bank B pad keeps its note");
    expect(padSamplePath(r.m.state.data, 17, 5) == "/x/l5.wav" && padLayerCount(r.m.state.data, 17) == 6, "fpc: six layers kept");
    PadEnvelope e;
    expect(parsePadEnvelope(padDataGet(r.m.state.data, "E18.v"), e) && e.on && e.n == 3 && e.sustain == 1 && std::fabs(e.t[2] - 0.5f) < 1e-5,
           "fpc: volume envelope (points, sustain) decoded");
    expect(padDataGet(r.m.state.data, "C1") == "A1914D", "fpc: pad colour (BGR → RGB)");
  }
}

// ---------------------------------------------------------------- Slicer
void testSlicer() {
  ChannelAux aux;
  aux.samples = {makeSample(44100, 300, 0.5)};
  auto base = [](std::initializer_list<std::pair<int, float>> extra) {
    PluginState s = st("gl.slicer", {{SlMode, static_cast<float>(SlModeManual)}, {slMarker(0), 0.f}, {slMarker(1), 0.5f}, {SlDeclick, 0.f}});
    for (auto [k, v] : extra) s.params[static_cast<size_t>(k)] = v;
    return s;
  };
  // Region end: slice 1 stops at 25 % instead of the next marker (50 %).
  {
    Buf full = run(base({}), aux, {{60, 1.f}}, 30000);
    Buf cut = run(base({{slParam2(0, SliceEnd), 0.25f}}), aux, {{60, 1.f}}, 30000);
    expect(rms(full.l, 12000, 20000) > 0.05 && rms(cut.l, 12000, 20000) < 1e-4, "slicer: region end point stops the slice early");
  }
  // Key assignment: slice 2 on note 72.
  {
    Buf b = run(base({{slParam2(1, SliceKey), 72.f}}), aux, {{72, 1.f}}, 4000);
    expect(rms(b.l) > 0.05, "slicer: a region assigned to a key plays on it");
  }
  // Delay: silence before the region starts.
  {
    Buf b = run(base({{slParam2(0, SliceDelay), 0.05f}}), aux, {{60, 1.f}}, 6000);
    expect(rms(b.l, 0, 2000) < 1e-6 && rms(b.l, 3000, 6000) > 0.05, "slicer: region delay");
  }
  // Cut groups: two regions in group 1 cut each other.
  {
    PluginState s = base({{slParam2(0, SliceCut), 1.f}, {slParam2(1, SliceCut), 1.f}});
    ChannelAux half;  // first half a tone, second half silence: only region 1 makes sound
    auto hs = std::make_shared<Sample>(*makeSample(44100, 300, 0.5));
    for (size_t i = 22050; i < 44100; ++i) hs->left[i] = hs->right[i] = 0.f;
    half.samples = {hs};
    auto g = createGenerator("gl.slicer");
    Buf out;
    out.l.assign(8000, 0.f), out.r.assign(8000, 0.f);
    g->noteOn(1, 60, 1.f, 0);
    g->process(s, half, ctx(), out.l.data(), out.r.data(), 2000);
    g->noteOn(2, 61, 1.f, 0);
    g->process(s, half, ctx(), out.l.data() + 2000, out.r.data() + 2000, 6000);
    PluginState free = base({});
    auto g2 = createGenerator("gl.slicer");
    Buf o2;
    o2.l.assign(8000, 0.f), o2.r.assign(8000, 0.f);
    g2->noteOn(1, 60, 1.f, 0);
    g2->process(free, half, ctx(), o2.l.data(), o2.r.data(), 2000);
    g2->noteOn(2, 61, 1.f, 0);
    g2->process(free, half, ctx(), o2.l.data() + 2000, o2.r.data() + 2000, 6000);
    expect(rms(out.l, 3000, 8000) < 1e-4 && rms(o2.l, 3000, 8000) > 0.1, "slicer: regions in one cut group stop each other");
  }
  // Slicex decoder: embedded RIFF with cues + "ltxt" region lengths.
  {
    Bytes riff, body;
    body.str("WAVE", false);
    Bytes fmt;
    fmt.u16(1).u16(1).u32(44100).u32(88200).u16(2).u16(16);
    body.str("fmt ", false).u32(16).raw(fmt.b);
    body.str("data", false).u32(2000).zeros(2000);  // 1000 frames
    Bytes cue;
    cue.u32(2);
    cue.u32(1).u32(0).str("data", false).u32(0).u32(0).u32(0);
    cue.u32(2).u32(0).str("data", false).u32(0).u32(0).u32(500);
    body.str("cue ", false).u32(static_cast<uint32_t>(cue.size())).raw(cue.b);
    Bytes adtl;
    adtl.str("adtl", false);
    adtl.str("ltxt", false).u32(20).u32(1).u32(200).str("rgn ", false).u32(0).u32(0);
    adtl.str("ltxt", false).u32(20).u32(2).u32(500).str("rgn ", false).u32(0).u32(0);
    body.str("LIST", false).u32(static_cast<uint32_t>(adtl.size())).raw(adtl.b);
    riff.str("RIFF", false).u32(static_cast<uint32_t>(body.size())).raw(body.b);
    Bytes stt;
    stt.u16(3).u16(6);
    for (int i = 0; i < 22; ++i) stt.i32(i == 2 ? 12800 : i == 3 ? 256 : i == 4 ? 128 : i == 6 || i == 7 ? 256 : 0);
    stt.u8(0).u8(0).u8(0).u8(0);  // 0x5C..0x5F (0x5F = 0: sample embedded)
    stt.u8(8).str("loop.wav", false);
    stt.u32(static_cast<uint32_t>(riff.size())).raw(riff.b);
    stt.zeros(0x18C).u32(2);
    for (int rg = 0; rg < 2; ++rg) stt.i32(0).i32(0).i32(0).i32(rg == 1 ? 16000 : 8000).i32(0).i32(12800).i32(128).i32(128).i32(0).i32(12800).i32(0).i32(0).i32(12800).i32(0).i32(0).i32(0).i32(12800);
    gen2test::Run r;
    gen2test::decode(r, "Slicex", stt.b);
    expect(r.ok && r.m.glType == "gl.slicer", "slicex decode");
    expect(std::fabs(gen2test::param(r, slMarker(1)) - 0.5f) < 1e-4 && std::fabs(gen2test::param(r, slParam2(0, SliceEnd)) - 0.2f) < 1e-4 &&
               gen2test::param(r, slParam2(1, SliceEnd)) < 0,
           "slicex: region 1 ends at its ltxt length, region 2 runs to the end");
    expect(std::fabs(gen2test::param(r, slParam(1, SliceGain)) - 6.0206f) < 0.01 && std::fabs(gen2test::param(r, SlModX) - 0.5f) < 1e-4,
           "slicex: region volume (16000 = +6 dB) and modulation X");
  }
}

// ---------------------------------------------------------------- Multisampler
void testMulti() {
  namespace smp = gl::packs::samplers;
  smp::ZoneMap m;
  smp::Zone lo, hi;
  lo.path = "tone:keys:60:0", lo.lo = 0, lo.hi = 59, lo.root = 48;
  hi.path = "tone:keys:72:0", hi.lo = 60, hi.hi = 127, hi.root = 72, hi.vlo = 64;
  m.zones = {lo, hi};
  PluginState s = st("gl.multisampler");
  s.data = smp::serializeZoneMap(m);
  smp::ZoneMap back = smp::parseZoneMap(s.data);
  expect(back.zones.size() == 2 && back.zones[1].vlo == 64 && back.zones[0].hi == 59, "multisampler: zone map round trip");
  ChannelAux aux;
  expect(packs::prepareAux(s, aux), "multisampler: aux prepared");
  Buf low = run(s, aux, {{48, 0.9f}}, 4000);
  Buf highSoft = run(s, aux, {{72, 0.3f}}, 4000), highLoud = run(s, aux, {{72, 0.9f}}, 4000);
  expect(rms(low.l) > 0.01, "multisampler: low zone plays");
  expect(rms(highSoft.l) < 1e-5 && rms(highLoud.l) > 0.01, "multisampler: velocity range gates the high zone");
}

// ---------------------------------------------------------------- Keys
void testKeys() {
  ChannelAux none;
  for (int model : {4, 5}) {
    Buf b = run(st("gl.keys", {{KyModel, static_cast<float>(model)}}), none, {{60, 0.8f}}, 44100, 20000);
    char name[64];
    std::snprintf(name, sizeof name, "keys: piano model %d sounds, finite, decays after release", model);
    expect(finite(b) && rms(b.l, 0, 4000) > 0.01 && rms(b.l, 40000) < rms(b.l, 0, 4000) * 0.1, name);
  }
  Buf soft = run(st("gl.keys", {{KyModel, 4.f}, {KyHardness, 0.f}}), none, {{60, 0.8f}}, 8192);
  Buf hard = run(st("gl.keys", {{KyModel, 4.f}, {KyHardness, 1.f}}), none, {{60, 0.8f}}, 8192);
  expect(roughness(hard.l) > roughness(soft.l), "keys: harder hammers sound brighter");
}

// ---------------------------------------------------------------- FL Studio files (optional)
std::string flData() {
  const char* env = std::getenv("GRIDLINE_FL_DATA");
  std::string d = env && *env ? env : "/Applications/FL Studio 2026.app/Contents/Resources/FL";
  std::error_code ec;
  return fs::is_directory(d + "/Data", ec) ? d : std::string();
}

void testFlFiles() {
  const std::string fl = flData();
  if (fl.empty()) {
    std::printf("# FL Studio not installed: render tests / presets skipped\n");
    return;
  }
  struct RT {
    const char* file;
    const char* glType;
  };
  const RT rts[] = {{"3OSC.flp", "gl.synth"}, {"FPC.flp", "gl.pads"}, {"Slicex.flp", "gl.slicer"}, {"Directwave.flp", "gl.multisampler"}, {"FL Keys.flp", "gl.keys"}};
  for (const RT& rt : rts) {
    Song song;
    flp::Report rep;
    std::string err;
    const std::string path = fl + "/Data/System/Render tests/" + rt.file;
    bool ok = importFlp(path, song, rep, &err, fl);
    bool found = false;
    for (const auto& c : song.channels)
      if (c.plugin.type == rt.glType) found = true;
    char name[96];
    std::snprintf(name, sizeof name, "render test %s imports to %s", rt.file, rt.glType);
    expect(ok && found, name);
  }
  // Shipped presets of the five plugins: converted / exact counts.
  const char* plugins[5] = {"3x Osc", "FPC", "Slicex", "DirectWave", "FL Keys"};
  for (const char* pl : plugins) {
    int conv = 0, exact = 0, total = 0;
    for (const char* sub : {"/Data/Patches/Plugin presets/Generators/", "/Data/Patches/Channel presets/"}) {
      std::error_code ec;
      fs::path dir = fs::path(fl + sub) / pl;
      if (!fs::is_directory(dir, ec)) continue;
      for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".fst") continue;
        ++total;
        flp::PresetResult r;
        if (flp::importFlPreset(it->path().string(), r, fl) && r.kind == flp::PresetResult::Plugin) {
          ++conv;
          if (r.exact) ++exact;
        }
      }
    }
    std::printf("# presets %-10s %3d files, %3d converted, %3d exact\n", pl, total, conv, exact);
    char name[96];
    std::snprintf(name, sizeof name, "%s presets convert", pl);
    expect(total == 0 || conv > 0, name);
  }
}

}  // namespace

int main() {
  testTrio();
  testPads();
  testSlicer();
  testMulti();
  testKeys();
  testFlFiles();
  return finish();
}
