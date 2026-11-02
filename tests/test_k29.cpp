// Gridline - K-29: pitch bend lines, round 3. The editor's double-click edits (remove one point,
// a glide back to a straight ramp) keep every other point as it was; every glide shape (straight,
// smooth, ease in, ease out, step) at curve amounts -1, -0.5, 0, 0.5, 1 plays what the editor draws
// (bend::value): measured as the pitch of every cycle of a sine played through a built-in
// instrument, as the channel pitch bend the fallback path sends (MIDI Out), as MIDI file pitch bend
// and as the FL Studio slide-note plan. The amount changes each shape (monotonically, audibly);
// a step ignores it.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MidiFile.h"
#include "core/NoteBend.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTps = 192.0;  // ticks per second at 120 BPM, 96 PPQ

std::string sineWav(const std::string& name, double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 6));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = std::filesystem::absolute("test_k29_" + name + ".wav").string();
  writeWav(path, b, kSr, 16);
  return path;
}

Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 4;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}

ObjectId addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

Note mk(Tick start, Tick len, int pitch, std::vector<BendPoint> b = {}) {
  Note n(start, len, pitch, 100);
  n.bend = std::move(b);
  return n;
}

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

std::vector<std::pair<double, double>> pitchTrack(const AudioBuffer& b, double ref) {
  std::vector<double> ups;
  for (size_t i = 1; i < b.frames; ++i) {
    const double y0 = b.at(0, i - 1), y1 = b.at(0, i);
    if (y0 < 0 && y1 >= 0 && std::fabs(y1 - y0) > 1e-4) ups.push_back((static_cast<double>(i - 1) + -y0 / (y1 - y0)) / kSr);
  }
  std::vector<std::pair<double, double>> out;
  for (size_t i = 1; i < ups.size(); ++i) {
    const double p = ups[i] - ups[i - 1];
    if (p > 0) out.push_back({0.5 * (ups[i] + ups[i - 1]), 12.0 * std::log2(1.0 / p / ref)});
  }
  return out;
}
// The first time the track reaches `v` semitones at or after `from` (NaN when never).
double reaches(const std::vector<std::pair<double, double>>& tr, double v, double from) {
  for (const auto& p : tr)
    if (p.first >= from && p.second >= v) return p.first;
  return std::nan("");
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

const bend::Curve kCurves[] = {bend::Straight, bend::Smooth, bend::EaseIn, bend::EaseOut, bend::Step};
const char* kCurveNames[] = {"straight", "smooth", "ease in", "ease out", "step"};
const float kAmounts[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};

// The test line: 0 st until 48 ticks (0.25 s), then the glide to `top` st at 240 ticks (1.25 s), held.
std::vector<BendPoint> testLine(bend::Curve c, float amount, float top) { return {{48, 0}, {240, top, static_cast<uint8_t>(c), amount}}; }

}  // namespace

int main() {
  // ------------------------------------------------ double-click edits (core)
  {
    const std::vector<BendPoint> line = {{24, 2, bend::Smooth, 0.4f}, {48, 5, bend::EaseIn, -0.3f}, {72, -1, bend::Step}, {90, 3, bend::EaseOut, 0.6f}};
    for (size_t k = 0; k < line.size(); ++k) {
      std::vector<BendPoint> b = line;
      const bool ok = bend::removePoint(b, k);
      std::vector<BendPoint> want = line;
      want.erase(want.begin() + static_cast<std::ptrdiff_t>(k));
      expect(ok && b == want, ("removing point " + std::to_string(k) + " keeps every other point (time, value, shape, amount)").c_str());
    }
    std::vector<BendPoint> b = line;
    bend::removePoint(b, 1);
    // The glide into the next point now starts at the previous point and keeps its own shape.
    expect(near(bend::value(b, 24), 2, 1e-9) && near(bend::value(b, 72), -1, 1e-9) &&
               near(bend::value(b, 48), 2, 1e-9) /* a step holds until its point */,
           "after a removal the next glide starts at the previous point");
    b = line;
    bend::removePoint(b, 3);
    expect(b.size() == 3 && near(bend::value(b, 95), -1, 1e-9), "removing the last point: the line holds the previous value");
    b = line;
    expect(!bend::removePoint(b, 9) && b == line, "an index out of range changes nothing");
    std::vector<BendPoint> one = {{24, 0}, {48, 3}};
    bend::removePoint(one, 1);
    expect(one.empty(), "a line left flat is cleared");
    b = line;
    expect(bend::resetCurve(b, 1) && b[1].curve == bend::Straight && b[1].tension == 0 && b[0] == line[0] && b[2] == line[2] && b[3] == line[3],
           "resetting a curve makes that glide straight, the others stay");
    expect(!bend::resetCurve(b, 1), "a straight glide has nothing to reset");
    expect(bend::segmentAt(line, 0) == 0 && bend::segmentAt(line, 30) == 1 && bend::segmentAt(line, 72) == 3 && bend::segmentAt(line, 95) == -1,
           "the segment under a time is the glide into the next point (-1: the hold)");
  }

  // ------------------------------------------------ the amount changes every shape (and not a step)
  {
    bool mono = true, audible = true;
    for (int ci = 0; ci < 4; ++ci) {
      const bend::Curve c = kCurves[ci];
      for (int ai = 1; ai < 5; ++ai) {
        const double a = bend::shape(c, kAmounts[ai - 1], 0.25), b = bend::shape(c, kAmounts[ai], 0.25);
        // Ease out rises with the amount (a faster start), the others fall (a slower start, a steeper S).
        mono = mono && (c == bend::EaseOut ? b > a : b < a);
        double most = 0;  // where along the glide the two amounts differ most
        for (int x = 1; x < 100; ++x) most = std::max(most, std::fabs(bend::shape(c, kAmounts[ai], x / 100.0) - bend::shape(c, kAmounts[ai - 1], x / 100.0)));
        audible = audible && most * 12.0 > 0.5;  // > 50 cents apart somewhere on a 12 st glide
      }
    }
    expect(mono, "the curve amount moves every shape one way (at 25 % of the glide)");
    expect(audible, "each half step of the amount changes a 12 st glide by more than 50 cents");
    bool stepFlat = true;
    for (float a : kAmounts)
      for (double x : {0.0, 0.3, 0.99}) stepFlat = stepFlat && bend::shape(bend::Step, a, x) == 0.0;
    expect(stepFlat && bend::shape(bend::Step, 1.0, 1.0) == 1.0, "a step ignores the amount");
    expect(near(bend::shape(bend::EaseIn, -1, 0.3), 0.3, 1e-12) && near(bend::shape(bend::EaseOut, -1, 0.3), 0.3, 1e-12) &&
               near(bend::shape(bend::Smooth, -1, 0.3), 0.3, 1e-12) && near(bend::shape(bend::Straight, 0, 0.3), 0.3, 1e-12),
           "amount -1 softens ease in, ease out and smooth to a straight ramp");
  }

  // ------------------------------------------------ playback: a built-in instrument (per-note glide)
  const double f0 = 441.0;
  const std::string wav = sineWav("a441", f0);
  {
    double worstAll = 0;
    bool ok = true, stepOk = true;
    std::printf("# built-in instrument (gl.sampler), 0 -> +12 st over 1 s: cents from bend::value, worst / mean\n");
    for (int ci = 0; ci < 5; ++ci) {
      std::printf("#   %-9s", kCurveNames[ci]);
      for (float amt : kAmounts) {
        Song s = baseSong();
        const ObjectId ch = addChannel(s, "gl.sampler", wav);
        s.patterns[0].notes[ch] = {mk(0, 384, 60, testLine(kCurves[ci], amt, 12))};
        const auto tr = pitchTrack(render(s), f0);
        const auto& line = s.patterns[0].notes[ch][0].bend;
        double worst = 0, mean = 0;
        int n = 0;
        for (const auto& [t, st] : tr) {  // every cycle against the line half way through it
          if (t < 0.30 || t > 1.70 || (kCurves[ci] == bend::Step && std::fabs(t - 1.25) < 0.0015)) continue;  // the jump's own cycle
          const double d = std::fabs(st - bend::value(line, t * kTps)) * 100.0;
          worst = std::max(worst, d), mean += d, ++n;
        }
        mean /= std::max(1, n);
        std::printf("  %+.1f: %5.1f/%4.1f", static_cast<double>(amt), worst, mean);
        worstAll = std::max(worstAll, worst);
        ok = ok && worst < 20.0 && mean < 4.0;
        if (kCurves[ci] == bend::Step) {
          const double at = reaches(tr, 6.0, 0.3);
          stepOk = stepOk && near(at, 1.25, 0.003);
          std::printf(" (jump at %.4f s)", at);
        }
      }
      std::printf("\n");
    }
    expect(ok, "every shape and amount plays as drawn through a built-in instrument (worst < 20 cents, mean < 4)");
    expect(stepOk, "a step jumps at its point (within 3 ms), not a block early");
  }

  // ------------------------------------------------ the same at other block sizes
  {
    bool ok = true;
    std::printf("# block sizes: worst cents from bend::value (ease in +1, step)\n");
    for (int block : {32, 64, 256, 1024, 4096}) {
      double worst[2] = {0, 0}, jump = 0;
      const bend::Curve cs[2] = {bend::EaseIn, bend::Step};
      for (int i = 0; i < 2; ++i) {
        Song s = baseSong();
        const ObjectId ch = addChannel(s, "gl.sampler", wav);
        s.patterns[0].notes[ch] = {mk(0, 384, 60, testLine(cs[i], i == 0 ? 1.0f : 0.0f, 12))};
        const auto tr = pitchTrack(render(s, block), f0);
        const auto& line = s.patterns[0].notes[ch][0].bend;
        for (const auto& [t, st] : tr) {
          if (t < 0.30 || t > 1.70 || (i == 1 && std::fabs(t - 1.25) < 0.0015)) continue;
          worst[i] = std::max(worst[i], std::fabs(st - bend::value(line, t * kTps)) * 100.0);
        }
        if (i == 1) jump = reaches(tr, 6.0, 0.3);
      }
      std::printf("#   block %4d: ease in %5.1f, step %5.1f (jump at %.4f s)\n", block, worst[0], worst[1], jump);
      ok = ok && worst[0] < 20.0 && worst[1] < 20.0 && near(jump, 1.25, 0.003);
    }
    expect(ok, "curves and steps play as drawn at block sizes 32 to 4096");
  }

  // ------------------------------------------------ playback: the channel pitch bend fallback (MIDI Out)
  {
    std::printf("# channel pitch bend fallback (gl.midiout, range 2), 0 -> +2 st: cents from bend::value at each message's time\n");
    bool ok = true;
    for (int ci = 0; ci < 5; ++ci) {
      std::printf("#   %-9s", kCurveNames[ci]);
      for (float amt : kAmounts) {
        Document d(baseSong());
        const ObjectId c = d.addChannel("out", "gl.midiout");
        const ObjectId pat = d.song().patterns[0].id;
        const std::vector<BendPoint> line = testLine(kCurves[ci], amt, 2);
        d.setNotes(pat, c, {mk(0, 384, 64, line)}, "notes", 0);
        ProgramBuilder pb;
        AudioEngine eng(kSr);
        eng.setProgram(pb.build(d.song()));
        eng.setMode(PlayMode::Pattern);
        eng.setPattern(pat);
        eng.play();
        constexpr int kBlock = 64;  // one message per call at most: each one's time is known (the ring carries no offsets)
        std::vector<float> L(kBlock), R(kBlock);
        auto rt = pb.channelRuntime(c);
        // The value held between messages, compared with the line every 2 ms (a lag or lead of
        // one 512-sample block would show as the slope times 11.6 ms).
        std::vector<std::pair<double, double>> held;  // (block start time, semitones)
        for (int k = 0; k < 1600; ++k) {
          eng.process(L.data(), R.data(), kBlock);
          MidiOutRing::Msg m{};
          while (rt && rt->midiOut.pop(m))
            if ((m.s & 0xF0) == 0xE0) held.push_back({static_cast<double>(k) * kBlock / kSr, ((m.a | (m.b << 7)) - 8192) / 8192.0 * 2.0});
        }
        double worst = 0, mean = 0;
        int n = 0;
        for (double t = 0.30; t < 1.70; t += 0.002) {
          if (kCurves[ci] == bend::Step && std::fabs(t - 1.25) < 0.013) continue;
          double v = 0;
          for (const auto& h : held)
            if (h.first <= t + 1e-9) v = h.second;
          const double dv = std::fabs(v - bend::value(line, t * kTps)) * 100.0;
          worst = std::max(worst, dv), mean += dv, ++n;
        }
        mean /= std::max(1, n);
        std::printf("  %+.1f: %5.1f/%4.1f", static_cast<double>(amt), worst, mean);
        ok = ok && worst < 12.0 && mean < 2.5;
      }
      std::printf("\n");
    }
    expect(ok, "the channel pitch bend fallback follows every shape and amount (worst < 12 cents, mean < 2.5 on a 2 st glide)");
  }

  // ------------------------------------------------ MIDI file export and the FL slide plan
  {
    bool midiOk = true, flOk = true;
    double midiWorst = 0, flWorst = 0;
    for (int ci = 0; ci < 5; ++ci)
      for (float amt : kAmounts) {
        const std::vector<BendPoint> line = testLine(kCurves[ci], amt, 12);
        // MIDI: the file's bend messages (range 12) held between events vs the line.
        Song s = baseSong();
        const ObjectId ch = addChannel(s, "gl.synth");
        s.patterns[0].notes[ch] = {mk(0, 384, 60, line)};
        const MidiFileData m = patternToMidi(s, s.patterns[0].id);
        std::vector<std::pair<double, double>> ev;
        for (const auto& tr : m.tracks)
          for (const auto& c : tr.ctls)
            if (c.controller == kMidiPitchBend) ev.push_back({static_cast<double>(c.tick), (c.value - 8192) / 8192.0 * 12.0});
        double w = 0;
        for (int t = 0; t < 384; ++t) {  // the value held at every tick (the file's resolution)
          double v = 0;
          for (const auto& e : ev)
            if (e.first <= t) v = e.second;
          w = std::max(w, std::fabs(v - bend::value(line, t)));
        }
        midiWorst = std::max(midiWorst, w);
        midiOk = midiOk && w < 0.01;
        // FL: the slide pieces joined straight vs the line.
        const bend::SlidePlan plan = bend::toSlides(line, 384);
        auto planV = [&](double t) {
          double v = plan.start;
          for (const auto& pc : plan.pieces) {
            const double a = static_cast<double>(pc.tick), z = a + static_cast<double>(pc.length);
            if (t >= z) v = pc.target;
            else if (t > a) return v + (pc.target - v) * (t - a) / (z - a);
          }
          return v;
        };
        double fw = 0;
        for (double t = 0; t < 384; t += 0.5) {
          if (kCurves[ci] == bend::Step && std::fabs(t - 240) < 1.01) continue;
          fw = std::max(fw, std::fabs(planV(t) - bend::value(line, t)));
        }
        flWorst = std::max(flWorst, fw);
        flOk = flOk && fw < 0.1;
      }
    std::printf("# MIDI export: worst %.4f st at whole ticks (12 st glides); FL slide plan: worst %.3f st between ticks\n", midiWorst, flWorst);
    expect(midiOk, "MIDI export samples every shape and amount");
    expect(flOk, "the FL slide-note plan follows every shape and amount within 0.1 st");
  }
  return finish();
}
