// Gridline — FLP import: shared helpers for the generator (instrument) decoders.
//
//   FlpGenerators.cpp   registry + sampler / layer channel pass-through
//   FlpGenCommon.cpp    byte reader, channel envelope / filter records, parameter tables
//   FlpGenOsc.cpp       3x Osc → Trio Synth (exact)
//   FlpGenSimple.cpp    FL Keys, MIDI Out, Envelope Controller, Control Surface,
//                       BooBass, Fruit Kick, BassDrum, DrumSynth, Drumpad
//   FlpGenSynths.cpp    Harmless, GMS, Sytrus, Harmor (approximations)
//   FlpGenSytrus.cpp    Sytrus → Matrix FM (full decode)
//   FlpGenSamplers.cpp  DirectWave → Multisampler, FLEX → closest instrument
//
// All layouts were worked out from observation of saved states (documented in
// format notes); no Image-Line code is used.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Flp.h"

namespace gl::flp::gen {

// Bounds-checked little-endian reader over a plugin state.
struct Bytes {
  const uint8_t* p = nullptr;
  size_t n = 0;
  Bytes() = default;
  Bytes(const uint8_t* d, size_t size) : p(d), n(size) {}
  explicit Bytes(const Event& e) : p(e.data), n(e.size) {}
  bool has(size_t at, size_t len) const { return at <= n && len <= n - at; }
  uint8_t u8(size_t at) const { return at < n ? p[at] : 0; }
  uint16_t u16(size_t at) const { return has(at, 2) ? static_cast<uint16_t>(p[at] | (p[at + 1] << 8)) : 0; }
  uint32_t u32(size_t at) const;
  int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }
  uint64_t u64(size_t at) const { return has(at, 8) ? (static_cast<uint64_t>(u32(at)) | (static_cast<uint64_t>(u32(at + 4)) << 32)) : 0; }
  float f32(size_t at) const;
  double f64(size_t at) const;
  Bytes sub(size_t at, size_t len) const { return has(at, len) ? Bytes(p + at, len) : Bytes(); }
  // Length-prefixed (1 byte) ANSI string at `at`; `next` receives the offset after it.
  std::string pstr(size_t at, size_t* next = nullptr) const;
  // UTF-16LE text of `chars` code units at `at`.
  std::string utf16(size_t at, size_t chars) const;
  // Index of the first occurrence of `pat` at or after `from` (npos when absent).
  size_t find(const std::string& pat, size_t from = 0) const;
};

// The state payload (event 213) of a plugin's event list (empty when absent).
Bytes stateOf(const std::vector<Event>& events);

// Decompresses a zlib stream (RFC 1950 header + RFC 1951 deflate data) into
// `out`, stopping at `maxOut` bytes. false on malformed input.
bool inflateZlib(Bytes in, std::vector<uint8_t>& out, size_t maxOut = 64u << 20);

// One FL channel envelope / LFO record (event 218, 17 dwords). Targets in file
// order: 0 panning, 1 volume, 2 mod X (cutoff), 3 mod Y (resonance), 4 pitch.
struct ChanEnv {
  bool enabled = false;
  int predelay = 100, attack = 20000, hold = 20000, decay = 30000, sustain = 50, release = 20000;  // raw (100..65536, sustain 0..128)
  int amount = 0;  // -128..128
  int lfoPredelay = 100, lfoAttack = 20000, lfoAmount = 0, lfoSpeed = 32950, lfoShape = 0;
  int attackTension = 0, decayTension = 0, releaseTension = 0;
  bool lfoOn() const { return lfoAmount != 0; }
};
enum EnvTarget { EnvPan, EnvVol, EnvModX, EnvModY, EnvPitch, EnvTargets };
// Channel filter / levels record (event 219): cutoff (mod X) and resonance
// (mod Y) 0..256, filter type (Fast LP, LP, BP, HP, BS, LPx2, SVF LP, SVF LPx2).
struct ChanFilter {
  int cutoff = 256, reso = 0, type = 0;
  bool open() const { return cutoff >= 256 && reso <= 0; }
};
struct ChanSettings {
  ChanEnv env[EnvTargets];
  ChanFilter filter;
  bool haveEnv = false, haveFilter = false;
};
ChanEnv parseEnv(Bytes b);
// Reads 218 / 219 events from a channel's events; fills from `fallback` (a copy
// embedded in some plugin states: 5 × 68 bytes + 24-byte levels) when absent.
ChanSettings channelSettings(const std::vector<Event>& events, Bytes fallbackEnv = {}, Bytes fallbackLevels = {});

// The channel's instrument envelopes / LFOs / filter (events 218 / 219, else
// the copy some plugin states embed) as PluginState::ins text (J-33,
// engine/ChannelIns.h). Returns "" when nothing is in use. `exact` turns false
// for settings the runtime does not reproduce (panning envelope, global LFOs).
std::string channelInsText(const std::vector<Event>& events, Bytes fallbackEnv, Bytes fallbackLevels, bool* exact = nullptr);

// Approximate seconds of an FL envelope time value (100..65536).
double envSeconds(int v);
// Sustain 0..128 → 0..1.
inline double envSustain(int v) { return v <= 0 ? 0.0 : (v >= 128 ? 1.0 : v / 128.0); }
// LFO speed value (200..65536) → Hz (approximate).
double lfoHz(int v);
// Channel filter cutoff (0..256) → Hz (approximate).
double cutoffHz(int v);

// Parameter writes on a Gridline state (index checked, value clamped by normalizePluginState later).
void setp(PluginState& s, int idx, double v);
// Base state of a Gridline type with every parameter at its default.
PluginState base(const std::string& type);

// FL parameter (automation / remote control index) → Gridline parameter and
// value conversion, shared by the mapping's `param` / `value` callbacks.
struct ParamTable {
  struct Row {
    int gl = -1;
    double lo = 0, hi = 1;  // FL normalized 0..1 → lo..hi (linear) …
    bool expo = false;      // … or exponential between lo and hi (both > 0)
    std::function<double(double)> fn;  // custom conversion (overrides lo/hi)
  };
  std::map<int, Row> rows;
  void lin(int fl, int gl, double lo, double hi) { rows[fl] = Row{gl, lo, hi, false, {}}; }
  void exp(int fl, int gl, double lo, double hi) { rows[fl] = Row{gl, lo, hi, true, {}}; }
  void fn(int fl, int gl, std::function<double(double)> f) { rows[fl] = Row{gl, 0, 1, false, std::move(f)}; }
  // The control's stored integer range (controls.md Min..Max) of FL parameter
  // `fl`: recorded pattern values → FL normalized (PluginMapping::rawNorm).
  std::vector<RawRange> raw;
  void range(int fl, double lo, double hi) { raw.push_back({fl, lo, hi}); }
  // Installs param / value / rawNorm callbacks on a mapping (the table is shared).
  void install(PluginMapping& m) const;
};

// Report helper: one line per approximated instrument.
void note(Context& c, const std::string& flName, const std::string& what);

// Subtractive-additive synth → Additive (full state, FlpGenHarmless.cpp).
bool decodeHarmlessFull(Context& c, const std::vector<Event>& events, PluginMapping& m);
// Two-part additive / resynthesis synth → Resynth (full state, FlpGenHarmor.cpp).
bool decodeHarmorFull(Context& c, const std::vector<Event>& events, PluginMapping& m);
// Groove-machine synth → Groove Synth (full state, FlpGenGms.cpp; also .gmsynth payloads).
bool decodeGmsFull(Context& c, const std::vector<Event>& events, PluginMapping& m);
// Sytrus → Matrix FM (full state walk, FlpGenSytrus.cpp); false when the state does not walk.
bool decodeSytrusMatrix(Context& c, const std::vector<Event>& events, PluginMapping& m);

// Registration of each decoder family.
void registerOscDecoders(std::map<std::string, PluginDecoder>& d);
void registerSimpleDecoders(std::map<std::string, PluginDecoder>& d);
void registerSynthDecoders(std::map<std::string, PluginDecoder>& d);
void registerSamplerDecoders(std::map<std::string, PluginDecoder>& d);

}  // namespace gl::flp::gen
