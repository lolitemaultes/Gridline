// Gridline — "platter" pack tests: Platter (gl.platter), the DJ deck.
// Controller decoding from synthetic bytes (DDJ-200 both decks, Inpulse 200,
// Mixtrack Pro FX, learn), mappings and models as text and preset files, the
// platter physics (hand tracking, motor start / brake, throws), click-free
// direction changes, band-limited reads at speed, key lock, tempo sync and beat
// phase, block-size invariance, no allocation on the audio thread, takes that
// replay sample for sample, project save / load, and the old Turntable effect.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <new>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/ExternalDrop.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/Sample.h"
#include "packs/Packs.h"
#include "packs/platter/platter.h"

using namespace gl;
namespace pl = gl::packs::platter;
namespace fs = std::filesystem;

namespace {

std::atomic<long> gAllocs{0};
thread_local bool gWatch = false;
constexpr double kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_platter_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// A test track: `fn(frame, channel)` for `seconds`, written as 24 bit WAV.
std::string writeTrack(const std::string& name, double seconds, const std::function<double(int64_t, int)>& fn) {
  const auto n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) b.data[static_cast<size_t>(c) * n + i] = static_cast<float>(fn(static_cast<int64_t>(i), c));
  const std::string path = (tmpDir() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

// Music-like material: a few partials and a click on every beat at `bpm`.
double music(int64_t i, int c, double bpm) {
  const double t = static_cast<double>(i) / kSr;
  const double beat = 60.0 / bpm;
  const double ph = std::fmod(t, beat);
  double x = 0.25 * std::sin(2 * kPi * 220 * t + c) + 0.12 * std::sin(2 * kPi * 331 * t) + 0.08 * std::sin(2 * kPi * 1250 * t + 0.5 * c);
  x += 0.4 * std::exp(-ph * 60.0) * std::sin(2 * kPi * 90 * ph);
  return x;
}

struct Deck {
  std::unique_ptr<Generator> g;
  PluginState st;
  ChannelAux aux;
  ProcessContext ctx;
  pl::Model m;
  std::string path;
  int64_t pos = 0;
  std::vector<float> L, R;
};

// A deck on `path` with a flat grid at `bpm` (cue at 0) and the DDJ-200 left mapping.
Deck makeDeck(const std::string& path, double bpm, const std::function<void(PluginState&, pl::Model&)>& tweak = nullptr) {
  Deck d;
  d.path = path;
  SamplePtr src = sampleCache().get(path);
  d.m = pl::defaultModel(*src, gl::packs::seltmann::flatGrid(*src, bpm), true);
  d.m.cue = 0;
  d.m.map = pl::profileMapping("ddj200", 0);
  d.st = defaultPluginState(pl::kType);
  if (tweak) tweak(d.st, d.m);
  d.st.data = pl::formatData(path, d.m);
  packs::prepareAux(d.st, d.aux);
  d.g = createGenerator(pl::kType);
  d.ctx.sampleRate = kSr;
  d.ctx.tempo = 120;
  d.ctx.ppq = 96;
  return d;
}

// Renders `frames` in blocks; `ev(pos, n)` adds events for the block [pos, pos + n).
void run(Deck& d, int64_t frames, int block, const std::function<void(Deck&, int64_t, int)>& ev = nullptr, bool watch = false) {
  const size_t base = d.L.size();
  d.L.resize(base + static_cast<size_t>(frames), 0.f);
  d.R.resize(base + static_cast<size_t>(frames), 0.f);
  for (int64_t k = 0; k < frames; k += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - k));
    d.ctx.tick = static_cast<double>(d.pos) / kSr * d.ctx.tempo / 60.0 * d.ctx.ppq;
    d.ctx.jumped = d.pos == 0;
    if (ev) ev(d, d.pos, n);
    if (watch && d.pos > 0) gWatch = true;
    d.g->process(d.st, d.aux, d.ctx, d.L.data() + base + k, d.R.data() + base + k, n);
    gWatch = false;
    d.pos += n;
  }
}

pl::LiveView view(const Deck& d) {
  pl::LiveView v;
  pl::liveView(d.g.get(), v);
  return v;
}
void cmd(Deck& d, uint8_t kind, uint8_t a = 0, double v = 0, double w = 0) {
  pl::Cmd c;
  c.kind = kind;
  c.a = a;
  c.v = v;
  c.w = w;
  pl::command(d.g.get(), c);
}
void button(Deck& d, int control, bool down) { cmd(d, pl::CmdButton, static_cast<uint8_t>(control), down ? 1 : 0); }

double maxStep(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0;
  for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  return m;
}
double rms(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0;
  size_t n = 0;
  for (size_t i = from; i < std::min(to, x.size()); ++i, ++n) s += static_cast<double>(x[i]) * x[i];
  return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}
bool finite(const std::vector<float>& x) {
  for (float v : x)
    if (!std::isfinite(v)) return false;
  return true;
}
// Rising zero crossings per second.
double frequency(const std::vector<float>& x, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from + 1; i < std::min(to, x.size()); ++i)
    if (x[i - 1] < 0 && x[i] >= 0) ++n;
  return n * kSr / static_cast<double>(to - from);
}

// ------------------------------------------------------------------ catalogue
void testCatalogue() {
  const PluginInfo* p = findPlugin(pl::kType);
  expect(p != nullptr, "catalogue: gl.platter is listed");
  if (!p) return;
  expect(p->kind == PluginKind::Generator && p->usesSample, "catalogue: an instrument that plays a sample");
  expect(static_cast<int>(p->params.size()) == pl::PCount, "catalogue: every deck parameter (automatable)");
  expect(p->presets.size() >= 2 && p->presets.size() <= 6, "catalogue: 2 to 6 factory set-ups");
  expect(createGenerator(pl::kType) != nullptr, "the deck is created");
  expect(xdrop::sampleSlot(pl::kType) == xdrop::SampleSlot::FirstLine, "dropped audio replaces the file and keeps the deck set-up");
  expect(xdrop::withSample(pl::kType, "/a.wav\nplatter 1", "/b.wav") == "/b.wav\nplatter 1", "a new file keeps the model text");
  // an old state with fewer parameters still loads
  PluginState s = defaultPluginState(pl::kType);
  s.params.resize(5);
  normalizePluginState(s);
  expect(static_cast<int>(s.params.size()) == pl::PCount, "short parameter lists are completed");
}

// ------------------------------------------------------------------ controller decoding
std::vector<pl::InEvent> feed(pl::Decoder& d, const pl::Mapping& m, uint8_t s, uint8_t a, uint8_t b) {
  pl::InEvent ev[8];
  const int n = d.feed(m, s, a, b, ev, 8);
  return std::vector<pl::InEvent>(ev, ev + n);
}
bool is(const std::vector<pl::InEvent>& e, int control) { return e.size() == 1 && e[0].control == control; }

void testDecoding() {
  const pl::Mapping left = pl::profileMapping("ddj200", 0), right = pl::profileMapping("ddj200", 1);
  pl::Decoder d;
  d.reset();
  auto e = feed(d, left, 0x90, 0x0B, 0x7F);
  expect(is(e, pl::CtlPlay) && e[0].down, "DDJ-200 left: play press");
  e = feed(d, left, 0x90, 0x0B, 0x00);
  expect(is(e, pl::CtlPlay) && !e[0].down, "DDJ-200 left: play release (note-on, velocity 0)");
  expect(feed(d, left, 0x91, 0x0B, 0x7F).empty(), "DDJ-200 left ignores the right deck's play");
  expect(is(feed(d, right, 0x91, 0x0B, 0x7F), pl::CtlPlay), "DDJ-200 right: play on channel 2");
  expect(is(feed(d, left, 0x90, 0x0C, 0x7F), pl::CtlCue), "DDJ-200: cue");
  expect(is(feed(d, left, 0x90, 0x47, 0x7F), pl::CtlCensor), "DDJ-200: shift + play = reverse with slip");
  expect(is(feed(d, left, 0x90, 0x58, 0x7F), pl::CtlSync), "DDJ-200: sync");
  e = feed(d, left, 0x90, 0x36, 0x7F);
  expect(is(e, pl::CtlTouch) && e[0].down, "DDJ-200: jog top touched");
  e = feed(d, left, 0xB0, 0x22, 0x41);
  expect(is(e, pl::CtlJogPlatter) && e[0].ticks == 1, "DDJ-200: platter +1 tick (offset binary)");
  e = feed(d, left, 0xB0, 0x22, 0x3D);
  expect(is(e, pl::CtlJogPlatter) && e[0].ticks == -3, "DDJ-200: platter -3 ticks");
  e = feed(d, left, 0xB0, 0x21, 0x42);
  expect(is(e, pl::CtlJogRim) && e[0].ticks == 2, "DDJ-200: rim +2 ticks");
  e = feed(d, left, 0xB0, 0x29, 0x3F);
  expect(is(e, pl::CtlJogSearch) && e[0].ticks == -1, "DDJ-200: shift + jog = search");
  // 14 bit pitch: MSB then LSB
  e = feed(d, left, 0xB0, 0x00, 0x40);
  expect(is(e, pl::CtlPitch) && std::fabs(e[0].value - 0.5f) < 0.01f, "DDJ-200: pitch MSB alone gives the value");
  e = feed(d, left, 0xB0, 0x20, 0x7F);
  expect(is(e, pl::CtlPitch) && e[0].value > 0.5f && e[0].value < 0.51f, "DDJ-200: pitch LSB refines it (14 bit)");
  e = feed(d, left, 0xB0, 0x13, 0x00);
  expect(is(e, pl::CtlVolume) && e[0].value == 0.f, "DDJ-200: channel fader down");
  e = feed(d, left, 0xB0, 0x07, 0x7F);
  expect(is(e, pl::CtlEqHigh) && e[0].value > 0.99f, "DDJ-200: EQ high");
  expect(is(feed(d, left, 0xB6, 0x17, 0x20), pl::CtlFilter), "DDJ-200 left: filter on channel 7 CC 0x17");
  expect(is(feed(d, right, 0xB6, 0x18, 0x20), pl::CtlFilter), "DDJ-200 right: filter on channel 7 CC 0x18");
  expect(feed(d, right, 0xB6, 0x17, 0x20).empty(), "DDJ-200 right ignores the left filter");
  expect(is(feed(d, left, 0xB6, 0x1F, 0x20), pl::CtlXfader), "DDJ-200: crossfader on both decks");
  expect(is(feed(d, left, 0x97, 0x00, 0x7F), pl::CtlHot1), "DDJ-200 left: pad 1 = hot cue 1 (channel 8)");
  expect(is(feed(d, left, 0x97, 0x07, 0x7F), pl::CtlHot8), "DDJ-200 left: pad 8 = hot cue 8");
  expect(is(feed(d, left, 0x98, 0x02, 0x7F), pl::CtlHotClear3), "DDJ-200 left: shift + pad 3 clears hot cue 3");
  expect(is(feed(d, right, 0x99, 0x00, 0x7F), pl::CtlHot1), "DDJ-200 right: pads on channel 10");
  expect(left.ticksPerRev == 128, "DDJ-200: 128 ticks per platter turn");
  bool lamp = false;
  for (const auto& l : right.leds) lamp = lamp || (l.control == pl::CtlPlay && l.status == 0x91 && l.num == 0x0B);
  expect(lamp, "DDJ-200 right: play LED feedback on channel 2");
  // other profiles: two's complement jogs
  const pl::Mapping inp = pl::profileMapping("inpulse200", 0);
  pl::Decoder d2;
  d2.reset();
  e = feed(d2, inp, 0xB1, 0x0A, 0x7E);
  expect(is(e, pl::CtlJogPlatter) && e[0].ticks == -2, "Inpulse 200: jog -2 ticks (two's complement)");
  const pl::Mapping mtx = pl::profileMapping("mixtrackprofx", 1);
  e = feed(d2, mtx, 0xB1, 0x06, 0x03);
  expect(is(e, pl::CtlJogPlatter) && e[0].ticks == 3, "Mixtrack Pro FX right: jog +3 ticks");
  e = feed(d2, mtx, 0x81, 0x00, 0x40);
  expect(is(e, pl::CtlPlay) && !e[0].down, "Mixtrack Pro FX: a note-off releases");
  expect(pl::profileForDevice("PIONEER DDJ-200") == "ddj200", "device name picks the DDJ-200 profile");
  expect(pl::profileForDevice("DJControl Inpulse 200 MIDI") == "inpulse200", "device name picks the Inpulse 200 profile");
  expect(pl::profileForDevice("Some Keyboard").empty(), "unknown devices get no profile");
  expect(pl::profileMapping("generic", 0).empty(), "no controller: nothing mapped");
  // learn
  const pl::Binding b = pl::learnBinding(pl::CtlJogPlatter, 0xB3, 0x10, 0x41, true);
  expect(b.kind == pl::MkCC && b.enc == pl::EncRelOffset && b.channel == 3 && b.num == 0x10, "learn: a relative jog (offset binary)");
  const pl::Binding b2 = pl::learnBinding(pl::CtlJogPlatter, 0xB3, 0x10, 0x7F, true);
  expect(b2.enc == pl::EncRelTwos, "learn: a relative jog (two's complement)");
  const pl::Binding b3 = pl::learnBinding(pl::CtlCue, 0x92, 0x24, 0x7F, false);
  expect(b3.kind == pl::MkNote && b3.num == 0x24, "learn: a button");
  pl::Mapping learned;
  learned.bindings.push_back(b);
  learned.bindings.push_back(b3);
  pl::Decoder d3;
  d3.reset();
  e = feed(d3, learned, 0xB3, 0x10, 0x43);
  expect(is(e, pl::CtlJogPlatter) && e[0].ticks == 3, "learned jog decodes");
  expect(is(feed(d3, learned, 0x92, 0x24, 0x7F), pl::CtlCue), "learned button decodes");
}

// ------------------------------------------------------------------ text forms
void testText() {
  for (const char* id : {"ddj200", "inpulse200", "mixtrackprofx"})
    for (int side = 0; side < 2; ++side) {
      pl::Mapping m = pl::profileMapping(id, side);
      m.device = "Test Device";
      pl::Mapping back;
      expect(pl::mappingFromText(pl::mappingToText(m), back) && back == m, "mapping text round trip");
    }
  pl::Mapping m = pl::profileMapping("ddj200", 1);
  m.profile = "";
  m.bindings.erase(m.bindings.begin());
  const std::string dir = (tmpDir() / "maps").string();
  expect(pl::saveMappingPreset(dir, "My Deck", m), "mapping preset: save");
  const auto files = pl::mappingPresetFiles(dir);
  pl::Mapping back;
  expect(files.size() == 1 && pl::loadMappingPreset(files[0], back) && back == m, "mapping preset: load gives the same mapping");
  // takes: points round trip exactly
  std::vector<pl::TakePoint> pts;
  double x = 1000;
  for (int i = 0; i < 3000; ++i) {
    pl::TakePoint p;
    const double v = i < 1000 ? 32.0 : (i < 2000 ? 32.0 * std::sin(i * 0.01) : -16.0);
    x += v;
    p.pos = std::round(x * 256) / 256;
    p.gain = i % 400 < 50 ? 0.f : 1.f;
    p.flags = static_cast<uint8_t>(i % 700 == 0 ? pl::TfJump | pl::TfMotor : (i < 1000 ? pl::TfMotor : pl::TfHand));
    if (p.flags & pl::TfJump) p.from = std::round((x - 5000.25) * 256) / 256;
    pts.push_back(p);
  }
  std::vector<pl::TakePoint> back2;
  const std::string enc = pl::encodePoints(pts);
  expect(pl::decodePoints(enc, back2) && back2 == pts, "take points: exact round trip");
  std::printf("# 3000 take points: %zu bytes of text\n", enc.size());
  // model
  pl::Model md;
  md.srcFrames = 441000;
  md.srcRate = 44100;
  md.analysed = true;
  for (int i = 0; i < 20; ++i) md.grid.beats.push_back(1000.0 + i * 22050.0);
  md.grid.bpm = 120;
  md.grid.beatsPerBar = 4;
  md.grid.bar1 = 1;
  md.cue = 23050;
  md.hot[2] = 67150;
  md.loopIn = 1000, md.loopOut = 89200;
  md.asked = true;
  md.armed = false;
  md.map = pl::profileMapping("ddj200", 0);
  md.map.device = "DDJ-200";
  pl::Take t;
  t.key = 37;
  t.name = "Chirps";
  t.pts = pts;
  t.startTick = 384;
  t.lengthTicks = 768;
  t.startPos = 1000;
  md.takes.push_back(t);
  pl::Model mb;
  std::string path;
  expect(pl::parseData(pl::formatData("/x/song.wav", md), path, mb) && path == "/x/song.wav" && mb == md, "model text round trip (grid, cues, mapping, takes)");
  expect(pl::pathOf(pl::formatData("/x/song.wav", md)) == "/x/song.wav", "the path is the first line");
  pl::Model bare;
  expect(!pl::parseData("/x/song.wav", path, bare) && path == "/x/song.wav", "a bare path has no model");
}

// ------------------------------------------------------------------ playback
void testPlay(const std::string& path) {
  Deck d = makeDeck(path, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
    s.params[pl::PStartTime] = 0;
  });
  run(d, 4410, 256);
  expect(rms(d.L, 0, d.L.size()) == 0.0, "stopped deck is silent");
  button(d, pl::CtlPlay, true);
  button(d, pl::CtlPlay, false);
  run(d, 44100, 256);
  SamplePtr src = sampleCache().get(path);
  // the deck reads the file at its own speed: the output is the file (from the
  // control step after the press)
  double err = 1;
  for (size_t lag = 4410; lag < 4410 + pl::kStep; ++lag) {
    double e = 0;
    for (size_t i = lag + 512; i < 44100; ++i) e = std::max(e, static_cast<double>(std::fabs(d.L[i] - src->left[i - lag])));
    err = std::min(err, e);
  }
  std::printf("# play error %.2e\n", err);
  expect(err < 1e-5, "play: the output is the file, sample for sample");
  const pl::LiveView v = view(d);
  expect(v.loaded && v.playing && std::fabs(v.speed - 1.0) < 1e-9, "play: speed 1");
  // the stop brakes (0.2 s) instead of cutting
  d.st.params[pl::PStopTime] = 0.2f;
  button(d, pl::CtlPlay, true);
  button(d, pl::CtlPlay, false);
  run(d, 4410, 256);
  const double half = view(d).speed;
  expect(half > 0.35 && half < 0.65, "brake: half speed after half the stop time");
  run(d, 13230, 256);
  expect(std::fabs(view(d).speed) < 1e-6, "brake: stopped after the stop time");
  expect(maxStep(d.L, 48000, d.L.size()) < 0.08, "brake: no click");
  expect(finite(d.L) && finite(d.R), "finite output");
}

// ------------------------------------------------------------------ physics
void testPhysics(const std::string& path) {
  Deck d = makeDeck(path, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
  });
  const double fpt = kSr * pl::kSecondsPerRev / 128.0;  // frames per jog tick
  // hand on the platter, turning it forward at one tick per `fpt` samples = normal speed
  int64_t next = 0;
  int dir = 1;
  bool turning = true;
  auto ev = [&](Deck& dd, int64_t pos, int n) {
    for (; turning && next < pos + n; next += static_cast<int64_t>(fpt))
      if (next >= pos) dd.g->midi(0xB0, 0x22, static_cast<uint8_t>(0x40 + dir), static_cast<int>(next - pos));
  };
  run(d, 256, 256, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
  next = 256;
  double sum = 0;
  int cnt = 0;
  run(d, 22050, 64, [&](Deck& dd, int64_t pos, int n) {
    ev(dd, pos, n);
    if (pos > 13000) sum += view(dd).speed, ++cnt;
  });
  const double fwd = sum / cnt;
  expect(view(d).touched, "touch: the hand is on the platter");
  expect(fwd > 0.9 && fwd < 1.1, "hand: ticks at the platter's own rate play at normal speed");
  std::printf("# hand forward speed %.3f\n", fwd);
  dir = -1;
  sum = 0, cnt = 0;
  run(d, 22050, 64, [&](Deck& dd, int64_t pos, int n) {
    ev(dd, pos, n);
    if (dd.pos > 22050 + 13000) sum += view(dd).speed, ++cnt;
  });
  const double back = sum / cnt;
  expect(back < -0.9 && back > -1.1, "hand: turning back plays backwards");
  turning = false;
  run(d, 11025, 64);
  expect(std::fabs(view(d).speed) < 0.05, "hand: holding the platter still stops the sound");
  // throw: release while the hand moves -> the record keeps turning (motor off: it brakes)
  // motor: play spins up over the start time
  run(d, 64, 64, [](Deck& dd, int64_t, int) {
    dd.g->midi(0x90, 0x36, 0x00, 0);
    dd.g->midi(0x90, 0x0B, 0x7F, 1);
    dd.g->midi(0x90, 0x0B, 0x00, 2);
  });
  run(d, static_cast<int64_t>(0.04 * kSr), 64);
  const double up = view(d).speed;
  expect(up > 0.3 && up < 0.7, "motor: half speed after half the start time");
  run(d, static_cast<int64_t>(0.2 * kSr), 64);
  expect(std::fabs(view(d).speed - 1.0) < 1e-6, "motor: full speed after the start time");
  // a rim nudge bends the speed for a moment (rim moves right after a release are the platter coasting: ignored)
  run(d, static_cast<int64_t>(0.3 * kSr), 64);
  run(d, 64, 64, [](Deck& dd, int64_t, int) {
    for (int k = 0; k < 8; ++k) dd.g->midi(0xB0, 0x21, 0x44, k);
  });
  run(d, 256, 64);
  const double bent = view(d).speed;
  run(d, static_cast<int64_t>(0.6 * kSr), 64);
  expect(bent > 1.02 && std::fabs(view(d).speed - 1.0) < 0.01, "rim: a nudge speeds up and settles back");
  // a scratch with the motor running: touch stops it, release lets the motor catch it
  run(d, 64, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
  run(d, static_cast<int64_t>(0.25 * kSr), 64);
  expect(std::fabs(view(d).speed) < 0.05, "touch while playing: the hand holds the record");
  run(d, 64, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x00, 0); });
  run(d, static_cast<int64_t>(0.2 * kSr), 64);
  expect(std::fabs(view(d).speed - 1.0) < 1e-3, "release: the motor takes the record back to speed");
  expect(finite(d.L), "physics: finite output");
  expect(maxStep(d.L, 0, d.L.size()) < 0.12, "physics: no clicks anywhere");
  // vinyl mode off: the platter top nudges instead of scratching
  d.st.params[pl::PVinyl] = 0;
  run(d, 64, 64, [](Deck& dd, int64_t, int) {
    dd.g->midi(0x90, 0x36, 0x7F, 0);
    for (int k = 0; k < 8; ++k) dd.g->midi(0xB0, 0x22, 0x44, k + 1);
  });
  run(d, 256, 64);
  expect(view(d).speed > 1.02 && !view(d).touched, "CDJ mode: the top nudges, the record keeps playing");
}

// ------------------------------------------------------------------ scratch quality
void testScratch(const std::string& sinePath) {
  Deck d = makeDeck(sinePath, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
  });
  d.m.cue = 0;
  run(d, 64, 64, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdHandDown); });
  // the mouse moves the record back and forth (2 Hz, about 1.1x peak speed); direction changes 4 times a second
  run(d, static_cast<int64_t>(2 * kSr), 64, [](Deck& dd, int64_t pos, int) {
    const double t = static_cast<double>(pos) / kSr;
    cmd(dd, pl::CmdHandMove, 0, 0.05 * std::sin(2 * kPi * 2 * t));
  });
  const double step = maxStep(d.L, 4410, d.L.size());
  const double level = rms(d.L, 4410, d.L.size());
  std::printf("# scratch: max step %.4f, rms %.3f\n", step, level);
  expect(level > 0.08, "scratch: the record sounds while the hand moves it");
  expect(step < 0.045, "scratch: no click on any direction change (sine 220 Hz, peak step 0.0157 at 1x)");
  expect(finite(d.L), "scratch: finite");
}

void testResampler(const std::string& highPath) {
  // an 18 kHz tone read at 1.5x would fold to 17.1 kHz: the band-limited read removes it
  Deck d = makeDeck(highPath, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
    s.params[pl::PStartTime] = 0;
    s.params[pl::PPitchRange] = pl::Range50;
    s.params[pl::PPitch] = 1;
  });
  button(d, pl::CtlPlay, true);
  run(d, 22050, 256);
  const double fast = rms(d.L, 4410, 22050);
  Deck u = makeDeck(highPath, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
    s.params[pl::PStartTime] = 0;
  });
  button(u, pl::CtlPlay, true);
  run(u, 22050, 256);
  const double unity = rms(u.L, 4410, 22050);
  std::printf("# 18 kHz at 1.5x: %.1f dB below 1x\n", 20 * std::log10(fast / unity + 1e-12));
  expect(unity > 0.2 && fast < unity * 0.03, "resampler: content above the new Nyquist is removed (< -30 dB)");
}

void testKeyLock(const std::string& sinePath) {
  auto make = [&](bool lock) {
    Deck d = makeDeck(sinePath, 120, [lock](PluginState& s, pl::Model&) {
      s.params[pl::PSync] = 0;
      s.params[pl::PKeyLock] = lock ? 1.f : 0.f;
      s.params[pl::PStartTime] = 0;
      s.params[pl::PPitchRange] = pl::Range16;
      s.params[pl::PPitch] = 0.5f;  // +8 %
    });
    d.g->prepareHost(d.st, kSr);
    button(d, pl::CtlPlay, true);
    run(d, 44100, 256);
    return d;
  };
  Deck a = make(false), b = make(true);
  const double fa = frequency(a.L, 8820, 44100), fb = frequency(b.L, 8820, 44100);
  std::printf("# +8 %%: %.1f Hz without key lock, %.1f Hz with\n", fa, fb);
  expect(std::fabs(fa / 220.0 - 1.08) < 0.01, "pitch fader +8 %: the tone is 8 % higher");
  expect(std::fabs(fb / 220.0 - 1.0) < 0.01, "key lock: the tone keeps its pitch");
  expect(view(b).keylockActive, "key lock engine in use");
  expect(std::fabs(view(b).speed - 1.08) < 1e-3, "key lock: the tempo still follows the fader");
  expect(maxStep(b.L, 8820, 44100) < 0.05 && finite(b.L), "key lock: clean output");
}

// ------------------------------------------------------------------ sync
void testSync(const std::string& path100) {
  Deck d = makeDeck(path100, 100, [](PluginState& s, pl::Model&) { s.params[pl::PKeyLock] = 0; });
  d.ctx.tempo = 125;
  d.ctx.playing = true;
  run(d, 4410, 256);
  button(d, pl::CtlPlay, true);
  run(d, static_cast<int64_t>(3 * kSr), 256);
  const pl::LiveView v = view(d);
  expect(std::fabs(v.speed - 1.25) < 0.01, "sync: a 100 BPM track plays at 1.25x in a 125 BPM project");
  expect(std::fabs(v.tempo - 125) < 0.01, "sync: the deck shows the project tempo");
  const double songBeat = d.ctx.tick / d.ctx.ppq + 256.0 / kSr * 125 / 60;  // end of the last block
  const double deckBeat = pl::beatAt(d.m, v.pos);
  double err = (deckBeat - std::floor(deckBeat)) - (songBeat - std::floor(songBeat));
  err -= std::round(err);
  std::printf("# sync phase error %.4f beats\n", err);
  expect(std::fabs(err) < 0.02, "sync: the deck's beats land on the project's beats");
  // the unlock key: back to the file's own tempo (pitch fader at 0)
  d.st.params[pl::PSync] = 0;
  run(d, static_cast<int64_t>(kSr), 256);
  expect(std::fabs(view(d).speed - 1.0) < 1e-6, "unlocked: the track's own tempo");
}

// ------------------------------------------------------------------ cues, loops, notes, block size
void performance(Deck& d, int64_t pos, int n) {
  auto at = [&](int64_t f, const std::function<void(int)>& fn) {
    if (f >= pos && f < pos + n) fn(static_cast<int>(f - pos));
  };
  at(1000, [&](int o) { d.g->noteOn(1, 60, 1.f, o); });   // hot cue 1 (unset: the cue)
  at(20000, [&](int o) { d.g->noteOff(1, o); });           // gate: stops
  at(25000, [&](int o) { d.g->noteOn(2, 62, 1.f, o); });  // hot cue 3 (unset: 2 bars after the cue)
  at(30000, [&](int o) { d.g->midi(0x90, 0x36, 0x7F, o); });
  for (int k = 0; k < 40; ++k) at(30100 + k * 300, [&](int o) { d.g->midi(0xB0, 0x22, k < 20 ? 0x43 : 0x3C, o); });
  at(43000, [&](int o) { d.g->midi(0x90, 0x36, 0x00, o); });
  at(44000, [&](int o) { d.g->midi(0xB0, 0x13, 0x00, o); });  // fader cut
  at(46000, [&](int o) { d.g->midi(0xB0, 0x13, 0x7F, o); });
  at(50000, [&](int o) { d.g->noteOff(2, o); });
}

void testBlockSize(const std::string& path) {
  std::vector<std::vector<float>> outs;
  for (int block : {64, 100, 512, 1024}) {
    Deck d = makeDeck(path, 120);
    d.g->prepareHost(d.st, kSr);
    run(d, 60000, block, performance);
    outs.push_back(d.L);
  }
  double diff = 0;
  for (size_t k = 1; k < outs.size(); ++k)
    for (size_t i = 0; i < outs[0].size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(outs[k][i] - outs[0][i])));
  std::printf("# block-size difference %.2e\n", diff);
  expect(diff < 1e-6, "block-size invariant (notes, jog, fader, key lock)");
  expect(rms(outs[0], 1000, 20000) > 0.05, "a hot cue key plays");
  expect(rms(outs[0], 21000, 24000) < 1e-3, "gate: the key's release stops the deck");
}

void testCues(const std::string& path) {
  Deck d = makeDeck(path, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PStartTime] = 0;
    s.params[pl::PKeyLock] = 0;
  });
  run(d, 256, 256, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdSeek, 0, 30000.0); });
  // stopped away from the cue: CUE sets the cue here (quantize: the nearest beat)
  button(d, pl::CtlCue, true);
  button(d, pl::CtlCue, false);
  run(d, 256, 256);
  expect(std::fabs(view(d).cue - 22050) < 1, "cue: set on the nearest beat (quantize)");
  // hold CUE: plays from the cue, release returns
  button(d, pl::CtlCue, true);
  run(d, 8820, 256);
  expect(view(d).playing && view(d).pos > 22050 + 4000, "cue held: previews from the cue");
  button(d, pl::CtlCue, false);
  run(d, 512, 256);
  expect(!view(d).playing && std::fabs(view(d).pos - 22050) < 1, "cue released: back to the cue, stopped");
  // hot cue: an empty pad stores, a set pad jumps
  run(d, 256, 256, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdSeek, 0, 66150.0); });
  button(d, pl::CtlHot1 + 1, true);
  button(d, pl::CtlHot1 + 1, false);
  run(d, 256, 256);
  expect(std::fabs(view(d).hot[1] - 66150) < 1, "hot cue 2 stored");
  pl::Marks mk;
  pl::marks(d.g.get(), mk);
  expect(mk.seq > 0 && std::fabs(mk.hot[1] - 66150) < 1, "the deck reports the marks it set (the editor saves them)");
  button(d, pl::CtlPlay, true);
  run(d, 4410, 256);
  d.st.params[pl::PQuantize] = 0;  // (quantized jumps keep the beat phase)
  button(d, pl::CtlHot1 + 1, true);
  run(d, 256, 256);
  expect(view(d).pos > 66150 && view(d).pos < 66150 + 2000, "hot cue: jumps while playing");
  // auto loop (4 beats) wraps
  button(d, pl::CtlLoopAuto, true);
  run(d, static_cast<int64_t>(3 * kSr), 256);
  const pl::LiveView v = view(d);
  expect(v.loopOn && v.pos >= v.loopIn && v.pos < v.loopOut && std::fabs(v.loopOut - v.loopIn - 4 * 22050) < 2, "auto loop: 4 beats, the head stays inside");
  expect(maxStep(d.L, 0, d.L.size()) < 0.2, "jumps and loop wraps crossfade (no clicks)");
}

// ------------------------------------------------------------------ takes
void testTakes(const std::string& path, bool keyLock) {
  pl::Take take;
  std::vector<float> live;
  const int64_t frames = 110000;
  {
    Deck d = makeDeck(path, 120, [keyLock](PluginState& s, pl::Model&) { s.params[pl::PKeyLock] = keyLock ? 1.f : 0.f; });
    d.g->prepareHost(d.st, kSr);
    d.ctx.playing = true;
    d.ctx.recording = true;
    run(d, frames, 128, [](Deck& dd, int64_t pos, int n) {
      auto at = [&](int64_t f, const std::function<void(int)>& fn) {
        if (f >= pos && f < pos + n) fn(static_cast<int>(f - pos));
      };
      at(10, [&](int o) { dd.g->midi(0x90, 0x0B, 0x7F, o); });
      at(20000, [&](int o) { dd.g->midi(0x90, 0x36, 0x7F, o); });
      for (int k = 0; k < 120; ++k) at(20100 + k * 400, [&](int o) { dd.g->midi(0xB0, 0x22, (k / 15) % 2 ? 0x3D : 0x43, o); });
      at(40000, [&](int o) { dd.g->midi(0xB0, 0x13, 0x00, o); });
      at(42000, [&](int o) { dd.g->midi(0xB0, 0x13, 0x7F, o); });
      at(70000, [&](int o) { dd.g->midi(0x90, 0x36, 0x00, o); });
      at(90000, [&](int o) { dd.g->midi(0x90, 0x58, 0x7F, o); });  // sync off: back to the file's tempo
    });
    d.ctx.recording = false;
    run(d, 256, 256);
    pl::TakeBuilder tb;
    std::vector<pl::CaptureEvent> evs(40000);
    bool done = false;
    for (int n; (n = pl::drainCapture(d.g.get(), evs.data(), static_cast<int>(evs.size()))) > 0;)
      for (int i = 0; i < n; ++i) done = tb.feed(evs[static_cast<size_t>(i)]) || done;
    expect(done, keyLock ? "take captured (key lock)" : "take captured");
    take = tb.take;
    live = d.L;
  }
  expect(take.startTick == 0 && take.pts.size() >= static_cast<size_t>(frames / pl::kStep), "take: starts on the beat, covers the recording");
  take.key = pl::kTakeKeyFirst;
  // the take replays on its key, with no controller
  Deck r = makeDeck(path, 120, [&](PluginState& s, pl::Model& m) {
    s.params[pl::PKeyLock] = keyLock ? 1.f : 0.f;
    m.takes.push_back(take);
  });
  // the take survives the model text
  pl::Model back;
  std::string p;
  expect(pl::parseData(r.st.data, p, back) && back.takes.size() == 1 && back.takes[0] == take, "take: stored in the project data");
  r.g->prepareHost(r.st, kSr);
  r.ctx.playing = true;
  run(r, frames, 300, [](Deck& dd, int64_t pos, int) {
    if (pos == 0) dd.g->noteOn(7, pl::kTakeKeyFirst, 1.f, 0);
  });
  const size_t n = static_cast<size_t>(take.pts.size()) * pl::kStep;
  double diff = 0;
  size_t first = n;
  for (size_t i = 0; i < std::min({n, live.size(), r.L.size()}); ++i) {
    const double e = std::fabs(live[i] - r.L[i]);
    if (e > 1e-5 && first == n) first = i;
    diff = std::max(diff, e);
  }
  if (first < n) std::printf("# first difference at sample %zu: %.4f vs %.4f\n", first, live[first], r.L[first]);
  std::printf("# take replay difference %.2e over %zu samples\n", diff, n);
  expect(diff < 1e-5, keyLock ? "take: replays sample for sample (key lock)" : "take: replays sample for sample (scratches, fader cuts)");
  expect(rms(live, 20000, 70000) > 0.02, "take: the scratch sounds");
}

// ------------------------------------------------------------------ audio thread
void testNoAlloc(const std::string& path) {
  Deck d = makeDeck(path, 120);
  d.g->prepareHost(d.st, kSr);
  d.ctx.playing = true;
  run(d, 512, 256);
  gAllocs = 0;
  run(d, 60000, 256, performance, true);
  std::printf("# allocations during process: %ld\n", gAllocs.load());
  expect(gAllocs.load() == 0, "no allocation in process() (play, scratch, key lock, cues)");
}

// ------------------------------------------------------------------ project, old effect
void testProject(const std::string& path) {
  Song s;
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState(pl::kType);
  pl::Model m;
  SamplePtr src = sampleCache().get(path);
  m = pl::defaultModel(*src, gl::packs::seltmann::flatGrid(*src, 120), true);
  m.map = pl::profileMapping("generic", 0);
  m.map.bindings.push_back(pl::learnBinding(pl::CtlPlay, 0x93, 0x10, 0x7F, false));
  m.asked = true;
  c.plugin.data = pl::formatData(path, m);
  c.plugin.params[pl::PPitch] = 0.25f;
  s.channels.push_back(c);
  const std::string proj = (tmpDir() / "deck.gridline.json").string();
  std::string err;
  expect(saveProject(proj, s, &err), "project with a deck saves");
  Song t;
  expect(loadProject(proj, t, &err), "project with a deck loads");
  const Channel* tc = t.channel(c.id);
  expect(tc && tc->plugin.data == c.plugin.data && tc->plugin.params == c.plugin.params, "save / load: mapping, grid and parameters come back");
  // the old Turntable effect is untouched
  const PluginInfo* old = findPlugin("gl.scratcher");
  expect(old && old->kind == PluginKind::Effect && old->name == "Turntable", "the old gl.scratcher effect is still there");
  auto fx = createEffect("gl.scratcher");
  expect(fx != nullptr, "gl.scratcher is created");
  if (fx && old) {
    PluginState st = defaultPluginState("gl.scratcher");
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    std::vector<float> L(4096), R(4096);
    for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.3 * std::sin(i * 0.05));
    fx->process(st, ctx, L.data(), R.data(), 4096);
    expect(finite(L) && finite(R), "gl.scratcher still processes");
  }
}

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  const std::string music120 = writeTrack("music120.wav", 12, [](int64_t i, int c) { return music(i, c, 120); });
  const std::string music100 = writeTrack("music100.wav", 12, [](int64_t i, int c) { return music(i, c, 100); });
  const std::string sine = writeTrack("sine220.wav", 8, [](int64_t i, int) { return 0.5 * std::sin(2 * kPi * 220 * static_cast<double>(i) / kSr); });
  const std::string high = writeTrack("sine18k.wav", 2, [](int64_t i, int) { return 0.5 * std::sin(2 * kPi * 18000 * static_cast<double>(i) / kSr); });
  auto want = [&](const char* n) { return only.empty() || only == n; };
  if (want("catalogue")) testCatalogue();
  if (want("decode")) testDecoding();
  if (want("text")) testText();
  if (want("play")) testPlay(music120);
  if (want("physics")) testPhysics(music120);
  if (want("scratch")) testScratch(sine);
  if (want("resampler")) testResampler(high);
  if (want("keylock")) testKeyLock(sine);
  if (want("sync")) testSync(music100);
  if (want("cues")) testCues(music120);
  if (want("blocks")) testBlockSize(music120);
  if (want("takes")) {
    testTakes(music120, false);
    testTakes(music120, true);
  }
  if (want("alloc")) testNoAlloc(music120);
  if (want("project")) testProject(music120);
  return finish();
}
