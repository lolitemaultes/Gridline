// Gridline — "synths" pack, J-33 group "hex": catalogue entry, factory presets
// and data text of Hex Synth (gl.hexsynth). Continuous controls keep the
// reference's 0..1 positions (shown as %; the editor shows the physical values
// through hex_laws.h); the reference's integer settings are choices.
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include "../Packs.h"
#include "hex.h"

namespace gl::packs::synths {

namespace {

using namespace hex;

ParamDesc pct(const std::string& n, double d) { return ParamDesc{n, "%", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }

std::string oscName(int k) { return "Osc " + std::to_string(k + 1); }

}  // namespace

namespace hex {

const std::vector<std::string>& waveNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n = {"Custom", "Sine", "Sine + octave", "Sine stack", "Sine fall", "Organ", "Triangle",
                                  "Triangle soft", "Square", "Square soft", "Pulse narrow", "Pulse thin", "Saw"};
    for (int i = 1; i <= 7; ++i) n.push_back("Tone " + std::to_string(i));
    for (int i = 1; i <= 4; ++i) n.push_back("Noise " + std::to_string(i));
    for (int i = 1; i <= 9; ++i) n.push_back("Gate " + std::to_string(i));
    return n;
  }();
  return v;
}

const std::vector<std::string>& modSourceNames() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> n = {"None", "Velocity", "LFO 1", "LFO 2", "Aftertouch", "Pitch bend", "(controllers)"};
    for (int cc = 0; cc < 128; ++cc) n.push_back("CC " + std::to_string(cc));
    return n;
  }();
  return v;
}

const std::vector<std::string>& syncNames() {
  static const std::vector<std::string> v = {"Off", "1/16", "2/16", "3/16", "1/4", "2/4", "3/4", "4/4", "8/4", "16/4"};
  return v;
}

std::string paramName(int i);

}  // namespace hex

namespace {

std::vector<ParamDesc> hexParams() {
  std::vector<ParamDesc> q(HxCount);
  auto P = [&](int i, const std::string& n, double d) { q[static_cast<size_t>(i)] = pct(n, d); };
  P(HxGain, "Gain", 0.5);
  P(HxDrive, "Drive", 0);
  P(HxTranspose, "Transpose", 0.5);
  P(HxAmpA, "Amp attack", 0);
  P(HxAmpD, "Amp decay", 1);
  P(HxAmpS, "Amp sustain", 1);
  P(HxAmpR, "Amp release", 0);
  P(HxVelCurve, "Velocity curve", 0);
  P(HxUniDetune, "Unison detune", 0.2);
  P(HxUniPan, "Unison spread", 0);
  P(HxGlide, "Glide time", 0);
  P(HxEnvToPitch, "Filter env to pitch", 0.5);
  P(HxLfo2ToPitch, "LFO 2 to pitch", 0);
  P(HxLfo1Speed, "LFO 1 speed", 0.4);
  P(HxLfo2Speed, "LFO 2 speed", 0.4);
  P(HxCutoff, "Cutoff", 0.5);
  P(HxReso, "Resonance", 0);
  P(HxKeyTrack, "Key tracking", 0);
  P(HxVelTrack, "Velocity tracking", 0);
  P(HxFltA, "Filter attack", 0);
  P(HxFltD, "Filter decay", 1);
  P(HxFltS, "Filter sustain", 1);
  P(HxFltR, "Filter release", 1);
  P(HxFltEnvAmt, "Filter env amount", 0.5);
  P(HxFltLfo1, "Filter LFO 1", 0);
  for (int k = 0; k < kOscs; ++k) {
    const std::string o = oscName(k) + " ";
    P(HxOscOn + k, o + "on", k == 0 ? 1 : 0);  // the reference's reset program: oscillator 1 on at full mix
    P(HxCoarse + k, o + "coarse", 0.5);
    P(HxFine + k, o + "fine", 0.5);
    P(HxShift + k, o + "shift", 0.5);
    P(HxFreeRun + k, o + "free run", 1);
    P(HxPhase + k, o + "phase", 0);
    P(HxVelSens + k, o + "velocity", 1);
    P(HxInit + k, o + "init level", 0);
    P(HxOscA + k, o + "attack", 0);
    P(HxOscD + k, o + "decay", 1);
    P(HxOscS + k, o + "sustain", 1);
    P(HxOscR + k, o + "release", 1);
    P(HxPan + k, o + "pan", 0.5);
    P(HxMix + k, o + "level", k == 0 ? 1 : 0);
    P(HxLfo1Osc + k, o + "LFO 1", 0.5);
    P(HxLfo2Osc + k, o + "LFO 2", 0.5);
    for (int d = 0; d < kOscs; ++d) P(HxFm + 6 * d + k, oscName(k) + " to " + std::to_string(d + 1), 0);
  }
  P(HxSeqPlay, "Sequencer on", 0);
  P(HxSeqSwing, "Sequencer swing", 0);
  P(HxSeqPingPong, "Sequencer ping-pong", 0);
  P(HxSeqRandom, "Sequencer random", 0);
  P(HxSeqDual, "Sequencer dual", 0);
  P(HxSeqSteps, "Sequencer steps", 0.25);
  for (int s = 0; s < kSeqSteps; ++s) P(HxSeqStep + s, "Step " + std::to_string(s + 1), 0);
  P(HxEqOn, "EQ on", 0);
  for (int b = 0; b < kEqBands; ++b) P(HxEqBand + b, "EQ band " + std::to_string(b + 1), 0.5);
  for (int s = 0; s < 2; ++s) {
    const std::string f = "FX " + std::to_string(s + 1) + " ";
    P(HxFxOn + s, f + "on", 0);
    P(HxFlDelay + s, f + "flanger delay", 0.22);
    P(HxFlFeedback + s, f + "flanger feedback", 0.8);
    P(HxFlDepth + s, f + "flanger depth", 1);
    P(HxFlRate + s, f + "flanger rate", 0.15);
    P(HxFlMix + s, f + "flanger mix", 0.8);
    P(HxFlInvFeedback + s, f + "flanger invert feedback", 1);
    P(HxFlInvMix + s, f + "flanger invert mix", 1);
    P(HxChDepth + s, f + "chorus depth", 0.55);
    P(HxChRate + s, f + "chorus rate", 0.2);
    P(HxChMix + s, f + "chorus mix", 0.4);
    P(HxRvMix + s, f + "reverb mix", 0.5);
    P(HxRvDecay + s, f + "reverb decay", 0.5);
    P(HxRvDamp + s, f + "reverb damping", 0.5);
    P(HxRvHighCut + s, f + "reverb high cut", 0.5);
    P(HxDlTime + s, f + "delay time", 0.5);
    P(HxDlFeedback + s, f + "delay feedback", 0.6);
    P(HxDlRate + s, f + "delay mod rate", 0.24);
    P(HxDlDepth + s, f + "delay mod depth", 0.2);
    P(HxDlBlur + s, f + "delay blur", 0.5);
    P(HxDlMix + s, f + "delay mix", 0.35);
    P(HxLfBits + s, f + "lo-fi bits", 1);
    P(HxLfRate + s, f + "lo-fi rate", 0.4);
    P(HxLfFilter + s, f + "lo-fi filter", 1);
    P(HxPhFreq + s, f + "phaser frequency", 0.4);
    P(HxPhRate + s, f + "phaser rate", 0.4);
    P(HxPhDepth + s, f + "phaser depth", 0.5);
    P(HxPhFeedback + s, f + "phaser feedback", 0.5);
    P(HxPhMix + s, f + "phaser mix", 0.7);
    q[static_cast<size_t>(HxFxSync + s)] = ch(f + "delay sync", syncNames(), 0);
    q[static_cast<size_t>(HxFxType + s)] =
        ch(f + "type", {"Delay", "Chorus", "Reverb", "Flanger", "Phaser", "Lo-fi"}, s == 0 ? FxChorus : FxDelay);
  }
  P(HxUnused, "(spare)", 0);
  P(HxBendRange, "Bend range", 0.175);
  q[HxLfo1Sync] = ch("LFO 1 sync", syncNames(), 0);
  q[HxLfo1Free] = tog("LFO 1 free running", true);
  q[HxLfo2Sync] = ch("LFO 2 sync", syncNames(), 0);
  q[HxLfo2Free] = tog("LFO 2 free running", true);
  q[HxFilterType] = ch("Filter type", {"Off", "Low pass", "Band pass", "High pass"}, FtOff);
  {
    std::vector<std::string> v = {"Mono"};
    for (int i = 1; i <= 32; ++i) v.push_back(std::to_string(i));
    q[HxVoices] = ch("Voices", v, 8);
  }
  q[HxUnison] = ch("Unison", {"Off", "2", "3", "4", "5", "6", "7", "8"}, 0);
  q[HxGlideMode] = ch("Glide mode", {"From last, rate", "From held, rate", "From last, time", "From held, time"}, 0);
  for (int s = 0; s < kSlots; ++s)
    q[static_cast<size_t>(HxWave + s)] = ch(s < kOscs ? oscName(s) + " wave" : "LFO " + std::to_string(s - kOscs + 1) + " wave", waveNames(), 1);
  std::vector<std::string> targets = {"None"};
  for (int i = 0; i < kAutomatable; ++i) targets.push_back(paramName(i));
  for (int s = 0; s < kModSlots; ++s) {
    const std::string m = "Mod " + std::to_string(s + 1) + " ";
    q[static_cast<size_t>(HxModSource + s)] = ch(m + "source", modSourceNames(), 0);
    q[static_cast<size_t>(HxModTarget + s)] = ch(m + "target", targets, 0);
    P(HxModAmount + s, m + "amount", 0.5);
  }
  return q;
}

// Original factory sounds.
std::vector<PluginInfo::Preset> hexPresets() {
  std::vector<PluginInfo::Preset> v;
  // Two-operator bell: osc 2 modulates osc 1 with a fast-decaying index.
  v.push_back({"Glass Bell", {{HxOscOn + 1, 1.f}, {HxCoarse + 1, 0.5f + 7.f / 198.f}, {HxOscA + 1, 0.05f}, {HxOscD + 1, 0.55f},
                              {HxOscS + 1, 0.f}, {HxFm + 0 + 1, 0.55f}, {HxOscD, 0.62f}, {HxOscS, 0.f}, {HxOscR, 0.55f},
                              {HxAmpR, 0.6f}, {HxFxOn, 1.f}, {HxFxType, static_cast<float>(FxReverb)}}});
  // Detuned saw stack through the low-pass with a filter sweep.
  v.push_back({"Stack Lead", {{HxWave, 12.f}, {HxUnison, 4.f}, {HxUniDetune, 0.35f}, {HxUniPan, 0.6f}, {HxFilterType, 1.f},
                              {HxCutoff, 0.45f}, {HxReso, 0.35f}, {HxFltEnvAmt, 0.78f}, {HxFltD, 0.5f}, {HxFltS, 0.2f},
                              {HxGlide, 0.25f}, {HxFxOn, 1.f}, {HxFxType, static_cast<float>(FxDelay)}}});
  // Organ-like drawbars on three oscillators.
  v.push_back({"Drawbar Keys", {{HxWave, 5.f}, {HxOscOn + 1, 1.f}, {HxMix + 1, 0.6f}, {HxCoarse + 1, 0.5f + 12.f / 198.f},
                                {HxOscOn + 2, 1.f}, {HxMix + 2, 0.45f}, {HxCoarse + 2, 0.5f + 19.f / 198.f}, {HxAmpR, 0.25f},
                                {HxFxOn, 1.f}, {HxFxType, static_cast<float>(FxChorus)}}});
  // Slow pad: two sines a fifth apart with tremolo and a long release.
  v.push_back({"Soft Pad", {{HxOscOn + 1, 1.f}, {HxMix + 1, 0.7f}, {HxCoarse + 1, 0.5f + 7.f / 198.f}, {HxOscA, 0.45f},
                            {HxOscA + 1, 0.5f}, {HxAmpA, 0.4f}, {HxAmpR, 0.7f}, {HxLfo1Osc + 1, 0.65f}, {HxLfo1Speed, 0.35f},
                            {HxUnison, 2.f}, {HxUniPan, 0.8f}, {HxFxOn, 1.f}, {HxFxType, static_cast<float>(FxReverb)}}});
  return v;
}

}  // namespace

namespace hex {

std::string paramName(int i) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> names(HxCount);
    auto set = [&](int k, const std::string& s) { names[static_cast<size_t>(k)] = s; };
    const char* g[] = {"Gain", "Drive", "Transpose", "Amp attack", "Amp decay", "Amp sustain", "Amp release",
                       "Velocity curve", "Unison detune", "Unison spread", "Glide time", "Filter env to pitch",
                       "LFO 2 to pitch", "LFO 1 speed", "LFO 2 speed", "Cutoff", "Resonance", "Key tracking",
                       "Velocity tracking", "Filter attack", "Filter decay", "Filter sustain", "Filter release",
                       "Filter env amount", "Filter LFO 1"};
    for (int k = 0; k <= HxFltLfo1; ++k) set(k, g[k]);
    const char* of[] = {"on", "coarse", "fine", "shift", "free run", "phase", "velocity", "init level", "attack",
                        "decay", "sustain", "release", "pan", "level"};
    for (int f = 0; f < 14; ++f)
      for (int k = 0; k < kOscs; ++k) set(HxOscOn + 6 * f + k, oscName(k) + " " + of[f]);
    for (int d = 0; d < kOscs; ++d)
      for (int k = 0; k < kOscs; ++k) set(HxFm + 6 * d + k, oscName(k) + " to " + std::to_string(d + 1));
    for (int k = 0; k < kOscs; ++k) set(HxLfo1Osc + k, oscName(k) + " LFO 1"), set(HxLfo2Osc + k, oscName(k) + " LFO 2");
    const char* sq[] = {"Sequencer on", "Sequencer swing", "Sequencer ping-pong", "Sequencer random", "Sequencer dual",
                        "Sequencer steps"};
    for (int k = 0; k < 6; ++k) set(HxSeqPlay + k, sq[k]);
    for (int s = 0; s < kSeqSteps; ++s) set(HxSeqStep + s, "Step " + std::to_string(s + 1));
    set(HxEqOn, "EQ on");
    for (int b = 0; b < kEqBands; ++b) set(HxEqBand + b, "EQ band " + std::to_string(b + 1));
    const char* fx[] = {"on", "flanger delay", "flanger feedback", "flanger depth", "flanger rate", "flanger mix",
                        "flanger invert feedback", "flanger invert mix", "chorus depth", "chorus rate", "chorus mix",
                        "reverb mix", "reverb decay", "reverb damping", "reverb high cut", "delay time",
                        "delay feedback", "delay mod rate", "delay mod depth", "delay blur", "delay mix", "lo-fi bits",
                        "lo-fi rate", "lo-fi filter", "phaser frequency", "phaser rate", "phaser depth",
                        "phaser feedback", "phaser mix"};
    for (int f = 0; f < 29; ++f)
      for (int s = 0; s < 2; ++s) set(HxFxOn + 2 * f + s, "FX " + std::to_string(s + 1) + " " + fx[f]);
    return names;
  }();
  return i >= 0 && i < HxCount ? names[static_cast<size_t>(i)] : std::string();
}

std::string formatHexData(const HexData& d) {
  std::string out;
  char buf[32];
  for (int s = 0; s < kSlots; ++s) {
    bool any = false;
    for (int h = 0; h < kHarmonics; ++h) any = any || d.amp[s][h] != 0.f || d.phase[s][h] != 0.f;
    if (!any) continue;
    out += "W" + std::to_string(s);
    for (int h = 0; h < kHarmonics; ++h) std::snprintf(buf, sizeof buf, " %.9g", static_cast<double>(d.amp[s][h])), out += buf;
    out += " |";
    for (int h = 0; h < kHarmonics; ++h) std::snprintf(buf, sizeof buf, " %.9g", static_cast<double>(d.phase[s][h])), out += buf;
    out += "\n";
  }
  std::string links;
  for (int i = 0; i < kAutomatable; ++i)
    if (d.link[i] >= 0) links += " " + std::to_string(i) + " " + std::to_string(d.link[i]);
  if (!links.empty()) out += "L" + links + "\n";
  if (!d.name.empty()) out += "N " + d.name + "\n";
  return out;
}

bool parseHexData(const std::string& text, HexData& d) {
  d = HexData{};
  std::istringstream in(text);
  std::string line;
  bool ok = true;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == 'W') {
      const int s = std::atoi(line.c_str() + 1);
      if (s < 0 || s >= kSlots) {
        ok = false;
        continue;
      }
      const size_t bar = line.find('|');
      if (bar == std::string::npos) {
        ok = false;
        continue;
      }
      const size_t sp = line.find(' ');
      std::istringstream a(line.substr(sp == std::string::npos ? line.size() : sp, bar - std::min(bar, sp)));
      std::istringstream p(line.substr(bar + 1));
      for (int h = 0; h < kHarmonics; ++h) {
        double v = 0;
        if (a >> v) d.amp[s][h] = static_cast<float>(v);
        if (p >> v) d.phase[s][h] = static_cast<float>(v);
      }
    } else if (line[0] == 'L') {
      std::istringstream l(line.substr(1));
      int i = 0, cc = 0;
      while (l >> i >> cc)
        if (i >= 0 && i < kAutomatable && cc >= 0 && cc < 128) d.link[i] = static_cast<int8_t>(cc);
    } else if (line[0] == 'N' && line.size() > 2) {
      d.name = line.substr(2);
    }
  }
  return ok;
}

}  // namespace hex

PluginInfo hexSynthInfo() {
  PluginInfo p{"gl.hexsynth", "Hex Synth", "Synth", PluginKind::Generator, hexParams(), false, hexPresets()};
  p.editorWidth = 980;
  return p;
}

}  // namespace gl::packs::synths
