// Gridline — J-166 tests: imported Slicex chops land in Kerf (gl.slicer region
// engine) exactly: the deck audio's regions in stored order (cue position, lengths
// and labels by cue id, explicit keys), the key map (explicit keys first, then
// base + index while free), playback of the right region on its key, the text
// state (key, name; older text still loads) and the Slicex export, which keeps
// unchanged chops and rebuilds them when keys / starts / ends change.
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

constexpr uint32_t kFrames = 8000;

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

// Noise sample of frame i (deterministic; every region sounds different).
int16_t noise(uint32_t i) {
  uint32_t x = i * 2654435761u + 12345u;
  x ^= x >> 13, x *= 0x5bd1e995u, x ^= x >> 15;
  return static_cast<int16_t>(static_cast<int32_t>(x % 40001u) - 20000);
}

struct Cue {
  uint32_t id, pos, ofs;
};
struct Txt {
  uint32_t id, len;
};
struct Lab {
  uint32_t id;
  std::string name;
};
struct Tl {
  int32_t index;
  uint8_t key;
};

// 16-bit mono 44.1 kHz noise WAV with region chunks exactly as given (any order).
Bytes wav(const std::vector<Cue>& cues, const std::vector<Txt>& txt, const std::vector<Lab>& labs, const std::vector<Tl>& tl) {
  Bytes body;
  putId(body, "WAVE");
  Bytes fmt;
  put16(fmt, 1), put16(fmt, 1), put32(fmt, 44100), put32(fmt, 88200), put16(fmt, 2), put16(fmt, 16);
  chunk(body, "fmt ", fmt);
  Bytes data;
  for (uint32_t i = 0; i < kFrames; ++i) put16(data, static_cast<uint16_t>(noise(i)));
  chunk(body, "data", data);
  if (!cues.empty()) {
    Bytes c;
    put32(c, static_cast<uint32_t>(cues.size()));
    for (const Cue& q : cues) {
      put32(c, q.id), put32(c, q.pos);
      putId(c, "data");
      put32(c, 0), put32(c, 0), put32(c, q.ofs);
    }
    chunk(body, "cue ", c);
  }
  if (!txt.empty() || !labs.empty()) {
    Bytes l;
    putId(l, "adtl");
    for (const Txt& t : txt) {  // ltxt first: matched by cue id, not position
      Bytes e;
      put32(e, t.id), put32(e, t.len);
      putId(e, "rgn ");
      put16(e, 0), put16(e, 0), put16(e, 0), put16(e, 0);
      chunk(l, "ltxt", e);
    }
    for (const Lab& a : labs) {
      Bytes e;
      put32(e, a.id);
      e.insert(e.end(), a.name.begin(), a.name.end());
      e.push_back(0);
      chunk(l, "labl", e);
    }
    chunk(body, "LIST", l);
  }
  if (!tl.empty()) {
    Bytes t;
    put32(t, static_cast<uint32_t>(tl.size()));
    for (const Tl& e : tl) {
      putId(t, "cue ");
      put32(t, static_cast<uint32_t>(e.index)), put32(t, 1);
      t.push_back(0xFF), t.push_back(e.key), t.push_back(0x90), t.push_back(0);
      put32(t, 0), put32(t, 0);
    }
    chunk(body, "tlst", t);
  }
  Bytes riff;
  putId(riff, "RIFF");
  put32(riff, static_cast<uint32_t>(body.size()));
  riff.insert(riff.end(), body.begin(), body.end());
  return riff;
}

// The deck audio a user project can carry: regions out of time order, a duplicate
// start, a cue whose sample-offset field differs from its position, lengths and
// labels listed in another order, and remapped keys.
//   region 0: 5000, len 1000, "Late", key 72      region 1: 0, to next, "Kick", automatic
//   region 2: 2000, len 500, "Snare", key 60      region 3: 2000, to next, "Dup", automatic
//   region 4: 3000, len 1000, (no name), key 64
Bytes userWav() {
  return wav({{3, 5000, 5000}, {1, 0, 0}, {7, 2000, 999}, {2, 2000, 2000}, {5, 3000, 3000}}, {{5, 1000}, {7, 500}, {3, 1000}},
             {{2, "Dup"}, {1, "Kick"}, {3, "Late"}, {7, "Snare"}}, {{4, 64}, {2, 72}, {6, 60}});  // key list: cue id − 1
}

bool sameRegions(const std::vector<sx::Region>& a, const std::vector<sx::Region>& b) {
  if (a.size() != b.size()) return false;
  for (size_t k = 0; k < a.size(); ++k)
    if (a[k].start != b[k].start || a[k].length != b[k].length || a[k].key != b[k].key || a[k].name != b[k].name) return false;
  return true;
}

void testReadRegions() {
  const Bytes w = userWav();
  const sx::WavRegions r = sx::readWavRegions(w.data(), w.size());
  expect(r.riff && r.frames == kFrames && r.regions.size() == 5, "deck audio: five regions, frame count from the data chunk");
  if (r.regions.size() != 5) return;
  const int64_t starts[] = {5000, 0, 2000, 2000, 3000};
  const int64_t lens[] = {1000, -1, 500, -1, 1000};
  const int keys[] = {72, -1, 60, -1, 64};
  const char* names[] = {"Late", "Kick", "Snare", "Dup", ""};
  bool st = true, ln = true, ky = true, nm = true;
  for (size_t k = 0; k < 5; ++k) {
    st = st && r.regions[k].start == starts[k];
    ln = ln && r.regions[k].length == lens[k];
    ky = ky && r.regions[k].key == keys[k];
    nm = nm && r.regions[k].name == names[k];
  }
  expect(st, "deck audio: regions kept in stored order (no sort, duplicate start kept), start = cue position");
  expect(ln, "deck audio: region lengths matched by cue id");
  expect(ky, "deck audio: explicit keys from the key list (by cue index)");
  expect(nm, "deck audio: region names matched by cue id");
  // No region chunks at all, and a non-RIFF blob.
  const Bytes plain = wav({}, {}, {}, {});
  const sx::WavRegions p = sx::readWavRegions(plain.data(), plain.size());
  expect(p.riff && p.frames == kFrames && p.regions.empty(), "deck audio without cue points: no regions");
  const Bytes junk = {'O', 'g', 'g', 'S', 0, 0, 0, 0};
  expect(!sx::readWavRegions(junk.data(), junk.size()).riff, "non-RIFF audio: not read as regions");
  // Truncated region chunks never read past the end.
  bool safe = true;
  for (size_t n = 0; n < w.size(); n += 7) safe = safe && sx::readWavRegions(w.data(), n).regions.size() <= 5;
  expect(safe, "deck audio: truncated files read safely");
}

void testWriteRegions() {
  const Bytes w = userWav();
  const sx::WavRegions r = sx::readWavRegions(w.data(), w.size());
  // Unchanged regions rebuild to the same regions.
  const Bytes again = sx::writeWavRegions(w, r.regions);
  const sx::WavRegions r2 = sx::readWavRegions(again.data(), again.size());
  expect(r2.frames == kFrames && sameRegions(r.regions, r2.regions), "region chunks rebuilt: same regions read back");
  // Edited regions (new key, moved start, new length, renamed, one more) read back as written.
  std::vector<sx::Region> ed = r.regions;
  ed[1].key = 70, ed[2].start = 2100, ed[3].length = 300, ed[4].name = "Tail";
  sx::Region extra;
  extra.start = 7000, extra.key = -1;
  ed.push_back(extra);
  const Bytes out = sx::writeWavRegions(w, ed);
  const sx::WavRegions r3 = sx::readWavRegions(out.data(), out.size());
  expect(r3.frames == kFrames && sameRegions(ed, r3.regions), "region chunks rebuilt: edited regions read back exactly");
  // The audio and RIFF size stay intact.
  const uint32_t riffSize = static_cast<uint32_t>(out[4]) | static_cast<uint32_t>(out[5]) << 8 | static_cast<uint32_t>(out[6]) << 16 |
                            static_cast<uint32_t>(out[7]) << 24;
  bool audio = riffSize + 8 == out.size();
  const char* d = "data";
  auto at = std::search(out.begin(), out.end(), d, d + 4);
  audio = audio && at != out.end() && std::equal(at, at + 8 + 2 * kFrames, std::search(w.begin(), w.end(), d, d + 4));
  expect(audio, "region chunks rebuilt: audio and RIFF size intact");
  // Non-RIFF input passes through unchanged.
  const Bytes junk = {'O', 'g', 'g', 'S', 1, 2, 3};
  expect(sx::writeWavRegions(junk, ed) == junk, "region chunks: non-RIFF audio unchanged");
}

sx::Model modelFor(const std::vector<sx::Region>& regs) {
  sx::Model m = sx::defaultModel();
  const auto proto = m.decks[0].regions.empty() ? sx::Region{} : m.decks[0].regions[0];
  m.decks[0].regions.clear();
  for (const auto& r : regs) {
    sx::Region x = proto;
    x.start = r.start, x.length = r.length, x.key = r.key, x.name = r.name;
    m.decks[0].regions.push_back(x);
  }
  return m;
}

void testText() {
  const Bytes w = userWav();
  sx::Model m = modelFor(sx::readWavRegions(w.data(), w.size()).regions);
  m.decks[0].regions[1].name = "two words\nand a line";
  sx::Model back;
  expect(sx::readText(sx::writeText(m), back) && back == m, "text state: region keys and names round-trip");
  // Older text: region lines end after the record.
  std::string old;
  {
    const std::string t = sx::writeText(m);
    size_t p = 0;
    while (p < t.size()) {
      size_t e = t.find('\n', p);
      if (e == std::string::npos) e = t.size();
      std::string line = t.substr(p, e - p);
      if (line.rfind("reg ", 0) == 0) {
        size_t cut = 0;
        for (int f = 0; f < 3 + sx::kRegionInts; ++f) cut = line.find(' ', cut + 1);
        line = line.substr(0, cut);
      }
      old += line + "\n";
      p = e + 1;
    }
  }
  sx::Model o;
  bool plain = sx::readText(old, o) && o.decks[0].regions.size() == m.decks[0].regions.size();
  for (const auto& r : o.decks[0].regions) plain = plain && r.key == -1 && r.name.empty();
  for (size_t k = 0; plain && k < o.decks[0].regions.size(); ++k) plain = o.decks[0].regions[k].start == m.decks[0].regions[k].start;
  expect(plain, "text state: older text (no key / name) loads with automatic keys");
}

void testKeyMap() {
  const Bytes w = userWav();
  const sx::Model m = modelFor(sx::readWavRegions(w.data(), w.size()).regions);
  const sx::KeyMap km = sx::buildKeyMap(m, nullptr);
  // Explicit: 72 → r0, 60 → r2, 64 → r4. Automatic: r1 → 61, r3 → 63 (both free).
  auto only = [&](int key, int region) { return km[0][static_cast<size_t>(key)].size() == 1 && km[0][static_cast<size_t>(key)][0] == region; };
  expect(only(72, 0) && only(60, 2) && only(64, 4) && only(61, 1) && only(63, 3), "key map: explicit keys, then base + index");
  expect(km[0][62].empty() && km[0][65].empty(), "key map: keys of explicitly mapped indices stay free");
  expect(sx::regionKey(km, 0, 0) == 72 && sx::regionKey(km, 0, 3) == 63, "key map: region → key lookup");
  // An automatic region whose key is taken has none.
  sx::Model c = m;
  c.decks[0].regions[2].key = 61;  // r2 explicit 61 takes r1's automatic key
  const sx::KeyMap kc = sx::buildKeyMap(c, nullptr);
  expect(sx::regionKey(kc, 0, 1) == -1 && sx::regionKey(kc, 0, 2) == 61, "key map: automatic key taken by an explicit one");
  // Two explicit regions on one key layer.
  c.decks[0].regions[4].key = 72;
  const sx::KeyMap kl = sx::buildKeyMap(c, nullptr);
  expect(kl[0][72].size() == 2, "key map: two regions on one key layer");
  // The slicer's key parameter wins for deck A.
  PluginState s = defaultPluginState("gl.slicer");
  s.params[static_cast<size_t>(pp::SlEngine)] = 1;
  for (int k = 0; k < 5; ++k) s.params[static_cast<size_t>(pp::slParam2(k, pp::SliceKey))] = static_cast<float>(m.decks[0].regions[static_cast<size_t>(k)].key);
  s.params[static_cast<size_t>(pp::slParam2(0, pp::SliceKey))] = 48;
  const sx::KeyMap kp = sx::buildKeyMap(m, &s);
  expect(sx::regionKey(kp, 0, 0) == 48 && kp[0][72].empty(), "key map: the key parameter wins");
  // More than 65 regions: the base drops by octaves.
  sx::Model big = sx::defaultModel();
  big.decks[0].regions.assign(80, sx::Region{});
  for (size_t k = 0; k < 80; ++k) big.decks[0].regions[k].start = static_cast<int64_t>(k) * 10;
  const sx::KeyMap kb = sx::buildKeyMap(big, nullptr);
  expect(sx::regionKey(kb, 0, 0) == 36 && sx::regionKey(kb, 0, 79) == 115, "key map: 80 regions start two octaves down");
}

// Plays one key on a gl.slicer state; returns the left channel.
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

// Which region start the output matches best (normalised correlation over 300
// frames after the declick, lag −2..2), and how loud it is after `after` frames.
int64_t matchStart(const std::vector<float>& out, double& corrOut) {
  int64_t best = -1;
  corrOut = -1;
  for (int64_t s : {0, 1000, 2000, 2100, 3000, 5000, 7000})
    for (int lag = -2; lag <= 2; ++lag) {
      double xy = 0, xx = 0, yy = 0;
      for (int i = 64; i < 364; ++i) {
        const int64_t f = s + i - lag;
        if (f < 0 || f >= kFrames) continue;
        const double x = out[static_cast<size_t>(i)], y = noise(static_cast<uint32_t>(f));
        xy += x * y, xx += x * x, yy += y * y;
      }
      const double c = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
      if (c > corrOut) corrOut = c, best = s;
    }
  return best;
}
double peakFrom(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < b && i < v.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(v[i])));
  return m;
}

// A Slicex plugin state carrying `riff` as deck A's embedded audio.
Bytes slicexState(const Bytes& riff, std::vector<sx::Region> regs) {
  sx::Model m = modelFor(regs);
  for (auto& r : m.decks[0].regions) r.key = -1, r.name.clear();  // the audio holds them
  return sx::writeState(m, {riff, Bytes{}});
}

void testDecodeAndPlay() {
  const enc::EncodeContext ec;
  const Bytes w = userWav();
  const auto regs = sx::readWavRegions(w.data(), w.size()).regions;
  gl::flp::PluginMapping pm;
  const bool ok = enc::decodeState("Slicex", slicexState(w, regs), pm, ec);
  expect(ok && pm.state.type == "gl.slicer", "decode: Slicex → gl.slicer");
  if (!ok) return;
  const PluginState& st = pm.state;
  sx::Model md;
  expect(sx::modelOf(st.data, md) && sameRegions(md.decks[0].regions, regs), "decode: regions as the deck audio stores them (order, keys, names)");
  const int keys[] = {72, -1, 60, -1, 64};
  bool kp = true;
  for (int k = 0; k < 5; ++k) kp = kp && std::lround(st.params[static_cast<size_t>(pp::slParam2(k, pp::SliceKey))]) == keys[k];
  expect(kp, "decode: key parameters carry the explicit keys");
  // Each key plays its own region and stops at its end.
  struct Want {
    int key;
    int64_t start, end;
  };
  const Want want[] = {{72, 5000, 6000}, {61, 0, 2000}, {60, 2000, 2500}, {63, 2000, 3000}, {64, 3000, 4000}};
  for (const Want& wv : want) {
    const std::vector<float> out = play(st, wv.key, 4000);
    double c = 0;
    const int64_t s = matchStart(out, c);
    const size_t len = static_cast<size_t>(wv.end - wv.start);
    const double after = peakFrom(out, len + 300, len + 1000);
    expect(s == wv.start && c > 0.99 && after < 1e-4,
           ("decode + play: key " + std::to_string(wv.key) + " plays region at " + std::to_string(wv.start) + " to " + std::to_string(wv.end)).c_str());
  }
  expect(peakFrom(play(st, 62, 2000), 0, 2000) < 1e-6, "decode + play: a key without a region is silent");
}

void testExport() {
  const enc::EncodeContext ec;
  const Bytes w = userWav();
  const auto regs = sx::readWavRegions(w.data(), w.size()).regions;
  const Bytes orig = slicexState(w, regs);
  gl::flp::PluginMapping pm;
  if (!enc::decodeState("Slicex", orig, pm, ec)) {
    expect(false, "export: decode");
    return;
  }
  // Unchanged: the deck audio travels with its own region chunks.
  enc::Result r = enc::encode(pm.state, "Slicex", &orig, ec);
  gl::flp::PluginMapping back;
  sx::Model mb;
  bool ok = r.hasState && enc::decodeState("Slicex", r.state, back, ec) && sx::modelOf(back.state.data, mb);
  expect(ok && sameRegions(mb.decks[0].regions, regs), "export: unchanged chops round-trip");
  std::array<Bytes, 2> rf;
  sx::Model raw;
  expect(ok && sx::parseState(r.state.data(), r.state.size(), raw, rf) && rf[0] == w, "export: unchanged deck audio written as it was");
  // Edited in Gridline: new key on region 1, region 2 start moved, region 4 ended earlier.
  PluginState g = pm.state;
  sx::Model md;
  sx::modelOf(g.data, md);
  md.decks[0].regions[2].start = 2100;
  g.data = sx::sampleOf(g.data) + "\n" + sx::writeText(md);
  g.params[static_cast<size_t>(pp::slParam2(1, pp::SliceKey))] = 70;
  g.params[static_cast<size_t>(pp::slParam2(4, pp::SliceEnd))] = 3600.0f / kFrames;
  r = enc::encode(g, "Slicex", &orig, ec);
  gl::flp::PluginMapping b2;
  sx::Model m2;
  ok = r.hasState && enc::decodeState("Slicex", r.state, b2, ec) && sx::modelOf(b2.state.data, m2) && m2.decks[0].regions.size() == 5;
  expect(ok && m2.decks[0].regions[1].key == 70 && std::lround(b2.state.params[static_cast<size_t>(pp::slParam2(1, pp::SliceKey))]) == 70,
         "export: a new key travels in the deck audio");
  expect(ok && m2.decks[0].regions[2].start == 2100 && m2.decks[0].regions[4].length == 600, "export: moved start and new end travel");
  expect(ok && m2.decks[0].regions[0].key == 72 && m2.decks[0].regions[0].name == "Late" && m2.decks[0].regions[3].start == 2000,
         "export: untouched regions keep their key, name and place");
  // Gridline save / load: the state text carries the chops as they play.
  sx::Model t;
  expect(sx::readText(sx::writeText(m2), t) && t == m2, "save / load: chops kept");
  // A play check after the round trip: key 70 plays region 1 (start 0).
  double c = 0;
  expect(matchStart(play(b2.state, 70, 1000), c) == 0 && c > 0.99, "export round trip: key 70 plays the region it was given");
}

}  // namespace

int main() {
  std::filesystem::create_directories("j166");
  gl::platform::setEnv("GRIDLINE_CACHE_DIR", std::filesystem::absolute("j166/cache").string().c_str());
  testReadRegions();
  testWriteRegions();
  testText();
  testKeyMap();
  testDecodeAndPlay();
  testExport();
  return finish();
}
