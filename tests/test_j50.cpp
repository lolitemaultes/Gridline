// Gridline — J-50 tests: Matrix FM reference model (mfmref.h). The exact
// patch text round trip, parameter overrides over the exact data, the model
// switch for old states, block-size independence and the prepared / unprepared
// paths of the instrument; corpus checks read FL Studio's shipped presets in
// place when it is installed (nothing is bundled).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "packs/Packs.h"
#include "packs/fl_corpus.h"
#include "packs/synths/matrixfm.h"
#include "packs/synths/mfmref.h"

using namespace gl;
namespace mf = gl::packs::synths::mfm;
namespace mr = gl::packs::synths::mfmref;

namespace {

struct Out {
  std::vector<float> l, r;
  float peak = 0;
  bool finite = true;
};

struct NoteSpec {
  double key, vel, on, off;
};

// Renders a generator state with notes at block starts, in blocks of `block` frames.
Out render(const PluginState& s, const std::vector<NoteSpec>& notes, double seconds, int block, bool prepare) {
  Out o;
  auto g = createGenerator(s.type);
  if (!g) return o;
  const double sr = 44100;
  if (prepare) g->prepareHost(s, sr);
  ChannelAux aux;
  prepareChannelAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  const size_t n = static_cast<size_t>(seconds * sr);
  o.l.assign(n, 0.f), o.r.assign(n, 0.f);
  std::vector<int> st(notes.size(), 0);
  for (size_t pos = 0; pos < n;) {
    int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
    // split at note times (sample-exact events)
    for (size_t i = 0; i < notes.size(); ++i) {
      const size_t on = static_cast<size_t>(notes[i].on * sr), off = static_cast<size_t>(notes[i].off * sr);
      if (st[i] == 0 && on > pos && on < pos + static_cast<size_t>(k)) k = static_cast<int>(on - pos);
      if (st[i] == 1 && off > pos && off < pos + static_cast<size_t>(k)) k = static_cast<int>(off - pos);
    }
    for (size_t i = 0; i < notes.size(); ++i) {
      if (st[i] == 0 && static_cast<size_t>(notes[i].on * sr) <= pos) {
        g->noteOn(static_cast<uint32_t>(i + 1), notes[i].key, static_cast<float>(notes[i].vel), 0);
        st[i] = 1;
      }
      if (st[i] == 1 && static_cast<size_t>(notes[i].off * sr) <= pos) {
        g->noteOff(static_cast<uint32_t>(i + 1), 0);
        st[i] = 2;
      }
    }
    g->process(s, aux, ctx, o.l.data() + pos, o.r.data() + pos, k);
    pos += static_cast<size_t>(k);
  }
  for (size_t i = 0; i < n; ++i) {
    o.finite = o.finite && std::isfinite(o.l[i]) && std::isfinite(o.r[i]);
    o.peak = std::max(o.peak, std::max(std::fabs(o.l[i]), std::fabs(o.r[i])));
  }
  return o;
}

bool same(const Out& a, const Out& b) {
  return a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * 4) == 0 &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * 4) == 0;
}

void testModelParam() {
  const PluginInfo* info = findPlugin("gl.matrixfm");
  expect(info && info->legacyModelParam == mf::MModel && info->params.size() == static_cast<size_t>(mf::kParamCountAll),
         "matrixfm: Engine parameter appended, legacy model parameter set");
  PluginState fresh = defaultPluginState("gl.matrixfm");
  expect(fresh.params.size() > static_cast<size_t>(mf::MModel) && fresh.params[mf::MModel] == 0.f, "matrixfm: new instances keep Gridline's own engine");
  PluginState old = fresh;
  old.params.resize(mf::kParamCount);
  normalizePluginState(old);
  expect(old.params[mf::MModel] == 0.f, "matrixfm: states saved before the model parameter keep the legacy engine");
  bool presetsLegacy = !info->presets.empty();
  for (size_t i = 0; info && i < info->presets.size(); ++i) presetsLegacy = presetsLegacy && presetState(*info, i).params[mf::MModel] == 0.f;
  expect(presetsLegacy, "matrixfm: factory presets stay on the legacy model");
}

void testFreshInstance() {
  PluginState s = defaultPluginState("gl.matrixfm");
  s.params[mf::MModel] = 1;
  const Out a = render(s, {{60, 0.8, 0, 0.4}}, 0.8, 256, true);
  expect(a.finite && a.peak > 0.01f && a.peak < 4.f, "matrixfm reference: a patch without exact data plays (parameters + drawn data converted)");
  const Out b = render(s, {{60, 0.8, 0, 0.4}}, 0.8, 256, false);
  expect(same(a, b), "matrixfm reference: prepared and unprepared instances render the same");
  PluginState legacy = s;
  legacy.params[mf::MModel] = 0;
  const Out c = render(legacy, {{60, 0.8, 0, 0.4}}, 0.8, 256, true);
  expect(c.finite && c.peak > 0.01f && !same(a, c), "matrixfm: the Engine parameter switches the model");
}

void testCorpus() {
  if (!testfl::flInstalled()) {
    expect(true, "FL Studio not installed: reference model corpus skipped");
    return;
  }
  auto all = testfl::decodePresets("Sytrus");
  size_t n = 0, withData = 0, roundTrip = 0, sameFromState = 0, exact = 0, native = 0;
  const PluginState* sample = nullptr;
  for (const auto& d : all) {
    if (!d.ok || d.state.type != "gl.matrixfm") continue;
    ++n;
    exact += d.exact;
    mr::Patch p;
    std::vector<float> snap;
    if (!mr::parse(d.state.data, p, snap)) continue;
    ++withData;
    std::vector<float> snap2;
    mr::Patch q;
    roundTrip += mr::parse(mr::serialize(p, snap), q, snap2) && mr::serialize(q, snap2) == mr::serialize(p, snap);
    sameFromState += mr::serialize(mr::patchFromState(d.state), {}) == mr::serialize(p, {});
    const std::vector<uint8_t> raw = mr::writeNative(p);
    mr::Patch back;
    native += mr::parseNative(raw.data(), raw.size(), p.ver, back) && mr::serialize(back, {}) == mr::serialize(p, {});
    if (!sample && d.where.find("Piano") != std::string::npos) sample = &d.state;
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "Sytrus presets: %zu decode, %zu keep the exact patch, %zu round-trip, %zu play it unchanged, %zu write back natively, %zu exact",
                n, withData, roundTrip, sameFromState, native, exact);
  expect(n > 900 && withData == n && roundTrip == n && sameFromState == n && native == n && exact > n * 9 / 10, buf);
  if (!sample) return;
  // An edited parameter applies over the exact data (filter 1 cutoff → unit ints / 256).
  PluginState s = *sample;
  s.params[mf::flt(0, mf::FlCut)] = 0.25f;
  s.params[mf::MVolume] = 0.5f;
  const mr::Patch p = mr::patchFromState(s);
  expect(p.units[mf::kOps].u[2] == 64 && p.main[0] == 64, "reference model: edited parameters override the exact patch");
  // Block-size independence (internal blocks follow the engine's own clock).
  const std::vector<NoteSpec> notes = {{60, 0.78125, 0, 0.5}, {67, 0.6, 0.25, 0.9}};
  const Out a = render(*sample, notes, 1.2, 256, true), b = render(*sample, notes, 1.2, 64, true), c = render(*sample, notes, 1.2, 1000, true);
  expect(a.finite && a.peak > 0.01f && same(a, b) && same(a, c), "reference model: output independent of the host block size");
}

}  // namespace

int main() {
  testModelParam();
  testFreshInstance();
  testCorpus();
  return finish();
}
