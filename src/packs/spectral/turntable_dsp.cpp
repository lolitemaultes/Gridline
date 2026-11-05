// Gridline — "spectral" pack: Turntable runtimes, the effect (gl.turntablefx,
// K-23) and the 0.7.1 instrument (gl.turntable), one deck engine. See
// turntable.h for the model. J-206: at 44.1 kHz every step runs in single
// precision in the reference's order (the few products it forms in double are
// formed in double here too), so the output matches it sample for sample; at
// other rates the per-sample coefficients are converted so the platter feels
// and sounds the same. No allocation on the audio thread: the deck is prepared
// on the UI thread (prepareTurntableAux) and only read here.
#include <algorithm>
#include "../../engine/SampleTap.h"
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <mutex>
#include "../../engine/Dsp.h"
#include "turntable.h"

namespace gl::packs::spectral {

namespace {

constexpr double kRefRate = 44100.0;
constexpr int kLatchFrames = 512;  // the reference measured at host blocks of 512
constexpr float kQ = 3.0517578125e-05f;  // 2^-15: one 16-bit step

// The deck as the reference plays it: 44.1 kHz stereo at 16-bit levels, with a
// guard frame after the end (the reference fills it from the first frame's
// right channel, on both sides).
struct TurntableDeck final : AuxData {
  SamplePtr src;            // keeps the source alive (direct reads at 44.1 kHz)
  std::vector<float> l, r;  // resampled copy for other rates
  const float* L = nullptr;
  const float* R = nullptr;
  int frames = 0;
};

// 16-bit level of a sample value (the host hands the reference 16-bit audio).
inline float q16(float x) {
  float y = x * 32768.0f;
  if (!(y > -32768.0f)) y = -32768.0f;  // also NaN
  if (y > 32767.0f) y = 32767.0f;
  return static_cast<float>(std::lrint(y)) * kQ;
}

std::shared_ptr<const TurntableDeck> makeDeck(const SamplePtr& s) {
  auto d = std::make_shared<TurntableDeck>();
  d->src = s;
  const size_t n = s->frames();
  const double sr = s->sampleRate > 0 && std::isfinite(s->sampleRate) ? s->sampleRate : kRefRate;
  if (std::fabs(sr - kRefRate) < 0.5) {
    d->L = s->left.data();
    d->R = s->right.vec().data();
    d->frames = static_cast<int>(std::min<size_t>(n, static_cast<size_t>(INT_MAX / 4)));
    return d;
  }
  // Other rates: one linear resample to 44.1 kHz (prepared once, cached).
  const double ratio = sr / kRefRate;
  const double m = std::floor(static_cast<double>(n) / ratio);
  const size_t out = static_cast<size_t>(std::clamp(m, 2.0, static_cast<double>(INT_MAX / 4)));
  d->l.resize(out);
  d->r.resize(out);
  const float* sl = s->left.data();
  const float* sr2 = s->right.vec().data();
  for (size_t i = 0; i < out; ++i) {
    const double x = static_cast<double>(i) * ratio;
    size_t i0 = static_cast<size_t>(x);
    if (i0 >= n - 1) i0 = n - 2;
    const double f = std::min(1.0, x - static_cast<double>(i0));
    d->l[i] = static_cast<float>(sl[i0] + (sl[i0 + 1] - sl[i0]) * f);
    d->r[i] = static_cast<float>(sr2[i0] + (sr2[i0 + 1] - sr2[i0]) * f);
  }
  d->L = d->l.data();
  d->R = d->r.data();
  d->frames = static_cast<int>(out);
  return d;
}

// Decks are cached by source sample (Program builds prepare the aux often).
std::shared_ptr<const TurntableDeck> deckFor(const SamplePtr& s) {
  static std::mutex mu;
  static std::array<std::weak_ptr<const TurntableDeck>, 8> cache;
  static size_t next = 0;
  std::lock_guard<std::mutex> lock(mu);
  for (auto& w : cache)
    if (auto d = w.lock(); d && d->src == s) return d;
  auto d = makeDeck(s);
  cache[next++ % cache.size()] = d;
  return d;
}

// The reference's index for a read at x >= 0: floor(x) between frames, x - 1
// on a frame (the interpolation then lands exactly on it).
inline int refIndex(float x) {
  const float t = x - 0.5f;
  int i = static_cast<int>(t);
  const double td = static_cast<double>(t);
  const double fr = td - static_cast<double>(static_cast<long long>(td));
  if (fr > 0.5) ++i;
  return i;
}

// Small values flushed to zero as the reference does.
inline void flush(float& v) {
  if (std::fabs(v) < 1e-14f) v = 0;
}

struct Params {
  const PluginState* s;
  const TurntableLayout& L;
  double operator[](int k) const {
    if (k < 0) return 0.0;  // a setting this type does not have
    if (s && static_cast<size_t>(k) < s->params.size() && std::isfinite(s->params[static_cast<size_t>(k)]))
      return s->params[static_cast<size_t>(k)];
    const PluginInfo* info = findPlugin(L.type);
    return info && static_cast<size_t>(k) < info->params.size() ? info->params[static_cast<size_t>(k)].def : 0.0;
  }
};
float clampf(double v, double lo, double hi) { return static_cast<float>(std::clamp(v, lo, hi)); }

// The deck (K-23: shared by the effect and the 0.7.1 instrument). render()
// adds the deck to L / R: the effect hands it the insert's audio, so the input
// passes through and the deck plays on top, as the reference does.
class TurntableCore {
 public:
  explicit TurntableCore(const TurntableLayout& layout) : lay_(layout) {}

  bool active() const {
    if (!deck_) return false;
    return keys_ > 0 || hold_ || (transport_ != 0 && speed_ > 0) || vel_ != 0.0f || std::fabs(dcy_[0]) > 1e-7f ||
           std::fabs(dcy_[1]) > 1e-7f || fadeLeft_ > 0;
  }
  const TurntableTelemetry& tel() const { return tel_; }

  void onNoteOn(uint32_t id, double pitch) {
    if (notesMode_ == TnOff) return;
    removeKey(id);  // last-note priority over a small stack of held keys
    if (keys_ < static_cast<int>(keyIds_.size())) {
      keyIds_[static_cast<size_t>(keys_)] = id;
      keyPitch_[static_cast<size_t>(keys_)] = pitch;
      ++keys_;
    }
    if (deck_ && deck_->src) tap::play(deck_->src.get());  // J-219: Track Receipt
    if (notesMode_ == TnCue && deck_) {
      // Start at the cue at full speed: the old read position fades out under the new one.
      fadeFrom_ = pos_;
      fadeVel_ = vel_;
      fadeLeft_ = fadeLen_;
      pos_ = cue_;
      vel_ = keyTarget(pitch);
    }
  }
  void onNoteOff(uint32_t id) { removeKey(id); }
  void onAllOff() { keys_ = 0; }
  void onKill() {
    keys_ = 0;
    g_ = 0;
    vel_ = 0;
    travel_ = 0;
    dcx_ = {0, 0};
    dcy_ = {0, 0};
    fadeLeft_ = 0;
  }

  void configure(const PluginState& state, const ChannelAux& aux, double sampleRate) {
    Params p{&state, lay_};
    sr_ = sampleRate > 0 && std::isfinite(sampleRate) ? sampleRate : kRefRate;
    auto* d = dynamic_cast<const TurntableDeck*>(aux.custom.get());
    if (d && d->frames < 2) d = nullptr;
    // A new sample keeps the platter's phase (the reference swaps it under the running platter).
    // The aux keeps the deck alive for this process() call; only the pointer is kept.
    if (d != deck_) fadeLeft_ = 0;
    deck_ = d;
    // A control change ends the reference's host block there: the targets are
    // taken (from the values before the change) and the block clock restarts.
    bool changed = false;
    const bool turnsAlone = p[TtTurns] != last_[TtTurns] && p[TtPosition] == last_[TtPosition];
    for (int k = 0; k < lay_.count && k < TtCount; ++k) {
      const double v = p[k];
      if (v != last_[static_cast<size_t>(k)]) changed = true;
      last_[static_cast<size_t>(k)] = v;
    }
    if (changed && started_) latch(posParam_, hold_ ? turns_ : 0);
    // Internal values, as the reference holds them.
    posParam_ = clampf(p[TtPosition], 0, 1);
    sound_ = p[TtMute] < 0.5;
    speed_ = clampf(p[TtSpeed], 0, 2) * 0.5f;
    accel_ = clampf(p[TtAccel], 0, 1);
    panParam_ = clampf(p[TtPan], -1, 1);
    volume_ = clampf(p[TtVolume], 0, 1);
    hold_ = p[TtHold] >= 0.5;
    transport_ = static_cast<int>(std::lround(std::clamp(p[TtTransport], 0.0, 2.0))) - 1;
    for (int k = 0; k < 4; ++k) smooth_[static_cast<size_t>(k)] = clampf(p[TtSmooth1 + k], 0, 1);
    turns_ = static_cast<int>(std::lround(std::clamp(p[TtTurns], -9.0, 9.0)));
    if (hold_ && turnsAlone) turnsTarget_ = turns_;  // (Position stays: they act at once)
    notesMode_ = lay_.notes < 0 ? TnOff : static_cast<int>(std::lround(std::clamp(p[lay_.notes], 0.0, 2.0)));
    keyTrack_ = lay_.keyTrack >= 0 && p[lay_.keyTrack] >= 0.5;
    cue_ = clampf(p[lay_.cue], 0, 1);
    fadeLen_ = std::max(1, static_cast<int>(0.004 * sr_));
    latchLen_ = std::max(1, static_cast<int>(std::lround(kLatchFrames * sr_ / kRefRate)));
    if (!started_) {  // first block: the targets start where the controls are
      // (the pan glides from the centre as the reference's does; the level
      // fades in from silence: the reference's first block with a sample is
      // a cut from its constructor level instead)
      started_ = true;
      latchCount_ = 0;
      posTarget_ = posParam_;
      turnsTarget_ = hold_ ? turns_ : 0;
      panTarget_ = panParam_;
      pan_ = 0.0f;
      gTarget_ = sound_ ? volume_ : 0.0f;
    }
    // One revolution per (frames - 1) deck samples at the sample's own speed.
    if (deck_) unit2_ = static_cast<float>(1.0 / static_cast<double>(deck_->frames - 1)) * 2.0f;
    rate_ = kRefRate / sr_;
    exactRate_ = std::fabs(sr_ - kRefRate) < 1e-6;
  }

  void render(float* L, float* R, int n) {
    if (!deck_) {
      publish();
      return;
    }
    const TurntableDeck& d = *deck_;
    const float N = static_cast<float>(static_cast<long long>(d.frames));
    // J-219: Track Receipt coverage (deck frames are 44.1 kHz frames of the file).
    tap::Run run;
    if (tap::active() && d.src) run.begin(d.src.get());
    const double toSrc = d.src ? d.src->sampleRate / 44100.0 : 1.0;
    // Output smoothers (one-poles defined per 44.1 kHz sample).
    double ga = 0.99, gb = 0.01, dca = 0.99;
    if (!exactRate_) {
      ga = dca = std::pow(0.99, rate_);
      gb = 1.0 - ga;
    }
    const bool keyMotor = keys_ > 0;
    if (hold_) {
      // Hand: the damped spring toward Position + Turns.
      const float s1 = smooth_[0], s2 = smooth_[1], s3 = smooth_[2];
      float s4 = smooth_[3];
      const int ns = refIndex(s1 * 2500.0f) + 250;  // the reference's rounding of s1 x 2500
      float a2 = static_cast<float>(static_cast<double>(s2) * 0.0099 + 0.99);
      const float a3 = static_cast<float>(static_cast<double>(s3) * 0.00999 + 0.99);
      float k = (1.0f - a2) / static_cast<float>(ns);
      float c = (1.0f - a3) * (1.0f - s4);
      if (!exactRate_) {
        const float a2r = static_cast<float>(std::pow(static_cast<double>(a2), rate_));
        if (a2 < 1.0f) k = k * (1.0f - a2r) / (1.0f - a2);
        a2 = a2r;
        c = static_cast<float>(c * rate_);
        s4 = static_cast<float>(s4 * rate_);
      }
      for (int i = 0; i < n; ++i) {
        const float err = (posTarget_ + static_cast<float>(turnsTarget_)) - pos_;
        vel_ = k * err + a2 * vel_;
        const float step = err * c + s4 * vel_;
        pos_ = pos_ + step;
        travel_ = step;
        float ph = wrapped(pos_);
        float l, r;
        run.at(static_cast<double>(N * ph) * toSrc);
        readAt(d, N, ph, l, r);
        out(L, R, i, l, r, ga, gb, dca);
        tick(false);
      }
      wasHold_ = true;
    } else {
      if (wasHold_) {  // let go: the hand leaves the platter where it is (unwrapped turns fold back)
        wasHold_ = false;
        if (!(std::fabs(pos_) < 1e6f)) pos_ = 0;
      }
      const int T = keyMotor ? 1 : transport_;
      const float sp = keyMotor ? keySpeed(keyPitch_[static_cast<size_t>(keys_ - 1)]) : speed_;
      float vt = static_cast<float>(T) * (unit2_ * sp);
      float a = static_cast<float>(0.99999 - static_cast<double>(accel_) * 0.0005);
      if (!exactRate_) {
        vt = static_cast<float>(vt * rate_);
        a = static_cast<float>(std::pow(static_cast<double>(a), rate_));
      }
      for (int i = 0; i < n; ++i) {
        vel_ = (vel_ - vt) * a + vt;
        flush(vel_);
        pos_ = wrapped(pos_);
        const float x = N * pos_;
        const int ix = refIndex(x);
        pos_ = pos_ + vel_;
        travel_ = vel_;
        const float t = x - static_cast<float>(ix);
        float l, r;
        run.at(static_cast<double>(x) * toSrc);
        frameAt(d, ix, t, l, r);
        if (fadeLeft_ > 0) fade(d, N, l, r);
        out(L, R, i, l, r, ga, gb, dca);
        tick(true);
      }
      posParam_ = pos_;  // the motor writes the position back (an automated Position has no effect here)
    }
    publish();
  }

 private:
  // The reference takes its targets (position, pan, level) at the end of each
  // host block. Here they are taken every kLatchFrames samples of the deck's
  // own clock (a fixed host block of 512 at 44.1 kHz) and at every control
  // change, so the sound does not depend on how the host splits its blocks. `motor`: the motor writes the
  // platter's position into the position control as it runs.
  void tick(bool motor) {
    if (++latchCount_ < latchLen_) return;
    if (motor) latch(pos_, 0);
    else latch(posParam_, turns_);
  }
  // A hand crossing the sample's start changes Position and Turns at once (0.001 + 0 turns to
  // 0.999 - 1 turn): they are taken together, one without the other is a target a whole turn away.
  void latch(float pos, int turns) {
    latchCount_ = 0;
    posTarget_ = pos;
    turnsTarget_ = turns;
    panTarget_ = panParam_;
    gTarget_ = sound_ ? volume_ : 0.0f;
  }
  // Wrap into [0, 1] the way the reference does (whole turns off, 1.0 stays 1.0).
  static float wrapped(float p) {
    if (!(std::fabs(p) < 1e6f)) return 0.0f;  // non-finite or runaway: restart at the top
    while (p > 1.0f) p -= 1.0f;
    while (p < 0.0f) p += 1.0f;
    return p;
  }
  static void frameAt(const TurntableDeck& d, int i, float t, float& l, float& r) {
    if (i < 0) i = 0;
    if (i > d.frames) i = d.frames;
    float l0, r0, l1, r1;
    if (i < d.frames) {
      l0 = q16(d.L[i]);
      r0 = q16(d.R[i]);
    } else {
      l0 = r0 = q16(d.R[0]);
    }
    if (i + 1 < d.frames) {
      l1 = q16(d.L[i + 1]);
      r1 = q16(d.R[i + 1]);
    } else {
      l1 = r1 = q16(d.R[0]);
    }
    l = (l1 - l0) * t + l0;
    r = (r1 - r0) * t + r0;
  }
  static void readAt(const TurntableDeck& d, float N, float ph, float& l, float& r) {
    const float x = N * ph;
    const int ix = refIndex(x);
    frameAt(d, ix, x - static_cast<float>(ix), l, r);
  }
  // Cue jump crossfade: the old position keeps running under the new one for a few ms.
  void fade(const TurntableDeck& d, float N, float& l, float& r) {
    fadeFrom_ = wrapped(fadeFrom_ + fadeVel_);
    float fl, fr;
    readAt(d, N, fadeFrom_, fl, fr);
    const float t = static_cast<float>(fadeLeft_) / static_cast<float>(fadeLen_);
    l = l * (1.0f - t) + fl * t;
    r = r * (1.0f - t) + fr * t;
    --fadeLeft_;
  }
  // DC blocker, volume / mute and pan smoothing, pan law (one side turned down).
  void out(float* L, float* R, int i, float l, float r, double ga, double gb, double dca) {
    dcy_[0] = static_cast<float>(static_cast<double>(dcy_[0]) * dca + static_cast<double>(l - dcx_[0]));
    dcx_[0] = l;
    flush(dcy_[0]);
    dcy_[1] = static_cast<float>(static_cast<double>(dcy_[1]) * dca + static_cast<double>(r - dcx_[1]));
    dcx_[1] = r;
    flush(dcy_[1]);
    g_ = static_cast<float>(static_cast<double>(g_) * ga + static_cast<double>(gTarget_) * gb);
    flush(g_);
    const float yl = dcy_[0] * g_, yr = dcy_[1] * g_;
    pan_ = static_cast<float>(static_cast<double>(pan_) * ga + static_cast<double>(panTarget_) * gb);
    flush(pan_);
    if (pan_ > 0.0f) {
      L[i] += (1.0f - pan_) * yl;
      R[i] += yr;
    } else {
      L[i] += yl;
      R[i] += (1.0f + pan_) * yr;
    }
  }
  float keySpeed(double pitch) const {
    const double key = keyTrack_ ? std::pow(2.0, (pitch - 60.0) / 12.0) : 1.0;
    return static_cast<float>(std::min(8.0, static_cast<double>(speed_) * key));
  }
  float keyTarget(double pitch) const {
    float v = unit2_ * keySpeed(pitch);
    if (!exactRate_) v = static_cast<float>(v * rate_);
    return v;
  }
  void removeKey(uint32_t id) {
    for (int i = 0; i < keys_; ++i)
      if (keyIds_[static_cast<size_t>(i)] == id) {
        for (int j = i + 1; j < keys_; ++j) {
          keyIds_[static_cast<size_t>(j - 1)] = keyIds_[static_cast<size_t>(j)];
          keyPitch_[static_cast<size_t>(j - 1)] = keyPitch_[static_cast<size_t>(j)];
        }
        --keys_;
        return;
      }
  }
  void publish() {
    const float ph = wrapped(pos_);
    tel_.pos.store(ph, std::memory_order_relaxed);
    const float unit = unit2_ * 0.5f;
    float sp = unit > 0 ? travel_ / unit : 0.0f;
    if (!exactRate_ && rate_ > 0) sp = static_cast<float>(sp / rate_);
    tel_.speed.store(std::isfinite(sp) ? sp : 0.0f, std::memory_order_relaxed);
    tel_.gain.store(g_, std::memory_order_relaxed);
    tel_.held.store(hold_, std::memory_order_relaxed);
    tel_.loaded.store(deck_ != nullptr, std::memory_order_relaxed);
  }

  const TurntableDeck* deck_ = nullptr;
  double sr_ = kRefRate, rate_ = 1.0;
  bool exactRate_ = true, started_ = false;
  // controls (internal values)
  float posParam_ = 0, speed_ = 0.5f, accel_ = 0.25f, panParam_ = 0, volume_ = 0.78f, cue_ = 0;
  std::array<float, 4> smooth_{0.5f, 0.908f, 0.999f, 0.4f};
  bool sound_ = true, hold_ = false, keyTrack_ = true, wasHold_ = false;
  int transport_ = 0, turns_ = 0, notesMode_ = TnCue;
  float unit2_ = 0;
  // platter and output
  float pos_ = 0, vel_ = 0, travel_ = 0, posTarget_ = 0;
  int turnsTarget_ = 0;
  float g_ = 0, gTarget_ = 0, pan_ = 0, panTarget_ = 0;
  std::array<float, 2> dcx_{0, 0}, dcy_{0, 0};
  // cue jump crossfade
  float fadeFrom_ = 0, fadeVel_ = 0;
  int fadeLeft_ = 0, fadeLen_ = 176;
  // target latching
  int latchCount_ = 0, latchLen_ = kLatchFrames;
  std::array<double, TtCount> last_{};
  // held keys
  std::array<uint32_t, 16> keyIds_{};
  std::array<double, 16> keyPitch_{};
  int keys_ = 0;
  TurntableTelemetry tel_;
  const TurntableLayout& lay_;
};

// The 0.7.1 instrument: the deck played by notes (and its motor).
class TurntableGen final : public Generator {
 public:
  bool active() const override { return core_.active(); }
  const TurntableTelemetry& tel() const { return core_.tel(); }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    (void)velocity;
    core_.onNoteOn(id, pitch);
  }
  void onNoteOff(uint32_t id) override { core_.onNoteOff(id); }
  void onAllOff() override { core_.onAllOff(); }
  void onKill() override { core_.onKill(); }
  void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) override {
    core_.configure(state, aux, ctx.sampleRate);
  }
  void render(float* L, float* R, int n) override { core_.render(L, R, n); }

 private:
  TurntableCore core_{kTtInstLayout};
};

// K-23: Turntable, the mixer effect. The insert's audio is in L / R; the deck
// is added to it (the reference's output is its input plus the deck, exactly).
// No prepared deck (process() without aux): the input passes unchanged.
class TurntableFx final : public Effect {
 public:
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    processAux(state, none_, ctx, L, R, n);
  }
  void processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (n <= 0) return;
    core_.configure(state, aux, ctx.sampleRate);
    core_.render(L, R, n);
  }
  void reset() override { core_.onKill(); }
  const TurntableTelemetry& tel() const { return core_.tel(); }

 private:
  TurntableCore core_{kTtFxLayout};
  const ChannelAux none_{};  // no deck
};

// ---------------------------------------------------------------- built-in sound
// One bar at 92 BPM: a vocal-like "ah" stab and a kick on the downbeat, snares
// on 2 and 4, a pushed kick, eighth-note hats and a lower "oh" stab on beat 3.
// Everything is synthesised here (fixed noise seed: the same sound every time).
SamplePtr buildBuiltin() {
  const double sr = kRefRate;
  const double beat = 60.0 / 92.0;
  const size_t n = static_cast<size_t>(std::lround(4.0 * beat * sr));
  auto s = std::make_shared<Sample>();
  s->path = "tone:turntable";
  s->sampleRate = sr;
  s->left.assign(n, 0.f);
  std::vector<double> mix(n, 0.0);
  uint32_t seed = 0x2545F491u;
  auto noise = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
  };
  auto at = [&](double b) { return static_cast<size_t>(std::lround(b * beat * sr)); };
  auto kick = [&](double b, double amp) {
    double ph = 0;
    for (size_t i = at(b), k = 0; i < n && k < static_cast<size_t>(0.35 * sr); ++i, ++k) {
      const double t = static_cast<double>(k) / sr;
      const double f = 45.0 + 110.0 * std::exp(-t * 28.0);
      ph += f / sr;
      mix[i] += amp * std::sin(6.283185307179586 * ph) * std::exp(-t * 9.0);
    }
  };
  auto snare = [&](double b, double amp) {
    dsp::Biquad bp;
    bp.set(dsp::Biquad::BandPass, sr, 1900, 0.9, 0);
    double ph = 0;
    for (size_t i = at(b), k = 0; i < n && k < static_cast<size_t>(0.25 * sr); ++i, ++k) {
      const double t = static_cast<double>(k) / sr;
      ph += 185.0 / sr;
      mix[i] += amp * (bp.process(noise()) * 1.4 * std::exp(-t * 18.0) + 0.35 * std::sin(6.283185307179586 * ph) * std::exp(-t * 30.0));
    }
  };
  auto hat = [&](double b, double amp) {
    double prev = 0;
    for (size_t i = at(b), k = 0; i < n && k < static_cast<size_t>(0.06 * sr); ++i, ++k) {
      const double t = static_cast<double>(k) / sr;
      const double x = noise();
      mix[i] += amp * (x - prev) * 0.5 * std::exp(-t * 70.0);
      prev = x;
    }
  };
  auto voice = [&](double b, double dur, double f0, double f1, double fa, double fb, double amp) {
    dsp::Biquad b1, b2;
    b1.set(dsp::Biquad::BandPass, sr, fa, 5, 0);
    b2.set(dsp::Biquad::BandPass, sr, fb, 7, 0);
    double ph = 0;
    const size_t a = at(b), e = std::min(n, a + static_cast<size_t>(dur * sr));
    for (size_t i = a; i < e; ++i) {
      const double u = static_cast<double>(i - a) / static_cast<double>(e - a);
      ph += (f0 + (f1 - f0) * u) / sr;
      ph -= std::floor(ph);
      const double env = std::min(1.0, u * 30.0) * std::pow(1.0 - u, 1.2);
      mix[i] += amp * env * (b1.process(2.0 * ph - 1.0) * 1.7 + b2.process(2.0 * ph - 1.0) * 1.0);
    }
  };
  voice(0.0, 0.42, 230, 170, 800, 1220, 1.0);
  kick(0.0, 0.9);
  hat(0.5, 0.25);
  snare(1.0, 0.7);
  hat(1.5, 0.25);
  kick(1.75, 0.7);
  voice(2.0, 0.40, 170, 130, 560, 950, 0.9);
  kick(2.0, 0.8);
  hat(2.5, 0.25);
  snare(3.0, 0.7);
  hat(3.5, 0.25);
  hat(3.75, 0.18);
  double peak = 1e-9;
  for (double v : mix) peak = std::max(peak, std::fabs(v));
  const double g = 0.8 / peak;
  for (size_t i = 0; i < n; ++i) s->left[i] = q16(static_cast<float>(mix[i] * g));
  s->setMono();
  return s;
}

}  // namespace

SamplePtr turntableBuiltinSample() {
  static std::once_flag once;
  static SamplePtr smp;
  std::call_once(once, [] { smp = buildBuiltin(); });
  return smp;
}

SamplePtr turntableSample(const PluginState& state) {
  if (!state.data.empty()) return sampleCache().get(state.data);
  const TurntableLayout* L = turntableLayout(state.type);
  if (!L) L = &kTtFxLayout;
  const size_t k = static_cast<size_t>(L->empty);
  const PluginInfo* info = findPlugin(L->type);
  const float empty = k < state.params.size() ? state.params[k] : (info && k < info->params.size() ? static_cast<float>(info->params[k].def) : 0.f);
  return empty >= 0.5f ? nullptr : turntableBuiltinSample();
}

const TurntableTelemetry* turntableTelemetry(const Generator* g) {
  const auto* t = dynamic_cast<const TurntableGen*>(g);
  return t ? &t->tel() : nullptr;
}
const TurntableTelemetry* turntableTelemetry(const Effect* e) {
  const auto* t = dynamic_cast<const TurntableFx*>(e);
  return t ? &t->tel() : nullptr;
}

std::unique_ptr<Generator> createTurntable() { return std::make_unique<TurntableGen>(); }
std::unique_ptr<Effect> createTurntableFx() { return std::make_unique<TurntableFx>(); }

bool prepareTurntableAux(const PluginState& state, ChannelAux& aux) {
  if (!turntableLayout(state.type)) return false;
  aux = ChannelAux{};
  SamplePtr s = turntableSample(state);
  aux.samples.push_back(s);
  if (s && s->frames() >= 2) aux.custom = deckFor(s);
  return true;
}

PluginState turntableFxOfInstrument(const PluginState& inst) {
  PluginState fx = defaultPluginState(kTurntableFx);
  fx.data = inst.data;
  auto at = [&](int k) -> float {
    const PluginInfo* info = findPlugin(kTurntableInst);
    if (static_cast<size_t>(k) < inst.params.size() && std::isfinite(inst.params[static_cast<size_t>(k)])) return inst.params[static_cast<size_t>(k)];
    return info && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
  };
  if (fx.params.size() < static_cast<size_t>(TxCount)) return fx;
  for (int k = TtPosition; k <= TtTurns; ++k) fx.params[static_cast<size_t>(k)] = at(k);
  fx.params[TxTimecode] = at(TtTimecode);
  fx.params[TxRiaa] = at(TtRiaa);
  fx.params[TxZoom] = at(TtZoom);
  fx.params[TxEmpty] = at(TtEmpty);
  return fx;
}

void turntableFromReference(const float* v, int n, std::vector<float>& params, const TurntableLayout& L) {
  params.assign(static_cast<size_t>(L.count), 0.f);
  const PluginInfo* info = findPlugin(L.type);
  if (info)
    for (int i = 0; i < L.count && static_cast<size_t>(i) < info->params.size(); ++i)
      params[static_cast<size_t>(i)] = static_cast<float>(info->params[static_cast<size_t>(i)].def);
  auto at = [&](int i, float d) { return i < n && std::isfinite(v[i]) ? v[i] : d; };
  params[TtPosition] = std::clamp(at(0, 0.f), 0.f, 1.f);
  params[TtMute] = at(1, 1.f) != 0.0f ? 0.f : 1.f;  // stored: sound on (any non-zero value)
  params[TtSpeed] = std::clamp(at(2, 0.5f), 0.f, 1.f) * 2.f;
  params[TtAccel] = std::clamp(at(3, 0.25f), 0.f, 1.f);
  params[TtPan] = std::clamp(at(4, 0.5f), 0.f, 1.f) * 2.f - 1.f;
  params[TtVolume] = std::clamp(at(5, 0.78f), 0.f, 1.f);
  params[TtHold] = at(6, 0.f) != 0.0f ? 1.f : 0.f;
  params[TtSense] = std::clamp(at(7, 0.5f), 0.f, 1.f);
  // Stored -1 / 0 / 1 (truncated); the selector cell is that + 1.
  const float tr = std::clamp(at(8, 0.f), -1.f, 1.f);
  params[TtTransport] = static_cast<float>(static_cast<int>(tr) + 1);
  for (int k = 0; k < 4; ++k) params[static_cast<size_t>(TtSmooth1 + k)] = std::clamp(at(9 + k, 0.5f), 0.f, 1.f);
  const float rot = std::clamp(at(13, 9.f), 0.f, 18.f);
  params[TtTurns] = static_cast<float>(static_cast<int>(rot) - 9);
  if (L.notes >= 0) params[static_cast<size_t>(L.notes)] = static_cast<float>(TnOff);  // the reference plays by its motor, not by notes
}

int turntableParamOfReference(int tag) {
  static const int map[14] = {TtPosition, TtMute, TtSpeed, TtAccel, TtPan, TtVolume, TtHold,
                              TtSense, TtTransport, TtSmooth1, TtSmooth2, TtSmooth3, TtSmooth4, TtTurns};
  return tag >= 0 && tag < 14 ? map[tag] : -1;
}

double turntableValueOfReference(int tag, double n) {
  // The reference sets its control to the nearest step of the control's range
  // and plays that step (measured: volume at a quarter plays 64 / 255).
  n = std::isfinite(n) ? std::clamp(n, 0.0, 1.0) : 0.0;
  auto step = [n](double steps) { return std::nearbyint(n * steps) / steps; };
  switch (tag) {
    case 0: return step(30000);                                      // position 0..30000
    case 1: case 6: return n >= 0.5 ? 1.0 : 0.0;                     // mute, hold
    case 2: return step(100) * 2.0;                                  // speed 0..100 (50 = the sample's own speed)
    case 3: case 7: return step(100);                                // acceleration, sensitivity
    case 4: return step(256) * 2.0 - 1.0;                            // pan -128..128
    case 5: return step(255);                                        // volume 0..255
    case 8: return static_cast<double>(std::lround(n * 2.0));        // motor cell
    case 9: case 10: case 11: case 12: return step(1000);            // smooth 0..1000
    case 13: return static_cast<double>(std::lround(n * 18.0) - 9);  // rotations 0..18 = turns -9..9
    default: return n;
  }
}

void turntableToReference(const std::vector<float>& params, float* v) {
  const PluginInfo* info = findPlugin("gl.turntable");
  auto P = [&](int k) {
    if (static_cast<size_t>(k) < params.size() && std::isfinite(params[static_cast<size_t>(k)])) return params[static_cast<size_t>(k)];
    return info ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
  };
  v[0] = std::clamp(P(TtPosition), 0.f, 1.f);
  v[1] = P(TtMute) >= 0.5f ? 0.f : 1.f;
  v[2] = std::clamp(P(TtSpeed), 0.f, 2.f) * 0.5f;
  v[3] = std::clamp(P(TtAccel), 0.f, 1.f);
  v[4] = (std::clamp(P(TtPan), -1.f, 1.f) + 1.f) * 0.5f;
  v[5] = std::clamp(P(TtVolume), 0.f, 1.f);
  v[6] = P(TtHold) >= 0.5f ? 1.f : 0.f;
  v[7] = std::clamp(P(TtSense), 0.f, 1.f);
  v[8] = static_cast<float>(std::lround(std::clamp(P(TtTransport), 0.f, 2.f)) - 1);
  for (int k = 0; k < 4; ++k) v[9 + k] = std::clamp(P(TtSmooth1 + k), 0.f, 1.f);
  v[13] = static_cast<float>(std::lround(std::clamp(P(TtTurns), -9.f, 9.f)) + 9);
}

}  // namespace gl::packs::spectral
