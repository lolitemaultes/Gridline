// Gridline — two clips of one pattern back to back, the long note ending where the next clip's same
// note starts. Built-in instruments follow notes by their id, so the new note sounds in every one;
// the MIDI Out channel goes by the key at the receiving end, so it ends the old note first.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "../../src/core/Document.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Render.h"

using namespace gl;

namespace {

constexpr int kRate = 48000;
constexpr int kBar = 96000;  // one bar at 120 BPM

enum class Lead {
  None,     // only the second clip
  Clip,     // the first clip right before it
  OneNote,  // one note over both bars, no second note at all
};

// Peak level in the body of the second bar.
float secondBar(const PluginState& plugin, Lead lead) {
  Document d(makeEmptySong());
  const Tick bar = d.song().barTicks();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("x", plugin.type);
  if (lead == Lead::OneNote) {
    d.setPatternLength(pat, 2, 16);
    d.addNote(pat, ch, Note{0, 2 * bar, 60, 100});
    d.addClip(pat, 0, 0);
  } else {
    d.addNote(pat, ch, Note{0, bar, 60, 100});
    if (lead == Lead::Clip) d.addClip(pat, 0, 0);
    d.addClip(pat, 0, bar);
  }
  Song s = d.song();
  s.tempo = 120;
  for (Channel& c : s.channels)
    if (c.id == ch) c.plugin = plugin;
  RenderOptions o;
  o.sampleRate = kRate;
  AudioBuffer out = renderSong(s, o);
  float pk = 0;
  for (size_t i = kBar + 2400; i < kBar + 48000 && i < out.frames; ++i) pk = std::max(pk, std::fabs(out.plane(0)[i]));
  return pk;
}

void testBuiltIns() {
  int checked = 0;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.kind != PluginKind::Generator || info.usesSample || info.type == "gl.midiout") continue;
    for (size_t k = 0; k <= info.presets.size(); ++k) {
      const PluginState plugin = k == 0 ? defaultPluginState(info.type) : presetState(info, k - 1);
      const std::string name = k == 0 ? info.type : info.type + " / " + info.presets[k - 1].name;
      const float alone = secondBar(plugin, Lead::None);
      if (alone < 1e-4f) continue;  // silent here (needs a sample, a kit, a sound file; a short sound)
      const float after = secondBar(plugin, Lead::Clip);
      ++checked;
      // A mono legato sound may carry on from the first note instead of starting again, so it may
      // be as quiet as one note held that long; a note ended at once is far below both.
      const float held = after > 0.25f * alone ? alone : secondBar(plugin, Lead::OneNote);
      expect(after > 0.25f * std::min(alone, held), (name + ": the second clip's note sounds after the first (" + std::to_string(after) + ", alone " + std::to_string(alone) + ", one held note " + std::to_string(held) + ")").c_str());
    }
  }
  expect(checked >= 5, ("built-in instruments checked: " + std::to_string(checked)).c_str());
}

void testMidiOut() {
  auto g = createGenerator("gl.midiout");
  expect(g != nullptr, "MIDI Out exists");
  if (!g) return;
  MidiOutRing ring;
  g->setMidiOut(&ring);
  PluginState st = defaultPluginState("gl.midiout");
  ProcessContext c;
  c.sampleRate = kRate;
  std::vector<float> L(256), R(256);
  const auto run = [&] { g->process(st, ChannelAux{}, c, L.data(), R.data(), 256); };
  const auto drain = [&] {
    std::string t;
    for (MidiOutRing::Msg m; ring.pop(m);)
      if ((m.s & 0xF0) == 0x90 || (m.s & 0xF0) == 0x80) t += std::string((m.s & 0xF0) == 0x90 ? "on" : "off") + std::to_string(m.a) + " ";
    return t;
  };
  g->noteOn(1, 60, 0.8f, 0);
  run();
  expect(drain() == "on60 ", "MIDI Out: the first note");
  g->noteOn(2, 60, 0.8f, 10);  // the order the engine sends them at a junction
  g->noteOff(1, 10);
  run();
  const std::string junction = drain();
  expect(junction == "off60 on60 ", ("MIDI Out: the old note ends, then the new one starts [" + junction + "]").c_str());
  g->noteOff(2, 0);
  run();
  const std::string end = drain();
  expect(end == "off60 ", ("MIDI Out: the new note ends when it should, once [" + end + "]").c_str());
  g->noteOn(3, 60, 0.8f, 0);
  g->noteOn(4, 62, 0.8f, 0);
  g->noteOff(3, 0);
  g->noteOff(4, 0);
  run();
  const std::string two = drain();
  expect(two == "on60 on62 off60 off62 ", ("MIDI Out: other keys are left alone [" + two + "]").c_str());
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("note_retrigger");
  testBuiltIns();
  testMidiOut();
  return finish();
}
