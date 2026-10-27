// Gridline - the test song: a compact 16-bar house loop (8 channels of built-in
// instruments, 4 patterns, mixer effects, 3 playlist tracks) that many tests
// build on. Test fixture only: the app's demo song is "Gridline" (core/Demo.h).
// Keep it exactly as it is: determinism, hash, perf and export tests pin its content.
#pragma once
#include <initializer_list>
#include <vector>

#include "../../src/core/Document.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"

namespace gl {

inline Song makeTestSong() {
  Document d(makeEmptySong());
  const Song& s = d.song();
  d.setParam(songParam(SongParam::Tempo), 124.0, false);
  d.setParam(songParam(SongParam::Swing), 0.18, false);
  SongInfo info;
  info.title = "Gridline Demo";
  info.author = "Gridline";
  info.genre = "House";
  d.setInfo(info);

  struct Ch { const char* name; const char* type; uint32_t color; };
  const Ch chans[] = {{"Kick", "gl.kick", 0xD8775A}, {"Clap", "gl.clap", 0xC9A43C}, {"Hat", "gl.hat", 0x4FB8B0},
                      {"Open hat", "gl.hat", 0x4FB8B0}, {"Snare", "gl.snare", 0x5A8FD8}, {"Bass", "gl.synth", 0xA56CD8},
                      {"Chords", "gl.keys", 0x7A6FE0}, {"Perc", "gl.pads", 0xC9A43C}};
  ObjectId id[8];
  for (int i = 0; i < 8; ++i) {
    id[i] = d.addChannel(chans[i].name, chans[i].type);
    ObjectId mt = s.mixer[static_cast<size_t>(i + 1)].id;
    d.editChannel(id[i], "", [&](Channel& c) {
      c.color = chans[i].color;
      c.mixerTrack = mt;
    });
    d.editMixerTrack(mt, "", [&](MixerTrack& m) {
      m.name = chans[i].name;
      m.color = chans[i].color;
    });
  }
  using namespace pp;
  d.setParam(channelPluginParam(id[3], HatOpen), 1);
  d.setParam(channelPluginParam(id[3], HatDecay), 0.12);
  d.setParam(channelParam(id[2], ChannelParam::Volume), 0.6);
  d.setParam(channelParam(id[2], ChannelParam::Pan), 0.15);
  d.setParam(channelParam(id[3], ChannelParam::Volume), 0.55);
  d.setParam(channelParam(id[3], ChannelParam::Pan), -0.15);
  // Bass: mono saw+square, short filter pluck.
  d.setParam(channelPluginParam(id[5], SynOsc2Coarse), 0);
  d.setParam(channelPluginParam(id[5], SynOsc2Level), 0.5);
  d.setParam(channelPluginParam(id[5], SynCutoff), 700);
  d.setParam(channelPluginParam(id[5], SynFilterEnv), 0.6);
  d.setParam(channelPluginParam(id[5], SynFilterDecay), 0.18);
  d.setParam(channelPluginParam(id[5], SynVoices), 1);
  d.setParam(channelPluginParam(id[5], SynRelease), 0.08);
  // Chords: Keys with a little tremolo.
  d.setParam(channelPluginParam(id[6], KyTremDepth), 0.25);
  d.setParam(channelPluginParam(id[6], KyVolume), 0.8);

  // Effects.
  ObjectId clapT = s.mixer[2].id, chordT = s.mixer[7].id, bassT = s.mixer[6].id, master = s.masterId();
  d.setSlotPlugin(clapT, 0, "gl.reverb");
  d.setParam(slotParam(clapT, 0, RvbWet), 0.22);
  d.setSlotPlugin(chordT, 0, "gl.chorus");
  d.setSlotPlugin(chordT, 1, "gl.delay");
  d.setParam(slotParam(chordT, 1, DlyWet), 0.22);
  d.setSlotPlugin(chordT, 2, "gl.reverb");
  d.setParam(slotParam(chordT, 2, RvbWet), 0.3);
  d.setSlotPlugin(bassT, 0, "gl.eq");
  d.setParam(slotParam(bassT, 0, eqParam(0, EqGain)), 3);
  d.setSlotPlugin(master, 0, "gl.comp");
  d.setParam(slotParam(master, 0, CmpThreshold), -14);
  d.setParam(slotParam(master, 0, CmpRatio), 2.5);
  d.setSlotPlugin(master, 1, "gl.limiter");

  const int ppq = s.ppq;
  const Tick step = ppq / 4;
  // Patterns.
  ObjectId pDrums = s.patterns[0].id;
  d.renamePattern(pDrums, "Beat");
  auto steps = [&](ObjectId pat, ObjectId ch, std::initializer_list<int> on, uint8_t vel = 100, int total = 16) {
    std::vector<uint8_t> row(static_cast<size_t>(total), 0);
    for (int k : on) row[static_cast<size_t>(k)] = vel;
    d.setSteps(pat, ch, row);
  };
  steps(pDrums, id[0], {0, 4, 8, 12}, 120);
  steps(pDrums, id[1], {4, 12}, 105);
  steps(pDrums, id[2], {0, 2, 4, 6, 8, 10, 12, 14}, 70);
  steps(pDrums, id[3], {2, 6, 10, 14}, 85);
  {  // Perc: rim on the off-beats, a tom fill at the end of the bar (drum pads 8, 6, 7).
    std::vector<Note> ns;
    for (int k : {3, 7, 11}) ns.push_back({k * step, step, 67, 70});
    ns.push_back({13 * step, step, 65, 90});
    ns.push_back({14 * step, step, 66, 90});
    d.setNotes(pDrums, id[7], ns);
  }

  ObjectId pFill = d.addPattern("Fill");
  steps(pFill, id[0], {0, 4, 8, 12}, 120);
  steps(pFill, id[1], {4, 12}, 105);
  steps(pFill, id[4], {10, 13, 14, 15}, 95);
  steps(pFill, id[2], {0, 2, 4, 6, 8}, 70);

  ObjectId pBass = d.addPattern("Bassline", 2);
  {
    const int line[] = {33, 33, 45, 33, 36, 36, 48, 31};
    std::vector<Note> ns;
    for (int i = 0; i < 16; ++i) {
      int p = line[i % 8];
      ns.push_back({static_cast<Tick>(i) * 2 * step + step, step, p, i % 4 == 0 ? 110 : 90});
    }
    d.setNotes(pBass, id[5], ns);
  }
  ObjectId pChords = d.addPattern("Chords", 2);
  {
    const int chords[4][3] = {{57, 60, 64}, {57, 60, 64}, {55, 60, 64}, {53, 57, 60}};
    std::vector<Note> ns;
    for (int c = 0; c < 4; ++c)
      for (int k = 0; k < 3; ++k) {
        ns.push_back({c * 8 * step + 2 * step, 3 * step, chords[c][k], 85});
        ns.push_back({c * 8 * step + 6 * step, 2 * step, chords[c][k], 75});
      }
    d.setNotes(pChords, id[6], ns);
  }

  // Arrangement: 16 bars.
  Tick bar = s.barTicks();
  for (int b = 0; b < 16; ++b) d.addClip(b % 4 == 3 ? pFill : pDrums, 0, b * bar);
  for (int b = 4; b < 16; b += 2) d.addClip(pBass, 1, b * bar);
  for (int b = 8; b < 16; b += 2) d.addClip(pChords, 2, b * bar);
  d.editPlaylistTrack(0, "", [](PlaylistTrack& t) { t.name = "Drums"; t.color = 0xD8775A; });
  d.editPlaylistTrack(1, "", [](PlaylistTrack& t) { t.name = "Bass"; t.color = 0xA56CD8; });
  d.editPlaylistTrack(2, "", [](PlaylistTrack& t) { t.name = "Chords"; t.color = 0x7A6FE0; });
  return d.song();
}

}  // namespace gl
