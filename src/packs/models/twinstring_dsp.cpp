// Gridline — Twin String (gl.twinstring): per voice an exciter (an impulse
// plus sample-and-held noise, overdriven, then a resonant high cut swept by
// its own ADSR and a resonant low cut) drives two waveguide strings (loop
// gain from the ring time, a two-stage damping low pass set by amount /
// offset / shape, optional inverted loop for odd-harmonic "reed" tones, soft
// saturation); string B is tuned by a ratio; the two are mixed and spread in
// stereo, sent through a per-voice comb (delay in note periods, feedback and
// level with polarity) and the amp ADSR. A free ADSR and an LFO feed an
// 8-slot modulation matrix (sources: envelope, LFO, mod wheel, pitch bend,
// pressure, velocity, key, release velocity). Globally: an 8-band resonator
// body, then delay (modulated, blurred, tempo-synced), chorus and reverb.
// Audio thread: fixed arrays only.
#include <array>
#include <cmath>
#include <cstring>
#include "models_rt.h"
#include "strings.h"
#include "strings_laws.h"

namespace gl::packs::models {

using namespace strs;

namespace {

using namespace dsp;
constexpr int kVoices = 16;
constexpr int kLine = 32768;  // per string (down to ~6 Hz at 192 kHz)
constexpr int kCtl = 32;      // control-rate period (samples)
constexpr int kChunk = 1024;  // render chunk (mix buffers)

struct Loop {
  float buf[kLine];
  int w = 0;
  float lp1 = 0, lp2 = 0, dc = 0, dcx = 0;
  double apx = 0, apy = 0;  // fractional-delay allpass state
  void clear() {
    std::memset(buf, 0, sizeof buf);
    w = 0;
    lp1 = lp2 = dc = dcx = 0;
    apx = apy = 0;
  }
  // Delay of d samples (d ≥ 2): integer read + first-order allpass for the
  // fraction (delay in [0.5, 1.5)), which keeps the loop level exact.
  float readAp(double d) {
    const int n = static_cast<int>(std::floor(d - 0.5));
    const double fr = d - n, a = (1.0 - fr) / (1.0 + fr);
    const double xi = buf[(w - n) & (kLine - 1)];
    const double y = a * xi + apx - a * apy;
    apx = xi;
    apy = std::fabs(y) < 1e-20 ? 0.0 : y;
    return static_cast<float>(y);
  }
  float read(double d) const {
    double rp = static_cast<double>(w) - d;
    while (rp < 0) rp += kLine;
    int i0 = static_cast<int>(rp);
    float f = static_cast<float>(rp - i0);
    int i1 = (i0 + 1) & (kLine - 1);
    i0 &= kLine - 1;
    return buf[i0] + (buf[i1] - buf[i0]) * f;
  }
  // Zeroes the `count` samples behind the write head (all a fresh note reads).
  void clearBehind(int count) {
    count = std::min(count, kLine);
    for (int i = 1; i <= count; ++i) buf[(w - i) & (kLine - 1)] = 0.0f;
    lp1 = lp2 = dc = dcx = 0;
    apx = apy = 0;
  }
  void write(float v) {
    buf[w] = v;
    w = (w + 1) & (kLine - 1);
  }
};

// Mod source polarity: LFO, pitch bend and key are bipolar.
inline bool bipolar(int src) { return src == MsLfo || src == MsPitchWheel || src == MsKey; }

class TwinStringGen final : public Generator {
 public:
  TwinStringGen() : v_(new Voice[kVoices]) {
    if (const PluginInfo* info = findPlugin("gl.twinstring"))
      for (size_t i = 0; i < static_cast<size_t>(TsCount) && i < info->params.size(); ++i) def_[i] = static_cast<float>(info->params[i].def);
    p_ = def_;
    for (int i = 0; i < kVoices; ++i) v_[i].clear();
    comb_.reset(new float[kVoices * 2 * kLine]);
    std::memset(comb_.get(), 0, sizeof(float) * kVoices * 2 * kLine);
    for (auto& b : res_) b.assign(8192, 0.0f);
    dly_[0].assign(static_cast<size_t>(192000 * 4.2) + 8, 0.0f);
    dly_[1] = dly_[0];
    cho_[0].assign(static_cast<size_t>(192000 * 0.06) + 8, 0.0f);
    cho_[1] = cho_[0];
    for (int i = 0; i < 4; ++i) rev_[i].assign(static_cast<size_t>(192000 * 0.12) + 8, 0.0f);
  }
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (v_[i].on) return true;
    return tail_ > 0;
  }

 private:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double note = 60;
    float vel = 1, relVel = 0.5f;
    uint64_t age = 0;
    Loop s[2];
    Adsr amp, hc, eg;
    Svf hiF, loF;
    Noise rng;
    float sh = 0;
    double shPh = 0;
    double pulsePh = 0, pulseInc = 0, carry = 0;
    float panL = 1, panR = 1;
    int cw = 0;  // comb write index
    // Control-rate state.
    double len[2] = {100, 100}, g[2] = {0.99, 0.99}, a1[2] = {0, 0}, a2[2] = {0, 0};
    double combLen = 100, noiseInc = 1, hiHz = 1000, loHz = 20, gain = 1, pan = 0;
    int ctl = 0;
    void clear() {
      s[0].clear();
      s[1].clear();
      on = held = false;
    }
  };

  void configure(const PluginState& st, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    // Parameters (missing trailing ones keep the catalogue defaults cached at construction).
    for (size_t i = 0; i < static_cast<size_t>(TsCount); ++i) p_[i] = i < st.params.size() ? st.params[i] : def_[i];
    for (int i = 0; i < kVoices; ++i) {
      v_[i].amp.set(sr_, law::tsAttack(p(TsAmpA)), law::tsDecay(p(TsAmpD)), p(TsAmpS), law::tsRelease(p(TsAmpR)));
      v_[i].eg.set(sr_, law::tsAttack(p(TsEgA)), law::tsDecay(p(TsEgD)), p(TsEgS), law::tsRelease(p(TsEgR)));
    }
    lfoInc_ = law::tsLfoHz(p(TsLfoRate)) / sr_;
    // Resonator body (global).
    int nOn = 0;
    for (int k = 0; k < kTsRes; ++k) nOn += p(TsResOn1 + k) >= 0.5f;
    resNorm_ = 0.6 / std::sqrt(std::max(1, nOn));
    for (int k = 0; k < kTsRes; ++k) {
      resOn_[k] = p(TsResOn1 + k) >= 0.5f;
      double hz = law::tsResHz(p(TsResFreq1 + k));
      resLen_[k] = std::clamp(sr_ / hz, 2.0, 8190.0);
      resFb_[k] = 0.985 * std::sqrt(law::clamp01(p(TsResFb1 + k)));
    }
    // Effects.
    dlyOn_ = p(TsDlyOn) >= 0.5f;
    int sync = std::clamp(static_cast<int>(std::lround(p(TsDlySync))), 0, kTsDlySyncs - 1);
    dlySec_ = sync > 0 ? law::tsDelayBeats(sync) * 60.0 / tempo_ : law::tsDelaySec(p(TsDlyTime));
    dlySec_ = std::clamp(dlySec_, 0.001, 4.1);
    dlyFb_ = 0.95 * p(TsDlyFb);
    dlyModHz_ = law::expMap(p(TsDlySpeed), 0.05, 8.0);
    dlyModDepth_ = 0.004 * p(TsDlyDepth);
    dlyBlur_ = p(TsDlyBlur);
    dlyWet_ = p(TsDlyWet);
    choOn_ = p(TsChoOn) >= 0.5f;
    choHz_ = law::expMap(p(TsChoRate), 0.05, 6.0);
    choDepth_ = 0.0005 + 0.006 * p(TsChoDepth);
    choWet_ = p(TsChoWet);
    revOn_ = p(TsRevOn) >= 0.5f;
    revWet_ = p(TsRevMix);
    double t60 = law::tsReverbSec(p(TsRevDecay));
    static const double kRevMs[4] = {29.7, 37.1, 41.1, 43.7};
    for (int i = 0; i < 4; ++i) {
      revLen_[i] = std::min(rev_[i].size() - 1, static_cast<size_t>(kRevMs[i] * 0.001 * sr_ * 1.6));
      revG_[i] = std::pow(10.0, -3.0 * (static_cast<double>(revLen_[i]) / sr_) / t60);
    }
    revDamp_ = std::exp(-kTwoPi * law::expMap(p(TsRevColor), 1500.0, 18000.0) / sr_);
  }

  float p(int i) const { return p_[static_cast<size_t>(i)]; }
  int pi(int i) const { return static_cast<int>(std::lround(p(i))); }

  // Sum of matrix slots targeting `dest` for a voice (signed, in source units).
  double mod(const Voice& v, int dest, double egv) const {
    double sum = 0;
    for (int k = 0; k < kTsMods; ++k) {
      if (pi(tsMod(k, TmDest)) != dest) continue;
      int src = pi(tsMod(k, TmSource));
      if (src <= MsNone || src >= MsCount) continue;
      double a = p(tsMod(k, TmAmount)) * (p(tsMod(k, TmNegative)) >= 0.5f ? -1.0 : 1.0);
      sum += a * source(v, src, egv);
    }
    return sum;
  }
  double source(const Voice& v, int src, double egv) const {
    switch (src) {
      case MsEnv: return egv;
      case MsLfo: return lfoOut_;
      case MsModWheel: return modWheel_;
      case MsPitchWheel: return bend_;
      case MsPressure: return pressure_;
      case MsVelocity: return v.vel;
      case MsKey: return std::clamp((v.note - 60.0) / 48.0, -1.0, 1.0);
      case MsRelease: return v.held ? 0.0 : v.relVel;
      default: return 0.0;
    }
  }
  bool targeted(int dest) const {
    for (int k = 0; k < kTsMods; ++k)
      if (pi(tsMod(k, TmDest)) == dest && pi(tsMod(k, TmSource)) > MsNone) return true;
    return false;
  }
  // Level: unipolar sources scale (velocity amount 1 → gain = velocity).
  double levelGain(const Voice& v, double egv) const {
    double g = 1;
    for (int k = 0; k < kTsMods; ++k) {
      if (pi(tsMod(k, TmDest)) != MdLevel) continue;
      int src = pi(tsMod(k, TmSource));
      if (src <= MsNone || src >= MsCount) continue;
      double a = p(tsMod(k, TmAmount));
      bool neg = p(tsMod(k, TmNegative)) >= 0.5f;
      double s = source(v, src, egv);
      if (bipolar(src))
        g *= std::max(0.0, 1.0 + (neg ? -a : a) * s);
      else
        g *= 1.0 - a + a * (neg ? 1.0 - s : s);
    }
    return std::max(0.0, g);
  }

  // Control-rate update of one voice: tuning, loop filters, exciter filters.
  void control(Voice& v) {
    const double egv = v.eg.level;
    double semis = law::tsFineSemis(p(TsTune)) + law::tsTransposeSemis(pi(TsTranspose)) + 12.0 * mod(v, MdPitch, egv);
    const double f0 = noteToHz(v.note + semis);
    const double ratio = law::tsRatio(pi(TsS2Ratio));
    const double dBoth = mod(v, MdBothDecay, egv), mBoth = mod(v, MdBothDamp, egv);
    for (int s = 0; s < 2; ++s) {
      const int b = s ? TsS2Decay : TsS1Decay;
      const double f = std::max(5.0, f0 * (s ? ratio : 1.0));
      const bool inv = p(b + 1) >= 0.5f;
      double decayPos = p(b) + 0.5 * (mod(v, s ? MdS2Decay : MdS1Decay, egv) + dBoth);
      double damp = law::clamp01(p(b + 3) + mod(v, s ? MdS2Damp : MdS1Damp, egv) + mBoth);
      double ofs = p(b + 2), shape = p(b + 4);
      // Damping low pass: corner from the offset (harmonic 1 .. 64 of the
      // string) lowered by the amount; shape blends in a second pole.
      double corner = f * std::pow(64.0, 1.0 - ofs) * std::pow(0.02, damp * damp);
      corner = std::clamp(corner, 30.0, sr_ * 0.45);
      double a = std::exp(-kTwoPi * corner / sr_);
      v.a1[s] = damp <= 0.0005 ? 0.0 : a;
      v.a2[s] = damp <= 0.0005 ? 0.0 : a * law::clamp01(shape);
      // Loop delay: period (inverted loops ring an octave down, so they run
      // at half length to keep the note) minus the filters' phase delay.
      double period = sr_ / f * (inv ? 0.5 : 1.0);
      double pd = 0;
      const double w = kTwoPi * f / sr_;
      for (double aa : {v.a1[s], v.a2[s]})
        if (aa > 0) pd += std::atan2(aa * std::sin(w), 1.0 - aa * std::cos(w)) / w;
      v.len[s] = std::clamp(period - pd, 2.0, static_cast<double>(kLine - 4));
      // Loop gain for the ring time (per period), corrected for the filters' gain at f.
      double hmag = 1;
      for (double aa : {v.a1[s], v.a2[s]})
        if (aa > 0) hmag *= (1 - aa) / std::sqrt(1 - 2 * aa * std::cos(w) + aa * aa);

      double t60 = law::tsStringT60(law::clamp01(decayPos));
      double gper = std::pow(10.0, -3.0 / (t60 * f * (inv ? 2.0 : 1.0)));
      v.g[s] = std::min(0.99995, gper / std::max(1e-6, hmag)) * (inv ? -1.0 : 1.0);
    }
    v.combLen = std::clamp(sr_ / std::max(5.0, f0) * law::tsCombPeriods(p(TsCombRatio)), 1.0, static_cast<double>(kLine - 4));
    double hcPos = law::clamp01(p(TsHiCut) + mod(v, MdHiCut, egv));
    v.hiHz = std::min(sr_ * 0.45, law::tsHiCutHz(hcPos * (0.6 + 0.4 * v.hc.level), f0));
    v.loHz = std::min(sr_ * 0.45, law::tsLoCutHz(p(TsLoCut) + mod(v, MdLoCut, egv), f0));
    v.hiF.set(sr_, v.hiHz, law::tsReso(p(TsHiRes)));
    v.loF.set(sr_, v.loHz, law::tsReso(p(TsLoRes)));
    v.pulseInc = std::min(0.5, f0 / sr_);
    v.noiseInc = law::tsNoiseHz(law::clamp01(p(TsNoiseRate) + mod(v, MdNoiseRate, egv)), sr_) / sr_;
    v.gain = levelGain(v, egv);
    v.pan = std::clamp(mod(v, MdPan, egv), -1.0, 1.0);
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    int slot = -1;
    for (int i = 0; i < kVoices; ++i)
      if (!v_[i].on) {
        slot = i;
        break;
      }
    if (slot < 0) {  // steal the oldest
      uint64_t best = UINT64_MAX;
      for (int i = 0; i < kVoices; ++i)
        if (v_[i].age < best) best = v_[i].age, slot = i;
    }
    Voice& v = v_[slot];
    v.on = v.held = true;
    v.id = id;
    v.note = pitch;
    v.vel = velocity;
    v.age = ++clock_;
    v.rng.seed(0x9E3779B9u ^ (id * 2654435761u) ^ static_cast<uint32_t>(clock_));
    v.amp.level = 0;
    v.amp.gate(true);
    v.hc.level = 0;
    v.hc.set(sr_, law::tsAttack(p(TsHiAtk)), law::tsDecay(p(TsHiDec)), p(TsHiSus), law::tsRelease(p(TsHiDec)));
    v.hc.gate(true);
    v.eg.level = 0;
    v.eg.gate(true);
    v.hiF.reset();
    v.loF.reset();
    v.pulsePh = 0.999999;
    v.carry = 0;
    v.shPh = 1;
    v.ctl = 0;
    control(v);
    // Fresh loops: clear what the new note reads (twice its length, for upward bends of the period).
    for (int s = 0; s < 2; ++s) v.s[s].clearBehind(static_cast<int>(2 * v.len[s]) + 8);
    float* comb = comb_.get() + static_cast<size_t>(slot) * 2 * kLine;
    const int cl = std::min(kLine, static_cast<int>(2 * v.combLen) + 8);
    for (int c = 0; c < 2; ++c)
      for (int i = 1; i <= cl; ++i) comb[c * kLine + ((v.cw - i) & (kLine - 1))] = 0.0f;
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i)
      if (v_[i].on && v_[i].held && v_[i].id == id) {
        v_[i].held = false;
        v_[i].relVel = 0.5f;
        v_[i].amp.gate(false);
        v_[i].hc.gate(false);
        v_[i].eg.gate(false);
      }
  }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const int kind = status & 0xF0;
    if (kind == 0xB0 && d1 == 1) modWheel_ = d2 / 127.0;
    if (kind == 0xE0) bend_ = std::clamp(((d2 << 7 | d1) - 8192) / 8192.0, -1.0, 1.0);
    if (kind == 0xD0) pressure_ = d1 / 127.0;
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i) onNoteOff(v_[i].id);
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) v_[i].on = false;
    tail_ = 0;
    for (auto& b : res_) std::fill(b.begin(), b.end(), 0.0f);
    for (auto& d : dly_) std::fill(d.begin(), d.end(), 0.0f);
    for (auto& d : cho_) std::fill(d.begin(), d.end(), 0.0f);
    for (auto& d : rev_) std::fill(d.begin(), d.end(), 0.0f);
  }

  void render(float* L, float* R, int n) override {
    while (n > 0) {
      const int m = std::min(n, kChunk);
      renderChunk(L, R, m);
      L += m;
      R += m;
      n -= m;
    }
  }
  void renderChunk(float* L, float* R, int n) {
    const double level = law::tsLevelGain(p(TsLevel));
    const double drive = p(TsDrive), noiseLvl = p(TsNoiseLevel), sat = p(TsSaturation);
    const double mix = p(TsMix), stereo = p(TsStereo);
    const double wA = std::sqrt(1.0 - mix), wB = std::sqrt(mix);
    const double combFb = 0.97 * p(TsCombFb) * (p(TsCombFbInv) >= 0.5f ? -1.0 : 1.0);
    const double combLv = p(TsCombLevel) * (p(TsCombLevelInv) >= 0.5f ? -1.0 : 1.0);
    const double lfoBase = targeted(MdLfoDepth) ? 0.0 : 1.0;
    bool any = false;
    for (int i = 0; i < n; ++i) mixL_[i] = mixR_[i] = 0;
    // LFO (global, control rate inside the loop).
    for (int vi = 0; vi < kVoices; ++vi) {
      Voice& v = v_[vi];
      if (!v.on) continue;
      any = true;
      float* comb = comb_.get() + static_cast<size_t>(vi) * 2 * kLine;
      for (int i = 0; i < n; ++i) {
        if (v.ctl-- <= 0) {
          v.ctl = kCtl;
          double ph = lfoPh_ + lfoInc_ * i;
          double depth = std::clamp(lfoBase + mod(v, MdLfoDepth, v.eg.level), 0.0, 1.0);
          lfoOut_ = depth * std::sin(kTwoPi * (ph - std::floor(ph)));
          control(v);
        }
        const double env = v.amp.next();
        const double exEnv = v.hc.next();
        v.eg.next();
        // Exciter.
        // Pulse train at the note (the tonal "bow" / "pick" part) crossfaded
        // with sample-and-held noise by the noise level.
        double x = v.carry;
        v.carry = 0;
        v.pulsePh += v.pulseInc;
        if (v.pulsePh >= 1) {
          v.pulsePh -= std::floor(v.pulsePh);
          const double fr = v.pulsePh / std::max(1e-9, v.pulseInc);  // fraction of a sample past the edge
          const double amp = 1.4 * (1.0 - noiseLvl);
          x += amp * (1.0 - std::min(1.0, fr));
          v.carry = amp * std::min(1.0, fr);
        }
        x -= v.pulseInc * 1.4 * (1.0 - noiseLvl);  // no DC
        v.shPh += v.noiseInc;
        if (v.shPh >= 1) {
          v.shPh -= std::floor(v.shPh);
          v.sh = v.rng.next();
        }
        x += 8.0 * v.sh * noiseLvl;
        x *= exEnv;
        if (drive > 0) x = std::tanh(x * (1.0 + 12.0 * drive)) / std::tanh(1.0 + 12.0 * drive) * (1.0 + drive);
        x = v.hiF.process(x);
        double hp = 0;
        v.loF.process(x, nullptr, &hp);
        x = hp * 0.5;
        // Strings.
        double out[2];
        for (int s = 0; s < 2; ++s) {
          Loop& lp = v.s[s];
          // The pickup sees the travelling wave (excitation included); the
          // reflection loses level / highs and saturates on its way back.
          const double raw = lp.readAp(v.len[s]);
          double y = raw;
          if (v.a1[s] > 0) {
            lp.lp1 = static_cast<float>(y + (lp.lp1 - y) * v.a1[s]);
            y = lp.lp1;
          }
          if (v.a2[s] > 0) {
            lp.lp2 = static_cast<float>(y + (lp.lp2 - y) * v.a2[s]);
            y = lp.lp2;
          }
          y *= v.g[s];
          if (sat > 0) y = std::tanh(y * (1.0 + 4.0 * sat)) / (1.0 + 4.0 * sat) * (1.0 + 0.5 * sat);
          if (std::fabs(y) > 0.6) y = std::copysign(0.6 + 0.4 * std::tanh((std::fabs(y) - 0.6) / 0.4), y);  // bounds driven (bowed) loops
          // DC block inside the loop.
          double dcy = y - lp.dcx + 0.9995 * lp.dc;
          lp.dcx = static_cast<float>(y);
          lp.dc = std::fabs(dcy) < 1e-20 ? 0.0f : static_cast<float>(dcy);
          lp.write(static_cast<float>(x + dcy));
          out[s] = raw;
        }
        double a = out[0] * wA, b = out[1] * wB;
        // Stereo spread: 0 = both strings centred, 1 = A left / B right.
        double l = a * (0.5 + 0.5 * stereo) + b * (0.5 - 0.5 * stereo), r = b * (0.5 + 0.5 * stereo) + a * (0.5 - 0.5 * stereo);
        // Comb (per voice, both channels).
        if (combLv != 0 || combFb != 0) {
          double d = v.combLen;
          for (int c = 0; c < 2; ++c) {
            float* cb = comb + c * kLine;
            double rp = static_cast<double>(v.cw) - d;
            while (rp < 0) rp += kLine;
            int i0 = static_cast<int>(rp);
            float f = static_cast<float>(rp - i0);
            float dv = cb[i0 & (kLine - 1)] + (cb[(i0 + 1) & (kLine - 1)] - cb[i0 & (kLine - 1)]) * f;
            double in = c ? r : l;
            cb[v.cw] = static_cast<float>(in + combFb * dv);
            double o = in + combLv * dv;
            (c ? r : l) = o;
          }
          v.cw = (v.cw + 1) & (kLine - 1);
        }
        double gpan = env * v.gain;
        double pl = v.pan > 0 ? 1.0 - v.pan : 1.0, pr = v.pan < 0 ? 1.0 + v.pan : 1.0;
        mixL_[i] += static_cast<float>(l * gpan * pl);
        mixR_[i] += static_cast<float>(r * gpan * pr);
        if (!v.amp.active()) {
          v.on = false;
          break;
        }
      }
    }
    lfoPh_ += lfoInc_ * n;
    lfoPh_ -= std::floor(lfoPh_);
    if (any) tail_ = static_cast<int64_t>(sr_ * 8);
    if (tail_ <= 0) return;
    tail_ -= n;
    // Global chain: resonators → delay → chorus → reverb.
    for (int i = 0; i < n; ++i) {
      double l = mixL_[i], r = mixR_[i];
      double body = 0, mono = 0.5 * (l + r);
      for (int k = 0; k < kTsRes; ++k) {
        if (!resOn_[k]) continue;
        auto& b = res_[k];
        const size_t N = b.size();
        double rp = static_cast<double>(resW_) - resLen_[k];
        while (rp < 0) rp += static_cast<double>(N);
        size_t i0 = static_cast<size_t>(rp) % N, i1 = (i0 + 1) % N;
        double f = rp - std::floor(rp);
        double dv = b[i0] + (b[i1] - b[i0]) * f;
        double y = mono + resFb_[k] * dv;
        b[resW_ % N] = static_cast<float>(std::fabs(y) < 1e-20 ? 0.0 : y);
        body += dv * (1.0 - 0.5 * resFb_[k]);
      }
      resW_ = (resW_ + 1) % 8192;
      l += resNorm_ * body;
      r += resNorm_ * body;
      if (dlyOn_) {
        const size_t N = dly_[0].size();
        dlyPh_ += dlyModHz_ / sr_;
        if (dlyPh_ >= 1) dlyPh_ -= 1;
        for (int c = 0; c < 2; ++c) {
          double d = (dlySec_ + dlyModDepth_ * std::sin(kTwoPi * (dlyPh_ + 0.25 * c))) * sr_;
          double rp = static_cast<double>(dlyW_) - std::max(1.0, d * (c ? 1.0 : 1.0));
          while (rp < 0) rp += static_cast<double>(N);
          size_t i0 = static_cast<size_t>(rp) % N, i1 = (i0 + 1) % N;
          double f = rp - std::floor(rp);
          double dv = dly_[c][i0] + (dly_[c][i1] - dly_[c][i0]) * f;
          dlyLp_[c] += (dv - dlyLp_[c]) * (1.0 - 0.85 * dlyBlur_);
          double in = c ? r : l;
          dly_[c][dlyW_] = static_cast<float>(in + dlyFb_ * dlyLp_[c]);
          (c ? r : l) = in + dlyWet_ * dlyLp_[c];
        }
        dlyW_ = (dlyW_ + 1) % N;
      }
      if (choOn_) {
        const size_t N = cho_[0].size();
        choPh_ += choHz_ / sr_;
        if (choPh_ >= 1) choPh_ -= 1;
        for (int c = 0; c < 2; ++c) {
          double d = (0.012 + choDepth_ * (1.0 + std::sin(kTwoPi * (choPh_ + 0.5 * c)))) * sr_;
          double rp = static_cast<double>(choW_) - d;
          while (rp < 0) rp += static_cast<double>(N);
          size_t i0 = static_cast<size_t>(rp) % N, i1 = (i0 + 1) % N;
          double f = rp - std::floor(rp);
          double dv = cho_[c][i0] + (cho_[c][i1] - cho_[c][i0]) * f;
          double in = c ? r : l;
          cho_[c][choW_] = static_cast<float>(in);
          (c ? r : l) = in * (1.0 - 0.5 * choWet_) + dv * choWet_;
        }
        choW_ = (choW_ + 1) % N;
      }
      if (revOn_) {
        // 4-line feedback delay network (Householder), damped by the tone.
        double in = 0.25 * (l + r);
        double o[4];
        for (int k = 0; k < 4; ++k) {
          const size_t N = rev_[k].size();
          size_t rp = (revW_[k] + N - revLen_[k]) % N;
          o[k] = rev_[k][rp];
        }
        double s = 0.5 * (o[0] + o[1] + o[2] + o[3]);
        for (int k = 0; k < 4; ++k) {
          const size_t N = rev_[k].size();
          double y = (o[k] - s) * revG_[k];
          revLp_[k] = y + (revLp_[k] - y) * revDamp_;
          double w = in + revLp_[k];
          rev_[k][revW_[k]] = static_cast<float>(std::fabs(w) < 1e-20 ? 0.0 : w);
          revW_[k] = (revW_[k] + 1) % N;
        }
        l = l * (1.0 - 0.5 * revWet_) + revWet_ * (o[0] + o[2]) * 0.6;
        r = r * (1.0 - 0.5 * revWet_) + revWet_ * (o[1] + o[3]) * 0.6;
      }
      L[i] += static_cast<float>(l * level);
      R[i] += static_cast<float>(r * level);
    }
  }

  std::unique_ptr<Voice[]> v_;
  std::unique_ptr<float[]> comb_;
  std::array<float, TsCount> p_{}, def_{};
  double sr_ = 44100, tempo_ = 120;
  uint64_t clock_ = 0;
  int64_t tail_ = 0;
  double lfoPh_ = 0, lfoInc_ = 0, lfoOut_ = 0;
  double modWheel_ = 0, bend_ = 0, pressure_ = 0;
  // Resonators.
  std::array<std::vector<float>, kTsRes> res_;
  bool resOn_[kTsRes] = {};
  double resLen_[kTsRes] = {}, resFb_[kTsRes] = {}, resNorm_ = 0.6;
  size_t resW_ = 0;
  // Delay / chorus / reverb.
  std::vector<float> dly_[2], cho_[2], rev_[4];
  bool dlyOn_ = false, choOn_ = false, revOn_ = false;
  double dlySec_ = 0.3, dlyFb_ = 0, dlyModHz_ = 1, dlyModDepth_ = 0, dlyBlur_ = 0, dlyWet_ = 0, dlyPh_ = 0, dlyLp_[2] = {0, 0};
  size_t dlyW_ = 0, choW_ = 0, revW_[4] = {0, 0, 0, 0}, revLen_[4] = {1, 1, 1, 1};
  double choHz_ = 0.5, choDepth_ = 0.002, choWet_ = 0, choPh_ = 0;
  double revWet_ = 0, revG_[4] = {}, revDamp_ = 0, revLp_[4] = {};
  float mixL_[kChunk] = {}, mixR_[kChunk] = {};
};

}  // namespace

std::unique_ptr<Generator> makeTwinString() { return std::make_unique<TwinStringGen>(); }

}  // namespace gl::packs::models
