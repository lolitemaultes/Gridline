// Gridline tests: a Patch Player with a SoundFont made in the test, rendered block by block.
#pragma once
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "core/PluginInfo.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/samplers/patchplayer.h"
#include "sf2_bank.h"

namespace pptest {

inline std::string tmpPath(const std::string& name) {
  const char* t = std::getenv("TMPDIR");
  return std::string(t ? t : "/tmp") + "/" + name;
}

// Writes the bank to a file of its own and returns the path.
inline std::string writeBank(const sf2t::Bank& b, const std::string& name) {
  const std::string p = tmpPath(name);
  sf2t::writeFile(p, b.build());
  return p;
}

struct Rig {
  gl::PluginState st;
  gl::ChannelAux aux;
  std::unique_ptr<gl::Generator> gen;
  gl::ProcessContext ctx;
  explicit Rig(const std::string& path, int bank = 0, int program = 0) {
    st = gl::defaultPluginState(gl::packs::samplers::kPatchPlayerType);
    gl::packs::samplers::setPatch(st, path, bank, program, "");
    st.params[gl::packs::samplers::PpVolume] = 0;  // 0 dB: levels read straight off the output
    prepare();
    gen = gl::packs::createGenerator(gl::packs::samplers::kPatchPlayerType);
    ctx.sampleRate = 44100;
    ctx.tempo = 120;
    gen->midi(0xB0, 7, 127, 0);  // full channel volume (the default 100 is a little lower)
  }
  void prepare() {
    aux = gl::ChannelAux{};
    gl::packs::prepareAux(st, aux);
  }
  void set(int k, float v) { st.params[static_cast<size_t>(k)] = v; }
  void run(std::vector<float>& L, std::vector<float>& R, size_t n, int block = 256, std::vector<float>* rev = nullptr,
           std::vector<float>* cho = nullptr) {
    const size_t base = L.size();
    L.resize(base + n, 0.f);
    R.resize(base + n, 0.f);
    for (size_t p = 0; p < n; p += static_cast<size_t>(block)) {
      const int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - p));
      gen->process(st, aux, ctx, L.data() + base + p, R.data() + base + p, len);
      const float *al = nullptr, *ar = nullptr;
      if (rev && gen->auxOutput(0, al, ar)) rev->insert(rev->end(), al, al + len);
      if (cho && gen->auxOutput(1, al, ar)) cho->insert(cho->end(), al, al + len);
    }
  }
};

inline float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
inline float mean(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to && i < v.size(); ++i) s += v[i];
  return static_cast<float>(s / static_cast<double>(std::max<size_t>(1, to - from)));
}
inline float rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to && i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>(std::max<size_t>(1, to - from))));
}
// Frequency from upward zero crossings in [from, to).
inline double freqAt(const std::vector<float>& v, size_t from, size_t to, double sr = 44100) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < to && i < v.size(); ++i)
    if (v[i - 1] < 0 && v[i] >= 0) {
      const double x = static_cast<double>(i - 1) + v[i - 1] / (v[i - 1] - v[i]);
      if (first < 0) first = x;
      else ++n;
      last = x;
    }
  return n > 0 ? sr * n / (last - first) : 0;
}
inline bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

}  // namespace pptest
