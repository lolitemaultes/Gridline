// Effect decoder tests for FlpFxDyn.cpp (included by test_flp_mixer.cpp).
#include "../src/core/PluginParams.h"
#include "../src/packs/dynamics/dynamics.h"

static void testFxDyn() {
  namespace pp = gl::pp;
  // Fruity Limiter: version, 18 knobs (comp threshold 286, ratio 415), spare 18, speed, view 1, toggles, ?, sidechain 1, u8.
  {
    Blob b;
    b.i32(7);
    const int32_t knobs[18] = {1000, 1000, 1000, 1115, 3, 5000, 3, 2882, 286, 415, 0, 0, 6666, 1, 650, 1000, 0, 4187};
    for (int32_t k : knobs) b.i32(k);
    for (int32_t k : knobs) b.i32(k);
    b.i32(50).i32(1).u8(1).u8(1).u8(1).u8(1).i32(0).i32(1).u8(0);
    flp::PluginMapping m;
    expect(decodeFx("Fruity Limiter", b.b, m) && m.glType == "gl.limiter", "limiter: decodes");
    expect(near(param(m, pp::LimCeiling), 0.0, 1e-6) && near(param(m, pp::LimGain), 0.0, 1e-6), "limiter: unity gain / ceiling");
    expect(near(param(m, pp::LimCompThresh), 20 * std::log10((std::pow(7.0, 0.286) - 1) / 6), 1e-3), "limiter: comp threshold (7^x law)");
    expect(param(m, pp::LimCompRatio) > 1.5 && param(m, pp::LimCompRatio) < 5, "limiter: comp ratio");
    expect(param(m, pp::LimSidechain) == 1 && param(m, pp::LimView) == 1, "limiter: sidechain input + comp view");
    expect(m.param && m.param(8) == pp::LimCompThresh && m.value && near(m.value(8, 1.0), 0.0, 1e-6), "limiter: automation mapping");
  }
  // Fruity Compressor.
  {
    Blob b;
    b.i32(2).i32(-245).i32(58).i32(-40).i32(150).i32(200).i32(4).i32(1);
    flp::PluginMapping m;
    expect(decodeFx("Fruity Compressor", b.b, m) && m.glType == "gl.comp", "compressor: decodes");
    expect(near(param(m, pp::CmpThreshold), -24.5, 1e-4) && near(param(m, pp::CmpRatio), 5.8, 1e-4), "compressor: threshold, ratio");
    expect(near(param(m, pp::CmpOutput), -4.0, 1e-4) && near(param(m, pp::CmpAttack), 0.015, 1e-6) && near(param(m, pp::CmpRelease), 0.2, 1e-6),
           "compressor: gain, attack, release");
    expect(param(m, pp::CmpCharacter) == 0 && param(m, pp::CmpFastRelease) == 1 && near(param(m, pp::CmpKnee), 0, 1e-6), "compressor: type 4 = hard, fast release");
  }
  // Fruity Multiband Compressor (defaults + a few edits).
  {
    Blob b;
    const int32_t v[28] = {32768, 1, 0, 0, 17000, 21845, 45875, 32768, 100, 1000, 52429, 1, 17000, 6500, 21845, 48875, 32768, 100, 1000,
                           52429, 2, 200, 21845, 45875, 32768, 200, 2000, 52429};
    b.i32(0);
    for (int32_t k : v) b.i32(k);
    b.i32(0).i32(50);
    flp::PluginMapping m;
    expect(decodeFx("Fruity Multiband Compressor", b.b, m) && m.glType == "gl.multiband", "multiband: decodes");
    expect(near(param(m, pp::MbLowXover), std::pow(19981.0, std::sqrt(180.0 / 19980)) + 19, 1e-2) && near(param(m, pp::MbHighXover), 9236.7, 0.1),
           "multiband: band edges (19981^sqrt(n) + 19 Hz)");
    expect(param(m, pp::MbMidState) == 2 && param(m, pp::MbLowState) == 1 && param(m, pp::MbLimiter) == 1, "multiband: band states (1 muted, 2 bypass), limiter");
    expect(near(param(m, pp::MbLowThresh), -18, 0.01) && near(param(m, pp::MbOutput), 0, 1e-6), "multiband: threshold / output");
    expect(near(param(m, pp::MbLowAtk), 0.02, 1e-6) && near(param(m, pp::MbLowRel), 0.2, 1e-6), "multiband: per-band attack / release");
  }
  // Maximus: header, 7 globals, 4 band blocks (low on, mid mute, high off, master).
  {
    Blob b;
    b.i32(0x000F0003);
    for (int32_t k : {19800, 1, 48065, 1, 1115, 1000, 32768}) b.i32(k);
    for (int k = 0; k < 4; ++k) {
      int mode = k == 0 ? 1 : k == 1 ? 2 : 0;
      for (int32_t x : {mode, 1000, k == 0 ? 2000 : 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0}) b.i32(x);
    }
    flp::PluginMapping m;
    namespace dy = gl::packs::dynamics;
    expect(decodeFx("Maximus", b.b, m) && m.glType == "gl.master", "maximus: decodes to Mastering Dynamics");
    expect(param(m, dy::msParam(0, dy::MbMode)) == 0 && param(m, dy::msParam(1, dy::MbMode)) == 2 && param(m, dy::msParam(2, dy::MbMode)) == 1,
           "maximus: band modes");
    expect(near(param(m, dy::msParam(0, dy::MbPost)), 20 * std::log10((std::pow(7.0, 2.0) - 1) / 6), 1e-3), "maximus: band post gain (7^x law)");
    expect(near(param(m, dy::MsLowFreq), 30 * std::pow(2.0, 19800 * 0.000138227748), 0.01) && param(m, dy::msExt(0, dy::MxCurveMode)) == 1,
           "maximus: band edge law, drawn curve engine");
  }
  // Soundgoodizer (FL data folder absent in tests: generic setting + amount).
  {
    Blob b;
    b.i32(3).i32(1).i32(600);
    flp::PluginMapping m;
    namespace dy = gl::packs::dynamics;
    expect(decodeFx("Soundgoodizer", b.b, m) && m.glType == "gl.enhance" && near(param(m, dy::EnAmount), 0.6, 1e-6) &&
               param(m, dy::EnMode) == 1,
           "soundgoodizer: mode + amount");
  }
  // Transient Processor.
  {
    Blob b;
    b.i32(1);
    for (int32_t k : {128, 0, 40, 80, 1, 2, 80, 80, 1}) b.i32(k);
    b.i32(5).i32(12800).u8(0);
    flp::PluginMapping m;
    expect(decodeFx("Transient Processor", b.b, m) && m.glType == "gl.transient", "transient: decodes");
    expect(near(param(m, pp::TrAttack), 0.6, 1e-6) && near(param(m, pp::TrSustain), -0.5, 1e-6) && param(m, pp::TrReleaseSpeed) == 2,
           "transient: attack, release, shape");
  }
}
