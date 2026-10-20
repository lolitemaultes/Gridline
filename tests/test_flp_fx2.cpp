// Gridline — FLP import, second-wave effect decoders, on hand-made state
// blobs (no FL Studio content). Optional checks run on FL's own supplied
// render-test projects when FL Studio is installed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/Json.h"
#include "../src/core/PluginParams.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpFx2.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/modulation/modulation.h"
#include "../src/packs/spacefx/spacefx.h"
#include "../src/packs/spectral/spectral.h"
#include "../src/packs/utility/utility.h"
#include "Check.h"

using namespace gl;

namespace {

struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Blob& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return i32(static_cast<int32_t>(u));
  }
  Blob& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    i32(static_cast<int32_t>(u & 0xFFFFFFFFu));
    return i32(static_cast<int32_t>(u >> 32));
  }
  Blob& u8(uint8_t v) { b.push_back(v); return *this; }
  Blob& bytes(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return *this; }
  Blob& u64(uint64_t v) { i32(static_cast<int32_t>(v & 0xFFFFFFFFu)); return i32(static_cast<int32_t>(v >> 32)); }
  Blob& chunk(uint32_t id, const std::vector<uint8_t>& payload) {
    i32(static_cast<int32_t>(id));
    u64(payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
    return *this;
  }
};

struct Env {
  flp::File file;
  Song song;
  flp::Report report;
  flp::Context ctx{file, song, report};
};

bool decode(Env& e, const std::string& name, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  return flp::fx2::decodeEmbedded(e.ctx, name, state.data(), state.size(), m);
}

float P(const flp::PluginMapping& m, int i) { return i < static_cast<int>(m.state.params.size()) ? m.state.params[static_cast<size_t>(i)] : NAN; }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  namespace pp = gl::pp;
  namespace sp = gl::packs::spectral;
  namespace sx = gl::packs::spacefx;
  namespace md = gl::packs::modulation;
  namespace ut = gl::packs::utility;
  namespace dy = gl::packs::dynamics;

  {  // PanOMatic: static pan / volume exact, LFO off
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(64).i32(320).i32(0).i32(0).i32(64).i32(27500);
    expect(decode(e, "Fruity PanOMatic", s.b, m) && m.glType == "gl.autopan", "PanOMatic → Auto Pan");
    expect(near(P(m, pp::ApStaticPan), -0.5, 1e-6) && near(P(m, pp::ApStaticVol), 1.25, 1e-6), "PanOMatic pan / volume");
    expect(P(m, pp::ApTarget) == 0 && P(m, pp::ApClassic) == 1 && m.exact, "PanOMatic LFO off is exact (reference engine)");
    expect(m.param && m.param(1) == pp::ApStaticVol && near(m.value(1, 0.8), 1.0, 1e-9), "PanOMatic volume automation");
    expect(m.rawNorm && near(m.rawNorm(1, 160), 0.5, 1e-9), "PanOMatic raw control value → normalized");
    Blob l;
    l.i32(128).i32(256).i32(1).i32(2).i32(128).i32(55000);
    flp::PluginMapping m2;
    expect(decode(e, "Fruity PanOMatic", l.b, m2) && P(m2, pp::ApTarget) == 2 && P(m2, pp::ApAmount) == 1 && P(m2, pp::ApShape) == 1,
           "PanOMatic LFO to volume (target 2)");
  }
  {  // Pitcher: float parameters
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.bytes("SSLF").i32(152).bytes("1RTP");
    float f[33] = {};
    f[0] = 1.0f;   // speed: instant
    f[1] = 1.0f;
    f[2] = 0.75f;  // gender → +6 st
    f[3] = 0.5f;
    f[17] = f[18] = f[19] = f[20] = 0.5f;
    f[22] = 1.0f;  // formant
    f[23] = 0.5f;  // 440 Hz
    f[28] = 9.0f;
    f[32] = 2.0f;  // 110 Hz
    for (float v : f) s.f32(v);
    const uint8_t mask[12] = {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1};  // C major
    for (uint8_t v : mask) s.u8(v);
    s.i32(0);
    expect(decode(e, "Pitcher", s.b, m) && m.glType == "gl.pitcher", "Pitcher → Pitch Corrector");
    expect(near(P(m, sp::PcSpeed), 1.0, 1e-6) && near(P(m, sp::PcFormant), 6.0, 1e-5) && P(m, sp::PcPreserve) == 1,
           "Pitcher speed / gender / formant");
    expect(near(P(m, sp::PcReference), 440.0, 1e-3) && P(m, sp::PcLowest) == 2, "Pitcher reference / range");
    bool maskOk = true;
    for (int k = 0; k < 12; ++k) maskOk = maskOk && P(m, sp::PcNote0 + k) == mask[k];
    expect(maskOk && P(m, sp::PcScale) == sp::kScaleCount - 1 && m.exact, "Pitcher scale mask (custom)");
    expect(P(m, sp::PcModel) == 1 && P(m, sp::PcCorrSpeed) == 1.0f && P(m, sp::PcRange6) == 2 && P(m, sp::PcRefChannel) == 0,
           "Pitcher reference model: speed / range / pitch reference");
    expect(P(m, sp::PcMidiPort) == -1 && P(m, sp::PcHarmonize) == 0 && near(P(m, sp::PcFine), 0, 1e-4) && P(m, sp::PcStereo) == 1.0f,
           "Pitcher MIDI port / harmonize / fine / stereo");
    expect(m.param && m.param(0) == sp::PcCorrSpeed && near(m.value(0, 0.25), 0.25, 1e-9), "Pitcher speed automation (raw 0..1)");
    // Harmonize state with fine tune, key bypass, 25 Hz range and MIDI port 1.
    Blob h;
    h.bytes("SSLF").i32(152).bytes("1RTP");
    f[3] = 0.5025f;
    f[7] = 1.0f;     // key D bypassed
    f[24] = 0.7f;    // both channels
    f[25] = f[26] = f[27] = 1.0f;
    f[17] = 0.0f;    // voice 1 hard left
    f[30] = f[31] = 1.0f;
    f[32] = 5.0f;    // 25 Hz
    for (float v : f) h.f32(v);
    for (uint8_t v : mask) h.u8(v);
    h.f32(0.01125f);  // port int(2.88) - 1 = 1
    flp::PluginMapping m2;
    expect(decode(e, "Pitcher", h.b, m2) && m2.exact, "Pitcher harmonize state decodes exactly");
    expect(near(P(m2, sp::PcFine), 0.5, 1e-3) && P(m2, sp::PcKeyBypass0 + 2) == 1 && P(m2, sp::PcNote0 + 2) == 1 && P(m2, sp::PcRefChannel) == 1,
           "Pitcher fine tune / key bypass keeps the scale note / both channels");
    expect(P(m2, sp::PcMidiTargets) == 1 && P(m2, sp::PcMidiOctaves) == 1 && P(m2, sp::PcHarmonize) == 1 && P(m2, sp::PcHarmMix) == 1 &&
               P(m2, sp::PcHarmVel) == 1 && near(P(m2, sp::PcHarmPan1), -1, 1e-6) && P(m2, sp::PcRange6) == 5 && P(m2, sp::PcMidiPort) == 1,
           "Pitcher MIDI / octaves / harmonize / mix / velocity / pan / range / port");
  }
  {  // Spreader
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(0x10003).i32(7500).i32(-2500).i32(250000).i32(1).i32(1).i32(0);
    expect(decode(e, "Spreader", s.b, m) && near(P(m, sp::SrSpread), 0.75, 1e-6) && near(P(m, sp::SrSeparation), -0.25, 1e-6),
           "Spreader spread / separation");
    expect(P(m, sp::SrModel) == 1 && P(m, sp::SrLowBypass) == 1 && near(P(m, sp::SrLowFreq), 1000.0 * 249000.0 / 999000.0, 1e-2) &&
               P(m, sp::SrBassMono) == 1 && P(m, sp::SrMonoCheck) == 0 && P(m, sp::SrEnabled) == 1 && m.exact,
           "Spreader low bypass curve, mono bass, enabled");
    expect(near(sp::spreadLowHz(1000 + 999000 * 0.75), 5250, 1e-6) && near(sp::spreadLowHz(1), 0, 1e-9) && near(sp::spreadLowHz(1000000), 10000, 1e-6),
           "Spreader low bypass: 500 Hz at mid range, 10 kHz at the top");
    Blob v1;  // version 1: three knobs, mono / enabled zero-filled
    v1.i32(5000).i32(0).i32(1);
    flp::PluginMapping m1;
    expect(decode(e, "Spreader", v1.b, m1) && P(m1, sp::SrEnabled) == 0 && P(m1, sp::SrLowBypass) == 0 && m1.exact, "Spreader v1 state loads disabled");
  }
  {  // X-Y Controller
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(2).i32(16384).i32(0).i32(65536).i32(64).i32(49152).i32(0).i32(65536).i32(0).i32(2048).i32(1024);
    expect(decode(e, "Fruity X-Y Controller", s.b, m) && m.glType == "gl.xyctl", "X-Y → X-Y Pad");
    expect(near(P(m, md::XyX), 0.25, 1e-6) && near(P(m, md::XyY), 0.75, 1e-6) && near(P(m, md::XyXTension), 0.5, 1e-6) && m.exact,
           "X-Y values and tension");
  }
  {  // X-Y-Z Controller
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(3).i32(0).i32(32768).i32(65536);
    for (int k = 0; k < 8; ++k) s.i32(2048);
    expect(decode(e, "Fruity X-Y-Z Controller", s.b, m) && m.glType == "gl.xyzctl" && near(P(m, md::XzZ), 1.0, 1e-6),
           "X-Y-Z → X-Y-Z Pad");
  }
  {  // Fast LP: open filter exact; cutoff curve monotonic
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(1).i32(1024).i32(0);
    expect(decode(e, "Fruity Fast LP", s.b, m) && near(P(m, ut::QlCutoff), 20000, 1e-3) && m.exact, "Fast LP open");
    expect(m.value(0, 0.5) < m.value(0, 0.7) && m.value(0, 0.7) < m.value(0, 0.9), "Fast LP cutoff curve rises");
  }
  {  // Pitch Shifter: cents, mix (new Gridline parameter)
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(7).i32(-700).i32(250).i32(0).i32(1200).i32(8500).i32(200).i32(0).i32(0).i32(768).i32(0).i32(0).i32(65536).i32(0);
    expect(decode(e, "Pitch Shifter", s.b, m) && near(P(m, sp::PsPitch), -7, 1e-6) && near(P(m, sp::PsFormant), 2.5, 1e-6),
           "Pitch Shifter pitch / formant");
    expect(near(P(m, sp::PsMix), 0.5, 1e-6) && P(m, sp::PsPreserve) == 1 && m.exact, "Pitch Shifter mix / correction");
    expect(P(m, sp::PsCorrection) == 1 && near(P(m, sp::PsDelay), 1000, 1e-4) && near(P(m, sp::PsDensity), 2, 1e-6) &&
               near(P(m, sp::PsDuration), 85, 1e-6) && P(m, sp::PsEngine) == sp::kPsMusic,
           "Pitch Shifter correction / delay curve / density / duration / engine");
    Blob v7;  // full v7 state: voice engine, tempo sync, post delay, partial correction, fixed C5
    v7.i32(7).i32(500).i32(-300).i32(2).i32(2400).i32(4000).i32(150).i32(40).i32(60).i32(192).i32(82).i32(1).i32(32768).i32(2);
    v7.i32(32768).u8(1).i32(0).i32(0).i32(1000).u8(1).u8(1);
    flp::PluginMapping m7;
    expect(decode(e, "Pitch Shifter", v7.b, m7) && m7.exact && P(m7, sp::PsEngine) == sp::kPsVoice && P(m7, sp::PsDetector) == 0,
           "Pitch Shifter v7 voice engine / detector");
    expect(P(m7, sp::PsDelaySync) == 1 && near(P(m7, sp::PsDelaySteps), 16.0 * 191.0 / 767.0, 1e-4) && P(m7, sp::PsDelayMode) == 1 && near(P(m7, sp::PsFeedback), 0.82, 1e-6),
           "Pitch Shifter tempo-synced delay steps, post shift, feedback");
    expect(near(P(m7, sp::PsCorrection), 0.5, 1e-6) && P(m7, sp::PsPitchMode) == 2 && P(m7, sp::PsAnalysis) == 2 && near(P(m7, sp::PsJitter), 0.4, 1e-6) &&
               near(P(m7, sp::PsRand), 0.6, 1e-6),
           "Pitch Shifter correction / pitch mode / analysis / jitter / rand");
  }
  {  // LuxeVerb: level slider 746 = 0 dB, -600 = off
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(8).i32(0).u8(0).u8(0).u8(1).u8(0).u8(0);
    int32_t t[39] = {746, 20000, 20, 1, 13105, 0, 192, 850, 50, 20, 100, 10, 1000, 0, 1, 0, 120, 1000, 192, 700, 1719, 20,
                     1000, 0, -4000, 50, 400, 50, 0, 0, 301, 10, -600, 746, 7726, 0, 500, 125, 1};
    for (int32_t v : t) s.i32(v);
    expect(decode(e, "LuxeVerb", s.b, m) && m.glType == "gl.hall", "LuxeVerb → Hall");
    expect(near(P(m, sx::HvInGain), 0.0, 0.1) && near(P(m, sx::HvWet), 0.0, 0.1) && near(P(m, sx::HvDry), -60, 1e-6),
           "LuxeVerb level curve");
    expect(near(P(m, sx::HvFbPitch), 12, 1e-6) && near(P(m, sx::HvWidth), 1.25, 1e-6) && P(m, sx::HvOn) == 1,
           "LuxeVerb shimmer pitch / width / reverb on");
    expect(P(m, sx::HvModel) == 1 && P(m, sx::HvFbOn) == 0 && P(m, sx::HvLimiter) == 1 && near(P(m, sx::HvEqQ), 0.25 + 4.75 * 475.0 / 4975.0, 1e-5) &&
               m.exact,
           "LuxeVerb reference model, switches from tags, peak Q law");
  }
  {  // Convolver: impulse file path from FL's data folder
    Env e;
    e.ctx.factoryData = "/fl";
    flp::PluginMapping m;
    Blob s;
    std::string path = "%FLStudioData%\\Patches\\Impulses\\Room.wav";
    s.i32(14).i32(1000).i32(250).i32(0).i32(0).u8(1).u8(static_cast<uint8_t>(path.size())).bytes(path);
    expect(decode(e, "Fruity Convolver", s.b, m) && m.state.data == "/fl/Data/Patches/Impulses/Room.wav", "Convolver impulse path");
    expect(near(P(m, sx::CvWet), (std::pow(1001.0, 0.25) - 1.0) / 1000.0, 1e-6) && near(P(m, sx::CvDry), 1.0, 1e-6) && P(m, sx::CvSource) == sx::SrcFile &&
               P(m, sx::CvFadeOut) == 0,
           "Convolver levels (FL wet / dry laws)");
  }
  {  // Vintage Chorus: index-ordered parameters
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(1);
    int32_t t[17] = {2, 0, 122881, 1, 3000, 600, 600, 1500, 1, 0, 0, 0, 0, 0, 1, 0, 0};
    for (int32_t v : t) s.i32(v);
    expect(decode(e, "Vintage Chorus", s.b, m) && P(m, sx::BcMode) == 3 && near(P(m, sx::BcMix), 0.25, 1e-6) && P(m, sx::BcModel) == 1,
           "Vintage Chorus mode (FL 2 = custom) / mix, reference model");
    expect(near(P(m, sx::BcRate), 2.5, 0.01) && P(m, sx::BcWave) == 0 && P(m, sx::BcMonoIn) == 1 && near(P(m, sx::BcPhaseLR), 0.5, 1e-6),
           "Vintage Chorus speed curve / switches (FL LFO 1 = sine)");
  }
  {  // NoteBook (RTF) → Notes
    Env e;
    flp::PluginMapping m;
    std::string rtf = "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}\\f0 Hello\\par World\\par }";
    Blob s;
    s.i32(1000).i32(1).i32(1).i32(static_cast<int32_t>(rtf.size())).bytes(rtf);
    expect(decode(e, "Fruity NoteBook", s.b, m) && m.glType == "gl.notes" && m.state.data.find("Hello\nWorld") != std::string::npos,
           "NoteBook RTF → text");
  }
  {  // Formula Controller (v2: ANSI formula)
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(2).i32(65536).i32(0).i32(32768).u8(5).bytes("a*b+c").i32(0);
    expect(decode(e, "Fruity Formula Controller", s.b, m) && m.state.data == "a*b+c" && near(P(m, md::FmA), 1.0, 1e-6) && m.exact,
           "Formula Controller knobs and formula");
  }
  {  // Wrapper: reported by the hosted plugin's name
    Env e;
    flp::PluginMapping m;
    Blob s;
    std::vector<uint8_t> nm = {'S', 'o', 'm', 'e', 'V', 'S', 'T'};
    s.i32(8).chunk(54, nm);
    expect(!decode(e, "Fruity Wrapper", s.b, m) && m.flName == "Fruity Wrapper (SomeVST)", "Wrapper reports the plugin name");
    // An FL plugin loaded through the wrapper: mapped from its normalized parameters.
    Blob w;
    std::vector<uint8_t> lp = {'F', 'r', 'u', 'i', 't', 'y', ' ', 'F', 'a', 's', 't', ' ', 'L', 'P'};
    Blob vals;
    vals.i32(-9).i32(-507).u8(0xFF).i32(0).i32(0).i32(0).i32(2).f32(1.0f).f32(0.5f);
    w.i32(8).chunk(54, lp).chunk(53, vals.b);
    flp::PluginMapping m2;
    expect(decode(e, "Fruity Wrapper", w.b, m2) && m2.glType == "gl.fastlp" && near(P(m2, ut::QlCutK), 1.0, 1e-6) &&
               near(P(m2, ut::QlResK), 0.5, 1e-6),
           "wrapped FL plugin mapped from its parameter values");
  }
  {  // Frequency Splitter: crossover curve and main-output levels
    Env e;
    flp::PluginMapping m;
    Blob s;
    // version, tags 0-13, 3 send targets, 2 monitor ints, view bits, mute bits (word 20), flags + analyser ints.
    s.i32(4).i32(0).i32(65536).i32(3).i32(7).i32(0).i32(1).i32(2).i32(45820).i32(0).i32(0).i32(0).i32(-600).i32(0).i32(300);
    s.i32(0).i32(5).i32(0).i32(2).i32(2).i32(12).i32(8 | 2);
    for (int k = 0; k < 7; ++k) s.i32(0);
    expect(decode(e, "Frequency Splitter", s.b, m) && near(P(m, sp::SpFreq1), 30, 1e-6) && near(P(m, sp::SpFreq2), 16000, 1e-3) &&
               P(m, sp::SpSlopeA) == 3 && P(m, sp::SpSlopeB) == 7 && P(m, sp::SpModel) == 1,
           "Frequency Splitter crossovers and slopes");
    expect(near(P(m, sp::splitParam2(0, sp::SxMain)), 20 * std::log10(1200.0 / 1800.0), 1e-4) &&
               near(P(m, sp::splitParam2(2, sp::SxMain)), 20 * std::log10(1 + 6.943282 * 300 / 1800), 1e-4) &&
               P(m, sp::splitParam(0, sp::SbMute)) == 1 && P(m, sp::splitParam2(1, sp::SxSendMute)) == 1 && P(m, sp::SpBands) == 1,
           "Frequency Splitter levels (amplitude curve) / main + send mute bits");
    expect(P(m, sp::splitParam2(1, sp::SxSendTo)) == 5 && P(m, sp::splitParam2(0, sp::SxSendTo)) == 0 && P(m, sp::SpRoute) == 1 && m.exact,
           "Frequency Splitter band send target = the hosting track's route index (J-45)");
  }
  {  // Patcher: input → Fast LP → output, plus a parallel Spreader
    Env e;
    flp::PluginMapping m;
    auto node = [](uint64_t id, const std::string& name, const std::vector<uint8_t>& state, uint64_t in, uint64_t out) {
      Blob n;
      Blob hdr;
      hdr.i32(2).u64(id).i32(1).i32(0);
      n.chunk(5000, hdr.b);
      n.chunk(5008, std::vector<uint8_t>(name.begin(), name.end()));
      n.chunk(5005, state);
      Blob mix;
      mix.f32(1.0f);
      n.chunk(5007, mix.b);
      Blob pin, pout;
      pin.i32(0).i32(0).u64(in).i32(3).i32(0);
      pout.i32(0).i32(0).u64(out).i32(2).i32(0);
      n.chunk(5009, pin.b).chunk(5009, pout.b).chunk(5001, {});
      return n.b;
    };
    auto io = [](uint64_t id, int kind) {
      Blob n, h;
      h.u64(id).i32(0).i32(kind).i32(2).i32(0).i32(0).i32(0);
      n.chunk(1400, h.b).chunk(1401, {});
      return n.b;
    };
    auto link = [](uint64_t dst, uint64_t src) {
      Blob n, h;
      h.u64(dst).u64(src).f32(1.0f).i32(0).i32(0).i32(0);
      n.chunk(1300, h.b).chunk(1301, {});
      return n.b;
    };
    Blob lp, spr;
    lp.i32(1).i32(1024).i32(0);
    spr.i32(0x10003).i32(5000).i32(0).i32(1).i32(0).i32(1).i32(0);
    Blob s;
    s.i32(4);
    s.chunk(0, io(1000, 7)).chunk(0, io(1001, 6));
    s.chunk(0, node(1002, "Fruity Fast LP", lp.b, 1010, 1011));
    s.chunk(0, node(1003, "Spreader", spr.b, 1020, 1021));
    s.chunk(0, link(1010, 1000)).chunk(0, link(1020, 1000)).chunk(0, link(1001, 1011)).chunk(0, link(1001, 1021));
    expect(decode(e, "Patcher", s.b, m) && m.glType == "gl.patcherfx", "Patcher → Patch Host FX");
    Json j;
    bool parsed = jsonParse(m.state.data, j);
    const Json* nodes = parsed ? j.find("nodes") : nullptr;
    expect(nodes && nodes->array.size() == 2 && nodes->array[0].text("type") == "gl.fastlp" && nodes->array[1].text("type") == "gl.spread",
           "Patcher inner plugins decoded through the registry");
    const Json* par = nodes && nodes->array.size() == 2 ? nodes->array[1].find("parallel") : nullptr;
    expect(par && par->boolean && m.exact, "Patcher sibling branch runs in parallel");
  }
  {  // Every second-wave name is registered and rejects empty / garbage states gracefully
    Env e;
    const char* names[] = {"Fruity PanOMatic", "Pitcher", "Spreader", "Patcher", "Fruity Blood Overdrive", "LuxeVerb", "Fruity Convolver",
                           "Fruity X-Y Controller", "Fruity X-Y-Z Controller", "Frequency Splitter", "Fruity Fast LP", "Pitch Shifter",
                           "Vintage Chorus", "EQUO", "Fruity Parametric EQ", "Fruity Reeverb", "Emphasis", "Emphasizer", "Frequency Shifter",
                           "Low Lifter", "Distructor", "Fruity Phaser", "Transporter", "Fruity Bass Boost", "Fruity Formula Controller",
                           "Hardcore", "Fruity NoteBook", "Fruity HTML NoteBook", "Fruity Squeeze", "Fruity Vocoder", "Fruity Send",
                           "Vintage Phaser", "Fruity dB Meter", "Fruity Delay", "Fruity Scratcher", "Edison", "Fruity Wrapper"};
    bool all = true;
    for (const char* n : names) all = all && flp::findDecoder(n) != nullptr;
    expect(all, "all second-wave decoders registered");
    std::vector<uint8_t> junk(7, 0xFF);
    for (const char* n : names) {
      flp::PluginMapping m;
      decode(e, n, junk, m);  // must not crash
      decode(e, n, {}, m);
    }
    expect(true, "garbage states do not crash");
  }
  {  // Pitch Shifter mix parameter appended to the Gridline plugin
    PluginState s;
    s.type = "gl.pitch";
    s.params.assign(sp::PsMix, 0.0f);  // old project: no mix value
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(sp::PsCount) && s.params[sp::PsMix] == 1.0f, "old Pitch Shifter states default to full wet");
  }

  // Optional: FL's own render-test projects (one plugin each), when installed.
  const std::string tests = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/System/Render tests/";
  std::error_code ec;
  if (std::filesystem::exists(tests, ec)) {
    for (const char* f : {"Pitcher.flp", "Fruity Convolver.flp", "Pitch Shifter.flp", "Hardcore.flp", "EQUO.flp", "Patcher (effect).flp"}) {
      Song song;
      flp::Report rep;
      std::string err;
      bool ok = importFlp(tests + f, song, rep, &err);
      bool none = true;
      for (const auto& [n, c] : rep.unmapped)
        if (n.find("Wrapper") == std::string::npos && n != "Fruity Peak Controller") none = none && c == 0;
      std::string label = std::string("render test imports with its effect mapped: ") + f;
      expect(ok && none, label.c_str());
    }
  }
  return finish();
}
