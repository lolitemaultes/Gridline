// Gridline — third-wave tests: older state forms of earlier decoders, as kept
// in FL's shipped presets (included by test_flp_dec3.cpp).
#pragma once
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Slicing.h"
#include "../../src/packs/models/models.h"
#include "../flp_gen2/Gen2Test.h"

namespace dec3test {

using namespace gen2test;

namespace fixtest {

inline void chunk(Bytes& out, uint32_t id, const Bytes& payload) {
  out.u32(id).u32(static_cast<uint32_t>(payload.size())).u32(0).raw(payload.b);
}

}  // namespace fixtest

inline void testFixes() {
  using namespace gl;
  using namespace gl::pp;
  using namespace fixtest;
  namespace md = gl::packs::models;

  // ------------------------------------------- FPC, FL 11 kit (8-bit strings)
  {
    Bytes st, pad, settings, name, layer, path, ls;
    st.u32(0x000F424E);
    settings.i32(127).i32(0).i32(0).i32(38).i32(0).i32(-1).i32(-1).i32(0).zeros(32);
    chunk(pad, 0x64, settings);
    name.str("Snare", false);
    chunk(pad, 0x65, name);
    path.str("%FLStudioData%\\Patches\\Packs\\kit\\snare.wav", false);
    chunk(layer, 0x12C, path);
    ls.i32(100).i32(0).i32(0).i32(127).i32(1).i32(0).i32(-1);
    chunk(layer, 0x12D, ls);
    chunk(pad, 0x69, layer);
    chunk(st, 2, pad);
    Run r;
    decode(r, "FPC", st.b);
    expect(r.ok && r.m.glType == "gl.pads", "fpc (FL 11): old magic accepted");
    expect(padSamplePath(r.m.state.data, 0, 0) == "/FLDATA/Data/Patches/Packs/kit/snare.wav", "fpc (FL 11): 8-bit sample path");
    expect(near(param(r, padExt(0, PadNote)), 38) && near(param(r, padParam(0, PadLevel)), 1.0), "fpc (FL 11): pad note and level");
    Bytes v10 = st;
    v10.b[0] = 0x4B;  // FL 10 magic 0x000F424B
    Run r10;
    decode(r10, "FPC", v10.b);
    expect(r10.ok && padSamplePath(r10.m.state.data, 0, 0) == "/FLDATA/Data/Patches/Packs/kit/snare.wav", "fpc (FL 10): old magic accepted");
  }

  // ------------------------------------ Fruity DrumSynth Live, version 1 (24 patches)
  {
    Bytes st;
    st.u32(1).u32(6);
    for (int k = 0; k < 24; ++k) st.str("Patch", false).zeros(19);
    for (int k = 0; k < 24; ++k) {
      int v[38] = {};
      v[0] = 128, v[4] = 300 + k, v[7] = 60, v[8] = 100;
      for (int i = 0; i < 38; ++i) st.i32(v[i]);
    }
    Run r;
    decode(r, "Fruity DrumSynth Live", st.b);
    expect(r.ok && r.m.glType == "gl.drumkit", "drumsynth v1: 24-patch state decodes");
    expect(near(param(r, md::DmBaseNote), 60), "drumsynth v1: patches start at C5");
    expect(near(param(r, md::drumPad(0, md::DpMute)), 0) && near(param(r, md::drumPad(15, md::DpMute)), 0), "drumsynth v1: 16 pads sound");
    double f0 = param(r, md::drumPad(0, md::DpFreq)), f1 = param(r, md::drumPad(1, md::DpFreq));
    expect(f1 > f0, "drumsynth v1: patch order kept");
    Run bad;
    decode(bad, "Fruity DrumSynth Live", Bytes().u32(1).u32(0).zeros(100).b);
    expect(!bad.ok, "drumsynth v1: truncated state rejected");
  }

  // --------------------------------------------- 3x Osc, FL 2 form (11 values)
  {
    Bytes st;
    st.i32(2).i32(0).i32(3);         // osc 1: square, 0 st, +3 ct
    st.i32(64).i32(3).i32(-12).i32(0);  // osc 2: mix 64, saw, -12 st
    st.i32(32).i32(0).i32(-24).i32(0);  // osc 3: mix 32, sine, -24 st
    Run r;
    decode(r, "3x Osc", st.b);
    expect(r.ok && r.m.glType == "gl.synth", "3x osc (FL 2): decodes");
    expect(near(param(r, SynOsc1Shape), 3) && near(param(r, SynOsc2Shape), 2) && near(param(r, SynOsc3Shape), 0), "3x osc (FL 2): shapes");
    expect(near(param(r, SynOsc1Fine), 3) && near(param(r, SynOsc2Coarse), -12) && near(param(r, SynOsc3Coarse), -24), "3x osc (FL 2): tuning");
    expect(near(param(r, SynOsc2Level), 0.5) && near(param(r, SynOsc3Level), 0.25), "3x osc (FL 2): mix levels");
  }

  // -------------------------------------- Fruity Chorus / Flanger, FL 3 float form
  {
    Bytes st;
    for (float f : {0.5f, 0.45f, 0.33f, 0.3f, 0.5f, 0.7f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.76f}) st.f32(f);
    Run r;
    decode(r, "Fruity Chorus", st.b);
    expect(r.ok && r.m.glType == "gl.chorus", "chorus (FL 3): float state decodes");
    expect(near(param(r, ChoDepthMs), 2.25) && near(param(r, ChoWetOnly), 1), "chorus (FL 3): depth and wet-only");
    expect(near(param(r, ChoDelay), 0.05 + 29.95 * 0.5, 0.05), "chorus (FL 3): delay scaled to the integer range");
  }
  {
    Bytes st;
    for (float f : {0.0f, 0.4f, 0.4f, 0.33f, 0.0f, 0.0f, 0.24f, 1.0f, 1.0f, 0.0f, 1.0f, 0.5f}) st.f32(f);
    Run r;
    decode(r, "Fruity Flanger", st.b);
    expect(r.ok && r.m.glType == "gl.flanger", "flanger (FL 3): float state decodes");
    expect(param(r, FlgFeedback) < 0 && near(param(r, FlgMix), 1.0) && near(param(r, FlgInvertWet), 1), "flanger (FL 3): inverted feedback, wet only");
  }
}

}  // namespace dec3test
