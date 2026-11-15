// Gridline — "platter" pack tests: scratch gestures. Scripted hand moves (baby scratches,
// chirps, a transform, quick cuts, 10-40 ms flicks, a slow drag) go into Platter the way the
// app delivers them, in 256-sample audio callbacks: the controller's jog wheel (DDJ-200, 128
// ticks a turn, a report every millisecond) through the hub, and the mouse on the record (a
// place per UI frame, 60 or 120 Hz, in whole points of the screen) through the editor's
// commands, both stamped when they come and taken at the next callback. The record's place
// is read from a ramp track and measured against the hand: how much of every stroke it
// plays, how far behind it gets, how soon it turns after the input shows the turn, that it
// ends where the hand put it; a sine read through the same moves has no clicks; a steady
// hand plays a steady speed.
//
// GL_SCRATCH_OUT=<dir>: also writes, per input, the gesture set played on music (WAV, one
// after the other) and each gesture's hand, record and input travel per control step (CSV).
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
constexpr int kAppBlock = 256;  // the audio callback the app runs (5.8 ms)
constexpr int kRead = pl::kStep;  // the record's place is read after every control step
constexpr double kTicksPerRev = 128;  // DDJ-200

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_platter_scratch_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

std::string writeTrack(const std::string& name, double seconds, const std::function<double(int64_t, int)>& fn) {
  const auto n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) b.data[static_cast<size_t>(c) * n + i] = static_cast<float>(fn(static_cast<int64_t>(i), c));
  const std::string path = (tmpDir() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

// Music-like material (partials, a kick-like hit on every beat at 120 BPM).
double music(int64_t i, int c) {
  const double t = static_cast<double>(i) / kSr;
  const double ph = std::fmod(t, 0.5);
  double x = 0.22 * std::sin(2 * kPi * 220 * t + c) + 0.1 * std::sin(2 * kPi * 331 * t) + 0.07 * std::sin(2 * kPi * 1250 * t + 0.5 * c);
  x += 0.1 * std::sin(2 * kPi * 2900 * t) * (0.5 + 0.5 * std::sin(2 * kPi * 3 * t));
  x += 0.4 * std::exp(-ph * 60.0) * std::sin(2 * kPi * 90 * ph);
  return x;
}

// ------------------------------------------------------------------ gestures
// A gesture is a list of strokes: the record moved `turns` (forward > 0) over `ms`, each on a
// smooth (raised cosine) path, still at both ends; a stroke of 0 turns holds the record.
struct Stroke {
  double ms, turns;
  bool steady = false;  // at one speed throughout (not still at its ends)
};
struct Gesture {
  std::string name;
  std::vector<Stroke> s;
  double ms() const {
    double t = 0;
    for (const Stroke& k : s) t += k.ms;
    return t;
  }
  // The hand's travel (turns of the record) `ms` into the gesture.
  double at(double ms) const {
    double t0 = 0, h = 0;
    for (const Stroke& k : s) {
      if (ms < t0 + k.ms) {
        const double u = std::max(0.0, (ms - t0) / k.ms);
        return h + k.turns * (k.steady ? u : 0.5 * (1.0 - std::cos(kPi * u)));
      }
      t0 += k.ms;
      h += k.turns;
    }
    return h;
  }
};
constexpr double kTurnsPerSecAt1x = 1.0 / pl::kSecondsPerRev;

// Out and back `n` times, `ms` each way, at a mean speed of `speed` x normal, `hold` ms apart.
Gesture backAndForth(const std::string& name, double ms, double speed, int n, double hold = 0) {
  Gesture g{name, {}};
  const double a = speed * kTurnsPerSecAt1x * ms * 1e-3;
  for (int k = 0; k < n; ++k) {
    g.s.push_back({ms, a});
    g.s.push_back({ms, -a});
    if (hold > 0 && k + 1 < n) g.s.push_back({hold, 0});
  }
  return g;
}
std::vector<Gesture> gestures() {
  std::vector<Gesture> v;
  v.push_back(backAndForth("baby", 90, 2.0, 3));
  v.push_back(backAndForth("chirp", 40, 3.0, 4));
  {
    // a transform's hand: the record pushed through at about normal speed and pulled back
    Gesture g{"transform", {{350, 0.19}, {350, -0.19}}};
    v.push_back(g);
  }
  {
    // quick cuts: short forward stabs with the record held between them, then back
    Gesture g{"cuts", {}};
    for (int k = 0; k < 4; ++k) g.s.push_back({22, 0.045}), g.s.push_back({45, 0});
    g.s.push_back({160, -0.18});
    v.push_back(g);
  }
  // very short back-and-forth flicks (a stroke each way: 10 ms flick = 5 ms out, 5 ms back)
  v.push_back(backAndForth("flick10", 5, 7.0, 3, 70));
  v.push_back(backAndForth("flick20", 10, 6.0, 3, 70));
  v.push_back(backAndForth("flick30", 15, 5.0, 3, 70));
  v.push_back(backAndForth("flick40", 20, 5.0, 3, 70));
  {
    Gesture g{"drag", {{800, 0.3 * kTurnsPerSecAt1x * 0.8}, {200, 0}, {500, -0.3 * kTurnsPerSecAt1x * 0.5}}};
    v.push_back(g);
  }
  return v;
}

// ------------------------------------------------------------------ the deck
struct Deck {
  std::unique_ptr<Generator> g;
  PluginState st;
  ChannelAux aux;
  ProcessContext ctx;
  pl::Model m;
  int64_t pos = 0;
};

Deck makeDeck(const std::string& path) {
  Deck d;
  SamplePtr src = sampleCache().get(path);
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, 120), true);
  dm.cue = 0.5 * static_cast<double>(src->frames());  // the hand starts in the middle of the track
  pl::loadTrack(d.m, 0, path, dm);
  d.m.map = pl::profileMapping("ddj200", pl::kSideBoth);
  d.st = defaultPluginState(pl::kType);
  d.st.params[pl::PSync] = 0;
  d.st.params[pl::PKeyLock] = 0;
  d.st.data = pl::formatData(path, d.m);
  packs::prepareAux(d.st, d.aux);
  d.g = createGenerator(pl::kType);
  d.ctx.sampleRate = kSr;
  d.ctx.tempo = 120;
  d.ctx.ppq = 96;
  return d;
}

void cmd(Deck& d, uint8_t kind, double v = 0) {
  pl::Cmd c;
  c.kind = kind;
  c.v = v;
  pl::command(d.g.get(), c);
}

enum class Input { Jog, Mouse60, Mouse120 };
const char* inputName(Input in) { return in == Input::Jog ? "jog" : in == Input::Mouse60 ? "mouse60" : "mouse120"; }

// The app's clock (the hub and the editor commands stamp their input with it): it runs with the samples.
int64_t gNs = 0;
int64_t fakeNow() { return gNs; }
int64_t nsAt(double sample) { return int64_t{1000000000} + static_cast<int64_t>(std::llround(sample * 1e9 / kSr)); }

// The ramp track's left channel is its own position (right: a constant), so the output tells
// the record's place at every sample (the level, the fader and the master scale both alike).
constexpr double kRampTop = 0.9;
double ramp(int64_t i, int c, double frames) { return c == 0 ? kRampTop * static_cast<double>(i) / frames : 0.5; }

struct Trace {
  std::vector<double> hand, rec;  // frames of record travel after each read step (from the gesture's start)
  std::vector<double> input;      // ... and the hand as the engine has it by then (ticks or mouse moves, a callback late)
  std::vector<float> L, R;        // the audio from the gesture's start
  double lead = 0;                // samples before the gesture starts (in L / R and the trace)
  double handEnd = 0;             // where the hand left the record, as the input said it (frames)
};

// A waveform drag moves in whole points of the screen: about 600 frames each at the default zoom.
constexpr double kMousePoint = 600;

// Plays gesture `g` through `in` on the track at `path` the way the app delivers it: 256-sample
// audio callbacks; the wheel's messages (a report every millisecond) through the hub, stamped
// when they come; the mouse once a UI frame through the editor's commands, stamped the same way;
// the engine takes what came during a callback at the next one's start. `rampFrames` > 0: `path`
// is the ramp track of that length (the record's place is read from its output).
Trace perform(const std::string& path, const Gesture& g, Input in, double rampFrames = 0) {
  Deck d = makeDeck(path);
  d.ctx.offline = false;
  pl::hub::setClock(fakeNow);
  pl::hub::setTapped(in == Input::Jog);
  const double sens = pl::kJogSensDefault;
  const double fpTurn = kSr * pl::kSecondsPerRev;
  const auto leadIn = static_cast<int64_t>(0.06 * kSr);  // the hand goes down this long before it moves
  const int64_t settle = 16 * kAppBlock;
  const int64_t total = settle + leadIn + static_cast<int64_t>((g.ms() + 300.0) * 1e-3 * kSr);
  const int64_t g0 = settle + leadIn;  // the gesture's first sample
  struct Ev {
    int64_t at;
    uint8_t kind;  // 0 jog message, 1 mouse down, 2 mouse move
    uint8_t s, a, b;
    double v;
  };
  std::vector<Ev> evs;  // input that came during the callback that plays
  long sent = 0;
  double nextPoll = static_cast<double>(g0);
  const double framePeriod = in == Input::Mouse60 ? kSr / 60.0 : kSr / 120.0;
  double nextFrame = static_cast<double>(settle) + 0.4 * framePeriod;  // (UI frames are not in step with the audio)
  Trace tr;
  tr.lead = static_cast<double>(leadIn);
  std::vector<float> L, R;
  std::vector<std::pair<int64_t, double>> seen;  // (sample the engine has it, the hand's travel the input says)
  for (int64_t c = 0; c < total; c += kAppBlock) {
    for (const Ev& e : evs) {  // (came while the last callback played)
      gNs = nsAt(static_cast<double>(e.at));
      if (e.kind == 0) pl::hub::push(0, e.s, e.a, e.b);
      else cmd(d, e.kind == 1 ? pl::CmdHandDown : pl::CmdHandMove, e.v);
    }
    evs.clear();
    gNs = nsAt(static_cast<double>(c));
    float bl[kAppBlock], br[kAppBlock];
    std::fill(bl, bl + kAppBlock, 0.f);
    std::fill(br, br + kAppBlock, 0.f);
    d.ctx.tick = static_cast<double>(d.pos) / kSr * d.ctx.tempo / 60.0 * d.ctx.ppq;
    d.ctx.jumped = d.pos == 0;
    d.g->process(d.st, d.aux, d.ctx, bl, br, kAppBlock);
    d.pos += kAppBlock;
    L.insert(L.end(), bl, bl + kAppBlock);
    R.insert(R.end(), br, br + kAppBlock);
    // the hand during this callback
    for (int64_t s = c; s < c + kAppBlock; ++s) {
      const double hand = g.at(static_cast<double>(s - g0) * 1000.0 / kSr);
      if (in == Input::Jog) {
        if (s == settle) evs.push_back({s, 0, 0x90, 0x36, 0x7F, 0});
        if (s >= g0 && static_cast<double>(s) >= nextPoll) {
          nextPoll += kSr * 0.001;
          const long want = static_cast<long>(std::floor(hand / sens * kTicksPerRev + 1e-9));
          const long dl = std::clamp(want - sent, -63L, 63L);
          if (dl != 0) {
            evs.push_back({s, 0, 0xB0, 0x22, static_cast<uint8_t>(0x40 + dl), 0});
            sent += dl;
            tr.handEnd = static_cast<double>(sent) / kTicksPerRev * sens * fpTurn;
            seen.push_back({s + kAppBlock, tr.handEnd});
          }
        }
      } else {
        if (s == settle) evs.push_back({s, 1, 0, 0, 0, 0});
        if (s > settle && static_cast<double>(s) >= nextFrame) {
          nextFrame += framePeriod;
          const double f = std::round(hand * fpTurn / kMousePoint) * kMousePoint;  // (whole points)
          evs.push_back({s, 2, 0, 0, 0, f / fpTurn});
          tr.handEnd = f;
          seen.push_back({s + kAppBlock, f});
        }
      }
    }
  }
  pl::hub::setTapped(false);
  pl::hub::setClock(nullptr);
  // the trace from the gesture's lead-in: the audio, and the record's place after every read step
  const auto from = static_cast<size_t>(settle);
  tr.L.assign(L.begin() + static_cast<std::ptrdiff_t>(from), L.end());
  tr.R.assign(R.begin() + static_cast<std::ptrdiff_t>(from), R.end());
  const double start = 0.5 * rampFrames;  // (the record rests on the cue: the middle of the track)
  double last = start;  // (a silent record: where it was last heard)
  size_t si = 0;
  double inHand = 0;
  for (size_t i = from; i < L.size(); ++i) {
    for (; si < seen.size() && seen[si].first <= static_cast<int64_t>(i); ++si) inHand = seen[si].second;
    if (rampFrames > 0 && std::fabs(R[i]) > 1e-4) last = static_cast<double>(L[i]) / R[i] * 0.5 / kRampTop * rampFrames;
    if ((i + 1 - from) % kRead == 0) {
      const double ms = (static_cast<double>(i + 1 - from) - tr.lead) * 1000.0 / kSr;
      tr.rec.push_back(last - start);
      tr.input.push_back(inHand);
      tr.hand.push_back(g.at(ms) * fpTurn);
    }
  }
  return tr;
}

// ------------------------------------------------------------------ measures
struct StrokeStat {
  double reach = 0;      // the record's excursion over the hand's (1 = all of it)
  double onsetMs = 0;    // the record reaches 10% of the stroke this much after the hand does
  double midMs = 0;      // ... and half of it
  double turnMs = NAN;   // it turns this long after the hand turned (strokes the hand reverses)
  double turnInMs = NAN; // ... and after the input showed the turn (the first tick or move back reached the engine)
};
struct Stats {
  std::vector<StrokeStat> strokes;
  double minReach = 1e9, maxOnset = 0, maxMid = 0, maxTurn = 0, maxTurnIn = 0, meanReach = 0, endErr = 0;
  int turns = 0;
};
// The trace's time (ms from the gesture's start) of read step i.
double msOf(const Trace& t, size_t i) { return (static_cast<double>(i + 1) * kRead - t.lead) * 1000.0 / kSr; }
size_t stepAt(const Trace& t, double ms) {
  const double i = (ms * 1e-3 * kSr + t.lead) / kRead - 1;
  return static_cast<size_t>(std::clamp(std::round(i), 0.0, static_cast<double>(t.rec.size() - 1)));
}

Stats measure(const Gesture& g, const Trace& t) {
  Stats st;
  const double fpTurn = kSr * pl::kSecondsPerRev;
  double t0 = 0, h0 = 0, prevPeak = NAN;
  int prevDir = 0;
  for (size_t k = 0; k < g.s.size(); ++k) {
    const Stroke& s = g.s[k];
    const double t1 = t0 + s.ms, h1 = h0 + s.turns;
    if (s.turns == 0) {
      prevDir = 0;
      t0 = t1, h0 = h1;
      continue;
    }
    const int dir = s.turns > 0 ? 1 : -1;
    const double span = std::fabs(s.turns) * fpTurn;
    const bool reversed = k + 1 < g.s.size() && g.s[k + 1].turns * s.turns < 0;
    // (to half way through the stroke back, and the input's own lag: a stroke this short can
    // still be on its way out when the hand is already coming back)
    const double wEnd = reversed ? t1 + 0.5 * g.s[k + 1].ms + 15.0 : t1 + 60.0;
    const size_t a = stepAt(t, t0), b = stepAt(t, wEnd);
    const double base = prevDir == -dir && std::isfinite(prevPeak) ? prevPeak : t.rec[a];
    double peak = t.rec[a];
    size_t ip = a;
    for (size_t i = a; i <= b; ++i)
      if (dir * t.rec[i] > dir * peak) peak = t.rec[i], ip = i;
    StrokeStat ss;
    ss.reach = dir * (peak - base) / span;
    auto cross = [&](double frac) {
      for (size_t i = a; i <= b; ++i)
        if (dir * (t.rec[i] - base) >= frac * span) return msOf(t, i);
      return wEnd + 100.0;
    };
    const double u10 = std::acos(0.8) / kPi;  // the raised cosine reaches 10% here
    ss.onsetMs = cross(0.1) - (t0 + u10 * s.ms);
    ss.midMs = cross(0.5) - (t0 + 0.5 * s.ms);
    if (reversed && ip < b) {
      ss.turnMs = msOf(t, ip) - t1;
      double ipk = t.input[a];
      size_t ii = a;
      for (size_t i = a; i <= b; ++i)
        if (dir * t.input[i] > dir * ipk) ipk = t.input[i], ii = i;
      for (size_t i = ii; i <= b; ++i)
        if (dir * (t.input[i] - ipk) < 0) {
          ss.turnInMs = msOf(t, ip) - msOf(t, i);
          break;
        }
    }
    st.strokes.push_back(ss);
    prevPeak = peak, prevDir = dir;
    t0 = t1, h0 = h1;
  }
  double sum = 0;
  for (const StrokeStat& s : st.strokes) {
    st.minReach = std::min(st.minReach, s.reach);
    st.maxOnset = std::max(st.maxOnset, s.onsetMs);
    st.maxMid = std::max(st.maxMid, s.midMs);
    sum += s.reach;
    if (std::isfinite(s.turnMs)) st.maxTurn = std::max(st.maxTurn, s.turnMs), ++st.turns;
    if (std::isfinite(s.turnInMs)) st.maxTurnIn = std::max(st.maxTurnIn, s.turnInMs);
  }
  st.meanReach = st.strokes.empty() ? 0 : sum / static_cast<double>(st.strokes.size());
  st.endErr = std::fabs(t.rec.back() - t.handEnd);
  return st;
}

// Level of a record at `r` times its speed (as the decks set it).
double vinylGain(double r) {
  r = std::fabs(r);
  return r >= 1.0 ? 1.0 : std::log1p(4.0 * r) / std::log(5.0);
}
// Clicks in a sine (amplitude <= 0.5, 220 Hz) read through the moves: samples whose step is
// larger than the sine at the local speed and level change could make. `place`: the record's
// travel per read step for the same moves (from the ramp track).
int clicks(const std::vector<float>& x, const std::vector<double>& place, double& worst) {
  int n = 0;
  worst = 0;
  const size_t steps = place.size();
  std::vector<double> v(steps, 0.0);
  for (size_t k = 1; k < steps; ++k) v[k] = (place[k] - place[k - 1]) / kRead;
  for (size_t i = 1; i < x.size(); ++i) {
    const size_t k = i / kRead;
    double vl = 0, gl = 0;
    for (size_t j = k > 2 ? k - 2 : 1; j <= std::min(steps - 1, k + 2); ++j) {
      vl = std::max(vl, std::fabs(v[j]));
      gl = std::max(gl, std::fabs(vinylGain(v[j]) - vinylGain(v[j - 1])) / kRead);
    }
    const double allowed = 1.6 * (0.5 * 2 * kPi * 220 / kSr * vl + 0.5 * gl) + 0.003;
    const double step = std::fabs(static_cast<double>(x[i]) - x[i - 1]);
    worst = std::max(worst, step / allowed);
    if (step > allowed) ++n;
  }
  return n;
}

void writeCsv(const std::string& path, const Trace& t) {
  std::ofstream f(path);
  f << "ms,hand,record,input\n";
  for (size_t i = 0; i < t.rec.size(); ++i) f << msOf(t, i) << "," << t.hand[i] << "," << t.rec[i] << "," << t.input[i] << "\n";
}

// ------------------------------------------------------------------ tests
std::string gRamp;  // the ramp track
constexpr double kRampFrames = 8 * kSr;

void testGestures(const std::string& musicPath, const std::string& sinePath, const char* out) {
  const auto set = gestures();
  for (Input in : {Input::Jog, Input::Mouse60, Input::Mouse120}) {
    std::vector<float> wavL, wavR;
    for (const Gesture& g : set) {
      const Trace t = perform(gRamp, g, in, kRampFrames);
      const Stats s = measure(g, t);
      std::printf("# %-8s %-9s reach min %3.0f%% mean %3.0f%%; half way <= %5.1f ms behind the hand; turns <= %5.1f ms after the hand, "
                  "<= %4.1f ms after the input (%d); ends %.2f frames off\n",
                  inputName(in), g.name.c_str(), 100 * s.minReach, 100 * s.meanReach, s.maxMid, s.maxTurn, s.maxTurnIn, s.turns, s.endErr);
      const Trace ts = perform(sinePath, g, in);
      double worst = 0;
      const int c = clicks(ts.L, t.rec, worst);
      std::printf("# %-8s %-9s clicks %d (largest step %.2f of the bound)\n", inputName(in), g.name.c_str(), c, worst);
      const std::string what = std::string(inputName(in)) + " " + g.name;
      expect(c == 0, (what + ": no clicks").c_str());
      expect(s.endErr < 0.5, (what + ": the record ends where the hand put it").c_str());
      const bool slow = g.name == "drag" || g.name == "transform";
      if (in == Input::Jog) {
        // the wheel: every stroke comes through (nearly) whole, close behind the hand, and the
        // record turns a few ms after the wheel's first tick back reaches the engine (the input
        // is an audio callback late, 5.8 ms, and comes in whole ticks of 1/128 turn). A flick
        // of a few ms each way is only a few ticks, most of it still in the callback's delay
        // when the hand is back: those keep most of their stroke.
        const double reach = g.name == "flick10" ? 0.3 : g.name == "flick20" ? 0.6 : g.name == "flick30" ? 0.75 : 0.85;
        expect(s.minReach > reach, (what + ": the record plays (nearly) all of every stroke").c_str());
        if (g.name != "flick10") expect(s.maxMid < (slow ? 70.0 : 20.0), (what + ": half way through every stroke soon after the hand").c_str());
        expect(s.turns == 0 || s.maxTurnIn < 6.0, (what + ": it turns within a few ms of the wheel's turn").c_str());
      } else {
        // the mouse: as the UI sees it (a place per frame, in whole points), about a frame behind
        const double frame = in == Input::Mouse60 ? 1000.0 / 60 : 1000.0 / 120;
        const bool fast = g.name.rfind("flick", 0) == 0 || (in == Input::Mouse60 && g.name == "chirp");
        if (!fast) {
          expect(s.minReach > (g.name == "chirp" ? 0.65 : 0.75), (what + ": the record plays (nearly) all of every stroke the UI sees").c_str());
          expect(s.maxMid < (slow ? 90.0 : 2.5 * frame + 12.0), (what + ": half way through every stroke a frame or two behind the hand").c_str());
        }
        expect(s.turns == 0 || s.maxTurnIn < frame + 6.0, (what + ": it turns within a frame or so of the mouse's turn").c_str());
      }
      if (out) {
        writeCsv((fs::path(out) / (std::string(inputName(in)) + "_" + g.name + ".csv")).string(), t);
        const Trace tm = perform(musicPath, g, in);
        wavL.insert(wavL.end(), tm.L.begin(), tm.L.end());
        wavR.insert(wavR.end(), tm.R.begin(), tm.R.end());
        wavL.insert(wavL.end(), static_cast<size_t>(0.35 * kSr), 0.f);
        wavR.insert(wavR.end(), static_cast<size_t>(0.35 * kSr), 0.f);
      }
    }
    if (out) {
      AudioBuffer b(2, wavL.size());
      std::copy(wavL.begin(), wavL.end(), b.data.begin());
      std::copy(wavR.begin(), wavR.end(), b.data.begin() + static_cast<std::ptrdiff_t>(wavL.size()));
      writeWav((fs::path(out) / (std::string("gestures_") + inputName(in) + ".wav")).string(), b, static_cast<int>(kSr), 24);
    }
  }
}

// Steady moves stay steady: the wheel spun at a steady speed (its reports every millisecond,
// whatever the tick spacing), a steady mouse drag (60 Hz frames in whole points): no wobble.
void testSteady() {
  struct Case {
    Input in;
    double speed, maxWobble;
  };
  for (const Case& c : {Case{Input::Jog, 0.4, 0.03}, Case{Input::Jog, 1.0, 0.03}, Case{Input::Jog, 3.0, 0.03}, Case{Input::Jog, 8.0, 0.03},
                        Case{Input::Mouse60, 0.3, 0.09}, Case{Input::Mouse60, 1.0, 0.06}, Case{Input::Mouse60, 2.0, 0.04}}) {
    // the hand at `speed` for 1.4 s (shorter at high speeds: as much as the test track holds)
    const double a = c.speed * kTurnsPerSecAt1x;
    const double ms = std::min(1400.0, 3500.0 / c.speed);
    Gesture g{"steady", {{ms, a * ms * 1e-3, true}}};
    const Trace t = perform(gRamp, g, c.in, kRampFrames);
    double s = 0, s2 = 0;
    int n = 0;
    for (size_t i = stepAt(t, ms * 3 / 7) + 1; i <= stepAt(t, ms * 13 / 14); ++i) {
      const double v = (t.rec[i] - t.rec[i - 1]) / kRead;
      s += v, s2 += v * v, ++n;
    }
    const double mean = s / n, sd = std::sqrt(std::max(0.0, s2 / n - mean * mean));
    std::printf("# steady %-8s at %.1fx: %.3fx, wobble %.1f%%\n", inputName(c.in), c.speed, mean, 100 * sd / mean);
    const std::string what = std::string(inputName(c.in)) + " steady";
    expect(std::fabs(mean - c.speed) < 0.03 * c.speed && sd < c.maxWobble * mean, (what + ": a steady hand plays a steady speed").c_str());
  }
}

}  // namespace

int main() {
  const std::string musicPath = writeTrack("music.wav", 8, music);
  const std::string sinePath = writeTrack("sine220.wav", 8, [](int64_t i, int) { return 0.5 * std::sin(2 * kPi * 220 * static_cast<double>(i) / kSr); });
  gRamp = writeTrack("ramp.wav", 8, [](int64_t i, int c) { return ramp(i, c, kRampFrames); });
  const char* out = std::getenv("GL_SCRATCH_OUT");
  if (out) {
    std::error_code ec;
    fs::create_directories(out, ec);
  }
  testGestures(musicPath, sinePath, out);
  testSteady();
  return finish();
}
