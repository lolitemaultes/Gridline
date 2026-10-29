// Gridline — J-177: FL channel stretch "time" (channel parameters 215 @96).
// Built from synthetic bytes and a synthetic sample (no FL Studio content):
//  * audio-clip channels: the sample spans the stored beats at the project
//    tempo (FL 2026 f32 and older u32 encodings, 1/192 beat units), on top of
//    the multiplier; Resample changes the pitch with it, the stretch keeps it;
//  * the rendered clicks land on the beat grid (Resample and Stretch);
//  * a time FL keeps without using (@89), a missing sample: no stretch, reported;
//  * sampler channels: stretch modes and Resample (pitch);
//  * the export keeps FL's time (verbatim and edited channels), round trip.
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/ClipRate.h"
#include "../src/core/PluginParams.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSrcSec = 2.0;     // the sample: four clicks, one every 0.5 s (120 BPM beats)
constexpr double kTempo = 100.0;    // the project: four beats are 2.4 s
constexpr double kFit = 2.4 / 2.0;  // the time factor the channel's 4-beat time gives

std::string writeClicks() {
  const size_t n = static_cast<size_t>(kSrcSec * kSr);
  AudioBuffer b(2, n);
  for (int k = 0; k < 4; ++k) {
    const size_t at = static_cast<size_t>(k * 0.5 * kSr);
    for (size_t i = 0; i < static_cast<size_t>(0.004 * kSr); ++i) {
      const double t = static_cast<double>(i) / kSr;
      const float v = static_cast<float>(0.9 * std::exp(-t * 900.0) * std::sin(2 * kPi * 1000.0 * t));
      b.at(0, at + i) = b.at(1, at + i) = v;
    }
  }
  const std::string path = (fs::temp_directory_path() / "gl_j177_clicks.wav").generic_string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  sampleCache().clear();
  return path;
}

struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(static_cast<uint8_t>(v >> 8)); }
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
  void text(uint8_t id, const std::string& s) {  // UTF-16LE + NUL
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

void p16(std::vector<uint8_t>& r, size_t at, uint32_t v) { r[at] = static_cast<uint8_t>(v), r[at + 1] = static_cast<uint8_t>(v >> 8); }
void p32(std::vector<uint8_t>& r, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) r[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}
void pf(std::vector<uint8_t>& r, size_t at, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  p32(r, at, u);
}
uint32_t g32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); }

// The channel's time stretch settings (215, FL's 168-byte layout).
struct Stretch {
  double time = 0;       // 1/192 beats
  bool floatTime = true; // FL 2026: f32; earlier: u32
  int mode = -1;         // FL method: 0 resample, -1 stretch
  int mul = 0;           // log2(multiplier) x 10000
  bool unused = false;   // @89: FL keeps the time without using it
};
std::vector<uint8_t> params215(const Stretch& st) {
  std::vector<uint8_t> r(168, 0);
  if (st.floatTime) pf(r, 96, static_cast<float>(st.time));
  else p32(r, 96, static_cast<uint32_t>(st.time));
  p32(r, 104, static_cast<uint32_t>(st.mul));
  p32(r, 108, static_cast<uint32_t>(st.mode));
  r[89] = st.unused ? 1 : 0;
  const double one = 1.0;
  std::memcpy(r.data() + 136, &one, 8);  // sample region 0..1
  return r;
}

// One 88-byte playlist record: channel `item`, `len` ticks at `pos`, the whole sample.
std::vector<uint8_t> clipRec(uint32_t pos, uint16_t item, uint32_t len, int track) {
  std::vector<uint8_t> r(88, 0);
  p32(r, 0, pos);
  p16(r, 4, 0x5000);
  p16(r, 6, item);
  p32(r, 8, len);
  p16(r, 12, static_cast<uint32_t>(499 - track));
  p16(r, 16, 0x78), p16(r, 18, 0x40);
  pf(r, 24, -1), pf(r, 28, -1);
  pf(r, 52, 1);
  const double one = 1.0;
  std::memcpy(r.data() + 64, &one, 8);
  p32(r, 76, 0xFFFFFFFFu);
  return r;
}

// A project at 100 BPM: channel 0 of FL type `type` (4 audio clip, 0 sampler)
// playing `path` with `st`; for audio clips one 4-beat clip at the song start.
std::vector<uint8_t> project(int type, const std::string& path, const Stretch& st) {
  Builder B;
  B.text(199, "26.1.4.5356");
  B.d(156, static_cast<uint32_t>(kTempo * 1000));
  B.w(64, 0), B.b(21, static_cast<uint8_t>(type)), B.text(192, "Loop");
  B.text(196, path);
  B.data(215, params215(st));
  if (type == 4) {
    B.w(99, 0);
    B.text(241, "Song");
    B.data(233, clipRec(0, 0, 384, 0));
  }
  return B.file(96);
}

bool load(const std::vector<uint8_t>& bytes, Song& s, flp::Report& rep) {
  std::string err;
  return importFlpBytes(bytes, s, rep, &err, "/nonexistent");
}
float param(const Song& s, int k) {
  return s.channels.empty() || static_cast<size_t>(k) >= s.channels[0].plugin.params.size() ? -1.0f : s.channels[0].plugin.params[static_cast<size_t>(k)];
}
const Clip* audioClip(const Song& s) {
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Audio) return &c;
  return nullptr;
}

// Time (s) of the loudest sample of x within t +- 0.08 s.
double peakNear(const std::vector<float>& x, double t) {
  const size_t a = static_cast<size_t>(std::max(0.0, t - 0.08) * kSr), b = std::min(x.size(), static_cast<size_t>((t + 0.08) * kSr));
  size_t best = a;
  for (size_t i = a; i < b; ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return b > a && std::fabs(x[best]) > 0.05f ? static_cast<double>(best) / kSr : -1.0;
}

// The clip's clicks land on the project's beats (0.6 s apart at 100 BPM).
bool onGrid(const Song& s, std::string& why) {
  RenderOptions o;
  AudioBuffer b = renderSong(s, o);
  std::vector<float> y(b.plane(0), b.plane(0) + b.frames);
  bool ok = true;
  why.clear();
  for (int k = 0; k < 4; ++k) {
    const double want = k * 60.0 / kTempo, got = peakNear(y, want);
    char t[64];
    std::snprintf(t, sizeof t, " %.3f", got);
    why += t;
    ok = ok && got >= 0 && std::fabs(got - want) < 0.015;
  }
  return ok;
}

// The channel's 215 event in an exported project.
std::vector<uint8_t> exported215(const flp::Bytes& out) {
  flp::File f;
  std::string err;
  if (!flp::readBytes(out, f, &err)) return {};
  for (const auto& e : f.events)
    if (e.id == 215 && e.size >= 112) return std::vector<uint8_t>(e.data, e.data + e.size);
  return {};
}
}  // namespace

int main() {
  const std::string path = writeClicks();

  // ---- audio-clip channels: the time fits the sample to its beats ----
  for (int mode : {-1, 0}) {
    for (bool flt : {true, false}) {
      const std::string tag = std::string(mode == 0 ? "Resample" : "Stretch") + (flt ? ", f32 time: " : ", u32 time: ");
      Stretch st;
      st.time = 4 * 192, st.floatTime = flt, st.mode = mode;
      Song s;
      flp::Report rep;
      expect(load(project(4, path, st), s, rep), tag + "imports");
      expect(std::fabs(param(s, pp::SmpTimeStretch) - kFit) < 1e-4, tag + "the channel stretches x1.2 (4 beats at 100 BPM / 2 s)");
      expect(rep.counts["audio clip channels stretched to a time"] == 1, tag + "reported");
      const Clip* c = audioClip(s);
      expect(c && c->length == 384 && c->stretch == 1.0 && c->syncBpm == 0, tag + "the clip keeps its length and own stretch");
      if (c) {
        const ClipRate r = clipRate(s.channels[0], *c);
        expect(std::fabs(r.speed - 1 / kFit) < 1e-4, tag + "plays at 1/1.2 speed");
        expect(mode == 0 ? std::fabs(r.pitch - 1 / kFit) < 1e-4 : std::fabs(r.pitch - 1.0) < 1e-6,
               tag + (mode == 0 ? "the pitch follows the speed" : "the pitch stays"));
      }
      if (flt) {
        std::string why;
        const bool grid = onGrid(s, why);
        expect(grid, tag + "the rendered clicks land on the beats (at" + why + ")");
      }
    }
  }

  // ---- with the multiplier: both apply ----
  {
    Stretch st;
    st.time = 4 * 192, st.mul = 10000;  // x2
    Song s;
    flp::Report rep;
    expect(load(project(4, path, st), s, rep) && std::fabs(param(s, pp::SmpTimeStretch) - 2 * kFit) < 1e-4,
           "time and multiplier: x2.4");
  }

  // ---- a time FL keeps without using, no time, a missing sample ----
  {
    Stretch st;
    st.time = 4 * 192, st.unused = true;
    Song s;
    flp::Report rep;
    expect(load(project(4, path, st), s, rep) && param(s, pp::SmpTimeStretch) == 1.0f && !rep.counts.count("audio clip channels stretched to a time"),
           "time kept but not in use (@89): no stretch");
    std::string why;
    expect(!onGrid(s, why), "time not in use: the clicks stay at the sample's own tempo");
  }
  {
    Stretch st;
    Song s;
    flp::Report rep;
    expect(load(project(4, path, st), s, rep) && param(s, pp::SmpTimeStretch) == 1.0f, "no time: no stretch");
  }
  {
    Stretch st;
    st.time = 4 * 192;
    Song s;
    flp::Report rep;
    expect(load(project(4, "/nonexistent/gl_j177_missing.wav", st), s, rep) && param(s, pp::SmpTimeStretch) == 1.0f &&
               rep.counts["audio clip channels stretched to a time (sample not found: not stretched)"] == 1,
           "missing sample: no stretch, reported");
  }
  {
    Stretch st;
    st.time = 64 * 192;  // 38.4 s of a 2 s sample: beyond x4
    Song s;
    flp::Report rep;
    expect(load(project(4, path, st), s, rep) && param(s, pp::SmpTimeStretch) == 4.0f &&
               rep.counts["audio clip channel time stretches beyond x0.25..x4 (clamped)"] == 1,
           "an extreme time: clamped to x4, reported");
  }

  // ---- sampler channels ----
  {
    Stretch st;
    st.time = 4 * 192, st.mode = -1;
    Song s;
    flp::Report rep;
    expect(load(project(0, path, st), s, rep) && std::fabs(param(s, pp::SmpTimeStretch) - kFit) < 1e-4 &&
               rep.counts["sampler channels stretched to a time"] == 1 && !rep.counts.count("sampler stretch-to-time (approximated)"),
           "sampler, stretch: the time stretches the sample x1.2");
  }
  {
    Stretch st;
    st.time = 4 * 192, st.mode = 0;
    Song s;
    flp::Report rep;
    const bool ok = load(project(0, path, st), s, rep) && !s.channels.empty();
    const int cents = ok ? s.channels[0].pitch * 100 + s.channels[0].fine : 0;
    const int want = static_cast<int>(std::lround(-1200 * std::log2(kFit)));
    expect(ok && std::abs(cents - want) <= 1 && param(s, pp::SmpStretchMode) == 0.0f,
           "sampler, resample: the time lowers the pitch (" + std::to_string(cents) + " cents)");
  }

  // ---- export: FL's time stays, the stretch survives the round trip ----
  {
    Stretch st;
    st.time = 4 * 192;
    Song s;
    flp::Report rep;
    expect(load(project(4, path, st), s, rep), "round trip: imports");
    for (bool origin : {true, false}) {
      const std::string tag = origin ? "round trip (origin): " : "round trip (encoders): ";
      flp::Bytes out;
      flp::ExportReport xr;
      flp::ExportOptions opt;
      opt.useOrigin = origin;
      opt.renderInstruments = false;
      expect(flp::exportFlp(s, out, xr, opt), tag + "exports");
      const std::vector<uint8_t> e = exported215(out);
      if (origin) {
        float t = 0;
        if (e.size() >= 112) std::memcpy(&t, e.data() + 96, 4);
        expect(e.size() >= 112 && t == 768.0f && g32(e.data() + 104) == 0, tag + "FL's time and multiplier kept as stored");
      }
      Song b;
      flp::Report rb;
      expect(load(out, b, rb) && std::fabs(param(b, pp::SmpTimeStretch) - kFit) < 1e-3, tag + "re-import: x1.2");
      const Clip* c = audioClip(b);
      expect(c && c->length == 384 && c->stretch == 1.0, tag + "re-import: the clip");
    }
    // Edited in Gridline: x1.8 overall. FL's time stays; the multiplier carries the rest (x1.5).
    Song e = s;
    e.channels[0].plugin.params[pp::SmpTimeStretch] = static_cast<float>(1.5 * kFit);
    flp::Bytes out;
    flp::ExportReport xr;
    flp::ExportOptions opt;
    opt.renderInstruments = false;
    expect(flp::exportFlp(e, out, xr, opt), "round trip, edited: exports");
    const std::vector<uint8_t> r = exported215(out);
    float t = 0;
    if (r.size() >= 112) std::memcpy(&t, r.data() + 96, 4);
    const int32_t mul = r.size() >= 112 ? static_cast<int32_t>(g32(r.data() + 104)) : 0;
    expect(t == 768.0f && std::abs(mul - static_cast<int32_t>(std::lround(std::log2(1.5) * 10000))) <= 2,
           "round trip, edited: FL's time kept, multiplier x1.5 (" + std::to_string(mul) + ")");
    Song b;
    flp::Report rb;
    expect(load(out, b, rb) && std::fabs(param(b, pp::SmpTimeStretch) - 1.5 * kFit) < 1e-3, "round trip, edited: re-import x1.8");
  }

  std::error_code ec;
  fs::remove(path, ec);
  return finish();
}
