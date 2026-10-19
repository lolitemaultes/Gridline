// Gridline — "synths" pack: Groove Synth runtime (gl.gms).
//
// Oscillators read band-limited wavetables: every wave (built-in single cycle
// or a single-cycle file) is analysed into 512 harmonics and rebuilt as ten
// tables (512, 256, … 1 harmonics, 2048 samples each); the table is picked so
// no harmonic passes Nyquist. Built-in banks are made once; file banks are
// prepared on the UI thread (packs::prepareAux) and cached by path.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <map>
#include <mutex>
#include "../../engine/Sample.h"
#include "../Packs.h"
#include "gms.h"
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using namespace gms;
using dsp::kTwoPi;

constexpr int kTab = 2048, kTabBits = 11, kLevels = 10, kCycle = 1024, kHarm = 512;
constexpr int kVoices = 16, kMaxUni = 16, kCtl = 16;

// ------------------------------------------------------------ wave banks
struct WaveBank {
  std::array<std::array<float, kTab + 1>, kLevels> t{};
};
using BankPtr = std::shared_ptr<const WaveBank>;

void fft(std::vector<std::complex<double>>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = kTwoPi / static_cast<double>(len) * (inverse ? 1 : -1);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Builds a bank from one cycle (any length; resampled to kCycle).
BankPtr makeBank(const std::vector<float>& cycleIn) {
  auto b = std::make_shared<WaveBank>();
  if (cycleIn.size() < 2) return b;
  std::vector<std::complex<double>> spec(kCycle);
  const double step = static_cast<double>(cycleIn.size()) / kCycle;
  for (int i = 0; i < kCycle; ++i) {
    const double x = i * step;
    const size_t i0 = static_cast<size_t>(x) % cycleIn.size(), i1 = (i0 + 1) % cycleIn.size();
    const double f = x - std::floor(x);
    spec[static_cast<size_t>(i)] = cycleIn[i0] * (1 - f) + cycleIn[i1] * f;
  }
  fft(spec, false);
  // Normalise so the full-band table peaks at 1.
  double peak = 0;
  for (int lv = 0; lv < kLevels; ++lv) {
    const int h = kHarm >> lv;
    std::vector<std::complex<double>> s(kTab, std::complex<double>(0.0, 0.0));
    for (int k = 1; k <= h && k < kCycle / 2; ++k) {
      s[static_cast<size_t>(k)] = spec[static_cast<size_t>(k)];
      s[static_cast<size_t>(kTab - k)] = std::conj(spec[static_cast<size_t>(k)]);
    }
    fft(s, true);
    for (int i = 0; i < kTab; ++i) b->t[static_cast<size_t>(lv)][static_cast<size_t>(i)] = static_cast<float>(s[static_cast<size_t>(i)].real() / kCycle);
    b->t[static_cast<size_t>(lv)][kTab] = b->t[static_cast<size_t>(lv)][0];
    if (lv == 0)
      for (int i = 0; i < kTab; ++i) peak = std::max(peak, static_cast<double>(std::fabs(b->t[0][static_cast<size_t>(i)])));
  }
  if (peak > 1e-9)
    for (auto& t : b->t)
      for (float& v : t) v = static_cast<float>(v / peak);
  return b;
}

std::vector<float> builtinCycle(int w) {
  std::vector<float> c(kCycle);
  for (int i = 0; i < kCycle; ++i) {
    const double x = i / static_cast<double>(kCycle), s = std::sin(kTwoPi * x);
    double v = 0;
    switch (w) {
      case WvSine: v = s; break;
      case WvTriangle: v = x < 0.25 ? 4 * x : x < 0.75 ? 2 - 4 * x : 4 * x - 4; break;
      case WvSaw: v = 1 - 2 * x; break;
      case WvSquare: v = x < 0.5 ? 1 : -1; break;
      case WvPulse25: v = x < 0.25 ? 1 : -0.33; break;
      case WvPulse10: v = x < 0.1 ? 1 : -0.11; break;
      case WvSoftSaw: v = std::tanh(2.5 * (1 - 2 * x)) / std::tanh(2.5); break;
      case WvHollow: v = 0.6 * (x < 0.5 ? 1 : -1) + 0.4 * std::sin(3 * kTwoPi * x); break;
      case WvOrgan: v = s + 0.55 * std::sin(2 * kTwoPi * x) + 0.35 * std::sin(4 * kTwoPi * x) + 0.2 * std::sin(8 * kTwoPi * x); break;
      case WvBell: v = s + 0.6 * std::sin(5 * kTwoPi * x) + 0.35 * std::sin(9 * kTwoPi * x) + 0.2 * std::sin(13 * kTwoPi * x); break;
      case WvSubSaw: v = 0.45 * (1 - 2 * x) + 0.8 * s; break;
      case WvFormant: v = std::sin(6 * kTwoPi * x) * (0.5 - 0.5 * std::cos(kTwoPi * x)) + 0.3 * s; break;
      case WvSpike: {
        const double d = (x - 0.5) / 0.035;
        v = std::exp(-d * d) * 2 - 0.12;
        break;
      }
      default: v = s; break;
    }
    c[static_cast<size_t>(i)] = static_cast<float>(v);
  }
  return c;
}

const std::array<BankPtr, WvFile>& builtinBanks() {
  static const std::array<BankPtr, WvFile> banks = [] {
    std::array<BankPtr, WvFile> a;
    for (int w = 0; w < WvFile; ++w) a[static_cast<size_t>(w)] = makeBank(builtinCycle(w));
    return a;
  }();
  return banks;
}

struct GmsAux final : AuxData {
  BankPtr file[kOscs];
};

BankPtr fileBank(const std::string& path) {
  static std::mutex mu;
  static std::map<std::string, BankPtr> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
  }
  SamplePtr s = sampleCache().get(path);
  BankPtr b;
  if (s && !s->left.empty()) {
    std::vector<float> c(s->left.begin(), s->left.end());
    if (s->right.size() == c.size())
      for (size_t i = 0; i < c.size(); ++i) c[i] = 0.5f * (c[i] + s->right[i]);
    b = makeBank(c);
  }
  std::lock_guard<std::mutex> lk(mu);
  if (cache.size() > 256) cache.clear();
  cache[path] = b;
  return b;
}

inline float readBank(const WaveBank& b, int lv, uint32_t ph) {
  const auto& t = b.t[static_cast<size_t>(lv)];
  const uint32_t i = ph >> (32 - kTabBits);
  const float f = static_cast<float>((ph >> (32 - kTabBits - 16)) & 0xFFFF) * (1.0f / 65536.0f);
  return t[i] + (t[i + 1] - t[i]) * f;
}

inline int levelFor(double hz, double sr) {
  const double maxH = sr * 0.5 / std::max(hz, 1.0);
  if (maxH >= kHarm) return 0;
  const int lv = static_cast<int>(std::ceil(std::log2(kHarm / std::max(1.0, maxH))));
  return std::clamp(lv, 0, kLevels - 1);
}

inline uint32_t incFor(double hz, double sr) {
  const double c = std::clamp(hz / sr, 0.0, 0.49);
  return static_cast<uint32_t>(c * 4294967296.0);
}

// ------------------------------------------------------------ effects
struct SimpleReverb {
  static constexpr int kC[4] = {1116, 1188, 1277, 1356}, kA[2] = {556, 441};
  std::vector<float> comb[2][4], ap[2][2];
  size_t cp[2][4] = {}, apos[2][2] = {}, clen[2][4] = {}, alen[2][2] = {};
  float z[2][4] = {};
  SimpleReverb() {
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) comb[c][i].assign(static_cast<size_t>((kC[i] + 23 * c) * 4.4) + 8, 0.f);
      for (int i = 0; i < 2; ++i) ap[c][i].assign(static_cast<size_t>((kA[i] + 13 * c) * 4.4) + 8, 0.f);
    }
  }
  void setRate(double sr) {
    const double k = sr / 44100.0;
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) clen[c][i] = std::min(comb[c][i].size() - 1, static_cast<size_t>((kC[i] + 23 * c) * k));
      for (int i = 0; i < 2; ++i) alen[c][i] = std::min(ap[c][i].size() - 1, static_cast<size_t>((kA[i] + 13 * c) * k));
    }
  }
  void clear() {
    for (int c = 0; c < 2; ++c) {
      for (auto& v : comb[c]) std::fill(v.begin(), v.end(), 0.f);
      for (auto& v : ap[c]) std::fill(v.begin(), v.end(), 0.f);
      for (float& v : z[c]) v = 0;
    }
  }
  void process(float in, float fb, float damp, float& oL, float& oR) {
    float out[2];
    for (int c = 0; c < 2; ++c) {
      float acc = 0;
      for (int i = 0; i < 4; ++i) {
        auto& b = comb[c][i];
        const float v = b[cp[c][i]];
        z[c][i] = v * (1 - damp) + z[c][i] * damp;
        b[cp[c][i]] = in + z[c][i] * fb;
        if (++cp[c][i] >= std::max<size_t>(1, clen[c][i])) cp[c][i] = 0;
        acc += v;
      }
      for (int i = 0; i < 2; ++i) {
        auto& b = ap[c][i];
        const float v = b[apos[c][i]];
        b[apos[c][i]] = acc + v * 0.5f;
        acc = v - acc;
        if (++apos[c][i] >= std::max<size_t>(1, alen[c][i])) apos[c][i] = 0;
      }
      out[c] = acc;
    }
    oL = out[0], oR = out[1];
  }
};

class GmsFx {
 public:
  GmsFx() {
    for (auto& d : delay_) d.assign(static_cast<size_t>(2.0 * 192000) + 8, 0.f);
    for (auto& d : flange_) d.assign(static_cast<size_t>(0.02 * 192000) + 8, 0.f);
    for (auto& d : pan_) d.assign(static_cast<size_t>(0.004 * 192000) + 8, 0.f);
  }
  void configure(const PluginState& s, double sr, double tempo) {
    if (sr != sr_) {
      sr_ = sr;
      rev_.setRate(sr);
    }
    tempo_ = tempo > 0 ? tempo : 120;
    for (int k = 0; k < kFxSlots; ++k) {
      Slot& sl = slot_[k];
      sl.on = prmb(s, fx(k, FfOn));
      sl.x = prm(s, fx(k, FfX), 0.5f);
      sl.y = prm(s, fx(k, FfY), 0.5f);
      sl.beats = kDivBeatsG[std::clamp(prmi(s, fx(k, FfDiv), 5), 0, kDivs - 1)] * (prmb(s, fx(k, FfX4)) ? 4.0 : 1.0);
      sl.shape = prmi(s, fx(k, FfShape), 0);
      sl.lfo = prm(s, fx(k, FfLfo), 0);
    }
    eqOn_ = prmb(s, GEqOn);
    static const double bw[kEqBands] = {1, 2.5, 2.8, 2.5, 2};
    for (int b = 0; b < kEqBands; ++b) {
      const double g = prm(s, GEq1 + b, 0);
      const double q = 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * bw[b]));
      const dsp::Biquad::Type t = b == 0 ? dsp::Biquad::LowShelf : b == kEqBands - 1 ? dsp::Biquad::HighShelf : dsp::Biquad::Peak;
      for (int c = 0; c < 2; ++c) eq_[b][c].set(t, sr_, kEqHz[b], q, g);
    }
  }
  bool busy() const {
    for (const auto& s : slot_)
      if (s.on || s.mix > 0.001f) return true;
    return false;
  }
  bool ringing() const { return busy() && quiet_ < static_cast<int64_t>(sr_ * 4); }
  void reset() {
    for (auto& d : delay_) std::fill(d.begin(), d.end(), 0.f);
    for (auto& d : flange_) std::fill(d.begin(), d.end(), 0.f);
    for (auto& d : pan_) std::fill(d.begin(), d.end(), 0.f);
    rev_.clear();
    for (auto& s : slot_) s.st = Slot::State{};
    quiet_ = 1 << 30;
  }
  void process(float* L, float* R, int n) {
    bool loud = false;
    for (int i = 0; i < n; ++i)
      if (std::fabs(L[i]) + std::fabs(R[i]) > 1e-7f) loud = true;
    for (int k = 0; k < kFxSlots; ++k) {
      Slot& sl = slot_[k];
      if (!sl.on && sl.mix <= 0.0f) continue;
      const double hz = tempo_ / 60.0 / std::max(0.01, sl.beats);
      for (int i = 0; i < n; ++i) {
        sl.mix += ((sl.on ? 1.0f : 0.0f) - sl.mix) * 0.01f;
        sl.ph += hz / sr_;
        bool wrapped = false;
        if (sl.ph >= 1) sl.ph -= std::floor(sl.ph), wrapped = true;
        double lv;
        switch (sl.shape) {
          case 1: lv = sl.ph < 0.5 ? 4 * sl.ph - 1 : 3 - 4 * sl.ph; break;
          case 2: lv = 2 * sl.ph - 1; break;
          case 3: lv = sl.ph < 0.5 ? 1 : -1; break;
          default: lv = std::sin(kTwoPi * sl.ph); break;
        }
        const float x = std::clamp(sl.x + sl.lfo * static_cast<float>(lv) * 0.5f, 0.0f, 1.0f), y = sl.y;
        float l = L[i], r = R[i], ol = l, orr = r;
        run(k, sl, x, y, wrapped, lv, l, r, ol, orr);
        L[i] = l + (ol - l) * sl.mix;
        R[i] = r + (orr - r) * sl.mix;
      }
    }
    if (eqOn_)
      for (int i = 0; i < n; ++i)
        for (int b = 0; b < kEqBands; ++b) {
          L[i] = static_cast<float>(eq_[b][0].process(L[i]));
          R[i] = static_cast<float>(eq_[b][1].process(R[i]));
        }
    for (int i = 0; i < n; ++i)
      if (std::fabs(L[i]) + std::fabs(R[i]) > 1e-6f) loud = true;
    quiet_ = loud ? 0 : quiet_ + n;
  }

 private:
  struct Slot {
    bool on = false;
    float x = 0.5f, y = 0.5f, lfo = 0, mix = 0;
    double beats = 1, ph = 0;
    int shape = 0;
    struct State {
      float hold[2] = {0, 0}, s = 0, ap[2][6] = {}, fb[2] = {0, 0}, dl[2] = {0, 0}, dh[2] = {0, 0};
      int count = 0;
      dsp::Biquad bq[2][3];
      float lastX = -1, lastY = -1;
    } st;
  };
  void lowpassTone(Slot& sl, float y, float& l, float& r) {
    if (y != sl.st.lastY) {
      const double fc = std::min(sr_ * 0.45, std::pow(2.0, 4.28 * y + 10.0));
      for (int c = 0; c < 2; ++c) sl.st.bq[c][0].set(dsp::Biquad::LowPass, sr_, fc, 0.707, 0);
      sl.st.lastY = y;
    }
    l = static_cast<float>(sl.st.bq[0][0].process(l));
    r = static_cast<float>(sl.st.bq[1][0].process(r));
  }
  void run(int k, Slot& sl, float x, float y, bool wrapped, double lv, float inL, float inR, float& l, float& r) {
    auto& st = sl.st;
    switch (k) {
      case FxDistort: {
        const float d = x * x * x * x;
        auto f = [&](float v) { return v * (512 * d + 0.5f) / (1 + 1024 * d * std::fabs(v)) * 1.6f; };
        l = f(inL), r = f(inR);
        lowpassTone(sl, y, l, r);
        break;
      }
      case FxCrush: {
        const int hold = 1 + static_cast<int>(64 * (1 - x) * (1 - x) * (x > 0.999f ? 0 : 1)) + (x < 0.999f ? 2 : 0);
        if (++st.count >= hold) st.count = 0, st.hold[0] = inL, st.hold[1] = inR;
        l = st.hold[0], r = st.hold[1];
        lowpassTone(sl, y, l, r);
        break;
      }
      case FxFlange:
      case FxPanner: {
        if (k == FxFlange) {
          const size_t n = flange_[0].size();
          const double dsec = (0.0003 + 0.0025 * std::clamp(1.0 - x + 0.3 * lv * std::fabs(sl.lfo), 0.02, 1.5)) * 3;
          const float fbk = -0.5f * y;
          float in[2] = {inL, inR}, out[2];
          for (int c = 0; c < 2; ++c) {
            double rp = static_cast<double>(fw_) - dsec * sr_;
            while (rp < 0) rp += static_cast<double>(n);
            const size_t i0 = static_cast<size_t>(rp) % n, i1 = (i0 + 1) % n;
            const float fr = static_cast<float>(rp - std::floor(rp));
            const float w = flange_[c][i0] + (flange_[c][i1] - flange_[c][i0]) * fr;
            flange_[c][fw_] = in[c] + w * fbk;
            out[c] = in[c] + w;
          }
          fw_ = (fw_ + 1) % n;
          l = out[0] * 0.7f, r = out[1] * 0.7f;
        } else {
          const size_t n = pan_[0].size();
          const double p = std::clamp(lv * x, -1.0, 1.0);
          pan_[0][pw_] = inL, pan_[1][pw_] = inR;
          const size_t d = static_cast<size_t>(std::fabs(p) * y * 0.002 * sr_);
          const size_t rd = (pw_ + n - std::min(d, n - 1)) % n;
          pw_ = (pw_ + 1) % n;
          const float gl = static_cast<float>(std::sqrt(0.5 * (1 - p))), gr = static_cast<float>(std::sqrt(0.5 * (1 + p)));
          l = (p > 0 ? pan_[0][rd] : inL) * gl * 1.414f;
          r = (p < 0 ? pan_[1][rd] : inR) * gr * 1.414f;
        }
        break;
      }
      case FxPhase: {
        const double f = ((1 + lv * 0.5) * x * x * 19950 + 50);
        const float a = static_cast<float>((1 - std::tan(std::clamp(kTwoPi * 0.5 * f / sr_, 1e-4, 1.5))) / (1 + std::tan(std::clamp(kTwoPi * 0.5 * f / sr_, 1e-4, 1.5))));
        const float fbk = static_cast<float>(std::cbrt(0.6 * y) * 0.98);
        float in[2] = {inL, inR}, out[2];
        for (int c = 0; c < 2; ++c) {
          float v = in[c] + st.fb[c] * fbk;
          for (int i = 0; i < 6; ++i) {
            const float yv = a * v + st.ap[c][i];
            st.ap[c][i] = v - a * yv;
            v = yv;
          }
          st.fb[c] = v;
          out[c] = (in[c] + v) * 0.6f;
        }
        l = out[0], r = out[1];
        break;
      }
      case FxLowPass:
      case FxHighPass: {
        if (x != st.lastX || y != st.lastY) {
          const double fc = k == FxLowPass ? std::pow(2.0, 8.28 * x + 6.0) : std::pow(2.0, 7.424 * x + 5.0);
          const double q = 1 + 2 * y * y;
          for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 3; ++i)
              st.bq[c][i].set(k == FxLowPass ? dsp::Biquad::LowPass : dsp::Biquad::HighPass, sr_, std::min(fc, sr_ * 0.45), i == 0 ? q : 0.707, 0);
          st.lastX = x, st.lastY = y;
        }
        double vl = inL, vr = inR;
        for (int i = 0; i < 3; ++i) vl = st.bq[0][i].process(vl), vr = st.bq[1][i].process(vr);
        l = static_cast<float>(vl), r = static_cast<float>(vr);
        break;
      }
      case FxDelay: {
        const size_t n = delay_[0].size();
        const double t = std::min(1.99, sl.beats * 60.0 / tempo_);
        const size_t rp = (dw_ + n - std::min(n - 1, static_cast<size_t>(t * sr_))) % n;
        const float g = x * x, lpA = std::min(2 * y, 1.0f), hpA = std::max(2 * y - 1, 0.0f);
        float in[2] = {inL, inR}, out[2];
        for (int c = 0; c < 2; ++c) {
          float d = delay_[c][rp];
          st.dl[c] += (d - st.dl[c]) * (1 - 0.9f * (1 - lpA * lpA));
          float v = st.dl[c];
          st.dh[c] += (v - st.dh[c]) * (0.02f + 0.3f * hpA * hpA);
          v -= st.dh[c] * hpA;
          delay_[c][dw_] = in[c] * g + v * g;
          out[c] = in[c] + d;
        }
        dw_ = (dw_ + 1) % n;
        l = out[0], r = out[1];
        break;
      }
      case FxReverb: {
        float wl, wr;
        rev_.process((inL + inR) * 0.015f, 0.72f + 0.27f * x * x, 0.1f + 0.8f * y * y, wl, wr);
        const float wet = 0.4f + 0.6f * x;
        l = inL + wl * wet, r = inR + wr * wet;
        break;
      }
      case FxGate: {
        if (wrapped) st.s = 1;
        const float rel = 1 + 50 * (1 - x) * (1 - x);
        st.s = std::max(0.0f, st.s - static_cast<float>(rel * 4.0 / (sr_ * std::max(0.01, sl.beats * 60.0 / tempo_))));
        const float g = 1 - (1 - st.s * st.s * st.s * st.s) * (1 - y * y);
        l = inL * g, r = inR * g;
        break;
      }
      default: break;
    }
  }
  double sr_ = 0, tempo_ = 120;
  Slot slot_[kFxSlots];
  std::vector<float> delay_[2], flange_[2], pan_[2];
  size_t dw_ = 0, fw_ = 0, pw_ = 0;
  SimpleReverb rev_;
  dsp::Biquad eq_[kEqBands][2];
  bool eqOn_ = false;
  int64_t quiet_ = 1 << 30;
};

// ------------------------------------------------------------ generator
class GmsGen final : public Generator {
 public:
  GmsGen() { (void)builtinBanks(); }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return fx_.ringing();
  }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    uint64_t age = 0;
    double note = 60, cur = 60, target = 60;
    float vel = 1;
    int stage = 0;  // 0 off, 1 attack, 2 decay/sustain, 3 release
    double amp = 0;
    double eg[2] = {0, 0};
    int egStage[2] = {0, 0};
    uint32_t ph[kMaxUni][kOscs] = {}, sync[kMaxUni] = {};
    double s[2][5] = {};  // ladder stages per channel (+ the driven input)
    // Control values of the current control period.
    double g = 0.5, kRes = 0, mod = 0, volMod = 1, hz[kOscs] = {220, 220, 220};
    int lv[kOscs] = {0, 0, 0};
    bool ctlDone = false;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    const auto& bb = builtinBanks();
    const GmsAux* ga = dynamic_cast<const GmsAux*>(aux.custom.get());
    for (int k = 0; k < kOscs; ++k) {
      const int w = std::clamp(prmi(s, osc(k, OsWave), WvSaw), 0, WvCount - 1);
      const WaveBank* b = nullptr;
      if (w == WvFile && ga && ga->file[k]) b = ga->file[k].get();
      bank_[k] = b ? b : bb[static_cast<size_t>(w == WvFile ? WvSaw : w)].get();
      oscSemi_[k] = prm(s, osc(k, OsSemi), 0) + prm(s, osc(k, OsCents), 0) / 100.0;
    }
    volume_ = static_cast<float>(dsp::dbToGain(prm(s, GVolume, 0)) * dsp::dbToGain(prm(s, GLevel, 0)));
    pan_ = prm(s, GPan, 0);
    pitch_ = prm(s, GPitch, 0) + 12.0 * prmi(s, GOctave, 0);
    velVol_ = prmb(s, GVelVol);
    ampA_ = prm(s, GAmpA, 0.002f), ampD_ = prm(s, GAmpD, 1.5f), ampS_ = prm(s, GAmpS, 0.8f), ampR_ = prm(s, GAmpR, 0.4f);
    slide_ = prmb(s, GSlide), slideT_ = prm(s, GSlideTime, 0.06f), mono_ = prmb(s, GMono);
    cutoff_ = prm(s, GCutoff, 9000), reso_ = prm(s, GReso, 0.1f), key_ = prm(s, GKey, 0), fltType_ = prmi(s, GFltType, 0);
    for (int k = 0; k < 2; ++k) {
      egAmt_[k] = prm(s, eg(k, EgAmt), 0), egA_[k] = prm(s, eg(k, EgAtt), 0.001f), egD_[k] = prm(s, eg(k, EgDec), 0.6f);
      egDest_[k] = prmi(s, eg(k, EgDest), 0);
      lfoAmt_[k] = prm(s, lfo(k, LfAmt), 0), lfoShape_[k] = prmi(s, lfo(k, LfShape), 0), lfoRate_[k] = prm(s, lfo(k, LfRate), 4);
      lfoSync_[k] = prmi(s, lfo(k, LfSync), 0), lfoDiv_[k] = std::clamp(prmi(s, lfo(k, LfDiv), 3), 0, kDivs - 1);
      lfoRetrig_[k] = prmb(s, lfo(k, LfRetrig)), lfoDest_[k] = prmi(s, lfo(k, LfDest), 0);
    }
    uni_ = std::clamp(prmi(s, GUniVoices, 1), 1, kMaxUni);
    const double det = 0.4 * prm(s, GUniDetune, 0.25f);
    const float stereo = prm(s, GUniStereo, 0.5f);
    for (int u = 0; u < uni_; ++u) {
      uniRatio_[u] = uni_ > 1 ? std::pow(2.0, (u / static_cast<double>(uni_ - 1) - 0.5) * det * det) : 1.0;
      double p = 0;
      if (uni_ > 1 && !(uni_ % 2 == 1 && u == 0)) p = (u % 2 ? -1.0 : 1.0) * stereo;
      double l, r;
      dsp::panGains(std::clamp(p, -1.0, 1.0), l, r);
      uniL_[u] = static_cast<float>(l), uniR_[u] = static_cast<float>(r);
    }
    uniPhase_ = prm(s, GUniPhase, 0), uniRetrig_ = prmb(s, GUniRetrig);
    noise_ = prm(s, GNoise, 0), m2_ = prm(s, GOsc2Mix, 0), m3_ = prm(s, GOsc3Mix, 0);
    modAmt_ = prm(s, GModAmt, 0), modKind_ = prmi(s, GModKind, GkFm);
    o2Phase_ = prm(s, GOsc2Phase, 0), o2Retrig_ = prmb(s, GOsc2Retrig), o2Inv_ = prmb(s, GOsc2Invert);
    velAmt_ = prm(s, GVelAmt, 0), velDest_ = prmi(s, GVelDest, 0);
    fx_.configure(s, sr_, tempo_);
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (lfoRetrig_[0]) lfoPh_[0] = 0;
    if (lfoRetrig_[1]) lfoPh_[1] = 0;
    const double from = lastNote_ < 0 ? pitch : lastNote_;
    lastNote_ = pitch;
    if (mono_) {
      Voice& v = voices_[0];
      const bool legato = v.on && v.held;
      v.id = id;
      v.target = pitch;
      v.note = pitch;
      v.vel = vel;
      if (!legato || !slide_) start(v, slide_ ? from : pitch, !legato);
      return;
    }
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick)
      for (auto& v : voices_)
        if (!pick || v.age < pick->age) pick = &v;
    pick->id = id;
    pick->target = pitch;
    pick->note = pitch;
    pick->vel = vel;
    start(*pick, slide_ ? from : pitch, true);
  }
  void start(Voice& v, double fromPitch, bool fresh) {
    const bool wasOn = v.on;
    v.on = v.held = true;
    v.age = ++clock_;
    v.cur = fromPitch;
    v.stage = 1;
    for (int k = 0; k < 2; ++k) v.egStage[k] = 1, v.eg[k] = fresh ? 0.0 : v.eg[k];
    if (!wasOn || uniRetrig_)
      for (int u = 0; u < uni_; ++u)
        for (int k = 0; k < kOscs; ++k) v.ph[u][k] = static_cast<uint32_t>(std::fmod(u * uniPhase_, 1.0) * 4294967296.0);
    if (!wasOn || o2Retrig_)
      for (int u = 0; u < uni_; ++u) v.ph[u][1] = static_cast<uint32_t>(std::fmod(o2Phase_ + u * uniPhase_, 1.0) * 4294967296.0);
    if (!wasOn)
      for (auto& c : v.s)
        for (double& x : c) x = 0;
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) v.held = false, v.stage = 3;
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.on) v.held = false, v.stage = 3;
  }
  void onKill() override {
    for (auto& v : voices_) v.on = false;
    fx_.reset();
  }

  double lfoValue(int k) const {
    const double p = lfoPh_[k];
    switch (lfoShape_[k]) {
      case LsTriangle: return p < 0.5 ? 2 * p : 2 - 2 * p;
      case LsSaw: return 1 - p;
      case LsSquare: return p < 0.5 ? 1 : 0;
      default: return std::sin(kTwoPi * p);
    }
  }
  double lfoHz(int k) const {
    if (lfoSync_[k] == LyFree) return lfoRate_[k];
    const double beats = kDivBeatsG[lfoDiv_[k]] * (lfoSync_[k] == LyBeats4 ? 4.0 : 1.0);
    return tempo_ / 60.0 / beats;
  }
  // Envelope step: exponential approach with time constant T (seconds).
  double rate(double T) const { return std::min(1.0, 8.82 / (std::max(1e-4, T) * sr_)); }

  void renderVoice(Voice& v, float* L, float* R, int n, const double lfo[2], bool fresh) {
    const double velT = velAmt_ * v.vel;
    auto velOf = [&](int d) { return velDest_ == d ? velT : 0.0; };
    const double tScale[2] = {std::pow(2.0, -4 * velOf(VdEg1A)), std::pow(2.0, -4 * velOf(VdEg2A))};
    const double dScale[2] = {std::pow(2.0, 4 * velOf(VdEg1D)), std::pow(2.0, 4 * velOf(VdEg2D))};
    const double egAmtV[2] = {egAmt_[0] + velOf(VdEg1Amt), egAmt_[1] + velOf(VdEg2Amt)};
    const double aA = rate(ampA_ * std::pow(2.0, -4 * velOf(VdAmpA))), aD = rate(ampD_ * std::pow(2.0, 4 * velOf(VdAmpD))), aR = rate(ampR_);
    const double slideK = rate(slideT_);
    const double lvl = velVol_ ? v.vel : 1.0;
    {
      const int i0 = 0, len = n;
      if (fresh) {
      // Control-rate modulation.
      double cutOct = key_ * (v.cur - 57.0) / 128.0 * 9.77 + 9.77 * velOf(VdCutoff), mod = modAmt_ + velOf(VdMod), pitch = 0, volMod = 1;
      double lfoA[2] = {lfoAmt_[0] + velOf(VdLfo1Amt), lfoAmt_[1] + velOf(VdLfo2Amt)};
      for (int k = 0; k < 2; ++k) {
        const double e = v.eg[k] * egAmtV[k];
        switch (egDest_[k]) {
          case EdCutoff: cutOct += 9.77 * e; break;
          case EdMod: mod += e; break;
          case EdPitch: pitch += 24 * e; break;
          default: lfoA[k] += e; break;
        }
      }
      for (int k = 0; k < 2; ++k) {
        const double l = lfo[k] * lfoA[k];
        switch (lfoDest_[k]) {
          case LdCutoff: cutOct += 9.77 * l * 0.5; break;
          case LdMod: mod += l; break;
          case LdPitch: pitch += 12 * l; break;
          default: volMod *= std::max(0.0, 1 + l); break;
        }
      }
      mod = std::clamp(mod, 0.0, 1.0);
      const double reso = std::clamp(reso_ + velOf(VdReso), 0.0, 1.0);
      const double fc = std::clamp(cutoff_ * std::pow(2.0, cutOct), 14.0, 20000.0);
      const double g = 1.0 - std::exp(-kTwoPi * std::min(fc, sr_ * 0.9) / (2.0 * sr_));  // 2× oversampled one-pole
      const double kRes = reso * 3.9;
      // Pitches.
      const double base = v.cur + pitch_ + pitch;
      for (int k = 0; k < kOscs; ++k) {
        v.hz[k] = 220.0 * std::pow(2.0, (base + oscSemi_[k] - 57.0) / 12.0);
        v.lv[k] = levelFor(v.hz[k] * (k == 0 && modKind_ == GkSync ? 1 + 3 * mod : 1.0), sr_);
      }
      v.g = g, v.kRes = kRes, v.mod = mod, v.volMod = volMod;
      }
      const double g = v.g, kRes = v.kRes, mod = v.mod, volMod = v.volMod;
      const double* hz = v.hz;
      const int* lv = v.lv;
      const float g1 = static_cast<float>((1 - m2_) * (1 - m3_) * (1 - noise_)), g2 = static_cast<float>(m2_ * (1 - m3_) * (1 - noise_)),
                  g3 = static_cast<float>(m3_ * (1 - noise_)), gn = static_cast<float>(noise_);
      const float norm = 1.0f / std::sqrt(static_cast<float>(uni_));
      for (int j = 0; j < len; ++j) {
        // Envelopes.
        switch (v.stage) {
          case 1:
            v.amp += (5.0 - v.amp) * aA;
            if (v.amp >= 1) v.amp = 1, v.stage = 2;
            break;
          case 2: v.amp += (ampS_ - v.amp) * aD; break;
          case 3:
            v.amp -= v.amp * aR;
            if (v.amp < 1e-4) v.amp = 0, v.on = false;
            break;
          default: break;
        }
        for (int k = 0; k < 2; ++k) {
          if (v.egStage[k] == 1) {
            v.eg[k] += (5.0 - v.eg[k]) * rate(egA_[k] * tScale[k]);
            if (v.eg[k] >= 1) v.eg[k] = 1, v.egStage[k] = 2;
          } else {
            v.eg[k] -= v.eg[k] * rate(egD_[k] * dScale[k]);
          }
        }
        if (slide_) v.cur += (v.target - v.cur) * slideK;
        else v.cur = v.target;
        float l = 0, r = 0;
        for (int u = 0; u < uni_; ++u) {
          const double ur = uniRatio_[u];
          uint32_t* ph = v.ph[u];
          const float o2 = readBank(*bank_[1], lv[1], ph[1]) * (o2Inv_ ? -1.0f : 1.0f);
          const float o3 = readBank(*bank_[2], lv[2], ph[2]);
          float o1;
          switch (modKind_) {
            case GkSync: {
              const uint32_t mInc = incFor(hz[0] * ur, sr_);
              const uint32_t before = v.sync[u];
              v.sync[u] += mInc;
              if (v.sync[u] < before) ph[0] = 0;  // master wrapped: reset the slave
              o1 = readBank(*bank_[0], lv[0], ph[0]);
              ph[0] += incFor(hz[0] * ur * (1 + 3 * mod), sr_);
              break;
            }
            case GkAm:
              o1 = readBank(*bank_[0], lv[0], ph[0]) * static_cast<float>(1 - 3 * mod * o2);
              ph[0] += incFor(hz[0] * ur, sr_);
              break;
            default: {
              const double off = 15.0 * mod * o2 * 0.25;
              o1 = readBank(*bank_[0], lv[0], ph[0] + static_cast<uint32_t>(static_cast<int64_t>((off - std::floor(off)) * 4294967296.0)));
              ph[0] += incFor(hz[0] * ur, sr_);
              break;
            }
          }
          ph[1] += incFor(hz[1] * ur, sr_);
          ph[2] += incFor(hz[2] * ur, sr_);
          const float nz = gn > 0 ? rng_.next() * 0.5f : 0.0f;
          const float x = (o1 * g1 + o2 * g2 + o3 * g3 + nz * gn) * norm;
          l += x * uniL_[u];
          r += x * uniR_[u];
        }
        // Ladder filter, 2× oversampled (same input twice), soft saturation.
        float in[2] = {l, r}, out[2];
        for (int c = 0; c < 2; ++c) {
          double* st = v.s[c];
          for (int os = 0; os < 2; ++os) {
            double u = in[c] - kRes * st[3];
            u = u / (1 + 0.04 * u * u);
            st[4] = u;
            st[0] += g * (u - st[0]);
            st[1] += g * (st[0] - st[1]);
            st[2] += g * (st[1] - st[2]);
            st[3] += g * (st[2] - st[3]);
          }
          double y = st[3];
          if (fltType_ == GfHighPass) y = st[4] - 4 * st[0] + 6 * st[1] - 4 * st[2] + st[3];  // (1 - H)^4 from the ladder taps
          else if (fltType_ == GfBandPass) y = 4.0 * (st[1] - 2 * st[2] + st[3]);           // H^2 (1 - H)^2
          out[c] = static_cast<float>(y * (1 + kRes * 0.25));
        }
        const float a = static_cast<float>(v.amp * v.amp * lvl * volMod) * volume_;
        L[i0 + j] += out[0] * a;
        R[i0 + j] += out[1] * a;
        if (!v.on) return;
      }
    }
  }

  void render(float* L, float* R, int n) override {
    float bl[256], br[256];
    for (int pos = 0; pos < n; pos += 256) {
      const int len = std::min(256, n - pos);
      std::fill(bl, bl + len, 0.0f);
      std::fill(br, br + len, 0.0f);
      // Control periods of kCtl samples on a grid independent of the block size.
      for (int i0 = 0; i0 < len;) {
        const bool fresh = ctr_ == 0;
        if (fresh) {
          lfoNow_[0] = lfoValue(0), lfoNow_[1] = lfoValue(1);
          for (int k = 0; k < 2; ++k) {
            lfoPh_[k] += lfoHz(k) * kCtl / sr_;
            lfoPh_[k] -= std::floor(lfoPh_[k]);
          }
          ctr_ = kCtl;
          for (auto& v : voices_) v.ctlDone = false;
        }
        const int sub = std::min(ctr_, len - i0);
        for (auto& v : voices_)
          if (v.on) {
            renderVoice(v, bl + i0, br + i0, sub, lfoNow_, !v.ctlDone);
            v.ctlDone = true;
          }
        ctr_ -= sub;
        i0 += sub;
      }
      fx_.process(bl, br, len);
      double gl, gr;
      dsp::panGains(pan_, gl, gr);
      for (int i = 0; i < len; ++i) {
        L[pos + i] += bl[i] * static_cast<float>(gl * 1.414);
        R[pos + i] += br[i] * static_cast<float>(gr * 1.414);
      }
    }
  }

 private:
  Voice voices_[kVoices];
  const WaveBank* bank_[kOscs] = {nullptr, nullptr, nullptr};
  double oscSemi_[kOscs] = {0, 0, 0};
  double sr_ = 44100, tempo_ = 120, pitch_ = 0, lastNote_ = -1;
  float volume_ = 1, pan_ = 0;
  bool velVol_ = true, slide_ = false, mono_ = false, uniRetrig_ = false, o2Retrig_ = false, o2Inv_ = false;
  double ampA_ = 0.002, ampD_ = 1.5, ampS_ = 0.8, ampR_ = 0.4, slideT_ = 0.06;
  double cutoff_ = 9000, reso_ = 0.1, key_ = 0;
  int fltType_ = 0;
  double egAmt_[2] = {0, 0}, egA_[2] = {0.001, 0.001}, egD_[2] = {0.6, 0.6};
  int egDest_[2] = {0, 1};
  double lfoAmt_[2] = {0, 0}, lfoRate_[2] = {4, 4}, lfoPh_[2] = {0, 0};
  int lfoShape_[2] = {0, 0}, lfoSync_[2] = {0, 0}, lfoDiv_[2] = {3, 3}, lfoDest_[2] = {0, 2};
  bool lfoRetrig_[2] = {false, false};
  int uni_ = 1, ctr_ = 0;
  double lfoNow_[2] = {0, 0};
  double uniRatio_[kMaxUni] = {}, uniPhase_ = 0;
  float uniL_[kMaxUni] = {}, uniR_[kMaxUni] = {};
  double noise_ = 0, m2_ = 0, m3_ = 0, modAmt_ = 0, o2Phase_ = 0, velAmt_ = 0;
  int modKind_ = GkFm, velDest_ = 0;
  uint64_t clock_ = 0;
  dsp::Noise rng_;
  GmsFx fx_;
};

}  // namespace

std::unique_ptr<Generator> makeGms() { return std::make_unique<GmsGen>(); }

std::shared_ptr<const AuxData> buildGmsAux(const PluginState& s) {
  const WaveFiles w = parseWaveFiles(s.data);
  auto a = std::make_shared<GmsAux>();
  bool any = false;
  for (int k = 0; k < kOscs; ++k)
    if (!w.path[k].empty() && prmi(s, osc(k, OsWave), WvSaw) == WvFile) {
      a->file[k] = fileBank(w.path[k]);
      any = any || a->file[k] != nullptr;
    }
  return any ? a : nullptr;
}

}  // namespace gl::packs::synths
