// Gridline — Slicex region engine (J-41): gl.slicer engine 1. Two decks of
// regions mapped to keys, region records (gain, pan, pitch, cut group,
// delay, sample start, filter, articulator groups and amounts), eight
// articulator groups (six articulators each: pan, volume, cutoff,
// resonance, speed, sample start; envelope + LFO + four maps on the shared
// FL articulator engine), layering / crossfades, the region filter
// (1–3 biquad stages), gated playback with declick, control blocks of 48
// (or 4, "HQ") samples at 44.1 kHz.
//
// Behavioural reimplementation from behaviour study notes
// (Part A and the shared envelope / LFO engine).
//
// State: gl.slicer data = "<deck A sample file>\n" + writeText(model); the
// per-region gain / pan / pitch / cut group / delay / end and the master
// level / pitch / mod X / Y are the slicer's parameters (pp::Slicer).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "FlArticulator.h"
#include "Generators.h"
#include "TimeStretch.h"

namespace gl::slicex {

inline constexpr int kGroups = 8, kTargets = 6, kRegionInts = 17, kOptBytes = 47, kDeckView = 0x4C;
enum Target { TPan, TVol, TCut, TRes, TSpeed, TStart };
// Region record ints (spec §A1.4).
enum RegionInt {
  RCut, ROutput, RPan, RVolume, RAmpGroup, RAmpAmt, RCutoff, RReso, RFiltGroup, RFiltAmt, RPitch, RSpeedGroup, RSpeedAmt, RDelay,
  RStart, RStartGroup, RStartAmt
};
inline constexpr std::array<int32_t, kRegionInts> kRegionDefault = {0, 0, 0, 8000, 0, 12800, 128, 128, 0, 12800, 0, 0, 12800, 0, 0, 0, 12800};

struct Articulator {
  int32_t version = 3;
  float floats[3] = {0, 1, 1};  // base, envelope amount, LFO amount
  flart::Record env, lfo;
  std::array<flart::Record, 4> maps;  // mod X, mod Y, velocity, random
  bool operator==(const Articulator& o) const;
};
struct Group {
  std::array<int32_t, 5> hdr = {128, 128, 128, 0, 0};  // filter env amount, cutoff, resonance, type, mode (stages)
  std::array<Articulator, kTargets> arts;
  bool operator==(const Group& o) const;
};
struct Region {
  int64_t start = 0, length = -1;  // frames (length −1: to the next region / end)
  std::array<int32_t, kRegionInts> rec = kRegionDefault;
  bool operator==(const Region& o) const { return start == o.start && length == o.length && rec == o.rec; }
};
struct Deck {
  uint8_t flag = 0;            // 1 = sample referenced by path only
  std::string rawPath;         // the path as FL stores it
  std::string file;            // resolved / cached sample file ("" = none)
  bool embedded = false;       // the state carries the audio (RIFF)
  std::vector<uint8_t> riff;   // small embedded audio kept inline (empty decks)
  std::vector<uint8_t> view;   // editor block (0x4C bytes)
  std::array<flart::Record, 4> envs;  // editing envelopes (kind 6)
  std::vector<Region> regions;
  bool operator==(const Deck& o) const;
};
struct Model {
  int32_t version = 0x00060003;
  std::array<int32_t, 8> knobs = {0, 0, 12800, 256, 128, 0, 256, 256};
  std::vector<uint8_t> opts;  // option block (47 bytes)
  std::array<int32_t, 3> view = {0, 0, 3};
  std::array<Deck, 2> decks;
  std::array<Group, kGroups> groups;
  bool operator==(const Model& o) const;
  // Option block fields.
  int layering() const;
  int crossfade() const;
  bool opt(int rel) const { return rel >= 0 && static_cast<size_t>(rel) < opts.size() && opts[static_cast<size_t>(rel)] != 0; }
};
// Option block byte offsets (relative to the block).
enum OptByte { OSmooth = 0x25, OStartWhole = 0x28, OHqEnv = 0x29, OSameDeck = 0x2A, OLoopHalf = 0x2D };

// Defaults of a fresh Slicex (neutral groups, one region per deck).
Model defaultModel();
Articulator defaultArticulator(int target);

// Binary state (FL plugin data). `riffOut[d]`: the embedded audio of deck d.
bool parseState(const uint8_t* p, size_t n, Model& out, std::array<std::vector<uint8_t>, 2>& riffOut);
std::vector<uint8_t> writeState(const Model& m, const std::array<std::vector<uint8_t>, 2>& riff);

// Text (Gridline state data after the first line).
std::string writeText(const Model& m);
bool readText(const std::string& text, Model& out);
// gl.slicer data helpers: the sample file (first line) and the model.
std::string sampleOf(const std::string& data);
bool modelOf(const std::string& data, Model& out);  // false: classic data (path only)
std::string makeData(const Model& m);

// Laws.
double masterGain(int32_t v);  // (11^(v/12800) − 1)/10
double regionGain(int32_t v);  // (3^(v/8000) − 1)/2
// FL's voice pan law (host ComputeLRVol, constant power): L/R gains at pan −1..1.
void panLaw(double pan, double vol, double& l, double& r);

// Prepared model (UI thread; ChannelAux::custom).
struct Aux final : AuxData {
  Model model;
  struct PArt {
    flart::Prepared env, lfo;
    std::array<flart::Prepared, 4> maps;
  };
  std::array<std::array<PArt, kTargets>, kGroups> groups;
  // key → regions of each deck (layers), built from the regions' keys.
  std::array<std::array<std::vector<int>, 128>, 2> keyMap;
};
std::shared_ptr<const Aux> prepare(const PluginState& s);

// The voice engine. Fixed voice pool, no allocation in render.
class Engine {
 public:
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx);
  void noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props);
  void noteOff(uint32_t id);
  void allOff();
  void kill();
  void render(float* L, float* R, int n);
  bool active() const;

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    double run(int c, double x) {
      const double y = b0 * x + z1[c];
      z1[c] = b1 * x - a1 * y + z2[c];
      z2[c] = b2 * x - a2 * y;
      return y;
    }
  };
  struct ArtRun {
    int group = -1;  // −1 = none
    flart::EnvState env, amp;
    uint32_t phase = 0;
    bool on() const { return group >= 0; }
  };
  struct Voice {
    bool on = false, held = false, released = false, master = true;
    uint32_t id = 0;
    int deck = 0, region = 0, cut = 0;
    double pos = 0, rate = 1, baseRate = 1, end = 0, start = 0, loopStart = -1;
    bool reverse = false;
    int64_t delay = 0;
    double layerGain = 1, regionGain = 1, velocity = 1, notePan = 0, fine = 0;
    double ampAmt = 1, filtAmt = 1, speedAmt = 0, startAmt = 0;
    double vx = 0, vy = 0;  // note mod X / Y
    std::array<ArtRun, kTargets> art;
    std::array<double, kTargets> init{};  // note-on values (velocity / random maps)
    // Gains ramped across a control block.
    double gl = 0, gr = 0, tl = 0, tr = 0, dl = 0, dr = 0;
    bool started = false;
    int stages = 0, ftype = 0;
    Biquad bq[3];
    int fade = -1;  // > 0: samples left of the release declick
    int blockPos = 0;
  };
  void control(Voice& v);
  void startVoice(int deck, int region, double layerGain, double pitch, float velocity, const NoteProps& props, uint32_t id, bool master);
  double articulator(Voice& v, int t, double dt);
  const PluginState* st_ = nullptr;
  const Aux* aux_ = nullptr;
  SamplePtr deck_[2];
  ProcessContext ctx_;
  int block_ = 48;
  const stretch::SincTable* sinc_ = &stretch::sincTable(stretch::Quality::Realtime);  // J-87: band-limited reads
  std::array<Voice, 32> voices_{};
  std::array<int, 128> cycle_{};
  uint64_t rng_ = 0x2545F4914F6CDD1Dull;
  double modX_ = 0.5, modY_ = 0.5;
  size_t next_ = 0;
};

}  // namespace gl::slicex
