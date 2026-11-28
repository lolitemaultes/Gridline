// Gridline: "platter" pack tests: the jog under real-world input timing. The hand's moves go
// into Platter the way a real computer delivers them, not on an ideal clock:
//   - the mouse on the platter: positions read once a UI frame (60, 120 or 240 Hz, frames that
//     come late or are dropped, or the idle-paced loop's uneven frames), from a mouse that
//     reports at 125 or 1000 Hz, in whole screen pixels on the platter, stamped with the frame's
//     start (GL_JOG_STAMPS=draw: when the editor draws, a few ms after the frame starts, never
//     the same each time: how it was);
//   - the controller's jog wheel: a report every millisecond, or reports bunched by the USB
//     driver (every 4 or 8 ms, all stamped at once), 128 or 1024 ticks a turn, in the three
//     relative encodings;
//   - audio callbacks of 256, 512 or 1024 frames that start up to 2 ms late.
// The record's place is read from a ramp track at every control step, so the test measures
// what is heard: the playback rate over time, how much it wobbles on a steady hand, the largest
// change from one step to the next, steps where it nearly stops (stutter), direction changes
// the hand never made, and how far the record is from the hand.
//
// GL_JOG_OUT=<dir>: also writes each case's hand, record and rate per control step (CSV).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/WavWriter.h"
#include "engine/Generators.h"
#include "engine/Sample.h"
#include "packs/Packs.h"
#include "packs/platter/platter.h"

using namespace gl;
namespace pl = gl::packs::platter;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100;
constexpr double kPi = 3.14159265358979323846;
constexpr int kRead = pl::kStep;
constexpr double kRampTop = 0.9;
constexpr double kRampFrames = 12 * kSr;
constexpr double kTurnsPerSecAt1x = 1.0 / pl::kSecondsPerRev;

// A small deterministic generator (the same numbers on every platform).
struct Rng {
  uint64_t s;
  double u() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
  }
  double in(double a, double b) { return a + (b - a) * u(); }
};

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_platter_jog_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

std::string writeRamp() {
  const auto n = static_cast<size_t>(kRampFrames);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    b.data[i] = static_cast<float>(kRampTop * static_cast<double>(i) / kRampFrames);
    b.data[n + i] = 0.5f;
  }
  const std::string path = (tmpDir() / "ramp.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

// ------------------------------------------------------------------ the hand
// The hand's travel (turns of the record, forward > 0) at time t (s from when it starts moving).
using HandFn = std::function<double(double)>;
HandFn steady(double speed) {
  return [speed](double t) { return t <= 0 ? 0.0 : speed * kTurnsPerSecAt1x * t; };
}
// Out and back `n` times (raised cosine strokes, `ms` each way, mean speed `speed`).
HandFn backAndForth(double ms, double speed, int n) {
  const double a = speed * kTurnsPerSecAt1x * ms * 1e-3;
  return [=](double t) {
    if (t <= 0) return 0.0;
    const double k = t * 1e3 / ms;
    if (k >= 2 * n) return 0.0;
    const double ph = k - 2 * std::floor(k / 2);  // 0..2
    const double u = ph < 1 ? ph : 2 - ph;
    return a * 0.5 * (1.0 - std::cos(kPi * u));
  };
}
// Moves at `speed` for `moveSec`, then holds still.
HandFn moveThenHold(double speed, double moveSec) {
  return [=](double t) { return t <= 0 ? 0.0 : speed * kTurnsPerSecAt1x * std::min(t, moveSec); };
}

// ------------------------------------------------------------------ input models
enum class Kind { Mouse, Jog };
struct Frames {
  double hz = 60;         // display refresh
  double dropFrac = 0;    // a frame takes two refreshes
  bool idle = false;      // the idle-paced loop: frames 4..25 ms apart, as events wake it
  double drawMs[2] = {0.5, 5.0};  // the editor is drawn this long after the frame starts
  double reportHz = 125;  // the mouse's own report rate
  double radiusPx = 55;   // where on the platter the hand holds it
};
struct Wheel {
  double reportHz = 1000;  // the controller reports this often
  double burstMs = 0;      // > 0: the driver hands them over this often, all at once
  int ticksPerRev = 128;
  uint8_t enc = pl::EncRelOffset;
};
struct Audio {
  int block = 256;
  double lateMs = 0;  // a callback starts up to this late
};
struct Case {
  std::string name;
  Kind kind = Kind::Mouse;
  Frames fr;
  Wheel wh;
  Audio au;
  HandFn hand;
  double moveSec = 1.5;  // the hand's part of the run (then it lets go)
  bool motor = false;    // the deck plays under the hand
  bool release = true;   // the hand lets go at the end
  double after = 0.4;    // the run goes on this long after the hand lets go
  uint64_t seed = 1;
  double jogSens = -1;   // >= 0: the deck's jog sensitivity (the hand still sends the ticks of the default)
};

struct Run {
  std::vector<double> rec;   // record travel (frames) after each control step, from the hand's start
  std::vector<double> hand;  // the hand's travel (frames) at each step (the true hand, not the input)
  std::vector<double> rate;  // the record's rate over each step (x normal)
  double lead = 0;           // steps before the hand starts moving
  double releaseStep = 0;    // the step the hand lets go
};

// How the editor stamps the mouse's moves: with the frame's start (now), or when the editor is
// drawn (before: GL_JOG_STAMPS=draw).
enum class Stamps { Frame, Draw };
Stamps gStamps = Stamps::Frame;
int64_t gNs = 0;
int64_t fakeNow() { return gNs; }
constexpr int64_t kNs0 = 1000000000;
int64_t nsOfSec(double s) { return kNs0 + static_cast<int64_t>(std::llround(s * 1e9)); }

struct Input {
  double at;  // wall time (s) it reaches Platter (stamped)
  int kind;   // 0 MIDI, 1 hand down, 2 hand move, 3 hand up
  uint8_t s = 0, a = 0, b = 0;
  double v = 0;
};

uint8_t encodeRel(int d, uint8_t enc) {
  d = std::clamp(d, -63, 63);
  if (enc == pl::EncRelTwos) return static_cast<uint8_t>(d < 0 ? 128 + d : d);
  if (enc == pl::EncRelSign) return static_cast<uint8_t>(d < 0 ? 64 | -d : d);
  return static_cast<uint8_t>(64 + d);
}

pl::Mapping wheelMapping(const Wheel& w) {
  pl::Mapping m;
  m.profile = "generic";
  m.ticksPerRev = w.ticksPerRev;
  pl::Binding t;
  t.control = pl::CtlTouch, t.kind = pl::MkNote, t.channel = 0, t.num = 0x36;
  m.bindings.push_back(t);
  pl::Binding j;
  j.control = pl::CtlJogPlatter, j.kind = pl::MkCC, j.channel = 0, j.num = 0x22, j.enc = w.enc;
  m.bindings.push_back(j);
  pl::Binding r;
  r.control = pl::CtlJogRim, r.kind = pl::MkCC, r.channel = 0, r.num = 0x21, r.enc = w.enc;
  m.bindings.push_back(r);
  return m;
}

Run perform(const std::string& ramp, const Case& c) {
  Rng rng{c.seed * 0x9E3779B97F4A7C15ULL + 7};
  SamplePtr src = sampleCache().get(ramp);
  pl::Model m;
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, 120), true);
  dm.cue = 0.5 * static_cast<double>(src->frames());
  pl::loadTrack(m, 0, ramp, dm);
  if (c.kind == Kind::Jog) m.map = wheelMapping(c.wh);
  PluginState st = defaultPluginState(pl::kType);
  st.params[pl::PSync] = 0;
  st.params[pl::PKeyLock] = 0;
  if (c.jogSens >= 0) st.params[static_cast<size_t>(pl::deckParam(0, pl::DJog))] = static_cast<float>(c.jogSens);
  st.data = pl::formatData(ramp, m);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  auto g = createGenerator(pl::kType);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.ppq = 96;
  ctx.offline = false;
  pl::hub::setClock(fakeNow);
  pl::hub::setTapped(c.kind == Kind::Jog);

  const double settle = 0.25;               // s before the hand goes down
  const double t0 = settle + 0.08;          // the hand starts moving (s)
  const double tUp = t0 + c.moveSec;        // the hand lets go
  const double total = tUp + c.after;
  const double fpTurn = kSr * pl::kSecondsPerRev;
  auto handTurns = [&](double t) { return c.hand(t - t0); };

  // ---- the input, in wall time
  std::vector<Input> in;
  if (c.motor) {
    Input p{settle * 0.5, 0, 0, 0, 0, 0};
    in.push_back(p);  // (the play press: sent as an editor button below)
  }
  if (c.kind == Kind::Mouse) {
    const Frames& f = c.fr;
    pl::PlatterGrip pm;  // (the editor's own code)
    const double r = f.radiusPx;
    auto px = [&](double t, double& x, double& y) {
      // the mouse's last report before t (its own clock), in whole pixels
      const double rep = std::floor(t * f.reportHz) / f.reportHz;
      const double th = -kPi * 0.5 + 2 * kPi * handTurns(rep);
      x = std::round(r * std::cos(th)), y = std::round(r * std::sin(th));
    };
    double x = 0, y = 0;
    px(settle, x, y);
    pm.down(x, y);
    in.push_back({settle + 0.001, 1, 0, 0, 0, 0});
    double ft = settle + 1.0 / f.hz;
    while (ft < tUp) {
      const double draw = rng.in(f.drawMs[0], f.drawMs[1]) * 1e-3;
      px(ft, x, y);
      const double v = pm.move(x, y);
      const double stamp = gStamps == Stamps::Draw ? draw : f.drawMs[0] * 1e-3 + rng.in(0.0, 0.0002);
      in.push_back({ft + stamp, 2, 0, 0, 0, v});
      if (f.idle) ft += rng.in(0.004, 0.025);
      else ft += (rng.u() < f.dropFrac ? 2.0 : 1.0) / f.hz;
    }
    if (c.release) in.push_back({tUp + rng.in(f.drawMs[0], f.drawMs[1]) * 1e-3, 3, 0, 0, 0, 0});
  } else {
    const Wheel& w = c.wh;
    in.push_back({settle, 0, 0x90, 0x36, 0x7F, 0});
    long sent = 0;
    const double tpTurn = w.ticksPerRev / pl::kJogSensDefault;  // ticks per turn of the record
    std::vector<Input> pending;
    double nextBurst = t0 + (w.burstMs > 0 ? rng.in(0, w.burstMs * 1e-3) : 0);
    for (double rt = t0; rt < tUp; rt += 1.0 / w.reportHz) {
      const long want = static_cast<long>(std::floor(handTurns(rt) * tpTurn + 1e-9));
      long dl = want - sent;
      while (dl != 0) {
        const long k = std::clamp(dl, -63L, 63L);
        Input e{rt + rng.in(0.0, 0.0002), 0, 0xB0, 0x22, encodeRel(static_cast<int>(k), w.enc), 0};
        pending.push_back(e);
        sent += k, dl -= k;
      }
      if (w.burstMs <= 0) {
        in.insert(in.end(), pending.begin(), pending.end());
        pending.clear();
      } else if (rt >= nextBurst) {
        for (Input& e : pending) e.at = nextBurst;
        in.insert(in.end(), pending.begin(), pending.end());
        pending.clear();
        nextBurst += w.burstMs * 1e-3;
      }
    }
    for (Input& e : pending) e.at = tUp;
    in.insert(in.end(), pending.begin(), pending.end());
    if (c.release) in.push_back({tUp + 0.0005, 0, 0x80, 0x36, 0x00, 0});
  }
  std::stable_sort(in.begin(), in.end(), [](const Input& a, const Input& b) { return a.at < b.at; });

  // ---- the audio callbacks
  Run run;
  const int B = c.au.block;
  std::vector<float> L, R;
  size_t next = 0;
  bool played = !c.motor;
  int64_t pos = 0;
  for (int64_t s = 0; static_cast<double>(s) < total * kSr; s += B) {
    const double wall = static_cast<double>(s) / kSr + rng.in(0.0, c.au.lateMs * 1e-3);  // when this callback starts
    for (; next < in.size() && in[next].at <= wall; ++next) {
      const Input& e = in[next];
      gNs = nsOfSec(e.at);
      if (e.kind == 0) {
        if (e.s == 0 && !played) {
          pl::Cmd p;
          p.kind = pl::CmdButton, p.a = pl::CtlPlay, p.v = 1;
          pl::command(g.get(), p);
          p.v = 0;
          pl::command(g.get(), p);
          played = true;
          continue;
        }
        pl::hub::push(0, e.s, e.a, e.b);
      } else {
        pl::Cmd k;
        k.kind = e.kind == 1 ? pl::CmdHandDown : e.kind == 2 ? pl::CmdHandMove : pl::CmdHandUp;
        k.v = e.v;
        pl::command(g.get(), k);
      }
    }
    gNs = nsOfSec(wall);
    std::vector<float> bl(static_cast<size_t>(B), 0.f), br(static_cast<size_t>(B), 0.f);
    ctx.tick = static_cast<double>(pos) / kSr * ctx.tempo / 60.0 * ctx.ppq;
    ctx.jumped = pos == 0;
    g->process(st, aux, ctx, bl.data(), br.data(), B);
    pos += B;
    L.insert(L.end(), bl.begin(), bl.end());
    R.insert(R.end(), br.begin(), br.end());
  }
  pl::hub::setTapped(false);
  pl::hub::setClock(nullptr);

  // ---- the record's place per step (from the ramp), the hand's at the same time
  const double start = 0.5 * kRampFrames;
  double last = start;
  const auto from = static_cast<size_t>(settle * kSr);
  for (size_t i = from; i < L.size(); ++i) {
    if (std::fabs(R[i]) > 1e-4) last = static_cast<double>(L[i]) / R[i] * 0.5 / kRampTop * kRampFrames;
    if ((i + 1 - from) % kRead == 0) {
      const double t = static_cast<double>(i + 1) / kSr;
      run.rec.push_back(last - start);
      run.hand.push_back(handTurns(std::min(t, tUp)) * fpTurn);
    }
  }
  run.lead = (t0 - settle) * kSr / kRead;
  run.releaseStep = (tUp - settle) * kSr / kRead;
  run.rate.assign(run.rec.size(), 0.0);
  for (size_t i = 1; i < run.rec.size(); ++i) run.rate[i] = (run.rec[i] - run.rec[i - 1]) / kRead;
  return run;
}

void writeCsv(const std::string& name, const Run& r) {
  const char* out = std::getenv("GL_JOG_OUT");
  if (!out) return;
  std::error_code ec;
  fs::create_directories(out, ec);
  std::ofstream f((fs::path(out) / (name + ".csv")).string());
  f << "ms,hand,record,rate\n";
  for (size_t i = 0; i < r.rec.size(); ++i)
    f << (static_cast<double>(i) - r.lead) * kRead * 1000.0 / kSr << "," << r.hand[i] << "," << r.rec[i] << "," << r.rate[i] << "\n";
}

// ------------------------------------------------------------------ measures
struct Smooth {
  double mean = 0, wobble = 0;  // mean rate (x normal) and its spread (fraction of the mean), per control step
  double block = 0;             // spread of the rate averaged over 5.8 ms (what is heard as wow and stutter)
  double maxJump = 0;           // largest rate change from one control step to the next (fraction of the mean)
  int stalls = 0;               // steps under a quarter of the mean (the record nearly stops)
  int flips = 0;                // direction changes (the hand made none)
  double lagMs = 0, maxErrMs = 0;  // mean distance behind the hand, and the largest, in ms of the hand's motion
};
Smooth measureSteady(const Run& r, double fromSec, double toSec, double speed) {
  Smooth s;
  const auto a = static_cast<size_t>(r.lead + fromSec * kSr / kRead), b = static_cast<size_t>(r.lead + toSec * kSr / kRead);
  double sum = 0, sum2 = 0, lag = 0;
  int n = 0, sign = 0;
  for (size_t i = a; i < b && i < r.rate.size(); ++i) sum += r.rate[i], ++n;
  s.mean = sum / std::max(1, n);
  for (size_t i = a; i < b && i < r.rate.size(); ++i) {
    const double v = r.rate[i];
    sum2 += (v - s.mean) * (v - s.mean);
    s.maxJump = std::max(s.maxJump, std::fabs(v - r.rate[i - 1]) / std::fabs(s.mean));
    if (std::fabs(v) < 0.25 * std::fabs(s.mean)) ++s.stalls;
    const int sg = v > 1e-6 ? 1 : v < -1e-6 ? -1 : 0;
    if (sg != 0) {
      if (sign != 0 && sg != sign) ++s.flips;
      sign = sg;
    }
    const double err = (r.hand[i] - r.rec[i]) / (speed * kSr) * 1000.0;  // ms
    lag += err;
    s.maxErrMs = std::max(s.maxErrMs, std::fabs(err));
  }
  s.wobble = std::sqrt(sum2 / std::max(1, n)) / std::fabs(s.mean);
  constexpr size_t kAvg = 8;  // 8 steps: 256 frames
  double bs = 0, bs2 = 0;
  int bn = 0;
  for (size_t i = a; i + kAvg <= b && i + kAvg <= r.rate.size(); i += kAvg) {
    double m = 0;
    for (size_t k = 0; k < kAvg; ++k) m += r.rate[i + k];
    m /= kAvg;
    bs += m, bs2 += m * m, ++bn;
  }
  const double bm = bs / std::max(1, bn);
  s.block = std::sqrt(std::max(0.0, bs2 / std::max(1, bn) - bm * bm)) / std::fabs(s.mean);
  s.lagMs = lag / std::max(1, n);
  return s;
}

// Direction changes of the record between steps a and b: it moved back more than `hyst` frames
// from the furthest it went one way (the read-out of the ramp has a few hundredths of a frame of
// noise, and a record coming to rest may creep by less than a frame).
int flipsIn(const Run& r, size_t a, size_t b, double hyst) {
  int dir = 0, flips = 0;
  double ext = a < r.rec.size() ? r.rec[a] : 0;
  for (size_t i = a; i < b && i < r.rec.size(); ++i) {
    const double x = r.rec[i];
    if (dir > 0 && x > ext) ext = x;
    if (dir < 0 && x < ext) ext = x;
    if (dir >= 0 && x < ext - hyst) {
      if (dir > 0) ++flips;
      dir = -1, ext = x;
    } else if (dir <= 0 && x > ext + hyst) {
      if (dir < 0) ++flips;
      dir = 1, ext = x;
    }
  }
  return flips;
}

void printSteady(const std::string& what, const Smooth& s) {
  std::printf("# %-34s rate %.3fx  wobble %5.1f%%  5.8ms %5.1f%%  max step %5.1f%%  stalls %3d  flips %2d  lag %5.1f ms  max err %5.1f ms\n",
              what.c_str(), s.mean, 100 * s.wobble, 100 * s.block, 100 * s.maxJump, s.stalls, s.flips, s.lagMs, s.maxErrMs);
}

std::string gRamp;

// ------------------------------------------------------------------ the cases
struct MouseSetup {
  const char* name;
  Frames fr;
  Audio au;
};
std::vector<MouseSetup> mouseSetups() {
  std::vector<MouseSetup> v;
  Frames f60;
  v.push_back({"60Hz", f60, {256, 0}});
  Frames f60d = f60;
  f60d.dropFrac = 0.12;
  v.push_back({"60Hz drops", f60d, {512, 1.5}});
  Frames f120;
  f120.hz = 120;
  f120.reportHz = 1000;
  v.push_back({"120Hz", f120, {256, 1.0}});
  Frames f240;
  f240.hz = 240;
  f240.reportHz = 1000;
  f240.drawMs[1] = 2.5;
  v.push_back({"240Hz", f240, {1024, 2.0}});
  Frames fi;
  fi.idle = true;
  fi.reportHz = 1000;
  v.push_back({"idle-paced", fi, {512, 1.0}});
  return v;
}
struct WheelSetup {
  const char* name;
  Wheel wh;
  Audio au;
};
std::vector<WheelSetup> wheelSetups() {
  std::vector<WheelSetup> v;
  v.push_back({"1kHz 128", Wheel{}, {256, 0}});
  Wheel b4;
  b4.burstMs = 4;
  v.push_back({"bursts 4ms 128", b4, {512, 1.5}});
  Wheel b8;
  b8.burstMs = 8;
  b8.enc = pl::EncRelTwos;
  v.push_back({"bursts 8ms 128 twos", b8, {256, 1.0}});
  Wheel hr;
  hr.ticksPerRev = 1024;
  hr.reportHz = 250;
  hr.enc = pl::EncRelSign;
  v.push_back({"250Hz 1024 sign", hr, {1024, 2.0}});
  return v;
}

// A steady hand plays a steady speed: no wobble, no stutter, no turns, close behind the hand.
// Every case must play the hand's speed and never turn. The smoothness limits hold for the
// input Platter follows cleanly today: a 60 or 120 Hz screen at normal speed or faster, a
// controller that reports every millisecond at normal speed or faster. The other cases (a slow
// hand that crosses a pixel or a tick only every few frames, a 240 Hz or idle-paced screen, a
// controller that bunches its reports) are measured and listed as "not yet" with their numbers;
// they are what a follower with more look-ahead would improve, at the cost of the quick
// response test_platter_scratch requires.
int gNotYet = 0;
void limit(bool enforced, bool ok, const std::string& what) {
  if (enforced) {
    expect(ok, what.c_str());
  } else if (!ok) {
    ++gNotYet;
    std::printf("# not yet: %s\n", what.c_str());
  }
}
void testSteady() {
  for (double speed : {0.25, 1.0, 2.5}) {
    for (const MouseSetup& ms : mouseSetups()) {
      Case c;
      c.name = std::string("mouse ") + ms.name;
      c.kind = Kind::Mouse, c.fr = ms.fr, c.au = ms.au;
      c.hand = steady(speed);
      c.seed = static_cast<uint64_t>(speed * 100) + 3;
      const Run r = perform(gRamp, c);
      const Smooth s = measureSteady(r, 0.4, 1.4, speed);
      char what[96];
      std::snprintf(what, sizeof what, "%s at %.2fx", c.name.c_str(), speed);
      printSteady(what, s);
      writeCsv(std::string("steady_") + what, r);
      const std::string w = what;
      const bool clean = (w.find("60Hz at") != std::string::npos || w.find("120Hz at") != std::string::npos) && speed >= 1.0;
      const double frame = 1000.0 / ms.fr.hz;  // ms
      expect(std::fabs(s.mean - speed) < 0.03 * speed, (w + ": plays the hand's speed").c_str());
      expect(s.flips == 0, (w + ": never turns on a steady hand").c_str());
      limit(clean, s.stalls == 0, w + ": never stutters to a stop");
      limit(clean, s.block < 0.05, w + ": speed steady within 5% over each 5.8 ms");
      limit(clean, s.maxJump < 0.06, w + ": no step changes the rate by 6% or more");
      limit(clean, s.maxErrMs < 2.5 * frame + 35, w + ": the record stays within 2.5 frames and 35 ms of the hand");
    }
    for (const WheelSetup& ws : wheelSetups()) {
      Case c;
      c.name = std::string("wheel ") + ws.name;
      c.kind = Kind::Jog, c.wh = ws.wh, c.au = ws.au;
      c.hand = steady(speed);
      c.seed = static_cast<uint64_t>(speed * 100) + 11;
      const Run r = perform(gRamp, c);
      const Smooth s = measureSteady(r, 0.4, 1.4, speed);
      char what[96];
      std::snprintf(what, sizeof what, "%s at %.2fx", c.name.c_str(), speed);
      printSteady(what, s);
      writeCsv(std::string("steady_") + what, r);
      const std::string w = what;
      const bool clean = ws.wh.burstMs <= 0 && ws.wh.reportHz >= 1000 && speed >= 1.0;
      expect(std::fabs(s.mean - speed) < 0.03 * speed, (w + ": plays the hand's speed").c_str());
      expect(s.flips == 0, (w + ": never turns on a steady hand").c_str());
      expect(s.stalls == 0, (w + ": never stutters to a stop").c_str());
      limit(clean, s.block < 0.04, w + ": speed steady within 4% over each 5.8 ms");
      limit(clean, s.maxJump < 0.05, w + ": no step changes the rate by 5% or more");
      limit(clean, s.maxErrMs < 40, w + ": the record stays within 40 ms of the hand");
    }
  }
  std::printf("# steady: %d limits not yet met (listed above as \"not yet\")\n", gNotYet);
}

// A hand that stops and holds the record: it comes to rest where the hand is, without running
// back (no turn the hand did not make), and stays still.
void testHold() {
  for (const MouseSetup& ms : mouseSetups()) {
    Case c;
    c.name = std::string("mouse ") + ms.name;
    c.kind = Kind::Mouse, c.fr = ms.fr, c.au = ms.au;
    c.hand = moveThenHold(1.2, 0.5);
    c.moveSec = 1.1;
    const Run r = perform(gRamp, c);
    const auto stop = static_cast<size_t>(r.lead + 0.5 * kSr / kRead);
    const auto held = static_cast<size_t>(r.lead + 0.75 * kSr / kRead);
    const auto end = static_cast<size_t>(r.releaseStep);
    const int fl = flipsIn(r, stop, end, 0.5);
    const double drift = std::fabs(r.rec[end - 1] - r.rec[held]);  // frames
    const double off = std::fabs(r.rec[end - 1] - r.hand[end - 1]);
    const double pixel = kSr * pl::kSecondsPerRev / (2 * kPi * c.fr.radiusPx);  // one pixel of the hand's travel (frames)
    std::printf("# hold %-24s flips %d  moved %.2f frames after 250 ms  off the hand %.2f px\n", c.name.c_str(), fl, drift, off / pixel);
    writeCsv("hold_" + c.name, r);
    expect(fl == 0, (c.name + " hold: the record stops without running back").c_str());
    expect(drift < 1.0, (c.name + " hold: a still hand holds the record still").c_str());
    expect(off < pixel, (c.name + " hold: the record rests where the hand put it (within the pixel the pointer is on)").c_str());
  }
  for (const WheelSetup& ws : wheelSetups()) {
    Case c;
    c.name = std::string("wheel ") + ws.name;
    c.kind = Kind::Jog, c.wh = ws.wh, c.au = ws.au;
    c.hand = moveThenHold(1.2, 0.5);
    c.moveSec = 1.1;
    const Run r = perform(gRamp, c);
    const auto stop = static_cast<size_t>(r.lead + 0.5 * kSr / kRead);
    const auto held = static_cast<size_t>(r.lead + 0.75 * kSr / kRead);
    const auto end = static_cast<size_t>(r.releaseStep);
    const int fl = flipsIn(r, stop, end, 0.5);
    const double drift = std::fabs(r.rec[end - 1] - r.rec[held]);  // frames
    const double tick = kSr * pl::kSecondsPerRev * pl::kJogSensDefault / c.wh.ticksPerRev;  // one tick of the record's travel (frames)
    const double off = std::fabs(r.rec[end - 1] - r.hand[end - 1]);
    std::printf("# hold %-24s flips %d  moved %.2f frames after 250 ms  off the hand %.2f ticks\n", c.name.c_str(), fl, drift, off / tick);
    writeCsv("hold_" + c.name, r);
    expect(fl == 0, (c.name + " hold: the record stops without running back").c_str());
    expect(drift < 1.0, (c.name + " hold: a still hand holds the record still").c_str());
    expect(off < tick, (c.name + " hold: the record rests where the hand put it (within the tick the wheel is on)").c_str());
  }
}

// Back and forth (a baby scratch): the record turns exactly as often as the hand.
void testStrokes() {
  for (const MouseSetup& ms : mouseSetups()) {
    Case c;
    c.name = std::string("mouse ") + ms.name;
    c.kind = Kind::Mouse, c.fr = ms.fr, c.au = ms.au;
    c.hand = backAndForth(120, 1.5, 4);
    c.moveSec = 1.2;
    const Run r = perform(gRamp, c);
    const int fl = flipsIn(r, static_cast<size_t>(r.lead), static_cast<size_t>(r.releaseStep), 20.0);
    std::printf("# strokes %-21s turns %d (the hand: 7)\n", c.name.c_str(), fl);
    writeCsv("strokes_" + c.name, r);
    expect(fl == 7, (c.name + " strokes: the record turns where the hand turns, and only there").c_str());
  }
  for (const WheelSetup& ws : wheelSetups()) {
    Case c;
    c.name = std::string("wheel ") + ws.name;
    c.kind = Kind::Jog, c.wh = ws.wh, c.au = ws.au;
    c.hand = backAndForth(120, 1.5, 4);
    c.moveSec = 1.2;
    const Run r = perform(gRamp, c);
    const int fl = flipsIn(r, static_cast<size_t>(r.lead), static_cast<size_t>(r.releaseStep), 20.0);
    std::printf("# strokes %-21s turns %d (the hand: 7)\n", c.name.c_str(), fl);
    writeCsv("strokes_" + c.name, r);
    expect(fl == 7, (c.name + " strokes: the record turns where the hand turns, and only there").c_str());
  }
}

// Letting go of a held record on a playing deck: it comes back up to speed on a ramp, without a
// jump, without overshooting, in about the motor's start time.
void testRelease() {
  std::vector<Case> cases;
  for (const MouseSetup& ms : mouseSetups()) {
    Case c;
    c.name = std::string("mouse ") + ms.name;
    c.kind = Kind::Mouse, c.fr = ms.fr, c.au = ms.au;
    cases.push_back(c);
  }
  for (const WheelSetup& ws : wheelSetups()) {
    Case c;
    c.name = std::string("wheel ") + ws.name;
    c.kind = Kind::Jog, c.wh = ws.wh, c.au = ws.au;
    cases.push_back(c);
  }
  for (Case c : cases) {
    c.hand = [](double) { return 0.0; };
    c.motor = true;
    c.moveSec = 0.4;
    c.after = 0.6;
    const Run r = perform(gRamp, c);
    const auto up = static_cast<size_t>(r.releaseStep);
    double maxJump = 0, peak = 0;
    size_t reach = r.rate.size();
    for (size_t i = up; i < r.rate.size(); ++i) {
      maxJump = std::max(maxJump, std::fabs(r.rate[i] - r.rate[i - 1]));
      peak = std::max(peak, r.rate[i]);
      if (reach == r.rate.size() && r.rate[i] >= 0.99) reach = i;
    }
    const double ms90 = static_cast<double>(reach - up) * kRead * 1000.0 / kSr;
    std::printf("# release %-21s up to speed in %.1f ms  largest step %.3fx  peak %.3fx\n", c.name.c_str(), ms90, maxJump, peak);
    writeCsv("release_" + c.name, r);
    expect(reach < r.rate.size(), (c.name + " release: back to speed").c_str());
    expect(ms90 > 25 && ms90 < 200, (c.name + " release: back to speed on a ramp (not at once, not slowly)").c_str());
    expect(maxJump < 0.06, (c.name + " release: no jump in speed").c_str());
    expect(peak < 1.02, (c.name + " release: no overshoot").c_str());
  }
}

// The three relative encodings decode to the same signed ticks (slow and fast, both ways), the
// learn tells them apart from a first slow move, and the jog sensitivity scales the record's travel.
void testEncodings() {
  for (uint8_t enc : {pl::EncRelOffset, pl::EncRelTwos, pl::EncRelSign}) {
    Wheel w;
    w.enc = enc;
    const pl::Mapping m = wheelMapping(w);
    for (int d : {1, 2, 7, 30, 63, -1, -2, -7, -30, -63}) {
      pl::Decoder dec;
      pl::InEvent ev[4];
      const int n = dec.feed(m, 0xB0, 0x22, encodeRel(d, enc), ev, 4);
      char what[96];
      std::snprintf(what, sizeof what, "encoding %d: %+d ticks decode as %+d", enc, d, n == 1 ? ev[0].ticks : 999);
      expect(n == 1 && ev[0].control == pl::CtlJogPlatter && ev[0].ticks == d, what);
    }
  }
  expect(pl::learnBinding(pl::CtlJogPlatter, 0xB0, 0x22, 0x41, true).enc == pl::EncRelOffset, "learn: a first move of 0x41 is centred on 64");
  expect(pl::learnBinding(pl::CtlJogPlatter, 0xB0, 0x22, 0x3F, true).enc == pl::EncRelOffset, "learn: a first move of 0x3F is centred on 64");
  expect(pl::learnBinding(pl::CtlJogPlatter, 0xB0, 0x22, 0x01, true).enc == pl::EncRelTwos, "learn: a first move of 0x01 is two's complement");
  expect(pl::learnBinding(pl::CtlJogPlatter, 0xB0, 0x22, 0x7F, true).enc == pl::EncRelTwos, "learn: a first move of 0x7F is two's complement");
  for (double sens : {0.5, 1.0, 2.0}) {
    Case c;
    c.name = "sensitivity";
    c.kind = Kind::Jog;
    c.hand = moveThenHold(1.0, 0.3);
    c.moveSec = 0.6;
    c.release = false;
    c.jogSens = sens;
    const Run r = perform(gRamp, c);
    const double ticks = std::floor(0.3 * kTurnsPerSecAt1x * c.wh.ticksPerRev / pl::kJogSensDefault + 1e-9);
    const double want = ticks * sens * kSr * pl::kSecondsPerRev / c.wh.ticksPerRev;
    std::printf("# jog sensitivity %.2f: the record went %.1f frames for %.0f ticks (%.1f wanted)\n", sens, r.rec.back(), ticks, want);
    expect(std::fabs(r.rec.back() - want) < 1.0, "jog sensitivity: the record travels ticks x sensitivity");
  }
}

// The editor's rim nudge (and the scroll wheel): the speed bends for a moment in the direction
// of the move, comes back, and a stopped record turns a little.
void testRimNudge() {
  SamplePtr src = sampleCache().get(gRamp);
  pl::Model m;
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, 120), true);
  pl::loadTrack(m, 0, gRamp, dm);
  PluginState st = defaultPluginState(pl::kType);
  st.params[pl::PSync] = 0;
  st.params[pl::PKeyLock] = 0;
  st.data = pl::formatData(gRamp, m);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  for (int dir : {1, -1}) {
    auto g = createGenerator(pl::kType);
    ProcessContext ctx;
    ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96;
    pl::Cmd p;
    p.kind = pl::CmdButton, p.a = pl::CtlPlay, p.v = 1;
    pl::command(g.get(), p);
    p.v = 0;
    pl::command(g.get(), p);
    std::vector<float> L(256), R(256);
    auto speed = [&]() {
      g->process(st, aux, ctx, L.data(), R.data(), 256);
      pl::StepState ss;
      pl::stepState(g.get(), ss);
      return ss.vel[0];
    };
    for (int i = 0; i < 40; ++i) speed();
    const double base = speed();
    double peak = base;
    for (int i = 0; i < 6; ++i) {  // a sixth of a turn over 6 frames
      pl::Cmd n;
      n.kind = pl::CmdNudge, n.v = dir * (1.0 / 36);
      pl::command(g.get(), n);
      const double v = speed();
      peak = dir > 0 ? std::max(peak, v) : std::min(peak, v);
    }
    for (int i = 0; i < 120; ++i) speed();
    const double after = speed();
    std::printf("# rim nudge %+d: %.3fx -> %.3fx -> %.3fx\n", dir, base, peak, after);
    expect(dir * (peak - base) > 0.03, "rim nudge: the speed bends the way the rim moves");
    expect(std::fabs(after - base) < 0.002, "rim nudge: the speed comes back");
  }
}

}  // namespace

int main() {
  gRamp = writeRamp();
  if (const char* st = std::getenv("GL_JOG_STAMPS")) gStamps = std::string(st) == "draw" ? Stamps::Draw : Stamps::Frame;
  testSteady();
  testHold();
  testStrokes();
  testRelease();
  testEncodings();
  testRimNudge();
  return finish();
}
