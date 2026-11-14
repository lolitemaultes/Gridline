// Gridline - Chater (gl.chater): the dreamy glitch, glitter and sparkle effect.
// Breaks fire by chance on the tempo grid and on the input's own hits (no sequencer); every
// break type does what it says at several tempos, block sizes and sample rates; renders
// repeat; Mix 0 is transparent; the latency matches the report; macros move exactly their
// targets; the dice respect locks; Freeze holds the space, Damp darkens the wet path, soft
// breaks have no steps; old states load as their old sound; every preset stays clean on
// five sources of our own (pad, voice, piano, plucked string, drums).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/chater/chater.h"
#include "../Check.h"

using namespace gl;
namespace ch = gl::packs::chater;

namespace {

constexpr double kPi = 3.14159265358979323846;

PluginState initState() {
  PluginState s = defaultPluginState(ch::kType);
  return s;
}
void set(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }

// A state with no breaks, no colour: the plain path (every macro at 0, Mix 100, hard edges).
PluginState plain() {
  PluginState s = initState();
  for (int m = ch::PSparkle; m <= ch::PSpace; ++m) set(s, m, 0);
  set(s, ch::PMix, 1);
  set(s, ch::PDensity, 0);
  set(s, ch::PSoftness, 0), set(s, ch::PWash, 0), set(s, ch::PDamp, 20000), set(s, ch::PWarmth, 0);
  return s;
}

// One break type only, firing on every grid step.
PluginState onlyBreak(int type, int grid = 1, int span = 0) {
  PluginState s = plain();
  for (int i = 0; i < 6; ++i) set(s, ch::POddRepeat + i, i + 1 == type ? 100 : 0);
  set(s, ch::PDensity, 1.0);
  set(s, ch::PGrid, grid);
  set(s, ch::PSpan, span);
  set(s, ch::PVariation, 0);
  set(s, ch::PPanScatter, 0);
  set(s, ch::PSmooth, 0.5);
  set(s, ch::PHaloEngine, ch::HaloOff);
  return s;
}

struct Render {
  double sr = 48000, tempo = 120;
  int block = 512;
  bool playing = true;
  std::vector<float> L, R;
  std::vector<ch::FiredBreak> fired;
};

// Renders `in` (mono, both sides) through Chater with the transport playing from 0.
void render(const PluginState& st, const std::vector<float>& inL, const std::vector<float>& inR, Render& r, Effect* keep = nullptr) {
  auto fxOwn = createEffect(ch::kType);
  Effect* fx = keep ? keep : fxOwn.get();
  const size_t n = inL.size();
  r.L = inL, r.R = inR;
  ProcessContext ctx;
  ctx.sampleRate = r.sr;
  ctx.tempo = r.tempo;
  ctx.ppq = 96;
  ctx.playing = r.playing;
  for (size_t p = 0; p < n;) {
    const int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(r.block), n - p));
    ctx.tick = static_cast<double>(p) / r.sr * r.tempo / 60.0 * ctx.ppq;
    ctx.jumped = p == 0;
    fx->process(st, ctx, r.L.data() + p, r.R.data() + p, len);
    p += static_cast<size_t>(len);
  }
  std::vector<ch::FiredBreak> f(4096);
  f.resize(static_cast<size_t>(ch::firedBreaks(fx, f.data(), 4096)));
  r.fired = f;
}

std::vector<float> sine(size_t n, double sr, double hz, double amp = 0.5) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / sr));
  return v;
}
// A signal whose every sample is distinct and smooth enough to read back by index.
std::vector<float> marker(size_t n, double sr) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    v[i] = static_cast<float>(0.4 * std::sin(2 * kPi * 97 * t) + 0.25 * std::sin(2 * kPi * 13.1 * t + 0.3));
  }
  return v;
}
std::vector<float> noise(size_t n, uint32_t seed, double amp) {
  std::vector<float> v(n);
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    v[i] = static_cast<float>(amp * ((x >> 8) / 8388608.0 - 1.0));
  }
  return v;
}

// Scales a test loop to a peak of 0.5 (-6 dBFS), a typical level on a track.
std::vector<float> normal(std::vector<float> v) {
  float p = 0;
  for (float x : v) p = std::max(p, std::fabs(x));
  if (p > 0)
    for (float& x : v) x *= 0.5f / p;
  return v;
}

// Test loops of our own: drums, a pad and a voice-like source.
std::vector<float> drums(size_t n, double sr, double tempo) {
  std::vector<float> v(n, 0.f);
  const double spb = 60.0 / tempo * sr;
  uint32_t x = 99;
  for (size_t i = 0; i < n; ++i) {
    const double b = static_cast<double>(i) / spb;
    const double inBeat = (b - std::floor(b)) * spb / sr;  // seconds since the beat
    const double inHalf = std::fmod(b, 2.0) * spb / sr;
    const double eighth = (b * 2 - std::floor(b * 2)) * spb / 2 / sr;
    double s = 0;
    s += 0.8 * std::sin(2 * kPi * (50 * inBeat + 60 * (1 - std::exp(-inBeat * 30)) / 30)) * std::exp(-inBeat * 9);  // kick
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    const double nz = (x >> 8) / 8388608.0 - 1.0;
    if (std::fmod(b, 2.0) >= 1.0) s += 0.45 * nz * std::exp(-(inHalf - spb / sr) * 18) * (inHalf >= spb / sr ? 1 : 0);  // snare
    s += 0.12 * nz * std::exp(-eighth * 60);  // hats
    v[i] = static_cast<float>(s * 0.8);
  }
  return normal(v);
}
std::vector<float> pad(size_t n, double sr) {
  std::vector<float> v(n);
  const double f[4] = {220, 261.63, 329.63, 392};
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    double s = 0;
    for (double hz : f) s += std::sin(2 * kPi * hz * t) + 0.3 * std::sin(4 * kPi * hz * t + 0.2);
    v[i] = static_cast<float>(s * 0.09 * std::min(1.0, t * 4));
  }
  return normal(v);
}
std::vector<float> voice(size_t n, double sr) {
  std::vector<float> v(n);
  double ph = 0, f1 = 0, f2 = 0, b1 = 0, b2 = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double hz = 180 * (1 + 0.012 * std::sin(2 * kPi * 5.5 * t)) * (1 + 0.15 * std::sin(2 * kPi * 0.3 * t));
    ph += hz / sr;
    const double src = (ph - std::floor(ph)) * 2 - 1;
    // two resonant formants (a vowel that moves)
    const double fa = 700 + 300 * std::sin(2 * kPi * 0.5 * t), fb = 1200 + 600 * std::sin(2 * kPi * 0.37 * t);
    const double ra = 0.995, rb = 0.993;
    const double ca = 2 * ra * std::cos(2 * kPi * fa / sr), cb = 2 * rb * std::cos(2 * kPi * fb / sr);
    const double ya = src * 0.05 + ca * f1 - ra * ra * f2;
    f2 = f1, f1 = ya;
    const double yb = src * 0.04 + cb * b1 - rb * rb * b2;
    b2 = b1, b1 = yb;
    const double env = 0.5 + 0.5 * std::sin(2 * kPi * 1.1 * t);
    v[i] = static_cast<float>((ya + yb) * 0.25 * env);
  }
  return normal(v);
}

// A piano of our own: decaying slightly inharmonic partials, a chord walk.
std::vector<float> piano(size_t n, double sr) {
  std::vector<float> v(n, 0.f);
  const double notes[8] = {261.63, 329.63, 392.0, 493.88, 220.0, 329.63, 440.0, 523.25};
  const size_t step = static_cast<size_t>(sr * 0.5);
  for (size_t k = 0; k * step < n; ++k) {
    const double f0 = notes[k % 8];
    for (size_t i = k * step; i < n && i < k * step + static_cast<size_t>(sr * 3); ++i) {
      const double t = static_cast<double>(i - k * step) / sr;
      double s = 0;
      for (int h = 1; h <= 8; ++h) {
        const double fh = f0 * h * std::sqrt(1 + 0.0004 * h * h);
        s += std::sin(2 * kPi * fh * t) * std::exp(-t * (1.5 + h * 0.9)) / h;
      }
      v[i] += static_cast<float>(s * std::min(1.0, t * 2000) * 0.2);
    }
  }
  return normal(v);
}
// A plucked string of our own (a delay line with an averaging loop), a new note every 3/8 s.
std::vector<float> pluck(size_t n, double sr) {
  std::vector<float> v(n, 0.f);
  const double notes[6] = {196.0, 246.94, 293.66, 392.0, 329.63, 246.94};
  const size_t step = static_cast<size_t>(sr * 0.375);
  uint32_t x = 4242;
  for (size_t k = 0; k * step < n; ++k) {
    const int len = static_cast<int>(sr / notes[k % 6]);
    std::vector<double> line(static_cast<size_t>(len));
    for (auto& y : line) x ^= x << 13, x ^= x >> 17, x ^= x << 5, y = (x >> 8) / 8388608.0 - 1.0;
    int at = 0;
    for (size_t i = k * step; i < n && i < k * step + static_cast<size_t>(sr * 2); ++i) {
      const int nx = (at + 1) % len;
      const double y = line[static_cast<size_t>(at)];
      line[static_cast<size_t>(at)] = 0.996 * 0.5 * (y + line[static_cast<size_t>(nx)]);
      at = nx;
      v[i] += static_cast<float>(y * 0.3);
    }
  }
  return normal(v);
}

double peak(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  double p = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
  return p;
}
double rms(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  double s = 0;
  size_t c = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) s += static_cast<double>(v[i]) * v[i], ++c;
  return c ? std::sqrt(s / static_cast<double>(c)) : 0;
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
float hermiteAt(const std::vector<float>& v, double idx) {
  const auto i = static_cast<long>(std::floor(idx));
  const float t = static_cast<float>(idx - static_cast<double>(i));
  auto at = [&](long k) { return k >= 0 && static_cast<size_t>(k) < v.size() ? v[static_cast<size_t>(k)] : 0.f; };
  const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
  const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}

// The first sample at or after grid step k (the engine's own rule, beat per sample).
long stepSample(long k, double grid, double sr, double tempo) {
  const double dBeat = tempo / 60.0 / sr;
  double beat = 0;
  long i = 0;
  // closed form, then settle on the engine's floor(beat / grid + 1e-9) rule
  i = static_cast<long>(std::floor(k * grid / dBeat)) - 2;
  if (i < 0) i = 0;
  for (;; ++i) {
    beat = static_cast<double>(i) * dBeat;
    if (std::floor(beat / grid + 1e-9) >= static_cast<double>(k)) return i;
  }
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin(ch::kType);
  expect(info != nullptr, "Chater is in the catalogue");
  if (!info) return finish();
  expect(info->kind == PluginKind::Effect && !info->midiInput, "Chater is an audio effect with no note input");
  expect(info->params.size() == static_cast<size_t>(ch::PCount), "parameter count matches the enum");
  expect(info->editorWidth == 1100, "editor is 1100 px wide");
  expect(info->presets.size() == 41 && ch::presetMeta().size() == 41, "Init plus 40 factory presets");
  {
    bool namesMatch = true;
    int perBank[ch::kBanks] = {};
    for (size_t i = 0; i < info->presets.size(); ++i) {
      namesMatch = namesMatch && info->presets[i].name == ch::presetMeta()[i].name;
      if (ch::presetMeta()[i].bank >= 0) ++perBank[ch::presetMeta()[i].bank];
    }
    bool five = true;
    for (int b : perBank) five = five && b == 5;
    expect(namesMatch && five && ch::kBanks == 8, "presets: notes line up, eight banks of five (by source)");
    expect(info->presets[0].name == "Init", "the first preset is Init");
  }
  for (int k = 0; k < ch::PCount; ++k) {
    const std::string n = info->params[static_cast<size_t>(k)].name;
    if (n.find("Step") != std::string::npos || n.find("Swing") != std::string::npos || n.find("Note") != std::string::npos) {
      expect(false, ("no sequencer or note parameter: " + n).c_str());
    }
  }

  // ---- silence in, silence out with every block on
  {
    PluginState s = initState();
    for (int m = ch::PSparkle; m <= ch::PSpace; ++m) set(s, m, 1.0);
    set(s, ch::PGrLevel, 1), set(s, ch::PShFeedback, 0.95), set(s, ch::PCrMix, 1), set(s, ch::PHaLift, 1), set(s, ch::PHaTwinkle, 1);
    set(s, ch::PHiss, 1), set(s, ch::PCrackle, 1), set(s, ch::PWow, 1), set(s, ch::PEnsemble, 1), set(s, ch::PAge, 1), set(s, ch::PRmShimmer, 1);
    set(s, ch::PRmMix, 1), set(s, ch::PEcMix, 1), set(s, ch::PEcFeedback, 0.95), set(s, ch::PGtDrive, 24), set(s, ch::PDensity, 1);
    set(s, ch::PHaloEngine, ch::HaloFull);
    Render r;
    std::vector<float> z(48000 * 3, 0.f);
    render(s, z, z, r);
    expect(peak(r.L) == 0 && peak(r.R) == 0, "silence in gives silence out (every block on)");
  }

  // ---- transparency: Mix 0 (no breaks firing), and the plain path at Mix 100
  for (int engine = 0; engine < 3; ++engine) {
    PluginState s = plain();
    set(s, ch::PHaloEngine, engine);
    set(s, ch::PMix, 0);
    set(s, ch::PSparkle, 1), set(s, ch::PShine, 1), set(s, ch::PSpace, 1), set(s, ch::PHaze, 1), set(s, ch::PMemory, 1), set(s, ch::PGlitch, 0);
    set(s, ch::PHiss, 1), set(s, ch::PCrackle, 1), set(s, ch::PWarmth, 1), set(s, ch::PRmFreeze, 1);
    const int lat = createEffect(ch::kType)->latencyFor(s, 48000);
    Render r;
    const auto in = noise(48000 * 2, 5, 0.5);
    render(s, in, in, r);
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) {
      const float want = i >= static_cast<size_t>(lat) ? in[i - static_cast<size_t>(lat)] : 0.f;
      same = same && r.L[i] == want && r.R[i] == want;
    }
    expect(same, ("Mix 0 is transparent (input delayed by the reported latency), Halo engine " + std::to_string(engine)).c_str());
    set(s, ch::PMix, 1);
    for (int m = ch::PSparkle; m <= ch::PSpace; ++m) set(s, m, 0);
    set(s, ch::PHiss, 0), set(s, ch::PCrackle, 0), set(s, ch::PWarmth, 0), set(s, ch::PRmFreeze, 0);
    render(s, in, in, r);
    same = true;
    for (size_t i = 0; i < in.size(); ++i) {
      const float want = i >= static_cast<size_t>(lat) ? in[i - static_cast<size_t>(lat)] : 0.f;
      same = same && std::fabs(r.L[i] - want) < 1e-6f;
    }
    expect(same, ("with nothing on, Mix 100 is transparent too, Halo engine " + std::to_string(engine)).c_str());
  }

  // ---- grid chance at 100 % fires exactly on each division
  {
    const double tempos[] = {120, 97.3, 174};
    const double rates[] = {44100, 48000, 96000};
    const int blocks[] = {1, 64, 1000};
    bool exact = true, all = true;
    int runs = 0;
    for (double tempo : tempos)
      for (double sr : rates)
        for (int bs : blocks) {
          if (bs == 1 && sr > 48000) continue;
          Render r;
          r.sr = sr, r.tempo = tempo, r.block = bs;
          PluginState s = onlyBreak(ch::BrSkip, 3, 0);  // 1/16, one step each
          const auto in = sine(static_cast<size_t>(sr * 2), sr, 220);
          render(s, in, in, r);
          const double grid = ch::gridBeats(3);
          const long steps = static_cast<long>(std::floor(2.0 * tempo / 60.0 / grid - 1));
          all = all && static_cast<long>(r.fired.size()) >= steps;
          for (size_t k = 0; k < r.fired.size(); ++k) {
            const long want = stepSample(static_cast<long>(k), grid, sr, tempo);
            if (r.fired[k].frame != want) exact = false;
          }
          ++runs;
        }
    expect(exact, "grid chance 100 %: every break starts on its step's first sample (3 tempos, 3 rates, 3 block sizes)");
    expect(all && runs > 20, "grid chance 100 %: every step fires");
  }

  // ---- Lean, Density 0, Hits only
  {
    Render r;
    PluginState s = onlyBreak(ch::BrSkip, 3, 0);
    set(s, ch::PDensity, 0);
    render(s, sine(48000, 48000, 220), sine(48000, 48000, 220), r);
    expect(r.fired.empty(), "Density 0 never fires on the grid");
    s = onlyBreak(ch::BrSkip, 3, 0);
    set(s, ch::PDensity, 0.3), set(s, ch::PLean, ch::LeanOff);
    render(s, sine(48000 * 8, 48000, 220), sine(48000 * 8, 48000, 220), r);
    int on = 0, off = 0;
    for (const auto& f : r.fired) {
      const double inBeat = f.beat - std::floor(f.beat + 1e-9);
      (inBeat < 1e-6 ? on : off)++;
    }
    expect(off > on * 2, "Lean Offbeats puts most breaks off the beat");
    s = onlyBreak(ch::BrSkip, 3, 0);
    set(s, ch::PTrigMode, ch::TrigHits);
    render(s, sine(48000 * 2, 48000, 220), sine(48000 * 2, 48000, 220), r);
    expect(r.fired.empty(), "Hits mode never fires on the grid (a steady tone has no hits)");
  }

  // ---- the onset trigger fires on clicks
  {
    const double sr = 48000;
    std::vector<float> in(static_cast<size_t>(sr * 4), 0.f);
    std::vector<long> clicks;
    for (int c = 0; c < 7; ++c) {
      const long at = static_cast<long>(sr * (0.25 + c * 0.53));
      clicks.push_back(at);
      for (int i = 0; i < 400; ++i) in[static_cast<size_t>(at + i)] = static_cast<float>(0.8 * std::exp(-i / 60.0) * std::sin(i * 0.9));
    }
    PluginState s = onlyBreak(ch::BrRepeat, 3, 0);
    set(s, ch::PTrigMode, ch::TrigHits), set(s, ch::PHitChance, 1.0), set(s, ch::PSensitivity, 0.5);
    for (int bs : {64, 333}) {
      Render r;
      r.sr = sr, r.block = bs;
      render(s, in, in, r);
      bool ok = r.fired.size() == clicks.size();
      for (size_t i = 0; ok && i < clicks.size(); ++i) ok = r.fired[i].hit && std::labs(r.fired[i].frame - clicks[i]) < static_cast<long>(sr * 0.003);
      expect(ok, ("the onset trigger fires once on each click, within 3 ms (block " + std::to_string(bs) + ")").c_str());
    }
    // Both: grid and hits together
    set(s, ch::PTrigMode, ch::TrigBoth), set(s, ch::PDensity, 0.0);
    Render r;
    render(s, in, in, r);
    expect(r.fired.size() == clicks.size(), "Both mode still fires on hits with the grid at 0");
  }

  // ---- every break type does what it says (tempos, block sizes, sample rates)
  {
    struct Case {
      double sr, tempo;
      int block;
    };
    const Case cases[] = {{48000, 120, 512}, {44100, 97, 64}, {96000, 140, 1000}};
    for (const Case& c : cases) {
      const std::string tag = " (" + std::to_string(static_cast<int>(c.sr)) + " Hz, " + std::to_string(static_cast<int>(c.tempo)) + " BPM, block " + std::to_string(c.block) + ")";
      const size_t n = static_cast<size_t>(c.sr * 3);
      const auto in = marker(n, c.sr);
      const double spb = 60.0 / c.tempo * c.sr;
      auto run = [&](int type, PluginState s) {
        Render r;
        r.sr = c.sr, r.tempo = c.tempo, r.block = c.block;
        render(s, in, in, r);
        (void)type;
        return r;
      };
      const int margin = static_cast<int>(0.0015 * c.sr) + 4;
      // Skip
      {
        Render r = run(ch::BrSkip, onlyBreak(ch::BrSkip, 1, 0));
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          ok = peak(r.L, static_cast<size_t>(s0 + margin), static_cast<size_t>(s0 + static_cast<long>(spb) - margin)) < 1e-6;
        }
        expect(ok, ("Skip is silent between its fades" + tag).c_str());
      }
      // Reverse
      {
        Render r = run(ch::BrReverse, onlyBreak(ch::BrReverse, 1, 0));
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          for (long el = margin; ok && el < static_cast<long>(spb) - margin; el += 97) ok = std::fabs(r.L[static_cast<size_t>(s0 + el)] - in[static_cast<size_t>(s0 - 1 - el)]) < 1e-5;
        }
        expect(ok, ("Reverse plays the capture backwards from where it fired" + tag).c_str());
      }
      // Repeat (1/16 slices in a 1/4 break)
      {
        PluginState s = onlyBreak(ch::BrRepeat, 1, 0);
        set(s, ch::PDivision, 3);
        Render r = run(ch::BrRepeat, s);
        const double slice = 0.25 * spb;
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          ok = r.fired[k].repeats == 4;
          // the slice's read point: live through the first slice, then from the slice's start again
          double sp = 0, sl = slice;
          int rep = 0;
          for (long el = 0; ok && el < static_cast<long>(spb) - margin; ++el) {
            if (rep >= 1 && sp > margin && sp < sl - margin && el % 7 == 0)
              ok = std::fabs(r.L[static_cast<size_t>(s0 + el)] - hermiteAt(in, static_cast<double>(s0) + sp)) < 1e-4;
            sp += 1;
            if (sp >= sl) {
              sp -= sl;
              if (rep == 0) sp = 0;
              ++rep;
            }
          }
        }
        expect(ok, ("Repeat plays the first slice again, four times in a beat" + tag).c_str());
      }
      // Jump: a whole number of Quantise steps back
      {
        PluginState s = onlyBreak(ch::BrJump, 1, 0);
        set(s, ch::PQuantise, 2);  // 1/16
        Render r = run(ch::BrJump, s);
        const double q = 0.25 * spb;
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          // find the offset that matches the middle of the break
          const long mid = s0 + static_cast<long>(spb / 2);
          bool found = false;
          for (int m = 1; m <= 64 && !found; ++m) {
            const long off = static_cast<long>(std::max<int64_t>(16, static_cast<int64_t>(q))) * m;
            if (mid - off < 0) break;
            bool match = true;
            for (long j = -200; match && j <= 200; j += 20) match = std::fabs(r.L[static_cast<size_t>(mid + j)] - in[static_cast<size_t>(mid + j - off)]) < 1e-5;
            found = match;
          }
          ok = found;
        }
        expect(ok, ("Jump plays a slice a whole number of Quantise steps back" + tag).c_str());
      }
      // Tape: the read point slows to a stop over Tape time (linear curve)
      {
        PluginState s = onlyBreak(ch::BrTape, 1, 0);
        set(s, ch::PTapeTime, 2), set(s, ch::PTapeCurve, 0.0);  // one beat, straight
        Render r = run(ch::BrTape, s);
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          const double T = std::max(64.0, std::min(static_cast<double>(r.fired[k].beats * spb), spb));
          for (long el = margin; ok && el < static_cast<long>(T * 0.9); el += 211) {
            const double pos = el - static_cast<double>(el) * (el - 1) / (2 * T);
            ok = std::fabs(r.L[static_cast<size_t>(s0 + el)] - hermiteAt(in, s0 - 2 + pos)) < 3e-3;
          }
        }
        expect(ok, ("Tape slows the read point to a stop (a tape stop)" + tag).c_str());
      }
      // Scrub: the read point swings around the start
      {
        PluginState s = onlyBreak(ch::BrScrub, 1, 0);
        set(s, ch::PScrubDepth, 0.5), set(s, ch::PScrubRate, 2);
        Render r = run(ch::BrScrub, s);
        bool ok = r.fired.size() > 3;
        for (size_t k = 1; ok && k + 1 < r.fired.size(); ++k) {
          const long s0 = r.fired[k].frame;
          const double len = r.fired[k].beats * spb;
          const double D = 0.5 * std::round(len), period = 0.5 * spb;
          for (long el = margin; ok && el < static_cast<long>(len) - margin; el += 173) {
            const double idx = s0 - D * 0.5 + D * 0.5 * std::sin(2 * kPi * el / period);
            ok = std::fabs(r.L[static_cast<size_t>(s0 + el)] - hermiteAt(in, idx)) < 3e-3;
          }
        }
        expect(ok, ("Scrub swings the read point around where it fired" + tag).c_str());
      }
    }
  }

  // ---- repeatable renders, the seed matters, and play restarts the dice
  {
    PluginState s = presetState(*info, 7);  // Shredder: busy
    const auto in = drums(48000 * 4, 48000, 120);
    Render a, b, c;
    render(s, in, in, a);
    render(s, in, in, b);
    expect(a.L == b.L && a.R == b.R && a.fired.size() == b.fired.size(), "renders repeat exactly");
    set(s, ch::PSeed, 42);
    render(s, in, in, c);
    expect(c.L != a.L, "another Seed gives other breaks");
    // the same instance played twice from the start gives the same result
    auto fx = createEffect(ch::kType);
    s = presetState(*info, 7);
    Render d, e;
    render(s, in, in, d, fx.get());
    render(s, in, in, e, fx.get());
    bool sameBreaks = d.fired.size() * 2 == e.fired.size();
    expect(sameBreaks, "play again: the same breaks fire (the dice restart on play)");
  }

  // ---- block-size invariance with the whole chain busy
  {
    for (int preset : {1, 9, 22, 26, 33, 37}) {
      PluginState s = presetState(*info, static_cast<size_t>(preset));
      std::vector<float> in = drums(48000 * 2, 48000, 120);
      const auto p = pad(in.size(), 48000);
      for (size_t i = 0; i < in.size(); ++i) in[i] = in[i] * 0.7f + p[i] * 0.5f;
      Render ref;
      ref.block = 4096;
      render(s, in, in, ref);
      bool same = true;
      for (int bs : {1, 37, 512}) {
        Render r;
        r.block = bs;
        render(s, in, in, r);
        for (size_t i = 0; i < in.size() && same; ++i) same = std::fabs(r.L[i] - ref.L[i]) < 1e-5f && std::fabs(r.R[i] - ref.R[i]) < 1e-5f;
      }
      expect(same, ("block-size invariant (1, 37, 512, 4096 frames): " + info->presets[static_cast<size_t>(preset)].name).c_str());
    }
  }

  // ---- parameter extremes stay finite
  {
    const auto in = drums(48000 * 2, 48000, 128);
    PluginState lo = initState(), hi = initState();
    for (int k = 0; k < ch::PCount; ++k) {
      const ParamDesc& d = info->params[static_cast<size_t>(k)];
      if (k >= ch::PLock0 && k < ch::PVersion) continue;
      set(lo, k, d.kind == ParamKind::Choice ? 0 : d.min);
      set(hi, k, d.kind == ParamKind::Choice ? static_cast<double>(d.choices.size() - 1) : d.max);
    }
    for (int l = 0; l < ch::kLinks; ++l) set(hi, ch::linkParam(l, ch::LkTarget), ch::linkTargets()[static_cast<size_t>(l * 5)]);
    Render a, b;
    render(lo, in, in, a);
    render(hi, in, in, b);
    expect(finite(a.L) && finite(a.R), "every parameter at its minimum: finite output");
    expect(finite(b.L) && finite(b.R) && peak(b.L) <= 1.0 && peak(b.R) <= 1.0, "every parameter at its maximum: finite, the safety limit holds");
    bool ok = true;
    uint32_t x = 7;
    for (int round = 0; round < 6 && ok; ++round) {
      PluginState s = initState();
      for (int k = 0; k < ch::PCount; ++k) {
        if (k >= ch::PLock0) continue;
        const ParamDesc& d = info->params[static_cast<size_t>(k)];
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        set(s, k, d.fromNormalized((x >> 8) / 16777216.0));
      }
      Render r;
      r.block = 256 + round * 77;
      render(s, in, in, r);
      ok = finite(r.L) && finite(r.R);
    }
    expect(ok, "random parameter fuzz: finite output");
  }

  // ---- normalizePluginState on empty, short and old vectors
  {
    PluginState s;
    s.type = ch::kType;
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(ch::PCount) && s.params[ch::PMix] == 0.4f && s.params[ch::PHaloEngine] == ch::HaloLow &&
               s.params[ch::PVersion] == ch::kVersion,
           "an empty state gets every default");
    s.params.assign(10, 0.3f);
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(ch::PCount) && std::fabs(s.params[ch::PSparkle] - 0.3f) < 1e-6f && s.params[ch::PVersion] == ch::kVersion,
           "a short state keeps its values and pads the rest with defaults");
    s.params[ch::PGrid] = 99, s.params[ch::PDensity] = std::nanf("");
    normalizePluginState(s);
    expect(s.params[ch::PGrid] == ch::kGridCount - 1 && s.params[ch::PDensity] == 0.04f, "out of range and NaN values are clamped or reset");
    auto fx = createEffect(ch::kType);
    PluginState shortSt;
    shortSt.type = ch::kType;
    shortSt.params.assign(5, 0.5f);
    std::vector<float> l(2048, 0.1f), r(2048, 0.1f);
    ProcessContext ctx;
    ctx.sampleRate = 48000;
    fx->process(shortSt, ctx, l.data(), r.data(), 2048);
    expect(finite(l) && finite(r), "the engine runs on a short state without normalising it first");
  }

  // ---- macros move exactly their mapped targets
  {
    PluginState s = initState();
    std::vector<double> page(ch::PCount);
    for (int k = 0; k < ch::PCount; ++k) page[static_cast<size_t>(k)] = s.params[static_cast<size_t>(k)];
    set(s, ch::PGrDensity, 24);
    bool exact = true, zero = true, full = true;
    for (int m = ch::PSparkle; m <= ch::PSpace; ++m) {
      double mz[8] = {}, mo[8] = {};
      mo[m] = 1.0;
      for (int k = 0; k < ch::PCount; ++k) {
        const double v = page[static_cast<size_t>(k)];
        const double at0 = ch::applyMacros(k, v, mz), at1 = ch::applyMacros(k, v, mo);
        if (at0 != v) zero = false;
        bool mapped = false;
        for (const auto& r : ch::macroRows())
          if (r.macro == m && r.param == k && r.kind != ch::MkInner) mapped = true;
        if (!mapped && at1 != v) exact = false;
        if (mapped && at1 == v && k != ch::PGtBits && k != ch::PGtRate) {
          // a mapped target may already sit at its limit; nothing else may stay put
          const ParamDesc& d = info->params[static_cast<size_t>(k)];
          if (v > d.min && v < d.max) full = false;
        }
      }
    }
    expect(zero, "macros at 0 leave every page value as it is");
    expect(exact, "a macro moves only the parameters on its list");
    expect(full, "a macro at 100 moves every parameter on its list");
    double mo[8] = {};
    mo[ch::PSparkle] = 1;
    expect(std::fabs(ch::applyMacros(ch::PGrLevel, 0.1, mo) - 0.8) < 1e-9, "Sparkle at 100 adds its full span to Glitter level");
    mo[ch::PSparkle] = 0.2;
    expect(ch::applyMacros(ch::PShFeedback, 0.0, mo) == 0.0, "Shimmer feedback only starts above 30 % Sparkle");
    double mg[8] = {};
    mg[ch::PGlitch] = 1;
    expect(std::fabs(ch::applyMacros(ch::PGtBits, 24, mg) - 6) < 1e-6 && std::fabs(ch::applyMacros(ch::PGtRate, 48000, mg) - 4000) < 1e-3,
           "Glitch at 100 takes Bits to 6 and Rate to 4 kHz");
    expect(ch::applyMacros(ch::PDivision, 3, mg) == 6, "Glitch at 100 cuts the division three steps finer");
    expect(ch::applyMacros(ch::PSoftness, 0.4, mg) < 0.05 && ch::applyMacros(ch::PWash, 0.15, mg) < 0.05, "Glitch at 100 takes the edges hard and the wash out");
    double mh[8] = {};
    mh[ch::PHaze] = 1;
    expect(std::fabs(ch::applyMacros(ch::PDamp, 20000, mh) - 2500) < 1, "Haze at 100 brings Damp down to 2.5 kHz");
    mh[ch::PHaze] = 0.5;
    const double half = ch::applyMacros(ch::PDamp, 20000, mh);
    expect(std::fabs(half - std::sqrt(20000.0 * 2500.0)) < 5, "Haze moves Damp evenly in pitch (half way = the geometric middle)");
    // the engine follows: Mix tile = Dry/Wet
    PluginState t = plain();
    set(t, ch::PHaloEngine, 0);
    set(t, ch::PMix, 0.0);
    set(t, ch::PCrMix, 1), set(t, ch::PCrStrike, 1);
    Render a;
    const auto in = drums(48000, 48000, 120);
    render(t, in, in, a);
    bool dry = true;
    for (size_t i = 0; i < in.size(); ++i) dry = dry && a.L[i] == in[i];
    set(t, ch::PMix, 1.0);
    Render b;
    render(t, in, in, b);
    expect(dry && b.L != in, "the Mix tile is the Dry/Wet: 0 keeps the dry beat, 100 brings in the crystal");
  }

  // ---- latency: reported equals the impulse delay
  for (int engine = 0; engine < 3; ++engine)
    for (bool active : {false, true}) {
      PluginState s = plain();
      set(s, ch::PHaloEngine, engine);
      if (active) set(s, ch::PHaLift, 0.6), set(s, ch::PHaFrom, 2000);
      for (double sr : {44100.0, 96000.0}) {
        const int lat = createEffect(ch::kType)->latencyFor(s, sr);
        std::vector<float> in(static_cast<size_t>(sr), 0.f);
        in[1000] = 1.0f;
        Render r;
        r.sr = sr, r.playing = false;
        render(s, in, in, r);
        size_t at = 0;
        float best = 0;
        for (size_t i = 0; i < in.size(); ++i)
          if (std::fabs(r.L[i]) > best) best = std::fabs(r.L[i]), at = i;
        const int want = engine == 0 ? 0 : engine == 1 ? 512 : 1024;
        expect(lat == want && static_cast<int>(at) - 1000 == lat,
               ("latency reported (" + std::to_string(lat) + ") equals the impulse delay, Halo engine " + std::to_string(engine) + (active ? " lifting" : " idle") +
                " at " + std::to_string(static_cast<int>(sr)) + " Hz")
                   .c_str());
      }
    }

  // ---- the dice
  {
    PluginState s = presetState(*info, 1);
    ch::setLocked(s.params, ch::PSparkle, true);
    ch::setLocked(s.params, ch::PShine, true);
    ch::setLocked(s.params, ch::PMix, true);
    expect(ch::locked(s.params, ch::PShine) && !ch::locked(s.params, ch::PGlitch), "locks are saved per parameter in the state");
    bool lockedSame = true, nudge = true, scopesKept = true, changed = false;
    for (uint32_t seed = 1; seed < 40; ++seed) {
      const auto out = ch::roll(s.params, ch::ScMacros | ch::ScBreaks | ch::ScGlitter, ch::DiceNudge, seed, 0);
      for (int k : {ch::PSparkle, ch::PShine, ch::PMix}) lockedSame = lockedSame && out[static_cast<size_t>(k)] == s.params[static_cast<size_t>(k)];
      for (int k = 0; k < ch::PCount; ++k) {
        const ParamDesc& d = info->params[static_cast<size_t>(k)];
        const double a = d.toNormalized(s.params[static_cast<size_t>(k)]), b = d.toNormalized(out[static_cast<size_t>(k)]);
        if (d.kind == ParamKind::Continuous && std::fabs(a - b) > 0.1 + 1e-6) nudge = false;
        if (out[static_cast<size_t>(k)] != s.params[static_cast<size_t>(k)]) {
          changed = true;
          const int sc = ch::diceScope(k);
          if (!(sc & (ch::ScMacros | ch::ScBreaks | ch::ScGlitter))) scopesKept = false;
        }
      }
      if (out[ch::POutput] != s.params[ch::POutput] || out[ch::PSafety] != s.params[ch::PSafety]) scopesKept = false;
    }
    expect(lockedSame, "the dice never move a locked knob");
    expect(nudge, "Nudge stays within 10 % of each knob's travel");
    expect(scopesKept && changed, "the dice move only the chosen scopes, never Output or the safety limit");
    const auto one = ch::roll(s.params, 0, ch::DiceNew, 5, -1, ch::PGlitch);
    int moved = 0;
    for (int k = 0; k < ch::PCount; ++k) moved += one[static_cast<size_t>(k)] != s.params[static_cast<size_t>(k)];
    expect(moved == 1, "Roll this knob only moves that knob");
    const auto a1 = ch::roll(s.params, ch::ScMacros, ch::DiceShake, 9, 0), a2 = ch::roll(s.params, ch::ScMacros, ch::DiceShake, 9, 0);
    expect(a1 == a2, "a roll is repeatable for its seed");
  }

  // ---- presets: clean levels on five sources, each sounds different
  {
    const double sr = 48000;
    const size_t n = static_cast<size_t>(sr * 7);
    const auto d = drums(n, sr, 124), p = pad(n, sr), v = voice(n, sr), pi = piano(n, sr), pl = pluck(n, sr);
    std::vector<std::vector<float>> outs;
    bool clean = true, alive = true, settled = true, sane = true;
    std::printf("# preset              pad pk/dB     voice pk/dB   piano pk/dB   pluck pk/dB   drums pk/dB  (dB = output RMS vs bypass)\n");
    for (size_t i = 0; i < info->presets.size(); ++i) {
      PluginState s = presetState(*info, i);
      std::printf("# %-18s", info->presets[i].name.c_str());
      for (int src = 0; src < 5; ++src) {
        const auto& in = src == 0 ? p : src == 1 ? v : src == 2 ? pi : src == 3 ? pl : d;
        Render r;
        r.tempo = 124;
        render(s, in, in, r);
        const double pk = std::max(peak(r.L), peak(r.R)), rm = 0.5 * (rms(r.L) + rms(r.R)), rin = rms(in);
        const double db = 20 * std::log10(std::max(1e-9, rm / rin));
        std::printf("  %5.2f/%+5.1f", pk, db);
        if (!(pk < 0.995) || !finite(r.L) || !finite(r.R)) clean = false;
        if (rm < rin * 0.15) alive = false;
        if (db < -12 || db > 6) sane = false;
        // no runaway: the last two seconds are no louder than the loudest earlier stretch
        const double late = rms(r.L, n - static_cast<size_t>(sr * 2)), early = rms(r.L, static_cast<size_t>(sr * 2), n - static_cast<size_t>(sr * 2));
        if (late > early * 1.8 + 0.02) settled = false;
        if (src == 4) outs.push_back(r.L);
      }
      std::printf("\n");
    }
    expect(clean, "every preset stays finite and under full scale on a pad, a voice, a piano, a plucked string and drums");
    expect(sane, "every preset stays within -12 dB and +6 dB of the bypass level on every source");
    expect(alive, "every preset keeps the sound alive (no preset kills the signal)");
    expect(settled, "no preset runs away (feedback settles)");
    bool distinct = true;
    for (size_t a = 0; a < outs.size(); ++a)
      for (size_t b = a + 1; b < outs.size(); ++b) {
        double diff = 0, ref = 0;
        for (size_t i = 0; i < outs[a].size(); i += 7) diff += std::fabs(outs[a][i] - outs[b][i]), ref += std::fabs(outs[a][i]);
        if (diff < ref * 0.05) distinct = false;
      }
    expect(distinct, "every preset sounds different from every other");
  }

  // ---- Freeze holds the space; without it the same tail dies away
  {
    auto tail = [&](bool freeze) {
      PluginState a = plain();
      set(a, ch::PHaloEngine, ch::HaloOff), set(a, ch::PRmMix, 1), set(a, ch::PRmDecay, 1.5), set(a, ch::PRmMod, 0.5);
      PluginState b = a;
      set(b, ch::PRmFreeze, freeze ? 1 : 0);
      auto fx = createEffect(ch::kType);
      ProcessContext ctx;
      ctx.sampleRate = 48000, ctx.tempo = 120, ctx.playing = false;
      const auto in = pad(48000 / 256 * 256, 48000);  // whole blocks
      std::vector<float> l(256), r(256), out;
      for (size_t p = 0; p < in.size(); p += 256) {
        std::copy(in.begin() + static_cast<long>(p), in.begin() + static_cast<long>(p + 256), l.begin());
        r = l;
        fx->process(a, ctx, l.data(), r.data(), 256);
      }
      for (int k = 0; k < 48000 * 4 / 256; ++k) {
        std::fill(l.begin(), l.end(), 0.f), std::fill(r.begin(), r.end(), 0.f);
        fx->process(b, ctx, l.data(), r.data(), 256);
        out.insert(out.end(), l.begin(), l.end());
      }
      return std::pair<double, double>(rms(out, 4800, 24000), rms(out, 48000 * 3, 48000 * 4));
    };
    const auto f = tail(true), g = tail(false);
    std::printf("# freeze: early %.4f late %.4f; open: early %.4f late %.5f\n", f.first, f.second, g.first, g.second);
    expect(f.first > 0.01 && f.second > f.first * 0.5, "Freeze holds the room's tail (still there three seconds later)");
    expect(g.second < g.first * 0.02, "without Freeze the same tail dies away");
  }

  // ---- Damp takes the highs out of the wet path (and leaves the dry alone)
  {
    PluginState s = plain();
    set(s, ch::PHaloEngine, ch::HaloOff);
    const auto in = noise(48000, 3, 0.4);
    Render open, damped;
    render(s, in, in, open);
    set(s, ch::PDamp, 2000);
    render(s, in, in, damped);
    auto hf = [](const std::vector<float>& v) {
      double e = 0;
      for (size_t i = 1; i < v.size(); ++i) e += (v[i] - v[i - 1]) * (v[i] - v[i - 1]);
      return std::sqrt(e / static_cast<double>(v.size()));
    };
    std::printf("# damp: highs %.4f open, %.4f damped\n", hf(open.L), hf(damped.L));
    expect(hf(damped.L) < hf(open.L) * 0.25 && rms(damped.L) > rms(open.L) * 0.2, "Damp 2 kHz takes most of the highs out, the body stays");
    set(s, ch::PMix, 0);
    Render dry;
    render(s, in, in, dry);
    expect(dry.L == in, "Damp never touches the dry signal");
  }

  // ---- soft breaks: no steps at their edges, slices or loop points
  {
    const auto in = sine(48000 * 3, 48000, 220, 0.5);
    double hardMax = 0;
    bool smooth = true;
    for (int type = ch::BrRepeat; type <= ch::BrSkip; ++type) {
      if (type == ch::BrScrub) continue;  // a scrub really speeds the sound up: its slopes are the pitch, not a step
      for (double soft : {0.0, 1.0}) {
        PluginState s = onlyBreak(type, 1, 1);
        set(s, ch::PSoftness, soft);
        set(s, ch::PSmooth, 0.5);
        Render r;
        render(s, in, in, r);
        double mx = 0;
        for (size_t i = 1; i < r.L.size(); ++i) mx = std::max(mx, static_cast<double>(std::fabs(r.L[i] - r.L[i - 1])));
        if (soft == 0) hardMax = std::max(hardMax, mx);
        else if (mx > 0.025) smooth = false;
        std::printf("# soft breaks: type %d soft %.0f, largest step %.4f (%zu fired)\n", type, soft, mx, r.fired.size());
      }
    }
    expect(smooth, "Soft edges 100: no break makes a step bigger than a 220 Hz sine's own slope allows");
  }

  // ---- wash: a washed break leaves a tail in the space after it ends
  {
    PluginState s = onlyBreak(ch::BrSkip, 1, 0);
    set(s, ch::PDensity, 1), set(s, ch::PRmMix, 0), set(s, ch::PSoftness, 0);
    const auto in = pad(48000 * 2, 48000);
    Render a, b;
    render(s, in, in, a);
    set(s, ch::PWash, 1);
    render(s, in, in, b);
    double ea = 0, eb = 0;
    for (const auto& f : a.fired) {
      const size_t at = static_cast<size_t>(f.frame) + 2000;
      if (at + 2000 < in.size()) ea += rms(a.L, at, at + 2000);
    }
    for (const auto& f : b.fired) {
      const size_t at = static_cast<size_t>(f.frame) + 2000;
      if (at + 2000 < in.size()) eb += rms(b.L, at, at + 2000);
    }
    expect(!b.fired.empty() && eb > ea * 1.2, "Wash keeps sound under a Skip (part of the input, sent into the space)");
  }

  // ---- states saved before the dreamy rework load as their old sound
  {
    std::vector<float> old = initState().params;
    old[ch::PVersion] = 1;
    old[1] = 0.7f, old[2] = 0.2f, old[5] = 0.6f;  // the old Glitch, Shatter and Grit tiles
    old[ch::PSoftness] = 0.9f;
    expect(ch::needsMigration(old.data(), old.size()), "a version 1 state is recognised");
    std::vector<float> m = old;
    ch::migrateV1(m.data(), m.size());
    expect(std::fabs(m[ch::PGlitch] - 0.7f) < 1e-6f && m[ch::PHaze] == 0 && m[ch::PMemory] == 0 && m[ch::PSoftness] == 0 && m[ch::PWash] == 0 &&
               m[ch::PDamp] == 20000 && m[ch::PVersion] == ch::kVersion && !ch::needsMigration(m.data(), m.size()),
           "migration: Glitch kept, Haze, Memory, Soft edges and Wash start at 0, Damp off, version 2");
    PluginState a = initState(), b = initState();
    a.params = old, b.params = m;
    const auto in = drums(48000 * 2, 48000, 120);
    Render ra, rb;
    render(a, in, in, ra);
    render(b, in, in, rb);
    expect(ra.L == rb.L && ra.R == rb.R, "the engine plays an old state exactly as its migrated form");
  }

  // ---- hiss and crackle stay quiet and stop when the music does
  {
    PluginState s = plain();
    set(s, ch::PHaloEngine, ch::HaloOff), set(s, ch::PHiss, 1), set(s, ch::PCrackle, 1), set(s, ch::PMix, 1);
    std::vector<float> in(48000 * 12, 0.f);
    const auto p = pad(48000 * 2, 48000);
    std::copy(p.begin(), p.end(), in.begin() + 48000);
    std::vector<float> zero(48000 * 12, 0.f);
    PluginState q = s;
    set(q, ch::PHiss, 0), set(q, ch::PCrackle, 0);
    Render with, without;
    render(s, in, in, with);
    render(q, in, in, without);
    std::vector<float> diff(in.size());
    for (size_t i = 0; i < in.size(); ++i) diff[i] = with.L[i] - without.L[i];
    const double nz = rms(diff, 48000, 48000 * 3);
    std::printf("# hiss and crackle: %.1f dB under the input while it plays\n", 20 * std::log10(std::max(1e-9, nz / rms(in, 48000, 48000 * 3))));
    expect(peak(with.L, 0, 48000) == 0, "no hiss before the music starts");
    expect(nz > 1e-4 && nz < rms(in, 48000, 48000 * 3) * 0.05, "hiss and crackle are there, but more than 26 dB under the music");
    expect(peak(with.L, 48000 * 10) == 0 && peak(with.R, 48000 * 10) == 0, "hiss and crackle stop within seconds of silence");
  }

  // ---- CPU per block within budget (generous for slow machines)
  {
    PluginState s = initState();
    for (int m = ch::PSparkle; m <= ch::PSpace; ++m) set(s, m, 0.8);
    set(s, ch::PGrLevel, 0.6), set(s, ch::PQuality, ch::QHigh), set(s, ch::PHaloEngine, ch::HaloFull), set(s, ch::PDensity, 0.5);
    set(s, ch::PHaFreeze, 0), set(s, ch::PCrMix, 0.6), set(s, ch::PRmMix, 0.5), set(s, ch::PEcMix, 0.4), set(s, ch::PGtBits, 10), set(s, ch::PGtRate, 12000);
    set(s, ch::PRmShimmer, 0.5), set(s, ch::PEcDiffuse, 0.6), set(s, ch::PWow, 0.5), set(s, ch::PFlutter, 0.5), set(s, ch::PEnsemble, 0.5), set(s, ch::PAge, 0.5);
    set(s, ch::PHiss, 0.3), set(s, ch::PCrackle, 0.3), set(s, ch::PDamp, 4000), set(s, ch::PWarmth, 0.5), set(s, ch::PSoftness, 0.6), set(s, ch::PWash, 0.5);
    auto fx = createEffect(ch::kType);
    const double sr = 48000;
    const int block = 512;
    const auto in = drums(static_cast<size_t>(sr * 6), sr, 128);
    std::vector<float> l(block), r(block);
    ProcessContext ctx;
    ctx.sampleRate = sr, ctx.tempo = 128, ctx.playing = true;
    const auto t0 = std::chrono::steady_clock::now();
    int blocks = 0;
    for (size_t p = 0; p + block <= in.size(); p += block, ++blocks) {
      std::copy(in.begin() + static_cast<long>(p), in.begin() + static_cast<long>(p + block), l.begin());
      std::copy(l.begin(), l.end(), r.begin());
      ctx.tick = static_cast<double>(p) / sr * 128 / 60 * 96;
      fx->process(s, ctx, l.data(), r.data(), block);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double perBlock = secs / blocks, budget = block / sr;
    std::printf("# CPU: %.3f ms per %d-frame block, %.1f %% of real time (everything on)\n", perBlock * 1000, block, perBlock / budget * 100);
    expect(perBlock < budget * 0.5, "CPU per block stays under half of real time with everything on");
    // idle blocks cost almost nothing
    std::vector<float> z(block, 0.f), z2(block, 0.f);
    PluginState q = initState();
    auto fx2 = createEffect(ch::kType);
    for (int i = 0; i < 450; ++i) std::fill(z.begin(), z.end(), 0.f), std::fill(z2.begin(), z2.end(), 0.f), fx2->process(q, ctx, z.data(), z2.data(), block);
    expect(ch::idle(fx2.get()), "a silent input puts Chater to sleep");
  }

  return finish();
}
