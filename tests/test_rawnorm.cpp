// Gridline — FLP import: recorded plugin-parameter values (pattern events,
// FL event 223) → FL normalized values through each decoder's rawNorm, on
// synthetic plugin states built here (no FL Studio content). The ranges are
// the plugins' control ranges (control Min..Max, tag = FL parameter).
#include <cmath>
#include <string>
#include <vector>
#include "../src/core/PluginParams.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/models/models.h"
#include "../src/packs/samplers/samplers.h"
#include "../src/packs/spectral/spectral.h"
#include "../src/packs/synths/synths.h"
#include "flp_gen2/Gen2Test.h"

using namespace gen2test;

namespace {

bool isNan(double v) { return std::isnan(v); }

// rawNorm(fl, raw) of a decoded mapping (NaN when the hook is missing).
double rn(const Run& r, int fl, int32_t raw) { return r.m.rawNorm ? r.m.rawNorm(fl, raw) : NAN; }

// FL parameter → Gridline value for a recorded raw value, as the importer
// chains it: rawNorm → value.
double glValue(const Run& r, int fl, int32_t raw) { return r.m.value(fl, r.m.rawNorm(fl, raw)); }

// zlib stream with one stored (uncompressed) deflate block.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& data) {
  Bytes z;
  z.u8(0x78).u8(0x01).u8(0x01);
  z.u16(static_cast<uint16_t>(data.size())).u16(static_cast<uint16_t>(~data.size() & 0xFFFF)).raw(data);
  uint32_t a = 1, b = 0;
  for (uint8_t c : data) a = (a + c) % 65521, b = (b + a) % 65521;
  const uint32_t adler = (b << 16) | a;
  z.u8(static_cast<uint8_t>(adler >> 24)).u8(static_cast<uint8_t>(adler >> 16)).u8(static_cast<uint8_t>(adler >> 8)).u8(static_cast<uint8_t>(adler));
  return z.b;
}

// Record {u32 id, u64 size, payload} (DirectWave).
Bytes& rec(Bytes& b, uint32_t id, const std::vector<uint8_t>& payload) {
  b.u32(id).u32(static_cast<uint32_t>(payload.size())).u32(0).raw(payload);
  return b;
}

void testInstruments() {
  namespace md = gl::packs::models;
  namespace sy = gl::packs::synths;
  namespace smp = gl::packs::samplers;
  {  // Harmless: one dword per control tag; filter frequency 0..8192, key tracking -2000..2000.
    Bytes s;
    s.u32(12);
    for (int t = 0; t < 100; ++t) s.i32(t == 54 ? 4096 : t == 36 ? 1000 : t == 65 ? 1 : 0);
    s.zeros(32 + 1);
    Run r;
    decode(r, "Harmless", s.b);
    expect(r.ok && r.m.rawNorm, "Harmless: rawNorm installed");
    expect(near(rn(r, 54, 4096), 0.5) && near(rn(r, 54, 0), 0.0) && near(rn(r, 54, 8192), 1.0), "Harmless: filter frequency 0..8192");
    expect(near(rn(r, 54, 9000), 1.0) && near(rn(r, 54, -5), 0.0), "Harmless: out-of-range values clamp");
    expect(near(rn(r, 57, 1000), 0.75) && near(rn(r, 38, 0), 0.5) && near(rn(r, 89, 3000), 11000.0 / 16000.0), "Harmless: key tracking, pitch, LFO speed ranges");
    expect(isNan(rn(r, 99, 10)), "Harmless: unmapped parameter → NaN");
    expect(near(glValue(r, 57, 1000), 1.0) && near(glValue(r, 57, 0), 0.0), "Harmless: key tracking value (1000 = full tracking)");
    expect(near(glValue(r, 54, 4096), r.m.state.params[static_cast<size_t>(md::AdFltCutoff)], 0.5), "Harmless: recorded cutoff = decoded cutoff for the same raw value");
  }
  {  // Fruit Kick: max freq -2400..3600 cents, decay 0..256.
    Bytes s;
    s.u32(0).i32(0).i32(-600).i32(64).i32(128).i32(32).i32(0).u32(1);
    Run r;
    decode(r, "Fruit Kick", s.b);
    expect(r.ok && r.m.param(3) == 3, "Fruit Kick: decay linked (Sweep Kick index = tag)");
    expect(near(rn(r, 0, 0), 0.4) && near(rn(r, 3, 128), 0.5) && near(rn(r, 4, 64), 1.0), "Fruit Kick: control ranges");
    expect(near(glValue(r, 0, 0), r.m.state.params[0], 0.01), "Fruit Kick: recorded max freq = decoded value");
    expect(near(glValue(r, 3, 128), r.m.state.params[3], 0.001), "Fruit Kick: recorded decay = decoded value");
  }
  {  // Sytrus: Mod X / Y 0..512 drive the approximation around the saved patch.
    std::vector<uint8_t> raw(256, 0);
    raw[0] = 128;  // master level
    Bytes s;
    s.u32(0x00030003).raw(zlibStored(raw));
    Run r;
    decode(r, "Sytrus", s.b);
    expect(r.ok && r.m.glType == "gl.fm", "Sytrus: FM approximation");
    expect(r.m.param && r.m.param(18) == gl::pp::fmParam(1, gl::pp::FmLevel) && r.m.param(19) == gl::pp::FmFeedback && r.m.param(225) == -1,
           "Sytrus: Mod X → modulator level, Mod Y → feedback");
    expect(near(rn(r, 18, 256), 0.5) && near(rn(r, 19, 512), 1.0) && isNan(rn(r, 225, 0)), "Sytrus: Mod X / Y range 0..512");
    const float lvl = r.m.state.params[static_cast<size_t>(gl::pp::fmParam(1, gl::pp::FmLevel))];
    expect(near(glValue(r, 18, 256), lvl) && near(glValue(r, 18, 0), 0.0), "Sytrus: Mod X centre keeps the saved modulator level");
  }
  {  // 3x Osc is covered through the shared ParamTable; DirectWave: zone amp envelope 32..35.
    Bytes zone;
    std::vector<uint8_t> z500(17, 0);
    z500[0] = 60, z500[2] = 127, z500[4] = 127;
    rec(zone, 500, z500);
    rec(zone, 502, std::vector<uint8_t>{'/', 'x', '.', 'w', 'a', 'v', 0});
    Bytes prog;
    rec(prog, 0x66, std::vector<uint8_t>{'P', 0});
    rec(prog, 3, zone.b);
    Bytes s;
    s.u32(1);
    rec(s, 1, prog.b);
    Run r;
    decode(r, "DirectWave", s.b);
    // J-33: DirectWave → Wave Program reads the chunked bank (versions 36..38);
    // the legacy raw-struct format (< 36, never shipped) is refused.
    expect(!r.ok, "DirectWave: legacy (< 36) state refused");
    Bytes s38;
    s38.u32(38);
    std::vector<uint8_t> hdr(30, 0);
    Bytes prog38;
    rec(prog38, 100, hdr);
    rec(prog38, 3, zone.b);
    rec(s38, 1, prog38.b);
    Run r38;
    decode(r38, "DirectWave", s38.b);
    expect(r38.ok && r38.m.glType == "gl.waveprogram", "DirectWave: decoded");
    expect(r38.m.param(0) == 0 && r38.m.param(15) == 15, "DirectWave: program tags 1:1");
  }
  {  // Kepler (second wave): sliders 0..6000, volume 0..65536.
    std::vector<float> t(36, 0.0f);
    Bytes b;
    b.u32(0).u32(0).u32(0).u32(9).str("Kepler-1").u32(4).u32(36);
    for (float f : t) b.f32(f);
    b.zeros(12);
    b.b[4] = static_cast<uint8_t>(b.b.size() - 12);
    Run r;
    decode(r, "Kepler", b.b);
    expect(r.ok && near(rn(r, 19, 3000), 0.5) && near(rn(r, 33, 65536), 1.0) && isNan(rn(r, 99, 1)), "Kepler: control ranges");
    // J-22: DCO Synth keeps the reference's 0..1 positions.
    expect(near(glValue(r, 19, 6000), 1.0, 1e-6), "Kepler: recorded VCF frequency → cutoff position");
  }
  {  // Keyboard Controller: smoothing 0..256.
    Bytes s;
    s.u32(2).i32(0).i32(0);
    for (int k = 0; k < 132; ++k) s.f32(0);
    s.i32(60).i32(67).i32(0).u8(1);
    Run r;
    decode(r, "Fruity Keyboard Controller", s.b);
    expect(r.ok && near(rn(r, 0, 128), 0.5) && near(rn(r, 1, 256), 1.0), "Keyboard Controller: smoothing ranges");
  }
  (void)sy::AnCutoff;
}

void testEffects() {
  namespace sp = gl::packs::spectral;
  namespace dy = gl::packs::dynamics;
  {  // Vocodex: release 0..10000, band width 5..1000, modulator pitch -1200..1200 (the
     // three parameters recorded in FL's supplied projects).
    Run r;
    decode(r, "Vocodex", std::vector<uint8_t>(8, 0));
    expect(r.ok && r.m.rawNorm, "Vocodex: rawNorm installed (also for unrecognised states)");
    expect(near(rn(r, 11, 3500), 0.35) && near(rn(r, 14, 5), 0.0) && near(rn(r, 14, 1000), 1.0) && near(rn(r, 17, 0), 0.5),
           "Vocodex: release, band width, modulator pitch ranges");
    expect(near(glValue(r, 17, -1200), -12.0) && near(glValue(r, 17, 600), 6.0), "Vocodex: recorded pitch shift → semitones");
    expect(near(glValue(r, 14, 349), std::pow(5.0, 0.349) - 1.0, 1e-4) && r.m.param(14) == sp::VxWidth,
           "Vocodex: recorded band width → reference band width (5^(v/1000) - 1)");
    expect(r.m.param(17) == sp::VcFormant, "Vocodex: pitch shift linked");
  }
  {  // Parametric EQ 2: band types 0..7, levels -1800..1800.
    Bytes s;
    s.u32(1);
    for (int t = 0; t < 36; ++t) s.i32(t >= 21 && t < 28 ? 6 : (t >= 7 && t < 14 ? 30000 : (t >= 14 && t < 21 ? 17500 : 0)));
    Run r;
    decode(r, "Fruity Parametric EQ 2", s.b);
    expect(r.ok && near(rn(r, 23, 7), 1.0) && near(rn(r, 0, 0), 0.5) && near(rn(r, 7, 65536), 1.0), "Parametric EQ 2: ranges");
    expect(near(glValue(r, 0, 600), 6.0), "Parametric EQ 2: recorded level → dB");
  }
  {  // Gross Beat: slot selectors 0..35.
    Run r;
    decode(r, "Gross Beat", std::vector<uint8_t>(8, 0));
    expect(r.ok && near(rn(r, 0, 35), 1.0) && near(rn(r, 2, 128), 0.5), "Gross Beat: slot / mix ranges");
    expect(near(glValue(r, 0, 3), 4.0), "Gross Beat: recorded slot 3 → slot 4 (1-based)");
  }
  {  // Effector: the table works in stored floats; recorded knobs are 0..256.
    Run r;
    decode(r, "Effector", std::vector<uint8_t>(8, 0));
    // J-26: tempo reduce is a 0..256 knob too; rate 0..8, shape 0..2.
    expect(r.ok && near(rn(r, 3, 128), 0.5) && near(rn(r, 0, 11), 1.0) && near(rn(r, 6, 64), 0.25) && near(rn(r, 5, 4), 0.5) && near(rn(r, 9, 2), 1.0),
           "Effector: knob / kind / rate / shape ranges");
    expect(near(glValue(r, 3, 64), 0.25) && r.m.param(3) == dy::EfRawX, "Effector: recorded X (reference engine value)");
  }
  {  // Control Surface: user-defined controls, no known range.
    Run r;
    decode(r, "Control Surface", std::vector<uint8_t>(4, 0));
    expect(r.ok && !r.m.rawNorm, "Control Surface: no rawNorm");
  }
}

}  // namespace

int main() {
  testInstruments();
  testEffects();
  return finish();
}
