// Gridline: time stretching and pitch shifting (J-87, J-153, J-157). See
// TimeStretch.h. The spectral engines are Signalsmith Stretch (MIT) and
// Rubber Band Library R3 (GPL-2.0-or-later); the streaming around them, the
// resampler, the clip renderer and the realtime voice are Gridline's own.
#include "TimeStretch.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <rubberband/RubberBandStretcher.h>
#include <signalsmith-stretch/signalsmith-stretch.h>
#include "../core/Automation.h"

namespace gl::stretch {

namespace {
using Spectral = signalsmith::stretch::SignalsmithStretch<float>;
constexpr long kSeed = 0x6c696e65;  // fixed: renders are reproducible

// ------------------------------------------------------------- configuration

// One block length and one hop at every sample rate (the same time
// resolution at 44.1, 48 and 96 kHz), offline and realtime alike. The hop
// must stay at 30 ms or more: with 20 or 25 ms hops the partials of a low
// note drift by 2 to 4 Hz near unity speed (measured at any block length).
// The 210 ms block (seven blocks overlapping) keeps stretches up to x4 free
// of hop-rate ripple and leaves the least noise between partials; the guard
// below removes the pre-echo a long block would leave ahead of hits.
constexpr double kBlockSec = 0.21;
constexpr double kIntervalSec = 0.03;
// Live effects (the Pitch Shifter): Signalsmith's default, 120 ms / 30 ms,
// for about half the latency.
constexpr double kLiveBlockSec = 0.12;
constexpr double kLiveIntervalSec = 0.03;

void configure(Spectral& e, int nch, double sr, bool live = false) {
  const int block = std::max(256, static_cast<int>(std::lround((live ? kLiveBlockSec : kBlockSec) * sr)));
  const int interval = std::max(32, static_cast<int>(std::lround((live ? kLiveIntervalSec : kIntervalSec) * sr)));
  e.configure(nch, block, interval);
  // Time only: the pitch is applied by resampling the engine's output.
  e.setTransposeFactor(1.0f);
  e.setFormantFactor(1.0f, false);
}

// ------------------------------------------------------------- envelope guard

// A spectral stretch spreads each hit over its block, so part of a drum hit
// sounds ahead of it, in the quiet before the hit (pre-echo). The guard keeps
// the stretch's output under twice (+6 dB) the source's level at the same
// moment (RMS over the last 25 ms, a period of a low bass, on both sides; the
// source looks 1 ms ahead). A steady sound is never touched (the stretch
// keeps its level within a few dB), and what sounds before a hit where the
// source was quiet is brought down to the source's level. The gain only cuts
// and moves smoothly (1 ms), on every stretch alike: no onsets, no switching.
// It runs on the engine's output (at the source's pitch, before resampling).
class Guard {
 public:
  static constexpr int kSub = 32;  // frames per energy sum
  void prepare(double sr) {
    w_ = std::max(1, static_cast<int>(std::lround(0.025 * sr / kSub)));
    window_ = w_ * kSub;
    lead_ = 0.001 * sr;
    coef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.001 * sr)));
    ring_.assign(static_cast<size_t>(w_), 0.0);
    reset();
  }
  void reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0);
    head_ = 0, n_ = 0;
    cur_ = 0, ringSum_ = 0, src_ = 0;
    gain_ = 1;
  }
  int window() const { return window_; }  // source frames the source level covers
  // Frames the source level looks ahead (1 ms of the output: the engine's
  // output runs `midPerOut` frames per output frame before the resampler).
  double lead(double midPerOut) const { return lead_ * midPerOut; }
  // Applies the guard to frames [0, m) of y. srcLevel(i): the source's mean
  // square over the `window()` frames up to the source position of frame
  // i + lead().
  template <class SrcLevel>
  void apply(float* const* y, int nch, int m, SrcLevel&& srcLevel) {
    const double inv = 1.0 / nch;
    for (int i = 0; i < m; ++i) {
      if (n_ == 0) src_ = srcLevel(i + kSub / 2);
      double e = 0;
      for (int c = 0; c < nch; ++c) e += static_cast<double>(y[c][i]) * y[c][i];
      cur_ += e * inv;
      ++n_;
      const double er = (ringSum_ + cur_) / (window_ + n_);  // the output's mean square, last 25 ms
      const float target = er > 1e-12 ? static_cast<float>(std::min(1.0, 2.0 * std::sqrt(src_ / er))) : 1.0f;
      gain_ += (target - gain_) * coef_;
      if (gain_ < 0.99999f)
        for (int c = 0; c < nch; ++c) {
          const float v = y[c][i] * gain_;
          y[c][i] = std::fabs(v) < 1e-20f ? 0.0f : v;  // (no subnormals)
        }
      if (n_ == kSub) {
        ring_[static_cast<size_t>(head_)] = cur_;
        head_ = (head_ + 1) % w_;
        ringSum_ = 0;
        for (double r : ring_) ringSum_ += r;
        cur_ = 0, n_ = 0;
      }
    }
  }

 private:
  std::vector<double> ring_;  // energy of the last w_ full sub-blocks of output
  int w_ = 1, window_ = kSub, head_ = 0, n_ = 0;
  double lead_ = 0, cur_ = 0, ringSum_ = 0, src_ = 0;
  float gain_ = 1, coef_ = 1;
};

// Energy of a whole source, kSub frames each (offline renders).
struct SourceLevel {
  std::vector<double> e;
  SourceLevel(const float* const* in, int nch, int64_t n) : e(static_cast<size_t>(n / Guard::kSub + 1), 0.0) {
    for (int c = 0; c < nch; ++c)
      for (int64_t i = 0; i < n; ++i) e[static_cast<size_t>(i / Guard::kSub)] += static_cast<double>(in[c][i]) * in[c][i] / nch;
  }
  // Mean square over input frames [a, b] (silence outside the source).
  double over(double a, double b) const {
    const auto n = static_cast<int64_t>(e.size());
    const int64_t k0 = static_cast<int64_t>(std::floor(a)) / Guard::kSub, k1 = static_cast<int64_t>(std::floor(b)) / Guard::kSub;
    double v = 0;
    for (int64_t k = std::max<int64_t>(0, k0); k <= std::min(k1, n - 1); ++k) v += e[static_cast<size_t>(k)];
    return v / (static_cast<double>(k1 - k0 + 1) * Guard::kSub);
  }
};

// Input view with zero padding outside [0, n) (silence before the start and
// past the end of the sound: past the end it flushes the engine's tail).
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

// Progress of an offline render: every output frame below `done` is final.
using Progress = std::function<void(int64_t done)>;
// Time map of a render: the input position (frames) heard at a frame.
using Map = std::function<double(int64_t)>;

// ------------------------------------------------------------- the stream

// Renders frames [0, len) of one continuous stretch whose frame q plays
// input position x(q) (non-decreasing; x(0) may lie inside the input).
// The engine starts before frame 0 on the input that leads into x(0) (up to
// one block and hop of it, silence before the input's start), so frame 0 is
// already settled, and it is fed at its real latency all the way: frame q
// is centred on x(q). Past the input's end it reads silence (its tail).
// `out` must hold len frames; progress reports finished frames.
void stream(const float* const* in, int nch, int64_t n, float* const* out, int64_t len, double sr, const Map& x,
            const Progress* pr, double midPerOut = 1.0) {
  if (len <= 0) return;
  Spectral e(kSeed);
  configure(e, nch, sr);
  const int inLat = e.inputLatency(), outLat = e.outputLatency();
  Guard guard;
  guard.prepare(sr);
  const SourceLevel level(in, nch, n);
  const auto blk = std::max<int64_t>(16, static_cast<int64_t>(0.01 * sr));
  const double x0 = x(0);
  const double r0 = std::max(1e-4, (x(blk) - x0) / static_cast<double>(blk));  // input frames per frame at the start
  // Pre-roll: frames before 0 on the map's start rate, over the input that
  // exists before x(0).
  const double lead = std::min(std::max(0.0, x0), static_cast<double>(e.blockSamples() + e.intervalSamples()));
  const auto pre = std::min<int64_t>(static_cast<int64_t>(std::floor(lead / r0)), 4 * static_cast<int64_t>(e.blockSamples()));
  const auto xq = [&](int64_t q) { return q < 0 ? x0 + static_cast<double>(q) * r0 : x(q); };
  const int64_t q0 = -pre;
  const auto base = static_cast<int64_t>(std::floor(xq(q0)));
  // Start-up input: to where the map is one output latency in (the engine
  // infers its starting rate from it), plus the input latency.
  const int seekLength = std::max(inLat, static_cast<int>(std::llround(xq(q0 + outLat) + inLat)) - static_cast<int>(base));
  e.outputSeek(PaddedIn{in, n, base}, seekLength);
  int64_t fed = base + seekLength;  // input frames given to the engine so far
  // The engine emits frame q + m + outLat's analysis once it has been fed up
  // to that frame's input position plus its input latency: feeding from the
  // map at that future frame keeps every frame where the map puts it (also
  // under tempo changes).
  std::vector<std::vector<float>> scratch(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(blk), 0.0f));
  std::vector<float*> sp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) sp[static_cast<size_t>(c)] = scratch[static_cast<size_t>(c)].data();
  std::vector<float*> at(static_cast<size_t>(nch));
  for (int64_t q = q0; q < len;) {
    // Pre-roll blocks end at 0, so no block straddles it.
    const int64_t m = q < 0 ? std::min(blk, -q) : std::min(blk, len - q);
    const auto want = static_cast<int64_t>(std::llround(xq(q + m + outLat) + inLat));
    const int64_t give = std::max<int64_t>(0, want - fed);
    float* const* dst = sp.data();
    if (q < 0) {
      e.process(PaddedIn{in, n, fed}, static_cast<int>(give), OutView{sp.data(), 0}, static_cast<int>(m));
    } else {
      e.process(PaddedIn{in, n, fed}, static_cast<int>(give), OutView{out, q}, static_cast<int>(m));
      for (int c = 0; c < nch; ++c) at[static_cast<size_t>(c)] = out[c] + q;
      dst = at.data();
    }
    const double lead = guard.lead(midPerOut);
    guard.apply(dst, nch, static_cast<int>(m), [&](int i) {
      const double p = xq(q + i + static_cast<int64_t>(std::lround(lead)));
      return level.over(p - guard.window(), p);
    });
    fed += give;
    q += m;
    if (q > 0 && pr && *pr) (*pr)(q);
  }
}

// Stretch with pitch: the engine renders an intermediate signal on a time
// line `pitch` times as long (its frame q plays at output time q / pitch),
// which band-limited resampling brings back to the output length, raising
// the pitch by `pitch`. `srcAt` maps output frames to input positions.
void stretchPitched(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, double pitch,
                    const Map& srcAt, const Progress* pr) {
  if (outLen <= 0) return;
  if (std::fabs(pitch - 1.0) < 1e-12) {
    stream(in, nch, n, out, outLen, sr, srcAt, pr);
    return;
  }
  const SincTable& t = sincTable(Quality::Offline);
  const int hw = pitch::reach(pitch, t) + 1;
  const auto midLen = static_cast<int64_t>(std::ceil(static_cast<double>(outLen) * pitch)) + hw + 2;
  std::vector<std::vector<float>> mid(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(midLen), 0.0f));
  std::vector<float*> mp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = mid[static_cast<size_t>(c)].data();
  // Intermediate frame q plays at output time q / pitch.
  const Map midAt = [&](int64_t q) {
    const double tq = static_cast<double>(q) / pitch;
    const auto j = static_cast<int64_t>(std::floor(tq));
    const double a = srcAt(j);
    return a + (tq - static_cast<double>(j)) * (srcAt(j + 1) - a);
  };
  // The resampler follows the engine as it goes: output frame j reads
  // intermediate frames up to j · pitch + hw.
  int64_t done = 0;
  auto resampleTo = [&](int64_t upTo) {
    upTo = std::clamp<int64_t>(upTo, done, outLen);
    for (int64_t j = done; j < upTo; ++j) {
      const double p = static_cast<double>(j) * pitch;
      if (nch == 2)
        readSinc2(mp[0], mp[1], midLen, p, pitch, t, out[0][j], out[1][j]);
      else
        for (int c = 0; c < nch; ++c) out[c][j] = readSinc(mp[static_cast<size_t>(c)], midLen, p, pitch, t);
    }
    if (upTo > done && pr && *pr) (*pr)(upTo);
    done = std::max(done, upTo);
  };
  const Progress midProgress = [&](int64_t midDone) {
    if (midDone >= midLen) return;  // the end: below
    resampleTo(static_cast<int64_t>(std::floor((static_cast<double>(midDone) - hw) / pitch)));
  };
  stream(in, nch, n, mp.data(), midLen, sr, midAt, pr ? &midProgress : nullptr, pitch);
  resampleTo(outLen);
}

// ------------------------------------------------------------- Rubber Band (J-157)

using RubberBand::RubberBandStretcher;

// Rubber Band reports to cerr unless it has a logger: nothing is reported
// (its warnings concern extreme ratios, which Gridline clamps; no I/O on the
// audio thread).
struct QuietLog final : RubberBandStretcher::Logger {
  void log(const char*) override {}
  void log(const char*, double) override {}
  void log(const char*, double, double) override {}
};
std::shared_ptr<RubberBandStretcher::Logger> quietLog() {
  static const auto l = std::make_shared<QuietLog>();
  return l;
}
// The R3 ("finer") engine with its standard window and the other options the
// rubberband command line uses with -3 (channels apart, formants shifted).
constexpr RubberBandStretcher::Options kRbOffline = RubberBandStretcher::OptionProcessOffline | RubberBandStretcher::OptionEngineFiner;
// Realtime voices bend their pitch: the high-consistency pitch method keeps a
// bend through 1:1 free of the click the engine's switch of method there makes.
constexpr RubberBandStretcher::Options kRbRealtime =
    RubberBandStretcher::OptionProcessRealTime | RubberBandStretcher::OptionEngineFiner | RubberBandStretcher::OptionPitchHighConsistency;
// Input around an offline render: up to this much before its start (the
// engine starts on the audio that leads in) and after its end (the last
// frames are not the engine's flush).
constexpr double kRbContextSec = 0.25;
constexpr double kRbMinRate = 1.0 / 32.0;  // input frames per output frame (x32 slower at most)
std::atomic<int64_t> gRbFed{0};            // J-170: rubberBandFramesFed()

// Renders frames [0, len) whose frame q plays input position x(q) (as
// stream()), at frequency ratio `pitch`, with Rubber Band's offline engine.
// A constant rate is one time ratio; any other map becomes a key frame map
// (one key frame per 50 ms of output) that the engine follows. Output frame 0
// is input position x(0): the engine starts `pre` frames earlier on the input
// that leads in, and those frames are dropped. Frames it does not reach stay
// as they are (the callers clear `out`).
void rubberStream(const float* const* in, int nch, int64_t n, float* const* out, int64_t len, double sr, double pitch, const Map& x,
                  const Progress* pr) {
  if (len <= 0 || nch <= 0) return;
  const int64_t blk = std::min<int64_t>(len, std::max<int64_t>(16, static_cast<int64_t>(0.01 * sr)));
  const double x0 = x(0), xe = std::max(x0, x(len));
  const double r0 = std::max(kRbMinRate, (x(blk) - x0) / static_cast<double>(blk));
  const double re = std::max(kRbMinRate, (xe - x(len - blk)) / static_cast<double>(blk));
  // One rate? (A clip at a fixed speed; a tempo-mapped clip is not.)
  bool affine = true;
  for (int k = 1; k < 16 && affine; ++k) {
    const int64_t q = len * k / 16;
    affine = std::fabs(x(q) - (x0 + (xe - x0) * static_cast<double>(q) / static_cast<double>(len))) < 0.25;
  }
  const double ctx = kRbContextSec * sr;
  const auto base = static_cast<int64_t>(std::floor(x0 - std::min(std::max(0.0, x0), ctx)));  // first input frame fed
  const auto inEnd = static_cast<int64_t>(std::ceil(xe + ctx));
  const int64_t inLen = std::max<int64_t>(1, inEnd - base);
  const auto pre = static_cast<int64_t>(std::llround((x0 - static_cast<double>(base)) / r0));  // engine frames before frame 0
  // The whole input at one ratio (affine), or the key frames: x(q) - base
  // plays at engine frame pre + q; the context after the end at its rate.
  const double rate = (xe - x0) / static_cast<double>(len);
  const double total = static_cast<double>(pre + len) + (static_cast<double>(inEnd) - xe) / re;
  const double ratio = std::clamp(affine ? 1.0 / std::max(kRbMinRate, rate) : total / static_cast<double>(inLen), 1.0 / 32.0, 32.0);
  RubberBandStretcher rb(static_cast<size_t>(std::lround(sr)), static_cast<size_t>(nch), quietLog(), kRbOffline, ratio, pitch);
  if (!affine) {
    std::map<size_t, size_t> keys;
    // (Much denser key frames, a few ms apart, make the engine drift.)
    const auto step = std::max<int64_t>(64, static_cast<int64_t>(0.05 * sr));
    int64_t lastIn = 0, lastOut = 0;
    for (int64_t q = 0;; q = std::min(len, q + step)) {
      const int64_t i = static_cast<int64_t>(std::llround(x(q))) - base, o = pre + q;
      if (i > lastIn && o > lastOut) keys[static_cast<size_t>(i)] = static_cast<size_t>(o), lastIn = i, lastOut = o;
      if (q >= len) break;
    }
    rb.setKeyFrameMap(keys);
  }
  constexpr int kChunk = 4096;
  rb.setMaxProcessSize(kChunk);
  std::vector<std::vector<float>> buf(static_cast<size_t>(nch), std::vector<float>(kChunk, 0.0f)), got = buf;
  std::vector<float*> bp(static_cast<size_t>(nch)), gp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) bp[static_cast<size_t>(c)] = buf[static_cast<size_t>(c)].data(), gp[static_cast<size_t>(c)] = got[static_cast<size_t>(c)].data();
  // Input frames [base + at, + m) into buf (silence outside the input).
  auto fill = [&](int64_t at, int m) {
    for (int c = 0; c < nch; ++c)
      for (int i = 0; i < m; ++i) {
        const int64_t k = base + at + i;
        buf[static_cast<size_t>(c)][static_cast<size_t>(i)] = k >= 0 && k < n ? in[c][k] : 0.0f;
      }
  };
  // The offline engine studies the whole input before it renders it.
  for (int64_t at = 0; at < inLen; at += kChunk) {
    const int m = static_cast<int>(std::min<int64_t>(kChunk, inLen - at));
    fill(at, m);
    rb.study(bp.data(), static_cast<size_t>(m), at + m >= inLen);
  }
  int64_t made = 0;  // engine frames retrieved (engine frame pre is output frame 0)
  const int64_t want = pre + len;
  auto drain = [&] {
    int av = 0;
    while (made < want && (av = static_cast<int>(rb.available())) > 0) {
      const auto k = static_cast<int64_t>(rb.retrieve(gp.data(), static_cast<size_t>(std::min(av, kChunk))));
      if (k <= 0) break;
      const int64_t a = std::max(made, pre), b = std::min(made + k, want);
      for (int c = 0; c < nch; ++c)
        for (int64_t j = a; j < b; ++j) out[c][j - pre] = got[static_cast<size_t>(c)][static_cast<size_t>(j - made)];
      made += k;
      if (b > a && pr && *pr) (*pr)(b - pre);
    }
  };
  // The engine moves its rate toward the key frames once per process call:
  // a key frame map needs short calls (256 frames or less) to be followed
  // closely; 4096-frame calls trailed it by up to 90 ms.
  const int64_t feed = affine ? kChunk : 256;
  for (int64_t at = 0; at < inLen && made < want; at += feed) {
    const int m = static_cast<int>(std::min<int64_t>(feed, inLen - at));
    fill(at, m);
    rb.process(bp.data(), static_cast<size_t>(m), at + m >= inLen);
    drain();
  }
}

// Resample mode: read at the mapped positions, the cutoff following the local rate.
void resampleMapped(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, const Map& srcAt) {
  const SincTable& t = sincTable(Quality::Offline);
  for (int64_t j = 0; j < outLen; ++j) {
    const double p = srcAt(j), r = std::max(1e-6, srcAt(j + 1) - p);
    if (nch == 2)
      readSinc2(in[0], in[1], n, p, r, t, out[0][j], out[1][j]);
    else
      for (int c = 0; c < nch; ++c) out[c][j] = readSinc(in[c], n, p, r, t);
  }
}

}  // namespace

// ------------------------------------------------------------- modes

const char* modeName(Mode m) { return m == Mode::Resample ? "Resample" : m == Mode::RubberBand ? "Rubber Band" : "Stretch"; }

const char* modeHint(Mode m) {
  switch (m) {
    case Mode::Resample: return "Resample: pitch and speed change together, like a turntable";
    case Mode::RubberBand: return "Rubber Band R3: sharper drums and transients, more CPU";
    default: return "Stretch: Gridline's engine, smooth on sustained sounds";
  }
}

Mode modeOf(double v) {
  if (!std::isfinite(v)) return Mode::Resample;
  const long i = std::lround(std::clamp(v, -1.0, 1e6));
  if (i <= 0) return Mode::Resample;
  return i < static_cast<long>(Mode::Count) ? static_cast<Mode>(i) : kStretch;
}

Mode modeFromFl(int code) {
  switch (code) {
    case 0: return Mode::Resample;
    case 2:   // e3 mono
    case 8:   // e2 mono
      return Mode::Mono;
    case 9:   // e2 speech
      return Mode::Voice;
    case 3:   // slice stretch
    case 4:   // slice map
      return Mode::Beats;
    case 7:   // e2 transient
      return Mode::Transient;
    case -2:  // stretch pro
      return Mode::Formant;
    default:  // -1 stretch, 1 e3 generic, 5 auto, 6 e2 generic, unknown
      return Mode::Auto;
  }
}

int flCodeOf(Mode m) {
  switch (m) {
    case Mode::Resample: return 0;
    case Mode::Mono: return 2;       // e3 mono
    case Mode::Transient: return 7;  // e2 transient
    case Mode::Formant: return -2;   // stretch pro
    case Mode::Voice: return 9;      // e2 speech
    case Mode::Beats: return 3;      // slice stretch
    default: return -1;              // stretch (also Rubber Band, which FL does not have)
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
  for (int64_t k = 26; k + 6 < m; ++k) {
    // Level before: the mean over the 10 ms ending 1 ms before; after: the max over the next 5 ms.
    double before = 0, hfBefore = 0;
    for (int64_t j = k - 11; j < k - 1; ++j) before += env[static_cast<size_t>(j)], hfBefore += hf[static_cast<size_t>(j)];
    before /= 10, hfBefore /= 10;
    double after = 0, hfAfter = 0;
    for (int64_t j = k; j < k + 5; ++j) after = std::max(after, env[static_cast<size_t>(j)]), hfAfter = std::max(hfAfter, hf[static_cast<size_t>(j)]);
    if (after < floor || after < jump * (before + 1e-9) || hfAfter < jump * 0.7 * (hfBefore + 1e-9)) continue;
    // J-140: the attack must also rise clearly above the loudest moment of the
    // 25 ms before (longer than one period down to 40 Hz): every pulse of a
    // low, buzzy tone (bass, voice, pulse waves) jumped over the mean level
    // and was taken for a new hit, chopping sustained notes into 30 ms pieces.
    double peakBefore = 0;
    for (int64_t j = k - 26; j < k - 1; ++j) peakBefore = std::max(peakBefore, env[static_cast<size_t>(j)]);
    if (after < std::sqrt(jump) * peakBefore) continue;
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

// ------------------------------------------------------------- offline API

void process(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s) {
  if (outLen <= 0 || nch <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
  if (n <= 0) return;
  const double rate = static_cast<double>(n) / static_cast<double>(outLen);
  if (s.mode == Mode::Resample) {
    if (nch == 2)
      pitch::resample2(in[0], in[1], n, 0.0, rate, out[0], out[1], outLen, Quality::Offline);
    else
      for (int c = 0; c < nch; ++c) resample(in[c], n, 0.0, rate, out[c], outLen);
    return;
  }
  const double p = std::clamp(s.pitch, 1.0 / 16.0, 16.0);
  if (outLen == n && std::fabs(p - 1.0) < 1e-9) {
    for (int c = 0; c < nch; ++c) std::copy(in[c], in[c] + n, out[c]);
    return;
  }
  const Map at = [rate](int64_t j) { return static_cast<double>(j) * rate; };
  if (s.mode == Mode::RubberBand)
    rubberStream(in, nch, n, out, outLen, sr, p, at, nullptr);
  else
    stretchPitched(in, nch, n, out, outLen, sr, p, at, nullptr);
}

void processMapped(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                   const std::function<double(int64_t)>& srcAt) {
  if (outLen <= 0 || nch <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
  if (n <= 0) return;
  if (s.mode == Mode::Resample) {
    resampleMapped(in, nch, n, out, outLen, srcAt);
    return;
  }
  const double p = std::clamp(s.pitch, 1.0 / 16.0, 16.0);
  if (s.mode == Mode::RubberBand)
    rubberStream(in, nch, n, out, outLen, sr, p, srcAt, nullptr);
  else
    stretchPitched(in, nch, n, out, outLen, sr, p, srcAt, nullptr);
}

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
  // Source window in frames; reversed clips read the reversed sample.
  const auto a1 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(r.srcStart * sr)), 0, total);
  const auto b1 = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(r.srcEnd * sr)), a1, total);
  // rc.L / rc.R are sized (and silent) already: the audio thread may read
  // them up to rc.readyFrames while this runs.
  if (b1 - a1 <= 1 || r.frames <= 0) return;
  // Context on both sides of the window (up to 0.25 s of the audio before
  // the clip's start and after its end): the stream starts on the audio that
  // leads into the clip and does not run out of input at its end.
  const auto ctx = static_cast<int64_t>(0.25 * sr);
  const int64_t a = std::max<int64_t>(0, a1 - ctx), b = std::min(total, b1 + ctx);
  const int64_t n = b - a;
  // Clip seconds are measured on the unreversed sample: playback time u reads
  // source second srcStart + u·speed; reversed, frame f maps to last − f.
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    const int64_t k = r.reverse ? (total - 1 - (a + i)) : a + i;
    const auto kk = static_cast<size_t>(std::clamp<int64_t>(k, 0, total - 1));
    L[static_cast<size_t>(i)] = s.left[kk];
    R[static_cast<size_t>(i)] = s.right[kk];
  }
  const float* in[2] = {L.data(), R.data()};
  const int nch = s.mono() ? 1 : 2;
  const double toSrc = sr / r.rate;  // source frames per output frame at speed 1
  // Output frame j plays frame srcAt(j) of the window.
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
  // Frames that hold the clip's audio: up to where the map leaves the window.
  // A clip cut inside its audio stops there (the clip's edge declicks it); a
  // clip that runs to the sound's end lets the engine's tail ring out.
  const double endPos = static_cast<double>(b1 - a);
  int64_t played = r.frames;
  {
    int64_t lo = 0, hi = r.frames;
    while (lo < hi) {
      const int64_t mid = lo + (hi - lo) / 2;
      if (srcAt(mid) >= endPos) hi = mid;
      else lo = mid + 1;
    }
    played = lo;
  }
  const int64_t len = b1 < total ? played : r.frames;
  if (len <= 0) return;
  // The source plays at the output rate inside the stretch: a source at
  // another rate is transposed back to its own pitch.
  const double p = r.mode == Mode::Resample ? 1.0 : std::clamp(r.pitch * toSrc, 1.0 / 16.0, 16.0);
  float* out[2] = {rc.L.data(), rc.R.data()};
  // Written in place; the finished part is published (readyFrames) so
  // playback uses it while the rest renders.
  int64_t published = 0;
  const Progress pr = [&](int64_t done) {
    const int64_t upTo = std::clamp<int64_t>(done, 0, len);
    if (upTo <= published) return;
    if (nch == 1) std::copy(out[0] + published, out[0] + upTo, rc.R.begin() + published);
    published = upTo;
    rc.readyFrames.store(published, std::memory_order_release);
  };
  if (r.mode == Mode::Resample) {
    resampleMapped(in, nch, n, out, len, srcAt);
    pr(len);
  } else if (r.mode == Mode::RubberBand) {
    rubberStream(in, nch, n, out, len, r.rate, p, srcAt, &pr);
    pr(len);
  } else {
    stretchPitched(in, nch, n, out, len, r.rate, p, srcAt, &pr);
    pr(len);
  }
  if (nch == 1) std::copy(rc.L.begin(), rc.L.end(), rc.R.begin());
  // (Non-finite samples never leave the engine; the stretch input is finite.)
}

std::string keyOf(const ClipRequest& r) {
  char buf[512];
  std::snprintf(buf, sizeof buf, "%p|%zu|%.9f|%.9f|%d|%d|%.12f|%.3f|%lld|%.12f|%.12f|%.6f|%.6f|", static_cast<const void*>(r.sample.get()),
                r.sample ? r.sample->frames() : 0, r.srcStart, r.srcEnd, r.reverse ? 1 : 0,
                r.mode == Mode::Resample ? 0 : r.mode == Mode::RubberBand ? 2 : 1, r.pitch, r.rate,
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

  RenderedClipPtr find(const ClipRequest& r) {
    const std::string key = keyOf(r);
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = cache_.find(key); it != cache_.end())
      if (auto p = it->second.lock()) {
        touch(key, p);
        return p;
      }
    return nullptr;
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

RenderedClipPtr findRenderedClip(const ClipRequest& r) {
  if (!r.sample || r.sample->frames() < 2 || r.frames <= 0 || r.rate <= 0) return nullptr;
  return renderer().find(r);
}

void waitForPendingRenders() { renderer().wait(); }
void shutdownClipRenderer() { renderer().shutdown(); }
void trimClipCache() { renderer().trim(); }
size_t clipCacheBytes() { return renderer().bytes(); }

// ------------------------------------------------------------- realtime voice

struct Voice::Impl {
  Spectral engine{kSeed};
  Guard guard;
  int channels = 2;
  double sampleRate = 44100;
  double maxPitch = 8, maxSpeed = 8;
  // Engine output (before the pitch resampler): frames [0, filled) of mid,
  // read at fractional position `pos`. Compacted as it is consumed.
  std::vector<float> mid[2];
  int64_t filled = 0;
  double pos = 0;
  double inAcc = 0;          // fractional input frames owed to the engine
  std::vector<float> in[2];  // reader scratch
  int inCap = 0, midCap = 0;
  // Pitch glide: two one-pole stages on the log pitch, per output frame, so
  // a pitch change (automation, a new block's value) never steps.
  static constexpr double kGlideSec = 0.006;  // time constant per stage
  double glideCoef = 1;
  double glide[2] = {0, 0};
  bool glideSet = false;
  static constexpr int kChunk = 256;  // engine output frames per process call (the stream never depends on the caller's blocks)
  // Source energy for the guard: one sum per Guard::kSub input frames read
  // since start(), the last srcCap of them kept; mid frame `filled` plays
  // input position midSrc (frame 0 plays the first input frame read).
  std::vector<double> srcE;
  int64_t srcCap = 1, inRead = 0;
  double srcCur = 0;
  double midSrc = 0;
  double midPerOut = 1;  // the pitch being played (engine frames per output frame)
  // J-157: Rubber Band's realtime engine (Engine::RubberBand). It is fed one
  // chunk of input (kChunk output frames' worth) whenever its output runs
  // dry, so the chunks it sees never depend on the callers' blocks; its start
  // delay is dropped. The pitch glides per chunk (its resampler smooths the
  // steps).
  Engine kind = Engine::Stretch;
  std::unique_ptr<RubberBandStretcher> rb;
  int rbPad = 0;              // the engine's preferred start pad at pitch 1 (input frames)
  int64_t rbSkip = 0;         // engine output frames still to drop
  double rbRatio = 0, rbPitch = 0;
  std::vector<float> rbDump[2];
  static constexpr int kRbDump = 1024;
  // J-170: pre-roll frames start() has not fed yet (startLater), the frames
  // fed since the stream began, and what its start-up needs in all (estimate).
  int rbPreLeft = 0;
  int64_t rbFed = 0, rbNeed = 0;

  void rbSet(double speed, double pitch) {
    const double ratio = 1.0 / std::max(kRbMinRate, speed);
    if (ratio != rbRatio) rb->setTimeRatio(ratio), rbRatio = ratio;
    if (pitch != rbPitch) rb->setPitchScale(pitch), rbPitch = pitch;
  }
  // Gives the engine k input frames from the reader (none: silence), k <= inCap.
  void rbFeed(Reader rd, void* ctx, int k) {
    if (k <= 0) return;
    float* d[2] = {in[0].data(), in[1].data()};
    if (rd) {
      rd(ctx, d, k);
    } else {
      std::fill(in[0].begin(), in[0].begin() + k, 0.0f);
      std::fill(in[1].begin(), in[1].begin() + k, 0.0f);
    }
    const float* ip[2] = {in[0].data(), in[1].data()};
    rb->process(ip, static_cast<size_t>(k), false);
    rbFed += k;
    if (rd) gRbFed.fetch_add(k, std::memory_order_relaxed);  // (sound only: not the silent start pads)
  }
  // J-170: feeds the pre-roll start() would have fed (chunk by chunk, the
  // same chunks), about `budget` frames of it; returns the budget left.
  int rbPre(Reader rd, void* ctx, int budget) {
    while (rbPreLeft > 0 && budget > 0) {
      const int k = std::min(rbPreLeft, inCap);
      rbFeed(rd, ctx, k);
      rbDrop();
      rbPreLeft -= k;
      budget -= k;
    }
    return budget;
  }
  bool rbReady() const { return rbPreLeft == 0 && rbSkip == 0 && rb->available() > 0; }
  // One step of the dry loop of rbRender(): the next chunk of input, at the gliding pitch.
  int rbStep(Reader rd, void* ctx, double speed, double target) {
    rbSet(speed, glideBy(target, kChunk));
    inAcc += std::max(kRbMinRate, speed) * kChunk;
    const int k = std::clamp(static_cast<int>(std::floor(inAcc)), 0, inCap);
    inAcc -= k;
    rbFeed(rd, ctx, k);
    return k;
  }
  // Drops what the engine has of the frames still to skip.
  void rbDrop() {
    while (rbSkip > 0) {
      const int av = static_cast<int>(rb->available());
      if (av <= 0) return;
      float* o[2] = {rbDump[0].data(), rbDump[1].data()};
      const auto k = static_cast<int64_t>(rb->retrieve(o, static_cast<size_t>(std::min<int64_t>({av, rbSkip, kRbDump}))));
      if (k <= 0) return;
      rbSkip -= k;
    }
  }
  // One glide step of `frames` output frames towards `target` (log pitch).
  double glideBy(double target, int frames) {
    const double a = 1.0 - std::pow(1.0 - glideCoef, frames);
    glide[0] += (target - glide[0]) * a, glide[1] += (glide[0] - glide[1]) * a;
    if (std::fabs(glide[0] - target) < 1e-9 && std::fabs(glide[1] - target) < 1e-9) glide[0] = glide[1] = target;
    return std::exp(glide[1]);
  }
  void rbRender(float* const* out, int n, double speed, double pitch, Reader rd, void* ctx) {
    rbPre(rd, ctx, std::numeric_limits<int>::max());  // J-170: what startLater() left of the pre-roll
    const double target = std::log(pitch);
    if (!glideSet) glide[0] = glide[1] = target, glideSet = true;
    rbSet(speed, rbPitch > 0 ? rbPitch : pitch);
    int done = 0;
    while (done < n) {
      rbDrop();
      const int av = rbSkip > 0 ? 0 : static_cast<int>(rb->available());
      if (av > 0) {
        float* o[2] = {out[0] + done, channels == 2 ? out[1] + done : nullptr};
        const auto k = static_cast<int>(rb->retrieve(o, static_cast<size_t>(std::min(av, n - done))));
        if (k <= 0) break;
        done += k;
        continue;
      }
      rbStep(rd, ctx, speed, target);  // dry: the next chunk of input
    }
    for (int c = 0; c < channels; ++c) std::fill(out[c] + done, out[c] + n, 0.0f);  // (never: the engine always delivers)
  }

  void pull(Reader rd, void* ctx, int frames) {
    float* d[2] = {in[0].data(), in[1].data()};
    rd(ctx, d, frames);
    for (int i = 0; i < frames; ++i) {
      srcCur += 0.5 * (static_cast<double>(in[0][i]) * in[0][i] + static_cast<double>(in[1][i]) * in[1][i]);
      if (++inRead % Guard::kSub == 0) {
        srcE[static_cast<size_t>((inRead / Guard::kSub - 1) % srcCap)] = srcCur;
        srcCur = 0;
      }
    }
  }
  // Source mean square over input frames [a, b] (what is still kept of it;
  // silence before the stream's start).
  double srcOver(double a, double b) const {
    const int64_t done = inRead / Guard::kSub;  // full sums stored
    const int64_t k0 = static_cast<int64_t>(std::floor(a)) / Guard::kSub, k1 = static_cast<int64_t>(std::floor(b)) / Guard::kSub;
    double v = k1 >= done ? srcCur : 0.0;
    for (int64_t k = std::max<int64_t>({0, done - srcCap, k0}); k <= std::min(k1, done - 1); ++k) v += srcE[static_cast<size_t>(k % srcCap)];
    return v / (static_cast<double>(k1 - k0 + 1) * Guard::kSub);
  }
  // Makes mid hold at least `need` frames.
  void produce(int64_t need, double perMid, Reader rd, void* ctx) {
    while (filled < need) {
      inAcc += perMid * kChunk;
      int k = static_cast<int>(std::floor(inAcc));
      k = std::clamp(k, 0, inCap);
      inAcc -= k;
      pull(rd, ctx, k);
      const float* ip[2] = {in[0].data(), in[1].data()};
      float* op[2] = {mid[0].data() + filled, mid[1].data() + filled};
      engine.process(ip, k, op, kChunk);
      const double s0 = midSrc, r = static_cast<double>(k) / kChunk, lead = guard.lead(midPerOut);
      guard.apply(op, channels, kChunk, [&](int i) {
        const double p = s0 + (i + lead) * r;
        return srcOver(p - guard.window(), p);
      });
      midSrc += k;
      filled += kChunk;
    }
  }
  // Drops the mid frames the reader has passed (`force`: now, else once a quarter of mid is free).
  void compact(int hw, bool force = false) {
    const auto drop = std::min(filled, static_cast<int64_t>(std::floor(pos)) - hw - 2);  // (pre-roll: pos may run past filled)
    if (drop <= 0 || (!force && drop < midCap / 4)) return;
    for (int c = 0; c < channels; ++c)
      std::copy(mid[c].begin() + static_cast<std::ptrdiff_t>(drop), mid[c].begin() + static_cast<std::ptrdiff_t>(filled), mid[c].begin());
    filled -= drop;
    pos -= static_cast<double>(drop);
  }
};

int64_t rubberBandFramesFed() { return gRbFed.load(std::memory_order_relaxed); }

Voice::Voice() : impl_(std::make_unique<Impl>()) {}
Voice::~Voice() = default;

Engine Voice::engine() const { return impl_->kind; }

void Voice::prepare(int channels, double sampleRate, double maxPitch, double maxSpeed, bool live, Engine engine) {
  Impl& m = *impl_;
  m.channels = std::clamp(channels, 1, 2);
  m.sampleRate = sampleRate;
  m.glideCoef = 1.0 - std::exp(-1.0 / (Impl::kGlideSec * sampleRate));
  m.maxPitch = std::max(1.0, maxPitch);
  m.maxSpeed = std::max(1.0, maxSpeed);
  m.kind = engine;
  if (engine == Engine::RubberBand) {
    // J-157: the engine and every buffer it fills are made here; the largest
    // chunk it is given bounds its own buffers (setMaxProcessSize).
    m.rb = std::make_unique<RubberBandStretcher>(static_cast<size_t>(std::lround(sampleRate)), static_cast<size_t>(m.channels), quietLog(),
                                                 kRbRealtime, 1.0, 1.0);
    m.rbPad = static_cast<int>(m.rb->getPreferredStartPad());
    m.inCap = std::max(1024, static_cast<int>(std::ceil(m.maxSpeed * Impl::kChunk)) + 16);
    m.rb->setMaxProcessSize(static_cast<size_t>(m.inCap));
    for (int c = 0; c < 2; ++c) {
      m.in[c].assign(static_cast<size_t>(m.inCap), 0.0f);
      m.rbDump[c].assign(Impl::kRbDump, 0.0f);
      m.mid[c].clear(), m.mid[c].shrink_to_fit();
    }
    // Its resampler sizes its buffers for each new pitch ratio the first time
    // it meets it: the pitches a voice is likely to play are met here.
    m.rbRatio = m.rbPitch = 0;
    m.rbSet(1.0, 1.0);
    m.rbFeed(nullptr, nullptr, m.inCap);
    for (double p : {1.0 / m.maxPitch, m.maxPitch, 0.5, 2.0, 1.0 / 1.5, 1.5, std::pow(2.0, -1.0 / 12), std::pow(2.0, 1.0 / 12), 1.0}) {
      m.rbSet(1.0, p);
      for (int i = 0; i < 4; ++i) m.rbFeed(nullptr, nullptr, m.inCap), m.rbSkip = int64_t{1} << 40, m.rbDrop();
    }
    m.rbSkip = 0;
    prepared_ = true;
    reset();
    return;
  }
  m.rb.reset();
  configure(m.engine, m.channels, sampleRate, live);
  m.guard.prepare(sampleRate);
  const SincTable& t = sincTable(Quality::Realtime);
  // mid: render steps at the top pitch, the resampler's reach, slack for compaction.
  const int hw = pitch::maxReach(m.maxPitch, t) + 4;
  m.midCap = 8 * (Impl::kChunk + hw) + static_cast<int>(2048 * m.maxPitch);
  // in: the engine's start-up input, or one chunk at the top input rate.
  const double perMid = m.maxSpeed * m.maxPitch;  // speed / pitch at the lowest pitch
  m.inCap = std::max(m.engine.outputSeekLength(static_cast<float>(perMid)), static_cast<int>(std::ceil(perMid * Impl::kChunk))) + 16;
  for (int c = 0; c < 2; ++c) {
    m.mid[c].assign(static_cast<size_t>(m.midCap), 0.0f);
    m.in[c].assign(static_cast<size_t>(m.inCap), 0.0f);
  }
  // The guard reads the source up to the engine's whole lead (start-up input
  // plus a chunk) and one window behind the frame being played.
  m.srcCap = (m.inCap + m.guard.window() + static_cast<int64_t>(std::ceil(perMid * (Impl::kChunk + m.guard.lead(m.maxPitch))))) / Guard::kSub + 4;
  m.srcE.assign(static_cast<size_t>(m.srcCap), 0.0);
  prepared_ = true;
  reset();
}

void Voice::reset() {
  Impl& m = *impl_;
  if (m.rb) {
    m.rb->reset();
    m.rbSkip = 0;
    m.rbPreLeft = 0;
    m.rbFed = m.rbNeed = 0;
    m.inAcc = 0;
    m.glideSet = false;
    return;
  }
  m.engine.reset();
  m.guard.reset();
  m.inRead = 0, m.srcCur = 0, m.midSrc = 0;
  m.filled = 0;
  m.pos = 0;
  m.inAcc = 0;
  m.glideSet = false;
}

int Voice::lookahead(double speed, double pitch) const {
  if (impl_->rb) {  // its pad, its latency and one chunk
    const double s = std::max(kRbMinRate, speed);
    return static_cast<int>(std::ceil(impl_->rbPad * std::max(1.0, pitch) + (impl_->rbPad + Impl::kChunk) * s)) + 2;
  }
  pitch = std::max(1e-3, pitch);
  const double perMid = speed / pitch;
  const SincTable& t = sincTable(Quality::Realtime);
  const double hw = pitch::reach(pitch, t);
  return static_cast<int>(std::ceil(impl_->engine.outputSeekLength(static_cast<float>(perMid)) + (hw + Impl::kChunk) * perMid)) + 2;
}

int Voice::prerollFor(double speed) const {
  const Impl& m = *impl_;
  if (m.rb) return 2 * m.rbPad;  // its window's lead-in (resampled ahead of it at up to an octave up)
  return static_cast<int>(std::ceil(std::clamp(speed, 0.0, m.maxSpeed) * (m.engine.blockSamples() + m.engine.intervalSamples())));
}

void Voice::start(Reader rd, void* ctx, double speed, double pitch, int preroll) {
  startLater(rd, ctx, speed, pitch, preroll);
  if (impl_->rb) impl_->rbPre(rd, ctx, std::numeric_limits<int>::max());
}

bool Voice::prime(Reader rd, void* ctx, double speed, double pitch, int budget) {
  Impl& m = *impl_;
  if (!m.rb) return true;
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  // The pre-roll, then the steps rbRender() takes before its first frame
  // (the same decisions in the same order, without taking the output).
  budget = m.rbPre(rd, ctx, std::max(1, budget));
  if (m.rbPreLeft > 0) return false;
  const double target = std::log(pitch);
  if (!m.glideSet) m.glide[0] = m.glide[1] = target, m.glideSet = true;
  m.rbSet(speed, m.rbPitch > 0 ? m.rbPitch : pitch);
  for (;;) {
    m.rbDrop();
    if (m.rbReady()) return true;
    if (budget <= 0) return false;
    budget -= std::max(1, m.rbStep(rd, ctx, speed, target));
  }
}

int Voice::primeLeft(double speed) const {
  const Impl& m = *impl_;
  if (!m.rb || m.rbReady()) return 0;
  const double s = std::max(kRbMinRate, std::clamp(speed, 0.0, m.maxSpeed));
  const int64_t need = m.rbNeed + static_cast<int64_t>(std::ceil(s * Impl::kChunk));
  return static_cast<int>(std::clamp<int64_t>(need - m.rbFed, 1, int64_t{1} << 30));
}

int64_t Voice::fedFrames() const { return impl_->rb ? impl_->rbFed : impl_->inRead; }

void Voice::startLater(Reader rd, void* ctx, double speed, double pitch, int preroll) {
  Impl& m = *impl_;
  reset();
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  if (m.rb) {
    // The engine wants `pad` input frames ahead of the start point and puts
    // the start point `delay` output frames in. The pre-roll is that lead-in
    // (silence where the reader has less); a longer pre-roll moves the start
    // point later by its extra frames at this speed. J-170: the pre-roll is
    // fed by start(), prime() or the first render().
    m.glide[0] = m.glide[1] = std::log(pitch);
    m.glideSet = true;
    m.rbSet(speed, pitch);
    const int pad = static_cast<int>(m.rb->getPreferredStartPad());
    preroll = std::max(0, preroll);
    for (int z = std::max(0, pad - preroll); z > 0; z -= m.inCap) m.rbFeed(nullptr, nullptr, std::min(z, m.inCap));
    m.rbSkip = static_cast<int64_t>(m.rb->getStartDelay()) +
               std::llround(static_cast<double>(std::max(0, preroll - pad)) / std::max(kRbMinRate, speed));
    m.rbPreLeft = preroll;
    // Its first frame comes once the engine holds two pads (its window) and
    // has made the frames to skip.
    m.rbNeed = 2 * static_cast<int64_t>(pad) + static_cast<int64_t>(std::ceil(static_cast<double>(m.rbSkip) * std::max(kRbMinRate, speed)));
    return;
  }
  m.glide[0] = m.glide[1] = std::log(pitch);
  m.glideSet = true;
  m.midPerOut = pitch;
  const double perMid = speed / pitch;  // input frames per mid frame
  const int seek = std::min(m.inCap, m.engine.outputSeekLength(static_cast<float>(std::max(1e-3, perMid))));
  m.pull(rd, ctx, seek);
  const float* ip[2] = {m.in[0].data(), m.in[1].data()};
  m.engine.outputSeek(ip, seek);
  // The stream starts `preroll` input frames before the start point: the
  // mid frames those make are rendered and passed over, chunk by chunk.
  if (preroll > 0 && perMid > 1e-6) {
    m.pos = static_cast<double>(preroll) / perMid;
    const int hw = pitch::maxReach(m.maxPitch, sincTable(Quality::Realtime)) + 4;
    while (m.filled < static_cast<int64_t>(std::floor(m.pos)) + 1) {
      m.produce(m.filled + 1, perMid, rd, ctx);
      m.compact(hw, true);
    }
  }
}

void Voice::render(float* const* out, int n, double speed, double pitch, Reader rd, void* ctx) {
  Impl& m = *impl_;
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  if (m.rb) {
    m.rbRender(out, n, speed, pitch, rd, ctx);
    return;
  }
  const SincTable& t = sincTable(Quality::Realtime);
  const double target = std::log(pitch);
  if (!m.glideSet) m.glide[0] = m.glide[1] = target, m.glideSet = true;
  // Gliding until both stages are within 1e-9 of the target (then exact).
  const bool glides = std::fabs(m.glide[0] - target) > 1e-9 || std::fabs(m.glide[1] - target) > 1e-9;
  if (!glides) m.glide[0] = m.glide[1] = target;
  const double pMax = std::exp(std::max({m.glide[0], m.glide[1], target}));
  const int hw = pitch::reach(pMax, t);
  const double a = m.glideCoef;
  // The engine's input rate follows the requested pitch (not the glide's
  // state at this call), so where a block is split cannot change it.
  const double perMid = speed / pitch;
  m.midPerOut = pitch;
  int done = 0;
  while (done < n) {
    // Output frames per step, bounded so mid never overflows.
    const int room = static_cast<int>((m.midCap / 2 - hw - Impl::kChunk) / pMax);
    const int step = std::min(n - done, std::max(1, room));
    // Mid frames this step reads (the positions advance by the pitch of each frame).
    double end = m.pos;
    if (glides) {
      double g0 = m.glide[0], g1 = m.glide[1];
      for (int j = 0; j + 1 < step; ++j) g0 += (target - g0) * a, g1 += (g0 - g1) * a, end += std::exp(g1);
    } else {
      end += pitch * (step - 1);
    }
    m.produce(static_cast<int64_t>(std::ceil(end)) + hw + 1, perMid, rd, ctx);
    double p = m.pos;
    for (int j = 0; j < step; ++j) {
      double pj = pitch;
      if (glides) {
        m.glide[0] += (target - m.glide[0]) * a, m.glide[1] += (m.glide[0] - m.glide[1]) * a;
        pj = std::exp(m.glide[1]);
      }
      if (m.channels == 2) {
        readSinc2(m.mid[0].data(), m.mid[1].data(), m.filled, p, pj, t, out[0][done + j], out[1][done + j]);
      } else {
        out[0][done + j] = readSinc(m.mid[0].data(), m.filled, p, pj, t);
      }
      p += pj;
    }
    m.pos = p;
    m.compact(hw);
    done += step;
  }
}

}  // namespace gl::stretch
