// J-1 space pack: hall (LuxeVerb → gl.hall reference model) and convolver
// (Fruity Convolver → gl.convolver: level laws, envelopes, EQ curve, long IRs).
#include <array>
#include <fstream>
#include <iterator>
#include "../../src/core/WavWriter.h"
#include "../../src/core/flp/FlpFx2.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/spacefx/spacefx.h"

namespace {

namespace sx = gl::packs::spacefx;

struct SpBlob {
  std::vector<uint8_t> b;
  SpBlob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  SpBlob& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return i32(static_cast<int32_t>(u));
  }
  SpBlob& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    i32(static_cast<int32_t>(u & 0xFFFFFFFFu));
    return i32(static_cast<int32_t>(u >> 32));
  }
  SpBlob& u8(uint8_t v) {
    b.push_back(v);
    return *this;
  }
  SpBlob& bytes(const std::vector<uint8_t>& v) {
    b.insert(b.end(), v.begin(), v.end());
    return *this;
  }
  SpBlob& str(const std::string& s) {
    b.push_back(static_cast<uint8_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
    return *this;
  }
};

struct SpEnv {
  flp::File file;
  Song song;
  flp::Report report;
  flp::Context ctx{file, song, report};
};

bool spDecode(SpEnv& e, const std::string& name, const std::vector<uint8_t>& st, flp::PluginMapping& m) {
  return flp::fx2::decodeEmbedded(e.ctx, name, st.data(), st.size(), m);
}
float spP(const PluginState& s, int k) { return s.params[static_cast<size_t>(k)]; }
void spSet(PluginState& s, int k, float v) { s.params[static_cast<size_t>(k)] = v; }
double spShape(double v, double rmin, double rmax, double lo, double hi, double c) {
  double n = std::clamp((v - rmin) / (rmax - rmin), 0.0, 1.0);
  return lo + (hi - lo) * std::pow(n, 1.0 / c);
}
// Magnitude of one frequency in a signal (Goertzel, normalised).
double spTone(const std::vector<float>& x, size_t from, size_t to, double hz, double sr = jt::kSr) {
  double w = 2 * 3.14159265358979 * hz / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = from; i < to && i < x.size(); ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
  return std::sqrt(re * re + im * im) / static_cast<double>(std::max<size_t>(1, to - from));
}
std::vector<float> spNoise(size_t n, uint32_t seed, double amp = 0.3) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    v[i] = static_cast<float>(amp * ((seed >> 8) / 8388608.0 - 1.0));
  }
  return v;
}
// Runs an effect created from the pack (fresh instance) over L/R.
void spRun(const PluginState& s, std::vector<float>& L, std::vector<float>& R, int block = 256) {
  auto fx = createEffect(s.type);
  jt::run(*fx, s, L, R, block);
}
PluginState spHallRef() {
  PluginState s = defaultPluginState("gl.hall");
  spSet(s, sx::HvDry, -60);
  spSet(s, sx::HvWet, 0);
  spSet(s, sx::HvModDepth, 0);
  return s;
}
double spPeak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  double p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(static_cast<double>(v[i])));
  return p;
}

// ---------------------------------------------------------------- hall
void testSpHallLaws() {
  // Prime-table line lengths (ms at 44.1 kHz) for S = 1, 20, 100.
  int l1[16], l20[16], l100[16];
  sx::hallLineLengths(0, l1);
  sx::hallLineLengths(19.0 / 99.0, l20);
  sx::hallLineLengths(1, l100);
  expect(jt::near(l1[0] / 44.1, 13.08, 0.01) && jt::near(l1[15] / 44.1, 35.9, 0.01), "hall: S 1 lines 13.08 .. 35.90 ms");
  expect(jt::near(l20[0] / 44.1, 16.76, 0.01) && jt::near(l20[15] / 44.1, 105.15, 0.01), "hall: S 20 lines 16.76 .. 105.15 ms");
  expect(jt::near(l100[0] / 44.1, 34.26, 0.01) && jt::near(l100[15] / 44.1, 440.39, 0.01), "hall: S 100 lines 34.26 .. 440.39 ms");
  // Synced tick wheels: 16·(v−1)/767 sixteenths.
  expect(jt::near(sx::hallTicksSeconds(768, 120), 2.0, 1e-9) && jt::near(sx::hallTicksSeconds(1, 120), 0.0, 1e-12) &&
             jt::near(sx::hallTicksSeconds(192, 120), 16.0 * 191.0 / 767.0 * 0.125, 1e-9),
         "hall: synced ticks law");
  // Decoder laws on a v9 state.
  SpEnv e;
  SpBlob s;
  s.i32(9).i32(0).u8(1).u8(1).u8(1).u8(0).u8(1);
  int32_t t[49] = {300, 5000, 900, 1, 9000, 2500, 384, 600, 30, 70, 250, 40, 800, 2, 0, 1, -50, 2500, 96, 650, 3000, 200,
                   400, -300, -2500, 120, 700, 80, 2, 0, 1000, 505, 1100, 200, 2000, 600, 1000, 150, 1, 1, 300, 12, -7, 500, 250, 1000, 192, 400, 384};
  for (int32_t v : t) s.i32(v);
  s.u8(0xB3).u8(0);
  flp::PluginMapping m;
  bool ok = spDecode(e, "LuxeVerb", s.b, m);
  const PluginState& h = m.state;
  expect(ok && m.glType == "gl.hall" && spP(h, sx::HvModel) == 1, "hall: LuxeVerb v9 decodes to the reference model");
  expect(jt::near(spP(h, sx::HvInGain), spShape(300, -600, 1500, -60, 15, 2), 1e-4) &&
             jt::near(spP(h, sx::HvInHighCut), spShape(5000, 40, 20000, 40, 20000, 1.5), 1e-2) &&
             jt::near(spP(h, sx::HvInLowCut), spShape(900, 20, 5000, 20, 5000, 0.5), 1e-3) &&
             jt::near(spP(h, sx::HvDecay), spShape(9000, 250, 20000, 0.25, 20, 0.4), 1e-5) &&
             jt::near(spP(h, sx::HvPreDelay), spShape(2500, 0, 5000, 0, 500, 0.5), 1e-4),
         "hall: level / cut / decay / pre-delay shape laws");
  expect(jt::near(spP(h, sx::HvBright), 0.6, 1e-6) && jt::near(spP(h, sx::HvCharacter), 0.3, 1e-6) &&
             jt::near(spP(h, sx::HvSize), 69.0 / 99.0, 1e-6) && jt::near(spP(h, sx::HvModFreq), 0.25, 1e-6) &&
             jt::near(spP(h, sx::HvModDepth), 0.4, 1e-6) && spP(h, sx::HvFreeze) == sx::HfSustain && spP(h, sx::HvHQ) == 0,
         "hall: brightness / character / size / modulation / sustain");
  expect(spP(h, sx::HvFbOn) == 1 && jt::near(spP(h, sx::HvFbPitch), -5, 1e-6) &&
             jt::near(spP(h, sx::HvFbDelay), 1 + 499 * 2490.0 / 4990.0, 1e-3) && spP(h, sx::HvFbTicks) == 96 &&
             jt::near(spP(h, sx::HvFbGain), 0.65, 1e-6) && jt::near(spP(h, sx::HvFbMix), 0.4, 1e-6),
         "hall: feedback pitch / delay / gain / mix");
  expect(jt::near(spP(h, sx::HvEnvScale), -0.3, 1e-6) && jt::near(spP(h, sx::HvEnvThresh), -25, 1e-6) &&
             jt::near(spP(h, sx::HvEnvAttack), 120, 1e-6) && jt::near(spP(h, sx::HvEnvRelease), 700, 1e-6) &&
             jt::near(spP(h, sx::HvEnvOffset), 0.3, 1e-6) && spP(h, sx::HvEnvRoute) == sx::HeDecay &&
             jt::near(spP(h, sx::HvEnvLowCut), spShape(1000, 20, 2000, 20, 2000, 0.5), 1e-3) &&
             jt::near(spP(h, sx::HvEnvSmooth), 1 + 99 * 495.0 / 990.0, 1e-4),
         "hall: envelope scale / threshold / times / offset / route / low cut / smoothing");
  expect(jt::near(spP(h, sx::HvEqFreq), spShape(2000, 40, 10000, 40, 10000, 0.5), 1e-3) && jt::near(spP(h, sx::HvEqGain), 6, 1e-6) &&
             jt::near(spP(h, sx::HvEqQ), 0.25 + 4.75 * 975.0 / 4975.0, 1e-5) && jt::near(spP(h, sx::HvWidth), 1.5, 1e-6) &&
             spP(h, sx::HvLimiter) == 1,
         "hall: output peak EQ / width / limiter");
  expect(spP(h, sx::HvFbMode) == 1 && jt::near(spP(h, sx::HvFfDirect), 0.3, 1e-6) && spP(h, sx::HvFfPitch1) == 12 &&
             spP(h, sx::HvFfPitch2) == -7 && jt::near(spP(h, sx::HvFfAmp1), 0.5, 1e-6) && jt::near(spP(h, sx::HvFfAmp2), 0.25, 1e-6) &&
             jt::near(spP(h, sx::HvFfDelay1), 1 + 499 * 990.0 / 4990.0, 1e-3) && jt::near(spP(h, sx::HvFfDelay2), 1 + 499 * 0.4, 1e-3) &&
             spP(h, sx::HvFfTicks2) == 384 && spP(h, sx::HvFfSync1) == 1 && spP(h, sx::HvFfSync2) == 0,
         "hall: feed-forward voices (pitch, level, delay, sync bytes)");
  expect(spP(h, sx::HvPreSync) == 1 && spP(h, sx::HvFbSync) == 1 && spP(h, sx::HvPreTicks) == 384 && m.exact, "hall: header sync bytes, exact");
  // v7 state: limiter loads on, feed-forward defaults; J-45: the sidechain source is the
  // hosting track's input route 1, resolved by the engine (exact).
  SpBlob s7;
  s7.i32(7).i32(1).u8(0).u8(0).u8(1).u8(0).u8(0);
  for (int k = 0; k < 38; ++k) s7.i32(k == 29 ? 1 : (k == 3 ? 1 : 0));
  flp::PluginMapping m7;
  expect(spDecode(e, "LuxeVerb", s7.b, m7) && spP(m7.state, sx::HvLimiter) == 1 && spP(m7.state, sx::HvFfPitch1) == 24 &&
             jt::near(spP(m7.state, sx::HvFfAmp1), 1.0, 1e-6) && spP(m7.state, sx::HvScIn) == 1 && m7.exact,
         "hall: v7 fix-ups (limiter on, feed-forward defaults), sidechain source = input route 1");
}

void testSpHallModels() {
  // Old states (saved before the model param) normalise to the legacy engine.
  PluginState old = defaultPluginState("gl.hall");
  old.params.resize(sx::HvLimiter + 1);
  normalizePluginState(old);
  expect(spP(old, sx::HvModel) == 0, "hall: pre-J-1 states keep the legacy model");
  expect(spP(defaultPluginState("gl.hall"), sx::HvModel) == 1, "hall: new instances use the reference model");
  // The legacy engine ignores every appended parameter.
  PluginState a = old, b = old;
  for (int k = sx::HvBright; k < sx::HvCount; ++k) spSet(b, k, static_cast<float>(findPlugin("gl.hall")->params[static_cast<size_t>(k)].max));
  auto in = spNoise(24000, 5);
  auto la = in, ra = in, lb = in, rb = in;
  spRun(a, la, ra);
  spRun(b, lb, rb);
  expect(la == lb && ra == rb, "hall: legacy output unaffected by reference-only parameters");
  // Legacy vs reference differ (different engines).
  PluginState rf = old;
  spSet(rf, sx::HvModel, 1);
  auto lr = in, rr = in;
  spRun(rf, lr, rr);
  expect(lr != la, "hall: reference model is a different engine");
}

void testSpHallRef() {
  // Decay: ~60 dB per RT60 at low frequencies (brightness 1 → flat absorption).
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvDecay, 1.0f);
    spSet(s, sx::HvBright, 1.0f);
    std::vector<float> L(static_cast<size_t>(jt::kSr * 2.5)), R(L.size());
    L[0] = R[0] = 1;
    spRun(s, L, R, 512);
    double e1 = jt::rms(std::vector<float>(L.begin() + 12000, L.begin() + 16800));
    double e2 = jt::rms(std::vector<float>(L.begin() + 60000, L.begin() + 64800));
    double drop = 20 * std::log10(e1 / std::max(1e-12, e2));
    char nm[96];
    std::snprintf(nm, sizeof nm, "hall ref: tail falls ~60 dB per RT60 (%.1f dB over 1 s, RT 1 s)", drop);
    expect(drop > 45 && drop < 75 && jt::finite(L), nm);
    double rt1k = sx::hallRt60(s, 1000);
    PluginState d = s;
    spSet(d, sx::HvBright, 0.3f);
    expect(jt::near(rt1k, 1.0, 0.02) && sx::hallRt60(d, 15000) < 0.45 && sx::hallRt60(d, 50) > 0.95, "hall ref: brightness sets the high-band decay");
  }
  // Silence in → silence out.
  {
    std::vector<float> L(20000, 0.f), R(20000, 0.f);
    spRun(spHallRef(), L, R);
    expect(spPeak(L) == 0 && spPeak(R) == 0, "hall ref: silence in, silence out");
  }
  // Freeze holds the tail and ignores new input; sustain keeps taking input.
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvDecay, 1.0f);
    PluginState fz = s, su = s;
    spSet(fz, sx::HvFreeze, sx::HfFreeze);
    spSet(su, sx::HvFreeze, sx::HfSustain);
    auto run2 = [&](const PluginState& second, bool loud) {
      auto fx = createEffect("gl.hall");
      ProcessContext c;
      c.sampleRate = jt::kSr;
      auto L = spNoise(24000, 7), R = spNoise(24000, 8);
      fx->process(s, c, L.data(), R.data(), 24000);
      std::vector<float> L2 = loud ? spNoise(96000, 9, 0.5) : std::vector<float>(96000, 0.f);
      std::fill(L2.begin(), L2.begin() + 9600, 0.f);  // new input starts once the switch has settled
      std::vector<float> R2 = L2;
      for (size_t p = 0; p < L2.size(); p += 500) fx->process(second, c, L2.data() + p, R2.data() + p, 500);
      return L2;
    };
    auto fq = run2(fz, false), fl = run2(fz, true), sq = run2(su, false), sl = run2(su, true);
    double f1 = jt::rms(std::vector<float>(fq.begin() + 24000, fq.begin() + 36000)), f2 = jt::rms(std::vector<float>(fq.begin() + 84000, fq.end()));
    char nm[96];
    std::snprintf(nm, sizeof nm, "hall ref: freeze sustains the tail (%.1f dB over 1.25 s)", 20 * std::log10(f2 / std::max(1e-12, f1)));
    expect(f1 > 1e-4 && f2 > 0.8 * f1, nm);
    double dFz = 0, dSu = 0;
    for (size_t i = 48000; i < fq.size(); ++i) {
      dFz = std::max(dFz, std::fabs(static_cast<double>(fq[i]) - fl[i]));
      dSu = std::max(dSu, std::fabs(static_cast<double>(sq[i]) - sl[i]));
    }
    // (the dry path at -60 dB = 0.001 still passes the new input: <= 0.0005 here)
    expect(dFz < 6e-4 && dSu > 1e-2, "hall ref: freeze ignores new input, sustain keeps feeding the tank");
    expect(jt::finite(sl) && spPeak(sl) < 30, "hall ref: sustain with input stays bounded");
  }
  // Envelope → wet (negative scale ducks the wet under a loud input).
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvDecay, 2.0f);
    PluginState dk = s;
    spSet(dk, sx::HvEnvRoute, sx::HeWet);
    spSet(dk, sx::HvEnvScale, -1.0f);
    spSet(dk, sx::HvEnvThresh, -40);
    auto L1 = spNoise(48000, 3, 0.5), R1 = L1, L2 = L1, R2 = L1;
    spRun(s, L1, R1);
    spRun(dk, L2, R2);
    double r1 = jt::rms(L1, 24000), r2 = jt::rms(L2, 24000);
    expect(r2 < 0.1 * r1, "hall ref: envelope → wet ducks the wet signal");
    // Envelope → decay lengthens the tail while the input is loud.
    PluginState dd = s;
    spSet(dd, sx::HvDecay, 0.5f);
    spSet(s, sx::HvDecay, 0.5f);
    spSet(dd, sx::HvEnvRoute, sx::HeDecay);
    spSet(dd, sx::HvEnvScale, 0.5f);
    spSet(dd, sx::HvEnvThresh, -40);
    spSet(dd, sx::HvEnvRelease, 1000);
    std::vector<float> a = spNoise(48000, 4, 0.5), b = a;
    a.resize(72000, 0.f);
    b.resize(72000, 0.f);
    auto ar = a, br = b;
    spRun(s, a, ar);
    spRun(dd, b, br);
    double ta = jt::rms(std::vector<float>(a.begin() + 50400, a.begin() + 60000)), tb = jt::rms(std::vector<float>(b.begin() + 50400, b.begin() + 60000));
    expect(tb > 3 * ta, "hall ref: envelope → decay lengthens the tail");
  }
  // Shimmer: +12 st feedback puts energy an octave up; feed-forward voices shift the input.
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvDecay, 3.0f);
    spSet(s, sx::HvBright, 1.0f);
    PluginState sh = s;
    spSet(sh, sx::HvFbOn, 1);
    spSet(sh, sx::HvFbPitch, 12);
    spSet(sh, sx::HvFbGain, 0.8f);
    spSet(sh, sx::HvFbDelay, 20);
    spSet(sh, sx::HvFbLowCut, 20);
    spSet(sh, sx::HvFbHighCut, 20000);
    auto L1 = jt::sine(440, 96000, 0.4), R1 = L1;
    auto L2 = L1, R2 = L1;
    spRun(s, L1, R1);
    spRun(sh, L2, R2);
    double o1 = spTone(L1, 48000, 96000, 880), o2 = spTone(L2, 48000, 96000, 880);
    expect(o2 > 4 * o1 && jt::finite(L2), "hall ref: feedback shimmer adds the octave");
    PluginState ff = s;
    spSet(ff, sx::HvFbOn, 1);
    spSet(ff, sx::HvFbMode, 1);
    spSet(ff, sx::HvFfDirect, 0);
    spSet(ff, sx::HvFfAmp1, 1);
    spSet(ff, sx::HvFfPitch1, 7);
    spSet(ff, sx::HvFfAmp2, 0);
    auto L3 = jt::sine(440, 96000, 0.4), R3 = L3;
    spRun(ff, L3, R3);
    double fifth = spTone(L3, 48000, 96000, 440 * std::pow(2.0, 7 / 12.0)), base = spTone(L3, 48000, 96000, 440);
    expect(fifth > 4 * base, "hall ref: feed-forward voice shifts the reverb input (+7 st)");
  }
  // Limiter: wet sum held near the 0.25 ceiling.
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvWidth, 1);
    spSet(s, sx::HvWet, 0);
    PluginState lm = s;
    spSet(lm, sx::HvLimiter, 1);
    auto L1 = spNoise(48000, 11, 0.9), R1 = spNoise(48000, 12, 0.9), L2 = L1, R2 = R1;
    spRun(s, L1, R1);
    spRun(lm, L2, R2);
    double p1 = spPeak(L1, 4800), p2 = spPeak(L2, 4800);
    char nm[96];
    std::snprintf(nm, sizeof nm, "hall ref: limiter holds the wet peak (%.2f → %.2f)", p1, p2);
    expect(p1 > 0.4 && p2 < 0.3, nm);
  }
  // Block-size invariance with everything moving.
  {
    PluginState s = spHallRef();
    spSet(s, sx::HvModDepth, 0.6f);
    spSet(s, sx::HvModFreq, 0.8f);
    spSet(s, sx::HvFbOn, 1);
    spSet(s, sx::HvEnvRoute, sx::HeDecay);
    spSet(s, sx::HvEnvScale, 0.1f);
    spSet(s, sx::HvLimiter, 1);
    spSet(s, sx::HvPreDelay, 33);
    auto L1 = spNoise(30000, 21), R1 = spNoise(30000, 22), L2 = L1, R2 = R1;
    spRun(s, L1, R1, 512);
    spRun(s, L2, R2, 37);
    double d = 0;
    for (size_t i = 0; i < L1.size(); ++i) d = std::max({d, std::fabs(static_cast<double>(L1[i]) - L2[i]), std::fabs(static_cast<double>(R1[i]) - R2[i])});
    expect(d < 1e-6, "hall ref: output independent of the block size");
  }
  // Extremes stay finite.
  {
    const PluginInfo* info = findPlugin("gl.hall");
    PluginState hi = defaultPluginState("gl.hall"), lo = hi;
    for (size_t k = 0; k < info->params.size(); ++k) {
      hi.params[k] = static_cast<float>(info->params[k].max);
      lo.params[k] = static_cast<float>(info->params[k].min);
    }
    spSet(hi, sx::HvModel, 1);
    spSet(lo, sx::HvModel, 1);
    auto L1 = spNoise(48000, 31), R1 = L1, L2 = L1, R2 = L1;
    spRun(hi, L1, R1);
    spRun(lo, L2, R2);
    expect(jt::finite(L1) && jt::finite(R1) && jt::finite(L2) && jt::finite(R2), "hall ref: parameter extremes stay finite");
  }
}

void testSpHallPresets() {
  if (!jt::haveFl()) return;
  std::error_code ec;
  fs::path dir = fs::path(jt::kFlFx) / "LuxeVerb";
  int total = 0, ok = 0, exact = 0;
  for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
    if (f.path().extension() != ".fst") continue;
    ++total;
    flp::PresetResult r;
    if (flp::importFlPreset(f.path().string(), r) && r.kind == flp::PresetResult::Plugin && r.state.type == "gl.hall") {
      ++ok;
      exact += r.exact ? 1 : 0;
    }
  }
  char nm[96];
  std::snprintf(nm, sizeof nm, "hall: FL presets decode (%d/%d, exact %d)", ok, total, exact);
  expect(total > 100 && ok == total && exact == total, nm);
}

// ------------------------------------------------------------ convolver
std::string spTmpDir() {
  fs::path d = fs::temp_directory_path() / "gridline-j1-space";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.string();
}
// Stereo IR file with unit impulses at the given seconds (left), right = half.
std::string spWriteIr(const std::string& name, double seconds, std::vector<double> at) {
  AudioBuffer b(2, static_cast<size_t>(seconds * jt::kSr));
  for (double t : at) {
    size_t i = static_cast<size_t>(t * jt::kSr);
    if (i < b.frames) {
      b.data[i] = 0.5f;
      b.data[b.frames + i] = 0.25f;
    }
  }
  std::string p = spTmpDir() + "/" + name;
  writeWav(p, b, static_cast<int>(jt::kSr), 24);
  return p;
}
PluginState spConv(const std::string& path) {
  PluginState s = defaultPluginState("gl.convolver");
  spSet(s, sx::CvSource, sx::SrcFile);
  spSet(s, sx::CvFadeOut, 0);
  spSet(s, sx::CvLength, 60);
  spSet(s, sx::CvNormalize, 0);
  spSet(s, sx::CvStereo, 0);
  spSet(s, sx::CvDry, 0);
  spSet(s, sx::CvWet, 1);
  s.data = path;
  return s;
}
// Impulse response of a convolver state (fresh instance, prepared like the engine).
void spConvImpulse(const PluginState& s, size_t n, std::vector<float>& L, std::vector<float>& R, int block = 256) {
  auto fx = createEffect("gl.convolver");
  fx->prepareHost(s, jt::kSr);
  ChannelAux aux;
  packs::prepareAux(s, aux);
  L.assign(n, 0.f);
  R.assign(n, 0.f);
  L[0] = R[0] = 1;
  ProcessContext c;
  c.sampleRate = jt::kSr;
  for (size_t i = 0; i < n; i += static_cast<size_t>(block)) {
    int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - i));
    fx->processAux(s, aux, c, L.data() + i, R.data() + i, k);
  }
}

void testSpConvolver() {
  sx::setConvolverRateHint(jt::kSr);
  // FL knob laws.
  expect(jt::near(sx::convDryGain(1), 1, 1e-12) && jt::near(sx::convDryGain(0), 0, 1e-12) && jt::near(sx::convDryGain(0.5), (std::sqrt(11.0) - 1) / 10, 1e-12),
         "convolver: dry law (11^x − 1)/10");
  expect(jt::near(sx::convWetGain(1), 1, 1e-12) && jt::near(sx::convWetGain(0.5), 0.0306386, 1e-6), "convolver: wet law (1001^x − 1)/1000");
  expect(jt::near(sx::convPreDelayMs(1), 500, 1e-9) && jt::near(sx::convPreDelayMs(0.5), 500 * (std::sqrt(7.0) - 1) / 6, 1e-9),
         "convolver: pre-delay law 500·(7^x − 1)/6 ms");
  expect(jt::near(sx::convStretch(-1), 0.5, 1e-12) && jt::near(sx::convStretch(1), 2, 1e-12), "convolver: stretch law 2^x");
  // Data format round trip (path first; old path-only states stay valid).
  {
    sx::ConvData d;
    d.path = "/x/ir.wav";
    d.env = {{0, 0, 0}, {1.5, 0.5, 0.2}, {3, 0.25, -0.4}};
    d.eq = {{0, 0.75, 0}, {1, 0.25, 0}};
    sx::ConvData r = sx::parseConvData(sx::formatConvData(d));
    expect(r.path == d.path && r.env.size() == 3 && jt::near(r.env[1].t, 1.5, 1e-9) && jt::near(r.env[2].k, -0.4, 1e-9) && r.eq.size() == 2 &&
               sx::parseConvData("/old/path.wav").path == "/old/path.wav" && sx::formatConvData(sx::ConvData{"/p", {}, {}, {}, {}}) == "/p",
           "convolver: envelope data round trip, old states unchanged");
    expect(jt::near(sx::envAt(d.env, 0.75), 0.5 * 0.25, 1e-9) && jt::near(sx::convEqDb(d.eq, 20), 9, 1e-9) && jt::near(sx::convEqDb(d.eq, 20000), -9, 1e-9),
           "convolver: envelope interpolation and EQ curve dB");
  }
  // The convolution equals the IR file; impulses beyond the default capacity play once stage C grows.
  {
    std::string p = spWriteIr("long.wav", 16.0, {0.0, 0.5, 15.0});
    PluginState s = spConv(p);
    std::vector<float> L, R;
    size_t n = static_cast<size_t>(15.2 * jt::kSr);
    spConvImpulse(s, n, L, R, 512);
    size_t i15 = static_cast<size_t>(15.0 * jt::kSr), ih = static_cast<size_t>(0.5 * jt::kSr);
    expect(jt::near(L[0], 0.5, 1e-3) && jt::near(L[ih], 0.5, 1e-3) && jt::near(R[ih], 0.25, 1e-3) && jt::near(L[i15], 0.5, 2e-3) &&
               spPeak(L, 1, ih) < 1e-3 && spPeak(L, ih + 1, i15) < 1e-3,
           "convolver: 15 s impulse plays through the long-IR stage");
    std::vector<float> L2, R2;
    spConvImpulse(s, n, L2, R2, 61);
    double d = 0;
    for (size_t i = 0; i < n; ++i) d = std::max(d, std::fabs(static_cast<double>(L[i]) - L2[i]));
    expect(d < 1e-5, "convolver: long-IR output independent of the block size");
  }
  // Pre-delay, stretch, normalize, envelope, EQ curve.
  {
    std::string p = spWriteIr("short.wav", 1.0, {0.0, 0.25});
    PluginState s = spConv(p);
    spSet(s, sx::CvPreDelay, 100);
    std::vector<float> L, R;
    spConvImpulse(s, 30000, L, R);
    size_t d = 4800;
    expect(spPeak(L, 0, d - 2) < 1e-4 && jt::near(L[d], 0.5, 2e-2), "convolver: pre-delay shifts the response");
    PluginState st = spConv(p);
    spSet(st, sx::CvStretch, 2);
    sx::IrView v1 = sx::convolverIr(spConv(p)), v2 = sx::convolverIr(st);
    expect(v1.ir && v2.ir && jt::near(static_cast<double>(v2.ir->frames()) / v1.ir->frames(), 2.0, 0.01), "convolver: stretch 2 doubles the IR");
    PluginState kp = st;
    spSet(kp, sx::CvStretchMode, 1);
    sx::IrView v3 = sx::convolverIr(kp);
    expect(v3.ir && jt::near(static_cast<double>(v3.ir->frames()) / v1.ir->frames(), 2.0, 0.01) && jt::finite(v3.ir->left),
           "convolver: keep-pitch stretch doubles the IR length");
    PluginState nz = spConv(p);
    spSet(nz, sx::CvNormalize, 1);
    sx::IrView vn = sx::convolverIr(nz);
    double e = 0;
    for (size_t i = 0; i < vn.ir->frames(); ++i) e += 0.5 * (vn.ir->left[i] * vn.ir->left[i] + vn.ir->right[i] * vn.ir->right[i]);
    expect(jt::near(e, 1.0, 1e-3), "convolver: normalize gives unit energy");
    PluginState en = spConv(p);
    spSet(en, sx::CvEnvOn, 1);
    en.data = sx::formatConvData(sx::ConvData{p, {{0, 0.5, 0}, {0.1, 0.5, 0}, {0.2, 0.125, 0}}, {}, {}, {}});
    sx::IrView ve = sx::convolverIr(en);
    size_t q = static_cast<size_t>(0.25 * jt::kSr);
    expect(ve.ir && jt::near(ve.ir->left[0], 0.5, 1e-4) && jt::near(ve.ir->left[q], 0.5 * 0.25, 1e-4), "convolver: IR amplitude envelope (0.5 = unity)");
    std::string pn = spTmpDir() + "/noise.wav";
    {
      AudioBuffer b(2, 24000);
      auto nz2 = spNoise(24000, 77, 0.5);
      for (size_t i = 0; i < 24000; ++i) b.data[i] = b.data[24000 + i] = nz2[i] * static_cast<float>(std::exp(-i / 4000.0));
      writeWav(pn, b, static_cast<int>(jt::kSr), 24);
    }
    PluginState eq = spConv(pn);
    eq.data = sx::formatConvData(sx::ConvData{pn, {}, {{0, 0.5, 0}, {0.6, 0.5, 0}, {0.8, 0.0, 0}, {1, 0.0, 0}}, {}, {}});
    PluginState eq0 = eq;
    spSet(eq0, sx::CvEqMix, 0);
    sx::IrView a = sx::convolverIr(eq), b = sx::convolverIr(eq0);
    double ha = spTone(a.ir->left, 0, 24000, 12000), hb = spTone(b.ir->left, 0, 24000, 12000);
    double la = spTone(a.ir->left, 0, 24000, 200), lb = spTone(b.ir->left, 0, 24000, 200);
    expect(ha < 0.3 * hb && jt::near(la, lb, 0.3 * lb), "convolver: EQ curve cuts the highs of the IR; EQ mix 0 bypasses");
  }
  // Decoder: full layout with envelopes, trailing knobs, embedded WAV.
  {
    auto env = [](SpBlob& s, std::vector<std::array<double, 2>> pts) {
      s.i32(6).i32(3).i32(static_cast<int32_t>(pts.size()));
      double prev = 0;
      for (auto& p : pts) {
        s.f64(p[0] - prev).f64(p[1]).f32(0).u8(0).u8(0).u8(0).u8(1);
        prev = p[0];
      }
      s.i32(0).i32(-1).i32(-1).i32(-1).i32(-1);
    };
    std::string riffPath = spWriteIr("embed.wav", 0.2, {0.0});
    std::ifstream f(riffPath, std::ios::binary);
    std::vector<uint8_t> riff((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    SpBlob s;
    s.i32(0x000F0003).i32(800).i32(1000).i32(0).i32(0).u8(0).str("My impulse.wav").i32(static_cast<int32_t>(riff.size())).bytes(riff);
    s.f64(-1).f64(-1).f64(0).f64(0.2).f32(0.02f);
    env(s, {{0, 0.5}, {0.2, 0.5}});
    env(s, {{0, 0.0}, {0.05, 0.5}, {0.2, 0.0}});
    env(s, {{0, 0.5}, {0.2, 0.5}});
    env(s, {{0, 0.5}, {0.2, 0.5}});
    env(s, {{0, 0.5}, {1, 0.5}});
    s.u8(0).u8(0).u8(1).u8(1).i32(128).i32(0).i32(500).i32(0).i32(-1000).i32(700);
    SpEnv e;
    flp::PluginMapping m;
    setenv("GRIDLINE_CACHE_DIR", spTmpDir().c_str(), 1);  // keep the test's IR out of the user cache
    bool ok = spDecode(e, "Fruity Convolver", s.b, m);
    sx::ConvData cd = sx::parseConvData(m.state.data);
    std::error_code ec;
    expect(ok && fs::exists(cd.path, ec) && cd.path.find("flp-ir") != std::string::npos, "convolver: embedded WAV written to the IR cache");
    expect(jt::near(spP(m.state, sx::CvDry), sx::convDryGain(0.8), 1e-6) && jt::near(spP(m.state, sx::CvWet), 1, 1e-6) &&
               jt::near(spP(m.state, sx::CvPreDelay), sx::convPreDelayMs(0.5), 1e-3) && jt::near(spP(m.state, sx::CvStretch), 0.5, 1e-6) &&
               jt::near(spP(m.state, sx::CvEqMix), 0.7, 1e-6) && spP(m.state, sx::CvNormalize) == 1 && spP(m.state, sx::CvLatency) == 128 &&
               spP(m.state, sx::CvFadeOut) == 0 && spP(m.state, sx::CvEnvOn) == 1,
           "convolver: knobs, pre-delay, stretch, EQ mix, normalize, latency");
    expect(cd.env.size() == 3 && jt::near(cd.env[1].t, 0.05, 1e-9) && jt::near(cd.env[1].v, 0.5, 1e-9) && cd.eq.empty() && m.exact,
           "convolver: amplitude envelope imported (J-42 reference build → exact)");
    sx::IrView v = sx::convolverIr(m.state);
    expect(v.ir != nullptr, "convolver: embedded impulse loads");
  }
  // %USERPROFILE% developer paths resolve by file name under FL's impulse folders.
  if (jt::haveFl()) {
    SpBlob s;
    s.i32(14).i32(1000).i32(500).i32(0).i32(0).u8(1).str("%USERPROFILE%\\Dropbox\\IL_Files\\x\\IMP Cabinet Model A.wv");
    SpEnv e;
    e.ctx.factoryData = fs::path(jt::kFlData).parent_path().string();
    flp::PluginMapping m;
    spDecode(e, "Fruity Convolver", s.b, m);
    std::error_code ec;
    std::string path = sx::parseConvData(m.state.data).path;
    expect(fs::exists(path, ec) && path.find("Impulses") != std::string::npos, "convolver: Dropbox paths resolve by file name");
  }
}

void testSpConvPresets() {
  if (!jt::haveFl()) return;
  std::error_code ec;
  fs::path dir = fs::path(jt::kFlFx) / "Fruity Convolver";
  int total = 0, ok = 0, exact = 0, loaded = 0;
  for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
    if (f.path().extension() != ".fst") continue;
    ++total;
    flp::PresetResult r;
    if (flp::importFlPreset(f.path().string(), r) && r.kind == flp::PresetResult::Plugin && r.state.type == "gl.convolver") {
      ++ok;
      exact += r.exact ? 1 : 0;
      if (total % 40 == 1) loaded += sx::convolverIr(r.state).ir ? 1 : 0;
    }
  }
  char nm[96];
  std::snprintf(nm, sizeof nm, "convolver: FL presets decode (%d/%d, exact %d)", ok, total, exact);
  expect(total > 200 && ok == total && exact >= total - 3, nm);
  expect(loaded >= 5, "convolver: sampled preset impulses load (WavPack)");
}

void testJ1Space() {
  testSpHallLaws();
  testSpHallModels();
  testSpHallRef();
  testSpHallPresets();
  testSpConvolver();
  testSpConvPresets();
}

}  // namespace
