// Gridline: steps on an audio clip channel in a collaboration session. The channel rack shows the
// channel a playlist audio clip plays from as a row of steps like any other (in every layout), so
// its steps are ordinary pattern edits. Two in-process collaborators (the collab_local pattern,
// messages delayed and reordered in memory):
//  * the song with an audio clip reaches the other computer, its sample with it;
//  * a step set on the audio clip channel by one person reaches the other, as one undo step;
//  * both computers render the same pattern (the step plays the sample) and the same song;
//  * the audio clip itself is untouched, and each undo takes back only its own person's change.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/ClipSources.h"
#include "core/PluginInfo.h"
#include "engine/Render.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

// A short 16-bit mono WAV holding a decaying tone.
std::string writeTone(const std::string& dir) {
  const std::string p = dir + "/tone.wav";
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t frames = 22050;
  f.write("RIFF", 4), u32(36 + frames * 2), f.write("WAVE", 4);
  f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(44100 * 2), u16(2), u16(16);
  f.write("data", 4), u32(frames * 2);
  for (uint32_t i = 0; i < frames; ++i) {
    const double v = std::sin(i * 2.0 * 3.141592653589793 * 220.0 / 44100.0) * std::exp(-3.0 * i / frames);
    u16(static_cast<uint16_t>(static_cast<int16_t>(v * 12000)));
  }
  return p;
}

// A song with one audio clip (a sampler channel playing `wav`) on track 0.
Song songWithClip(const std::string& wav, ObjectId* chOut, ObjectId* clipOut) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Loop";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = 0;
  c.start = 0;
  c.length = s.ppq * 2;
  if (s.tracks.size() < 8) s.tracks.resize(8);
  s.playlist.push_back(c);
  *chOut = ch.id;
  *clipOut = c.id;
  return s;
}

struct Pair {
  Clock clock;
  Net net{78};
  Peer a, b;
  explicit Pair(const Song& song) {
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    a.doc.reset(song);
    a.folder = tmpDir("k078cr_a");
    b.folder = tmpDir("k078cr_b");
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock));
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s));
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle(net, {&a, &b}, clock);
  }
  void sync() { settle(net, {&a, &b}, clock); }
};

std::vector<uint8_t> stepsOf(const Peer& p, ObjectId pat, ObjectId ch) {
  const Pattern* x = p.doc.song().pattern(pat);
  if (!x) return {};
  auto it = x->steps.find(ch);
  return it == x->steps.end() ? std::vector<uint8_t>{} : it->second;
}

int onSteps(const std::vector<uint8_t>& v) {
  int n = 0;
  for (uint8_t x : v) n += x ? 1 : 0;
  return n;
}

AudioBuffer render(const Peer& p, RenderScope scope) {
  RenderJob job;
  job.scope = scope;
  job.pattern = p.doc.song().patterns[0].id;
  job.tail = TailMode::Cut;
  return renderAudio(p.doc.song(), job).audio;
}

float diff(const AudioBuffer& x, const AudioBuffer& y) {
  if (x.frames != y.frames || x.data.size() != y.data.size()) return 1e9f;
  float d = 0;
  for (size_t i = 0; i < x.data.size(); ++i) d = std::max(d, std::fabs(x.data[i] - y.data[i]));
  return d;
}

float peakOf(const AudioBuffer& x) {
  float p = 0;
  for (float v : x.data) p = std::max(p, std::fabs(v));
  return p;
}

}  // namespace

int main() {
  const std::string wav = writeTone(tmpDir("k078cr_media"));
  ObjectId ch = kNoId, clip = kNoId;
  Pair t(songWithClip(wav, &ch, &clip));
  expect(t.b.s->ready(), "the collaborator has the project");
  for (int i = 0; i < 2000; ++i) {
    const Channel* cb = t.b.doc.song().channel(ch);
    if (cb && std::filesystem::exists(cb->plugin.data) && !t.b.s->fileLoading(cb->plugin.data)) break;
    run(t.net, {&t.a, &t.b}, t.clock, 0.02);
  }
  const Channel* cb = t.b.doc.song().channel(ch);
  expect(cb && std::filesystem::exists(cb->plugin.data), "the audio clip's sample reaches the collaborator");
  expect(ops::audioClipChannels(t.b.doc.song()).count(ch) > 0, "it is an audio clip channel there too");
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  const AudioBuffer songBefore = render(t.a, RenderScope::Song);
  expect(peakOf(render(t.a, RenderScope::Pattern)) < 1e-6f, "the pattern is silent before any step");

  // B sets two steps on the audio clip channel (as a click on its rack row does): one undo step each.
  const size_t undo0 = t.b.doc.history().position();
  expect(t.b.doc.setStep(pat, ch, 0, 100), "a step on the audio clip channel");
  expect(t.b.doc.history().position() == undo0 + 1, "the step is one undo step");
  t.b.doc.setStep(pat, ch, 8, 100);
  t.sync();
  expect(onSteps(stepsOf(t.a, pat, ch)) == 2 && stepsOf(t.a, pat, ch) == stepsOf(t.b, pat, ch), "the steps reach the other computer");
  const AudioBuffer pa = render(t.a, RenderScope::Pattern), pb = render(t.b, RenderScope::Pattern);
  expect(peakOf(pa) > 1e-3f, "the steps play the audio clip channel's sample");
  expect(diff(pa, pb) < 1e-6f, "both computers play the steps the same way");
  expect(diff(render(t.a, RenderScope::Song), render(t.b, RenderScope::Song)) < 1e-6f, "both computers render the same song");
  const Clip* ca = t.a.doc.song().clip(clip);
  const Clip* cbb = t.b.doc.song().clip(clip);
  expect(ca && cbb && ca->kind == ClipKind::Audio && cbb->kind == ClipKind::Audio && ca->source == ch && cbb->source == ch, "the playlist audio clip stays as it was");

  // A mutes the channel meanwhile; B's undos take back only B's steps, for both.
  t.a.doc.editChannel(ch, "mute", [](Channel& c) { c.muted = true; });
  t.sync();
  t.b.doc.undo();
  t.b.doc.undo();
  t.sync();
  expect(onSteps(stepsOf(t.a, pat, ch)) == 0 && onSteps(stepsOf(t.b, pat, ch)) == 0, "B's undos take back the steps, for both");
  expect(t.a.doc.song().channel(ch)->muted && t.b.doc.song().channel(ch)->muted, "and leave A's mute");
  t.a.doc.undo();
  t.sync();
  expect(!t.b.doc.song().channel(ch)->muted, "A's undo takes back the mute, for both");
  expect(diff(render(t.a, RenderScope::Song), songBefore) < 1e-6f && diff(render(t.b, RenderScope::Song), songBefore) < 1e-6f,
         "after the undos both play the song as at the start");
  return finish();
}
