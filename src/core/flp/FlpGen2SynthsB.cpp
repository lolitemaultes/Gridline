// Gridline — FLP import, second wave: Sakura, Kepler, PoiZone, Sawer, SimSynth,
// Morphine, Toxic Biohazard.
//
// Layouts (worked out from FL's supplied projects, render tests and presets):
//   Sawer / PoiZone / Sakura  "SSLF" u32 size, 4-char id ("Swr1"/"Ver4"/"Skr1"
//                             reversed), then one float per control tag (knobs
//                             0..1, combos / digit wheels as raw indices), then
//                             extra data and the preset name.
//   Kepler                    u32 0, u32 size, u32 0, u32 n, name ("Kepler-1"),
//                             u32 version, u32 count, `count` floats 0..1 in tag
//                             order (unaligned), then a few bytes.
//   SimSynth                  45 doubles 0..1 (three 7-value oscillator blocks,
//                             LFO, envelopes, filter) then 32-bit switches.
//   Morphine                  "SSLF" "Mor0" nested chunks; "MBP0" chunks hold
//                             harmonic spectra (mask byte per 8 partials, then a
//                             float per set bit).
//   Toxic Biohazard           "SSLF" "TBi2" FM engine block (not mapped).
// All map onto Gridline instruments with different engines, so every mapping
// is approximate (exact = false).
#include <algorithm>
#include <cmath>
#include "FlpGen2.h"

namespace gl::flp::gen2 {

namespace {

// Gridline parameter indices used here (see the catalogue).
namespace an {  // gl.analog
enum {
  Volume = 0, Pan, Transpose, Fine, Poly, VoiceMode, Glide, GlideMode, VelVol, VelCut, UniVoices, UniDetune, UniSpread,
  O1On = 13, O1Wave, O1Coarse, O1Fine, O1Level, O1Pw, O1Mod, O1ModAmt,
  O2On = 21, O2Wave, O2Coarse, O2Fine, O2Level, O2Pw, O2Mod, O2ModAmt,
  O3On = 29, O3Wave, O3Coarse, O3Fine, O3Level, O3Pw, O3Mod, O3ModAmt,
  SubLevel = 37, SubOct, SubWave, Noise,
  FltMode = 41, Cutoff, Reso, FltDrive, KeyTrack, FltEnv,
  AmpA = 47, AmpD, AmpS, AmpR, FA, FD, FS, FR, MA, MD, MS, MR, ModEnvTarget, ModEnvAmt,
  L1Shape = 61, L1Rate, L1Sync, L1Div, L1Retrig, L1Fade, L1Target, L1Amt,
  L2Shape = 69, L2Rate, L2Sync, L2Div, L2Retrig, L2Fade, L2Target, L2Amt,
  M1Src = 77, M1Dst, M1Amt,
  Arp = 89, ArpMode, ArpOct, ArpRate, ArpGate, Drive,
  Chorus = 95, ChoRate, ChoDepth, ChoMix, Delay, DlyTime, DlyFb, DlyMix, Reverb, RevSize, RevDamp, RevMix
};
// Waves, filter modes, destinations.
enum { Sine = 0, Tri, Saw, Pulse, NoiseW };
enum { Lp12 = 0, Lp24, Bp12, Bp24, Hp12, Hp24, Notch, Peak };
enum { DNone = 0, DPitch, DO1, DO2, DO3, DPw, DOscMod, DCutoff, DReso, DVolume, DPan };
enum { SNone = 0, SLfo1, SLfo2, SModEnv, SFiltEnv, SAmpEnv };
}  // namespace an
namespace st {  // gl.string
enum {
  Exciter = 0, Position, Hardness, Noise, Force, Decay, Damping, Stiffness, Pickup, Saturation,
  String2, Level2, Detune, Body, BodyMix, Size, LoCut, HiCut, HiCutEnv, EnvDecay, Attack, Sustain, Release,
  Strum, StrumTime, Direction, Drive, Tone, Cabinet, Vibrato, VibRate, Spread, Voices, Volume, Chorus, Delay, DelayTime,
  Feedback, Reverb, Room
};
}  // namespace st
namespace ad {  // gl.additive
enum { SpecA = 0, SpecB, Morph, Partials = 4, Attack = 33, Decay, Sustain, Release, Volume = 45, Drawn = 52 };
inline constexpr int kDrawn = 32;
}  // namespace ad

double tlog(double x, double lo, double hi) { return lo * std::pow(hi / lo, std::clamp(x, 0.0, 1.0)); }
double cutoffHz(double x) { return tlog(x, 20.0, 20000.0); }
double bip(double x) { return std::clamp(x * 2.0 - 1.0, -1.0, 1.0); }
int idx(double x, int n) { return std::clamp(static_cast<int>(std::lround(x * (n - 1))), 0, n - 1); }

// Floats after an "SSLF" header with the given id (reversed as stored).
struct TagFloats {
  Blob b;
  size_t base = 12;
  size_t count = 0;
  bool ok = false;
  double operator()(int tag, double def = 0.0) const {
    if (tag < 0 || static_cast<size_t>(tag) >= count) return def;
    float v = b.f32(base + 4 * static_cast<size_t>(tag));
    return std::isfinite(v) ? v : def;
  }
};

TagFloats sslf(const Blob& b, const char* id, size_t count) {
  TagFloats t;
  t.b = b;
  if (b.size() < 12 || b.find("SSLF") != 0 || b.find(id) != 8) return t;
  t.count = std::min(count, (b.size() - 12) / 4);
  t.ok = t.count > 0;
  return t;
}

// Amplitude (seconds) of the shared synth time knobs.
double atk(double x) { return tlog(x, 0.001, 8.0); }
double dec(double x) { return tlog(x, 0.005, 10.0); }
double rel(double x) { return tlog(x, 0.005, 12.0); }

// The SSLF synths' knobs store 0..1 but are 0..256 controls (recorded pattern
// values are in that range).
std::vector<RawRange> knobRanges(std::initializer_list<int> tags) {
  std::vector<RawRange> r;
  for (int t : tags) r.push_back({t, 0, 256});
  return r;
}

// Effects block shared by the SSLF synths: chorus / delay / reverb.
void analogFx(PluginMapping& m, bool cho, double choRate, double choDepth, double choMix, bool dly, double dlyTime,
              double dlyFb, double dlyMix, bool rev, double revSize, double revDamp, double revMix) {
  set(m, an::Chorus, cho);
  set(m, an::ChoRate, tlog(choRate, 0.05, 5.0));
  set(m, an::ChoDepth, choDepth);
  set(m, an::ChoMix, choMix);
  set(m, an::Delay, dly);
  set(m, an::DlyTime, idx(dlyTime, 14));
  set(m, an::DlyFb, dlyFb * 0.95);
  set(m, an::DlyMix, dlyMix);
  set(m, an::Reverb, rev);
  set(m, an::RevSize, revSize);
  set(m, an::RevDamp, revDamp);
  set(m, an::RevMix, revMix);
}

// ------------------------------------------------------------------ Kepler
// Tags: 0 arp on, 1 arp mode, 2 arp range, 3 arp rate, 4 arp sync, 5 LFO rate,
// 6 LFO sync, 7 LFO delay, 8 LFO trigger mode, 9 LFO trigger, 10 DCO LFO,
// 11 PWM, 12 PWM source, 13 pulse on, 14 saw on, 15 sub on, 16 sub level,
// 17 noise, 18 HPF, 19 VCF freq, 20 VCF reso, 21 VCF env polarity, 22 VCF env,
// 23 VCF LFO, 24 VCF key follow, 25 VCA source, 26-29 env ADSR, 30 chorus mode,
// 31 hold, 32 octave, 33 volume, 34 LFO retrigger phase, 35 LFO phase.
double kepLfoHz(double x) { return tlog(x, 0.1, 30.0); }
double kepAtk(double x) { return tlog(x, 0.001, 3.0); }
double kepDec(double x) { return tlog(x, 0.002, 12.0); }

bool decodeKepler(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.analog");
  m.exact = false;
  uint32_t nameLen = b.u32(12);
  size_t at = 16 + nameLen + 4;  // past name and version
  if (nameLen > 64 || !b.has(at, 4)) return true;
  size_t count = std::min<size_t>(b.u32(at), 64);
  size_t base = at + 4;
  auto t = [&](int tag, double def) -> double {
    if (static_cast<size_t>(tag) >= count || !b.has(base + 4 * static_cast<size_t>(tag), 4)) return def;
    float v = b.f32(base + 4 * static_cast<size_t>(tag));
    return std::isfinite(v) ? std::clamp(static_cast<double>(v), 0.0, 1.0) : def;
  };
  // Oscillators: osc 1 = pulse, osc 2 = saw, sub = square one octave down.
  set(m, an::O1On, t(13, 0) > 0.5);
  set(m, an::O1Wave, an::Pulse);
  set(m, an::O1Level, 0.8);
  set(m, an::O2On, t(14, 1) > 0.5);
  set(m, an::O2Wave, an::Saw);
  set(m, an::O2Fine, 0);
  set(m, an::O2Level, 0.8);
  set(m, an::O3On, 0);
  bool sub = t(15, 0) > 0.5;
  set(m, an::SubLevel, sub ? t(16, 0.01) : 0.0);
  set(m, an::SubOct, 0);
  set(m, an::SubWave, 1);
  set(m, an::Noise, t(17, 0));
  set(m, an::UniVoices, 1);
  // PWM: manual width or LFO / envelope modulation.
  double pwm = t(11, 0);
  int pwmSrc = idx(t(12, 0), 3);  // 0 LFO, 1 manual, 2 envelope
  if (pwmSrc == 1) {
    set(m, an::O1Pw, 0.5 - 0.45 * pwm);
  } else if (pwm > 0) {
    set(m, an::M1Src, pwmSrc == 0 ? an::SLfo1 : an::SAmpEnv);
    set(m, an::M1Dst, an::DPw);
    set(m, an::M1Amt, pwm);
  }
  // LFO 1: DCO vibrato; LFO 2 (same rate): VCF.
  double lfoHz = kepLfoHz(t(5, 0.01));
  set(m, an::L1Shape, 1);
  set(m, an::L1Rate, lfoHz);
  set(m, an::L1Fade, tlog(t(7, 0), 0.001, 5.0));
  set(m, an::L1Retrig, t(8, 0) > 0.5);
  set(m, an::L1Target, an::DPitch);
  set(m, an::L1Amt, 0.25 * t(10, 0.01));
  set(m, an::L2Shape, 1);
  set(m, an::L2Rate, lfoHz);
  set(m, an::L2Target, an::DCutoff);
  set(m, an::L2Amt, t(23, 0));
  // Filter: 24 dB low-pass; one envelope drives filter and amp (VCA "env").
  set(m, an::FltMode, an::Lp24);
  set(m, an::Cutoff, cutoffHz(t(19, 1)));
  set(m, an::Reso, 0.95 * t(20, 0));
  double envAmt = t(22, 0.01);
  set(m, an::FltEnv, t(21, 0) > 0.5 ? -envAmt : envAmt);
  set(m, an::KeyTrack, t(24, 0));
  double A = kepAtk(t(26, 0.01)), D = kepDec(t(27, 0.5)), S = t(28, 0.5), R = kepDec(t(29, 0));
  set(m, an::FA, A), set(m, an::FD, D), set(m, an::FS, S), set(m, an::FR, R);
  if (t(25, 0) > 0.5) {  // VCA gate
    set(m, an::AmpA, 0.002), set(m, an::AmpD, 0.3), set(m, an::AmpS, 1.0), set(m, an::AmpR, 0.01);
  } else {
    set(m, an::AmpA, A), set(m, an::AmpD, D), set(m, an::AmpS, S), set(m, an::AmpR, R);
  }
  // Arpeggiator.
  set(m, an::Arp, t(0, 0) > 0.5);
  set(m, an::ArpMode, idx(t(1, 0), 3));
  set(m, an::ArpOct, 1 + idx(t(2, 0), 3));
  set(m, an::ArpRate, 2);  // 1/16
  // Chorus modes off / I / II / I+II.
  int cho = idx(t(30, 0), 4);
  set(m, an::Chorus, cho > 0);
  set(m, an::ChoRate, cho == 1 ? 0.5 : cho == 2 ? 0.85 : 1.0);
  set(m, an::ChoDepth, cho == 3 ? 0.7 : 0.5);
  set(m, an::ChoMix, 0.5);
  set(m, an::Transpose, 12.0 * (idx(t(32, 0.5), 3) - 1));
  set(m, an::Volume, std::min(1.0, 1.4 * t(33, 0.5)));
  linkParams(m, {{5, an::L1Rate, kepLfoHz},
                 {10, an::L1Amt, [](double v) { return 0.25 * v; }},
                 {16, an::SubLevel, {}},
                 {17, an::Noise, {}},
                 {19, an::Cutoff, cutoffHz},
                 {20, an::Reso, [](double v) { return 0.95 * v; }},
                 {22, an::FltEnv, {}},
                 {23, an::L2Amt, {}},
                 {24, an::KeyTrack, {}},
                 {26, an::AmpA, kepAtk},
                 {27, an::AmpD, kepDec},
                 {28, an::AmpS, {}},
                 {29, an::AmpR, kepDec},
                 {33, an::Volume, [](double v) { return std::min(1.0, 1.4 * v); }}},
             // Sliders 0..6000, LFO rate 1..245760, volume 0..65536.
             {{5, 1, 245760}, {10, 0, 6000}, {16, 0, 6000}, {17, 0, 6000}, {19, 0, 6000}, {20, 0, 6000}, {22, 0, 6000},
              {23, 0, 6000}, {24, 0, 6000}, {26, 0, 6000}, {27, 0, 6000}, {28, 0, 6000}, {29, 0, 6000}, {33, 0, 65536}});
  return true;
}

// ------------------------------------------------------------------- Sawer
// Tags (float per tag): 0 volume, 1 pan, 2-5 amp ADSR, 6 transpose, 7 fine,
// 8 mono, 9/10 glide modes, 11 glide time, 12 octaver, 13 unison retrig,
// 14 unison voices, 15 unison detune, 16 unison pan, 17-20 sub-saw phase /
// harmonic / level / detune, 21-23 sub-saw switches, 24 sync freq, 25 sync,
// 26 noise, 27 cutoff, 28 reso, 29 key track, 30 velocity track, 31 filter
// mode, 32-35 filter ADSR, 36 filter env amount, 37 env destination, 38 LFO
// shape, 39 LFO speed, 40 LFO sync, 41/42 LFO attack / release, 43 retrig,
// 44 invert, 45 LFO amount, 46 LFO destination, 47-50 chorus (on, depth, rate,
// mix), 51-56 phaser, 57-64 delay (on, L, R, feedback, depth, lo, hi, mix),
// 65-69 reverb (on, decay, hi cut, damp, mix), 70-74 arpeggiator.
bool decodeSawer(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  TagFloats t = sslf(stateBlob(evs), "1rwS", 96);
  begin(m, "gl.analog");
  m.exact = false;
  if (!t.ok) return true;
  set(m, an::Volume, std::min(1.0, 1.4 * t(0, 0.5)));
  set(m, an::Pan, bip(t(1, 0.5)));
  set(m, an::AmpA, atk(t(2, 0))), set(m, an::AmpD, dec(t(3, 1))), set(m, an::AmpS, t(4, 1)), set(m, an::AmpR, rel(t(5, 0)));
  set(m, an::Transpose, std::round(48.0 * (t(6, 0.5) - 0.5)));
  set(m, an::Fine, 200.0 * (t(7, 0.5) - 0.5));
  set(m, an::VoiceMode, t(8, 0) > 0.5 ? 2 : 0);
  set(m, an::Glide, tlog(t(11, 0), 0.001, 2.0));
  static const int uni[4] = {1, 2, 4, 8};
  set(m, an::UniVoices, uni[idx(t(14, 0), 4)]);
  set(m, an::UniDetune, t(15, 0.25));
  set(m, an::UniSpread, t(16, 0.5));
  // Main saw + sub saw (harmonic h → h+1 times lower).
  set(m, an::O1On, 1), set(m, an::O1Wave, an::Saw), set(m, an::O1Level, 0.8);
  double subLvl = t(19, 0);
  int h = 1 + idx(t(18, 0), 9);
  set(m, an::O2On, subLvl > 0.001);
  set(m, an::O2Wave, t(23, 0) > 0.5 ? an::Pulse : an::Saw);
  set(m, an::O2Coarse, -std::round(12.0 * std::log2(static_cast<double>(h))));
  set(m, an::O2Fine, 100.0 * (t(20, 0.5) - 0.5));
  set(m, an::O2Level, 0.8 * subLvl);
  set(m, an::O3On, 0);
  if (t(25, 0) > 0.5) set(m, an::O1Mod, 1), set(m, an::O1ModAmt, t(24, 0.5));
  set(m, an::Noise, t(26, 0));
  static const int modes[4] = {an::Lp24, an::Bp12, an::Hp24, an::Notch};
  set(m, an::FltMode, modes[idx(t(31, 0), 4)]);
  set(m, an::Cutoff, cutoffHz(t(27, 1)));
  set(m, an::Reso, 0.95 * t(28, 0));
  set(m, an::KeyTrack, t(29, 0));
  set(m, an::VelCut, t(30, 0));
  set(m, an::FA, atk(t(32, 0))), set(m, an::FD, dec(t(33, 1))), set(m, an::FS, t(34, 1)), set(m, an::FR, rel(t(35, 1)));
  set(m, an::FltEnv, t(36, 0));
  static const int lfoShapes[4] = {0, 1, 3, 4};  // sine, triangle, saw, square
  set(m, an::L1Shape, lfoShapes[idx(t(38, 0), 4)]);
  set(m, an::L1Rate, tlog(t(39, 0.5), 0.05, 30.0));
  set(m, an::L1Sync, t(40, 0) > 0.5);
  set(m, an::L1Retrig, t(43, 0) > 0.5);
  static const int lfoDest[7] = {an::DPitch, an::DCutoff, an::DVolume, an::DPan, an::DPw, an::DReso, an::DOscMod};
  double lfoAmt = t(45, 0);
  set(m, an::L1Target, lfoAmt > 0 ? lfoDest[idx(t(46, 0), 7)] : an::DNone);
  set(m, an::L1Amt, (t(44, 0) > 0.5 ? -1 : 1) * lfoAmt * (idx(t(46, 0), 7) == 0 ? 0.25 : 1.0));
  analogFx(m, t(47, 0) > 0.5, t(49, 0.5), t(48, 0.5), t(50, 0.5), t(57, 0) > 0.5, 0.5 * (t(58, 0.5) + t(59, 0.5)),
           t(60, 0.3), t(64, 0.3), t(65, 0) > 0.5, t(66, 0.5), t(68, 0.5), t(69, 0.3));
  set(m, an::Arp, t(70, 0) > 0.5);
  set(m, an::ArpMode, idx(t(71, 0), 4));
  set(m, an::ArpOct, 1 + idx(t(72, 0), 4));
  linkParams(m, {{0, an::Volume, [](double v) { return std::min(1.0, 1.4 * v); }},
                 {1, an::Pan, bip},
                 {2, an::AmpA, atk},
                 {3, an::AmpD, dec},
                 {4, an::AmpS, {}},
                 {5, an::AmpR, rel},
                 {15, an::UniDetune, {}},
                 {26, an::Noise, {}},
                 {27, an::Cutoff, cutoffHz},
                 {28, an::Reso, [](double v) { return 0.95 * v; }},
                 {29, an::KeyTrack, {}},
                 {36, an::FltEnv, {}},
                 {39, an::L1Rate, [](double v) { return tlog(v, 0.05, 30.0); }},
                 {45, an::L1Amt, {}}},
             knobRanges({0, 1, 2, 3, 4, 5, 15, 26, 27, 28, 29, 36, 39, 45}));
  return true;
}

// ----------------------------------------------------------------- PoiZone
// Tags: 0 osc A shape, 1 A PW, 2 A sync, 3 A ring, 4 osc B shape, 5 B PW,
// 6 B pitch, 7 B detune, 8 osc balance, 9 noise balance, 10 EG amount,
// 11-14 EG ADSR, 15 gain, 16 fine, 17 transpose, 18 cutoff, 19 reso,
// 20 key track, 21 velocity track, 22-25 amp ADSR, 26 LFO amount, 27 LFO
// retrig, 28 LFO rate, 29/30 unison detune / pan, 31-38 delay, 39-42 chorus,
// 43 mod wheel amount, 44 mono, 45 octaver, 46 glide, 47/48 gate mix / smooth,
// 49-64 gate steps, 65 EG destination, 66 filter mode, 67 LFO destination,
// 68 LFO shape, 69/70 arp, 71 mod wheel destination, 72/73 glide modes,
// 75 unison (raw 0..6), 76 polyphony (raw), 77 arp sync, 78 bend range,
// 79 gate sync, 80 LFO sync.
bool decodePoiZone(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  TagFloats t = sslf(stateBlob(evs), "4reV", 81);
  begin(m, "gl.analog");
  m.exact = false;
  if (!t.ok) return true;
  double bal = t(8, 0.5);
  set(m, an::O1On, 1), set(m, an::O1Wave, t(0, 0) > 0.5 ? an::Pulse : an::Saw);
  set(m, an::O1Pw, 0.05 + 0.9 * t(1, 0.5));
  set(m, an::O1Level, 0.8 * std::min(1.0, 2.0 * (1.0 - bal)));
  set(m, an::O2On, 1), set(m, an::O2Wave, t(4, 0) > 0.5 ? an::Pulse : an::Saw);
  set(m, an::O2Pw, 0.05 + 0.9 * t(5, 0.5));
  set(m, an::O2Coarse, std::round(48.0 * (t(6, 0.5) - 0.5)));
  set(m, an::O2Fine, 100.0 * t(7, 0));
  set(m, an::O2Level, 0.8 * std::min(1.0, 2.0 * bal));
  if (t(2, 0) > 0.5) set(m, an::O2Mod, 1), set(m, an::O2ModAmt, 0.5);
  if (t(3, 0) > 0.5) set(m, an::O2Mod, 2), set(m, an::O2ModAmt, 1.0);
  set(m, an::O3On, 0);
  set(m, an::Noise, t(9, 0));
  // Envelope 2 → destination (0 cutoff, 1 pitch, 2 osc B pitch …).
  set(m, an::MA, atk(t(11, 0))), set(m, an::MD, dec(t(12, 1))), set(m, an::MS, t(13, 1)), set(m, an::MR, rel(t(14, 1)));
  double egAmt = bip(t(10, 0.5));
  int egDest = idx(t(65, 0), 4);
  if (egDest == 0) {
    set(m, an::FA, atk(t(11, 0))), set(m, an::FD, dec(t(12, 1))), set(m, an::FS, t(13, 1)), set(m, an::FR, rel(t(14, 1)));
    set(m, an::FltEnv, egAmt);
  } else {
    set(m, an::FltEnv, 0);
    set(m, an::ModEnvTarget, egDest == 1 ? an::DPitch : egDest == 2 ? an::DO2 : an::DPw);
    set(m, an::ModEnvAmt, egAmt);
  }
  set(m, an::Volume, std::min(1.0, 1.4 * t(15, 0.5)));
  set(m, an::Fine, 200.0 * (t(16, 0.5) - 0.5));
  set(m, an::Transpose, std::round(48.0 * (t(17, 0.5) - 0.5)));
  static const int modes[4] = {an::Lp24, an::Bp12, an::Hp24, an::Notch};
  set(m, an::FltMode, modes[idx(t(66, 0), 4)]);
  set(m, an::Cutoff, cutoffHz(t(18, 1)));
  set(m, an::Reso, 0.95 * t(19, 0));
  set(m, an::KeyTrack, t(20, 0));
  set(m, an::VelCut, t(21, 0));
  set(m, an::AmpA, atk(t(22, 0))), set(m, an::AmpD, dec(t(23, 1))), set(m, an::AmpS, t(24, 1)), set(m, an::AmpR, rel(t(25, 0)));
  double lfoAmt = bip(t(26, 0.5));
  static const int lfoDest[4] = {an::DCutoff, an::DPitch, an::DPw, an::DVolume};
  set(m, an::L1Target, std::fabs(lfoAmt) > 0.001 ? lfoDest[idx(t(67, 0), 4)] : an::DNone);
  set(m, an::L1Amt, lfoAmt * (idx(t(67, 0), 4) == 1 ? 0.25 : 1.0));
  set(m, an::L1Retrig, t(27, 0) > 0.5);
  set(m, an::L1Rate, tlog(t(28, 0.5), 0.05, 30.0));
  set(m, an::L1Shape, idx(t(68, 0), 5));
  double uniRaw = t(75, 0);
  int uniN = std::clamp(static_cast<int>(std::lround(uniRaw)), 0, 6);
  set(m, an::UniVoices, std::min(8, 1 + uniN));
  set(m, an::UniDetune, t(29, 0.5));
  set(m, an::UniSpread, t(30, 0.5));
  set(m, an::VoiceMode, t(44, 0) > 0.5 ? 2 : 0);
  set(m, an::Glide, tlog(t(46, 0), 0.001, 2.0));
  analogFx(m, t(39, 0) > 0.5, t(40, 0.5), t(41, 0.5), t(42, 0.5), t(31, 0) > 0.5, 0.5 * (t(32, 0.5) + t(33, 0.5)),
           t(34, 0.3), t(38, 0.3), false, 0.5, 0.5, 0.0);
  linkParams(m, {{9, an::Noise, {}},
                 {10, an::FltEnv, bip},
                 {15, an::Volume, [](double v) { return std::min(1.0, 1.4 * v); }},
                 {18, an::Cutoff, cutoffHz},
                 {19, an::Reso, [](double v) { return 0.95 * v; }},
                 {20, an::KeyTrack, {}},
                 {22, an::AmpA, atk},
                 {23, an::AmpD, dec},
                 {24, an::AmpS, {}},
                 {25, an::AmpR, rel},
                 {28, an::L1Rate, [](double v) { return tlog(v, 0.05, 30.0); }},
                 {29, an::UniDetune, {}}},
             knobRanges({9, 10, 15, 18, 19, 20, 22, 23, 24, 25, 28, 29}));
  return true;
}

// ---------------------------------------------------------------- SimSynth
// Doubles: 0-20 three oscillator blocks of 7 (level, LFO, pitch, env, PW,
// fine, shape/10), 21-25 LFO, 26-29 amp envelope, 30-44 filter and its
// envelope; switches follow as 32-bit integers.
bool decodeSimSynth(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.analog");
  m.exact = false;
  if (b.size() < 45 * 8) return true;
  auto d = [&](int i) {
    double v = b.f64(8 * static_cast<size_t>(i));
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.5;
  };
  static const int waves[6] = {an::Sine, an::Saw, an::Pulse, an::Tri, an::Sine, an::NoiseW};
  for (int o = 0; o < 3; ++o) {
    int base = 7 * o;
    int w = std::clamp(static_cast<int>(std::lround(d(base + 6) * 10.0)), 0, 5);
    int p = an::O1On + 8 * o;
    set(m, p + 0, 1);              // on
    set(m, p + 1, waves[w]);       // wave
    set(m, p + 4, 0.8 * d(base));  // level
    set(m, p + 5, 0.05 + 0.9 * d(base + 4));
  }
  set(m, an::AmpA, atk(d(26))), set(m, an::AmpD, dec(d(27))), set(m, an::AmpS, d(28)), set(m, an::AmpR, rel(d(29)));
  set(m, an::UniVoices, 1);
  return true;
}

// ------------------------------------------------------------------ Sakura
// Tags: 0 level, 1 tune, 2-5 amp ADSR, 6 string 1 decay, 7 decay inverted,
// 8 damp offset, 9 damp amount, 10 damp sharpness, 11-15 string 2 (same),
// 16 saturation, 17 string mix, 18 stereo separation, 19-23 comb filter,
// 24/25 lo-cut, 26-30 hi-cut ADSR / cutoff / reso, 31 exciter noise level,
// 32 exciter noise rate, 33 drive, 34-37 free EG, 38 LFO rate, 39-62
// resonators (freq, feedback, on × 8), 63-69 delay (on, time, feedback,
// speed, depth, blur, wet), 70-73 chorus (on, rate, depth, wet), 74-77 reverb
// (on, mix, decay, colour), 79-110 modulation slots, 111 string 2 ratio
// (raw 0..6), 112 delay sync, 113 transpose.
bool decodeSakura(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  TagFloats t = sslf(stateBlob(evs), "1rkS", 114);
  begin(m, "gl.string");
  m.exact = false;
  if (!t.ok) return true;
  set(m, st::Exciter, 0);
  set(m, st::Volume, std::min(1.0, 1.6 * t(0, 0.5)));
  set(m, st::Attack, tlog(t(2, 0), 0.001, 5.0));
  set(m, st::Sustain, t(4, 1));
  set(m, st::Release, tlog(t(5, 0.3), 0.01, 10.0));
  set(m, st::Decay, tlog(t(6, 0.8), 0.2, 12.0));
  set(m, st::Damping, t(9, 0.3));
  set(m, st::Stiffness, 0.3 * t(10, 0.2));
  set(m, st::Saturation, t(16, 0));
  // String 2: ratio choice (unison, octave, fifth …) and level from the mix.
  double mix = t(17, 0.5);
  static const int ratio[7] = {1, 2, 3, 4, 5, 2, 3};  // → Unison/Octave/Fifth/Oct down/Twelfth
  int r = std::clamp(static_cast<int>(std::lround(t(111, 0))), 0, 6);
  set(m, st::String2, mix > 0.02 ? ratio[r] : 0);
  set(m, st::Level2, mix);
  set(m, st::Detune, 20.0 * t(13, 0));
  set(m, st::Spread, t(18, 0.5));
  set(m, st::LoCut, tlog(t(24, 0), 20.0, 1000.0));
  set(m, st::HiCut, tlog(t(29, 1), 200.0, 20000.0));
  set(m, st::HiCutEnv, t(28, 0) < 0.99 ? t(26, 0) : 0.0);
  set(m, st::EnvDecay, tlog(t(27, 0.3), 0.01, 5.0));
  set(m, st::Noise, 0.5 * t(31, 0));
  set(m, st::Hardness, t(32, 0.5));
  set(m, st::Drive, t(33, 0));
  bool reson = false;
  for (int k = 55; k <= 62; ++k) reson = reson || t(k, 0) > 0.5;
  set(m, st::Body, reson ? 1 : 0);
  set(m, st::BodyMix, reson ? 0.4 : 0.0);
  set(m, st::Chorus, t(70, 0) > 0.5 ? t(73, 0.4) : 0.0);
  set(m, st::Delay, t(63, 0) > 0.5 ? t(69, 0.3) : 0.0);
  set(m, st::DelayTime, tlog(t(64, 0.5), 0.02, 1.5));
  set(m, st::Feedback, 0.9 * t(65, 0.5));
  set(m, st::Reverb, t(74, 0) > 0.5 ? t(75, 0.3) : 0.0);
  set(m, st::Room, t(76, 0.5));
  linkParams(m, {{0, st::Volume, [](double v) { return std::min(1.0, 1.6 * v); }},
                 {2, st::Attack, [](double v) { return tlog(v, 0.001, 5.0); }},
                 {4, st::Sustain, {}},
                 {5, st::Release, [](double v) { return tlog(v, 0.01, 10.0); }},
                 {6, st::Decay, [](double v) { return tlog(v, 0.2, 12.0); }},
                 {9, st::Damping, {}},
                 {16, st::Saturation, {}},
                 {17, st::Level2, {}},
                 {24, st::LoCut, [](double v) { return tlog(v, 20.0, 1000.0); }},
                 {29, st::HiCut, [](double v) { return tlog(v, 200.0, 20000.0); }},
                 {33, st::Drive, {}}},
             knobRanges({0, 2, 4, 5, 6, 9, 16, 17, 24, 29, 33}));
  return true;
}

// ---------------------------------------------------------------- Morphine
// The first "MBP0" spectrum (mask byte per 8 partials, one float per set bit)
// becomes the additive synth's drawn partial levels, relative to a saw.
bool decodeMorphine(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.additive");
  m.exact = false;
  float amp[ad::kDrawn] = {};
  int got = 0;
  size_t at = b.find("0PBM");
  if (at != std::string::npos) {
    at += 4;
    while (got < ad::kDrawn && b.has(at, 1)) {
      uint8_t mask = b.u8(at++);
      bool bad = false;
      for (int bit = 0; bit < 8 && got < ad::kDrawn; ++bit, ++got) {
        if (!(mask & (1u << bit))) continue;
        float v = b.f32(at);
        at += 4;
        if (!std::isfinite(v) || v < 0 || v > 16) {
          bad = true;
          break;
        }
        amp[got] = v;
      }
      if (bad) break;
    }
  }
  if (got >= 8 && amp[0] + amp[1] + amp[2] > 0) {
    float peak = 0;
    for (int k = 0; k < got; ++k) peak = std::max(peak, amp[k]);  // loudest partial → full level
    set(m, ad::SpecA, 0);  // saw
    set(m, ad::SpecB, 0);
    set(m, ad::Morph, 0);
    set(m, ad::Partials, got);
    for (int k = 0; k < ad::kDrawn; ++k) set(m, ad::Drawn + k, k < got && peak > 0 ? std::min(1.0f, amp[k] * static_cast<float>(k + 1) / peak) : 0.0);
  }
  return true;
}

// --------------------------------------------------------- Toxic Biohazard
// The FM engine block is not decoded; the instrument imports as a plain
// detuned-saw analog patch so the channel stays audible.
bool decodeToxic(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.analog");
  m.exact = false;
  (void)b;
  set(m, an::O1Wave, an::Saw), set(m, an::O2Wave, an::Saw), set(m, an::O2Fine, 8);
  set(m, an::UniVoices, 2);
  set(m, an::Cutoff, 6000.0);
  return true;
}

}  // namespace

void registerSynthsB(std::map<std::string, PluginDecoder>& d) {
  add(d, "Kepler", decodeKepler);
  add(d, "Sawer", decodeSawer);
  add(d, "PoiZone", decodePoiZone);
  add(d, "SimSynth", decodeSimSynth);
  add(d, "Sakura", decodeSakura);
  add(d, "Morphine", decodeMorphine);
  add(d, "Toxic Biohazard", decodeToxic);
}

}  // namespace gl::flp::gen2
