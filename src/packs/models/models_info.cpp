// Gridline — "models" pack: catalogue entries and factory presets.
#include <algorithm>
#include <cmath>
#include <string>
#include "../Packs.h"
#include "models.h"

namespace gl::packs::models {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, bool d) { return ParamDesc{n, "", 0, 1, d ? 1.0 : 0.0, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, int d) {
  double hi = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, hi, static_cast<double>(d), ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
using Ov = std::vector<std::pair<int, float>>;

void fxParams(std::vector<ParamDesc>& p, int base) {
  p[static_cast<size_t>(base + FxChorus)] = lin("Chorus", "%", 0, 1, 0);
  p[static_cast<size_t>(base + FxDelay)] = lin("Delay", "%", 0, 1, 0);
  p[static_cast<size_t>(base + FxDelayTime)] = lg("Delay time", "s", 0.02, 1.5, 0.3);
  p[static_cast<size_t>(base + FxDelayFeedback)] = lin("Feedback", "%", 0, 0.9, 0.35);
  p[static_cast<size_t>(base + FxReverb)] = lin("Reverb", "%", 0, 1, 0);
  p[static_cast<size_t>(base + FxRoomSize)] = lin("Room", "%", 0, 1, 0.6);
}

// ------------------------------------------------------------------ Additive
PluginInfo additive() {
  PluginInfo p{"gl.additive", "Additive", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(AdCount);
  std::vector<std::string> spectra = {"Saw", "Square", "Triangle", "Sine", "Pulse", "Organ", "Vowel", "Glass", "Choir", "Random",
                                      "Bright", "Brighter", "Scattered", "Flat phase", "Square-saw", "Square-saw 2", "Soft",
                                      "Saw + oct", "Saw stack", "Saw stack 2", "Custom"};
  q[AdSpectrumA] = ch("Timbre A", spectra, SpSaw);
  q[AdSpectrumB] = ch("Timbre B", spectra, SpSquare);
  q[AdMorph] = lin("Morph", "%", 0, 1, 0);
  q[AdMorphEnv] = lin("Morph env", "", -1, 1, 0);
  q[AdPartials] = lin("Partials", "", 1, 256, 128);
  q[AdTilt] = lin("Tilt", "dB", -12, 12, 0);
  q[AdOddEven] = lin("Odd / even", "", -1, 1, 0);
  q[AdSub] = lin("Sub", "", 0, 2, 0);
  q[AdStretch] = lin("Prism", "", -1, 1, 0);
  q[AdScatter] = lin("Scatter", "%", 0, 1, 0);
  q[AdSeed] = lin("Seed", "", 1, 64, 1);
  q[AdPhaseRand] = lin("Phase rand", "%", 0, 1, 0);
  q[AdBlur] = lin("Blur", "%", 0, 1, 0);
  q[AdMaskMix] = lin("Mask mix", "%", 0, 1, 0);
  q[AdFltType] = ch("Filter", {"Off", "Low pass", "Band pass", "Band stop", "High pass", "Soft LP", "Soft BP", "Soft BS", "Soft HP",
                                "Hill LP", "Slope LP", "Blend LP", "Ripple LP", "Edge LP", "Leak LP", "Comb", "Band comb", "Flat BP"},
                     AfLowPass);
  q[AdFltCutoff] = lg("Cutoff", "Hz", 30, 20000, 6000);
  q[AdFltWidth] = lin("Width", "oct", 0.1, 8, 2.5);
  q[AdFltReso] = lin("Resonance", "%", 0, 1, 0);
  q[AdFltKey] = lin("Key track", "", -2, 2, 0.5);
  q[AdFEnvAmt] = lin("Env amount", "oct", -8, 8, 0);
  q[AdFEnvAtt] = lg("Env attack", "s", 0.001, 10, 0.005);
  q[AdFEnvDec] = lg("Env decay", "s", 0.01, 10, 0.5);
  q[AdFEnvSus] = lin("Env sustain", "%", 0, 1, 0.3);
  q[AdFEnvRel] = lg("Env release", "s", 0.01, 10, 0.5);
  q[AdPhMix] = lin("Phaser", "%", 0, 1, 0);
  q[AdPhDensity] = lg("Density", "", 0.05, 32, 2);
  q[AdPhOffset] = lin("Offset", "", 0, 1, 0);
  q[AdPhSpeed] = lin("Motion", "", -16, 16, 0);
  q[AdPluck] = lin("Pluck", "%", 0, 1, 0);
  q[AdPluckTime] = lg("Pluck time", "s", 0.05, 10, 1);
  q[AdUniOrder] = lin("Unison", "", 1, 9, 1);
  q[AdUniDetune] = lin("Detune", "ct", 0, 100, 12);
  q[AdUniSpread] = lin("Spread", "%", 0, 1, 0.6);
  q[AdAttack] = lg("Attack", "s", 0.001, 10, 0.005);
  q[AdDecay] = lg("Decay", "s", 0.01, 20, 0.6);
  q[AdSustain] = lin("Sustain", "%", 0, 1, 0.7);
  q[AdRelease] = lg("Release", "s", 0.005, 10, 0.4);
  q[AdLfoShape] = ch("LFO shape", {"Sine", "Triangle", "Saw", "Square", "Random", "Soft saw", "Soft pulse", "Steps", "Hook", "Multi-sine",
                                     "Filter env", "Cutoff", "Voice rand", "Velocity", "Pitch"},
                     0);
  q[AdLfoRate] = lg("LFO rate", "Hz", 0.05, 30, 5);
  q[AdLfoPitch] = lin("LFO > pitch", "st", 0, 12, 0);
  q[AdLfoCutoff] = lin("LFO > cutoff", "", -3, 3, 0);
  q[AdLfoPhaser] = lin("LFO > phaser", "", -1, 1, 0);
  q[AdLfoMorph] = lin("LFO > morph", "", -1, 1, 0);
  q[AdGlide] = lin("Glide", "s", 0, 1, 0);
  q[AdVoices] = lin("Voices", "", 1, 16, 8);
  q[AdVolume] = lin("Volume", "", 0, 2, 0.8);
  fxParams(q, AdFx);
  for (int i = 0; i < kAdDrawn; ++i) q[static_cast<size_t>(AdDrawnBase + i)] = lin("Partial " + std::to_string(i + 1), "%", 0, 1, 1);
  for (int i = 0; i < kAdMask; ++i) q[static_cast<size_t>(AdMaskBase + i)] = lin("Mask " + std::to_string(i + 1), "%", 0, 1, 1);
  q[AdPitch] = lin("Pitch", "st", -24, 24, 0);
  q[AdFine] = lin("Fine", "ct", -100, 100, 0);
  for (int i = 0; i < kAdTeq; ++i) q[static_cast<size_t>(AdTeqBase + i)] = lin("EQ band " + std::to_string(i + 1), "", -1, 1, 0);
  q[AdSub3] = lin("Half 3", "", 0, 2, 0);
  q[AdSub4] = lin("Half 5", "", 0, 2, 0);
  q[AdProtect] = lin("Low protect", "%", 0, 1, 0);
  q[AdMaskLfo] = lin("LFO > mask", "", -1, 1, 0);
  q[AdTremolo] = lin("Tremolo", "%", 0, 1, 0);
  q[AdGrit] = lin("Grit", "", -1, 1, 0);
  q[AdDetune] = lin("Detune", "ct", -1200, 1200, 0);
  q[AdVibrato] = lin("Vibrato depth", "%", 0, 1, 0);
  q[AdVelAtt] = tog("Vel > attack", false);
  q[AdVelRel] = tog("Vel > release", false);
  q[AdVelVol] = tog("Vel > volume", true);
  q[AdVelFEnv] = tog("Vel > env amount", false);
  q[AdVelPluck] = tog("Vel > pluck", false);
  q[AdVelHarm] = tog("Vel > harmonizer", false);
  q[AdVelPorta] = tog("Vel > glide", false);
  q[AdVelUni] = tog("Vel > unison", false);
  q[AdPluckMask] = tog("Pluck masked", false);
  q[AdPluckAlt] = tog("Pluck alt", false);
  q[AdPluckRel] = tog("Pluck on release", false);
  q[AdPlayMode] = ch("Voice mode", {"Auto", "Poly", "Poly glide", "Mono", "Mono glide"}, ApAuto);
  q[AdFEnvAttSlope] = lin("Env att slope", "", -1, 1, 0);
  q[AdFEnvDecSlope] = lin("Env dec slope", "", -1, 1, 0);
  q[AdFEnvAdaptive] = tog("Adaptive env", false);
  q[AdFltScale] = ch("Filter scale", {"Octaves", "Hz"}, 0);
  q[AdResType] = ch("Reso shape", {"Peak", "Spring", "Plateau", "Hat", "Bump", "Twin", "Dip", "Ramp", "Noise"}, ArPeak);
  q[AdResOsc] = lin("Self-osc", "%", 0, 1, 0);
  q[AdResOffset] = lin("Reso offset", "st", -24, 24, 0);
  q[AdResScale] = ch("Reso scale", {"Octaves", "Hz"}, 0);
  q[AdResAdaptive] = tog("Adaptive width", false);
  q[AdResTrack] = tog("Track motion", false);
  q[AdUniPhase] = lin("Unison phase", "%", 0, 1, 0.5);
  q[AdUniVar] = lin("Unison var", "%", 0, 1, 0.5);
  q[AdUniType] = ch("Unison type", {"Spread", "Even", "Smeared"}, 0);
  q[AdPhType] = ch("Phaser shape", {"Notch", "Triangle", "Round", "Deep", "Deeper", "Needle", "Twin", "Cascade", "Block", "Linear"}, ApClassic);
  q[AdPhScale] = ch("Phaser scale", {"Octaves", "Hz"}, 0);
  q[AdPhWidthLfo] = lin("LFO > density", "", -4, 4, 0);
  q[AdPhKey] = lin("Key > offset", "", -2, 2, 1);
  q[AdHzMix] = lin("Harmonizer", "%", 0, 1, 0);
  q[AdHzWidth] = lin("Harm width", "%", 0, 1, 0.5);
  q[AdHzStrength] = lin("Harm strength", "%", 0, 1, 0);
  q[AdHzType] = ch("Harm type", {"Octave", "Octave +1", "Octave +2", "Next", "Next +1"}, AhOct);
  q[AdHzPos] = ch("Harm position", {"Before filter", "Before phaser", "Before pluck", "Last"}, AhPreFilter);
  q[AdHzWidthLfo] = lin("LFO > harm", "", -1, 1, 0);
  q[AdLfoAttack] = lg("LFO fade", "s", 0.001, 10, 0.001);
  q[AdLfoGlobal] = tog("LFO global", true);
  q[AdLfoTempo] = tog("LFO tempo", false);
  q[AdChType] = ch("Chorus type", {"Classic", "Thick", "Deep", "Flanger", "Flanger 2", "Wide", "Mod", "Surround", "Vibrato"}, AcClassic);
  q[AdDelType] = ch("Delay type", {"Straight", "Ping-pong", "Wide"}, AdlPingPong);
  q[AdRevType] = ch("Reverb type", {"Room", "Hall", "Church", "Club", "Drum room", "Booth"}, AvClassic);
  q[AdCompType] = ch("Comp type", {"Off", "Limit", "Warm", "Hot", "Drive"}, AkOff);
  q[AdComp] = lin("Comp", "%", 0, 1, 0.5);
  auto fx = [](int f, float v) { return std::make_pair(AdFx + f, v); };
  Ov oddMask = {{AdMaskMix, 1.f}, {AdUniOrder, 3.f}, {AdUniDetune, 18.f}, {AdFltCutoff, 3500.f}, {AdFltReso, 0.35f}, {AdGlide, 0.05f}, {AdVoices, 1.f}};
  for (int i = 0; i < kAdMask; ++i) oddMask.push_back({AdMaskBase + i, i % 2 ? 0.1f : (i % 4 == 0 ? 1.f : 0.6f)});
  Ov hollow = {{AdSpectrumA, SpSaw}, {AdFltType, AfOff}, {AdSub, 0.35f}, {AdSustain, 1.f}, {AdRelease, 0.15f}};
  for (int i = 0; i < kAdDrawn; ++i) hollow.push_back({AdDrawnBase + i, (i + 1) % 2 == 0 ? 0.15f : 1.f / (1.f + i * 0.05f)});
  p.presets = {
      {"Init saw", {}},
      {"Glass pad", {{AdSpectrumA, SpGlass}, {AdAttack, 0.8f}, {AdRelease, 2.5f}, {AdSustain, 0.9f}, {AdUniOrder, 3.f}, {AdPhMix, 0.45f}, {AdPhSpeed, 0.12f}, fx(FxChorus, 0.4f), fx(FxReverb, 0.45f)}},
      {"Harp pluck", {{AdPluck, 0.92f}, {AdPluckTime, 0.7f}, {AdSustain, 0.f}, {AdDecay, 3.f}, {AdRelease, 1.2f}, {AdTilt, -2.f}, {AdFltCutoff, 12000.f}, fx(FxReverb, 0.3f)}},
      {"Vowel morph", {{AdSpectrumA, SpVowel}, {AdSpectrumB, SpChoir}, {AdLfoMorph, 0.9f}, {AdLfoRate, 0.35f}, {AdFltReso, 0.3f}, {AdFltCutoff, 2500.f}, {AdAttack, 0.15f}, {AdSustain, 0.9f}}},
      {"Hollow organ", hollow},
      {"Struck metal", {{AdSpectrumA, SpRandom}, {AdSeed, 7.f}, {AdStretch, 0.55f}, {AdScatter, 0.5f}, {AdPluck, 0.8f}, {AdPluckTime, 2.5f}, {AdSustain, 0.f}, {AdDecay, 5.f}, {AdRelease, 3.f}, {AdFltType, AfOff}, fx(FxReverb, 0.35f)}},
      {"Phaser sweep", {{AdPhMix, 0.85f}, {AdPhSpeed, 0.35f}, {AdPhDensity, 3.f}, {AdUniOrder, 2.f}, {AdFltCutoff, 9000.f}, {AdSustain, 1.f}, fx(FxDelay, 0.25f)}},
      {"Blurred choir", {{AdSpectrumA, SpChoir}, {AdBlur, 0.75f}, {AdFEnvAmt, 2.5f}, {AdFEnvAtt, 0.6f}, {AdFEnvDec, 2.f}, {AdAttack, 0.4f}, {AdRelease, 1.8f}, {AdUniOrder, 3.f}, fx(FxReverb, 0.5f)}},
      {"Sub bass", {{AdSpectrumA, SpSquare}, {AdPartials, 24.f}, {AdFltCutoff, 700.f}, {AdFEnvAmt, 2.5f}, {AdFEnvDec, 0.2f}, {AdFEnvSus, 0.f}, {AdSub, 0.6f}, {AdVoices, 1.f}, {AdGlide, 0.06f}, {AdFltKey, 0.8f}}},
      {"Masked lead", oddMask},
  };
  p.editorWidth = 1040;
  return p;
}

// ------------------------------------------------------------------ Image Synth
PluginInfo imageSynth() {
  PluginInfo p{"gl.imagesynth", "Image Synth", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(ImCount);
  q[ImSource] = ch("Source", {"Pattern", "Image file", "Canvas"}, SrcPattern);
  q[ImPattern] = ch("Pattern", {"Sweep", "Chords", "Rain", "Rings", "Stairs", "Waves", "Pulse grid", "Cloud"}, 0);
  q[ImLow] = lg("Low", "Hz", 20, 4000, 110);
  q[ImHigh] = lg("High", "Hz", 200, 20000, 7000);
  q[ImScale] = ch("Scale", {"Log", "Linear", "Harmonic"}, 0);
  q[ImPixel] = lg("Pixel", "s", 0.005, 1, 0.05);
  q[ImStart] = lin("Start", "%", 0, 1, 0);
  q[ImLoop] = tog("Loop", false);
  q[ImReverse] = tog("Reverse", false);
  q[ImGrainy] = tog("Grainy", false);
  q[ImWiden] = tog("Widen", false);
  q[ImUseBlue] = tog("Use blue", false);
  q[ImKeyTrack] = tog("Key track", true);
  q[ImGamma] = lg("Contrast", "", 0.3, 3, 1);
  q[ImAttack] = lg("Attack", "s", 0.001, 5, 0.01);
  q[ImRelease] = lg("Release", "s", 0.01, 10, 0.3);
  q[ImVoices] = lin("Voices", "", 1, 12, 6);
  q[ImVolume] = lin("Volume", "%", 0, 1, 0.8);
  p.presets = {
      {"Sweep", {}},
      {"Chord ladder", {{ImPattern, 1.f}, {ImPixel, 0.04f}, {ImLow, 80.f}, {ImHigh, 2400.f}, {ImRelease, 0.8f}}},
      {"Rain drops", {{ImPattern, 2.f}, {ImGrainy, 1.f}, {ImWiden, 1.f}, {ImPixel, 0.03f}, {ImLoop, 1.f}, {ImLow, 300.f}, {ImHigh, 9000.f}}},
      {"Ring tones", {{ImPattern, 3.f}, {ImScale, 2.f}, {ImLow, 55.f}, {ImPixel, 0.04f}, {ImLoop, 1.f}, {ImGamma, 0.8f}}},
      {"Stair arp", {{ImPattern, 4.f}, {ImPixel, 0.02f}, {ImLoop, 1.f}, {ImLow, 130.f}, {ImHigh, 4000.f}}},
      {"Wave drone", {{ImPattern, 5.f}, {ImLoop, 1.f}, {ImWiden, 1.f}, {ImAttack, 1.2f}, {ImRelease, 2.f}, {ImPixel, 0.12f}}},
      {"Pulse loop", {{ImPattern, 6.f}, {ImLoop, 1.f}, {ImPixel, 0.015f}, {ImScale, 1.f}, {ImLow, 60.f}, {ImHigh, 6000.f}}},
      {"Cloud", {{ImPattern, 7.f}, {ImGrainy, 1.f}, {ImUseBlue, 1.f}, {ImPixel, 0.08f}, {ImAttack, 0.5f}, {ImRelease, 1.5f}}},
      {"Canvas", {{ImSource, static_cast<float>(SrcCanvas)}, {ImLoop, 1.f}, {ImPixel, 0.06f}}},
  };
  p.editorWidth = 900;
  return p;
}

// ------------------------------------------------------------------ String Model
PluginInfo stringModel() {
  PluginInfo p{"gl.string", "String Model", "Synth", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(StCount);
  q[StExciter] = ch("Exciter", {"Pluck", "Pick", "Hammer", "Bow", "Breath"}, ExPluck);
  q[StExcPos] = lin("Position", "%", 0.02, 0.5, 0.15);
  q[StExcHard] = lin("Hardness", "%", 0, 1, 0.6);
  q[StExcNoise] = lin("Noise", "%", 0, 1, 0.08);
  q[StBowForce] = lin("Force", "%", 0, 1, 0.5);
  q[StDecay] = lg("Decay", "s", 0.1, 30, 4);
  q[StDamp] = lin("Damping", "%", 0, 1, 0.35);
  q[StStiffness] = lin("Stiffness", "%", 0, 1, 0.08);
  q[StPickup] = lin("Pickup", "%", 0, 1, 0.3);
  q[StSaturation] = lin("Saturation", "%", 0, 1, 0.05);
  q[St2Ratio] = ch("String 2", {"Off", "Unison", "Octave", "Fifth", "Oct down", "Twelfth"}, 1);
  q[St2Level] = lin("Level 2", "%", 0, 1, 0.45);
  q[StDetune] = lin("Detune", "ct", -50, 50, 4);
  q[StBody] = ch("Body", {"None", "Acoustic", "Nylon", "Violin", "Cello", "Zither", "Wood box", "Metal cone"}, 1);
  q[StBodyMix] = lin("Body mix", "%", 0, 1, 0.5);
  q[StBodySize] = lg("Size", "", 0.5, 2, 1);
  q[StLoCut] = lg("Lo cut", "Hz", 20, 1000, 40);
  q[StHiCut] = lg("Hi cut", "Hz", 200, 20000, 9000);
  q[StHiCutEnv] = lin("Hi cut env", "", -1, 1, 0);
  q[StHiCutDec] = lg("Env decay", "s", 0.01, 5, 0.3);
  q[StAttack] = lg("Attack", "s", 0.001, 5, 0.002);
  q[StSustain] = lin("Sustain", "%", 0, 1, 1);
  q[StRelease] = lg("Release", "s", 0.01, 10, 0.3);
  q[StStrum] = ch("Strum", {"Off", "Major", "Minor", "Power", "Seventh", "Sus4", "Octaves"}, 0);
  q[StStrumTime] = lg("Strum time", "s", 0.002, 0.2, 0.025);
  q[StStrumDir] = ch("Direction", {"Down", "Up", "Alternate"}, 0);
  q[StDrive] = lin("Drive", "%", 0, 1, 0);
  q[StTone] = lin("Tone", "", -1, 1, 0);
  q[StCab] = tog("Cabinet", false);
  q[StVibrato] = lin("Vibrato", "%", 0, 1, 0);
  q[StVibRate] = lg("Vib rate", "Hz", 0.5, 12, 5);
  q[StSpread] = lin("Spread", "%", 0, 1, 0.3);
  q[StVoices] = lin("Voices", "", 1, 32, 16);
  q[StVolume] = lin("Volume", "%", 0, 1, 0.8);
  fxParams(q, StFx);
  auto fx = [](int f, float v) { return std::make_pair(StFx + f, v); };
  p.presets = {
      {"Steel string", {}},
      {"Nylon", {{StBody, 2.f}, {StDamp, 0.55f}, {StExcHard, 0.4f}, {StDecay, 3.f}, {StExcPos, 0.2f}, fx(FxReverb, 0.2f)}},
      {"Concert harp", {{StBody, 6.f}, {StBodySize, 1.4f}, {StExcPos, 0.35f}, {StExcHard, 0.45f}, {StDecay, 7.f}, {StDamp, 0.45f}, {St2Ratio, 0.f}, {StRelease, 2.f}, fx(FxReverb, 0.4f)}},
      {"Zither pick", {{StExciter, ExPick}, {StBody, 5.f}, {St2Ratio, 2.f}, {St2Level, 0.25f}, {StExcPos, 0.1f}, {StDamp, 0.2f}, {StDecay, 5.f}, fx(FxDelay, 0.2f)}},
      {"Bowed cello", {{StExciter, ExBow}, {StBody, 4.f}, {StBowForce, 0.55f}, {StAttack, 0.12f}, {StRelease, 0.6f}, {StDamp, 0.4f}, {StDecay, 3.f}, {StVibrato, 0.25f}, {StVibRate, 5.5f}, {St2Ratio, 0.f}, fx(FxReverb, 0.3f)}},
      {"Fiddle", {{StExciter, ExBow}, {StBody, 3.f}, {StBowForce, 0.65f}, {StAttack, 0.06f}, {StDamp, 0.3f}, {StVibrato, 0.3f}, {StVibRate, 6.f}, {St2Ratio, 0.f}, {StHiCut, 7000.f}}},
      {"Hammered", {{StExciter, ExHammer}, {StStiffness, 0.4f}, {StExcHard, 0.55f}, {StDetune, 2.f}, {StBody, 6.f}, {StDecay, 8.f}, {StSustain, 0.f}, {StRelease, 0.5f}, {StExcPos, 0.12f}}},
      {"Buzz sitar", {{StExciter, ExPick}, {StSaturation, 0.75f}, {StPickup, 0.08f}, {StBody, 5.f}, {St2Ratio, 2.f}, {St2Level, 0.3f}, {StDecay, 6.f}, {StDamp, 0.15f}}},
      {"Power chords", {{StStrum, 3.f}, {StStrumTime, 0.008f}, {StExciter, ExPick}, {StBody, 0.f}, {StDrive, 0.65f}, {StCab, 1.f}, {StPickup, 0.15f}, {StDecay, 6.f}, {StRelease, 0.15f}, {StVolume, 0.6f}}},
      {"Folk strum", {{StStrum, 1.f}, {StStrumDir, 2.f}, {StStrumTime, 0.03f}, {StExciter, ExPick}, {StExcHard, 0.5f}, fx(FxReverb, 0.25f)}},
      {"Breath string", {{StExciter, ExBreath}, {StAttack, 0.3f}, {StRelease, 1.f}, {StBody, 3.f}, {StDecay, 2.f}, {StExcHard, 0.4f}, {St2Ratio, 2.f}, fx(FxReverb, 0.4f), fx(FxChorus, 0.3f)}},
      {"Clean electric", {{StExciter, ExPick}, {StBody, 0.f}, {StPickup, 0.12f}, {StCab, 1.f}, {StDrive, 0.1f}, {StDamp, 0.2f}, {StDecay, 6.f}, fx(FxChorus, 0.45f), fx(FxDelay, 0.2f)}},
  };
  p.editorWidth = 1000;
  return p;
}

// ------------------------------------------------------------------ Drum Model
struct PadDef {
  int shape;
  float freq, spread, decay, material, pos, bend, bendT, mallet, click, noise, nDec, nTone, wires;
  int choke;
  float drive, level, pan;
};
const PadDef kStudio[kDrumPads] = {
    {DsMembrane, 52, 0.6f, 0.5f, 0.7f, 0.1f, 16, 0.035f, 0.6f, 0.35f, 0, 0.05f, 2000, 0, 0, 0.25f, 0.95f, 0},          // kick
    {DsMembrane, 185, 1, 0.2f, 0.5f, 0.4f, 3, 0.02f, 0.7f, 0.25f, 0.75f, 0.2f, 4200, 0.45f, 0, 0.1f, 0.8f, 0.05f},     // snare
    {DsBar, 780, 1, 0.06f, 0.6f, 0.6f, 0, 0.05f, 0.95f, 0.6f, 0.15f, 0.025f, 3000, 0, 0, 0, 0.6f, 0.1f},               // rim
    {DsPlate, 1150, 1, 0.03f, 0.8f, 0.5f, 0, 0.05f, 0.9f, 0.15f, 1, 0.2f, 1400, 0, 0, 0.2f, 0.7f, -0.05f},            // clap
    {DsPlate, 3300, 1.7f, 0.05f, 0.3f, 1, 0, 0.05f, 0.95f, 0.3f, 0.8f, 0.05f, 9500, 0, 1, 0, 0.5f, 0.2f},             // closed hat
    {DsPlate, 3300, 1.7f, 0.6f, 0.3f, 1, 0, 0.05f, 0.95f, 0.3f, 0.8f, 0.55f, 9500, 0, 1, 0, 0.5f, 0.2f},              // open hat
    {DsTom, 88, 1, 0.55f, 0.4f, 0.25f, 5, 0.09f, 0.5f, 0.15f, 0.05f, 0.1f, 1500, 0, 0, 0.05f, 0.85f, -0.3f},          // low tom
    {DsTom, 128, 1, 0.45f, 0.4f, 0.25f, 5, 0.08f, 0.55f, 0.15f, 0.05f, 0.1f, 1800, 0, 0, 0.05f, 0.8f, -0.1f},         // mid tom
    {DsTom, 182, 1, 0.38f, 0.4f, 0.25f, 5, 0.07f, 0.6f, 0.15f, 0.05f, 0.1f, 2100, 0, 0, 0.05f, 0.8f, 0.15f},          // high tom
    {DsPlate, 2500, 2, 1.8f, 0.2f, 1, 0, 0.05f, 0.9f, 0.3f, 0.6f, 1.6f, 7000, 0, 0, 0, 0.45f, -0.25f},                // crash
    {DsBell, 560, 1, 0.35f, 0.5f, 0.6f, 0, 0.05f, 0.9f, 0.2f, 0, 0.05f, 3000, 0, 0, 0.1f, 0.55f, 0.3f},              // cowbell
    {DsBar, 2300, 1, 0.12f, 0.5f, 0.5f, 0, 0.05f, 1, 0.25f, 0, 0.05f, 3000, 0, 0, 0, 0.55f, -0.3f},                  // clave
    {DsTom, 300, 0.9f, 0.35f, 0.45f, 0.4f, 2, 0.03f, 0.6f, 0.2f, 0, 0.05f, 2000, 0, 0, 0, 0.75f, 0.25f},             // conga
    {DsTom, 440, 0.9f, 0.25f, 0.5f, 0.5f, 2, 0.02f, 0.7f, 0.25f, 0, 0.05f, 2000, 0, 0, 0, 0.7f, -0.2f},              // bongo
    {DsTabla, 330, 1, 0.6f, 0.55f, 0.3f, -3, 0.12f, 0.75f, 0.15f, 0, 0.05f, 2000, 0, 0, 0, 0.7f, 0.1f},              // tabla
    {DsMembrane, 98, 1, 2.5f, 0.3f, 0.3f, 0, 0.05f, 0.45f, 0.1f, 0, 0.05f, 2000, 0, 0, 0, 0.8f, 0},                  // timpani
};

void padOverrides(Ov& o, int pad, const PadDef& d) {
  auto put = [&](int prm, float v) { o.push_back({drumPad(pad, prm), v}); };
  put(DpShape, static_cast<float>(d.shape));
  put(DpFreq, d.freq);
  put(DpSpread, d.spread);
  put(DpDecay, d.decay);
  put(DpMaterial, d.material);
  put(DpPosition, d.pos);
  put(DpBend, d.bend);
  put(DpBendTime, d.bendT);
  put(DpMallet, d.mallet);
  put(DpClick, d.click);
  put(DpNoise, d.noise);
  put(DpNoiseDecay, d.nDec);
  put(DpNoiseTone, d.nTone);
  put(DpWires, d.wires);
  put(DpChoke, static_cast<float>(d.choke));
  put(DpDrive, d.drive);
  put(DpLevel, d.level);
  put(DpPan, d.pan);
}

// A kit derived from the studio kit by a per-pad transform.
template <typename F>
Ov kitFrom(F f) {
  Ov o;
  for (int i = 0; i < kDrumPads; ++i) {
    PadDef d = kStudio[i];
    f(i, d);
    padOverrides(o, i, d);
  }
  return o;
}

PluginInfo drumModel() {
  PluginInfo p{"gl.drumkit", "Drum Model", "Drums", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(kDrumParamCount);
  q[DmBaseNote] = lin("Base note", "note", 0, 112, 60);
  q[DmVelocity] = lin("Velocity", "%", 0, 1, 0.7);
  q[DmVolume] = lin("Volume", "%", 0, 1, 0.8);
  q[DmLow] = lin("Low", "dB", -12, 12, 0);
  q[DmMid] = lin("Mid", "dB", -12, 12, 0);
  q[DmHigh] = lin("High", "dB", -12, 12, 0);
  q[DmLimiter] = tog("Limiter", true);
  q[DmLofi] = lin("Lo-fi", "%", 0, 1, 0);
  q[DmKeyMode] = ch("Key mode", {"Pad per key", "Pad 1 pitched"}, 0);
  std::vector<std::string> shapes = {"Membrane", "Tom", "Bar", "Plate", "Bell", "Tabla"};
  for (int i = 0; i < kDrumPads; ++i) {
    const PadDef& d = kStudio[i];
    std::string n = "Pad " + std::to_string(i + 1) + " ";
    auto at = [&](int prm) -> ParamDesc& { return q[static_cast<size_t>(drumPad(i, prm))]; };
    at(DpLevel) = lin(n + "level", "%", 0, 1, d.level);
    at(DpPan) = lin(n + "pan", "pan", -1, 1, d.pan);
    at(DpTune) = lin(n + "tune", "st", -24, 24, 0);
    at(DpMute) = tog(n + "mute", false);
    at(DpShape) = ch(n + "shape", shapes, d.shape);
    at(DpFreq) = lg(n + "tension", "Hz", 20, 5000, d.freq);
    at(DpSpread) = lin(n + "spread", "", 0.25, 2, d.spread);
    at(DpDecay) = lg(n + "decay", "s", 0.02, 6, d.decay);
    at(DpMaterial) = lin(n + "material", "%", 0, 1, d.material);
    at(DpPosition) = lin(n + "position", "%", 0, 1, d.pos);
    at(DpBend) = lin(n + "bend", "st", -12, 36, d.bend);
    at(DpBendTime) = lg(n + "bend time", "s", 0.003, 1, d.bendT);
    at(DpMallet) = lin(n + "hardness", "%", 0, 1, d.mallet);
    at(DpClick) = lin(n + "click", "%", 0, 1, d.click);
    at(DpNoise) = lin(n + "noise", "%", 0, 1, d.noise);
    at(DpNoiseDecay) = lg(n + "noise decay", "s", 0.01, 2, d.nDec);
    at(DpNoiseTone) = lg(n + "noise tone", "Hz", 200, 16000, d.nTone);
    at(DpWires) = lin(n + "wires", "%", 0, 1, d.wires);
    at(DpChoke) = lin(n + "choke", "", 0, 8, d.choke);
    at(DpDrive) = lin(n + "drive", "%", 0, 1, d.drive);
  }
  p.presets = {
      {"Studio kit", {}},
      {"Deep kit", kitFrom([](int i, PadDef& d) {
         d.freq *= 0.8f;
         d.decay *= 1.4f;
         if (i == 0) d.bend = 22, d.drive = 0.4f;
       })},
      {"Tight kit", kitFrom([](int, PadDef& d) {
         d.decay *= 0.55f;
         d.nDec *= 0.6f;
         d.mallet = std::min(1.0f, d.mallet + 0.2f);
       })},
      {"Boom electro", kitFrom([](int i, PadDef& d) {
         d.material = 0.9f;
         d.spread *= 0.5f;
         d.click *= 1.5f;
         if (i == 0) d.decay = 1.2f, d.bend = 24, d.bendT = 0.06f, d.drive = 0.5f;
         if (i == 1) d.noise = 1, d.wires = 0, d.nDec = 0.25f;
       })},
      {"Hand drums", kitFrom([](int i, PadDef& d) {
         static const int shapes[kDrumPads] = {DsTom, DsTabla, DsTom, DsTabla, DsBar, DsBell, DsTom, DsTom, DsTom, DsBell, DsBell, DsBar, DsTom, DsTom, DsTabla, DsMembrane};
         d.shape = shapes[i];
         d.noise *= 0.2f;
         d.wires = 0;
         d.mallet *= 0.7f;
         d.freq = 120.0f * std::pow(1.16f, static_cast<float>(i));
       })},
      {"Metal shop", kitFrom([](int i, PadDef& d) {
         d.shape = i % 3 == 0 ? DsBell : (i % 3 == 1 ? DsPlate : DsBar);
         d.freq = 220.0f * std::pow(1.12f, static_cast<float>(i));
         d.decay = 0.4f + 0.1f * (i % 5);
         d.material = 0.15f;
         d.mallet = 0.95f;
         d.noise *= 0.3f;
       })},
      {"Tuned toms", kitFrom([](int i, PadDef& d) {
         static const int scale[kDrumPads] = {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26, 28, 31, 33, 36};
         d.shape = DsTom;
         d.freq = 65.4f * std::pow(2.0f, scale[i] / 12.0f);
         d.decay = 0.6f;
         d.bend = 3;
         d.noise = 0.03f;
         d.wires = 0;
         d.choke = 0;
       })},
      {"Lo-fi kit", kitFrom([](int, PadDef& d) {
         d.drive = std::min(1.0f, d.drive + 0.35f);
         d.nTone *= 0.6f;
       })},
  };
  p.presets.back().values.push_back({DmLofi, 0.55f});
  p.presets.back().values.push_back({DmHigh, -4.f});
  p.editorWidth = 1060;
  return p;
}

}  // namespace

PluginInfo harmorInfo();  // harmor_info.cpp
void appendDrumsInfos(std::vector<PluginInfo>& v);    // drums_info.cpp (J-22)
void appendStringsInfos(std::vector<PluginInfo>& v);  // strings_info.cpp (J-22)
void appendMetalInfos(std::vector<PluginInfo>& v);    // metal_info.cpp (J-22)

void appendInfos(std::vector<PluginInfo>& v) {
  v.push_back(additive());
  v.push_back(harmorInfo());
  v.push_back(imageSynth());
  v.push_back(stringModel());
  v.push_back(drumModel());
  appendDrumsInfos(v);
  appendStringsInfos(v);
  appendMetalInfos(v);
}

}  // namespace gl::packs::models
