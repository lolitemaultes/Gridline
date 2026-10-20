// Gridline — "synths" pack: Matrix FM reference model (gl.matrixfm, MModel = 1).
//
// J-50. The reference model reproduces the imported FM-matrix instrument's
// own engine sample for sample (checked against the instrument's output):
// operators read harmonic / shaped wave tables (16384 points, optional
// pre-filter, band-limited 4096-point versions per note) with
// extended-precision phase and ring modulation, a per-sample loop for
// operators that modulate or are modulated and four-lane block rendering for
// the others; the articulators run a point-envelope / LFO / mapping engine at
// a fixed control rate; thirteen filter types (state-variable, two-pole,
// biquads, ladder, allpass chain, 2x oversampling) with an optional table
// shaper; unison (independent or shared-filter voices), string-loop
// operators, noise, declick / soften, centring filters, portamento through
// the pitch envelope; effects chorus → three delays → plate reverb, a
// three-band EQ, oversampling by 2 / 4 / 8 / 16 with half-band decimation.
//
// Everything the engine needs is the Patch below. Imported states keep it
// exactly in PluginState::data ("x" lines, see mfmref_patch_dsp.cpp) together
// with the parameter values the importer wrote; a parameter that no longer
// matches is applied over the exact data. States without the section (new
// instances, edited curves) are converted from the parameters and the drawn
// data (Gridline's own curve shapes, so approximate).
//
// Real-time rules: makeEngine (UI thread) allocates everything; note and
// render calls never allocate.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/FlArticulator.h"

namespace gl {
struct PluginState;
}

namespace gl::packs::synths::mfmref {

inline constexpr int kOps = 6, kFilters = 3, kUnits = 10, kMain = 9;
inline constexpr int kTableLen = 16384;  // base wave table points
inline constexpr int kBandLen = 4096;    // band-limited table points
inline constexpr int kMaxVoices = 64;    // voice pool (unison voices included)
inline constexpr int kBlock = 256;       // host frames per internal block

// ------------------------------------------------------------------ patch
using Record = gl::flart::Record;

struct Art {
  int32_t version = 2;
  float base = 0, envAmt = 1, lfoAmt = 1;
  Record env, lfo;
  std::array<Record, 6> maps;  // key, velocity, mod X, mod Y, random, unison
};
struct Unit {
  int32_t u[10] = {};
  int32_t ratio = 20000, freqOfs = 0, noise = 0;  // operators
  std::array<float, 256> lv{}, ph{};
  int32_t tail[5] = {0, 0, 0, 0, 128};  // shape, tension, skew, sine, pre-filter
  uint8_t flags[8] = {};                // half, even, abs, free phase, centre, declick, string loop, band limit
  int32_t fb[12] = {};                  // filters: quality / normalise bytes, …, shaper switch, type
  Record ws;                            // shaper transfer curve
  uint8_t wsBip = 0;
  std::vector<Art> arts;  // 6 (operators), 8 (filters), 2 (main)
};
struct Patch {
  int ver = 25;
  int32_t main[240] = {};
  uint8_t mutes[162] = {};  // matrix cell mutes (frequency layer, then ring layer)
  int32_t soften = 0;
  uint8_t opt[16] = {};  // option bytes; [5], [8], [10] default to 1 in older states
  uint8_t delays[3][23] = {};
  uint8_t reverb[40] = {};
  int32_t eqTarget = 0;
  int32_t eq[3][4] = {};
  Unit units[kUnits];
  uint8_t b510 = 0, b5f28 = 0;
};

// The instrument's own state layout (decompressed), `ver` from its header.
bool parseNative(const uint8_t* p, size_t n, int ver, Patch& out);
// The instrument's own layout for a patch (no info text): parseNative reads it
// back to the same patch.
std::vector<uint8_t> writeNative(const Patch& p);
// Exact text form: "x" lines appended to Matrix FM data, with the parameter
// values written next to it (`params`, empty = none).
std::string serialize(const Patch& p, const std::vector<float>& params);
// The data without the exact section (what the drawn-data editor and the
// FL writer compare; "mfm 1" alone counts as no data).
std::string withoutExact(const std::string& data);
// Reads the section; false when the data has none.
bool parse(const std::string& data, Patch& out, std::vector<float>& params);
// The patch the reference model plays for a Matrix FM state.
Patch patchFromState(const PluginState& s);
// False (with the reason) for patches the model cannot reproduce exactly:
// random mapping curves (the instrument randomises them per session).
bool exactPatch(const Patch& p, std::string* why);

// ------------------------------------------------------------------ engine
struct Engine;
struct EngineDeleter {
  void operator()(Engine* e) const;
};
using EnginePtr = std::unique_ptr<Engine, EngineDeleter>;

// Builds an engine (UI thread: allocates). tempo in BPM.
EnginePtr makeEngine(const Patch& p, int sampleRate, double tempo);
// Values of a patch that apply to a running engine without a rebuild (main
// volume, matrix, operator volumes, filter controls, mod X / Y).
struct Live {
  int32_t mainVol = 128, modX = 256, modY = 256;
  int32_t matrix[162] = {};
  uint8_t mutes[162] = {};
  int32_t opVol[kOps] = {};
  int32_t flt[kFilters][10] = {};
};
Live liveOf(const Patch& p);
// True when two patches differ only in live values.
bool sameStructure(const Patch& a, const Patch& b);
// How the live values follow the parameters of one state (built on the UI
// thread; liveFromParams is allocation-free for the audio thread).
struct LiveSpec {
  struct Field {
    int param = -1;
    float scale = 1;
    bool hasSnap = false;
    uint32_t snap = 0;  // the importer's parameter value (bits)
    int32_t exact = 0;  // the exact value while the parameter keeps it
    uint8_t mute = 0;
  };
  static constexpr int kFields = 3 + 162 + kOps + kFilters * 8;
  Field f[kFields];
};
LiveSpec liveSpec(const PluginState& s);
void liveFromParams(const LiveSpec& spec, const float* params, size_t n, Live& out);
void applyLive(Engine& e, const Live& l);  // audio thread
void setTempo(Engine& e, double bpm);      // audio thread (envelope / LFO speeds)

// Note-on: pitch in cents (0 = C5), host voice volume (1 = velocity 100) and
// pan (-1..1), note mod X / Y (-1..1), velocity (0..1), colour group 0..15.
void noteOn(Engine& e, uint32_t id, double pitchCents, double vol, double pan, double modX, double modY, double velocity, int color);
void noteOff(Engine& e, uint32_t id);
void allOff(Engine& e);
void kill(Engine& e);
// Renders n frames (any n) adding into L / R. The engine works in whole
// 256-frame blocks on its own clock (as the instrument does), so the output
// does not depend on the host's block size; notes start at the next block.
void render(Engine& e, float* L, float* R, int n);
bool active(const Engine& e);

}  // namespace gl::packs::synths::mfmref
