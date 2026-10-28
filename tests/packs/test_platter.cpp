// Gridline — "platter" pack tests: Platter (gl.platter), the two-deck DJ setup.
// Controller decoding from synthetic bytes (DDJ-200 one side and the whole
// controller, Inpulse 200, Mixtrack Pro FX, learn), mappings and models as text
// and preset files, the platter physics (jog and mouse scratching, motor start / brake,
// throws), click-free direction changes, band-limited reads at speed, key lock,
// tempo sync (to the project and deck to deck) and beat phase, the mixer
// (channels, crossfader assign / curve / cut / reverse, master), block-size
// invariance, no allocation on the audio thread (two decks, key lock,
// scratching), takes of both decks and the mixer moves that replay sample for
// sample, single-deck (J-150) states, project save / load, and the old
// Turntable effect.
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
#include "core/ProjectInterop.h"
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

// A Platter setup (both decks and the mixer); the tests mostly play deck A.
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

// A deck model on `path` with a flat grid at `bpm` (cue at 0).
pl::DeckModel flatDeck(const std::string& path, double bpm) {
  SamplePtr src = sampleCache().get(path);
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, bpm), true);
  dm.cue = 0;
  return dm;
}

// A setup with `path` on deck A (and `pathB` on deck B when given), flat grids,
// and the DDJ-200 mapped as a whole (left side deck A, right side deck B).
Deck makeDeck(const std::string& path, double bpm, const std::function<void(PluginState&, pl::Model&)>& tweak = nullptr,
              const std::string& pathB = "", double bpmB = 120) {
  Deck d;
  d.path = path;
  pl::loadTrack(d.m, 0, path, flatDeck(path, bpm));
  if (!pathB.empty()) pl::loadTrack(d.m, 1, pathB, flatDeck(pathB, bpmB));
  d.m.map = pl::profileMapping("ddj200", pl::kSideBoth);
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

pl::LiveView view(const Deck& d, int deck = 0) {
  pl::LiveView v;
  pl::liveView(d.g.get(), deck, v);
  return v;
}
void cmd(Deck& d, uint8_t kind, uint8_t a = 0, double v = 0, double w = 0, int deck = 0) {
  pl::Cmd c;
  c.kind = kind;
  c.a = a;
  c.deck = static_cast<uint8_t>(deck);
  c.v = v;
  c.w = w;
  pl::command(d.g.get(), c);
}
void button(Deck& d, int control, bool down, int deck = 0) { cmd(d, pl::CmdButton, static_cast<uint8_t>(control), down ? 1 : 0, 0, deck); }
// Captured take of a recording setup (after recording stopped).
bool drainTake(Deck& d, pl::Take& out) {
  pl::TakeBuilder tb;
  std::vector<pl::CaptureEvent> evs(40000);
  bool done = false;
  for (int n; (n = pl::drainCapture(d.g.get(), evs.data(), static_cast<int>(evs.size()))) > 0;)
    for (int i = 0; i < n; ++i) done = tb.feed(evs[static_cast<size_t>(i)]) || done;
  out = tb.take;
  return done;
}
double maxDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
  double m = 0;
  for (size_t i = from; i < std::min({to, a.size(), b.size()}); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

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
  // parameter ids: deck A keeps J-150's ids, deck B and the new mixer values follow
  expect(pl::PCount == 56 && pl::PPosA == 48 && pl::PDeckB == 23 && pl::PJogA == 46,
         "parameter ids: 23 from the single deck, deck B and the mixer after them, then the jog sensitivities");
  for (int d = 0; d < pl::DCount; ++d) {
    int dd = -1;
    expect(pl::deckOfParam(pl::deckParam(0, d), dd) == 0 && dd == d, "deck A parameter ids map back");
    expect(pl::deckOfParam(pl::deckParam(1, d), dd) == 1 && dd == d, "deck B parameter ids map back");
    expect(p->params[static_cast<size_t>(pl::deckParam(1, d))].name.rfind("B ", 0) == 0, "deck B parameters are named for deck B");
  }
  int dd = -1;
  for (int k : {pl::PXfader, pl::PXfCurve, pl::PFaderCurve, pl::PNoteMode, pl::PSyncTo, pl::PMaster, pl::PXfCut, pl::PXfReverse})
    expect(pl::deckOfParam(k, dd) < 0, "mixer parameters belong to no deck");
  // J-154: each deck's jog sensitivity (appended after the mixer's ids)
  for (int d = 0; d < 2; ++d) {
    const int k = pl::deckParam(d, pl::DJog);
    expect(k == (d == 0 ? pl::PJogA : pl::PJogB) && pl::deckOfParam(k, dd) == d && dd == pl::DJog, "jog sensitivity ids map to their deck");
    const ParamDesc& pd = p->params[static_cast<size_t>(k)];
    expect(std::fabs(pd.def - 0.8) < 1e-9 && pd.min == 0.25 && pd.max == 2.0 && pd.name == std::string(d == 0 ? "A " : "B ") + "jog sensitivity",
           "jog sensitivity: 0.25x .. 2x, a little under one turn per turn by default (0.8x)");
  }
  expect(!p->noteFxPanel && p->editorMinWidth > 0, "no note processors panel; the editor reflows in a narrower window");
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

// The whole controller: the left side drives deck A, the right side deck B, the mixer once.
void testDecodingBoth() {
  const pl::Mapping both = pl::profileMapping("ddj200", pl::kSideBoth);
  expect(both.side == pl::kSideBoth, "DDJ-200: the whole controller");
  int xf = 0, xfDeck = 0;
  for (const auto& b : both.bindings)
    if (b.control == pl::CtlXfader) ++xf, xfDeck += b.deck;
  expect(xf == 1 && xfDeck == 0, "the crossfader is mapped once (a mixer control)");
  int init = 0;
  for (const auto& i : both.init) init += i.empty() ? 0 : 1;
  expect(init == 1, "the set-up message is sent once");
  pl::Decoder d;
  d.reset();
  auto e = feed(d, both, 0x90, 0x0B, 0x7F);
  expect(is(e, pl::CtlPlay) && e[0].deck == 0, "left play: deck A");
  e = feed(d, both, 0x91, 0x0B, 0x7F);
  expect(is(e, pl::CtlPlay) && e[0].deck == 1, "right play: deck B");
  e = feed(d, both, 0xB1, 0x22, 0x43);
  expect(is(e, pl::CtlJogPlatter) && e[0].deck == 1 && e[0].ticks == 3, "right jog: deck B");
  e = feed(d, both, 0xB6, 0x17, 0x20);
  expect(is(e, pl::CtlFilter) && e[0].deck == 0, "left filter: deck A");
  e = feed(d, both, 0xB6, 0x18, 0x20);
  expect(is(e, pl::CtlFilter) && e[0].deck == 1, "right filter: deck B");
  e = feed(d, both, 0xB6, 0x1F, 0x7F);
  expect(is(e, pl::CtlXfader) && e[0].deck == 0, "crossfader: one event for the mixer");
  e = feed(d, both, 0x99, 0x02, 0x7F);
  expect(is(e, pl::CtlHot3) && e[0].deck == 1, "right pads: deck B's hot cues");
  // shift is per deck
  feed(d, both, 0x91, 0x3F, 0x7F);
  expect(d.shift(1) && !d.shift(0), "shift on the right holds deck B's shift only");
  feed(d, both, 0x91, 0x3F, 0x00);
  expect(!d.shift(1), "shift released");
  // LEDs go to the side of their deck
  bool lampB = false, lampA = false;
  for (const auto& l : both.leds) {
    lampA = lampA || (l.control == pl::CtlPlay && l.deck == 0 && l.status == 0x90);
    lampB = lampB || (l.control == pl::CtlPlay && l.deck == 1 && l.status == 0x91);
  }
  expect(lampA && lampB, "play LEDs: deck A on the left, deck B on the right");
  // learn: deck controls keep their deck, mixer controls belong to no deck
  expect(pl::learnBinding(pl::CtlCue, 0x92, 0x24, 0x7F, false, 1).deck == 1, "learn on deck B: a deck B binding");
  expect(pl::learnBinding(pl::CtlXfader, 0xB2, 0x24, 0x40, false, 1).deck == 0, "learn the crossfader: a mixer binding");
  expect(pl::controlInfo(pl::CtlXfader).mixer && pl::controlInfo(pl::CtlMaster).mixer && !pl::controlInfo(pl::CtlVolume).mixer,
         "the crossfader and the master are mixer controls");
  expect(pl::controlParam(pl::CtlVolume, 1) == pl::deckParam(1, pl::DVolume) && pl::controlParam(pl::CtlXfader, 1) == pl::PXfader,
         "controls move their deck's parameter (mixer controls the mixer's)");
  // the mapping text keeps the deck of every binding and LED
  pl::Mapping back;
  expect(pl::mappingFromText(pl::mappingToText(both), back) && back == both, "whole-controller mapping text round trip");
  const std::string dir = (tmpDir() / "maps2").string();
  expect(pl::saveMappingPreset(dir, "Both Sides", both), "whole-controller preset: save");
  const auto files = pl::mappingPresetFiles(dir);
  pl::Mapping back2;
  expect(!files.empty() && pl::loadMappingPreset(files[0], back2) && back2 == both, "whole-controller preset: load");
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
  // take moves round trip exactly
  std::vector<pl::TakeMove> mv;
  for (int i = 0; i < 200; ++i) mv.push_back(pl::TakeMove{static_cast<uint32_t>(i * 37), pl::moveParams()[static_cast<size_t>(i % pl::kMoveParams)], static_cast<float>(std::sin(i * 0.3) * 7.0)});
  std::vector<pl::TakeMove> mvb;
  expect(pl::decodeMoves(pl::encodeMoves(mv), mvb) && mvb == mv, "take moves: exact round trip");
  // model (both decks)
  pl::Model md;
  md.files = {"/x/song.wav", "/x/other track.wav", "/x/old.wav"};
  pl::DeckModel& a = md.deck[0];
  a.file = 0;
  a.srcFrames = 441000;
  a.srcRate = 44100;
  a.analysed = true;
  for (int i = 0; i < 20; ++i) a.grid.beats.push_back(1000.0 + i * 22050.0);
  a.grid.bpm = 120;
  a.grid.beatsPerBar = 4;
  a.grid.bar1 = 1;
  a.cue = 23050;
  a.hot[2] = 67150;
  a.loopIn = 1000, a.loopOut = 89200;
  pl::DeckModel& b = md.deck[1];
  b.file = 1;
  b.srcFrames = 882000;
  b.srcRate = 48000;
  b.analysed = false;
  for (int i = 0; i < 9; ++i) b.grid.beats.push_back(500.25 + i * 22000.5 + (i == 4 ? 3.0 : 0.0));  // an irregular grid
  b.grid.bpm = 128;
  b.grid.beatsPerBar = 4;
  b.grid.bar1 = 2;
  b.grid.key = 9;
  b.grid.minor = true;
  b.cue = 500.25;
  b.hot[0] = 44001, b.hot[7] = 600000;
  md.asked = true;
  md.armed = false;
  md.map = pl::profileMapping("ddj200", pl::kSideBoth);
  md.map.device = "DDJ-200";
  pl::Take t;
  t.key = 37;
  t.name = "Chirps";
  t.startTick = 384;
  t.lengthTicks = 768;
  t.s[0].on = true;
  t.s[0].file = 0;
  t.s[0].pts = pts;
  t.s[0].startPos = 1000;
  t.s[1].on = true;
  t.s[1].file = 2;  // a track no deck holds any more
  t.s[1].pts = pts;
  t.s[1].startPos = 2000.5;
  t.s[1].startRate = 1.25;
  t.s[1].startGain = 0.5f;
  t.moves = mv;
  md.takes.push_back(t);
  pl::Model mb;
  std::string path;
  expect(pl::parseData(pl::formatData("/x/song.wav", md), path, mb) && path == "/x/song.wav" && mb == md,
         "model text round trip (both decks, grids, cues, mapping, takes of both decks and the mixer)");
  expect(pl::pathOf(pl::formatData("/x/song.wav", md)) == "/x/song.wav", "the path is deck A's track (the first line)");
  md.layout = pl::LayoutOne;
  expect(pl::parseData(pl::formatData("/x/song.wav", md), path, mb) && mb.layout == pl::LayoutOne && !mb.twoDecks(), "the one-deck layout is kept");
  // a new first line (audio dropped on the channel): deck A's new track, deck B and the takes stay
  expect(pl::parseData(pl::formatData("/x/dropped.wav", md), path, mb) && mb.trackOf(0) == "/x/dropped.wav" && mb.trackOf(1) == "/x/other track.wav" &&
             mb.deck[1] == md.deck[1] && mb.takes.size() == 1 && mb.files[static_cast<size_t>(mb.takes[0].s[0].file)] == "/x/song.wav",
         "a dropped file replaces deck A's track only");
  // files: pruned when nothing refers to them
  pl::Model pm = md;
  pm.takes.clear();
  pl::pruneFiles(pm);
  expect(pm.files.size() == 2 && pm.trackOf(0) == "/x/song.wav" && pm.trackOf(1) == "/x/other track.wav", "unused files are pruned");
  pl::loadTrack(pm, 1, "/x/song.wav", pl::DeckModel{});
  expect(pm.files.size() == 1 && pm.deck[1].file == 0, "both decks on one track share its file");
  pl::Model bare;
  expect(!pl::parseData("/x/song.wav", path, bare) && path == "/x/song.wav" && bare.trackOf(0) == "/x/song.wav" && bare.trackOf(1).empty(),
         "a bare path has no model (the track on deck A)");
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
    s.params[pl::PJogA] = 1;  // the record turns as far as the wheel
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
  // a rim nudge bends the speed for a moment (rim moves within 0.9 s of a release are the platter coasting: ignored)
  run(d, static_cast<int64_t>(0.7 * kSr), 64);
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

// ------------------------------------------------------------------ scratching (J-154, J-155)
// The ramp track's left channel is its own position (right: a constant), so the
// output tells the read position at every sample: p = L / R * kRampScale.
constexpr double kRampN = 2.0 * kSr, kRampTop = 0.9;
constexpr double kRampScale = 0.5 * kRampN / kRampTop;
std::vector<double> rampPos(const Deck& d, size_t from, size_t to) {
  std::vector<double> p;
  for (size_t i = from; i < to; ++i) p.push_back(std::fabs(d.R[i]) > 2e-3 ? d.L[i] / d.R[i] * kRampScale : NAN);
  return p;
}
Deck rampDeck(const std::string& rampPath, float sens = 1.0f) {
  Deck d = makeDeck(rampPath, 120, [sens](PluginState& s, pl::Model& m) {
    s.params[pl::PSync] = 0;
    s.params[pl::PKeyLock] = 0;
    s.params[pl::PJogA] = sens;
    m.deck[0].cue = 0.5 * kRampN;  // the hand starts in the middle of the ramp
  });
  d.ctx.offline = true;  // (input lands at the block's start: no wall clock in the test)
  return d;
}
// Jog ticks (DDJ-200, 128 per turn) for a hand at `hand(t)` frames, each tick on its own sample.
void jogTicks(Deck& dd, int64_t pos, int n, const std::function<double(double)>& hand, long& sent, double fpt) {
  for (int o = 0; o < n; ++o) {
    const long want = static_cast<long>(std::floor(hand(static_cast<double>(pos + o) / kSr) / fpt));
    if (want != sent) {
      const long dlt = std::clamp(want - sent, -63L, 63L);
      dd.g->midi(0xB0, 0x22, static_cast<uint8_t>(0x40 + dlt), o);
      sent += dlt;
    }
  }
}

// The deck's published speed (x normal) after every block of `block` samples.
std::vector<double> speeds(Deck& d, int64_t frames, int block, const std::function<void(Deck&, int64_t, int)>& ev = nullptr) {
  std::vector<double> v;
  for (int64_t k = 0; k < frames; k += block) {
    run(d, block, block, ev);
    v.push_back(view(d).speed);
  }
  return v;
}
double speedAt(const std::vector<double>& v, double sec, int block = 64) {
  return v[std::min(v.size() - 1, static_cast<size_t>(sec * kSr / block))];
}

void testHand(const std::string& rampPath) {
  const double fpt = kSr * pl::kSecondsPerRev / 128.0;
  // 1. jog ticks -> speed (J-155, after Mixxx's alpha-beta jog filter): a touched
  //    wheel turning steadily at 0.5x is followed smoothly, about 20-35 ms behind
  //    (the record has some mass), and settles on the wheel's speed
  {
    Deck d = rampDeck(rampPath);
    run(d, 256, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
    long sent = 0;
    const int64_t t0 = d.pos;
    auto hand = [&](double t) { return 0.5 * kSr * std::max(0.0, t - static_cast<double>(t0) / kSr); };
    const auto v = speeds(d, static_cast<int64_t>(0.3 * kSr), 64, [&](Deck& dd, int64_t pos, int n) { jogTicks(dd, pos, n, hand, sent, fpt); });
    double top = 0;
    for (size_t i = static_cast<size_t>(0.15 * kSr / 64); i < v.size(); ++i) top = std::max(top, v[i]);
    std::printf("# jog 0.5x from rest: %.3f after 10 ms, %.3f after 30 ms, %.3f after 100 ms, %.3f after 300 ms (top %.3f)\n", speedAt(v, 0.01),
                speedAt(v, 0.03), speedAt(v, 0.1), v.back(), top);
    // (at 0.5x the first tick comes after 28 ms; Mixxx's filter overshoots a step by about 15%)
    expect(speedAt(v, 0.01) == 0.0 && speedAt(v, 0.03) > 0.02 && speedAt(v, 0.03) < 0.3, "jog: the speed follows the wheel's tick rate with the filter's lag (some mass)");
    expect(std::fabs(speedAt(v, 0.1) - 0.5) < 0.08 && std::fabs(v.back() - 0.5) < 0.03 && top < 0.5 * 1.2, "jog: then settles on the wheel's speed");
  }
  // 2. a slow steady drag on a coarse jog (128 ticks a turn, one tick every 37 ms):
  //    a continuous speed (the filter rides through the gaps between ticks); then
  //    the hand holds the record: it comes to rest, then silence
  {
    Deck d = rampDeck(rampPath);
    long sent = 0;
    run(d, 256, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
    const int64_t t0 = d.pos;
    const double v = 0.4, drag = 1.2;
    auto hand = [&](double t) {
      const double tt = t - static_cast<double>(t0) / kSr;
      return tt <= 0 ? 0.0 : v * kSr * std::min(tt, drag);
    };
    run(d, static_cast<int64_t>(1.8 * kSr), 64, [&](Deck& dd, int64_t pos, int n) { jogTicks(dd, pos, n, hand, sent, fpt); });
    const auto p = rampPos(d, static_cast<size_t>(t0), d.L.size());
    double s = 0, s2 = 0, lo = 1e9;
    int n = 0;
    const int w = 64;  // speed over 1.5 ms
    for (size_t i = static_cast<size_t>(0.3 * kSr); i + w < static_cast<size_t>(1.1 * kSr); i += w)
      if (std::isfinite(p[i]) && std::isfinite(p[i + w])) {
        const double r = (p[i + w] - p[i]) / w;
        s += r, s2 += r * r, ++n, lo = std::min(lo, r);
      }
    const double mean = s / n, sd = std::sqrt(std::max(0.0, s2 / n - mean * mean));
    double still = 0;
    for (size_t i = static_cast<size_t>((drag + 0.35) * kSr); i < static_cast<size_t>((drag + 0.55) * kSr); ++i)
      still = std::max(still, static_cast<double>(std::fabs(d.R[static_cast<size_t>(t0) + i])));
    std::printf("# coarse jog drag at 0.4x: speed %.3f +- %.3f (lowest %.3f); held: peak %.1e\n", mean, sd, lo, still);
    expect(std::fabs(mean - v) < 0.02 && sd < 0.1 && lo > 0.15, "coarse jog: a slow drag plays a continuous speed (it never stops between ticks)");
    expect(still < 1e-6, "coarse jog: the record held still comes to rest: silence");
  }
  // 3. jog sensitivity: the default moves the record 0.8 turns per wheel turn; it scales
  for (float sens : {0.8f, 0.5f, 2.0f}) {
    Deck d = rampDeck(rampPath, sens);
    long sent = 0;
    run(d, 256, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
    const int64_t t0 = d.pos;
    const double v = 0.5;
    auto hand = [&](double t) { return v * kSr * std::max(0.0, t - static_cast<double>(t0) / kSr); };
    run(d, static_cast<int64_t>(0.4 * kSr), 64, [&](Deck& dd, int64_t pos, int n) { jogTicks(dd, pos, n, hand, sent, fpt); });
    const auto p = rampPos(d, static_cast<size_t>(t0), d.L.size());
    const auto a = static_cast<size_t>(0.15 * kSr), b = static_cast<size_t>(0.35 * kSr);
    const double r = (p[b] - p[a]) / static_cast<double>(b - a);
    std::printf("# jog sensitivity %.2f: the wheel at 0.5x plays %.3fx\n", sens, r);
    expect(std::fabs(r - v * sens) < 0.03 * sens, sens == 0.8f ? "jog sensitivity: the default 0.8 plays 0.8 of the wheel's speed" : "jog sensitivity scales the record's travel");
  }
  // 4. the stopped wheel (no touch) moves the record by hand at half that: 0.4 of a tick by default
  {
    Deck d = rampDeck(rampPath, static_cast<float>(pl::kJogSensDefault));
    run(d, 2048, 64);
    const double before = view(d).pos;
    run(d, 64, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0xB0, 0x21, 0x40 + 8, 0); });
    run(d, static_cast<int64_t>(0.3 * kSr), 64);
    const double moved = view(d).pos - before;
    std::printf("# stopped wheel: 8 ticks move the record %.1f frames (%.3f ticks)\n", moved, moved / fpt);
    expect(std::fabs(moved - 8 * fpt * 0.5 * 0.8) < 0.02 * 8 * fpt, "stopped wheel: 8 ticks move the record 8 x 0.5 x 0.8 ticks (the sensitivity applies)");
  }
  // 5. touch and release on a playing record: the hand catches it smoothly (no
  //    swing back) and holds it silent; the release ramps it back to the motor
  {
    Deck d = rampDeck(rampPath);
    d.st.params[pl::PStartTime] = 0;
    button(d, pl::CtlPlay, true);
    button(d, pl::CtlPlay, false);
    run(d, static_cast<int64_t>(0.2 * kSr), 64);
    const auto t0 = static_cast<size_t>(d.pos);
    run(d, 64, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x7F, 0); });
    const auto caught = speeds(d, static_cast<int64_t>(0.4 * kSr), 64);
    const auto p = rampPos(d, t0, d.L.size());
    double back = 0, top = 0;
    for (size_t i = 1; i < p.size(); ++i)
      if (std::isfinite(p[i])) top = std::max(top, p[i]), back = std::max(back, top - p[i]);
    double still = 0;
    for (size_t i = static_cast<size_t>(0.3 * kSr); i < p.size(); ++i) still = std::max(still, static_cast<double>(std::fabs(d.R[t0 + i])));
    std::printf("# touch on a playing record: %.3f after 10 ms, %.3f after 50 ms, %.3f after 150 ms; swings back %.2f frames; then %.1e\n",
                speedAt(caught, 0.01), speedAt(caught, 0.05), speedAt(caught, 0.15), back, still);
    expect(speedAt(caught, 0.01) > 0.5 && speedAt(caught, 0.05) < 0.5 && std::fabs(speedAt(caught, 0.15)) < 0.1, "touch: a playing record is caught smoothly");
    expect(back < 0.5 && still < 1e-6, "touch: no swing back, then silence");
    run(d, 64, 64, [](Deck& dd, int64_t, int) { dd.g->midi(0x90, 0x36, 0x00, 0); });
    const auto rel = speeds(d, static_cast<int64_t>(0.4 * kSr), 64);
    double peak = 0;
    for (double x : rel) peak = std::max(peak, x);
    std::printf("# release: %.3f after 10 ms, %.3f after 50 ms, %.3f after 400 ms (peak %.3f)\n", speedAt(rel, 0.01), speedAt(rel, 0.05), rel.back(), peak);
    expect(speedAt(rel, 0.01) > 0.05 && speedAt(rel, 0.01) < 0.9 && std::fabs(rel.back() - 1.0) < 1e-6 && peak < 1.1,
           "release: the speed ramps back to the motor's (not a jump), then the motor has it");
    expect(!view(d).touched && view(d).playing, "release: playing again, the hand is off");
  }
  // 6. the mouse (J-155, after Mixxx's position loop): a steady drag at 1x is
  //    taken up smoothly and held; Hand feel tight takes it up quicker
  double rises[2] = {0, 0};
  for (int f = 0; f < 2; ++f) {
    const float feel = f == 0 ? 0.5f : 1.0f;
    Deck d = rampDeck(rampPath);
    d.st.params[pl::PFeel] = feel;
    run(d, 256, 64, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdHandDown); });
    const int64_t t0 = d.pos;
    const auto v = speeds(d, static_cast<int64_t>(0.6 * kSr), 64, [&](Deck& dd, int64_t pos, int) {
      if ((pos - t0) % 735 < 64) cmd(dd, pl::CmdHandMove, 0, static_cast<double>(pos - t0) / (kSr * pl::kSecondsPerRev));
    });
    size_t k = 0;
    while (k < v.size() && v[k] < 0.9) ++k;
    double mean = 0, jump = 0;
    for (size_t i = v.size() / 2; i < v.size(); ++i) mean += v[i];
    mean /= static_cast<double>(v.size() - v.size() / 2);
    const auto p = rampPos(d, static_cast<size_t>(t0), d.L.size());
    // the speed over 8 samples (the ramp's read-back has some sample-to-sample noise), window to window
    for (size_t i = 0; i + 16 < p.size(); i += 8)
      if (std::isfinite(p[i]) && std::isfinite(p[i + 8]) && std::isfinite(p[i + 16]))
        jump = std::max(jump, std::fabs((p[i + 16] - p[i + 8]) - (p[i + 8] - p[i])) / 8);
    rises[f] = static_cast<double>(k * 64) / kSr * 1000;
    std::printf("# mouse drag at 1x (hand feel %.1f): 0.9x after %.0f ms, then %.3f; largest speed change in 8 samples %.4f\n", feel, rises[f], mean, jump);
    expect(std::fabs(mean - 1.0) < 0.06, "mouse: a steady drag plays its speed");
    expect(jump < 0.04, "mouse: no speed steps from the 60 Hz mouse (the loop's rate changes are ramped)");
  }
  expect(rises[0] > 40 && rises[0] < 250 && rises[1] < 0.7 * rises[0], "mouse: the record takes up a drag with the loop's lag; tight hand feel quicker");
  // 7. a mouse throw (let go above 2.5x) coasts and slows exponentially; a slower
  //    release stops at once (the motor is off)
  for (double sp : {4.0, 1.5}) {
    Deck d = rampDeck(rampPath);
    d.st.params[pl::PFeel] = 1.0f;  // (tight: the record is near the hand's speed when it lets go)
    run(d, 256, 64, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdHandDown); });
    const int64_t t0 = d.pos;
    run(d, static_cast<int64_t>(0.25 * kSr), 64, [&](Deck& dd, int64_t pos, int) {
      if ((pos - t0) % 735 < 64) cmd(dd, pl::CmdHandMove, 0, -sp * static_cast<double>(pos - t0) / (kSr * pl::kSecondsPerRev));
    });
    const double at0 = view(d).speed;
    run(d, 64, 64, [](Deck& dd, int64_t, int) { cmd(dd, pl::CmdHandUp); });
    const auto v = speeds(d, static_cast<int64_t>(1.0 * kSr), 64);
    if (sp > 2.5) {
      // 100x would stop in 1 s: |v(t)| = |v(0)| (0.01 / 100)^t
      const double want = std::fabs(speedAt(v, 0.1)) * std::pow(1e-4, 0.2);
      std::printf("# throw at %.2fx: %.3f after 0.1 s, %.3f after 0.3 s (exponential: %.3f), %.4f after 1 s\n", at0, speedAt(v, 0.1), speedAt(v, 0.3), -want, v.back());
      expect(speedAt(v, 0.1) < -1.0 && std::fabs(std::fabs(speedAt(v, 0.3)) - want) < 0.05 * want + 0.01, "mouse throw: the record coasts and slows exponentially (100x to rest in 1 s)");
      expect(v.back() == 0.0 && !view(d).touched, "mouse throw: then rests");
    } else {
      std::printf("# slow release at %.2fx: %.3f after 10 ms\n", at0, speedAt(v, 0.01));
      expect(std::fabs(at0) > 1.0 && speedAt(v, 0.01) == 0.0, "mouse: a release under 2.5x stops the record (the motor is off)");
    }
  }
  // 8. the jog filter of each controller (the values Mixxx's mappings use)
  expect(pl::jogFilterOf("ddj200").alpha == 1.0 / 8 && pl::jogFilterOf("ddj200").beta == 1.0 / 256 && pl::jogFilterOf("inpulse200").alpha == 1.0 / 8 &&
             pl::jogFilterOf("mixtrackprofx").alpha == 1.0 && pl::jogFilterOf("mixtrackprofx").beta == 1.0 / 32 && pl::jogFilterOf("").beta == 1.0 / 256,
         "jog filter per controller: DDJ-200 and Inpulse 200 1/8, 1/256; Mixtrack Pro FX 1, 1/32");
  // 9. scratching is block-size independent (jog ticks on their samples, a release)
  {
    std::vector<float> ref;
    for (int block : {32, 100, 256, 1024}) {
      Deck d = rampDeck(rampPath);
      auto ev = [&](Deck& dd, int64_t pos, int n) {
        auto at = [&](int64_t f, const std::function<void(int)>& fn) {
          if (f >= pos && f < pos + n) fn(static_cast<int>(f - pos));
        };
        at(100, [&](int o) { dd.g->midi(0x90, 0x36, 0x7F, o); });
        for (int k = 0; k < 60; ++k) at(300 + k * 211, [&](int o) { dd.g->midi(0xB0, 0x22, k < 35 ? 0x42 : 0x3D, o); });
        at(14000, [&](int o) { dd.g->midi(0x90, 0x36, 0x00, o); });
      };
      run(d, 20000, block, ev);
      if (ref.empty()) ref = d.L;
      else expect(maxDiff(ref, d.L, 0, ref.size()) == 0.0, "jog scratch: the same audio at any block size");
    }
  }
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
  const double deckBeat = pl::beatAt(d.m.deck[0], v.pos);
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
  pl::marks(d.g.get(), 0, mk);
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
    Deck d = makeDeck(path, 120, [keyLock](PluginState& s, pl::Model& m) {
      s.params[pl::PKeyLock] = keyLock ? 1.f : 0.f;
      m.recordAs = pl::RecTake;  // J-159: takes are the second recording kind
    });
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
    expect(drainTake(d, take), keyLock ? "take captured (key lock)" : "take captured");
    live = d.L;
  }
  expect(take.startTick == 0 && take.s[0].on && !take.s[1].on && take.s[0].pts.size() >= static_cast<size_t>(frames / pl::kStep),
         "take: starts on the beat, covers the recording (deck A only: deck B is empty)");
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
  const size_t n = static_cast<size_t>(take.steps()) * pl::kStep;
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
  // the same take written by J-150 (a single-deck "take" line) replays the same
  if (!keyLock) {
    char b[256];
    const pl::TakeStream& x = take.s[0];
    std::string v1 = path + "\nplatter 1\n";
    std::snprintf(b, sizeof b, "src %lld %.3f 1\n", static_cast<long long>(r.m.deck[0].srcFrames), r.m.deck[0].srcRate);
    v1 += b;
    const auto& g = r.m.deck[0].grid;
    std::snprintf(b, sizeof b, "grid %.6f %d %d reg %.6f %.6f %zu\n", g.bpm, g.beatsPerBar, g.bar1, g.beats[0], g.beats[1] - g.beats[0], g.beats.size());
    v1 += b;
    v1 += "cue 0.0000\nhot -1 -1 -1 -1 -1 -1 -1 -1\nloop -1 -1\nflags 1 1\nmap ddj200 0 128.000 1\ndev \n";
    std::snprintf(b, sizeof b, "take %d %.3f %d %.6f %d %lld %lld %.6f %.9f %.6f %zu ", take.key, take.sampleRate, take.step, take.tempo, take.ppq,
                  static_cast<long long>(take.startTick), static_cast<long long>(take.lengthTicks), x.startPos, x.startRate,
                  static_cast<double>(x.startGain), x.pts.size());
    v1 += b + pl::encodePoints(x.pts) + "\n";
    pl::Model old;
    std::string op;
    expect(pl::parseData(v1, op, old) && old.takes.size() == 1 && old.takes[0].s[0] == x && old.takes[0].s[0].file == 0 && !old.takes[0].s[1].on &&
               old.takes[0].moves.empty(),
           "a J-150 take line: deck A's stream on the track");
    Deck o = makeDeck(path, 120);
    o.st.data = v1;
    o.st.params.resize(23);  // J-150 had 23 parameters
    normalizePluginState(o.st);
    o.st.params[pl::PXfSide] = pl::XfThru;  // J-150's default assign
    packs::prepareAux(o.st, o.aux);
    o.g->prepareHost(o.st, kSr);
    o.ctx.playing = true;
    run(o, frames, 300, [](Deck& dd, int64_t pos, int) {
      if (pos == 0) dd.g->noteOn(7, pl::kTakeKeyFirst, 1.f, 0);
    });
    const double d1 = maxDiff(live, o.L, 0, n);
    std::printf("# J-150 take replay difference %.2e\n", d1);
    expect(d1 < 1e-5, "a J-150 take replays sample for sample");
  }
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

// ------------------------------------------------------------------ two decks and the mixer
// Both decks playing their own tempo (no sync, no key lock, instant start): `tweak` sets the mixer.
std::vector<float> mixRun(const std::string& a, const std::string& b, const std::function<void(PluginState&)>& tweak, int64_t frames = 22050) {
  Deck d = makeDeck(
      a, 120,
      [&](PluginState& s, pl::Model&) {
        for (int k = 0; k < pl::kDecks; ++k) {
          s.params[static_cast<size_t>(pl::deckParam(k, pl::DSync))] = 0;
          s.params[static_cast<size_t>(pl::deckParam(k, pl::DKeyLock))] = 0;
          s.params[static_cast<size_t>(pl::deckParam(k, pl::DStartTime))] = 0;
        }
        if (tweak) tweak(s);
      },
      b, 100);
  d.g->prepareHost(d.st, kSr);
  button(d, pl::CtlPlay, true, 0);
  button(d, pl::CtlPlay, true, 1);
  run(d, frames, 256);
  return d.L;
}

void testMixer(const std::string& a, const std::string& b) {
  auto vol = [](int deck, float v) {
    return [deck, v](PluginState& s) { s.params[static_cast<size_t>(pl::deckParam(deck, pl::DVolume))] = v; };
  };
  const std::vector<float> both = mixRun(a, b, nullptr);
  const std::vector<float> onlyA = mixRun(a, b, vol(1, 0));
  const std::vector<float> onlyB = mixRun(a, b, vol(0, 0));
  std::vector<float> sum(both.size());
  for (size_t i = 0; i < sum.size(); ++i) sum[i] = onlyA[i] + onlyB[i];
  // (from the second step: a fader set before the first play fades from unity over one step)
  const size_t S = 2 * pl::kStep;
  std::printf("# mix: both decks vs the sum of each alone %.2e\n", maxDiff(both, sum, S, both.size()));
  expect(rms(onlyA, 2000, 22050) > 0.05 && rms(onlyB, 2000, 22050) > 0.05, "mixer: each deck sounds on its own");
  expect(maxDiff(both, sum, S, both.size()) < 1e-5, "mixer: both decks play at once, summed");
  // crossfader: A assigned left, B right; all the way to A silences B
  auto xf = [](float x, float cut, float rev) {
    return [x, cut, rev](PluginState& s) {
      s.params[pl::PXfader] = x;
      s.params[pl::PXfCut] = cut;
      s.params[pl::PXfReverse] = rev;
    };
  };
  expect(maxDiff(mixRun(a, b, xf(0, 0, 0)), onlyA, S, both.size()) < 1e-6, "crossfader at A: deck B is silent");
  expect(maxDiff(mixRun(a, b, xf(1, 0, 0)), onlyB, S, both.size()) < 1e-6, "crossfader at B: deck A is silent");
  expect(maxDiff(mixRun(a, b, xf(0, 0, 1)), onlyB, S, both.size()) < 1e-6, "crossfader reverse: the A end plays deck B");
  // cut: a hair off the end brings the other deck in fully (the scratch cut)
  expect(maxDiff(mixRun(a, b, xf(0.01f, 1, 0)), both, S, both.size()) < 1e-5, "crossfader cut: the other deck is fully in just off the end");
  const std::vector<float> smooth = mixRun(a, b, [](PluginState& s) {
    s.params[pl::PXfader] = 0.01f;
    s.params[pl::PXfCurve] = 0;
  });
  expect(rms(smooth, 2000, 22050) < rms(both, 2000, 22050) * 0.9, "smooth curve: the other deck is still low just off the end");
  // Thru ignores the crossfader
  const std::vector<float> thru = mixRun(a, b, [](PluginState& s) {
    s.params[pl::PXfader] = 0;
    s.params[static_cast<size_t>(pl::deckParam(1, pl::DXfSide))] = pl::XfThru;
  });
  expect(maxDiff(thru, both, S, both.size()) < 1e-6, "Thru: a deck ignores the crossfader");
  // master: -6.02 dB halves the output, the bottom is off
  const std::vector<float> half = mixRun(a, b, [](PluginState& s) { s.params[pl::PMaster] = static_cast<float>(20 * std::log10(0.5)); });
  double err = 0;
  for (size_t i = S; i < both.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(half[i] - 0.5f * both[i])));
  expect(err < 1e-5, "master: -6 dB halves the output");
  expect(rms(mixRun(a, b, [](PluginState& s) { s.params[pl::PMaster] = static_cast<float>(pl::kMasterOffDb); }), S, both.size()) == 0.0,
         "master at the bottom: off");
  // a master move ramps (no click)
  Deck d = makeDeck(a, 120, [](PluginState& s, pl::Model&) {
    s.params[pl::PSync] = 0;
    s.params[pl::PStartTime] = 0;
  });
  button(d, pl::CtlPlay, true);
  run(d, 11025, 256);
  d.st.params[pl::PMaster] = static_cast<float>(pl::kMasterOffDb);
  run(d, 11025, 256);
  expect(maxStep(d.L, 1000, d.L.size()) < 0.08 && rms(d.L, 20000, 22050) == 0.0, "master: a move ramps, then off");
  pl::MixView mv;
  expect(pl::mixView(d.g.get(), mv) && mv.valid, "mix view");
  // the levels the editor meters
  Deck e = makeDeck(a, 120, [](PluginState& s, pl::Model&) { s.params[static_cast<size_t>(pl::deckParam(1, pl::DStartTime))] = 0; }, b, 100);
  button(e, pl::CtlPlay, true, 1);
  run(e, 4410, 256);
  expect(view(e, 1).pre > 0.05f && view(e, 1).level > 0.05f && view(e, 0).pre == 0.f, "meters: deck B's channel level, deck A silent");
  // one-deck layout: deck B is not played
  Deck o = makeDeck(
      a, 120,
      [](PluginState& s, pl::Model& m) {
        s.params[static_cast<size_t>(pl::deckParam(1, pl::DStartTime))] = 0;
        m.layout = pl::LayoutOne;
      },
      b, 100);
  button(o, pl::CtlPlay, true, 1);
  run(o, 4410, 256);
  expect(rms(o.L, 0, o.L.size()) == 0.0, "one-deck layout: deck B stays silent");
}

// Deck B synced to deck A (Sync to: Other deck): it plays at A's tempo, on A's beat.
void testSyncDeck(const std::string& path120, const std::string& path100) {
  Deck d = makeDeck(
      path120, 120,
      [](PluginState& s, pl::Model&) {
        s.params[pl::PSyncTo] = pl::SyncDeck;
        s.params[pl::PSync] = 0;  // A runs free: +8 % on its fader
        s.params[pl::PPitchRange] = pl::Range16;
        s.params[pl::PPitch] = 0.5f;
        s.params[pl::PKeyLock] = 0;
        s.params[static_cast<size_t>(pl::deckParam(1, pl::DKeyLock))] = 0;
      },
      path100, 100);
  d.ctx.tempo = 90;  // the project's tempo is not the one followed
  d.ctx.playing = true;
  button(d, pl::CtlPlay, true, 0);
  run(d, 4410, 256);
  button(d, pl::CtlPlay, true, 1);
  run(d, static_cast<int64_t>(3 * kSr), 256);
  const pl::LiveView a = view(d, 0), b = view(d, 1);
  std::printf("# deck sync: A %.3f BPM, B %.3f BPM (speed %.4f)\n", a.tempo, b.tempo, b.speed);
  expect(std::fabs(a.tempo - 129.6) < 0.05, "deck A: its own tempo with the pitch fader");
  expect(b.following && std::fabs(b.tempo - a.tempo) < 0.05 && std::fabs(b.speed - 1.296) < 0.005, "deck B follows deck A's tempo");
  double err = (b.beat - std::floor(b.beat)) - (a.beat - std::floor(a.beat));
  err -= std::round(err);
  std::printf("# deck sync phase error %.4f beats\n", err);
  expect(std::fabs(err) < 0.02, "deck B's beats land on deck A's");
  // both synced: A follows the project, B follows A
  d.st.params[pl::PSync] = 1;
  run(d, static_cast<int64_t>(2 * kSr), 256);
  expect(std::fabs(view(d, 0).tempo - 90) < 0.05 && std::fabs(view(d, 1).tempo - 90) < 0.05 && !view(d, 0).following,
         "both synced: A leads at the project tempo");
  // sync to the project: B ignores deck A
  d.st.params[pl::PSync] = 0;
  d.st.params[pl::PSyncTo] = pl::SyncProject;
  run(d, static_cast<int64_t>(2 * kSr), 256);
  expect(std::fabs(view(d, 1).tempo - 90) < 0.05 && !view(d, 1).following, "sync to the project: deck B follows the project");
}

// A take of both decks and the mixer replays sample for sample.
void testTakesBoth(const std::string& a, const std::string& b, bool keyLock) {
  pl::Take take;
  std::vector<float> live;
  const int64_t frames = 120000;
  auto setup = [keyLock](PluginState& s, pl::Model& m) {
    m.recordAs = pl::RecTake;
    for (int k = 0; k < pl::kDecks; ++k) s.params[static_cast<size_t>(pl::deckParam(k, pl::DKeyLock))] = keyLock ? 1.f : 0.f;
  };
  {
    Deck d = makeDeck(a, 120, setup, b, 100);
    d.g->prepareHost(d.st, kSr);
    d.ctx.playing = true;
    d.ctx.recording = true;
    run(d, frames, 128, [](Deck& dd, int64_t pos, int n) {
      auto at = [&](int64_t f, const std::function<void(int)>& fn) {
        if (f >= pos && f < pos + n) fn(static_cast<int>(f - pos));
      };
      at(10, [&](int o) { dd.g->midi(0x90, 0x0B, 0x7F, o); });  // A plays
      at(15, [&](int o) { dd.g->midi(0x91, 0x0B, 0x7F, o); });  // B plays
      // A: a scratch
      at(20000, [&](int o) { dd.g->midi(0x90, 0x36, 0x7F, o); });
      for (int k = 0; k < 60; ++k) at(20100 + k * 400, [&](int o) { dd.g->midi(0xB0, 0x22, (k / 15) % 2 ? 0x3D : 0x43, o); });
      at(45000, [&](int o) { dd.g->midi(0x90, 0x36, 0x00, o); });
      // B: a nudge and a scratch
      for (int k = 0; k < 8; ++k) at(30000 + k, [&](int o) { dd.g->midi(0xB1, 0x21, 0x44, o); });
      at(60000, [&](int o) { dd.g->midi(0x91, 0x36, 0x7F, o); });
      for (int k = 0; k < 50; ++k) at(60100 + k * 350, [&](int o) { dd.g->midi(0xB1, 0x22, (k / 10) % 2 ? 0x3C : 0x44, o); });
      at(80000, [&](int o) { dd.g->midi(0x91, 0x36, 0x00, o); });
      // mixer: crossfader moves, EQ kill on A, B's filter and channel fader
      at(25000, [&](int o) { dd.g->midi(0xB6, 0x1F, 0x08, o); });
      at(35000, [&](int o) { dd.g->midi(0xB6, 0x1F, 0x7F, o); });
      at(50000, [&](int o) { dd.g->midi(0xB6, 0x1F, 0x40, o); });
      at(40000, [&](int o) { dd.g->midi(0xB0, 0x0F, 0x00, o); });  // A: low EQ all the way down
      at(70000, [&](int o) { dd.g->midi(0xB0, 0x0F, 0x40, o); });
      at(55000, [&](int o) { dd.g->midi(0xB6, 0x18, 0x18, o); });  // B: filter
      at(90000, [&](int o) { dd.g->midi(0xB6, 0x18, 0x40, o); });
      at(85000, [&](int o) { dd.g->midi(0xB1, 0x13, 0x20, o); });  // B: channel fader
      // the master level from the project (automation)
      if (pos <= 95000 && pos + n > 95000) dd.st.params[pl::PMaster] = -9.f;
    });
    d.ctx.recording = false;
    run(d, 256, 256);
    expect(drainTake(d, take), keyLock ? "two-deck take captured (key lock)" : "two-deck take captured");
    live = d.L;
  }
  bool master = false, eq = false, filt = false, xfm = false;
  for (const auto& m : take.moves) {
    master = master || (m.param == pl::PMaster && m.value == -9.f);
    eq = eq || (m.param == pl::PEqLow && m.value < -20.f);
    filt = filt || m.param == pl::deckParam(1, pl::DFilter);
    xfm = xfm || m.param == pl::PXfader;
  }
  expect(take.s[0].on && take.s[1].on && take.s[0].file == 0 && take.s[1].file == 1, "take: a stream for each deck, on its own track");
  expect(master && eq && filt && !xfm, "take: mixer moves (master, EQ, filter) recorded; fader and crossfader in the stream gains");
  take.key = pl::kTakeKeyFirst + 1;
  // replay on a fresh setup: the decks stopped, defaults in every parameter
  Deck r = makeDeck(
      a, 120,
      [&](PluginState& s, pl::Model& m) {
        setup(s, m);
        m.takes.push_back(take);
      },
      b, 100);
  pl::Model back;
  std::string p;
  expect(pl::parseData(r.st.data, p, back) && back.takes.size() == 1 && back.takes[0] == take, "two-deck take: stored in the project data");
  r.g->prepareHost(r.st, kSr);
  r.ctx.playing = true;
  run(r, frames, 300, [](Deck& dd, int64_t pos, int) {
    if (pos == 0) dd.g->noteOn(7, pl::kTakeKeyFirst + 1, 1.f, 0);
  });
  const size_t n = static_cast<size_t>(take.steps()) * pl::kStep;
  size_t first = n;
  for (size_t i = 0; i < std::min({n, live.size(), r.L.size()}); ++i)
    if (std::fabs(live[i] - r.L[i]) > 1e-5f) {
      first = i;
      break;
    }
  if (first < n) std::printf("# first difference at sample %zu: %.4f vs %.4f\n", first, live[first], r.L[first]);
  const double diff = maxDiff(live, r.L, 0, n);
  std::printf("# two-deck take replay difference %.2e over %zu samples\n", diff, n);
  expect(diff < 1e-5, keyLock ? "two-deck take: replays sample for sample (key lock)" : "two-deck take: replays sample for sample (both decks, mixer moves)");
  expect(rms(live, 60000, 80000) > 0.02 && rms(live, 96000, 110000) > 0.005, "two-deck take: both decks sound");
  // the take plays its own tracks: with another track on deck B it replays the same
  Deck q = makeDeck(a, 120, [&](PluginState& s, pl::Model& m) {
    setup(s, m);
    pl::Take t2 = take;
    t2.s[1].file = pl::fileIndex(m, b);
    m.takes.push_back(t2);
  });
  q.g->prepareHost(q.st, kSr);
  q.ctx.playing = true;
  run(q, frames, 300, [](Deck& dd, int64_t pos, int) {
    if (pos == 0) dd.g->noteOn(7, pl::kTakeKeyFirst + 1, 1.f, 0);
  });
  if (!keyLock) expect(maxDiff(live, q.L, 0, n) < 1e-5, "a take replays its own tracks, whatever the decks hold now");
}

// A J-150 state (single deck, "platter 1", 23 parameters) plays as deck A, unchanged.
void testOldState(const std::string& path) {
  SamplePtr src = sampleCache().get(path);
  char b[256];
  std::string v1 = path + "\nplatter 1\n";
  std::snprintf(b, sizeof b, "src %lld %.3f 1\ngrid 120.000000 4 1 reg 0.000000 22050.000000 20\n", static_cast<long long>(src->left.size()), kSr);
  v1 += b;
  v1 += "cue 0.0000\nhot 44100.0000 -1 -1 -1 -1 -1 -1 -1\nloop -1 -1\nflags 1 0\nmap ddj200 1 128.000 1\ndev DDJ-200\n";
  v1 += "bind play 0 1 11 0 0 0 0\nled play 145 11 127 0\n";
  pl::Model m;
  std::string p;
  expect(pl::parseData(v1, p, m) && p == path && m.files.size() == 1 && m.deck[0].file == 0 && m.deck[1].file < 0,
         "J-150 state: deck A on its track, deck B empty");
  expect(m.deck[0].hot[0] == 44100 && m.deck[0].grid.beats.size() == 20 && m.asked && !m.armed && m.twoDecks(), "J-150 state: grid, cues, flags");
  expect(m.map.side == 1 && m.map.bindings.size() == 1 && m.map.bindings[0].deck == 0 && m.map.bindings[0].channel == 1 && m.map.leds.size() == 1,
         "J-150 mapping: the right side drives deck A");
  // plays exactly like J-150's deck: the output is the file
  Deck d = makeDeck(path, 120);
  d.st = defaultPluginState(pl::kType);
  d.st.params.resize(23);
  normalizePluginState(d.st);
  expect(static_cast<int>(d.st.params.size()) == pl::PCount, "J-150 parameters are completed");
  expect(std::fabs(d.st.params[pl::PJogA] - 0.8f) < 1e-6 && std::fabs(d.st.params[pl::PJogB] - 0.8f) < 1e-6,
         "an old project's decks get the default jog sensitivity (0.8)");
  {
    // a J-151 project (two decks, 46 parameters): its values stay, the jog sensitivities are added
    PluginState s = defaultPluginState(pl::kType);
    s.params.resize(46);
    s.params[pl::PXfReverse] = 1;
    s.params[pl::deckParam(1, pl::DFeel)] = 0.25f;
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == pl::PCount && s.params[pl::PXfReverse] == 1 && s.params[pl::deckParam(1, pl::DFeel)] == 0.25f &&
               std::fabs(s.params[pl::PJogA] - 0.8f) < 1e-6 && std::fabs(s.params[pl::PJogB] - 0.8f) < 1e-6,
           "a J-151 project loads with its values and the default jog sensitivity");
  }
  d.st.params[pl::PSync] = 0;
  d.st.params[pl::PKeyLock] = 0;
  d.st.params[pl::PStartTime] = 0;
  d.st.params[pl::PXfSide] = pl::XfThru;
  d.st.data = v1;
  d.aux = ChannelAux{};
  packs::prepareAux(d.st, d.aux);
  d.g->prepareHost(d.st, kSr);
  run(d, 4410, 256);
  d.g->midi(0x91, 0x0B, 0x7F, 0);  // its own mapping: right-side play
  run(d, 44100, 256);
  double err = 1;
  for (size_t lag = 4410; lag < 4410 + pl::kStep; ++lag) {
    double e = 0;
    for (size_t i = lag + 512; i < 44100; ++i) e = std::max(e, static_cast<double>(std::fabs(d.L[i] - src->left[i - lag])));
    err = std::min(err, e);
  }
  std::printf("# J-150 state play error %.2e\n", err);
  expect(err < 1e-5, "J-150 state: the deck plays the file sample for sample (no change in level)");
  expect(std::fabs(view(d, 0).hot[0] - 44100) < 1 && !view(d, 1).loaded, "J-150 state: hot cues on deck A, nothing on deck B");
}

// Two decks playing, key lock, scratching, mixer moves: still no allocation.
void testNoAllocBoth(const std::string& a, const std::string& b) {
  Deck d = makeDeck(a, 120, nullptr, b, 100);
  d.g->prepareHost(d.st, kSr);
  d.ctx.playing = true;
  run(d, 512, 256);
  d.g->midi(0x91, 0x0B, 0x7F, 0);
  run(d, 512, 256);
  gAllocs = 0;
  run(
      d, 60000, 256,
      [](Deck& dd, int64_t pos, int n) {
        performance(dd, pos, n);
        auto at = [&](int64_t f, const std::function<void(int)>& fn) {
          if (f >= pos && f < pos + n) fn(static_cast<int>(f - pos));
        };
        at(5000, [&](int o) { dd.g->midi(0x91, 0x36, 0x7F, o); });
        for (int k = 0; k < 40; ++k) at(5100 + k * 300, [&](int o) { dd.g->midi(0xB1, 0x22, k < 20 ? 0x44 : 0x3B, o); });
        at(18000, [&](int o) { dd.g->midi(0x91, 0x36, 0x00, o); });
        at(20000, [&](int o) { dd.g->midi(0xB6, 0x1F, 0x10, o); });
        at(26000, [&](int o) { dd.g->midi(0xB0, 0x0F, 0x00, o); });
        at(32000, [&](int o) { dd.g->midi(0xB6, 0x18, 0x10, o); });
        at(36000, [&](int o) { dd.g->noteOn(1, pl::kHotKeyFirstB + 1, 1.f, o); });
      },
      true);
  std::printf("# allocations during process (two decks): %ld\n", gAllocs.load());
  expect(gAllocs.load() == 0, "no allocation in process() (two decks, key lock, scratching, mixer)");
  expect(view(d, 0).keylockActive || view(d, 1).keylockActive, "key lock engines in use");
}

// ------------------------------------------------------------------ project, old effect
void testProject(const std::string& path) {
  Song s;
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState(pl::kType);
  pl::Model m;
  pl::loadTrack(m, 0, path, flatDeck(path, 120));
  pl::loadTrack(m, 1, path, flatDeck(path, 120));
  m.map = pl::profileMapping("generic", 0);
  m.map.bindings.push_back(pl::learnBinding(pl::CtlPlay, 0x93, 0x10, 0x7F, false));
  m.asked = true;
  c.plugin.data = pl::formatData(path, m);
  c.plugin.params[pl::PPitch] = 0.25f;
  c.plugin.params[static_cast<size_t>(pl::deckParam(1, pl::DPitch))] = -0.5f;
  c.plugin.params[pl::PMaster] = -3.f;
  s.channels.push_back(c);
  const std::string proj = (tmpDir() / "deck.gridline.json").string();
  std::string err;
  expect(saveProject(proj, s, &err), "project with a deck saves");
  Song t;
  expect(loadProject(proj, t, &err), "project with a deck loads");
  const Channel* tc = t.channel(c.id);
  expect(tc && tc->plugin.data == c.plugin.data && tc->plugin.params == c.plugin.params, "save / load: mapping, grid and parameters come back");
  // every track of the setup is a file reference (collect, relink): both decks and the takes' tracks
  {
    PluginState st = c.plugin;
    pl::Model fm;
    pl::loadTrack(fm, 0, "/old/a.wav", pl::DeckModel{});
    pl::loadTrack(fm, 1, "/old/b.wav", pl::DeckModel{});
    pl::Take tk;
    tk.s[0].on = true;
    tk.s[0].file = pl::fileIndex(fm, "/old/c.wav");
    tk.s[0].pts.resize(4);
    fm.takes.push_back(tk);
    st.data = pl::formatData("/old/a.wav", fm);
    std::vector<std::string> seen;
    forEachStateFileRef(st, [&](std::string& f) {
      seen.push_back(f);
      f = "/new" + f.substr(4);
    });
    pl::Model back;
    std::string p0;
    expect(seen.size() == 4 && pl::parseData(st.data, p0, back) && p0 == "/new/a.wav" && back.trackOf(0) == "/new/a.wav" &&
               back.trackOf(1) == "/new/b.wav" && back.files.size() == 3 && back.files[2] == "/new/c.wav",
           "file references: deck A, deck B and a take's track are relinked");
  }
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
  // (platter_flow.cmake) writetrack <path> <bpm>: 12 s of music-like material at that tempo
  if (argc > 3 && std::string(argv[1]) == "writetrack") {
    const double bpm = std::atof(argv[3]);
    const auto n = static_cast<size_t>(12 * kSr);
    AudioBuffer b(2, n);
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < 2; ++c) b.data[static_cast<size_t>(c) * n + i] = static_cast<float>(music(static_cast<int64_t>(i), c, bpm));
    writeWav(argv[2], b, static_cast<int>(kSr), 24);
    return fs::exists(argv[2]) ? 0 : 1;
  }
  const std::string only = argc > 1 ? argv[1] : "";
  const std::string music120 = writeTrack("music120.wav", 12, [](int64_t i, int c) { return music(i, c, 120); });
  const std::string music100 = writeTrack("music100.wav", 12, [](int64_t i, int c) { return music(i, c, 100); });
  const std::string sine = writeTrack("sine220.wav", 8, [](int64_t i, int) { return 0.5 * std::sin(2 * kPi * 220 * static_cast<double>(i) / kSr); });
  const std::string high = writeTrack("sine18k.wav", 2, [](int64_t i, int) { return 0.5 * std::sin(2 * kPi * 18000 * static_cast<double>(i) / kSr); });
  auto want = [&](const char* n) { return only.empty() || only == n; };
  if (want("catalogue")) testCatalogue();
  if (want("decode")) testDecoding();
  if (want("decode")) testDecodingBoth();
  if (want("text")) testText();
  if (want("play")) testPlay(music120);
  if (want("physics")) testPhysics(music120);
  if (want("scratch")) testScratch(sine);
  if (want("hand")) {
    const std::string ramp =
        writeTrack("ramp.wav", 2, [](int64_t i, int c) { return c == 0 ? kRampTop * static_cast<double>(i) / kRampN : 0.5; });
    testHand(ramp);
  }
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
  if (want("alloc")) testNoAllocBoth(music120, music100);
  if (want("mixer")) testMixer(music120, music100);
  if (want("syncdeck")) testSyncDeck(music120, music100);
  if (want("takes2")) {
    testTakesBoth(music120, music100, false);
    testTakesBoth(music120, music100, true);
  }
  if (want("old")) testOldState(music120);
  if (want("project")) testProject(music120);
  return finish();
}
