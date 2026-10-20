// Gridline — FLP import: helpers shared by the mixer importer and the effect
// decoders (FlpMixer.cpp, FlpEffects.cpp, FlpFx*.cpp).
//
// FL value curves used here were established by observation of saved files
// and of the plugins' own behaviour (notes: analysis/demos/notes_gamma.md);
// the code is original.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Flp.h"

namespace gl::flp::fx {

// ---------------------------------------------------------------- curves

// FL mixer fader / send level, 0..16000 (12800 = 0 dB) → linear gain.
double flMixerGain(double v);
// Linear gain → Gridline fader value (taper (v / 0.8)^2, see dsp::faderGain).
double glFaderFromGain(double gain);
// FL insert-EQ / Parametric EQ 2 frequency word 0..65536 → Hz.
double flEqFreqHz(double v);
// FL insert-EQ bandwidth word 0..65536 → Gridline Q.
double flInsertEqQ(double v);

// ---------------------------------------------------------------- state

// Little-endian reader over a plugin state payload (event 213).
struct Reader {
  const uint8_t* p = nullptr;
  size_t n = 0, pos = 0;
  Reader() = default;
  explicit Reader(const Event* e) : p(e ? e->data : nullptr), n(e ? e->size : 0) {}
  Reader(const uint8_t* d, size_t size) : p(d), n(size) {}
  bool has(size_t k) const { return pos + k <= n; }
  size_t left() const { return pos < n ? n - pos : 0; }
  uint8_t u8() { return has(1) ? p[pos++] : (pos = n, uint8_t{0}); }
  uint16_t u16() {
    if (!has(2)) return pos = n, uint16_t{0};
    uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
    pos += 2;
    return v;
  }
  uint32_t u32() {
    if (!has(4)) return pos = n, 0u;
    uint32_t v = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
                 (static_cast<uint32_t>(p[pos + 2]) << 16) | (static_cast<uint32_t>(p[pos + 3]) << 24);
    pos += 4;
    return v;
  }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  float f32();
  double f64();
  void skip(size_t k) { pos = std::min(n, pos + k); }
};

// ---------------------------------------------------------------- table-driven decoders
//
// Most native FL effects save their knobs as a version word followed by one
// 32-bit integer per parameter, in parameter (tag) order. A Knob row maps one
// FL parameter to one Gridline parameter. Automation clips address FL
// parameters by tag with a normalized 0..1 value spanning [flMin, flMax].
struct Knob {
  int flTag = -1;            // FL parameter index
  double flMin = 0, flMax = 1;
  int glParam = -1;          // Gridline parameter index (-1: not mapped)
  std::function<double(double flValue)> conv;  // FL integer value → Gridline value (empty: identity)
};
using KnobTable = std::vector<Knob>;

// Sets m.param / m.value from a table (shared, so the functions stay valid
// after the mapping is copied).
void bindTable(PluginMapping& m, std::shared_ptr<const KnobTable> table);
// Applies FL values (indexed by tag) to m.state through the table.
void applyTable(PluginMapping& m, const KnobTable& table, const std::vector<double>& flByTag);
// Starts a mapping: m.glType = type, m.state = defaults of that type.
void begin(PluginMapping& m, const std::string& glType);
// Sets a Gridline parameter (ignored when out of range).
void set(PluginMapping& m, int glParam, double v);
// J-42: a Maximus program file → gl.enhance parameters (reference engine), see FlpFxDyn.cpp.
bool maximusProgramToEnhance(const std::string& path, std::vector<float>& params, bool* exact);

// Linear helpers for conv functions.
inline std::function<double(double)> scale(double k, double add = 0) {
  return [k, add](double v) { return v * k + add; };
}

// zlib stream (RFC 1950) → bytes; empty on malformed input or when larger than limit.
std::vector<uint8_t> inflateZlib(const uint8_t* p, size_t n, size_t limit = 8u << 20);

// ---------------------------------------------------------------- registries
// One file per effect family (FlpFx<Family>.cpp), called by FlpEffects.cpp.
void registerFxEq(std::map<std::string, PluginDecoder>& d);    // EQ, filters, stereo, utility, distortion
void registerFxTime(std::map<std::string, PluginDecoder>& d);  // reverb, delays, chorus / flanger family
void registerFxDyn(std::map<std::string, PluginDecoder>& d);   // compressors, limiters, maximizers, transients
void registerFxMisc(std::map<std::string, PluginDecoder>& d);  // envelopes, time effects, vocoder, analyzers, notes, panels
void registerFxK3(std::map<std::string, PluginDecoder>& d);    // κ3 stereo / creative effects (registered last: overrides)

}  // namespace gl::flp::fx
