// Gridline - the Tracker's demo module (see TrackerDemo.h). Original music: A minor, 126 BPM,
// Am7 - Fmaj7 - Dm7 - E7 over each 64-row pattern (four bars of sixteen rows).
#include "TrackerDemo.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include "Demo.h"
#include "PluginInfo.h"
#include "TrackerData.h"

namespace gl {

namespace {

constexpr int kRows = 64, kBarRows = 16;

struct Chord {
  int root;                 // bass note (MIDI)
  std::array<int, 4> tones; // chord tones, low to high (MIDI, around middle C)
};
// Am7, Fmaj7, Dm7, E7.
constexpr std::array<Chord, 4> kChords = {{{45, {57, 60, 64, 67}}, {41, {57, 60, 64, 65}}, {38, {57, 60, 62, 65}}, {40, {56, 59, 62, 64}}}};

struct Writer {
  Song& s;
  Pattern& p;
  Tick row;  // ticks a row
  void note(ObjectId ch, int r, int pitch, int lenRows, int vel) {
    if (ch == kNoId || r < 0 || r >= kRows) return;
    Note n;
    n.start = row * r;
    n.length = row * std::max(1, std::min(lenRows, kRows - r));
    n.pitch = std::clamp(pitch, 0, 127);
    n.velocity = std::clamp(vel, 1, 127);
    p.notes[ch].push_back(n);
  }
};

ObjectId find(const Song& s, const char* name) {
  for (const Channel& c : s.channels)
    if (c.name == name) return c.id;
  return kNoId;
}

}  // namespace

Song makeTrackerDemo() {
  Song s = makeDemoTrack();
  s.patterns.clear();
  s.playlist.clear();
  s.automations.clear();
  s.markers.clear();
  s.tempo = 126;
  s.info.title = "Glasswork";
  s.info.genre = "Tracker";
  s.info.author = "Gridline";
  s.info.comments = "The Tracker's demo module: 64-row patterns on many tracks at once, made with the built-in instruments.";

  // Three Tracker Samplers playing their built-in sounds: lead, arpeggio, bell.
  const ObjectId keys = find(s, "Felt Keys");
  const ObjectId keysMix = s.channel(keys) ? s.channel(keys)->mixerTrack : kNoId;  // (read now: adding channels moves them)
  auto addSampler = [&](const char* name, int sound, Rgb color) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.color = color;
    c.plugin = defaultPluginState(tracker::kInstrumentType);
    if (c.plugin.params.size() > static_cast<size_t>(tracker::PSound)) c.plugin.params[tracker::PSound] = static_cast<float>(sound);
    c.gain = 0.5f;
    c.mixerTrack = keysMix;
    s.channels.push_back(c);
    return c.id;
  };
  const ObjectId lead = addSampler("Saw Lead", 2, 0xE0A040), arp = addSampler("Glass Arp", 1, 0x58B8E0), bell = addSampler("Tine Bell", 3, 0xC890E0);

  // The tracks the pattern view shows first: drums, bass, pads and keys, the three samplers, then
  // the plucks and effects; every other instrument after them.
  const char* const order[] = {"Kick", "Snare", "Clap", "Hat", "Open Hat", "Shaker", "Sub", "Reese", "Haze Pad", "Felt Keys",
                               "Saw Lead", "Glass Arp", "Tine Bell", "Graphite Mallet", "Felt Pluck", "Ride", "Impact", "Riser"};
  std::vector<Channel> sorted;
  for (const char* n : order)
    for (const Channel& c : s.channels)
      if (c.name == n) sorted.push_back(c);
  for (const Channel& c : s.channels)
    if (std::find_if(sorted.begin(), sorted.end(), [&](const Channel& x) { return x.id == c.id; }) == sorted.end()) sorted.push_back(c);
  s.channels = std::move(sorted);

  const ObjectId kick = find(s, "Kick"), snare = find(s, "Snare"), clap = find(s, "Clap"), hat = find(s, "Hat"), open = find(s, "Open Hat"),
                 shaker = find(s, "Shaker"), sub = find(s, "Sub"), reese = find(s, "Reese"), pad = find(s, "Haze Pad"),
                 mallet = find(s, "Graphite Mallet"), pluck = find(s, "Felt Pluck"), ride = find(s, "Ride"), impact = find(s, "Impact"),
                 riser = find(s, "Riser");
  const Tick row = std::max<Tick>(1, s.ppq / 4);

  // The theme (bar, row, pitch, length in rows).
  struct Mel {
    int bar, r, pitch, len;
  };
  static const Mel theme[] = {{0, 0, 76, 4},  {0, 4, 72, 2},  {0, 6, 74, 2},  {0, 8, 76, 6},  {0, 14, 79, 2},  {1, 0, 77, 4},  {1, 4, 76, 2},
                              {1, 6, 72, 2},  {1, 8, 69, 6},  {1, 14, 72, 2}, {2, 0, 74, 3},  {2, 3, 77, 3},  {2, 6, 81, 4},  {2, 10, 79, 2},
                              {2, 12, 77, 4}, {3, 0, 76, 6},  {3, 6, 80, 2},  {3, 8, 83, 4},  {3, 12, 80, 2}, {3, 14, 76, 2}};
  static const Mel bells[] = {{0, 0, 88, 8}, {0, 10, 84, 6}, {1, 0, 84, 8}, {1, 10, 81, 6}, {2, 0, 86, 8}, {2, 10, 89, 6}, {3, 0, 88, 8}, {3, 10, 92, 6}};

  enum Part { Intro, Groove, Theme, Break };
  auto make = [&](Part part, const char* name, Rgb color) {
    Pattern p;
    p.id = s.allocateId();
    p.name = name;
    p.color = color;
    p.stepsPerBar = 16;
    p.bars = 4;
    Writer w{s, p, row};
    const bool drums = part == Groove || part == Theme;
    for (int b = 0; b < 4; ++b) {
      const int o = b * kBarRows;
      const Chord& ch = kChords[static_cast<size_t>(b)];
      // Drums.
      if (drums) {
        for (int r : {0, 4, 8, 12}) w.note(kick, o + r, 60, 2, r == 0 ? 120 : 108);
        if (b == 3) w.note(kick, o + 14, 60, 2, 92);
        w.note(snare, o + 4, 60, 2, 112);
        w.note(snare, o + 12, 60, 2, 116);
        if (b % 2 == 1) w.note(clap, o + 12, 60, 2, 100);
        for (int r : {2, 6, 10, 14}) w.note(open, o + r, 60, 2, 84);
        if (part == Theme)
          for (int r = 1; r < kBarRows; r += 2) w.note(shaker, o + r, 60, 1, r % 4 == 3 ? 76 : 58);
        if (part == Groove) w.note(ride, o + 0, 60, 4, 70);
      }
      if (part != Break)
        for (int r = 0; r < kBarRows; r += 2) w.note(hat, o + r, 60, 1, r % 4 == 0 ? 96 : 68);
      else
        for (int r = 0; r < kBarRows; r += 4) w.note(hat, o + r, 60, 1, 60);
      // Bass.
      if (part != Intro) {
        w.note(sub, o + 0, ch.root, 6, 112);
        w.note(sub, o + 6, ch.root, 2, 96);
        w.note(sub, o + 8, ch.root + 12, 4, 104);
        w.note(sub, o + 12, ch.root + 7, 4, 96);
      }
      if (part == Theme) w.note(reese, o, ch.root + 12, kBarRows, 88);
      // Pad: the chord's three upper tones, the whole bar.
      for (int k = 1; k < 4; ++k) w.note(pad, o, ch.tones[static_cast<size_t>(k)], kBarRows, part == Break ? 90 : 74);
      // Keys: off-beat stabs of the whole chord.
      if (drums)
        for (int r : {2, 7, 10})
          for (int t : ch.tones) w.note(keys, o + r, t, 2, 82);
      // Arpeggio: every row, up through the chord two octaves above (the intro every other row).
      for (int r = 0; r < kBarRows; r += part == Intro ? 2 : 1) {
        const int k = r % 8 < 4 ? r % 4 : 3 - r % 4;
        w.note(arp, o + r, ch.tones[static_cast<size_t>(k)] + 12, 1, r % 4 == 0 ? 100 : 78);
      }
      // Plucks on the off-beats, the mallet in the quiet parts.
      if (part == Groove)
        for (int r : {3, 7, 11, 15}) w.note(pluck, o + r, ch.tones[static_cast<size_t>((r / 4) % 4)] + 12, 1, 86);
      if (part == Intro || part == Break)
        for (int r = 0; r < kBarRows; r += 4) w.note(mallet, o + r, ch.tones[static_cast<size_t>((r / 4) % 4)] + (r == 8 ? 12 : 0), 3, 90);
    }
    if (part == Theme) {
      for (const Mel& m : theme) w.note(lead, m.bar * kBarRows + m.r, m.pitch, m.len, 104);
      for (const Mel& m : bells) w.note(bell, m.bar * kBarRows + m.r, m.pitch - 12, m.len, 80);
    }
    if (part == Break) {
      for (const Mel& m : bells) w.note(bell, m.bar * kBarRows + m.r, m.pitch - 12, m.len, 92);
      w.note(riser, 3 * kBarRows, 60, kBarRows, 100);
    }
    if (part == Groove) w.note(impact, 0, 60, 8, 110);
    for (auto& [id, v] : p.notes)
      std::sort(v.begin(), v.end(), [](const Note& x, const Note& y) { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; });
    s.patterns.push_back(p);
    return s.patterns.back().id;
  };
  const ObjectId pIntro = make(Intro, "Intro", 0x5A8FD8), pGroove = make(Groove, "Groove", 0x6E9F4B), pTheme = make(Theme, "Theme", 0xE0A040),
                 pBreak = make(Break, "Break", 0xA070C8);

  // The order list: one pattern clip after another on the first lane.
  const Tick len = row * kRows;
  Tick at = 0;
  for (ObjectId pid : {pIntro, pGroove, pTheme, pGroove, pBreak, pTheme, pTheme}) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = pid;
    c.track = 0;
    c.start = at;
    c.length = len;
    s.playlist.push_back(c);
    at += len;
  }
  if (!s.tracks.empty()) s.tracks[0].name = "Song";
  return s;
}

}  // namespace gl
