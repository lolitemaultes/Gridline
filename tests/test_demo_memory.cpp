// K-154: playing the demo song holds its memory steady. The demo song plays through the realtime
// path (processInterleaved, device-sized blocks) for 120 s of song time, longer than the 10 s after
// which a user's computer ran out of memory; the process's peak resident memory may grow by at most
// 64 MB after the first 10 s (instruments warmed up). Also with odd block sizes and at 96 kHz.
#include <algorithm>
#include <cstdio>
#include <vector>
#include "Check.h"
#include "core/Demo.h"
#include "core/Platform.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

namespace {
void play(int rate, int block, double seconds) {
  gl::Song song = gl::makeDemoTrack();
  gl::ProgramBuilder builder;
  gl::AudioEngine eng(rate);
  eng.setProgram(builder.build(song));
  eng.setMode(gl::PlayMode::Song);
  eng.play();
  std::vector<float> buf(2 * static_cast<size_t>(block));
  const long long total = static_cast<long long>(seconds * rate), warm = 10LL * rate;
  double atWarm = 0;
  float peak = 0;
  for (long long done = 0; done < total; done += block) {
    eng.processInterleaved(buf.data(), block);
    for (float v : buf) peak = std::max(peak, v < 0 ? -v : v);
    if (done < warm && done + block >= warm) atWarm = gl::platform::peakRssMb();
  }
  const double end = gl::platform::peakRssMb();
  std::printf("demo at %d Hz, %d-frame blocks, %.0f s: peak memory %.1f MB after 10 s, %.1f MB at the end (+%.1f MB), output peak %.2f\n", rate, block,
              seconds, atWarm, end, end - atWarm, static_cast<double>(peak));
  expect(peak > 0.05f, "demo: the song is audible");
  expect(end - atWarm < 64.0, "demo: playing holds its memory steady (under 64 MB of growth after the first 10 s)");
}
}  // namespace

int main() {
  play(48000, 256, 120);
  play(44100, 441, 60);   // a block size that is no power of two
  play(96000, 1024, 40);  // a high rate, large device blocks
  return finish();
}
