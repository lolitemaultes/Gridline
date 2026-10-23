// Gridline — "tideline" pack tests: Tideline (gl.tideline) gain curves per shape /
// rate / tempo, Amount 0 transparency, block-size invariance, click-free gain,
// song-position sync after jumps and loops, audio and MIDI triggers, one-shot
// end, band split, mid/side, automation, project save/load, every parameter
// audible in its mode, presets.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginBrand.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/flp/FlpStandIn.h"
#include "engine/TimeGateSlots.h"
#include "engine/Effects.h"
#include "packs/tideline/tideline.h"

using namespace gl;
namespace bw = gl::packs::tideline;

namespace {

constexpr double kSr = 48000;

PluginState fresh() { return defaultPluginState("gl.tideline"); }

struct Run {
  std::vector<float> L, R;
};

// Renders `frames` of constant (inL, inR) through a fresh instance at `tempo`,
// transport playing from `startBeat`, in blocks of `block`. `beatAt` maps the
// sample index to the song position (beats) when given (jumps / loops).
Run render(const PluginState& st, double tempo, long frames, int block, float inL = 1, float inR = 1, double startBeat = 0,
           double (*beatAt)(long, double) = nullptr, bool playing = true, const float* scL = nullptr, MidiPortBus* ports = nullptr,
           const std::vector<std::pair<long, int>>* midiNotes = nullptr) {
  auto fx = createEffect("gl.tideline");
  Run r;
  r.L.assign(static_cast<size_t>(frames), inL);
  r.R.assign(static_cast<size_t>(frames), inR);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  ctx.ppq = 96;
  ctx.playing = playing;
  ctx.ports = ports;
  for (long pos = 0; pos < frames;) {
    int n = static_cast<int>(std::min<long>(block, frames - pos));
    if (beatAt) {  // a block never straddles a jump in these tests (block divides the jump points)
      ctx.tick = beatAt(pos, tempo) * ctx.ppq;
    } else {
      ctx.tick = (startBeat + pos / kSr * tempo / 60.0) * ctx.ppq;
    }
    SideIo io;
    if (scL) {
      io.scL = scL + pos;
      io.scR = scL + pos;
    }
    fx->setSideIo(io);
    if (ports && midiNotes) {
      ports->beginBlock();
      for (const auto& [start, len] : *midiNotes)
        if (pos >= start && pos < start + len) ports->set(0, 36, 100);
    }
    fx->process(st, ctx, r.L.data() + pos, r.R.data() + pos, n);
    pos += n;
  }
  return r;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

long beatFrames(double tempo, double beats) { return static_cast<long>(std::llround(beats * 60.0 / tempo * kSr)); }

// J-98 Listen: renders 2 beats at 120 BPM of mid (m1, m2 Hz) + side (s1, s2 Hz) tones,
// 0.2 each; returns the amplitudes of each tone in the mid / side of the output,
// at the duck (just after beat 1) and at the end of the cycle (curve back up).
struct ToneAmps {
  double duck[4], open[4];  // m1, m2 (in the mid), s1, s2 (in the side)
};
double toneAmp(const std::vector<double>& x, long from, long len, double hz) {
  double re = 0, im = 0, wsum = 0;
  for (long i = 0; i < len; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / len);
    re += x[static_cast<size_t>(from + i)] * w * std::cos(2 * M_PI * hz * i / kSr);
    im += x[static_cast<size_t>(from + i)] * w * std::sin(2 * M_PI * hz * i / kSr);
    wsum += w;
  }
  return 2 * std::sqrt(re * re + im * im) / wsum;
}
ToneAmps listenRender(const PluginState& st, double m1, double m2, double s1, double s2) {
  auto fx = createEffect("gl.tideline");
  const long n = beatFrames(120, 2);
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (long i = 0; i < n; ++i) {
    const double t = i / kSr;
    const double m = 0.2 * (std::sin(2 * M_PI * m1 * t) + std::sin(2 * M_PI * m2 * t));
    const double sd = 0.2 * (std::sin(2 * M_PI * s1 * t) + std::sin(2 * M_PI * s2 * t));
    L[static_cast<size_t>(i)] = static_cast<float>(m + sd);
    R[static_cast<size_t>(i)] = static_cast<float>(m - sd);
  }
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.ppq = 96;
  ctx.playing = true;
  for (long pos = 0; pos < n; pos += 256) {
    ctx.tick = pos / kSr * 2.0 * ctx.ppq;
    fx->process(st, ctx, L.data() + pos, R.data() + pos, static_cast<int>(std::min<long>(256, n - pos)));
  }
  std::vector<double> mid(static_cast<size_t>(n)), side(static_cast<size_t>(n));
  for (size_t i = 0; i < mid.size(); ++i) mid[i] = 0.5 * (L[i] + R[i]), side[i] = 0.5 * (L[i] - R[i]);
  ToneAmps a{};
  const long b1 = beatFrames(120, 1), w = 2400;  // 50 ms
  const double hz[4] = {m1, m2, s1, s2};
  for (int k = 0; k < 4; ++k) {
    const std::vector<double>& x = k < 2 ? mid : side;
    a.duck[k] = toneAmp(x, b1 + 240, w, hz[k]);
    a.open[k] = toneAmp(x, b1 + beatFrames(120, 0.9), w, hz[k]);
  }
  return a;
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.tideline");
  expect(info != nullptr, "gl.tideline is in the catalogue");
  if (!info) return finish();
  expect(info->kind == PluginKind::Effect && info->params.size() == static_cast<size_t>(bw::BwCount), "effect with every parameter");
  expect(info->category == "Dynamics", "listed under Dynamics");
  expect(isFlagship("gl.tideline") && pluginBrand("gl.tideline")->family == BrandFamily::Dynamics && !pluginTagline("gl.tideline").empty(),
         "J-94 flagship of the Dynamics family with a tagline");
  {
    const std::string kw = pluginKeywords("gl.tideline");
    expect(kw.find("sidechain") != std::string::npos && kw.find("pump") != std::string::npos && kw.find("ducking") != std::string::npos &&
               kw.find("kickstart") != std::string::npos,
           "search keywords: sidechain, pump, ducking, kickstart");
  }
  expect(info->presets.size() >= 6 && info->presets.size() <= 12, "6-12 factory presets");
  expect(info->sideInParam == bw::BwScIn && info->midiInput, "sidechain route param + MIDI input declared");

  // ---------------------------------------------------------------- curve model
  {
    bool ok = true;
    for (int s = 0; s < bw::kShapeCount; ++s) {
      bw::CustomCurve cc;
      double lo = 1, hi = 0;
      for (int i = 0; i <= 1000; ++i) {
        double g = bw::curveAt(s, cc, i / 1000.0, 0);
        if (!(g >= -1e-9 && g <= 1 + 1e-9)) ok = false;
        lo = std::min(lo, g);
        hi = std::max(hi, g);
      }
      if (lo > 0.05 || hi < 0.99) ok = false;
      // closed loop: the end of a cycle meets the start of the next within a small step
    }
    expect(ok, "every shape stays in 0..1 and spans silence to unity");
    bw::CustomCurve cc;
    expect(bw::curveAt(0, cc, 0.006, 0) < 0.02 && bw::curveAt(0, cc, 0.9, 0) > 0.99, "Pump: silent on the beat, open late in the beat");
    expect(bw::curveAt(10, cc, 0.02, 0) > 0.99 && bw::curveAt(10, cc, 0.5, 0) < 0.01, "Trim: lets the hit through, then cuts");
    expect(bw::curveAt(12, cc, 0.1, 0) > 0.9 && bw::curveAt(12, cc, 0.995, 0) < 0.1, "Inhale: open early, sucked down into the beat");
    // Duration stretches the release
    expect(bw::curveAt(0, cc, 0.3, 1) < bw::curveAt(0, cc, 0.3, 0) && bw::curveAt(0, cc, 0.3, -1) > bw::curveAt(0, cc, 0.3, 0),
           "Duration stretches / shortens the duck");
    // custom default curve
    std::vector<float> v(static_cast<size_t>(bw::BwCount));
    for (int i = 0; i < bw::kNodes; ++i) {
      v[static_cast<size_t>(bw::BwNodeX0 + i)] = bw::kDefNodeX[i];
      v[static_cast<size_t>(bw::BwNodeY0 + i)] = bw::kDefNodeY[i];
    }
    v[bw::BwTension] = 0.3f;
    bw::customFrom(v, cc);
    expect(bw::curveAt(bw::kShapeCustom, cc, 0.0, 0) < 0.01 && bw::curveAt(bw::kShapeCustom, cc, 0.8, 0) > 0.95, "custom curve follows its points");
    bool mono = true;
    for (int i = 1; i <= 900; ++i) mono = mono && bw::curveAt(bw::kShapeCustom, cc, i / 1000.0, 0) + 1e-9 >= bw::curveAt(bw::kShapeCustom, cc, (i - 1) / 1000.0, 0);
    expect(mono, "custom curve rises monotonically between rising points");
  }

  // ---------------------------------------------------------------- tempo sync
  // Gain sampled per beat position at 120 and 140 BPM for every shape and rate matches the model.
  {
    bool ok = true;
    double worst = 0;
    for (double tempo : {120.0, 140.0})
      for (int rate : {0, 1, 2, 3, 4, 6, 8})
        for (int shape = 0; shape < bw::kShapeCount; shape += 3) {
          PluginState st = fresh();
          st.params[bw::BwShape] = static_cast<float>(shape);
          st.params[bw::BwRate] = static_cast<float>(rate);
          st.params[bw::BwSmooth] = 0;  // the floor smoothing (0.05 ms) only
          const double cyc = bw::kRateBeats[rate];
          long frames = beatFrames(tempo, cyc * 2.0);
          Run r = render(st, tempo, frames, 256);
          bw::CustomCurve cc;
          // compare inside the second cycle away from steep edges
          for (int k = 1; k < 32; ++k) {
            double p = k / 32.0;
            long i = beatFrames(tempo, cyc * (1.0 + p));
            if (i >= frames) continue;
            double want = bw::curveAt(shape, cc, p, 0);
            double slope = std::fabs(bw::curveAt(shape, cc, p + 0.002, 0) - bw::curveAt(shape, cc, p - 0.002, 0));
            if (slope > 0.05) continue;
            double d = std::fabs(r.L[static_cast<size_t>(i)] - want);
            worst = std::max(worst, d);
            if (d > 0.02) ok = false;
          }
        }
    char b[96];
    std::snprintf(b, sizeof b, "gain follows the curve per shape / rate at 120 + 140 BPM (worst %.4f)", worst);
    expect(ok, b);
  }
  // Pump at 1/4, 120 BPM: phase locked to the beat (silent on every beat, open before the next)
  {
    PluginState st = fresh();
    Run r = render(st, 120, beatFrames(120, 4), 512);
    bool ok = true;
    for (int beat = 1; beat < 4; ++beat) {
      long b0 = beatFrames(120, beat);
      ok = ok && r.L[static_cast<size_t>(b0 + 200)] < 0.1f && r.L[static_cast<size_t>(b0 - 400)] > 0.98f;
    }
    expect(ok, "Pump 1/4: ducks on every beat, recovers before the next");
    // 1/8 ducks twice per beat
    st.params[bw::BwRate] = 1;
    r = render(st, 120, beatFrames(120, 2), 512);
    long half = beatFrames(120, 0.5);
    expect(r.L[static_cast<size_t>(half + 200)] < 0.1f && r.L[static_cast<size_t>(half - 300)] > 0.9f, "1/8 rate: a duck every half beat");
  }

  // ---------------------------------------------------------------- Amount
  {
    PluginState st = fresh();
    st.params[bw::BwAmount] = 0;
    Run r = render(st, 128, beatFrames(128, 4), 300, 0.7f, -0.4f);
    bool same = true;
    for (size_t i = 0; i < r.L.size(); ++i) same = same && r.L[i] == 0.7f && r.R[i] == -0.4f;
    expect(same, "Amount 0 is bit-transparent");
    st.params[bw::BwAmount] = 0.5f;
    r = render(st, 120, beatFrames(120, 2), 256);
    float deepest = *std::min_element(r.L.begin() + beatFrames(120, 1), r.L.begin() + beatFrames(120, 1.5));
    expect(std::fabs(deepest - 0.5f) < 0.03f, "Amount 50% halves the deepest point");
    st.params[bw::BwBand] = bw::BandLows;
    st.params[bw::BwBandFreq] = 150;
    st.params[bw::BwAmount] = 0;
    same = true;
    for (double hz : {40.0, 150.0, 1000.0}) {  // magnitude-flat split (allpass phase)
      auto fx = createEffect("gl.tideline");
      ProcessContext ctx;
      ctx.sampleRate = kSr;
      ctx.playing = true;
      std::vector<float> L(48000), R;
      for (size_t k = 0; k < L.size(); ++k) L[k] = static_cast<float>(0.5 * std::sin(2 * M_PI * hz * k / kSr));
      R = L;
      for (int pos = 0; pos < 48000; pos += 480) {
        ctx.tick = pos / kSr * 2.0 * ctx.ppq;
        fx->process(st, ctx, L.data() + pos, R.data() + pos, 480);
      }
      double pk = 0;
      for (size_t k = 24000; k < L.size(); ++k) pk = std::max(pk, static_cast<double>(std::fabs(L[k])));
      same = same && std::fabs(20 * std::log10(pk / 0.5)) < 0.1;
    }
    expect(same, "Amount 0 with the band split is magnitude-flat (within 0.1 dB)");
  }

  // ---------------------------------------------------------------- block size invariance
  {
    PluginState st = fresh();
    st.params[bw::BwShape] = 4;
    st.params[bw::BwSmooth] = 3;
    st.params[bw::BwBand] = bw::BandLows;
    std::vector<float> noise(static_cast<size_t>(beatFrames(140, 3)));
    uint32_t s = 1;
    for (auto& x : noise) {
      s = s * 1664525u + 1013904223u;
      x = static_cast<float>(s >> 8) / 16777216.0f * 2 - 1;
    }
    auto run = [&](int block) {
      auto fx = createEffect("gl.tideline");
      std::vector<float> L = noise, R = noise;
      ProcessContext ctx;
      ctx.sampleRate = kSr;
      ctx.tempo = 140;
      ctx.playing = true;
      for (long pos = 0; pos < static_cast<long>(L.size());) {
        int n = static_cast<int>(std::min<long>(block, static_cast<long>(L.size()) - pos));
        ctx.tick = pos / kSr * 140 / 60.0 * ctx.ppq;
        fx->process(st, ctx, L.data() + pos, R.data() + pos, n);
        pos += n;
      }
      return L;
    };
    auto a = run(64), b = run(1000), c = run(17);
    expect(maxAbsDiff(a, b) < 1e-5 && maxAbsDiff(a, c) < 1e-5, "block size invariant (64 / 1000 / 17)");
    // audio trigger too
    st.params[bw::BwTrigger] = bw::TrigAudio;
    auto d = run(64), e = run(999);
    expect(maxAbsDiff(d, e) < 1e-5, "audio trigger is block size invariant");
  }

  // ---------------------------------------------------------------- click-free
  {
    bool ok = true;
    double worst = 0;
    for (int shape = 0; shape < bw::kShapeCount; ++shape) {
      PluginState st = fresh();
      st.params[bw::BwShape] = static_cast<float>(shape);
      Run r = render(st, 174, beatFrames(174, 3), 256);
      for (size_t i = 1; i < r.L.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(r.L[i] - r.L[i - 1])));
    }
    ok = worst < 0.03;  // 1 ms default smoothing: < 3 % of full scale per sample
    char b[96];
    std::snprintf(b, sizeof b, "no clicks: largest gain step per sample %.4f", worst);
    expect(ok, b);
  }

  // ---------------------------------------------------------------- sync after jump / loop
  {
    PluginState st = fresh();
    st.params[bw::BwSmooth] = 0;
    // song jumps from beat 0.5 to beat 7.25 after one beat of audio, a loop wrap later
    static double tempoS = 120;
    auto beatAt = [](long pos, double tempo) -> double {
      double b = pos / kSr * tempo / 60.0;
      if (b < 1.0) return b;            // 0 .. 1
      if (b < 2.0) return 7.25 + (b - 1.0);  // jump to 7.25
      return 3.0 + (b - 2.0);           // loop back to 3.0
    };
    (void)tempoS;
    Run r = render(st, 120, beatFrames(120, 3), 250, 1, 1, 0, beatAt);
    bw::CustomCurve cc;
    auto at = [&](double audioBeat, double songBeat) {
      double want = bw::curveAt(0, cc, songBeat - std::floor(songBeat), 0);
      return std::fabs(r.L[static_cast<size_t>(beatFrames(120, audioBeat))] - want) < 0.03;
    };
    expect(at(1.1, 7.35) && at(1.5, 7.75), "follows the song position after a jump");
    expect(at(2.2, 3.2) && at(2.6, 3.6), "follows the song position after a loop wrap");
    // stopped transport in Tempo mode passes the sound
    Run s = render(fresh(), 120, 4800, 256, 1, 1, 0, nullptr, false);
    expect(std::fabs(s.L.back() - 1.0f) < 1e-4, "stopped transport: no ducking");
  }

  // ---------------------------------------------------------------- audio trigger
  {
    PluginState st = fresh();
    st.params[bw::BwTrigger] = bw::TrigAudio;
    st.params[bw::BwShape] = 1;  // Snap
    const long frames = beatFrames(120, 4);
    std::vector<float> kick(static_cast<size_t>(frames), 0.0f);
    const long hits[3] = {10000, 40000, 70001};  // off the grid
    for (long h : hits)
      for (long i = 0; i < 2400; ++i) kick[static_cast<size_t>(h + i)] = 0.9f * static_cast<float>(std::sin(2 * M_PI * 55 * i / kSr) * std::exp(-i / 900.0));
    Run r = render(st, 120, frames, 256, 1, 1, 0, nullptr, true, kick.data());
    bool ok = true;
    for (long h : hits) ok = ok && r.L[static_cast<size_t>(h + 600)] < 0.2f && r.L[static_cast<size_t>(h - 50)] > 0.99f;
    expect(ok, "audio trigger: ducks right after each sidechain hit, open before it");
    expect(r.L[static_cast<size_t>(hits[0] + 9000)] > 0.95f, "one shot: back to unity after the curve");
    // without a sidechain buffer it listens to its own input; silence never triggers
    Run q = render(st, 120, 24000, 256, 0.0f, 0.0f);
    bool silent = true;
    for (float x : q.L) silent = silent && x == 0.0f;
    expect(silent, "silence in, silence out");
    // threshold above the hit level: no trigger
    st.params[bw::BwThreshold] = -0.1f;
    r = render(st, 120, frames, 256, 1, 1, 0, nullptr, true, kick.data());
    expect(r.L[static_cast<size_t>(hits[0] + 600)] > 0.99f, "threshold gates the audio trigger");
    // one-shot end holds the curve value
    st.params[bw::BwThreshold] = -24;
    st.params[bw::BwEnd] = 0.05f;
    r = render(st, 120, frames, 256, 1, 1, 0, nullptr, true, kick.data());
    expect(r.L[static_cast<size_t>(hits[0] + 9000)] < 0.5f, "one-shot end marker holds the curve until the next trigger");
    // loop on: keeps pumping at the rate after one trigger
    st.params[bw::BwEnd] = 1;
    st.params[bw::BwLoop] = 1;
    std::vector<float> one(static_cast<size_t>(frames), 0.0f);
    for (long i = 0; i < 2400; ++i) one[static_cast<size_t>(1000 + i)] = kick[static_cast<size_t>(hits[0] + i)];
    r = render(st, 120, frames, 256, 1, 1, 0, nullptr, true, one.data());
    expect(r.L[static_cast<size_t>(1000 + 24000 + 600)] < 0.2f, "loop: the curve repeats after a single trigger");
  }

  // ---------------------------------------------------------------- MIDI trigger
  {
    PluginState st = fresh();
    st.params[bw::BwTrigger] = bw::TrigMidi;
    st.params[bw::BwShape] = 1;
    auto bus = std::make_unique<MidiPortBus>();
    std::vector<std::pair<long, int>> notes = {{5120, 2048}, {30720, 2048}};
    Run r = render(st, 120, 48000, 256, 1, 1, 0, nullptr, true, nullptr, bus.get(), &notes);
    expect(r.L[5000] > 0.99f && r.L[5120 + 400] < 0.2f, "MIDI note starts the curve");
    expect(r.L[29000] > 0.95f && r.L[30720 + 400] < 0.2f, "the next note restarts it");
    st.params[bw::BwMidiNote] = 40;  // other note: ignored
    r = render(st, 120, 48000, 256, 1, 1, 0, nullptr, true, nullptr, bus.get(), &notes);
    expect(r.L[5120 + 400] > 0.99f, "note filter ignores other notes");
  }

  // ---------------------------------------------------------------- band split and stereo
  {
    PluginState st = fresh();
    st.params[bw::BwBand] = bw::BandLows;
    st.params[bw::BwBandFreq] = 150;
    auto toneThrough = [&](double hz) {
      auto fx = createEffect("gl.tideline");
      ProcessContext ctx;
      ctx.sampleRate = kSr;
      ctx.tempo = 120;
      ctx.playing = true;
      const long n = beatFrames(120, 2);
      std::vector<float> L(static_cast<size_t>(n)), R;
      for (long i = 0; i < n; ++i) L[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * M_PI * hz * i / kSr));
      R = L;
      for (long pos = 0; pos < n; pos += 256) {
        ctx.tick = pos / kSr * 2.0 * ctx.ppq;
        fx->process(st, ctx, L.data() + pos, R.data() + pos, static_cast<int>(std::min<long>(256, n - pos)));
      }
      double pk = 0;  // just after beat 1
      long b1 = beatFrames(120, 1);
      for (long i = b1 + 150; i < b1 + 1000; ++i) pk = std::max(pk, static_cast<double>(std::fabs(L[static_cast<size_t>(i)])));
      return pk;
    };
    std::printf("# band lows: 50 Hz %.3f, 4 kHz %.3f\n", toneThrough(50), toneThrough(4000));
    expect(toneThrough(50) < 0.1 && toneThrough(4000) > 0.4, "Lows band: ducks the bass, leaves the highs");
    st.params[bw::BwBand] = bw::BandHighs;
    expect(toneThrough(50) > 0.4 && toneThrough(4000) < 0.1, "Highs band: ducks the highs, leaves the bass");
    // mid / side
    PluginState ms = fresh();
    ms.params[bw::BwStereo] = bw::StSide;
    Run r = render(ms, 120, beatFrames(120, 2), 256, 1, 1);  // pure mid input
    expect(std::fabs(r.L[static_cast<size_t>(beatFrames(120, 1) + 200)] - 1.0f) < 1e-4, "Side mode leaves a centred sound alone");
    r = render(ms, 120, beatFrames(120, 2), 256, 1, -1);  // pure side input
    expect(r.L[static_cast<size_t>(beatFrames(120, 1) + 200)] < 0.1f, "Side mode ducks the sides");
    ms.params[bw::BwStereo] = bw::StMid;
    r = render(ms, 120, beatFrames(120, 2), 256, 1, -1);
    expect(std::fabs(r.L[static_cast<size_t>(beatFrames(120, 1) + 200)] - 1.0f) < 1e-4, "Mid mode leaves the sides alone");
  }

  // ---------------------------------------------------------------- automation (parameter changes mid-stream)
  {
    auto fx = createEffect("gl.tideline");
    PluginState st = fresh();
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
    ctx.playing = true;
    std::vector<float> L(256), R(256);
    double prev = 1;
    double worst = 0;
    for (int b = 0; b < 400; ++b) {
      st.params[bw::BwAmount] = static_cast<float>((b % 100) / 99.0);  // ramps
      st.params[bw::BwShape] = static_cast<float>((b / 37) % bw::kShapeCount);
      std::fill(L.begin(), L.end(), 1.0f);
      std::fill(R.begin(), R.end(), 1.0f);
      ctx.tick = b * 256 / kSr * 2.0 * ctx.ppq;
      fx->process(st, ctx, L.data(), R.data(), 256);
      for (float x : L) {
        worst = std::max(worst, std::fabs(x - prev));
        prev = x;
      }
    }
    expect(worst < 0.03, "automated Amount / Shape changes stay click-free");
  }

  // ---------------------------------------------------------------- every parameter is audible in its mode
  {
    struct Case {
      int p;
      float v;
      int trig;
      int band = 0;
    };
    const std::vector<Case> cases = {
        {bw::BwAmount, 0.3f, 0},     {bw::BwShape, 7, 0},         {bw::BwRate, 0, 0},          {bw::BwDuration, 0.8f, 0},
        {bw::BwShift, 0.25f, 0},     {bw::BwSmooth, 15, 0},       {bw::BwBand, 1, 0},          {bw::BwStereo, 2, 0},
        {bw::BwTrigger, 1, 0},       {bw::BwLoop, 1, 1},          {bw::BwEnd, 0.1f, 1},        {bw::BwThreshold, -1, 1},
        {bw::BwRetrigger, 1500, 1},  {bw::BwDetLow, 1500, 1},     {bw::BwDetHigh, 40, 1},      {bw::BwMidiNote, 50, 2},
        {bw::BwMidiPort, 3, 2},      {bw::BwScIn, 0, 1},
        {bw::BwBandFreq, 40, 0, bw::BandLows},
    };
    const long frames = beatFrames(120, 4);
    std::vector<float> kick(static_cast<size_t>(frames), 0.0f);
    for (long h : {3000L, 60000L})
      for (long i = 0; i < 2400; ++i) kick[static_cast<size_t>(h + i)] = 0.9f * static_cast<float>(std::sin(2 * M_PI * 55 * i / kSr) * std::exp(-i / 900.0));
    auto bus = std::make_unique<MidiPortBus>();
    std::vector<std::pair<long, int>> notes = {{5120, 2048}, {30720, 2048}};
    int bad = 0;
    for (const auto& c : cases) {
      PluginState a = fresh();
      a.params[bw::BwTrigger] = static_cast<float>(c.trig);
      a.params[bw::BwBand] = static_cast<float>(c.band);
      PluginState b = a;
      b.params[static_cast<size_t>(c.p)] = c.v;
      // tone input so the band / smoothing / stereo changes show
      auto go = [&](const PluginState& st) {
        auto fx = createEffect("gl.tideline");
        ProcessContext ctx;
        ctx.sampleRate = kSr;
        ctx.tempo = 120;
        ctx.playing = true;
        ctx.ports = bus.get();
        std::vector<float> L(static_cast<size_t>(frames)), R;
        for (long i = 0; i < frames; ++i) L[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * M_PI * 60 * i / kSr));
        R = L;
        for (size_t i = 0; i < R.size(); i += 2) R[i] *= 0.3f;
        for (long pos = 0; pos < frames; pos += 256) {
          bus->beginBlock();
          for (const auto& [s0, len] : notes)
            if (pos >= s0 && pos < s0 + len) bus->set(0, 36, 100);
          SideIo io;
          io.scL = kick.data() + pos;
          io.scR = kick.data() + pos;
          // BwScIn = 0 (own input) is resolved by the engine: the effect then gets no sidechain buffer
          if (st.params[bw::BwScIn] >= -0.5f) io = SideIo{};
          fx->setSideIo(io);
          ctx.tick = pos / kSr * 2.0 * ctx.ppq;
          fx->process(st, ctx, L.data() + pos, R.data() + pos, static_cast<int>(std::min<long>(256, frames - pos)));
        }
        L.insert(L.end(), R.begin(), R.end());
        return L;
      };
      if (maxAbsDiff(go(a), go(b)) < 1e-3) {
        ++bad;
        std::printf("# not audible: %s\n", info->params[static_cast<size_t>(c.p)].name.c_str());
      }
    }
    // custom points + tension with the Custom shape
    {
      PluginState a = fresh();
      a.params[bw::BwShape] = bw::kShapeCustom;
      Run base = render(a, 120, beatFrames(120, 2), 256);
      for (int k = bw::BwTension; k < bw::BwCount; ++k) {
        PluginState b = a;
        const auto& d = info->params[static_cast<size_t>(k)];
        b.params[static_cast<size_t>(k)] = static_cast<float>(b.params[static_cast<size_t>(k)] > (d.min + d.max) / 2 ? d.min + (d.max - d.min) * 0.1 : d.max - (d.max - d.min) * 0.1);
        if (maxAbsDiff(base.L, render(b, 120, beatFrames(120, 2), 256).L) < 1e-3) {
          ++bad;
          std::printf("# not audible: %s\n", d.name.c_str());
        }
      }
    }
    expect(bad == 0, "every parameter changes the sound in its mode");
    expect(cases.size() + static_cast<size_t>(bw::BwCount - bw::BwTension) + 2 >= static_cast<size_t>(bw::BwCount),
           "the audit covers every parameter (band listen / split checked below)");
    PluginState a = fresh(), b = fresh();
    a.params[bw::BwBand] = b.params[bw::BwBand] = bw::BandLows;
    b.params[bw::BwBandListen] = 1;
    Run ra = render(a, 120, 9600, 256, 0.5f, 0.5f), rb = render(b, 120, 9600, 256, 0.5f, 0.5f);
    expect(maxAbsDiff(ra.L, rb.L) > 0.01, "Band listen is audible");
  }

  // ---------------------------------------------------------------- J-98 Listen: solo what is ducked
  {
    // Band split at 300 Hz; tones at 60 / 3000 Hz in the mid and 90 / 4000 Hz in the side.
    // Amount 0.6: the duck is audible (about 0.4 of the level) but not silent.
    auto st = [](int band, int stereo, bool listen) {
      PluginState p = fresh();
      p.params[bw::BwAmount] = 0.6f;
      p.params[bw::BwBand] = static_cast<float>(band);
      p.params[bw::BwBandFreq] = 300;
      p.params[bw::BwStereo] = static_cast<float>(stereo);
      p.params[bw::BwBandListen] = listen ? 1.f : 0.f;
      return p;
    };
    auto db = [](double v) { return 20 * std::log10(std::max(v, 1e-9) / 0.2); };
    auto show = [&](const char* name, const ToneAmps& a) {
      std::printf("# listen %-18s duck dB: m60 %6.1f m3k %6.1f s90 %6.1f s4k %6.1f | open: m60 %6.1f m3k %6.1f s90 %6.1f s4k %6.1f\n", name, db(a.duck[0]),
                  db(a.duck[1]), db(a.duck[2]), db(a.duck[3]), db(a.open[0]), db(a.open[1]), db(a.open[2]), db(a.open[3]));
    };
    const double M1 = 60, M2 = 3000, S1 = 90, S2 = 4000;
    // Lows, stereo: listen off = highs untouched, lows ducked; on = lows only (still ducked).
    ToneAmps off = listenRender(st(bw::BandLows, bw::StStereo, false), M1, M2, S1, S2);
    ToneAmps on = listenRender(st(bw::BandLows, bw::StStereo, true), M1, M2, S1, S2);
    show("lows stereo off", off);
    show("lows stereo on", on);
    expect(db(off.duck[1]) > -1 && db(off.duck[0]) < -4 && db(off.open[0]) > -1.5, "Lows, Listen off: lows ducked, highs pass");
    expect(db(on.duck[1]) < -50 && db(on.open[1]) < -50 && db(on.duck[3]) < -50, "Lows, Listen on: the high band is muted");
    expect(std::fabs(db(on.duck[0]) - db(off.duck[0])) < 0.5 && db(on.open[0]) > -1.5 && std::fabs(db(on.duck[2]) - db(off.duck[2])) < 0.5,
           "Lows, Listen on: the low band plays with the ducking applied");
    // Highs, stereo.
    off = listenRender(st(bw::BandHighs, bw::StStereo, false), M1, M2, S1, S2);
    on = listenRender(st(bw::BandHighs, bw::StStereo, true), M1, M2, S1, S2);
    show("highs stereo off", off);
    show("highs stereo on", on);
    expect(db(off.duck[0]) > -1 && db(off.duck[1]) < -4, "Highs, Listen off: highs ducked, lows pass");
    expect(db(on.duck[0]) < -50 && db(on.open[0]) < -50 && db(on.duck[2]) < -40, "Highs, Listen on: the low band is muted (90 Hz: LR4 at 300 Hz, -42 dB)");
    expect(std::fabs(db(on.duck[1]) - db(off.duck[1])) < 0.5 && db(on.open[1]) > -1.5, "Highs, Listen on: the high band plays, ducked");
    // Full, stereo: everything is ducked, so Listen changes nothing.
    {
      Run a = render(st(bw::BandFull, bw::StStereo, false), 120, 9600, 256, 0.5f, 0.3f);
      Run b = render(st(bw::BandFull, bw::StStereo, true), 120, 9600, 256, 0.5f, 0.3f);
      expect(maxAbsDiff(a.L, b.L) < 1e-7 && maxAbsDiff(a.R, b.R) < 1e-7, "Full + Stereo: Listen has nothing to solo (no change)");
    }
    // Full, mid / side: Listen solos the ducked component.
    off = listenRender(st(bw::BandFull, bw::StMid, false), M1, M2, S1, S2);
    on = listenRender(st(bw::BandFull, bw::StMid, true), M1, M2, S1, S2);
    show("full mid off", off);
    show("full mid on", on);
    expect(db(off.duck[2]) > -1 && db(off.duck[0]) < -4, "Full + Mid, Listen off: the side passes");
    expect(db(on.duck[2]) < -100 && db(on.duck[3]) < -100 && std::fabs(db(on.duck[0]) - db(off.duck[0])) < 0.2,
           "Full + Mid, Listen on: only the (ducked) mid");
    on = listenRender(st(bw::BandFull, bw::StSide, true), M1, M2, S1, S2);
    show("full side on", on);
    expect(db(on.duck[0]) < -100 && db(on.duck[1]) < -100 && db(on.duck[2]) < -4 && db(on.open[2]) > -1.5, "Full + Side, Listen on: only the (ducked) side");
    // Lows of the mid / highs of the side.
    on = listenRender(st(bw::BandLows, bw::StMid, true), M1, M2, S1, S2);
    show("lows mid on", on);
    expect(db(on.duck[0]) < -4 && db(on.open[0]) > -1.5 && db(on.duck[1]) < -50 && db(on.duck[2]) < -100 && db(on.duck[3]) < -100,
           "Lows + Mid, Listen on: only the mid's low band, ducked");
    on = listenRender(st(bw::BandHighs, bw::StSide, true), M1, M2, S1, S2);
    show("highs side on", on);
    expect(db(on.duck[3]) < -4 && db(on.open[3]) > -1.5 && db(on.duck[2]) < -40 && db(on.duck[0]) < -100 && db(on.duck[1]) < -100,
           "Highs + Side, Listen on: only the side's high band, ducked");
    // The button is a parameter: automatable (ParamId set / get) and saved with the project.
    Song s = makeEmptySong();
    ensureMixer(s, 2);
    s.mixer[1].slots[0].plugin = fresh();
    const ObjectId tr = s.mixer[1].id;
    const ParamId lp = slotParam(tr, 0, bw::BwBandListen);
    const auto d = describeParam(s, lp);
    expect(d && d->kind == ParamKind::Toggle && setParam(s, lp, 1.0) && getParam(s, lp).value_or(0) == 1.0, "Listen: an automatable toggle parameter");
    Json j;
    Song back;
    expect(jsonParse(jsonStringify(songToJson(s)), j) && songFromJson(j, back) && getParam(back, lp).value_or(0) == 1.0, "Listen: saved with the project");
  }

  // ---------------------------------------------------------------- extremes, presets, state
  {
    bool finite = true;
    for (const auto& pr : info->presets) {
      PluginState st = fresh();
      for (const auto& [k, v] : pr.values) st.params[static_cast<size_t>(k)] = v;
      Run r = render(st, 140, 20000, 256, 0.5f, 0.5f);
      for (float x : r.L) finite = finite && std::isfinite(x) && std::fabs(x) <= 1.0f;
    }
    PluginState st = fresh();
    for (size_t k = 0; k < info->params.size(); ++k) st.params[k] = static_cast<float>(info->params[k].max);
    Run r = render(st, 999, 20000, 33, 0.5f, 0.5f);
    for (float x : r.L) finite = finite && std::isfinite(x);
    for (size_t k = 0; k < info->params.size(); ++k) st.params[k] = static_cast<float>(info->params[k].min);
    r = render(st, 10, 20000, 33, 0.5f, 0.5f);
    for (float x : r.L) finite = finite && std::isfinite(x);
    expect(finite, "presets and parameter extremes give finite, bounded output");
    PluginState shortSt;
    shortSt.type = "gl.tideline";
    shortSt.params = {0.5f, 3};
    normalizePluginState(shortSt);
    expect(shortSt.params.size() == static_cast<size_t>(bw::BwCount) && shortSt.params[0] == 0.5f &&
               shortSt.params[bw::BwRate] == static_cast<float>(bw::kRateDefault),
           "normalizePluginState pads short states with defaults");
  }

  // ---------------------------------------------------------------- project save / load
  {
    Song s = makeEmptySong();
    ensureMixer(s, 2);
    PluginState st = fresh();
    st.params[bw::BwShape] = 13;
    st.params[bw::BwRate] = 7;
    st.params[bw::BwAmount] = 0.42f;
    st.params[bw::BwNodeY0 + 3] = 0.11f;
    s.mixer[1].slots[0].plugin = st;
    const std::string path = (std::filesystem::temp_directory_path() / "gridline_test_tideline.glp").string();
    std::string err;
    bool saved = saveProject(path, s, &err);
    Song back;
    bool loaded = saved && loadProject(path, back, &err);
    expect(loaded && back.mixer.size() > 1 && back.mixer[1].slots[0].plugin == st, "project save / load keeps every parameter");
    std::filesystem::remove(path);
  }
  // ---------------------------------------------------------------- FLP export stand-in (Gross Beat volume envelope)
  {
    expect(flp::standInPolicy("gl.tideline").kind == flp::StandInKind::Mapped && flp::standInPolicy("gl.tideline").flName == "Gross Beat",
           "FLP export policy: mapped to Gross Beat");
    Song s = makeEmptySong();
    ensureMixer(s, 2);
    PluginState st = fresh();
    st.params[bw::BwShape] = 3;
    st.params[bw::BwAmount] = 0.8f;
    s.mixer[1].slots[0].plugin = st;
    flp::ExportOptions eo;
    flp::StandInOptions so;
    flp::StandInResult res;
    const bool ok = flp::applyStandIns(s, eo, so, res);
    const PluginState& out = s.mixer[1].slots[0].plugin;
    bool envOk = ok && out.type == "gl.timegate";
    double worst = 0;
    if (envOk) {
      auto bank = std::make_unique<tg::Bank>();
      tg::factory(*bank);
      tg::parse(out.data.c_str(), *bank);
      bank->vol[0].prepare();
      bw::CustomCurve cc;
      for (int k = 0; k < 64; ++k) {
        double beat = 0.03 + k * (3.9 / 64);
        if (beat - std::floor(beat) < 0.03) continue;  // the drop on the beat is a short ramp in the envelope
        double want = bw::applyAmount(bw::curveAt(3, cc, beat - std::floor(beat), 0), 0.8);
        if (std::fabs(bank->vol[0].eval(beat) - want) > 0.08) std::printf("# beat %.3f env %.3f want %.3f\n", beat, bank->vol[0].eval(beat), want);
        worst = std::max(worst, std::fabs(bank->vol[0].eval(beat) - want));
      }
    }
    char b[96];
    std::snprintf(b, sizeof b, "stand-in volume envelope follows the curve (worst %.3f)", worst);
    expect(envOk && worst < 0.08, b);
  }
  return finish();
}
