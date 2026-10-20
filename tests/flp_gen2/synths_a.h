// Gridline — second-wave instrument decoder tests: synths_a (included by test_flp_gen2.cpp).
// Fruity DX10 → gl.fm, MiniSynth → gl.synth, Ogun / Autogun → gl.alloy (J-22), BeepMap → gl.imagesynth.
#pragma once
#include <memory>
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Slicing.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/models.h"
#include "Gen2Test.h"

namespace gen2test {

namespace synthsa {

// Renders one second of middle C through a decoded state; returns the peak.
inline float renderPeak(const gl::PluginState& st) {
  auto g = gl::createGenerator(st.type);
  if (!g) return -1;
  gl::ChannelAux aux;
  if (!gl::packs::prepareAux(st, aux)) gl::prepareBuiltinAux(st, aux);
  gl::ProcessContext ctx;
  ctx.sampleRate = 44100;
  float peak = 0;
  std::vector<float> L(256), R(256);
  for (int b = 0; b < 172; ++b) {
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    if (b == 0) g->noteOn(1, 60, 0.8f, 0);
    if (b == 120) g->noteOff(1, 0);
    g->process(st, aux, ctx, L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) {
      if (!std::isfinite(L[static_cast<size_t>(i)])) return -2;
      peak = std::max(peak, std::fabs(L[static_cast<size_t>(i)]));
    }
  }
  return peak;
}

// zlib wrapper around raw bytes: fixed-Huffman compressed (made with zlib level 9 from
// synthetic Ogun-style tags) — see testOgun for the values.
inline std::vector<uint8_t> ogunFixed() {
  return {0x78, 0xda, 0x3b, 0xc1, 0xc0, 0xc0, 0xf0, 0xe5, 0xff, 0xff, 0xff, 0x5c, 0x40, 0x9a, 0x81, 0x11, 0x82, 0x75, 0x80,
          0xb8, 0x43, 0x98, 0x81, 0xe1, 0x0e, 0x2b, 0x50, 0x0e, 0xc8, 0xde, 0xc3, 0xc4, 0xc0, 0xc0, 0x0e, 0x94, 0x62, 0x86,
          0x62, 0x07, 0x20, 0x6e, 0x80, 0x62, 0x10, 0xfb, 0x00, 0x94, 0x66, 0x61, 0xc0, 0x0e, 0x1c, 0xd0, 0xf8, 0x0a, 0x58,
          0xd4, 0x18, 0x11, 0xa1, 0x0f, 0x17, 0x28, 0x49, 0xcc, 0xcc, 0xa1, 0x26, 0x06, 0x00, 0x2e, 0x24, 0x2c, 0xcd};
}
inline std::vector<int32_t> ogunTags() {
  std::vector<int32_t> v = {200, -12, 10, 256, 256, 300, 5000, 1500, 500, 700, 7, 3, 3, 64, 128, 128, 64, 192, 64};
  v.resize(49, 0);
  v[19] = 4, v[25] = 64, v[29] = 32, v[34] = 50, v[39] = 64;
  return v;
}
// The same tags as a zlib stream with one stored (uncompressed) block.
inline std::vector<uint8_t> ogunStored() {
  Bytes raw;
  for (int32_t x : ogunTags()) raw.i32(x);
  Bytes z;
  z.u8(0x78).u8(0x01);  // zlib header (deflate, 32K window, check bits)
  uint16_t n = static_cast<uint16_t>(raw.size());
  z.u8(0x01).u16(n).u16(static_cast<uint16_t>(~n)).raw(raw.b);
  z.u32(0);  // adler (not checked)
  return z.b;
}

inline void testDx10() {
  using namespace gl::pp;
  expect(registered("Fruity DX10"), "dx10: registered");
  Bytes b;
  b.i32(1);  // version
  std::vector<int32_t> tags = {0, 42598, 28901, 32768, 0, 15073, 52429, 3277, 52429, 58982, 0, 29295, 0, 27132,
                               10000, 0, 30000, 52429, 0, 52429, 58982, -2};
  for (int32_t t : tags) b.i32(t);
  Run r;
  decode(r, "Fruity DX10", b.b);
  // J-22: DX10 → FM Duo (gl.fmduo, parameter index = control tag, exact).
  expect(r.ok && r.m.glType == "gl.fmduo", "dx10: decodes to gl.fmduo");
  expect(near(param(r, 21), -2), "dx10: octave (signed tag 21)");
  expect(near(param(r, 0), 0) && near(param(r, 1), 42598.0 / 65536.0, 1e-6), "dx10: attack / decay positions");
  expect(near(param(r, 3), 0.5) && near(param(r, 14), 10000.0 / 65536.0, 1e-6), "dx10: modulator coarse positions");
  expect(near(param(r, 13), 27132.0 / 65536.0, 1e-6), "dx10: LFO rate position");
  expect(r.m.param && r.m.param(13) == 13 && r.m.param(99) == -1, "dx10: automation link (LFO rate)");
  expect(r.m.value && near(r.m.value(21, 1.0), 3), "dx10: automation value (octave)");
  expect(r.m.exact, "dx10: exact");
  expect(renderPeak(r.m.state) > 0.01f, "dx10: decoded patch sounds");
  Run shortRun;
  decode(shortRun, "Fruity DX10", std::vector<uint8_t>(40, 0));
  expect(!shortRun.ok, "dx10: short state rejected");
}

inline void testMiniSynth() {
  using namespace gl::pp;
  expect(registered("MiniSynth"), "minisynth: registered");
  std::vector<float> v(43, 0.f);
  v[0] = 1.f;      // voices mode: mono
  v[1] = 0.5f;     // transpose 0
  v[2] = 0.3f;     // slide
  v[5] = 8 / 9.f;  // oscillator kind 8 (octave spread)
  v[6] = 0.5f;
  v[8] = 0.5f;     // filter frequency
  v[9] = 0.5f;     // peak
  v[16] = 1.f;     // filter env amount +100 %
  v[21] = 0.f, v[22] = 1.f, v[23] = 0.75f, v[24] = 0.5f;  // amp ADSR
  Bytes data;
  data.str("0rev", false);
  for (float f : v) data.f32(f);
  Bytes inner;
  inner.raw({0x99, 'F', 'L', 'S', 0x0d, 0x0a, 0x1a, 0x0a}).str("HEAD", false).u32(6).u32(0x00010001).u16(0);
  inner.str("DATA", false).u32(static_cast<uint32_t>(data.size())).raw(data.b);
  Bytes b;
  b.str("SSLF", false).u32(static_cast<uint32_t>(inner.size())).raw(inner.b);
  Run r;
  // J-33: MiniSynth → Pocket Synth (gl.pocketsynth), parameters 1:1 in reference order.
  decode(r, "MiniSynth", b.b);
  expect(r.ok && r.m.glType == "gl.pocketsynth", "minisynth: decodes to gl.pocketsynth");
  expect(near(param(r, 0), 1) && near(param(r, 2), 0.3), "minisynth: voice mode / slide 1:1");
  expect(near(param(r, 23), 0.75) && near(param(r, 22), 1), "minisynth: amp envelope 1:1");
  expect(near(param(r, 5), 8 / 9.f) && near(param(r, 16), 1), "minisynth: oscillator kind / filter env amount 1:1");
  expect(near(param(r, 8), 0.5), "minisynth: cutoff position 1:1");
  expect(r.m.param && r.m.param(8) == 8, "minisynth: automation link (cutoff)");
  expect(renderPeak(r.m.state) > 0.001f, "minisynth: decoded patch sounds");
  Run bad;
  decode(bad, "MiniSynth", std::vector<uint8_t>(64, 0));
  expect(!bad.ok, "minisynth: state without data chunk rejected");
}

inline void testOgun() {
  using namespace gl::packs::models;
  expect(registered("Ogun") && registered("Autogun"), "ogun: registered (Ogun + Autogun)");
  for (int k = 0; k < 2; ++k) {
    Bytes b;
    b.u32(0x00130003).raw(k == 0 ? ogunFixed() : ogunStored());
    Run r;
    decode(r, k == 0 ? "Ogun" : "Autogun", b.b);
    // J-22: Ogun / Autogun → Alloy (gl.alloy, parameter index = control tag);
    // a tags-only record (never written by the reference) keeps its tags.
    const char* what = k == 0 ? "ogun (fixed Huffman): decodes to gl.alloy" : "autogun (stored block): decodes to gl.alloy";
    expect(r.ok && r.m.glType == "gl.alloy", what);
    if (!r.ok) continue;
    expect(near(param(r, 1), -12) && near(param(r, 2), 10), "ogun: coarse / fine pitch tags");
    expect(near(param(r, 6), 5000) && near(param(r, 7), 1500), "ogun: decay / release tags");
    expect(near(param(r, 12), 3), "ogun: unison order");
    expect(near(param(r, 9), 700), "ogun: randomness / morph");
    expect(near(param(r, 0), 200), "ogun: volume");
    expect(near(param(r, 19), 4) && near(param(r, 34), 50), "ogun: chorus order / reverb room");
    expect(renderPeak(r.m.state) > 0.005f, "ogun: decoded patch sounds");
  }
  Bytes broken;
  broken.u32(0x00130003).raw({0x78, 0xda, 0xff, 0xff, 0x00});
  Run r;
  decode(r, "Ogun", broken.b);
  expect(!r.ok, "ogun: corrupt stream rejected");
}

inline void testBeepMap() {
  using namespace gl::packs::models;
  expect(registered("BeepMap"), "beepmap: registered");
  // 8 × 4 pixel 24-bit bottom-up BMP: a bright diagonal on black.
  const int w = 8, h = 4;
  size_t stride = (w * 3 + 3) & ~3u;
  Bytes bmp;
  bmp.str("BM", false).u32(static_cast<uint32_t>(54 + stride * h)).u32(0).u32(54);
  bmp.u32(40).i32(w).i32(h).u16(1).u16(24).u32(0).u32(static_cast<uint32_t>(stride * h)).u32(0).u32(0).u32(0).u32(0);
  for (int y = 0; y < h; ++y) {
    size_t start = bmp.size();
    for (int x = 0; x < w; ++x) {
      bool on = x / 2 == (h - 1 - y);
      bmp.u8(0).u8(on ? 255 : 0).u8(on ? 255 : 0);  // B, G, R
    }
    bmp.pad(start + stride);
  }
  Bytes b;
  b.i32(0).i32(7200).i32(4096).i32(1).i32(0).i32(1).i32(1).i32(0);  // header, range, pixel, scale linear, blue, grainy, loop, widen
  b.zeros(4 * 8).u32(static_cast<uint32_t>(bmp.size())).raw(bmp.b);
  Run r;
  decode(r, "BeepMap", b.b);
  // J-33: BeepMap → Pixel Tone (gl.pixeltone): raw controls, picture kept as text.
  expect(r.ok && r.m.glType == "gl.pixeltone", "beepmap: decodes to gl.pixeltone");
  expect(near(param(r, 0), 7200) && near(param(r, 1), 4096) && near(param(r, 2), 1), "beepmap: range / length / scale raw");
  expect(near(param(r, 3), 0) && near(param(r, 4), 1) && near(param(r, 5), 1), "beepmap: scale / switches");
  expect(r.m.state.data.rfind("P ", 0) == 0, "beepmap: embedded picture kept");
  expect(renderPeak(r.m.state) > 0.001f, "beepmap: decoded picture sounds");
}

}  // namespace synthsa

inline void testSynthsA() {
  synthsa::testDx10();
  synthsa::testMiniSynth();
  synthsa::testOgun();
  synthsa::testBeepMap();
  expect(gl::findPlugin("gl.additive")->params.size() > static_cast<size_t>(gl::packs::models::AdFine), "additive: appended pitch / fine params");
}

}  // namespace gen2test
