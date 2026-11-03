// Gridline - K-AB: pitch bend export to FL Studio projects, the gaps closed.
//
// FL's slide notes glide every sounding note of their colour, by the interval from the last note
// of that colour started. A bent note sharing its colour with another sounding note moves to a free
// colour; only when none is free (or the colour selects something in the instrument) is the line
// left out, and counted. Curves become ramps on FL's 10 cent grid within a stated bound.
//
// Every check goes through the real exporter and Gridline's own FL importer; `playedAt` replays the
// reimported notes by the engine's slide rule and is compared with bend::value at sample points.
// The renders at the end prove the same by ear (pitch tracks).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/NoteBend.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpExportInternal.h"
#include "engine/Render.h"
#include "packs/plasma/plasma.h"

using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

constexpr int kSr = 44100;
constexpr double kBound = 5.05;  // cents: half of FL's 10 cent fine pitch step, and a hair for rounding
constexpr double kPi = 3.14159265358979323846;
const char* const kWritten = "notes: pitch bend lines written as slide notes";
const char* const kMoved = "notes: pitch bend lines moved to a free note colour (their slides would glide other notes of the old colour)";
const char* const kNoColour = "notes: pitch bend lines not written (other notes of their colour sound with them and no note colour is free)";
const char* const kColourSelects = "notes: pitch bend lines not written (other notes of their colour sound with them and this instrument reads note colours)";
const char* const kUnderSlide = "notes: pitch bend lines not written (a slide note of their colour plays over them or counts from them)";
const char* const kRelaxed = "notes: pitch bend lines over the limit of 64 slides (a wider tolerance was used)";

Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 8;
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

Note mk(Tick start, Tick len, int pitch, std::vector<BendPoint> b = {}, int color = 0) {
  Note n(start, len, pitch, 100);
  n.bend = std::move(b);
  n.color = color;
  return n;
}
Note slideNote(Tick start, Tick len, int pitch, int color = 0) {
  Note n(start, len, pitch, 100);
  n.slide = true;
  n.color = color;
  return n;
}

bool roundTrip(const Song& s, Song& back, flp::ExportReport& rep, std::vector<uint8_t>* bytesOut = nullptr) {
  std::vector<uint8_t> bytes;
  flp::ExportOptions opt;
  opt.renderInstruments = false;
  if (!flp::exportFlp(s, bytes, rep, opt)) return false;
  flp::Report ir;
  std::string err;
  const bool ok = importFlpBytes(bytes, back, ir, &err, "", "", false);
  if (bytesOut) *bytesOut = std::move(bytes);
  return ok;
}
std::vector<Note> firstNotes(const Song& s) {
  for (const auto& p : s.patterns)
    for (const auto& [ch, ns] : p.notes)
      if (!ns.empty()) return ns;
  return {};
}

// ---- the engine's slide rule (AudioEngine::startNote), replayed on reimported notes ----
struct Voice {
  double start = 0, end = 0, pitch = 0;
  int color = 0;
  struct Glide {
    double t0, v0, t1, v1;
  };
  std::vector<Glide> glides;
  double at(double t) const {
    double v = pitch;
    for (const Glide& g : glides) {
      if (t < g.t0) break;
      v = t >= g.t1 ? g.v1 : g.v0 + (g.v1 - g.v0) * (t - g.t0) / (g.t1 - g.t0);
    }
    return v;
  }
};
std::vector<Voice> play(std::vector<Note> ns) {
  std::stable_sort(ns.begin(), ns.end(), [](const Note& a, const Note& b) {
    if (a.start != b.start) return a.start < b.start;
    return !a.slide && b.slide;
  });
  std::vector<Voice> out;
  std::map<int, double> last;
  for (const Note& n : ns) {
    const double p = n.pitch + n.fine / 100.0, t = static_cast<double>(n.start);
    if (!n.slide) {
      Voice v;
      v.start = t, v.end = t + static_cast<double>(n.length), v.pitch = p, v.color = n.color;
      out.push_back(v);
      last[n.color] = p;
      continue;
    }
    if (!last.count(n.color)) continue;
    const double shift = p - last[n.color];
    for (Voice& v : out)
      if (v.color == n.color && v.start <= t && t < v.end) v.glides.push_back({t, v.at(t), t + static_cast<double>(n.length), v.pitch + shift});
  }
  return out;
}

struct Err {
  double worst = 0, sum = 0;
  int n = 0;
  void add(double cents) {
    worst = std::max(worst, cents);
    sum += cents;
    ++n;
  }
  double mean() const { return n ? sum / n : 0; }
};

// Every note of `orig` against the voice played from `back`, every quarter tick: bends kept where
// `keepBend`, flat otherwise. A step's own tick (the format's shortest slide) is skipped.
Err compareLines(const std::vector<Note>& orig, const std::vector<Note>& back) {
  std::vector<Voice> vs = play(back);
  std::vector<bool> used(vs.size(), false);
  Err e;
  for (const Note& n : orig) {
    if (n.slide) continue;
    const double want0 = n.pitch + n.fine / 100.0 + bend::value(n.bend, 0.0);
    int best = -1;
    for (size_t i = 0; i < vs.size(); ++i)
      if (!used[i] && vs[i].start == static_cast<double>(n.start) && (best < 0 || std::fabs(vs[i].pitch - want0) < std::fabs(vs[static_cast<size_t>(best)].pitch - want0)))
        best = static_cast<int>(i);
    if (best < 0) {
      e.add(1e6);
      continue;
    }
    used[static_cast<size_t>(best)] = true;
    for (double t = 0; t < static_cast<double>(n.length); t += 0.25) {
      bool skip = false;
      for (const BendPoint& p : n.bend)
        if (p.curve == bend::Step && t >= std::round(p.tick) && t <= std::round(p.tick) + 1.0) skip = true;
      if (skip) continue;
      const double want = n.pitch + n.fine / 100.0 + bend::value(n.bend, t);
      e.add(std::fabs(vs[static_cast<size_t>(best)].at(static_cast<double>(n.start) + t) - want) * 100.0);
    }
  }
  return e;
}

int count(const std::vector<Note>& ns, bool slide) {
  int k = 0;
  for (const Note& n : ns) k += n.slide == slide;
  return k;
}

// ---- renders ----
std::string sineWav(const std::string& name, double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 8));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = std::filesystem::absolute("test_kab_" + name + ".wav").string();
  writeWav(path, b, kSr, 16);
  return path;
}
AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.blockSize = 512;
  return renderSong(s, o);
}
// The high tone's pitch per cycle: two first differences lift it far above a tone four octaves down.
std::vector<std::pair<double, double>> highTrack(const AudioBuffer& b, double ref) {
  std::vector<double> d(b.frames, 0.0), ups;
  for (size_t i = 2; i < b.frames; ++i) d[i] = b.at(0, i) - 2.0 * b.at(0, i - 1) + b.at(0, i - 2);
  for (size_t i = 3; i < b.frames; ++i)
    if (d[i - 1] < 0 && d[i] >= 0 && d[i] - d[i - 1] > 1e-5) ups.push_back((static_cast<double>(i - 1) + -d[i - 1] / (d[i] - d[i - 1])) / kSr);
  std::vector<std::pair<double, double>> out;
  for (size_t i = 1; i < ups.size(); ++i) {
    const double p = ups[i] - ups[i - 1];
    if (p > 0) out.push_back({0.5 * (ups[i] + ups[i - 1]), 12.0 * std::log2(1.0 / p / ref)});
  }
  return out;
}
// Mean of up to 32 cycles around `t` (one cycle of a differenced 16-bit sine is noisy).
double trackAt(const std::vector<std::pair<double, double>>& tr, double t) {
  double sum = 0;
  int n = 0;
  for (const auto& [x, v] : tr)
    if (std::fabs(x - t) < 0.002) sum += v, ++n;
  return n ? sum / n : std::nan("");
}
double goertzel(const AudioBuffer& x, double hz, double t0, double t1) {
  const size_t i0 = static_cast<size_t>(t0 * kSr), i1 = static_cast<size_t>(t1 * kSr);
  const double w = 2 * kPi * hz / kSr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = i0; i < i1 && i < x.frames; ++i) {
    const double s0 = x.at(0, i) + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(i1 - i0);
}

}  // namespace

int main() {
  // ------------------------------------------------ curve accuracy: every shape, measured
  {
    struct Row {
      const char* name;
      uint8_t curve;
    };
    const Row rows[] = {{"straight", bend::Straight}, {"smooth", bend::Smooth}, {"ease in", bend::EaseIn}, {"ease out", bend::EaseOut}, {"step", bend::Step}};
    const float tensions[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    const float amps[] = {0.5f, 1.23f, 2.0f, 7.0f, 12.0f, 24.0f, -7.3f};
    const Tick spans[] = {24, 48, 96, 192, 384, 768};
    std::printf("# 96 ticks per beat. Glide: worst cents at 2 beats and up, 1 beat, half a beat, quarter beat; mean; most slides\n");
    int most = 0;
    for (const Row& r : rows) {
      Err all;
      double byLen[4] = {0, 0, 0, 0}, usual = 0, usualShort = 0;
      int slides = 0;
      for (float tn : tensions)
        for (float amp : amps)
          for (Tick span : spans) {
            if (r.curve == bend::Step && tn != 0) continue;
            Song s = baseSong();
            const ObjectId ch = addChannel(s, "gl.sampler");
            std::vector<BendPoint> line;
            if (r.curve == bend::Step) line = {{static_cast<float>(span), amp, bend::Step}};
            else line = {{static_cast<float>(span), amp, r.curve, tn}};
            s.patterns[0].notes[ch] = {mk(0, span + 48, 60, line)};
            Song back;
            flp::ExportReport rep;
            if (!roundTrip(s, back, rep)) {
              expect(false, "round trip");
              continue;
            }
            const std::vector<Note> ns = firstNotes(back);
            const Err e = compareLines(s.patterns[0].notes[ch], ns);
            slides = std::max(slides, count(ns, true));
            all.sum += e.sum, all.n += e.n;
            const int bin = span >= 192 ? 0 : span == 96 ? 1 : span == 48 ? 2 : 3;
            byLen[bin] = std::max(byLen[bin], e.worst);
            if (std::fabs(tn) <= 0.5f && std::fabs(amp) <= 12) {  // curve amounts up to half, up to an octave
              if (span >= 96) usual = std::max(usual, e.worst);
              else if (span == 48) usualShort = std::max(usualShort, e.worst);
            }
          }
      std::printf("# %-9s %6.2f %6.2f %6.2f %6.2f  mean %5.2f  slides %d  (usual amounts: %.2f from a beat, %.2f at half)\n", r.name, byLen[0], byLen[1], byLen[2], byLen[3], all.mean(), slides, usual, usualShort);
      most = std::max(most, slides);
      // The bound. 5 cents is half of FL's fine pitch step; shorter and steeper than the file's ticks
      // can follow (one tick is the shortest ramp), the error is the ticks', not the planner's.
      expect(byLen[0] <= kBound, std::string(r.name) + ": every amount and range, glides of 2 beats and up: within 5 cents");
      expect(byLen[1] <= 5.7, std::string(r.name) + ": every amount and range, one beat: within 5.7 cents");
      expect(byLen[2] <= 12.6, std::string(r.name) + ": every amount and range, half a beat: within 12.6 cents (a sharp knee between two ticks)");
      expect(usual <= kBound, std::string(r.name) + ": usual amounts and ranges from a beat up: within 5 cents");
      expect(usualShort <= 5.85, std::string(r.name) + ": usual amounts and ranges, half a beat: within 5.85 cents");
      expect(all.mean() <= 2.5, std::string(r.name) + ": mean within 2.5 cents");
    }
    expect(most <= 32, "a single glide takes at most 32 slides");

    // At a finer project resolution the same holds for the shortest glides.
    Err fine;
    for (uint8_t c : {static_cast<uint8_t>(bend::Smooth), static_cast<uint8_t>(bend::EaseIn), static_cast<uint8_t>(bend::EaseOut), static_cast<uint8_t>(bend::Straight)})
      for (float tn : tensions)
        for (float amp : {2.0f, 12.0f}) {
          const std::vector<BendPoint> line = {{240, amp, c, tn}};
          const bend::SlidePlan plan = bend::toSlides(line, 480, 0.05, 0.1, 0.0);
          double v = plan.start;
          for (double t = 0; t < 480; t += 0.5) {
            double at = v;
            double prev = plan.start;
            for (const auto& pc : plan.pieces) {
              const double a = static_cast<double>(pc.tick), z = a + static_cast<double>(pc.length);
              if (t >= z) at = pc.target, prev = pc.target;
              else if (t > a) {
                at = prev + (pc.target - prev) * (t - a) / (z - a);
                break;
              } else {
                at = prev;
                break;
              }
            }
            fine.add(std::fabs(at - bend::value(line, t)) * 100.0);
          }
        }
    std::printf("# a quarter beat at 960 ticks per beat: worst %.2f cents, mean %.2f\n", fine.worst, fine.mean());
    expect(fine.worst <= kBound, "a quarter beat glide at 960 ticks per beat stays within 5 cents");
  }

  // ------------------------------------------------ the plan itself
  {
    const std::vector<BendPoint> sm = {{192, 7, bend::Smooth}};
    const bend::SlidePlan p = bend::toSlides(sm, 240, 0.05, 0.1, 0.0);
    bool onGrid = std::fabs(p.start * 10 - std::round(p.start * 10)) < 1e-9, ordered = true;
    int64_t at = 0;
    for (const auto& pc : p.pieces) {
      onGrid = onGrid && std::fabs(pc.target * 10 - std::round(pc.target * 10)) < 1e-9;
      ordered = ordered && pc.tick >= at && pc.length >= 1;
      at = pc.tick + pc.length;
    }
    expect(onGrid && ordered && !p.relaxed, "every slide lands on the 10 cent grid, in order, never overlapping");
    const bend::SlidePlan st = bend::toSlides({{48, 0}, {48, 5, bend::Step}}, 96, 0.05, 0.1, 0.0);
    expect(st.pieces.size() == 1 && st.pieces[0].tick == 48 && st.pieces[0].length == 1 && st.pieces[0].target == 5 && st.steps == 1,
           "a step is one slide of the format's shortest length, starting on its point");
    const bend::SlidePlan off = bend::toSlides({{48, 1.23f}}, 96, 0.05, 0.1, 0.0);
    expect(off.offGrid && off.pieces.size() == 1 && std::fabs(off.pieces[0].target - 1.2) < 1e-9, "a value between two fine pitch steps goes to the nearest, flagged");
    const bend::SlidePlan base = bend::toSlides({{48, 1.0f}}, 96, 0.05, 0.1, 0.03);
    expect(!base.pieces.empty() && std::fabs(base.pieces[0].target + 0.03 - 1.0) < 1e-9, "the grid is the pitch's, a note off the grid lands on it");
    // A busy line: the limit holds, and the tolerance widens only as far as needed.
    std::vector<BendPoint> busy;
    for (int i = 1; i <= bend::kMaxPoints; ++i) busy.push_back({static_cast<float>(i * 32), i % 2 ? 24.0f : -24.0f, bend::Smooth, 1.0f});
    const bend::SlidePlan b = bend::toSlides(busy, 32 * bend::kMaxPoints, 0.05, 0.1, 0.0);
    std::printf("# 32 sharp 48 semitone glides: %d slides, relaxed %d\n", static_cast<int>(b.pieces.size()), b.relaxed ? 1 : 0);
    expect(static_cast<int>(b.pieces.size()) <= bend::kMaxSlidesPerLine && b.relaxed, "a line asking for more than the limit stays at the limit and says so");
    // A very long note stays quick: the search doesn't walk every tick.
    const bend::SlidePlan lng = bend::toSlides({{4000000, 12, bend::Smooth}}, 4000000, 0.05, 0.1, 0.0);
    expect(!lng.pieces.empty() && static_cast<int>(lng.pieces.size()) <= bend::kMaxSlidesPerLine, "a glide of millions of ticks plans in few slides");
  }

  // ------------------------------------------------ a bent note over another note of its colour
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    const std::vector<BendPoint> up = {{96, 5, bend::Smooth}};
    s.patterns[0].notes[ch] = {mk(0, 192, 48), mk(0, 192, 60, up)};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep), "overlap: exports");
    const std::vector<Note> ns = firstNotes(back);
    int heldColour = -1, bentColour = -1;
    bool slidesFollow = true;
    for (const Note& n : ns)
      if (!n.slide) (n.pitch == 48 ? heldColour : bentColour) = n.color;
    for (const Note& n : ns)
      if (n.slide) slidesFollow = slidesFollow && n.color == bentColour;
    expect(rep.counts[kWritten] == 1 && rep.counts[kMoved] == 1 && rep.counts[kNoColour] == 0, "overlap: the line is written and the move counted");
    expect(heldColour == 0 && bentColour == 1 && slidesFollow && count(ns, true) >= 2, "overlap: the other note keeps its colour, the bent one and its slides take the first free one");
    const Err e = compareLines(s.patterns[0].notes[ch], ns);
    std::printf("# overlap: worst %.2f cents over both notes\n", e.worst);
    expect(e.worst <= kBound, "overlap: the bent note follows its line, the held note holds");

    for (uint8_t c : {static_cast<uint8_t>(bend::Straight), static_cast<uint8_t>(bend::EaseIn), static_cast<uint8_t>(bend::EaseOut), static_cast<uint8_t>(bend::Step)}) {
      Song z = baseSong();
      const ObjectId zc = addChannel(z, "gl.sampler");
      z.patterns[0].notes[zc] = {mk(0, 192, 48), mk(24, 144, 60, {{96, -7, c}})};
      Song zb;
      flp::ExportReport zr;
      expect(roundTrip(z, zb, zr) && compareLines(z.patterns[0].notes[zc], firstNotes(zb)).worst <= kBound && zr.counts[kMoved] == 1,
             "overlap: shape " + std::to_string(c) + " stays within 5 cents and leaves the other note alone");
    }

    // A note that started after the bent one but ended before its first slide still counts: slides
    // are measured from the last note started.
    Song r = baseSong();
    const ObjectId rc = addChannel(r, "gl.sampler");
    r.patterns[0].notes[rc] = {mk(0, 192, 60, {{96, 7}, {144, 0}}), mk(12, 12, 72)};
    r.patterns[0].notes[rc][0].bend = {{48, 0}, {96, 7}};
    Song rb;
    flp::ExportReport rr;
    expect(roundTrip(r, rb, rr) && rr.counts[kMoved] == 1 && compareLines(r.patterns[0].notes[rc], firstNotes(rb)).worst <= kBound,
           "a short note before the first slide moves the line too (slides count from the last note started)");

    // Overlap only where the line holds: nothing can glide, no colour has to change.
    Song h = baseSong();
    const ObjectId hc = addChannel(h, "gl.sampler");
    h.patterns[0].notes[hc] = {mk(0, 192, 60, {{48, 3}}), mk(96, 96, 67)};
    Song hb;
    flp::ExportReport hr;
    expect(roundTrip(h, hb, hr) && hr.counts[kWritten] == 1 && hr.counts[kMoved] == 0, "a note that starts after the last slide has landed stays in the colour");
    for (const Note& n : firstNotes(hb)) expect(n.color == 0, "colour kept where nothing glides");
    expect(compareLines(h.patterns[0].notes[hc], firstNotes(hb)).worst <= kBound, "and both notes still play as drawn");

    // Notes one after the other never conflict.
    Song a = baseSong();
    const ObjectId ac = addChannel(a, "gl.sampler");
    a.patterns[0].notes[ac] = {mk(0, 96, 60, {{96, 2}}), mk(96, 96, 62, {{96, -2}})};
    Song ab;
    flp::ExportReport ar;
    expect(roundTrip(a, ab, ar) && ar.counts[kMoved] == 0 && ar.counts[kWritten] == 2, "bent notes in a row keep their colour");
    expect(compareLines(a.patterns[0].notes[ac], firstNotes(ab)).worst <= kBound, "bent notes in a row play as drawn");
  }

  // ------------------------------------------------ chords
  {
    // Bending as one over a held note of the colour: the whole chord moves, one set of slides.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    const std::vector<BendPoint> up = {{96, 2}};
    s.patterns[0].notes[ch] = {mk(0, 192, 36), mk(0, 192, 60, up), mk(0, 192, 64, up), mk(0, 192, 67, up)};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kWritten] == 3 && rep.counts[kMoved] == 3, "chord over a held note: moved together, counted per note");
    const std::vector<Note> ns = firstNotes(back);
    expect(count(ns, true) == 1, "chord over a held note: one slide glides the whole chord");
    expect(compareLines(s.patterns[0].notes[ch], ns).worst <= kBound, "chord over a held note: every voice follows, the held note holds");

    // A chord whose notes bend differently: each line gets its colour.
    Song d = baseSong();
    const ObjectId dc = addChannel(d, "gl.sampler");
    d.patterns[0].notes[dc] = {mk(0, 192, 60, {{96, 2}}), mk(0, 192, 64, {{96, -1, bend::Smooth}}), mk(0, 192, 67, {{48, 3, bend::EaseIn}, {144, 1.5f, bend::EaseOut}})};
    Song db;
    flp::ExportReport dr;
    expect(roundTrip(d, db, dr) && dr.counts[kWritten] == 3 && dr.counts[kNoColour] == 0, "a chord with three lines: all written");
    const Err e = compareLines(d.patterns[0].notes[dc], firstNotes(db));
    std::printf("# three lines in a chord: worst %.2f cents\n", e.worst);
    expect(e.worst <= kBound, "a chord with three lines: each voice plays its own");
    std::map<int, int> colours;
    for (const Note& n : firstNotes(db))
      if (!n.slide) colours[n.color]++;
    expect(colours.size() == 3, "a chord with three lines: three colours");

    // Colours reused once free: lines that don't overlap share the moved colour.
    Song u = baseSong();
    const ObjectId uc = addChannel(u, "gl.sampler");
    u.patterns[0].notes[uc] = {mk(0, 384, 36), mk(0, 96, 60, {{96, 2}}), mk(96, 96, 62, {{96, 2}}), mk(192, 96, 64, {{96, 2}})};
    Song ub;
    flp::ExportReport ur;
    expect(roundTrip(u, ub, ur) && ur.counts[kMoved] == 3, "a run of bent notes over a pedal note: all moved");
    std::map<int, int> ucol;
    for (const Note& n : firstNotes(ub))
      if (!n.slide) ucol[n.color]++;
    expect(ucol.size() == 2, "a run of bent notes over a pedal note: one colour is enough");
    expect(compareLines(u.patterns[0].notes[uc], firstNotes(ub)).worst <= kBound, "a run of bent notes over a pedal note: plays as drawn");
  }

  // ------------------------------------------------ all colours used
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    std::vector<Note> ns;
    for (int c = 0; c < kNoteColors; ++c) ns.push_back(mk(0, 192, 40 + c, {}, c));
    ns.push_back(mk(0, 192, 72, {{96, 3}}, 0));
    s.patterns[0].notes[ch] = ns;
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kNoColour] == 1 && rep.counts[kWritten] == 0 && rep.counts[kMoved] == 0, "all 16 colours sounding: left out and counted");
    const std::vector<Note> b = firstNotes(back);
    expect(count(b, true) == 0 && count(b, false) == kNoteColors + 1, "all 16 colours sounding: no slide written");
    bool same = true;
    for (const Note& n : b)
      if (n.pitch < 72) same = same && n.color == n.pitch - 40;
    expect(same, "all 16 colours sounding: nothing changes colour");

    // One colour free: that one.
    Song f = baseSong();
    const ObjectId fc = addChannel(f, "gl.sampler");
    std::vector<Note> fs;
    for (int c = 0; c < kNoteColors; ++c)
      if (c != 11) fs.push_back(mk(0, 192, 40 + c, {}, c));
    fs.push_back(mk(0, 192, 72, {{96, 3}}, 0));
    f.patterns[0].notes[fc] = fs;
    Song fb;
    flp::ExportReport fr;
    expect(roundTrip(f, fb, fr) && fr.counts[kMoved] == 1, "one colour free: the line moves");
    for (const Note& n : firstNotes(fb))
      if (n.pitch >= 72 || n.slide) expect(n.color == 11, "one colour free: into it");
    expect(compareLines(fs, firstNotes(fb)).worst <= kBound, "one colour free: all 16 voices play as drawn");

    // More lines at once than colours: as many as fit, the rest counted.
    Song m = baseSong();
    const ObjectId mc = addChannel(m, "gl.sampler");
    std::vector<Note> ms;
    for (int k = 0; k < 18; ++k) ms.push_back(mk(0, 192, 40 + k, {{96, 1.0f + 0.1f * static_cast<float>(k)}}));
    m.patterns[0].notes[mc] = ms;
    Song mb;
    flp::ExportReport mr;
    expect(roundTrip(m, mb, mr) && mr.counts[kWritten] == 15 && mr.counts[kNoColour] == 3, "18 different lines at once: 15 written, 3 counted (the plain ones share the last colour)");
  }

  // ------------------------------------------------ colours with another meaning stay
  {
    for (const char* type : {"gl.matrixfm", "gl.slicer", "gl.midiout", "gl.patcher"}) {
      Song s = baseSong();
      const ObjectId ch = addChannel(s, type);
      s.patterns[0].notes[ch] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 5}})};
      Song back;
      flp::ExportReport rep;
      expect(roundTrip(s, back, rep) && rep.counts[kColourSelects] == 1 && rep.counts[kMoved] == 0, std::string(type) + ": a colour selects something there, the line is left out and counted");
      for (const Note& n : firstNotes(back)) expect(n.color == 0, std::string(type) + ": no colour changed");
      // ... but a bend with nothing to glide is still written.
      Song a = baseSong();
      const ObjectId ac = addChannel(a, type);
      a.patterns[0].notes[ac] = {mk(0, 192, 60, {{96, 5}})};
      Song ab;
      flp::ExportReport ar;
      expect(roundTrip(a, ab, ar) && ar.counts[kWritten] == 1, std::string(type) + ": a lone bent note converts as before");
    }
    // A hosted plugin gets the colour as the MIDI channel.
    {
      Song z = baseSong();
      const ObjectId zc = addChannel(z, "gl.sampler");
      z.channels.back().plugin.type = "vst3:00000000000000000000000000000000";
      z.patterns[0].notes[zc] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 5}})};
      std::vector<uint8_t> zb;
      flp::ExportReport zr;
      flp::ExportOptions zo;
      zo.renderInstruments = false;
      expect(flp::exportFlp(z, zb, zr, zo) && zr.counts[kColourSelects] == 1 && zr.counts[kMoved] == 0, "a hosted plugin: colours stay, the line is left out and counted");
    }
    // A note processor on the channel can read the colour.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    s.channels.back().noteFx.push_back(defaultPluginState("gl.colormap"));
    s.patterns[0].notes[ch] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 5}})};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kColourSelects] == 1, "a note processor on the channel: left out and counted");
    // Plasma reads colours as channels only in MPE mode.
    for (const bool mpe : {false, true}) {
      Song z = baseSong();
      const ObjectId zc = addChannel(z, "gl.plasma");
      PluginState& st = z.channels.back().plugin;
      if (static_cast<size_t>(packs::plasma::G_MpeOn) < st.params.size()) st.params[static_cast<size_t>(packs::plasma::G_MpeOn)] = mpe ? 1.0f : 0.0f;
      z.patterns[0].notes[zc] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 5}})};
      Song zb;
      flp::ExportReport zr;
      expect(roundTrip(z, zb, zr) && (mpe ? zr.counts[kColourSelects] == 1 : zr.counts[kMoved] == 1), mpe ? "Plasma in MPE mode: colours stay" : "Plasma without MPE: the line moves");
    }
  }

  // ------------------------------------------------ the user's own slide notes keep their sound
  {
    // A slide of the colour over the bent note takes it over in Gridline: moving the note would
    // take it away from that slide.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    s.patterns[0].notes[ch] = {mk(0, 192, 60, {{48, 2}}), mk(0, 192, 48), slideNote(96, 48, 62)};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kUnderSlide] == 1 && rep.counts[kMoved] == 0, "a slide note over the line: left out and counted");
    // A later slide counted from the bent note: moving it would change that slide's interval.
    Song l = baseSong();
    const ObjectId lc = addChannel(l, "gl.sampler");
    l.patterns[0].notes[lc] = {mk(0, 288, 48), mk(0, 96, 60, {{48, 2}}), slideNote(144, 48, 62)};
    Song lb;
    flp::ExportReport lr;
    expect(roundTrip(l, lb, lr) && lr.counts[kUnderSlide] == 1 && lr.counts[kMoved] == 0, "a later slide that counts from the bent note: left out and counted");
    // A free colour with a slide of its own is not free.
    Song f = baseSong();
    const ObjectId fc = addChannel(f, "gl.sampler");
    f.patterns[0].notes[fc] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 3}}), mk(500, 96, 50, {}, 1), slideNote(520, 48, 52, 1)};
    Song fb;
    flp::ExportReport fr;
    expect(roundTrip(f, fb, fr) && fr.counts[kMoved] == 1, "colour 1 holds a slide: the line takes another");
    for (const Note& n : firstNotes(fb))
      if (n.pitch >= 60 && !n.slide) expect(n.color == 2, "the next free colour instead");
    // Slide notes without lines pass through untouched.
    Song p = baseSong();
    const ObjectId pc = addChannel(p, "gl.sampler");
    p.patterns[0].notes[pc] = {mk(0, 192, 60), slideNote(48, 48, 64)};
    Song pb;
    flp::ExportReport pr;
    expect(roundTrip(p, pb, pr) && count(firstNotes(pb), true) == 1 && compareLines({mk(0, 192, 60)}, firstNotes(pb)).worst > 100, "a plain slide still slides");
  }

  // ------------------------------------------------ a step row sounds in colour 0
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    s.patterns[0].steps[ch] = std::vector<uint8_t>(16, 0);
    s.patterns[0].steps[ch][0] = 100;
    s.patterns[0].notes[ch] = {mk(0, 192, 60, {{96, 3}})};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kMoved] == 1, "a channel with active steps: the bent note leaves colour 0");
  }

  // ------------------------------------------------ the other patterns of the channel
  {
    // A colour used by this channel in another pattern is taken last (clips can overlap).
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    Pattern other = s.patterns[0];
    other.id = s.allocateId();
    other.notes[ch] = {mk(0, 96, 50, {}, 1)};
    s.patterns.push_back(other);
    s.patterns[0].notes[ch] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 3}})};
    Song back;
    flp::ExportReport rep;
    expect(roundTrip(s, back, rep) && rep.counts[kMoved] == 1, "two patterns: the line moves");
    bool ok = false;
    for (const auto& p : back.patterns)
      for (const auto& [c, ns] : p.notes)
        for (const Note& n : ns)
          if (n.pitch == 60 && !n.slide) ok = n.color == 2;
    expect(ok, "two patterns: the other pattern's colour is skipped");
  }

  // ------------------------------------------------ no bends: nothing changes, to the byte
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler");
    std::vector<Note> ns = {mk(0, 96, 60, {}, 0), mk(0, 192, 48, {}, 0), mk(48, 96, 64, {}, 3), mk(96, 96, 67, {}, 15)};
    ns[1].fine = 30, ns[2].pan = 20, ns[3].release = 90;
    s.patterns[0].notes[ch] = ns;
    Song back;
    flp::ExportReport rep;
    std::vector<uint8_t> bytes, again;
    expect(roundTrip(s, back, rep, &bytes), "no bends: exports");
    int bendKeys = 0;
    for (const auto& [k, v] : rep.counts) bendKeys += k.find("pitch bend") != std::string::npos;
    expect(bendKeys == 0, "no bends: the report says nothing about bends");
    // The records, as written before K-AB: same order, same bytes.
    int chIndex = -1;
    for (size_t i = 0; i < s.channels.size(); ++i)
      if (s.channels[i].id == ch) chIndex = static_cast<int>(i);
    std::vector<uint8_t> want;
    std::vector<Note> sorted = ns;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    for (const Note& n : sorted) {
      const auto r = flp::xp::noteRecord(n, chIndex, nullptr);
      want.insert(want.end(), r.begin(), r.end());
    }
    expect(std::search(bytes.begin(), bytes.end(), want.begin(), want.end()) != bytes.end(), "no bends: the note records are the ones written without the bend pass");
    flp::ExportReport rep2;
    flp::ExportOptions opt;
    opt.renderInstruments = false;
    expect(flp::exportFlp(s, again, rep2, opt) && again == bytes, "no bends: two exports are the same bytes");

    // A bent note on one channel leaves another channel's notes alone, and any export is repeatable.
    Song t = s;
    const ObjectId other = addChannel(t, "gl.sampler");
    t.patterns[0].notes[other] = {mk(0, 192, 60, {{96, 3}})};
    Song tb;
    flp::ExportReport tr;
    std::vector<uint8_t> tbytes, tagain;
    expect(roundTrip(t, tb, tr, &tbytes) && tr.counts[kMoved] == 0, "a bent note alone on its channel: not moved");
    flp::ExportReport tr2;
    expect(flp::exportFlp(t, tagain, tr2, opt) && tagain == tbytes, "bent notes: two exports are the same bytes");
    for (const auto& p : tb.patterns)
      for (const auto& [c, xs] : p.notes)
        if (xs.size() == ns.size()) {
          bool same = true;
          for (size_t i = 0; i < xs.size(); ++i) same = same && xs[i].color == sorted[i].color && xs[i].pitch == sorted[i].pitch;
          expect(same, "another channel's notes are untouched");
        }
    // Gridline's own project keeps its lines and colours: export reads, it doesn't edit.
    Song orig = baseSong();
    const ObjectId oc = addChannel(orig, "gl.sampler");
    orig.patterns[0].notes[oc] = {mk(0, 192, 48), mk(0, 192, 60, {{96, 3}})};
    const Song keep = orig;
    Song ob;
    flp::ExportReport or_;
    expect(roundTrip(orig, ob, or_) && orig.patterns[0].notes.at(oc) == keep.patterns[0].notes.at(oc), "the Gridline project itself is not changed by the export");
    // No slide limit report on ordinary lines.
    expect(or_.counts[kRelaxed] == 0, "an ordinary line stays under the limit");
  }

  // ------------------------------------------------ by ear: pitch tracks of real renders
  {
    const double f0 = 1760.0;
    const std::string wav = sineWav("a", f0);
    // Sampler root is C5 (key 60): the bent note plays around 1760 Hz, the held note four octaves
    // down at 110 Hz. Before the fix this line was left out; a slide in one colour glides both.
    for (uint8_t c : {static_cast<uint8_t>(bend::Straight), static_cast<uint8_t>(bend::Smooth), static_cast<uint8_t>(bend::EaseIn), static_cast<uint8_t>(bend::EaseOut)}) {
      Song s = baseSong();
      const ObjectId ch = addChannel(s, "gl.sampler", wav);
      const std::vector<BendPoint> line = {{192, 7, c}, {384, 2, c}};
      s.patterns[0].notes[ch] = {mk(0, 576, 12), mk(0, 576, 60, line)};
      Song back;
      flp::ExportReport rep;
      expect(roundTrip(s, back, rep) && rep.counts[kMoved] == 1, "render: the line moves colour");
      const AudioBuffer a = render(s), b = render(back);
      const auto ta = highTrack(a, f0), tb = highTrack(b, f0);
      Err e;
      for (double t = 0.05; t < 2.9; t += 0.01) {
        const double pa = trackAt(ta, t), pb = trackAt(tb, t);
        if (!std::isnan(pa) && !std::isnan(pb)) e.add(std::fabs(pa - pb) * 100.0);
      }
      const double lowA = goertzel(a, 110.0, 1.2, 2.8), lowB = goertzel(b, 110.0, 1.2, 2.8);
      std::printf("# render, shape %d: %d windows, bent note mean %.2f cents, worst %.2f; held note level %.4f vs %.4f\n", c, e.n, e.mean(), e.worst, lowA, lowB);
      expect(e.n > 200 && e.mean() < 4.0 && e.worst < 12.0, "render: the bent note's pitch track follows the line");
      expect(lowA > 0.01 && std::fabs(lowA - lowB) / lowA < 0.1, "render: the held note stays on its pitch");
    }
    std::error_code ec;
    std::filesystem::remove(wav, ec);
  }

  return finish();
}
