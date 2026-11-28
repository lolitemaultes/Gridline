// Gridline — K-407, K-416, K-417 Browser Pro Preview unit tests:
//  - K-407: phase-locked tempo-synced loop preview (< 1 ms phase error across
//    tempos 90/120/145 BPM, quantisations 1/8..1 bar, speeds x1/2..x2, Song &
//    Pattern mode, mid-preview tempo changes, transport stop, one-shots unsynced)
//  - K-416: waveform scrubbing (seekPreview jump & playhead reporting, synced + unsynced)
//  - K-417: pitch transpose knob (+/-12 st via shared stretch engine, duration preserved,
//    composes with synced loop preview)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "Check.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Sample.h"

using namespace gl;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;

// Builds a 1-bar click-track loop at `loopBpm` with sharp windowed pulses on each of the 4 beats.
SamplePtr makeClickLoop(double loopBpm) {
  const double sec = 4.0 * 60.0 / loopBpm;
  const size_t total = static_cast<size_t>(std::llround(sec * kSr));
  auto s = std::make_shared<Sample>();
  s->path = "click_loop.wav";
  s->sampleRate = kSr;
  s->left.assign(total, 0.0f);
  s->right.assign(total, 0.0f);
  for (int b = 0; b < 4; ++b) {
    const int64_t center = static_cast<int64_t>(std::llround((static_cast<double>(b) / 4.0) * static_cast<double>(total)));
    // Short 4 ms symmetric Hann-windowed burst centered exactly at `center` so its energy centroid stays at beat `b` at any stretch ratio.
    const int64_t halfLen = 88;  // +/- 2 ms around center
    for (int64_t k = -halfLen; k <= halfLen; ++k) {
      int64_t idx = (center + k) % static_cast<int64_t>(total);
      if (idx < 0) idx += static_cast<int64_t>(total);
      const double u = static_cast<double>(k + halfLen) / static_cast<double>(2 * halfLen);
      const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * u));
      const float v = static_cast<float>(0.9 * w * std::cos(2.0 * kPi * 1500.0 * static_cast<double>(k) / kSr));
      s->left[static_cast<size_t>(idx)] = v;
      s->right[static_cast<size_t>(idx)] = v;
    }
  }
  return s;
}

// Builds a 1.0 s pure sine wave at `freqHz`.
SamplePtr makeSineSample(double freqHz, double seconds = 1.0) {
  const size_t total = static_cast<size_t>(std::llround(seconds * kSr));
  auto s = std::make_shared<Sample>();
  s->path = "sine.wav";
  s->sampleRate = kSr;
  s->left.resize(total);
  s->right.resize(total);
  for (size_t i = 0; i < total; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * freqHz * static_cast<double>(i) / kSr));
    s->left[i] = s->right[i] = v;
  }
  return s;
}

// Builds a 1.0 s two-tone sample: 220 Hz in first half, 880 Hz in second half (for scrubbing verification).
SamplePtr makeTwoHalfSample() {
  const size_t total = static_cast<size_t>(kSr);
  auto s = std::make_shared<Sample>();
  s->path = "twohalf.wav";
  s->sampleRate = kSr;
  s->left.resize(total);
  s->right.resize(total);
  for (size_t i = 0; i < total; ++i) {
    const double f = i < total / 2 ? 220.0 : 880.0;
    const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(i) / kSr));
    s->left[i] = s->right[i] = v;
  }
  return s;
}

// Energy at `freqHz` via Goertzel / DFT magnitude over buf[from..from+len).
double freqEnergy(const std::vector<float>& buf, size_t from, size_t len, double freqHz) {
  double re = 0.0, im = 0.0;
  const size_t end = std::min(buf.size(), from + len);
  for (size_t i = from; i < end; ++i) {
    const double ang = 2.0 * kPi * freqHz * static_cast<double>(i - from) / kSr;
    re += buf[i] * std::cos(ang);
    im -= buf[i] * std::sin(ang);
  }
  return std::hypot(re, im) / std::max<size_t>(1, end - from);
}

// Finds peak envelope onset/center near `expectedSample` within +/- searchWin samples.
double peakErrorMs(const std::vector<float>& buf, int64_t expectedSample, int searchWin = 1200) {
  const int64_t lo = std::max<int64_t>(0, expectedSample - searchWin);
  const int64_t hi = std::min<int64_t>(static_cast<int64_t>(buf.size()) - 1, expectedSample + searchWin);
  // Burst is symmetric around `expectedSample`.
  const int64_t targetCenter = expectedSample;
  double bestE = 0.0;
  int64_t bestIdx = -1;
  for (int64_t c = lo; c <= hi; ++c) {
    const float a = std::fabs(buf[static_cast<size_t>(c)]);
    if (a > bestE) {
      bestE = a;
      bestIdx = c;
    }
  }
  if (bestIdx < 0 || bestE < 0.05) return 999.0;
  // Energy-weighted centroid within +/- 180 samples of peak to measure burst center precisely across stretch ratios.
  double num = 0.0, den = 0.0;
  const int64_t cLo = std::max<int64_t>(0, bestIdx - 180);
  const int64_t cHi = std::min<int64_t>(static_cast<int64_t>(buf.size()) - 1, bestIdx + 180);
  for (int64_t i = cLo; i <= cHi; ++i) {
    const double e = static_cast<double>(buf[static_cast<size_t>(i)]) * static_cast<double>(buf[static_cast<size_t>(i)]);
    num += static_cast<double>(i) * e;
    den += e;
  }
  const double measuredCenter = den > 0.0 ? num / den : static_cast<double>(bestIdx);
  return std::fabs(measuredCenter - static_cast<double>(targetCenter)) * 1000.0 / kSr;
}

Song makeSilentSong(double bpm, int bars = 16) {
  Song s = makeEmptySong();
  s.tempo = bpm;
  s.patterns[0].bars = bars;
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Pattern;
  c.pattern = s.patterns[0].id;
  c.track = 0;
  c.start = 0;
  c.length = s.barTicks() * bars;
  s.playlist.push_back(c);
  return s;
}

void testPhaseLockTempo(double songBpm, PlayMode mode, AudioEngine::PreviewQuant quant, double speedMult, double pitchSt, const char* label) {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(songBpm);
  eng.setProgram(pb.build(s));
  eng.setMode(mode);
  eng.play();

  // Advance transport to fractional beat 0.37 so launch must wait for the quantised boundary.
  const double samplesPerBeat = kSr * 60.0 / songBpm;
  const int preSamples = static_cast<int>(std::llround(0.37 * samplesPerBeat));
  std::vector<float> tmpL(256), tmpR(256);
  for (int done = 0; done < preSamples;) {
    const int k = std::min(256, preSamples - done);
    eng.process(tmpL.data(), tmpR.data(), k);
    done += k;
  }

  SamplePtr loop = makeClickLoop(120.0);
  AudioEngine::PreviewOptions opt;
  opt.synced = true;
  opt.sampleBpm = 120.0;
  opt.quant = quant;
  opt.speedMult = speedMult;
  opt.pitchSemitones = pitchSt;
  eng.preview(loop, opt);

  // Render 8 beats of audio in 256-sample blocks.
  const int renderSamples = static_cast<int>(std::ceil(8.0 * samplesPerBeat));
  std::vector<float> outL(static_cast<size_t>(renderSamples), 0.0f);
  std::vector<float> outR(static_cast<size_t>(renderSamples), 0.0f);
  bool sawPending = false;
  for (int off = 0; off < renderSamples;) {
    const int k = std::min(256, renderSamples - off);
    eng.process(outL.data() + off, outR.data() + off, k);
    if (off == 0 && eng.previewPending()) sawPending = true;
    off += k;
  }

  const double qBeats = quant == AudioEngine::PreviewQuant::Eighth  ? 0.5
                      : quant == AudioEngine::PreviewQuant::Quarter ? 1.0
                      : quant == AudioEngine::PreviewQuant::Half    ? 2.0
                                                                    : 4.0;
  const double launchBeat = std::ceil((0.37 - 1e-6) / qBeats) * qBeats;
  const int64_t launchSample = static_cast<int64_t>(std::llround((launchBeat - 0.37) * samplesPerBeat));

  expect(sawPending, (std::string(label) + ": armed pending before quant boundary").c_str());

  // Silence before launchSample (leave 20-sample tolerance for block boundary rounding).
  float prePeak = 0.0f;
  for (int64_t i = 0; i < std::max<int64_t>(0, launchSample - 20); ++i) prePeak = std::max(prePeak, std::fabs(outL[static_cast<size_t>(i)]));
  expect(prePeak < 1e-5f, (std::string(label) + ": silent prior to launch boundary").c_str());

  // Measure phase error on every loop-beat click starting at or after launchBeat!
  // Loop clicks occur whenever beat * speedMult is an integer (beat = k / speedMult).
  const int firstClickK = static_cast<int>(std::ceil(launchBeat * speedMult - 1e-6));
  double maxErrMs = 0.0;
  int checked = 0;
  for (int k = firstClickK; k < firstClickK + 6; ++k) {
    const double beat = static_cast<double>(k) / speedMult;
    if (beat < launchBeat + 0.05) continue;  // skip partial half-burst cut right at launch edge
    const int64_t expectedSample = static_cast<int64_t>(std::llround((beat - 0.37) * samplesPerBeat));
    if (expectedSample + 1500 >= renderSamples) break;
    const double err = peakErrorMs(outL, expectedSample);
    maxErrMs = std::max(maxErrMs, err);
    ++checked;
  }
  char msg[160];
  std::snprintf(msg, sizeof msg, "%s: max phase error %.3f ms < 1.0 ms (%d beats)", label, maxErrMs, checked);
  expect(checked >= 2 && maxErrMs < 1.0, msg);

  // Stopping transport stops synced loop preview cleanly.
  eng.stop();
  eng.process(tmpL.data(), tmpR.data(), 256);
  expect(!eng.previewing() && !eng.previewPending(), (std::string(label) + ": transport stop stops synced preview").c_str());
}

void testMidPreviewTempoChange() {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(120.0);
  eng.setProgram(pb.build(s));
  eng.setMode(PlayMode::Song);
  eng.play();

  SamplePtr loop = makeClickLoop(120.0);
  AudioEngine::PreviewOptions opt;
  opt.synced = true;
  opt.sampleBpm = 120.0;
  opt.quant = AudioEngine::PreviewQuant::Quarter;
  opt.speedMult = 1.0;
  eng.preview(loop, opt);

  // Render 2 beats at 120 BPM.
  const int b2 = static_cast<int>(std::llround(2.0 * kSr * 60.0 / 120.0));
  std::vector<float> tmpL(256), tmpR(256);
  for (int done = 0; done < b2;) {
    const int k = std::min(256, b2 - done);
    eng.process(tmpL.data(), tmpR.data(), k);
    done += k;
  }

  // Change tempo mid-preview to 144 BPM!
  s.tempo = 144.0;
  eng.setProgram(pb.build(s));
  const double startBeat = eng.position() / static_cast<double>(s.ppq);
  const double spbNew = kSr * 60.0 / 144.0;
  const int renderN = static_cast<int>(std::ceil(5.0 * spbNew));
  std::vector<float> outL(static_cast<size_t>(renderN), 0.0f), outR(static_cast<size_t>(renderN), 0.0f);
  bool allFinite = true;
  for (int off = 0; off < renderN;) {
    const int k = std::min(256, renderN - off);
    eng.process(outL.data() + off, outR.data() + off, k);
    for (int i = 0; i < k; ++i)
      if (!std::isfinite(outL[static_cast<size_t>(off + i)])) allFinite = false;
    off += k;
  }
  expect(allFinite, "mid-preview tempo change produces finite output");

  // Check beats 1..3 after the tempo change!
  const double firstWholeBeat = std::ceil(startBeat + 0.2);
  double maxErr = 0.0;
  int checked = 0;
  for (int k = 0; k < 3; ++k) {
    const double beat = firstWholeBeat + k;
    const int64_t expected = static_cast<int64_t>(std::llround((beat - startBeat) * spbNew));
    if (expected + 1500 >= renderN) break;
    maxErr = std::max(maxErr, peakErrorMs(outL, expected));
    ++checked;
  }
  char msg[128];
  std::snprintf(msg, sizeof msg, "mid-preview tempo change 120->144 BPM: max error %.3f ms < 1.0 ms", maxErr);
  expect(checked >= 2 && maxErr < 1.0, msg);
}

void testOneShotUnsynced() {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(120.0);
  eng.setProgram(pb.build(s));
  eng.play();

  std::vector<float> tmpL(256), tmpR(256);
  eng.process(tmpL.data(), tmpR.data(), 256);
  eng.process(tmpL.data(), tmpR.data(), 256);

  // Short 0.1 s one-shot starting with an immediate impulse.
  auto oneShot = std::make_shared<Sample>();
  oneShot->path = "kick.wav";
  oneShot->sampleRate = kSr;
  oneShot->left.assign(4410, 0.0f);
  oneShot->right.assign(4410, 0.0f);
  oneShot->left[0] = oneShot->right[0] = 0.9f;
  oneShot->left[1] = oneShot->right[1] = 0.7f;

  AudioEngine::PreviewOptions opt;
  opt.synced = false;
  opt.immediate = true;
  eng.preview(oneShot, opt);
  eng.process(tmpL.data(), tmpR.data(), 256);
  expect(!eng.previewPending(), "one-shot is not pending");
  expect(std::fabs(tmpL[0]) > 0.5f, "one-shot plays immediately at frame 0");
  for (int i = 0; i < 25; ++i) eng.process(tmpL.data(), tmpR.data(), 256);
  expect(!eng.previewing(), "one-shot stops cleanly without looping");
}

void testWaveformScrubbing() {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(120.0);
  eng.setProgram(pb.build(s));

  SamplePtr smp = makeTwoHalfSample();
  AudioEngine::PreviewOptions opt;
  opt.synced = false;
  opt.immediate = true;
  eng.preview(smp, opt);

  // Play initial 0.1 s -> 220 Hz dominant, position near 0.1.
  std::vector<float> bufL(4096), bufR(4096);
  for (int off = 0; off < 4096; off += 256) eng.process(bufL.data() + off, bufR.data() + off, 256);
  const float posEarly = eng.previewPosition();
  expect(posEarly > 0.05f && posEarly < 0.20f, "playhead advances from start");
  expect(freqEnergy(bufL, 0, 4096, 220.0) > freqEnergy(bufL, 0, 4096, 880.0) * 3.0, "first half plays 220 Hz");

  // Scrub to 0.65 (inside the 880 Hz second half!).
  eng.seekPreview(0.65);
  std::fill(bufL.begin(), bufL.end(), 0.0f);
  for (int off = 0; off < 4096; off += 256) eng.process(bufL.data() + off, bufR.data() + off, 256);
  const float posAfter = eng.previewPosition();
  expect(posAfter > 0.68f && posAfter < 0.85f, "scrub jumped playhead past 0.65");
  expect(freqEnergy(bufL, 0, 4096, 880.0) > freqEnergy(bufL, 0, 4096, 220.0) * 3.0, "scrubbed audio plays 880 Hz second half");
}

void testPitchTransposeKnob() {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(120.0);
  eng.setProgram(pb.build(s));

  SamplePtr sine440 = makeSineSample(440.0, 0.8);

  // +12 st -> ~880 Hz, duration still ~0.8 s!
  {
    AudioEngine::PreviewOptions opt;
    opt.synced = false;
    opt.immediate = true;
    opt.pitchSemitones = 12.0;
    eng.preview(sine440, opt);
    std::vector<float> bufL(16384), bufR(16384);
    for (int off = 0; off < 16384; off += 256) eng.process(bufL.data() + off, bufR.data() + off, 256);
    const double e880 = freqEnergy(bufL, 4096, 8192, 880.0);
    const double e440 = freqEnergy(bufL, 4096, 8192, 440.0);
    expect(e880 > 0.15 && e880 > e440 * 5.0, "+12 st shifts 440 Hz to 880 Hz");
    expect(eng.previewing(), "at 0.37 s sample is still playing (tempo/duration preserved)");
    // Advance another 0.55 s -> should finish cleanly near 0.8 s.
    std::vector<float> tmpL(256), tmpR(256);
    for (int i = 0; i < 100; ++i) eng.process(tmpL.data(), tmpR.data(), 256);
    expect(!eng.previewing(), "+12 st finishes at original duration");
  }

  // -12 st -> ~220 Hz!
  {
    AudioEngine::PreviewOptions opt;
    opt.synced = false;
    opt.immediate = true;
    opt.pitchSemitones = -12.0;
    eng.preview(sine440, opt);
    std::vector<float> bufL(16384), bufR(16384);
    for (int off = 0; off < 16384; off += 256) eng.process(bufL.data() + off, bufR.data() + off, 256);
    const double e220 = freqEnergy(bufL, 4096, 8192, 220.0);
    const double e440 = freqEnergy(bufL, 4096, 8192, 440.0);
    expect(e220 > 0.15 && e220 > e440 * 5.0, "-12 st shifts 440 Hz to 220 Hz");
  }
}

// A file at another sample rate than the engine keeps its pitch on the pitched (stretch) path.
void testPitchedPreviewOtherRate() {
  AudioEngine eng(kSr);
  ProgramBuilder pb;
  Song s = makeSilentSong(120.0);
  eng.setProgram(pb.build(s));
  const double fileSr = 48000.0;
  const size_t total = static_cast<size_t>(0.8 * fileSr);
  auto smp = std::make_shared<Sample>();
  smp->path = "sine48k.wav";
  smp->sampleRate = fileSr;
  smp->left.resize(total);
  smp->right.resize(total);
  for (size_t i = 0; i < total; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / fileSr));
    smp->left[i] = smp->right[i] = v;
  }
  AudioEngine::PreviewOptions opt;
  opt.synced = false;
  opt.immediate = true;
  opt.pitchSemitones = 12.0;
  eng.preview(smp, opt);
  std::vector<float> bufL(16384), bufR(16384);
  for (int off = 0; off < 16384; off += 256) eng.process(bufL.data() + off, bufR.data() + off, 256);
  const double e880 = freqEnergy(bufL, 4096, 8192, 880.0);
  const double e808 = freqEnergy(bufL, 4096, 8192, 880.0 * kSr / fileSr);  // where a rate mix-up lands
  expect(e880 > 0.15 && e880 > e808 * 3.0, "48 kHz file at +12 st on a 44.1 kHz engine plays 880 Hz");
}

}  // namespace

int main() {
  testPhaseLockTempo(120.0, PlayMode::Song, AudioEngine::PreviewQuant::Bar, 1.0, 0.0, "120 BPM Song 1bar x1");
  testPhaseLockTempo(90.0, PlayMode::Song, AudioEngine::PreviewQuant::Quarter, 1.0, 0.0, "90 BPM Song 1/4 x1");
  testPhaseLockTempo(145.0, PlayMode::Song, AudioEngine::PreviewQuant::Half, 1.0, 0.0, "145 BPM Song 1/2 x1");
  testPhaseLockTempo(128.0, PlayMode::Pattern, AudioEngine::PreviewQuant::Eighth, 1.0, 0.0, "128 BPM Pattern 1/8 x1");
  testPhaseLockTempo(120.0, PlayMode::Song, AudioEngine::PreviewQuant::Quarter, 2.0, 0.0, "120 BPM Song 1/4 x2");
  testPhaseLockTempo(120.0, PlayMode::Song, AudioEngine::PreviewQuant::Half, 0.5, 0.0, "120 BPM Song 1/2 x1/2");
  testPhaseLockTempo(135.0, PlayMode::Song, AudioEngine::PreviewQuant::Quarter, 1.0, 7.0, "135 BPM Song 1/4 +7st");
  testMidPreviewTempoChange();
  testOneShotUnsynced();
  testWaveformScrubbing();
  testPitchTransposeKnob();
  testPitchedPreviewOtherRate();
  return finish();
}
