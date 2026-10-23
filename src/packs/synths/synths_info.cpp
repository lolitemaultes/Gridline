// Gridline — "synths" pack: catalogue entries (Analog, Wavetable).
#include <initializer_list>
#include <utility>
#include "../Packs.h"
#include "synths.h"

namespace gl::packs::synths {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
ParamDesc steps(const std::string& n, double mn, double mx, double d) { return lin(n, "", mn, mx, d); }

// Sub-hertz rates: "Hz" would format as an integer, so use the generic "%.2f unit" form.
const char* const kRateUnit = "Hz ";

const std::vector<std::string> kFilterModes = {"LP 12", "LP 24", "BP 12", "BP 24", "HP 12", "HP 24", "Notch", "Peak"};
const std::vector<std::string> kLfoShapes = {"Sine", "Triangle", "Ramp up", "Ramp down", "Square", "Step random", "Smooth random"};
const std::vector<std::string> kDivisions = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/4D", "1/2", "1 bar", "2 bars", "4 bars"};
const std::vector<std::string> kPlayModes = {"Poly", "Mono", "Legato"};

const std::vector<std::string> kAnDests = {"None", "Pitch", "Osc 1 pitch", "Osc 2 pitch", "Osc 3 pitch", "Pulse width", "Osc mod depth",
                                           "Cutoff", "Resonance", "Volume", "Pan", "Unison detune", "LFO 1 rate", "LFO 2 rate", "Noise"};
const std::vector<std::string> kAnSources = {"None", "LFO 1", "LFO 2", "Mod env", "Filter env", "Amp env", "Velocity", "Key", "Random"};

const std::vector<std::string> kWtDests = {"None", "Pitch", "A pitch", "B pitch", "A position", "B position", "A warp", "B warp",
                                           "A level", "B level", "A detune", "B detune", "F1 cutoff", "F1 reso", "F2 cutoff", "F2 reso",
                                           "Sub level", "Noise level", "Volume", "Pan", "LFO 1 rate", "LFO 2 rate"};
const std::vector<std::string> kWtSources = {"None", "Env 2", "Env 3", "Env 4", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "Velocity",
                                             "Key", "Random", "Macro 1", "Macro 2", "Macro 3", "Macro 4"};

void fxParams(std::vector<ParamDesc>& p, int base) {
  p[base + FxDrive] = lin("Drive", "%", 0, 1, 0);
  p[base + FxChoOn] = tog("Chorus", 0);
  p[base + FxChoRate] = lg("Chorus rate", kRateUnit, 0.05, 5, 0.6);
  p[base + FxChoDepth] = lin("Chorus depth", "%", 0, 1, 0.5);
  p[base + FxChoMix] = lin("Chorus mix", "%", 0, 1, 0.5);
  p[base + FxDlyOn] = tog("Delay", 0);
  p[base + FxDlyDiv] = ch("Delay time", kDivisions, Div1_8D);
  p[base + FxDlyFb] = lin("Delay feedback", "%", 0, 0.95, 0.4);
  p[base + FxDlyMix] = lin("Delay mix", "%", 0, 1, 0.25);
  p[base + FxRevOn] = tog("Reverb", 0);
  p[base + FxRevSize] = lin("Reverb size", "%", 0, 1, 0.6);
  p[base + FxRevDamp] = lin("Reverb damping", "%", 0, 1, 0.5);
  p[base + FxRevMix] = lin("Reverb mix", "%", 0, 1, 0.25);
}

using Values = std::vector<std::pair<int, float>>;

// ------------------------------------------------------------------ Analog
PluginInfo analogInfo() {
  PluginInfo p{"gl.analog", "Analog Synth", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(AnCount);
  q[AnVolume] = lin("Volume", "%", 0, 1, 0.7);
  q[AnPan] = lin("Pan", "pan", -1, 1, 0);
  q[AnTranspose] = lin("Transpose", "st", -24, 24, 0);
  q[AnFine] = lin("Fine tune", "ct", -100, 100, 0);
  q[AnVoices] = steps("Polyphony", 1, 32, 16);
  q[AnPlayMode] = ch("Voice mode", kPlayModes, PlayPoly);
  q[AnGlide] = lg("Glide", "s", 0.001, 2, 0.001);
  q[AnGlideMode] = ch("Glide mode", {"Always", "Overlap"}, 0);
  q[AnVelAmp] = lin("Velocity to volume", "%", 0, 1, 0.6);
  q[AnVelFilter] = lin("Velocity to cutoff", "%", 0, 1, 0.2);
  q[AnUniVoices] = steps("Unison voices", 1, 8, 1);
  q[AnUniDetune] = lin("Unison detune", "%", 0, 1, 0.25);
  q[AnUniSpread] = lin("Unison spread", "%", 0, 1, 0.7);
  const std::vector<std::string> waves = {"Sine", "Triangle", "Saw", "Pulse", "Noise"};
  const std::vector<std::string> mods = {"Off", "Sync", "Ring", "FM"};
  const char* srcName[3] = {"3", "1", "2"};
  for (int k = 0; k < kAnOscCount; ++k) {
    std::string o = "Osc " + std::to_string(k + 1) + " ";
    q[anOsc(k, OscOn)] = tog(o + "on", k < 2 ? 1 : 0);
    q[anOsc(k, OscWave)] = ch(o + "wave", waves, k == 2 ? WavePulse : WaveSaw);
    q[anOsc(k, OscCoarse)] = lin(o + "coarse", "st", -36, 36, k == 2 ? -12 : 0);
    q[anOsc(k, OscFine)] = lin(o + "fine", "ct", -100, 100, k == 1 ? 7 : 0);
    q[anOsc(k, OscLevel)] = lin(o + "level", "%", 0, 1, k == 0 ? 0.8 : 0.6);
    q[anOsc(k, OscPw)] = lin(o + "pulse width", "%", 0.05, 0.95, 0.5);
    q[anOsc(k, OscMod)] = ch(o + "mod by osc " + srcName[k], mods, ModNone);
    q[anOsc(k, OscModAmt)] = lin(o + "mod depth", "%", 0, 1, 0.5);
  }
  q[AnSubLevel] = lin("Sub level", "%", 0, 1, 0);
  q[AnSubOct] = ch("Sub octave", {"-1 oct", "-2 oct"}, 0);
  q[AnSubWave] = ch("Sub wave", {"Sine", "Square"}, 1);
  q[AnNoise] = lin("Noise", "%", 0, 1, 0);
  q[AnFltMode] = ch("Filter mode", kFilterModes, FltLp24);
  q[AnCutoff] = lg("Cutoff", "Hz", 20, 20000, 4000);
  q[AnReso] = lin("Resonance", "%", 0, 1, 0.15);
  q[AnDrive] = lin("Filter drive", "%", 0, 1, 0);
  q[AnKeyTrack] = lin("Key tracking", "%", 0, 1, 0.3);
  q[AnFltEnv] = lin("Filter env amount", "%", -1, 1, 0.3);
  q[AnAmpA] = lg("Amp attack", "s", 0.001, 10, 0.003);
  q[AnAmpD] = lg("Amp decay", "s", 0.005, 10, 0.3);
  q[AnAmpS] = lin("Amp sustain", "%", 0, 1, 0.8);
  q[AnAmpR] = lg("Amp release", "s", 0.005, 15, 0.25);
  q[AnFenA] = lg("Filter attack", "s", 0.001, 10, 0.003);
  q[AnFenD] = lg("Filter decay", "s", 0.005, 10, 0.4);
  q[AnFenS] = lin("Filter sustain", "%", 0, 1, 0.3);
  q[AnFenR] = lg("Filter release", "s", 0.005, 15, 0.3);
  q[AnModA] = lg("Mod attack", "s", 0.001, 10, 0.001);
  q[AnModD] = lg("Mod decay", "s", 0.005, 10, 0.25);
  q[AnModS] = lin("Mod sustain", "%", 0, 1, 0);
  q[AnModR] = lg("Mod release", "s", 0.005, 15, 0.2);
  q[AnModDest] = ch("Mod env target", kAnDests, AdNone);
  q[AnModAmt] = lin("Mod env amount", "%", -1, 1, 0);
  for (int k = 0; k < 2; ++k) {
    std::string l = "LFO " + std::to_string(k + 1) + " ";
    q[anLfo(k, LfoShapeP)] = ch(l + "shape", kLfoShapes, LfoSine);
    q[anLfo(k, LfoRate)] = lg(l + "rate", kRateUnit, 0.02, 40, k == 0 ? 5 : 0.5);
    q[anLfo(k, LfoSync)] = tog(l + "tempo sync", 0);
    q[anLfo(k, LfoDiv)] = ch(l + "division", kDivisions, Div1_4);
    q[anLfo(k, LfoRetrig)] = tog(l + "retrigger", k == 0 ? 1 : 0);
    q[anLfo(k, LfoDelay)] = lg(l + "fade in", "s", 0.001, 5, 0.001);
    q[anLfo(k, LfoDest)] = ch(l + "target", kAnDests, AdNone);
    q[anLfo(k, LfoAmt)] = lin(l + "amount", "%", -1, 1, 0);
  }
  for (int r = 0; r < kAnMatrixRows; ++r) {
    std::string m = "Mod " + std::to_string(r + 1) + " ";
    q[anMx(r, 0)] = ch(m + "source", kAnSources, AsNone);
    q[anMx(r, 1)] = ch(m + "target", kAnDests, AdNone);
    q[anMx(r, 2)] = lin(m + "amount", "%", -1, 1, 0);
  }
  q[AnArpOn] = tog("Arpeggiator", 0);
  q[AnArpMode] = ch("Arp mode", {"Up", "Down", "Up-down", "Random", "As played"}, ArpUp);
  q[AnArpOct] = steps("Arp octaves", 1, 4, 1);
  q[AnArpDiv] = ch("Arp rate", kDivisions, Div1_16);
  q[AnArpGate] = lin("Arp gate", "%", 0.05, 1, 0.6);
  fxParams(q, AnFxDrive);

  auto P = [&](const char* name, Values v) { p.presets.push_back({name, std::move(v)}); };
  const int o1 = AnOsc1, o2 = AnOsc1 + OscStride, o3 = AnOsc1 + 2 * OscStride;
  P("Velvet Pad", {{AnUniVoices, 4}, {AnUniDetune, 0.35f}, {AnUniSpread, 1}, {o2 + OscFine, 9}, {o2 + OscLevel, 0.5f},
                   {AnFltMode, FltLp12}, {AnCutoff, 2200}, {AnReso, 0.1f}, {AnFltEnv, 0.15f}, {AnFenA, 1.2f}, {AnFenD, 2.0f},
                   {AnFenS, 0.5f}, {AnAmpA, 0.9f}, {AnAmpD, 1.5f}, {AnAmpS, 0.85f}, {AnAmpR, 2.2f}, {anLfo(1, LfoDest), AdCutoff},
                   {anLfo(1, LfoAmt), 0.18f}, {anLfo(1, LfoRate), 0.25f}, {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxRevOn, 1},
                   {AnFxDrive + FxRevMix, 0.35f}, {AnFxDrive + FxRevSize, 0.8f}, {AnVolume, 0.55f}});
  P("Rubber Bass", {{AnPlayMode, PlayLegato}, {AnGlide, 0.06f}, {o2 + OscWave, WavePulse}, {o2 + OscCoarse, -12}, {o2 + OscFine, 0},
                    {o2 + OscPw, 0.35f}, {AnSubLevel, 0.6f}, {AnFltMode, FltLp24}, {AnCutoff, 380}, {AnReso, 0.35f}, {AnFltEnv, 0.55f},
                    {AnFenD, 0.22f}, {AnFenS, 0.05f}, {AnAmpS, 0.9f}, {AnAmpR, 0.08f}, {AnTranspose, -12}, {AnFxDrive, 0.2f}});
  P("Glass Pluck", {{o1 + OscWave, WaveTriangle}, {o2 + OscWave, WaveSine}, {o2 + OscCoarse, 12}, {o2 + OscFine, 3}, {o2 + OscLevel, 0.5f},
                    {o3 + OscOn, 1}, {o3 + OscWave, WaveSine}, {o3 + OscCoarse, 19}, {o3 + OscLevel, 0.25f}, {AnFltMode, FltLp12},
                    {AnCutoff, 900}, {AnFltEnv, 0.75f}, {AnFenD, 0.3f}, {AnFenS, 0}, {AnAmpD, 0.45f}, {AnAmpS, 0}, {AnAmpR, 0.4f},
                    {AnFxDrive + FxDlyOn, 1}, {AnFxDrive + FxDlyMix, 0.2f}, {AnFxDrive + FxRevOn, 1}, {AnFxDrive + FxRevMix, 0.2f}});
  P("Trance Stack", {{AnUniVoices, 8}, {AnUniDetune, 0.45f}, {AnUniSpread, 1}, {o2 + OscCoarse, 12}, {o2 + OscFine, 0}, {o2 + OscLevel, 0.35f},
                     {AnFltMode, FltLp12}, {AnCutoff, 9000}, {AnReso, 0.05f}, {AnFltEnv, 0.1f}, {AnAmpS, 0.9f}, {AnAmpR, 0.4f},
                     {AnFxDrive + FxDlyOn, 1}, {AnFxDrive + FxDlyMix, 0.18f}, {AnFxDrive + FxRevOn, 1}, {AnFxDrive + FxRevMix, 0.25f},
                     {AnVolume, 0.5f}});
  P("Sync Scream", {{AnPlayMode, PlayMono}, {AnGlide, 0.04f}, {o2 + OscMod, ModSync}, {o2 + OscCoarse, 7}, {o2 + OscFine, 0},
                    {o2 + OscLevel, 0.8f}, {o1 + OscLevel, 0.3f}, {AnModDest, AdOsc2Pitch}, {AnModAmt, 0.6f}, {AnModD, 0.6f},
                    {AnFltMode, FltLp24}, {AnCutoff, 5000}, {AnReso, 0.25f}, {AnFxDrive, 0.35f}, {anLfo(0, LfoDest), AdPitch},
                    {anLfo(0, LfoAmt), 0.02f}, {anLfo(0, LfoDelay), 0.4f}, {AnFxDrive + FxDlyOn, 1}});
  P("Ring Bells", {{o1 + OscWave, WaveSine}, {o2 + OscWave, WaveSine}, {o2 + OscCoarse, 7}, {o2 + OscFine, 14}, {o2 + OscMod, ModRing},
                   {o2 + OscModAmt, 1}, {o2 + OscLevel, 0.7f}, {o1 + OscLevel, 0.3f}, {AnFltMode, FltLp12}, {AnCutoff, 12000},
                   {AnFltEnv, 0}, {AnAmpD, 2.5f}, {AnAmpS, 0}, {AnAmpR, 2.5f}, {AnFxDrive + FxRevOn, 1}, {AnFxDrive + FxRevMix, 0.35f}});
  P("Arp Bubbles", {{AnArpOn, 1}, {AnArpOct, 2}, {AnArpDiv, Div1_16}, {AnArpGate, 0.5f}, {o1 + OscWave, WavePulse}, {o1 + OscPw, 0.3f},
                    {o2 + OscOn, 0}, {AnFltMode, FltLp24}, {AnCutoff, 700}, {AnReso, 0.55f}, {AnFltEnv, 0.6f}, {AnFenD, 0.15f}, {AnFenS, 0},
                    {AnAmpD, 0.2f}, {AnAmpS, 0.2f}, {AnAmpR, 0.1f}, {AnFxDrive + FxDlyOn, 1}, {AnFxDrive + FxDlyMix, 0.3f}});
  P("PWM Strings", {{o1 + OscWave, WavePulse}, {o1 + OscPw, 0.35f}, {o2 + OscWave, WaveSaw}, {o2 + OscFine, -5}, {o2 + OscLevel, 0.4f},
                    {anLfo(0, LfoDest), AdPulseWidth}, {anLfo(0, LfoAmt), 0.35f}, {anLfo(0, LfoRate), 0.7f}, {anLfo(0, LfoRetrig), 0},
                    {AnFltMode, FltLp12}, {AnCutoff, 3200}, {AnFltEnv, 0.05f}, {AnAmpA, 0.35f}, {AnAmpS, 0.9f}, {AnAmpR, 0.9f},
                    {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxChoMix, 0.6f}, {AnFxDrive + FxRevOn, 1}, {AnVolume, 0.55f}});
  P("Wobble Bass", {{AnPlayMode, PlayLegato}, {o2 + OscWave, WavePulse}, {o2 + OscCoarse, -12}, {o2 + OscFine, 0}, {AnSubLevel, 0.5f},
                    {AnFltMode, FltLp24}, {AnCutoff, 500}, {AnReso, 0.5f}, {AnFltEnv, 0}, {anLfo(0, LfoSync), 1}, {anLfo(0, LfoDiv), Div1_8},
                    {anLfo(0, LfoDest), AdCutoff}, {anLfo(0, LfoAmt), 0.55f}, {anLfo(0, LfoShapeP), LfoTriangle}, {AnFxDrive, 0.4f},
                    {AnTranspose, -12}});
  P("Fingered Bass", {{o2 + OscOn, 0}, {AnSubLevel, 0.4f}, {AnSubWave, 0}, {AnFltMode, FltLp12}, {AnCutoff, 550}, {AnReso, 0.1f},
                      {AnFltEnv, 0.45f}, {AnFenD, 0.18f}, {AnFenS, 0.1f}, {AnVelFilter, 0.6f}, {AnAmpD, 0.9f}, {AnAmpS, 0.35f},
                      {AnAmpR, 0.12f}, {AnTranspose, -12}, {AnVoices, 4}});
  P("Brass Section", {{o2 + OscFine, -8}, {o2 + OscLevel, 0.7f}, {o3 + OscOn, 1}, {o3 + OscWave, WaveSaw}, {o3 + OscCoarse, 0},
                      {o3 + OscFine, 6}, {o3 + OscLevel, 0.5f}, {AnFltMode, FltLp12}, {AnCutoff, 900}, {AnFltEnv, 0.55f}, {AnFenA, 0.08f},
                      {AnFenD, 0.5f}, {AnFenS, 0.55f}, {AnAmpA, 0.04f}, {AnAmpS, 0.9f}, {AnAmpR, 0.3f}, {AnFxDrive + FxRevOn, 1}});
  P("Laser Zap", {{o1 + OscWave, WaveSaw}, {o2 + OscOn, 0}, {AnNoise, 0.15f}, {AnModDest, AdPitch}, {AnModAmt, 1}, {AnModD, 0.12f},
                  {AnModS, 0}, {AnFltMode, FltBp12}, {AnCutoff, 2500}, {AnReso, 0.5f}, {AnFltEnv, 0.5f}, {AnFenD, 0.15f}, {AnFenS, 0},
                  {AnAmpD, 0.25f}, {AnAmpS, 0}, {AnAmpR, 0.1f}, {AnFxDrive + FxDlyOn, 1}, {AnTranspose, 12}});
  p.editorWidth = 1080;
  return p;
}

// --------------------------------------------------------------- Wavetable
PluginInfo wavetableInfo() {
  PluginInfo p{"gl.wavetable", "Isoline", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(WtCount);
  q[WtVolume] = lin("Volume", "%", 0, 1, 0.7);
  q[WtTranspose] = lin("Transpose", "st", -24, 24, 0);
  q[WtVoices] = steps("Polyphony", 1, 32, 12);
  q[WtPlayMode] = ch("Voice mode", kPlayModes, PlayPoly);
  q[WtGlide] = lg("Glide", "s", 0.001, 2, 0.001);
  q[WtVelAmp] = lin("Velocity to volume", "%", 0, 1, 0.5);
  std::vector<std::string> tables;
  for (int i = 0; i < TblCount; ++i) tables.push_back(wavetableNames()[i]);
  const std::vector<std::string> warps = {"Off", "Bend +", "Bend -", "Sync", "Mirror", "Crush", "FM"};
  for (int k = 0; k < 2; ++k) {
    std::string o = std::string("Osc ") + (k ? "B " : "A ");
    q[wtOsc(k, WoOn)] = tog(o + "on", k == 0 ? 1 : 0);
    q[wtOsc(k, WoTable)] = ch(o + "table", tables, k == 0 ? TblAnalog : TblPulse);
    q[wtOsc(k, WoPos)] = lin(o + "position", "%", 0, 1, k == 0 ? 4.0 / 7.0 : 0.0);
    q[wtOsc(k, WoWarp)] = ch(o + "warp", warps, WarpOff);
    q[wtOsc(k, WoWarpAmt)] = lin(o + "warp amount", "%", 0, 1, 0);
    q[wtOsc(k, WoCoarse)] = lin(o + "coarse", "st", -36, 36, 0);
    q[wtOsc(k, WoFine)] = lin(o + "fine", "ct", -100, 100, 0);
    q[wtOsc(k, WoLevel)] = lin(o + "level", "%", 0, 1, 0.75);
    q[wtOsc(k, WoPan)] = lin(o + "pan", "pan", -1, 1, 0);
    q[wtOsc(k, WoUni)] = steps(o + "unison", 1, kWtMaxUnison, 1);
    q[wtOsc(k, WoDetune)] = lin(o + "detune", "%", 0, 1, 0.2);
    q[wtOsc(k, WoBlend)] = lin(o + "blend", "%", 0, 1, 0.75);
    q[wtOsc(k, WoWidth)] = lin(o + "stereo width", "%", 0, 1, 0.8);
    q[wtOsc(k, WoPhase)] = lin(o + "phase", "%", 0, 1, 0);
    q[wtOsc(k, WoRandPhase)] = tog(o + "random phase", 1);
  }
  q[WtSubLevel] = lin("Sub level", "%", 0, 1, 0);
  q[WtSubOct] = ch("Sub octave", {"-1 oct", "-2 oct"}, 0);
  q[WtSubWave] = ch("Sub wave", {"Sine", "Triangle", "Square"}, 0);
  q[WtNoiseLevel] = lin("Noise level", "%", 0, 1, 0);
  q[WtNoiseColor] = lin("Noise colour", "%", 0, 1, 0.5);
  for (int k = 0; k < 2; ++k) {
    std::string f = "Filter " + std::to_string(k + 1) + " ";
    q[wtFlt(k, WfOn)] = tog(f + "on", k == 0 ? 1 : 0);
    q[wtFlt(k, WfType)] = ch(f + "type", kFilterModes, k == 0 ? FltLp24 : FltHp12);
    q[wtFlt(k, WfCutoff)] = lg(f + "cutoff", "Hz", 20, 20000, k == 0 ? 6000 : 80);
    q[wtFlt(k, WfReso)] = lin(f + "resonance", "%", 0, 1, 0.15);
    q[wtFlt(k, WfDrive)] = lin(f + "drive", "%", 0, 1, 0);
    q[wtFlt(k, WfKeyTrack)] = lin(f + "key tracking", "%", 0, 1, 0.2);
  }
  q[WtRouting] = ch("Filter routing", {"Serial", "Parallel"}, RouteSerial);
  const char* stage[4] = {"attack", "decay", "sustain", "release"};
  const double defs[4][4] = {{0.003, 0.4, 0.8, 0.3}, {0.003, 0.5, 0.2, 0.4}, {0.2, 1.0, 0.5, 0.8}, {0.001, 0.15, 0.0, 0.15}};
  for (int e = 0; e < 4; ++e)
    for (int s = 0; s < 4; ++s) {
      std::string n = (e == 0 ? std::string("Amp env ") : "Env " + std::to_string(e + 1) + " ") + stage[s];
      if (s == 2)
        q[wtEnv(e, s)] = lin(n, "%", 0, 1, defs[e][s]);
      else
        q[wtEnv(e, s)] = lg(n, "s", s == 0 ? 0.001 : 0.005, s == 3 ? 15 : 10, defs[e][s]);
    }
  const double lfoRates[4] = {2, 0.3, 5, 0.12};
  for (int k = 0; k < 4; ++k) {
    std::string l = "LFO " + std::to_string(k + 1) + " ";
    q[wtLfo(k, WlShape)] = ch(l + "shape", kLfoShapes, k == 2 ? LfoTriangle : LfoSine);
    q[wtLfo(k, WlRate)] = lg(l + "rate", kRateUnit, 0.02, 40, lfoRates[k]);
    q[wtLfo(k, WlSync)] = tog(l + "tempo sync", 0);
    q[wtLfo(k, WlDiv)] = ch(l + "division", kDivisions, Div1_4);
    q[wtLfo(k, WlRetrig)] = tog(l + "retrigger", k == 2 ? 1 : 0);
  }
  for (int r = 0; r < kWtMatrixRows; ++r) {
    std::string m = "Mod " + std::to_string(r + 1) + " ";
    q[wtMx(r, 0)] = ch(m + "source", kWtSources, r == 0 ? WsEnv2 : WsNone);
    q[wtMx(r, 1)] = ch(m + "target", kWtDests, r == 0 ? WdF1Cutoff : WdNone);
    q[wtMx(r, 2)] = lin(m + "amount", "%", -1, 1, r == 0 ? 0.25 : 0);
  }
  for (int k = 0; k < 4; ++k) q[WtMacro1 + k] = lin("Macro " + std::to_string(k + 1), "%", 0, 1, 0);
  fxParams(q, WtFxDrive);

  auto P = [&](const char* name, Values v) { p.presets.push_back({name, std::move(v)}); };
  const int A = WtOscA, B = WtOscA + WoStride, F1 = WtFlt1, F2 = WtFlt1 + WfStride;
  auto mx = [](int row, int src, int dst, float amt) { return Values{{wtMx(row, 0), float(src)}, {wtMx(row, 1), float(dst)}, {wtMx(row, 2), amt}}; };
  auto cat = [](std::initializer_list<Values> parts) {
    Values out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
  };
  P("Hyper Saw", cat({{{A + WoPos, 4.0f / 7.0f}, {A + WoUni, 16}, {A + WoDetune, 0.35f}, {A + WoBlend, 0.8f}, {A + WoWidth, 1},
                       {B + WoOn, 1}, {B + WoTable, TblAnalog}, {B + WoPos, 4.0f / 7.0f}, {B + WoCoarse, 12}, {B + WoUni, 8}, {B + WoDetune, 0.3f},
                       {B + WoLevel, 0.4f}, {F1 + WfType, FltLp12}, {F1 + WfCutoff, 9000}, {wtEnv(0, 2), 0.9f}, {wtEnv(0, 3), 0.5f},
                       {WtFxDrive + FxDlyOn, 1}, {WtFxDrive + FxDlyMix, 0.2f}, {WtFxDrive + FxRevOn, 1}, {WtFxDrive + FxRevMix, 0.3f},
                       {WtVolume, 0.5f}},
                      mx(0, WsNone, WdNone, 0)}));
  P("Formant Choir", cat({{{A + WoTable, TblFormant}, {A + WoPos, 0.2f}, {A + WoUni, 4}, {A + WoDetune, 0.15f}, {F1 + WfCutoff, 5000},
                           {wtEnv(0, 0), 0.6f}, {wtEnv(0, 3), 1.6f}, {WtFxDrive + FxChoOn, 1}, {WtFxDrive + FxRevOn, 1},
                           {WtFxDrive + FxRevMix, 0.4f}, {WtFxDrive + FxRevSize, 0.85f}, {wtLfo(1, WlRate), 0.15f}},
                          mx(0, WsLfo2, WdAPos, 0.45f), mx(1, WsNone, WdNone, 0)}));
  P("Digital Keys", cat({{{A + WoTable, TblFm}, {A + WoPos, 0.1f}, {F1 + WfCutoff, 12000}, {wtEnv(0, 1), 1.2f}, {wtEnv(0, 2), 0.0f},
                          {wtEnv(0, 3), 0.6f}, {wtEnv(1, 1), 0.6f}, {wtEnv(1, 2), 0.0f}, {B + WoOn, 1}, {B + WoTable, TblHarmonic},
                          {B + WoPos, 0.3f}, {B + WoCoarse, 12}, {B + WoLevel, 0.25f}, {WtFxDrive + FxRevOn, 1}},
                         mx(0, WsEnv2, WdAPos, 0.6f), mx(1, WsVelocity, WdAPos, 0.3f)}));
  P("Resonant Drift", cat({{{A + WoTable, TblReso}, {A + WoUni, 3}, {A + WoDetune, 0.2f}, {F1 + WfCutoff, 8000}, {wtEnv(0, 0), 0.8f},
                            {wtEnv(0, 3), 2.0f}, {wtLfo(3, WlRate), 0.08f}, {WtFxDrive + FxChoOn, 1}, {WtFxDrive + FxRevOn, 1},
                            {WtFxDrive + FxRevMix, 0.35f}},
                           mx(0, WsLfo4, WdAPos, 0.8f), mx(1, WsNone, WdNone, 0)}));
  P("Growl Bass", cat({{{WtPlayMode, PlayLegato}, {WtGlide, 0.03f}, {WtTranspose, -12}, {A + WoTable, TblSync}, {A + WoPos, 0.3f},
                        {A + WoWarp, WarpBendPlus}, {A + WoWarpAmt, 0.4f}, {WtSubLevel, 0.6f}, {F1 + WfType, FltLp24}, {F1 + WfCutoff, 1800},
                        {F1 + WfReso, 0.35f}, {F1 + WfDrive, 0.5f}, {wtLfo(0, WlSync), 1}, {wtLfo(0, WlDiv), Div1_8},
                        {wtLfo(0, WlShape), LfoTriangle}, {wtLfo(0, WlRetrig), 1}, {WtFxDrive, 0.35f}},
                       mx(0, WsLfo1, WdAPos, 0.55f), mx(1, WsLfo1, WdF1Cutoff, 0.35f)}));
  P("Pulse Drift", cat({{{A + WoTable, TblPulse}, {A + WoPos, 0.3f}, {A + WoUni, 2}, {A + WoDetune, 0.12f}, {F1 + WfType, FltLp12},
                         {F1 + WfCutoff, 3500}, {wtEnv(0, 0), 0.15f}, {wtEnv(0, 3), 0.9f}, {wtLfo(1, WlRate), 0.6f},
                         {WtFxDrive + FxChoOn, 1}, {WtFxDrive + FxChoMix, 0.6f}},
                        mx(0, WsLfo2, WdAPos, 0.35f), mx(1, WsEnv2, WdF1Cutoff, 0.2f)}));
  P("Drawbar Organ", cat({{{A + WoTable, TblOrgan}, {A + WoPos, 0.55f}, {F1 + WfOn, 0}, {wtEnv(0, 0), 0.004f}, {wtEnv(0, 2), 1.0f},
                           {wtEnv(0, 3), 0.08f}, {wtLfo(2, WlRate), 6.0f}, {WtFxDrive + FxChoOn, 1}, {WtFxDrive + FxChoRate, 3.5f},
                           {WtFxDrive + FxRevOn, 1}, {WtFxDrive + FxRevMix, 0.15f}},
                          mx(0, WsLfo3, WdPitch, 0.004f)}));
  P("Glass Bell", cat({{{A + WoTable, TblHarmonic}, {A + WoPos, 0.75f}, {B + WoOn, 1}, {B + WoTable, TblFm}, {B + WoPos, 0.5f},
                        {B + WoCoarse, 19}, {B + WoLevel, 0.3f}, {F1 + WfCutoff, 14000}, {wtEnv(0, 1), 3.0f}, {wtEnv(0, 2), 0.0f},
                        {wtEnv(0, 3), 3.0f}, {WtFxDrive + FxDlyOn, 1}, {WtFxDrive + FxRevOn, 1}, {WtFxDrive + FxRevMix, 0.4f}},
                       mx(0, WsEnv2, WdBLevel, -0.5f)}));
  P("Sub Pressure", cat({{{A + WoPos, 0.0f}, {WtSubLevel, 0.8f}, {WtSubOct, 0}, {F1 + WfCutoff, 600}, {WtTranspose, -12},
                          {wtEnv(0, 1), 0.6f}, {wtEnv(0, 2), 0.7f}, {wtEnv(0, 3), 0.12f}, {WtPlayMode, PlayMono}, {WtFxDrive, 0.15f}},
                         mx(0, WsEnv2, WdF1Cutoff, 0.3f)}));
  P("Macro Morph Lead", cat({{{WtPlayMode, PlayLegato}, {WtGlide, 0.05f}, {A + WoTable, TblDigital}, {A + WoUni, 3}, {A + WoDetune, 0.1f},
                              {B + WoOn, 1}, {B + WoTable, TblOddEven}, {B + WoFine, 9}, {B + WoLevel, 0.5f}, {F1 + WfCutoff, 3000},
                              {F1 + WfReso, 0.3f}, {WtMacro1, 0.3f}, {WtMacro2, 0.5f}, {WtFxDrive + FxDlyOn, 1}, {WtFxDrive + FxDlyMix, 0.2f}},
                             mx(0, WsMacro1, WdAPos, 1.0f), mx(1, WsMacro1, WdBPos, 1.0f), mx(2, WsMacro2, WdF1Cutoff, 0.6f),
                             mx(3, WsLfo3, WdPitch, 0.006f), mx(4, WsEnv2, WdF1Cutoff, 0.2f)}));
  P("Vowel Bass", cat({{{WtPlayMode, PlayMono}, {WtTranspose, -12}, {A + WoTable, TblFormant}, {A + WoWarp, WarpMirror},
                        {A + WoWarpAmt, 0.3f}, {WtSubLevel, 0.5f}, {F1 + WfCutoff, 2500}, {F2 + WfOn, 1}, {F2 + WfType, FltPeak},
                        {F2 + WfCutoff, 900}, {F2 + WfReso, 0.5f}, {wtLfo(0, WlSync), 1}, {wtLfo(0, WlDiv), Div1_4}},
                       mx(0, WsLfo1, WdAPos, 0.9f), mx(1, WsLfo1, WdF2Cutoff, 0.3f)}));
  p.editorWidth = 1120;
  return p;
}

}  // namespace

PluginInfo matrixFmInfo();  // matrixfm_info.cpp
PluginInfo gmsInfo();       // gms_info.cpp
void appendVaInfos(std::vector<PluginInfo>& v);   // va_info.cpp (J-22)
void appendDcoInfos(std::vector<PluginInfo>& v);  // dco_info.cpp (J-22)
PluginInfo hexSynthInfo();                        // hex_info.cpp (J-33)
PluginInfo pocketSynthInfo();                     // pocket_info.cpp (J-33)
PluginInfo pixelToneInfo();                       // pixel_info.cpp (J-33)
PluginInfo sampleKeysInfo();                      // samplekeys_info.cpp (J-33)

void appendInfos(std::vector<PluginInfo>& v) {
  v.push_back(analogInfo());
  v.push_back(wavetableInfo());
  v.push_back(matrixFmInfo());
  v.push_back(gmsInfo());
  appendVaInfos(v);
  appendDcoInfos(v);
  v.push_back(hexSynthInfo());
  v.push_back(pocketSynthInfo());
  v.push_back(pixelToneInfo());
  v.push_back(sampleKeysInfo());
}

}  // namespace gl::packs::synths
