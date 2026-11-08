// Gridline - K-133: project bones over an earlier export replace nothing until every new file was
// written (a Cancel or a failure leaves the earlier export as it was); the render worker's sign of
// life (a worker that stops moving on is seen once, and seen moving again).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectFiles.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/WorkerWatch.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& what) { ::expect(ok, what.c_str()); }

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void spit(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << text;
}

// Every file under a folder with its content, by relative name.
std::map<std::string, std::string> files(const fs::path& dir) {
  std::map<std::string, std::string> out;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return out;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) out[fs::relative(it->path(), dir, ec).generic_u8string()] = slurp(it->path());
  return out;
}

bool anyNamed(const std::map<std::string, std::string>& f, const std::string& part) {
  for (const auto& [n, text] : f)
    if (n.find(part) != std::string::npos) return true;
  return false;
}

Song tune(const fs::path& dir, int pitch, double tempo) {
  Song s = makeEmptySong();
  s.channels.clear();
  s.tempo = tempo;
  spit(dir / "src" / "one.wav", std::string(1 << 16, 'a'));
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = (dir / "src" / "one.wav").u8string();
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  Pattern p;
  p.id = s.allocateId();
  p.name = "Verse";
  Note n;
  n.pitch = pitch;
  n.length = 96;
  p.notes[c.id].push_back(n);
  s.patterns.push_back(p);
  return s;
}

void testBonesOverEarlier(const fs::path& base) {
  const fs::path dir = base / "bones";
  const fs::path out = dir / "out" / "Tune bones";
  const Song first = tune(dir, 60, 120), second = tune(dir, 67, 97);
  {
    const BonesReport r = exportProjectBones(first, out.u8string(), "Tune");
    expect(r.files.error.empty() && r.midiFiles >= 1 && r.presets == 1, "the first export (" + r.files.error + ")");
  }
  const auto before = files(out);
  expect(before.count("Tune" + std::string(kProjectExt)) == 1, "the project file is there");
  expect(anyNamed(before, "MIDI/") && anyNamed(before, "Presets/"), "with its MIDI files and presets");

  {  // Cancelled while the MIDI files and presets are written: the earlier export is untouched.
    std::atomic<bool> flag{false};
    ExportControl c;
    c.cancel = &flag;
    c.progress = [&](double, const std::string& what) {
      if (what.find("MIDI") != std::string::npos) flag = true;
    };
    const BonesReport r = exportProjectBones(second, out.u8string(), "Tune", &c);
    expect(r.files.cancelled, "a second export cancelled at the MIDI files");
    const auto after = files(out);
    expect(after == before, "leaves every file of the earlier export as it was (" + std::to_string(after.size()) + " files, " +
                                std::to_string(before.size()) + " before)");
  }
  {  // A preset cannot be put in place (a folder stands where its file goes): an error, nothing replaced.
    const fs::path preset = out / "Presets" / "gl.sampler" / "Lead.json";
    expect(fs::is_regular_file(preset), "the preset is where it should be");
    std::error_code ec;
    fs::remove(preset, ec);
    fs::create_directories(preset / "in the way", ec);
    auto blocked = before;
    blocked.erase("Presets/gl.sampler/Lead.json");
    const BonesReport r = exportProjectBones(second, out.u8string(), "Tune");
    expect(!r.files.error.empty(), "a second export that cannot write a preset reports it");
    const auto after = files(out);
    expect(after == blocked, "and replaced nothing of the earlier export (" + std::to_string(after.size()) + " files)");
    fs::remove_all(preset, ec);
  }
  {  // Then it can: every file is the new one, and no staging folder stays.
    const BonesReport r = exportProjectBones(second, out.u8string(), "Tune");
    expect(r.files.error.empty() && !r.files.cancelled, "the second export (" + r.files.error + ")");
    const auto after = files(out);
    const std::string project = "Tune" + std::string(kProjectExt);
    expect(after.count(project) && after.at(project) != before.at(project), "the project is the new one");
    bool midiNew = false;
    for (const auto& [n, text] : after)
      if (n.rfind("MIDI/", 0) == 0 && before.count(n) && before.at(n) != text) midiNew = true;
    expect(midiNew, "the MIDI files are the new ones");
    expect(fs::is_regular_file(out / "Presets" / "gl.sampler" / "Lead.json"), "the preset is back");
    expect(!anyNamed(after, "staging") && !fs::exists(out / ".gridline-bones-staging"), "no staging folder stays");
  }
  {  // A new place: as before, everything there at the end.
    const fs::path fresh = dir / "out" / "Fresh bones";
    const BonesReport r = exportProjectBones(second, fresh.u8string(), "Tune");
    expect(r.files.error.empty() && fs::is_regular_file(fresh / ("Tune" + std::string(kProjectExt))) && fs::is_directory(fresh / "MIDI"),
           "an export into a new folder");
  }
}

void testStallWatch() {
  StallWatch w(1000);
  WorkerBeat b;
  w.start(0, b.count());
  expect(w.update(500, b.count()) == StallWatch::Event::None, "silent for less than the limit: nothing");
  b.beat();
  expect(w.update(900, b.count()) == StallWatch::Event::None, "a beat: nothing");
  expect(w.update(1800, b.count()) == StallWatch::Event::None, "counted from the last beat");
  expect(w.update(1900, b.count()) == StallWatch::Event::Stalled && w.stalled(), "silent for the limit: stalled");
  expect(w.update(5000, b.count()) == StallWatch::Event::None && w.stalled(), "reported once");
  expect(w.silentMs(5000) == 4100, "how long it has been silent");
  b.beat();
  expect(w.update(5100, b.count()) == StallWatch::Event::Moving && !w.stalled(), "a beat after that: moving again");
  expect(w.update(5200, b.count()) == StallWatch::Event::None, "and nothing more");
  w.start(10000, b.count());
  expect(!w.stalled() && w.update(10999, b.count()) == StallWatch::Event::None, "a new job counts from its start");
  expect(StallWatch::defaultLimitMs() > 0, "a default limit");
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path base = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("k133_units"));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  testBonesOverEarlier(base);
  testStallWatch();
  fs::remove_all(base, ec);
  return finish();
}
