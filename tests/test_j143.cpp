// Gridline J-143: the top bar OUT meter (shared level meter ballistics and the
// engine's master bus peak) and the shipped template list without the removed
// "Song sketch" project.
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/LevelMeter.h"
#include "core/Templates.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr float kPi = 3.14159265358979f;

// Block peaks of a sine at `amp` (what the engine hands the UI between frames).
float sinePeak(float amp, int frames, float hz = 997.0f, float sr = 48000.0f) {
  float p = 0;
  for (int i = 0; i < frames; ++i) p = std::max(p, std::fabs(amp * std::sin(2.0f * kPi * hz * static_cast<float>(i) / sr)));
  return p;
}

// Drives a meter at a fixed frame rate for `seconds` with a constant peak per frame.
void run(LevelMeter& m, float peak, double fps, double seconds) {
  const int frames = static_cast<int>(std::lround(seconds * fps));
  for (int i = 0; i < frames; ++i) m.update(peak, peak, static_cast<float>(1.0 / fps));
}

void testScale() {
  expect(LevelMeter::toDb(0.0f) == LevelMeter::kFloorDb && LevelMeter::toPos(LevelMeter::toDb(0.0f)) == 0.0f, "scale: silence is the floor (position 0)");
  expect(LevelMeter::toDb(1e-7f) == LevelMeter::kFloorDb, "scale: a -140 dB denormal tail is at the floor");
  expect(std::fabs(LevelMeter::toDb(1.0f)) < 1e-4f, "scale: full scale is 0 dB");
  expect(std::fabs(LevelMeter::toDb(0.5011872f) + 6.0f) < 1e-3f, "scale: -6 dBFS is -6 dB");
  // The mixer master meter's mapping: -60..+6 dB over the travel.
  expect(std::fabs(LevelMeter::toPos(0.0f) - 60.0f / 66.0f) < 1e-6f && LevelMeter::toPos(6.0f) == 1.0f, "scale: matches the mixer meter (-60..+6 dB)");
  expect(LevelMeter::toGain(LevelMeter::kFloorDb) == 0.0f && std::fabs(LevelMeter::toGain(-6.0f) - 0.501187f) < 1e-5f, "scale: dB back to gain");
}

void testBallistics() {
  {
    LevelMeter m;
    run(m, 0.0f, 60, 2.0);
    expect(m.atRest() && m.level(0) == 0.0f && m.hold(1) == 0.0f, "silence: the meter rests at the floor, no hold, no clip");
  }
  {  // A 0 dBFS sine reads 0 dB, no clip (a full-scale sample is not an over).
    LevelMeter m;
    const float pk = sinePeak(1.0f, 800);
    expect(pk <= 1.0f && pk > 0.999f, "0 dBFS sine peaks at full scale");
    run(m, pk, 60, 0.5);
    expect(std::fabs(m.levelDb(0)) < 0.01f && std::fabs(m.levelDb(1)) < 0.01f, "0 dBFS sine reads 0 dB");
    expect(!m.clipping(), "0 dBFS sine: no clip");
  }
  {
    LevelMeter m;
    run(m, sinePeak(0.5011872f, 800), 60, 0.5);
    expect(std::fabs(m.levelDb(0) + 6.0f) < 0.02f, "-6 dBFS sine reads -6 dB");
    expect(!m.clipping(), "-6 dBFS sine: no clip");
  }
  {  // Clip only above 0 dBFS, held for a while, then out.
    LevelMeter m;
    m.update(1.0001f, 0.2f, 1.0f / 60.0f);
    expect(m.clipping(), "a sample above 0 dBFS lights the clip lamp");
    run(m, 0.5f, 60, LevelMeter::kClipSeconds + 0.1);
    expect(!m.clipping(), "the clip lamp goes out after its hold time");
  }
  {  // Attack is instant at any frame rate.
    LevelMeter m;
    m.update(0.25f, 0.25f, 1.0f / 30.0f);
    expect(std::fabs(m.levelDb(0) - LevelMeter::toDb(0.25f)) < 1e-4f, "attack: one frame reaches the peak");
  }
  {  // Release: the same fall in dB per second at 30, 60, 144 and 240 frames per second.
    const double rates[] = {30, 60, 144, 240};
    float after[4] = {};
    for (int i = 0; i < 4; ++i) {
      LevelMeter m;
      run(m, 1.0f, rates[i], 0.2);
      run(m, 0.0f, rates[i], 0.5);
      after[i] = m.levelDb(0);
    }
    bool same = true;
    for (float v : after) same = same && std::fabs(v - after[0]) < 0.05f;
    expect(same, "release: decay is independent of the frame rate");
    expect(std::fabs(after[0] + LevelMeter::kReleaseDbPerSec * 0.5f) < 0.05f, "release: falls at the release rate in dB per second");
    LevelMeter m;
    run(m, 1.0f, 60, 0.2);
    run(m, 0.0f, 60, 4.0);
    expect(m.atRest(), "release: after a loud note the meter comes fully to rest");
  }
  {  // Hold: per channel, kept for its hold time, then falls.
    LevelMeter m;
    run(m, 0.0f, 60, 0.1);
    m.update(0.5f, 0.0f, 1.0f / 60.0f);
    run(m, 0.0f, 60, 1.0);
    expect(std::fabs(m.holdDb(0) - LevelMeter::toDb(0.5f)) < 1e-3f && m.levelDb(0) < m.holdDb(0) - 10.0f, "hold: the peak marker stays while the bar falls");
    expect(m.holdDb(1) == LevelMeter::kFloorDb, "hold: the other channel keeps its own (silent) hold");
    m.update(0.0f, 0.9f, 1.0f / 60.0f);
    expect(std::fabs(m.holdDb(0) - LevelMeter::toDb(0.5f)) < 1e-3f, "hold: a peak on the right does not reset the left hold");
    run(m, 0.0f, 60, 2.0);
    expect(m.holdDb(0) < LevelMeter::toDb(0.5f) - 10.0f, "hold: the marker falls after the hold time");
  }
  {  // Bad frame times never break it.
    LevelMeter m;
    m.update(0.5f, 0.5f, -1.0f);
    m.update(0.0f, 0.0f, std::nanf(""));
    expect(std::isfinite(m.levelDb(0)) && std::fabs(m.levelDb(0) - LevelMeter::toDb(0.5f)) < 1e-3f, "bad frame time: no change, no NaN");
  }
}

// The engine's master peak: the master bus (what the mixer master meter shows),
// taken by the reader, so it falls back to silence instead of sticking at the
// loudest moment ever played.
void testEnginePeak() {
  Document d(makeEmptySong());
  const ObjectId c = d.addChannel("syn", "gl.synth");
  ProgramBuilder b;
  AudioEngine eng(44100);
  eng.setProgram(b.build(d.song()));
  std::vector<float> L(2048), R(2048);
  eng.noteOn(c, 69, 0.9f);
  eng.process(L.data(), R.data(), 2048);
  float outPk = 0;
  for (float v : L) outPk = std::max(outPk, std::fabs(v));
  const float pl = eng.takeMasterPeakL(), pr = eng.takeMasterPeakR();
  float busPk = -1;
  if (auto rt = b.mixerRuntime(d.song().masterId())) busPk = rt->peakL.exchange(0);
  expect(pl > 0.01f && pr > 0.01f, "engine: a playing note moves the master peak");
  expect(std::fabs(pl - busPk) < 1e-6f, "engine: the OUT peak equals the mixer master meter's peak");
  expect(std::fabs(pl - outPk) < 1e-6f, "engine: the OUT peak is the master output");
  expect(eng.takeMasterPeakL() == 0.0f, "engine: reading takes the peak (no stuck value)");
  eng.noteOff(c, 69);
  for (int i = 0; i < 60; ++i) eng.process(L.data(), R.data(), 2048);
  (void)eng.takeMasterPeakL(), (void)eng.takeMasterPeakR();
  eng.process(L.data(), R.data(), 2048);
  expect(eng.takeMasterPeakL() < 1e-4f && eng.takeMasterPeakR() < 1e-4f, "engine: after the release the master peak is silent again");
}

void testTemplates() {
  auto ship = scanTemplates({{"assets/templates", false}});
  bool sketch = false, songwriting = false;
  for (const auto& t : ship) {
    sketch = sketch || t.name == "Song sketch";
    songwriting = songwriting || t.category == "Songwriting";
  }
  expect(!ship.empty(), "templates: the shipped templates are found");
  expect(!sketch && !songwriting, "templates: Song sketch (and its Songwriting folder) are gone");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("j143");
  testScale();
  testBallistics();
  testEnginePeak();
  testTemplates();
  return finish();
}
