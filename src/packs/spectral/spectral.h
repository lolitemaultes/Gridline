// Gridline — "spectral" pack: vocoder, pitch shifter, pitch corrector,
// scratcher, band splitter and spreader. Parameter indices, live telemetry
// (audio thread -> editors) and small helpers shared by the info, DSP and UI
// files.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../engine/Effects.h"

namespace gl::packs::spectral {

// ------------------------------------------------------------------ Vocoder
enum VocParam {
  VcBands,         // choice: band count
  VcLowFreq,       // Hz
  VcHighFreq,      // Hz
  VcSpacing,       // -1..1 (0 = logarithmic)
  VcFormant,       // st: analysis bands shifted against synthesis bands
  VcBandwidth,     // relative to band spacing
  VcOrder,         // choice: filter slope
  VcAttack,        // ms
  VcRelease,       // ms
  VcHold,          // toggle: freeze the modulator envelope
  VcInvert,        // toggle: mirror the band mapping
  VcCarrier,       // choice: carrier source
  VcRoot,          // note: built-in carrier root
  VcChord,         // choice: built-in carrier chord
  VcFlatten,       // 0..1: carrier whitening
  VcSibilance,     // 0..1: unvoiced (noise) detection & mix
  VcSpread,        // 0..1: alternate bands left/right
  VcModLevel,      // modulator pass-through
  VcCarLevel,      // carrier pass-through
  VcWetLevel,      // vocoded output
  VcSwap,          // toggle: modulator right / carrier left
  VcListen,        // choice: wet / modulator / carrier
  VcContour0,      // 8 contour points (dB), low to high band
  VcContourLast = VcContour0 + 7,
  VcModSource,     // choice (0.5 FL import): modulator from the L/R pair or from the sidechain input
  // ---- J-1 reference model (appended; VxModel = Legacy keeps the 0.5 engine above) ----
  VxModel,         // choice Legacy / Reference (pre-J-1 states normalise to Legacy)
  VxBands,         // 5..100 bands
  VxOrder,         // choice: resonators per band 1..4
  VxFlatness,      // 0..1 stagger spread of the resonators
  VxWidth,         // band width (reference display units 0.008..4)
  VxModWidth,      // modulator (analysis) band width multiplier 0.25..4
  VxHoldMs,        // ms
  VxAttackMs,      // ms
  VxAttCurve,      // 1..4 (exponent)
  VxReleaseMs,     // ms
  VxRelCurve,      // 1..4
  VxHoldMode,      // choice Peak / RMS
  VxMinTimes,      // toggle
  VxModHp,         // Hz; <= kVxOffLow = off
  VxCarLp,         // Hz; >= kVxOffHigh = off
  VxNoiseHp,       // Hz; <= kVxOffLow = off
  VxNoise,         // carrier added noise level (raw 0..1)
  VxEnhance,       // output enhancer amount 0..1
  VxUnison,        // analysis unison copies 1..5
  VxUnisonShift,   // 0..1
  VxUnisonPan,     // -1..1
  VxContourMod,    // toggles: contour applied to the modulator pass-through / the carrier noise,
  VxContourNoise,  //   taken from the carrier / gated by consonants
  VxContourCar,
  VxContourCons,
  VxDenoise,       // toggle: subtract the modulator noise floor (else gate)
  VxCarSource,     // choice: built-in synth / input / sidechain
  VxLrEncoding,    // toggle: input L = modulator, R = carrier
  VxTone,          // choice: built-in carrier tone
  VxCarVoices,     // 1..4 carrier unison voices
  VxCarDetune,     // ct
  VxMidiPort,      // -1 = held keys only, else MIDI port whose notes play the carrier
  VxEditTarget,    // choice: curve shown in the band editor (UI state)
  VcCount
};
inline constexpr int kVxMaps = 13;       // per-band curves kept in PluginState::data
inline constexpr int kVxMaxBands = 100;
inline constexpr int kVxMaxOrder = 4;
inline constexpr int kVxMaxUnison = 5;
inline constexpr double kVxOffLow = 39.5, kVxOffHigh = 15000.5;
// Pass-through / noise filter wheels (0..65536 raw): Hz = 40 * 375^(v/65536).
inline double vxFilterHz(double raw) { return 40.0 * std::pow(375.0, raw / 65536.0); }
// Frequency scale of the band distribution map (y 0..1 -> Hz).
inline double vxScaleHz(double y) {
  const double lo = 65.40639132514966, hi = 21096.16376864;  // notes 24 and 124 (A4 = note 57)
  const double k = std::log(2.0) / std::log(2.0 * hi / lo), L = std::log(hi / lo + 1.0);
  double f = y < k ? lo * y / k : lo + (hi - lo) * (std::exp((y - k) * L / (1.0 - k)) - 1.0) * lo / hi;
  return f < 20.0 ? 20.0 : f;
}
// Envelope times (raw 0..10000 -> ms) and curve (0..2000 -> exponent).
inline double vxHoldMs(double raw) { return 0.5 * (std::pow(501.0, raw / 1e4) - 1.0); }
inline double vxAttackMs(double raw) { return 2.0 * (std::pow(501.0, raw / 1e4) - 1.0); }
inline double vxReleaseMs(double raw) { return 4.0 * (std::pow(501.0, raw / 1e4) - 1.0); }
// Map value -> quantity for the 13 editor targets (y 0..1).
enum VxMap { VmGain, VmPan, VmOffset, VmNoise, VmPass, VmHold, VmAttack, VmRelease, VmDist, VmWidth, VmPitch, VmSatMix, VmSatCurve };
inline double vxMapGain(double y) { return (std::pow(3.0, 2.0 * y) - 1.0) / 2.0; }
inline double vxMapTime(double y) { return std::pow(2.0, 6.0 * y) / 8.0; }
inline double vxMapWidth(double y) { return std::pow(2.0, 4.0 * y) / 4.0; }
inline double vxMapNoise(double y) { return (std::pow(1025.0, y) - 1.0) / 1024.0; }
inline double vxMapOffset(double y) {
  double s = 2.0 * y - 1.0;
  return (s < 0 ? -1.0 : 1.0) * (std::pow(1025.0, std::fabs(s)) - 1.0) / 1024.0;
}
// Stagger tuning of the `order` resonators of one band of `width` octaves:
// centre offsets (octaves) and resonator widths (octaves).
inline void vxStagger(int order, double flat, double width, double* offOct, double* widthOct) {
  for (int s = 0; s < order; ++s) {
    offOct[s] = order > 1 ? (static_cast<double>(s) / (order - 1) - 0.5) * flat * width : 0.0;
    double ws = width / (0.5 * flat + 1.0);
    if (order == 3 && s != 1) ws *= 0.5;
    if (order == 4 && (s == 0 || s == 3)) ws *= 0.3636;
    widthOct[s] = std::min(ws, 2.0);
  }
}
// Built-in carrier tone family for a saved carrier tone name (keyword match;
// VxTone's approximate families, used when the tone's preset is not there).
inline int vxToneFamily(std::string name) {
  for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto has = [&](const char* k) { return name.find(k) != std::string::npos; };
  if (has("noise") || has("breath") || has("whisper")) return 8;
  if (has("organ")) return 6;
  if (has("string") || has("ensemble") || has("pad") || has("choir") || has("phase")) return 5;
  if (has("super") || has("stack") || has("unison") || has("group")) return 4;
  if (has("pwm") || has("pulse")) return 2;
  if (has("square")) return 1;
  if (has("tri") || has("sine") || has("soft")) return 3;
  if (has("growl") || has("power") || has("buzz") || has("elderly") || has("robot")) return 7;
  return 0;
}
// Per-band curves, pitch-follow flags and held carrier keys, kept in
// PluginState::data as versioned text (parsed on the UI thread):
//   vx1\n m<i>=dx:y:t;dx:y:t;…\n pf=0001100000000\n keys=60,64\n
// dx = distance from the previous point (x positions are normalised by the
// total), y 0..1, t = segment tension -1..1.
struct VxPoint {
  double dx = 0, y = 0, t = 0;
};
struct VxData {
  std::vector<VxPoint> maps[kVxMaps];
  bool pf[kVxMaps] = {};
  std::vector<int> keys;  // empty: built-in carrier root + chord parameters
  std::string extra;      // J-42: kept verbatim ("sge ...": the enhancer's mastering program)
  // J-65: the imported built-in carrier tone ("tone=<name>"; empty name = no tone loaded).
  bool hasTone = false;
  std::string tone;
};
inline constexpr double kVxDistLo = 0.14293710887432098, kVxDistHi = 0.9917563796043396;
inline double vxMapDefault(int m, double x) {
  switch (m) {
    case VmNoise: case VmPass: case VmSatMix: return 0.0;
    case VmDist: return kVxDistLo + (kVxDistHi - kVxDistLo) * x;
    case VmSatCurve: return x;
    default: return 0.5;
  }
}
inline VxData vxDefaultData() {
  VxData d;
  for (int m = 0; m < kVxMaps; ++m) d.maps[m] = {{0, vxMapDefault(m, 0), 0}, {1, vxMapDefault(m, 1), 0}};
  d.pf[VmNoise] = d.pf[VmPass] = true;
  return d;
}
// Segment shape for a tension t (-1..1; 0 = straight line).
inline double vxTension(double u, double t) {
  if (std::fabs(t) < 1e-4) return u;
  double e = std::pow(2.0, -4.0 * t);  // t > 0: slow start, t < 0: fast start
  return t > 0 ? std::pow(u, 1.0 / e) : 1.0 - std::pow(1.0 - u, e);
}
inline double vxEval(const std::vector<VxPoint>& pts, double x) {
  if (pts.empty()) return 0.5;
  double total = 0;
  for (size_t i = 1; i < pts.size(); ++i) total += std::max(0.0, pts[i].dx);
  if (pts.size() == 1 || total <= 0) return pts[0].y;
  x = std::clamp(x, 0.0, 1.0) * total;
  double px = 0;
  for (size_t i = 1; i < pts.size(); ++i) {
    double nx = px + std::max(0.0, pts[i].dx);
    if (x <= nx || i + 1 == pts.size()) {
      double u = nx > px ? std::clamp((x - px) / (nx - px), 0.0, 1.0) : 1.0;
      return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * vxTension(u, pts[i].t);
    }
    px = nx;
  }
  return pts.back().y;
}
inline std::string vxSerialize(const VxData& d) {
  std::string s = "vx1\n";
  char buf[96];
  for (int m = 0; m < kVxMaps; ++m) {
    s += "m" + std::to_string(m) + "=";
    for (size_t i = 0; i < d.maps[m].size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%.9g:%.9g:%.6g", i ? ";" : "", d.maps[m][i].dx, d.maps[m][i].y, d.maps[m][i].t);
      s += buf;
    }
    s += "\n";
  }
  s += "pf=";
  for (bool b : d.pf) s += b ? '1' : '0';
  s += "\nkeys=";
  for (size_t i = 0; i < d.keys.size(); ++i) s += (i ? "," : "") + std::to_string(d.keys[i]);
  s += "\n";
  if (d.hasTone) s += "tone=" + d.tone + "\n";
  if (!d.extra.empty()) s += d.extra + "\n";
  return s;
}
// False (and defaults) when `s` is not vocoder data.
inline bool vxParse(const std::string& s, VxData& d) {
  d = vxDefaultData();
  if (s.compare(0, 4, "vx1\n") != 0) return false;
  size_t pos = 4;
  while (pos < s.size()) {
    size_t e = s.find('\n', pos);
    if (e == std::string::npos) e = s.size();
    std::string line = s.substr(pos, e - pos);
    pos = e + 1;
    if (line.compare(0, 4, "sge ") == 0) {
      d.extra = line;
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k.size() > 1 && k[0] == 'm') {
      int m = std::atoi(k.c_str() + 1);
      if (m < 0 || m >= kVxMaps) continue;
      std::vector<VxPoint> pts;
      const char* c = v.c_str();
      while (*c && pts.size() < 4096) {
        VxPoint p;
        char* end = nullptr;
        p.dx = std::strtod(c, &end);
        if (end == c || *end != ':') break;
        c = end + 1;
        p.y = std::strtod(c, &end);
        if (end == c || *end != ':') break;
        c = end + 1;
        p.t = std::strtod(c, &end);
        if (end == c) break;
        c = end;
        if (std::isfinite(p.dx) && std::isfinite(p.y) && std::isfinite(p.t))
          pts.push_back({std::clamp(p.dx, 0.0, 1e6), std::clamp(p.y, 0.0, 1.0), std::clamp(p.t, -1.0, 1.0)});
        if (*c == ';') ++c;
      }
      if (!pts.empty()) d.maps[m] = std::move(pts);
    } else if (k == "tone") {
      d.hasTone = true;
      d.tone = v;
    } else if (k == "pf") {
      for (int m = 0; m < kVxMaps && static_cast<size_t>(m) < v.size(); ++m) d.pf[m] = v[static_cast<size_t>(m)] == '1';
    } else if (k == "keys") {
      d.keys.clear();
      const char* c = v.c_str();
      while (*c && d.keys.size() < 16) {
        char* end = nullptr;
        long n = std::strtol(c, &end, 10);
        if (end == c) break;
        if (n >= 0 && n < 128) d.keys.push_back(static_cast<int>(n));
        c = *end == ',' ? end + 1 : end;
      }
    }
  }
  return true;
}
inline constexpr int kVocContour = 8;
inline constexpr int kVocMaxBands = 128;
inline constexpr int kVocBandChoices[] = {4, 8, 12, 16, 24, 32, 48, 64, 96, 128};
inline constexpr int kVocBandChoiceCount = 10;
inline int vocBandCount(float choice) {
  int i = static_cast<int>(choice + 0.5f);
  i = i < 0 ? 0 : (i >= kVocBandChoiceCount ? kVocBandChoiceCount - 1 : i);
  return kVocBandChoices[i];
}
// Centre frequency of band b of n between lo and hi; spacing 0 = log,
// +1 = linear, -1 = denser at the bottom than log.
inline double vocBandFreq(int b, int n, double lo, double hi, double spacing) {
  if (hi < lo * 1.05) hi = lo * 1.05;
  double t = n > 1 ? static_cast<double>(b) / (n - 1) : 0.5;
  double logF = lo * std::pow(hi / lo, t);
  if (spacing >= 0) {
    double linF = lo + (hi - lo) * t;
    return logF + (linF - logF) * spacing;
  }
  double t2 = std::pow(t, 1.0 - spacing);  // spacing < 0: exponent > 1
  return logF + (lo * std::pow(hi / lo, t2) - logF) * (-spacing);
}
// Contour gain (dB) for a band position t (0..1) from the 8 points.
inline double vocContourDb(const float* pts, double t) {
  double x = t * (kVocContour - 1);
  int i = static_cast<int>(x);
  if (i >= kVocContour - 1) return pts[kVocContour - 1];
  if (i < 0) return pts[0];
  double f = x - i;
  return pts[i] + (pts[i + 1] - pts[i]) * f;
}

// ------------------------------------------------------------ Pitch shifter
enum PitchParam {
  PsMode,       // choice: Fast (delay) / Spectral (phase vocoder)
  PsPitch,      // st
  PsFine,       // ct
  PsFormant,    // st (spectral mode)
  PsPreserve,   // toggle: keep formants (spectral mode)
  PsGrain,      // ms (fast mode window)
  PsDetune,     // ct: stereo detune (left down, right up)
  PsDelay,      // ms: delay after the shifter
  PsFeedback,   // 0..1: delay output fed back into the shifter
  PsOutput,     // dB
  PsMix,        // 0..1 dry/wet (0.5: FL import; the dry path is latency-aligned)
  // ---- J-1 reference engines (appended; pre-J-1 states normalise PsEngine to Classic) ----
  PsEngine,     // choice: Classic (PsMode engines) / Voice (PSOLA) / Music (grains) / Hi-fi (J-87)
  PsCorrection, // 0..1 formant correction (Voice): formant st = formant + pitch * (1 - corr)
  PsDuration,   // ms 20..150: grain length (Music)
  PsDensity,    // 1..5: grain overlap (Music)
  PsJitter,     // 0..1: random grain spacing (Music)
  PsRand,       // 0..1: random read position (Music)
  PsDelayMode,  // choice: Off / Post shift / Pre shift / No shift
  PsDelaySync,  // toggle: delay in 1/16 steps (PsDelaySteps) instead of ms (PsDelay)
  PsDelaySteps, // 0..16 steps (1 step = 48 ticks = a sixteenth note)
  PsAnalysis,   // choice: Left / Right / Left + Right (Voice pitch analysis)
  PsPitchMode,  // choice: Relative / Absolute C4 / C5 / C6 (Voice)
  PsDetector,   // choice: Fast / Accurate (Voice analysis window)
  PsCount
};
inline constexpr int kPsClassic = 0, kPsVoice = 1, kPsMusic = 2;
// J-87: Hi-fi engine (engine/TimeStretch: spectral time stretch + band-limited
// resampling, formant keep). Gridline's own; exported to FL as Music.
inline constexpr int kPsHifi = 3;
enum PsDelayModes { PsDlyOff = 0, PsDlyPost, PsDlyPre, PsDlyNoShift };
// FL delay word 1..768 → ms (free) and 1/16 steps (tempo sync).
inline double psDelayMs(double raw) { return 1.0 + 999.0 * (raw - 1.0) / 767.0; }
inline double psDelaySteps(double raw) { return 16.0 * (raw - 1.0) / 767.0; }
// Absolute pitch-mode note frequencies (C4 / C5 / C6 in the reference's octave naming).
inline constexpr double kPsAbsHz[3] = {130.8128, 261.6256, 523.2511};
// Voice engine formant shift (st) with continuous formant correction.
inline double psFormantSt(double pitchSt, double formantSt, double corr) {
  double f = formantSt + (corr < 1.0 ? pitchSt * (1.0 - corr) : 0.0);
  return f < -12 ? -12 : (f > 12 ? 12 : f);
}

// ---------------------------------------------------------- Pitch corrector
enum PitcherParam {
  PcKey,        // choice C..B
  PcScale,      // choice (last = custom)
  PcSpeed,      // ms: correction glide (0 = hard)
  PcAmount,     // 0..1 correction strength
  PcLowest,     // choice: detector range
  PcReference,  // Hz: A4
  PcFormant,    // st: gender / formant shift
  PcPreserve,   // toggle: keep formants while correcting
  PcVoice1, PcVoice1Level, PcVoice1Pan,  // harmony voices: interval st, level, pan
  PcVoice2, PcVoice2Level, PcVoice2Pan,
  PcVoice3, PcVoice3Level, PcVoice3Pan,
  PcDiatonic,   // toggle: snap harmony voices into the scale
  PcLeadLevel,  // corrected lead voice level
  PcLeadPan,
  PcNote0,      // 12 custom scale toggles (C..B)
  PcNoteLast = PcNote0 + 11,
  // ---- J-1 reference model (appended; pre-J-1 states normalise PcModel to Legacy) ----
  PcModel,       // choice: Legacy / Reference
  PcCorrSpeed,   // 0..1 correction speed (reference glide law, 1 = instant)
  PcFine,        // ct ±100, added to every correction
  PcStereo,      // 0..1: voice pan spread
  PcRefChannel,  // choice: Left / Both / Right (which input channel drives the detection)
  PcBypassAll,   // toggle: no correction at all
  PcKeyBypass0,  // 12 toggles: no correction while the sung note is C..B
  PcKeyBypassLast = PcKeyBypass0 + 11,
  PcRange6,      // choice: lowest pitch 220 / 170 / 110 / 80 / 55 / 25 Hz
  PcMidiTargets, // toggle: correct to the held MIDI notes instead of the scale
  PcMidiOctaves, // toggle: MIDI notes keep their octave
  PcHarmonize,   // toggle: voices from the held MIDI notes
  PcHarmPan1,    // 4 voice pans (-1..1)
  PcHarmPanLast = PcHarmPan1 + 3,
  PcHarmMix,     // toggle: harmonize adds the dry input (else replaces it)
  PcHarmVel,     // toggle: voice level follows velocity (2 v²)
  PcMidiPort,    // -1..255: MIDI port bus (-1 = all ports)
  PcCount
};
inline constexpr double kPcRangeHz[6] = {220, 170, 110, 80, 55, 25};
// Reference correction-speed law: per-sample glide coefficient (1 = instant).
inline double pitcherGlideAlpha(double speed, double sr) {
  double x = 1.0 - (speed < 0 ? 0 : (speed > 1 ? 1 : speed));
  if (x < 0.01) return 1.0;
  double a = 8.82 / (sr * (3.0 * x * x * x + 0.001));
  return a > 1.0 ? 1.0 : a;
}
// Glide time constant (ms) of a correction speed (0 = instant).
inline double pitcherGlideMs(double speed) {
  double x = 1.0 - (speed < 0 ? 0 : (speed > 1 ? 1 : speed));
  return x < 0.01 ? 0.0 : 1000.0 * (3.0 * x * x * x + 0.001) / 8.82;
}
// Reference voice pan law: balance gains of pan p (-1..1) scaled by the stereo spread.
inline void pitcherPanGains(double pan, double spread, double& gl, double& gr) {
  double d = pan * spread;
  gl = 1.0 - d;
  gr = 1.0 + d;
  gl = gl < 0 ? 0 : (gl > 1 ? 1 : gl);
  gr = gr < 0 ? 0 : (gr > 1 ? 1 : gr);
}
// Telemetry (reference model): wave[0..127] = notes held on the MIDI port
// (velocity 0..1), wave[128..131] = sounding voice pitches (MIDI, 0 = silent),
// v[TvPcCorrection] = applied correction (st), v[TvPcHarm] = harmonize active.
inline constexpr int TvPcCorrection = 5, TvPcHarm = 6, TvPcInput = 7;
inline constexpr int kScaleCount = 10;  // index kScaleCount-1 = Custom
inline constexpr uint16_t kScaleMasks[kScaleCount - 1] = {
    0xFFF,                                                   // chromatic
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),  // major
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),  // natural minor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),  // harmonic minor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),  // dorian
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),  // mixolydian
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),   // major pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),  // minor pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),  // blues
};
// Enabled pitch classes (bit 0 = C) for a plugin state's key/scale/custom notes.
inline uint16_t pitcherMask(const float* p) {
  int scale = static_cast<int>(p[PcScale] + 0.5f);
  if (scale >= 0 && scale < kScaleCount - 1) {
    int key = static_cast<int>(p[PcKey] + 0.5f) % 12;
    uint16_t m = kScaleMasks[scale];
    return static_cast<uint16_t>(((m << key) | (m >> (12 - key))) & 0xFFF);
  }
  uint16_t m = 0;
  for (int i = 0; i < 12; ++i)
    if (p[PcNote0 + i] >= 0.5f) m = static_cast<uint16_t>(m | (1 << i));
  return m ? m : 0xFFF;
}
// Nearest enabled note (fractional MIDI in, integer MIDI out).
inline int nearestInMask(double midi, uint16_t mask) {
  int c = static_cast<int>(std::floor(midi + 0.5));
  for (int d = 0; d <= 12; ++d) {
    int lo = c - d, hi = c + d;
    bool okLo = mask & (1 << (((lo % 12) + 12) % 12)), okHi = mask & (1 << (((hi % 12) + 12) % 12));
    if (okLo && okHi) return std::fabs(midi - lo) <= std::fabs(midi - hi) ? lo : hi;
    if (okLo) return lo;
    if (okHi) return hi;
  }
  return c;
}

// --------------------------------------------------------------- Scratcher
enum ScratchParam {
  ScHand,       // 0..1 platter (hand) position
  ScGrab,       // toggle: hand holds the platter
  ScMotor,      // toggle: motor on
  ScStart,      // s: motor start time
  ScStop,       // s: motor stop (brake) time
  ScRange,      // s: buffer length covered by the hand range
  ScSmooth,     // ms: hand smoothing
  ScPattern,    // choice: auto-scratch pattern
  ScRate,       // choice: pattern rate
  ScFader,      // 0..1 crossfader (0 = live input, 1 = deck)
  ScCurve,      // choice: crossfader curve
  ScVolume,     // dB
  ScPan,        // pan
  ScCount
};

// ----------------------------------------------------------- Band splitter
enum SplitParam {
  SpBands,      // choice: 2..4
  SpSlope,      // choice: 12/24/48 dB
  SpFreq1, SpFreq2, SpFreq3,  // Hz
  SpGain0,      // per band: gain, pan, width, mute, solo (stride 5)
  // ---- J-1 reference model (appended) ----
  SpModel = SpGain0 + 4 * 5,  // choice Legacy / Reference (pre-J-1 states normalise to Legacy)
  SpSlopeA,     // choice 6..96 dB: first crossover
  SpSlopeB,     // second crossover
  SpSlopeC,     // third crossover (4 bands, Gridline extra)
  SpLinear,     // toggle: linear-phase (FFT) mode
  SpQuality,    // choice: FFT size 512 * 2^q (scaled by the sample rate)
  SpLinSpeed,   // ms: crossfade time when the linear-phase filters are redesigned
  SpMonitor,    // choice (display): graph shows main / send levels
  SpLinked,     // toggle (editor): crossover handles move together
  SpViewMain,   // choice (display): band curves / summed curve, main group
  SpViewSend,   // same, send group
  SpBand2,      // per band: main level, send level, send mute, send to (stride 4)
  // J-45: the band send targets are FL route indexes of the hosting track (PluginInfo::routeModeParam).
  SpRoute = SpBand2 + 4 * 4,
  SpCount
};
inline constexpr int kSplitStride = 5;
enum SplitBandField { SbGain = 0, SbPan, SbWidth, SbMute, SbSolo };
inline int splitParam(int band, int field) { return SpGain0 + band * kSplitStride + field; }
inline constexpr int kSplitStride2 = 4;
enum SplitBandField2 { SxMain = 0, SxSend, SxSendMute, SxSendTo };
inline int splitParam2(int band, int field) { return SpBand2 + band * kSplitStride2 + field; }
inline constexpr int kSplitSlopes = 8;
inline constexpr int kSplitSlopeDb[kSplitSlopes] = {6, 12, 18, 24, 36, 48, 72, 96};
inline constexpr double kSplitSilentDb = -100.0;  // level wheels: at or below = silence
// Reference level wheel (raw -1800..1800) -> amplitude: linear below the
// centre, +18 dB at the top.
inline double splitLevelAmp(double raw) {
  raw = raw < -1800 ? -1800 : (raw > 1800 ? 1800 : raw);
  return raw < 0 ? (raw + 1800.0) / 1800.0 : 1.0 + 6.943282 * raw / 1800.0;
}
inline double splitLevelDb(double raw) {
  double a = splitLevelAmp(raw);
  return a <= 1e-5 ? kSplitSilentDb : std::max(kSplitSilentDb, 20.0 * std::log10(a));
}
inline double splitLevelGain(double db) { return db <= kSplitSilentDb + 0.01 ? 0.0 : std::pow(10.0, db / 20.0); }
// Reference crossover wheel (raw 0..65536) -> Hz (30 Hz .. 16 kHz).
inline double splitRefHz(double raw) { return 30.0 * std::pow(16000.0 / 30.0, raw / 65536.0); }
// Linear-phase FFT size for a quality index at a sample rate.
inline int splitFftSize(int quality, double sr) {
  quality = quality < 0 ? 0 : (quality > 5 ? 5 : quality);
  int n = 512 << quality;
  int sh = static_cast<int>(std::lround(std::log2(std::max(8000.0, sr) / 44100.0)));
  n = sh >= 0 ? n << sh : n >> (-sh);
  return n < 256 ? 256 : (n > 65536 ? 65536 : n);
}
inline double splitLinSpeedMs(double raw) { return 2.0 * std::pow(10.0, 2.0 * raw / 65536.0); }

// ----------------------------------------------------------------- Spreader
enum SpreadParam {
  SrMode,       // choice: widening method
  SrSpread,     // 0..1 amount
  SrSeparation, // -1..1 (negative narrows)
  SrBassMono,   // toggle
  SrBassFreq,   // Hz
  SrOffset,     // ms: left/right time offset
  SrInvert,     // choice: none / left / right / both
  SrPosition,   // choice: separation before / after the widener
  SrPan,
  SrVolume,     // dB
  SrMonoCheck,  // toggle: listen in mono
  SrDcBlock,    // toggle: remove DC offset
  // ---- J-1 reference model (appended; pre-J-1 states normalise SrModel to Legacy) ----
  SrModel,       // choice: Legacy / Reference
  SrLowBypass,   // toggle: lows below SrLowFreq bypass the widener (keep their stereo)
  SrLowFreq,     // Hz 10..10000: split frequency (low bypass and mono bass)
  SrEnabled,     // toggle: engine on (2 ms crossfade)
  SrPreMonitor,  // toggle (display): input trace + correlation
  SrPostMonitor, // toggle (display): output trace + correlation
  SrCount
};
// Reference low-bypass word (1000..1000000) → Hz: 0..500 Hz over the first
// half of the range, 500 Hz..10 kHz over the second.
inline double spreadLowHz(double raw) {
  double n = (raw - 1000.0) / 999000.0;
  n = n < 0 ? 0 : (n > 1 ? 1 : n);
  return n <= 0.5 ? 1000.0 * n : 500.0 + 19000.0 * (n - 0.5);
}
inline constexpr double kSpreadMaxDelayMs = 6.3492;  // decorrelation tap at full spread
inline constexpr double kSpreadFadeMs = 23.2;        // tap crossfade when the delay changes
// Telemetry (reference model): v[TvSrPreCorr] = input correlation; a[]/b[] = input trace (L/R).
inline constexpr int TvSrPreCorr = 10;

// ---------------------------------------------------------------- telemetry
// Written by the audio thread (relaxed atomics), read by the editors through
// the slot runtime's effect instance.
struct Telemetry {
  std::atomic<int> count{0};
  std::atomic<float> a[kVocMaxBands];   // vocoder: modulator band level; splitter: band peak
  std::atomic<float> b[kVocMaxBands];   // vocoder: output band level
  std::atomic<float> v[16];             // per-plugin scalars (see each effect)
  std::atomic<float> wave[256];         // scratcher buffer overview; spreader vectorscope (pairs)
  std::atomic<int> wavePos{0};
  Telemetry() {
    for (auto& x : a) x.store(-120.f, std::memory_order_relaxed);
    for (auto& x : b) x.store(-120.f, std::memory_order_relaxed);
    for (auto& x : v) x.store(0.f, std::memory_order_relaxed);
    for (auto& x : wave) x.store(0.f, std::memory_order_relaxed);
  }
};
// Scalar slots.
enum TelSlot {
  TvLatency = 0,     // samples
  TvDetected = 1,    // pitcher: detected MIDI pitch (0 = unvoiced)
  TvTarget = 2,      // pitcher: target MIDI note
  TvOutput = 3,      // pitcher: output MIDI pitch
  TvConfidence = 4,  // pitcher: 0..1
  TvAngle = 5,       // scratcher: platter angle (turns)
  TvVelocity = 6,    // scratcher: platter speed (1 = normal)
  TvLag = 7,         // scratcher: read position behind the input (s)
  TvCorrelation = 8, // spreader: -1..1
  TvWidth = 9,       // spreader: side/mid ratio
  TvUnvoiced = 10,   // vocoder: unvoiced detector 0..1
  TvHandPos = 11,    // scratcher: effective hand position 0..1
};

class SpectralFx : public Effect {
 public:
  Telemetry tel;
};

}  // namespace gl::packs::spectral
