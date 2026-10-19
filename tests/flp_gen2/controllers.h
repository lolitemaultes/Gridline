// Gridline — second-wave instrument decoder tests: controllers (Keyboard
// Controller, Voltage Controller, Patcher). Included by test_flp_gen2.cpp.
#pragma once
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/modulation/modulation_dsp.h"
#include "Gen2Test.h"

namespace gen2test {

inline std::vector<uint8_t> kcState(int32_t atk, int32_t rel) {
  Bytes s;
  s.u32(2).i32(atk).i32(rel);
  for (int k = 0; k < 132; ++k) s.f32(k == 60 ? 0.5f : (k > 60 ? 1.0f : 0.0f));
  s.i32(60).i32(67).i32(0).u8(1);
  return s.b;
}

inline std::vector<uint8_t> vcState() {
  Bytes s;
  s.u32(2);
  s.i32(49152).i32(16384);
  for (int i = 0; i < 6; ++i) s.i32(32768);
  s.i32(32768).i32(65536);
  for (int i = 0; i < 6; ++i) s.i32(65536);
  for (uint8_t src : {3, 8, 3, 5, 6, 7, 9, 9}) s.u8(src);
  s.u8(1);
  s.pad(0x63).i32(48).i32(441).f64(1.2);
  return s.b;
}

// Patcher chunk: u32 id, u64 length, payload.
inline Bytes& chunk(Bytes& b, uint32_t id, const std::vector<uint8_t>& payload) {
  b.u32(id).u32(static_cast<uint32_t>(payload.size())).u32(0).raw(payload);
  return b;
}
inline std::vector<uint8_t> u16s(const std::string& s) {
  Bytes b;
  b.utf16(s, false);
  return b.b;
}
inline std::vector<uint8_t> patcherNode(const std::string& plugin, const std::vector<uint8_t>& state) {
  Bytes n;
  chunk(n, 5000, std::vector<uint8_t>(40, 0));
  chunk(n, 5004, u16s(plugin));
  chunk(n, 5008, u16s(plugin));
  chunk(n, 5005, state);
  chunk(n, 5001, {});
  return n.b;
}
inline std::vector<uint8_t> patcherState(const std::vector<std::vector<uint8_t>>& nodes, const std::vector<std::string>& ports) {
  Bytes s;
  s.u32(4);
  chunk(s, 1000, std::vector<uint8_t>(64, 0));
  for (const auto& n : nodes) chunk(s, 0, n);
  for (const auto& p : ports) {
    Bytes port;
    chunk(port, 1400, std::vector<uint8_t>(28, 0));
    chunk(port, 1402, u16s(p));
    chunk(port, 1401, {});
    chunk(s, 0, port.b);
  }
  chunk(s, 1001, {});
  return s.b;
}

inline void testControllers() {
  namespace mod = gl::packs::modulation;
  {  // Keyboard Controller: key table + instant smoothing
    Run r;
    decode(r, "Fruity Keyboard Controller", kcState(0, 0));
    expect(r.ok && r.m.glType == "gl.keyctl", "keyctl: decoded");
    auto keys = mod::parseKeyValues(r.m.state.data);
    expect(near(keys[60], 0.5) && near(keys[59], 0.0) && near(keys[127], 1.0), "keyctl: per-key values");
    expect(near(param(r, mod::KcAttack), 0.001) && near(param(r, mod::KcRelease), 0.001), "keyctl: zero smoothing");
    expect(r.m.exact, "keyctl: exact");
    expect(r.m.param && r.m.param(0) == mod::KcAttack && r.m.param(1) == mod::KcRelease && r.m.param(5) == -1, "keyctl: automation links");
  }
  {
    Run r;
    decode(r, "Fruity Keyboard Controller", kcState(128, 256));
    expect(r.ok && !r.m.exact, "keyctl: smoothing times are approximate");
    expect(param(r, mod::KcAttack) > 0.1f && param(r, mod::KcRelease) > param(r, mod::KcAttack), "keyctl: smoothing times grow");
  }
  {
    Run r;
    decode(r, "Fruity Keyboard Controller", {1, 2, 3});
    expect(r.ok && r.m.glType == "gl.keyctl", "keyctl: short state gives defaults");
  }
  {  // Voltage Controller
    Run r;
    decode(r, "Fruity Voltage Controller", vcState());
    expect(r.ok && r.m.glType == "gl.cvctl", "cvctl: decoded");
    expect(near(param(r, mod::CvLeftOfs), 0.5) && near(param(r, mod::CvRightOfs), -0.5), "cvctl: offsets");
    expect(near(param(r, mod::CvLeftScale), 0.5) && near(param(r, mod::CvRightScale), 1.0), "cvctl: scales");
    expect(param(r, mod::CvLeftSrc) == mod::CvsVelocity && param(r, mod::CvRightSrc) == mod::CvsClock, "cvctl: sources");
    expect(param(r, mod::CvEnable) == 1 && param(r, mod::CvClockPpq) == 7 && param(r, mod::CvPitchScale) == 1, "cvctl: options");
    expect(near(param(r, mod::CvPulse), 0.01), "cvctl: pulse length");
    expect(r.m.value && near(r.m.value(0, 1.0), 1.0) && near(r.m.value(0, 0.5), 0.0), "cvctl: offset automation");
  }
  {  // Patcher (instrument): hosted plugins through the decoder registry
    Run r;
    auto st = patcherState({patcherNode("Fruity Voltage Controller", vcState()), patcherNode("Mystery Synth", {1, 2, 3, 4})}, {"Notes", "Audio output"});
    decode(r, "Patcher", st);
    expect(r.ok && r.m.glType == "gl.patcher", "patcher: instrument version");
    auto g = mod::parsePatchGraph(r.m.state.data);
    expect(g.nodes.size() == 1 && g.nodes[0].plugin.type == "gl.cvctl", "patcher: hosted plugin decoded");
    expect(!g.nodes.empty() && near(g.nodes[0].plugin.params[mod::CvLeftOfs], 0.5), "patcher: hosted state kept");
    expect(!r.report.warnings.empty(), "patcher: unknown hosted plugin reported");
  }
  {
    Run r;
    decode(r, "Patcher", patcherState({patcherNode("Mystery Synth", {1, 2, 3, 4})}, {"Notes", "Audio output"}));
    auto g = mod::parsePatchGraph(r.m.state.data);
    expect(r.ok && g.nodes.size() == 1 && g.nodes[0].plugin.type == "gl.synth", "patcher: undecodable instrument gets a stand-in");
  }
  {  // Patcher (effect): same FL name, audio input port
    Run r;
    decode(r, "Patcher", patcherState({patcherNode("Fruity Mystery FX", {1, 2})}, {"Audio input", "Audio output"}));
    expect(r.ok && r.m.glType == "gl.patcherfx", "patcher: effect version");
    expect(mod::parsePatchGraph(r.m.state.data).nodes.empty() && !r.m.state.data.empty(), "patcher fx: no default graph");
  }
  {
    Run r;
    std::vector<uint8_t> junk(200, 0xFF);
    junk[0] = 4;
    decode(r, "Patcher", junk);
    expect(r.ok, "patcher: garbage state does not crash");
  }
}

}  // namespace gen2test
