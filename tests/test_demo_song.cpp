// Gridline — J-135 tests: the "Gridline" demo song. It loads (quickly) from the copy compiled into
// the programs, that copy matches assets/demo/Gridline.gridline.json, it has its eight sections as
// named markers, and a full render has no NaN / Inf, no clipping, -9.5 LUFS integrated (+-0.6) and a
// true peak at or below -1 dBTP. Renders of the same range are identical (deterministic).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Demo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "engine/Loudness.h"
#include "engine/Render.h"

using namespace gl;

namespace {

// True peak (dBTP) by 4x oversampling with a windowed-sinc interpolator (BS.1770-4 annex 2 style).
double truePeakDb(const AudioBuffer& b) {
  constexpr int kOver = 4, kTaps = 48;  // 12 input samples per phase
  std::vector<std::vector<double>> phase(kOver, std::vector<double>(kTaps / kOver));
  const double pi = 3.14159265358979323846;
  for (int p = 0; p < kOver; ++p)
    for (int k = 0; k < kTaps / kOver; ++k) {
      double n = (k * kOver + p) - (kTaps - 1) / 2.0;  // position in output samples
      double x = n / kOver;
      double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
      double w = 0.5 - 0.5 * std::cos(2 * pi * (k * kOver + p + 0.5) / kTaps);
      phase[static_cast<size_t>(p)][static_cast<size_t>(k)] = sinc * w;
    }
  double peak = 0;
  const int half = kTaps / kOver;
  for (size_t c = 0; c < b.channels; ++c) {
    const float* x = b.data.data() + c * b.frames;
    for (size_t i = static_cast<size_t>(half); i < b.frames; ++i)
      for (int p = 0; p < kOver; ++p) {
        double acc = 0;
        for (int k = 0; k < half; ++k) acc += phase[static_cast<size_t>(p)][static_cast<size_t>(k)] * x[i - static_cast<size_t>(k)];
        peak = std::max(peak, std::abs(acc));
      }
  }
  return peak > 0 ? 20 * std::log10(peak) : -144.0;
}

bool finiteBuffer(const AudioBuffer& b) {
  return std::all_of(b.data.begin(), b.data.end(), [](float v) { return std::isfinite(v); });
}

}  // namespace

int main() {
  // --- load
  auto t0 = std::chrono::steady_clock::now();
  Song song;
  std::string err;
  bool ok = loadDemoTrack(song, &err);
  double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# load %.1f ms%s%s\n", loadMs, err.empty() ? "" : ": ", err.c_str());
  expect(ok, "the embedded demo song loads");
  if (!ok) return finish();
  expect(loadMs < 2000, "the demo song loads in well under 2 s");
  expect(song.info.title == "Gridline", "title is Gridline");
  expect(song.info.author == "Gridline", "author is Gridline");
  expect(song.info.genre == "Drum & Bass", "genre is Drum & Bass");
  expect(std::abs(song.tempo - 174.0) < 1e-9, "174 BPM");
  expect(makeDemoTrack().info.title == "Gridline", "makeDemoTrack returns the Gridline song");

  // --- the compiled-in copy is the shipped asset (rebuild after regenerating the asset)
  {
    std::ifstream is("assets/demo/Gridline.gridline.json", std::ios::binary);
    std::stringstream ss;
    ss << is.rdbuf();
    expect(is.good() || is.eof(), "assets/demo/Gridline.gridline.json is readable");
    expect(ss.str() == std::string(demoSongJson()), "the embedded demo song matches assets/demo/Gridline.gridline.json");
  }

  // --- the eight sections as named markers, in order
  {
    const std::vector<std::string> want = {"Intro", "Verse", "Chorus", "Verse 2", "Bridge", "Breakdown", "Chorus 2", "Outro"};
    std::vector<Marker> ms = song.markers;
    std::sort(ms.begin(), ms.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
    std::vector<std::string> got;
    for (const auto& m : ms) got.push_back(m.name);
    expect(got == want, "markers: Intro, Verse, Chorus, Verse 2, Bridge, Breakdown, Chorus 2, Outro");
    bool rising = true;
    for (size_t i = 1; i < ms.size(); ++i) rising = rising && ms[i].pos > ms[i - 1].pos;
    expect(rising && !ms.empty() && ms.front().pos == 0, "markers start at 0 and rise");
  }

  // --- deterministic: the same range renders identically twice
  {
    RenderJob job;
    job.scope = RenderScope::Range;
    job.start = 64 * song.barTicks();
    job.end = 66 * song.barTicks();
    RenderResult a = renderAudio(song, job), b = renderAudio(song, job);
    expect(a.error.empty() && a.audio.frames > 0 && a.audio.data == b.audio.data, "two renders of bars 65-66 are identical");
  }

  // --- full render: no NaN, no clipping, loudness and true peak on target
  {
    RenderJob job;
    job.tail = TailMode::Leave;
    job.maxTailSeconds = 15;
    job.threads = 4;
    auto r0 = std::chrono::steady_clock::now();
    RenderResult r = renderAudio(song, job);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
    expect(r.error.empty(), "the full song renders");
    const double len = static_cast<double>(r.audio.frames) / r.sampleRate;
    expect(len > 210 && len < 300, "length between 3:30 and 5:00 (with the tail)");
    expect(finiteBuffer(r.audio), "no NaN or Inf samples");
    const double peak = samplePeakDb(r.audio), lufs = integratedLoudness(r.audio, r.sampleRate);
    const double tp = truePeakDb(r.audio);
    std::printf("# render %.1f s for %.1f s of audio: %.2f LUFS, sample peak %.2f dBFS, true peak %.2f dBTP\n", secs, len,
                lufs, peak, tp);
    expect(peak < -0.5, "no clipping (sample peak below -0.5 dBFS)");
    expect(lufs > -10.1 && lufs < -8.9, "integrated loudness -9.5 LUFS +-0.6");
    expect(tp <= -0.9, "true peak at or below -1 dBTP (0.1 dB measuring tolerance)");
  }
  return finish();
}
