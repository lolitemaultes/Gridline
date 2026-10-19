// Gridline — FLP import, third wave: instruments whose shipped presets had no
// decoder yet.
//   Plucked!         → gl.pluck
//   Transistor Bass  → gl.acid
//   Fruity Slicer 2  → gl.slicer
#include <algorithm>
#include <cmath>
#include "../PluginParams.h"
#include "FlpDec3.h"

namespace gl::flp::dec3 {

namespace {

using namespace gl::pp;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }
double logMap(double u, double a, double b) { return a * std::pow(b / a, clampd(u, 0, 1)); }

// ---------------------------------------------------------------- Plucked!
// 5 × i32 (tags 0..4): decay 0..128, colour 0..128, normalize (0/1), gate
// (short release, 0/1), widen (0/1).
double pluckDecay(double v) { return logMap(v / 128.0, 0.1, 20.0); }

bool decodePlucked(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 20) return false;
  gen2::begin(m, "gl.pluck");
  std::vector<double> v(5);
  for (size_t i = 0; i < 5; ++i) v[i] = b.i32(4 * i);
  auto flag = [](double x) { return x >= 0.5 ? 1.0 : 0.0; };
  applyRows(m,
            {{0, 0, 128, PlkDecay, pluckDecay},
             {1, 0, 128, PlkBrightness, [](double x) { return clampd(x / 128.0, 0, 1); }},
             {2, 0, 1, PlkNormalize, flag},
             {3, 0, 1, PlkGate, flag},
             {4, 0, 1, PlkWidth, [](double x) { return x >= 0.5 ? 0.8 : 0.0; }}},
            v);
  m.exact = false;  // decay curve and string model differ
  return true;
}

// --------------------------------------------------------- Transistor Bass
// A container (header, then the program list: 128 entries of name, patch
// and sequence index) followed by 128 patches: u32 name length, the name,
// then 100 floats 0..1 — float k = parameter tag k (1 waveform, 2 cutoff,
// 3 key follow, 4 resonance, 5 env mod, 6 decay, 7 accent, 8 volume, 9 HP,
// 10 minimum decay, 11 pulse width, 12 LFO rate, 13 LFO → PW, 14..26 reverb,
// 27..31 distortion (27 on, 29 drive, 30 tone), 32..39 delay, 40 clicks,
// 41 303 pulse); then the 16-step sequences. The first patch is imported
// (the program the editor shows is not resolved); the sequences are not.
constexpr int kTbFloats = 100;

bool tbPatchAt(const Blob& b, size_t p, size_t& floatsAt) {
  uint32_t n = b.u32(p);
  if (n == 0 || n > 64 || !b.has(p + 4 + n, kTbFloats * 4)) return false;
  size_t at = p + 4 + n;
  int big = 0;
  for (int k = 0; k < kTbFloats; ++k) {
    float f = b.f32(at + 4 * static_cast<size_t>(k));
    if (!std::isfinite(f) || f < 0.0f || f > 1.0f) return false;
    if (f >= 0.01f) ++big;
  }
  if (big < 8) return false;
  floatsAt = at;
  return true;
}

bool decodeTransistorBass(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 0x40 + kTbFloats * 4) return false;
  size_t at = 0;
  bool found = false;
  for (size_t p = 0x30; p + 4 + kTbFloats * 4 < b.size() && p < 0x4000 && !found; ++p) found = tbPatchAt(b, p, at);
  if (!found) return false;
  gen2::begin(m, "gl.acid");
  std::vector<double> f(42);
  for (size_t k = 0; k < f.size(); ++k) f[k] = b.f32(at + 4 * k);
  auto unit = [](double x) { return clampd(x, 0, 1); };
  applyRows(m,
            {{1, 0, 1, AcBlend, unit},
             {2, 0, 1, AcCutoff, [](double x) { return logMap(x, 60, 8000); }},
             {3, 0, 1, AcKeyFollow, unit},
             {4, 0, 1, AcReso, unit},
             {5, 0, 1, AcEnvMod, unit},
             {6, 0, 1, AcDecay, [](double x) { return logMap(x, 0.03, 2); }},
             {7, 0, 1, AcAccent, unit},
             {8, 0, 1, AcVolume, unit},
             {9, 0, 1, AcHighPass, [](double x) { return logMap(x, 10, 500); }},
             {11, 0, 1, AcPulseWidth, [](double x) { return 0.05 + 0.9 * clampd(x, 0, 1); }},
             {12, 0, 1, AcPwmRate, [](double x) { return logMap(x, 0.05, 10); }},
             {13, 0, 1, AcPwmDepth, [](double x) { return 0.45 * clampd(x, 0, 1); }},
             {30, 0, 1, AcDriveTone, [](double x) { return logMap(x, 500, 20000); }},
             {40, 0, 1, AcClicks, unit},
             {41, 0, 1, AcVintage, [](double x) { return x >= 0.5 ? 1.0 : 0.0; }}},
            f);
  gen2::set(m, AcWave, f[1] >= 0.5 ? 1 : 0);
  gen2::set(m, AcDrive, f[27] >= 0.5 ? clampd(f[29], 0, 1) : 0.0);
  gen2::set(m, AcSeqOn, 0);
  m.exact = false;  // value curves estimated; reverb / delay, sequences and other programs not imported
  return true;
}

// ---------------------------------------------------------- Fruity Slicer 2
// u32 version, then chunks; the shipped preset is the empty default (no
// sample loaded), which imports as an empty slicer.
bool decodeSlicer2(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 8) return false;
  gen2::begin(m, "gl.slicer");
  gen2::set(m, SlBaseNote, 60);
  // A loaded sample shows up as a file path; those states are not decoded yet.
  bool hasPath = b.find(".wav") != std::string::npos || b.find(std::string("w\0a\0v", 5)) != std::string::npos ||
                 b.find(std::string(":\\")) != std::string::npos || b.find("%FL") != std::string::npos;
  m.exact = !hasPath;
  return true;
}

}  // namespace

void registerInst(std::map<std::string, PluginDecoder>& d) {
  add(d, "Plucked!", decodePlucked);
  add(d, "Transistor Bass", decodeTransistorBass);
  add(d, "Fruity Slicer 2", decodeSlicer2);
}

}  // namespace gl::flp::dec3
