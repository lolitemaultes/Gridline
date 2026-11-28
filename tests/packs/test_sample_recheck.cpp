// Gridline - a sample file that failed to load is not given up for good (the sample cache's failed
// list): once the file is there, or replaced by one that reads, the sampler and audio clips play it
// without reopening anything.
//   * the cache looks at failed files only when asked (the app asks each frame), at most once per
//     interval, and moves its generation only when a file changed (programs build again then);
//   * a file still missing, or unchanged, stays failed (no decode attempt, no generation move);
//   * a Sampler channel's prepared data and an audio clip's source get the sample once it arrives;
//   * a file that was there but unreadable plays once a readable file replaces it.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/RemoteFs.h"
#include "../../src/core/Song.h"
#include "../../src/engine/ClipSource.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void writeWav(const fs::path& p, int frames, double freq, int rate = 22050) {
  std::ofstream o(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
  o.write("RIFF", 4);
  u32(36 + static_cast<uint32_t>(frames) * 2);
  o.write("WAVEfmt ", 8);
  u32(16), u16(1), u16(1), u32(static_cast<uint32_t>(rate)), u32(static_cast<uint32_t>(rate) * 2), u16(2), u16(16);
  o.write("data", 4);
  u32(static_cast<uint32_t>(frames) * 2);
  for (int i = 0; i < frames; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(9000 * std::sin(2 * M_PI * freq * i / rate))));
}

size_t framesOf(const SamplePtr& s) { return s ? s->frames() : 0; }

}  // namespace

int main() {
  const fs::path home = fs::u8path(gl::test::hermeticTestEnv("sample_recheck"));
  std::error_code ec;
  const fs::path dir = home / "samples later";
  fs::create_directories(dir, ec);
  SampleCache& cache = sampleCache();

  // ---- the cache itself ----
  {
    const fs::path f = dir / "arrives.wav";
    expect(!cache.get(f.u8string()), "a file that is not there does not load");
    const uint64_t g0 = cache.generation();
    expect(cache.recheckFailed(0) == 0 && cache.generation() == g0, "still missing: nothing changes, the generation stays");
    expect(!cache.get(f.u8string()), "and it still reads as missing");
    writeWav(f, 3000, 220);
    expect(cache.recheckFailed(0) == 1 && cache.generation() != g0, "once it is there, the cache forgets the failure and its generation moves");
    expect(framesOf(cache.get(f.u8string())) == 3000, "the next get reads it");
    // At most once per interval.
    const fs::path f2 = dir / "second.wav";
    expect(!cache.get(f2.u8string()), "another missing file");
    expect(cache.recheckFailed(0) == 0, "(looked at now)");
    writeWav(f2, 1500, 330);
    expect(cache.recheckFailed(0.5) == 0, "within half a second of the last look, nothing is looked at");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    expect(cache.recheckFailed(0.5) == 1 && framesOf(cache.get(f2.u8string())) == 1500, "after it, the file is found");
  }

  // ---- a Sampler channel ----
  {
    const fs::path f = dir / "sampler.wav";
    PluginState st = defaultPluginState("gl.sampler");
    st.data = f.u8string();
    ChannelAux aux;
    prepareChannelAux(st, aux);
    expect(!aux.sample(), "a Sampler whose file is missing has no sample");
    writeWav(f, 4000, 440);
    expect(cache.recheckFailed(0) == 1, "the file arrives");
    ChannelAux aux2;
    prepareChannelAux(st, aux2);
    expect(framesOf(aux2.sample()) == 4000, "the Sampler's next build plays it");
  }

  // ---- an audio clip's channel ----
  {
    const fs::path f = dir / "clip.wav";
    Channel ch;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = f.u8string();
    expect(!clipSource(ch).ok(), "an audio clip whose file is missing has no sound");
    writeWav(f, 5000, 110);
    expect(cache.recheckFailed(0) == 1, "the file arrives");
    expect(clipSource(ch).ok() && framesOf(clipSource(ch).sample) == 5000, "the clip plays it");
  }

  // ---- a file that was there but could not be read, replaced by one that reads ----
  {
    const fs::path f = dir / "broken.wav";
    std::ofstream(f, std::ios::binary) << "not audio";
    expect(!cache.get(f.u8string()), "an unreadable file does not load");
    expect(cache.recheckFailed(0) == 0, "unchanged, it stays failed");
    writeWav(f, 2500, 550);
    expect(cache.recheckFailed(0) == 1 && framesOf(cache.get(f.u8string())) == 2500, "replaced by a readable file, it plays");
  }

  // ---- a file on a web or network drive is never looked at ----
  {
    // (a local folder counted as a network drive, as the remote file tests do)
    const fs::path share = home / "share";
    fs::create_directories(share, ec);
    remotefs::setTestPrefixes({share.u8string()});
    const fs::path f = share / "later.wav";
    expect(!cache.get(f.u8string()), "a file on the drive that is not there does not load");
    writeWav(f, 1000, 220);
    expect(cache.recheckFailed(0) == 0, "a failed file on a network drive is not looked at (the drive may not answer)");
    remotefs::setTestPrefixes({});
  }

  return finish();
}
