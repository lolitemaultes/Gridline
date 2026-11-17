// Gridline - K-106 cp17 (K-316 plan 7.3): the Tracker Sampler's editing session's pure edits
// (tracker_session.h). Each op that both front ends now share (the plugin editor and the Tracker's
// screens) is checked three ways on fixture instruments:
//   * against the plugin editor's inline edit as it was before the move (the reference lambdas
//     below are that code, copied from tracker_ui.cpp at k106-tracker-cp16), byte for byte on the
//     written instrument text;
//   * against a golden hash of that text (so a later change to either side shows here);
//   * labels and merge keys equal the ones the editor always used (undo text, drag folding).
// Plus the invariants: loop inside the played part, envelope sustain / loop indices follow
// inserted and deleted points, the key map follows swapped samples.
// Run with GOLDEN_PRINT=1 to print the hashes (when a change is meant).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/TrackerData.h"
#include "../../src/packs/tracker/tracker.h"
#include "../../src/packs/tracker/tracker_session.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

// Two samples (one looped), a volume envelope of three points with sustain and a loop.
tp::Edit fixture() {
  tp::Edit e = tp::readEdit("", 0);
  tp::addSample(e, "/fixture/kick.wav", 9000, 44100, "kick");
  tp::addSample(e, "/fixture/snare.wav", 4000, 22050, "snare");
  trk::SampleDef& s = e.text.def.samples[0];
  s.loop = trk::kLoopForward;
  s.loopStart = 2000;
  s.loopLength = 3000;
  e.cuts.start[0] = 100;
  e.cuts.end[0] = 8400;
  trk::Envelope& v = e.text.def.vol;
  v = trk::Envelope{};
  tp::insertEnvPoint(v, 0, 64);
  tp::insertEnvPoint(v, 10, 40);
  tp::insertEnvPoint(v, 30, 0);
  v.flags = trk::kEnvOn | trk::kEnvSustain | trk::kEnvLoop;
  v.sustain = 1;
  v.loopStart = 1;
  v.loopEnd = 2;
  e.text.name = "Fixture";
  return e;
}

struct Case {
  const char* name;
  std::function<void(tp::Edit&)> now;  // the session's edit
  std::function<void(tp::Edit&)> was;  // the plugin editor's inline edit before cp17
  uint64_t golden;
};

}  // namespace

int main() {
  const bool print = std::getenv("GOLDEN_PRINT") != nullptr;
  const int32_t frames = 9000;
  const tp::Edit base = fixture();
  auto s0s1 = [&](const tp::Edit& e, int k, int32_t* s0, int32_t* s1) {
    const int32_t cutA = e.cuts.start[static_cast<size_t>(k)], cutB = e.cuts.end[static_cast<size_t>(k)];
    const int32_t fr = k == 0 ? frames : 4000;  // (the snare fixture is 4000 frames)
    *s0 = std::clamp(cutA, 0, fr - 1);
    *s1 = cutB > 0 ? std::clamp(cutB, *s0 + 1, fr) : fr;
  };
  // The inline marker drag, as tracker_ui.cpp waveformPanel had it.
  auto markerWas = [&](tp::Edit& e, int k, int marker, int32_t f) {
    trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
    int32_t& cutA = e.cuts.start[static_cast<size_t>(k)];
    int32_t& cutB = e.cuts.end[static_cast<size_t>(k)];
    const int32_t s0 = std::clamp(cutA, 0, frames - 1);
    const int32_t s1 = cutB > 0 ? std::clamp(cutB, s0 + 1, frames) : frames;
    const int32_t ls = sd.loopStart, le = sd.loopStart + sd.loopLength;
    switch (marker) {
      case 0: cutA = std::clamp(f, 0, s1 - 1); break;
      case 1: {
        const int32_t v = std::clamp(f, s0 + 1, frames);
        cutB = v >= frames ? 0 : v;
        break;
      }
      case 2: {
        const int32_t v = std::clamp(f, s0, le - 1);
        sd.loopStart = v;
        sd.loopLength = le - v;
        break;
      }
      default: sd.loopLength = std::clamp(f, ls + 1, s1) - ls; break;
    }
  };
  // The inline envelope point drag.
  auto moveWas = [](tp::Edit& e, bool pan, int i, int x, int y) {
    trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
    const int lo = i == 0 ? 0 : env.x[static_cast<size_t>(i - 1)] + 1;
    const int hi = i == 0 ? 0 : i + 1 < env.count ? env.x[static_cast<size_t>(i + 1)] - 1 : 32767;
    const int nx = std::clamp(x, lo, std::max(lo, hi));
    if (nx != env.x[static_cast<size_t>(i)] || y != env.y[static_cast<size_t>(i)]) {
      env.x[static_cast<size_t>(i)] = static_cast<int16_t>(nx);
      env.y[static_cast<size_t>(i)] = static_cast<int16_t>(y);
    }
  };
  auto addWas = [](tp::Edit& e, bool pan, int x, int y) {
    trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
    const bool empty = env.count == 0;
    if (empty) tp::insertEnvPoint(env, 0, y);
    const int i = tp::insertEnvPoint(env, std::max(1, x), y);
    if (i >= 0 && empty) env.flags |= trk::kEnvOn;
  };
  auto flagWas = [](tp::Edit& e, bool pan, uint8_t flag) {
    trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
    const bool on = (env.flags & flag) != 0;
    env.flags = static_cast<uint8_t>(on ? env.flags & ~flag : env.flags | flag);
    if (env.count == 0 && !on) {
      tp::insertEnvPoint(env, 0, pan ? 32 : 64);
      tp::insertEnvPoint(env, 16, pan ? 32 : 64);
    }
  };

  std::vector<Case> cases = {
      {"volume 50", [](tp::Edit& e) { tp::applySampleField(e, 0, tp::SampleField::Volume, 50); },
       [](tp::Edit& e) { e.text.def.samples[0].volume = static_cast<uint8_t>(std::clamp(std::lround(50.0), 0L, 64L)); }, 0},
      {"volume below the bottom", [](tp::Edit& e) { tp::applySampleField(e, 0, tp::SampleField::Volume, -5); },
       [](tp::Edit& e) { e.text.def.samples[0].volume = static_cast<uint8_t>(std::clamp(std::lround(-5.0), 0L, 64L)); }, 0},
      {"pan left 40", [](tp::Edit& e) { tp::applySampleField(e, 1, tp::SampleField::Pan, -40); },
       [](tp::Edit& e) { e.text.def.samples[1].pan = static_cast<uint8_t>(std::clamp(std::lround(-40.0) + 128, 0L, 255L)); }, 0},
      {"fine -200", [](tp::Edit& e) { tp::applySampleField(e, 0, tp::SampleField::Fine, -200); },
       [](tp::Edit& e) { e.text.def.samples[0].finetune = static_cast<int8_t>(std::clamp(std::lround(-200.0), -128L, 127L)); }, 0},
      {"transpose +12", [](tp::Edit& e) { tp::applySampleField(e, 1, tp::SampleField::RelNote, 12); },
       [](tp::Edit& e) { e.text.def.samples[1].relNote = static_cast<int8_t>(std::clamp(std::lround(12.0), -48L, 71L)); }, 0},
      {"loop off", [&](tp::Edit& e) { int32_t a, b; s0s1(e, 0, &a, &b); tp::applyLoopType(e, 0, trk::kLoopOff, a, b); },
       [](tp::Edit& e) { e.text.def.samples[0].loop = trk::kLoopOff; }, 0},
      {"loop on an unlooped sample takes the played part",
       [&](tp::Edit& e) { int32_t a, b; s0s1(e, 1, &a, &b); tp::applyLoopType(e, 1, trk::kLoopPingPong, a, b); },
       [&](tp::Edit& e) {
         int32_t s0, s1;
         s0s1(e, 1, &s0, &s1);
         trk::SampleDef& sd = e.text.def.samples[1];
         sd.loop = trk::kLoopPingPong;
         if (sd.loopLength < 1) sd.loopStart = s0, sd.loopLength = s1 - s0;
       },
       0},
      {"8-bit", [](tp::Edit& e) { e.eightBit[1] = true; }, [](tp::Edit& e) { e.eightBit[1] = true; }, 0},
      {"start marker", [](tp::Edit& e) { tp::applyMarker(e, 0, 0, 700, 9000); }, [&](tp::Edit& e) { markerWas(e, 0, 0, 700); }, 0},
      {"start marker past the end", [](tp::Edit& e) { tp::applyMarker(e, 0, 0, 9999, 9000); }, [&](tp::Edit& e) { markerWas(e, 0, 0, 9999); }, 0},
      {"end marker to the end", [](tp::Edit& e) { tp::applyMarker(e, 0, 1, 9000, 9000); }, [&](tp::Edit& e) { markerWas(e, 0, 1, 9000); }, 0},
      {"loop start marker", [](tp::Edit& e) { tp::applyMarker(e, 0, 2, 2500, 9000); }, [&](tp::Edit& e) { markerWas(e, 0, 2, 2500); }, 0},
      {"loop end marker past the cut", [](tp::Edit& e) { tp::applyMarker(e, 0, 3, 8900, 9000); }, [&](tp::Edit& e) { markerWas(e, 0, 3, 8900); }, 0},
      {"rename sample", [](tp::Edit& e) { e.text.sampleNames[1] = "rim"; }, [](tp::Edit& e) { e.text.sampleNames[1] = "rim"; }, 0},
      {"swap samples (the key map follows)", [](tp::Edit& e) { tp::swapSamples(e, 0, 1); }, [](tp::Edit& e) { tp::swapSamples(e, 0, 1); }, 0},
      {"envelope flag off", [](tp::Edit& e) { tp::applyEnvFlag(e, false, trk::kEnvLoop); }, [&](tp::Edit& e) { flagWas(e, false, trk::kEnvLoop); }, 0},
      {"panning envelope on from nothing", [](tp::Edit& e) { tp::applyEnvFlag(e, true, trk::kEnvOn); }, [&](tp::Edit& e) { flagWas(e, true, trk::kEnvOn); }, 0},
      {"point added between (indices follow)", [](tp::Edit& e) { tp::applyEnvAdd(e, false, 5, 50); }, [&](tp::Edit& e) { addWas(e, false, 5, 50); }, 0},
      {"point added to an empty envelope", [](tp::Edit& e) { tp::applyEnvAdd(e, true, 12, 20); }, [&](tp::Edit& e) { addWas(e, true, 12, 20); }, 0},
      {"point moved past its neighbour", [](tp::Edit& e) { tp::applyEnvMove(e, false, 1, 99, 33); }, [&](tp::Edit& e) { moveWas(e, false, 1, 99, 33); }, 0},
      {"first point stays at 0", [](tp::Edit& e) { tp::applyEnvMove(e, false, 0, 7, 60); }, [&](tp::Edit& e) { moveWas(e, false, 0, 7, 60); }, 0},
      {"point deleted (indices follow)", [](tp::Edit& e) { tp::applyEnvDelete(e, false, 1); }, [](tp::Edit& e) { tp::deleteEnvPoint(e.text.def.vol, 1); }, 0},
      {"sustain point", [](tp::Edit& e) { tp::applyEnvSustain(e, false, 2); },
       [](tp::Edit& e) { e.text.def.vol.sustain = 2, e.text.def.vol.flags |= trk::kEnvSustain; }, 0},
      {"loop start after its end", [](tp::Edit& e) { tp::applyEnvLoopStart(e, false, 2); },
       [](tp::Edit& e) {
         trk::Envelope& env = e.text.def.vol;
         env.loopStart = 2;
         if (env.loopEnd < 2) env.loopEnd = 2;
         env.flags |= trk::kEnvLoop;
       },
       0},
      {"loop end before its start", [](tp::Edit& e) { tp::applyEnvLoopEnd(e, false, 0); },
       [](tp::Edit& e) {
         trk::Envelope& env = e.text.def.vol;
         env.loopEnd = 0;
         if (env.loopStart > 0) env.loopStart = 0;
         env.flags |= trk::kEnvLoop;
       },
       0},
      {"fadeout", [](tp::Edit& e) { tp::applyFadeout(e, 5000); }, [](tp::Edit& e) { e.text.def.fadeout = static_cast<uint16_t>(std::clamp(5000L, 0L, 4095L)); }, 0},
      {"vibrato depth", [](tp::Edit& e) { tp::applyVibrato(e, tp::VibField::Depth, 9); }, [](tp::Edit& e) { e.text.def.vibDepth = 9; }, 0},
      {"vibrato rate past the top", [](tp::Edit& e) { tp::applyVibrato(e, tp::VibField::Rate, 99); }, [](tp::Edit& e) { e.text.def.vibRate = 63; }, 0},
      {"key map", [](tp::Edit& e) { tp::mapKeys(e, 40, 60, 1); }, [](tp::Edit& e) { tp::mapKeys(e, 40, 60, 1); }, 0},
      {"MIDI mute (the box)", [](tp::Edit& e) {
         tp::MidiSettings m;
         m.on = true, m.mute = true, m.channel = 3;
         tp::applyMidi(e, m, true);
       },
       [](tp::Edit& e) {
         tp::MidiSettings m;
         m.on = true, m.mute = true, m.channel = 3;
         e.midi = m;
         e.text.def.mute = m.on && m.mute;
       },
       0},
      {"MIDI channel (the editor's knob)", [](tp::Edit& e) {
         tp::MidiSettings m = e.midi;
         m.channel = 9;
         tp::applyMidi(e, m, false);
       },
       [](tp::Edit& e) { e.midi.channel = 9; }, 0},
  };
  // Golden hashes of the written text (captured from the cp16 inline code, through the reference lambdas).
  static const uint64_t golden[] = {
#include "tracker_ops_golden.inc"
  };
  const size_t nGolden = sizeof golden / sizeof golden[0];
  for (size_t i = 0; i < cases.size(); ++i) {
    tp::Edit a = base, b = base;
    cases[i].now(a);
    cases[i].was(b);
    const std::string ta = tp::writeEdit(a), tb = tp::writeEdit(b);
    expect(ta == tb, std::string("op equals the editor's old inline edit: ") + cases[i].name);
    expect(ta != tp::writeEdit(base) || std::string(cases[i].name).find("first point") != std::string::npos,
           std::string("op changes the instrument: ") + cases[i].name);
    if (print) std::printf("  0x%016llxull,  // %s\n", static_cast<unsigned long long>(fnv(tb)), cases[i].name);
    if (!print) expect(i < nGolden && golden[i] == fnv(ta), std::string("golden: ") + cases[i].name);
    // The text reads back to the same text (nothing the op wrote is lost on a save and load).
    expect(tp::writeEdit(tp::readEdit(ta, 0)) == ta, std::string("reads back: ") + cases[i].name);
  }

  // Labels and merge keys: the undo menu's words and the drag folding are the editor's.
  expect(std::string(tp::sampleFieldLabel(tp::SampleField::Volume)) == "sample volume" && tp::sampleFieldMerge(tp::SampleField::Volume) == 1, "volume label, merge 1");
  expect(std::string(tp::sampleFieldLabel(tp::SampleField::Pan)) == "sample pan" && tp::sampleFieldMerge(tp::SampleField::Pan) == 2, "pan label, merge 2");
  expect(std::string(tp::sampleFieldLabel(tp::SampleField::Fine)) == "sample fine tune" && tp::sampleFieldMerge(tp::SampleField::Fine) == 3, "fine label, merge 3");
  expect(std::string(tp::sampleFieldLabel(tp::SampleField::RelNote)) == "sample transpose" && tp::sampleFieldMerge(tp::SampleField::RelNote) == 4, "transpose, merge 4");
  expect(std::string(tp::markerLabel(0)) == "sample start" && std::string(tp::markerLabel(3)) == "loop end", "marker labels");
  expect(std::string(tp::vibLabel(tp::VibField::Sweep)) == "vibrato sweep" && tp::vibMerge(tp::VibField::Sweep) == 31 && tp::vibMerge(tp::VibField::Rate) == 33,
         "vibrato labels, merge 31..33");
  expect(std::string(tp::envLabel(true)) == "panning envelope" && std::string(tp::envLabel(false)) == "volume envelope", "envelope labels");
  const uint64_t key = 0x1234567ull;
  expect(tp::mergeKeyFor(key, 5) == ((key * 2654435761ull) ^ (0x7A1C0000ull + 5ull)), "merge key formula unchanged");
  expect(tp::mergeKeyFor(key, 20) != tp::mergeKeyFor(key, 21), "the two envelopes merge apart");

  // Invariants.
  {
    tp::Edit e = base;
    tp::applyMarker(e, 0, 2, 0, frames);  // the loop start dragged before the cut start stays at the cut
    expect(e.text.def.samples[0].loopStart >= e.cuts.start[0], "loop start stays inside the played part");
    tp::applyMarker(e, 0, 3, 99999, frames);
    expect(e.text.def.samples[0].loopStart + e.text.def.samples[0].loopLength <= e.cuts.end[0], "loop end stays inside the played part");
  }
  {
    tp::Edit e = base;
    tp::applyEnvAdd(e, false, 5, 50);  // before the sustain point: its index moves on with it
    expect(e.text.def.vol.sustain == 2 && e.text.def.vol.loopStart == 2 && e.text.def.vol.loopEnd == 3, "sustain and loop follow an inserted point");
    expect(tp::applyEnvDelete(e, false, 1) && e.text.def.vol.sustain == 1, "and a deleted one");
    expect(!tp::applyEnvDelete(e, false, 0), "the first point is never deleted");
    tp::Edit two = base;
    tp::applyEnvDelete(two, false, 2);
    expect(!tp::applyEnvDelete(two, false, 1), "never below two points");
  }
  {
    tp::Edit e = base;
    const int before0 = e.text.def.keymap[50];
    tp::swapSamples(e, 0, 1);
    expect(e.text.def.keymap[50] == (before0 == 0 ? 1 : before0 == 1 ? 0 : before0), "the key map follows a swap");
  }
  expect(tp::hasSample(base, 0) && tp::hasSample(base, 1) && !tp::hasSample(base, 2) && tp::sampleCountOf(base) == 2, "samples present");
  expect(tp::editName(base, 1) == "snare", "the edited file is named after the sample");

  // K-106 cp19: the envelope edits the Tracker's Instrument editor makes (the same pure edits as
  // the plugin editor's), and the key map.
  {
    tp::Edit e = tp::readEdit("", 0);
    e.text.def.pan = trk::Envelope{};
    const int i = tp::applyEnvAdd(e, true, 20, 10);
    const trk::Envelope& p = e.text.def.pan;
    expect(i == 1 && p.count == 2 && p.x[0] == 0 && p.x[1] == 20 && p.y[1] == 10 && p.on(), "env: a point on an empty envelope also sets its first point and turns it on");
    tp::Edit f = e;
    for (int k = 0; k < 20; ++k) {
      tp::applyEnvAdd(f, true, 30 + k * 4, 20);
    }
    expect(f.text.def.pan.count == trk::kEnvPoints, "env: twelve points at most");
    expect(tp::applyEnvAdd(f, true, 500, 20) == -1, "env: a thirteenth is refused");
    expect(tp::applyEnvAdd(e, true, 20, 40) == -1, "env: never two points on one tick");
  }
  {
    tp::Edit e = fixture();  // points 0:64, 10:40, 30:0
    expect(!tp::applyEnvMove(e, false, 0, 15, 50) || e.text.def.vol.x[0] == 0, "env: the first point stays at tick 0");
    expect(tp::applyEnvMove(e, false, 1, 99, 70) && e.text.def.vol.x[1] == 29 && e.text.def.vol.y[1] == 64, "env: a point stays before the next one, the level at most 64");
    expect(tp::applyEnvMove(e, false, 1, -5, -3) && e.text.def.vol.x[1] == 1 && e.text.def.vol.y[1] == 0, "env: and after the one before, at least 0");
    expect(!tp::applyEnvMove(e, false, 1, 1, 0), "env: no move, no change");
    expect(tp::applyEnvMove(e, false, 2, 5000, 10) && e.text.def.vol.x[2] == 5000, "env: the last point moves on freely");
  }
  {
    tp::Edit e = fixture();
    trk::Envelope& v = e.text.def.vol;
    tp::applyEnvFlag(e, false, trk::kEnvSustain);
    expect(!(v.flags & trk::kEnvSustain), "env: the sustain flag toggles off");
    tp::applyEnvSustain(e, false, 2);
    expect((v.flags & trk::kEnvSustain) && v.sustain == 2, "env: setting the sustain point turns sustain on");
    tp::applyEnvLoopStart(e, false, 2);
    expect(v.loopStart == 2 && v.loopEnd == 2, "env: a loop start past the end takes the end along");
    tp::applyEnvLoopEnd(e, false, 0);
    expect(v.loopStart == 0 && v.loopEnd == 0, "env: a loop end before the start takes the start along");
    tp::applyEnvFlag(e, false, trk::kEnvOn);
    expect(!v.on() && v.count == 3, "env: off keeps the points");
    tp::Edit n = tp::readEdit("", 0);
    n.text.def.vol = trk::Envelope{};
    tp::applyEnvFlag(n, false, trk::kEnvOn);
    expect(n.text.def.vol.on() && n.text.def.vol.count == 2 && n.text.def.vol.y[0] == 64 && n.text.def.vol.y[1] == 64, "env: on with no points: a flat line at full volume");
    n.text.def.pan = trk::Envelope{};
    tp::applyEnvFlag(n, true, trk::kEnvOn);
    expect(n.text.def.pan.count == 2 && n.text.def.pan.y[0] == 32, "env: the panning one in the middle");
    // The text round trip keeps every envelope field.
    tp::Edit r = tp::readEdit(tp::writeEdit(e), 0);
    const trk::Envelope& rv = r.text.def.vol;
    expect(rv.flags == v.flags && rv.count == v.count && rv.sustain == v.sustain && rv.loopStart == v.loopStart && rv.loopEnd == v.loopEnd && rv.x == v.x && rv.y == v.y,
           "env: written and read back the same");
  }
  {
    // Add after the selected point: halfway to the next, past the last, the empty case, no room.
    const tp::Edit e = fixture();
    int x = 0, y = 0;
    expect(tp::envPointAfter(e.text.def.vol, false, 0, &x, &y) && x == 5 && y == 52, "add after: halfway to the next point");
    expect(tp::envPointAfter(e.text.def.vol, false, 2, &x, &y) && x == 46 && y == 0, "add after the last: 16 ticks on at its level");
    trk::Envelope none;
    expect(tp::envPointAfter(none, true, 0, &x, &y) && x == 16 && y == 32, "add on an empty panning envelope: tick 16 in the middle");
    trk::Envelope tight = e.text.def.vol;
    tight.x[1] = 1;
    expect(!tp::envPointAfter(tight, false, 0, &x, &y), "add after: no free tick between the points");
    tp::Edit f = e;
    for (int k = 0; k < 9; ++k) {
      tp::applyEnvAdd(f, false, 40 + k, 10);
    }
    expect(!tp::envPointAfter(f.text.def.vol, false, 0, &x, &y), "add after: a full envelope");
    // The pair the Add button makes: the point lands where envPointAfter says, selection indices follow.
    tp::Edit g = e;
    tp::envPointAfter(g.text.def.vol, false, 0, &x, &y);
    expect(tp::applyEnvAdd(g, false, x, y) == 1 && g.text.def.vol.sustain == 2 && g.text.def.vol.loopEnd == 3, "add after: sustain and loop follow");
  }
  {
    // Presets: a volume slot brings the fadeout and the vibrato, a panning slot only its envelope.
    const tp::EnvSlots f = tp::factoryEnvSlots();
    tp::Edit e = fixture();
    tp::EnvSlot s = f.vol[1];
    s.fadeout = 777, s.vibType = 2, s.vibSweep = 9, s.vibDepth = 5, s.vibRate = 20;
    tp::applyEnvSlot(e.text.def, s, false);
    expect(e.text.def.fadeout == 777 && e.text.def.vibType == 2 && e.text.def.vibDepth == 5 && e.text.def.vol.count == s.env.count, "preset: a volume slot with its fadeout and vibrato");
    const trk::Envelope volBefore = e.text.def.vol;
    tp::applyEnvSlot(e.text.def, f.pan[2], true);
    expect(e.text.def.fadeout == 777 && e.text.def.vol.x == volBefore.x && e.text.def.pan.count == f.pan[2].env.count, "preset: a panning slot leaves the rest");
    const tp::EnvSlot back = tp::takeEnvSlot(e.text.def, false);
    expect(back.fadeout == 777 && back.vibRate == 20 && back.env.x == e.text.def.vol.x, "preset: kept from the instrument as it is");
  }
  {
    // The key map: a drag in either direction, clamped; every key; the keys of a sample to the nearest other.
    tp::Edit e = fixture();
    tp::mapKeys(e, 40, 30, 1);
    bool ok = true;
    for (int n = 0; n < 96; ++n) {
      ok = ok && e.text.def.keymap[static_cast<size_t>(n)] == (n >= 30 && n <= 40 ? 1 : 0);
    }
    expect(ok, "keys: a drag right to left gives the same keys");
    tp::mapKeys(e, -5, 2, 1);
    tp::mapKeys(e, 90, 200, 1);
    expect(e.text.def.keymap[0] == 1 && e.text.def.keymap[95] == 1 && e.text.def.keymap[3] == 0, "keys: clamped to the 96 keys");
    tp::Edit away = e;
    expect(tp::applyKeysAway(away, 1), "keys away: done");
    bool none = true;
    for (int n = 0; n < 96; ++n) {
      none = none && away.text.def.keymap[static_cast<size_t>(n)] != 1;
    }
    expect(none, "keys away: no key plays sample 02 any more");
    tp::Edit lone = tp::readEdit("", 0);
    tp::addSample(lone, "/fixture/one.wav", 1000, 44100, "one");
    expect(!tp::applyKeysAway(lone, 0), "keys away: refused with no other sample");
    tp::Edit three = fixture();
    tp::addSample(three, "/fixture/hat.wav", 2000, 44100, "hat");
    tp::mapKeys(three, 0, 95, 0);
    tp::mapKeys(three, 0, 9, 1);
    tp::mapKeys(three, 80, 95, 2);
    expect(tp::applyKeysAway(three, 0), "keys away: three samples");
    expect(three.text.def.keymap[10] == 1 && three.text.def.keymap[44] == 1 && three.text.def.keymap[46] == 2 && three.text.def.keymap[79] == 2,
           "keys away: each key to its nearest neighbour's sample");
    tp::Edit unused = fixture();
    tp::mapKeys(unused, 0, 95, 0);
    expect(!tp::applyKeysAway(unused, 1), "keys away: nothing to do when no key plays it");
    // Text round trip.
    expect(tp::readEdit(tp::writeEdit(three), 0).text.def.keymap == three.text.def.keymap, "keys: written and read back the same");
  }
  {
    // The selected sample's values as the right column sets them: clamped as the knobs did.
    tp::Edit e = fixture();
    tp::applySampleField(e, 0, tp::SampleField::RelNote, 100);
    expect(e.text.def.samples[0].relNote == 71, "relative note: at most +71");
    tp::applySampleField(e, 0, tp::SampleField::RelNote, -100);
    expect(e.text.def.samples[0].relNote == -48, "relative note: at least -48");
    tp::applySampleField(e, 0, tp::SampleField::Fine, -300);
    expect(e.text.def.samples[0].finetune == -128, "fine tune: at least -128");
    tp::applySampleField(e, 0, tp::SampleField::Pan, 30);
    expect(e.text.def.samples[0].pan == 158, "pan: +30 from the middle");
    tp::applyFadeout(e, 9999);
    tp::applyVibrato(e, tp::VibField::Depth, 99);
    tp::applyVibrato(e, tp::VibField::Rate, -4);
    expect(e.text.def.fadeout == 4095 && e.text.def.vibDepth == 15 && e.text.def.vibRate == 0, "fadeout and vibrato clamped");
    // The root key and the relative note agree (Root key steps, Octave up).
    tp::Edit r = fixture();
    trk::SampleDef& sd = r.text.def.samples[0];
    const int root = tp::rootNote(sd, 44100.0);
    tp::setRootNote(sd, 44100.0, root + 12);
    expect(tp::rootNote(sd, 44100.0) == root + 12, "root key: an octave up");
    const int rel = sd.relNote;
    tp::applySampleField(r, 0, tp::SampleField::RelNote, rel + 12);
    expect(tp::rootNote(sd, 44100.0) == root, "octave up on the relative note: the root an octave down");
  }

  // K-106 cp18: Convert to Tracker Sampler (the plain Sampler's file and settings).
  {
    PluginState smp = defaultPluginState("gl.sampler");
    smp.data = "/samples/pad.wav";
    if (smp.params.size() <= static_cast<size_t>(pp::SmpLoopMode)) smp.params.resize(static_cast<size_t>(pp::SmpLoopMode) + 1, 0.0f);
    smp.params[pp::SmpVolume] = 0.5f;
    smp.params[pp::SmpPan] = -0.5f;
    smp.params[pp::SmpRoot] = 72;  // C6 in MIDI: the file plays at its own rate on tracker note 61
    smp.params[pp::SmpFine] = 25;
    smp.params[pp::SmpStart] = 0.1f;
    smp.params[pp::SmpEnd] = 0.9f;
    smp.params[pp::SmpLoop] = 1;
    smp.params[pp::SmpLoopMode] = 1;
    smp.params[pp::SmpLoopStart] = 0.25f;
    smp.params[pp::SmpLoopEnd] = 0.75f;
    smp.params[pp::SmpAttack] = 0.1f;
    smp.params[pp::SmpDecay] = 0.2f;
    smp.params[pp::SmpSustain] = 0.5f;
    smp.params[pp::SmpRelease] = 0.3f;
    const int32_t n = 48000;
    const PluginState ts = tp::fromSamplerState(smp, n, 48000.0, "pad");
    expect(ts.type == trk::kInstrumentType && !ts.data.empty(), "convert: a Tracker Sampler with its data");
    const tp::Edit e = tp::readEdit(ts.data, 0);
    const trk::SampleDef& sd = e.text.def.samples[0];
    expect(tp::hasSample(e, 0) && tp::sampleCountOf(e) == 1, "convert: the file as sample 01");
    bool every = true;
    for (int k = 0; k < 96; ++k) every = every && e.text.def.keymap[static_cast<size_t>(k)] == 0;
    expect(every, "convert: sample 01 on every key");
    expect(sd.volume == 32, "convert: the volume (half of full)");
    expect(sd.pan == 64, "convert: the pan (half left)");
    expect(tp::rootNote(sd, 48000.0) == 72 - trk::kPitchOffset, "convert: the root key");
    expect(e.cuts.start[0] == 4800 && e.cuts.end[0] == 43200, "convert: the start and end as the sample's cuts");
    expect(sd.loop == trk::kLoopPingPong && sd.loopStart == 12000 && sd.loopLength == 24000, "convert: the loop points and ping-pong");
    const trk::Envelope& v = e.text.def.vol;
    expect((v.flags & trk::kEnvOn) && (v.flags & trk::kEnvSustain) && v.count == 4, "convert: a volume envelope with a sustain point");
    expect(v.count == 4 && v.x[1] == 5 && v.y[1] == 64 && v.x[2] == 15 && v.y[2] == 32 && v.sustain == 2 && v.x[3] == 30 && v.y[3] == 0,
           "convert: attack, decay to the sustain level, release to silence");
    const PluginState none = tp::fromSamplerState(defaultPluginState("gl.trio"), n, 48000.0, "x");
    expect(none.data.empty() || tp::sampleCountOf(tp::readEdit(none.data, 0)) == 0, "convert: only a Sampler with a file converts");
  }
  // cp20: the pencil's edit: values from a frame on, a NaN keeps its frame, clamped to the wave and to -1..1.
  {
    tp::Wave w;
    w.stereo = true;
    w.L.assign(6, 0.0f);
    w.R.assign(6, 0.25f);
    const std::vector<float> stroke = {0.5f, std::nanf(""), 2.0f, -3.0f, 0.1f};
    expect(tp::waveDraw(w, 1, stroke), "pencil: a stroke changes the wave");
    expect(w.L[0] == 0.0f && w.L[1] == 0.5f && w.L[2] == 0.0f && w.L[3] == 1.0f && w.L[4] == -1.0f && w.L[5] == 0.1f, "pencil: the left values");
    expect(w.R[1] == 0.5f && w.R[2] == 0.25f && w.R[3] == 1.0f && w.R[5] == 0.1f, "pencil: both channels, a NaN keeps its frame");
    expect(!tp::waveDraw(w, 1, stroke), "pencil: the same stroke again changes nothing");
    expect(!tp::waveDraw(w, 6, {0.3f}) && !tp::waveDraw(w, -2, {0.3f, 0.3f}), "pencil: nothing outside the wave");
    expect(tp::waveDraw(w, -1, {0.7f, 0.7f}) && w.L[0] == 0.7f, "pencil: a stroke from before the start draws its part inside");
    expect(w.frames() == 6, "pencil: the length stays");
  }
  return finish();
}
