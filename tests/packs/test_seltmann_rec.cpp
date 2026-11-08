// Gridline — J-218: recording into Seltmann from an input. The take buffer (pause / resume joins
// without a click, clear, the length limit, the WAV it writes holds exactly the captured frames)
// and the engine's input tap the recorder drains (every block's device input, transport stopped
// or playing, no allocation on the audio thread).
//   gridline_test_seltmann_rec [wavinfo <file>]   (wavinfo: "frames N rate R channels C", for the app test)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/Song.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/edison/edison.h"
#include "../../src/packs/seltmann/seltmann.h"
#include "../Check.h"
#include "../Hermetic.h"

namespace fs = std::filesystem;
using namespace gl;
using gl::packs::seltmann::TakeBuffer;

namespace {

bool gWatch = false;
long gAllocs = 0;

std::vector<float> sine(size_t n, double hz, double rate, double phase, float amp) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2 * M_PI * hz * static_cast<double>(i) / rate + phase));
  return v;
}

float maxStep(const std::vector<float>& x, size_t a, size_t b) {
  float m = 0;
  for (size_t i = std::max<size_t>(a, 1); i < b && i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

void testTakeBasics() {
  TakeBuffer t;
  t.reset(48000, true);
  expect(t.frames() == 0 && t.peaks().empty() && t.seconds() == 0, "a new take is empty");
  std::vector<float> L(1000, 0.25f), R(1000, -0.5f);
  expect(t.append(L.data(), R.data(), 1000) == 1000, "append takes every frame");
  expect(t.append(L.data(), R.data(), 500) == 500 && t.frames() == 1500, "appends add up");
  expect(t.L()[1499] == 0.25f && t.R()[1499] == -0.5f, "both channels kept");
  expect(t.peaks().size() == (1500 + TakeBuffer::kPeakBlock - 1) / TakeBuffer::kPeakBlock, "one peak per block");
  expect(std::fabs(t.peaks()[0] - 0.5f) < 1e-6f, "a peak is the louder channel");
  expect(std::fabs(t.seconds() - 1500.0 / 48000.0) < 1e-12, "seconds");
  // mono: one channel, R reads as L
  TakeBuffer m;
  m.reset(44100, false);
  m.append(L.data(), R.data(), 1000);
  expect(!m.stereo() && m.R()[10] == 0.25f && std::fabs(m.peaks()[0] - 0.25f) < 1e-6f, "a mono take ignores the right input");
  // clear
  t.reset(48000, true);
  expect(t.frames() == 0 && t.peaks().empty(), "clear empties the take");
  // a join on an empty take is harmless
  t.join();
  expect(t.append(L.data(), R.data(), 100) == 100 && t.frames() == 100, "a join before any audio adds nothing");
}

void testJoin() {
  const double sr = 44100;
  // a sine paused and resumed half a cycle out of phase: without the crossfade a ~1.6 jump
  TakeBuffer t;
  t.reset(sr, true);
  const auto a = sine(44100, 220, sr, 0, 0.8f), b = sine(22050, 220, sr, M_PI * 0.93, 0.8f);
  t.append(a.data(), a.data(), a.size());
  t.join();
  // the resume arrives in small blocks (as the UI drains it), the crossfade spans them
  for (size_t i = 0; i < b.size(); i += 100) t.append(b.data() + i, b.data() + i, std::min<size_t>(100, b.size() - i));
  expect(t.frames() == a.size() + b.size() - TakeBuffer::kJoinFrames, "the join overlaps the crossfade (frames = captured - join)");
  const float sineStep = static_cast<float>(2 * M_PI * 220 / sr * 0.8);
  const size_t j = a.size() - TakeBuffer::kJoinFrames;
  const float step = maxStep(t.L(), j - 64, j + TakeBuffer::kJoinFrames + 64);
  char msg[128];
  std::snprintf(msg, sizeof msg, "pause / resume joins without a click (largest step %.4f, the sine's own %.4f)", static_cast<double>(step),
                static_cast<double>(sineStep));
  expect(step < sineStep * 1.6f, msg);
  expect(t.R()[j + 10] == t.L()[j + 10], "both channels crossfade");
  // DC to the opposite DC: a raised-cosine ramp, no step
  TakeBuffer d;
  d.reset(sr, false);
  std::vector<float> hi(2000, 0.5f), lo(2000, -0.5f);
  d.append(hi.data(), nullptr, hi.size());
  d.join();
  d.append(lo.data(), nullptr, lo.size());
  expect(maxStep(d.L(), 0, d.frames()) < 0.01f, "a level jump at a resume is ramped (no click)");
  expect(d.L().front() == 0.5f && d.L().back() == -0.5f, "the audio either side of the join is untouched");
  // the peaks follow the crossfade
  bool peaksOk = true;
  for (size_t b2 = 0; b2 < d.peaks().size(); ++b2) {
    float p = 0;
    for (size_t k = b2 * TakeBuffer::kPeakBlock; k < std::min(d.frames(), (b2 + 1) * TakeBuffer::kPeakBlock); ++k) p = std::max(p, std::fabs(d.L()[k]));
    peaksOk = peaksOk && std::fabs(p - d.peaks()[b2]) < 1e-6f;
  }
  expect(peaksOk, "the overview peaks match the take after a join");
}

void testLimit() {
  TakeBuffer t;
  t.reset(1000, true);  // 600 000 frames at most
  std::vector<float> x(100000, 0.1f);
  size_t got = 0;
  for (int i = 0; i < 7; ++i) got += t.append(x.data(), x.data(), x.size());
  expect(got == 600000 && t.frames() == 600000 && t.full(), "the take stops at its ten-minute limit");
  expect(t.append(x.data(), x.data(), 10) == 0, "nothing more is taken when full");
}

void testWav(const std::string& dir) {
  const double sr = 48000;
  TakeBuffer t;
  t.reset(sr, true);
  const auto a = sine(30001, 330, sr, 0, 0.5f), b = sine(30001, 110, sr, 0, 0.25f);
  t.append(a.data(), b.data(), a.size());
  const std::string f = (fs::path(dir) / "take.wav").string();
  expect(t.writeWav(f, 32), "the take writes as a WAV");
  SamplePtr s = loadSample(f);
  expect(s && s->frames() == t.frames(), "the WAV holds exactly the captured frames");
  expect(s && std::fabs(s->sampleRate - sr) < 0.5, "... at the take's rate");
  bool same = s != nullptr;
  if (s) {
    const std::vector<float> r = s->right.vec();
    for (size_t i = 0; i < t.frames(); i += 997) same = same && s->left[i] == t.L()[i] && r[i] == t.R()[i];
  }
  expect(same, "... sample for sample (32-bit float)");
  // save(): the recordings' folder and naming
  const std::string rec = (fs::path(dir) / "Project" / "Recorded").string();
  std::string err;
  const std::string p = t.save(rec, 24, &err);
  expect(!p.empty() && fs::is_regular_file(p), "save writes into the Recorded folder (made when missing)");
  expect(fs::path(p).filename().string().rfind("Seltmann_rec-", 0) == 0 && fs::path(p).extension() == ".wav", "... named Seltmann_rec-<hash>.wav");
  SamplePtr s2 = loadSample(p);
  expect(s2 && s2->frames() == t.frames(), "... with every frame (24-bit)");
  expect(t.save(rec, 24) == p, "the same take saves to the same file");
  // mono takes are one channel
  TakeBuffer m;
  m.reset(sr, false);
  m.append(a.data(), nullptr, 4800);
  const std::string mf = (fs::path(dir) / "mono.wav").string();
  expect(m.writeWav(mf, 16), "a mono take writes");
  packs::edison::Content c;
  expect(packs::edison::loadContent(mf, c) && c.audio && c.audio->channels == 1 && c.frames() == 4800, "... as one channel");
  TakeBuffer e;
  e.reset(sr, true);
  expect(!e.writeWav(mf, 16) && e.save(rec, 16).empty(), "an empty take writes nothing");
}

// The input tap: device input of every block, whatever the transport does.
void testTap() {
  const double sr = 48000;
  AudioEngine eng(sr);
  eng.setInputChannels(4);
  const int n = 256;
  std::vector<float> L(n), R(n), in(static_cast<size_t>(n) * 4);
  auto push = [&](int64_t at) {
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 4; ++c) in[static_cast<size_t>(i * 4 + c)] = static_cast<float>(c + 1) * 0.1f + static_cast<float>((at + i) % 100) * 1e-4f;
    eng.pushInput(in.data(), n);
  };
  push(0);
  eng.process(L.data(), R.data(), n);
  expect(!eng.inputTapOn(), "the tap is off until asked");
  eng.setInputTap(1, true);  // In 2 - In 3
  const auto& ring = eng.inputTap();
  const size_t w0 = ring.written();
  push(n);
  eng.process(L.data(), R.data(), n);
  expect(ring.written() == w0 + static_cast<size_t>(n), "every block's input enters the tap (transport stopped, no program)");
  std::vector<float> tl(n), tr(n);
  size_t from = w0;
  ring.read(from, static_cast<size_t>(n), tl.data(), tr.data(), nullptr);
  expect(std::fabs(tl[5] - (0.2f + static_cast<float>((n + 5) % 100) * 1e-4f)) < 1e-6f &&
             std::fabs(tr[5] - (0.3f + static_cast<float>((n + 5) % 100) * 1e-4f)) < 1e-6f,
         "a stereo pair taps its two channels");
  eng.setInputTap(3, false);  // In 4, mono
  from = ring.written();
  push(2 * n);
  eng.process(L.data(), R.data(), n);
  ring.read(from, static_cast<size_t>(n), tl.data(), tr.data(), nullptr);
  expect(std::fabs(tl[0] - (0.4f + static_cast<float>((2 * n) % 100) * 1e-4f)) < 1e-6f && tl[0] == tr[0], "a mono input feeds both sides");
  eng.setInputTap(3, true);  // a pair past the last channel: its single channel
  from = ring.written();
  push(3 * n);
  eng.process(L.data(), R.data(), n);
  ring.read(from, static_cast<size_t>(n), tl.data(), tr.data(), nullptr);
  expect(tl[7] == tr[7], "a pair without its second channel taps one channel");
  // with a song playing
  Song song = makeEmptySong();
  ProgramBuilder b;
  b.setSampleRate(sr);
  auto prog = b.build(song);
  eng.setProgram(prog);
  eng.setMode(PlayMode::Song);
  eng.play();
  for (int k = 0; k < 4; ++k) {
    push(4 * n + k * n);
    eng.process(L.data(), R.data(), n);
  }
  const size_t w1 = ring.written();
  push(8 * n);
  eng.process(L.data(), R.data(), n);
  expect(ring.written() == w1 + static_cast<size_t>(n), "the tap keeps running while the song plays");
  // the audio thread does not allocate for the tap
  for (int k = 0; k < 8; ++k) {
    push(9 * n + k * n);
    eng.process(L.data(), R.data(), n);
  }
  gAllocs = 0;
  gWatch = true;
  for (int k = 0; k < 32; ++k) {
    eng.pushInput(in.data(), n);
    eng.process(L.data(), R.data(), n);
  }
  gWatch = false;
  expect(gAllocs == 0, "no allocation in the audio thread with the tap on");
  eng.stop();
  eng.setInputTap(-1, true);
  const size_t w2 = ring.written();
  push(0);
  eng.process(L.data(), R.data(), n);
  expect(!eng.inputTapOn() && ring.written() == w2, "off: the tap is left alone");
}

int wavInfo(const char* path) {
  packs::edison::Content c;
  if (!packs::edison::loadContent(path, c) || !c.audio) {
    std::printf("unreadable\n");
    return 1;
  }
  std::printf("frames %lld rate %.0f channels %d\n", static_cast<long long>(c.frames()), c.rate(), c.audio->channels);
  return 0;
}

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11  // (GCC 11 added this warning)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "wavinfo") return wavInfo(argv[2]);
  const std::string dir = gl::test::hermeticTestEnv("seltmann_rec");
  testTakeBasics();
  testJoin();
  testLimit();
  testWav(dir);
  testTap();
  return finish();
}
