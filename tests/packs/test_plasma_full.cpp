// Gridline: Plasma's full pass. Every part of the sound, one by one, as the editor offers them:
//  - each source block (three layers, Root, Texture) sounds alone and is silent when off; each
//    filter changes the sound when on and passes it untouched when off; every parameter of every
//    block, LFO, envelope and macro at its minimum and maximum renders finite audio with no
//    blow-up and no denormal output;
//  - every modulation source drives a link by source x amount (the matrix output the editor
//    shows), a linked constant moves a layer's level exactly as the knob would, and a macro,
//    velocity, the mod wheel, pressure, LFOs and envelopes each move their target;
//  - every effect type works, bypasses bit-exactly and is block-size invariant;
//  - a sound saved and opened again renders bit-identically;
//  - polyphony and its limit, voice stealing, mono, legato (no new attack), glide, slides,
//    pitch bend and per-note bend with MPE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Denormals.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

using namespace gl;
namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 48000;
void expect(bool cond, const std::string& name) { ::expect(cond, name.c_str()); }

struct Ev {
  int at;
  int kind;  // 0 on, 1 off, 2 midi, 3 glide
  uint32_t id;
  double pitch;
  float vel;
  NoteProps props{};
  uint8_t st = 0, d1 = 0, d2 = 0;
  double secs = 0;
};
Ev on(int at, uint32_t id, double pitch, float vel = 0.8f) { return {at, 0, id, pitch, vel}; }
Ev off(int at, uint32_t id) { return {at, 1, id, 0, 0}; }
Ev midi(int at, uint8_t st, uint8_t d1, uint8_t d2) {
  Ev e{at, 2, 0, 0, 0};
  e.st = st, e.d1 = d1, e.d2 = d2;
  return e;
}

struct Out {
  std::vector<float> L, R;
  bool finite = true;
  long subnormal = 0;
  float peak = 0;
  std::vector<pl::LiveView> live;  // one per block (when asked)
};

Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, bool watch = false) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f), o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96, ctx.playing = true;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    ctx.tick = pos / kSr * 2.0 * ctx.ppq;
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        const int at = e.at - pos;
        if (e.kind == 0) g->noteOn(e.id, e.pitch, e.vel, at, e.props);
        else if (e.kind == 1) g->noteOff(e.id, at);
        else if (e.kind == 2) g->midi(e.st, e.d1, e.d2, at);
        else g->glide(e.id, e.pitch, e.secs, at);
      }
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    g->process(st, aux, ctx, L.data(), R.data(), n);
    for (int i = 0; i < n; ++i) {
      const float l = L[static_cast<size_t>(i)], r = R[static_cast<size_t>(i)];
      o.L[static_cast<size_t>(pos + i)] = l, o.R[static_cast<size_t>(pos + i)] = r;
      if (!std::isfinite(l) || !std::isfinite(r)) o.finite = false;
      if (std::fpclassify(l) == FP_SUBNORMAL || std::fpclassify(r) == FP_SUBNORMAL) ++o.subnormal;
      if (std::isfinite(l)) o.peak = std::max(o.peak, std::fabs(l));
      if (std::isfinite(r)) o.peak = std::max(o.peak, std::fabs(r));
    }
    if (watch) {
      pl::LiveView lv;
      pl::liveView(g.get(), lv);
      o.live.push_back(lv);
    }
  }
  return o;
}

double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
  return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0;
}
// The frequency of a plain wave from its rising zero crossings (between frames a and b).
double freqOf(const std::vector<float>& x, size_t a, size_t b) {
  std::vector<double> cr;
  for (size_t i = a + 1; i < b && i < x.size(); ++i)
    if (x[i - 1] <= 0 && x[i] > 0) cr.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
  if (cr.size() < 3) return 0;
  return (static_cast<double>(cr.size()) - 1) * kSr / (cr.back() - cr.front());
}
bool same(const Out& a, const Out& b) { return a.L == b.L && a.R == b.R; }
double maxDiff(const Out& a, const Out& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size() && i < b.L.size(); ++i) d = std::max({d, std::fabs(static_cast<double>(a.L[i]) - b.L[i]), std::fabs(static_cast<double>(a.R[i]) - b.R[i])});
  return d;
}

void setP(PluginState& s, int k, float v) {
  if (static_cast<int>(s.params.size()) <= k) s.params.resize(static_cast<size_t>(k + 1), 0.f);
  s.params[static_cast<size_t>(k)] = v;
}
float getP(const PluginState& s, int k) { return k < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(k)] : 0.f; }
const PluginInfo& info() { return *findPlugin(pl::kType); }
const ParamDesc& desc(int k) { return info().params[static_cast<size_t>(k)]; }

// Every source and filter off: nothing sounds.
PluginState allOff() {
  PluginState s = defaultPluginState(pl::kType);
  for (int o = 0; o < pl::kOscs; ++o) setP(s, pl::oscP(o, pl::O_Enable), 0);
  setP(s, pl::subP(pl::S_Enable), 0), setP(s, pl::noiseP(pl::N_Enable), 0);
  for (int f = 0; f < pl::kFilters; ++f) setP(s, pl::filtP(f, pl::F_Enable), 0);
  for (int k = 0; k < pl::kFxSlots; ++k) setP(s, pl::fxP(k, pl::X_Type), 0);
  return s;
}
// Only Root, a plain sine (clean pitch measurements).
PluginState sineOnly() {
  PluginState s = allOff();
  setP(s, pl::subP(pl::S_Enable), 1), setP(s, pl::subP(pl::S_Shape), 0), setP(s, pl::subP(pl::S_Octave), 0);
  setP(s, pl::subP(pl::S_Level), 0.8f);
  return s;
}
void link(PluginState& s, int slot, int src, int dest, float amount) {
  setP(s, pl::modP(slot, pl::M_Source), static_cast<float>(src));
  setP(s, pl::modP(slot, pl::M_Dest), static_cast<float>(dest + 1));
  setP(s, pl::modP(slot, pl::M_Amount), amount);
}
const std::vector<Ev> kHold = {on(0, 1, 60)};
const std::vector<Ev> kNote = {on(0, 1, 60), off(9000, 1)};

// The parameter range of each block: (first, count, name).
struct Range {
  int lo, n;
  std::string name;
};
std::vector<Range> blocks() {
  std::vector<Range> r;
  for (int o = 0; o < pl::kOscs; ++o) r.push_back({pl::oscP(o, 0), pl::O_Count, "Layer " + std::to_string(o + 1)});
  r.push_back({pl::subP(0), pl::S_Count, "Root"});
  r.push_back({pl::noiseP(0), pl::N_Count, "Texture"});
  for (int f = 0; f < pl::kFilters; ++f) r.push_back({pl::filtP(f, 0), pl::F_Count, "Filter " + std::to_string(f + 1)});
  for (int e = 0; e < pl::kEnvs; ++e) r.push_back({pl::envP(e, 0), pl::E_Count, "Envelope " + std::to_string(e + 1)});
  for (int l = 0; l < pl::kLfos; ++l) r.push_back({pl::lfoP(l, 0), pl::L_Count, "LFO " + std::to_string(l + 1)});
  r.push_back({pl::macroP(0), pl::kMacros, "Macros"});
  r.push_back({0, pl::G_Count, "Voices and global"});
  return r;
}
// A patch where every block is placed and every movement source is linked, so each parameter matters.
PluginState everyBlock() {
  PluginState s = defaultPluginState(pl::kType);
  for (int o = 0; o < pl::kOscs; ++o) setP(s, pl::oscP(o, pl::O_Enable), 1), setP(s, pl::oscP(o, pl::O_Level), 0.5f);
  setP(s, pl::subP(pl::S_Enable), 1), setP(s, pl::noiseP(pl::N_Enable), 1);
  for (int f = 0; f < pl::kFilters; ++f) setP(s, pl::filtP(f, pl::F_Enable), 1);
  setP(s, pl::routeP(pl::RtA, pl::R_Dest), pl::RouteFilter), setP(s, pl::routeP(pl::RtA, pl::R_Balance), -1);
  setP(s, pl::routeP(pl::RtB, pl::R_Dest), pl::RouteFilter), setP(s, pl::routeP(pl::RtB, pl::R_Balance), 1);
  int slot = 0;
  for (int l = 0; l < pl::kLfos; ++l) link(s, slot++, pl::SrcLfo1 + l, pl::oscP(l % 3, pl::O_WtPos), 0.3f);
  for (int e = 1; e < pl::kEnvs; ++e) link(s, slot++, pl::SrcEnv1 + e, pl::filtP(e % 2, pl::F_Freq), 0.3f);
  for (int m = 0; m < pl::kMacros; ++m) link(s, slot++, pl::SrcMacro1 + m, pl::oscP(m % 3, pl::O_Level), 0.2f);
  return s;
}
}  // namespace

int main() {
  flushDenormals();  // as every DSP thread of the engine runs
  // ================================================================ 1. blocks
  {
    const Out silent = render(allOff(), kNote, 12000, 256);
    expect(silent.peak == 0.f, "blocks: every source off is silent");
    const char* names[5] = {"Layer 1", "Layer 2", "Layer 3", "Root", "Texture"};
    for (int b = 0; b < 5; ++b) {
      PluginState s = allOff();
      const int en = b < 3 ? pl::oscP(b, pl::O_Enable) : b == 3 ? pl::subP(pl::S_Enable) : pl::noiseP(pl::N_Enable);
      if (b < 3) setP(s, pl::oscP(b, pl::O_Level), 0.7f);
      setP(s, en, 1);
      const Out o = render(s, kNote, 12000, 256);
      expect(o.finite && o.peak > 1e-3f, std::string("blocks: ") + names[b] + " sounds on its own");
      setP(s, en, 0);
      expect(render(s, kNote, 12000, 256).peak == 0.f, std::string("blocks: ") + names[b] + " off (bypassed) is silent");
    }
    for (int f = 0; f < pl::kFilters; ++f) {
      PluginState s = allOff();
      setP(s, pl::oscP(0, pl::O_Enable), 1), setP(s, pl::oscP(0, pl::O_Level), 0.7f);
      const Out plain = render(s, kHold, 12000, 256);
      setP(s, pl::routeP(pl::RtA, pl::R_Dest), pl::RouteFilter), setP(s, pl::routeP(pl::RtA, pl::R_Balance), f == 0 ? -1.f : 1.f);
      setP(s, pl::filtP(f, pl::F_Enable), 1), setP(s, pl::filtP(f, pl::F_Freq), static_cast<float>(desc(pl::filtP(f, pl::F_Freq)).min));
      const Out lp = render(s, kHold, 12000, 256);
      expect(lp.finite && rms(lp.L, 4000, 12000) < 0.7 * rms(plain.L, 4000, 12000), "blocks: Filter " + std::to_string(f + 1) + " on changes the sound");
      setP(s, pl::filtP(f, pl::F_Enable), 0);
      const Out by = render(s, kHold, 12000, 256);
      expect(maxDiff(by, plain) < 1e-6, "blocks: Filter " + std::to_string(f + 1) + " off passes the sound untouched");
    }
    // every parameter of every block at its extremes, with every block placed and linked
    const PluginState full = everyBlock();
    for (const auto& r : blocks()) {
      int bad = 0, sub = 0, loud = 0, tried = 0;
      std::string first;
      for (int k = r.lo; k < r.lo + r.n; ++k) {
        const ParamDesc& d = desc(k);
        if (k == pl::G_Poly || k == pl::G_ArpOn || k == pl::G_ClipOn || k == pl::G_MidiOut || k == pl::G_NoteLatch) continue;  // notes, not sound
        const double hi = d.kind == ParamKind::Choice && !d.choices.empty() ? static_cast<double>(d.choices.size() - 1) : d.max;
        for (double v : {d.min, hi}) {
          PluginState s = full;
          setP(s, k, static_cast<float>(v));
          const Out o = render(s, kNote, 14400, 512);
          ++tried;
          if (!o.finite) ++bad, first = first.empty() ? d.name : first;
          if (o.subnormal) ++sub, first = first.empty() ? d.name + " (denormal)" : first;
          if (o.peak > 64.f) ++loud, first = first.empty() ? d.name + " (blow-up)" : first;
        }
      }
      if (bad || sub || loud) std::printf("# %s: %d not finite, %d denormal, %d blow-ups of %d (first: %s)\n", r.name.c_str(), bad, sub, loud, tried, first.c_str());
      expect(bad == 0 && sub == 0 && loud == 0, "blocks: " + r.name + ": every parameter at its extremes is finite, without denormals or blow-ups");
    }
  }

  // ================================================================ 2. wavetables
  {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("gl-plasma-full-" + std::to_string(static_cast<long>(std::rand())));
    fs::remove_all(base);
    auto wav = [](const fs::path& p, int frames) {
      fs::create_directories(p.parent_path());
      AudioBuffer b(1, static_cast<size_t>(2048 * frames));
      for (size_t i = 0; i < b.frames; ++i) b.data[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * 6.2831853 / 2048.0 * (1 + static_cast<double>(i / 2048))));
      writeWav(p.u8string(), b, 44100, 16);
    };
    const fs::path user = base / "user", imported = user / "Imported.wav", root = base / "lib", tables = root / "Tables";
    wav(user / "Mine.wav", 4), wav(imported, 2), wav(tables / "Analog" / "Stack.wav", 8), wav(tables / "Top.wav", 1);
    const pl::TableCatalog cat = pl::buildTableCatalog({user.u8string()}, pl::libraryTableDirs({root.u8string()}));
    const int factory = static_cast<int>(pl::factoryTableNames().size());
    expect(static_cast<int>(cat.entries.size()) == factory + 4, "tables: the factory tables, your folder, an imported file and the installed library are listed");
    bool inPlace = true;
    for (const auto& e : cat.entries)
      if (e.factory == 0) inPlace = inPlace && e.ref.rfind("file:", 0) == 0 && fs::exists(fs::u8path(e.ref.substr(5))) && fs::u8path(e.ref.substr(5)).parent_path().u8string().rfind(base.u8string(), 0) == 0;
    expect(inPlace, "tables: files are read where they are (never copied)");
    int plays = 0, silent = 0;
    for (const auto& e : cat.entries) {
      PluginState s = allOff();
      setP(s, pl::oscP(0, pl::O_Enable), 1), setP(s, pl::oscP(0, pl::O_Level), 0.7f);
      pl::Model m = pl::defaultModel();
      if (e.factory > 0) setP(s, pl::oscP(0, pl::O_Table), static_cast<float>(e.factory));
      else m.osc[0].table = e.ref, setP(s, pl::oscP(0, pl::O_Table), 0);
      s.data = pl::modelToData(m);
      const Out o = render(s, kNote, 9600, 512);
      if (o.finite && o.peak > 1e-3f) ++plays;
      else ++silent, std::printf("# table %s does not play\n", e.name.c_str());
    }
    expect(silent == 0 && plays == static_cast<int>(cat.entries.size()), "tables: every listed table loads into a layer and plays");
    fs::remove_all(base);
  }

  // ================================================================ 3. links
  {
    // the matrix output the editor shows is source x amount, for every source
    int wrong = 0, still = 0, checked = 0;
    std::string firstWrong, firstStill;
    for (int src = pl::SrcNone + 1; src < pl::SourceCount; ++src) {
      PluginState s = sineOnly();
      for (int o = 0; o < pl::kOscs; ++o) setP(s, pl::oscP(o, pl::O_Enable), 1), setP(s, pl::oscP(o, pl::O_Level), 0.3f);
      setP(s, pl::noiseP(pl::N_Enable), 1), setP(s, pl::filtP(0, pl::F_Enable), 1), setP(s, pl::filtP(1, pl::F_Enable), 1);
      setP(s, pl::routeP(pl::RtA, pl::R_Dest), pl::RouteFilter), setP(s, pl::routeP(pl::RtA, pl::R_Balance), 0);  // into both filters
      for (int m = 0; m < pl::kMacros; ++m) setP(s, pl::macroP(m), 0.7f);
      for (int l = 0; l < pl::kLfos; ++l) setP(s, pl::lfoP(l, pl::L_Bpm), 0), setP(s, pl::lfoP(l, pl::L_Rate), 0.35f);
      setP(s, pl::envP(1, pl::E_Sustain), 0.6f), setP(s, pl::envP(2, pl::E_Sustain), 0.6f), setP(s, pl::envP(3, pl::E_Sustain), 0.6f);
      link(s, 0, src, pl::filtP(0, pl::F_Freq), 0.5f);
      NoteProps np;
      np.pan = 0.4f, np.modX = 0.8f, np.modY = 0.7f, np.release = 0.6f;
      std::vector<Ev> ev = {midi(0, 0xB0, 1, 100), midi(0, 0xD0, 90, 0), midi(0, 0xE0, 0, 96), on(0, 1, 64, 0.8f), midi(256, 0xA0, 64, 80)};
      ev[3].props = np;
      const Out o = render(s, ev, 24000, 256, true);
      // slow sources: compare in the same block; audio-rate and stepped sources only move
      const bool fast = src == pl::SrcNoise || (src >= pl::SrcOscA && src <= pl::SrcSub) || src == pl::SrcFilter1 || src == pl::SrcFilter2 ||
                        src == pl::SrcRand1 || src == pl::SrcRand2 || src == pl::SrcRandDiscrete || (src >= pl::SrcLfo1 && src <= pl::SrcLfo10Y);
      bool moved = false, ok = o.finite;
      for (size_t b = 8; b < o.live.size(); ++b) {
        const float sv = o.live[b].source[src], mv = o.live[b].mod[0];
        if (std::fabs(mv) > 1e-4f) moved = true;
        if (!fast && std::fabs(mv - 0.5f * sv) > 0.02f) ok = false;
      }
      ++checked;
      const bool idle = src == pl::SrcReleaseVelo || src == pl::SrcActiveVoices || src == pl::SrcVoiceIndex || src == pl::SrcAlt1 || src == pl::SrcAlt2 ||
                        src == pl::SrcVoiceMod1 || src == pl::SrcVoiceMod2 || src == pl::SrcPolyAT || src == pl::SrcExprZ;  // may rest at zero for one held note
      if (!ok) ++wrong, firstWrong = firstWrong.empty() ? pl::sourceNames()[static_cast<size_t>(src)] : firstWrong;
      if (!moved && !idle) ++still, firstStill = firstStill.empty() ? pl::sourceNames()[static_cast<size_t>(src)] : firstStill;
    }
    std::printf("# links: %d sources, %d off the source x amount law (%s), %d that never moved (%s)\n", checked, wrong, firstWrong.c_str(), still, firstStill.c_str());
    expect(wrong == 0, "links: every source drives its link by source x amount");
    expect(still == 0, "links: every source that has a value while a note is held moves its link");
    // a constant linked to a layer's level moves it exactly as the knob would
    {
      PluginState a = allOff();
      setP(a, pl::oscP(0, pl::O_Enable), 1), setP(a, pl::oscP(0, pl::O_Level), 0.f);
      link(a, 0, pl::SrcFixed, pl::oscP(0, pl::O_Level), 0.5f);
      PluginState b = allOff();
      setP(b, pl::oscP(0, pl::O_Enable), 1), setP(b, pl::oscP(0, pl::O_Level), 0.5f);
      const double ra = rms(render(a, kHold, 12000, 256).L, 4000, 12000), rb = rms(render(b, kHold, 12000, 256).L, 4000, 12000);
      std::printf("# constant link to level 0.5: %.5f vs knob %.5f\n", ra, rb);
      expect(rb > 1e-3 && std::fabs(ra / rb - 1) < 0.01, "links: a linked constant moves a layer's level exactly as the knob");
      PluginState m = a;
      link(m, 0, pl::SrcMacro1, pl::oscP(0, pl::O_Level), 1.f), setP(m, pl::macroP(0), 0.5f);
      const double rm = rms(render(m, kHold, 12000, 256).L, 4000, 12000);
      expect(std::fabs(rm / rb - 1) < 0.01, "links: a macro at half with a full link moves the level to half");
    }
    // targets move: velocity -> level, mod wheel -> cutoff, LFO -> pitch, envelope -> level
    {
      PluginState s = allOff();
      setP(s, pl::oscP(0, pl::O_Enable), 1), setP(s, pl::oscP(0, pl::O_Level), 0.f);
      link(s, 0, pl::SrcVelo, pl::oscP(0, pl::O_Level), 1.f);
      const double soft = rms(render(s, {on(0, 1, 60, 0.3f)}, 12000, 256).L, 4000, 12000), hard = rms(render(s, {on(0, 1, 60, 1.f)}, 12000, 256).L, 4000, 12000);
      expect(hard > soft * 1.5 && soft > 0, "links: velocity moves a layer's level");
      PluginState w = sineOnly();
      link(w, 0, pl::SrcModWheel, pl::subP(pl::S_Level), -1.f);
      const double down = rms(render(w, {midi(0, 0xB0, 1, 127), on(0, 1, 60)}, 12000, 256).L, 4000, 12000);
      const double up = rms(render(w, {midi(0, 0xB0, 1, 0), on(0, 1, 60)}, 12000, 256).L, 4000, 12000);
      expect(up > 0.05 && down < up * 0.2, "links: the mod wheel moves its target");
      PluginState p = sineOnly();
      link(p, 0, pl::SrcAftertouch, pl::subP(pl::S_Level), -1.f);
      const double pr = rms(render(p, {midi(0, 0xD0, 127, 0), on(0, 1, 60)}, 12000, 256).L, 4000, 12000);
      expect(pr < up * 0.2, "links: pressure moves its target");
      PluginState e = sineOnly();
      setP(e, pl::subP(pl::S_Level), 0.f);
      setP(e, pl::envP(1, pl::E_Attack), static_cast<float>(pl::envTimeInv(0.2))), setP(e, pl::envP(1, pl::E_Sustain), 1.f);
      link(e, 0, pl::SrcEnv2, pl::subP(pl::S_Level), 0.8f);
      const Out eo = render(e, kHold, 24000, 256);
      expect(rms(eo.L, 0, 2400) < 0.5 * rms(eo.L, 14400, 24000), "links: an envelope moves its target over time");
    }
  }

  // ================================================================ 4. effects
  {
    PluginState dry = allOff();
    setP(dry, pl::oscP(0, pl::O_Enable), 1), setP(dry, pl::oscP(0, pl::O_Level), 0.6f);
    const std::vector<Ev> ev = {on(0, 1, 60), off(16000, 1)};
    const Out plain = render(dry, ev, 30000, 256);
    for (int t = 1; t < pl::FxTypeCount; ++t) {
      const std::string nm = pl::fxTypeNames()[static_cast<size_t>(t)];
      PluginState s = dry;
      setP(s, pl::fxP(0, pl::X_Type), static_cast<float>(t)), setP(s, pl::fxP(0, pl::X_On), 1);
      for (int q = 0; q < static_cast<int>(pl::fxParams(t).size()); ++q) setP(s, pl::fxP(0, pl::X_P0 + q), pl::fxDefaultNorm(t, q));
      const Out a = render(s, ev, 30000, 256), b = render(s, ev, 30000, 64), c = render(s, ev, 30000, 1000);
      if (!a.finite || a.subnormal || a.peak >= 16.f) std::printf("# %s: finite %d denormal %ld peak %.3g\n", nm.c_str(), a.finite, a.subnormal, a.peak);
      expect(a.finite && a.subnormal == 0 && a.peak < 16.f, "effects: " + nm + " is finite");
      if (t < pl::FxSplitLH) {  // a split with no effects in its bands passes the sound as it is
        PluginState w = s;    // at its defaults it may be neutral (a flat EQ, a compressor above the level): turn it up
        for (int q = 0; q < static_cast<int>(pl::fxParams(t).size()); ++q) setP(w, pl::fxP(0, pl::X_P0 + q), 0.85f);
        expect(maxDiff(render(w, ev, 30000, 256), plain) > 1e-4, "effects: " + nm + " changes the sound");
      }
      const double inv = std::max(maxDiff(a, b), maxDiff(a, c));
      if (inv > 1e-5) std::printf("# %s block-size difference %.3g\n", nm.c_str(), inv);
      expect(inv <= 1e-5, "effects: " + nm + " is block-size invariant");
      setP(s, pl::fxP(0, pl::X_On), 0);
      expect(same(render(s, ev, 30000, 256), plain), "effects: " + nm + " off is bypassed bit-exactly");
    }
  }

  // ================================================================ 4b. effects added, reordered, removed
  {
    PluginState s = sineOnly();
    const Out plain = render(s, kNote, 24000, 256);
    setP(s, pl::fxP(0, pl::X_Type), pl::FxDistortion), setP(s, pl::fxP(0, pl::X_On), 1);
    setP(s, pl::fxP(1, pl::X_Type), pl::FxDelay), setP(s, pl::fxP(1, pl::X_On), 1);
    for (int k : {0, 1})
      for (int q = 0; q < static_cast<int>(pl::fxParams(static_cast<int>(getP(s, pl::fxP(k, pl::X_Type)))).size()); ++q)
        setP(s, pl::fxP(k, pl::X_P0 + q), 0.85f);
    pl::Model m = pl::defaultModel();
    m.fx = {pl::FxNode{0, 0, -1, 0}, pl::FxNode{1, 0, -1, 0}};
    s.data = pl::modelToData(m);
    const Out ab = render(s, kNote, 24000, 256);
    m.fx = {pl::FxNode{1, 0, -1, 0}, pl::FxNode{0, 0, -1, 0}};
    s.data = pl::modelToData(m);
    const Out ba = render(s, kNote, 24000, 256);
    expect(ab.finite && ba.finite && maxDiff(ab, plain) > 1e-3, "effects: two effects in a chain are added and change the sound");
    expect(maxDiff(ab, ba) > 1e-4, "effects: reordering the chain changes the result (distortion before or after the delay)");
    setP(s, pl::fxP(0, pl::X_Type), 0), setP(s, pl::fxP(1, pl::X_Type), 0);
    m.fx.clear();
    s.data = pl::modelToData(m);
    expect(same(render(s, kNote, 24000, 256), plain), "effects: removing them gives back the plain sound bit-exactly");
  }

  // ================================================================ 6. save and open again
  {
    PluginState s = everyBlock();
    pl::Model m = pl::defaultModel();
    m.lfo[0].pts = {{0, 0, 0}, {0.3f, 1, 0.4f}, {1, 0, 0}};
    m.macroName[0] = "Bite";
    s.data = pl::modelToData(m);
    setP(s, pl::fxP(0, pl::X_Type), pl::FxDelay), setP(s, pl::fxP(0, pl::X_On), 1);
    const Out a = render(s, kNote, 24000, 256);
    pl::Model back;
    const bool read = pl::modelFromData(s.data, back);
    PluginState t = s;
    t.data = pl::modelToData(back);
    expect(read && t.data == s.data, "save: the sound's data reads back the same");
    expect(same(a, render(t, kNote, 24000, 256)), "save: a sound saved and opened again renders bit-identically");
    PluginState shortV = s;
    shortV.params.resize(static_cast<size_t>(pl::kOscBase));  // a state from before most parameters existed
    normalizePluginState(shortV);
    expect(shortV.params.size() == info().params.size() && render(shortV, kNote, 12000, 256).finite, "save: an old short state opens padded and plays");
  }

  // ================================================================ 11. voices
  {
    PluginState s = sineOnly();
    std::vector<Ev> chord;
    for (int i = 0; i < 6; ++i) chord.push_back(on(i * 10, static_cast<uint32_t>(i + 1), 48 + i * 4));
    const Out poly = render(s, chord, 9600, 256, true);
    expect(poly.live.back().voices == 6, "voices: six keys play six notes");
    setP(s, pl::G_Poly, 4);
    const Out lim = render(s, chord, 9600, 256, true);
    expect(lim.finite && lim.live.back().voices == 4, "voices: a limit of four keeps four notes (the oldest stolen)");
    double jump = 0;  // stealing: no click larger than the wave's own steps
    for (size_t i = 1; i < lim.L.size(); ++i) jump = std::max(jump, std::fabs(static_cast<double>(lim.L[i]) - lim.L[i - 1]));
    expect(jump < 0.2, "voices: stealing is click-free");
    PluginState mono = sineOnly();
    setP(mono, pl::G_Mono, 1);
    const Out mo = render(mono, {on(0, 1, 60), on(4800, 2, 67)}, 14400, 256, true);
    expect(mo.live.back().voices == 1, "voices: mono plays one note");
    const double f1 = freqOf(mo.L, 9600, 14400), want = 440.0 * std::pow(2.0, (67 - 69) / 12.0);
    expect(std::fabs(f1 / want - 1) < 0.01, "voices: mono follows the newest key");
    // legato: the second key does not start the envelope again
    PluginState leg = mono;
    setP(leg, pl::G_Legato, 1), setP(leg, pl::envP(0, pl::E_Attack), static_cast<float>(pl::envTimeInv(0.08)));
    const Out lo = render(leg, {on(0, 1, 60), on(9600, 2, 64)}, 14400, 256, true);
    bool attacked = false;
    for (size_t b = 9600 / 256 + 1; b < lo.live.size(); ++b) attacked = attacked || lo.live[b].envStage[0] == 1;
    expect(!attacked, "voices: legato keeps the envelope going on a new key");
    PluginState rel = mono;
    setP(rel, pl::envP(0, pl::E_Attack), static_cast<float>(pl::envTimeInv(0.08)));
    const Out ro = render(rel, {on(0, 1, 60), on(9600, 2, 64)}, 14400, 256, true);
    bool again = false;
    for (size_t b = 9600 / 256 + 1; b < ro.live.size(); ++b) again = again || ro.live[b].envStage[0] == 1;
    expect(again, "voices: mono without legato starts the envelope again");
    // glide: the pitch travels from the old key to the new one
    PluginState gl = mono;
    setP(gl, pl::G_PortaTime, static_cast<float>(std::pow(0.3 / 8.0, 0.2))), setP(gl, pl::G_Legato, 1);
    const Out go = render(gl, {on(0, 1, 60), on(9600, 2, 72)}, 9600 + 24000, 256);
    const double mid = freqOf(go.L, 9600 + 2400, 9600 + 4800), end = freqOf(go.L, 9600 + 19200, 9600 + 24000);
    const double fa = 440.0 * std::pow(2.0, (60 - 69) / 12.0), fb = fa * 2;
    expect(mid > fa * 1.03 && mid < fb * 0.97 && std::fabs(end / fb - 1) < 0.01, "voices: glide travels to the new key");
    // a slide (the host's glide of a sounding note)
    Ev sl{4800, 3, 1, 72, 0};
    sl.secs = 0.05;
    const Out so = render(sineOnly(), {on(0, 1, 60), sl}, 19200, 256);
    expect(std::fabs(freqOf(so.L, 12000, 19200) / fb - 1) < 0.01, "voices: a slide moves the sounding note to the new pitch");
    // pitch bend: the wheel bends every note by the bend range
    PluginState bend = sineOnly();
    setP(bend, pl::G_BendUp, 2);
    const Out bo = render(bend, {midi(0, 0xE0, 127, 127), on(0, 1, 60)}, 14400, 256);
    expect(std::fabs(freqOf(bo.L, 4800, 14400) / (fa * std::pow(2.0, 2.0 / 12)) - 1) < 0.01, "voices: the bend wheel bends by its range");
    // MPE: a channel's bend moves only the note on that channel
    PluginState mpe = sineOnly();
    setP(mpe, pl::G_MpeOn, 1), setP(mpe, pl::G_MpeBend, 12);
    Ev n1 = on(0, 1, 60);
    n1.props.color = 1;
    const Out own = render(mpe, {n1, midi(0, 0xE1, 0, 96)}, 14400, 256), other = render(mpe, {n1, midi(0, 0xE2, 0, 96)}, 14400, 256);
    const double fo = freqOf(own.L, 4800, 14400), ft = freqOf(other.L, 4800, 14400);
    std::printf("# MPE: own channel %.2f Hz, other channel %.2f Hz (key %.2f)\n", fo, ft, fa);
    expect(fo > fa * 1.2 && std::fabs(ft / fa - 1) < 0.01, "voices: with MPE a channel's bend moves only its own note");
  }
  return finish();
}
