// Gridline — FLP export tests: FxEq plugin encoders (FlpEncFxEq.cpp).
#pragma once
#include <functional>
#include "enc_common.h"

namespace enctest {

// The encoder's defaults with some stored values changed, decoded: a
// non-default Gridline state every stored value of which is representable.
inline bool fxaEqEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

inline void fxaEqCorpus(const char* flName, const std::string& what) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName);
  expect(s.decoded > 0 && s.nativeExact == s.decoded, (what + ": every shipped state encodes exactly").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

inline void testEncFxEq() {
  // Balance: pan / volume words.
  {
    expect(defaultsDecode("Fruity Balance", "gl.utility"), "Balance: defaults decode");
    gl::flp::PluginMapping m;
    gl::flp::ByteWriter w;
    w.i32(-64);
    w.i32(200);
    expect(enc::decodeState("Fruity Balance", w.b, m, ctx()), "Balance: synthetic state decodes");
    expect(nativeRoundTrip("Fruity Balance", m.state), "Balance: a panned, quieter state encodes back exactly");
  }
  if (flInstalled()) {
    Stats s = corpusRoundTrip("Fruity Balance");
    expect(s.decoded > 0 && s.nativeExact == s.decoded, "Balance: every shipped state encodes exactly");
    expect(s.patchExact == s.decoded, "Balance: unchanged states are written verbatim");
  }

  using gl::flp::putI32;
  // Parametric EQ 2: 36 knob words after the version (levels, frequencies,
  // widths, types, orders, main level).
  {
    expect(defaultsDecode("Fruity Parametric EQ 2", "gl.eq"), "EQ 2: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Parametric EQ 2", "gl.eq", [](Bytes& b) {
      putI32(b, 4 + 4 * 0, -1200);   // band 1 level -12 dB
      putI32(b, 4 + 4 * 9, 30000);   // band 3 frequency
      putI32(b, 4 + 4 * 16, 9000);   // band 3 width
      putI32(b, 4 + 4 * 21, 3);      // band 1 type: low cut
      putI32(b, 4 + 4 * 28, -2);     // band 1 order
      putI32(b, 4 + 4 * 35, 450);    // main level +4.5 dB
    }, g);
    expect(ok, "EQ 2: synthetic state decodes");
    expect(ok && nativeRoundTrip("Fruity Parametric EQ 2", g), "EQ 2: an edited band set encodes back exactly");
  }
  fxaEqCorpus("Fruity Parametric EQ 2", "EQ 2");

  // Soft Clipper: threshold, post gain.
  {
    expect(defaultsDecode("Fruity Soft Clipper", "gl.clipper"), "Soft Clipper: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Soft Clipper", "gl.clipper", [](Bytes& b) { putI32(b, 0, 64), putI32(b, 4, 150); }, g);
    expect(ok && nativeRoundTrip("Fruity Soft Clipper", g), "Soft Clipper: a lower threshold, louder output encodes back exactly");
  }
  fxaEqCorpus("Fruity Soft Clipper", "Soft Clipper");

  // Stereo Shaper: matrix gains, delay, dephase, effects position.
  {
    expect(defaultsDecode("Fruity Stereo Shaper", "gl.stereomatrix"), "Stereo Shaper: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Stereo Shaper", "gl.stereomatrix", [](Bytes& b) {
      putI32(b, 4, 6400), putI32(b, 8, 9000), putI32(b, 20, -2048), putI32(b, 24, 1000), putI32(b, 32, 0);
    }, g);
    expect(ok && nativeRoundTrip("Fruity Stereo Shaper", g), "Stereo Shaper: a cross-fed, delayed matrix encodes back exactly");
  }
  fxaEqCorpus("Fruity Stereo Shaper", "Stereo Shaper");

  // Stereo Enhancer: pan, volume, separation, phase offset, position, inversion.
  {
    expect(defaultsDecode("Fruity Stereo Enhancer", "gl.stereo"), "Stereo Enhancer: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Stereo Enhancer", "gl.stereo", [](Bytes& b) {
      putI32(b, 0, 40), putI32(b, 4, 300), putI32(b, 8, -50), putI32(b, 12, 200), putI32(b, 16, 0), putI32(b, 20, 2);
    }, g);
    expect(ok && nativeRoundTrip("Fruity Stereo Enhancer", g), "Stereo Enhancer: a widened, phase-shifted state encodes back exactly");
  }
  fxaEqCorpus("Fruity Stereo Enhancer", "Stereo Enhancer");

  // Free Filter: type, frequency, Q, gain.
  {
    expect(defaultsDecode("Fruity Free Filter", "gl.freefilter"), "Free Filter: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Free Filter", "gl.freefilter", [](Bytes& b) {
      putI32(b, 4, 500), putI32(b, 8, 300), putI32(b, 12, 700), putI32(b, 16, 900);
    }, g);
    expect(ok && nativeRoundTrip("Fruity Free Filter", g), "Free Filter: another type, frequency, Q and gain encode back exactly");
  }
  fxaEqCorpus("Fruity Free Filter", "Free Filter");

  // Filter: cutoff, resonance, three levels, x2.
  {
    expect(defaultsDecode("Fruity Filter", "gl.filter"), "Filter: defaults decode");
    gl::PluginState g;
    bool ok = fxaEqEdited("Fruity Filter", "gl.filter", [](Bytes& b) {
      putI32(b, 4, 600), putI32(b, 8, 800), putI32(b, 12, 100), putI32(b, 16, 900), putI32(b, 20, 0), putI32(b, 24, 1);
    }, g);
    expect(ok && nativeRoundTrip("Fruity Filter", g), "Filter: a resonant band pass with x2 encodes back exactly");
  }
  fxaEqCorpus("Fruity Filter", "Filter");
}

}  // namespace enctest
