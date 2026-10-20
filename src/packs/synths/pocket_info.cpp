// Gridline — "synths" pack, J-33 group "pocket": catalogue entry and factory
// presets of Pocket Synth (gl.pocketsynth). Every control keeps the
// reference's 0..1 position (switches included: the runtime quantises them the
// way the reference does); the editor shows the physical values.
#include <string>
#include <vector>
#include "../Packs.h"
#include "pocket.h"

namespace gl::packs::synths {

namespace {

using namespace pocket;

ParamDesc pct(const std::string& n, double d) { return ParamDesc{n, "%", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; }

// The reference's built-in defaults.
constexpr float kDefaults[PkCount] = {0.375f, 0.5f, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0.5f, 0, 0, 0.25f, 0.5f, 0, 1, 1,
                                      0.07f, 0, 0, 0, 0, 0.7f, 0.5f, 1, 0.5f, 0, 0.25f, 0.5f, 0.5f, 0, 0.1122f, 0.4f, 0.1f, 1,
                                      0.5f};

}  // namespace

PluginInfo pocketSynthInfo() {
  static const char* names[PkCount] = {"Voice mode", "Transpose", "Slide", "Wheel target", "Wheel amount", "Oscillator kind",
                                       "Oscillator mod", "Noise", "Cutoff", "Peak", "Key tracking", "Velocity to cutoff",
                                       "Filter attack", "Filter decay", "Filter sustain", "Filter release", "Filter env amount",
                                       "LFO shape", "LFO target", "LFO rate", "LFO amount", "Amp attack", "Amp decay",
                                       "Amp sustain", "Amp release", "Drive on", "Drive", "Decimate", "Delay on", "Delay time",
                                       "Delay feedback", "Delay tone", "Delay mix", "Chorus on", "Chorus rate", "Chorus depth",
                                       "Chorus mix", "Phaser on", "Phaser rate", "Phaser width", "Phaser feedback",
                                       "Phaser mix", "Phaser center"};
  std::vector<ParamDesc> q;
  for (int i = 0; i < PkCount; ++i) q.push_back(pct(names[i], kDefaults[i]));
  auto P = [](std::initializer_list<std::pair<int, float>> v) { return std::vector<std::pair<int, float>>(v); };
  std::vector<PluginInfo::Preset> presets = {
      {"Fat Bass", P({{PkVoiceMode, 0.5f}, {PkOscKind, 1.f / 9}, {PkOscMod, 0.52f}, {PkCutoff, 0.35f}, {PkPeak, 0.35f},
                      {PkFltAmount, 0.75f}, {PkFltD, 0.45f}, {PkFltS, 0.2f}, {PkAmpR, 0.2f}})},
      {"Super Lead", P({{PkVoiceMode, 1.f}, {PkOscKind, 5.f / 9}, {PkOscMod, 0.6f}, {PkSlide, 0.4f}, {PkCutoff, 0.7f},
                        {PkDelay, 1.f}, {PkDelayMix, 0.4f}, {PkChorus, 1.f}})},
      {"Bell Keys", P({{PkOscKind, 6.f / 9}, {PkOscMod, 0.3f}, {PkAmpD, 0.55f}, {PkAmpS, 0.f}, {PkAmpR, 0.4f}, {PkPhaser, 1.f},
                       {PkPhaserMix, 0.5f}})},
      {"Spread Pad", P({{PkOscKind, 8.f / 9}, {PkOscMod, 0.5f}, {PkAmpA, 0.35f}, {PkAmpR, 0.6f}, {PkCutoff, 0.6f},
                        {PkLfoAmount, 0.65f}, {PkLfoRate, 0.55f}, {PkChorus, 1.f}})},
  };
  PluginInfo p{"gl.pocketsynth", "Pocket Synth", "Synth", PluginKind::Generator, std::move(q), false, std::move(presets)};
  p.editorWidth = 760;
  return p;
}

}  // namespace gl::packs::synths
