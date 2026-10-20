// Gridline — "synths" pack: Matrix FM catalogue entry (parameters, presets).
#include <string>
#include <utility>
#include "../Packs.h"
#include "matrixfm.h"

namespace gl::packs::synths {

namespace {

using namespace mfm;

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
ParamDesc steps(const std::string& n, double mn, double mx, double d) {
  return ParamDesc{n, "", mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}

const char* const kRate = "Hz ";

std::string unitName(int u) {
  if (u < kOps) return "Op " + std::to_string(u + 1);
  if (u < kOps + kFilters) return "Filter " + std::to_string(u - kOps + 1);
  return "Main";
}
std::string artName(int u, int a) {
  const char* t = u < kOps ? kOpArtNames[a] : u < kOps + kFilters ? kFltArtNames[a] : kMainArtNames[a];
  return unitName(u) + " " + t + " ";
}

void artParams(std::vector<ParamDesc>& q, int u) {
  for (int a = 0; a < unitArts(u); ++a) {
    const std::string n = artName(u, a);
    q[static_cast<size_t>(art(u, a, AfEnvOn))] = tog(n + "envelope", 0);
    q[static_cast<size_t>(art(u, a, AfLfoOn))] = tog(n + "LFO", 0);
    q[static_cast<size_t>(art(u, a, AfAmount))] = lin(n + "depth", "%", -1, 1, 1);
    q[static_cast<size_t>(art(u, a, AfLfoAmt))] = lin(n + "LFO depth", "%", 0, 1, 1);
    q[static_cast<size_t>(art(u, a, AfAtt))] = lin(n + "attack scale", "x", 0, 2, 1);
    q[static_cast<size_t>(art(u, a, AfDec))] = lin(n + "decay scale", "x", 0, 2, 1);
    q[static_cast<size_t>(art(u, a, AfSus))] = lin(n + "sustain offset", "%", -1, 1, 0);
    q[static_cast<size_t>(art(u, a, AfRel))] = lin(n + "release scale", "x", 0, 2, 1);
    q[static_cast<size_t>(art(u, a, AfLfoSpeed))] = lg(n + "LFO speed", kRate, 0.01, 50, 2);
    q[static_cast<size_t>(art(u, a, AfLfoSkew))] = lin(n + "LFO skew", "%", -1, 1, 0);
    q[static_cast<size_t>(art(u, a, AfTempo))] = tog(n + "LFO tempo sync", 0);
    q[static_cast<size_t>(art(u, a, AfLfoGlobal))] = tog(n + "LFO global", 0);
  }
}

using Values = std::vector<std::pair<int, float>>;

}  // namespace

PluginInfo matrixFmInfo() {
  PluginInfo p{"gl.matrixfm", "Matrix FM", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(kParamCountAll);
  for (int i = 0; i < kParamCount; ++i) q[static_cast<size_t>(i)] = lin("Reserved", "", 0, 1, 0);
  q[MModel] = ch("Engine", {"Legacy", "Reference"}, 0);  // imported patches select the reference model
  p.legacyModelParam = MModel;
  q[MVolume] = lin("Master volume", "%", 0, 1.25, 1);
  q[MLfoLevel] = lin("Master LFO level", "%", -1, 1, 1);
  q[MPitch] = lin("Master pitch", "st", -24, 24, 0);
  q[MVolA] = lin("Volume attack scale", "x", 0, 2, 1);
  q[MVolD] = lin("Volume decay scale", "x", 0, 2, 1);
  q[MVolS] = lin("Volume sustain offset", "%", -1, 1, 0);
  q[MVolR] = lin("Volume release scale", "x", 0, 2, 1);
  q[MFltA] = lin("Filter attack scale", "x", 0, 2, 1);
  q[MFltD] = lin("Filter decay scale", "x", 0, 2, 1);
  q[MFltS] = lin("Filter sustain offset", "%", -1, 1, 0);
  q[MFltR] = lin("Filter release scale", "x", 0, 2, 1);
  q[MUniOrder] = steps("Unison voices", 1, 9, 1);
  q[MUniPan] = lin("Unison spread", "%", 0, 1, 1);
  q[MUniVol] = lin("Unison volume", "%", 0.125, 1, 0.78125);
  q[MUniPitch] = lin("Unison detune", "%", 0, 1, 0.5);
  q[MUniSub] = lin("Unison sub level", "%", 0, 0.75, 0);
  q[MUniPhase] = lin("Unison phase", "%", 0, 1, 0.125);
  q[MUniEnv] = lin("Unison envelope variation", "%", 0, 1.5, 0);
  q[MModX] = lin("Mod X", "%", 0, 1, 0.5);
  q[MModY] = lin("Mod Y", "%", 0, 1, 0.5);
  q[MPorta] = tog("Portamento", 0);
  q[MPortaTime] = lg("Portamento time", "s", 0.001, 2, 0.08);
  q[MVoices] = steps("Polyphony", 1, 32, 16);
  q[MMonoKey] = tog("One voice per key", 0);
  q[MGlobalPitch] = tog("Op 1 pitch articulator is global", 0);
  q[MSoften] = lin("Soften low velocities", "%", 0, 1, 0);
  q[MCenter] = tog("Remove DC offset", 0);
  q[MGibbs] = tog("Attenuate ringing", 0);
  q[MRandom] = tog("Randomness", 1);
  q[MSmooth] = tog("Smooth modulation", 0);
  q[MEqTarget] = ch("EQ target", {"Off", "Out + FX", "Out", "FX"}, 0);
  for (int b = 0; b < 3; ++b) {
    const std::string n = "EQ " + std::to_string(b + 1) + " ";
    q[static_cast<size_t>(eq(b, 0))] = ch(n + "type", {"Low shelf", "Peaking", "High shelf"}, b);
    q[static_cast<size_t>(eq(b, 1))] = lg(n + "frequency", "Hz", 20, 20000, b == 0 ? 120 : b == 1 ? 1000 : 6000);
    q[static_cast<size_t>(eq(b, 2))] = lin(n + "bandwidth", "oct", 0.1, 4, 1);
    q[static_cast<size_t>(eq(b, 3))] = lin(n + "gain", "dB", -18, 18, 0);
  }
  q[FxPan] = lin("Effects pan", "pan", -1, 1, 0);
  q[ChOrder] = steps("Chorus voices", 0, 9, 0);
  q[ChDepth] = lin("Chorus depth", "%", 0, 1, 0.5);
  q[ChSpeed] = lin("Chorus speed", "%", 0, 1, 0.75);
  q[ChDelay] = lin("Chorus delay", "%", 0, 1, 0.0625);
  q[ChSpread] = lin("Chorus spread", "%", 0, 1, 1);
  q[ChCross] = lin("Chorus cross", "%", -1, 1, -0.25);
  q[ChVol] = lin("Chorus volume", "%", -1, 1, 1);
  q[DlSerial] = tog("Delays in series", 0);
  for (int d = 0; d < 3; ++d) {
    const std::string n = "Delay " + std::to_string(d + 1) + " ";
    q[static_cast<size_t>(dly(d, 0))] = tog(n + "on", 0);
    q[static_cast<size_t>(dly(d, 1))] = ch(n + "feedback mode", {"Normal", "Inverted", "Ping-pong"}, 0);
    q[static_cast<size_t>(dly(d, 2))] = lin(n + "feedback", "%", 0, 0.99, 0.5);
    q[static_cast<size_t>(dly(d, 3))] = lin(n + "time", "beats", 0.01, 4, 0.75);
    q[static_cast<size_t>(dly(d, 4))] = lin(n + "stereo offset", "%", -1, 1, 0);
    q[static_cast<size_t>(dly(d, 5))] = lin(n + "volume", "%", -1, 1, 0.5);
    q[static_cast<size_t>(dly(d, 6))] = tog(n + "tempo sync", 1);
  }
  q[RvOn] = tog("Reverb", 0);
  q[RvVol] = lin("Reverb wet", "%", -1, 1, 0.2);
  q[RvHighDamp] = lg("Reverb high damping", "Hz", 500, 22000, 4000);
  q[RvDecay] = lg("Reverb decay", "s", 0.1, 20, 1.5);
  q[RvDiff] = lin("Reverb diffusion", "%", 0, 1, 1);
  q[RvRoom] = lin("Reverb room size", "%", 0.01, 1, 0.4);
  q[RvPreDelay] = lin("Reverb predelay", "s", 0, 0.5, 0);
  q[RvHighCut] = lg("Reverb high cut", "Hz", 500, 22000, 4000);
  q[RvLowCut] = lg("Reverb low cut", "Hz", 19, 3000, 75);
  q[RvColor] = ch("Reverb colour", {"Brighter", "Bright", "Flat", "Warm", "Warmer"}, 2);
  q[RvTempo] = tog("Reverb tempo predelay", 0);
  artParams(q, kUnits - 1);
  for (int r = 0; r < kRows; ++r)
    for (int c = 0; c < kCols; ++c) {
      const std::string row = r < kOps ? "op " + std::to_string(r + 1) : "filter " + std::to_string(r - kOps + 1);
      const std::string col = c < kOps ? "op " + std::to_string(c + 1) : c == kColPan ? "pan" : c == kColFx ? "effects" : "output";
      std::string nf, nr;
      if (c < kOps && r < kOps) {
        nf = "Matrix FM op " + std::to_string(c + 1) + " to " + row;
        nr = "Matrix RM op " + std::to_string(c + 1) + " to " + row;
      } else if (c < kOps) {
        nf = "Matrix op " + std::to_string(c + 1) + " to " + row;
        nr = "Matrix RM (unused) " + row + " " + col;
      } else {
        nf = "Matrix " + row + " " + col;
        nr = "Matrix RM " + row + " " + col;
      }
      q[static_cast<size_t>(fm(r, c))] = lin(nf, "%", -1, 1, 0);
      q[static_cast<size_t>(rm(r, c))] = lin(nr, "%", -1, 1, 0);
    }
  for (int k = 0; k < kOps; ++k) {
    const std::string n = "Op " + std::to_string(k + 1) + " ";
    q[static_cast<size_t>(op(k, OpPhase))] = lin(n + "start phase", "%", 0, 1, 0);
    q[static_cast<size_t>(op(k, OpVol))] = lin(n + "volume", "%", -1, 1, 1);
    q[static_cast<size_t>(op(k, OpPitchRange))] = lin(n + "pitch envelope range", "st", 0, 48, 12);
    q[static_cast<size_t>(op(k, OpRatio))] = lin(n + "ratio", "x", 0, 32, 1);
    q[static_cast<size_t>(op(k, OpFreq))] = lin(n + "frequency offset", "Hz", -5000, 5000, 0);
    q[static_cast<size_t>(op(k, OpNoise))] = lin(n + "noise", "%", 0, 1, 0);
    q[static_cast<size_t>(op(k, OpShape))] = lin(n + "shape", "%", 0, 2, 0);
    q[static_cast<size_t>(op(k, OpTension))] = lin(n + "tension", "%", -1, 1, 0);
    q[static_cast<size_t>(op(k, OpSkew))] = lin(n + "skew", "%", 0, 1, 0);
    q[static_cast<size_t>(op(k, OpSine))] = lin(n + "sine shaper", "%", 0, 1, 0);
    q[static_cast<size_t>(op(k, OpFilter))] = lin(n + "pre-filter", "%", 0, 1, 1);
    q[static_cast<size_t>(op(k, OpHalf))] = tog(n + "half wave", 0);
    q[static_cast<size_t>(op(k, OpEven))] = tog(n + "even cycles silent", 0);
    q[static_cast<size_t>(op(k, OpAbs))] = tog(n + "absolute", 0);
    q[static_cast<size_t>(op(k, OpGlobal))] = tog(n + "free-running phase", 0);
    q[static_cast<size_t>(op(k, OpCenter))] = tog(n + "remove DC", 0);
    q[static_cast<size_t>(op(k, OpDeclick))] = tog(n + "declick", 0);
    q[static_cast<size_t>(op(k, OpPluck))] = tog(n + "plucked", 0);
    q[static_cast<size_t>(op(k, OpBandLimit))] = tog(n + "band limit", 1);
    artParams(q, k);
  }
  std::vector<std::string> types(kFltTypeNames, kFltTypeNames + kFltTypes), modes(kFltModeNames, kFltModeNames + kFltModes);
  for (int j = 0; j < kFilters; ++j) {
    const std::string n = "Filter " + std::to_string(j + 1) + " ";
    q[static_cast<size_t>(flt(j, FlMode))] = ch(n + "mode", modes, FmX1);
    q[static_cast<size_t>(flt(j, FlType))] = ch(n + "type", types, FtLp);
    q[static_cast<size_t>(flt(j, FlEnv))] = lin(n + "cutoff articulator amount", "x", 0, 2, 1);
    q[static_cast<size_t>(flt(j, FlCut))] = lin(n + "cutoff", "%", 0, 1, 0.5);
    q[static_cast<size_t>(flt(j, FlRes))] = lin(n + "resonance", "%", 0, 1, 0);
    q[static_cast<size_t>(flt(j, FlLow))] = lin(n + "low / gain / drive", "%", 0, 1, 1);
    q[static_cast<size_t>(flt(j, FlBand))] = lin(n + "band", "%", 0, 1, 0);
    q[static_cast<size_t>(flt(j, FlHigh))] = lin(n + "high", "%", 0, 1, 0);
    q[static_cast<size_t>(flt(j, FlWsAmp))] = lin(n + "shaper pre-amp", "%", 0.5, 1.5, 1);
    q[static_cast<size_t>(flt(j, FlWsMix))] = lin(n + "shaper mix", "%", 0, 1, 1);
    q[static_cast<size_t>(flt(j, FlNext))] = lin(n + "to next filter", "%", -1, 1, 0);
    q[static_cast<size_t>(flt(j, FlHq))] = tog(n + "oversample", 0);
    q[static_cast<size_t>(flt(j, FlFlat))] = tog(n + "alternate resonance", 0);
    q[static_cast<size_t>(flt(j, FlWsOn))] = tog(n + "shaper on", 0);
    q[static_cast<size_t>(flt(j, FlWsBipolar))] = tog(n + "shaper bipolar", 0);
    artParams(q, kOps + j);
  }
  // Defaults: operator 1 to the output, the main volume envelope on.
  const int M = kUnits - 1;
  q[static_cast<size_t>(fm(0, kColOut))].def = 1;
  q[static_cast<size_t>(art(M, MaVol, AfEnvOn))].def = 1;

  auto P = [&](const char* name, Values v) { p.presets.push_back({name, std::move(v)}); };
  auto envOn = [](int u, int a) { return std::pair<int, float>{art(u, a, AfEnvOn), 1.f}; };
  // Two-operator electric piano: op 2 (x14) strikes op 1; op 3 + op 4 add a soft body.
  P("Tine Keys", {{op(1, OpRatio), 14}, {fm(0, 1), 0.22f}, envOn(1, OaVol), {art(1, OaVol, AfDec), 0.8f}, {art(1, OaVol, AfSus), -0.65f},
                  {fm(2, kColOut), 0.5f}, {op(2, OpRatio), 1}, {fm(2, 3), 0.12f}, {op(3, OpRatio), 1}, {fm(0, kColOut), 0.8f},
                  {art(M, MaVol, AfDec), 1.3f}, {art(M, MaVol, AfSus), -0.6f}, {art(M, MaVol, AfRel), 1.15f}, {MVolume, 0.9f}});
  // FM bass with a decaying index and feedback.
  P("Fold Bass", {{op(1, OpRatio), 1}, {fm(0, 1), 0.45f}, {fm(1, 1), 0.2f}, envOn(1, OaVol), {art(1, OaVol, AfDec), 0.85f},
                  {art(1, OaVol, AfSus), -0.6f}, {MPitch, -12}, {MPorta, 1}, {MVoices, 1}, {art(M, MaVol, AfRel), 0.85f}});
  // Bell: inharmonic ratio, long decay.
  P("Glass Bell", {{op(1, OpRatio), 3.5f}, {fm(0, 1), 0.3f}, envOn(1, OaVol), {art(1, OaVol, AfDec), 1.3f}, {art(1, OaVol, AfSus), -0.7f},
                   {op(2, OpRatio), 2}, {op(2, OpFreq), 1.5f}, {fm(2, kColOut), 0.35f}, {art(M, MaVol, AfDec), 1.45f},
                   {art(M, MaVol, AfSus), -0.7f}, {art(M, MaVol, AfRel), 1.3f}, {fm(0, kColFx), 0.5f}, {RvOn, 1}, {RvVol, 0.4f}, {MVolume, 0.55f}});
  // Subtractive: saw through filter 1, cutoff articulator envelope.
  P("Saw Filter Lead", {{op(0, OpShape), 1}, {fm(0, kColOut), 0}, {fm(6, 0), 1}, {fm(6, kColOut), 1}, {flt(0, FlType), FtLp},
                        {flt(0, FlMode), FmX2}, {flt(0, FlCut), 0.55f}, {flt(0, FlRes), 0.35f}, {flt(0, FlEnv), 0.8f},
                        envOn(kOps, FaCut), {MUniOrder, 3}, {MUniPitch, 0.35f}, {MVolume, 0.8f}});
  // Pad: two detuned saws, slow attack, chorus and reverb.
  P("Wide Pad", {{op(0, OpShape), 1}, {op(1, OpShape), 1}, {op(1, OpFreq), 0.7f}, {fm(1, kColOut), 0.8f}, {fm(0, kColOut), 0.8f},
                 {fm(0, kColPan), -0.5f}, {fm(1, kColPan), 0.5f}, {fm(0, kColFx), 0.6f}, {fm(1, kColFx), 0.6f},
                 {art(M, MaVol, AfAtt), 2}, {art(M, MaVol, AfRel), 1.4f}, {ChOrder, 4}, {RvOn, 1}, {RvVol, 0.3f}, {MVolume, 0.7f}});
  p.editorWidth = 1180;
  return p;
}

}  // namespace gl::packs::synths
