// Gridline - per-note expression (MPE) on Gridline's own instruments: the expression event
// (pitch, pressure, timbre), the channel's expression setting, the notes' pressure and timbre
// lanes (save / load, note edits, playback) and live MPE input from a controller.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Json.h"
#include "core/NoteBend.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "engine/Render.h"
#include "packs/Packs.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

fs::path dir() {
  static const fs::path d = [] {
    fs::path p = fs::temp_directory_path() / "gridline_test_mpe";
    fs::create_directories(p);
    return p;
  }();
  return d;
}

// A 220 Hz sine (the sampler plays it at that pitch on key 60).
std::string sineWav() {
  const std::string path = (dir() / "sine220.wav").string();
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.4 * std::sin(2 * kPi * 220.0 * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  writeWav(path, b, kSr, 16);
  return path;
}

std::string sfzFile(const std::string& name, const std::string& text) {
  const std::string path = (dir() / name).string();
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
  return path;
}

// One instrument driven directly.
struct Gen {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
  ProcessContext ctx;
  std::vector<float> L, R;
  Gen(const std::string& type, const std::string& data = "") {
    st = defaultPluginState(type);
    st.data = data;
    packs::prepareAux(st, aux);
    if (type == "gl.sampler" && !data.empty()) aux.samples = {sampleCache().get(data)};
    g = createGenerator(type);
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
  }
  void run(size_t n, int block = 256) {
    const size_t base = L.size();
    L.resize(base + n, 0.f);
    R.resize(base + n, 0.f);
    for (size_t p = 0; p < n; p += static_cast<size_t>(block)) {
      const int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - p));
      g->process(st, aux, ctx, L.data() + base + p, R.data() + base + p, len);
    }
  }
  size_t at() const { return L.size(); }
};

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  if (b <= a) return 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(b - a));
}
double freq(const std::vector<float>& v, size_t a, size_t b) {
  int z = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; ++i)
    if ((v[i - 1] < 0) != (v[i] < 0)) ++z;
  return b > a ? z / 2.0 / (static_cast<double>(b - a) / kSr) : 0.0;
}
// High-frequency share: rms of the first difference over rms.
double bright(const std::vector<float>& v, size_t a, size_t b) {
  double d = 0, s = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i) d += (v[i] - v[i - 1]) * (v[i] - v[i - 1]), s += v[i] * v[i];
  return s > 0 ? std::sqrt(d / s) : 0.0;
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// A held note, then one expression event, then more sound; returns {before, after} measures.
struct Two {
  double before, after;
};
template <class F>
Two around(Gen& r, ExprDim d, float v, F measure, const ExprSettings& s = {}) {
  r.g->setExpression(s);
  r.g->noteOn(1, 60, 0.8f, 0);
  r.run(kSr / 4);
  const double b = measure(r.L, kSr / 8, kSr / 4);
  r.g->expr(1, d, v, 0);
  const size_t at = r.at();
  r.run(kSr / 4);
  return {b, measure(r.L, at + kSr / 16, at + kSr / 4)};
}

void testEvent(const std::string& sine) {
  {
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pitch, 12, freq);
    expect(std::fabs(t.before - 220) < 4 && std::fabs(t.after - 440) < 8, "pitch: +12 semitones of note bend plays an octave up");
  }
  {
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pitch, -7, freq);
    expect(std::fabs(t.after - 220 * std::pow(2.0, -7 / 12.0)) < 4, "pitch: a downward bend follows too");
  }
  {
    ExprSettings s;
    s.pitch = false;
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pitch, 12, freq, s);
    expect(std::fabs(t.after - 220) < 4, "pitch: the channel's note bend switch off keeps the pitch");
  }
  {
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pressure, 1, rms);
    expect(std::fabs(t.after / t.before - 2.0) < 0.1, "pressure: full pressure lifts the level by 6 dB (the default for most instruments)");
  }
  {
    ExprSettings s;
    s.amount = 0.5f;
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pressure, 1, rms, s);
    expect(std::fabs(t.after / t.before - std::sqrt(2.0)) < 0.08, "pressure: the amount scales the response (half: +3 dB)");
  }
  {
    ExprSettings s;
    s.pressure = ExprUse::Off;
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Pressure, 1, rms, s);
    expect(std::fabs(t.after / t.before - 1.0) < 0.02, "pressure: Off leaves the sound alone");
  }
  {
    Gen up("gl.synth"), down("gl.synth"), mid("gl.synth");
    const Two a = around(up, ExprDim::Timbre, 1, bright), b = around(down, ExprDim::Timbre, 0, bright), c = around(mid, ExprDim::Timbre, 0.5f, bright);
    expect(a.after > c.after * 1.2 && b.after < c.after * 0.85, "timbre: sliding up brightens, sliding down darkens (the default for most instruments)");
    Gen none("gl.synth");
    none.g->noteOn(1, 60, 0.8f, 0);
    none.run(kSr / 4);  // the same blocks as `around`
    none.run(kSr / 4);
    double diff = 0;
    for (size_t i = 0; i < none.L.size() && i < mid.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(none.L[i] - mid.L[i])));
    expect(diff < 1e-6, "timbre: the centre is neutral (the same sound as no expression)");
  }
  {
    ExprSettings s;
    s.timbre = ExprUse::Level;
    Gen r("gl.sampler", sine);
    const Two t = around(r, ExprDim::Timbre, 1, rms, s);
    expect(std::fabs(t.after / t.before - 2.0) < 0.1, "timbre: the channel can send it to the level instead");
  }
  {
    Gen r("gl.sampler", sine);
    r.g->noteOn(1, 60, 0.8f, 0);
    r.run(kSr / 4);
    const double b = rms(r.L, kSr / 8, kSr / 4);
    r.g->expr(99, ExprDim::Pressure, 1, 0);  // no such note
    r.g->expr(1, ExprDim::Pressure, std::nanf(""), 0);
    const size_t at = r.at();
    r.run(kSr / 4);
    expect(std::fabs(rms(r.L, at, r.at()) / b - 1.0) < 0.02 && finite(r.L), "expression for a note not held, or not a number, changes nothing");
  }
  {
    Gen r("gl.sampler", sine);  // a note starts with its expression (MPE sets it before the note-on)
    NoteProps p;
    p.pressure = 1;
    r.g->noteOn(1, 60, 0.8f, 0, p);
    r.run(kSr / 2);
    Gen plain("gl.sampler", sine);
    plain.g->noteOn(1, 60, 0.8f, 0);
    plain.run(kSr / 2);
    expect(std::fabs(rms(r.L, kSr / 4, kSr / 2) / rms(plain.L, kSr / 4, kSr / 2) - 2.0) < 0.1, "a note's starting pressure applies from its start");
  }
  {
    // Two notes: the generic response follows the stronger one; releasing it hands over.
    Gen r("gl.sampler", sine);
    r.g->noteOn(1, 60, 0.8f, 0);
    r.g->noteOn(2, 67, 0.8f, 0);
    r.run(kSr / 4);
    const double b = rms(r.L, kSr / 8, kSr / 4);
    r.g->expr(2, ExprDim::Pressure, 1, 0);
    r.g->expr(1, ExprDim::Pressure, 0.2f, 0);
    size_t at = r.at();
    r.run(kSr / 4);
    expect(std::fabs(rms(r.L, at + kSr / 16, r.at()) / b - 2.0) < 0.12, "two notes: the strongest pressure leads");
    expect(r.g->noteExpr(1, ExprDim::Pressure) > 0.19f && r.g->noteExpr(2, ExprDim::Pressure) > 0.99f, "each note keeps its own values");
  }
  {
    // Every block size gives the same sound.
    std::vector<float> a, b;
    for (int pass = 0; pass < 2; ++pass) {
      Gen r("gl.sampler", sine);
      const int blk = pass ? 37 : 512;
      r.g->noteOn(1, 60, 0.8f, 0);
      r.run(4000, blk);
      r.g->expr(1, ExprDim::Timbre, 0.9f, 0);
      r.g->expr(1, ExprDim::Pressure, 0.7f, 0);
      r.run(4000, blk);
      (pass ? b : a) = r.L;
    }
    double diff = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(diff < 1e-4, "the block size does not change the expression");
  }
}

// The SFZ map in the Multisampler takes pressure as channel aftertouch and timbre as CC 74.
void testSfz() {
  auto play = [&](const char* opcode, ExprDim d, float v) {
    std::string text = "<region> sample=*sine key=60 amplitude=0 amplitude_";
    text += opcode;
    text += "=100\n";
    Gen r("gl.multisampler", sfzFile(std::string(opcode) + ".sfz", text));
    r.g->noteOn(1, 60, 1, 0);
    r.run(4096);
    r.g->expr(1, d, v, 0);
    const size_t at = r.at();
    r.run(kSr / 4);
    return rms(r.L, at + 4096, r.at());
  };
  expect(play("oncc129", ExprDim::Pressure, 1) > 0.05 && play("oncc129", ExprDim::Pressure, 0) < 1e-4,
         "SFZ: note pressure reaches the map's channel aftertouch opcodes");
  expect(play("oncc74", ExprDim::Timbre, 1) > 0.05 && play("oncc74", ExprDim::Timbre, 0) < 1e-4, "SFZ: note timbre reaches the map's CC 74 opcodes");
}

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

void testLanes(const std::string& sine) {
  // Save / load: lanes and the channel setting; plain notes and channels write no new keys.
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", sine);
    s.channels.back().expr.pressure = ExprUse::Brightness;
    s.channels.back().expr.pitch = false;
    s.channels.back().expr.amount = 0.25f;
    Note n(0, 192, 60, 100);
    n.pressure = {{96, 1.0f, bend::Smooth}};
    n.timbre = {{0, -0.25f}, {150, 0.5f}};
    s.patterns[0].notes[ch] = {n, Note(192, 96, 62, 100)};
    Json j;
    expect(jsonParse(jsonStringify(songToJson(s)), j), "a project with expression saves");
    Song back;
    expect(songFromJson(j, back) && back == s, "note pressure / timbre lanes and the channel's expression setting survive save and load");
    Song plain = baseSong();
    const ObjectId p = addChannel(plain, "gl.sampler", sine);
    plain.patterns[0].notes[p] = {Note(0, 96, 60, 100)};
    const std::string text = jsonStringify(songToJson(plain));
    expect(text.find("\"pressure\"") == std::string::npos && text.find("\"timbre\"") == std::string::npos &&
               text.find("\"expression\"") == std::string::npos,
           "notes and channels without expression write no new keys");
    Song again;
    Json jp;
    expect(jsonParse(text, jp) && songFromJson(jp, again) && again.channels.back().expr == ExprSettings{} &&
               again.patterns[0].notes[p][0].pressure.empty(),
           "older projects load with neutral expression");
  }
  // Note edits carry the lanes: a split cuts them where the note is cut.
  {
    Note n(0, 192, 60, 100), l, r;
    n.pressure = {{192, 1.0f}};
    bend::split(n, 96, l, r);
    expect(!l.pressure.empty() && !r.pressure.empty() && std::fabs(bend::value(r.pressure, 0) - 0.5) < 1e-6 &&
               std::fabs(bend::value(l.pressure, 96) - 0.5) < 1e-6,
           "a split note keeps each half of its pressure lane");
  }
  // Playback: a pressure lane rising over the note raises its level as it goes; a timbre lane.
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", sine);
    Note n(0, 384, 60, 100);
    n.pressure = {{96, 0.0f}, {192, 1.0f}};
    s.patterns[0].notes[ch] = {n};
    RenderOptions o;
    for (int blk : {512, 61}) {
      o.blockSize = blk;
      const AudioBuffer b = renderSong(s, o);
      std::vector<float> l(b.frames);
      for (size_t i = 0; i < b.frames; ++i) l[i] = b.at(0, i);
      const double early = rms(l, kSr / 8, kSr / 2), late = rms(l, kSr * 11 / 10, kSr * 3 / 2);
      char name[96];
      std::snprintf(name, sizeof name, "a pressure lane plays: the level rises 6 dB along the note (block %d)", blk);
      expect(early > 0 && std::fabs(late / early - 2.0) < 0.15, name);
    }
  }
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.synth");
    Note n(0, 384, 48, 100);
    n.timbre = {{96, 0.0f}, {192, 0.5f}};
    s.patterns[0].notes[ch] = {n};
    const AudioBuffer b = renderSong(s, RenderOptions{});
    std::vector<float> l(b.frames);
    for (size_t i = 0; i < b.frames; ++i) l[i] = b.at(0, i);
    expect(bright(l, kSr * 11 / 10, kSr * 3 / 2) > bright(l, kSr / 8, kSr / 2) * 1.2, "a timbre lane plays: the note brightens along it");
  }
}

// Live MPE input: member channels move their own note; channel 1 stays global.
void testLive(const std::string& sine) {
  Song song = makeEmptySong();
  const ObjectId ch = addChannel(song, "gl.sampler", sine);
  ProgramBuilder builder;
  builder.setSampleRate(kSr);
  AudioEngine eng{kSr};
  eng.setProgram(builder.build(song));
  eng.setMidiTarget(ch);
  std::vector<float> L, R;
  auto run = [&](size_t n) {
    std::vector<float> l(256), r(256);
    for (size_t d = 0; d < n; d += 256) {
      std::fill(l.begin(), l.end(), 0.f);
      std::fill(r.begin(), r.end(), 0.f);
      eng.process(l.data(), r.data(), 256);
      L.insert(L.end(), l.begin(), l.end());
      R.insert(R.end(), r.begin(), r.end());
    }
  };
  eng.setMpeInput(true, 48);
  eng.midiEvent(0x91, 60, 100);  // channel 2: one note
  run(kSr / 4);
  const double f0 = freq(L, kSr / 8, kSr / 4), r0 = rms(L, kSr / 8, kSr / 4);
  // +12 of 48 semitones on channel 2: 8192 + 2048
  const int v = 8192 + 2048;
  eng.midiEvent(0xE1, static_cast<uint8_t>(v & 0x7F), static_cast<uint8_t>(v >> 7));
  eng.midiEvent(0xD1, 127, 0);
  size_t at = L.size();
  run(kSr / 4);
  expect(std::fabs(f0 - 220) < 4 && std::fabs(freq(L, at + kSr / 16, L.size()) - 440) < 8, "live MPE: a member channel's bend moves its note (48 semitone range)");
  expect(std::fabs(rms(L, at + kSr / 16, L.size()) / r0 - 2.0) < 0.15, "live MPE: its channel pressure raises the note's level");
  eng.midiEvent(0x81, 60, 0);
  run(kSr / 2);
  // Without MPE the same messages keep their old meaning: pressure goes to the instrument as is.
  eng.setMpeInput(false, 48);
  eng.midiEvent(0xE1, 0, 64);
  eng.midiEvent(0xD1, 0, 0);
  L.clear();
  eng.midiEvent(0x91, 60, 100);
  run(kSr / 4);
  const double p0 = rms(L, kSr / 8, kSr / 4);
  eng.midiEvent(0xD1, 127, 0);
  at = L.size();
  run(kSr / 4);
  expect(std::fabs(rms(L, at + kSr / 16, L.size()) / p0 - 1.0) < 0.02, "without MPE, channel pressure does not change a sampler note");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv();
  const std::string sine = sineWav();
  testEvent(sine);
  testSfz();
  testLanes(sine);
  testLive(sine);
  return finish();
}
