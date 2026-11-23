// Gridline: K-262, note vibrato in playback: a sampler note with vibrato swings its pitch by the
// depth at the rate (zero-crossing frequency), the same for any block size; a note with a bend line
// and vibrato plays both; the fade-in grows the swing; a synced rate follows the tempo; MIDI Out
// (no per-note pitch) gets the vibrato as channel pitch bend.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/NoteBend.h"
#include "core/NoteVibrato.h"
#include "core/PluginParams.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

std::string sineWav(double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = "test_k262_sine.wav";
  writeWav(path, b, kSr, 16);
  return path;
}

Song baseSong(double bpm = 120) {
  Song s = makeEmptySong();
  s.tempo = bpm;
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

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

// The frequency over time: one value per `win` seconds from zero-crossing periods.
std::vector<double> track(const AudioBuffer& b, double t0, double t1, double win) {
  std::vector<double> out;
  for (double t = t0; t + win <= t1; t += win) {
    const size_t a = static_cast<size_t>(t * kSr), e = std::min(b.frames, static_cast<size_t>((t + win) * kSr));
    // the first and last upward crossings, interpolated
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = a + 1; i < e; ++i) {
      const double y0 = b.at(0, i - 1), y1 = b.at(0, i);
      if (y0 < 0 && y1 >= 0) {
        const double x = static_cast<double>(i - 1) + (-y0) / (y1 - y0);
        if (first < 0) first = x;
        else ++n;
        last = x;
      }
    }
    out.push_back(n > 0 ? n / ((last - first) / kSr) : 0.0);
  }
  return out;
}

double maxOf(const std::vector<double>& v) {
  double m = 0;
  for (double x : v) m = std::max(m, x);
  return m;
}
double minOf(const std::vector<double>& v) {
  double m = 1e9;
  for (double x : v) m = std::min(m, x);
  return m;
}
// How many times the frequency crosses `mid` going up.
int swings(const std::vector<double>& v, double mid) {
  int n = 0;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i - 1] < mid && v[i] >= mid) ++n;
  return n;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

Note vibNote(Tick start, Tick len, float depth, float rate) {
  Note n(start, len, 60, 100);
  n.vib.depth = depth;
  n.vib.rate = rate;
  n.vib.fadeIn = 0;
  return n;
}

}  // namespace

int main() {
  const std::string sine = sineWav(441.0);  // C5 (60) plays the sample at 441 Hz
  const double up = 441 * std::pow(2.0, 1 / 12.0), down = 441 * std::pow(2.0, -1 / 12.0);
  {
    // 100 cents at 4 Hz over two seconds (384 ticks at 120 BPM): 441 Hz swings a semitone each way.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", sine);
    s.patterns[0].notes[ch] = {vibNote(0, 384, 100, 4)};
    const AudioBuffer b = render(s);
    const std::vector<double> f = track(b, 0.05, 1.95, 0.01);
    std::printf("# vibrato: %.1f .. %.1f Hz, %d swings\n", minOf(f), maxOf(f), swings(f, 441));
    expect(near(maxOf(f), up, 4) && near(minOf(f), down, 4), "the pitch swings by the depth (a semitone each way)");
    expect(near(swings(f, 441), 8, 1), "at the rate (4 Hz: about 8 swings in 1.9 s)");
    // Block-size invariance: the same swing at 64 and 1024.
    for (int block : {64, 1024}) {
      const std::vector<double> g = track(render(s, block), 0.05, 1.95, 0.01);
      double worst = 0;
      for (size_t i = 0; i < f.size() && i < g.size(); ++i) worst = std::max(worst, std::fabs(f[i] - g[i]));
      expect(worst < 3.0, std::string("the same vibrato at block size " + std::to_string(block)).c_str());
    }
    // Without vibrato: steady.
    s.patterns[0].notes[ch][0].vib = NoteVibrato{};
    const std::vector<double> flat = track(render(s), 0.05, 1.95, 0.01);
    expect(maxOf(flat) - minOf(flat) < 2.0, "a note without vibrato stays steady");
  }
  {
    // A bend line up an octave plus vibrato: the swing is around the bent pitch.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", sine);
    Note n = vibNote(0, 384, 50, 5);
    n.bend = {{0, 12, 0}};
    s.patterns[0].notes[ch] = {n};
    const std::vector<double> f = track(render(s), 0.1, 1.9, 0.01);
    const double hi = 882 * std::pow(2.0, 0.5 / 12.0), lo = 882 * std::pow(2.0, -0.5 / 12.0);
    std::printf("# bend + vibrato: %.1f .. %.1f Hz\n", minOf(f), maxOf(f));
    expect(near(maxOf(f), hi, 8) && near(minOf(f), lo, 8), "the vibrato rides the bend line");
  }
  {
    // A fade in over the first half: the swing early on is smaller than late.
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", sine);
    Note n = vibNote(0, 384, 100, 4);
    n.vib.fadeIn = 0.5f;
    s.patterns[0].notes[ch] = {n};
    const AudioBuffer b = render(s);
    const std::vector<double> early = track(b, 0.05, 0.4, 0.01), late = track(b, 1.2, 1.9, 0.01);
    expect(maxOf(early) - minOf(early) < 0.5 * (maxOf(late) - minOf(late)), "the fade in grows the swing");
  }
  {
    // Synced: a cycle per eighth (0.5 beat): 4 Hz at 120 BPM, 2 Hz at 60 BPM.
    for (double bpm : {120.0, 60.0}) {
      Song s = baseSong(bpm);
      const ObjectId ch = addChannel(s, "gl.sampler", sine);
      Note n = vibNote(0, 384, 100, 0.5f);
      n.vib.sync = true;
      s.patterns[0].notes[ch] = {n};
      const double len = 384.0 / 96.0 * 60.0 / bpm;
      const std::vector<double> f = track(render(s), 0.05, len - 0.05, 0.01);
      const double want = (len - 0.1) * (bpm / 60.0) * 2.0;
      std::printf("# synced at %.0f BPM: %d swings (want %.1f)\n", bpm, swings(f, 441), want);
      expect(near(swings(f, 441), want, 1.5), std::string("a synced rate follows the tempo (" + std::to_string(static_cast<int>(bpm)) + " BPM)").c_str());
    }
  }
  {
    // MIDI Out has no per-note pitch: the vibrato goes out as channel pitch bend.
    Document d(makeEmptySong());
    const ObjectId c = d.addChannel("out", "gl.midiout");
    const ObjectId pat = d.song().patterns[0].id;
    Note n(0, 192, 64, 100);
    n.vib.depth = 100;
    n.vib.rate = 5;
    n.vib.fadeIn = 0;
    d.setNotes(pat, c, {n}, "notes", 0);
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
    int bends = 0, maxV = 0, minV = 1 << 14;
    while (rt && rt->midiOut.pop(m)) {
      if ((m.s & 0xF0) == 0xE0) {
        const int v = m.a | (m.b << 7);
        ++bends, maxV = std::max(maxV, v), minV = std::min(minV, v);
      }
    }
    std::printf("# midi out: %d bend messages, %d .. %d\n", bends, minV, maxV);
    expect(bends > 40 && near(maxV, 8192 + 4096, 120) && near(minV, 8192 - 4096, 120), "MIDI Out gets the vibrato as channel pitch bend (a semitone of a 2 semitone range)");
  }
  return finish();
}
