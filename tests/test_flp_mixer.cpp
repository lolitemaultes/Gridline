// Gridline — FLP import: mixer (inserts, flags, 225 parameter records,
// routing, strip EQ) and effect-state decoders, on small synthetic files and
// hand-made state blobs (no FL Studio content).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/Project.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpFx.h"
#include "../src/engine/StripEq.h"
#include "Check.h"

using namespace gl;
namespace fx = gl::flp::fx;

namespace {
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

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
// One 225 record.
void rec(std::vector<uint8_t>& v, int id, int kind, int insertBase, int insert, int slot, int32_t value) {
  put32(v, 0);
  v.push_back(static_cast<uint8_t>(id));
  v.push_back(static_cast<uint8_t>(kind));
  uint16_t where = static_cast<uint16_t>(((insertBase + insert) << 6) | slot);
  v.push_back(static_cast<uint8_t>(where & 0xFF)), v.push_back(static_cast<uint8_t>(where >> 8));
  put32(v, static_cast<uint32_t>(value));
}
std::vector<uint8_t> flags(uint32_t f) {
  std::vector<uint8_t> v;
  put32(v, 0), put32(v, f), put32(v, 0);
  return v;
}
// One insert: name, flags, optional plugin in slot `fxSlot`, routing bytes.
void insert(Builder& B, const std::string& name, uint32_t fl, const std::vector<uint8_t>& routing, int fxSlot = -1,
            const std::string& fxName = "", const std::vector<uint8_t>& state = {}) {
  if (!name.empty()) B.text(204, name);
  B.data(236, flags(fl));
  for (int k = 0; k < 10; ++k) {
    if (k == fxSlot) {
      B.text(201, fxName);
      B.data(213, state);
    }
    B.w(98, static_cast<uint16_t>(k));
  }
  B.data(235, routing);
  B.d(154, 0xFFFFFFFFu);
  B.d(147, 0xFFFFFFFFu);
}

bool importBytes(const std::vector<uint8_t>& bytes, Song& s, flp::Report& rep) {
  std::string path = "build_flp_mixer_test.flp", err;
  if (FILE* fp = std::fopen(path.c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
  }
  bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  std::remove(path.c_str());
  return ok;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Decodes one effect from a synthetic plugin event list (201 name + 213 state).
[[maybe_unused]] bool decodeFx(const std::string& flName, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  static Builder B;  // keeps the bytes alive for the events
  B.ev.clear();
  B.text(201, flName);
  B.data(213, state);
  static flp::File f;
  std::string err;
  if (!flp::readBytes(B.file(96), f, &err)) return false;
  static Song song;
  static flp::Report rep;
  flp::Context c(f, song, rep);
  auto it = flp::decoders().find(flName);
  if (it == flp::decoders().end()) return false;
  m = flp::PluginMapping{};
  m.flName = flName;
  if (!it->second(c, f.events, m) || m.glType.empty()) return false;
  normalizePluginState(m.state);
  return true;
}
// Little-endian blob builder for effect states.
struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) { return put32(b, static_cast<uint32_t>(v)), *this; }
  Blob& u8(uint8_t v) { return b.push_back(v), *this; }
  Blob& f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return put32(b, u), *this;
  }
};
[[maybe_unused]] double param(const flp::PluginMapping& m, int i) { return i >= 0 && static_cast<size_t>(i) < m.state.params.size() ? m.state.params[static_cast<size_t>(i)] : -1e9; }
}  // namespace

#include "flp_fx_eq.inc"
#include "flp_fx_time.inc"
#include "flp_fx_dyn.inc"
#include "flp_fx_misc.inc"

static void testMixerFl25() {
  // FL 25 layout: master, 3 inserts, "current"; count in event 103; insert base 448.
  Builder B;
  B.text(199, "25.2.3.5164");
  B.w(64, 0), B.b(21, 0), B.text(192, "Kick"), B.w(104, 1);  // channel 0 → insert 1 (FL ≥ 24.2.99 field)
  B.w(103, 5);
  B.b(42, 0);
  insert(B, "", 0x0C, {0});                                                         // master
  insert(B, "Kick", 0x4C | 0x1 | 0x2, {1, 0, 0, 1});                                 // → master + sidechain to 3
  insert(B, "Pad", 0x44 /* track disabled = muted */, {0, 0, 0, 1});                 // → 3 only
  insert(B, "Bus", 0x48 /* effects off */, {1}, 0, "Unknown FX", {1, 2, 3, 4});       // → master
  insert(B, "", 0x4C, {1}, 0, "Unknown FX", {1});                                    // current
  std::vector<uint8_t> p;
  const int base = 448;
  rec(p, 0, 0, 256, 0, 0, 12800);  // header record (ignored)
  rec(p, 192, 31, base, 0, 0, 12800);
  rec(p, 192, 31, base, 1, 0, 10996);
  rec(p, 193, 31, base, 2, 0, -3200);
  rec(p, 194, 31, base, 1, 0, -32);
  rec(p, 209, 31, base, 2, 0, -268);
  rec(p, 217, 31, base, 2, 0, 44848);
  rec(p, 1, 31, base, 3, 0, 6400);
  rec(p, 0, 31, base, 3, 1, 0);
  rec(p, 3, 32, base, 1, 0, 0);       // route 1 → 3 level 0 (sidechain only)
  rec(p, 3, 32, base, 2, 0, 7983);    // route 2 → 3 level
  rec(p, 192, 31, base, 501, 0, 1);   // the "current" track's record
  B.data(225, p);
  Song s;
  flp::Report rep;
  expect(importBytes(B.file(96), s, rep), "mixer: FL 25 file imports");
  expect(s.mixer.size() == 4, "mixer: current track dropped, inserts kept");
  if (s.mixer.size() != 4) return;
  const MixerTrack &m0 = s.mixer[0], &k = s.mixer[1], &pad = s.mixer[2], &bus = s.mixer[3];
  expect(k.name == "Kick" && k.invertPolarity && k.swapLR && !k.muted, "flags: polarity, swap");
  expect(pad.muted && !bus.muted, "flags: track enabled bit = not muted");
  expect(!bus.slots[0].enabled, "flags: effects-off bypasses the slots");
  expect(near(m0.volume, 0.8, 1e-6), "volume: 12800 = unity");
  expect(k.volume < 0.8f && k.volume > 0.5f, "volume: 10996 below unity");
  expect(near(fx::flMixerGain(12800), 1.0, 1e-9) && near(20 * std::log10(fx::flMixerGain(16000)), 5.6, 0.1), "fader curve: 0 dB at 0.8, +5.6 dB at max");
  expect(near(pad.pan, -0.5, 1e-6), "pan: -3200 = half left");
  expect(near(k.stereoSep, 0.5, 1e-6), "stereo separation sign and scale");
  expect(near(pad.eqGain[1], -2.68, 1e-4) && pad.eqFreq[1] > 1000 && pad.eqFreq[1] < 6000, "strip EQ: mid gain and frequency");
  expect(near(bus.slots[0].mix, 0.5, 1e-6), "slot mix");
  // routing
  auto sendTo = [&](const MixerTrack& t, size_t dest) -> const Send* {
    for (const auto& sd : t.sends)
      if (sd.to == s.mixer[dest].id) return &sd;
    return nullptr;
  };
  const Send* ks = sendTo(k, 3);
  expect(sendTo(k, 0) && ks && ks->sidechain, "routing: master + sidechain-only route");
  const Send* ps = sendTo(pad, 3);
  expect(!sendTo(pad, 0) && ps && !ps->sidechain && near(ps->level, fx::flMixerGain(7983), 1e-6), "routing: send level, no master");
  expect(sendTo(bus, 0) && bus.sends.size() == 1, "routing: bus → master");
  // (channel → insert via event 104 is the channel importer's job; here it only keeps insert 1 in use)
}

static void testMixerOld() {
  // FL ≤ 21 layout: 127 inserts written (here 70 to keep it short, ≥ 64 → last is "current"), base 128,
  // route levels as ids 64 + destination.
  Builder B;
  B.text(199, "20.99.3000.3031");
  B.b(42, 0);
  const int count = 70;
  for (int i = 0; i < count; ++i) {
    if (i == 0) insert(B, "", 0x0C, {0});
    else if (i == 1) insert(B, "Lead", 0x4C, {0, 0, 1});
    else if (i == 2) insert(B, "Verb", 0x4C, {1}, 0, "Unknown FX", {0});
    else insert(B, "", 0x4C, {1});
  }
  std::vector<uint8_t> p;
  const int base = 128;
  for (int i = 0; i < count; ++i) rec(p, 192, 31, base, i, 0, 12800);
  rec(p, 64 + 2, 31, base, 1, 0, 6400);
  rec(p, 64 + 2, 31, base, 3, 0, 0);  // stale level of an unrouted destination
  B.data(225, p);
  Song s;
  flp::Report rep;
  expect(importBytes(B.file(96), s, rep), "mixer: FL 20 file imports");
  expect(s.mixer.size() == 3, "mixer: unused trailing inserts trimmed");
  if (s.mixer.size() != 3) return;
  const MixerTrack& lead = s.mixer[1];
  expect(lead.sends.size() == 1 && lead.sends[0].to == s.mixer[2].id && near(lead.sends[0].level, fx::flMixerGain(6400), 1e-6),
         "old route level ids (64 + destination)");
  expect(s.mixer[2].sends.size() == 1 && s.mixer[2].sends[0].to == s.mixer[0].id, "old: default master route");
}

static void testStripEqDsp() {
  StripEqSettings st;
  st.gain = {6.0f, -9.0f, 4.0f};
  st.freq = {100.0f, 1000.0f, 8000.0f};
  st.q = {0.7f, 1.5f, 0.7f};
  const double sr = 48000;
  double low = 20 * std::log10(stripEqMagnitude(st, 30, sr));
  double mid = 20 * std::log10(stripEqMagnitude(st, 1000, sr));
  double high = 20 * std::log10(stripEqMagnitude(st, 18000, sr));
  expect(near(low, 6.0, 0.6), "strip EQ: low shelf boost");
  expect(near(mid, -9.0, 0.3), "strip EQ: mid peak cut at its frequency");
  expect(near(high, 4.0, 0.6), "strip EQ: high shelf boost");
  // Processing matches the magnitude response (sine at 1 kHz).
  StripEqState state;
  std::vector<float> L(48000), R(48000);
  for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(std::sin(2 * 3.141592653589793 * 1000.0 * static_cast<double>(i) / sr));
  for (size_t at = 0; at < L.size(); at += 256)
    state.process(st, sr, L.data() + at, R.data() + at, static_cast<int>(std::min<size_t>(256, L.size() - at)));
  float pk = 0;
  for (size_t i = 24000; i < L.size(); ++i) pk = std::max(pk, std::fabs(L[i]));
  expect(near(20 * std::log10(pk), mid, 0.3), "strip EQ: processing matches response");
  // Flat settings pass audio untouched.
  StripEqSettings flat;
  flat.freq = {90, 1500, 8000};
  flat.q = {0.7f, 0.7f, 0.7f};
  float x[4] = {0.5f, -0.25f, 1.0f, 0.0f}, y[4] = {0.5f, -0.25f, 1.0f, 0.0f};
  state.process(flat, sr, x, y, 4);
  expect(x[0] == 0.5f && x[2] == 1.0f && y[1] == -0.25f, "strip EQ: flat = bypass");
}

static void testStripEqProject() {
  Song s = makeEmptySong();
  s.mixer[1].eqGain = {3.0f, -2.5f, 0.0f};
  s.mixer[1].eqFreq[1] = 2200.0f;
  s.mixer[1].eqQ[1] = 1.4f;
  std::string text = projectToText(s);
  Song back;
  std::string err;
  Json j;
  bool ok = jsonParse(text, j) && songFromJson(j, back, &err);
  expect(ok && back.mixer[1].eqGain == s.mixer[1].eqGain && back.mixer[1].eqFreq == s.mixer[1].eqFreq && back.mixer[1].eqQ == s.mixer[1].eqQ,
         "strip EQ survives save/load");
  expect(ok && back == s, "project with strip EQ round-trips");
  expect(projectToText(makeEmptySong()).find("\"eq\"") == std::string::npos, "default strip EQ is not written");
}

int main() {
  testStripEqProject();
  testMixerFl25();
  testMixerOld();
  testStripEqDsp();
  testFxEq();
  testFxTime();
  testFxDyn();
  testFxMisc();
  return finish();
}
