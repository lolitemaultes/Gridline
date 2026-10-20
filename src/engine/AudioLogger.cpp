// Gridline — audio logger (J-31). See AudioLogger.h.
#include "AudioLogger.h"
#include <algorithm>
#include <cmath>

namespace gl {

AudioLogger::AudioLogger(double sampleRate, double seconds, Source source)
    : sr_(sampleRate > 0 ? sampleRate : 44100.0), seconds_(std::clamp(seconds, 1.0, 3600.0)), source_(source) {
  keep_ = static_cast<size_t>(std::ceil(sr_ * seconds_));
  const size_t cap = keep_ + static_cast<size_t>(std::ceil(sr_));
  L_.assign(cap, 0.0f);
  R_.assign(cap, 0.0f);
}

void AudioLogger::write(const float* L, const float* R, int n) {
  if (n <= 0 || L_.empty()) return;
  const size_t cap = L_.size();
  size_t w = pos_.load(std::memory_order_relaxed);
  size_t at = w % cap;
  for (int i = 0; i < n; ++i) {
    L_[at] = L ? L[i] : 0.0f;
    R_[at] = R ? R[i] : (L ? L[i] : 0.0f);
    if (++at == cap) at = 0;
  }
  pos_.store(w + static_cast<size_t>(n), std::memory_order_release);
}

int64_t AudioLogger::available() const {
  return static_cast<int64_t>(std::min(pos_.load(std::memory_order_acquire), keep_));
}

int64_t AudioLogger::copyLast(int64_t frames, std::vector<float>& L, std::vector<float>& R) const {
  const size_t w = pos_.load(std::memory_order_acquire);
  const size_t n = static_cast<size_t>(std::clamp<int64_t>(frames, 0, static_cast<int64_t>(std::min(w, keep_))));
  L.resize(n);
  R.resize(n);
  const size_t cap = L_.size();
  size_t at = (w - n) % cap;
  for (size_t i = 0; i < n; ++i) {
    L[i] = L_[at];
    R[i] = R_[at];
    if (++at == cap) at = 0;
  }
  return static_cast<int64_t>(n);
}

float AudioLogger::peakLast(int64_t frames) const {
  const size_t w = pos_.load(std::memory_order_acquire);
  const size_t n = static_cast<size_t>(std::clamp<int64_t>(frames, 0, static_cast<int64_t>(std::min(w, keep_))));
  const size_t cap = L_.size();
  size_t at = (w - n) % cap;
  float pk = 0;
  for (size_t i = 0; i < n; ++i) {
    pk = std::max(pk, std::max(std::fabs(L_[at]), std::fabs(R_[at])));
    if (++at == cap) at = 0;
  }
  return pk;
}

}  // namespace gl
