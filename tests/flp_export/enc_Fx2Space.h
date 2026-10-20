// Gridline — FLP export tests: Fx2Space plugin encoders (FlpEncFx2Space.cpp).
#pragma once
#include <functional>
#include "enc_common.h"
#include "core/PluginParams.h"
#include "packs/spacefx/spacefx.h"

namespace enctest {

// The encoder's defaults with some stored values changed, decoded: a
// non-default Gridline state every stored value of which is representable.
inline bool fxdEdited(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// Defaults decode; the edited defaults encode back exactly; every shipped
// state encodes exactly from the defaults and unchanged ones verbatim.
inline void fxdCheck(const char* flName, const char* glType, const std::string& what, const std::function<void(Bytes&)>& edit) {
  expect(defaultsDecode(flName, glType), (what + ": defaults decode").c_str());
  gl::PluginState g;
  const bool ok = fxdEdited(flName, glType, edit, g);
  expect(ok, (what + ": edited state decodes").c_str());
  expect(ok && nativeRoundTrip(flName, g), (what + ": an edited state encodes back exactly").c_str());
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(flName);
  expect(s.decoded > 0 && s.nativeExact == s.decoded, (what + ": every shipped state encodes exactly").c_str());
  expect(s.patchExact == s.decoded, (what + ": unchanged states are written verbatim").c_str());
}

inline void testEncFx2Space() {
  using gl::flp::putI32;
  using gl::flp::putU8;
  namespace sx = gl::packs::spacefx;
  const auto tag = [](size_t t) { return 4 + 4 * t; };

  // LuxeVerb v9: ints from offset 13, header sync bytes 8 / 9, feed-forward sync bytes after the tags.
  fxdCheck("LuxeVerb", "gl.hall", "LuxeVerb", [](Bytes& b) {
    putI32(b, 13 + 4 * 4, 15000);   // decay
    putI32(b, 13 + 4 * 13, 2);      // freeze: sustain
    putI32(b, 13 + 4 * 16, -60);    // feedback pitch
    putI32(b, 13 + 4 * 35, -900);   // EQ gain
    putI32(b, 13 + 4 * 41, -12);    // feed-forward pitch 1
    putU8(b, 8, 1);                 // pre-delay synced
    putU8(b, 13 + 49 * 4 + 1, 1);   // feed-forward 2 synced
  });

  // Convolver: knobs, normalize flag, pre-delay / stretch / EQ mix words after the envelopes.
  fxdCheck("Fruity Convolver", "gl.convolver", "Convolver", [](Bytes& b) {
    putI32(b, 4, 600), putI32(b, 8, 900), putI32(b, 12, -300), putI32(b, 16, 400);
    const size_t n = b.size();
    putI32(b, n - 16, 250);  // pre-delay
    putI32(b, n - 8, -500);  // stretch
    putI32(b, n - 4, 700);   // EQ mix
    putU8(b, n - 28 + 3, 0);  // normalize off (flag byte 3 before latency + 5 ints)
  });
  // Convolver data: impulse path, IR amplitude envelope and EQ curve.
  {
    const enc::Encoder* e = enc::findEncoder("Fruity Convolver", "gl.convolver");
    gl::flp::PluginMapping m;
    bool ok = e && e->defaults && enc::decodeState("Fruity Convolver", e->defaults(), m, ctx());
    expect(ok, "Convolver: defaults decode (data)");
    if (ok) {
      gl::PluginState g = m.state;
      sx::ConvData cd = sx::parseConvData(g.data);
      cd.env = {{0, 0.5, 0}, {0.75, 0.8, 0.25}, {3.5, 0.1, -0.5}};
      cd.eq = {{0, 0.5, 0}, {0.4, 0.7, 0}, {0.8, 0.3, 0.3}, {1, 0.5, 0}};
      g.data = sx::formatConvData(cd);
      g.params[sx::CvEnvOn] = 1;
      expect(nativeRoundTrip("Fruity Convolver", g), "Convolver: an IR envelope and EQ curve encode back exactly");
      // Another impulse file (an absolute path outside FL's data is written as is).
      cd.path = "/tmp/gridline-test-impulse-that-does-not-exist.wav";
      g.data = sx::formatConvData(cd);
      enc::Result r = enc::encode(g, "Fruity Convolver", nullptr, ctx());
      gl::flp::PluginMapping back;
      expect(r.hasState && enc::decodeState("Fruity Convolver", r.state, back, ctx()) &&
                 sx::parseConvData(back.state.data).env.size() == 3 && sx::parseConvData(back.state.data).eq.size() == 4,
             "Convolver: a new impulse path keeps the envelopes");
    }
  }

  // Reeverb: version 1, 10 tags.
  fxdCheck("Fruity Reeverb", "gl.reverb", "Reeverb", [&](Bytes& b) {
    putI32(b, tag(0), 3000), putI32(b, tag(2), 40), putI32(b, tag(3), 50000), putI32(b, tag(5), 33), putI32(b, tag(9), 30000);
  });
  // Delay: input, feedback, cutoff, steps, mode.
  fxdCheck("Fruity Delay", "gl.delay", "Delay", [&](Bytes& b) {
    putI32(b, tag(0), 700), putI32(b, tag(1), 300), putI32(b, tag(2), 600), putI32(b, tag(4), 8), putI32(b, tag(5), 1024);
  });
  // Phaser.
  fxdCheck("Fruity Phaser", "gl.phaser", "Phaser", [&](Bytes& b) {
    putI32(b, tag(0), 1200), putI32(b, tag(2), 950), putI32(b, tag(5), 15), putI32(b, tag(6), 800), putI32(b, tag(8), 3000);
  });
  // Vintage Chorus: mode I+II (link), speed, sine LFO, sync.
  fxdCheck("Vintage Chorus", "gl.bbdchorus", "Vintage Chorus", [&](Bytes& b) {
    putI32(b, tag(1), 1), putI32(b, tag(2), 120000), putI32(b, tag(3), 1), putI32(b, tag(7), 5000), putI32(b, tag(15), 1);
  });
  // Vintage Phaser v3: sweep, rate range, delay, drive, inverted feedback.
  fxdCheck("Vintage Phaser", "gl.vphaser", "Vintage Phaser", [&](Bytes& b) {
    putI32(b, tag(0), 300000), putI32(b, tag(6), 100000), putI32(b, tag(12), 1), putI32(b, tag(16), 300);
    putI32(b, tag(29), 1000), putI32(b, tag(30), 90000), putI32(b, tag(34), 400000), putI32(b, tag(35), 1);
  });

  // Native export picks these plugins.
  {
    // A native plate-model reverb (Reeverb 2 cannot hold the plate model).
    gl::PluginState rv;
    expect(fxdEdited("Fruity Reeverb", "gl.reverb", [](Bytes& b) { gl::flp::putI32(b, 4 + 4 * 3, 40000); }, rv), "Reeverb: plate state decodes");
    enc::Result r = enc::encode(rv, "", nullptr, ctx());
    expect(r.flName == "Fruity Reeverb" && r.quality == enc::Result::Exact, "Reverb (plate model) exports as Fruity Reeverb");
    const enc::Encoder* h = enc::nativeEncoder(gl::defaultPluginState("gl.hall"));
    expect(h && h->flName == "LuxeVerb", "Hall exports as LuxeVerb");
    const enc::Encoder* d = enc::nativeEncoder(gl::defaultPluginState("gl.delay"));
    expect(!d || d->flName != "Fruity Delay", "Delay does not prefer the old Fruity Delay");
  }
}

}  // namespace enctest
