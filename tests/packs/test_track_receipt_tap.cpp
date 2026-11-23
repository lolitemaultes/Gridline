// Gridline - K-321: the sample tap's core for the Track Receipt. The tap itself (a pass bound on
// a thread, the output clock, the segment log and its extension, play seconds, jumps, pitch, the
// cue-block flag, ranges past the limit cut back to the exact reads, two passes at once) and,
// through the measurement, what those change: short reads that are not a cue block count, the
// tail follows the reads in the fast and the reference pass alike (and says when it stopped at
// 10 s), a hibernating channel is measured, and both passes give the same play seconds, jumps,
// pitch and segments.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Song.h"
#include "../../src/core/TrackReceipt.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/SampleTap.h"
#include "../../src/engine/TrackReceiptPass.h"
#include "../Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;
constexpr double kSr = 44100.0;

bool near(double v, double want, double tol) { return std::fabs(v - want) <= tol; }

Sample fakeSample(const std::string& path, double seconds) {
  Sample s;
  s.path = path;
  s.sampleRate = kSr;
  s.left.assign(static_cast<size_t>(seconds * kSr), 0.f);
  s.setMono();
  return s;
}

const tap::Use* useOf(const std::vector<tap::Use>& uses, ObjectId owner) {
  for (const tap::Use& u : uses)
    if (u.owner == owner) return &u;
  return nullptr;
}

// One block of a voice reading `frames` output frames from source frame `from` at `speed`.
void block(const Sample& s, int64_t outFrame, double from, double speed, int frames) {
  tap::Run run(&s);
  for (int i = 0; i < frames; ++i) run.at(from + speed * i);
  (void)outFrame;
}

void testTapCore() {
  const Sample a = fakeSample("/k321/a.wav", 10.0);
  tap::Pass::Options po;
  po.segments = true;
  tap::Pass pass({tap::WatchFile{a.path, a.seconds()}}, po);
  {
    const tap::Bind bind(&pass);
    // 1. A voice at constant speed over four blocks: one segment, extended block after block.
    for (int b = 0; b < 4; ++b) {
      const tap::OwnerScope own(1, 0.0, 512 * b, kSr);
      tap::play(tap::slotFor(&a));
      block(a, 512 * b, 512.0 * b, 1.0, 512);
    }
    // 2. Backwards, at half speed, from frame 200000, three blocks.
    for (int b = 0; b < 3; ++b) {
      const tap::OwnerScope own(2, 0.0, 10000 + 512 * b, kSr);
      block(a, 0, 200000.0 - 0.5 * 512 * b, -0.5, 512);
    }
    // 3. A block that reads 100 frames, then jumps (a cue block), and plays on from 100000.
    {
      const tap::OwnerScope own(3, 0.0, 0, kSr);
      tap::Run run(&a);
      for (int i = 0; i < 100; ++i) run.at(50000.0 + i);
      for (int i = 0; i < 412; ++i) run.at(100000.0 + i);
    }
    // 4. A cue block that touches another read loses its flag.
    {
      const tap::OwnerScope own(4, 0.0, 0, kSr);
      {
        tap::Run run(&a);
        for (int i = 0; i < 100; ++i) run.at(150000.0 + i);
        for (int i = 0; i < 10; ++i) run.at(300000.0 + i);
      }
      tap::Run plain(&a);
      for (int i = 0; i < 300; ++i) plain.at(149950.0 + i);
    }
    // 5. A stutter: 20 reads of 200 frames (4.5 ms), far apart, none ending in a jump.
    for (int k = 0; k < 20; ++k) {
      const tap::OwnerScope own(5, 0.0, 4096 * k, kSr);
      block(a, 0, 4410.0 * 2 * k, 1.0, 200);
    }
    // 6. More ranges than a pair keeps: 150 reads of 2 ms, 10 ms apart.
    for (int k = 0; k < 150; ++k) {
      const tap::OwnerScope own(6, 0.0, 0, kSr);
      block(a, 0, 0.010 * kSr * k, 1.0, 88);
    }
    // 7. Code that knows the block's output frames (audio clips): forwards and reversed.
    {
      const tap::OwnerScope own(7, 0.0, 44100, kSr);
      tap::Slot* slot = tap::slotFor(&a);
      tap::mark(slot, 1.0, 1.5, 0, 22050);
      tap::mark(slot, 1.5, 2.0, 22050, 44100);  // continues: one segment
      tap::mark(slot, 6.0, 6.5, 44100, 66150, true);
    }
    // 8. Pitch reports.
    {
      const tap::OwnerScope own(8, 0.0, 0, kSr);
      tap::pitch(&a, -3.0, 0.9);
      tap::pitch(&a, 5.0, 0.0);
      block(a, 0, 0.0, 1.0, 64);
    }
    // 9. An instrument that renders its block in pieces: the run of the second piece starts at its offset.
    {
      const tap::OwnerScope own(9, 0.0, 1000, kSr);
      tap::blockOffset(100);
      block(a, 0, 0.0, 1.0, 50);
      tap::blockOffset(0);
    }
    // 10. No output clock (an owner scope without one): ranges, but no play time or segments.
    {
      const tap::OwnerScope own(10, 0.0);
      block(a, 0, 0.0, 1.0, 512);
    }
    // 11. Two voices of one owner, interleaved block after block: still one segment each.
    for (int b = 0; b < 4; ++b) {
      const tap::OwnerScope own(11, 0.0, 512 * b, kSr);
      block(a, 0, 512.0 * b, 1.0, 512);
      block(a, 0, 300000.0 + 2.0 * 512 * b, 2.0, 512);
    }
  }
  const std::vector<tap::Use> uses = pass.uses();
  {
    const tap::Use* u = useOf(uses, 1);
    expect(u && u->segments.size() == 1, "tap: a voice at constant speed is one segment over its blocks");
    if (u && !u->segments.empty()) {
      const tap::Segment& g = u->segments[0];
      expect(near(g.outA, 0, 1e-12) && near(g.outB, 2048 / kSr, 1e-9) && near(g.srcA, 0, 1e-12) && near(g.srcB, 2048 / kSr, 1e-9),
             "tap: the segment maps output 0..2048 frames onto source 0..2048");
    }
    expect(u && near(u->outSeconds, 2048 / kSr, 1e-12), "tap: play seconds are the output frames it read in");
    expect(u && u->plays == 4 && u->jumps == 0, "tap: plays counted, no jump");
    expect(u && u->ranges.size() == 1 && !u->ranges[0].cue, "tap: one range, not a cue block");
  }
  {
    const tap::Use* u = useOf(uses, 2);
    expect(u && u->segments.size() == 1, "tap: backwards at half speed is one segment too");
    if (u && !u->segments.empty()) {
      const tap::Segment& g = u->segments[0];
      expect(g.srcB < g.srcA && near(g.srcA - g.srcB, 0.5 * 1536 / kSr, 1e-9) && near(g.outA, 10000 / kSr, 1e-9) &&
                 near(g.outB - g.outA, 1536 / kSr, 1e-9),
             "tap: read backwards: srcB below srcA, half the output time in source time");
    }
  }
  {
    const tap::Use* u = useOf(uses, 3);
    expect(u && u->jumps == 1 && u->ranges.size() == 2, "tap: a jump inside a run counts and starts a new range");
    expect(u && u->ranges.size() == 2 && u->ranges[0].cue && !u->ranges[1].cue, "tap: the run's first range, ended by the jump, is a cue block");
    expect(u && u->segments.size() == 2 && near(u->segments[1].outA, 100 / kSr, 1e-9), "tap: the jump starts a segment at its output frame");
    expect(u && near(u->outSeconds, 512 / kSr, 1e-12), "tap: play seconds include both parts");
  }
  {
    const tap::Use* u = useOf(uses, 4);
    bool touched = false, lone = false;
    if (u)
      for (const tap::Range& r : u->ranges) {
        if (r.a < 150000 / kSr && r.b > 150099 / kSr) touched = !r.cue;
        if (r.a >= 290000 / kSr) lone = true;
      }
    expect(touched, "tap: a cue block that another read touches is no longer a cue block");
    expect(lone, "tap: the read after the jump is kept");
  }
  {
    const tap::Use* u = useOf(uses, 5);
    bool anyCue = false;
    if (u)
      for (const tap::Range& r : u->ranges) anyCue = anyCue || r.cue;
    expect(u && u->ranges.size() == 20 && !anyCue, "tap: 20 short reads far apart: 20 ranges, none a cue block");
    expect(u && near(u->outSeconds, 20 * 200 / kSr, 1e-12), "tap: the stutter plays 20 x 200 frames");
  }
  {
    const tap::Use* u = useOf(uses, 6);
    double sum = 0;
    if (u)
      for (const tap::Range& r : u->ranges) sum += r.b - r.a;
    const double read = 150 * 88 / kSr;  // (each read: frames lo..hi, 88 of them)
    expect(u && u->overflow, "tap: 150 ranges overflow the pair");
    expect(u && sum <= read + 150 * 2 * tap::Pass::kGrain + 1e-9 && sum >= read - 1e-9,
           "tap: overflowed ranges are cut back to the exact reads (no gap counts)");
    std::printf("# overflow: %.4f s read, %.4f s counted\n", read, sum);
  }
  {
    const tap::Use* u = useOf(uses, 7);
    expect(u && u->segments.size() == 2, "tap: marks with output frames: a continuing mark extends the segment");
    if (u && u->segments.size() == 2) {
      const tap::Segment& g = u->segments[0];
      const tap::Segment& r = u->segments[1];
      expect(near(g.outA, 1.0, 1e-12) && near(g.outB, 2.0, 1e-12) && near(g.srcA, 1.0, 1e-12) && near(g.srcB, 2.0, 1e-12),
             "tap: output 1..2 s read source 1..2 s");
      expect(near(r.srcA, 6.5, 1e-12) && near(r.srcB, 6.0, 1e-12), "tap: a reversed mark reads from its end down");
    }
    expect(u && near(u->outSeconds, 1.5, 1e-12), "tap: marks count their output frames as play seconds");
  }
  {
    const tap::Use* u = useOf(uses, 8);
    expect(u && u->pitched == 2 && u->semisMin == -3.0 && u->semisMax == 5.0 && u->ratioMin == 0.9 && u->ratioMax == 0.9,
           "tap: pitch reports keep their range (a ratio of 0 is not known)");
  }
  {
    const tap::Use* u = useOf(uses, 9);
    expect(u && u->segments.size() == 1 && near(u->segments[0].outA, 1100 / kSr, 1e-12), "tap: a piece of a block counts from its offset");
  }
  {
    const tap::Use* u = useOf(uses, 10);
    expect(u && u->ranges.size() == 1 && u->outSeconds == 0 && u->segments.empty(), "tap: without an output clock only the ranges");
  }
  {
    const tap::Use* u = useOf(uses, 11);
    expect(u && u->segments.size() == 2, "tap: two voices of one owner interleaved: one segment each");
  }
  expect(!pass.segmentsFull(), "tap: the log had room");
  // The log's capacity.
  tap::Pass::Options small;
  small.segments = true;
  small.segmentCapacity = 2;
  tap::Pass tiny({tap::WatchFile{a.path, a.seconds()}}, small);
  {
    const tap::Bind bind(&tiny);
    for (int k = 0; k < 3; ++k) {
      const tap::OwnerScope own(1, 0.0, 100000 * k, kSr);
      block(a, 0, 50000.0 * k, 1.0, 64);
    }
  }
  const std::vector<tap::Use> tu = tiny.uses();
  expect(tiny.segmentsFull() && tu.size() == 1 && tu[0].segments.size() == 2 && tu[0].ranges.size() == 3,
         "tap: a full log keeps what fitted and says so; the ranges stay complete");
  // A pass without segments logs none.
  tap::Pass plain({tap::WatchFile{a.path, a.seconds()}});
  {
    const tap::Bind bind(&plain);
    const tap::OwnerScope own(1, 0.0, 0, kSr);
    block(a, 0, 0.0, 1.0, 64);
  }
  expect(plain.uses().size() == 1 && plain.uses()[0].segments.empty() && plain.uses()[0].outSeconds > 0,
         "tap: without the log: play seconds, no segments");
  // Nothing bound: nothing is read.
  {
    const tap::OwnerScope own(1, 0.0, 0, kSr);
    block(a, 0, 0.0, 1.0, 64);
    tap::pitch(&a, 1, 1);
  }
  expect(plain.uses().size() == 1 && plain.uses()[0].pitched == 0, "tap: with no pass bound nothing is recorded");
  // Two passes at once, on two threads: each gets its own reads only.
  tap::Pass p1({tap::WatchFile{a.path, a.seconds()}}, po), p2({tap::WatchFile{a.path, a.seconds()}}, po);
  auto work = [&a](tap::Pass* p, double from) {
    const tap::Bind bind(p);
    for (int b = 0; b < 200; ++b) {
      const tap::OwnerScope own(1, 0.0, 512 * b, kSr);
      block(a, 0, from + 512.0 * b, 1.0, 512);
    }
  };
  std::thread t1(work, &p1, 0.0), t2(work, &p2, 200000.0);
  t1.join();
  t2.join();
  const std::vector<tap::Use> u1 = p1.uses(), u2 = p2.uses();
  expect(u1.size() == 1 && u2.size() == 1 && u1[0].ranges.size() == 1 && u2[0].ranges.size() == 1 && u1[0].ranges[0].a == 0 &&
             near(u2[0].ranges[0].a, 200000 / kSr, 1e-9) && u1[0].segments.size() == 1 && u2[0].segments.size() == 1,
         "tap: two passes at once on two threads keep their own reads");
}

// ---- through the measurement

std::string writeFile(const std::string& dir, const std::string& name, double seconds) {
  fs::create_directories(dir);
  const auto n = static_cast<size_t>(seconds * kRate);
  AudioBuffer b(2, n);
  uint32_t seed = 777;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.1;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.35 * std::sin(2 * 3.141592653589793 * (110.0 + 20.0 * t) * t) + noise);
  }
  const std::string path = (fs::path(dir) / name).generic_string();
  writeWav(path, b, kRate, 16);
  return path;
}
std::string storeName(const std::string& stem) { return "Tap Test-Core Song-" + stem + "--120.00bpm-Am-[Clearance Required].wav"; }

Tick tps(const Song& s) { return static_cast<Tick>(std::llround(static_cast<double>(s.beatTicks()) * s.tempo / 60.0)); }
Tick at(const Song& s, double sec) { return static_cast<Tick>(std::llround(sec * static_cast<double>(tps(s)))); }

ObjectId addChannel(Song& s, const std::string& type, const std::string& data, const std::string& name) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState(type);
  ch.plugin.data = data;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}
float& param(Song& s, ObjectId c, int k) { return s.channel(c)->plugin.params[static_cast<size_t>(k)]; }
ObjectId audioChannel(Song& s, const std::string& path, const std::string& name) {
  const ObjectId id = addChannel(s, "gl.sampler", path, name);
  s.channel(id)->audioClip = true;
  return id;
}
ObjectId addClip(Song& s, ObjectId ch, double a, double b, double startSec, int track) {
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = ch, k.track = track;
  k.start = at(s, startSec);
  k.length = std::max<Tick>(1, at(s, b - a));
  k.audioStart = a, k.audioEnd = b;
  s.playlist.push_back(k);
  return k.id;
}
void addNotes(Song& s, ObjectId ch, const std::vector<std::tuple<double, double, int>>& notes, int track, int bars = 4) {
  Pattern p;
  p.id = s.allocateId();
  p.bars = bars;
  for (const auto& [st, len, pitch] : notes) {
    Note n;
    n.start = at(s, st);
    n.length = std::max<Tick>(1, at(s, len));
    n.pitch = pitch;
    p.notes[ch].push_back(n);
  }
  s.patterns.push_back(p);
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Pattern, k.pattern = p.id, k.track = track;
  k.start = 0, k.length = static_cast<Tick>(bars) * s.barTicks();
  s.playlist.push_back(k);
}
Song base() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  return s;
}

struct Both {
  receipt::Measurement fast, ref;
  std::vector<tap::Use> fastTap, refTap;
};
Both measureBoth(const Song& s) {
  Both b;
  receipt::PassOptions o;
  o.sampleRate = kRate;
  o.tapUses = &b.fastTap;
  b.fast = receipt::measure(s, o);
  o.reference = true;
  o.tapUses = &b.refTap;
  b.ref = receipt::measure(s, o);
  return b;
}
double usedOf(const receipt::Measurement& m) {
  receipt::Ranges all;
  for (const receipt::Use& u : m.uses)
    if (!u.muted)
      for (const receipt::Range& r : u.ranges) all.push_back(r);
  return receipt::total(receipt::unite(all));
}
double endOf(const receipt::Measurement& m) {
  double e = 0;
  for (const receipt::Use& u : m.uses)
    for (const receipt::Range& r : u.ranges) e = std::max(e, r.b);
  return e;
}
bool sameRanges(const receipt::Measurement& a, const receipt::Measurement& b) {
  if (a.uses.size() != b.uses.size()) return false;
  for (size_t i = 0; i < a.uses.size(); ++i)
    if (!(a.uses[i].ranges == b.uses[i].ranges) || a.uses[i].plays != b.uses[i].plays) return false;
  return a.warnings == b.warnings;
}
// Play seconds, jumps, pitch and segments of every pair, exactly.
bool sameTap(const std::vector<tap::Use>& a, const std::vector<tap::Use>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const tap::Use &x = a[i], &y = b[i];
    if (x.owner != y.owner || x.outSeconds != y.outSeconds || x.jumps != y.jumps || x.pitched != y.pitched || x.semisMin != y.semisMin ||
        x.semisMax != y.semisMax || x.ratioMin != y.ratioMin || x.ratioMax != y.ratioMax || x.segments.size() != y.segments.size())
      return false;
    for (size_t k = 0; k < x.segments.size(); ++k) {
      const tap::Segment &g = x.segments[k], &h = y.segments[k];
      if (g.outA != h.outA || g.outB != h.outB || g.srcA != h.srcA || g.srcB != h.srcB) return false;
    }
  }
  return true;
}

void testShortReads(const std::string& dir) {
  // A stutter edit: 20 one-tick audio clips (about 5 ms each), far apart in the file. Each is
  // shorter than 10 ms and none is a cue block: all of them count.
  Song s = base();
  const ObjectId c = audioChannel(s, writeFile(dir, storeName("Stutter"), 10.0), "stutter");
  const double tick = 1.0 / static_cast<double>(tps(s));
  for (int k = 0; k < 20; ++k) addClip(s, c, 0.4 * k, 0.4 * k + tick, 0.5 * k, 0);
  const Both b = measureBoth(s);
  const double used = usedOf(b.fast);
  std::printf("# stutter: %.4f s used (20 x %.4f s)\n", used, tick);
  expect(near(used, 20 * tick, 0.002), "receipt: 20 stutter reads under 10 ms each count (20 x one tick)");
  expect(sameRanges(b.fast, b.ref) && sameTap(b.fastTap, b.refTap), "receipt: the stutter: fast == reference, play time and segments too");
  const receipt::Report r = receipt::build(b.fast, {}, "t");
  expect(!r.songs.empty() && r.songs[0].seconds > 0.05, "receipt: the report counts them (0.1 s)");
}

void testTail(const std::string& dir) {
  const std::string file = writeFile(dir, storeName("Tail"), 4.0), longFile = writeFile(dir, storeName("Long"), 30.0);
  {  // (b) an inaudible one-shot read past the song end: both passes count it to the same end
    Song s = base();
    const ObjectId c = addChannel(s, "gl.sampler", file, "quiet");
    s.channel(c)->gain = 1e-6f;
    addNotes(s, c, {{7.5, 0.25, 60}}, 0);  // the song ends at 8 s; the one-shot reads 4 s
    const Both b = measureBoth(s);
    std::printf("# tail (b): fast to %.3f s, reference to %.3f s\n", endOf(b.fast), endOf(b.ref));
    expect(near(endOf(b.fast), 4.0, 0.01) && sameRanges(b.fast, b.ref), "tail: an inaudible one-shot past the end is read to the same end in both passes");
    expect(sameTap(b.fastTap, b.refTap), "tail: and the same play time and segments");
  }
  {  // (a) a voice that starts reading after a silent gap in the tail (a delayed note)
    Song s = base();
    const ObjectId lead = audioChannel(s, file, "lead");
    addClip(s, lead, 0.0, 2.0, 6.0, 0);  // reads to the song end (8 s)
    const ObjectId d = addChannel(s, "gl.sampler", file, "delayed");
    param(s, d, pp::SmpEnvDelay) = 0.5f;
    addNotes(s, d, {{7.8, 0.1, 60}}, 1);  // starts reading at 8.3 s, 0.3 s after the last read
    const Both b = measureBoth(s);
    double delayedRead = 0;
    for (const receipt::Use& u : b.ref.uses)
      if (u.owner == d)
        for (const receipt::Range& r : u.ranges) delayedRead += r.b - r.a;
    expect(delayedRead > 1.0 && sameRanges(b.fast, b.ref), "tail: a delayed voice that starts in the tail is counted in both passes");
  }
  {  // the 10 s cap: still reading 10 s after the end: counted to there, and said
    Song s = base();
    const ObjectId c = addChannel(s, "gl.sampler", longFile, "long");
    addNotes(s, c, {{7.0, 0.25, 60}}, 0);
    const Both b = measureBoth(s);
    bool said = false;
    for (const std::string& w : b.fast.warnings) said = said || w.find("10 s after the song end") != std::string::npos;
    std::printf("# tail cap: read to %.3f s\n", endOf(b.fast));
    expect(said && sameRanges(b.fast, b.ref), "tail: a sound still reading at the 10 s cap is cut there in both passes, with a warning");
    expect(endOf(b.fast) > 10.5 && endOf(b.fast) < 11.5, "tail: it reads 1 s of song and 10 s of tail");
  }
}

void testHibernating(const std::string& dir) {
  Song s = base();
  const ObjectId c = addChannel(s, "gl.sampler", writeFile(dir, storeName("Sleep"), 4.0), "sleeper");
  param(s, c, pp::SmpOneShot) = 0;
  addNotes(s, c, {{0.0, 1.0, 60}, {2.0, 0.5, 64}}, 0);
  receipt::PassOptions o;
  o.sampleRate = kRate;
  const receipt::Measurement awake = receipt::measure(s, o);
  s.channel(c)->hibernate = true;
  const receipt::Measurement asleep = receipt::measure(s, o);
  expect(usedOf(awake) > 1.0 && sameRanges(awake, asleep), "receipt: a hibernating channel is measured as when it is awake");
}

// No automation the fast pass takes out: both passes run the same blocks, so play time, jumps,
// pitch and segments are the same to the last bit; and the figures are the ones expected.
void testFigures(const std::string& dir) {
  Song s = base();
  const std::string f = writeFile(dir, storeName("Figures"), 10.0);
  const ObjectId loop = audioChannel(s, f, "loop"), rev = audioChannel(s, f, "rev"), slow = audioChannel(s, f, "slow");
  for (int k = 0; k < 8; ++k) addClip(s, loop, 1.0, 3.0, 2.0 * k, 0);  // a 2 s loop played 8 times
  ObjectId r = addClip(s, rev, 4.0, 6.0, 1.0, 1);
  s.clip(r)->reverse = true;
  {
    Clip& k = *s.clip(addClip(s, slow, 7.0, 8.0, 4.0, 2));  // stretched x2: 1 s of source over 2 s
    k.stretch = 2.0;
    k.length *= 2;
  }
  const ObjectId smp = addChannel(s, "gl.sampler", f, "sampler");
  param(s, smp, pp::SmpOneShot) = 0;
  addNotes(s, smp, {{0.0, 0.5, 72}, {1.0, 0.5, 60}}, 3);  // an octave up, then at the root
  const ObjectId sl = addChannel(s, "gl.slicer", writeFile(dir, storeName("Slices"), 4.0), "slicer");
  addNotes(s, sl, {{0.0, 0.25, 60}, {0.5, 0.25, 62}, {1.0, 0.25, 61}}, 4);
  const Both b = measureBoth(s);
  expect(sameRanges(b.fast, b.ref), "figures: fast == reference (ranges)");
  expect(sameTap(b.fastTap, b.refTap), "figures: fast == reference (play seconds, jumps, pitch, segments)");
  double loopPlay = 0, revPlay = 0;
  size_t loopSegs = 0;
  bool revBack = false, slowHalf = false, octave = false;
  for (const tap::Use& u : b.fastTap) {
    const Clip* k = s.clip(u.owner);
    if (k && k->source == loop) loopPlay += u.outSeconds, loopSegs += u.segments.size();
    if (k && k->source == rev) {
      revPlay += u.outSeconds;
      revBack = !u.segments.empty() && u.segments[0].srcB < u.segments[0].srcA;
    }
    if (k && k->source == slow && u.segments.size() == 1) {
      const tap::Segment& g = u.segments[0];
      slowHalf = near((g.srcB - g.srcA) / (g.outB - g.outA), 0.5, 0.01) && near(u.ratioMin, 0.5, 0.01);
    }
    if (u.owner == smp) octave = u.pitched == 2 && near(u.semisMax, 12.0, 1e-9) && near(u.semisMin, 0.0, 1e-9);
  }
  std::printf("# figures: loop plays %.3f s in %zu segments, reversed plays %.3f s\n", loopPlay, loopSegs, revPlay);
  expect(near(loopPlay, 16.0, 0.01) && loopSegs == 8, "figures: a 2 s loop played 8 times plays 16 s, one segment per pass");
  expect(near(revPlay, 2.0, 0.01) && revBack, "figures: a reversed clip reads backwards");
  expect(slowHalf, "figures: a clip at half speed reads 0.5 s of source per second");
  expect(octave, "figures: the sampler reports its notes' pitch (+12 and 0 semitones)");
}

}  // namespace

int main() {
  testTapCore();
  const std::string dir = (fs::temp_directory_path() / "gl_track_receipt_tap").generic_string();
  fs::remove_all(dir);
  testShortReads(dir + "/short");
  testTail(dir + "/tail");
  testHibernating(dir + "/hibernate");
  testFigures(dir + "/figures");
  fs::remove_all(dir);
  return finish();
}
