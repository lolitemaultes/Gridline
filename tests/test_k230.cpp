// Gridline tests - K-230: one clean snapping for every delay time knob (core/TimeKnob.h).
//  * every delay's time knob has the snapping: note values when synced, round ms when free;
//  * the knob travels evenly: each doubling of time takes the same room (no crowded end);
//  * a drag near a note value lands exactly on it, between note values it moves freely, in
//    whole ticks, never backwards and without jumps;
//  * the readout names the value ("1/8 dotted (321 ms)", "1/4", "0.83 beats", "250 ms").
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/KnobMagnet.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/TimeKnob.h"
#include "packs/mini/mini.h"
#include "packs/spacefx/spacefx.h"

using namespace gl;

namespace {
const ParamDesc* desc(const char* type, int k) {
  const PluginInfo* p = findPlugin(type);
  return p && k >= 0 && static_cast<size_t>(k) < p->params.size() ? &p->params[static_cast<size_t>(k)] : nullptr;
}
}  // namespace

int main() {
  using namespace packs;
  struct Knob {
    const char* type;
    int param;
    timeknob::Unit unit;
  };
  std::vector<Knob> knobs = {
      {"gl.delay", pp::DlyTicks, timeknob::Unit::Ticks},           {"gl.delay", pp::DlyTimeMs, timeknob::Unit::Ms},
      {"gl.banddelay", spacefx::BdMaxTime, timeknob::Unit::Seconds}, {"gl.vphaser", spacefx::VpDelayTicks, timeknob::Unit::Ticks},
      {"gl.vphaser", spacefx::VpDelayTime, timeknob::Unit::Ms},     {"gl.miniecho", mini::echo::Time, timeknob::Unit::Ms},
      {"gl.minivolley", mini::volley::Time, timeknob::Unit::Ms},     {"gl.minirewind", mini::rewind::Length, timeknob::Unit::Ms}};
  for (int tap = 0; tap < spacefx::kTaps; ++tap) {
    knobs.push_back({"gl.multidelay", spacefx::mdParam(tap, spacefx::MdSteps), timeknob::Unit::Steps});
    knobs.push_back({"gl.multidelay", spacefx::mdParam(tap, spacefx::MdTimeMs), timeknob::Unit::Ms});
  }

  // 1. every delay time knob has the snapping, its magnets inside the range, exact through the travel
  bool all = true, inRange = true, exact = true, monotone = true, enough = true;
  for (const Knob& k : knobs) {
    const auto s = timeknob::forParam(k.type, k.param);
    const ParamDesc* d = desc(k.type, k.param);
    if (!s || !d || s->unit != k.unit) {
      all = false;
      std::printf("# no time knob: %s %d\n", k.type, k.param);
      continue;
    }
    if (s->magnets.size() < 5) enough = false;
    double prev = -1;
    for (double m : s->magnets) {
      if (m < d->min - 1e-9 || m > d->max + 1e-9) inRange = false;
      const double p = timeknob::toPos(*s, *d, m);
      if (p <= prev) monotone = false;
      prev = p;
      if (timeknob::toValue(*s, *d, p, true) != m) exact = false;
    }
  }
  expect(all, "every delay's time knob has the time snapping");
  expect(enough, "each has five or more magnets");
  expect(inRange, "every magnet is inside its parameter's range");
  expect(monotone, "the magnets run in order along the knob");
  expect(exact, "a magnet's knob position gives back exactly the magnet");
  expect(!timeknob::forParam("gl.delay", pp::DlyFeedback), "other knobs keep their plain behaviour");
  expect(!timeknob::forParam("gl.reverb", 0), "a reverb's knobs are not delay times");

  // 2. Tracer's synced time: even travel, note values spread out
  const auto tk = *timeknob::forParam("gl.delay", pp::DlyTicks);
  const ParamDesc& td = *desc("gl.delay", pp::DlyTicks);
  auto pos = [&](double t) { return timeknob::toPos(tk, td, t); };
  const double oct1 = pos(96) - pos(48), oct2 = pos(384) - pos(192), oct0 = pos(48) - pos(24);
  expect(std::fabs(oct1 - oct2) / oct2 < 0.15, "1/16 to 1/8 takes about the same drag as 1/4 to 1/2");
  expect(oct0 / oct2 > 0.7, "1/32 to 1/16 is not crowded either");
  double minGap = 1;
  for (size_t i = 1; i < tk.magnets.size(); ++i) minGap = std::min(minGap, pos(tk.magnets[i]) - pos(tk.magnets[i - 1]));
  // the closest pair (1/4 triplet and 1/8 dotted, 1/8 of an octave apart) still has room for both
  expect(minGap * 170 >= 4, "neighbouring note values are 4 px of drag apart or more (no stack of magnets)");
  double straightGap = 1;
  for (double t : {24.0, 48.0, 96.0, 192.0, 384.0, 768.0}) straightGap = std::min(straightGap, pos(t) - pos(t / 2));
  expect(straightGap * 170 >= 15, "straight note values are 15 px of drag apart or more");
  std::printf("# smallest gap %.1f px, 1/16..1/8 %.1f px, 1/4..1/2 %.1f px\n", minGap * 170, oct1 * 170, oct2 * 170);

  // 3. a drag (skin wheel: 170 px of travel) holds on 1/4 near it, moves freely between, whole ticks
  std::vector<double> mags;
  for (double m : tk.magnets) mags.push_back(pos(m));
  const double zone = magnet::kZonePx / 170.0;
  auto dragTo = [&](double s) { return timeknob::toValue(tk, td, magnet::shape(s, mags, zone), true); };
  const double q = pos(192);
  expect(dragTo(q + 2 / 170.0) == 192 && dragTo(q - 2 / 170.0) == 192, "2 px either side of 1/4 holds on 1/4");
  const double mid = (pos(192) + pos(256)) * 0.5;
  const double vmid = dragTo(mid);
  expect(vmid > 192 && vmid < 256 && vmid == std::round(vmid), "halfway to 1/2T the value is free, in whole ticks");
  bool mono = true, jumps = false;
  double prevV = dragTo(0);
  for (int i = 1; i <= 1700; ++i) {
    const double v = dragTo(i / 1700.0);
    if (v < prevV) mono = false;
    // one tenth of a pixel never moves the value by more than a few ticks (no jump)
    if (v - prevV > 4 + v * 0.02) jumps = true;
    prevV = v;
  }
  expect(mono, "a drag never goes backwards");
  expect(!jumps, "a drag never jumps");
  expect(dragTo(0) == 1 && dragTo(1) == 768, "the drag reaches both ends");
  // every note value is caught by a drag that ends within 2 px of it
  bool caught = true;
  for (double m : tk.magnets)
    for (double off : {-0.8, 0.8})
      if (dragTo(pos(m) + off / 170.0) != m && m > 1 && m < 768) caught = false;
  expect(caught, "every note value holds a drag that ends close to it");

  // 4. free time: round milliseconds; linear ms params get the even travel and whole ms
  const auto ms = *timeknob::forParam("gl.delay", pp::DlyTimeMs);
  expect(ms.magnets == std::vector<double>({1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000}), "free time: 1, 2, 5, 10 ... 2000 ms");
  const auto vp = *timeknob::forParam("gl.vphaser", spacefx::VpDelayTime);
  const ParamDesc& vd = *desc("gl.vphaser", spacefx::VpDelayTime);
  expect(vp.offset > 0 && timeknob::toValue(vp, vd, 0.5, true) == std::round(timeknob::toValue(vp, vd, 0.5, false)), "a linear ms knob travels evenly, whole ms");
  expect(timeknob::toPos(ms, *desc("gl.delay", pp::DlyTimeMs), 300) == desc("gl.delay", pp::DlyTimeMs)->toNormalized(300),
         "a log ms knob keeps its own mapping (automation and drawing agree)");

  // 5. readouts
  expect(timeknob::noteName(192) == "1/4" && timeknob::noteName(144) == "1/8 dotted" && timeknob::noteName(64) == "1/8 triplet", "note names");
  expect(timeknob::noteName(144, true) == "1/8 D" && timeknob::noteName(32, true) == "1/16 T", "compact note names");
  expect(timeknob::noteName(768) == "1 bar" && timeknob::noteName(1536) == "2 bars" && timeknob::noteName(12) == "1/64", "bars and 1/64");
  expect(timeknob::noteName(200).empty() && timeknob::noteName(192.5).empty(), "no name between note values");
  expect(timeknob::readout(tk, 144, 140, false) == "1/8 dotted (321 ms)", "readout: name and ms at the tempo");
  expect(timeknob::readout(tk, 160, 140, true) == "0.83 bt", "compact readout between note values");
  expect(timeknob::readout(tk, 160, 120, false) == "0.83 beats (417 ms)", "readout between note values");
  expect(timeknob::readout(ms, 250, 140, false) == "250 ms" && timeknob::readout(ms, 1500, 140, true) == "1.50 s" &&
             timeknob::readout(ms, 2.5, 140, true) == "2.5 ms",
         "ms readouts");
  const auto st = *timeknob::forParam("gl.multidelay", spacefx::mdParam(0, spacefx::MdSteps));
  expect(timeknob::readout(st, 3, 0, true) == "1/8 D" && timeknob::readout(st, 4, 0, true) == "1/4", "Multi Delay steps name their note value");
  return finish();
}
