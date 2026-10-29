// Gridline — J-180 tests: a note's fine pitch (and the channel / master pitch)
// tunes what a key-mapped instrument plays without moving the note to another
// key. FL stores fine pitch per note (byte 120 = 0, 10 cents a unit, ±1200);
// notes pitched by it on a Slicex channel play their own chop, pitched, in
// Kerf (they played the chop of another key, or nothing, before).
// All files and audio are synthetic: a sine per region, so the pitch of every
// note can be measured on the render.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Platform.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "engine/Render.h"
#include "engine/SlicexEngine.h"

using namespace gl;
namespace sx = gl::slicex;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr int kRegions = 16;
constexpr uint32_t kRegionFrames = 22050;  // 0.5 s at 44.1 kHz
constexpr uint32_t kFrames = kRegions * kRegionFrames;
constexpr double kPi = 3.14159265358979323846;

double regionHz(int k) { return 211.0 + 41.0 * k; }  // no tested pitch ratio lands near another region

void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFF));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void putId(Bytes& b, const char* id) { b.insert(b.end(), id, id + 4); }
void chunk(Bytes& out, const char* id, const Bytes& body) {
  putId(out, id);
  put32(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  if (body.size() & 1) out.push_back(0);
}

// 16-bit mono WAV: region k is a sine of regionHz(k) (phase 0 at its start), one cue per region.
Bytes wav() {
  Bytes body;
  putId(body, "WAVE");
  Bytes fmt;
  put16(fmt, 1), put16(fmt, 1), put32(fmt, 44100), put32(fmt, 88200), put16(fmt, 2), put16(fmt, 16);
  chunk(body, "fmt ", fmt);
  Bytes data;
  for (uint32_t i = 0; i < kFrames; ++i) {
    const int k = static_cast<int>(i / kRegionFrames);
    const double t = static_cast<double>(i % kRegionFrames) / 44100.0;
    put16(data, static_cast<uint16_t>(static_cast<int16_t>(std::lround(16000.0 * std::sin(2 * kPi * regionHz(k) * t)))));
  }
  chunk(body, "data", data);
  Bytes c;
  put32(c, kRegions);
  for (uint32_t k = 0; k < kRegions; ++k) {
    put32(c, k + 1), put32(c, k * kRegionFrames);
    putId(c, "data");
    put32(c, 0), put32(c, 0), put32(c, k * kRegionFrames);
  }
  chunk(body, "cue ", c);
  Bytes l;
  putId(l, "adtl");
  chunk(body, "LIST", l);
  Bytes riff;
  putId(riff, "RIFF");
  put32(riff, static_cast<uint32_t>(body.size()));
  riff.insert(riff.end(), body.begin(), body.end());
  return riff;
}

Bytes slicexState(const Bytes& riff) {
  sx::Model m = sx::defaultModel();
  m.decks[0].rawPath = "%FLStudioUserData%/j180_tones.wav";
  m.decks[0].regions.assign(kRegions, sx::Region{});
  return sx::writeState(m, {riff, Bytes{}});
}

// FLP event stream builder (as test_flp.cpp).
struct Builder {
  Bytes ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(static_cast<uint8_t>(v >> 8)); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const Bytes& p) {
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
    Bytes p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  Bytes file(int ppq) const {
    Bytes f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    put32(f, static_cast<uint32_t>(ev.size()));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

// One FL note record: flags 0x4000 as FL writes them, `fine` = FL's byte (120 = none).
Bytes note(uint32_t pos, uint32_t len, int key, int fine) {
  Bytes n;
  put32(n, pos);
  put16(n, 0x4000);
  put16(n, 0);  // channel 0
  put32(n, len);
  put16(n, static_cast<uint16_t>(key));
  put16(n, 0);  // group
  n.push_back(static_cast<uint8_t>(fine)), n.push_back(0), n.push_back(64), n.push_back(0);
  n.push_back(64), n.push_back(100), n.push_back(128), n.push_back(128);
  return n;
}

// The notes under test: key, FL fine byte, the cents it means.
struct Case {
  int key, fineByte, cents;
};
const std::array<Case, 7> kCases = {{{60, 240, 1200}, {61, 190, 700}, {62, 0, -1200}, {63, 142, 220}, {64, 121, 10}, {65, 120, 0}, {66, 169, 490}}};
constexpr uint32_t kSpacing = 192;  // ticks between notes (1 s at 120 BPM, 96 PPQ)
constexpr uint32_t kNoteLen = 48;   // 0.25 s

Bytes fixtureFlp() {
  Builder B;
  B.text(199, "25.2.3.5164");
  B.d(156, 120000);
  B.text(194, "J-180 fine pitch");
  B.w(65, 1);
  Bytes notes;
  for (size_t i = 0; i < kCases.size(); ++i) {
    const Bytes n = note(static_cast<uint32_t>(i) * kSpacing, kNoteLen, kCases[i].key, kCases[i].fineByte);
    notes.insert(notes.end(), n.begin(), n.end());
  }
  B.data(224, notes);
  B.w(64, 0), B.b(21, 2), B.text(201, "Slicex"), B.text(192, "Chops");
  B.data(213, slicexState(wav()));
  return B.file(96);
}

// Frequency from interpolated upward zero crossings over [from, to) seconds (left channel).
double measureHz(const AudioBuffer& b, double from, double to) {
  const size_t a = static_cast<size_t>(from * 44100), z = std::min(b.frames, static_cast<size_t>(to * 44100));
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = a + 1; i < z; ++i) {
    const float p = b.data[i - 1], q = b.data[i];
    if (p < 0 && q >= 0) {
      const double t = static_cast<double>(i - 1) + p / static_cast<double>(p - q);
      if (first < 0) first = t;
      last = t;
      ++n;
    }
  }
  return n > 2 ? (n - 1) * 44100.0 / (last - first) : 0.0;
}

double peak(const AudioBuffer& b, double from, double to) {
  double m = 0;
  for (size_t i = static_cast<size_t>(from * 44100); i < std::min(b.frames, static_cast<size_t>(to * 44100)); ++i) m = std::max(m, std::fabs(static_cast<double>(b.data[i])));
  return m;
}

AudioBuffer renderPattern(const Song& s) {
  RenderOptions o;
  o.patternMode = true;
  o.pattern = s.patterns.empty() ? kNoId : s.patterns[0].id;
  return renderSong(s, o);
}

// Every case plays its own region at its key, tuned by its fine pitch plus `extraSemis`.
bool pitchesRight(const AudioBuffer& b, double extraSemis, const std::string& what, bool print = false) {
  bool ok = true;
  for (size_t i = 0; i < kCases.size(); ++i) {
    const double t0 = static_cast<double>(i) * 1.0;  // seconds (192 ticks at 120 BPM)
    const double want = regionHz(kCases[i].key - 60) * std::pow(2.0, (kCases[i].cents / 100.0 + extraSemis) / 12.0);
    const double got = measureHz(b, t0 + 0.04, t0 + 0.2);
    const bool good = std::fabs(got / want - 1.0) < 0.002;
    if (!good || print) std::printf("  %s: key %d fine %+d c: %.2f Hz (want %.2f)\n", what.c_str(), kCases[i].key, kCases[i].cents, got, want);
    ok = ok && good;
  }
  return ok;
}

bool importFixture(Song& s, flp::Report& rep) {
  std::string err;
  return importFlpBytes(fixtureFlp(), s, rep, &err, "", "", true);
}

void testImport() {
  Song s;
  flp::Report rep;
  const bool ok = importFixture(s, rep);
  expect(ok && s.channels.size() == 1 && s.channels[0].plugin.type == "gl.slicer" &&
             std::lround(s.channels[0].plugin.params[static_cast<size_t>(pp::SlEngine)]) == 1,
         "import: the Slicex channel opens in Kerf's region engine");
  if (!ok || s.patterns.empty() || s.channels.empty()) return;
  const auto& ns = s.patterns[0].notes.at(s.channels[0].id);
  bool fine = ns.size() == kCases.size();
  for (size_t i = 0; fine && i < ns.size(); ++i) fine = ns[i].pitch == kCases[i].key && ns[i].fine == kCases[i].cents;
  expect(fine, "import: every note keeps its key and its fine pitch (10 cents a unit, -1200..+1200)");
  expect(rep.counts["notes: fine pitch"] == 6, "import: report counts the fine-pitched notes");
}

void testPlayback() {
  Song s;
  flp::Report rep;
  if (!importFixture(s, rep) || s.channels.empty()) {
    expect(false, "playback: fixture imports");
    return;
  }
  const AudioBuffer b = renderPattern(s);
  expect(pitchesRight(b, 0, "fine"), "play: each fine-pitched note plays its own chop, pitched by its fine pitch (+-1200 c too)");
  // Channel pitch and master pitch tune the chops; they never pick another key.
  Song c = s;
  c.channels[0].pitch = 2;
  expect(pitchesRight(renderPattern(c), 2, "channel +2"), "play: channel pitch tunes the chops (keys stay)");
  Song m = s;
  m.masterPitch = -1;
  expect(pitchesRight(renderPattern(m), -1, "master -1"), "play: master pitch tunes the chops (keys stay)");
  // The key range filters by the written key: the +1200 c note on key 60 is in 60..60.
  Song r = s;
  r.channels[0].keyLo = 60, r.channels[0].keyHi = 60;
  const AudioBuffer rb = renderPattern(r);
  expect(peak(rb, 0.04, 0.2) > 0.05 && peak(rb, 1.04, 6.9) < 1e-4, "play: key range uses the written key, not key + fine pitch");
}

void testExportRoundTrip() {
  Song s;
  flp::Report rep;
  if (!importFixture(s, rep)) {
    expect(false, "round trip: fixture imports");
    return;
  }
  for (bool origin : {true, false}) {
    flp::ExportOptions o;
    o.useOrigin = origin;
    flp::ExportReport er;
    flp::Bytes out;
    const bool ok = flp::exportFlp(s, out, er, o);
    Song back;
    flp::Report r2;
    std::string err;
    const bool ok2 = ok && importFlpBytes(out, back, r2, &err, "", "", false);
    const std::string tag = origin ? " (with FL origin)" : " (fresh)";
    expect(ok2 && back.channels.size() == 1 && back.channels[0].plugin.type == "gl.slicer", ("round trip: exports and imports" + tag).c_str());
    if (!ok2 || back.channels.empty() || back.patterns.empty()) continue;
    const auto& a = s.patterns[0].notes.at(s.channels[0].id);
    const auto it = back.patterns[0].notes.find(back.channels[0].id);
    bool same = it != back.patterns[0].notes.end() && it->second.size() == a.size();
    for (size_t i = 0; same && i < a.size(); ++i) same = it->second[i].pitch == a[i].pitch && it->second[i].fine == a[i].fine;
    expect(same, ("round trip: keys and fine pitches come back" + tag).c_str());
    expect(pitchesRight(renderPattern(back), 0, "round trip"), ("round trip: the exported project plays the same pitches" + tag).c_str());
  }
  // Fine bytes in the written note records (120 + cents / 10).
  flp::ExportOptions o;
  o.useOrigin = false;
  flp::ExportReport er;
  flp::Bytes out;
  if (flp::exportFlp(s, out, er, o)) {
    flp::File f;
    std::string err;
    bool bytes = flp::readBytes(out, f, &err);
    std::vector<int> fines;
    for (const auto& e : f.events)
      if (e.id == 224)
        for (uint32_t k = 0; k + 24 <= e.size; k += 24) fines.push_back(e.u8(k + 16));
    bytes = bytes && fines.size() == kCases.size();
    for (size_t i = 0; bytes && i < kCases.size(); ++i) bytes = fines[i] == kCases[i].fineByte;
    expect(bytes, "export: note records carry FL's fine byte (240 = +1200 c, 0 = -1200 c)");
  }
}

// Kerf's classic engine (markers) and the region engine take the key from the note
// props' detune when driven directly (live input and other hosts of the instrument).
void testGeneratorDetune() {
  const std::string path = std::filesystem::absolute("j180/tones.wav").string();
  {
    const Bytes w = wav();
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      std::fwrite(w.data(), 1, w.size(), fp);
      std::fclose(fp);
    }
  }
  PluginState st = defaultPluginState("gl.slicer");
  st.data = path;
  st.params[static_cast<size_t>(pp::SlEngine)] = 0;
  st.params[static_cast<size_t>(pp::SlMode)] = pp::SlModeManual;
  for (int k = 0; k < kRegions; ++k) st.params[static_cast<size_t>(pp::slMarker(k))] = static_cast<float>(k) / kRegions;
  for (int k = kRegions; k < pp::kMaxSlices; ++k) st.params[static_cast<size_t>(pp::slMarker(k))] = -1;
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto run = [&](double pitch, float detune) {
    auto gen = createGenerator(st.type);
    AudioBuffer b(1, 22050);
    if (!gen) return b;
    std::vector<float> L(b.frames, 0.0f), R(b.frames, 0.0f);
    ProcessContext ctx;
    ctx.sampleRate = 44100;
    ctx.tempo = 120;
    NoteProps np;
    np.detune = detune;
    gen->noteOn(1, pitch, 1.0f, 0, np);
    for (size_t p = 0; p < b.frames; p += 256)
      gen->process(st, aux, ctx, L.data() + p, R.data() + p, static_cast<int>(std::min<size_t>(256, b.frames - p)));
    b.data = L;
    return b;
  };
  const double base = regionHz(2);
  const double octave = measureHz(run(62 + 12, 12), 0.03, 0.2), fifth = measureHz(run(62 + 7, 7), 0.03, 0.2);
  expect(std::fabs(octave / (2 * base) - 1) < 0.002 && std::fabs(fifth / (base * std::pow(2.0, 7.0 / 12)) - 1) < 0.002,
         "classic slicer: a detuned note plays the slice of its key, pitched");
  const double plain = measureHz(run(62 + 7, 0), 0.03, 0.2);
  expect(std::fabs(plain / regionHz(9) - 1) < 0.002, "classic slicer: without detune the pitch is the key (unchanged)");
}

}  // namespace

int main() {
  std::filesystem::create_directories("j180");
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", std::filesystem::absolute("j180/cache").string().c_str());
  testImport();
  testPlayback();
  testExportRoundTrip();
  testGeneratorDetune();
  return finish();
}
