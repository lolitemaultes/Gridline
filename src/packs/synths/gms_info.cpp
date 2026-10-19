// Gridline — "synths" pack: Groove Synth catalogue entry, factory presets and
// the wave-file data text.
#include <sstream>
#include <string>
#include <vector>
#include "../Packs.h"
#include "gms.h"

namespace gl::packs::synths {

namespace {

using namespace gms;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

PluginInfo gmsInfo() {
  PluginInfo p{"gl.gms", "Groove Synth", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(GCount);
  q[GVolume] = lin("Volume", "dB", -60, 20, 0);
  q[GPan] = lin("Pan", "", -1, 1, 0);
  q[GPitch] = lin("Pitch", "st", -24, 24, 0);
  q[GOctave] = lin("Octave", "oct", -2, 2, 0);
  q[GLevel] = lin("Level", "dB", -25, 25, 0);
  q[GVelVol] = tog("Velocity > level", true);
  q[GBend] = lin("Bend range", "st", 0, 12, 2);
  q[GAmpA] = lg("Attack", "s", 0.001, 50, 0.002);
  q[GAmpD] = lg("Decay", "s", 0.001, 50, 1.5);
  q[GAmpS] = lin("Sustain", "%", 0, 1, 0.8);
  q[GAmpR] = lg("Release", "s", 0.001, 30, 0.4);
  q[GSlide] = tog("Slide", false);
  q[GSlideTime] = lg("Slide time", "s", 0.001, 2, 0.06);
  q[GMono] = tog("Mono", false);
  q[GCutoff] = lg("Cutoff", "Hz", 14, 20000, 9000);
  q[GReso] = lin("Resonance", "%", 0, 1, 0.1);
  q[GKey] = lin("Key track", "%", 0, 1, 0);
  q[GFltType] = ch("Filter type", {"Low pass", "Band pass", "High pass"}, GfLowPass);
  const std::vector<std::string> egDest = {"Cutoff", "Mod amount", "Pitch", "LFO depth"};
  const std::vector<std::string> lfoDest = {"Cutoff", "Mod amount", "Pitch", "Volume"};
  const std::vector<std::string> shapes = {"Sine", "Triangle", "Saw", "Square"};
  const std::vector<std::string> divs = {"1/32", "1/16", "3/32", "1/8", "3/16", "1/4", "1/2", "1 bar", "2 bars"};
  for (int k = 0; k < 2; ++k) {
    const std::string e = "Env " + std::to_string(k + 1) + " ", l = "LFO " + std::to_string(k + 1) + " ";
    q[eg(k, EgAmt)] = lin(e + "amount", "", -1, 1, 0);
    q[eg(k, EgAtt)] = lg(e + "attack", "s", 0.001, 300, 0.001);
    q[eg(k, EgDec)] = lg(e + "decay", "s", 0.001, 70, 0.6);
    q[eg(k, EgDest)] = ch(e + "target", egDest, k == 0 ? EdCutoff : EdMod);
    q[lfo(k, LfAmt)] = lin(l + "amount", "", -1, 1, 0);
    q[lfo(k, LfShape)] = ch(l + "shape", shapes, LsSine);
    q[lfo(k, LfRate)] = lg(l + "rate", "Hz", 0.01, 25, 4);
    q[lfo(k, LfSync)] = ch(l + "sync", {"Free", "Beats", "Beats x4"}, LyFree);
    q[lfo(k, LfDiv)] = ch(l + "division", divs, 3);
    q[lfo(k, LfRetrig)] = tog(l + "retrigger", false);
    q[lfo(k, LfDest)] = ch(l + "target", lfoDest, k == 0 ? LdCutoff : LdPitch);
  }
  q[GUniVoices] = lin("Unison", "", 1, 16, 1);
  q[GUniDetune] = lin("Unison detune", "%", 0, 1, 0.25);
  q[GUniStereo] = lin("Unison stereo", "%", 0, 1, 0.5);
  q[GUniPhase] = lin("Unison phase", "%", 0, 1, 0);
  q[GUniRetrig] = tog("Unison phase reset", false);
  q[GNoise] = lin("Noise", "%", 0, 1, 0);
  q[GOsc2Mix] = lin("Osc 2 mix", "%", 0, 1, 0);
  q[GOsc3Mix] = lin("Osc 3 mix", "%", 0, 1, 0);
  q[GModAmt] = lin("Mod amount", "%", 0, 1, 0);
  q[GModKind] = ch("Mod type", {"Sync", "FM", "AM"}, GkFm);
  q[GOsc2Phase] = lin("Osc 2 phase", "cyc", 0, 0.5, 0);
  q[GOsc2Retrig] = tog("Osc 2 phase reset", false);
  q[GOsc2Invert] = tog("Osc 2 invert", false);
  const std::vector<std::string> waves = {"Sine", "Triangle", "Saw", "Square", "Pulse 25", "Pulse 10", "Soft saw",
                                          "Hollow", "Organ", "Bell", "Sub saw", "Formant", "Spike", "File"};
  for (int k = 0; k < kOscs; ++k) {
    const std::string o = "Osc " + std::to_string(k + 1) + " ";
    q[osc(k, OsWave)] = ch(o + "wave", waves, WvSaw);
    q[osc(k, OsSemi)] = lin(o + "semitones", "st", -36, 36, 0);
    q[osc(k, OsCents)] = lin(o + "cents", "ct", -50, 50, 0);
  }
  q[GVelAmt] = lin("Velocity amount", "", -1, 1, 0);
  q[GVelDest] = ch("Velocity target", {"None", "Cutoff", "Resonance", "Mod amount", "Env 1 attack", "Env 1 decay", "Env 1 amount",
                                       "Env 2 attack", "Env 2 decay", "Env 2 amount", "LFO 1 rate", "LFO 1 amount", "LFO 2 rate",
                                       "LFO 2 amount", "Amp attack", "Amp decay"},
                   VdNone);
  q[GEqOn] = tog("EQ", false);
  for (int b = 0; b < kEqBands; ++b) q[GEq1 + b] = lin("EQ band " + std::to_string(b + 1), "dB", -12, 12, 0);
  for (int k = 0; k < kFxSlots; ++k) {
    const std::string f = std::string(kFxNames[k]) + " ";
    q[fx(k, FfOn)] = tog(f + "on", false);
    q[fx(k, FfX)] = lin(f + kFxX[k], "%", 0, 1, 0.5);
    q[fx(k, FfY)] = lin(f + kFxY[k], "%", 0, 1, 0.5);
    q[fx(k, FfDiv)] = ch(f + "LFO division", divs, 5);
    q[fx(k, FfShape)] = ch(f + "LFO shape", {"Sine", "Triangle", "Saw up", "Square"}, 0);
    q[fx(k, FfX4)] = tog(f + "LFO x4", false);
    q[fx(k, FfLfo)] = lin(f + "LFO amount", "", -1, 1, 0);
  }
  using Ov = std::vector<std::pair<int, float>>;
  p.presets = {
      {"Init", {}},
      {"Wide stack", Ov{{GUniVoices, 7}, {GUniDetune, 0.35f}, {GUniStereo, 0.9f}, {GOsc2Mix, 0.4f}, {osc(1, OsSemi), 12}, {GCutoff, 4200},
                        {GReso, 0.2f}, {eg(0, EgAmt), 0.3f}, {eg(0, EgDec), 0.8f}, {GAmpR, 0.6f}, {fx(7, FfOn), 1}, {fx(7, FfX), 0.55f}}},
      {"Sync lead", Ov{{GModKind, GkSync}, {GModAmt, 0.35f}, {eg(1, EgDest), EdMod}, {eg(1, EgAmt), 0.6f}, {eg(1, EgDec), 0.5f},
                       {GMono, 1}, {GSlide, 1}, {GCutoff, 6000}, {lfo(1, LfAmt), 0.08f}, {lfo(1, LfRate), 5.5f}, {fx(6, FfOn), 1},
                       {fx(6, FfX), 0.45f}}},
      {"FM bell keys", Ov{{osc(0, OsWave), WvSine}, {osc(1, OsWave), WvSine}, {osc(1, OsSemi), 19}, {GModKind, GkFm}, {GModAmt, 0.25f},
                          {eg(0, EgDest), EdMod}, {eg(0, EgAmt), 0.7f}, {eg(0, EgDec), 1.2f}, {GAmpS, 0}, {GAmpD, 6}, {GAmpR, 2}}},
      {"Acid bite", Ov{{osc(0, OsWave), WvSquare}, {GCutoff, 300}, {GReso, 0.75f}, {eg(0, EgAmt), 0.55f}, {eg(0, EgDec), 0.35f}, {GMono, 1},
                       {GSlide, 1}, {GOctave, -1}, {fx(0, FfOn), 1}, {fx(0, FfX), 0.45f}, {fx(0, FfY), 0.7f}}},
      {"Gated pad", Ov{{osc(0, OsWave), WvSoftSaw}, {GOsc3Mix, 0.35f}, {osc(2, OsWave), WvFormant}, {osc(2, OsSemi), 7}, {GAmpA, 2},
                       {GAmpR, 4}, {GUniVoices, 4}, {GCutoff, 2500}, {fx(9, FfOn), 1}, {fx(9, FfX), 0.6f}, {fx(9, FfY), 0.3f},
                       {fx(7, FfOn), 1}}},
  };
  p.editorWidth = 1040;
  return p;
}

namespace gms {

WaveFiles parseWaveFiles(const std::string& data) {
  WaveFiles w;
  std::istringstream in(data);
  std::string line;
  if (!std::getline(in, line) || line.rfind("gms ", 0) != 0) return w;
  while (std::getline(in, line)) {
    if (line.size() < 5 || line[0] != 'w' || line[1] != ' ') continue;
    const int k = line[2] - '0';
    if (k < 0 || k >= kOscs || line[3] != ' ') continue;
    w.path[k] = line.substr(4);
  }
  return w;
}

std::string serializeWaveFiles(const WaveFiles& w) {
  std::string s;
  for (int k = 0; k < kOscs; ++k)
    if (!w.path[k].empty()) s += "w " + std::to_string(k) + " " + w.path[k] + "\n";
  return s.empty() ? "" : "gms 1\n" + s;
}

}  // namespace gms

}  // namespace gl::packs::synths
