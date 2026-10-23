// Gridline — 0.5 parity tests: every feature appended to the built-in plugins
// (EQ order / enable / listen, dynamics stages, delay network, reverb decay,
// modulation voices, time-gate slots, synth modulation, sampler loops and
// stretch, drum pads layers, slicer markers, …) measurably does its job.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Effects.h"
#include "engine/EqDesign.h"
#include "engine/Generators.h"
#include "engine/Slicing.h"
#include "engine/TimeGateSlots.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPI = 3.14159265358979323846;

struct Buf {
  std::vector<float> l, r;
  size_t size() const { return l.size(); }
};

PluginState st(const std::string& type, std::initializer_list<std::pair<int, float>> set = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

Buf sine(double hz, size_t n, double amp = 0.5, double hzR = -1) {
  Buf s;
  s.l.resize(n), s.r.resize(n);
  for (size_t i = 0; i < n; ++i) {
    s.l[i] = static_cast<float>(amp * std::sin(2 * kPI * hz * static_cast<double>(i) / kSr));
    s.r[i] = static_cast<float>(amp * std::sin(2 * kPI * (hzR > 0 ? hzR : hz) * static_cast<double>(i) / kSr));
  }
  return s;
}

Buf noise(size_t n, double amp = 0.3, uint32_t seed = 1) {
  Buf s;
  s.l.resize(n), s.r.resize(n);
  uint32_t x = seed;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    s.l[i] = static_cast<float>(amp * (static_cast<int32_t>(x) / 2147483648.0));
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    s.r[i] = static_cast<float>(amp * (static_cast<int32_t>(x) / 2147483648.0));
  }
  return s;
}

ProcessContext ctx(bool playing = false, double tick = 0) {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  c.ppq = 96;
  c.tick = tick;
  c.playing = playing;
  return c;
}

// Runs an effect over a stereo buffer; when playing, the song position advances.
Buf runFx(const PluginState& s, Buf in, int block = 256, bool playing = false) {
  auto fx = createEffect(s.type);
  if (!fx) return in;
  double tick = 0;
  for (size_t p = 0; p < in.size(); p += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - p));
    fx->process(s, ctx(playing, tick), in.l.data() + p, in.r.data() + p, n);
    tick += n / kSr * 2.0 * 96;  // 120 BPM: 2 beats per second
  }
  return in;
}

double rms(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  b = std::min(b, v.size());
  double acc = 0;
  for (size_t i = a; i < b; ++i) acc += static_cast<double>(v[i]) * v[i];
  return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
}
double peak(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  b = std::min(b, v.size());
  double p = 0;
  for (size_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
  return p;
}
double mean(const std::vector<float>& v, size_t a = 0) {
  double acc = 0;
  for (size_t i = a; i < v.size(); ++i) acc += v[i];
  return acc / static_cast<double>(v.size() - a);
}
bool finite(const Buf& s) {
  for (size_t i = 0; i < s.size(); ++i)
    if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
  return true;
}
bool same(const Buf& a, const Buf& b, double tol = 1e-6) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::fabs(a.l[i] - b.l[i]) > tol || std::fabs(a.r[i] - b.r[i]) > tol) return false;
  return true;
}

// Instruments: renders n frames after a note-on (optional note-off at `offAt`).
Buf runGen(const PluginState& s, const ChannelAux& aux, std::vector<std::pair<double, float>> notes, size_t n, int block = 256,
              bool playing = false, size_t offAt = SIZE_MAX) {
  auto g = createGenerator(s.type);
  Buf out;
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  if (!g) return out;
  uint32_t id = 1;
  for (auto [p, v] : notes) g->noteOn(id++, p, v, 0);
  double tick = 0;
  for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
    int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
    if (offAt >= pos && offAt < pos + static_cast<size_t>(k))
      for (uint32_t i = 1; i < id; ++i) g->noteOff(i, static_cast<int>(offAt - pos));
    g->process(s, aux, ctx(playing, tick), out.l.data() + pos, out.r.data() + pos, k);
    tick += k / kSr * 2.0 * 96;
  }
  return out;
}

SamplePtr makeSample(size_t frames, double hz, double amp = 0.5, double dc = 0) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(frames), s->right.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    s->left[i] = static_cast<float>(amp * std::sin(2 * kPI * hz * static_cast<double>(i) / kSr) + dc);
    s->right[i] = s->left[i];
  }
  return s;
}

int zeroCrossings(const std::vector<float>& v, size_t a, size_t b) {
  int z = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i)
    if ((v[i - 1] < 0) != (v[i] < 0)) ++z;
  return z;
}

}  // namespace

int main() {
  const size_t N = 44100;

  // ---------------------------------------------------------------- catalogue
  {
    bool ok = true;
    static const char* builtins[] = {"gl.eq", "gl.filter", "gl.comp", "gl.limiter", "gl.gate", "gl.delay", "gl.reverb", "gl.chorus", "gl.flanger",
                                     "gl.phaser", "gl.dist", "gl.bitcrush", "gl.clipper", "gl.stereo", "gl.transient", "gl.multiband", "gl.analyzer",
                                     "gl.geq", "gl.waveshaper", "gl.autopan", "gl.shaper", "gl.timegate", "gl.maximizer", "gl.freqshift", "gl.lowlift",
                                     "gl.tuner", "gl.utility"};
    for (const char* ty : builtins)
      if (const PluginInfo* pi = findPlugin(ty); pi && pi->params.size() > 99) {
        const PluginInfo& p = *pi;  // slot params must fit the 99-param stride
        ok = false;
        std::printf("# %s has %zu parameters\n", p.type.c_str(), p.params.size());
      }
    expect(ok, "every built-in effect has at most 99 parameters (mixer slot stride)");
    PluginState old;
    old.type = "gl.reverb";
    old.params = {10, 0.6f, 0.9f, 0.4f, 80, 9000, 1, 0.3f, 1, 0.25f};  // a 0.4 project
    normalizePluginState(old);
    expect(old.params.size() == static_cast<size_t>(RvbCount), "old reverb state is padded to the new count");
    expect(std::fabs(old.params[RvbDecayTime] - (0.3 + 0.81 * 7.0 * 0.8)) < 0.01, "old reverb decay is migrated to seconds");
    PluginState eq;
    eq.type = "gl.eq";
    eq.params.assign(static_cast<size_t>(EqMain + 1), 0.f);
    normalizePluginState(eq);
    expect(eq.params[eqExt(2, EqEnabled)] == 1.f && eq.params[eqExt(2, EqOrder)] == EqOrderNormal, "old EQ state gets enabled bands");
    expect(eq.params[EqModel] == 1.f && eq.params[eqFl(2, EqFlOrder)] == EqOrd2, "old EQ state is migrated to the reference band model");
  }

  // ---------------------------------------------------------------- EQ
  {
    PluginState base = st("gl.eq", {{eqParam(3, EqGain), 12.f}, {eqParam(3, EqFreq), 1000.f}, {EqModel, 0.f}});
    EqBandFilters f;
    designEqBand(base, 3, kSr, f);
    double on = eqBandMagnitude(f, 1000, kSr);
    PluginState off = base;
    off.params[eqExt(3, EqEnabled)] = 0;
    designEqBand(off, 3, kSr, f);
    expect(on > 3.5 && f.count == 0, "disabled EQ band has no effect (enabled +12 dB band boosts)");
    PluginState steep = base;
    steep.params[eqExt(3, EqOrder)] = EqOrderSteep4;
    EqBandFilters fs, fn;
    designEqBand(steep, 3, kSr, fs);
    designEqBand(base, 3, kSr, fn);
    double atOct = eqBandMagnitude(fs, 2000, kSr), atOctN = eqBandMagnitude(fn, 2000, kSr);
    expect(fs.count == 4 && std::fabs(dsp::gainToDb(eqBandMagnitude(fs, 1000, kSr)) - 12) < 0.5 && atOct < atOctN,
           "steep order keeps the peak gain and narrows the skirt");
    PluginState cut = st("gl.eq", {{eqParam(0, EqType), EqLowCut}, {eqParam(0, EqFreq), 1000}, {eqParam(0, EqSlope), EqSlope6}, {EqModel, 0.f}});
    designEqBand(cut, 0, kSr, f);
    double g6 = dsp::gainToDb(eqBandMagnitude(f, 250, kSr));
    cut.params[eqParam(0, EqSlope)] = 3;
    designEqBand(cut, 0, kSr, f);
    double g48 = dsp::gainToDb(eqBandMagnitude(f, 250, kSr));
    expect(g6 < -9 && g6 > -15 && g48 < -80, "6 dB/oct cut is gentle, 48 dB/oct is steep (Butterworth)");
    PluginState lis = st("gl.eq", {{EqListen, 4}, {eqParam(3, EqFreq), 1000.f}});  // band 4 at 1 kHz
    Buf low = runFx(lis, sine(80, N));
    Buf mid = runFx(lis, sine(1000, N));
    expect(rms(low.l, N / 2) < 0.05 && rms(mid.l, N / 2) > 0.3, "listen mode passes only the band region");
  }

  // ---------------------------------------------------------------- Compressor
  {
    Buf in = sine(200, N, 0.8);
    Buf dry = runFx(st("gl.comp", {{CmpThreshold, -30.f}, {CmpRatio, 10.f}, {CmpMix, 0.f}, {CmpLimiter, 0.f}}), in);
    expect(same(dry, in, 1e-5), "compressor mix 0 returns the dry signal");
    Buf comp = runFx(st("gl.comp", {{CmpThreshold, -30.f}, {CmpRatio, 10.f}}), in);
    Buf rmsDet = runFx(st("gl.comp", {{CmpThreshold, -30.f}, {CmpRatio, 10.f}, {CmpDetector, 1.f}}), in);
    expect(rms(comp.l, N / 2) < 0.2 && rms(rmsDet.l, N / 2) < 0.25 && finite(rmsDet), "peak and RMS detectors both compress");
    Buf trim = runFx(st("gl.comp", {{CmpOutput, -6.f}, {CmpThreshold, 0.f}, {CmpKnee, 0.f}}), in);
    expect(std::fabs(rms(trim.l, N / 2) / rms(in.l, N / 2) - 0.501) < 0.01, "compressor output trim");
    Buf imp;
    imp.l.assign(4410, 0.f), imp.r.assign(4410, 0.f);
    imp.l[100] = 0.5f;
    Buf la = runFx(st("gl.comp", {{CmpLookahead, 5.f}, {CmpThreshold, 0.f}, {CmpLimiter, 0.f}}), imp);
    size_t at = 0;
    for (size_t i = 0; i < la.size(); ++i)
      if (std::fabs(la.l[i]) > 0.1) {
        at = i;
        break;
      }
    expect(at == 100 + static_cast<size_t>(0.005 * kSr), "compressor look-ahead delays the audio");
    Buf vint = runFx(st("gl.comp", {{CmpThreshold, -30.f}, {CmpRatio, 6.f}, {CmpCharacter, 1.f}}), in);
    expect(finite(vint) && rms(vint.l, N / 2) < rms(in.l), "vintage character compresses");
  }

  // ---------------------------------------------------------------- Limiter
  {
    Buf hot = sine(150, N, 0.9);
    Buf lim = runFx(st("gl.limiter", {{LimGain, 12.f}, {LimCeiling, -1.f}}), hot);
    expect(peak(lim.l) <= dsp::dbToGain(-1.0) + 1e-4, "limiter: look-ahead keeps the ceiling");
    Buf safe = runFx(st("gl.limiter", {{LimGain, 12.f}, {LimCeiling, 0.f}, {LimSafety, 1.f}}), hot);
    expect(peak(safe.l) <= dsp::dbToGain(-0.2) + 1e-4, "limiter safety margin lowers the ceiling by 0.2 dB");
    Buf comp = runFx(st("gl.limiter", {{LimCompThresh, -20.f}, {LimCompRatio, 8.f}}), sine(150, N, 0.5));
    expect(rms(comp.l, N / 2) < 0.2, "limiter's RMS compressor stage reduces the level");
    Buf quiet = sine(150, N, 0.003);
    Buf gated = runFx(st("gl.limiter", {{LimGateThresh, -30.f}, {LimGateFloor, -60.f}, {LimGateRelease, 0.02f}}), quiet);
    expect(rms(gated.l, N / 2) < rms(quiet.l, N / 2) * 0.01, "limiter's gate closes on quiet input");
    Buf sat = runFx(st("gl.limiter", {{LimSaturation, -12.f}, {LimCeiling, 0.f}}), sine(150, N, 0.9));
    expect(finite(sat) && peak(sat.l) < 0.95, "limiter saturation rounds the peaks");
  }

  // ---------------------------------------------------------------- Gate
  {
    Buf loud = sine(300, N, 0.5);
    Buf duck = runFx(st("gl.gate", {{GtThreshold, -30.f}, {GtFlip, 1.f}, {GtRange, -60.f}}), loud);
    expect(rms(duck.l, N / 2) < 0.01, "gate duck mode attenuates loud input");
    Buf keyed = runFx(st("gl.gate", {{GtThreshold, -20.f}, {GtKeyHpf, 2000.f}}), sine(100, N, 0.5));
    expect(rms(keyed.l, N / 2) < 0.01, "gate key high-pass ignores bass");
  }

  // ---------------------------------------------------------------- Multiband / Maximizer / Transient / Clipper
  {
    Buf in = sine(60, N, 0.5);
    Buf muted = runFx(st("gl.multiband", {{MbLowState, 2.f}}), in);
    expect(rms(muted.l, N / 2) < 0.05, "multiband: muting the low band removes bass");
    Buf lim = runFx(st("gl.multiband", {{MbLimiter, 1.f}, {MbOutput, 18.f}, {MbLowThresh, 0.f}}), in);
    expect(peak(lim.l, N / 4) <= 1.0, "multiband output limiter holds 0 dBFS");
    Buf amt0 = runFx(st("gl.maximizer", {{MxAmount, 0.f}}), in);
    expect(same(amt0, in, 1e-6), "maximizer amount 0 returns the dry signal");
    Buf band = runFx(st("gl.maximizer", {{MxLowThresh, -30.f}, {MxLowRatio, 10.f}, {MxDrive, 0.f}}), in);
    Buf noComp = runFx(st("gl.maximizer", {{MxDrive, 0.f}}), in);
    expect(rms(band.l, N / 2) < rms(noComp.l, N / 2) * 0.7, "maximizer per-band compressor reduces its band");
    Buf solo = runFx(st("gl.maximizer", {{MxSolo, 3.f}, {MxDrive, 0.f}}), in);
    expect(rms(solo.l, N / 2) < 0.05, "maximizer solo high band removes the bass");
    Buf tm = runFx(st("gl.transient", {{TrMix, 0.f}}), noise(N));
    expect(same(tm, noise(N), 1e-6), "transient shaper mix 0 returns the dry signal");
    Buf ts = runFx(st("gl.transient", {{TrSplit, 1000.f}, {TrBalance, 1.f}, {TrDrive, 0.5f}, {TrAttackSpeed, 0.f}}), noise(N));
    expect(finite(ts) && rms(ts.l) > 0.05, "transient shaper split/drive produce sound");
    Buf full = runFx(st("gl.clipper", {{ClpThreshold, -6.f}, {ClpCeilingMode, 1.f}, {ClpInput, 12.f}, {ClpOversample, 2.f}}), sine(200, N, 0.9));
    expect(finite(full) && peak(full.l) <= 1.001 && peak(full.l) > 0.9, "soft clipper reaches toward 0 dBFS, oversampled");
  }

  // ---------------------------------------------------------------- Graphic EQ / Distortion / Bitcrusher / Waveshaper
  {
    Buf in = noise(N);
    Buf g0 = runFx(st("gl.geq", {{0, 12.f}, {GeqMix, 0.f}}), in);
    expect(same(g0, in, 1e-6), "graphic EQ mix 0 is transparent");
    Buf gc = runFx(st("gl.geq", {{1, 12.f}}), sine(250, N));
    Buf gcc = runFx(st("gl.geq", {{1, 12.f}, {GeqLayout, 1.f}}), sine(250, N));
    expect(rms(gcc.l, N / 2) > rms(gc.l, N / 2) * 1.3, "classic layout centres band 2 at 250 Hz");
    Buf hard = runFx(st("gl.dist", {{DstMode, 1.f}, {DstThreshold, -12.f}, {DstOutput, 0.f}, {DstTone, 20000.f}}), sine(100, N, 0.9));
    expect(peak(hard.l, N / 4) < dsp::dbToGain(-11.5), "distortion threshold limits the hard clipper");
    Buf col = runFx(st("gl.dist", {{DstColor, 1.f}, {DstDcRemove, 1.f}, {DstOversample, 2.f}}), sine(100, N, 0.5));
    expect(finite(col) && std::fabs(mean(col.l, N / 2)) < 0.01, "distortion colour bias is DC-free");
    for (int m = 4; m <= 7; ++m) {
      Buf d = runFx(st("gl.dist", {{DstMode, static_cast<float>(m)}, {DstExtreme, 1.f}, {DstSoftOut, 1.f}}), sine(100, 8000, 0.5));
      expect(finite(d) && peak(d.l) <= 1.0, ("distortion mode " + std::to_string(m) + " with x100 drive and soft output").c_str());
    }
    Buf bc = runFx(st("gl.bitcrush", {{BcBits, 24.f}, {BcRate, static_cast<float>(kBcRateOff)}, {BcFilterOn, 1.f}, {BcFilterFreq, 300.f}}), sine(5000, N));
    expect(rms(bc.l, N / 2) < 0.05, "bitcrusher filter (after) removes highs");
    Buf bo = runFx(st("gl.bitcrush", {{BcOutput, -6.f}, {BcBits, 24.f}, {BcRate, static_cast<float>(kBcRateOff)}}), sine(200, N));
    expect(std::fabs(rms(bo.l, N / 2) / rms(sine(200, N).l, N / 2) - 0.501) < 0.02, "bitcrusher output gain");
    std::initializer_list<std::pair<int, float>> asym = {{WsBipolar, 0.f}, {0, 0.f}, {1, 0.f}, {2, 0.f}, {3, 0.f}, {4, 0.f}, {5, 0.f}, {6, 0.f}, {7, 0.f}, {WsCenter, 0.f}};
    Buf w = runFx(st("gl.waveshaper", asym), sine(100, N, 0.5));
    expect(mean(w.l, N / 2) > 0.02, "waveshaper with Remove DC off keeps an asymmetric curve's offset");
    PluginState steps = st("gl.waveshaper", {{WsInterp, 2.f}});
    expect(std::fabs(waveshaperCurve(steps, 0.06) - waveshaperCurve(steps, 0.0)) < 1e-9, "waveshaper step curve holds between points");
  }

  // ---------------------------------------------------------------- Buf / Utility
  {
    Buf in = sine(300, N, 0.5, 700);
    Buf sw = runFx(st("gl.stereo", {{StClassic, 0.f}, {StLL, 0.f}, {StRR, 0.f}, {StRL, 1.f}, {StLR, 1.f}}), in);
    Buf ref = in;
    std::swap(ref.l, ref.r);
    expect(same(sw, ref, 1e-5), "stereo matrix swaps the channels");
    Buf inv = runFx(st("gl.stereo", {{StClassic, 0.f}, {StInvert, 1.f}}), in);
    expect(std::fabs(inv.l[1000] + in.l[1000]) < 1e-5 && std::fabs(inv.r[1000] - in.r[1000]) < 1e-5, "stereo invert left only");
    Buf wide = sine(60, N, 0.5, 60);
    for (size_t i = 0; i < wide.size(); ++i) wide.r[i] = -wide.r[i];  // pure side signal at 60 Hz
    Buf mono = runFx(st("gl.stereo", {{StClassic, 0.f}, {StMonoBelow, 300.f}}), wide);
    expect(rms(mono.l, N / 2) < 0.05, "stereo 'mono below' removes low side content");
    Buf dep = runFx(st("gl.stereo", {{StClassic, 0.f}, {StDephase, 0.5f}, {StFxPos, 0.f}}), in);
    expect(finite(dep) && std::fabs(rms(dep.r, N / 2) - rms(in.r, N / 2)) < 0.02, "dephase keeps the level (all-pass)");
    Buf mt = runFx(st("gl.utility", {{UtlMute, 1.f}, {UtlMuteChannel, 1.f}}), in);
    expect(rms(mt.l, N / 2) < 1e-6 && rms(mt.r, N / 2) > 0.3, "utility mutes the left channel only");
    Buf il = runFx(st("gl.utility", {{UtlInvert, 1.f}, {UtlInvertMode, 2.f}}), in);
    expect(std::fabs(il.r[500] + in.r[500]) < 1e-5 && std::fabs(il.l[500] - in.l[500]) < 1e-5, "utility inverts the right channel only");
    Buf mo = runFx(st("gl.utility", {{UtlMono, 1.f}}), in);
    expect(std::fabs(mo.l[1234] - mo.r[1234]) < 1e-6, "utility mono switch");
    Buf dc = sine(300, N, 0.3);
    for (auto& v : dc.l) v += 0.3f;
    Buf dcr = runFx(st("gl.utility", {{UtlDcRemove, 1.f}}), dc);
    expect(std::fabs(mean(dcr.l, N / 2)) < 0.01, "utility removes DC");
    Buf cp = runFx(st("gl.utility", {{UtlPan, -1.f}, {UtlPanLaw, 1.f}}), in);
    expect(rms(cp.r) < 1e-6 && rms(cp.l) > 0.3, "utility constant-power pan hard left");
  }

  // ---------------------------------------------------------------- Shaper / Low Lifter
  {
    std::initializer_list<std::pair<int, float>> left = {{ShPan + 0, -1.f}, {ShPan + 1, -1.f}, {ShPan + 2, -1.f}, {ShPan + 3, -1.f}, {ShPan + 4, -1.f},
                                                          {ShPan + 5, -1.f}, {ShPan + 6, -1.f}, {ShPan + 7, -1.f}, {ShPan + 8, -1.f}, {ShPan + 9, -1.f},
                                                          {ShPan + 10, -1.f}, {ShPan + 11, -1.f}, {ShPan + 12, -1.f}, {ShPan + 13, -1.f}, {ShPan + 14, -1.f},
                                                          {ShPan + 15, -1.f}};
    Buf pl = runFx(st("gl.shaper", left), sine(300, N));
    expect(rms(pl.r, N / 2) < 1e-6 && rms(pl.l, N / 2) > 0.3, "shaper pan lane moves the sound hard left");
    Buf sh = runFx(st("gl.shaper", {{ShFilter, 5.f}, {ShShelfGain, 12.f}, {ShCutMin, 1000.f}, {ShCutMax, 1000.f}, {ShSlope, 1.f},
                                       {ShDrive, 12.f}, {ShDriveMix, 0.5f}, {ShFollow, 0.5f}, {ShLfoAmt, 0.5f}, {ShTension, 0.5f}}),
                      sine(100, N, 0.2), 256, true);
    expect(finite(sh) && rms(sh.l, N / 2) > 0.2, "shaper shelf filter, drive, follower and LFO run");
    Buf ap = runFx(st("gl.shaper", {{ShFilter, 8.f}}), sine(300, N));
    expect(std::fabs(rms(ap.l, N / 2) - rms(sine(300, N).l, N / 2)) < 0.02, "shaper all-pass keeps the level");
    Buf ll = runFx(st("gl.lowlift", {{LlModel, 0.f}, {LlLowOn, 0.f}}), sine(50, N));  // legacy engine (J-26 reference: test_j26)
    expect(rms(ll.l, N / 2) < 0.05, "low lifter low band switch removes bass");
    Buf lb = runFx(st("gl.lowlift", {{LlModel, 0.f}, {LlBoost, 1.f}, {LlBoostFreq, 60.f}, {LlLowGain, 0.f}, {LlHarmonics, 0.f}}), sine(30, N, 0.1));
    expect(rms(lb.l, N / 2) > 0.2, "low lifter bass boost");
    Buf lh = runFx(st("gl.lowlift", {{LlModel, 0.f}, {LlDiffuse, 1.f}, {LlSlope, 1.f}, {LlInvertHarm, 1.f}, {LlDrive, 12.f}, {LlMultiband, 0.f}}), sine(60, N));
    expect(finite(lh), "low lifter diffuser, slope, inverted harmonics, full-band");
  }

  // ---------------------------------------------------------------- Time Gate slots
  {
    auto b = std::make_unique<tg::Bank>();
    tg::factory(*b);
    expect(b->time[3].env.n > 4 && b->vol[0].env.n > 4 && b->time[0].name[0], "time gate factory slots exist");
    auto custom = std::make_unique<tg::Slot>();
    custom->len = 2, custom->setName("Mine");
    custom->env.add(0, 1.f), custom->env.add(1, 0.75f, 0.3f), custom->env.add(2, 0.5f);
    std::string data = tg::replaceSlot("", 'T', 4, *custom);
    auto b2 = std::make_unique<tg::Bank>();
    tg::factory(*b2);
    tg::parse(data.c_str(), *b2);
    expect(b2->time[4].env.n == 3 && b2->time[4].len == 2 && std::fabs(b2->time[4].env.y[1] - 0.75f) < 1e-6 &&
               std::string(b2->time[4].name) == "Mine",
           "time gate slot serialize / parse round-trip");
    expect(std::fabs(b->time[0].eval(2.0) - tg::timeY(1.0)) < 1e-6, "half-speed slot is one beat behind at mid-bar");
    // Repeat beat 1: during beat 2 the output equals the input from beat 1.
    size_t spb = static_cast<size_t>(kSr * 0.5);
    Buf in = noise(spb * 4);
    Buf rep = runFx(st("gl.timegate", {{TgTimeSlot, 4.f}, {TgDeclick, 0.f}}), in, 256, true);
    double err = 0;
    for (size_t i = 1000; i < 5000; ++i) err = std::max(err, static_cast<double>(std::fabs(rep.l[spb + i] - in.l[i])));
    expect(err < 1e-5, "time slot 'repeat beat 1' replays the first beat");
    Buf gate = runFx(st("gl.timegate", {{TgVolSlot, 1.f}, {TgVolAttack, 0.0005f}, {TgVolRelease, 0.0005f}}), sine(300, spb * 4), 256, true);
    size_t q = spb / 4;  // 1/16 note
    expect(rms(gate.l, q / 4, q / 2) > 0.2 && rms(gate.l, q + q / 4, q + q / 2) < 0.02, "volume slot 'gate 1/8' gates 16ths");
    Buf hold = runFx(st("gl.timegate", {{TgHold, 1.f}, {TgTimeSlot, 1.f}}), noise(spb * 2), 256, true);
    expect(finite(hold), "time gate hold freezes the buffer");
    Buf fr = runFx(st("gl.timegate", {{TgVolSlot, 1.f}, {TgFreeRun, 1.f}, {TgVolAttack, 0.0005f}, {TgVolRelease, 0.0005f}}), sine(300, spb * 2), 256, false);
    expect(rms(fr.l, q + q / 4, q + q / 2) < 0.05, "time gate runs its envelopes when stopped (free run)");
  }

  // ---------------------------------------------------------------- Filter / Delay / Reverb
  {
    Buf f12 = runFx(st("gl.filter", {{FltCutoff, 500.f}}), sine(2000, N));
    Buf f24 = runFx(st("gl.filter", {{FltCutoff, 500.f}, {FltSlope, 1.f}}), sine(2000, N));
    expect(rms(f24.l, N / 2) < rms(f12.l, N / 2) * 0.3, "filter 24 dB slope is steeper");
    Buf shelf = runFx(st("gl.filter", {{FltMode, 4.f}, {FltCutoff, 500.f}, {FltGain, 12.f}}), sine(80, N, 0.1));
    expect(rms(shelf.l, N / 2) > 0.25, "filter low shelf boosts");
    Buf blend = runFx(st("gl.filter", {{FltBlend, 1.f}, {FltLPLevel, 0.f}, {FltHPLevel, 1.f}, {FltCutoff, 2000.f}}), sine(100, N));
    expect(rms(blend.l, N / 2) < 0.05, "filter multi-mode blend (high-pass level only)");

    Buf impulse;
    impulse.l.assign(N, 0.f), impulse.r.assign(N, 0.f);
    impulse.l[10] = impulse.r[10] = 1.f;
    Buf off = runFx(st("gl.delay", {{DlyModel, 0.f}, {DlyMode, 4.f}, {DlyDry, 0.f}}), impulse);
    expect(peak(off.l, 2000) < 1e-4, "delay 'off' mode has no echoes");
    Buf lofi = runFx(st("gl.delay", {{DlyModel, 0.f}, {DlyBits, 4.f}, {DlyRateReduce, 4000.f}, {DlyDistType, 1.f}, {DlyDistThresh, -12.f}, {DlyFeedback, 1.2f},
                                        {DlyDiffusion, 0.8f}, {DlyFilterType, 1.f}, {DlyResonance, 0.8f}}),
                         noise(N, 0.3));
    expect(finite(lofi) && peak(lofi.l) < 4.0, "delay with 120% feedback stays bounded by the loop limiter");
    Buf spread0 = runFx(st("gl.delay", {{DlyModel, 0.f}, {DlyStereoSpread, 0.f}, {DlyDry, 0.f}, {DlyMode, 0.f}}), sine(300, N, 0.5, 900));
    expect(std::fabs(spread0.l[30000] - spread0.r[30000]) < 1e-5, "delay stereo spread 0 is mono");
    Buf kp = runFx(st("gl.delay", {{DlyKeepPitch, 1.f}, {DlySync, 0.f}}), noise(N));
    expect(finite(kp), "delay keep-pitch crossfade runs");
    Buf tone = runFx(st("gl.delay", {{DlyTone, -1.f}, {DlyDry, 0.f}, {DlyFeedback, 0.f}}), sine(8000, N));
    expect(rms(tone.l, N / 2) < 0.05, "delay output tone low-passes the wet signal");

    Buf burst = noise(N * 3);
    for (size_t i = 4410; i < burst.size(); ++i) burst.l[i] = burst.r[i] = 0;
    PluginState shortR = st("gl.reverb", {{RvbDecayTime, 0.5f}, {RvbDry, 0.f}});
    PluginState longR = st("gl.reverb", {{RvbDecayTime, 6.f}, {RvbDry, 0.f}});
    Buf rs = runFx(shortR, burst), rl = runFx(longR, burst);
    expect(rms(rl.l, N, 2 * N) > rms(rs.l, N, 2 * N) * 10, "reverb decay time in seconds controls the tail");
    Buf rf = burst;
    {
      auto fx = createEffect("gl.reverb");
      PluginState a = st("gl.reverb", {{RvbDry, 0.f}, {RvbDecayTime, 0.5f}}), f = a;
      f.params[RvbFreeze] = 1;
      for (size_t p = 0; p < rf.size(); p += 256) {
        int n = static_cast<int>(std::min<size_t>(256, rf.size() - p));
        fx->process(p < 6000 ? a : f, ctx(), rf.l.data() + p, rf.r.data() + p, n);
      }
    }
    expect(rms(rf.l, 2 * N, 3 * N) > rms(rs.l, 2 * N, 3 * N) * 10 && finite(rf), "reverb freeze sustains the tail");
    Buf rb = runFx(st("gl.reverb", {{RvbBassMult, 3.f}, {RvbModDepth, 1.f}, {RvbInputMode, 1.f}, {RvbColor, 0.5f}, {RvbDiffusion, 0.f}, {RvbPreSync, 1.f}}), burst);
    expect(finite(rb) && rms(rb.l, N, 2 * N) > 0, "reverb bass multiplier, modulation, stereo input, colour");
    Buf blocks1 = runFx(longR, noise(8000), 1), blocks2 = runFx(longR, noise(8000), 777);
    expect(same(blocks1, blocks2, 1e-6), "reverb is block-size invariant");
  }

  // ---------------------------------------------------------------- Modulation effects
  {
    Buf in = sine(440, N, 0.5);
    Buf c3 = runFx(st("gl.chorus", {{ChoClassic, 0.f}, {ChoVoices, 2.f}, {ChoShape, 0.5f}, {ChoFeedback, 0.5f}}), in);
    expect(finite(c3) && rms(c3.l, N / 2) > 0.1, "chorus with three voices");
    Buf wo = runFx(st("gl.chorus", {{ChoClassic, 0.f}, {ChoCrossMode, 0.f}, {ChoDelay, 12.f}, {ChoWetOnly, 1.f}, {ChoDepth, 0.f}}), in);
    expect(std::fabs(wo.l[N / 2] - in.l[N / 2 - static_cast<size_t>(0.012 * kSr)]) < 0.02, "chorus wet only = delayed copy");
    Buf cx = runFx(st("gl.chorus", {{ChoClassic, 0.f}, {ChoCrossMode, 1.f}, {ChoCrossFreq, 2000.f}, {ChoWetOnly, 1.f}}), sine(100, N));
    expect(rms(cx.l, N / 2) > 0.3, "chorus crossover keeps the low band dry");
    Buf fl = runFx(st("gl.flanger", {{FlgInvertWet, 1.f}, {FlgCross, 0.5f}, {FlgDamp, 2000.f}, {FlgShape, 1.f}, {FlgPhase, 180.f}}), in);
    expect(finite(fl), "flanger inverted wet, cross feedback, damping");
    Buf ph = runFx(st("gl.phaser", {{PhsRangeMode, 1.f}, {PhsStages, 8.f}, {PhsOutGain, -6.f}, {PhsShape, 1.f}}), noise(N));
    expect(finite(ph) && rms(ph.l) > 0.01, "phaser min/max sweep with 24 stages");
    Buf ap = runFx(st("gl.autopan", {{ApClassic, 0.f}, {ApStaticPan, -1.f}, {ApPan, 0.f}}), in);
    expect(rms(ap.r, N / 2) < 1e-6, "auto pan static pan hard left");
    Buf vol = runFx(st("gl.autopan", {{ApClassic, 0.f}, {ApStaticVol, 0.5f}, {ApPan, 0.f}}), in);
    expect(std::fabs(rms(vol.l, N / 2) - 0.5 * rms(in.l, N / 2)) < 0.01, "auto pan static volume");
    Buf ring = runFx(st("gl.freqshift", {{FsType, 2.f}, {FsShift, 100.f}, {FsMix, 1.f}}), sine(1000, N));
    expect(finite(ring) && rms(ring.l, N / 2) > 0.15, "frequency shifter ring-mod type");
    Buf hq = runFx(st("gl.freqshift", {{FsType, 1.f}, {FsRange, 1.f}, {FsShiftWide, 500.f}, {FsMix, 1.f}, {FsShapeL, 0.5f}}), sine(1000, N));
    expect(finite(hq) && rms(hq.l, N / 2) > 0.1, "frequency shifter HQ, wide range, carrier shape");
  }

  // ---------------------------------------------------------------- Trio Synth
  {
    ChannelAux aux;
    PluginState base = st("gl.synth", {{SynOsc2Level, 0.f}, {SynOsc3Level, 0.f}, {SynCutoff, 20000.f}});
    Buf plain = runGen(base, aux, {{60, 1.f}}, N / 2);
    PluginState am = base;
    am.params[SynOsc3Level] = 1.f, am.params[SynOsc3Mode] = SynModeAM, am.params[SynOsc3Coarse] = -24, am.params[SynOsc3Shape] = 0;
    Buf amO = runGen(am, aux, {{60, 1.f}}, N / 2);
    expect(rms(amO.l) > 0.01 && std::fabs(rms(amO.l) - rms(plain.l)) > 0.002 && !same(amO, plain), "osc 3 as AM modulates oscs 1+2 (and is not heard alone)");
    PluginState pan = base;
    pan.params[SynOsc1Pan] = -1.f;
    Buf pO = runGen(pan, aux, {{60, 1.f}}, N / 4);
    expect(rms(pO.r) < 1e-6 && rms(pO.l) > 0.01, "osc pan hard left");
    PluginState det = base;
    det.params[SynOsc1Detune] = 30.f;
    Buf dO = runGen(det, aux, {{60, 1.f}}, N / 2);
    expect(!same(Buf{dO.l, dO.l}, Buf{dO.r, dO.r}) && rms(dO.r) > 0.01, "stereo detune makes the sides differ");
    PluginState oct = base;
    oct.params[SynOsc1Coarse] = 12.f, oct.params[SynOsc1Shape] = 0;
    PluginState low = base;
    low.params[SynOsc1Shape] = 0;
    Buf hi = runGen(oct, aux, {{60, 1.f}}, N / 2), lo = runGen(low, aux, {{60, 1.f}}, N / 2);
    int zh = zeroCrossings(hi.l, 2000, 20000), zl = zeroCrossings(lo.l, 2000, 20000);
    expect(zh > zl * 1.8 && zh < zl * 2.2, "osc 1 coarse +12 doubles the pitch");
    for (int mode = 2; mode <= 4; ++mode) {
      PluginState m = base;
      m.params[SynOsc3Level] = 0.6f, m.params[SynOsc3Mode] = static_cast<float>(mode);
      m.params[SynOsc2Level] = 0.5f, m.params[SynOsc2Mod] = static_cast<float>(mode - 1);
      m.params[SynFilterType] = static_cast<float>(mode), m.params[SynLfoTarget] = static_cast<float>(mode - 1), m.params[SynLfoAmount] = 0.5f;
      m.params[SynOsc1Shape] = 5, m.params[SynOsc2Shape] = 6, m.params[SynPhaseRand] = 0.5f;
      Buf o = runGen(m, aux, {{60, 1.f}, {64, 0.7f}}, N / 2);
      expect(finite(o) && rms(o.l) > 1e-3, ("synth modulation/filter/LFO combination " + std::to_string(mode)).c_str());
    }
    PluginState inv = base;
    inv.params[SynOsc2Level] = 0.8f, inv.params[SynOsc2Shape] = 2, inv.params[SynOsc1Shape] = 2, inv.params[SynOsc2Invert] = 1;
    inv.params[SynOsc1Level] = 0.8f, inv.params[SynOsc2Coarse] = 0, inv.params[SynOsc2Fine] = 0;
    Buf io = runGen(inv, aux, {{60, 1.f}}, N / 4);
    expect(rms(io.l) < 1e-4, "inverting an identical osc 2 cancels osc 1");
    Buf b1 = runGen(am, aux, {{60, 1.f}}, 8000, 1), b2 = runGen(am, aux, {{60, 1.f}}, 8000, 999);
    expect(same(b1, b2, 1e-6), "synth AM render is block-size invariant");
  }

  // ---------------------------------------------------------------- Sampler
  {
    ChannelAux aux;
    aux.samples.push_back(makeSample(4410, 441, 0.25, 0.1));
    PluginState one = st("gl.sampler");
    Buf o = runGen(one, aux, {{60, 1.f}}, N / 2);
    expect(rms(o.l, 6000) < 1e-6, "one-shot sample ends");
    PluginState lp = st("gl.sampler", {{SmpLoop, 1.f}, {SmpOneShot, 0.f}, {SmpLoopMode, 1.f}, {SmpLoopStart, 0.3f}, {SmpLoopEnd, 0.7f}});
    Buf l = runGen(lp, aux, {{60, 1.f}}, N / 2);
    expect(rms(l.l, N / 4) > 0.05, "ping-pong loop keeps sounding");
    PluginState norm = st("gl.sampler", {{SmpNormalize, 1.f}, {SmpRemoveDc, 1.f}, {SmpVolume, 1.f}});
    Buf n2 = runGen(norm, aux, {{60, 1.f}}, 4000, 256);
    expect(peak(n2.l, 500, 4000) > 0.6 && std::fabs(mean(n2.l, 500)) < 0.03, "normalize and remove DC (render-time)");
    PluginState str = st("gl.sampler", {{SmpStretchMode, 1.f}, {SmpTimeStretch, 2.f}, {SmpDeclick, 0.f}});
    Buf s1 = runGen(str, aux, {{60, 1.f}}, N / 2);
    size_t last = 0;
    for (size_t i = 0; i < s1.size(); ++i)
      if (std::fabs(s1.l[i]) > 1e-4) last = i;
    expect(last > 8000 && last < 9500, "time stretch x2 doubles the length");
    PluginState flt = st("gl.sampler", {{SmpFilterType, 1.f}, {SmpCutoff, 100.f}});
    Buf fo = runGen(flt, aux, {{60, 1.f}}, 4000);
    expect(rms(fo.l, 500, 4000) < rms(o.l, 500, 4000) * 0.5, "sampler filter");
    PluginState pol = st("gl.sampler", {{SmpPolarity, 1.f}, {SmpPan, 1.f}, {SmpLfoTarget, 4.f}, {SmpLfoAmount, 0.2f}, {SmpEnvDelay, 0.01f}, {SmpEnvHold, 0.05f}});
    Buf po = runGen(pol, aux, {{60, 1.f}}, 6000);
    expect(rms(po.l) < 1e-6 && rms(po.r) > 0.01 && rms(po.r, 0, 400) < 1e-6, "sampler pan, delayed envelope, LFO");
  }

  // ---------------------------------------------------------------- Drums / Pluck
  {
    ChannelAux aux;
    for (const char* type : {"gl.kick", "gl.snare", "gl.hat", "gl.clap"}) {
      const PluginInfo* info = findPlugin(type);
      PluginState s = defaultPluginState(type);
      for (size_t k = 0; k < info->params.size(); ++k)
        if (info->params[k].kind == ParamKind::Continuous) s.params[k] = static_cast<float>(info->params[k].fromNormalized(0.7));
      Buf o = runGen(s, aux, {{60, 1.f}}, N / 2);
      expect(finite(o) && rms(o.l) > 1e-4, (std::string(type) + " with every new control up").c_str());
    }
    Buf kp = runGen(st("gl.kick", {{KickSub, 1.f}}), aux, {{60, 1.f}}, N / 2);
    Buf k0 = runGen(st("gl.kick"), aux, {{60, 1.f}}, N / 2);
    expect(rms(kp.l) > rms(k0.l), "kick sub layer adds energy");
    Buf hc = runGen(st("gl.hat", {{HatChoke, 0.f}, {HatOpen, 1.f}}), aux, {{60, 1.f}, {60, 1.f}}, N / 4);
    expect(rms(hc.l) > 0, "hats without choke ring together");
    Buf cp = runGen(st("gl.clap", {{ClapStereo, 1.f}}), aux, {{60, 1.f}}, N / 4);
    expect(!same(Buf{cp.l, cp.l}, Buf{cp.r, cp.r}), "clap stereo spread");
    Buf sp = runGen(st("gl.snare", {{SnPan, -1.f}}), aux, {{60, 1.f}}, N / 4);
    expect(rms(sp.r) < 1e-6, "snare pan");
    PluginState pk = st("gl.pluck", {{PlkNormalize, 0.f}, {PlkDecay, 2.f}});
    Buf hiN = runGen(pk, aux, {{84, 1.f}}, N), loN = runGen(pk, aux, {{48, 1.f}}, N);
    expect(rms(hiN.l, N / 2) / std::max(1e-9, rms(hiN.l, 0, 4000)) < rms(loN.l, N / 2) / std::max(1e-9, rms(loN.l, 0, 4000)),
           "pluck without normalisation: high notes die faster");
    Buf gate = runGen(st("gl.pluck", {{PlkGate, 0.f}}), aux, {{60, 1.f}}, N, 256, false, 2000);
    Buf gated = runGen(st("gl.pluck"), aux, {{60, 1.f}}, N, 256, false, 2000);
    expect(rms(gate.l, N / 4, N / 2) > rms(gated.l, N / 4, N / 2) * 3, "pluck gate off rings past note-off");
    Buf pb1 = runGen(st("gl.pluck", {{PlkDecay, 0.5f}}), aux, {{72, 1.f}}, 2 * N, 64), pb2 = runGen(st("gl.pluck", {{PlkDecay, 0.5f}}), aux, {{72, 1.f}}, 2 * N, 1000);
    expect(same(pb1, pb2, 0), "pluck is block-size invariant (voice end on its own clock)");
  }

  // ---------------------------------------------------------------- Drum Pads
  {
    std::string data = setPadSamplePath("", 0, 1, "/tmp/layer2.wav");
    expect(padSamplePath(data, 0, 1) == "/tmp/layer2.wav" && padSamplePath(data, 0, 0).empty() && padSamplePath(data, 3, 2) == "none",
           "pad layer paths round-trip in the data text");
    ChannelAux aux;
    aux.samples.assign(static_cast<size_t>(kPads + kPads * (kPadLayers - 1)), nullptr);
    aux.samples[0] = makeSample(8000, 200, 0.2);
    aux.samples[static_cast<size_t>(padLayerSample(0, 1))] = makeSample(8000, 200, 0.8);
    PluginState s = st("gl.pads", {{padLayer(0, 0, LayVelHigh), 80.f}, {padLayer(0, 1, LayEnabled), 1.f}, {padLayer(0, 1, LayVelLow), 81.f},
                                   {padExt(0, PadVelScale), 0.f}});
    Buf soft = runGen(s, aux, {{60, 0.3f}}, 4000), hard = runGen(s, aux, {{60, 1.f}}, 4000);
    expect(rms(hard.l) > rms(soft.l) * 3, "velocity picks the louder layer");
    PluginState st2 = s;
    st2.params[padLayer(0, 1, LayVelLow)] = 0;
    st2.params[padLayer(0, 0, LayVelHigh)] = 127;
    st2.params[padExt(0, PadLayerMode)] = LayerCycle;
    auto g = createGenerator("gl.pads");
    Buf a;
    a.l.assign(20000, 0.f), a.r.assign(20000, 0.f);
    g->noteOn(1, 60, 1.f, 0);
    g->process(st2, aux, ctx(), a.l.data(), a.r.data(), 9000);
    double r1 = rms(a.l, 0, 2000);
    g->noteOn(2, 60, 1.f, 0);
    g->process(st2, aux, ctx(), a.l.data() + 9000, a.r.data() + 9000, 11000);
    double r2 = rms(a.l, 9000, 11000);
    expect(r1 > 0 && r2 > 0 && (r2 > r1 * 1.5 || r1 > r2 * 1.5), "cycle mode alternates layers");
    PluginState mapped = st("gl.pads", {{padExt(5, PadNote), 36.f}});
    ChannelAux kit;
    kit.samples.assign(static_cast<size_t>(kPads), nullptr);
    kit.samples[5] = makeSample(4000, 300, 0.5);
    expect(rms(runGen(mapped, kit, {{36, 1.f}}, 3000).l) > 0.01 && rms(runGen(mapped, kit, {{65, 1.f}}, 3000).l) < 1e-9, "pad note map");
    PluginState muted = st("gl.pads", {{padExt(5, PadMute), 1.f}});
    expect(rms(runGen(muted, kit, {{65, 1.f}}, 3000).l) < 1e-9, "pad mute");
    PluginState env = st("gl.pads", {{padExt(5, PadOneShot), 0.f}, {padExt(5, PadRelease), 0.005f}});
    Buf eo = runGen(env, kit, {{65, 1.f}}, 3900, 256, false, 500);
    expect(rms(eo.l, 1500, 3900) < 1e-4, "pad envelope releases on note-off when not one-shot");
    ChannelAux prepared;
    PluginState ps = st("gl.pads");
    ps.data = setPadSamplePath("", 1, 0, "none");
    expect(prepareBuiltinAux(ps, prepared) && prepared.samples.size() == static_cast<size_t>(kPadSampleSlots) && !prepared.samples[1] &&
               prepared.samples[0],
           "prepareBuiltinAux loads pads (built-in kit) and layer slots");
  }

  // ---------------------------------------------------------------- FM / Acid / Keys
  {
    ChannelAux aux;
    PluginState carrierOnly = st("gl.fm", {{FmAlgorithm, 7.f}, {fmParam(1, FmLevel), 0.f}, {fmParam(2, FmLevel), 0.f}, {fmParam(3, FmLevel), 0.f}});
    PluginState up = carrierOnly;
    up.params[FmOctave] = 1;
    Buf a = runGen(carrierOnly, aux, {{60, 1.f}}, N / 2), b = runGen(up, aux, {{60, 1.f}}, N / 2);
    int za = zeroCrossings(a.l, 1000, 20000), zb = zeroCrossings(b.l, 1000, 20000);
    expect(zb > za * 1.8 && zb < za * 2.2, "FM octave transposes");
    PluginState thru = st("gl.fm", {{FmAlgorithm, 0.f}, {fmExt(3, FmThru), 1.f}, {fmExt(1, FmWave), 0.8f}, {FmVibrato, 50.f}, {fmExt(0, FmPan), 1.f}});
    Buf t = runGen(thru, aux, {{60, 1.f}}, N / 2);
    expect(finite(t) && rms(t.r) > rms(t.l) * 5, "FM operator thru, waveform, vibrato and carrier pan");
    PluginState seq = st("gl.acid", {{AcSeqOn, 1.f}});
    Buf s = runGen(seq, aux, {}, N, 256, true);
    expect(rms(s.l) > 0.01, "acid bass sequencer plays with the transport");
    Buf stopped = runGen(seq, aux, {}, N / 2, 256, false);
    expect(rms(stopped.l) < 1e-6, "acid sequencer is silent when stopped");
    Buf pw = runGen(st("gl.acid", {{AcBlend, 1.f}, {AcPwmDepth, 0.3f}, {AcVintage, 0.f}, {AcClicks, 0.5f}, {AcHighPass, 200.f}, {AcKeyFollow, 1.f}}), aux,
                       {{48, 1.f}}, N / 2);
    expect(finite(pw) && rms(pw.l) > 0.01, "acid pulse width modulation, clicks, high-pass, key follow");
    Buf kp = runGen(st("gl.keys", {{KyModMode, 1.f}, {KyTremDepth, 1.f}, {KyModel, 1.f}, {KyVelMuffle, 0.5f}, {KyVelCurve, 2.f}, {KyKeyPan, 1.f}}), aux,
                       {{72, 0.9f}}, N / 2);
    expect(finite(kp) && rms(kp.l) > 1e-3 && std::fabs(rms(kp.l) - rms(kp.r)) > 1e-4, "keys auto-pan, model, velocity muffle and key pan");
    Buf big = runGen(st("gl.keys"), aux, {{60, 1.f}}, 8192, 4096);
    expect(finite(big) && rms(big.l) > 1e-3, "keys handles blocks larger than its mix buffer");
  }

  // ---------------------------------------------------------------- Slicer / Granular / MIDI out
  {
    ChannelAux aux;
    aux.samples.push_back(makeSample(44100, 220, 0.5));
    PluginState manual = st("gl.slicer", {{SlMode, static_cast<float>(SlModeManual)}, {slMarker(0), 0.f}, {slMarker(1), 0.5f}});
    Buf s0 = runGen(manual, aux, {{60, 1.f}}, 30000);
    size_t last = 0;
    for (size_t i = 0; i < s0.size(); ++i)
      if (std::fabs(s0.l[i]) > 1e-4) last = i;
    expect(last > 21000 && last < 22200, "manual markers define the slice length");
    PluginState mute = manual;
    mute.params[slParam(1, SliceMute)] = 1;
    expect(rms(runGen(mute, aux, {{61, 1.f}}, 3000).l) < 1e-9, "per-slice mute");
    PluginState chrom = manual;
    chrom.params[SlChromatic] = 0;
    Buf c0 = runGen(chrom, aux, {{60, 1.f}}, 8000), c12 = runGen(chrom, aux, {{72, 1.f}}, 8000);
    int z0 = zeroCrossings(c0.l, 500, 7000), z12 = zeroCrossings(c12.l, 500, 7000);
    expect(z12 > z0 * 1.8, "chromatic slice plays at pitch");
    PluginState mono = manual;
    mono.params[SlMono] = 1, mono.params[slParam(0, SliceGain)] = -6, mono.params[SlFilterType] = 1, mono.params[SlCutoff] = 500;
    Buf mo = runGen(mono, aux, {{60, 1.f}, {61, 1.f}}, 6000);
    expect(finite(mo) && rms(mo.l) > 0.01, "slicer mono, per-slice gain, filter");
    PluginState fit = manual;
    fit.params[SlFitTempo] = 1, fit.params[SlLoopBeats] = 4;  // 1 s sample over 4 beats at 120 BPM (2 s) → half speed
    Buf fo = runGen(fit, aux, {{60, 1.f}}, 50000);
    last = 0;
    for (size_t i = 0; i < fo.size(); ++i)
      if (std::fabs(fo.l[i]) > 1e-4) last = i;
    expect(last > 43000, "fit to tempo slows the loop to the project length");
    ChannelAux ga;
    ga.samples.push_back(makeSample(44100, 220, 0.5));
    Buf gr = runGen(st("gl.granular", {{GrFreeze, 0.f}, {GrSpeed, -1.f}, {GrShape, 0.1f}, {GrKeyMode, 1.f}, {GrWobble, 2.f}, {GrJitter, 0.5f}}), ga,
                       {{64, 1.f}}, N / 2);
    expect(finite(gr) && rms(gr.l) > 0.01, "granular speed, shape, position keys, wobble, jitter");
    auto mo2 = createGenerator("gl.midiout");
    MidiOutRing ring;
    mo2->setMidiOut(&ring);
    PluginState ms = st("gl.midiout", {{MoProgram, 5.f}, {MoBankMsb, 2.f}, {moKnob(0, MoKnobValue), 64.f}, {moKnob(0, MoKnobNumber), 74.f}});
    float l[16], r[16];
    mo2->process(ms, aux, ctx(), l, r, 16);
    MidiOutRing::Msg m;
    std::vector<MidiOutRing::Msg> msgs;
    while (ring.pop(m)) msgs.push_back(m);
    bool bankFirst = msgs.size() >= 3 && msgs[0].s == 0xB0 && msgs[0].a == 0 && msgs[0].b == 2 && msgs[1].s == 0xC0 && msgs[1].a == 4;
    bool cc = false;
    for (auto& x : msgs) cc = cc || (x.s == 0xB0 && x.a == 74 && x.b == 64);
    expect(bankFirst && cc, "MIDI out sends bank select before program, and knob CCs");
  }

  return finish();
}
