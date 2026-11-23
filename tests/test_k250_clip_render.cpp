// Gridline: K-250 / K-251, playlist clips rendered to audio one at a time (engine/ClipRender.h).
// Checks, on a song with an audio clip (through a mixer track with an effect), a pattern clip
// playing a synth, an automation clip on the synth's volume and a clip on another track:
//  * one clip's render is sample for sample the reference render of the same span: the song
//    rendered from its start with only that clip (and the automation) playing, cut to the span;
//  * the other clips are never in it, the automation is, a muted clip still renders;
//  * Render clip to audio (in place): the written file holds exactly that render, the new
//    clip takes the clip's track, start, length and mute, its channel goes to the master, and
//    the song then sounds as before over the clip's span; one undo step brings the clip back;
//  * Render each clip (K-251): every selected clip gets its own render, in place or each to its
//    own file (32-bit: exact; 24-bit: within the dither), never one merged file; automation
//    clips are left out; names repeat as "Name 2"; an existing file is never overwritten.
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/ClipRender.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace fs = std::filesystem;
namespace ed = gl::packs::edison;

namespace {

std::string gDir;
constexpr int kRate = 44100;

std::string writeWav(const std::string& name, double secs, const std::function<float(double)>& f) {
  ed::Audio a = ed::makeAudio(kRate, 2, static_cast<int64_t>(std::llround(secs * kRate)));
  for (int64_t i = 0; i < a.frames(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    a.L[static_cast<size_t>(i)] = f(t);
    a.R[static_cast<size_t>(i)] = 0.5f * f(t * 1.01);
  }
  const std::string p = gDir + "/" + name + ".wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

struct Fixture {
  Song s;
  ObjectId audioClip = kNoId, patClip = kNoId, otherClip = kNoId, autoClip = kNoId, synth = kNoId;
};

Fixture fixture() {
  Fixture f;
  Song& s = f.s;
  s = makeEmptySong();
  s.tempo = 120;
  const std::string tone = writeWav("tone", 3.0, [](double t) { return 0.3f * static_cast<float>(std::sin(2 * M_PI * 330 * t) * std::exp(-t)); });
  // An audio clip on track 1, through mixer track 1 with a gain on it.
  Channel smp;
  smp.id = s.allocateId();
  smp.name = "Tone";
  smp.plugin = defaultPluginState("gl.sampler");
  smp.plugin.data = tone;
  smp.mixerTrack = s.mixer.size() > 1 ? s.mixer[1].id : s.masterId();
  s.channels.push_back(smp);
  if (s.mixer.size() > 1) s.mixer[1].volume = 0.5f;
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Audio;
  ac.source = smp.id;
  ac.track = 1;
  ac.start = 2 * s.barTicks();
  ac.length = 2 * s.beatTicks();
  ac.audioStart = 0.25;
  ac.gain = 0.8f;
  ac.fadeIn = 0.01;
  s.playlist.push_back(ac);
  f.audioClip = ac.id;
  // A synth pattern clip on track 0.
  Channel syn;
  syn.id = s.allocateId();
  syn.name = "Lead";
  syn.plugin = defaultPluginState("gl.synth");
  syn.mixerTrack = s.masterId();
  s.channels.push_back(syn);
  f.synth = syn.id;
  Pattern p;
  p.id = s.allocateId();
  p.name = "Riff";
  for (int k = 0; k < 4; ++k) {
    Note n;
    n.start = k * s.beatTicks();
    n.length = s.beatTicks() / 2;
    n.pitch = 60 + 3 * k;
    p.notes[syn.id].push_back(n);
  }
  s.patterns.push_back(p);
  Clip pc;
  pc.id = s.allocateId();
  pc.pattern = p.id;
  pc.track = 0;
  pc.start = s.barTicks();
  pc.length = s.barTicks();
  s.playlist.push_back(pc);
  f.patClip = pc.id;
  // Another clip of the same pattern on track 2, overlapping both: never in their renders.
  Clip oc = pc;
  oc.id = s.allocateId();
  oc.track = 2;
  oc.start = s.barTicks() + s.beatTicks();
  oc.length = 3 * s.barTicks();
  s.playlist.push_back(oc);
  f.otherClip = oc.id;
  // Automation of the synth's volume over the song.
  AutomationClip au;
  au.id = s.allocateId();
  au.name = "Lead volume";
  au.owner = syn.id;
  au.index = static_cast<uint32_t>(ChannelParam::Volume);
  au.points = {AutoPoint{0, 0.9f, 0, AutoShape::Curve}, AutoPoint{4 * s.barTicks(), 0.2f, 0, AutoShape::Curve}};
  s.automations.push_back(au);
  Clip k;
  k.id = s.allocateId();
  k.kind = ClipKind::Automation;
  k.source = au.id;
  k.track = 3;
  k.start = 0;
  k.length = 4 * s.barTicks();
  s.playlist.push_back(k);
  f.autoClip = k.id;
  return f;
}

// The reference: the song from its start with only `clip` (and the automation) playing, the
// master neutral, cut to the clip's span by the engine's own frame count.
AudioBuffer reference(const Song& song, ObjectId clip) {
  Song r = stemSong(song, song.masterId(), false);
  r.playlist.erase(std::remove_if(r.playlist.begin(), r.playlist.end(), [&](const Clip& c) { return c.id != clip && c.kind != ClipKind::Automation; }),
                   r.playlist.end());
  for (auto& c : r.playlist) c.muted = false;
  const Clip* k = r.clip(clip);
  RenderJob j;
  j.sampleRate = kRate;
  j.scope = RenderScope::Range;
  j.start = 0;
  j.end = k->start + r.clipLength(*k);
  j.tail = TailMode::Cut;
  RenderResult full = renderAudio(r, j);
  const int64_t from = scopeFrames(r, j, 0, k->start, kRate);
  AudioBuffer out(2, full.audio.frames > static_cast<size_t>(from) ? full.audio.frames - static_cast<size_t>(from) : 0);
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < out.frames; ++i) out.at(c, i) = full.audio.at(c, static_cast<size_t>(from) + i);
  return out;
}

// The reference render of the same range: the song's own range export with the other clips
// muted (not taken out), the master neutral. Another path to the same sound: it must match exactly.
AudioBuffer rangeReference(const Song& song, ObjectId clip) {
  Song r = stemSong(song, song.masterId(), false);
  for (auto& c : r.playlist) c.muted = c.id != clip && c.kind != ClipKind::Automation;
  for (auto& t : r.tracks) t.muted = false;
  const Clip* k = r.clip(clip);
  RenderJob j;
  j.sampleRate = kRate;
  j.scope = RenderScope::Range;
  j.start = k->start;
  j.end = k->start + r.clipLength(*k);
  j.tail = TailMode::Cut;
  return renderAudio(r, j).audio;
}

double maxDiff(const AudioBuffer& a, const AudioBuffer& b, size_t& frames) {
  frames = std::min(a.frames, b.frames);
  double d = a.frames == b.frames ? 0.0 : 1.0;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < frames; ++i) d = std::max(d, static_cast<double>(std::fabs(a.at(c, i) - b.at(c, i))));
  return d;
}

double peakOf(const AudioBuffer& b) { return b.peak(); }

AudioBuffer fileAudio(const std::string& path) {
  SamplePtr sp = loadSample(path);
  AudioBuffer b;
  if (!sp) return b;
  b.resize(2, sp->frames());
  std::copy(sp->left.begin(), sp->left.end(), b.plane(0));
  std::copy(sp->right.begin(), sp->right.end(), b.plane(1));
  return b;
}

void testRenderMatchesReference() {
  Fixture f = fixture();
  for (ObjectId id : {f.audioClip, f.patClip}) {
    const char* what = id == f.audioClip ? "audio clip" : "pattern clip";
    RenderResult r = renderClipAlone(f.s, id, kRate);
    AudioBuffer ref = rangeReference(f.s, id), from0 = reference(f.s, id);
    size_t n = 0, n0 = 0;
    const double d = maxDiff(r.audio, ref, n), d0 = maxDiff(r.audio, from0, n0);
    std::printf("# %s: %zu frames, reference %zu, max diff %.3g; played from the song start: max diff %.3g, peak %.3f\n", what, r.audio.frames, ref.frames, d,
                d0, peakOf(r.audio));
    expect(r.error.empty() && r.audio.frames > 0 && peakOf(r.audio) > 0.01, (std::string(what) + ": renders sound").c_str());
    expect(d == 0.0, (std::string(what) + ": sample for sample the reference render of the same range").c_str());
    // From the song's start the blocks fall elsewhere: the same sound to float rounding (under -100 dB).
    expect(n0 == r.audio.frames && d0 < 1e-5, (std::string(what) + ": the same as the clip played from the song's start (float rounding)").c_str());
    const Clip* k = f.s.clip(id);
    RenderJob j;
    j.sampleRate = kRate;
    j.scope = RenderScope::Range;
    expect(static_cast<int64_t>(r.audio.frames) == scopeFrames(f.s, j, k->start, k->start + f.s.clipLength(*k), kRate),
           (std::string(what) + ": exactly the clip's span, the tail cut").c_str());
  }
  // The other clip of the same pattern plays over the pattern clip's span: not in its render.
  {
    Song solo = f.s;
    solo.playlist.erase(std::remove_if(solo.playlist.begin(), solo.playlist.end(), [&](const Clip& c) { return c.id == f.otherClip; }),
                        solo.playlist.end());
    size_t n = 0;
    expect(maxDiff(renderClipAlone(f.s, f.patClip, kRate).audio, renderClipAlone(solo, f.patClip, kRate).audio, n) == 0.0,
           "another clip over the span is never in the render");
  }
  // The automation is: without it the render differs.
  {
    Song noAuto = f.s;
    noAuto.playlist.erase(std::remove_if(noAuto.playlist.begin(), noAuto.playlist.end(), [&](const Clip& c) { return c.id == f.autoClip; }),
                          noAuto.playlist.end());
    size_t n = 0;
    expect(maxDiff(renderClipAlone(f.s, f.patClip, kRate).audio, renderClipAlone(noAuto, f.patClip, kRate).audio, n) > 1e-4,
           "the song's automation plays in the render");
  }
  // A muted clip renders as if it played; an automation clip renders nothing.
  {
    Song m = f.s;
    m.clip(f.audioClip)->muted = true;
    size_t n = 0;
    expect(maxDiff(renderClipAlone(m, f.audioClip, kRate).audio, renderClipAlone(f.s, f.audioClip, kRate).audio, n) == 0.0,
           "a muted clip renders as it would play");
    expect(!renderClipAlone(f.s, f.autoClip, kRate).error.empty(), "an automation clip has nothing to render");
  }
}

void testRenderInPlace() {
  Fixture f = fixture();
  const Clip before = *f.s.clip(f.patClip);
  const AudioBuffer want = renderClipAlone(f.s, f.patClip, kRate).audio;
  Document doc(f.s);
  ObjectId nid = kNoId;
  doc.editSong("render clip to audio", [&](Song& x) { nid = renderClipInPlace(x, f.patClip, gDir + "/Rendered", kRate); });
  const Song& s = doc.song();
  const Clip* c = s.clip(nid);
  expect(c && !s.clip(f.patClip), "render in place: the clip is replaced");
  expect(c && c->kind == ClipKind::Audio && c->track == before.track && c->start == before.start && c->length == s.clipLength(before) &&
             c->muted == before.muted,
         "render in place: the audio clip takes the clip's track, start, length and mute");
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  expect(ch && ch->plugin.type == "gl.sampler" && ch->mixerTrack == s.masterId(), "render in place: on a new sampler channel to the master");
  size_t n = 0;
  const AudioBuffer got = ch ? fileAudio(ch->plugin.data) : AudioBuffer{};
  expect(ch && maxDiff(got, want, n) == 0.0 && n > 0, "render in place: the file holds the clip's render, sample for sample");
  // The song sounds as before over the span (the sampler plays the file at unity).
  if (c) {
    const AudioBuffer after = renderClipAlone(s, nid, kRate).audio;
    const double d = maxDiff(after, want, n);
    std::printf("# played back: max diff %.3g\n", d);
    expect(d < 1e-5, "render in place: the new clip plays the same sound");
  }
  expect(doc.undo() && doc.song().clip(f.patClip) && !doc.song().clip(nid), "render in place: one undo step brings the clip back");
  // A muted clip stays muted after its render.
  Song m = f.s;
  m.clip(f.audioClip)->muted = true;
  const ObjectId mid = renderClipInPlace(m, f.audioClip, gDir + "/Rendered", kRate);
  expect(mid != kNoId && m.clip(mid)->muted, "render in place: a muted clip's render is muted too");
}

void testBatch() {
  Fixture f = fixture();
  const std::set<ObjectId> sel = {f.audioClip, f.patClip, f.otherClip, f.autoClip};
  const std::vector<ObjectId> order = renderableClips(f.s, sel);
  expect(order.size() == 3 && order[0] == f.patClip && order[1] == f.otherClip && order[2] == f.audioClip,
         "batch: automation clips left out, the rest in playlist order");
  const auto names = batchFileNames(f.s, order);
  expect(names.size() == 3 && names[0] == "Riff" && names[1] == "Riff 2" && names[2] == "Tone", "batch: one name each, repeats numbered");
  // In place: each clip its own render, one undo step.
  {
    Document doc(f.s);
    std::vector<ObjectId> made;
    doc.editSong("render clips to audio", [&](Song& x) { made = renderClipsInPlace(x, sel, gDir + "/Rendered", kRate); });
    expect(made.size() == 3, "batch in place: three new audio clips");
    bool each = made.size() == 3;
    for (size_t i = 0; i < made.size() && each; ++i) {
      const Clip* c = doc.song().clip(made[i]);
      const Channel* ch = c ? doc.song().channel(c->source) : nullptr;
      size_t n = 0;
      each = ch && maxDiff(fileAudio(ch->plugin.data), renderClipAlone(f.s, order[i], kRate).audio, n) == 0.0 && n > 0;
    }
    expect(each, "batch in place: every clip holds its own render, never the others' sound");
    expect(doc.song().clip(f.autoClip) != nullptr, "batch in place: the automation clip stays");
    expect(doc.undo() && doc.song().clip(f.patClip) && doc.song().clip(f.otherClip) && doc.song().clip(f.audioClip),
           "batch in place: one undo step brings every clip back");
  }
  // To files: 32-bit exact, one file per clip.
  {
    const std::string dir = gDir + "/batch32";
    std::string err;
    const auto files = renderClipsToFiles(f.s, sel, dir, kRate, 32, &err);
    expect(files.size() == 3 && err.empty(), "batch to files: one file per clip");
    bool exact = files.size() == 3;
    for (size_t i = 0; i < files.size() && exact; ++i) {
      size_t n = 0;
      exact = files[i].clip == order[i] && maxDiff(fileAudio(files[i].path), renderClipAlone(f.s, order[i], kRate).audio, n) == 0.0 && n > 0;
    }
    expect(exact, "batch to files (32-bit float): each file is its clip's render, sample for sample");
    expect(files.size() == 3 && fs::path(files[0].path).filename() == "Riff.wav" && fs::path(files[1].path).filename() == "Riff 2.wav" &&
               fs::path(files[2].path).filename() == "Tone.wav",
           "batch to files: named after the clips");
    // Again into the same folder: the files there stay, the new ones take the next numbers.
    const auto again = renderClipsToFiles(f.s, {f.audioClip}, dir, kRate, 32, &err);
    expect(again.size() == 1 && fs::path(again[0].path).filename() == "Tone 2.wav", "batch to files: an existing file is never overwritten");
  }
  // 24-bit: within the dither of the 32-bit render.
  {
    std::string err;
    const auto files = renderClipsToFiles(f.s, {f.patClip}, gDir + "/batch24", kRate, 24, &err);
    size_t n = 0;
    const double d = files.empty() ? 1.0 : maxDiff(fileAudio(files[0].path), renderClipAlone(f.s, f.patClip, kRate).audio, n);
    expect(files.size() == 1 && d < 4.0 / 8388608.0, "batch to files (24-bit): the render within the dither");
  }
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k250");
  testRenderMatchesReference();
  testRenderInPlace();
  testBatch();
  return finish();
}
