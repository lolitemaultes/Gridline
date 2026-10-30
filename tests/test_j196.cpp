// Gridline: J-196, clip edits that stay cheap while a pointer drags them.
//  * Document: a run of clip level edits (gain, fades, curves: one clip or a
//    selection) is known as such (clipLevelsOnlySince); any other change, an
//    undo included, ends it; a selection edit records only the clips it
//    changed and stays one undo step;
//  * ProgramBuilder::patchClipLevels writes those levels into the program it
//    built last, exactly as a new build would compile them (crossfades too),
//    without a build;
//  * a pitch drag (clip pitch, channel pitch) with renders deferred starts no
//    clip render and keeps every clip's realtime voice from build to build
//    (no new voice, no restart; a new timing, e.g. the time knob, restarts
//    the stream in the same voice); the release starts the renders and the
//    voices play on until they are ready;
//  * playing through a pitch knob turn has no clicks;
//  * Rubber Band clips preview through Gridline's stretch while editing (a
//    Rubber Band voice takes ~100 ms to prepare), a few new voices per build.
// Counts, not wall-clock time.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/ClipGain.h"
#include "../src/core/ClipMultiEdit.h"
#include "../src/core/Document.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Song.h"
#include "../src/core/WavWriter.h"
#include "../src/engine/Program.h"
#include "../src/engine/Sample.h"
#include "../src/engine/TimeStretch.h"
#include "Check.h"

namespace fs = std::filesystem;
using namespace gl;
using clipmulti::Param;

namespace {

constexpr double kSr = 48000;

std::string writeTone(const std::string& name, double seconds) {
  const size_t n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr;
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.4 * std::sin(2 * 3.141592653589793 * 220.0 * t) * (1.0 + 0.3 * std::sin(t * 5.0)));
  }
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 16);
  return path;
}

struct Fix {
  Song song;
  ObjectId chan = kNoId;
  std::vector<ObjectId> clips;
};

// `n` audio clips of one bar on tracks 0.. (two per track, the second one
// overlapping the first by a beat: an automatic crossfade).
Fix make(const std::string& wav, int n, double stretch, float stretchMode = 0.0f) {
  Fix f;
  f.song = makeEmptySong();
  Song& s = f.song;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "take";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.plugin.params[static_cast<size_t>(pp::SmpStretchMode)] = stretchMode;
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  f.chan = ch.id;
  const Tick bar = s.barTicks(), beat = s.beatTicks();
  for (int i = 0; i < n; ++i) {
    Clip k;
    k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = f.chan, k.track = i / 2;
    k.start = (i % 2) * (bar - beat), k.length = bar, k.stretch = stretch;
    k.gain = clipgain::fromDb(-3.0 * i);
    s.playlist.push_back(k);
    f.clips.push_back(k.id);
  }
  return f;
}

struct Lv {
  float gain;
  double fadeIn, fadeOut;
  FadeShape shapeIn, shapeOut;
  float tensionIn, tensionOut;
  uint8_t declickIn, declickOut;
  bool operator==(const Lv& o) const {
    return gain == o.gain && fadeIn == o.fadeIn && fadeOut == o.fadeOut && shapeIn == o.shapeIn && shapeOut == o.shapeOut &&
           tensionIn == o.tensionIn && tensionOut == o.tensionOut && declickIn == o.declickIn && declickOut == o.declickOut;
  }
};
std::map<ObjectId, Lv> levels(const Program& p) {
  std::map<ObjectId, Lv> out;
  for (const ProgChannel& pc : p.channels)
    for (const ProgAudioClip& c : pc.audio)
      out[c.clip] = Lv{c.gain, c.fadeIn, c.fadeOut, c.shapeIn, c.shapeOut, c.tensionIn, c.tensionOut, c.declickIn, c.declickOut};
  return out;
}

std::vector<const ProgAudioClip*> audioOf(const Program& p) {
  std::vector<const ProgAudioClip*> v;
  for (const ProgChannel& pc : p.channels)
    for (const ProgAudioClip& c : pc.audio) v.push_back(&c);
  return v;
}

std::shared_ptr<const Program> prepared(ProgramBuilder& b, const Song& s) {
  b.setSampleRate(kSr);
  b.setAsyncSamples(false);  // samples decoded, first renders made
  b.build(s);
  stretch::waitForPendingRenders();
  b.setAsyncSamples(true);  // as the app builds
  return b.build(s);
}

void documentRuns(const std::string& wav) {
  Fix f = make(wav, 4, 1.0);
  Document doc(f.song);
  const Song& s = doc.song();
  expect(!doc.clipLevelsOnlySince(doc.revision()), "nothing changed: not a level run");
  const uint64_t r0 = doc.revision();
  const auto gains = clipmulti::capture(s, f.clips, Param::GainDb, f.clips[0]);
  const size_t steps0 = doc.history().position();
  for (int k = 1; k <= 5; ++k) clipmulti::offset(doc, "clip volume", gains, 0.5 * k, 77);
  expect(doc.clipLevelsOnlySince(r0), "a selection volume drag is a run of level edits");
  expect(doc.history().position() == steps0 + 1, "... one undo step");
  const uint64_t r1 = doc.revision();
  clipmulti::offset(doc, "fade in", clipmulti::capture(s, f.clips, Param::FadeIn, f.clips[0]), 0.05, 78);
  clipmulti::offset(doc, "fade curve", clipmulti::capture(s, f.clips, Param::FadeOutCurve, f.clips[0]), 0.3, 79);
  doc.editClip(f.clips[2], "clip volume", [](Clip& x) { x.gain = 0.25f; }, 80);
  expect(doc.clipLevelsOnlySince(r0) && doc.clipLevelsOnlySince(r1), "fades, curves and a single clip's volume join the run");
  const uint64_t r2 = doc.revision();
  doc.editClip(f.clips[1], "clip pitch", [](Clip& x) { x.pitch = 50.0f; }, 81);
  expect(!doc.clipLevelsOnlySince(r0) && !doc.clipLevelsOnlySince(r2), "a pitch edit is not a level edit");
  const uint64_t r3 = doc.revision();
  clipmulti::offset(doc, "clip volume", clipmulti::capture(s, f.clips, Param::GainDb, f.clips[0]), 1.0, 82);
  expect(doc.clipLevelsOnlySince(r3) && !doc.clipLevelsOnlySince(r2), "a new run starts after it");
  clipmulti::offset(doc, "move", clipmulti::capture(s, f.clips, Param::Pitch, f.clips[0]), 10.0, 83);
  expect(!doc.clipLevelsOnlySince(r3), "a selection pitch edit ends the run");
  const uint64_t r4 = doc.revision();
  doc.editClip(f.clips[0], "clip volume", [](Clip& x) { x.gain = 0.5f; }, 84);
  doc.undo();
  expect(!doc.clipLevelsOnlySince(r4), "an undo is not a level edit");
  // The selection edit records the clips it changed: undo / redo restore exactly.
  Document d2(f.song);
  const std::vector<Clip> before = d2.song().playlist;
  const auto g2 = clipmulti::capture(d2.song(), {f.clips[1], f.clips[3]}, Param::GainDb, f.clips[1]);
  for (int k = 1; k <= 4; ++k) clipmulti::offset(d2, "clip volume", g2, -1.0 * k, 90);
  std::vector<Clip> after = d2.song().playlist;
  expect(!(after[1] == before[1]) && !(after[3] == before[3]) && after[0] == before[0] && after[2] == before[2],
         "only the two selected clips changed");
  d2.undo();
  expect(d2.song().playlist == before, "undo restores the playlist");
  d2.redo();
  expect(d2.song().playlist == after, "redo applies the merged drag");
  Clip a = before[0], b = a;
  b.gain = 2.0f, b.fadeIn = 0.1, b.fadeInManual = true, b.fadeOutTension = 0.4f, b.fadeOutShape = 1, b.fadeInDeclick = 2;
  expect(clipLevelsOnly(a, b), "clipLevelsOnly: gain, fades, curves, declick");
  b.start += 1;
  expect(!clipLevelsOnly(a, b), "clipLevelsOnly: not a move");
  b = a, b.pitch = 1.0f;
  expect(!clipLevelsOnly(a, b), "clipLevelsOnly: not a pitch");
}

void patchMatchesBuild(const std::string& wav) {
  Fix f = make(wav, 6, 1.0);
  Document doc(f.song);
  ProgramBuilder b;
  std::shared_ptr<const Program> prog = prepared(b, doc.song());
  const uint64_t builds0 = b.builds();
  const uint64_t rev = doc.revision();
  const Song& s = doc.song();
  clipmulti::offset(doc, "clip volume", clipmulti::capture(s, f.clips, Param::GainDb, f.clips[0]), 4.5, 1);
  clipmulti::offset(doc, "fade in", clipmulti::capture(s, f.clips, Param::FadeIn, f.clips[0]), 0.08, 2);
  clipmulti::offset(doc, "fade curve", clipmulti::capture(s, f.clips, Param::FadeInCurve, f.clips[0]), -0.4, 3);
  doc.editClip(f.clips[5], "declick", [](Clip& x) { x.fadeOutDeclick = 2, x.fadeOutShape = 1; });
  expect(doc.clipLevelsOnlySince(rev), "level edits only");
  expect(levels(*prog).size() == 6, "six clips compiled");
  expect(b.patchClipLevels(doc.song()), "patched into the playing program");
  expect(b.builds() == builds0 && b.patches() == 1, "... without a build");
  ProgramBuilder fresh;
  std::shared_ptr<const Program> ref = prepared(fresh, doc.song());
  expect(levels(*prog) == levels(*ref), "patched levels equal a new build's (crossfades, curves, declick)");
  bool moved = false;
  for (const auto& [id, lv] : levels(*prog)) moved = moved || lv.fadeIn > 0.05;
  expect(moved, "... and they did change");
  prog.reset();
  expect(!b.patchClipLevels(doc.song()), "no program to patch: build instead");
}

void pitchDragDefers(const std::string& wav) {
  Fix f = make(wav, 6, 1.37);
  Document doc(f.song);
  ProgramBuilder b;
  std::shared_ptr<const Program> prog = prepared(b, doc.song());
  std::vector<const ClipVoice*> v0;
  for (const ProgAudioClip* c : audioOf(*prog)) v0.push_back(c->rt.get());
  bool all = v0.size() == 6;
  for (const ClipVoice* v : v0) all = all && v;
  expect(all, "stretched clips with renders: a realtime voice each (as before)");
  const uint64_t made0 = b.voicesMade();
  const size_t bytes0 = stretch::clipCacheBytes();
  b.setDeferRenders(true);
  const auto snap = clipmulti::capture(doc.song(), f.clips, Param::Pitch, f.clips[0]);
  bool deferred = true, same = true, noHq = true;
  for (int k = 1; k <= 12; ++k) {
    clipmulti::offset(doc, "clip pitch", snap, 7.0 * k, 5);
    prog = b.build(doc.song());
    deferred = deferred && b.deferredRenders();
    const auto a = audioOf(*prog);
    for (size_t i = 0; i < a.size() && i < v0.size(); ++i) same = same && a[i]->rt.get() == v0[i], noHq = noHq && !a[i]->hq;
  }
  stretch::waitForPendingRenders();
  expect(deferred && noHq, "a pitch drag starts no render (12 builds)");
  expect(stretch::clipCacheBytes() == bytes0, "... the render cache did not grow");
  expect(same && b.voicesMade() == made0, "every clip keeps its realtime voice: no new voice, no restart");
  std::vector<uint32_t> gen0;
  for (const ProgAudioClip* c : audioOf(*prog)) gen0.push_back(c->rtGen);
  bool resync = false;
  for (uint32_t g : gen0) resync = resync || g != 0;
  expect(!resync, "... the streams go on where they are (pitch only)");
  // The channel's time knob: a new speed restarts each stream where the clip now is, in the same voice.
  for (int k = 1; k <= 5; ++k) {
    doc.editChannel(f.chan, "time", [k](Channel& x) { x.plugin.params[static_cast<size_t>(pp::SmpTimeStretch)] = 1.0f + 0.05f * static_cast<float>(k); }, 9);
    prog = b.build(doc.song());
  }
  same = true, resync = true;
  {
    const auto t = audioOf(*prog);
    for (size_t i = 0; i < t.size() && i < v0.size(); ++i) same = same && t[i]->rt.get() == v0[i], resync = resync && t[i]->rtGen == 5;
  }
  expect(same && b.voicesMade() == made0 && resync, "a time knob drag: the same voices, restarted at the new timing");
  b.setDeferRenders(false);  // the release
  prog = b.build(doc.song());
  bool hq = true;
  same = true;
  const auto a = audioOf(*prog);
  for (size_t i = 0; i < a.size() && i < v0.size(); ++i) hq = hq && a[i]->hq, same = same && a[i]->rt.get() == v0[i];
  expect(!b.deferredRenders() && hq, "the release starts the renders");
  expect(same, "... and the voices play on meanwhile");
  stretch::waitForPendingRenders();
  // A channel pitch knob: crossing zero (no voice needed there) keeps the voices for when it comes back.
  Fix g = make(wav, 4, 1.0);
  Document d2(g.song);
  ProgramBuilder b2;
  prog = prepared(b2, d2.song());
  b2.setDeferRenders(true);
  uint64_t made = 0;
  for (int k : {1, 2, 0, -1, 0, 2, 3}) {
    d2.editChannel(g.chan, "pitch", [k](Channel& x) { x.pitch = k; }, 6);
    prog = b2.build(d2.song());
    if (k == 1) made = b2.voicesMade();
  }
  expect(made == 4 && b2.voicesMade() == made, "a channel pitch knob: four voices, kept through zero");
  b2.setDeferRenders(false);
  prog = b2.build(d2.song());
  stretch::waitForPendingRenders();
}

void rubberBandPreview(const std::string& wav) {
  const float rb = static_cast<float>(stretch::Mode::RubberBand);
  Fix f = make(wav, 10, 1.0, rb);
  for (Clip& c : f.song.playlist) c.pitch = 100.0f;  // keeps the pitch: rendered
  Document doc(f.song);
  ProgramBuilder b;
  std::shared_ptr<const Program> prog = prepared(b, doc.song());
  bool none = true;
  for (const ProgAudioClip* c : audioOf(*prog)) none = none && !c->rt && c->hq && c->hq->ready.load();
  expect(none, "Rubber Band clips with ready renders: no realtime voice (as before)");
  b.setDeferRenders(true);
  const auto snap = clipmulti::capture(doc.song(), f.clips, Param::Pitch, f.clips[0]);
  std::vector<int> given;
  for (int k = 1; k <= 6; ++k) {
    clipmulti::offset(doc, "clip pitch", snap, 10.0 * k, 7);
    prog = b.build(doc.song());
    int n = 0;
    bool preview = true;
    for (const ProgAudioClip* c : audioOf(*prog))
      if (c->rt) ++n, preview = preview && c->engine == stretch::Engine::Stretch;
    given.push_back(n);
    expect(preview, "... previews play through Gridline's stretch while editing");
    if (!b.voicesPending()) break;
  }
  expect(given.size() == 3 && given[0] == 4 && given[1] == 8 && given[2] == 10, "four new voices per build until all ten have one");
  std::vector<const ClipVoice*> v;
  for (const ProgAudioClip* c : audioOf(*prog)) v.push_back(c->rt.get());
  b.setDeferRenders(false);
  prog = b.build(doc.song());
  bool kept = true;
  const auto a = audioOf(*prog);
  for (size_t i = 0; i < a.size() && i < v.size(); ++i)
    kept = kept && a[i]->rt.get() == v[i] && a[i]->engine == stretch::Engine::Stretch && a[i]->hq;
  expect(kept, "the release keeps the preview voices while the Rubber Band renders are made (no 100 ms stall per clip)");
  stretch::waitForPendingRenders();
  prog = b.build(doc.song());
  none = true;
  for (const ProgAudioClip* c : audioOf(*prog)) none = none && !c->rt && c->engine == stretch::Engine::RubberBand;
  expect(none, "renders ready: the voices are gone");
}

// Playing through a pitch knob turn: one clip played block by block, its
// pitch moved 2 cents and the program rebuilt (renders deferred) every two
// blocks, as the app does once per frame. No step in the output may stand
// out of the signal (a new voice per build restarted the stream: a click
// every frame, steps 60 times the signal's).
void playThroughPitchTurn(const std::string& wav) {
  Fix f = make(wav, 1, 1.0, static_cast<float>(stretch::kStretch));
  f.song.playlist[0].pitch = 100.0f, f.song.playlist[0].length = f.song.barTicks() * 2;
  Document doc(f.song);
  ProgramBuilder b;
  std::shared_ptr<const Program> prog = prepared(b, doc.song());
  const int block = 512;
  std::vector<float> L;
  int64_t clock = 0;
  for (int frame = 0; frame < 120; ++frame) {
    if (frame >= 30) {
      doc.editClip(f.clips[0], "clip pitch", [](Clip& x) { x.pitch += 2.0f; }, 11);
      b.setDeferRenders(true);
      prog = b.build(doc.song());
    }
    for (int k = 0; k < 2; ++k) {
      std::vector<float> l(block, 0.f), r(block, 0.f);
      renderAudioClips(prog->channels[0].audio, prog->tempoMap, kSr, 0, clock, 0, block, l.data(), r.data());
      L.insert(L.end(), l.begin(), l.end());
      clock += block;
    }
  }
  auto maxStep = [&](size_t a, size_t z) {
    double m = 0;
    for (size_t i = a + 1; i < z; ++i) m = std::max(m, static_cast<double>(std::fabs(L[i] - L[i - 1])));
    return m;
  };
  const size_t fb = 2 * block;
  const double steady = maxStep(5 * fb, 30 * fb), turning = maxStep(30 * fb, 120 * fb);
  std::printf("# steady largest step %.4f, while turning %.4f\n", steady, turning);
  expect(steady > 1e-3 && turning < 2.0 * steady, "turning the pitch while playing: no clicks");
  b.setDeferRenders(false);
  prog = b.build(doc.song());
  stretch::waitForPendingRenders();
}

}  // namespace

int main() {
  const std::string wav = writeTone("gridline_j196_tone.wav", 3.0);
  sampleCache().clear();
  documentRuns(wav);
  patchMatchesBuild(wav);
  pitchDragDefers(wav);
  rubberBandPreview(wav);
  playThroughPitchTurn(wav);
  stretch::shutdownClipRenderer();
  std::error_code ec;
  fs::remove(wav, ec);
  return finish();
}
