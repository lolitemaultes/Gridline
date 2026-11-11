// Gridline: the <effect> buses of SFZ instruments the Multisampler plays (K-144). Each effect type
// of the format is built here from Gridline's own DSP blocks. Buffers are sized on the UI thread
// (Fx::prepare) for up to 192 kHz; process() never allocates.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../core/SfzMap.h"
#include "../../engine/Dsp.h"

namespace gl::packs::samplers::sfzfx {

enum Type { TNone, TApan, TComp, TDelay, TDisto, TEq, TFilter, TReverb, TGate, TLofi, TPhaser, TStatic, TStrings, TTdfir, TLimiter };

// Parameter names per type (index = slot in Def::p) and defaults.
struct P {
  const char* name;
  float def;
};
inline const std::vector<P>& paramsOf(Type t) {
  static const std::vector<P> none{};
  static const std::vector<P> apan{{"apan_depth", 50}, {"apan_freq", 1}, {"apan_phase", 180}, {"apan_waveform", 0}, {"apan_dry", 0}, {"apan_wet", 100}};
  static const std::vector<P> comp{{"comp_attack", 0.01f}, {"comp_release", 0.1f}, {"comp_ratio", 4}, {"comp_threshold", -20}, {"comp_gain", 0}, {"comp_stlink", 1}};
  static const std::vector<P> delay{{"delay_timel", 0.25f}, {"delay_timer", 0.25f}, {"delay_timec", 0.25f}, {"delay_feedback", 30}, {"delay_wet", 30},
                                    {"delay_dry", 100}, {"delay_damphi", 0}, {"delay_damplo", 0}, {"delay_input", 100}, {"delay_levell", 100},
                                    {"delay_levelr", 100}, {"delay_levelc", 0}, {"delay_panl", -100}, {"delay_panr", 100}, {"delay_panc", 0},
                                    {"delay_mode", 0}, {"delay_cutoff", 0}, {"delay_resonance", 0}, {"delay_lfofreq", 0}, {"delay_moddepth", 0},
                                    {"delay_spread", 0}, {"delay_time_tap", 0}, {"delay_syncl", 0}, {"delay_syncr", 0}, {"delay_syncc", 0}, {"delay_filter", 0}};
  static const std::vector<P> disto{{"disto_depth", 50}, {"disto_tone", 100}, {"disto_stages", 1}, {"disto_wet", 100}, {"disto_dry", 0}};
  static const std::vector<P> eq{{"eq_freq", 1000}, {"eq_bw", 1}, {"eq_gain", 0}, {"eq_type", 0}};
  static const std::vector<P> filter{{"filter_cutoff", 1000}, {"filter_resonance", 0}, {"filter_type", 2}};
  static const std::vector<P> reverb{{"reverb_size", 50}, {"reverb_damp", 50}, {"reverb_predelay", 0}, {"reverb_tone", 100}, {"reverb_wet", 30},
                                     {"reverb_dry", 100}, {"reverb_input", 100}, {"reverb_type", 0}};
  static const std::vector<P> gate{{"gate_threshold", -60}, {"gate_attack", 0.001f}, {"gate_release", 0.05f}, {"gate_stlink", 1}};
  static const std::vector<P> lofi{{"bitred", 0}, {"decim", 0}};
  static const std::vector<P> phaser{{"phaser_depth", 50}, {"phaser_feedback", 50}, {"phaser_freq", 0.5f}, {"phaser_stages", 4},
                                     {"phaser_waveform", 0}, {"phaser_wet", 50}, {"phaser_phase", 0}};
  static const std::vector<P> stat{{"static_level", 0}, {"static_tone", 50}, {"static_filter", 0}, {"static_stereo", 1}, {"static_cyclic_level", 0},
                                   {"static_cyclic_time", 1}, {"static_random_level", 0}, {"static_random_mintime", 0.05f}, {"static_random_maxtime", 0.5f}};
  static const std::vector<P> strings{{"strings_number", 12}, {"strings_wet", 30}};
  static const std::vector<P> tdfir{{"tdfir_gain", 0}, {"tdfir_wet", 100}, {"tdfir_dry", 0}};
  switch (t) {
    case TApan: return apan;
    case TComp: case TLimiter: return comp;
    case TDelay: return delay;
    case TDisto: return disto;
    case TEq: return eq;
    case TFilter: return filter;
    case TReverb: return reverb;
    case TGate: return gate;
    case TLofi: return lofi;
    case TPhaser: return phaser;
    case TStatic: return stat;
    case TStrings: return strings;
    case TTdfir: return tdfir;
    default: return none;
  }
}
inline Type typeOf(const std::string& s) {
  if (s == "apan") return TApan;
  if (s == "comp" || s == "compressor") return TComp;
  if (s == "limiter") return TLimiter;
  if (s == "delay") return TDelay;
  if (s == "disto" || s == "distortion") return TDisto;
  if (s == "eq") return TEq;
  if (s == "filter") return TFilter;
  if (s == "fverb" || s == "reverb") return TReverb;
  if (s == "gate") return TGate;
  if (s == "lofi") return TLofi;
  if (s == "phaser") return TPhaser;
  if (s == "static") return TStatic;
  if (s == "strings") return TStrings;
  if (s == "tdfir") return TTdfir;
  return TNone;
}

constexpr int kMaxParams = 32;
struct Def {
  Type type = TNone;
  int bus = 0;  // 0 main, 1..4 fx1..fx4, 5..12 aux1..aux8
  std::array<float, kMaxParams> p{};
  struct Cc {
    int param, cc;
    float depth;
  };
  std::vector<Cc> cc;
  int bypassCc = -1;
  int gateCc = -1;
  float directToMain = 100;
  std::array<float, 4> fxToMain{}, fxToMix{};
  std::vector<float> ir;  // tdfir impulse (mono, at most 2048 taps)
};

inline int busOf(const std::string& b) {
  if (b.rfind("fx", 0) == 0 && b.size() == 3 && b[2] >= '1' && b[2] <= '4') return b[2] - '0';
  if (b.rfind("aux", 0) == 0 && b.size() == 4 && b[3] >= '1' && b[3] <= '8') return 4 + (b[3] - '0');
  return 0;
}

inline Def makeDef(const sfz::EffectDef& e) {
  Def d;
  d.type = typeOf(e.type);
  d.bus = busOf(e.bus);
  d.directToMain = e.directToMain;
  for (int k = 0; k < 4; ++k) d.fxToMain[static_cast<size_t>(k)] = e.fxToMain[k], d.fxToMix[static_cast<size_t>(k)] = e.fxToMix[k];
  const auto& ps = paramsOf(d.type);
  for (size_t k = 0; k < ps.size() && k < kMaxParams; ++k) d.p[k] = ps[k].def;
  for (const auto& [name, val] : e.params) {
    const float v = static_cast<float>(std::strtod(val.c_str(), nullptr));
    if (name == "filter_type") {
      d.p[2] = static_cast<float>(std::max(0, sfz::filterTypeOf(val)));
      continue;
    }
    if (name == "eq_type") {
      d.p[3] = val == "lshelf" ? 1.f : val == "hshelf" ? 2.f : 0.f;
      continue;
    }
    if (name == "delay_mode") {
      d.p[15] = val == "pingpong" || val == "ping_pong" || v > 0.5f ? 1.f : 0.f;
      continue;
    }
    if (name == "delay_filter") {
      d.p[25] = static_cast<float>(std::max(0, sfz::filterTypeOf(val)));
      continue;
    }
    if (name == "static_filter") {
      d.p[2] = static_cast<float>(std::max(0, sfz::filterTypeOf(val)));
      continue;
    }
    // controllers: name_onccN
    const size_t on = name.rfind("_oncc");
    if (on != std::string::npos) {
      const std::string base = name.substr(0, on);
      const int cc = std::atoi(name.c_str() + on + 5);
      if (base == "bypass") {
        d.bypassCc = cc;
        continue;
      }
      if (base == "gate") {
        d.gateCc = cc;
        continue;
      }
      for (size_t k = 0; k < ps.size(); ++k)
        if (base == ps[k].name) d.cc.push_back({static_cast<int>(k), cc, v});
      continue;
    }
    for (size_t k = 0; k < ps.size(); ++k)
      if (name == ps[k].name) d.p[k] = v;
  }
  return d;
}

// --------------------------------------------------------------------- state
struct Ring {
  std::vector<float> b;
  size_t w = 0, mask = 0;
  void size(size_t n) {
    size_t s = 1;
    while (s < n) s <<= 1;
    b.assign(s, 0.f);
    mask = s - 1;
    w = 0;
  }
  void push(float x) {
    if (b.empty()) return;
    b[w] = x;
    w = (w + 1) & mask;
  }
  float tap(double d) const {
    if (b.empty()) return 0.f;
    d = std::clamp(d, 1.0, static_cast<double>(mask - 2));
    const size_t i = static_cast<size_t>(d);
    const float fr = static_cast<float>(d - static_cast<double>(i));
    const float a = b[(w - i) & mask], c = b[(w - i - 1) & mask];
    return a + (c - a) * fr;
  }
  void clear() { std::fill(b.begin(), b.end(), 0.f); }
};

struct State {
  // generic
  double ph = 0, env[2] = {0, 0}, gain = 1, z[8] = {};
  dsp::Biquad bq[2][3];
  dsp::Svf svf[2][3];
  Ring ring[3];                     // delay lines / predelay
  std::array<Ring, 8> comb;         // reverb / strings
  std::array<Ring, 4> ap;           // reverb diffusers
  std::array<dsp::Allpass1, 24> phaserAp;
  std::vector<float> firHist;       // tdfir history (2 x taps)
  size_t firW = 0;
  uint32_t rng = 0x1234567u;
  double holdL = 0, holdR = 0, holdT = 0, staticNext = 0, staticCycle = 0;
  void prepare(const Def& d) {
    switch (d.type) {
      case TDelay:
        for (auto& r : ring) r.size(static_cast<size_t>(192000 * 2));
        break;
      case TReverb:
        ring[0].size(static_cast<size_t>(192000 * 0.5));
        for (auto& c : comb) c.size(static_cast<size_t>(192000 * 0.12));
        for (auto& a : ap) a.size(static_cast<size_t>(192000 * 0.03));
        break;
      case TStrings:
        for (auto& c : comb) c.size(static_cast<size_t>(192000 / 30));
        break;
      case TTdfir: firHist.assign(d.ir.size() * 2 + 2, 0.f); break;
      default: break;
    }
  }
  void reset() {
    for (auto& r : ring) r.clear();
    for (auto& c : comb) c.clear();
    for (auto& a : ap) a.clear();
    std::fill(firHist.begin(), firHist.end(), 0.f);
    for (auto& b : bq)
      for (auto& x : b) x.clear();
    for (auto& s : svf)
      for (auto& x : s) x.ic1 = x.ic2 = 0;
    for (auto& a : phaserAp) a.reset();
    env[0] = env[1] = 0, gain = 1, ph = 0;
    for (double& x : z) x = 0;
  }
};

inline uint32_t rnd(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
inline float rndf(uint32_t& s) { return static_cast<float>(rnd(s) & 0xFFFFFF) / 16777215.0f; }

inline double lfoWave(int w, double ph) {
  ph -= std::floor(ph);
  switch (w) {
    case 1: return 1.0 - 4.0 * std::fabs(ph - 0.5);  // triangle
    case 2: return ph < 0.5 ? 1.0 : -1.0;             // square
    case 3: return 2.0 * ph - 1.0;                    // saw
    default: return std::sin(dsp::kTwoPi * ph);
  }
}

// Processes one effect over n frames in place. cc: controller values 0..1 (512 slots).
inline void process(const Def& d, State& s, float* L, float* R, int n, double sr, double tempo, const float* cc) {
  if (d.type == TNone) return;
  if (d.bypassCc >= 0 && d.bypassCc < sfz::kCcCount && cc[d.bypassCc] >= 0.5f) return;
  std::array<float, kMaxParams> p = d.p;
  for (const auto& c : d.cc)
    if (c.cc >= 0 && c.cc < sfz::kCcCount) p[static_cast<size_t>(c.param)] += c.depth * cc[c.cc];
  auto pct = [&](int k) { return static_cast<double>(p[static_cast<size_t>(k)]) / 100.0; };
  switch (d.type) {
    case TApan: {
      const double depth = std::clamp(pct(0), 0.0, 1.0), inc = std::max(0.0, static_cast<double>(p[1])) / sr, phase = p[2] / 360.0;
      const int wave = static_cast<int>(p[3]);
      const double dry = pct(4), wet = pct(5);
      for (int i = 0; i < n; ++i) {
        const double gl = 1.0 - depth * (0.5 + 0.5 * lfoWave(wave, s.ph)), gr = 1.0 - depth * (0.5 + 0.5 * lfoWave(wave, s.ph + phase));
        s.ph += inc;
        if (s.ph >= 1) s.ph -= 1;
        L[i] = static_cast<float>(L[i] * (dry + wet * gl));
        R[i] = static_cast<float>(R[i] * (dry + wet * gr));
      }
      break;
    }
    case TComp:
    case TLimiter: {
      const double att = std::exp(-1.0 / std::max(1.0, p[0] * sr)), rel = std::exp(-1.0 / std::max(1.0, p[1] * sr));
      const double ratio = d.type == TLimiter ? 100.0 : std::max(1.0, static_cast<double>(p[2])), thr = p[3], makeup = std::pow(10.0, p[4] / 20.0);
      const bool link = p[5] > 0.5f;
      for (int i = 0; i < n; ++i) {
        const double al = std::fabs(L[i]), ar = std::fabs(R[i]);
        const double lv[2] = {link ? std::max(al, ar) : al, link ? std::max(al, ar) : ar};
        double g[2];
        for (int c = 0; c < 2; ++c) {
          s.env[c] = lv[c] > s.env[c] ? lv[c] + (s.env[c] - lv[c]) * att : lv[c] + (s.env[c] - lv[c]) * rel;
          const double db = 20.0 * std::log10(std::max(1e-9, s.env[c]));
          const double over = db - thr;
          g[c] = over > 0 ? std::pow(10.0, -(over - over / ratio) / 20.0) : 1.0;
        }
        L[i] = static_cast<float>(L[i] * g[0] * makeup);
        R[i] = static_cast<float>(R[i] * g[1] * makeup);
      }
      break;
    }
    case TDelay: {
      const double beat = 60.0 / std::max(1.0, tempo);
      double t[3] = {p[22] > 0 ? p[22] * beat : p[0], p[23] > 0 ? p[23] * beat : p[1], p[24] > 0 ? p[24] * beat : p[2]};
      if (p[21] > 0) t[0] = t[1] = t[2] = p[21];
      const double fb = std::clamp(pct(3), 0.0, 0.98), wet = pct(4), dry = pct(5), input = pct(8);
      const double lv[3] = {pct(9), pct(10), pct(11)};
      const double pan[3] = {p[12] / 100.0, p[13] / 100.0, p[14] / 100.0};
      const bool ping = p[15] > 0.5f;
      const double damp = p[6] > 0 ? std::exp(-dsp::kTwoPi * std::clamp(static_cast<double>(p[6]), 20.0, sr * 0.45) / sr) : 0.0;
      const double lfoInc = p[18] / sr, mod = p[19] / 1000.0 * sr, spread = p[20] / 1000.0 * sr;
      for (int i = 0; i < n; ++i) {
        const double in = (L[i] + R[i]) * 0.5 * input;
        const double m = mod > 0 ? mod * (0.5 + 0.5 * std::sin(dsp::kTwoPi * s.ph)) : 0.0;
        s.ph += lfoInc;
        if (s.ph >= 1) s.ph -= 1;
        double outL = 0, outR = 0;
        double taps[3];
        for (int k = 0; k < 3; ++k) taps[k] = s.ring[k].tap(std::max(1.0, t[k] * sr + m + (k == 1 ? spread : 0.0)));
        for (int k = 0; k < 3; ++k) {
          const double y = taps[k] * lv[k];
          outL += y * std::min(1.0, 1.0 - pan[k]);
          outR += y * std::min(1.0, 1.0 + pan[k]);
        }
        for (int k = 0; k < 3; ++k) {
          double back = ping ? (k == 0 ? taps[1] : k == 1 ? taps[0] : taps[2]) : taps[k];
          if (damp > 0) back = s.z[k] = back + (s.z[k] - back) * damp;
          s.ring[k].push(static_cast<float>(in + back * fb));
        }
        L[i] = static_cast<float>(L[i] * dry + outL * wet);
        R[i] = static_cast<float>(R[i] * dry + outR * wet);
      }
      break;
    }
    case TDisto: {
      const double drive = 1.0 + 30.0 * std::clamp(pct(0), 0.0, 1.0), wet = pct(3), dry = pct(4);
      const int stages = std::clamp(static_cast<int>(p[2]), 1, 8);
      const double tone = std::exp(-dsp::kTwoPi * (200.0 + 18000.0 * std::clamp(pct(1), 0.0, 1.0)) / sr);
      for (int i = 0; i < n; ++i) {
        double x[2] = {L[i], R[i]};
        for (int c = 0; c < 2; ++c) {
          double y = x[c];
          for (int k = 0; k < stages; ++k) y = std::tanh(y * drive) / std::tanh(drive);
          y = s.z[c] = y + (s.z[c] - y) * tone;
          x[c] = x[c] * dry + y * wet;
        }
        L[i] = static_cast<float>(x[0]);
        R[i] = static_cast<float>(x[1]);
      }
      break;
    }
    case TEq: {
      const int ty = static_cast<int>(p[3]);
      const dsp::Biquad::Type bt = ty == 1 ? dsp::Biquad::LowShelf : ty == 2 ? dsp::Biquad::HighShelf : dsp::Biquad::Peak;
      const double bw = std::max(0.05, static_cast<double>(p[1])), q = std::sqrt(std::exp2(bw)) / (std::exp2(bw) - 1.0);
      for (int c = 0; c < 2; ++c) s.bq[c][0].set(bt, sr, p[0], q, p[2]);
      for (int i = 0; i < n; ++i) L[i] = static_cast<float>(s.bq[0][0].process(L[i])), R[i] = static_cast<float>(s.bq[1][0].process(R[i]));
      break;
    }
    case TFilter: {
      const int ty = static_cast<int>(p[2]);
      const double q = std::pow(10.0, p[1] / 20.0) * 0.7071;
      const double res01 = std::clamp((2.0 - 1.0 / std::max(0.5, q)) / 1.98, 0.0, 0.99);
      for (int c = 0; c < 2; ++c) s.svf[c][0].set(sr, p[0], res01);
      for (int i = 0; i < n; ++i) {
        float* x[2] = {&L[i], &R[i]};
        for (int c = 0; c < 2; ++c) {
          double b, h;
          const double lo = s.svf[c][0].process(*x[c], &b, &h);
          const bool hp = ty == 1 || ty == 3 || ty == 10 || ty == 14 || ty == 16;
          const bool bp = ty == 4 || ty == 6 || ty == 11;
          const bool br = ty == 5 || ty == 7 || ty == 12;
          *x[c] = static_cast<float>(hp ? h : bp ? b : br ? lo + h : lo);
        }
      }
      break;
    }
    case TReverb: {
      static const double kLen[8] = {0.0297, 0.0371, 0.0411, 0.0437, 0.0507, 0.0563, 0.0617, 0.0673};
      static const double kAp[4] = {0.0050, 0.0068, 0.0102, 0.0127};
      const double size = 0.3 + 0.7 * std::clamp(pct(0), 0.0, 1.0), fb = 0.7 + 0.28 * std::clamp(pct(0), 0.0, 1.0);
      const double damp = 0.05 + 0.9 * std::clamp(pct(1), 0.0, 1.0), pre = std::max(0.0, static_cast<double>(p[2])) * sr;
      const double tone = std::exp(-dsp::kTwoPi * (500.0 + 17000.0 * std::clamp(pct(3), 0.0, 1.0)) / sr);
      const double wet = pct(4), dry = pct(5), input = pct(6);
      for (int i = 0; i < n; ++i) {
        s.ring[0].push(static_cast<float>((L[i] + R[i]) * 0.5 * input));
        double x = pre >= 1 ? s.ring[0].tap(pre) : (L[i] + R[i]) * 0.5 * input;
        for (int k = 0; k < 4; ++k) {  // diffusion
          const double dl = kAp[k] * sr;
          const double y = s.ap[static_cast<size_t>(k)].tap(dl);
          const double v = x + y * 0.6;
          s.ap[static_cast<size_t>(k)].push(static_cast<float>(v));
          x = y - v * 0.6;
        }
        double outL = 0, outR = 0;
        for (int k = 0; k < 8; ++k) {
          double y = s.comb[static_cast<size_t>(k)].tap(kLen[k] * size * sr);
          y = s.z[k] = y + (s.z[k] - y) * damp;
          s.comb[static_cast<size_t>(k)].push(static_cast<float>(x + y * fb));
          (k & 1 ? outR : outL) += y;
        }
        outL = s.env[0] = outL * 0.25 + (s.env[0] - outL * 0.25) * tone;
        outR = s.env[1] = outR * 0.25 + (s.env[1] - outR * 0.25) * tone;
        L[i] = static_cast<float>(L[i] * dry + outL * wet);
        R[i] = static_cast<float>(R[i] * dry + outR * wet);
      }
      break;
    }
    case TGate: {
      const double thr = std::pow(10.0, p[0] / 20.0), att = std::exp(-1.0 / std::max(1.0, p[1] * sr)), rel = std::exp(-1.0 / std::max(1.0, p[2] * sr));
      for (int i = 0; i < n; ++i) {
        const double lv = std::max(std::fabs(L[i]), std::fabs(R[i]));
        const bool open = d.gateCc >= 0 ? cc[d.gateCc] >= 0.5f : lv >= thr;
        s.gain = open ? 1.0 + (s.gain - 1.0) * att : s.gain * rel;
        L[i] = static_cast<float>(L[i] * s.gain);
        R[i] = static_cast<float>(R[i] * s.gain);
      }
      break;
    }
    case TLofi: {
      const double bits = 16.0 - 15.0 * std::clamp(pct(0), 0.0, 1.0), q = std::exp2(bits - 1);
      const double hold = 1.0 + 63.0 * std::clamp(pct(1), 0.0, 1.0);
      for (int i = 0; i < n; ++i) {
        if ((s.holdT += 1.0) >= hold) s.holdT -= hold, s.holdL = L[i], s.holdR = R[i];
        L[i] = static_cast<float>(std::round((hold > 1 ? s.holdL : L[i]) * q) / q);
        R[i] = static_cast<float>(std::round((hold > 1 ? s.holdR : R[i]) * q) / q);
      }
      break;
    }
    case TPhaser: {
      const double depth = std::clamp(pct(0), 0.0, 1.0), fb = std::clamp(pct(1), 0.0, 0.95) * 0.9, inc = p[2] / sr, wet = std::clamp(pct(5), 0.0, 1.0);
      const int stages = std::clamp(static_cast<int>(p[3]), 1, 12), wave = static_cast<int>(p[4]);
      const double phase = p[6] / 360.0;
      for (int i = 0; i < n; ++i) {
        float* x[2] = {&L[i], &R[i]};
        for (int c = 0; c < 2; ++c) {
          const double m = 0.5 + 0.5 * lfoWave(wave, s.ph + (c ? 0.25 + phase : phase));
          const double hz = 200.0 * std::exp2(depth * 5.0 * m);
          double y = *x[c] + s.z[c] * fb;
          for (int k = 0; k < stages; ++k) {
            auto& a = s.phaserAp[static_cast<size_t>(c * 12 + k)];
            a.setFreq(sr, hz);
            y = a.process(y);
          }
          s.z[c] = y;
          *x[c] = static_cast<float>(*x[c] * (1.0 - wet) + y * wet);
        }
        s.ph += inc;
        if (s.ph >= 1) s.ph -= 1;
      }
      break;
    }
    case TStatic: {
      const double level = pct(0), tone = std::exp(-dsp::kTwoPi * (300.0 + 15000.0 * std::clamp(pct(1), 0.0, 1.0)) / sr);
      const bool stereo = p[3] > 0.5f;
      const double cycL = pct(4), cycT = std::max(0.01, static_cast<double>(p[5])), rndL = pct(6);
      const double tmin = std::max(0.001, static_cast<double>(p[7])), tmax = std::max(tmin, static_cast<double>(p[8]));
      for (int i = 0; i < n; ++i) {
        double nl = (rndf(s.rng) * 2 - 1) * level, nr = stereo ? (rndf(s.rng) * 2 - 1) * level : nl;
        s.staticCycle += 1.0 / (cycT * sr);
        if (s.staticCycle >= 1) s.staticCycle -= 1, nl += cycL, nr += cycL;
        s.staticNext -= 1.0 / sr;
        if (s.staticNext <= 0) {
          s.staticNext = tmin + (tmax - tmin) * rndf(s.rng);
          nl += rndL * (rndf(s.rng) * 2 - 1), nr += rndL * (rndf(s.rng) * 2 - 1);
        }
        s.z[0] = nl + (s.z[0] - nl) * tone, s.z[1] = nr + (s.z[1] - nr) * tone;
        L[i] = static_cast<float>(L[i] + s.z[0] * 0.25);
        R[i] = static_cast<float>(R[i] + s.z[1] * 0.25);
      }
      break;
    }
    case TStrings: {
      const int count = std::clamp(static_cast<int>(p[0]), 1, 8);
      const double wet = std::clamp(pct(1), 0.0, 1.0);
      for (int i = 0; i < n; ++i) {
        const double x = (L[i] + R[i]) * 0.5;
        double y = 0;
        for (int k = 0; k < count; ++k) {  // strings tuned a fifth apart from C2
          const double hz = 65.406 * std::exp2(7.0 * k / 12.0);
          const double t = s.comb[static_cast<size_t>(k)].tap(sr / hz);
          s.comb[static_cast<size_t>(k)].push(static_cast<float>(x * 0.1 + t * 0.985));
          y += t;
        }
        y /= count;
        L[i] = static_cast<float>(L[i] + y * wet);
        R[i] = static_cast<float>(R[i] + y * wet);
      }
      break;
    }
    case TTdfir: {
      const size_t taps = d.ir.size();
      if (taps == 0 || s.firHist.size() < taps * 2) break;
      const double g = std::pow(10.0, p[0] / 20.0), wet = pct(1), dry = pct(2);
      for (int i = 0; i < n; ++i) {
        s.firHist[s.firW] = L[i];
        s.firHist[taps + s.firW] = R[i];
        double yl = 0, yr = 0;
        size_t idx = s.firW;
        for (size_t k = 0; k < taps; ++k) {
          yl += d.ir[k] * s.firHist[idx];
          yr += d.ir[k] * s.firHist[taps + idx];
          idx = idx == 0 ? taps - 1 : idx - 1;
        }
        s.firW = s.firW + 1 == taps ? 0 : s.firW + 1;
        L[i] = static_cast<float>(L[i] * dry + yl * g * wet);
        R[i] = static_cast<float>(R[i] * dry + yr * g * wet);
      }
      break;
    }
    default: break;
  }
}

}  // namespace gl::packs::samplers::sfzfx
