// Gridline — Loupe's spectrogram analysis (analyseSpecView): a sine lands on its row at its
// level (every band, mono, stereo, anti-phase, other sample rates, every resolution), close
// tones are told apart (a semitone at 100 Hz, 1 kHz and 8 kHz), a click stays sharp in time
// without smearing ahead of itself, short bursts between far-apart columns still show,
// silence stays at the floor, a zoomed view agrees with the whole, cancelling stops it, and
// how long a screen-sized image takes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../Check.h"
#include "packs/edison/edison.h"

using namespace gl::packs::edison;

namespace {

constexpr double kTau = 6.283185307179586;

Audio tones(const std::vector<std::pair<double, double>>& hzAmp, double seconds, double rate = 44100, int channels = 2, bool antiPhase = false) {
  Audio a = makeAudio(rate, channels, static_cast<int64_t>(seconds * rate));
  for (size_t i = 0; i < a.L.size(); ++i) {
    double v = 0;
    for (const auto& [hz, amp] : hzAmp) v += amp * std::sin(kTau * hz * static_cast<double>(i) / rate + hz * 0.001);
    a.L[i] = static_cast<float>(v);
    a.R[i] = static_cast<float>(antiPhase ? -v : v);
  }
  return a;
}

double rowHz(const SpecView& v, int r) {
  const double top = v.req.maxHz > 0 ? v.req.maxHz : v.rate * 0.5;
  return v.req.minHz * std::pow(top / v.req.minHz, 1.0 - (r + 0.5) / v.req.rows);
}
int rowOf(const SpecView& v, double hz) {
  int best = 0;
  for (int r = 0; r < v.req.rows; ++r)
    if (std::fabs(std::log(rowHz(v, r) / hz)) < std::fabs(std::log(rowHz(v, best) / hz))) best = r;
  return best;
}
// The loudest row within `oct` octaves of hz in a column.
int peakRow(const SpecView& v, int c, double hz, double oct = 0.25) {
  int best = -1;
  for (int r = 0; r < v.req.rows; ++r) {
    if (std::fabs(std::log2(rowHz(v, r) / hz)) > oct) continue;
    if (best < 0 || v.at(c, r) > v.at(c, best)) best = r;
  }
  return best;
}

SpecView view(const Audio& a, int cols, int rows, int res = 1, double from = 0, double to = -1) {
  SpecViewRequest q;
  q.from = static_cast<int64_t>(from * a.rate);
  q.to = to < 0 ? a.frames() : static_cast<int64_t>(to * a.rate);
  q.cols = cols, q.rows = rows, q.resolution = res;
  return analyseSpecView(a, q);
}

bool sineOk(const SpecView& v, double hz, double db, double tolDb, const char* what) {
  bool ok = true;
  double worstDb = 0, worstHz = 0;
  for (int c = v.req.cols / 4; c < v.req.cols * 3 / 4; c += std::max(1, v.req.cols / 16)) {
    const int r = peakRow(v, c, hz);
    const double got = v.at(c, r), dHz = std::fabs(std::log2(rowHz(v, r) / hz)) * 12;  // semitones
    worstDb = std::max(worstDb, std::fabs(got - db));
    worstHz = std::max(worstHz, dHz);
    ok = ok && std::fabs(got - db) <= tolDb && std::abs(r - rowOf(v, hz)) <= 1;
  }
  std::printf("# %s: level off by %.2f dB at most, row off by %.3f semitones\n", what, worstDb, worstHz);
  return ok;
}

}  // namespace

int main() {
  // ---- a sine on its row at its level, in every band and across the cross-fades
  {
    const double amp = 0.5, db = 20 * std::log10(amp);
    bool ok = true;
    for (double hz : {45.0, 100.0, 220.0, 400.0, 700.0, 1000.0, 1600.0, 3000.0, 6000.0, 12000.0, 18000.0}) {
      char name[64];
      std::snprintf(name, sizeof name, "sine %.0f Hz", hz);
      ok = sineOk(view(tones({{hz, amp}}, 3.0), 120, 700), hz, db, 1.0, name) && ok;
    }
    expect(ok, "a -6 dBFS sine reads -6 dB (within 1 dB) on its own row, 45 Hz to 18 kHz");
  }
  {
    const Audio st = tones({{1000, 0.25}}, 2.0), mono = tones({{1000, 0.25}}, 2.0, 44100, 1), anti = tones({{1000, 0.25}}, 2.0, 44100, 2, true);
    const double db = 20 * std::log10(0.25);
    expect(sineOk(view(mono, 80, 400), 1000, db, 1.0, "mono"), "mono audio reads the same level");
    expect(sineOk(view(anti, 80, 400), 1000, db, 1.0, "anti-phase"), "left and right in opposite phase do not cancel");
    const SpecView a = view(st, 80, 400), b = view(mono, 80, 400);
    float d = 0;
    for (size_t i = 0; i < a.db.size(); ++i)
      if (a.db[i] > -80 || b.db[i] > -80) d = std::max(d, std::fabs(a.db[i] - b.db[i]));
    expect(d < 0.1f, "stereo with equal channels matches mono (everything above -80 dB)");
  }
  for (double rate : {48000.0, 96000.0}) {
    char name[64];
    std::snprintf(name, sizeof name, "sine at %.0f Hz rate", rate);
    const bool ok = sineOk(view(tones({{60, 0.5}}, 3.0, rate), 60, 500), 60, -6.02, 1.0, name) &&
                    sineOk(view(tones({{5000, 0.5}}, 3.0, rate), 60, 500), 5000, -6.02, 1.0, name);
    std::snprintf(name, sizeof name, "levels and rows hold at %.0f kHz", rate / 1000);
    expect(ok, name);
  }
  for (int res : {0, 2}) {
    bool ok = true;
    for (double hz : {150.0, 1000.0, 8000.0}) ok = sineOk(view(tones({{hz, 0.5}}, 3.0), 60, 500, res), hz, -6.02, 1.0, res ? "frequency res" : "time res") && ok;
    expect(ok, res ? "sharper-in-frequency resolution: levels and rows hold" : "sharper-in-time resolution: levels and rows hold");
  }

  // ---- close tones: a semitone apart, each pair resolved with a dip between in every column
  {
    const double st = std::pow(2.0, 1.0 / 12.0);
    for (double hz : {100.0, 1000.0, 8000.0}) {
      const Audio a = tones({{hz, 0.25}, {hz * st, 0.25}}, 4.0);
      const SpecView v = view(a, 64, 900);
      const int r1 = rowOf(v, hz), r2 = rowOf(v, hz * st);
      double worst = 1e9;
      for (int c = 16; c < 48; ++c) {
        float lo = 1e9f;
        for (int r = std::min(r1, r2) + 1; r < std::max(r1, r2); ++r) lo = std::min(lo, v.at(c, r));
        const float pk = std::min(v.at(c, peakRow(v, c, hz, 0.03)), v.at(c, peakRow(v, c, hz * st, 0.03)));
        worst = std::min(worst, static_cast<double>(pk - lo));
      }
      std::printf("# semitone at %.0f Hz: dip of %.1f dB at least\n", hz, worst);
      char name[96];
      std::snprintf(name, sizeof name, "two tones a semitone apart at %.0f Hz are resolved (a dip of 6 dB or more)", hz);
      expect(worst >= 6.0, name);
    }
  }

  // ---- a click: sharp in time, nothing ahead of it
  {
    Audio a = makeAudio(44100, 2, 44100);
    a.L[22050] = a.R[22050] = 1.0f;
    const SpecView v = view(a, 400, 600), vt = view(a, 400, 600, 0);  // 2.5 ms per column
    auto extent = [&](const SpecView& x, double hz) {
      const int r = rowOf(x, hz);
      float pk = -1e9f;
      for (int c = 0; c < x.req.cols; ++c) pk = std::max(pk, x.at(c, r));
      int n = 0;
      for (int c = 0; c < x.req.cols; ++c) n += x.at(c, r) > pk - 20 ? 1 : 0;
      return n * 1000.0 / x.req.cols;  // ms
    };
    const double e8k = extent(v, 8000), e2k = extent(v, 2000), e100 = extent(v, 100), t100 = extent(vt, 100);
    std::printf("# click within 20 dB of its peak: %.1f ms at 8 kHz, %.1f ms at 2 kHz, %.1f ms at 100 Hz (%.1f ms sharper in time)\n", e8k, e2k, e100, t100);
    expect(e8k <= 7.5, "a click is sharp in time at 8 kHz (7.5 ms or less within 20 dB of its peak)");
    expect(e2k <= 25, "... and at 2 kHz (25 ms or less)");
    expect(e100 <= 125, "... and at 100 Hz, where the windows are long (125 ms or less, 12 periods)");
    expect(t100 <= 65, "... and sharper at 100 Hz with the sharper-in-time resolution (65 ms or less)");
    const int r = rowOf(v, 100), c0 = 200;
    float pk = -1e9f;
    for (int c = 0; c < v.req.cols; ++c) pk = std::max(pk, v.at(c, r));
    expect(v.at(c0 - 60, r) < pk - 40, "no smear 150 ms ahead of a click in the lows");
  }

  // ---- far-apart columns still show a short burst between their centres
  {
    Audio a = makeAudio(44100, 2, static_cast<int64_t>(44100 * 60.0));
    const int64_t b0 = static_cast<int64_t>(44100 * 30.25);  // a 20 ms burst between the centres of two 0.6 s columns
    for (int64_t i = b0; i < b0 + 882; ++i) a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(kTau * 5000 * static_cast<double>(i) / 44100));
    const SpecView v = view(a, 100, 400);
    float pk = -1e9f;
    const int r = rowOf(v, 5000);
    for (int c = 0; c < v.req.cols; ++c) pk = std::max(pk, v.at(c, r));
    std::printf("# 20 ms burst in 0.6 s columns: %.1f dB\n", static_cast<double>(pk));
    expect(pk > -20, "a 20 ms burst shows in a view of 60 s (columns 0.6 s apart)");
    expect(sineOk(view(tones({{440, 0.5}}, 60.0), 100, 400), 440, -6.02, 1.0, "long view"), "a long view keeps the level of a steady tone");
  }

  // ---- silence, finite values, a zoomed view, cancelling
  {
    const SpecView v = view(makeAudio(44100, 2, 44100), 50, 200);
    bool floor = !v.empty(), finite = true;
    for (float x : v.db) floor = floor && x <= -150, finite = finite && std::isfinite(x);
    expect(floor && finite, "silence stays at the floor (finite values)");
    const Audio a = tones({{3000, 0.5}}, 2.0);
    const SpecView whole = view(a, 200, 300), part = view(a, 50, 300, 1, 0.5, 1.0);
    float d = 0;
    for (int c = 0; c < 50; ++c)
      for (int r = 0; r < 300; ++r) d = std::max(d, std::fabs(part.at(c, r) - whole.at(50 + c, r)));
    expect(d < 0.5f, "a zoomed view agrees with the same columns of the whole");
    std::atomic<bool> stop{true};
    SpecViewRequest q;
    q.from = 0, q.to = a.frames(), q.cols = 200, q.rows = 300;
    expect(analyseSpecView(a, q, &stop).empty(), "a cancelled image comes back empty");
    q.cols = 0;
    expect(analyseSpecView(a, q).empty() && analyseSpecView(Audio{}, SpecViewRequest{}).empty(), "nothing to draw: an empty image");
  }

  // ---- how long a screen's worth takes (reported; generous bound)
  {
    const Audio a = tones({{220, 0.3}, {3000, 0.1}}, 4.0);
    for (auto [cols, rows] : {std::pair<int, int>{1400, 500}, {2800, 1000}}) {
      const auto t0 = std::chrono::steady_clock::now();
      const SpecView v = view(a, cols, rows);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("# %dx%d image of 4 s: %.0f ms\n", cols, rows, ms);
      char name[64];
      std::snprintf(name, sizeof name, "a %dx%d image is made in under 5 s", cols, rows);
      expect(!v.empty() && ms < 5000, name);
    }
  }
  return finish();
}
