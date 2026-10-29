// Gridline — J-164: a playlist audio clip's pitch never changes its length or
// how its audio fills it.
//  * every time mode (Resample, Stretch, Rubber Band), pitch up and down,
//    after a stretch resize: the clip keeps its length, the audio still runs
//    from the clip start to exactly the clip end (no silent gap, nothing cut
//    off), in tune (Resample: the stretch still resamples, the pitch is on top);
//  * tempo-synced, trimmed and reversed clips, multi-clip pitch edits (one undo step);
//  * time mode switches keep the clip's length; "Time" x2 doubles it;
//  * the waveform's mapping (clipPlaybackSpeed) matches the engine's;
//  * the rendered clip and the realtime path agree on the channel pitch;
//  * FL projects keep FL's timing (the pitch there sets the speed), save / load.
#include <cmath>
#include <cstdio>
#include <complex>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "../src/core/ClipRate.h"
#include "../src/core/Document.h"
#include "../src/core/MenuParity.h"
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
#include "../src/engine/TimeStretch.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSrcSec = 2.0;   // source length
constexpr double kMarkSec = 0.05; // loud end marker

// A 220 Hz tone at 0.2 whose last 50 ms are loud (0.9): where the clip's audio ends is audible.
std::string writeSource() {
  const size_t n = static_cast<size_t>(kSrcSec * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double amp = t >= kSrcSec - kMarkSec ? 0.9 : 0.2;
    b.at(0, i) = b.at(1, i) = static_cast<float>(amp * std::sin(2 * kPi * 220 * t));
  }
  const std::string path = (fs::temp_directory_path() / "gl_j164_src.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  sampleCache().clear();
  return path;
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
// Strongest partial within +-6 % of `near` in x[a, a + 16384), interpolated.
double peakHz(const std::vector<float>& x, size_t a, double near) {
  const size_t N = 16384;
  if (a + N > x.size()) return 0;
  std::vector<cd> b(N);
  for (size_t i = 0; i < N; ++i) {
    const double w = 2 * kPi * static_cast<double>(i) / static_cast<double>(N);
    b[i] = static_cast<double>(x[a + i]) * (0.35875 - 0.48829 * std::cos(w) + 0.14128 * std::cos(2 * w) - 0.01168 * std::cos(3 * w));
  }
  fft(b);
  const size_t k0 = static_cast<size_t>(near / 1.06 * N / kSr), k1 = static_cast<size_t>(near * 1.06 * N / kSr);
  size_t best = k0;
  for (size_t k = k0; k <= k1; ++k)
    if (std::abs(b[k]) > std::abs(b[best])) best = k;
  const double ym = std::log(std::abs(b[best - 1])), y0 = std::log(std::abs(b[best])), yp = std::log(std::abs(b[best + 1]));
  return (static_cast<double>(best) + 0.5 * (ym - yp) / (ym - 2 * y0 + yp)) * kSr / static_cast<double>(N);
}
double cents(double f, double ref) { return 1200 * std::log2(f / ref); }
double rms(const std::vector<float>& x, double fromSec, double toSec) {
  const size_t a = static_cast<size_t>(std::max(0.0, fromSec) * kSr), b = std::min(x.size(), static_cast<size_t>(toSec * kSr));
  double e = 0;
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0.0;
}

// One sampler channel in time mode `mode` holding the source; one audio clip
// of its natural length (one bar at 120 BPM) at the song start.
Song clipSong(const std::string& path, int mode) {
  Song s = makeEmptySong();
  s.tempo = 120;
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
  c.start = 0;
  c.length = audioClipNaturalLength(s, c);
  s.playlist.push_back(c);
  return s;
}

double secondsOf(const Song& s, Tick t) {
  TempoMap m;
  m.build(s);
  return m.secondsAt(static_cast<double>(t));
}
double sourceAtEnd(const Song& s, const Clip& c) {
  TempoMap m;
  m.build(s);
  return clipSourceAt(s, c, m, static_cast<double>(c.start + s.clipLength(c)));
}
std::vector<float> render(const Song& s) {
  RenderOptions o;
  AudioBuffer b = renderSong(s, o);
  return std::vector<float>(b.plane(0), b.plane(0) + b.frames);
}

// The clip's audio reaches its loud end marker right at the clip end (no
// gap: the marker is not earlier; no cut: it is there at all).
bool fillsClip(const std::vector<float>& y, double endSec, double tf, std::string& why) {
  const double mark = kMarkSec * tf;  // the marker's length on the timeline
  const double body = rms(y, 0.3, 0.8);
  const double atEnd = rms(y, endSec - mark * 0.8, endSec - mark * 0.2);
  const double before = rms(y, endSec - mark * 6, endSec - mark * 3);
  char b[160];
  std::snprintf(b, sizeof b, " (body %.3f, end %.3f, before end %.3f)", body, atEnd, before);
  why = b;
  return body > 0.02 && atEnd > 2.5 * body && before < 1.8 * body;
}

}  // namespace

int main() {
  const std::string path = writeSource();

  // ---------------------------------------------------------------- every mode, pitch up / down, after a stretch resize
  for (int mode : {0, 1, 7}) {
    const char* mname = mode == 0 ? "Resample" : mode == 1 ? "Stretch" : "Rubber Band";
    for (float pitch : {700.0f, -700.0f, 1200.0f, -500.0f}) {
      Song s = clipSong(path, mode);
      const ObjectId id = s.playlist[0].id;
      const Tick natural = s.playlist[0].length;
      // Stretch-resize to two bars (the playlist's stretch edit; the time mode stays as set here).
      TempoMap tm;
      tm.build(s);
      s.playlist = pltools::resizeEnds(s, s.playlist, {id}, natural, true, &tm, false);
      const Tick len = s.clip(id)->length;
      const double tf = s.clip(id)->stretch;
      Document d(s);
      ops::pitchAudioClips(d, {id}, pitch);
      const Song& p = d.song();
      const Clip& c = *p.clip(id);
      char name[200];
      std::snprintf(name, sizeof name, "%s, stretched x%.1f, pitch %+.0f ct: the clip keeps its length (%lld -> %lld ticks)", mname, tf,
                    static_cast<double>(pitch), static_cast<long long>(len), static_cast<long long>(c.length));
      expect(len == 2 * natural && c.length == len && c.stretch == tf, name);
      const double srcEnd = sourceAtEnd(p, c);
      std::snprintf(name, sizeof name, "%s, stretched, pitch %+.0f ct: the source maps exactly onto the clip (ends at %.4f s of %.1f)", mname,
                    static_cast<double>(pitch), srcEnd, kSrcSec);
      expect(std::fabs(srcEnd - kSrcSec) < 1e-3, name);
      std::snprintf(name, sizeof name, "%s, stretched, pitch %+.0f ct: the clip's waveform maps like the engine", mname, static_cast<double>(pitch));
      // (the waveform draws source second audioStart + u * speed at playback second u)
      const double drawnEnd = c.audioStart + secondsOf(p, c.start + c.length) * clipPlaybackSpeed(*p.channel(c.source), c);
      expect(std::fabs(drawnEnd - srcEnd) < 1e-6, name);
      const std::vector<float> y = render(p);
      std::string why;
      const bool fills = fillsClip(y, secondsOf(p, c.start + c.length), tf, why);
      std::snprintf(name, sizeof name, "%s, stretched, pitch %+.0f ct: audio to the clip end, no gap, nothing cut off%s", mname,
                    static_cast<double>(pitch), why.c_str());
      expect(fills, name);
      // In tune: the pitch on top of the stretch (Resample: the stretch also resamples).
      const double want = 220.0 * std::pow(2.0, pitch / 1200.0) / (mode == 0 ? tf : 1.0);
      const double got = peakHz(y, static_cast<size_t>(0.5 * kSr), want);
      std::snprintf(name, sizeof name, "%s, stretched, pitch %+.0f ct: in tune (%.2f Hz, want %.2f)", mname, static_cast<double>(pitch), got, want);
      expect(std::fabs(cents(got, want)) < 5.0, name);
      d.undo();
      expect(d.song().clip(id)->pitch == 0.0f && d.song().clip(id)->length == len, "pitch edit: one undo step back");
    }
  }

  // ---------------------------------------------------------------- unstretched resample clip: same length and fill
  {
    Song s = clipSong(path, 0);
    Clip& c = s.playlist[0];
    const Tick len = c.length;
    for (float pitch : {1200.0f, -1200.0f}) {
      c.pitch = pitch;
      std::string why;
      const bool fills = fillsClip(render(s), secondsOf(s, c.length), 1.0, why);
      char name[160];
      std::snprintf(name, sizeof name, "Resample, natural length, pitch %+.0f ct: audio fills the clip%s", static_cast<double>(pitch), why.c_str());
      expect(c.length == len && std::fabs(sourceAtEnd(s, c) - kSrcSec) < 1e-3 && fills, name);
    }
  }

  // ---------------------------------------------------------------- multi-clip relative pitch edits
  {
    Song s = clipSong(path, 0);
    Clip b = s.playlist[0];
    b.id = s.allocateId();
    b.start = b.length;
    b.pitch = -300;
    s.playlist.push_back(b);
    const ObjectId a = s.playlist[0].id;
    const std::vector<Clip> before = s.playlist;
    Document d(s);
    ops::pitchAudioClips(d, {a, b.id}, 500.0f);
    bool same = true;
    for (const Clip& x : before) {
      const Clip* y = d.song().clip(x.id);
      same = same && y && y->length == x.length && y->start == x.start && std::fabs(sourceAtEnd(d.song(), *y) - sourceAtEnd(s, x)) < 1e-9;
    }
    expect(same && d.song().clip(b.id)->pitch == 200.0f, "several clips pitched together: lengths and timing unchanged");
    d.undo();
    expect(d.song().playlist == before, "several clips pitched together: one undo step");
  }

  // ---------------------------------------------------------------- tempo-synced clip across a tempo change
  {
    Song s = clipSong(path, 1);
    Clip& c = s.playlist[0];
    c.syncBpm = 120;
    s.tempo = 100;  // the clip follows: one bar at 100 BPM, the whole source
    const Tick len = c.length;
    c.pitch = 700;
    expect(c.length == len && std::fabs(sourceAtEnd(s, c) - kSrcSec) < 1e-3, "tempo-synced clip: pitch keeps the tempo fit");
    std::string why;
    const bool fills = fillsClip(render(s), secondsOf(s, c.length), 1.2, why);
    expect(fills, ("tempo-synced clip at another tempo, pitched: audio to the clip end" + why).c_str());
    s.channels[0].plugin.params[pp::SmpStretchMode] = 0;  // the time mode does not matter while it follows the tempo
    expect(std::fabs(sourceAtEnd(s, c) - kSrcSec) < 1e-3, "tempo-synced clip in Resample mode: pitch keeps the tempo fit");
  }

  // ---------------------------------------------------------------- trimmed, faded, reversed, slipped
  {
    Song s = clipSong(path, 0);
    Clip& c = s.playlist[0];
    c.audioStart = 0.4, c.audioEnd = 1.6;
    c.reverse = true;
    c.fadeIn = 0.05, c.fadeOut = 0.1, c.fadeInManual = c.fadeOutManual = true;
    c.stretch = 1.5;
    c.length = audioClipNaturalLength(s, c);
    const Tick len = c.length;
    TempoMap tm;
    tm.build(s);
    const double mid0 = clipSourceAt(s, c, tm, static_cast<double>(len) / 2);
    c.pitch = -900;
    const double mid1 = clipSourceAt(s, c, tm, static_cast<double>(len) / 2);
    expect(c.length == len && std::fabs(sourceAtEnd(s, c) - 1.6) < 2e-3 /* half a tick */ && std::fabs(mid0 - mid1) < 1e-9,
           "trimmed, faded, reversed, stretched clip: pitch changes neither the window nor its timing");
    // A split after the pitch edit continues where the left part stops.
    Song t = s;
    const ObjectId r = splitClipAt(t, c.id, len / 2);
    expect(r != kNoId && std::fabs(t.clip(r)->audioStart - mid1) < 1e-9, "split of a pitched clip: the right part continues the audio");
  }

  // ---------------------------------------------------------------- time mode switches and the Time factor keep the clip
  {
    Song s = clipSong(path, 1);
    const ObjectId id = s.playlist[0].id;
    s.clip(id)->pitch = 700;
    s.clip(id)->length -= s.clip(id)->length / 4;  // trimmed by its length
    const Tick len = s.clip(id)->length;
    setClipStretchMode(s, id, 1.0, 0);
    expect(s.clip(id)->length == len, "time mode Stretch -> Resample on a pitched, trimmed clip: length kept");
    setClipStretchMode(s, id, 1.0, 7);
    expect(s.clip(id)->length == len, "time mode Resample -> Rubber Band: length kept");
    setClipStretch(s, id, 2.0, false);
    expect(s.clip(id)->length == 2 * len, "Time x2: the clip doubles (trim kept)");
  }

  // ---------------------------------------------------------------- render and realtime agree on the channel pitch
  {
    Song s = clipSong(path, 0);
    s.channels[0].pitch = 3;  // channel pitch knob, semitones, on top of a resampled stretch
    Clip& c = s.playlist[0];
    c.stretch = 2.0;
    c.length *= 2;
    const std::vector<float> y = render(s);
    const double want = 220.0 / 2.0 * std::pow(2.0, 3 / 12.0);
    const double got = peakHz(y, static_cast<size_t>(0.5 * kSr), want);
    char name[160];
    std::snprintf(name, sizeof name, "Resample stretched x2 with the channel pitch +3 st: rendered in tune (%.2f Hz, want %.2f)", got, want);
    expect(std::fabs(cents(got, want)) < 5.0, name);
  }

  // ---------------------------------------------------------------- FL timing, save / load
  {
    // An FL project's resample clip: its pitch sets its speed (speedPitch = the pitch).
    Song s = clipSong(path, 0);
    Clip& c = s.playlist[0];
    c.pitch = 1200;
    c.speedPitch = 1200;
    c.length /= 2;
    expect(std::fabs(sourceAtEnd(s, c) - kSrcSec) < 1e-3, "FL resample clip: the pitch sets the speed (FL's timing)");
    const double before = sourceAtEnd(s, c);
    c.pitch = 1000;  // a Gridline edit afterwards moves only the pitch
    expect(std::fabs(sourceAtEnd(s, c) - before) < 1e-9, "FL resample clip pitched in Gridline: timing kept");
    Song back;
    expect(songFromJson(songToJson(s), back) && back.playlist[0].speedPitch == 1200.0f && back.playlist[0].pitch == 1000.0f,
           "save / load: the clip's speed pitch is kept");
    Song old;
    Json j = songToJson(clipSong(path, 0));
    expect(songFromJson(j, old) && old.playlist[0].speedPitch == 0.0f, "projects without it load with no speed pitch");

    // FL export and import: FL's pitch comes back as the speed pitch.
    Song g = clipSong(path, 0);
    g.playlist[0].pitch = 500;
    g.channels[0].plugin.params[pp::SmpPitchShift] = 100;
    flp::ExportOptions opt;
    opt.useOrigin = false;
    opt.projectDir = fs::temp_directory_path().string();
    std::vector<uint8_t> bytes;
    flp::ExportReport rep;
    Song in;
    flp::Report ir;
    std::string err;
    const bool ok = flp::exportFlp(g, bytes, rep, opt) && importFlpBytes(bytes, in, ir, &err, "", opt.projectDir, false);
    const Clip* ic = nullptr;
    for (const Clip& x : in.playlist)
      if (x.kind == ClipKind::Audio) ic = &x;
    expect(ok && ic && std::fabs(ic->pitch - 500.0f) < 0.5f && std::fabs(ic->speedPitch - 600.0f) < 0.5f,
           "FL import: a resample clip's whole pitch (channel + clip) sets its speed, as in FL");
    bool reported = false;
    for (const auto& [k, v] : rep.counts) reported = reported || (k.find("speed") != std::string::npos && v > 0);
    expect(reported, "FL export: a resample clip pitched apart from its speed is reported");
  }

  return finish();
}
