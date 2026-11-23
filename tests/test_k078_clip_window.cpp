// Gridline: the audio clip editor's clip window against what the engine plays (engine/ClipOps.h
// clipAudibleSpans, clipHeardSourceAt; engine/ClipAudioEdit.h clipRangeFrames).
//
// The editor draws an audio clip's waveform, its spectrum and its time selection through
// clipAudibleSpans. For clips with other starts, slip offsets, loops (with an offset into the
// loop), tempos and tempo sync, the clip is rendered alone by the engine and:
//  * the spans' song ticks match where the render is not silent (within a millisecond);
//  * at the editor's zoom (an x0, a first tick and pixels per tick), the pixel range the spans
//    give matches the pixel range of the render's audible frames (within a pixel);
//  * a click in the file (at source 0.5 s) sounds where clipHeardSourceAt says source 0.5 s
//    plays, in every pass of a loop;
//  * a time selection in a loop's second pass edits the frames that pass plays (inside the loop
//    window; it read past the loop's end before), and a selection over the silence after the
//    audio has none to edit.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "core/Automation.h"
#include "engine/ClipAudioEdit.h"
#include "engine/ClipOps.h"
#include "engine/ClipRender.h"
#include "engine/Render.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace ed = gl::packs::edison;

namespace {

std::string gDir;
constexpr int kFileRate = 44100;
constexpr int kRate = 48000;
constexpr double kFileSec = 2.0;
constexpr double kClickSec = 0.5;

// 2 s of a steady tone (never silent) with a short loud click at 0.5 s.
std::string writeFile() {
  ed::Audio a = ed::makeAudio(kFileRate, 2, static_cast<int64_t>(kFileSec * kFileRate));
  for (int64_t i = 0; i < a.frames(); ++i) {
    const double t = static_cast<double>(i) / kFileRate;
    float v = 0.2f + 0.05f * static_cast<float>(std::sin(2 * M_PI * 110 * t));
    if (t >= kClickSec && t < kClickSec + 0.002) v = 0.95f;
    a.L[static_cast<size_t>(i)] = v;
    a.R[static_cast<size_t>(i)] = v;
  }
  const std::string p = gDir + "/steady.wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

struct Case {
  const char* name;
  double tempo = 120;
  Tick start = 0;
  Tick length = 0;
  double audioStart = 0, audioEnd = 0;
  bool loop = false;
  Tick offset = 0;
  double syncBpm = 0;
  double tailMs = 0;  // a pitch-keeping stretch rings this long past the audio's last sample (its grain window)
};

Song songFor(const Case& k, const std::string& file, ObjectId& clipId) {
  Song s = makeEmptySong();
  s.tempo = k.tempo;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Steady";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = file;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = 0;
  c.start = k.start;
  c.length = k.length;
  c.audioStart = k.audioStart;
  c.audioEnd = k.audioEnd;
  c.loop = k.loop;
  c.offset = k.offset;
  c.syncBpm = k.syncBpm;
  c.fadeInManual = c.fadeOutManual = true;  // no automatic declick fades at the edges
  s.playlist.push_back(c);
  clipId = c.id;
  return s;
}

// The editor's mapping of song ticks to pixels (ClipEditor.cpp Geo).
struct View {
  float x0 = 120;
  double t0 = 0;
  float px = 0.75f;
  float xOf(double t) const { return x0 + static_cast<float>((t - t0) * px); }
};

struct Run {
  double t0, t1;  // ticks
};

// Song-tick runs where the render is not silent (gaps under 1 ms joined).
std::vector<Run> audibleRuns(const RenderResult& r, const TempoMap& tm, Tick start) {
  std::vector<Run> out;
  const float* l = r.audio.plane(0);
  const double sec0 = tm.secondsAt(static_cast<double>(start));
  int64_t runStart = -1, last = -1;
  const int64_t gap = kRate / 1000;
  auto close = [&]() {
    if (runStart < 0) return;
    out.push_back({tm.tickAtSeconds(sec0 + static_cast<double>(runStart) / kRate), tm.tickAtSeconds(sec0 + static_cast<double>(last + 1) / kRate)});
  };
  for (int64_t i = 0; i < static_cast<int64_t>(r.audio.frames); ++i) {
    if (std::fabs(l[i]) < 1e-4f) continue;
    if (runStart >= 0 && i - last > gap) {
      close();
      runStart = -1;
    }
    if (runStart < 0) runStart = i;
    last = i;
  }
  close();
  return out;
}

// Song ticks where the render's click sounds (the loud frames, one per click).
std::vector<double> clicks(const RenderResult& r, const TempoMap& tm, Tick start) {
  std::vector<double> out;
  const float* l = r.audio.plane(0);
  const double sec0 = tm.secondsAt(static_cast<double>(start));
  int64_t lastClick = -kRate;
  for (int64_t i = 0; i < static_cast<int64_t>(r.audio.frames); ++i)
    if (std::fabs(l[i]) > 0.5f) {
      if (i - lastClick > kRate / 50) out.push_back(tm.tickAtSeconds(sec0 + static_cast<double>(i) / kRate));
      lastClick = i;
    }
  return out;
}

void check(const Case& k, const std::string& file) {
  ObjectId id = kNoId;
  const Song s = songFor(k, file, id);
  const Clip& c = *s.clip(id);
  TempoMap tm;
  tm.build(s);
  const std::vector<AudibleSpan> spans = clipAudibleSpans(s, c, tm);
  const RenderResult r = renderClipAlone(s, id, kRate);
  const std::string n = std::string(k.name) + ": ";
  expect(r.error.empty() && r.audio.frames > 0, (n + "the clip renders").c_str());
  if (!r.error.empty() || r.audio.frames == 0) return;
  // Joined spans (a loop's passes follow each other without a gap).
  std::vector<Run> want;
  for (const AudibleSpan& sp : spans) {
    if (!want.empty() && std::fabs(want.back().t1 - sp.t0) < 0.5) want.back().t1 = sp.t1;
    else want.push_back({sp.t0, sp.t1});
  }
  const std::vector<Run> got = audibleRuns(r, tm, c.start);
  const double msTicks = tm.tickAtSeconds(tm.secondsAt(static_cast<double>(c.start)) + 0.001) - static_cast<double>(c.start);
  char b[256];
  std::snprintf(b, sizeof b, "%sthe audible runs match the render (%zu spans, %zu runs; first %.1f-%.1f against %.1f-%.1f ticks)", n.c_str(), want.size(),
                got.size(), want.empty() ? -1.0 : want[0].t0, want.empty() ? -1.0 : want[0].t1, got.empty() ? -1.0 : got[0].t0,
                got.empty() ? -1.0 : got[0].t1);
  bool same = want.size() == got.size() && !want.empty();
  for (size_t i = 0; same && i < want.size(); ++i)
    same = std::fabs(want[i].t0 - got[i].t0) <= msTicks + 0.5 && got[i].t1 - want[i].t1 <= msTicks * (1 + k.tailMs) + 0.5 &&
           want[i].t1 - got[i].t1 <= msTicks + 0.5;
  expect(same, b);
  // In pixels at the editor's zoom: the window's audible part against the render's.
  const View v{120.0f, static_cast<double>(c.start) - 96.0, 0.75f};
  if (same) {
    float worst = 0;
    for (size_t i = 0; i < want.size(); ++i)
      worst = std::max({worst, std::fabs(v.xOf(want[i].t0) - v.xOf(got[i].t0)),
                        std::fabs(v.xOf(want[i].t1) - v.xOf(got[i].t1)) - static_cast<float>(msTicks * k.tailMs) * v.px});
    std::snprintf(b, sizeof b, "%sat the editor's zoom the window's audible pixels are the render's (%.1f-%.1f px, off by %.2f px)", n.c_str(),
                  static_cast<double>(v.xOf(want[0].t0)), static_cast<double>(v.xOf(want.back().t1)), static_cast<double>(worst));
    expect(worst <= 1.0f, b);
  }
  // The click: where the spans say source 0.5 s plays.
  std::vector<double> predicted;
  for (const AudibleSpan& sp : spans) {
    if (!(kClickSec >= sp.s0 && kClickSec < sp.s1)) continue;
    // the tick in this span where the source reaches the click
    double lo = sp.t0, hi = sp.t1;
    for (int it = 0; it < 60; ++it) {
      const double mid = 0.5 * (lo + hi);
      (clipHeardSourceAt(spans, s, tm, mid) < kClickSec ? lo : hi) = mid;
    }
    predicted.push_back(hi);
  }
  const std::vector<double> heard = clicks(r, tm, c.start);
  bool clicksOk = predicted.size() == heard.size() && !predicted.empty();
  for (size_t i = 0; clicksOk && i < predicted.size(); ++i) clicksOk = std::fabs(predicted[i] - heard[i]) <= msTicks + 0.5;
  std::snprintf(b, sizeof b, "%sthe click sounds where the editor maps source 0.5 s (%zu predicted, %zu heard; first %.1f against %.1f ticks)", n.c_str(),
                predicted.size(), heard.size(), predicted.empty() ? -1.0 : predicted[0], heard.empty() ? -1.0 : heard[0]);
  expect(clicksOk, b);
}

void testSpans() {
  const std::string file = writeFile();
  // 120 BPM at 96 PPQ: 192 ticks a second. A 2 s file in a 4 bar clip: sound for 2 s, then silence.
  Case a{"plain, longer than its audio"};
  a.start = 384, a.length = 4 * 384;
  check(a, file);
  Case b{"slipped by 0.5 s, 90 BPM"};
  b.tempo = 90, b.start = 768, b.length = 3 * 384, b.audioStart = 0.5;
  check(b, file);
  Case c{"trimmed at 1.6 s, 140 BPM"};
  c.tempo = 140, c.start = 96, c.length = 4 * 384, c.audioStart = 0.2, c.audioEnd = 1.6;
  check(c, file);
  Case d{"looping a 1 s window from 0.25 s, starting 0.3 s into it"};
  d.tempo = 120, d.start = 384, d.length = 3 * 384, d.audioStart = 0.25, d.audioEnd = 1.25, d.loop = true, d.offset = 58;  // 58 ticks: 0.302 s
  check(d, file);
  Case e{"tempo-synced (recorded at 100 BPM, song at 124)"};
  e.tempo = 124, e.start = 192, e.length = 6 * 384, e.syncBpm = 100, e.tailMs = 30;
  check(e, file);
  Case f{"tempo-synced loop at 150 BPM"};
  f.tempo = 150, f.start = 0, f.length = 5 * 384, f.syncBpm = 120, f.tailMs = 30, f.audioStart = 0.3, f.audioEnd = 0.9, f.loop = true, f.offset = 40;
  check(f, file);
}

void testSelectionFrames() {
  const std::string file = writeFile();
  Case k{"loop"};
  k.tempo = 120, k.start = 0, k.length = 3 * 384, k.audioStart = 0.25, k.audioEnd = 1.25, k.loop = true;
  ObjectId id = kNoId;
  const Song s = songFor(k, file, id);
  TempoMap tm;
  tm.build(s);
  // One pass: 1 s = 192 ticks. A selection 0.25 s to 0.5 s into the second pass hears source 0.5 to 0.75 s.
  int64_t fa = 0, fb = 0;
  const bool ok = clipedit::clipRangeFrames(s, id, 192 + 48, 192 + 96, fa, fb);
  char b[160];
  std::snprintf(b, sizeof b, "a selection in a loop's second pass edits the frames it plays (%lld-%lld, want %d-%d)", static_cast<long long>(fa),
                static_cast<long long>(fb), static_cast<int>(0.5 * kFileRate), static_cast<int>(0.75 * kFileRate));
  expect(ok && std::llabs(fa - static_cast<int64_t>(0.5 * kFileRate)) <= 2 && std::llabs(fb - static_cast<int64_t>(0.75 * kFileRate)) <= 2, b);
  // The whole clip: the whole loop window.
  expect(clipedit::clipRangeFrames(s, id, 0, 3 * 384, fa, fb) && std::llabs(fa - static_cast<int64_t>(0.25 * kFileRate)) <= 2 &&
             std::llabs(fb - static_cast<int64_t>(1.25 * kFileRate)) <= 2,
         "the whole looping clip: its loop window");
  // A plain clip longer than its audio: a selection over the silence after it edits nothing.
  Case p{"plain"};
  p.start = 0, p.length = 4 * 384;
  ObjectId pid = kNoId;
  const Song ps = songFor(p, file, pid);
  expect(!clipedit::clipRangeFrames(ps, pid, 3 * 192, 4 * 192, fa, fb), "a selection over the silence after the audio: nothing to edit");
  expect(clipedit::clipRangeFrames(ps, pid, 192, 4 * 192, fa, fb) && std::llabs(fa - kFileRate) <= 2 && std::llabs(fb - 2 * kFileRate) <= 2,
         "a selection running past the audio's end: up to its end");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k078cw");
  testSpans();
  testSelectionFrames();
  return finish();
}
