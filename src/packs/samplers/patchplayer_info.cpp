// Gridline: the Patch Player's catalogue entry (gl.patchplayer).
#include <cstdio>
#include "../Packs.h"
#include "patchplayer.h"

namespace gl::packs::samplers {

namespace {
ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc ch(const char* n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
// "Chosen" (the patch picked in the editor, saved with the file), then the numbers 000..n-1: a
// number chooses that bank or program instead (automation moves it in whole steps).
ParamDesc numbers(const char* n, int count) {
  std::vector<std::string> c = {"Chosen"};
  for (int i = 0; i < count; ++i) {
    char b[8];
    std::snprintf(b, sizeof b, "%03d", i);
    c.emplace_back(b);
  }
  return ch(n, std::move(c), 0);
}
}  // namespace

PluginInfo patchPlayerInfo() {
  PluginInfo p;
  p.type = kPatchPlayerType;
  p.name = "Patch Player";
  p.category = "Samplers";
  p.kind = PluginKind::Generator;
  p.usesSample = true;  // line 0 of data is the SF2 file (found, relinked and shared like a sample)
  p.editorWidth = 1028;
  p.params.resize(PpCount);
  p.params[PpBank] = numbers("Bank", 129);
  p.params[PpPatch] = numbers("Patch", 128);
  p.params[PpVolume] = lin("Volume", "dB", -60, 12, -3);
  p.params[PpPan] = lin("Pan", "", -1, 1, 0);
  p.params[PpReverb] = lin("Reverb", "%", 0, 1, 0);
  p.params[PpChorus] = lin("Chorus", "%", 0, 1, 0);
  p.params[PpTranspose] = lin("Transpose", "st", -48, 48, 0);
  p.params[PpFine] = lin("Fine", "ct", -100, 100, 0);
  p.params[PpBendRange] = lin("Bend range", "st", 0, 24, 2);
  p.params[PpGlide] = lin("Glide", "ms", 0, 2000, 0);
  p.params[PpPolyphony] = lin("Polyphony", "", 1, kPpMaxVoices, 64);
  p.params[PpMono] = ch("Voices", {"Poly", "Mono"}, 0);
  p.params[PpVelocity] = ch("Velocity", {"Normal", "Soft", "Hard", "Fixed"}, 0);
  p.params[PpCutoff] = lin("Cutoff", "%", -1, 1, 0);
  p.params[PpResonance] = lin("Resonance", "%", -1, 1, 0);
  p.params[PpKeyTrack] = lin("Key track", "%", 0, 2, 0);
  p.params[PpEnvMode] = ch("Envelope", {"From the patch", "Scale", "Replace"}, 0);
  p.params[PpAttackScale] = lg("Attack scale", "times", 0.1, 10, 1);
  p.params[PpDecayScale] = lg("Decay scale", "times", 0.1, 10, 1);
  p.params[PpSustainScale] = lin("Sustain scale", "times", 0, 2, 1);
  p.params[PpReleaseScale] = lg("Release scale", "times", 0.1, 10, 1);
  p.params[PpAttack] = lg("Attack", "s", 0.001, 10, 0.01);
  p.params[PpDecay] = lg("Decay", "s", 0.01, 20, 1);
  p.params[PpSustain] = lin("Sustain", "%", 0, 1, 1);
  p.params[PpRelease] = lg("Release", "s", 0.005, 20, 0.3);
  p.params[PpVibMode] = ch("Vibrato", {"From the patch", "Own"}, 0);
  p.params[PpVibDepth] = lin("Vibrato depth", "ct", 0, 200, 0);
  p.params[PpVibRate] = lg("Vibrato rate", "Hz", 0.1, 20, 5);
  p.params[PpVibDelay] = lin("Vibrato delay", "ms", 0, 5000, 0);
  p.params[PpModWheel] = lin("Mod wheel", "ct", 0, 200, 50);
  p.presets = {
      {"As the file plays it", {}},
      {"Soft and wide", {{PpVelocity, 1.f}, {PpReverb, 0.35f}, {PpChorus, 0.25f}, {PpEnvMode, 1.f}, {PpReleaseScale, 1.6f}}},
      {"Tight mono lead", {{PpMono, 1.f}, {PpGlide, 60.f}, {PpEnvMode, 1.f}, {PpReleaseScale, 0.4f}, {PpVibMode, 1.f}, {PpVibDepth, 18.f},
                           {PpVibRate, 5.5f}, {PpVibDelay, 250.f}}},
      {"Darker", {{PpCutoff, -0.35f}, {PpResonance, 0.15f}}},
      {"Slow swell", {{PpEnvMode, 1.f}, {PpAttackScale, 6.f}, {PpReleaseScale, 2.5f}, {PpReverb, 0.3f}}},
  };
  return p;
}

}  // namespace gl::packs::samplers
