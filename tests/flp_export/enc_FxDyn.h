// Gridline — FLP export tests: FxDyn plugin encoders (FlpEncFxDyn.cpp).
#pragma once
#include <functional>
#include "enc_common.h"
#include "core/PluginParams.h"
#include "packs/dynamics/dynamics.h"

namespace enctest {

// The encoder's defaults with some stored values changed, decoded.
inline bool fxaDynEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

inline void fxaDynCorpus(const char* flName, const std::string& what) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName);
  expect(s.decoded > 0 && s.nativeExact == s.decoded, (what + ": every shipped state encodes exactly").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

inline void testEncFxDyn() {
  using gl::flp::putI32;
  namespace dy = gl::packs::dynamics;

  // Limiter: 18 knobs, spare state, view, sidechain, safety flag.
  {
    expect(defaultsDecode("Fruity Limiter", "gl.limiter"), "Limiter: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Fruity Limiter", "gl.limiter", [](Bytes& b) {
      putI32(b, 4 + 4 * 0, 1400);   // gain
      putI32(b, 4 + 4 * 2, 800);    // ceiling
      putI32(b, 4 + 4 * 8, 400);    // compressor threshold
      putI32(b, 4 + 4 * 9, 600);    // ratio
      putI32(b, 4 + 4 * 16, 200);   // gate threshold
      putI32(b, 152, 1);            // view: compressor
      gl::flp::putU8(b, 168, 1);    // 0.2 dB safety margin
    }, g);
    expect(ok && nativeRoundTrip("Fruity Limiter", g), "Limiter: a compressing, gated state encodes back exactly");
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Fruity Limiter" && r.quality == enc::Result::Exact, "Limiter: native limiters export as Fruity Limiter");
  }
  fxaDynCorpus("Fruity Limiter", "Limiter");

  // Compressor: 6 knobs; the type word carries knee, character and fast release.
  {
    expect(defaultsDecode("Fruity Compressor", "gl.comp"), "Compressor: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Fruity Compressor", "gl.comp", [](Bytes& b) {
      putI32(b, 4, -240), putI32(b, 8, 40), putI32(b, 12, 35), putI32(b, 16, 25), putI32(b, 20, 80), putI32(b, 24, 6), putI32(b, 32, 1);
    }, g);
    expect(ok && g.params[gl::pp::CmpCharacter] == 1 && g.params[gl::pp::CmpFastRelease] == 1, "Compressor: vintage fast type decodes");
    expect(ok && nativeRoundTrip("Fruity Compressor", g), "Compressor: a vintage fast-release state encodes back exactly");
  }
  fxaDynCorpus("Fruity Compressor", "Compressor");

  // Multiband Compressor: 28 knobs.
  {
    expect(defaultsDecode("Fruity Multiband Compressor", "gl.multiband"), "Multiband: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Fruity Multiband Compressor", "gl.multiband", [](Bytes& b) {
      putI32(b, 4 + 4 * 0, 40000), putI32(b, 4 + 4 * 1, 1), putI32(b, 4 + 4 * 3, 1), putI32(b, 4 + 4 * 6, 30000);
      putI32(b, 4 + 4 * 13, 3000), putI32(b, 4 + 4 * 24, 50000), putI32(b, 4 + 4 * 26, 2500);
    }, g);
    expect(ok && nativeRoundTrip("Fruity Multiband Compressor", g), "Multiband: edited bands encode back exactly");
  }
  fxaDynCorpus("Fruity Multiband Compressor", "Multiband");

  // Maximus: globals, band words, saturation sign, drawn curves (records resized).
  {
    expect(defaultsDecode("Maximus", "gl.master"), "Maximus: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Maximus", "gl.master", [](Bytes& b) {
      putI32(b, 4, 25000);                  // low band frequency
      putI32(b, 4 + 4 * 6, -1);             // low cut: off
      const size_t mid = 32 + 52;           // mid band words
      putI32(b, mid + 4 * 1, 1400);         // pre gain
      putI32(b, mid + 4 * 4, -300);         // saturation mode A
      putI32(b, mid + 4 * 5, 1500);         // saturation ceiling
      putI32(b, 32 + 52 * 2 + 4 * 4, 650);  // high band: saturation mode B
      putI32(b, 32 + 52 * 3 + 4 * 7, 3000); // master: release 2
    }, g);
    expect(ok && g.params[dy::msExt(1, dy::MxSatMode)] == 1 && g.params[dy::msExt(2, dy::MxSatMode)] == 2, "Maximus: saturation modes decode");
    // Drawn curves: 5 points on the low band, 2 on the master band.
    const float lowCurve[5][3] = {{0, 0, 0}, {0.25f, 0.3f, 0}, {0.5f, 0.45f, 0.2f}, {0.75f, 0.5f, -0.4f}, {1, 0.55f, 0}};
    g.params[dy::msExt(0, dy::MxPoints)] = 5;
    for (int i = 0; i < 5; ++i)
      for (int c = 0; c < 3; ++c) g.params[dy::msExt(0, dy::MxPt0 + 3 * i + c)] = lowCurve[i][c];
    g.params[dy::msExt(3, dy::MxPoints)] = 2;
    g.params[dy::msExt(3, dy::MxPt0 + 3)] = 1, g.params[dy::msExt(3, dy::MxPt0 + 4)] = 0.7f, g.params[dy::msExt(3, dy::MxPt0 + 5)] = 0;
    expect(ok && nativeRoundTrip("Maximus", g), "Maximus: edited bands and drawn curves encode back exactly");
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Maximus" && r.quality == enc::Result::Exact && r.state.size() == 1375 + 2 * 24 - 24,
           "Maximus: native mastering chains export as Maximus with resized curve records");
  }
  fxaDynCorpus("Maximus", "Maximus");

  // Soundgoodizer: mode, amount (the program follows the mode).
  {
    expect(defaultsDecode("Soundgoodizer", "gl.enhance"), "Soundgoodizer: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Soundgoodizer", "gl.enhance", [](Bytes& b) { putI32(b, 4, 2), putI32(b, 8, 350); }, g);
    expect(ok && nativeRoundTrip("Soundgoodizer", g), "Soundgoodizer: mode C at 35 % encodes back exactly");
  }
  fxaDynCorpus("Soundgoodizer", "Soundgoodizer");

  // Transient Processor: 9 knobs.
  {
    expect(defaultsDecode("Transient Processor", "gl.transient"), "Transient Processor: defaults decode");
    gl::PluginState g;
    bool ok = fxaDynEdited("Transient Processor", "gl.transient", [](Bytes& b) {
      putI32(b, 4, 140), putI32(b, 8, 30), putI32(b, 12, 20), putI32(b, 16, 100), putI32(b, 20, 2), putI32(b, 24, 0), putI32(b, 28, 120),
          putI32(b, 32, 40);
    }, g);
    expect(ok && nativeRoundTrip("Transient Processor", g), "Transient Processor: a punchy, split state encodes back exactly");
  }
  fxaDynCorpus("Transient Processor", "Transient Processor");
}

}  // namespace enctest
