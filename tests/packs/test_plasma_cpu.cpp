// Gridline — "plasma" pack tests: pay only for what you use. Parts that are off, and movement
// sources nothing links to, cost no audio work, yet the sound stays bit-identical:
//  - a render corpus (every factory preset, every data preset, a high-quality pass, an
//    everything-on patch, and every preset again with extra links) hashed sample by sample and
//    compared with the hashes the engine made before parts were skipped
//    (tests/fixtures/plasma_render_golden.tsv, one line per corpus item and machine kind);
//  - a link made mid-note to an LFO or an envelope that ran unlinked picks it up where its
//    clock is, without a jump to its start;
//  - a 1-layer plain patch costs well under half of an everything-on patch.
// GL_PLASMA_GOLDEN_WRITE=<file> writes this machine's hashes instead of checking them.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

using namespace gl;
namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 48000;

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};

// Renders st; edit (when set) changes a parameter at frame `editAt` (a link made mid-note).
std::vector<float> render(const PluginState& st0, const std::vector<Ev>& evs, int total, int block, int editAt = -1,
                          const std::vector<std::pair<int, float>>& edit = {}) {
  PluginState st = st0;
  std::vector<float> out(static_cast<size_t>(total) * 2, 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) return {};
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.ppq = 96;
  ctx.playing = true;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    if (editAt >= pos && editAt < pos + n)
      for (const auto& kv : edit) {
        if (static_cast<int>(st.params.size()) <= kv.first) st.params.resize(static_cast<size_t>(kv.first + 1), 0.f);
        st.params[static_cast<size_t>(kv.first)] = kv.second;
      }
    ctx.tick = pos / kSr * 2.0 * ctx.ppq;
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    g->process(st, aux, ctx, L.data(), R.data(), n);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(2 * (pos + i))] = L[static_cast<size_t>(i)], out[static_cast<size_t>(2 * (pos + i) + 1)] = R[static_cast<size_t>(i)];
  }
  return out;
}

uint64_t hashOf(const std::vector<float>& x) {
  uint64_t h = 1469598103934665603ull;
  for (float v : x) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    for (int k = 0; k < 4; ++k) h = (h ^ ((b >> (8 * k)) & 0xffu)) * 1099511628211ull;
  }
  return h;
}

void setP(PluginState& s, int k, float v) {
  if (static_cast<int>(s.params.size()) <= k) s.params.resize(static_cast<size_t>(k + 1), 0.f);
  s.params[static_cast<size_t>(k)] = v;
}
float getP(const PluginState& s, int k) { return k < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(k)] : 0.f; }
PluginState base() { return defaultPluginState(pl::kType); }

// adds a link src -> dest (param index) in the first free slot
void link(PluginState& s, int src, int dest, float amount) {
  for (int k = 0; k < pl::kModSlots; ++k)
    if (std::lround(getP(s, pl::modP(k, pl::M_Source))) == pl::SrcNone && std::lround(getP(s, pl::modP(k, pl::M_Dest))) == 0) {
      setP(s, pl::modP(k, pl::M_Source), static_cast<float>(src));
      setP(s, pl::modP(k, pl::M_Dest), static_cast<float>(dest + 1));
      setP(s, pl::modP(k, pl::M_Amount), amount);
      return;
    }
}

// Every part placed: three layers with unison, Root, Texture, both filters, eight effects, every
// LFO, envelope and macro linked.
PluginState everythingOn() {
  PluginState s = base();
  for (int o = 0; o < pl::kOscs; ++o) {
    setP(s, pl::oscP(o, pl::O_Enable), 1);
    setP(s, pl::oscP(o, pl::O_Unison), 4);
    setP(s, pl::oscP(o, pl::O_Level), 0.5f);
  }
  setP(s, pl::subP(pl::S_Enable), 1);
  setP(s, pl::noiseP(pl::N_Enable), 1);
  setP(s, pl::noiseP(pl::N_Type), 1);
  for (int f = 0; f < pl::kFilters; ++f) setP(s, pl::filtP(f, pl::F_Enable), 1);
  const int fx[8] = {pl::FxDistortion, pl::FxChorus, pl::FxPhaser, pl::FxFlanger, pl::FxDelay, pl::FxComp, pl::FxEq, pl::FxReverb};
  for (int k = 0; k < 8; ++k) setP(s, pl::fxP(k, pl::X_Type), static_cast<float>(fx[k])), setP(s, pl::fxP(k, pl::X_On), 1);
  const int dests[] = {pl::filtP(0, pl::F_Freq), pl::filtP(1, pl::F_Freq), pl::oscP(0, pl::O_Pan), pl::oscP(1, pl::O_Pan), pl::oscP(2, pl::O_Pan),
                       pl::filtP(0, pl::F_Res), pl::oscP(0, pl::O_Level), pl::oscP(1, pl::O_Level), pl::oscP(2, pl::O_Level), pl::filtP(1, pl::F_Res)};
  for (int l = 0; l < pl::kLfos; ++l) link(s, pl::SrcLfo1 + l, dests[l], 0.1f);
  for (int e = 1; e < pl::kEnvs; ++e) link(s, pl::SrcEnv1 + e, dests[e + 3], 0.1f);
  for (int m = 0; m < 4; ++m) link(s, pl::SrcMacro1 + m, dests[m], 0.05f);
  return s;
}

// The corpus: name -> state and notes
struct Item {
  std::string name;
  PluginState st;
};
std::vector<Item> corpus() {
  std::vector<Item> v;
  const PluginInfo* info = findPlugin(pl::kType);
  for (size_t i = 0; i < info->presets.size(); ++i) v.push_back({"preset " + info->presets[i].name, presetState(*info, i)});
  for (const auto& dp : pl::factoryDataPresets()) {
    PluginState s = base();
    for (const auto& kv : dp.values) setP(s, kv.first, kv.second);
    s.data = dp.data;
    v.push_back({"data " + dp.name, s});
  }
  const size_t n = v.size();
  for (size_t i = 0; i < n; i += 3) {  // high quality (oversampled) on every third
    Item it = v[i];
    setP(it.st, pl::G_Quality, 2);
    it.name += " (ultra)";
    v.push_back(it);
  }
  for (size_t i = 0; i < n; ++i) {  // extra links from sources the preset may leave unlinked
    Item it = v[i];
    link(it.st, pl::SrcLfo1 + static_cast<int>(i % 9) + 1, pl::filtP(0, pl::F_Freq), 0.2f);
    link(it.st, pl::SrcEnv3, pl::oscP(0, pl::O_Level), -0.3f);
    link(it.st, pl::SrcMacro1 + 2, pl::oscP(1, pl::O_Pan), 0.4f);
    it.name += " (links)";
    v.push_back(it);
  }
  v.push_back({"everything on", everythingOn()});
  return v;
}
const std::vector<Ev> kNotes = {{0, true, 1, 48, 0.8f}, {0, true, 2, 55, 0.7f}, {2000, true, 3, 60, 0.9f}, {9000, true, 4, 64, 0.6f},
                                {24000, false, 1, 48, 0}, {24000, false, 2, 55, 0}, {26000, false, 3, 60, 0}, {30000, false, 4, 64, 0}};

std::string machine() {
#if defined(__APPLE__)
  std::string os = "mac";
#elif defined(_WIN32)
  std::string os = "win";
#else
  std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  return os + "-arm64";
#else
  return os + "-x64";
#endif
}

double seconds(const PluginState& s, int notes) {
  std::vector<Ev> ev;
  for (int i = 0; i < notes; ++i) ev.push_back({0, true, static_cast<uint32_t>(i + 1), 48.0 + i * 3, 0.8f});
  double best = 1e9;
  for (int r = 0; r < 3; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto o = render(s, ev, 48000, 256);
    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (o.empty()) return 0;
  }
  return best;
}
}  // namespace

int main() {
  // ---- the render corpus: bit-identical with the hashes made before parts were skipped
  {
    const std::string mk = machine();
    const auto items = corpus();
    std::map<std::string, std::string> have;  // "<machine>\t<name>" -> hash
    {
      std::ifstream in("tests/fixtures/plasma_render_golden.tsv");
      std::string line;
      while (std::getline(in, line)) {
        const size_t a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || b == a) continue;
        have[line.substr(0, b)] = line.substr(b + 1);
      }
    }
    const char* write = std::getenv("GL_PLASMA_GOLDEN_WRITE");
    std::ofstream out;
    if (write) out.open(write, std::ios::app);
    int checked = 0, differ = 0;
    for (const auto& it : items) {
      char h[24];
      std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(hashOf(render(it.st, kNotes, 36000, 256))));
      const std::string key = mk + "\t" + it.name;
      if (write) {
        out << key << "\t" << h << "\n";
        continue;
      }
      const auto f = have.find(key);
      if (f == have.end()) continue;
      ++checked;
      if (f->second != h) {
        ++differ;
        std::printf("# differs: %s\n", it.name.c_str());
      }
    }
    if (!write) {
      std::printf("# render corpus: %d items, %d checked on %s, %d differ\n", static_cast<int>(items.size()), checked, mk.c_str(), differ);
      expect(differ == 0, "every corpus render is bit-identical to the engine before parts were skipped");
      if (checked == 0) std::printf("# no golden hashes for %s: the comparison was skipped\n", mk.c_str());
    }
    if (write) return 0;
  }
  // ---- a link made mid-note to a source that ran unlinked: it is where its clock is
  {
    PluginState s = base();
    setP(s, pl::oscP(0, pl::O_Enable), 1);
    setP(s, pl::lfoP(1, pl::L_Rate), 0.6f);
    const int at = 24000, dest = pl::oscP(0, pl::O_Pan);
    // reference: linked from the start with amount 0, then the amount set mid-note
    PluginState ref = s;
    link(ref, pl::SrcLfo1 + 1, dest, 0.f);
    int slot = 0;
    const auto a = render(ref, {{0, true, 1, 60, 0.8f}}, 36000, 256, at, {{pl::modP(slot, pl::M_Amount), 0.8f}});
    // the LFO unlinked until mid-note, then linked with the same amount
    const auto b = render(s, {{0, true, 1, 60, 0.8f}}, 36000, 256, at,
                          {{pl::modP(slot, pl::M_Source), static_cast<float>(pl::SrcLfo1 + 1)}, {pl::modP(slot, pl::M_Dest), static_cast<float>(dest + 1)},
                           {pl::modP(slot, pl::M_Amount), 0.8f}});
    const auto never = render(s, {{0, true, 1, 60, 0.8f}}, 36000, 256);
    double moved = 0;
    for (size_t i = static_cast<size_t>(at + 2048) * 2; i < never.size() && i < b.size(); ++i) moved += std::fabs(never[i] - b[i]);
    expect(moved > 1e-3, "the late link changes the sound (it took effect)");
    double diff = 0, ref2 = 0;
    for (size_t i = static_cast<size_t>(at + 2048) * 2; i < a.size(); ++i) diff += std::fabs(a[i] - b[i]), ref2 += std::fabs(a[i]);
    std::printf("# late link: %.5f of the signal differs\n", diff / std::max(1e-9, ref2));
    expect(!a.empty() && diff <= ref2 * 0.02, "an LFO linked mid-note continues from its clock (no restart)");
    // an envelope too
    PluginState refE = s;
    link(refE, pl::SrcEnv3, pl::oscP(0, pl::O_Level), 0.f);
    const auto c = render(refE, {{0, true, 1, 60, 0.8f}}, 36000, 256, at, {{pl::modP(slot, pl::M_Amount), -0.5f}});
    const auto d = render(s, {{0, true, 1, 60, 0.8f}}, 36000, 256, at,
                          {{pl::modP(slot, pl::M_Source), static_cast<float>(pl::SrcEnv3)}, {pl::modP(slot, pl::M_Dest), static_cast<float>(pl::oscP(0, pl::O_Level) + 1)},
                           {pl::modP(slot, pl::M_Amount), -0.5f}});
    diff = 0, ref2 = 0;
    for (size_t i = static_cast<size_t>(at + 2048) * 2; i < c.size(); ++i) diff += std::fabs(c[i] - d[i]), ref2 += std::fabs(c[i]);
    std::printf("# late envelope link: %.5f of the signal differs\n", diff / std::max(1e-9, ref2));
    expect(!c.empty() && diff <= ref2 * 0.02, "an envelope linked mid-note continues from where it is");
  }
  // ---- CPU: a 1-layer plain patch against everything on (8 notes, best of 3)
  {
    PluginState one = base();
    for (int o = 0; o < pl::kOscs; ++o) setP(one, pl::oscP(o, pl::O_Enable), o == 0 ? 1.f : 0.f);
    setP(one, pl::subP(pl::S_Enable), 0);
    setP(one, pl::noiseP(pl::N_Enable), 0);
    for (int f = 0; f < pl::kFilters; ++f) setP(one, pl::filtP(f, pl::F_Enable), 0);
    const double t1 = seconds(one, 8), tAll = seconds(everythingOn(), 8);
    std::printf("# CPU, 8 notes for 1 s: 1 layer %.2f ms, everything on %.2f ms (%.0f %%)\n", t1 * 1000, tAll * 1000, 100.0 * t1 / std::max(1e-9, tAll));
    expect(t1 > 0 && tAll > 0 && t1 < tAll * 0.4, "a 1-layer plain patch costs well under half of an everything-on patch");
  }
  return finish();
}
