// K-37 (K-AD): the newer state layouts FL 2026 writes for Kepler Exo and Transistor Bass, and
// Fruity Slicer 2's whole-instrument knobs. Built byte by byte here; no FL data in this test.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/packs/synths/dco.h"
#include "../../src/packs/models/strings.h"
#include "../Check.h"

using namespace gl;

namespace {

struct W {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
  void u16(uint32_t v) { u8(v), u8(v >> 8); }
  void u32(uint32_t v) { u16(v), u16(v >> 16); }
  void f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
  }
  void f64(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    u32(static_cast<uint32_t>(u)), u32(static_cast<uint32_t>(u >> 32));
  }
  void zeros(size_t n) { b.insert(b.end(), n, uint8_t{0}); }
};

std::vector<uint8_t> exo(uint32_t ver, uint32_t count, float tagValue, int optionBytes, uint8_t option = 0) {
  const std::string name = "Kepler-2";
  W w;
  w.u32(1), w.u32(1), w.u32(0), w.u32(0), w.u32(0), w.u32(0), w.u32(0);
  w.u32(static_cast<uint32_t>(name.size() + 1));
  for (char c : name) w.u8(static_cast<uint8_t>(c));
  w.u8(0);
  w.u32(ver), w.u32(count);
  for (uint32_t t = 0; t < count; ++t) w.f32(tagValue);
  w.u32(1), w.u32(8);
  for (int r = 0; r < 8; ++r) w.u32(0xffffffffu), w.u32(0xffffffffu);
  for (int k = 0; k < optionBytes; ++k) w.u8(k == 0 ? option : 0);
  w.u32(2), w.u32(56), w.u32(0);
  w.zeros(56);
  return w.b;
}

std::vector<uint8_t> bass(uint32_t headVersion, uint16_t pre, bool trailer2, float firstValue) {
  W w;
  w.u32(headVersion), w.u32(1), w.u32(8), w.u32(0), w.u32(0), w.u32(3), w.u32(2), w.u32(0), w.u32(0), w.u32(0), w.u32(0), w.u32(0);
  w.u16(headVersion == 2 ? 5 : 3);
  w.u32(60), w.u32(60), w.u8(1);
  auto name = [&] { w.u32(1), w.u8(0); };
  for (int k = 0; k < 128; ++k) {
    w.u16(0), w.u8(1);
    name();
    w.u32(static_cast<uint32_t>(k)), w.u32(static_cast<uint32_t>(k)), w.u32(0);
  }
  for (int k = 0; k < 256; ++k) {
    name();
    w.f32(firstValue);
    for (int i = 1; i < 100; ++i) w.f32(0.25f);
  }
  for (int k = 0; k < 128; ++k) {
    w.u16(pre);
    name();
    w.u16(0), w.u32(16);
    for (int st = 0; st < 32; ++st) {
      w.u8(1), w.u8(0), w.u8(0), w.u32(60), w.u8(1), w.f64(0.5), w.f32(0.5f), w.u8(0);
    }
    w.f64(125.0);
    w.zeros(9);
    if (pre >= 2) w.f64(0.5);
  }
  w.u32(0), w.u32(0), w.u32(0), w.u32(0), w.f32(1.f), w.u32(4);
  if (trailer2) w.u32(1);
  const auto n = static_cast<uint32_t>(w.b.size());
  auto put = [&](size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) w.b[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
  };
  put(7 * 4, n - 36), put(10 * 4, n - 48);
  return w.b;
}

bool decode(const char* flName, const std::vector<uint8_t>& st, flp::PluginMapping& m) {
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState(flName, st, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}

}  // namespace

int main() {
  namespace dc = gl::packs::synths::dco;
  // ---------------------------------------------------------- Kepler Exo
  {
    flp::PluginMapping a, b, opt;
    expect(decode("Kepler Exo", exo(2, 151, 0.25f, 0), a), "Kepler Exo version 2 decodes");
    expect(decode("Kepler Exo", exo(4, 155, 0.25f, 3), b), "Kepler Exo version 4 decodes");
    expect(b.exact, "version 4 with default option bytes is exact");
    expect(a.state.params[dc::DcCutoff] == b.state.params[dc::DcCutoff], "both versions read the same tag");
    expect(b.state.params[dc::DcMasterTune] != a.state.params[dc::DcMasterTune], "version 4 reads tags 151..154");
    expect(decode("Kepler Exo", exo(4, 155, 0.25f, 3, 1), opt) && !opt.exact, "unknown option bytes make the import approximate");
    flp::PluginMapping bad;
    expect(!decode("Kepler Exo", exo(5, 155, 0.25f, 3), bad), "an unknown version is refused");
    expect(!decode("Kepler Exo", exo(4, 155, 0.25f, 0), bad), "version 4 without its option bytes is refused");
  }
  // ---------------------------------------------------------- Transistor Bass
  {
    flp::PluginMapping oldS, newS, bad;
    expect(decode("Transistor Bass", bass(1, 0, false, 0.5f), oldS), "bank version 3 decodes");
    expect(decode("Transistor Bass", bass(2, 2, true, 0.5f), newS), "bank version 5 decodes");
    expect(oldS.state.params == newS.state.params && oldS.state.data == newS.state.data, "both layouts give the same result");
    expect(!decode("Transistor Bass", bass(3, 2, true, 0.5f), bad), "an unknown head is refused");
    expect(!decode("Transistor Bass", bass(2, 2, false, 0.5f), newS) || newS.state.params[0] == oldS.state.params[0], "no extra word is fine");
    // The export keeps the layout it was given.
    std::vector<uint8_t> v5 = bass(2, 2, true, 0.5f);
    flp::PluginMapping back;
    flp::enc::EncodeContext c;
    flp::PluginMapping fromV5;
    expect(decode("Transistor Bass", v5, fromV5), "origin decodes");
    PluginState edited = fromV5.state;
    edited.params[0] = 0.75f;
    const flp::enc::Result out = flp::enc::encode(edited, "Transistor Bass", &v5, c);
    expect(out.hasState && out.state.size() == v5.size(), "export keeps the bank layout");
    expect(decode("Transistor Bass", out.state, back) && std::fabs(back.state.params[0] - 0.75f) < 1e-5f, "export writes the value");
  }
  // ---------------------------------------------------------- Fruity Slicer 2
  {
    auto slicer = [](int semi, float cents, float cut, float res) {
      std::vector<uint8_t> b(294, 0);
      std::memcpy(b.data() + 40, "slc1", 4);
      b[89] = static_cast<uint8_t>(static_cast<int8_t>(semi));
      std::memcpy(b.data() + 93, &cents, 4);
      std::memcpy(b.data() + 106, &cut, 4);
      std::memcpy(b.data() + 110, &res, 4);
      return b;
    };
    flp::PluginMapping def, m;
    expect(decode("Fruity Slicer 2", slicer(0, 0.5f, 1.f, 0.f), def) && def.exact, "Slicer 2: untouched knobs stay exact");
    PluginState empty;
    empty.type = "gl.slicer";
    normalizePluginState(empty);
    expect(def.state.params[pp::SlCutoff] == empty.params[pp::SlCutoff] && def.state.params[pp::SlReso] == empty.params[pp::SlReso], "Slicer 2: untouched knobs keep the defaults");
    const std::vector<uint8_t> moved = slicer(12, 0.25f, 0.5f, 0.5f);
    expect(decode("Fruity Slicer 2", moved, m), "Slicer 2 decodes");
    expect(std::fabs(m.state.params[pp::SlPitch] - 12.f) < 1e-3f && std::fabs(m.state.params[pp::SlFine] + 50.f) < 1e-3f, "Slicer 2: transpose and detune");
    expect(std::fabs(m.state.params[pp::SlReso] - 0.5f) < 1e-5f && m.state.params[pp::SlCutoff] < 2000.f, "Slicer 2: cutoff and resonance");
    expect(!m.exact, "Slicer 2: moved knobs make the import approximate");
  }
  return finish();
}
