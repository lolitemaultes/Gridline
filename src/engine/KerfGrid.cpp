// Gridline — Kerf's slicing on the beat grid (see KerfGrid.h).
#include "KerfGrid.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include "../core/PluginParams.h"
#include "../core/PluginInfo.h"
#include "../packs/seltmann/seltmann.h"

namespace gl::kerfgrid {

namespace sm = packs::seltmann;

const char* divisionName(int div) {
  static const char* const n[DivCount] = {"1 bar", "1/2 bar", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/16 T"};
  return div >= 0 && div < DivCount ? n[div] : "";
}

const char* meterName(int meter) {
  static const char* const n[MeterCount] = {"Detected", "2/4", "3/4", "4/4", "5/4", "6/8"};
  return meter >= 0 && meter < MeterCount ? n[meter] : "";
}

double cellBeats(int div, int bpb, bool compound) {
  bpb = std::max(1, bpb);
  // A note value as a share of a whole note; a beat is a quarter (or a dotted quarter).
  const double beatWhole = compound ? 0.375 : 0.25;
  auto note = [&](double whole) { return whole / beatWhole; };
  switch (div) {
    case DivBar: return bpb;
    case DivHalfBar: return bpb / 2.0;
    case DivBeat: return note(1.0 / 4);
    case Div8th: return note(1.0 / 8);
    case Div16th: return note(1.0 / 16);
    case Div32nd: return note(1.0 / 32);
    case Div4T: return note(1.0 / 6);
    case Div8T: return note(1.0 / 12);
    case Div16T: return note(1.0 / 24);
    default: return note(1.0 / 16);
  }
}

namespace {
float par(const PluginState& st, int i, float def) { return static_cast<size_t>(i) < st.params.size() ? st.params[static_cast<size_t>(i)] : def; }
int ipar(const PluginState& st, int i, int def) { return static_cast<int>(std::lround(par(st, i, static_cast<float>(def)))); }
}  // namespace

Settings settingsOf(const PluginState& st) {
  using namespace pp;
  Settings s;
  s.division = std::clamp(ipar(st, SlGridDiv, Div16th), 0, DivCount - 1);
  s.meter = std::clamp(ipar(st, SlGridMeter, MeterDetected), 0, MeterCount - 1);
  s.hits = par(st, SlGridHits, 1) >= 0.5f;
  s.zero = par(st, SlGridZero, 1) >= 0.5f;
  s.follow = par(st, SlGridFollow, 1) >= 0.5f;
  s.loopBeats = par(st, SlLoopBeats, 4);
  s.fitted = par(st, SlFitTempo, 0) >= 0.5f;
  return s;
}

sm::Analysis loopGrid(const Sample& s, double beats, int bpb) {
  const double secs = s.seconds();
  sm::Analysis a = sm::flatGrid(s, secs > 0 && beats > 0 ? beats * 60.0 / secs : 120.0);
  a.beatsPerBar = std::max(1, bpb);
  a.bar1 = 0;
  return a;
}

std::vector<int64_t> cutsOn(const Sample& s, const sm::Analysis& g, double cell, bool hits, bool zero) {
  std::vector<int64_t> out;
  if (s.frames() == 0) return out;
  sm::Model m;
  m.srcFrames = static_cast<int64_t>(s.frames());
  m.srcRate = s.sampleRate;
  m.grid = g;
  m.analysed = true;
  m.regionStart = 0;
  m.regionEnd = m.srcFrames;
  m.chop.mode = sm::ChopGrid;
  m.chop.cellBeats = std::max(1.0 / 64, cell);
  m.chop.hitSnap = hits;
  m.chop.zeroCross = zero;
  m.chop.autoSize = false;
  sm::rechop(m, &s);  // Seltmann's grid chop: the same cuts it makes for a region
  out.reserve(m.slices.size());
  for (const auto& sl : m.slices) out.push_back(sl.start);
  return out;
}

float Result::progress() const { return job ? std::clamp(job->progress.load(), 0.0f, 1.0f) : (analysing ? 0.0f : 1.0f); }

kerfmarks::Grid Result::timeGrid() const {
  kerfmarks::Grid g;
  if (!grid) return g;
  g.fpb = sm::beatFrames(*grid);
  g.beatsPerBar = std::max(1, beatsPerBar);
  g.map = grid;
  return g;
}

namespace {

struct Cache {
  std::mutex mu;
  std::map<std::string, ResultPtr> map;
};
Cache& cache() {
  static Cache c;
  return c;
}

// The meter a setting asks for (beats per bar, compound), from the detected one.
void meterOf(int meter, int detBpb, bool detCompound, int& bpb, bool& compound) {
  switch (meter) {
    case Meter24: bpb = 2, compound = false; break;
    case Meter34: bpb = 3, compound = false; break;
    case Meter44: bpb = 4, compound = false; break;
    case Meter54: bpb = 5, compound = false; break;
    case Meter68: bpb = 2, compound = true; break;
    default: bpb = std::max(1, detBpb), compound = detCompound; break;
  }
}

}  // namespace

ResultPtr resultFor(const std::string& path, const SamplePtr& s, const Settings& set, bool wait) {
  if (!s || s->frames() == 0) return nullptr;
  sm::JobPtr job;
  if (set.follow && !path.empty()) {
    job = sm::requestAnalysis(path);
    // Offline renders wait for the analysis (short loops take well under a second).
    for (int i = 0; wait && job->state.load() == 0 && i < 300000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const int jobState = job ? job->state.load() : -1;
  char key[320];
  std::snprintf(key, sizeof key, "%p|%p|%d|%d|%d|%d|%d|%d|%.6g|%d", static_cast<const void*>(s.get()), static_cast<const void*>(job.get()), jobState,
                set.division, set.meter, set.hits ? 1 : 0, set.zero ? 1 : 0, set.follow ? 1 : 0, set.loopBeats, set.fitted ? 1 : 0);
  const std::string k = path + "|" + key;
  Cache& c = cache();
  {
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.map.find(k);
    if (it != c.map.end()) return it->second;
  }
  auto r = std::make_shared<Result>();
  r->job = job;
  const bool usable = jobState == 1 && job->result.frames == static_cast<int64_t>(s->frames()) && job->result.beats.size() >= 2 && job->result.bpm > 0;
  r->analysing = jobState == 0;
  r->failed = set.follow && (jobState == 2 || (jobState == 1 && !usable));
  auto g = std::make_shared<sm::Analysis>();
  if (usable) {
    *g = job->result;
    g->onsetEnv.clear();
    meterOf(set.meter, g->beatsPerBar, g->compound, r->beatsPerBar, r->compound);
    r->source = SourceDetected;
  } else {
    meterOf(set.meter, 4, false, r->beatsPerBar, r->compound);
    *g = loopGrid(*s, kerfmarks::loopBeats(s->seconds(), set.fitted, set.loopBeats), r->beatsPerBar);
    r->source = SourceLoop;
  }
  g->beatsPerBar = r->beatsPerBar;
  g->compound = r->compound;
  r->bpm = g->bpm;
  std::vector<int64_t> cuts = cutsOn(*s, *g, cellBeats(set.division, r->beatsPerBar, r->compound), set.hits, set.zero);
  r->found = static_cast<int>(cuts.size());
  if (cuts.size() > static_cast<size_t>(pp::kMaxSlices)) cuts.resize(static_cast<size_t>(pp::kMaxSlices));
  r->cuts = std::move(cuts);
  r->grid = g;
  std::lock_guard<std::mutex> lk(c.mu);
  if (c.map.size() > 64) c.map.clear();
  c.map[k] = r;
  return r;
}

uint64_t analysisGeneration() { return sm::analysisGeneration(); }

ResultPtr resultFor(const PluginState& st, const SamplePtr& s, bool wait) {
  return resultFor(kerfmarks::pathOf(st.data), s, settingsOf(st), wait);
}

}  // namespace gl::kerfgrid
