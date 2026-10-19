// Gridline — second-wave instrument decoder tests: pads (included by test_flp_gen2.cpp).
#pragma once
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Slicing.h"
#include "../../src/packs/models/drums.h"
#include "../../src/packs/models/models.h"
#include "Gen2Test.h"

namespace gen2test {

namespace padtest {

// FPC chunk: u32 id, u64 size, payload.
inline void chunk(Bytes& out, uint32_t id, const Bytes& payload) {
  out.u32(id).u32(static_cast<uint32_t>(payload.size())).u32(0).raw(payload.b);
}

struct FpcLayerSpec {
  int vol, pan, lo, hi, tune;
  std::string path;
};

inline Bytes fpcPad(int vol, int pan, int note, int group, int offBy, int tune, int trigger, const std::vector<FpcLayerSpec>& layers) {
  Bytes pad, settings, name, opts;
  settings.i32(vol).i32(pan).i32(0).i32(note).i32(0).i32(group).i32(offBy).i32(tune).zeros(32);
  chunk(pad, 0x64, settings);
  name.utf16("Pad", false);
  chunk(pad, 0x65, name);
  opts.i32(trigger).zeros(28);
  chunk(pad, 0x66, opts);
  for (const auto& l : layers) {
    Bytes layer, path, ls;
    path.utf16(l.path, false);
    chunk(layer, 0x12C, path);
    ls.i32(l.vol).i32(l.pan).i32(l.lo).i32(l.hi).i32(1).i32(l.tune).i32(-1);
    chunk(layer, 0x12D, ls);
    chunk(layer, 0x12E, Bytes());
    chunk(pad, 0x69, layer);
  }
  return pad;
}

inline Bytes sslf(const char* id, size_t padTo) {
  Bytes b;
  b.str("SSLF", false).u32(0).str(id, false).str("Preset", true);
  b.pad(padTo);
  return b;
}

}  // namespace padtest

inline void testPads() {
  using namespace gl;
  using namespace gl::pp;
  using namespace padtest;
  namespace md = gl::packs::models;

  // ---------------------------------------------------------------- FPC
  {
    Bytes st;
    st.u32(0x1E84E403);
    chunk(st, 2, fpcPad(127, -64, 40, 0, -1, 64, 3,
                        {{127, 0, 0, 63, 0, "%FLStudioData%\\Patches\\kit\\soft.wav"},
                         {64, 32, 64, 127, -32, "%FLStudioFactoryData%\\Patches\\kit\\hard.wav"}}));
    chunk(st, 2, fpcPad(128, 0, 41, -1, -1, 0, 1, {{99, 0, 0, 127, 0, ""}}));  // empty pad
    chunk(st, 2, fpcPad(100, 0, 42, -1, 0, 0, 1, {{99, 0, 0, 127, 0, "%FLStudioData%\\open.wav"}}));
    Run r;
    decode(r, "FPC", st.b);
    expect(r.ok && r.m.glType == "gl.pads", "fpc: decodes to drum pads");
    expect(r.m.exact, "fpc: small kit maps exactly");
    expect(near(param(r, padParam(0, PadLevel)), 1.0) && near(param(r, padParam(0, PadPan)), -0.5), "fpc: pad volume and pan");
    expect(near(param(r, padParam(0, PadTune)), 6.0), "fpc: pad tune 64 = +600 cents");
    expect(near(param(r, padExt(0, PadNote)), 40) && near(param(r, padExt(2, PadNote)), 42), "fpc: pad notes, pads keep their bank position");
    expect(near(param(r, padExt(0, PadLayerMode)), LayerRandom), "fpc: random trigger option");
    expect(near(param(r, padExt(2, PadLayerMode)), LayerStack), "fpc: plain pads stack layers");
    expect(near(param(r, padLayer(0, 1, LayEnabled)), 1) && near(param(r, padLayer(0, 1, LayVelLow)), 64) &&
               near(param(r, padLayer(0, 1, LayLevel)), 64 / 127.0) && near(param(r, padLayer(0, 1, LayPan)), 0.25) &&
               near(param(r, padLayer(0, 1, LayTune)), -3.0),
           "fpc: layer velocity range, level, pan, tune");
    expect(near(param(r, padLayer(0, 0, LayVelHigh)), 63), "fpc: first layer velocity range");
    expect(padSamplePath(r.m.state.data, 0, 0) == "/FLDATA/Data/Patches/kit/soft.wav", "fpc: %FLStudioData% sample path");
    expect(padSamplePath(r.m.state.data, 0, 1) == "/FLDATA/Patches/kit/hard.wav", "fpc: factory data layer path");
    expect(padSamplePath(r.m.state.data, 2, 0) == "/FLDATA/Data/open.wav", "fpc: third pad");
    expect(padSamplePath(r.m.state.data, 1, 0) == "none" && padSamplePath(r.m.state.data, 5, 0) == "none",
           "fpc: unused pads are empty (no built-in kit)");
    // Cut groups: pad 1 is in FL group 0 → silences Gridline group 1; pad 3 is
    // silenced by FL group 0 → belongs to Gridline group 1.
    expect(near(param(r, padExt(0, PadCutBy)), 1) && near(param(r, padParam(0, PadCut)), 0), "fpc: cut group → chokes");
    expect(near(param(r, padParam(2, PadCut)), 1) && near(param(r, padExt(2, PadCutBy)), 0), "fpc: off-by group → choked");
    expect(r.m.param && r.m.param(0) == padParam(0, PadLevel) && r.m.param(64 + 2) == padParam(2, PadTune) && r.m.param(1) == padParam(1, PadLevel) &&
               r.m.param(90) == -1,
           "fpc: automation targets follow the pads");
    expect(r.m.value && near(r.m.value(64, 1.0), 12.0) && near(r.m.value(16, 0.0), -1.0), "fpc: automation values");
  }
  {  // Layers 5..8 live in the data text (exact).
    Bytes st;
    st.u32(0x1E84E403);
    std::vector<FpcLayerSpec> ls;
    for (int k = 0; k < 8; ++k) ls.push_back({127, 0, k * 16, k * 16 + 15, 0, "/x/l" + std::to_string(k) + ".wav"});
    chunk(st, 2, fpcPad(127, 0, 36, -1, -1, 0, 1, ls));
    Run r;
    decode(r, "FPC", st.b);
    expect(r.ok && r.m.exact, "fpc: eight layers decode exactly");
    PadLayerX x8;
    expect(near(param(r, padLayer(0, 3, LayVelLow)), 48) && near(param(r, padLayer(0, 3, LayVelHigh)), 63) &&
               padSamplePath(r.m.state.data, 0, 3) == "/x/l3.wav" && padSamplePath(r.m.state.data, 0, 7) == "/x/l7.wav" &&
               parsePadLayer(padDataGet(r.m.state.data, "S1.8"), x8) && x8.lo == 112 && x8.hi == 127 && padLayerCount(r.m.state.data, 0) == 8,
           "fpc: layers keep their velocity ranges (5..8 in the data text)");
  }
  {
    Run r;
    decode(r, "FPC", Bytes().u32(0x12345678).zeros(40).b);
    expect(!r.ok, "fpc: unknown state is rejected");
  }

  // ------------------------------------------------------------ Drumpad (J-22: Membrane Pad)
  {
    namespace dr = gl::packs::models::dr;
    Bytes st = sslf("3PMD", 0x8C);
    for (int k = 0; k < 38; ++k) st.f32(k == 11 ? 0.2f : (k == 30 ? 1.0f : (k > 30 && k < 34 ? 0.0f : 0.5f)));
    st.f32(0.7f).u32(0);  // newer saves: output level, then a word after the chunk
    const uint32_t chunk = static_cast<uint32_t>(st.b.size() - 12);
    for (int i = 0; i < 4; ++i) st.b[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(chunk >> (8 * i));
    for (const char* name : {"Drumpad", "DrumPad"}) {
      Run r;
      decode(r, name, st.b);
      expect(r.ok && r.m.glType == "gl.membranepad" && r.m.exact, "drumpad: decodes exactly to the membrane pad");
      expect(near(param(r, dr::MdSize), 0.2) && near(param(r, dr::MdVelSrc1), 1) && near(param(r, dr::PdOutput), 0.7), "drumpad: tags 1:1 + output");
      expect(r.m.param && r.m.param(11) == 11 && r.m.param(29) == 29, "drumpad: automation index = tag");
    }
    Run bad;
    decode(bad, "Drumpad", Bytes().str("XXXX").b);
    expect(!bad.ok, "drumpad: short state is rejected");
  }

  // ------------------------------------------------------------ Drumaxx (J-22: Membrane Kit)
  {
    namespace dr = gl::packs::models::dr;
    Bytes st = sslf("3SMD", 0x10C);
    for (int p = 0; p < 16; ++p) {
      st.f32(p == 2 ? 0.25f : 1.0f).f32(0.5f).f32(0).f32(0).f32(static_cast<float>(36 + p));
      for (int k = 0; k < 38; ++k) st.f32(k >= 30 && k < 34 ? 0.0f : 0.5f);
      st.f32(0);
    }
    for (int k = 0; k < 16; ++k) st.f32(k == 1 ? 0.6f : 0.0f);
    for (int k = 0; k < 16; ++k) {
      st.u8(k == 0 ? 15 : 0x7F).u8(k == 0 ? 60 : 0x7F);
      for (int c = 0; c < 16 * 64; ++c) st.u8(c == 4 ? 0xFF : 0x7F);
    }
    for (int p = 0; p < 16; ++p) {
      Bytes nm;
      nm.str(p == 0 ? "Kick" : "...", true);
      nm.b.resize(128, 0);
      st.raw(nm.b);
    }
    st.zeros(32);
    const uint32_t size = static_cast<uint32_t>(st.b.size() - 8);
    for (int i = 0; i < 4; ++i) st.b[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
    Run r;
    decode(r, "Drumaxx", st.b);
    expect(r.ok && r.m.glType == "gl.membranekit" && r.m.exact, "drumaxx: decodes exactly to the membrane kit");
    expect(near(param(r, dr::kitPad(2, dr::KpVolume)), 0.25) && near(param(r, dr::kitPad(5, dr::KpKey)), 41) && near(param(r, dr::KmLevel), 0.6),
           "drumaxx: channel volume / key, master level at their tags");
    dr::KitData d;
    expect(dr::decodeKitData(r.m.state.data, d) && d.pattern[0].length == 16 && d.pattern[0].key == 60 && d.pattern[0].cell[0][4] == 0xFF &&
               d.padName[0] == "Kick",
           "drumaxx: patterns and channel names kept");
    Run bad;
    decode(bad, "Drumaxx", std::vector<uint8_t>(st.b.begin(), st.b.end() - 1));
    expect(!bad.ok, "drumaxx: truncated state is rejected");
  }

  // ------------------------------------------------ Fruity DrumSynth Live
  {
    Bytes st;
    st.u32(4).u32(62);
    for (int k = 0; k < 120; ++k) st.u8(1).str("P", false);
    for (int k = 0; k < 120; ++k) {
      int v[38] = {};
      v[0] = 128, v[4] = 300, v[7] = 64, v[8] = 127, v[14] = 0, v[24] = 0;
      if (k == 62) v[1] = 32;
      for (int i = 0; i < 38; ++i) st.i32(v[i]);
    }
    st.u32(1);
    Run r;
    decode(r, "Fruity DrumSynth Live", st.b);
    expect(r.ok && r.m.glType == "gl.drumkit", "drumsynth live: decodes to the drum model");
    expect(near(param(r, md::DmBaseNote), 62), "drumsynth live: base note = selected key without notes");
    expect(near(param(r, md::drumPad(0, md::DpFreq)), 160.0, 0.5) && near(param(r, md::drumPad(0, md::DpPan)), 0.5),
           "drumsynth live: oscillator pitch and pan");
    Run bad;
    decode(bad, "Fruity DrumSynth Live", Bytes().u32(4).u32(60).u8(3).b);
    expect(!bad.ok, "drumsynth live: truncated state is rejected");
  }

  // ------------------------------------------------------------ BassDrum (J-22: Kick Sculptor)
  {
    namespace dr = gl::packs::models::dr;
    Bytes st = sslf("2kiK", 0x10C);
    st.str("%FLStudioFactoryData%/Plugins/Fruity/Generators/BassDrum/Noise Samples/Metallic.wav", true);
    st.pad(0x50C);
    for (int k = 0; k < 23; ++k) st.f32(k == 2 ? 0.5f : 0.25f);
    const uint32_t size = static_cast<uint32_t>(st.b.size() - 8);
    for (int i = 0; i < 4; ++i) st.b[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
    Run r;
    decode(r, "BassDrum", st.b);
    expect(r.ok && r.m.glType == "gl.kicksculpt" && r.m.exact, "bassdrum: decodes exactly to the kick sculptor");
    expect(near(param(r, dr::KsDuration), 0.5) && near(param(r, dr::KsNoiseSample), 6), "bassdrum: duration tag + noise sample");
    expect(r.m.param && r.m.param(2) == 2 && r.m.param(0) == 0, "bassdrum: automation index = tag");
    Run bad;
    decode(bad, "BassDrum", Bytes().str("SSLF", false).zeros(16).b);
    expect(!bad.ok, "bassdrum: short state is rejected");
  }
}

}  // namespace gen2test
