// Gridline — "modulation" pack: control voltage maths, calibration fits and the per-computer
// calibration store (K-170). See cv.h.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include "../../core/Json.h"
#include "../../core/Project.h"
#include "cv.h"

namespace gl::packs::modulation {

double quantisePitch(double pitch, int mode, unsigned mask, int root) {
  if (!std::isfinite(pitch)) return 0.0;
  if (mode == CqOff) return pitch;
  const double near = std::round(pitch);
  if (mode == CqSemitone || (mask & 0xFFFu) == 0 || (mask & 0xFFFu) == 0xFFFu) return near;
  root = ((root % 12) + 12) % 12;
  auto inScale = [&](int key) { return ((mask >> static_cast<unsigned>((((key - root) % 12) + 12) % 12)) & 1u) != 0; };
  const int base = static_cast<int>(near);
  // The nearest key of the scale to the unrounded pitch (ties go up).
  int best = base;
  double bestD = 1e9;
  for (int d = -6; d <= 6; ++d) {
    const int k = base + d;
    if (!inScale(k)) continue;
    const double dist = std::fabs(static_cast<double>(k) - pitch);
    if (dist < bestD - 1e-9 || (std::fabs(dist - bestD) <= 1e-9 && k > best)) {
      bestD = dist;
      best = k;
    }
  }
  return static_cast<double>(best);
}

void ClockTracker::edge(int64_t at) {
  if (last_ >= 0 && at > last_) {
    const double iv = static_cast<double>(at - last_);
    if (count_ > 0) {
      double mean = 0;
      const int n = std::min(count_, kMax);
      for (int i = 0; i < n; ++i) mean += iv_[i];
      mean /= n;
      if (iv > mean * 1.34 || iv < mean / 1.34) count_ = 0, head_ = 0;  // tempo jump: start again
    }
    iv_[head_] = iv;
    head_ = (head_ + 1) % kMax;
    if (count_ < kMax) ++count_;
  }
  last_ = at;
}

double ClockTracker::bpm(double sampleRate, int ppq) const {
  if (count_ < 2 || ppq <= 0) return 0.0;
  const int n = std::min(count_, std::min(ppq, kMax));  // the last beat's intervals
  double sum = 0;
  for (int i = 0; i < n; ++i) sum += iv_[((head_ - 1 - i) % kMax + kMax) % kMax];
  const double mean = sum / n;
  return mean > 0 ? 60.0 * sampleRate / (mean * ppq) : 0.0;
}

LineFit fitLine(const std::vector<double>& x, const std::vector<double>& y) {
  LineFit f;
  const size_t n = std::min(x.size(), y.size());
  if (n < 2) return f;
  double mx = 0, my = 0;
  for (size_t i = 0; i < n; ++i) mx += x[i], my += y[i];
  mx /= static_cast<double>(n), my /= static_cast<double>(n);
  double sxx = 0, sxy = 0;
  for (size_t i = 0; i < n; ++i) sxx += (x[i] - mx) * (x[i] - mx), sxy += (x[i] - mx) * (y[i] - my);
  if (sxx < 1e-12) return f;
  f.gain = sxy / sxx;
  f.offset = my - f.gain * mx;
  double e = 0;
  for (size_t i = 0; i < n; ++i) {
    const double r = y[i] - (f.gain * x[i] + f.offset);
    e += r * r;
  }
  f.rmsError = std::sqrt(e / static_cast<double>(n));
  f.ok = std::isfinite(f.gain) && std::isfinite(f.offset);
  return f;
}

CouplingResult analyseCoupling(const std::vector<double>& t, const std::vector<double>& v) {
  CouplingResult r;
  const size_t n = std::min(t.size(), v.size());
  if (n < 8) return r;
  const double end = t[n - 1];
  if (end < 0.5) return r;
  double s = 0, e = 0, e2 = 0;
  int ns = 0, ne = 0;
  for (size_t i = 0; i < n; ++i) {
    if (t[i] >= 0.03 && t[i] <= 0.13) s += v[i], ++ns;
    if (t[i] >= end * 0.75) e += v[i], e2 += v[i] * v[i], ++ne;
  }
  if (ns == 0 || ne == 0) return r;
  r.startVolts = s / ns;
  r.endVolts = e / ne;
  r.noise = std::sqrt(std::max(0.0, e2 / ne - r.endVolts * r.endVolts));
  if (std::fabs(r.startVolts) < 0.05) return r;  // nothing arrived: not patched, or the input is off
  r.ok = true;
  r.held = r.endVolts / r.startVolts;
  r.dcCoupled = r.held >= 0.8;
  return r;
}

TunerFit fitTuner(const std::vector<double>& volts, const std::vector<double>& hz, double voltsPerOct) {
  TunerFit f;
  std::vector<double> x, y;
  for (size_t i = 0; i < volts.size() && i < hz.size(); ++i)
    if (hz[i] > 0 && std::isfinite(hz[i])) x.push_back(volts[i]), y.push_back(std::log2(hz[i]));
  const LineFit l = fitLine(x, y);
  if (!l.ok || l.gain <= 1e-6 || voltsPerOct <= 0) return f;
  f.ok = true;
  f.octavesPerVolt = l.gain;
  f.gain = l.gain * voltsPerOct;  // > 1: the output (or the oscillator) moves too far per volt
  // After calibration the output sends v / gain: each step then moves (v - v0) / voltsPerOct octaves
  // plus what the line does not explain (the oscillator's own non-linearity).
  auto resid = [&](size_t i) { return y[i] - (l.offset + l.gain * x[i]); };
  for (size_t i = 0; i < x.size(); ++i) {
    const double want = (x[i] - x[0]) / voltsPerOct;  // octaves above the first step
    f.centsBefore.push_back(1200.0 * ((y[i] - y[0]) - want));
    f.centsAfter.push_back(1200.0 * (resid(i) - resid(0)));
  }
  return f;
}

std::string calKey(bool input, const std::string& device, int channel) {
  return std::string(input ? "in|" : "out|") + (device.empty() ? "default" : device) + "|" + std::to_string(channel);
}

CvCal CalStore::get(const std::string& key) const {
  auto it = map_.find(key);
  return it == map_.end() ? CvCal{} : it->second;
}

void CalStore::set(const std::string& key, const CvCal& c) {
  CvCal v = c;
  v.valid = std::isfinite(c.gain) && std::isfinite(c.offset) && std::fabs(c.gain) > 1e-3;
  if (v.valid) map_[key] = v;
}

void CalStore::erase(const std::string& key) { map_.erase(key); }

std::string CalStore::toText() const {
  Json root = Json::makeObject();
  root.object["version"] = Json::makeNumber(1);
  Json list = Json::makeObject();
  for (const auto& [k, c] : map_) {
    Json e = Json::makeObject();
    e.object["gain"] = Json::makeNumber(c.gain);
    e.object["offset"] = Json::makeNumber(c.offset);
    list.object[k] = e;
  }
  root.object["channels"] = list;
  return jsonStringify(root);
}

bool CalStore::fromText(const std::string& text) {
  Json root;
  if (!jsonParse(text, root) || !root.is(Json::Type::Object)) return false;
  map_.clear();
  if (const Json* list = root.find("channels"); list && list->is(Json::Type::Object))
    for (const auto& [k, e] : list->object) {
      if (!e.is(Json::Type::Object)) continue;
      CvCal c;
      c.gain = e.num("gain", 1.0);
      c.offset = e.num("offset", 0.0);
      set(k, c);
    }
  return true;
}

bool CalStore::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    map_.clear();
    return true;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return fromText(ss.str());
}

bool CalStore::save(const std::string& path) const { return writeFileAtomic(path, toText()); }

namespace {
struct Global {
  std::mutex m;
  CalStore store;
  std::string inDev, outDev;
};
Global& global() {
  static Global g;
  return g;
}
}  // namespace

CalStore calStoreCopy() {
  std::lock_guard<std::mutex> lk(global().m);
  return global().store;
}
void setCalStore(const CalStore& s) {
  std::lock_guard<std::mutex> lk(global().m);
  global().store = s;
}
void setCalDevices(const std::string& inputDevice, const std::string& outputDevice) {
  std::lock_guard<std::mutex> lk(global().m);
  global().inDev = inputDevice;
  global().outDev = outputDevice;
}
std::string calInputDevice() {
  std::lock_guard<std::mutex> lk(global().m);
  return global().inDev;
}
std::string calOutputDevice() {
  std::lock_guard<std::mutex> lk(global().m);
  return global().outDev;
}
CvCal inputCal(int ch) {
  std::lock_guard<std::mutex> lk(global().m);
  return global().store.get(calKey(true, global().inDev, ch));
}
CvCal outputCal(int ch) {
  std::lock_guard<std::mutex> lk(global().m);
  return global().store.get(calKey(false, global().outDev, ch));
}

}  // namespace gl::packs::modulation
