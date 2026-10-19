// Gridline — second-wave instrument decoder tests: synths_a (included by test_flp_gen2.cpp).
// Fruity DX10 → gl.fm, MiniSynth → gl.synth, Ogun / Autogun → gl.additive, BeepMap → gl.imagesynth.
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
  expect(r.ok && r.m.glType == "gl.fm", "dx10: decodes to gl.fm");
  expect(near(param(r, FmAlgorithm), 3), "dx10: carrier + two parallel modulators");
  expect(near(param(r, FmOctave), -2), "dx10: octave (signed tag 21)");
  expect(near(param(r, fmParam(0, FmAttack)), 0.001, 1e-4), "dx10: zero attack");
  expect(near(param(r, fmParam(0, FmSustain)), 0), "dx10: carrier decays (no sustain)");
  // Coarse 0.5 → floor(40.1 * 0.25) = 10, fine 0 → ratio 10.
  expect(near(param(r, fmParam(1, FmRatio)), 10), "dx10: modulator 1 ratio from coarse");
  expect(param(r, fmParam(1, FmLevel)) > 0.2f && param(r, fmParam(2, FmLevel)) > 0.4f, "dx10: modulator depths");
  expect(near(param(r, fmParam(3, FmLevel)), 0), "dx10: op 4 unused");
  expect(near(param(r, FmLfoRate), 25.0 * 0.414 * 0.414, 0.05), "dx10: LFO rate");
  expect(r.m.param && r.m.param(13) == FmLfoRate && r.m.param(99) == -1, "dx10: automation link (LFO rate)");
  expect(r.m.value && near(r.m.value(21, 1.0), 3), "dx10: automation value (octave)");
  expect(!r.m.exact, "dx10: approximate");
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
  Bytes b;
  b.str("SSLF", false).u32(0).raw({0x99, 'F', 'L', 'S', 0x0d, 0x0a, 0x1a, 0x0a}).str("HEAD", false).u32(4).u32(0x00010001);
  b.str("DATA", false).u32(static_cast<uint32_t>(data.size())).raw(data.b);
  Run r;
  decode(r, "MiniSynth", b.b);
  expect(r.ok && r.m.glType == "gl.synth", "minisynth: decodes to gl.synth");
  expect(near(param(r, SynVoices), 1), "minisynth: mono voice mode");
  expect(near(param(r, SynSustain), 0.75), "minisynth: amp sustain");
  expect(near(param(r, SynOsc2Coarse), 12), "minisynth: octave spread oscillator");
  expect(near(param(r, SynFilterEnv), 1), "minisynth: bipolar filter env amount");
  expect(param(r, SynCutoff) > 500 && param(r, SynCutoff) < 1500, "minisynth: cutoff mid position");
  expect(r.m.param && r.m.param(8) == SynCutoff, "minisynth: automation link (cutoff)");
  expect(renderPeak(r.m.state) > 0.01f, "minisynth: decoded patch sounds");
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
    const char* what = k == 0 ? "ogun (fixed Huffman): decodes to gl.additive" : "autogun (stored block): decodes to gl.additive";
    expect(r.ok && r.m.glType == "gl.additive", what);
    if (!r.ok) continue;
    expect(near(param(r, AdPitch), -12), "ogun: coarse pitch");
    expect(near(param(r, AdDecay), 5.0) && near(param(r, AdRelease), 1.5), "ogun: decay / release (ms)");
    expect(near(param(r, AdUniOrder), 3), "ogun: unison order");
    expect(near(param(r, AdScatter), 0.7), "ogun: randomness");
    expect(near(param(r, AdVolume), 200.0 / 256 * 0.8), "ogun: volume");
    expect(param(r, AdFx + FxChorus) > 0.4f && near(param(r, AdFx + FxRoomSize), 0.5), "ogun: chorus / reverb");
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
  expect(r.ok && r.m.glType == "gl.imagesynth", "beepmap: decodes to gl.imagesynth");
  expect(near(param(r, ImSource), SrcCanvas), "beepmap: embedded picture → canvas");
  expect(near(param(r, ImScale), 1) && near(param(r, ImUseBlue), 0) && near(param(r, ImGrainy), 1) && near(param(r, ImLoop), 1), "beepmap: scale / switches");
  expect(near(param(r, ImHigh), 65.0 * 64.0, 1.0), "beepmap: 6-octave frequency range");
  std::string canvas = imageDataCanvas(r.m.state.data);
  auto px = decodeCanvas(canvas);
  expect(!canvas.empty() && px[0] == 15 && px[static_cast<size_t>(31 * kCanvasW + 63)] == 15 && px[static_cast<size_t>(31 * kCanvasW)] == 0,
         "beepmap: picture orientation kept (top row = first column)");
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
