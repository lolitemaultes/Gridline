// Gridline - K-25: per-note pitch bend lines (the piano roll's Pitch bend tool).
// The line's shape (straight, smooth, step, hold), project save / load and old projects,
// undo, copies, note edits that carry or cut the line (move, resize, split, chop, glue, PPQ),
// playback (the rendered frequency follows the line on a built-in instrument, per voice, for
// any block size; MIDI Out gets channel pitch bend), MIDI file export / import, FL export.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/Json.h"
#include "core/MenuOps.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/NoteBend.h"
#include "core/NoteTools.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

std::string sineWav(const std::string& name, double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = "test_k25_" + name + ".wav";
  writeWav(path, b, kSr, 16);
  return path;
}

// 120 BPM (one beat = 96 ticks = 0.5 s), one 2-bar pattern placed once.
Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 2;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}

ObjectId addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = type;
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

// Frequency from zero crossings of the left channel in [t0, t1) seconds.
double freqAt(const AudioBuffer& b, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  int z = 0;
  for (size_t i = a + 1; i < e; ++i)
    if ((b.at(0, i - 1) < 0) != (b.at(0, i) < 0)) ++z;
  return z / 2.0 / (static_cast<double>(e - a) / kSr);
}

// Goertzel magnitude of `hz` in [t0, t1) (normalised to a full-scale sine = 0.5).
double level(const AudioBuffer& b, double hz, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  const double w = 2 * 3.14159265358979323846 * hz / kSr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = a; i < e; ++i) {
    const double s0 = b.at(0, i) + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(e - a);
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  // ------------------------------------------------ the line's shape
  {
    const std::vector<BendPoint> ramp = {{48, 2, bend::Straight}};
    expect(near(bend::value(ramp, 0), 0, 1e-9) && near(bend::value(ramp, 24), 1, 1e-9) && near(bend::value(ramp, 48), 2, 1e-9) &&
               near(bend::value(ramp, 500), 2, 1e-9),
           "a line starts at 0 on the note start, ramps to its point and holds the last value");
    const std::vector<BendPoint> smooth = {{0, -1, 0}, {40, 3, bend::Smooth}};
    expect(near(bend::value(smooth, 0), -1, 1e-9) && near(bend::value(smooth, 20), 1, 1e-6) && bend::value(smooth, 5) < -0.8,
           "a point at tick 0 sets the start value; a smooth segment eases in and out");
    const std::vector<BendPoint> step = {{30, 5, bend::Step}};
    expect(near(bend::value(step, 29.9), 0, 1e-9) && near(bend::value(step, 30), 5, 1e-9), "a step segment jumps at its point");
    std::vector<BendPoint> messy = {{50, 1, 0}, {10, 0.5f, 7}, {50, 1, 0}, {-3, 99, 0}};
    bend::normalize(messy);
    expect(messy.size() == 3 && messy[0].tick == 0 && messy[0].semis == bend::kMaxSemis && messy[1].tick == 10 && messy[1].curve == 0 &&
               messy[2].tick == 50,
           "normalize sorts, clamps, drops duplicates and unknown curves");
    std::vector<BendPoint> zero = {{10, 0, 0}, {20, 0, 1}};
    bend::normalize(zero);
    expect(zero.empty(), "a line that never leaves 0 is no line");
  }

  // ------------------------------------------------ project save / load, old projects, undo, copies
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.synth");
    s.patterns[0].notes[ch] = {mk(0, 96, 60, {{24, 2, 0}, {72, -1.5f, bend::Smooth}, {120, 7, bend::Step}}), mk(96, 96, 64)};
    Json j;
    expect(jsonParse(jsonStringify(songToJson(s)), j), "a project with bend lines saves");
    Song back;
    expect(songFromJson(j, back) && back == s, "bend lines (points past the note's end too) survive save and load");
    Song plain = baseSong();
    const ObjectId p = addChannel(plain, "gl.synth");
    plain.patterns[0].notes[p] = {mk(0, 96, 60)};
    const std::string text = jsonStringify(songToJson(plain));
    expect(text.find("\"bend\"") == std::string::npos, "notes without a bend write no new key (older builds read the same file)");
    Json jp;
    Song pb;
    expect(jsonParse(text, jp) && songFromJson(jp, pb) && pb == plain && pb.patterns[0].notes[p][0].bend.empty(),
           "projects without bend lines load with none");

    Document d(s);
    const ObjectId pat = d.song().patterns[0].id;
    std::vector<Note> ns = d.song().pattern(pat)->notes.at(ch);
    ns[1].bend = {{48, 12, 0}};
    d.setNotes(pat, ch, ns, "bend notes", 77);
    ns[1].bend = {{48, 5, 0}};
    d.setNotes(pat, ch, ns, "bend notes", 77);  // the same drag: one step
    expect(d.song().pattern(pat)->notes.at(ch)[1].bend == std::vector<BendPoint>{{48, 5, 0}}, "the drag's last state is kept");
    d.undo();
    expect(d.song().pattern(pat)->notes.at(ch)[1].bend.empty(), "one undo takes the whole bend drag back");
    d.redo();
    expect(d.song().pattern(pat)->notes.at(ch)[1].bend.size() == 1, "redo brings it back");

    std::vector<Note> cp = {mk(0, 96, 60, {{24, 2, 0}})};
    std::vector<bool> sel = {true};
    tools::cloneNotes(cp, sel);
    expect(cp.size() == 2 && cp[1].bend == cp[0].bend, "copies (Shift+drag, copy / paste) carry the line");
  }

  // ------------------------------------------------ note edits
  {
    const Note n = mk(0, 96, 60, {{48, 4, 0}, {96, 0, 0}});
    // Move / transpose / resize keep the line in ticks from the note start.
    Note moved = n;
    moved.start += 200, moved.pitch += 5, moved.length = 192;
    expect(moved.bend == n.bend && near(bend::value(moved.bend, 48), 4, 1e-9), "move, transpose and resize keep the line's timing");
    // Split (slice tool).
    Note l = n, r = n;
    l.length = 24, r.start = 24, r.length = 72;
    bend::split(n, 24, l, r);
    expect(near(bend::value(l.bend, 24), 2, 1e-6) && near(bend::value(r.bend, 0), 2, 1e-6) && near(bend::value(r.bend, 24), 4, 1e-6) &&
               near(bend::value(r.bend, 72), 0, 1e-6),
           "a split note's parts play their own pieces of the line");
    // Chop (ops::chop and the chop tool).
    std::vector<Note> ch = {n};
    std::vector<bool> sel = {true};
    ops::chop(ch, sel, 48);
    expect(ch.size() == 2 && near(bend::value(ch[0].bend, 47), 47.0 / 12.0, 1e-3) && near(bend::value(ch[1].bend, 0), 4, 1e-6) &&
               near(bend::value(ch[1].bend, 24), 2, 1e-6),
           "chopped pieces follow the original line");
    std::vector<Note> ch2 = {n};
    std::vector<bool> sel2 = {true};
    tools::chopEvery(ch2, sel2, 24);
    expect(ch2.size() == 4 && near(bend::value(ch2[2].bend, 0), 4, 1e-6), "the chop tool keeps the line too");
    // Glue: the second note's line continues where it starts.
    std::vector<Note> gl2 = {mk(0, 48, 60, {{24, 2, 0}}), mk(48, 48, 60, {{24, -3, 0}})};
    std::vector<bool> gs = {true, true};
    ops::glue(gl2, gs);
    expect(gl2.size() == 1 && gl2[0].length == 96 && near(bend::value(gl2[0].bend, 40), 2, 1e-6) && near(bend::value(gl2[0].bend, 48), 0, 1e-6) &&
               near(bend::value(gl2[0].bend, 72), -3, 1e-6),
           "glued notes play both lines in turn");
    // PPQ change rescales the points.
    Song s = baseSong();
    const ObjectId c = addChannel(s, "gl.synth");
    s.patterns[0].notes[c] = {n};
    changePpq(s, 192);
    expect(near(s.patterns[0].notes[c][0].bend[0].tick, 96, 1e-4), "a PPQ change moves the points with the notes");
  }

  // ------------------------------------------------ playback: the rendered pitch follows the line
  const std::string a441 = sineWav("a441", 441.0);
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", a441);  // C5 (60) plays the sample at 441 Hz
    s.patterns[0].notes[ch] = {mk(0, 288, 60, {{96, 12, bend::Straight}})};  // up an octave over the first beat (0.5 s)
    const AudioBuffer b = render(s);
    const double f0 = freqAt(b, 0.0, 0.04), fm = freqAt(b, 0.23, 0.27), fe = freqAt(b, 0.7, 1.2);
    std::printf("# bend: %.1f Hz, %.1f Hz half way, %.1f Hz held\n", f0, fm, fe);
    expect(near(f0, 452, 20) && near(fm, 441 * std::pow(2.0, 0.5), 25) && near(fe, 882, 10), "a built-in instrument's pitch follows the line");
    const AudioBuffer b64 = render(s, 64), b1024 = render(s, 1024);
    expect(near(freqAt(b64, 0.23, 0.27), fm, 6) && near(freqAt(b1024, 0.23, 0.27), fm, 12) && near(freqAt(b64, 0.7, 1.2), fe, 2),
           "the same line at block sizes 64, 512 and 1024");
    // A step down a fifth at beat 2, a start point a tone up.
    s.patterns[0].notes[ch] = {mk(0, 288, 60, {{0, 2, 0}, {96, 2, 0}, {96, -5, bend::Step}})};
    const AudioBuffer bs = render(s);
    const double s0 = freqAt(bs, 0.05, 0.45), s1 = freqAt(bs, 0.6, 1.2);
    std::printf("# step: %.1f Hz then %.1f Hz\n", s0, s1);
    expect(near(s0, 441 * std::pow(2.0, 2 / 12.0), 8) && near(s1, 441 * std::pow(2.0, -5 / 12.0), 8), "a start point and a step play as drawn");
    // Fine pitch adds to the line.
    s.patterns[0].notes[ch] = {mk(0, 288, 60, {{0, 12, 0}})};
    s.patterns[0].notes[ch][0].fine = 100;
    expect(near(freqAt(render(s), 0.2, 1.0), 441 * std::pow(2.0, 13 / 12.0), 12), "fine pitch adds to the bend");
  }
  {
    // Overlapping notes bend on their own voices: a bent note over a held one leaves it alone.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", a441);
    s.patterns[0].notes[ch] = {mk(0, 288, 48), mk(0, 288, 60, {{48, 12, 0}})};
    const AudioBuffer b = render(s);
    const double low = level(b, 220.5, 0.5, 1.2), up = level(b, 882, 0.5, 1.2), orig = level(b, 441, 0.5, 1.2);
    std::printf("# overlap: 220.5 Hz %.4f, 882 Hz %.4f, 441 Hz %.4f\n", low, up, orig);
    expect(low > 0.05 && up > 0.05 && orig < 0.02, "each overlapping note plays its own line (per voice)");
    // A slide note takes over a bent note's pitch.
    Note slide = mk(192, 48, 72);
    slide.slide = true;
    s.patterns[0].notes[ch] = {mk(0, 384, 60, {{48, -12, 0}}), slide};
    const AudioBuffer sb = render(s);
    expect(near(freqAt(sb, 0.3, 0.9), 220.5, 6) && near(freqAt(sb, 1.4, 1.9), 882, 12), "a slide glides a bent note from the line on");
  }

  // ------------------------------------------------ MIDI Out: channel pitch bend
  {
    Document d(makeEmptySong());
    const ObjectId c = d.addChannel("out", "gl.midiout");
    const ObjectId pat = d.song().patterns[0].id;
    d.setNotes(pat, c, {mk(0, 96, 64, {{48, 1, 0}})}, "notes", 0);
    ProgramBuilder pb;
    AudioEngine eng(44100);
    eng.setProgram(pb.build(d.song()));
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(pat);
    eng.play();
    std::vector<float> L(512), R(512);
    for (int k = 0; k < 120; ++k) eng.process(L.data(), R.data(), 512);
    auto rt = pb.channelRuntime(c);
    MidiOutRing::Msg m{};
    int bends = 0, maxV = 0, lastV = -1;
    bool on = false;
    while (rt && rt->midiOut.pop(m)) {
      if (m.s == 0x90) on = true;
      if ((m.s & 0xF0) == 0xE0) {
        const int v = m.a | (m.b << 7);
        ++bends, maxV = std::max(maxV, v), lastV = v;
      }
    }
    std::printf("# midi out: %d bend messages, max %d, last %d\n", bends, maxV, lastV);
    expect(on && bends > 4 && near(maxV, 8192 + 4096, 40) && lastV == 8192, "MIDI Out sends the line as channel pitch bend, centred at the end");
  }

  // ------------------------------------------------ MIDI files
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.synth");
    s.patterns[0].notes[ch] = {mk(0, 96, 60, {{48, 5, 0}}), mk(192, 96, 62, {{0, -2, 0}, {48, 1, bend::Smooth}})};
    MidiFileData m = patternToMidi(s, s.patterns[0].id);
    bool rpn = false;
    int bendEvents = 0;
    for (const auto& tr : m.tracks)
      for (const auto& c : tr.ctls) {
        rpn = rpn || (c.controller == 6 && c.value == 5);
        bendEvents += c.controller == kMidiPitchBend;
      }
    expect(rpn && bendEvents > 10, "MIDI export writes the lines as pitch bend with the range (RPN 0) they need");
    MidiFileData back;
    expect(parseMidi(encodeMidi(m), back), "the exported file reads back");
    Song imp = makeEmptySong();
    MidiImportResult r = importMidiIntoSong(imp, back, "bend", {});
    const Pattern* ip = imp.pattern(r.pattern);
    const std::vector<Note>* ns = ip && !r.channels.empty() && ip->notes.count(r.channels[0]) ? &ip->notes.at(r.channels[0]) : nullptr;
    bool same = ns && ns->size() == 2;
    for (size_t i = 0; same && i < 2; ++i)
      for (double t : {0.0, 12.0, 24.0, 47.0, 60.0, 90.0}) {
        const double want = bend::value(s.patterns[0].notes[ch][i].bend, t), got = bend::value((*ns)[i].bend, t);
        same = same && near(got, want, 0.2);
      }
    std::printf("# import: %zu notes, skipped %d\n", ns ? ns->size() : size_t(0), r.skippedControllers);
    expect(same && r.skippedControllers == 0, "MIDI import turns pitch bend during notes back into their lines");
    // Dropped into an existing pattern (piano roll MIDI drop).
    Song drop = baseSong();
    const ObjectId dc = addChannel(drop, "gl.synth");
    expect(insertMidiNotes(drop, drop.patterns[0].id, dc, 0, back) == 2 && !drop.patterns[0].notes[dc][0].bend.empty(),
           "notes dropped from a MIDI file keep their bends");
  }

  // ------------------------------------------------ FL export: slide notes (K-28)
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.synth");
    s.patterns[0].notes[ch] = {mk(0, 96, 60, {{48, 5, 0}}), mk(96, 96, 62)};
    std::vector<uint8_t> out;
    flp::ExportReport rep;
    flp::ExportOptions opt;
    expect(flp::exportFlp(s, out, rep, opt) && rep.counts["notes: pitch bend lines written as slide notes"] == 1,
           "FL export writes the line as slide notes (K-28, test_k28)");
  }
  std::remove(a441.c_str());
  return finish();
}
