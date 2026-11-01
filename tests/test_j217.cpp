// Gridline — J-217: the waveform summary takes are drawn with while they record.
//   - PeakSummary: incremental, block-size invariant, equal to a full rescan,
//     range queries (coarse level) equal to a bin-by-bin scan, memory bounded
//     (bins widen at the cap, deterministically);
//   - takePlacement / takeSpan: exactly where commitTakes() places each take
//     (track, stacked loop passes, muted earlier passes, start, length);
//   - a take summarised block by block as the recorder drains it (the leading
//     pad first) equals the summary of its committed file, stereo and mono.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/AudioRecord.h"
#include "../src/engine/PeakSummary.h"
#include "../src/engine/Sample.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

std::string gDir;
void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }

std::vector<float> noise(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  std::vector<float> v(n);
  for (auto& x : v) x = d(rng);
  return v;
}

PeakSummary chunked(const std::vector<float>& L, const std::vector<float>& R, size_t chunk, size_t maxBins = PeakSummary::kDefaultMaxBins) {
  PeakSummary p(maxBins);
  for (size_t i = 0; i < L.size(); i += chunk) p.add(L.data() + i, R.data() + i, std::min(chunk, L.size() - i));
  return p;
}

void testSummary() {
  const size_t n = 300000 + 77;  // a partial last bin
  const auto L = noise(n, 1), R = noise(n, 2);
  const PeakSummary whole = chunked(L, R, n);
  expect(whole.frames() == n && whole.bins() == (n + PeakSummary::kBin - 1) / PeakSummary::kBin, "summary: one bin per 128 frames");
  bool inv = true;
  for (size_t c : {1u, 7u, 64u, 128u, 129u, 1000u, 4096u}) inv = inv && chunked(L, R, c).checksum() == whole.checksum();
  expect(inv, "summary: block-size invariant (1, 7, 64, 128, 129, 1000, 4096 frames per add)");
  // A full rescan, written out bin by bin (the playlist's old per-sample summary).
  bool same = true;
  for (size_t b = 0; b < whole.bins() && same; ++b) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = b * PeakSummary::kBin; i < std::min(n, (b + 1) * PeakSummary::kBin); ++i) {
      const float v = 0.5f * (L[i] + R[i]);
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    same = lo == whole.lo(b) && hi == whole.hi(b);
  }
  expect(same, "summary: equals a full rescan of the mono mix");
  // Range queries through the coarse level equal a bin-by-bin scan.
  std::mt19937 rng(3);
  bool rq = true;
  for (int k = 0; k < 2000 && rq; ++k) {
    size_t b0 = rng() % whole.bins(), b1 = b0 + 1 + rng() % (k % 2 ? 40 : 3000);
    float lo = 0, hi = 0, lo2 = 0, hi2 = 0;
    whole.range(b0, b1, lo, hi);
    for (size_t b = b0; b < std::min(b1, whole.bins()); ++b) lo2 = std::min(lo2, whole.lo(b)), hi2 = std::max(hi2, whole.hi(b));
    rq = lo == lo2 && hi == hi2;
  }
  expect(rq, "summary: range queries (coarse level) equal a bin-by-bin scan");
  // Silence then signal (a take's leading pad).
  PeakSummary pad;
  pad.addSilence(100);
  pad.add(L.data(), R.data(), 1000);
  std::vector<float> pl(1100, 0.f), pr(1100, 0.f);
  std::copy(L.begin(), L.begin() + 1000, pl.begin() + 100);
  std::copy(R.begin(), R.begin() + 1000, pr.begin() + 100);
  expect(pad.checksum() == chunked(pl, pr, 1100).checksum(), "summary: leading silence is part of the file");
}

void testBounded() {
  // A small cap: the bins widen (merge in pairs) and stay below it, whatever the chunking.
  const size_t cap = 256, n = 128 * 256 * 5 + 333;
  const auto L = noise(n, 4), R = noise(n, 5);
  const PeakSummary a = chunked(L, R, n, cap), b = chunked(L, R, 97, cap), c = chunked(L, R, 4096, cap);
  expect(a.bins() <= cap && a.binFrames() == 128 * 8, "bounded: bins widen to stay below the cap");
  expect(a.checksum() == b.checksum() && a.checksum() == c.checksum(), "bounded: widening is block-size invariant");
  bool same = true;
  for (size_t bi = 0; bi < a.bins() && same; ++bi) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = bi * a.binFrames(); i < std::min(n, (bi + 1) * a.binFrames()); ++i) {
      const float v = 0.5f * (L[i] + R[i]);
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    same = lo == a.lo(bi) && hi == a.hi(bi);
  }
  expect(same, "bounded: widened bins equal a rescan at their width");
  float lo = 0, hi = 0, lo2 = 0, hi2 = 0;
  a.range(3, 200, lo, hi);
  for (size_t bi = 3; bi < 200; ++bi) lo2 = std::min(lo2, a.lo(bi)), hi2 = std::max(hi2, a.hi(bi));
  expect(lo == lo2 && hi == hi2, "bounded: the coarse level follows the widening");
  // Default cap: an hour at 48 kHz stays far below 32 MB of summary.
  const size_t hour = static_cast<size_t>(48000) * 3600;
  expect(hour / PeakSummary::kBin <= PeakSummary::kDefaultMaxBins * 2, "bounded: the default cap holds over an hour at 48 kHz");
}

// A recording rig as in test_recording: generated input, the recorder pumped every block,
// and each take summarised from the frames each pump added (the playlist's live path).
struct Rig {
  double sr = 48000;
  Song song = makeEmptySong();
  ProgramBuilder builder;
  AudioEngine eng{48000};
  AudioRecorder rec;
  std::shared_ptr<const Program> prog;
  int64_t frame = 0;
  std::vector<std::unique_ptr<PeakSummary>> live;
  std::vector<size_t> fed;
  std::vector<int64_t> pads;
  TempoMap map;
  Rig() {
    builder.setSampleRate(sr);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
    map.build(song);
  }
  void feed() {
    const auto& t = rec.takes();
    for (size_t i = 0; i < t.size(); ++i) {
      if (i == live.size()) {
        live.push_back(std::make_unique<PeakSummary>());
        fed.push_back(0);
        pads.push_back(takeSpan(map, t[i].startTick, 0, sr).pad);
        live.back()->addSilence(static_cast<size_t>(pads.back()));
      }
      const MixerTrack* mt = song.mixerTrack(t[i].mixer);
      const bool stereo = mt->inputStereo;
      const size_t n = static_cast<size_t>(t[i].frames());
      live[i]->add(t[i].L.data() + fed[i], (stereo ? t[i].R.data() : t[i].L.data()) + fed[i], n - fed[i]);
      fed[i] = n;
    }
  }
  void run(int64_t frames, int block) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    for (int64_t done = 0; done < frames;) {
      const int n = static_cast<int>(std::min<int64_t>(block, frames - done));
      for (int i = 0; i < n; ++i) {
        const int64_t f = frame + i;
        const float amp = (f / 24000) % 2 ? 0.7f : 0.2f;
        in[static_cast<size_t>(2 * i)] = (f % 100) < 50 ? amp : -amp;
        in[static_cast<size_t>(2 * i + 1)] = (f % 70) < 35 ? 0.5f * amp : -0.5f * amp;
      }
      eng.pushInput(in.data(), n);
      eng.process(L.data(), R.data(), n);
      frame += n;
      done += n;
      rec.pump(eng, ringsOf(*prog));
      feed();
    }
  }
};

void testLiveEqualsCommitted(bool stereo) {
  const std::string tag = stereo ? "stereo" : "mono";
  Rig r;
  r.song.tempo = 120;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].inputStereo = stereo;
  r.song.mixer[1].armed = true;
  r.song.mixer[2].input = 0;
  r.song.mixer[2].inputStereo = true;
  r.song.mixer[2].armed = true;
  r.build();
  const Tick bar = r.song.barTicks();
  // Loop recording over a bar starting off the tick grid: three passes on each of two tracks.
  r.eng.setLooping(true);
  r.eng.setLoopRange(bar / 3, bar / 3 + bar);
  r.eng.setPosition(bar / 3, false);
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  const double barFrames = static_cast<double>(bar) * r.sr * 60.0 / (r.song.tempo * r.song.ppq);
  r.run(static_cast<int64_t>(barFrames * 2.4), 384);
  r.eng.setRecording(false);
  r.eng.stop();
  r.run(r.eng.recordLatency() + 1024, 256);
  const std::vector<RecordedTake>& takes = r.rec.takes();
  expect(takes.size() == 6, tag + ": three passes on two tracks (" + std::to_string(takes.size()) + ")");
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded-" + tag;
  o.sampleRate = r.sr;
  // Placement and span before the commit (as the live clips are drawn) ...
  std::vector<TakePlacement> place;
  std::vector<TakeSpan> span;
  for (size_t i = 0; i < takes.size(); ++i) {
    place.push_back(takePlacement(r.song, takes, i, o));
    span.push_back(takeSpan(r.map, takes[i].startTick, takes[i].frames(), r.sr));
  }
  Song song = r.song;
  const TakeCommitResult res = commitTakes(song, takes, o);
  expect(res.error.empty() && res.clips.size() == takes.size() && res.takeIndex.size() == takes.size(), tag + ": every take committed");
  bool geom = true, peaks = true, pads = true;
  for (size_t k = 0; k < res.clips.size(); ++k) {
    const size_t ti = res.takeIndex[k];
    const Clip* c = song.clip(res.clips[k]);
    const Channel* ch = c ? song.channel(c->source) : nullptr;
    geom = geom && c && ch && c->track == place[ti].track && c->start == span[ti].start && c->length == span[ti].length &&
           c->muted == place[ti].muted && c->take == place[ti].take && ch->name == place[ti].name && ch->color == place[ti].color;
    pads = pads && r.pads[ti] == span[ti].pad;
    // ... and the live summary against the committed file's.
    SamplePtr smp = ch ? sampleCache().get(ch->plugin.data) : nullptr;
    PeakSummary file;
    if (smp) file.add(smp->left.data(), smp->right.data(), smp->frames());
    peaks = peaks && smp && smp->frames() == static_cast<size_t>(span[ti].pad + takes[ti].frames()) && file.checksum() == r.live[ti]->checksum();
  }
  expect(geom, tag + ": live placement (track, start, length, muted, take, name, colour) equals the committed clips");
  expect(pads, tag + ": the live summary starts with the file's leading pad");
  expect(peaks, tag + ": the live summary equals the committed file's summary");
  // Stacking: each track's passes on consecutive tracks, earlier passes muted.
  int muted = 0;
  for (const auto& p : place) muted += p.muted ? 1 : 0;
  expect(muted == 4, tag + ": earlier passes muted (4 of 6)");
  // The envelope: the square wave's peaks follow its amplitude (0.2 / 0.7).
  size_t first = 0;
  while (first + 1 < takes.size() && takes[first].mixer != r.song.mixer[1].id) ++first;
  float lo = 0, hi = 0;
  r.live[first]->range(0, r.live[first]->bins(), lo, hi);
  expect(std::fabs(hi - (stereo ? 0.525f : 0.7f)) < 1e-6f, tag + ": the peak follows the input's amplitude");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("j217");
  testSummary();
  testBounded();
  testLiveEqualsCommitted(true);
  testLiveEqualsCommitted(false);
  sampleCache().shutdown();
  return finish();
}
