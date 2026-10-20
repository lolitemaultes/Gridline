// Gridline — FLP export tests: FxTime plugin encoders (FlpEncFxTime.cpp).
#pragma once
#include <functional>
#include <string>
#include "enc_common.h"

namespace enctest {

// The encoder's defaults with some stored values changed, decoded.
inline bool fxbEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// defaults decode; an edited state round-trips; the shipped corpus encodes
// exactly (`nativeOk`: achievable native count, -1 = all) and verbatim.
inline void fxbPlugin(const char* flName, const char* glType, const std::function<void(Bytes&)>& edit, int nativeMissing = 0) {
  const std::string n = flName;
  expect(defaultsDecode(flName, glType), (n + ": defaults decode").c_str());
  gl::PluginState g;
  const bool ok = fxbEdited(flName, glType, edit, g);
  expect(ok, (n + ": edited state decodes").c_str());
  expect(ok && nativeRoundTrip(flName, g), (n + ": edited state encodes back exactly").c_str());
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName);
  expect(s.decoded > 0 && s.nativeExact == s.decoded - nativeMissing, (n + ": shipped states encode exactly").c_str());
  expect(s.patchExact == s.decoded, (n + ": unchanged states are written verbatim").c_str());
}

inline void testEncFxTime() {
  using gl::flp::putI32;
  using gl::flp::putU8;
  fxbPlugin("Fruity Reeverb 2", "gl.reverb", [](Bytes& b) {
    putI32(b, 4 + 4 * 0, 120);   // low cut
    putI32(b, 4 + 4 * 5, 60);    // decay 6 s
    putI32(b, 4 + 4 * 9, -20);   // separation
    putI32(b, 4 + 4 * 13, 1500); // mod speed
    putI32(b, 4 + 4 * 14, 300);  // mod depth
    putU8(b, 64, 1);             // tempo pre-delay
  });
  fxbPlugin("Fruity Delay 3", "gl.delay", [](Bytes& b) {
    putI32(b, 4 + 4 * 2, 0);     // free time
    putI32(b, 4 + 4 * 4, 300);   // time
    putI32(b, 4 + 4 * 14, 4500); // feedback
    putI32(b, 4 + 4 * 15, 2);    // filter mode
    putI32(b, 4 + 4 * 16, 2000); // cutoff
    putI32(b, 4 + 4 * 24, -1000);  // tone
  });
  fxbPlugin("Fruity Delay 2", "gl.delay", [](Bytes& b) {
    putI32(b, 0, -40);   // input pan
    putI32(b, 4 * 3, 90);  // feedback
    putI32(b, 4 * 4, 96);  // time: 1/2 beat (synced division)
    putI32(b, 4 * 6, 2);   // ping-pong
    putI32(b, 4 * 7, 70);  // cutoff
  });
  fxbPlugin("Fruity Delay 2", "gl.delay", [](Bytes& b) { putI32(b, 4 * 4, 101); putI32(b, 4 * 1, 150); });  // odd ticks, hot input
  fxbPlugin("Fruity Delay Bank", "gl.multidelay", [](Bytes& b) {
    const size_t u1 = 4 + 4 * (4 + 16), rec1 = 4 + 132 * 4 + 4 + 32;
    putI32(b, u1 + 4 * 1, 100);    // unit 2 input level
    putI32(b, u1 + 4 * 6, -90);    // unit 2 feedback
    putI32(b, u1 + 4 * 8, 960);    // unit 2 time
    putI32(b, rec1 + 0, 1);        // unit 2 input filter: low pass
    putU8(b, rec1 + 29, 1);        // unit 2 on
    putU8(b, rec1 + 31, 0);        // unit 2 time in ms
    putI32(b, 4 + 132 * 4, 1);     // oversampling
  });
  fxbPlugin("Hyper Chorus", "gl.ensemble", [](Bytes& b) {
    putI32(b, 4 + 4 * 0, 250);
    putI32(b, 4 + 4 * 6, 1);
    putI32(b, 4 + 4 * 7, 9000);
    putI32(b, 4 + 4 * 9, 800);
  });
  fxbPlugin("Fruity Flanger", "gl.flanger", [](Bytes& b) {
    putI32(b, 4 + 4 * 6, 60);    // feedback
    putI32(b, 4 + 4 * 7, 0);     // not inverted
    putI32(b, 4 + 4 * 9, 1000);  // dry
    putI32(b, 4 + 4 * 10, 4000); // wet
  });
  // Native Gridline states pick the FL plugin by model.
  {
    gl::PluginState g;
    if (fxbEdited("Fruity Delay 2", "gl.delay", [](Bytes&) {}, g)) {
      enc::Result r = enc::encode(g, "", nullptr, ctx());
      expect(r.flName == "Fruity Delay 2" && r.quality == enc::Result::Exact, "Delay: a classic-model delay is written as Delay 2");
    }
    if (fxbEdited("Fruity Delay 3", "gl.delay", [](Bytes&) {}, g)) {
      enc::Result r = enc::encode(g, "", nullptr, ctx());
      expect(r.flName == "Fruity Delay 3" && r.quality == enc::Result::Exact, "Delay: a reference-model delay is written as Delay 3");
    }
  }
}

}  // namespace enctest
