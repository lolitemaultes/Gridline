// Gridline — FLP export tests: Fx2Tone plugin encoders (FlpEncFx2Tone.cpp).
#pragma once
#include "enc_Fx2Space.h"  // fxdCheck / fxdEdited
#include "enc_common.h"

namespace enctest {

inline void testEncFx2Tone() {
  using gl::flp::putI32;
  using gl::flp::putF32;
  using gl::flp::putU8;
  const auto tag = [](size_t t) { return 4 + 4 * t; };

  fxdCheck("Fruity Blood Overdrive", "gl.overdrive", "Blood Overdrive", [&](Bytes& b) {
    putI32(b, tag(0), 2500), putI32(b, tag(2), 7000), putI32(b, tag(3), 1), putI32(b, tag(5), 4000);
  });
  fxdCheck("Fruity Fast LP", "gl.fastlp", "Fast LP", [&](Bytes& b) { putI32(b, tag(0), 600), putI32(b, tag(1), 300); });
  fxdCheck("Fruity Bass Boost", "gl.bassboost", "Bass Boost", [&](Bytes& b) { putI32(b, tag(0), 200), putI32(b, tag(1), 1000); });
  fxdCheck("Fruity Squeeze", "gl.squeeze", "Squeeze", [&](Bytes& b) {
    putI32(b, tag(0), 140), putI32(b, tag(2), 900), putI32(b, tag(3), 1), putI32(b, tag(4), 3), putI32(b, tag(8), 2),
        putI32(b, tag(10), 2000), putI32(b, tag(11), 40000);
  });
  // Parametric EQ (v1): no version word.
  fxdCheck("Fruity Parametric EQ", "gl.eq", "Parametric EQ", [](Bytes& b) {
    putI32(b, 4 * 2, 900), putI32(b, 4 * 9, 30000), putI32(b, 4 * 16, 9000), putI32(b, 4 * 23, 4), putI32(b, 4 * 28, 200);
  });
  // EQUO: band modes, bank curve values (f32), global words and option bytes.
  fxdCheck("EQUO", "gl.bankeq", "EQUO", [](Bytes& b) {
    putI32(b, 4 + (5 * 3 + 1) * 8 + 4, 2);                        // band 5, curve 1: mode 2
    putF32(b, 748 + ((2 * 31 + 7) * 3 + 0) * 4, 1.625f);          // bank 2, band 7, curve 0
    putF32(b, 748 + ((8 * 31 + 30) * 3 + 2) * 4, 0.25f);          // master, band 30, curve 2
    putI32(b, 4096, -2400), putI32(b, 4096 + 8, 3500), putI32(b, 4096 + 16, 200), putI32(b, 4096 + 24, 400);
    putU8(b, 4096 + 41, 1);                                       // add mode
  });
  // Frequency Splitter: two bands, slopes, levels and mutes (word 20).
  fxdCheck("Frequency Splitter", "gl.splitter", "Frequency Splitter", [&](Bytes& b) {
    putI32(b, tag(5), 0), putI32(b, tag(0), 30000), putI32(b, tag(2), 5), putI32(b, tag(11), -600), putI32(b, tag(13), 900);
    putI32(b, tag(10), 300), putI32(b, tag(20), (1 << 5) | (1 << 0));
  });
  // Native Splitter with three bands and mutes changed: write() sets the mute word.
  {
    gl::PluginState g;
    if (fxdEdited("Frequency Splitter", "gl.splitter", [&](Bytes& b) { putI32(b, tag(5), 1), putI32(b, tag(20), (1 << 4) | (1 << 2)); }, g))
      expect(nativeRoundTrip("Frequency Splitter", g), "Frequency Splitter: three-band mutes encode back exactly");
    else
      expect(false, "Frequency Splitter: three-band state decodes");
  }
  fxdCheck("Emphasis", "gl.emphasis", "Emphasis", [&](Bytes& b) {
    putI32(b, tag(1), 1), putI32(b, tag(4), 2), putI32(b, tag(5), 6000), putI32(b, tag(8), 4000), putI32(b, tag(10), 1);
  });
  fxdCheck("Emphasizer", "gl.enhance", "Emphasizer", [&](Bytes& b) { putI32(b, tag(1), 8200); });
  fxdCheck("Low Lifter", "gl.lowlift", "Low Lifter", [&](Bytes& b) {
    putI32(b, tag(2), 8000), putI32(b, tag(5), 1), putI32(b, tag(10), 2500), putI32(b, tag(12), 1), putI32(b, tag(14), 0);
  });

  // Distructor / Hardcore: no state is built (the decoders read none); FL loads its defaults.
  {
    gl::PluginState rack = gl::defaultPluginState("gl.amprack");
    const enc::Encoder* e = enc::nativeEncoder(rack);
    expect(e && e->flName == "Distructor", "Amp Rack exports as Distructor");
    enc::Result r = enc::encode(rack, "", nullptr, ctx());
    expect(r.flName == "Distructor" && r.quality == enc::Result::Defaults && !r.hasState, "Amp Rack: written without a state (FL defaults)");
    if (flInstalled())
      for (const char* n : {"Distructor", "Hardcore"}) {
        Stats s = corpusRoundTrip(n);
        expect(s.decoded > 0 && s.patchExact == s.decoded, (std::string(n) + ": unchanged states are written verbatim").c_str());
      }
  }
}

}  // namespace enctest
