// Gridline — "mini" pack: catalogue entries of Gridline Suite Mini (35
// compact effects), with their factory presets.
#include "../Packs.h"
#include <iterator>
#include "mini.h"

namespace gl::packs::mini {

namespace {

ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
template <size_t N>
ParamDesc ch(const char* n, const char* const (&labels)[N], double d) {
  return ParamDesc{n, "", 0, static_cast<double>(N - 1), d, ParamKind::Choice, ParamScale::Linear,
                   std::vector<std::string>(std::begin(labels), std::end(labels))};
}
ParamDesc mix(double d) { return lin("Mix", "%", 0, 1, d); }

using P = PluginInfo::Preset;
constexpr float kEditorWidth = 440.0f;  // one width for the whole suite

PluginInfo make(const char* type, std::vector<ParamDesc> params, std::vector<P> presets) {
  const Entry* e = findEntry(type);
  PluginInfo p{type, e ? e->name : type, kCategory, PluginKind::Effect, std::move(params), false, std::move(presets)};
  p.editorWidth = kEditorWidth;
  return p;
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  // ---- utility ----
  {
    using namespace trim;
    std::vector<ParamDesc> ps(Count);
    ps[Gain] = lin("Gain", "dB", -36, 36, 0);
    ps[FlipL] = tog("Flip Left", 0);
    ps[FlipR] = tog("Flip Right", 0);
    v.push_back(make("gl.minitrim", ps,
                     {{"Down 6 dB", {{Gain, -6.f}}},
                      {"Up 6 dB", {{Gain, 6.f}}},
                      {"Polarity Flip", {{FlipL, 1.f}, {FlipR, 1.f}}},
                      {"Left Flipped", {{FlipL, 1.f}}}}));
  }
  {
    using namespace breadth;
    std::vector<ParamDesc> ps(Count);
    ps[Width] = lin("Width", "%", 0, 2, 1);
    ps[Mid] = lin("Mid", "%", 0, 1, 1);
    ps[Pan] = lin("Pan", "pan", -1, 1, 0);
    ps[MonoBelow] = lin("Mono Below", "Hz", 0, 400, 0);
    v.push_back(make("gl.minibreadth", ps,
                     {{"Wide", {{Width, 1.6f}}},
                      {"Mono", {{Width, 0.f}}},
                      {"Tight Low End", {{Width, 1.3f}, {MonoBelow, 150.f}}},
                      {"Narrow Left", {{Width, 0.5f}, {Pan, -0.4f}}}}));
  }
  {
    using namespace skew;
    std::vector<ParamDesc> ps(Count);
    ps[Delay] = lg("Delay", "ms", 0.1, 40, 8);
    ps[Side] = ch("Side", kSides, 1);
    ps[Balance] = lin("Balance", "pan", -1, 1, 0);
    v.push_back(make("gl.miniskew", ps,
                     {{"Wide Room", {{Delay, 12.f}}},
                      {"Subtle", {{Delay, 3.f}}},
                      {"Right Lead", {{Delay, 18.f}, {Side, 0.f}}},
                      {"Slap Side", {{Delay, 35.f}, {Balance, -0.2f}}}}));
  }
  {
    using namespace weave;
    std::vector<ParamDesc> ps(Count);
    ps[LL] = lin("Left to Left", "%", -1, 1, 1);
    ps[RL] = lin("Right to Left", "%", -1, 1, 0);
    ps[LR] = lin("Left to Right", "%", -1, 1, 0);
    ps[RR] = lin("Right to Right", "%", -1, 1, 1);
    v.push_back(make("gl.miniweave", ps,
                     {{"Swap Sides", {{LL, 0.f}, {RL, 1.f}, {LR, 1.f}, {RR, 0.f}}},
                      {"Mono Sum", {{LL, 0.5f}, {RL, 0.5f}, {LR, 0.5f}, {RR, 0.5f}}},
                      {"Left Only", {{RL, 0.f}, {LR, 1.f}, {RR, 0.f}}},
                      {"Right Only", {{LL, 0.f}, {RL, 1.f}}},
                      {"Sides Only", {{LL, 0.5f}, {RL, -0.5f}, {LR, -0.5f}, {RR, 0.5f}}}}));
  }
  // ---- tone ----
  {
    using namespace bands;
    std::vector<ParamDesc> ps(Count);
    ps[Low] = lin("Low", "dB", -18, 18, 0);
    ps[Mid] = lin("Mid", "dB", -18, 18, 0);
    ps[High] = lin("High", "dB", -18, 18, 0);
    ps[LowSplit] = lg("Low Split", "Hz", 40, 1000, 250);
    ps[HighSplit] = lg("High Split", "Hz", 1000, 12000, 2500);
    v.push_back(make("gl.minibands", ps,
                     {{"Smile", {{Low, 4.f}, {Mid, -3.f}, {High, 4.f}}},
                      {"Telephone", {{Low, -18.f}, {Mid, 4.f}, {High, -18.f}, {LowSplit, 400.f}, {HighSplit, 3000.f}}},
                      {"Warmth", {{Low, 3.f}, {High, -2.f}}},
                      {"Bass Cut", {{Low, -18.f}, {LowSplit, 120.f}}}}));
  }
  {
    using namespace sieve;
    std::vector<ParamDesc> ps(Count);
    ps[Type] = ch("Type", kSieveTypes, 0);
    ps[Cutoff] = lg("Cutoff", "Hz", 20, 20000, 1000);
    ps[Q] = lg("Q", "", 0.1, 18, 0.707);
    ps[Gain] = lin("Gain", "dB", -24, 24, 0);
    ps[Slope] = ch("Slope", kSieveSlopes, 0);
    v.push_back(make("gl.minisieve", ps,
                     {{"Steep Low Pass", {{Cutoff, 800.f}, {Slope, 1.f}}},
                      {"Thin Out", {{Type, 1.f}, {Cutoff, 300.f}, {Slope, 1.f}}},
                      {"Radio Band", {{Type, 2.f}, {Cutoff, 1500.f}, {Q, 1.2f}}},
                      {"Presence", {{Type, 4.f}, {Cutoff, 3000.f}, {Q, 1.f}, {Gain, 6.f}}},
                      {"Air", {{Type, 6.f}, {Cutoff, 9000.f}, {Gain, 5.f}}}}));
  }
  {
    using namespace rungs;
    std::vector<ParamDesc> ps(Count);
    ps[Cutoff] = lg("Cutoff", "Hz", 20, 20000, 800);
    ps[Resonance] = lin("Resonance", "%", 0, 1, 0.2);
    ps[Drive] = lin("Drive", "dB", 0, 36, 0);
    ps[Mode] = ch("Mode", kRungsModes, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minirungs", ps,
                     {{"Squelch", {{Cutoff, 400.f}, {Resonance, 0.85f}, {Drive, 12.f}}},
                      {"Soft Low", {{Cutoff, 1200.f}, {Resonance, 0.1f}}},
                      {"Band Talk", {{Mode, 2.f}, {Cutoff, 900.f}, {Resonance, 0.6f}}},
                      {"Screamer", {{Cutoff, 700.f}, {Resonance, 0.95f}, {Drive, 30.f}}}}));
  }
  {
    using namespace scorch;
    std::vector<ParamDesc> ps(Count);
    ps[Type] = ch("Type", kScorchTypes, 0);
    ps[Cutoff] = lg("Cutoff", "Hz", 20, 20000, 1000);
    ps[Q] = lg("Q", "", 0.5, 20, 0.9);
    ps[Drive] = lin("Drive", "%", 0, 1, 0.3);
    ps[Flavour] = ch("Flavour", kScorchFlavours, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.miniscorch", ps,
                     {{"Hot Low", {{Cutoff, 600.f}, {Q, 3.f}, {Drive, 0.7f}, {Flavour, 1.f}}},
                      {"Folded Band", {{Type, 1.f}, {Q, 5.f}, {Drive, 0.8f}, {Flavour, 2.f}}},
                      {"Lean Highs", {{Type, 2.f}, {Cutoff, 2000.f}, {Drive, 0.6f}, {Flavour, 3.f}}},
                      {"Clean Notch", {{Type, 3.f}, {Cutoff, 1200.f}, {Q, 2.f}, {Drive, 0.f}}}}));
  }
  {
    using namespace vowel;
    std::vector<ParamDesc> ps(Count);
    ps[X] = lin("Open", "%", 0, 1, 0.5);
    ps[Y] = lin("Front", "%", 0, 1, 0.5);
    ps[Sharpness] = lin("Sharpness", "%", 0, 1, 0.5);
    ps[Shift] = lin("Shift", "st", -12, 12, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minivowel", ps,
                     {{"Ooh", {{X, 0.1f}, {Y, 0.1f}}},
                      {"Eee", {{X, 0.1f}, {Y, 0.95f}}},
                      {"Aah", {{X, 0.9f}, {Y, 0.4f}}},
                      {"Talk Box", {{X, 0.6f}, {Y, 0.6f}, {Sharpness, 0.85f}}},
                      {"Small Voice", {{X, 0.4f}, {Y, 0.7f}, {Shift, 7.f}}}}));
  }
  {
    using namespace tines;
    std::vector<ParamDesc> ps(Count);
    ps[Freq] = lg("Frequency", "Hz", 20, 8000, 220);
    ps[Feedback] = lin("Feedback", "%", -0.95, 0.95, 0.5);
    ps[Damp] = lin("Damp", "%", 0, 1, 0.3);
    ps[Stereo] = lin("Stereo", "st", 0, 12, 0);
    ps[Polarity] = ch("Polarity", kPolarities, 0);
    ps[Mix] = mix(0.5);
    v.push_back(make("gl.minitines", ps,
                     {{"Metal Pipe", {{Freq, 180.f}, {Feedback, 0.85f}, {Damp, 0.1f}}},
                      {"Flutter", {{Freq, 3000.f}, {Feedback, 0.3f}}},
                      {"Hollow", {{Freq, 400.f}, {Feedback, 0.6f}, {Polarity, 1.f}}},
                      {"Wide Comb", {{Freq, 300.f}, {Feedback, 0.7f}, {Stereo, 7.f}}}}));
  }
  {
    using namespace chime;
    std::vector<ParamDesc> ps(Count);
    ps[Note] = lin("Note", "note", 24, 96, 57);
    ps[Decay] = lg("Decay", "ms", 5, 4000, 300);
    ps[Bright] = lin("Bright", "%", 0, 1, 0.6);
    ps[Partials] = ch("Partials", kChimePartials, 0);
    ps[Mix] = mix(0.5);
    v.push_back(make("gl.minichime", ps,
                     {{"Bell Tone", {{Partials, 2.f}, {Decay, 1500.f}}},
                      {"Plucked A", {{Note, 45.f}, {Decay, 150.f}}},
                      {"Odd Drone", {{Note, 36.f}, {Partials, 1.f}, {Decay, 3000.f}}},
                      {"Glass", {{Note, 84.f}, {Bright, 0.9f}, {Decay, 900.f}}}}));
  }
  // ---- dynamics ----
  {
    using namespace press;
    std::vector<ParamDesc> ps(Count);
    ps[Threshold] = lin("Threshold", "dB", -60, 0, -18);
    ps[Ratio] = lg("Ratio", "x", 1, 20, 4);
    ps[Attack] = lg("Attack", "ms", 0.1, 200, 10);
    ps[Release] = lg("Release", "ms", 5, 2000, 120);
    ps[Knee] = lin("Knee", "dB", 0, 24, 6);
    ps[Makeup] = lin("Makeup", "dB", 0, 24, 0);
    ps[Detect] = ch("Detect", kDetects, 0);
    ps[Sidechain] = tog("Sidechain", 0);
    PluginInfo p = make("gl.minipress", ps,
                        {{"Glue", {{Threshold, -20.f}, {Ratio, 2.f}, {Attack, 30.f}, {Release, 200.f}, {Knee, 10.f}, {Makeup, 3.f}}},
                         {"Vocal Even", {{Threshold, -24.f}, {Ratio, 4.f}, {Attack, 5.f}, {Release, 80.f}, {Makeup, 6.f}}},
                         {"Smash", {{Threshold, -40.f}, {Ratio, 20.f}, {Attack, 0.5f}, {Release, 60.f}, {Knee, 0.f}, {Makeup, 18.f}}},
                         {"Slow Lift", {{Threshold, -30.f}, {Ratio, 3.f}, {Attack, 80.f}, {Release, 400.f}, {Makeup, 6.f}, {Detect, 1.f}}}});
    v.push_back(std::move(p));
  }
  {
    using namespace ceiling;
    std::vector<ParamDesc> ps(Count);
    ps[Input] = lin("Input", "dB", 0, 24, 0);
    ps[Ceiling] = lin("Ceiling", "dB", -12, 0, -0.3);
    ps[Release] = lg("Release", "ms", 1, 1000, 60);
    v.push_back(make("gl.miniceiling", ps,
                     {{"Loud", {{Input, 8.f}}},
                      {"Safe Peaks", {{Ceiling, -1.f}}},
                      {"Pumping", {{Input, 14.f}, {Release, 20.f}}},
                      {"Gentle", {{Input, 4.f}, {Release, 300.f}}}}));
  }
  {
    using namespace shear;
    std::vector<ParamDesc> ps(Count);
    ps[Drive] = lin("Drive", "dB", 0, 24, 0);
    ps[Ceiling] = lin("Ceiling", "dB", -24, 0, 0);
    ps[Soft] = lin("Softness", "%", 0, 1, 0.2);
    ps[Output] = lin("Output", "dB", -24, 12, 0);
    v.push_back(make("gl.minishear", ps,
                     {{"Hard Edge", {{Drive, 6.f}, {Soft, 0.f}}},
                      {"Round Edge", {{Drive, 6.f}, {Soft, 0.8f}}},
                      {"Drum Bus", {{Drive, 4.f}, {Soft, 0.4f}}},
                      {"Crunch", {{Drive, 18.f}, {Soft, 0.f}, {Output, -10.f}}}}));
  }
  {
    using namespace pack;
    std::vector<ParamDesc> ps(Count);
    ps[Threshold] = lin("Threshold", "dB", -48, 0, -12);
    ps[Amount] = lin("Amount", "%", 0, 2, 1);
    ps[Attack] = lin("Attack", "ms", 0, 10, 2);
    ps[Release] = lg("Release", "ms", 5, 1000, 80);
    ps[Link] = lin("Link", "%", 0, 1, 1);
    ps[Detect] = ch("Detect", kDetects, 0);
    v.push_back(make("gl.minipack", ps,
                     {{"Level Out", {{Threshold, -18.f}}},
                      {"Flatten", {{Threshold, -30.f}, {Amount, 1.6f}}},
                      {"Breathe", {{Amount, 0.6f}, {Release, 300.f}}},
                      {"Inverted", {{Threshold, -24.f}, {Amount, 2.f}}}}));
  }
  {
    using namespace span;
    std::vector<ParamDesc> ps(Count);
    ps[LowThresh] = lin("Low Threshold", "dB", -70, 0, -45);
    ps[LowRatio] = lin("Low Amount", "%", -1, 1, 0);
    ps[HighThresh] = lin("High Threshold", "dB", -40, 0, -12);
    ps[HighRatio] = lg("High Ratio", "x", 1, 20, 1);
    ps[Speed] = lin("Speed", "%", 0, 1, 0.5);
    ps[Output] = lin("Output", "dB", -24, 24, 0);
    v.push_back(make("gl.minispan", ps,
                     {{"Lift Quiet", {{LowThresh, -40.f}, {LowRatio, 0.6f}}},
                      {"Tame Peaks", {{HighThresh, -10.f}, {HighRatio, 4.f}}},
                      {"Both Ends", {{LowRatio, 0.5f}, {HighRatio, 3.f}}},
                      {"Expand Noise", {{LowThresh, -50.f}, {LowRatio, -0.8f}}}}));
  }
  {
    using namespace latch;
    std::vector<ParamDesc> ps(Count);
    ps[Threshold] = lin("Threshold", "dB", -80, 0, -40);
    ps[Hysteresis] = lin("Hysteresis", "dB", 0, 20, 6);
    ps[Attack] = lg("Attack", "ms", 0.1, 100, 1);
    ps[Hold] = lg("Hold", "ms", 1, 1000, 30);
    ps[Release] = lg("Release", "ms", 5, 2000, 120);
    ps[Floor] = lin("Floor", "dB", -80, 0, -80);
    ps[Flip] = tog("Flip", 0);
    ps[Sidechain] = tog("Sidechain", 0);
    v.push_back(make("gl.minilatch", ps,
                     {{"Tight Drums", {{Threshold, -30.f}, {Hold, 10.f}, {Release, 40.f}}},
                      {"Noise Floor", {{Threshold, -55.f}, {Floor, -24.f}, {Release, 300.f}}},
                      {"Duck", {{Threshold, -30.f}, {Floor, -18.f}, {Flip, 1.f}}},
                      {"Choppy", {{Threshold, -20.f}, {Hold, 1.f}, {Release, 5.f}}}}));
  }
  {
    using namespace punch;
    std::vector<ParamDesc> ps(Count);
    ps[Attack] = lin("Attack", "%", -1, 1, 0);
    ps[Sustain] = lin("Sustain", "%", -1, 1, 0);
    ps[Speed] = lg("Speed", "times", 0.25, 4, 1);
    ps[Output] = lin("Output", "dB", -24, 12, 0);
    ps[Clip] = tog("Clip", 0);
    v.push_back(make("gl.minipunch", ps,
                     {{"Snap Up", {{Attack, 0.6f}}},
                      {"Tighten", {{Sustain, -0.6f}}},
                      {"Room Bloom", {{Attack, -0.2f}, {Sustain, 0.7f}}},
                      {"Smack", {{Attack, 1.f}, {Output, -3.f}, {Clip, 1.f}}}}));
  }
  // ---- drive ----
  {
    using namespace burn;
    std::vector<ParamDesc> ps(Count);
    ps[Drive] = lin("Drive", "dB", 0, 36, 12);
    ps[Type] = ch("Type", kBurnTypes, 0);
    ps[Bias] = lin("Bias", "%", -1, 1, 0);
    ps[Tone] = lin("Tone", "%", -1, 1, 0);
    ps[Output] = lin("Output", "dB", -24, 12, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.miniburn", ps,
                     {{"Warm Tape", {{Drive, 8.f}}},
                      {"Tube Edge", {{Drive, 18.f}, {Type, 1.f}, {Bias, 0.2f}}},
                      {"Wavefolder", {{Drive, 20.f}, {Type, 3.f}}},
                      {"Sine Grit", {{Drive, 14.f}, {Type, 4.f}}},
                      {"Low Steps", {{Drive, 10.f}, {Type, 5.f}, {Mix, 0.6f}}}}));
  }
  {
    using namespace bend;
    std::vector<ParamDesc> ps(Count);
    ps[Shape] = ch("Shape", kBendShapes, 0);
    ps[Amount] = lin("Amount", "%", 0, 1, 0.5);
    ps[Drive] = lin("Drive", "dB", -6, 24, 0);
    ps[Edge] = ch("Edge", kBendEdges, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minibend", ps,
                     {{"Soft S", {{Amount, 0.7f}}},
                      {"Steep", {{Shape, 1.f}, {Amount, 1.f}, {Drive, 6.f}}},
                      {"Stairs", {{Shape, 2.f}, {Amount, 0.8f}}},
                      {"Ripple Fold", {{Shape, 3.f}, {Drive, 12.f}, {Edge, 1.f}}},
                      {"Bounce Wrap", {{Shape, 5.f}, {Drive, 8.f}, {Edge, 2.f}}}}));
  }
  {
    using namespace warp;
    std::vector<ParamDesc> ps(Count);
    ps[Drive] = lin("Drive", "%", 0, 1, 0.3);
    ps[Tone] = lg("Tone", "Hz", 20, 10000, 600);
    ps[Bias] = lin("Bias", "%", -1, 1, 0);
    ps[Stereo] = lin("Stereo", "%", 0, 1, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.miniwarp", ps,
                     {{"Brighten", {{Drive, 0.3f}, {Tone, 2000.f}}},
                      {"Growl", {{Drive, 0.8f}, {Tone, 200.f}}},
                      {"Wide Warp", {{Drive, 0.5f}, {Stereo, 0.8f}}},
                      {"Edge", {{Drive, 1.f}, {Tone, 6000.f}, {Mix, 0.7f}}}}));
  }
  {
    using namespace crumble;
    std::vector<ParamDesc> ps(Count);
    ps[Rate] = lg("Rate", "Hz", 200, 48000, 12000);
    ps[Bits] = lin("Bits", "bits", 1, 16, 16);
    ps[Jitter] = lin("Jitter", "%", 0, 1, 0);
    ps[Smooth] = lin("Smooth", "%", 0, 1, 0);
    ps[Dither] = lin("Dither", "%", 0, 1, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minicrumble", ps,
                     {{"Eight Bit", {{Rate, 22000.f}, {Bits, 8.f}}},
                      {"Phone Grit", {{Rate, 4000.f}, {Bits, 10.f}, {Smooth, 0.6f}}},
                      {"Broken", {{Rate, 1500.f}, {Bits, 4.f}, {Jitter, 0.7f}}},
                      {"Dusty", {{Rate, 18000.f}, {Bits, 12.f}, {Dither, 0.5f}, {Mix, 0.7f}}}}));
  }
  {
    using namespace clang;
    std::vector<ParamDesc> ps(Count);
    ps[Freq] = lg("Frequency", "Hz", 1, 5000, 220);
    ps[Shape] = ch("Shape", kClangShapes, 0);
    ps[Bias] = lin("Bias", "%", 0, 1, 0);
    ps[Spread] = lin("Spread", "%", 0, 1, 0);
    ps[Source] = ch("Source", kClangSources, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.miniclang", ps,
                     {{"Robot", {{Freq, 90.f}}},
                      {"Bell Tone", {{Freq, 740.f}, {Bias, 0.2f}}},
                      {"Tremble", {{Freq, 6.f}, {Bias, 0.5f}}},
                      {"Square Clank", {{Freq, 330.f}, {Shape, 2.f}}},
                      {"Wide Metal", {{Freq, 500.f}, {Spread, 0.6f}}}}));
  }
  // ---- pitch ----
  {
    using namespace slide;
    std::vector<ParamDesc> ps(Count);
    ps[Shift] = lin("Shift", "Hz", -2000, 2000, 0);
    ps[Stereo] = lin("Stereo", "Hz", 0, 100, 0);
    ps[Feedback] = lin("Feedback", "%", 0, 0.9, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minislide", ps,
                     {{"Detune Wide", {{Stereo, 6.f}, {Mix, 0.5f}}},
                      {"Rising Loop", {{Shift, 40.f}, {Feedback, 0.7f}, {Mix, 0.5f}}},
                      {"Inharmonic", {{Shift, 300.f}}},
                      {"Down Grime", {{Shift, -150.f}, {Feedback, 0.3f}}}}));
  }
  {
    using namespace interval;
    std::vector<ParamDesc> ps(Count);
    ps[Pitch] = lin("Pitch", "st", -24, 24, 0);
    ps[Fine] = lin("Fine", "ct", -100, 100, 0);
    ps[Grain] = lg("Grain", "ms", 20, 200, 60);
    ps[Jitter] = lin("Jitter", "%", 0, 1, 0);
    ps[Mix] = mix(1);
    v.push_back(make("gl.miniinterval", ps,
                     {{"Octave Up", {{Pitch, 12.f}}},
                      {"Octave Down", {{Pitch, -12.f}}},
                      {"Fifth Blend", {{Pitch, 7.f}, {Mix, 0.5f}}},
                      {"Thick Detune", {{Fine, 12.f}, {Grain, 40.f}, {Mix, 0.5f}}},
                      {"Grain Shimmer", {{Pitch, 12.f}, {Jitter, 0.6f}, {Mix, 0.4f}}}}));
  }
  // ---- motion ----
  {
    using namespace dbl;
    std::vector<ParamDesc> ps(Count);
    ps[Delay] = lg("Delay", "ms", 1, 40, 8);
    ps[Depth] = lin("Depth", "ms", 0, 20, 3);
    ps[Rate] = lg("Rate", "Hz", 0.05, 8, 0.5);
    ps[Voices] = ch("Voices", kVoiceCounts, 0);
    ps[Spread] = lin("Spread", "%", 0, 1, 0.8);
    ps[Mix] = mix(0.5);
    v.push_back(make("gl.minidouble", ps,
                     {{"Lush", {{Voices, 2.f}, {Depth, 6.f}, {Rate, 0.3f}}},
                      {"Vibrato", {{Delay, 3.f}, {Depth, 2.f}, {Rate, 5.f}, {Mix, 1.f}}},
                      {"Doubler", {{Delay, 25.f}, {Depth, 1.f}, {Rate, 0.2f}}},
                      {"Thin Shimmer", {{Delay, 2.f}, {Depth, 1.5f}, {Rate, 1.5f}, {Voices, 1.f}}}}));
  }
  {
    using namespace crowd;
    std::vector<ParamDesc> ps(Count);
    ps[Voices] = lin("Voices", "", 2, 12, 6);
    ps[Detune] = lin("Detune", "%", 0, 1, 0.3);
    ps[Spread] = lin("Spread", "%", 0, 1, 0.6);
    ps[Motion] = ch("Motion", kCrowdMotions, 0);
    ps[Mix] = mix(0.6);
    v.push_back(make("gl.minicrowd", ps,
                     {{"Choir Room", {{Voices, 8.f}, {Detune, 0.25f}}},
                      {"Wall", {{Voices, 12.f}, {Detune, 0.6f}, {Spread, 1.f}}},
                      {"Subtle Width", {{Voices, 4.f}, {Detune, 0.1f}}},
                      {"Swirl", {{Motion, 1.f}, {Detune, 0.4f}}}}));
  }
  {
    using namespace jet;
    std::vector<ParamDesc> ps(Count);
    ps[Delay] = lg("Delay", "ms", 0.1, 10, 1);
    ps[Depth] = lin("Depth", "%", 0, 1, 0.5);
    ps[Rate] = lg("Rate", "Hz", 0.02, 10, 0.25);
    ps[Feedback] = lin("Feedback", "%", -0.95, 0.95, 0.5);
    ps[Stereo] = lin("Stereo", "%", 0, 1, 0.5);
    ps[Mix] = mix(0.5);
    v.push_back(make("gl.minijet", ps,
                     {{"Jet Plane", {{Feedback, 0.8f}, {Rate, 0.1f}}},
                      {"Metallic", {{Delay, 3.f}, {Feedback, -0.85f}}},
                      {"Slow Wash", {{Rate, 0.05f}, {Depth, 0.9f}}},
                      {"Fast Wobble", {{Rate, 6.f}, {Depth, 0.3f}, {Feedback, 0.2f}}}}));
  }
  {
    using namespace orbit;
    std::vector<ParamDesc> ps(Count);
    ps[Stages] = ch("Stages", kOrbitStages, 1);
    ps[Centre] = lg("Centre", "Hz", 50, 8000, 700);
    ps[Depth] = lin("Depth", "%", 0, 1, 0.6);
    ps[Rate] = lg("Rate", "Hz", 0.02, 8, 0.4);
    ps[Feedback] = lin("Feedback", "%", -0.9, 0.9, 0.3);
    ps[Stereo] = lin("Stereo", "%", 0, 1, 0.5);
    ps[Mix] = mix(0.5);
    v.push_back(make("gl.miniorbit", ps,
                     {{"Deep Twelve", {{Stages, 4.f}, {Feedback, 0.6f}}},
                      {"Slow Swirl", {{Rate, 0.08f}, {Depth, 0.9f}}},
                      {"Bubbles", {{Stages, 0.f}, {Rate, 3.f}, {Feedback, 0.7f}}},
                      {"Notch Sweep", {{Stages, 3.f}, {Feedback, -0.5f}, {Rate, 0.2f}}}}));
  }
  // ---- time ----
  {
    using namespace echo;
    std::vector<ParamDesc> ps(Count);
    ps[Time] = lg("Time", "ms", 1, 2000, 300);
    ps[Sync] = tog("Sync", 1);
    ps[Steps] = ch("Note", kNoteNames, 7);
    ps[Feedback] = lin("Feedback", "%", 0, 1, 0.4);
    ps[PingPong] = tog("Ping Pong", 0);
    ps[Tone] = lin("Tone", "%", -1, 1, 0);
    ps[Duck] = lin("Duck", "%", 0, 1, 0);
    ps[Mix] = mix(0.35);
    v.push_back(make("gl.miniecho", ps,
                     {{"Ping Pong Eighths", {{Steps, 5.f}, {PingPong, 1.f}}},
                      {"Slapback", {{Sync, 0.f}, {Time, 90.f}, {Feedback, 0.1f}}},
                      {"Dub Dark", {{Steps, 8.f}, {Feedback, 0.7f}, {Tone, -0.7f}}},
                      {"Ducked Vocal", {{Duck, 0.7f}, {Mix, 0.4f}}}}));
  }
  {
    using namespace volley;
    std::vector<ParamDesc> ps(Count);
    ps[Time] = lg("Time", "ms", 10, 2000, 250);
    ps[Ratio] = lg("Ratio", "times", 0.25, 4, 1.5);
    ps[Feedback] = lin("Feedback", "%", 0, 1, 0.4);
    ps[Cross] = lin("Cross", "%", 0, 1, 0.4);
    ps[Tone] = lin("Tone", "%", -1, 1, 0);
    ps[Mix] = mix(0.35);
    v.push_back(make("gl.minivolley", ps,
                     {{"Three Two", {{Time, 300.f}, {Ratio, 1.5f}}},
                      {"Cross Chaos", {{Feedback, 0.8f}, {Cross, 1.f}}},
                      {"Short Space", {{Time, 40.f}, {Ratio, 1.3f}, {Feedback, 0.5f}}},
                      {"Dark Repeats", {{Time, 420.f}, {Ratio, 0.75f}, {Tone, -0.6f}, {Feedback, 0.6f}}}}));
  }
  {
    using namespace vault;
    std::vector<ParamDesc> ps(Count);
    ps[Size] = lin("Size", "%", 0, 1, 0.5);
    ps[Decay] = lg("Decay", "s", 0.2, 30, 2.5);
    ps[Damp] = lin("Damp", "%", 0, 1, 0.4);
    ps[PreDelay] = lin("Pre-delay", "ms", 0, 200, 10);
    ps[Width] = lin("Width", "%", 0, 1, 1);
    ps[Early] = lin("Early", "%", 0, 1, 0.3);
    ps[Mix] = mix(0.3);
    v.push_back(make("gl.minivault", ps,
                     {{"Small Room", {{Size, 0.3f}, {Decay, 0.8f}}},
                      {"Hall", {{Size, 0.7f}, {Decay, 3.f}, {PreDelay, 20.f}}},
                      {"Cathedral", {{Size, 1.f}, {Decay, 9.f}, {Damp, 0.3f}}},
                      {"Dark Plate", {{Size, 0.4f}, {Decay, 2.f}, {Damp, 0.8f}, {Early, 0.f}}},
                      {"Endless", {{Size, 0.8f}, {Decay, 30.f}, {Mix, 0.5f}}}}));
  }
  {
    using namespace rewind;
    std::vector<ParamDesc> ps(Count);
    ps[Length] = lg("Length", "ms", 30, 2000, 250);
    ps[Sync] = tog("Sync", 1);
    ps[Steps] = ch("Note", kRewindNames, 2);
    ps[Fade] = lin("Fade", "%", 0, 0.5, 0.1);
    ps[Mix] = mix(1);
    v.push_back(make("gl.minirewind", ps,
                     {{"Eighth Stutter", {{Steps, 1.f}, {Mix, 0.6f}}},
                      {"Bar Rewind", {{Steps, 4.f}}},
                      {"Free 120 ms", {{Sync, 0.f}, {Length, 120.f}}},
                      {"Soft Halves", {{Steps, 3.f}, {Fade, 0.4f}, {Mix, 0.5f}}}}));
  }
  {
    using namespace halt;
    std::vector<ParamDesc> ps(Count);
    ps[Run] = tog("Run", 1);
    ps[StopTime] = lg("Stop Time", "ms", 20, 4000, 600);
    ps[StartTime] = lg("Start Time", "ms", 20, 4000, 300);
    ps[Curve] = lg("Curve", "", 0.25, 4, 1);
    v.push_back(make("gl.minihalt", ps,
                     {{"Quick Stop", {{StopTime, 200.f}}},
                      {"Slow Down", {{StopTime, 2500.f}, {Curve, 0.5f}}},
                      {"Spin Up", {{StartTime, 1200.f}}},
                      {"Brake", {{StopTime, 400.f}, {Curve, 3.f}}}}));
  }
  {
    using namespace pulse;
    std::vector<ParamDesc> ps(Count);
    static const char* const kStepNames[kSteps] = {"Step 1", "Step 2",  "Step 3",  "Step 4",  "Step 5",  "Step 6",
                                                   "Step 7", "Step 8",  "Step 9",  "Step 10", "Step 11", "Step 12",
                                                   "Step 13", "Step 14", "Step 15", "Step 16"};
    for (int i = 0; i < kSteps; ++i) ps[static_cast<size_t>(i)] = tog(kStepNames[i], i % 2 == 0 ? 1 : 0);
    ps[Rate] = ch("Rate", kPulseRates, 2);
    ps[Attack] = lg("Attack", "ms", 0.5, 200, 3);
    ps[Release] = lg("Release", "ms", 1, 500, 40);
    ps[Floor] = lin("Floor", "dB", -60, 0, -60);
    ps[Swing] = lin("Swing", "%", 0, 1, 0);
    ps[Mix] = mix(1);
    auto pattern = [](const char* bits, std::vector<std::pair<int, float>> extra) {
      std::vector<std::pair<int, float>> vals;
      for (int i = 0; i < kSteps; ++i) vals.push_back({Step0 + i, bits[i] == '1' ? 1.f : 0.f});
      for (auto& e : extra) vals.push_back(e);
      return vals;
    };
    v.push_back(make("gl.minipulse", ps,
                     {{"Driving Chop", pattern("1101101110110110", {})},
                      {"Triplet Flutter", pattern("1101101101101101", {{Rate, 1.f}})},
                      {"Soft Pump", pattern("0111011101110111", {{Attack, 30.f}, {Release, 120.f}, {Floor, -12.f}})},
                      {"Stutter", pattern("1110111011101100", {{Rate, 4.f}, {Release, 10.f}})},
                      {"Swung Eighths", pattern("1010101010101010", {{Rate, 0.f}, {Swing, 0.5f}})}}));
  }
}

}  // namespace gl::packs::mini
