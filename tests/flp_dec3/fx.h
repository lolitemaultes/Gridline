// Gridline — third-wave decoder tests: effects and controllers (included by
// test_flp_dec3.cpp).
#pragma once
#include "../../src/core/PluginParams.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../../src/packs/utility/utility.h"
#include "../flp_gen2/Gen2Test.h"

namespace dec3test {

using namespace gen2test;

inline void testFx() {
  using namespace gl;
  using namespace gl::pp;
  namespace mod = gl::packs::modulation;
  namespace dyn = gl::packs::dynamics;
  namespace sfx = gl::packs::spacefx;
  namespace ut = gl::packs::utility;

  // ------------------------------------------------- Peak Controller
  {
    Bytes st;
    st.i32(32768).i32(-128).i32(64).i32(66).i32(0).i32(256).i32(0).i32(32950).i32(4).i32(16384).u8(0).u8(1);
    Run r;
    decode(r, "Fruity Peak Controller", st.b);
    expect(r.ok && r.m.glType == "gl.peakctl", "peak: decodes to the peak follower");
    expect(near(param(r, mod::PkBase), 0.5) && near(param(r, mod::PkAmount), -0.5) && near(param(r, mod::PkTension), 0.5), "peak: base / amount / tension");
    expect(near(param(r, mod::LfAmount), 1.0) && near(param(r, mod::LfShape), 4) && near(param(r, mod::LfPhase), 0.25), "peak: LFO amount, shape, phase");
    expect(near(param(r, mod::PkRamp), 1) && near(param(r, mod::PkMute), 0) && near(param(r, mod::LfSync), 0), "peak: ramp / mute flags, free LFO");
    double d = param(r, mod::PkDecay);
    expect(d > 0.05 && d < 1.0, "peak: default decay is a fraction of a second");
    expect(r.m.param && r.m.param(1) == mod::PkAmount && r.m.param(20) == -1, "peak: automation targets by tag");
    expect(r.m.value && near(r.m.value(1, 0.0), -1.0) && near(r.m.value(1, 1.0), 1.0), "peak: automation values");
    expect(r.m.rawNorm && near(r.m.rawNorm(1, 0), 0.5) && std::isnan(r.m.rawNorm(30, 5)), "peak: recorded values → normalized");
    Run bad;
    decode(bad, "Fruity Peak Controller", Bytes().zeros(12).b);
    expect(!bad.ok, "peak: short state rejected");
  }

  // ---------------------------------------------------------- Center
  {
    Run on, off;
    decode(on, "Fruity Center", Bytes().u32(1).i32(1).b);
    decode(off, "Fruity Center", Bytes().u32(1).i32(0).b);
    expect(on.ok && on.m.glType == "gl.utility" && on.m.exact, "center: decodes exactly to the utility");
    expect(near(param(on, UtlDcRemove), 1) && near(param(off, UtlDcRemove), 0), "center: status → DC removal");
  }

  // ------------------------------------------------------ Spectroman
  {
    Run r;
    decode(r, "Fruity Spectroman", Bytes().i32(100).i32(1).i32(96).i32(64).u8(1).u8(1).u8(0).b);
    expect(r.ok && r.m.glType == "gl.spectro", "spectroman: decodes");
    expect(near(param(r, ut::SgAmp), 12) && near(param(r, ut::SgScale), 0.5) && near(param(r, ut::SgStereo), 1), "spectroman: amp, scale, stereo");
  }

  // ----------------------------------------------------------- Tuner
  {
    Run r;
    decode(r, "Tuner", Bytes().u32(1).i32(3000).i32(1).i32(1).i32(2).b);
    expect(r.ok && r.m.glType == "gl.tuner" && r.m.exact, "tuner: decodes");
    expect(near(param(r, TnReactivity), 0.5) && near(param(r, TnBassMode), 1) && near(param(r, TnDisplay), 2), "tuner: reactivity, bass mode, circular display");
  }

  // ------------------------------------------------------ 7 Band EQ
  {
    Bytes st;
    st.u32(1);
    for (int k = 0; k < 7; ++k) st.i32((k - 3) * 600);
    Run r;
    decode(r, "Fruity 7 Band EQ", st.b);
    expect(r.ok && r.m.glType == "gl.geq" && r.m.exact, "7 band: decodes exactly");
    expect(near(param(r, 0), -18) && near(param(r, 3), 0) && near(param(r, 6), 18) && near(param(r, GeqLayout), 1), "7 band: levels, classic centres");
    expect(r.m.value && near(r.m.value(2, 0.5), 0.0) && r.m.rawNorm && near(r.m.rawNorm(2, 1800), 1.0), "7 band: automation");
  }

  // ------------------------------------------------------ Transmitter
  {
    Bytes st;
    st.u32(3);
    std::vector<int32_t> v(42, 5000);
    v[3] = 12800, v[4] = 0, v[13] = 10000, v[14] = 5000, v[36] = 1, v[37] = 1, v[38] = 0, v[39] = 5;
    for (int32_t x : v) st.i32(x);
    Run r;
    decode(r, "Transmitter", st.b);
    expect(r.ok && r.m.glType == "gl.transplit", "transmitter: decodes to the transient splitter");
    expect(near(param(r, dyn::TsAttack), 1.0) && near(param(r, dyn::TsRelease), -1.0), "transmitter: transient / sustain levels");
    expect(near(param(r, dyn::TsTransFx), dyn::TsFxSaturate) && near(param(r, dyn::TsTransFxMix), 1.0) &&
               near(param(r, dyn::TsTransFxA), 0.5) && near(param(r, dyn::TsTransFxB), 0.5),
           "transmitter: transient saturation slot (mix, drive, warmth)");
    expect(near(param(r, dyn::TsSusFx), dyn::TsFxOff) && near(param(r, dyn::TsDrive), 0.0), "transmitter: sustain slot off, no global drive");
    expect(near(param(r, dyn::TsSensitivity), 0.5) && near(param(r, dyn::TsHold), 25.0) && near(param(r, dyn::TsTransTilt), 0.0),
           "transmitter: sensitivity, hold, tilt");
    expect(r.m.param && r.m.param(14) == dyn::TsTransFxA && r.m.param(17) < 0, "transmitter: only the selected effect's knobs are linked");
    v[36] = 0;
    v[39] = 4, v[41] = 1, v[29] = 2500, v[31] = 96, v[32] = 8000;  // sustain: tempo-synced delay, 96 ticks = 1 beat
    v[5] = 10000, v[0] = 2000, v[33] = 1;
    Bytes st2;
    st2.u32(3);
    for (int32_t x : v) st2.i32(x);
    Run r2;
    decode(r2, "Transmitter", st2.b);
    expect(near(param(r2, dyn::TsTransOn), 0.0), "transmitter: disabled transient path");
    expect(near(param(r2, dyn::TsSusFx), dyn::TsFxEcho) && near(param(r2, dyn::TsSusFxMix), 0.25) && near(param(r2, dyn::TsSusFxA), 0.8) &&
               near(param(r2, dyn::TsSusSync), 1.0) && near(dyn::tsEchoSeconds(param(r2, dyn::TsSusFxB), true, 120), 0.5),
           "transmitter: synced sustain delay (one beat)");
    expect(near(param(r2, dyn::TsTransTilt), 1.0) && near(param(r2, dyn::TsSensitivity), 0.2) && near(param(r2, dyn::TsLookahead), 1.0),
           "transmitter: tilt, sensitivity, lookahead");
  }

  // ------------------------------------------------- Multiband Delay
  {
    Bytes st;
    st.u32(4).u32(393);
    for (int bank = 0; bank < 8; ++bank)
      for (int t = 0; t < 3; ++t)
        for (int k = 0; k < 16; ++k) {
          double v = t == 0 ? k / 15.0 : (t == 1 ? 1.0 : 0.5);
          if (bank == 1 && t == 0) v = 1.0 - k / 15.0;
          st.i32(static_cast<int32_t>(v * 1073741824.0));
        }
    for (int32_t g : {0, 0, 3000, 1500, 0, 1, 500, 64, 0}) st.i32(g);
    st.zeros(16);
    Run r;
    decode(r, "Multiband Delay", st.b);
    expect(r.ok && r.m.glType == "gl.banddelay", "multiband delay: decodes to band delay");
    sfx::BandCurves c = sfx::parseBandCurves(r.m.state.data);
    expect(near(c.v[0][sfx::BtDelay][0], 0) && near(c.v[0][sfx::BtDelay][sfx::kBdPoints - 1], 1.0), "multiband delay: bank A delay curve");
    expect(near(c.v[1][sfx::BtDelay][0], 1.0) && near(c.v[1][sfx::BtLevel][5], 1.0) && near(c.v[1][sfx::BtPan][9], 0.5), "multiband delay: bank B curves");
    expect(near(param(r, sfx::BdMorph), 0.5) && near(param(r, sfx::BdFeedback), 0.5) && near(param(r, sfx::BdKeepPitch), 1), "multiband delay: morph, feedback, keep pitch");
    expect(near(param(r, sfx::BdDry), 0.5) && near(param(r, sfx::BdWet), 1.0), "multiband delay: wet/dry");
    expect(near(param(r, sfx::BdSmooth), 0.15), "multiband delay: smoothing");
    Run bad;
    decode(bad, "Multiband Delay", Bytes().u32(4).u32(393).zeros(40).b);
    expect(!bad.ok, "multiband delay: truncated state rejected");
  }
}

}  // namespace dec3test
