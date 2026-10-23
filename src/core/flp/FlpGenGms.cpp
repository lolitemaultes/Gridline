// Gridline — FLP import: the groove-machine synth → Groove Synth ("gl.gms").
//
// State (event 213: "SSLF", dword size, payload; a .gmsynth preset file is the
// payload alone). Payload, worked out from the 306 shipped presets and the 15
// states in FL's projects (every one parses):
//   0x000  magic "3nyS" (older) or "4nyS" / "ED4S"
//   0x004  three 512-byte wave file paths (NUL-terminated, Windows or macOS)
//   0x604  reserved (zeros)
//   0xE04  older: 212 floats (0..1, one per parameter)
//   0xE04  newer: three UI dwords, then the 212 floats at 0xE10
//   then the program name.
// Parameter order: 0 level, 1 pan, 2 pitch, 4..7 amp A/D/S/R, 8 amp level,
// 10/11 slide time / on, 12..15 filter cutoff / resonance / key / type,
// 16..25 two AD envelopes (amount, invert, attack, decay, target),
// 26..39 two LFOs (retrigger, amount, invert, shape, rate, sync, target),
// 40 mono, 41..43 unison voices / detune / stereo, 44 octave, 45 noise,
// 46/47 unison phase / reset, 48/49 osc 2 / 3 mix, 50/51 mod amount / type,
// 52..54 osc 2 phase / reset / invert, 55..60 osc pitches / detunes,
// 117..186 ten effect slots (X, Y, LFO rate, on, LFO shape, ×4, LFO amount),
// 198..205 velocity / wheel / aftertouch routing, 200 velocity → volume,
// 205 bend range, 206..211 master EQ. The DSP is Gridline's own (exact = false).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../PluginBrand.h"
#include "../../packs/synths/gms.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace gm = gl::packs::synths::gms;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Integer step of a knob (truncates like the plugin; float noise next to an
// integer snaps to it).
double itrunc(double v) {
  const double r = std::round(v);
  return std::fabs(v - r) < 1e-3 ? r : std::trunc(v);
}

// Wave file stem (both separators).
std::string stemOf(const std::string& path) {
  size_t slash = path.find_last_of("\\/");
  std::string f = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = f.find_last_of('.');
  return dot == std::string::npos ? f : f.substr(0, dot);
}

int nearestBuiltin(const std::string& stem) {
  const std::string l = lower(stem);
  if (l.find("sine") != std::string::npos || l.find("sub") != std::string::npos) return gm::WvSine;
  if (l.find("tri") != std::string::npos) return gm::WvTriangle;
  if (l.find("square") != std::string::npos) return gm::WvSquare;
  if (l.find("key") != std::string::npos || l.find("rhodes") != std::string::npos) return gm::WvOrgan;
  if (l.find("vocal") != std::string::npos || l.find("throat") != std::string::npos) return gm::WvFormant;
  if (l.find("spike") != std::string::npos || l.find("horn") != std::string::npos) return gm::WvSpike;
  return gm::WvSaw;
}

}  // namespace

bool decodeGmsFull(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes raw = stateOf(events);
  if (!raw.has(0, 8)) return false;
  Bytes b = raw;
  if (raw.find("SSLF") == 0) b = raw.sub(8, raw.n - 8);
  if (!b.has(0, 4)) return false;
  const std::string magic(reinterpret_cast<const char*>(b.p), 4);
  if (magic != "3nyS" && magic != "4nyS" && magic != "ED4S") return false;
  const size_t floatsAt = magic == "3nyS" ? 0xE04 : 0xE10;
  float p[212];
  // Plugin defaults for a short state.
  for (float& v : p) v = 0.0f;
  p[0] = 0.75f, p[1] = 0.5f, p[2] = 0.5f, p[6] = 1.0f, p[8] = 0.5f, p[12] = 1.0f, p[44] = 0.5f;
  p[55] = p[57] = p[59] = 0.5f, p[56] = p[58] = p[60] = 0.5f, p[200] = 1.0f;
  int count = 0;
  for (int i = 0; i < 212 && b.has(floatsAt + static_cast<size_t>(i) * 4, 4); ++i, ++count) {
    const float v = b.f32(floatsAt + static_cast<size_t>(i) * 4);
    p[i] = std::isfinite(v) ? v : 0.0f;
  }
  if (count < 61) return false;
  auto P = [&](int i) { return static_cast<double>(std::clamp(p[i], -4.0f, 4.0f)); };
  auto on = [&](int i) { return p[i] >= 0.5f; };
  auto sq = [&](int i) { return clampd(P(i), 0, 1) * clampd(P(i), 0, 1); };
  auto idx = [&](int i, int mult, int hi) { return std::clamp(static_cast<int>(mult * clampd(P(i), 0, 1)), 0, hi); };

  PluginState s = base("gl.gms");
  // Waves → the FL install's own single-cycle files (converted locally, never bundled).
  gm::WaveFiles wf;
  int missing = 0;
  std::string waveNames;
  for (int k = 0; k < 3; ++k) {
    Bytes w = b.sub(0x04 + static_cast<size_t>(k) * 512, 512);
    std::string path;
    for (size_t i = 0; i < w.n && w.p[i]; ++i) path += static_cast<char>(w.p[i]);
    std::string stem = stemOf(path);
    if (lower(stem) == "devils horn") stem = "Double Spike";
    if (!waveNames.empty()) waveNames += " / ";
    waveNames += stem.empty() ? "-" : stem;
    std::string file;
    if (!stem.empty() && !c.factoryData.empty()) {
      const std::filesystem::path f = std::filesystem::path(c.factoryData) / "Plugins/Fruity/Generators/GMS/Synth Waves" / (stem + ".wav");
      std::error_code ec;
      if (std::filesystem::exists(f, ec)) file = f.string();
    }
    if (!file.empty()) {
      setp(s, gm::osc(k, gm::OsWave), gm::WvFile);
      wf.path[k] = file;
    } else {
      setp(s, gm::osc(k, gm::OsWave), nearestBuiltin(stem));
      if (!stem.empty()) ++missing;
    }
    setp(s, gm::osc(k, gm::OsSemi), itrunc(72 * P(55 + 2 * k) - 36));
    setp(s, gm::osc(k, gm::OsCents), itrunc(100 * P(56 + 2 * k) - 50));
  }
  s.data = gm::serializeWaveFiles(wf);
  // Master / amp.
  setp(s, gm::GVolume, clampd(80 * P(0) - 60, -60, 20));
  setp(s, gm::GPan, clampd(2 * P(1) - 1, -1, 1));
  setp(s, gm::GPitch, clampd(itrunc(48 * P(2) - 24), -24, 24));
  setp(s, gm::GAmpA, 50 * std::pow(clampd(P(4), 0, 1), 3) + 0.001);
  setp(s, gm::GAmpD, 50 * std::pow(clampd(P(5), 0, 1), 4) + 0.001);
  setp(s, gm::GAmpS, clampd(P(6), 0, 1));
  setp(s, gm::GAmpR, 25 * std::pow(0.55 * clampd(P(7), 0, 1) + 0.1, 3) + 0.001);
  setp(s, gm::GLevel, clampd(50 * P(8) - 25, -25, 25));
  setp(s, gm::GSlideTime, 2 * std::pow(clampd(P(10), 0, 1), 3) + 0.001);
  setp(s, gm::GSlide, on(11) ? 1 : 0);
  // Filter.
  setp(s, gm::GCutoff, clampd(14.0 * std::pow(2.0, 9.7735 * clampd(P(12), 0, 1)), 14, 20000));
  setp(s, gm::GReso, clampd(P(13), 0, 1));
  setp(s, gm::GKey, clampd(P(14), 0, 1));
  setp(s, gm::GFltType, idx(15, 2, 2));
  // Envelopes, LFOs.
  for (int k = 0; k < 2; ++k) {
    setp(s, gm::eg(k, gm::EgAmt), sq(16 + k) * (on(18 + k) ? -1 : 1));
    setp(s, gm::eg(k, gm::EgAtt), 300 * std::pow(clampd(P(20 + k), 0, 1), 5) + 0.001);
    setp(s, gm::eg(k, gm::EgDec), 70 * std::pow(0.8 * clampd(P(22 + k), 0, 1) + 0.2, 5) + 0.001);
    setp(s, gm::eg(k, gm::EgDest), idx(24 + k, 3, 3));
    setp(s, gm::lfo(k, gm::LfRetrig), on(26 + k) ? 1 : 0);
    setp(s, gm::lfo(k, gm::LfAmt), sq(28 + k) * (on(30 + k) ? -1 : 1));
    setp(s, gm::lfo(k, gm::LfShape), idx(32 + k, 3, 3));
    setp(s, gm::lfo(k, gm::LfRate), clampd(0.01 + 25 * (1 - clampd(P(34 + k), 0, 1)) * (1 - clampd(P(34 + k), 0, 1)), 0.01, 25));
    setp(s, gm::lfo(k, gm::LfDiv), idx(34 + k, 9, 8));
    setp(s, gm::lfo(k, gm::LfSync), idx(36 + k, 2, 2));
    setp(s, gm::lfo(k, gm::LfDest), idx(38 + k, 3, 3));
  }
  // Voice, oscillators.
  setp(s, gm::GMono, on(40) ? 1 : 0);
  setp(s, gm::GUniVoices, std::clamp(static_cast<int>(15 * clampd(P(41), 0, 1)) + 1, 1, 16));
  setp(s, gm::GUniDetune, clampd(P(42), 0, 1));
  setp(s, gm::GUniStereo, clampd(P(43), 0, 1));
  setp(s, gm::GOctave, std::clamp(static_cast<int>(4 * clampd(P(44), 0, 1)), 0, 4) - 2);
  setp(s, gm::GNoise, sq(45));
  setp(s, gm::GUniPhase, clampd(P(46), 0, 1));
  setp(s, gm::GUniRetrig, on(47) ? 1 : 0);
  setp(s, gm::GOsc2Mix, sq(48));
  setp(s, gm::GOsc3Mix, sq(49));
  setp(s, gm::GModAmt, sq(50));
  setp(s, gm::GModKind, idx(51, 2, 2));
  setp(s, gm::GOsc2Phase, clampd(P(52), 0, 1) / 2);
  setp(s, gm::GOsc2Retrig, on(53) ? 1 : 0);
  setp(s, gm::GOsc2Invert, on(54) ? 1 : 0);
  // Effects.
  int fxOn = 0;
  if (count >= 187)
    for (int k = 0; k < gm::kFxSlots; ++k) {
      setp(s, gm::fx(k, gm::FfX), clampd(P(117 + k), 0, 1));
      setp(s, gm::fx(k, gm::FfY), clampd(P(127 + k), 0, 1));
      setp(s, gm::fx(k, gm::FfDiv), idx(137 + k, 9, 8));
      setp(s, gm::fx(k, gm::FfOn), on(147 + k) ? 1 : 0);
      setp(s, gm::fx(k, gm::FfShape), idx(157 + k, 3, 3));
      setp(s, gm::fx(k, gm::FfX4), on(167 + k) ? 1 : 0);
      const double a = 2 * clampd(P(177 + k), 0, 1) - 1;
      setp(s, gm::fx(k, gm::FfLfo), a * std::fabs(a));
      fxOn += on(147 + k);
    }
  // Velocity routing, EQ.
  if (count >= 212) {
    setp(s, gm::GVelAmt, clampd(2 * P(198) - 1, -1, 1));
    static const int kVel[22] = {gm::VdNone,    gm::VdCutoff,  gm::VdReso,   gm::VdNone,     gm::VdMod,      gm::VdNone,
                                 gm::VdEg1A,    gm::VdEg1D,    gm::VdEg1Amt, gm::VdNone,     gm::VdEg2A,     gm::VdEg2D,
                                 gm::VdEg2Amt,  gm::VdNone,    gm::VdLfo1Rate, gm::VdLfo1Amt, gm::VdNone,    gm::VdLfo2Rate,
                                 gm::VdLfo2Amt, gm::VdNone,    gm::VdAmpA,   gm::VdAmpD};
    const int vd = static_cast<int>(std::lround(P(199)));
    setp(s, gm::GVelDest, vd >= 0 && vd < 22 ? kVel[vd] : gm::VdNone);
    setp(s, gm::GVelVol, on(200) ? 1 : 0);
    setp(s, gm::GBend, std::clamp(static_cast<int>(12 * clampd(P(205), 0, 1)), 0, 12));
    setp(s, gm::GEqOn, on(206) ? 1 : 0);
    for (int i = 0; i < gm::kEqBands; ++i) setp(s, gm::GEq1 + i, clampd(24 * P(207 + i) - 12, -12, 12));
  }
  m.glType = "gl.gms";
  m.state = s;
  m.exact = false;
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s → %s: waves %s%s, %d effect slot(s), %d parameters", magic.c_str(), pluginDisplayName("gl.gms").c_str(), waveNames.c_str(),
                missing ? " (some wave files not found: nearest built-in)" : "", fxOn, count);
  note(c, "GMS", buf);
  // Automation (FL parameter = index, normalized 0..1).
  ParamTable t;
  t.fn(0, gm::GVolume, [](double n) { return clampd(80 * n - 60, -60, 20); });
  t.fn(12, gm::GCutoff, [](double n) { return clampd(14.0 * std::pow(2.0, 9.7735 * n), 14, 20000); });
  t.lin(13, gm::GReso, 0, 1);
  t.lin(14, gm::GKey, 0, 1);
  t.lin(6, gm::GAmpS, 0, 1);
  t.fn(4, gm::GAmpA, [](double n) { return 50 * n * n * n + 0.001; });
  t.fn(7, gm::GAmpR, [](double n) { return 25 * std::pow(0.55 * n + 0.1, 3) + 0.001; });
  t.fn(16, gm::eg(0, gm::EgAmt), [](double n) { return n * n; });
  t.fn(50, gm::GModAmt, [](double n) { return n * n; });
  t.fn(48, gm::GOsc2Mix, [](double n) { return n * n; });
  t.fn(49, gm::GOsc3Mix, [](double n) { return n * n; });
  t.install(m);
  return true;
}

}  // namespace gl::flp::gen
