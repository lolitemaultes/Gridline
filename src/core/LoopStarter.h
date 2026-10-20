// Gridline — loop starter (GEN-01): builds a ready-to-play project from a
// style, tempo, key / scale, mood and energy. Four parts (drums, bass,
// chords, lead) each have their own seed, so a part can be rolled again (or
// locked) without touching the others; the same settings always give the
// same project. Sounds are Gridline's own instruments and effects (factory
// presets of the built-in plugins), routed to named mixer inserts with two
// send buses (reverb, delay) and a gentle master chain. The chords come from
// the chord progression engine (core/ChordTools.h); bass and lead follow them.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "Song.h"

namespace gl::loopstarter {

enum Part : int { kDrums = 0, kBass, kChords, kLead, kPartCount };
const char* partName(int part);  // "Drums", "Bass", "Chords", "Lead"

struct StyleInfo {
  const char* name;   // "House"
  const char* blurb;  // one line for the style list
  double tempoMin, tempoMax, tempoDefault;
  int defaultKey;     // 0..11
  int defaultScale;   // tools::scales() index
};
const std::vector<StyleInfo>& styles();

enum class Mood : int { Dark = 0, Warm, Bright, Dreamy, Count };
const char* moodName(Mood m);
// Length of what is built: a loop of 4 or 8 bars, or a short arrangement
// (intro, groove, main, break, main, outro) of 4-bar patterns.
enum class Form : int { Loop4 = 0, Loop8, Arrangement, Count };
const char* formName(Form f);
// Scales the generator offers (seven-note scales, tools::scales() indexes):
// every chord, bass and lead note stays inside the chosen scale.
const std::vector<int>& scaleChoices();

struct Settings {
  int style = 0;
  double tempo = 0;  // BPM; 0 = the style's default
  int key = 9;       // 0..11 (A)
  int scale = 2;     // tools::scales() index (natural minor)
  Mood mood = Mood::Warm;
  double energy = 0.6;  // 0..1: density and drive of drums, bass and lead
  Form form = Form::Loop4;
  std::array<uint32_t, kPartCount> seeds{{1, 2, 3, 4}};
  std::array<bool, kPartCount> locked{{false, false, false, false}};
  bool operator==(const Settings& o) const {
    return style == o.style && tempo == o.tempo && key == o.key && scale == o.scale && mood == o.mood && energy == o.energy &&
           form == o.form && seeds == o.seeds && locked == o.locked;
  }
  bool operator!=(const Settings& o) const { return !(*this == o); }
};

// A style's tempo, key and scale with seeds derived from `seed`.
Settings styleDefaults(int style, uint32_t seed = 1);
double effectiveTempo(const Settings& s);
// New seeds from `entropy` (deterministic): `part` >= 0 rolls that part again
// unless it is locked; -1 rolls every unlocked part.
void reroll(Settings& s, int part, uint32_t entropy);
// The project. Channels: Kick, Snare / Clap, Hat, Open hat, Perc, Bass,
// Chords, Lead (each on its own mixer insert); patterns: Drums, Bass, Chords,
// Lead (+ Drums light in the arrangement); a key marker at the start.
Song generate(const Settings& s);
// "House · 124 BPM · A Natural minor · 4 bars"
std::string describe(const Settings& s);

// Channel names of a part's channels in generate()'s songs (tests, re-roll).
std::vector<std::string> partChannels(int part);

// The sounds a style uses for a part (drums: kick, backbeat, hat, open hat,
// perc; other parts: the alternatives a seed picks from) and the one picked.
struct Sound {
  std::string type, preset;  // plugin type, factory preset name ("" = defaults)
  float gain = 0.78f;        // channel volume
};
std::vector<Sound> soundOptions(int style, int part);
std::vector<Sound> chosenSounds(const Settings& s, int part);

}  // namespace gl::loopstarter
