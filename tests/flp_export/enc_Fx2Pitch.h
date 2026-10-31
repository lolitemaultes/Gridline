// Gridline — FLP export tests: Fx2Pitch plugin encoders (FlpEncFx2Pitch.cpp).
#pragma once
#include "enc_Fx2Mod.h"
#include "packs/spectral/spectral.h"

namespace enctest {

inline void testEncFx2Pitch() {
  using gl::flp::putF32;
  using gl::flp::putI32;
  using gl::flp::putU8;
  encCase("Pitcher", "Pitcher", "gl.pitcher", [](Bytes& b) {
    putF32(b, 12 + 4 * 0, 0.3f);    // correction speed
    putF32(b, 12 + 4 * 2, 0.75f);   // gender
    putF32(b, 12 + 4 * 7, 1.0f);    // key D bypassed
    putF32(b, 12 + 4 * 18, 0.25f);  // voice 2 pan
    putF32(b, 12 + 4 * 23, 0.6f);   // A4 reference
    putF32(b, 12 + 4 * 28, 9.0f);   // root A
    putF32(b, 12 + 4 * 32, 4.0f);   // lowest range
    putU8(b, 12 + 4 * 33 + 1, 0);   // C# out of the scale
    putF32(b, 12 + 4 * 33 + 12, 3.0f / 256);  // MIDI port 2
  });
  encCase("Pitch Shifter", "Pitch Shifter", "gl.pitch", [](Bytes& b) {
    putI32(b, 4, -700);    // pitch
    putI32(b, 16, 1200);   // mix
    putI32(b, 36, 300);    // delay
    putI32(b, 40, 40);     // feedback
    putU8(b, 60, 1);       // tempo sync
    putI32(b, 61, 0);      // voice engine
  });
  encCase("Frequency Shifter", "Frequency Shifter", "gl.freqshift", [](Bytes& b) {
    putI32(b, 4, 6000);     // mix
    putI32(b, 8, 2);        // ring
    putI32(b, 12, -12345);  // frequency
    putI32(b, 28, 7000);    // feedback
    putI32(b, 36, 2);       // tempo range
  });
  encCase("Vocoder", "Fruity Vocoder", "gl.vocoder", [](Bytes& b) {
    // 32 bands: the level table grows with the count
    Bytes knobs(b.end() - 48, b.end());
    b.resize(17);
    putI32(b, 4, 32);
    for (int k = 0; k < 32; ++k) {
      gl::flp::ByteWriter w;
      w.f32(1.0f);
      b.insert(b.end(), w.b.begin(), w.b.end());
    }
    b.insert(b.end(), knobs.begin(), knobs.end());
  });
  // J-205: the Turntable instrument; a changed speed and sound switch encode back.
  encCase("Scratcher", "Fruity Scratcher", "gl.turntable", [](Bytes& b) {
    gl::flp::putF32(b, 12, 0.75f);
    gl::flp::putF32(b, 8, 0.0f);
  });
}

}  // namespace enctest
