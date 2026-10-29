// Gridline — J-176 tests: Slicex states whose sample path is 255 bytes or longer
// (FL writes the length as 0xFF and a u32; such paths build up when a project
// has been moved between machines) still bring their chops into Kerf: the full
// state parses, the regions come from the embedded deck audio, every key plays
// its chop, decks with hundreds of regions keep all of them (keys from 0 up),
// and the export writes the long path back the same way.
// All states and audio are synthetic (no FL content needed).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Platform.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpEncode.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "engine/SlicexEngine.h"

using namespace gl;
namespace sx = gl::slicex;
namespace enc = gl::flp::enc;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint32_t kFrames = 48000;

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

int16_t noise(uint32_t i) {
  uint32_t x = i * 2654435761u + 777u;
  x ^= x >> 13, x *= 0x5bd1e995u, x ^= x >> 15;
  return static_cast<int16_t>(static_cast<int32_t>(x % 40001u) - 20000);
}

// 16-bit mono noise WAV with one cue per start (cue ids 1..n) and an empty
// "LIST adtl" (as FL stores loops it sliced itself: positions only).
Bytes wav(const std::vector<uint32_t>& starts) {
  Bytes body;
  putId(body, "WAVE");
  Bytes fmt;
  put16(fmt, 1), put16(fmt, 1), put32(fmt, 44100), put32(fmt, 88200), put16(fmt, 2), put16(fmt, 16);
  chunk(body, "fmt ", fmt);
  Bytes data;
  for (uint32_t i = 0; i < kFrames; ++i) put16(data, static_cast<uint16_t>(noise(i)));
  chunk(body, "data", data);
  Bytes c;
  put32(c, static_cast<uint32_t>(starts.size()));
  for (size_t k = 0; k < starts.size(); ++k) {
    put32(c, static_cast<uint32_t>(k + 1)), put32(c, starts[k]);
    putId(c, "data");
    put32(c, 0), put32(c, 0), put32(c, starts[k]);
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

// A path of `n` bytes like the ones FL piles up after moves between machines.
std::string longPath(size_t n) {
  std::string p;
  while (p.size() < n) p += "%FLStudioFactoryData%/";
  p = p.substr(0, n > 12 ? n - 12 : 0);
  p += "/loop_96.wav";
  return p.substr(0, n);
}

// A Slicex state: deck A embeds `riff` under `path`, one default record per region.
Bytes state(const Bytes& riff, const std::string& path, size_t regions) {
  sx::Model m = sx::defaultModel();
  m.decks[0].rawPath = path;
  m.decks[0].regions.assign(std::max<size_t>(1, regions), sx::Region{});
  return sx::writeState(m, {riff, Bytes{}});
}

std::vector<float> play(const PluginState& st, double key, size_t n) {
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto gen = createGenerator(st.type);
  std::vector<float> L(n, 0.0f), R(n, 0.0f);
  if (!gen) return L;
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  gen->noteOn(1, key, 1.0f, 0);
  for (size_t p = 0; p < n; p += 128) gen->process(st, aux, ctx, L.data() + p, R.data() + p, static_cast<int>(std::min<size_t>(128, n - p)));
  return L;
}

// Normalised correlation of the output (after the declick) with the audio from `start`.
double corrAt(const std::vector<float>& out, int64_t start, int from = 64, int to = 264) {
  double best = -1;
  for (int lag = -2; lag <= 2; ++lag) {
    double xy = 0, xx = 0, yy = 0;
    for (int i = from; i < to; ++i) {
      const int64_t f = start + i - lag;
      if (f < 0 || f >= kFrames) continue;
      const double x = out[static_cast<size_t>(i)], y = noise(static_cast<uint32_t>(f));
      xy += x * y, xx += x * x, yy += y * y;
    }
    best = std::max(best, xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0);
  }
  return best;
}

void testParseLongPath() {
  const std::vector<uint32_t> starts = {0, 1500, 3200, 4100, 6000, 9000};
  const Bytes w = wav(starts);
  for (size_t n : {size_t{254}, size_t{255}, size_t{256}, size_t{342}, size_t{70000}}) {
    const std::string p = longPath(n);
    const Bytes s = state(w, p, starts.size());
    sx::Model m;
    std::array<Bytes, 2> rf;
    const bool ok = sx::parseState(s.data(), s.size(), m, rf);
    const std::string tag = " (" + std::to_string(n) + "-byte path)";
    expect(ok && m.decks[0].rawPath == p && rf[0] == w && m.decks[0].regions.size() == starts.size(), ("parse: full state, path and deck audio" + tag).c_str());
    expect(ok && sx::writeState(m, rf) == s, ("write: byte for byte" + tag).c_str());
    const bool escaped = s[0x60] == 0xFF;
    expect(escaped == (n >= 255), ("write: 0xFF + u32 length from 255 bytes on" + tag).c_str());
  }
}

void testDecodeLongPath() {
  const enc::EncodeContext ec;
  const std::vector<uint32_t> starts = {0, 1500, 3200, 4100, 6000, 9000};
  const Bytes w = wav(starts);
  const Bytes s = state(w, longPath(342), starts.size());
  gl::flp::PluginMapping pm;
  const bool ok = enc::decodeState("Slicex", s, pm, ec);
  expect(ok && pm.state.type == "gl.slicer" && std::lround(pm.state.params[static_cast<size_t>(pp::SlEngine)]) == 1,
         "decode: long-path Slicex opens on the region engine");
  if (!ok) return;
  sx::Model md;
  bool regs = sx::modelOf(pm.state.data, md) && md.decks[0].regions.size() == starts.size();
  for (size_t k = 0; regs && k < starts.size(); ++k) regs = md.decks[0].regions[k].start == starts[k];
  expect(regs, "decode: every chop of the embedded deck audio arrives, in place");
  expect(!sx::sampleOf(pm.state.data).empty() && std::filesystem::is_regular_file(sx::sampleOf(pm.state.data)),
         "decode: the embedded audio is stored as the deck sample");
  bool markers = true;
  for (size_t k = 0; k < starts.size(); ++k)
    markers = markers && std::fabs(pm.state.params[static_cast<size_t>(pp::slMarker(static_cast<int>(k)))] - static_cast<double>(starts[k]) / kFrames) < 1e-6;
  expect(markers, "decode: slice markers at the chop starts");
  bool plays = true;
  for (size_t k = 0; k < starts.size(); ++k) plays = plays && corrAt(play(pm.state, 60.0 + static_cast<double>(k), 400), starts[k]) > 0.99;
  expect(plays, "decode + play: C5 upwards play the chops in order");
  // Export: unchanged, the long path and the audio go back as they came.
  const enc::Result r = enc::encode(pm.state, "Slicex", &s, ec);
  expect(r.hasState && r.state == s, "export: unchanged long-path state written back byte for byte");
  // Edited (a new key): still a full state with the long path.
  PluginState g = pm.state;
  g.params[static_cast<size_t>(pp::slParam2(2, pp::SliceKey))] = 48;
  const enc::Result r2 = enc::encode(g, "Slicex", &s, ec);
  sx::Model back;
  std::array<Bytes, 2> rf;
  const bool ok2 = r2.hasState && sx::parseState(r2.state.data(), r2.state.size(), back, rf);
  const sx::WavRegions wr = sx::readWavRegions(rf[0].data(), rf[0].size());
  expect(ok2 && back.decks[0].rawPath == longPath(342) && wr.regions.size() == starts.size() && wr.regions[2].key == 48,
         "export: edited long-path state keeps its path and carries the new key");
}

void testManyRegions() {
  const enc::EncodeContext ec;
  std::vector<uint32_t> starts;
  for (uint32_t k = 0; k < 300; ++k) starts.push_back(100 + k * 150);
  const Bytes w = wav(starts);
  const Bytes s = state(w, longPath(300), starts.size());
  gl::flp::PluginMapping pm;
  if (!enc::decodeState("Slicex", s, pm, ec)) {
    expect(false, "many regions: decode");
    return;
  }
  sx::Model md;
  expect(sx::modelOf(pm.state.data, md) && md.decks[0].regions.size() == 300, "many regions: all 300 chops kept in the model");
  const sx::KeyMap km = sx::buildKeyMap(md, &pm.state);
  expect(sx::regionKey(km, 0, 0) == 0 && sx::regionKey(km, 0, 2) == 2 && sx::regionKey(km, 0, 127) == 127 && sx::regionKey(km, 0, 128) == -1,
         "many regions: automatic keys from 0 (region k on key k up to 127)");
  expect(corrAt(play(pm.state, 2, 300), starts[2], 40, 140) > 0.99 && corrAt(play(pm.state, 100, 300), starts[100], 40, 140) > 0.99,
         "many regions: low and high keys play their chops (past the 64 slice parameters)");
}

void testClassicLongPath() {
  // A state the full parser does not take (cut short after the deck audio): the
  // fallback reading still finds the audio after a long path.
  const std::vector<uint32_t> starts = {0, 2000, 5000};
  const Bytes w = wav(starts);
  Bytes s = state(w, longPath(400), starts.size());
  const size_t riffEnd = 0x60 + 5 + 400 + 4 + w.size();
  s.resize(riffEnd + 0x190 + 68 * 3, 0);
  put32(s, 0);  // trailing junk: no full parse
  sx::Model m;
  std::array<Bytes, 2> rf;
  expect(!sx::parseState(s.data(), s.size(), m, rf), "classic: cut state is not a full parse");
  gl::flp::PluginMapping pm;
  const enc::EncodeContext ec;
  const bool ok = enc::decodeState("Slicex", s, pm, ec);
  bool markers = ok && pm.state.type == "gl.slicer" && !pm.state.data.empty();
  for (size_t k = 0; markers && k < starts.size(); ++k)
    markers = std::fabs(pm.state.params[static_cast<size_t>(pp::slMarker(static_cast<int>(k)))] - static_cast<double>(starts[k]) / kFrames) < 1e-6;
  expect(markers, "classic: the chops of a long-path state still arrive");
}

}  // namespace

int main() {
  std::filesystem::create_directories("j176");
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", std::filesystem::absolute("j176/cache").string().c_str());
  testParseLongPath();
  testDecodeLongPath();
  testManyRegions();
  testClassicLongPath();
  return finish();
}
