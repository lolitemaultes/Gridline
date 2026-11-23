// Gridline: a MIDI file imported onto Patch Players (one per track, each on its program; MIDI
// channel 10 on the drum kits; a program the SF2 file lacks on the nearest patch).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/PatternAutomation.h"
#include "patchplayer_rig.h"

using namespace gl;
using namespace gl::packs::samplers;
using namespace sf2t;

int main() {
  gl::test::hermeticTestEnv("midi_import_patchplayer");
  // A small General MIDI style file: Piano (0), Bright Piano (1), Strings (48), Trumpet (56) and
  // a drum kit (bank 128, program 0) and a Brush kit (128:40).
  Bank b;
  b.name = "Mini GM";
  b.samples.push_back(sf2t::Sample{"S", dc(2000, 8000), {}, {}, 44100, 60, 0, 0, 2000, 1, 0});
  b.insts.push_back(Inst{"I", {sf2t::Zone{}.g(SampleModes, 1).g(SampleId, 0)}});
  auto pre = [&](const char* n, uint16_t prog, uint16_t bank) { return Preset{n, prog, bank, {sf2t::Zone{}.g(Instrument, 0)}}; };
  b.presets = {pre("Piano", 0, 0), pre("Bright", 1, 0), pre("Strings", 48, 0), pre("Trumpet", 56, 0), pre("Kit", 0, 128), pre("Brushes", 40, 128)};
  const std::string font = pptest::writeBank(b, "mini gm.sf2");

  // Sixteen tracks, one per MIDI channel; programs 0..15 but channel 10 (index 9) is drums.
  MidiFileData m;
  m.format = 1;
  m.division = 96;
  const int programs[16] = {0, 1, 48, 56, 49, 57, 2, 120, 0, 40, 33, 0, 48, 56, 1, -1};
  for (int ch = 0; ch < 16; ++ch) {
    MidiTrack t;
    t.name = "";
    t.channel = ch;
    t.program = programs[ch];
    if (ch == 9) t.program = 40;  // a Brush kit on the drum channel
    MidiNote n;
    n.start = 96 * ch, n.length = 48, n.pitch = ch == 9 ? 38 : 60 + ch, n.channel = ch;
    t.notes.push_back(n);
    m.tracks.push_back(t);
  }
  MidiFileData parsed;
  std::string err;
  expect(parseMidi(encodeMidi(m, 1), parsed, &err), "the 16-channel file reads back");
  expect(parsed.tracks.size() == 16, "sixteen tracks");

  Song song;
  song.mixer.resize(1);
  song.mixer[0].id = song.allocateId();
  MidiImportOptions o;
  o.instrument = o.drumInstrument = kPatchPlayerType;
  o.setUpChannel = [&](Channel& c, int mch, int bank, int program) { setPatchForMidiTrack(c.plugin, font, mch, bank, program); };
  MidiImportResult res = importMidiIntoSong(song, parsed, "song", o);
  expect(res.error.empty() && res.channels.size() == 16, "one channel per track");
  int right = 0;
  for (size_t i = 0; i < res.channels.size(); ++i) {
    const Channel* c = song.channel(res.channels[i]);
    if (!c) continue;
    const PatchRef pr = parsePatchRef(c->plugin.data);
    const int want = programs[i];
    bool ok = c->plugin.type == kPatchPlayerType && pr.path == font;
    if (i == 9) ok = ok && pr.bank == 128 && pr.program == 40 && pr.name == "Brushes";
    else if (want == 0 || want == 1 || want == 48 || want == 56) ok = ok && pr.bank == 0 && pr.program == want;
    else if (want == 49) ok = ok && pr.program == 48;   // Strings 2 -> the Strings the file has (same family)
    else if (want == 57) ok = ok && pr.program == 56;   // Trombone -> Trumpet (Brass)
    else if (want == 2) ok = ok && (pr.program == 1 || pr.program == 0);  // a piano
    else if (want == -1) ok = ok && pr.program == 0;    // no program change: program 0
    else ok = ok && pr.bank == 0;                        // some melodic patch
    if (ok) ++right;
    else std::printf("track %zu: type %s bank %d program %d name %s\n", i, c->plugin.type.c_str(), pr.bank, pr.program, pr.name.c_str());
  }
  expect(right == 16, "every track on the right patch (drums on bank 128, missing programs on the nearest)");
  // The channels play: a note of each.
  for (size_t i = 0; i < res.channels.size(); i += 5) {
    const Channel* c = song.channel(res.channels[i]);
    if (!c) continue;
    pptest::Rig rig(font);
    rig.st = c->plugin;
    rig.st.params[PpVolume] = 0;
    rig.prepare();
    rig.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    rig.run(L, R, 1000);
    expect(pptest::peak(L) > 0.05f, "an imported channel plays");
  }
  // Every program change of a track is kept, not only the first: read, written back, split by
  // channel, and on a Patch Player a lane of its Patch parameter that switches the patch in time.
  {
    MidiFileData pc;
    pc.format = 1;
    pc.division = 96;
    MidiTrack t;
    t.channel = 2;
    for (int k = 0; k < 4; ++k) {
      MidiNote n;
      n.start = 192 * k, n.length = 96, n.pitch = 60, n.channel = 2;
      t.notes.push_back(n);
    }
    t.programs = {{0, kMidiProgram, 0, 2}, {192, kMidiProgram, 48, 2}, {384, kMidiProgram, 56, 2}, {576, kMidiProgram, 1, 2}};
    t.program = 0;
    pc.tracks.push_back(t);
    MidiFileData back;
    expect(parseMidi(encodeMidi(pc, 1), back, &err) && back.tracks.size() == 1, "a track with four program changes reads back");
    const MidiTrack* bt = back.tracks.empty() ? nullptr : &back.tracks[0];
    bool all = bt && bt->programs.size() == 4 && bt->program == 0 && bt->channelProgram[2] == 0;
    for (size_t k = 0; all && k < 4; ++k) all = bt->programs[k].tick == t.programs[k].tick && bt->programs[k].value == t.programs[k].value && bt->programs[k].channel == 2;
    expect(all, "every program change is kept with its tick (the first one is still the track's program)");
    MidiFileData f0;
    expect(parseMidi(encodeMidi(pc, 0), f0, &err) && f0.tracks.size() == 1 && f0.tracks[0].programs.size() == 4, "a format 0 file keeps them too");
    splitTracksByChannel(f0);
    expect(f0.tracks.size() == 1 && f0.tracks[0].programs.size() == 4, "splitting by channel keeps them");
    // A track mixing two channels: each part keeps its own channel's changes.
    MidiFileData mix = pc;
    MidiNote other;
    other.start = 0, other.length = 96, other.pitch = 40, other.channel = 5;
    mix.tracks[0].notes.push_back(other);
    mix.tracks[0].programs.push_back({96, kMidiProgram, 33, 5});
    MidiFileData mixBack;
    expect(parseMidi(encodeMidi(mix, 1), mixBack, &err), "a two-channel track reads back");
    splitTracksByChannel(mixBack);
    bool split = mixBack.tracks.size() == 2;
    for (const auto& tr : mixBack.tracks) split = split && tr.programs.size() == (tr.channel == 5 ? 1u : 4u);
    expect(split, "split tracks keep their own channel's program changes");

    Song ps;
    ps.mixer.resize(1);
    ps.mixer[0].id = ps.allocateId();
    MidiImportOptions po;
    po.instrument = po.drumInstrument = kPatchPlayerType;
    po.setUpChannel = [&](Channel& c, int mch, int bank, int program) { setPatchForMidiTrack(c.plugin, font, mch, bank, program); };
    po.programParam = [](const Channel& c) { return c.plugin.type == kPatchPlayerType ? static_cast<int>(PpPatch) : -1; };
    MidiImportResult pr = importMidiIntoSong(ps, back, "changes", po);
    const Pattern* pat = pr.channels.empty() ? nullptr : ps.pattern(pr.pattern);
    const PatternAutomation* lane = pat ? findPatternLane(*pat, pr.channels[0], channelPluginParam(pr.channels[0], PpPatch).index) : nullptr;
    const PluginInfo* info = findPlugin(kPatchPlayerType);
    bool laneOk = lane && info && lane->points.size() == 4;
    const int want[4] = {0, 48, 56, 1};
    for (size_t k = 0; laneOk && k < 4; ++k) {
      const double v = info->params[PpPatch].fromNormalized(lane->points[k].value);
      laneOk = std::lround(v) == want[k] + 1 && lane->points[k].pos == static_cast<Tick>(192 * k) * ps.ppq / 96;
    }
    expect(laneOk, "on a Patch Player the program changes become its Patch lane, each at its time");
    // One program only (or none): no lane, as before.
    MidiImportResult one = importMidiIntoSong(ps, parsed, "one", po);
    const Pattern* p1 = one.channels.empty() ? nullptr : ps.pattern(one.pattern);
    expect(p1 && p1->automation.empty(), "a track with a single program change has no Patch lane");
  }
  // Without the option, the import is as before.
  Song plain;
  plain.mixer.resize(1);
  plain.mixer[0].id = plain.allocateId();
  MidiImportResult before = importMidiIntoSong(plain, parsed, "song");
  const Channel* c0 = before.channels.empty() ? nullptr : plain.channel(before.channels[0]);
  expect(c0 && c0->plugin.type != kPatchPlayerType && c0->plugin.data.empty(), "the default import is unchanged");
  return finish();
}
