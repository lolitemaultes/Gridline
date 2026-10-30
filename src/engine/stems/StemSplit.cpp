// Gridline - J-191: stem separation driver (see StemSplit.h).
#include "StemSplit.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include "../Resample.h"
#include "HTDemucs.h"

namespace gl::stems {

std::vector<float> segmentWeights(int L) {
  std::vector<float> w(size_t(std::max(0, L)));
  const int half = L / 2;
  float peak = 0.f;
  for (int i = 0; i < L; ++i) {
    const float v = i < half ? float(i + 1) : float(L - i);
    w[size_t(i)] = v;
    peak = std::max(peak, v);
  }
  if (peak > 0)
    for (float& v : w) v /= peak;
  return w;
}

std::vector<long long> segmentOffsets(long long frames, int L, double overlap) {
  std::vector<long long> out;
  const long long stride = std::max(1LL, (long long)((1.0 - overlap) * double(L)));
  for (long long o = 0; o < frames; o += stride) out.push_back(o);
  return out;
}

bool separate(const AudioBuffer& in, double rate, const SplitRequest& req, SplitResult& out, const SplitProgress& progress,
              const std::atomic<bool>* cancel, std::string* err) {
  auto cancelled = [&] { return cancel && cancel->load(); };
  if (req.modelFiles.empty()) {
    if (err) *err = "no model";
    return false;
  }
  if (in.frames == 0 || in.channels == 0) {
    if (err) *err = "no audio";
    return false;
  }
  Pool pool(req.threads);
  const size_t nModels = req.modelFiles.size();
  std::vector<std::vector<float>> acc;  // per source: 2 planes at model rate
  std::vector<float> totals;
  long long frames = 0;
  int C = 2;
  double refMean = 0, refStd = 1, modelRate = rate;
  AudioBuffer mix;
  for (size_t m = 0; m < nModels; ++m) {
    if (cancelled()) {
      if (err) *err = "cancelled";
      return false;
    }
    HTDemucs model;
    if (!model.load(req.modelFiles[m], err)) return false;
    const DemucsConfig& cfg = model.config();
    if (m == 0) {
      out.sources = cfg.sources;
      C = cfg.audioChannels;
      modelRate = double(cfg.samplerate);
      // channels: mono -> copies, more -> the first C
      AudioBuffer src;
      src.resize(size_t(C), in.frames);
      for (int c = 0; c < C; ++c) std::memcpy(src.plane(size_t(c)), in.plane(std::min(size_t(c), in.channels - 1)), sizeof(float) * in.frames);
      mix = std::abs(rate - double(cfg.samplerate)) > 1e-6 ? resampleBuffer(src, rate, double(cfg.samplerate), 32) : std::move(src);
      frames = (long long)mix.frames;
      // normalisation by the mono reference (mean / unbiased std), as the reference separator does
      double s = 0;
      for (long long i = 0; i < frames; ++i) {
        double v = 0;
        for (int c = 0; c < C; ++c) v += mix.at(size_t(c), size_t(i));
        s += v / C;
      }
      refMean = s / double(frames);
      double s2 = 0;
      for (long long i = 0; i < frames; ++i) {
        double v = 0;
        for (int c = 0; c < C; ++c) v += mix.at(size_t(c), size_t(i));
        v = v / C - refMean;
        s2 += v * v;
      }
      refStd = frames > 1 ? std::sqrt(s2 / double(frames - 1)) : 0.0;
      if (!(refStd > 1e-8)) refStd = 1.0;  // silence: keep the input scale
      for (float& v : mix.data) v = float((v - refMean) / refStd);
      acc.assign(out.sources.size(), std::vector<float>(size_t(C) * size_t(frames), 0.f));
      totals.assign(out.sources.size(), 0.f);
    } else if (cfg.sources != out.sources || cfg.audioChannels != C) {
      if (err) *err = "the models of this set do not match";
      return false;
    }
    const int S = int(cfg.sources.size());
    std::vector<float> wsrc(size_t(S), 1.f);
    if (m < req.weights.size() && int(req.weights[m].size()) == S) wsrc = req.weights[m];
    const int L = cfg.segmentLength();
    const std::vector<float> win = segmentWeights(L);
    const std::vector<long long> offs = segmentOffsets(frames, L, req.overlap);
    std::vector<std::vector<float>> est(size_t(S), std::vector<float>(size_t(C) * size_t(frames), 0.f));
    std::vector<float> sumW(size_t(frames), 0.f);
    std::vector<float> seg(size_t(C) * size_t(L));
    for (size_t k = 0; k < offs.size(); ++k) {
      if (cancelled()) {
        if (err) *err = "cancelled";
        return false;
      }
      const long long off = offs[k];
      const int chunk = int(std::min<long long>(L, frames - off));
      const int delta = L - chunk;
      const long long start = off - delta / 2;
      for (int c = 0; c < C; ++c)
        for (int i = 0; i < L; ++i) {
          const long long j = start + i;
          seg[size_t(c) * size_t(L) + size_t(i)] = (j >= 0 && j < frames) ? mix.at(size_t(c), size_t(j)) : 0.f;
        }
      const std::vector<float> y = model.forward(pool, seg.data(), L);
      const int trim = delta / 2;
      for (int s = 0; s < S; ++s)
        for (int c = 0; c < C; ++c) {
          const float* src = &y[(size_t(s) * size_t(C) + size_t(c)) * size_t(L) + size_t(trim)];
          float* dst = &est[size_t(s)][size_t(c) * size_t(frames) + size_t(off)];
          for (int i = 0; i < chunk; ++i) dst[i] += win[size_t(i)] * src[i];
        }
      for (int i = 0; i < chunk; ++i) sumW[size_t(off + i)] += win[size_t(i)];
      if (progress) progress((double(m) + double(k + 1) / double(offs.size())) / double(nModels));
    }
    for (int s = 0; s < S; ++s) {
      const float wgt = wsrc[size_t(s)];
      totals[size_t(s)] += wgt;
      if (wgt == 0.f) continue;
      for (int c = 0; c < C; ++c)
        for (long long i = 0; i < frames; ++i)
          acc[size_t(s)][size_t(c) * size_t(frames) + size_t(i)] += wgt * est[size_t(s)][size_t(c) * size_t(frames) + size_t(i)] / sumW[size_t(i)];
    }
  }
  // de-normalise, back to the input rate / length / channel count
  out.stems.clear();
  for (size_t s = 0; s < out.sources.size(); ++s) {
    const float tot = totals[s] > 0 ? totals[s] : 1.f;
    AudioBuffer b;
    b.resize(size_t(C), size_t(frames));
    for (size_t i = 0; i < b.data.size(); ++i) b.data[i] = float(acc[s][i] / tot * refStd + refMean);
    if (std::abs(rate - modelRate) > 1e-6) b = resampleBuffer(b, modelRate, rate, 32);
    AudioBuffer r;
    const size_t outCh = in.channels == 1 ? 1 : std::min(in.channels, size_t(C));
    r.resize(outCh, in.frames);
    for (size_t i = 0; i < in.frames; ++i) {
      if (outCh == 1) {
        double v = 0;
        for (int c = 0; c < C; ++c) v += i < b.frames ? b.at(size_t(c), i) : 0.f;
        r.at(0, i) = float(v / C);
      } else {
        for (size_t c = 0; c < outCh; ++c) r.at(c, i) = i < b.frames ? b.at(c, i) : 0.f;
      }
    }
    out.stems.push_back(std::move(r));
  }
  return true;
}

}  // namespace gl::stems
