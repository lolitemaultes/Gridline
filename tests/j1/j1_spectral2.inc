// J-1 spectral pack, part 2: pitch corrector, pitch shifter, spreader
// (reference models: gl.pitcher PcModel, gl.pitch PsEngine, gl.spread SrModel).
#include "../../src/packs/spectral/spectral.h"
#include "../../src/packs/spectral/spectral_blocks.h"

namespace {
namespace spc2 {
namespace sp = gl::packs::spectral;
constexpr double kPi = 3.14159265358979323846;

struct St {
  std::vector<float> L, R;
};
St stereo(const std::vector<float>& l, const std::vector<float>& r) { return St{l, r}; }
St mono(const std::vector<float>& x) { return St{x, x}; }

void run(Effect& fx, const PluginState& s, St& b, int block = 256, double sr = jt::kSr, MidiPortBus* bus = nullptr) {
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.ports = bus;
  for (size_t i = 0; i < b.L.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), b.L.size() - i));
    fx.process(s, ctx, b.L.data() + i, b.R.data() + i, n);
  }
}
St process(const PluginState& s, St b, int block = 256, MidiPortBus* bus = nullptr, double sr = jt::kSr) {
  auto fx = createEffect(s.type);
  if (fx) run(*fx, s, b, block, sr, bus);
  return b;
}
PluginState def(const char* type) { return defaultPluginState(type); }
void set(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }

// Goertzel magnitude (amplitude) of hz over [a, b).
double tone(const std::vector<float>& x, size_t a, size_t b, double hz, double sr = jt::kSr) {
  double w = 2 * kPi * hz / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = a; i < b; ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(b - a);
}
// Strongest frequency in [lo, hi] (coarse 1 Hz grid, then 0.05 Hz refinement).
double peakHz(const std::vector<float>& x, size_t a, size_t b, double lo, double hi) {
  double best = 0, bf = lo;
  for (double f = lo; f <= hi; f += 1.0) {
    double m = tone(x, a, b, f);
    if (m > best) {
      best = m;
      bf = f;
    }
  }
  double c = bf;
  for (double f = c - 1.0; f <= c + 1.0; f += 0.05) {
    double m = tone(x, a, b, f);
    if (m > best) {
      best = m;
      bf = f;
    }
  }
  return bf;
}
// Fundamental from the normalised autocorrelation (first peak above 0.8 of the
// maximum between minHz and maxHz), parabolic refinement.
double acfHz(const std::vector<float>& x, size_t a, size_t b, double minHz, double maxHz, double sr = jt::kSr) {
  const int lo = static_cast<int>(sr / maxHz), hi = static_cast<int>(sr / minHz) + 2;
  std::vector<double> r(static_cast<size_t>(hi + 2), 0.0);
  for (int t = lo - 1; t <= hi + 1; ++t) {
    double s = 0;
    for (size_t i = a; i + static_cast<size_t>(t) < b; ++i) s += static_cast<double>(x[i]) * x[i + static_cast<size_t>(t)];
    r[static_cast<size_t>(t)] = s;
  }
  double mx = 0;
  for (int t = lo; t <= hi; ++t) mx = std::max(mx, r[static_cast<size_t>(t)]);
  for (int t = lo; t <= hi; ++t) {
    double v = r[static_cast<size_t>(t)];
    if (v >= 0.8 * mx && v >= r[static_cast<size_t>(t - 1)] && v >= r[static_cast<size_t>(t + 1)]) {
      double y0 = r[static_cast<size_t>(t - 1)], y2 = r[static_cast<size_t>(t + 1)], den = y0 - 2 * v + y2;
      double off = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
      return sr / (t + off);
    }
  }
  return 0;
}
std::vector<float> saw(double hz, size_t n, double amp = 0.4) {
  std::vector<float> v(n);
  double ph = 0;
  for (size_t i = 0; i < n; ++i) {
    // band-limited-ish: 12 harmonics
    double s = 0;
    for (int h = 1; h <= 12; ++h) s += std::sin(2 * kPi * h * ph) / h;
    v[i] = static_cast<float>(amp * 0.55 * s);
    ph += hz / jt::kSr;
    ph -= std::floor(ph);
  }
  return v;
}
double maxDiff(const St& a, const St& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.L[i] - b.L[i]) + std::fabs(a.R[i] - b.R[i])));
  return d;
}
bool finiteSt(const St& s) { return jt::finite(s.L) && jt::finite(s.R); }

// Reference pitch corrector with a chromatic scale, instant correction, plain shifter.
PluginState pitcher(int scale = 0, double speed = 1.0, bool formant = false) {
  PluginState s = def("gl.pitcher");
  set(s, sp::PcModel, 1);
  set(s, sp::PcScale, scale);
  set(s, sp::PcKey, 0);
  set(s, sp::PcCorrSpeed, speed);
  set(s, sp::PcPreserve, formant ? 1 : 0);
  return s;
}
}  // namespace spc2

void testJ1Spectral2() {
  using namespace spc2;
  const double sr = jt::kSr;
  const size_t N1 = static_cast<size_t>(sr), N2 = static_cast<size_t>(2 * sr);

  // ============================================================ pitch corrector
  {  // glide law: alpha = 8.82 / (sr (3x^3 + 0.001)), x = 1 - speed; instant above 0.99
    expect(sp::pitcherGlideAlpha(1.0, sr) == 1.0 && sp::pitcherGlideAlpha(0.995, sr) == 1.0, "pitcher glide: instant at full speed");
    expect(jt::near(sp::pitcherGlideAlpha(0.5, sr), 8.82 / (sr * (3 * 0.125 + 0.001)), 1e-12), "pitcher glide: per-sample coefficient");
    expect(jt::near(sp::pitcherGlideMs(0.0), 1000.0 * 3.001 / 8.82, 1e-9) && jt::near(sp::pitcherGlideMs(0.9), 1000.0 * 0.004 / 8.82, 1e-9),
           "pitcher glide: time constant 0.45 .. 340 ms");
  }
  {  // detector on sines, incl. the 220 Hz and 25 Hz ranges and the 1 kHz ceiling
    auto detect = [&](double hz, double fmin) {
      sp::blk::AmdfDetector d;
      d.configure(sr, fmin);
      std::vector<float> x = jt::sine(hz, static_cast<size_t>(0.5 * sr), 0.5);
      for (float v : x) d.push(v);
      return d.hz();
    };
    double a = detect(440, 110), b = detect(300, 220), c = detect(110, 220), e = detect(30, 25), f = detect(55, 25), g = detect(900, 110);
    double h = detect(100, 110);
    std::printf("# pitcher detector: 440->%.2f 300(220)->%.2f 110(220)->%.2f 30(25)->%.2f 55(25)->%.2f 900->%.1f 100(110)->%.1f\n", a, b, c, e,
                f, g, h);
    expect(jt::near(a, 440, 0.6) && jt::near(b, 300, 0.4), "pitcher detector: 440 Hz and 300 Hz (220 Hz range)");
    expect(c == 0 && h == 0, "pitcher detector: pitches below the range are unvoiced");
    expect(jt::near(e, 30, 0.1) && jt::near(f, 55, 0.1), "pitcher detector: 25 Hz range finds 30 Hz and 55 Hz");
    expect(jt::near(g, 900, 2.0), "pitcher detector: 900 Hz (below the 1 kHz ceiling)");
    sp::blk::AmdfDetector d;
    d.configure(sr, 110);
    for (int i = 0; i < 24000; ++i) d.push(0.f);
    expect(!d.voiced(), "pitcher detector: silence is unvoiced");
  }
  auto corrected = [&](const PluginState& s, double hz) {
    const size_t n = N2;
    St b = process(s, mono(jt::sine(hz, n, 0.5)));
    return std::make_pair(peakHz(b.L, n / 2, n, hz * 0.8, hz * 1.25), peakHz(b.R, n / 2, n, hz * 0.8, hz * 1.25));
  };
  {  // correction to the scale (plain shifter and PSOLA), fine tune
    auto r1 = corrected(pitcher(0), 452.0);
    auto r2 = corrected(pitcher(0, 1.0, true), 452.0);
    auto r3 = corrected(pitcher(1), 425.0);  // C major: no G# -> A
    std::printf("# pitcher ref 452 -> %.2f (plain) %.2f (formant); 425 major -> %.2f\n", r1.first, r2.first, r3.first);
    expect(jt::near(r1.first, 440, 1.0) && jt::near(r1.second, 440, 1.0), "pitcher ref: 452 Hz corrected to 440 (plain shifter)");
    expect(jt::near(r2.first, 440, 1.0), "pitcher ref: 452 Hz corrected to 440 (PSOLA, formant)");
    expect(jt::near(r3.first, 440, 1.0), "pitcher ref: scale respected (C major)");
    PluginState f = pitcher(0);
    set(f, sp::PcFine, 50);
    auto r4 = corrected(f, 452.0);
    double want = 440 * std::pow(2.0, 0.5 / 12);
    std::printf("# pitcher ref fine +50 ct -> %.2f (want %.2f)\n", r4.first, want);
    expect(jt::near(r4.first, want, 1.0), "pitcher ref: fine tune added to the correction");
  }
  {  // slow speed: the glide is still under way 0.3 s after the onset
    St b = process(pitcher(0, 0.0), mono(jt::sine(452, N1, 0.5)));
    double early = peakHz(b.L, static_cast<size_t>(0.15 * sr), static_cast<size_t>(0.3 * sr), 400, 480);
    St c = process(pitcher(0, 1.0), mono(jt::sine(452, N1, 0.5)));
    double fast = peakHz(c.L, static_cast<size_t>(0.15 * sr), static_cast<size_t>(0.3 * sr), 400, 480);
    std::printf("# pitcher speed 0 early %.2f, speed 1 early %.2f\n", early, fast);
    expect(early > 443 && jt::near(fast, 440, 1.5), "pitcher ref: speed sets the glide time");
  }
  {  // input-key bypass: no correction while the sung note is a bypassed key
    PluginState s = pitcher(0);
    set(s, sp::PcKeyBypass0 + 9, 1);  // A
    auto r1 = corrected(s, 452.0);
    PluginState t = pitcher(0);
    set(t, sp::PcKeyBypass0 + 0, 1);  // C (not sung)
    auto r2 = corrected(t, 452.0);
    PluginState u = pitcher(0);
    set(u, sp::PcBypassAll, 1);
    auto r3 = corrected(u, 452.0);
    std::printf("# pitcher key bypass A -> %.2f, bypass C -> %.2f, all -> %.2f\n", r1.first, r2.first, r3.first);
    expect(jt::near(r1.first, 452, 1.0) && jt::near(r2.first, 440, 1.0) && jt::near(r3.first, 452, 1.0),
           "pitcher ref: key bypass passes the sung key only; bypass all");
  }
  {  // lowest-pitch range: 150 Hz is unvoiced with the 220 Hz range, corrected with 110 Hz
    PluginState s = pitcher(0);
    set(s, sp::PcRange6, 0);
    auto r1 = corrected(s, 150.0);
    set(s, sp::PcRange6, 2);
    auto r2 = corrected(s, 150.0);
    std::printf("# pitcher range 220: 150 -> %.2f; range 110: 150 -> %.2f\n", r1.first, r2.first);
    expect(jt::near(r1.first, 150, 0.6) && jt::near(r2.first, 146.83, 0.6), "pitcher ref: range 220 Hz / 110 Hz");
  }
  {  // pitch reference channel: Left / Right / Both
    const double fl = 452, fr = 340;  // A4 +47 ct, F4 -46 ct (E4 +54 ct)
    auto runRef = [&](int ch) {
      PluginState s = pitcher(0);
      set(s, sp::PcRefChannel, ch);
      St b = process(s, stereo(jt::sine(fl, N2, 0.5), jt::sine(fr, N2, 0.5)));
      return std::make_pair(peakHz(b.L, N2 / 2, N2, 380, 520), peakHz(b.R, N2 / 2, N2, 290, 400));
    };
    auto left = runRef(0), right = runRef(2), both = runRef(1);
    std::printf("# pitcher ref L: %.2f / %.2f, R: %.2f / %.2f, both: %.2f / %.2f\n", left.first, left.second, right.first, right.second, both.first,
                both.second);
    expect(jt::near(left.first, 440, 1.0) && jt::near(left.second, fr * 440 / fl, 1.0), "pitcher ref: left channel drives both outputs");
    expect(jt::near(right.second, 349.23, 1.0) && jt::near(right.first, fl * 349.23 / fr, 1.2), "pitcher ref: right channel drives both outputs");
    expect(jt::near(both.first, 440, 1.0) && jt::near(both.second, 349.23, 1.0), "pitcher ref: both channels detect separately");
  }
  {  // MIDI targets from the port bus (pitch classes, then exact notes with Octaves)
    MidiPortBus bus;
    bus.set(3, 60, 100);  // C4: pitch class C
    PluginState s = pitcher(0);
    set(s, sp::PcMidiTargets, 1);
    set(s, sp::PcMidiPort, 3);
    St b = process(s, mono(jt::sine(452, N2, 0.5)), 256, &bus);
    double c5 = peakHz(b.L, N2 / 2, N2, 400, 600);
    MidiPortBus bus2;
    bus2.set(0, 64, 100);  // E4 exactly
    PluginState o = pitcher(0);
    set(o, sp::PcMidiTargets, 1);
    set(o, sp::PcMidiOctaves, 1);
    set(o, sp::PcMidiPort, -1);  // any port
    St c = process(o, mono(jt::sine(452, N2, 0.5)), 256, &bus2);
    double e4 = peakHz(c.L, N2 / 2, N2, 280, 480);
    PluginState w = o;
    set(w, sp::PcMidiPort, 7);  // nothing held there: no correction
    St d = process(w, mono(jt::sine(452, N2, 0.5)), 256, &bus2);
    double none = peakHz(d.L, N2 / 2, N2, 380, 520);
    std::printf("# pitcher MIDI targets: C -> %.2f, E4 exact -> %.2f, empty port -> %.2f\n", c5, e4, none);
    expect(jt::near(c5, 523.25, 1.2), "pitcher ref: MIDI pitch-class target (C5 nearest)");
    expect(jt::near(e4, 329.63, 1.0), "pitcher ref: MIDI octaves target (exact note)");
    expect(jt::near(none, 452, 1.0), "pitcher ref: no held notes on the port, no correction");
  }
  {  // harmonize: voices at the held notes' pitch, pan x stereo, velocity, replace / mix
    MidiPortBus bus;
    bus.set(0, 64, 127);  // E4
    bus.set(0, 67, 127);  // G4
    for (int formant = 0; formant < 2; ++formant) {
      PluginState s = pitcher(0, 1.0, formant != 0);
      set(s, sp::PcHarmonize, 1);
      set(s, sp::PcMidiPort, 0);
      set(s, sp::PcHarmPan1, -1);  // voice 1 (E4) hard left
      set(s, sp::PcHarmPan1 + 1, 0);
      St b = process(s, mono(jt::sine(440, N2, 0.4)), 256, &bus);
      double eL = tone(b.L, N2 / 2, N2, 329.63), eR = tone(b.R, N2 / 2, N2, 329.63), gL = tone(b.L, N2 / 2, N2, 392.0);
      double gR = tone(b.R, N2 / 2, N2, 392.0), aL = tone(b.L, N2 / 2, N2, 440.0);
      std::printf("# harmonize (%s): E4 L %.3f R %.3f, G4 L %.3f R %.3f, A4 L %.3f\n", formant ? "psola" : "plain", eL, eR, gL, gR, aL);
      expect(eL > 0.15 && gL > 0.15 && gR > 0.15, (std::string("pitcher harmonize: voices at the held notes (") + (formant ? "PSOLA)" : "plain)")).c_str());
      expect(eR < 0.05 * eL, "pitcher harmonize: voice pan x stereo (voice 1 hard left)");
      expect(aL < 0.1 * gL, "pitcher harmonize: replace mode drops the input");
    }
    PluginState s = pitcher(0);
    set(s, sp::PcHarmonize, 1);
    set(s, sp::PcMidiPort, 0);
    set(s, sp::PcHarmMix, 1);
    St b = process(s, mono(jt::sine(440, N2, 0.4)), 256, &bus);
    double a = tone(b.L, N2 / 2, N2, 440.0), e = tone(b.L, N2 / 2, N2, 329.63);
    expect(a > 0.3 && e > 0.15, "pitcher harmonize: mix mode adds the dry input");
    PluginState stereo0 = s;
    set(stereo0, sp::PcHarmMix, 0);
    set(stereo0, sp::PcHarmPan1, -1);
    set(stereo0, sp::PcStereo, 0);
    St c = process(stereo0, mono(jt::sine(440, N2, 0.4)), 256, &bus);
    expect(tone(c.R, N2 / 2, N2, 329.63) > 0.15, "pitcher harmonize: stereo 0 centres the voices");
    // Velocity: 2 v^2 (v = 64/127 -> 0.508) against full velocity (2.0).
    MidiPortBus soft, loud;
    soft.set(0, 64, 64);
    loud.set(0, 64, 127);
    PluginState v = pitcher(0);
    set(v, sp::PcHarmonize, 1);
    set(v, sp::PcHarmVel, 1);
    set(v, sp::PcMidiPort, 0);
    St x1 = process(v, mono(jt::sine(440, N2, 0.2)), 256, &soft), x2 = process(v, mono(jt::sine(440, N2, 0.2)), 256, &loud);
    double r = tone(x1.L, N2 / 2, N2, 329.63) / std::max(1e-9, tone(x2.L, N2 / 2, N2, 329.63));
    std::printf("# harmonize velocity ratio %.3f (want %.3f)\n", r, (64.0 * 64.0) / (127.0 * 127.0));
    expect(jt::near(r, (64.0 * 64.0) / (127.0 * 127.0), 0.03), "pitcher harmonize: velocity scales the voice by 2 v^2");
    // Nothing held, replace mode: the input passes unshifted.
    MidiPortBus empty;
    PluginState q = pitcher(0);
    set(q, sp::PcHarmonize, 1);
    St y = process(q, mono(jt::sine(452, N2, 0.4)), 256, &empty);
    expect(jt::near(peakHz(y.L, N2 / 2, N2, 400, 500), 452, 1.0) && tone(y.L, N2 / 2, N2, 452) > 0.3, "pitcher harmonize: no notes held passes the input");
  }
  {  // block-size invariance + finite extremes (reference model features on)
    MidiPortBus bus;
    bus.set(0, 62, 90);
    bus.set(0, 69, 120);
    PluginState s = pitcher(1, 0.7, true);
    set(s, sp::PcHarmonize, 1);
    set(s, sp::PcHarmMix, 1);
    set(s, sp::PcRefChannel, 0);
    St in = stereo(jt::sine(300, N1, 0.4), jt::sine(207, N1, 0.3));
    St a = process(s, in, 64, &bus), b = process(s, in, 509, &bus);
    expect(maxDiff(a, b) < 1e-5, "pitcher ref: block-size invariant");
    PluginState t = pitcher(0, 0.3, false);
    set(t, sp::PcRefChannel, 2);
    St c = process(t, in, 64), d = process(t, in, 509);
    expect(maxDiff(c, d) < 1e-5, "pitcher ref: block-size invariant (plain shifter)");
    bool fin = true;
    for (double rate : {22050.0, 192000.0})
      for (int range : {0, 5}) {
        PluginState e = pitcher(0, 1.0, range == 5);
        set(e, sp::PcRange6, range);
        set(e, sp::PcFormant, 12);
        St x = mono(jt::sine(90, static_cast<size_t>(rate / 2), 0.9));
        St y = process(e, x, 333, &bus, rate);
        fin = fin && finiteSt(y);
      }
    expect(fin, "pitcher ref: finite at 22.05 / 192 kHz, 220 / 25 Hz ranges");
  }

  // ============================================================== pitch shifter
  {  // formant correction law and the delay curves
    expect(jt::near(sp::psFormantSt(7, 0, 1), 0, 1e-12) && jt::near(sp::psFormantSt(7, 0, 0), 7, 1e-12) &&
               jt::near(sp::psFormantSt(7, 2, 0.5), 5.5, 1e-12) && jt::near(sp::psFormantSt(12, 5, 0), 12, 1e-12),
           "pitch shifter: formant = formant + pitch (1 - correction), +-12 st");
    expect(jt::near(sp::psDelayMs(1), 1, 1e-12) && jt::near(sp::psDelayMs(768), 1000, 1e-9) && jt::near(sp::psDelaySteps(768), 16, 1e-12),
           "pitch shifter: delay 1 + 999 n ms, 16 n steps");
  }
  auto shifter = [&](int engine) {
    PluginState s = def("gl.pitch");
    set(s, sp::PsEngine, engine);
    return s;
  };
  {  // Music (granular) engine ratios
    for (double st : {7.0, -12.0, 3.0}) {
      PluginState s = shifter(sp::kPsMusic);
      set(s, sp::PsPitch, st);
      St b = process(s, mono(jt::sine(440, N2, 0.5)));
      // Overlapping grains form lines spaced by the grain rate around the
      // shifted pitch: compare the power centroid near the target.
      const double want = 440 * std::pow(2.0, st / 12);
      double pw = 0, pf = 0;
      for (double f = want * 0.85; f <= want * 1.15; f += 1.0) {
        double m = tone(b.L, N2 / 2, N2, f);
        pw += m * m;
        pf += m * m * f;
      }
      const double got = pf / std::max(1e-30, pw);
      const double orig = st > 0 ? tone(b.L, N2 / 2, N2, 440.0) : 0.0;
      std::printf("# music %+g st: centroid %.2f (want %.2f), rms %.3f\n", st, got, want, jt::rms(b.L, N2 / 2));
      expect(jt::near(got, want, want * 0.015) && jt::rms(b.L, N2 / 2) > 0.15 && orig < 0.1,
             ("pitch shifter (music) moves a sine by " + std::to_string(static_cast<int>(st)) + " st").c_str());
    }
  }
  {  // Voice (PSOLA) engine: correction 0 moves a sine; correction 1 on a harmonic tone
    PluginState s = shifter(sp::kPsVoice);
    set(s, sp::PsPitch, 7);
    set(s, sp::PsCorrection, 0);
    St b = process(s, mono(jt::sine(220, N2, 0.5)));
    double want = 220 * std::pow(2.0, 7 / 12.0), got = peakHz(b.L, N2 / 2, N2, 150, 500);
    std::printf("# voice +7 (corr 0): %.2f (want %.2f)\n", got, want);
    expect(jt::near(got, want, want * 0.004), "pitch shifter (voice): +7 st with formants following");
    PluginState k = shifter(sp::kPsVoice);
    set(k, sp::PsPitch, 5);
    set(k, sp::PsCorrection, 1);
    St c = process(k, mono(saw(165, N2)));
    double f0 = acfHz(c.L, N2 / 2, N2 / 2 + 8192, 120, 400), w5 = 165 * std::pow(2.0, 5 / 12.0);
    std::printf("# voice +5 (corr 1) on a harmonic tone: f0 %.2f (want %.2f)\n", f0, w5);
    expect(jt::near(f0, w5, w5 * 0.01), "pitch shifter (voice): +5 st keeping formants");
    // Fixed pitch modes: the output locks to C4 / C5 (+ pitch).
    PluginState a = shifter(sp::kPsVoice);
    set(a, sp::PsPitchMode, 2);  // C5 = 261.6 Hz
    St d = process(a, mono(saw(200, N2)));
    double fa = acfHz(d.L, N2 / 2, N2 / 2 + 8192, 120, 600);
    set(a, sp::PsPitchMode, 1);  // C4 = 130.8 Hz, + 12 st
    set(a, sp::PsPitch, 12);
    St e = process(a, mono(saw(200, N2)));
    double fb = acfHz(e.L, N2 / 2, N2 / 2 + 8192, 120, 600);
    std::printf("# voice fixed C5: %.2f, C4 + 12: %.2f (want 261.63)\n", fa, fb);
    expect(jt::near(fa, 261.63, 2.6) && jt::near(fb, 261.63, 2.6), "pitch shifter (voice): fixed C5 / C4 + 12 st pitch modes");
    // Analysis channel: right-only analysis follows the right input.
    PluginState an = shifter(sp::kPsVoice);
    set(an, sp::PsAnalysis, 1);
    St g = process(an, stereo(std::vector<float>(N1, 0.f), saw(150, N1)));
    expect(finiteSt(g) && jt::rms(g.R, N1 / 2) > 0.05, "pitch shifter (voice): right-channel analysis");
  }
  {  // delay modes on an unvoiced click (the voice engine is transparent there)
    auto clicks = [&](int mode, double fb) {
      PluginState s = shifter(sp::kPsVoice);
      set(s, sp::PsDelayMode, mode);
      set(s, sp::PsDelay, 100);
      set(s, sp::PsFeedback, fb);
      std::vector<float> x(N1, 0.f);
      x[2000] = 1.0f;
      St b = process(s, mono(x));
      return b.L;
    };
    std::vector<float> off = clicks(sp::PsDlyOff, 0);
    size_t p0 = 0;
    for (size_t i = 0; i < off.size(); ++i)
      if (std::fabs(off[i]) > std::fabs(off[p0])) p0 = i;
    const size_t D = static_cast<size_t>(0.1 * sr);
    auto at = [](const std::vector<float>& v, size_t i) {
      float m = 0;
      for (size_t k = i - 3; k <= i + 3 && k < v.size(); ++k) m = std::max(m, std::fabs(v[k]));
      return m;
    };
    std::vector<float> post = clicks(sp::PsDlyPost, 0), pre = clicks(sp::PsDlyPre, 0), nos = clicks(sp::PsDlyNoShift, 0);
    const size_t lat = p0 - 2000;
    std::printf("# delay modes: latency %zu, off echo %.3f, post %.3f, pre %.3f, no-shift %.3f (at %zu)\n", lat, at(off, p0 + D), at(post, p0 + D),
                at(pre, p0 + D), at(nos, 2000 + D), D);
    expect(at(off, p0) > 0.8 && at(off, p0 + D) < 0.05, "pitch shifter: delay off has no echo");
    expect(at(post, p0) > 0.8 && at(post, p0 + D) > 0.8, "pitch shifter: post-shift echo after the shifter");
    expect(at(pre, p0) > 0.8 && at(pre, p0 + D) > 0.8, "pitch shifter: pre-shift echo through the shifter");
    expect(at(nos, p0) > 0.8 && at(nos, 2000 + D) > 0.8 && (lat < 8 || at(nos, p0 + D) < 0.05), "pitch shifter: no-shift echo bypasses the shifter");
    std::vector<float> fb = clicks(sp::PsDlyNoShift, 0.6);
    double e2 = 0;
    for (size_t i = 2000 + 2 * D - 40; i < 2000 + 2 * D + 40; ++i) e2 += std::fabs(fb[i]);
    expect(e2 > 0.1 && jt::finite(fb), "pitch shifter: feedback repeats the echo");
  }
  {  // tempo-synced delay: 4 steps at 120 BPM = 0.5 s
    PluginState s = shifter(sp::kPsVoice);
    set(s, sp::PsDelayMode, sp::PsDlyNoShift);
    set(s, sp::PsDelaySync, 1);
    set(s, sp::PsDelaySteps, 4);
    std::vector<float> x(N1, 0.f);
    x[1000] = 1.0f;
    St b = mono(x);
    auto fx = createEffect("gl.pitch");
    ProcessContext ctx;
    ctx.sampleRate = sr;
    ctx.tempo = 120;
    for (size_t i = 0; i < N1; i += 256) fx->process(s, ctx, b.L.data() + i, b.R.data() + i, static_cast<int>(std::min<size_t>(256, N1 - i)));
    expect(std::fabs(b.L[1000 + static_cast<size_t>(0.5 * sr)]) > 0.8, "pitch shifter: synced delay 4 steps = 1/4 bar at 120 BPM");
  }
  {  // block invariance + extremes
    for (int engine : {sp::kPsVoice, sp::kPsMusic}) {
      PluginState s = shifter(engine);
      set(s, sp::PsPitch, 5);
      set(s, sp::PsDelayMode, sp::PsDlyPost);
      set(s, sp::PsDelay, 120);
      set(s, sp::PsFeedback, 0.5);
      set(s, sp::PsDetune, 20);
      St in = stereo(saw(180, N1), jt::sine(310, N1, 0.4));
      St a = process(s, in, 64), b = process(s, in, 509);
      expect(maxDiff(a, b) < 1e-5, engine == sp::kPsVoice ? "pitch shifter (voice): block-size invariant" : "pitch shifter (music): block-size invariant");
      set(s, sp::PsPitch, 24);
      set(s, sp::PsFeedback, 0.95);
      set(s, sp::PsDensity, 5);
      set(s, sp::PsJitter, 1);
      set(s, sp::PsRand, 1);
      set(s, sp::PsDuration, 150);
      St c = process(s, in, 333, nullptr, 192000), d = process(s, in, 333, nullptr, 22050);
      expect(finiteSt(c) && finiteSt(d), "pitch shifter: extremes finite at 192 / 22.05 kHz");
    }
  }

  // =================================================================== spreader
  auto spread = [&]() {
    PluginState s = def("gl.spread");
    set(s, sp::SrModel, 1);
    set(s, sp::SrBassMono, 0);
    set(s, sp::SrLowBypass, 0);
    return s;
  };
  auto noise = [&](size_t n, uint32_t seed) {
    std::vector<float> v(n);
    uint32_t st = seed;
    for (auto& x : v) {
      st = st * 1664525u + 1013904223u;
      x = static_cast<float>(0.3 * ((st >> 8) / 8388608.0 - 1.0));
    }
    return v;
  };
  {  // low-bypass curve
    expect(jt::near(sp::spreadLowHz(1000 + 999000 * 0.25), 250, 1e-9) && jt::near(sp::spreadLowHz(500500), 500, 1e-9) &&
               jt::near(sp::spreadLowHz(1000000), 10000, 1e-6) && sp::spreadLowHz(500) == 0,
           "spreader: low bypass 0..500 Hz, then 500 Hz..10 kHz");
  }
  {  // mono-safe widening: L+R unchanged; side energy grows; spread 0 is transparent
    PluginState s = spread();
    set(s, sp::SrSpread, 1);
    St in = stereo(noise(N1, 1), noise(N1, 2));
    St b = process(s, in);
    double md = 0, side = 0, sideIn = 0;
    for (size_t i = 0; i < N1; ++i) {
      md = std::max(md, static_cast<double>(std::fabs((b.L[i] + b.R[i]) - (in.L[i] + in.R[i]))));
      side += std::pow(b.L[i] - b.R[i], 2.0);
      sideIn += std::pow(in.L[i] - in.R[i], 2.0);
    }
    std::printf("# spreader ref: mono-sum error %.2e, side gain %.2f\n", md, side / sideIn);
    expect(md < 1e-5, "spreader ref: widening keeps L+R");
    expect(side > sideIn * 1.1, "spreader ref: widening adds side energy");
    St m = mono(noise(N1, 3));
    St w = process(s, m);
    double sw = 0;
    for (size_t i = 0; i < N1; ++i) sw += std::fabs(w.L[i] - w.R[i]);
    expect(sw > 10, "spreader ref: mono input gains width (decorrelation tap)");
    set(s, sp::SrSpread, 0);
    St z = process(s, in);
    expect(maxDiff(z, in) < 1e-6, "spreader ref: spread 0 / separation 0 is transparent");
    set(s, sp::SrSeparation, -1);
    St mm = process(s, in);
    double d = 0;
    for (size_t i = N1 / 10; i < N1; ++i) d = std::max(d, static_cast<double>(std::fabs(mm.L[i] - mm.R[i])));
    expect(d < 1e-4, "spreader ref: separation -100 % merges to mono");
  }
  {  // low bypass keeps the original stereo of the lows; mono bass removes it
    std::vector<float> lo = jt::sine(50, N1, 0.5), hi = jt::sine(3000, N1, 0.3);
    std::vector<float> l(N1), r(N1);
    for (size_t i = 0; i < N1; ++i) {
      l[i] = lo[i] + hi[i];
      r[i] = -lo[i] + hi[i];  // side at 50 Hz, mid at 3 kHz
    }
    St in = stereo(l, r);
    auto sideAt = [&](const St& b, double hz) {
      std::vector<float> s(N1);
      for (size_t i = 0; i < N1; ++i) s[i] = 0.5f * (b.L[i] - b.R[i]);
      return tone(s, N1 / 2, N1, hz);
    };
    PluginState s = spread();
    set(s, sp::SrSpread, 0);
    set(s, sp::SrSeparation, -1);  // merge everything above the split
    set(s, sp::SrLowFreq, 300);
    St a = process(s, in);
    set(s, sp::SrLowBypass, 1);
    St b = process(s, in);
    set(s, sp::SrLowBypass, 0);
    set(s, sp::SrSeparation, 0);
    set(s, sp::SrBassMono, 1);
    St c = process(s, in);
    std::printf("# spreader ref: 50 Hz side merged %.3f, low bypass %.3f, mono bass %.3f; 3 kHz mid kept %.3f\n", sideAt(a, 50), sideAt(b, 50),
                sideAt(c, 50), tone(c.L, N1 / 2, N1, 3000));
    expect(sideAt(a, 50) < 0.01 && sideAt(b, 50) > 0.45, "spreader ref: low bypass keeps the lows' stereo");
    expect(sideAt(c, 50) < 0.01 && tone(c.L, N1 / 2, N1, 3000) > 0.25, "spreader ref: mono bass monos the band below the split");
  }
  {  // enabled off: the input passes untouched
    PluginState s = spread();
    set(s, sp::SrSpread, 1);
    set(s, sp::SrSeparation, 0.5);
    set(s, sp::SrEnabled, 0);
    St in = stereo(noise(N1, 5), noise(N1, 6));
    St b = process(s, in);
    expect(maxDiff(b, in) < 1e-6, "spreader ref: disabled passes the input");
    PluginState old;  // pre-J-1 state: legacy model
    old.type = "gl.spread";
    old.params.assign(sp::SrModel, 0.0f);
    normalizePluginState(old);
    expect(old.params[sp::SrModel] == 0 && old.params[sp::SrEnabled] == 1, "spreader: old states normalise to the legacy model, enabled");
  }
  {  // block invariance, crossfaded tap changes, extremes
    PluginState s = spread();
    set(s, sp::SrSpread, 0.8);
    set(s, sp::SrLowBypass, 1);
    set(s, sp::SrBassMono, 1);
    set(s, sp::SrLowFreq, 2000);
    St in = stereo(noise(N1, 7), noise(N1, 8));
    St a = process(s, in, 64), b = process(s, in, 509);
    expect(maxDiff(a, b) < 1e-5, "spreader ref: block-size invariant");
    // Spread sweep: the tap crossfade keeps the output continuous.
    auto fx = createEffect("gl.spread");
    St x = mono(jt::sine(500, N1, 0.5));
    ProcessContext ctx;
    ctx.sampleRate = sr;
    PluginState t = spread();
    double jump = 0;
    for (size_t i = 0; i < N1; i += 64) {
      set(t, sp::SrSpread, 0.5 + 0.5 * std::sin(static_cast<double>(i) / 3000.0));
      fx->process(t, ctx, x.L.data() + i, x.R.data() + i, 64);
    }
    for (size_t i = 1; i < N1; ++i) jump = std::max(jump, static_cast<double>(std::fabs(x.L[i] - x.L[i - 1])));
    std::printf("# spreader ref: max step during a spread sweep %.3f\n", jump);
    expect(jump < 0.2, "spreader ref: spread changes crossfade without clicks");
    set(s, sp::SrLowFreq, 10000);
    St c = process(s, in, 333, nullptr, 22050), d = process(s, in, 333, nullptr, 192000);
    expect(finiteSt(c) && finiteSt(d), "spreader ref: finite at 22.05 / 192 kHz, 10 kHz split");
  }

  // ======================================================== model switches
  {
    for (const char* t : {"gl.pitcher", "gl.pitch", "gl.spread"}) {
      const PluginInfo* info = findPlugin(t);
      PluginState fresh = defaultPluginState(t);
      PluginState old;
      old.type = t;
      old.params.assign(static_cast<size_t>(info->legacyModelParam), 0.5f);
      normalizePluginState(old);
      expect(fresh.params[static_cast<size_t>(info->legacyModelParam)] != 0 && old.params[static_cast<size_t>(info->legacyModelParam)] == 0,
             (std::string("model switch: new instances reference, old states legacy: ") + t).c_str());
    }
  }

  // ================================================== FL presets / corpus
  if (jt::haveFl()) {
    int total = 0, exact = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(jt::kFlFx) / "Pitch Shifter")) {
      if (e.path().extension() != ".fst") continue;
      flp::PresetResult r;
      if (!flp::importFlPreset(e.path().string(), r) || r.kind != flp::PresetResult::Plugin) continue;
      ++total;
      exact += r.exact && r.state.type == "gl.pitch";
    }
    std::printf("# Pitch Shifter presets: %d/%d exact\n", exact, total);
    expect(total == 26 && exact == 26, "Pitch Shifter: all shipped presets decode exactly");
    // Every Pitcher / Pitch Shifter / Spreader state in FL's shipped projects,
    // templates and plugin databases.
    std::map<std::string, std::pair<int, int>> count;
    const std::string names[3] = {"Pitcher", "Pitch Shifter", "Spreader"};
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(jt::kFlData, fs::directory_options::skip_permission_denied, ec)) {
      const std::string ext = e.path().extension().string();
      if (!e.is_regular_file() || (ext != ".flp" && ext != ".fst")) continue;
      if (e.path().string().find("Plugin presets") != std::string::npos) continue;
      std::vector<uint8_t> bytes;
      {
        FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(static_cast<size_t>(std::max(0L, sz)));
        size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        bytes.resize(got);
      }
      bool any = false;
      for (const auto& nm : names) {
        std::string u16;
        for (char ch : nm) {
          u16.push_back(ch);
          u16.push_back('\0');
        }
        std::string hay(bytes.begin(), bytes.end());
        if (hay.find(u16) != std::string::npos || hay.find(nm) != std::string::npos) any = true;
      }
      if (!any) continue;
      flp::File file;
      std::string err;
      if (!flp::readBytes(bytes, file, &err)) continue;
      flp::Sections sec = flp::split(file);
      std::vector<const std::vector<flp::Event>*> lists;
      for (const auto& [k, evs] : sec.channels) lists.push_back(&evs);
      for (const auto& ins : sec.inserts)
        for (const auto& sl : ins.slots) lists.push_back(&sl.events);
      if (sec.channels.empty() && sec.inserts.empty()) lists.push_back(&file.events);
      for (const auto* evs : lists) {
        const flp::Event* nm = flp::find(*evs, 201);
        if (!nm) continue;
        std::string name = nm->text();
        if (name != "Pitcher" && name != "Pitch Shifter" && name != "Spreader") continue;
        if (!flp::find(*evs, 213)) continue;
        Song song;
        flp::Report rep;
        flp::Context ctx(file, song, rep);
        flp::PluginMapping pm;
        const flp::PluginDecoder* dec = flp::findDecoder(name);
        bool ok = dec && (*dec)(ctx, *evs, pm);
        count[name].second++;
        count[name].first += ok && pm.exact;
      }
    }
    for (const auto& [k, v] : count) std::printf("# FL corpus %-14s %d/%d exact\n", k.c_str(), v.first, v.second);
    expect(count["Pitcher"].second >= 60 && count["Pitcher"].first == count["Pitcher"].second, "Pitcher: every FL corpus state decodes exactly");
    expect(count["Spreader"].second >= 30 && count["Spreader"].first == count["Spreader"].second, "Spreader: every FL corpus state decodes exactly");
    expect(count["Pitch Shifter"].second >= 15 && count["Pitch Shifter"].first == count["Pitch Shifter"].second,
           "Pitch Shifter: every FL corpus state decodes exactly");
  }
}
}  // namespace
