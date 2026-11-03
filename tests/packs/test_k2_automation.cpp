// K-37 (K-AD): FL automation import of the note effects and Band Delay. Each linked
// parameter must give the same plain value as loading that raw value from a state.
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../Check.h"

using namespace gl;

namespace {

struct Blob {
  std::vector<uint8_t> b;
  Blob& u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Blob& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
};

bool decode(const char* fl, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState(fl, state, m, c)) return false;
  normalizePluginState(m.state);
  return true;
}
float P(const flp::PluginMapping& m, int k) { return m.state.params[static_cast<size_t>(k)]; }
bool near(double a, double b, double e = 1e-4) { return std::fabs(a - b) <= e; }

std::vector<uint8_t> bandDelay(int scale, int tenth, int fb, int smooth, int mode, int keep, int morph, int mix, int harm, int weight, int hp, int sat) {
  Blob b;
  b.u32(4).u32(396);
  for (int i = 0; i < 384; ++i) b.i32(1073741824 / 2);
  for (int v : {scale, tenth, fb, smooth, mode, keep, morph, mix, harm, weight, hp, sat}) b.i32(v);
  for (int i = 0; i < 64; ++i) b.u32(0);
  return b.b;
}

std::vector<uint8_t> levelScaler(int fill[6]) {
  Blob b;
  b.u32(1);
  b.u32(1001).u32(144).u32(0);
  for (int t = 0; t < 6; ++t)
    for (int f = 0; f < 6; ++f) b.i32(fill[f]);
  return b.b;
}

}  // namespace

int main() {
  namespace mod = packs::modulation;
  namespace sfx = packs::spacefx;

  // ------------------------------------------------------------------ Band Delay engine values
  {
    flp::PluginMapping m;
    expect(decode("Multiband Delay", bandDelay(0, 0, 0, 1500, 0, 0, 0, 128, 0, 0, 1000, 0), m), "band delay: decodes");
    struct C {
      int fl, gl;
      double lo, hi;
    };
    const C rows[] = {{384, sfx::BdRScale, -6000, 6000}, {385, sfx::BdRTenth, 0, 1},       {386, sfx::BdRFeedback, 0, 6000},
                      {387, sfx::BdRSmooth, 0, 6000},    {388, sfx::BdRMode, 0, 2},        {389, sfx::BdRKeep, 0, 1},
                      {390, sfx::BdRMorph, 0, 7000},     {391, sfx::BdRMix, -128, 128},    {392, sfx::BdRHarmonic, 0, 1},
                      {393, sfx::BdRWeight, 0, 1},       {394, sfx::BdRHp, 1000, 100000},  {395, sfx::BdRSat, 0, 2}};
    for (const C& r : rows) {
      expect(m.param && m.param(r.fl) == r.gl, "band delay: engine parameter is linked");
      // The link's value at each end equals loading that raw value from a state.
      for (double n : {0.0, 1.0}) {
        int v[12] = {0, 0, 0, 1500, 0, 0, 0, 128, 0, 0, 1000, 0};
        v[r.fl - 384] = static_cast<int>(r.lo + n * (r.hi - r.lo));
        flp::PluginMapping d;
        decode("Multiband Delay", bandDelay(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]), d);
        expect(near(m.value(r.fl, n), P(d, r.gl)), "band delay: link and state agree");
      }
      expect(near(m.rawNorm(r.fl, static_cast<int32_t>(r.hi)), 1.0), "band delay: recorded values use the control's range");
    }
    expect(m.param(0) < 0 && m.param(383) < 0, "band delay: band values stay in the state text (not parameters)");
  }

  // ------------------------------------------------------------------ Level scaler: 6 x 6
  {
    int fill[6] = {0, 8192, 0, 0, 0, 1};
    flp::PluginMapping m;
    expect(decode("VFX Level Scaler", levelScaler(fill), m), "level scaler: decodes");
    expect(near(P(m, mod::VsModel), 1), "level scaler: reference model");
    for (int t = 0; t < mod::kVsTargets; ++t)
      for (int f = 0; f < mod::VfCount; ++f) expect(m.param(t * 6 + f) == mod::vsRef(t, f), "level scaler: every control is linked");
    expect(near(m.value(1, 0.75), 1.0), "level scaler: multiplier x1 at FL's 8192");
    expect(near(m.value(6 + 2, 1.0), 1.0) && near(m.value(6 + 2, 0.0), -1.0), "level scaler: offset ends");
    expect(near(m.value(5, 1.0), 2.0) && near(m.value(5, 0.4), 1.0), "level scaler: humanize mode steps");
    expect(near(m.rawNorm(0, 4096), 0.5), "level scaler: recorded centre");
    int other[6] = {4096, -8192, 2048, 0, 0, 2};
    flp::PluginMapping d;
    decode("VFX Level Scaler", levelScaler(other), d);
    expect(near(m.value(0, 0.5), P(d, mod::vsRef(0, mod::VfCenter))) && near(m.value(1, 0.25), P(d, mod::vsRef(0, mod::VfMultiply))),
           "level scaler: link and state agree");
  }

  // ------------------------------------------------------------------ Colour router: ports
  {
    Blob b;
    b.u32(1);
    for (int k = 0; k < 16; ++k) b.i32(k);
    for (int k = 0; k < 16; ++k) b.i32(k);
    flp::PluginMapping m;
    expect(decode("VFX Color Mapper", b.b, m), "colour router: decodes");
    for (int k = 0; k < mod::kColorGroups && k < 16; ++k) expect(m.param(k) == mod::CmMap + k, "colour router: group is linked");
    expect(near(m.value(0, 0.0), 1) && near(m.value(0, 3.0 / 15.0), 4) && near(m.value(0, 7.0 / 15.0), 8), "colour router: port p to target p + 1");
    expect(near(m.value(0, 1.0), 8), "colour router: ports past the targets stay on the last");
    expect(m.param(16) < 0, "colour router: channel remapping is not modelled");
  }

  // ------------------------------------------------------------------ Note envelope: each articulator
  {
    flp::PluginMapping m;
    Blob b;
    b.u32(3 | (1u << 16)).u32(1003).u32(16);
    while (b.b.size() < 0x100) b.u32(0);
    for (int t = 0; t < 20; ++t) {
      b.i32(3).u32(0).u32(0).u32(0).i32(1).i32(3).i32(2);
      for (int k = 0; k < 2; ++k) {
        for (double d : {static_cast<double>(k), 0.5}) {
          uint64_t u;
          std::copy(reinterpret_cast<const uint8_t*>(&d), reinterpret_cast<const uint8_t*>(&d) + 8, reinterpret_cast<uint8_t*>(&u));
          b.u32(static_cast<uint32_t>(u)).u32(static_cast<uint32_t>(u >> 32));
        }
        b.u32(0).u32(0);
      }
    }
    expect(decode("VFX Envelope", b.b, m), "note envelope: decodes");
    for (int a = 0; a < mod::kEnvArts; ++a) {
      expect(m.param(a * 55 + 8) == mod::envParam(a, mod::EaBase), "note envelope: base level of each articulator");
      expect(m.param(a * 55 + 9) == mod::envParam(a, mod::EaEnvAmt), "note envelope: envelope level of each articulator");
      expect(m.param(a * 55 + 10) == mod::envParam(a, mod::EaLfoAmt), "note envelope: LFO level of each articulator");
    }
    expect(near(m.value(55 + 8, 0.5), 0.5) && near(m.value(55 + 9, 1.0), 1.0) && near(m.value(55 + 9, 0.0), -1.0), "note envelope: level laws");
  }

  return finish();
}
