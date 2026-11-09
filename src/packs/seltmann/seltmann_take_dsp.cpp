// Gridline — "seltmann" pack: the take recorded from an input on the start screen (J-218).
// UI thread only: the recorder (seltmann_rec_ui.cpp) drains the engine's input tap into it.
#include <algorithm>
#include <cmath>
#include "../edison/edison.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

namespace {
edison::Audio toAudio(const TakeBuffer& t) {
  edison::Audio a;
  a.rate = t.rate();
  a.channels = t.stereo() ? 2 : 1;
  a.L = t.L();
  a.R = t.R();
  return a;
}
}  // namespace

void TakeBuffer::reset(double rate, bool stereo) {
  rate_ = rate > 0 ? rate : 44100;
  stereo_ = stereo;
  L_.clear();
  R_.clear();
  peaks_.clear();
  xfAt_ = xfLen_ = xfDone_ = 0;
  changed_ = 0;
}

void TakeBuffer::join() {
  xfLen_ = std::min(kJoinFrames, frames());
  xfAt_ = frames() - xfLen_;
  xfDone_ = 0;
}

size_t TakeBuffer::append(const float* L, const float* R, size_t n) {
  if (!stereo_ || !R) R = L;
  const size_t from = frames();
  size_t i = 0;
  // a resume: the new audio fades in over the end of the part before it (raised cosine)
  const size_t xfFrom = xfDone_ < xfLen_ ? xfAt_ + xfDone_ : from;
  for (; i < n && xfDone_ < xfLen_; ++i, ++xfDone_) {
    const double u = static_cast<double>(xfDone_ + 1) / static_cast<double>(xfLen_ + 1);
    const auto w = static_cast<float>(0.5 - 0.5 * std::cos(M_PI * u));
    const size_t k = xfAt_ + xfDone_;
    L_[k] += (L[i] - L_[k]) * w;
    if (stereo_) R_[k] += (R[i] - R_[k]) * w;
  }
  const size_t room = maxFrames() > from ? maxFrames() - from : 0;
  const size_t add = std::min(n - i, room);
  L_.insert(L_.end(), L + i, L + i + add);
  if (stereo_) R_.insert(R_.end(), R + i, R + i + add);
  changed_ = std::min(xfFrom, from);
  repeak(changed_);
  return i + add;
}

void TakeBuffer::repeak(size_t from) {
  const size_t b0 = from / kPeakBlock, nb = (frames() + kPeakBlock - 1) / kPeakBlock;
  peaks_.resize(nb, 0.0f);
  for (size_t b = b0; b < nb; ++b) {
    float p = 0;
    const size_t e = std::min(frames(), (b + 1) * kPeakBlock);
    for (size_t k = b * kPeakBlock; k < e; ++k) {
      p = std::max(p, std::fabs(L_[k]));
      if (stereo_) p = std::max(p, std::fabs(R_[k]));
    }
    peaks_[b] = p;
  }
}

bool TakeBuffer::writeWav(const std::string& path, int bits, std::string* err) const {
  if (frames() == 0) {
    if (err) *err = "the recording is empty";
    return false;
  }
  return edison::writeWavFile(path, edison::makeContent(toAudio(*this)), bits == 16 || bits == 24 ? bits : 32, err);
}

std::string TakeBuffer::save(const std::string& dir, int bits, std::string* err) const {
  if (frames() == 0) {
    if (err) *err = "the recording is empty";
    return "";
  }
  edison::Audio a = toAudio(*this);
  const std::string path = edison::renderPath(dir, "Seltmann rec", edison::audioHash(a));
  if (!edison::writeWavFile(path, edison::makeContent(std::move(a)), bits == 16 || bits == 24 ? bits : 32, err)) return "";
  return path;
}

}  // namespace gl::packs::seltmann
