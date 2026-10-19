// Gridline — preset library (folders, data, chains) and FL preset (.fst) import
// on a synthetic file built here (no FL Studio content).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "../src/core/Presets.h"
#include "../src/core/flp/FlpPresets.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;

int main() {
  gl::test::hermeticTestEnv("presets");
  const fs::path root = fs::temp_directory_path() / "gridline_test_presets";
  std::error_code ec;
  fs::remove_all(root, ec);

  // Plugin presets in folders, with data.
  PluginState s = defaultPluginState("gl.sampler");
  s.params[0] = 0.25f;
  s.data = "/samples/kick.wav";
  expect(savePreset(root.string(), s, "FL Studio/Kits/Punchy kick", "FL Studio: test", false), "save preset in folders");
  PluginState u = defaultPluginState("gl.sampler");
  expect(savePreset(root.string(), u, "Mine"), "save top-level preset");
  auto list = listPresets(root.string(), "gl.sampler");
  expect(list.size() == 2 && list[0].name == "Mine" && list[0].folder.empty() && list[1].folder == "FL Studio/Kits",
         "listing: top level first, then folders");
  PluginState back;
  expect(loadPreset(list[1].file, "gl.sampler", back) && std::fabs(back.params[0] - 0.25f) < 1e-6f && back.data == s.data,
         "load restores params and data");
  expect(!loadPreset(list[1].file, "gl.reverb", back), "type mismatch is refused");
  expect(safePresetName("a/b:c?") == "a_b_c_", "unsafe characters replaced");

  // Effect chains.
  ChainPreset c;
  c.slots = {defaultPluginState("gl.eq"), PluginState{}, defaultPluginState("gl.reverb")};
  c.mix = {1.0f, 1.0f, 0.4f};
  expect(saveChain(root.string(), c, "FL Studio/Vocal chain"), "save chain");
  auto chains = listChains(root.string());
  ChainPreset cb;
  expect(chains.size() == 1 && loadChain(chains[0].file, cb) && cb.slots.size() == 3 && cb.slots[0].type == "gl.eq" &&
             cb.slots[1].type.empty() && std::fabs(cb.mix[2] - 0.4f) < 1e-6f,
         "chain round trip keeps empty slots and mix");

  // FL channel preset (.fst): a sampler channel with a sample path.
  std::vector<uint8_t> ev;
  auto text = [&](uint8_t id, const std::string& t) {
    std::vector<uint8_t> p;
    for (char ch : t) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    ev.push_back(id);
    ev.push_back(static_cast<uint8_t>(p.size()));
    ev.insert(ev.end(), p.begin(), p.end());
  };
  text(199, "21.0.0");
  ev.insert(ev.end(), {64, 0, 0, 21, 0});  // Channel.New 0, type sampler
  text(196, "/samples/snare.wav");
  std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0x20, 0, 1, 0, 96, 0, 'F', 'L', 'd', 't'};
  for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(ev.size() >> (8 * i)));
  f.insert(f.end(), ev.begin(), ev.end());
  const fs::path fst = root / "snare.fst";
  fs::create_directories(root, ec);
  {
    std::ofstream o(fst, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
  }
  flp::PresetResult r;
  expect(flp::importFlPreset(fst.string(), r, "/nonexistent") && r.kind == flp::PresetResult::Plugin && r.state.type == "gl.sampler" &&
             r.state.data == "/samples/snare.wav",
         "FL sampler channel preset → sampler preset with its sample");
  fs::remove_all(root, ec);
  return finish();
}
