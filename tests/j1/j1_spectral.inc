// J-1 spectral pack: vocoder (Vocodex), band splitter (Frequency Splitter).
#include "../../src/core/Document.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/spectral/spectral.h"
#include "../../src/packs/spectral/spectral_blocks.h"
namespace {
namespace vs1 {
namespace sp = gl::packs::spectral;

PluginState st(const char* type) { return defaultPluginState(type); }
void setp(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }

// Gain (dB) of an effect state at one frequency (stereo sine, steady state).
double gainAt(const PluginState& s, double hz, double sr = jt::kSr, int block = 256) {
  auto fx = createEffect(s.type);
  if (!fx) return -999;
  fx->prepareHost(s, sr);
  size_t n = static_cast<size_t>(sr * 0.6);
  std::vector<float> L = jt::sine(hz, n, 0.5, sr), R = L;
  jt::run(*fx, s, L, R, block, sr);
  std::vector<float> ref = jt::sine(hz, n, 0.5, sr);
  return 20.0 * std::log10(std::max(1e-12, jt::rms(L, n / 2) / jt::rms(ref, n / 2)));
}

// Preset files of one FL effect (paths relative to the effect preset folder).
std::vector<std::string> presetNames(const char* plugin) {
  std::vector<std::string> v;
  fs::path root = fs::path(jt::kFlFx) / plugin;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".fst") v.push_back(fs::relative(it->path(), jt::kFlFx).string());
  std::sort(v.begin(), v.end());
  return v;
}

// ------------------------------------------------------------------ splitter
void testSplitterCurves() {
  expect(jt::near(sp::splitLevelDb(-900), -6.0206, 0.01) && jt::near(sp::splitLevelDb(900), 13.009, 0.01) &&
             jt::near(sp::splitLevelDb(1800), 18.0, 0.01) && sp::splitLevelDb(-1800) == sp::kSplitSilentDb,
         "splitter: level curve (linear below the centre, +18 dB top, silence at the bottom)");
  expect(jt::near(sp::splitLevelDb(259), 6.0, 0.02) && jt::near(sp::splitLevelDb(108), 3.0, 0.05) &&
             jt::near(sp::splitLevelDb(-529), -3.0, 0.05),
         "splitter: level curve at the reference presets' values (+6 / +3 / -3 dB)");
  expect(jt::near(sp::splitRefHz(0), 30, 1e-9) && jt::near(sp::splitRefHz(65536), 16000, 1e-6) &&
             jt::near(sp::splitRefHz(19800), 200, 0.1) && jt::near(sp::splitRefHz(48065), 3000, 1),
         "splitter: crossover wheel curve 30 Hz .. 16 kHz");
  expect(sp::splitFftSize(2, 44100) == 2048 && sp::splitFftSize(2, 48000) == 2048 && sp::splitFftSize(2, 96000) == 4096 &&
             sp::splitFftSize(0, 22050) == 256 && sp::splitFftSize(5, 192000) == 65536 && sp::splitFftSize(5, 44100) == 16384,
         "splitter: linear-phase FFT size from quality and sample rate");
  expect(jt::near(sp::splitLinSpeedMs(0), 2, 1e-9) && jt::near(sp::splitLinSpeedMs(65536), 200, 1e-9) &&
             jt::near(sp::splitLinSpeedMs(45820), 50, 0.1),
         "splitter: linear-phase speed curve 2..200 ms (default 50 ms)");
}

void testSplitterIir() {
  // Sums: every reference slope, 3 bands (200 Hz / 3 kHz).
  double worstLr = 0, worst36 = 0;
  const double fr[] = {40, 120, 180, 200, 230, 400, 900, 2500, 3000, 3500, 6000, 12000};
  for (int sl = 0; sl < sp::kSplitSlopes; ++sl) {
    PluginState s = st("gl.splitter");
    setp(s, sp::SpSlopeA, sl);
    setp(s, sp::SpSlopeB, sl);
    for (double f : fr) {
      double g = std::fabs(gainAt(s, f));
      (sl == 4 ? worst36 : worstLr) = std::max(sl == 4 ? worst36 : worstLr, g);
    }
  }
  std::printf("# splitter reference sum deviation: LR/BW %.4f dB, 36 dB %.3f dB\n", worstLr, worst36);
  expect(worstLr < 0.1, "splitter: bands sum flat (+-0.1 dB) for 6/12/18/24/48/72/96 dB");
  expect(worst36 < 0.45, "splitter: 36 dB (BW4 x BW2) sum within its +-0.45 dB ripple");
  // Slopes: the low band alone follows the closed-form response at 2 and 4 x f1.
  double worstSlope = 0;
  for (int sl = 0; sl < sp::kSplitSlopes; ++sl) {
    PluginState s = st("gl.splitter");
    setp(s, sp::SpBands, 0);
    setp(s, sp::SpFreq1, 500);
    setp(s, sp::SpSlopeA, sl);
    setp(s, sp::splitParam(1, sp::SbMute), 1);
    for (double mul : {2.0, 4.0}) {
      double r = std::tan(3.14159265358979 * 500 * mul / jt::kSr) / std::tan(3.14159265358979 * 500 / jt::kSr), lp, hp;
      gl::packs::spectral::blk::slopeMag(sl, r, lp, hp);
      double want = 20 * std::log10(lp), got = gainAt(s, 500 * mul);
      worstSlope = std::max(worstSlope, std::fabs(want - got));
      if (sl == 7 && mul == 4.0) std::printf("# splitter 96 dB low band at 2 oct: %.1f dB (want %.1f)\n", got, want);
    }
  }
  expect(worstSlope < 0.2, "splitter: low-band attenuation follows each slope's response (6..96 dB)");
  // Mutes and levels.
  {
    PluginState s = st("gl.splitter");
    setp(s, sp::splitParam(0, sp::SbMute), 1);
    setp(s, sp::splitParam(1, sp::SbMute), 1);
    double hi = gainAt(s, 10000), lo = gainAt(s, 60);
    setp(s, sp::splitParam2(2, sp::SxMain), sp::splitLevelDb(-900));
    double hi6 = gainAt(s, 10000);
    expect(std::fabs(hi) < 0.1 && lo < -40 && jt::near(hi6, -6.02, 0.1), "splitter: main mutes silence bands, main level -900 = -6 dB");
    setp(s, sp::splitParam2(2, sp::SxMain), sp::kSplitSilentDb);
    expect(gainAt(s, 10000) < -100, "splitter: main level at the bottom = silence");
  }
}

// Band sends: the effect adds band x send level into SideIo::multiL/R.
void testSplitterSends() {
  for (int linear = 0; linear < 2; ++linear) {
    PluginState s = st("gl.splitter");
    setp(s, sp::SpLinear, linear);
    setp(s, sp::splitParam2(2, sp::SxSendTo), 3);
    setp(s, sp::splitParam2(0, sp::SxSendTo), 4);
    setp(s, sp::splitParam2(0, sp::SxSendMute), 1);
    setp(s, sp::splitParam2(2, sp::SxSend), sp::splitLevelDb(-900));
    for (int b = 0; b < 3; ++b) setp(s, sp::splitParam(b, sp::SbMute), 1);  // main mutes leave the sends alone
    auto fx = createEffect("gl.splitter");
    fx->prepareHost(s, jt::kSr);
    ProcessContext ctx;
    ctx.sampleRate = jt::kSr;
    const int B = 256, blocks = 120;
    std::vector<float> in = jt::sine(8000, static_cast<size_t>(B * blocks), 0.5);
    std::vector<float> outMain, send0, send2;
    for (int k = 0; k < blocks; ++k) {
      float L[B], R[B], s0L[B] = {}, s0R[B] = {}, s2L[B] = {}, s2R[B] = {};
      for (int i = 0; i < B; ++i) L[i] = R[i] = in[static_cast<size_t>(k * B + i)];
      SideIo io;
      io.multiL[0] = s0L, io.multiR[0] = s0R, io.multiL[2] = s2L, io.multiR[2] = s2R;
      fx->setSideIo(io);
      fx->process(s, ctx, L, R, B);
      outMain.insert(outMain.end(), L, L + B);
      send0.insert(send0.end(), s0L, s0L + B);
      send2.insert(send2.end(), s2L, s2L + B);
    }
    size_t from = outMain.size() / 2;
    double ref = jt::rms(in, from), g2 = jt::rms(send2, from) / ref;
    std::printf("# splitter send (%s): band 3 send %.4f, band 1 send %.6f, main %.6f\n", linear ? "linear" : "iir", g2,
                jt::rms(send0, from), jt::rms(outMain, from));
    expect(jt::near(g2, 0.5, 0.02) && jt::rms(send0, from) < 1e-6 && jt::rms(outMain, from) < 1e-6,
           linear ? "splitter (linear phase): band send at -6 dB, send mute, main mute independent"
                  : "splitter: band send at -6 dB into its output buffer, send mute, main mute independent");
  }
  const PluginInfo* pi = findPlugin("gl.splitter");
  expect(pi && pi->sendParams.size() == 4 && pi->sendParams[2] == sp::splitParam2(2, sp::SxSendTo),
         "splitter: send targets declared as PluginInfo::sendParams");
}

// Engine: a splitter on insert 1 sends its high band to insert 2; insert 1
// does not reach the master, so whatever is heard came through the send.
void testSplitterEngine() {
  auto make = [](double sendDb, bool route) {
    gl::Document d(gl::makeEmptySong());
    const gl::Song& s = d.song();
    gl::ObjectId pat = s.patterns[0].id;
    gl::ObjectId ch = d.addChannel("lead", "gl.synth");
    d.routeChannel(ch, s.mixer[1].id);
    d.addNote(pat, ch, {0, 384, 84, 110});
    d.addClip(pat, 0, 0);
    d.setSend(s.mixer[1].id, s.mixer[0].id, 0.0f);  // insert 1 → master off
    d.setSlotPlugin(s.mixer[1].id, 0, "gl.splitter");
    gl::ObjectId t1 = s.mixer[1].id;
    d.setParam(gl::slotParam(t1, 0, sp::splitParam2(2, sp::SxSendTo)), route ? 2 : -1, false);
    d.setParam(gl::slotParam(t1, 0, sp::splitParam2(2, sp::SxSend)), sendDb, false);
    gl::RenderOptions o;
    o.blockSize = 512;
    return gl::renderSong(d.song(), o);
  };
  float on = make(0, true).peak(), quiet = make(sp::kSplitSilentDb, true).peak(), off = make(0, false).peak();
  std::printf("# splitter engine send: routed %.4f, silent level %.6f, unrouted %.6f\n", on, quiet, off);
  expect(on > 0.01f && quiet < 1e-6f && off < 1e-6f, "engine: splitter band send reaches another insert (multi-send path)");
}

void testSplitterLinear() {
  PluginState s = st("gl.splitter");
  setp(s, sp::SpLinear, 1);
  auto fx = createEffect("gl.splitter");
  fx->prepareHost(s, jt::kSr);
  std::vector<float> L(16384, 0.f), R(16384, 0.f);
  L[0] = R[0] = 1.f;
  jt::run(*fx, s, L, R, 256);
  size_t pk = 0;
  for (size_t i = 0; i < L.size(); ++i)
    if (std::fabs(L[i]) > std::fabs(L[pk])) pk = i;
  // J-42: the reference's FFT filters (3 bands): latency 3N/4 (1536 at quality 2 / 48 kHz).
  expect(fx->latencyFor(s, jt::kSr) == 1536 && pk == 1536, "splitter linear phase: latency 3N/4 (1536 at quality 2 / 48 kHz), impulse centred");
  double sym = 0;
  for (int k = 1; k < 900; ++k) sym = std::max(sym, static_cast<double>(std::fabs(L[pk + static_cast<size_t>(k)] - L[pk - static_cast<size_t>(k)])));
  expect(sym < 1e-4, "splitter linear phase: symmetric impulse response (linear phase)");
  // Flat sum for an LR slope (24 dB), and the slope itself.
  double worst = 0;
  for (double f : {60.0, 150.0, 200.0, 260.0, 1000.0, 2500.0, 3000.0, 3600.0, 9000.0}) worst = std::max(worst, std::fabs(gainAt(s, f)));
  std::printf("# splitter linear-phase sum deviation %.4f dB\n", worst);
  expect(worst < 0.1, "splitter linear phase: LR bands sum flat (+-0.1 dB)");
  PluginState lo = s;
  setp(lo, sp::splitParam(1, sp::SbMute), 1);
  setp(lo, sp::splitParam(2, sp::SbMute), 1);
  // J-42: reference mask: −6 dB at the split, then 24 dB per "6 dB octave" (2^(−(6 + 24·oct)/6)).
  const double lp = std::pow(2.0, (-6.0 - 24.0 * std::log2(800.0 / 200.0)) / 6.0);
  double got = gainAt(lo, 800);
  expect(std::fabs(got - 20 * std::log10(lp)) < 0.5, "splitter linear phase: low band follows the reference 24 dB mask");
  // Crossover moves are crossfaded (no clicks): output stays bounded.
  {
    auto f2 = createEffect("gl.splitter");
    f2->prepareHost(s, jt::kSr);
    PluginState m = s;
    std::vector<float> a = jt::sine(300, 48000, 0.5), b = a;
    ProcessContext ctx;
    ctx.sampleRate = jt::kSr;
    float peak = 0;
    for (size_t i = 0; i < a.size(); i += 256) {
      setp(m, sp::SpFreq1, 150 + 200 * std::sin(static_cast<double>(i) / 3000.0));
      const size_t len = std::min<size_t>(256, a.size() - i);
      f2->process(m, ctx, a.data() + i, b.data() + i, static_cast<int>(len));
      for (size_t k = i; k < i + len; ++k) peak = std::max(peak, std::fabs(a[k]));
    }
    expect(jt::finite(a) && peak < 0.9f, "splitter linear phase: moving crossovers stay bounded (crossfaded redesign)");
  }
}

void testSplitterInvariance() {
  for (int linear = 0; linear < 2; ++linear) {
    PluginState s = st("gl.splitter");
    setp(s, sp::SpLinear, linear);
    setp(s, sp::SpSlopeA, 7);
    setp(s, sp::splitParam2(1, sp::SxMain), -4);
    std::vector<float> L1(20000), R1(20000);
    gl::dsp::Noise nz;
    for (size_t i = 0; i < L1.size(); ++i) L1[i] = nz.next() * 0.5f, R1[i] = nz.next() * 0.5f;
    std::vector<float> L2 = L1, R2 = R1;
    auto a = createEffect("gl.splitter"), b = createEffect("gl.splitter");
    a->prepareHost(s, jt::kSr);
    b->prepareHost(s, jt::kSr);
    jt::run(*a, s, L1, R1, 64);
    jt::run(*b, s, L2, R2, 1000);
    double d = 0;
    for (size_t i = 0; i < L1.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(L1[i] - L2[i]) + std::fabs(R1[i] - R2[i])));
    expect(d < 1e-6, linear ? "splitter linear phase: block-size invariant" : "splitter reference: block-size invariant");
  }
  // Extremes stay finite.
  PluginState s = st("gl.splitter");
  setp(s, sp::SpBands, 2);
  setp(s, sp::SpSlopeA, 7), setp(s, sp::SpSlopeB, 7), setp(s, sp::SpSlopeC, 7);
  setp(s, sp::SpFreq1, 20), setp(s, sp::SpFreq2, 20000), setp(s, sp::SpFreq3, 20000);
  for (int b = 0; b < 4; ++b) setp(s, sp::splitParam2(b, sp::SxMain), 18);
  std::vector<float> L(30000), R(30000);
  gl::dsp::Noise nz;
  for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = nz.next();
  auto fx = createEffect("gl.splitter");
  jt::run(*fx, s, L, R, 333);
  setp(s, sp::SpLinear, 1), setp(s, sp::SpQuality, 5);
  auto fx2 = createEffect("gl.splitter");
  fx2->prepareHost(s, 192000);
  jt::run(*fx2, s, L, R, 4096, 192000);
  expect(jt::finite(L) && jt::finite(R), "splitter: extreme settings (96 dB, 20 Hz / 20 kHz, +18 dB, highest quality) stay finite");
  // Pre-J-1 states keep the legacy engine.
  PluginState old = st("gl.splitter");
  old.params.resize(sp::SpModel);
  normalizePluginState(old);
  expect(old.params[sp::SpModel] == 0 && st("gl.splitter").params[sp::SpModel] == 1, "splitter: old states normalise to the legacy model");
}

void testSplitterPresets() {
  if (!jt::haveFl()) return;
  int exact = 0, total = 0;
  for (const std::string& n : presetNames("Frequency Splitter")) {
    flp::PresetResult r;
    if (!jt::flPreset(n, r)) continue;
    ++total;
    exact += r.exact ? 1 : 0;
    const auto& p = r.state.params;
    const std::string nm = fs::path(n).stem().string();
    if (nm == "Low boost")
      expect(jt::near(p[sp::splitParam2(0, sp::SxMain)], 6.0, 0.02) && p[sp::SpBands] == 1, "preset Low boost: +6 dB low band");
    if (nm == "Isolate over 4kHz")
      expect(p[sp::SpBands] == 0 && p[sp::splitParam(0, sp::SbMute)] == 1 && p[sp::splitParam(1, sp::SbMute)] == 0 &&
                 jt::near(p[sp::SpFreq1], sp::splitRefHz(51069), 0.5) && p[sp::SpSlopeA] == 2,
             "preset Isolate over 4kHz: 2 bands, low muted, high kept, 18 dB");
    if (nm == "Steep tri band") expect(p[sp::SpSlopeA] == 7 && p[sp::SpSlopeB] == 7, "preset Steep tri band: 96 dB slopes");
    if (nm == "Mastering curve")
      expect(jt::near(p[sp::splitParam2(1, sp::SxMain)], -3.0, 0.05) && p[sp::SpSlopeA] == 0, "preset Mastering curve: -3 dB mid, 6 dB slopes");
  }
  std::printf("# Frequency Splitter presets exact: %d/%d\n", exact, total);
  expect(total == 9 && exact == 9, "Frequency Splitter presets decode exactly (9/9)");
}

// ------------------------------------------------------------------- vocoder
void testVocoderCurves() {
  expect(jt::near(sp::vxScaleHz(sp::kVxDistLo), 82.4, 0.1) && jt::near(sp::vxScaleHz(sp::kVxDistHi), 20000, 40) &&
             jt::near(sp::vxScaleHz(1.0), 21096.2, 0.1) && jt::near(sp::vxScaleHz(3.0 / 28.0), 65.406, 0.01),
         "vocoder: reference frequency scale (82.4 Hz .. 20 kHz default band range)");
  expect(jt::near(sp::vxAttackMs(1115), 2.0, 0.01) && jt::near(sp::vxReleaseMs(3500), 31.3, 0.1) && jt::near(sp::vxHoldMs(1666), 0.91, 0.01) &&
             jt::near(sp::vxReleaseMs(10000), 2000, 1e-6) && jt::near(sp::vxAttackMs(10000), 1000, 1e-6),
         "vocoder: hold / attack / release time curves");
  expect(jt::near(sp::vxFilterHz(55000), 5.8e3, 50) && jt::near(sp::vxFilterHz(28000), 503, 2) && jt::near(sp::vxMapGain(0.5), 1, 1e-12) &&
             jt::near(sp::vxMapTime(0.5), 1, 1e-12) && jt::near(sp::vxMapWidth(0.5), 1, 1e-12) && sp::vxMapOffset(0.5) == 0,
         "vocoder: filter wheel curve and per-band curve laws");
  // Band centres of the default layout: band k at the distribution curve's y(k / (N - 1)).
  double f0 = sp::vxScaleHz(sp::vxEval(sp::vxDefaultData().maps[sp::VmDist], 0.0));
  double f46 = sp::vxScaleHz(sp::vxEval(sp::vxDefaultData().maps[sp::VmDist], 1.0));
  expect(jt::near(f0, 82.4, 0.1) && jt::near(f46, 20000, 40), "vocoder: default 47-band layout spans 82 Hz .. 20 kHz");
  // Data round trip.
  sp::VxData d = sp::vxDefaultData(), e;
  d.maps[sp::VmGain] = {{0, 0.25, 0}, {0.5, 0.75, 0.3}, {0.5, 0.1, -0.2}};
  d.keys = {60, 64, 67};
  d.pf[0] = true;
  expect(sp::vxParse(sp::vxSerialize(d), e) && e.keys == d.keys && e.pf[0] && e.pf[3] && !e.pf[5] && e.maps[0].size() == 3 &&
             jt::near(e.maps[0][1].y, 0.75, 1e-9) && jt::near(sp::vxEval(e.maps[0], 0.5), 0.75, 1e-9),
         "vocoder: curve / key data round-trips");
  // Stagger tuning: order 3 outer resonators half width, symmetric offsets; flatness 0 = coincident.
  double off[4], w[4];
  sp::vxStagger(3, 1.0, 0.6, off, w);
  bool ok3 = jt::near(off[0], -0.3, 1e-12) && off[1] == 0 && jt::near(off[2], 0.3, 1e-12) && jt::near(w[0], w[1] * 0.5, 1e-12);
  sp::vxStagger(4, 0.0, 0.6, off, w);
  bool ok0 = off[0] == 0 && off[3] == 0 && jt::near(w[0], 0.6 * 0.3636, 1e-12);
  expect(ok3 && ok0, "vocoder: resonator stagger tuning (order 3/4, flatness)");
  // Flatness flattens the band top: order-2 cascade response across the band.
  auto topRipple = [](double flat) {
    double off2[4], w2[4];
    sp::vxStagger(2, flat, 0.5, off2, w2);
    gl::packs::spectral::blk::Bq q[2];
    for (int k = 0; k < 2; ++k) q[k].bandpass(48000, 1000 * std::pow(2.0, off2[k]), 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * w2[k])));
    auto mag = [&](double hz) { return q[0].magnitude(hz, 48000) * q[1].magnitude(hz, 48000); };
    double c = mag(1000), e1 = mag(1000 * std::pow(2.0, 0.1));
    return 20 * std::log10(c / e1);
  };
  expect(std::fabs(topRipple(1.0)) < std::fabs(topRipple(0.0)), "vocoder: flatness widens / flattens the band top");
}

// Runs the vocoder with L = modulator, R = carrier (L-R encoding).
std::vector<float> vocRun(const PluginState& s, const std::vector<float>& mod, const std::vector<float>& car, int block = 256,
                          std::vector<float>* right = nullptr) {
  auto fx = createEffect("gl.vocoder");
  ChannelAux aux;
  gl::packs::prepareAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = jt::kSr;
  std::vector<float> L = mod, R = car;
  for (size_t i = 0; i < L.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - i));
    fx->processAux(s, aux, ctx, L.data() + i, R.data() + i, n);
  }
  if (right) *right = R;
  return L;
}
std::vector<float> vocNoise(size_t n, float amp, uint32_t seed) {
  gl::dsp::Noise nz;
  nz.seed(seed);
  std::vector<float> v(n);
  for (auto& x : v) x = amp * nz.next();
  return v;
}
// Amplitude of the hz component (Goertzel over [from, end)).
double vocTone(const std::vector<float>& x, double hz, size_t from) {
  double w = 2 * 3.14159265358979 * hz / jt::kSr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = from; i < x.size(); ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
  return 2 * std::sqrt(re * re + im * im) / static_cast<double>(x.size() - from);
}
// Energy of a signal through a band-pass at hz.
double vocBand(const std::vector<float>& x, double hz, size_t from) {
  gl::packs::spectral::blk::Bq q;
  q.bandpass(jt::kSr, hz, 8);
  double e = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    double y = q.process(x[i]);
    if (i >= from) e += y * y;
  }
  return std::sqrt(e / static_cast<double>(x.size() - from));
}

void testVocoderDsp() {
  PluginState base = st("gl.vocoder");
  setp(base, sp::VxLrEncoding, 1);
  setp(base, sp::VcModLevel, -60), setp(base, sp::VcCarLevel, -60), setp(base, sp::VxEnhance, 0);
  const size_t n = 48000;
  std::vector<float> car = vocNoise(n, 0.3f, 7);
  // Spectrum transfer: modulator sine at 1 kHz → output energy near 1 kHz, not at 4 kHz.
  {
    std::vector<float> out = vocRun(base, jt::sine(1000, n, 0.3), car);
    double at1 = vocBand(out, 1000, n / 2), at4 = vocBand(out, 4000, n / 2), at250 = vocBand(out, 250, n / 2);
    double lvl = jt::rms(out, n / 2);
    std::printf("# vocoder ref: 1 kHz %.5f, 4 kHz %.6f, 250 Hz %.6f, out rms %.4f\n", at1, at4, at250, lvl);
    expect(at1 > 20 * at4 && at1 > 20 * at250, "vocoder ref: modulator spectrum transfers onto the carrier (band centres)");
    expect(lvl > 0.01 && lvl < 1.5 && jt::finite(out), "vocoder ref: output level sane");
    std::vector<float> silent = vocRun(base, std::vector<float>(n, 0.f), car);
    expect(jt::rms(silent, n / 2) < 1e-5, "vocoder ref: silent modulator gates the carrier");
  }
  // Attack time: a short attack reaches the level sooner than a long one.
  {
    std::vector<float> mod(n / 4, 0.f);
    for (size_t i = 2400; i < mod.size(); ++i) mod[i] = static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 1000 * static_cast<double>(i) / jt::kSr));
    std::vector<float> c4(car.begin(), car.begin() + static_cast<std::ptrdiff_t>(mod.size()));
    PluginState fast = base, slow = base, curved = base;
    setp(fast, sp::VxAttackMs, 1), setp(slow, sp::VxAttackMs, 300);
    setp(curved, sp::VxAttackMs, 1), setp(curved, sp::VxAttCurve, 4);
    auto early = [&](const PluginState& s) {
      std::vector<float> o = vocRun(s, mod, c4);
      double e = 0;
      for (size_t i = 2400; i < 2400 + 960; ++i) e += static_cast<double>(o[i]) * o[i];
      return e;
    };
    double ef = early(fast), es = early(slow), ec = early(curved);
    std::printf("# vocoder attack energy (20 ms): fast %.5f curved %.5f slow %.6f\n", ef, ec, es);
    expect(ef > 5 * es && ec > es && jt::finite(mod), "vocoder ref: attack time / curve shape the envelope rise");
  }
  // Release + freeze.
  {
    std::vector<float> mod = jt::sine(1000, n / 2, 0.3);
    for (size_t i = n / 4; i < mod.size(); ++i) mod[i] = 0;
    std::vector<float> c2(car.begin(), car.begin() + static_cast<std::ptrdiff_t>(mod.size()));
    PluginState shortR = base, longR = base;
    setp(shortR, sp::VxReleaseMs, 5), setp(longR, sp::VxReleaseMs, 1500);
    auto tail = [&](const PluginState& s) {
      std::vector<float> o = vocRun(s, mod, c2);
      double e = 0;
      for (size_t i = n / 4 + 2400; i < n / 4 + 7200; ++i) e += static_cast<double>(o[i]) * o[i];
      return e;
    };
    double es = tail(shortR), el = tail(longR);
    expect(el > 50 * es, "vocoder ref: release time sets the decay");
    PluginState fr = base;
    setp(fr, sp::VcHold, 1);
    std::vector<float> o = vocRun(fr, std::vector<float>(n / 4, 0.f), std::vector<float>(c2.begin(), c2.begin() + static_cast<std::ptrdiff_t>(n / 4)));
    expect(jt::rms(o) < 1e-5, "vocoder ref: freeze holds the (empty) envelope");
  }
  // Hold mode RMS vs peak; orders 1..4 all work.
  {
    std::vector<float> mod = vocNoise(n / 2, 0.3f, 21), c2(car.begin(), car.begin() + static_cast<std::ptrdiff_t>(n / 2));
    double lv[4], lh[4];
    for (int o = 0; o < 4; ++o) {
      PluginState s = base;
      setp(s, sp::VxOrder, o);
      lv[o] = jt::rms(vocRun(s, mod, c2), n / 4);
      setp(s, sp::VxFlatness, 0.5);
      lh[o] = jt::rms(vocRun(s, mod, c2), n / 4);
    }
    PluginState r = base;
    setp(r, sp::VxHoldMode, 1);
    double lr = jt::rms(vocRun(r, mod, c2), n / 4);
    PluginState syn = st("gl.vocoder");
    setp(syn, sp::VxEnhance, 0);
    double ls = jt::rms(vocRun(syn, mod, mod), n / 4);
    std::printf("# vocoder order levels (noise in/carrier) %.4f %.4f %.4f %.4f (flatness 0.5: %.4f %.4f %.4f %.4f), rms mode %.4f, synth carrier %.4f\n",
                lv[0], lv[1], lv[2], lv[3], lh[0], lh[1], lh[2], lh[3], lr, ls);
    double lo = 1e9, hi = 0;
    for (int o = 0; o < 4; ++o) lo = std::min({lo, lv[o], lh[o]}), hi = std::max({hi, lv[o], lh[o]});
    expect(lo > 0.01 && hi < 2.5 * lo && lr > 0.5 * lv[1] && lr < 2 * lv[1], "vocoder ref: orders 1..4, flatness and RMS hold mode keep the level (within 8 dB)");
  }
  // Built-in carrier: held keys from the data; MIDI port overrides.
  {
    PluginState s = st("gl.vocoder");
    sp::VxData d = sp::vxDefaultData();
    d.keys = {57};
    s.data = sp::vxSerialize(d);
    setp(s, sp::VxEnhance, 0);
    std::vector<float> mod(n / 2, 0.f);
    for (double hz : {300.0, 660.0, 1500.0, 3000.0}) {
      std::vector<float> t1 = jt::sine(hz, n / 2, 0.1);
      for (size_t i = 0; i < mod.size(); ++i) mod[i] += t1[i];
    }
    std::vector<float> o = vocRun(s, mod, mod);
    double at220 = vocTone(o, 660, n / 4), at233 = vocTone(o, 699.5, n / 4);
    std::printf("# vocoder held key: 3rd harmonic 660 Hz %.5f, 699 Hz %.6f, rms %.4f\n", at220, at233, jt::rms(o, n / 4));
    expect(jt::rms(o, n / 4) > 0.005 && at220 > 10 * at233, "vocoder ref: built-in carrier plays the held key (A3)");
    // MIDI port.
    setp(s, sp::VxMidiPort, 7);
    auto fx = createEffect("gl.vocoder");
    ChannelAux aux;
    gl::packs::prepareAux(s, aux);
    MidiPortBus bus;
    ProcessContext ctx;
    ctx.sampleRate = jt::kSr;
    ctx.ports = &bus;
    std::vector<float> L = mod, R = mod;
    for (size_t i = 0; i < L.size(); i += 256) {
      bus.beginBlock();
      bus.set(7, 69, 100);
      fx->processAux(s, aux, ctx, L.data() + i, R.data() + i, static_cast<int>(std::min<size_t>(256, L.size() - i)));
    }
    expect(vocTone(L, 1320, n / 4) > 10 * vocTone(L, 660, n / 4), "vocoder ref: carrier follows notes on its MIDI port");
  }
  // Curves from the data: band gain curve low = silence on low bands.
  {
    PluginState s = base;
    sp::VxData d = sp::vxDefaultData();
    d.maps[sp::VmGain] = {{0, 0.5, 0}, {0.5, 0.5, 0}, {0, 0.0, 0}, {0.5, 0.0, 0}};  // upper half of the bands off
    s.data = sp::vxSerialize(d);
    std::vector<float> mod = vocNoise(n / 2, 0.3f, 5), c2(car.begin(), car.begin() + static_cast<std::ptrdiff_t>(n / 2));
    std::vector<float> o = vocRun(s, mod, c2), o2 = vocRun(base, mod, c2);
    double hi = vocBand(o, 6000, n / 4), hi2 = vocBand(o2, 6000, n / 4), lo = vocBand(o, 400, n / 4), lo2 = vocBand(o2, 400, n / 4);
    std::printf("# vocoder gain curve: 6 kHz %.6f (flat %.6f), 400 Hz %.6f (flat %.6f)\n", hi, hi2, lo, lo2);
    expect(hi < 0.05 * hi2 && jt::near(lo / lo2, 1.0, 0.2), "vocoder ref: band gain curve (data) shapes the bands");
  }
  // Block-size invariance and extremes.
  {
    PluginState s = base;
    setp(s, sp::VxUnison, 3);
    std::vector<float> mod = vocNoise(20000, 0.3f, 9), c2 = vocNoise(20000, 0.3f, 10);
    std::vector<float> a = vocRun(s, mod, c2, 64), b = vocRun(s, mod, c2, 1000);
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(d < 1e-6, "vocoder ref: block-size invariant");
    PluginState x = st("gl.vocoder");
    setp(x, sp::VxBands, 100), setp(x, sp::VxOrder, 3), setp(x, sp::VxUnison, 5), setp(x, sp::VxWidth, 4), setp(x, sp::VxModWidth, 4);
    setp(x, sp::VxAttackMs, 0), setp(x, sp::VxReleaseMs, 0), setp(x, sp::VxHoldMs, 0), setp(x, sp::VxMinTimes, 0);
    setp(x, sp::VxAttCurve, 4), setp(x, sp::VxRelCurve, 4), setp(x, sp::VcWetLevel, 18), setp(x, sp::VxNoise, 1), setp(x, sp::VxEnhance, 1);
    setp(x, sp::VcFormant, 12), setp(x, sp::VxCarVoices, 4), setp(x, sp::VxTone, 7), setp(x, sp::VxContourCar, 1);
    std::vector<float> o = vocRun(x, vocNoise(20000, 1.0f, 11), c2);
    setp(x, sp::VxWidth, 0.008), setp(x, sp::VcFormant, -12), setp(x, sp::VxBands, 5), setp(x, sp::VxHoldMode, 1);
    std::vector<float> o2 = vocRun(x, vocNoise(20000, 1.0f, 12), c2);
    expect(jt::finite(o) && jt::finite(o2), "vocoder ref: extreme settings stay finite");
  }
  // Old states keep the 0.5 engine.
  PluginState old = st("gl.vocoder");
  old.params.resize(sp::VxModel);
  normalizePluginState(old);
  expect(old.params[sp::VxModel] == 0, "vocoder: old states normalise to the legacy model");
}

void testVocoderPresets() {
  if (!jt::haveFl()) return;
  int exact = 0, total = 0;
  for (const std::string& n : presetNames("Vocodex")) {
    flp::PresetResult r;
    if (!jt::flPreset(n, r)) continue;
    ++total;
    exact += r.exact ? 1 : 0;
    const auto& p = r.state.params;
    sp::VxData d;
    bool parsed = sp::vxParse(r.state.data, d);
    const std::string nm = fs::path(n).stem().string();
    if (nm == "Default")
      expect(parsed && p[sp::VxModel] == 1 && jt::near(p[sp::VxBands], 47, 1e-6) && p[sp::VxOrder] == 1 && jt::near(p[sp::VcWetLevel], 0, 1e-4) &&
                 jt::near(p[sp::VcModLevel], 0, 1e-4) && jt::near(p[sp::VxReleaseMs], sp::vxReleaseMs(3500), 1e-3) &&
                 jt::near(p[sp::VxWidth], std::pow(5.0, 0.349) - 1, 1e-4) && p[sp::VcModSource] == 1 && p[sp::VxCarSource] == 1 &&
                 d.pf[3] && d.pf[4] && !d.pf[0] && d.keys.size() == 1 && d.keys[0] == 60 && r.exact,
             "preset Default: levels, times, band count, inputs, pitch-follow flags, keys");
    if (nm == "Clearer") expect(d.keys.size() == 2 && d.keys[0] == 64 && p[sp::VxTone] == 5 && p[sp::VxCarSource] == 0 && !r.exact,
                                "preset Clearer: synth carrier keys and tone family");
    if (nm == "Bark scale") expect(d.maps[sp::VmDist].size() == 24 && jt::near(d.maps[sp::VmDist][0].y, 0.09829, 1e-4), "preset Bark scale: band distribution curve");
    if (nm == "Robot") expect(jt::near(p[sp::VxBands], 24, 1e-6) && p[sp::VxHoldMode] == 1 && p[sp::VxOrder] == 0, "preset Robot: 24 bands, RMS hold, order 1");
    if (nm == "Sidechained carrier") expect(p[sp::VxCarSource] == 2 && p[sp::VcModSource] == 0, "preset Sidechained carrier: carrier from the sidechain");
    if (nm == "Stereo interlacing") expect(jt::near(p[sp::VxNoiseHp], sp::vxFilterHz(58000), 0.5) && jt::near(p[sp::VxModHp], sp::vxFilterHz(58000), 0.5),
                                           "preset (version 2): missing / legacy filter values");
    // Every decoded preset renders finite audio.
    std::vector<float> o = vocRun(r.state, vocNoise(9600, 0.3f, 1), vocNoise(9600, 0.3f, 2));
    if (!jt::finite(o)) expect(false, ("preset renders finite: " + nm).c_str());
  }
  std::printf("# Vocodex presets exact: %d/%d\n", exact, total);
  expect(total == 21 && exact == 6, "Vocodex presets: 21 decoded, the 6 external-carrier ones exact");
}
}  // namespace vs1

void testJ1Spectral() {
  vs1::testSplitterCurves();
  vs1::testSplitterIir();
  vs1::testSplitterSends();
  vs1::testSplitterEngine();
  vs1::testSplitterLinear();
  vs1::testSplitterInvariance();
  vs1::testSplitterPresets();
  vs1::testVocoderCurves();
  vs1::testVocoderDsp();
  vs1::testVocoderPresets();
}
}  // namespace
