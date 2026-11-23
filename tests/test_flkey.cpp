// Gridline - k078-flkey tests: the Novation FLkey keyboards (both generations, every size), driven by
// a virtual device per model: the documented messages in, Gridline's state and the exact bytes the
// surface sends back checked (handshake, step colours, screen text, scale, standalone on leave).
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/SongTools.h"
#include "core/surfaces/FlKey.h"

using namespace gl;
using flkey::Button;
using flkey::KnobMode;
using flkey::PadPage;
using Bytes = std::vector<uint8_t>;

namespace {

// A virtual FLkey on its DAW port: what Gridline sends is recorded.
struct Device {
  std::vector<Bytes> sent;
  flkey::Surface::Out out() {
    return [this](const Bytes& b) { sent.push_back(b); };
  }
  bool saw(const Bytes& b) const { return std::find(sent.begin(), sent.end(), b) != sent.end(); }
  // Sysex bodies (after the 6-byte header) that start with `prefix`.
  std::vector<Bytes> sysex(uint8_t id, const Bytes& prefix) const {
    std::vector<Bytes> r;
    for (const auto& b : sent) {
      if (b.size() < 8 || b[0] != 0xF0 || b[1] != 0x00 || b[2] != 0x20 || b[3] != 0x29 || b[4] != 0x02 || b[5] != id) continue;
      Bytes body(b.begin() + 6, b.end() - 1);
      if (body.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), body.begin())) r.push_back(body);
    }
    return r;
  }
  // The last text written with a sysex starting with `prefix`.
  std::string text(uint8_t id, const Bytes& prefix) const {
    auto all = sysex(id, prefix);
    if (all.empty()) return "<none>";
    const Bytes& b = all.back();
    return std::string(b.begin() + static_cast<long>(prefix.size()), b.end());
  }
};

struct Rig {
  Document doc{makeEmptySong()};
  flkey::TestHost host{doc};
  Device dev;
  const flkey::Model* model = nullptr;
  std::unique_ptr<flkey::Surface> s;
  explicit Rig(const char* id) {
    model = flkey::modelById(id);
    while (doc.song().mixer.size() < 18) doc.addMixerTrack();
    const char* names[] = {"Kick", "Snare", "Hat", "Clap", "Bass", "Lead", "Pad", "Keys", "Perc", "Vox"};
    for (const char* n : names) doc.addChannel(n, "gl.synth");
    doc.editChannel(doc.song().channels[0].id, "colour", [](Channel& c) { c.color = 0xFF2010; });
    doc.editMixerTrack(doc.song().mixer[1].id, "name", [](MixerTrack& t) { t.name = "Drums"; });
    host.pattern = doc.song().patterns[0].id;
    host.channelSel = 0;
    s = std::make_unique<flkey::Surface>(host, *model, flkey::defaultWire(*model), dev.out());
  }
  void in(uint8_t st, uint8_t d1, uint8_t d2) { s->onMidi(MidiMessage{st, d1, d2}); }
  void tick() {
    host.clock += 0.05;
    s->idle(host.clock);
  }
  void press(Button b) {
    const int cc = s->wire().buttons[static_cast<size_t>(b)];
    in(0xBF, static_cast<uint8_t>(cc), 127);
    in(0xBF, static_cast<uint8_t>(cc), 0);
  }
  void pad(int i, uint8_t vel = 100) {
    in(0x90, static_cast<uint8_t>(s->padNote(i)), vel);
    host.clock += 0.1;
    in(0x90, static_cast<uint8_t>(s->padNote(i)), 0);
  }
  std::vector<uint8_t> row() const {
    const Pattern& p = doc.song().patterns[0];
    auto it = p.steps.find(doc.song().channels[0].id);
    return it == p.steps.end() ? std::vector<uint8_t>{} : it->second;
  }
  double volNorm(int m) const {
    auto d = describeParam(doc.song(), mixerParam(doc.song().mixer[static_cast<size_t>(m)].id, MixerParam::Volume));
    return d ? d->toNormalized(doc.song().mixer[static_cast<size_t>(m)].volume) : -1;
  }
};

std::string label(const flkey::Model& m, const char* what) { return std::string(m.name) + ": " + what; }

void testPorts() {
  struct Case {
    const char* port;
    const char* id;
    bool daw;
  } cases[] = {
      {"FLkey Mini MIDI Out", "flkey-mini", false},
      {"FLkey Mini DAW Out", "flkey-mini", true},
      {"FLkey 37 DAW Out", "flkey-37", true},
      {"MIDIIN2 (FLkey 37)", "flkey-37", true},
      {"FLkey 49 MIDI Out", "flkey-49", false},
      {"FLkey 61:FLkey 61 MIDI 2 24:1", "flkey-61", true},
      {"FLkey 2 Mini 25 DAW Out", "flkey2-mini", true},
      {"Focusrite - Novation - FLkey 2 37 - DAW Out", "flkey2-37", true},
      {"MIDIIN2 (FLkey 2 49 MIDI)", "flkey2-49", true},
      {"FLkey 2 61 MIDI Out", "flkey2-61", false},
      {"FLkey 49 DAW In", "flkey-49", true},
      {"FLkey DAW Out", "flkey-49", true},
      {"MIDIIN2 (FLkey)", "flkey-49", true},
  };
  for (const auto& c : cases) {
    const flkey::PortMatch m = flkey::matchPort(c.port);
    expect(m.model && std::string(m.model->id) == c.id && m.daw == c.daw, (std::string("port name: ") + c.port).c_str());
  }
  expect(flkey::matchPort("FLkey DAW Out").sizeGuessed && !flkey::matchPort("FLkey 49 DAW Out").sizeGuessed,
         "a name without a size is taken for the 49 and said so");
  expect(!flkey::matchPort("Launchkey 37 DAW Out").model && !flkey::matchPort("Keystation 49").model, "other keyboards are not FLkeys");
  expect(flkey::models().size() == 8, "eight models: FLkey Mini, 37, 49, 61 and FLkey 2 Mini 25, 37, 49, 61");
  const std::vector<std::string> outs = {"FLkey 37 MIDI In", "FLkey 37 DAW In", "MIDIOUT2 (FLkey 2 61 MIDI)"};
  expect(flkey::pairedOutput("FLkey 37 DAW Out", outs) == "FLkey 37 DAW In", "macOS: DAW Out pairs with DAW In");
  expect(flkey::pairedOutput("MIDIIN2 (FLkey 2 61 MIDI)", outs) == "MIDIOUT2 (FLkey 2 61 MIDI)", "Windows: MIDIIN2 pairs with MIDIOUT2");
  expect(flkey::pairedOutput("FLkey 49 DAW", {"FLkey 49 DAW"}) == "FLkey 49 DAW", "ALSA: the same name both ways");
}

// Every model: handshake, steps on the pads, a step toggled (one undo step), leaving to standalone.
void testHandshakeAndSteps(const char* id) {
  Rig r(id);
  const flkey::Model& m = *r.model;
  const flkey::Wire& w = r.s->wire();
  r.s->connect();
  expect(!r.dev.sent.empty() && r.dev.sent[0] == Bytes({0x9F, 0x0C, 0x7F}), label(m, "connect puts the keyboard in DAW mode first").c_str());
  expect(r.dev.saw({static_cast<uint8_t>(0xB0 | w.modeChannel), static_cast<uint8_t>(w.padModeCc), 2}), label(m, "the pads are set to the DAW layout").c_str());
  // Steps 1 and 5 on: lit in the channel colour, the rest dim.
  r.doc.setStep(r.host.pattern, r.doc.song().channels[0].id, 0, 100);
  r.doc.setStep(r.host.pattern, r.doc.song().channels[0].id, 4, 100);
  r.dev.sent.clear();
  r.tick();
  expect(r.s->page() == PadPage::Steps, label(m, "the pads start on the steps of the selected channel").c_str());
  expect(r.s->padColour(0) == 0xFF2010 && r.s->padColour(4) == 0xFF2010, label(m, "set steps show the channel colour").c_str());
  expect(r.s->padColour(1) != 0 && r.s->padColour(1) != 0xFF2010, label(m, "empty steps are dim").c_str());
  if (w.rgb) {
    // F0 00 20 29 02 <id> 01 43 <pad> r g b F7, components halved to 7 bits.
    expect(r.dev.saw({0xF0, 0x00, 0x20, 0x29, 0x02, w.sysexId, 0x01, 0x43, 0x60, 0x7F, 0x10, 0x08, 0xF7}), label(m, "step 1 sent as RGB").c_str());
  } else {
    const int idx = flkey::paletteIndex(0xFF2010);
    expect(r.dev.saw({0x90, 0x60, static_cast<uint8_t>(idx)}), label(m, "step 1 sent as a palette colour").c_str());
    expect(idx == 5 || idx == 72 || idx == 6, label(m, "red matches a red palette entry").c_str());
  }
  // Nothing changed: nothing is sent again.
  r.dev.sent.clear();
  r.tick();
  expect(r.dev.sent.empty(), label(m, "an idle with no change sends nothing").c_str());
  // Playing: the playhead step is white.
  r.host.isPlaying = true;
  r.host.pos = 2 * r.doc.song().ppq / 4;  // step 3
  r.tick();
  expect(r.s->padColour(2) == 0xFFFFFF, label(m, "the playing step is white").c_str());
  r.host.isPlaying = false;
  // Press step 2: on, one undo step; press it again: off.
  const size_t before = r.doc.history().labels().size();
  r.pad(1);
  expect(r.row().size() > 1 && r.row()[1] == 100, label(m, "a pad turns its step on").c_str());
  expect(r.doc.history().labels().size() == before + 1, label(m, "one undo step").c_str());
  r.pad(1);
  expect(r.row()[1] == 0, label(m, "pressed again, the step turns off").c_str());
  r.doc.undo();
  expect(r.row()[1] == 100, label(m, "undo brings it back").c_str());
  // Leaving: back to standalone.
  r.dev.sent.clear();
  r.s->disconnect();
  expect(!r.dev.sent.empty() && r.dev.sent.back() == Bytes({0x9F, 0x0C, 0x00}), label(m, "disconnect returns the keyboard to standalone").c_str());
  expect(!r.s->connected(), label(m, "disconnected").c_str());
}

void testKnobsAndScreen(const char* id) {
  Rig r(id);
  const flkey::Model& m = *r.model;
  const flkey::Wire& w = r.s->wire();
  r.s->connect();
  // Mixer volume mode, reported by the keyboard.
  const int mixerValue = 1;  // Mixer / Volume in both families
  r.in(static_cast<uint8_t>(0xB0 | w.modeChannel), static_cast<uint8_t>(w.knobModeCc), static_cast<uint8_t>(mixerValue));
  expect(r.s->knobMode() == KnobMode::MixerVolume, label(m, "the knob mode report selects mixer volume").c_str());
  const double start = r.volNorm(1);
  const uint8_t near = static_cast<uint8_t>(std::lround(start * 127));
  r.in(0xBF, w.knobCc, near);  // on the value: picked up
  r.in(0xBF, w.knobCc, 127);
  expect(std::fabs(r.volNorm(1) - 1.0) < 1e-6, label(m, "knob 1 moves mixer insert 1").c_str());
  r.tick();
  if (m.screen) {
    if (w.oled) {
      expect(r.dev.text(w.sysexId, {0x06, 0x15, 0x00}) == "Drums", label(m, "the screen names knob 1's target").c_str());
      expect(r.dev.text(w.sysexId, {0x06, 0x15, 0x01}).find("%") != std::string::npos, label(m, "and shows its value").c_str());
      expect(!r.dev.sysex(w.sysexId, {0x04, 0x15, 0x61}).empty(), label(m, "the knob displays are configured on connect").c_str());
    } else {
      expect(r.dev.text(w.sysexId, {0x07, 0x38}) == "Drums", label(m, "the LCD names knob 1's target").c_str());
      expect(r.dev.text(w.sysexId, {0x08, 0x38}).find("%") != std::string::npos, label(m, "and shows its value").c_str());
    }
  } else {
    expect(r.s->controlText(0).first == "Drums", label(m, "no screen: the text is kept, nothing sent").c_str());
    expect(r.dev.sysex(w.sysexId, {}).empty(), label(m, "no screen: no sysex at all").c_str());
  }
  // First generation pots pick the value up: a far value does nothing until the pot reaches it.
  if (m.gen == flkey::Gen::First) {
    Rig q(id);
    q.s->connect();
    q.in(static_cast<uint8_t>(0xB0 | w.modeChannel), static_cast<uint8_t>(w.knobModeCc), 1);
    const double v0 = q.volNorm(1);
    q.in(0xBF, w.knobCc, 0);
    expect(std::fabs(q.volNorm(1) - v0) < 1e-9, label(m, "a pot far from the value waits (pickup)").c_str());
    q.in(0xBF, w.knobCc, 127);
    expect(std::fabs(q.volNorm(1) - 1.0) < 1e-6, label(m, "and moves once it passed the value").c_str());
  }
  // Mixer bank. FLkey 2: the next eight inserts. First FLkey (Pot Mode: Banking): one track, except
  // in the Channel Rack pad mode, where it moves by eight.
  if (w.buttons[static_cast<size_t>(Button::MixerRight)] >= 0) {
    if (m.gen == flkey::Gen::First && r.s->page() != PadPage::Channels) {
      const int sel = r.host.mixerSel;
      r.press(Button::MixerRight);
      expect(r.host.mixerSel == std::max(1, sel) + 1, label(m, "mixer right moves one track outside the Channel Rack pad mode").c_str());
    } else {
      r.press(Button::MixerRight);
      expect(r.s->mixerBank() == 8 && r.host.mixerSel == 9, label(m, "mixer right moves the bank by eight and selects its first insert").c_str());
    }
  }
  // Pan in the bank (FLkey 2: the encoder bank buttons flip volume and pan).
  const int shiftCc = w.buttons[static_cast<size_t>(Button::Shift)];
  if (m.gen == flkey::Gen::Second && w.buttons[static_cast<size_t>(Button::KnobBankDown)] >= 0) {
    // FLkey 2 guides: the encoder bank buttons page the encoders (mixer volume <-> pan).
    r.press(Button::KnobBankDown);
    expect(r.s->knobMode() == KnobMode::MixerPan && r.host.presetSteps.empty(), label(m, "the encoder bank button flips mixer volume to pan").c_str());
    // "Channel Rack (Preset) ... hold shift to change Preset".
    r.in(0xBF, static_cast<uint8_t>(shiftCc), 127);
    r.press(Button::ChannelDown);
    r.press(Button::ChannelUp);
    r.in(0xBF, static_cast<uint8_t>(shiftCc), 0);
    expect(r.host.presetSteps == std::vector<int>({1, -1}), label(m, "Shift + Channel Rack down / up: next / previous preset").c_str());
    expect(r.dev.text(w.sysexId, {0x06, 0x21, 0x01}) == "Preset 2", label(m, "the screen shows the preset's name").c_str());
  }
  if (m.gen == flkey::Gen::First && w.buttons[static_cast<size_t>(Button::KnobBankDown)] >= 0 && shiftCc >= 0) {
    // Preset down alone: the selected instrument's next preset, the knobs stay as they are.
    const KnobMode before = r.s->knobMode();
    r.press(Button::KnobBankDown);
    expect(r.host.presetSteps.size() == 1 && r.host.presetSteps[0] == 1 && r.s->knobMode() == before,
           label(m, "Preset down steps to the next preset (the knob bank stays)").c_str());
    r.press(Button::KnobBankUp);
    expect(r.host.presetSteps.size() == 2 && r.host.presetSteps[1] == -1, label(m, "Preset up steps to the previous preset").c_str());
    // Shift + Preset down: the knob banks.
    r.in(0xBF, static_cast<uint8_t>(shiftCc), 127);
    r.press(Button::KnobBankDown);
    r.in(0xBF, static_cast<uint8_t>(shiftCc), 0);
    expect(r.s->knobMode() == KnobMode::MixerPan && r.host.presetSteps.size() == 2,
           label(m, "Shift + Preset down flips mixer volume to pan (no preset step)").c_str());
  }
}

void testButtons(const char* id) {
  Rig r(id);
  const flkey::Model& m = *r.model;
  const flkey::Wire& w = r.s->wire();
  r.s->connect();
  r.press(Button::Play);
  expect(r.host.isPlaying, label(m, "play").c_str());
  r.press(Button::Record);
  expect(r.host.isRecording, label(m, "record").c_str());
  if (w.buttons[static_cast<size_t>(Button::Stop)] >= 0) {
    r.press(Button::Stop);
    expect(!r.host.isPlaying, label(m, "stop").c_str());
  }
  r.press(Button::ChannelDown);
  expect(r.host.channelSel == 1, label(m, "channel down selects the next channel").c_str());
  r.press(Button::ChannelUp);
  expect(r.host.channelSel == 0, label(m, "channel up the previous").c_str());
  if (w.buttons[static_cast<size_t>(Button::Metronome)] >= 0) {
    r.press(Button::Metronome);
    expect(r.host.metronome, label(m, "metronome").c_str());
  }
  if (w.buttons[static_cast<size_t>(Button::ScoreLog)] >= 0) {
    r.press(Button::ScoreLog);
    expect(r.host.scoreLogs == 1, label(m, "score log").c_str());
  }
  if (w.buttons[static_cast<size_t>(Button::Undo)] >= 0) {
    r.doc.setStep(r.host.pattern, r.doc.song().channels[0].id, 3, 100);
    r.press(Button::Undo);
    expect(r.row().empty() || r.row()[3] == 0, label(m, "undo").c_str());
    if (w.buttons[static_cast<size_t>(Button::Redo)] >= 0) {
      r.press(Button::Redo);
    } else {  // FLkey 2: Shift + Undo
      r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 127);
      r.press(Button::Undo);
      r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 0);
    }
    expect(r.row().size() > 3 && r.row()[3] == 100, label(m, "redo").c_str());
  }
  if (w.buttons[static_cast<size_t>(Button::PatternSong)] >= 0) {
    r.press(Button::PatternSong);
    expect(r.host.isSongMode, label(m, "pattern / song").c_str());
  }
  // Pages: Shift + Page (or Shift + up / down on the first Mini) cycles the pad pages.
  const int shift = w.buttons[static_cast<size_t>(Button::Shift)];
  r.in(0xBF, static_cast<uint8_t>(shift), 127);
  if (w.buttons[static_cast<size_t>(Button::PageRight)] >= 0)
    r.press(Button::PageRight);
  else
    r.press(Button::ChannelDown);
  r.in(0xBF, static_cast<uint8_t>(shift), 0);
  expect(r.s->page() == PadPage::Channels, label(m, "Shift + page goes to the channels page").c_str());
}

void testPages() {
  Rig r("flkey2-37");
  const flkey::Wire& w = r.s->wire();
  r.s->connect();
  auto shiftPage = [&]() {
    r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 127);
    r.press(Button::PageRight);
    r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 0);
  };
  // Channels: the bottom row is the first eight channels, a pad selects and plays its channel.
  shiftPage();
  r.tick();
  expect(r.s->padColour(8) == 0xFFFFFF, "channels page: the selected channel (bottom left) is white");
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(10)), 90);
  expect(r.host.channelSel == 2 && !r.host.notes.empty() && r.host.notes.back().pitch == 60 && r.host.notes.back().velocity == 90,
         "channels page: a pad selects its channel and plays it");
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(10)), 0);
  expect(r.host.notes.back().velocity == 0, "channels page: release ends the note");
  // Patterns: pad 2 (after the only pattern) makes a new one and selects it.
  shiftPage();
  r.tick();
  expect(r.s->page() == PadPage::Patterns && r.s->padColour(0) == 0xFFFFFF, "patterns page: the current pattern is white");
  const size_t n = r.doc.song().patterns.size();
  r.pad(static_cast<int>(n));
  expect(r.doc.song().patterns.size() == n + 1 && r.host.pattern == r.doc.song().patterns.back().id, "patterns page: the next pad adds a pattern");
  r.pad(0);
  expect(r.host.pattern == r.doc.song().patterns[0].id, "patterns page: a pad selects its pattern");
  // FLkey 2 guide (Patterns Pad Mode): Shift + Page down clones the selected pattern.
  const size_t before = r.doc.song().patterns.size();
  shiftPage();
  expect(r.s->page() == PadPage::Patterns && r.doc.song().patterns.size() == before + 1 && r.host.pattern == r.doc.song().patterns[1].id,
         "patterns page: Shift + Page down clones the pattern (placed after it) and selects the copy");
  r.pad(0);
  // Channel Rack up / down move the patterns shown by eight.
  r.press(Button::ChannelDown);
  expect(r.s->patternBank() == 0, "patterns page: no further eight to show (three patterns)");
  // Instrument: chromatic from C5; with a key marker, the song's scale. (Shift + Page up, three times.)
  for (int i = 0; i < 3; ++i) {
    r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 127);
    r.press(Button::PageLeft);
    r.in(0xBF, static_cast<uint8_t>(w.buttons[static_cast<size_t>(Button::Shift)]), 0);
  }
  expect(r.s->page() == PadPage::Instrument, "Shift + Page up steps back through the pad pages");
  r.host.channelSel = 0;
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(8)), 100);
  expect(r.host.notes.back().pitch == 60, "instrument page: the bottom left pad plays C5");
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(8)), 0);
  ops::addKeyMarker(r.doc, 0, 9, 2);  // A natural minor
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(9)), 100);
  expect(r.host.notes.back().pitch == 60 + 9 + 2, "instrument page: in a key, pads step through the scale (A minor: B)");
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(9)), 0);
  // The key reaches the keyboard's Scale setting: root A, type Minor (FLkey 2 list index 1).
  r.dev.sent.clear();
  r.tick();
  expect(r.dev.saw({0xB6, 61, 9}) && r.dev.saw({0xB6, 62, 1}), "the song's key sets the keyboard's scale root and type");
  // Back to steps: page right moves 16 steps on a 32-step pattern.
  shiftPage();
  expect(r.s->page() == PadPage::Steps, "pages cycle back to steps");
  r.doc.setPatternLength(r.host.pattern, 2, 16);
  r.press(Button::PageRight);
  expect(r.s->stepPage() == 1, "page right: steps 17 to 32");
  r.pad(0);
  const auto& row = r.doc.song().patterns[0].steps.at(r.doc.song().channels[0].id);
  expect(row.size() == 32 && row[16] == 100, "a pad on the second page sets step 17");
}

void testStepEdit() {
  Rig r("flkey-61");
  r.s->connect();
  const flkey::Wire& w = r.s->wire();
  // Hold step 1, turn knob 2 (velocity): the step is set to it and stays on when released.
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(0)), 100);
  r.in(0xBF, static_cast<uint8_t>(w.knobCc + 1), 40);
  r.host.clock += 0.2;
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(0)), 0);
  expect(r.row()[0] == 40, "holding a step, knob 2 sets its velocity");
  // Knob 1 sets its pitch.
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(0)), 100);
  r.in(0xBF, w.knobCc, 72);
  r.in(0x90, static_cast<uint8_t>(r.s->padNote(0)), 0);
  const auto& props = r.doc.song().patterns[0].stepProps.at(r.doc.song().channels[0].id);
  expect(props[0].pitch == 72 && r.row()[0] == 40, "knob 1 sets the held step's pitch; the step stays on");
}

void testFaders(const char* id) {
  Rig r(id);
  const flkey::Model& m = *r.model;
  const flkey::Wire& w = r.s->wire();
  r.s->connect();
  // Fader 2 to the top (from its value: picked up first).
  const uint8_t near = static_cast<uint8_t>(std::lround(r.volNorm(2) * 127));
  r.in(0xBF, static_cast<uint8_t>(w.faderCc + 1), near);
  r.in(0xBF, static_cast<uint8_t>(w.faderCc + 1), 127);
  expect(std::fabs(r.volNorm(2) - 1.0) < 1e-6, label(m, "fader 2 moves mixer insert 2").c_str());
  const uint8_t nearMaster = static_cast<uint8_t>(std::lround(r.volNorm(0) * 127));
  r.in(0xBF, static_cast<uint8_t>(w.faderCc + 8), nearMaster);
  r.in(0xBF, static_cast<uint8_t>(w.faderCc + 8), 0);
  expect(r.volNorm(0) < 1e-6, label(m, "the ninth fader moves the master").c_str());
  // Fader button 1 mutes insert 1; its light goes off.
  r.tick();
  r.in(0xBF, w.faderButtonCc, 127);
  expect(r.doc.song().mixer[1].muted, label(m, "fader button 1 mutes insert 1").c_str());
  r.dev.sent.clear();
  r.tick();
  if (w.rgb)
    expect(r.dev.saw({0xF0, 0x00, 0x20, 0x29, 0x02, w.sysexId, 0x01, 0x53, w.faderButtonCc, 0, 0, 0, 0xF7}), label(m, "muted: its button is dark").c_str());
  else
    expect(r.dev.saw({0xB0, w.faderButtonCc, 0}), label(m, "muted: its button is dark").c_str());
  // Solo mode: fader button 3 solos insert 3.
  r.press(Button::Solo);
  r.in(0xBF, static_cast<uint8_t>(w.faderButtonCc + 2), 127);
  expect(r.s->soloMode() && r.doc.song().mixer[3].solo, label(m, "solo mode: a fader button solos its insert").c_str());
  if (m.screen) {
    r.s->invalidate();  // everything again (as after a replug)
    r.tick();
    const std::string name = w.oled ? r.dev.text(w.sysexId, {0x06, 0x05, 0x00}) : r.dev.text(w.sysexId, {0x07, 0x50});
    if (name != "Drums") std::printf("# fader 1 name: [%s]\n", name.c_str());
    expect(name == "Drums", label(m, "the screen names fader 1's target").c_str());
  }
}

void testOverridesAndHui() {
  const flkey::Model& m = *flkey::modelById("flkey-37");
  flkey::Wire w = flkey::defaultWire(m);
  std::string err;
  expect(flkey::applyOverrides(w, m, R"({"*": {"knobCc": 40}, "flkey-37": {"sysexId": 18, "buttons": {"play": 20}, "knobModes": {"12": "mixer pan"}}})", &err),
         "an override file parses");
  expect(w.knobCc == 40 && w.sysexId == 18 && w.buttons[static_cast<size_t>(Button::Play)] == 20 && w.knobModes[12] == KnobMode::MixerPan,
         "overrides change the numbers per model and for all");
  expect(!flkey::applyOverrides(w, m, "nope", &err) && !err.empty(), "a broken override file is reported");
  // HUI traffic on the DAW port (FLkey 2's mode for other software) is recognised.
  Rig r("flkey2-49");
  r.s->connect();
  expect(!r.s->huiSeen(), "no HUI at first");
  r.in(0xB0, 0x0F, 0x02);
  expect(r.s->huiSeen(), "a HUI zone select marks the port as HUI");
  // Two keyboards on one song at once, each with its own banks.
  Rig a("flkey-49");
  flkey::Surface second(a.host, *flkey::modelById("flkey2-mini"), flkey::defaultWire(*flkey::modelById("flkey2-mini")), [](const Bytes&) {});
  a.s->connect();
  second.connect();
  for (int i = 0; i < 8; ++i) a.press(Button::MixerRight);  // the first FLkey moves one track at a time here
  expect(a.s->mixerBank() == 8 && second.mixerBank() == 0, "two keyboards keep their own banks");
  a.pad(2);
  second.idle(10);
  expect(second.padColour(2) == 0xFF2010, "a step set from one keyboard shows on the other");
}

void testPalette() {
  expect(flkey::paletteIndex(0) == 0, "black is off");
  expect(flkey::paletteIndex(0xFFFFFF) == 3, "white is palette 3");
  const int g = flkey::paletteIndex(0x10E020);
  expect(g == 21 || g == 87 || g == 22 || g == 25, "green is a green entry");
  const int b = flkey::paletteIndex(0x1030FF);
  expect(b == 45 || b == 67 || b == 41 || b == 79, "blue is a blue entry");
}

}  // namespace

int main() {
  testPorts();
  for (const auto& m : flkey::models()) {
    testHandshakeAndSteps(m.id);
    testKnobsAndScreen(m.id);
    testButtons(m.id);
    if (m.faders) testFaders(m.id);
  }
  testPages();
  testStepEdit();
  testOverridesAndHui();
  testPalette();
  return finish();
}
