// Gridline — K-85: the audio an audio clip plays from its channel. See ClipSource.h.
#include "ClipSource.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include "../core/PluginParams.h"
#include "SamplePrecompute.h"

namespace gl {

namespace {

bool isSampler(const Channel& ch) { return ch.plugin.type == "gl.sampler"; }

double par(const Channel& ch, int k, double d) {
  return isSampler(ch) && k < static_cast<int>(ch.plugin.params.size()) ? static_cast<double>(ch.plugin.params[static_cast<size_t>(k)]) : d;
}

// The sampler's processing switches as its notes apply them (Generators.cpp,
// the sampler's read): (x - dc) * gain, then the stereo swap, then the region
// fades (fractions of the region between the handles).
struct Processing {
  bool normalize = false, removeDc = false, polarity = false, swap = false;
  double fadeIn = 0, fadeOut = 0;
  bool neutral() const { return !normalize && !removeDc && !polarity && !swap && fadeIn <= 0 && fadeOut <= 0; }
};

Processing processingOf(const Channel& ch) {
  Processing p;
  if (!isSampler(ch)) return p;
  p.normalize = par(ch, pp::SmpNormalize, 0) >= 0.5;
  p.removeDc = par(ch, pp::SmpRemoveDc, 0) >= 0.5;
  p.polarity = par(ch, pp::SmpPolarity, 0) >= 0.5;
  p.swap = par(ch, pp::SmpSwapStereo, 0) >= 0.5;
  p.fadeIn = std::clamp(par(ch, pp::SmpFadeIn, 0), 0.0, 0.5);
  p.fadeOut = std::clamp(par(ch, pp::SmpFadeOut, 0), 0.0, 0.5);
  return p;
}

SamplePtr process(const SamplePtr& in, const Processing& p, double startFrac, double endFrac) {
  const size_t n = in->frames();
  double peak = 0, sumL = 0, sumR = 0;
  for (size_t i = 0; i < n; ++i) {
    peak = std::max(peak, static_cast<double>(std::max(std::fabs(in->left[i]), std::fabs(in->right[i]))));
    sumL += in->left[i], sumR += in->right[i];
  }
  double gain = p.normalize && peak > 1e-6 ? 1.0 / peak : 1.0;
  if (p.polarity) gain = -gain;
  const double dcL = p.removeDc && n ? sumL / static_cast<double>(n) : 0.0, dcR = p.removeDc && n ? sumR / static_cast<double>(n) : 0.0;
  auto out = std::make_shared<Sample>();
  out->path = in->path;
  out->sampleRate = in->sampleRate;
  out->left.resize(n);
  const bool mono = in->mono() && !p.swap && dcL == dcR;
  std::vector<float> right;
  if (!mono) right.resize(n);
  const double last = static_cast<double>(n > 0 ? n - 1 : 0);
  const double lo = startFrac * last, hi = std::min(endFrac, 1.0) * last, span = std::max(1.0, hi - lo);
  for (size_t i = 0; i < n; ++i) {
    double l = (in->left[i] - dcL) * gain, r = (in->right[i] - dcR) * gain;
    if (p.swap) std::swap(l, r);
    if (p.fadeIn > 0 || p.fadeOut > 0) {
      const double rel = (static_cast<double>(i) - lo) / span;
      double g = 1;
      if (p.fadeIn > 0 && rel < p.fadeIn) g *= std::max(0.0, rel / p.fadeIn);
      if (p.fadeOut > 0 && rel > 1 - p.fadeOut) g *= std::max(0.0, (1 - rel) / p.fadeOut);
      l *= g, r *= g;
    }
    out->left[i] = static_cast<float>(l);
    if (!mono) right[i] = static_cast<float>(r);
  }
  if (mono) {
    out->setMono();
  } else {
    out->right = std::move(right);
  }
  return out;
}

SamplePtr processedSample(const SamplePtr& in, const Processing& p, double startFrac, double endFrac) {
  if (!in || p.neutral() || in->frames() < 2) return in;
  static std::mutex mu;
  static std::map<std::string, std::pair<std::weak_ptr<const Sample>, SamplePtr>> cache;
  char k[160];
  std::snprintf(k, sizeof k, "%p|%d%d%d%d|%.9g|%.9g|%.9g|%.9g", static_cast<const void*>(in.get()), p.normalize, p.removeDc, p.polarity,
                p.swap, p.fadeIn, p.fadeOut, p.fadeIn > 0 || p.fadeOut > 0 ? startFrac : 0.0, p.fadeIn > 0 || p.fadeOut > 0 ? endFrac : 0.0);
  const std::string key(k);
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end() && it->second.first.lock() == in) return it->second.second;
  SamplePtr out = process(in, p, startFrac, endFrac);
  cache[key] = {in, out};
  if (cache.size() > 64)  // drop entries whose source sample is gone
    for (auto i = cache.begin(); i != cache.end();) i = i->second.first.expired() ? cache.erase(i) : std::next(i);
  return out;
}

ClipSource make(const Channel& ch, SamplePtr raw) {
  ClipSource s;
  if (!raw || raw->frames() < 2 || raw->sampleRate <= 0) return s;
  double a = 0, b = 1;
  channelTrim(ch, a, b);
  SamplePtr smp = raw;
  if (isSampler(ch)) {
    const SamplePrecomputeFx fx = samplePrecomputeFx(ch.plugin);
    if (!fx.neutral()) smp = precomputedSample(ch.plugin.data, fx);  // (the raw sample is decoded: no wait)
    if (!smp) smp = raw;
    smp = processedSample(smp, processingOf(ch), a, b);
  }
  s.sample = smp;
  const double d = smp->seconds();
  // The handles are fractions of the last frame's position, as the notes read them.
  const double last = static_cast<double>(smp->frames() - 1) / smp->sampleRate;
  s.start = a <= 0 ? 0.0 : a * last;
  s.end = b >= 1 ? d : std::min(d, b * last);
  if (!(s.end > s.start)) s.end = std::min(d, s.start + 0.001);
  return s;
}

}  // namespace

void channelTrim(const Channel& ch, double& startFrac, double& endFrac) {
  startFrac = 0, endFrac = 1;
  if (!isSampler(ch)) return;
  double a = par(ch, pp::SmpStart, 0), b = par(ch, pp::SmpEnd, 1);
  if (!std::isfinite(a)) a = 0;
  if (!std::isfinite(b)) b = 1;
  a = std::clamp(a, 0.0, 1.0), b = std::clamp(b, 0.0, 1.0);
  if (b <= a) b = std::min(1.0, a + 0.001);  // (crossed handles: the notes' own guard)
  startFrac = a, endFrac = b;
}

double clampTrimHandle(bool isEnd, double v, double other, double seconds) {
  const double gap = std::clamp(seconds > 0 ? 0.001 / seconds : 0.001, 1e-6, 0.5);
  if (!std::isfinite(v)) v = isEnd ? 1.0 : 0.0;
  v = std::clamp(v, 0.0, 1.0);
  return isEnd ? std::max(v, std::min(1.0, other + gap)) : std::min(v, std::max(0.0, other - gap));
}

ClipSource clipSource(const Channel& ch) { return make(ch, ch.plugin.data.empty() ? nullptr : sampleCache().get(ch.plugin.data)); }

ClipSource clipSourceIfReady(const Channel& ch) {
  return make(ch, ch.plugin.data.empty() ? nullptr : sampleCache().request(ch.plugin.data));
}

bool clipPlaysReversed(const Channel& ch, const Clip& c) { return c.reverse != (par(ch, pp::SmpReverse, 0) >= 0.5); }

void clipLimits(const ClipSource& src, bool reversed, double& lo, double& hi) {
  const double d = src.seconds();
  if (reversed) {
    // The engine reads a reversed clip at (last frame - position): the handles mirror over the last frame.
    const double last = src.sample ? static_cast<double>(src.sample->frames() - 1) / src.sample->sampleRate : 0.0;
    lo = src.end >= d ? 0.0 : std::max(0.0, last - src.end);
    hi = src.start <= 0 ? d : std::max(0.0, last - src.start);
  } else {
    lo = src.start, hi = src.end;
  }
}

bool clipWindow(const ClipSource& src, bool reversed, const Clip& c, double& a, double& b) {
  if (!src.ok()) return false;
  double lo = 0, hi = 0;
  clipLimits(src, reversed, lo, hi);
  a = std::clamp(std::max(0.0, c.audioStart), lo, hi);
  b = c.audioEnd > c.audioStart ? std::clamp(c.audioEnd, lo, hi) : hi;
  return b > a;
}

bool clipWindow(const Song& song, const Clip& c, ClipSource* out, double& a, double& b) {
  const Channel* ch = c.kind == ClipKind::Audio ? song.channel(c.source) : nullptr;
  if (!ch) return false;
  ClipSource s = clipSource(*ch);
  const bool ok = clipWindow(s, clipPlaysReversed(*ch, c), c, a, b);
  if (out) *out = std::move(s);
  return ok;
}

std::vector<WarpPoint> clipWarp(const ClipSource& src, bool reversed, const Clip& c) {
  if (c.warp.empty() || !src.sample || src.sample->frames() < 2 || warpIsIdentity(c.warp)) return {};
  return warpPoints(c.warp, reversed, static_cast<double>(src.sample->frames() - 1) / src.sample->sampleRate);
}

std::vector<WarpPoint> clipWarp(const Song& song, const Clip& c) {
  const Channel* ch = c.kind == ClipKind::Audio && !c.warp.empty() ? song.channel(c.source) : nullptr;
  if (!ch) return {};
  return clipWarp(clipSource(*ch), clipPlaysReversed(*ch, c), c);
}

double warpedSpan(const std::vector<WarpPoint>& w, double a, double b) {
  return w.empty() ? b - a : warpTimeAt(w, b) - warpTimeAt(w, a);
}

void mirrorClipWindow(Clip& c, double seconds) {
  if (!(seconds > 0)) return;
  const double a = std::clamp(c.audioStart, 0.0, seconds);
  const double b = c.audioEnd > c.audioStart ? std::min(c.audioEnd, seconds) : seconds;
  const double na = std::max(0.0, seconds - b), nb = seconds - a;
  c.audioStart = na < 1e-12 ? 0.0 : na;
  c.audioEnd = a <= 1e-12 ? 0.0 : nb;  // (0: the file's end, as before)
}

void toggleClipReverse(const Song& song, Clip& c) {
  c.reverse = !c.reverse;
  if (c.kind != ClipKind::Audio) return;
  const Channel* ch = song.channel(c.source);
  if (!ch) return;
  const ClipSource s = clipSource(*ch);
  if (s.sample && s.sample->frames() > 1) mirrorClipWindow(c, static_cast<double>(s.sample->frames() - 1) / s.sample->sampleRate);
}

}  // namespace gl
