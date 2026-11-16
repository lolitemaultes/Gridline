// Gridline — K-214 tests: Mackie Control (MCU) and HUI surfaces, driven by a simulated device
// (messages in, the bytes the surface sends back recorded per unit).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/ControllerScript.h"
#include "core/Document.h"
#include "core/MackieControl.h"
#include "core/MidiTemplates.h"
#include "core/Params.h"

using namespace gl;

namespace {

struct Device {
  std::vector<std::pair<int, std::vector<uint8_t>>> sent;
  mackie::Surface::Out out() {
    return [this](int unit, const std::vector<uint8_t>& b) { sent.push_back({unit, b}); };
  }
  bool saw(int unit, std::vector<uint8_t> b) const {
    return std::find(sent.begin(), sent.end(), std::make_pair(unit, b)) != sent.end();
  }
  // The text written at `offset` of a unit's scribble strip (last write wins).
  std::string text(int unit, int offset, size_t n) const {
    std::string screen(112, ' ');
    for (const auto& [u, b] : sent)
      if (u == unit && b.size() > 8 && b[0] == 0xF0 && b[5] == 0x12)
        for (size_t i = 7; i + 1 < b.size(); ++i)
          if (b[6] + i - 7 < screen.size()) screen[b[6] + i - 7] = static_cast<char>(b[i]);
    return screen.substr(static_cast<size_t>(offset), n);
  }
};

Document makeDoc(int inserts) {
  Document doc(makeEmptySong());
  while (static_cast<int>(doc.song().mixer.size()) < inserts + 1) doc.addMixerTrack();
  return doc;
}

double volNorm(const Document& d, int m) {
  auto desc = describeParam(d.song(), mixerParam(d.song().mixer[static_cast<size_t>(m)].id, MixerParam::Volume));
  return desc ? desc->toNormalized(d.song().mixer[static_cast<size_t>(m)].volume) : -1;
}

void testMcu() {
  Document doc = makeDoc(20);
  doc.editMixerTrack(doc.song().mixer[1].id, "name", [](MixerTrack& t) { t.name = "Drums"; });
  script::SimpleHost host(doc);
  Device dev;
  mackie::Surface s(host, mackie::Options{mackie::Protocol::Mcu, 1, false}, dev.out());
  expect(s.strips() == 16 && s.mixerOf(0) == 1 && s.mixerOf(15) == 16, "MCU + one extender: 16 strips on inserts 1..16");
  s.idle(0);
  expect(dev.text(0, 0, 7) == "Drums  " && dev.text(1, 0, 6) == "Ins 9 ", "scribble strips: names on the main unit and the extender");
  expect(dev.text(0, 56, 7) == "+0.0   ", "scribble strips: the level below the name");
  // Fader 1 to the top (14-bit pitch bend, channel 1).
  expect(s.onMidi(0, MidiMessage{0xE0, 0x7F, 0x7F}), "fader message used");
  expect(std::fabs(volNorm(doc, 1) - 1.0) < 1e-6, "fader 1 moves insert 1 to the top");
  // Extender fader 3 = strip 11 = insert 11.
  s.onMidi(1, MidiMessage{0xE2, 0x00, 0x40});
  expect(std::fabs(volNorm(doc, 11) - 8192.0 / 16383.0) < 1e-3, "extender fader 3 moves insert 11");
  // Master fader (channel 9).
  s.onMidi(0, MidiMessage{0xE8, 0, 0});
  expect(volNorm(doc, 0) < 1e-6, "master fader");
  // Motor feedback: a fader under the hand is not moved; let go, it follows the stored value.
  dev.sent.clear();
  s.onMidi(0, MidiMessage{0x90, 0x69, 0x7F});  // touch fader 2
  doc.setParam(mixerParam(doc.song().mixer[2].id, MixerParam::Volume), 0.2, false);
  s.idle(0.1);
  bool moved = false;
  for (const auto& [u, b] : dev.sent) moved = moved || (u == 0 && b[0] == 0xE1);
  expect(!moved, "a touched fader is not driven");
  s.onMidi(0, MidiMessage{0x90, 0x69, 0x00});
  s.idle(0.2);
  moved = false;
  for (const auto& [u, b] : dev.sent) moved = moved || (u == 0 && b[0] == 0xE1);
  expect(moved, "released, the fader moves to the insert's level");
  // Encoder: 3 steps clockwise, then 5 counter-clockwise.
  const float pan0 = doc.song().mixer[1].pan;
  s.onMidi(0, MidiMessage{0xB0, 0x10, 0x03});
  expect(doc.song().mixer[1].pan > pan0, "encoder clockwise pans right");
  s.onMidi(0, MidiMessage{0xB0, 0x10, 0x45});
  expect(doc.song().mixer[1].pan < pan0, "encoder counter-clockwise pans left");
  s.onMidi(0, MidiMessage{0x90, 0x20, 0x7F});
  expect(std::fabs(doc.song().mixer[1].pan) < 1e-6, "encoder push centres the pan");
  // Strip buttons and their LEDs.
  dev.sent.clear();
  s.onMidi(0, MidiMessage{0x90, 0x10, 0x7F});  // mute 1
  s.onMidi(0, MidiMessage{0x90, 0x10, 0x00});
  s.onMidi(0, MidiMessage{0x90, 0x09, 0x7F});  // solo 2
  s.onMidi(0, MidiMessage{0x90, 0x1A, 0x7F});  // select 3
  s.onMidi(0, MidiMessage{0x90, 0x03, 0x7F});  // arm 4
  expect(doc.song().mixer[1].muted && doc.song().mixer[2].solo && host.selectedMixer() == 3 && host.armed(4), "mute, solo, select, arm");
  s.idle(0.3);
  expect(dev.saw(0, {0x90, 0x10, 0x7F}) && dev.saw(0, {0x90, 0x09, 0x7F}) && dev.saw(0, {0x90, 0x1A, 0x7F}) && dev.saw(0, {0x90, 0x03, 0x7F}),
         "their LEDs light");
  expect(doc.history().canUndo(), "surface edits are undoable Document edits");
  // Banks: right by 16 (limited to the last full bank), channel shift by one.
  s.onMidi(0, MidiMessage{0x90, 0x2F, 0x7F});
  expect(s.mixerOf(0) == 5 && s.mixerOf(15) == 20, "bank right stops at the last 16 inserts");
  s.onMidi(0, MidiMessage{0x90, 0x30, 0x7F});
  expect(s.mixerOf(0) == 4, "channel left shifts by one");
  s.onMidi(0, MidiMessage{0x90, 0x2E, 0x7F});
  expect(s.mixerOf(0) == 1, "bank left back to insert 1");
  // Transport and the jog wheel.
  s.onMidi(0, MidiMessage{0x90, 0x5E, 0x7F});
  s.onMidi(0, MidiMessage{0x90, 0x5D, 0x7F});
  s.onMidi(0, MidiMessage{0x90, 0x5F, 0x7F});
  expect(host.actions.size() >= 3 && host.actions[host.actions.size() - 3] == RemoteAction::PlayPause &&
             host.actions[host.actions.size() - 2] == RemoteAction::Stop && host.actions.back() == RemoteAction::Record,
         "play, stop, record");
  host.pos = 0;
  s.onMidi(0, MidiMessage{0xB0, 0x3C, 0x02});
  expect(host.pos == 2 * doc.song().ppq, "jog wheel: two beats on");
  s.onMidi(0, MidiMessage{0x90, 0x5C, 0x7F});
  expect(host.pos == 6 * doc.song().ppq, "fast forward: a bar on");
  // Timecode: bar 2, beat 3 → the bar and beat digits.
  dev.sent.clear();
  host.pos = 5 * doc.song().ppq;
  s.idle(0.4);
  expect(dev.saw(0, {0xB0, 0x47, 0x32 | 0x40}) && dev.saw(0, {0xB0, 0x45, 0x32 | 0x40}), "timecode: bar 2 . beat 2");
  // Meters.
  dev.sent.clear();
  std::vector<float> lv(doc.song().mixer.size(), 0.f);
  lv[1] = 1.0f;
  s.idle(0.5, lv);
  expect(dev.saw(0, {0xD0, 0x0C}), "meter: strip 1 at the top");
  // Names fit the 6-character cells.
  expect(mackie::Surface::stripName("Lead Synth", 6) == "LdSynt" || mackie::Surface::stripName("Lead Synth", 6).size() == 6, "long names are shortened");
}

void testSingleStrip() {
  Document doc = makeDoc(4);
  script::SimpleHost host(doc);
  host.mixerSel = 2;
  Device dev;
  mackie::Surface s(host, mackie::Options{mackie::Protocol::Mcu, 0, true}, dev.out());
  expect(s.strips() == 1 && s.mixerOf(0) == 2, "one strip: the selected insert");
  s.onMidi(0, MidiMessage{0xE0, 0x7F, 0x7F});
  expect(std::fabs(volNorm(doc, 2) - 1.0) < 1e-6, "its fader drives the selected insert");
  s.onMidi(0, MidiMessage{0x90, 0x31, 0x7F});
  expect(host.mixerSel == 3 && s.mixerOf(0) == 3, "channel right selects the next insert");
}


void testHui() {
  Document doc = makeDoc(10);
  script::SimpleHost host(doc);
  Device dev;
  mackie::Surface s(host, mackie::Options{mackie::Protocol::Hui, 0, false}, dev.out());
  s.idle(0);
  expect(dev.saw(0, {0x90, 0x00, 0x00}), "HUI: the host pings the surface");
  bool name = false;
  for (const auto& [u, b] : dev.sent) name = name || (b.size() == 13 && b[0] == 0xF0 && b[4] == 0x05 && b[6] == 0x10 && b[7] == 0 && b[8] == 'I');
  expect(name, "HUI: 4-character strip names");
  // Fader 2: high bits, then low bits.
  s.onMidi(0, MidiMessage{0xB0, 0x01, 0x7F});
  s.onMidi(0, MidiMessage{0xB0, 0x21, 0x7F});
  expect(std::fabs(volNorm(doc, 2) - 1.0) < 1e-6, "HUI fader 2");
  // Zone 0 (strip 1), port 2 pressed: mute; port 3: solo; zone 0x0E port 4: play.
  s.onMidi(0, MidiMessage{0xB0, 0x0F, 0x00});
  s.onMidi(0, MidiMessage{0xB0, 0x2F, 0x42});
  s.onMidi(0, MidiMessage{0xB0, 0x0F, 0x00});
  s.onMidi(0, MidiMessage{0xB0, 0x2F, 0x02});
  s.onMidi(0, MidiMessage{0xB0, 0x0F, 0x00});
  s.onMidi(0, MidiMessage{0xB0, 0x2F, 0x43});
  expect(doc.song().mixer[1].muted && doc.song().mixer[1].solo, "HUI: mute and solo on strip 1");
  s.onMidi(0, MidiMessage{0xB0, 0x0F, 0x0E});
  s.onMidi(0, MidiMessage{0xB0, 0x2F, 0x44});
  expect(!host.actions.empty() && host.actions.back() == RemoteAction::PlayPause, "HUI: play");
  dev.sent.clear();
  s.idle(0.5);
  expect(dev.saw(0, {0xB0, 0x0C, 0x00}) && dev.saw(0, {0xB0, 0x2C, 0x42}), "HUI: the mute LED lights (zone, port)");
  s.idle(1.6);
  expect(dev.saw(0, {0x90, 0x00, 0x00}), "HUI: pinged again a second later");
  // Encoder 1: clockwise pans right.
  const float p0 = doc.song().mixer[1].pan;
  s.onMidi(0, MidiMessage{0xB0, 0x40, 0x42});
  expect(doc.song().mixer[1].pan > p0, "HUI encoder");
}

void testTemplate() {
  std::vector<std::string> err;
  const auto all = loadControllerTemplates({"assets/midi/controllers"}, &err);
  const ControllerTemplate* t = findControllerTemplate(all, "mackie-transport");
  expect(t != nullptr && err.empty(), "K-278: the Mackie transport template loads");
  if (!t) return;
  const TemplateHit h = templateLookup(*t, MidiMessage{0x90, 94, 127});
  expect(h.kind == TemplateHit::Action && h.action == RemoteAction::PlayPause, "K-278: Mackie play note plays");
  expect(templateLookup(*t, MidiMessage{0x90, 95, 127}).action == RemoteAction::Record, "K-278: record");
  expect(matchControllerTemplate(all, "Some Keyboard") == nullptr || matchControllerTemplate(all, "Some Keyboard")->id != "mackie-transport",
         "K-278: picked by hand, never by name");
}

}  // namespace

int main() {
  testMcu();
  testSingleStrip();
  testHui();
  testTemplate();
  return finish();
}
