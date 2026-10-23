// Gridline — high-quality time stretching and pitch shifting (J-87).
// See TimeStretch.h for the modes. The spectral engine is Signalsmith Stretch
// (MIT); the modes, the transient layer, the resampler, the clip renderer and
// the realtime voice are Gridline's own.
#include "TimeStretch.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <signalsmith-stretch/signalsmith-stretch.h>
#include "../core/Automation.h"

namespace gl::stretch {

namespace {
constexpr double kPi = 3.14159265358979323846;
using Engine = signalsmith::stretch::SignalsmithStretch<float>;
constexpr long kSeed = 0x6c696e65;  // fixed: renders are reproducible

struct Config {
  double block, interval;  // seconds
};
// Spectral block / hop per mode. Offline renders overlap more (smoother
// partials); realtime voices use the cheaper hop.
Config configOf(Mode m, bool offline) {
  switch (m) {
    case Mode::Mono: return offline ? Config{0.07, 0.0125} : Config{0.07, 0.0175};
    case Mode::Transient: return offline ? Config{0.08, 0.016} : Config{0.05, 0.0125};
    default: return offline ? Config{0.12, 0.02} : Config{0.12, 0.03};
  }
}

void setup(Engine& e, int nch, double sr, Mode m, bool offline) {
  const Config c = configOf(m, offline);
  e.configure(nch, std::max(64, static_cast<int>(sr * c.block)), std::max(16, static_cast<int>(sr * c.interval)));
}

// Pitch is never left to the engine's own transposition (its partials can
// land a few cents off, more on low notes): the engine only stretches time,
// and the pitch comes from resampling its output (exact). In Formant mode the
// engine moves the formants by the inverse of that resampling first, so they
// end where they started (plus the formant control).
void applyPitch(Engine& e, Mode m, double pitch, double formantSemis) {
  e.setTransposeFactor(1.0f);
  if (m == Mode::Formant) {
    e.setFormantFactor(static_cast<float>(std::pow(2.0, formantSemis / 12.0) / pitch), false);
    e.setFormantBase(0);
  } else {
    e.setFormantFactor(1.0f, false);
  }
}

// Input view with zero padding past `n` (and before 0).
struct PaddedIn {
  const float* const* x;
  int64_t n, offset;
  struct Ch {
    const float* p;
    int64_t n, offset;
    float operator[](int i) const {
      const int64_t k = offset + i;
      return k >= 0 && k < n ? p[k] : 0.0f;
    }
  };
  Ch operator[](int c) const { return Ch{x[c], n, offset}; }
};
struct OutView {
  float* const* y;
  int64_t offset;
  struct Ch {
    float* p;
    float& operator[](int i) const { return p[i]; }
  };
  Ch operator[](int c) const { return Ch{y[c] + offset}; }
};

double kaiserI0(double x) {
  double s = 1, t = 1, q = x * x / 4;
  for (int k = 1; k < 64; ++k) {
    t *= q / (static_cast<double>(k) * k);
    s += t;
    if (t < 1e-12 * s) break;
  }
  return s;
}

// Progress of an offline render: every output frame below `done` is final.
using Progress = std::function<void(int64_t done)>;

void stretchOnly(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                 const Progress* pr = nullptr);
void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen);

// Plain spectral stretch of a whole buffer (exact length, aligned): time
// stretch to outLen · pitch frames, then resampling by the pitch ratio.
void plain(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
           const Progress* pr = nullptr) {
  if (outLen <= 0) return;
  if (std::fabs(s.pitch - 1.0) < 1e-12) {
    stretchOnly(in, nch, n, out, outLen, sr, s, pr);
    return;
  }
  const SincTable& t = sincTable(Quality::Offline);
  const auto mid = static_cast<int64_t>(std::ceil(static_cast<double>(outLen) * s.pitch)) + t.zeroCrossings + 2;
  const int64_t midUsed = static_cast<int64_t>(std::llround(static_cast<double>(outLen) * s.pitch));
  std::vector<std::vector<float>> m(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(mid), 0.0f));
  std::vector<float*> mp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = m[static_cast<size_t>(c)].data();
  // The stretch covers exactly midUsed frames (aligned ends); the rest stays
  // silent. The resampler follows the stretch as it progresses (it reads
  // `reach` frames ahead of each position).
  int64_t doneOut = 0;
  const double reach = t.zeroCrossings / (std::min(1.0, 1.0 / s.pitch) * 0.97) + 2;
  auto resampleTo = [&](int64_t upTo) {
    upTo = std::clamp<int64_t>(upTo, doneOut, outLen);
    if (upTo <= doneOut) return;
    for (int64_t j = doneOut; j < upTo; ++j) {
      const double p = static_cast<double>(j) * s.pitch;
      if (nch == 2)
        readSinc2(mp[0], mp[1], mid, p, s.pitch, t, out[0][j], out[1][j]);
      else
        for (int c = 0; c < nch; ++c) out[c][j] = readSinc(mp[static_cast<size_t>(c)], mid, p, s.pitch, t);
    }
    doneOut = upTo;
    if (pr && *pr) (*pr)(doneOut);
  };
  const Progress midProgress = [&](int64_t midDone) {
    if (midDone >= midUsed) return;  // the end: below, with the silence after it
    resampleTo(static_cast<int64_t>(std::floor((static_cast<double>(midDone) - reach) / s.pitch)));
  };
  stretchOnly(in, nch, n, mp.data(), std::max<int64_t>(1, midUsed), sr, s, pr ? &midProgress : nullptr);
  resampleTo(outLen);
}

void stretchOnly(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                 const Progress* pr) {
  if (outLen <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
  if (n <= 0) return;
  Engine e(kSeed);
  setup(e, nch, sr, s.mode, true);
  applyPitch(e, s.mode, s.pitch, s.formant);
  const double rate = static_cast<double>(n) / static_cast<double>(outLen);
  // exact() needs at least one seek length of input: pad short inputs with
  // silence (and the output in proportion), keep the first outLen frames.
  const int64_t need = static_cast<int64_t>(e.outputSeekLength(static_cast<float>(rate))) + e.blockSamples() + 2;
  const int64_t pad = n < need ? need - n : 0;
  const int64_t n2 = n + pad;
  const int64_t out2 = pad > 0 ? outLen + static_cast<int64_t>(std::ceil(static_cast<double>(pad) / rate)) : outLen;
  std::vector<std::vector<float>> tmp;
  std::vector<float*> yp(static_cast<size_t>(nch));
  if (out2 != outLen) {
    tmp.assign(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(out2), 0.0f));
    for (int c = 0; c < nch; ++c) yp[static_cast<size_t>(c)] = tmp[static_cast<size_t>(c)].data();
  } else {
    for (int c = 0; c < nch; ++c) yp[static_cast<size_t>(c)] = out[c];
  }
  // The engine works in int frame counts: very long inputs go through in one
  // call each (2^31 frames is 13 hours at 44.1 kHz).
  PaddedIn pin{in, n, 0};
  // Same steps as Engine::exact, on the padded views.
  const int seekLength = e.outputSeekLength(static_cast<float>(static_cast<double>(n2) / static_cast<double>(out2)));
  e.outputSeek(pin, seekLength);
  const double r2 = static_cast<double>(n2) / static_cast<double>(out2);
  const int64_t outputIndex = std::max<int64_t>(0, out2 - static_cast<int64_t>(seekLength / r2));
  // The body in chunks (input in proportion, cumulative rounding: the same
  // mapping as one call), so progress can be reported as it goes.
  const int64_t bodyIn = n2 - seekLength;
  constexpr int64_t kChunkOut = 1 << 15;
  int64_t inDone = 0;
  for (int64_t o = 0; o < outputIndex;) {
    const int64_t o2 = std::min(outputIndex, o + kChunkOut);
    const auto in2 = static_cast<int64_t>(std::llround(static_cast<double>(bodyIn) * static_cast<double>(o2) / static_cast<double>(outputIndex)));
    e.process(PaddedIn{in, n, seekLength + inDone}, static_cast<int>(in2 - inDone), OutView{yp.data(), o}, static_cast<int>(o2 - o));
    inDone = in2, o = o2;
    if (pr && *pr && out2 == outLen) (*pr)(o);
  }
  OutView pout2{yp.data(), outputIndex};
  e.flush(pout2, static_cast<int>(out2 - outputIndex), static_cast<float>(r2));
  if (out2 != outLen)
    for (int c = 0; c < nch; ++c) std::copy(yp[static_cast<size_t>(c)], yp[static_cast<size_t>(c)] + outLen, out[c]);
  if (pr && *pr) (*pr)(outLen);
}

double envRms(const std::vector<float>& m, int64_t a, int64_t w) {
  double e = 0;
  const auto n = static_cast<int64_t>(m.size());
  for (int64_t i = std::max<int64_t>(0, a); i < std::min(n, a + w); ++i) e += static_cast<double>(m[static_cast<size_t>(i)]) * m[static_cast<size_t>(i)];
  return std::sqrt(e / static_cast<double>(std::max<int64_t>(1, w)));
}

// Transient layer: each strong onset is stretched as its own segment (aligned
// at the onset, so nothing of the hit leaks before it), and its first
// milliseconds come straight from the original (resampled for the pitch).
void withTransients(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                    double sensitivity, const Progress* pr = nullptr) {
  std::vector<float> mono(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    float v = 0;
    for (int c = 0; c < nch; ++c) v += in[c][i];
    mono[static_cast<size_t>(i)] = v / static_cast<float>(nch);
  }
  std::vector<int64_t> on = detectOnsets(mono.data(), n, sr, sensitivity);
  const double ratio = static_cast<double>(outLen) / static_cast<double>(n);  // output frames per input frame
  // Segments need room for the spectral blocks: drop onsets closer than 40 ms.
  std::vector<int64_t> cut;
  const auto minGap = static_cast<int64_t>(0.04 * sr);
  for (int64_t o : on)
    if (o > minGap && o < n - minGap && (cut.empty() || o - cut.back() >= minGap)) cut.push_back(o);
  if (cut.empty()) {
    plain(in, nch, n, out, outLen, sr, s, pr);
  } else {
    std::vector<int64_t> b{0};
    b.insert(b.end(), cut.begin(), cut.end());
    b.push_back(n);
    std::vector<int64_t> B(b.size());
    for (size_t k = 0; k < b.size(); ++k) B[k] = std::min(outLen, static_cast<int64_t>(std::llround(static_cast<double>(b[k]) * ratio)));
    B.back() = outLen;
    for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
    // Each segment also renders a short continuation into the next one (the
    // millisecond before that hit's attack), so the joins crossfade.
    const auto ext = std::max<int64_t>(8, static_cast<int64_t>(0.001 * sr));
    std::vector<std::vector<float>> si(static_cast<size_t>(nch)), so(static_cast<size_t>(nch)), tail(static_cast<size_t>(nch));
    const SincTable& tab = sincTable(Quality::Offline);
    for (size_t k = 0; k + 1 < b.size(); ++k) {
      const int64_t a = b[k], e = b[k + 1];
      const int64_t oa = B[k], oe = B[k + 1];
      if (oe <= oa) continue;
      const int64_t extIn = std::min(ext, n - e);
      const int64_t extOut = extIn > 0 ? std::max<int64_t>(1, std::llround(static_cast<double>(extIn) * ratio)) : 0;
      const int64_t len = e - a + extIn, olen = oe - oa + extOut;
      std::vector<const float*> ip(static_cast<size_t>(nch));
      std::vector<float*> op(static_cast<size_t>(nch));
      for (int c = 0; c < nch; ++c) {
        auto& v = si[static_cast<size_t>(c)];
        v.assign(in[c] + a, in[c] + a + len);
        ip[static_cast<size_t>(c)] = v.data();
        so[static_cast<size_t>(c)].assign(static_cast<size_t>(olen), 0.0f);
        op[static_cast<size_t>(c)] = so[static_cast<size_t>(c)].data();
      }
      plain(ip.data(), nch, len, op.data(), olen, sr, s);
      // Attack: the original's first milliseconds (pitch by resampling), then
      // a crossfade into the stretched audio.
      const bool attack = k > 0 || std::fabs(mono[0]) > 1e-4f;
      if (attack) {
        // Attack length: up to the envelope peak (+2 ms), 3 … 15 ms.
        const auto w = std::max<int64_t>(1, static_cast<int64_t>(0.0005 * sr));
        double pk = 0;
        int64_t pkAt = a;
        for (int64_t i = a; i < std::min(e, a + static_cast<int64_t>(0.015 * sr)); i += w)
          if (double v = envRms(mono, i, w); v > pk) pk = v, pkAt = i;
        const double atk = std::clamp(static_cast<double>(pkAt - a) / sr + 0.002, 0.003, 0.015);
        const auto A = std::min<int64_t>(oe - oa, static_cast<int64_t>(atk * sr));
        const auto X = std::min<int64_t>(oe - oa - A, static_cast<int64_t>(0.008 * sr));
        for (int c = 0; c < nch; ++c) {
          float* y = so[static_cast<size_t>(c)].data();
          for (int64_t j = 0; j < A + X; ++j) {
            const float d = readSinc(in[c], n, static_cast<double>(a) + static_cast<double>(j) * s.pitch, s.pitch, tab);
            const double g = j < A ? 1.0 : 0.5 + 0.5 * std::cos(kPi * static_cast<double>(j - A) / static_cast<double>(std::max<int64_t>(1, X)));
            y[j] = static_cast<float>(g * d + (1.0 - g) * y[j]);
          }
        }
      }
      for (int c = 0; c < nch; ++c) {
        float* y = out[c] + oa;
        const float* v = so[static_cast<size_t>(c)].data();
        const auto& t = tail[static_cast<size_t>(c)];
        const int64_t m = oe - oa, tl = static_cast<int64_t>(t.size());
        for (int64_t j = 0; j < m; ++j) {
          if (j < tl) {
            const double g = static_cast<double>(j + 1) / static_cast<double>(tl + 1);
            y[j] = static_cast<float>(t[static_cast<size_t>(j)] * (1.0 - g) + v[j] * g);
          } else {
            y[j] = v[j];
          }
        }
        tail[static_cast<size_t>(c)].assign(v + m, v + olen);
      }
      if (pr && *pr) (*pr)(oe);  // [0, oe) is final (the next segment writes after it)
    }
  }
}

}  // namespace

// ------------------------------------------------------------- modes

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Resample: return "Resample";
    case Mode::Auto: return "Auto";
    case Mode::Mono: return "Mono / vocal";
    case Mode::Transient: return "Transient";
    case Mode::Formant: return "Formant keep";
    default: return "?";
  }
}

const char* modeHint(Mode m) {
  switch (m) {
    case Mode::Resample: return "Pitch and speed change together, like a turntable (band-limited, no aliasing)";
    case Mode::Auto: return "Pitch and time apart, for full mixes, chords and loops; drum hits stay sharp";
    case Mode::Mono: return "Single voices, vocals, bass and leads: tighter timing";
    case Mode::Transient: return "Drum loops and percussion: every hit re-attacked from the original";
    case Mode::Formant: return "Pitch shift that keeps a voice's character (no chipmunk effect)";
    default: return "";
  }
}

Mode modeOf(double v) {
  if (!std::isfinite(v)) return Mode::Resample;
  const int i = static_cast<int>(std::lround(v));
  return static_cast<Mode>(std::clamp(i, 0, static_cast<int>(Mode::Count) - 1));
}

Mode modeFromFl(int code) {
  switch (code) {
    case 0: return Mode::Resample;
    case 2:   // e3 mono
    case 8:   // e2 mono
    case 9:   // e2 speech
      return Mode::Mono;
    case 3:   // slice stretch
    case 4:   // slice map
    case 7:   // e2 transient
      return Mode::Transient;
    case -2:  // stretch pro (formant control)
      return Mode::Formant;
    default:  // -1 stretch, 1 e3 generic, 5 auto, 6 e2 generic, unknown
      return Mode::Auto;
  }
}

int flCodeOf(Mode m) {
  switch (m) {
    case Mode::Resample: return 0;
    case Mode::Mono: return 2;       // e3 mono
    case Mode::Transient: return 3;  // slice stretch
    case Mode::Formant: return -2;   // stretch pro
    default: return -1;              // stretch
  }
}

// ------------------------------------------------------------- onsets

std::vector<int64_t> detectOnsets(const float* x, int64_t n, double sr, double sensitivity) {
  std::vector<int64_t> res;
  if (n < 64 || sr <= 0) return res;
  // 1 ms RMS envelope; an onset is a jump of the short-term level over the
  // level just before it (percussive attacks), confirmed by a high-frequency
  // energy rise (so slow swells and vibrato do not count).
  const auto w = std::max<int64_t>(1, static_cast<int64_t>(0.001 * sr));
  const int64_t m = n / w;
  std::vector<double> env(static_cast<size_t>(m)), hf(static_cast<size_t>(m));
  double prev = 0;
  for (int64_t k = 0; k < m; ++k) {
    double e = 0, h = 0;
    for (int64_t i = k * w; i < (k + 1) * w; ++i) {
      const double v = x[i];
      e += v * v;
      const double d = v - prev;
      h += d * d;
      prev = v;
    }
    env[static_cast<size_t>(k)] = std::sqrt(e / static_cast<double>(w));
    hf[static_cast<size_t>(k)] = std::sqrt(h / static_cast<double>(w));
  }
  // Thresholds: sensitivity 0 … 1 → level jump 18 … 6 dB within 5 ms.
  sensitivity = std::clamp(sensitivity, 0.0, 1.0);
  const double jump = std::pow(10.0, (18.0 - 12.0 * sensitivity) / 20.0);
  double peak = 0;
  for (double v : env) peak = std::max(peak, v);
  const double floor = peak * std::pow(10.0, -45.0 / 20.0) + 1e-9;
  int64_t last = -1000000;
  for (int64_t k = 12; k + 6 < m; ++k) {
    // Level before: the mean over the 10 ms ending 1 ms before; after: the max over the next 5 ms.
    double before = 0, hfBefore = 0;
    for (int64_t j = k - 11; j < k - 1; ++j) before += env[static_cast<size_t>(j)], hfBefore += hf[static_cast<size_t>(j)];
    before /= 10, hfBefore /= 10;
    double after = 0, hfAfter = 0;
    for (int64_t j = k; j < k + 5; ++j) after = std::max(after, env[static_cast<size_t>(j)]), hfAfter = std::max(hfAfter, hf[static_cast<size_t>(j)]);
    if (after < floor || after < jump * (before + 1e-9) || hfAfter < jump * 0.7 * (hfBefore + 1e-9)) continue;
    if (k - last < 30) continue;  // 30 ms apart at least
    // Attack start: the first sample of block k−1 … k+4 whose level passes
    // 10 % of the post-onset level (1 ms early, so nothing is cut).
    int64_t at = k * w;
    const double thr = before + 0.1 * (after - before);
    for (int64_t i = (k - 1) * w; i < (k + 5) * w && i < n; ++i)
      if (std::fabs(x[i]) > thr) {
        at = i;
        break;
      }
    at = std::max<int64_t>(0, at - w);
    res.push_back(at);
    last = k;
    k += 5;
  }
  return res;
}

std::shared_ptr<const std::vector<int64_t>> onsetsOf(const SamplePtr& s, double sensitivity) {
  static std::mutex mu;
  static std::map<std::pair<const Sample*, int>, std::pair<std::weak_ptr<const Sample>, std::shared_ptr<const std::vector<int64_t>>>> cache;
  if (!s || s->frames() == 0) return std::make_shared<const std::vector<int64_t>>();
  const std::pair<const Sample*, int> key{s.get(), static_cast<int>(std::lround(sensitivity * 1000))};
  {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
    if (auto it = cache.find(key); it != cache.end()) return it->second.second;
  }
  std::vector<float> mono(s->frames());
  for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (s->left[i] + s->right[i]);
  auto v = std::make_shared<const std::vector<int64_t>>(detectOnsets(mono.data(), static_cast<int64_t>(mono.size()), s->sampleRate, sensitivity));
  std::lock_guard<std::mutex> lk(mu);
  cache[key] = {s, v};
  return v;
}

// ------------------------------------------------------------- offline API

namespace {
void processP(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
              const Progress* pr);
}

void process(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s) {
  processP(in, nch, n, out, outLen, sr, s, nullptr);
}

namespace {
void processP(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
              const Progress* pr) {
  if (outLen <= 0 || nch <= 0) return;
  if (n <= 0) {
    for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
    return;
  }
  if (s.mode == Mode::Resample) {
    const double rate = static_cast<double>(n) / static_cast<double>(outLen);
    if (nch == 2)
      resample2(in[0], in[1], n, 0.0, rate, out[0], out[1], outLen);
    else
      for (int c = 0; c < nch; ++c) resample(in[c], n, 0.0, rate, out[c], outLen);
    return;
  }
  Settings t = s;
  t.pitch = std::clamp(s.pitch, 1.0 / 16.0, 16.0);
  const bool sameTime = outLen == n;
  if (sameTime && std::fabs(t.pitch - 1.0) < 1e-9 && (t.mode != Mode::Formant || std::fabs(t.formant) < 1e-9)) {
    for (int c = 0; c < nch; ++c) std::copy(in[c], in[c] + n, out[c]);
    return;
  }
  switch (t.mode) {
    case Mode::Transient: withTransients(in, nch, n, out, outLen, sr, t, 0.75, pr); break;
    case Mode::Auto: withTransients(in, nch, n, out, outLen, sr, t, 0.25, pr); break;
    default: plain(in, nch, n, out, outLen, sr, t, pr); break;
  }
}
}  // namespace

namespace {
// Variable-rate stretch (engine time only; `pitch` sets the formant compensation).
void mappedCore(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s, double pitch,
                const std::function<double(int64_t)>& srcAt) {
  Engine e(kSeed);
  setup(e, nch, sr, s.mode, true);
  applyPitch(e, s.mode, pitch, s.formant);
  const auto blk = std::max<int64_t>(16, static_cast<int64_t>(0.01 * sr));
  const double p0 = srcAt(0);
  const double r0 = std::max(1e-3, (srcAt(blk) - p0) / static_cast<double>(blk));
  const int seekLength = e.outputSeekLength(static_cast<float>(r0));
  const auto base = static_cast<int64_t>(std::floor(p0));
  e.outputSeek(PaddedIn{in, n, base}, seekLength);
  int64_t fed = base + seekLength;  // input frames given to the engine so far
  for (int64_t j = 0; j < outLen; j += blk) {
    const int64_t m = std::min(blk, outLen - j);
    const double pe = srcAt(j + m);
    const double r = std::max(1e-3, (pe - srcAt(j)) / static_cast<double>(m));
    const auto want = static_cast<int64_t>(std::llround(pe + e.inputLatency() + r * e.outputLatency()));
    const int64_t give = std::max<int64_t>(0, want - fed);
    e.process(PaddedIn{in, n, fed}, static_cast<int>(give), OutView{out, j}, static_cast<int>(m));
    fed += give;
  }
}

}  // namespace

void processMapped(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                   const std::function<double(int64_t)>& srcAt) {
  if (outLen <= 0 || nch <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
  if (n <= 0) return;
  if (s.mode == Mode::Resample) {
    // Read at the mapped positions, the cutoff following the local rate.
    const SincTable& t = sincTable(Quality::Offline);
    for (int64_t j = 0; j < outLen; ++j) {
      const double p = srcAt(j), r = std::max(1e-6, srcAt(j + 1) - p);
      if (nch == 2)
        readSinc2(in[0], in[1], n, p, r, t, out[0][j], out[1][j]);
      else
        out[0][j] = readSinc(in[0], n, p, r, t);
    }
    return;
  }
  const double pitch = std::clamp(s.pitch, 1.0 / 16.0, 16.0);
  if (std::fabs(pitch - 1.0) > 1e-12) {
    // As in plain(): stretch only (to pitch × the length), then resample.
    const SincTable& t = sincTable(Quality::Offline);
    const auto mid = static_cast<int64_t>(std::ceil(static_cast<double>(outLen) * pitch)) + t.zeroCrossings + 2;
    std::vector<std::vector<float>> m(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(mid), 0.0f));
    std::vector<float*> mp(static_cast<size_t>(nch));
    for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = m[static_cast<size_t>(c)].data();
    // Intermediate frame q plays at output time q / pitch.
    mappedCore(in, nch, n, mp.data(), mid, sr, s, pitch, [&](int64_t q) {
      const double t = static_cast<double>(q) / pitch;
      const auto j = static_cast<int64_t>(std::floor(t));
      const double a = srcAt(j);
      return a + (t - static_cast<double>(j)) * (srcAt(j + 1) - a);
    });
    if (nch == 2)
      resample2(mp[0], mp[1], mid, 0.0, pitch, out[0], out[1], outLen);
    else
      for (int c = 0; c < nch; ++c) resample(mp[static_cast<size_t>(c)], mid, 0.0, pitch, out[c], outLen);
    return;
  }
  mappedCore(in, nch, n, out, outLen, sr, s, 1.0, srcAt);
}

// ------------------------------------------------------------- resampling

const SincTable& sincTable(Quality q) {
  static const SincTable rt = [] {
    SincTable t;
    t.zeroCrossings = 16, t.phases = 512;
    const double beta = 7.5;
    const size_t len = static_cast<size_t>(t.zeroCrossings * t.phases) + 2;
    t.table.resize(len);
    for (size_t i = 0; i < len; ++i) {
      const double x = static_cast<double>(i) / t.phases;
      const double r = x / t.zeroCrossings;
      const double win = r >= 1 ? 0.0 : kaiserI0(beta * std::sqrt(1 - r * r)) / kaiserI0(beta);
      const double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      t.table[i] = static_cast<float>(sinc * win);
    }
    return t;
  }();
  static const SincTable off = [] {
    SincTable t;
    t.zeroCrossings = 48, t.phases = 1024;
    const double beta = 10.0;
    const size_t len = static_cast<size_t>(t.zeroCrossings * t.phases) + 2;
    t.table.resize(len);
    for (size_t i = 0; i < len; ++i) {
      const double x = static_cast<double>(i) / t.phases;
      const double r = x / t.zeroCrossings;
      const double win = r >= 1 ? 0.0 : kaiserI0(beta * std::sqrt(1 - r * r)) / kaiserI0(beta);
      const double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      t.table[i] = static_cast<float>(sinc * win);
    }
    return t;
  }();
  return q == Quality::Offline ? off : rt;
}

namespace {
// Cutoff (fraction of the input Nyquist): the output Nyquist when reading
// faster than 1:1, with a guard band for the kernel's transition.
inline double cutoffOf(double rate, const SincTable& t) {
  const double guard = t.zeroCrossings >= 32 ? 0.97 : 0.94;
  return std::min(1.0, 1.0 / std::clamp(rate, 1e-6, 8.0)) * guard;
}
}  // namespace

namespace {
// The kernel walk shared by every reader: weights for the taps around `pos`
// (table position stepped per tap, no per-tap division), applied to up to two
// channels at once, normalised by their sum (DC exact). Taps outside [0, n)
// read zero; the common case (all inside) skips the bounds tests.
template <int C>
inline void sincTaps(const float* const* x, int64_t n, double pos, double rate, const SincTable& t, float* out) {
  const double fc = cutoffOf(rate, t);
  const double hw = t.zeroCrossings / fc;
  const auto i0 = static_cast<int64_t>(std::ceil(pos - hw)), i1 = static_cast<int64_t>(std::floor(pos + hw));
  const float* tab = t.table.data();
  const auto last = static_cast<double>(t.table.size() - 2);
  const double step = fc * t.phases;  // table steps per input sample
  double acc[2] = {0, 0}, wsum = 0;
  const bool inside = i0 >= 0 && i1 < n;
  // Left half: i ≤ pos, table position (pos − i)·fc·phases decreasing.
  const auto ic = static_cast<int64_t>(std::floor(pos));
  double tp = (pos - static_cast<double>(i0)) * step;
  for (int64_t i = i0; i <= std::min(ic, i1); ++i, tp -= step) {
    const double q = tp < 0 ? 0 : (tp > last ? last : tp);
    const auto k = static_cast<size_t>(q);
    const double w = tab[k] + (tab[k + 1] - tab[k]) * (q - static_cast<double>(k));
    wsum += w;
    if (inside || (i >= 0 && i < n))
      for (int c = 0; c < C; ++c) acc[c] += w * x[c][i];
  }
  // Right half: i > pos, table position (i − pos)·fc·phases increasing.
  tp = (static_cast<double>(ic + 1) - pos) * step;
  for (int64_t i = ic + 1; i <= i1; ++i, tp += step) {
    const double q = tp > last ? last : tp;
    const auto k = static_cast<size_t>(q);
    const double w = tab[k] + (tab[k + 1] - tab[k]) * (q - static_cast<double>(k));
    wsum += w;
    if (inside || (i >= 0 && i < n))
      for (int c = 0; c < C; ++c) acc[c] += w * x[c][i];
  }
  for (int c = 0; c < C; ++c) out[c] = wsum > 1e-9 ? static_cast<float>(acc[c] / wsum) : 0.0f;
}
}  // namespace

float readSinc(const float* x, int64_t n, double pos, double rate, const SincTable& t) {
  const float* xs[1] = {x};
  float o;
  sincTaps<1>(xs, n, pos, rate, t, &o);
  return o;
}

void readSinc2(const float* l, const float* r, int64_t n, double pos, double rate, const SincTable& t, float& outL, float& outR) {
  const float* xs[2] = {l, r};
  float o[2];
  sincTaps<2>(xs, n, pos, rate, t, o);
  outL = o[0], outR = o[1];
}

void resample(const float* x, int64_t n, double start, double rate, float* out, int64_t outLen, Quality q) {
  const SincTable& t = sincTable(q);
  const float* xs[1] = {x};
  for (int64_t j = 0; j < outLen; ++j) sincTaps<1>(xs, n, start + static_cast<double>(j) * rate, rate, t, out + j);
}

namespace {
// Two channels in one pass (one weight computation for both).
void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen) {
  const SincTable& t = sincTable(Quality::Offline);
  const float* xs[2] = {l, r};
  float o[2];
  for (int64_t j = 0; j < outLen; ++j) {
    sincTaps<2>(xs, n, start + static_cast<double>(j) * rate, rate, t, o);
    outL[j] = o[0], outR[j] = o[1];
  }
}
}  // namespace

// ------------------------------------------------------------- rendered clips

namespace {

struct Job {
  std::weak_ptr<RenderedClip> clip;
  ClipRequest req;
  TempoMap map;  // own copy (the request's pointer is not kept)
  bool hasMap = false;
};

void renderInto(RenderedClip& rc, const ClipRequest& r) {
  const Sample& s = *r.sample;
  const double sr = s.sampleRate;
  const auto total = static_cast<int64_t>(s.frames());
  // Source window in frames; reversed clips read the reversed window.
  const auto a1 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(r.srcStart * sr)), 0, total);
  const auto b = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(r.srcEnd * sr)), a1, total);
  // A clip that starts inside its audio also takes up to 0.25 s before it as
  // context (the engine starts warm instead of from silence); dropped below.
  const int64_t pre = std::min<int64_t>(a1, static_cast<int64_t>(0.25 * sr));
  const int64_t a = a1 - pre;
  const int64_t n = b - a;
  // rc.L / rc.R are sized (and silent) already: the audio thread may read
  // them up to rc.readyFrames while this runs.
  if (b - a1 <= 1 || r.frames <= 0) return;
  // Clip seconds are measured on the unreversed sample: playback time u reads
  // source second srcStart + u·speed; reversed, frame f maps to last − f.
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    const int64_t k = r.reverse ? (total - 1 - (a + i)) : a + i;
    const auto kk = static_cast<size_t>(std::clamp<int64_t>(k, 0, total - 1));
    L[static_cast<size_t>(i)] = s.left[kk];
    R[static_cast<size_t>(i)] = s.right[kk];
  }
  // Reversed: the window [a, b) of the reversed sample starts at total − b … but the
  // engine reads (total−1) − f for clip position f, so the frames above are already in play order.
  const float* in[2] = {L.data(), R.data()};
  float* out[2] = {rc.L.data(), rc.R.data()};
  const int nch = s.mono() ? 1 : 2;
  const double toSrc = sr / r.rate;  // source frames per output frame at speed 1
  // Output frame j plays source frame (srcAt(j) − a) of the window.
  std::function<double(int64_t)> srcAt;
  const double offset = r.srcStart * sr - static_cast<double>(a);
  if (r.map && r.secPerTick > 0) {
    const TempoMap* map = r.map;
    const double st = r.startSec, t0 = r.startTick, spt = r.secPerTick, rate = r.rate;
    srcAt = [=](int64_t j) {
      const double tick = map->tickAtSeconds(st + static_cast<double>(j) / rate);
      return offset + (tick - t0) * spt * sr;
    };
  } else {
    const double step = r.speed * toSrc;
    srcAt = [=](int64_t j) { return offset + static_cast<double>(j) * step; };
  }
  Settings set;
  set.mode = r.mode;
  // The stretcher plays input frames at the output rate: a source at another
  // rate is transposed back to its own pitch.
  set.pitch = r.pitch * toSrc;
  // Linear maps (no tempo changes) render through the exact-length path (transient layer).
  const bool linear = !(r.map && r.secPerTick > 0) || r.map->constant();
  if (r.mode == Mode::Resample) {
    processMapped(in, nch, n, out, r.frames, r.rate, set, srcAt);
  } else if (linear) {
    // Frames of output that hold source audio, then silence past the window's end.
    const double p0 = srcAt(0), p1 = srcAt(1);
    const double step = p1 - p0;
    int64_t played = step > 0 ? static_cast<int64_t>(std::floor((static_cast<double>(n) - p0) / step)) : r.frames;
    played = std::clamp<int64_t>(played, 0, r.frames);
    const auto i0 = std::clamp<int64_t>(static_cast<int64_t>(std::llround(p0)), 0, n);
    const int64_t used = std::min<int64_t>(n - i0, static_cast<int64_t>(std::llround(static_cast<double>(played) * step)));
    if (played > 0 && used > 1 && step > 0) {
      // Context on both sides (the audio before the clip's start and after its
      // end, up to 0.25 s each), so neither end is faded by the engine
      // starting cold or running out of input; the extra output is dropped.
      const int64_t preOut = static_cast<int64_t>(std::floor(static_cast<double>(std::min<int64_t>(i0, pre)) / step));
      const int64_t preIn = std::min<int64_t>(i0, static_cast<int64_t>(std::llround(static_cast<double>(preOut) * step)));
      const int64_t extraIn = std::min<int64_t>(n - i0 - used, static_cast<int64_t>(0.25 * sr));
      const int64_t extraOut = extraIn > 0 ? static_cast<int64_t>(std::floor(static_cast<double>(extraIn) / step)) : 0;
      const int64_t inLen = preIn + used + (extraOut > 0 ? extraIn : 0), outLen = preOut + played + extraOut;
      const float* in2[2] = {L.data() + (i0 - preIn), R.data() + (i0 - preIn)};
      // Rendered in place where possible; with context, through a buffer
      // copied as it progresses. Either way the finished part is published
      // (readyFrames) so playback uses it while the rest renders.
      std::vector<float> tl, tr;
      float* t2[2] = {out[0], out[1]};
      const bool direct = outLen == played;
      if (!direct) {
        tl.assign(static_cast<size_t>(outLen), 0.0f);
        tr.assign(tl.size(), 0.0f);
        t2[0] = tl.data(), t2[1] = tr.data();
      }
      int64_t copied = 0;  // clip frames published
      const Progress pr = [&](int64_t done) {
        const int64_t upTo = std::clamp<int64_t>(done - (direct ? 0 : preOut), 0, played);
        if (upTo <= copied) return;
        if (!direct) {
          std::copy(tl.begin() + preOut + copied, tl.begin() + preOut + upTo, out[0] + copied);
          if (nch > 1) std::copy(tr.begin() + preOut + copied, tr.begin() + preOut + upTo, out[1] + copied);
        }
        if (nch == 1) std::copy(out[0] + copied, out[0] + upTo, rc.R.begin() + copied);
        copied = upTo;
        rc.readyFrames.store(copied, std::memory_order_release);
      };
      processP(in2, nch, direct ? used : inLen, t2, outLen, r.rate, set, &pr);
      pr(outLen + preOut);
    }
  } else {
    processMapped(in, nch, n, out, r.frames, r.rate, set, srcAt);
  }
  if (nch == 1) std::copy(rc.L.begin(), rc.L.end(), rc.R.begin());
  // (Non-finite samples never leave the engine; the stretch input is finite.)
}

std::string keyOf(const ClipRequest& r) {
  char buf[512];
  std::snprintf(buf, sizeof buf, "%p|%zu|%.9f|%.9f|%d|%d|%.12f|%.3f|%lld|%.12f|%.12f|%.6f|%.6f|", static_cast<const void*>(r.sample.get()),
                r.sample ? r.sample->frames() : 0, r.srcStart, r.srcEnd, r.reverse ? 1 : 0, static_cast<int>(r.mode), r.pitch, r.rate,
                static_cast<long long>(r.frames), r.speed, r.secPerTick, r.startTick, r.startSec);
  return std::string(buf) + (r.sample ? r.sample->path : std::string()) + "|" + (r.map && r.secPerTick > 0 ? r.mapKey : std::string());
}

class Renderer {
 public:
  ~Renderer() { shutdown(); }

  // J-103: queued renders are dropped and the worker joined (the clip it is rendering
  // finishes); later requests render on the calling thread. Idempotent.
  void shutdown() {
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
      pending_ -= static_cast<int>(queue_.size());
      queue_.clear();
      t = std::move(thread_);
    }
    work_.notify_all();
    idle_.notify_all();
    if (t.joinable()) t.join();
  }

  RenderedClipPtr get(const ClipRequest& r, bool async) {
    const std::string key = keyOf(r);
    std::unique_lock<std::mutex> lk(mu_);
    if (auto it = cache_.find(key); it != cache_.end())
      if (auto p = it->second.lock()) {
        touch(key, p);
        return p;
      }
    auto rc = std::make_shared<RenderedClip>();
    rc->source = r.sample;
    rc->L.assign(static_cast<size_t>(std::max<int64_t>(0, r.frames)), 0.0f);  // sized before anyone can read it
    rc->R.assign(rc->L.size(), 0.0f);
    cache_[key] = rc;
    if (!async || quit_) {
      lk.unlock();
      renderInto(*rc, r);
      rc->readyFrames.store(rc->frames(), std::memory_order_release);
      rc->ready.store(true, std::memory_order_release);
      lk.lock();
      touch(key, rc);
      return rc;
    }
    Job j;
    j.clip = rc;
    j.req = r;
    if (r.map) {
      j.map = *r.map;
      j.hasMap = true;
    }
    j.req.map = nullptr;
    queue_.push_back(std::move(j));
    ++pending_;
    if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    lk.unlock();
    work_.notify_one();
    // Keep it alive while queued: the clip holds it; the job only a weak ref.
    lk.lock();
    touch(key, rc);
    return rc;
  }

  void wait() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_.wait(lk, [this] { return pending_ == 0; });
  }

  void trim() {
    std::lock_guard<std::mutex> lk(mu_);
    recent_.clear();
    recentBytes_ = 0;
    for (auto it = cache_.begin(); it != cache_.end();) it = it->second.expired() ? cache_.erase(it) : std::next(it);
  }

  size_t bytes() {
    std::lock_guard<std::mutex> lk(mu_);
    size_t s = 0;
    for (auto& [k, w] : cache_)
      if (auto p = w.lock()) s += (p->L.capacity() + p->R.capacity()) * sizeof(float);
    return s;
  }

 private:
  // Recently used renders stay in memory (up to kKeepBytes) so undo / redo and
  // toggling a setting back are instant.
  static constexpr size_t kKeepBytes = size_t{512} << 20;
  void touch(const std::string& key, const std::shared_ptr<RenderedClip>& p) {
    for (auto it = recent_.begin(); it != recent_.end(); ++it)
      if (it->first == key) {
        recentBytes_ -= sizeOf(*it->second);
        recent_.erase(it);
        break;
      }
    recent_.emplace_front(key, p);
    recentBytes_ += sizeOf(*p);
    while (recentBytes_ > kKeepBytes && recent_.size() > 1) {
      recentBytes_ -= sizeOf(*recent_.back().second);
      recent_.pop_back();
    }
  }
  static size_t sizeOf(const RenderedClip& c) {
    // Size of the finished render (queued ones count with their final size later).
    return (c.L.size() + c.R.size()) * sizeof(float) + 64;
  }
  void loop() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      work_.wait(lk, [this] { return quit_ || !queue_.empty(); });
      if (quit_) return;
      Job j = std::move(queue_.front());
      queue_.pop_front();
      std::shared_ptr<RenderedClip> rc = j.clip.lock();
      if (rc && !rc->ready.load(std::memory_order_acquire)) {
        lk.unlock();
        if (j.hasMap) j.req.map = &j.map;
        renderInto(*rc, j.req);
        rc->readyFrames.store(rc->frames(), std::memory_order_release);
        rc->ready.store(true, std::memory_order_release);
        rc.reset();  // the clip (or the cache) owns it; never freed here while locked
        lk.lock();
      }
      if (--pending_ == 0) idle_.notify_all();
    }
  }
  std::mutex mu_;
  std::condition_variable work_, idle_;
  std::deque<Job> queue_;
  int pending_ = 0;
  bool quit_ = false;
  std::thread thread_;
  std::map<std::string, std::weak_ptr<RenderedClip>> cache_;
  std::list<std::pair<std::string, std::shared_ptr<RenderedClip>>> recent_;
  size_t recentBytes_ = 0;
};

Renderer& renderer() {
  static Renderer r;
  return r;
}

}  // namespace

RenderedClipPtr renderClip(const ClipRequest& r, bool async) {
  if (!r.sample || r.sample->frames() < 2 || r.frames <= 0 || r.rate <= 0) return nullptr;
  sincTable(Quality::Offline);  // built here, not on the worker's first job under a lock
  return renderer().get(r, async);
}

void waitForPendingRenders() { renderer().wait(); }
void shutdownClipRenderer() { renderer().shutdown(); }
void trimClipCache() { renderer().trim(); }
size_t clipCacheBytes() { return renderer().bytes(); }

// ------------------------------------------------------------- realtime voice

struct Voice::Impl {
  Engine engine{kSeed};
  int channels = 2;
  double sampleRate = 44100;
  double maxPitch = 8, maxSpeed = 8;
  // Engine output (before the pitch resampler): frames [0, filled) of mid,
  // read at fractional position `pos`. Compacted as it is consumed.
  std::vector<float> mid[2];
  int64_t filled = 0;
  double pos = 0;
  double inAcc = 0;          // fractional input frames owed to the engine
  double xAtFilled = 0;      // input frame (since the segment start) heard at mid frame `filled`
  int64_t inTotal = 0;       // input frames read since the segment start
  double boundary = -1;      // mid frame of the next onset (the engine restarts there), < 0: none
  std::vector<float> in[2];  // reader scratch
  std::vector<float> tail[2];  // the old segment's last frames, crossfaded into the new one
  int tailLen = 0, tailPos = 0;
  // Attack of the sound / segment: its first milliseconds straight from the
  // input (played at speed 1 before the pitch resampler), then a crossfade
  // into the engine's output — the hit stays as sharp as the original.
  std::vector<float> atk[2];
  int atkHold = 0, atkLen = 0, atkPos = 0;
  void takeAttack(int frames) {
    const int w = std::max(1, static_cast<int>(0.0005 * sampleRate));
    const int maxScan = std::min(frames, static_cast<int>(0.015 * sampleRate));
    double pk = 0;
    int pkAt = 0;
    for (int i = 0; i + w <= maxScan; i += w) {
      double e = 0;
      for (int j = i; j < i + w; ++j)
        for (int c = 0; c < channels; ++c) e += static_cast<double>(in[c][static_cast<size_t>(j)]) * in[c][static_cast<size_t>(j)];
      if (e > pk) pk = e, pkAt = i;
    }
    atkHold = std::clamp(pkAt + static_cast<int>(0.002 * sampleRate), static_cast<int>(0.003 * sampleRate), static_cast<int>(0.015 * sampleRate));
    atkLen = std::min({frames, atkHold + static_cast<int>(0.008 * sampleRate), static_cast<int>(atk[0].size())});
    atkHold = std::min(atkHold, atkLen);
    for (int c = 0; c < 2; ++c) std::copy(in[c].begin(), in[c].begin() + atkLen, atk[c].begin());
    atkPos = 0;
  }
  int inCap = 0, midCap = 0;
  static constexpr int kChunk = 256;   // engine output frames per process call
  static constexpr int kXfade = 48;    // segment crossfade (mid frames)
  int pull(Reader rd, void* ctx, int frames, bool resume) {
    float* d[2] = {in[0].data(), in[1].data()};
    const int on = rd(ctx, d, frames, resume);
    inTotal += frames;
    return on;
  }
  void noteOnset(int on, int frames, double perMid) {
    if (on < 0 || boundary >= 0) return;
    const double idx = static_cast<double>(inTotal - frames + on);  // the onset's input frame
    boundary = static_cast<double>(filled) + std::max(1.0, (idx - xAtFilled) / std::max(1e-6, perMid));
  }
  void restart(double speed, double pitch, Reader rd, void* ctx) {
    // The old segment's tail: a few more frames from silence (the onset never reached it).
    const float* zp[2] = {in[0].data(), in[1].data()};
    std::fill(in[0].begin(), in[0].begin() + kXfade, 0.0f);
    std::fill(in[1].begin(), in[1].begin() + kXfade, 0.0f);
    float* tp[2] = {tail[0].data(), tail[1].data()};
    const double perMid = speed / pitch;
    engine.process(zp, std::min(kXfade, static_cast<int>(std::lround(kXfade * perMid))), tp, kXfade);
    tailLen = kXfade, tailPos = 0;
    // The new segment starts at the onset, aligned (no pre-echo of the hit).
    engine.reset();
    inTotal = 0, inAcc = 0, xAtFilled = 0, boundary = -1;
    const int seek = std::min(inCap, engine.outputSeekLength(static_cast<float>(std::max(1e-3, perMid))));
    const int on = pull(rd, ctx, seek, true);
    const float* ip[2] = {in[0].data(), in[1].data()};
    engine.outputSeek(ip, seek);
    takeAttack(on >= 0 ? on : seek);
    noteOnset(on, seek, perMid);
  }
  // Makes mid hold at least `need` frames.
  void produce(int64_t need, double speed, double pitch, Reader rd, void* ctx) {
    const double perMid = speed / pitch;  // input frames per engine output frame
    while (filled < need) {
      if (boundary >= 0 && static_cast<double>(filled) >= boundary) restart(speed, pitch, rd, ctx);
      // Whole chunks (cut only at a restart point), so the output does not
      // depend on how the caller splits its blocks.
      int64_t lim = kChunk;
      if (boundary >= 0) lim = std::min<int64_t>(lim, std::max<int64_t>(1, static_cast<int64_t>(std::ceil(boundary)) - filled));
      const int m = static_cast<int>(std::min<int64_t>(kChunk, lim));
      inAcc += perMid * m;
      int k = static_cast<int>(std::floor(inAcc));
      k = std::clamp(k, 0, inCap);
      inAcc -= k;
      const int on = pull(rd, ctx, k, false);
      const float* ip[2] = {in[0].data(), in[1].data()};
      float* op[2] = {mid[0].data() + filled, mid[1].data() + filled};
      engine.process(ip, k, op, m);
      if (atkPos < atkLen) {
        const int x = atkLen - atkHold;
        for (int c = 0; c < channels; ++c)
          for (int j = 0; j < m && atkPos + j < atkLen; ++j) {
            const int a = atkPos + j;
            const float g = a < atkHold ? 1.0f : static_cast<float>(0.5 + 0.5 * std::cos(kPi * (a - atkHold) / std::max(1, x)));
            op[c][j] = atk[c][static_cast<size_t>(a)] * g + op[c][j] * (1.0f - g);
          }
        atkPos = std::min(atkLen, atkPos + m);
      }
      for (int c = 0; c < channels && tailPos < tailLen; ++c) {
        for (int j = 0; j < m && tailPos + j < tailLen; ++j) {
          const float g = static_cast<float>(tailPos + j + 1) / static_cast<float>(tailLen + 1);
          op[c][j] = op[c][j] * g + tail[c][static_cast<size_t>(tailPos + j)] * (1.0f - g);
        }
      }
      if (tailPos < tailLen) tailPos = std::min(tailLen, tailPos + m);
      filled += m;
      xAtFilled += perMid * m;
      noteOnset(on, k, perMid);
    }
  }
  void compact(int hw) {
    const auto drop = static_cast<int64_t>(std::floor(pos)) - hw - 2;
    if (drop <= 0 || drop < midCap / 4) return;
    for (int c = 0; c < channels; ++c)
      std::copy(mid[c].begin() + static_cast<std::ptrdiff_t>(drop), mid[c].begin() + static_cast<std::ptrdiff_t>(filled), mid[c].begin());
    filled -= drop;
    pos -= static_cast<double>(drop);
    if (boundary >= 0) boundary -= static_cast<double>(drop);
  }
};

Voice::Voice() : impl_(std::make_unique<Impl>()) {}
Voice::~Voice() = default;

void Voice::prepare(int channels, double sampleRate, Mode mode, double maxPitch, double maxSpeed) {
  mode_ = mode == Mode::Resample ? Mode::Auto : mode;
  Impl& m = *impl_;
  m.channels = std::clamp(channels, 1, 2);
  m.sampleRate = sampleRate;
  m.maxPitch = std::max(1.0, maxPitch);
  m.maxSpeed = std::max(1.0, maxSpeed);
  setup(m.engine, m.channels, sampleRate, mode_, false);
  applyPitch(m.engine, mode_, 1.0, 0.0);
  const SincTable& t = sincTable(Quality::Realtime);
  // mid: render steps at the top pitch, the resampler's reach, slack for compaction.
  const int hw = static_cast<int>(std::ceil(t.zeroCrossings / 0.94 * m.maxPitch)) + 4;
  m.midCap = 8 * (Impl::kChunk + hw) + static_cast<int>(2048 * m.maxPitch);
  // in: the engine's start-up input, or one chunk at the top input rate.
  const double perMid = m.maxSpeed * m.maxPitch;  // speed / pitch at the lowest pitch
  m.inCap = std::max({m.engine.outputSeekLength(static_cast<float>(perMid)), static_cast<int>(std::ceil(perMid * Impl::kChunk)), Impl::kXfade}) + 16;
  for (int c = 0; c < 2; ++c) {
    m.mid[c].assign(static_cast<size_t>(m.midCap), 0.0f);
    m.in[c].assign(static_cast<size_t>(m.inCap), 0.0f);
    m.tail[c].assign(static_cast<size_t>(Impl::kXfade), 0.0f);
    m.atk[c].assign(static_cast<size_t>(std::ceil(0.025 * sampleRate)) + 1, 0.0f);
  }
  prepared_ = true;
  reset();
}

void Voice::reset() {
  Impl& m = *impl_;
  m.engine.reset();
  m.filled = 0;
  m.pos = 0;
  m.inAcc = 0;
  m.xAtFilled = 0;
  m.inTotal = 0;
  m.boundary = -1;
  m.tailLen = m.tailPos = 0;
  m.atkHold = m.atkLen = m.atkPos = 0;
}

int Voice::lookahead(double speed, double pitch) const {
  pitch = std::max(1e-3, pitch);
  const double perMid = speed / pitch;
  const SincTable& t = sincTable(Quality::Realtime);
  const double hw = t.zeroCrossings / (std::min(1.0, 1.0 / pitch) * 0.94);
  return static_cast<int>(std::ceil(impl_->engine.outputSeekLength(static_cast<float>(perMid)) + (hw + Impl::kChunk) * perMid)) + 2;
}

void Voice::start(Reader rd, void* ctx, double speed, double pitch, bool attack) {
  Impl& m = *impl_;
  reset();
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  const double perMid = speed / pitch;
  const int seek = std::min(m.inCap, m.engine.outputSeekLength(static_cast<float>(std::max(1e-3, perMid))));
  const int on = m.pull(rd, ctx, seek, false);
  const float* ip[2] = {m.in[0].data(), m.in[1].data()};
  m.engine.outputSeek(ip, seek);
  if (attack) m.takeAttack(on >= 0 ? on : seek);
  m.noteOnset(on, seek, perMid);
}

void Voice::render(float* const* out, int n, double speed, double pitch, double formantSemis, Reader rd, void* ctx) {
  Impl& m = *impl_;
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  if (mode_ == Mode::Formant) applyPitch(m.engine, mode_, pitch, formantSemis);
  const SincTable& t = sincTable(Quality::Realtime);
  const int hw = static_cast<int>(std::ceil(t.zeroCrossings / (std::min(1.0, 1.0 / pitch) * 0.94))) + 1;
  int done = 0;
  while (done < n) {
    // Output frames per step, bounded so mid never overflows.
    const int room = static_cast<int>((m.midCap / 2 - hw - Impl::kChunk) / pitch);
    const int step = std::min(n - done, std::max(1, room));
    const auto need = static_cast<int64_t>(std::ceil(m.pos + (step - 1) * pitch)) + hw + 1;
    m.produce(need, speed, pitch, rd, ctx);
    for (int j = 0; j < step; ++j) {
      const double p = m.pos + j * pitch;
      if (m.channels == 2) {
        readSinc2(m.mid[0].data(), m.mid[1].data(), m.filled, p, pitch, t, out[0][done + j], out[1][done + j]);
      } else {
        out[0][done + j] = readSinc(m.mid[0].data(), m.filled, p, pitch, t);
      }
    }
    m.pos += step * pitch;
    m.compact(hw);
    done += step;
  }
}

}  // namespace gl::stretch
