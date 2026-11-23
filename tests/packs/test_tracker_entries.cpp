// Gridline - 0.7.8: the Tracker Sampler's sample entries (the classic sample list 01 .. 10 of one
// instrument) and the Sample editor's To entry.
//   * a file loaded into a picked entry goes there: the first sample of an instrument takes every key
//     in that entry (before: always entry 01); a later one keeps the key map;
//   * a range to the next free entry, to a picked free entry, over a used entry (its keys stay), the
//     region played from the same file (a cut, nothing written), the source's settings copied, the
//     loop kept only when it lies inside the range; nothing done when there is nothing to do;
//   * each edit one document step that undo takes back whole;
//   * the instrument text and the project file carry the entries back byte for byte;
//   * an entry past the first plays from the keys mapped to it (the play tap names its slot) and
//     plays the range's frames.
//   * the Backwards option (a sample played from its end): one undo step, carried by the project file
//     byte for byte and by the entry edits (duplicate, copy, exchange, a range to an entry); an old
//     project's text reads and writes as it was.
//   * 0.7.8: Cut after loop and Minimise on a sample played backwards drop what precedes the loop (the
//     part it never reaches), its points moving with the audio; a forward sample loses what follows the
//     loop exactly as before.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/Document.h"
#include "../../src/core/Json.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Project.h"
#include "../../src/core/TrackerData.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/tracker/tracker.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// An entry holds a sample (its definition, or a file).
bool hasSample(const tp::Edit& e, int k) {
  const auto i = static_cast<size_t>(k);
  return e.text.def.samples[i].present() || e.text.fileOf[i] >= 0;
}

int keysOf(const tp::Edit& e, int slot) {
  int n = 0;
  for (uint8_t m : e.text.def.keymap) n += m == slot ? 1 : 0;
  return n;
}
std::string fileOf(const tp::Edit& e, int slot) {
  const int f = e.text.fileOf[static_cast<size_t>(slot)];
  return f >= 0 && f < static_cast<int>(e.text.files.size()) ? e.text.files[static_cast<size_t>(f)] : std::string();
}

// Kick (looped 2000 + 3000, cut 100 .. 8400, 8-bit, its own settings) in 01, snare in 02.
tp::Edit fixture() {
  tp::Edit e = tp::readEdit("", 0);
  tp::addSample(e, "/fixture/kick.wav", 9000, 44100, "kick");
  tp::addSample(e, "/fixture/snare.wav", 4000, 22050, "snare");
  trk::SampleDef& s = e.text.def.samples[0];
  s.loop = trk::kLoopPingPong;
  s.loopStart = 2000;
  s.loopLength = 3000;
  s.volume = 40;
  s.pan = 200;
  s.finetune = -12;
  s.relNote = 5;
  e.cuts.start[0] = 100;
  e.cuts.end[0] = 8400;
  e.eightBit[0] = true;
  tp::mapKeys(e, 48, 95, 1);  // the upper keys play the snare
  return e;
}

void testLoadIntoEntry() {
  // An instrument with no sample: the file goes into the entry picked, every key plays it.
  tp::Edit e = tp::readEdit("", 0);
  expect(tp::firstFreeEntry(e) == 0, "empty instrument: the first free entry is 01");
  expect(tp::replaceSample(e, 2, "/x/pad.wav", 5000, 44100, "pad") == 2, "load into entry 03 of an empty instrument: entry 03");
  expect(hasSample(e, 2) && !hasSample(e, 0) && !hasSample(e, 1), "load into entry 03: entries 01 and 02 stay empty");
  expect(keysOf(e, 2) == 96, "load into entry 03: the first sample takes every key");
  expect(tp::firstFreeEntry(e) == 0, "entry 01 is still the first free one");
  // Another into entry 06: the keys stay with 03.
  expect(tp::replaceSample(e, 5, "/x/bell.wav", 3000, 22050, "bell") == 5 && keysOf(e, 2) == 96 && keysOf(e, 5) == 0,
         "load into entry 06: a second sample, the key map stays");
  // Over a used entry: its file and name change, its keys stay.
  tp::mapKeys(e, 0, 11, 5);
  expect(tp::replaceSample(e, 5, "/x/chime.wav", 2000, 44100, "chime") == 5 && fileOf(e, 5) == "/x/chime.wav" && keysOf(e, 5) == 12 &&
             e.text.sampleNames[5] == "chime",
         "load over entry 06: the new file, its keys stay");
  expect(e.text.files.size() == 2, "the file list keeps only the files in use");
}

void testRegionToEntry() {
  const tp::Edit base = fixture();
  // To the next free entry (03).
  {
    tp::Edit e = base;
    const std::vector<uint8_t> keys(e.text.def.keymap.begin(), e.text.def.keymap.end());
    const int k = tp::regionToEntry(e, 0, tp::Span{1000, 6000}, -1);
    expect(k == 2, "range to the next free entry: entry 03");
    const trk::SampleDef& s = e.text.def.samples[2];
    expect(fileOf(e, 2) == "/fixture/kick.wav" && e.text.files.size() == 2, "the same file, nothing new written");
    expect(e.cuts.start[2] == 1000 && e.cuts.end[2] == 6000, "the range as the entry's start and end");
    expect(s.volume == 40 && s.pan == 200 && s.finetune == -12 && s.relNote == 5 && e.eightBit[2],"the source's volume, pan, fine tune, transpose, bits");
    expect(s.loop == trk::kLoopPingPong && s.loopStart == 2000 && s.loopLength == 3000, "a loop inside the range stays");
    expect(e.text.sampleNames[2] == "kick part", "named after the source");
    expect(std::equal(keys.begin(), keys.end(), e.text.def.keymap.begin()) && keysOf(e, 2) == 0, "a free entry gets no keys");
    expect(e.cuts.start[0] == 100 && e.cuts.end[0] == 8400 && e.text.def.samples[0].loopStart == 2000, "the source stays as it was");
    // A part of the part keeps the name; a loop that is not inside goes.
    const int k2 = tp::regionToEntry(e, 2, tp::Span{3000, 4500}, -1);
    expect(k2 == 3 && e.text.sampleNames[3] == "kick part" && e.text.def.samples[3].loop == trk::kLoopOff && e.text.def.samples[3].loopLength == 0,
           "a range that cuts the loop: no loop");
  }
  // To a picked free entry (0A).
  {
    tp::Edit e = base;
    expect(tp::regionToEntry(e, 1, tp::Span{0, 1000}, 9) == 9 && fileOf(e, 9) == "/fixture/snare.wav" && e.cuts.end[9] == 1000 && keysOf(e, 9) == 0,
           "range to entry 0A");
    expect(!hasSample(e, 2), "the entries between stay empty");
  }
  // Over a used entry (02): replaced, its keys stay.
  {
    tp::Edit e = base;
    const int before = keysOf(e, 1);
    expect(tp::regionToEntry(e, 0, tp::Span{500, 2500}, 1) == 1 && fileOf(e, 1) == "/fixture/kick.wav" && keysOf(e, 1) == before && before == 48,
           "range over entry 02: replaced, its 48 keys stay");
    expect(e.text.files.size() == 1, "the snare's file is no longer in the list");
  }
  // Over the source itself: the sample becomes its range.
  {
    tp::Edit e = base;
    expect(tp::regionToEntry(e, 0, tp::Span{1500, 7000}, 0) == 0 && e.cuts.start[0] == 1500 && e.cuts.end[0] == 7000 && e.text.def.samples[0].loop == trk::kLoopPingPong,
           "range over its own entry");
  }
  // Nothing to do.
  {
    tp::Edit e = base;
    const std::string t = tp::writeEdit(e);
    expect(tp::regionToEntry(e, 0, tp::Span{3000, 3000}, -1) == -1, "an empty range: nothing");
    expect(tp::regionToEntry(e, 4, tp::Span{0, 100}, -1) == -1, "an empty source entry: nothing");
    expect(tp::writeEdit(e) == t, "nothing changed");
    for (int k = 2; k < trk::kMaxSamples; ++k) tp::replaceSample(e, k, "/x/f" + std::to_string(k) + ".wav", 100, 44100, "f");
    expect(tp::firstFreeEntry(e) == -1 && tp::regionToEntry(e, 0, tp::Span{0, 50}, -1) == -1, "all 16 used: no next free entry");
    expect(tp::regionToEntry(e, 0, tp::Span{0, 50}, 15) == 15, "all 16 used: a picked entry is replaced");
  }
}

// The edits as the session commits them (the channel's plugin data, one document step each): undo
// takes each back whole, and the project file carries the result.
void testUndoAndRoundTrip() {
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Kit";
  ch.plugin = defaultPluginState(trk::kInstrumentType);
  ch.plugin.data = tp::writeEdit(fixture());
  s.channels.push_back(ch);
  Document doc(s);
  const ObjectId id = ch.id;
  auto data = [&] { return doc.song().channel(id)->plugin.data; };
  const std::string d0 = data();
  auto step = [&](const char* label, auto f) {
    tp::Edit e = tp::readEdit(data(), 0);
    f(e);
    doc.editChannel(id, label, [&](Channel& c) { c.plugin.data = tp::writeEdit(e); });
  };
  step("load sample", [](tp::Edit& e) { tp::replaceSample(e, 4, "/x/pad.wav", 5000, 44100, "pad"); });
  const std::string d1 = data();
  step("range to entry", [](tp::Edit& e) { tp::regionToEntry(e, 0, tp::Span{1000, 6000}, -1); });
  const std::string d2 = data();
  step("range to entry", [](tp::Edit& e) { tp::regionToEntry(e, 0, tp::Span{200, 900}, 7); });
  const std::string d3 = data();
  step("range to entry", [](tp::Edit& e) { tp::regionToEntry(e, 0, tp::Span{200, 900}, 1); });
  expect(d1 != d0 && d2 != d1 && d3 != d2 && data() != d3, "four edits, each a change");
  // Save and load the project: the entries come back byte for byte.
  {
    Song back;
    std::string err;
    const bool ok = songFromJson(songToJson(doc.song()), back, &err);
    const Channel* c = ok ? back.channel(id) : nullptr;
    expect(c && c->plugin.data == data(), "project round trip: the instrument text is the same" + (err.empty() ? std::string() : " (" + err + ")"));
    const tp::Edit e = tp::readEdit(c ? c->plugin.data : std::string(), 0);
    expect(e.cuts.start[2] == 1000 && e.cuts.end[2] == 6000 && e.cuts.start[7] == 200 && e.cuts.end[7] == 900 && hasSample(e, 4) && keysOf(e, 1) == 48,
           "project round trip: the entries' cuts, files and keys");
    expect(tp::writeEdit(e) == data(), "instrument text round trip: written back byte for byte");
  }
  doc.undo();
  expect(data() == d3, "undo: range over a used entry, one step");
  doc.undo();
  expect(data() == d2, "undo: range to a picked entry, one step");
  doc.undo();
  expect(data() == d1, "undo: range to the next free entry, one step");
  doc.undo();
  expect(data() == d0, "undo: load into an entry, one step");
  doc.redo();
  expect(data() == d1, "redo");
}

std::string writeRamp(const std::string& dir, int n) {
  AudioBuffer b(1, static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) b.data[static_cast<size_t>(i)] = 0.05f + 0.9f * static_cast<float>(i) / static_cast<float>(n);
  const std::string p = (fs::u8path(dir) / "ramp.wav").u8string();
  writeWav(p, b, 44100, 24);
  return p;
}

// The region entry plays the range of the same file, from the keys mapped to it.
void testPlaysFromKeys() {
  const fs::path dir = fs::temp_directory_path() / "gridline-k078-entries";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const int n = 8000;
  const std::string path = writeRamp(dir.u8string(), n);
  tp::Edit e = tp::readEdit("", 0);
  e.text.def.vol.flags = 0;
  tp::addSample(e, path, n, 44100, "ramp");
  const int k = tp::regionToEntry(e, 0, tp::Span{3000, 7000}, -1);
  expect(k == 1, "the range is entry 02");
  tp::mapKeys(e, 48, 59, k);  // C-4 .. B-4 play it (MIDI 60 is key 48)
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = 0;
  st.params[trk::PInterp] = trk::InterpNone;
  st.params[trk::PRamp] = 0;
  st.data = tp::writeEdit(e);
  auto g = packs::createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  const auto* inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
  expect(inst && inst->data[1] && inst->files[1] && inst->data[1] == inst->files[1]->left.data() + 3000, "entry 02 plays from frame 3000 of the same file");
  expect(inst && inst->text.def.samples[1].length == 4000, "entry 02 is 4000 frames long");
  if (!inst) return;
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  std::vector<float> L(256), R(256);
  g->noteOn(1, 60, 100.0f / 127.0f, 0);
  g->process(st, aux, ctx, L.data(), R.data(), 256);
  tp::TapVoice v[tp::kTapVoices];
  const int nv = tp::readPlayTap(inst, v, tp::kTapVoices);
  expect(nv == 1 && v[0].slot == 1, "a key mapped to entry 02 plays entry 02 (the tap's slot)");
  // Its first output is the ramp's value at frame 3000 (the gain: the first block's ratio to entry 01's start is not needed).
  const double expectFirst = 0.05 + 0.9 * 3000.0 / n;
  const double ratio = L[1] / L[0], want = (0.05 + 0.9 * 3001.0 / n) / expectFirst;
  expect(std::abs(ratio - want) < 1e-3 && L[0] > 0, "it plays the range's frames");
  g.reset();  // (its tap goes with it: the second generator plays the same cached instrument)
  // A key still mapped to entry 01 plays entry 01.
  auto g2 = packs::createGenerator(st.type);
  ChannelAux aux2;
  packs::prepareAux(st, aux2);
  const auto* inst2 = dynamic_cast<const tp::InstrumentAux*>(aux2.custom.get());
  g2->noteOn(1, 40, 100.0f / 127.0f, 0);
  g2->process(st, aux2, ctx, L.data(), R.data(), 256);
  expect(inst2 && tp::readPlayTap(inst2, v, tp::kTapVoices) >= 1 && v[0].slot == 0, "a key left on entry 01 plays entry 01");
}

void testBackwardsRoundTrip() {
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Kit";
  ch.plugin = defaultPluginState(trk::kInstrumentType);
  ch.plugin.data = tp::writeEdit(fixture());
  s.channels.push_back(ch);
  Document doc(s);
  const ObjectId id = ch.id;
  auto data = [&] { return doc.song().channel(id)->plugin.data; };
  const std::string d0 = data();
  // An old project's text (no Backwards line) reads and writes back as it was.
  expect(tp::writeEdit(tp::readEdit(d0, 0)) == d0 && d0.find("\nrev ") == std::string::npos, "old text: read and written the same");
  doc.editChannel(id, "sample play direction", [&](Channel& c) {
    tp::Edit e = tp::readEdit(c.plugin.data, 0);
    e.text.def.samples[1].reverse = true;
    c.plugin.data = tp::writeEdit(e);
  });
  const std::string d1 = data();
  expect(d1 != d0 && tp::readEdit(d1, 0).text.def.samples[1].reverse, "backwards: on, in the text");
  {
    Song back;
    std::string err;
    const bool ok = songFromJson(songToJson(doc.song()), back, &err);
    const Channel* c = ok ? back.channel(id) : nullptr;
    expect(c && c->plugin.data == d1, "project round trip: backwards comes back byte for byte");
    const tp::Edit e = tp::readEdit(c ? c->plugin.data : std::string(), 0);
    expect(e.text.def.samples[1].reverse && !e.text.def.samples[0].reverse, "project round trip: on its sample only");
  }
  // The entry edits carry it with the sample.
  {
    tp::Edit e = tp::readEdit(d1, 0);
    const int dup = tp::duplicateSample(e, 1);
    expect(dup >= 0 && e.text.def.samples[static_cast<size_t>(dup)].reverse, "duplicate: the copy plays backwards too");
    tp::exchangeSampleData(e, 0, 1);
    expect(e.text.def.samples[0].reverse && !e.text.def.samples[1].reverse, "exchange: backwards goes with the sample");
    const int part = tp::regionToEntry(e, 0, tp::Span{100, 2000}, -1);
    expect(part >= 0 && e.text.def.samples[static_cast<size_t>(part)].reverse, "range to entry: the part plays backwards as its source");
    tp::removeSample(e, 0);
    expect(!e.text.def.samples[0].reverse, "remove: the emptied entry is plain");
  }
  doc.undo();
  expect(data() == d0, "undo: backwards off again, one step");
  doc.redo();
  expect(data() == d1, "redo: backwards on");
}

// 0.7.8: the destructive trim of the loop's unheard side follows the play direction.
void testBackwardsMinimise() {
  auto ramp = [](bool stereo) {
    tp::Wave w;
    w.stereo = stereo;
    for (int i = 0; i < 10000; ++i) w.L.push_back(static_cast<float>(i));
    if (stereo) w.R = w.L;
    w.loop = trk::kLoopForward;
    w.loopStart = 3000;
    w.loopLength = 2000;
    w.cutA = 1000;
    w.cutB = 9000;
    return w;
  };
  // Forward: as before, what follows the loop goes and the cuts keep their frames.
  tp::Wave f = ramp(true);
  expect(tp::waveMinimise(f), "forward: minimise changes the wave");
  expect(f.frames() == 5000 && f.L.back() == 4999.0f && f.R.size() == 5000, "forward: the sample ends with its loop");
  expect(f.loopStart == 3000 && f.loopLength == 2000 && f.cutA == 1000 && f.cutB == 0, "forward: loop and start where they were, the end is the end");
  // Backwards: what precedes the loop goes; the loop starts the sample, the end cut moves with the audio.
  tp::Wave b = ramp(true);
  b.reverse = true;
  expect(tp::waveMinimise(b), "backwards: minimise changes the wave");
  expect(b.frames() == 7000 && b.L.front() == 3000.0f && b.L.back() == 9999.0f && b.R.size() == 7000 && b.R.front() == 3000.0f,
         "backwards: the part before the loop is gone, the part after it stays (both sides)");
  expect(b.loopStart == 0 && b.loopLength == 2000 && b.loop == trk::kLoopForward, "backwards: the loop now starts the sample");
  expect(b.cutA == 0 && b.cutB == 6000, "backwards: the start cut (before the loop) goes, the end cut moves with the audio");
  expect(!tp::waveMinimise(b), "backwards: minimised again, nothing changes");
  // Ping-pong, mono.
  tp::Wave p = ramp(false);
  p.loop = trk::kLoopPingPong;
  p.reverse = true;
  expect(tp::waveMinimise(p) && p.frames() == 7000 && p.loopStart == 0 && p.loop == trk::kLoopPingPong, "backwards, ping-pong: the same");
  // Nothing to take: no loop, or a loop at the very start (forward: one at the very end).
  tp::Wave n = ramp(false);
  n.reverse = true;
  n.loop = trk::kLoopOff;
  n.loopLength = 0;
  expect(!tp::waveMinimise(n) && n.frames() == 10000, "backwards, no loop: nothing changes");
  tp::Wave z = ramp(false);
  z.reverse = true;
  z.loopStart = 0;
  expect(!tp::waveMinimise(z) && z.frames() == 10000, "backwards, loop at the start: nothing changes");
  tp::Wave e = ramp(false);
  e.loopStart = 8000;
  e.loopLength = 2000;
  expect(!tp::waveMinimise(e) && e.frames() == 10000, "forward, loop at the end: nothing changes");
  e.reverse = true;
  expect(tp::waveMinimise(e) && e.frames() == 2000 && e.L.front() == 8000.0f, "backwards, loop at the end: only the loop stays");
}

}  // namespace

int main() {
  testLoadIntoEntry();
  testRegionToEntry();
  testUndoAndRoundTrip();
  testPlaysFromKeys();
  testBackwardsRoundTrip();
  testBackwardsMinimise();
  return finish();
}
