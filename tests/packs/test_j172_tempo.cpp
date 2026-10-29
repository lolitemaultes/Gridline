// Gridline — J-172: tempo and beat-grid regression on a labelled drum corpus generated here
// (the analysis Platter and Seltmann share). Drum & bass at 160..185 BPM used to read as two
// thirds of its tempo (174 -> 116: kick to kick across the bar, 1.5 beats, taken for the beat)
// or as half of it. Every groove below has a known tempo and beat grid; the test checks:
//   - no metrical-level errors other than an octave (no x2/3, x3/2, x4/3 or other ratios),
//   - drum & bass (two-step, roller, jungle, bare kick + snare, with a bass and pads, as a
//     song and as a loop) reads at its own tempo, not half,
//   - straight grooves (four on the floor, rock, boom bap, breaks) read exactly,
//   - the beat grid sits on the beats (phase), and the analysis is deterministic.
// Half-time grooves (snare on 3 only: trap, half-time drum & bass) may read at either octave;
// they must not read at another ratio and their grid must sit on the beats.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../Check.h"
#include "engine/Sample.h"
#include "packs/seltmann/seltmann.h"
#include "seltmann_synth.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace sd = seltsynth::detail;

namespace {

// One bar of 16 steps per voice: 'x' a hit, 'g' a ghost (quiet), '.' nothing.
struct Bar {
  const char* kick;
  const char* snare;
  const char* hats;
};

struct Groove {
  const char* name;
  std::vector<Bar> bars;  // repeated
};

const Groove kFour{"four", {{"x...x...x...x...", "....x.......x...", "..x...x...x...x."}}};
const Groove kRock{"rock", {{"x.......x.x.....", "....x.......x...", "x.x.x.x.x.x.x.x."}}};
const Groove kBoomBap{"boombap", {{"x......x..x.....", "....x.......x...", "x.x.x.x.x.x.x.x."},
                                  {"x..x......xx....", "....x.......x...", "x.x.x.x.x.x.x.x."}}};
const Groove kBreaks{"breaks", {{"x.....x...x.....", "....x.......x.g.", "x.x.x.x.x.x.x.x."}}};
const Groove kTwoStep{"dnb", {{"x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x."}}};
const Groove kRoller{"roller", {{"x.........x.....", "....x..g....x..g", "xxxxxxxxxxxxxxxx"},
                                {"x.x.......x.....", "....x....g..x.g.", "xxxxxxxxxxxxxxxx"}}};
const Groove kJungle{"jungle", {{"x.x.......xx....", "....x..g.g..x..g", "x.x.x.x.x.x.x.x."},
                                {"x.x.......x.....", "....x..x.g..x.gg", "x.x.x.x.x.x.x.x."}}};
const Groove kBare{"dnb-bare", {{"x.........x.....", "....x.......x...", "................"}}};
const Groove kHalf{"half-time", {{"x..x.......x....", "........x.......", "x.x.x.x.x.x.x.x."}}};

struct Item {
  std::string name;
  const Groove* g;
  double bpm;
  int bars;
  bool mix = false;          // sub bass on the kicks (ducked after them) and chord pads
  double lead = 0.05;        // seconds of silence before beat 0
  int introBars = 0;         // pads only (needs mix)
  bool halfOk = false;       // half-time feel: either octave is right
  bool mustExact = false;    // the tempo itself (not an octave)
};

struct Made {
  std::shared_ptr<Sample> s;
  std::vector<double> beats;  // seconds
};

Made make(const Item& it, uint32_t seed) {
  const double rate = 44100, beat = 60.0 / it.bpm, step = beat / 4;
  const int bars = it.introBars + it.bars;
  const double total = it.lead + bars * 4 * beat + 1.0;
  std::vector<float> L(static_cast<size_t>(total * rate), 0.f), R(L.size(), 0.f);
  sd::Rng rng{seed};
  const auto K = sd::kick(rate, 1.0f);
  for (int bar = 0; bar < bars; ++bar) {
    const double b0 = it.lead + bar * 4 * beat;
    if (it.mix) {  // pads: a chord per bar; bass: a sub note from each kick, ducked under it
      for (int n : {57, 60, 64}) sd::add(L, R, b0, rate, sd::tone(rate, sd::midiHz(n + (bar % 2 ? 2 : 0)), 4 * beat, 4, 0.05), 0.9f, 1.0f);
    }
    if (bar < it.introBars) continue;
    const Bar& B = it.g->bars[static_cast<size_t>(bar - it.introBars) % it.g->bars.size()];
    for (int s = 0; s < 16; ++s) {
      const double t = b0 + s * step;
      if (B.kick[s] == 'x') {
        sd::add(L, R, t, rate, K, 1.0f, 1.0f);
        if (it.mix) sd::add(L, R, t + 0.06, rate, sd::tone(rate, sd::midiHz(33), 2.5 * step, 2, 0.14), 1.0f, 1.0f);
      }
      if (B.snare[s] != '.') {
        const float g = B.snare[s] == 'g' ? 0.25f : 0.9f;
        sd::add(L, R, t, rate, sd::snare(rate, rng), g, g);
      }
      if (B.hats[s] == 'x') {
        const float g = s % 4 == 0 ? 0.3f : 0.2f;
        sd::add(L, R, t, rate, sd::hat(rate, rng, 0.04f), g, g);
      }
    }
  }
  float peak = 0;
  for (size_t i = 0; i < L.size(); ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
  for (size_t i = 0; i < L.size(); ++i) L[i] *= 0.8f / peak, R[i] *= 0.8f / peak;
  Made m;
  m.s = std::make_shared<Sample>();
  m.s->sampleRate = rate;
  m.s->left = std::move(L);
  m.s->right = std::move(R);
  for (int k = it.introBars * 4; k < bars * 4; ++k) m.beats.push_back(it.lead + k * beat);
  return m;
}

// The metrical relation of an estimate to the truth (2 % tolerance).
const char* relation(double est, double ref) {
  static const struct {
    const char* n;
    double r;
  } kR[] = {{"exact", 1.0}, {"x2", 2.0}, {"x1/2", 0.5}, {"x2/3", 2.0 / 3.0}, {"x3/2", 1.5}, {"x4/3", 4.0 / 3.0}, {"x3/4", 0.75}};
  for (const auto& r : kR)
    if (std::fabs(est / (ref * r.r) - 1.0) < 0.02) return r.n;
  return "other";
}

// Share of the analysed beats (over the drummed span) within 35 ms of the true grid at the
// analysed level (half-tempo beats may sit on either true beat).
double onBeat(const Made& m, const sm::Analysis& a) {
  if (m.beats.size() < 2) return 0;
  const double period = m.beats[1] - m.beats[0];
  int hit = 0, all = 0;
  for (double f : a.beats) {
    const double t = f / a.sampleRate;
    if (t < m.beats.front() - 0.05 || t > m.beats.back() + 0.05) continue;
    const double ph = (t - m.beats.front()) / period;
    const double err = std::fabs(ph - std::round(ph)) * period;
    ++all;
    hit += err < 0.035 ? 1 : 0;
  }
  return all ? static_cast<double>(hit) / all : 0;
}

}  // namespace

int main() {
  std::vector<Item> items;
  auto add = [&](const Groove& g, double bpm, int bars, bool mustExact, bool halfOk = false) {
    Item it;
    it.g = &g;
    it.bpm = bpm;
    it.bars = bars;
    it.mustExact = mustExact;
    it.halfOk = halfOk;
    char b[64];
    std::snprintf(b, sizeof b, "%s %.0f", g.name, bpm);
    it.name = b;
    items.push_back(it);
    return &items.back();
  };
  // straight grooves across the usual range
  for (double b : {100.0, 115.0, 124.0, 128.0}) add(kFour, b, 12, true);
  for (double b : {80.0, 90.0, 110.0, 132.0}) add(kRock, b, 12, true);
  for (double b : {85.0, 92.0, 98.0}) add(kBoomBap, b, 12, true);
  for (double b : {130.0, 136.0, 140.0}) add(kBreaks, b, 12, true);
  // drum & bass, 160..185: its own tempo, never two thirds or half of it
  for (double b : {160.0, 170.0, 174.0, 180.0}) add(kTwoStep, b, 16, true);
  for (double b : {165.0, 174.0, 176.0}) add(kRoller, b, 16, true);
  for (double b : {170.0, 174.0}) add(kJungle, b, 16, true);
  for (double b : {172.0, 178.0}) add(kBare, b, 16, true);
  // as a song (over 45 s: no loop-length fit) with a sub bass and pads, after a pad intro
  {
    Item* it = add(kTwoStep, 174, 40, true);
    it->mix = true;
    it->introBars = 4;
    it->name = "dnb 174 song with bass + pads";
  }
  {
    Item* it = add(kRoller, 172, 40, true);
    it->mix = true;
    it->lead = 1.5;
    it->name = "roller 172 song, 1.5 s silence first";
  }
  // half-time feels: either octave
  for (double b : {140.0, 150.0, 174.0}) add(kHalf, b, 12, false, true);
  // above 180: four on the floor at its own tempo
  for (double b : {185.0, 190.0}) add(kFour, b, 16, true);

  int exact = 0, octave = 0, wrongLevel = 0, dnb = 0, dnbExact = 0;
  double phaseSum = 0;
  int phaseN = 0;
  uint32_t seed = 172;
  for (const Item& it : items) {
    const Made m = make(it, ++seed);
    sm::Analysis a;
    const bool ok = sm::analyse(*m.s, a);
    const char* rel = relation(a.bpm, it.bpm);
    const bool isExact = !std::strcmp(rel, "exact");
    const bool isOctave = !std::strcmp(rel, "x2") || !std::strcmp(rel, "x1/2");
    const double ph = isExact || isOctave ? onBeat(m, a) : 0.0;
    std::printf("# %-38s est %7.2f  %-6s conf %.2f  on-beat %.2f  meter %d\n", it.name.c_str(), a.bpm, rel, a.confidence, ph, a.beatsPerBar);
    char b[160];
    std::snprintf(b, sizeof b, "%s: analysed", it.name.c_str());
    expect(ok, b);
    exact += isExact;
    octave += isOctave;
    wrongLevel += !isExact && !isOctave;
    if (it.bpm >= 160 && it.bpm <= 185 && !it.halfOk) {
      ++dnb;
      dnbExact += isExact;
    }
    std::snprintf(b, sizeof b, "%s: tempo %.2f is %s (want %s)", it.name.c_str(), a.bpm, rel, it.mustExact ? "exact" : "exact or an octave");
    expect(it.mustExact ? isExact : (isExact || isOctave), b);
    if (isExact || isOctave) {
      phaseSum += ph;
      ++phaseN;
      std::snprintf(b, sizeof b, "%s: beat grid on the beats (%.2f of beats within 35 ms)", it.name.c_str(), ph);
      expect(ph >= 0.9, b);
    }
    if (it.mix && it.bars >= 40) {
      std::snprintf(b, sizeof b, "%s: 4 beats per bar", it.name.c_str());
      expect(a.beatsPerBar == 4, b);
    }
  }
  const int n = static_cast<int>(items.size());
  std::printf("# %d grooves: exact %d, octave %d, other level %d; drum & bass exact %d/%d; mean on-beat %.3f\n", n, exact, octave, wrongLevel,
              dnbExact, dnb, phaseN ? phaseSum / phaseN : 0.0);
  expect(wrongLevel == 0, "no metrical-level errors other than an octave");
  expect(dnbExact == dnb, "drum & bass reads at its own tempo");
  expect(exact >= n - 3, "exact tempo on all but the half-time grooves");

  // deterministic: the same audio gives the same grid
  {
    const Made m = make(items[16], 7);
    sm::Analysis a1, a2;
    sm::analyse(*m.s, a1);
    sm::analyse(*m.s, a2);
    expect(a1.bpm == a2.bpm && a1.beats == a2.beats && a1.bar1 == a2.bar1, "deterministic analysis");
  }
  // the octave controls: halving and doubling the grid keep its phase
  {
    const Made m = make(items[16], 9);
    sm::Analysis a;
    sm::analyse(*m.s, a);
    sm::Analysis h = a;
    sm::gridHalve(h);
    sm::Analysis d = h;
    sm::gridDouble(d);
    expect(std::fabs(h.bpm * 2 - a.bpm) < 1e-9 && std::fabs(d.bpm - a.bpm) < 1e-9, "half / double tempo");
    expect(onBeat(m, h) >= 0.9 && onBeat(m, d) >= 0.9, "half / double keep the grid on the beats");
  }
  return finish();
}
