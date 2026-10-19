// Gridline — "synths" pack, J-22 group "va": catalogue entries and factory
// presets of Twin VA, Saw Swarm, and Finger Bass. Parameter index =
// the reference control tag; each value is linear in the reference control's
// normalized position (unit "%" = 0..1, bipolar knobs −1..1, pitch in
// semitones / cents, selectors as choices).
#include <string>
#include <utility>
#include <vector>
#include "../Packs.h"
#include "va.h"

namespace gl::packs::synths {

namespace {

using namespace va;

ParamDesc pct(const std::string& n, double d) { return ParamDesc{n, "%", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
ParamDesc bip(const std::string& n, double d) { return ParamDesc{n, "%", -1, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }
ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  const double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
ParamDesc num(const std::string& n, double mn, double mx, double d) {
  return ParamDesc{n, "", mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}

PluginInfo twinVaInfo() {
  PluginInfo p{"gl.twinva", "Twin VA", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(TwCount);
  const std::vector<std::string> shapes = {"Saw", "Pulse"};
  const std::vector<std::string> dest5 = {"Cutoff", "Pitch A", "Pitch A+B", "Width A", "Width A+B"};
  q[TwAShape] = ch("Osc A shape", shapes, 0);
  q[TwAPw] = pct("Osc A width", 1);
  q[TwSync] = tog("Osc B sync", false);
  q[TwRing] = tog("Osc B ring", false);
  q[TwBShape] = ch("Osc B shape", shapes, 0);
  q[TwBPw] = pct("Osc B width", 1);
  q[TwBPitch] = lin("Osc B pitch", "st", -24, 24, 0);
  q[TwBDetune] = lin("Osc B detune", "ct", 0, 50, 0);
  q[TwBalance] = pct("Osc balance", 0);
  q[TwNoise] = pct("Noise balance", 0);
  q[TwEgAmt] = bip("Env amount", 0);
  q[TwEgA] = pct("Env attack", 0);
  q[TwEgD] = pct("Env decay", 1);
  q[TwEgS] = pct("Env sustain", 1);
  q[TwEgR] = pct("Env release", 1);
  q[TwGain] = lin("Gain", "dB", -20, 10, -5);
  q[TwFine] = lin("Fine", "ct", -100, 100, 0);
  q[TwTranspose] = lin("Transpose", "st", -24, 24, 0);
  q[TwCutoff] = pct("Cutoff", 1);
  q[TwReso] = pct("Resonance", 0);
  q[TwKeyTrk] = pct("Key tracking", 0);
  q[TwVelTrk] = pct("Velocity tracking", 0);
  q[TwAmpA] = pct("Amp attack", 0);
  q[TwAmpD] = pct("Amp decay", 1);
  q[TwAmpS] = pct("Amp sustain", 1);
  q[TwAmpR] = pct("Amp release", 0);
  q[TwLfoAmt] = bip("LFO amount", 0);
  q[TwLfoRetrig] = tog("LFO retrigger", false);
  q[TwLfoRate] = pct("LFO rate", 0.2);
  q[TwUniDetune] = pct("Unison detune", 0.1);
  q[TwUniPan] = pct("Unison spread", 0);
  q[TwDlyOn] = tog("Delay", false);
  q[TwDlyL] = pct("Delay left", 3.0 / 7.0);
  q[TwDlyR] = pct("Delay right", 3.0 / 7.0);
  q[TwDlyFb] = pct("Delay feedback", 0.5);
  q[TwDlyDepth] = pct("Delay depth", 0.3);
  q[TwDlyLoCut] = pct("Delay low cut", 0);
  q[TwDlyHiCut] = pct("Delay high cut", 1);
  q[TwDlyWet] = pct("Delay wet", 0.5);
  q[TwChoOn] = tog("Chorus", false);
  q[TwChoRate] = pct("Chorus rate", 0.5);
  q[TwChoDepth] = pct("Chorus depth", 0.5);
  q[TwChoWet] = pct("Chorus wet", 0.4);
  q[TwMwAmt] = bip("Wheel amount", 0);
  q[TwMono] = tog("Mono", false);
  q[TwOctaver] = tog("Octaver", false);
  q[TwGlide] = pct("Glide", 0);
  q[TwGateMix] = pct("Gate mix", 0);
  q[TwGateSmooth] = pct("Gate smooth", 0);
  for (int k = 0; k < kTwGateSteps; ++k) q[TwGate1 + k] = tog("Gate step " + std::to_string(k + 1), k % 2 == 0);
  q[TwEgDest] = ch("Env target", dest5, TdCutoff);
  q[TwFltMode] = ch("Filter mode", {"Low pass", "Band pass", "High pass"}, TfLow);
  q[TwLfoDest] = ch("LFO target", {"Cutoff", "Pitch A", "Pitch A+B", "Width A", "Width A+B", "Pan"}, TdCutoff);
  q[TwLfoShape] = ch("LFO shape", {"Saw", "Pulse", "Triangle", "Sine", "Random"}, TlTriangle);
  q[TwArpMode] = ch("Arpeggiator", {"Off", "Up", "Down", "Up + down", "Random"}, TaOff);
  q[TwArpRange] = ch("Arp range", {"1 oct", "2 oct", "3 oct", "4 oct"}, 0);
  q[TwMwDest] = ch("Wheel target", {"Cutoff", "LFO amount", "Pitch A", "Width A", "Width A+B"}, TmCutoff);
  q[TwGlideHeld] = tog("Glide held notes", false);
  q[TwGlideStatic] = tog("Glide constant time", false);
  q[TwModWheel] = pct("Mod wheel", 0);
  q[TwUnison] = num("Unison", 0, 6, 0);
  q[TwPoly] = num("Polyphony", 0, 63, 15);
  q[TwArpSync] = num("Arp step", 0, 6, 1);
  q[TwBendRange] = num("Bend range", 0, 11, 1);
  q[TwGateSync] = num("Gate step", 0, 6, 0);
  q[TwLfoSync] = num("LFO sync", 0, 10, 0);
  // Integer wheels are stored as raw indices (the reference's digit wheels).
  for (int k : {TwUnison, TwPoly, TwArpSync, TwBendRange, TwGateSync, TwLfoSync}) q[static_cast<size_t>(k)].kind = ParamKind::Continuous;
  using V = std::vector<std::pair<int, float>>;
  auto preset = [&](const char* n, V v) { p.presets.push_back({n, std::move(v)}); };
  preset("Init", {});
  preset("Rubber Bass", {{TwAShape, 1}, {TwAPw, 0.42f}, {TwBShape, 1}, {TwBPw, 0.25f}, {TwBalance, 0.5f}, {TwEgAmt, 0.64f}, {TwEgD, 0.55f},
                         {TwEgS, 0.2f}, {TwEgR, 0.38f}, {TwCutoff, 0.12f}, {TwReso, 0.2f}, {TwAmpD, 0.65f}, {TwAmpS, 0.8f}, {TwAmpR, 0.3f},
                         {TwTranspose, -12}, {TwMono, 1}, {TwGlide, 0.18f}, {TwGlideHeld, 1}});
  preset("Gated Choir", {{TwBalance, 0.5f}, {TwBPitch, 7}, {TwBDetune, 8}, {TwUnison, 3}, {TwUniDetune, 0.3f},
                         {TwUniPan, 0.7f}, {TwCutoff, 0.55f}, {TwAmpA, 0.3f}, {TwAmpR, 0.5f}, {TwGateMix, 1}, {TwGateSmooth, 0.2f},
                         {TwChoOn, 1}, {TwDlyOn, 1}, {TwDlyWet, 0.3f}});
  preset("Sync Lead", {{TwSync, 1}, {TwBalance, 1}, {TwBPitch, 12}, {TwEgDest, TdFreqAB}, {TwEgAmt, 0.4f}, {TwEgD, 0.5f}, {TwEgS, 0},
                       {TwCutoff, 0.75f}, {TwReso, 0.3f}, {TwMono, 1}, {TwGlide, 0.15f}, {TwLfoDest, TdFreqA}, {TwLfoShape, TlSine},
                       {TwLfoAmt, 0.04f}, {TwLfoRate, 0.55f}, {TwDlyOn, 1}, {TwDlyWet, 0.25f}});
  preset("Arp Pluck", {{TwAShape, 1}, {TwAPw, 0.7f}, {TwEgAmt, 0.7f}, {TwEgD, 0.4f}, {TwEgS, 0}, {TwCutoff, 0.2f}, {TwReso, 0.4f},
                       {TwAmpD, 0.45f}, {TwAmpS, 0}, {TwAmpR, 0.3f}, {TwArpMode, TaUpDown}, {TwArpRange, 1}, {TwArpSync, 0},
                       {TwDlyOn, 1}, {TwDlyWet, 0.35f}});
  p.editorWidth = 855;
  return p;
}

PluginInfo sawSwarmInfo() {
  PluginInfo p{"gl.sawswarm", "Saw Swarm", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(SwCount);
  q[SwLevel] = lin("Level", "dB", -24, 24, 0);
  q[SwPan] = bip("Pan", 0);
  q[SwAmpA] = pct("Attack", 0);
  q[SwAmpD] = pct("Decay", 1);
  q[SwAmpS] = pct("Sustain", 1);
  q[SwAmpR] = pct("Release", 0);
  q[SwTranspose] = lin("Transpose", "st", -24, 24, 0);
  q[SwFine] = lin("Fine", "ct", -100, 100, 0);
  q[SwMono] = tog("Mono", false);
  q[SwGlideStatic] = tog("Glide static time", false);
  q[SwGlideHeld] = tog("Glide held notes", false);
  q[SwGlide] = pct("Glide", 0);
  q[SwOctaver] = tog("Octaver", false);
  q[SwUniRetrig] = tog("Unison restart", false);
  q[SwUnison] = ch("Unison", {"Off", "2 voices", "4 voices", "8 voices"}, 0);
  q[SwUniDetune] = pct("Unison detune", 0.25);
  q[SwUniPan] = pct("Unison spread", 0);
  q[SwSubPhase] = pct("Sub phase", 1);
  q[SwSubOct] = ch("Sub octave", {"-2 oct", "-1.5 oct", "-1 oct", "-0.5 oct", "0 oct", "+0.5 oct", "+1 oct", "+1.5 oct", "+2 oct"}, 4);
  q[SwSubLevel] = pct("Sub level", 0);
  q[SwSubDetune] = pct("Sub detune", 0);
  q[SwSubRetrig] = tog("Sub restart", false);
  q[SwSubInvert] = tog("Sub invert", false);
  q[SwSubSquare] = tog("Sub square", false);
  q[SwSyncFreq] = pct("Sync frequency", 0);
  q[SwSyncOn] = tog("Sync", false);
  q[SwNoise] = pct("Noise", 0);
  q[SwCutoff] = pct("Cutoff", 1);
  q[SwReso] = pct("Resonance", 0);
  q[SwKeyTrk] = pct("Key tracking", 0);
  q[SwVelTrk] = pct("Velocity tracking", 0);
  q[SwFltMode] = ch("Filter mode", {"LP24", "LP12", "BP", "HP"}, SfLp24);
  q[SwEgA] = pct("Filter attack", 0);
  q[SwEgD] = pct("Filter decay", 1);
  q[SwEgS] = pct("Filter sustain", 1);
  q[SwEgR] = pct("Filter release", 1);
  q[SwEgAmt] = pct("Filter env amount", 0);
  q[SwEgDest] = ch("Filter env target", {"Cutoff", "Sync frequency", "Sub phase", "Master pitch"}, SeCutoff);
  q[SwLfoShape] = ch("LFO shape", {"Triangle", "Saw", "Square", "Random"}, SsTriangle);
  q[SwLfoSpeed] = pct("LFO speed", 0.5);
  q[SwLfoSync] = tog("LFO tempo sync", false);
  q[SwLfoAttack] = pct("LFO attack", 0);
  q[SwLfoRelease] = pct("LFO release", 1);
  q[SwLfoRetrig] = tog("LFO retrigger", false);
  q[SwLfoInvert] = tog("LFO invert", false);
  q[SwLfoAmt] = pct("LFO amount", 0);
  q[SwLfoDest] = ch("LFO target", {"Cutoff", "Sync frequency", "Sub phase", "Master pitch", "Noise", "Level", "Pan"}, SlCutoff);
  q[SwChoOn] = tog("Chorus", false);
  q[SwChoDepth] = pct("Chorus depth", 0.5);
  q[SwChoRate] = pct("Chorus rate", 0.25);
  q[SwChoMix] = pct("Chorus mix", 0.3);
  q[SwPhOn] = tog("Phaser", false);
  q[SwPhCenter] = pct("Phaser centre", 0.3);
  q[SwPhRate] = pct("Phaser rate", 0.2);
  q[SwPhDepth] = pct("Phaser depth", 0.7);
  q[SwPhFb] = pct("Phaser feedback", 0);
  q[SwPhMix] = pct("Phaser mix", 0.6);
  q[SwDlyOn] = tog("Delay", false);
  q[SwDlyL] = pct("Delay left", 0.4);
  q[SwDlyR] = pct("Delay right", 0.4);
  q[SwDlyFb] = pct("Delay feedback", 0.3);
  q[SwDlyDepth] = pct("Delay depth", 0.25);
  q[SwDlyLoCut] = pct("Delay low cut", 0);
  q[SwDlyHiCut] = pct("Delay high cut", 1);
  q[SwDlyMix] = pct("Delay mix", 0.5);
  q[SwRevOn] = tog("Reverb", false);
  q[SwRevDecay] = pct("Reverb decay", 0.5);
  q[SwRevHiCut] = pct("Reverb high cut", 0.5);
  q[SwRevDamp] = pct("Reverb damp", 0.5);
  q[SwRevMix] = pct("Reverb mix", 0.5);
  q[SwArpOn] = tog("Arpeggiator", false);
  q[SwArpMode] = ch("Arp mode", {"Up", "Down", "Up + down", "Random"}, SaUp);
  q[SwArpRange] = ch("Arp range", {"1 oct", "2 oct", "3 oct", "4 oct"}, 0);
  q[SwModWheel] = pct("Mod wheel", 0);
  q[SwArpSync] = ch("Arp step", {"1/32", "1/16", "2/16", "3/16", "1/4", "2/4", "3/4"}, 1);
  q[SwPoly] = ch("Polyphony", {"4", "8", "12", "16", "20", "24"}, 1);
  q[SwBendRange] = ch("Bend range", {"Off", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "12"}, 2);
  q[SwSpare] = pct("(unused)", 0);
  std::vector<std::string> srcs = {"None", "Velocity", "Aftertouch", "Pitch wheel"};
  for (int c : kSwCc) srcs.push_back("CC " + std::to_string(c));
  // Matrix targets: any parameter before the matrix (its name).
  std::vector<std::string> targets;
  for (int k = 0; k < SwModAmt1; ++k) targets.push_back(q[static_cast<size_t>(k)].name);
  for (int m = 0; m < kSwModSlots; ++m) {
    const std::string s = "Mod " + std::to_string(m + 1) + " ";
    q[SwModAmt1 + m] = bip(s + "amount", 0);
    q[SwModDest1 + m] = ch(s + "target", targets, 0);
    q[SwModSrc1 + m] = ch(s + "source", srcs, 0);
  }
  using V = std::vector<std::pair<int, float>>;
  auto preset = [&](const char* n, V v) { p.presets.push_back({n, std::move(v)}); };
  preset("Init", {});
  preset("Swarm Lead", {{SwUnison, 3}, {SwUniDetune, 0.45f}, {SwUniPan, 0.8f}, {SwCutoff, 0.62f}, {SwReso, 0.15f}, {SwMono, 1},
                        {SwGlide, 0.2f}, {SwChoOn, 1}, {SwDlyOn, 1}, {SwDlyMix, 0.3f}, {SwRevOn, 1}, {SwRevMix, 0.25f}});
  preset("Twin Saw Bass", {{SwSubLevel, 1}, {SwSubOct, 2}, {SwSubPhase, 0.25f}, {SwCutoff, 0.18f}, {SwEgAmt, 0.6f}, {SwEgD, 0.4f},
                           {SwEgS, 0.1f}, {SwAmpR, 0.2f}, {SwTranspose, -12}, {SwMono, 1}});
  preset("Sync Sweep", {{SwSyncOn, 1}, {SwSyncFreq, 0.3f}, {SwEgDest, SeSyncFreq}, {SwEgAmt, 0.6f}, {SwEgD, 0.6f}, {SwEgS, 0.2f},
                        {SwLfoDest, SlSyncFreq}, {SwLfoAmt, 0.15f}, {SwLfoSpeed, 0.45f}, {SwPhOn, 1}});
  preset("Wide Pad", {{SwUnison, 3}, {SwUniDetune, 0.35f}, {SwUniPan, 1}, {SwAmpA, 0.3f}, {SwAmpR, 0.6f}, {SwCutoff, 0.45f},
                      {SwLfoAmt, 0.12f}, {SwLfoSpeed, 0.3f}, {SwChoOn, 1}, {SwRevOn, 1}, {SwRevMix, 0.45f}, {SwRevDecay, 0.75f},
                      {SwModSrc1, 5}, {SwModDest1, SwCutoff}, {SwModAmt1, 0.5f}});
  p.editorWidth = 897;
  return p;
}

PluginInfo fingerBassInfo() {
  PluginInfo p{"gl.fingerbass", "Finger Bass", "Synth", PluginKind::Generator, {}, false, {}};
  p.params.resize(BbCount);
  p.params[BbBass] = pct("Bass", 0.5);
  p.params[BbMid] = pct("Mid", 0.5);
  p.params[BbTreble] = pct("Treble", 0.5);
  p.presets.push_back({"Flat", {}});
  p.presets.push_back({"Deep Round", {{BbBass, 0.8f}, {BbMid, 0.4f}, {BbTreble, 0.25f}}});
  p.presets.push_back({"Punchy Mid", {{BbBass, 0.55f}, {BbMid, 0.8f}, {BbTreble, 0.6f}}});
  p.presets.push_back({"Bright Slap", {{BbBass, 0.6f}, {BbMid, 0.3f}, {BbTreble, 0.95f}}});
  p.editorWidth = 340;
  return p;
}

}  // namespace

void appendVaInfos(std::vector<PluginInfo>& v) {
  v.push_back(twinVaInfo());
  v.push_back(sawSwarmInfo());
  v.push_back(fingerBassInfo());
}

}  // namespace gl::packs::synths
