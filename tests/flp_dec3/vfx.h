// Gridline — third-wave decoder tests: VFX note effects (included by test_flp_dec3.cpp).
#pragma once
#include <cmath>
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/modulation/modulation_dsp.h"
#include "../flp_gen2/Gen2Test.h"

namespace dec3test {

using namespace gen2test;

namespace vfxtest {

namespace mod = gl::packs::modulation;

// Key mapper state: header then one target list per key of the octave.
inline Bytes keyMapper(int enable, int base, int offset, int mode, int slide, const std::vector<std::vector<int>>& recs) {
  Bytes b;
  b.i32(2).i32(enable).i32(base).i32(offset).i32(mode).i32(slide);
  for (const auto& r : recs) {
    b.u32(static_cast<uint32_t>(r.size()));
    for (int t : r) b.u16(static_cast<uint16_t>(static_cast<int16_t>(t)));
  }
  return b;
}

// Splitter envelope from absolute (x, level) points.
inline void splitEnv(Bytes& b, const std::vector<std::pair<double, double>>& pts) {
  b.u32(3).u32(3).u32(static_cast<uint32_t>(pts.size()));
  double prev = 0;
  for (size_t k = 0; k < pts.size(); ++k) {
    b.f64(pts[k].first - prev).f64(pts[k].second).f32(0).u32(k ? 0x02000000u : 0u);
    prev = pts[k].first;
  }
  b.u32(0);
  for (int i = 0; i < 16; ++i) b.u8(0xFF);
}

inline void splitZone(Bytes& b, const std::vector<std::pair<double, double>>& keys,
                      const std::vector<std::pair<double, double>>& vels, int transpose, bool absolute) {
  splitEnv(b, keys);
  splitEnv(b, vels);
  b.i32(transpose).u8(absolute ? 1 : 0).u8(0).u8(0);
}

inline Bytes scriptState(const std::string& ini) {
  Bytes b;
  std::string t = "\xEF\xBB\xBF" + ini;
  b.u32(static_cast<uint32_t>(t.size())).str(t, false);
  return b;
}

inline bool hasLine(const std::string& data, const std::string& line) {
  return data.find("\n" + line + "\n") != std::string::npos || data.rfind(line + "\n", 0) == 0;
}

// Envelope list: version 3, three floats, 1, 3, point count, points (Δbeats, level).
inline void envList(Bytes& b, const std::vector<std::pair<double, double>>& pts) {
  b.i32(3).f32(0).f32(1).f32(1).i32(1).i32(3).i32(static_cast<int32_t>(pts.size()));
  for (const auto& p : pts) b.f64(p.first).f64(p.second).f32(0).u32(0);
}

}  // namespace vfxtest

inline void testVfx() {
  using namespace vfxtest;
  expect(registered("VFX Key Mapper") && registered("VFX Keyboard Splitter") && registered("VFX Script") && registered("VFX Envelope"),
         "vfx: decoders registered");

  // ------------------------------------------------------------ Key Mapper
  {
    // Snap black keys down; base key 0, offset +3.
    std::vector<std::vector<int>> recs;
    for (int r = 0; r < 12; ++r) recs.push_back({(r == 1 || r == 3 || r == 6 || r == 8 || r == 10) ? r - 1 : r});
    recs[11] = {};  // muted key
    Run r;
    decode(r, "VFX Key Mapper", keyMapper(1, 0, 3, 0, 0, recs).b);
    expect(r.ok && r.m.glType == "gl.keymap", "keymap: decodes");
    expect(r.m.exact, "keymap: one target per key maps exactly");
    expect(near(param(r, mod::KmMap + 1), -1) && near(param(r, mod::KmMap + 2), 0) && near(param(r, mod::KmMap + 10), -1),
           "keymap: per key offsets");
    expect(near(param(r, mod::KmMap + 11), -13), "keymap: empty key list mutes");
    expect(near(param(r, mod::KmTranspose), 3) && near(param(r, mod::KmMulti), 0), "keymap: offset → transpose");
    expect(r.m.param && r.m.param(2) == mod::KmTranspose && r.m.value && near(r.m.value(2, 1.0), 12) && r.m.rawNorm &&
               near(r.m.rawNorm(2, 0), 0.5),
           "keymap: offset automation param / value / rawNorm");
  }
  {
    // Harmony: each key plus a fifth above; base key D rotates the records.
    std::vector<std::vector<int>> recs;
    for (int r = 0; r < 12; ++r) recs.push_back({r, r + 7});
    Run r;
    decode(r, "VFX Key Mapper", keyMapper(1, 2, 0, 0, 0, recs).b);
    expect(r.ok && r.m.exact && near(param(r, mod::KmMulti), 1) && near(param(r, mod::KmMap + 0), 7) && near(param(r, mod::KmMap + 5), 7),
           "keymap: key + fifth → mapped + original");
  }
  {
    std::vector<std::vector<int>> recs;
    for (int r = 0; r < 12; ++r) recs.push_back({r, r + 4, r + 7});
    Run r;
    decode(r, "VFX Key Mapper", keyMapper(1, 0, 0, 0, 0, recs).b);
    expect(r.ok && !r.m.exact && near(param(r, mod::KmMap + 0), 4), "keymap: chords approximate (first other key)");
    Run s;
    std::vector<std::vector<int>> one;
    for (int k = 0; k < 12; ++k) one.push_back({k - 12});
    decode(s, "VFX Key Mapper", keyMapper(1, 0, 0, 0, 120, one).b);
    expect(s.ok && !s.m.exact && near(param(s, mod::KmMap + 3), -12), "keymap: pitch slide marks approximate");
    Run t;
    decode(t, "VFX Key Mapper", Bytes().i32(2).i32(1).b);
    expect(!t.ok, "keymap: truncated state rejected");
  }

  // ------------------------------------------------------- Keyboard Splitter
  {
    const double k59 = 59 / 132.0, k60 = 60 / 132.0;
    Bytes b;
    b.u32(3);
    splitZone(b, {{0, 1}, {k59, 1}, {k60, 0}, {1, 0}}, {{0, 1}, {1, 1}}, 0, false);
    splitZone(b, {{0, 0}, {k59, 0}, {k60, 1}, {1, 1}}, {{0, 1}, {1, 1}}, 12, false);
    for (int z = 2; z < 16; ++z) splitZone(b, {{0, 0}, {1, 0}}, {{0, 1}, {1, 1}}, 0, false);
    Run r;
    decode(r, "VFX Keyboard Splitter", b.b);
    expect(r.ok && r.m.glType == "gl.split", "split: decodes");
    expect(r.m.exact, "split: step zones map exactly");
    expect(near(param(r, mod::splitParam(0, mod::SpOn)), 1) && near(param(r, mod::splitParam(0, mod::SpLow)), 0) &&
               near(param(r, mod::splitParam(0, mod::SpHigh)), 59),
           "split: zone 1 keys 0..59");
    expect(near(param(r, mod::splitParam(1, mod::SpLow)), 60) && near(param(r, mod::splitParam(1, mod::SpHigh)), 127) &&
               near(param(r, mod::splitParam(1, mod::SpTranspose)), 12),
           "split: zone 2 keys 60.., transpose");
    expect(near(param(r, mod::splitParam(2, mod::SpOn)), 0) && near(param(r, mod::splitParam(3, mod::SpOn)), 0), "split: silent zones off");
  }
  {
    // Velocity layers (5-byte header form), the upper one in absolute mode.
    Bytes b;
    b.u8(3).u8(0).u8(16).u8(0).u8(1);
    splitZone(b, {{0, 1}, {1, 1}}, {{0, 1}, {0.5, 1}, {0.5, 0}, {1, 0}}, 0, false);
    splitZone(b, {{0, 1}, {1, 1}}, {{0, 0}, {0.5, 0}, {0.5, 1}, {1, 1}}, 0, true);
    for (int z = 2; z < 16; ++z) splitZone(b, {{0, 0}, {1, 0}}, {{0, 1}, {1, 1}}, 0, false);
    Run r;
    decode(r, "VFX Keyboard Splitter", b.b);
    expect(r.ok && near(param(r, mod::splitParam(0, mod::SpVelHigh)), 63 / 127.0) &&
               near(param(r, mod::splitParam(1, mod::SpVelLow)), 64 / 127.0),
           "split: velocity ranges");
    expect(!r.m.exact, "split: absolute velocity marks approximate");
    Run t;
    decode(t, "VFX Keyboard Splitter", Bytes().u32(3).u32(9).b);
    expect(!t.ok, "split: malformed state rejected");
  }

  // ------------------------------------------------------------- VFX Script
  {
    Run r;
    decode(r, "VFX Script",
           scriptState("[Script]\r\nSource=# demo<linebreak>pass\r\n\r\n[Parameters]\r\nFormulas: note out :=note + 12 * (count % 2)\r\n"
                       "Formulas: velocity out :=(random() < B) * (velocity * 0.5)\r\nParameters: A=131072\r\nParameters: B=196608\r\n"
                       "Parameters: C=0\r\nParameters: D=0\r\n")
               .b);
    expect(r.ok && r.m.glType == "gl.notescript", "script: decodes");
    expect(r.m.exact, "script: translatable formulas map exactly");
    expect(hasLine(r.m.state.data, "pitch = (pitch + (12 * (n % 2)))"), "script: note formula translated");
    expect(hasLine(r.m.state.data, "velocity = (vel * 0.5)") && hasLine(r.m.state.data, "play = (rand() < b)"),
           "script: probability factor becomes play");
    expect(r.m.state.data.find("# pass") != std::string::npos, "script: source kept as comments");
    expect(near(param(r, mod::NsA), 0.5) && near(param(r, mod::NsB), 0.75), "script: knobs A, B → a, b");
    gl::packs::modulation::NoteScriptProgram prog;
    gl::packs::modulation::compileNoteScript(r.m.state.data, prog);
    expect(prog.error.empty() && !prog.pitch.empty() && !prog.play.empty(), "script: data compiles");
    expect(r.m.param && r.m.param(3) == mod::NsB && r.m.rawNorm && near(r.m.rawNorm(3, 131072), 0.5), "script: knob automation");
  }
  {
    Run r;
    decode(r, "VFX Script",
           scriptState("[Script]\r\nSource=x\r\n[Parameters]\r\nFormulas: note out :=note + choice([3,7]) + D * 12\r\n"
                       "Formulas: velocity out :=velocity + gauss(0, 0.1)\r\nParameters: D=262144\r\n")
               .b);
    gl::packs::modulation::NoteScriptProgram prog;
    gl::packs::modulation::compileNoteScript(r.m.state.data, prog);
    expect(r.ok && !r.m.exact && prog.error.empty() && !prog.pitch.empty(), "script: choice / gauss / knob D approximated");
    Run s;
    decode(s, "VFX Script",
           scriptState("[Script]\r\nSource=x\r\n[Parameters]\r\nFormulas: note out :=note if velocity else 0\r\n"
                       "Formulas: velocity out :=velocity\r\n")
               .b);
    expect(s.ok && !s.m.exact && hasLine(s.m.state.data, "pitch = pitch"), "script: untranslatable formula passes notes");
    Run u;
    decode(u, "VFX Script", scriptState("[Script]\r\nSource=def onTick():<linebreak>  v.note = 3\r\n").b);
    expect(u.ok && !u.m.exact && u.m.state.data.find("#   v.note = 3") != std::string::npos, "script: general script kept, approximate");
    Run w;
    decode(w, "VFX Script",
           scriptState("[Script]\r\nSource=def onTriggerVoice(i):<linebreak>  v = V(i)<linebreak>  v.trigger()<linebreak>"
                       "def onTick():<linebreak>  v.copyFrom(p)<linebreak>def onReleaseVoice(i):<linebreak>  v.release()\r\n")
               .b);
    expect(w.ok && w.m.exact, "script: pass-through script exact");
  }

  // ----------------------------------------------------------- VFX Envelope
  {
    Bytes b;
    b.u16(3).u16(1).i32(1003).i32(16);
    b.pad(0x100);
    envList(b, {{0, 0}, {1, 1}, {1, 0.5}, {2, 0}});
    for (int t = 1; t < 5; ++t) envList(b, {{0, 0.5}, {0.25, 0.5}});
    Run r;
    decode(r, "VFX Envelope", b.b);
    const double spb = 60.0 / (r.song.tempo > 1 ? r.song.tempo : 120.0);
    expect(r.ok && r.m.glType == "gl.envctl" && !r.m.exact, "envelope: decodes (approximate)");
    expect(near(param(r, mod::envParam(0, mod::EaNumPoints)), 4) && near(param(r, mod::envPointLevel(0, 1)), 1) &&
               near(param(r, mod::envPointLevel(0, 2)), 0.5) && near(param(r, mod::envPointTime(0, 3)), 2 * spb, 1e-3),
           "envelope: articulator 1 points (beats → seconds)");
    expect(near(param(r, mod::envParam(0, mod::EaSustain)), 3), "envelope: holds before the release");
    expect(r.m.param && r.m.param(9) == mod::envParam(0, mod::EaEnvAmt) && near(r.m.rawNorm(9, 0), 0.5), "envelope: level automation");
  }
}

}  // namespace dec3test
