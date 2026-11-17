// Gridline — the sustain pedal switch (Sustain holds notes) and MIDI Out reaching channels on
// its port (GitHub #63 and #60): live and recorded pedal in both modes, MIDI Out notes and knob
// automation at their samples, the envelope controller driven by MIDI Out, loop protection and
// the MIDI input port in projects (old projects open unchanged).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MidiRecord.h"
#include "core/Params.h"
#include "core/PatternAutomation.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr double kSpt = 44100.0 * 60.0 / (140.0 * 96.0);  // samples per tick at the default tempo

float peak(const std::vector<float>& v, size_t from = 0, size_t to = ~size_t(0)) {
  float p = 0;
  for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

// First sample louder than `eps` (-1: none).
long onset(const std::vector<float>& v, float eps = 1e-6f) {
  for (size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > eps) return static_cast<long>(i);
  return -1;
}

struct Msg {
  long at;  // sample (block start + offset, or the block of a hardware message)
  uint8_t s, a, b;
};

// Plays `pat` in pattern mode for `frames` frames in blocks of `block`; returns the output and,
// for `watch` (a MIDI Out channel), what it sent: with offsets when nobody listens to it (the
// engine leaves its messages to us), else its hardware messages at their block.
std::vector<float> play(Document& d, ObjectId pat, int frames, int block, ObjectId watch, std::vector<Msg>* sent,
                        bool holds = true, bool hardware = false) {
  ProgramBuilder b;
  AudioEngine eng(44100);
  eng.setSustainHolds(holds);
  eng.setProgram(b.build(d.song()));
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(pat);
  eng.setLooping(false);
  eng.play();
  std::vector<float> out, L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  auto rt = watch != kNoId ? b.channelRuntime(watch) : nullptr;
  for (long pos = 0; pos < frames; pos += block) {
    eng.process(L.data(), R.data(), block);
    out.insert(out.end(), L.begin(), L.end());
    if (!rt || !sent) continue;
    if (hardware) {
      MidiOutRing::Msg m{};
      while (rt->midiOut.pop(m)) sent->push_back({pos, m.s, m.a, m.b});
    } else {
      Generator::MidiOutEvent ev[1024];
      const int k = rt->gen->takeMidiOut(ev, 1024);
      for (int i = 0; i < k; ++i) sent->push_back({pos + ev[i].offset, ev[i].s, ev[i].a, ev[i].b});
    }
  }
  return out;
}

const Msg* find(const std::vector<Msg>& v, int kind, int a, int b = -1) {
  for (const auto& m : v)
    if ((m.s & 0xF0) == kind && m.a == a && (b < 0 || m.b == b)) return &m;
  return nullptr;
}

int count(const std::vector<Msg>& v, int kind) {
  int n = 0;
  for (const auto& m : v) n += (m.s & 0xF0) == kind;
  return n;
}

void setPort(Document& d, ObjectId ch, int port) { d.setParam(channelPluginParam(ch, pp::MoPort), port, false); }
void listen(Document& d, ObjectId ch, int port) {
  d.editChannel(ch, "port", [port](Channel& c) { c.midiInPort = port; });
}

}  // namespace

int main() {
  // ------------------------------------------------------------ #63: live pedal, both modes
  for (bool holds : {true, false}) {
    Document d(makeEmptySong());
    ObjectId lead = d.addChannel("lead", "gl.synth");
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setSustainHolds(holds);
    eng.setProgram(b.build(d.song()));
    eng.setMidiTarget(lead);
    std::vector<float> L(4096), R(4096);
    eng.midiEvent(0xB0, 64, 127);
    eng.midiEvent(0x90, 60, 100);
    eng.process(L.data(), R.data(), 4096);
    eng.midiEvent(0x80, 60, 0);
    for (int i = 0; i < 20; ++i) eng.process(L.data(), R.data(), 4096);
    const float down = peak(L);
    eng.midiEvent(0xB0, 64, 0);
    for (int i = 0; i < 30; ++i) eng.process(L.data(), R.data(), 4096);
    const float up = peak(L);
    if (holds)
      expect(down > 0.01f && up < 1e-3f, "live, holds notes on: the note sounds until the pedal lifts");
    else
      expect(down < 1e-3f, "live, holds notes off: the note ends when its key lifts, pedal down or not");
  }
  for (bool holds : {true, false}) {  // CC 64 reaches the instrument (a MIDI Out channel passes it on)
    Document d(makeEmptySong());
    ObjectId out = d.addChannel("out", "gl.midiout");
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setSustainHolds(holds);
    eng.setProgram(b.build(d.song()));
    eng.setMidiTarget(out);
    std::vector<float> L(512), R(512);
    auto rt = b.channelRuntime(out);
    std::vector<Msg> sent;
    auto drain = [&](long at) {
      MidiOutRing::Msg m{};
      while (rt && rt->midiOut.pop(m)) sent.push_back({at, m.s, m.a, m.b});
    };
    eng.midiEvent(0xB0, 64, 127);
    eng.midiEvent(0x90, 60, 100);
    eng.process(L.data(), R.data(), 512);
    drain(0);
    eng.midiEvent(0x80, 60, 0);
    eng.process(L.data(), R.data(), 512);
    drain(1);
    const int offWhileDown = count(sent, 0x80);
    eng.midiEvent(0xB0, 64, 0);
    eng.process(L.data(), R.data(), 512);
    drain(2);
    const Msg* on = find(sent, 0xB0, 64, 127);
    const Msg* lift = find(sent, 0xB0, 64, 0);
    expect(on && lift, holds ? "holds notes on: MIDI Out passes the pedal on (down and up)"
                             : "holds notes off: MIDI Out passes the pedal on (down and up)");
    if (holds)
      expect(offWhileDown == 0 && count(sent, 0x80) == 1, "holds notes on: the note-off goes out when the pedal lifts");
    else
      expect(offWhileDown == 1, "holds notes off: the note-off goes out when the key lifts, the receiver decides");
  }

  // ------------------------------------------------------------ #60: MIDI Out reaches channels
  {
    // Reference: the synth plays the note itself.
    Document ref(makeEmptySong());
    ObjectId rs = ref.addChannel("lead", "gl.synth");
    const ObjectId rp = ref.song().patterns[0].id;
    ref.addNote(rp, rs, {1, 96, 60, 110});
    const std::vector<float> direct = play(ref, rp, 8192, 64, kNoId, nullptr);
    // The same note on a MIDI Out channel (port 0), the synth listening on input port 0.
    Document d(makeEmptySong());
    ObjectId out = d.addChannel("out", "gl.midiout");
    ObjectId lead = d.addChannel("lead", "gl.synth");
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, out, {1, 96, 60, 110});
    const std::vector<float> silent = play(d, pat, 8192, 64, kNoId, nullptr);
    listen(d, lead, 0);
    const std::vector<float> routed = play(d, pat, 8192, 64, kNoId, nullptr);
    expect(peak(silent) < 1e-6f, "a channel with no MIDI input port does not hear MIDI Out");
    expect(peak(routed) > 0.01f, "a channel on the MIDI Out channel's port plays its notes");
    expect(onset(routed) >= 0 && onset(routed) == onset(direct),
           "MIDI Out notes start on the same sample as the instrument's own (same block, sample-accurate)");
    std::printf("# onset direct %ld routed %ld (tick 1 = sample %.1f)\n", onset(direct), onset(routed), kSpt);
    // Larger engine blocks: still the same sample.
    const std::vector<float> big = play(d, pat, 8192, 512, kNoId, nullptr);
    expect(onset(big) == onset(direct), "the delivery does not depend on the block size");
    // A synth on another port hears nothing.
    listen(d, lead, 3);
    expect(peak(play(d, pat, 8192, 64, kNoId, nullptr)) < 1e-6f, "a channel on another port does not hear it");
  }
  {
    // Notes, knob CCs (with automation), program change, pitch bend and NRPN to a listener (a
    // second MIDI Out channel on input port 0, which shows what it received with offsets).
    Document d(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.midiout");
    ObjectId bch = d.addChannel("B", "gl.midiout");
    setPort(d, bch, 1);
    listen(d, bch, 0);
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, a, {1, 96, 64, 100});
    // Knob 1: CC 74, automated from 0 to 127 at tick 48. Knob 2: NRPN 300. Program 5, bend range 12.
    d.setParam(channelPluginParam(a, pp::moKnob(0, pp::MoKnobNumber)), 74, false);
    d.setParam(channelPluginParam(a, pp::moKnob(1, pp::MoKnobType)), 2, false);
    d.setParam(channelPluginParam(a, pp::moKnob(1, pp::MoKnobNumber)), 300, false);
    d.setParam(channelPluginParam(a, pp::moKnob(1, pp::MoKnobValue)), 100, false);
    d.setParam(channelPluginParam(a, pp::MoProgram), 5, false);
    d.setParam(channelPluginParam(a, pp::MoBendRange), 12, false);
    const ParamId knob = channelPluginParam(a, pp::moKnob(0, pp::MoKnobValue));
    d.editPattern(pat, "auto", [&](Pattern& p) {
      AutoPoint p0, p1;
      p0.pos = 0, p0.value = 0, p0.shape = AutoShape::Hold;
      p1.pos = 48, p1.value = 1, p1.shape = AutoShape::Hold;
      setPatternLane(p, knob.owner, knob.index, {p0, p1});
    });
    std::vector<Msg> got;
    play(d, pat, 32768, 64, bch, &got);
    const Msg* on = find(got, 0x90, 64);
    const Msg* off = find(got, 0x80, 64);
    expect(on && std::fabs(static_cast<double>(on->at) - kSpt) <= 1.0, "the listener gets the note-on at its sample");
    expect(off && std::fabs(static_cast<double>(off->at) - 97 * kSpt) <= 1.0, "and the note-off at its sample");
    const Msg* cc = find(got, 0xB0, 74, 127);
    const double due = 48 * kSpt;
    expect(cc && cc->at >= due - 1 && cc->at < due + 64, "knob automation reaches the listener as CC 74 within its control block");
    if (cc) std::printf("# CC 74 at %ld, automation point at %.1f\n", cc->at, due);
    bool nrpn = false;
    for (size_t i = 0; i + 3 < got.size(); ++i)
      nrpn = nrpn || (got[i].a == 99 && got[i].b == 2 && got[i + 1].a == 98 && got[i + 1].b == 44 && got[i + 2].a == 6);
    expect(nrpn, "an NRPN knob reaches the listener (99, 98, 6, 38)");
    expect(find(got, 0xC0, 4) != nullptr, "the program change reaches the listener");
    bool rpn = false;
    for (size_t i = 0; i + 2 < got.size(); ++i) rpn = rpn || (got[i].a == 101 && got[i].b == 0 && got[i + 1].a == 100 && got[i + 2].a == 6 && got[i + 2].b == 12);
    expect(rpn, "the bend range reaches the listener as RPN 0");
  }
  {
    // Pitch bend from the keyboard on a MIDI Out channel reaches its listener.
    Document d(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.midiout");
    ObjectId bch = d.addChannel("B", "gl.midiout");
    setPort(d, bch, 1);
    listen(d, bch, 0);
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    eng.setMidiTarget(a);
    std::vector<float> L(64), R(64);
    eng.midiEvent(0xE0, 0, 96);
    eng.midiEvent(0xB0, 64, 127);
    eng.process(L.data(), R.data(), 64);
    Generator::MidiOutEvent ev[64];
    const int k = b.channelRuntime(bch)->gen->takeMidiOut(ev, 64);
    bool bend = false, ped = false;
    for (int i = 0; i < k; ++i) {
      bend = bend || ((ev[i].s & 0xF0) == 0xE0 && ev[i].b == 96);
      ped = ped || ((ev[i].s & 0xF0) == 0xB0 && ev[i].a == 64 && ev[i].b == 127);
    }
    expect(bend && ped, "pitch bend and the pedal from a keyboard reach the listener of a MIDI Out channel");
  }
  {
    // The envelope controller driven by MIDI Out.
    Document d(makeEmptySong());
    ObjectId out = d.addChannel("out", "gl.midiout");
    ObjectId env = d.addChannel("env", "gl.envctl");
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, out, {0, 384, 60, 110});
    auto envLevel = [&]() {
      ProgramBuilder b;
      AudioEngine eng(44100);
      eng.setProgram(b.build(d.song()));
      eng.setMode(PlayMode::Pattern);
      eng.setPattern(pat);
      eng.play();
      std::vector<float> L(64), R(64);
      float hi = 0;
      for (int i = 0; i < 200; ++i) {
        eng.process(L.data(), R.data(), 64);
        hi = std::max(hi, b.channelRuntime(env)->ctlUi[0].load());
      }
      return hi;
    };
    const float without = envLevel();
    listen(d, env, 0);
    const float with = envLevel();
    expect(with > without + 0.3f, "the envelope controller on the MIDI Out port is triggered by its notes");
    std::printf("# envelope A: %.3f without the port, %.3f with it\n", without, with);
  }

  {
    // GitHub #60 (follow-up): the envelope controller drives a MIDI Out controller knob through an
    // internal controller link, and the knob's CC goes out as the envelope moves.
    Document d(makeEmptySong());
    ObjectId out = d.addChannel("out", "gl.midiout");
    ObjectId env = d.addChannel("env", "gl.envctl");
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, env, {0, 192, 60, 110});
    auto ccValues = [&]() {
      std::vector<Msg> sent;
      play(d, pat, 44100, 64, out, &sent);
      std::vector<int> v;
      for (const auto& m : sent)
        if ((m.s & 0xF0) == 0xB0 && m.a == 20) v.push_back(m.b);
      return v;
    };
    const std::vector<int> before = ccValues();
    ModLink m;
    m.sourceOwner = env, m.sourceSlot = -1, m.sourceOutput = 0;
    const ParamId knob = channelPluginParam(out, pp::moKnob(0, pp::MoKnobValue));
    m.owner = knob.owner, m.index = knob.index;
    expect(d.setModLink(m), "an envelope controller output links to a MIDI Out controller knob");
    const std::vector<int> after = ccValues();
    int hi = 0;
    for (int v : after) hi = std::max(hi, v);
    expect(before.empty() && after.size() > 8 && hi > 90, "the linked knob's CC 20 follows the envelope (it was silent unlinked)");
    std::printf("# #60 envelope -> CC 20: %zu messages unlinked, %zu linked, peak %d\n", before.size(), after.size(), hi);
    // GitHub #60 (second follow-up): the knob and its readout show the value the envelope sets. The
    // editor reads the engine's live value (ProgramBuilder::modulatedValue) whenever showsLiveValue.
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(pat);
    eng.setLooping(false);
    double shownBefore = -1, peak = 0;
    const bool none = !b.modulatedValue(knob, shownBefore);
    eng.play();
    std::vector<float> L(64), R(64);
    for (int i = 0; i < 300; ++i) {
      eng.process(L.data(), R.data(), 64);
      double v = 0;
      if (b.modulatedValue(knob, v)) peak = std::max(peak, v);
    }
    const double stored = getParam(d.song(), knob).value_or(-1);
    expect(none && peak > 90 && stored == 0, "the engine has the knob's live value as the envelope moves it (the stored value stays 0)");
    const ParamId vel = channelPluginParam(out, pp::MoVelocity);
    expect(showsLiveValue(d.song(), knob, true, false, false) && !showsLiveValue(d.song(), knob, true, false, true) &&
               !showsLiveValue(d.song(), vel, true, false, false) && showsLiveValue(d.song(), vel, true, true, false) &&
               !showsLiveValue(d.song(), vel, false, true, false),
           "a controller-driven knob shows its live value even stopped (not while dragged); automation only while playing");
    std::printf("# #60 knob 1 shows up to %.1f (stored %.1f)\n", peak, stored);
  }

  // ------------------------------------------------------------ loop protection
  {
    Document d(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.midiout");  // port 0
    listen(d, a, 0);                                // listens to itself: never
    ProgramBuilder b;
    auto p = b.build(d.song());
    expect(p->channels[0].midiTargets.empty() && p->midiOrder.empty(), "a channel never hears its own MIDI");
  }
  {
    Document d(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.midiout");
    ObjectId bch = d.addChannel("B", "gl.midiout");
    ObjectId c = d.addChannel("C", "gl.midiout");
    ObjectId s = d.addChannel("S", "gl.synth");
    setPort(d, a, 0), setPort(d, bch, 1), setPort(d, c, 2);
    listen(d, bch, 0), listen(d, c, 1), listen(d, a, 2);  // A -> B -> C -> A: a circle
    listen(d, s, 2);
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, a, {0, 96, 60, 100});
    ProgramBuilder b;
    auto p = b.build(d.song());
    expect(p->midiLoops.size() == 1 && p->midiLoops[0].first == c && p->midiLoops[0].second == a,
           "a circle of MIDI ports: the link that closes it is left out");
    expect(p->midiOrder.size() >= 3 && p->midiOrder[0] == 0 && p->midiOrder[1] == 1 && p->midiOrder[2] == 2,
           "MIDI Out channels run in order along the chain");
    const std::vector<float> o = play(d, pat, 16384, 64, kNoId, nullptr);
    expect(peak(o) > 0.01f, "the chain still plays the synth at its end");
    // Same block along the chain: the synth starts on the sample the note does.
    Document ref(makeEmptySong());
    ObjectId rs = ref.addChannel("S", "gl.synth");
    ref.addNote(ref.song().patterns[0].id, rs, {0, 96, 60, 100});
    expect(onset(o) == onset(play(ref, ref.song().patterns[0].id, 16384, 64, kNoId, nullptr)),
           "two MIDI Out channels in a chain deliver in the same block");
    // Plugin MIDI output ports take part too: S sends on port 1 (to C), and C feeds S: a circle.
    d.editChannel(s, "out", [](Channel& ch) { ch.midiOutPort = 1; });
    auto p2 = b.build(d.song());
    expect(!p2->midiLoops.empty(), "loops through plugin MIDI output ports are found as well");
  }

  // ------------------------------------------------------------ recorded pedal, both modes
  {
    Document d(makeEmptySong());
    ObjectId m = d.addChannel("piano", "gl.midiout");
    const ObjectId pat = d.song().patterns[0].id;
    NoteRecorder r;
    RecordOptions o;
    r.beginPattern(d, pat, o, 384);
    r.noteOn(d, m, 60, 100, 0);
    r.pedal(m, true, 10);
    r.noteOff(d, m, 60, 24);  // the key lifts while the pedal is down
    r.pedal(m, false, 96);
    r.end(d, 120);
    const Pattern* p = d.song().pattern(pat);
    const auto& notes = p->notes.at(m);
    expect(notes.size() == 1 && notes[0].length == 24, "recording keeps the note as long as the key was held");
    const PatternAutomation* lane = findPatternLane(*p, m, static_cast<uint32_t>(ChannelParam::Sustain));
    expect(lane && lane->events && lane->points.size() == 2 && lane->points[0].pos == 10 && lane->points[0].value == 1.0f &&
               lane->points[1].pos == 96 && lane->points[1].value == 0.0f,
           "recording keeps the pedal as played (its Sustain lane: down at 10, up at 96)");
    for (bool holds : {true, false}) {
      std::vector<Msg> got;
      play(d, pat, 30000, 16, m, &got, holds, true);
      const Msg* down = find(got, 0xB0, 64, 127);
      const Msg* up = find(got, 0xB0, 64, 0);
      const Msg* off = find(got, 0x80, 60);
      const bool pedalOk = down && up && std::fabs(down->at - 10 * kSpt) <= 32 && std::fabs(up->at - 96 * kSpt) <= 32;
      if (holds)
        expect(pedalOk && off && std::fabs(off->at - 96 * kSpt) <= 32,
               "playback, holds notes on: CC 64 at 10 and 96, the note held until the pedal lifts");
      else
        expect(pedalOk && off && std::fabs(off->at - 24 * kSpt) <= 32,
               "playback, holds notes off: CC 64 at 10 and 96, the note ends where its key lifted");
    }
  }
  {
    // A synth: the recorded pedal sustains it on playback (holds notes on) or not (off).
    Document d(makeEmptySong());
    ObjectId s = d.addChannel("lead", "gl.synth");
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, s, {0, 24, 60, 110});
    d.editPattern(pat, "pedal", [&](Pattern& p) {
      AutoPoint a, b;
      a.pos = 2, a.value = 1, a.shape = AutoShape::Hold;
      b.pos = 300, b.value = 0, b.shape = AutoShape::Hold;
      setPatternLane(p, s, static_cast<uint32_t>(ChannelParam::Sustain), {a, b});
      if (PatternAutomation* l = findPatternLane(p, s, static_cast<uint32_t>(ChannelParam::Sustain))) l->events = true;
    });
    const size_t mid = static_cast<size_t>(200 * kSpt), late = static_cast<size_t>(450 * kSpt);
    const std::vector<float> on = play(d, pat, static_cast<int>(late) + 64, 64, kNoId, nullptr, true);
    const std::vector<float> off = play(d, pat, static_cast<int>(late) + 64, 64, kNoId, nullptr, false);
    expect(peak(on, mid, mid + 2000) > 0.01f && peak(on, late - 200, late) < 1e-3f,
           "a synth's note sustains with the recorded pedal and ends when it lifts (holds notes on)");
    expect(peak(off, mid, mid + 2000) < 1e-3f, "with holds notes off the synth's note ends at its length");
  }
  {
    // Stopping releases what the pedal holds.
    Document d(makeEmptySong());
    ObjectId s = d.addChannel("lead", "gl.synth");
    const ObjectId pat = d.song().patterns[0].id;
    d.addNote(pat, s, {0, 24, 60, 110});
    d.setParam(channelParam(s, ChannelParam::Sustain), 1, false);  // the pedal set down
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(pat);
    eng.play();
    std::vector<float> L(4096), R(4096);
    for (int i = 0; i < 4; ++i) eng.process(L.data(), R.data(), 4096);
    const float held = peak(L);
    eng.stop();
    for (int i = 0; i < 30; ++i) eng.process(L.data(), R.data(), 4096);
    expect(held > 0.01f && peak(L) < 1e-3f, "the Sustain parameter holds notes, and Stop releases them");
  }

  // ------------------------------------------------------------ projects
  {
    Document d(makeEmptySong());
    ObjectId out = d.addChannel("out", "gl.midiout");
    ObjectId env = d.addChannel("env", "gl.envctl");
    ObjectId syn = d.addChannel("syn", "gl.synth");
    (void)out;
    const std::string plain = projectToText(d.song());
    expect(plain.find("midiInPort") == std::string::npos && plain.find("sustainPedal") == std::string::npos,
           "channels without a MIDI input port save as before");
    listen(d, env, 0);
    listen(d, syn, 5);
    d.editChannel(syn, "pedal", [](Channel& c) { c.sustainPedal = true; });
    Song back;
    std::string err;
    expect(loadProjectText(projectToText(d.song()), "k263.gridline", back, &err), "project with MIDI input ports saves and loads");
    const Channel* e2 = back.channel(env);
    const Channel* s2 = back.channel(syn);
    expect(e2 && s2 && e2->midiInPort == 0 && s2->midiInPort == 5 && s2->sustainPedal && back.channel(out)->midiInPort == -1,
           "the MIDI input port (and the Sustain pedal) come back as saved");
  }
  {
    Song old;
    std::string err;
    const bool ok = loadProject("tests/fixtures/test_song.gridline.json", old, &err);
    bool defaults = ok && !old.channels.empty();
    for (const auto& c : old.channels) defaults = defaults && c.midiInPort == -1 && !c.sustainPedal;
    expect(defaults, "an older project opens with no MIDI input ports and the pedal up");
    Song v1;
    const bool ok1 = loadProject("tests/fixtures/v1_song.json", v1, &err);
    bool d1 = ok1 && !v1.channels.empty();
    for (const auto& c : v1.channels) d1 = d1 && c.midiInPort == -1 && !c.sustainPedal;
    expect(d1, "a version 1 project opens with the same defaults");
    ProgramBuilder b;
    auto p = b.build(old);
    expect(p->midiOrder.empty() && p->midiLoops.empty(), "an older project has no MIDI Out delivery to run");
  }
  return finish();
}
