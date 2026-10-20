// Gridline — FLP import, second-wave instrument decoders: shared helpers.
//
// Each group of FL generators lives in its own file (FlpGen2<Group>.cpp) and
// registers its decoders from registerGeneratorDecoders2 (FlpGenerators2.cpp).
// The state layouts were worked out from FL Studio's supplied projects and
// presets (observation of stored values against the plugins' published
// controls); no Image-Line code is used.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../PluginInfo.h"
#include "Flp.h"

namespace gl::flp::gen2 {

// Bounds-checked little-endian view of a byte blob (reads past the end give 0).
struct Blob {
  const uint8_t* p = nullptr;
  size_t n = 0;
  Blob() = default;
  Blob(const uint8_t* data, size_t size) : p(data), n(size) {}
  size_t size() const { return n; }
  bool empty() const { return n == 0; }
  bool has(size_t at, size_t len) const { return at <= n && len <= n - at; }
  uint8_t u8(size_t at) const { return at < n ? p[at] : 0; }
  uint16_t u16(size_t at) const { return has(at, 2) ? static_cast<uint16_t>(p[at] | (p[at + 1] << 8)) : 0; }
  uint32_t u32(size_t at) const {
    return has(at, 4) ? static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) |
                            (static_cast<uint32_t>(p[at + 2]) << 16) | (static_cast<uint32_t>(p[at + 3]) << 24)
                      : 0;
  }
  int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }
  int16_t i16(size_t at) const { return static_cast<int16_t>(u16(at)); }
  float f32(size_t at) const {
    uint32_t u = u32(at);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double f64(size_t at) const {
    if (!has(at, 8)) return 0.0;
    uint64_t u = static_cast<uint64_t>(u32(at)) | (static_cast<uint64_t>(u32(at + 4)) << 32);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  }
  Blob sub(size_t at, size_t len) const {
    if (at > n) return {};
    if (len > n - at) len = n - at;
    return {p + at, len};
  }
  // NUL-terminated (or length-limited) Latin-1 string → UTF-8.
  std::string ascii(size_t at, size_t maxLen = SIZE_MAX) const;
  // UTF-16LE string of `chars` code units (stops at NUL) → UTF-8.
  std::string utf16(size_t at, size_t chars) const;
  // Finds a byte pattern at or after `from` (npos when absent).
  size_t find(const std::string& pattern, size_t from = 0) const;
};

// The plugin state (event 213) / wrapper (event 212) of a plugin's events.
Blob stateBlob(const std::vector<Event>& events);
Blob wrapperBlob(const std::vector<Event>& events);

// Starts a mapping: defaults of `glType` (params sized to the descriptors).
void begin(PluginMapping& m, const char* glType);
// Sets a parameter (ignored when out of range; clamped by normalizePluginState).
void set(PluginMapping& m, int glParam, double value);

// FL's common 0..65536 knob integer → 0..1.
inline double knob(int32_t v) { return v <= 0 ? 0.0 : (v >= 65536 ? 1.0 : v / 65536.0); }

// Automation links: FL parameter index → Gridline parameter, with an optional
// conversion of the FL normalized value (default: the Gridline descriptor's
// fromNormalized, i.e. the same 0..1 position).
struct Link {
  int fl = 0;
  int gl = 0;
  std::function<double(double flNorm)> conv = {};
};
// `ranges`: each linked control's stored integer range (controls.md Min..Max),
// for recorded pattern values (PluginMapping::rawNorm).
void linkParams(PluginMapping& m, std::vector<Link> links, std::vector<RawRange> ranges = {});

// J-51: the Drum kit pad parameters (md::DpLevel, …) one DrumSynth Live
// patch (its 38 stored values) decodes to, in the decoder's order. Shared
// with the FLP encoder's joint search (FlpEncGenB.cpp): one law for both.
struct DslPadValues {
  int n = 0;
  int param[20];
  double value[20];
};
DslPadValues drumSynthLivePad(const int32_t* patch);

// Groups (one file each).
void registerSlicers(std::map<std::string, PluginDecoder>& d);      // FlpGen2Slicers.cpp
void registerPads(std::map<std::string, PluginDecoder>& d);         // FlpGen2Pads.cpp
void registerSynthsA(std::map<std::string, PluginDecoder>& d);      // FlpGen2SynthsA.cpp
void registerSynthsB(std::map<std::string, PluginDecoder>& d);      // FlpGen2SynthsB.cpp
void registerControllers(std::map<std::string, PluginDecoder>& d);  // FlpGen2Controllers.cpp

// Adds `dec` under `name` unless the first wave already registered it.
inline void add(std::map<std::string, PluginDecoder>& d, const std::string& name, PluginDecoder dec) {
  d.emplace(name, std::move(dec));
}

}  // namespace gl::flp::gen2
