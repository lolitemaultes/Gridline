// Gridline - K-276: blocks on a time grid keep one gap between touching neighbours (app/PixelGrid.h).
// Rows of touching clips (lengths on and off the grid) are laid out the way the playlist does it
// (x = origin + tick * pxPerTick - scroll), at many zooms, fractional origins and scrolls, and the
// interface scales that give whole and fractional device pixels (1, 1.25, 4/3, 1.5, 2, 2.5, 3).
// For each pair of touching clips:
//   1. every clip edge and grid line lies on a whole device pixel;
//   2. the gap is the same for every pair (2 device px per logical px of inset, made whole), at
//      every zoom: clip boxes (1 px in on each side), notes (1 px at the end only), step pads;
//   3. the grid line at the shared time lies inside the gap, starting at the shared edge;
//   4. a clip too short for its insets keeps a device pixel and never covers its right neighbour's box.
// Also the old rounding (round to a whole logical px, then 1 px in), to show the test catches the bug.
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>
#include "Check.h"
#include "app/PixelGrid.h"

using namespace gl;

namespace {
bool onDevice(float x, float dpp) { return std::fabs(x * dpp - std::round(x * dpp)) < 2e-3f; }
int devicePx(float w, float dpp) { return static_cast<int>(std::lround(w * dpp)); }

// The visible gap between two drawn boxes in whole device pixels (columns no box covers), the way a
// screenshot would count it: the boxes' edges rounded to the pixel grid as a rasteriser fills them.
int visibleGap(float right, float left, float dpp) {
  return static_cast<int>(std::lround(left * dpp)) - static_cast<int>(std::lround(right * dpp));
}
}  // namespace

int main() {
  const float scales[] = {1.0f, 1.25f, 4.0f / 3.0f, 1.5f, 2.0f, 2.5f, 3.0f};
  const float zooms[] = {0.031f, 0.05f, 0.0733f, 0.1f, 0.137f, 0.2f, 0.3333f, 0.5f, 0.71f, 1.0f, 1.9f};  // logical px per tick
  const float origins[] = {0.0f, 187.0f, 187.4f, 220.75f};
  const float scrolls[] = {0.0f, 13.3f, 250.5f, 1001.71f};
  const long long gridLens[] = {384, 96, 192, 48, 384, 24, 96, 384, 192, 96, 384, 384, 192, 192, 96, 96, 384, 384};
  const long long oddLens[] = {100, 37, 77, 150, 13, 211, 60, 333, 91, 55, 129, 71, 203, 47, 111, 300, 250, 7, 3, 500};

  bool edgesWhole = true, linesWhole = true, clipGapsEqual = true, noteGapsEqual = true, linesInGap = true, tinyOk = true;
  bool clipGapsSet = true;
  std::set<int> clipGaps, noteGaps;
  int pairs = 0;
  bool oldUneven = false;  // the pre-K-276 rounding gives more than one gap somewhere
  for (float dpp : scales) {
    const int wantClip = 2 * devicePx(pixelgrid::whole(1.0f, dpp), dpp), wantNote = devicePx(pixelgrid::whole(1.0f, dpp), dpp);
    for (float ppt : zooms)
      for (float o : origins)
        for (float sc : scrolls)
          for (int set = 0; set < 2; ++set) {
            std::vector<long long> starts{0};
            const long long* lens = set ? oddLens : gridLens;
            const size_t n = set ? sizeof oddLens / sizeof *oddLens : sizeof gridLens / sizeof *gridLens;
            for (size_t i = 0; i < n; ++i) starts.push_back(starts.back() + lens[i]);
            auto xOf = [&](long long t) { return o + static_cast<float>(t) * ppt - sc; };
            std::set<int> oldGaps;
            for (size_t i = 0; i + 1 < n; ++i) {
              const float xa = xOf(starts[i]), xb = xOf(starts[i + 1]), xc = xOf(starts[i + 2]);
              const pixelgrid::Span a = pixelgrid::block(xa, xb, dpp), b = pixelgrid::block(xb, xc, dpp);
              edgesWhole = edgesWhole && onDevice(a.x0, dpp) && onDevice(a.x1, dpp) && onDevice(b.x0, dpp) && onDevice(b.x1, dpp);
              const pixelgrid::Span l = pixelgrid::line(xb, dpp);
              linesWhole = linesWhole && onDevice(l.x0, dpp) && onDevice(l.x1, dpp);
              // A clip is "regular" when its snapped span leaves room for both insets and a pixel.
              const bool regular = pixelgrid::edge(xb, dpp) - pixelgrid::edge(xa, dpp) >= 3 * pixelgrid::whole(1.0f, dpp) - 1e-4f;
              const int g = visibleGap(a.x1, b.x0, dpp);
              if (regular) {
                ++pairs;
                clipGaps.insert(g);
                if (g != wantClip) clipGapsEqual = false;
                if (!(l.x0 >= a.x1 - 1e-4f && l.x1 <= b.x0 + 1e-4f && std::fabs(l.x0 - pixelgrid::edge(xb, dpp)) < 1e-4f)) linesInGap = false;
              } else {
                // 4: a tiny clip keeps a device pixel and stops short of its neighbour's box (unless it
                // snaps to no pixel at all: then it still shows one, over the gap).
                const bool zero = pixelgrid::edge(xb, dpp) == pixelgrid::edge(xa, dpp);
                if (devicePx(a.w(), dpp) < 1 || (!zero && visibleGap(a.x1, b.x0, dpp) < 0)) tinyOk = false;
              }
              // Notes: no inset at the start, 1 px at the end (PianoRoll.cpp).
              const pixelgrid::Span na = pixelgrid::block(xa, xb, dpp, 0.0f, 1.0f), nb = pixelgrid::block(xb, xc, dpp, 0.0f, 1.0f);
              if (pixelgrid::edge(xb, dpp) - pixelgrid::edge(xa, dpp) >= 2 * pixelgrid::whole(1.0f, dpp) - 1e-4f) {
                const int ng = visibleGap(na.x1, nb.x0, dpp);
                noteGaps.insert(ng);
                if (ng != wantNote) noteGapsEqual = false;
              }
              // The old rule: round to a whole logical px, then 1 logical px in.
              if (regular) oldGaps.insert(visibleGap(std::round(xb) - 1, std::round(xb) + 1, dpp));
            }
            if (oldGaps.size() > 1) oldUneven = true;
          }
    if (clipGaps.size() > 1) clipGapsSet = false;
    std::printf("# scale %.3f: clip gaps {", static_cast<double>(dpp));
    for (int g : clipGaps) std::printf(" %d", g);
    std::printf(" } device px, note gaps {");
    for (int g : noteGaps) std::printf(" %d", g);
    std::printf(" }\n");
    clipGaps.clear(), noteGaps.clear();
  }
  std::printf("# %d touching pairs checked\n", pairs);
  expect(pairs > 10000, "many touching clip pairs measured");
  expect(edgesWhole, "every clip edge lies on a whole device pixel");
  expect(linesWhole, "every grid line lies on whole device pixels");
  expect(clipGapsEqual && clipGapsSet, "touching clips: one gap (two inset px) at every zoom, origin and scroll, per scale");
  expect(noteGapsEqual, "touching notes: one gap (the end inset) at every zoom, per scale");
  expect(linesInGap, "the grid line at the shared time starts at the shared edge, inside the gap");
  expect(tinyOk, "a clip too short for its insets keeps a pixel and never covers its neighbour");
  expect(oldUneven, "the old per-edge rounding gives uneven gaps at fractional scales (the bug this guards)");

  // Step pads (ChannelRack.cpp): pitch stepW + 2, a group of four 4 px further; the gap to the next pad
  // stays one value inside a group and one between groups at every scale and origin.
  bool padsEqual = true, padsWhole = true;
  for (float dpp : scales)
    for (float stepW : {9.0f, 14.0f, 21.0f, 26.0f})
      for (float o : origins) {
        auto pos = [&](int k) { return o + static_cast<float>(k) * (stepW + 2.0f) + static_cast<float>(k / 4) * 4.0f; };
        std::set<int> inGroup, between;
        for (int k = 0; k < 31; ++k) {
          const float x = pos(k), nx = pos(k + 1), nnx = pos(k + 2);
          const pixelgrid::Span p = pixelgrid::block(x, nx, dpp, 0.0f, nx - x - stepW), q = pixelgrid::block(nx, nnx, dpp, 0.0f, nnx - nx - stepW);
          padsWhole = padsWhole && onDevice(p.x0, dpp) && onDevice(p.x1, dpp);
          ((k + 1) % 4 == 0 ? between : inGroup).insert(visibleGap(p.x1, q.x0, dpp));
        }
        if (inGroup.size() != 1 || between.size() != 1) padsEqual = false;
      }
  expect(padsWhole, "step pads on whole device pixels");
  expect(padsEqual, "step pads: one gap inside a group and one between groups, at every scale");

  // The helper's small rules.
  expect(pixelgrid::edge(10.3f, 1.0f) == 10.0f && pixelgrid::edge(10.3f, 2.0f) == 10.5f && pixelgrid::edge(10.3f, 1.25f) == 10.4f, "edge() snaps to device px");
  expect(pixelgrid::whole(1.0f, 1.25f) == 0.8f && pixelgrid::whole(1.0f, 2.0f) == 1.0f && pixelgrid::whole(0.1f, 1.0f) == 1.0f, "whole(): whole device px, at least one");
  {
    const pixelgrid::Span s = pixelgrid::block(100.2f, 160.7f, 1.0f);  // at 1x: the old round(x0) + 1, round(x1) - 1
    expect(s.x0 == 101.0f && s.x1 == 160.0f, "block() at 1x: the clip box as before");
  }
  return finish();
}
