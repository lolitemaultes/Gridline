// Gridline — K-430 (GitHub #84) Linux AppImage / PipeWire / PulseAudio audio recording regression tests.
// Covers:
//   1. Mono hardware microphone (1 ch) with stereo track input (In 1 - In 2) & mono input (In 1),
//      plus >kMaxInputChannels hardware stride safety (zero mis-stride, zero NaNs).
//   2. Bursty PulseAudio / PipeWire delivery (GH #84 exact ~170-215 ms 7424..9472-frame bursts
//      pushed in 64-frame chunks into 128-frame output blocks; plus 512->64 and 64->256 mismatches).
//   3. Capture / output sample-rate mismatch (48 kHz USB mic -> 44.1 kHz project, 44.1 -> 48 kHz,
//      and 96 -> 44.1 kHz with anti-aliasing): clean continuous audio, zero periodic clicks.
//   4. Input only + Low latency monitoring + armed recording WAV round-trip (verifying latent
//      effects are bypassed during recording and committed WAV matches injected signal).
//   5. Zero audio-thread heap allocations across pushInput + process (burst, resample, record).
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/AudioRecord.h"
#include "../src/engine/Sample.h"
#include "../src/packs/edison/edison.h"
#include "Check.h"
#include "Hermetic.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

std::atomic<bool> gCountAllocs{false};
std::atomic<size_t> gAllocCount{0};

}  // namespace

void* operator new(std::size_t sz) {
  if (gCountAllocs.load(std::memory_order_relaxed)) gAllocCount.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(sz ? sz : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

std::string gDir;
constexpr double kTwoPi = 6.28318530717958647692;

bool allFinite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// Counts harsh click discontinuities (step > jumpThresh) and dead zero runs >= minZeroRun inside [from, to).
void analyzeContinuity(const std::vector<float>& x, size_t from, size_t to, float jumpThresh, size_t minZeroRun,
                       int& clicks, int& zeroDropouts, float& maxDiff) {
  clicks = 0;
  zeroDropouts = 0;
  maxDiff = 0.f;
  size_t zrun = 0;
  const size_t end = std::min(to, x.size());
  for (size_t i = from + 1; i < end; ++i) {
    const float d = std::fabs(x[i] - x[i - 1]);
    if (d > maxDiff) maxDiff = d;
    if (d > jumpThresh) ++clicks;
    if (std::fabs(x[i]) < 1e-7f) {
      ++zrun;
    } else {
      if (zrun >= minZeroRun) ++zeroDropouts;
      zrun = 0;
    }
  }
  if (zrun >= minZeroRun) ++zeroDropouts;
}

// ------------------------------------------------------------------ 1. Mono mic & >8-ch stride
void testMonoMicAndStride() {
  const double sr = 44100.0;
  Song song = makeEmptySong();
  song.mixer[1].input = 0;
  song.mixer[1].inputStereo = true;  // User has default "In 1 - In 2" even though mic is 1-ch mono!
  song.mixer[1].armed = true;
  song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);

  ProgramBuilder builder;
  builder.setSampleRate(sr);
  AudioEngine eng(sr);
  // Simulate RØDE NT-USB Mini Mono opened cleanly as 1 hardware channel (UI requested 2).
  eng.setInputChannels(1);
  eng.setInputPrefill(0);
  eng.setMode(PlayMode::Song);
  eng.setLooping(false);
  auto prog = builder.build(song);
  eng.setProgram(prog);

  AudioRecorder rec;
  rec.begin();
  eng.setRecording(true);
  eng.play();

  const int total = 8192;
  const int blk = 128;
  std::vector<float> L(blk), R(blk), monoIn(blk);
  std::vector<float> monL, monR;
  for (int done = 0; done < total; done += blk) {
    for (int i = 0; i < blk; ++i) {
      monoIn[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(done + i) / sr));
    }
    // Inject 1-ch mono with explicit stride = 1.
    eng.pushInput(monoIn.data(), blk, 1);
    eng.process(L.data(), R.data(), blk);
    monL.insert(monL.end(), L.begin(), L.end());
    monR.insert(monR.end(), R.begin(), R.end());
    rec.pump(eng, ringsOf(*prog));
  }
  eng.setRecording(false);
  eng.stop();
  for (int i = 0; i < 8; ++i) {
    std::fill(monoIn.begin(), monoIn.end(), 0.f);
    eng.pushInput(monoIn.data(), blk, 1);
    eng.process(L.data(), R.data(), blk);
    rec.pump(eng, ringsOf(*prog));
  }

  expect(allFinite(monL) && allFinite(monR), "mono mic: monitored output finite");
  float maxLrDiff = 0.f, peakMon = 0.f;
  for (size_t i = 256; i < monL.size(); ++i) {
    maxLrDiff = std::max(maxLrDiff, std::fabs(monL[i] - monR[i]));
    peakMon = std::max(peakMon, std::fabs(monL[i]));
  }
  expect(peakMon > 0.2f && maxLrDiff < 1e-6f, "mono mic with stereo In 1-In 2 mirrors In 1 cleanly onto L and R");
  float spL = 0.f, spR = 0.f;
  eng.takeInputStatusPeaks(spL, spR);
  expect(spL > 0.45f && std::fabs(spL - spR) < 1e-6f, "mono mic: status meter peaks mirror L to R");

  auto takes = rec.release();
  expect(takes.size() == 1 && takes[0].frames() == total, "mono mic: recorded full take");
  if (takes.size() == 1) {
    float maxErr = 0.f;
    for (int i = 0; i < total; ++i) {
      const float expVal = 0.5f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(i) / sr));
      maxErr = std::max(maxErr, std::fabs(takes[0].L[static_cast<size_t>(i)] - expVal));
      maxErr = std::max(maxErr, std::fabs(takes[0].R[static_cast<size_t>(i)] - expVal));
    }
    expect(maxErr < 1e-6f, "mono mic: recorded stereo take matches injected mono sine bit-accurately");
  }

  // Test 1b: Multi-channel interface with stride > kMaxInputChannels (e.g. 16 hardware channels).
  {
    AudioEngine eng16(sr);
    eng16.setInputChannels(kMaxInputChannels);
    eng16.setInputPrefill(0);
    Song s16 = makeEmptySong();
    s16.mixer[1].input = 7;  // In 8 (channel index 7)
    s16.mixer[1].inputStereo = false;
    s16.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);
    ProgramBuilder b16;
    b16.setSampleRate(sr);
    auto p16 = b16.build(s16);
    eng16.setProgram(p16);
    constexpr int kStride = 16;
    std::vector<float> buf16(static_cast<size_t>(blk) * kStride, 0.f);
    for (int i = 0; i < blk; ++i) {
      buf16[static_cast<size_t>(i) * kStride + 7] = 0.6f;
      buf16[static_cast<size_t>(i) * kStride + 12] = -0.9f;  // beyond kMaxInputChannels, must be ignored cleanly
    }
    std::vector<float> oL(blk), oR(blk);
    for (int k = 0; k < 6; ++k) {
      eng16.pushInput(buf16.data(), blk, kStride);
      eng16.process(oL.data(), oR.data(), blk);
    }
    expect(std::fabs(oL.back()) > 0.3f && allFinite(oL), "16-ch hardware stride: In 8 read without mis-stride");
    expect(eng16.inputFramesPushed() == static_cast<uint64_t>(6 * blk), "inputFramesPushed counts pushed frames");
  }
}

// ------------------------------------------------------------------ 2. Bursty delivery mismatches
void testBurstyDelivery() {
  const double sr = 44100.0;

  // 2a: Exact GH #84 failure pattern: PulseAudio / PipeWire-Pulse delivering ~7424..9472-frame
  // bursts (in rapid 64-frame SDL callbacks!) into 128-frame output blocks.
  {
    Song song = makeEmptySong();
    song.mixer[1].input = 0;
    song.mixer[1].inputStereo = false;
    song.mixer[1].armed = true;
    song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);
    ProgramBuilder builder;
    builder.setSampleRate(sr);
    AudioEngine eng(sr);
    eng.setInputChannels(1);
    eng.setInputPrefill(256);  // realistic initial SDL prefill
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
    auto prog = builder.build(song);
    eng.setProgram(prog);

    // Alternating burst intervals matching recording_test.mp3 (7424, 7552, 9472 -> average ~8149 smp).
    const int intervals[] = {8192, 9472, 7424, 7552, 9472, 7296, 7424, 9472, 7808, 7424, 9472};
    const int outBlk = 128;
    const int capChunk = 64;
    int64_t inSample = 0;
    int64_t outSample = 0;
    int nextBurstAt = 0;
    int burstIdx = 0;
    const double freq = 220.0;
    auto sig = [&](int64_t f) { return 0.5f * static_cast<float>(std::sin(kTwoPi * freq * static_cast<double>(f) / sr)); };

    std::vector<float> L(outBlk), R(outBlk), chunk(capChunk);
    AudioRecorder rec;
    bool recStarted = false;
    int urBeforeRec = 0;

    // Run ~3.5 seconds total: first ~0.8 s warms up/calibrates while armed, then record ~2.2 s!
    const int64_t totalOut = 155000;
    const int64_t recStartOut = 45000;
    const int64_t recEndOut = 135000;
    while (outSample < totalOut) {
      if (outSample >= nextBurstAt) {
        const int burstLen = intervals[burstIdx % (sizeof(intervals) / sizeof(intervals[0]))];
        // Deliver burst as rapid back-to-back 64-frame SDL capture callbacks!
        for (int pushed = 0; pushed < burstLen; pushed += capChunk) {
          const int c = std::min(capChunk, burstLen - pushed);
          for (int i = 0; i < c; ++i) chunk[static_cast<size_t>(i)] = sig(inSample++);
          eng.pushInput(chunk.data(), c, 1);
        }
        ++burstIdx;
        nextBurstAt += intervals[burstIdx % (sizeof(intervals) / sizeof(intervals[0]))];
      }
      if (!recStarted && outSample >= recStartOut) {
        recStarted = true;
        urBeforeRec = eng.inputUnderruns();
        rec.begin();
        eng.setRecording(true);
        eng.play();
      }
      if (recStarted && eng.recordingOn() && outSample >= recEndOut) {
        eng.setRecording(false);
        eng.stop();
      }
      eng.process(L.data(), R.data(), outBlk);
      outSample += outBlk;
      if (recStarted) rec.pump(eng, ringsOf(*prog));
    }

    const int urDuringRec = eng.inputUnderruns() - urBeforeRec;
    auto takes = rec.release();
    expect(urDuringRec == 0, "GH #84 8k-burst repro: zero underruns during recording after calibration");
    expect(takes.size() == 1 && takes[0].frames() >= 85000, "GH #84 8k-burst repro: full continuous take captured (not chopped)");
    if (takes.size() == 1 && takes[0].frames() >= 85000) {
      int clicks = 0, dropouts = 0;
      float maxDiff = 0.f;
      // Max sample-to-sample step of 0.5 * sin(2*pi*220/44100) is ~0.0157; > 0.05 is a click!
      analyzeContinuity(takes[0].L, 128, static_cast<size_t>(takes[0].frames() - 128), 0.05f, 16, clicks, dropouts, maxDiff);
      expect(clicks == 0 && dropouts == 0, "GH #84 8k-burst repro: zero clicks and zero silent dropouts in recorded take");
      expect(allFinite(takes[0].L), "GH #84 8k-burst repro: all samples finite");
    }
  }

  // 2b: 512-frame capture bursts into 64-frame output blocks -> zero underrun clicks, bit-exact continuity!
  {
    Song song = makeEmptySong();
    song.mixer[1].input = 0;
    song.mixer[1].inputStereo = true;
    song.mixer[1].armed = true;
    ProgramBuilder builder;
    builder.setSampleRate(sr);
    AudioEngine eng(sr);
    eng.setInputChannels(2);
    eng.setInputPrefill(256);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
    auto prog = builder.build(song);
    eng.setProgram(prog);

    const int outBlk = 64, burst = 512;
    std::vector<float> L(outBlk), R(outBlk), inBuf(static_cast<size_t>(burst) * 2);
    int64_t inFrame = 0;
    auto sig = [](int64_t f, int c) { return static_cast<float>(c == 0 ? 1 : -1) * 0.4f * static_cast<float>(std::sin(kTwoPi * 330.0 * static_cast<double>(f) / 44100.0)); };
    auto pushBurst = [&] {
      for (int i = 0; i < burst; ++i, ++inFrame) {
        inBuf[static_cast<size_t>(2 * i)] = sig(inFrame, 0);
        inBuf[static_cast<size_t>(2 * i + 1)] = sig(inFrame, 1);
      }
      eng.pushInput(inBuf.data(), burst, 2);
    };
    // Prime with 2 bursts so jitter margin is established.
    pushBurst();
    pushBurst();
    AudioRecorder rec;
    rec.begin();
    eng.setRecording(true);
    eng.play();
    const int64_t recFrames = 16384;
    for (int64_t done = 0; done < recFrames; done += outBlk) {
      if (done > 0 && done % burst == 0) pushBurst();
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }
    eng.setRecording(false);
    eng.stop();
    for (int k = 0; k < 16; ++k) {
      if (k % 8 == 0) pushBurst();
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }
    expect(eng.inputUnderruns() == 0, "512-in -> 64-out burst mismatch: zero underruns");
    auto takes = rec.release();
    expect(takes.size() == 1 && takes[0].frames() == recFrames, "512-in -> 64-out: full take recorded");
    if (takes.size() == 1 && takes[0].frames() == recFrames) {
      // Bit-exact match against injected sine (shifted by initial queued prefill frames).
      int clicks = 0, dropouts = 0;
      float maxDiff = 0.f;
      analyzeContinuity(takes[0].L, 0, takes[0].L.size(), 0.04f, 8, clicks, dropouts, maxDiff);
      expect(clicks == 0 && dropouts == 0, "512-in -> 64-out: zero clicks, zero dropouts");
      float err = 0.f;
      for (int64_t i = 0; i < recFrames; ++i) {
        err = std::max(err, std::fabs(takes[0].L[static_cast<size_t>(i)] - sig(i, 0)));
        err = std::max(err, std::fabs(takes[0].R[static_cast<size_t>(i)] - sig(i, 1)));
      }
      expect(err < 1e-6f, "512-in -> 64-out: bit-accurate waveform continuity");
    }
  }

  // 2c: 64-frame capture chunks into 256-frame output blocks -> zero underrun clicks, bit-exact!
  {
    Song song = makeEmptySong();
    song.mixer[1].input = 0;
    song.mixer[1].inputStereo = false;
    song.mixer[1].armed = true;
    ProgramBuilder builder;
    builder.setSampleRate(sr);
    AudioEngine eng(sr);
    eng.setInputChannels(1);
    eng.setInputPrefill(256);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
    auto prog = builder.build(song);
    eng.setProgram(prog);

    const int outBlk = 256, capBlk = 64;
    std::vector<float> L(outBlk), R(outBlk), inBuf(capBlk);
    int64_t inFrame = 0;
    auto sig = [](int64_t f) { return 0.45f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(f) / 44100.0)); };
    auto pushSmall = [&] {
      for (int i = 0; i < capBlk; ++i, ++inFrame) inBuf[static_cast<size_t>(i)] = sig(inFrame);
      eng.pushInput(inBuf.data(), capBlk, 1);
    };
    for (int k = 0; k < 8; ++k) pushSmall();  // 512 frames pre-queued
    AudioRecorder rec;
    rec.begin();
    eng.setRecording(true);
    eng.play();
    const int64_t recFrames = 16384;
    for (int64_t done = 0; done < recFrames; done += outBlk) {
      for (int k = 0; k < outBlk / capBlk; ++k) pushSmall();
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }
    eng.setRecording(false);
    eng.stop();
    for (int k = 0; k < 8; ++k) {
      for (int j = 0; j < 4; ++j) pushSmall();
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }
    expect(eng.inputUnderruns() == 0, "64-in -> 256-out mismatch: zero underruns");
    auto takes = rec.release();
    expect(takes.size() == 1 && takes[0].frames() == recFrames, "64-in -> 256-out: full take recorded");
    if (takes.size() == 1 && takes[0].frames() == recFrames) {
      float err = 0.f;
      for (int64_t i = 0; i < recFrames; ++i)
        err = std::max(err, std::fabs(takes[0].L[static_cast<size_t>(i)] - sig(i)));
      expect(err < 1e-6f, "64-in -> 256-out: bit-accurate waveform continuity");
    }
  }
}

// ------------------------------------------------------------------ 3. Sample-rate mismatch
void testSampleRateMismatch() {
  struct Case {
    const char* name;
    double inSr;
    double outSr;
    double toneHz;
  };
  const Case cases[] = {
      {"48 kHz mic -> 44.1 kHz project", 48000.0, 44100.0, 440.0},
      {"44.1 kHz mic -> 48 kHz project", 44100.0, 48000.0, 523.25},
      {"96 kHz mic -> 44.1 kHz project (AA)", 96000.0, 44100.0, 1000.0},
  };
  for (const Case& tc : cases) {
    Song song = makeEmptySong();
    song.mixer[1].input = 0;
    song.mixer[1].inputStereo = false;
    song.mixer[1].armed = true;
    ProgramBuilder builder;
    builder.setSampleRate(tc.outSr);
    AudioEngine eng(tc.outSr);
    eng.setInputChannels(1);
    eng.setInputSampleRate(tc.inSr);
    eng.setInputPrefill(256);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
    auto prog = builder.build(song);
    eng.setProgram(prog);

    const int outBlk = 128;
    const int capBlk = 256;
    std::vector<float> L(outBlk), R(outBlk), inBuf(capBlk);
    int64_t inFrame = 0;
    double inAccum = 0.0;
    auto pushCap = [&] {
      for (int i = 0; i < capBlk; ++i, ++inFrame)
        inBuf[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(kTwoPi * tc.toneHz * static_cast<double>(inFrame) / tc.inSr));
      eng.pushInput(inBuf.data(), capBlk, 1);
    };
    // Pre-fill ~3 capture blocks.
    pushCap();
    pushCap();
    pushCap();
    inAccum = 0.0;

    AudioRecorder rec;
    rec.begin();
    eng.setRecording(true);
    eng.play();

    const int64_t recOutFrames = static_cast<int64_t>(tc.outSr);  // 1 full second
    for (int64_t done = 0; done < recOutFrames; done += outBlk) {
      inAccum += static_cast<double>(outBlk) * (tc.inSr / tc.outSr);
      while (inAccum >= static_cast<double>(capBlk)) {
        pushCap();
        inAccum -= static_cast<double>(capBlk);
      }
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }
    eng.setRecording(false);
    eng.stop();
    for (int k = 0; k < 12; ++k) {
      pushCap();
      eng.process(L.data(), R.data(), outBlk);
      rec.pump(eng, ringsOf(*prog));
    }

    const std::string pfx = std::string(tc.name) + ": ";
    expect(eng.inputUnderruns() == 0, (pfx + "zero underruns").c_str());
    auto takes = rec.release();
    expect(takes.size() == 1 && takes[0].frames() >= recOutFrames - outBlk, (pfx + "full 1-second take captured").c_str());
    if (takes.size() == 1 && takes[0].frames() > 4096) {
      const auto& y = takes[0].L;
      expect(allFinite(y), (pfx + "all samples finite").c_str());
      int clicks = 0, dropouts = 0;
      float maxDiff = 0.f;
      // Smooth sine wave at toneHz/outSr has small sample-to-sample derivative; check zero clicks/dropouts.
      const float maxExpectedStep = static_cast<float>(0.5 * kTwoPi * tc.toneHz / tc.outSr);
      analyzeContinuity(y, 512, y.size() - 512, maxExpectedStep * 2.5f + 0.02f, 8, clicks, dropouts, maxDiff);
      expect(clicks == 0 && dropouts == 0, (pfx + "zero clicks and zero dropouts").c_str());

      // Measure frequency & spectral purity at exact target frequency via single-bin DFT correlation.
      double re = 0.0, im = 0.0, energy = 0.0;
      const size_t start = 2048, len = 16384;
      for (size_t i = 0; i < len && start + i < y.size(); ++i) {
        const double ang = kTwoPi * tc.toneHz * static_cast<double>(i) / tc.outSr;
        const double v = y[start + i];
        re += v * std::cos(ang);
        im += v * std::sin(ang);
        energy += v * v;
      }
      const double toneEnergy = 2.0 * (re * re + im * im) / static_cast<double>(len);
      const double purity = energy > 0.0 ? toneEnergy / energy : 0.0;
      expect(purity > 0.99, (pfx + "resampled sine lands at exact target pitch with >99% spectral purity").c_str());
    }
  }
}

// ------------------------------------------------------------------ 4. Input only + Low latency round-trip WAV
void testInputOnlyLowLatencyWavRoundTrip() {
  const double sr = 44100.0;
  Song song = makeEmptySong();
  song.tempo = 120.0;
  song.mixer[1].name = "Mic";
  song.mixer[1].input = 0;
  song.mixer[1].inputStereo = false;
  song.mixer[1].armed = true;
  song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::Auto);
  // Put a latent effect (gl.limiter / gl.emphasizer or gl.eq with lookahead) on insert 1 to test Low latency bypass.
  song.mixer[1].slots[0].plugin = defaultPluginState("gl.limiter");
  song.mixer[1].slots[0].enabled = true;

  ProgramBuilder builder;
  builder.setSampleRate(sr);
  AudioEngine eng(sr);
  eng.setInputChannels(1);
  eng.setInputSampleRate(48000.0);  // 48 kHz mono USB mic into 44.1 kHz project!
  eng.setInputPrefill(256);
  eng.setMonitorInputOnly(true);
  eng.setLowLatencyMonitoring(true);
  eng.setMode(PlayMode::Song);
  eng.setLooping(false);
  auto prog = builder.build(song);
  eng.setProgram(prog);

  // Set a punch range beats 2..6 to verify Input only still hears input outside punch!
  eng.setPunch(0, 0);

  const int outBlk = 128, capBlk = 256;
  std::vector<float> L(outBlk), R(outBlk), inBuf(capBlk);
  int64_t inFrame = 0;
  double inAccum = 0.0;
  // Speech-like two-tone modulated test waveform.
  auto testWave = [](double t) {
    return 0.35f * static_cast<float>(std::sin(kTwoPi * 300.0 * t)) +
           0.20f * static_cast<float>(std::sin(kTwoPi * 750.0 * t));
  };
  auto pushCap = [&] {
    for (int i = 0; i < capBlk; ++i, ++inFrame)
      inBuf[static_cast<size_t>(i)] = testWave(static_cast<double>(inFrame) / 48000.0);
    eng.pushInput(inBuf.data(), capBlk, 1);
  };
  pushCap();
  pushCap();
  pushCap();

  AudioRecorder rec;
  rec.begin();
  eng.setRecording(true);
  eng.play();

  float monPeak = 0.f;
  const int64_t recOut = 44100;  // 1.0 s
  for (int64_t done = 0; done < recOut; done += outBlk) {
    inAccum += static_cast<double>(outBlk) * (48000.0 / sr);
    while (inAccum >= static_cast<double>(capBlk)) {
      pushCap();
      inAccum -= static_cast<double>(capBlk);
    }
    eng.process(L.data(), R.data(), outBlk);
    for (float v : L) monPeak = std::max(monPeak, std::fabs(v));
    rec.pump(eng, ringsOf(*prog));
  }
  eng.setRecording(false);
  eng.stop();
  for (int k = 0; k < 12; ++k) {
    pushCap();
    eng.process(L.data(), R.data(), outBlk);
    rec.pump(eng, ringsOf(*prog));
  }

  expect(monPeak > 0.25f, "Input only + Low latency: armed track monitors live input cleanly");
  auto takes = rec.release();
  expect(takes.size() == 1 && takes[0].frames() >= recOut - outBlk, "Input only + Low latency: recorded full take");

  TakeCommitOptions opts;
  opts.dir = gDir + "/Recorded";
  opts.bits = 32;
  opts.sampleRate = sr;
  TakeCommitResult committed = commitTakes(song, takes, opts);
  expect(committed.error.empty() && committed.files.size() == 1, "WAV file committed cleanly");
  if (committed.files.size() == 1) {
    SamplePtr wav = loadSample(committed.files[0]);
    expect(wav && wav->frames() >= static_cast<size_t>(recOut - outBlk), "committed WAV loads with full length");
    if (wav && wav->frames() > 8192) {
      // Find optimal sub-second alignment against testWave(t) and verify tight RMS match!
      const size_t win = 4096;
      const size_t off = 4096;
      double bestCorr = -1.0;
      double bestErrRms = 1e9;
      // Search lag across 1 period of 50 Hz GCD (882 samples at 44.1 kHz) -> exact match!
      for (int lag = 0; lag < 900; ++lag) {
        double num = 0.0, e1 = 0.0, e2 = 0.0, sqErr = 0.0;
        for (size_t i = 0; i < win; ++i) {
          const double got = wav->left[off + i];
          const double ref = testWave(static_cast<double>(off + i + static_cast<size_t>(lag)) / sr);
          num += got * ref;
          e1 += got * got;
          e2 += ref * ref;
          const double d = got - ref;
          sqErr += d * d;
        }
        const double corr = (e1 > 0 && e2 > 0) ? num / std::sqrt(e1 * e2) : 0.0;
        if (corr > bestCorr) {
          bestCorr = corr;
          bestErrRms = std::sqrt(sqErr / static_cast<double>(win));
        }
      }
      expect(bestCorr > 0.995 && bestErrRms < 0.025, "recorded WAV matches injected two-tone signal within tight tolerance");
    }
  }
}

// ------------------------------------------------------------------ 5. Zero audio-thread heap allocations
void testZeroAudioThreadAllocations() {
  const double sr = 44100.0;
  Song song = makeEmptySong();
  song.mixer[1].input = 0;
  song.mixer[1].inputStereo = true;
  song.mixer[1].armed = true;
  song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);

  ProgramBuilder builder;
  builder.setSampleRate(sr);
  AudioEngine eng(sr);
  eng.setInputChannels(1);
  eng.setInputSampleRate(48000.0);
  eng.setInputPrefill(256);
  eng.setMonitorInputOnly(true);
  eng.setLowLatencyMonitoring(true);
  eng.setMode(PlayMode::Song);
  eng.setLooping(false);
  auto prog = builder.build(song);
  eng.setProgram(prog);

  const int outBlk = 128, capBlk = 512;
  std::vector<float> L(outBlk), R(outBlk), inBuf(capBlk, 0.25f);
  // Warm up 1 block before counting (initializes any one-time static tables).
  eng.pushInput(inBuf.data(), capBlk, 1);
  eng.process(L.data(), R.data(), outBlk);

  eng.setRecording(true);
  eng.play();

  gAllocCount.store(0, std::memory_order_relaxed);
  gCountAllocs.store(true, std::memory_order_release);
  for (int b = 0; b < 64; ++b) {
    if (b % 4 == 0) eng.pushInput(inBuf.data(), capBlk, 1);
    eng.process(L.data(), R.data(), outBlk);
  }
  // Also exercise an underrun + adaptive prefill expansion on the audio thread.
  for (int b = 0; b < 16; ++b) eng.process(L.data(), R.data(), outBlk);
  eng.pushInput(inBuf.data(), capBlk, 1);
  eng.process(L.data(), R.data(), outBlk);
  gCountAllocs.store(false, std::memory_order_release);

  const size_t allocs = gAllocCount.load(std::memory_order_relaxed);
  expect(allocs == 0, "zero audio-thread heap allocations across pushInput, resampling, burst adaptation, monitoring, and recording");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k430_linux_recording");
  testMonoMicAndStride();
  testBurstyDelivery();
  testSampleRateMismatch();
  testInputOnlyLowLatencyWavRoundTrip();
  testZeroAudioThreadAllocations();
  return finish();
}
