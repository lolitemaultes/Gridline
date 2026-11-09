// Gridline — the waveform of a recording in progress (K-145). See LiveWave.h.
#include "LiveWave.h"
#include <algorithm>
#include <cmath>

namespace gl {

namespace {
constexpr int16_t kClip = 32734;  // 0.999 of full scale
}

int16_t LiveWave::q(float v) {
  if (!(v == v)) return 0;  // NaN
  const float s = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
  return static_cast<int16_t>(s >= 0 ? s + 0.5f : s - 0.5f);
}

void LiveWave::reset(double rate, int channels) {
  rate_ = rate > 0 ? rate : 44100;
  channels_ = channels == 1 ? 1 : 2;
  lv_.clear();
  lv_.emplace_back();
  frames_ = 0;
  inBucket_ = 0;
}

size_t LiveWave::buckets(int level) const {
  if (level < 0 || level >= static_cast<int>(lv_.size())) return 0;
  return lv_[static_cast<size_t>(level)].mn[0].size();
}

void LiveWave::bucket(int ch, int level, size_t b, float& mn, float& mx) const {
  const Level& L = lv_[static_cast<size_t>(level)];
  const size_t c = static_cast<size_t>(std::clamp(ch, 0, channels_ - 1));
  mn = static_cast<float>(L.mn[c][b]) / 32767.0f;
  mx = static_cast<float>(L.mx[c][b]) / 32767.0f;
}

bool LiveWave::clipped(int ch, int level, size_t b) const {
  const Level& L = lv_[static_cast<size_t>(level)];
  const size_t c = static_cast<size_t>(std::clamp(ch, 0, channels_ - 1));
  return L.mx[c][b] >= kClip || L.mn[c][b] <= -kClip;
}

size_t LiveWave::memoryBytes() const {
  size_t n = 0;
  for (const Level& L : lv_)
    for (int c = 0; c < 2; ++c) n += (L.mn[c].capacity() + L.mx[c].capacity()) * sizeof(int16_t);
  return n;
}

void LiveWave::push(float l, float r) {
  Level& L = lv_[0];
  const int16_t a = q(l), b = q(r);
  if (inBucket_ == 0) {
    L.mn[0].push_back(a), L.mx[0].push_back(a);
    if (channels_ == 2) L.mn[1].push_back(b), L.mx[1].push_back(b);
  } else {
    int16_t& m0 = L.mn[0].back();
    int16_t& x0 = L.mx[0].back();
    m0 = std::min(m0, a), x0 = std::max(x0, a);
    if (channels_ == 2) {
      int16_t& m1 = L.mn[1].back();
      int16_t& x1 = L.mx[1].back();
      m1 = std::min(m1, b), x1 = std::max(x1, b);
    }
  }
  ++frames_;
  if (++inBucket_ == kBase) inBucket_ = 0;
}

void LiveWave::parents(size_t b0) {
  size_t lo = b0;  // first changed bucket of the level below
  for (size_t j = 1;; ++j) {
    const size_t nChild = lv_[j - 1].mn[0].size();
    if (nChild <= 1) {  // the level below is the top
      lv_.resize(j);
      return;
    }
    if (j >= static_cast<size_t>(kMaxLevels)) return;
    if (lv_.size() <= j) lv_.emplace_back();
    const Level& C = lv_[j - 1];
    Level& P = lv_[j];
    const size_t n = (nChild + 1) / 2;
    const size_t p0 = std::min(lo / 2, n);
    for (int c = 0; c < channels_; ++c) {
      P.mn[c].resize(n), P.mx[c].resize(n);
      for (size_t p = p0; p < n; ++p) {
        const size_t a = 2 * p, b = std::min(2 * p + 1, nChild - 1);
        P.mn[c][p] = std::min(C.mn[c][a], C.mn[c][b]);
        P.mx[c][p] = std::max(C.mx[c][a], C.mx[c][b]);
      }
    }
    lo = p0;
  }
}

void LiveWave::add(const float* L, const float* R, size_t n) {
  if (lv_.empty()) reset(rate_, channels_);
  if (n == 0) return;
  if (channels_ == 1 || !R) R = L;
  const size_t first = lv_[0].mn[0].empty() ? 0 : lv_[0].mn[0].size() - (inBucket_ == 0 ? 0 : 1);
  for (size_t i = 0; i < n; ++i) push(L[i], R[i]);
  parents(first);
}

void LiveWave::addSilence(size_t n) {
  if (lv_.empty()) reset(rate_, channels_);
  if (n == 0) return;
  const size_t first = lv_[0].mn[0].empty() ? 0 : lv_[0].mn[0].size() - (inBucket_ == 0 ? 0 : 1);
  for (size_t i = 0; i < n; ++i) push(0.0f, 0.0f);
  parents(first);
}

size_t LiveWave::truncate(size_t frames) {
  if (lv_.empty()) reset(rate_, channels_);
  if (frames >= frames_) return frames_;
  const size_t keep = frames / kBase;
  Level& L = lv_[0];
  for (int c = 0; c < channels_; ++c) L.mn[c].resize(keep), L.mx[c].resize(keep);
  frames_ = keep * kBase;
  inBucket_ = 0;
  parents(keep > 0 ? keep - 1 : 0);
  return frames_;
}

// ------------------------------------------------------------------ fit
void LiveFit::begin(double baseFrames, double sampleRate, double startFrames) {
  rate = sampleRate > 0 ? sampleRate : 44100;
  base = std::max(1.0, baseFrames);
  shown = last = std::max(0.0, startFrames);
  vel = rate;
  active = true;
}

void LiveFit::step(double recorded, double dt) {
  recorded = std::max(0.0, recorded);
  dt = std::clamp(dt, 0.0, 0.25);
  if (recorded < shown) {  // cleared or cut back
    shown = last = recorded;
    return;
  }
  if (recorded - shown > 2.0 * std::max(rate, vel)) {  // a long stall or a jump: catch up at once
    shown = last = recorded;
    return;
  }
  // How fast frames arrive (they come in blocks; smoothed over about a quarter second).
  if (dt > 0) vel += ((recorded - last) / dt - vel) * std::min(1.0, dt / 0.25);
  last = recorded;
  vel = std::max(0.0, vel);
  // The head moves at the arrival speed every frame, trailing the recorded end by about a
  // block (20 ms of arrivals), and closes any other difference over about 60 ms; never past
  // the recorded end, never back.
  const double target = recorded - vel * 0.02;
  double next = shown + vel * dt + (target - shown) * std::min(1.0, dt / 0.06);
  next = std::clamp(next, shown, recorded);
  if (recorded - next < 0.5) next = recorded;
  shown = next;
}

double LiveFit::span(float width, float pad) const {
  const double W = std::max(1.0f, width);
  const double usable = std::max(1.0, W - static_cast<double>(std::max(0.0f, pad)));
  return std::max(std::max(1.0, base), shown * W / usable);
}

// ------------------------------------------------------------------ trace
int traceLiveWave(const LiveWave& w, int ch, const LiveWaveView& v, std::vector<LiveWavePoint>& out) {
  out.clear();
  if (w.frames() == 0 || w.levels() == 0 || v.span <= 0 || v.width <= 0) return 0;
  const double devW = std::max(1.0, static_cast<double>(v.width) * std::max(0.25f, v.pxScale));
  const double fpp = v.span / devW;
  int k = 0;
  while (k + 1 < w.levels() && static_cast<double>(LiveWave::bucketFrames(k + 1)) <= fpp) ++k;
  const double r = static_cast<double>(LiveWave::bucketFrames(k));
  const double f = k + 1 < w.levels() && fpp > r ? std::clamp(std::log2(fpp / r), 0.0, 1.0) : 0.0;
  const double total = static_cast<double>(w.frames());
  double endF = v.end < 0 ? total : std::min(v.end, total);
  endF = std::min(endF, v.start + v.span);
  const double startF = std::max(0.0, v.start);
  if (endF <= startF) return k;
  const size_t nb = w.buckets(k);
  const size_t b0 = std::min(nb, static_cast<size_t>(startF / r));
  const size_t b1 = std::min(nb, static_cast<size_t>(std::ceil(endF / r)));
  if (b1 <= b0) return k;
  const double pxPerFrame = static_cast<double>(v.width) / v.span;
  auto valueOf = [&](int level, size_t b, float& lo, float& hi) {
    float l0, h0, l1, h1;
    if (ch >= 0) {
      w.bucket(ch, level, b, lo, hi);
      return;
    }
    w.bucket(0, level, b, l0, h0);
    w.bucket(1, level, b, l1, h1);
    if (ch == -1) {
      lo = std::min(l0, l1), hi = std::max(h0, h1);
    } else {
      const float p = std::max(std::max(std::fabs(l0), std::fabs(h0)), std::max(std::fabs(l1), std::fabs(h1)));
      lo = -p, hi = p;
    }
  };
  out.reserve(b1 - b0 + 2);
  auto emit = [&](double frame, float lo, float hi) {
    const float x = static_cast<float>((frame - v.start) * pxPerFrame);
    if (!out.empty() && x < out.back().x) return;
    out.push_back({x, lo, hi});
  };
  for (size_t b = b0; b < b1; ++b) {
    float lo, hi;
    valueOf(k, b, lo, hi);
    if (f > 0) {
      float plo, phi;
      valueOf(k + 1, b / 2, plo, phi);
      const auto t = static_cast<float>(f);
      lo += (plo - lo) * t, hi += (phi - hi) * t;
    }
    const double a = static_cast<double>(b) * r, e = std::min(total, a + r);
    if (b == b0) emit(std::max(a, startF), lo, hi);
    // The point moves toward its parent's centre with its value, so at the next level both
    // children sit on the parent: the outline is the next level's exactly.
    double c = (a + e) * 0.5;
    if (f > 0) {
      const double pa = static_cast<double>(b / 2) * 2.0 * r, pc = (pa + std::min(total, pa + 2.0 * r)) * 0.5;
      c += (pc - c) * f;
    }
    emit(std::clamp(c, startF, endF), lo, hi);
    if (b + 1 == b1) emit(endF, lo, hi);
  }
  return k;
}

}  // namespace gl
