// Gridline — synthetic songs with known beats for the Seltmann analysis tests:
// drums (kick, snare, hats), bass and chords over sections, with tempo drift,
// swing, drumless intros, tempo changes and 3/4 or 4/4 bars. Every beat time
// is known exactly (the ground truth), so tempo, beat and downbeat accuracy
// and section labels can be measured.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "engine/Sample.h"

namespace seltsynth {

struct Spec {
  double bpm = 120;
  int bars = 16;
  int meter = 4;            // beats per bar (3 or 4)
  double drift = 0;         // +- share of slow tempo wander (0.03 = 3 %)
  double swing = 0;         // 0..1: off-beat 8ths / 16ths later (0.33 = triplet feel)
  int introBars = 0;        // bars of chords only (no drums) at the start
  int changeBar = -1;       // bar where the tempo jumps to bpm2
  double bpm2 = 0;
  std::string sections;     // letters, one per section (e.g. "ABAC"); empty = all "A"
  int sectionBars = 8;
  bool sixteenths = false;  // hats in 16ths
  bool halfTime = false;    // snare on beat 3 only (half-time feel)
  double rate = 44100;
  double startOffset = 0.05;  // seconds of silence before beat 0
  uint32_t seed = 1;
};

struct Song {
  std::shared_ptr<gl::Sample> sample;
  std::vector<double> beats;      // seconds
  std::vector<double> downbeats;  // seconds
  std::vector<int> sectionStartBar;
  std::string labels;
  double meanBpm = 0;
};

namespace detail {
struct Rng {
  uint32_t s;
  float next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
  }
};
inline void add(std::vector<float>& L, std::vector<float>& R, double t, double rate, const std::vector<float>& v, float gl, float gr) {
  const auto i0 = static_cast<int64_t>(std::llround(t * rate));
  for (size_t k = 0; k < v.size(); ++k) {
    const int64_t i = i0 + static_cast<int64_t>(k);
    if (i < 0 || i >= static_cast<int64_t>(L.size())) continue;
    L[static_cast<size_t>(i)] += v[k] * gl;
    R[static_cast<size_t>(i)] += v[k] * gr;
  }
}
inline std::vector<float> kick(double rate, float tone) {
  std::vector<float> v(static_cast<size_t>(rate * 0.25));
  double ph = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    const double t = i / rate;
    const double f = 45 + (120 * tone) * std::exp(-t * 30);
    ph += 2 * 3.14159265358979 * f / rate;
    v[i] = static_cast<float>(std::sin(ph) * std::exp(-t * 14) * 0.9);
  }
  return v;
}
inline std::vector<float> snare(double rate, Rng& r) {
  std::vector<float> v(static_cast<size_t>(rate * 0.18));
  double ph = 0, lp = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    const double t = i / rate;
    ph += 2 * 3.14159265358979 * 185 / rate;
    const double n = r.next();
    lp += (n - lp) * 0.6;
    v[i] = static_cast<float>((0.35 * std::sin(ph) * std::exp(-t * 30) + 0.55 * (n - lp) * std::exp(-t * 22)));
  }
  return v;
}
inline std::vector<float> hat(double rate, Rng& r, float len) {
  std::vector<float> v(static_cast<size_t>(rate * len));
  double prev = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    const double t = i / rate;
    const double n = r.next();
    v[i] = static_cast<float>((n - prev) * 0.5 * std::exp(-t / len * 5));
    prev = n;
  }
  return v;
}
inline std::vector<float> tone(double rate, double hz, double secs, int harmonics, double amp) {
  std::vector<float> v(static_cast<size_t>(rate * secs));
  const double att = 0.01, rel = 0.05;
  for (size_t i = 0; i < v.size(); ++i) {
    const double t = i / rate;
    double s = 0;
    for (int h = 1; h <= harmonics; ++h) s += std::sin(2 * 3.14159265358979 * hz * h * t) / h;
    const double env = std::min({1.0, t / att, (secs - t) / rel});
    v[i] = static_cast<float>(s * amp * std::max(0.0, env));
  }
  return v;
}
inline double midiHz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }
}  // namespace detail

// Chord roots (MIDI) per bar for a section letter, and its minor/major quality.
inline void chordOf(char sec, int barInSec, int& root, bool& minor) {
  static const int A[4] = {57, 53, 48, 55};  // Am F C G
  static const bool Am[4] = {true, false, false, false};
  static const int B[4] = {53, 55, 52, 57};  // F G Em Am
  static const bool Bm[4] = {false, false, true, true};
  static const int C[4] = {50, 57, 52, 52};  // Dm Am E E
  static const bool Cm[4] = {true, true, false, false};
  static const int D[4] = {48, 52, 53, 55};  // C Em F G
  static const bool Dm[4] = {false, true, false, false};
  const int k = barInSec % 4;
  switch (sec) {
    case 'B': root = B[k], minor = Bm[k]; break;
    case 'C': root = C[k], minor = Cm[k]; break;
    case 'D': root = D[k], minor = Dm[k]; break;
    default: root = A[k], minor = Am[k]; break;
  }
}

inline Song make(const Spec& sp) {
  Song song;
  detail::Rng rng{sp.seed};
  const int totalBeats = sp.bars * sp.meter;
  // beat times
  std::vector<double> bt;
  double t = sp.startOffset;
  for (int k = 0; k <= totalBeats; ++k) {
    bt.push_back(t);
    const int bar = k / sp.meter;
    double bpm = sp.changeBar >= 0 && bar >= sp.changeBar ? sp.bpm2 : sp.bpm;
    if (sp.drift > 0) bpm *= 1.0 + sp.drift * std::sin(2 * 3.14159265358979 * k / 48.0);
    t += 60.0 / bpm;
  }
  const double total = t + 1.0;
  std::vector<float> L(static_cast<size_t>(total * sp.rate), 0.f), R(L.size(), 0.f);
  auto at = [&](double beat) {  // beat position -> seconds (interpolated)
    const int k = std::clamp(static_cast<int>(std::floor(beat)), 0, totalBeats - 1);
    return bt[static_cast<size_t>(k)] + (beat - k) * (bt[static_cast<size_t>(k + 1)] - bt[static_cast<size_t>(k)]);
  };
  const auto K = detail::kick(sp.rate, 1.0f), K2 = detail::kick(sp.rate, 0.7f);
  const std::string secs = sp.sections.empty() ? std::string("A") : sp.sections;
  for (int bar = 0; bar < sp.bars; ++bar) {
    const int si = std::min(static_cast<int>(secs.size()) - 1, bar / sp.sectionBars);
    const char sec = secs[static_cast<size_t>(si)];
    if (bar % sp.sectionBars == 0) {
      song.sectionStartBar.push_back(bar);
      song.labels += sec;
    }
    const int b0 = bar * sp.meter;
    // chords: three notes, the whole bar
    int root;
    bool minor;
    chordOf(sec, bar % sp.sectionBars, root, minor);
    const double barSec = at(b0 + sp.meter) - at(b0);
    for (int n : {0, minor ? 3 : 4, 7}) {
      auto v = detail::tone(sp.rate, detail::midiHz(root + 12 + n), barSec, sec == 'C' ? 2 : 4, sec == 'B' ? 0.05 : 0.06);
      detail::add(L, R, at(b0), sp.rate, v, 0.8f, 1.0f);
    }
    // bass: root per beat
    for (int b = 0; b < sp.meter; ++b) {
      auto v = detail::tone(sp.rate, detail::midiHz(root - 12), (at(b0 + b + 1) - at(b0 + b)) * 0.9, 2, 0.16);
      detail::add(L, R, at(b0 + b), sp.rate, v, 1.0f, 1.0f);
    }
    if (bar < sp.introBars) continue;
    // drums
    for (int b = 0; b < sp.meter; ++b) {
      const double beat = b0 + b;
      const bool kickHit = sp.meter == 3 ? b == 0 : (sp.halfTime ? b == 0 : (b == 0 || b == 2));
      if (kickHit) detail::add(L, R, at(beat), sp.rate, b == 0 ? K : K2, 1.0f, 1.0f);
      if (sec == 'B' && sp.meter == 4 && b == 2) detail::add(L, R, at(beat + 0.5), sp.rate, K2, 0.8f, 0.8f);
      const bool snareHit = sp.meter == 3 ? (b == 1 || b == 2) : (sp.halfTime ? b == 2 : (b == 1 || b == 3));
      if (snareHit) {
        auto v = detail::snare(sp.rate, rng);
        detail::add(L, R, at(beat), sp.rate, v, sp.meter == 3 ? 0.5f : 0.9f, sp.meter == 3 ? 0.5f : 0.9f);
      }
      if (sec != 'C') {
        const int sub = sp.sixteenths || sec == 'B' ? 4 : 2;
        for (int s = 0; s < sub; ++s) {
          double pos = beat + static_cast<double>(s) / sub;
          if (s % 2 == 1) pos += sp.swing * 0.5 / sub * 2 * 0.5;
          auto v = detail::hat(sp.rate, rng, s == 0 ? 0.05f : 0.03f);
          detail::add(L, R, at(pos), sp.rate, v, s == 0 ? 0.35f : 0.22f, s == 0 ? 0.3f : 0.25f);
        }
      } else {
        auto v = detail::hat(sp.rate, rng, 0.25f);  // ride-ish wash on the beat
        detail::add(L, R, at(beat), sp.rate, v, 0.18f, 0.22f);
      }
    }
  }
  for (int k = 0; k < totalBeats; ++k) {
    song.beats.push_back(bt[static_cast<size_t>(k)]);
    if (k % sp.meter == 0) song.downbeats.push_back(bt[static_cast<size_t>(k)]);
  }
  song.meanBpm = 60.0 * (totalBeats - 1) / (bt[static_cast<size_t>(totalBeats - 1)] - bt[0]);
  float peak = 0;
  for (size_t i = 0; i < L.size(); ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
  const float g = peak > 0 ? 0.8f / peak : 1.0f;
  for (size_t i = 0; i < L.size(); ++i) L[i] *= g, R[i] *= g;
  auto s = std::make_shared<gl::Sample>();
  s->sampleRate = sp.rate;
  s->left = std::move(L);
  s->right = R;
  song.sample = s;
  return song;
}

// ---- J-107: short material: drum loops (breaks) and tonal loops cut like sample-pack
// files, with leading silence, a pickup before bar 1, hits played late, a ring-out tail;
// one-shots and drumless ambient pads.
struct LoopSpec {
  double bpm = 92;
  int bars = 2;
  double lead = 0;        // seconds of silence before bar 1
  bool pickup = false;    // a snare an eighth before bar 1 (inside the lead)
  double late = 0;        // every hit this many seconds behind the grid
  double tail = 0;        // seconds of ring-out after the last bar (0: the file ends on the bar line)
  bool drums = true;
  bool chords = false;
  bool sixteenths = true;
  double rate = 44100;
  uint32_t seed = 3;
};

inline Song loop(const LoopSpec& sp) {
  Song song;
  detail::Rng rng{sp.seed};
  const double beat = 60.0 / sp.bpm, t0 = sp.lead;
  const int beats = sp.bars * 4;
  const double total = t0 + beats * beat + sp.tail;
  std::vector<float> L(static_cast<size_t>(std::llround(total * sp.rate)), 0.f), R(L.size(), 0.f);
  const auto K = detail::kick(sp.rate, 1.0f);
  auto at = [&](double b) { return t0 + b * beat + sp.late; };
  if (sp.pickup) {
    auto v = detail::snare(sp.rate, rng);
    detail::add(L, R, t0 - beat / 2 + sp.late, sp.rate, v, 0.8f, 0.8f);
  }
  for (int bar = 0; bar < sp.bars; ++bar) {
    const int b0 = bar * 4;
    if (sp.chords) {
      static const int roots[4] = {57, 53, 60, 55};
      const int root = roots[bar % 4];
      for (int n : {0, 3, 7}) {
        auto v = detail::tone(sp.rate, detail::midiHz(root + n), 4 * beat * 0.98, 4, 0.08);
        detail::add(L, R, at(b0), sp.rate, v, 0.9f, 1.0f);
      }
      for (int b = 0; b < 4; ++b) {  // a plucked line on the beats and the off-beats
        auto v = detail::tone(sp.rate, detail::midiHz(root + 12 + (b % 2 ? 7 : 0)), beat * 0.45, 3, 0.12);
        detail::add(L, R, at(b0 + b), sp.rate, v, 1.0f, 0.9f);
        auto w = detail::tone(sp.rate, detail::midiHz(root + 19), beat * 0.4, 3, 0.07);
        detail::add(L, R, at(b0 + b + 0.5), sp.rate, w, 0.9f, 1.0f);
      }
    }
    if (!sp.drums) continue;
    for (double k : {0.0, 1.5, 2.0}) detail::add(L, R, at(b0 + k), sp.rate, K, 1.0f, 1.0f);
    for (int k : {1, 3}) {
      auto v = detail::snare(sp.rate, rng);
      detail::add(L, R, at(b0 + k), sp.rate, v, 0.9f, 0.9f);
    }
    const int sub = sp.sixteenths ? 16 : 8;
    for (int s = 0; s < sub; ++s) {
      auto v = detail::hat(sp.rate, rng, s % 2 ? 0.03f : 0.05f);
      detail::add(L, R, at(b0 + 4.0 * s / sub), sp.rate, v, s % 2 ? 0.2f : 0.3f, s % 2 ? 0.25f : 0.3f);
    }
  }
  for (int k = 0; k < beats; ++k) {
    song.beats.push_back(t0 + k * beat);
    if (k % 4 == 0) song.downbeats.push_back(t0 + k * beat);
  }
  song.meanBpm = sp.bpm;
  float peak = 0;
  for (size_t i = 0; i < L.size(); ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
  const float g = peak > 0 ? 0.8f / peak : 1.0f;
  for (size_t i = 0; i < L.size(); ++i) L[i] *= g, R[i] *= g;
  auto s = std::make_shared<gl::Sample>();
  s->sampleRate = sp.rate;
  s->left = std::move(L);
  s->right = R;
  song.sample = s;
  return song;
}

// One decaying tone (a one-shot) of `secs` seconds.
inline std::shared_ptr<gl::Sample> oneShot(double secs, double rate = 44100) {
  std::vector<float> L(static_cast<size_t>(secs * rate));
  for (size_t i = 0; i < L.size(); ++i) {
    const double t = i / rate;
    L[i] = static_cast<float>(0.7 * std::exp(-t * 6) * (std::sin(2 * 3.14159265358979 * 220 * t) + 0.4 * std::sin(2 * 3.14159265358979 * 440 * t)));
  }
  auto s = std::make_shared<gl::Sample>();
  s->sampleRate = rate;
  s->left = std::move(L);
  return s;
}

// A drumless ambient texture: a slowly swelling wash of filtered noise under a soft chord that
// fades in over seconds (no onsets, no beat).
inline std::shared_ptr<gl::Sample> ambient(double secs, double rate = 44100) {
  std::vector<float> L(static_cast<size_t>(secs * rate)), R(L.size());
  detail::Rng rng{77};
  double lp1 = 0, lp2 = 0, lr1 = 0, lr2 = 0;
  for (size_t i = 0; i < L.size(); ++i) {
    const double t = i / rate;
    const double swell = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * t / secs);
    lp1 += (rng.next() - lp1) * 0.25, lp2 += (lp1 - lp2) * 0.25;
    lr1 += (rng.next() - lr1) * 0.25, lr2 += (lr1 - lr2) * 0.25;
    double chord = 0;
    for (double f : {110.0, 164.8, 220.3, 261.9}) chord += std::sin(2 * 3.14159265358979 * f * t + f) * (0.6 + 0.4 * std::sin(0.31 * t + f));
    const double fade = std::min(1.0, t / 3.0);
    L[i] = static_cast<float>(swell * (0.5 * lp2 + 0.05 * fade * chord));
    R[i] = static_cast<float>(swell * (0.5 * lr2 + 0.05 * fade * chord));
  }
  auto s = std::make_shared<gl::Sample>();
  s->sampleRate = rate;
  s->left = std::move(L);
  s->right = R;
  return s;
}

// F-measure of estimated vs reference times (seconds) within +-tol.
inline double fmeasure(const std::vector<double>& est, const std::vector<double>& ref, double tol = 0.07) {
  if (est.empty() || ref.empty()) return 0;
  std::vector<bool> used(ref.size(), false);
  int hit = 0;
  for (double e : est) {
    int best = -1;
    double bd = tol;
    for (size_t i = 0; i < ref.size(); ++i)
      if (!used[i] && std::fabs(ref[i] - e) <= bd) bd = std::fabs(ref[i] - e), best = static_cast<int>(i);
    if (best >= 0) used[static_cast<size_t>(best)] = true, ++hit;
  }
  const double p = static_cast<double>(hit) / est.size(), r = static_cast<double>(hit) / ref.size();
  return p + r > 0 ? 2 * p * r / (p + r) : 0;
}

}  // namespace seltsynth
