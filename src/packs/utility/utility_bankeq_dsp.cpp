// Gridline — "utility" pack: Morph EQ (gl.bankeq) runtime and curve helpers.
//
// Signal flow (per channel): 31 parallel band-passes (constant 0 dB peak,
// centres 20·2^(i/3) Hz, bandwidth in octaves) → each band output times its
// level gain and pan factor → summed and normalised (bqNorm). Add mode keeps
// the dry signal and adds band·(gain − 1). The send curve uses the same band
// outputs with its own gains (and the band's pan) and is summed into the
// destination insert handed over by the engine (SideIo::multiL/R[0]). Optional
// 2x oversampling (half-band FIR) around the whole bank.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include "../../engine/Dsp.h"
#include "utility_rt.h"

namespace gl::packs::utility {

void bankEffective(const float* p, double shiftOct, double mix, double morph, BankFrame& o) {
  morph = std::clamp(morph, 0.0, static_cast<double>(kBqBanks - 1));
  const int k0 = std::min(static_cast<int>(morph), kBqBanks - 2);
  const double tm = morph - k0;
  const int edit = std::clamp(static_cast<int>(std::lround(p[BqEditBank])), 0, kBqBanks - 1);
  const double m = std::clamp(mix, -1.0, 1.0);
  for (int t = 0; t < kBqTargets; ++t)
    for (int b = 0; b < kBqBands; ++b) {
      int mode = std::clamp(static_cast<int>(std::lround(p[bqMode(t, b)])), 0, 2);
      double v;
      if (mode == BmLocked) {
        v = p[bqValue(edit, t, b)];
      } else {
        const double x = b - 3.0 * shiftOct;
        double va = bqAt(p, k0, t, x), vb = bqAt(p, k0 + 1, t, x);
        va += (1.0 - va) * (1.0 - m);
        vb += (1.0 - vb) * (1.0 - m);
        v = va + (vb - va) * tm;
      }
      v = std::clamp(v, 0.0, 2.0);
      o.v[t][b] = static_cast<float>(v);
      o.mode[t][b] = static_cast<uint8_t>(mode);
      const double master = mode == BmLocked ? 1.0 : std::clamp(static_cast<double>(p[bqValue(kBqMaster, t, b)]), 0.0, 2.0);
      if (t == BtPan) {
        o.pan[b] = mode == BmOff ? 0.f : static_cast<float>(std::clamp((v - 1.0) + (master - 1.0), -1.0, 1.0));
        o.gain[t][b] = 0;
      } else {
        o.gain[t][b] = mode == BmOff ? 0.f : static_cast<float>(bqGain(v) * bqGain(master));
      }
    }
}

double bankResponse(double hz, double sr, double bwOct, const double* g, bool add) {
  std::complex<double> sum(add ? 1.0 : 0.0, 0.0);
  const double w = 2 * 3.14159265358979323846 * hz / sr;
  const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
  for (int b = 0; b < kBqBands; ++b) {
    double f = bqBandHz(b);
    if (f >= sr * 0.5 || g[b] == 0) continue;
    Coefs c = bqBandCoefs(sr, f, bwOct);
    std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    sum += h * g[b];
  }
  return std::abs(sum);
}

namespace {

// 2x half-band oversampler (linear-phase windowed sinc, 63 taps at the high rate).
class Halfband2x {
 public:
  static constexpr int kTaps = 63;
  static constexpr int kLatency = (kTaps - 1) / 2;  // in high-rate samples, per filter
  Halfband2x() {
    static const double* k = kernel();
    h_ = k;
  }
  void clear() {
    std::memset(up_, 0, sizeof up_);
    std::memset(dn_, 0, sizeof dn_);
    pu_ = pd_ = 0;
  }
  // One base-rate sample → two high-rate samples.
  void up(double x, double out[2]) {
    for (int ph = 0; ph < 2; ++ph) {
      double u = ph == 0 ? 2.0 * x : 0.0;
      pu_ = (pu_ + kTaps - 1) % kTaps;
      up_[pu_] = up_[pu_ + kTaps] = u;
      double y = 0;
      const double* hist = up_ + pu_;
      for (int j = 0; j < kTaps; ++j) y += h_[j] * hist[j];
      out[ph] = y;
    }
  }
  // Two high-rate samples → one base-rate sample.
  double down(const double in[2]) {
    for (int ph = 0; ph < 2; ++ph) {
      pd_ = (pd_ + kTaps - 1) % kTaps;
      dn_[pd_] = dn_[pd_ + kTaps] = in[ph];
    }
    double y = 0;
    const double* hist = dn_ + pd_;
    for (int j = 0; j < kTaps; ++j) y += h_[j] * hist[j];
    return y;
  }
  void flush() {
    for (double& v : up_)
      if (std::fabs(v) < 1e-25) v = 0;
    for (double& v : dn_)
      if (std::fabs(v) < 1e-25) v = 0;
  }

 private:
  static const double* kernel() {
    static double h[kTaps];
    const double pi = 3.14159265358979323846;
    double sum = 0;
    for (int j = 0; j < kTaps; ++j) {
      double n = j - (kTaps - 1) * 0.5;
      double s = n == 0 ? 0.5 : std::sin(pi * 0.5 * n) / (pi * n);  // cutoff 1/4 of the high rate
      double w = 0.42 - 0.5 * std::cos(2 * pi * j / (kTaps - 1)) + 0.08 * std::cos(4 * pi * j / (kTaps - 1));
      h[j] = s * w;
      sum += h[j];
    }
    for (double& v : h) v /= sum;
    return h;
  }
  const double* h_ = nullptr;
  double up_[2 * kTaps] = {}, dn_[2 * kTaps] = {};
  int pu_ = 0, pd_ = 0;
};

class BankEqFx final : public UtilityFx {
 public:
  BankEqFx() { reset(); }
  void reset() override {
    std::memset(z1_, 0, sizeof z1_);
    std::memset(z2_, 0, sizeof z2_);
    for (auto& o : os_) o.clear();
    primed_ = false;
    left_ = 0;
    designedSr_ = designedBw_ = -1;
  }
  void setSideIo(const SideIo& io) override {
    sendL_ = io.multiL[0];
    sendR_ = io.multiR[0];
  }
  int latencyFor(const PluginState& s, double) const override {
    return s.params.size() > static_cast<size_t>(BqOversample) && s.params[BqOversample] >= 0.5f ? Halfband2x::kLatency : 0;
  }
  BandTelemetry* bandTelemetry() override { return &tel_; }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    float* sL = sendL_;
    float* sR = sendR_;
    sendL_ = sendR_ = nullptr;  // valid for this call only
    if (s.params.size() < static_cast<size_t>(kBqCount)) return;
    const float* p = s.params.data();
    const double sr = ctx.sampleRate;
    const bool ovs = p[BqOversample] >= 0.5f;
    if (ovs != ovs_) {
      ovs_ = ovs;
      std::memset(z1_, 0, sizeof z1_);
      std::memset(z2_, 0, sizeof z2_);
      for (auto& o : os_) o.clear();
      latency_.store(ovs ? Halfband2x::kLatency : 0, std::memory_order_relaxed);
    }
    const double procSr = sr * (ovs ? 2 : 1);
    const double bw = std::clamp(static_cast<double>(p[BqBandwidth]), 80.0 / 300.0, 500.0 / 300.0);
    if (procSr != designedSr_ || bw != designedBw_) design(procSr, bw);
    const bool add = p[BqAddMode] >= 0.5f, sendOnly = p[BqSendOnly] >= 0.5f;
    const bool sending = sL && sR;
    const bool capture = tel_.capture.load(std::memory_order_relaxed);
    if (tel_.reset.exchange(false, std::memory_order_acq_rel)) {
      std::memset(acc_, 0, sizeof acc_);
      accFrames_ = 0;
    }
    for (int i = 0; i < n; ++i) {
      if (left_ == 0) {
        startSub(p, sr, add, sending);
        left_ = kSub;
      }
      --left_;
      for (int b = 0; b < kBqBands; ++b) {
        cL_[b] += dL_[b];
        cR_[b] += dR_[b];
        sgL_[b] += dsL_[b];
        sgR_[b] += dsR_[b];
      }
      out_ += dOut_;
      const double xl = L[i], xr = R[i];
      double ml, mr, el = 0, er = 0;
      if (ovs) {
        double ul[2], ur[2], oml[2], omr[2], osl[2], osr[2];
        os_[0].up(xl, ul);
        os_[1].up(xr, ur);
        for (int ph = 0; ph < 2; ++ph) bank(ul[ph], ur[ph], add, sending, capture, oml[ph], omr[ph], osl[ph], osr[ph]);
        ml = os_[0].down(oml);
        mr = os_[1].down(omr);
        if (sending) {
          el = os_[2].down(osl);
          er = os_[3].down(osr);
        }
      } else {
        bank(xl, xr, add, sending, capture, ml, mr, el, er);
      }
      if (sendOnly) {
        ml = xl;
        mr = xr;
      }
      L[i] = static_cast<float>(ml * out_);
      R[i] = static_cast<float>(mr * out_);
      if (sending) {
        sL[i] += static_cast<float>(el);
        sR[i] += static_cast<float>(er);
      }
    }
    for (int b = 0; b < kBqBands; ++b)
      for (int c = 0; c < 2; ++c) {
        if (std::fabs(z1_[c][b]) < 1e-25) z1_[c][b] = 0;
        if (std::fabs(z2_[c][b]) < 1e-25) z2_[c][b] = 0;
      }
    if (ovs)
      for (auto& o : os_) o.flush();
    if (capture) {
      accFrames_ += n;
      for (int b = 0; b < kBqBands; ++b) tel_.energy[b].store(static_cast<float>(acc_[b]), std::memory_order_relaxed);
      tel_.frames.store(accFrames_, std::memory_order_release);
    }
  }

 private:
  static constexpr int kSub = 32;

  void design(double procSr, double bw) {
    designedSr_ = procSr;
    designedBw_ = bw;
    for (int b = 0; b < kBqBands; ++b) {
      double f = bqBandHz(b);
      active_[b] = f < procSr * 0.5;
      Coefs c = active_[b] ? bqBandCoefs(procSr, f, bw) : Coefs{};
      b0_[b] = c.b0;
      a1_[b] = c.a1;
      a2_[b] = c.a2;
      if (!active_[b]) z1_[0][b] = z1_[1][b] = z2_[0][b] = z2_[1][b] = 0;
    }
    norm_ = bqNorm(bw);
  }

  // Start of a sub-block: smoothing of the motion controls, target gains, ramps.
  void startSub(const float* p, double sr, bool add, bool sending) {
    const double tsh = p[BqShift], tmx = p[BqMix], tmo = p[BqMorph];
    if (!primed_) {
      sh_ = tsh, mx_ = tmx, mo_ = tmo;
      out_ = p[BqOutput] <= -59.99f ? 0.0 : dsp::dbToGain(p[BqOutput]);
    }
    const double t99 = bqSmoothSec(p[BqSmooth]);
    const double a = t99 > 1e-4 ? std::exp(-kSub * std::log(1000.0) / (t99 * sr)) : 0.0;
    sh_ = tsh + (sh_ - tsh) * a;
    mx_ = tmx + (mx_ - tmx) * a;
    mo_ = tmo + (mo_ - tmo) * a;
    bankEffective(p, sh_, mx_, mo_, frame_);
    const double outT = p[BqOutput] <= -59.99f ? 0.0 : dsp::dbToGain(p[BqOutput]);
    for (int b = 0; b < kBqBands; ++b) {
      double tl = 0, tr = 0, ts = 0, tsr = 0;
      if (active_[b]) {
        const double pan = frame_.pan[b], pl = bqPanL(pan), pr = bqPanR(pan);
        const bool off = frame_.mode[BtLevel][b] == BmOff;
        const double g = frame_.gain[BtLevel][b];
        if (add) {
          tl = off ? 0.0 : (g * pl - 1.0) * norm_;
          tr = off ? 0.0 : (g * pr - 1.0) * norm_;
        } else {
          tl = g * pl * norm_;
          tr = g * pr * norm_;
        }
        if (sending) {
          const double gs = frame_.gain[BtSend][b];
          ts = gs * pl * norm_;
          tsr = gs * pr * norm_;
        }
      }
      if (!primed_) {
        cL_[b] = tl, cR_[b] = tr, sgL_[b] = ts, sgR_[b] = tsr;
      }
      dL_[b] = (tl - cL_[b]) / kSub;
      dR_[b] = (tr - cR_[b]) / kSub;
      dsL_[b] = (ts - sgL_[b]) / kSub;
      dsR_[b] = (tsr - sgR_[b]) / kSub;
    }
    dOut_ = (outT - out_) / kSub;
    primed_ = true;
  }

  // One (possibly oversampled) frame through the bank.
  void bank(double xl, double xr, bool add, bool sending, bool capture, double& ml, double& mr, double& el, double& er) {
    double sl = add ? xl : 0.0, sr = add ? xr : 0.0, a = 0, c = 0;
    for (int b = 0; b < kBqBands; ++b) {
      if (!active_[b]) continue;
      const double k0 = b0_[b], k1 = a1_[b], k2 = a2_[b];
      double yl = k0 * xl + z1_[0][b];
      z1_[0][b] = -k1 * yl + z2_[0][b];
      z2_[0][b] = -k0 * xl - k2 * yl;
      double yr = k0 * xr + z1_[1][b];
      z1_[1][b] = -k1 * yr + z2_[1][b];
      z2_[1][b] = -k0 * xr - k2 * yr;
      sl += yl * cL_[b];
      sr += yr * cR_[b];
      if (sending) {
        a += yl * sgL_[b];
        c += yr * sgR_[b];
      }
      if (capture) {
        double mid = 0.5 * (yl + yr);
        acc_[b] += mid * mid;
      }
    }
    ml = sl, mr = sr, el = a, er = c;
  }

  // Filter bank
  double b0_[kBqBands] = {}, a1_[kBqBands] = {}, a2_[kBqBands] = {};
  bool active_[kBqBands] = {};
  double z1_[2][kBqBands] = {}, z2_[2][kBqBands] = {};
  double norm_ = 1, designedSr_ = -1, designedBw_ = -1;
  // Gains (current + per-sample increments)
  double cL_[kBqBands] = {}, cR_[kBqBands] = {}, sgL_[kBqBands] = {}, sgR_[kBqBands] = {};
  double dL_[kBqBands] = {}, dR_[kBqBands] = {}, dsL_[kBqBands] = {}, dsR_[kBqBands] = {};
  double out_ = 1, dOut_ = 0;
  double sh_ = 0, mx_ = 1, mo_ = 0;
  BankFrame frame_{};
  bool primed_ = false, ovs_ = false;
  int left_ = 0;
  Halfband2x os_[4];  // up/down L, up/down R; [2], [3] down-sample the send
  float* sendL_ = nullptr;
  float* sendR_ = nullptr;
  std::atomic<int> latency_{0};
  BandTelemetry tel_;
  double acc_[kBqBands] = {};
  int64_t accFrames_ = 0;
};

}  // namespace

std::unique_ptr<Effect> createBankEq() { return std::make_unique<BankEqFx>(); }

}  // namespace gl::packs::utility
