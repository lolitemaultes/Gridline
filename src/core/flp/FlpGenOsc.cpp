// Gridline — FLP import: 3x Osc → Trio Synth ("gl.synth").
//
// State layout (observed; version dword first):
//   dword  version (10, 11, 12, 13, 14)
//   dword  21 parameters in control-tag order: osc 1 pan, shape, coarse, fine,
//          stereo phase, stereo detune; osc 2 mix, pan, shape, coarse, fine,
//          stereo phase, stereo detune; osc 3 mix, pan, …; phase randomness
//          (version 10 stops before phase randomness)
//   byte   v12+: invert osc 1, invert osc 2, invert osc 3, osc 3 = AM
//   byte   v13+: high-quality oscillators
//   v14+:  3 pad bytes, a copy of the five channel envelope records (5 × 68 bytes)
//          and of the channel levels record (24 bytes; cutoff, resonance, type)
// Shapes: 0 sine, 1 triangle, 2 square, 3 saw, 4 rounded saw, 5 noise, 6 custom sample.
// Osc 1 has no mix control (full level); osc 2 / 3 mix 0..128 relative to it.
#include <algorithm>
#include <cmath>
#include "../PluginParams.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

using namespace gl::pp;

int shapeToTrio(int fl) {
  switch (fl) {
    case 0: return 0;  // sine
    case 1: return 1;  // triangle
    case 2: return 3;  // square
    case 3: return 2;  // saw
    case 4: return 5;  // rounded saw
    case 5: return 4;  // noise
    default: return 7;  // custom sample: one cycle of the channel sample
  }
}

// Channel filter type → Trio filter type (LP, LP24, BP, HP, notch).
int filterToTrio(int fl) {
  switch (fl) {
    case 2: return 2;          // BP
    case 3: return 3;          // HP
    case 4: return 4;          // band stop → notch
    case 5: case 7: return 1;  // LPx2 / SVF LPx2 → 24 dB
    default: return 0;         // fast LP, LP, SVF LP
  }
}

}  // namespace

// Applies the channel's envelopes / filter (FL applies them to native
// generators) to the Trio Synth's amp envelope, filter and LFO. Returns false
// when something could not be represented.
bool applyChannelToTrio(const ChanSettings& cs, PluginState& s, std::string* lost) {
  bool exact = true;
  auto lose = [&](const char* what) {
    exact = false;
    if (lost) *lost += (lost->empty() ? "" : ", ") + std::string(what);
  };
  const ChanEnv& vol = cs.env[EnvVol];
  if (vol.enabled) {
    setp(s, SynAttack, envSeconds(vol.attack));
    setp(s, SynDecay, std::max(0.005, envSeconds(vol.hold) * 0.25 + envSeconds(vol.decay)));
    setp(s, SynSustain, envSustain(vol.sustain));
    setp(s, SynRelease, envSeconds(vol.release));
    lose("volume envelope times (curve approximated)");
  } else {
    setp(s, SynAttack, 0.001);
    setp(s, SynDecay, 1.0);
    setp(s, SynSustain, 1.0);
    setp(s, SynRelease, 0.008);
  }
  const ChanFilter& f = cs.filter;
  const ChanEnv& mx = cs.env[EnvModX];
  bool filtered = !f.open() || (mx.enabled && mx.amount != 0);
  if (filtered) {
    setp(s, SynCutoff, std::clamp(cutoffHz(f.cutoff), 30.0, 20000.0));
    setp(s, SynResonance, std::clamp(f.reso / 256.0, 0.0, 1.0));
    setp(s, SynFilterType, filterToTrio(f.type));
    lose("channel filter (response approximated)");
  } else {
    setp(s, SynCutoff, 20000.0);
    setp(s, SynResonance, 0.0);
    setp(s, SynFilterType, 0);
  }
  if (mx.enabled && mx.amount != 0) {
    // Mod X sweeps the cutoff knob range (~11 octaves) by `amount`; the Trio's
    // filter envelope moves ±6 octaves at full amount.
    setp(s, SynFilterEnv, std::clamp(mx.amount / 128.0 * 11.0 / 6.0, -1.0, 1.0));
    setp(s, SynFilterDecay, std::max(0.005, envSeconds(mx.hold) * 0.25 + envSeconds(mx.decay)));
    if (mx.sustain > 8) lose("filter envelope sustain");
  } else {
    setp(s, SynFilterEnv, 0.0);
  }
  if (cs.env[EnvModY].enabled && cs.env[EnvModY].amount != 0) lose("resonance envelope");
  if (cs.env[EnvPitch].enabled && cs.env[EnvPitch].amount != 0) lose("pitch envelope");
  if (cs.env[EnvPan].enabled && cs.env[EnvPan].amount != 0) lose("panning envelope");
  // One LFO: the first channel LFO in use (volume, cutoff, pitch, pan).
  setp(s, SynLfoTarget, 0);
  struct Cand { int target, trio; double scale; };
  const Cand cands[] = {{EnvVol, 3, 1.0}, {EnvModX, 2, 1.0}, {EnvPitch, 1, 0.5}, {EnvPan, 4, 1.0}};
  int used = 0;
  for (const auto& c : cands) {
    const ChanEnv& e = cs.env[c.target];
    if (!e.lfoOn()) continue;
    if (used++ == 0) {
      setp(s, SynLfoTarget, c.trio);
      setp(s, SynLfoShape, std::clamp(e.lfoShape, 0, 2));
      setp(s, SynLfoRate, lfoHz(e.lfoSpeed));
      setp(s, SynLfoAmount, std::clamp(std::fabs(e.lfoAmount) / 128.0 * c.scale, 0.0, 1.0));
      lose("channel LFO (rate approximated)");
    } else {
      lose("additional channel LFOs");
    }
  }
  return exact;
}

namespace {

bool decode3xOsc(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  int p[21] = {0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0};
  int version = 0;
  if (b.n == 11 * 4) {
    // Early presets (FL 2.x): no version word, 11 values: osc 1 shape, coarse,
    // fine; osc 2 / 3 mix, shape, coarse, fine (no pan, phase or detune).
    static const int to[11] = {1, 2, 3, 6, 8, 9, 10, 13, 15, 16, 17};
    for (int i = 0; i < 11; ++i) p[to[i]] = b.i32(static_cast<size_t>(i) * 4);
  } else {
    if (!b.has(0, 4 + 20 * 4)) return false;
    version = b.i32(0);
    const int count = std::min(21, static_cast<int>((b.n - 4) / 4));
    for (int i = 0; i < count; ++i) p[i] = b.i32(4 + static_cast<size_t>(i) * 4);
  }
  bool inv[3] = {false, false, false}, am = false;
  if (version >= 12 && b.has(88, 4)) {
    for (int k = 0; k < 3; ++k) inv[k] = b.u8(88 + static_cast<size_t>(k)) != 0;
    am = b.u8(91) != 0;
  }
  // v13+: HQ byte after the switches (older versions always used the classic oscillators).
  const bool hq = version >= 13 && b.has(92, 1) ? b.u8(92) != 0 : false;
  PluginState s = base("gl.synth");
  const int shapeIdx[3] = {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape};
  const int coarseIdx[3] = {SynOsc1Coarse, SynOsc2Coarse, SynOsc3Coarse};
  const int fineIdx[3] = {SynOsc1Fine, SynOsc2Fine, SynOsc3Fine};
  const int levelIdx[3] = {SynOsc1Level, SynOsc2Level, SynOsc3Level};
  bool custom = false;
  for (int k = 0; k < 3; ++k) {
    // osc 1: tags 0..5 (pan, shape, coarse, fine, phase, detune); osc 2 / 3: mix then the same six.
    const int at = k == 0 ? 0 : 6 + (k - 1) * 7 + 1;
    const int mix = k == 0 ? 128 : p[at - 1];
    const int pan = p[at], shape = p[at + 1], coarse = p[at + 2], fine = p[at + 3], phase = p[at + 4], detune = p[at + 5];
    if (shape == 6) custom = true;
    setp(s, shapeIdx[k], shapeToTrio(shape));
    setp(s, levelIdx[k], std::clamp(mix / 128.0, 0.0, 1.0));
    setp(s, coarseIdx[k], std::clamp(coarse, -24, 24));
    setp(s, fineIdx[k], std::clamp(fine, -100, 100));
    setp(s, SynOsc1Pan + k, std::clamp(pan / 64.0, -1.0, 1.0));
    setp(s, SynOsc1Invert + k, inv[k] ? 1.0 : 0.0);
    setp(s, SynOsc1Phase + k, std::clamp(phase / 128.0, -0.5, 0.5));
    setp(s, SynOsc1Detune + k, std::clamp(detune, -50, 50));
  }
  setp(s, SynPhaseRand, std::clamp(p[20] / 64.0, 0.0, 1.0));
  setp(s, SynOsc3Mode, am ? SynModeAM : SynModeNormal);
  setp(s, SynHQ, hq ? 1.0 : 0.0);
  setp(s, SynOsc2Mod, 0);
  setp(s, SynNoise, 0.0);
  setp(s, SynVelCutoff, 0.0);
  setp(s, SynKeyTrack, 0.0);
  setp(s, SynGlide, 0.001);
  setp(s, SynVoices, 32);
  setp(s, SynVolume, 1.0);
  // Channel envelopes / filter: the channel's own records, else the copy in the state.
  ChanSettings cs = channelSettings(events, b.sub(96, 5 * 68), b.sub(436, 24));
  std::string lost;
  bool chanExact = applyChannelToTrio(cs, s, &lost);
  // Custom-shape oscillators play one cycle of the channel's sample (event 196).
  std::string customPath;
  if (custom)
    for (const Event& e : events)
      if (e.id == 196) customPath = c.resolvePath(e.text());
  if (custom && customPath.empty()) lost += std::string(lost.empty() ? "" : ", ") + "custom-wave oscillator without a channel sample";
  s.data = customPath;
  m.glType = "gl.synth";
  m.state = s;
  m.exact = chanExact && (!custom || !customPath.empty());
  if (!lost.empty()) note(c, "3x Osc", lost);

  // Automation: FL parameter index = control tag.
  ParamTable t;
  auto shapeFn = [](double n) { return static_cast<double>(shapeToTrio(static_cast<int>(std::lround(n * 6)))); };
  for (int k = 0; k < 3; ++k) {
    int base = k == 0 ? 0 : 6 + (k - 1) * 7;  // osc 1: tags 0..5; osc 2: 6..12; osc 3: 13..19
    if (k > 0) t.lin(base, levelIdx[k], 0.0, 1.0), ++base;
    t.lin(base + 0, SynOsc1Pan + k, -1.0, 1.0);
    t.fn(base + 1, shapeIdx[k], shapeFn);
    t.lin(base + 2, coarseIdx[k], -24.0, 24.0);
    t.lin(base + 3, fineIdx[k], -100.0, 100.0);
    t.lin(base + 4, SynOsc1Phase + k, -0.5, 0.5);
    t.lin(base + 5, SynOsc1Detune + k, -50.0, 50.0);
    // Control ranges: level 0..128, pan -64..64, shape 0..6, coarse ±24, fine ±100, phase ±64, detune ±50.
    if (k > 0) t.range(base - 1, 0, 128);
    t.range(base + 0, -64, 64), t.range(base + 1, 0, 6), t.range(base + 2, -24, 24);
    t.range(base + 3, -100, 100), t.range(base + 4, -64, 64), t.range(base + 5, -50, 50);
  }
  t.lin(20, SynPhaseRand, 0.0, 1.0);
  t.range(20, 0, 64);
  t.install(m);
  return true;
}

}  // namespace

void registerOscDecoders(std::map<std::string, PluginDecoder>& d) { d["3x Osc"] = decode3xOsc; }

}  // namespace gl::flp::gen
