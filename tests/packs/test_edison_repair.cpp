// Gridline tests: Loupe's repair tools, measured on generated material (all deterministic).
//  * deverb: a dry signal through a 0.7 s room: the reverb time is estimated within 25 %, the
//    tails after each sound drop by 8 dB or more while the sounds themselves stay within 3 dB
//    of the dry signal; a selection leaves the rest untouched; Stop leaves the audio as it was;
//  * declip: a mix driven 4 dB into hard clipping: every clipped region is found, none in
//    the clean mix, and the rebuilt audio is 10 dB or more closer to the unclipped original;
//    the gain option keeps the rebuilt peaks under the ceiling;
//  * drum loop stretch: a two-bar loop at 120 BPM to 100 and 140 BPM in both tail modes: the
//    length is exact, every hit starts on its new grid place within 1 ms with its attack kept,
//    and the end runs into the start with no click; the tempo is detected;
//  * denoise, the earlier tool against the new one, on speech-like tones, music, drums after
//    silence and hum: SNR 6 dB or more above the input (4 on drums) and no worse than before,
//    the noise alone down 12 dB or more, no musical noise (kurtosis), the adaptive mode 4 dB
//    or more without a profile, drum attacks kept;
//  * declick, the earlier tool against the new one, on a sine, drums, music, speech and vinyl
//    crackle: 15 dB or more better (8 on drums), clean audio left alone (40 dB or more);
//  * gate: no chatter near the threshold; spectral brush: soft edges.
// GL_REPAIR_WAVS=<dir> also writes before / after WAV files there for listening.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../Check.h"
#include "packs/edison/edison.h"
#include "packs/edison/edison_repair.h"

using namespace gl::packs::edison;

namespace {

constexpr double kRate = 44100;
constexpr double kPi = 3.14159265358979323846;

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed) {}
  double next() {  // -1..1
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 8388608.0 - 1.0;
  }
};

void save(const char* name, const Audio& a) {
  const char* dir = std::getenv("GL_REPAIR_WAVS");
  if (!dir || !*dir) return;
  writeWavFile(std::string(dir) + "/" + name + ".wav", makeContent(a), 24);
}

double energy(const Audio& a, int64_t f0, int64_t f1) {
  double e = 0;
  f0 = std::max<int64_t>(0, f0), f1 = std::min(f1, a.frames());
  for (int64_t i = f0; i < f1; ++i) e += static_cast<double>(a.L[static_cast<size_t>(i)]) * a.L[static_cast<size_t>(i)] + static_cast<double>(a.R[static_cast<size_t>(i)]) * a.R[static_cast<size_t>(i)];
  return e;
}
double db(double r) { return 10.0 * std::log10(std::max(r, 1e-30)); }

double snrDb(const Audio& ref, const Audio& x, int64_t f0 = 0, int64_t f1 = -1) {
  if (f1 < 0) f1 = ref.frames();
  double s = 0, n = 0;
  for (int64_t i = f0; i < f1; ++i)
    for (int ch = 0; ch < 2; ++ch) {
      const double r = ref.ch(ch)[i], d = r - x.ch(ch)[i];
      s += r * r, n += d * d;
    }
  return db(s / std::max(n, 1e-30));
}

// ---- deverb material
struct Event {
  int64_t on, off;
};
Audio dryEvents(std::vector<Event>& ev) {
  Audio a = makeAudio(kRate, 2, static_cast<int64_t>(kRate * 7.5));
  Rng rng(7);
  for (int k = 0; k < 6; ++k) {
    const int64_t on = static_cast<int64_t>(kRate * (0.3 + 1.2 * k));
    const int64_t len = static_cast<int64_t>(kRate * (k % 2 ? 0.25 : 0.18));
    ev.push_back({on, on + len});
    for (int64_t i = 0; i < len; ++i) {
      const double t = static_cast<double>(i) / kRate;
      const double env = std::min(1.0, i / 40.0) * std::min(1.0, static_cast<double>(len - i) / 200.0);
      double v;
      if (k % 2) {  // a harmonic tone
        v = 0;
        for (int h = 1; h <= 6; ++h) v += std::sin(2 * kPi * 196.0 * (1 + k * 0.1) * h * t) / h;
        v *= 0.35;
      } else {      // a noisy hit
        v = 0.5 * rng.next() * std::exp(-t * 12);
      }
      a.L[static_cast<size_t>(on + i)] = a.R[static_cast<size_t>(on + i)] = static_cast<float>(v * env);
    }
  }
  return a;
}
// A room: the direct sound, a few early reflections, then a decorrelated late tail
// (exponential noise reaching -60 dB after t60) carrying as much energy as the direct sound.
Audio roomIr(double t60) {
  const int64_t n = static_cast<int64_t>(kRate * t60 * 1.2);
  Audio ir = makeAudio(kRate, 2, n);
  ir.L[0] = ir.R[0] = 1.0f;
  const double er[][2] = {{0.007, 0.45}, {0.013, -0.35}, {0.021, 0.3}, {0.029, -0.25}, {0.041, 0.2}};
  for (const auto& e : er) {
    const size_t i = static_cast<size_t>(e[0] * kRate);
    ir.L[i] += static_cast<float>(e[1]);
    ir.R[i + 3] += static_cast<float>(e[1] * 0.9);
  }
  Rng rl(11), rr(23);
  const double d = 3.0 * std::log(10.0) / t60;
  double el = 0, erg = 0;
  std::vector<double> tl(static_cast<size_t>(n)), tr(static_cast<size_t>(n));
  for (int64_t i = static_cast<int64_t>(kRate * 0.01); i < n; ++i) {
    const double t = static_cast<double>(i) / kRate, g = std::exp(-d * t) * std::min(1.0, (t - 0.01) / 0.03);
    tl[static_cast<size_t>(i)] = rl.next() * g, tr[static_cast<size_t>(i)] = rr.next() * g;
    el += tl[static_cast<size_t>(i)] * tl[static_cast<size_t>(i)], erg += tr[static_cast<size_t>(i)] * tr[static_cast<size_t>(i)];
  }
  for (int64_t i = 0; i < n; ++i) {
    ir.L[static_cast<size_t>(i)] += static_cast<float>(tl[static_cast<size_t>(i)] / std::sqrt(el));
    ir.R[static_cast<size_t>(i)] += static_cast<float>(tr[static_cast<size_t>(i)] / std::sqrt(erg));
  }
  return ir;
}

// ---- drum material: hits at exact frames
struct Hit {
  int64_t at;
  int kind;  // 0 kick, 1 snare, 2 hat
};
void addHit(Audio& a, int64_t at, int kind, Rng& rng) {
  const int64_t len = static_cast<int64_t>(kRate * (kind == 0 ? 0.35 : kind == 1 ? 0.25 : 0.06));
  double ph = 0, lp = 0;
  for (int64_t i = 0; i < len && at + i < a.frames(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    double v;
    if (kind == 0) {
      ph += 2 * kPi * (50 + 110 * std::exp(-t * 30)) / kRate;
      v = 0.8 * std::sin(ph) * std::exp(-t * 9);
    } else if (kind == 1) {
      ph += 2 * kPi * 185 / kRate;
      v = (0.35 * rng.next() + 0.3 * std::sin(ph)) * std::exp(-t * 18);
    } else {
      const double w = rng.next();
      v = 0.25 * (w - lp) * std::exp(-t * 70);
      lp = w;
    }
    a.L[static_cast<size_t>(at + i)] += static_cast<float>(v);
    a.R[static_cast<size_t>(at + i)] += static_cast<float>(v * 0.95);
  }
}
Audio drumLoop(double bpm, int bars, std::vector<Hit>& hits) {
  const double beat = 60.0 / bpm * kRate;
  const int64_t n = std::llround(beat * 4 * bars);
  Audio a = makeAudio(kRate, 2, n);
  Rng rng(5);
  for (int e = 0; e < bars * 8; ++e) {
    const int64_t at = std::llround(e * beat / 2);
    const int b = e / 2 % 4;
    int kind = 2;
    if (e % 2 == 0 && (b == 0 || b == 2)) kind = 0;
    if (e % 2 == 0 && (b == 1 || b == 3)) kind = 1;
    if (e == 5) kind = 0;  // an off-beat kick
    hits.push_back({at, kind});
    addHit(a, at, kind, rng);
  }
  return a;
}
// First frame from `from` to `to` louder than `frac` of the peak in the 6 ms after `from`.
int64_t attackAt(const Audio& a, int64_t from, int64_t to, double frac) {
  from = std::max<int64_t>(0, from);
  double pk = 0;
  for (int64_t i = from; i < std::min(a.frames(), from + static_cast<int64_t>(kRate * 0.008)); ++i) pk = std::max<double>(pk, std::fabs(a.L[static_cast<size_t>(i)]));
  for (int64_t i = from; i < std::min(to, a.frames()); ++i)
    if (std::fabs(a.L[static_cast<size_t>(i)]) >= frac * pk) return i;
  return -1;
}
double peakIn(const Audio& a, int64_t f0, int64_t f1) {
  double pk = 0;
  for (int64_t i = std::max<int64_t>(0, f0); i < std::min(f1, a.frames()); ++i) pk = std::max<double>(pk, std::fabs(a.L[static_cast<size_t>(i)]));
  return pk;
}

}  // namespace

int main() {
  // ================================================================ deverb
  {
    std::vector<Event> ev;
    const Audio dry = dryEvents(ev);
    const Audio ir = roomIr(0.7);
    Audio wet = convolveAudio(dry, ir);
    wet.resize(dry.frames());
    const Content wc = makeContent(wet);
    const DecayEstimate est = estimateDecay(wet, {0, wet.frames()});
    std::printf("# deverb: estimated reverb time %.3f s from %d decays (bands", est.t60, est.decays);
    for (double b : est.band) std::printf(" %.2f", b);
    std::printf(")\n");
    expect(est.valid() && std::fabs(est.t60 - 0.7) <= 0.7 * 0.25, "deverb: the reverb time is estimated within 25 %");
    DeverbSettings s;
    s.estimate = est;
    s.amount = 0.6;
    const Content out = deverb(wc, {0, wet.frames()}, s);
    double tailIn = 0, tailOut = 0, dirDry = 0, dirOut = 0, dirWet = 0;
    for (const Event& e : ev) {
      const int64_t t0 = e.off + static_cast<int64_t>(kRate * 0.08), t1 = e.off + static_cast<int64_t>(kRate * 0.7);
      tailIn += energy(wet, t0, t1), tailOut += energy(out.a(), t0, t1);
      dirDry += energy(dry, e.on, e.off), dirOut += energy(out.a(), e.on, e.off), dirWet += energy(wet, e.on, e.off);
    }
    const double tailCut = db(tailIn / tailOut), direct = db(dirOut / dirDry);
    std::printf("# deverb: tails -%.1f dB, sounds %+.2f dB against the dry signal (reverberant %+.2f dB)\n", tailCut, direct, db(dirWet / dirDry));
    expect(tailCut >= 8.0, "deverb: the reverb tails drop by 8 dB or more");
    expect(std::fabs(direct) <= 3.0, "deverb: the sounds stay within 3 dB of the dry signal");
    bool finite = true;
    for (float v : out.a().L) finite = finite && std::isfinite(v);
    expect(finite && out.frames() == wc.frames(), "deverb: finite output of the same length");
    // Amount: more takes out more.
    DeverbSettings lo = s;
    lo.amount = 0.2;
    const Content outLo = deverb(wc, {0, wet.frames()}, lo);
    double tailLo = 0;
    for (const Event& e : ev) tailLo += energy(outLo.a(), e.off + static_cast<int64_t>(kRate * 0.08), e.off + static_cast<int64_t>(kRate * 0.7));
    expect(db(tailIn / tailLo) > 2.0 && tailLo > tailOut, "deverb: a lower amount takes out less");
    // A manual reverb time works too.
    DeverbSettings man = s;
    man.useEstimate = false;
    man.t60 = 0.7;
    const Content outMan = deverb(wc, {0, wet.frames()}, man);
    double tailMan = 0;
    for (const Event& e : ev) tailMan += energy(outMan.a(), e.off + static_cast<int64_t>(kRate * 0.08), e.off + static_cast<int64_t>(kRate * 0.7));
    expect(db(tailIn / tailMan) >= 8.0, "deverb: the manual reverb time takes the tails out too");
    // A selection: the audio outside it is untouched.
    const Range sel{static_cast<int64_t>(kRate * 2), static_cast<int64_t>(kRate * 4)};
    const Content part = deverb(wc, sel, s);
    bool outside = true;
    for (int64_t i = 0; i < wet.frames(); ++i)
      if (i < sel.a || i >= sel.b) outside = outside && part.a().L[static_cast<size_t>(i)] == wet.L[static_cast<size_t>(i)];
    expect(outside, "deverb: a selection leaves the audio outside it untouched");
    expect(std::fabs(part.a().L[static_cast<size_t>(sel.a)] - wet.L[static_cast<size_t>(sel.a)]) < 0.02, "deverb: the selection fades in from the audio at its start");
    // Stop: the content stays as it was.
    Progress pr;
    pr.cancel = true;
    const Content stoppedOut = deverb(wc, {0, wet.frames()}, s, &pr);
    expect(stoppedOut.audio == wc.audio, "deverb: Stop leaves the audio as it was");
    // Silence in, silence out; amount 0 changes nothing.
    const Content sil = makeContent(makeAudio(kRate, 2, 44100));
    const Content silOut = deverb(sil, {0, 44100}, s);
    expect(energy(silOut.a(), 0, 44100) == 0.0, "deverb: silence stays silent");
    DeverbSettings off = s;
    off.amount = 0;
    expect(deverb(wc, {}, off).audio == wc.audio, "deverb: amount 0 changes nothing");
    save("deverb_before", wet);
    save("deverb_after", out.a());
    save("deverb_dry_reference", dry);
  }

  // ================================================================ declip
  {
    const int64_t n = static_cast<int64_t>(kRate * 3);
    Audio clean = makeAudio(kRate, 2, n);
    Rng rng(3);
    double lp = 0;
    for (int64_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kRate;
      lp = 0.97 * lp + 0.03 * rng.next();
      const double env = 0.6 + 0.4 * std::sin(2 * kPi * 0.7 * t);
      double v = env * (0.5 * std::sin(2 * kPi * 110 * t) + 0.25 * std::sin(2 * kPi * 330 * t + 0.4) + 0.15 * std::sin(2 * kPi * 523.25 * t + 1.1) +
                        0.1 * std::sin(2 * kPi * 880 * t)) + 0.6 * lp;
      clean.L[static_cast<size_t>(i)] = static_cast<float>(v);
      clean.R[static_cast<size_t>(i)] = static_cast<float>(0.9 * v);
    }
    double pk = 0;
    for (float v : clean.L) pk = std::max<double>(pk, std::fabs(v));
    for (int64_t i = 0; i < n; ++i) clean.L[static_cast<size_t>(i)] /= static_cast<float>(pk), clean.R[static_cast<size_t>(i)] /= static_cast<float>(pk);
    // Driven 4 dB over and cut at 0.63 (hard clipping at the same level on both sides).
    const float drive = static_cast<float>(std::pow(10.0, 4.0 / 20.0)), cut = 0.63f;
    Audio ref = clean, clipped = clean;
    for (int64_t i = 0; i < n; ++i)
      for (int ch = 0; ch < 2; ++ch) {
        ref.ch(ch)[i] = clean.ch(ch)[i] * drive * cut;
        clipped.ch(ch)[i] = std::clamp(ref.ch(ch)[i], -cut, cut);
      }
    int truth = 0;  // clipped runs of the left channel (two samples or more)
    for (int64_t i = 1; i < n; ++i)
      if (std::fabs(clipped.L[static_cast<size_t>(i)]) >= cut && std::fabs(clipped.L[static_cast<size_t>(i - 1)]) < cut && i + 1 < n &&
          std::fabs(clipped.L[static_cast<size_t>(i + 1)]) >= cut)
        ++truth;
    const ClipScan scan = findClipping(clipped, {0, n}, true, 0);
    std::printf("# declip: %zu clipped regions found (%d in the left channel), %lld samples\n", scan.regions.size(), truth, static_cast<long long>(scan.samples));
    expect(static_cast<int>(scan.regions.size()) >= truth && truth > 10, "declip: every clipped region is found");
    expect(findClipping(clean, {0, n}, true, 0).regions.empty(), "declip: the clean mix has no clipped regions");
    DeclipSettings s;
    s.fitGain = false;
    int fixed = 0;
    const Content out = declip(makeContent(clipped), {}, s, nullptr, &fixed);
    const double before = snrDb(ref, clipped), after = snrDb(ref, out.a());
    std::printf("# declip: SNR against the unclipped original %.1f dB clipped, %.1f dB rebuilt (%d regions)\n", before, after, fixed);
    expect(after >= before + 10.0, "declip: the rebuilt audio is 10 dB or more closer to the original");
    expect(fixed == static_cast<int>(scan.regions.size()), "declip: it reports the regions it rebuilt");
    double pkOut = 0;
    for (float v : out.a().L) pkOut = std::max<double>(pkOut, std::fabs(v));
    expect(pkOut > cut * 1.05, "declip: the rebuilt peaks rise above the clip level");
    DeclipSettings g = s;
    g.fitGain = true;
    g.ceilingDb = -1.0;
    const Content fit = declip(makeContent(clipped), {}, g);
    double pkFit = 0;
    for (int ch = 0; ch < 2; ++ch)
      for (int64_t i = 0; i < n; ++i) pkFit = std::max<double>(pkFit, std::fabs(fit.a().ch(ch)[i]));
    expect(pkFit <= std::pow(10.0, -1.0 / 20.0) + 1e-6, "declip: lower gain keeps the rebuilt peaks under the ceiling");
    // Manual level: a lower level finds at least as much.
    expect(findClipping(clipped, {0, n}, false, -1.0).regions.size() >= scan.regions.size(), "declip: a lower clip level finds more");
    Progress pr;
    pr.cancel = true;
    const Content cc = makeContent(clipped);
    expect(declip(cc, {}, s, &pr).audio == cc.audio, "declip: Stop leaves the audio as it was");
    save("declip_before", clipped);
    save("declip_after", out.a());
    save("declip_original", ref);
  }

  // ================================================================ drum loop stretch
  {
    std::vector<Hit> hits;
    const Audio loop = drumLoop(120, 2, hits);
    const LoopTempo lt = analyseLoop(loop, {0, loop.frames()}, 0.5, 4);
    std::printf("# drum stretch: detected %.2f BPM, %.2f bars, %zu hits (of %zu) at", lt.bpm, lt.bars, lt.hits.size(), hits.size());
    for (int64_t h : lt.hits) std::printf(" %lld", static_cast<long long>(h));
    std::printf("\n");
    expect(std::fabs(lt.bpm - 120) < 0.5 && std::fabs(lt.bars - 2) < 1e-6, "drum stretch: the loop's tempo and bars are detected");
    expect(lt.hits.size() >= hits.size() - 2, "drum stretch: the hits are found");
    save("drums_before", loop);
    for (int tails = 0; tails < 2; ++tails)
      for (double bpm : {100.0, 140.0}) {
        DrumStretchSettings s;
        s.sourceBpm = lt.bpm;
        s.targetBpm = bpm;
        s.bars = 2;
        s.tails = tails;
        const Content out = drumStretch(makeContent(loop), {}, s);
        const Audio& y = out.a();
        const int64_t want = std::llround(2 * 4 * 60.0 / bpm * kRate);
        char nm[160];
        std::snprintf(nm, sizeof nm, "drum stretch %s to %.0f BPM: the length is exact", tails ? "(slices)" : "(stretch)", bpm);
        expect(y.frames() == want, nm);
        const double f = static_cast<double>(want) / static_cast<double>(loop.frames());
        double worst = 0, keep = 1e9;
        for (const Hit& h : hits) {
          const int64_t nh = std::llround(static_cast<double>(h.at) * f);
          const int64_t ai = attackAt(loop, h.at, h.at + 200, 0.3), ao = attackAt(y, nh - 100, nh + 200, 0.3);
          if (ai < 0 || ao < 0) {
            worst = 1e9;
            continue;
          }
          worst = std::max(worst, std::fabs(static_cast<double>((ao - nh) - (ai - h.at))) / kRate * 1000.0);
          const double pi = peakIn(loop, h.at, h.at + static_cast<int64_t>(kRate * 0.005)), po = peakIn(y, nh, nh + static_cast<int64_t>(kRate * 0.005));
          keep = std::min(keep, po / std::max(pi, 1e-9));
        }
        std::printf("# drum stretch %s %.0f BPM: worst hit offset %.3f ms, weakest attack %.2f of the original\n", tails ? "slices" : "stretch", bpm, worst, keep);
        std::snprintf(nm, sizeof nm, "drum stretch %s to %.0f BPM: every hit lands on the new grid within 1 ms", tails ? "(slices)" : "(stretch)", bpm);
        expect(worst <= 1.0, nm);
        std::snprintf(nm, sizeof nm, "drum stretch %s to %.0f BPM: the attacks stay sharp", tails ? "(slices)" : "(stretch)", bpm);
        expect(keep >= 0.85, nm);
        // The loop point: the jump from the last sample to the first is no bigger than the
        // signal's own steps (99th percentile).
        std::vector<double> steps;
        for (int64_t i = 1; i < y.frames(); ++i) steps.push_back(std::fabs(y.L[static_cast<size_t>(i)] - y.L[static_cast<size_t>(i - 1)]));
        std::sort(steps.begin(), steps.end());
        const double p99 = steps[steps.size() * 99 / 100];
        const double seam = std::fabs(y.L[0] - y.L[static_cast<size_t>(y.frames() - 1)]);
        std::printf("# drum stretch %s %.0f BPM: loop point step %.5f (99th percentile step %.5f)\n", tails ? "slices" : "stretch", bpm, seam, p99);
        std::snprintf(nm, sizeof nm, "drum stretch %s to %.0f BPM: no click at the loop point", tails ? "(slices)" : "(stretch)", bpm);
        expect(seam <= p99, nm);
        bool finite = true;
        for (float v : y.L) finite = finite && std::isfinite(v) && std::fabs(v) < 4;
        std::snprintf(nm, sizeof nm, "drum stretch %s to %.0f BPM: finite output", tails ? "(slices)" : "(stretch)", bpm);
        expect(finite, nm);
        std::snprintf(nm, sizeof nm, "drums_%s_%.0fbpm", tails ? "slices" : "stretch", bpm);
        save(nm, y);
      }
    // A selection keeps the rest of the file around it.
    Audio two = loop;
    two.resize(loop.frames() * 2);
    for (int64_t i = 0; i < loop.frames(); ++i) two.L[static_cast<size_t>(loop.frames() + i)] = loop.L[static_cast<size_t>(i)], two.R[static_cast<size_t>(loop.frames() + i)] = loop.R[static_cast<size_t>(i)];
    DrumStretchSettings s;
    s.targetBpm = 100;
    s.bars = 2;
    const Content part = drumStretch(makeContent(two), {0, loop.frames()}, s);
    const int64_t want = std::llround(2 * 4 * 60.0 / 100 * kRate);
    bool rest = part.frames() == want + loop.frames();
    for (int64_t i = 0; rest && i < loop.frames(); ++i) rest = part.a().L[static_cast<size_t>(want + i)] == loop.L[static_cast<size_t>(i)];
    expect(rest, "drum stretch: a selection is replaced and the audio after it follows unchanged");
    expect(drumStretchLength(s, kRate) == want, "drum stretch: the new length is bars at the target tempo");
  }

  // ================================================================ material for the clean-up tools
  // Speech-like (harmonics through formants, syllables with gaps), music (held chords that
  // decay), drums and a tonal mix; each with a second of silence first.
  auto speech = [](int64_t n) {
    Audio a = makeAudio(kRate, 2, n);
    for (int64_t i = static_cast<int64_t>(kRate); i < n; ++i) {
      const double t = static_cast<double>(i) / kRate;
      const double syl = std::max(0.0, std::sin(2 * kPi * 2.5 * t));
      const double f0 = 150 + 20 * std::sin(2 * kPi * 0.8 * t);
      double v = 0;
      for (int h = 1; h <= 12; ++h) {
        const double hz = f0 * h;
        const double formant = std::exp(-std::pow((hz - 600) / 300, 2)) + 0.6 * std::exp(-std::pow((hz - 1700) / 400, 2)) + 0.05;
        v += formant * std::sin(2 * kPi * f0 * h * t + h);
      }
      a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = static_cast<float>(0.25 * v * syl);
    }
    return a;
  };
  auto music = [](int64_t n) {
    Audio a = makeAudio(kRate, 2, n);
    const double chords[3][3] = {{261.63, 329.63, 392.0}, {220.0, 261.63, 329.63}, {196.0, 246.94, 293.66}};
    for (int64_t i = static_cast<int64_t>(kRate); i < n; ++i) {
      const double t = static_cast<double>(i) / kRate - 1.0;
      const int c = static_cast<int>(t / 1.0) % 3;
      const double lt = std::fmod(t, 1.0);
      double v = 0;
      for (double hz : chords[c])
        for (int h = 1; h <= 4; ++h) v += std::sin(2 * kPi * hz * h * t) * std::exp(-lt * (1.5 + h)) / (h * h);
      v *= 0.25 * std::min(1.0, lt * 400);
      a.L[static_cast<size_t>(i)] = static_cast<float>(v);
      a.R[static_cast<size_t>(i)] = static_cast<float>(v * 0.9);
    }
    return a;
  };
  auto drumsAfterSilence = [](int64_t n) {
    std::vector<Hit> h;
    const Audio d = drumLoop(120, 2, h);
    Audio a = makeAudio(kRate, 2, n);
    for (int64_t i = 0; i + static_cast<int64_t>(kRate) < n && i < d.frames(); ++i)
      a.L[static_cast<size_t>(i + static_cast<int64_t>(kRate))] = d.L[static_cast<size_t>(i)], a.R[static_cast<size_t>(i + static_cast<int64_t>(kRate))] = d.R[static_cast<size_t>(i)];
    return a;
  };
  auto addNoise = [](const Audio& clean, double level, uint32_t seed, bool hum) {
    Audio a = clean;
    Rng rng(seed);
    double pinkL = 0, pinkR = 0;
    for (int64_t i = 0; i < a.frames(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      pinkL = 0.9 * pinkL + 0.1 * rng.next() * 3.0, pinkR = 0.9 * pinkR + 0.1 * rng.next() * 3.0;
      double hl = 0;
      if (hum)
        for (int h = 1; h <= 7; h += 2) hl += std::sin(2 * kPi * 60.0 * h * t) / h;
      a.L[static_cast<size_t>(i)] += static_cast<float>(level * (hum ? 1.2 * hl + 0.15 * rng.next() : 0.6 * rng.next() + 0.6 * pinkL));
      a.R[static_cast<size_t>(i)] += static_cast<float>(level * (hum ? 1.2 * hl + 0.15 * rng.next() : 0.6 * rng.next() + 0.6 * pinkR));
    }
    return a;
  };
  // Musical noise: the kurtosis of the power spectrum over a noise-only part, after against
  // before (1 is natural noise; isolated tones popping up raise it).
  auto kurtosis = [](const Audio& a, int64_t f0, int64_t f1) {
    const int fft = 512;
    double m1 = 0, m2 = 0;
    int cnt = 0;
    for (int64_t s0 = f0; s0 + fft <= f1; s0 += fft / 2) {
      std::vector<double> re(fft), im(fft);
      for (int k = 2; k < fft / 2; k += 2) {  // a plain DFT of a few bins (enough for statistics)
        double cr = 0, ci = 0;
        for (int i = 0; i < fft; ++i) {
          const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / fft), v = a.L[static_cast<size_t>(s0 + i)] * w;
          cr += v * std::cos(2 * kPi * k * i / fft), ci -= v * std::sin(2 * kPi * k * i / fft);
        }
        const double pw = cr * cr + ci * ci;
        m1 += pw, m2 += pw * pw, ++cnt;
      }
    }
    m1 /= std::max(1, cnt), m2 /= std::max(1, cnt);
    return m2 / std::max(1e-30, m1 * m1);
  };

  // ================================================================ denoise: the earlier tool against the new one
  {
    const int64_t n = static_cast<int64_t>(kRate * 4);
    const int64_t quiet = static_cast<int64_t>(kRate * 0.9);
    struct Case {
      const char* name;
      Audio clean;
      double level;
      bool hum;
    };
    std::vector<Case> cases = {{"speech", speech(n), 0.02, false}, {"music", music(n), 0.015, false}, {"drums", drumsAfterSilence(n), 0.015, false},
                               {"hum", music(n), 0.01, true}};
    for (const Case& cs : cases) {
      const Audio noisy = addNoise(cs.clean, cs.level, 21, cs.hum);
      const NoiseProfile prof = acquireNoise(noisy, {0, quiet});
      const Content old = denoise(makeContent(noisy), {}, prof, 24, 0.5);
      DenoiseSettings s;
      s.profile = prof;
      s.character = cs.hum ? 1 : 0;
      const Content neu = denoiseHq(makeContent(noisy), {}, s);
      DenoiseSettings ad = s;
      ad.adaptive = true;
      ad.profile = NoiseProfile{};
      const Content adapt = denoiseHq(makeContent(noisy), {}, ad);
      const int64_t t0 = static_cast<int64_t>(kRate);
      const double snrIn = snrDb(cs.clean, noisy, t0, n);
      const double snrOld = snrDb(cs.clean, old.a(), t0, n), snrNew = snrDb(cs.clean, neu.a(), t0, n), snrAd = snrDb(cs.clean, adapt.a(), t0, n);
      const double e0 = energy(noisy, 0, quiet);
      const double cutOld = db(e0 / energy(old.a(), 0, quiet)), cutNew = db(e0 / energy(neu.a(), 0, quiet));
      const double kIn = kurtosis(noisy, 0, quiet), kOld = kurtosis(old.a(), 0, quiet) / kIn, kNew = kurtosis(neu.a(), 0, quiet) / kIn;
      std::printf("# denoise %s: SNR in %.1f dB; earlier %.1f dB (noise -%.1f dB, musical %.2f); new %.1f dB (noise -%.1f dB, musical %.2f); adaptive %.1f dB\n",
                  cs.name, snrIn, snrOld, cutOld, kOld, snrNew, cutNew, kNew, snrAd);
      std::printf("# denoise %s: SNR per half second from 1 s (in / learned / adaptive):", cs.name);
      for (int64_t q = t0; q + static_cast<int64_t>(kRate / 2) <= n; q += static_cast<int64_t>(kRate / 2))
        std::printf(" %.1f/%.1f/%.1f", snrDb(cs.clean, noisy, q, q + static_cast<int64_t>(kRate / 2)), snrDb(cs.clean, neu.a(), q, q + static_cast<int64_t>(kRate / 2)),
                    snrDb(cs.clean, adapt.a(), q, q + static_cast<int64_t>(kRate / 2)));
      std::printf("\n");
      char nm[160];
      std::snprintf(nm, sizeof nm, "denoise %s: the new tool is cleaner (SNR 6 dB or more above the input, 4 on drums, at least the earlier tool's)", cs.name);
      // Drums: the snare and hats are noise-like themselves, so less can come out under them.
      expect(snrNew >= snrIn + (std::string(cs.name) == "drums" ? 4 : 6) && snrNew >= snrOld - 0.5, nm);
      std::snprintf(nm, sizeof nm, "denoise %s: noise on its own lowered by 12 dB or more", cs.name);
      expect(cutNew >= 12, nm);
      std::snprintf(nm, sizeof nm, "denoise %s: no musical noise (residual kurtosis within 2.5 of natural)", cs.name);
      expect(kNew <= 2.5, nm);
      std::snprintf(nm, sizeof nm, "denoise %s: the adaptive mode cleans without a profile (4 dB or more)", cs.name);
      expect(snrAd >= snrIn + 4, nm);
      std::snprintf(nm, sizeof nm, "denoise_%s_before", cs.name);
      save(nm, noisy);
      std::snprintf(nm, sizeof nm, "denoise_%s_after", cs.name);
      save(nm, neu.a());
      Audio removed = noisy;
      for (int ch = 0; ch < 2; ++ch)
        for (int64_t i = 0; i < n; ++i) removed.ch(ch)[i] = noisy.ch(ch)[i] - neu.a().ch(ch)[i];
      std::snprintf(nm, sizeof nm, "denoise_%s_removed", cs.name);
      save(nm, removed);
      std::snprintf(nm, sizeof nm, "denoise_%s_earlier_tool", cs.name);
      save(nm, old.a());
      if (std::string(cs.name) == "drums") {  // attacks: the first 3 ms of each hit keep their peak
        double worst = 1;
        std::vector<Hit> hits;
        drumLoop(120, 2, hits);
        for (const Hit& h : hits) {
          const int64_t at = h.at + t0;
          if (at + 132 > n) continue;  // past the end of the test audio
          worst = std::min(worst, peakIn(neu.a(), at, at + 132) / std::max(1e-9, peakIn(cs.clean, at, at + 132)));
        }
        std::printf("# denoise drums: weakest attack %.2f of the clean one\n", worst);
        expect(worst >= 0.8, "denoise drums: attacks kept (80 % of their peak or more)");
      }
    }
    Progress pr;
    pr.cancel = true;
    DenoiseSettings s;
    const Content c = makeContent(addNoise(speech(n), 0.02, 3, false));
    expect(denoiseHq(c, {}, s, &pr).audio == c.audio, "denoise: Stop leaves the audio as it was");
  }

  // ================================================================ declick: the earlier tool against the new one
  {
    const int64_t n = static_cast<int64_t>(kRate * 3);
    Audio sine = makeAudio(kRate, 2, n);
    for (int64_t i = 0; i < n; ++i) sine.L[static_cast<size_t>(i)] = sine.R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * kPi * 440 * i / kRate));
    std::vector<Hit> hh;
    const Audio drums = drumLoop(120, 1, hh);
    Audio drumsN = makeAudio(kRate, 2, n);
    for (int64_t i = 0; i < n; ++i) drumsN.L[static_cast<size_t>(i)] = drums.L[static_cast<size_t>(i % drums.frames())], drumsN.R[static_cast<size_t>(i)] = drums.R[static_cast<size_t>(i % drums.frames())];
    struct Case {
      const char* name;
      Audio clean;
      bool crackle;
    };
    std::vector<Case> cases = {{"sine", sine, false}, {"drums", drumsN, false}, {"music", music(n), false}, {"speech", speech(n), false},
                               {"music_crackle", music(n), true}};
    for (const Case& cs : cases) {
      Audio dirty = cs.clean;
      Rng rng(9);
      const int count = cs.crackle ? 600 : 30;
      std::vector<std::pair<int64_t, int>> placed;
      for (int k = 0; k < count; ++k) {
        const int64_t at = static_cast<int64_t>((k + 0.5 + 0.3 * rng.next()) / count * static_cast<double>(n - 64));
        const int w = cs.crackle ? 1 + static_cast<int>((rng.next() + 1) * 3) : (k % 3 == 0 ? 1 : k % 3 == 1 ? 4 : 12);
        const double amp = cs.crackle ? 0.03 + 0.12 * std::fabs(rng.next()) : 0.4;
        placed.push_back({at, w});
        for (int q = 0; q < w; ++q)
          for (int ch = 0; ch < 2; ++ch) dirty.ch(ch)[at + q] += static_cast<float>(amp * (k % 2 ? 1 : -1) * std::exp(-q * 0.4) * (ch ? 0.9 : 1.0));
      }
      const Content old = declick(makeContent(dirty), {}, 0.5);
      int fixed = 0;
      const Content neu = declickHq(makeContent(dirty), {}, DeclickSettings{}, nullptr, &fixed);
      const Content keep = declickHq(makeContent(cs.clean), {}, DeclickSettings{});
      {  // the clicks left worst (error energy around each, after)
        std::vector<std::pair<double, size_t>> errs;
        for (size_t q = 0; q < placed.size(); ++q) {
          double e = 0;
          for (int64_t i = std::max<int64_t>(0, placed[q].first - 16); i < std::min<int64_t>(n, placed[q].first + placed[q].second + 48); ++i) {
            const double d = neu.a().L[static_cast<size_t>(i)] - cs.clean.L[static_cast<size_t>(i)];
            e += d * d;
          }
          errs.push_back({e, q});
        }
        std::sort(errs.rbegin(), errs.rend());
        std::printf("# declick %s: worst left", cs.name);
        for (size_t q = 0; q < errs.size() && q < 4; ++q)
          std::printf(" %lld(w%d):%.2e", static_cast<long long>(placed[errs[q].second].first), placed[errs[q].second].second, errs[q].first);
        std::printf("\n");
      }
      const Content keepOld = declick(makeContent(cs.clean), {}, 0.5);
      {
        const std::vector<Range> fp = findClicks(cs.clean, {0, n}, DeclickSettings{});
        std::printf("# declick %s: %zu found in the clean audio", cs.name, fp.size());
        for (size_t q = 0; q < fp.size() && q < 8; ++q) std::printf(" %lld+%lld", static_cast<long long>(fp[q].a), static_cast<long long>(fp[q].len()));
        std::printf("\n");
      }
      const double in = snrDb(cs.clean, dirty), so = snrDb(cs.clean, old.a()), sn = snrDb(cs.clean, neu.a());
      const double ko = snrDb(cs.clean, keepOld.a()), kn = snrDb(cs.clean, keep.a());
      std::printf("# declick %s: SNR in %.1f dB; earlier %.1f dB; new %.1f dB (%d clicks); clean audio kept at %.1f dB earlier, %.1f dB new\n", cs.name, in, so, sn,
                  fixed, ko > 200 ? 999.0 : ko, kn > 200 ? 999.0 : kn);
      char nm[160];
      // Drums: a click inside a snare or hat (noise itself) is left alone rather than risk the hit.
      const double need = std::string(cs.name) == "drums" ? 8.0 : 15.0;
      std::snprintf(nm, sizeof nm, "declick %s: the new tool repairs the clicks (%.0f dB or more better, at least the earlier tool)", cs.name, need);
      expect(sn >= in + need && sn >= so - 0.5, nm);
      std::snprintf(nm, sizeof nm, "declick %s: clean audio is left alone (40 dB or more)", cs.name);
      expect(kn >= 40, nm);
      std::snprintf(nm, sizeof nm, "declick_%s_before", cs.name);
      save(nm, dirty);
      std::snprintf(nm, sizeof nm, "declick_%s_after", cs.name);
      save(nm, neu.a());
      Audio removed = dirty;
      for (int ch = 0; ch < 2; ++ch)
        for (int64_t i = 0; i < n; ++i) removed.ch(ch)[i] = dirty.ch(ch)[i] - neu.a().ch(ch)[i];
      std::snprintf(nm, sizeof nm, "declick_%s_removed", cs.name);
      save(nm, removed);
    }
    DeclickSettings lng;
    lng.maxClickMs = 0.1;
    Audio wide = sine;
    for (int q = 0; q < 40; ++q) wide.L[static_cast<size_t>(10000 + q)] += 0.4f, wide.R[static_cast<size_t>(10000 + q)] += 0.4f;
    expect(findClicks(wide, {0, n}, lng).empty(), "declick: disturbances longer than the longest click are left");
  }

  // ================================================================ gate
  {
    const int64_t n = static_cast<int64_t>(kRate * 3);
    Audio a = makeAudio(kRate, 2, n);
    Rng rng(4);
    for (int64_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kRate;
      const bool on = std::fmod(t, 1.0) < 0.4;
      const double tone = on ? 0.3 * std::sin(2 * kPi * 110 * t) * (0.6 + 0.4 * std::sin(2 * kPi * 3 * t)) : 0.0;  // low notes fluctuating near the threshold
      a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = static_cast<float>(tone + 0.003 * rng.next());
    }
    auto toggles = [&](const Audio& y) {  // gain changes of direction (chatter) in the notes
      int c = 0;
      bool wasOpen = false;
      for (int64_t i = 0; i < n; i += 64) {
        const bool open = std::fabs(y.L[static_cast<size_t>(i)]) > 0.0 || std::fabs(a.L[static_cast<size_t>(i)]) < 1e-9;
        if (open != wasOpen) ++c;
        wasOpen = open;
      }
      return c;
    };
    GateSettings g;
    g.thresholdDb = -24;
    const Content neu = gateHq(makeContent(a), {}, g);
    const Content old = gate(makeContent(a), {0, n}, -24, 1, 30, 80, -90);
    const double gapIn = energy(a, static_cast<int64_t>(kRate * 0.6), static_cast<int64_t>(kRate * 0.95));
    const double gapNew = energy(neu.a(), static_cast<int64_t>(kRate * 0.6), static_cast<int64_t>(kRate * 0.95));
    const double notesNew = db(energy(neu.a(), static_cast<int64_t>(kRate * 0.05), static_cast<int64_t>(kRate * 0.35)) / energy(a, static_cast<int64_t>(kRate * 0.05), static_cast<int64_t>(kRate * 0.35)));
    std::printf("# gate: state changes earlier %d, new %d; gaps -%.1f dB; notes %+.2f dB\n", toggles(old.a()), toggles(neu.a()), db(gapIn / std::max(gapNew, 1e-30)), notesNew);
    expect(toggles(neu.a()) <= toggles(old.a()) && toggles(neu.a()) <= 8, "gate: no chatter on notes near the threshold");
    expect(db(gapIn / std::max(gapNew, 1e-30)) > 40 && std::fabs(notesNew) < 0.5, "gate: the gaps go silent and the notes stay");
  }

  // ================================================================ spectral brush
  {
    const int64_t n = static_cast<int64_t>(kRate * 2);
    Audio a = makeAudio(kRate, 2, n);
    for (int64_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kRate;
      a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(2 * kPi * 1000 * t) + 0.3 * std::sin(2 * kPi * 250 * t));
    }
    const std::vector<SpectralRect> rect = {{static_cast<int64_t>(kRate * 0.5), static_cast<int64_t>(kRate * 1.5), 800, 1250}};
    const Content soft = spectralGainSoft(makeContent(a), rect, -40);
    // Inside the painted time: the 1 kHz tone, measured by correlation, against the 250 Hz one.
    auto tone = [&](const Audio& y, double hz) {
      double c = 0, s2 = 0;
      for (int64_t i = static_cast<int64_t>(kRate * 0.8); i < static_cast<int64_t>(kRate * 1.2); ++i) {
        c += y.L[static_cast<size_t>(i)] * std::sin(2 * kPi * hz * i / kRate), s2 += y.L[static_cast<size_t>(i)] * std::cos(2 * kPi * hz * i / kRate);
      }
      return std::sqrt(c * c + s2 * s2);
    };
    const double cut = 20 * std::log10(tone(a, 1000) / tone(soft.a(), 1000)), other = 20 * std::log10(tone(soft.a(), 250) / tone(a, 250));
    double jump = 0;
    for (int64_t i = 1; i < n; ++i) jump = std::max<double>(jump, std::fabs(soft.a().L[static_cast<size_t>(i)] - soft.a().L[static_cast<size_t>(i - 1)]));
    std::printf("# spectral brush: painted tone -%.1f dB, the tone outside %+.2f dB, largest step %.4f\n", cut, other, jump);
    expect(cut >= 30 && std::fabs(other) < 0.5, "spectral brush: the painted tone goes, the rest stays");
    expect(jump < 0.2, "spectral brush: soft edges (no clicks where the area starts and ends)");
  }
  return finish();
}
