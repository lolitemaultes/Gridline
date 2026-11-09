// Gridline — K-145: the live waveform of a recording in progress (engine/LiveWave). The model
// fills correctly from a simulated recording (frame counts, every level's peaks against the
// audio, any block size, truncate and add again, mono, clipping), the fit grows at the normal
// scale and then covers the whole take at every frame after it reaches the right edge (no
// jumps, no stutter, pauses and a pre-roll burst), the trace stays O(width) on an hour-long
// take, a level change is continuous, and the capture ring the recorders drain does not
// allocate on the audio thread.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "../../src/engine/LiveWave.h"
#include "../../src/packs/edison/edison.h"
#include "../Check.h"

using namespace gl;

namespace {

bool gWatch = false;
long gAllocs = 0;

int16_t q(float v) {
  const float s = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
  return static_cast<int16_t>(s >= 0 ? s + 0.5f : s - 0.5f);
}

// A take: hits, a tone, noise (deterministic).
void signal(size_t n, double rate, std::vector<float>& L, std::vector<float>& R, uint32_t seed = 7) {
  L.resize(n), R.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate, beat = std::fmod(t, 0.5);
    seed = seed * 1664525u + 1013904223u;
    const double nz = (static_cast<double>(seed >> 8) / 16777216.0 * 2 - 1) * 0.05;
    const double v = 0.8 * std::exp(-beat * 14) * std::sin(6.2832 * 70 * beat) + 0.2 * std::sin(6.2832 * 440 * t) + nz;
    L[i] = static_cast<float>(v);
    R[i] = static_cast<float>(-0.7 * v + nz);
  }
}

bool sameWave(const LiveWave& a, const LiveWave& b) {
  if (a.frames() != b.frames() || a.levels() != b.levels()) return false;
  for (int k = 0; k < a.levels(); ++k) {
    if (a.buckets(k) != b.buckets(k)) return false;
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < a.buckets(k); ++i) {
        float m1, x1, m2, x2;
        a.bucket(c, k, i, m1, x1);
        b.bucket(c, k, i, m2, x2);
        if (m1 != m2 || x1 != x2) return false;
      }
  }
  return true;
}

void testFill() {
  const double rate = 48000;
  std::vector<float> L, R;
  signal(static_cast<size_t>(rate * 3.3), rate, L, R);
  LiveWave w;
  w.reset(rate, 2);
  // a simulated recording: blocks of uneven size, as the rings drain
  size_t at = 0, k = 0;
  const size_t sizes[] = {1, 511, 128, 4096, 37, 1024, 129, 7777};
  while (at < L.size()) {
    const size_t n = std::min(sizes[k++ % 8], L.size() - at);
    w.add(L.data() + at, R.data() + at, n);
    at += n;
  }
  expect(w.frames() == L.size(), "every frame recorded is counted");
  expect(w.buckets(0) == (L.size() + LiveWave::kBase - 1) / LiveWave::kBase, "one level-0 bucket per 128 frames");
  bool peaksOk = true, levelsOk = w.levels() > 1;
  for (int lv = 0; lv < w.levels(); ++lv) {
    const size_t bf = LiveWave::bucketFrames(lv);
    if (w.buckets(lv) != (L.size() + bf - 1) / bf) levelsOk = false;
    for (size_t b = 0; b < w.buckets(lv); ++b)
      for (int c = 0; c < 2; ++c) {
        const std::vector<float>& x = c == 0 ? L : R;
        int16_t mn = 32767, mx = -32767;
        for (size_t i = b * bf; i < std::min(x.size(), (b + 1) * bf); ++i) mn = std::min(mn, q(x[i])), mx = std::max(mx, q(x[i]));
        float a, bb;
        w.bucket(c, lv, b, a, bb);
        if (std::lround(a * 32767.0f) != mn || std::lround(bb * 32767.0f) != mx) peaksOk = false;
      }
  }
  expect(levelsOk, "each level halves the bucket count up to one bucket");
  expect(w.buckets(w.levels() - 1) == 1, "the top level is one bucket (the whole take)");
  expect(peaksOk, "every bucket of every level is the min / max of its audio, per channel");
  LiveWave one;
  one.reset(rate, 2);
  one.add(L.data(), R.data(), L.size());
  expect(sameWave(w, one), "block-size invariant: blocks equal one add of the whole take");

  // truncate and add again (a resume crossfade rewrites the tail)
  LiveWave t;
  t.reset(rate, 2);
  t.add(L.data(), R.data(), 100000);
  const size_t kept = t.truncate(70001);
  expect(kept == 70001 / LiveWave::kBase * LiveWave::kBase && t.frames() == kept, "truncate keeps whole buckets");
  t.add(L.data() + kept, R.data() + kept, L.size() - kept);
  expect(sameWave(t, one), "truncate, then add again from there: the same as a fresh build");
  t.truncate(0);
  expect(t.frames() == 0 && t.buckets(0) == 0 && t.levels() == 1, "truncate to nothing empties it");

  // mono
  LiveWave m;
  m.reset(rate, 1);
  m.add(L.data(), nullptr, 5000);
  float a0, b0, a1, b1;
  m.bucket(0, 0, 3, a0, b0);
  m.bucket(1, 0, 3, a1, b1);
  expect(m.channels() == 1 && a0 == a1 && b0 == b1, "a mono take answers its channel for both");

  // clipping
  std::vector<float> c(1000, 0.2f);
  c[300] = 1.4f;
  LiveWave cw;
  cw.reset(rate, 2);
  cw.add(c.data(), c.data(), c.size());
  expect(cw.clipped(0, 0, 2) && !cw.clipped(0, 0, 0) && cw.clipped(1, cw.levels() - 1, 0), "full-scale buckets are flagged up the levels");
  float lo, hi;
  cw.bucket(0, 0, 2, lo, hi);
  expect(hi == 1.0f, "samples past full scale are drawn at full scale");
  std::vector<float> nan(300, std::nanf(""));
  cw.add(nan.data(), nan.data(), nan.size());
  cw.bucket(0, 0, cw.buckets(0) - 1, lo, hi);
  expect(std::isfinite(lo) && std::isfinite(hi), "NaN input stays finite");
}

// The view while recording: blocks of 512 frames arrive in real time, frames are drawn at
// 60 Hz with jitter; checks every frame.
void testFit() {
  const double rate = 48000, dt = 1.0 / 60.0;
  const float W = 1200, pad = 8;
  LiveFit f;
  f.begin(rate * 10.0, rate);
  double recorded = 0, clock = 0, lastSpan = f.span(W, pad), lastShown = 0;
  bool beforeOk = true, coverOk = true, noJump = true, behindOk = true, smooth = true, reached = false;
  double maxLag = 0;
  uint32_t seed = 3;
  for (int frame = 0; frame < 60 * 40; ++frame) {
    seed = seed * 1664525u + 1013904223u;
    const double jitter = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.006;  // ±3 ms
    clock += dt + jitter;
    while (recorded + 512 <= clock * rate) recorded += 512;  // the blocks that arrived
    f.step(recorded, dt + jitter);
    const double span = f.span(W, pad);
    const double headX = f.shown / span * W;
    if (f.shown > recorded + 1e-9) behindOk = false;
    if (frame > 30) maxLag = std::max(maxLag, (recorded - f.shown) / rate);
    if (f.shown * W / (W - pad) <= rate * 10.0) {
      if (span != rate * 10.0) beforeOk = false;  // the normal scale while it fits
    } else {
      reached = true;
      if (std::fabs(headX - (W - pad)) > 0.01 || span < f.shown) coverOk = false;  // the whole take in view
    }
    if (span < lastSpan - 1e-6 || (span - lastSpan) / lastSpan > 0.02) noJump = false;
    if (frame > 30) {
      const double adv = (f.shown - lastShown) / (rate * dt);
      if (adv < 0.4 || adv > 1.8) smooth = false;
    }
    lastSpan = span;
    lastShown = f.shown;
  }
  expect(behindOk, "the drawn head never passes what was recorded");
  expect(beforeOk, "while the take fits it grows at the view's normal scale");
  expect(reached, "a 40 s take passes the right edge of a 10 s view");
  expect(coverOk, "after the edge the whole take is fitted to the width every frame (head at the edge less the gap)");
  expect(noJump, "the span grows continuously: never shrinks, under 2 % a frame");
  expect(smooth, "the head moves every frame at about the sample rate (no block stutter)");
  char b[96];
  std::snprintf(b, sizeof b, "the head trails the recorded end by little (%.1f ms)", maxLag * 1000);
  expect(maxLag < 0.06, b);

  // a pause: nothing arrives; the head settles on the end and the view holds still
  for (int i = 0; i < 30; ++i) f.step(recorded, dt);
  const double held = f.span(W, pad);
  for (int i = 0; i < 30; ++i) f.step(recorded, dt);
  expect(f.shown == recorded && f.span(W, pad) == held, "paused: the head rests on the end, the view holds");

  // a pre-roll: half a second arrives at once
  LiveFit p;
  p.begin(rate * 10.0, rate);
  p.step(rate * 0.5, dt);
  int frames = 1;
  while (p.shown < rate * 0.5 - 1 && frames < 100) p.step(rate * 0.5, dt), ++frames;
  expect(p.shown <= rate * 0.5 && frames < 20, "a pre-roll burst glides in within a third of a second");
  // a clear
  p.step(0, dt);
  expect(p.shown == 0, "a cleared take resets the head");
  // a long stall catches up at once
  p.step(rate * 30, dt);
  expect(p.shown > rate * 29.9, "a long stall catches up at once");
  // appending after existing audio: the head starts at its end
  LiveFit ap;
  ap.begin(rate * 70, rate, rate * 60);
  expect(ap.shown == rate * 60 && ap.span(W, pad) == rate * 70, "adding after existing audio: the take starts at its end");
}

void testTrace() {
  const double rate = 48000;
  std::vector<float> L, R;
  signal(static_cast<size_t>(rate * 20), rate, L, R);
  LiveWave w;
  w.reset(rate, 2);
  w.add(L.data(), R.data(), L.size());
  std::vector<LiveWavePoint> pts;
  LiveWaveView v;
  v.span = static_cast<double>(L.size());
  v.width = 1000;
  v.pxScale = 1;
  traceLiveWave(w, 0, v, pts);
  bool mono = true;
  for (size_t i = 1; i < pts.size(); ++i) mono = mono && pts[i].x >= pts[i - 1].x;
  float hi = -1, lo = 1, shi = -1, slo = 1;
  for (const auto& p : pts) hi = std::max(hi, p.hi), lo = std::min(lo, p.lo);
  for (float x : L) shi = std::max(shi, x), slo = std::min(slo, x);
  expect(!pts.empty() && pts.front().x == 0 && std::fabs(pts.back().x - 1000) < 1e-3f && mono, "the trace spans the lane left to right");
  expect(std::fabs(hi - shi) < 2e-4f && std::fabs(lo - slo) < 2e-4f, "the traced peaks are the audio's");
  expect(pts.size() <= 2 * 1000 + 3, "one to two points a pixel");
  // the drawn end
  v.end = rate * 5;
  traceLiveWave(w, 0, v, pts);
  expect(std::fabs(pts.back().x - 250) < 1e-3f, "the trace ends at the drawn head");
  v.end = -1;
  // mirrored: the louder channel, both ways
  traceLiveWave(w, -2, v, pts);
  bool mirror = true;
  for (const auto& p : pts) mirror = mirror && p.lo == -p.hi && p.hi >= 0;
  expect(mirror, "the mirrored trace is the louder channel's peak");

  // a level change is continuous: just under and just over two buckets a pixel
  auto sampleAt = [&](double fpp, std::vector<float>& out) {
    LiveWaveView u;
    u.span = fpp * 1000;
    u.width = 1000;
    traceLiveWave(w, 0, u, pts);
    // compared at the same frames (every 200th frame-per-pixel step), whatever the scale
    out.assign(900, 0);
    size_t j = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      const auto x = static_cast<float>(static_cast<double>(i) * 256.0 / fpp);
      while (j + 1 < pts.size() && pts[j + 1].x <= x) ++j;
      const LiveWavePoint& p0 = pts[j];
      const LiveWavePoint& p1 = pts[std::min(j + 1, pts.size() - 1)];
      const float u = p1.x > p0.x ? std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) : 0.0f;
      out[i] = p0.hi + (p1.hi - p0.hi) * u;  // the outline as drawn (straight between points)
    }
  };
  std::vector<float> a, b;
  const double edge = static_cast<double>(LiveWave::bucketFrames(1));
  sampleAt(edge * 0.999, a);
  sampleAt(edge * 1.001, b);
  double diff = 0, diffFar = 0;
  for (size_t i = 0; i < a.size(); ++i) diff += std::fabs(a[i] - b[i]);
  std::vector<float> c;
  sampleAt(edge * 0.75, c);  // a real change of scale, for comparison
  for (size_t i = 0; i < a.size(); ++i) diffFar += std::fabs(a[i] - c[i]);
  char m[128];
  std::snprintf(m, sizeof m, "a level change is continuous (difference %.4f vs %.4f for a 25%% zoom)", diff / 1000, diffFar / 1000);
  expect(diff < diffFar * 0.25, m);

  // an hour: memory bounded, every frame's trace costs the same as for a short take
  LiveWave hour;
  hour.reset(rate, 2);
  std::vector<float> blk(48000);
  for (size_t i = 0; i < blk.size(); ++i) blk[i] = static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 0.05));
  for (int s = 0; s < 3600; ++s) hour.add(blk.data(), blk.data(), blk.size());
  expect(hour.frames() == static_cast<size_t>(rate * 3600), "an hour of stereo at 48 kHz");
  char mb[96];
  std::snprintf(mb, sizeof mb, "an hour's pyramid stays small (%.1f MB)", static_cast<double>(hour.memoryBytes()) / 1e6);
  expect(hour.memoryBytes() < 48u * 1000 * 1000, mb);
  LiveWaveView hv;
  hv.span = static_cast<double>(hour.frames()) * 1.003;
  hv.width = 1700;
  hv.pxScale = 2;
  const auto t0 = std::chrono::steady_clock::now();
  size_t most = 0;
  for (int i = 0; i < 200; ++i) {
    hv.span *= 1.0001;
    traceLiveWave(hour, 0, hv, pts);
    traceLiveWave(hour, 1, hv, pts);
    most = std::max(most, pts.size());
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 200.0;
  char tb[128];
  std::snprintf(tb, sizeof tb, "an hour-long take, two lanes at 200 %%: %zu points, %.3f ms a frame", most, ms);
#ifdef NDEBUG
  const double budget = 4.0;  // a frame's share of the drawing, optimised
#else
  const double budget = 40.0;  // debug and sanitizer builds
#endif
  expect(most <= 2 * 3400 + 3 && ms < budget, tb);
}

void testRingNoAlloc() {
  auto* ring = new packs::edison::CaptureRing();
  std::vector<float> L(512, 0.1f), R(512, -0.1f);
  gWatch = true;
  gAllocs = 0;
  for (int i = 0; i < 4000; ++i) ring->write(L.data(), R.data(), 512, (i & 1) != 0);
  gWatch = false;
  expect(gAllocs == 0 && ring->written() == 4000u * 512u, "the capture ring the recorders drain does not allocate on the audio thread");
  delete ring;
}

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11  // (GCC 11 added this warning)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
  testFill();
  testFit();
  testTrace();
  testRingNoAlloc();
  return finish();
}
