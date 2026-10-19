// Gridline — FLP import: shared curves and helpers for the mixer importer and
// the effect decoders.
#include "FlpFx.h"
#include <algorithm>
#include <cstring>

namespace gl::flp::fx {

double flMixerGain(double v) {
  // Fader position x = v / 16000; 0.8 = 0 dB, 1.0 ≈ +5.6 dB, exponential taper.
  double x = std::clamp(v / 16000.0, 0.0, 1.0);
  const double b = 20.0;
  return (std::pow(b, x) - 1.0) / (std::pow(b, 0.8) - 1.0);
}

double glFaderFromGain(double gain) { return std::clamp(0.8 * std::sqrt(std::max(0.0, gain)), 0.0, 1.25); }

double flEqFreqHz(double v) {
  // FL's frequency table (mixer strip EQ, observed in the host): 10 Hz at 0,
  // 20 kHz at 65536, exponential: 10 + (e^(k*x) - 1) * 19990 / 150 with e^k = 151.
  double x = std::clamp(v / 65536.0, 0.0, 1.0);
  return 10.0 + (std::exp(x * std::log(151.0)) - 1.0) * 19990.0 / 150.0;
}

double flInsertEqQ(double v) {
  // Bandwidth word -> Q. FL's default (17500) maps to Gridline's default Q;
  // two decades around it (approximation: the demos only use the default).
  double x = std::clamp(v / 65536.0, 0.0, 1.0);
  return std::clamp(0.7 * std::pow(10.0, (x - 17500.0 / 65536.0) * 2.0), 0.1, 10.0);
}

float Reader::f32() {
  uint32_t u = u32();
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

double Reader::f64() {
  uint64_t lo = u32(), hi = u32();
  uint64_t u = lo | (hi << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

void begin(PluginMapping& m, const std::string& glType) {
  m.glType = glType;
  m.state = defaultPluginState(glType);
}

void set(PluginMapping& m, int glParam, double v) {
  if (glParam < 0) return;
  if (static_cast<size_t>(glParam) >= m.state.params.size()) m.state.params.resize(static_cast<size_t>(glParam) + 1, 0.0f);
  m.state.params[static_cast<size_t>(glParam)] = static_cast<float>(v);
}

void applyTable(PluginMapping& m, const KnobTable& table, const std::vector<double>& flByTag) {
  for (const Knob& k : table) {
    if (k.glParam < 0 || k.flTag < 0 || static_cast<size_t>(k.flTag) >= flByTag.size()) continue;
    double v = flByTag[static_cast<size_t>(k.flTag)];
    set(m, k.glParam, k.conv ? k.conv(v) : v);
  }
}

void bindTable(PluginMapping& m, std::shared_ptr<const KnobTable> table) {
  // Recorded values (pattern events) are the control's integer in [flMin, flMax].
  std::vector<RawRange> ranges;
  for (const Knob& k : *table)
    if (k.flTag >= 0) ranges.push_back({k.flTag, k.flMin, k.flMax});
  setRawRanges(m, std::move(ranges));
  m.param = [table](int flParam) {
    for (const Knob& k : *table)
      if (k.flTag == flParam) return k.glParam;
    return -1;
  };
  m.value = [table](int flParam, double flNorm) {
    for (const Knob& k : *table)
      if (k.flTag == flParam) {
        double v = k.flMin + std::clamp(flNorm, 0.0, 1.0) * (k.flMax - k.flMin);
        return k.conv ? k.conv(v) : v;
      }
    return 0.0;
  };
}

}  // namespace gl::flp::fx
