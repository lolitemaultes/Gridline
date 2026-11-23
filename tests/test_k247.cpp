// K-247: the LFO board. Song-level LFOs that move any parameter.
//  1. Model and file: every LFO field round trips, defaults are not written, projects without LFOs
//     write exactly what they wrote before, a reader that does not know the LFOs drops their links.
//  2. Document: add, remove, edit, move with undo and redo; a knob drag is one step; removing an LFO
//     removes its links in the same step; loops through LFOs and macros are refused; several LFOs and
//     a controller link on one parameter live side by side.
//  3. Engine: each shape at known phases; On play follows the song position (same values on every
//     pass, at 120 and 97 BPM, dotted and triplet); Each note restarts; random shapes repeat; block
//     size does not change the movement; two LFOs add and clip; an LFO swings around a controller's
//     value; an LFO moves a macro that moves three parameters in the same block; an LFO moves another
//     LFO's depth in the same block; no allocation while it runs.
//  4. LFO settings are parameters: automation of the depth changes the output.
//  5. (0.7.8) Chaos and Random sources: file round trip, numbering, an unknown kind left out; Roll and
//     New for a knob are one undo step; the same seed from the same place plays the same values at
//     any block size; Bar, Play and Note restarts; calm against wild; bipolar and up only ranges;
//     Random's steps, Middle spread, quantise levels, glide, Note and Roll triggers; a source that
//     moves nothing is not run; no allocation; an export is bit-identical from run to run.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Lfo.h"
#include "core/Macros.h"
#include "core/Params.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

// A counting allocator: allocations made while `gCounting` is set.
namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
// GCC 11 and later warn on these frees once they are inlined next to the library's calls to
// operator new; the replaced operator new above uses malloc, so they do match.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

using namespace gl;

namespace {

constexpr double kSr = 48000;

bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }

struct Rig {
  Song s = makeEmptySong();
  ObjectId lead = kNoId, pad = kNoId, track = kNoId;
  Rig() {
    lead = add("Lead", "gl.synth");
    pad = add("Pad", "gl.synth");
    track = s.mixer[1].id;
    s.mixer[1].slots[0].plugin = defaultPluginState("gl.filter");
    s.channel(lead)->mixerTrack = track;
    s.tempo = 120;
  }
  ObjectId add(const std::string& name, const std::string& type) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.plugin = defaultPluginState(type);
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    return c.id;
  }
};

// A constant LFO: depth 0 and an offset, so its output is the offset (bipolar).
SongLfo constant(float v) {
  SongLfo l;
  l.depth = 0;
  l.offset = v;
  return l;
}

// --------------------------------------------------------------- 1. model and file

void testModelAndFile() {
  Rig r;
  Document d(r.s);
  SongLfo l;
  l.name = "Wobble";
  l.colour = 3;
  l.on = false;
  l.shape = LfoShape::Drawn;
  l.pulseWidth = 0.25f;
  l.randomSteps = 8;
  l.seed = 77;
  l.points = {{0, -1, 0.5f}, {0.5f, 1, -0.25f}};
  l.drawnSteps = true;
  l.tempoSync = false;
  l.rateNote = 7;
  l.rateKind = 2;
  l.rateHz = 3.5f;
  l.phase = 90;
  l.depth = 0.8f;
  l.offset = -0.2f;
  l.smoothMs = 30;
  l.fadeIn = 2;
  l.unipolar = true;
  l.restart = LfoRestart::EachNote;
  l.noteChannel = r.lead;
  const ObjectId id = d.addLfo(l);
  expect(id != kNoId && d.song().lfos.size() == 1, "add an LFO");
  const ObjectId plain = d.addLfo(SongLfo{});
  const ParamId cut = channelPluginParam(r.lead, pp::SynOsc1Level);
  expect(lfo::link(d, id, cut, -0.4f), "link it to a knob");
  d.editLfoLink(id, cut, "set", [](ModLink& x) { x.direction = kModDownOnly, x.invert = true, x.smoothMs = 12; });
  const Json j = songToJson(d.song());
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), "load it back");
  expect(back.lfos == d.song().lfos, "every LFO field round trips");
  expect(back.modLinks == d.song().modLinks, "LFO links round trip (amount, mode, direction, invert)");
  // Defaults are not written: the plain LFO is its id alone.
  const Json* lj = j.find("lfos");
  expect(lj && lj->array.size() == 2 && lj->array[1].object.size() == 1 && lj->array[1].find("id"), "an LFO at its defaults writes only its id");
  (void)plain;
  // A project without LFOs writes no "lfos" key and its links no new keys.
  Rig old;
  Document od(old.s);
  macro::link(od, channelPluginParam(old.lead, pp::SynOsc1Level), old.lead, 0);
  const Json oj = songToJson(od.song());
  bool newKeys = false;
  if (const Json* ml = oj.find("modLinks"))
    for (const auto& e : ml->array)
      for (const char* k : {"amount", "mode", "direction", "invert"}) newKeys = newKeys || e.find(k) != nullptr;
  expect(!oj.find("lfos") && !newKeys, "a project without LFOs saves exactly as before");
  Song ob;
  expect(songFromJson(oj, ob) && ob.modLinks.size() == 1 && ob.modLinks[0].mode == kModAbsolute && ob.modLinks[0].amount == 0,
         "older links load as Between two values (Absolute)");
  // A reader that does not know the board (no "lfos" key) opens the song and drops the LFO links.
  Json stripped = j;
  stripped.object.erase("lfos");
  Song sb;
  expect(songFromJson(stripped, sb), "the song opens without its board");
  expect(sb.lfos.empty() && std::none_of(sb.modLinks.begin(), sb.modLinks.end(), [](const ModLink& m) { return m.isLfo(); }),
         "links whose LFO is missing are dropped");
  // An LFO's settings are parameters.
  const ParamId depth = lfo::lfoParam(id, lfo::LfoParam::Depth);
  expect(describeParam(d.song(), depth).has_value() && near(*getParam(d.song(), depth), 0.8), "an LFO's depth is a parameter");
  const auto all = listParams(d.song());
  expect(std::find(all.begin(), all.end(), depth) != all.end(), "LFO settings are in the automation lists");
  expect(paramDisplayName(d.song(), depth).find("LFO 1 Wobble") != std::string::npos, "its name reads LFO 1 Wobble");
  expect(lfo::targetLabel(d.song(), cut).find("Lead · ") == 0, "a target reads Owner · Parameter");
}

// --------------------------------------------------------------- 2. document

void testDocument() {
  Rig r;
  Document d(r.s);
  const Song& s = d.song();
  const size_t h0 = d.history().position();
  const ObjectId a = lfo::addNew(d);
  const ObjectId b = lfo::addNew(d);
  expect(s.lfos.size() == 2 && s.lfos[0].colour != s.lfos[1].colour, "two LFOs, each its own colour");
  expect(d.history().position() == h0 + 2, "adding is one step each");
  d.undo();
  expect(s.lfos.size() == 1 && s.lfo(a), "undo an add");
  d.redo();
  expect(s.lfos.size() == 2 && s.lfo(b), "redo an add");
  // A knob drag (one merge key) is one step.
  const size_t h1 = d.history().position();
  for (int k = 0; k < 10; ++k) d.editLfo(a, "change LFO depth", [k](SongLfo& l) { l.depth = 0.1f * static_cast<float>(k); }, 0x1234);
  d.endGesture();
  expect(d.history().position() == h1 + 1 && near(s.lfo(a)->depth, 0.9), "a knob drag is one step");
  d.undo();
  expect(near(s.lfo(a)->depth, 1.0), "undo the drag");
  d.redo();
  expect(d.editLfo(a, "change LFO rate", [](SongLfo& l) { l.rateHz = 1000; }) && s.lfo(a)->rateHz == lfo::kMaxHz, "settings are clamped");
  expect(d.moveLfo(b, 0) && s.lfos[0].id == b, "move an LFO on the board");
  d.undo();
  expect(s.lfos[0].id == a, "undo a move");
  // Links: an LFO link beside a controller link; a second link from the same LFO edits the first.
  const ParamId cut = channelPluginParam(r.lead, pp::SynOsc1Level);
  expect(macro::link(d, cut, r.lead, 0), "a macro drives the knob");
  expect(lfo::link(d, a, cut) && lfo::link(d, b, cut), "two LFOs on the same knob");
  expect(findModLink(s, cut.owner, cut.index) && findModLink(s, cut.owner, cut.index)->sourceSlot == kLayerMacroSlot &&
             lfoLinksOn(s, cut.owner, cut.index).size() == 2,
         "the controller link stays, the LFO links add beside it");
  const ModLink* la = findLfoLink(s, a, cut.owner, cut.index);
  expect(la && la->mode == kModAround && near(la->amount, 0.25), "a new target is 25 percent around the knob");
  d.editLfoLink(a, cut, "change LFO amount", [](ModLink& x) { x.amount = 0.6f; });
  expect(lfo::link(d, a, cut) && lfoLinksOn(s, cut.owner, cut.index).size() == 2 && near(findLfoLink(s, a, cut.owner, cut.index)->amount, 0.6),
         "linking the same LFO again keeps its one link and its amount");
  expect(d.removeModLink(cut) && !findModLink(s, cut.owner, cut.index) && lfoLinksOn(s, cut.owner, cut.index).size() == 2,
         "removing the controller link leaves the LFO links");
  // Removing an LFO removes its links in one step.
  const size_t h2 = d.history().position();
  expect(d.removeLfo(a) && !s.lfo(a) && lfoLinksOn(s, cut.owner, cut.index).size() == 1, "remove an LFO and its links");
  expect(d.history().position() == h2 + 1, "in one step");
  d.undo();
  expect(s.lfo(a) && lfoLinksOn(s, cut.owner, cut.index).size() == 2, "undo brings both back");
  // Loops are refused: LFO a moves Macro 2 of the pad, Macro 2 moves LFO a's depth.
  expect(lfo::link(d, a, layerMacroParam(r.pad, 1)), "an LFO moves a macro");
  expect(!macro::link(d, lfo::lfoParam(a, lfo::LfoParam::Depth), r.pad, 1), "the macro cannot move that LFO's depth");
  expect(d.lastLinkRefusal().find("This would make a loop: Macro 2 moves LFO 1") == 0, "the refusal names the loop");
  expect(!lfo::link(d, a, lfo::lfoParam(a, lfo::LfoParam::Depth)), "an LFO cannot move its own depth");
  expect(lfo::link(d, b, lfo::lfoParam(a, lfo::LfoParam::Depth)), "an LFO may move another LFO's depth");
  expect(!lfo::link(d, a, lfo::lfoParam(b, lfo::LfoParam::Phase)), "but not back");
  // Removing a channel removes the LFO links to its parameters.
  d.removeChannel(r.lead);
  expect(lfoLinksOn(s, cut.owner, cut.index).empty() && s.lfos.size() == 2, "removing a channel removes LFO links to it, the LFOs stay");
  // The board holds at most 32.
  for (int k = 0; k < 40; ++k) lfo::addNew(d);
  expect(s.lfos.size() == static_cast<size_t>(kMaxSongLfos), "at most 32 LFOs per song");
  // A new LFO for a knob: one step.
  Rig r2;
  Document d2(r2.s);
  const size_t h3 = d2.history().position();
  const ObjectId n = lfo::newForKnob(d2, mixerParam(r2.track, MixerParam::Pan));
  expect(n != kNoId && d2.history().position() == h3 + 1 && lfoLinksOn(d2.song(), r2.track, static_cast<uint32_t>(MixerParam::Pan)).size() == 1,
         "New LFO for this knob is one step");
  expect(lfo::newForKnob(d2, songParam(SongParam::Tempo)) == kNoId, "the tempo cannot be a target");
}

// --------------------------------------------------------------- 3. engine

void testShapes() {
  using lfo::shapeValue;
  expect(near(shapeValue(LfoShape::Sine, 0.25, 0, 0.5f, 4, 1, nullptr), 1) && near(shapeValue(LfoShape::Sine, 0.75, 0, 0.5f, 4, 1, nullptr), -1),
         "sine");
  expect(near(shapeValue(LfoShape::Triangle, 0.25, 0, 0.5f, 4, 1, nullptr), 1) && near(shapeValue(LfoShape::Triangle, 0.5, 0, 0.5f, 4, 1, nullptr), 0) &&
             near(shapeValue(LfoShape::Triangle, 0.75, 0, 0.5f, 4, 1, nullptr), -1),
         "triangle");
  expect(near(shapeValue(LfoShape::SawUp, 0.0, 0, 0.5f, 4, 1, nullptr), -1) && near(shapeValue(LfoShape::SawUp, 0.5, 0, 0.5f, 4, 1, nullptr), 0), "saw up");
  expect(near(shapeValue(LfoShape::SawDown, 0.0, 0, 0.5f, 4, 1, nullptr), 1) && near(shapeValue(LfoShape::SawDown, 0.75, 0, 0.5f, 4, 1, nullptr), -0.5),
         "saw down");
  expect(shapeValue(LfoShape::Square, 0.2, 0, 0.25f, 4, 1, nullptr) == 1 && shapeValue(LfoShape::Square, 0.3, 0, 0.25f, 4, 1, nullptr) == -1,
         "square with a pulse width");
  const float h1 = shapeValue(LfoShape::RandomHold, 0.1, 3, 0.5f, 4, 9, nullptr), h2 = shapeValue(LfoShape::RandomHold, 0.2, 3, 0.5f, 4, 9, nullptr);
  const float h3 = shapeValue(LfoShape::RandomHold, 0.3, 3, 0.5f, 4, 9, nullptr);
  expect(h1 == h2 && h1 != h3 && h1 >= -1 && h1 <= 1, "random hold holds a value per step");
  expect(shapeValue(LfoShape::RandomHold, 0.1, 3, 0.5f, 4, 10, nullptr) != h1, "another seed, other values");
  const float g0 = shapeValue(LfoShape::RandomGlide, 0.0, 3, 0.5f, 4, 9, nullptr);
  expect(near(g0, shapeValue(LfoShape::RandomHold, 0.0, 3, 0.5f, 4, 9, nullptr)) &&
             near(shapeValue(LfoShape::RandomGlide, 0.9999, 3, 0.5f, 4, 9, nullptr), shapeValue(LfoShape::RandomHold, 0.0, 4, 0.5f, 4, 9, nullptr), 1e-3),
         "random glide runs from step to step, into the next cycle");
  SongLfo dl;
  dl.shape = LfoShape::Drawn;
  dl.points = {{0, -1, 0}, {0.5f, 1, 0}};
  lfo::DrawnTable t{};
  lfo::buildTable(dl, t);
  expect(near(shapeValue(LfoShape::Drawn, 0.25, 0, 0.5f, 4, 1, &t), 0, 1e-2) && near(shapeValue(LfoShape::Drawn, 0.5, 0, 0.5f, 4, 1, &t), 1, 1e-2) &&
             near(shapeValue(LfoShape::Drawn, 0.75, 0, 0.5f, 4, 1, &t), 0, 1e-2),
         "drawn: straight lines between the points, round to the start");
  dl.drawnSteps = true;
  lfo::buildTable(dl, t);
  expect(near(shapeValue(LfoShape::Drawn, 0.25, 0, 0.5f, 4, 1, &t), -1, 1e-6) && near(shapeValue(LfoShape::Drawn, 0.75, 0, 0.5f, 4, 1, &t), 1, 1e-6),
         "drawn steps hold each point");
  // Output and link shapes.
  expect(near(lfo::shapeOutput(1, 0.5f, 0.1f, false, 1), 0.6) && near(lfo::shapeOutput(-1, 1, 0, true, 1), 0) &&
             near(lfo::shapeOutput(1, 1, 0, false, 0.5f), 0.5),
         "depth, offset, Up only and the fade");
  lfo::LinkShape k;
  k.amount = 0.5f;
  expect(near(lfo::linkValue(k, 1), 0.5) && near(lfo::linkValue(k, -1), -0.5), "around the knob, both ways");
  k.direction = kModUpOnly;
  expect(near(lfo::linkValue(k, -1), 0) && near(lfo::linkValue(k, 1), 0.5), "up only");
  k.direction = kModDownOnly;
  expect(near(lfo::linkValue(k, 1), -0.5), "down only");
  k.direction = kModBothWays;
  k.invert = true;
  expect(near(lfo::linkValue(k, 1), -0.5), "invert");
  k.mode = kModAbsolute;
  k.invert = false;
  k.min = 0.2f, k.max = 0.6f;
  expect(near(lfo::linkValue(k, 0), 0.4), "between two values");
}

// Plays `s` from the start in control blocks of `block` frames: the LFO's output after each call.
std::vector<float> playLfo(const Song& s, ObjectId id, int calls, int block = kControlBlock, std::vector<float>* phases = nullptr,
                           bool loop = false, std::vector<double>* ticks = nullptr) {
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setMode(PlayMode::Song);
  eng.setLooping(loop);
  if (loop) eng.setLoopRange(0, s.barTicks());  // one bar
  eng.setProgram(pb.build(s));
  eng.play();
  std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block)), out;
  for (int c = 0; c < calls; ++c) {
    eng.process(l.data(), r.data(), block);
    const auto rt = pb.lfoRuntime(id);
    out.push_back(rt ? rt->display.load() : -9);
    if (phases) phases->push_back(rt ? rt->phaseUi.load() : -9);
    if (ticks) ticks->push_back(eng.position());
  }
  eng.stop();
  return out;
}

void testOnPlay() {
  struct Case {
    double bpm;
    int note, kind;
  };
  for (const Case& c : {Case{120, 5, 0}, Case{97, 5, 0}, Case{120, 6, 1}, Case{97, 7, 2}, Case{120, 3, 0}}) {
    Rig r;
    r.s.tempo = c.bpm;
    Document d(r.s);
    SongLfo l;
    l.rateNote = c.note;
    l.rateKind = c.kind;
    const ObjectId id = d.addLfo(l);
    std::vector<float> ph;
    const auto v = playLfo(d.song(), id, 300, kControlBlock, &ph);
    const double beats = lfo::rateBeats(c.note, c.kind);
    bool ok = true;
    for (int k = 0; k < 300; ++k) {
      const double cycles = k * kControlBlock / kSr * c.bpm / 60.0 / beats;
      const double want = cycles - std::floor(cycles);
      const double got = ph[static_cast<size_t>(k)];
      ok = ok && (near(got, want, 1e-4) || near(std::fabs(got - want), 1.0, 1e-4));
      ok = ok && near(v[static_cast<size_t>(k)], std::sin(6.283185307179586 * got), 1e-4);
    }
    char name[96];
    std::snprintf(name, sizeof name, "On play follows the song position (%.0f BPM, %s%s)", c.bpm, lfo::kRateNames[c.note],
                  c.kind == 1 ? " dotted" : c.kind == 2 ? " triplet" : "");
    expect(ok, name);
  }
  // The same values on every play, and on each pass round a loop: every block that starts at the same
  // song position after the loop back plays the same value as on the first pass.
  Rig r;
  Document d(r.s);
  SongLfo l;
  l.shape = LfoShape::RandomHold;
  l.randomSteps = 16;
  l.rateNote = 3;  // 1/1
  const ObjectId id = d.addLfo(l);
  std::vector<double> ticks;
  const auto a = playLfo(d.song(), id, 13000, kControlBlock, nullptr, true, &ticks);
  const auto b = playLfo(d.song(), id, 13000, kControlBlock, nullptr, true);
  expect(a == b, "random hold plays the same values on every play");
  size_t back = 0;  // the first block after the loop back
  for (size_t k = 1; k < ticks.size() && !back; ++k)
    if (ticks[k] < ticks[k - 1]) back = k;
  std::map<double, float> first;
  int same = 0, differ = 0;
  for (size_t k = 0; k < back; ++k) first.emplace(ticks[k], a[k]);
  // (ticks are where each block ends; a block that wraps started before the loop end, so it is skipped)
  for (size_t k = back; k < ticks.size(); ++k)
    if (ticks[k] > ticks[k - 1])
      if (auto it = first.find(ticks[k]); it != first.end()) (it->second == a[k] ? same : differ)++;
  char loopMsg[128];
  std::snprintf(loopMsg, sizeof loopMsg, "and on each pass round the loop (back at block %zu, %d the same, %d not)", back, same, differ);
  expect(back > 0 && same > 100 && differ == 0, loopMsg);
}

void testFreeAndNotes() {
  Rig r;
  Document d(r.s);
  SongLfo l;
  l.restart = LfoRestart::Free;
  l.tempoSync = false;
  l.rateHz = 3;
  l.noteChannel = r.lead;
  const ObjectId id = d.addLfo(l);
  std::vector<float> ph;
  playLfo(d.song(), id, 100, kControlBlock, &ph);
  expect(near(ph[0], 0) && near(ph[10], 10 * 64 / kSr * 3, 1e-5), "Free runs on its own clock");
  // Each note: a note of its channel restarts it.
  d.editLfo(id, "restart", [](SongLfo& x) { x.restart = LfoRestart::EachNote; });
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setProgram(pb.build(d.song()));
  std::vector<float> L(64), R(64);
  for (int k = 0; k < 50; ++k) eng.process(L.data(), R.data(), 64);
  const auto rt = pb.lfoRuntime(id);
  expect(rt && rt->phaseUi.load() > 0.1f, "it runs between notes");
  eng.noteOn(r.lead, 60, 0.8f);
  for (int k = 0; k < 2; ++k) eng.process(L.data(), R.data(), 64);
  expect(rt->phaseUi.load() < 0.01f, "a note of its channel restarts it");
  for (int k = 0; k < 20; ++k) eng.process(L.data(), R.data(), 64);
  const float before = rt->phaseUi.load();
  eng.noteOn(r.pad, 62, 0.8f);
  for (int k = 0; k < 2; ++k) eng.process(L.data(), R.data(), 64);
  expect(rt->phaseUi.load() > before, "another channel's note does not");
}

void testBlockSize() {
  for (LfoRestart rs : {LfoRestart::OnPlay, LfoRestart::Free}) {
    Rig r;
    Document d(r.s);
    SongLfo l;
    l.shape = LfoShape::RandomGlide;
    l.restart = rs;
    l.smoothMs = 20;
    l.rateNote = 6;
    const ObjectId id = d.addLfo(l);
    const auto a = playLfo(d.song(), id, 64, 1024);
    const auto b = playLfo(d.song(), id, 256, 256);
    const auto c = playLfo(d.song(), id, 1024, 64);
    bool same = true;
    for (int k = 0; k < 64; ++k) same = same && a[static_cast<size_t>(k)] == b[static_cast<size_t>(k * 4 + 3)] && a[static_cast<size_t>(k)] == c[static_cast<size_t>(k * 16 + 15)];
    expect(same, rs == LfoRestart::OnPlay ? "block size does not change the movement (On play)" : "block size does not change the movement (Free)");
  }
}

// One control block of the song; the program after it.
std::shared_ptr<const Program> oneBlock(const Song& s, ProgramBuilder& pb, AudioEngine& eng, int blocks = 1) {
  eng.setProgram(pb.build(s));
  std::vector<float> l(64), r(64);
  for (int k = 0; k < blocks; ++k) eng.process(l.data(), r.data(), 64);
  return eng.program();
}

void testTargets() {
  Rig r;
  Document d(r.s);
  const ParamId vol = mixerParam(r.track, MixerParam::Volume);  // 0..1.25, 0.8
  const ObjectId a = d.addLfo(constant(1)), b = d.addLfo(constant(1));
  lfo::link(d, a, vol, 0.1f);
  lfo::link(d, b, vol, 0.05f);
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    auto p = oneBlock(d.song(), pb, eng);
    const double want = 1.25 * (0.8 / 1.25 + 0.15);
    double got = 0, base = 0;
    expect(pb.lfoValue(vol, got) && near(got, want, 1e-4), "two LFOs on one target add up");
    expect(near(p->mixer[static_cast<size_t>(d.song().mixerIndex(r.track))].modVolume, want, 1e-4), "the engine plays the moved value");
    expect(pb.modulatedValue(vol, base) && near(base, 0.8, 1e-4), "the knob keeps showing its own value");
  }
  d.editLfoLink(a, vol, "amount", [](ModLink& x) { x.amount = 0.9f; });
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    oneBlock(d.song(), pb, eng);
    double got = 0;
    expect(pb.lfoValue(vol, got) && near(got, 1.25), "the sum is clipped to the range");
  }
  d.editLfo(b, "off", [](SongLfo& x) { x.on = false; });
  d.editLfo(a, "off", [](SongLfo& x) { x.on = false; });
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    auto p = oneBlock(d.song(), pb, eng);
    expect(near(p->mixer[static_cast<size_t>(d.song().mixerIndex(r.track))].modVolume, 0.8, 1e-6), "LFOs switched off leave the target alone");
  }
  // Around a controller's value: a macro sets the pan, the LFO swings around it.
  Rig r2;
  Document d2(r2.s);
  const ParamId pan = mixerParam(r2.track, MixerParam::Pan);
  macro::link(d2, pan, r2.track, 0);
  d2.editModLink(pan, "range", [](ModLink& x) { x.min = 0, x.max = 1; });
  d2.setParam(layerMacroParam(r2.track, 0), 0.25, false);  // pan -0.5
  const ObjectId c = d2.addLfo(constant(1));
  lfo::link(d2, c, pan, 0.1f);
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    oneBlock(d2.song(), pb, eng);
    double got = 0;
    expect(pb.lfoValue(pan, got) && near(got, -0.3, 1e-4), "an LFO swings around a controller link's value");
  }
  // Between two values ignores the knob.
  d2.editLfoLink(c, pan, "mode", [](ModLink& x) { x.mode = kModAbsolute, x.min = 0.9f, x.max = 0.1f; });  // output 1 -> 0.1
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    oneBlock(d2.song(), pb, eng);
    double got = 0;
    expect(pb.lfoValue(pan, got) && near(got, -0.8, 1e-4), "between two values sweeps its own range");
  }
}

void testMacroAndLfoChains() {
  Rig r;
  Document d(r.s);
  // LFO -> the lead's Macro 1 -> three parameters, all in the first control block.
  const ParamId m1 = layerMacroParam(r.lead, 0);
  const ParamId p1 = channelPluginParam(r.lead, pp::SynOsc1Level), p2 = mixerParam(r.track, MixerParam::Pan), p3 = slotMixParam(r.track, 0);
  for (ParamId t : {p1, p2, p3}) {
    macro::link(d, t, r.lead, 0);
    d.editModLink(t, "range", [](ModLink& x) { x.min = 0, x.max = 1; });
  }
  const ObjectId a = d.addLfo(constant(1));
  expect(lfo::link(d, a, m1, 0.5f), "an LFO moves a macro");
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    auto p = oneBlock(d.song(), pb, eng);
    const Song& s = d.song();
    const ParamDesc d1 = *describeParam(s, p1);
    const auto& ch = p->channels[static_cast<size_t>(s.channelIndex(r.lead))];
    const auto& mx = p->mixer[static_cast<size_t>(s.mixerIndex(r.track))];
    expect(near(ch.macroLive[0], 0.5, 1e-5), "the macro moves");
    expect(near(ch.live.params[pp::SynOsc1Level], d1.fromNormalized(0.5), 1e-4) && near(mx.modPan, 0.0, 1e-4) && near(mx.slots[0].mix, 0.5, 1e-4),
           "and everything it drives, in the same block");
  }
  // LFO A moves LFO B's depth; B moves the pad's pan; all in the first block.
  Rig r2;
  Document d2(r2.s);
  const ObjectId A = d2.addLfo(constant(1));
  SongLfo bl;
  bl.shape = LfoShape::Square;
  bl.pulseWidth = 0.98f;  // +1 from the start
  bl.depth = 0;
  const ObjectId B = d2.addLfo(bl);
  const ParamId pan = mixerParam(r2.track, MixerParam::Pan);
  expect(lfo::link(d2, A, lfo::lfoParam(B, lfo::LfoParam::Depth), 0.5f) && lfo::link(d2, B, pan, 0.5f), "LFO to LFO to a knob");
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    oneBlock(d2.song(), pb, eng);
    double got = 0;
    expect(pb.lfoValue(pan, got) && near(got, 0.5, 1e-4), "an LFO moves another LFO's depth in the same block");
  }
  // Automation of an LFO's depth changes its output (LFO settings are parameters).
  Song s = d2.song();
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = B;
  ac.index = static_cast<uint32_t>(lfo::LfoParam::Depth);
  ac.points = {{0, 0.25f, 0, AutoShape::Hold}, {768, 0.25f, 0, AutoShape::Hold}};
  s.automations.push_back(ac);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = ac.id, c.start = 0, c.length = 768;
  s.playlist.push_back(c);
  s.modLinks.erase(std::remove_if(s.modLinks.begin(), s.modLinks.end(), [&](const ModLink& l) { return l.owner == B; }), s.modLinks.end());
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setMode(PlayMode::Song);
    eng.setProgram(pb.build(s));
    eng.play();
    std::vector<float> l(64), rr(64);
    for (int k = 0; k < 4; ++k) eng.process(l.data(), rr.data(), 64);
    double got = 0;
    expect(pb.lfoValue(pan, got) && near(got, 0.25, 1e-4), "an automation clip on an LFO's depth changes what it does");
    eng.stop();
  }
}

void testNoAllocation() {
  Rig r;
  Document d(r.s);
  for (int k = 0; k < 8; ++k) {
    SongLfo l;
    l.shape = static_cast<LfoShape>(k % static_cast<int>(LfoShape::Count));
    l.restart = k % 2 ? LfoRestart::Free : LfoRestart::OnPlay;
    l.smoothMs = 10;
    const ObjectId id = d.addLfo(l);
    lfo::link(d, id, mixerParam(r.track, MixerParam::Pan), 0.1f);
    lfo::link(d, id, channelPluginParam(r.lead, pp::SynOsc1Level), 0.2f);
    lfo::link(d, id, layerMacroParam(r.pad, 2), 0.2f);
  }
  macro::link(d, slotMixParam(r.track, 0), r.pad, 2);
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setMode(PlayMode::Song);
  eng.setProgram(pb.build(d.song()));
  eng.play();
  std::vector<float> l(256), rr(256);
  for (int k = 0; k < 8; ++k) eng.process(l.data(), rr.data(), 256);  // warm up
  gAllocs = 0;
  gCounting = true;
  for (int k = 0; k < 64; ++k) eng.process(l.data(), rr.data(), 256);
  gCounting = false;
  if (gAllocs.load() != 0) std::printf("# %ld allocations while playing\n", gAllocs.load());
  expect(gAllocs.load() == 0, "no allocation while 8 LFOs run");
  eng.stop();
}

void testOldProjectsUnchanged() {
  // A song without LFOs compiles no LFO pass and no late links: the links run exactly as before.
  Rig r;
  Document d(r.s);
  macro::link(d, channelPluginParam(r.lead, pp::SynOsc1Level), r.lead, 0);
  macro::link(d, layerMacroParam(r.pad, 0), r.lead, 1);
  ProgramBuilder pb;
  const auto p = pb.build(d.song());
  expect(p->lfos.empty() && p->lfoOps.empty() && p->lfoTargets.empty() &&
             std::none_of(p->links.begin(), p->links.end(), [](const ProgLink& l) { return l.late; }),
         "a project without LFOs has no LFO pass");
}


// --------------------------------------------------------------- 5. chaos and random (0.7.8)

// A Chaos or Random source on the board, linked to the track's pan (a source that moves nothing is not
// run); its id.
ObjectId addSource(Document& d, const Rig& r, SongLfo l) {
  const ObjectId id = d.addLfo(l);
  lfo::link(d, id, mixerParam(r.track, MixerParam::Pan), 0.2f);
  return id;
}

void testChaosRandomFile() {
  Rig r;
  Document d(r.s);
  const ObjectId lfoId = d.addLfo(SongLfo{});
  SongLfo c = lfo::makeDefault(d.song(), LfoKind::Chaos);
  c.character = 0.8f;
  c.seed = 1234;
  c.restart = LfoRestart::EachBar;
  c.unipolar = true;
  const ObjectId cid = addSource(d, r, c);
  SongLfo q = lfo::makeDefault(d.song(), LfoKind::Random);
  q.trigger = RandomTrigger::Roll;
  q.centred = true;
  q.quantise = 5;
  q.roll = 3;
  const ObjectId qid = addSource(d, r, q);
  const SongLfo* cs = d.song().lfo(cid);
  expect(cs && cs->kind == LfoKind::Chaos && cs->restart == LfoRestart::EachBar && cs->character == 0.8f, "a chaos source keeps its settings");
  expect(lfo::number(d.song(), lfoId) == "LFO 1" && lfo::number(d.song(), cid) == "Chaos 1" && lfo::number(d.song(), qid) == "Random 1",
         "sources are numbered by kind: LFO 1, Chaos 1, Random 1");
  const Json j = songToJson(d.song());
  Song back;
  expect(songFromJson(j, back), "a song with chaos and random sources loads");
  expect(back.lfos == d.song().lfos && back.modLinks == d.song().modLinks, "chaos and random fields and links round trip");
  const Json* lj = j.find("lfos");
  expect(lj && lj->array[0].object.size() == 1, "an LFO writes none of the new keys");
  // Character is a Chaos source's parameter alone.
  expect(describeParam(d.song(), lfo::lfoParam(cid, lfo::LfoParam::Character)).has_value() &&
             !describeParam(d.song(), lfo::lfoParam(lfoId, lfo::LfoParam::Character)).has_value(),
         "Character is a chaos source's parameter, not an LFO's");
  const auto all = listParams(d.song());
  expect(std::find(all.begin(), all.end(), lfo::lfoParam(cid, lfo::LfoParam::Character)) != all.end() &&
             std::find(all.begin(), all.end(), lfo::lfoParam(lfoId, lfo::LfoParam::Character)) == all.end(),
         "the automation lists offer it on chaos sources only");
  // A kind this version does not know: left out with its links, never played as an LFO.
  Json future = j;
  future.object["lfos"].array[1].object["kind"] = Json::makeNumber(9);
  Song fb;
  expect(songFromJson(future, fb) && fb.lfos.size() == 2 && !fb.lfo(cid) &&
             std::none_of(fb.modLinks.begin(), fb.modLinks.end(), [cid](const ModLink& m) { return m.isLfo() && m.sourceOwner == cid; }),
         "an unknown source kind is left out with its links");
  // A bar restart is Chaos's alone; the document puts an LFO's back to Play.
  d.editLfo(lfoId, "bar", [](SongLfo& x) { x.restart = LfoRestart::EachBar; });
  expect(d.song().lfo(lfoId)->restart == LfoRestart::OnPlay, "an LFO cannot restart every bar");
  // Roll: one undo step, a new value.
  expect(lfo::roll(d, qid) && d.song().lfo(qid)->roll == 4 && d.history().undoLabel() == "roll a random value", "Roll is one undo step");
  d.undo();
  expect(d.song().lfo(qid)->roll == 3, "and undoes");
  expect(!lfo::roll(d, cid), "Roll is Random's alone");
  // New for a knob: one step, the source and its link together.
  const size_t before = d.song().lfos.size();
  const ObjectId nk = lfo::newForKnob(d, channelParam(r.lead, ChannelParam::Volume), nullptr, LfoKind::Random);
  expect(nk != kNoId && d.song().lfo(nk)->kind == LfoKind::Random && findLfoLink(d.song(), nk, r.lead, static_cast<uint32_t>(ChannelParam::Volume)),
         "New random for a knob makes the source and links it");
  d.undo();
  expect(d.song().lfos.size() == before && !d.song().lfo(nk), "... in one undo step");
}

double movement(const std::vector<float>& v) {
  double m = 0;
  for (size_t k = 1; k < v.size(); ++k) m += std::fabs(v[k] - v[k - 1]);
  return m;
}

void testChaosEngine() {
  // The same seed from the same place plays the same values, every time and at every block size.
  for (LfoRestart rs : {LfoRestart::OnPlay, LfoRestart::Free, LfoRestart::EachBar}) {
    Rig r;
    Document d(r.s);
    SongLfo l = lfo::makeDefault(d.song(), LfoKind::Chaos);
    l.restart = rs;
    l.rateNote = 5;
    const ObjectId id = addSource(d, r, l);
    const auto a = playLfo(d.song(), id, 64, 1024);
    const auto b = playLfo(d.song(), id, 1024, 64);
    const auto c = playLfo(d.song(), id, 64, 1024);
    bool same = a == c;
    for (int k = 0; k < 64; ++k) same = same && a[static_cast<size_t>(k)] == b[static_cast<size_t>(k * 16 + 15)];
    expect(same, rs == LfoRestart::OnPlay ? "chaos: the same values on every play and block size (Play)"
                 : rs == LfoRestart::Free ? "chaos: the same values on every play and block size (Free)"
                                          : "chaos: the same values on every play and block size (Bar)");
  }
  Rig r;
  Document d(r.s);
  SongLfo l = lfo::makeDefault(d.song(), LfoKind::Chaos);
  l.smoothMs = 0;
  l.rateNote = 7;  // 1/16: many bends in a short run
  l.character = 0.2f;
  const ObjectId id = addSource(d, r, l);
  const auto calm = playLfo(d.song(), id, 1500);
  d.editLfo(id, "seed", [](SongLfo& x) { x.seed += 77; });
  const auto other = playLfo(d.song(), id, 1500);
  expect(calm != other, "chaos: another seed, another movement");
  d.editLfo(id, "wild", [](SongLfo& x) { x.character = 1.0f, x.seed -= 77; });
  const auto wild = playLfo(d.song(), id, 1500);
  std::printf("# chaos movement calm %.1f wild %.1f\n", movement(calm), movement(wild));
  expect(movement(wild) > movement(calm) * 1.3, "chaos: a wild character moves more than a calm one");
  float lo = 9, hi = -9;
  for (float v : wild) lo = std::min(lo, v), hi = std::max(hi, v);
  expect(lo >= -1.0f && hi <= 1.0f && lo < -0.3f && hi > 0.3f, "chaos: bipolar, inside -1..1 and both ways");
  d.editLfo(id, "up", [](SongLfo& x) { x.unipolar = true; });
  const auto up = playLfo(d.song(), id, 1500);
  lo = 9, hi = -9;
  for (float v : up) lo = std::min(lo, v), hi = std::max(hi, v);
  expect(lo >= 0.0f && hi <= 1.0f && hi > 0.5f, "chaos: up only stays in 0..1");
  // No corners: a smooth curve (one bend a cycle) never jumps between control blocks.
  float jump = 0;
  for (size_t k = 1; k < calm.size(); ++k) jump = std::max(jump, std::fabs(calm[k] - calm[k - 1]));
  expect(jump < 0.08f, "chaos: calm moves smoothly (no jumps between blocks)");
  // Bar: each bar moves the same way (a bar at 120 BPM is 1500 blocks of 64).
  d.editLfo(id, "bar", [](SongLfo& x) { x.restart = LfoRestart::EachBar, x.unipolar = false; });
  const auto bars = playLfo(d.song(), id, 3100);
  bool repeat = true;
  double worst = 0;
  for (size_t k = 20; k < 1480; ++k) worst = std::max(worst, std::fabs(static_cast<double>(bars[k]) - bars[k + 1500]));
  repeat = worst < 1e-5;  // (the song position in ticks carries rounding from bar to bar)
  if (!repeat) std::printf("# bar repeat worst %g\n", worst);
  expect(repeat, "chaos: Bar starts the walk again at every bar");
  d.editLfo(id, "play", [](SongLfo& x) { x.restart = LfoRestart::OnPlay; });
  const auto play = playLfo(d.song(), id, 3100);
  bool differs = false;
  for (size_t k = 20; k < 1480; ++k) differs = differs || std::fabs(play[k] - play[k + 1500]) > 1e-3f;
  expect(differs, "chaos: Play walks on through the bars");
  // Note: a note of its channel starts it again.
  d.editLfo(id, "note", [&r](SongLfo& x) { x.restart = LfoRestart::EachNote, x.noteChannel = r.lead; });
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setProgram(pb.build(d.song()));
  std::vector<float> L(64), R(64);
  for (int k = 0; k < 200; ++k) eng.process(L.data(), R.data(), 64);
  const auto rt = pb.lfoRuntime(id);
  expect(rt && rt->xUi.load() > 1.0, "chaos: it walks on between notes");
  eng.noteOn(r.lead, 60, 0.8f);
  for (int k = 0; k < 2; ++k) eng.process(L.data(), R.data(), 64);
  expect(rt->xUi.load() < 0.1, "chaos: Note starts the walk again");
}

void testRandomEngine() {
  Rig r;
  Document d(r.s);
  SongLfo l = lfo::makeDefault(d.song(), LfoKind::Random);
  l.rateNote = 5;  // 1/4: a new value every 750 blocks of 64 at 120 BPM
  const ObjectId id = addSource(d, r, l);
  const uint32_t seed = d.song().lfo(id)->seed;
  const auto a = playLfo(d.song(), id, 3000);
  const auto b = playLfo(d.song(), id, 3000);
  expect(a == b, "random: the same values on every play");
  // Held between steps, a new value at each step, the value the pure function gives.
  bool held = true, values = true;
  int changes = 0;
  for (size_t k = 1; k < a.size(); ++k) {
    // block k covers song samples [k*64, (k+1)*64): its value is the step of its start
    const uint64_t step = static_cast<uint64_t>(k * 64 / 24000);
    values = values && a[k] == lfo::randomSample(seed, step, false, 0);
    if (a[k] != a[k - 1]) ++changes, held = held && (k * 64) % 24000 == 0;
  }
  expect(values, "random: each step's value is randomSample(seed, step)");
  expect(held && changes >= 3, "random: held between steps, a new value at each step");
  // Centred values gather round the middle; quantised values sit on the levels.
  double spreadEven = 0, spreadMid = 0;
  for (uint64_t k = 0; k < 4000; ++k) {
    spreadEven += std::fabs(lfo::randomSample(seed, k, false, 0));
    spreadMid += std::fabs(lfo::randomSample(seed, k, true, 0));
  }
  expect(spreadMid < spreadEven * 0.75, "random: Middle gathers values round the middle");
  bool onLevels = true;
  for (uint64_t k = 0; k < 2000; ++k) {
    const float v = lfo::randomSample(seed, k, false, 5);
    const float lvl = (v + 1.0f) * 0.5f * 4.0f;
    onLevels = onLevels && std::fabs(lvl - std::round(lvl)) < 1e-5f;
  }
  expect(onLevels, "random: 5 steps puts every value on one of 5 levels");
  d.editLfo(id, "q", [](SongLfo& x) { x.quantise = 3, x.unipolar = true; });
  const auto q = playLfo(d.song(), id, 3000);
  bool three = true;
  for (float v : q) three = three && (v == 0.0f || v == 0.5f || v == 1.0f);
  expect(three, "random: 3 steps, up only: the knob gets 0, half or all of the swing");
  // Glide: smoothing turns the steps into slopes.
  d.editLfo(id, "glide", [](SongLfo& x) { x.quantise = 0, x.unipolar = false, x.smoothMs = 200; });
  const auto g = playLfo(d.song(), id, 3000);
  float maxJump = 0;
  for (size_t k = 1; k < g.size(); ++k) maxJump = std::max(maxJump, std::fabs(g[k] - g[k - 1]));
  expect(maxJump < 0.05f, "random: glide slides from value to value");
  // Note: every note a new value, in order.
  d.editLfo(id, "note", [](SongLfo& x) { x.trigger = RandomTrigger::Note, x.noteChannel = kNoId, x.smoothMs = 0; });
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(d.song()));
    std::vector<float> L(64), R(64);
    for (int k = 0; k < 4; ++k) eng.process(L.data(), R.data(), 64);
    const auto rt = pb.lfoRuntime(id);
    bool ok = rt && rt->display.load() == lfo::randomSample(seed, 0, false, 0);
    for (uint64_t n = 1; n <= 3; ++n) {
      eng.noteOn(n == 2 ? r.pad : r.lead, 60, 0.8f);  // (every channel's notes)
      for (int k = 0; k < 3; ++k) eng.process(L.data(), R.data(), 64);
      ok = ok && rt->display.load() == lfo::randomSample(seed, n, false, 0);
    }
    expect(ok, "random: Note draws the next value on each note of any channel");
  }
  // Roll: the value of the rolls so far; a roll is a song edit.
  d.editLfo(id, "roll", [](SongLfo& x) { x.trigger = RandomTrigger::Roll; });
  {
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(d.song()));
    std::vector<float> L(64), R(64);
    for (int k = 0; k < 4; ++k) eng.process(L.data(), R.data(), 64);
    const auto rt = pb.lfoRuntime(id);
    const float v0 = rt->display.load();
    lfo::roll(d, id);
    eng.setProgram(pb.build(d.song()));
    for (int k = 0; k < 4; ++k) eng.process(L.data(), R.data(), 64);
    expect(v0 == lfo::randomSample(seed, 0, false, 0) && rt->display.load() == lfo::randomSample(seed, 1, false, 0),
           "random: Roll draws the next value");
  }
}

void testSourcesRest() {
  // A Chaos or Random source that moves nothing is not run: the song rests as one without it.
  Rig r;
  Document d(r.s);
  const ObjectId c = d.addLfo(lfo::makeDefault(d.song(), LfoKind::Chaos));
  d.addLfo(lfo::makeDefault(d.song(), LfoKind::Random));
  {
    ProgramBuilder pb;
    const auto p = pb.build(d.song());
    expect(p->lfos.size() == 2 && p->lfoOps.empty() && !p->controlGrid(), "unassigned chaos and random sources are not run");
  }
  lfo::link(d, c, mixerParam(r.track, MixerParam::Pan));
  {
    ProgramBuilder pb;
    const auto p = pb.build(d.song());
    expect(p->lfoOps.size() == 2 && p->controlGrid(), "an assigned one runs (its evaluation and its target)");
  }
  // An LFO with no targets still runs, as before.
  Document e(r.s);
  e.addLfo(SongLfo{});
  ProgramBuilder pb;
  expect(pb.build(e.song())->lfoOps.size() == 1, "an LFO with no targets runs as before");
  // No allocation while chaos and random sources run.
  Document f(r.s);
  for (int k = 0; k < 6; ++k) {
    SongLfo l = lfo::makeDefault(f.song(), k % 2 ? LfoKind::Random : LfoKind::Chaos);
    l.trigger = static_cast<RandomTrigger>(k % 4);
    l.restart = static_cast<LfoRestart>(k % 4);
    l.smoothMs = 10;
    l.quantise = k;
    const ObjectId id = f.addLfo(l);
    lfo::link(f, id, mixerParam(r.track, MixerParam::Pan), 0.1f);
    lfo::link(f, id, channelPluginParam(r.lead, pp::SynOsc1Level), 0.2f);
  }
  ProgramBuilder fb;
  AudioEngine eng(kSr);
  eng.setMode(PlayMode::Song);
  eng.setProgram(fb.build(f.song()));
  eng.play();
  std::vector<float> L(256), R(256);
  for (int k = 0; k < 8; ++k) eng.process(L.data(), R.data(), 256);
  gAllocs = 0;
  gCounting = true;
  for (int k = 0; k < 64; ++k) {
    if (k == 20) eng.noteOn(r.lead, 60, 0.8f);
    eng.process(L.data(), R.data(), 256);
  }
  gCounting = false;
  expect(gAllocs.load() == 0, "no allocation while chaos and random sources run");
  eng.stop();
}

// An export of a song (a synth playing for two bars) with a chaos and a random source on its knobs.
AudioBuffer renderWithSources(uint32_t chaosSeed) {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId lead = d.addChannel("lead", "gl.synth");
  d.routeChannel(lead, d.song().mixer[1].id);
  d.addNote(pat, lead, {0, 300, 60, 110});
  d.addNote(pat, lead, {192, 150, 67, 110});
  d.addClip(pat, 0, 0);
  d.addClip(pat, 0, 384);
  SongLfo c = lfo::makeDefault(d.song(), LfoKind::Chaos);
  c.seed = chaosSeed;
  c.character = 0.9f;
  c.rateNote = 6;
  const ObjectId cid = d.addLfo(c);
  lfo::link(d, cid, channelParam(lead, ChannelParam::Pan), 0.8f);
  lfo::link(d, cid, channelPluginParam(lead, pp::SynOsc1Level), 0.4f);
  SongLfo q = lfo::makeDefault(d.song(), LfoKind::Random);
  q.rateNote = 7;
  q.smoothMs = 15;
  const ObjectId qid = d.addLfo(q);
  lfo::link(d, qid, channelParam(lead, ChannelParam::Volume), -0.5f);
  RenderOptions o;
  o.sampleRate = 48000;
  o.blockSize = 512;
  o.tail = 96;
  return renderSong(d.song(), o);
}

void testExportIdentical() {
  const AudioBuffer a = renderWithSources(99), b = renderWithSources(99), c = renderWithSources(100);
  const bool same = a.frames > 0 && a.frames == b.frames && a.data == b.data;
  expect(same, "an export with chaos and random sources is bit-identical from run to run");
  expect(a.frames == c.frames && a.data != c.data, "and another chaos seed exports another sound");
}
}  // namespace

int main() {
  testModelAndFile();
  testDocument();
  testShapes();
  testOnPlay();
  testFreeAndNotes();
  testBlockSize();
  testTargets();
  testMacroAndLfoChains();
  testNoAllocation();
  testOldProjectsUnchanged();
  testChaosRandomFile();
  testChaosEngine();
  testRandomEngine();
  testSourcesRest();
  testExportIdentical();
  return finish();
}
