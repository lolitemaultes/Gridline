// Gridline - k078-linkctl: "Link to controller" takes the control the user moved on purpose.
// The capture rules (LinkCapture) and the decoder (NRPN / RPN pairs, 14-bit CCs) on simulated
// MIDI: an FLkey 49 (its MIDI port and its DAW port) and a generic controller with a value stream.
// Clock and active sensing never reach this layer (the MIDI input drops system messages) and are
// fed here only to show the decoder ignores them too. No hardware.
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Params.h"
#include "core/LinkCapture.h"
#include "core/Project.h"
#include "core/Remote.h"

using namespace gl;

namespace {

const std::string kKnobs = "Studio Knobs", kKeys = "FLkey 49 MIDI Out", kDaw = "FLkey 49 DAW Out";
constexpr int kKnobsPort = 2, kKeysPort = 0, kDawPort = 1;

MidiMessage msg(int status, int d1, int d2, int port) {
  return {static_cast<uint8_t>(status), static_cast<uint8_t>(d1), static_cast<uint8_t>(d2), port};
}

// A little rig: messages go through the decoder into the capture, each at its own time.
struct Rig {
  MidiControlDecoder dec;
  LinkCapture cap;
  double t = 100.0;
  int captures = 0;
  void open(LinkCapture::Options o = {}) {
    cap.start(t, o);
    t += 0.2;  // past the settling time
  }
  void send(const std::string& dev, int status, int d1, int d2, double dt = 0.01) {
    t += dt;
    const int port = dev == kKnobs ? kKnobsPort : dev == kKeys ? kKeysPort : kDawPort;
    std::vector<ControlEvent> ev;
    dec.feed(msg(status, d1, d2, port), ev);
    for (const auto& e : ev)
      if (cap.feed(e, dev, dev == kDaw, t)) ++captures;
    if (cap.tick(t)) ++captures;
  }
  void wait(double dt) {
    t += dt;
    if (cap.tick(t)) ++captures;
  }
  const LinkCapture::Captured& c() const { return cap.captured(); }
};

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void decoderTests() {
  MidiControlDecoder d;
  std::vector<ControlEvent> ev;
  d.feed(msg(0xF8, 0, 0, 0), ev);  // clock
  d.feed(msg(0xFE, 0, 0, 0), ev);  // active sensing
  d.feed(msg(0xF0, 0x7E, 0, 0), ev);
  expect(ev.empty(), "decoder: clock, active sensing and SysEx give no control events");

  ev.clear();
  d.feed(msg(0xB0, 99, 1, 0), ev);
  d.feed(msg(0xB0, 98, 2, 0), ev);
  expect(ev.empty(), "decoder: the NRPN select CCs (99, 98) give no events of their own");
  d.feed(msg(0xB0, 6, 64, 0), ev);
  expect(ev.size() == 1 && ev[0].source == RemoteSource::Nrpn && ev[0].number == 130 && !ev[0].hiRes && ev[0].value == 64,
         "decoder: NRPN 1/2 data entry gives one NRPN 130 event (7-bit)");
  d.feed(msg(0xB0, 38, 100, 0), ev);
  expect(ev.size() == 2 && ev[1].source == RemoteSource::Nrpn && ev[1].hiRes && ev[1].value == (64 << 7 | 100),
         "decoder: data entry fine (CC 38) makes it 14 bits");
  d.feed(msg(0xB0, 96, 0, 0), ev);
  expect(ev.size() == 3 && ev[2].delta == 1, "decoder: data increment is a +1 step of the NRPN");

  ev.clear();
  d.feed(msg(0xB1, 101, 0, 0), ev);
  d.feed(msg(0xB1, 100, 0, 0), ev);
  d.feed(msg(0xB1, 6, 12, 0), ev);
  expect(ev.size() == 1 && ev[0].source == RemoteSource::Rpn && ev[0].number == 0 && ev[0].channel == 1, "decoder: RPN 0 on channel 2");
  d.feed(msg(0xB1, 101, 127, 0), ev);
  d.feed(msg(0xB1, 100, 127, 0), ev);
  d.feed(msg(0xB1, 6, 20, 0), ev);
  expect(ev.size() == 2 && ev[1].source == RemoteSource::CC && ev[1].number == 6, "decoder: after the null RPN, CC 6 is a plain CC again");

  ev.clear();
  d.feed(msg(0xB0, 1, 70, 3), ev);
  d.feed(msg(0xB0, 33, 5, 3), ev);
  expect(ev.size() == 2 && ev[0].number == 1 && !ev[0].hiRes && ev[1].number == 1 && ev[1].hiRes && ev[1].value == (70 << 7 | 5),
         "decoder: CC 1 then CC 33 is one 14-bit control (CC 1)");
  d.feed(msg(0xB0, 1, 71, 3), ev);
  expect(ev.size() == 3 && ev[2].hiRes && ev[2].value == 71 << 7, "decoder: the pair's coarse part counts in 14 bits from then on");
  ev.clear();
  d.feed(msg(0xB0, 40, 9, 3), ev);
  expect(ev.size() == 1 && ev[0].number == 40 && !ev[0].hiRes, "decoder: a CC 40 that does not follow CC 8 stays CC 40");
  ev.clear();
  d.feed(msg(0xE3, 0, 64, 0), ev);
  expect(ev.size() == 1 && ev[0].source == RemoteSource::PitchBend && ev[0].hiRes && ev[0].value == 8192 && ev[0].channel == 3,
         "decoder: pitch bend is a 14-bit value");
}

void captureTests() {
  {  // what "maps randomly" was: the first message of any kind won
    Rig r;
    r.send(kKnobs, 0xB0, 20, 64, 0.02);  // a value the controller sends on its own, already waiting
    r.open();
    for (int i = 0; i < 6; ++i) r.send(kKnobs, 0xB0, 20, 64, 0.05);  // ... and keeps sending
    r.send(kKeys, 0x90, 60, 100);        // a key (the hand on the keyboard)
    r.send(kKeys, 0xD0, 1, 0);           // a brush of key pressure
    r.send(kKeys, 0x80, 60, 0);
    r.wait(0.3);
    expect(!r.cap.hasCapture(), "capture: a repeating CC, a key and a light touch capture nothing");
    for (int v = 30; v <= 40; v += 2) r.send(kKnobs, 0xB0, 74, v);
    expect(r.cap.hasCapture() && r.c().source == RemoteSource::CC && r.c().number == 74 && r.c().channel == 0 && r.c().port == kKnobsPort &&
               r.c().device == kKnobs,
           "capture: the knob turned (CC 74, channel 1, its port) is captured");
    expect(near(r.c().value, 40 / 127.0), "capture: the meter follows the knob");
    r.send(kKnobs, 0xB0, 20, 64, 0.05);
    expect(r.c().number == 74, "capture: the stream does not take it back");
    r.send(kKnobs, 0xB0, 74, 90);
    expect(near(r.c().value, 90 / 127.0) && r.captures == 1, "capture: moving the same knob only moves the meter");
  }
  {  // latest deliberate move wins
    Rig r;
    r.open();
    for (int v = 10; v < 20; ++v) r.send(kKnobs, 0xB0, 21, v);
    for (int v = 100; v > 90; --v) r.send(kKnobs, 0xB0, 22, v);
    expect(r.c().number == 22 && r.c().captures == 2, "capture: moving a second knob captures it (the latest move wins)");
    for (int v = 20; v < 30; ++v) r.send(kKnobs, 0xB0, 21, v);
    expect(r.c().number == 21 && r.c().captures == 3, "capture: back to the first knob captures it again");
  }
  {  // jitter and waiting messages
    Rig r;
    r.open();
    for (int i = 0; i < 20; ++i) r.send(kKnobs, 0xB0, 7, i % 2 ? 64 : 63, 0.03);
    expect(!r.cap.hasCapture(), "capture: a fader that jitters by one step is not a move");
    Rig q;
    q.cap.start(q.t, {});
    q.send(kKnobs, 0xB0, 30, 0, 0.01);  // during the settling time: only where it rests
    q.send(kKnobs, 0xB0, 30, 90, 0.01);
    q.t += 0.2;
    expect(!q.cap.hasCapture(), "capture: messages in the first moments after opening capture nothing");
  }
  {  // the FLkey's DAW port: its pots belong to the surface; moving one captures it with its port
    Rig r;
    r.open();
    for (int v = 40; v < 50; ++v) r.send(kDaw, 0xBF, 0x15, v);
    expect(r.cap.hasCapture() && r.c().number == 0x15 && r.c().channel == 15 && r.c().port == kDawPort && r.c().surface,
           "capture: an FLkey pot (DAW port, CC 21, channel 16) is captured as that port's control and marked as the surface's");
    ControllerLink l;
    r.cap.apply(l);
    expect(l.port == kDawPort && l.midiChannel == 15 && l.cc == 0x15, "capture: the link keeps the DAW port (so it takes the pot over from the surface)");
    r.send(kDaw, 0x9F, 0x60, 100);  // a DAW-mode pad: a note
    expect(r.c().number == 0x15, "capture: a pad's note on the DAW port is ignored unless a note link is wanted");
  }
  {  // notes and buttons when asked
    Rig r;
    LinkCapture::Options o;
    o.notes = true;
    r.open(o);
    r.send(kKeys, 0x90, 48, 90);
    expect(r.cap.hasCapture() && r.c().source == RemoteSource::Note && r.c().number == 48, "capture: with notes on, a key or pad press is captured");
    r.send(kKeys, 0x80, 48, 0);
    expect(near(r.c().value, 0) && r.c().source == RemoteSource::Note, "capture: its release moves the meter to 0");
    Rig b;
    b.open();
    b.send(kKnobs, 0xB0, 64 + 30, 127);  // a button that sends one CC per press
    expect(!b.cap.hasCapture(), "capture: a lone CC waits a moment (it could be the start of a stream)");
    b.wait(0.2);
    expect(b.cap.hasCapture() && b.c().number == 94, "capture: then it is captured as a button press");
    b.send(kKnobs, 0xB0, 94, 127, 0.5);
    b.send(kKnobs, 0xB0, 94, 127, 0.5);
    b.send(kKnobs, 0xB0, 94, 127, 0.5);
    expect(!b.cap.hasCapture(), "capture: the same value again and again at a steady pace was a stream: the capture is undone");
  }
  {  // pitch bend, aftertouch, NRPN, 14-bit
    Rig r;
    r.open();
    r.send(kKeys, 0xE0, 0, 64);
    r.send(kKeys, 0xE0, 3, 64);
    expect(!r.cap.hasCapture(), "capture: a pitch wheel at rest captures nothing");
    r.send(kKeys, 0xE0, 0, 80);
    expect(r.c().source == RemoteSource::PitchBend, "capture: the pitch wheel moved is captured");
    for (int v = 10; v < 60; v += 10) r.send(kKeys, 0xD0, v, 0);
    expect(r.c().source == RemoteSource::ChannelPressure, "capture: pressing harder (aftertouch) is captured");
    for (int v = 0; v < 6; ++v) {
      r.send(kKnobs, 0xB2, 99, 0);
      r.send(kKnobs, 0xB2, 98, 74);
      r.send(kKnobs, 0xB2, 6, 20 + v * 4);
      r.send(kKnobs, 0xB2, 38, 0);
    }
    expect(r.c().source == RemoteSource::Nrpn && r.c().number == 74 && r.c().channel == 2, "capture: an NRPN knob (99, 98, 6, 38) is captured as NRPN 74, not as CC 99 or CC 6");
    ControllerLink l;
    r.cap.apply(l);
    expect(l.source == RemoteSource::Nrpn && l.cc == 74, "capture: the link is an NRPN link");
    for (int v = 0; v < 6; ++v) {
      r.send(kKnobs, 0xB0, 3, 40 + v);
      r.send(kKnobs, 0xB0, 35, 100);
    }
    expect(r.c().source == RemoteSource::CC && r.c().number == 3 && r.c().hiRes, "capture: a 14-bit fader (CC 3 with CC 35) is captured as one 14-bit CC 3");
    r.cap.apply(l);
    expect(l.source == RemoteSource::CC && l.cc == 3 && l.hiRes, "capture: the link is a 14-bit CC link");
    expect(remoteSourceLabel(l).find("14-bit") != std::string::npos, "capture: its label says 14-bit");
  }
}

// Decoded links drive their target like any link: pickup, range, 14 bits.
void decodedLinkTests() {
  ControllerLink l;
  l.source = RemoteSource::CC;
  l.cc = 3;
  l.hiRes = true;
  l.midiChannel = 0;
  expect(!remoteMatches(l, msg(0xB0, 3, 64, 0)), "links: a 14-bit link ignores the raw CC (it runs on decoded events)");
  MidiControlDecoder d;
  RemoteMapper map;
  std::vector<ControlEvent> ev;
  d.feed(msg(0xB0, 3, 64, 0), ev);
  d.feed(msg(0xB0, 35, 64, 0), ev);
  double out = -1;
  const ControlEvent& fine = ev.back();
  expect(remoteMatchesEvent(l, fine), "links: the decoded 14-bit event matches");
  expect(map.processOsc(l, remoteEventInput(l, fine), false, 0.0, &out) && near(out, (64 << 7 | 64) / 16383.0), "links: 14-bit value sets the target exactly");

  ControllerLink n;
  n.source = RemoteSource::Nrpn;
  n.cc = 74;
  n.takeover = RemoteTakeover::Pickup;
  n.min = 0.0f, n.max = 1.0f;
  ev.clear();
  d.feed(msg(0xB0, 99, 0, 0), ev);
  d.feed(msg(0xB0, 98, 74, 0), ev);
  d.feed(msg(0xB0, 6, 10, 0), ev);
  out = -1;
  const bool moved = map.processOsc(n, remoteEventInput(n, ev.back()), false, 0.5, &out);
  expect(remoteMatchesEvent(n, ev.back()) && !moved, "links: pickup: an NRPN far from the value waits");
  d.feed(msg(0xB0, 6, 70, 0), ev);  // passes 0.5
  expect(map.processOsc(n, remoteEventInput(n, ev.back()), false, 0.5, &out) && near(out, 70 / 127.0), "links: pickup: once it passes the value it takes over");
  ControlEvent other = ev.back();
  other.number = 75;
  expect(!remoteMatchesEvent(n, other), "links: another NRPN does not match");
}

void saveTests() {
  Song s = makeEmptySong();
  ControllerLink a;
  a.owner = s.mixer[0].id;
  a.index = static_cast<uint32_t>(MixerParam::Volume);
  a.source = RemoteSource::Nrpn;
  a.cc = 9000;
  a.port = 2;
  ControllerLink b = a;
  b.index = static_cast<uint32_t>(MixerParam::Pan);
  b.source = RemoteSource::CC;
  b.cc = 3;
  b.hiRes = true;
  s.links = {a, b};
  Song back;
  std::string err;
  const bool ok = songFromJson(songToJson(s), back, &err);
  expect(ok && back.links.size() == 2 && back.links[0] == a && back.links[1] == b, "save: NRPN 9000 and 14-bit CC links load back the same");
}

}  // namespace

int main() {
  decoderTests();
  captureTests();
  decodedLinkTests();
  saveTests();
  return finish();
}
