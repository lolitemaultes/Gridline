// Gridline: K-58 and K-60, FL audio clips stretched to a time under tempo automation. A channel whose
// time mode resamples fits the clip at the tempo where it starts and holds that rate and pitch while
// it plays. A channel that keeps its pitch follows the tempo. Every file here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/ClipRate.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kPpq = 96;
constexpr int kRate = 44100;
constexpr int kResample = 0;
constexpr int kKeepPitch = -1;
// The tempo clip: 176 BPM for 16 beats, a ramp of 4, 182 BPM for 16.
constexpr uint32_t kLowAt = 0;
constexpr uint32_t kHighAt = 20 * kPpq;
constexpr uint32_t kSection = 16 * kPpq;
// What a rate fitted at the project's tempo becomes at the tempo where a clip starts.
constexpr double kAt176 = 176.0 / 182.0;

void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.3f", v);
  return b;
}

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
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, kPpq, 0, 'F', 'L', 'd', 't'};
    const uint32_t n = static_cast<uint32_t>(ev.size());
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

std::vector<uint8_t> record(uint32_t pos, uint16_t channel, uint32_t len, int track, float startMs, float endMs, double stretch) {
  Bytes r(88);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, channel);
  r.u32(8, len);
  r.u16(12, static_cast<uint16_t>(499 - track));
  r.u16(18, 0x40);
  r.f32(24, startMs);
  r.f32(28, endMs);
  r.f32(52, 1.0f);
  r.f64(64, stretch);
  return r.v;
}

// mode: 0 resample, -1 keeps the pitch. timeBeats: the channel's time, 0 = none.
void audioChannel(Builder& B, int index, const std::string& sample, double timeBeats, int shiftCents, int mode) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 4), B.text(203, "Channel");
  B.text(196, sample);
  Bytes lv(24);
  lv.u32(0, 6400), lv.u32(4, 10000);
  B.data(219, lv.v);
  Bytes pr(168);
  pr.f32(96, static_cast<float>(timeBeats * 192.0));
  pr.u32(100, static_cast<uint32_t>(shiftCents));
  pr.u32(108, static_cast<uint32_t>(mode));
  B.data(215, pr.v);
}

// One tempo clip on channel `index`, placed on track 7: 176, then 182.
void tempoClip(Builder& B, int index) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 5), B.text(203, "Tempo");
  Bytes lv(24);
  lv.u32(0, 1250), lv.u32(4, 4450);
  B.data(219, lv.v);
  const std::vector<std::pair<double, double>> pts = {{0, 0.90625}, {16, 0.90625}, {4, 0.953125}, {16, 0.953125}};
  Bytes a(21 + 24 * pts.size() + 112);
  a.u32(0, 1), a.u32(4, 64), a.u32(9, 4), a.u32(13, 3), a.u32(17, static_cast<uint32_t>(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) a.f64(21 + 24 * i, pts[i].first), a.f64(29 + 24 * i, pts[i].second);
  B.data(234, a.v);
  Bytes l(20);
  l.u16(2, static_cast<uint16_t>(index)), l.u32(8, flp::kFlTempoTarget), l.u32(12, 8);
  B.data(227, l.v);
}

std::vector<uint8_t> tempoItem(uint16_t channel) {
  return record(0, channel, 36 * kPpq, 7, -1.0f, -1.0f, 1.0);
}

void finish(Builder& B, const std::vector<std::vector<uint8_t>>& items) {
  B.w(99, 0);
  std::vector<uint8_t> all;
  for (const auto& i : items) all.insert(all.end(), i.begin(), i.end());
  B.data(233, all);
  for (int ins = 0; ins < 2; ++ins) {
    B.data(236, std::vector<uint8_t>(12, 0));
    for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
    B.d(147, 0);
  }
}

bool load(const Builder& B, Song& s) {
  flp::Report rep;
  std::string err;
  const bool ok = importFlpBytes(B.file(), s, rep, &err);
  for (auto& t : s.mixer) t.muted = false;  // the synthetic inserts carry no enable flag
  return ok;
}

std::string writeSine(const std::string& name, double seconds, double hz) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = b.at(1, i) = 0.5f * static_cast<float>(std::sin(2 * M_PI * hz * static_cast<double>(i) / kRate));
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, kRate, 24);
  sampleCache().clear();
  return path;
}

// The frequency between two times, from its rising zero crossings.
double frequency(const AudioBuffer& a, double from, double to) {
  const size_t i0 = static_cast<size_t>(from * kRate), i1 = std::min(a.frames, static_cast<size_t>(to * kRate));
  double first = -1, last = -1;
  int count = 0;
  for (size_t i = i0 + 1; i < i1; ++i) {
    const float p = a.at(0, i - 1), q = a.at(0, i);
    if (p <= 0 && q > 0) {
      const double at = static_cast<double>(i - 1) + static_cast<double>(p / (p - q));
      if (first < 0) first = at;
      last = at, ++count;
    }
  }
  return count > 1 ? (count - 1) * kRate / (last - first) : 0;
}

double cents(double hz, double want) { return hz > 0 && want > 0 ? 1200 * std::log2(hz / want) : 1e9; }

double audioEnd(const AudioBuffer& b) {
  for (size_t i = b.frames; i-- > 0;)
    if (std::fabs(b.at(0, i)) > 0.01f) return static_cast<double>(i + 1) / kRate;
  return 0;
}

AudioBuffer renderOffline(const Song& s) {
  RenderOptions o;
  o.sampleRate = kRate;
  return renderSong(s, o);
}

// The path that plays until a clip's render is ready: no render exists here.
AudioBuffer renderRealtime(const Song& s, double seconds) {
  ProgramBuilder builder;
  builder.setSampleRate(kRate);
  builder.setDeferRenders(true);
  auto prog = builder.build(s);
  AudioEngine eng(kRate);
  eng.setProgram(prog);
  eng.setLooping(false);
  eng.setOffline(true);
  eng.setFixedBlock(AudioEngine::kOfflineBlock);
  eng.setMode(PlayMode::Song);
  eng.play();
  const size_t n = static_cast<size_t>(seconds * kRate);
  AudioBuffer out(2, n);
  std::vector<float> L(n), R(n);
  for (size_t at = 0; at < n; at += 512) eng.process(L.data() + at, R.data() + at, static_cast<int>(std::min<size_t>(512, n - at)));
  std::copy(L.begin(), L.end(), out.plane(0));
  std::copy(R.begin(), R.end(), out.plane(1));
  return out;
}

const Clip* audioClipAt(const Song& s, Tick start) {
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Audio && c.start == start) return &c;
  return nullptr;
}

// Keeps only the audio clip at `start`, and where that clip runs.
struct Solo {
  Song song;
  double from = 0, to = 0;
};
Solo solo(const Song& s, Tick start) {
  Solo o;
  o.song = s;
  auto& pl = o.song.playlist;
  pl.erase(std::remove_if(pl.begin(), pl.end(), [&](const Clip& c) { return c.kind == ClipKind::Audio && c.start != start; }), pl.end());
  TempoMap m;
  m.build(o.song);
  if (const Clip* c = audioClipAt(o.song, start)) {
    o.from = m.secondsAt(static_cast<double>(start));
    o.to = m.secondsAt(static_cast<double>(start + c->length));
  }
  return o;
}

// A song with one channel, the tempo clip, and the same clip in both sections.
Song twoSections(double projectBpm, const std::string& sample, double timeBeats, int shift, int mode, uint32_t len, float startMs, float endMs) {
  Builder B;
  B.d(156, static_cast<uint32_t>(projectBpm * 1000));
  audioChannel(B, 0, sample, timeBeats, shift, mode);
  tempoClip(B, 1);
  finish(B, {record(kLowAt, 0, len, 0, startMs, endMs, 1.0), record(kHighAt, 0, len, 1, startMs, endMs, 1.0), tempoItem(1)});
  Song s;
  expect(load(B, s), "imports the two tempo sections");
  return s;
}

struct Heard {
  double low = 0, high = 0;
};

// The frequency heard in the middle of each section's clip, each rendered by the clip alone.
template <typename Render>
Heard heardIn(const Song& s, Render render) {
  Heard h;
  const Solo a = solo(s, kLowAt), b = solo(s, kHighAt);
  const AudioBuffer ra = render(a.song), rb = render(b.song);
  h.low = frequency(ra, a.from + 0.2, a.from + 0.2 + 0.6 * (a.to - a.from));
  h.high = frequency(rb, b.from + 0.2, b.from + 0.2 + 0.6 * (b.to - b.from));
  return h;
}

bool reimport(const Song& song, Song& out) {
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions o;
  if (!flp::exportFlp(song, bytes, rep, o)) return false;
  flp::Report r;
  std::string err;
  const bool ok = importFlpBytes(bytes, out, r, &err);
  for (auto& t : out.mixer) t.muted = false;
  return ok;
}

double speedOf(const Song& s, const Clip* c) {
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  return ch ? clipRate(*ch, *c).speed : 0;
}

}  // namespace

int main() {
  const auto offline = [](const Song& x) { return renderOffline(x); };
  // ---------------------------------------------------- a resampled clip in two tempo sections
  {
    // 8 s of 440 Hz fitted to 16 beats: those last 5.2747 s at 182 BPM, a speed of 1.5167, and
    // 5.4545 s at 176 BPM, a speed of 1.4667.
    const std::string file = writeSine("gl_k58_a.wav", 8.0, 440.0);
    const double speedHigh = 8.0 / (16 * 60.0 / 182.0), speedLow = 8.0 / (16 * 60.0 / 176.0);
    const Song s = twoSections(182, file, 16, 0, kResample, kSection, 0, 8000);
    const Clip* a = audioClipAt(s, kLowAt);
    const Clip* b = audioClipAt(s, kHighAt);
    expect(a && b && a->syncBpm == 0 && !a->tempoLock && b->syncBpm == 0 && !b->tempoLock, "a resampling channel's clips are not locked to the tempo");
    expect(b && b->rateFit == 1.0, "at the project tempo the channel's own time says the rate");
    expect(std::fabs(speedOf(s, b) / speedHigh - 1) < 1e-5, "the speed at 182 BPM is the one fitted at 182");
    expect(std::fabs(speedOf(s, a) / speedLow - 1) < 1e-5, "the speed at 176 BPM is the one fitted at 176");
    const Heard h = heardIn(s, offline);
    printf("  resampled, project 182: %.3f Hz at 176 BPM, %.3f Hz at 182 BPM, %.2f cents apart\n", h.low, h.high, cents(h.low, h.high));
    expect(std::fabs(cents(h.low, 440.0 * speedLow)) <= 5, "the pitch of the rate at 176 (" + num(h.low) + " Hz)");
    expect(std::fabs(cents(h.high, 440.0 * speedHigh)) <= 5, "the pitch of the rate at 182 (" + num(h.high) + " Hz)");
    const Heard rt = heardIn(s, [](const Song& x) { return renderRealtime(x, 30.0); });
    expect(std::fabs(cents(rt.low, 440.0 * speedLow)) <= 5 && std::fabs(cents(rt.high, 440.0 * speedHigh)) <= 5, "and so does the realtime path (" + num(rt.low) + ", " + num(rt.high) + " Hz)");
    // Audio end: the file spans its clip in both sections, up to the right edge.
    const Solo la = solo(s, kLowAt), lb = solo(s, kHighAt);
    const AudioBuffer ra = renderOffline(la.song), rb = renderOffline(lb.song);
    printf("  project 182: at 176 BPM the clip lasts %.3f s, audio stops after %.3f s; at 182 BPM %.3f s, %.3f s\n", la.to - la.from, audioEnd(ra) - la.from, lb.to - lb.from, audioEnd(rb) - lb.from);
    expect(std::fabs(audioEnd(ra) - la.to) < 0.03, "at the lower tempo the audio reaches the clip's edge");
    expect(std::fabs(audioEnd(rb) - lb.to) < 0.03, "at the project tempo the file spans the clip");
    fs::remove(file);
  }
  {
    // The project says 176: the same two rates, whichever tempo it was saved at.
    const std::string file = writeSine("gl_k58_b.wav", 8.0, 440.0);
    const double speedHigh = 8.0 / (16 * 60.0 / 182.0), speedLow = 8.0 / (16 * 60.0 / 176.0);
    const Song s = twoSections(176, file, 16, 0, kResample, kSection, 0, 8000);
    const Heard h = heardIn(s, offline);
    expect(std::fabs(cents(h.low, 440.0 * speedLow)) <= 5 && std::fabs(cents(h.high, 440.0 * speedHigh)) <= 5, "a project at 176: each section has the rate of its tempo (" + num(h.low) + ", " + num(h.high) + " Hz)");
    const Solo la = solo(s, kLowAt), lb = solo(s, kHighAt);
    const AudioBuffer ra = renderOffline(la.song), rb = renderOffline(lb.song);
    printf("  project 176: at 176 BPM the clip lasts %.3f s, audio stops after %.3f s; at 182 BPM %.3f s, %.3f s\n", la.to - la.from, audioEnd(ra) - la.from, lb.to - lb.from, audioEnd(rb) - lb.from);
    expect(std::fabs(audioEnd(ra) - la.to) < 0.03, "at the project tempo the file spans the clip");
    expect(std::fabs(audioEnd(rb) - lb.to) < 0.03, "and at the higher tempo too");
    fs::remove(file);
  }
  // ---------------------------------------------------- a clip that crosses the tempo change
  {
    // Starts at 176 BPM, 4 beats before the ramp, and ends at 182: the pitch does not move while it plays.
    const std::string file = writeSine("gl_k58_cross.wav", 8.0, 440.0);
    const double speedLow = 8.0 / (16 * 60.0 / 176.0);
    const uint32_t at = 12 * kPpq, len = 20 * kPpq;
    Builder B;
    B.d(156, 182000);
    audioChannel(B, 0, file, 16, 0, kResample);
    tempoClip(B, 1);
    finish(B, {record(at, 0, len, 0, -1.0f, -1.0f, 1.0), tempoItem(1)});
    Song s;
    expect(load(B, s), "imports a clip across the tempo change");
    expect(std::fabs(speedOf(s, audioClipAt(s, at)) / speedLow - 1) < 1e-5, "its rate is the one at the tempo where it starts");
    const Solo o = solo(s, at);
    const AudioBuffer r = renderOffline(o.song);
    const double before = frequency(r, o.from + 0.1, o.from + 1.2), after = frequency(r, o.from + 3.5, o.from + 5.2);
    printf("  across the tempo change: %.3f Hz at 176 BPM, %.3f Hz at 182 BPM\n", before, after);
    expect(std::fabs(cents(before, 440.0 * speedLow)) <= 5 && std::fabs(cents(after, 440.0 * speedLow)) <= 5, "it holds its pitch on both sides (" + num(before) + ", " + num(after) + " Hz)");
    fs::remove(file);
  }
  // ---------------------------------------------------- the stored end
  {
    // A clip that plays the first half of the file. The end FL stored is that clip's length at
    // the project tempo. That end means no shorter a read at the lower tempo: the clip's audio lasts.
    const std::string file = writeSine("gl_k58_c.wav", 16.0, 440.0);
    const Song s = twoSections(182, file, 32, 0, kResample, kSection, 0, 8000);
    const Clip* a = audioClipAt(s, kLowAt);
    expect(a && a->audioEnd == 0, "the end derived from the project tempo is not kept");
    const Solo la = solo(s, kLowAt), lb = solo(s, kHighAt);
    const AudioBuffer ra = renderOffline(la.song), rb = renderOffline(lb.song);
    printf("  a window inside the file, project 182: audio stops after %.3f s of %.3f s at 176 BPM, %.3f s of %.3f s at 182 BPM\n", audioEnd(ra) - la.from, la.to - la.from, audioEnd(rb) - lb.from, lb.to - lb.from);
    expect(std::fabs(audioEnd(ra) - la.to) < 0.03, "the lower tempo's clip is not cut short by a stale end");
    expect(std::fabs(audioEnd(rb) - lb.to) < 0.03, "the project tempo's clip ends where it did");
    fs::remove(file);
  }
  {
    // A window 3% short of the rate at the project tempo is not the derived end: it stays, and stops the audio there.
    const std::string file = writeSine("gl_k58_d.wav", 16.0, 440.0);
    const Song s = twoSections(182, file, 32, 0, kResample, kSection, 0, 7760);
    const Clip* a = audioClipAt(s, kLowAt);
    expect(a && std::fabs(a->audioEnd - 7.76) < 1e-5, "an end that is not derived stays");
    fs::remove(file);
  }
  // ---------------------------------------------------- a channel that keeps its pitch
  {
    const std::string file = writeSine("gl_k58_e.wav", 8.0, 440.0);
    const Song s = twoSections(182, file, 16, 0, kKeepPitch, kSection, 0, 8000);
    const Clip* a = audioClipAt(s, kLowAt);
    const Clip* b = audioClipAt(s, kHighAt);
    expect(a && b && a->syncBpm > 0 && a->tempoLock && b->syncBpm > 0 && b->tempoLock, "a channel that keeps its pitch stays locked to the tempo");
    expect(a && a->rateFit == 1.0, "and needs no rate fit");
    const Heard h = heardIn(s, offline);
    expect(std::fabs(cents(h.low, 440.0)) <= 10 && std::fabs(cents(h.high, 440.0)) <= 10, "its pitch stays (" + num(h.low) + ", " + num(h.high) + " Hz)");
    const Solo la = solo(s, kLowAt), lb = solo(s, kHighAt);
    const AudioBuffer ra = renderOffline(la.song), rb = renderOffline(lb.song);
    printf("  kept pitch, project 182: audio stops after %.3f s of %.3f s at 176 BPM, %.3f s of %.3f s at 182 BPM\n", audioEnd(ra) - la.from, la.to - la.from, audioEnd(rb) - lb.from, lb.to - lb.from);
    expect(std::fabs(audioEnd(ra) - la.to) < 0.1 && std::fabs(audioEnd(rb) - lb.to) < 0.1, "and its speed follows the tempo: the file spans the clip in both sections");
    fs::remove(file);
  }
  // ---------------------------------------------------- a resampled channel with a pitch
  {
    const std::string file = writeSine("gl_k58_f.wav", 8.0, 440.0);
    const double speed = 8.0 / (16 * 60.0 / 182.0) * std::pow(2.0, 0.25);
    const Song s = twoSections(182, file, 16, 300, kResample, kSection, -1.0f, -1.0f);
    const Clip* a = audioClipAt(s, kLowAt);
    const Clip* b = audioClipAt(s, kHighAt);
    expect(a && a->syncBpm == 0 && std::fabs(speedOf(s, b) / speed - 1) < 1e-5, "a resampled pitch also sets the fixed speed");
    const Heard h = heardIn(s, offline);
    expect(std::fabs(cents(h.high, 440.0 * speed)) <= 5 && std::fabs(cents(h.low, 440.0 * speed * kAt176)) <= 5, "and keeps its offset in both sections (" + num(h.low) + ", " + num(h.high) + " Hz)");
    fs::remove(file);
  }
  // ---------------------------------------------------- a window that disagrees with the time
  {
    // The time says 8 s in 64 beats, a speed of 0.379. Each clip's window says 4 s in 16 beats at
    // the project's 182: a speed of 0.7583 there, and that fit at 176 in the lower section.
    const std::string file = writeSine("gl_k58_g.wav", 8.0, 440.0);
    const double speed = 4.0 / (16 * 60.0 / 182.0);
    const Song s = twoSections(182, file, 64, 0, kResample, kSection, 0, 4000);
    const Clip* a = audioClipAt(s, kLowAt);
    const Clip* h1 = audioClipAt(s, kHighAt);
    expect(a && a->syncBpm == 0 && !a->tempoLock, "a window that disagrees does not lock the clip to the tempo");
    expect(h1 && std::fabs(h1->rateFit - 0.5) < 1e-5 && h1->stretch == 1.0, "its rate is carried apart from the clip's stretch");
    expect(std::fabs(speedOf(s, h1) / speed - 1) < 1e-5, "the speed is the window's at the project tempo");
    expect(std::fabs(speedOf(s, a) / (speed * kAt176) - 1) < 1e-5, "and that window fitted at the lower tempo");
    const Heard h = heardIn(s, offline);
    printf("  a window that disagrees: %.3f Hz at 176 BPM, %.3f Hz at 182 BPM (%.2f cents apart)\n", h.low, h.high, cents(h.low, h.high));
    expect(std::fabs(cents(h.high, 440.0 * speed)) <= 5 && std::fabs(cents(h.low, 440.0 * speed * kAt176)) <= 5, "its pitch is the window's in each section (" + num(h.low) + ", " + num(h.high) + " Hz)");

    // FL export: unchanged, and with the clip moved so its record is written again.
    Song same;
    expect(reimport(s, same), "exports and imports again");
    const Clip* c = audioClipAt(same, kLowAt);
    expect(c && c->stretch == 1.0 && !c->tempoLock && std::fabs(speedOf(same, c) / (speed * kAt176) - 1) < 1e-5, "the rate round-trips");
    const Clip* c2 = audioClipAt(same, kHighAt);
    expect(c2 && std::fabs(speedOf(same, c2) / speed - 1) < 1e-5, "in both sections");
    expect(c && c->audioEnd == 0, "and the end it writes is not read back as a stale end");
    Song moved = s;
    for (auto& x : moved.playlist)
      if (x.kind == ClipKind::Audio && x.start == kHighAt) x.start = kHighAt + kPpq;
    Song back;
    expect(reimport(moved, back), "a moved clip exports");
    const Clip* m = audioClipAt(back, kHighAt + kPpq);
    expect(m && m->stretch == 1.0, "the export writes FL's stretch, not the rate");
    expect(m && !m->tempoLock && std::fabs(speedOf(back, m) / speed - 1) < 1e-5, "and the window it writes gives the same rate again");
    // Saved and loaded with the project.
    Song loaded;
    expect(songFromJson(songToJson(s), loaded) && audioClipAt(loaded, kLowAt) && *audioClipAt(loaded, kLowAt) == *a, "the rate is saved with the project");
    // Back to the original length forgets it, with the rest of the time.
    Song reset = s;
    expect(a && resetClipStretch(reset, a->id) && reset.clip(a->id)->rateFit == 1.0, "resetting the clip's stretch clears it");
    fs::remove(file);
  }
  // ---------------------------------------------------- times beyond x0.25..x4
  {
    // 1 s of 440 Hz in 20 beats: 6.5934 s at 182, a speed of 0.1517, beyond the sampler's x4.
    const std::string file = writeSine("gl_k58_h.wav", 1.0, 440.0);
    const double speed = 1.0 / (20 * 60.0 / 182.0);
    const Song s = twoSections(182, file, 20, 0, kResample, kSection, -1.0f, -1.0f);
    const Clip* a = audioClipAt(s, kLowAt);
    const Clip* b = audioClipAt(s, kHighAt);
    expect(a && a->syncBpm == 0 && !a->tempoLock && a->rateFit > 1 && a->stretch == 1.0, "a time beyond the range does not lock a resampled clip under tempo automation");
    expect(std::fabs(speedOf(s, b) / speed - 1) < 1e-5, "and its rate is the time's at the project tempo");
    const Heard h = heardIn(s, offline);
    printf("  beyond the range: %.3f Hz at 176 BPM, %.3f Hz at 182 BPM (%.2f cents apart)\n", h.low, h.high, cents(h.low, h.high));
    expect(std::fabs(cents(h.high, 440.0 * speed)) <= 5 && std::fabs(cents(h.low, 440.0 * speed * kAt176)) <= 5, "each section has the rate of its tempo (" + num(h.low) + ", " + num(h.high) + " Hz)");
    Song back;
    Song moved = s;
    for (auto& x : moved.playlist)
      if (x.kind == ClipKind::Audio && x.start == kHighAt) x.start = kHighAt + kPpq;
    expect(reimport(moved, back), "exports");
    const Clip* m = audioClipAt(back, kHighAt + kPpq);
    expect(m && m->stretch == 1.0 && !m->tempoLock && std::fabs(speedOf(back, m) / speed - 1) < 1e-5, "the rate round-trips through FL export");
    fs::remove(file);
  }
  {
    // 20 s of 44 Hz in 4 beats: 1.3187 s at 182, a speed of 15.17, beyond the sampler's x0.25.
    const std::string file = writeSine("gl_k58_i.wav", 20.0, 44.0);
    const double speed = 20.0 / (4 * 60.0 / 182.0);
    const Song s = twoSections(182, file, 4, 0, kResample, 4 * kPpq, -1.0f, -1.0f);
    const Clip* a = audioClipAt(s, kLowAt);
    expect(a && a->syncBpm == 0 && a->rateFit < 1 && std::fabs(speedOf(s, a) / (speed * kAt176) - 1) < 1e-5, "a time squeezed beyond the range holds its rate too");
    const Solo la = solo(s, kLowAt), lb = solo(s, kHighAt);
    const AudioBuffer ra = renderOffline(la.song), rb = renderOffline(lb.song);
    const double lo = frequency(ra, la.from + 0.1, la.to - 0.2), hi = frequency(rb, lb.from + 0.1, lb.to - 0.2);
    expect(std::fabs(cents(lo, 44.0 * speed * kAt176)) <= 5 && std::fabs(cents(hi, 44.0 * speed)) <= 5, "and its pitch (" + num(lo) + ", " + num(hi) + " Hz)");
    fs::remove(file);
  }
  // ---------------------------------------------------- a constant tempo changes nothing
  {
    const std::string file = writeSine("gl_k58_j.wav", 1.0, 440.0);
    Builder B;
    B.d(156, 182000);
    audioChannel(B, 0, file, 20, 0, kResample);    // beyond the range
    audioChannel(B, 1, file, 64, 0, kResample);    // a window that disagrees
    audioChannel(B, 2, file, 2, 0, kResample);     // inside the range
    finish(B, {record(0, 0, 8 * kPpq, 0, 0, 1000 * 8.0f / 20.0f, 1.0), record(32 * kPpq, 1, 8 * kPpq, 1, 0, 1000, 1.0), record(64 * kPpq, 2, 2 * kPpq, 2, 0, 1000, 1.0)});
    Song s;
    expect(load(B, s), "imports a constant tempo");
    const Clip* beyond = audioClipAt(s, 0);
    const Clip* other = audioClipAt(s, 32 * kPpq);
    const Clip* inside = audioClipAt(s, 64 * kPpq);
    expect(beyond && beyond->syncBpm > 0 && beyond->tempoLock && beyond->rateFit == 1.0 && beyond->audioEnd > 0, "constant tempo: beyond the range stays as it was");
    expect(other && other->syncBpm > 0 && other->tempoLock && other->rateFit == 1.0 && std::fabs(other->audioEnd - 1.0) < 1e-6, "constant tempo: a window that disagrees stays as it was");
    expect(inside && inside->syncBpm == 0 && !inside->tempoLock && inside->rateFit == 1.0 && std::fabs(inside->audioEnd - 1.0) < 1e-6, "constant tempo: a time inside the range stays as it was");
    // Project files without a rate fit, and clips that have none: nothing is written, nothing changes.
    expect(jsonCompact(songToJson(s)).find("rateFit") == std::string::npos, "a clip without a rate fit writes nothing for it");
    Song back;
    expect(songFromJson(songToJson(s), back) && audioClipAt(back, 0) && audioClipAt(back, 0)->rateFit == 1.0, "and loads with none");
    fs::remove(file);
  }
  return finish();
}
