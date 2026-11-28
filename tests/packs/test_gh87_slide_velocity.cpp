// Gridline - slide notes glide velocity (GitHub #87).
//
// A slide note glides the notes it slides (its colour group, sounding when it starts) from their
// velocity to its own over its length, as well as their pitch: a 0% note fades in, a slide to 0%
// fades out, a louder or softer slide swells, slides chain. Checked here on rendered audio:
//  - every case on Tercet (per voice), the sampler (its velocity-to-volume law), Plasma (per note)
//    and Step Bass (an instrument whose whole output is scaled);
//  - every built-in instrument: a slide to 0% silences a held note that sustains, and a slide at
//    the note's own velocity renders bit for bit as it does with velocity slides off;
//  - a project saved before 0.7.9 whose slides would change a velocity loads with them off (same
//    sound), one whose slides would not loads with them on; 0% notes survive editing and saving;
//  - other colour groups keep their level; block sizes 64 and 512 render the same;
//  - nothing is allocated on the audio thread while velocity slides play.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "../Check.h"
#include "../../src/core/Document.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"

namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr Tick Q = 96;  // one beat: 0.5 s at 120 BPM

Note mk(Tick start, Tick len, int pitch, int vel, bool slide = false, int color = 0) {
  Note n(start, len, pitch, vel);
  n.slide = slide;
  n.color = color;
  return n;
}

std::string sineWav() {
  AudioBuffer b(2, static_cast<size_t>(kSr * 4));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * 261.6256 * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = (std::filesystem::temp_directory_path() / "gridline_test_gh87_c4.wav").u8string();
  writeWav(path, b, kSr, 16);
  return path;
}

// One channel of `type` on the master, one 4-bar pattern clip, 120 BPM.
Song songFor(const std::string& type, const std::string& wav, ObjectId& ch) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 4;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  Channel x;
  x.id = s.allocateId();
  x.name = type;
  x.plugin = defaultPluginState(type);
  if (type == "gl.synth") {  // a plain sustained sine: its level reads cleanly
    x.plugin.params[pp::SynOsc1Shape] = 0.0f;
    x.plugin.params[pp::SynOsc1Level] = 1.0f;
    x.plugin.params[pp::SynOsc2Level] = 0.0f;
    x.plugin.params[pp::SynOsc3Level] = 0.0f;
    x.plugin.params[pp::SynAttack] = 0.002f;
    x.plugin.params[pp::SynSustain] = 1.0f;
    x.plugin.params[pp::SynRelease] = 0.05f;
  } else if (type == "gl.sampler") {
    x.plugin.data = wav;
    x.plugin.params[pp::SmpLoop] = 1.0f;
    x.plugin.params[pp::SmpAttack] = 0.002f;
    x.plugin.params[pp::SmpSustain] = 1.0f;
    x.plugin.params[pp::SmpVelVolume] = 1.0f;
  }
  x.mixerTrack = s.masterId();
  s.channels.push_back(x);
  ch = x.id;
  return s;
}

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.sampleRate = kSr;
  o.blockSize = block;
  o.tail = 0;
  return renderSong(s, o);
}

double rms(const AudioBuffer& b, double t0, double t1) {
  const size_t a = std::min(b.frames, static_cast<size_t>(t0 * kSr)), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  double sum = 0;
  for (size_t i = a; i < e; ++i) sum += 0.5 * (static_cast<double>(b.at(0, i)) * b.at(0, i) + static_cast<double>(b.at(1, i)) * b.at(1, i));
  return e > a ? std::sqrt(sum / static_cast<double>(e - a)) : 0.0;
}

bool same(const AudioBuffer& a, const AudioBuffer& b) { return a.frames == b.frames && a.data == b.data; }

float maxDiff(const AudioBuffer& a, const AudioBuffer& b) {
  float d = a.frames == b.frames ? 0.0f : 1e9f;
  for (size_t i = 0; i < std::min(a.data.size(), b.data.size()); ++i) d = std::max(d, std::fabs(a.data[i] - b.data[i]));
  return d;
}

bool finite(const AudioBuffer& b) {
  for (float v : b.data)
    if (!std::isfinite(v)) return false;
  return true;
}

// The level of a plain held note at velocity `vel`, in [t0, t1) s.
double plainLevel(Song s, ObjectId ch, int vel, double t0, double t1) {
  s.patterns[0].notes[ch] = {mk(0, 12 * Q, 60, vel)};
  return rms(render(s), t0, t1);
}

// The cases on one instrument. A slide's velocity acts on the note's level: a note a slide takes
// from velocity a to b plays b/a as loud as before (0% is silence), whatever the instrument's own
// response to the velocity a note starts with.
void testCases(const std::string& type, const std::string& wav) {
  ObjectId ch = kNoId;
  Song s = songFor(type, wav, ch);
  auto& notes = s.patterns[0].notes[ch];
  const std::string t = type + ": ";
  const double full = plainLevel(s, ch, 127, 0.3, 0.45), late = plainLevel(s, ch, 127, 1.1, 1.6), mid127 = plainLevel(s, ch, 127, 0.7, 0.8);
  const double tol = 0.01 * full + 1e-6;
  expect(full > 1e-3, (t + "the held note sounds").c_str());

  // Fade out: 100% -> 0% over beat 2 (0.5 s .. 1.0 s).
  notes = {mk(0, 8 * Q, 60, 127), mk(Q, Q, 60, 0, true)};
  const AudioBuffer out = render(s), out64 = render(s, 64);
  const double before = rms(out, 0.3, 0.45), mid = rms(out, 0.7, 0.8), after = rms(out, 1.1, 1.6);
  std::printf("# %-12s fade out: %.4f -> %.4f -> %.6f (plain %.4f)\n", type.c_str(), before, mid, after, full);
  expect(finite(out) && std::fabs(before - full) < 1e-6 * std::max(1.0, full), (t + "fade out: before the slide the note plays as it is").c_str());
  expect(mid > 0.3 * mid127 && mid < 0.7 * mid127, (t + "fade out: about half as loud halfway").c_str());
  expect(after < 1e-4, (t + "fade out: silent once the slide reaches 0%").c_str());
  expect(maxDiff(out, out64) < 1e-5f, (t + "fade out: block size 64 renders as 512").c_str());

  // Fade in: a 0% note, a slide to 100% over beat 2.
  notes = {mk(0, 8 * Q, 60, 0), mk(Q, Q, 60, 127, true)};
  const AudioBuffer in = render(s), in64 = render(s, 64);
  const double inBefore = rms(in, 0.05, 0.5), inMid = rms(in, 0.7, 0.8), inAfter = rms(in, 1.1, 1.6);
  std::printf("# %-12s fade in: %.6f -> %.4f -> %.4f (plain %.4f)\n", type.c_str(), inBefore, inMid, inAfter, late);
  expect(finite(in) && inBefore < 1e-5, (t + "fade in: silent before the slide").c_str());
  expect(inMid > 0.3 * mid127 && inMid < 0.7 * mid127, (t + "fade in: about half as loud halfway").c_str());
  expect(std::fabs(inAfter - late) < tol, (t + "fade in: as loud as a 100% note once the slide reaches 100%").c_str());
  expect(maxDiff(in, in64) < 1e-5f, (t + "fade in: block size 64 renders as 512").c_str());
  // A 0% note with no slide plays as it always did.
  notes = {mk(0, 8 * Q, 60, 0)};
  const AudioBuffer z = render(s);
  s.slidesVelocity = false;
  expect(same(z, render(s)), (t + "a 0% note without a slide plays as before").c_str());
  s.slidesVelocity = true;

  // Crescendo and decrescendo: 40 -> 120 and back: the level goes 3 times up or down.
  const double soft = plainLevel(s, ch, 40, 1.1, 1.6), loud = plainLevel(s, ch, 120, 1.1, 1.6);
  notes = {mk(0, 8 * Q, 60, 40), mk(Q, Q, 60, 120, true)};
  const double cres = rms(render(s), 1.1, 1.6);
  notes = {mk(0, 8 * Q, 60, 120), mk(Q, Q, 60, 40, true)};
  const double decres = rms(render(s), 1.1, 1.6);
  std::printf("# %-12s crescendo %.4f -> %.4f, decrescendo %.4f -> %.4f\n", type.c_str(), soft, cres, loud, decres);
  expect(std::fabs(cres - 3.0 * soft) < 0.02 * cres + 1e-6, (t + "crescendo: from 40 to 120 the note gets 3 times as loud").c_str());
  expect(std::fabs(decres - loud / 3.0) < 0.02 * loud + 1e-6, (t + "decrescendo: from 120 to 40 the note gets 3 times as soft").c_str());

  // Chained swells: 0 -> 100 (beat 2), -> 0 (beat 3), -> 100 (beat 4), holding in between.
  notes = {mk(0, 12 * Q, 60, 0), mk(Q, Q, 60, 127, true), mk(3 * Q, Q, 60, 0, true), mk(5 * Q, Q, 60, 127, true)};
  const AudioBuffer sw = render(s);
  const double s1 = rms(sw, 1.1, 1.45), s2 = rms(sw, 2.1, 2.45), s3 = rms(sw, 3.1, 3.45);
  const double p1 = plainLevel(s, ch, 127, 1.1, 1.45), p3 = plainLevel(s, ch, 127, 3.1, 3.45);
  std::printf("# %-12s swells: %.4f, %.6f, %.4f (plain %.4f, %.4f)\n", type.c_str(), s1, s2, s3, p1, p3);
  expect(std::fabs(s1 - p1) < tol && s2 < 1e-4 && std::fabs(s3 - p3) < tol, (t + "chained swells: in, out and in again, holding between the slides").c_str());

  // A slide at the note's own velocity changes only its pitch: bit for bit as with velocity slides off.
  notes = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 67, 100, true)};
  const AudioBuffer on = render(s);
  s.slidesVelocity = false;
  const AudioBuffer off = render(s);
  expect(same(on, off), (t + "a slide at the note's own velocity renders bit for bit as before").c_str());
  // Velocity slides off (projects saved before them): a slide to 0% changes the pitch only.
  notes = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 67, 0, true)};
  expect(same(render(s), off), (t + "with velocity slides off a slide's velocity changes nothing").c_str());
  s.slidesVelocity = true;
}

// Colour groups: a slide of group 1 fades only the group 1 note (each note on its own key).
void testColours() {
  ObjectId ch = kNoId;
  Song s = songFor("gl.synth", "", ch);
  auto& notes = s.patterns[0].notes[ch];
  notes = {mk(0, 8 * Q, 60, 110, false, 0)};
  const AudioBuffer a = render(s);
  notes = {mk(0, 8 * Q, 60, 110, false, 0), mk(0, 8 * Q, 67, 110, false, 1), mk(Q, Q, 67, 0, true, 1)};
  const AudioBuffer b = render(s);
  expect(maxDiff(a, b) > 0.01f, "colours: the group 1 note plays while it sounds");
  // After the slide only the group 0 note is left: the render equals that note alone.
  const size_t from = static_cast<size_t>(1.1 * kSr);
  float d = 0;
  for (size_t i = from; i < std::min(a.frames, b.frames); ++i) d = std::max(d, std::fabs(a.at(0, i) - b.at(0, i)));
  std::printf("# colours: after the group 1 fade the difference to group 0 alone is %.3g\n", static_cast<double>(d));
  expect(d < 1e-4f, "colours: a slide fades only the notes of its own colour group");
}

// Every built-in instrument: a slide at the note's velocity is bit-identical with velocity slides on
// and off; a slide to 0% silences a held note (where the note still sounds after the slide).
void testEveryInstrument(const std::string& wav) {
  int faded = 0, checked = 0;
  std::string decayed, notFaded;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.kind != PluginKind::Generator || p.hidden) continue;
    ObjectId ch = kNoId;
    Song s = songFor(p.type, wav, ch);
    Channel& c = s.channels.back();
    if (p.usesSample && c.plugin.data.empty()) c.plugin.data = wav;
    auto& notes = s.patterns[0].notes[ch];
    notes = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 64, 100, true)};
    const AudioBuffer on = render(s);
    s.slidesVelocity = false;
    const AudioBuffer off = render(s);
    s.slidesVelocity = true;
    ++checked;
    expect(same(on, off) && finite(on), (p.type + ": a slide at the note's own velocity renders bit for bit as before").c_str());
    notes = {mk(0, 8 * Q, 60, 100)};
    const AudioBuffer plain = render(s);
    const double left = rms(plain, 1.1, 1.6);
    if (left < 1e-3 * std::max(1e-9, rms(plain, 0.05, 0.45)) || left < 1e-5) {
      decayed += " " + p.type;  // the note is over before the slide ends: nothing to fade
      continue;
    }
    // A slide to 0% silences the note; one to 40 leaves it softer but sounding.
    notes = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 60, 0, true)};
    const AudioBuffer out = render(s);
    const double after = rms(out, 1.1, 1.6);
    notes = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 60, 40, true)};
    const double down = rms(render(s), 1.1, 1.6);
    const bool good = after < 1e-3 * left && down > 0.02 * left && down < 0.7 * left;
    if (good) {
      ++faded;
    } else {
      char b[128];
      std::snprintf(b, sizeof b, " %s(0%%: %.4f, 40: %.4f, 100: %.4f)", p.type.c_str(), after, down, left);
      notFaded += b;
    }
    expect(finite(out) && good, (p.type + ": a slide to 0% fades the held note out, one to 40 softens it").c_str());
  }
  std::printf("# %d instruments checked, %d fade; over before the slide ends:%s\n", checked, faded, decayed.c_str());
  if (!notFaded.empty()) std::printf("# not faded:%s\n", notFaded.c_str());
}

// Old projects: no "slidesVelocity" key. Slides that would change a velocity: loaded off.
void testOldProjects() {
  ObjectId ch = kNoId;
  Song s = songFor("gl.synth", "", ch);
  s.patterns[0].notes[ch] = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 67, 60, true)};
  Json j = songToJson(s);
  expect(j.find("slidesVelocity") && j.find("slidesVelocity")->boolean, "a saved project says slides glide velocity");
  j.object.erase("slidesVelocity");
  Song old;
  expect(songFromJson(j, old) && !old.slidesVelocity, "an old project whose slide changes a velocity loads with velocity slides off");
  Song pitchOnly = s;
  pitchOnly.slidesVelocity = false;
  expect(same(render(old), render(pitchOnly)), "the old project sounds as it did (its slide moves the pitch only)");
  s.patterns[0].notes[ch] = {mk(0, 8 * Q, 60, 100), mk(Q, Q, 67, 100, true)};
  Json k = songToJson(s);
  k.object.erase("slidesVelocity");
  Song old2;
  expect(songFromJson(k, old2) && old2.slidesVelocity, "an old project whose slides keep the velocity loads with velocity slides on");
  s.slidesVelocity = false;
  Song back;
  expect(songFromJson(songToJson(s), back) && !back.slidesVelocity, "the setting survives saving");

  // 0% notes: the document keeps them, saving keeps them.
  Document d(makeEmptySong());
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId c = d.addChannel("Lead", "gl.synth");
  expect(d.setNotes(pat, c, {mk(0, Q, 60, 0), mk(0, Q, 64, -5)}, "notes"), "notes set");
  const auto& ns = d.song().pattern(pat)->notes.at(c);
  expect(ns.size() == 2 && ns[0].velocity == 0 && ns[1].velocity == 0, "a 0% note stays 0% (below 0 is 0)");
  Song rt;
  expect(songFromJson(songToJson(d.song()), rt) && rt.pattern(pat)->notes.at(c)[0].velocity == 0, "a 0% note survives saving");
}

// The program marks a 0% note with the loudest slide that starts while it sounds (its colour).
void testProgramRef() {
  ObjectId ch = kNoId;
  Song s = songFor("gl.synth", "", ch);
  s.patterns[0].notes[ch] = {mk(0, 4 * Q, 60, 0), mk(Q, Q, 60, 64, true), mk(2 * Q, Q, 60, 127, true), mk(2 * Q, Q, 60, 100, true, 3),
                             mk(5 * Q, Q, 60, 90, true)};
  const std::vector<NoteEv> ev = compilePatternChannel(s, s.patterns[0], ch);
  float ref = -1;
  for (const NoteEv& e : ev)
    if (!(e.flags & kNoteSlide)) ref = e.slideVelRef;
  expect(std::fabs(ref - 1.0f) < 1e-6f, "a 0% note starts at the loudest slide of its colour while it sounds");
  s.slidesVelocity = false;
  const std::vector<NoteEv> ev2 = compilePatternChannel(s, s.patterns[0], ch);
  for (const NoteEv& e : ev2)
    if (!(e.flags & kNoteSlide)) ref = e.slideVelRef;
  expect(ref == 0.0f, "with velocity slides off a 0% note stays a 0% note");
}

// Nothing is allocated on the audio thread while velocity slides play (four instruments).
void testNoAlloc(const std::string& wav) {
  for (const char* type : {"gl.synth", "gl.sampler", "gl.plasma", "gl.stepbass"}) {
    ObjectId ch = kNoId;
    Song s = songFor(type, wav, ch);
    s.patterns[0].notes[ch] = {mk(0, 12 * Q, 60, 0), mk(0, Q, 60, 127, true), mk(Q, Q, 62, 0, true), mk(2 * Q, Q, 60, 90, true),
                               mk(4 * Q, 4 * Q, 67, 50), mk(5 * Q, Q, 67, 120, true)};
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setMode(PlayMode::Song);
    eng.setProgram(pb.build(s));
    eng.play();
    std::vector<float> l(256), r(256);
    for (int k = 0; k < 4; ++k) eng.process(l.data(), r.data(), 256);  // warm up
    gAllocs = 0;
    gCounting = true;
    for (int k = 0; k < 600; ++k) eng.process(l.data(), r.data(), 256);  // 3.5 s: every slide
    gCounting = false;
    if (gAllocs.load() != 0) std::printf("# %s: %ld allocations while velocity slides play\n", type, gAllocs.load());
    expect(gAllocs.load() == 0, (std::string(type) + ": no allocation on the audio thread while velocity slides play").c_str());
    eng.stop();
  }
}

}  // namespace

int main() {
  const std::string wav = sineWav();
  for (const char* type : {"gl.synth", "gl.sampler", "gl.plasma", "gl.stepbass"}) testCases(type, wav);
  testColours();
  testOldProjects();
  testProgramRef();
  testNoAlloc(wav);
  testEveryInstrument(wav);
  return finish();
}
