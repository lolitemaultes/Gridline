// Gridline — K-438: sequencers sending MIDI and being written into the piano roll.
//   MIDI out: the note processors (arpeggiator, note sequencer) on a MIDI Out channel, the channel
//   rack's steps, and the own sequencers of Acid Bass and Step Bass reach the channels listening on
//   the channel's MIDI output port (Acid Bass and Step Bass sent nothing before).
//   Print: "Write the sequence to the piano roll" (Acid Bass, Step Bass) and "Bake note effects"
//   (steps go in where they play: their shift and fine pitch were dropped before).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "../collab/CollabSim.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/MidiOutTake.h"
#include "engine/NoteFxBurn.h"
#include "engine/Program.h"
#include "packs/models/strings.h"
#include "packs/plasma/plasma.h"

using namespace gl;

namespace {

Document baseDoc() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  d.setPatternLength(d.song().patterns[0].id, 1, 16);
  return d;
}

// Plays the first pattern (looping) for `frames` and returns the note-ons (keys) the MIDI Out
// channel `listener` sent to its device: what reached it on its MIDI input port.
struct Heard {
  std::vector<int> keys, vels;
  int offs = 0;
};
Heard listen(const Song& s, ObjectId listener, int frames, int block = 128) {
  ProgramBuilder b;
  b.setSampleRate(44100);
  AudioEngine e(44100.0);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Pattern);
  e.setPattern(s.patterns[0].id);
  e.play();
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  Heard h;
  auto rt = b.channelRuntime(listener);
  for (int at = 0; at < frames; at += block) {
    e.process(L.data(), R.data(), std::min(block, frames - at));
    MidiOutRing::Msg m;
    while (rt && rt->midiOut.pop(m)) {
      if ((m.s & 0xF0) == 0x90 && m.b > 0) h.keys.push_back(m.a), h.vels.push_back(m.b);
      else if ((m.s & 0xF0) == 0x80 || (m.s & 0xF0) == 0x90) ++h.offs;
    }
  }
  return h;
}

ObjectId addListener(Document& d, int port) {
  const ObjectId l = d.addChannel("listener", "gl.midiout");
  d.editChannel(l, "port", [port](Channel& c) { c.midiInPort = port; });
  d.setParam(channelPluginParam(l, pp::MoPort), 5, false);  // its own port elsewhere
  return l;
}

constexpr int kBar = 88200;  // one 4/4 bar at 120 BPM, 44.1 kHz

// ---------------------------------------------------------------- inventory: paths that worked
void testMidiOutPaths() {
  {  // arpeggiator on a MIDI Out channel -> the channel listening on its port
    Document d = baseDoc();
    const ObjectId pat = d.song().patterns[0].id;
    const ObjectId out = d.addChannel("arp", "gl.midiout");
    d.setParam(channelPluginParam(out, pp::MoPort), 2, false);
    d.addNote(pat, out, {0, 384, 60, 100});
    d.addNote(pat, out, {0, 384, 64, 100});
    d.addNoteFx(out, "gl.arp");
    const ObjectId l = addListener(d, 2);
    const Heard h = listen(d.song(), l, kBar);
    expect(h.keys.size() == 16, ("arpeggiator on a MIDI Out channel: 16 notes a bar reach the listener (" + std::to_string(h.keys.size()) + ")").c_str());
    bool both = std::count(h.keys.begin(), h.keys.end(), 60) == 8 && std::count(h.keys.begin(), h.keys.end(), 64) == 8;
    expect(both, "arpeggiator on a MIDI Out channel: it alternates the held keys");
  }
  {  // note sequencer on a MIDI Out channel
    Document d = baseDoc();
    const ObjectId pat = d.song().patterns[0].id;
    const ObjectId out = d.addChannel("seq", "gl.midiout");
    d.setParam(channelPluginParam(out, pp::MoPort), 2, false);
    d.addNote(pat, out, {0, 384, 60, 100});
    d.addNoteFx(out, "gl.notesq");
    const ObjectId l = addListener(d, 2);
    const Heard h = listen(d.song(), l, kBar);
    expect(h.keys.size() == 16, ("note sequencer on a MIDI Out channel: 16 steps reach the listener (" + std::to_string(h.keys.size()) + ")").c_str());
  }
  {  // channel rack steps on a MIDI Out channel
    Document d = baseDoc();
    const ObjectId pat = d.song().patterns[0].id;
    const ObjectId out = d.addChannel("steps", "gl.midiout");
    d.setParam(channelPluginParam(out, pp::MoPort), 2, false);
    std::vector<uint8_t> row(16, 0);
    row[0] = row[4] = row[8] = row[12] = 100;
    d.setSteps(pat, out, row);
    const ObjectId l = addListener(d, 2);
    const Heard h = listen(d.song(), l, kBar);
    expect(h.keys.size() == 4 && h.offs >= 4, "channel rack steps on a MIDI Out channel reach the listener");
  }
}

// ---------------------------------------------------------------- Acid Bass
// Default sequence: 16 steps, a note (C, middle octave = key 48) on every even step, gate 50 %.
ObjectId acidChannel(Document& d) {
  const ObjectId ch = d.addChannel("acid", "gl.acid");
  d.setParam(channelPluginParam(ch, pp::AcSeqOn), 1, false);
  return ch;
}

void testAcidMidiOut() {
  Document d = baseDoc();
  const ObjectId ch = acidChannel(d);
  d.editChannel(ch, "port", [](Channel& c) { c.midiOutPort = 4; });
  const ObjectId l = addListener(d, 4);
  const Heard h = listen(d.song(), l, kBar);
  expect(h.keys.size() == 8, ("Acid Bass sequencer: its 8 notes a bar reach the channel on its MIDI output port (" + std::to_string(h.keys.size()) + ")").c_str());
  expect(!h.keys.empty() && std::all_of(h.keys.begin(), h.keys.end(), [](int k) { return k == 48; }), "Acid Bass sequencer: the step's key goes out");
  expect(h.offs >= 7, "Acid Bass sequencer: each note ends (gate)");
  // Without the port nobody hears it.
  d.editChannel(ch, "unport", [](Channel& c) { c.midiOutPort = -1; });
  expect(listen(d.song(), l, kBar).keys.empty(), "Acid Bass sequencer: no output port, no MIDI");
}

void testAcidPrint() {
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = acidChannel(d);
  expect(sequencerOn(d.song(), ch), "Acid Bass: its sequencer counts as on");
  auto notes = printSequence(d.song(), pat, ch);
  expect(notes.size() == 8, ("Acid Bass print: 8 notes in the bar (" + std::to_string(notes.size()) + ")").c_str());
  bool grid = notes.size() == 8, lens = true;
  for (size_t i = 0; i < notes.size(); ++i) {
    grid = grid && notes[i].start == static_cast<Tick>(i) * 48 && notes[i].pitch == 48 && notes[i].velocity == 100;
    lens = lens && std::abs(notes[i].length - 12) <= 1;  // half a 16th step (24 ticks)
  }
  expect(grid, "Acid Bass print: every even 16th, key 48, unaccented velocity");
  expect(lens, "Acid Bass print: notes as long as the gate (half a step)");
  // An accent and a slide into another key: the slid notes overlap, the accent is velocity 127.
  d.setParam(channelPluginParam(ch, pp::acStep(0, pp::AcStepAccent)), 1, false);
  d.setParam(channelPluginParam(ch, pp::acStep(0, pp::AcStepSlide)), 1, false);
  d.setParam(channelPluginParam(ch, pp::acStep(1, pp::AcStepGate)), 1, false);  // step 2 plays
  d.setParam(channelPluginParam(ch, pp::acStep(1, pp::AcStepNote)), 7, false);  // G
  notes = printSequence(d.song(), pat, ch);
  const Note* a = nullptr;
  const Note* b = nullptr;
  for (const auto& n : notes) {
    if (n.start == 0) a = &n;
    if (n.start == 24) b = &n;
  }
  expect(a && a->velocity == 127, "Acid Bass print: an accented step is velocity 127");
  expect(a && b && b->pitch == 55 && a->start + a->length > b->start, "Acid Bass print: a slide overlaps the next note");
  // Into the pattern: one undo step, the sequencer off.
  d.setSteps(pat, ch, std::vector<uint8_t>(16, 0));
  expect(printSequenceToPattern(d, pat, ch), "Acid Bass: sequence written into the pattern");
  expect(!sequencerOn(d.song(), ch), "Acid Bass: the sequencer is switched off");
  expect(d.song().pattern(pat)->notes.count(ch) && d.song().pattern(pat)->notes.at(ch).size() == notes.size(), "Acid Bass: the notes are in the pattern");
  d.undo();
  expect(sequencerOn(d.song(), ch) && (!d.song().pattern(pat)->notes.count(ch) || d.song().pattern(pat)->notes.at(ch).empty()),
         "Acid Bass: one undo step brings the sequencer back");
  // Off: nothing to write.
  d.setParam(channelPluginParam(ch, pp::AcSeqOn), 0, false);
  expect(!printSequenceToPattern(d, pat, ch), "Acid Bass: sequencer off, nothing written");
}

// The printed notes play as the sequence did (same instrument, sequencer off): the levels of the
// two renders over the bar match closely (the step clock and the note grid differ by under a tick).
void testAcidPrintSounds() {
  auto render = [](const Song& s) {
    ProgramBuilder b;
    b.setSampleRate(44100);
    AudioEngine e(44100.0);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Pattern);
    e.setPattern(s.patterns[0].id);
    e.setLooping(false);
    e.play();
    std::vector<float> L(kBar), R(kBar);
    for (int at = 0; at < kBar; at += 256) e.process(L.data() + at, R.data() + at, std::min(256, kBar - at));
    return L;
  };
  for (int slides = 0; slides < 2; ++slides) {
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = acidChannel(d);
  d.addClip(pat, 0, 0);
  if (slides) {  // accents, and slides into other keys (the written notes overlap and slide too)
    for (int k : {0, 4, 8}) {
      d.setParam(channelPluginParam(ch, pp::acStep(k, pp::AcStepSlide)), 1, false);
      d.setParam(channelPluginParam(ch, pp::acStep(k + 1, pp::AcStepGate)), 1, false);
      d.setParam(channelPluginParam(ch, pp::acStep(k + 1, pp::AcStepNote)), k + 3, false);
    }
    d.setParam(channelPluginParam(ch, pp::acStep(2, pp::AcStepAccent)), 1, false);
    d.setParam(channelPluginParam(ch, pp::acStep(6, pp::AcStepAccent)), 1, false);
  }
  const auto seq = render(d.song());
  expect(printSequenceToPattern(d, pat, ch), "Acid Bass: written for the sound check");
  const auto notes = render(d.song());
  double es = 0, en = 0, worst = 0;
  const size_t win = 2205;  // 50 ms windows
  for (size_t w = 0; w + win <= seq.size(); w += win) {
    double a = 0, b = 0;
    for (size_t i = w; i < w + win; ++i) a += seq[i] * seq[i], b += notes[i] * notes[i];
    es += a, en += b;
    worst = std::max(worst, std::fabs(std::sqrt(a / win) - std::sqrt(b / win)));
  }
  std::printf("Acid Bass%s: sequence rms %.4f, written notes rms %.4f, worst 50 ms window difference %.4f\n", slides ? " (slides, accents)" : "",
              std::sqrt(es / seq.size()), std::sqrt(en / notes.size()), worst);
  expect(es > 0 && std::fabs(std::sqrt(es) - std::sqrt(en)) < 0.05 * std::sqrt(es),
         slides ? "Acid Bass, slides and accents: the written notes sound as loud as the sequence" : "Acid Bass: the written notes sound as loud as the sequence");
  expect(worst < 0.02, slides ? "Acid Bass, slides and accents: the written notes follow the sequence" : "Acid Bass: the written notes follow the sequence's rhythm");
  }
}

// ---------------------------------------------------------------- Step Bass
// Default bank: 16 steps of C (key 60), gate on; the sequence runs while a key is held,
// transposed by key - sequence key (60).
ObjectId stepBassChannel(Document& d, ObjectId pat) {
  const ObjectId ch = d.addChannel("bass", "gl.stepbass");
  d.setParam(channelPluginParam(ch, packs::models::strs::SbSeqOn), 1, false);
  d.addNote(pat, ch, {0, 384, 62, 100});
  return ch;
}

void testStepBass() {
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = stepBassChannel(d, pat);
  d.editChannel(ch, "port", [](Channel& c) { c.midiOutPort = 6; });
  const ObjectId l = addListener(d, 6);
  const Heard h = listen(d.song(), l, kBar - 2000);  // (before the loop's next pass)
  expect(h.keys.size() == 16, ("Step Bass sequencer: 16 steps reach the channel on its MIDI output port (" + std::to_string(h.keys.size()) + ")").c_str());
  expect(!h.keys.empty() && std::all_of(h.keys.begin(), h.keys.end(), [](int k) { return k == 62; }), "Step Bass sequencer: transposed by the held key");
  d.editChannel(ch, "unport", [](Channel& c) { c.midiOutPort = -1; });
  auto notes = printSequence(d.song(), pat, ch);
  expect(notes.size() == 16, ("Step Bass print: 16 notes (" + std::to_string(notes.size()) + ")").c_str());
  bool grid = notes.size() == 16;
  for (size_t i = 0; i < notes.size(); ++i)
    grid = grid && std::abs(notes[i].start - static_cast<Tick>(i) * 24) <= 1 && notes[i].pitch == 62 && std::abs(notes[i].length - 12) <= 1;
  expect(grid, "Step Bass print: a note each 16th, half a step long, at the held key");
  expect(printSequenceToPattern(d, pat, ch), "Step Bass: sequence written into the pattern");
  const PluginState& st = d.song().channel(ch)->plugin;
  expect(st.params[packs::models::strs::SbSeqOn] < 0.5f && d.song().pattern(pat)->notes.at(ch).size() == 16,
         "Step Bass: sequencer off, the notes replace the held key");
  d.undo();
  expect(d.song().channel(ch)->plugin.params[packs::models::strs::SbSeqOn] >= 0.5f && d.song().pattern(pat)->notes.at(ch).size() == 1,
         "Step Bass: one undo step");
}

// ---------------------------------------------------------------- Plasma
// Its arpeggiator with MIDI Out set to Synth reaches the channel on the output port (the engine
// carried it; Channel Settings had no output port for a built-in instrument to set).
void testPlasmaMidiOut() {
  namespace pl = gl::packs::plasma;
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("plasma", pl::kType);
  d.setParam(channelPluginParam(ch, pl::G_ArpOn), 1, false);
  d.setParam(channelPluginParam(ch, pl::G_MidiOut), 2, false);
  d.addNote(pat, ch, {0, 384, 60, 100});
  d.addNote(pat, ch, {0, 384, 67, 100});
  d.editChannel(ch, "port", [](Channel& c) { c.midiOutPort = 7; });
  const ObjectId l = addListener(d, 7);
  const Heard h = listen(d.song(), l, kBar - 2000);
  std::printf("Plasma arpeggiator: %zu notes reached the listener\n", h.keys.size());
  expect(h.keys.size() >= 4, "Plasma: its arpeggiator's notes reach the channel on its MIDI output port");
}

// ---------------------------------------------------------------- live: record the MIDI output
// Plays the pattern `passes` times with the channel's MIDI output captured (MidiOutTake polled every
// block, as the UI does every frame) and writes the take; the new pattern (kNoId: none).
ObjectId recordTake(Document& d, ObjectId pat, ObjectId ch, double passes, uint64_t* dropped = nullptr) {
  ProgramBuilder b;
  b.setSampleRate(44100);
  AudioEngine e(44100.0);
  e.setProgram(b.build(d.song()));
  e.setMode(PlayMode::Pattern);
  e.setPattern(pat);
  MidiOutTake take;
  take.begin(e, ch, pat, d.song().patternContentLength(*d.song().pattern(pat)));
  e.play();
  std::vector<float> L(128), R(128);
  for (int at = 0; at < static_cast<int>(passes * kBar); at += 128) {
    e.process(L.data(), R.data(), 128);
    take.poll(e);
  }
  e.stop();
  if (dropped) *dropped = e.capturedMidiDropped();
  return take.end(d, e, midiOutputTarget(d.song(), ch));
}

void testAcidLiveTake() {
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = acidChannel(d);
  const std::vector<Note> printed = printSequence(d.song(), pat, ch);
  const size_t patterns = d.song().patterns.size();
  uint64_t dropped = 1;
  const ObjectId made = recordTake(d, pat, ch, 2.5, &dropped);
  const Pattern* np = d.song().pattern(made);
  const std::vector<Note> got = np && np->notes.count(ch) ? np->notes.at(ch) : std::vector<Note>{};
  std::printf("Acid Bass live take: %zu notes (print %zu), dropped %llu\n", got.size(), printed.size(), static_cast<unsigned long long>(dropped));
  for (const Note& n : got) std::printf("  [%lld +%lld key %d]\n", static_cast<long long>(n.start), static_cast<long long>(n.length), n.pitch);
  bool same = got.size() == printed.size() && !got.empty();
  for (size_t i = 0; same && i < got.size(); ++i)
    same = std::abs(got[i].start - printed[i].start) <= 1 && got[i].pitch == printed[i].pitch && std::abs(got[i].length - printed[i].length) <= 1;
  expect(np != nullptr, "live take: the Acid Bass's MIDI output recorded into a new pattern (nothing listens on its port)");
  expect(same, "live take: two and a half loop passes give the bar once, the notes the print writes");
  expect(dropped == 0, "live take: nothing dropped from the capture ring");
  expect(np && np->name == d.song().channel(ch)->name + " MIDI output" && d.song().patterns.size() == patterns + 1, "live take: the new pattern is named after the channel");
  d.undo();
  expect(d.song().patterns.size() == patterns && !d.song().pattern(made), "live take: one undo step takes the pattern away");
  // Sent to a listener: the notes go to the channel that plays them.
  d.editChannel(ch, "port", [](Channel& c) { c.midiOutPort = 4; });
  const ObjectId l = addListener(d, 4);
  const ObjectId made2 = recordTake(d, pat, ch, 1.2);
  const Pattern* np2 = d.song().pattern(made2);
  expect(np2 && np2->notes.count(l) && np2->notes.at(l).size() == printed.size() && !np2->notes.count(ch),
         "live take: with a channel listening on its port, the notes are the listener's");
  // Stopped before anything played, or the sequencer off: no pattern.
  d.setParam(channelPluginParam(ch, pp::AcSeqOn), 0, false);
  const size_t before = d.song().patterns.size();
  expect(recordTake(d, pat, ch, 1.0) == kNoId && d.song().patterns.size() == before, "live take: nothing sent, nothing written");
}

// Plasma with its arpeggiator and clip player both on: each part prints alone, so nothing doubles.
void testPlasmaParts() {
  namespace pl = gl::packs::plasma;
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("plasma", pl::kType);
  pl::Model m = pl::defaultModel();
  auto& c = m.clip[0];
  c.notes = {{0, 0.2f, 60, 0.8f, false}, {0.25f, 0.2f, 64, 0.7f, false}, {0.5f, 0.2f, 67, 0.8f, false}, {0.75f, 0.2f, 72, 0.6f, false}};
  c.lengthBeats = 1;
  c.span = 2;
  d.editChannel(ch, "model", [&](Channel& x) { x.plugin.data = pl::modelToData(m); });
  d.setParam(channelPluginParam(ch, pl::G_ArpOn), 1, false);
  d.setParam(channelPluginParam(ch, pl::G_ClipOn), 1, false);
  d.addNote(pat, ch, {0, 384, 60, 100});
  d.editChannel(ch, "port", [](Channel& x) { x.midiOutPort = 7; });
  const ObjectId l = addListener(d, 7);
  auto heard = [&](int out) {
    d.setParam(channelPluginParam(ch, pl::G_MidiOut), out, false);
    return listen(d.song(), l, kBar - 2000).keys.size();
  };
  const size_t clipOnly = heard(1), both = heard(2), arpOnly = heard(3);
  d.setParam(channelPluginParam(ch, pl::G_MidiOut), 0, false);
  std::printf("Plasma MIDI out: clip %zu, arpeggiator %zu, both %zu\n", clipOnly, arpOnly, both);
  expect(clipOnly > 0 && arpOnly > 0 && both == clipOnly + arpOnly, "Plasma: MIDI Out Synth sends both parts; Arpeggiator sends the arpeggiator's notes only");
  expect(sequencerOn(d.song(), ch) && sequencerPartsOn(d.song(), ch), "Plasma: both parts on (the print asks which)");
  const std::vector<Note> arp = printSequence(d.song(), pat, ch, SeqPart::Arpeggiator);
  const std::vector<Note> clip = printSequence(d.song(), pat, ch, SeqPart::ClipPlayer);
  std::printf("Plasma print: arpeggiator %zu notes, clip player %zu notes\n", arp.size(), clip.size());
  expect(!arp.empty() && !clip.empty() && arp.size() + clip.size() >= both, "Plasma print: each part prints its own notes");
  expect(std::abs(static_cast<int>(arp.size()) - static_cast<int>(arpOnly)) <= 2 && std::abs(static_cast<int>(clip.size()) - static_cast<int>(clipOnly)) <= 2,
         "Plasma print: a part prints as many notes as it sends (no doubling)");
  expect(d.song().channel(ch)->plugin.params[pl::G_MidiOut] < 0.5f, "Plasma print: its MIDI Out setting stays as it was");
  auto P = [&](int k) { return d.song().channel(ch)->plugin.params[static_cast<size_t>(k)]; };
  expect(printSequenceToPattern(d, pat, ch, SeqPart::Arpeggiator) && P(pl::G_ArpOn) < 0.5f && P(pl::G_ClipOn) < 0.5f &&
             d.song().pattern(pat)->notes.at(ch).size() == arp.size(),
         "Plasma: the arpeggiator's notes written, both parts off");
  d.undo();
  expect(P(pl::G_ArpOn) >= 0.5f && P(pl::G_ClipOn) >= 0.5f && d.song().pattern(pat)->notes.at(ch).size() == 1, "Plasma: one undo step");
  expect(printSequenceToPattern(d, pat, ch, SeqPart::ClipPlayer) && P(pl::G_ArpOn) >= 0.5f && P(pl::G_ClipOn) < 0.5f &&
             d.song().pattern(pat)->notes.at(ch).size() == clip.size(),
         "Plasma: the clip player's notes written, its arpeggiator goes on playing them");
  // The written clip notes through the arpeggiator: the arpeggiator plays as it did from the clip.
  d.setParam(channelPluginParam(ch, pl::G_MidiOut), 3, false);
  const size_t after = listen(d.song(), l, kBar - 2000).keys.size();
  std::printf("Plasma: arpeggiator after the clip print %zu notes (before %zu)\n", after, arpOnly);
  expect(after > 0 && std::abs(static_cast<int>(after) - static_cast<int>(arpOnly)) <= 2, "Plasma: after the clip print the arpeggiator sends what it sent before");
  d.undo();
  // Only the arpeggiator on: no choice, it prints the arpeggiator.
  d.setParam(channelPluginParam(ch, pl::G_ClipOn), 0, false);
  expect(!sequencerPartsOn(d.song(), ch) && !printSequence(d.song(), pat, ch).empty(), "Plasma: one part on prints without asking");
}

// ---------------------------------------------------------------- bake: steps where they play
void testBakeSteps() {
  Document d = baseDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("keys", "gl.synth");
  d.addNoteFx(ch, "gl.chord");
  std::vector<uint8_t> row(16, 0);
  row[4] = 100;
  d.setSteps(pat, ch, row);
  d.editPattern(pat, "props", [&](Pattern& p) {
    auto& pr = p.stepProps[ch];
    pr.assign(16, StepProps{});
    pr[4].pitch = 62;
    pr[4].shift = 50;  // half a step late
    pr[4].fine = 25;
  });
  expect(burnNoteFxToPattern(d, pat, ch), "bake: steps with properties bake");
  const auto& out = d.song().pattern(pat)->notes.at(ch);
  bool shifted = out.size() == 3, fine = out.size() == 3;
  std::vector<int> keys;
  for (const auto& n : out) {
    shifted = shifted && n.start == 4 * 24 + 12;
    fine = fine && n.fine == 25;
    keys.push_back(n.pitch);
  }
  std::sort(keys.begin(), keys.end());
  expect(keys == std::vector<int>({62, 66, 69}), "bake: the step's own key makes the chord");
  expect(shifted, "bake: a shifted step bakes where it plays (half a step late)");
  expect(fine, "bake: a step's fine pitch is kept");
}

void testInfo() {
  for (const char* t : {"gl.acid", "gl.stepbass", "gl.plasma"}) {
    const PluginInfo* i = findPlugin(t);
    expect(i && i->midiOutput, (std::string(t) + ": sends MIDI on the channel's output port (Channel Settings offers it)").c_str());
  }
  expect(findPlugin("gl.acid")->sequencerParams == std::vector<int>({pp::AcSeqOn}), "Acid Bass: its sequencer switch");
  expect(findPlugin("gl.synth") && findPlugin("gl.synth")->sequencerParams.empty() && !findPlugin("gl.synth")->midiOutput,
         "a plain synth has no sequencer of its own");
  Document d = baseDoc();
  const ObjectId ch = d.addChannel("keys", "gl.synth");
  expect(!sequencerOn(d.song(), ch) && !printSequenceToPattern(d, d.song().patterns[0].id, ch), "a plain synth: nothing to write");
}

// Writing the sequence in a collaboration session: one edit that reaches the other computer, and
// the writer's one undo takes it back there too.
void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(438);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  Document seed = baseDoc();
  const ObjectId pat = seed.song().patterns[0].id;
  const ObjectId ch = acidChannel(seed);
  a.doc.reset(seed.song());
  a.folder = tmpDir("k438_a");
  b.folder = tmpDir("k438_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "collab: a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  expect(sequencerOn(b.doc.song(), ch), "collab: Ben has the running sequence");
  expect(printSequenceToPattern(a.doc, pat, ch), "collab: Ann writes the sequence to the piano roll");
  settle(net, {&a, &b}, clock);
  const Pattern* pb = b.doc.song().pattern(pat);
  expect(!sequencerOn(b.doc.song(), ch) && pb && pb->notes.count(ch) && pb->notes.at(ch).size() == 8,
         "collab: Ben gets the written notes and the sequencer switched off");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  pb = b.doc.song().pattern(pat);
  expect(sequencerOn(b.doc.song(), ch) && pb && (!pb->notes.count(ch) || pb->notes.at(ch).empty()), "collab: Ann's one undo takes it back on Ben's computer");
  // A live take of its MIDI output: the new pattern reaches Ben, and Ann's one undo takes it away.
  const ObjectId take = recordTake(a.doc, pat, ch, 1.5);
  settle(net, {&a, &b}, clock);
  const Pattern* tb = b.doc.song().pattern(take);
  expect(take != kNoId && tb && tb->notes.count(ch) && tb->notes.at(ch).size() == 8, "collab: Ben gets Ann's recorded MIDI output pattern");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!b.doc.song().pattern(take), "collab: Ann's one undo takes the recorded pattern away on Ben's computer");
  // Write its MIDI output to the piano roll (the offline print of a plugin's output): one edit, one undo.
  const size_t patterns0 = a.doc.song().patterns.size();
  const ObjectId printed = printMidiOutputToPattern(a.doc, pat, ch);
  settle(net, {&a, &b}, clock);
  const Pattern* pr = b.doc.song().pattern(printed);
  expect(printed != kNoId && pr && pr->notes.count(ch) && pr->notes.at(ch).size() == 8,
         "collab: Ben gets Ann's printed MIDI output pattern");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!b.doc.song().pattern(printed) && b.doc.song().patterns.size() == patterns0 && a.doc.song().patterns.size() == patterns0,
         "collab: Ann's one undo takes the printed pattern away on Ben's computer");
}

}  // namespace

int main() {
  // (pack tests run in the source folder: the collaboration folders go in the test's own directory)
  std::error_code ec;
  std::filesystem::current_path(gl::test::hermeticTestEnv("k438seqprint"), ec);
  testMidiOutPaths();
  testAcidMidiOut();
  testAcidPrint();
  testAcidPrintSounds();
  testStepBass();
  testPlasmaMidiOut();
  testAcidLiveTake();
  testPlasmaParts();
  testBakeSteps();
  testInfo();
  testCollab();
  return finish();
}
