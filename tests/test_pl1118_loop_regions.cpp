// Gridline — PL-1118: the Smooth layout's region Loop (its inspector's switch, the region menu and
// the L key: engine/ClipOps.h loopClipsToNext). On, the selected regions loop and run on in repeats
// to the next region on their track (none after: the song's end, one repeat at least), pattern and
// audio regions alike, several at once; the repeats play where they are drawn (rendered against the
// passes laid out as plain clips, and the audio's clicks heard at each repeat). Off, the repeats go.
// loop2-1118: the looped end is on the grid: a song end between bars is taken down to the bar (up when
// that leaves no whole repeat), the Project End marker counts where placed, a next region that starts
// off the grid cuts the last repeat there.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/ClipLoop.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/ClipOps.h"
#include "engine/PlaylistTools.h"
#include "engine/Render.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.threads = 1;
  o.tail = s.barTicks();
  return renderSong(s, o);
}
bool sameAudio(const AudioBuffer& a, const AudioBuffer& b) { return a.frames == b.frames && a.frames > 0 && a.data == b.data; }
double peak(const AudioBuffer& b, size_t f0, size_t f1) {
  double p = 0;
  for (size_t i = f0; i < std::min(f1, b.frames); ++i) {
    p = std::max({p, std::fabs(static_cast<double>(b.at(0, i))), std::fabs(static_cast<double>(b.at(1, i)))});
  }
  return p;
}
size_t frameAt(const Song& s, Tick t) { return static_cast<size_t>(std::llround(static_cast<double>(t) * 60.0 / (s.tempo * s.ppq) * kSr)); }

// A synth channel, a 1-bar pattern of four notes, and a sampler channel playing a 1 s file with one
// click at its start.
struct Fx {
  Song s;
  ObjectId ch = kNoId, pat = kNoId, smp = kNoId;
  explicit Fx(const std::string& wav) {
    s = makeEmptySong();
    s.tempo = 120;
    Channel c;
    c.id = s.allocateId();
    c.name = "Synth";
    c.plugin = defaultPluginState("gl.synth");
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    ch = c.id;
    Channel a;
    a.id = s.allocateId();
    a.name = "Click";
    a.plugin = defaultPluginState("gl.sampler");
    a.plugin.data = wav;
    a.mixerTrack = s.masterId();
    a.audioClip = true;
    s.channels.push_back(a);
    smp = a.id;
    Pattern p;
    p.id = s.allocateId();
    p.name = "Riff";
    p.bars = 1;
    const Tick b = s.beatTicks();
    for (int k = 0; k < 4; ++k) {
      p.notes[ch].push_back(Note(k * b, b / 2, 60 + 3 * k, 100));
    }
    s.patterns.push_back(p);
    pat = p.id;
  }
  ObjectId patternClip(int track, Tick start, Tick len = 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = pat;
    c.track = track;
    c.start = start;
    c.length = len;
    s.playlist.push_back(c);
    return c.id;
  }
  ObjectId audioClip(int track, Tick start) {
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = smp;
    k.track = track;
    k.start = start;
    k.length = audioClipNaturalLength(s, k);
    s.playlist.push_back(k);
    return k.id;
  }
};

std::string clickFile(double seconds = 1.0, const char* name = "gl_pl1118_click.wav") {
  const size_t n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < static_cast<size_t>(0.01 * kSr); ++i) {
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.8 * std::sin(2 * kPi * 1500 * static_cast<double>(i) / kSr));
  }
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

void testPatternToNext(const std::string& wav) {
  Fx f(wav);
  const Tick L = f.s.barTicks();
  const ObjectId a = f.patternClip(0, 0);
  f.patternClip(0, 3 * L);  // the next region on the track
  f.patternClip(1, 0, 6 * L);  // a longer one on another track (the song's end)
  expect(loopClipsToNext(f.s, {a}, true), "pattern: Loop on changes the region");
  const Clip* c = f.s.clip(a);
  expect(c && c->loop && f.s.clipLength(*c) == 3 * L, "pattern: it loops and runs on to the next region on its track (3 bars)");
  expect(c && clipPasses(f.s, *c).size() == 3, "pattern: ... three passes of its bar");
  // It plays its passes (the same render as the passes laid out as plain clips).
  Fx g(wav);
  for (int k = 0; k < 3; ++k) {
    g.patternClip(0, k * L, L);
  }
  g.patternClip(0, 3 * L);
  g.patternClip(1, 0, 6 * L);
  expect(sameAudio(render(f.s), render(g.s)), "pattern: the looped region plays its repeats, sample-exact");
  expect(!loopClipsToNext(f.s, {a}, true), "pattern: Loop on again changes nothing");
  expect(loopClipsToNext(f.s, {a}, false), "pattern: Loop off changes the region");
  c = f.s.clip(a);
  expect(c && !c->loop && f.s.clipLength(*c) == L, "pattern: Loop off: back to one pass, not looping");
}

void testAudioToEnd(const std::string& wav) {
  Fx f(wav);
  const Tick L = f.s.barTicks();  // 2 s at 120 bpm
  const ObjectId au = f.audioClip(2, L);
  const ObjectId pa = f.patternClip(0, 0);
  f.patternClip(1, 0, 4 * L);  // the song runs to bar 5
  TempoMap tm;
  tm.build(f.s);
  const Tick nat = f.s.clipLength(*f.s.clip(au));
  expect(nat > 0 && nat < L, "audio: the file is shorter than a bar");
  expect(loopClipsToNext(f.s, {au, pa}, true), "several: Loop on for an audio and a pattern region at once");
  const Clip* c = f.s.clip(au);
  expect(c && c->loop && c->start + f.s.clipLength(*c) == 4 * L, "audio: with no region after it, it runs on to the song's end");
  const Clip* p = f.s.clip(pa);
  expect(p && p->loop && f.s.clipLength(*p) == 4 * L, "pattern: ... and the pattern region beside it too");
  tm.build(f.s);
  const std::vector<Clip> passes = audioLoopPasses(f.s, *c, tm);
  expect(passes.size() >= 3 && passes[1].start == c->start + nat, "audio: its passes repeat the file");
  // The click is heard at the start of every repeat, and silence between.
  const AudioBuffer r = render(f.s);
  Fx q(wav);  // the audio region alone, for its clicks
  q.s.playlist.push_back(*c);
  const AudioBuffer ra = render(q.s);
  int heard = 0;
  for (const Clip& ps : passes) {
    const size_t at = frameAt(q.s, ps.start);
    heard += peak(ra, at, at + static_cast<size_t>(0.012 * kSr)) > 0.3 ? 1 : 0;
  }
  expect(heard == static_cast<int>(passes.size()), std::string("audio: the click plays at each repeat (" + std::to_string(heard) + " of " + std::to_string(passes.size()) + ")").c_str());
  const size_t mid = frameAt(q.s, passes[0].start + nat / 2);
  expect(peak(ra, mid, mid + 2000) < 1e-3, "audio: ... and nothing between");
  expect(r.frames >= ra.frames, "render: the whole song renders");
  // A short region at the very end: one repeat at least.
  Fx e(wav);
  const ObjectId last = e.patternClip(0, 0);
  expect(loopClipsToNext(e.s, {last}, true) && e.s.clipLength(*e.s.clip(last)) == 2 * L, "pattern: alone in the song, it repeats once");
  expect(loopClipsToNext(f.s, {au, pa}, false), "several: Loop off");
  expect(f.s.clipLength(*f.s.clip(au)) == nat && !f.s.clip(au)->loop && f.s.clipLength(*f.s.clip(pa)) == L, "several: ... back to one pass each");
}

// loop2-1118: where the Loop switch ends a region.
void testEndsOnGrid(const std::string& wav) {
  {
    Fx f(wav);
    const Tick L = f.s.barTicks();
    const ObjectId a = f.patternClip(0, 0);
    f.patternClip(1, 0, 4 * L + L / 2);  // the song ends half way through bar 5
    expect(loopClipsToNext(f.s, {a}, true), "grid: Loop on");
    const Clip* c = f.s.clip(a);
    expect(c && f.s.clipLength(*c) == 4 * L, "grid: a song end between bars: down to the bar (4 whole repeats, not 4.5)");
    expect(c && clipPasses(f.s, *c).size() == 4 && clipPasses(f.s, *c).back().len == L, "grid: ... every pass whole");
  }
  {
    Fx f(wav);
    const Tick L = f.s.barTicks();
    const ObjectId a = f.patternClip(0, 0);
    f.patternClip(0, 3 * L + L / 2);  // the next region starts off the bar
    expect(loopClipsToNext(f.s, {a}, true), "grid: Loop on before a region off the bar");
    const Clip* c = f.s.clip(a);
    expect(c && f.s.clipLength(*c) == 3 * L + L / 2, "grid: ... the last repeat is cut where the next region starts");
    const std::vector<ClipPass> ps = c ? clipPasses(f.s, *c) : std::vector<ClipPass>{};
    expect(ps.size() == 4 && ps.back().at == 3 * L && ps.back().len == L / 2, "grid: ... a cut fourth pass of half a bar");
  }
  {
    Fx f(wav);
    const Tick L = f.s.barTicks();
    const ObjectId a = f.patternClip(0, 0);
    f.patternClip(1, 0, 2 * L);
    f.s.projectEnd = 6 * L;  // the Project End marker, placed
    expect(loopClipsToNext(f.s, {a}, true) && f.s.clipLength(*f.s.clip(a)) == 6 * L, "grid: the Project End marker where placed (bar 7)");
  }
  {
    const std::string w3 = clickFile(0.75, "gl_loop2_click075.wav");  // 0.375 of a bar at 120 bpm
    Fx f(w3);
    const Tick L = f.s.barTicks();
    const ObjectId au = f.audioClip(0, 0);
    const Tick nat = f.s.clipLength(*f.s.clip(au));
    expect(nat > 0 && nat < L / 2, "grid: an audio region shorter than half a bar");
    expect(loopClipsToNext(f.s, {au}, true), "grid: Loop on, the audio region alone");
    const Clip* c = f.s.clip(au);
    expect(c && f.s.clipLength(*c) == L, "grid: ... it runs on to the bar after its first repeat, not to 2 x its odd length");
    std::error_code ec;
    fs::remove(w3, ec);
  }
}

}  // namespace

int main() {
  test::hermeticTestEnv("pl1118");
  const std::string wav = clickFile();
  testPatternToNext(wav);
  testAudioToEnd(wav);
  testEndsOnGrid(wav);
  std::error_code ec;
  fs::remove(wav, ec);
  return finish();
}
