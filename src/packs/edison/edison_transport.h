// Gridline - "edison" pack: Loupe's playback rules, pure (the editor and tests share them).
//  * what Play plays: the selection; else, looping, the loop points (from the cursor when it
//    is inside them) or the whole file from the cursor; else the cursor to the end;
//  * a new range while playing: the playback keeps its place when that lies inside the new
//    range, else it goes to the range's start (EdisonFx does the same on the audio thread);
//  * the selection edge under the pointer (dragging an edge moves only that edge);
//  * scrubbing outside a mixer slot: the position follows the pointer at the speed it moves
//    (reaching it within 40 ms, at most four times normal speed), heard as short grains read
//    at that speed, each starting where the last one has got to.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "edison.h"

namespace gl::packs::edison::transport {

struct PlayRange {
  int64_t from = 0, to = 0;  // [from, to): the part that plays (and loops)
  int64_t start = 0;         // where playback starts
  bool loop = false;
};

inline PlayRange playRange(int64_t frames, Range sel, bool hasLoopPoints, int64_t loopStart, int64_t loopEnd, bool loopOn, int64_t cursor) {
  PlayRange r;
  r.loop = loopOn;
  const int64_t n = std::max<int64_t>(0, frames);
  const int64_t cur = cursor >= 0 && cursor < n ? cursor : 0;
  sel.a = std::clamp<int64_t>(sel.a, 0, n);
  sel.b = std::clamp<int64_t>(sel.b, 0, n);
  if (!sel.empty()) {
    r.from = sel.a, r.to = sel.b, r.start = sel.a;
  } else if (loopOn && hasLoopPoints && loopEnd > loopStart && loopStart < n) {
    r.from = std::max<int64_t>(0, loopStart), r.to = std::min(n, loopEnd);
    r.start = cur >= r.from && cur < r.to ? cur : r.from;
  } else {
    r.from = loopOn ? 0 : cur, r.to = n, r.start = cur;
  }
  return r;
}

// The playback place after the range changed to [from, to) (EdisonFx::setRange).
inline double keepOrJump(double pos, int64_t from, int64_t to) {
  return pos < static_cast<double>(from) || pos >= static_cast<double>(to) ? static_cast<double>(from) : pos;
}

// Which selection edge is under x: 0 none, 1 the start, 2 the end (the nearer one when both
// are within reach).
inline int edgeAt(float x, float x0, float x1, float reach) {
  const float d0 = std::fabs(x - x0), d1 = std::fabs(x - x1);
  if (d0 > reach && d1 > reach) return 0;
  return d0 <= d1 ? 1 : 2;
}

// Scrub follower (frames of the source, seconds of wall time).
struct ScrubFollow {
  double pos = 0;  // where the scrub has got to
  double vel = 0;  // source frames per second (signed)
  // One step of dt seconds towards `target`; frames: the source length, rate: its rate.
  void step(double target, double dt, int64_t frames, double rate) {
    dt = std::clamp(dt, 0.0, 0.1);
    const double maxV = 4.0 * rate;
    const double want = std::clamp((target - pos) / 0.04, -maxV, maxV);
    vel += (want - vel) * std::min(1.0, dt / 0.03);
    pos = std::clamp(pos + vel * dt, 0.0, static_cast<double>(std::max<int64_t>(1, frames) - 1));
    if ((pos <= 0 && vel < 0) || (pos >= static_cast<double>(frames - 1) && vel > 0)) vel = 0;
  }
  // Slower than this the scrub is silent (the pointer rests).
  bool audible(double rate) const { return std::fabs(vel) >= 0.02 * rate; }
};

// A grain of `outFrames` frames read from src at `pos`, `speed` source frames per output
// frame (negative: backwards), at the source's rate. fadeIn: a 2 ms fade at the start (the
// first grain); every grain fades out over its last 10 ms (it ends quietly when no new grain
// replaces it).
inline Audio scrubGrain(const Audio& src, double pos, double speed, int64_t outFrames, bool fadeIn) {
  Audio g;
  g.rate = src.rate;
  g.channels = src.channels;
  const int64_t n = src.frames();
  if (n < 2 || outFrames <= 0) return g;
  g.L.assign(static_cast<size_t>(outFrames), 0.f);
  g.R.assign(static_cast<size_t>(outFrames), 0.f);
  const int64_t fin = fadeIn ? std::max<int64_t>(1, static_cast<int64_t>(src.rate * 0.002)) : 0;
  const int64_t fout = std::min(outFrames, std::max<int64_t>(1, static_cast<int64_t>(src.rate * 0.010)));
  for (int64_t k = 0; k < outFrames; ++k) {
    const double p = pos + speed * static_cast<double>(k);
    if (p < 0 || p > static_cast<double>(n - 1)) break;
    const auto i = static_cast<int64_t>(p);
    const int64_t j = std::min(n - 1, i + 1);
    const float t = static_cast<float>(p - static_cast<double>(i));
    float gain = 1.f;
    if (k < fin) gain *= static_cast<float>(k) / static_cast<float>(fin);
    if (k >= outFrames - fout) gain *= static_cast<float>(outFrames - 1 - k) / static_cast<float>(fout);
    const auto si = static_cast<size_t>(i), sj = static_cast<size_t>(j), sk = static_cast<size_t>(k);
    g.L[sk] = gain * (src.L[si] + (src.L[sj] - src.L[si]) * t);
    g.R[sk] = gain * (src.R[si] + (src.R[sj] - src.R[si]) * t);
  }
  return g;
}

}  // namespace gl::packs::edison::transport
