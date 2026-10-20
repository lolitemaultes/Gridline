// Gridline — Envelope Controller articulator model (J-41): the data half of
// gl.envctl model 1. PluginState::data holds, as text, the per-articulator
// records (envelope, LFO amplitude envelope and the five maps), flags and
// key ranges; the automatable knobs are parameters (modulation.h artParam).
// The FL Envelope Controller decoder / encoder and the editor share it.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/FlArticulator.h"
#include "../../engine/Generators.h"
#include "modulation.h"

namespace gl::packs::modulation {

enum EnvcMap { MapKey, MapVel, MapModX, MapModY, MapRandom, kEnvcMaps };

struct EnvcArt {
  int32_t flags = 1;  // bit 0: bipolar sum (centre 0.5), else unipolar product
  int32_t version = 3;
  int32_t keyLo = 0, keyHi = 256;  // note range that triggers it (FL notes, C5 = 60)
  flart::Record env, lfo;
  std::array<flart::Record, kEnvcMaps> maps;
  bool bipolar() const { return (flags & 1) != 0; }
  bool operator==(const EnvcArt& o) const;
};

struct EnvcModel {
  int32_t version = 0x00060003;  // state version word
  int32_t cur = 0, part = 0, sel = 0;  // editor: articulator / part shown
  int32_t uiBits = 2;                  // option bits other than "continuous output"
  std::array<EnvcArt, kArts> arts;
  bool operator==(const EnvcModel& o) const;
};

// A fresh controller: every articulator bipolar with the default ADSR
// (articulator 1's envelope enabled), LFOs off, neutral maps.
EnvcModel defaultEnvc();
// Text form kept in PluginState::data ("" = defaultEnvc()).
std::string writeEnvc(const EnvcModel& m);
bool readEnvc(const std::string& text, EnvcModel& out);
// The model of a state (defaults when data is empty or unreadable).
EnvcModel envcOf(const std::string& data);

// ---- FL state (plugin data event) ---------------------------------------------
// Version word, 90 knob ints (8 × 11 + mod X / Y; 44 before version 6),
// editor ints, option bits, then per articulator: flags, the articulator
// record (version, base / envelope / LFO amounts as floats, envelope + extra,
// LFO + extra, five maps) and the key range (spec §1).
inline constexpr int kEnvcKnobs = kArts * AfFields + 2;
struct EnvcState {
  EnvcModel model;
  std::array<int32_t, kEnvcKnobs> knobs{};
  int32_t options = 2;  // bit 2: continuous output
  // Articulator record floats / extras that differ from the knobs (FL then
  // uses the knob values); reported, not kept.
  bool recordsMatchKnobs = true;
};
EnvcState defaultEnvcState();
bool parseEnvcState(const uint8_t* p, size_t n, EnvcState& out);
std::vector<uint8_t> writeEnvcState(const EnvcState& s);
// Knob ints ↔ Gridline parameter values (exact: power-of-two divisors).
double envcKnobToParam(int knob, int32_t v);
int32_t envcParamToKnob(int knob, double v);
inline int envcKnobParam(int knob) { return knob < kArts * AfFields ? EcArt + knob : (knob == kArts * AfFields ? EcModX : EcModY); }

// ---- runtime -----------------------------------------------------------------
// Prepared model (UI thread; ChannelAux::custom of a gl.envctl channel).
struct EnvcAux final : AuxData {
  EnvcModel model;
  struct Art {
    flart::Prepared env, lfo;
    std::array<flart::Prepared, kEnvcMaps> maps;
    bool bipolar = true;
  };
  std::array<Art, kArts> arts;
};
std::shared_ptr<const AuxData> prepareEnvc(const PluginState& s);

// The articulator engine (spec §2–§5): eight monophonic articulators run at
// the FL tick rate; outputs 0..1 per articulator.
class EnvcRuntime {
 public:
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx);
  void noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props);
  void noteOff(uint32_t id);
  void allOff();
  void kill();
  void render(int n);
  int outputs(float* out, int max) const;
  // Ticks rendered so far (tests).
  uint64_t ticks() const { return ticks_; }

 private:
  struct Run {
    bool attached = false;
    uint32_t voice = 0;
    double pitch = 60, vx = 0, vy = 0, velRand = 0;
    flart::EnvState env, amp;
    uint32_t phase = 0;
    double out = 0;
  };
  void tick();
  double idle(int a) const;
  const PluginState* st_ = nullptr;
  const EnvcAux* aux_ = nullptr;
  ProcessContext ctx_;
  std::array<Run, kArts> run_{};
  std::array<uint32_t, kArts> gphase_{};
  double untilTick_ = 0;
  bool primed_ = false;
  uint64_t ticks_ = 0;
  uint64_t rng_ = 0x9E3779B97F4A7C15ull;
};

}  // namespace gl::packs::modulation
