// Gridline — κ3 tests: multi-mode envelope maths, Time Gate (36-slot banks,
// time remap, sync, one-shot, link, hold, volume tension), and the stereo /
// creative effects rebuilt to the reference behaviour.
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Effects.h"
#include "engine/EffectsK3.h"
#include "engine/MEnv.h"
#include "engine/TimeGateSlots.h"
#include "engine/WaveCurve.h"
#include "packs/dynamics/dynamics.h"
#include "packs/utility/utility.h"
#include "packs/creative/creative.h"
#include "packs/creative/octafilter_dsp.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPI = 3.14159265358979323846;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

PluginState st(const std::string& type, std::initializer_list<std::pair<int, float>> set = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

ProcessContext ctx(bool playing, double tick, double tempo = 120) {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = tempo;
  c.ppq = 96;
  c.tick = tick;
  c.playing = playing;
  return c;
}

struct Buf {
  std::vector<float> l, r;
};

// Runs an effect while playing at 120 BPM (spb = 22050 samples). `before(fx, block start sample, state)`
// may change the state between blocks.
Buf run(Effect& fx, PluginState s, const Buf& in, int block, const std::function<void(size_t, PluginState&)>& before = {}) {
  Buf out = in;
  double tick = 0;
  for (size_t p = 0; p < in.l.size(); p += static_cast<size_t>(block)) {
    if (before) before(p, s);
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.l.size() - p));
    fx.process(s, ctx(true, tick), out.l.data() + p, out.r.data() + p, n);
    tick += n / kSr * 2.0 * 96;
  }
  return out;
}

Buf ramp(size_t n) {  // sample index / 1e6 (distinct, smooth values)
  Buf b;
  b.l.resize(n), b.r.resize(n);
  for (size_t i = 0; i < n; ++i) b.l[i] = b.r[i] = static_cast<float>(static_cast<double>(i) * 1e-6);
  return b;
}
Buf sine(double hz, size_t n, double amp = 0.5) {
  Buf b;
  b.l.resize(n), b.r.resize(n);
  for (size_t i = 0; i < n; ++i) b.l[i] = b.r[i] = static_cast<float>(amp * std::sin(2 * kPI * hz * static_cast<double>(i) / kSr));
  return b;
}
double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}

// ---------------------------------------------------------------- envelope maths
void testMEnv() {
  using namespace menv;
  Env<16> e;
  e.add(0, 0.f), e.add(1, 1.f, 0.f, SingleCurve);
  e.prepare();
  expect(near(e.eval(0.25), 0.25, 1e-9) && near(e.eval(0.5), 0.5, 1e-9), "menv: zero tension is linear");
  e.t[1] = 0.5f, e.prepare();
  double C = kLn1e6 * 0.5, B = 1.0 / (std::exp(C) - 1.0);
  expect(near(e.eval(0.3), 1.0 - B * (std::exp(C * 0.7) - 1.0), 1e-6) && e.eval(0.3) > 0.3, "menv: single curve, positive tension = fast start (exp, ln 1e6)");
  e.t[1] = -0.5f, e.prepare();
  expect(near(e.eval(0.3), B * (std::exp(C * 0.3) - 1.0), 1e-6) && e.eval(0.3) < 0.3, "menv: negative tension = slow start");
  e.mode[1] = DoubleCurve, e.t[1] = 0.4f, e.prepare();
  expect(near(e.eval(0.5), 0.5, 1e-6) && near(e.eval(0.2) + e.eval(0.8), 1.0, 1e-6), "menv: double curve is point-symmetric");
  e.mode[1] = Hold, e.prepare();
  expect(near(e.eval(0.99), 0.0, 1e-9) && near(e.eval(1.0), 1.0, 1e-9), "menv: hold keeps the previous value until the point");
  // Stair / pulse counts from the tension (reference preset checks).
  expect(stepCount(0.425f) + 1 + 1 == 32 && stepCount(0.335f) + 2 == 64 && stepCount(0.515f) + 2 == 16, "menv: stairs step counts 32 / 64 / 16");
  expect(stepCount(0.54f) + 1 == 12 && stepCount(0.51f) + 1 == 16 && stepCount(0.455f) + 1 == 24, "menv: pulse counts 12 / 16 / 24");
  e.mode[1] = Stairs, e.t[1] = 0.425f, e.prepare();
  int changes = 0;
  double prev = e.eval(0);
  for (int i = 1; i < 3200; ++i) {
    double v = e.eval(i / 3200.0);
    if (std::fabs(v - prev) > 1e-9) ++changes;
    prev = v;
  }
  expect(changes == 31 && near(e.eval(0.999), 1.0, 1e-6), "menv: 32 stairs reach the end value");
  e.mode[1] = Pulse, e.t[1] = 0.54f, e.prepare();
  int ones = 0;
  for (int i = 0; i < 2400; ++i) ones += e.eval((i + 0.5) / 2400.0) > 0.5 ? 1 : 0;
  expect(near(ones / 2400.0, 0.5, 0.01), "menv: pulse alternates (half the time high)");
  e.mode[1] = Wave, e.t[1] = -0.6f, e.prepare();
  expect(near(e.eval(0), 0.0, 1e-6) && e.eval(0.5) >= 0 && e.eval(0.5) <= 1, "menv: sine wave starts at the first value");
  e.mode[1] = HalfSine, e.t[1] = 1.f, e.prepare();
  expect(near(e.eval(0.5), std::sin(kPI * 0.25), 1e-6), "menv: half sine (full tension)");
  e.mode[1] = SingleCurve2, e.t[1] = 0.5f, e.prepare();
  {
    double f = 0.4, g = 1 - f;
    expect(near(e.eval(f), f - 0.5 * (std::pow(g, 6) - g), 1e-6), "menv: single curve 2 polynomial");
  }
  e.mode[1] = SingleCurve3, e.t[1] = 0.f, e.prepare();
  expect(near(e.eval(0.0), 0.0, 1e-4) && near(e.eval(1.0), 1.0, 1e-9), "menv: single curve 3 ends");
  // Smooth: monotone through three points.
  Env<16> sm;
  sm.add(0, 0.f), sm.add(1, 0.6f, 0, Smooth), sm.add(2, 1.f, 0, Smooth);
  sm.prepare();
  bool mono = true;
  double pv = -1;
  for (int i = 0; i <= 200; ++i) {
    double v = sm.eval(i / 100.0);
    if (v < pv - 1e-9) mono = false;
    pv = v;
  }
  expect(mono && near(sm.eval(1.0), 0.6, 1e-6), "menv: smooth segments stay monotone and pass through points");
}

// ---------------------------------------------------------------- Time Gate
std::string slotLine(char bank, int k, const std::function<void(tg::Slot&)>& f) {
  auto s = std::make_unique<tg::Slot>();
  s->len = 4;
  f(*s);
  return tg::serialize(bank, k, *s) + "\n";
}

void testTimeGate() {
  const size_t spb = static_cast<size_t>(kSr * 0.5);  // 120 BPM
  // Data model.
  {
    auto b = std::make_unique<tg::Bank>();
    tg::factory(*b);
    expect(tg::kSlots == 36 && b->time[35].env.n >= 2 && b->vol[35].env.n >= 2, "time gate: 36 + 36 slots");
    auto c = std::make_unique<tg::Bank>();
    tg::factory(*c);
    tg::parse("T5 2 Mine|0,0,0 0.5,0.25,0.3 1,0.5,0", *c);  // 0.5 data
    const tg::Slot& s = c->time[4];
    expect(s.env.n == 3 && near(s.env.x[1], 1.0, 1e-6) && near(s.env.y[2], tg::timeY(1.0), 1e-6) && std::string(s.name) == "Mine",
           "time gate: 0.5 slot lines convert (x in beats, delay = y x len)");
    std::string up = tg::upgrade("T5 2 Mine|0,0,0 0.5,0.25,0.3 1,0.5,0\n");
    auto d = std::make_unique<tg::Bank>();
    tg::factory(*d);
    tg::parse(up.c_str(), *d);
    expect(up.rfind("GB2", 0) == 0 && d->time[4].env.n == 3 && near(d->time[4].env.y[2], s.env.y[2], 1e-6), "time gate: upgrade keeps edited slots");
    std::string line = slotLine('V', 20, [](tg::Slot& x) {
      x.setName("Opts");
      x.env.add(0, 1.f), x.env.add(4, 0.f, 0.5f, menv::Pulse);
      x.trig = 2, x.pos = 0, x.force = 2, x.link = true, x.keyHeld = true, x.oneShot = true;
    });
    auto e = std::make_unique<tg::Bank>();
    tg::factory(*e);
    tg::parse(("GB2\n" + line).c_str(), *e);
    const tg::Slot& o = e->vol[20];
    expect(o.trig == 2 && o.pos == 0 && o.force == 2 && o.link && o.keyHeld && o.oneShot && o.env.mode[1] == menv::Pulse && near(o.env.t[1], 0.5, 1e-6),
           "time gate: per-slot options and point modes round-trip");
  }
  // 2-beat repeat: hold at y = 0.75 (2 beats back) from beat 2 → beats 2..4 replay beats 0..2.
  {
    std::string data = "GB2\n" + slotLine('T', 0, [](tg::Slot& x) {
                         x.env.add(0, 1.f), x.env.add(2, 1.f, 0, menv::Hold), x.env.add(2, 0.75f, 0, menv::Hold), x.env.add(4, 0.75f, 0, menv::Hold);
                       });
    PluginState s = st("gl.timegate", {{TgTimeSlot, 1.f}, {TgDeclick, 0.f}, {TgVolSlot, 0.f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in = ramp(spb * 4);
    Buf out = run(*fx, s, in, 256);
    double err = 0, errLive = 0;
    for (size_t i = spb * 2 + 10; i < spb * 4 - 10; ++i) err = std::max(err, std::fabs(static_cast<double>(out.l[i]) - in.l[i - 2 * spb]));
    for (size_t i = 10; i < spb * 2 - 10; ++i) errLive = std::max(errLive, std::fabs(static_cast<double>(out.l[i]) - in.l[i]));
    expect(errLive < 1e-9 && err < 1e-9, "time gate: hold segment at y = 0.75 repeats the bar's first two beats exactly");
  }
  // Half speed: delay ramps 0 → 2 beats over the bar → output = input at half the position.
  {
    std::string data = "GB2\n" + slotLine('T', 0, [](tg::Slot& x) { x.env.add(0, 1.f), x.env.add(4, 0.75f); });
    PluginState s = st("gl.timegate", {{TgTimeSlot, 1.f}, {TgDeclick, 0.f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in = ramp(spb * 4);
    Buf out = run(*fx, s, in, 256);
    double err = 0;
    for (size_t i = spb / 2; i < spb * 4 - 64; i += 97) err = std::max(err, std::fabs(out.l[i] - in.l[i] * 0.5));
    expect(err < 2e-6, "time gate: sloped segment resamples (half speed)");
  }
  // Trigger sync: a slot change requested mid-beat lands on the next 1-beat line.
  {
    std::string data = "GB2\n" + slotLine('V', 0, [](tg::Slot& x) {
                         x.flat(0.f);
                         x.trig = 3;
                       });
    PluginState s = st("gl.timegate", {{TgVolSlot, 0.f}, {TgVolAttack, 0.0001f}, {TgVolRelease, 0.0001f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in = sine(300, spb * 3);
    Buf out = run(*fx, s, in, 256, [&](size_t p, PluginState& ps) {
      if (p >= spb / 2) ps.params[static_cast<size_t>(TgVolSlot)] = 1.f;  // request at beat 0.5
    });
    expect(rms(out.l, spb / 2 + 2000, spb - 500) > 0.3 && rms(out.l, spb + 500, spb * 2) < 1e-3, "time gate: trigger sync waits for the next beat");
  }
  // One-shot: back to the first slot after 4 beats; link: the volume slot follows the time slot.
  {
    std::string data = "GB2\n" + slotLine('V', 1, [](tg::Slot& x) {
                         x.flat(0.f);
                         x.oneShot = true;
                         x.trig = 0;
                       }) + slotLine('V', 0, [](tg::Slot& x) { x.flat(1.f); });
    PluginState s = st("gl.timegate", {{TgVolSlot, 2.f}, {TgVolAttack, 0.0001f}, {TgVolRelease, 0.0001f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in = sine(300, spb * 6);
    Buf out = run(*fx, s, in, 256);
    expect(rms(out.l, spb, spb * 3) < 1e-3 && rms(out.l, spb * 4 + 1000, spb * 5) > 0.3, "time gate: one-shot slot returns to slot 1 after 4 beats");
    std::string data2 = "GB2\n" + slotLine('T', 2, [](tg::Slot& x) {
                          x.flat(1.f);
                          x.link = true;
                          x.trig = 0;
                        }) + slotLine('V', 2, [](tg::Slot& x) { x.flat(0.f); });
    PluginState s2 = st("gl.timegate", {{TgTimeSlot, 3.f}, {TgVolSlot, 0.f}, {TgVolAttack, 0.0001f}, {TgVolRelease, 0.0001f}});
    s2.data = data2;
    auto fx2 = createEffect("gl.timegate");
    Buf o2 = run(*fx2, s2, sine(300, spb * 2), 256);
    float m[4] = {};
    fx2->meters(m, 4);
    expect(rms(o2.l, spb / 2, spb) < 1e-3 && near(m[2], 3, 1e-9), "time gate: linked time slot selects the same volume slot");
  }
  // Volume tension curve: constant y = 0.5, tension +1 → 1 - B(e^(C/2) - 1), C = ln 1001.
  {
    std::string data = "GB2\n" + slotLine('V', 0, [](tg::Slot& x) { x.flat(0.5f); });
    PluginState s = st("gl.timegate", {{TgVolSlot, 1.f}, {TgTension, 1.f}, {TgVolAttack, 0.0001f}, {TgVolRelease, 0.0001f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in;
    in.l.assign(spb, 1.f), in.r.assign(spb, 1.f);
    Buf out = run(*fx, s, in, 256);
    double C = std::log(1001.0), B = 1.0 / (std::exp(C) - 1.0);
    expect(near(out.l[spb - 10], 1.0 - B * (std::exp(C * 0.5) - 1.0), 1e-4), "time gate: volume tension is exponential (ratio 1001)");
  }
  // Hold: loops the last bar from the moment it engages.
  {
    PluginState s = st("gl.timegate", {{TgTimeSlot, 0.f}, {TgVolSlot, 0.f}});
    auto fx = createEffect("gl.timegate");
    Buf in = ramp(spb * 10);
    Buf out = run(*fx, s, in, 200, [&](size_t p, PluginState& ps) { ps.params[static_cast<size_t>(TgHold)] = p >= spb * 4 ? 1.f : 0.f; });
    double err = 0;
    for (size_t i = spb * 4 + 10; i < spb * 10 - 10; i += 51) {
      size_t back = spb * 4 - 4 * spb + ((i - spb * 4) % (4 * spb));  // T0 - bar + (t - T0) mod bar
      err = std::max(err, std::fabs(static_cast<double>(out.l[i]) - in.l[back]));
    }
    expect(err < 1e-9, "time gate: hold repeats the last bar");
  }
  // Notes select slots (time 60..95, volume 24..59); key-held slots return on release.
  {
    std::string data = "GB2\n" + slotLine('V', 4, [](tg::Slot& x) {
                         x.flat(0.f);
                         x.keyHeld = true;
                         x.trig = 0;
                       });
    PluginState s = st("gl.timegate", {{TgVolSlot, 1.f}});
    s.data = data;
    auto fx = createEffect("gl.timegate");
    Buf in = sine(300, spb);
    run(*fx, s, in, 256);
    timeGateNote(fx.get(), 24 + 4, true);
    run(*fx, s, sine(300, 512), 256);
    float m[4] = {};
    fx->meters(m, 4);
    bool held = near(m[2], 5, 1e-9);
    timeGateNote(fx.get(), 24 + 4, false);
    run(*fx, s, sine(300, 512), 256);
    fx->meters(m, 4);
    expect(held && near(m[2], 1, 1e-9), "time gate: note selects a key-held slot, release returns");
  }
  // Stopped transport: dry output.
  {
    PluginState s = st("gl.timegate", {{TgVolSlot, 1.f}});
    auto fx = createEffect("gl.timegate");
    Buf in = sine(300, 4096);
    Buf out = in;
    fx->process(s, ctx(false, 0), out.l.data(), out.r.data(), 2048);
    fx->process(s, ctx(false, 0), out.l.data() + 2048, out.r.data() + 2048, 2048);
    double err = 0;
    for (size_t i = 2048; i < 4096; ++i) err = std::max(err, std::fabs(static_cast<double>(out.l[i]) - in.l[i]));
    expect(err < 1e-9, "time gate: dry when the transport is stopped");
  }
  // Extremes stay finite; old (short) states normalize.
  {
    PluginState s = st("gl.timegate", {{TgTimeSlot, 36.f}, {TgVolSlot, 36.f}, {TgScratch, 8.f}, {TgHq, 1.f}, {TgCompensate, 1.f}, {TgDcRemove, 1.f}});
    auto fx = createEffect("gl.timegate");
    Buf out = run(*fx, s, sine(100, spb * 3), 333);
    bool fin = true;
    for (float x : out.l) fin = fin && std::isfinite(x);
    PluginState old = defaultPluginState("gl.timegate");
    old.params.resize(static_cast<size_t>(TgFreeRun + 1));
    normalizePluginState(old);
    expect(fin && old.params.size() == static_cast<size_t>(TgCount) && near(old.params[static_cast<size_t>(TgHq)], 1, 1e-9),
           "time gate: extremes finite, old states gain the new options");
  }
}


// ---------------------------------------------------------------- Octa Filter
void testOctaFilter() {
  namespace cr = gl::packs::creative;
  const size_t spb = static_cast<size_t>(kSr * 0.5);
  auto U = [](int u, int f) { return cr::unitParam(u, f); };
  auto runOf = [&](PluginState s, const Buf& in, int block = 256) {
    auto fx = createEffect("gl.octafilter");
    return run(*fx, std::move(s), in, block);
  };
  // Volume curve: 0.1 (11^x - 1), unity at 1.
  expect(near(cr::volCurve(1.0), 1.0, 1e-12) && near(cr::volCurve(0.5), 0.1 * (std::sqrt(11.0) - 1), 1e-12) && cr::volCurve(0) == 0,
         "octa filter: volume curve");
  // Unit off (filter mode off) passes the input at unity.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UOrder), 0.f}});
    Buf in = sine(440, 4096);
    Buf out = runOf(s, in);
    double err = 0;
    for (size_t i = 0; i < 4096; ++i) err = std::max(err, std::fabs(static_cast<double>(out.l[i]) - in.l[i]));
    expect(err < 1e-6, "octa filter: unit without filter is transparent");
  }
  // Low pass (type 3) attenuates highs, keeps lows.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UType), 3.f}, {U(0, cr::UCut), 0.4f}, {U(0, cr::URes), 0.45f}});
    Buf lo = runOf(s, sine(100, spb)), hi = runOf(s, sine(8000, spb));
    expect(rms(lo.l, spb / 2, spb) > 0.3 && rms(hi.l, spb / 2, spb) < 0.02, "octa filter: low pass keeps 100 Hz, cuts 8 kHz");
  }
  // Series routing: unit 1 (low pass) feeds unit 2 (high pass) through NEXT; unit 2 hears no input; unit 1 muted.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UType), 3.f}, {U(0, cr::UCut), 0.4f}, {U(0, cr::URes), 0.45f}, {U(0, cr::UNext), 1.f},
                                         {U(0, cr::UOut), 0.f}, {U(1, cr::UEnabled), 1.f}, {U(1, cr::UType), 6.f}, {U(1, cr::UCut), 0.55f},
                                         {U(1, cr::URes), 0.45f}, {U(1, cr::UIn), 0.f}});
    Buf lo = runOf(s, sine(60, spb)), mid = runOf(s, sine(500, spb)), hi = runOf(s, sine(8000, spb));
    expect(rms(hi.l, spb / 2, spb) < 0.02 && rms(lo.l, spb / 2, spb) < 0.05 && rms(mid.l, spb / 2, spb) > 0.05,
           "octa filter: to-next routing chains unit 1 into unit 2 (band from LP into HP)");
    PluginState par = s;
    par.params[static_cast<size_t>(U(0, cr::UNext))] = 0.f;
    Buf none = runOf(par, sine(500, spb));
    expect(rms(none.l, spb / 2, spb) < 1e-6, "octa filter: without NEXT the input-less unit is silent");
  }
  // Parallel: two enabled units sum; solo keeps one.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UOrder), 0.f}, {U(1, cr::UEnabled), 1.f}, {U(1, cr::UOrder), 0.f}});
    Buf in = sine(300, 4096);
    Buf out = runOf(s, in);
    expect(near(out.l[2000], 2.0 * in.l[2000], 1e-5), "octa filter: enabled units sum to the master");
    s.params[static_cast<size_t>(cr::OfSolo)] = 2.f;
    Buf so = runOf(s, in);
    expect(near(so.l[2000], in.l[2000], 1e-5), "octa filter: solo keeps one unit");
  }
  // Out pan with cross-feed: hard right moves half of the left into the right.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UOrder), 0.f}, {U(0, cr::UPan), 1.f}, {cr::OfModel, 0.f}});
    Buf in;
    in.l.assign(1024, 0.5f), in.r.assign(1024, 0.f);
    Buf out = runOf(s, in);
    expect(near(out.l[500], 0, 1e-6) && near(out.r[500], 0.25, 1e-6), "octa filter: pan law cross-feeds half of the cut side");
    s.params[static_cast<size_t>(cr::OfModel)] = 1.f;  // J-42 reference: constant power x sqrt2, the far side folded in
    Buf ref = runOf(s, in);
    expect(std::fabs(ref.l[500]) < 1e-6 && near(ref.r[500], 0.5 / std::sqrt(2.0), 1e-5), "octa filter (reference): hard right folds the left in at -3 dB");
  }
  // Pattern envelope on the volume: a hold step from 1 to 0 at beat 2.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UOrder), 0.f}});
    cr::OEnv e;
    e.add(0, 1.f), e.add(2, 1.f, 0, gl::menv::Hold), e.add(2, 0.f, 0, gl::menv::Hold), e.add(4, 0.f, 0, gl::menv::Hold);
    s.data = "OF1\n" + cr::envLine(0, cr::TVol, 0, e, 0, 3) + "\n";
    Buf in = sine(300, spb * 4);
    Buf out = runOf(s, in);
    expect(rms(out.l, spb / 2, spb) > 0.3 && rms(out.l, spb * 2 + 200, spb * 3) < 1e-4 && rms(out.l, spb * 4 - 1, spb * 4) >= 0,
           "octa filter: pattern envelope gates the volume on the beat grid");
  }
  // X map on the cutoff: X = 0 closes a low pass, X = 1 opens it.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UType), 3.f}, {U(0, cr::UCut), 0.5f}, {U(0, cr::URes), 0.45f}, {cr::OfModX, 0.f}});
    cr::OEnv e;
    e.add(0, 0.f), e.add(1, 1.f);
    s.data = "OF1\n" + cr::envLine(0, cr::TCut, cr::KMapX, e) + "\n";
    Buf closed = runOf(s, sine(3000, spb));
    s.params[static_cast<size_t>(cr::OfModX)] = 1.f;
    Buf open = runOf(s, sine(3000, spb));
    expect(rms(open.l, spb / 2, spb) > 10 * rms(closed.l, spb / 2, spb), "octa filter: X map moves the cutoff");
  }
  // Shaper: bipolar identity curve passes, default bend saturates.
  {
    PluginState s = st("gl.octafilter", {{U(0, cr::UOrder), 0.f}, {U(0, cr::UWsOn), 1.f}});
    Buf in = sine(200, 4096, 0.9);
    Buf out = runOf(s, in);
    double peak = 0;
    for (float x : out.l) peak = std::max(peak, std::fabs(static_cast<double>(x)));
    expect(peak < 0.9 && peak > 0.5, "octa filter: shaper bends the waveform (default curve)");
  }
  // All filter types and modes stay finite at extremes.
  {
    bool fin = true;
    for (int type = 0; type < cr::FilterTypeCount; ++type)
      for (int order : {1, 3, 5})
        for (float res : {0.f, 1.f}) {
          PluginState s = st("gl.octafilter", {{U(0, cr::UType), static_cast<float>(type)}, {U(0, cr::UOrder), static_cast<float>(order)},
                                               {U(0, cr::URes), res}, {U(0, cr::UCut), 1.f}, {U(0, cr::ULow), 1.f}, {U(0, cr::UAlt), 1.f}});
          Buf out = runOf(s, sine(1000, 4096, 0.8));
          for (float x : out.l) fin = fin && std::isfinite(x) && std::fabs(x) < 100;
        }
    expect(fin, "octa filter: every type / mode / resonance extreme stays finite");
  }
}


// ---------------------------------------------------------------- Chorus (reference engine)
void testChorus() {
  // Static taps (rates 0, depth 0, no split): wet = 0.6 x 3 voices x 0.7 x input, delayed by the delay.
  {
    PluginState s = st("gl.chorus", {{ChoRate, 0.f}, {ChoRate2, 0.f}, {ChoRate3, 0.f}, {ChoDepthMs, 0.f}, {ChoDelay, 10.f},
                                     {ChoCrossMode, 0.f}, {ChoWetOnly, 1.f}});
    auto fx = createEffect("gl.chorus");
    Buf in = sine(300, 8192);
    Buf out = in;
    fx->process(s, ctx(false, 0), out.l.data(), out.r.data(), 8192);
    size_t d = static_cast<size_t>(0.010 * kSr);
    double err = 0;
    for (size_t i = 2000; i < 8000; ++i) err = std::max(err, std::fabs(out.l[i] - 1.26 * in.l[i - d]));
    expect(err < 0.01, "chorus: static taps = 0.6 x 3 x 0.7 x input, delayed");
  }
  // Dry path is 0.7 x the full band; "process highs" keeps a low tone out of the wet path.
  {
    PluginState s = st("gl.chorus", {{ChoCrossMode, 1.f}, {ChoCrossFreq, 2000.f}});
    auto fx = createEffect("gl.chorus");
    Buf in = sine(60, 16384);
    Buf out = in;
    fx->process(s, ctx(false, 0), out.l.data(), out.r.data(), 16384);
    expect(std::fabs(rms(out.l, 8000, 16000) - 0.7 * rms(in.l, 8000, 16000)) < 0.03, "chorus: low tone stays dry when the highs are processed");
  }
  // Wave tables: the four shapes differ and stay in 0..1 weights (sweep within delay .. delay + depth).
  {
    bool fin = true;
    for (int w = 0; w < 4; ++w) {
      PluginState s = st("gl.chorus", {{ChoWave1, static_cast<float>(w)}, {ChoWave2, static_cast<float>(w)}, {ChoWave3, static_cast<float>(w)},
                                       {ChoRate, 5.f}, {ChoDepthMs, 5.f}, {ChoDelay, 0.05f}});
      auto fx = createEffect("gl.chorus");
      Buf o = sine(1000, 8192, 0.9);
      fx->process(s, ctx(false, 0), o.l.data(), o.r.data(), 8192);
      for (float x : o.l) fin = fin && std::isfinite(x) && std::fabs(x) < 3;
    }
    PluginState old = defaultPluginState("gl.chorus");
    old.params.resize(static_cast<size_t>(ChoFeedback + 1));
    normalizePluginState(old);
    expect(fin && old.params[static_cast<size_t>(ChoClassic)] == 0.f && defaultPluginState("gl.chorus").params[static_cast<size_t>(ChoClassic)] == 1.f,
           "chorus: waves finite; old projects keep the morph engine, new ones use the reference engine");
  }
}


// ---------------------------------------------------------------- Waveshaper / Fast Dist
void testShapers() {
  // Free-point curve with tension: unipolar curve (0,0) → (1,1) single curve t = 0.5 (fast start).
  {
    wscurve::Curve c;
    c.add(0, 0.f), c.add(1, 1.f, 0.5f, gl::menv::SingleCurve);
    c.prepare();
    PluginState s = st("gl.waveshaper", {{WsMix, 1.f}, {WsCenter, 0.f}});
    s.data = wscurve::serialize(c);
    double C = gl::menv::kLn1e6 * 0.5, B = 1.0 / (std::exp(C) - 1.0), x = 0.3;
    double want = 1.0 - B * (std::exp(C * (1 - x)) - 1.0);
    expect(near(waveshaperCurve(s, x), want, 1e-6) && near(waveshaperCurve(s, -x), -want, 1e-6), "waveshaper: curve tension (single curve, mirrored)");
    auto fx = createEffect("gl.waveshaper");
    Buf in;
    in.l.assign(256, 0.3f), in.r.assign(256, -0.3f);
    Buf out = in;
    fx->process(s, ctx(false, 0), out.l.data(), out.r.data(), 256);
    expect(near(out.l[200], want, 1e-3) && near(out.r[200], -want, 1e-3), "waveshaper: table lookup matches the curve");
    // Reference gains: pre knob 1.5 → (4^1.5 - 1) / 3 = 7/3; mix 0 → dry = post x pre x post.
    PluginState g = s;
    g.data = "WS1|0,0,0,0 1,1,0,0";
    g.params[static_cast<size_t>(WsPreAmp)] = 1.5f;
    g.params[static_cast<size_t>(WsMix)] = 0.f;
    auto fx2 = createEffect("gl.waveshaper");
    Buf o2 = in;
    fx2->process(g, ctx(false, 0), o2.l.data(), o2.r.data(), 256);
    expect(near(o2.l[100], 0.3 * 7.0 / 3.0, 1e-4), "waveshaper: reference dry path (post x pre x post)");
    // Bipolar curve maps -1..1 onto 0..1: a flat curve at 0.5 outputs silence.
    PluginState bp = s;
    bp.params[static_cast<size_t>(WsBipolar)] = 0.f;
    bp.data = "WS1|0,0.5,0,0 1,0.5,0,0";
    expect(near(waveshaperCurve(bp, 0.7), 0.0, 1e-9), "waveshaper: bipolar mapping");
    // Polarity switch keeps the transfer function.
    wscurve::Curve u = c;
    wscurve::toBipolar(u);
    double y = 2 * u.eval((0.3 + 1) * 0.5) - 1, yn = 2 * u.eval((-0.3 + 1) * 0.5) - 1;
    expect(near(y, want, 1e-6) && near(yn, -want, 1e-6), "waveshaper: unipolar to bipolar conversion");
  }
  // Fast Dist reference tables: drive 1 is clean, type A drive 10 compresses above -25 dB.
  {
    namespace dy = gl::packs::dynamics;
    expect(near(dy::fastDistRef(0.5, 0, 1), 0.5, 1e-9) && near(dy::fastDistRef(1.0, 0, 10), std::pow(10.0, (-25 + 2.5 + 14.4) / 20.0), 1e-6),
           "fast dist: type A table (clean at 1, 0.1 slope + make-up at 10)");
    expect(near(dy::fastDistRef(1.0, 1, 10), 1.0, 1e-9) && near(dy::fastDistRef(0.1, 1, 10), std::pow(10.0, -std::pow(20.0, 0.8) / 20.0), 1e-9),
           "fast dist: type B dB power law");
    PluginState s = st("gl.fastdist");
    auto fx = createEffect("gl.fastdist");
    Buf in = sine(200, 4096, 0.9);
    Buf out = in;
    fx->process(s, ctx(false, 0), out.l.data(), out.r.data(), 4096);
    double pk = 0;
    for (float x : out.l) pk = std::max(pk, std::fabs(static_cast<double>(x)));
    expect(pk < 0.5 && pk > 0.3, "fast dist: default (drive 10, type A) squashes the peak");
  }
}


// ---------------------------------------------------------------- stereo (Balance / PanOMatic laws)
void testStereo() {
  auto runConst = [&](PluginState s, double l, double r, double& ol, double& orr) {
    auto fx = createEffect(s.type);
    Buf b;
    b.l.assign(4096, static_cast<float>(l)), b.r.assign(4096, static_cast<float>(r));
    fx->process(s, ctx(false, 0), b.l.data(), b.r.data(), 4096);
    ol = b.l[4000], orr = b.r[4000];
  };
  double ol, orr;
  // Balance (folded law): centre is transparent; hard left folds the right side in: L = (L + R) / sqrt 2 x 1 ... (comp 0.5).
  runConst(st("gl.utility"), 0.5, 0.25, ol, orr);
  expect(near(ol, 0.5, 1e-6) && near(orr, 0.25, 1e-6), "balance: centre passes through");
  runConst(st("gl.utility", {{UtlPan, -1.f}}), 0.5, 0.25, ol, orr);
  expect(near(ol, std::sqrt(2.0) * 0.5 * 0.75, 1e-5) && near(orr, 0, 1e-6), "balance: hard left folds the right channel in");
  // PanOMatic: pan +0.5 → LL = 0.5, LR = 0.25, RR = 0.75 (unity volume).
  runConst(st("gl.autopan", {{ApStaticPan, 0.5f}}), 0.4, 0.2, ol, orr);
  expect(near(ol, 0.2, 1e-4) && near(orr, 0.75 * 0.2 + 0.25 * 0.4, 1e-4), "auto pan: reference pan law (far side folded in)");
  runConst(st("gl.autopan", {{ApStaticVol, 0.5f}}), 0.4, 0.4, ol, orr);
  expect(near(ol, 0.4 * 0.1 * (std::sqrt(11.0) - 1), 1e-4), "auto pan: volume taper 0.1 (11^x - 1)");
  // Volume LFO (square) moves the gain by +/- amount / 2 around the static volume.
  {
    PluginState s = st("gl.autopan", {{ApTarget, 2.f}, {ApShape, 2.f}, {ApAmount, 1.f}, {ApSpeedK, 0.05f}});
    auto fx = createEffect("gl.autopan");
    Buf b;
    b.l.assign(44096, 0.5f), b.r.assign(44096, 0.5f);
    for (size_t p = 0; p + 64 <= b.l.size(); p += 64) fx->process(s, ctx(false, 0), b.l.data() + p, b.r.data() + p, 64);
    double lo = 1, hi = 0;
    for (size_t i = 2000; i < b.l.size(); ++i) lo = std::min(lo, static_cast<double>(b.l[i])), hi = std::max(hi, static_cast<double>(b.l[i]));
    expect(near(hi, 0.75, 0.01) && near(lo, 0.25, 0.01), "auto pan: square volume LFO (amount / 2 on the linear gain)");
  }
  // Stereo Matrix (reference law): slider x → sign(x) (3^|x| - 1) / 2; "mid" position ln2/ln3 = half gain.
  {
    namespace ut = gl::packs::utility;
    const float mid = static_cast<float>(std::log(2.0) / std::log(3.0));
    PluginState s = st("gl.stereomatrix", {{ut::SmLL, mid}, {ut::SmRL, mid}, {ut::SmLR, mid}, {ut::SmRR, mid}});
    runConst(s, 0.6, 0.2, ol, orr);
    expect(near(ol, 0.4, 1e-4) && near(orr, 0.4, 1e-4), "stereo matrix: reference gain law (mid = half gain)");
    PluginState d = st("gl.stereomatrix", {{ut::SmDelayK, static_cast<float>(std::log(21.0) / std::log(1001.0))}});  // 10 ms on the right
    auto fx = createEffect("gl.stereomatrix");
    Buf b = sine(200, 4096);
    Buf in = b;
    fx->process(d, ctx(false, 0), b.l.data(), b.r.data(), 4096);
    expect(near(b.r[3000], in.r[3000 - 441], 1e-5) && near(b.l[3000], in.l[3000], 1e-6), "stereo matrix: delay curve (1001^|k| - 1) / 2 ms");
  }
  // Stereo Shaper (reference separation): s = 0.75 sep; LL = RR = (1 - s/2), cross = LL s.
  {
    PluginState s = st("gl.stereo", {{StSep, 1.f}});
    runConst(s, 1.0, 0.0, ol, orr);
    expect(near(ol, 0.625, 1e-5) && near(orr, 0.625 * 0.75, 1e-5), "stereo shaper: separation matrix (merged)");
    PluginState p = st("gl.stereo", {{StPan, -0.5f}, {StInvert, 1.f}});
    runConst(p, 0.4, 0.4, ol, orr);
    expect(near(ol, -0.4, 1e-5) && near(orr, 0.2, 1e-5), "stereo shaper: pan scales one side, invert flips the left output");
  }
  // Old projects keep the morph engine.
  PluginState old = defaultPluginState("gl.autopan");
  old.params.resize(static_cast<size_t>(ApStaticVol + 1));
  normalizePluginState(old);
  expect(old.params[static_cast<size_t>(ApClassic)] == 0.f, "auto pan: old projects keep the morph engine");
}

}  // namespace

int main() {
  testMEnv();
  testTimeGate();
  testOctaFilter();
  testChorus();
  testShapers();
  testStereo();
  return finish();
}
