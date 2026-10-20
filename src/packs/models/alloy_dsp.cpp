// Gridline — Alloy (gl.alloy) runtime: a metallic harmonic timbre generator.
//
// Timbre: every note builds a harmonic spectrum from two seeded random
// timbres (seed 0 = a fresh random timbre per note) morphed by "Morph", tilted
// by "Fullness", over 2^(richness + 4) harmonics; the timbre EQ curves (x =
// log2(harmonic) / 15) force harmonics on, shape the impulse levels and scale
// the per-harmonic decay / release times. Each harmonic decays exponentially
// (a fast "pre-decay" component plus the main decay; 50 ms .. 20 s, the top
// of the range holds) and releases with its own time. The strongest harmonics
// under Nyquist play as a sine bank (budget shared with the unison copies:
// detune / pan / level / phase spread). Five articulators (volume, filter
// frequency / resonance, pitch, unison pitch: envelope + LFO + key / velocity
// / X / Y maps) run at control rate; the eight mapping targets scale the
// timbre settings at note-on. Then a multimode filter (1–4 stages), magic dust
// (soft saturation + air), chorus / delay / reverb and the 3-band mix EQ.
// Laws: decay / pre-decay / fullness / morph scaling, seed semantics, unison
// gain and articulation ranges follow the reference's behaviour (see
// the J-22 metal-group notes in the task report); the random timbre itself is Gridline's.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../engine/Dsp.h"
#include "metal.h"
#include "metal_rt.h"

namespace gl::packs::models::metal {

namespace {

constexpr int kVoices = 10;
constexpr int kMaxOsc = 320;      // sine oscillators per voice (all unison copies)
constexpr int kMaxHarm = 32768;   // largest timbre (richness 11)
constexpr int kCand = 4096;       // harmonics considered per note (lowest under Nyquist)
constexpr int kCtl = 32;

uint32_t hashU(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
float hashF(uint32_t a, uint32_t b) { return static_cast<float>(hashU(a, b) >> 8) / 16777216.0f; }

// Decay time law (ms) of a 0..1 knob position: 50 ms .. 20 s.
constexpr double kPiD = 3.141592653589793;

// A state-variable filter at `hz` with quality `q` (any Q > 0, below 0.5 too).
void setSvfQ(dsp::Svf& f, double sr, double hz, double q) {
  f.set(sr, hz, 0.0);
  f.k = 1.0 / std::max(0.01, q);
  f.a1 = 1.0 / (1.0 + f.g * (f.g + f.k));
  f.a2 = f.g * f.a1;
  f.a3 = f.g * f.a2;
}

double decayMs(double d) { return (std::pow(21.0, std::clamp(d, 0.0, 1.0)) - 1.0) * 997.5 + 50.0; }

const AlloyAux& defaultAux() {
  static const AlloyAux a = [] {
    AlloyAux x;
    AlloyData d;
    for (int i = 0; i < AaCount; ++i) {
      x.arts[static_cast<size_t>(i)].bip = d.arts[static_cast<size_t>(i)].bipolar;
    }
    for (int e = 0; e < AeCount; ++e) x.eq[static_cast<size_t>(e)].fill(alloyEqNeutral(e));
    return x;
  }();
  return a;
}

}  // namespace

std::shared_ptr<const AuxData> buildAlloyAux(const PluginState& s) {
  AlloyData d;
  if (!parseAlloy(s.data, d)) d = AlloyData{};
  auto a = std::make_shared<AlloyAux>();
  for (int i = 0; i < AaCount; ++i) {
    const AlloyArtData& A = d.arts[static_cast<size_t>(i)];
    AlloyArtPrep& r = a->arts[static_cast<size_t>(i)];
    r.bip = A.bipolar;
    r.env = A.env, r.lfo = A.lfo;
    r.envOn = i < kAlloyEnvArts && A.env.enabled && A.env.pts.size() >= 2;
    r.lfoOn = i < kAlloyEnvArts && A.lfo.enabled && A.lfo.pts.size() >= 2;
    r.envLen = hm::curveLength(A.env);
    r.lfoLen = std::max(1e-6, hm::curveLength(A.lfo));
    auto xOf = [&](int idx) {
      double x = 0;
      for (int k = 1; k <= idx && k < static_cast<int>(A.env.pts.size()); ++k) x += A.env.pts[static_cast<size_t>(k)].dt;
      return x;
    };
    r.susX = A.env.sustain >= 0 ? xOf(A.env.sustain) : -1;
    r.loopX = A.env.loopStart >= 0 && A.env.sustain >= 0 ? xOf(A.env.loopStart) : -1;
    r.envTempo = A.env.tempo, r.lfoTempo = A.lfo.tempo, r.lfoGlobal = A.lfo.global;
    r.lfoPhase = A.lfoPhase;
    const float neutral = r.bip ? 0.5f : 1.0f;
    bool anyMap = false;
    for (int m = 0; m < AmCount; ++m) {
      const hm::Curve& c = A.maps[static_cast<size_t>(m)];
      r.maps[static_cast<size_t>(m)] = c;
      bool flat = true;
      for (const auto& pt : c.pts) flat = flat && std::fabs(pt.y - c.pts[0].y) < 1e-5f;
      r.mapFlat[static_cast<size_t>(m)] = flat;
      r.mapConst[static_cast<size_t>(m)] = c.pts.empty() ? neutral : c.pts[0].y;
      anyMap = anyMap || !flat || std::fabs(r.mapConst[static_cast<size_t>(m)] - neutral) > 1e-5f;
    }
    r.any = r.envOn || r.lfoOn || anyMap;
  }
  for (int e = 0; e < AeCount; ++e) {
    const hm::Curve& c = d.eq[static_cast<size_t>(e)];
    const float neutral = alloyEqNeutral(e);
    a->eqUsed[static_cast<size_t>(e)] = !c.pts.empty();
    const double len = std::max(1e-9, hm::curveLength(c));
    for (int i = 0; i < kAlloyEqPts; ++i)
      a->eq[static_cast<size_t>(e)][static_cast<size_t>(i)] =
          c.pts.empty() ? neutral : static_cast<float>(hm::curveAt(c, len * i / (kAlloyEqPts - 1)));
  }
  return a;
}

namespace {

class AlloyGen final : public Generator {
 public:
  AlloyGen() {
    for (auto& v : v_) v = std::make_unique<Voice>();
  }
  bool active() const override {
    for (const auto& v : v_)
      if (v->on) return true;
    return fx_.ringing();
  }
  bool supportsGlide() const override { return true; }

 protected:
  struct ArtState {
    double x = 0, lfoPh = 0;
    float relFrom = -1, mapBase = 1;
  };
  struct Voice {
    bool on = false, held = false, released = false;
    uint32_t id = 0, serial = 0;
    uint64_t age = 0;
    double note = 60, cur = 60, glideFrom = 60, glideT = 1, glideLen = 0;
    float vel = 1;
    double t = 0;
    int nOsc = 0, ctl = 0;
    uint32_t ph[kMaxOsc], inc[kMaxOsc];
    float harm[kMaxOsc], det[kMaxOsc];      // harmonic number, unison detune ratio
    float a1[kMaxOsc], a2[kMaxOsc];         // main / pre-decay components
    float m1[kMaxOsc], m2[kMaxOsc], mr[kMaxOsc];  // per-sample decay / release multipliers
    float panL[kMaxOsc], panR[kMaxOsc];
    ArtState art[AaCount];
    float gate = 0;
    dsp::Svf flt[2][4];
    float volSm = 0, volT = 0;
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    prep_ = dynamic_cast<const AlloyAux*>(aux.custom.get());
    if (!prep_) prep_ = &defaultAux();
    for (int i = 0; i < AlCount; ++i) p_[i] = prmf(s, i, 0);
    // Effects.
    const float chMix = p_[AlChOrder] >= 1 ? std::clamp((p_[AlChMix] + 128.0f) / 256.0f, 0.0f, 1.0f) : 0.0f;
    const float dlVol = p_[AlDlOn] >= 0.5f ? std::clamp((p_[AlDlVol] + 128.0f) / 256.0f, 0.0f, 1.0f) : 0.0f;
    const double dlSec = p_[AlDlTempo] >= 0.5f ? p_[AlDlTime] / 192.0 * 60.0 / tempo_ : p_[AlDlTime] / 384.0;  // 192 = 1 beat / 384 = 1 s
    const float rvWet = p_[AlRvOn] >= 0.5f ? std::clamp((p_[AlRvWet] + 128.0f) / 256.0f, 0.0f, 1.0f) : 0.0f;
    fx_.setDirect(sr_, chMix, dlVol, std::max(0.01, dlSec), std::clamp(p_[AlDlFeedback] / 127.0f, 0.0f, 0.95f), rvWet,
                  std::clamp(0.5f * p_[AlRvRoom] / 100.0f + 0.5f * p_[AlRvDecay] / 200.0f, 0.0f, 1.0f));
    fx_.setKinds(std::clamp(static_cast<int>(p_[AlChOrder]) - 1, 0, 8), static_cast<int>(p_[AlDlMode]) % 2 == 1 ? 1 : 0,
                 std::clamp(static_cast<int>(p_[AlRvColor]), 0, 4), tempo_);
    // Mix EQ.
    for (int k = 0; k < 3; ++k) {
      const double hz = 20.0 * std::pow(1000.0, std::clamp(p_[AlEqFreq1 + k] / 65536.0, 0.0, 1.0));
      const double db = p_[AlEqLevel1 + k] / 100.0;
      const double bw = 0.1 + 4.0 * std::clamp(p_[AlEqWidth1 + k] / 65536.0, 0.0, 1.0);
      const double q = 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * bw));
      const int type = static_cast<int>(p_[AlEqType1 + k]);
      eqOn_[k] = std::fabs(db) > 0.01;
      for (int c = 0; c < 2; ++c)
        eq_[c][k].set(type == 0 ? dsp::Biquad::LowShelf : type == 2 ? dsp::Biquad::HighShelf : dsp::Biquad::Peak, sr_, hz, q, db);
    }
  }

  // ---- articulators
  float mapValue(const AlloyArtPrep& ap, int m, double x) const {
    const size_t i = static_cast<size_t>(m);
    if (ap.mapFlat[i]) return ap.mapConst[i];
    return static_cast<float>(hm::curveAt(ap.maps[i], x * std::max(1e-9, hm::curveLength(ap.maps[i]))));
  }
  float noteMaps(const AlloyArtPrep& ap, const Voice& v) const {
    const double xs[AmCount] = {v.note / 127.0, v.vel, p_[AlModX] / 512.0, p_[AlModY] / 512.0};
    float acc = ap.bip ? 0.0f : 1.0f;
    for (int m = 0; m < AmCount; ++m) {
      const float y = mapValue(ap, m, xs[m]);
      if (ap.bip) acc += y - 0.5f;
      else acc *= std::max(0.0f, y);
    }
    return acc;
  }
  // Envelope / LFO / maps of an articulation target (unipolar factor or bipolar 0..1 around 0.5).
  float art(Voice& v, int a, double dt) {
    const AlloyArtPrep& ap = prep_->arts[static_cast<size_t>(a)];
    ArtState& st = v.art[a];
    const float neutral = ap.bip ? 0.5f : 1.0f;
    if (!ap.any) return neutral;
    const float kA = std::max(0.01f, p_[alKnob(a, AkAttack)] / 128.0f), kD = std::max(0.01f, p_[alKnob(a, AkDecay)] / 128.0f);
    const float kR = std::max(0.01f, p_[alKnob(a, AkRelease)] / 128.0f), kS = p_[alKnob(a, AkSustain)] / 128.0f;
    float e = neutral;
    if (ap.envOn) {
      const double tScale = ap.envTempo ? tempo_ / 120.0 : 1.0;
      const bool beforeSus = ap.susX < 0 || st.x < ap.susX;
      const double scale = v.released && !beforeSus ? kR : st.x < ap.susX * 0.5 ? kA : kD;
      double nx = st.x + dt * tScale / scale;
      if (!v.released && ap.susX >= 0 && nx >= ap.susX) {
        if (ap.loopX >= 0 && ap.loopX < ap.susX) nx = ap.loopX + std::fmod(nx - ap.susX, std::max(1e-6, ap.susX - ap.loopX));
        else nx = ap.susX;
      }
      st.x = std::min(nx, ap.envLen + 1.0);
      double y = hm::curveAt(ap.env, st.x);
      if (st.relFrom >= 0 && ap.susX >= 0) {
        const double ys = hm::curveAt(ap.env, ap.susX);
        y = ys > 1e-6 ? y * st.relFrom / ys : y;
      }
      if (!v.released) y += kS;
      e = static_cast<float>(ap.bip ? y : std::clamp(y, 0.0, 4.0));
    }
    float l = neutral;
    if (ap.lfoOn) {
      const double hz = (std::pow(10001.0, std::clamp(p_[alKnob(a, AkLfoSpeed)] / 65536.0, 0.0, 1.0)) - 1.0) / 625.0 *
                        (ap.lfoTempo ? tempo_ / 60.0 : 1.0);
      double& ph = ap.lfoGlobal ? globalLfo_[a] : st.lfoPh;
      if (!ap.lfoGlobal) ph += hz * dt;
      ph -= std::floor(ph);
      l = static_cast<float>(hm::curveAt(ap.lfo, ph * ap.lfoLen));
    }
    float val;
    if (ap.bip) val = 0.5f + (e - 0.5f) + (l - 0.5f) + st.mapBase;
    else val = e * l * st.mapBase;
    // X / Y maps follow the live controls.
    return val;
  }
  bool volumeDone(const Voice& v) const {
    const AlloyArtPrep& ap = prep_->arts[AaVolume];
    if (!v.released) return false;
    if (!ap.envOn) return v.gate <= 0.0f;
    return v.art[AaVolume].x >= ap.envLen;
  }

  // ---- notes
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    const double prevNote = lastNote_;
    lastNote_ = pitch;
    if (p_[AlOneVoicePerKey] >= 0.5f)
      for (auto& vp : v_)
        if (vp->on && std::fabs(vp->note - pitch) < 0.01) release(*vp);
    Voice* pick = nullptr;
    for (auto& vp : v_)
      if (!vp->on) {
        pick = vp.get();
        break;
      }
    if (!pick)
      for (auto& vp : v_)
        if (!pick || vp->age < pick->age) pick = vp.get();
    Voice& v = *pick;
    v.on = v.held = true;
    v.released = false;
    v.id = id;
    v.serial = ++serial_;
    v.note = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.t = 0;
    v.ctl = 0;
    v.gate = 0;
    v.volSm = 0;
    v.cur = pitch, v.glideT = 1;
    if (p_[AlPorta] >= 0.5f && prevNote >= 0 && std::fabs(prevNote - pitch) > 0.01) {
      double from = prevNote;
      const int lim = static_cast<int>(p_[AlPortaLimit]);
      double diff = pitch - from;
      if (lim == 1) {
        while (std::fabs(diff) > 12.5) diff -= diff > 0 ? 12 : -12;
      } else if (lim >= 2) {
        diff = std::clamp(diff, -(6.0 - lim), 6.0 - lim);
      }
      from = pitch - diff;
      v.glideFrom = from, v.cur = from, v.glideT = 0;
      v.glideLen = 0.08 * (p_[AlPortaVar] >= 0.5f ? std::max(0.25, std::fabs(diff) / 12.0) : 1.0);
    }
    for (auto& f : v.flt)
      for (auto& s : f) s.reset();
    for (int a = 0; a < AaCount; ++a) {
      const AlloyArtPrep& ap = prep_->arts[static_cast<size_t>(a)];
      v.art[a] = ArtState{};
      v.art[a].lfoPh = ap.lfoPhase;
      v.art[a].mapBase = noteMaps(ap, v);
    }
    buildTimbre(v);
  }
  void release(Voice& v) {
    if (!v.held) return;
    v.held = false;
    v.released = true;
    for (int a = 0; a < kAlloyEnvArts; ++a) {
      const AlloyArtPrep& ap = prep_->arts[static_cast<size_t>(a)];
      ArtState& st = v.art[a];
      if (ap.envOn && ap.susX >= 0 && st.x < ap.susX) {
        st.relFrom = static_cast<float>(hm::curveAt(ap.env, st.x));
        st.x = ap.susX;
      }
    }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& vp : v_)
      if (vp->on && vp->held && vp->id == id) release(*vp);
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& vp : v_)
      if (vp->on && vp->id == id) vp->glideFrom = vp->cur, vp->note = pitch, vp->glideT = 0, vp->glideLen = std::max(0.001, seconds);
  }
  void onAllOff() override {
    for (auto& vp : v_)
      if (vp->on) release(*vp);
  }
  void onKill() override {
    for (auto& vp : v_) vp->on = false;
    fx_.reset();
    for (auto& c : eq_)
      for (auto& b : c) b.clear();
  }

  // Mapping target at note-on: unipolar factor or bipolar offset (0.5 neutral) → factor.
  // As a factor (1 = neutral; bipolar maps span 0..2) …
  float mapTarget(const Voice& v, int a) const {
    const AlloyArtPrep& ap = prep_->arts[static_cast<size_t>(a)];
    if (!ap.any) return 1.0f;
    return ap.bip ? std::max(0.0f, 1.0f + 2.0f * v.art[a].mapBase) : v.art[a].mapBase;
  }
  // … or as an offset (0 = neutral, -0.5..0.5).
  float mapOffset(const Voice& v, int a) const { return (mapTarget(v, a) - 1.0f) * 0.5f; }
  float eqAt(int e, double x) const {
    const auto& t = prep_->eq[static_cast<size_t>(e)];
    const double u = std::clamp(x, 0.0, 1.0) * (kAlloyEqPts - 1);
    const int i0 = static_cast<int>(u), i1 = std::min(kAlloyEqPts - 1, i0 + 1);
    const float f = static_cast<float>(u - i0);
    return t[static_cast<size_t>(i0)] * (1 - f) + t[static_cast<size_t>(i1)] * f;
  }

  // The note's harmonic set (audio thread; fixed-size scratch, no allocation).
  void buildTimbre(Voice& v) {
    const int richness = std::clamp(static_cast<int>(std::lround(p_[AlRichness])), 1, 11);
    const int harmonics = std::min(kMaxHarm, 1 << (richness + 4));
    const double f0 = dsp::noteToHz(notePitch(v, 0.5f));
    const int nyqH = std::max(1, static_cast<int>(sr_ * 0.45 / std::max(1.0, f0)));
    const int K = std::min({harmonics, nyqH, kCand});
    // Seeds: 0 = a new random timbre for every note; key → seed 1 option.
    const int s1 = static_cast<int>(p_[AlSeed1]), s2 = static_cast<int>(p_[AlSeed2]);
    uint32_t seed1 = s1 > 0 ? hashU(static_cast<uint32_t>(s1) + 19u, 499u) : hashU(v.serial, 77u);
    if (p_[AlKeySeed] >= 0.5f) seed1 = hashU(static_cast<uint32_t>(std::lround(v.note)) + static_cast<uint32_t>(s1) + 20u, 173u);
    const uint32_t seed2 = s2 > 0 ? hashU(static_cast<uint32_t>(s2) + 19u, 499u) : hashU(++randCounter_ + 20u, 307u);
    const float morph = std::clamp(p_[AlMorph] / 1000.0f * mapTarget(v, AaMorph), 0.0f, 1.0f);
    const float full = std::clamp(p_[AlFullness] / 1000.0f + 4.0f * mapOffset(v, AaFullness), -1.0f, 1.0f);
    const float tilt = -1.6f + 1.25f * full;  // level ∝ k^tilt
    const float preAmt = std::clamp(p_[AlPreDecay] / 1000.0f * mapTarget(v, AaPreDecay), 0.0f, 1.0f);
    const double decayK = p_[AlDecay] >= 10000.0f ? -1.0 : p_[AlDecay] / 10000.0 * mapTarget(v, AaDecay);
    const double relK = p_[AlRelease] / 10000.0 * mapTarget(v, AaRelease);
    const float rot = mapOffset(v, AaRotation);
    const float eqShift = mapOffset(v, AaEqShift);
    const int comb = static_cast<int>(p_[AlComb]);
    const bool alt = p_[AlAltDecay] >= 0.5f;
    // Candidate levels.
    int n = 0;
    const int rotOfs = static_cast<int>(std::lround(rot * 64.0));
    for (int k = 1; k <= K; ++k) {
      if (comb >= 1 && comb <= 4 && k % (comb + 1) == 0) continue;       // remove every 2nd..5th
      if (comb >= 5 && comb <= 7 && k > 1 && k % (comb - 2) != 0) continue;  // keep every 3rd..5th
      const uint32_t hk = static_cast<uint32_t>(std::max(1, k + rotOfs));
      const float r1 = hashF(seed1, hk), r2 = hashF(seed2, hk);
      // Sparse "metallic" random spectra: most harmonics weak, some strong.
      const float t1 = r1 * r1 * r1 * r1, t2 = r2 * r2 * r2 * r2;
      float lvl = (t1 * (1 - morph) + t2 * morph) * std::pow(static_cast<float>(k), tilt);
      if (k <= 2) lvl = std::max(lvl, 0.35f * std::pow(static_cast<float>(k), tilt));  // keep a fundamental
      const double ex = std::clamp(std::log2(static_cast<double>(k)) / 15.0 + (eqShift - 0.0) * 0.5, 0.0, 1.0);
      if (prep_->eqUsed[AeForced]) lvl = std::max(lvl, eqAt(AeForced, ex) * std::pow(static_cast<float>(k), tilt));
      if (prep_->eqUsed[AeImpulse]) lvl *= std::pow(4.0f, (eqAt(AeImpulse, ex) - 0.5f) * 2.0f);
      cand_[n] = lvl, candK_[n] = k;
      ++n;
    }
    // Keep the strongest harmonics within the oscillator budget.
    const int uni = std::clamp(static_cast<int>(std::lround(p_[AlUniOrder])), 1, 9);
    const int budget = std::max(8, kMaxOsc / uni);
    for (int i = 0; i < n; ++i) order_[i] = i;
    if (n > budget) {
      std::nth_element(order_, order_ + budget, order_ + n, [&](int a, int b) { return cand_[a] > cand_[b]; });
      n = budget;
    }
    // J-41: the partials are normalised to unit power (the sum of their
    // squared levels), not to their summed peak: a rich timbre keeps the level
    // of a single partial instead of dropping by the number of partials.
    float sum2 = 0;
    for (int i = 0; i < n; ++i) sum2 += cand_[order_[i]] * cand_[order_[i]];
    const float norm = sum2 > 1e-18f ? 1.0f / std::sqrt(sum2) : 0.0f;
    // Unison copies.
    const float uniPitch = p_[AlUniPitch] / 256.0f;   // 0..1 → spread up to ±35 cents
    const float uniPan = p_[AlUniPan] / 128.0f, uniVol = p_[AlUniVol] / 128.0f, uniPhase = p_[AlUniPhase] / 128.0f;
    const float uniGain = uni < 3 ? 1.0f : 1.0f / std::sqrt(static_cast<float>(uni));
    int o = 0;
    for (int u = 0; u < uni; ++u) {
      const float pos = uni == 1 ? 0.0f : static_cast<float>(u) / (uni - 1) * 2.0f - 1.0f;  // -1..1
      const float cents = pos * 35.0f * uniPitch * uniPitch * 4.0f;
      const float detune = std::pow(2.0f, cents / 1200.0f);
      const float pan = pos * uniPan * ((u & 1) ? -1.0f : 1.0f);
      const float gain = uniGain * (u == 0 ? 1.0f : uniVol);
      const float pl = std::sqrt(0.5f * (1 - pan)), pr = std::sqrt(0.5f * (1 + pan));
      for (int i = 0; i < n && o < kMaxOsc; ++i, ++o) {
        const int c = order_[i];
        const int k = candK_[c];
        const double ex = std::clamp(std::log2(static_cast<double>(k)) / 15.0 + eqShift * 0.5, 0.0, 1.0);
        double tauMs = decayK < 0 ? 1e9 : decayMs(decayK);
        if (prep_->eqUsed[AeDecay]) tauMs *= std::pow(4.0, (eqAt(AeDecay, ex) - 0.5) * 2.0);
        tauMs /= 1.0 + (alt ? 0.05 : 0.18) * std::log2(static_cast<double>(k));  // higher harmonics die sooner
        double relMs = decayMs(relK);
        if (prep_->eqUsed[AeRelease]) relMs *= std::pow(4.0, (eqAt(AeRelease, ex) - 0.5) * 2.0);
        const float lvl = cand_[c] * norm * gain;
        v.harm[o] = static_cast<float>(k), v.det[o] = detune;
        v.a1[o] = lvl * (1 - preAmt), v.a2[o] = lvl * preAmt * 3.0f;
        v.m1[o] = tauMs > 1e8 ? 1.0f : static_cast<float>(std::exp(-1.0 / (tauMs * 0.001 * sr_ / 6.9)));
        v.m2[o] = static_cast<float>(std::exp(-1.0 / (std::max(5.0, tauMs * 0.04) * 0.001 * sr_ / 6.9)));
        v.mr[o] = static_cast<float>(std::exp(-1.0 / (relMs * 0.001 * sr_ / 6.9)));
        v.panL[o] = pl, v.panR[o] = pr;
        const double ph0 = uni > 1 ? uniPhase * hashF(v.serial * 31u + static_cast<uint32_t>(u), static_cast<uint32_t>(k)) : 0.0;
        v.ph[o] = static_cast<uint32_t>(ph0 * 4294967296.0);
      }
    }
    v.nOsc = o;
  }
  double notePitch(const Voice& v, float pitchArt) const {
    const double base = p_[AlKeyPitch] >= 0.5f ? v.cur : 60.0;
    return base + p_[AlCoarse] + p_[AlFine] / 100.0 + (pitchArt - 0.5) * 4.0 * 12.0;
  }

  void control(Voice& v, double dt) {
    v.t += dt;
    if (v.glideT < 1) {
      v.glideT = std::min(1.0, v.glideT + dt / std::max(1e-4, v.glideLen));
      v.cur = v.glideFrom + (v.note - v.glideFrom) * v.glideT;
    } else {
      v.cur = v.note;
    }
    const float aVol = art(v, AaVolume, dt), aCut = art(v, AaCutoff, dt), aRes = art(v, AaReso, dt), aPitch = art(v, AaPitch, dt),
                aUni = art(v, AaUniPitch, dt);
    // Gate (when the volume envelope is off): declick attack, release fades with the timbre release.
    if (!prep_->arts[AaVolume].envOn) {
      const float att = p_[AlDeclick] >= 0.5f ? 0.004f : 0.0005f;
      v.gate = v.released ? std::max(0.0f, v.gate - static_cast<float>(dt / 0.6)) : std::min(1.0f, v.gate + static_cast<float>(dt / att));
    } else {
      v.gate = 1;
    }
    const double f0 = dsp::noteToHz(notePitch(v, aPitch));
    const double uniScale = std::max(0.0f, aUni);  // unison pitch articulation scales the detune
    for (int o = 0; o < v.nOsc; ++o) {
      const double det = std::pow(static_cast<double>(v.det[o]), uniScale);
      v.inc[o] = phaseInc(f0 * v.harm[o] * det, sr_);
    }
    const float velG = p_[AlVelVol] >= 0.5f ? 0.2f + 0.8f * v.vel : 1.0f;
    // Each unison voice leaves at 0.3 per channel (centre: 0.3 / 0.7071 before the pan law).
    v.volT = std::max(0.0f, aVol) * v.gate * velG * p_[AlVolume] / 128.0f * 0.42426f;
    // Filter.
    const int stages = std::clamp(static_cast<int>(p_[AlFltMode]), 0, 4);
    if (stages > 0) {
      // J-41: the reference filter law. The cutoff is a fixed frequency
      // (not relative to the note): low pass c → (1501^c − 1)·0.000664667 of
      // Nyquist (+0.003), band / notch / high pass (1501^c − 1)·0.00039993
      // (+0.0001; at most 1.0001 / 0.6001). Resonance r: low / high pass
      // Q = (2001^r − 1)·0.02495 + 0.1 (50 at the top); band / notch a
      // bandwidth (101^r − 1)·0.1199 + 0.01 octaves (12 at the top). The
      // alternate mode takes 0.5·(c + 1)·r as the resonance input. Every
      // stage gets the same Q.
      const int ftype = static_cast<int>(p_[AlFltType]);
      const double c = std::clamp(p_[AlCutoff] / 256.0 + (aCut - 0.5) * 0.5, 0.0, 1.0);
      double r = std::clamp(p_[AlReso] / 256.0 + (aRes - 0.5), 0.0, 1.0);
      if (p_[AlFltFlat] >= 0.5f) r = 0.5 * (c + 1.0) * r;
      const double e = c > 0 ? std::pow(1501.0, c) - 1.0 : 0.0;
      double fn, off, q;
      if (ftype == 1 || ftype == 2) {
        fn = std::min(e * 0.0003999333333333333, 1.0001), off = 0.0001;
      } else if (ftype == 3) {
        fn = std::min(e * 0.0003999333333333333, 0.6001), off = 0.0001;
      } else {
        fn = e * 0.0006646666666666667, off = 0.003;
      }
      const double w = std::clamp((fn + off) * kPiD * 44100.0 / sr_, 1e-5, 2.849517146113191);
      if (ftype == 1 || ftype == 2) {
        const double bw = r < 1.0 ? (std::pow(101.0, r) - 1.0) * 0.1199 + 0.01 : 12.0;
        q = 1.0 / (2.0 * std::sinh(0.34657359027997264 * bw * w / std::sin(w)));
      } else {
        q = r < 1.0 ? (std::pow(2001.0, r) - 1.0) * 0.02495 + 0.1 : 50.0;
      }
      const double hz = w * sr_ / (2.0 * kPiD);
      for (auto& ch : v.flt)
        for (int s = 0; s < stages; ++s) setSvfQ(ch[s], sr_, hz, q);
    }
    if (volumeDone(v)) v.on = false;
  }

  void render(float* L, float* R, int n) override {
    const SineTable& sine = sineTable();
    const double dt = kCtl / sr_;
    const int stages = std::clamp(static_cast<int>(p_[AlFltMode]), 0, 4);
    const int ftype = static_cast<int>(p_[AlFltType]);
    for (auto& vp : v_) {
      Voice& v = *vp;
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        if (v.ctl <= 0) {
          control(v, dt);
          v.ctl = kCtl;
          if (!v.on) break;
        }
        --v.ctl;
        v.volSm += (v.volT - v.volSm) * 0.05f;
        float l = 0, r = 0;
        const bool rel = v.released;
        for (int o = 0; o < v.nOsc; ++o) {
          v.ph[o] += v.inc[o];
          const float s = sine.at(v.ph[o]) * (v.a1[o] + v.a2[o]);
          l += s * v.panL[o], r += s * v.panR[o];
          v.a1[o] *= rel ? v.m1[o] * v.mr[o] : v.m1[o];
          v.a2[o] *= v.m2[o];
          if (v.inc[o] == 0) v.a1[o] = v.a2[o] = 0;
        }
        float out[2] = {l * v.volSm, r * v.volSm};
        if (stages > 0)
          for (int c = 0; c < 2; ++c) {
            double x = out[c];
            for (int s = 0; s < stages; ++s) {
              double band = 0, high = 0;
              const double low = v.flt[c][s].process(x, &band, &high);
              x = ftype == 1 ? band * v.flt[c][s].k : ftype == 2 ? low + high : ftype == 3 ? high : low;  // band pass: 0 dB peak
            }
            out[c] = static_cast<float>(x);
          }
        L[i] += out[0];
        R[i] += out[1];
      }
      // Done when every harmonic has died away (no sustain in the timbre).
      if (v.on && (v.t > 0.05)) {
        float peak = 0;
        for (int o = 0; o < v.nOsc; ++o) peak = std::max(peak, v.a1[o] + v.a2[o]);
        if (peak < 1e-6f) v.on = false;
      }
    }
    // Magic dust: gentle saturation plus air.
    const float dust = std::clamp(p_[AlDust] / 1000.0f, 0.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
      for (int c = 0; c < 2; ++c) {
        float* x = c ? &R[i] : &L[i];
        float y = *x;
        if (dust > 0) {
          const float drive = 1.0f + 2.5f * dust;
          y = std::tanh(y * drive) / std::tanh(drive) * (1.0f - 0.15f * dust) + y * 0.15f * dust;
          air_[c] += (y - air_[c]) * 0.35f;
          y += (y - air_[c]) * 0.6f * dust;
        }
        for (int k = 0; k < 3; ++k)
          if (eqOn_[k]) y = static_cast<float>(eq_[c][k].process(y));
        *x = y;
      }
    }
    fx_.process(L, R, n);
  }

 private:
  std::unique_ptr<Voice> v_[kVoices];
  float p_[AlCount] = {};
  const AlloyAux* prep_ = nullptr;
  ModelFx fx_;
  dsp::Biquad eq_[2][3];
  bool eqOn_[3] = {};
  float air_[2] = {};
  double globalLfo_[AaCount] = {};
  float cand_[kCand] = {};
  int candK_[kCand] = {}, order_[kCand] = {};
  double sr_ = 44100, tempo_ = 120, lastNote_ = -1;
  uint32_t serial_ = 0, randCounter_ = 0;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeAlloy() { return std::make_unique<AlloyGen>(); }

}  // namespace gl::packs::models::metal
