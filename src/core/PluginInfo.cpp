#include "../packs/Packs.h"
#include "PluginInfo.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <cstdio>
#include "PluginParams.h"

namespace gl {

double ParamDesc::toNormalized(double v) const {
  v = clamp(v);
  if (kind == ParamKind::Choice) {
    double hi = choices.empty() ? max : static_cast<double>(choices.size() - 1);
    return hi > 0 ? v / hi : 0.0;
  }
  if (max <= min) return 0.0;
  if (scale == ParamScale::Log && min > 0) return std::log(v / min) / std::log(max / min);
  return (v - min) / (max - min);
}

double ParamDesc::fromNormalized(double n) const {
  n = n < 0 ? 0 : (n > 1 ? 1 : n);
  if (kind == ParamKind::Choice) {
    double hi = choices.empty() ? max : static_cast<double>(choices.size() - 1);
    return clamp(n * hi);
  }
  if (scale == ParamScale::Log && min > 0) return clamp(min * std::pow(max / min, n));
  return clamp(min + n * (max - min));
}

std::string ParamDesc::format(double v) const {
  v = clamp(v);
  char buf[64];
  if (kind == ParamKind::Toggle) return v >= 0.5 ? "On" : "Off";
  if (kind == ParamKind::Choice) {
    size_t i = static_cast<size_t>(v);
    return i < choices.size() ? choices[i] : std::to_string(i);
  }
  if (unit == "Hz") {
    if (std::fabs(v) >= 1000)
      std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0);
    else if (std::fabs(v) < 10)
      std::snprintf(buf, sizeof buf, "%.2f Hz", v);
    else if (std::fabs(v) < 100)
      std::snprintf(buf, sizeof buf, "%.1f Hz", v);
    else
      std::snprintf(buf, sizeof buf, "%.0f Hz", v);
  } else if (unit == "s") {
    if (v < 1.0)
      std::snprintf(buf, sizeof buf, "%.0f ms", v * 1000.0);
    else
      std::snprintf(buf, sizeof buf, "%.2f s", v);
  } else if (unit == "ms") {
    std::snprintf(buf, sizeof buf, "%.1f ms", v);
  } else if (unit == "%") {
    std::snprintf(buf, sizeof buf, "%.0f%%", v * 100.0);
  } else if (unit == "dB") {
    std::snprintf(buf, sizeof buf, "%+.1f dB", v);
  } else if (unit == "st") {
    std::snprintf(buf, sizeof buf, "%+.0f st", v);
  } else if (unit == "ct") {
    std::snprintf(buf, sizeof buf, "%+.0f ct", v);
  } else if (unit == "pan") {
    if (std::fabs(v) < 0.005) return "Centred";
    std::snprintf(buf, sizeof buf, "%.0f%% %s", std::fabs(v) * 100.0, v < 0 ? "left" : "right");
  } else if (unit == "times") {
    std::snprintf(buf, sizeof buf, "x%.2f", v);
  } else if (unit == "x") {
    std::snprintf(buf, sizeof buf, "%.1f:1", v);
  } else if (unit == "note") {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = static_cast<int>(v + 0.5);
    std::snprintf(buf, sizeof buf, "%s%d", names[((n % 12) + 12) % 12], n / 12 - 1);
  } else if (unit.empty()) {
    bool integral = max - min >= 4 && std::floor(def) == def && std::floor(min) == min;
    std::snprintf(buf, sizeof buf, integral ? "%.0f" : "%.2f", integral ? std::round(v) : v);
  } else {
    std::snprintf(buf, sizeof buf, "%.2f %s", v, unit.c_str());
  }
  return buf;
}

namespace {

ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const char* n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

const std::vector<std::string> kShapes = {"Sine", "Triangle", "Saw", "Square", "Noise", "Rounded saw", "Pulse 25%"};

std::vector<PluginInfo> build() {
  using namespace pp;
  std::vector<PluginInfo> v;

  {
    PluginInfo p{"gl.synth", "Trio Synth", "Synth", PluginKind::Generator, {}, true, {}};
    p.presets = {};
    p.params.resize(SynCount);
    p.params[SynOsc1Shape] = ch("Osc 1 shape", kShapes, 2);
    p.params[SynOsc1Level] = lin("Osc 1 level", "%", 0, 1, 0.8);
    p.params[SynOsc2Shape] = ch("Osc 2 shape", kShapes, 3);
    p.params[SynOsc2Level] = lin("Osc 2 level", "%", 0, 1, 0.4);
    p.params[SynOsc2Coarse] = lin("Osc 2 coarse", "st", -24, 24, -12);
    p.params[SynOsc2Fine] = lin("Osc 2 fine", "ct", -100, 100, 6);
    p.params[SynOsc3Shape] = ch("Osc 3 shape", kShapes, 2);
    p.params[SynOsc3Level] = lin("Osc 3 level", "%", 0, 1, 0.0);
    p.params[SynOsc3Coarse] = lin("Osc 3 coarse", "st", -24, 24, 7);
    p.params[SynOsc3Fine] = lin("Osc 3 fine", "ct", -100, 100, -6);
    p.params[SynNoise] = lin("Noise", "%", 0, 1, 0);
    p.params[SynAttack] = lg("Attack", "s", 0.001, 5, 0.005);
    p.params[SynDecay] = lg("Decay", "s", 0.005, 5, 0.3);
    p.params[SynSustain] = lin("Sustain", "%", 0, 1, 0.7);
    p.params[SynRelease] = lg("Release", "s", 0.005, 8, 0.25);
    p.params[SynCutoff] = lg("Cutoff", "Hz", 30, 20000, 6000);
    p.params[SynResonance] = lin("Resonance", "%", 0, 1, 0.2);
    p.params[SynFilterEnv] = lin("Filter env", "%", -1, 1, 0.3);
    p.params[SynFilterDecay] = lg("Filter decay", "s", 0.005, 5, 0.4);
    p.params[SynGlide] = lg("Glide", "s", 0.001, 2, 0.001);
    p.params[SynVoices] = lin("Voices", "", 1, 32, 16);
    p.params[SynVolume] = lin("Volume", "%", 0, 1, 0.7);
    p.params[SynOsc1Coarse] = lin("Osc 1 coarse", "st", -24, 24, 0);
    p.params[SynOsc1Fine] = lin("Osc 1 fine", "ct", -100, 100, 0);
    for (int o = 0; o < 3; ++o) {
      std::string n = "Osc " + std::to_string(o + 1);
      p.params[SynOsc1Pan + o] = lin((n + " pan").c_str(), "pan", -1, 1, 0);
      p.params[SynOsc1Invert + o] = tog((n + " invert").c_str(), 0);
      p.params[SynOsc1Phase + o] = lin((n + " stereo phase").c_str(), "%", -0.5, 0.5, 0);
      p.params[SynOsc1Detune + o] = lin((n + " stereo detune").c_str(), "ct", -50, 50, 0);
    }
    p.params[SynPhaseRand] = lin("Phase randomness", "%", 0, 1, 0);
    p.params[SynOsc3Mode] = ch("Osc 3 mode", {"Normal", "AM", "Ring", "FM", "PM"}, 0);
    p.params[SynOsc2Mod] = ch("Osc 2 to osc 1", {"Off", "Ring", "FM", "PM", "Sync"}, 0);
    p.params[SynFilterType] = ch("Filter type", {"Low pass", "Low pass 24", "Band pass", "High pass", "Notch"}, 0);
    p.params[SynVelCutoff] = lin("Velocity to cutoff", "%", -1, 1, 0);
    p.params[SynKeyTrack] = lin("Key tracking", "%", 0, 1, 0);
    p.params[SynLfoTarget] = ch("LFO target", {"Off", "Pitch", "Cutoff", "Volume", "Pan"}, 0);
    p.params[SynLfoShape] = ch("LFO shape", {"Sine", "Triangle", "Square", "Saw", "Random"}, 0);
    p.params[SynLfoRate] = lg("LFO rate", "Hz", 0.05, 20, 4);
    p.params[SynLfoAmount] = lin("LFO amount", "%", 0, 1, 0.2);
    p.params[SynHQ] = tog("High quality oscillators", 1);
    for (int k : {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape}) {
      ParamDesc& d = p.params[static_cast<size_t>(k)];
      d.choices.push_back("Custom wave");
      d.max = static_cast<double>(d.choices.size() - 1);
    }
    p.usesSample = true;  // custom-wave oscillators read the channel sample
    p.editorWidth = 600;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.sampler", "Sampler", "Sampler", PluginKind::Generator, {}, true, {}};
    p.params.resize(SmpCount);
    p.params[SmpRoot] = lin("Root note", "note", 0, 127, 60);
    p.params[SmpFine] = lin("Fine tune", "ct", -100, 100, 0);
    p.params[SmpAttack] = lg("Attack", "s", 0.0005, 5, 0.0005);
    p.params[SmpDecay] = lg("Decay", "s", 0.005, 10, 10);
    p.params[SmpSustain] = lin("Sustain", "%", 0, 1, 1);
    p.params[SmpRelease] = lg("Release", "s", 0.002, 8, 0.02);
    p.params[SmpStart] = lin("Start offset", "%", 0, 1, 0);
    p.params[SmpReverse] = tog("Reverse", 0);
    p.params[SmpLoop] = tog("Loop", 0);
    p.params[SmpOneShot] = tog("One shot", 1);
    p.params[SmpVolume] = lin("Volume", "%", 0, 1, 0.8);
    p.params[SmpEnd] = lin("End", "%", 0, 1, 1);
    p.params[SmpLoopStart] = lin("Loop start", "%", 0, 1, 0);
    p.params[SmpLoopEnd] = lin("Loop end", "%", 0, 1, 1);
    p.params[SmpLoopMode] = ch("Loop mode", {"Forward", "Ping-pong"}, 0);
    p.params[SmpNormalize] = tog("Normalize", 0);
    p.params[SmpPolarity] = tog("Reverse polarity", 0);
    p.params[SmpRemoveDc] = tog("Remove DC", 0);
    p.params[SmpSwapStereo] = tog("Swap stereo", 0);
    p.params[SmpFadeIn] = lin("Fade in", "%", 0, 0.5, 0);
    p.params[SmpFadeOut] = lin("Fade out", "%", 0, 0.5, 0);
    p.params[SmpLoopXfade] = lin("Loop crossfade", "%", 0, 0.5, 0);
    p.params[SmpDeclick] = ch("Declick", {"Off", "Out only", "Transient", "Generic", "Smooth"}, 0);
    p.params[SmpStretchMode] = ch("Time mode", {"Resample", "Stretch"}, 0);
    p.params[SmpPitchShift] = lin("Pitch shift", "ct", -1200, 1200, 0);
    p.params[SmpTimeStretch] = lg("Time stretch", "times", 0.25, 4, 1);
    p.params[SmpEnvDelay] = lin("Env delay", "s", 0, 5, 0);
    p.params[SmpEnvHold] = lin("Env hold", "s", 0, 10, 0);
    p.params[SmpFilterType] = ch("Filter", {"Off", "Low pass", "Low pass 24", "Band pass", "High pass", "Notch"}, 0);
    p.params[SmpCutoff] = lg("Cutoff", "Hz", 20, 20000, 20000);
    p.params[SmpReso] = lin("Resonance", "%", 0, 1, 0);
    p.params[SmpFiltEnv] = lin("Filter env", "%", -1, 1, 0);
    p.params[SmpFiltAttack] = lg("Filter attack", "s", 0.001, 10, 0.001);
    p.params[SmpFiltDecay] = lg("Filter decay", "s", 0.005, 30, 0.3);
    p.params[SmpFiltSustain] = lin("Filter sustain", "%", 0, 1, 0);
    p.params[SmpFiltRelease] = lg("Filter release", "s", 0.005, 10, 0.3);
    p.params[SmpVelCutoff] = lin("Velocity to cutoff", "%", -1, 1, 0);
    p.params[SmpKeyCutoff] = lin("Key to cutoff", "%", -1, 1, 0);
    p.params[SmpLfoTarget] = ch("LFO target", {"Off", "Volume", "Pan", "Cutoff", "Pitch"}, 0);
    p.params[SmpLfoShape] = ch("LFO shape", {"Sine", "Triangle", "Pulse"}, 0);
    p.params[SmpLfoRate] = lg("LFO rate", "Hz", 0.01, 20, 2);
    p.params[SmpLfoAmount] = lin("LFO amount", "%", -1, 1, 0);
    p.params[SmpLfoDelay] = lin("LFO delay", "s", 0, 5, 0);
    p.params[SmpPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[SmpVelVolume] = lin("Velocity to volume", "%", 0, 1, 1);
    p.params[SmpVoices] = lin("Voices", "", 1, 32, 32);
    p.params[SmpGlide] = lg("Glide", "s", 0.001, 2, 0.001);
    p.params[SmpPreRampIn] = lin("Ramp in", "x", 0, 4, 0);
    p.params[SmpPreRampOut] = lin("Ramp out", "x", 0, 4, 0);
    p.params[SmpPreBoost] = lin("Boost", "%", 0, 1, 0);
    p.params[SmpPreClip] = tog("Boost clip", 0);
    p.params[SmpPreTilt] = lin("Tilt", "%", -1, 1, 0);
    p.params[SmpPreCutoff] = lin("Pre cutoff", "%", 0.015625, 1, 1);
    p.params[SmpPreReso] = lin("Pre resonance", "%", 0, 1, 0);
    p.params[SmpPreRingMix] = lin("Ring mix", "%", 0, 1, 0);
    p.params[SmpPreRingFreq] = lg("Ring freq", "Hz", 61.25, 2021.25, 1041.25);
    p.params[SmpPreStereoDelay] = lin("Stereo delay", "ms", -46.44, 46.44, 0);
    p.params[SmpPreWarp] = lin("Time warp", "%", -1, 1, 0);
    p.params[SmpPreReverbMix] = lin("Pre reverb", "%", 0, 1, 0);
    p.params[SmpPreReverbType] = ch("Pre reverb type", {"Room", "Hall"}, 1);
    p.params[SmpPreFadeStereo] = tog("Stereo fade", 0);
    p.presets = {{"One shot", {}}, {"Sustained", {{SmpOneShot, 0.f}, {SmpSustain, 1.f}, {SmpRelease, 0.3f}, {SmpDeclick, 3.f}}},
                 {"Looped pad", {{SmpOneShot, 0.f}, {SmpLoop, 1.f}, {SmpLoopStart, 0.2f}, {SmpLoopEnd, 0.8f}, {SmpLoopXfade, 0.2f}, {SmpAttack, 0.3f}, {SmpRelease, 1.2f}}},
                 {"Filtered pluck", {{SmpFilterType, 1.f}, {SmpCutoff, 800.f}, {SmpFiltEnv, 0.6f}, {SmpFiltDecay, 0.25f}, {SmpReso, 0.3f}}},
                 {"Mono glide", {{SmpOneShot, 0.f}, {SmpVoices, 1.f}, {SmpGlide, 0.08f}}}};
    p.editorWidth = 680;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.kick", "Kick", "Drums", PluginKind::Generator, {}, false, {}};
    p.params.resize(KickCount);
    p.params[KickPitch] = lg("Start pitch", "Hz", 40, 1000, 220);
    p.params[KickEndPitch] = lg("End pitch", "Hz", 25, 200, 48);
    p.params[KickSweep] = lg("Sweep", "s", 0.005, 0.5, 0.06);
    p.params[KickDecay] = lg("Decay", "s", 0.05, 2, 0.45);
    p.params[KickClick] = lin("Click", "%", 0, 1, 0.3);
    p.params[KickDrive] = lin("Drive", "%", 0, 1, 0.2);
    p.params[KickVolume] = lin("Volume", "%", 0, 1, 0.9);
    p.params[KickKeyTrack] = tog("Key tracking", 1);
    p.params[KickSub] = lin("Sub", "%", 0, 1, 0);
    p.params[KickClickTone] = lg("Click tone", "Hz", 1000, 12000, 4000);
    p.params[KickClickLen] = lg("Click length", "s", 0.001, 0.02, 0.004);
    p.params[KickAttack] = lin("Attack", "s", 0, 0.02, 0);
    p.params[KickTone] = lg("Tone", "Hz", 200, 20000, 20000);
    p.params[KickVariation] = lin("Variation", "%", 0, 1, 0);
    p.params[KickNoise] = lin("Noise", "%", 0, 1, 0);
    p.params[KickNoiseDecay] = lg("Noise decay", "s", 0.005, 1, 0.08);
    p.params[KickSweepCurve] = ch("Sweep curve", {"Exponential", "Linear"}, 0);
    p.presets = {{"Default", {}}, {"Deep 808", {{KickDecay, 1.2f}, {KickEndPitch, 42.f}, {KickSweep, 0.09f}, {KickClick, 0.1f}, {KickSub, 0.3f}}},
                 {"Punchy", {{KickPitch, 340.f}, {KickSweep, 0.03f}, {KickDecay, 0.3f}, {KickClick, 0.6f}, {KickDrive, 0.4f}}},
                 {"Lo-fi", {{KickDrive, 0.8f}, {KickTone, 2500.f}, {KickNoise, 0.2f}}}};
    p.editorWidth = 600;  // channel settings window width
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.snare", "Snare", "Drums", PluginKind::Generator, {}, false, {}};
    p.params.resize(SnCount);
    p.params[SnTone] = lin("Tone", "%", 0, 1, 0.5);
    p.params[SnSnappy] = lin("Snappy", "%", 0, 1, 0.6);
    p.params[SnDecay] = lg("Body decay", "s", 0.02, 1, 0.12);
    p.params[SnNoiseDecay] = lg("Noise decay", "s", 0.02, 1, 0.2);
    p.params[SnTune] = lg("Tune", "Hz", 100, 400, 190);
    p.params[SnVolume] = lin("Volume", "%", 0, 1, 0.8);
    p.params[SnSweep] = lin("Pitch sweep", "st", 0, 24, 0);
    p.params[SnSweepTime] = lg("Sweep time", "s", 0.002, 0.2, 0.02);
    p.params[SnNoiseBand] = lg("Noise band", "Hz", 1000, 12000, 1800);
    p.params[SnNoiseWidth] = lin("Noise width", "%", 0, 1, 1);
    p.params[SnRing] = lin("Ring mod", "%", 0, 1, 0);
    p.params[SnHarmonics] = lin("Body harmonics", "%", -1, 1, 0);
    p.params[SnClick] = lin("Click", "%", 0, 1, 0);
    p.params[SnTilt] = lin("Tone tilt", "dB", -12, 12, 0);
    p.params[SnPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[SnVelTone] = lin("Velocity to tone", "%", -1, 1, 0);
    p.presets = {{"Default", {}}, {"Tight", {{SnDecay, 0.07f}, {SnNoiseDecay, 0.1f}, {SnSweep, 5.f}, {SnClick, 0.3f}}},
                 {"Fat", {{SnTune, 150.f}, {SnDecay, 0.25f}, {SnNoiseDecay, 0.35f}, {SnHarmonics, 0.4f}}},
                 {"Rim", {{SnSnappy, 0.1f}, {SnTune, 380.f}, {SnDecay, 0.03f}, {SnRing, 0.6f}}}};
    p.editorWidth = 600;  // channel settings window width
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.hat", "Hi-hat", "Drums", PluginKind::Generator, {}, false, {}};
    p.params.resize(HatCount);
    p.params[HatDecay] = lg("Decay", "s", 0.01, 1.5, 0.06);
    p.params[HatTone] = lg("Tone", "Hz", 3000, 16000, 8000);
    p.params[HatOpen] = tog("Open", 0);
    p.params[HatVolume] = lin("Volume", "%", 0, 1, 0.5);
    p.params[HatMetal] = lin("Metal", "%", 0, 1, 0.17);
    p.params[HatBase] = lg("Base freq", "Hz", 100, 800, 205);
    p.params[HatWidth] = lin("Width", "%", 0, 1, 0.5);
    p.params[HatChoke] = ch("Choke", {"None", "Group"}, 1);
    p.params[HatAttack] = lin("Attack", "s", 0, 0.02, 0);
    p.params[HatClick] = lin("Click", "%", 0, 1, 0);
    p.params[HatPan] = lin("Pan", "pan", -1, 1, 0);
    p.presets = {{"Closed", {}}, {"Open", {{HatOpen, 1.f}}}, {"Metallic", {{HatMetal, 0.7f}, {HatBase, 330.f}}},
                 {"Soft shaker", {{HatMetal, 0.f}, {HatAttack, 0.008f}, {HatTone, 6000.f}}}};
    p.editorWidth = 600;  // channel settings window width
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.clap", "Clap", "Drums", PluginKind::Generator, {}, false, {}};
    p.params.resize(ClapCount);
    p.params[ClapDecay] = lg("Decay", "s", 0.05, 1, 0.25);
    p.params[ClapSpread] = lg("Spread", "s", 0.004, 0.03, 0.011);
    p.params[ClapTone] = lg("Tone", "Hz", 500, 5000, 1300);
    p.params[ClapVolume] = lin("Volume", "%", 0, 1, 0.7);
    p.params[ClapHits] = lin("Hits", "", 1, 8, 3);
    p.params[ClapHitDecay] = lg("Hit decay", "s", 0.002, 0.05, 0.0035);
    p.params[ClapFalloff] = lin("Hit falloff", "%", 0, 1, 0);
    p.params[ClapWidth] = lin("Width", "%", 0, 1, 0.5);
    p.params[ClapStereo] = lin("Stereo spread", "%", 0, 1, 0);
    p.params[ClapHumanize] = lin("Humanize", "%", 0, 1, 0);
    p.params[ClapPan] = lin("Pan", "pan", -1, 1, 0);
    p.presets = {{"Default", {}}, {"Wide", {{ClapStereo, 0.7f}, {ClapHits, 4.f}}}, {"Room", {{ClapDecay, 0.6f}, {ClapHumanize, 0.4f}}},
                 {"Snap", {{ClapHits, 1.f}, {ClapDecay, 0.12f}, {ClapTone, 2200.f}}}};
    p.editorWidth = 600;  // channel settings window width
    v.push_back(p);
  }

  {
    PluginInfo p{"gl.pluck", "Pluck", "Synth", PluginKind::Generator, {}, false, {}};
    p.params.resize(PlkCount);
    p.params[PlkDecay] = lg("Decay", "s", 0.1, 20, 2.5);
    p.params[PlkDamping] = lin("Damping", "%", 0, 1, 0.35);
    p.params[PlkBrightness] = lin("Brightness", "%", 0, 1, 0.7);
    p.params[PlkPosition] = lin("Pick position", "%", 0.02, 0.5, 0.18);
    p.params[PlkWidth] = lin("Stereo width", "%", 0, 1, 0.5);
    p.params[PlkRelease] = lg("Release", "s", 0.005, 2, 0.08);
    p.params[PlkVolume] = lin("Volume", "%", 0, 1, 0.8);
    p.params[PlkNormalize] = tog("Normalize decay", 1);
    p.params[PlkGate] = tog("Gate (damp on release)", 1);
    p.editorWidth = 600;  // channel settings window width
    p.presets = {{"Nylon", {{PlkDamping, 0.55f}, {PlkBrightness, 0.45f}, {PlkDecay, 3.0f}}},
                 {"Steel", {{PlkDamping, 0.2f}, {PlkBrightness, 0.9f}, {PlkDecay, 5.0f}}},
                 {"Harp", {{PlkDamping, 0.3f}, {PlkBrightness, 0.6f}, {PlkDecay, 8.0f}, {PlkPosition, 0.3f}}},
                 {"Muted", {{PlkDamping, 0.85f}, {PlkDecay, 0.4f}, {PlkBrightness, 0.4f}}}};
    v.push_back(p);
  }

  {
    PluginInfo p{"gl.pads", "Drum Pads", "Drums", PluginKind::Generator, {}, true, {}};
    p.params.resize(PadsCount);
    p.params[PadsBaseNote] = lin("Base note", "note", 0, 112, 60);
    p.params[PadsVelocity] = lin("Velocity to level", "%", 0, 1, 0.8);
    p.params[PadsVolume] = lin("Volume", "%", 0, 1, 0.8);
    const std::vector<std::string> groups = {"None", "1", "2", "3", "4", "5", "6", "7", "8"};
    for (int k = 0; k < kPadsAll; ++k) {  // bank A: pads 1..16, bank B: pads 17..32
      std::string n = "Pad " + std::to_string(k + 1);
      p.params[padP(k, PadTune)] = lin((n + " tune").c_str(), "st", -24, 24, 0);
      p.params[padP(k, PadLevel)] = lin((n + " level").c_str(), "%", 0, 1, 0.8);
      p.params[padP(k, PadPan)] = lin((n + " pan").c_str(), "pan", -1, 1, 0);
      p.params[padP(k, PadCut)] = ch((n + " cut group").c_str(), groups, k == 3 || k == 4 ? 1 : 0);
      p.params[padP(k, PadReverse)] = tog((n + " reverse").c_str(), 0);
      p.params[padE(k, PadNote)] = lin((n + " note").c_str(), "", -1, 127, -1);
      p.params[padE(k, PadCutBy)] = ch((n + " cut by").c_str(), groups, 0);
      p.params[padE(k, PadMute)] = tog((n + " mute").c_str(), 0);
      p.params[padE(k, PadSolo)] = tog((n + " solo").c_str(), 0);
      p.params[padE(k, PadVelScale)] = tog((n + " velocity scaling").c_str(), 1);
      p.params[padE(k, PadLayerMode)] = ch((n + " layer mode").c_str(), {"Stack", "Cycle", "Cycle back", "Random", "Random no repeat"}, 0);
      p.params[padE(k, PadAttack)] = lin((n + " attack").c_str(), "s", 0, 2, 0);
      p.params[padE(k, PadHold)] = lin((n + " hold").c_str(), "s", 0, 10, 10);
      p.params[padE(k, PadDecay)] = lg((n + " decay").c_str(), "s", 0.005, 10, 10);
      p.params[padE(k, PadSustain)] = lin((n + " sustain").c_str(), "%", 0, 1, 1);
      p.params[padE(k, PadRelease)] = lg((n + " release").c_str(), "s", 0.002, 5, 0.05);
      p.params[padE(k, PadOneShot)] = tog((n + " one shot").c_str(), 1);
      p.params[padE(k, PadStart)] = lin((n + " start").c_str(), "%", 0, 1, 0);
      for (int l = 0; l < kPadLayers; ++l) {
        std::string ln = n + " layer " + std::to_string(l + 1);
        p.params[padL(k, l, LayEnabled)] = tog((ln + " on").c_str(), l == 0 ? 1 : 0);
        p.params[padL(k, l, LayVelLow)] = lin((ln + " velocity low").c_str(), "", 0, 127, 0);
        p.params[padL(k, l, LayVelHigh)] = lin((ln + " velocity high").c_str(), "", 0, 127, 127);
        p.params[padL(k, l, LayLevel)] = lin((ln + " level").c_str(), "%", 0, 1, 1);
        p.params[padL(k, l, LayPan)] = lin((ln + " pan").c_str(), "pan", -1, 1, 0);
        p.params[padL(k, l, LayTune)] = lin((ln + " tune").c_str(), "st", -24, 24, 0);
        p.params[padL(k, l, LayReverse)] = tog((ln + " reverse").c_str(), 0);
        p.params[padL(k, l, LayStart)] = lin((ln + " start").c_str(), "%", 0, 1, 0);
        p.params[padL(k, l, LayEnd)] = lin((ln + " end").c_str(), "%", 0, 1, 1);
      }
    }
    p.editorWidth = 790;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.fm", "FM Synth", "Synth", PluginKind::Generator, {}, false, {}};
    p.params.resize(FmCount);
    const float ratios[kFmOps] = {1, 1, 2, 3.5f}, levels[kFmOps] = {1, 0.55f, 0.3f, 0.2f};
    for (int o = 0; o < kFmOps; ++o) {
      std::string n = "Op " + std::to_string(o + 1);
      p.params[fmParam(o, FmRatio)] = lin((n + " ratio").c_str(), "", 0.5, 16, ratios[o]);
      p.params[fmParam(o, FmFine)] = lin((n + " fine").c_str(), "ct", -100, 100, 0);
      p.params[fmParam(o, FmLevel)] = lin((n + " level").c_str(), "%", 0, 1, levels[o]);
      p.params[fmParam(o, FmAttack)] = lg((n + " attack").c_str(), "s", 0.001, 5, 0.002);
      p.params[fmParam(o, FmDecay)] = lg((n + " decay").c_str(), "s", 0.005, 10, o == 0 ? 1.2 : 0.6);
      p.params[fmParam(o, FmSustain)] = lin((n + " sustain").c_str(), "%", 0, 1, o == 0 ? 0.6 : 0.3);
      p.params[fmParam(o, FmRelease)] = lg((n + " release").c_str(), "s", 0.005, 10, 0.4);
      p.params[fmParam(o, FmVel)] = lin((n + " velocity").c_str(), "%", 0, 1, 0.5);
    }
    p.params[FmAlgorithm] = ch("Algorithm", {"4→3→2→1", "(3+4)→2→1", "3→2→1 + 4→1", "4→3→1 + 2→1", "4→3 + 2→1", "4→(1,2,3)", "4→3 + 2 + 1", "1+2+3+4"}, 0);
    p.params[FmFeedback] = lin("Feedback", "%", 0, 1, 0.1);
    p.params[FmVoices] = lin("Voices", "", 1, 16, 8);
    p.params[FmVolume] = lin("Volume", "%", 0, 1, 0.6);
    p.params[FmOctave] = lin("Octave", "", -3, 3, 0);
    p.params[FmLfoRate] = lg("LFO rate", "Hz", 0.1, 20, 5);
    p.params[FmVibrato] = lin("Vibrato", "ct", 0, 100, 0);
    p.params[FmLfoIndex] = lin("LFO to index", "%", 0, 1, 0);
    p.params[FmGlide] = lg("Glide", "s", 0.001, 2, 0.001);
    p.params[FmPitchEnv] = lin("Pitch env", "st", -24, 24, 0);
    for (int o = 0; o < kFmOps; ++o) {
      std::string n = "Op " + std::to_string(o + 1);
      p.params[fmExt(o, FmWave)] = lin((n + " waveform").c_str(), "%", 0, 1, 0);
      p.params[fmExt(o, FmFixed)] = tog((n + " fixed freq").c_str(), 0);
      p.params[fmExt(o, FmOffset)] = lin((n + " offset").c_str(), "Hz", -100, 100, 0);
      p.params[fmExt(o, FmThru)] = lin((n + " thru").c_str(), "%", 0, 1, 0);
      p.params[fmExt(o, FmPan)] = lin((n + " pan").c_str(), "pan", -1, 1, 0);
      p.params[fmExt(o, FmPhase)] = lin((n + " start phase").c_str(), "%", 0, 1, 0);
      p.params[fmExt(o, FmKeyScale)] = lin((n + " key scaling").c_str(), "%", -1, 1, 0);
      p.params[fmExt(o, FmInit)] = lin((n + " initial level").c_str(), "%", 0, 1, 0);
    }
    p.editorWidth = 760;
    auto op = [](int o, int f, float x) { return std::make_pair(fmParam(o, f), x); };
    p.presets = {{"E. piano", {}},
                 {"Bell", {op(1, FmRatio, 3.5f), op(1, FmLevel, 0.6f), op(0, FmDecay, 4.f), op(0, FmSustain, 0.f), {FmAlgorithm, 4.f}}},
                 {"FM bass", {op(1, FmRatio, 1.f), op(1, FmLevel, 0.7f), op(1, FmDecay, 0.25f), op(1, FmSustain, 0.1f), {FmVoices, 1.f}}},
                 {"Brass", {op(1, FmAttack, 0.08f), op(1, FmLevel, 0.65f), op(0, FmAttack, 0.04f), {FmFeedback, 0.4f}}},
                 {"Organ", {{FmAlgorithm, 7.f}, op(1, FmRatio, 2.f), op(2, FmRatio, 3.f), op(3, FmRatio, 4.f), op(0, FmSustain, 1.f), op(1, FmSustain, 1.f), op(2, FmSustain, 1.f), op(3, FmSustain, 1.f)}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.acid", "Acid Bass", "Synth", PluginKind::Generator, {}, false, {}};
    p.params.resize(AcCount);
    p.params[AcWave] = ch("Wave", {"Saw", "Square"}, 0);
    p.params[AcCutoff] = lg("Cutoff", "Hz", 60, 8000, 500);
    p.params[AcReso] = lin("Resonance", "%", 0, 1, 0.7);
    p.params[AcEnvMod] = lin("Env mod", "%", 0, 1, 0.55);
    p.params[AcDecay] = lg("Decay", "s", 0.03, 2, 0.25);
    p.params[AcAccent] = lin("Accent", "%", 0, 1, 0.6);
    p.params[AcSlide] = lg("Slide time", "s", 0.01, 0.5, 0.06);
    p.params[AcDrive] = lin("Drive", "%", 0, 1, 0.2);
    p.params[AcVolume] = lin("Volume", "%", 0, 1, 0.7);
    p.params[AcTuning] = lin("Tuning", "st", -12, 12, 0);
    p.params[AcBlend] = lin("Wave blend", "%", 0, 1, 0);
    p.params[AcVintage] = tog("Vintage square", 0);
    p.params[AcPulseWidth] = lin("Pulse width", "%", 0.05, 0.95, 0.5);
    p.params[AcPwmRate] = lg("PWM rate", "Hz", 0.05, 10, 1);
    p.params[AcPwmDepth] = lin("PWM depth", "%", 0, 0.45, 0);
    p.params[AcHighPass] = lg("High-pass", "Hz", 10, 500, 10);
    p.params[AcAccentDecay] = lg("Accent decay", "s", 0.03, 2, 0.25);
    p.params[AcKeyFollow] = lin("Key follow", "%", 0, 1, 0);
    p.params[AcClicks] = lin("Clicks", "%", 0, 1, 0);
    p.params[AcDriveTone] = lg("Drive tone", "Hz", 500, 20000, 20000);
    p.params[AcAccentThresh] = lin("Accent threshold", "%", 0, 1, 0.9);
    p.params[AcSeqOn] = tog("Sequencer", 0);
    p.params[AcSeqLength] = lin("Steps", "", 1, 16, 16);
    p.params[AcSeqShuffle] = lin("Shuffle", "%", 0, 1, 0);
    p.params[AcSeqGate] = lin("Gate length", "%", 0.1, 1, 0.5);
    for (int k = 0; k < kAcSteps; ++k) {
      std::string n = "Step " + std::to_string(k + 1);
      p.params[acStep(k, AcStepNote)] = ch((n + " note").c_str(), {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C+"}, 0);
      p.params[acStep(k, AcStepOctave)] = ch((n + " octave").c_str(), {"Down", "Mid", "Up"}, 1);
      p.params[acStep(k, AcStepAccent)] = tog((n + " accent").c_str(), 0);
      p.params[acStep(k, AcStepSlide)] = tog((n + " slide").c_str(), 0);
      p.params[acStep(k, AcStepGate)] = ch((n + " gate").c_str(), {"Rest", "Note", "Tie"}, k % 2 == 0 ? 1 : 0);
    }
    p.editorWidth = 700;
    p.presets = {{"Classic", {}}, {"Squelch", {{AcCutoff, 300.f}, {AcReso, 0.9f}, {AcEnvMod, 0.85f}, {AcDecay, 0.4f}}},
                 {"Square sub", {{AcWave, 1.f}, {AcCutoff, 900.f}, {AcReso, 0.3f}, {AcEnvMod, 0.2f}}},
                 {"Dirty", {{AcDrive, 0.8f}, {AcReso, 0.8f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.keys", "Keys", "Synth", PluginKind::Generator, {}, false, {}};
    p.params.resize(KyCount);
    p.params[KyDecay] = lg("Decay", "s", 0.3, 12, 3.5);
    p.params[KyRelease] = lg("Release", "s", 0.02, 3, 0.25);
    p.params[KyHardness] = lin("Hardness", "%", 0, 1, 0.45);
    p.params[KyTreble] = lin("Treble", "dB", -12, 12, 0);
    p.params[KyMuffle] = lg("Muffle", "Hz", 500, 20000, 9000);
    p.params[KyTremRate] = lg("Tremolo rate", "Hz", 0.1, 10, 4.5);
    p.params[KyTremDepth] = lin("Tremolo depth", "%", 0, 1, 0.15);
    p.params[KyStereo] = lin("Stereo", "%", 0, 1, 0.5);
    p.params[KyDetune] = lin("Detune", "ct", 0, 30, 4);
    p.params[KyDrive] = lin("Overdrive", "%", 0, 1, 0.1);
    p.params[KyVelocity] = lin("Velocity sens.", "%", 0, 1, 0.7);
    p.params[KyVolume] = lin("Volume", "%", 0, 1, 0.7);
    p.params[KyVelHardness] = lin("Velocity to hardness", "%", -1, 1, 0);
    p.params[KyVelMuffle] = lin("Velocity to muffle", "%", 0, 1, 0);
    p.params[KyTune] = lin("Tune", "ct", -100, 100, 0);
    p.params[KyStretch] = lin("Stretch", "ct", -50, 50, 0);
    p.params[KyModMode] = ch("Modulation", {"Tremolo", "Auto-pan"}, 0);
    p.params[KyModel] = ch("Model", {"Tine", "Reed", "Bright tine", "Soft", "Grand piano", "Upright piano"}, 0);
    p.params[KyVelCurve] = ch("Velocity curve", {"Linear", "Soft", "Hard"}, 0);
    p.params[KyKeyPan] = lin("Key-tracked pan", "%", 0, 1, 0);
    p.editorWidth = 600;
    p.presets = {{"Classic tine", {}}, {"Soft", {{KyHardness, 0.2f}, {KyMuffle, 4000.f}, {KyTremDepth, 0.f}}},
                 {"Bright bark", {{KyHardness, 0.85f}, {KyDrive, 0.5f}, {KyTreble, 4.f}}},
                 {"Wide tremolo", {{KyTremDepth, 0.6f}, {KyTremRate, 5.5f}, {KyStereo, 1.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.slicer", "Slicer", "Sampler", PluginKind::Generator, {}, true, {}};
    p.params.resize(SlCount);
    p.params[SlMode] = ch("Slicing", {"Transients", "4", "8", "16", "32", "Manual"}, 0);
    p.params[SlSensitivity] = lin("Sensitivity", "%", 0, 1, 0.5);
    p.params[SlBaseNote] = lin("Base note", "note", 0, 120, 60);
    p.params[SlPitch] = lin("Pitch", "st", -24, 24, 0);
    p.params[SlAttack] = lg("Attack", "s", 0.0005, 0.5, 0.001);
    p.params[SlRelease] = lg("Release", "s", 0.002, 1, 0.01);
    p.params[SlReverse] = tog("Reverse", 0);
    p.params[SlVolume] = lin("Volume", "%", 0, 1, 0.8);
    p.params[SlFine] = lin("Fine", "ct", -100, 100, 0);
    p.params[SlPlayToEnd] = tog("Play to end", 0);
    p.params[SlHold] = tog("Hold", 0);
    p.params[SlMono] = tog("Monophonic", 0);
    p.params[SlFadeIn] = lin("Fade in", "%", 0, 1, 0);
    p.params[SlFadeOut] = lin("Fade out", "%", 0, 1, 0);
    p.params[SlDeclick] = tog("Declick", 1);
    p.params[SlFitTempo] = tog("Fit to tempo", 0);
    p.params[SlLoopBeats] = lin("Loop length", "beats", 1, 64, 4);
    p.params[SlFilterType] = ch("Filter", {"Off", "Low pass", "High pass", "Band pass"}, 0);
    p.params[SlCutoff] = lg("Cutoff", "Hz", 20, 20000, 20000);
    p.params[SlReso] = lin("Resonance", "%", 0, 1, 0);
    p.params[SlPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[SlVelocity] = lin("Velocity to level", "%", 0, 1, 1);
    p.params[SlChromatic] = lin("Chromatic slice", "", -1, 63, -1);
    p.params[SlMinGap] = lg("Minimum slice gap", "s", 0.01, 0.5, 0.06);
    for (int k = 0; k < kMaxSlices; ++k) {
      std::string n = "Slice " + std::to_string(k + 1);
      p.params[slMarker(k)] = lin((n + " position").c_str(), "%", -1, 1, -1);
      p.params[slParam(k, SliceGain)] = lin((n + " gain").c_str(), "dB", -24, 12, 0);
      p.params[slParam(k, SlicePan)] = lin((n + " pan").c_str(), "pan", -1, 1, 0);
      p.params[slParam(k, SlicePitch)] = lin((n + " pitch").c_str(), "st", -24, 24, 0);
      p.params[slParam(k, SliceReverse)] = tog((n + " reverse").c_str(), 0);
      p.params[slParam(k, SliceCutoff)] = lin((n + " cutoff").c_str(), "%", -1, 1, 0);
      p.params[slParam(k, SliceStart)] = lin((n + " start").c_str(), "%", 0, 1, 0);
      p.params[slParam(k, SliceMute)] = tog((n + " mute").c_str(), 0);
      p.params[slParam2(k, SliceEnd)] = lin((n + " end").c_str(), "%", -1, 1, -1);
      p.params[slParam2(k, SliceReso)] = lin((n + " resonance").c_str(), "%", -1, 1, 0);
      p.params[slParam2(k, SliceDelay)] = lin((n + " delay").c_str(), "s", 0, 1, 0);
      p.params[slParam2(k, SliceCut)] = lin((n + " cut group").c_str(), "", 0, 99, 0);
      p.params[slParam2(k, SliceKey)] = lin((n + " key").c_str(), "", -1, 127, -1);
    }
    p.params[SlModX] = lin("Modulation X (cutoff)", "%", 0, 1, 0.5);
    p.params[SlModY] = lin("Modulation Y (resonance)", "%", 0, 1, 0.5);
    p.presets = {{"Transients", {}}, {"Even 16ths, mono", {{SlMode, 3.f}, {SlMono, 1.f}}}, {"Fit to tempo", {{SlFitTempo, 1.f}, {SlMono, 1.f}}},
                 {"Lo-fi", {{SlFilterType, 1.f}, {SlCutoff, 3000.f}, {SlPitch, -2.f}}}};
    p.editorWidth = 714;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.granular", "Granular", "Sampler", PluginKind::Generator, {}, true, {}};
    p.params.resize(GrCount);
    p.params[GrPosition] = lin("Position", "%", 0, 1, 0.3);
    p.params[GrSpread] = lin("Spread", "%", 0, 1, 0.1);
    p.params[GrSize] = lg("Grain size", "s", 0.01, 0.5, 0.08);
    p.params[GrDensity] = lg("Density", "Hz", 2, 120, 25);
    p.params[GrPitchRand] = lin("Pitch random", "st", 0, 12, 0.1);
    p.params[GrStereo] = lin("Stereo", "%", 0, 1, 0.6);
    p.params[GrAttack] = lg("Attack", "s", 0.001, 4, 0.05);
    p.params[GrRelease] = lg("Release", "s", 0.01, 8, 0.6);
    p.params[GrFreeze] = tog("Freeze position", 1);
    p.params[GrVolume] = lin("Volume", "%", 0, 1, 0.7);
    p.params[GrSpeed] = lin("Speed", "times", -2, 2, 1);
    p.params[GrShape] = lin("Grain shape", "%", 0.02, 0.5, 0.5);
    p.params[GrLoop] = tog("Loop", 1);
    p.params[GrKeyMode] = ch("Keys", {"Pitch", "Position", "Steps"}, 0);
    p.params[GrBaseNote] = lin("Base note", "note", 0, 127, 60);
    p.params[GrTempoSync] = tog("Tempo sync", 0);
    p.params[GrWobble] = lin("Wobble", "st", 0, 12, 0);
    p.params[GrWobbleRate] = lg("Wobble rate", "Hz", 0.1, 20, 4);
    p.params[GrJitter] = lin("Time jitter", "%", 0, 1, 0);
    p.params[GrTransientHold] = lin("Step hold", "s", 0, 2, 0.25);
    p.params[GrPitch] = lin("Pitch", "st", -24, 24, 0);
    p.editorWidth = 600;  // channel settings window width
    p.presets = {{"Cloud", {}}, {"Glitch", {{GrSize, 0.02f}, {GrDensity, 60.f}, {GrSpread, 0.5f}}},
                 {"Pad", {{GrSize, 0.25f}, {GrDensity, 12.f}, {GrAttack, 1.2f}, {GrRelease, 3.f}}},
                 {"Scan", {{GrFreeze, 0.f}, {GrSpread, 0.02f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.midiout", "MIDI Out", "MIDI", PluginKind::Generator, {}, false, {}};
    p.params.resize(MoCount);
    p.params[MoChannel] = lin("MIDI channel", "", 1, 16, 1);
    p.params[MoProgram] = lin("Program", "", 0, 128, 0);
    p.params[MoVelocity] = lin("Velocity scale", "%", 0, 2, 1);
    p.params[MoPort] = lin("Port", "", 0, 15, 0);
    p.params[MoBankMsb] = lin("Bank MSB", "", -1, 127, -1);
    p.params[MoBankLsb] = lin("Bank LSB", "", -1, 127, -1);
    p.params[MoBendRange] = lin("Pitch bend range", "st", 1, 48, 2);
    p.params[MoVelCurve] = ch("Velocity curve", {"Linear", "Soft", "Hard", "Fixed 100"}, 0);
    for (int k = 0; k < kMoKnobs; ++k) {
      std::string n = "Knob " + std::to_string(k + 1);
      p.params[moKnob(k, MoKnobValue)] = lin((n).c_str(), "", 0, 127, 0);
      p.params[moKnob(k, MoKnobType)] = ch((n + " type").c_str(), {"CC", "RPN", "NRPN", "Aftertouch", "Note"}, 0);
      p.params[moKnob(k, MoKnobNumber)] = lin((n + " number").c_str(), "", 0, 16383, 20 + k);
      p.params[moKnob(k, MoKnobMin)] = lin((n + " min").c_str(), "", 0, 16383, 0);
      p.params[moKnob(k, MoKnobMax)] = lin((n + " max").c_str(), "", 0, 16383, 127);
      p.params[moKnob(k, MoKnobChannel)] = lin((n + " channel").c_str(), "", 0, 16, 0);
    }
    p.editorWidth = 600;  // channel settings window width
    v.push_back(p);
  }

  // ---- effects ------------------------------------------------------------
  {
    PluginInfo p{"gl.eq", "Parametric EQ", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(EqCount);
    static const char* bandNames[] = {"1", "2", "3", "4", "5", "6", "7"};
    // Reference defaults: bands evenly spaced on the log axis (1/6 + k/9 of
    // 20 Hz..20 kHz), low shelf, five peaks, high shelf.
    double freqs[kEqBands];
    for (int b = 0; b < kEqBands; ++b) freqs[b] = 20.0 * std::pow(1000.0, 1.0 / 6.0 + b / 9.0);
    const int types[kEqBands] = {EqLowShelf, EqPeak, EqPeak, EqPeak, EqPeak, EqPeak, EqHighShelf};
    for (int b = 0; b < kEqBands; ++b) {
      std::string n = std::string("Band ") + bandNames[b];
      p.params[eqParam(b, EqType)] = ch((n + " type").c_str(),
                                        {"Off", "Low cut", "Band pass", "High cut", "Notch", "Low shelf", "Peak", "High shelf"}, types[b]);
      p.params[eqParam(b, EqFreq)] = lg((n + " freq").c_str(), "Hz", 20, 20000, freqs[b]);
      p.params[eqParam(b, EqGain)] = lin((n + " gain").c_str(), "dB", -18, 18, 0);
      p.params[eqParam(b, EqQ)] = lg((n + " Q (legacy)").c_str(), "", 0.1, 18, b == 0 || b == 6 ? 0.707 : 1.0);
      p.params[eqParam(b, EqSlope)] = ch((n + " slope (legacy)").c_str(), {"12 dB", "24 dB", "36 dB", "48 dB", "6 dB"}, 0);
      p.params[eqExt(b, EqOrder)] = ch((n + " order (legacy)").c_str(), {"Gentle", "Normal", "Steep", "Steeper", "Steepest"}, EqOrderNormal);
      p.params[eqExt(b, EqEnabled)] = tog((n + " enabled").c_str(), 1);
      // 25412 / 40124 of 65536 in the reference: 2.0 octaves (peaks), slope 1 (shelves).
      p.params[eqFl(b, EqWidth)] = lin((n + " width").c_str(), "%", 0, 1, b == 0 || b == 6 ? 40124.0 / 65536.0 : 25412.0 / 65536.0);
      p.params[eqFl(b, EqFlOrder)] = ch((n + " order").c_str(), {"Steep 8", "Steep 6", "Steep 4", "Normal", "Gentle 4", "Gentle 6", "Gentle 8"}, EqOrd2);
    }
    p.params[EqMain] = lin("Main level", "dB", -18, 18, 0);
    p.params[EqListen] = ch("Listen", {"Off", "Band 1", "Band 2", "Band 3", "Band 4", "Band 5", "Band 6", "Band 7"}, 0);
    p.params[EqSpectrum] = ch("Histogram", {"Off", "On", "On + peak hold"}, 1);
    p.params[EqHQ] = ch("Quality", {"Standard", "High (decramped)"}, 0);
    p.params[EqTokens] = tog("Band tokens", 1);
    p.params[EqSoloShift] = ch("Band solo on Shift", {"Off", "Adaptive", "Band pass", "Inherit width"}, 1);
    p.params[EqMonitor] = ch("Monitor", {"Off", "Input", "Output"}, 2);
    p.params[EqHistRange] = ch("Histogram range", {"-60 dB", "-90 dB", "-120 dB", "Matched"}, 1);
    p.params[EqPivot] = ch("Pivot slope", {"0 dB/oct", "1.5 dB/oct", "3 dB/oct", "4.5 dB/oct", "6 dB/oct"}, 2);
    p.params[EqSmoothing] = ch("Time smoothing", {"Fastest", "Fast", "Medium", "Slow", "Slowest"}, 2);
    p.params[EqHeatmap] = ch("Heat map", {"Off", "Top", "Bottom", "Full"}, 0);
    p.params[EqModel] = ch("Band model", {"Legacy", "Reference"}, 1);
    p.params[EqLinear] = ch("Linear phase", {"Off", "Lowest", "Low", "Medium", "High", "Higher", "Highest"}, 0);
    p.params[EqPhaseView] = tog("Show phase", 0);
    p.params[EqHistAvg] = tog("Histogram average", 0);
    p.editorWidth = 900;
    auto band = [](int b, int f, float v) { return std::make_pair(eqParam(b, f), v); };
    auto fl = [](int b, int f, float v) { return std::make_pair(eqFl(b, f), v); };
    p.presets = {{"Flat", {}},
                 {"Low cut 80 Hz", {band(0, EqType, EqLowCut), band(0, EqFreq, 80), fl(0, EqWidth, 0.55f), fl(0, EqFlOrder, EqSteep4)}},
                 {"Vocal presence", {band(0, EqType, EqLowCut), band(0, EqFreq, 100), fl(0, EqWidth, 0.55f), band(2, EqGain, -2.5f), band(2, EqFreq, 350),
                                     band(4, EqGain, 3), band(4, EqFreq, 3200), band(6, EqGain, 2), band(6, EqFreq, 11000)}},
                 {"Kick punch", {band(0, EqGain, 4), band(0, EqFreq, 55), band(2, EqGain, -4), band(2, EqFreq, 300), band(4, EqGain, 3), band(4, EqFreq, 4000)}},
                 {"Air", {band(6, EqGain, 4), band(6, EqFreq, 10000)}},
                 {"Telephone", {band(0, EqType, EqLowCut), band(0, EqFreq, 400), fl(0, EqWidth, 0.55f), fl(0, EqFlOrder, EqSteep6), band(6, EqType, EqHighCut),
                                band(6, EqFreq, 3200), fl(6, EqWidth, 0.55f), fl(6, EqFlOrder, EqSteep6), band(3, EqGain, 6), band(3, EqFreq, 1500)}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.filter", "Filter", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(FltCount);
    p.params[FltMode] = ch("Mode", {"Low pass", "Band pass", "High pass", "Notch", "Low shelf", "Peak", "High shelf"}, 0);
    p.params[FltCutoff] = lg("Cutoff", "Hz", 20, 20000, 20000);
    p.params[FltReso] = lin("Resonance", "%", 0, 1, 0.1);
    p.params[FltMix] = lin("Mix", "%", 0, 1, 1);
    p.params[FltSlope] = ch("Slope", {"12 dB/oct", "24 dB/oct"}, 0);
    p.params[FltGain] = lin("Gain", "dB", -24, 24, 0);
    p.params[FltBlend] = tog("Multi-mode blend", 0);
    p.params[FltLPLevel] = lin("Low pass level", "%", 0, 1, 1);
    p.params[FltBPLevel] = lin("Band pass level", "%", 0, 1, 0);
    p.params[FltHPLevel] = lin("High pass level", "%", 0, 1, 0);
    p.params[FltDcBlock] = tog("Remove DC", 0);
    p.editorWidth = 680;
    p.presets = {{"Open", {}}, {"Dark", {{FltCutoff, 1200.f}}}, {"Telephone band", {{FltMode, 1.f}, {FltCutoff, 1500.f}, {FltReso, 0.4f}}},
                 {"Thin", {{FltMode, 2.f}, {FltCutoff, 400.f}}},
                 {"Steep sweep", {{FltSlope, 1.f}, {FltCutoff, 900.f}, {FltReso, 0.5f}}}, {"Air shelf", {{FltMode, 6.f}, {FltCutoff, 9000.f}, {FltGain, 4.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.comp", "Compressor", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(CmpCount);
    p.params[CmpThreshold] = lin("Threshold", "dB", -60, 0, -18);
    p.params[CmpRatio] = lg("Ratio", "x", 0.4, 30, 4);
    p.params[CmpAttack] = lg("Attack", "s", 0.0001, 0.4, 0.005);
    p.params[CmpRelease] = lg("Release", "s", 0.001, 4, 0.12);
    p.params[CmpKnee] = lin("Knee", "dB", 0, 24, 6);
    p.params[CmpMakeup] = lin("Makeup", "dB", 0, 24, 0);
    p.params[CmpDetector] = ch("Detector", {"Peak", "RMS"}, 0);
    p.params[CmpCharacter] = ch("Character", {"Clean", "Vintage"}, 0);
    p.params[CmpLookahead] = lin("Lookahead", "ms", 0, 10, 0);
    p.params[CmpMix] = lin("Mix", "%", 0, 1, 1);
    p.params[CmpOutput] = lin("Gain", "dB", -30, 30, 0);
    p.params[CmpKeyHpf] = lg("Detector high-pass", "Hz", 20, 500, 20);
    p.params[CmpFastRelease] = tog("Fast release", 0);
    p.params[CmpLimiter] = tog("Output limiter", 1);
    p.params[CmpSidechain] = ch("Detector input", {"Own signal", "Sidechain"}, 0);
    p.editorWidth = 720;
    p.presets = {{"Gentle glue", {{CmpThreshold, -14.f}, {CmpRatio, 2.f}, {CmpAttack, 0.01f}, {CmpRelease, 0.2f}, {CmpMakeup, 2.f}}},
                 {"Punchy drums", {{CmpThreshold, -20.f}, {CmpRatio, 4.f}, {CmpAttack, 0.02f}, {CmpRelease, 0.08f}, {CmpMakeup, 4.f}}},
                 {"Vocal leveler", {{CmpThreshold, -22.f}, {CmpRatio, 3.f}, {CmpAttack, 0.003f}, {CmpRelease, 0.15f}, {CmpMakeup, 5.f}}},
                 {"Squash", {{CmpThreshold, -32.f}, {CmpRatio, 12.f}, {CmpAttack, 0.0005f}, {CmpRelease, 0.05f}, {CmpMakeup, 10.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.limiter", "Limiter", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(LimCount);
    // κ2: ranges and defaults follow the reference limiter (gain up to +18 dB,
    // ceiling up to +12 dB, times up to 1 s / 2 s, curves 1-8).
    p.params[LimGain] = lin("Gain", "dB", -36, 24, 0);
    p.params[LimCeiling] = lin("Ceiling", "dB", -36, 12, 0);
    p.params[LimRelease] = lg("Release", "s", 0.001, 2, 0.0855);
    p.params[LimAttack] = lg("Attack (look-ahead)", "s", 0.0001, 1, 0.002);
    p.params[LimHold] = lg("Sustain (peak window)", "s", 0.0001, 1, 0.01);
    p.params[LimRelCurve] = ch("Release curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 2);
    p.params[LimAtkCurve] = ch("Attack curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 2);
    p.params[LimSaturation] = lin("Saturation threshold", "dB", -36, 0, 0);
    p.params[LimCompThresh] = lin("Comp threshold", "dB", -60, 0, 0);
    p.params[LimCompRatio] = lg("Comp ratio", "x", 0.25, 20, 1);
    p.params[LimCompKnee] = lin("Comp soft knee", "dB", 0, 24, 0);
    p.params[LimCompAttack] = lg("Comp attack", "s", 0.0001, 1, 0.0001);
    p.params[LimCompRelease] = lg("Comp release", "s", 0.001, 2, 0.248);
    p.params[LimCompRms] = lg("Comp RMS window", "s", 0.0001, 1, 0.001);
    p.params[LimCompCurve] = ch("Comp curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 0);
    p.params[LimGateThresh] = lin("Noise threshold", "dB", -80, 0, -80);
    p.params[LimGateFloor] = lin("Noise gain", "dB", -80, 0, 0);
    p.params[LimGateRelease] = lg("Noise release", "s", 0.001, 2, 0.05);
    p.params[LimSafety] = tog("0.2 dB safety", 0);
    p.params[LimView] = ch("View", {"Limiter", "Compressor"}, 0);
    p.params[LimSidechain] = ch("Comp detector input", {"Own signal", "Sidechain"}, 0);
    p.params[LimCompShape] = lin("Comp knee", "%", -1, 1, 0);
    p.editorWidth = 780;
    p.presets = {{"Safety", {}}, {"Loud master", {{LimGain, 6.f}, {LimCeiling, -0.3f}, {LimRelease, 0.06f}}},
                 {"Brickwall -1 dB", {{LimCeiling, -1.f}}},
                 {"Bus glue", {{LimCompThresh, -18.f}, {LimCompRatio, 2.5f}, {LimCompAttack, 0.01f}, {LimCompRelease, 0.2f}, {LimGain, 3.f}}},
                 {"Gated drums", {{LimGateThresh, -38.f}, {LimGateFloor, -60.f}, {LimGateRelease, 0.08f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.gate", "Gate", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(GtCount);
    p.params[GtThreshold] = lin("Threshold", "dB", -80, 0, -40);
    p.params[GtAttack] = lg("Attack", "s", 0.0001, 0.1, 0.001);
    p.params[GtHold] = lg("Hold", "s", 0.001, 1, 0.02);
    p.params[GtRelease] = lg("Release", "s", 0.005, 2, 0.1);
    p.params[GtRange] = lin("Range", "dB", -80, 0, -80);
    p.params[GtHysteresis] = lin("Hysteresis", "dB", 0, 12, 0);
    p.params[GtLookahead] = lin("Look-ahead", "ms", 0, 5, 0);
    p.params[GtKeyHpf] = lg("Key high-pass", "Hz", 20, 2000, 20);
    p.params[GtKeyLpf] = lg("Key low-pass", "Hz", 200, 20000, 20000);
    p.params[GtFlip] = tog("Duck (inverse)", 0);
    p.editorWidth = 720;
    p.presets = {{"Noise gate", {}}, {"Tight drums", {{GtThreshold, -30.f}, {GtHold, 0.005f}, {GtRelease, 0.04f}}},
                 {"Gentle", {{GtThreshold, -45.f}, {GtRange, -12.f}, {GtRelease, 0.25f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.delay", "Delay", "Delay & reverb", PluginKind::Effect, {}, false, {}};
    p.params.resize(DlyCount);
    p.params[DlySync] = tog("Tempo sync", 1);
    p.params[DlyTime] = ch("Time", {"1/32", "1/16T", "1/16.", "1/16", "1/8T", "1/16..", "1/8", "1/4T", "1/8.", "1/4", "1/4.", "1/2", "1/64", "1/2T", "1/2.", "1 bar"}, 8);
    p.params[DlyTimeMs] = lg("Time (free)", "ms", 1, 3000, 300);
    p.params[DlyFeedback] = lin("Feedback", "%", 0, 1.25, 0.4);
    p.params[DlyMode] = ch("Feedback mode", {"Normal", "Ping-pong", "Invert", "Mono", "Off"}, 1);
    p.params[DlyOffset] = lin("Stereo offset", "ms", -500, 500, 0);
    p.params[DlyCutoff] = lg("Feedback cutoff", "Hz", 300, 20000, 7000);
    p.params[DlyLowCut] = lg("Feedback low cut", "Hz", 20, 2000, 60);
    p.params[DlyModRate] = lg("Mod rate", "Hz", 0.05, 10, 0.5);
    p.params[DlyModDepth] = lin("Mod depth", "%", 0, 1, 0);
    p.params[DlySaturation] = lin("Saturation", "%", 0, 1, 0);
    p.params[DlyDry] = lin("Dry", "%", 0, 1, 1);
    p.params[DlyWet] = lin("Wet", "%", 0, 1, 0.3);
    p.params[DlyFilterType] = ch("Feedback filter", {"Classic", "Resonant LP", "Resonant HP", "Resonant BP", "Off"}, 0);
    p.params[DlyResonance] = lin("Feedback resonance", "%", 0, 1, 0.3);
    p.params[DlyDiffusion] = lin("Diffusion", "%", 0, 1, 0);
    p.params[DlyDiffSpread] = lin("Diffusion spread", "%", 0, 1, 0.5);
    p.params[DlyDistType] = ch("Distortion", {"Off", "Limit", "Saturate"}, 0);
    p.params[DlyDistThresh] = lin("Threshold", "dB", -28.5, 0, 0);
    p.params[DlyDistHardness] = lin("Hardness", "%", 0, 1, 0);
    p.params[DlyDistSymmetry] = lin("Symmetry", "%", 0, 1, 0);
    p.params[DlyBits] = lin("Bit depth", "", 2, 16, 16);
    p.params[DlyRateReduce] = lg("Sample rate", "Hz", 500, 48000, 48000);
    p.params[DlyKeepPitch] = tog("Keep pitch", 0);
    p.params[DlyTimeSmooth] = lin("Time smooth", "ms", 0, 1000, 20);
    p.params[DlyModCutoff] = lin("Mod cutoff", "%", 0, 1, 0);
    p.params[DlyStereoSpread] = lin("Stereo spread", "%", 0, 1, 1);
    p.params[DlyTone] = lin("Output tone", "%", -1, 1, 0);
    p.params[DlyInput] = lin("Input send", "%", 0, 1, 1);
    p.params[DlyInputPan] = lin("Input pan", "pan", -1, 1, 0);
    p.params[DlyModel] = ch("Delay model", {"Legacy", "Reference"}, 1);
    p.params[DlyType3] = ch("Delay type", {"Fake stereo", "True stereo", "Ping-pong", "Off"}, 1);
    p.params[DlyTicks] = lin("Time (ticks)", "ticks", 1, 768, 192);
    p.params[DlyTimeR] = lin("Time R/L offset", "%", -1, 1, 0);
    p.params[DlySpread3] = lin("Stereo spread", "%", 0, 1, 1);
    p.params[DlyModRate3] = lg("Modulation rate", "Hz", 0.11, 20, 1.1386);
    p.params[DlyFilter3] = ch("Filter mode", {"Low pass", "High pass", "Band pass", "Off"}, 0);
    p.params[DlyCutoff3] = lg("Feedback cutoff", "Hz", 20, 22000, 22000);
    p.params[DlyRate3] = lg("Rate reduction", "Hz", 150, 48000, 48000);
    // Reference defaults (shipped default state): quarter note, 62.5 % feedback, full wet, 500 ms smoothing.
    p.params[DlyFeedback].def = 0.625;
    p.params[DlyWet].def = 1.0;
    p.params[DlyTimeSmooth].def = 500;
    p.params[DlyDistType].def = 2;
    p.editorWidth = 900;
    p.presets = {{"Quarter note", {}},
                 {"Dotted eighth", {{DlyTicks, 144.f}, {DlyFeedback, 0.5f}, {DlyWet, 0.6f}}},
                 {"Slapback", {{DlySync, 0.f}, {DlyTimeMs, 110.f}, {DlyFeedback, 0.1f}, {DlyType3, 0.f}, {DlyWet, 0.5f}}},
                 {"Wide ping-pong", {{DlyType3, 2.f}, {DlyTicks, 96.f}, {DlyTimeR, -0.4f}, {DlyFeedback, 0.6f}, {DlyCutoff3, 4500.f}}},
                 {"Tape echo", {{DlyTicks, 144.f}, {DlyFeedback, 0.7f}, {DlyCutoff3, 3500.f}, {DlyModDepth, 0.35f}, {DlyModRate3, 0.6f}, {DlyDistThresh, -6.f}}},
                 {"Diffused wash", {{DlyFeedback, 0.85f}, {DlyDiffusion, 0.8f}, {DlyDiffSpread, 0.85f}, {DlyCutoff3, 3000.f}, {DlyWet, 0.5f}}},
                 {"Crushed echo", {{DlyBits, 6.f}, {DlyRate3, 6000.f}, {DlyDistType, 2.f}, {DlyDistThresh, -12.f}, {DlyFeedback, 0.75f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.reverb", "Reverb", "Delay & reverb", PluginKind::Effect, {}, false, {}};
    p.params.resize(RvbCount);
    p.params[RvbPreDelay] = lin("Pre-delay", "ms", 0, 384, 0);
    p.params[RvbSize] = lin("Room size", "%", 0, 1, 0.5);
    p.params[RvbDecay] = lin("Decay (legacy)", "%", 0, 1, 0.6);
    p.params[RvbDamp] = lin("Damping (legacy)", "%", 0, 1, 0);
    p.params[RvbLowCut] = lg("Low cut", "Hz", 20, 3000, 75);
    p.params[RvbHighCut] = lg("High cut", "Hz", 500, 22100, 4000);
    p.params[RvbWidth] = lin("Width", "%", -1, 1, 1);
    p.params[RvbEarly] = lin("Early reflections", "%", 0, 1, 1);
    p.params[RvbDry] = lin("Dry", "%", 0, 1, 1);
    p.params[RvbWet] = lin("Wet", "%", 0, 1, 0.54);
    p.params[RvbDecayTime] = lg("Decay time", "s", 0.1, 20, 1.5);
    p.params[RvbDiffusion] = lin("Diffusion", "%", 0, 1, 1);
    p.params[RvbBassMult] = lg("Bass multiplier", "times", 0.2, 3, 1);
    p.params[RvbCrossover] = lg("Crossover", "Hz", 25, 2000, 500);
    p.params[RvbModDepth] = lin("Mod depth", "%", 0, 1, 0);
    p.params[RvbModRate] = lg("Mod speed", "Hz", 0.05, 5, 0.6);
    p.params[RvbInputMode] = ch("Input", {"Mid", "Mid + side"}, 0);
    p.params[RvbPreSync] = tog("Tempo pre-delay", 0);
    p.params[RvbColor] = lin("Color", "%", -1, 1, 0);
    p.params[RvbFreeze] = ch("Freeze", {"Off", "Freeze", "Sustain"}, 0);
    p.params[RvbDampHz] = lg("High damping", "Hz", 500, 22100, 4000);
    p.params[RvbModel] = ch("Model", {"Hall", "Plate"}, 0);
    p.editorWidth = 880;
    p.presets = {{"Room", {}}, {"Small room", {{RvbSize, 0.3f}, {RvbDecayTime, 0.7f}, {RvbEarly, 0.5f}}},
                 {"Hall", {{RvbSize, 0.85f}, {RvbDecayTime, 4.5f}, {RvbPreDelay, 25.f}, {RvbWet, 0.3f}, {RvbModDepth, 0.3f}}},
                 {"Plate", {{RvbSize, 0.55f}, {RvbDecayTime, 3.f}, {RvbDamp, 0.15f}, {RvbEarly, 0.05f}, {RvbHighCut, 14000.f}, {RvbDiffusion, 1.f}}},
                 {"Huge ambience", {{RvbSize, 1.f}, {RvbDecayTime, 12.f}, {RvbPreDelay, 60.f}, {RvbWet, 0.45f}, {RvbDry, 0.7f}, {RvbModDepth, 0.5f}}},
                 {"Dark chamber", {{RvbBassMult, 1.8f}, {RvbColor, -0.6f}, {RvbDecayTime, 2.f}, {RvbInputMode, 1.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.chorus", "Chorus", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(ChoCount);
    p.params[ChoRate] = lin("LFO 1 rate", "Hz", 0, 8, 0.45);
    p.params[ChoDepth] = lin("Depth", "%", 0, 1, 0.5);
    p.params[ChoDelay] = lin("Delay", "ms", 0.05, 30, 15.03);
    p.params[ChoMix] = lin("Mix", "%", 0, 1, 0.5);
    p.params[ChoVoices] = ch("Voices", {"1", "2", "3"}, 0);
    p.params[ChoRate2] = lin("LFO 2 rate", "Hz", 0, 8, 1.25);
    p.params[ChoRate3] = lin("LFO 3 rate", "Hz", 0, 8, 2.45);
    p.params[ChoStereo] = lin("Stereo", "%", 0, 1, 1.0 / 3.0);
    p.params[ChoShape] = lin("LFO shape", "%", 0, 1, 0);
    p.params[ChoCrossMode] = ch("Cross type", {"Off", "Process highs", "Process lows"}, 1);
    p.params[ChoCrossFreq] = lg("Cross cutoff", "Hz", 8, 20000, 320.2);
    p.params[ChoWetOnly] = tog("Wet only", 0);
    p.params[ChoFeedback] = lin("Feedback", "%", -0.9, 0.9, 0);
    {
      std::vector<std::string> waves = {"Sine", "Sine cubed", "Multi-sine", "Multi-sine cubed"};
      p.params[ChoWave1] = ch("LFO 1 wave", waves, 0);
      p.params[ChoWave2] = ch("LFO 2 wave", waves, 0);
      p.params[ChoWave3] = ch("LFO 3 wave", waves, 0);
    }
    p.params[ChoDepthMs] = lin("Sweep depth", "ms", 0, 5, 2.25);
    p.params[ChoClassic] = tog("Reference engine", 1);
    p.editorWidth = 700;
    p.presets = {{"Classic", {}},
                 {"Subtle widen", {{ChoDepthMs, 0.8f}, {ChoStereo, 0.7f}, {ChoDelay, 9.f}}},
                 {"Deep", {{ChoRate, 0.2f}, {ChoRate2, 0.35f}, {ChoRate3, 0.6f}, {ChoDepthMs, 4.6f}, {ChoDelay, 18.f}}},
                 {"Lush ensemble", {{ChoWave1, 2.f}, {ChoWave2, 1.f}, {ChoWave3, 3.f}, {ChoStereo, 0.9f}, {ChoDepthMs, 3.f}}},
                 {"Tight bass", {{ChoCrossMode, 1.f}, {ChoCrossFreq, 250.f}, {ChoDepthMs, 1.5f}}},
                 {"Morph engine", {{ChoClassic, 0.f}, {ChoDelay, 12.f}, {ChoRate, 0.6f}, {ChoRate2, 1.f}, {ChoRate3, 1.6f}, {ChoCrossMode, 0.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.flanger", "Flanger", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(FlgCount);
    p.params[FlgDelay] = lin("Delay", "ms", 0.1, 10, 1.5);
    p.params[FlgDepth] = lin("Depth", "%", 0, 1, 0.6);
    p.params[FlgRate] = lg("Rate", "Hz", 0.02, 5, 0.2);
    p.params[FlgFeedback] = lin("Feedback", "%", -0.95, 0.95, 0.5);
    p.params[FlgMix] = lin("Mix", "%", 0, 1, 0.5);
    p.params[FlgPhase] = lin("Stereo phase", "deg", 0, 360, 90);
    p.params[FlgDamp] = lg("Damp", "Hz", 500, 20000, 20000);
    p.params[FlgShape] = lin("LFO shape", "%", 0, 1, 0);
    p.params[FlgInvertWet] = tog("Invert wet", 0);
    p.params[FlgCross] = lin("Cross", "%", 0, 1, 0);
    p.editorWidth = 700;
    p.presets = {{"Jet", {}}, {"Metallic", {{FlgFeedback, 0.85f}, {FlgDelay, 0.5f}}}, {"Negative", {{FlgFeedback, -0.7f}}},
                 {"Swirl", {{FlgCross, 0.6f}, {FlgPhase, 180.f}, {FlgShape, 1.f}}}, {"Through zero", {{FlgInvertWet, 1.f}, {FlgDelay, 0.1f}, {FlgMix, 0.5f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.phaser", "Phaser", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(PhsCount);
    p.params[PhsStages] = ch("Stages", {"2", "4", "6", "8", "10", "12", "16", "20", "24"}, 2);
    p.params[PhsRate] = lg("Rate", "Hz", 0.02, 8, 0.4);
    p.params[PhsDepth] = lin("Depth", "%", 0, 1, 0.7);
    p.params[PhsCenter] = lg("Centre", "Hz", 100, 5000, 800);
    p.params[PhsFeedback] = lin("Feedback", "%", -0.9, 0.9, 0.4);
    p.params[PhsMix] = lin("Mix", "%", 0, 1, 0.5);
    p.params[PhsRangeMode] = ch("Sweep range", {"Centre + depth", "Min / max"}, 0);
    p.params[PhsMinFreq] = lg("Min freq", "Hz", 20, 20000, 200);
    p.params[PhsMaxFreq] = lg("Max freq", "Hz", 20, 20000, 3000);
    p.params[PhsStereo] = lin("Stereo phase", "deg", 0, 180, 90);
    p.params[PhsOutGain] = lin("Output", "dB", -24, 6, 0);
    p.params[PhsShape] = ch("LFO shape", {"Sine", "Triangle"}, 0);
    p.params[PhsManual] = tog("Manual (no LFO)", 0);
    p.editorWidth = 700;
    p.presets = {{"Classic", {}}, {"Slow sweep", {{PhsRate, 0.08f}, {PhsStages, 4.f}}}, {"Resonant", {{PhsFeedback, 0.8f}, {PhsStages, 5.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.dist", "Distortion", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(DstCount);
    p.params[DstMode] = ch("Mode", {"Soft", "Hard", "Fold", "Tube", "Fast A", "Fast B", "Overdrive", "Rectify"}, 0);
    p.params[DstDrive] = lin("Drive", "dB", 0, 40, 12);
    p.params[DstTone] = lg("Tone", "Hz", 500, 20000, 8000);
    p.params[DstMix] = lin("Mix", "%", 0, 1, 1);
    p.params[DstOutput] = lin("Output", "dB", -24, 6, -6);
    p.params[DstPreBand] = lg("Pre band", "Hz", 100, 8000, 1000);
    p.params[DstPreAmount] = lin("Pre band amount", "dB", 0, 18, 0);
    p.params[DstColor] = lin("Color", "%", -1, 1, 0);
    p.params[DstOversample] = ch("Oversampling", {"1x", "2x", "4x", "8x"}, 0);
    p.params[DstDcRemove] = tog("Remove DC", 0);
    p.params[DstThreshold] = lin("Threshold", "dB", -30, 0, 0);
    p.params[DstExtreme] = tog("x100 drive", 0);
    p.params[DstSoftOut] = tog("Soft clip output", 0);
    p.editorWidth = 720;
    p.presets = {{"Warm", {{DstDrive, 6.f}, {DstMode, 3.f}, {DstOutput, -3.f}}}, {"Crunch", {}},
                 {"Fuzz", {{DstMode, 1.f}, {DstDrive, 30.f}, {DstTone, 4000.f}, {DstOutput, -12.f}}},
                 {"Wavefold", {{DstMode, 2.f}, {DstDrive, 18.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.bitcrush", "Bitcrusher", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(BcCount);
    p.params[BcBits] = lin("Bits", "", 1, 24, 8);
    p.params[BcDownsample] = lin("Downsample", "", 1, 50, 4);
    p.params[BcMix] = lin("Mix", "%", 0, 1, 1);
    p.params[BcFilterOn] = tog("Filter", 0);
    p.params[BcFilterType] = ch("Filter type", {"Low pass", "High pass", "Band pass", "Notch"}, 0);
    p.params[BcFilterFreq] = lg("Filter freq", "Hz", 20, 20000, 2000);
    p.params[BcFilterReso] = lin("Filter reso", "%", 0, 1, 0.3);
    p.params[BcFilterPos] = ch("Filter position", {"Before", "After"}, 1);
    p.params[BcOutput] = lin("Output", "dB", -24, 12, 0);
    p.params[BcPunch] = lin("Punch", "%", 0, 1, 0);
    p.params[BcPreserve] = lg("Preserve", "s", 0.001, 1, 0.001);
    p.params[BcImpact] = lg("Impact", "s", 0.001, 1, 0.01);
    p.params[BcSquarize] = lin("Squarize", "%", 0, 1, 0);
    p.editorWidth = 680;
    p.presets = {{"8-bit", {}}, {"Lo-fi", {{BcBits, 12.f}, {BcDownsample, 2.f}}}, {"Destroyed", {{BcBits, 3.f}, {BcDownsample, 20.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.clipper", "Soft Clipper", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(ClpCount);
    p.params[ClpThreshold] = lin("Threshold", "dB", -42.2, 0, -2.144);
    p.params[ClpPostGain] = lin("Post gain", "dB", -60, 12, 0);
    p.params[ClpCeilingMode] = ch("Ceiling", {"At threshold", "At 0 dBFS"}, 1);
    p.params[ClpOversample] = ch("Oversampling", {"Off", "2x", "4x"}, 0);
    p.params[ClpInput] = lin("Input gain", "dB", 0, 24, 0);
    p.params[ClpMix] = lin("Mix", "%", 0, 1, 1);
    p.presets = {{"Gentle", {}}, {"Loud master", {{ClpInput, 4.f}, {ClpThreshold, -3.f}, {ClpCeilingMode, 1.f}, {ClpOversample, 2.f}}},
                 {"Drum smash", {{ClpInput, 9.f}, {ClpThreshold, -9.f}, {ClpPostGain, 3.f}, {ClpOversample, 1.f}, {ClpCeilingMode, 0.f}}}};
    p.editorWidth = 560;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.stereo", "Stereo Shaper", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(StCount);
    p.params[StWidth] = lin("Width", "%", 0, 2, 1);
    p.params[StMid] = lin("Mid level", "dB", -24, 12, 0);
    p.params[StSide] = lin("Side level", "dB", -24, 12, 0);
    p.params[StHaas] = lin("Haas delay", "ms", -30, 30, 0);
    p.params[StPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[StLL] = lin("L to L", "%", -2, 2, 1);
    p.params[StRR] = lin("R to R", "%", -2, 2, 1);
    p.params[StRL] = lin("R to L", "%", -2, 2, 0);
    p.params[StLR] = lin("L to R", "%", -2, 2, 0);
    p.params[StInvert] = ch("Invert", {"None", "Left", "Right", "Both"}, 0);
    p.params[StDephase] = lin("Dephase", "%", -1, 1, 0);
    p.params[StFxPos] = ch("Delay position", {"Before matrix", "After matrix"}, 1);
    p.params[StMonoBelow] = lg("Mono below", "Hz", 10, 1000, 10);
    p.params[StVolume] = lin("Volume", "dB", -24, 6, 0);
    p.params[StSep] = lin("Separation", "%", -1, 1, 0);
    p.params[StVolK] = lin("Volume", "%", 0, 1.25, 1);
    p.params[StPhaseK] = lin("Phase offset", "%", -1, 1, 0);
    p.params[StClassic] = tog("Reference engine", 1);
    p.editorWidth = 700;
    p.presets = {{"Neutral", {}}, {"Wide", {{StSep, -0.8f}}}, {"Narrow", {{StSep, 0.9f}}}, {"Haas widen", {{StPhaseK, 0.3f}}},
                 {"Swap sides", {{StClassic, 0.f}, {StLL, 0.f}, {StRR, 0.f}, {StRL, 1.f}, {StLR, 1.f}}},
                 {"Tight low end", {{StClassic, 0.f}, {StWidth, 1.3f}, {StMonoBelow, 150.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.transient", "Transient Shaper", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(TrCount);
    // κ2: reference ranges — attack / release -100..+100 %, gain ±9 dB, split 5 Hz-1 kHz (x² law)
    p.params[TrAttack] = lin("Attack", "%", -1, 1, 0);
    p.params[TrSustain] = lin("Release", "%", -1, 1, 0);
    p.params[TrOutput] = lin("Gain", "dB", -18, 18, 0);
    p.params[TrAttackSpeed] = ch("Attack shape", {"Sharp", "Medium", "Slow"}, 1);
    p.params[TrReleaseSpeed] = ch("Release shape", {"Sharp", "Medium", "Slow"}, 1);
    p.params[TrSplit] = lg("Split frequency", "Hz", 5, 20000, 253.75);
    p.params[TrBalance] = lin("Split balance", "%", -1, 1, 0);
    p.params[TrDrive] = lin("Drive", "%", 0, 1, 0);
    p.params[TrMix] = lin("Mix", "%", 0, 1, 1);
    p.params[TrEnabled] = tog("Effect", 1);
    p.editorWidth = 720;
    p.presets = {{"Neutral", {}}, {"More snap", {{TrAttack, 0.6f}}}, {"Tighten", {{TrSustain, -0.6f}}}, {"Soften", {{TrAttack, -0.6f}, {TrSustain, 0.3f}}},
                 {"Punchy highs", {{TrAttack, 0.8f}, {TrAttackSpeed, 0.f}, {TrSplit, 400.f}, {TrBalance, 0.5f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.multiband", "Multiband Compressor", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(MbCount);
    // κ2: reference engine — band edges 20 Hz-20 kHz (defaults 300 Hz / 9.23 kHz),
    // threshold -60..0 dB, ratio 1..inf (>= 1000 = inf), band gain up to +16.9 dB,
    // attack 0.1-100 ms, release 10-1000 ms, knee hardness 0..1.
    p.params[MbLowXover] = lg("Low band edge", "Hz", 20, 20000, 300.3);
    p.params[MbHighXover] = lg("High band edge", "Hz", 20, 20000, 9236.7);
    const char* bn[3] = {"Low", "Mid", "High"};
    for (int b = 0; b < 3; ++b) {
      std::string n = bn[b];
      p.params[MbLowThresh + b * 3] = lin((n + " threshold").c_str(), "dB", -60, 0, b == 1 ? -15.25 : -18);
      p.params[MbLowRatio + b * 3] = lg((n + " ratio").c_str(), "x", 1, 1000, 2);
      p.params[MbLowGain + b * 3] = lin((n + " gain").c_str(), "dB", -24, 16.9, 0);
      p.params[MbLowTime + b] = lg((n + " speed (legacy)").c_str(), "times", 0.25, 4, 1);
      p.params[MbLowKnee + b] = lin((n + " knee (legacy)").c_str(), "dB", 0, 24, 6);
      p.params[MbLowState + b] = ch((n + " state").c_str(), {"Compress", "Bypass", "Mute"}, 0);
      p.params[MbLowAtk + b] = lg((n + " attack").c_str(), "s", 0.0001, 0.1, 0.01);
      p.params[MbLowRel + b] = lg((n + " release").c_str(), "s", 0.01, 1, 0.1);
      p.params[MbLowHard + b] = lin((n + " knee").c_str(), "%", 0, 1, 0.8);
    }
    p.params[MbAttack] = lg("Attack (legacy)", "s", 0.0005, 0.2, 0.01);
    p.params[MbRelease] = lg("Release (legacy)", "s", 0.01, 2, 0.15);
    p.params[MbLimiter] = tog("Output limiter", 0);
    p.params[MbOutput] = lin("Output", "dB", -24, 9.54, 0);
    p.params[MbMidLowEdge] = lg("Mid band low edge", "Hz", 20, 20000, 300.3);
    p.params[MbMidHighEdge] = lg("Mid band high edge", "Hz", 20, 20000, 9236.7);
    p.params[MbEdgeMode] = ch("Mid band edges", {"Linked", "Independent"}, 0);
    p.params[MbFilterType] = ch("Band filters", {"Butterworth 24 dB", "Linear phase", "Linkwitz-Riley"}, 0);
    p.editorWidth = 1040;
    p.presets = {{"Master glue", {}}, {"Loudness", {{MbLowThresh, -24.f}, {MbMidThresh, -22.f}, {MbHighThresh, -20.f}, {MbLowGain, 4.f}, {MbMidGain, 4.f}, {MbHighGain, 5.f}}},
                 {"De-ess highs", {{MbHighXover, 5000.f}, {MbHighThresh, -30.f}, {MbHighRatio, 8.f}, {MbLowThresh, 0.f}, {MbMidThresh, 0.f}}},
                 {"Flat crossover", {{MbFilterType, 2.f}, {MbLowXover, 200.f}, {MbHighXover, 3000.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.analyzer", "Analyzer", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(AnzCount);
    p.params[AnzMode] = ch("View", {"Spectrum", "Oscilloscope", "Both", "Sonogram", "Meter", "Stereo"}, 2);
    p.params[AnzGain] = lin("Display gain", "dB", -24, 24, 0);
    p.params[AnzStereo] = tog("Stereo", 0);
    p.params[AnzPeaks] = tog("Peak hold", 1);
    p.params[AnzRange] = lin("dB range", "dB", 48, 144, 96);
    p.params[AnzSlope] = lin("Tilt", "dB", 0, 6, 0);
    p.params[AnzSmoothing] = ch("Smoothing", {"Fast", "Medium", "Slow"}, 1);
    p.params[AnzHold] = tog("Freeze", 0);
    p.editorWidth = 760;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.geq", "Graphic EQ", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(GeqCount);
    static const char* names[kGeqBands] = {"63 Hz", "160 Hz", "400 Hz", "1 kHz", "2.5 kHz", "6.3 kHz", "16 kHz"};
    for (int b = 0; b < kGeqBands; ++b) p.params[b] = lin(names[b], "dB", -18, 18, 0);
    p.params[GeqOutput] = lin("Output", "dB", -18, 18, 0);
    p.params[GeqLayout] = ch("Band layout", {"Wide", "Classic"}, 0);
    p.params[GeqBandwidth] = lg("Bandwidth", "oct", 0.3, 3, 1.27);
    p.params[GeqMix] = lin("Mix", "%", -1, 1, 1);
    p.editorWidth = 640;
    p.presets = {{"Flat", {}}, {"Smile", {{0, 4.f}, {1, 2.f}, {3, -1.5f}, {5, 2.f}, {6, 4.f}}}, {"Warm", {{0, 2.f}, {1, 2.f}, {5, -2.f}, {6, -3.f}}},
                 {"Radio", {{0, -18.f}, {1, -8.f}, {3, 5.f}, {4, 3.f}, {6, -18.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.waveshaper", "Waveshaper", "Distortion", PluginKind::Effect, {}, false, {}};
    p.params.resize(WsCount);
    for (int k = 0; k < kShapePoints; ++k) {
      double x = -1.0 + 2.0 * k / (kShapePoints - 1);
      p.params[k] = lin(("Point " + std::to_string(k + 1)).c_str(), "", -1, 1, x);
    }
    p.params[WsPreGain] = lin("Pre gain", "dB", -12, 36, 6);
    p.params[WsPostGain] = lin("Post gain", "dB", -36, 12, -3);
    p.params[WsMix] = lin("Mix", "%", 0, 1, 1);
    p.params[WsBipolar] = tog("Mirror negative half", 1);
    p.params[WsCenter] = tog("Remove DC", 0);
    p.params[WsOversample] = ch("Oversampling", {"Off", "2x", "4x", "8x", "16x"}, 0);
    p.params[WsInterp] = ch("Curve", {"Linear", "Smooth", "Steps"}, 0);
    p.params[WsPreAmp] = lin("Pre amp", "%", 0.5, 1.5, 1);  // knob position (reference: 64..192 / 128)
    p.params[WsPost] = lin("Post gain", "%", 0, 1, 1);  // knob position (reference: 0..128 / 128)
    p.params[WsClassic] = tog("Reference gain staging", 1);
    p.params[WsHq] = tog("HQ (no truncation)", 0);
    p.params[WsSmooth] = tog("Smooth modulation", 0);
    p.editorWidth = 520;
    auto curve = [](auto f) {
      std::vector<std::pair<int, float>> v;
      for (int k = 0; k < kShapePoints; ++k) v.push_back({k, static_cast<float>(f(-1.0 + 2.0 * k / (kShapePoints - 1)))});
      return v;
    };
    p.presets = {{"Linear", {}}, {"Soft saturate", curve([](double x) { return std::tanh(2 * x) / std::tanh(2.0); })},
                 {"Hard clip", curve([](double x) { return std::clamp(1.8 * x, -1.0, 1.0); })},
                 {"Fold", curve([](double x) { return std::sin(3.14159 * x * 1.5); })},
                 {"Steps", curve([](double x) { return std::round(x * 3) / 3; })}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.autopan", "Auto Pan", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(ApCount);
    p.params[ApShape] = ch("Shape", {"Sine", "Triangle", "Square", "Saw up", "Saw down"}, 0);
    p.params[ApSync] = tog("Tempo sync", 1);
    p.params[ApRate] = ch("Rate", {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16"}, 3);
    p.params[ApRateHz] = lg("Rate (free)", "Hz", 0.05, 20, 1);
    p.params[ApPan] = lin("Pan amount", "%", 0, 1, 0.7);
    p.params[ApVolume] = lin("Volume amount", "%", 0, 1, 0);
    p.params[ApPhase] = lin("Phase", "%", 0, 1, 0);
    p.params[ApStaticPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[ApStaticVol] = lin("Volume", "%", 0, 1.25, 1);
    p.params[ApTarget] = ch("LFO target", {"Off", "Pan", "Volume"}, 0);
    p.params[ApAmount] = lin("LFO amount", "%", 0, 1, 0.5);
    p.params[ApSpeedK] = lin("LFO speed", "%", 0, 55000.0 / 65536.0, 27500.0 / 65536.0);
    p.params[ApClassic] = tog("Reference engine", 1);
    p.editorWidth = 700;
    p.presets = {{"Static", {}},
                 {"Slow pan", {{ApTarget, 1.f}, {ApAmount, 0.6f}, {ApSpeedK, 0.35f}}},
                 {"Fast tremolo", {{ApTarget, 2.f}, {ApAmount, 1.f}, {ApSpeedK, 0.62f}}},
                 {"Square ping-pong", {{ApTarget, 1.f}, {ApShape, 2.f}, {ApAmount, 0.8f}, {ApSpeedK, 0.45f}}},
                 {"Synced pan (morph engine)", {{ApClassic, 0.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.shaper", "Shaper", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(ShCount);
    for (int k = 0; k < kShaperPoints; ++k) {
      p.params[ShVol + k] = lin(("Volume " + std::to_string(k + 1)).c_str(), "%", 0, 1, 1);
      p.params[ShCut + k] = lin(("Cutoff " + std::to_string(k + 1)).c_str(), "%", 0, 1, 1);
    }
    p.params[ShLength] = ch("Length", {"1/4", "1/2", "1 bar", "2 bars", "4 bars"}, 2);
    p.params[ShFilter] = ch("Filter", {"Off", "Low pass", "Band pass", "High pass", "Notch", "Low shelf", "Peak", "High shelf", "All pass"}, 0);
    p.params[ShCutMin] = lg("Cutoff min", "Hz", 20, 20000, 200);
    p.params[ShCutMax] = lg("Cutoff max", "Hz", 20, 20000, 12000);
    p.params[ShReso] = lin("Resonance", "%", 0, 1, 0.3);
    p.params[ShSmooth] = lg("Smoothing", "ms", 0.5, 100, 4);
    p.params[ShMix] = lin("Mix", "%", 0, 1, 1);
    p.params[ShTension] = lin("Curve", "%", -1, 1, 0);
    p.params[ShCutDepth] = lin("Env amount", "%", -1, 1, 1);
    for (int k = 0; k < kShaperPoints; ++k) p.params[ShPan + k] = lin(("Pan " + std::to_string(k + 1)).c_str(), "pan", -1, 1, 0);
    p.params[ShDrive] = lin("Drive", "dB", 0, 24, 0);
    p.params[ShDriveMix] = lin("Drive mix", "%", 0, 1, 0);
    p.params[ShSlope] = ch("Slope", {"12 dB", "24 dB", "36 dB"}, 0);
    p.params[ShShelfGain] = lin("Filter gain", "dB", -24, 24, 0);
    p.params[ShFollow] = lin("Follower", "%", -1, 1, 0);
    p.params[ShFollowAtt] = lg("Follower attack", "s", 0.001, 0.5, 0.01);
    p.params[ShFollowRel] = lg("Follower release", "s", 0.01, 2, 0.15);
    p.params[ShLfoAmt] = lin("LFO amount", "%", -1, 1, 0);
    p.params[ShLfoRate] = ch("LFO rate", {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"}, 2);
    p.params[ShLfoShape] = ch("LFO shape", {"Sine", "Triangle", "Saw", "Square"}, 0);
    p.params[ShInput] = lin("Input", "dB", -24, 12, 0);
    p.params[ShOutput] = lin("Output", "dB", -24, 6, 0);
    p.editorWidth = 860;
    std::vector<std::pair<int, float>> pump, gate8;
    for (int k = 0; k < kShaperPoints; ++k) {
      float ph = static_cast<float>(k % 4) / 4.0f;
      pump.push_back({ShVol + k, std::min(1.0f, 0.1f + ph * 1.4f)});
      gate8.push_back({ShVol + k, k % 2 ? 0.f : 1.f});
    }
    p.presets = {{"Off", {}}, {"Sidechain pump", pump}, {"Gate 1/8", gate8}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.timegate", "Time Gate", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(TgCount);
    p.params[TgMode] = ch("Time effect", {"Off", "Repeat 1/4", "Repeat 1/8", "Repeat 1/16", "Repeat 1/32", "Half speed", "Tape stop", "Reverse"}, 0);
    p.params[TgGateOn] = tog("Gate", 0);
    for (int k = 0; k < kGateSteps; ++k) p.params[TgGate + k] = tog(("Gate step " + std::to_string(k + 1)).c_str(), 1);
    p.params[TgSmooth] = lg("Smoothing", "ms", 0.5, 50, 3);
    p.params[TgMix] = lin("Mix", "%", 0, 1, 1);
    {
      std::vector<std::string> slots = {"Off"};
      for (int k = 1; k <= kTgSlots; ++k) slots.push_back("Slot " + std::to_string(k));
      p.params[TgTimeSlot] = ch("Time slot", slots, 0);
      p.params[TgVolSlot] = ch("Volume slot", slots, 0);
    }
    p.params[TgTimeMix] = lin("Time mix", "%", 0, 1, 1);
    p.params[TgVolMix] = lin("Volume mix", "%", 0, 1, 1);
    p.params[TgVolAttack] = lg("Volume attack", "s", 0.0001, 0.5, 0.002);
    p.params[TgVolRelease] = lg("Volume release", "s", 0.0001, 1.0, 0.0428);
    p.params[TgTension] = lin("Tension", "%", -1, 1, 0);
    p.params[TgHold] = tog("Hold", 0);
    p.params[TgTrigSync] = ch("Trigger sync", {"Off", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats"}, 0);
    p.params[TgDeclick] = ch("Declick", {"Off", "Low", "High"}, 1);
    p.params[TgFreeRun] = tog("Run when stopped", 0);
    p.params[TgScratch] = lin("Scratch", "beats", -8, 8, 0);
    p.params[TgDcRemove] = tog("Remove DC offset", 0);
    p.params[TgCompensate] = tog("Attack compensation", 0);
    p.params[TgHq] = tog("HQ resampling", 1);
    p.params[TgMidiTrigger] = tog("Trigger slots from notes", 1);
    p.editorWidth = 1000;
    std::vector<std::pair<int, float>> trance;
    for (int k = 0; k < kGateSteps; ++k) trance.push_back({TgGate + k, (k % 4 == 1 || k % 4 == 3) ? 0.f : 1.f});
    trance.push_back({TgGateOn, 1.f});
    p.presets = {{"Clean", {}}, {"Stutter 1/16", {{TgMode, 3.f}}}, {"Half time", {{TgMode, 5.f}}}, {"Tape stop", {{TgMode, 6.f}}},
                 {"Trance gate", trance}, {"Slot: half speed + chop", {{TgTimeSlot, 2.f}, {TgVolSlot, 3.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.maximizer", "Maximizer", "Dynamics", PluginKind::Effect, {}, false, {}};
    p.params.resize(MxCount);
    p.params[MxLowFreq] = lg("Low / mid", "Hz", 40, 1000, 140);
    p.params[MxHighFreq] = lg("Mid / high", "Hz", 1000, 16000, 4000);
    p.params[MxLowGain] = lin("Low", "dB", -12, 12, 0);
    p.params[MxMidGain] = lin("Mid", "dB", -12, 12, 0);
    p.params[MxHighGain] = lin("High", "dB", -12, 12, 0);
    p.params[MxDrive] = lin("Drive", "dB", 0, 24, 4);
    p.params[MxRelease] = lg("Release", "s", 0.01, 1, 0.12);
    p.params[MxCeiling] = lin("Ceiling", "dB", -12, 0, -0.3);
    p.params[MxLowCut] = lg("Low cut", "Hz", 10, 200, 20);
    {
      const char* bn[3] = {"Low", "Mid", "High"};
      for (int b = 0; b < 3; ++b) {
        std::string n = bn[b];
        p.params[MxLowThresh + b] = lin((n + " threshold").c_str(), "dB", -40, 0, 0);
        p.params[MxLowRatio + b] = lg((n + " ratio").c_str(), "x", 1, 20, 3);
        p.params[MxLowAttack + b] = lg((n + " attack").c_str(), "s", 0.0001, 0.2, 0.003);
        p.params[MxLowRel + b] = lg((n + " release").c_str(), "s", 0.01, 2, 0.15);
        p.params[MxLowMode + b] = ch((n + " mode").c_str(), {"Compress", "Off", "Mute"}, 0);
        p.params[MxLowWidth + b] = lin((n + " width").c_str(), "%", 0, 2, 1);
        p.params[MxLowSat + b] = lin((n + " saturation").c_str(), "%", 0, 1, 0);
      }
    }
    p.params[MxSlope] = ch("Crossover slope", {"12 dB/oct", "24 dB/oct"}, 1);
    p.params[MxLookahead] = lin("Look-ahead", "ms", 0, 10, 0);
    p.params[MxBandMix] = lin("Band mix", "%", 0, 1, 1);
    p.params[MxAttack] = lg("Master attack", "s", 0.0001, 0.05, 0.001);
    p.params[MxRelease2] = lin("Release 2", "s", 0, 3, 0);
    p.params[MxRelCurve] = ch("Release curve", {"1", "2", "3", "4", "5", "6", "7", "8"}, 0);
    p.params[MxWidth] = lin("Master width", "%", 0, 2, 1);
    p.params[MxAmount] = lin("Amount", "%", 0, 1, 1);
    p.params[MxSolo] = ch("Solo band", {"None", "Low", "Mid", "High"}, 0);
    p.params[MxStage] = ch("Edit stage", {"Low", "Mid", "High", "Master"}, 3);
    p.editorWidth = 860;
    p.presets = {{"Master", {}}, {"Loud", {{MxDrive, 9.f}, {MxLowGain, 1.5f}, {MxHighGain, 1.5f}}},
                 {"Gentle", {{MxDrive, 2.f}, {MxRelease, 0.3f}}}, {"Bass heavy", {{MxLowGain, 4.f}, {MxDrive, 6.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.freqshift", "Freq Shifter", "Modulation", PluginKind::Effect, {}, false, {}};
    p.params.resize(FsCount);
    p.params[FsShift] = lin("Shift", "Hz", -1000, 1000, 50);
    p.params[FsFeedback] = lin("Feedback", "%", -0.95, 0.95, 0);
    p.params[FsStereo] = lin("Stereo", "%", 0, 1, 0.25);
    p.params[FsMix] = lin("Mix", "%", 0, 1, 0.5);
    p.params[FsType] = ch("Type", {"Shifter", "Shifter HQ", "Ring mod"}, 0);
    p.params[FsRange] = ch("Range", {"Fine", "Wide", "Tempo"}, 0);
    p.params[FsShiftWide] = lin("Shift (wide)", "Hz", -20000, 20000, 0);
    p.params[FsShiftBeats] = lin("Shift (tempo)", "", -16, 16, 1);
    p.params[FsWidth] = lin("Stereo width", "%", 0, 1, 1);
    p.params[FsStartPhase] = lin("Start phase", "deg", 0, 360, 0);
    p.params[FsShapeL] = lin("Shape L", "%", 0, 1, 1);
    p.params[FsShapeR] = lin("Shape R", "%", 0, 1, 1);
    p.params[FsModel] = ch("Engine", {"Legacy", "Reference"}, 1);
    p.params[FsFreqPos] = lin("Frequency", "%", 0, 1, 0.9088);  // ≈ +50 Hz in the narrow range
    p.params[FsMixRef] = lin("Dry / wet", "%", 0, 1, 0.5);
    p.params[FsLrPhase] = lin("L/R phase", "%", 0, 1, 0);
    p.params[FsFeedbackRef] = lin("Feedback", "%", 0, 1, 0.5);
    p.params[FsStartRef] = lin("Start phase", "%", 0, 1, 0.5);
    p.legacyModelParam = FsModel;
    p.editorWidth = 760;
    p.presets = {{"Subtle", {}},
                 {"Barber pole", {{FsFreqPos, 0.5728f}, {FsFeedbackRef, 0.9f}, {FsMixRef, 1.f}}},
                 {"Metal", {{FsRange, 1.f}, {FsFreqPos, 0.7454f}, {FsMixRef, 1.f}}},
                 {"Wide ring", {{FsType, 2.f}, {FsRange, 1.f}, {FsFreqPos, 0.55f}, {FsMixRef, 0.75f}, {FsLrPhase, 1.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.lowlift", "Low Lifter", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(LlCount);
    p.params[LlCrossover] = lg("Crossover", "Hz", 40, 400, 120);
    p.params[LlLowGain] = lin("Low gain", "dB", -12, 12, 3);
    p.params[LlHarmonics] = lin("Harmonics", "%", 0, 1, 0.3);
    p.params[LlHighGain] = lin("High gain", "dB", -12, 12, 0);
    p.params[LlLowCut] = lg("Low cut", "Hz", 10, 80, 25);
    p.params[LlMix] = lin("Mix", "%", 0, 1, 1);
    p.params[LlDrive] = lin("Pre-shaping boost", "dB", 0, 24, 0);
    p.params[LlSlope] = lin("Harmonic slope", "%", 0, 1, 0);
    p.params[LlInvertHarm] = tog("Invert harmonics", 0);
    p.params[LlDiffuse] = lin("Diffuser", "%", 0, 1, 0);
    p.params[LlDiffuseAmt] = lin("Diffuser coef", "%", 0, 1, 0.5);
    p.params[LlLowOn] = tog("Low band", 1);
    p.params[LlHighOn] = tog("High band", 1);
    p.params[LlMultiband] = tog("Multiband", 1);
    p.params[LlBoost] = lin("Bass boost", "%", 0, 1, 0);
    p.params[LlBoostFreq] = lg("Boost freq", "Hz", 20, 250, 60);
    p.editorWidth = 760;
    p.presets = {{"Lift", {}}, {"Sub harmonics", {{LlHarmonics, 0.7f}, {LlLowGain, 2.f}}}, {"Tight", {{LlLowCut, 45.f}, {LlLowGain, 1.f}}}};
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.tuner", "Tuner", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(TnCount);
    p.params[TnReference] = lin("Reference A4", "Hz", 410, 470, 440);
    p.params[TnReactivity] = lin("Reactivity", "%", 0, 1, 0.5);
    p.params[TnBassMode] = tog("Bass mode", 0);
    p.params[TnDisplay] = ch("Display", {"Needle", "Strip", "Ring"}, 0);
    p.editorWidth = 520;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.utility", "Utility", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(UtlCount);
    p.params[UtlGain] = lin("Gain", "dB", -36, 24, 0);
    p.params[UtlPan] = lin("Pan", "pan", -1, 1, 0);
    p.params[UtlWidth] = lin("Width", "%", 0, 2, 1);
    p.params[UtlInvert] = tog("Invert polarity", 0);
    p.params[UtlSwap] = tog("Swap L/R", 0);
    p.params[UtlInvertMode] = ch("Invert which", {"Both", "Left", "Right"}, 0);
    p.params[UtlMute] = tog("Mute", 0);
    p.params[UtlMuteChannel] = ch("Mute which", {"Both", "Left", "Right"}, 0);
    p.params[UtlDcRemove] = tog("Remove DC", 0);
    p.params[UtlPanLaw] = ch("Pan law", {"Balance", "Constant power", "Folded balance"}, 2);
    p.params[UtlMono] = tog("Mono", 0);
    p.presets = {{"Neutral", {}}, {"Mono check", {{UtlMono, 1.f}}}, {"Flip left", {{UtlInvert, 1.f}, {UtlInvertMode, 1.f}}},
                 {"Clean DC", {{UtlDcRemove, 1.f}}}};
    p.editorWidth = 640;
    v.push_back(p);
  }
  return v;
}

}  // namespace

const std::vector<PluginInfo>& pluginCatalog() {
  static const std::vector<PluginInfo> cat = [] {
    std::vector<PluginInfo> v = build();
    packs::appendInfos(v);
    // One spelling per browser category across all packs.
    for (auto& p : v)
      if (p.category == "Samplers") p.category = "Sampler";
    std::stable_partition(v.begin(), v.end(), [](const PluginInfo& p) { return p.kind == PluginKind::Generator; });
    return v;
  }();
  return cat;
}

namespace {
// Append-only table of runtime-registered plugins. Writers serialize on the
// mutex; readers only load the published count and the immutable entries.
struct DynamicTable {
  static constexpr size_t kCapacity = 16384;
  std::mutex writeLock;
  std::deque<std::unique_ptr<PluginInfo>> owned;
  std::array<std::atomic<const PluginInfo*>, kCapacity> entries{};
  std::atomic<size_t> count{0};
};
DynamicTable& dynamicTable() {
  static DynamicTable* t = new DynamicTable;  // never destroyed: entries outlive static destructors
  return *t;
}
}  // namespace

bool isHostedType(const std::string& type) {
  return type.compare(0, 5, "vst3:") == 0 || type.compare(0, 5, "clap:") == 0 || type.compare(0, 5, "vst2:") == 0 ||
         type.compare(0, 3, "au:") == 0;
}

void registerDynamicPlugin(PluginInfo info) {
  DynamicTable& t = dynamicTable();
  std::lock_guard<std::mutex> lock(t.writeLock);
  size_t n = t.count.load(std::memory_order_relaxed);
  if (n >= DynamicTable::kCapacity) return;
  t.owned.push_back(std::make_unique<PluginInfo>(std::move(info)));
  t.entries[n].store(t.owned.back().get(), std::memory_order_relaxed);
  t.count.store(n + 1, std::memory_order_release);
}

std::vector<const PluginInfo*> dynamicPlugins() {
  DynamicTable& t = dynamicTable();
  size_t n = t.count.load(std::memory_order_acquire);
  std::vector<const PluginInfo*> out;
  for (size_t i = 0; i < n; ++i) {
    const PluginInfo* p = t.entries[i].load(std::memory_order_relaxed);
    auto same = std::find_if(out.begin(), out.end(), [&](const PluginInfo* q) { return q->type == p->type; });
    if (same != out.end())
      *same = p;  // superseded by a later registration
    else
      out.push_back(p);
  }
  return out;
}

const PluginInfo* findPlugin(const std::string& type) {
  for (const auto& p : pluginCatalog())
    if (p.type == type) return &p;
  if (!isHostedType(type)) return nullptr;
  DynamicTable& t = dynamicTable();
  for (size_t i = t.count.load(std::memory_order_acquire); i-- > 0;) {
    const PluginInfo* p = t.entries[i].load(std::memory_order_relaxed);
    if (p->type == type) return p;
  }
  return nullptr;
}

PluginState defaultPluginState(const std::string& type) {
  PluginState s;
  s.type = type;
  if (const PluginInfo* p = findPlugin(type))
    for (const auto& d : p->params) s.params.push_back(static_cast<float>(d.def));
  return s;
}

PluginState presetState(const PluginInfo& info, size_t preset) {
  PluginState s = defaultPluginState(info.type);
  if (preset < info.presets.size())
    for (const auto& [k, v] : info.presets[preset].values)
      if (k >= 0 && static_cast<size_t>(k) < s.params.size()) s.params[static_cast<size_t>(k)] = static_cast<float>(info.params[static_cast<size_t>(k)].clamp(v));
  return s;
}

void normalizePluginState(PluginState& s) {
  const PluginInfo* p = findPlugin(s.type);
  if (!p) return;
  if (s.params.size() > p->params.size()) s.params.resize(p->params.size());
  const size_t oldSize = s.params.size();
  for (size_t i = s.params.size(); i < p->params.size(); ++i) s.params.push_back(static_cast<float>(p->params[i].def));
  if (p->legacyModelParam >= 0 && oldSize > 0 && oldSize <= static_cast<size_t>(p->legacyModelParam))
    s.params[static_cast<size_t>(p->legacyModelParam)] = 0;  // pre-model state: keep the legacy engine
  // Migrations for states saved before 0.5 appended parameters.
  if (s.type == "gl.reverb" && oldSize > static_cast<size_t>(pp::RvbDecay) && oldSize <= static_cast<size_t>(pp::RvbDecayTime)) {
    // Old normalised decay → seconds (the curve the 0.4 engine produced).
    double d = s.params[pp::RvbDecay], size = s.params[pp::RvbSize];
    s.params[pp::RvbDecayTime] = static_cast<float>(0.3 + d * d * 7.0 * (0.5 + size * 0.5));
  }
  if (s.type == "gl.autopan" && oldSize > static_cast<size_t>(pp::ApPhase) && oldSize <= static_cast<size_t>(pp::ApClassic))
    s.params[static_cast<size_t>(pp::ApClassic)] = 0;  // pre-κ3 auto pan: synced pan / volume depths
  if (s.type == "gl.stereo" && oldSize > 0 && oldSize <= static_cast<size_t>(pp::StClassic))
    s.params[static_cast<size_t>(pp::StClassic)] = 0;  // pre-κ3 stereo shaper: width / mid-side engine
  if (s.type == "gl.stereomatrix" && oldSize > 0 && oldSize <= 9)  // pre-κ3 stereo matrix: linear gains (SmClassic = 9)
    s.params[9] = 0;
  if (s.type == "gl.swirl" && oldSize > 0 && oldSize <= 15)  // pre-κ3 swirl: legacy engine (SwModel = 15)
    s.params[15] = 0;
  if (s.type == "gl.fastdist" && oldSize > 0 && oldSize <= 9)  // pre-κ3 clip dist: keep the clip / fold engine (FdClassic = 9)
    s.params[9] = 0;
  if (s.type == "gl.waveshaper" && oldSize > static_cast<size_t>(pp::WsInterp) && oldSize <= static_cast<size_t>(pp::WsClassic))
    s.params[static_cast<size_t>(pp::WsClassic)] = 0;  // pre-κ3 waveshaper: dB pre / post gains
  if (s.type == "gl.chorus" && oldSize > static_cast<size_t>(pp::ChoMix) && oldSize <= static_cast<size_t>(pp::ChoClassic))
    s.params[static_cast<size_t>(pp::ChoClassic)] = 0;  // pre-κ3 chorus: keep its engine
  if (s.type == "gl.delay" && oldSize > static_cast<size_t>(pp::DlyWet) && oldSize <= static_cast<size_t>(pp::DlyModel))
    s.params[static_cast<size_t>(pp::DlyModel)] = 0;  // pre-κ1 delay: keep its sound (legacy model)
  if (s.type == "gl.scope" && oldSize > 0 && oldSize <= 11 && s.params.size() > 32)
    s.params[32] = 0;  // pre-J-1 scope (11 params): legacy display engine (utility::ScEngine)
  if ((s.type == "gl.bbdchorus" || s.type == "gl.vphaser" || s.type == "gl.multidelay") && oldSize > 0)
    for (size_t i = oldSize; i < p->params.size(); ++i)  // pre-J-1 state: appended "Legacy/Reference" model → legacy
      if (p->params[i].choices.size() == 2 && p->params[i].choices[0] == "Legacy") s.params[i] = 0;
  if (s.type == "gl.eq" && oldSize > static_cast<size_t>(pp::EqMain) && oldSize <= static_cast<size_t>(pp::EqModel)) {
    // Pre-κ1 EQ: Q / slope / order → the reference band model's width + order.
    using namespace pp;
    for (int b = 0; b < kEqBands; ++b) {
      int type = static_cast<int>(std::lround(s.params[static_cast<size_t>(eqParam(b, EqType))]));
      double q = std::max(0.1f, s.params[static_cast<size_t>(eqParam(b, EqQ))]);
      double w = 0.4;
      int ord = EqOrd2;
      if (type == EqLowCut || type == EqHighCut) {
        // Q = 0.7071 (0.1 + 9.9 (1 - w)^3)
        w = 1.0 - std::cbrt(std::max(0.0, (q / 0.70710678 - 0.1) / 9.9));
        int slope = static_cast<int>(std::lround(s.params[static_cast<size_t>(eqParam(b, EqSlope))]));
        static const int bySlope[5] = {EqOrd2, EqSteep4, EqSteep6, EqSteep8, EqOrd2};
        ord = bySlope[std::clamp(slope, 0, 4)];
      } else if (type == EqLowShelf || type == EqHighShelf) {
        // Shelf slope S = 1/3 + 1.7193 (1 - w); Q 0.707 ≙ S 1.
        double S = std::clamp(q / 0.70710678, 1.0 / 3.0, 2.05);
        w = 1.0 - (S - 1.0 / 3.0) / 1.7192982456;
      } else {
        // Bandwidth in octaves = 0.1 + 4.9 w (band pass: × 0.708, notch: ÷ 0.708).
        double bw = 2.0 / std::log(2.0) * std::asinh(1.0 / (2.0 * q));
        if (type == EqBandPass) bw /= 0.70794578;
        if (type == EqNotch) bw *= 0.70794578;
        w = (bw - 0.1) / 4.9;
      }
      if (type != EqLowCut && type != EqHighCut) {
        int o = static_cast<int>(std::lround(s.params[static_cast<size_t>(eqExt(b, EqOrder))]));
        static const int byOrder[5] = {EqGentle4, EqOrd2, EqSteep4, EqSteep6, EqSteep8};
        ord = byOrder[std::clamp(o, 0, 4)];
      }
      s.params[static_cast<size_t>(eqFl(b, EqWidth))] = static_cast<float>(std::clamp(w, 0.0, 1.0));
      s.params[static_cast<size_t>(eqFl(b, EqFlOrder))] = static_cast<float>(ord);
    }
    s.params[static_cast<size_t>(EqModel)] = 1;
  }
  for (size_t i = 0; i < p->params.size(); ++i) s.params[i] = static_cast<float>(p->params[i].clamp(s.params[i]));
}

}  // namespace gl
