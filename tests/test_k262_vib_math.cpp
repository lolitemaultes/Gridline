// Gridline: K-262, note vibrato: the math (value at points, fades, delay, synced rates at two
// tempos, centre modes, rate sweeps), the clamps, the project file round trip (old projects load
// unchanged) and a split note keeping its vibrato.
#include <cmath>
#include "Check.h"
#include "core/NoteBend.h"
#include "core/NoteVibrato.h"
#include "core/Project.h"

using namespace gl;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

NoteVibrato plain(float depth, float rate) {
  NoteVibrato v;
  v.depth = depth;
  v.rate = rate;
  v.fadeIn = 0;
  return v;
}

void values() {
  NoteVibrato none;
  expect(!none.active() && vib::value(none, 0.3, 1.0, 0.5) == 0.0, "no depth: no vibrato");
  const NoteVibrato v = plain(50, 5);  // 50 cents, 5 Hz, sine, no fades
  // A sine at 5 Hz: the peak a twentieth of a second in (a quarter cycle), zero at a tenth.
  expect(near(vib::value(v, 0.05, 2.0, 0.5), 0.5), "the peak is the depth (50 cents = 0.5 semitones)");
  expect(near(vib::value(v, 0.10, 2.0, 0.5), 0.0, 1e-9), "half a cycle in: back at the pitch");
  expect(near(vib::value(v, 0.15, 2.0, 0.5), -0.5), "three quarters: the trough");
  // Fade in over the first half: a quarter of the way, half the depth.
  NoteVibrato f = v;
  f.fadeIn = 0.5f;
  expect(near(vib::value(f, 0.05, 0.4, 0.5), 0.5 * 0.25), "fade in: depth grows straight");
  expect(near(vib::envelope(f, 0.0), 0.0) && near(vib::envelope(f, 0.5), 1.0), "fade in: 0 at the start, full at its end");
  // Fade out over the last quarter.
  NoteVibrato o = v;
  o.fadeOut = 0.25f;
  expect(near(vib::envelope(o, 1.0), 0.0) && near(vib::envelope(o, 0.875), 0.5) && near(vib::envelope(o, 0.5), 1.0), "fade out at the end");
  // Delay: silent before it, the wave starts at its own phase 0 after it.
  NoteVibrato d = v;
  d.delay = 0.5f;
  expect(vib::value(d, 0.4, 1.0, 0.5) == 0.0, "nothing during the delay");
  expect(near(vib::value(d, 0.55, 1.0, 0.5), 0.5), "the wave starts after the delay");
  // Curves: positive tension rises early, negative late.
  expect(vib::curve(0.25, 1.0f) > 0.25 && vib::curve(0.25, -1.0f) < 0.25 && near(vib::curve(0.25, 0), 0.25), "fade tension");
}

void synced() {
  NoteVibrato v = plain(100, 0.25f);  // a cycle per sixteenth
  v.sync = true;
  // 120 BPM: a beat is 0.5 s, a sixteenth 0.125 s: 8 Hz. 60 BPM: 4 Hz.
  expect(near(vib::rateHz(v, 0.5), 8.0) && near(vib::rateHz(v, 1.0), 4.0), "synced rate follows the tempo");
  expect(near(vib::value(v, 0.125 / 4, 2.0, 0.5), 1.0), "the peak a quarter of a sixteenth in at 120 BPM");
  expect(near(vib::value(v, 0.25 / 4, 2.0, 1.0), 1.0), "and a quarter of a sixteenth in at 60 BPM");
}

void centres() {
  NoteVibrato v = plain(100, 5);
  auto range = [&](float c) {
    v.centre = c;
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 2000; ++i) {
      const double x = vib::value(v, i / 2000.0, 1.0, 0.5);
      lo = std::min(lo, x), hi = std::max(hi, x);
    }
    return std::make_pair(lo, hi);
  };
  auto [l0, h0] = range(0.5f);
  expect(near(l0, -1, 1e-3) && near(h0, 1, 1e-3), "centre 0.5: around the pitch");
  auto [l1, h1] = range(0.0f);
  expect(near(l1, 0, 1e-3) && near(h1, 1, 1e-3), "centre 0: only above");
  auto [l2, h2] = range(1.0f);
  expect(near(l2, -1, 1e-3) && near(h2, 0, 1e-3), "centre 1: only below");
}

void sweep() {
  NoteVibrato v = plain(100, 4);
  v.rateEnd = 12;
  // The phase over a one-second note: 4 t + 8 t^2 / 2 -> 8 cycles in all (the mean rate).
  expect(near(vib::phaseAt(v, 1.0, 1.0, 0.5), 8.0), "a sweep from 4 to 12 Hz makes 8 cycles in a second");
  auto crossings = [&](double from, double to) {
    int n = 0;
    double prev = vib::value(v, from, 1.0, 0.5);
    for (double t = from; t < to; t += 1e-4) {
      const double x = vib::value(v, t, 1.0, 0.5);
      if ((prev < 0) != (x < 0)) ++n;
      prev = x;
    }
    return n;
  };
  expect(crossings(0.5, 1.0) > crossings(0.0, 0.5), "the second half swings faster");
}

void shapes() {
  for (int s = 0; s < static_cast<int>(vib::Shape::Count); ++s) {
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 1000; ++i) {
      const double w = vib::wave(static_cast<vib::Shape>(s), i / 1000.0);
      lo = std::min(lo, w), hi = std::max(hi, w);
    }
    expect(lo >= -1.0001 && hi <= 1.0001 && hi > 0.95 && lo < -0.95, (std::string("shape spans -1..1: ") + vib::shapeName(static_cast<vib::Shape>(s))).c_str());
  }
}

void clamps() {
  NoteVibrato v;
  v.depth = 5000;
  v.rate = -3;
  v.delay = 4;
  v.fadeIn = NAN;
  v.shape = 99;
  v.phase = 2.25f;
  v.centre = 3;
  vib::clamp(v);
  expect(v.depth == vib::kMaxDepth && v.rate == vib::kMinRateHz && v.delay <= 0.95f && v.fadeIn == 0.25f && v.shape == 0 && near(v.phase, 0.25) &&
             v.centre == 1.0f,
         "every field clamped");
}

void files() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  s.channels.push_back(c);
  Pattern p;
  p.id = s.allocateId();
  Note a(0, 96, 60, 100);
  a.vib.depth = 35;
  a.vib.rate = 0.5f;
  a.vib.sync = true;
  a.vib.delay = 0.1f;
  a.vib.fadeOut = 0.2f;
  a.vib.fadeInCurve = 0.5f;
  a.vib.shape = 1;
  a.vib.phase = 0.25f;
  a.vib.rateEnd = 0.25f;
  a.vib.centre = 0.0f;
  Note b(96, 96, 62, 100);  // no vibrato
  p.notes[c.id] = {a, b};
  s.patterns.push_back(p);
  const Json js = songToJson(s);
  const std::string text = jsonCompact(js);
  Song back;
  std::string err;
  expect(songFromJson(js, back, &err), "the project reads back");
  const Pattern* bp = back.pattern(p.id);
  expect(bp && bp->notes.count(c.id) && bp->notes.at(c.id).size() == 2, "both notes");
  if (bp && bp->notes.count(c.id) && bp->notes.at(c.id).size() == 2) {
    expect(bp->notes.at(c.id)[0].vib == a.vib, "the vibrato round trips");
    expect(!bp->notes.at(c.id)[1].vib.active() && bp->notes.at(c.id)[1] == b, "a note without vibrato stays without");
  }
  expect(text.find("\"vib\"") != std::string::npos, "saved under vib");
  // A project without the key (older files): no vibrato anywhere.
  Song old = s;
  old.pattern(p.id)->notes[c.id][0].vib = NoteVibrato{};
  const std::string t2 = jsonCompact(songToJson(old));
  expect(t2.find("\"vib\"") == std::string::npos, "nothing written for notes without vibrato");
}

void splits() {
  Note n(0, 400, 60, 100);
  n.vib.depth = 40;
  n.vib.delay = 0.0f;
  n.vib.fadeIn = 0.5f;
  n.vib.fadeOut = 0.0f;
  Note l = n, r = n;
  l.length = 200;
  r.start = 200;
  r.length = 200;
  bend::split(n, 200, l, r);
  expect(l.vib.active() && r.vib.active(), "both parts keep the vibrato");
  expect(near(l.vib.fadeIn, 1.0, 1e-6), "the first part fades in over all of it (the old first half)");
  expect(r.vib.fadeIn == 0.0f, "the second part starts at full depth");
}

}  // namespace

int main() {
  values();
  synced();
  centres();
  sweep();
  shapes();
  clamps();
  files();
  splits();
  return finish();
}
