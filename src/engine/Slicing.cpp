#include "SliceProcess.h"
#include "SlicexEngine.h"
#include "Slicing.h"
#include "SamplePrecompute.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "Generators.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace gl {

std::vector<int64_t> computeSlices(const Sample& s, int mode, double sensitivity) {
  std::vector<int64_t> out{0};
  const int64_t n = static_cast<int64_t>(s.frames());
  if (n < 64) return out;
  if (mode >= 1) {
    int count = 4 << (mode - 1);
    for (int k = 1; k < count; ++k) out.push_back(n * k / count);
    return out;
  }
  // Energy per hop; an onset is a jump well above the recent average.
  const int hop = 256;
  std::vector<double> e;
  for (int64_t i = 0; i + hop <= n; i += hop) {
    double acc = 0;
    for (int64_t k = i; k < i + hop; ++k) {
      double v = 0.5 * (s.left[static_cast<size_t>(k)] + s.right[static_cast<size_t>(k)]);
      acc += v * v;
    }
    e.push_back(acc / hop);
  }
  double ratio = 1.6 + (1.0 - std::clamp(sensitivity, 0.0, 1.0)) * 8.0;
  int64_t minGap = static_cast<int64_t>(s.sampleRate * 0.06);
  for (size_t i = 4; i < e.size() && out.size() < 64; ++i) {
    double avg = 0;
    for (size_t k = i - 4; k < i; ++k) avg += e[k];
    avg /= 4;
    if (e[i] > 1e-5 && e[i] > avg * ratio && e[i] > e[i - 1]) {
      int64_t pos = static_cast<int64_t>(i - 1) * hop;
      if (pos - out.back() >= minGap) out.push_back(pos);
    }
  }
  return out;
}

namespace {
std::vector<std::string> splitLines(const std::string& data) {
  std::vector<std::string> lines;
  size_t st = 0;
  while (st <= data.size()) {
    size_t e = data.find('\n', st);
    lines.push_back(data.substr(st, e == std::string::npos ? std::string::npos : e - st));
    if (e == std::string::npos) break;
    st = e + 1;
  }
  return lines;
}
}  // namespace

namespace {
std::string join(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
  return out;
}
std::string layerKey(int pad, int layer) { return "L" + std::to_string(pad + 1) + "." + std::to_string(layer + 1); }
}  // namespace

std::string padDataGet(const std::string& data, const std::string& key) {
  auto lines = splitLines(data);
  const std::string k = key + " ";
  for (size_t i = 16; i < lines.size(); ++i)
    if (lines[i].compare(0, k.size(), k) == 0) return lines[i].substr(k.size());
  return std::string();
}

std::string padDataSet(const std::string& data, const std::string& key, const std::string& value) {
  auto lines = splitLines(data);
  if (lines.size() < 16) lines.resize(16);
  const std::string k = key + " ";
  bool found = false;
  for (size_t i = 16; i < lines.size();) {
    if (lines[i].compare(0, k.size(), k) == 0) {
      if (value.empty() || found) {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      lines[i] = k + value, found = true;
    }
    ++i;
  }
  if (!found && !value.empty()) lines.push_back(k + value);
  while (lines.size() > 16 && lines.back().empty()) lines.pop_back();
  return join(lines);
}

std::string padSamplePath(const std::string& data, int pad, int layer) {
  auto lines = splitLines(data);
  if (layer == 0 && pad < 16) return pad < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(pad)] : std::string();
  const std::string key = layerKey(pad, layer) + " ";
  for (size_t i = 16; i < lines.size(); ++i)
    if (lines[i].compare(0, key.size(), key) == 0) return lines[i].substr(key.size());
  return "none";
}

std::string setPadSamplePath(const std::string& data, int pad, int layer, const std::string& path) {
  if (layer == 0 && pad < 16) {
    auto lines = splitLines(data);
    if (lines.size() < 16) lines.resize(16);
    lines[static_cast<size_t>(pad)] = path;
    while (lines.size() > 16 && lines.back().empty()) lines.pop_back();
    return join(lines);
  }
  return padDataSet(data, layerKey(pad, layer), path.empty() ? "none" : path);
}

bool parsePadEnvelope(const std::string& text, PadEnvelope& e) {
  e = PadEnvelope{};
  int on = 0, used = 0;
  if (std::sscanf(text.c_str(), "%d %d %d %d%n", &on, &e.sustain, &e.loopStart, &e.loopEnd, &used) < 4) return false;
  e.on = on != 0;
  const char* p = text.c_str() + used;
  while (e.n < PadEnvelope::kMax) {
    float t = 0, v = 0, ten = 0;
    int k = 0;
    if (std::sscanf(p, " %f,%f,%f%n", &t, &v, &ten, &k) < 3 || k <= 0) break;
    e.t[e.n] = std::max(0.0f, t), e.v[e.n] = std::clamp(v, 0.0f, 1.0f), e.ten[e.n] = std::clamp(ten, -1.0f, 1.0f);
    ++e.n;
    p += k;
  }
  if (e.n > 0) e.t[0] = 0;
  if (e.sustain >= e.n) e.sustain = -1;
  if (e.loopStart < 0 || e.loopStart >= e.n || e.loopEnd >= e.n || e.loopEnd <= e.loopStart) e.loopStart = e.loopEnd = -1;
  return e.n > 0;
}

std::string formatPadEnvelope(const PadEnvelope& e) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%d %d %d %d", e.on ? 1 : 0, e.sustain, e.loopStart, e.loopEnd);
  std::string s = buf;
  for (int i = 0; i < e.n; ++i) {
    std::snprintf(buf, sizeof buf, " %.6g,%.6g,%.6g", static_cast<double>(e.t[i]), static_cast<double>(e.v[i]), static_cast<double>(e.ten[i]));
    s += buf;
  }
  return s;
}

double padEnvCurve(double tension, double x) {
  x = std::clamp(x, 0.0, 1.0);
  const double t = std::clamp(tension, -1.0, 1.0);
  return std::pow(x, std::pow(2.0, 4.0 * t));  // positive tension: slow start, fast finish
}

bool parsePadLayer(const std::string& text, PadLayerX& l) {
  l = PadLayerX{};
  int rev = 0;
  if (std::sscanf(text.c_str(), "%d %d %f %f %f %d", &l.lo, &l.hi, &l.level, &l.pan, &l.tune, &rev) < 5) return false;
  l.reverse = rev != 0;
  l.lo = std::clamp(l.lo, 0, 127), l.hi = std::clamp(l.hi, 0, 127);
  return true;
}

std::string formatPadLayer(const PadLayerX& l) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%d %d %.6g %.6g %.6g %d", l.lo, l.hi, static_cast<double>(l.level), static_cast<double>(l.pan),
                static_cast<double>(l.tune), l.reverse ? 1 : 0);
  return buf;
}

int padLayerCount(const std::string& data, int pad) {
  auto lines = splitLines(data);
  const std::string pre = "L" + std::to_string(pad + 1) + ".";
  int count = 1;
  for (size_t i = 16; i < lines.size(); ++i)
    if (lines[i].compare(0, pre.size(), pre) == 0) {
      int l = std::atoi(lines[i].c_str() + pre.size());
      size_t sp = lines[i].find(' ');
      std::string path = sp == std::string::npos ? std::string() : lines[i].substr(sp + 1);
      if (l >= 1 && path != "none" && !path.empty()) count = std::max(count, std::min(l, pp::kPadMaxLayers));
    }
  return count;
}

struct PadsAux final : AuxData {
  PadEnvelope vol[pp::kPadsAll], pan[pp::kPadsAll];
  PadLayerX extra[pp::kPadsAll * pp::kPadMaxLayers];
  int layers[pp::kPadsAll] = {};
};

const PadsAux* padsAuxOf(const ChannelAux& aux) { return dynamic_cast<const PadsAux*>(aux.custom.get()); }

PadsAuxView padsAuxView(const PadsAux* a) {
  PadsAuxView v;
  if (!a) return v;
  v.vol = a->vol, v.pan = a->pan, v.extra = a->extra, v.layers = a->layers;
  return v;
}

bool prepareBuiltinAux(const PluginState& st, ChannelAux& aux) {
  using namespace pp;
  if (st.type == "gl.pads") {
    aux.samples.assign(static_cast<size_t>(kPadSampleSlots), nullptr);
    auto pa = std::make_shared<PadsAux>();
    for (int k = 0; k < kPadsAll; ++k) {
      const int nl = std::max(kPadLayers, padLayerCount(st.data, k));
      pa->layers[k] = nl;
      for (int l = 0; l < nl; ++l) {
        std::string path = padSamplePath(st.data, k, l);
        if (l == 0 && k < kPads && path.empty() && static_cast<size_t>(k) < builtinKit().size()) path = "builtin:" + builtinKit()[static_cast<size_t>(k)];
        if (l >= kPadLayers) parsePadLayer(padDataGet(st.data, "S" + std::to_string(k + 1) + "." + std::to_string(l + 1)), pa->extra[k * kPadMaxLayers + l]);
        if (path.empty() || path == "none") continue;
        aux.samples[static_cast<size_t>(padLayerSampleX(k, l))] = sampleCache().get(path);
      }
      parsePadEnvelope(padDataGet(st.data, "E" + std::to_string(k + 1) + ".v"), pa->vol[k]);
      parsePadEnvelope(padDataGet(st.data, "E" + std::to_string(k + 1) + ".p"), pa->pan[k]);
    }
    aux.custom = pa;
    return true;
  }
  if (st.type == "gl.synth") {  // custom-wave oscillators: one cycle = the channel sample
    aux.samples.assign(1, st.data.empty() ? nullptr : sampleCache().get(st.data));
    return true;
  }
  if (st.type == "gl.slicer" && st.data.find('\n') != std::string::npos) {
    // J-41: Slicex region model ("<deck A file>\n<model>"): both decks' samples.
    auto sx = slicex::prepare(st);
    aux.samples.assign(2, nullptr);
    const std::string a = slicex::sampleOf(st.data);
    if (!a.empty()) aux.samples[0] = sampleCache().get(a);
    if (sx && !sx->model.decks[1].file.empty()) aux.samples[1] = sampleCache().get(sx->model.decks[1].file);
    aux.custom = sx;
    return true;
  }
  if (st.type == "gl.sampler" || st.type == "gl.slicer" || st.type == "gl.granular") {
    // Sampler: its precomputed effects (J-23) run here, once, on the UI thread.
    aux.samples.assign(1, st.type == "gl.sampler" ? precomputedSample(st.data, samplePrecomputeFx(st)) : sampleCache().get(st.data));
    if (st.type == "gl.slicer" && aux.samples[0] && st.params.size() > static_cast<size_t>(SlEngine) &&
        st.params[SlEngine] >= SlEngineProcessed - 0.5f) {
      // J-41: processed slices (offline, here on the UI thread); the voices
      // play the processed sample from the processed slice starts.
      aux.samples[0] = sliceproc::processedFor(st, aux.samples[0], &aux.slices);
      return true;
    }
    if (st.type == "gl.slicer" && aux.samples[0]) {
      int mode = st.params.size() > SlMode ? static_cast<int>(st.params[SlMode] + 0.5f) : 0;
      double sens = st.params.size() > SlSensitivity ? st.params[SlSensitivity] : 0.5;
      aux.slices = computeSlices(*aux.samples[0], mode, sens);
    }
    return true;
  }
  return false;
}

const std::vector<std::string>& builtinKit() {
  static const std::vector<std::string> kit = {"kick", "snare", "clap", "hat", "openhat", "tomlo", "tomhi", "rim"};
  return kit;
}

}  // namespace gl
