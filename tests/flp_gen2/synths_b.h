// Gridline — second-wave instrument decoder tests: synths_b (included by test_flp_gen2.cpp).
// Sakura, Kepler, PoiZone, Sawer, SimSynth, Morphine (J-22: Spectra Morph), Toxic Biohazard.
#pragma once
#include "Gen2Test.h"

namespace gen2test {

// "SSLF" chunk with a 4-char id and one float per control tag.
inline std::vector<uint8_t> sslfState(const char* id, const std::vector<float>& tags) {
  Bytes b;
  b.str("SSLF", false).u32(static_cast<uint32_t>(4 + 4 * tags.size())).str(id, false);
  for (float f : tags) b.f32(f);
  b.str("<noname>").zeros(16);
  return b.b;
}

inline void testSynthsB() {
  for (const char* n : {"Sakura", "Kepler", "PoiZone", "Sawer", "SimSynth", "Morphine", "Toxic Biohazard"})
    expect(registered(n), (std::string("synths B: decoder registered for ") + n).c_str());

  // Kepler: header, name, version, count, 36 floats in tag order.
  {
    std::vector<float> t(36, 0.0f);
    t[13] = 0, t[14] = 1, t[15] = 1, t[16] = 0.5f;  // saw on, sub on at half level
    t[19] = 0.5f, t[20] = 0.4f, t[22] = 0.6f, t[21] = 1;  // cutoff, reso, inverted env
    t[26] = 0, t[27] = 0.5f, t[28] = 0.75f, t[29] = 0.25f;
    t[30] = 1.0f / 3.0f, t[32] = 1, t[33] = 0.5f;  // chorus I, octave up, volume
    t[0] = 1, t[1] = 0.5f, t[2] = 1;               // arp on, down, 3 octaves
    Bytes b;
    b.u32(0).u32(0).u32(0).u32(9).str("Kepler-1").u32(4).u32(36);
    for (float f : t) b.f32(f);
    b.zeros(12);
    b.b[4] = static_cast<uint8_t>(b.b.size() - 12);  // payload size
    Run r;
    decode(r, "Kepler", b.b);
    // J-22: Kepler → DCO Synth (one-DCO panel; tags through the classic table).
    expect(r.ok && r.m.glType == "gl.dcosynth", "Kepler → gl.dcosynth");
    expect(r.m.exact, "Kepler maps exactly");
    expect(param(r, 7) == 0 && param(r, 8) == 1 && param(r, 9) == 1 && near(param(r, 10), 0.5f), "Kepler: DCO pulse / saw / sub");
    expect(near(param(r, 37), 0.5f) && near(param(r, 38), 0.4f) && param(r, 43) == 1 && near(param(r, 44), 0.6f),
           "Kepler: VCF cutoff, resonance, inverted env amount");
    expect(near(param(r, 63), 0.75f) && param(r, 69) == 1 && param(r, 83) == 2, "Kepler: envelope, chorus I, octave up");
    expect(param(r, 85) == 1 && param(r, 88) == 2 && near(param(r, 72), 0.5f), "Kepler: arpeggiator, volume");
    expect(r.m.param && r.m.param(19) == 37 && r.m.param(99) == -1, "Kepler: automation index map");
    expect(r.m.value && near(r.m.value(19, 1.0), 1.0), "Kepler: automation value map");
  }
  // Sawer: "Swr1" floats.
  {
    std::vector<float> t(96, 0.0f);
    t[0] = 0.5f, t[1] = 0.5f, t[4] = 1, t[6] = 0.5f, t[7] = 0.5f;
    t[14] = 1, t[15] = 0.3f, t[19] = 0.5f, t[18] = 0.125f;  // 8 unison voices, sub at one octave down
    t[27] = 0.25f, t[28] = 0.2f, t[31] = 2.0f / 3.0f;       // high-pass
    t[47] = 1, t[50] = 0.4f, t[65] = 1, t[69] = 0.2f;
    Run r;
    decode(r, "Sawer", sslfState("1rwS", t));
    // J-22: Sawer → Saw Swarm (parameter index = control tag).
    expect(r.ok && r.m.glType == "gl.sawswarm", "Sawer → gl.sawswarm");
    expect(param(r, 14) == 3 && near(param(r, 15), 0.3f), "Sawer: unison 8 voices + detune");
    expect(param(r, 18) == 1 && near(param(r, 19), 0.5f), "Sawer: sub-saw octave step (-1.5 oct) + level");
    expect(param(r, 31) == 2, "Sawer: filter mode (third item, BP)");
    expect(param(r, 47) == 1 && near(param(r, 50), 0.4f) && param(r, 65) == 1 && near(param(r, 69), 0.2f), "Sawer: chorus + reverb");
    expect(near(param(r, 6), 0) && near(param(r, 1), 0), "Sawer: centred transpose / pan");
  }
  // PoiZone: "Ver4" floats.
  {
    std::vector<float> t(81, 0.0f);
    t[0] = 1, t[1] = 0.5f, t[8] = 0.5f, t[10] = 1, t[15] = 0.5f, t[16] = 0.5f, t[17] = 0.75f;
    t[18] = 1, t[24] = 1, t[44] = 1, t[75] = 3;
    Run r;
    decode(r, "PoiZone", sslfState("4reV", t));
    // J-22: PoiZone → Twin VA (parameter index = control tag).
    expect(r.ok && r.m.glType == "gl.twinva", "PoiZone → gl.twinva");
    expect(param(r, 0) == 1 && param(r, 4) == 0, "PoiZone: osc A pulse, osc B saw");
    expect(near(param(r, 17), 12) && param(r, 44) == 1, "PoiZone: transpose, mono");
    expect(param(r, 75) == 3 && near(param(r, 10), 1.0f) && near(param(r, 18), 1.0f), "PoiZone: unison, env amount, cutoff");
  }
  // Sakura: "Skr1" floats → Twin String (J-22: parameter index = control tag;
  // full coverage in tests/packs/test_strings.cpp).
  {
    std::vector<float> t(114, 0.0f);
    t[0] = 0.5f, t[4] = 1, t[6] = 1, t[9] = 0.4f, t[17] = 0.6f, t[111] = 1, t[29] = 1;
    t[74] = 1, t[75] = 0.5f, t[57] = 1, t[113] = 0.5f;
    Bytes b;
    b.str("SSLF", false).u32(588).str("1rkS", false);
    for (float f : t) b.f32(f);
    b.str("<noname>").zeros(119);
    Run r;
    decode(r, "Sakura", b.b);
    expect(r.ok && r.m.glType == "gl.twinstring" && r.m.exact, "Sakura → gl.twinstring (exact)");
    expect(near(param(r, 6), 1.0f) && near(param(r, 9), 0.4f), "Sakura: string decay + damping");
    expect(param(r, 111) == 1 && near(param(r, 17), 0.6f), "Sakura: string 2 ratio + mix");
    expect(near(param(r, 75), 0.5f) && param(r, 74) == 1 && param(r, 57) == 1 && param(r, 113) == 3, "Sakura: reverb, resonator, octave");
    expect(r.m.param && r.m.param(29) == 29, "Sakura: hi-cut automation link");
  }
  // SimSynth: 45 doubles then switches.
  {
    Bytes b;
    for (int i = 0; i < 45; ++i) b.f64(i == 3 ? 1.0 : (i == 6 || i == 13 || i == 20) ? 0.1 : 0.5);
    b.zeros(4 * 50).i32(14);
    Run r;
    decode(r, "SimSynth", b.b);
    // J-22: SimSynth → Warm Trio (parameter index = control tag).
    expect(r.ok && r.m.glType == "gl.warmsynth", "SimSynth → gl.warmsynth");
    expect(near(param(r, 0), 1.0f) && param(r, 54) == 1, "SimSynth: osc 1 level and saw shape");
  }
  // Morphine: first "MBP0" spectrum frame (mask + floats) → additive partials.
  {
    Bytes b;
    b.str("SSLF", false).u32(0).str("0roM", false).str("0PBM", false);
    b.u8(0xFF);
    for (int k = 1; k <= 8; ++k) b.f32(1.0f / static_cast<float>(k));  // saw spectrum
    b.u8(0x01).f32(0.5f);  // partial 9 loud, 10-16 silent
    b.u8(0x00).u8(0x00);
    Run r;
    decode(r, "Morphine", b.b);
    // J-22: Morphine → Spectra Morph (full program walk, tests/packs/test_metal.cpp);
    // a lone spectrum frame is not a program and is refused.
    expect(!r.ok, "Morphine: a fragment without programs is refused");
  }
  // Toxic Biohazard + damaged / empty states never fail.
  {
    Run r;
    decode(r, "Toxic Biohazard", {'S', 'S', 'L', 'F', 0, 0, 0, 0, '2', 'i', 'B', 'T'});
    expect(r.ok && r.m.glType == "gl.analog", "Toxic Biohazard → gl.analog");
    bool allOk = true;
    // (Kepler / SimSynth refuse truncated states since J-22: tests/packs/test_dco.cpp;
    //  Sakura likewise: tests/packs/test_strings.cpp.)
    for (const char* n : {"PoiZone", "Sawer"}) {
      Run e;
      decode(e, n, {1, 2, 3});
      allOk = allOk && e.ok && !e.m.state.params.empty();
    }
    expect(allOk, "synths B: truncated states import with defaults");
  }
}

}  // namespace gen2test
