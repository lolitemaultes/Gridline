// Gridline tests: Loupe's playback rules (packs/edison/edison_transport.h) and the slot
// player (EdisonFx):
//  * Play plays the selection; with loop on and no selection the loop points (from the
//    cursor when it is inside them) or the whole file from the cursor; else cursor to end;
//  * a new selection while playing changes the playing range at once: the place is kept
//    when inside, else playback goes to the new start (the old loop never keeps playing);
//  * play starts at the cursor (not at the start of the last range), and a song transport
//    that Loupe follows starts at the cursor too;
//  * selection edges are picked within reach; scrubbing follows the pointer at its speed;
//  * scrub grains read the audio at that speed, forwards and backwards, and stay finite.
#include <cmath>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "engine/Effects.h"
#include "packs/edison/edison.h"
#include "packs/edison/edison_transport.h"

using namespace gl;
using namespace gl::packs::edison;
namespace tp = gl::packs::edison::transport;

namespace {

Audio ramp(int64_t n, double rate = 44100) {
  Audio a = makeAudio(rate, 2, n);
  for (int64_t i = 0; i < n; ++i) {
    a.L[static_cast<size_t>(i)] = static_cast<float>(i) / 100000.f;
    a.R[static_cast<size_t>(i)] = -static_cast<float>(i) / 200000.f;
  }
  return a;
}

// Runs the player for n frames and returns the left output.
std::vector<float> run(EdisonFx& fx, const PluginState& st, const ChannelAux& aux, ProcessContext& ctx, int n) {
  std::vector<float> L(static_cast<size_t>(n), 0.f), R(static_cast<size_t>(n), 0.f);
  fx.processAux(st, aux, ctx, L.data(), R.data(), n);
  return L;
}

}  // namespace

int main() {
  // ---- what Play plays
  {
    const Range none{};
    tp::PlayRange r = tp::playRange(1000, none, false, 0, 0, false, 250);
    expect(r.from == 250 && r.to == 1000 && r.start == 250 && !r.loop, "no selection: from the cursor to the end");
    r = tp::playRange(1000, Range{100, 300}, false, 0, 0, false, 250);
    expect(r.from == 100 && r.to == 300 && r.start == 100, "a selection plays the selection");
    r = tp::playRange(1000, none, true, 400, 600, true, 450);
    expect(r.from == 400 && r.to == 600 && r.start == 450 && r.loop, "loop on, loop points: loops them, from the cursor inside them");
    r = tp::playRange(1000, none, true, 400, 600, true, 50);
    expect(r.from == 400 && r.start == 400, "... from the loop start when the cursor is outside");
    r = tp::playRange(1000, none, false, 0, 0, true, 700);
    expect(r.from == 0 && r.to == 1000 && r.start == 700 && r.loop, "loop on, no loop points: the whole file loops, from the cursor");
    r = tp::playRange(1000, none, false, 0, 0, false, 1000);
    expect(r.start == 0 && r.from == 0, "cursor at the end: from the start");
    r = tp::playRange(1000, Range{900, 5000}, false, 0, 0, false, 0);
    expect(r.from == 900 && r.to == 1000, "a selection past the end is clamped");
  }
  // ---- a new range while playing
  {
    expect(tp::keepOrJump(150, 100, 300) == 150, "inside the new range: the place is kept");
    expect(tp::keepOrJump(50, 100, 300) == 100 && tp::keepOrJump(300, 100, 300) == 100, "outside: the new start");
  }
  // ---- selection edges
  {
    expect(tp::edgeAt(103, 100, 400, 5) == 1, "near the start edge");
    expect(tp::edgeAt(398, 100, 400, 5) == 2, "near the end edge");
    expect(tp::edgeAt(250, 100, 400, 5) == 0, "inside the selection: no edge");
    expect(tp::edgeAt(102, 100, 104, 5) == 1 && tp::edgeAt(103.5f, 100, 104, 5) == 2, "a narrow selection: the nearer edge");
  }
  // ---- scrub follower
  {
    const double rate = 44100;
    tp::ScrubFollow f;
    f.pos = 10000;
    for (int i = 0; i < 6; ++i) f.step(10000, 1.0 / 60, 100000, rate);
    expect(!f.audible(rate), "the pointer rests: silent");
    for (int i = 0; i < 30; ++i) f.step(10000 + (i + 1) * 735.0, 1.0 / 60, 100000, rate);  // 1x speed to the right
    expect(f.vel > 0.7 * rate && f.vel < 1.3 * rate && f.audible(rate), "dragging at normal speed: about normal speed forwards");
    expect(std::fabs(f.pos - (10000 + 30 * 735.0)) < 0.1 * rate, "... and close behind the pointer");
    tp::ScrubFollow b;
    b.pos = 50000;
    for (int i = 0; i < 30; ++i) b.step(50000 - (i + 1) * 1470.0, 1.0 / 60, 100000, rate);
    expect(b.vel < -1.5 * rate && b.vel > -2.5 * rate, "dragging left at double speed: double speed backwards");
    tp::ScrubFollow c;
    c.pos = 0;
    for (int i = 0; i < 60; ++i) c.step(99999, 1.0 / 60, 100000, rate);
    expect(c.vel <= 4.0 * rate + 1, "a fling is capped at four times normal speed");
    tp::ScrubFollow e;
    e.pos = 100;
    for (int i = 0; i < 30; ++i) e.step(-5000, 1.0 / 60, 100000, rate);
    expect(e.pos == 0 && e.vel == 0, "the scrub stops at the start of the file");
  }
  // ---- scrub grains
  {
    Audio src = ramp(20000);
    Audio g = tp::scrubGrain(src, 1000, 1.0, 2000, false);
    bool same = true;
    for (int k = 0; k < 1000; ++k) same = same && g.L[static_cast<size_t>(k)] == src.L[static_cast<size_t>(1000 + k)];
    expect(g.frames() == 2000 && same, "a grain at normal speed reads the audio as it is");
    expect(std::fabs(g.L[1999]) < 1e-6f, "... and fades out at its end");
    Audio b = tp::scrubGrain(src, 5000, -2.0, 1000, false);
    expect(std::fabs(b.L[100] - src.L[5000 - 200]) < 1e-6f, "backwards at double speed");
    Audio fi = tp::scrubGrain(src, 5000, 1.0, 1000, true);
    expect(fi.L[0] == 0.f && fi.L[200] == src.L[5200], "the first grain fades in over 2 ms");
    Audio edge = tp::scrubGrain(src, 19990, 4.0, 1000, false);
    bool finite = true;
    for (float v : edge.L) finite = finite && std::isfinite(v);
    expect(finite && edge.L[10] == 0.f, "a grain running off the end is silent there, never out of bounds");
    expect(tp::scrubGrain(Audio{}, 0, 1, 100, true).frames() == 0, "no audio: no grain");
  }
  // ---- the slot player: start, new range while playing, a following song transport
  {
    Audio a = ramp(20000);
    ChannelAux aux;
    aux.samples.push_back(toSample(a, "mem"));
    PluginState st = defaultPluginState(kType);
    ProcessContext ctx;
    EdisonFx fx;
    fx.play(0, 20000, false, 5000);
    std::vector<float> L = run(fx, st, aux, ctx, 64);
    expect(L[0] == a.L[5000] && L[63] == a.L[5063], "play starts at the cursor (start), not at the range's start");
    // Looping 1000..2000, then the selection becomes 1500..1800 while playing (inside: kept).
    fx.play(1000, 2000, true);
    run(fx, st, aux, ctx, 600);  // at 1600
    fx.setRange(1500, 1800);
    L = run(fx, st, aux, ctx, 300);  // 1600..1800 then wraps to 1500
    expect(L[0] == a.L[1600] && L[199] == a.L[1799] && L[200] == a.L[1500], "a new selection inside: the place is kept, the loop is the new one");
    // A selection elsewhere: at once to its start, looping it (never the old loop).
    fx.setRange(8000, 8100);
    L = run(fx, st, aux, ctx, 150);
    expect(L[0] == a.L[8000] && L[100] == a.L[8000], "a new selection elsewhere: plays it at once and loops it");
    fx.stop();
    run(fx, st, aux, ctx, 16);
    // Follow the song transport: starts where the cursor was set (setRange while stopped).
    st.params[EdFollowHost] = 1;
    fx.setRange(0, 20000, 12345);
    ctx.playing = false;
    run(fx, st, aux, ctx, 16);
    ctx.playing = true;
    L = run(fx, st, aux, ctx, 32);
    expect(L[0] == a.L[12345] && fx.isPlaying(), "a followed song transport starts at the cursor");
    ctx.playing = false;
    run(fx, st, aux, ctx, 16);
    fx.setRange(0, 20000, 3000);  // the cursor moved while stopped
    run(fx, st, aux, ctx, 16);
    ctx.playing = true;
    L = run(fx, st, aux, ctx, 32);
    expect(L[0] == a.L[3000], "... and from the new cursor next time (not where play was last pressed)");
  }
  return finish();
}
