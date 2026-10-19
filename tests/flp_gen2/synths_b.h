// Gridline — second-wave instrument decoder tests: synths_b (included by test_flp_gen2.cpp).
// Sakura, Kepler, PoiZone, Sawer, SimSynth, Morphine, Toxic Biohazard.
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
    b.u32(0).u32(0).u32(0).u32(9).str("Kepler-1").u32(0x400).u32(36);
    for (float f : t) b.f32(f);
    b.zeros(13);
    Run r;
    decode(r, "Kepler", b.b);
    expect(r.ok && r.m.glType == "gl.analog", "Kepler → gl.analog");
    expect(!r.m.exact, "Kepler is approximate");
    expect(param(r, 13) == 0 && param(r, 21) == 1, "Kepler: pulse off, saw on");
    expect(near(param(r, 37), 0.5f), "Kepler: sub level");
    expect(near(param(r, 42), 20.0 * std::sqrt(1000.0), 0.5), "Kepler: cutoff curve");
    expect(near(param(r, 43), 0.38f) && near(param(r, 46), -0.6f), "Kepler: resonance, inverted env amount");
    expect(near(param(r, 49), 0.75f) && near(param(r, 53), 0.75f), "Kepler: shared envelope sustain");
    expect(param(r, 95) == 1 && param(r, 2) == 12, "Kepler: chorus on, octave up");
    expect(param(r, 89) == 1 && param(r, 90) == 1 && param(r, 91) == 3, "Kepler: arpeggiator");
    expect(near(param(r, 0), 0.7f), "Kepler: volume");
    expect(r.m.param && r.m.param(19) == 42 && r.m.param(99) == -1, "Kepler: automation index map");
    expect(r.m.value && near(r.m.value(19, 1.0), 20000.0, 1.0), "Kepler: automation value map");
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
    expect(r.ok && r.m.glType == "gl.analog", "Sawer → gl.analog");
    expect(param(r, 10) == 8 && near(param(r, 11), 0.3f), "Sawer: unison voices + detune");
    expect(param(r, 21) == 1 && param(r, 23) == -12, "Sawer: sub saw an octave down");
    expect(param(r, 41) == 5, "Sawer: filter mode high-pass");
    expect(param(r, 95) == 1 && near(param(r, 98), 0.4f) && param(r, 103) == 1 && near(param(r, 106), 0.2f), "Sawer: chorus + reverb");
    expect(near(param(r, 2), 0) && near(param(r, 1), 0), "Sawer: centred transpose / pan");
  }
  // PoiZone: "Ver4" floats.
  {
    std::vector<float> t(81, 0.0f);
    t[0] = 1, t[1] = 0.5f, t[8] = 0.5f, t[10] = 1, t[15] = 0.5f, t[16] = 0.5f, t[17] = 0.75f;
    t[18] = 1, t[24] = 1, t[44] = 1, t[75] = 3;
    Run r;
    decode(r, "PoiZone", sslfState("4reV", t));
    expect(r.ok && r.m.glType == "gl.analog", "PoiZone → gl.analog");
    expect(param(r, 14) == 3 && param(r, 22) == 2, "PoiZone: osc A pulse, osc B saw");
    expect(param(r, 2) == 12 && param(r, 5) == 2, "PoiZone: transpose, mono");
    expect(param(r, 10) == 4 && near(param(r, 46), 1.0f), "PoiZone: unison, env → cutoff");
  }
  // Sakura: "Skr1" floats → string model.
  {
    std::vector<float> t(114, 0.0f);
    t[0] = 0.5f, t[4] = 1, t[6] = 1, t[9] = 0.4f, t[17] = 0.6f, t[111] = 1, t[29] = 1;
    t[74] = 1, t[75] = 0.5f, t[57] = 1;
    Run r;
    decode(r, "Sakura", sslfState("1rkS", t));
    expect(r.ok && r.m.glType == "gl.string", "Sakura → gl.string");
    expect(near(param(r, 5), 12.0f, 0.01) && near(param(r, 6), 0.4f), "Sakura: decay + damping");
    expect(param(r, 10) == 2 && near(param(r, 11), 0.6f), "Sakura: string 2 octave at mix level");
    expect(near(param(r, 38), 0.5f) && param(r, 13) == 1, "Sakura: reverb, resonator body");
    expect(r.m.param && r.m.param(29) == 17, "Sakura: hi-cut automation link");
  }
  // SimSynth: 45 doubles then switches.
  {
    Bytes b;
    for (int i = 0; i < 45; ++i) b.f64(i == 0 ? 1.0 : (i == 6 || i == 13 || i == 20) ? 0.1 : 0.5);
    b.zeros(4 * 30);
    Run r;
    decode(r, "SimSynth", b.b);
    expect(r.ok && r.m.glType == "gl.analog", "SimSynth → gl.analog");
    expect(near(param(r, 17), 0.8f) && param(r, 14) == 2, "SimSynth: osc 1 level and saw shape");
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
    expect(r.ok && r.m.glType == "gl.additive", "Morphine → gl.additive");
    expect(near(param(r, 52), 1.0f) && near(param(r, 55), 1.0f), "Morphine: saw partials stay at full level");
    expect(near(param(r, 60), 1.0f) && near(param(r, 61), 0.0f), "Morphine: loud partial clamps, empty partial is silent");
    expect(param(r, 4) == 32, "Morphine: partial count");
  }
  // Toxic Biohazard + damaged / empty states never fail.
  {
    Run r;
    decode(r, "Toxic Biohazard", {'S', 'S', 'L', 'F', 0, 0, 0, 0, '2', 'i', 'B', 'T'});
    expect(r.ok && r.m.glType == "gl.analog", "Toxic Biohazard → gl.analog");
    bool allOk = true;
    for (const char* n : {"Sakura", "Kepler", "PoiZone", "Sawer", "SimSynth", "Morphine"}) {
      Run e;
      decode(e, n, {1, 2, 3});
      allOk = allOk && e.ok && !e.m.state.params.empty();
    }
    expect(allOk, "synths B: truncated states import with defaults");
  }
}

}  // namespace gen2test
