// Gridline - "tracker" pack (K-76): catalogue entry of the Tracker Sampler.
#include "../Packs.h"
#include "tracker.h"

namespace gl::packs::trackerpack {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{trk::kInstrumentType, "Tracker Sampler", "Sampler", PluginKind::Generator, {}, true, {}};
  p.params.resize(trk::PCount);
  p.params[trk::PInterp] = ch("Interpolation", {"None", "Linear", "Cubic", "Sinc"}, trk::InterpSinc);
  p.params[trk::PAmiga] = ch("Pitch table", {"Linear", "Periods"}, 0);
  p.params[trk::PRowsPerBeat] = ch("Rows per beat", {"1", "2", "4", "8", "16"}, 2);
  p.params[trk::PTicks] = lin("Ticks per row", "", 1, 31, 6);
  p.params[trk::PLevel] = lin("Level", "", 0, 2, 1);
  p.params[trk::PRamp] = tog("Click removal", 1);
  std::vector<std::string> sounds;
  for (int k = 0; k <= kSoundCount; ++k) sounds.push_back(soundName(k));
  // K-129: a new instance is silent until it gets a sample, a built-in sound or a preset. States
  // saved with a sound store it, so they keep it; states saved before the sounds have none.
  p.params[trk::PSound] = ch("Built-in sound", sounds, 0);
  p.legacyDefaults.push_back({trk::PSound, 0.0f});
  auto snd = [](int k) { return std::pair<int, float>{trk::PSound, static_cast<float>(k)}; };
  p.presets = {
      {"Glass Pulse", {snd(1)}},
      {"Velvet Saw", {snd(2)}},
      {"Tine Bell", {snd(3)}},
      {"Rubber Bass", {snd(4), {trk::PLevel, 1.2f}}},
      {"Square Pad", {snd(5), {trk::PTicks, 8.0f}}},
      {"Crunchy Pulse", {snd(1), {trk::PInterp, static_cast<float>(trk::InterpNone)}, {trk::PAmiga, 1.0f}}},
  };
  p.editorWidth = 860;
  p.editorMinWidth = 660;
  v.push_back(p);
}

}  // namespace gl::packs::trackerpack
