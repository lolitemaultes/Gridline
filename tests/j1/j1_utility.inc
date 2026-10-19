// J-1 utility pack: morph EQ (EQUO → gl.bankeq), scope (Wave Candy → gl.scope).
#include "../../src/packs/utility/utility.h"
#include "../../src/packs/utility/utility_candy.h"
#include "../../src/packs/utility/utility_rt.h"

namespace {

namespace ut = gl::packs::utility;

PluginState bqState(std::vector<std::pair<int, float>> v = {}) {
  PluginState s = defaultPluginState("gl.bankeq");
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}
void bqSetBank(PluginState& s, int bank, int target, float v) {
  for (int b = 0; b < ut::kBqBands; ++b) s.params[static_cast<size_t>(ut::bqValue(bank, target, b))] = v;
}
// Steady-state gain (dB) of a sine through gl.bankeq (left channel, or right).
double bqGainDb(const PluginState& s, double hz, bool right = false, double sr = jt::kSr) {
  auto fx = createEffect("gl.bankeq");
  size_t n = static_cast<size_t>(sr * 0.8);
  auto L = jt::sine(hz, n, 0.25, sr), R = L, in = L;
  jt::run(*fx, s, L, R, 256, sr);
  return 20 * std::log10(std::max(1e-12, jt::rms(right ? R : L, n / 2) / jt::rms(in, n / 2)));
}
double dbOfLin(double x) { return 20 * std::log10(std::max(x, 1e-12)); }

void testBankEqDesign() {
  expect(jt::near(ut::bqBandHz(0), 20, 1e-9) && jt::near(ut::bqBandHz(15), 640, 1e-9) && jt::near(ut::bqBandHz(30), 20480, 1e-6),
         "morph EQ: band centres 20·2^(i/3)");
  bool peak = true, edges = true;
  for (int b : {3, 9, 15, 21}) {
    double f = ut::bqBandHz(b);
    ut::Coefs c = ut::bqBandCoefs(jt::kSr, f, 0.5);
    peak = peak && jt::near(c.magnitude(f, jt::kSr), 1.0, 1e-6);
    // -3 dB edges half the bandwidth either side of the centre.
    double lo = c.magnitude(f * std::pow(2.0, -0.25), jt::kSr), hi = c.magnitude(f * std::pow(2.0, 0.25), jt::kSr);
    edges = edges && jt::near(dbOfLin(lo), -3.01, 0.25) && jt::near(dbOfLin(hi), -3.01, 0.25);
  }
  expect(peak, "morph EQ: band-pass peak gain is exactly 0 dB at the centre");
  expect(edges, "morph EQ: band-pass -3 dB points at ±bandwidth/2");
  expect(jt::near(ut::bqNorm(0.5), 0.47196, 1e-4) && jt::near(ut::bqNorm(80.0 / 300.0), 0.8849, 1e-3), "morph EQ: output normalisation 10^(-3/20)·100/BW");
  // Gain, pan and output laws.
  expect(jt::near(ut::bqGain(1), 1, 1e-12) && jt::near(ut::bqGain(2), 4, 1e-12) && ut::bqGain(0) == 0 &&
             jt::near(ut::bqValueOfGain(ut::bqGain(0.37)), 0.37, 1e-12),
         "morph EQ: level law (3^v - 1)/2 and its inverse");
  expect(ut::bqPanL(1) == 0 && ut::bqPanR(1) == 1 && ut::bqPanL(-0.5) == 1 && ut::bqPanR(-0.5) == 0.5 && ut::bqPanL(0) == 1,
         "morph EQ: linear balance pan law, 0 dB centre");
  expect(jt::near(ut::bqOutputDbOfRaw(128), 0, 1e-9) && jt::near(ut::bqOutputDbOfRaw(256), 21.5836, 1e-3) && ut::bqOutputDbOfRaw(0) == -60 &&
             jt::near(ut::bqOutputDbOfRaw(173), 7.80, 0.01),
         "morph EQ: output law (11^(v/128) - 1)/10");
  expect(jt::near(ut::bqSmoothSec(170.0 / 1024.0), 0.5, 0.03), "morph EQ: default smoothing ≈ 0.5 s");
}

void testBankEqMotion() {
  PluginState s = bqState();
  s.params[static_cast<size_t>(ut::bqValue(0, ut::BtLevel, 10))] = 0.5f;
  s.params[static_cast<size_t>(ut::bqValue(1, ut::BtLevel, 10))] = 1.5f;
  ut::BankFrame f;
  ut::bankEffective(s.params.data(), 0, 1, 0.5, f);
  bool lerp = jt::near(f.v[ut::BtLevel][10], 1.0, 1e-6);
  ut::bankEffective(s.params.data(), 0, 1, 0.25, f);
  lerp = lerp && jt::near(f.v[ut::BtLevel][10], 0.75, 1e-6) && jt::near(f.gain[ut::BtLevel][10], ut::bqGain(0.75), 1e-6);
  expect(lerp, "morph EQ: morph interpolates neighbouring banks in the v domain");
  ut::bankEffective(s.params.data(), 1.0, 1, 0, f);
  expect(jt::near(f.v[ut::BtLevel][13], 0.5, 1e-6) && jt::near(f.v[ut::BtLevel][10], 1.0, 1e-6), "morph EQ: shift of 1 octave moves the curve by 3 bands");
  ut::bankEffective(s.params.data(), 0, 0, 0, f);
  bool mix = jt::near(f.v[ut::BtLevel][10], 1.0, 1e-6);
  ut::bankEffective(s.params.data(), 0, -1, 0, f);
  mix = mix && jt::near(f.v[ut::BtLevel][10], 1.5, 1e-6);
  expect(mix, "morph EQ: mix 0 flattens, mix -1 inverts around unity");
  // Master curve multiplies the level gain; pan adds.
  s.params[static_cast<size_t>(ut::bqValue(ut::kBqMaster, ut::BtLevel, 10))] = 2.0f;
  s.params[static_cast<size_t>(ut::bqValue(0, ut::BtPan, 4))] = 1.5f;
  s.params[static_cast<size_t>(ut::bqValue(ut::kBqMaster, ut::BtPan, 4))] = 1.8f;
  ut::bankEffective(s.params.data(), 0, 1, 0, f);
  expect(jt::near(f.gain[ut::BtLevel][10], ut::bqGain(0.5) * 4, 1e-6) && jt::near(f.pan[4], 1.0, 1e-6), "morph EQ: master curve applies on top");
  // Band modes: locked keeps the edited bank (no motion), off silences.
  s.params[static_cast<size_t>(ut::bqMode(ut::BtLevel, 10))] = ut::BmLocked;
  s.params[static_cast<size_t>(ut::bqMode(ut::BtLevel, 11))] = ut::BmOff;
  ut::bankEffective(s.params.data(), 0, 1, 1.0, f);
  expect(jt::near(f.v[ut::BtLevel][10], 0.5, 1e-6) && f.gain[ut::BtLevel][11] == 0, "morph EQ: locked and off band modes");
}

void testBankEqAudio() {
  // Flat curve: the reference's small ripple of the parallel band sum.
  const double sr = 44100;
  auto ripple = [&](double bw, double& lo, double& hi) {
    double g[ut::kBqBands];
    for (double& x : g) x = ut::bqNorm(bw);
    lo = 1e9, hi = -1e9;
    for (double f = 60; f <= 12000; f *= 1.01) {
      double d = dbOfLin(ut::bankResponse(f, sr, bw, g, false));
      lo = std::min(lo, d), hi = std::max(hi, d);
    }
  };
  double lo, hi;
  ripple(0.5, lo, hi);
  std::printf("# morph EQ flat ripple BW 0.5 oct: %.2f .. %.2f dB\n", lo, hi);
  bool r = lo > -0.4 && hi < 2.2 && hi > 1.2;
  ripple(80.0 / 300.0, lo, hi);
  std::printf("# morph EQ flat ripple BW 0.27 oct: %.2f .. %.2f dB\n", lo, hi);
  r = r && lo > -1.4 && hi < 2.7;
  ripple(500.0 / 300.0, lo, hi);
  std::printf("# morph EQ flat ripple BW 1.67 oct: %.2f .. %.2f dB\n", lo, hi);
  r = r && lo > -1.6 && hi < 0.9;
  expect(r, "morph EQ: flat-curve ripple within the reference's simulated range");
  // The running effect matches the analytic band sum.
  {
    double g[ut::kBqBands];
    for (double& x : g) x = ut::bqNorm(0.5);
    double want = dbOfLin(ut::bankResponse(1000, jt::kSr, 0.5, g, false)), got = bqGainDb(bqState(), 1000);
    expect(jt::near(got, want, 0.05), "morph EQ: processed sine follows the band-sum response");
  }
  // Level curve at v = 2 (x4) lifts by about 12 dB.
  {
    PluginState s = bqState();
    bqSetBank(s, 0, ut::BtLevel, 2.0f);
    double d = bqGainDb(s, 1000) - bqGainDb(bqState(), 1000);
    expect(jt::near(d, 12.04, 0.1), "morph EQ: level value 2 = +12 dB");
  }
  // Pan curve hard right: left silent, right untouched.
  {
    PluginState s = bqState();
    bqSetBank(s, 0, ut::BtPan, 2.0f);
    expect(bqGainDb(s, 1000) < -80 && jt::near(bqGainDb(s, 1000, true), bqGainDb(bqState(), 1000, true), 0.05), "morph EQ: pan curve balances the band");
  }
  // Add mode with a flat curve is transparent.
  {
    PluginState s = bqState({{ut::BqAddMode, 1.f}});
    auto fx = createEffect("gl.bankeq");
    std::vector<float> L(24000), R(24000);
    for (size_t i = 0; i < L.size(); ++i) L[i] = std::sin(0.013 * i) * 0.3f + std::sin(0.31 * i) * 0.2f, R[i] = std::cos(0.07 * i) * 0.4f;
    auto L0 = L, R0 = R;
    jt::run(*fx, s, L, R, 256);
    double err = 0;
    for (size_t i = 0; i < L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(L[i] - L0[i]) + std::fabs(R[i] - R0[i])));
    expect(err < 1e-5, "morph EQ: add mode with a flat curve is transparent");
    bqSetBank(s, 0, ut::BtLevel, 2.0f);
    double d = bqGainDb(s, 1000);
    expect(d > 3, "morph EQ: add mode boosts with the curve");
  }
  // All bands off: silence; output law.
  {
    PluginState s = bqState();
    for (int b = 0; b < ut::kBqBands; ++b) s.params[static_cast<size_t>(ut::bqMode(ut::BtLevel, b))] = ut::BmOff;
    expect(bqGainDb(s, 1000) < -100, "morph EQ: bands switched off are muted");
    PluginState o = bqState({{ut::BqOutput, 6.f}});
    expect(jt::near(bqGainDb(o, 1000) - bqGainDb(bqState(), 1000), 6.0, 0.01), "morph EQ: output volume");
  }
  // Oversampling: latency reported, response kept.
  {
    PluginState s = bqState({{ut::BqOversample, 1.f}});
    auto fx = createEffect("gl.bankeq");
    std::vector<float> L(64, 0.f), R(64, 0.f);
    jt::run(*fx, s, L, R, 64);
    expect(fx->latencyFor(s, jt::kSr) == 31, "morph EQ: 2x oversampling reports its latency");
    expect(jt::near(bqGainDb(s, 1000), bqGainDb(bqState(), 1000), 0.3) && jt::near(bqGainDb(s, 15000), bqGainDb(bqState(), 15000), 1.5),
           "morph EQ: oversampled response matches");
  }
  // Send output: written to SideIo::multiL/R[0] when routed.
  {
    const PluginInfo* info = findPlugin("gl.bankeq");
    expect(info && info->sendParams.size() == 1 && info->sendParams[0] == ut::BqSendTo, "morph EQ: send target declared as a send parameter");
    PluginState s = bqState({{ut::BqSendTo, 2.f}, {ut::BqSendOnly, 1.f}});
    auto fx = createEffect("gl.bankeq");
    const size_t n = 24000;
    auto L = jt::sine(1000, n, 0.25), R = L, in = L;
    std::vector<float> sl(n, 0.f), sr2(n, 0.f);
    ProcessContext ctx;
    ctx.sampleRate = jt::kSr;
    for (size_t i = 0; i < n; i += 256) {
      SideIo io;
      io.multiL[0] = sl.data() + i;
      io.multiR[0] = sr2.data() + i;
      fx->setSideIo(io);
      int m = static_cast<int>(std::min<size_t>(256, n - i));
      fx->process(s, ctx, L.data() + i, R.data() + i, m);
    }
    double sendDb = dbOfLin(jt::rms(sl, n / 2) / jt::rms(in, n / 2));
    expect(L == in, "morph EQ: send only keeps the main output untouched");
    expect(jt::near(sendDb, bqGainDb(bqState(), 1000), 0.05) && jt::rms(sr2, n / 2) > 0.1, "morph EQ: send curve feeds the destination insert");
    // Unrouted: nothing written, no crash.
    auto fx2 = createEffect("gl.bankeq");
    auto L2 = in, R2 = in;
    jt::run(*fx2, s, L2, R2, 256);
    expect(L2 == in, "morph EQ: unrouted send is ignored");
  }
  // Parameters round trip (bank values / modes live in the parameter vector).
  {
    PluginState s = bqState();
    s.params[static_cast<size_t>(ut::bqValue(7, ut::BtSend, 30))] = 0.25f;
    s.params[static_cast<size_t>(ut::bqMode(ut::BtPan, 3))] = ut::BmLocked;
    PluginState t = s;
    t.params.resize(static_cast<size_t>(ut::kBqModeBase));  // older / shorter vector
    normalizePluginState(t);
    PluginState u = s;
    normalizePluginState(u);
    expect(u.params == s.params && t.params.size() == static_cast<size_t>(ut::kBqCount) && t.params[static_cast<size_t>(ut::bqValue(7, ut::BtSend, 30))] == 0.25f,
           "morph EQ: state round trip and padding");
  }
}

void testBankEqFl() {
  if (!jt::haveFl()) return;
  int total = 0, exact = 0;
  for (const char* f : {"Default", "Random bank", "Resonant lowpass", "Stereo separation", "Vocals enhancer", "Vowels"}) {
    flp::PresetResult r;
    if (!jt::flPreset(std::string("EQUO/") + f + ".fst", r)) continue;
    ++total;
    exact += r.exact && r.state.type == "gl.bankeq";
  }
  std::printf("# EQUO presets exact: %d/%d\n", exact, total);
  expect(total == 6 && exact == 6, "EQUO presets decode exactly to the morph EQ");
  flp::PresetResult r;
  if (jt::flPreset("EQUO/Default.fst", r)) {
    bool flat = true;
    for (int i = 0; i < ut::kBqValues; ++i) flat = flat && r.state.params[static_cast<size_t>(ut::kBqValueBase + i)] == 1.0f;
    expect(flat && jt::near(r.state.params[ut::BqBandwidth], 0.5, 1e-6) && r.state.params[ut::BqMix] == 1.0f &&
               jt::near(r.state.params[ut::BqSmooth], 170.0 / 1024.0, 1e-6) && r.state.params[ut::BqSendTo] == -1.0f,
           "EQUO default: flat banks and default controls");
  }
  if (jt::flPreset("EQUO/Stereo separation.fst", r)) {
    bool pan = false;
    for (int b = 0; b < ut::kBqBands; ++b) pan = pan || r.state.params[static_cast<size_t>(ut::bqValue(0, ut::BtPan, b))] != 1.0f;
    expect(pan, "EQUO stereo preset: pan curves imported");
  }
  if (jt::flPreset("EQUO/Resonant lowpass.fst", r)) {
    expect(jt::near(r.state.params[ut::BqMorph], 5.0, 1e-6) && r.state.params[ut::BqOversample] == 1.0f && r.state.params[ut::BqSmooth] == 0.0f,
           "EQUO resonant preset: morph, oversampling, smoothing");
    bool lowCut = false;  // banks hold zero-gain bands (no truncation at the first silent band)
    for (int b = 0; b < ut::kBqBands; ++b) lowCut = lowCut || r.state.params[static_cast<size_t>(ut::bqValue(5, ut::BtLevel, b))] < 0.05f;
    expect(lowCut, "EQUO resonant preset: silent bands kept");
  }
}

// ------------------------------------------------------------------- scope
void testScopeReference() {
  // Spectrum bands → FFT size.
  expect(ut::scopeBands(0) == 32 && ut::scopeBands(5) == 1024 && ut::scopeBands(8) == 8192 && ut::scopeFftSize(0) == 1024 &&
             ut::scopeFftSize(5) == 2048 && ut::scopeFftSize(8) == 16384,
         "scope: spectrum bands 32..8192 → FFT of 2 × bands (min 1024)");
  // Old projects keep the legacy display; new instances use the reference one.
  {
    PluginState s;
    s.type = "gl.scope";
    s.params.assign(ut::kScLegacyCount, 0.f);
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(ut::ScCount) && s.params[ut::ScEngine] == 0 && defaultPluginState("gl.scope").params[ut::ScEngine] == 1,
           "scope: pre-J-1 states normalise to the legacy engine");
  }
  // Reassigned spectrum: sharp peak frequency.
  {
    ut::CandySpectrum sp;
    sp.init(ut::scopeFftSize(5));
    auto x = jt::sine(1003.7, 2048, 0.5);
    sp.analyse(x.data(), jt::kSr, true);
    double sharp = sp.peakHz();
    sp.analyse(x.data(), jt::kSr, false);
    double plain = sp.peakHz();
    std::printf("# scope spectrum peak: plain %.2f Hz, sharpened %.2f Hz\n", plain, sharp);
    expect(jt::near(sharp, 1003.7, 0.5) && std::fabs(plain - 1003.7) > 1.0, "scope: peak sharpening reassigns to the true frequency");
    std::vector<float> cols;
    sp.columns(300, 1.0, false, 0, jt::kSr, cols);
    float mx = *std::max_element(cols.begin(), cols.end());
    expect(jt::near(mx, dbOfLin(0.5), 1.6), "scope: spectrum columns read the sine level");
  }
  // Meter ballistics.
  auto feedSine = [](ut::CandyMeter& m, double hz, double amp, double sec) {
    auto x = jt::sine(hz, static_cast<size_t>(sec * jt::kSr), amp);
    m.feed(x.data(), x.data(), x.size());
  };
  auto feedSilence = [](ut::CandyMeter& m, double sec) {
    std::vector<float> z(static_cast<size_t>(sec * jt::kSr), 0.f);
    m.feed(z.data(), z.data(), z.size());
  };
  {
    ut::CandyMeter m;
    ut::CandyMeter::Config c;
    c.sr = jt::kSr;
    c.curve = 0;  // linear bar
    c.fallMs = 1000;
    c.holdMs = 500;
    m.configure(c);
    expect(m.blockFrames() == 960, "scope meter: 20 ms analysis blocks");
    feedSine(m, 1000, 1.0, 0.2);
    bool peak = jt::near(m.level(0), 1.0, 0.01) && jt::near(m.pos(0), 0.5, 0.01);
    feedSilence(m, 0.25);
    peak = peak && jt::near(m.pos(0), 0.25, 0.03) && jt::near(m.hold(0), 0.5, 1e-9);  // falls 1/4 scale in 250 ms, hold kept
    feedSilence(m, 0.5);
    peak = peak && m.pos(0) == 0 && m.hold(0) < 0.5 && m.hold(0) > 0.2;  // hold released after 500 ms, then falls
    expect(peak, "scope meter: peak level, linear fall time, hold time");
    c.holdInfinite = true;
    m.configure(c);
    feedSine(m, 1000, 0.5, 0.1);
    feedSilence(m, 3.0);
    expect(jt::near(m.hold(0), 0.25, 0.01) && m.pos(0) == 0, "scope meter: infinite hold");
    c.holdInfinite = false;
    c.holdMs = 0;
    m.configure(c);
    feedSine(m, 1000, 0.5, 0.1);
    feedSilence(m, 0.1);
    expect(m.hold(0) == m.pos(0), "scope meter: no hold");
  }
  auto levelOf = [&](int type, double hz, double sec) {
    ut::CandyMeter m;
    ut::CandyMeter::Config c;
    c.sr = jt::kSr;
    c.type = type;
    m.configure(c);
    feedSine(m, hz, 0.5, sec);
    return dbOfLin(m.level(0) / 0.5);
  };
  double rmsDb = levelOf(ut::SmtRms, 1000, 1.5), vuDb = levelOf(ut::SmtVu, 1000, 1.5), ppmDb = levelOf(ut::SmtPpm, 1000, 0.5);
  double leq1k = levelOf(ut::SmtLeqA, 1000, 6), leq100 = levelOf(ut::SmtLeqA, 100, 6);
  std::printf("# scope meters (re sine peak): RMS %.2f, VU %.2f, PPM %.2f, LeqA 1k %.2f, LeqA 100 %.2f dB\n", rmsDb, vuDb, ppmDb, leq1k, leq100);
  expect(jt::near(rmsDb, -3.01, 0.1) && jt::near(vuDb, -3.01, 0.15) && jt::near(ppmDb, 0, 0.3), "scope meter: RMS / VU / PPM detectors");
  expect(jt::near(leq1k, -3.01, 0.2) && jt::near(leq100 - leq1k, -19.1, 0.5), "scope meter: A-weighted Leq");
  {
    ut::CandyMeter m;
    ut::CandyMeter::Config c;
    c.sr = jt::kSr;
    c.type = ut::SmtLoudness;
    m.configure(c);
    feedSine(m, 1000, 0.5, 1.0);
    expect(m.level(0) > 0.2 && m.level(0) < 0.8, "scope meter: BS.1770 loudness");
  }
  expect(jt::near(ut::candyMeterAmp(ut::candyMeterPos(0.3, 0.5), 0.5), 0.3, 1e-9) && ut::candyMeterPos(2.0, 0.7) == 1.0,
         "scope meter: scale curve and its inverse");
}

void testWaveCandyFl() {
  if (!jt::haveFl()) return;
  int total = 0, exact = 0;
  std::error_code ec;
  fs::path dir = fs::path(jt::kFlFx) / "Wave Candy";
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->path().extension() != ".fst") continue;
    flp::PresetResult r;
    if (!flp::importFlPreset(it->path().string(), r)) continue;
    ++total;
    exact += r.exact && r.state.type == "gl.scope" && r.state.params[ut::ScEngine] == 1;
  }
  std::printf("# Wave Candy presets exact: %d/%d\n", exact, total);
  expect(total == 38 && exact == 38, "Wave Candy presets decode exactly to the scope");
  flp::PresetResult r;
  auto p = [&](int k) { return r.state.params[static_cast<size_t>(k)]; };
  if (jt::flPreset("Wave Candy/Function/Oscilloscope.fst", r))
    expect(p(ut::ScMode) == ut::SvOsc && jt::near(p(ut::ScWindow), 0.02, 1e-6) && p(ut::ScWaveRefresh) == 10 && p(ut::ScTaper) == 1 &&
               r.state.data.find("title=Oscilloscope") != std::string::npos,
           "Wave Candy oscilloscope (v3): view, span, refresh, caption");
  if (jt::flPreset("Wave Candy/Function/Spectrum.fst", r))
    expect(p(ut::ScMode) == ut::SvSpectrum && p(ut::ScSpecBands) == 5 && p(ut::ScRange) == 96 && p(ut::ScPink) == 1 && p(ut::ScSharpen) == 1,
           "Wave Candy spectrum (v3)");
  if (jt::flPreset("Wave Candy/Function/Peak meter.fst", r))
    expect(p(ut::ScMode) == ut::SvMeters && p(ut::ScMeterType) == ut::SmtPeak && jt::near(p(ut::ScHold), 2.5, 1e-6) && p(ut::ScFallTime) == 750,
           "Wave Candy peak meter (v3)");
  if (jt::flPreset("Wave Candy/Function/Vectorscope.fst", r))
    expect(p(ut::ScMode) == ut::SvVector && p(ut::ScAutoView) == 1 && p(ut::ScVecStyle) == ut::SvsTube && jt::near(p(ut::ScVecWidth), 3.5, 1e-6),
           "Wave Candy vectorscope (v6)");
  if (jt::flPreset("Wave Candy/Theme/Aurora.fst", r))
    expect(p(ut::ScMode) == ut::SvOsc && p(ut::ScSplit) == 1 && jt::near(p(ut::ScSplitGap), 77.2, 1e-4) && p(ut::ScVecStyle) == ut::SvsThin &&
               jt::near(p(ut::ScVecWidth), 1.18, 1e-5) && p(ut::ScVecAdd) == 0 && r.state.data.empty(),
           "Wave Candy v8 theme: split, vector settings");
  if (jt::flPreset("Wave Candy/Theme/Mini meter.fst", r))
    expect(p(ut::ScFade) == 1 && p(ut::ScFadeDelay) == 3024 && jt::near(p(ut::ScHold), 1.0, 1e-6) && p(ut::ScFallTime) == 398,
           "Wave Candy v1 theme: fade on silence, hold, fall");
  if (jt::flPreset("Wave Candy/Theme/Glacier.fst", r))
    expect(p(ut::ScMeterType) == ut::SmtLoudness && p(ut::ScSplit) == 1, "Wave Candy v6 theme: loudness meter type");
  if (jt::flPreset("Wave Candy/Theme/Pitch monitoring.fst", r))
    expect(p(ut::ScSpecBands) == 8 && p(ut::ScFreqCurve) == 1.0f && p(ut::ScRange) == 70 && jt::near(p(ut::ScGain), 12, 1e-6) &&
               jt::near(p(ut::ScSpecRefresh), 19.8, 1e-5) && p(ut::ScFade) == 1 && p(ut::ScFadeDelay) == 1000,
           "Wave Candy v7 theme: spectrum settings, gain");
}

void testJ1Utility() {
  testBankEqDesign();
  testBankEqMotion();
  testBankEqAudio();
  testBankEqFl();
  testScopeReference();
  testWaveCandyFl();
}

}  // namespace
