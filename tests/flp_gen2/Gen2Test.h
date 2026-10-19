// Gridline — helpers for the second-wave instrument decoder tests: build a
// plugin's events from synthetic bytes and run its registered decoder.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/Flp.h"
#include "../Check.h"

namespace gen2test {

// Little-endian byte writer for synthetic plugin states.
struct Bytes {
  std::vector<uint8_t> b;
  Bytes& u8(uint8_t v) { b.push_back(v); return *this; }
  Bytes& u16(uint16_t v) { b.push_back(v & 0xFF), b.push_back(static_cast<uint8_t>(v >> 8)); return *this; }
  Bytes& u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Bytes& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
  Bytes& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u32(u);
  }
  Bytes& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    u32(static_cast<uint32_t>(u)), u32(static_cast<uint32_t>(u >> 32));
    return *this;
  }
  Bytes& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
  Bytes& str(const std::string& s, bool nul = true) {
    b.insert(b.end(), s.begin(), s.end());
    if (nul) b.push_back(0);
    return *this;
  }
  Bytes& utf16(const std::string& s, bool nul = true) {  // ASCII → UTF-16LE
    for (char c : s) b.push_back(static_cast<uint8_t>(c)), b.push_back(0);
    if (nul) b.push_back(0), b.push_back(0);
    return *this;
  }
  Bytes& raw(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
  Bytes& pad(size_t to) {
    if (b.size() < to) b.resize(to, 0);
    return *this;
  }
  size_t size() const { return b.size(); }
};

// One decode run: owns the bytes the events point into.
struct Run {
  gl::flp::File file;
  gl::Song song;
  gl::flp::Report report;
  gl::flp::Context ctx;
  std::vector<std::vector<uint8_t>> store;
  std::vector<gl::flp::Event> events;
  gl::flp::PluginMapping m;
  bool ok = false;
  Run() : ctx(file, song, report) {}
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;
  void add(uint8_t id, std::vector<uint8_t> payload) {
    store.push_back(std::move(payload));
    gl::flp::Event e;
    e.id = id;
    e.data = store.back().data();
    e.size = static_cast<uint32_t>(store.back().size());
    events.push_back(e);
  }
};

// Runs the decoder registered for `flName` on a state payload (event 213) and
// an optional wrapper payload (event 212). `run.ok` = decoder returned true.
inline void decode(Run& run, const std::string& flName, const std::vector<uint8_t>& state,
                   const std::vector<uint8_t>& wrapper = {}, const std::string& factoryData = "/FLDATA") {
  run.ctx.factoryData = factoryData;
  run.ctx.projectDir = "/proj";
  run.store.reserve(8);
  Bytes name;
  name.utf16(flName);
  run.add(201, name.b);
  if (!wrapper.empty()) run.add(212, wrapper);
  run.add(213, state);
  auto& d = gl::flp::decoders();
  auto it = d.find(flName);
  run.m = {};
  run.m.flName = flName;
  run.ok = it != d.end() && it->second(run.ctx, run.events, run.m) && !run.m.glType.empty();
  if (run.ok) gl::normalizePluginState(run.m.state);
}

inline bool registered(const std::string& flName) { return gl::flp::decoders().count(flName) != 0; }

inline bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

inline float param(const Run& r, int i) {
  return i >= 0 && static_cast<size_t>(i) < r.m.state.params.size() ? r.m.state.params[static_cast<size_t>(i)] : NAN;
}

}  // namespace gen2test
