// Gridline — J-193: a stretched audio clip keeps its musical structure when
// the project tempo changes; a clip the user never stretched keeps the audio
// it was cut to (its border follows the tempo instead).
//  * stretch a 176 BPM loop to fit 170 BPM: the clip is tempo-locked; at 140
//    and 200 BPM it keeps its ticks and its audio span and its beats land on
//    the song's beats (rendered onsets); Resample: pitch ratio = tempo ratio,
//    Stretch: pitch kept;
//  * tempo automation in the middle of a locked clip: onsets on the tempo map;
//  * split / trim after a tempo change; "Follow the project tempo" off keeps
//    the sound; old synced clips keep their pitch;
//  * unstretched clip at 120 BPM over 2 s: at 90 and 150 BPM the same audio,
//    length = its seconds at the new tempo, audio at the same seconds; a cut
//    clip keeps its cut exactly over a tempo drag (one undo step); tempo
//    automation added later refits it too;
//  * save / load, old projects unchanged, FL export / import.
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/ClipRate.h"
#include "../src/core/Document.h"
#include "../src/core/Params.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/PlaylistTools.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLoopBpm = 176.0;
constexpr int kLoopBeats = 8;

std::string writeWavFile(const std::string& name, const std::vector<float>& x) {
  AudioBuffer b(2, x.size());
  for (size_t i = 0; i < x.size(); ++i) b.at(0, i) = b.at(1, i) = x[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

// Short 2 kHz bursts at the given seconds, silence between.
std::vector<float> clicks(double total, const std::vector<double>& at) {
  std::vector<float> x(static_cast<size_t>(total * kSr), 0.0f);
  for (double t : at) {
    const size_t a = static_cast<size_t>(std::llround(t * kSr));
    for (size_t i = 0; i < static_cast<size_t>(0.004 * kSr) && a + i < x.size(); ++i)
      x[a + i] = static_cast<float>(0.9 * std::sin(2 * kPi * 2000 * static_cast<double>(i) / kSr));
  }
  return x;
}
// The 176 BPM loop: a click on each of its 8 beats.
std::string loopFile() {
  std::vector<double> at;
  for (int k = 0; k < kLoopBeats; ++k) at.push_back(k * 60.0 / kLoopBpm);
  return writeWavFile("gl_j193_loop.wav", clicks(kLoopBeats * 60.0 / kLoopBpm, at));
}
std::string toneFile() {
  std::vector<float> x(static_cast<size_t>(3.0 * kSr));
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 440 * static_cast<double>(i) / kSr));
  return writeWavFile("gl_j193_tone.wav", x);
}
// 2 s with clicks at 0.25, 1.0 and 1.75 s.
std::string takeFile() { return writeWavFile("gl_j193_take.wav", clicks(2.0, {0.25, 1.0, 1.75})); }

// One sampler channel (time mode `mode`) and one audio clip of its natural length at the song start.
Song clipSong(const std::string& path, int mode, double bpm) {
  Song s = makeEmptySong();
  s.tempo = bpm;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Audio";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(mode);
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.length = audioClipNaturalLength(s, c);
  s.playlist.push_back(c);
  return s;
}

// The loop stretch-resized (the playlist's stretch edit) to 8 beats at 170 BPM.
Song lockedLoop(const std::string& path, int mode) {
  Song s = clipSong(path, mode, 170.0);
  const ObjectId id = s.playlist[0].id;
  TempoMap tm;
  tm.build(s);
  s.playlist = pltools::resizeEnds(s, s.playlist, {id}, kLoopBeats * s.ppq - s.playlist[0].length, true, &tm, false);
  return s;
}

void setTempo(Document& d, double bpm, bool merge = false) { d.setParam(songParam(SongParam::Tempo), bpm, merge); }

std::vector<float> render(const Song& s) {
  RenderOptions o;
  AudioBuffer b = renderSong(s, o);
  return std::vector<float>(b.plane(0), b.plane(0) + b.frames);
}
// Onsets: the first sample above `thr` after at least 30 ms below it.
std::vector<double> onsets(const std::vector<float>& y, float thr) {
  std::vector<double> out;
  size_t quiet = static_cast<size_t>(kSr);
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::fabs(y[i]) > thr) {
      if (quiet >= static_cast<size_t>(0.03 * kSr)) out.push_back(static_cast<double>(i) / kSr);
      quiet = 0;
    } else {
      ++quiet;
    }
  }
  return out;
}
double sourceAtEnd(const Song& s, const Clip& c) {
  TempoMap m;
  m.build(s);
  return clipSourceAt(s, c, m, static_cast<double>(c.start + s.clipLength(c)));
}
double secondsAt(const Song& s, Tick t) {
  TempoMap m;
  m.build(s);
  return m.secondsAt(static_cast<double>(t));
}

using cd = std::complex<double>;
void fft(std::vector<cd>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / static_cast<double>(len);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v, a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}
// Strongest partial between 200 and 800 Hz in y[a, a + 16384), interpolated.
double peakHz(const std::vector<float>& y, size_t a) {
  const size_t N = 16384;
  if (a + N > y.size()) return 0;
  std::vector<cd> b(N);
  for (size_t i = 0; i < N; ++i) {
    const double w = 2 * kPi * static_cast<double>(i) / static_cast<double>(N);
    b[i] = static_cast<double>(y[a + i]) * (0.35875 - 0.48829 * std::cos(w) + 0.14128 * std::cos(2 * w) - 0.01168 * std::cos(3 * w));
  }
  fft(b);
  const size_t k0 = static_cast<size_t>(200.0 * N / kSr), k1 = static_cast<size_t>(800.0 * N / kSr);
  size_t best = k0;
  for (size_t k = k0; k <= k1; ++k)
    if (std::abs(b[k]) > std::abs(b[best])) best = k;
  const double ym = std::log(std::abs(b[best - 1])), y0 = std::log(std::abs(b[best])), yp = std::log(std::abs(b[best + 1]));
  return (static_cast<double>(best) + 0.5 * (ym - yp) / (ym - 2 * y0 + yp)) * kSr / static_cast<double>(N);
}
double cents(double f, double ref) { return 1200 * std::log2(f / ref); }

// Every one of `want` (seconds) has an onset within `tol`, and there are no others.
bool onBeats(const std::vector<double>& got, const std::vector<double>& want, double tol, std::string& why) {
  char b[200];
  std::snprintf(b, sizeof b, " (%zu onsets, want %zu", got.size(), want.size());
  why = b;
  bool ok = got.size() == want.size();
  for (size_t k = 0; k < want.size() && k < got.size(); ++k) {
    if (std::fabs(got[k] - want[k]) > tol) {
      ok = false;
      std::snprintf(b, sizeof b, "; #%zu at %.4f s, want %.4f", k, got[k], want[k]);
      why += b;
    }
  }
  why += ")";
  return ok;
}

}  // namespace

int main() {
  const std::string loop = loopFile(), tone = toneFile(), take = takeFile();
  sampleCache().clear();
  char name[400];

  // ---------------------------------------------------------------- the stretch locks the clip to the tempo
  for (int mode : {0, 1}) {
    const char* mn = mode == 0 ? "Resample" : "Stretch";
    Song s = lockedLoop(loop, mode);
    const Clip c0 = s.playlist[0];
    std::snprintf(name, sizeof name, "%s: a stretch-resize locks the clip (tempoLock %d, syncBpm %.3f, %lld ticks)", mn, c0.tempoLock ? 1 : 0,
                  c0.syncBpm, static_cast<long long>(c0.length));
    expect(c0.tempoLock && c0.syncBpm > 0 && c0.length == kLoopBeats * s.ppq, name);
    // (the natural length is whole ticks: the loop ends within half a tick of the clip end)
    std::snprintf(name, sizeof name, "%s: locking keeps the mapping (the loop ends at the clip end: %.5f s)", mn, sourceAtEnd(s, c0));
    expect(std::fabs(sourceAtEnd(s, c0) - kLoopBeats * 60.0 / kLoopBpm) < 0.5 * 60.0 / (170.0 * s.ppq), name);

    for (double bpm : {140.0, 200.0}) {
      Document d(s);
      setTempo(d, bpm);
      const Clip& c = d.song().playlist[0];
      std::snprintf(name, sizeof name, "%s, 170 -> %.0f BPM: the clip keeps its ticks and audio span (%lld ticks, %.4f..%.4f)", mn, bpm,
                    static_cast<long long>(c.length), c.audioStart, sourceAtEnd(d.song(), c));
      expect(c.length == c0.length && c.audioStart == c0.audioStart && c.audioEnd == c0.audioEnd && c.stretch == c0.stretch &&
                 std::fabs(sourceAtEnd(d.song(), c) - sourceAtEnd(s, c0)) < 1e-9,
             name);
      // Rendered: the loop's beats on the song's beats.
      std::vector<double> want;
      for (int k = 0; k < kLoopBeats; ++k) want.push_back(k * 60.0 / bpm);
      std::string why;
      const bool ok = onBeats(onsets(render(d.song()), mode == 0 ? 0.3f : 0.2f), want, mode == 0 ? 0.002 : 0.012, why);
      std::snprintf(name, sizeof name, "%s at %.0f BPM: the rendered beats land on the song's beats%s", mn, bpm, why.c_str());
      expect(ok, name);
    }
  }

  // ---------------------------------------------------------------- pitch: Resample follows the tempo, Stretch keeps it
  for (int mode : {0, 1, 7}) {
    const char* mn = mode == 0 ? "Resample" : mode == 1 ? "Stretch" : "Rubber Band";
    Song s = clipSong(tone, mode, 170.0);
    const ObjectId id = s.playlist[0].id;
    TempoMap tm;
    tm.build(s);
    // Stretched to 8 beats (3 s of tone at 170 BPM is 8.5 beats).
    s.playlist = pltools::resizeEnds(s, s.playlist, {id}, 8 * s.ppq - s.playlist[0].length, true, &tm, false);
    const double tf = s.playlist[0].stretch;
    const double at170 = peakHz(render(s), static_cast<size_t>(0.3 * kSr));
    Document d(s);
    setTempo(d, 140.0);
    const double at140 = peakHz(render(d.song()), static_cast<size_t>(0.3 * kSr));
    const double want170 = mode == 0 ? 440.0 / tf : 440.0;
    const double want140 = mode == 0 ? want170 * 140.0 / 170.0 : 440.0;
    std::snprintf(name, sizeof name, "%s locked clip: in tune at the lock tempo (%.2f Hz, want %.2f)", mn, at170, want170);
    expect(std::fabs(cents(at170, want170)) < 5.0, name);
    std::snprintf(name, sizeof name, "%s locked clip at 140 BPM: %s (%.2f Hz, want %.2f, ratio %.4f vs tempo ratio %.4f)", mn,
                  mode == 0 ? "the pitch follows the speed" : "the pitch is kept", at140, want140, at140 / at170, 140.0 / 170.0);
    expect(std::fabs(cents(at140, want140)) < 5.0, name);
  }
  {  // Old tempo-synced clips (no lock) keep their pitch in Resample mode, as before.
    Song s = clipSong(tone, 0, 170.0);
    s.playlist[0].syncBpm = 170.0;
    s.playlist[0].length = 8 * s.ppq;
    Document d(s);
    setTempo(d, 140.0);
    const double hz = peakHz(render(d.song()), static_cast<size_t>(0.3 * kSr));
    std::snprintf(name, sizeof name, "an older synced clip (no lock) keeps its pitch in Resample mode (%.2f Hz)", hz);
    expect(std::fabs(cents(hz, 440.0)) < 5.0 && !d.song().playlist[0].tempoLock, name);
  }

  // ---------------------------------------------------------------- tempo automation in the middle of a locked clip
  {
    Song s = lockedLoop(loop, 0);
    auto td = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = kSongId;
    a.index = static_cast<uint32_t>(SongParam::Tempo);
    a.points = {{0, static_cast<float>(td->toNormalized(170)), 0, AutoShape::Hold},
                {static_cast<Tick>(3 * s.ppq), static_cast<float>(td->toNormalized(130)), 0, AutoShape::Hold}};
    s.automations.push_back(a);
    Clip ac;
    ac.id = s.allocateId(), ac.kind = ClipKind::Automation, ac.source = a.id, ac.track = 2, ac.length = 16 * s.ppq;
    const Clip loopClip = s.playlist[0];
    Document d(s);
    d.editSong("tempo automation", [&](Song& x) { x.playlist.push_back(ac); });
    TempoMap m;
    m.build(d.song());
    std::vector<double> want;
    for (int k = 0; k < kLoopBeats; ++k) want.push_back(m.secondsAt(static_cast<double>(k * s.ppq)));
    std::string why;
    const bool ok = onBeats(onsets(render(d.song()), 0.3f), want, 0.002, why);
    std::snprintf(name, sizeof name, "tempo automation 170 -> 130 BPM inside a locked clip: its beats follow the tempo map%s", why.c_str());
    expect(!m.constant() && ok && d.song().playlist[0] == loopClip, name);
  }

  // ---------------------------------------------------------------- split / trim after a tempo change
  {
    Song s = lockedLoop(loop, 1);
    const ObjectId id = s.playlist[0].id;
    Document d(s);
    setTempo(d, 140.0);
    Song t = d.song();
    // Source seconds per tick of the locked clip (the same at any tempo).
    const double spt = sourceAtEnd(s, s.playlist[0]) / static_cast<double>(s.playlist[0].length);
    const ObjectId r = splitClipAt(t, id, 4 * t.ppq);
    std::snprintf(name, sizeof name, "split at beat 4 after 170 -> 140 BPM: the right part starts on the loop's beat 4 (%.5f s)",
                  r != kNoId ? t.clip(r)->audioStart : -1.0);
    expect(r != kNoId && std::fabs(t.clip(r)->audioStart - 4 * t.ppq * spt) < 1e-9 && std::fabs(t.clip(r)->audioStart - 4 * 60.0 / kLoopBpm) < 1e-3 &&
               t.clip(r)->tempoLock && t.clip(id)->length == 4 * t.ppq,
           name);
    TempoMap tm;
    tm.build(d.song());
    const std::vector<Clip> tr = pltools::trimStarts(d.song(), tm, d.song().playlist, {id}, t.ppq, false, false);
    std::snprintf(name, sizeof name, "trim one beat after the tempo change: the clip starts on the loop's beat 1 (%.5f s)", tr[0].audioStart);
    expect(std::fabs(tr[0].audioStart - t.ppq * spt) < 1e-9 && std::fabs(tr[0].audioStart - 60.0 / kLoopBpm) < 1e-3 && tr[0].length == 7 * t.ppq, name);
  }

  // ---------------------------------------------------------------- "Follow the project tempo": on / off, undo
  {
    Song s = lockedLoop(loop, 1);
    const ObjectId id = s.playlist[0].id;
    Document d(s);
    setTempo(d, 140.0);
    const double span = sourceAtEnd(d.song(), *d.song().clip(id));
    const size_t pos = d.history().position();
    d.editSong("follow tempo", [&](Song& x) { setClipFollowsTempo(x, id, false); });
    const Clip& c = *d.song().clip(id);
    std::snprintf(name, sizeof name, "off at 140 BPM: same length, same audio, the speed kept (stretch %.4f, span %.5f vs %.5f)", c.stretch,
                  sourceAtEnd(d.song(), c), span);
    expect(c.syncBpm == 0 && !c.tempoLock && c.length == kLoopBeats * s.ppq && std::fabs(sourceAtEnd(d.song(), c) - span) < 1e-6 &&
               d.history().position() == pos + 1,
           name);
    setTempo(d, 170.0);
    const Clip& c2 = *d.song().clip(id);
    // (its seconds stay: 8 beats at 140 BPM are 9.71 beats at 170)
    const Tick want = static_cast<Tick>(std::llround(kLoopBeats * s.ppq * 170.0 / 140.0));
    std::snprintf(name, sizeof name, "then 140 -> 170 BPM: the unlocked clip keeps its audio, its border follows (%lld ticks, want %lld)",
                  static_cast<long long>(c2.length), static_cast<long long>(want));
    expect(std::llabs(c2.length - want) <= 1 && std::fabs(c2.audioEnd) < 1e-12, name);
    d.undo();
    d.undo();
    expect(d.song().clip(id)->tempoLock && d.song().clip(id)->length == kLoopBeats * s.ppq, "undo: the tempo change and the toggle (one step each)");
    d.editSong("follow tempo", [&](Song& x) { setClipFollowsTempo(x, id, false); });
    d.editSong("follow tempo", [&](Song& x) { setClipFollowsTempo(x, id, true); });
    const Clip& c3 = *d.song().clip(id);
    expect(c3.tempoLock && std::fabs(c3.syncBpm - 140.0) < 1e-6 && c3.length == kLoopBeats * s.ppq,
           "on again at 140 BPM: locked at 140 with its geometry");
    // Time (clip settings) is a stretch: it locks too.
    Song u = clipSong(loop, 1, 170.0);
    setClipStretch(u, u.playlist[0].id, 1.25, true);
    expect(u.playlist[0].tempoLock && u.playlist[0].syncBpm > 0, "a typed time stretch locks the clip");
    // "Original length" on a locked clip: unstretched, fixed seconds again, its audio kept.
    Song o = lockedLoop(loop, 1);
    const double oSpan = sourceAtEnd(o, o.playlist[0]);
    resetClipStretch(o, o.playlist[0].id);
    expect(!o.playlist[0].tempoLock && o.playlist[0].syncBpm == 0 && o.playlist[0].stretch == 1.0 &&
               std::fabs(sourceAtEnd(o, o.playlist[0]) - oSpan) < 0.5 * 60.0 / (170.0 * o.ppq),
           "original length: the lock is gone, the clip plays its audio at its own speed");
    Song f = clipSong(loop, 1, 170.0);
    fitClipToTempo(f, f.playlist[0].id);
    expect(f.playlist[0].tempoLock && f.playlist[0].syncBpm > 0, "fit to tempo locks the clip");
  }

  // ---------------------------------------------------------------- unstretched clip: its audio stays, its border follows
  for (int mode : {0, 1}) {
    const char* mn = mode == 0 ? "Resample" : "Stretch";
    Song s = clipSong(take, mode, 120.0);
    const Clip c0 = s.playlist[0];
    expect(c0.length == 4 * s.ppq && !c0.tempoLock, "an unstretched 2 s clip at 120 BPM: 4 beats, not locked");
    for (double bpm : {90.0, 150.0}) {
      Document d(s);
      const size_t pos = d.history().position();
      setTempo(d, bpm);
      const Clip& c = d.song().playlist[0];
      const Tick want = static_cast<Tick>(std::llround(2.0 * bpm / 60.0 * s.ppq));
      std::snprintf(name, sizeof name, "%s, 120 -> %.0f BPM: same audio (%.4f..%.4f s), length = 2 s at the new tempo (%lld ticks, want %lld), one step", mn,
                    bpm, c.audioStart, sourceAtEnd(d.song(), c), static_cast<long long>(c.length), static_cast<long long>(want));
      expect(c.length == want && c.audioStart == 0 && c.audioEnd == 0 && c.syncBpm == 0 && c.start == 0 &&
                 std::fabs(secondsAt(d.song(), c.length) - 2.0) < 1e-9 && d.history().position() == pos + 1,
             name);
      std::string why;
      const bool ok = onBeats(onsets(render(d.song()), 0.3f), {0.25, 1.0, 1.75}, 0.001, why);
      std::snprintf(name, sizeof name, "%s at %.0f BPM: the audio plays at the same seconds from the clip start%s", mn, bpm, why.c_str());
      expect(ok, name);
      d.undo();
      expect(d.song().playlist[0] == c0 && d.song().tempo == 120.0, "undo restores the tempo and the clip");
    }
  }
  {  // A cut clip keeps its cut exactly over a tempo drag (one undo step).
    Song s = clipSong(take, 1, 120.0);
    s.playlist[0].length = 3 * s.ppq;  // cut at 1.5 s
    s.playlist[0].audioStart = 0.0;
    const Clip c0 = s.playlist[0];
    Document d(s);
    const size_t pos = d.history().position();
    for (int k = 1; k <= 30; ++k) setTempo(d, 120.0 - k, true);
    const Clip mid = d.song().playlist[0];
    for (int k = 29; k >= 0; --k) setTempo(d, 120.0 - k, true);
    d.endGesture();
    const Clip& c = d.song().playlist[0];
    std::snprintf(name, sizeof name, "a cut clip over a tempo drag 120 -> 90 -> 120: cut kept (audioEnd %.6f), length back (%lld), one step (%zu)",
                  c.audioEnd, static_cast<long long>(c.length), d.history().position() - pos);
    expect(std::fabs(c.audioEnd - 1.5) < 1e-9 && c.length == c0.length && d.history().position() == pos + 1 &&
               mid.length == static_cast<Tick>(std::llround(1.5 * 90.0 / 60.0 * s.ppq)),
           name);
    d.undo();
    expect(d.song().playlist[0] == c0 && d.song().tempo == 120.0, "undo of the drag: the clip as it was");
    // Tempo automation added later: the clip's length follows the tempo map.
    auto td = describeParam(s, songParam(SongParam::Tempo));
    AutomationClip a;
    a.id = 900, a.owner = kSongId, a.index = static_cast<uint32_t>(SongParam::Tempo);
    a.points = {{0, static_cast<float>(td->toNormalized(120)), 0, AutoShape::Hold},
                {static_cast<Tick>(s.ppq), static_cast<float>(td->toNormalized(60)), 0, AutoShape::Hold}};
    Clip ac;
    ac.id = 901, ac.kind = ClipKind::Automation, ac.source = a.id, ac.track = 2, ac.length = 8 * s.ppq;
    d.editSong("tempo automation", [&](Song& x) {
      x.automations.push_back(a);
      x.playlist.push_back(ac);
    });
    TempoMap m;
    m.build(d.song());
    const Clip& ca = d.song().playlist[0];
    // 1 beat at 120 (0.5 s) + 1 s at 60 BPM (1 beat) = 2 beats.
    std::snprintf(name, sizeof name, "tempo automation 120 -> 60 at beat 1: the cut clip spans its 1.5 s on the map (%lld ticks, ends at %.4f s)",
                  static_cast<long long>(ca.length), m.secondsAt(static_cast<double>(ca.length)));
    expect(ca.length == 2 * s.ppq && std::fabs(ca.audioEnd - 1.5) < 1e-9, name);
  }

  // ---------------------------------------------------------------- save / load, old projects
  {
    Song s = lockedLoop(loop, 1);
    Song back;
    expect(songFromJson(songToJson(s), back) && back.playlist[0].tempoLock && back.playlist[0].syncBpm == s.playlist[0].syncBpm,
           "save / load: the lock is kept");
    Song old = clipSong(loop, 0, 170.0);
    old.playlist[0].stretch = 1.5;  // stretched before J-193: not locked
    old.playlist[0].length = old.playlist[0].length * 3 / 2;
    Song o2 = clipSong(loop, 0, 170.0);
    o2.playlist[0].syncBpm = 150.0;  // synced before J-193: keeps its pitch rule
    Song l1, l2;
    expect(songFromJson(songToJson(old), l1) && l1.playlist[0] == old.playlist[0] && !l1.playlist[0].tempoLock,
           "an old stretched clip opens unchanged (not locked)");
    expect(songFromJson(songToJson(o2), l2) && l2.playlist[0] == o2.playlist[0] && !l2.playlist[0].tempoLock,
           "an old synced clip opens unchanged (no lock)");
    Document d(l1);
    expect(d.song().playlist[0] == old.playlist[0], "opening does not convert clips");
  }

  // ---------------------------------------------------------------- FL export / import
  {
    Song g = clipSong(loop, 1, 170.0);
    g.playlist[0].stretch = 1.5;
    g.playlist[0].length = g.playlist[0].length * 3 / 2;
    flp::ExportOptions opt;
    opt.useOrigin = false;
    opt.projectDir = fs::temp_directory_path().string();
    std::vector<uint8_t> bytes;
    flp::ExportReport rep;
    Song in;
    flp::Report ir;
    std::string err;
    bool ok = flp::exportFlp(g, bytes, rep, opt) && importFlpBytes(bytes, in, ir, &err, "", opt.projectDir, false);
    const Clip* ic = nullptr;
    for (const Clip& x : in.playlist)
      if (x.kind == ClipKind::Audio) ic = &x;
    std::snprintf(name, sizeof name, "FL import of a stretched clip: as before (stretch %.3f, not locked)", ic ? ic->stretch : 0.0);
    expect(ok && ic && std::fabs(ic->stretch - 1.5) < 1e-9 && ic->syncBpm == 0 && !ic->tempoLock, name);
    // A locked clip at another tempo: written as the stretch it plays at the song tempo.
    Song l = lockedLoop(loop, 1);
    Document d(l);
    setTempo(d, 140.0);
    const Song h = d.song();
    bytes.clear();
    Song in2;
    flp::ExportReport rep2;
    flp::Report ir2;
    ok = flp::exportFlp(h, bytes, rep2, opt) && importFlpBytes(bytes, in2, ir2, &err, "", opt.projectDir, false);
    ic = nullptr;
    for (const Clip& x : in2.playlist)
      if (x.kind == ClipKind::Audio) ic = &x;
    const double hs = sourceAtEnd(h, h.playlist[0]);
    std::snprintf(name, sizeof name, "FL export of a locked clip at 140 BPM: it plays the same span in FL (%.4f vs %.4f s)", ic ? sourceAtEnd(in2, *ic) : -1.0,
                  hs);
    expect(ok && ic && std::fabs(sourceAtEnd(in2, *ic) - hs) < 0.01, name);
  }

  return finish();
}
