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
#include "Dsp.h"
#include "pitch/Hpss.h"
#include "pitch/Level.h"
#include "pitch/Psola.h"
#include "pitch/TdStretch.h"

namespace gl::stretch {

namespace {
constexpr double kPi = 3.14159265358979323846;
using Spectral = signalsmith::stretch::SignalsmithStretch<float>;
constexpr long kSeed = 0x6c696e65;  // fixed: renders are reproducible

// J-140: the time-domain modes. Beats runs the WSOLA engine
// (engine/pitch/TdStretch); Voice runs pitch-synchronous overlap-add
// (engine/pitch/Psola), which also keeps the formants in place.
// Modes that keep the formants in place (and the loudness with them).
bool keepsFormants(Mode m) { return m == Mode::Formant || m == Mode::Voice; }

// The stretch core of a mode behind the streaming calls the engines share
// (seek, process, flush).
class Engine {
 public:
  enum Kind { kSpectral, kWsola, kPsola };
  explicit Engine(long seed) : ss(seed) {}
  Spectral ss;
  pitch::TdStretch td;
  pitch::Psola ps;
  Kind kind = kSpectral;
  // Formant mode (J-144): the voice's pitch, tracked alongside, tells the
  // spectral engine how far apart the harmonics are when it measures the
  // formants (its own guess often lands on a harmonic or an octave off, and
  // the output then keeps the source's harmonic comb).
  pitch::VoiceTrack f0;
  bool trackF0 = false;
  int nch = 1;
  template <class In>
  void feedF0(const In& in, int n) {
    if (!trackF0) return;
    for (int i = 0; i < n; ++i) f0.push(nch == 2 ? 0.5f * (in[0][i] + in[1][i]) : in[0][i]);
    const double p = f0.recentPeriod(static_cast<int>(0.1 * f0.sampleRate()));
    ss.setFormantBase(p > 0 ? static_cast<float>(1.0 / p) : 0.0f);
  }
  bool useTd() const { return kind != kSpectral; }
  template <class F>
  decltype(auto) with(F&& f) {
    switch (kind) {
      case kWsola: return f(td);
      case kPsola: return f(ps);
      default: return f(ss);
    }
  }
  void reset() {
    with([](auto& e) { e.reset(); });
    if (trackF0) f0.reset();
  }
  int outputSeekLength(float r) {
    return with([r](auto& e) { return static_cast<int>(e.outputSeekLength(r)); });
  }
  int inputLatency() {
    return with([](auto& e) { return static_cast<int>(e.inputLatency()); });
  }
  int outputLatency() {
    return with([](auto& e) { return static_cast<int>(e.outputLatency()); });
  }
  int blockSamples() {
    return with([](auto& e) { return static_cast<int>(e.blockSamples()); });
  }
  template <class In>
  void outputSeek(In&& in, int len) {
    if (trackF0) f0.reset();
    feedF0(in, len);
    with([&](auto& e) { e.outputSeek(in, len); });
  }
  template <class In, class Out>
  void process(In&& in, int inN, Out&& out, int outN) {
    feedF0(in, inN);
    with([&](auto& e) { e.process(in, inN, out, outN); });
  }
  template <class Out>
  void flush(Out&& out, int outN, float rate) {
    with([&](auto& e) { e.flush(out, outN, rate); });
  }
};

struct Config {
  double block, interval;  // seconds
};
// Spectral block / hop per mode. J-145: realtime voices use the offline hop
// too: a hop above about a sixth of the block (17.5 ms on 70 ms, 30 ms on
// 120 ms) left an audible level ripple on held partials near ratio 2, and the
// cheaper hop saved only about 20 % CPU. Transient's own config serves only
// its whole-sound path (loops, reversed play); the hybrid's harmonic part runs
// the default config.
Config configOf(Mode m, bool offline) {
  switch (m) {
    case Mode::Mono: return Config{0.07, 0.0125};
    case Mode::Transient: return offline ? Config{0.08, 0.016} : Config{0.05, 0.0125};
    default: return Config{0.12, 0.02};
  }
}

// J-145: the onset modes (Auto, Transient, Beats) split the sound into its
// harmonic and percussive parts (pitch/Hpss.h); each part has its own engine.
enum class Part { Whole, Harmonic, Percussive };

void setup(Engine& e, int nch, double sr, Mode m, bool offline, bool light = false, Part part = Part::Whole) {
  e.trackF0 = false;
  if (m == Mode::Voice) {
    e.kind = Engine::kPsola;
    e.ps.configure(nch, sr);
    return;
  }
  if (part == Part::Percussive) {  // hits: copied by a time-domain engine restarted at each one
    e.kind = Engine::kWsola;
    e.td.configure(nch, sr, pitch::TdStretch::percussive());
    return;
  }
  if (m == Mode::Beats || light) {
    e.kind = Engine::kWsola;
    // The harmonic part of Beats (and every light voice) flows through one continuous engine.
    e.td.configure(nch, sr, part == Part::Harmonic || !(m == Mode::Beats || m == Mode::Transient) ? pitch::TdStretch::general() : pitch::TdStretch::beats());
    return;
  }
  e.kind = Engine::kSpectral;
  e.nch = nch;
  e.trackF0 = m == Mode::Formant;
  if (e.trackF0) e.f0.configure(sr, 0);
  // The harmonic part has no hits left to keep short: the long blocks resolve low notes best.
  const Config c = configOf(part == Part::Harmonic ? Mode::Auto : m, offline);
  e.ss.configure(nch, std::max(64, static_cast<int>(sr * c.block)), std::max(16, static_cast<int>(sr * c.interval)));
  // J-145: a new note in the harmonic part starts in phase with the input,
  // so it adds up with the percussive part's copy of its attack.
  e.ss.onsetPhaseReset = part == Part::Harmonic ? 4.0f : 0.0f;  // +6 dB from one block to the next
}

// Pitch is never left to the engine's own transposition (its partials can
// land a few cents off, more on low notes): the engine only stretches time,
// and the pitch comes from resampling its output (exact). In Formant mode the
// engine moves the formants by the inverse of that resampling first, so they
// end where they started (plus the formant control).
void applyPitch(Engine& e, Mode m, double pitch, double formantSemis) {
  if (e.kind == Engine::kPsola) {
    // Voice changes the pitch itself (grains placed at the new period):
    // nothing is resampled after it.
    e.ps.setShift(pitch, std::pow(2.0, formantSemis / 12.0));
    return;
  }
  if (e.kind == Engine::kWsola) return;
  e.ss.setTransposeFactor(1.0f);
  if (m == Mode::Formant) {
    e.ss.setFormantFactor(static_cast<float>(std::pow(2.0, formantSemis / 12.0) / pitch), false);
  } else {
    e.ss.setFormantFactor(1.0f, false);
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

// Progress of an offline render: every output frame below `done` is final.
using Progress = std::function<void(int64_t done)>;

// J-145: post-processing of the engine output as it becomes final, in order:
// post(y, a, b) may change y[c][a, b).
using Post = std::function<void(float* const* y, int64_t a, int64_t b)>;
void stretchOnly(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                 const Progress* pr = nullptr, Part part = Part::Whole, const Post* post = nullptr);
void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen);

// Plain spectral stretch of a whole buffer (exact length, aligned): time
// stretch to outLen · pitch frames, then resampling by the pitch ratio.
// J-145: `part` picks the engine for one part of a split sound; `post` runs
// on the stretch's own frames, before the pitch resampling (outLen frames, or
// llround(outLen * pitch)).
void plain(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
           const Progress* pr = nullptr, Part part = Part::Whole, const Post* post = nullptr) {
  if (outLen <= 0) return;
  const bool level = keepsFormants(s.mode);  // J-140: formant shifts keep the loudness
  if (std::fabs(s.pitch - 1.0) < 1e-12 || s.mode == Mode::Voice) {
    stretchOnly(in, nch, n, out, outLen, sr, s, level ? nullptr : pr, part, post);
    if (level) {
      pitch::matchLevel(out, nch, outLen, in, n, [&](int64_t q) { return static_cast<double>(q) * static_cast<double>(n) / static_cast<double>(outLen); }, sr);
      if (pr && *pr) (*pr)(outLen);
    }
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
  const double reach = pitch::reach(s.pitch, t) + 1;
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
    if (pr && *pr && !level) (*pr)(doneOut);
  };
  const Progress midProgress = [&](int64_t midDone) {
    if (midDone >= midUsed) return;  // the end: below, with the silence after it
    resampleTo(static_cast<int64_t>(std::floor((static_cast<double>(midDone) - reach) / s.pitch)));
  };
  stretchOnly(in, nch, n, mp.data(), std::max<int64_t>(1, midUsed), sr, s, pr && !level ? &midProgress : nullptr, part, post);
  resampleTo(outLen);
  if (level) {
    // On the band-limited result: what the resampler removed must not count.
    pitch::matchLevel(out, nch, outLen, in, n, [&](int64_t q) { return static_cast<double>(q) * static_cast<double>(n) / static_cast<double>(outLen); }, sr);
    if (pr && *pr) (*pr)(outLen);
  }
}

void stretchOnly(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
                 const Progress* pr, Part part, const Post* post) {
  if (outLen <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + outLen, 0.0f);
  int64_t added = 0;
  auto addTo = [&](int64_t upTo) {  // runs `post` on [added, upTo)
    if (!post || !*post || upTo <= added) return;
    (*post)(out, added, upTo);
    added = upTo;
  };
  if (n <= 0) {
    addTo(outLen);
    return;
  }
  Engine e(kSeed);
  setup(e, nch, sr, s.mode, true, false, part);
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
    if (out2 == outLen) {
      addTo(o);
      if (pr && *pr) (*pr)(o);
    }
  }
  OutView pout2{yp.data(), outputIndex};
  e.flush(pout2, static_cast<int>(out2 - outputIndex), static_cast<float>(r2));
  if (out2 != outLen)
    for (int c = 0; c < nch; ++c) std::copy(yp[static_cast<size_t>(c)], yp[static_cast<size_t>(c)] + outLen, out[c]);
  addTo(outLen);
  if (pr && *pr) (*pr)(outLen);
}

// J-145: the onsets the hybrid stretch restarts at: 40 ms apart at least, and from the ends.
std::vector<int64_t> hitsOf(const std::vector<int64_t>& on, int64_t n, double sr) {
  std::vector<int64_t> r;
  const auto minGap = static_cast<int64_t>(0.04 * sr);
  int64_t last = 0;
  for (int64_t o : on)
    if (o > minGap && o < n - minGap && o - last >= minGap) r.push_back(o), last = o;
  return r;
}

// J-145: pre-echo gate of the hybrid stretch. The continuous engine smears
// whatever of a hit stayed in the harmonic part ahead of the hit (a spectral
// block lasts 0.12 s). In the last `window` frames before each hit the output
// may not be louder than the input was there (RMS over 20 ms or more, with
// 2 dB to spare, the input measured up to the hit only): what is louder is
// the smear, and is turned down. Sustained sounds (a bass under the drums)
// pass as they are. Values under 1e-25 come out as 0 (no subnormals).
// Realtime safe.
class PreGate {
 public:
  void configure(double sr) {
    sr_ = sr > 0 ? sr : 44100.0;
    window_ = 0.07 * sr_;
    ramp_ = 0.002 * sr_;
    avg_ = std::clamp(static_cast<int>(std::lround(0.02 * sr_ / kSub)), 1, kRing);
    reset();
  }
  void reset() {
    g_ = 1;
    head_ = 0;
    std::fill(std::begin(e_), std::end(e_), 0.0);
    std::fill(std::begin(len_), std::end(len_), 0);
  }
  // Input frames the input RMS spans for an output (mid) span of `frames` at perMid.
  int64_t inputSpan(double perMid) const {
    return std::clamp<int64_t>(static_cast<int64_t>(std::ceil(std::max(0.02 * sr_, perMid * avg_ * kSub))), 1, 8192);
  }
  // y[c][0, m): the next output frames. untilHit: output frames from y[.][0]
  // to the next hit (< 0: none known). inRms(k): the input RMS for output
  // frame k (over inputSpan frames up to its input position, not past the hit).
  // stretch: output frames per input frame. The engines reach ahead a fixed
  // span of input (grain, block), which lands that much earlier the slower the
  // playback: the window before a hit grows with the stretch.
  template <class F>
  void apply(float* const* y, int nch, int m, double untilHit, double stretch, F&& inRms) {
    const double window = window_ * std::clamp(stretch, 1.0, 16.0);
    for (int s = 0; s < m;) {
      int len = std::min(kSub, m - s);
      const bool before = untilHit >= 0 && static_cast<double>(s) < untilHit;
      if (before) len = std::max(1, std::min(len, static_cast<int>(std::ceil(untilHit - s))));
      double e = 0;
      for (int c = 0; c < nch; ++c)
        for (int j = s; j < s + len; ++j) e += static_cast<double>(y[c][j]) * y[c][j];
      e_[head_] = e / nch, len_[head_] = len;
      head_ = (head_ + 1) % kRing;
      double target = 1;
      const double d = untilHit - (s + len);  // frames from this piece's end to the hit
      if (before && d < window) {
        double ey = 0;
        int ny = 0;
        for (int k = 1; k <= avg_; ++k) {
          const int i = (head_ - k + kRing) % kRing;
          ey += e_[i], ny += len_[i];
        }
        const double ry = std::sqrt(ey / std::max(1, ny));
        if (ry > 1e-6) target = std::min(1.0, 1.26 * inRms(s + len) / ry);
        if (d < ramp_) target += (1 - target) * (1 - std::max(0.0, d) / ramp_);  // back to 1 at the hit
      }
      for (int j = 0; j < len; ++j) {
        const double g = g_ + (target - g_) * (j + 1) / len;
        for (int c = 0; c < nch; ++c) {
          const auto v = static_cast<float>(y[c][s + j] * g);
          y[c][s + j] = std::fabs(v) < 1e-25f ? 0.0f : v;  // and no subnormal values
        }
      }
      g_ = target;
      s += len;
    }
  }

 private:
  static constexpr int kSub = 32, kRing = 128;
  double sr_ = 44100, window_ = 0, ramp_ = 1, g_ = 1;
  int avg_ = 1, head_ = 0;
  double e_[kRing] = {};
  int len_[kRing] = {};
};

// The time map of a hybrid stretch: x(j) is the input position of output
// frame j (nondecreasing), outAt(i) the output frame of input frame i.
struct TimeMap {
  std::function<double(int64_t)> x;
  std::function<int64_t(int64_t)> outAt;
};

// J-145: the stretch around each hit. Both parts play each hit at the
// original speed from `lead` frames before it to `att` frames after it (the
// rest of the segment takes up the difference): the percussive engine copies
// the attack as it is, and the harmonic engine, at the same speed and the
// same place, takes over the note from it without a gap or an echo (with the
// parts at different places, a plucked note faded out of one before it was
// in the other: a dip of 10 dB or more 50 ms into the note). The attack span
// covers the separation's handover (pitch/Hpss.h: the analysis window and
// the ramp after a hit). Per segment k (starting at b[k]; the first starts at
// 0 and has neither).
struct HitPlan {
  std::vector<int64_t> lead, att;
};
constexpr double kLeadSec = 0.012, kAttackSec = 0.1;
HitPlan hitPlan(const std::vector<int64_t>& b, const std::function<int64_t(int64_t)>& outAt, double sr) {
  HitPlan p;
  const size_t segs = b.size() > 1 ? b.size() - 1 : 0;
  p.lead.assign(segs, 0), p.att.assign(segs, 0);
  const auto pre = static_cast<int64_t>(kLeadSec * sr), att = static_cast<int64_t>(kAttackSec * sr);
  // Frames of segment k (in I, out O) that may run at the original speed: the
  // rest then runs at most twice as fast or as slow as the segment's own rate,
  // (I - S) / (O - S) within a factor 2 of I / O: S <= I O / (2 max - min).
  std::vector<int64_t> budget(segs, 0);
  for (size_t k = 0; k < segs; ++k) {
    const auto I = static_cast<double>(b[k + 1] - b[k]), O = static_cast<double>(outAt(b[k + 1]) - outAt(b[k]));
    const double den = 2 * std::max(I, O) - std::min(I, O);
    budget[k] = I > 0 && O > 0 && den > 0 ? static_cast<int64_t>(std::floor(I * O / den)) : 0;
  }
  for (size_t k = 1; k < segs; ++k) {
    const int64_t i0 = b[k] - b[k - 1], o0 = outAt(b[k]) - outAt(b[k - 1]);
    const int64_t i1 = b[k + 1] - b[k], o1 = outAt(b[k + 1]) - outAt(b[k]);
    // The lead: at most a quarter of the segment before the hit (in, out, and its budget).
    p.lead[k] = std::max<int64_t>(0, std::min({pre, i0 / 4, o0 / 4, budget[k - 1] / 4}));
    p.att[k] = std::max<int64_t>(0, std::min({att, i1 / 2, o1 / 2, budget[k]}));
  }
  // The attack and the next hit's lead share the segment's budget.
  for (size_t k = 1; k + 1 < segs; ++k) p.att[k] = std::max<int64_t>(0, std::min(p.att[k], budget[k] - p.lead[k + 1]));
  return p;
}

// The hybrid's map: `base` with each hit's span at the original speed
// (HitPlan), the frames between two spans following `base` with a linear
// correction. The speed changes are smoothed over kSmoothSec (an abrupt one
// makes the spectral engine's phase prediction jump: a dip in a bass line
// under the hit).
constexpr double kSmoothSec = 0.04;
constexpr double kSmoothRtSec = 0.02;  // the realtime map: the same smoothing (a triangle of half this width)
TimeMap elasticMap(const TimeMap& base, const std::vector<int64_t>& b, const HitPlan& plan, int64_t midLen, double sr) {
  struct Knot {
    double j, x, d;      // output frame, input position, x - base.x(j)
    bool rigid = false;  // the span to the next knot runs at the original speed
  };
  std::vector<Knot> knots;
  auto add = [&](int64_t j, double x, bool rigid) {
    if (!knots.empty() && static_cast<double>(j) <= knots.back().j) return;
    knots.push_back({static_cast<double>(j), x, x - base.x(j), rigid});
  };
  add(0, base.x(0), false);
  for (size_t k = 1; k + 1 < b.size(); ++k) {
    const int64_t l = plan.lead[k], a = plan.att[k];
    if (l + a <= 0) continue;
    const int64_t o = base.outAt(b[k]);
    add(o - l, static_cast<double>(b[k] - l), true);
    add(o + a, static_cast<double>(b[k] + a), false);
  }
  add(midLen, base.x(midLen), false);
  // The correction d(j) = x(j) - base.x(j) on a grid of kGrid frames, smoothed
  // (two moving averages: a triangle kSmoothSec wide).
  constexpr int64_t kGrid = 64;
  const int64_t nq = midLen / kGrid + 2;
  auto d = std::make_shared<std::vector<double>>(static_cast<size_t>(nq), 0.0);
  size_t ki = 0;
  for (int64_t q = 0; q < nq; ++q) {
    const auto j = static_cast<double>(q * kGrid);
    while (ki + 1 < knots.size() && knots[ki + 1].j <= j) ++ki;
    const Knot& k0 = knots[ki];
    double v = k0.d;
    if (ki + 1 < knots.size()) {
      const Knot& k1 = knots[ki + 1];
      v = k0.rigid ? k0.x + (j - k0.j) - base.x(q * kGrid) : k0.d + (k1.d - k0.d) * (j - k0.j) / (k1.j - k0.j);
    }
    (*d)[static_cast<size_t>(q)] = v;
  }
  const auto half = static_cast<int64_t>(std::llround(kSmoothSec * sr / (4 * kGrid)));
  if (half > 0) {
    // The ends stay put (the first output frame reads the first input frame,
    // the last the last: a clip that starts on a hit starts on time): the
    // correction is extended past each end by its odd reflection there, which
    // a symmetric average leaves at the end's own value.
    const int64_t pad = 2 * half, ne = nq + 2 * pad;
    std::vector<double> e(static_cast<size_t>(ne)), t(static_cast<size_t>(ne));
    const auto at = [&](int64_t q) { return (*d)[static_cast<size_t>(std::clamp<int64_t>(q, 0, nq - 1))]; };
    for (int64_t q = 0; q < ne; ++q) {
      const int64_t s = q - pad;
      e[static_cast<size_t>(q)] = s < 0 ? 2 * at(0) - at(-s) : s >= nq ? 2 * at(nq - 1) - at(2 * (nq - 1) - s) : at(s);
    }
    for (int pass = 0; pass < 2; ++pass) {
      double acc = 0;
      int64_t cnt = 0;
      // Moving average over [q - half, q + half] (clamped at the extended ends).
      for (int64_t q = -half; q < ne + half; ++q) {
        const int64_t add2 = q + half, sub = q - half - 1;
        if (add2 >= 0 && add2 < ne) acc += e[static_cast<size_t>(add2)], ++cnt;
        if (sub >= 0 && sub < ne) acc -= e[static_cast<size_t>(sub)], --cnt;
        if (q >= 0 && q < ne) t[static_cast<size_t>(q)] = cnt > 0 ? acc / static_cast<double>(cnt) : 0.0;
      }
      e.swap(t);
    }
    for (int64_t q = 0; q < nq; ++q) (*d)[static_cast<size_t>(q)] = e[static_cast<size_t>(q + pad)];
  }
  TimeMap m;
  const auto bx = base.x;
  m.x = [d, bx, nq](int64_t j) {
    const double f = static_cast<double>(j) / static_cast<double>(kGrid);
    const auto q = std::clamp<int64_t>(static_cast<int64_t>(std::floor(f)), 0, nq - 2);
    const double w = std::clamp(f - static_cast<double>(q), 0.0, 1.0);
    return bx(j) + (*d)[static_cast<size_t>(q)] * (1 - w) + (*d)[static_cast<size_t>(q) + 1] * w;
  };
  const auto mx = m.x;
  m.outAt = [mx, midLen](int64_t i) {
    // The first output frame whose input position reaches i.
    const auto di = static_cast<double>(i);
    int64_t lo = 0, hi = midLen;
    while (lo < hi) {
      const int64_t mid = lo + (hi - lo) / 2;
      if (mx(mid) < di)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  };
  return m;
}

// J-145: the percussive part of a split sound, stretched hit by hit. Each
// segment (from one hit to the next) runs a fresh WSOLA engine started on its
// hit, so every hit comes out once, at full level, where the time map puts it;
// it plays the hit's span at the original speed (HitPlan, as the map does),
// the rest of the segment takes up the difference. The separation spreads each
// hit a little ahead of its onset (the analysis window), so a segment starts
// `lead` frames early and fades in there, while the previous one (which never
// reads that far: TdStretch::setInputEnd) fades out. y: midLen frames.
void percussivePart(const float* const* p, int nch, int64_t n, const std::vector<int64_t>& b, int64_t midLen, double sr, const TimeMap& map,
                    const HitPlan& plan, float* const* y) {
  for (int c = 0; c < nch; ++c) std::fill(y[c], y[c] + midLen, 0.0f);
  if (n <= 0 || midLen <= 0 || b.size() < 2) return;
  const auto outAt = [&](int64_t i) { return std::clamp<int64_t>(map.outAt(i), 0, midLen); };
  pitch::TdStretch td;
  td.configure(nch, sr, pitch::TdStretch::percussive());
  const int seek = td.outputSeekLength(1.0f);
  std::vector<std::vector<float>> seg(static_cast<size_t>(nch));
  std::vector<float*> sp(static_cast<size_t>(nch));
  for (size_t k = 0; k + 1 < b.size(); ++k) {
    const bool last = k + 2 == b.size();
    const int64_t lead = plan.lead[k], nextLead = last ? 0 : plan.lead[k + 1];
    const int64_t a = b[k] - lead, e = last ? n : b[k + 1] - nextLead;
    const int64_t oa = outAt(b[k]) - lead, oe = last ? midLen : outAt(b[k + 1]) - nextLead;
    const int64_t inLen = e - a, outLen = oe - oa;
    if (outLen <= 0 || inLen <= 0) continue;
    const int64_t tail = last ? 0 : std::min(nextLead, midLen - oe);
    for (int c = 0; c < nch; ++c) {
      seg[static_cast<size_t>(c)].assign(static_cast<size_t>(outLen + tail), 0.0f);
      sp[static_cast<size_t>(c)] = seg[static_cast<size_t>(c)].data();
    }
    td.outputSeek(PaddedIn{p, n, a}, seek);
    if (!last) td.setInputEnd(inLen);
    // Along the map (as the harmonic part), in steps of kStep output frames.
    // The segment starts at a: the input is fed relative to the map there.
    constexpr int64_t kStep = 256;
    const double x0 = map.x(oa);
    int64_t inPos = 0;
    for (int64_t o = 0; o < outLen + tail;) {
      const int64_t o2 = std::min(outLen + tail, o + kStep);
      const auto i2 = std::max(inPos, static_cast<int64_t>(std::llround(map.x(oa + o2) - x0)));
      td.process(PaddedIn{p, n, a + seek + inPos}, static_cast<int>(i2 - inPos), OutView{sp.data(), o}, static_cast<int>(o2 - o));
      inPos = i2, o = o2;
    }
    for (int c = 0; c < nch; ++c) {
      const float* v = sp[static_cast<size_t>(c)];
      float* o = y[c] + oa;
      for (int64_t j = 0; j < outLen; ++j) {
        const double g = j < lead ? 0.5 - 0.5 * std::cos(kPi * static_cast<double>(j + 1) / static_cast<double>(lead + 1)) : 1.0;
        o[j] += static_cast<float>(g * v[j]);
      }
      for (int64_t j = 0; j < tail; ++j) {
        const double g = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(j + 1) / static_cast<double>(tail + 1));
        o[outLen + j] += static_cast<float>(g * v[outLen + j]);
      }
    }
  }
}

// Segment starts for the hybrid: 0, the hits, n.
std::vector<int64_t> segmentStarts(const float* const* in, int nch, int64_t n, double sr, double sensitivity) {
  std::vector<float> mono(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    float v = 0;
    for (int c = 0; c < nch; ++c) v += in[c][i];
    mono[static_cast<size_t>(i)] = v / static_cast<float>(nch);
  }
  std::vector<int64_t> b{0};
  for (int64_t o : hitsOf(detectOnsets(mono.data(), n, sr, sensitivity), n, sr)) b.push_back(o);
  b.push_back(n);
  return b;
}

// J-145: hybrid stretch for the onset modes (replaces J-87's segment-wise
// stretch, which restarted the whole sound at every hit: bass and pads dipped
// or broke off at each one, and the attack copied from the original was
// crossfaded into stretched audio that no longer lined up with it). The sound
// is split into its harmonic and percussive parts (pitch/Hpss.h, what is new
// at a hit counting as percussive); the harmonic part goes through one
// continuous engine, the percussive part hit by hit (percussivePart), and a
// gate keeps the continuous engine's smear from sounding ahead of the hits
// (PreGate). Works in the stretch's own frames (before any pitch resampling):
// midLen frames on `map`. renderH(h, post) renders the harmonic part h on the
// same map with `post` applied to its frames as they become final.
using HarmonicRender = std::function<void(const float* const* h, const TimeMap& map, const Post& post)>;

void hybridCore(const float* const* in, int nch, int64_t n, int64_t midLen, double sr, double sensitivity, const TimeMap& baseMap,
                const HarmonicRender& renderH) {
  std::vector<std::vector<float>> hv(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(n)));
  std::vector<float*> h(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) h[static_cast<size_t>(c)] = hv[static_cast<size_t>(c)].data();
  const std::vector<int64_t> b = segmentStarts(in, nch, n, sr, sensitivity);
  const std::vector<int64_t> hits(b.begin() + 1, b.end() - 1);
  const HitPlan plan = hitPlan(b, [&](int64_t i) { return std::clamp<int64_t>(baseMap.outAt(i), 0, midLen); }, sr);
  const TimeMap map = elasticMap(baseMap, b, plan, midLen, sr);
  pitch::HpssSettings hs;
  hs.onsets = &hits;
  pitch::harmonicPart(in, nch, n, sr, h.data(), hs);
  // The percussive part: x - h.
  std::vector<std::vector<float>> mv(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(midLen)));
  std::vector<float*> mp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = mv[static_cast<size_t>(c)].data();
  {
    std::vector<std::vector<float>> pv(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(n)));
    std::vector<const float*> pp(static_cast<size_t>(nch));
    for (int c = 0; c < nch; ++c) {
      float* q = pv[static_cast<size_t>(c)].data();
      for (int64_t i = 0; i < n; ++i) {
        const float v = in[c][i] - h[static_cast<size_t>(c)][i];
        q[i] = std::fabs(v) < 1e-25f ? 0.0f : v;  // no subnormal values
      }
      pp[static_cast<size_t>(c)] = q;
    }
    percussivePart(pp.data(), nch, n, b, midLen, sr, map, plan, mp.data());
  }
  // The pre-echo gate measures the input (channel mean of the energy, prefix sums).
  std::vector<double> cum(static_cast<size_t>(n) + 1, 0.0);
  for (int64_t i = 0; i < n; ++i) {
    double e = 0;
    for (int c = 0; c < nch; ++c) e += static_cast<double>(in[c][i]) * in[c][i];
    cum[static_cast<size_t>(i) + 1] = cum[static_cast<size_t>(i)] + e / nch;
  }
  std::vector<int64_t> hitAt(b.size());
  for (size_t k = 0; k < b.size(); ++k) hitAt[k] = std::clamp<int64_t>(map.outAt(b[k]), 0, midLen);
  PreGate gate;
  gate.configure(sr);
  std::vector<float*> yp(static_cast<size_t>(nch));
  size_t next = 1;  // b[next]: the next hit (b[0] = 0 and b.back() = n are no hits)
  const Post post = [&](float* const* y, int64_t a, int64_t e) {
    for (int c = 0; c < nch; ++c)
      for (int64_t j = a; j < e; ++j) y[c][j] += mp[static_cast<size_t>(c)][j];
    while (a < e) {
      while (next + 1 < b.size() && hitAt[next] <= a) ++next;
      const bool hit = next + 1 < b.size();
      const int64_t at = hit ? hitAt[next] : e;
      const int64_t to = std::min(e, std::max(a + 1, at));
      const double onsetIn = hit ? static_cast<double>(b[next]) : static_cast<double>(n);
      for (int c = 0; c < nch; ++c) yp[static_cast<size_t>(c)] = y[c] + a;
      const int64_t a0 = a;
      const double perOut = std::max(1e-3, map.x(a + 1) - map.x(a));
      const int64_t span = gate.inputSpan(perOut);
      gate.apply(yp.data(), nch, static_cast<int>(to - a), hit ? static_cast<double>(at - a) : -1.0, 1.0 / perOut, [&](int k) {
        const double x = std::min(map.x(a0 + k), onsetIn);
        const auto i1 = std::clamp<int64_t>(static_cast<int64_t>(std::llround(x)), 0, n);
        const int64_t i0 = std::max<int64_t>(0, i1 - span);
        return i1 > i0 ? std::sqrt((cum[static_cast<size_t>(i1)] - cum[static_cast<size_t>(i0)]) / static_cast<double>(i1 - i0)) : 0.0;
      });
      a = to;
    }
  };
  std::vector<const float*> hc(h.begin(), h.end());
  renderH(hc.data(), map, post);
}

void mappedCore(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s, double pitch,
                const std::function<double(int64_t)>& srcAt, Part part = Part::Whole, const Post* post = nullptr, const Progress* pr = nullptr);

// J-145: a variable-rate stretch to midLen frames on `x` (stretch frames),
// then resampling by s.pitch to outLen frames, as plain() does at a constant
// rate. `post` runs on the stretch's frames, `pr` reports the output.
void mappedRender(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, int64_t midLen, double sr,
                  const Settings& s, const std::function<double(int64_t)>& x, Part part, const Post* post, const Progress* pr) {
  if (outLen <= 0) return;
  if (std::fabs(s.pitch - 1.0) < 1e-12) {
    mappedCore(in, nch, n, out, outLen, sr, s, 1.0, x, part, post, pr);
    return;
  }
  const SincTable& t = sincTable(Quality::Offline);
  const auto mid = std::max(midLen, static_cast<int64_t>(std::ceil(static_cast<double>(outLen) * s.pitch))) + t.zeroCrossings + 2;
  std::vector<std::vector<float>> m(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(mid), 0.0f));
  std::vector<float*> mp(static_cast<size_t>(nch));
  for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = m[static_cast<size_t>(c)].data();
  int64_t doneOut = 0;
  const double reach = pitch::reach(s.pitch, t) + 1;
  auto resampleTo = [&](int64_t upTo) {
    upTo = std::clamp<int64_t>(upTo, doneOut, outLen);
    if (upTo <= doneOut) return;
    for (int64_t j = doneOut; j < upTo; ++j) {
      const double q = static_cast<double>(j) * s.pitch;
      if (nch == 2)
        readSinc2(mp[0], mp[1], mid, q, s.pitch, t, out[0][j], out[1][j]);
      else
        for (int c = 0; c < nch; ++c) out[c][j] = readSinc(mp[static_cast<size_t>(c)], mid, q, s.pitch, t);
    }
    doneOut = upTo;
    if (pr && *pr) (*pr)(doneOut);
  };
  const Progress midProgress = [&](int64_t midDone) {
    if (midDone >= midLen) return;  // the end: below, with the silence after it
    resampleTo(static_cast<int64_t>(std::floor((static_cast<double>(midDone) - reach) / s.pitch)));
  };
  mappedCore(in, nch, n, mp.data(), std::max<int64_t>(1, midLen), sr, s, s.pitch, x, part, post, pr ? &midProgress : nullptr);
  resampleTo(outLen);
}

// The hybrid at a constant rate (exact length), the pitch by resampling.
void hybrid(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s,
            double sensitivity, const Progress* pr = nullptr) {
  const bool pitched = std::fabs(s.pitch - 1.0) >= 1e-12;
  const int64_t midLen = pitched ? std::max<int64_t>(1, std::llround(static_cast<double>(outLen) * s.pitch)) : outLen;
  const double ratio = static_cast<double>(midLen) / static_cast<double>(n);
  TimeMap map;
  map.x = [=](int64_t j) { return static_cast<double>(j) / ratio; };
  map.outAt = [=](int64_t i) { return static_cast<int64_t>(std::llround(static_cast<double>(i) * ratio)); };
  hybridCore(in, nch, n, midLen, sr, sensitivity, map, [&](const float* const* h, const TimeMap& em, const Post& post) {
    mappedRender(h, nch, n, out, outLen, midLen, sr, s, em.x, Part::Harmonic, &post, pr);
  });
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
    case Mode::Voice: return "Voice";
    case Mode::Beats: return "Beats";
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
    case Mode::Voice: return "Vocals, speech and solo instruments: natural pitch shifts with the formants kept, no phasing";
    case Mode::Beats: return "Drums and loops: every hit copied as it is, nothing smeared or doubled";
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
      return Mode::Mono;
    case 9:   // e2 speech
      return Mode::Voice;
    case 3:   // slice stretch
    case 4:   // slice map
      return Mode::Beats;
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
    case Mode::Transient: return 7;  // e2 transient
    case Mode::Formant: return -2;   // stretch pro
    case Mode::Voice: return 9;      // e2 speech
    case Mode::Beats: return 3;      // slice stretch
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

SamplePtr harmonicOf(const SamplePtr& s, double sensitivity) {
  static std::mutex mu;
  static std::map<std::pair<const Sample*, int>, std::pair<std::weak_ptr<const Sample>, SamplePtr>> cache;
  if (!s || s->frames() == 0 || s->seconds() > kHarmonicMaxSeconds) return nullptr;
  const std::pair<const Sample*, int> key{s.get(), static_cast<int>(std::lround(sensitivity * 1000))};
  {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
    if (auto it = cache.find(key); it != cache.end()) return it->second.second;
  }
  const std::vector<int64_t> hits = hitsOf(*onsetsOf(s, sensitivity), static_cast<int64_t>(s->frames()), s->sampleRate);
  pitch::HpssSettings hs;
  hs.onsets = &hits;
  auto h = std::make_shared<Sample>();
  h->path = s->path;
  h->sampleRate = s->sampleRate;
  const auto n = static_cast<int64_t>(s->frames());
  h->left.assign(s->frames(), 0.0f);
  if (s->mono()) {
    const float* x[1] = {s->left.data()};
    float* y[1] = {h->left.data()};
    pitch::harmonicPart(x, 1, n, s->sampleRate, y, hs);
    h->setMono();
  } else {
    h->right.assign(s->frames(), 0.0f);
    const float* x[2] = {s->left.data(), s->right.data()};
    float* y[2] = {h->left.data(), h->right.data()};
    pitch::harmonicPart(x, 2, n, s->sampleRate, y, hs);
  }
  SamplePtr r = h;
  std::lock_guard<std::mutex> lk(mu);
  cache[key] = {s, r};
  return r;
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
  if (sameTime && std::fabs(t.pitch - 1.0) < 1e-9 && (!keepsFormants(t.mode) || std::fabs(t.formant) < 1e-9)) {
    for (int c = 0; c < nch; ++c) std::copy(in[c], in[c] + n, out[c]);
    return;
  }
  switch (t.mode) {
    case Mode::Transient:
    case Mode::Beats:
    case Mode::Auto: hybrid(in, nch, n, out, outLen, sr, t, onsetSensitivity(t.mode), pr); break;
    default: plain(in, nch, n, out, outLen, sr, t, pr); break;
  }
}
}  // namespace

namespace {
// Variable-rate stretch (engine time only; `pitch` sets the formant compensation).
// J-145: `post` (optional) runs on each block as it is written.
void mappedCore(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s, double pitch,
                const std::function<double(int64_t)>& srcAt, Part part, const Post* post, const Progress* pr) {
  Engine e(kSeed);
  setup(e, nch, sr, s.mode, true, false, part);
  applyPitch(e, s.mode, pitch, s.formant);
  const auto blk = std::max<int64_t>(16, static_cast<int64_t>(0.01 * sr));
  const double p0 = srcAt(0);
  const auto base = static_cast<int64_t>(std::floor(p0));
  // The start-up input runs to where the map is one output latency in (the
  // engine infers its rate from it), plus the input latency.
  const int seekLength = std::max(e.inputLatency(), static_cast<int>(std::llround(srcAt(e.outputLatency()) + e.inputLatency())) - static_cast<int>(base));
  e.outputSeek(PaddedIn{in, n, base}, seekLength);
  int64_t fed = base + seekLength;  // input frames given to the engine so far
  // J-145: the engine emits output frame j + m + outputLatency's analysis
  // when it has been fed up to that frame's input position plus its input
  // latency. Feeding from the map at that future frame keeps every output
  // frame where the map puts it. (J-87 fed srcAt(j + m) + rate * latency
  // with the rate of the block being written, then clamped: on every tempo
  // drop the engine ran ahead and never came back, so clips under tempo
  // changes drifted, late by 100 ms after a minute of steps.)
  const int outLat = e.outputLatency(), inLat = e.inputLatency();
  for (int64_t j = 0; j < outLen; j += blk) {
    const int64_t m = std::min(blk, outLen - j);
    const auto want = static_cast<int64_t>(std::llround(srcAt(j + m + outLat) + inLat));
    const int64_t give = std::max<int64_t>(0, want - fed);
    e.process(PaddedIn{in, n, fed}, static_cast<int>(give), OutView{out, j}, static_cast<int>(m));
    fed += give;
    if (post && *post) (*post)(out, j, j + m);
    if (pr && *pr) (*pr)(j + m);
  }
}

// J-145: the inverse of a time map: the first output frame whose input position reaches i.
int64_t frameOf(const std::function<double(int64_t)>& srcAt, int64_t outLen, double i) {
  int64_t lo = 0, hi = outLen;
  while (lo < hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    if (srcAt(mid) < i)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// Variable rate, every mode but Resample, engine time only (the caller resamples for the pitch).
void mappedStretch(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sr, const Settings& s, double pitch,
                   const std::function<double(int64_t)>& srcAt) {
  if (!usesOnsets(s.mode)) {
    mappedCore(in, nch, n, out, outLen, sr, s, pitch, srcAt);
    return;
  }
  TimeMap map;
  map.x = srcAt;
  map.outAt = [&](int64_t i) { return frameOf(srcAt, outLen, static_cast<double>(i)); };
  hybridCore(in, nch, n, outLen, sr, onsetSensitivity(s.mode), map, [&](const float* const* h, const TimeMap& em, const Post& post) {
    mappedCore(h, nch, n, out, outLen, sr, s, pitch, em.x, Part::Harmonic, &post);
  });
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
  if (std::fabs(pitch - 1.0) > 1e-12 && s.mode != Mode::Voice) {
    // As in plain(): stretch only (to pitch × the length), then resample.
    const SincTable& t = sincTable(Quality::Offline);
    const auto mid = static_cast<int64_t>(std::ceil(static_cast<double>(outLen) * pitch)) + t.zeroCrossings + 2;
    std::vector<std::vector<float>> m(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(mid), 0.0f));
    std::vector<float*> mp(static_cast<size_t>(nch));
    for (int c = 0; c < nch; ++c) mp[static_cast<size_t>(c)] = m[static_cast<size_t>(c)].data();
    // Intermediate frame q plays at output time q / pitch.
    const auto midSrc = [&](int64_t q) {
      const double t = static_cast<double>(q) / pitch;
      const auto j = static_cast<int64_t>(std::floor(t));
      const double a = srcAt(j);
      return a + (t - static_cast<double>(j)) * (srcAt(j + 1) - a);
    };
    mappedStretch(in, nch, n, mp.data(), mid, sr, s, pitch, midSrc);
    if (nch == 2)
      resample2(mp[0], mp[1], mid, 0.0, pitch, out[0], out[1], outLen);
    else
      for (int c = 0; c < nch; ++c) resample(mp[static_cast<size_t>(c)], mid, 0.0, pitch, out[c], outLen);
    // On the band-limited result: what the resampler removed must not count.
    if (keepsFormants(s.mode)) pitch::matchLevel(out, nch, outLen, in, n, srcAt, sr);
    return;
  }
  mappedStretch(in, nch, n, out, outLen, sr, s, s.mode == Mode::Voice ? pitch : 1.0, srcAt);  // Voice: the engine's own pitch
  if (keepsFormants(s.mode)) pitch::matchLevel(out, nch, outLen, in, n, srcAt, sr);
}

// ------------------------------------------------------------- resampling

namespace {
// Two channels in one pass (one weight computation for both).
void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen) {
  pitch::resample2(l, r, n, start, rate, outL, outR, outLen, Quality::Offline);
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
  Engine engine{kSeed};  // the whole sound, or (hybrid) its harmonic part
  int channels = 2;
  double sampleRate = 44100;
  double maxPitch = 8, maxSpeed = 8;
  // Voice: the engine changes the pitch itself, mid is the output (no resampler, no attack copy).
  bool native = false;
  double midRate(double pitch) const { return native ? 1.0 : pitch; }
  // Engine output (before the pitch resampler): frames [0, filled) of mid,
  // read at fractional position `pos`. Compacted as it is consumed.
  std::vector<float> mid[2];
  int64_t filled = 0;
  double pos = 0;
  // J-145: the pitch glides to each new value through two one-pole stages (log
  // domain, per frame): automation and LFOs move smoothly instead of stepping
  // once per call, and the result does not depend on how the calls are cut.
  double glide[2] = {0, 0};  // log pitch after stage 1 / 2
  bool glideSet = false;
  double glideCoef = 0;      // per-frame stage coefficient
  double inAcc = 0;          // fractional input frames owed to the engine
  double xAtFilled = 0;      // input frame heard at mid frame `filled`
  int64_t inTotal = 0;       // input frames read since the start
  std::vector<float> in[4];  // reader scratch (input, harmonic part)
  // Attack of the sound: its first milliseconds straight from the input
  // (played at speed 1 before the pitch resampler), then a crossfade into the
  // engine's output (the modes without the hybrid).
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
  static constexpr int kChunk = 256;  // engine output frames per process call
  static constexpr double kGlideSec = 0.006;  // pitch glide time constant per stage

  // ---- J-145: hybrid stretch (onset modes, the reader supplies the harmonic part)
  // The harmonic part runs through `engine`, never restarted; the percussive
  // part (input - harmonic) through `perc`, restarted at each onset as the
  // offline percussivePart() does it: the segment starts `lead` frames ahead
  // of the hit (fading in while the previous one fades out) and never reads
  // past the next segment's start. Both engines follow one elastic time map
  // (as offline: hitPlan / elasticMap): each hit's span plays at the original
  // speed in both, so the harmonic part takes a new note over from the
  // percussive part in step and in phase; around the spans the map drifts
  // back to the nominal one (at most at half or twice its rate). The map is
  // the nominal one plus the offset e(j) of the spans (Span); the harmonic
  // engine is fed for the frame it will play an output latency later. Both
  // engines read a voice-owned history of the input, each at its own cursor.
  bool split = false;   // prepared for the hybrid (onset modes)
  bool hybrid = false;  // this sound runs it (start with harmonic)
  pitch::TdStretch perc;
  std::vector<float> hist[4];  // input (0, 1) and its harmonic part (2, 3), ring
  int64_t hmask = 0;
  int64_t hFed = 0, pFed = 0, pStart = 0;  // input frames given to each engine; the segment's first
  double pAcc = 0;
  int leadLen = 0, leadPos = 0;            // fade-in of the segment
  std::vector<float> pOut[2], pTail[2];
  int tailLen = 0, tailPos = 0;            // fade-out of the previous segment
  int pre = 0, attack = 0, maxPull = 0, minGap = 0;
  static constexpr int kOnsets = 16;
  int64_t ons[kOnsets] = {};
  int onHead = 0, onCount = 0;
  int64_t lastOnset = 0;
  bool endSet = false;
  PreGate gate;
  // Elastic map: a hit's span. Mid frames: the hit plays at H, the span at the
  // original speed runs from H - lead to H + att, the offset ramps in over D
  // frames before it and back out over R frames after it.
  struct Span {
    double H = 0, lead = 0, att = 0, D = 0, R = 0, s = 0;  // s: offset per frame in the span (1 - input per mid frame)
    double e(double j) const {
      const double a = H - lead, b = H + att;
      if (j <= a - D || j >= b + R) return 0.0;
      if (j < a) return -lead * s * (j - (a - D)) / D;
      if (j <= b) return (j - H) * s;
      return att * s * (1.0 - (j - b) / R);
    }
    double end() const { return H + att + R; }
  };
  static constexpr int kSpans = 4;
  Span spans[kSpans];  // started hits whose offset may still apply (ring)
  int spanHead = 0, spanCount = 0;
  Span qspan[kOnsets];  // queued hits: their span once fixed
  bool qfixed[kOnsets] = {};
  double curO = -1e18;          // input frame of the running segment's hit
  double eLastH = 0, eLastP = 0;  // the offsets the engines were last fed for
  int hOutLat = 0, hInLat = 0;    // the harmonic engine's latencies
  static Span makeSpan(double H, double lead, double att, double perMid) {
    Span sp;
    sp.H = H, sp.lead = lead, sp.att = att, sp.s = 1.0 - perMid;
    const double drain = std::max(1e-9, sp.s > 0 ? 0.5 * perMid : perMid);  // offset change per mid frame around the span
    sp.D = std::fabs(lead * sp.s) / drain, sp.R = std::fabs(att * sp.s) / drain;
    return sp;
  }
  // The span of queued hit q (0: the next one), fixed or as the queue stands.
  Span queuedSpan(int q, double perMid) const {
    const int i = (onHead + q) % kOnsets;
    if (qfixed[i]) return qspan[i];
    perMid = std::max(1e-6, perMid);
    const auto o = static_cast<double>(ons[i]);
    const double prev = q == 0 ? curO : static_cast<double>(ons[(i + kOnsets - 1) % kOnsets]);
    // The next hit, or a lower bound for it (what has been read): every bound below grows with it.
    const double next = q + 1 < onCount ? static_cast<double>(ons[(i + 1) % kOnsets]) : std::max(o + 1.0, static_cast<double>(inTotal));
    auto budget = [](double I, double O) {
      const double den = 2 * std::max(I, O) - std::min(I, O);
      return I > 0 && O > 0 && den > 0 ? I * O / den : 0.0;
    };
    const double i0 = std::max(0.0, o - prev), o0 = i0 / perMid, i1 = next - o, o1 = i1 / perMid;
    const double lead = std::floor(std::max(0.0, std::min({static_cast<double>(pre), i0 / 4, o0 / 4, budget(i0, o0) / 4})));
    const double nextLead = std::min({static_cast<double>(pre), i1 / 4, o1 / 4, budget(i1, o1) / 4});
    const double att = std::floor(std::max(0.0, std::min({static_cast<double>(attack), i1 / 2, o1 / 2, budget(i1, o1) - nextLead})));
    return makeSpan(static_cast<double>(filled) + (o - xAtFilled) / perMid, lead, att, perMid);
  }
  // The spans' offset at mid frame j (input frames), piecewise linear.
  double rawOffset(double j, double perMid) const {
    double e = 0;
    for (int k = 0; k < spanCount; ++k) e += spans[(spanHead + k) % kSpans].e(j);
    for (int q = 0; q < onCount; ++q) e += queuedSpan(q, perMid).e(j);
    return e;
  }
  // The map's offset: the spans' one smoothed (a triangle of half-width
  // smoothW mid frames, as the offline map's two box passes), so the
  // engines' rate eases in and out of each span instead of stepping (a rate
  // step in the harmonic engine dips the low notes).
  double smoothW = 0;
  double offsetAt(double j, double perMid) const {
    if (smoothW < 1.0) return rawOffset(j, perMid);
    constexpr int kTaps = 8;  // per side
    double e = 0;
    for (int k = -kTaps; k <= kTaps; ++k) e += (kTaps + 1 - std::abs(k)) * rawOffset(j + smoothW * k / kTaps, perMid);
    return e / ((kTaps + 1) * (kTaps + 1));
  }
  void pushSpan(const Span& sp) {
    if (spanCount == kSpans) spanHead = (spanHead + 1) % kSpans, --spanCount;  // (the oldest has run out by far)
    spans[(spanHead + spanCount) % kSpans] = sp;
    ++spanCount;
  }

  struct HistView {  // the harmonic part (diff = false) or the rest (diff = true) from input frame `off`
    const Impl* m;
    int64_t off;
    bool diff;
    struct Ch {
      const float* x;
      const float* h;
      int64_t off, mask;
      bool diff;
      float operator[](int i) const {
        const auto k = static_cast<size_t>((off + i) & mask);
        return diff ? x[k] - h[k] : h[k];
      }
    };
    Ch operator[](int c) const { return Ch{m->hist[c].data(), m->hist[c + 2].data(), off, m->hmask, diff}; }
  };
  void noteOnset(int64_t o) {
    if (o < minGap || o - lastOnset < minGap || onCount >= kOnsets) return;
    lastOnset = o;
    ons[(onHead + onCount) % kOnsets] = o;
    qfixed[(onHead + onCount) % kOnsets] = false;
    ++onCount;
    if (!endSet && onCount == 1) setEnd();
  }
  // The running segment never reads into the next one (it starts a lead ahead of its hit).
  void setEnd() {
    if (onCount <= 0) return;
    const auto lead = static_cast<int64_t>(qfixed[onHead] ? qspan[onHead].lead : pre);
    perc.setInputEnd(std::max<int64_t>(1, ons[onHead] - lead - pStart));
    endSet = true;
  }
  void pullHist(Reader rd, void* ctx, int frames) {
    float* d[4] = {in[0].data(), in[1].data(), in[2].data(), in[3].data()};
    const int on = rd(ctx, d, frames, false);
    for (int c = 0; c < 4; ++c) {
      float* h = hist[c].data();
      const float* s = in[c].data();
      for (int k = 0; k < frames; ++k) h[static_cast<size_t>((inTotal + k) & hmask)] = s[k];
    }
    if (on >= 0 && on < frames) noteOnset(inTotal + on);
    inTotal += frames;
  }
  // Reads until the history holds input frames [.., upTo).
  void ensure(int64_t upTo, Reader rd, void* ctx) {
    while (inTotal < upTo) pullHist(rd, ctx, static_cast<int>(std::min<int64_t>(maxPull, upTo - inTotal)));
  }
  void startPerc(int64_t s, int lead, Reader rd, void* ctx) {
    const int seek = perc.outputSeekLength(1.0f);
    ensure(s + seek, rd, ctx);
    perc.outputSeek(HistView{this, s, true}, seek);
    pStart = s, pFed = s + seek, pAcc = 0;
    leadLen = lead, leadPos = 0;
    endSet = false;
    setEnd();
  }
  // At the segment boundary: the old segment's last frames (to fade out), then the new segment.
  void restartPerc(double perMid, Reader rd, void* ctx) {
    const Span sp = queuedSpan(0, perMid);
    const int64_t o = ons[onHead];
    onHead = (onHead + 1) % kOnsets, --onCount;
    const int lead = static_cast<int>(sp.lead);
    const int tl = std::min(pre, std::max(lead, 1));
    pushSpan(sp);
    {
      // The old segment's last frames (along the map), to fade out.
      const double d = perMid * tl + offsetAt(static_cast<double>(filled + tl), perMid) - offsetAt(static_cast<double>(filled), perMid);
      const int kt = std::clamp(static_cast<int>(std::lround(d)), 1, inCap);
      ensure(pFed + kt, rd, ctx);
      float* tp[2] = {pTail[0].data(), pTail[1].data()};
      perc.process(HistView{this, pFed, true}, kt, tp, tl);
      pFed += kt;
      tailLen = tl, tailPos = 0;
    }
    curO = static_cast<double>(o);
    const int64_t oldest = inTotal - hmask;
    const int64_t s = std::max({int64_t{0}, o - lead, oldest});
    startPerc(s, static_cast<int>(o - s), rd, ctx);
    eLastP = offsetAt(static_cast<double>(filled), perMid);
  }
  // Read-ahead (input frames past the harmonic engine's feed): far enough to
  // know each hit, and the one after it, before the feed reaches its span.
  int64_t readAhead(double perMid) const {
    return static_cast<int64_t>(std::ceil(2.0 * attack + 3.0 * pre + perMid * kChunk + smoothW * (1.0 + perMid))) + 1;
  }
  // Makes mid hold at least `need` frames (hybrid).
  void produceSplit(int64_t need, double speed, double pitch, Reader rd, void* ctx) {
    const double perMid = std::max(1e-6, speed / pitch);
    const int64_t ahead = readAhead(perMid);
    while (filled < need) {
      // Spans the harmonic feed reaches in this chunk are fixed from now on.
      for (int q = 0; q < onCount; ++q) {
        const int i = (onHead + q) % kOnsets;
        if (qfixed[i]) continue;
        const Span sp = queuedSpan(q, perMid);
        if (static_cast<double>(filled + kChunk + hOutLat) + smoothW < sp.H - sp.lead - sp.D) break;
        qspan[i] = sp, qfixed[i] = true;
        if (q == 0) setEnd();
      }
      while (spanCount > 0 && spans[spanHead].end() + smoothW < static_cast<double>(filled)) spanHead = (spanHead + 1) % kSpans, --spanCount;
      int64_t lim = kChunk;
      if (onCount > 0) {
        const Span sp = queuedSpan(0, perMid);
        const double r = sp.H - sp.lead;  // the next segment starts here
        if (r <= static_cast<double>(filled)) {
          restartPerc(perMid, rd, ctx);
          continue;
        }
        lim = std::min<int64_t>(lim, static_cast<int64_t>(std::ceil(r)) - filled);
      }
      const int m = static_cast<int>(std::max<int64_t>(1, lim));
      // Harmonic part, fed for the frame it plays an output latency later.
      const double eH = offsetAt(static_cast<double>(filled + m + hOutLat), perMid);
      inAcc += perMid * m + (eH - eLastH);
      eLastH = eH;
      const int k = std::clamp(static_cast<int>(std::floor(inAcc)), 0, inCap);
      inAcc -= k;
      ensure(hFed + k + ahead, rd, ctx);
      float* op[2] = {mid[0].data() + filled, mid[1].data() + filled};
      engine.process(HistView{this, hFed, false}, k, op, m);
      hFed += k;
      // Percussive part.
      const double eP = offsetAt(static_cast<double>(filled + m), perMid);
      pAcc += perMid * m + (eP - eLastP);
      eLastP = eP;
      const int kp = std::clamp(static_cast<int>(std::floor(pAcc)), 0, inCap);
      pAcc -= kp;
      ensure(pFed + kp, rd, ctx);
      float* pp[2] = {pOut[0].data(), pOut[1].data()};
      perc.process(HistView{this, pFed, true}, kp, pp, m);
      pFed += kp;
      for (int c = 0; c < channels; ++c) {
        for (int j = 0; j < m; ++j) {
          const int a = leadPos + j;
          const double g = a < leadLen ? 0.5 - 0.5 * std::cos(kPi * (a + 1) / (leadLen + 1)) : 1.0;
          op[c][j] += static_cast<float>(g * pOut[c][static_cast<size_t>(j)]);
        }
        for (int j = 0; j < m && tailPos + j < tailLen; ++j) {
          const int a = tailPos + j;
          const double g = 0.5 + 0.5 * std::cos(kPi * (a + 1) / (tailLen + 1));
          op[c][j] += static_cast<float>(g * pTail[c][static_cast<size_t>(a)]);
        }
      }
      // The pre-echo gate, up to the hit that starts the running segment or the next one.
      double untilHit = -1, onsetIn = 0;
      if (leadPos < leadLen) {
        untilHit = leadLen - leadPos, onsetIn = static_cast<double>(pStart + leadLen);
      } else if (onCount > 0) {
        onsetIn = static_cast<double>(ons[onHead]);
        untilHit = std::max(0.0, queuedSpan(0, perMid).H - static_cast<double>(filled));
      }
      if (untilHit >= 0) {
        const int64_t span = gate.inputSpan(perMid);
        const double x0 = xAtFilled + offsetAt(static_cast<double>(filled), perMid);
        gate.apply(op, channels, m, untilHit, 1.0 / perMid, [&](int k) {
          const double x = std::min(x0 + k * perMid, onsetIn);
          const int64_t i1 = std::clamp<int64_t>(static_cast<int64_t>(std::llround(x)), 0, inTotal);
          const int64_t i0 = std::max({int64_t{0}, i1 - span, inTotal - hmask});
          double e = 0;
          for (int c = 0; c < channels; ++c) {
            const float* h = hist[c].data();
            for (int64_t i = i0; i < i1; ++i) e += static_cast<double>(h[static_cast<size_t>(i & hmask)]) * h[static_cast<size_t>(i & hmask)];
          }
          return i1 > i0 ? std::sqrt(e / static_cast<double>(channels * (i1 - i0))) : 0.0;
        });
      } else {
        gate.apply(op, channels, m, -1.0, 1.0, [](int) { return 0.0; });
      }
      leadPos = std::min(leadLen, leadPos + m);
      tailPos = std::min(tailLen, tailPos + m);
      filled += m;
      xAtFilled += perMid * m;
    }
  }

  // J-140 Formant mode: cumulative input energy by input frame (ring), so
  // each engine chunk's level can follow the source span it came from.
  bool followLevel = false;
  pitch::LevelFollower level;
  std::vector<double> esum;
  std::vector<float> apk;  // per input frame: the largest |sample| of the channels (ring, as esum)
  int64_t emask = 0;
  double ecum = 0;
  // The engine output as the pitch resampler will leave it (its band limit),
  // for the level measure: content above the output Nyquist frequency must
  // not count. Two low-pass sections per channel, set for lpPitch.
  dsp::Biquad lp[2][2];
  double lpPitch = 0;
  double peakIn(double a, double b) const {
    const auto i0 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(a)), std::max<int64_t>(0, inTotal - emask), inTotal);
    const auto i1 = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(b)), i0, inTotal);
    float p = 0;
    for (int64_t i = i0; i < i1; ++i) p = std::max(p, apk[static_cast<size_t>(i & emask)]);
    return p;
  }
  double energyAt(double x) const {
    const auto i = std::clamp<int64_t>(static_cast<int64_t>(std::llround(x)), std::max<int64_t>(0, inTotal - emask), inTotal);
    return esum[static_cast<size_t>(i & emask)];
  }
  // One continuous engine (every mode but the hybrid): reads straight into `in`.
  void pull(Reader rd, void* ctx, int frames) {
    float* d[4] = {in[0].data(), in[1].data(), in[2].data(), in[3].data()};
    rd(ctx, d, frames, false);
    if (followLevel) {
      for (int k = 0; k < frames; ++k) {
        double e = 0;
        for (int c = 0; c < channels; ++c) e += static_cast<double>(in[c][static_cast<size_t>(k)]) * in[c][static_cast<size_t>(k)];
        ecum += e / channels;
        esum[static_cast<size_t>((inTotal + k + 1) & emask)] = ecum;
        float pk = 0;
        for (int c = 0; c < channels; ++c) pk = std::max(pk, std::fabs(in[c][static_cast<size_t>(k)]));
        apk[static_cast<size_t>((inTotal + k) & emask)] = pk;
      }
    }
    inTotal += frames;
  }
  void resetLevel() {
    ecum = 0;
    if (followLevel) esum[0] = 0;
    level.reset();
    for (auto& ch : lp)
      for (auto& s : ch) s.clear();
    lpPitch = 0;
  }
  // Makes mid hold at least `need` frames (one engine).
  void produce(int64_t need, double speed, double pitch, Reader rd, void* ctx) {
    if (hybrid) {
      produceSplit(need, speed, pitch, rd, ctx);
      return;
    }
    const double perMid = speed / pitch;  // input frames per engine output frame
    while (filled < need) {
      // Whole chunks, so the output does not depend on how the caller splits its blocks.
      const int m = kChunk;
      inAcc += perMid * m;
      const int k = std::clamp(static_cast<int>(std::floor(inAcc)), 0, inCap);
      inAcc -= k;
      pull(rd, ctx, k);
      const float* ip[2] = {in[0].data(), in[1].data()};
      float* op[2] = {mid[0].data() + filled, mid[1].data() + filled};
      engine.process(ip, k, op, m);
      if (followLevel) {
        const double a = xAtFilled, b = xAtFilled + perMid * m;
        const double srcMs = b - a > 0.5 ? (energyAt(b) - energyAt(a)) / (b - a) : 0.0;
        // Measured as it will be heard: band-limited like the resampler
        // leaves it when pitching up (a formant-keeping engine's mid signal
        // can carry content the resampler removes).
        const bool band = pitch > 1.02;
        if (band && std::fabs(pitch - lpPitch) > 1e-3) {
          const double fc = 0.45 * sampleRate / pitch;
          for (auto& ch : lp) {
            ch[0].set(dsp::Biquad::LowPass, sampleRate, fc, 0.5412, 0);
            ch[1].set(dsp::Biquad::LowPass, sampleRate, fc, 1.3066, 0);
          }
          lpPitch = pitch;
        }
        double outMs = 0, outPk = 0;
        for (int c = 0; c < channels; ++c)
          for (int j = 0; j < m; ++j) {
            const double v = band ? lp[c][1].process(lp[c][0].process(op[c][j])) : op[c][j];
            outMs += v * v;
            outPk = std::max(outPk, std::fabs(v));
          }
        // Voice moves whole periods, and a formant-keeping shift moves the
        // pulses of a voice within its blocks: a block can hold a pulse whose
        // source span holds none, so the source peak is taken over a period
        // around it (J-144 for Voice; J-145 for every mode: without it the
        // limit pumps at the pulse rate, Formant +7 st by 0.5 dB).
        const double around = 0.02 * sampleRate;
        const double srcPk = b - a > 0.5 ? peakIn(a - around, b + around) : 0.0;
        level.apply(op, channels, m, srcMs, outMs / (channels * m), outPk > 1e-9 && srcPk > 0 ? srcPk / outPk : 0.0);
      }
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
      filled += m;
      xAtFilled += perMid * m;
    }
  }
  void compact(int hw) {
    const auto drop = static_cast<int64_t>(std::floor(pos)) - hw - 2;
    if (drop <= 0 || drop < midCap / 4) return;
    for (int c = 0; c < channels; ++c)
      std::copy(mid[c].begin() + static_cast<std::ptrdiff_t>(drop), mid[c].begin() + static_cast<std::ptrdiff_t>(filled), mid[c].begin());
    filled -= drop;
    pos -= static_cast<double>(drop);
    // The spans of the elastic map are in mid frames too.
    for (auto& sp : spans) sp.H -= static_cast<double>(drop);
    for (auto& sp : qspan) sp.H -= static_cast<double>(drop);
  }
};

Voice::Voice() : impl_(std::make_unique<Impl>()) {}
Voice::~Voice() = default;

void Voice::prepare(int channels, double sampleRate, Mode mode, double maxPitch, double maxSpeed, bool light) {
  mode_ = mode == Mode::Resample ? Mode::Auto : mode;
  Impl& m = *impl_;
  m.channels = std::clamp(channels, 1, 2);
  m.sampleRate = sampleRate;
  m.glideCoef = 1.0 - std::exp(-1.0 / (Impl::kGlideSec * sampleRate));
  m.maxPitch = std::max(1.0, maxPitch);
  m.maxSpeed = std::max(1.0, maxSpeed);
  m.split = usesOnsets(mode_);
  setup(m.engine, m.channels, sampleRate, mode_, false, light, m.split ? Part::Harmonic : Part::Whole);
  m.native = m.engine.kind == Engine::kPsola;
  applyPitch(m.engine, mode_, 1.0, 0.0);
  const SincTable& t = sincTable(Quality::Realtime);
  // mid: render steps at the top pitch, the resampler's reach, slack for compaction.
  const int hw = pitch::maxReach(m.maxPitch, t) + 4;
  m.midCap = 8 * (Impl::kChunk + hw) + static_cast<int>(2048 * m.maxPitch);
  // in: the engine's start-up input, or one chunk at the top input rate.
  const double perMid = m.maxSpeed * m.maxPitch;  // speed / pitch at the lowest pitch
  m.inCap = std::max(m.engine.outputSeekLength(static_cast<float>(perMid)), static_cast<int>(std::ceil(perMid * Impl::kChunk))) + 16;
  m.followLevel = keepsFormants(mode_);
  m.level.setRate(sampleRate);
  if (m.followLevel) {
    int64_t cap = 1;
    while (cap < 2 * static_cast<int64_t>(m.inCap) + 4096) cap <<= 1;
    m.esum.assign(static_cast<size_t>(cap), 0.0);
    m.apk.assign(static_cast<size_t>(cap), 0.0f);
    m.emask = cap - 1;
  } else {
    m.esum.clear();
    m.apk.clear();
    m.emask = 0;
  }
  m.pre = static_cast<int>(kLeadSec * sampleRate);
  m.attack = static_cast<int>(kAttackSec * sampleRate);
  m.smoothW = kSmoothRtSec * sampleRate;
  m.minGap = static_cast<int>(0.04 * sampleRate);
  m.maxPull = std::max(16, std::min(512, static_cast<int>(0.025 * sampleRate)));
  m.hOutLat = m.engine.outputLatency(), m.hInLat = m.engine.inputLatency();
  if (m.split) {
    m.perc.configure(m.channels, sampleRate, pitch::TdStretch::percussive());
    m.gate.configure(sampleRate);
    // History: both engines' start-up input, the read-ahead, a segment's lead-in, chunks at the top rate.
    const int64_t need = static_cast<int64_t>(m.engine.outputSeekLength(static_cast<float>(perMid))) + m.perc.outputSeekLength(1.0f) +
                         4 * static_cast<int64_t>(m.inCap) + m.readAhead(perMid) + m.pre + m.attack + 4096;
    int64_t cap = 1;
    while (cap < need) cap <<= 1;
    for (auto& h : m.hist) h.assign(static_cast<size_t>(cap), 0.0f);
    m.hmask = cap - 1;
  } else {
    for (auto& h : m.hist) h.clear();
    m.hmask = 0;
  }
  for (int c = 0; c < 4; ++c) m.in[c].assign(static_cast<size_t>(std::max(m.inCap, m.maxPull)), 0.0f);
  for (int c = 0; c < 2; ++c) {
    m.mid[c].assign(static_cast<size_t>(m.midCap), 0.0f);
    m.atk[c].assign(static_cast<size_t>(std::ceil(0.025 * sampleRate)) + 1, 0.0f);
    m.pOut[c].assign(m.split ? static_cast<size_t>(Impl::kChunk) : 0, 0.0f);
    m.pTail[c].assign(m.split ? static_cast<size_t>(m.pre) + 1 : 0, 0.0f);
  }
  prepared_ = true;
  reset();
}

void Voice::reset() {
  Impl& m = *impl_;
  m.engine.reset();
  m.filled = 0;
  m.pos = 0;
  m.glideSet = false;
  m.inAcc = 0;
  m.xAtFilled = 0;
  m.inTotal = 0;
  m.atkHold = m.atkLen = m.atkPos = 0;
  m.hybrid = false;
  if (m.split) m.perc.reset(), m.gate.reset();
  m.hFed = m.pFed = m.pStart = 0;
  m.pAcc = 0;
  m.leadLen = m.leadPos = m.tailLen = m.tailPos = 0;
  m.onHead = m.onCount = 0;
  m.lastOnset = 0;
  m.endSet = false;
  m.spanHead = m.spanCount = 0;
  std::fill(std::begin(m.qfixed), std::end(m.qfixed), false);
  m.curO = -1e18;
  m.eLastH = m.eLastP = 0;
  m.resetLevel();
}

int Voice::lookahead(double speed, double pitch) const {
  pitch = std::max(1e-3, pitch);
  const double perMid = speed / impl_->midRate(pitch);
  const SincTable& t = sincTable(Quality::Realtime);
  const double hw = impl_->native ? 0.0 : pitch::reach(pitch, t);
  const Impl& m = *impl_;
  const double extra = m.split ? static_cast<double>(m.readAhead(perMid)) : 0.0;  // the hybrid's read-ahead for hits
  return static_cast<int>(std::ceil(impl_->engine.outputSeekLength(static_cast<float>(perMid)) + (hw + Impl::kChunk) * perMid + extra)) + 2;
}

void Voice::start(Reader rd, void* ctx, double speed, double pitch, bool attack, bool harmonic) {
  Impl& m = *impl_;
  reset();
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  m.glide[0] = m.glide[1] = std::log(pitch);
  m.glideSet = true;
  const double perMid = speed / m.midRate(pitch);
  const int seek = std::min(m.inCap, m.engine.outputSeekLength(static_cast<float>(std::max(1e-3, perMid))));
  if (m.split && harmonic) {
    m.hybrid = true;
    // The sound's start is its first hit: its span (the hits read ahead bound it).
    const double pm = std::max(1e-6, perMid);
    m.ensure(m.readAhead(pm), rd, ctx);
    m.curO = 0;
    {
      const double next = m.onCount > 0 ? static_cast<double>(m.ons[m.onHead]) : static_cast<double>(m.inTotal);
      const double i1 = next, o1 = i1 / pm, den = 2 * std::max(i1, o1) - std::min(i1, o1);
      const double budget = den > 0 ? i1 * o1 / den : 0.0;
      const double nextLead = std::min({static_cast<double>(m.pre), i1 / 4, o1 / 4, budget / 4});
      const double att = std::floor(std::max(0.0, std::min({static_cast<double>(m.attack), i1 / 2, o1 / 2, budget - nextLead})));
      m.pushSpan(Impl::makeSpan(0.0, m.smoothW, att, pm));  // (the lead: before the start, so the smoothed map is 0 there)
    }
    // Seek so that output frame 0 plays input frame 0 and the feed runs on the map.
    const int hseek = std::clamp(static_cast<int>(std::llround(m.hInLat + pm * m.hOutLat + m.offsetAt(static_cast<double>(m.hOutLat), pm))), 1, m.inCap);
    m.ensure(hseek, rd, ctx);
    m.engine.outputSeek(Impl::HistView{&m, 0, false}, hseek);
    m.hFed = hseek;
    m.eLastH = m.offsetAt(static_cast<double>(m.hOutLat), pm);
    m.startPerc(0, 0, rd, ctx);
    m.eLastP = 0;
    return;
  }
  m.pull(rd, ctx, seek);
  const float* ip[2] = {m.in[0].data(), m.in[1].data()};
  m.engine.outputSeek(ip, seek);
  if (attack && !m.native) m.takeAttack(seek);
}

void Voice::render(float* const* out, int n, double speed, double pitch, double formantSemis, Reader rd, void* ctx) {
  Impl& m = *impl_;
  pitch = std::clamp(pitch, 1.0 / m.maxPitch, m.maxPitch);
  speed = std::clamp(speed, 0.0, m.maxSpeed);
  if (keepsFormants(mode_)) applyPitch(m.engine, mode_, pitch, formantSemis);
  if (m.native) {
    // The engine's output is the voice's output: copied, never resampled.
    int done = 0;
    while (done < n) {
      const int step = std::min(n - done, m.midCap / 4);
      const auto p0 = static_cast<int64_t>(m.pos);
      m.produce(p0 + step, speed, 1.0, rd, ctx);
      for (int c = 0; c < m.channels; ++c)
        std::copy(m.mid[c].begin() + static_cast<std::ptrdiff_t>(p0), m.mid[c].begin() + static_cast<std::ptrdiff_t>(p0 + step), out[c] + done);
      m.pos += step;
      m.compact(0);
      done += step;
    }
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
    const auto need = static_cast<int64_t>(std::ceil(end)) + hw + 1;
    // The engine's input rate follows the requested pitch (not the glide's
    // state at this call), so where a block is split cannot change it.
    m.produce(need, speed, pitch, rd, ctx);
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
