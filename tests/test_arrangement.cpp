// Gridline — arrangement: automation curves and lanes, tempo map, audio and
// automation clips in the engine, project round-trip, FLP arrangement import
// (synthetic files built here), sample decoders (WavPack / Ogg when FL Studio
// content is installed; skipped otherwise).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/Json.h"
#include "../src/core/Params.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "../src/engine/WavPack.h"
#include "Check.h"

using namespace gl;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// Writes a stereo 16-bit WAV of `frames` frames produced by fn(i).
template <typename F>
std::string writeTestWav(const std::string& name, size_t frames, F&& fn) {
  AudioBuffer b(2, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = b.at(1, i) = fn(i);
  std::string path = (std::filesystem::temp_directory_path() / name).string();
  writeWav(path, b, 44100, 16);
  sampleCache().clear();
  return path;
}

// A song with a master at unity, one sampler channel (unity strip) holding `sample`.
Song audioSong(const std::string& sample, double tempo = 120) {
  Song s = makeEmptySong();
  s.tempo = tempo;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Audio";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = sample;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return s;
}

Clip audioClip(Song& s, Tick start, Tick len) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = s.channels.back().id;
  c.start = start;
  c.length = len;
  return c;
}

int firstAbove(const AudioBuffer& b, float thr) {
  for (size_t i = 0; i < b.frames; ++i)
    if (std::fabs(b.at(0, i)) > thr) return static_cast<int>(i);
  return -1;
}

// FLP builder (same container helpers as test_flp).
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
struct Bytes {
  std::vector<uint8_t> v;
  explicit Bytes(size_t n) : v(n, 0) {}
  void u16(size_t at, uint16_t x) { v[at] = x & 0xFF, v[at + 1] = x >> 8; }
  void u32(size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i)); }
  void f32(size_t at, float x) { uint32_t b; std::memcpy(&b, &x, 4); u32(at, b); }
  void f64(size_t at, double x) { uint64_t b; std::memcpy(&b, &x, 8); u32(at, static_cast<uint32_t>(b)), u32(at + 4, static_cast<uint32_t>(b >> 32)); }
};
// One 80-byte playlist record.
std::vector<uint8_t> item(uint32_t pos, uint16_t idx, uint32_t len, int track, uint16_t flags) {
  Bytes r(80);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, idx);
  r.u32(8, len);
  r.u16(12, static_cast<uint16_t>(499 - track));
  r.u16(18, flags);
  r.f32(52, 1.0f);
  r.f64(64, 1.0);
  return r.v;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- curves
  for (AutoShape sh : {AutoShape::Curve, AutoShape::SCurve, AutoShape::Smooth})
    for (double t : {-1.0, -0.3, 0.0, 0.5, 1.0})
      expect(near(autoShape(sh, t, 0), 0) && near(autoShape(sh, t, 1), 1), "shapes run from 0 to 1");
  expect(near(autoShape(AutoShape::Curve, 0, 0.25), 0.25), "zero tension is linear");
  expect(autoShape(AutoShape::Curve, 0.5, 0.5) < 0.5 && autoShape(AutoShape::Curve, -0.5, 0.5) > 0.5,
         "tension bends the curve (positive: slow start)");
  {
    bool mono = true;
    double prev = -1;
    for (int i = 0; i <= 100; ++i) {
      double v = autoShape(AutoShape::Curve, 0.8, i / 100.0);
      mono = mono && v >= prev;
      prev = v;
    }
    expect(mono, "single curve is monotonic");
  }
  expect(near(autoShape(AutoShape::Hold, 0, 0.999), 0) && near(autoShape(AutoShape::Hold, 0, 1), 1), "hold jumps at the point");

  AutomationClip ac;
  ac.points = {{0, 0.2f, 0, AutoShape::Curve}, {96, 1.0f, 0, AutoShape::Curve}, {192, 0.0f, 0.6f, AutoShape::Curve},
               {288, 0.5f, 0, AutoShape::Hold}};
  expect(near(automationValue(ac, -10), 0.2, 1e-6) && near(automationValue(ac, 1000), 0.5, 1e-6), "holds outside the points");
  expect(near(automationValue(ac, 48), 0.6, 1e-6), "linear interpolation");
  expect(near(automationValue(ac, 144), 1.0 - autoShape(AutoShape::Curve, 0.6, 0.5), 1e-6), "tension applied to the segment ending at the point");
  expect(near(automationValue(ac, 287), 0.0, 1e-6) && near(automationValue(ac, 288), 0.5, 1e-6), "hold segment");
  {
    AutomationClip x;
    insertAutoPoint(x, {100, 0.1f});
    insertAutoPoint(x, {50, 0.2f});
    int k = insertAutoPoint(x, {100, 0.3f});
    expect(x.points.size() == 3 && x.points[0].pos == 50 && k == 2, "insertAutoPoint keeps order");
  }

  // ---------------------------------------------------------------- lanes
  {
    Song s = makeEmptySong();
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = s.masterId();
    a.index = static_cast<uint32_t>(MixerParam::Volume);
    a.min = 0.2f, a.max = 0.6f;
    a.points = {{0, 0.0f}, {384, 1.0f}};
    s.automations.push_back(a);
    Clip c1;
    c1.id = s.allocateId(), c1.kind = ClipKind::Automation, c1.source = a.id, c1.start = 384, c1.length = 384;
    Clip c2 = c1;
    c2.id = s.allocateId(), c2.start = 1536, c2.offset = 192;
    Clip c3 = c1;
    c3.id = s.allocateId(), c3.start = 3000, c3.muted = true;
    s.playlist = {c2, c1, c3};
    auto lanes = automationLanes(s);
    expect(lanes.size() == 1 && lanes[0].inst.size() == 2 && lanes[0].inst[0].start == 384, "lane: sorted, muted clip skipped");
    const auto& L = lanes[0];
    expect(near(L.valueAt(0), 0.2, 1e-6), "lane: before the first clip → its start value");
    expect(near(L.valueAt(384 + 192), 0.4, 1e-6), "lane: inside a clip, mapped through min/max");
    expect(near(L.valueAt(1000), 0.6, 1e-6), "lane: after a clip → its end value");
    expect(near(L.valueAt(1536), 0.4, 1e-6), "lane: clip start offset");
    s.playlist[2].muted = false;
    s.playlist[2].start = 400;  // overlaps c1: the later clip wins
    lanes = automationLanes(s);
    expect(lanes[0].inst.size() == 3 && lanes[0].inst[0].end == 400 && near(lanes[0].valueAt(400), 0.2, 1e-6), "lane: overlap resolved");
  }

  // ---------------------------------------------------------------- tempo map
  {
    TempoMap m;
    m.setConstant(120, 96);
    const double sr = 44100;
    double spt = sr * 60.0 / (120 * 96);
    expect(m.constant() && near(m.secondsAt(192), 1.0) && near(m.tickAtSeconds(2.0), 384), "constant map");
    expect(m.samplesBetween(10, 1000, sr) == (1000 - 10) * spt && m.tickAfterSamples(10, 12345, sr) == 10 + 12345 / spt,
           "constant map uses the constant-tempo arithmetic exactly");
    m.setSegments({{0, 120}, {384, 60}, {768, 240}}, 96);
    expect(m.segments().size() == 3 && near(m.secondsAt(384), 2.0) && near(m.secondsAt(480), 3.0) && near(m.secondsAt(768), 6.0) &&
               near(m.secondsAt(768 + 96), 6.25),
           "tempo segments: tick → seconds");
    bool inv = true;
    for (double t = 0; t < 1500; t += 37.5) inv = inv && near(m.tickAtSeconds(m.secondsAt(t)), t, 1e-6);
    expect(inv, "seconds → tick inverts");
    expect(near(m.samplesBetween(300, 500, sr), (m.secondsAt(500) - m.secondsAt(300)) * sr, 1e-6), "samples across a tempo change");
    expect(near(m.tickAfterSamples(300, 2.0 * sr, sr), m.tickAtSeconds(m.secondsAt(300) + 2.0), 1e-6), "tick after samples across segments");
    expect(near(m.bpmAt(383), 120) && near(m.bpmAt(384), 60) && m.signature() != TempoMap().signature(), "bpm lookup + signature");
  }

  // ---------------------------------------------------------------- audio clips
  const std::string click = writeTestWav("gl_arr_click.wav", 22050, [](size_t i) { return i == 0 || i == 4851 ? 1.0f : 0.0f; });
  const std::string dc = writeTestWav("gl_arr_dc.wav", 441000, [](size_t) { return 0.5f; });
  {
    Song s = audioSong(click);
    s.playlist.push_back(audioClip(s, 384, 192));  // 2.0 s at 120 BPM
    RenderOptions o;
    AudioBuffer out = renderSong(s, o);
    expect(firstAbove(out, 0.5f) == 88200, "audio clip starts sample-accurately");
    expect(std::fabs(out.at(0, 88200 + 4851)) > 0.9f, "audio clip plays its sample at natural speed");
    s.playlist[0].audioStart = 0.1;  // 4410 frames in
    out = renderSong(s, o);
    expect(firstAbove(out, 0.5f) == 88200 + 441, "audio clip start offset");
    s.playlist[0].audioStart = 0;
    s.playlist[0].muted = true;
    out = renderSong(s, o);
    expect(firstAbove(out, 0.01f) < 0, "muted audio clip is silent");
    s.playlist[0].muted = false;
    s.playlist[0].gain = 0.5f;
    out = renderSong(s, o);
    expect(std::fabs(out.at(0, 88200) - 0.5f * 32767.f / 32768.f) < 1e-3f, "clip gain");
    // Tempo-synced at the song tempo: identical to plain playback.
    s.playlist[0].gain = 1.0f;
    AudioBuffer plain = renderSong(s, o);
    s.playlist[0].syncBpm = 120;
    AudioBuffer synced = renderSong(s, o);
    float diff = 0;
    for (size_t i = 0; i < plain.frames; ++i) diff = std::max(diff, std::fabs(plain.at(0, i) - synced.at(0, i)));
    expect(diff < 1e-5f, "tempo-synced clip at its own tempo equals plain playback");
    // Song at half the tempo: the synced clip plays half as fast (second click
    // about twice as late; grains smear transients by up to one grain).
    s.tempo = 60;
    s.playlist[0].length = 384;
    AudioBuffer slow = renderSong(s, o);
    int start = 176400, peakAt = -1;
    float pk = 0;
    for (int i = start + 3000; i < start + 15000 && i < static_cast<int>(slow.frames); ++i)
      if (std::fabs(slow.at(0, i)) > pk) pk = std::fabs(slow.at(0, i)), peakAt = i;
    expect(firstAbove(slow, 0.5f) == start && peakAt > start + 2 * 4851 - 2700 && peakAt <= start + 2 * 4851 + 100 &&
               std::fabs(slow.at(0, static_cast<size_t>(start + 4851))) < 0.1f,
           "tempo-synced clip follows the tempo");
    // Pitch shift without a speed change (stretch mode): clip length unchanged, DC level kept.
    Song p = audioSong(dc);
    p.channels[0].plugin.params[pp::SmpStretchMode] = 1;
    p.channels[0].plugin.params[pp::SmpPitchShift] = 700;
    p.playlist.push_back(audioClip(p, 0, 384));
    p.playlist.push_back(audioClip(p, 768, 96));  // keeps the song going after the first clip
    p.playlist.back().muted = true;
    AudioBuffer ps = renderSong(p, o);
    expect(std::fabs(ps.at(0, 30000) - 0.5f) < 0.01f && std::fabs(ps.at(0, 88199 - 200)) > 0.4f && std::fabs(ps.at(0, 88400)) < 1e-6f,
           "granular pitch shift keeps level and length");
    // Reverse plays from the end.
    s = audioSong(click);
    s.playlist.push_back(audioClip(s, 0, 192));
    s.playlist[0].reverse = true;
    out = renderSong(s, o);
    expect(firstAbove(out, 0.5f) == 22050 - 1 - 4851, "reversed clip");
  }

  // ---------------------------------------------------------------- automation in the engine
  {
    Song s = audioSong(dc);
    s.playlist.push_back(audioClip(s, 0, 768));
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = s.masterId();
    a.index = static_cast<uint32_t>(MixerParam::Volume);
    a.max = static_cast<float>(0.8 / 1.25);  // clip 0..1 → volume 0..0.8 (unity)
    a.points = {{0, 0.0f}, {384, 1.0f}};
    s.automations.push_back(a);
    Clip c;
    c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.length = 384, c.track = 1;
    s.playlist.push_back(c);
    RenderOptions o;
    AudioBuffer out = renderSong(s, o);
    auto expected = [](double t) {  // seconds → output
      double v = std::min(1.0, t / 2.0) * 0.8;
      return 0.5 * (v / 0.8) * (v / 0.8);
    };
    bool ok = true;
    for (double t : {0.5, 1.0, 1.5, 2.5, 3.0}) ok = ok && std::fabs(out.at(0, static_cast<size_t>(t * 44100)) - expected(t)) < 0.01;
    expect(ok, "mixer volume follows the automation clip");
    o.blockSize = 37;
    AudioBuffer out2 = renderSong(s, o);
    float diff = 0;
    for (size_t i = 0; i < out.frames; ++i) diff = std::max(diff, std::fabs(out.at(0, i) - out2.at(0, i)));
    expect(diff == 0.0f, "automation is block-size invariant");
    // Muted track: no automation (the volume stays at its value).
    s.tracks[1].muted = true;
    o.blockSize = 512;
    AudioBuffer out3 = renderSong(s, o);
    expect(std::fabs(out3.at(0, 22050) - 0.5f) < 0.01f, "automation on a muted track does nothing");
  }

  // ---------------------------------------------------------------- tempo automation
  {
    Song s = audioSong(click);
    auto td = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = kSongId;
    a.index = static_cast<uint32_t>(SongParam::Tempo);
    a.points = {{0, static_cast<float>(td->toNormalized(120))}, {384, static_cast<float>(td->toNormalized(60)), 0, AutoShape::Hold}};
    s.automations.push_back(a);
    Clip c;
    c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.length = 3072;
    s.playlist.push_back(c);
    s.playlist.push_back(audioClip(s, 480, 96));  // 2 s at 120 + 1 s at 60 BPM
    TempoMap m;
    m.build(s);
    expect(!m.constant() && std::fabs(m.secondsAt(480) - 3.0) < 1e-3, "tempo map from tempo automation");
    RenderOptions o;
    AudioBuffer out = renderSong(s, o);
    expect(std::abs(firstAbove(out, 0.5f) - 132300) <= 1, "events land on the tempo map");
    expect(out.frames == static_cast<size_t>(std::ceil(m.samplesBetween(0, 3072 + o.tail, 44100))) + 1, "offline render length follows the tempo map");
    ProgramBuilder pb;
    AudioEngine eng(44100);
    eng.setProgram(pb.build(s));
    eng.setMode(PlayMode::Song);
    eng.play();
    std::vector<float> L(512), R(512);
    for (int i = 0; i < 3 * 44100; i += 441) eng.process(L.data(), R.data(), 441);
    expect(std::fabs(eng.position() - 480.0) < 1e-6, "transport position follows the tempo map");
    // Changing the program (tempo edit) mid-play keeps the position continuous.
    s.automations[0].points[1].value = static_cast<float>(td->toNormalized(90));
    double before = eng.position();
    eng.setProgram(pb.build(s));
    eng.process(L.data(), R.data(), 441);
    expect(std::fabs(eng.position() - (before + 441.0 / 44100.0 * 90.0 / 60.0 * 96.0)) < 1e-6, "re-anchors when the tempo map changes");
  }

  // ---------------------------------------------------------------- project round-trip
  {
    Song s = audioSong("/tmp/none.wav");
    Clip c = audioClip(s, 96, 200);
    c.audioStart = 0.25, c.audioEnd = 1.5, c.fadeIn = 0.01, c.fadeOut = 0.2, c.gain = 0.7f, c.pitch = -300, c.stretch = 1.5,
    c.syncBpm = 128, c.reverse = true, c.muted = true, c.track = 3;
    s.playlist.push_back(c);
    AutomationClip a;
    a.id = s.allocateId(), a.name = "Cutoff", a.owner = s.masterId(), a.index = 1, a.min = 0.1f, a.max = 0.9f;
    a.points = {{0, 0.25f, 0, AutoShape::Curve}, {96, 0.75f, -0.5f, AutoShape::SCurve}};
    s.automations.push_back(a);
    Clip ca;
    ca.id = s.allocateId(), ca.kind = ClipKind::Automation, ca.source = a.id, ca.start = 384, ca.length = 96, ca.offset = 12;
    s.playlist.push_back(ca);
    Clip cp;
    cp.id = s.allocateId(), cp.pattern = s.patterns[0].id, cp.offset = 48, cp.length = 96;
    s.playlist.push_back(cp);
    s.markers = {{0, "4/4", 4, 4}, {768, "Drop", 0, 0}};
    s.tracks[2].height = 1.5f;
    Song back;
    std::string err;
    bool ok = songFromJson(songToJson(s), back, &err);
    expect(ok && back.playlist == s.playlist && back.automations == s.automations && back.markers == s.markers && back.tracks == s.tracks,
           "arrangement round-trips through the project file");
    Song plain = makeEmptySong();
    Json j = songToJson(plain);
    expect(!j.find("automations") && !j.find("markers"), "no arrangement keys when unused");
  }

  // ---------------------------------------------------------------- FLP import
  {
    Builder B;
    B.text(199, "25.2.3.5164");
    B.d(156, 150000);
    B.w(65, 1);
    B.data(224, std::vector<uint8_t>{});  // empty pattern
    // Channel 0: audio clip channel → insert 1, sample, levels, pitch 200 ct stretch mode.
    B.w(64, 0), B.b(21, 4), B.text(203, "Vox"), B.d(128, 0x00FF0000);
    B.w(104, 1);
    {
      Bytes lv(24);
      lv.u32(0, 6400), lv.u32(4, 10000);
      B.data(219, lv.v);
      Bytes pr(168);
      pr.u32(100, 200), pr.u32(104, 0), pr.u32(108, 0xFFFFFFFF);
      B.data(215, pr.v);
    }
    B.text(196, "/nonexistent/vox.wav");
    // Channel 1: automation (tempo): points 0 → 0.75, +4 beats → 0.25 (curve, tension 0.5).
    B.w(64, 1), B.b(21, 5), B.text(203, "TEMPO");
    {
      Bytes a(21 + 48 + 112);
      a.u32(0, 1), a.u32(4, 64), a.u32(9, 4), a.u32(13, 3), a.u32(17, 2);
      a.f64(21, 0.0), a.f64(29, 0.75);
      a.f64(45, 4.0), a.f64(53, 0.25), a.f32(61, 0.5f);
      a.v[65] = 0;
      B.data(234, a.v);
    }
    // Channel 2: automation → insert 1 volume.
    B.w(64, 2), B.b(21, 5), B.text(203, "Bus volume");
    {
      Bytes a(21 + 24 + 112);
      a.u32(0, 1), a.u32(17, 1), a.f64(29, 0.5);
      B.data(234, a.v);
    }
    {
      Bytes l(20);
      l.u16(2, 1), l.u32(8, 0x40000005u), l.u32(12, 8);
      B.data(227, l.v);
      Bytes m(20);
      m.u16(2, 2), m.u32(8, (0x7000u + 1u * 64u) << 16 | 0x1FC0u), m.u32(12, 8);
      B.data(227, m.v);
    }
    B.w(99, 0);
    std::vector<uint8_t> items;
    {
      Bytes au(80);
      au.v = item(96, 0, 192, 2, 0x2040);  // audio, muted
      au.f32(24, 100.0f), au.f32(28, 600.0f), au.f32(36, 25.0f), au.f32(52, 0.5f), au.f32(48, -50.0f);
      items.insert(items.end(), au.v.begin(), au.v.end());
      Bytes at(80);
      at.v = item(0, 1, 768, 5, 0x40);  // tempo automation
      at.f32(24, -1.0f), at.f32(28, -1.0f);
      items.insert(items.end(), at.v.begin(), at.v.end());
      Bytes pt(80);
      pt.v = item(384, 0x5001, 288, 0, 0x40);  // pattern 1, starts 96 ticks in
      pt.u32(24, 96), pt.u32(28, 384);
      items.insert(items.end(), pt.v.begin(), pt.v.end());
      Bytes bv(80);
      bv.v = item(0, 2, 384, 499, 0x40);  // last track (index 499)
      bv.f32(24, -1.0f), bv.f32(28, -1.0f);
      items.insert(items.end(), bv.v.begin(), bv.v.end());
    }
    B.data(233, items);
    B.d(148, 0x08000000), B.b(33, 3), B.b(34, 4), B.text(205, "3/4");
    B.d(148, 768), B.b(33, 4), B.b(34, 4), B.text(205, "Drop");
    {
      Bytes t(70);
      t.u32(0, 1), t.u32(4, 0x000000FF), t.v[12] = 1, t.f32(13, 1.0f);
      B.data(238, t.v);
      B.b(43, 1);
      B.text(239, "Drums");
      Bytes t2(70);
      t2.u32(0, 2), t2.v[12] = 0, t2.f32(13, 2.0f);
      B.data(238, t2.v);
    }
    for (int ins = 0; ins < 2; ++ins) {
      B.data(236, std::vector<uint8_t>(12, 0));
      for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
      B.d(147, 0);
    }
    std::string path = (std::filesystem::temp_directory_path() / "gl_arr_test.flp").string();
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
    expect(ok, "FLP: import succeeds");
    const Channel* vox = nullptr;
    for (const auto& c : s.channels)
      if (c.name == "Vox") vox = &c;
    expect(vox && vox->plugin.type == "gl.sampler" && vox->plugin.data == "/nonexistent/vox.wav" && vox->mixerTrack == s.mixer[1].id &&
               std::fabs(vox->gain - 10000.f / 12800.f) < 1e-6f && vox->color == 0x0000FF,
           "FLP: audio clip channel (sample, insert, level, colour)");
    expect(vox && vox->plugin.params[pp::SmpPitchShift] == 200 && vox->plugin.params[pp::SmpStretchMode] == 1, "FLP: channel pitch + stretch mode");
    const Clip* au = nullptr;
    const Clip* ta = nullptr;
    const Clip* pc = nullptr;
    const Clip* bv = nullptr;
    for (const auto& c : s.playlist) {
      if (c.kind == ClipKind::Audio) au = &c;
      if (c.kind == ClipKind::Pattern) pc = &c;
      if (c.kind == ClipKind::Automation && c.start == 0 && c.length == 768) ta = &c;
      if (c.kind == ClipKind::Automation && c.length == 384) bv = &c;
    }
    expect(au && au->source == (vox ? vox->id : 0) && au->start == 96 && au->length == 192 && au->track == 2 && au->muted &&
               near(au->audioStart, 0.1, 1e-6) && near(au->audioEnd, 0.6, 1e-6) && near(au->fadeIn, 0.025, 1e-6) && au->gain == 0.5f &&
               au->pitch == -50.0f,
           "FLP: audio clip (offsets ms, mute flag, fade, gain, pitch)");
    expect(pc && pc->offset == 96 && pc->start == 384 && pc->length == 288, "FLP: pattern clip start offset");
    expect(bv && bv->track == 499 && s.tracks.size() == 500, "FLP: content on track 500 (track = 499 − stored value)");
    const AutomationClip* tempo = ta ? s.automation(ta->source) : nullptr;
    auto td = describeParam(s, songParam(SongParam::Tempo));
    expect(tempo && tempo->owner == kSongId && tempo->index == static_cast<uint32_t>(SongParam::Tempo) && tempo->points.size() == 2 &&
               tempo->points[1].pos == 384 && tempo->points[1].tension == -0.5f && near(tempo->min, td->toNormalized(60), 1e-6),
           "FLP: tempo automation clip (points in beats, tension (FL sign flipped), 60–180 BPM range)");
    TempoMap m;
    m.build(s);
    expect(std::fabs(m.bpmAt(1) - 150.0) < 1.0 && std::fabs(m.bpmAt(600) - 90.0) < 0.01, "FLP: tempo map from the imported clip");
    const AutomationClip* bus = bv ? s.automation(bv->source) : nullptr;
    expect(bus && bus->owner == s.mixer[1].id && bus->index == static_cast<uint32_t>(MixerParam::Volume), "FLP: mixer volume target (FL 2025 base)");
    expect(s.markers.size() == 2 && s.markers[0].pos == 0 && s.markers[0].timeSigNum == 3 && s.markers[0].timeSigDen == 4 &&
               s.markers[1].pos == 768 && s.markers[1].name == "Drop" && s.markers[1].timeSigNum == 0,
           "FLP: markers + time signature marker");
    expect(s.tracks[0].name == "Drums" && s.tracks[0].color == 0xFF0000 && !s.tracks[0].muted && s.tracks[1].muted && s.tracks[1].height == 2.0f,
           "FLP: track name, colour, mute, height");
  }

  // ---------------------------------------------------------------- decoders (FL content, optional)
  {
    std::string err;
    std::vector<uint8_t> junk = {'w', 'v', 'p', 'k', 1, 0, 0, 0};
    DecodedAudio d;
    expect(!decodeWavPack(junk.data(), junk.size(), d, &err), "WavPack: truncated input rejected");
    const char* wv = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Packs/Risers/Riser Noise Metal.wv";
    if (std::filesystem::exists(wv)) {
      SamplePtr smp = loadSample(wv, &err);
      expect(smp && smp->frames() > 1000 && smp->sampleRate > 0, "WavPack: FL sample decodes (checksums verified)");
      std::FILE* f = std::fopen(wv, "rb");
      std::vector<uint8_t> bytes;
      if (f) {
        int ch;
        while ((ch = std::fgetc(f)) != EOF) bytes.push_back(static_cast<uint8_t>(ch));
        std::fclose(f);
      }
      if (bytes.size() > 2000) bytes[bytes.size() / 2] ^= 0x55;
      expect(!decodeWavPack(bytes.data(), bytes.size(), d, &err), "WavPack: corruption detected");
    } else {
      std::printf("# FL Studio content not found: WavPack sample test skipped\n");
    }
    const char* ogg = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Packs/Drums/Kicks/Grv Kick 27.wav";
    if (std::filesystem::exists(ogg)) {
      SamplePtr smp = loadSample(ogg, &err);
      expect(smp && smp->frames() > 1000, "Ogg Vorbis inside RIFF decodes");
    }
  }
  std::remove(click.c_str());
  std::remove(dc.c_str());
  return finish();
}
