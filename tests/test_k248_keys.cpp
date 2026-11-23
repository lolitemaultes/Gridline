// Gridline: K-248, the Waveform key context (core/Keymap.h): warp mode is W in Studio and Alt+W in
// Clips, Smooth and Tracker; the spectrum is Shift+W everywhere (Loupe's W before); no chord of the
// Waveform context is given twice in it, nor to a global command, in any set (Command told apart or
// not); the Tracker sample editor's Wave preview is Alt+P and its warp mode Alt+W, clashing with none
// of its own keys.
#include <map>
#include <set>
#include <string>
#include "Check.h"
#include "core/Keymap.h"
#include "core/TrackerKeys.h"

using namespace gl::keys;

namespace {

bool has(const Keymap& k, const char* id, Key key, uint8_t mods) {
  for (const Chord& c : k.bindings(id))
    if (c.key == key && c.mods == mods) return true;
  return false;
}

}  // namespace

int main() {
  Keymap studio, clips, smooth, smoothMac, tracker;
  studio.setProfile(Profile::Studio, false);
  clips.setProfile(Profile::Live, false);
  smooth.setProfile(Profile::Logic, false);
  smoothMac.setProfile(Profile::Logic, true);
  tracker.setProfile(Profile::Tracker, false);
  expect(has(studio, "wave.warpMode", Key::W, 0), "Studio: W is warp mode");
  expect(has(clips, "wave.warpMode", Key::W, kAlt), "Clips: Alt+W is warp mode");
  expect(has(smooth, "wave.warpMode", Key::W, kAlt) && has(smoothMac, "wave.warpMode", Key::W, kAlt), "Smooth: Alt+W is warp mode");
  expect(has(tracker, "wave.warpMode", Key::W, kAlt), "Tracker: Alt+W is warp mode");
  for (const Keymap* k : {&studio, &clips, &smooth, &smoothMac, &tracker}) expect(has(*k, "wave.spectrum", Key::W, kShift), "Shift+W: the spectrum");
  const Command* w = findCommand("wave.warpMode");
  expect(w && w->context == Context::Waveform, "warp mode is a Waveform command");
  expect(std::string(contextName(Context::Waveform)) == "Waveform", "the context's name");
  // No clash in the Waveform context, or between it and a global command.
  const std::pair<const char*, const Keymap*> sets[] = {{"Studio", &studio}, {"Clips", &clips}, {"Smooth", &smooth}, {"Smooth (Cmd)", &smoothMac}, {"Tracker", &tracker}};
  for (const auto& [name, k] : sets) {
    std::map<std::string, std::string> wave, global;
    int clash = 0;
    for (const Command& c : commands())
      for (const Chord& ch : k->bindings(c.id)) {
        const std::string f = format(ch);
        if (c.context == Context::Waveform) {
          if (wave.count(f)) ++clash, std::printf("# %s: %s %s and %s\n", name, f.c_str(), wave[f].c_str(), c.id);
          wave[f] = c.id;
        } else if (c.context == Context::Global) {
          global[f] = c.id;
        }
      }
    for (const auto& [f, id] : wave)
      if (global.count(f)) ++clash, std::printf("# %s: %s %s and the global %s\n", name, f.c_str(), id.c_str(), global[f].c_str());
    expect(clash == 0, (std::string(name) + ": no Waveform chord is given twice").c_str());
  }
  // The Tracker sample editor: Wave preview on Alt+P, warp mode on Alt+W, no chord twice in the area.
  std::map<std::string, std::string> seen;
  int dup = 0;
  bool play = false, warp = false;
  for (const gl::trackerkeys::Row& r : gl::trackerkeys::rows()) {
    if (std::string(r.id).rfind("sample.", 0) != 0) continue;
    for (const std::string& c : r.chords) {
      if (seen.count(c) && seen[c] != r.id) ++dup, std::printf("# sample editor: %s %s and %s\n", c.c_str(), seen[c].c_str(), r.id);
      seen[c] = r.id;
    }
    if (std::string(r.id) == "sample.play") play = !r.chords.empty() && r.chords[0] == "Alt+P";
    for (const std::string& c : r.chords) warp = warp || c == "Alt+W";
  }
  expect(play, "Tracker sample editor: Alt+P plays the sample (the Wave preview)");
  expect(!warp, "Tracker sample editor: Alt+W is free for warp mode");
  expect(dup == 0, "Tracker sample editor: no chord twice");
  return finish();
}
