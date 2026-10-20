// Gridline — FLP export: plugin state encoders for FlpFx2Pitch.cpp (Pitcher,
// Pitch Shifter, Frequency Shifter, Fruity Vocoder, Fruity Scratcher). The
// reverse of the decoders in that file; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include "FlpEncode.h"
#include "../../packs/spectral/spectral.h"
#include "../PluginParams.h"

namespace gl::flp::enc {

namespace {

namespace sp = gl::packs::spectral;

float param(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }

Field f32(int gl, size_t offset, double lo, double hi, std::function<double(double)> inverse = {}) {
  return Field{gl, offset, Type::F32, lo, hi, std::move(inverse), {}};
}

// ---------------------------------------------------------------- Pitcher
// "SSLF", u32 size, "1RTP", 33 f32 parameters (0..1 unless noted), 12 bytes
// scale note mask (C..B), f32 MIDI port.
constexpr int kPitcherParams = 33;
constexpr size_t kPitcherBase = 12;
constexpr size_t kPitcherMask = kPitcherBase + 4 * kPitcherParams;
constexpr size_t kPitcherPort = kPitcherMask + 12;
size_t pf(int k) { return kPitcherBase + 4 * static_cast<size_t>(k); }

Encoder pitcher() {
  Encoder e;
  e.flName = "Pitcher";
  e.glType = "gl.pitcher";
  e.nativeRank = 0;
  e.defaults = [] {
    float f[kPitcherParams] = {};
    f[0] = 1.0f;   // correction speed (instant)
    f[1] = 1.0f;   // stereo
    f[2] = 0.5f;   // gender (neutral)
    f[3] = 0.5f;   // fine tune (centre)
    f[4] = 0.0f;   // bypass all off
    // 5..16 per-key bypass off
    for (int k = 17; k <= 20; ++k) f[k] = 0.5f;  // voice 1..4 pan (centre)
    f[22] = 0.0f;  // formant preservation off
    f[23] = 0.5f;  // A4 reference 440 Hz
    f[24] = 0.0f;  // pitch reference: mixer
    // 25..27 MIDI targets / octaves / harmonize off, 28 root C, 29 scale, 30 mix, 31 velocity
    f[32] = 2.0f;  // lowest note range (110 Hz)
    ByteWriter w;
    w.bytes("SSLF", 4);
    w.u32(static_cast<uint32_t>(4 + 4 * kPitcherParams + 12 + 4));  // size of what follows
    w.bytes("1RTP", 4);
    for (float v : f) w.f32(v);
    for (int k = 0; k < 12; ++k) w.u8(1);  // every note of the scale
    w.f32(0.0f);                           // MIDI port: none
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    auto same = [](double v) { return v; };
    auto pan = [](double g) { return (g + 1.0) / 2.0; };
    std::vector<Field> v{
        f32(sp::PcCorrSpeed, pf(0), 0, 1, same),
        f32(sp::PcStereo, pf(1), 0, 1, same),
        f32(sp::PcFormant, pf(2), 0, 1, [](double st) { return st / 24.0 + 0.5; }),
        f32(sp::PcFine, pf(3), 0, 1, [](double c) { return (c / 100.0 + 1.0) / 2.0; }),
        f32(sp::PcBypassAll, pf(4), 0, 1, same),
        f32(sp::PcPreserve, pf(22), 0, 1, same),
        f32(sp::PcReference, pf(23), 0, 1, [](double hz) { return 0.5 + 6.0 * std::log2(hz / 440.0); }),
        f32(sp::PcRefChannel, pf(24), 0, 1, [](double c) { return c / 2.0; }),
        f32(sp::PcMidiTargets, pf(25), 0, 1, same),
        f32(sp::PcMidiOctaves, pf(26), 0, 1, same),
        f32(sp::PcHarmonize, pf(27), 0, 1, same),
        f32(sp::PcKey, pf(28), 0, 11, same),
        f32(sp::PcHarmMix, pf(30), 0, 1, same),
        f32(sp::PcHarmVel, pf(31), 0, 1, same),
        f32(sp::PcRange6, pf(32), 0, 5, same),
    };
    for (int k = 0; k < 12; ++k) v.push_back(f32(sp::PcKeyBypass0 + k, pf(5 + k), 0, 1, same));
    for (int k = 0; k < 4; ++k) v.push_back(f32(sp::PcHarmPan1 + k, pf(17 + k), 0, 1, pan));
    for (int k = 0; k < 12; ++k) v.push_back(field(sp::PcNote0 + k, kPitcherMask + static_cast<size_t>(k), Type::U8, 0, 1));
    if (b.size() >= kPitcherPort + 4)
      v.push_back(f32(sp::PcMidiPort, kPitcherPort, 0, 1, [](double port) { return (port + 1.0) / 256.0; }));
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- Pitch Shifter
// v7: version, 13 ints by tag, u32 correction copy, u8 tempo sync, i32
// engine (0 voice, 1 music), i32 detector, i32 spectrum speed, u8 spectrum
// on, u8 spectrum mono.
Encoder pitchShifter() {
  Encoder e;
  e.flName = "Pitch Shifter";
  e.glType = "gl.pitch";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(7);      // version
    w.i32(0);      // 0 pitch (cents)
    w.i32(0);      // 1 formant (cents)
    w.i32(0);      // 2 analysis channel
    w.i32(2400);   // 3 mix (full wet)
    w.i32(8500);   // 4 duration (85 ms)
    w.i32(200);    // 5 density
    w.i32(0);      // 6 jitter
    w.i32(0);      // 7 randomness
    w.i32(768);    // 8 delay
    w.i32(0);      // 9 feedback
    w.i32(0);      // 10 delay mode
    w.i32(65536);  // 11 formant correction
    w.i32(0);      // 12 pitch mode
    w.u32(65536);  // correction wheel copy
    w.u8(0);       // tempo sync off
    w.i32(1);      // engine: music
    w.i32(1);      // detector: high quality
    w.i32(1000);   // spectrum speed
    w.u8(1);       // spectrum on
    w.u8(1);       // spectrum mono
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> v{i32(sp::PsPitch, 4, -1200, 1200),     i32(sp::PsFormant, 8, -1200, 1200), i32(sp::PsAnalysis, 12, 0, 2),
                         i32(sp::PsMix, 16, 0, 2400),          i32(sp::PsDuration, 20, 2000, 15000), i32(sp::PsDensity, 24, 100, 500),
                         i32(sp::PsJitter, 28, 0, 100),        i32(sp::PsRand, 32, 0, 100),        i32(sp::PsDelay, 36, 1, 768),
                         i32(sp::PsDelaySteps, 36, 1, 768),    i32(sp::PsFeedback, 40, 0, 100),    i32(sp::PsDelayMode, 44, 0, 3),
                         i32(sp::PsCorrection, 48, 0, 65536),  i32(sp::PsPitchMode, 52, 0, 3)};
    if (b.size() >= 4 + 13 * 4 + 4 + 1 + 8) {
      v.push_back(field(sp::PsDelaySync, 60, Type::U8, 0, 1));
      v.push_back(i32(sp::PsEngine, 61, 0, 1));
      v.push_back(i32(sp::PsDetector, 65, 0, 1));
    }
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- Frequency Shifter
// version 4, ints by tag: mix, type, frequency, L/R phase, left / right
// shape, feedback, stereo, range, start phase; then flags, sidechain track.
Encoder freqShifter() {
  Encoder e;
  e.flName = "Frequency Shifter";
  e.glType = "gl.freqshift";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(4);      // version
    w.i32(10000);  // 0 mix (full wet)
    w.i32(0);      // 1 type: HQ
    w.i32(70000);  // 2 frequency (top of the range)
    w.i32(0);      // 3 L/R phase
    w.i32(10000);  // 4 left shape
    w.i32(10000);  // 5 right shape
    w.i32(5000);   // 6 feedback (none)
    w.i32(10000);  // 7 stereo
    w.i32(1);      // 8 range: narrow
    w.i32(5000);   // 9 start phase
    w.i32(0);      // flags (shape link)
    w.i32(0);      // sidechain track: none
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::FsMixRef, 4, 0, 10000),     i32(pp::FsType, 8, 0, 2),           i32(pp::FsFreqPos, 12, -70000, 70000),
                              i32(pp::FsLrPhase, 16, 0, 10000),   i32(pp::FsShapeL, 20, 0, 10000),     i32(pp::FsShapeR, 24, 0, 10000),
                              i32(pp::FsFeedbackRef, 28, 0, 10000), i32(pp::FsWidth, 32, 0, 10000),  i32(pp::FsRange, 36, 0, 2),
                              i32(pp::FsStartRef, 40, 0, 10000)};
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Vocoder
// version 2, band count, u32 filter order, u32 listen mode, u8 carrier side,
// f32 level per band, then 12 ints by tag (frequency range, scale, invert,
// formant, bandwidth, attack, release, hold, modulator / carrier / wet level).
constexpr size_t kVocLevels = 17;
Bytes vocoderState(int bands, const Bytes& old) {
  ByteWriter w;
  w.u32(2);  // version
  w.i32(bands);
  const int oldBands = old.size() >= 8 ? static_cast<int32_t>(getU32(old, 4)) : 0;
  const bool keep = oldBands > 0 && old.size() >= kVocLevels + 4 * static_cast<size_t>(oldBands);
  if (keep) {
    w.bytes(old.data() + 8, 9);  // filter order, listen mode, carrier side
  } else {
    w.u32(2);  // filter order
    w.u32(2);  // listen: wet
    w.u8(0);   // carrier on the right
  }
  for (int k = 0; k < bands; ++k) w.f32(keep && k < oldBands ? getF32(old, kVocLevels + 4 * static_cast<size_t>(k)) : 1.0f);  // band levels
  const size_t knobs = kVocLevels + 4 * static_cast<size_t>(std::max(oldBands, 0));
  if (keep && old.size() >= knobs) {
    w.bytes(old.data() + knobs, old.size() - knobs);
  } else {
    w.i32(0);      // 0 start frequency
    w.i32(65536);  // 1 end frequency
    w.i32(64);     // 2 frequency scale
    w.i32(0);      // 3 invert off
    w.i32(0);      // 4 formant
    w.i32(50);     // 5 bandwidth
    w.i32(20);     // 6 attack
    w.i32(100);    // 7 release
    w.i32(0);      // 8 hold off
    w.i32(0);      // 9 modulator level
    w.i32(0);      // 10 carrier level
    w.i32(128);    // 11 wet level
  }
  return w.b;
}
Encoder vocoder() {
  Encoder e;
  e.flName = "Fruity Vocoder";
  e.glType = "gl.vocoder";
  e.nativeRank = -1;  // only the band count is carried
  e.defaults = [] { return vocoderState(16, {}); };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    const int choice = std::clamp(static_cast<int>(std::lround(param(g, sp::VcBands))), 0, static_cast<int>(sp::kVocBandChoiceCount) - 1);
    const int bands = sp::kVocBandChoices[choice];
    if (b.size() < 8 || static_cast<int32_t>(getU32(b, 4)) != bands) b = vocoderState(bands, b);
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Scratcher
// version 1001000, 14 f32 controls by tag (normalized; rotations raw), u32
// length + sample path. Nothing is decoded: written with the defaults.
Encoder scratcher() {
  Encoder e;
  e.flName = "Fruity Scratcher";
  e.glType = "gl.scratcher";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(1001000);       // version
    w.f32(0.0f);          // 0 wave position
    w.f32(1.0f);          // 1 mute switch (FL's default position)
    w.f32(50.0f / 100);   // 2 turntable speed
    w.f32(25.0f / 100);   // 3 turntable acceleration
    w.f32(0.5f);          // 4 pan (centre)
    w.f32(199.0f / 255);  // 5 volume
    w.f32(0.0f);          // 6 turntable hold off
    w.f32(50.0f / 100);   // 7 turntable sensitivity
    w.f32(0.0f);          // 8 transport
    w.f32(500.0f / 1000); // 9 smooth 1
    w.f32(908.0f / 1000); // 10 smooth 2
    w.f32(999.0f / 1000); // 11 smooth 3
    w.f32(400.0f / 1000); // 12 smooth 4
    w.f32(9.0f);          // 13 rotations
    w.u32(0);             // no sample
    return w.b;
  };
  return e;
}

}  // namespace

void registerFx2Pitch(std::vector<Encoder>& v) {
  v.push_back(pitcher());
  v.push_back(pitchShifter());
  v.push_back(freqShifter());
  v.push_back(vocoder());
  v.push_back(scratcher());
}

}  // namespace gl::flp::enc
