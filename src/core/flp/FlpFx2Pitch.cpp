// Gridline — FLP import, second wave: pitch / frequency-shift / vocoder /
// scratch effects.
//
// State layouts:
//   Pitcher            "SSLF", u32 size, "1RTP" tag, 33 float parameters
//                      (0 speed, 1 stereo, 2 gender, 3 fine tune, 4 bypass all,
//                      5-16 key C..B bypass, 17-20 voice 1-4 pan, 21 unused, 22
//                      formant, 23 A4 reference, 24 pitch reference, 25-27 MIDI /
//                      octaves / harmonize, 28 root, 29 scale, 30 mix, 31
//                      velocity, 32 lowest note range), 12 bytes scale note
//                      mask (C..B), float MIDI port (int(256 v) - 1).
//   Pitch Shifter      version 7, ints: pitch (cents ±1200), formant (cents),
//                      analysis channel, mix (0..2400), duration (2000..15000,
//                      0.01 ms), density, jitter, rand, delay (1..768), feedback
//                      (0..100), delay mode, formant correction (0..65536),
//                      pitch mode; then a spare word and flag bytes.
//   Frequency Shifter  version 4, ints: mix (0..10000), type, frequency
//                      (±70000), L/R phase, left shape, right shape, feedback
//                      (5000 = none), stereo, range, start phase, 2 spare.
//   Fruity Vocoder     version 2, band count, 2 words, band levels (one per
//                      band), then the knob block.
//   Fruity Scratcher   version 1001000 and float settings, sample path.
#include "../../packs/spectral/spectral.h"
#include "../PluginParams.h"
#include "FlpFx2.h"

namespace gl::flp::fx2 {

namespace {

namespace pp = gl::pp;
namespace sp = gl::packs::spectral;

// ------------------------------------------------------------------ Pitcher → Pitch Corrector
// Correction speed 0..1 → glide time (ms). The plugin smooths the correction
// with a one-pole whose time constant is (3x³ + 0.001) / 8.82 s, x = 1 - speed
// (instant above 0.99).
double pitcherSpeedMs(double s) {
  double x = 1.0 - clampd(s, 0, 1);
  if (x < 0.01) return 1.0;
  return clampd(1000.0 * (3.0 * x * x * x + 0.001) / 8.82, 1.0, 500.0);
}
// Gender 0..1 → formant shift: ratio 2^(2g - 1), i.e. ±12 semitones.
double pitcherGenderSt(double g) { return clampd(12.0 * (2.0 * g - 1.0), -12, 12); }
// A4 reference 0..1 → Hz (440 · 2^((v - 0.5) / 6), ±1 semitone).
double pitcherRefHz(double v) { return 440.0 * std::pow(2.0, (clampd(v, 0, 1) - 0.5) / 6.0); }

bool decodePitcher(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 12 + 33 * 4 + 12 || b[0] != 'S' || b[1] != 'S' || b[2] != 'L' || b[3] != 'F') return false;
  Reader r(b.data(), b.size());
  r.skip(12);
  float f[33];
  for (float& v : f) v = r.f32();
  uint8_t mask[12];
  for (uint8_t& v : mask) v = r.u8();
  const bool hasPort = r.n - r.pos >= 4;  // MIDI port, stored after the mask
  const float port = hasPort ? r.f32() : 0.0f;
  fx::begin(m, "gl.pitcher");
  fx::set(m, sp::PcModel, 1);
  fx::set(m, sp::PcCorrSpeed, clampd(f[0], 0, 1));
  fx::set(m, sp::PcSpeed, pitcherSpeedMs(f[0]));  // legacy engine equivalent
  fx::set(m, sp::PcStereo, clampd(f[1], 0, 1));
  fx::set(m, sp::PcFormant, pitcherGenderSt(f[2]));
  fx::set(m, sp::PcFine, clampd(100.0 * (2.0 * f[3] - 1.0), -100, 100));
  fx::set(m, sp::PcAmount, 1);
  fx::set(m, sp::PcBypassAll, f[4] >= 0.5f ? 1 : 0);
  for (int k = 0; k < 12; ++k) fx::set(m, sp::PcKeyBypass0 + k, f[5 + k] >= 0.5f ? 1 : 0);
  for (int k = 0; k < 4; ++k) fx::set(m, sp::PcHarmPan1 + k, clampd(2.0 * f[17 + k] - 1.0, -1, 1));
  fx::set(m, sp::PcLeadPan, clampd(2.0 * f[17] - 1.0, -1, 1));
  fx::set(m, sp::PcPreserve, f[22] >= 0.5f ? 1 : 0);
  const double a4 = pitcherRefHz(f[23]);
  fx::set(m, sp::PcReference, a4);
  fx::set(m, sp::PcRefChannel, clampd(std::floor(2.0 * clampd(f[24], 0, 1)), 0, 2));
  fx::set(m, sp::PcMidiTargets, f[25] >= 0.5f ? 1 : 0);
  fx::set(m, sp::PcMidiOctaves, f[26] >= 0.5f ? 1 : 0);
  fx::set(m, sp::PcHarmonize, f[27] >= 0.5f ? 1 : 0);
  fx::set(m, sp::PcKey, clampd(std::lround(f[28]), 0, 11));
  // The saved note mask (C..B) is what the plugin corrects to: a custom scale.
  int notes = 0;
  for (int k = 0; k < 12; ++k) {
    fx::set(m, sp::PcNote0 + k, mask[k] != 0 ? 1 : 0);
    notes += mask[k] != 0;
  }
  fx::set(m, sp::PcScale, notes ? sp::kScaleCount - 1 : 0);
  fx::set(m, sp::PcHarmMix, f[30] >= 0.5f ? 1 : 0);
  fx::set(m, sp::PcHarmVel, f[31] >= 0.5f ? 1 : 0);
  const int ri = std::clamp(static_cast<int>(std::lround(f[32])), 0, 5);
  fx::set(m, sp::PcRange6, ri);
  fx::set(m, sp::PcLowest, nearestChoice(sp::kPcRangeHz[ri], {55, 80, 110, 170}));  // legacy engine equivalent
  // MIDI port: int(256 v) - 1; "none" (-1) maps onto Gridline's "all ports".
  fx::set(m, sp::PcMidiPort, clampd(std::floor(256.0 * clampd(port, 0, 1)) - 1.0, -1, 255));
  for (int v : {sp::PcVoice1Level, sp::PcVoice2Level, sp::PcVoice3Level}) fx::set(m, v, 0);
  // Every control has a 1:1 parameter; only an A4 above 466 Hz is clamped.
  m.exact = a4 <= 466.0 + 1e-9 && a4 >= 415.0 - 1e-9;
  auto on = [](double v) { return v >= 0.5 ? 1.0 : 0.0; };
  KnobTable tab = {{0, 0, 1, sp::PcCorrSpeed, [](double v) { return clampd(v, 0, 1); }},
                   {1, 0, 1, sp::PcStereo, [](double v) { return clampd(v, 0, 1); }},
                   {2, 0, 1, sp::PcFormant, pitcherGenderSt},
                   {3, 0, 1, sp::PcFine, [](double v) { return clampd(100.0 * (2.0 * v - 1.0), -100, 100); }},
                   {4, 0, 1, sp::PcBypassAll, on},
                   {22, 0, 1, sp::PcPreserve, on},
                   {23, 0, 1, sp::PcReference, pitcherRefHz},
                   {24, 0, 1, sp::PcRefChannel, [](double v) { return clampd(std::floor(2.0 * clampd(v, 0, 1)), 0, 2); }},
                   {25, 0, 1, sp::PcMidiTargets, on},
                   {26, 0, 1, sp::PcMidiOctaves, on},
                   {27, 0, 1, sp::PcHarmonize, on},
                   {28, 0, 11, sp::PcKey, [](double v) { return clampd(std::lround(v), 0, 11); }},
                   {30, 0, 1, sp::PcHarmMix, on},
                   {31, 0, 1, sp::PcHarmVel, on},
                   {32, 0, 5, sp::PcRange6, [](double v) { return clampd(std::lround(v), 0, 5); }}};
  for (int k = 0; k < 12; ++k) tab.push_back({5 + k, 0, 1, sp::PcKeyBypass0 + k, on});
  for (int k = 0; k < 4; ++k) tab.push_back({17 + k, 0, 1, sp::PcHarmPan1 + k, [](double v) { return clampd(2.0 * v - 1.0, -1, 1); }});
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Pitch Shifter → Pitch Shifter
// v6/v7: version, 13 ints (tags 0..12), u32 correction copy, u8 tempo sync,
// i32 engine (0 voice, 1 music), i32 detector quality, v7: i32 spectrum
// speed, u8 spectrum on, u8 spectrum mono.
bool decodePitchShifter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 13 * 4) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 13);
  if (t.size() < 13) return false;
  int sync = 0, type = 1, det = 1;
  if (r.n - r.pos >= 4 + 1 + 8) {
    r.u32();  // correction wheel copy
    sync = r.u8() != 0;
    type = r.i32() != 0 ? 1 : 0;
    det = r.i32() != 0 ? 1 : 0;
  }
  fx::begin(m, "gl.pitch");
  auto delayMs = [](double v) { return clampd(sp::psDelayMs(v), 0, 1000); };
  auto delaySteps = [](double v) { return clampd(sp::psDelaySteps(v), 0, 16); };
  KnobTable tab = {
      {0, -1200, 1200, sp::PsPitch, [](double v) { return clampd(v / 100.0, -24, 24); }},
      {1, -1200, 1200, sp::PsFormant, [](double v) { return clampd(v / 100.0, -12, 12); }},
      {2, 0, 2, sp::PsAnalysis, [](double v) { return clampd(std::lround(v), 0, 2); }},
      {3, 0, 2400, sp::PsMix, [](double v) { return clampd(v / 2400.0, 0, 1); }},
      {4, 2000, 15000, sp::PsDuration, [](double v) { return clampd(v / 100.0, 20, 150); }},
      {5, 100, 500, sp::PsDensity, [](double v) { return clampd(v / 100.0, 1, 5); }},
      {6, 0, 100, sp::PsJitter, [](double v) { return clampd(v / 100.0, 0, 1); }},
      {7, 0, 100, sp::PsRand, [](double v) { return clampd(v / 100.0, 0, 1); }},
      {8, 1, 768, sync ? sp::PsDelaySteps : sp::PsDelay, sync ? std::function<double(double)>(delaySteps) : std::function<double(double)>(delayMs)},
      {9, 0, 100, sp::PsFeedback, [](double v) { return clampd(v / 100.0, 0, 0.95); }},
      {10, 0, 3, sp::PsDelayMode, [](double v) { return clampd(std::lround(v), 0, 3); }},
      {11, 0, 65536, sp::PsCorrection, [](double v) { return clampd(v / 65536.0, 0, 1); }},
      {12, 0, 3, sp::PsPitchMode, [](double v) { return clampd(std::lround(v), 0, 3); }},
  };
  fx::applyTable(m, tab, t);
  fx::set(m, sp::PsDelay, delayMs(t[8]));
  fx::set(m, sp::PsDelaySteps, delaySteps(t[8]));
  fx::set(m, sp::PsDelaySync, sync);
  fx::set(m, sp::PsEngine, type ? sp::kPsMusic : sp::kPsVoice);
  fx::set(m, sp::PsDetector, det);
  fx::set(m, sp::PsFine, 0);
  fx::set(m, sp::PsDetune, 0);
  fx::set(m, sp::PsOutput, 0);
  // Classic-engine equivalents (used only when the engine is switched back).
  fx::set(m, sp::PsMode, 1);
  fx::set(m, sp::PsPreserve, t[11] >= 32768 ? 1 : 0);
  fx::set(m, sp::PsGrain, clampd(t[4] / 100.0, 10, 120));
  // Every field maps 1:1; feedback above 95 % is capped.
  m.exact = t[9] <= 95;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Frequency Shifter → Freq Shifter

// J-26: reference model (FsModel = 1); every knob is stored as its position.
// FL type 0 HQ / 1 normal / 2 ring → Gridline 1 / 0 / 2; FL range 0 wide /
// 1 narrow / 2 tempo → Gridline 1 / 0 / 2.
bool decodeFreqShifter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 10 * 4) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 10);
  if (t.size() < 10) return false;
  fx::begin(m, "gl.freqshift");
  fx::set(m, pp::FsModel, 1);
  auto lvl = [](double v) { return clampd(v / 10000.0, 0, 1); };
  auto type = [](double v) {
    double n = clampd(v / 2.0, 0, 1);
    return n < 1.0 / 3 ? 1.0 : n < 2.0 / 3 ? 0.0 : 2.0;
  };
  auto range = [](double v) {
    double n = clampd(v / 2.0, 0, 1);
    return n < 1.0 / 3 ? 1.0 : n < 2.0 / 3 ? 0.0 : 2.0;
  };
  KnobTable tab = {
      {0, 0, 10000, pp::FsMixRef, lvl},
      {1, 0, 2, pp::FsType, type},
      {2, -70000, 70000, pp::FsFreqPos, [](double v) { return clampd((v + 70000.0) / 140000.0, 0, 1); }},
      {3, 0, 10000, pp::FsLrPhase, lvl},
      {4, 0, 10000, pp::FsShapeL, lvl},
      {5, 0, 10000, pp::FsShapeR, lvl},
      {6, 0, 10000, pp::FsFeedbackRef, lvl},
      {7, 0, 10000, pp::FsWidth, lvl},
      {8, 0, 2, pp::FsRange, range},
      {9, 0, 10000, pp::FsStartRef, lvl},
  };
  fx::applyTable(m, tab, t);
  r.i32();  // flags (shape link button)
  const int sidechain = r.left() >= 4 ? r.i32() : 0;
  m.exact = sidechain == 0;  // a sidechain source track is not modelled
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fruity Vocoder → Vocoder
bool decodeFruityVocoder(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 16) return false;
  r.i32();  // version
  const int bands = r.i32();
  fx::begin(m, "gl.vocoder");
  if (bands >= 1 && bands <= 512) {
    std::vector<double> ch(sp::kVocBandChoices, sp::kVocBandChoices + sp::kVocBandChoiceCount);
    fx::set(m, sp::VcBands, nearestChoice(bands, ch));
  }
  // FL's vocoder takes the modulator on the left and the carrier on the right
  // input channel (routed by panning in the mixer).
  fx::set(m, sp::VcCarrier, 0);
  fx::set(m, sp::VcModSource, 0);
  fx::set(m, sp::VxModel, 0);  // the 0.5 vocoder engine (left modulator / right carrier)
  m.exact = false;  // band shaping, envelope and level curves approximate
  return true;
}

// ------------------------------------------------------------------ Fruity Scratcher → Scratcher
bool decodeScratcher(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  if (!pluginState(evs)) return false;
  fx::begin(m, "gl.scratcher");
  m.exact = false;  // turntable movement curves and the loaded sample are not imported
  return true;
}

}  // namespace

void registerFx2Pitch(std::map<std::string, PluginDecoder>& d) {
  d["Pitcher"] = decodePitcher;
  d["Pitch Shifter"] = decodePitchShifter;
  d["Frequency Shifter"] = decodeFreqShifter;
  d["Fruity Vocoder"] = decodeFruityVocoder;
  d["Fruity Scratcher"] = decodeScratcher;
}

}  // namespace gl::flp::fx2
