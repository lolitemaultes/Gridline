// Effect decoder tests for FlpFxEq.cpp (included by test_flp_mixer.cpp).
// State blobs are built here by hand from the documented layouts.
#include "../src/core/PluginParams.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/utility/utility.h"

namespace gl::flp::fx {
double flEq2FreqHz(double v);  // FlpFxEq.cpp
}

namespace fxeq {
// Parametric EQ 2: version + 36 knobs + 36 spare + 8 option words.
std::vector<uint8_t> eq2(const int level[7], const int freq[7], const int width[7], const int type[7], const int order[7], int main) {
  Blob b;
  b.i32(8);
  for (int k = 0; k < 7; ++k) b.i32(level[k]);
  for (int k = 0; k < 7; ++k) b.i32(freq[k]);
  for (int k = 0; k < 7; ++k) b.i32(width[k]);
  for (int k = 0; k < 7; ++k) b.i32(type[k]);
  for (int k = 0; k < 7; ++k) b.i32(order[k]);
  b.i32(main);
  for (int k = 0; k < 36 + 8; ++k) b.i32(0);
  return b.b;
}
}  // namespace fxeq

static void testFxEq() {
  namespace pp = gl::pp;
  namespace ut = gl::packs::utility;
  namespace dy = gl::packs::dynamics;
  flp::PluginMapping m;
  {
    const int level[7] = {0, -600, 0, 0, 0, 300, -1800};
    const int freq[7] = {6000, 18000, 25000, 32768, 40000, 47000, 60000};
    const int width[7] = {40000, 25000, 25000, 25000, 25000, 25000, 40000};
    const int type[7] = {3, 6, 0, 6, 6, 7, 1};  // high pass, peak, off, peak, peak, high shelf, low pass
    const int order[7] = {-2, 0, 0, -1, 2, 0, 0};
    expect(decodeFx("Fruity Parametric EQ 2", fxeq::eq2(level, freq, width, type, order, -250), m) && m.glType == "gl.eq",
           "EQ2: decodes to gl.eq");
    expect(param(m, pp::eqParam(0, pp::EqType)) == pp::EqLowCut && param(m, pp::eqParam(6, pp::EqType)) == pp::EqHighCut &&
               param(m, pp::eqParam(2, pp::EqType)) == pp::EqOff && param(m, pp::eqParam(5, pp::EqType)) == pp::EqHighShelf,
           "EQ2: band types (FL high pass = low cut, low pass = high cut)");
    expect(near(param(m, pp::eqParam(1, pp::EqGain)), -6.0, 1e-5) && near(param(m, pp::EqMain), -2.5, 1e-5), "EQ2: levels in 0.01 dB");
    expect(param(m, pp::eqFl(0, pp::EqFlOrder)) == pp::EqSteep6 && param(m, pp::eqFl(3, pp::EqFlOrder)) == pp::EqSteep4 &&
               param(m, pp::eqFl(1, pp::EqFlOrder)) == pp::EqOrd2 && param(m, pp::eqFl(4, pp::EqFlOrder)) == pp::EqGentle6,
           "EQ2: orders map 1:1 (-3..3)");
    expect(near(param(m, pp::eqFl(0, pp::EqWidth)), 40000.0 / 65536.0, 1e-6) && param(m, pp::EqModel) == 1, "EQ2: widths, reference model");
    expect(m.exact, "EQ2: exact");
    expect(near(param(m, pp::eqParam(3, pp::EqFreq)), 20.0 * std::sqrt(1000.0), 0.01), "EQ2: band frequency curve");
    expect(near(fx::flEq2FreqHz(0), 20.0, 1e-9) && near(fx::flEq2FreqHz(65536), 20000.0, 1e-6), "EQ2: frequency curve 20 Hz..20 kHz");
    expect(m.param && m.param(8) == pp::eqParam(1, pp::EqFreq) && m.param(15) == pp::eqFl(1, pp::EqWidth) &&
               m.param(29) == pp::eqFl(1, pp::EqFlOrder) && m.param(35) == pp::EqMain && m.param(99) == -1, "EQ2: automation map");
    expect(m.value && near(m.value(0, 0.0), -18.0, 1e-9) && near(m.value(35, 1.0), 18.0, 1e-9), "EQ2: automation values");
  }
  {
    Blob b;
    b.i32(-64).i32(128);
    expect(decodeFx("Fruity Balance", b.b, m) && m.glType == "gl.utility" && m.exact, "Balance: decodes (exact)");
    expect(near(param(m, pp::UtlPan), -0.5, 1e-6) && near(param(m, pp::UtlGain), 20 * std::log10((std::sqrt(11.0) - 1) / 10), 1e-3) && param(m, pp::UtlPanLaw) == 2, "Balance: pan, volume taper, folded law");
  }
  {
    Blob b;
    b.i32(0).i32(-12800).i32(12800).i32(6400).i32(12800).i32(0).i32(0).i32(0).i32(1);
    expect(decodeFx("Fruity Stereo Shaper", b.b, m) && m.glType == "gl.stereomatrix", "Stereo Shaper: decodes");
    expect(near(param(m, ut::SmLL), 1.0, 1e-6) && near(param(m, ut::SmRL), -1.0, 1e-6) && near(param(m, ut::SmRR), 0.5, 1e-6) &&
               near(param(m, ut::SmLR), 1.0, 1e-6),
           "Stereo Shaper: matrix (tag 0 = right into left)");
  }
  {
    Blob b;
    b.i32(100).i32(128);
    expect(decodeFx("Fruity Soft Clipper", b.b, m) && m.glType == "gl.clipper", "Soft Clipper: decodes");
    expect(near(param(m, pp::ClpPostGain), 0.0, 1e-6) && param(m, pp::ClpThreshold) < 0, "Soft Clipper: post gain unity, threshold below 0 dB");
  }
  {
    Blob b;
    b.i32(128).i32(5).i32(1).i32(64).i32(128);
    expect(decodeFx("Fruity Fast Dist", b.b, m) && m.glType == "gl.fastdist", "Fast Dist: decodes");
    expect(near(param(m, dy::FdDrive), 5, 1e-6) && param(m, dy::FdType) == 1 && near(param(m, dy::FdMixK), 0.5, 1e-6) &&
               near(param(m, dy::FdPostK), 1.0, 1e-6) && near(param(m, dy::FdPreK), 1.0, 1e-6) && param(m, dy::FdClassic) == 1 && m.exact,
           "Fast Dist: threshold, type, mix, post (reference engine, exact)");
  }
  {
    Blob b;
    b.i32(0).i32(256).i32(0).i32(0).i32(1).i32(1);
    expect(decodeFx("Fruity Stereo Enhancer", b.b, m) && m.glType == "gl.stereo", "Stereo Enhancer: decodes");
    expect(near(param(m, pp::StVolume), 0.0, 1e-6) && near(param(m, pp::StWidth), 1.0, 1e-6) && param(m, pp::StInvert) == 1,
           "Stereo Enhancer: neutral width, volume, inversion");
  }
  {
    Blob b;
    b.i32(2).i32(300).i32(512).i32(169).i32(676).u8(1);
    expect(decodeFx("Fruity Free Filter", b.b, m) && m.glType == "gl.freefilter", "Free Filter: decodes");
    // J-26 reference laws: type 300 → high pass, gain 36 n - 18 dB (676 → +5.77 dB), DC remover byte.
    expect(param(m, ut::FfType) == 2 && near(param(m, ut::FfGain), 36.0 * 676 / 1024 - 18, 1e-3) && param(m, ut::FfDc) == 1 && m.exact,
           "Free Filter: type (300 = high pass), gain law, centre byte");
  }
  {
    Blob b;
    b.i32(2).i32(1024).i32(205).i32(1024).i32(0).i32(512).i32(1).u8(1);
    expect(decodeFx("Fruity Filter", b.b, m) && m.glType == "gl.filter", "Filter: decodes");
    expect(param(m, pp::FltBlend) == 1 && near(param(m, pp::FltLPLevel), 1.0, 1e-6) && near(param(m, pp::FltHPLevel), 0.5, 1e-6) &&
               param(m, pp::FltSlope) == 1,
           "Filter: blend levels and x2");
  }
  {
    // Version 1 (float points): unipolar soft clip curve (0,0) → (0.5,0.5) → (1,0.75).
    Blob b;
    b.i32(1).i32(128).i32(128).i32(128);
    b.u8(0).u8(0).u8(0).u8(0).u8(0).u8(0);
    b.i32(3).i32(0).i32(3);
    b.f32(0).f32(0).f32(0);
    b.f32(0.5f).f32(0.5f).f32(0);
    b.f32(0.5f).f32(0.75f).f32(0);
    b.f32(0);
    expect(decodeFx("Fruity WaveShaper", b.b, m) && m.glType == "gl.waveshaper", "WaveShaper: decodes");
    m.state.type = "gl.waveshaper";
    expect(param(m, pp::WsBipolar) == 1 && m.state.data.rfind("WS1|", 0) == 0 && near(gl::waveshaperCurve(m.state, 1.0), 0.75, 1e-6) &&
               near(gl::waveshaperCurve(m.state, 0.25), 0.25, 1e-6) && near(gl::waveshaperCurve(m.state, -1.0), -0.75, 1e-6) &&
               near(gl::waveshaperCurve(m.state, 0.75), 0.625, 1e-6),
           "WaveShaper: free points kept (unipolar, mirrored)");
  }
}
