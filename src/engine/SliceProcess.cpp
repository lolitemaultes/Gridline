// Gridline — offline slice processing for the beat slicer (J-41). See SliceProcess.h.
#include "SliceProcess.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"

namespace gl::sliceproc {

namespace {

constexpr double kPi = 3.141592653589793;

float par(const PluginState& s, int i, float dflt) { return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt; }

// Fade law over n frames: t_i = (i + 1) / (n + 1).
void fadeRange(float* x, int64_t n, bool in, double sampleRate) {
  if (n <= 0) return;
  const bool cosine = static_cast<double>(n) * 1000.0 / std::max(1.0, sampleRate) <= 50.0;
  const double step = 1.0 / static_cast<double>(n + 1);
  const double lnB = std::log(11.0);  // tension 10 (fade out)
  for (int64_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i + 1) * step;
    double g;
    if (cosine) {
      g = (std::cos((1.0 - t) * kPi) + 1.0) * 0.5;
    } else if (in) {
      g = t;  // linear
    } else {
      // exponential, tension 10, mirrored for a falling ramp: (11^(1-t) - 1) / 10
      const float tm = static_cast<float>(1.0 - t);
      g = static_cast<float>(std::exp(lnB * static_cast<double>(tm)) - 1.0) * 0.1;
    }
    const double v = in ? g : (cosine ? 1.0 - g : g);
    x[i] = static_cast<float>(static_cast<double>(x[i]) * v);
  }
}

// Cubic (Catmull-Rom) read.
float readCubic(const std::vector<float>& v, double pos) {
  const int64_t n = static_cast<int64_t>(v.size());
  if (n == 0) return 0.0f;
  const int64_t i = static_cast<int64_t>(std::floor(pos));
  const double f = pos - static_cast<double>(i);
  auto at = [&](int64_t k) { return static_cast<double>(v[static_cast<size_t>(std::clamp<int64_t>(k, 0, n - 1))]); };
  const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
  const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c = -0.5 * y0 + 0.5 * y2;
  return static_cast<float>(((a * f + b) * f + c) * f + y1);
}

}  // namespace

double ratioOf(int fit) { return static_cast<float>(std::exp(static_cast<double>(static_cast<float>(fit)) / 10000.0 * 0.6931471805599453)); }

void declickEnd(float* x, int64_t len, double sampleRate) {
  if (len <= 0) return;
  // Trailing silence is left alone; the fade ends at the last non-zero frame.
  int64_t end = len;
  while (end > 0 && x[end - 1] == 0.0f) --end;
  if (end <= 0) return;
  const int64_t maxLen = std::llround(static_cast<double>(0.7f) * static_cast<double>(0.04f) * sampleRate);
  const int64_t n = std::min(maxLen, end);
  if (n <= 0) return;
  const int64_t m = std::min<int64_t>(std::llround(sampleRate * 0.001), n);
  const double k = std::log(22050.0) / static_cast<double>(n);  // tracking speed rises to the full rate
  float* p = x + (end - n);
  double dc = 0.0, w = 1.0;
  for (int64_t i = 1; i <= n; ++i) {
    const double c = std::min(1.0, std::exp(static_cast<double>(i) * k) * (2.0 / sampleRate));
    const double s = static_cast<double>(p[i - 1]);
    dc = (s - dc) * c + dc;
    if (i > n - m) w = (std::cos(static_cast<double>(i - (n - m)) * (1.0 / static_cast<double>(m + 1)) * kPi) + 1.0) * 0.5;
    p[i - 1] = static_cast<float>((s - dc) * w);
  }
}

void fadeIn(float* x, int64_t len, int64_t frames, double sampleRate) { fadeRange(x, std::min(len, frames), true, sampleRate); }

void fadeOut(float* x, int64_t len, int64_t frames, double sampleRate) {
  const int64_t n = std::min(len, frames);
  fadeRange(x + (len - n), n, false, sampleRate);
}

std::vector<std::vector<float>> stretch(const float* const* in, int nch, int64_t n, int64_t outLen, double pitch, double sampleRate) {
  std::vector<std::vector<float>> out(static_cast<size_t>(std::max(0, nch)), std::vector<float>(static_cast<size_t>(std::max<int64_t>(0, outLen)), 0.0f));
  if (n <= 0 || outLen <= 0 || nch <= 0) return out;
  pitch = std::clamp(pitch, 0.125, 8.0);
  const int64_t len1 = std::max<int64_t>(1, std::llround(static_cast<double>(outLen) * pitch));  // before resampling
  std::vector<std::vector<float>> mid(static_cast<size_t>(nch));
  if (len1 == n) {
    for (int c = 0; c < nch; ++c) mid[static_cast<size_t>(c)].assign(in[c], in[c] + n);
  } else {
    // WSOLA: Hann frames of ~23 ms at 50 % overlap; each frame is placed where
    // it best continues the previous one (search ±N/4 around the nominal
    // position, correlation on the channel sum).
    int N = 1024;
    while (N > 64 && N > sampleRate * 0.03) N /= 2;
    while (N < 4096 && N < sampleRate * 0.015) N *= 2;
    const int Ha = N / 2, D = N / 4;
    const double Hs = static_cast<double>(Ha) * static_cast<double>(n) / static_cast<double>(len1);
    std::vector<float> win(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));
    std::vector<float> mono(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      double s = 0;
      for (int c = 0; c < nch; ++c) s += in[c][i];
      mono[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    auto monoAt = [&](int64_t i) { return i >= 0 && i < n ? mono[static_cast<size_t>(i)] : 0.0f; };
    for (int c = 0; c < nch; ++c) mid[static_cast<size_t>(c)].assign(static_cast<size_t>(len1 + N), 0.0f);
    std::vector<float> wsum(static_cast<size_t>(len1 + N), 0.0f);
    int64_t prev = 0;
    for (int64_t j = 0; j * Ha < len1; ++j) {
      const int64_t nominal = std::llround(static_cast<double>(j) * Hs);
      int64_t best = nominal;
      if (j > 0) {
        const int64_t natural = prev + Ha;  // where the previous frame would continue
        double bestC = -1e300;
        for (int64_t d = -D; d <= D; d += 2) {
          const int64_t cand = nominal + d;
          double cc = 0;
          for (int i = 0; i < N; i += 4) cc += static_cast<double>(monoAt(cand + i)) * static_cast<double>(monoAt(natural + i));
          if (cc > bestC) bestC = cc, best = cand;
        }
      }
      prev = best;
      const int64_t o = j * Ha;
      for (int i = 0; i < N; ++i) {
        const int64_t src = best + i;
        if (o + i >= len1 + N) break;
        wsum[static_cast<size_t>(o + i)] += win[static_cast<size_t>(i)];
        if (src < 0 || src >= n) continue;
        for (int c = 0; c < nch; ++c) mid[static_cast<size_t>(c)][static_cast<size_t>(o + i)] += win[static_cast<size_t>(i)] * in[c][src];
      }
    }
    for (int c = 0; c < nch; ++c) {
      auto& v = mid[static_cast<size_t>(c)];
      for (size_t i = 0; i < v.size(); ++i)
        if (wsum[i] > 1e-3f) v[i] /= wsum[i];
      v.resize(static_cast<size_t>(len1));
    }
  }
  // Resample len1 → outLen (raises the pitch by len1 / outLen).
  for (int c = 0; c < nch; ++c) {
    const auto& v = mid[static_cast<size_t>(c)];
    auto& o = out[static_cast<size_t>(c)];
    if (len1 == outLen) {
      std::copy(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(outLen), o.begin());
      continue;
    }
    const double step = static_cast<double>(len1) / static_cast<double>(outLen);
    for (int64_t i = 0; i < outLen; ++i) o[static_cast<size_t>(i)] = readCubic(v, static_cast<double>(i) * step);
  }
  return out;
}

SamplePtr process(const Sample& in, const std::vector<int64_t>& starts0, const std::vector<char>& reverse, const Settings& s,
                  std::vector<int64_t>* outStarts) {
  auto out = std::make_shared<Sample>(in);
  const bool mono = in.mono();
  const int nch = mono ? 1 : 2;
  const int64_t frames = static_cast<int64_t>(in.frames());
  const double sr = in.sampleRate;
  std::vector<int64_t> starts;
  for (int64_t x : starts0)
    if (x >= 0 && x < frames) starts.push_back(x);
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  const size_t S = starts.size();
  std::vector<int64_t> lens(S);
  for (size_t k = 0; k < S; ++k) lens[k] = (k + 1 < S ? starts[k + 1] : frames) - starts[k];
  auto chan = [&](Sample& smp, int c) -> float* { return c == 0 ? smp.left.data() : smp.right.data(); };
  if (!mono) out->right.mut();  // own storage
  // 1. reverse
  for (size_t k = 0; k < S && k < reverse.size(); ++k)
    if (reverse[k] && lens[k] > 1)
      for (int c = 0; c < nch; ++c) std::reverse(chan(*out, c) + starts[k], chan(*out, c) + starts[k] + lens[k]);
  // 2. declick
  auto declickAll = [&](Sample& smp, const std::vector<int64_t>& st, const std::vector<int64_t>& ln) {
    if (!s.declick) return;
    for (size_t k = 0; k < st.size(); ++k)
      if (ln[k] > 0)
        for (int c = 0; c < nch; ++c) declickEnd(chan(smp, c) + st[k], ln[k], sr);
  };
  declickAll(*out, starts, lens);
  // 3. stretch / pitch
  std::vector<int64_t> st2 = starts, ln2 = lens;
  if (frames > 0 && (s.fit != 0 || s.pitchCents != 0)) {
    const double r = ratioOf(s.fit);
    const int64_t nf = std::llround(static_cast<double>(frames) * r);
    for (size_t k = S; k-- > 0;) {
      st2[k] = std::llround(static_cast<double>(starts[k]) * r);
      ln2[k] = k + 1 < S ? std::max<int64_t>(1, st2[k + 1] - st2[k]) : nf - st2[k];
    }
    auto res = std::make_shared<Sample>();
    res->path = in.path;
    res->sampleRate = sr;
    res->left.assign(static_cast<size_t>(std::max<int64_t>(0, nf)), 0.0f);
    if (!mono) res->right.assign(static_cast<size_t>(std::max<int64_t>(0, nf)), 0.0f);
    const bool keepAudio = s.method < 2 && s.fit != 0;
    const double pitch = std::pow(2.0, s.pitchCents / 1200.0);
    for (size_t k = 0; k < S; ++k) {
      if (ln2[k] <= 0 || st2[k] >= nf) continue;
      const int64_t outLen = std::min(ln2[k], nf - st2[k]);
      if (keepAudio) {  // the slice's own audio, then silence
        const int64_t cp = std::min(lens[k], outLen);
        for (int c = 0; c < nch; ++c) std::copy(chan(*out, c) + starts[k], chan(*out, c) + starts[k] + cp, chan(*res, c) + st2[k]);
        if (s.pitchCents != 0) {
          const float* src[2] = {chan(*res, 0) + st2[k], nch > 1 ? chan(*res, 1) + st2[k] : nullptr};
          auto v = stretch(src, nch, outLen, outLen, pitch, sr);
          for (int c = 0; c < nch; ++c) std::copy(v[static_cast<size_t>(c)].begin(), v[static_cast<size_t>(c)].end(), chan(*res, c) + st2[k]);
        }
      } else {
        const float* src[2] = {chan(*out, 0) + starts[k], nch > 1 ? chan(*out, 1) + starts[k] : nullptr};
        auto v = stretch(src, nch, lens[k], outLen, pitch, sr);
        for (int c = 0; c < nch; ++c) std::copy(v[static_cast<size_t>(c)].begin(), v[static_cast<size_t>(c)].end(), chan(*res, c) + st2[k]);
      }
    }
    if (mono) res->setMono();
    if (!keepAudio || s.pitchCents != 0) declickAll(*res, st2, ln2);
    out = res;
  }
  // 4. fades
  if (s.fadeInMs != 0 || s.fadeOutMs != 0) {
    const float perMs = static_cast<float>(sr / 1000.0);
    const int64_t fi = std::llround(static_cast<float>(s.fadeInMs) * perMs), fo = std::llround(static_cast<float>(s.fadeOutMs) * perMs);
    for (size_t k = 0; k < S; ++k)
      if (ln2[k] > 0 && st2[k] + ln2[k] <= static_cast<int64_t>(out->frames()))
        for (int c = 0; c < nch; ++c) {
          fadeIn(chan(*out, c) + st2[k], ln2[k], fi, sr);
          fadeOut(chan(*out, c) + st2[k], ln2[k], fo, sr);
        }
  }
  if (outStarts) *outStarts = st2;
  return out;
}

Settings settingsOf(const PluginState& st) {
  using namespace pp;
  Settings s;
  s.pitchCents = static_cast<int>(std::lround(par(st, SlProcPitch, 0)));
  s.fit = static_cast<int>(std::lround(par(st, SlProcFit, 0)));
  s.method = static_cast<int>(std::lround(par(st, SlProcMethod, 4)));
  s.fadeInMs = static_cast<int>(std::lround(par(st, SlProcFadeIn, 0)));
  s.fadeOutMs = static_cast<int>(std::lround(par(st, SlProcFadeOut, 0)));
  s.declick = par(st, SlProcDeclick, 1) >= 0.5f;
  return s;
}

SamplePtr processedFor(const PluginState& st, const SamplePtr& in, std::vector<int64_t>* outStarts) {
  using namespace pp;
  if (!in) return in;
  const Settings s = settingsOf(st);
  std::vector<int64_t> starts;
  std::vector<char> rev;
  const double frames = static_cast<double>(in->frames());
  for (int k = 0; k < kMaxSlices; ++k) {
    const float m = par(st, slMarker(k), -1);
    if (m < 0) continue;
    starts.push_back(std::llround(static_cast<double>(m) * frames));
    rev.push_back(par(st, slParam(k, SliceReverse), 0) >= 0.5f ? 1 : 0);
  }
  // Markers are not necessarily sorted: keep each reverse flag with its slice.
  std::vector<size_t> ord(starts.size());
  for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
  std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return starts[a] < starts[b]; });
  std::vector<int64_t> ss;
  std::vector<char> rr;
  for (size_t i : ord)
    if (ss.empty() || starts[i] != ss.back()) ss.push_back(starts[i]), rr.push_back(rev[i]);
  // Cache (UI thread; a few entries).
  std::string key = std::to_string(reinterpret_cast<uintptr_t>(in.get())) + "|" + std::to_string(s.pitchCents) + "," + std::to_string(s.fit) + "," +
                    std::to_string(s.method) + "," + std::to_string(s.fadeInMs) + "," + std::to_string(s.fadeOutMs) + "," + (s.declick ? "1" : "0");
  for (size_t i = 0; i < ss.size(); ++i) key += ";" + std::to_string(ss[i]) + (rr[i] ? "r" : "");
  struct Entry {
    SamplePtr src, out;
    std::vector<int64_t> starts;
  };
  static std::mutex mu;
  static std::map<std::string, Entry> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.src == in) {
      if (outStarts) *outStarts = it->second.starts;
      return it->second.out;
    }
  }
  std::vector<int64_t> os;
  SamplePtr out = process(*in, ss, rr, s, &os);
  {
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() >= 8) cache.erase(cache.begin());
    cache[key] = Entry{in, out, os};
  }
  if (outStarts) *outStarts = os;
  return out;
}

}  // namespace gl::sliceproc
