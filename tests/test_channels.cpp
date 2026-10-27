// Gridline — channel and note features: per-note properties (pan, fine pitch,
// release, mod X/Y, colour, slide, porta), channel note settings (fine tune,
// swing mix, time shift, polyphony, mono, porta, cut groups, echo), layer
// channels, display groups — model round-trip, engine behaviour, block-size
// invariance, and the FLP import of these fields (synthetic files only).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Json.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Render.h"
#include "engine/SamplePrecompute.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

// A 2 s sine at `hz` written to a 16-bit WAV (sampler source).
std::string sineWav(const std::string& name, double hz, double amp = 0.5) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 2));
  for (size_t i = 0; i < b.frames; ++i) {
    float v = static_cast<float>(amp * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  std::string path = "test_channels_" + name + ".wav";
  writeWav(path, b, kSr, 16);
  return path;
}

// Empty 120 BPM song with one 2-bar pattern placed once.
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

Note mk(Tick start, Tick len, int pitch, int vel = 100) {
  Note n;
  n.start = start;
  n.length = len;
  n.pitch = pitch;
  n.velocity = vel;
  return n;
}

AudioBuffer render(const Song& s, int block = 512, Tick tail = 0) {
  RenderOptions o;
  o.blockSize = block;
  o.tail = tail;
  return renderSong(s, o);
}

// Frequency from zero crossings of the left channel in [t0, t1) seconds.
double freqAt(const AudioBuffer& b, double t0, double t1) {
  size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  int z = 0;
  for (size_t i = a + 1; i < e; ++i)
    if ((b.at(0, i - 1) < 0) != (b.at(0, i) < 0)) ++z;
  return z / 2.0 / (static_cast<double>(e - a) / kSr);
}

double rms(const AudioBuffer& b, size_t ch, double t0, double t1) {
  size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  double acc = 0;
  for (size_t i = a; i < e; ++i) acc += b.at(ch, i) * b.at(ch, i);
  return e > a ? std::sqrt(acc / static_cast<double>(e - a)) : 0.0;
}

// ---- synthetic FLP writer (same container as the FL format reference) ----
struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}
std::vector<uint8_t> flNote(uint32_t pos, uint16_t ch, uint32_t len, uint16_t key, uint16_t flags, uint8_t fine, uint8_t rel,
                            uint8_t color, uint8_t pan, uint8_t vel, uint8_t mx, uint8_t my) {
  std::vector<uint8_t> n(24, 0);
  put32(n, 0, pos);
  n[4] = static_cast<uint8_t>(flags), n[5] = static_cast<uint8_t>(flags >> 8);
  n[6] = static_cast<uint8_t>(ch), n[7] = static_cast<uint8_t>(ch >> 8);
  put32(n, 8, len);
  n[12] = static_cast<uint8_t>(key);
  n[16] = fine, n[18] = rel, n[19] = color, n[20] = pan, n[21] = vel, n[22] = mx, n[23] = my;
  return n;
}
std::vector<uint8_t> i32s(std::initializer_list<int32_t> v) {
  std::vector<uint8_t> out;
  for (int32_t x : v)
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(x) >> (8 * i)));
  return out;
}

}  // namespace

int main() {
  // ------------------------------------------------ model round-trip
  {
    Song s = baseSong();
    ObjectId a = addChannel(s, "gl.synth"), b = addChannel(s, "gl.sampler");
    Note n = mk(0, 96, 60);
    n.pan = -20, n.fine = 35, n.release = 90, n.modX = 10, n.modY = 250, n.color = 7, n.slide = true, n.porta = true;
    s.patterns[0].notes[a] = {n, mk(96, 48, 62)};
    Channel& ca = s.channels[0];
    ca.fine = -12, ca.swingMix = 0.5f, ca.timeShift = 0.125f, ca.polyphony = 3, ca.mono = true, ca.porta = true, ca.portaTime = 0.3f;
    ca.cutGroup = 2, ca.cutBy = 5, ca.echo.count = 3, ca.echo.feedback = 0.4f, ca.echo.time = 0.5f, ca.echo.pan = -0.5f, ca.echo.modX = 0.25f, ca.echo.modY = -0.5f;
    ca.echo.pitch = 100, ca.echo.pingPong = true, ca.layer = {b}, ca.layerMode = 1, ca.group = 1;
    s.channelGroups = {"Drums", "Synths"};
    Json j;
    expect(jsonParse(jsonStringify(songToJson(s)), j), "round-trip parses");
    Song back;
    expect(songFromJson(j, back) && back == s, "note properties, channel settings, layers and groups survive a save/load");

    // Defaults write nothing new (older readers see the same project).
    Song plain = baseSong();
    ObjectId p = addChannel(plain, "gl.synth");
    plain.patterns[0].notes[p] = {mk(0, 96, 60)};
    std::string text = jsonStringify(songToJson(plain));
    bool clean = true;
    for (const char* k : {"\"slide\"", "\"modX\"", "\"fine\"", "\"layer\"", "\"cutGroup\"", "\"channelGroups\"", "\"echo\"", "\"group\""})
      clean = clean && text.find(k) == std::string::npos;
    expect(clean, "default notes and channels write no extra keys");
    Json jp;
    jsonParse(text, jp);
    Song pb;
    expect(songFromJson(jp, pb) && pb == plain, "projects without the new keys load with neutral defaults");
  }

  // ------------------------------------------------ compile: properties, echo, time shift, swing mix, layers
  {
    Song s = baseSong();
    ObjectId a = addChannel(s, "gl.synth");
    Note n = mk(0, 96, 60);
    n.fine = 50, n.pan = 32;
    s.patterns[0].notes[a] = {n};
    auto ev = compilePatternChannel(s, s.patterns[0], a);
    expect(ev.size() == 1 && std::fabs(ev[0].pitch - 60.5f) < 1e-4f && std::fabs(ev[0].props.pan - 0.5f) < 1e-4f,
           "fine pitch adds cents to the pitch; pan maps to -1..1");
    Channel& c = s.channels[0];
    c.echo.count = 2, c.echo.feedback = 0.5f, c.echo.time = 0.5f, c.echo.pan = 0.5f, c.echo.pingPong = true, c.echo.pitch = 100;
    ev = compilePatternChannel(s, s.patterns[0], a);
    expect(ev.size() == 3 && ev[1].on == 48 && ev[2].on == 96 && std::fabs(ev[1].velocity - ev[0].velocity * 0.5f) < 1e-4f &&
               std::fabs(ev[2].pitch - 62.5f) < 1e-4f && ev[1].props.pan > 0 && ev[2].props.pan < ev[1].props.pan,
           "echo repeats notes with feedback, pitch steps and ping-pong pan");
    c.echo = ChannelEcho{};
    c.timeShift = 0.25f;
    ev = compilePatternChannel(s, s.patterns[0], a);
    expect(ev.size() == 1 && ev[0].on == 24 && ev[0].off == 120, "time shift delays the channel's notes");
    c.timeShift = 0;
    s.swing = 1.0f;
    s.patterns[0].steps[a] = std::vector<uint8_t>(32, 0);
    s.patterns[0].steps[a][1] = 100;
    c.swingMix = 0.5f;
    ev = compilePatternChannel(s, s.patterns[0], a);
    bool swung = false;
    for (const auto& e : ev) swung = swung || e.on == 24 + 6;  // half of the full swing (12 ticks)
    expect(swung, "channel swing mix scales the song swing");
    // Odd steps per bar: swing pairs restart at each bar line, so bar 2's downbeat stays on the grid.
    Pattern odd = s.patterns[0];
    odd.bars = 2, odd.stepsPerBar = 5;
    odd.notes.clear(), odd.stepProps.clear(), odd.stepLoop.clear();
    odd.steps[a] = std::vector<uint8_t>(10, 0);
    odd.steps[a][1] = odd.steps[a][5] = odd.steps[a][6] = 100;
    c.swingMix = 1.0f;
    ev = compilePatternChannel(s, odd, a);
    const Tick st = s.ticksPerStep(odd);
    expect(ev.size() == 3 && ev[0].on == st + st / 2 && ev[1].on == 5 * st && ev[2].on == 6 * st + st / 2,
           "swing pairs restart at each bar line");
  }
  {
    Song s = baseSong();
    ObjectId l = addChannel(s, "gl.synth"), k1 = addChannel(s, "gl.synth"), k2 = addChannel(s, "gl.synth");
    s.channels[0].plugin = PluginState{};
    s.channels[0].layer = {k1, k2};
    std::vector<Note> ns;
    for (int i = 0; i < 16; ++i) ns.push_back(mk(i * 24, 24, 60 + i % 5));
    s.patterns[0].notes[l] = ns;
    expect(compilePatternChannel(s, s.patterns[0], k1).size() == 16 && compilePatternChannel(s, s.patterns[0], k2).size() == 16,
           "layer: every child gets the layer's notes");
    s.channels[0].layerMode = 1;
    size_t n1 = compilePatternChannel(s, s.patterns[0], k1).size(), n2 = compilePatternChannel(s, s.patterns[0], k2).size();
    expect(n1 + n2 == 16 && n1 > 0 && n2 > 0, "random layer: each note goes to exactly one child");
    s.channels[1].layer = {l};  // cycle through a child: must terminate
    expect(compilePatternChannel(s, s.patterns[0], k2).size() == n2, "layer cycles are ignored");
  }

  // ------------------------------------------------ engine: slide notes glide
  const std::string a441 = sineWav("a441", 441.0), a2 = sineWav("silent", 441.0, 0.0);
  {
    Song s = baseSong();
    ObjectId ch = addChannel(s, "gl.sampler", a441);
    Note slide = mk(0, 96, 72);  // one beat = 0.5 s at 120 BPM
    slide.slide = true;
    s.patterns[0].notes[ch] = {mk(0, 384, 60), slide};
    AudioBuffer b = render(s);
    double f0 = freqAt(b, 0.0, 0.03), fm = freqAt(b, 0.22, 0.28), fe = freqAt(b, 0.6, 0.9);
    std::printf("# slide: %.1f Hz → %.1f Hz → %.1f Hz\n", f0, fm, fe);
    expect(f0 < 470 && fm > 560 && fm < 700 && std::fabs(fe - 882) < 15, "slide note glides the sounding note an octave up over its length");
    // A slide note alone does not start a note.
    s.patterns[0].notes[ch] = {slide};
    expect(render(s).peak() < 1e-4f, "a slide note with nothing sounding is silent");
  }

  // ------------------------------------------------ cut groups
  {
    Song s = baseSong();
    ObjectId victim = addChannel(s, "gl.sampler", a441), cutter = addChannel(s, "gl.sampler", a2);
    s.patterns[0].notes[victim] = {mk(0, 768, 60)};
    s.patterns[0].notes[cutter] = {mk(192, 24, 60)};  // beat 3 = 1.0 s
    AudioBuffer free = render(s);
    s.channels[0].cutBy = 4;
    s.channels[1].cutGroup = 4;
    AudioBuffer cut = render(s);
    expect(rms(free, 0, 1.1, 1.4) > 0.2 && rms(cut, 0, 0.5, 0.95) > 0.2 && rms(cut, 0, 1.02, 1.4) < 1e-3,
           "cut group: the cutter's note chokes the cut-by channel (also one-shot samples)");
    // Self cut: a retrigger cuts the previous note, notes starting together survive.
    Song t = baseSong();
    ObjectId c = addChannel(t, "gl.sampler", a441);
    t.channels[0].cutGroup = t.channels[0].cutBy = 1;
    t.patterns[0].notes[c] = {mk(0, 96, 60), mk(0, 96, 60)};
    AudioBuffer two = render(t);
    t.patterns[0].notes[c] = {mk(0, 96, 60)};
    AudioBuffer one = render(t);
    expect(std::fabs(rms(two, 0, 0.1, 0.4) / rms(one, 0, 0.1, 0.4) - 2.0) < 0.05, "self cut spares notes that start together");
    t.patterns[0].notes[c] = {mk(0, 96, 60), mk(96, 96, 72)};
    AudioBuffer re = render(t);
    expect(std::fabs(freqAt(re, 0.7, 0.9) - 882) < 15, "self cut: a retrigger replaces the previous note");
  }

  // ------------------------------------------------ mono, porta, polyphony
  {
    Song s = baseSong();
    ObjectId ch = addChannel(s, "gl.sampler", a441);
    s.patterns[0].notes[ch] = {mk(0, 384, 60), mk(96, 288, 67)};  // overlapping
    AudioBuffer poly = render(s);
    // Two sines sum to ~1.41x the level of one; mono keeps a single voice.
    const double single = rms(poly, 0, 0.1, 0.45), both = rms(poly, 0, 0.7, 0.95);
    s.channels[0].mono = true;
    AudioBuffer mono = render(s);
    std::printf("# mono: single %.3f poly %.3f mono %.3f\n", single, both, rms(mono, 0, 0.7, 0.95));
    expect(std::fabs(freqAt(mono, 0.7, 0.95) - 660.8) < 12 && both > single * 1.3 && std::fabs(rms(mono, 0, 0.7, 0.95) / single - 1) < 0.05,
           "mono: a new note replaces the sounding one");
    s.channels[0].mono = false;
    s.channels[0].polyphony = 1;
    expect(std::fabs(freqAt(render(s), 0.7, 0.95) - 660.8) < 12, "polyphony 1 cuts the oldest note");
    s.channels[0].polyphony = 0;
    s.channels[0].mono = true;
    s.channels[0].porta = true;
    s.channels[0].portaTime = 0.4f;
    s.patterns[0].notes[ch] = {mk(0, 96, 60), mk(96, 192, 72)};  // legato, octave up at 0.5 s
    AudioBuffer gl = render(s);
    double fmid = freqAt(gl, 0.66, 0.74), fend = freqAt(gl, 1.0, 1.3);
    std::printf("# porta: mid %.1f Hz, end %.1f Hz\n", fmid, fend);
    expect(fmid > 560 && fmid < 760 && std::fabs(fend - 882) < 15, "mono + porta: legato notes glide over the porta time");
    // A per-note porta flag on a polyphonic channel.
    s.channels[0].mono = false;
    s.channels[0].porta = false;
    Note pn = mk(192, 192, 72);
    pn.porta = true;
    s.patterns[0].notes[ch] = {mk(0, 96, 60), pn};
    AudioBuffer pp = render(s);
    double fp = freqAt(pp, 1.15, 1.25);
    expect(fp > 560 && fp < 760 && std::fabs(freqAt(pp, 1.5, 1.9) - 882) < 15, "porta note glides from the previous note");
  }

  // ------------------------------------------------ per-note pan, mod X / Y
  {
    Song s = baseSong();
    ObjectId ch = addChannel(s, "gl.sampler", a441);
    Note n = mk(0, 384, 60);
    n.pan = -kNotePanMax;
    s.patterns[0].notes[ch] = {n};
    AudioBuffer b = render(s);
    expect(rms(b, 0, 0.1, 0.5) > 0.2 && rms(b, 1, 0.1, 0.5) < 1e-4, "note pan (sampler): hard left");
    // Instrument without per-voice pan: the generator output follows the note's pan.
    Song k = baseSong();
    ObjectId kc = addChannel(k, "gl.kick");
    Note kn = mk(0, 96, 60);
    kn.pan = kNotePanMax;
    k.patterns[0].notes[kc] = {kn};
    AudioBuffer kb = render(k);
    std::printf("# kick pan: L %.4f R %.4f\n", rms(kb, 0, 0.0, 0.3), rms(kb, 1, 0.0, 0.3));
    expect(rms(kb, 1, 0.0, 0.3) > 0.01 && rms(kb, 0, 0.0, 0.3) < rms(kb, 1, 0.0, 0.3) * 0.1, "note pan on any instrument (output pan)");
    // Mod X lowers the sampler's filter cutoff.
    s.channels[0].plugin.params[pp::SmpFilterType] = 1;
    s.channels[0].plugin.params[pp::SmpCutoff] = 2000;
    n.pan = 0;
    s.patterns[0].notes[ch] = {n};
    double open = rms(render(s), 0, 0.1, 0.5);
    n.modX = 0;
    s.patterns[0].notes[ch] = {n};
    double closed = rms(render(s), 0, 0.1, 0.5);
    expect(closed < open * 0.5, "mod X moves the sampler cutoff (per note)");
  }

  // ------------------------------------------------ layer triggering + live notes
  {
    Song s = baseSong();
    ObjectId l = addChannel(s, "gl.synth"), child = addChannel(s, "gl.sampler", a441);
    s.channels[0].plugin = PluginState{};
    s.channels[0].layer = {child};
    s.patterns[0].notes[l] = {mk(0, 192, 72)};
    AudioBuffer b = render(s);
    expect(std::fabs(freqAt(b, 0.1, 0.4) - 882) < 15, "layer channel notes play on its child");
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(s));
    eng.noteOn(l, 60, 0.8f);
    std::vector<float> L(4096), R(4096);
    eng.process(L.data(), R.data(), 4096);
    float pk = 0;
    for (float v : L) pk = std::max(pk, std::fabs(v));
    expect(pk > 0.05f, "live notes on a layer channel play its children");
  }

  // ------------------------------------------------ block-size invariance (everything together)
  {
    Song s = baseSong();
    ObjectId smp = addChannel(s, "gl.sampler", a441), syn = addChannel(s, "gl.synth"), lay = addChannel(s, "gl.synth"),
             hat = addChannel(s, "gl.sampler", a441);
    s.channels[2].plugin = PluginState{};
    s.channels[2].layer = {syn};
    s.channels[1].mono = true, s.channels[1].porta = true, s.channels[1].portaTime = 0.07f;
    s.channels[1].echo.count = 2, s.channels[1].echo.feedback = 0.6f, s.channels[1].echo.time = 0.33f, s.channels[1].echo.pan = 0.4f;
    s.channels[0].cutBy = 3, s.channels[3].cutGroup = 3, s.channels[3].timeShift = 0.1f;
    Note sl = mk(30, 70, 67);
    sl.slide = true;
    Note pn = mk(100, 50, 55);
    pn.pan = 40, pn.fine = -30, pn.modX = 60;
    s.patterns[0].notes[smp] = {mk(0, 300, 60), sl, pn};
    s.patterns[0].notes[lay] = {mk(10, 40, 48), mk(40, 60, 55), mk(170, 100, 50)};
    s.patterns[0].notes[hat] = {mk(77, 10, 60), mk(250, 10, 60)};
    AudioBuffer ref = render(s, 512, 96);
    expect(ref.peak() > 0.05f, "combined song sounds");
    expect(render(s, 1, 96).data == ref.data && render(s, 64, 96).data == ref.data && render(s, 1000, 96).data == ref.data,
           "identical output for block sizes 1, 64, 512, 1000");
  }

  // ------------------------------------------------ FLP import of these fields
  {
    Builder B;
    B.text(199, "25.2.3.5164");
    B.d(156, 120000);
    B.text(231, "Drums");
    B.text(231, "Keys");
    B.w(65, 1);
    std::vector<uint8_t> notes;
    for (auto n : {flNote(0, 1, 96, 60, 0x4000, 130, 100, 0x83, 0, 128, 0, 255), flNote(0, 1, 48, 72, 0x4008, 120, 64, 0, 64, 100, 128, 128),
                   flNote(96, 0, 0, 60, 0x4000, 120, 64, 0, 64, 100, 128, 128), flNote(192, 3, 96, 64, 0x4000, 120, 64, 0, 64, 100, 128, 128)})
      notes.insert(notes.end(), n.begin(), n.end());
    B.data(224, notes);
    // Channel 0: sampler — levels, fine tune, cut group, echo, root, reverse, fades, volume envelope, filter, display group 0.
    B.w(64, 0), B.b(21, 0), B.text(192, "Hat");
    B.data(219, i32s({12800, 6400, 150, 128, 64, 3}));  // pan right, half volume, +150 cents, HP at mod X 128
    B.data(229, i32s({0, 12800, 0, 0, 0}));
    B.d(142, static_cast<uint32_t>(-20));
    B.d(132, 0x00020001);  // cut 1, cut by 2
    B.data(209, i32s({6400, 6400 + 3200, 100, 3, 48}));  // 50 % feedback, pan right half, +100 ct, 3 echoes, 48 ticks
    B.d(135, 72);
    B.w(70, 2 | 4 | 1);    // reverse, boost clip, stereo fade
    B.w(76, 256), B.w(75, 512);
    // Precomputed effects: tilt, cutoff, boost, resonance, stereo delay, warp, ring, echo mod, reverb.
    B.w(69, 192), B.w(71, 512), B.w(74, 64), B.w(83, 320), B.w(85, 2048 + 441), B.w(86, 128);
    B.d(131, (24u << 16) | 128u), B.d(138, (192u << 16) | 64u), B.d(139, (0u << 16) | 64u);
    B.w(97, 64), B.w(89, 512);
    B.data(221, {0, 0, 0, 0, 0, 4, 0, 0, 3});  // mono + porta, porta 1024
    {
      std::vector<uint8_t> env(68, 0);
      auto e32 = [&](size_t at, int32_t v) { put32(env, at, static_cast<uint32_t>(v)); };
      for (int k = 0; k < 5; ++k) {
        std::fill(env.begin(), env.end(), 0);
        e32(4, k == 1 ? -1 : 0);  // volume envelope on
        e32(12, 100), e32(16, 65536), e32(20, 100), e32(24, 0), e32(28, 100), e32(48, 32950);
        B.data(218, env);
      }
    }
    B.d(145, 0);
    // Channel 1: plugin channel with an arpeggiator, display group 1, self cut (ignored).
    B.w(64, 1), B.b(21, 2), B.text(192, "Keys");
    {
      std::vector<uint8_t> par(168, 0);
      put32(par, 40, 1), put32(par, 44, 2), put32(par, 52, 512), put32(par, 56, 50), put32(par, 92, 2);
      B.data(215, par);
    }
    B.d(132, 0x00050005);
    B.d(145, 1);
    // Channel 2: layer of channels 0 and 1 (random).
    B.w(64, 2), B.b(21, 3), B.text(192, "Layer");
    B.w(94, 0), B.w(94, 1), B.d(144, 1);
    // Channel 3: another sampler (default settings).
    B.w(64, 3), B.b(21, 0), B.text(192, "Plain");
    // Remote-control links: CC 7 on MIDI channel 3, port 2 → channel 0 volume (note mode latch);
    // pitch bend → the focused channel's pitch (not imported).
    B.data(226, i32s({static_cast<int32_t>((3u << 22) | (2u << 16) | 7u), 0, 0x00000000, 4 | (2 << 8), 469}));
    B.data(226, i32s({255, 255, 0x0FFF0004, 4, -512}));
    std::string path = "test_channels.flp";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      auto bytes = B.file(96);
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
    }
    Song s;
    flp::Report rep;
    std::string err;
    bool ok = importFlp(path, s, rep, &err, "/nonexistent");
    std::remove(path.c_str());
    expect(ok && s.channels.size() == 4, "synthetic project imports");
    const Channel& hat = s.channels[0];
    const Channel& keys = s.channels[1];
    const Channel& lay = s.channels[2];
    expect(std::fabs(hat.pan - 1.0f) < 1e-4f && std::fabs(hat.gain - 0.5f) < 1e-4f && hat.pitch == 1 && hat.fine == 30,
           "levels: pan, volume, pitch + fine tune (cents split into semitones and fine)");
    expect(hat.cutGroup == 1 && hat.cutBy == 2 && keys.cutGroup == 0 && keys.cutBy == 0, "cut groups (plugin self-cut ignored)");
    expect(hat.echo.count == 3 && std::fabs(hat.echo.feedback - 0.5f) < 1e-4f && std::fabs(hat.echo.time - 0.5f) < 1e-4f &&
               std::fabs(hat.echo.pan - 0.5f) < 1e-4f && hat.echo.pitch == 100,
           "echo delay");
    expect(std::fabs(hat.swingMix - 0.5f) < 1e-4f && std::fabs(hat.timeShift - 0.125f) < 1e-4f, "swing mix and time shift");
    expect(hat.mono && hat.porta && hat.portaTime > 0.4f, "polyphony: mono + porta + porta time");
    const auto& sp = hat.plugin.params;
    std::printf("# hat: oneshot %g hold %g sus %g ftype %g reso %g\n", sp[pp::SmpOneShot], sp[pp::SmpEnvHold], sp[pp::SmpSustain], sp[pp::SmpFilterType], sp[pp::SmpReso]);
    expect(hat.plugin.type == "gl.sampler" && sp[pp::SmpRoot] == 72 && sp[pp::SmpReverse] == 1 && std::fabs(sp[pp::SmpPreRampIn] - 1.0f) < 1e-4f &&
               std::fabs(sp[pp::SmpPreRampOut] - 2.0f) < 1e-4f && sp[pp::SmpFadeIn] == 0 && sp[pp::SmpFadeOut] == 0,
           "sampler: root note, reverse, quick fades (whole-sample ramps)");
    expect(sp[pp::SmpPreClip] == 1 && sp[pp::SmpPreFadeStereo] == 1 && std::fabs(sp[pp::SmpPreTilt] - 0.5f) < 1e-4f &&
               std::fabs(sp[pp::SmpPreCutoff] - 0.5f) < 1e-4f && std::fabs(sp[pp::SmpPreBoost] - 0.25f) < 1e-4f &&
               std::fabs(sp[pp::SmpPreReso] - 0.5f) < 1e-4f && std::fabs(sp[pp::SmpPreStereoDelay] - 10.0f) < 1e-3f &&
               std::fabs(sp[pp::SmpPreWarp] + 0.5f) < 1e-4f,
           "sampler: precomputed tilt, cutoff, boost, resonance, stereo delay, warp, clip, stereo fade");
    expect(std::fabs(sp[pp::SmpPreRingMix] - 0.5f) < 1e-4f && std::fabs(sp[pp::SmpPreRingFreq] - 32 * 44100.0f / 5760) < 1e-2f &&
               std::fabs(sp[pp::SmpPreReverbMix] - 0.25f) < 1e-4f && sp[pp::SmpPreReverbType] == 0,
           "sampler: precomputed ring modulation and reverb");
    expect(s.links.size() == 1 && s.links[0].source == RemoteSource::CC && s.links[0].cc == 7 && s.links[0].midiChannel == 2 &&
               s.links[0].port == 2 && s.links[0].owner == hat.id && s.links[0].button == RemoteButton::Toggle &&
               s.links[0].max < 0.81f && s.links[0].max > 0.79f,
           "remote links: fixed-target link imported (source, channel, port, note mode, volume range); focused-channel link skipped");
    {
      flp::Event e;
      std::vector<uint8_t> raw = i32s({256 + 60, 0, 0x70071F01, 4, 469});
      e.id = 226, e.data = raw.data(), e.size = static_cast<uint32_t>(raw.size());
      flp::FlRemoteLink r;
      flp::FlTarget t;
      t.id = {hat.id, 3};
      ControllerLink l;
      expect(flp::decodeRemoteLink(e, r) && r.port == -1 && r.control == 316 && r.target == 0x70071F01u && !r.focused() &&
                 flp::remoteLinkToController(r, t, l) && l.source == RemoteSource::Note && l.cc == 60 && l.port == -1,
             "remote links: note source, any port, mixer target");
      r.control = 200;
      expect(!flp::remoteLinkToController(r, t, l), "remote links: unknown control refused");
      r.target = 0x3FC71F01;
      expect(r.focused(), "remote links: focused mixer track target recognised");
    }
    expect(std::fabs(hat.echo.modX + 0.5f) < 1e-4f && std::fabs(hat.echo.modY - 0.5f) < 1e-4f, "echo: mod X / Y per echo");
    expect(sp[pp::SmpOneShot] == 0 && sp[pp::SmpEnvHold] > 5 && sp[pp::SmpSustain] < 1e-4f && sp[pp::SmpFilterType] == 4 &&
               std::fabs(sp[pp::SmpReso] - 0.25f) < 1e-3f,
           "sampler: volume envelope gates the note; channel filter (high pass)");
    expect(s.channels[3].plugin.params[pp::SmpOneShot] == 1 && s.channels[3].plugin.params[pp::SmpFilterType] == 0,
           "sampler without envelope plays one-shot, filter off");
    expect(keys.noteFx.size() == 1 && keys.noteFx[0].type == "gl.arp" && keys.noteFx[0].params[1] == 1 && keys.noteFx[0].params[2] == 5 &&
               keys.noteFx[0].params[4] == 2,
           "arpeggiator → note processor (range, rate, repeats)");
    expect(lay.isLayer() && lay.layer.size() == 2 && lay.layer[0] == hat.id && lay.layer[1] == keys.id && lay.layerMode == 1 &&
               lay.plugin.type.empty(),
           "layer channel: children and random mode");
    expect(s.channelGroups.size() == 2 && s.channelGroups[1] == "Keys" && hat.group == 0 && keys.group == 1 && lay.group == -1,
           "display groups");
    const auto& ns = s.patterns.at(0).notes;
    const Note& p = ns.at(keys.id)[0];
    expect(p.fine == 100 && p.release == 100 && p.color == 3 && p.pan == -64 && p.velocity == 127 && p.modX == 0 && p.modY == 255 && !p.slide,
           "note properties: fine, release, colour, pan, velocity, mod X / Y");
    expect(ns.at(keys.id)[1].slide && ns.at(keys.id)[1].release == kNoteReleaseDefault, "slide flag");
    expect(ns.at(hat.id)[0].length == 24, "step notes (length 0) last one step");
    expect(ns.count(s.channels[3].id) && rep.counts["notes: slide"] == 1, "notes on channels after a layer map correctly; report counts");
  }
  // ------------------------------------------------ J-74: the slicer keeps a note's fine pitch
  {
    auto play = [&](int fine) {
      Song s = baseSong();
      ObjectId ch = addChannel(s, "gl.slicer", a441);
      Note n = mk(0, 192, 60);
      n.fine = fine;
      s.patterns[0].notes[ch].push_back(n);
      return freqAt(render(s), 0.1, 0.4);
    };
    const double f0 = play(0), f40 = play(40);
    expect(f0 > 400 && std::fabs(f40 / f0 - std::pow(2.0, 40.0 / 1200.0)) < 0.004, "J-74: slicer note fine pitch (+40 cents) tunes the slice");
  }
  // ------------------------------------------------ J-74: FL velocity law, fine pitch, foreign sample paths
  {
    namespace fs = std::filesystem;
    const std::string dir = "test_j74_proj";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir + "/Samples", ec);
    fs::copy_file(a441, dir + "/Samples/kick.wav", ec);
    fs::copy_file(a441, dir + "/loop.wav", ec);
    Builder B;
    B.text(199, "25.1.1.4879");
    B.d(156, 162000);
    B.w(65, 1);
    std::vector<uint8_t> notes;
    // FL velocity bytes 100 (default), 64, 65, 128 (100 %), 12; fine pitch 156 (+360 cents) and 121 (+10).
    const uint8_t vels[] = {100, 64, 65, 128, 12};
    for (int k = 0; k < 5; ++k) {
      auto n = flNote(static_cast<uint32_t>(k * 48), 0, 48, static_cast<uint16_t>(60 + k), 0x4000, k == 0 ? 156 : k == 1 ? 121 : 120, 64,
                      0, 64, vels[k], 128, 128);
      notes.insert(notes.end(), n.begin(), n.end());
    }
    B.data(224, notes);
    // Samples saved on another machine: absolute Windows paths into the project's own folder.
    B.w(64, 0), B.b(21, 0), B.text(203, "Kick");
    B.text(196, "C:\\Users\\Someone Else\\Music\\Old Place\\My Song\\Samples\\kick.wav");
    B.w(64, 1), B.b(21, 0), B.text(203, "Loop");
    B.text(196, "D:\\elsewhere\\loop.wav");
    B.w(64, 2), B.b(21, 0), B.text(203, "Gone");
    B.text(196, "C:\\Users\\Someone Else\\missing.wav");
    const std::string path = dir + "/song.flp";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      auto bytes = B.file(96);
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
    }
    Song s;
    flp::Report rep;
    std::string err;
    const bool ok = importFlp(path, s, rep, &err, "/nonexistent");
    expect(ok && s.channels.size() == 3 && s.patterns.size() == 1, "J-74: synthetic project imports");
    if (ok && s.channels.size() == 3 && s.patterns.size() == 1) {
      const auto& ns = s.patterns[0].notes.at(s.channels[0].id);
      expect(ns.size() == 5 && ns[0].velocity == 100 && ns[1].velocity == 64 && ns[2].velocity == 65 && ns[3].velocity == 127 &&
                 ns[4].velocity == 12,
             "J-74: FL velocity is the MIDI velocity (100 stays 100, 64 / 65 stay apart, 128 → 127)");
      expect(ns.size() == 5 && ns[0].fine == 360 && ns[1].fine == 10 && ns[2].fine == 0, "J-74: note fine pitch (10 cents per step)");
      expect(fs::path(s.channels[0].plugin.data) == fs::path(dir + "/Samples/kick.wav"),
             "J-74: a foreign absolute sample path resolves into the project's Samples folder");
      expect(fs::path(s.channels[1].plugin.data) == fs::path(dir + "/loop.wav"), "J-74: … or next to the project");
      expect(s.channels[2].plugin.data == "C:/Users/Someone Else/missing.wav", "J-74: a sample found nowhere keeps its path");
      // FL export without the original events: the velocity bytes come back as FL stored them.
      flp::ExportOptions opt;
      opt.useOrigin = false;
      opt.projectDir = dir;
      std::vector<uint8_t> out;
      flp::ExportReport xr;
      flp::File f;
      std::vector<int> back;
      if (flp::exportFlp(s, out, xr, opt) && flp::readBytes(out, f, &err))
        for (const auto& e : f.events)
          if (e.id == 224)
            for (uint32_t o = 0; o + 24 <= e.size; o += 24) back.push_back(e.u8(o + 21) | ((e.u8(o + 16)) << 8));
      expect(back.size() == 5 && (back[0] & 0xFF) == 100 && (back[1] & 0xFF) == 64 && (back[2] & 0xFF) == 65 && (back[3] & 0xFF) == 127 &&
                 (back[4] & 0xFF) == 12 && (back[0] >> 8) == 156 && (back[1] >> 8) == 121,
             "J-74: exported velocity / fine pitch bytes match FL's");
    }
    fs::remove_all(dir, ec);
  }
  std::remove(a441.c_str());
  std::remove(a2.c_str());
  // ------------------------------------------------ precomputed sample effects (J-23)
  {
    auto mk = [](size_t n, bool stereo) {
      auto smp = std::make_shared<Sample>();
      smp->sampleRate = 44100;
      smp->left.resize(n), smp->right.resize(n);
      for (size_t i = 0; i < n; ++i) {
        smp->left[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 440.0 * i / 44100.0));
        smp->right[i] = stereo ? static_cast<float>(0.25 * std::sin(2 * 3.14159265358979 * 660.0 * i / 44100.0)) : smp->left[i];
      }
      return SamplePtr(smp);
    };
    SamplePtr in = mk(44101, false);
    SamplePrecomputeFx none;
    expect(precomputeSample(in, none) == in, "precompute: neutral settings keep the sample");
    // Quick fade out (raw 1024 = 4): silent from a quarter of the sample on; linear from 1 before that.
    SamplePrecomputeFx fo;
    fo.rampOut = 4;
    SamplePtr o = precomputeSample(in, fo);
    const size_t N = in->frames() - 1;
    expect(o->left[N / 4 + 10] == 0.f && std::fabs(o->left[N / 8] - in->left[N / 8] * 0.5f) < 1e-5f, "precompute: ramp out law (1 - v/256 · n/N)");
    SamplePrecomputeFx fi;
    fi.rampIn = 1;  // raw 256: ramps over the whole sample
    o = precomputeSample(in, fi);
    expect(std::fabs(o->left[N / 2] - in->left[N / 2] * 0.5f) < 1e-5f && o->left[0] == 0.f, "precompute: ramp in law");
    SamplePrecomputeFx bo;
    bo.boost = 0.25;  // raw 64 → ×5
    bo.clip = true;
    o = precomputeSample(in, bo);
    float pk = 0;
    for (float v : o->left) pk = std::max(pk, std::fabs(v));
    expect(std::fabs(o->left[3] - std::clamp(in->left[3] * 5.f, -1.f, 1.f)) < 1e-5f && pk <= 1.f, "precompute: boost ×(1 + v/16), clip");
    SamplePrecomputeFx sd;
    sd.stereoDelayMs = 10;  // 441 frames, left later
    o = precomputeSample(in, sd);
    expect(o->frames() == in->frames() + 441 && o->left[441 + 100] == in->left[100] && o->right[100] == in->right[100],
           "precompute: stereo delay shifts the left channel and grows the sample");
    SamplePrecomputeFx lp;
    lp.cutoff = 16.0 / 1024;
    o = precomputeSample(mk(44101, true), lp);
    double eIn = 0, eOut = 0;
    for (size_t i = 1000; i < 40000; ++i) eIn += in->left[i] * in->left[i], eOut += o->left[i] * o->left[i];
    expect(eOut < eIn * 0.1, "precompute: low pass at the lowest cutoff attenuates 440 Hz");
    SamplePrecomputeFx off;
    off.cutoff = 1;
    off.tilt = 0;
    off.ringMix = 0;
    expect(precomputeSample(in, off) == in, "precompute: cutoff fully open is neutral");
    SamplePrecomputeFx rm;
    rm.ringMix = 1;
    o = precomputeSample(in, rm);
    expect(o->left == o->right && std::fabs(o->left[0]) < 1e-6f, "precompute: ring modulation (mono stays mono, starts at phase 0)");
    SamplePrecomputeFx wp;
    wp.warp = 1;
    o = precomputeSample(in, wp);
    bool fin = true;
    for (float v : o->left) fin = fin && std::isfinite(v);
    expect(fin && o->frames() == in->frames() && std::fabs(o->left[N] - in->left[N]) < 1e-3f, "precompute: time warp keeps the ends");
    SamplePrecomputeFx tl;
    tl.tilt = 1;  // raw 256 → k = (192 − 96) / 128 = 0.75
    o = precomputeSample(in, tl);
    expect(std::fabs(o->left[1] - (in->left[1] + 0.75f * in->left[0])) < 1e-6f, "precompute: tilt one-pole feedback law");
    SamplePrecomputeFx rv;
    rv.reverbMix = 0.5;
    o = precomputeSample(in, rv);
    expect(o->frames() > in->frames(), "precompute: reverb adds a tail");
    SamplePrecomputeFx fs;
    fs.fadeStereo = true;
    o = precomputeSample(in, fs);
    expect(std::fabs(o->right[0]) < 1e-6f && std::fabs(o->left[N]) < 1e-3f, "precompute: stereo fade (left out, right in)");
  }

  return finish();
}
