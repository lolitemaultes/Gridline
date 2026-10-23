// Gridline — FLP import: the subtractive-additive synth → Additive ("gl.additive").
//
// State (worked out from the 430 shipped presets, the render test and the
// demo projects; every one walks to its exact end):
//   dword version (4, 5, 9 … 12)
//   100 dwords: control tags 0..99 in order (raw control values, ranges as on
//     the controls: e.g. 54 filter frequency 0..8192, 59 resonance 0..1000)
//   option dwords — 6 (versions 4 / 5) or 8: filter shape, resonance shape,
//     phaser shape, compression type, timbre colour, quality, threading,
//     custom timbre size N (combo indices skip the menus' separators)
//   N floats custom harmonic levels, N floats phases (when N > 0)
//   one flag byte (version 12)
// Mapping: every control has a counterpart (the Additive engine carries the
// same blocks); values are converted to Gridline units. The DSP is Gridline's
// own, so the mapping is approximate (exact = false).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../PluginBrand.h"
#include "../../packs/models/models.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace md = gl::packs::models;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

// Control curves (controls store raw integers; x = v / 1000).
struct HlConv {
  static double l11(double y) { return 0.1 * (std::pow(11.0, clampd(y, 0, 1)) - 1.0); }  // generic level curve
  static double l3(double y) { return 0.5 * (std::pow(3.0, clampd(y, 0, 2)) - 1.0); }    // 1 at y = 1
  static double attack(double v) { return 0.05 * (std::pow(101.0, clampd(v / 1000, 0, 1)) - 1.0); }
  static double decay(double v) { return 0.11995 * (std::pow(1001.0, clampd(v / 1000, 0, 1)) - 1.0) + 0.05; }
  static double release(double v) { return 0.0249 * (std::pow(101.0, clampd(v / 1000, 0, 1)) - 1.0) + 0.01; }
  static double fenvTime(double v) { return 0.12 * (std::pow(1001.0, clampd(v / 1000, 0, 1)) - 1.0); }
  static double pluck(double v) { return 0.11975 * (std::pow(1001.0, clampd(v / 1000, 0, 1)) - 1.0); }
  // Cutoff 0..8192: linear in octaves from 73.42 Hz to 17 kHz.
  static constexpr double kLo = 73.42, kOct = 7.855;
  static double cutoff(double v) { return clampd(kLo * std::pow(2.0, kOct * clampd(v / 8192.0, 0, 1)), 30, 20000); }
  // Width 0..1000: the shape table spans 2^(v/250 - 1) octaves per 1024 entries.
  static double width(double v) { return clampd(std::pow(2.0, clampd(v, 0, 1000) / 250.0 - 1.0), 0.1, 8); }
  static double volume(double v) { return clampd(0.75 * l3(v / 1000.0), 0, 2); }
  // Speeds (LFO, phaser motion): tempo-locked, 2^(|v|/1000) / 64 cycles per beat.
  static double cyclesPerBar(double v) { return clampd(4.0 * std::pow(2.0, std::fabs(v) / 1000.0) / 64.0, 0.05, 30); }
  static double porta(double v) { return clampd((std::pow(41.0, std::fabs(v) / 1000.0) - 1.0) / 26.667, 0.001, 1.5); }
  static double uniCents(double v) { return clampd((std::pow(41.0, clampd(v, 0, 256) / 256.0) - 1.0) * 1.875, 0, 100); }
  static double phDensity(double v) { return clampd(32.0 * std::pow(2.0, 8.0 * (clampd(v, 0, 10000) * 1e-4 - 1.0)), 0.05, 32); }
};

}  // namespace

bool decodeHarmlessFull(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 8)) return false;
  const int version = b.i32(0);
  if (version < 1 || version > 64) return false;
  // Control defaults for anything a short state lacks.
  int p[100] = {};
  const int defs[][2] = {{1, 256},   {23, 500},  {27, 500},  {28, 250},  {31, 500},  {36, 1000}, {49, 500}, {54, 4096},
                         {56, 500},  {57, 1000}, {65, 1},    {66, 128},  {67, 128},  {68, 64},   {69, 64},  {73, 5000},
                         {75, 500},  {77, 3000}, {78, 1000}, {80, 500},  {89, 3000}, {91, 500},  {93, 500}, {94, 300},
                         {95, 144},  {97, 500},  {98, 500}};
  for (const auto& d : defs) p[d[0]] = d[1];
  const int count = std::min(100, static_cast<int>((b.n - 4) / 4));
  for (int i = 0; i < count; ++i) p[i] = b.i32(4 + static_cast<size_t>(i) * 4);
  // Options.
  int opt[8] = {0, 0, 0, 0, 0, 1, 0, 0};
  const size_t optAt = 4 + 100 * 4;
  const int nOpt = b.has(optAt, 32) ? 8 : b.has(optAt, 24) ? 6 : 0;
  for (int i = 0; i < nOpt; ++i) opt[i] = b.i32(optAt + static_cast<size_t>(i) * 4);
  md::AdditiveData custom;
  if (nOpt == 8 && opt[7] > 0 && opt[7] <= 4096) {
    const size_t at = optAt + 32, n = static_cast<size_t>(opt[7]);
    if (b.has(at, n * 4)) {
      for (size_t k = 0; k < n && k < static_cast<size_t>(md::kAdCustomMax); ++k) custom.level.push_back(b.f32(at + k * 4));
      if (b.has(at + n * 4, n * 4))
        for (size_t k = 0; k < n && k < static_cast<size_t>(md::kAdCustomMax); ++k) custom.phase.push_back(b.f32(at + n * 4 + k * 4));
    }
  }
  // Custom levels relative to the loudest (kept as the analysed shape).
  if (!custom.level.empty()) {
    float peak = 1e-9f;
    for (float v : custom.level) peak = std::max(peak, std::fabs(v));
    for (float& v : custom.level) v = std::fabs(v) / peak;
  }
  auto f = [&](int t, double div) { return p[t] / div; };

  PluginState s = base("gl.additive");
  // ---- timbre
  const int colour = std::clamp(opt[4], 0, 11);
  static const int kColour[12] = {md::SpSaw,     md::SpBright, md::SpBrighter, md::SpDephased, md::SpDephased2, md::SpSqSaw,
                                  md::SpSqSaw2,  md::SpNotched, md::SpDouble,  md::SpTriple,   md::SpTriple2,   md::SpCustom};
  setp(s, md::AdSpectrumA, kColour[colour]);
  setp(s, md::AdSpectrumB, kColour[colour]);
  setp(s, md::AdMorph, 0.0);
  setp(s, md::AdPartials, 256);
  if (!custom.level.empty() && !c.paramsOnly) s.data = md::serializeAdditiveData(custom);  // kept even when another colour is selected
  for (int k = 0; k < md::kAdDrawn; ++k) setp(s, md::AdDrawnBase + k, 1.0);
  for (int i = 0; i < md::kAdTeq; ++i) setp(s, md::AdTeqBase + i, clampd(f(5 + i, 1024), -1, 1));
  setp(s, md::AdSub, clampd(f(2, 1024), 0, 2));
  setp(s, md::AdSub3, clampd(f(3, 1024), 0, 2));
  setp(s, md::AdSub4, clampd(f(4, 1024), 0, 2));
  setp(s, md::AdProtect, HlConv::l11(f(1, 1024)));
  setp(s, md::AdPhaseRand, clampd(f(0, 1000) * f(0, 1000), 0, 1));
  for (int k = 0; k < md::kAdMask; ++k) setp(s, md::AdMaskBase + k, HlConv::l11(std::fabs(f(11 + k, 1000))));
  setp(s, md::AdMaskMix, clampd(f(23, 1000), 0, 1));
  setp(s, md::AdMaskLfo, clampd(f(24, 1000), -1, 1));
  setp(s, md::AdTremolo, clampd(f(25, 1000), 0, 1));
  // ---- volume envelope (decay at its maximum holds the level), velocity
  setp(s, md::AdAttack, std::max(0.001, HlConv::attack(p[26])));
  if (p[27] >= 1000) {
    setp(s, md::AdDecay, 1.0), setp(s, md::AdSustain, 1.0);
  } else {
    setp(s, md::AdDecay, clampd(HlConv::decay(p[27]), 0.01, 20)), setp(s, md::AdSustain, 0.0);
  }
  setp(s, md::AdRelease, clampd(HlConv::release(p[28]), 0.005, 10));
  setp(s, md::AdVelAtt, p[29] ? 1 : 0);
  setp(s, md::AdVelRel, p[30] ? 1 : 0);
  setp(s, md::AdVelVol, p[37] ? 1 : 0);
  // ---- pluck (decay length below the maximum = plucked)
  if (p[31] < 1000) {
    setp(s, md::AdPluck, 1.0);
    setp(s, md::AdPluckTime, clampd(HlConv::pluck(p[31]) * 4.0, 0.05, 10));
  } else {
    setp(s, md::AdPluck, 0.0);
  }
  setp(s, md::AdVelPluck, p[32] ? 1 : 0);
  setp(s, md::AdPluckAlt, p[33] ? 1 : 0);
  setp(s, md::AdPluckMask, p[34] ? 1 : 0);
  setp(s, md::AdPluckRel, p[35] ? 1 : 0);
  // ---- master, pitch (detuning moves the partials with the pitch)
  setp(s, md::AdVolume, HlConv::volume(p[36]));
  setp(s, md::AdPitch, clampd((static_cast<double>(p[38]) + p[39]) / 100.0, -24, 24));
  setp(s, md::AdFine, 0.0);
  setp(s, md::AdDetune, 0.0);
  setp(s, md::AdGrit, clampd(f(40, 1000), -1, 1));
  setp(s, md::AdVibrato, clampd(p[41] * 0.075 / 100.0, 0, 1));
  setp(s, md::AdLfoPitch, clampd((std::pow(101.0, std::fabs(f(42, 1000))) - 1.0) * 12.0 / 100.0, 0, 12));
  // ---- voices
  const bool porta = p[43] != 0, mono = p[44] != 0;
  setp(s, md::AdPlayMode, mono ? (porta ? md::ApMonoPorta : md::ApMono) : (porta ? md::ApPolyPorta : md::ApPoly));
  setp(s, md::AdGlide, porta ? HlConv::porta(p[45]) : 0.0);
  setp(s, md::AdVelPorta, p[46] ? 1 : 0);
  setp(s, md::AdVoices, 16);
  // ---- filter
  static const int kShape[17] = {md::AfLowPass, md::AfBandPass, md::AfBandStop, md::AfHighPass, md::AfSoftLp, md::AfSoftBp,
                                 md::AfSoftBs,  md::AfSoftHp,   md::AfHill,     md::AfSlope,    md::AfHybrid, md::AfSnake,
                                 md::AfFin,     md::AfLeaky,    md::AfComb,     md::AfBandComb, md::AfFlatBp};
  setp(s, md::AdFltType, kShape[std::clamp(opt[0], 0, 16)]);
  setp(s, md::AdResType, std::clamp(opt[1], 0, md::ArCount - 1));
  setp(s, md::AdFEnvAdaptive, p[47] ? 1 : 0);
  // Filter envelope: attack to 1, decay to 0; it is centred (0.5 = the knob's
  // cutoff), so the cutoff is moved down by half the sweep and swept by the
  // whole amount (7.855 octaves at ±1). A maximal decay holds the peak.
  const double envAmt = clampd(f(52, 1000), -1, 1) * HlConv::kOct;
  setp(s, md::AdFEnvAtt, clampd(HlConv::fenvTime(p[48]), 0.001, 10));
  if (p[49] >= 1000) {
    setp(s, md::AdFEnvDec, 10.0), setp(s, md::AdFEnvSus, 1.0);
  } else {
    setp(s, md::AdFEnvDec, clampd(HlConv::fenvTime(p[49]), 0.01, 10)), setp(s, md::AdFEnvSus, 0.0);
  }
  setp(s, md::AdFEnvRel, 0.1);
  setp(s, md::AdFEnvAttSlope, clampd(f(50, 1000), -1, 1));
  setp(s, md::AdFEnvDecSlope, clampd(f(51, 1000), -1, 1));
  setp(s, md::AdFEnvAmt, clampd(envAmt, -8, 8));
  setp(s, md::AdVelFEnv, p[53] ? 1 : 0);
  setp(s, md::AdFltCutoff, clampd(HlConv::cutoff(p[54]) * std::pow(2.0, p[47] ? 0.0 : -0.5 * envAmt), 30, 20000));
  setp(s, md::AdFltScale, p[55] ? 1 : 0);
  setp(s, md::AdFltWidth, HlConv::width(p[56]));
  setp(s, md::AdFltKey, clampd(f(57, 1000), -2, 2));
  setp(s, md::AdLfoCutoff, clampd(f(58, 1000) * 3.0, -3, 3));
  setp(s, md::AdFltReso, clampd(f(59, 1000), 0, 1));
  setp(s, md::AdResScale, p[60] ? 1 : 0);
  setp(s, md::AdResOffset, clampd(p[61] / 100.0, -24, 24));
  setp(s, md::AdResOsc, clampd(f(62, 2048), 0, 1));
  setp(s, md::AdResAdaptive, p[63] ? 1 : 0);
  setp(s, md::AdResTrack, p[64] ? 1 : 0);
  // ---- unison
  setp(s, md::AdUniOrder, std::clamp(p[65], 1, 9));
  setp(s, md::AdUniSpread, clampd(f(66, 128), 0, 1));
  setp(s, md::AdUniDetune, HlConv::uniCents(p[67]));
  setp(s, md::AdUniPhase, clampd(f(68, 128), 0, 1));
  setp(s, md::AdUniVar, clampd(f(69, 128), 0, 1));
  setp(s, md::AdUniType, std::clamp(p[70], 0, 2));
  setp(s, md::AdVelUni, p[99] ? 1 : 0);
  // ---- phaser
  setp(s, md::AdPhType, std::clamp(opt[2], 0, md::ApCount - 1));
  setp(s, md::AdPhMix, clampd(f(71, 1000), 0, 1));
  setp(s, md::AdPhScale, p[72] == 1 ? 1 : 0);
  setp(s, md::AdPhDensity, HlConv::phDensity(p[73]));
  setp(s, md::AdPhWidthLfo, clampd(p[74] * 0.004, -4, 4));
  setp(s, md::AdPhOffset, clampd(f(75, 1000), 0, 1));
  setp(s, md::AdLfoPhaser, clampd(f(76, 1000), -1, 1));
  const double tempo = c.song.tempo > 0 ? c.song.tempo : 120.0;
  setp(s, md::AdPhSpeed, clampd((p[77] < 0 ? -1 : 1) * (p[77] == 0 ? 0.0 : HlConv::cyclesPerBar(p[77]) * tempo / 240.0), -16, 16));
  setp(s, md::AdPhKey, clampd(f(78, 1000), -2, 2));
  // ---- harmonizer
  setp(s, md::AdHzMix, clampd(f(79, 1000), 0, 1));
  setp(s, md::AdHzWidth, clampd(f(80, 1000), 0, 1));
  setp(s, md::AdHzWidthLfo, clampd(f(81, 1000), -1, 1));
  setp(s, md::AdVelHarm, p[82] ? 1 : 0);
  setp(s, md::AdHzStrength, clampd(f(83, 1024), 0, 1));
  setp(s, md::AdHzType, std::clamp(p[84], 0, md::AhCount - 1));
  setp(s, md::AdHzPos, std::clamp(p[85], 0, 3));
  // ---- LFO: 0..8 shapes, 9..13 special sources
  static const int kLfo[14] = {md::AlSine,    md::AlSmoothSaw, md::AlSmoothPulse, md::AlSaw,       md::AlSquare,
                               md::AlStairs,  md::AlTriangle,  md::AlSickle,      md::AlMultiSine, md::AlFiltEnv,
                               md::AlFiltFreq, md::AlVoiceRand, md::AlVelocity,   md::AlPitch};
  setp(s, md::AdLfoShape, kLfo[std::clamp(p[86], 0, 13)]);
  setp(s, md::AdLfoGlobal, p[87] ? 0 : 1);
  setp(s, md::AdLfoAttack, clampd(HlConv::attack(p[88]), 0.001, 10));
  setp(s, md::AdLfoTempo, 1);  // always tempo-locked (rate = cycles per bar)
  setp(s, md::AdLfoRate, p[89] == 0 ? 0.05 : HlConv::cyclesPerBar(p[89]));
  // ---- effects (combo indices: 0 = off)
  setp(s, md::AdChType, std::clamp(p[90], 1, 9) - 1);
  setp(s, md::AdFx + md::FxChorus, p[90] >= 1 ? clampd(f(91, 1000), 0, 1) : 0.0);
  setp(s, md::AdDelType, std::clamp(p[92], 1, 3) - 1);
  setp(s, md::AdFx + md::FxDelay, p[92] >= 1 ? clampd(HlConv::l11(f(93, 1000)), 0, 1) : 0.0);
  setp(s, md::AdFx + md::FxDelayFeedback, clampd(f(94, 1000), 0, 0.9));
  setp(s, md::AdFx + md::FxDelayTime, clampd(p[95] * 0.3125 / tempo, 0.02, 1.5));  // 1/192-beat ticks
  setp(s, md::AdRevType, std::clamp(p[96], 1, 6) - 1);
  setp(s, md::AdFx + md::FxReverb, p[96] >= 1 ? clampd(0.5 * HlConv::l11(f(97, 1000)) * 1.6, 0, 1) : 0.0);
  setp(s, md::AdFx + md::FxRoomSize, 0.6);
  setp(s, md::AdCompType, std::clamp(opt[3], 0, 4));
  setp(s, md::AdComp, clampd(f(98, 1000), 0, 1));

  m.glType = "gl.additive";
  m.state = s;
  m.exact = false;
  char buf[240];
  std::snprintf(buf, sizeof buf,
                "v%d → %s: colour %d%s, timbre EQ / sub / mask, filter shape %d (%.0f Hz), resonance shape %d, unison %d, phaser, "
                "harmonizer, pluck, LFO, FX",
                version, pluginDisplayName("gl.additive").c_str(), colour, custom.level.empty() ? "" : " (custom table)", opt[0], HlConv::cutoff(p[54]), opt[1],
                std::clamp(p[65], 1, 9));
  note(c, "Harmless", buf);
  // Automation targets (FL parameter = control tag; normalized 0..1 over the control range).
  ParamTable t;
  auto lin = [&](int tag, int gl, double lo, double hi, double rawLo, double rawHi, double div) {
    t.fn(tag, gl, [=](double n) { return clampd((rawLo + (rawHi - rawLo) * n) / div, lo, hi); });
    t.range(tag, rawLo, rawHi);
  };
  t.fn(36, md::AdVolume, [](double n) { return HlConv::volume(n * 2000); });
  t.range(36, 0, 2000);
  lin(38, md::AdPitch, -24, 24, -2400, 2400, 100);
  t.fn(52, md::AdFEnvAmt, [](double n) { return clampd((2 * n - 1) * HlConv::kOct, -8, 8); });
  t.range(52, -1000, 1000);
  t.fn(54, md::AdFltCutoff, [](double n) { return HlConv::cutoff(n * 8192); });
  t.range(54, 0, 8192);
  t.fn(56, md::AdFltWidth, [](double n) { return HlConv::width(n * 1000); });
  t.range(56, 0, 1000);
  lin(57, md::AdFltKey, -2, 2, -2000, 2000, 1000);
  lin(59, md::AdFltReso, 0, 1, 0, 1000, 1000);
  lin(61, md::AdResOffset, -24, 24, -2400, 2400, 100);
  lin(62, md::AdResOsc, 0, 1, 0, 2048, 2048);
  lin(23, md::AdMaskMix, 0, 1, 0, 1000, 1000);
  lin(24, md::AdMaskLfo, -1, 1, -1000, 1000, 1000);
  lin(25, md::AdTremolo, 0, 1, 0, 1000, 1000);
  lin(71, md::AdPhMix, 0, 1, 0, 1000, 1000);
  lin(75, md::AdPhOffset, 0, 1, 0, 999, 1000);
  lin(79, md::AdHzMix, 0, 1, 0, 1000, 1000);
  lin(80, md::AdHzWidth, 0, 1, 0, 1000, 1000);
  lin(83, md::AdHzStrength, 0, 1, 0, 1024, 1024);
  lin(40, md::AdGrit, -1, 1, -1000, 1000, 1000);
  lin(41, md::AdVibrato, 0, 1, 0, 1000, 1000 / 0.75);
  lin(58, md::AdLfoCutoff, -3, 3, -1000, 1000, 1000.0 / 3);
  lin(76, md::AdLfoPhaser, -1, 1, -1000, 1000, 1000);
  lin(91, md::AdFx + md::FxChorus, 0, 1, 0, 1000, 1000);
  t.fn(97, md::AdFx + md::FxReverb, [](double n) { return clampd(0.8 * HlConv::l11(n), 0, 1); });
  t.range(97, 0, 1000);
  for (int i = 0; i < 6; ++i) lin(5 + i, md::AdTeqBase + i, -1, 1, -1024, 1024, 1024);
  for (int i = 0; i < 12; ++i) lin(11 + i, md::AdMaskBase + i, 0, 1, 0, 1000, 1000);
  t.fn(26, md::AdAttack, [](double n) { return std::max(0.001, HlConv::attack(n * 1000)); });
  t.fn(28, md::AdRelease, [](double n) { return clampd(HlConv::release(n * 1000), 0.005, 10); });
  t.fn(48, md::AdFEnvAtt, [](double n) { return clampd(HlConv::fenvTime(n * 1000), 0.001, 10); });
  t.fn(49, md::AdFEnvDec, [](double n) { return clampd(HlConv::fenvTime(n * 1000), 0.01, 10); });
  t.fn(31, md::AdPluckTime, [](double n) { return clampd(HlConv::pluck(n * 1000) * 4.0, 0.05, 10); });
  t.fn(89, md::AdLfoRate, [](double n) { return HlConv::cyclesPerBar(-8000 + 16000 * n); });
  for (int tag : {26, 28, 48, 49, 31}) t.range(tag, 0, 1000);
  t.range(89, -8000, 8000);
  t.install(m);
  return true;
}

}  // namespace gl::flp::gen
