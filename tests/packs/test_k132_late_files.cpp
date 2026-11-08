// Gridline - K-132: files that arrive after a project opened (a collaborator's wavetable, sample,
// impulse response or noise still on its way). Plasma's prepared data is read again once the file
// is there (the collab session tells the sample cache, which starts a rebuild), with no reopen.
#include <cmath>
#include <filesystem>
#include <string>
#include "../../src/core/AudioBuffer.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../Check.h"
#include "../Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;
namespace pl = gl::packs::plasma;

namespace {

void writeTone(const std::string& path, size_t frames, double cycle) {
  AudioBuffer b(1, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = static_cast<float>(0.5 * std::sin(6.283185307179586 * static_cast<double>(i) / cycle));
  writeWav(path, b, 44100, 16);
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("k132_late");
  const std::string table = (fs::path(dir) / "Audio" / "late-table.wav").string();
  const std::string sample = (fs::path(dir) / "Audio" / "late-sample.wav").string();
  const std::string ir = (fs::path(dir) / "Audio" / "late-ir.wav").string();
  const std::string noise = (fs::path(dir) / "Audio" / "late-noise.wav").string();
  fs::create_directories(fs::path(table).parent_path());

  PluginState st = defaultPluginState(pl::kType);
  pl::Model m;
  pl::modelFromData(st.data, m);
  m.osc[0].table = "file:" + table;
  m.osc[1].sample = sample;
  m.fxExtra[0].ir = ir;
  m.noiseFile = noise;
  st.data = pl::modelToData(m);

  // Opened before the files are there: nothing to read yet. (The first build's failed reads move
  // the sample cache's generation once, so the app builds once more; from then on it is stable.)
  auto first = pl::buildAux(st);
  auto before = pl::buildAux(st);
  expect(first && before, "prepared while the files are missing");
  expect(before && !before->osc[0].mip && !before->osc[1].sample && !before->irFile[0] && !before->noiseFile, "files not there yet: nothing loaded");
  expect(before && before->incomplete, "files not there yet: the prepared data says it is incomplete");
  // Nothing arrived: a rebuild gives the same prepared data (no work for nothing).
  expect(pl::buildAux(st) == before, "nothing arrived: the same prepared data");

  // The files arrive; the session tells the sample cache about each (Session::finishFile).
  writeTone(table, 2048 * 4, 2048);
  writeTone(sample, 22050, 100);
  writeTone(ir, 4410, 10);
  writeTone(noise, 44100, 7);
  for (const std::string& f : {table, sample, ir, noise}) sampleCache().forget(f);
  auto after = pl::buildAux(st);
  expect(after && after != before, "files arrived: built again");
  expect(after && after->osc[0].mip, "a late wavetable file loads when it arrives");
  expect(after && after->osc[1].sample && after->osc[1].sample->frames() == 22050, "a late sample loads when it arrives");
  expect(after && after->irFile[0], "a late impulse response loads when it arrives");
  expect(after && after->noiseFile, "a late noise file loads when it arrives");
  expect(after && !after->incomplete, "every file there: complete");
  expect(pl::buildAux(st) == after, "complete: kept from then on");

  // The sampler: its next build finds the late sample (no cache of its own).
  const std::string late = (fs::path(dir) / "Audio" / "late-sampler.wav").string();
  PluginState smp = defaultPluginState("gl.sampler");
  smp.data = late;
  ChannelAux a0;
  prepareChannelAux(smp, a0);
  expect(!a0.sample(), "sampler: its file not there yet");
  writeTone(late, 4410, 50);
  sampleCache().forget(late);
  ChannelAux a1;
  prepareChannelAux(smp, a1);
  expect(a1.sample() && a1.sample()->frames() == 4410, "sampler: a late sample plays from the next build");
  return finish();
}
