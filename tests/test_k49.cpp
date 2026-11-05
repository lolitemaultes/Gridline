// Gridline: K-49, FL import of tempo clips and of audio clips stretched to a time.
// An automation clip's values sit in the range its channel stores, not a fixed one;
// clips stretched to a time on a channel that keeps its pitch span their ticks at whatever
// the tempo is, on a channel that resamples they hold their rate (test_k58.cpp); the clip end
// FL derives from the project tempo does not stop the audio under tempo automation;
// the FL export writes those values back. Every file here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/Params.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

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
  void u32(size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
  }
  void f32(size_t at, float x) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    u32(at, b);
  }
  void f64(size_t at, double x) {
    uint64_t b;
    std::memcpy(&b, &x, 8);
    u32(at, static_cast<uint32_t>(b)), u32(at + 4, static_cast<uint32_t>(b >> 32));
  }
};

constexpr int kPpq = 96;

// An 88-byte (FL 2026) audio clip record.
std::vector<uint8_t> audioItem(uint32_t pos, uint16_t channel, uint32_t len, int track, float startMs, float endMs, double stretch = 1.0, float fadeInMs = 0) {
  Bytes r(88);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, channel);
  r.u32(8, len);
  r.u16(12, static_cast<uint16_t>(499 - track));
  r.u16(18, 0x40);
  r.f32(24, startMs);
  r.f32(28, endMs);
  r.f32(36, fadeInMs);
  r.f32(52, 1.0f);
  r.f64(64, stretch);
  return r.v;
}

std::vector<uint8_t> autoItem(uint32_t pos, uint16_t channel, uint32_t len, int track) {
  Bytes r(88);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, channel);
  r.u32(8, len);
  r.u16(12, static_cast<uint16_t>(499 - track));
  r.u16(18, 0x40);
  r.f32(24, -1.0f);
  r.f32(28, -1.0f);
  r.f32(52, 1.0f);
  r.f64(64, 1.0);
  return r.v;
}

struct Point {
  double beatsAfter, value;
};

void automationChannel(Builder& B, int index, const char* name, const std::vector<Point>& pts, int lo, int hi) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 5), B.text(203, name);
  if (lo >= 0) {
    Bytes lv(24);
    lv.u32(0, static_cast<uint32_t>(lo)), lv.u32(4, static_cast<uint32_t>(hi));
    B.data(219, lv.v);
  }
  Bytes a(21 + 24 * pts.size() + 112);
  a.u32(0, 1), a.u32(4, 64), a.u32(9, 4), a.u32(13, 3), a.u32(17, static_cast<uint32_t>(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) a.f64(21 + 24 * i, pts[i].beatsAfter), a.f64(29 + 24 * i, pts[i].value);
  B.data(234, a.v);
}

void audioChannel(Builder& B, int index, const char* name, const std::string& sample, float timeUnits, int cents, int mode, int pitchKnob = 0) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 4), B.text(203, name);
  B.text(196, sample);
  Bytes lv(24);
  lv.u32(0, 6400), lv.u32(4, 10000), lv.u32(8, static_cast<uint32_t>(pitchKnob));
  B.data(219, lv.v);
  Bytes pr(168);
  pr.f32(96, timeUnits);
  pr.u32(100, static_cast<uint32_t>(cents));
  pr.u32(108, static_cast<uint32_t>(mode));
  B.data(215, pr.v);
}

void link(Builder& B, int source, uint32_t target) {
  Bytes l(20);
  l.u16(2, static_cast<uint16_t>(source)), l.u32(8, target), l.u32(12, 8);
  B.data(227, l.v);
}

void finish(Builder& B, const std::vector<std::vector<uint8_t>>& items, int inserts = 2) {
  B.w(99, 0);
  std::vector<uint8_t> all;
  for (const auto& i : items) all.insert(all.end(), i.begin(), i.end());
  B.data(233, all);
  for (int ins = 0; ins < inserts; ++ins) {
    B.data(236, std::vector<uint8_t>(12, 0));
    for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
    B.d(147, 0);
  }
}

bool load(const Builder& B, Song& s, const std::string& dir = "") {
  flp::Report rep;
  std::string err;
  return importFlpBytes(B.file(kPpq), s, rep, &err, "", dir);
}

std::string writeSample(const std::string& name, double seconds) {
  const size_t n = static_cast<size_t>(seconds * 44100);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = b.at(1, i) = 0.25f;
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, 44100, 16);
  sampleCache().clear();
  return path;
}

// Seconds from the start of the render to where the audio stops.
double audioEnd(const AudioBuffer& b, int rate) {
  for (size_t i = b.frames; i-- > 0;)
    if (std::fabs(b.at(0, i)) > 0.01f) return static_cast<double>(i + 1) / rate;
  return 0;
}

AudioBuffer renderOf(const Song& song, Tick) {
  Song s = song;
  for (auto& t : s.mixer) t.muted = false;  // the synthetic inserts carry no enable flag
  RenderOptions o;
  o.sampleRate = 44100;
  return renderSong(s, o);
}

const AutomationClip* byName(const Song& s, const char* name) {
  for (const auto& a : s.automations)
    if (a.name == name) return &a;
  return nullptr;
}

const Clip* audioClipAt(const Song& s, Tick start) {
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Audio && c.start == start) return &c;
  return nullptr;
}

}  // namespace

int main() {
  // ---------------------------------------------------- the clip's stored range
  {
    // A tempo clip whose stored range reaches past 180 (a project over 180 BPM widens it).
    Builder B;
    B.text(199, "26.1.7.5421");
    B.d(156, 182000);
    automationChannel(B, 0, "Tempo", {{0, 0.90625}, {16, 0.90625}, {4, 0.953125}, {16, 0.953125}}, 1250, 4450);
    link(B, 0, flp::kFlTempoTarget);
    finish(B, {autoItem(0, 0, 36 * kPpq, 0)});
    Song s;
    expect(load(B, s), "imports a tempo clip with its own range");
    TempoMap m;
    m.build(s);
    expect(near(m.bpmAt(kPpq), 176.0, 1e-3), "tempo clip point 0.90625 in 1250..4450 is 176 BPM (60..188)");
    expect(near(m.bpmAt(35 * kPpq), 182.0, 1e-3), "tempo clip point 0.953125 in 1250..4450 is 182 BPM");
    expect(near(s.tempo, 182.0, 1e-9), "the project tempo stays what the file stores");
  }
  {
    // The range a new FL tempo clip gets: 60..180.
    Builder B;
    B.d(156, 150000);
    automationChannel(B, 0, "Tempo", {{0, 0.75}, {8, 0.75}}, 1250, 4250);
    automationChannel(B, 1, "No levels", {{0, 0.5}, {8, 0.5}}, -1, -1);
    link(B, 0, flp::kFlTempoTarget);
    finish(B, {autoItem(0, 0, 8 * kPpq, 0)});
    Song s;
    load(B, s);
    TempoMap m;
    m.build(s);
    expect(near(m.bpmAt(kPpq), 150.0, 1e-3), "the usual range keeps 0.75 = 150 BPM");
  }
  {
    // Without stored levels (older files): the range a new clip gets.
    Builder B;
    B.d(156, 120000);
    automationChannel(B, 0, "Tempo", {{0, 0.5}, {8, 0.5}}, -1, -1);
    link(B, 0, flp::kFlTempoTarget);
    finish(B, {autoItem(0, 0, 8 * kPpq, 0)});
    Song s;
    load(B, s);
    TempoMap m;
    m.build(s);
    expect(near(m.bpmAt(kPpq), 120.0, 1e-3), "no stored levels: 0.5 = 120 BPM");
  }
  {
    // Other targets: a channel pitch clip and a mixer volume clip with narrower ranges, one reversed.
    Builder B;
    B.d(156, 120000);
    B.w(64, 2), B.b(21, 4), B.text(203, "Bed");
    automationChannel(B, 0, "Pitch", {{0, 0.0}, {8, 1.0}}, 3200, 9600);
    automationChannel(B, 1, "Volume", {{0, 0.0}, {8, 1.0}}, 0, 6400);
    automationChannel(B, 3, "Backwards", {{0, 0.0}, {8, 1.0}}, 12800, 0);
    link(B, 0, 2u << 16 | 0x04u);
    link(B, 1, (0x7000u + 1u * 64u) << 16 | 0x1FC0u);
    link(B, 3, (0x7000u + 1u * 64u) << 16 | 0x1FC1u);
    finish(B, {});
    Song s;
    load(B, s);
    const AutomationClip* p = byName(s, "Pitch");
    const AutomationClip* v = byName(s, "Volume");
    const AutomationClip* r = byName(s, "Backwards");
    expect(p && p->min < p->max, "pitch clip resolved");
    if (p) {
      const auto d = describeParam(s, ParamId{p->owner, p->index});
      expect(d && near(d->fromNormalized(p->min), -4.0, 1e-3) && near(d->fromNormalized(p->max), 4.0, 1e-3), "channel pitch clip range 3200..9600 is -4..+4 semitones");
    }
    expect(v && near(v->min, 0.0, 1e-6) && near(v->max, 0.4, 1e-6), "mixer volume clip with half the range reaches half as far");
    expect(r && r->min > r->max, "a reversed range stays reversed");
  }
  // ---------------------------------------------------- clips stretched to a time
  const std::string fourSec = writeSample("gl_k49_a.wav", 4.0);
  {
    // 4 s stretched to 8 beats (1536 / 192); the tempo clip plays 120 while the project says 150.
    Builder B;
    B.d(156, 150000);
    audioChannel(B, 0, "Kept pitch", fourSec, 1536.0f, 0, -1);
    audioChannel(B, 1, "Resampled", fourSec, 1536.0f, 0, 0);
    automationChannel(B, 2, "Tempo", {{0, 0.5}, {64, 0.5}}, 1250, 4250);
    link(B, 2, flp::kFlTempoTarget);
    finish(B, {audioItem(0, 0, 8 * kPpq, 0, 0, 4000), audioItem(16 * kPpq, 1, 8 * kPpq, 1, 0, 4000), autoItem(0, 2, 64 * kPpq, 2)});
    Song s;
    expect(load(B, s), "imports clips stretched to a time");
    const Clip* a = audioClipAt(s, 0);
    const Clip* b = audioClipAt(s, 16 * kPpq);
    expect(a && a->syncBpm > 0 && a->tempoLock, "a clip stretched to a time on a channel that keeps its pitch follows the tempo");
    expect(b && b->syncBpm == 0 && !b->tempoLock, "on a channel that resamples it does not");
    AudioBuffer ka = [&] {
      Song one = s;
      one.playlist.erase(std::remove_if(one.playlist.begin(), one.playlist.end(), [](const Clip& c) { return c.kind == ClipKind::Audio && c.start != 0; }), one.playlist.end());
      return renderOf(one, 12 * kPpq);
    }();
    // 8 beats at 120 BPM = 4 s, the whole sample, not the 3.2 s it spans at the project's 150.
    expect(near(audioEnd(ka, 44100), 4.0, 0.03), "kept pitch: audio ends at the clip's end at the tempo that plays");
    AudioBuffer kb = [&] {
      Song one = s;
      one.playlist.erase(std::remove_if(one.playlist.begin(), one.playlist.end(), [](const Clip& c) { return c.kind == ClipKind::Audio && c.start == 0; }), one.playlist.end());
      return renderOf(one, 28 * kPpq);
    }();
    // K-60: the rate is fitted at the tempo where the clip starts: the 4 s window spans the clip's 4 s.
    expect(near(audioEnd(kb, 44100), 8.0 + 4.0, 0.03), "resampled: the audio spans the clip at the tempo where it starts");
  }
  {
    // Half the window in half the ticks, a clip stretch, and a window that says something else.
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, "Fit", fourSec, 1536.0f, 0, -1);
    audioChannel(B, 1, "Other file", fourSec, 1536.0f * 4.0f, 0, -1);
    automationChannel(B, 2, "Tempo", {{0, 0.5}, {64, 0.5}}, 1250, 4250);
    link(B, 2, flp::kFlTempoTarget);
    finish(B, {audioItem(0, 0, 4 * kPpq, 0, 1000, 3000), audioItem(8 * kPpq, 0, 8 * kPpq, 0, 0, 2000, 2.0), audioItem(32 * kPpq, 1, 4 * kPpq, 0, 0, 2000), autoItem(0, 2, 64 * kPpq, 1)});
    Song s;
    load(B, s);
    const Clip* half = audioClipAt(s, 0);
    const Clip* st = audioClipAt(s, 8 * kPpq);
    const Clip* other = audioClipAt(s, 32 * kPpq);
    auto perTick = [&](const Clip* c) {
      const Channel* ch = c ? s.channel(c->source) : nullptr;
      if (!ch) return 0.0;
      return 60.0 / (c->syncBpm * s.ppq) / (c->stretch * ch->plugin.params[pp::SmpTimeStretch]);
    };
    expect(near(perTick(half), 2.0 / (4 * kPpq), 1e-6), "a clip's window plays through its ticks");
    expect(near(perTick(st), 2.0 / (8 * kPpq), 1e-6), "the clip's stretch multiplies the time");
    expect(near(perTick(other), 2.0 / (4 * kPpq), 1e-6), "a window that disagrees with the channel's time is what plays");
    expect(st && near(st->stretch, 2.0, 1e-9), "the stretch is kept");
  }
  {
    // A constant tempo: the channel's stretch does it; a window that disagrees still plays as stored.
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, "Fit", fourSec, 1536.0f, 0, -1);
    audioChannel(B, 1, "Other file", fourSec, 1536.0f * 4.0f, 0, -1);
    finish(B, {audioItem(0, 0, 8 * kPpq, 0, 0, 4000), audioItem(32 * kPpq, 1, 4 * kPpq, 0, 0, 2000)});
    Song s;
    load(B, s);
    const Clip* fit = audioClipAt(s, 0);
    const Clip* other = audioClipAt(s, 32 * kPpq);
    expect(fit && fit->syncBpm == 0 && !fit->tempoLock, "constant tempo: the clip stays as it was");
    expect(other && other->syncBpm > 0, "constant tempo: a window that disagrees is what plays");
    Song only = s;
    only.playlist.erase(std::remove_if(only.playlist.begin(), only.playlist.end(), [](const Clip& c) { return c.start != 0; }), only.playlist.end());
    expect(near(audioEnd(renderOf(only, 12 * kPpq), 44100), 4.0, 0.03), "constant tempo: the fit plays the same length");
  }
  // ---------------------------------------------------- clips without a time
  {
    const std::string longer = writeSample("gl_k49_b.wav", 10.0);
    Builder B;
    B.d(156, 150000);
    audioChannel(B, 0, "Plain", longer, 0.0f, 0, 0);
    automationChannel(B, 1, "Tempo", {{0, 0.5}, {64, 0.5}}, 1250, 4250);
    link(B, 1, flp::kFlTempoTarget);
    // 8 beats at the project's 150 BPM are 3.2 s: the end FL derives. The second clip was cut by hand.
    finish(B, {audioItem(0, 0, 8 * kPpq, 0, 0, 3200), audioItem(32 * kPpq, 0, 8 * kPpq, 0, 0, 1000), autoItem(0, 1, 64 * kPpq, 1)});
    Song s;
    load(B, s);
    const Clip* derived = audioClipAt(s, 0);
    const Clip* cut = audioClipAt(s, 32 * kPpq);
    expect(derived && derived->audioEnd == 0, "under tempo automation a derived end does not stop the clip");
    expect(cut && near(cut->audioEnd, 1.0, 1e-6), "an end that does not match the length stays");
    Song one = s;
    one.playlist.erase(std::remove_if(one.playlist.begin(), one.playlist.end(), [](const Clip& c) { return c.kind == ClipKind::Audio && c.start != 0; }), one.playlist.end());
    expect(near(audioEnd(renderOf(one, 12 * kPpq), 44100), 4.0, 0.03), "the audio reaches the clip's end at the tempo that plays");
    // No tempo automation: stored ends stand.
    Builder C;
    C.d(156, 150000);
    audioChannel(C, 0, "Plain", longer, 0.0f, 0, 0);
    finish(C, {audioItem(0, 0, 8 * kPpq, 0, 0, 3200)});
    Song t;
    load(C, t);
    const Clip* kept = audioClipAt(t, 0);
    expect(kept && near(kept->audioEnd, 3.2, 1e-6), "a constant tempo keeps the stored end");
  }
  {
    // A crossfade FL stored at the project tempo is the automatic one, also where the clip plays slower.
    const std::string longer = writeSample("gl_k49_c.wav", 10.0);
    Builder B;
    B.d(156, 150000);
    B.b(44, 1);
    audioChannel(B, 0, "Plain", longer, 0.0f, 0, 0);
    automationChannel(B, 1, "Tempo", {{0, 0.5}, {64, 0.5}}, 1250, 4250);
    link(B, 1, flp::kFlTempoTarget);
    const float overlapMs = static_cast<float>(1.0 * 60000.0 / 150.0);  // one beat at the project's tempo
    auto first = audioItem(0, 0, 8 * kPpq, 0, 0, 3200);
    {
      Bytes f(0);
      f.v = first;
      f.f32(44, overlapMs);
      first = f.v;
    }
    finish(B, {first, audioItem(7 * kPpq, 0, 8 * kPpq, 0, 0, 3200, 1.0, overlapMs), autoItem(0, 1, 64 * kPpq, 1)});
    Song s;
    load(B, s);
    const Clip* a = audioClipAt(s, 0);
    const Clip* b = audioClipAt(s, 7 * kPpq);
    expect(a && b && a->fadeOut == 0 && b->fadeIn == 0, "a crossfade stored at the project tempo follows the overlap");
  }
  // ---------------------------------------------------- AIFF samples
  {
    // 100 frames, stereo 16-bit big-endian PCM, at 44100 Hz.
    std::vector<uint8_t> f;
    auto be32 = [&](uint32_t v) { for (int k = 3; k >= 0; --k) f.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    auto be16 = [&](uint16_t v) { f.push_back(static_cast<uint8_t>(v >> 8)), f.push_back(static_cast<uint8_t>(v & 0xFF)); };
    auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    tag("FORM");
    be32(4 + 8 + 18 + 8 + 8 + 400);
    tag("AIFF");
    tag("COMM");
    be32(18);
    be16(2), be32(100), be16(16);
    for (uint8_t x : {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0}) f.push_back(x);
    tag("SSND");
    be32(8 + 400);
    be32(0), be32(0);
    for (int i = 0; i < 200; ++i) be16(0x2000);
    const std::string path = (fs::temp_directory_path() / "gl_k49_d.aiff").string();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    std::string err;
    const SamplePtr smp = loadSample(path, &err);
    expect(smp && smp->frames() == 100 && near(smp->left[50], 0.25, 1e-3), "an AIFF sample loads");
    fs::remove(path);
  }
  // ---------------------------------------------------- FL export
  {
    Builder B;
    B.text(199, "26.1.7.5421");
    B.d(156, 182000);
    audioChannel(B, 0, "Fit", fourSec, 1536.0f, 0, -1);
    automationChannel(B, 1, "Tempo", {{0, 0.90625}, {16, 0.90625}, {4, 0.953125}, {16, 0.953125}}, 1250, 4450);
    link(B, 1, flp::kFlTempoTarget);
    finish(B, {audioItem(0, 0, 8 * kPpq, 0, 0, 4000, 1.25), audioItem(32 * kPpq, 0, 8 * kPpq, 0, 0, 4000), autoItem(0, 1, 36 * kPpq, 1)});
    Song s;
    load(B, s);
    auto reimport = [&](const Song& song, Song& out) {
      flp::Bytes bytes;
      flp::ExportReport rep;
      flp::ExportOptions o;
      if (!flp::exportFlp(song, bytes, rep, o)) return false;
      flp::Report r;
      std::string err;
      return importFlpBytes(bytes, out, r, &err);
    };
    Song same;
    expect(reimport(s, same), "exports and imports again");
    {
      TempoMap m;
      m.build(same);
      expect(near(m.bpmAt(kPpq), 176.0, 1e-3) && near(m.bpmAt(35 * kPpq), 182.0, 1e-3), "the tempo clip round-trips");
    }
    // An edited point in the clip's range: the range stays, the values are right.
    Song edited = s;
    for (auto& a : edited.automations)
      if (a.name == "Tempo") a.points[0].value = 0.5f;
    {
      Song back;
      expect(reimport(edited, back), "an edited tempo clip exports");
      TempoMap m, want;
      m.build(back), want.build(edited);
      expect(near(m.bpmAt(1), want.bpmAt(1), 0.05) && near(m.bpmAt(35 * kPpq), 182.0, 1e-2), "an edited tempo clip keeps its tempo in the export");
    }
    // A clip in another song, far past 180: its values fit the whole scale.
    Song fresh = makeEmptySong();
    fresh.tempo = 120;
    {
      AutomationClip a;
      a.id = fresh.allocateId(), a.name = "Tempo", a.owner = kSongId, a.index = static_cast<uint32_t>(SongParam::Tempo);
      const auto d = describeParam(fresh, songParam(SongParam::Tempo));
      a.points = {{0, static_cast<float>(d->toNormalized(240.0)), 0, AutoShape::Curve}, {384, static_cast<float>(d->toNormalized(240.0)), 0, AutoShape::Curve}};
      fresh.automations.push_back(a);
      Clip c;
      c.id = fresh.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 0, c.length = 384;
      fresh.playlist.push_back(c);
      Song back;
      expect(reimport(fresh, back), "a Gridline tempo clip exports");
      TempoMap m;
      m.build(back);
      expect(near(m.bpmAt(96), 240.0, 0.05), "a Gridline tempo clip at 240 BPM is 240 BPM in the export");
    }
    // A clip edit elsewhere rewrites the records; the fit clip keeps its own stretch.
    Song moved = s;
    for (auto& c : moved.playlist)
      if (c.kind == ClipKind::Audio && c.start == 0) c.start = 4 * kPpq;
    {
      Song back;
      expect(reimport(moved, back), "a moved clip exports");
      const Clip* c = audioClipAt(back, 4 * kPpq);
      expect(c && near(c->stretch, 1.25, 1e-9), "the export writes the clip's stretch, not the tempo fit");
      expect(c && c->syncBpm > 0 && c->tempoLock, "and it follows the tempo again on import");
    }
  }
  fs::remove(fourSec);
  return finish();
}
