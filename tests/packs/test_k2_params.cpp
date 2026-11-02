// Gridline: K-2 parameter audit, round 1: new instances of the FL-equivalent
// effects start at the reference plugin's defaults, while old projects and the
// factory presets keep the values they had (legacy defaults); FL automation
// positions become the control's integer the way the reference does (half to
// even); version 1 Emphasizer states keep a 0 dB output; Delay Bank unit
// controls take automation. All on hand-made states (no FL content).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Song.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpFx.h"
#include "../../src/core/flp/FlpFx2.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/dynamics/emphasizer_ref.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../../src/packs/spectral/spectral.h"
#include "../Check.h"

using namespace gl;

namespace {

struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Blob& u8(uint8_t v) {
    b.push_back(v);
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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The plugins whose defaults round 1 moved to the reference's.
const char* const kTypes[] = {"gl.comp",    "gl.flanger",  "gl.timegate", "gl.freqshift", "gl.lowlift", "gl.bassboost",
                              "gl.overdrive", "gl.squeeze", "gl.spread",   "gl.bbdchorus", "gl.vphaser", "gl.hall",
                              "gl.xyzctl",  "gl.formulactl"};

bool finiteRun(const std::string& type, const std::vector<float>& params) {
  auto fx = createEffect(type);
  if (!fx) return true;  // controllers without an audio path
  PluginState s;
  s.type = type;
  s.params = params;
  ProcessContext ctx;
  std::vector<float> L(256), R(256);
  bool ok = true;
  for (int b = 0; b < 40 && ok; ++b) {
    for (int i = 0; i < 256; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.05 * (b * 256 + i)));
    fx->process(s, ctx, L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) ok = ok && std::isfinite(L[static_cast<size_t>(i)]) && std::isfinite(R[static_cast<size_t>(i)]);
  }
  return ok;
}

}  // namespace

int main() {
  namespace sx = gl::packs::spacefx;
  namespace dy = gl::packs::dynamics;

  {  // every changed plugin records its former defaults, and they differ from the new ones
    bool all = true;
    for (const char* t : kTypes) {
      const PluginInfo* p = findPlugin(t);
      if (!p || p->legacyDefaults.empty()) {
        all = false;
        std::printf("  no legacy defaults: %s\n", t);
        continue;
      }
      for (const auto& [idx, old] : p->legacyDefaults)
        all = all && idx >= 0 && static_cast<size_t>(idx) < p->params.size() && static_cast<float>(p->params[static_cast<size_t>(idx)].def) != old;
    }
    expect(all, "changed plugins carry their former defaults");
  }
  {  // factory presets sound as before: every preset pins the former default of each moved parameter
    bool all = true;
    for (const char* t : kTypes) {
      const PluginInfo* p = findPlugin(t);
      if (!p) continue;
      for (const auto& pr : p->presets) {
        std::map<int, float> v(pr.values.begin(), pr.values.end());
        for (const auto& [idx, old] : p->legacyDefaults) all = all && v.count(idx) > 0;
      }
    }
    expect(all, "factory presets keep the former defaults");
  }
  {  // an old, shorter state gets the former default for the parameters it predates
    bool all = true;
    for (const char* t : kTypes) {
      const PluginInfo* p = findPlugin(t);
      if (!p) continue;
      for (const auto& [idx, old] : p->legacyDefaults) {
        if (idx <= 0) continue;
        PluginState s;
        s.type = t;
        for (int i = 0; i < idx; ++i) s.params.push_back(static_cast<float>(p->params[static_cast<size_t>(i)].def));
        normalizePluginState(s);
        all = all && s.params.size() == p->params.size() && s.params[static_cast<size_t>(idx)] == old;
      }
      // a full state keeps its stored values; an empty state is a new instance
      PluginState full;
      full.type = t;
      for (const auto& d : p->params) full.params.push_back(static_cast<float>(d.def));
      const auto before = full.params;
      normalizePluginState(full);
      all = all && full.params == before;
      PluginState fresh;
      fresh.type = t;
      normalizePluginState(fresh);
      for (const auto& [idx, old] : p->legacyDefaults)
        all = all && fresh.params[static_cast<size_t>(idx)] == static_cast<float>(p->params[static_cast<size_t>(idx)].def);
    }
    expect(all, "old states keep the former defaults; new instances take the new ones");
  }
  {  // new defaults equal the reference values (spot checks, Gridline units)
    const PluginInfo* c = findPlugin("gl.comp");
    const PluginInfo* b = findPlugin("gl.bassboost");
    const PluginInfo* h = findPlugin("gl.hall");
    expect(c && near(c->params[pp::CmpThreshold].def, 0, 1e-9) && near(c->params[pp::CmpRatio].def, 1, 1e-9) &&
               near(c->params[pp::CmpAttack].def, 0.015, 1e-9) && near(c->params[pp::CmpRelease].def, 0.2, 1e-9),
           "compressor starts at the reference defaults");
    expect(b && b->params.size() > 1, "bass boost present");
    expect(h && near(h->params[sx::HvDecay].def, 5.22196, 1e-4) && near(h->params[sx::HvSize].def, 0.191919, 1e-5),
           "hall reverb starts at the reference defaults");
  }
  {  // extremes and defaults stay finite
    bool all = true;
    for (const char* t : kTypes) {
      const PluginInfo* p = findPlugin(t);
      if (!p) continue;
      std::vector<float> lo, hi, def;
      for (const auto& d : p->params) {
        lo.push_back(static_cast<float>(d.min));
        hi.push_back(static_cast<float>(d.max));
        def.push_back(static_cast<float>(d.def));
      }
      const bool ok = finiteRun(t, lo) && finiteRun(t, hi) && finiteRun(t, def);
      if (!ok) std::printf("  not finite: %s\n", t);
      all = all && ok;
    }
    expect(all, "extremes and new defaults stay finite");
  }
  {  // FL automation rounding: half to even on the offset from the control's minimum
    expect(flp::fx::flRoundInt(3.5) == 4 && flp::fx::flRoundInt(0.5) == 0 && flp::fx::flRoundInt(4.5) == 4 &&
               flp::fx::flRoundInt(2.49) == 2 && flp::fx::flRoundInt(2.51) == 3,
           "half-to-even rounding");
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(2);
    for (int v : {-200, 40, 0, 50, 200, 0}) s.i32(v);
    s.i32(0);
    expect(decode(e, "Fruity Compressor", s.b, m) && m.value && m.param, "compressor decodes");
    // tag 5 (type 0..7) at 0.5 is 4 in the reference: knee of type 0 (hard)
    expect(m.param(5) == pp::CmpKnee && near(m.value(5, 0.5), 0.0, 1e-9), "compressor type at 0.5 = hard knee (4)");
    expect(near(m.value(5, 1.0 / 7.0), 6.0, 1e-9) && near(m.value(5, 3.0 / 7.0), 15.0, 1e-9), "compressor type positions 1 and 3");
  }
  {  // Emphasizer version 1 (no output word) keeps 0 dB output
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(1);
    for (int v : {5000, 3000, 0, 1, 0}) s.i32(v);
    expect(decode(e, "Emphasizer", s.b, m), "emphasizer v1 decodes");
    expect(m.state.params.size() > static_cast<size_t>(dy::EnPeakOut) &&
               near(m.state.params[dy::EnPeakOut], dy::emref::outLin(1.0), 1e-6),
           "emphasizer v1 output = 0 dB");
  }
  {  // Delay Bank: unit controls take automation (tag 4 + 16 k + field)
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(16);
    for (int i = 0; i < 4 + 8 * 16; ++i) s.i32(i < 4 ? 128 : 0);
    s.i32(0);
    for (int k = 0; k < 8; ++k)
      for (int i = 0; i < 32; ++i) s.u8(0);
    expect(decode(e, "Fruity Delay Bank", s.b, m) && m.param && m.value, "delay bank decodes");
    const int outLvl1 = 4 + 16 * 1 + 14, pan0 = 4;
    expect(m.param(outLvl1) == sx::mdParam(1, sx::MdOutLevel), "unit 2 output level automation mapped");
    expect(m.param(pan0) == sx::mdParam(0, sx::MdInPan) && near(m.value(pan0, 0.0), -1.0, 1e-9) && near(m.value(pan0, 1.0), 1.0, 1e-9),
           "unit 1 input pan automation law");
    expect(m.param(0) == sx::MdDry, "dry still mapped");
    expect(m.param(12) == sx::mdParam(0, sx::MdTimeMs) && near(m.value(12, 0.5), 1000.0, 1e-6), "free unit time automation in ms");
    expect(m.param(4 + 16 * 7 + 15) < 0, "last unit has no send to next");
  }
  {  // Vintage Phaser / Vintage Chorus: switches take automation
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(3);
    for (int i = 0; i < 36; ++i) s.i32(0);
    expect(decode(e, "Vintage Phaser", s.b, m) && m.param && m.value, "vintage phaser decodes");
    expect(m.param(22) == sx::VpMonoIn && m.value(22, 1.0) == 1.0 && m.value(22, 0.4) == 0.0, "phaser mono input automation");
    expect(m.param(8) == sx::VpWave && m.value(8, 1.0) == 0.0 && m.value(8, 0.0) == 1.0, "phaser LFO shape automation (order reversed)");
    flp::PluginMapping c;
    Blob cs;
    cs.i32(1);
    for (int i = 0; i < 17; ++i) cs.i32(0);
    expect(decode(e, "Vintage Chorus", cs.b, c) && c.param && c.param(14) == sx::BcNoiseGate && c.value(14, 1.0) == 1.0,
           "chorus noise gate automation");
  }
  {  // Fruity Vocoder: the knob block after the band levels
    namespace sp = gl::packs::spectral;
    Env e;
    flp::PluginMapping m;
    Blob s;
    s.i32(2).i32(16).i32(2).i32(2).u8(0);
    for (int i = 0; i < 16; ++i) s.i32(0x3f800000);  // band levels 1.0
    for (int v : {32768, 65536, 64, 1, 600, 50, 20, 300, 1, 128, 64, 160}) s.i32(v);
    expect(decode(e, "Fruity Vocoder", s.b, m), "vocoder decodes");
    auto P = [&](int i) { return static_cast<double>(m.state.params[static_cast<size_t>(i)]); };
    expect(near(P(sp::VcLowFreq), 1457.0, 1.5) && near(P(sp::VcHighFreq), 16000.0, 1.0), "vocoder band range (10 Hz .. 16 kHz law)");
    expect(near(P(sp::VcFormant), 6.0, 1e-6) && P(sp::VcInvert) == 1 && P(sp::VcHold) == 1, "vocoder formant, invert, hold");
    expect(near(P(sp::VcAttack), 20.0, 1e-6) && near(P(sp::VcRelease), 300.0, 1e-6), "vocoder envelope times in ms");
    expect(near(P(sp::VcModLevel), 0.0, 1e-4) && near(P(sp::VcCarLevel), -6.0206, 1e-3) && near(P(sp::VcWetLevel), 1.9382, 1e-3),
           "vocoder levels (128 = 0 dB)");
    expect(m.param && m.param(9) == sp::VcModLevel && m.value && near(m.value(9, 0.0), -60.0, 1e-9), "vocoder automation mapped");
  }
  return finish();
}
