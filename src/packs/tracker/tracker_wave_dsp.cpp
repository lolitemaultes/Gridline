// Gridline - "tracker" pack (K-129): the Tracker Sampler's sample edits.
// UI thread only. An edit works on a copy of the slot's audio and the points that belong to it
// (loop, sample start and end, transpose), then commitWave writes the result as a new file next to
// the project and points the slot at it: the old file stays, so undoing the edit (the instrument
// text before it) brings the old audio back, and the project, its zip and collaboration carry the
// new file like any other sample.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include "../../engine/Sample.h"
#include "../edison/edison.h"
#include "tracker.h"

namespace gl::packs::trackerpack {

namespace fs = std::filesystem;
namespace ed = gl::packs::edison;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Moves every point of the wave through `f` (frames before the edit -> frames after it), then
// keeps them inside the new length.
void movePoints(Wave& w, const std::function<int32_t(int32_t)>& f) {
  const int32_t ls = f(w.loopStart), le = f(w.loopStart + w.loopLength);
  w.loopStart = ls;
  w.loopLength = std::max(0, le - ls);
  w.cutA = f(w.cutA);
  if (w.cutB > 0) w.cutB = f(w.cutB);
}
void tidy(Wave& w) {
  const int32_t n = w.frames();
  w.cutA = std::clamp(w.cutA, 0, std::max(0, n - 1));
  if (w.cutB > 0) w.cutB = w.cutB >= n ? 0 : std::max(w.cutB, w.cutA + 1);
  w.loopStart = std::clamp(w.loopStart, 0, n);
  w.loopLength = std::clamp(w.loopLength, 0, n - w.loopStart);
  if (w.loopLength < 1) {
    w.loopLength = 0;
    w.loop = trk::kLoopOff;
  }
}

// The clip in the wave's channel layout (a stereo clip into mono audio: its two sides averaged).
void clipChannels(const Wave& w, const Wave& clip, std::vector<float>& L, std::vector<float>& R) {
  L = clip.L;
  R.clear();
  if (w.stereo) {
    R = clip.stereo && clip.R.size() == clip.L.size() ? clip.R : clip.L;
  } else if (clip.stereo && clip.R.size() == clip.L.size()) {
    for (size_t i = 0; i < L.size(); ++i) L[i] = 0.5f * (clip.L[i] + clip.R[i]);
  }
}

template <class F>
void eachChannel(Wave& w, F f) {
  f(w.L);
  if (w.stereo) f(w.R);
}

Span orAll(const Wave& w, Span r) {
  r = clampSpan(w, r);
  return r.empty() ? Span{0, w.frames()} : r;
}

}  // namespace

Span clampSpan(const Wave& w, Span r) {
  const int32_t n = w.frames();
  if (r.a > r.b) std::swap(r.a, r.b);
  r.a = std::clamp(r.a, 0, n);
  r.b = std::clamp(r.b, r.a, n);
  return r;
}

bool waveOf(const Edit& e, int slot, const InstrumentAux& inst, Wave& out) {
  if (slot < 0 || slot >= trk::kMaxSamples) return false;
  const auto i = static_cast<size_t>(slot);
  const SamplePtr& sp = inst.files[i];
  if (!sp || sp->frames() == 0) return false;
  out = Wave{};
  out.rate = sp->sampleRate > 0 ? sp->sampleRate : 44100.0;
  out.L = sp->left;
  out.stereo = !sp->mono() && sp->right.size() == sp->left.size();
  if (out.stereo) out.R = sp->right;
  const trk::SampleDef& sd = e.text.def.samples[i];
  out.loopStart = sd.loopStart;
  out.loopLength = sd.loopLength;
  out.loop = sd.loopLength > 0 ? sd.loop : static_cast<uint8_t>(trk::kLoopOff);
  out.cutA = e.cuts.start[i];
  out.cutB = e.cuts.end[i];
  out.relNote = sd.relNote;
  tidy(out);
  return true;
}

bool waveDelete(Wave& w, Span r) {
  r = clampSpan(w, r);
  const int32_t len = r.b - r.a;
  if (len <= 0 || len >= w.frames()) return false;  // a sample keeps at least one frame
  eachChannel(w, [&](std::vector<float>& v) { v.erase(v.begin() + r.a, v.begin() + r.b); });
  movePoints(w, [&](int32_t p) { return p < r.a ? p : p >= r.b ? p - len : r.a; });
  tidy(w);
  return true;
}

Wave waveCopy(const Wave& w, Span r) {
  r = orAll(w, r);
  Wave c;
  c.rate = w.rate;
  c.stereo = w.stereo;
  c.L.assign(w.L.begin() + r.a, w.L.begin() + r.b);
  if (w.stereo) c.R.assign(w.R.begin() + r.a, w.R.begin() + r.b);
  return c;
}

bool wavePaste(Wave& w, Span r, const Wave& clip) {
  if (clip.L.empty()) return false;
  r = clampSpan(w, r);
  std::vector<float> L, R;
  clipChannels(w, clip, L, R);
  const auto n = static_cast<int32_t>(L.size());
  const int32_t gone = r.b - r.a;
  w.L.erase(w.L.begin() + r.a, w.L.begin() + r.b);
  w.L.insert(w.L.begin() + r.a, L.begin(), L.end());
  if (w.stereo) {
    w.R.erase(w.R.begin() + r.a, w.R.begin() + r.b);
    w.R.insert(w.R.begin() + r.a, R.begin(), R.end());
  }
  movePoints(w, [&](int32_t p) {
    if (p < r.a) return p;
    if (gone == 0) return p + n;     // inserted: what follows moves on
    if (p >= r.b) return p - gone + n;
    return r.a;                      // inside the replaced range: to its start
  });
  tidy(w);
  return true;
}

bool waveCrop(Wave& w, Span r) {
  r = clampSpan(w, r);
  const int32_t len = r.b - r.a;
  if (len <= 0 || len >= w.frames()) return false;
  eachChannel(w, [&](std::vector<float>& v) {
    v.erase(v.begin() + r.b, v.end());
    v.erase(v.begin(), v.begin() + r.a);
  });
  movePoints(w, [&](int32_t p) { return std::clamp(p - r.a, 0, len); });
  tidy(w);
  return true;
}

bool waveSilence(Wave& w, Span r) {
  r = clampSpan(w, r);
  if (r.empty()) return false;
  eachChannel(w, [&](std::vector<float>& v) { std::fill(v.begin() + r.a, v.begin() + r.b, 0.0f); });
  return true;
}

bool waveReverse(Wave& w, Span r) {
  r = orAll(w, r);
  if (r.b - r.a < 2) return false;
  eachChannel(w, [&](std::vector<float>& v) { std::reverse(v.begin() + r.a, v.begin() + r.b); });
  return true;
}

bool waveRamp(Wave& w, Span r, double startGain, double endGain) {
  r = orAll(w, r);
  if (r.empty() || (startGain == 1.0 && endGain == 1.0)) return false;
  const int32_t len = r.b - r.a;
  eachChannel(w, [&](std::vector<float>& v) {
    for (int32_t i = 0; i < len; ++i) {
      const double t = len > 1 ? static_cast<double>(i) / (len - 1) : 0.0;
      v[static_cast<size_t>(r.a + i)] = static_cast<float>(v[static_cast<size_t>(r.a + i)] * (startGain + (endGain - startGain) * t));
    }
  });
  return true;
}

double waveMaxScale(const Wave& w, Span r) {
  r = orAll(w, r);
  float peak = 0;
  for (int32_t i = r.a; i < r.b; ++i) {
    peak = std::max(peak, std::abs(w.L[static_cast<size_t>(i)]));
    if (w.stereo) peak = std::max(peak, std::abs(w.R[static_cast<size_t>(i)]));
  }
  return peak > 0 ? 1.0 / peak : 0.0;
}

bool waveNormalise(Wave& w, Span r) {
  const double g = waveMaxScale(w, r);
  if (g <= 0 || std::abs(g - 1.0) < 1e-7) return false;
  r = orAll(w, r);
  eachChannel(w, [&](std::vector<float>& v) {
    for (int32_t i = r.a; i < r.b; ++i) v[static_cast<size_t>(i)] = static_cast<float>(v[static_cast<size_t>(i)] * g);
  });
  return true;
}

bool waveResample(Wave& w, int semitones) {
  if (semitones == 0 || w.L.empty()) return false;
  const double ratio = std::exp2(semitones / 12.0);
  const double newLen = std::floor(w.frames() * ratio);
  if (newLen < 1 || newLen > 0x3FFFFFFF) return false;
  ed::Audio a;
  a.rate = w.rate;
  a.channels = w.stereo ? 2 : 1;
  a.L = w.L;
  a.R = w.stereo ? w.R : w.L;
  // As many frames as `ratio` times the old count: the same sound, `semitones` lower when played
  // at the old speed, so the transpose rises by as much and the pitch stays.
  ed::Audio o = ed::resampleAudio(a, w.rate * ratio, 1);
  if (o.L.empty()) return false;
  w.L = std::move(o.L);
  if (w.stereo) w.R = std::move(o.R);
  movePoints(w, [&](int32_t p) { return static_cast<int32_t>(std::floor(p * ratio)); });
  w.relNote = std::clamp(w.relNote + semitones, -48, 71);
  tidy(w);
  return true;
}

bool waveCrossfadeLoop(Wave& w, int32_t fadeFrames) {
  if (w.loop == trk::kLoopOff || w.loopLength < 2) return false;
  const int32_t ls = w.loopStart, le = w.loopStart + w.loopLength;
  const int32_t f = std::min({fadeFrames, ls, w.loopLength / 2});
  if (f <= 0) return false;
  // The loop's last frames fade into the audio just before its start, so the jump back joins
  // what the sound was doing there.
  eachChannel(w, [&](std::vector<float>& v) {
    for (int32_t i = 0; i < f; ++i) {
      const double t = static_cast<double>(i + 1) / (f + 1);
      const auto dst = static_cast<size_t>(le - f + i), src = static_cast<size_t>(ls - f + i);
      v[dst] = static_cast<float>(v[dst] * std::cos(t * kPi * 0.5) + v[src] * std::sin(t * kPi * 0.5));
    }
  });
  return true;
}

bool waveFixDc(Wave& w, Span r) {
  r = orAll(w, r);
  if (r.empty()) return false;
  bool changed = false;
  eachChannel(w, [&](std::vector<float>& v) {
    double sum = 0;
    for (int32_t i = r.a; i < r.b; ++i) sum += v[static_cast<size_t>(i)];
    const double mean = sum / (r.b - r.a);
    if (std::abs(mean) < 1e-9) return;
    changed = true;
    for (int32_t i = r.a; i < r.b; ++i) v[static_cast<size_t>(i)] = static_cast<float>(v[static_cast<size_t>(i)] - mean);
  });
  return changed;
}

bool waveMix(Wave& w, const Wave& other, double balance) {
  balance = std::clamp(balance, 0.0, 1.0);
  if (other.L.empty() || balance <= 0) return false;
  std::vector<float> L, R;
  clipChannels(w, other, L, R);
  const size_t n = std::max(w.L.size(), L.size());
  auto mixInto = [&](std::vector<float>& v, const std::vector<float>& o) {
    v.resize(n, 0.0f);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(v[i] * (1.0 - balance) + (i < o.size() ? o[i] : 0.0f) * balance);
  };
  mixInto(w.L, L);
  if (w.stereo) mixInto(w.R, R);
  tidy(w);
  return true;
}

bool waveEcho(Wave& w, int count, int32_t distance, double feedback, bool extend) {
  if (count < 1 || distance < 1 || w.L.empty()) return false;
  feedback = std::clamp(feedback, 0.0, 1.0);
  const int64_t n = w.frames();
  const int64_t out = extend ? std::min<int64_t>(n + static_cast<int64_t>(count) * distance, 0x3FFFFFFF) : n;
  auto echo = [&](std::vector<float>& v) {
    std::vector<float> o(static_cast<size_t>(out), 0.0f);
    for (int64_t i = 0; i < out; ++i) {
      double s = 0, g = 1;
      for (int k = 0; k <= count; ++k) {
        const int64_t j = i - static_cast<int64_t>(k) * distance;
        if (j < 0) break;
        if (j < n) s += v[static_cast<size_t>(j)] * g;
        g *= feedback;
      }
      o[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    v = std::move(o);
  };
  eachChannel(w, echo);
  tidy(w);
  return true;
}

bool waveMinimise(Wave& w) {
  if (w.loop == trk::kLoopOff || w.loopLength < 1) return false;
  const int32_t le = w.loopStart + w.loopLength;
  if (le >= w.frames()) return false;
  eachChannel(w, [&](std::vector<float>& v) { v.resize(static_cast<size_t>(le)); });
  tidy(w);
  return true;
}

std::string commitWave(Edit& e, int slot, const Wave& w, const std::string& dir, const std::string& name) {
  if (slot < 0 || slot >= trk::kMaxSamples || w.L.empty() || dir.empty()) return {};
  ed::Audio a;
  a.rate = w.rate;
  a.channels = w.stereo ? 2 : 1;
  a.L = w.L;
  a.R = w.stereo ? w.R : w.L;
  const std::string path = ed::renderPath(dir, name.empty() ? std::string("sample") : name, ed::audioHash(a));
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  if (!fs::is_regular_file(fs::u8path(path), ec) && !ed::writeWavFile(path, ed::makeContent(a), 32, nullptr)) return {};
  const auto i = static_cast<size_t>(slot);
  e.text.fileOf[i] = static_cast<int>(e.text.files.size());
  e.text.files.push_back(path);
  trk::SampleDef& sd = e.text.def.samples[i];
  sd.length = w.frames();
  sd.loopStart = w.loopStart;
  sd.loopLength = w.loopLength;
  sd.loop = w.loopLength > 0 ? w.loop : static_cast<uint8_t>(trk::kLoopOff);
  sd.relNote = static_cast<int8_t>(std::clamp(w.relNote, -48, 71));
  e.cuts.start[i] = w.cutA;
  e.cuts.end[i] = w.cutB;
  compactFiles(e);
  return path;
}

}  // namespace gl::packs::trackerpack
