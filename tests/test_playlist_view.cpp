// Gridline — playlist view (J-28): every tool's effect on synthetic songs,
// audio clip move / resize / trim / slip / slice with undo and sample-accurate
// source offsets, and the default workspace layout for every supported size.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include "../src/core/Document.h"
#include "../src/core/Fades.h"
#include "../src/core/MixerLayout.h"
#include "../src/core/Project.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/SmoothSnap.h"
#include "../src/core/Song.h"
#include "../src/core/WavWriter.h"
#include "../src/core/Workspace.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/PlaylistTools.h"
#include "../src/engine/Render.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

// 2 s, 44.1 kHz stereo sine: at 120 BPM / 96 PPQ that is 4 beats = 384 ticks.
std::string writeTone() {
  const std::string path = (fs::temp_directory_path() / "gridline_j28_tone.wav").string();
  AudioBuffer b(2, 88200);
  for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = 0.5f * std::sin(static_cast<float>(i) * 0.0313f);
  writeWav(path, b, 44100, 16);
  return path;
}

struct Fixture {
  Song s;
  ObjectId pat = kNoId, chan = kNoId, autoId = kNoId;
  ObjectId pClip = kNoId, aClip = kNoId, mClip = kNoId;
};

Fixture makeFixture(const std::string& wav) {
  Fixture f;
  f.s = makeEmptySong();
  f.s.tempo = 120.0;
  f.pat = f.s.patterns[0].id;
  Pattern& p = f.s.patterns[0];
  Note n;
  n.start = 0, n.length = 96, n.pitch = 60;
  p.notes[1].push_back(n);  // content: one beat of notes, pattern length 1 bar
  Channel ch;
  ch.id = f.s.allocateId();
  ch.name = "tone";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = f.s.masterId();
  f.s.channels.push_back(ch);
  f.chan = ch.id;
  AutomationClip ac;
  ac.id = f.s.allocateId();
  ac.name = "auto";
  ac.points = {{0, 0.0f, 0.0f, AutoShape::Curve}, {384, 1.0f, 0.0f, AutoShape::Curve}};
  f.s.automations.push_back(ac);
  f.autoId = ac.id;
  Clip c;
  c.id = f.s.allocateId(), c.kind = ClipKind::Pattern, c.pattern = f.pat, c.track = 0, c.start = 0;
  f.s.playlist.push_back(c);
  f.pClip = c.id;
  Clip a;
  a.id = f.s.allocateId(), a.kind = ClipKind::Audio, a.source = f.chan, a.track = 1, a.start = 384;
  a.length = audioClipNaturalLength(f.s, a);
  f.s.playlist.push_back(a);
  f.aClip = a.id;
  Clip m;
  m.id = f.s.allocateId(), m.kind = ClipKind::Automation, m.source = f.autoId, m.track = 2, m.start = 0;
  f.s.playlist.push_back(m);
  f.mClip = m.id;
  return f;
}

const Clip* clipIn(const std::vector<Clip>& v, ObjectId id) {
  for (const auto& c : v)
    if (c.id == id) return &c;
  return nullptr;
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

#include "playlist_view_j38.inc"  // J-38: fades, automatic crossfades, thin mixer layout

}  // namespace

int main() {
  const std::string wav = writeTone();
  Fixture f = makeFixture(wav);
  TempoMap tm;
  tm.build(f.s);
  const Tick bar = f.s.barTicks(), beat = f.s.beatTicks();
  expect(bar == 384 && beat == 96, "fixture: 96 PPQ 4/4");
  expect(f.s.clip(f.aClip)->length == 384, "audio clip: natural length 2 s = 4 beats at 120 BPM");

  // ---- root cause of "audio clips can't be moved / resized": Document::editClips
  //      refused every edit of a playlist holding a non-pattern clip ----
  {
    Document d(f.s);
    const auto moved = pltools::moveClips(d.song().playlist, {f.aClip, f.mClip, f.pClip}, bar, 1, static_cast<int>(d.song().tracks.size()));
    expect(d.editClips("move clips", [&](std::vector<Clip>& cs) { cs = moved; }), "editClips accepts audio + automation clips");
    expect(d.song().clip(f.aClip)->start == 2 * bar && d.song().clip(f.aClip)->track == 2, "audio clip moved a bar and a track");
    expect(d.song().clip(f.mClip)->start == bar && d.song().clip(f.mClip)->track == 3, "automation clip moved");
    expect(d.song().clip(f.pClip)->start == bar && d.song().clip(f.pClip)->track == 1, "pattern clip moved alongside");
    d.undo();
    expect(d.song().clip(f.aClip)->start == bar && d.song().clip(f.aClip)->track == 1, "undo restores the audio clip");
    // A dangling clip (its channel gone) does not block edits of the others.
    Song dang = f.s;
    Clip z;
    z.id = dang.allocateId(), z.kind = ClipKind::Audio, z.source = 999999, z.track = 5, z.start = 0, z.length = 10;
    dang.playlist.push_back(z);
    Document d2(dang);
    const auto mv = pltools::moveClips(d2.song().playlist, {f.pClip}, beat, 0, 16);
    expect(d2.editClips("move", [&](std::vector<Clip>& cs) { cs = mv; }) && d2.song().clip(f.pClip)->start == beat,
           "untouched dangling clip doesn't block moving others");
  }

  // ---- move limits ----
  {
    auto v = pltools::moveClips(f.s.playlist, {f.aClip}, -10 * bar, -5, 16);
    expect(clipIn(v, f.aClip)->start == 0 && clipIn(v, f.aClip)->track == 0, "move clamps at song start and the first track");
    v = pltools::moveClips(f.s.playlist, {f.aClip}, 0, 40, 16);
    expect(clipIn(v, f.aClip)->track == 15, "move clamps at the last track");
    std::vector<char> locked(16, 0);
    locked[3] = 1;
    v = pltools::moveClips(f.s.playlist, {f.aClip}, 0, 2, 16, locked);
    expect(clipIn(v, f.aClip)->track == 1, "locked target track: nothing moves");
  }

  // ---- right edge (resize) ----
  {
    auto v = pltools::resizeEnds(f.s, f.s.playlist, {f.pClip}, bar, false);
    expect(f.s.clipLength(*clipIn(v, f.pClip)) == 2 * bar, "pattern clip: right edge lengthens (pattern repeats)");
    v = pltools::resizeEnds(f.s, f.s.playlist, {f.aClip}, -beat, false);
    expect(clipIn(v, f.aClip)->length == 3 * beat && near(clipIn(v, f.aClip)->audioStart, 0.0), "audio clip: right edge shortens, start untouched");
    v = pltools::resizeEnds(f.s, f.s.playlist, {f.aClip}, bar, false);
    expect(clipIn(v, f.aClip)->length == bar, "audio clip: right edge stops at the end of the audio");
    v = pltools::resizeEnds(f.s, f.s.playlist, {f.aClip}, bar, true);
    expect(clipIn(v, f.aClip)->length == 2 * bar && near(clipIn(v, f.aClip)->stretch, 2.0), "stretch mode: twice as long = stretch x2");
    Song st = f.s;
    *st.clip(f.aClip) = *clipIn(v, f.aClip);
    expect(audioClipNaturalLength(st, *st.clip(f.aClip)) == 2 * bar, "stretched clip's natural length matches its new length");
    v = pltools::resizeEnds(f.s, f.s.playlist, {f.aClip}, -10 * bar, false);
    expect(clipIn(v, f.aClip)->length == 1, "resize keeps at least one tick");
  }

  // ---- left edge (trim) ----
  {
    auto v = pltools::trimStarts(f.s, tm, f.s.playlist, {f.aClip}, beat, false);
    const Clip* c = clipIn(v, f.aClip);
    expect(c->start == bar + beat && c->length == 3 * beat, "audio trim: start later, end stays");
    expect(near(c->audioStart, 0.5) && std::llround(c->audioStart * 44100.0) == 22050, "audio trim: source start exactly 1 beat = 22050 frames");
    Song t = f.s;
    *t.clip(f.aClip) = *c;
    auto back = pltools::trimStarts(t, tm, t.playlist, {f.aClip}, -2 * beat, false);
    expect(clipIn(back, f.aClip)->start == bar && near(clipIn(back, f.aClip)->audioStart, 0.0), "audio trim back stops at the audio's beginning");
    auto odd = pltools::trimStarts(f.s, tm, f.s.playlist, {f.aClip}, 7, false);
    expect(near(clipIn(odd, f.aClip)->audioStart, 7.0 * 0.5 / 96.0), "audio trim by 7 ticks: sample-accurate seconds");
    auto zc = pltools::trimStarts(f.s, tm, f.s.playlist, {f.aClip}, 7, false, true);
    const double zs = clipIn(zc, f.aClip)->audioStart;
    expect(std::fabs(zs - 7.0 * 0.5 / 96.0) <= 0.0051, "zero-crossing trim stays within 5 ms");
    v = pltools::trimStarts(f.s, tm, f.s.playlist, {f.pClip}, beat, false);
    expect(clipIn(v, f.pClip)->start == beat && clipIn(v, f.pClip)->offset == beat && f.s.clipLength(*clipIn(v, f.pClip)) == 3 * beat,
           "pattern trim: offset follows the start");
    v = pltools::trimStarts(f.s, tm, f.s.playlist, {f.pClip}, -beat, false);
    expect(clipIn(v, f.pClip)->start == 0 && clipIn(v, f.pClip)->offset == 0, "pattern trim can't go before its content");
    v = pltools::trimStarts(f.s, tm, f.s.playlist, {f.aClip}, beat, true);
    expect(clipIn(v, f.aClip)->length == 3 * beat && near(clipIn(v, f.aClip)->stretch, 0.75) && near(clipIn(v, f.aClip)->audioStart, 0.0),
           "stretch trim: whole audio squeezed into the shorter clip");
    // Undo of a trim drag (several frames, one merged step).
    Document d(f.s);
    for (Tick k : {Tick(24), Tick(48), Tick(96)}) {
      auto w = pltools::trimStarts(d.song(), tm, f.s.playlist, {f.aClip}, k, false);
      d.editClips("trim clips", [&](std::vector<Clip>& cs) { cs = w; }, 0x1234);
    }
    d.endGesture();
    expect(near(d.song().clip(f.aClip)->audioStart, 0.5), "trim drag applied");
    d.undo();
    expect(near(d.song().clip(f.aClip)->audioStart, 0.0) && d.song().clip(f.aClip)->start == bar, "one undo step restores the trim drag");
  }

  // ---- slip ----
  {
    Clip c = *f.s.clip(f.pClip);
    Clip s1 = pltools::slipClip(f.s, tm, c, beat);
    expect(s1.offset == 3 * beat && s1.start == c.start, "pattern slip right: content wraps (offset = len - 1 beat)");
    expect(pltools::slipClip(f.s, tm, c, -beat).offset == beat, "pattern slip left");
    Clip a = *f.s.clip(f.aClip);
    a.audioStart = 0.5;
    Clip s2 = pltools::slipClip(f.s, tm, a, beat / 2);
    expect(near(s2.audioStart, 0.25) && s2.start == a.start && s2.length == a.length, "audio slip right shows earlier audio");
    expect(near(pltools::slipClip(f.s, tm, a, 4 * beat).audioStart, 0.0), "audio slip stops at the beginning");
    expect(near(pltools::slipClip(f.s, tm, a, -beat).audioStart, 1.0), "audio slip left");
    Clip m = *f.s.clip(f.mClip);
    expect(pltools::slipClip(f.s, tm, m, -beat).offset == beat && pltools::slipClip(f.s, tm, m, beat).offset == 0, "automation slip");
  }

  // ---- slice (all kinds, vertical line over tracks) ----
  {
    Document d(f.s);
    auto hits = pltools::clipsAcross(d.song(), bar + beat, 0, 2);
    expect(hits.size() == 1 && hits[0] == f.aClip, "slice line at bar 2 beat 2 crosses only the audio clip");
    std::vector<ObjectId> right;
    d.editSong("slice clips", [&](Song& x) { right = pltools::sliceClips(x, hits, bar + beat); });
    expect(right.size() == 1, "audio clip sliced");
    const Clip* l = d.song().clip(f.aClip);
    const Clip* r = right.empty() ? nullptr : d.song().clip(right[0]);
    expect(l && r && l->length == beat && r->start == bar + beat && r->length == 3 * beat, "slice: both parts keep their times");
    expect(r && near(r->audioStart, 0.5), "slice: right part starts 1 beat into the audio");
    auto across = pltools::clipsAcross(d.song(), beat * 2, 0, 15);
    expect(across.size() == 2, "slice line over every track crosses the pattern and automation clips");
    d.undo();
    expect(d.song().playlist.size() == 3 && d.song().clip(f.aClip)->length == bar, "undo removes the slice");
  }

  // ---- paint, delete, mute, clone ----
  {
    auto slots = pltools::paintSlots(0, 1000, bar);
    expect(slots.size() == 3 && slots[2] == 2 * bar, "paint trail: clips back to back towards the pointer");
    auto backSlots = pltools::paintSlots(2 * bar, 10, bar);
    expect(backSlots.size() == 3 && backSlots[0] == 0 && backSlots[2] == 2 * bar, "paint trail leftwards");
    expect(pltools::occupied(f.s, 1, bar + 10, 10) && !pltools::occupied(f.s, 1, 0, bar), "occupied test per track");
    Document d(f.s);
    d.editSong("paint clips", [&](Song& x) {
      for (Tick t : pltools::paintSlots(2 * bar, 5 * bar, bar)) {
        if (pltools::occupied(x, 0, t, bar)) continue;
        Clip c = pltools::makeClip(x, {ClipKind::Pattern, f.pat}, 0, t);
        c.id = x.allocateId();
        x.playlist.push_back(c);
      }
    });
    expect(d.song().playlist.size() == 7, "paint placed 4 pattern clips");
    d.editSong("paint audio", [&](Song& x) {
      Clip c = pltools::makeClip(x, {ClipKind::Audio, f.chan}, 4, 0);
      c.id = x.allocateId();
      x.playlist.push_back(c);
    });
    expect(d.song().playlist.back().kind == ClipKind::Audio && d.song().playlist.back().length == bar, "audio source places its natural length");
    expect(pltools::sourceLength(d.song(), {ClipKind::Automation, f.autoId}) == bar, "automation source length = its points");
    d.undo();
    d.undo();
    expect(d.song().playlist.size() == 3, "undo paint");
    // Delete tool: removeClip; mute tool: editClip mute.
    d.removeClip(f.aClip);
    expect(!d.song().clip(f.aClip), "delete tool removes an audio clip");
    d.undo();
    d.editClip(f.aClip, "mute clips", [](Clip& c) { c.muted = true; });
    expect(d.song().clip(f.aClip)->muted, "mute tool mutes an audio clip");
    std::vector<ObjectId> copies;
    d.editSong("clone", [&](Song& x) { copies = pltools::cloneClips(x, {f.aClip, f.mClip}); });
    expect(copies.size() == 2 && d.song().clip(copies[0])->kind == ClipKind::Audio && d.song().clip(copies[0])->start == bar, "clone keeps kind and place");
    const auto mv = pltools::moveClips(d.song().playlist, {copies.begin(), copies.end()}, bar, 0, 16);
    expect(d.editClips("move copies", [&](std::vector<Clip>& cs) { cs = mv; }) && d.song().clip(copies[0])->start == 2 * bar &&
               d.song().clip(f.aClip)->start == bar,
           "Shift+drag: copies move, originals stay");
  }

  // ---- tracks ----
  {
    Song t = f.s;
    t.tracks[1].name = "Audio";
    expect(pltools::moveTrack(t, 1, 4) && t.tracks[4].name == "Audio" && t.clip(f.aClip)->track == 4 && t.clip(f.mClip)->track == 1,
           "drag a track header down: clips follow, others shift up");
    expect(pltools::moveTrack(t, 4, 0) && t.tracks[0].name == "Audio" && t.clip(f.aClip)->track == 0 && t.clip(f.pClip)->track == 1,
           "drag a track header up");
  }

  // ---- default workspace layout ----
  {
    bool allOk = true, rackOk = true, mixerOk = true, playOk = true;
    const int sizes[][2] = {{1280, 720}, {1366, 768}, {1440, 900}, {1600, 900}, {1920, 1080}, {2560, 1440}};
    for (const auto& sz : sizes)
      for (float scale : {1.0f, 1.5f}) {
        // Logical workspace like main.cpp: scale limited so it stays >= 960 x 600; minus top and status bars.
        const float sc = std::max(1.0f, std::min({scale, sz[0] / 960.0f, sz[1] / 600.0f}));
        const float w = sz[0] / sc - 8, h = sz[1] / sc - 52 - 22 - 8;
        for (int channels : {1, 5, 12, 40}) {
          const auto L = workspace::defaultLayout(w, h, channels);
          const bool noOverlap = !L.browser.overlaps(L.playlist) && !L.playlist.overlaps(L.mixer) && !L.browser.overlaps(L.mixer);
          const bool tiles = std::fabs(L.browser.w + L.playlist.w + L.mixer.w - w) < 1.0f;
          allOk = allOk && noOverlap && tiles && L.area.contains(L.browser) && L.area.contains(L.mixer) && L.area.contains(L.playlist);
          rackOk = rackOk && L.area.contains(L.rack) && L.playlist.contains(L.rack) && !L.rack.overlaps(L.mixer) &&
                   (L.rack.x - L.playlist.x) <= (L.playlist.right() - L.rack.right()) + 1.0f && L.rack.w >= 300;
          if (channels <= 5) rackOk = rackOk && L.rack.h >= workspace::rackHeightFor(channels) - 0.5f;
          mixerOk = mixerOk && L.mixer.w >= 330 && L.mixer.w <= w * 0.36f && L.mixer.x + L.mixer.w <= w + 0.5f;
          playOk = playOk && L.playlist.w >= w * 0.4f && L.playlist.w > L.mixer.w;
        }
      }
    expect(allOk, "layout: browser | playlist | mixer tile the dock space without overlaps (all sizes, scale 1 / 1.5)");
    expect(rackOk, "layout: floating rack fully in view over the playlist's left half, tall enough for its rows");
    expect(mixerOk, "layout: mixer is a thin right column (>= 330 px for compact strips)");
    expect(playOk, "layout: playlist is the largest panel");
  }

  // ---- J-37: smooth snapping (drawn position easing) ----
  {
    using smooth::Tracker;
    const Tracker<1>::Vec eps{0.001};  // a one-cell step of 1.0: settled within 0.1 %
    // Simulates a one-cell move (0 -> 1) at `fps`: time until settled, largest
    // value seen, value after one 30 Hz frame (33 ms).
    struct Run {
      double settle = -1, peak = 0, at100 = 0;
    };
    auto run = [&](double fps) {
      Tracker<1> tr;
      Run r;
      tr.seed(7, {0.0}, 0.0);
      const double dt = 1.0 / fps;
      for (int k = 1; k <= static_cast<int>(fps); ++k) {
        const double now = k * dt;
        const double v = tr.follow(7, {1.0}, now, true, eps)[0];
        r.peak = std::max(r.peak, v);
        if (std::fabs(now - 1.0 / 30.0) < 1e-9) r.at100 = v;
        if (v == 1.0 && r.settle < 0) r.settle = now;
      }
      return r;
    };
    const double fpsList[] = {30, 60, 120, 240};
    Run runs[4];
    for (int i = 0; i < 4; ++i) runs[i] = run(fpsList[i]);
    // Analytic settle time to 0.1 %: tau * ln(1000).
    const double analytic = smooth::settleTime(smooth::kTau, 0.001);
    bool sameSettle = true, sameAt100 = true, noOvershoot = true;
    for (int i = 0; i < 4; ++i) {
      sameSettle = sameSettle && runs[i].settle >= analytic - 1e-9 && runs[i].settle <= analytic + 1.0 / fpsList[i] + 1e-9;
      sameAt100 = sameAt100 && std::fabs(runs[i].at100 - runs[3].at100) < 1e-9;
      noOvershoot = noOvershoot && runs[i].peak <= 1.0;
    }
    expect(sameSettle, "smooth snap: settles at the same time at 30/60/120/240 fps (within one frame)");
    expect(sameAt100, "smooth snap: frame-rate independent (same drawn value after 33 ms at every rate)");
    expect(noOvershoot, "smooth snap: no overshoot past the target cell");
    const double settle1 = smooth::settleTime();
    expect(settle1 > 0.100 && settle1 < 0.180, "smooth snap: a one-cell step settles in 100-180 ms (J-188: visible)");
    {  // J-188: the first 60 Hz frame covers part of the step: visible, still moving at once (no lag feel)
      Tracker<1> tr;
      tr.seed(1, {0.0}, 0.0);
      const double v = tr.follow(1, {1.0}, 1.0 / 60.0, true, eps)[0];
      expect(v > 0.3 && v < 0.6, "smooth snap: the first 60 Hz frame covers 30-60 % of a cell");
    }
    {  // target changes mid-animation: continuous, monotonic, lands exactly
      Tracker<1> tr;
      tr.seed(1, {0.0}, 0.0);
      double prev = 0, t = 0;
      bool continuous = true, monotonic = true;
      for (int k = 0; k < 2; ++k) prev = tr.follow(1, {1.0}, t += 1.0 / 120, true, eps)[0];
      const double before = prev;
      for (int k = 0; k < 60; ++k) {
        const double v = tr.follow(1, {3.0}, t += 1.0 / 120, true, eps)[0];
        if (k == 0) continuous = v >= before && v - before < 2.0;  // no jump: eases from where it is drawn
        monotonic = monotonic && v >= prev && v <= 3.0;
        prev = v;
      }
      expect(continuous && monotonic && prev == 3.0, "smooth snap: a new target mid-glide continues from the drawn position and lands exactly");
      // ... and back the other way.
      double low = 3.0;
      for (int k = 0; k < 60; ++k) low = std::min(low, tr.follow(1, {2.0}, t += 1.0 / 120, true, eps)[0]);
      expect(low >= 2.0 && tr.follow(1, {2.0}, t += 1.0 / 120, true, eps)[0] == 2.0, "smooth snap: reversing never undershoots the new cell");
    }
    {  // disabled / free grid: identity
      Tracker<2> tr;
      tr.seed(3, {0.0, 0.0}, 0.0);
      const auto v = tr.follow(3, {5.0, -2.0}, 0.001, false, {0.001, 0.001});
      expect(v[0] == 5.0 && v[1] == -2.0, "smooth snap off: drawn position = logical position at once");
      const auto u = tr.follow(99, {4.0, 1.0}, 0.002, true, {0.001, 0.001});
      expect(u[0] == 4.0 && u[1] == 1.0, "smooth snap: objects that were never seeded draw at their logical position");
    }
    {  // bookkeeping: settled entries go once the drag ends, kept while it lasts
      Tracker<1> tr;
      tr.seed(1, {0.0}, 0.0);
      double t = 0;
      for (int k = 0; k < 30; ++k) tr.follow(1, {1.0}, t += 1.0 / 60, true, eps);
      tr.prune(t, true);
      const bool kept = tr.contains(1);
      tr.prune(t, false);
      expect(kept && tr.empty(), "smooth snap: settled glides are kept during the drag, dropped after it");
      tr.seed(2, {0.0}, t);
      tr.prune(t + 1.0, false);
      expect(tr.empty(), "smooth snap: glides of vanished objects are dropped");
      // Re-seeding a key that is still gliding keeps its drawn position.
      tr.seed(4, {0.0}, t);
      const double mid = tr.follow(4, {1.0}, t + 0.004, true, eps)[0];
      tr.seed(4, {1.0}, t + 0.004);
      const double again = tr.follow(4, {1.0}, t + 0.004, true, eps)[0];
      expect(mid > 0 && mid < 1 && again == mid, "smooth snap: a new drag continues from where a gliding clip is drawn");
    }
    {  // a whole snapped drag: every frame's drawn position stays between the
       // previous and the current cell, and the release lands on the commit
      Tracker<1> tr;
      tr.seed(1, {0.0}, 0.0);
      double t = 0, cell = 0;
      bool between = true;
      double drawnPrev = 0;
      for (int k = 0; k < 240; ++k) {
        if (k % 20 == 0 && k < 200) cell += 1.0;  // pointer crosses a cell every 20 frames (240 Hz)
        const double v = tr.follow(1, {cell}, t += 1.0 / 240, true, eps)[0];
        between = between && v >= drawnPrev && v <= cell;
        drawnPrev = v;
      }
      expect(between && drawnPrev == cell, "smooth snap: drawn clip trails the snapped cell monotonically and lands on it");
    }
  }

  crossfadeTests();
  mixerLayoutTests();
  fs::remove(wav);
  return finish();
}
