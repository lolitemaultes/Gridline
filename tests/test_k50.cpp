// Gridline: K-50, the pitch an imported FL audio clip is heard at, in two cases an audit of a real
// project turned up. A channel stretched to a time beyond the sampler's x0.25..x4 range still spans
// that time and, when it resamples, is heard at that speed's pitch. A clip whose source runs at a
// much higher sample rate than the engine keeps its pitch through the realtime stretch, which plays
// until the clip's offline render is ready. Every file here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/ClipRate.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kPpq = 96;
constexpr int kRate = 44100;

void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }

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

// mode: 0 resample, -1 keeps the pitch. timeUnits: 1/192 of a beat, 0 = no time.
void audioChannel(Builder& B, int index, const std::string& sample, float timeUnits, int shiftCents, int mode) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, 4), B.text(203, "Channel");
  B.text(196, sample);
  Bytes lv(24);
  lv.u32(0, 6400), lv.u32(4, 10000);
  B.data(219, lv.v);
  Bytes pr(168);
  pr.f32(96, timeUnits);
  pr.u32(100, static_cast<uint32_t>(shiftCents));
  pr.u32(108, static_cast<uint32_t>(mode));
  B.data(215, pr.v);
}

// An 88-byte audio clip record without a stored window.
std::vector<uint8_t> audioItem(uint32_t pos, uint16_t channel, uint32_t len) {
  Bytes r(88);
  r.u32(0, pos);
  r.u16(4, 0x5000);
  r.u16(6, channel);
  r.u32(8, len);
  r.u16(12, 499);
  r.u16(18, 0x40);
  r.f32(24, -1.0f);
  r.f32(28, -1.0f);
  r.f32(52, 1.0f);
  r.f64(64, 1.0);
  return r.v;
}

void finish(Builder& B, const std::vector<uint8_t>& item) {
  B.w(99, 0);
  B.data(233, item);
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

std::string writeSine(const std::string& name, double seconds, double hz, int rate) {
  const size_t n = static_cast<size_t>(seconds * rate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = b.at(1, i) = 0.5f * static_cast<float>(std::sin(2 * M_PI * hz * static_cast<double>(i) / rate));
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, rate, 24);
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

double audioEnd(const AudioBuffer& b) {
  for (size_t i = b.frames; i-- > 0;)
    if (std::fabs(b.at(0, i)) > 0.01f) return static_cast<double>(i + 1) / kRate;
  return 0;
}

double centsOff(double hz, double want) { return hz > 0 ? 1200 * std::log2(hz / want) : 1e9; }

AudioBuffer renderOffline(const Song& s) {
  RenderOptions o;
  o.sampleRate = kRate;
  return renderSong(s, o);
}

// The stretch that plays until a clip's render is ready: no render exists here.
AudioBuffer renderRealtimeStretch(const Song& s, double seconds) {
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

}  // namespace

int main() {
  // ---------------------------------------------------- times beyond x0.25..x4
  {
    // 1 s of 440 Hz resampled to 20 beats at 120 BPM: 10 s, a tenth of the speed, 44 Hz.
    const std::string slow = writeSine("gl_k50_slow.wav", 1.0, 440.0, kRate);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, slow, 20 * 192.0f, 0, 0);
    finish(B, audioItem(0, 0, 20 * kPpq));
    Song s;
    expect(load(B, s), "imports a resampled channel stretched ten times");
    const AudioBuffer a = renderOffline(s);
    const double hz = frequency(a, 1.0, 9.0);
    expect(std::fabs(centsOff(hz, 44.0)) <= 10, "ten times slower is heard a tenth of the frequency (" + std::to_string(hz) + " Hz)");
    expect(std::fabs(audioEnd(a) - 10.0) < 0.05, "and spans its 10 s (" + std::to_string(audioEnd(a)) + " s)");
  }
  {
    // 10 s of 44 Hz resampled to 1 beat at 120 BPM: 0.5 s, twenty times the speed, 880 Hz.
    const std::string fast = writeSine("gl_k50_fast.wav", 10.0, 44.0, kRate);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, fast, 192.0f, 0, 0);
    finish(B, audioItem(0, 0, kPpq));
    Song s;
    expect(load(B, s), "imports a resampled channel squeezed twenty times");
    const AudioBuffer a = renderOffline(s);
    const double hz = frequency(a, 0.05, 0.45);
    expect(std::fabs(centsOff(hz, 880.0)) <= 10, "twenty times faster is heard twenty times the frequency (" + std::to_string(hz) + " Hz)");
  }
  {
    // The same time on a channel that keeps its pitch: the pitch stays and the time holds.
    const std::string slow = writeSine("gl_k50_keep.wav", 1.0, 440.0, kRate);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, slow, 20 * 192.0f, 300, -1);
    finish(B, audioItem(0, 0, 20 * kPpq));
    Song s;
    expect(load(B, s), "imports a pitch-keeping channel stretched ten times");
    const AudioBuffer a = renderOffline(s);
    const double hz = frequency(a, 1.0, 9.0);
    expect(std::fabs(centsOff(hz, 440.0 * std::pow(2.0, 0.25))) <= 10, "a kept pitch stays where its knob puts it (" + std::to_string(hz) + " Hz)");
    expect(std::fabs(audioEnd(a) - 10.0) < 0.3, "and the stretch still spans its 10 s (" + std::to_string(audioEnd(a)) + " s)");
    // The FL export writes the clip's own stretch: the channel's time goes back with it.
    flp::Bytes bytes;
    flp::ExportReport rep;
    flp::ExportOptions o;
    expect(flp::exportFlp(s, bytes, rep, o), "exports");
    Song back;
    flp::Report r;
    std::string err;
    expect(importFlpBytes(bytes, back, r, &err), "imports its export");
    const Clip* c0 = nullptr;
    const Clip* c1 = nullptr;
    for (const auto& c : s.playlist) if (c.kind == ClipKind::Audio) c0 = &c;
    for (const auto& c : back.playlist) if (c.kind == ClipKind::Audio) c1 = &c;
    expect(c0 && c1 && std::fabs(c0->stretch - c1->stretch) < 1e-9 && std::fabs(c0->syncBpm - c1->syncBpm) < 1e-6, "the stretch and the tempo fit round-trip");
  }
  {
    // Right at the range's edge the channel's own stretch still carries it, with the pitch kept and shifted.
    const std::string edge = writeSine("gl_k50_edge.wav", 1.0, 440.0, kRate);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, edge, 8 * 192.0f, 300, -1);
    finish(B, audioItem(0, 0, 8 * kPpq));
    Song s;
    expect(load(B, s), "imports a pitch-keeping channel stretched four times");
    const AudioBuffer a = renderOffline(s);
    expect(std::fabs(centsOff(frequency(a, 1.0, 3.0), 440.0 * std::pow(2.0, 0.25))) <= 10, "four times longer, the kept pitch stays");
  }
  {
    // A time inside the range at a constant tempo is what it always was: the channel's own stretch, not a lock.
    const std::string two = writeSine("gl_k50_inside.wav", 1.0, 440.0, kRate);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, two, 4 * 192.0f, 0, 0);
    finish(B, audioItem(0, 0, 4 * kPpq));
    Song s;
    expect(load(B, s), "imports a channel stretched twice");
    const Clip* c = nullptr;
    for (const auto& x : s.playlist) if (x.kind == ClipKind::Audio) c = &x;
    expect(c && c->syncBpm == 0 && !c->tempoLock, "a time inside the range does not lock the clip to the tempo");
    const AudioBuffer a = renderOffline(s);
    expect(std::fabs(centsOff(frequency(a, 0.2, 1.8), 220.0)) <= 10, "twice as long is an octave lower");
  }
  // ---------------------------------------------------- source rates above the engine's
  for (const int srcRate : {192000, 96000, 48000, 22050}) {
    for (const int shift : {1130, 0, -1100}) {
      // 4 s of 1 kHz through the realtime stretch, its pitch kept and shifted.
      const std::string hi = writeSine("gl_k50_rate.wav", 4.0, 1000.0, srcRate);
      Builder B;
      B.d(156, 120000);
      audioChannel(B, 0, hi, 0, shift, -1);
      finish(B, audioItem(0, 0, 8 * kPpq));
      Song s;
      const std::string name = std::to_string(srcRate) + " Hz source, " + std::to_string(shift) + " cents";
      expect(load(B, s), "imports " + name);
      const AudioBuffer a = renderRealtimeStretch(s, 4.0);
      const double want = 1000.0 * std::pow(2.0, shift / 1200.0);
      const double hz = frequency(a, 1.0, 3.0);
      expect(std::fabs(centsOff(hz, want)) <= 10, name + ": the realtime stretch holds the pitch (" + std::to_string(hz) + " Hz)");
      const AudioBuffer off = renderOffline(s);
      const double hzOff = frequency(off, 1.0, 3.0);
      expect(std::fabs(centsOff(hzOff, want)) <= 10, name + ": the offline render holds it too (" + std::to_string(hzOff) + " Hz)");
    }
  }
  {
    // Fast reads: 192 kHz at twice the speed is over 8 source frames per output frame.
    const std::string hi = writeSine("gl_k50_speed.wav", 8.0, 1000.0, 192000);
    Builder B;
    B.d(156, 120000);
    audioChannel(B, 0, hi, 8 * 192.0f, 0, -1);  // 8 beats at 120 BPM: 4 s of the 8 s file
    finish(B, audioItem(0, 0, 8 * kPpq));
    Song s;
    expect(load(B, s), "imports a fast read of a 192 kHz file");
    const AudioBuffer a = renderRealtimeStretch(s, 5.0);
    expect(std::fabs(centsOff(frequency(a, 1.0, 3.0), 1000.0)) <= 10, "a fast read keeps its pitch");
    expect(std::fabs(audioEnd(a) - 4.0) < 0.2, "and reaches the file's end when the clip ends (" + std::to_string(audioEnd(a)) + " s)");
  }
  return finish();
}
