// Gridline — FLP export tests: helpers shared by the plugin encoder tests.
// Corpus checks read FL Studio's shipped projects / presets when FL is
// installed (skipped otherwise); synthetic checks need no FL content.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpCoverage.h"
#include "core/flp/FlpEncode.h"

namespace enctest {

using gl::flp::Bytes;
namespace enc = gl::flp::enc;

inline const enc::EncodeContext& ctx() {
  static enc::EncodeContext c = [] {
    enc::EncodeContext x;
    x.factoryData = gl::flp::defaultFactoryData();
    return x;
  }();
  return c;
}

// FL's shipped states of one plugin (projects, presets, plugin database),
// at most `max`. Empty when FL Studio is not installed.
inline std::vector<Bytes> corpusStates(const std::string& flName, size_t max = 400) {
  static std::map<std::string, std::vector<Bytes>> all = [] {
    std::map<std::string, std::vector<Bytes>> m;
    const std::string root = gl::flp::defaultFactoryData();
    if (root.empty()) return m;
    gl::flp::CoverageFiles cf = gl::flp::coverageFiles(root);
    std::vector<std::string> files;
    for (auto* v : {&cf.projects, &cf.presets, &cf.database}) files.insert(files.end(), v->begin(), v->end());
    for (const auto& f : files) {
      gl::flp::File file;
      std::string err;
      if (!gl::flp::readFile(f, file, &err)) continue;
      gl::flp::Sections sec = gl::flp::split(file);
      auto take = [&](const std::vector<gl::flp::Event>& evs) {
        const gl::flp::Event* n = gl::flp::find(evs, 201);
        const gl::flp::Event* s = gl::flp::find(evs, 213);
        if (!n || !s) return;
        auto& v = m[n->text()];
        if (v.size() < 2000) v.emplace_back(s->data, s->data + s->size);
      };
      for (const auto& [k, evs] : sec.channels) take(evs);
      for (const auto& ins : sec.inserts)
        for (const auto& sl : ins.slots) take(sl.events);
      if (sec.channels.empty() && sec.inserts.empty()) take(file.events);
    }
    return m;
  }();
  auto it = all.find(flName);
  if (it == all.end()) return {};
  std::vector<Bytes> v = it->second;
  if (v.size() > max) v.resize(max);
  return v;
}

struct Stats {
  int states = 0, decoded = 0, nativeExact = 0, patchExact = 0;
};

// Every shipped state of `flName`: decoded, encoded natively (from the
// encoder's defaults, forced to this FL plugin) and decoded again.
inline Stats corpusRoundTrip(const std::string& flName, size_t max = 400) {
  Stats st;
  for (const Bytes& b : corpusStates(flName, max)) {
    ++st.states;
    gl::flp::PluginMapping m;
    if (!enc::decodeState(flName, b, m, ctx())) continue;
    ++st.decoded;
    const enc::Encoder* e = enc::findEncoder(flName, m.state.type);
    if (!e) continue;
    Bytes t = e->defaults ? e->defaults() : Bytes{};
    enc::Result r = enc::encode(m.state, flName, t.empty() ? nullptr : &t, ctx());
    if (r.quality == enc::Result::Exact || r.quality == enc::Result::Verbatim) ++st.nativeExact;
    enc::Result p = enc::encode(m.state, flName, &b, ctx());
    if (p.quality == enc::Result::Verbatim && p.state == b) ++st.patchExact;
  }
  return st;
}

// A Gridline state encoded as `flName` (from its defaults) decodes back exactly.
inline bool nativeRoundTrip(const std::string& flName, const gl::PluginState& g) {
  const enc::Encoder* e = enc::findEncoder(flName, g.type);
  if (!e || !e->defaults) return false;
  Bytes t = e->defaults();
  enc::Result r = enc::encode(g, flName, &t, ctx());
  return r.quality == enc::Result::Exact || r.quality == enc::Result::Verbatim;
}

// The encoder's defaults decode to a valid state of its Gridline type.
inline bool defaultsDecode(const std::string& flName, const std::string& glType) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  gl::flp::PluginMapping m;
  return enc::decodeState(flName, e->defaults(), m, ctx()) && m.state.type == glType;
}

inline bool flInstalled() { return !gl::flp::defaultFactoryData().empty(); }

}  // namespace enctest
