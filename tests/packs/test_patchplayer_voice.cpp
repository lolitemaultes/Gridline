// Gridline: the Patch Player's voice against curves computed here from the format's rules:
// volume envelope (attack, decay to sustain, release, in decibels), modulation envelope and
// LFOs to pitch and level, the low-pass cutoff in absolute cents, velocity to level through the
// default modulator, exclusive classes, stereo pairs, sustain pedal, block-size invariance.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "patchplayer_rig.h"

using namespace gl;
using namespace gl::packs::samplers;
using namespace pptest;
using namespace sf2t;

namespace {

// A bank with one preset playing one looped sample through one zone set up by `z`.
std::string oneZone(const std::string& name, const std::vector<int16_t>& pcm, sf2t::Zone z, uint32_t loopS = 0, uint32_t loopE = 0) {
  Bank b;
  b.samples = {sf2t::Sample{"S", pcm, {}, {}, 44100, 60, 0, loopS, loopE ? loopE : static_cast<uint32_t>(pcm.size()), 1, 0}};
  z.g(SampleModes, 1).g(SampleId, 0);
  b.insts = {Inst{"I", {z}}};
  b.presets = {Preset{"P", 0, 0, {sf2t::Zone{}.g(Instrument, 0)}}};
  return writeBank(b, name);
}

double concave(double x) { return x <= 0 ? 0 : x >= 1 ? 1 : std::min(1.0, -(5.0 / 6.0) * std::log10(1.0 - x)); }

}  // namespace

int main() {
  gl::test::hermeticTestEnv("ppvoice");
  const auto dcS = dc(4410, 16384);  // 0.5
  // ------------------------------------------------------------ volume envelope
  {
    // Attack 0.5 s (-1200 timecents), hold none, decay 1 s per 100 dB, sustain 20 dB down,
    // release 1 s per 100 dB.
    auto p = oneZone("env.sf2", dcS, sf2t::Zone{}.g(AttackVolEnv, -1200).g(DecayVolEnv, 0).g(SustainVolEnv, 200).g(ReleaseVolEnv, 0));
    Rig r(p);
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 44100);
    auto at = [&](double t) { return L[static_cast<size_t>(t * 44100)]; };
    expect(std::fabs(at(0.25) - 0.25f) < 0.01f, "attack rises linearly (half way at half the time)");
    expect(std::fabs(at(0.6) - 0.5f * static_cast<float>(std::pow(10.0, -100.0 / 200.0))) < 0.01f, "decay falls 10 dB in 0.1 s (100 dB per second)");
    expect(std::fabs(at(0.95) - 0.05f) < 0.003f, "sustain 20 dB down");
    r.gen->noteOff(1, 0);
    std::vector<float> L2, R2;
    r.run(L2, R2, 22050);
    expect(std::fabs(L2[4410] - 0.05f * static_cast<float>(std::pow(10.0, -100.0 / 200.0))) < 0.002f, "release falls 10 dB in 0.1 s");
    r.run(L2, R2, 44100);
    expect(!r.gen->active(), "release ends the voice");
    // Key to decay: two octaves up halves... (keynumToVolEnvDecay 100 timecents per key: 12 keys up
    // = 1200 timecents shorter = half the decay time).
    auto pk = oneZone("keydecay.sf2", dcS, sf2t::Zone{}.g(AttackVolEnv, -12000).g(DecayVolEnv, 0).g(SustainVolEnv, 1000).g(KeyToVolEnvDecay, 100));
    Rig k(pk);
    k.gen->noteOn(1, 72, 1.f, 0);
    std::vector<float> L3, R3;
    k.run(L3, R3, 44100);
    const double want = 0.5 * std::pow(10.0, -(1000.0 * 0.1 / 0.5) / 200.0);  // decay 0.5 s per 100 dB, read at 0.1 s
    expect(std::fabs(L3[4410] - want) < 0.004, "key number shortens the decay");
  }
  // ------------------------------------------------------------ velocity to level (default modulator)
  {
    auto p = oneZone("vel.sf2", dcS, sf2t::Zone{}.g(AttackVolEnv, -12000));
    Rig a(p), b(p);
    a.gen->noteOn(1, 60, 1.f, 0);
    b.gen->noteOn(1, 60, 64.f / 127.f, 0);
    std::vector<float> La, Ra, Lb, Rb;
    a.run(La, Ra, 4410);
    b.run(Lb, Rb, 4410);
    const double want = std::pow(10.0, -(960.0 * concave(1.0 - 64.0 / 127.0)) * 0.4 / 200.0);
    expect(std::fabs(Lb[3000] / La[3000] - want) < 0.005, "velocity 64 through the concave curve");
    Rig c(p);
    c.set(PpVelocity, PpVelFixed);
    c.gen->noteOn(1, 60, 0.2f, 0);
    std::vector<float> Lc, Rc;
    c.run(Lc, Rc, 4410);
    expect(std::fabs(Lc[3000] - La[3000]) < 1e-4f, "fixed velocity plays at full level");
  }
  // ------------------------------------------------------------ attenuation and pan
  {
    auto p = oneZone("att.sf2", dcS, sf2t::Zone{}.g(AttackVolEnv, -12000).g(Attenuation, 150).g(Pan, 250));
    Rig r(p);
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 4410);
    const double g = 0.5 * std::pow(10.0, -150.0 * 0.4 / 200.0);
    expect(std::fabs(R[3000] - g) < 1e-3 && std::fabs(L[3000] - g * 0.5) < 1e-3, "attenuation (0.4 scale) and pan half right");
  }
  // ------------------------------------------------------------ pitch: key, root, tuning, mod envelope, vibrato
  {
    const auto sn = sine(4410, 44, 16000);  // 440 Hz at 44.1 kHz
    auto p = oneZone("pitch.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(RootKey, 69));
    Rig r(p);
    r.gen->noteOn(1, 81, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 8820);
    expect(std::fabs(freqAt(L, 1000, 8000) - 880.0) < 3.0, "an octave above the root plays at twice the rate");
    auto pt = oneZone("tune.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(RootKey, 69).g(CoarseTune, 7).g(FineTune, -20).g(ScaleTuning, 50));
    Rig t(pt);
    t.gen->noteOn(1, 81, 1.f, 0);  // 12 keys at 50 cents + 700 - 20 cents
    std::vector<float> Lt, Rt;
    t.run(Lt, Rt, 8820);
    expect(std::fabs(freqAt(Lt, 1000, 8000) - 440.0 * std::pow(2.0, (600.0 + 680.0) / 1200.0)) < 3.0, "coarse, fine and scale tuning");
    // Mod envelope to pitch: instant attack, long hold: +1200 cents while it holds.
    auto pm = oneZone("modenv.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(RootKey, 69).g(ModEnvToPitch, 1200).g(AttackModEnv, -12000).g(HoldModEnv, 2400));
    Rig m(pm);
    m.gen->noteOn(1, 69, 1.f, 0);
    std::vector<float> Lm, Rm;
    m.run(Lm, Rm, 8820);
    expect(std::fabs(freqAt(Lm, 1000, 8000) - 880.0) < 4.0, "mod envelope to pitch");
    // Vibrato LFO: 100 cents at 8.176 Hz (0 absolute cents): a quarter cycle in it sits 100 cents up.
    auto pv = oneZone("vib.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(RootKey, 69).g(VibLfoToPitch, 100).g(FreqVibLfo, 0));
    Rig v(pv);
    v.gen->noteOn(1, 69, 1.f, 0);
    std::vector<float> Lv, Rv;
    v.run(Lv, Rv, 22050);
    const double quarter = 44100.0 / 8.176 / 4.0;
    const double up = freqAt(Lv, static_cast<size_t>(quarter - 200), static_cast<size_t>(quarter + 200));
    const double down = freqAt(Lv, static_cast<size_t>(3 * quarter - 200), static_cast<size_t>(3 * quarter + 200));
    expect(std::fabs(up / down - std::pow(2.0, 200.0 / 1200.0)) < 0.02, "vibrato LFO to pitch, 100 cents each way");
    // Pitch wheel: default modulator, sensitivity = Bend range.
    Rig w(p);
    w.set(PpBendRange, 12);
    w.gen->midi(0xE0, 0x7F, 0x7F, 0);
    w.gen->noteOn(1, 69, 1.f, 0);
    std::vector<float> Lw, Rw;
    w.run(Lw, Rw, 8820);
    expect(std::fabs(freqAt(Lw, 1000, 8000) - 880.0) < 4.0, "pitch wheel up a full bend range (12 semitones)");
  }
  // ------------------------------------------------------------ mod LFO to level
  {
    auto p = oneZone("trem.sf2", dcS, sf2t::Zone{}.g(AttackVolEnv, -12000).g(ModLfoToVol, 60).g(FreqModLfo, 0));
    Rig r(p);
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 22050);
    const double quarter = 44100.0 / 8.176 / 4.0;
    expect(std::fabs(L[static_cast<size_t>(quarter)] - 0.5 * std::pow(10.0, 60.0 / 200.0)) < 0.02 &&
               std::fabs(L[static_cast<size_t>(3 * quarter)] - 0.5 * std::pow(10.0, -60.0 / 200.0)) < 0.01,
           "mod LFO to level, 6 dB each way");
  }
  // ------------------------------------------------------------ filter cutoff in absolute cents
  {
    const auto sn = sine(4410, 10, 16000);  // 100 Hz
    const double fcCents = 1200.0 * std::log2(400.0 / 8.176);
    auto open = oneZone("fopen.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000));
    auto low = oneZone("flow.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(FilterFc, static_cast<int>(std::lround(fcCents))));
    auto test = [&](const std::string& path, int key) {
      Rig r(path);
      r.gen->noteOn(1, key, 1.f, 0);
      std::vector<float> L, R;
      r.run(L, R, 22050);
      return rms(L, 8000, 22000);
    };
    // 400 Hz (the cutoff): 3 dB down; 1600 Hz (two octaves above): 1/sqrt(1 + 4^4).
    const float a0 = test(open, 84), a1 = test(low, 84), b0 = test(open, 108), b1 = test(low, 108);
    expect(std::fabs(a1 / a0 - 0.7071f) < 0.03f, "low-pass at its cutoff (cents) is 3 dB down");
    expect(std::fabs(b1 / b0 - 1.0f / std::sqrt(1.0f + 256.0f)) < 0.01f, "two octaves above the cutoff, 24 dB down");
    // Resonance: Q 120 centibels: the peak stands 12 dB over the pass band (9 dB over the level at
    // the cutoff a Q of 0 gives).
    auto res = oneZone("fres.sf2", sn, sf2t::Zone{}.g(AttackVolEnv, -12000).g(FilterFc, static_cast<int>(std::lround(fcCents))).g(FilterQ, 120));
    const float c1 = test(res, 84);
    expect(std::fabs(c1 / a0 - std::pow(10.0f, (12.0f - 3.01f) / 20.0f)) < 0.2f, "resonance in centibels");
  }
  // ------------------------------------------------------------ exclusive class, stereo pair, sustain pedal
  {
    Bank b;
    b.samples = {sf2t::Sample{"Quiet", dc(4410, 8192), {}, {}, 44100, 60, 0, 0, 4410, 1, 0},
                 sf2t::Sample{"Loud", dc(4410, 16384), {}, {}, 44100, 60, 0, 0, 4410, 1, 0},
                 sf2t::Sample{"L", dc(4410, 16384), {}, {}, 44100, 60, 0, 0, 4410, 4, 3},
                 sf2t::Sample{"R", dc(4410, -8192), {}, {}, 44100, 60, 0, 0, 4410, 2, 2}};
    Inst in{"Kit", {}};
    in.zones.push_back(sf2t::Zone{}.g(AttackVolEnv, -12000).g(SampleModes, 1).g(ReleaseVolEnv, -12000));
    in.zones.push_back(sf2t::Zone{}.range(KeyRange, 42, 42).g(Exclusive, 1).g(SampleId, 0));
    in.zones.push_back(sf2t::Zone{}.range(KeyRange, 46, 46).g(Exclusive, 1).g(SampleId, 1));
    in.zones.push_back(sf2t::Zone{}.range(KeyRange, 60, 60).g(Pan, -500).g(SampleId, 2));
    in.zones.push_back(sf2t::Zone{}.range(KeyRange, 60, 60).g(Pan, 500).g(SampleId, 3));
    in.zones.push_back(sf2t::Zone{}.range(KeyRange, 70, 70).g(SampleId, 1));
    b.insts = {in};
    b.presets = {Preset{"Kit", 0, 0, {sf2t::Zone{}.g(Instrument, 0)}}};
    auto p = writeBank(b, "kit.sf2");
    Rig r(p);
    r.gen->noteOn(1, 42, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 2000);
    r.gen->noteOn(2, 46, 1.f, 0);
    r.run(L, R, 2000);
    expect(std::fabs(L[1500] - 0.25f) < 1e-3f && std::fabs(L[3500] - 0.5f) < 1e-3f, "exclusive class: the open hat cuts the closed one");
    Rig s(p);
    s.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> Ls, Rs;
    s.run(Ls, Rs, 2000);
    expect(std::fabs(Ls[1500] - 0.5f) < 1e-3f && std::fabs(Rs[1500] + 0.25f) < 1e-3f, "stereo pair: left and right samples on their own sides");
    // Sustain pedal holds a released note until the pedal goes up.
    Rig h(p);
    h.gen->midi(0xB0, 64, 127, 0);
    h.gen->noteOn(1, 70, 1.f, 0);
    std::vector<float> Lh, Rh;
    h.run(Lh, Rh, 2000);
    h.gen->noteOff(1, 0);
    h.run(Lh, Rh, 4000);
    const float held = std::fabs(Lh[5000]);
    h.gen->midi(0xB0, 64, 0, 0);
    h.run(Lh, Rh, 4000);
    expect(held > 0.4f && std::fabs(Lh.back()) < 1e-3f, "sustain pedal holds, then releases");
  }
  // ------------------------------------------------------------ block size, extremes
  {
    const auto sn = sine(4410, 44, 16000);
    auto p = oneZone("blocks.sf2", sn,
                     sf2t::Zone{}.g(RootKey, 69).g(VibLfoToPitch, 30).g(ModLfoToFc, 600).g(FilterFc, 9000).g(FilterQ, 80).g(ModEnvToFc, 2000).g(ModLfoToVol, 20)
                         .g(AttackVolEnv, -3000).g(ReleaseVolEnv, -2000));
    auto seq = [&](int block) {
      Rig r(p);
      r.set(PpGlide, 50);
      std::vector<float> L, R;
      r.gen->noteOn(1, 60, 0.9f, 0);
      r.run(L, R, 3000, block);
      r.gen->noteOn(2, 67, 0.5f, 0);
      r.gen->midi(0xE0, 0, 0x50, 0);
      r.run(L, R, 3000, block);
      r.gen->noteOff(1, 0);
      r.run(L, R, 3000, block);
      return L;
    };
    auto a = seq(64), b = seq(1000), c = seq(333);
    double diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
      diff = std::max({diff, static_cast<double>(std::fabs(a[i] - b[i])), static_cast<double>(std::fabs(a[i] - c[i]))});
    expect(a.size() == b.size() && a.size() == c.size() && diff < 1e-6, "block-size invariant");
    const PluginInfo* info = findPlugin(kPatchPlayerType);
    for (int ext = 0; ext < 2 && info; ++ext) {
      Rig r(p);
      for (size_t k = 0; k < info->params.size(); ++k) r.set(static_cast<int>(k), static_cast<float>(ext ? info->params[k].max : info->params[k].min));
      r.set(PpBank, 0), r.set(PpPatch, 0);
      for (int k = 0; k < 8; ++k) r.gen->noteOn(static_cast<uint32_t>(k + 1), k * 16, 1.f, k);
      std::vector<float> L, R;
      r.run(L, R, 20000);
      for (int k = 0; k < 8; ++k) r.gen->noteOff(static_cast<uint32_t>(k + 1), 0);
      r.run(L, R, 4000);
      expect(finite(L) && finite(R) && peak(L) < 50.f, ext ? "max parameters finite" : "min parameters finite");
    }
  }
  return finish();
}
