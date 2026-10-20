// Gridline — "flex" pack (J-53): the stand-in player for FL Studio preset-player
// (FLEX) channels. Parameters 0..44 hold the FLEX controls exactly as FLEX
// stores them (0..1), 45..48 the stand-in's own settings (core/FlexStandin.h).
#include "../../core/FlexStandin.h"
#include "../Packs.h"

namespace gl::packs::flexpack {

namespace {

ParamDesc unit01(const std::string& n, const char* u = "%") { return ParamDesc{n, u, 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}}; }

PluginInfo info() {
  using namespace gl::flex;
  PluginInfo p{kType, "Preset Stand-in", "Imported", PluginKind::Generator, {}, false, {}};
  auto& q = p.params;
  q.resize(PCount);
  // FLEX controls: every one a 0..1 value as stored (continuous, so a value is
  // never rounded and the exported state keeps it exactly). Envelope, filter
  // and pitch controls are centred offsets on the preset's own sound.
  const char* env[5] = {"attack", "hold", "decay", "sustain", "release"};
  for (int k = 0; k < 5; ++k) q[VolA + k] = unit01(std::string("Amp ") + env[k]);
  for (int k = 0; k < 5; ++k) q[FltA + k] = unit01(std::string("Filter env ") + env[k]);
  for (int k = 0; k < 8; ++k) q[Macro1 + k] = unit01("Macro " + std::to_string(k + 1));
  q[Cutoff] = unit01("Cutoff");
  q[EnvAmt] = unit01("Filter env amount");
  q[Reso] = unit01("Resonance");
  q[PostCut] = unit01("Post filter cutoff");
  q[PostReso] = unit01("Post filter resonance");
  q[PostType] = unit01("Post filter type", "");
  q[DlyType] = unit01("Delay type", "");
  q[DlyMix] = unit01("Delay mix");
  q[DlyTime] = unit01("Delay time", "");
  q[DlyFb] = unit01("Delay feedback");
  q[DlyMod] = unit01("Delay modulation");
  q[DlyColor] = unit01("Delay tone");
  q[RevMix] = unit01("Reverb mix");
  q[RevDecay] = unit01("Reverb decay");
  q[RevSize] = unit01("Reverb size");
  q[RevMod] = unit01("Reverb modulation");
  q[RevColor] = unit01("Reverb tone");
  q[LimPre] = unit01("Limiter input");
  q[OutVol] = unit01("Output");
  q[DlyOn] = unit01("Delay on", "");
  q[RevOn] = unit01("Reverb on", "");
  q[Pitch] = unit01("Pitch", "");
  q[PostOn] = unit01("Post filter on", "");
  q[LimOn] = unit01("Limiter on", "");
  q[BandMix] = unit01("Limiter band mix");
  q[LimType] = unit01("Limiter type", "");
  q[RevModSpeed] = unit01("Reverb modulation rate");
  // Stand-in only (never written to the FL state).
  q[PTemplate] = ParamDesc{"Stand-in", "", 0, CatCount - 1, 0, ParamKind::Choice, ParamScale::Linear, catChoices()};
  q[PTone] = ParamDesc{"Stand-in tone", "", -1, 1, 0, ParamKind::Continuous, ParamScale::Linear, {}};
  q[PLevel] = ParamDesc{"Stand-in level", "dB", -24, 12, 0, ParamKind::Continuous, ParamScale::Linear, {}};
  q[POctave] = ParamDesc{"Stand-in octave", "", -2, 2, 0, ParamKind::Continuous, ParamScale::Linear, {}};
  p.presets = {{"Automatic", {}},
               {"Pad stand-in", {{PTemplate, static_cast<float>(Pad)}}},
               {"Piano stand-in", {{PTemplate, static_cast<float>(Keys)}}},
               {"Bass stand-in", {{PTemplate, static_cast<float>(Bass)}}},
               {"Drum kit stand-in", {{PTemplate, static_cast<float>(Drums)}}}};
  p.editorWidth = 1080;
  return p;
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) { v.push_back(info()); }

}  // namespace gl::packs::flexpack
