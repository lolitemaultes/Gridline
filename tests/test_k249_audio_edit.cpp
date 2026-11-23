// Gridline: K-249, the audio clip editor's Edit audio operations (engine/ClipAudioEdit.h). Each one
// writes a new file and never changes the source. Checks, per group of operations:
//  * cut / trim / silence / insert silence: the expected length and content;
//  * level: fade in, normalize, amplify, reverse, invert;
//  * time and pitch: stretch twice as long, resample to another rate;
//  * channels: mono, swap;
//  * warp: two moved markers give the expected length; markers that move nothing are refused;
//  * the same edit twice gives the same file (named by its audio);
//  * a clip's time range maps to its file's frames (also reversed).
#include <cmath>
#include <filesystem>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/Song.h"
#include "engine/ClipAudioEdit.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace ed = gl::packs::edison;
namespace fs = std::filesystem;
using clipedit::Op;

namespace {

std::string gDir;
constexpr double kRate = 44100;
constexpr int64_t kN = 44100;  // one second

// Left: a ramp 0 .. 0.5; right: a 220 Hz tone at 0.25.
std::string source() {
  ed::Audio a = ed::makeAudio(kRate, 2, kN);
  for (int64_t i = 0; i < kN; ++i) {
    a.L[static_cast<size_t>(i)] = 0.5f * static_cast<float>(i) / static_cast<float>(kN);
    a.R[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(2 * M_PI * 220 * static_cast<double>(i) / kRate));
  }
  const std::string p = gDir + "/source.wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

ed::Audio load(const std::string& p) {
  ed::Content c;
  ed::loadContent(p, c);
  return c.audio ? c.a() : ed::Audio{};
}

clipedit::Result run(const std::string& src, int64_t a, int64_t b, Op op, clipedit::Args g = {}) {
  const clipedit::Result r = clipedit::apply(src, a, b, op, g, gDir + "/edited");
  std::printf("# %s -> %s (%lld frames)%s\n", clipedit::opName(op), r.path.empty() ? "-" : fs::path(r.path).filename().string().c_str(),
              static_cast<long long>(r.frames), r.error.c_str());
  return r;
}

void testRanges(const std::string& src) {
  const auto before = fs::last_write_time(src);
  clipedit::Result r = run(src, 11025, 22050, Op::Cut);
  expect(!r.path.empty() && r.frames == kN - 11025, "cut: a quarter second shorter");
  expect(r.path != src && fs::last_write_time(src) == before, "cut: a new file; the source is untouched");
  const ed::Audio cut = load(r.path);
  expect(cut.frames() == kN - 11025 && std::fabs(cut.L[11025] - 0.5f * 22050.0f / kN) < 1e-5f, "cut: the audio after the cut follows on");
  r = run(src, 11025, 22050, Op::TrimToSelection);
  expect(r.frames == 11025, "trim to the selection");
  r = run(src, 0, 0, Op::TrimToSelection);
  expect(r.frames == kN, "trim with no selection: the whole file");
  r = run(src, 0, 22050, Op::Silence);
  const ed::Audio sil = load(r.path);
  expect(r.frames == kN && sil.L[100] == 0.0f && sil.L[30000] != 0.0f, "silence: the selection only");
  clipedit::Args g;
  g.seconds = 0.5;
  r = run(src, 22050, 22050, Op::InsertSilence, g);
  expect(r.frames == kN + 22050, "insert silence: half a second longer");
}

void testLevels(const std::string& src) {
  ed::Audio a = load(run(src, 0, kN, Op::FadeIn).path);
  expect(std::fabs(a.L[1]) < 1e-4f && std::fabs(a.L[kN - 1] - 0.5f) < 1e-3f, "fade in: silent at its start, untouched at its end");
  a = load(run(src, 0, kN, Op::Normalize).path);
  float pk = 0;
  for (float v : a.L) pk = std::max(pk, std::fabs(v));
  for (float v : a.R) pk = std::max(pk, std::fabs(v));
  expect(std::fabs(pk - 1.0f) < 1e-3f, "normalize: the peak at 0 dB");
  clipedit::Args g;
  g.db = -6.0206;
  a = load(run(src, 0, kN, Op::Amplify, g).path);
  expect(std::fabs(a.L[kN / 2] - 0.125f) < 1e-3f, "amplify -6 dB: half the level");
  a = load(run(src, 0, kN, Op::Reverse).path);
  expect(std::fabs(a.L[0] - 0.5f * (kN - 1) / kN) < 1e-5f, "reverse: the ramp runs down");
  a = load(run(src, 0, kN, Op::Invert).path);
  expect(std::fabs(a.L[kN / 2] + 0.25f) < 1e-4f, "invert: upside down");
}

void testTime(const std::string& src) {
  clipedit::Args g;
  g.factor = 2.0;
  clipedit::Result r = run(src, 0, kN, Op::StretchPitch, g);
  expect(std::llabs(r.frames - 2 * kN) <= 64, "stretch: twice as long");
  g = {};
  g.rate = 22050;
  r = run(src, 0, kN, Op::Resample, g);
  expect(std::fabs(r.rate - 22050) < 1 && std::llabs(r.frames - kN / 2) <= 2, "resample: the new rate, the same duration");
}

void testChannels(const std::string& src) {
  ed::Audio a = load(run(src, 0, kN, Op::Swap).path);
  expect(std::fabs(a.R[kN / 2] - 0.25f) < 1e-5f, "swap: the ramp on the right");
  a = load(run(src, 0, kN, Op::Mono).path);
  expect(a.channels == 1 || (a.L == a.R), "mono: one channel");
}

void testWarp(const std::string& src) {
  clipedit::Args g;
  g.warp = {{1, 0.0, 0.0}, {2, 1.0, 2.0}};
  clipedit::Result r = run(src, 0, kN, Op::ApplyWarp, g);
  expect(!r.path.empty() && std::llabs(r.frames - 2 * kN) <= 64, "apply warp: the file plays the warped time (twice as long)");
  g.warp = {{1, 0.0, 0.0}, {2, 1.0, 1.0}};
  r = run(src, 0, kN, Op::ApplyWarp, g);
  expect(r.path.empty() && !r.error.empty(), "markers that move nothing are refused");
}

void testSameFile(const std::string& src) {
  const clipedit::Result a = run(src, 0, kN, Op::Reverse), b = run(src, 0, kN, Op::Reverse);
  expect(!a.path.empty() && a.path == b.path, "the same edit gives the same file");
}

void testClipRange(const std::string& src) {
  Song s;
  Channel ch;
  ch.id = 10;
  ch.plugin.type = "gl.sampler";
  ch.plugin.data = src;
  s.channels.push_back(ch);
  Clip c;
  c.id = 20;
  c.kind = ClipKind::Audio;
  c.source = 10;
  c.start = 0;
  TempoMap tm;
  tm.build(s);
  c.length = static_cast<Tick>(std::floor(tm.tickAtSeconds(1.0)));
  s.playlist.push_back(c);
  const double half = tm.tickAtSeconds(0.5);
  int64_t a = 0, b = 0;
  expect(clipedit::clipRangeFrames(s, 20, 0, half, a, b) && a == 0 && std::llabs(b - kN / 2) <= 2, "the clip's first half second: frames 0 .. 22050");
  s.playlist[0].reverse = true;
  expect(clipedit::clipRangeFrames(s, 20, 0, half, a, b) && std::llabs(a - kN / 2) <= 2 && std::llabs(b - kN) <= 2, "reversed: the file's second half");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k249e");
  const std::string src = source();
  testRanges(src);
  testLevels(src);
  testTime(src);
  testChannels(src);
  testWarp(src);
  testSameFile(src);
  testClipRange(src);
  return finish();
}
