// Gridline: cut groups, mono and the voice limit (K-165). A channel's note cuts the channels
// "cut by" its group, mono ends the channel's other notes, and both reach every kind of note:
// held ones, ones already let go whose sound rings on (one-shot samples, release tails) and
// live notes (computer keyboard, MIDI, pads). Cuts are sample accurate with a short fade.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
std::string gDir;

// A 3 s sine at `hz` (sampler source).
std::string sineWav(const std::string& name, double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 3));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = gDir + "/cut_" + name + ".wav";
  writeWav(path, b, kSr, 16);
  return path;
}

// 120 BPM, one 4-bar pattern placed once: 96 ticks = 0.5 s.
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

Channel& addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return s.channels.back();
}

Note mk(Tick start, Tick len, int pitch) {
  Note n;
  n.start = start;
  n.length = len;
  n.pitch = pitch;
  n.velocity = 100;
  return n;
}

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

double rms(const AudioBuffer& b, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  double acc = 0;
  for (size_t i = a; i < e; ++i) acc += static_cast<double>(b.at(0, i)) * b.at(0, i);
  return e > a ? std::sqrt(acc / static_cast<double>(e - a)) : 0.0;
}

double freqAt(const AudioBuffer& b, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  int z = 0;
  for (size_t i = a + 1; i < e; ++i)
    if ((b.at(0, i - 1) < 0) != (b.at(0, i) < 0)) ++z;
  return z / 2.0 / (static_cast<double>(e - a) / kSr);
}

// The first frame where two renders differ (frames if none).
size_t firstDiff(const AudioBuffer& a, const AudioBuffer& b) {
  const size_t n = std::min(a.frames, b.frames);
  for (size_t i = 0; i < n; ++i)
    if (a.at(0, i) != b.at(0, i) || a.at(1, i) != b.at(1, i)) return i;
  return n;
}

double peak(const AudioBuffer& b, size_t from, size_t to) {
  double p = 0;
  for (size_t i = from; i < std::min(to, b.frames); ++i) p = std::max(p, static_cast<double>(std::fabs(b.at(0, i))));
  return p;
}

// ---- sequenced notes on the Sampler (pattern, piano roll, step sequencer, song render, exports)
void testSequencedSampler(const std::string& a441, const std::string& a882) {
  // A cuts B: B's long one-shot sample, started by a short step that ended long ago, stops when A plays.
  {
    Song s = baseSong();
    addChannel(s, "gl.sampler", a441);
    Channel& cutter = addChannel(s, "gl.sampler", a882);
    cutter.muted = true;  // only the victim is heard
    s.patterns[0].notes[s.channels[0].id] = {mk(0, 12, 60)};  // one short step
    s.patterns[0].notes[s.channels[1].id] = {mk(192, 12, 60)};  // 1.0 s
    const AudioBuffer free = render(s);
    s.channels[0].cutBy = 3;
    s.channels[1].cutGroup = 3;
    const AudioBuffer cut = render(s);
    std::printf("# step cut: free %.3f, cut before %.3f after %.5f\n", rms(free, 1.1, 1.5), rms(cut, 0.5, 0.95), rms(cut, 1.01, 1.5));
    expect(rms(free, 1.1, 1.5) > 0.2, "one-shot sample rings on after its short step");
    expect(rms(cut, 0.5, 0.95) > 0.2 && rms(cut, 1.01, 1.5) < 1e-4, "cut group: a step that ended still gets cut (one-shot sample)");
    // Sample accurate, with a short fade (no click): identical up to the cut sample, then a
    // few milliseconds of fade, then silence.
    const size_t at = static_cast<size_t>(kSr);  // 192 ticks at 120 BPM
    const size_t d = firstDiff(free, cut);
    std::printf("# cut starts at frame %zu (note at %zu)\n", d, at);
    expect(d >= at && d <= at + 1, "cut group: the cut starts on the cutting note's sample");
    expect(peak(cut, at, at + 22) > 0.2, "cut group: the first half millisecond still sounds (fade, not a click)");
    expect(peak(cut, at + 300, at + 2000) < 1e-4, "cut group: silent after the short fade");
    double step = 0;
    for (size_t i = at; i < at + 300; ++i) step = std::max(step, static_cast<double>(std::fabs(cut.at(0, i) - cut.at(0, i - 1))));
    expect(step < 0.1, "cut group: the fade has no jump larger than the signal's own slope");
    // Block size does not move the cut.
    expect(firstDiff(cut, render(s, 37)) == cut.frames || peak(render(s, 37), at + 300, at + 2000) < 1e-4,
           "cut group: the same cut at another block size");
  }
  // Held note is cut too (the case that already worked stays working).
  {
    Song s = baseSong();
    addChannel(s, "gl.sampler", a441);
    Channel& cutter = addChannel(s, "gl.sampler", a882);
    cutter.muted = true;
    s.patterns[0].notes[s.channels[0].id] = {mk(0, 384, 60)};
    s.patterns[0].notes[s.channels[1].id] = {mk(192, 12, 60)};
    s.channels[0].cutBy = 1, s.channels[1].cutGroup = 1;
    expect(rms(render(s), 1.01, 1.5) < 1e-4, "cut group: a held note is cut");
    s.channels[0].cutBy = 2;  // another group: no cut
    expect(rms(render(s), 1.1, 1.5) > 0.2, "cut group: a channel cut by another group plays on");
  }
  // The same channel cutting itself, with one-shot steps.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.cutGroup = c.cutBy = 1;
    s.patterns[0].notes[c.id] = {mk(0, 12, 60), mk(96, 12, 72)};
    const AudioBuffer b = render(s);
    std::printf("# self cut: %.1f Hz, rms %.3f\n", freqAt(b, 0.6, 0.9), rms(b, 0.6, 0.9));
    expect(std::fabs(freqAt(b, 0.6, 0.9) - 882) < 12 && rms(b, 0.6, 0.9) < 0.26, "cut itself: the second step ends the first one-shot");
    s.channels[0].cutGroup = s.channels[0].cutBy = 0;
    expect(rms(render(s), 0.6, 0.9) > 0.29, "without a cut group both one-shots ring together");
  }
  // Mono: a second note ends the first, also when the first was a short step that rings on.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.mono = true;
    s.patterns[0].notes[c.id] = {mk(0, 12, 60), mk(96, 12, 72)};
    const AudioBuffer b = render(s);
    expect(std::fabs(freqAt(b, 0.6, 0.9) - 882) < 12 && rms(b, 0.6, 0.9) < 0.26, "mono: a second step ends the ringing first one");
    // Chord on a mono channel: one note is left.
    s.patterns[0].notes[c.id] = {mk(0, 192, 60), mk(0, 192, 72)};
    expect(rms(render(s), 0.2, 0.4) < 0.26, "mono: a chord plays one note");
    // Mono with porta: legato glide (the held note glides to the new pitch).
    s.channels[0].porta = true, s.channels[0].portaTime = 0.4f;
    s.patterns[0].notes[c.id] = {mk(0, 96, 60), mk(96, 192, 72)};
    const AudioBuffer g = render(s);
    const double fmid = freqAt(g, 0.66, 0.74), fend = freqAt(g, 1.0, 1.3);
    expect(fmid > 560 && fmid < 760 && std::fabs(fend - 882) < 15, "mono + porta: legato notes glide over the porta time");
  }
  // Voice limit counts notes that ring on after their step.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.polyphony = 1;
    s.patterns[0].notes[c.id] = {mk(0, 12, 60), mk(96, 12, 72)};
    const AudioBuffer b = render(s);
    expect(std::fabs(freqAt(b, 0.6, 0.9) - 882) < 12 && rms(b, 0.6, 0.9) < 0.26, "voice limit 1: a second step ends the ringing first");
  }
  // The Sampler's own voice count of one still retriggers (instrument mono).
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.plugin.params[pp::SmpVoices] = 1;
    s.patterns[0].notes[c.id] = {mk(0, 12, 60), mk(96, 12, 72)};
    const AudioBuffer b = render(s);
    expect(std::fabs(freqAt(b, 0.6, 0.9) - 882) < 12 && rms(b, 0.6, 0.9) < 0.26, "sampler voices 1: one note at a time");
  }
}

// ---- live notes (computer keyboard, MIDI keyboard, pads, step and piano roll previews)
struct Live {
  ProgramBuilder builder;
  AudioEngine eng{kSr};
  std::vector<float> L, R;
  explicit Live(const Song& s) { eng.setProgram(builder.build(s)); }
  AudioBuffer run(double secs) {
    const int n = static_cast<int>(secs * kSr);
    L.assign(static_cast<size_t>(n), 0.f), R.assign(static_cast<size_t>(n), 0.f);
    for (int pos = 0; pos < n; pos += 256) eng.process(L.data() + pos, R.data() + pos, std::min(256, n - pos));
    AudioBuffer b(2, static_cast<size_t>(n));
    for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = L[i], b.at(1, i) = R[i];
    return b;
  }
};

void testLive(const std::string& a441, const std::string& a882) {
  // A cuts B, played live.
  {
    Song s = baseSong();
    addChannel(s, "gl.sampler", a441);
    Channel& cutter = addChannel(s, "gl.sampler", a882);
    cutter.muted = true;
    s.channels[0].cutBy = 4, s.channels[1].cutGroup = 4;
    Live lv(s);
    lv.eng.noteOn(s.channels[0].id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOff(s.channels[0].id, 60);  // let go: the one-shot rings on
    expect(rms(lv.run(0.3), 0.1, 0.3) > 0.2, "live: the one-shot rings on after the key is let go");
    lv.eng.noteOn(s.channels[1].id, 60, 0.8f);
    const AudioBuffer b = lv.run(0.3);
    expect(rms(b, 0.05, 0.3) < 1e-4, "live: cut group cuts a let-go one-shot");
    lv.eng.noteOff(s.channels[1].id, 60);
    lv.eng.noteOn(s.channels[0].id, 62, 0.8f);  // held this time
    lv.run(0.1);
    lv.eng.noteOn(s.channels[1].id, 64, 0.8f);
    expect(rms(lv.run(0.3), 0.05, 0.3) < 1e-4, "live: cut group cuts a held live note");
  }
  // Self cut, live.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.cutGroup = c.cutBy = 2;
    Live lv(s);
    lv.eng.noteOn(c.id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOn(c.id, 72, 0.8f);
    const AudioBuffer b = lv.run(0.4);
    expect(std::fabs(freqAt(b, 0.1, 0.4) - 882) < 12 && rms(b, 0.1, 0.4) < 0.26, "live: cut itself, the new key ends the old");
  }
  // Mono, live: a second key ends the first; porta glides.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.mono = true;
    Live lv(s);
    lv.eng.noteOn(c.id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOn(c.id, 72, 0.8f);
    const AudioBuffer b = lv.run(0.4);
    expect(std::fabs(freqAt(b, 0.1, 0.4) - 882) < 12 && rms(b, 0.1, 0.4) < 0.26, "live: mono, the new key ends the held one");
    lv.eng.noteOff(c.id, 72);
    lv.eng.noteOff(c.id, 60);
  }
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.mono = true, c.porta = true, c.portaTime = 0.4f;
    c.plugin.params[pp::SmpOneShot] = 0;
    Live lv(s);
    lv.eng.noteOn(c.id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOn(c.id, 72, 0.8f);
    const AudioBuffer b = lv.run(0.8);
    const double fmid = freqAt(b, 0.16, 0.24), fend = freqAt(b, 0.5, 0.8);
    std::printf("# live porta: mid %.1f end %.1f\n", fmid, fend);
    expect(fmid > 560 && fmid < 760 && std::fabs(fend - 882) < 15, "live: mono + porta glides to the new key");
  }
  // Voice limit, live.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    c.polyphony = 1;
    Live lv(s);
    lv.eng.noteOn(c.id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOn(c.id, 72, 0.8f);
    const AudioBuffer b = lv.run(0.4);
    expect(std::fabs(freqAt(b, 0.1, 0.4) - 882) < 12 && rms(b, 0.1, 0.4) < 0.26, "live: voice limit 1 keeps the newest key");
  }
  // Without any setting, live keys still layer.
  {
    Song s = baseSong();
    Channel& c = addChannel(s, "gl.sampler", a441);
    Live lv(s);
    lv.eng.noteOn(c.id, 60, 0.8f);
    lv.run(0.1);
    lv.eng.noteOn(c.id, 72, 0.8f);
    expect(rms(lv.run(0.4), 0.1, 0.4) > 0.29, "live: without mono or cuts both keys sound");
  }
}

// ---- every built-in instrument: a cut is never slower than a note-off at that moment, and an
// instrument that rings on after its note-off (one-shot drums and samples) stops within 50 ms.
void testEveryInstrument(const std::string& a441) {
  int checked = 0, oneShots = 0;
  std::string bad;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.kind != PluginKind::Generator || isHostedType(info.type)) continue;
    if (const char* only = std::getenv("GL_CUT_TYPE"); only && info.type != only) continue;
    for (int held = 1; held >= 0; --held) {
      Song s = baseSong();
      addChannel(s, info.type, info.usesSample ? a441 : "");
      Channel& cutter = addChannel(s, "gl.sampler", a441);
      cutter.muted = true;
      const ObjectId vid = s.channels[0].id;
      s.patterns[0].notes[s.channels[1].id] = {mk(192, 12, 60)};
      s.patterns[0].notes[vid] = {mk(0, 192, 60)};  // ends where the cut comes
      const AudioBuffer released = render(s);
      s.patterns[0].notes[vid] = {mk(0, held ? 384 : 24, 60)};
      const AudioBuffer free = render(s);
      s.channels[0].cutBy = 1, s.channels[1].cutGroup = 1;
      const AudioBuffer cut = render(s);
      const double f = rms(free, 1.05, 1.5), c = rms(cut, 1.05, 1.5), r = rms(released, 1.05, 1.5);
      if (f < 1e-3) continue;  // silent there anyway
      ++checked;
      // A note-off does not stop it. Not the record decks (a note-off lets the platter run down,
      // a cut does the same) nor the bank player (its reverb and delay tail rings on).
      const bool deck = info.type == "gl.turntable" || info.type == "gl.splinescratch" || info.type == "gl.flex";
      const bool oneShot = held && r > 0.5 * f && !deck;
      oneShots += oneShot;
      const bool ok = c <= r * 1.1 + 1e-4 && (!oneShot || c < f * 0.05);
      std::printf("# %-22s %-6s free %.4f released %.4f cut %.5f %s%s\n", info.type.c_str(), held ? "held" : "let go", f, r, c,
                  oneShot ? "one-shot " : "", ok ? "" : "<- rings on");
      if (!ok) bad += " " + info.type + (held ? "(held)" : "(let go)");
    }
  }
  std::printf("# instruments checked: %d (%d ring on after a note-off); not stopped by a cut:%s\n", checked, oneShots,
              bad.empty() ? " none" : bad.c_str());
  expect(checked > 20 && oneShots >= 3, "every instrument: many instruments checked, one-shot ones among them");
  expect(bad.empty(), "every instrument: a cut is never slower than a note-off, one-shots stop within 50 ms");
}

// ---- settings: save / load and undo
void testSettings() {
  Document d;
  const ObjectId c = d.addChannel("Sampler", "gl.sampler");
  d.editChannel(c, "cut", [](Channel& ch) { ch.cutGroup = 5, ch.cutBy = 7, ch.mono = true; });
  Song back;
  expect(songFromJson(songToJson(d.song()), back) && back.channel(c) && back.channel(c)->cutGroup == 5 && back.channel(c)->cutBy == 7 &&
             back.channel(c)->mono,
         "settings: cut, cut by and mono save and load");
  d.undo();
  expect(d.song().channel(c) && d.song().channel(c)->cutGroup == 0 && d.song().channel(c)->cutBy == 0 && !d.song().channel(c)->mono,
         "settings: undo restores them");
  d.redo();
  expect(d.song().channel(c)->cutGroup == 5 && d.song().channel(c)->mono, "settings: redo");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("cutgroups");
  const std::string a441 = sineWav("a441", 441), a882 = sineWav("a882", 882);
  testSequencedSampler(a441, a882);
  testLive(a441, a882);
  testSettings();
  testEveryInstrument(a441);
  return finish();
}
