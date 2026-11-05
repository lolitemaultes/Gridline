// Gridline: K-61, stretching a bass. The engines' windows follow the source's sample rate, whatever
// the output's (a 96 kHz bass at a 44.1 kHz output was left with noise between its partials), and a
// clip's render slows a low monophonic sound through the waveform-keeping stretch (LowTonal.h)
// without taking anything else there. Every signal here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "../../src/core/Automation.h"
#include "../../src/engine/LowTonal.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/TimeStretch.h"
#include "../Check.h"

using namespace gl;

namespace {

constexpr double kOut = 44100;

// A bass note line: sawish, with the partials' phases spread, plucked or sustained. Notes every 0.8 s
// (fixedHz: one steady note, no envelope at all).
std::vector<float> bass(double sr, double seconds, bool plucked, double fixedHz = 0) {
  const double notes[] = {55, 41.2, 73.4, 110, 82.4, 49, 65.4, 55};
  std::vector<float> x(static_cast<size_t>(seconds * sr));
  double ph = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    const int k = static_cast<int>(t / 0.8);
    const double u = t - k * 0.8;
    const double f = fixedHz > 0 ? fixedHz : notes[k % 8];
    ph += 2 * M_PI * f / sr;
    const double env = fixedHz > 0 ? 1.0 : plucked ? std::min(1.0, u / 0.002) * std::exp(-u / 0.5) : std::min(1.0, u / 0.01) * std::min(1.0, (0.8 - u) / 0.01);
    double v = 0;
    for (int h = 1; h <= 14; ++h) v += std::sin(h * ph + 0.3 * h * h) / std::pow(h, 1.1);
    x[i] = static_cast<float>(0.25 * env * v);
  }
  return x;
}

SamplePtr sampleOf(const std::vector<float>& l, const std::vector<float>& r, double sr) {
  auto s = std::make_shared<Sample>();
  s->left.assign(l.begin(), l.end());
  s->right.assign(r.begin(), r.end());
  s->sampleRate = sr;
  return s;
}

double power(const float* x, int n, double hz, double sr) {
  double re = 0, im = 0, sum = 0;
  for (int i = 0; i < n; ++i) {
    const double h = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / n);
    re += h * x[i] * std::cos(2 * M_PI * hz * i / sr), im -= h * x[i] * std::sin(2 * M_PI * hz * i / sr);
    sum += h;
  }
  return (re * re + im * im) / (sum * sum);
}

// Noise between the partials of a steady note of f0 against the partials, dB (0.4 s at `at` seconds).
double floorDb(const std::vector<float>& x, double sr, double at, double f0) {
  const int n = static_cast<int>(0.4 * sr);
  const size_t a = static_cast<size_t>(at * sr);
  if (a + static_cast<size_t>(n) > x.size()) return 0;
  double tone = 0, noise = 0;
  for (double hz = f0 / 2; hz < 800; hz += 1.0) {
    const double p = power(x.data() + a, n, hz, sr);
    const double off = std::fabs(hz / f0 - std::round(hz / f0)) * f0;
    if (off < 10) tone += p;
    else if (off > 14) noise += p;
  }
  return 10 * std::log10(std::max(noise, 1e-20) / std::max(tone, 1e-20));
}

// A waveform's shape: peak over RMS across a span, dB.
double crestDb(const std::vector<float>& x, double sr, double from, double to) {
  double e = 0, p = 0;
  size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i], p = std::max(p, static_cast<double>(std::fabs(x[i])));
  return 10 * std::log10(p * p / std::max(e / static_cast<double>(b - a), 1e-20));
}

double rmsDb(const std::vector<float>& x, double sr, double from, double to) {
  double e = 0;
  size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return 10 * std::log10(std::max(e / static_cast<double>(b - a), 1e-20));
}

// The strongest frequency within 8% of `near` over a span, to 0.05 Hz.
double fundamentalHz(const std::vector<float>& x, double sr, double from, double to, double near) {
  const size_t a = static_cast<size_t>(from * sr);
  const int n = static_cast<int>(std::min(x.size() - a, static_cast<size_t>((to - from) * sr)));
  double best = 0, at = 0;
  for (double hz = near * 0.92; hz < near * 1.08; hz += 0.05) {
    const double p = power(x.data() + a, n, hz, sr);
    if (p > best) best = p, at = hz;
  }
  return at;
}

std::vector<float> rendered(const SamplePtr& s, double speed, double pitch, double seconds) {
  stretch::ClipRequest r;
  r.sample = s;
  r.srcStart = 0, r.srcEnd = s->seconds();
  r.mode = stretch::kStretch;
  r.pitch = pitch;
  r.rate = kOut;
  r.speed = speed;
  r.frames = static_cast<int64_t>(seconds * kOut);
  auto c = stretch::renderClip(r, false);
  return c ? std::vector<float>(c->L.begin(), c->L.end()) : std::vector<float>();
}

struct Reading {
  const std::vector<float>* x;
  int64_t at = 0;
};
void readFrames(void* ctx, float* const* dst, int frames) {
  auto* r = static_cast<Reading*>(ctx);
  for (int i = 0; i < frames; ++i, ++r->at) {
    const float v = r->at >= 0 && r->at < static_cast<int64_t>(r->x->size()) ? (*r->x)[static_cast<size_t>(r->at)] : 0.f;
    dst[0][i] = v, dst[1][i] = v;
  }
}

std::vector<float> voiced(const std::vector<float>& x, double srcRate, double declaredRate, double speed, double seconds) {
  auto v = std::make_unique<stretch::Voice>();
  const double toFrames = srcRate / kOut;
  v->prepare(2, kOut, 8.0 * toFrames, 8.0 * toFrames, false, stretch::Engine::Stretch, toFrames / 8.0, declaredRate);
  Reading r{&x, 0};
  const double frames = speed * toFrames;
  v->start(&readFrames, &r, frames, toFrames, 0);
  std::vector<float> l(static_cast<size_t>(seconds * kOut)), o(l.size());
  float* out[2] = {l.data(), o.data()};
  v->render(out, static_cast<int>(l.size()), frames, toFrames, &readFrames, &r);
  return l;
}

bool taken(const std::vector<float>& l, const std::vector<float>& r, double sr, bool stereo = false) {
  const float* in[2] = {l.data(), r.data()};
  return stretch::lowTonal(in, stereo ? 2 : 1, static_cast<int64_t>(l.size()), sr);
}

}  // namespace

int main() {
  // ---- the windows follow the source's rate ----
  {
    // A steady 55 Hz note at 96 kHz, sped up (the spectral stretch) at a 44.1 kHz output.
    const std::vector<float> x = bass(96000, 6.0, false, 55.0), low = bass(kOut, 6.0, false, 55.0);
    const auto a = rendered(sampleOf(x, x, 96000), 1.4, 1.0, 3.5), b = rendered(sampleOf(low, low, kOut), 1.4, 1.0, 3.5);
    const double fa = floorDb(a, kOut, 1.0, 55.0), fb = floorDb(b, kOut, 1.0, 55.0);
    std::printf("# render, sped up: floor between partials %.1f dB (96 kHz source), %.1f dB (44.1 kHz source)\n", fa, fb);
    expect(fa < -35.0, "a 96 kHz bass renders with little noise between its partials at a 44.1 kHz output");
    expect(std::fabs(fa - fb) < 6.0, "a bass renders alike from a 96 kHz and a 44.1 kHz source");
    // The realtime voice: told the source's rate, and as it was (windows sized at the output's rate).
    const auto va = voiced(x, 96000, 96000, 1.4, 3.5), vo = voiced(x, 96000, 0, 1.4, 3.5);
    const double fv = floorDb(va, kOut, 1.0, 55.0), fw = floorDb(vo, kOut, 1.0, 55.0);
    std::printf("# voice, sped up: floor %.1f dB with the source's rate, %.1f dB with windows at the output's rate\n", fv, fw);
    expect(fv < fw - 6.0, "the realtime voice's windows follow the source's rate: less noise between the partials");
    expect(std::fabs(fundamentalHz(va, kOut, 1.0, 3.0, 55.0) - 55.0) < 0.2, "the realtime voice stays in tune at a 96 kHz source");
    // The default keeps the voice on the output's rate when the source is at that rate.
    const auto same = voiced(low, kOut, 0, 1.4, 3.5), told = voiced(low, kOut, kOut, 1.4, 3.5);
    double most = 0;
    for (size_t i = 0; i < same.size(); ++i) most = std::max(most, static_cast<double>(std::fabs(same[i] - told[i])));
    expect(most == 0.0, "a source at the output's rate plays exactly as before");
  }
  // ---- what the low tonal stretch takes ----
  {
    const std::vector<float> pl = bass(kOut, 4.0, true), su = bass(96000, 4.0, false);
    expect(taken(pl, pl, kOut), "a plucked bass line is taken");
    expect(taken(su, su, 96000), "a sustained bass line at 96 kHz is taken");
    expect(taken(pl, pl, kOut, true), "the same bass in two equal channels is taken");
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.15f);
    std::vector<float> noise(pl.size()), hats = pl, chord(pl.size()), high(pl.size()), kicks(pl.size()), left(pl.size()), right(pl.size());
    for (size_t i = 0; i < pl.size(); ++i) {
      const double t = static_cast<double>(i) / kOut;
      noise[i] = nd(rng);
      const double u = std::fmod(t, 0.25);
      hats[i] += static_cast<float>(4.0 * std::exp(-u / 0.03)) * nd(rng);
      chord[i] = static_cast<float>(0.2 * (std::sin(2 * M_PI * 61 * t) + std::sin(2 * M_PI * 83.7 * t) + std::sin(2 * M_PI * 137.3 * t)));
      high[i] = static_cast<float>(0.3 * std::sin(2 * M_PI * 1760 * t));
      left[i] = static_cast<float>(0.3 * std::sin(2 * M_PI * 55 * t));
      right[i] = static_cast<float>(0.3 * std::sin(2 * M_PI * 55 * t + 2.0));
      kicks[i] = static_cast<float>(0.8 * std::exp(-u / 0.02) * std::sin(2 * M_PI * 140 * std::exp(-u / 0.03) * u)) + nd(rng) * static_cast<float>(std::exp(-u / 0.01));
    }
    expect(!taken(noise, noise, kOut), "noise is not taken");
    expect(!taken(hats, hats, kOut), "a bass with loud hits over it is not taken");
    expect(!taken(chord, chord, kOut), "a low chord of unrelated notes is not taken");
    expect(!taken(high, high, kOut), "a high tone is not taken");
    expect(!taken(kicks, kicks, kOut), "a drum loop is not taken");
    expect(!taken(left, right, kOut, true), "a bass far apart in its two channels is not taken");
    const std::vector<float> silent(pl.size(), 0.f);
    expect(!taken(silent, silent, kOut), "silence is not taken");
    expect(stretch::lowTonalInRange(0.7) && !stretch::lowTonalInRange(1.0) && !stretch::lowTonalInRange(0.1), "only slowing down, and not by very much");
  }
  // ---- the render of a slowed bass ----
  {
    const std::vector<float> x = bass(96000, 6.4, true);
    const SamplePtr s = sampleOf(x, x, 96000);
    const double speed = 0.7, pitch = 0.9117;
    const auto y = rendered(s, speed, pitch, 6.4 / speed);
    bool finite = true;
    for (float v : y) finite = finite && std::isfinite(v);
    expect(finite, "a slowed bass renders finite");
    // Compared with the spectral stretch at the same setting: render through process() (always spectral).
    const float* in[2] = {x.data(), x.data()};
    std::vector<float> sl(y.size());
    float* so[1] = {sl.data()};
    stretch::Settings st;
    st.pitch = pitch * 96000 / kOut;
    stretch::processMapped(in, 1, static_cast<int64_t>(x.size()), so, static_cast<int64_t>(sl.size()), 96000, st,
                           [&](int64_t j) { return static_cast<double>(j) * speed * 96000 / kOut; });
    const double src = crestDb(x, 96000, 0.8 * 2 + 0.1, 0.8 * 3 - 0.1), crest = crestDb(y, kOut, (0.8 * 2 + 0.1) / speed, (0.8 * 3 - 0.1) / speed),
                 spectral = crestDb(sl, kOut, (0.8 * 2 + 0.1) / speed, (0.8 * 3 - 0.1) / speed);
    std::printf("# crest factor: source %.2f dB, low tonal %.2f dB, spectral %.2f dB\n", src, crest, spectral);
    expect(std::fabs(crest - src) < 1.0, "the slowed bass keeps its waveform's crest factor");
    expect(std::fabs(crest - src) < std::fabs(spectral - src), "closer to the source's waveform than the spectral stretch");
    const double level = rmsDb(y, kOut, 1.0 / speed, 5.4 / speed) - rmsDb(x, 96000, 1.0, 5.4);
    std::printf("# level %+.2f dB\n", level);
    expect(std::fabs(level) < 1.0, "the slowed bass keeps its level");
    // In tune: the third note (73.4 Hz in the source).
    const double wantHz = 73.4 * pitch;
    const double hz = fundamentalHz(y, kOut, (0.8 * 2 + 0.15) / speed, (0.8 * 3 - 0.1) / speed, wantHz);
    std::printf("# tuning %.2f cents\n", 1200 * std::log2(hz / wantHz));
    expect(std::fabs(1200 * std::log2(hz / wantHz)) < 3.0, "the slowed bass is in tune");
    // On time: every note's attack within a period of where the map puts it.
    for (int k = 2; k <= 6; ++k) {
      const double t = k * 0.8 / speed;
      const double before = rmsDb(y, kOut, t - 0.06, t - 0.03), after = rmsDb(y, kOut, t + 0.03, t + 0.06);
      expect(after > before - 3.0, ("note " + std::to_string(k) + " sounds where the map puts it").c_str());
    }
    // A render is the same every time (the renders' cache aside).
    stretch::trimClipCache();
    const std::vector<float> x2 = x;
    const auto again = rendered(sampleOf(x2, x2, 96000), speed, pitch, 6.4 / speed);
    expect(again == y, "the same render every time");
  }
  // ---- the stream's edges ----
  {
    const std::vector<float> x = bass(kOut, 2.0, true);
    const float* in[1] = {x.data()};
    const int64_t n = static_cast<int64_t>(x.size());
    const std::function<double(int64_t)> slow = [](int64_t q) { return 0.6 * static_cast<double>(q); };
    // Its windows sum to one: at a rate of 1 it gives back the source.
    std::vector<float> same(x.size(), 0.f);
    float* so[1] = {same.data()};
    stretch::lowTonalStream(in, 1, n, so, n, kOut, [](int64_t q) { return static_cast<double>(q); }, nullptr);
    double most = 0;
    for (size_t i = 2000; i + 4000 < x.size(); ++i) most = std::max(most, static_cast<double>(std::fabs(same[i] - x[i])));
    std::printf("# at a rate of 1: largest difference %.2e\n", most);
    expect(most < 1e-3, "at a rate of 1 the stream gives back its source");
    // What it reports as final stays so.
    std::vector<float> y(static_cast<size_t>(n / 0.6), 0.f), snap(y.size(), 0.f);
    float* yo[1] = {y.data()};
    bool held = true;
    int64_t seen = 0, frozen = 0;
    const std::function<void(int64_t)> pr = [&](int64_t done) {
      held = held && done >= seen;
      seen = done;
      if (!frozen && done > static_cast<int64_t>(y.size()) / 2) {  // what is final half way through
        frozen = done;
        std::copy(y.begin(), y.begin() + done, snap.begin());
      }
    };
    stretch::lowTonalStream(in, 1, n, yo, static_cast<int64_t>(y.size()), kOut, slow, &pr);
    double moved = 0;
    for (int64_t i = 0; i < frozen; ++i) moved = std::max(moved, static_cast<double>(std::fabs(snap[static_cast<size_t>(i)] - y[static_cast<size_t>(i)])));
    expect(held && seen == static_cast<int64_t>(y.size()) && frozen > 0, "progress runs to the end");
    expect(moved == 0.0, "frames reported final stay final");
    // Extremes: a very short input, no input, silence, a map past the end.
    std::vector<float> few(40, 0.5f), out(2000, 0.f);
    const float* fin[1] = {few.data()};
    float* fo[1] = {out.data()};
    stretch::lowTonalStream(fin, 1, 40, fo, 2000, kOut, slow, nullptr);
    std::fill(out.begin(), out.end(), 0.f);
    stretch::lowTonalStream(fin, 1, 0, fo, 2000, kOut, slow, nullptr);
    bool quiet = true;
    for (float v : out) quiet = quiet && v == 0.f;
    expect(quiet, "no input: silence");
    const std::vector<float> zeros(20000, 0.f);
    const float* zin[1] = {zeros.data()};
    std::fill(out.begin(), out.end(), 0.f);
    stretch::lowTonalStream(zin, 1, 20000, fo, 2000, kOut, slow, nullptr);
    quiet = true;
    for (float v : out) quiet = quiet && v == 0.f;
    expect(quiet, "silence in: silence out");
  }
  return finish();
}
