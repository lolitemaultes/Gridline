// Gridline — K-85: a sampler channel's start / end handles and processing
// shape the audio its playlist clips play, and warp markers.
//  * the handles trim every clip of the channel (render, natural length,
//    loops, trim and slip limits, chop), they never cross;
//  * an untrimmed, unprocessed channel plays exactly as before;
//  * reversing a clip keeps its own audio (the window mirrors);
//  * the processing switches (polarity, normalize) reach the clips;
//  * FL: clip offsets count from the trimmed start, export and import agree;
//  * warp markers: the time map, its limits, save / load.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/TimeWarp.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClipOps.h"
#include "engine/ClipSource.h"
#include "engine/PlaylistTools.h"
#include "engine/Program.h"
#include "engine/WarpOps.h"
#include "engine/Render.h"
#include "engine/TimeStretch.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kClicks[4] = {0.1, 0.3, 0.5, 0.7};
constexpr double kLevels[4] = {0.9, 0.7, 0.5, 0.3};

// A 1 s file with four short bursts of falling level.
std::string clickFile() {
  const size_t n = static_cast<size_t>(kSr);
  AudioBuffer b(2, n);
  for (int k = 0; k < 4; ++k) {
    const size_t a = static_cast<size_t>(kClicks[k] * kSr);
    for (size_t i = 0; i < static_cast<size_t>(0.01 * kSr) && a + i < n; ++i)
      b.at(0, a + i) = b.at(1, a + i) = static_cast<float>(kLevels[k] * std::sin(2 * kPi * 1000 * static_cast<double>(i) / kSr));
  }
  const std::string path = (fs::temp_directory_path() / "gl_k85_clicks.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

struct Fx {
  Song s;
  ObjectId ch = kNoId, clip = kNoId;
  explicit Fx(const std::string& path, double start = 0, double end = 1) {
    s = makeEmptySong();
    s.tempo = 120;
    Channel c;
    c.id = s.allocateId();
    c.name = "Clicks";
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = path;
    c.plugin.params[pp::SmpStart] = static_cast<float>(start);
    c.plugin.params[pp::SmpEnd] = static_cast<float>(end);
    c.mixerTrack = s.masterId();
    c.audioClip = true;
    s.channels.push_back(c);
    ch = c.id;
    Clip k;
    k.id = clip = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = ch;
    k.start = s.barTicks();
    k.length = audioClipNaturalLength(s, k);
    s.playlist.push_back(k);
  }
  Clip& c() { return *s.clip(clip); }
  Channel& chan() { return *s.channel(ch); }
  size_t clipFrame() const { return static_cast<size_t>(std::llround(static_cast<double>(s.barTicks()) * 60.0 / (s.tempo * s.ppq) * kSr)); }
};

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.threads = 1;
  o.tail = s.barTicks();
  return renderSong(s, o);
}

// Seconds after `from` of each burst heard (onsets above a threshold, 50 ms apart at least), with its peak.
struct Onset {
  double at = 0, level = 0;
};
std::vector<Onset> onsets(const AudioBuffer& b, size_t from, double gap = 0.05) {
  std::vector<Onset> out;
  size_t i = from;
  while (i < b.frames) {
    if (std::fabs(b.at(0, i)) > 0.02) {
      Onset o;
      o.at = static_cast<double>(i - from) / kSr;
      for (size_t j = i; j < std::min(b.frames, i + static_cast<size_t>(0.02 * kSr)); ++j) o.level = std::max(o.level, static_cast<double>(std::fabs(b.at(0, j))));
      out.push_back(o);
      i += static_cast<size_t>(gap * kSr);
    } else {
      ++i;
    }
  }
  return out;
}
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
// Where a handle at fraction f sits (fractions of the last frame's position, as the notes read them).
double handle(double f) { return static_cast<double>(static_cast<float>(f)) * (kSr - 1) / kSr; }
double secondsOf(const Song& s, Tick t) { return static_cast<double>(t) * 60.0 / (s.tempo * s.ppq); }

void testHandlesTrimClips(const std::string& path) {
  {  // B1: the start handle trims the clip (it starts playing at the handle)
    Fx a(path, 0.4, 1.0);
    const std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
    // The clip starts at 0.4 s of the file: first burst is the one at 0.5 s, 0.1 s in.
    expect(!o.empty() && near(o[0].at, 0.1, 0.002) && near(o[0].level, kLevels[2], 0.05), "B1: the start handle trims a playlist clip");
    expect(o.size() == 2, "B1: ... only the bursts after the handle play");
  }
  {  // B1: the end handle cuts it
    Fx a(path, 0.0, 0.4);
    a.c().length = 4 * a.s.barTicks();  // longer than the audio: it still stops at the handle
    const std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
    expect(o.size() == 2 && near(o[0].at, 0.1, 0.002) && near(o[1].at, 0.3, 0.002), "B1: the end handle cuts a playlist clip");
  }
  {  // B3: natural length and loop length follow the handles
    Fx a(path, 0.25, 0.75);
    TempoMap tm;
    tm.build(a.s);
    const double len = secondsOf(a.s, a.c().length);
    expect(near(len, 0.5, 0.002), "B3: a new clip is as long as the part between the handles");
    a.c().loop = true;
    expect(near(secondsOf(a.s, audioLoopLength(a.s, a.c(), tm)), 0.5, 0.002), "B3: one loop pass is the part between the handles");
    a.c().length *= 3;
    const std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
    // Passes of 0.5 s starting at 0.25 s: bursts at 0.05, 0.25 and 0.45 s into each pass.
    bool ok = o.size() == 9;
    for (size_t k = 0; ok && k < o.size(); ++k) ok = near(o[k].at, 0.5 * static_cast<double>(k / 3) + 0.05 + 0.2 * static_cast<double>(k % 3), 0.003);
    expect(ok, "B3: a looping clip repeats the part between the handles");
  }
  {  // B4: slip and trim cannot reach audio the handles cut away
    Fx a(path, 0.4, 0.8);
    TempoMap tm;
    tm.build(a.s);
    const Clip back = pltools::slipClip(a.s, tm, a.c(), -a.s.barTicks());
    expect(clipSourceStart(a.s, back) >= handle(0.4) - 1e-9, "B4: slipping back stops at the start handle");
    const Clip fwd = pltools::slipClip(a.s, tm, a.c(), 10 * a.s.barTicks());
    expect(clipSourceStart(a.s, fwd) < 0.8, "B4: slipping forward stays before the end handle");
    const std::vector<Clip> t = pltools::trimStarts(a.s, tm, a.s.playlist, {a.clip}, -a.s.barTicks(), false);
    expect(!t.empty() && clipSourceStart(a.s, t[0]) >= handle(0.4) - 1e-9 && t[0].start >= a.c().start, "B4: trimming the start out stops at the start handle");
    const std::vector<Clip> r = pltools::resizeEnds(a.s, a.s.playlist, {a.clip}, a.s.barTicks(), false, &tm, false);
    expect(!r.empty() && near(secondsOf(a.s, r[0].length), 0.4, 0.002), "B4: extending the end stops at the end handle");
  }
  {  // B5: slicing works inside the handles
    Fx a(path, 0.4, 1.0);
    const std::vector<ObjectId> parts = sliceClipAtTransients(a.s, a.clip, 0.5);
    double lo = 1e9;
    for (const Clip& c : a.s.playlist)
      if (c.kind == ClipKind::Audio) lo = std::min(lo, clipSourceStart(a.s, c));
    expect(lo >= handle(0.4) - 1e-9, "B5: slices never start before the start handle");
    (void)parts;
  }
}

void testHandlesNeverCross() {
  const double sec = 2.0;
  expect(clampTrimHandle(false, 0.9, 0.5, sec) < 0.5, "B6: start dragged past the end stops before it");
  expect(clampTrimHandle(true, 0.1, 0.5, sec) > 0.5, "B6: end dragged before the start stops after it");
  expect(near((0.5 - clampTrimHandle(false, 0.9, 0.5, sec)) * sec, 0.001, 1e-9), "B6: ... 1 ms apart");
  expect(clampTrimHandle(false, -3, 0.5, sec) == 0.0 && clampTrimHandle(true, 7, 0.5, sec) == 1.0, "B6: handles stay inside the file");
  expect(clampTrimHandle(false, std::nan(""), 0.5, sec) == 0.0, "B6: a non-finite drag is ignored");
  Channel ch;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.params[pp::SmpStart] = 0.7f;
  ch.plugin.params[pp::SmpEnd] = 0.2f;  // crossed (an old project)
  double a = 0, b = 0;
  channelTrim(ch, a, b);
  expect(b > a, "B6: crossed handles in a file still give a window");
}

void testUntrimmedUnchanged(const std::string& path) {
  Fx a(path);
  const ClipSource src = clipSource(a.chan());
  double lo = 0, hi = 0, w0 = 0, w1 = 0;
  clipLimits(src, false, lo, hi);
  expect(lo == 0.0 && hi == src.seconds(), "untrimmed: the limits are the whole file");
  clipLimits(src, true, lo, hi);
  expect(lo == 0.0 && hi == src.seconds(), "untrimmed: ... reversed too");
  a.c().audioStart = 0.123;
  a.c().audioEnd = 0.6;
  expect(clipWindow(src, false, a.c(), w0, w1) && w0 == 0.123 && w1 == 0.6, "untrimmed: the clip's window is exactly its own");
  expect(src.sample == sampleCache().get(path), "untrimmed, unprocessed: clips play the file itself");
}

void testReverseKeepsAudio(const std::string& path) {
  Fx a(path);
  a.c().audioStart = 0.25;  // the bursts at 0.3 and 0.5 s
  a.c().audioEnd = 0.55;
  a.c().length = a.s.barTicks();
  toggleClipReverse(a.s, a.c());
  expect(a.c().reverse, "B10: reverse is on");
  std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
  // Played backwards from 0.55 s: the 0.5 s burst ends 0.04 s in (its tail first), then the 0.3 s one.
  bool ok = o.size() == 2 && near(o[0].level, kLevels[2], 0.06) && near(o[1].level, kLevels[1], 0.06);
  expect(ok, "B10: a reversed clip plays its own audio backwards");
  toggleClipReverse(a.s, a.c());
  expect(!a.c().reverse && near(a.c().audioStart, 0.25, 1e-9) && near(a.c().audioEnd, 0.55, 1e-9), "B10: reversing twice gives the window back");
  Fx w(path);
  const double s0 = w.c().audioStart, e0 = w.c().audioEnd;
  toggleClipReverse(w.s, w.c());
  expect(w.c().audioStart == s0 && w.c().audioEnd == e0, "B10: a whole-file window stays as it is");
}

void testProcessing(const std::string& path) {
  Fx a(path);
  const AudioBuffer plain = render(a.s);
  a.chan().plugin.params[pp::SmpPolarity] = 1;
  const AudioBuffer flipped = render(a.s);
  double sum = 0, mx = 0;
  for (size_t i = 0; i < std::min(plain.frames, flipped.frames); ++i)
    sum = std::max(sum, std::fabs(static_cast<double>(plain.at(0, i) + flipped.at(0, i)))), mx = std::max(mx, std::fabs(static_cast<double>(plain.at(0, i))));
  expect(mx > 0.1 && sum < 1e-5, "processing: polarity reaches the clips");
  a.chan().plugin.params[pp::SmpPolarity] = 0;
  a.chan().plugin.params[pp::SmpNormalize] = 1;
  const std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
  expect(!o.empty() && o[0].level > 0.95, "processing: normalize reaches the clips");
}

void testFl(const std::string& path) {
  Fx a(path, 0.4, 0.9);
  a.c().audioStart = 0.45;
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(a.s, bytes, rep, eo), "B9: exported");
  Song imp;
  flp::Report ir;
  std::string err;
  expect(importFlpBytes(bytes, imp, ir, &err), "B9: re-imported");
  const Clip* c = nullptr;
  for (const Clip& k : imp.playlist)
    if (k.kind == ClipKind::Audio) c = &k;
  const Channel* ch = c ? imp.channel(c->source) : nullptr;
  expect(ch && ch->plugin.type == "gl.sampler" && near(ch->plugin.params[pp::SmpStart], 0.4, 1e-3) && near(ch->plugin.params[pp::SmpEnd], 0.9, 1e-3),
         "B9: the handles round trip");
  expect(c && near(c->audioStart, 0.45, 1e-3), "B9: the clip's start round trips (FL counts it from the start handle)");
  if (c) {
    const std::vector<Onset> o0 = onsets(render(a.s), a.clipFrame());
    Song& s = imp;
    const size_t f = static_cast<size_t>(std::llround(secondsOf(s, c->start) * kSr));
    const std::vector<Onset> o1 = onsets(render(s), f);
    expect(!o0.empty() && o0.size() == o1.size() && near(o0[0].at, o1[0].at, 0.002), "B9: ... and sounds the same");
  }
}

void testWarpMap() {
  std::vector<WarpMarker> m = {{0, 0.5, 0.5}, {0, 1.0, 1.5}, {0, 2.0, 2.0}};
  expect(!normalizeWarp(m) || m.size() == 3, "warp: valid markers are kept");
  expect(m.size() == 3 && m[0].id && m[1].id && m[2].id && m[0].id != m[1].id, "warp: ids are unique");
  const std::vector<WarpPoint> p = warpPoints(m, false, 3.0);
  expect(warpSourceAt(p, 0.25) == 0.25 && near(warpSourceAt(p, 1.0), 0.75, 1e-12) && near(warpSourceAt(p, 1.75), 1.5, 1e-12) && near(warpSourceAt(p, 2.5), 2.5, 1e-12),
         "warp: sections stretch independently, slope 1 outside");
  expect(near(warpTimeAt(p, warpSourceAt(p, 1.3)), 1.3, 1e-12), "warp: the inverse");
  expect(near(warpSpeedAt(p.data(), p.size(), 1.0), 0.5, 1e-12) && near(warpSpeedAt(p.data(), p.size(), 1.7), 2.0, 1e-12), "warp: section speeds");
  const std::vector<WarpPoint> r = warpPoints(m, true, 3.0);
  // Reversed: warped -w maps to file (3 - s) backwards; the middle section mirrors.
  expect(near(3.0 - warpSourceAt(r, -1.0), 0.75, 1e-12), "warp: a reversed clip sees the same map from the end");
  std::vector<WarpMarker> bad = {{1, 1.0, 1.0}, {2, 0.5, 2.0}, {3, 1.001, 1.0001}, {4, std::nan(""), 1}, {5, 3.0, 1.1}, {5, 4.0, 4.0}};
  normalizeWarp(bad);
  bool sorted = true;
  for (size_t k = 1; k < bad.size(); ++k) sorted = sorted && bad[k].at > bad[k - 1].at && bad[k].to - bad[k - 1].to >= kWarpMinSection;
  expect(sorted && bad.size() >= 2, "warp: crossing, too close, too fast and non-finite markers are dropped");
  expect(bad.size() < 2 || bad[0].id != bad.back().id, "warp: duplicate ids are made unique");
  std::vector<WarpMarker> many;
  for (size_t k = 0; k < 2 * kMaxWarpMarkers; ++k) many.push_back({0, 0.01 * static_cast<double>(k), 0.01 * static_cast<double>(k)});
  normalizeWarp(many);
  expect(many.size() == kMaxWarpMarkers, "warp: at most the marker limit");
}

void testWarpSaveLoad(const std::string& path) {
  Fx a(path);
  a.c().warp = {{1, 0.3, 0.4}, {2, 0.5, 0.55}};
  const Json j = songToJson(a.s);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), "warp: the project loads");
  const Clip* c = back.clip(a.clip);
  expect(c && c->warp == a.c().warp, "warp: markers save and load");
  Fx b(path);
  Song back2;
  songFromJson(songToJson(b.s), back2, &err);
  expect(back2.clip(b.clip) && back2.clip(b.clip)->warp.empty(), "warp: clips without markers stay without");
}


// The song tick `sec` seconds after the clip's start (120 BPM, no tempo changes).
double tickAfter(const Fx& a, double sec) { return static_cast<double>(a.s.barTicks()) + sec * a.s.tempo * a.s.ppq / 60.0; }

void testWarpPlayback(const std::string& path) {
  Fx a(path);
  TempoMap tm;
  tm.build(a.s);
  const uint32_t m1 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3), false);
  const uint32_t m2 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.5), false);
  expect(m1 && m2 && a.c().warp.size() == 2, "warp: markers added on the clip");
  const AudioBuffer before = render(a.s);
  {
    Fx plain(path);
    expect(before.data == render(plain.s).data, "warp: markers that move nothing leave the sound bit-identical");
  }
  // The burst at 0.5 s now plays at 0.6 s: the section 0.3 .. 0.5 s spans 0.3 s (two thirds of its speed).
  expect(moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 0.6)), "warp: a marker drags");
  expect(a.c().warp.size() == 3, "warp: the first drag pins the clip's start");
  const std::vector<WarpMarkerView> v = warpMarkerViews(a.s, a.c(), tm);
  bool at06 = false;
  for (const WarpMarkerView& k : v) at06 = at06 || (k.id == m2 && near(k.tick, tickAfter(a, 0.6), 0.5));
  expect(at06, "warp: the marker shows where it was dropped");
  TempoMap tm2;
  tm2.build(a.s);
  expect(near(clipSourceAt(a.s, a.c(), tm2, tickAfter(a, 0.4)), 0.3 + 0.1 * 2.0 / 3.0, 1e-6), "warp: the clip's time map follows the markers (two thirds of the speed in the section)");
  expect(near(clipSourceAt(a.s, a.c(), tm2, tickAfter(a, 0.8)), 0.7, 1e-6), "warp: ... and the clip's speed after the last marker");
  a.c().length = audioClipNaturalLength(a.s, a.c());
  expect(near(secondsOf(a.s, a.c().length), 1.1, 0.002), "warp: the clip's natural length grows by the stretch");
  const AudioBuffer r = render(a.s);
  const std::vector<Onset> o = onsets(r, a.clipFrame());
  const double want[4] = {0.1, 0.3, 0.6, 0.8};
  bool times = o.size() == 4;
  for (size_t k = 0; times && k < 4; ++k) times = near(o[k].at, want[k], 0.004);
  char msg[200];
  std::snprintf(msg, sizeof msg, "warp: each section stretches on its own, sample-accurate at the markers (%zu onsets, %.4f %.4f %.4f %.4f)", o.size(),
                o.size() > 0 ? o[0].at : -1, o.size() > 1 ? o[1].at : -1, o.size() > 2 ? o[2].at : -1, o.size() > 3 ? o[3].at : -1);
  expect(times, msg);
  bool pitch = o.size() == 4;  // pitch kept: the 1 kHz bursts keep their period (zero crossings 0.5 ms apart)
  if (pitch) {
    const size_t f0 = a.clipFrame() + static_cast<size_t>(o[2].at * kSr) + 40;
    int zc = 0;
    for (size_t i = f0; i + 1 < f0 + 220 && i + 1 < r.frames; ++i) zc += (r.at(0, i) < 0) != (r.at(0, i + 1) < 0);
    pitch = zc >= 9 && zc <= 11;  // 220 frames of 1 kHz: about 10 crossings (resampled to this speed: about 7)
  }
  expect(pitch, "warp: a stretched section keeps its pitch");
  RenderOptions small;
  small.threads = 1;
  small.tail = a.s.barTicks();
  small.blockSize = 77;
  expect(renderSong(a.s, small).data == r.data, "warp: the render does not depend on the block size");
  // The channel's time mode stays as it was; other clips on it are unchanged.
  Fx b(path);
  Clip other = b.c();
  other.id = b.s.allocateId();
  other.track = 1;
  b.s.playlist.push_back(other);
  b.c().warp = a.c().warp;
  b.c().length = a.c().length;
  const std::vector<Onset> ob = onsets(render(b.s), b.clipFrame());
  expect(ob.size() == 6 && b.chan().plugin.params == Fx(path).chan().plugin.params, "warp: per clip, the other clip of the channel plays unwarped");
  // Reversing keeps the markers on their audio.
  Fx rv(path);
  rv.c().warp = a.c().warp;
  toggleClipReverse(rv.s, rv.c());
  rv.c().length = audioClipNaturalLength(rv.s, rv.c());
  expect(near(secondsOf(rv.s, rv.c().length), 1.1, 0.003), "warp: a reversed clip keeps its stretch");
  // Delete and clear.
  expect(deleteWarpMarker(a.s, a.clip, m1) && a.c().warp.size() == 2, "warp: a marker deletes");
  expect(clearWarp(a.s, a.clip) && a.c().warp.empty(), "warp: markers clear");
}

// A new marker snaps to a burst within 40 ms, but with a window of a few ticks (the playlist passes
// 8 pixels) it stays where it was clicked: a deep zoom never pulls it far from the pointer.
void testWarpSnapWindow(const std::string& path) {
  auto markerTick = [](Fx& a, const TempoMap& tm, uint32_t id) {
    for (const WarpMarkerView& v : warpMarkerViews(a.s, a.c(), tm))
      if (v.id == id) return v.tick;
    return -1.0;
  };
  const double clickAt = 0.47;  // 30 ms before the burst at 0.5 s
  {
    Fx a(path);
    TempoMap tm;
    tm.build(a.s);
    const uint32_t id = addWarpMarker(a.s, a.clip, tm, tickAfter(a, clickAt), true);
    expect(id && std::fabs(markerTick(a, tm, id) - tickAfter(a, 0.5)) < std::fabs(markerTick(a, tm, id) - tickAfter(a, clickAt)),
           "warp snap: without a window a new marker snaps to the burst 30 ms away");
  }
  {
    Fx a(path);
    TempoMap tm;
    tm.build(a.s);
    const double window = 0.005 * a.s.tempo * a.s.ppq / 60.0;  // 5 ms either side
    const uint32_t id = addWarpMarker(a.s, a.clip, tm, tickAfter(a, clickAt), true, window);
    expect(id && near(markerTick(a, tm, id), tickAfter(a, clickAt), 0.5), "warp snap: a small window keeps the marker where it was clicked");
  }
}

void testWarpLimits(const std::string& path) {
  Fx a(path);
  TempoMap tm;
  tm.build(a.s);
  const uint32_t m1 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3), false);
  const uint32_t m2 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.5), false);
  expect(addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3002), false) == 0, "warp limits: no second marker on the same spot");
  moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 0.0));  // dragged across the marker before it: stops at the limit
  std::vector<WarpMarker> w = a.c().warp;
  expect(!normalizeWarp(w), "warp limits: a drag across a neighbour leaves valid markers");
  const std::vector<WarpMarkerView> v = warpMarkerViews(a.s, a.c(), tm);
  double t1 = 0, t2 = 0;
  for (const WarpMarkerView& k : v) {
    if (k.id == m1) t1 = k.tick;
    if (k.id == m2) t2 = k.tick;
  }
  expect(t2 > t1, "warp limits: markers never cross");
  moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 50.0));
  w = a.c().warp;
  expect(!normalizeWarp(w), "warp limits: a far drag stops at the slowest speed");
  const AudioBuffer r = render(a.s);
  bool finite = r.frames > 0;
  for (float x : r.data) finite = finite && std::isfinite(x) && std::fabs(x) < 4;
  expect(finite, "warp limits: extreme sections render finite audio");
}

void testQuantize(const std::string& path) {
  Fx a(path);
  TempoMap tm;
  tm.build(a.s);
  // Bursts at 0.1, 0.3, 0.5, 0.7 s; a 1/16 grid at 120 BPM is 0.125 s (targets 0.125, 0.25, 0.5, 0.75).
  const Tick grid = a.s.ppq / 4;
  const int n = quantizeAudioClip(a.s, a.clip, tm, grid, 1.0);
  expect(n >= 3, "quantize: markers on the transients moved to the grid");
  TempoMap tm2;
  tm2.build(a.s);
  bool onGrid = true;
  int seen = 0;
  for (const WarpMarkerView& v : warpMarkerViews(a.s, a.c(), tm2)) {
    if (near(v.tick, static_cast<double>(a.c().start), 0.5)) continue;  // the start's own marker
    ++seen;
    const double g = static_cast<double>(grid);
    onGrid = onGrid && std::fabs(v.tick - std::round(v.tick / g) * g) < 1.0;
  }
  expect(seen >= 3 && onGrid, "quantize: strength 100% lands on the grid");
  Fx h(path);
  quantizeAudioClip(h.s, h.clip, tm, grid, 0.5);
  TempoMap tm3;
  tm3.build(h.s);
  double maxOff = 0;
  for (const WarpMarkerView& v : warpMarkerViews(h.s, h.c(), tm3)) {
    const double g = static_cast<double>(grid);
    maxOff = std::max(maxOff, std::fabs(v.tick - std::round(v.tick / g) * g));
  }
  // At most 0.05 s off the grid: halfway is 0.025 s.
  const double tickPerSec = a.s.tempo * a.s.ppq / 60.0;
  expect(maxOff > 0.018 * tickPerSec && maxOff < 0.032 * tickPerSec, "quantize: strength 50% moves halfway");
}

void testWarpFlExport(const std::string& path) {
  Fx a(path);
  a.chan().plugin.params[pp::SmpStretchMode] = 1;  // keeps the pitch: FL plays the sections at their pitch too
  TempoMap tm;
  tm.build(a.s);
  addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3), false);
  const uint32_t m2 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.5), false);
  moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 0.6));
  a.c().length = audioClipNaturalLength(a.s, a.c());
  a.c().fadeIn = 0.01, a.c().fadeOut = 0.02;
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(a.s, bytes, rep, eo), "warp flp: exported");
  expect(rep.counts["warped audio clips (written as one stretched clip per section)"] == 1, "warp flp: reported");
  Song imp;
  flp::Report ir;
  std::string err;
  expect(importFlpBytes(bytes, imp, ir, &err), "warp flp: re-imported");
  std::vector<const Clip*> clips;
  for (const Clip& k : imp.playlist)
    if (k.kind == ClipKind::Audio) clips.push_back(&k);
  std::sort(clips.begin(), clips.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
  // Sections: start .. 0.3 s, 0.3 .. 0.6 s (1.5 times as long), 0.6 s .. the end.
  bool ok = clips.size() == 3;
  for (size_t k = 0; ok && k < clips.size(); ++k) ok = clips[k]->warp.empty() && clips[k]->group != kNoId && clips[k]->group == clips[0]->group;
  expect(ok, "warp flp: one grouped clip per section");
  if (ok) {
    expect(near(clips[1]->audioStart, 0.3, 0.002) && near(clips[1]->stretch / clips[0]->stretch, 1.5, 0.02), "warp flp: the stretched section");
    expect(clips[0]->fadeIn > 0 && clips[1]->fadeIn == 0 && clips[1]->fadeOut == 0 && clips[2]->fadeOut > 0, "warp flp: fades at the outer edges only");
    const std::vector<Onset> o0 = onsets(render(a.s), a.clipFrame());
    const size_t f = static_cast<size_t>(std::llround(secondsOf(imp, clips[0]->start) * kSr));
    const std::vector<Onset> o1 = onsets(render(imp), f);
    bool same = o0.size() == o1.size() && !o0.empty();
    for (size_t k = 0; same && k < o0.size(); ++k) same = near(o0[k].at, o1[k].at, 0.004);
    expect(same, "warp flp: the sections play the bursts where the warped clip does");
  }
}

void testWarpUndoCollab(const std::string& path) {
  {  // undo: a drag is one step, undo gives the clip back
    Fx f(path);
    Document doc;
    doc.reset(f.s);
    TempoMap tm;
    tm.build(doc.song());
    Clip c = *doc.song().clip(f.clip);
    const uint32_t id = addWarpMarker(doc.song(), c, tm, tickAfter(f, 0.5), false);
    doc.beginGroup("warp audio");
    doc.editClip(f.clip, "add warp marker", [&c](Clip& k) { k.warp = c.warp; }, 77);
    for (double t : {0.52, 0.55, 0.6}) {
      Clip d = *doc.song().clip(f.clip);
      if (moveWarpMarker(doc.song(), d, tm, id, tickAfter(f, t))) doc.editClip(f.clip, "move warp marker", [&d](Clip& k) { k.warp = d.warp; }, 77);
    }
    doc.endGroup();
    doc.endGesture();
    expect(doc.song().clip(f.clip)->warp.size() == 2, "undo: the drag made its markers");
    expect(doc.undo() && doc.song().clip(f.clip)->warp.empty(), "undo: one step takes the whole drag back");
    expect(doc.redo() && doc.song().clip(f.clip)->warp.size() == 2, "undo: redo brings it back");
  }
  {  // collaboration: markers added on both sides merge, a move arrives
    using namespace gl::collab;
    using namespace sim;
    Clock clock;
    Net net(85);
    Peer a, b;
    a.label = "Ann", b.label = "Ben";
    for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
    Fx f(path);
    f.c().warp = {{1, 0.3, 0.3}};
    a.doc.reset(f.s);
    a.folder = tmpDir("k85_a");
    b.folder = tmpDir("k85_b");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    expect(a.s != nullptr, "collab: a session");
    if (!a.s) return;
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle(net, {&a, &b}, clock);
    expect(b.doc.song().clip(f.clip) && b.doc.song().clip(f.clip)->warp.size() == 1, "collab: the markers arrive");
    a.doc.editClip(f.clip, "add warp marker", [](Clip& k) { k.warp.push_back({2, 0.5, 0.5}); });
    b.doc.editClip(f.clip, "move warp marker", [](Clip& k) { k.warp[0].to = 0.35; });
    settle(net, {&a, &b}, clock);
    const Clip* ca = a.doc.song().clip(f.clip);
    const Clip* cb = b.doc.song().clip(f.clip);
    bool merged = ca && cb && ca->warp == cb->warp && ca->warp.size() == 2;
    if (merged) merged = std::any_of(ca->warp.begin(), ca->warp.end(), [](const WarpMarker& m) { return m.id == 1 && m.to == 0.35; });
    expect(merged, "collab: a marker added on one side and one moved on the other both stay, both sides agree");
  }
}

// K-140 + warp: a warped clip cut short and looped repeats its warped cut part, pass by pass.
void testWarpCutLoop(const std::string& path) {
  Fx a(path);
  TempoMap tm;
  tm.build(a.s);
  addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3), false);
  const uint32_t m2 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.5), false);
  moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 0.6));  // bursts now at 0.1, 0.3, 0.6, 0.8 s
  // Cut at 0.7 s (between the third and fourth burst), then loop it over three passes.
  a.c().length = static_cast<Tick>(std::llround(tickAfter(a, 0.7) - static_cast<double>(a.c().start)));
  TempoMap tm2;
  tm2.build(a.s);
  a.c() = withClipLoop(a.s, tm2, a.c(), true);
  expect(a.c().loop && a.c().loopLength == 0 && a.c().warp.size() == 3, "cut loop: an audio clip loops its window, its markers stay");
  const Tick pass = audioLoopLength(a.s, a.c(), tm2);
  expect(near(secondsOf(a.s, pass), 0.7, 0.003), "cut loop: one pass is the warped cut part");
  a.c().length = 3 * pass;
  const std::vector<Onset> o = onsets(render(a.s), a.clipFrame());
  bool ok = o.size() == 9;
  const double ps = secondsOf(a.s, pass);
  for (size_t k = 0; ok && k < o.size(); ++k) ok = near(o[k].at, ps * static_cast<double>(k / 3) + (k % 3 == 0 ? 0.1 : k % 3 == 1 ? 0.3 : 0.6), 0.005);
  expect(ok, "cut loop: each pass plays the warped cut part");
  // Passes as clips (what FL export writes before splitting the sections) play the same.
  Fx b(path);
  b.s.playlist = audioLoopPasses(a.s, a.c(), tm2);
  for (size_t k = 1; k < b.s.playlist.size(); ++k) b.s.playlist[k].id = b.s.allocateId();
  const std::vector<Onset> ob = onsets(render(b.s), b.clipFrame());
  bool same = ob.size() == o.size();
  for (size_t k = 0; same && k < o.size(); ++k) same = near(o[k].at, ob[k].at, 0.003);
  expect(same, "cut loop: the passes laid out as clips play the same");
}


// A warped clip (bursts at 0.1, 0.3, 0.6, 0.8 s; 1.1 s long).
void warpIt(Fx& a) {
  TempoMap tm;
  tm.build(a.s);
  addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.3), false);
  const uint32_t m2 = addWarpMarker(a.s, a.clip, tm, tickAfter(a, 0.5), false);
  moveWarpMarker(a.s, a.clip, tm, m2, tickAfter(a, 0.6));
  a.c().length = audioClipNaturalLength(a.s, a.c());
}

// Each burst at want[k] * scale seconds after the clip's start, and every
// marker at the same fraction of the clip as before (on its own audio).
void checkScaled(Fx& a, double scale, const std::vector<double>& fracBefore, const char* what) {
  const double want[4] = {0.1, 0.3, 0.6, 0.8};
  const std::vector<Onset> o = onsets(render(a.s), a.clipFrame(), 0.12 * scale);  // (a stretched burst rings on: one onset each)
  bool times = o.size() == 4;
  for (size_t k = 0; times && k < 4; ++k) times = near(o[k].at, want[k] * scale, 0.006);
  char msg[240];
  std::snprintf(msg, sizeof msg, "%s: each section keeps its timing, scaled by %.3f (%zu onsets, %.4f %.4f %.4f %.4f)", what, scale, o.size(),
                o.size() > 0 ? o[0].at : -1, o.size() > 1 ? o[1].at : -1, o.size() > 2 ? o[2].at : -1, o.size() > 3 ? o[3].at : -1);
  expect(times, msg);
  char lm[200];
  std::snprintf(lm, sizeof lm, "%s: the clip's length is the warped length, scaled (%.4f s)", what, secondsOf(a.s, a.c().length));
  expect(near(secondsOf(a.s, a.c().length), 1.1 * scale, std::max(0.004, 0.51 * secondsOf(a.s, 1))), lm);  // (whole ticks)
  TempoMap tm;
  tm.build(a.s);
  const std::vector<WarpMarkerView> v = warpMarkerViews(a.s, a.c(), tm);
  bool same = v.size() == fracBefore.size();
  for (size_t k = 0; same && k < v.size(); ++k)
    same = near((v[k].tick - static_cast<double>(a.c().start)) / static_cast<double>(a.c().length), fracBefore[k], 0.004);
  expect(same, (std::string(what) + ": the markers stay in place relative to the audio").c_str());
}

std::vector<double> markerFractions(Fx& a) {
  TempoMap tm;
  tm.build(a.s);
  std::vector<double> f;
  for (const WarpMarkerView& k : warpMarkerViews(a.s, a.c(), tm))
    f.push_back((k.tick - static_cast<double>(a.c().start)) / static_cast<double>(a.c().length));
  return f;
}

void testWarpStretchFit(const std::string& path) {
  {  // warp, then stretch the whole clip x2
    Fx a(path);
    warpIt(a);
    const std::vector<WarpMarker> m = a.c().warp;
    const std::vector<double> f = markerFractions(a);
    expect(setClipStretch(a.s, a.clip, 2.0, true), "warp + stretch: the clip stretches");
    expect(a.c().warp == m, "warp + stretch: the markers themselves are unchanged");
    checkScaled(a, 2.0, f, "warp + stretch");
    expect(resetClipStretch(a.s, a.clip), "warp + stretch: resets");
    checkScaled(a, 1.0, f, "warp + stretch reset");
  }
  {  // warp, then fit to the tempo: 1.1 s at 120 BPM is two beats -> plays in exactly 1 s
    Fx a(path);
    warpIt(a);
    const std::vector<double> f = markerFractions(a);
    expect(fitClipToTempo(a.s, a.clip) == 2, "warp + fit: counts the beats of the warped length");
    expect(a.c().length == 2 * a.s.ppq, "warp + fit: two beats long");
    checkScaled(a, 1.0 / 1.1, f, "warp + fit");
  }
  {  // warp, then halve the tempo: the clip keeps its seconds (and so its warp)
    Fx a(path);
    warpIt(a);
    const std::vector<double> f = markerFractions(a);
    const std::vector<Clip> before = a.s.playlist;
    TempoMap oldMap;
    oldMap.build(a.s);
    a.s.tempo = 60;
    TempoMap newMap;
    newMap.build(a.s);
    expect(refitClipsToTempo(a.s, before, oldMap, newMap) == 1, "warp + tempo: the clip is refitted");
    checkScaled(a, 1.0, f, "warp + tempo");
  }
}

// A steady 220 Hz tone (slowly swelling), `seconds` long.
std::string toneFile(double seconds) {
  const size_t n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.4 * std::sin(2 * kPi * 220.0 * t) * (1.0 + 0.3 * std::sin(t * 5.0)));
  }
  const std::string path = (fs::temp_directory_path() / "gl_k85_tone.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

// Dragging a warp marker while the song plays through the section it
// stretches: the program is rebuilt every frame (renders deferred, as the app
// does during a pointer gesture); the stream must neither click nor drift
// from where the new time map says the clip is, and the release (the render
// made in the background, then crossfaded in) must not jump.
void testPlayThroughWarpDrag() {
  const std::string tone = toneFile(4.0);
  Fx a(tone);
  a.chan().plugin.params[pp::SmpStretchMode] = static_cast<float>(stretch::kStretch);
  Document doc(a.s);
  const ObjectId clip = a.clip;
  TempoMap tm;
  tm.build(doc.song());
  const double t0 = static_cast<double>(doc.song().barTicks());
  auto tickAt = [&](double sec) { return t0 + sec * doc.song().tempo * doc.song().ppq / 60.0; };
  Clip k = *doc.song().clip(clip);
  addWarpMarker(doc.song(), k, tm, tickAt(0.5), false);
  const uint32_t m2 = addWarpMarker(doc.song(), k, tm, tickAt(2.5), false);
  moveWarpMarker(doc.song(), k, tm, m2, tickAt(2.4));
  k.length = audioClipNaturalLength(doc.song(), k);
  doc.editClip(clip, "warp", [&k](Clip& x) { x = k; });

  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.setAsyncSamples(false);
  b.build(doc.song());
  stretch::waitForPendingRenders();
  b.setAsyncSamples(true);
  b.setDeferRenders(true);  // the pointer is down from the start (realtime stream plays)
  std::shared_ptr<const Program> prog = b.build(doc.song());
  const int block = 512;
  std::vector<float> L;
  int64_t clock = static_cast<int64_t>(a.clipFrame());
  double drift = 0;
  int frames = 0;
  auto play = [&](int blocks) {
    for (int i = 0; i < blocks; ++i) {
      std::vector<float> l(block, 0.f), r(block, 0.f);
      for (const ProgChannel& pc : prog->channels) renderAudioClips(pc.audio, prog->tempoMap, kSr, 0, clock, 0, block, l.data(), r.data());
      L.insert(L.end(), l.begin(), l.end());
      clock += block;
    }
  };
  auto streamDrift = [&]() {  // the realtime stream's source position against the time map's, seconds
    for (const ProgChannel& pc : prog->channels)
      for (const ProgAudioClip& c : pc.audio)
        if (c.rt) {
          TempoMap m;
          m.build(doc.song());
          const double tick = m.tickAtSeconds(static_cast<double>(clock) / kSr);
          return std::fabs(c.rt->pos - clipSourceAt(doc.song(), *doc.song().clip(clip), m, tick));
        }
    return 1e9;
  };
  play(40);  // about 0.46 s steady, then the drag (the playhead in the dragged section)
  const size_t dragFrom = L.size();
  for (int f = 0; f < 120; ++f) {  // two frames of audio per UI frame; the marker moves 4 ms a frame
    Clip c = *doc.song().clip(clip);
    TempoMap m;
    m.build(doc.song());
    moveWarpMarker(doc.song(), c, m, m2, tickAt(2.4 - 0.004 * (f + 1)));
    doc.editClip(clip, "move warp marker", [&c](Clip& x) { x.warp = c.warp; }, 7);
    prog = b.build(doc.song());
    play(2);
    drift = std::max(drift, f > 4 ? streamDrift() : 0.0);
    ++frames;
  }
  const size_t dragTo = L.size();
  b.setDeferRenders(false);  // the release: the render starts in the background
  prog = b.build(doc.song());
  play(20);
  stretch::waitForPendingRenders();
  prog = b.build(doc.song());
  play(40);
  auto maxStep = [&](size_t x, size_t z) {
    double mx = 0;
    for (size_t i = x + 1; i < z; ++i) mx = std::max(mx, static_cast<double>(std::fabs(L[i] - L[i - 1])));
    return mx;
  };
  const double steady = maxStep(static_cast<size_t>(5 * block), dragFrom);
  const double dragging = maxStep(dragFrom, dragTo), release = maxStep(dragTo, L.size());
  std::printf("# warp drag: steady largest step %.4f, while dragging %.4f, release %.4f, stream drift %.2f ms over %d frames\n", steady, dragging,
              release, drift * 1000.0, frames);
  expect(steady > 1e-3 && dragging < 1.5 * steady, "dragging a warp marker while playing: no clicks");
  expect(drift < 0.006, "dragging a warp marker while playing: the audio follows the marker (stream within 6 ms of the time map)");
  expect(release < 1.5 * steady, "releasing a warp marker while playing: the background render crossfades in without a jump");
}

// A marker drag lands like a clip move: no jump at the grab, Alt follows the
// pointer 1:1 with the grab offset, on the grid it sits on the nearest line,
// moves monotonically and never skips a line.
void testWarpDragFeel() {
  expect(warpDragTick(1000.3, 1002, 1002, 1) == 1000.3, "drag feel: free, an unmoved pointer leaves the marker where it is (on the grid the UI waits for a move)");
  bool free = true;
  for (Tick p = 900; p < 1100; ++p) free = free && warpDragTick(1000.3, 1002, p, 1) == 1000.3 + static_cast<double>(p - 1002);
  expect(free, "drag feel: Alt (free) follows the pointer 1:1, keeping the grab offset");
  bool onGrid = true, mono = true, noSkip = true;
  double prev = warpDragTick(1000.3, 1002, 700, 96);
  for (Tick p = 701; p < 1400; ++p) {
    const double t = warpDragTick(1000.3, 1002, p, 96);
    onGrid = onGrid && std::fmod(t, 96.0) == 0.0;
    mono = mono && t >= prev;
    noSkip = noSkip && t - prev <= 96.0;
    prev = t;
  }
  expect(onGrid && mono && noSkip, "drag feel: on the grid it lands on the nearest line, monotonic, one line at a time");
  expect(warpDragTick(10.0, 500, 0, 1) == 0.0, "drag feel: never before the song's start");
}

// The work of one frame of a marker drag on a long clip with many markers
// (the edit, the marker views, the waveform's time map over a wide view and
// the program build with renders deferred) against one frame of a plain clip
// move on the same song: within budget and within a few times the move's.
void testWarpDragFrameCost() {
  const std::string tone = toneFile(4.0);
  Fx a(tone);
  TempoMap tm;
  tm.build(a.s);
  const double t0 = static_cast<double>(a.s.barTicks());
  auto tickAt = [&](double sec) { return t0 + sec * a.s.tempo * a.s.ppq / 60.0; };
  for (int k = 1; k < 60; ++k) addWarpMarker(a.s, a.clip, tm, tickAt(0.06 * k), false);
  const uint32_t mid = warpMarkerViews(a.s, a.c(), tm)[30].id;
  Document doc(a.s);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.setAsyncSamples(false);
  b.build(doc.song());
  stretch::waitForPendingRenders();
  b.setAsyncSamples(true);
  b.setDeferRenders(true);
  b.build(doc.song());
  const int frames = 120;
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  double sink = 0;
  const double w0 = now();
  for (int f = 0; f < frames; ++f) {
    const Song& s = doc.song();
    Clip c = *s.clip(a.clip);
    TempoMap m;
    m.build(s);
    moveWarpMarker(s, c, m, mid, tickAt(1.8 + 0.002 * (f % 40)));
    doc.editClip(a.clip, "move warp marker", [&c](Clip& x) { x.warp = c.warp; }, 9);
    for (const WarpMarkerView& v : warpMarkerViews(doc.song(), *doc.song().clip(a.clip), m)) sink += v.tick;
    const std::vector<WarpPoint> w = clipWarp(doc.song(), *doc.song().clip(a.clip));
    for (int x = 0; x < 2400; ++x) sink += warpSourceAt(w, 4.0 * x / 2400.0);  // a 2400 px wide waveform
    b.build(doc.song());
  }
  const double warpMs = (now() - w0) / frames;
  const double m0 = now();
  for (int f = 0; f < frames; ++f) {
    doc.editClip(a.clip, "move clip", [&](Clip& x) { x.start = static_cast<Tick>(t0) + (f % 2) * 4; }, 10);
    b.build(doc.song());
  }
  const double moveMs = (now() - m0) / frames;
  std::printf("# frame cost: marker drag %.3f ms, clip move %.3f ms (%d frames, 60 markers)%s\n", warpMs, moveMs, frames, sink > 0 ? "" : " ");
  expect(warpMs < std::max(4.0 * moveMs, 2.0) && warpMs < 8.0, "drag cost: a marker drag frame stays within budget (a few times a clip move's)");
}
}  // namespace

int main() {
  test::hermeticTestEnv("k85");
  const std::string path = clickFile();
  testHandlesTrimClips(path);
  testHandlesNeverCross();
  testUntrimmedUnchanged(path);
  testReverseKeepsAudio(path);
  testProcessing(path);
  testFl(path);
  testWarpMap();
  testWarpSaveLoad(path);
  testWarpPlayback(path);
  testWarpLimits(path);
  testWarpSnapWindow(path);
  testQuantize(path);
  testWarpFlExport(path);
  testWarpUndoCollab(path);
  testWarpCutLoop(path);
  testWarpStretchFit(path);
  testPlayThroughWarpDrag();
  testWarpDragFeel();
  testWarpDragFrameCost();
  return finish();
}
