// Gridline — "seltmann" pack: `gridline seltmann-analyse`, the command-line
// analysis harness. Analyses audio files the way the editor does (analysis,
// then the automatic chop), prints one line (or one JSON object) per file with
// the tempo, the file-name tempo, the material kind, the chop and its slice
// statistics, and optionally applies every layout and renders it to check for
// clicks and missing slices. Used to tune the analysis on large sample sets.
//
//   gridline seltmann-analyse [--json] [-j N] [--list <file>] [--presets] <files...>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../engine/TimeStretch.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

namespace {

std::string jsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

struct SliceStats {
  int count = 0, degenerate = 0;
  double cv = 0, firstRatio = 1, minRatio = 1;
};

// Slice statistics of a model: the nominal slice length is the grid cell for
// grid chops, else the median; a slice shorter than half of it is degenerate.
SliceStats sliceStats(const Model& m) {
  SliceStats s;
  s.count = static_cast<int>(m.slices.size());
  if (m.slices.empty()) return s;
  std::vector<double> len;
  for (const auto& sl : m.slices) len.push_back(static_cast<double>(sl.end - sl.start));
  std::vector<double> sorted = len;
  std::sort(sorted.begin(), sorted.end());
  double nominal = sorted[sorted.size() / 2];
  if (m.chop.mode == ChopGrid) nominal = divisionBeats(m.chop.division, m.grid.beatsPerBar) * beatFrames(m.grid);
  const double minLen = degenerateLimit(m);
  if (len.size() > 1)  // a single slice is the whole sound
    for (double l : len)
      if (l < minLen) ++s.degenerate;
  double mean = 0;
  for (double l : len) mean += l;
  mean /= static_cast<double>(len.size());
  double var = 0;
  for (double l : len) var += (l - mean) * (l - mean);
  s.cv = mean > 0 ? std::sqrt(var / static_cast<double>(len.size())) / mean : 0;
  s.firstRatio = nominal > 0 ? len.front() / nominal : 1;
  s.minRatio = nominal > 0 ? sorted.front() / nominal : 1;
  return s;
}

// Largest sample-to-sample step of a buffer.
double maxStep(const std::vector<float>& x) {
  double m = 0;
  for (size_t i = 1; i < x.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  return m;
}

struct PresetCheck {
  int applied = 0, degenerate = 0, missingRefs = 0, clicks = 0, renders = 0;
  int onTransient = 0, starts = 0;
};

void checkPresets(const Sample& s, const Model& base, PresetCheck& pc) {
  // the source's own largest step over the region (clicks are steps well above it)
  std::vector<float> mono(s.frames());
  const bool st = !s.mono() && s.right.size() == s.left.size();
  for (size_t i = 0; i < mono.size(); ++i) mono[i] = st ? 0.5f * (s.left[i] + s.right[i]) : s.left[i];
  const std::vector<int64_t> on = stretch::detectOnsets(mono.data(), static_cast<int64_t>(mono.size()), s.sampleRate, 0.5);
  SamplePtr src = std::make_shared<Sample>(s);
  for (const auto& p : factoryPresets()) {
    Model m = base;
    applyPreset(m, &s, p, 7);
    ++pc.applied;
    pc.degenerate += sliceStats(m).degenerate;
    const int n = static_cast<int>(m.slices.size());
    for (const auto& stp : m.pattern.steps) {
      if (stp.src == kRest) continue;
      if (!stp.slot && stp.src >= n) ++pc.missingRefs;
      if (stp.slot && (stp.src >= static_cast<int>(m.pattern.slots.size()) || m.pattern.slots[static_cast<size_t>(stp.src)] < 0 ||
                       m.pattern.slots[static_cast<size_t>(stp.src)] >= n))
        ++pc.missingRefs;
    }
    // slice starts on transients (grid / transient chops of percussive material)
    // (counted where a hit sits within a quarter of a slice of the cut: does the cut land on it?)
    if (m.chop.mode == ChopGrid || m.chop.mode == ChopTransients)
      for (const auto& sl : m.slices) {
        const int64_t near = std::max<int64_t>(static_cast<int64_t>(0.015 * s.sampleRate), (sl.end - sl.start) / 4);
        auto it = std::lower_bound(on.begin(), on.end(), sl.start - near);
        if (it == on.end() || *it > sl.start + near) continue;
        ++pc.starts;
        const int64_t tol = static_cast<int64_t>(0.015 * s.sampleRate);
        auto jt = std::lower_bound(on.begin(), on.end(), sl.start - tol);
        if (jt != on.end() && *jt <= sl.start + tol) ++pc.onTransient;
      }
    RenderOpts o;
    o.sampleRate = s.sampleRate;
    o.tempo = m.grid.bpm > 0 ? m.grid.bpm : 120;
    o.sync = SyncOriginal;
    std::vector<float> L, R;
    if (renderPatternLoop(m, src, o, L, R) && !L.empty()) {
      ++pc.renders;
      // a click: a step much larger than the source's largest step over the slices played
      // (the left channel against the source's left channel: a hard-panned hat steps twice as far as the mono mix)
      double srcStep = 0;
      for (int64_t i = std::max<int64_t>(1, m.regionStart); i < m.regionEnd && i < static_cast<int64_t>(s.left.size()); ++i)
        srcStep = std::max(srcStep, static_cast<double>(std::fabs(s.left[static_cast<size_t>(i)] - s.left[static_cast<size_t>(i - 1)])));
      // pitched steps (up to +12 st) legitimately double the steps: allow for the layout's highest pitch
      float up = 0;
      for (const auto& stp : m.pattern.steps) up = std::max(up, stp.pitch);
      const double allow = std::pow(2.0, up / 12.0);
      if (maxStep(L) > srcStep * 1.6 * allow + 0.02) {
        if (std::getenv("SELTMANN_CLICKS")) {
          size_t at = 1;
          for (size_t i = 1; i < L.size(); ++i)
            if (std::fabs(L[i] - L[i - 1]) > std::fabs(L[at] - L[at - 1])) at = i;
          std::fprintf(stderr, "  at %zu of %zu (%.4f s), region %lld..%lld, slices %zu, first slice %lld..%lld, unit %.4f\n", at, L.size(),
                       static_cast<double>(at) / o.sampleRate, static_cast<long long>(m.regionStart), static_cast<long long>(m.regionEnd),
                       m.slices.size(), static_cast<long long>(m.slices[0].start), static_cast<long long>(m.slices[0].end), m.pattern.unitBeats);
        }
        ++pc.clicks;
        if (std::getenv("SELTMANN_CLICKS")) std::fprintf(stderr, "click: %s  %.3f > %.3f  %s\n", p.name.c_str(), maxStep(L), srcStep, s.sampleRate > 0 ? "" : "");
      }
    }
  }
}

bool gNoHints = false;  // --no-hints: the audio alone (no file-name tempo / key)

std::string analyseOne(const std::string& path, bool json, bool presets, bool& ok) {
  ok = false;
  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  SamplePtr s = loadSample(path, &err);
  char b[2048];
  if (!s || s->frames() == 0) {
    std::snprintf(b, sizeof b, json ? "{\"file\":\"%s\",\"ok\":false,\"error\":\"decode\"}" : "FAIL decode  %s", jsonEsc(path).c_str());
    return b;
  }
  const auto t1 = std::chrono::steady_clock::now();
  Analysis a;
  const bool good = analyse(*s, a, nullptr, nullptr, gNoHints ? AnalyseHints{} : analyseHintsFor(path));
  const auto t2 = std::chrono::steady_clock::now();
  if (!good) {
    std::snprintf(b, sizeof b, json ? "{\"file\":\"%s\",\"ok\":false,\"error\":\"analysis\"}" : "FAIL analysis  %s", jsonEsc(path).c_str());
    return b;
  }
  Model m = defaultModel(*s, a);
  const SliceStats st = sliceStats(m);
  const double secs = s->seconds();
  const AnalyseHints h = analyseHintsFor(path);
  const double barSec = 60.0 / std::max(1.0, a.bpm) * std::max(1, a.beatsPerBar);
  const double regionBars = static_cast<double>(m.regionEnd - m.regionStart) / s->sampleRate / barSec;
  const double fileBars = secs / barSec;
  const double decodeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double anaMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
  PresetCheck pc;
  if (presets) checkPresets(*s, m, pc);
  ok = true;
  if (json) {
    std::snprintf(b, sizeof b,
                  "{\"file\":\"%s\",\"ok\":true,\"secs\":%.3f,\"rate\":%.0f,\"bpm\":%.3f,\"nameBpm\":%.3f,\"nameStrong\":%d,\"conf\":%.3f,"
                  "\"bpb\":%d,\"bar1\":%d,\"bar1Sec\":%.4f,\"kind\":\"%s\",\"contentStart\":%.4f,\"chopMode\":%d,\"division\":%d,\"slices\":%d,"
                  "\"degenerate\":%d,\"cv\":%.4f,\"firstRatio\":%.4f,\"minRatio\":%.4f,\"regionStart\":%.4f,\"regionEnd\":%.4f,"
                  "\"regionBars\":%.4f,\"fileBars\":%.4f,\"loopFit\":%d,\"key\":%d,\"minor\":%d,\"nameKey\":%d,\"nameMinor\":%d,"
                  "\"decodeMs\":%.1f,\"analysisMs\":%.1f,\"presets\":%d,\"presetDegenerate\":%d,\"missingRefs\":%d,\"renders\":%d,"
                  "\"clicks\":%d,\"starts\":%d,\"onTransient\":%d,\"onsetRate\":%.3f,\"tonality\":%.3f}",
                  jsonEsc(path).c_str(), secs, s->sampleRate, a.bpm, h.bpm, h.strong ? 1 : 0, a.confidence, a.beatsPerBar, a.bar1,
                  a.beats.empty() ? 0.0 : a.beats[static_cast<size_t>(std::clamp(a.bar1, 0, static_cast<int>(a.beats.size()) - 1))] / s->sampleRate,
                  kindName(a.kind), static_cast<double>(a.contentStart) / s->sampleRate, m.chop.mode, m.chop.division, st.count, st.degenerate,
                  st.cv, st.firstRatio, st.minRatio, static_cast<double>(m.regionStart) / s->sampleRate,
                  static_cast<double>(m.regionEnd) / s->sampleRate, regionBars, fileBars, a.loopFit ? 1 : 0, a.key, a.minor ? 1 : 0, h.key,
                  h.minor ? 1 : 0, decodeMs, anaMs, pc.applied, pc.degenerate, pc.missingRefs, pc.renders, pc.clicks, pc.starts, pc.onTransient, a.onsetRate, a.tonality);
  } else {
    std::snprintf(b, sizeof b, "%7.2f bpm (name %6.2f)  %-7s %2d slices  degenerate %d  first %.2f  cv %.3f  %6.0f ms  %s", a.bpm, h.bpm,
                  kindName(a.kind), st.count, st.degenerate, st.firstRatio, st.cv, anaMs, path.c_str());
  }
  return b;
}

}  // namespace

int cliMain(int argc, char** argv) {
  bool json = false, presets = false;
  int jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));
  std::vector<std::string> files;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") json = true;
    else if (a == "--presets") presets = true;
    else if (a == "--no-hints") gNoHints = true;
    else if (a == "-j" && i + 1 < argc) jobs = std::max(1, std::atoi(argv[++i]));
    else if (a == "--list" && i + 1 < argc) {
      std::ifstream in(argv[++i]);
      for (std::string l; std::getline(in, l);)
        if (!l.empty()) files.push_back(l);
    } else files.push_back(a);
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: gridline seltmann-analyse [--json] [-j N] [--presets] [--list <file>] <audio files...>\n");
    return 2;
  }
  std::vector<std::string> out(files.size());
  std::vector<char> okv(files.size(), 0);
  std::atomic<size_t> next{0};
  std::mutex mu;
  std::vector<std::thread> ts;
  for (int k = 0; k < jobs; ++k)
    ts.emplace_back([&] {
      for (size_t i; (i = next++) < files.size();) {
        bool ok = false;
        std::string l = analyseOne(files[i], json, presets, ok);
        std::lock_guard<std::mutex> lk(mu);
        out[i] = l;
        okv[i] = ok ? 1 : 0;
        std::printf("%s\n", l.c_str());
        std::fflush(stdout);
      }
    });
  for (auto& t : ts) t.join();
  int failed = 0;
  for (char c : okv) failed += c ? 0 : 1;
  std::fprintf(stderr, "%zu files, %d failed\n", files.size(), failed);
  return failed ? 1 : 0;
}

}  // namespace gl::packs::seltmann
