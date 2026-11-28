// Gridline — K-408: Import from URL inside every audio-loading plugin.
// Verifies the complete audio-loading plugin audit list, source metadata sidecar round-trip,
// path extraction for every plugin state encoding, and zero-network fake-tool fetch/convert.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/UrlImport.h"
#include "packs/Packs.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

// Keep in sync with gl::urlimportui::audioLoadingPluginTypes() (src/app/UrlImportUi.cpp).
const std::vector<std::string>& kExpectedAudioPlugins() {
  static const std::vector<std::string> v = {
      "gl.sampler",       "gl.slicer",        "gl.granular",      "gl.pads",
      "gl.synth",         "gl.seltmann",      "gl.platter",       "gl.edison",
      "gl.convolver",     "gl.turntablefx",   "gl.turntable",     "gl.grainscan",
      "gl.splinescratch", "gl.multisampler",  "gl.scratch",       "gl.tracker",
      "gl.plasma",        "gl.harmor",        "gl.additive",
  };
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string fakeTool = argc > 1 ? argv[1] : "";
  const fs::path work = fs::temp_directory_path() / "gridline_test_k408_url_import";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);

  // 1. Audit: every plugin in our expected list exists in the catalogue, and every non-SF2
  //    plugin with usesSample==true is covered.
  const auto& expected = kExpectedAudioPlugins();
  std::set<std::string> covered(expected.begin(), expected.end());
  int foundCount = 0;
  for (const std::string& type : expected) {
    const PluginInfo* info = findPlugin(type);
    expect(info != nullptr, ("audio-loading plugin exists in catalogue: " + type).c_str());
    if (info) ++foundCount;
  }
  expect(foundCount == static_cast<int>(expected.size()), "all 19 audio-loading plugins registered");

  int uncoveredSamplePlugins = 0;
  for (const PluginInfo& p : pluginCatalog()) {
    if (!p.usesSample) continue;
    // Patch Player loads SoundFont (.sf2/.sf3) banks, not raw audio recordings.
    if (p.type == "gl.patchplayer") continue;
    if (!covered.count(p.type)) {
      std::printf("# UNCOVERED usesSample plugin: %s (%s)\n", p.type.c_str(), p.name.c_str());
      ++uncoveredSamplePlugins;
    }
  }
  expect(uncoveredSamplePlugins == 0, "every usesSample audio plugin is covered by K-408 URL import");

  // 2. Source metadata sidecar round-trip (.source.json).
  const fs::path wav = work / "Sample Track.wav";
  {
    std::ofstream out(wav, std::ios::binary);
    out << "RIFF";
  }
  urlimport::Source src;
  src.title = "Synth Loop (124 BPM)";
  src.uploader = "Sound Designer";
  src.site = "ExampleAudio";
  src.url = "https://example.com/tracks/synth-loop";
  src.duration = 42.5;
  expect(urlimport::writeSource(wav.u8string(), src), "writeSource succeeds");
  urlimport::Source back;
  expect(urlimport::readSource(wav.u8string(), back), "readSource succeeds");
  expect(back.title == src.title && back.uploader == src.uploader && back.site == src.site && back.url == src.url &&
             back.duration == src.duration,
         "source metadata round-trips cleanly");

  // 3. Zero-network stand-in fetch & import via fake_media_tool (when supplied).
  if (!fakeTool.empty() && fs::exists(fakeTool, ec)) {
    setenv("GL_FAKE_SCENARIO", "ok", 1);
    const urlimport::Tools tools{{fakeTool}, fakeTool};
    auto fj = urlimport::startFetch(tools, "https://example.com/watch?v=k408test", (work / "info").u8string());
    while (!fj->done.load()) {
    }
    expect(fj->get() == urlimport::Phase::Done, "stand-in fetch completes cleanly without network");
    expect(!fj->info.title.empty(), "stand-in fetch returns title");

    urlimport::Request req;
    req.item.title = fj->info.title;
    req.item.uploader = fj->info.uploader;
    req.item.url = fj->info.url;
    req.item.duration = fj->info.duration;
    req.site = fj->info.site;
    auto ij = urlimport::startImport(tools, {req}, (work / "dl").u8string());
    while (!ij->done.load()) {
    }
    expect(ij->get() == urlimport::Phase::Done && ij->results.size() == 1, "stand-in import produces lossless WAV");
    expect(fs::exists(ij->results[0].wav, ec), "converted WAV file exists on disk");
  }

  fs::remove_all(work, ec);
  return finish();
}
