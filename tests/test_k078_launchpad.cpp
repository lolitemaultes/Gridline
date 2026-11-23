// Gridline - Launchpad Pro [MK3] control surface tests: a virtual device feeds the documented messages
// (Programmer's Reference Guide) to the controller and checks Gridline's state changes and the exact
// bytes sent back (handshake, LEDs, layouts, faders), with a fake model in place of the app.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "core/surfaces/LaunchpadProMk3.h"
#include "core/surfaces/Surface.h"

using namespace gl;
using namespace gl::surfaces;
using namespace gl::surfaces::lpp3;

namespace {

// The device end: every message the controller sends.
struct Wire {
  std::vector<Bytes> sent;
  Out out() {
    return [this](const Bytes& b) { sent.push_back(b); };
  }
  bool saw(const Bytes& b) const { return std::find(sent.begin(), sent.end(), b) != sent.end(); }
  int count(const Bytes& b) const { return static_cast<int>(std::count(sent.begin(), sent.end(), b)); }
  // The last short message for `status` / `d1`, or -1.
  int last(uint8_t status, uint8_t d1) const {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      if (it->size() == 3 && (*it)[0] == status && (*it)[1] == d1) return (*it)[2];
    return -1;
  }
  int shortCount() const {
    return static_cast<int>(std::count_if(sent.begin(), sent.end(), [](const Bytes& b) { return b.size() == 3; }));
  }
  void clear() { sent.clear(); }
};

struct FakeModel : Model {
  int cols = 3, rows = 4;
  std::map<std::pair<int, int>, Cell> cells;
  std::vector<std::string> log;
  Transport tr;
  int selChan = 0, selPat = 0, chans = 3, pats = 2;
  Keys k;
  Steps st;
  std::array<std::array<double, 16>, 4> fv{};
  int faders = 4;

  int sessionCols() const override { return cols; }
  int sessionRows() const override { return rows; }
  Cell sessionCell(int c, int r) const override {
    auto it = cells.find({c, r});
    return it == cells.end() ? Cell{} : it->second;
  }
  void sessionPress(int c, int r, Mod m) override { log.push_back("press " + std::to_string(c) + "," + std::to_string(r) + " mod " + std::to_string(static_cast<int>(m))); }
  void sessionRelease(int c, int r) override { log.push_back("release " + std::to_string(c) + "," + std::to_string(r)); }
  Cell sideCell(int r, lpp3::TrackMode) const override { return r == 0 ? Cell{State::Filled, 0x00FF00} : Cell{}; }
  void sidePress(int r, lpp3::TrackMode, bool shift) override { log.push_back("scene " + std::to_string(r) + (shift ? " shift" : "")); }
  Cell bottomCell(int c, lpp3::TrackMode m) const override {
    if (m == lpp3::TrackMode::Mute) return c == 1 ? Cell{State::Dim, 0xFF8400} : Cell{State::Filled, 0xFF8400};
    return c < cols ? Cell{State::Filled, 0xFF0000} : Cell{};
  }
  void bottomPress(int c, lpp3::TrackMode m) override { log.push_back("track " + std::to_string(c) + " mode " + std::to_string(static_cast<int>(m))); }
  Transport transport() const override { return tr; }
  void button(Button b) override { log.push_back("button " + std::to_string(static_cast<int>(b))); }
  int channelCount() const override { return chans; }
  Rgb channelColour(int) const override { return 0x0000FF; }
  int selectedChannel() const override { return selChan; }
  void selectChannel(int i) override { selChan = i, log.push_back("channel " + std::to_string(i)); }
  Keys keys() const override { return k; }
  void shiftRoot(int by) override { k.root = ((k.root + by) % 12 + 12) % 12; }
  Steps steps() const override { return st; }
  void toggleStep(int i) override {
    if (i < static_cast<int>(st.velocity.size())) st.velocity[static_cast<size_t>(i)] = st.velocity[static_cast<size_t>(i)] ? 0 : 100;
    log.push_back("step " + std::to_string(i));
  }
  void setStepLength(int n) override { log.push_back("length " + std::to_string(n)); }
  int patternCount() const override { return pats; }
  Rgb patternColour(int) const override { return 0xFFFF00; }
  int selectedPattern() const override { return selPat; }
  void selectPattern(int i) override { selPat = i; }
  int faderCount(Bank) const override { return faders; }
  Fader fader(Bank b, int i) const override {
    Fader f;
    f.shown = i < faders;
    f.value = fv[static_cast<size_t>(b)][static_cast<size_t>(i)];
    f.bipolar = b == Bank::Pan;
    f.colour = 0xFF0000;
    return f;
  }
  void setFader(Bank b, int i, double v) override {
    fv[static_cast<size_t>(b)][static_cast<size_t>(i)] = v;
    log.push_back("fader " + std::to_string(static_cast<int>(b)) + " " + std::to_string(i));
  }
  bool has(const std::string& s) const { return std::find(log.begin(), log.end(), s) != log.end(); }
};

MidiMessage note(int n, int v) { return {static_cast<uint8_t>(v ? 0x90 : 0x80), static_cast<uint8_t>(n), static_cast<uint8_t>(v)}; }
MidiMessage cc(int n, int v, int ch = 0) { return {static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(n), static_cast<uint8_t>(v)}; }
void tap(Controller& c, int n, double now) {
  c.onMidi(cc(n, 127), now);
  c.onMidi(cc(n, 0), now);
}
Bytes layoutReport(int layout, int page) { return {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x00, static_cast<uint8_t>(layout), static_cast<uint8_t>(page), 0x00, 0xF7}; }
// Flush every LED (the cap spreads a full refresh over a few rounds).
void settle(Controller& c, double& now) {
  for (int i = 0; i < 6; ++i) c.idle(now += 0.04);
}

void testProtocol() {
  expect(dawMode(true) == Bytes{0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x10, 0x01, 0xF7}, "DAW mode on: F0 00 20 29 02 0E 10 01 F7");
  expect(dawMode(false) == Bytes{0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x10, 0x00, 0xF7}, "DAW mode off: ... 10 00 F7");
  expect(selectLayout(LSession, 0) == Bytes{0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0xF7}, "select Session layout");
  expect(selectLayout(LFader, 2) == Bytes{0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x00, 0x01, 0x02, 0x00, 0xF7}, "select fader bank 3");
  expect(deviceInquiry() == Bytes{0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, "device inquiry");
  const Bytes fs = faderSetup(1, true, {{0, true, 28, 5}});
  expect(fs == Bytes{0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E, 0x01, 0x01, 0x01, 0x00, 0x01, 0x1C, 0x05, 0xF7}, "fader setup bytes");
  expect(padOf(0, 0) == 81 && padOf(7, 0) == 88 && padOf(0, 7) == 11 && padOf(7, 7) == 18, "grid numbering: 81 top left, 18 bottom right");
  expect(isPad(11) && isPad(88) && !isPad(19) && !isPad(10) && !isPad(90), "pads 11..88 only (not the side buttons)");
  expect(padCol(34) == 3 && padRow(34) == 5, "pad 34: column 3, row 5 from the top");
  // Port names (macOS / Linux, and Windows' numbered interfaces).
  expect(isDawPort("LPProMK3 DAW"), "macOS DAW port recognised");
  expect(isDawPort("Launchpad Pro MK3:Launchpad Pro MK3 LPProMK3 DAW 20:1"), "Linux ALSA DAW port recognised");
  expect(isDawPort("MIDIIN3 (LPProMK3 MIDI)"), "Windows DAW interface recognised");
  expect(!isDawPort("LPProMK3 MIDI") && !isDawPort("LPProMK3 DIN") && !isDawPort("MIDIIN2 (LPProMK3 MIDI)"), "its MIDI and DIN ports stay ordinary inputs");
  expect(!isDawPort("Some Keyboard DAW"), "another device's DAW port is not taken");
  expect(outputFor("LPProMK3 DAW", {"LPProMK3 MIDI", "LPProMK3 DAW"}) == "LPProMK3 DAW", "answers on the output of the same name");
  expect(outputFor("MIDIIN3 (LPProMK3 MIDI)", {"LPProMK3 MIDI", "MIDIOUT2 (LPProMK3 MIDI)", "MIDIOUT3 (LPProMK3 MIDI)"}) == "MIDIOUT3 (LPProMK3 MIDI)",
         "Windows: MIDIIN3 answers on MIDIOUT3");
  // Palette and colour matching.
  const auto& p = palette();
  expect(p[0] == 0 && p[5] == 0xFF0000 && p[21] == 0x00FF00 && p[45] == 0x0000FF && p[3] == 0xFFFFFF, "palette: off, red 5, green 21, blue 45, white 3");
  expect(nearestColour(p, 0xFF0000) == 5 || nearestColour(p, 0xFF0000) == 72, "pure red matches a red entry");
  expect(nearestColour(p, 0x00FF00) == 21 || nearestColour(p, 0x00FF00) == 87, "pure green matches a green entry");
  expect(nearestColour(p, 0x101010) != 5, "a dark grey never matches red");
}

void testOptions() {
  Options o;
  o.modePages = false, o.velocity = 2, o.brightness = 1, o.exactColours = true, o.stepsPage = false;
  const Options back = Options::load(o.save());
  expect(back == o, "options round trip through the saved string");
  expect(Options::load("") == Options{}, "empty options: the defaults");
  expect(Options::load("vel=9;bright=-3;junk;zz=1").velocity == 3 && Options::load("vel=9;bright=-3").brightness == 0, "options clamp and skip junk");
}

void testHandshake() {
  FakeModel m;
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  expect(w.sent.size() >= 3 && w.sent[0] == dawMode(true), "handshake: DAW mode on first");
  expect(w.sent.size() >= 3 && w.sent[1] == selectLayout(LSession, 0), "handshake: then the Session layout");
  expect(w.saw(deviceInquiry()), "handshake: asks the firmware version");
  // The unit answers its version and the layout it now shows.
  c.onSysex({0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x20, 0x29, 0x13, 0x01, 0x00, 0x00, 0x00, 0x04, 0x05, 0x03, 0xF7}, 0.01);
  expect(c.firmware() == "0.4.5.3", "device inquiry answer read: firmware digits");
  c.onSysex(layoutReport(LSession, 0), 0.02);
  expect(c.page() == Page::Session, "the handshake's own layout report keeps the Session page");
  w.clear();
  double now = 0.1;
  settle(c, now);
  expect(w.shortCount() > 64, "first idle after connect lights every pad and button");
  // Fader banks set up at once (four 01 messages: Volume vertical, Pan horizontal bipolar).
  int setups = 0;
  for (const Bytes& b : w.sent)
    if (b.size() > 8 && b[6] == CmdFaderSetup) ++setups;
  expect(setups == 4, "the four fader banks are set up");
  expect(w.saw(faderSetup(1, true, {{0, true, 28, std::max(1, nearestColour(palette(), 0xFF0000))},
                                    {1, true, 29, std::max(1, nearestColour(palette(), 0xFF0000))},
                                    {2, true, 30, std::max(1, nearestColour(palette(), 0xFF0000))},
                                    {3, true, 31, std::max(1, nearestColour(palette(), 0xFF0000))},
                                    {4, false, 32, 0}, {5, false, 33, 0}, {6, false, 34, 0}, {7, false, 35, 0}})),
         "Pan bank: horizontal, bipolar, CCs 28..35, unused faders hidden");
  // Nothing changed: nothing sent.
  w.clear();
  settle(c, now);
  expect(w.sent.empty(), "no change: idle sends nothing");
  // Disconnect gives it back.
  c.disconnect();
  expect(w.sent.size() == 1 && w.sent[0] == dawMode(false), "disconnect: DAW mode off (standalone)");
  w.clear();
  c.idle(now += 1);
  c.disconnect();
  expect(w.sent.empty(), "disconnected: silent (and a second disconnect sends nothing)");
  // Replug: a new handshake sends everything again.
  c.connect();
  settle(c, now);
  expect(w.saw(dawMode(true)) && w.shortCount() > 64, "reconnect: handshake and a full refresh again");
}

void testSessionLeds() {
  FakeModel m;
  m.cells[{0, 0}] = {State::Filled, 0xFF0000};
  m.cells[{1, 0}] = {State::Playing, 0x00FF00};
  m.cells[{2, 1}] = {State::Queued, 0x0000FF};
  m.cells[{0, 2}] = {State::Recording, 0};
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  double now = 0;
  settle(c, now);
  const int red = nearestColour(palette(), 0xFF0000), green = nearestColour(palette(), 0x00FF00), blue = nearestColour(palette(), 0x0000FF);
  expect(w.last(0x90, 81) == red, "a clip: static note on pad 81 in its colour (channel 1)");
  expect(w.last(0x92, 82) == green, "the playing clip pulses in its colour (channel 3)");
  expect(w.last(0x90, 73) == blue && w.last(0x91, 73) == KGreen, "a queued clip flashes green over its colour (channel 2)");
  expect(w.last(0x92, 61) == KRed, "a recording clip pulses red");
  expect(w.last(0x90, 84) == 0, "outside the model's columns: off");
  expect(w.last(0xB0, 89) == green, "scene button lit from the model");
  expect(w.last(0xB0, 101) == red, "Track Select row lit from the model");
  // The playing clip changes: only that LED goes out.
  m.cells[{1, 0}] = {State::Filled, 0x00FF00};
  w.clear();
  settle(c, now);
  expect(w.sent.size() == 1 && w.sent[0] == Bytes{0x90, 82, static_cast<uint8_t>(green)}, "only the changed pad is sent");
  // Exact colours: RGB SysEx 03, type 3, values 0..127.
  Options o;
  o.exactColours = true;
  c.setOptions(o);
  w.clear();
  settle(c, now);
  bool rgb = false;
  for (const Bytes& b : w.sent)
    if (b.size() > 12 && b[6] == CmdLed)
      for (size_t i = 7; i + 4 < b.size(); i += 5)
        if (b[i] == 3 && b[i + 1] == 81) rgb = b[i + 2] == 127 && b[i + 3] == 0 && b[i + 4] == 0;
  expect(rgb, "exact colours: pad 81 as RGB SysEx 03 03 51 7F 00 00");
  // Brightness: a dimmer entry.
  o.exactColours = false;
  o.brightness = 2;
  c.setOptions(o);
  w.clear();
  settle(c, now);
  expect(w.last(0x90, 81) > 0 && w.last(0x90, 81) != red, "low brightness: a darker palette entry");
}

void testSessionPresses() {
  FakeModel m;
  m.cols = 20, m.rows = 12;
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  double now = 0;
  c.onMidi(note(81, 100), now);
  c.onMidi(note(81, 0), now);
  expect(m.has("press 0,0 mod 0") && m.has("release 0,0"), "pad 81: launch column 0 scene 0, then release");
  c.onMidi(note(18, 90), now);
  expect(m.has("press 7,7 mod 0"), "pad 18: column 7 row 7");
  // Scrolling.
  tap(c, CRight, now);
  tap(c, CDown, now);
  expect(c.colOffset() == 1 && c.rowOffset() == 1, "arrows scroll the view one step");
  c.onMidi(cc(CShift, 127), now);
  tap(c, CRight, now);
  c.onMidi(cc(CShift, 0), now);
  expect(c.colOffset() == 9, "Shift + arrow scrolls a page");
  tap(c, CRight, now);
  tap(c, CRight, now);
  tap(c, CRight, now);
  tap(c, CRight, now);
  expect(c.colOffset() == 12, "the view stops at the last column");
  c.onMidi(note(81, 100), now);
  expect(m.has("press 12,1 mod 0"), "presses follow the view");
  // Modifiers held.
  c.onMidi(cc(CClear, 127), now);
  c.onMidi(note(82, 100), now);
  c.onMidi(cc(CClear, 0), now);
  expect(m.has("press 13,1 mod 2"), "Clear + pad: clear the slot");
  c.onMidi(cc(CDuplicate, 127), now);
  c.onMidi(note(83, 100), now);
  c.onMidi(cc(CDuplicate, 0), now);
  expect(m.has("press 14,1 mod 3"), "Duplicate + pad");
  c.onMidi(cc(CQuantise, 127), now);
  c.onMidi(note(84, 100), now);
  c.onMidi(cc(CQuantise, 0), now);
  expect(m.has("press 15,1 mod 5"), "Quantise + pad");
  // Scenes and track controls.
  tap(c, CScene - 20, now);
  expect(m.has("scene 3"), "scene button 69: scene 3 of the view");
  tap(c, CMute, now);
  expect(c.trackMode() == lpp3::TrackMode::Mute, "Mute: the Track Select row mutes");
  tap(c, CTrackSelect + 2, now);
  expect(m.has("track 14 mode 2"), "Track Select 3 in Mute mode: mute track 14");
  tap(c, CMute, now);
  expect(c.trackMode() == lpp3::TrackMode::Select, "Mute again: back to Select");
  // Transport.
  tap(c, CPlay, now);
  expect(m.has("button 0"), "Play: play / pause");
  c.onMidi(cc(CShift, 127), now);
  tap(c, CPlay, now);
  tap(c, CRecord, now);
  tap(c, CRecordArm, now);
  tap(c, CMute, now);
  tap(c, CSolo, now);
  tap(c, CStopClip, now);
  tap(c, CSends, now);
  c.onMidi(cc(CShift, 0), now);
  expect(m.has("button 1") && m.has("button 3"), "Shift + Play: stop; Shift + Record: capture");
  expect(m.has("button 4") && m.has("button 5") && m.has("button 6") && m.has("button 8") && m.has("button 7"),
         "Shift + track controls: undo, redo, click, stop all, tap tempo");
  tap(c, CRecord, now);
  tap(c, CFixedLength, now);
  expect(m.has("button 2") && m.has("button 9"), "Record and Fixed Length (loop record)");
}

void testPages() {
  FakeModel m;
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  c.onSysex(layoutReport(LSession, 0), 0.01);
  double now = 1;
  // Shift + Track Select 2: Keys.
  c.onMidi(cc(CShift, 127), now);
  tap(c, CTrackSelect + 1, now);
  c.onMidi(cc(CShift, 0), now);
  expect(c.page() == Page::Keys, "Shift + Track Select 2: Keys page");
  // The unit's Note button: its layout report brings Keys / Drums, and Gridline asks Session back.
  c.setPage(Page::Session, now);
  w.clear();
  now += 2;
  c.onSysex(layoutReport(LNote, 0), now);
  expect(c.page() == Page::Keys && w.saw(selectLayout(LSession, 0)), "Note pressed: Keys page drawn in the Session layout");
  c.onSysex(layoutReport(LSession, 0), now + 0.05);
  expect(c.page() == Page::Keys, "the confirming report keeps the Keys page");
  m.k.drums = true;
  now += 2;
  c.onSysex(layoutReport(LNote, 0), now);
  expect(c.page() == Page::Drums, "Note with a drum instrument: Drums page");
  now += 2;
  c.onSysex(layoutReport(7, 0), now);
  expect(c.page() == Page::Steps, "Sequencer pressed: Steps page");
  now += 2;
  c.onSysex(layoutReport(LChord, 0), now);
  expect(c.page() == Page::Device, "Chord: the unit's own mode (Gridline draws nothing over it)");
  now += 2;
  c.onSysex(layoutReport(LSession, 0), now);
  expect(c.page() == Page::Session, "Session pressed: back to Gridline's Session page");
  // Option off: Note stays the unit's.
  Options o;
  o.modePages = false;
  c.setOptions(o);
  w.clear();
  now += 2;
  c.onSysex(layoutReport(LNote, 0), now);
  expect(c.page() == Page::Device && !w.saw(selectLayout(LSession, 0)), "option off: the unit's own Note mode stays");
  c.setOptions(Options{});
  now += 2;
  c.onSysex(layoutReport(LSession, 0), now);
  // Faders: Volume opens the fader layout.
  w.clear();
  tap(c, CVolume, now);
  expect(c.page() == Page::Faders && c.bank() == Bank::Volume && w.saw(selectLayout(LFader, 0)), "Volume: fader layout bank 1");
  tap(c, CPan, now);
  expect(c.bank() == Bank::Pan && w.saw(selectLayout(LFader, 1)), "Pan: fader bank 2");
  w.clear();
  tap(c, CPan, now);
  expect(c.page() == Page::Session && w.saw(selectLayout(LSession, 0)), "Pan again: back to the Session page");
  // Held (momentary): let go after half a second, the page comes back.
  c.onMidi(cc(CSends, 127), now);
  expect(c.page() == Page::Faders && c.bank() == Bank::Sends, "Sends held: fader bank 3");
  c.onMidi(cc(CSends, 0), now + 0.8);
  expect(c.page() == Page::Session, "Sends let go after a hold: back");
  now += 2;
  // A page report from the unit itself (its fader layout chosen there).
  c.onSysex(layoutReport(LFader, 3), now);
  expect(c.page() == Page::Faders && c.bank() == Bank::Device, "the unit's fader bank 4 report: Device faders");
  // Pages off: the picker ignores them.
  o = Options{};
  o.keysPage = false;
  o.faders = false;
  c.setOptions(o);
  expect(c.page() == Page::Session, "faders switched off while showing: back to the page before");
  c.onMidi(cc(CShift, 127), now);
  tap(c, CTrackSelect + 1, now);
  tap(c, CTrackSelect + 4, now);
  c.onMidi(cc(CShift, 0), now);
  expect(c.page() == Page::Session, "Keys and Faders off: the picker leaves them out");
}

void testKeysAndDrums() {
  FakeModel m;
  m.k.root = 2;  // D
  m.k.scale = 0b101010110101;  // major
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  double now = 0;
  c.setPage(Page::Keys, now);
  settle(c, now);
  const auto& p = c.padPitch();
  expect(p[11] == 48 + 2, "Keys in D major: bottom-left pad is D (octave 4)");
  expect(p[12] == 48 + 4 && p[13] == 48 + 6 && p[14] == 48 + 7, "along the row: the scale's next degrees E, F#, G");
  expect(p[21] == 48 + 7, "the row above starts three degrees up (G)");
  expect(p[81] >= 0 && p[19] == -1 && p[10] == -1, "only grid pads play");
  const uint64_t s0 = c.padSerial();
  tap(c, CUp, now);
  settle(c, now);
  expect(c.octave() == 5 && c.padPitch()[11] == 62 && c.padSerial() != s0, "Up: an octave higher, the pad map republished");
  // The root lit in full, other in-key notes dim, a held pad white.
  c.onMidi(note(11, 100), now);
  w.clear();
  settle(c, now);
  expect(w.last(0x90, 11) == KWhite, "a held key lights white");
  c.onMidi(note(11, 0), now);
  w.clear();
  settle(c, now);
  expect(w.last(0x90, 11) == nearestColour(palette(), m.k.colour), "let go: the root in the channel's colour");
  // Chromatic: rows a fourth apart.
  m.k.scale = 0x0FFF;
  settle(c, now);
  expect(c.padPitch()[21] - c.padPitch()[11] == 5 && c.padPitch()[12] - c.padPitch()[11] == 1, "chromatic: semitones along, fourths up");
  // Shift + left / right: the key's root moves (in the piano roll).
  c.onMidi(cc(CShift, 127), now);
  tap(c, CRight, now);
  c.onMidi(cc(CShift, 0), now);
  expect(m.k.root == 3, "Shift + Right: the key one semitone up");
  // Track Select picks the channel on Keys.
  tap(c, CTrackSelect + 2, now);
  expect(m.selChan == 2, "Track Select 3 on Keys: channel 3 played");
  // Drums: four 4x4 blocks from note 36.
  c.setPage(Page::Drums, now);
  settle(c, now);
  const auto& d = c.padPitch();
  expect(d[11] == 36 && d[14] == 39 && d[41] == 48, "Drums bottom left: 36 .. 51");
  expect(d[51] == 52 && d[15] == 68 && d[55] == 84, "top left 52, bottom right 68, top right 84");
  tap(c, CUp, now);
  settle(c, now);
  expect(c.padPitch()[11] == 52, "Up: the blocks move 16 notes");
}

void testSteps() {
  FakeModel m;
  m.st.count = 16;
  m.st.velocity.assign(16, 0);
  m.st.velocity[0] = 100;
  m.st.velocity[4] = 100;
  m.st.playhead = 2;
  m.st.colour = 0xFF0000;
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  double now = 0;
  c.setPage(Page::Steps, now);
  settle(c, now);
  const int full = nearestColour(palette(), 0xFF0000);
  expect(w.last(0x90, 81) == full && w.last(0x90, 85) == full, "steps on: lit in the channel colour");
  expect(w.last(0x90, 83) == KWhite, "the playhead step white");
  expect(w.last(0x90, 82) != full && w.last(0x90, 82) > 0, "an empty step dim");
  expect(w.last(0x90, 61) == 0, "steps past the row: off");
  c.onMidi(note(82, 100), now);
  expect(m.has("step 1") && m.st.velocity[1] == 100, "pad toggles step 2 (one edit)");
  c.onMidi(cc(CShift, 127), now);
  c.onMidi(note(78, 100), now);
  c.onMidi(cc(CShift, 0), now);
  expect(m.has("length 16"), "Shift + step: the row's loop length");
  c.onMidi(note(42, 100), now);
  expect(m.has("channel 1"), "lower half: channel pick");
  tap(c, CScene - 10, now);
  expect(m.selPat == 1, "scene column on Steps: pattern pick");
}

void testFaders() {
  FakeModel m;
  m.fv[0][0] = 0.5;
  m.fv[0][1] = 1.0;
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  double now = 0;
  settle(c, now);
  expect(w.last(0xB4, 20) == 64 && w.last(0xB4, 21) == 127, "fader positions on channel 5 (CC 20, 21)");
  // Moving a fader by hand: the model is told, the unit is not echoed while the hand is on it.
  w.clear();
  c.onMidi(cc(20, 100, 4), now);
  expect(m.has("fader 0 0") && std::abs(m.fv[0][0] - 100 / 127.0) < 1e-9, "fader move (CC 20 on channel 5): volume of track 0");
  m.fv[0][0] = 0.2;  // the project disagrees (an automation) while the hand is on it
  c.idle(now + 0.05);
  expect(w.last(0xB4, 20) == -1, "no echo while touched");
  c.idle(now + 1.0);
  expect(w.last(0xB4, 20) == 25, "after a pause the fader follows the project again");
  // Pan bank CCs 28..
  c.onMidi(cc(29, 0, 4), now + 2);
  expect(m.has("fader 1 1"), "Pan bank: CC 29 is pan of track 1");
  // A new fader count rebuilds the set-up.
  m.faders = 2;
  w.clear();
  c.idle(now + 3);
  int setups = 0;
  for (const Bytes& b : w.sent)
    if (b.size() > 8 && b[6] == CmdFaderSetup) ++setups;
  expect(setups == 4, "fewer tracks: every bank set up again, the rest hidden");
}

void testLedCap() {
  FakeModel m;
  m.cols = 8, m.rows = 8;
  for (int c0 = 0; c0 < 8; ++c0)
    for (int r = 0; r < 8; ++r) m.cells[{c0, r}] = {State::Filled, 0x00FF00};
  Wire w;
  Controller c(m, Options{}, w.out());
  c.connect();
  w.clear();
  c.idle(0);
  int msgs = w.shortCount();
  expect(msgs <= Controller::kMaxPerIdle + 32, "one round sends at most the cap (plus fader positions)");
  LedCache cache;
  std::array<bool, 128> used{};
  used[5] = used[6] = true;
  cache.use(used);
  cache.want(5, Led::solid(3));
  cache.want(9, Led::solid(3));
  const std::vector<int> t = cache.take(10);
  expect(t.size() == 2 && t[0] == 5 && t[1] == 6, "LED cache: only used LEDs, all sent after invalidate");
  expect(cache.take(10).empty() && cache.pending() == 0, "LED cache: nothing twice");
}

void testTwoDevices() {
  FakeModel m;
  m.cells[{0, 0}] = {State::Playing, 0x00FF00};
  Wire w1, w2;
  Controller a(m, Options{}, w1.out()), b(m, Options{}, w2.out());
  a.connect();
  b.connect();
  double now = 0;
  settle(a, now);
  settle(b, now);
  a.setPage(Page::Keys, now);
  w1.clear(), w2.clear();
  settle(a, now);
  settle(b, now);
  expect(a.page() == Page::Keys && b.page() == Page::Session, "two units: each keeps its own page");
  expect(w2.sent.empty() && !w1.sent.empty(), "two units: only the one that changed sends");
}

}  // namespace

int main() {
  testProtocol();
  testOptions();
  testHandshake();
  testSessionLeds();
  testSessionPresses();
  testPages();
  testKeysAndDrums();
  testSteps();
  testFaders();
  testLedCap();
  testTwoDevices();
  return finish();
}
