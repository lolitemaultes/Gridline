// Gridline — J-30: sample memory and background decoding.
//  * Mono and dual-mono files keep one copy (the right channel shares the
//    left one); copies of a Sample keep sharing their own left channel; writing
//    the right channel splits it off.
//  * request() never blocks: it queues a background decode; generation() and
//    pending() report progress; get() waits for a decode already running.
//  * Asynchronous program builds leave samples that are not decoded yet out
//    (silence) and pick them up in a later build; offline renders wait.
//  * trim() releases samples nothing else holds.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../../src/core/Document.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"
#include "../../src/engine/Sample.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;

namespace {

std::string writeTone(const std::string& dir, const std::string& name, size_t ch, size_t frames, bool dual) {
  AudioBuffer b(ch, frames);
  for (size_t i = 0; i < frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 220.0 * static_cast<double>(i) / 44100.0));
    b.at(0, i) = v;
    if (ch > 1) b.at(1, i) = dual ? v : -v;
  }
  const std::string path = dir + "/" + name;
  writeWav(path, b, 44100, 16);
  return path;
}

void testMonoSharing(const std::string& dir) {
  const std::string mono = writeTone(dir, "mono.wav", 1, 4410, false);
  const std::string dual = writeTone(dir, "dual.wav", 2, 4410, true);
  const std::string stereo = writeTone(dir, "stereo.wav", 2, 4410, false);
  SamplePtr m = loadSample(mono), d = loadSample(dual), s = loadSample(stereo);
  expect(m && d && s, "test files decode");
  if (!m || !d || !s) return;
  expect(m->mono() && m->right.data() == m->left.data() && m->right.size() == m->frames(), "mono file: one copy, right shares left");
  expect(d->mono(), "dual-mono stereo file: one copy");
  expect(!s->mono() && s->right[100] != s->left[100] && s->right.size() == s->frames(), "true stereo keeps both channels");
  expect(m->left == m->right && !(s->left == s->right), "channel comparison");
  // Copies share their own left channel; writing the right channel splits it off.
  Sample c = *m;
  expect(c.mono() && std::as_const(c).right.data() == c.left.data() && std::as_const(c).right.data() != m->left.data(), "a copy shares its own left channel");
  c.right[10] = 0.25f;
  expect(!c.mono() && c.right[10] == 0.25f && c.left[10] == m->left[10] && c.right[11] == m->left[11], "writing the right channel splits it");
  Sample moved = std::move(c);
  expect(!moved.mono() && moved.right[10] == 0.25f, "move keeps a split channel");
  Sample tmp = *m;
  Sample mm = std::move(tmp);
  expect(mm.mono() && std::as_const(mm).right.data() == mm.left.data() && mm.right.size() == m->frames(), "move of a shared sample re-binds to its own left channel");
  Sample as;
  as = *m;
  expect(as.mono() && std::as_const(as).right.data() == as.left.data(), "assignment keeps the sharing");
}

void testAsync(const std::string& dir) {
  std::vector<std::string> paths;
  for (int k = 0; k < 6; ++k) paths.push_back(writeTone(dir, "async" + std::to_string(k) + ".wav", 2, 44100 * 2, false));
  SampleCache& sc = sampleCache();
  const uint64_t g0 = sc.generation();
  sc.pauseBackground(true);
  int ready = 0;
  for (const auto& p : paths) ready += sc.request(p) ? 1 : 0;
  expect(ready == 0 && sc.pending() == 6, "request() does not decode on the calling thread; six decodes pending");
  sc.pauseBackground(false);
  sc.waitIdle();
  expect(sc.pending() == 0 && sc.generation() >= g0 + 1, "background decodes finish; generation advances");
  int now = 0;
  for (const auto& p : paths) now += sc.request(p) ? 1 : 0;
  expect(now == 6, "decoded samples are served immediately");
  expect(!sc.request(dir + "/missing.wav") && (sc.waitIdle(), !sc.request(dir + "/missing.wav")), "a missing file stays missing");
  // get() while a background decode of the same file runs: waits for it (one decode).
  const std::string big = writeTone(dir, "big.wav", 2, 44100 * 20, false);
  sc.request(big);
  SamplePtr g = sc.get(big);
  expect(g && g->frames() == 44100u * 20u && !sc.isPending(big), "get() waits for the running background decode");
}

void testProgramBuild(const std::string& dir) {
  const std::string tone = writeTone(dir, "prog.wav", 1, 44100, false);
  Document d(makeEmptySong());
  ObjectId ch = d.addChannel("Smp", "gl.sampler");
  d.editChannel(ch, "", [&](Channel& c) { c.plugin.data = tone; });
  std::vector<Note> notes = {{0, d.song().ppq, 60, 100}};
  d.setNotes(d.song().patterns[0].id, ch, notes);
  d.addClip(d.song().patterns[0].id, 0, 0);
  const Song song = d.song();
  sampleCache().clear();
  sampleCache().pauseBackground(true);
  ProgramBuilder async;
  async.setAsyncSamples(true);
  auto p1 = async.build(song);
  const ChannelAux& a1 = p1->channels[0].aux;
  expect(!a1.sample(), "asynchronous build: the sample is not there yet (silence), nothing waited");
  expect(sampleCache().isPending(tone), "…and it is queued for the background decoders");
  sampleCache().pauseBackground(false);
  sampleCache().waitIdle();
  auto p2 = async.build(song);
  expect(p2->channels[0].aux.sample() != nullptr, "a later build picks the decoded sample up");
  // Offline renders wait for samples.
  sampleCache().clear();
  RenderOptions o;
  AudioBuffer out = renderSong(song, o);
  float pk = 0;
  for (float v : out.data) pk = std::max(pk, std::fabs(v));
  expect(pk > 0.05f, "offline render waits for its samples");
  // trim(): nothing else holds the sample once the programs are gone.
  p1.reset(), p2.reset();
  ProgramBuilder fresh;
  (void)fresh;
  const size_t dropped = sampleCache().trim();
  expect(dropped >= 1 && !sampleCache().isPending(tone), "trim() releases samples nothing holds");
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("samplecache") + "/tmp";
  testMonoSharing(dir);
  testAsync(dir);
  testProgramBuild(dir);
  return finish();
}
