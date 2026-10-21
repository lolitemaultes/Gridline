// Gridline — loop starter (see LoopStarter.h). Original grooves, recipes and
// sound choices: every sound is a factory preset of a Gridline instrument.
#include "LoopStarter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "ChordTools.h"
#include "NoteTools.h"
#include "PluginParams.h"

namespace gl::loopstarter {

namespace {

// ------------------------------------------------------------------ random ----
uint64_t mixHash(uint64_t a, uint64_t b) {
  uint64_t z = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {  // splitmix64
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
  bool chance(double p) { return uni() < p; }
  int jitter(int amount) { return below(2 * amount + 1) - amount; }
};

int mod12(int v) { return ((v % 12) + 12) % 12; }

// ------------------------------------------------------------------ recipes ----
struct Snd {
  const char* type;
  const char* preset;  // factory preset name ("" = defaults)
  float gain;          // channel volume (0.78 = unity)
};

enum class LeadMode { Arp, Melody };

struct Recipe {
  float swing;
  // Drums: 16-step templates per bar ('x' accent, 'o' normal, '.' ghost, '-' off).
  Snd kick, snare, hat, ohat, perc;
  bool clap;      // the backbeat channel is a clap
  bool hatNotes;  // hats as notes with rolls (trap)
  std::vector<const char*> kickT, snareT, hatT, ohatT;
  // Perc: 'r' rim, 'l' low tom, 'h' high tom, 's' shaker, 'c' clap (upper case = accent).
  std::vector<const char*> percT;
  // Bass: 'R' root, 'O' octave, 'F' fifth, 'A' approach to the next chord, '_' tie, '.' rest.
  std::vector<Snd> bass;
  std::vector<const char*> bassT;
  int bassLo;  // lowest bass root (MIDI)
  // Chords: degree progressions (1-based, one chord per bar), chord rhythms.
  std::vector<Snd> chords;
  std::vector<std::array<int, 4>> progressions;
  bool sevenths;
  int chordOctave;  // ChordProgression::octave (C5 = 60)
  std::vector<const char*> chordRhythms;
  float chordGate;
  // Lead.
  std::vector<Snd> lead;
  LeadMode leadMode;
  std::vector<const char*> leadT;  // rhythm: 'x' onset, '_' hold, '.' rest
  int leadLo;                      // lowest lead pitch
  float reverb, delay;             // send levels to the buses
};

const std::vector<Recipe>& recipes() {
  static const std::vector<Recipe> r = [] {
    std::vector<Recipe> v;
    // 0 House
    v.push_back(Recipe{
        0.10f,
        {"gl.kick", "Punchy", 0.54f}, {"gl.clap", "Default", 0.8f}, {"gl.hat", "Closed", 0.9f}, {"gl.hat", "Open", 0.6f},
        {"gl.pads", "", 0.6f}, true, false,
        {"x---x---x---x---"},
        {"----x-------x---", "----x-------x--."},
        {"o.-.o.-.o.-.o.-.", "x-.-x-.-x-.-x-.-", "o.-oo.-.o.-oo.-."},
        {"--x---x---x---x-"},
        {"---r--r----r--R-", "-------r-s-----r", "--s---s---s-r-s-"},
        {{"gl.analog", "Rubber Bass", 0.54f}, {"gl.fingerbass", "Deep Round", 0.66f}, {"gl.pocketsynth", "Fat Bass", 1.1f}},
        {"..R_..R_..R_..R_", "..R...O...R...O.", "..R_..R_..R_.AR_"},
        33,
        {{"gl.keys", "Classic tine", 0.54f}, {"gl.fm", "Organ", 0.5f}, {"gl.analog", "Velvet Pad", 0.57f}},
        {{{1, 6, 4, 5}}, {{6, 4, 1, 5}}, {{2, 5, 1, 6}}, {{1, 4, 6, 5}}},
        true, 5, {"Offbeat stabs", "Two pushes", "Push and hold"}, 0.9f,
        {{"gl.analog", "Glass Pluck", 0.6f}, {"gl.pluck", "Harp", 0.56f}, {"gl.twinva", "Arp Pluck", 0.52f}},
        LeadMode::Arp, {"x.x.xx.x.x.xx.x.", "x_x_x_x_x_x_x_x_", "..x_..x_..x_x_x_"}, 72, 0.22f, 0.18f});
    // 1 Techno
    v.push_back(Recipe{
        0.0f,
        {"gl.kick", "Punchy", 0.52f}, {"gl.clap", "Wide", 1.0f}, {"gl.hat", "Closed", 0.9f}, {"gl.hat", "Open", 0.72f},
        {"gl.pads", "", 0.4f}, true, false,
        {"x---x---x---x---", "x---x---x---x--o"},
        {"----x-------x---", "----o-------o---", "------------x---"},
        {"..x...x...x...x.", "o.x.o.x.o.x.o.x.", "-ox--ox--ox--ox-"},
        {"--o---o---o---o-", "------o-------o-"},
        {"r--r--r--r--r--r", "--r--h--r--h--l-", "-s-s-s-s-s-s-s-s"},
        {{"gl.acid", "Classic", 0.5f}, {"gl.acid", "Squelch", 0.5f}, {"gl.analog", "Rubber Bass", 0.5f}},
        {".RRR.RRR.RRR.RRR", "..R_..R_..R_..R_", ".R.R.R.R.R.R.R.O"},
        33,
        {{"gl.analog", "PWM Strings", 0.5f}, {"gl.dcosynth", "Polar Strings", 1.05f}, {"gl.fm", "Organ", 0.56f}},
        {{{1, 1, 6, 6}}, {{1, 7, 1, 6}}, {{1, 1, 4, 4}}, {{1, 6, 1, 7}}},
        false, 5, {"Offbeat stabs", "Held"}, 0.8f,
        {{"gl.analog", "Arp Bubbles", 0.72f}, {"gl.fm", "Bell", 0.57f}, {"gl.twinva", "Arp Pluck", 0.58f}},
        LeadMode::Arp, {"x.xx.xx.x.xx.xx.", "x..x..x..x..x.x.", "xxxxxxxxxxxxxxxx"}, 69, 0.16f, 0.22f});
    // 2 Hip hop (boom bap)
    v.push_back(Recipe{
        0.55f,
        {"gl.kick", "Default", 0.68f}, {"gl.snare", "Fat", 0.64f}, {"gl.hat", "Closed", 1.05f}, {"gl.hat", "Open", 0.9f},
        {"gl.pads", "", 0.7f}, false, false,
        {"x------o--x-----", "x-----o---x--o--", "x--o------x-----", "x------x-o------"},
        {"----x-------x---", "----x-------x--."},
        {"o-o-o-o-o-o-o-o-", "x-o-x-o-x-o-x-o-", "o-o-o-ooo-o-o-o-"},
        {"----------------", "--------------o-"},
        {"-----r---------r", "--s---s---s---s-", "-------r------r-"},
        {{"gl.fingerbass", "Punchy Mid", 0.74f}, {"gl.fingerbass", "Deep Round", 0.71f}, {"gl.analog", "Fingered Bass", 1.0f}},
        {"R______.R__.F___", "R_____R_.__R____", "R___..R_R___.A__"},
        33,
        {{"gl.keys", "Soft", 0.56f}, {"gl.fmduo", "Tine Piano", 0.5f}, {"gl.fm", "E. piano", 0.36f}},
        {{{2, 5, 1, 6}}, {{1, 4, 1, 4}}, {{6, 2, 5, 1}}, {{1, 6, 2, 5}}},
        true, 5, {"Held", "Push and hold", "Two pushes"}, 0.95f,
        {{"gl.pluck", "Nylon", 0.5f}, {"gl.fm", "Bell", 0.53f}, {"gl.keys", "Classic tine", 0.57f}},
        LeadMode::Melody, {"x_.x_.x_x___....", "..x_x_x___x_x___", "x___x___x_x_x___"}, 67, 0.15f, 0.12f});
    // 3 Trap
    v.push_back(Recipe{
        0.0f,
        {"gl.kick", "Punchy", 0.55f}, {"gl.clap", "Snap", 1.05f}, {"gl.hat", "Closed", 0.85f}, {"gl.hat", "Open", 0.6f},
        {"gl.pads", "", 0.6f}, true, true,
        {"x------x--x-----", "x-----x---x-----", "x--x------x---x-"},
        {"--------x-------"},
        {"o-o-o-o-o-o-o-o-"},
        {"----------------", "--------------o-"},
        {"---------------r", "-----r-----r----", "----------------"},
        {{"gl.wavetable", "Sub Pressure", 0.57f}, {"gl.pocketsynth", "Fat Bass", 0.87f}, {"gl.additive", "Sub bass", 0.77f}},
        {"R______R__R_____", "R_____R___R_____", "R__R______R___A_"},
        33,
        {{"gl.analog", "Velvet Pad", 0.6f}, {"gl.hexsynth", "Soft Pad", 0.92f}, {"gl.pocketsynth", "Spread Pad", 0.38f}},
        {{{1, 6, 3, 7}}, {{1, 1, 6, 6}}, {{1, 4, 6, 5}}, {{6, 6, 1, 7}}},
        false, 5, {"Held", "Two pushes"}, 1.0f,
        {{"gl.fm", "Bell", 0.51f}, {"gl.wavetable", "Glass Bell", 0.51f}, {"gl.plucker", "Long bell", 0.71f}},
        LeadMode::Melody, {"x_.x_.x_..x_x___", "x.x.x___..x.x___", "x__x__x_x_......"}, 72, 0.18f, 0.2f});
    // 4 Lo-fi
    v.push_back(Recipe{
        0.6f,
        {"gl.kick", "Lo-fi", 0.55f}, {"gl.snare", "Tight", 0.89f}, {"gl.hat", "Soft shaker", 1.1f}, {"gl.hat", "Open", 0.6f},
        {"gl.pads", "", 0.45f}, false, false,
        {"x-----o---x-----", "x------o-ox-----", "x--o--o---x-----"},
        {"----x-------x---"},
        {"o-o-o-o-o-o-o-o-", "o-.-o-.-o-.-o-.-"},
        {"----------------"},
        {"..s...s...s...s.", "-------r-------r", "----------------"},
        {{"gl.fingerbass", "Deep Round", 0.69f}, {"gl.fingerbass", "Flat", 0.73f}},
        {"R_______F___R___", "R_____R_.___O___", "R___.__.R_____A_"},
        33,
        {{"gl.keys", "Soft", 0.57f}, {"gl.fmduo", "Tine Piano", 0.47f}, {"gl.keys", "Wide tremolo", 0.61f}},
        {{{2, 5, 1, 6}}, {{4, 3, 2, 1}}, {{1, 6, 2, 5}}, {{4, 5, 3, 6}}},
        true, 5, {"Held", "Push and hold"}, 1.0f,
        {{"gl.pluck", "Nylon", 0.55f}, {"gl.plucker", "Soft harp", 0.9f}, {"gl.keys", "Soft", 0.59f}},
        LeadMode::Melody, {"x__x__x_....x___", "..x_x_x_____....", "x_x_x___x_______"}, 67, 0.2f, 0.12f});
    // 5 Drum and bass
    v.push_back(Recipe{
        0.0f,
        {"gl.kick", "Punchy", 0.59f}, {"gl.snare", "Tight", 0.67f}, {"gl.hat", "Closed", 0.84f}, {"gl.hat", "Open", 0.6f},
        {"gl.pads", "", 0.4f}, false, false,
        {"x---------x-----", "x-------x-x-----", "x---------x---o-"},
        {"----x-------x---", "----x-------x--.", "----x--.----x---"},
        {"o-o-o-o-o-o-o-o-", "o.o.o.o.o.o.o.o."},
        {"----------------", "------o-------o-"},
        {"--r-----r-r-----", "-------h------l-", "s-s-s-s-s-s-s-s-"},
        {{"gl.sawswarm", "Twin Saw Bass", 0.98f}, {"gl.wavetable", "Growl Bass", 0.43f}, {"gl.analog", "Wobble Bass", 0.42f}},
        {"R_______________", "R_______R_______", "R___________A___"},
        33,
        {{"gl.sawswarm", "Wide Pad", 1.25f}, {"gl.analog", "PWM Strings", 0.45f}, {"gl.pocketsynth", "Spread Pad", 0.38f}},
        {{{1, 6, 4, 5}}, {{1, 7, 6, 7}}, {{6, 4, 1, 5}}, {{1, 3, 6, 7}}},
        false, 5, {"Held", "Two pushes"}, 1.0f,
        {{"gl.analog", "Glass Pluck", 0.63f}, {"gl.pocketsynth", "Bell Keys", 0.66f}, {"gl.wavetable", "Glass Bell", 0.51f}},
        LeadMode::Arp, {"x_x_x_x_x_x_x_x_", "x.x.xx.x.x.xx.x.", "x___x___x_x_x___"}, 72, 0.22f, 0.2f});
    // 6 Pop
    v.push_back(Recipe{
        0.0f,
        {"gl.kick", "Default", 0.59f}, {"gl.clap", "Default", 0.82f}, {"gl.hat", "Closed", 0.9f}, {"gl.hat", "Open", 0.6f},
        {"gl.pads", "", 0.9f}, true, false,
        {"x-------x-------", "x-----x-x-------", "x-------x-x-----"},
        {"----x-------x---"},
        {"o-o-o-o-o-o-o-o-", "x-o-x-o-x-o-x-o-"},
        {"--------------o-", "----------------"},
        {"------r-------r-", "-s-s-s-s-s-s-s-s", "----------------"},
        {{"gl.analog", "Fingered Bass", 0.86f}, {"gl.pocketsynth", "Fat Bass", 0.87f}, {"gl.fingerbass", "Punchy Mid", 0.58f}},
        {"R_R_R_R_R_R_R_R_", "R__R__R_R__R__R_", "R___R___R___R_O_"},
        33,
        {{"gl.fmduo", "Tine Piano", 0.49f}, {"gl.keys", "Classic tine", 0.56f}, {"gl.dcosynth", "Polar Strings", 0.97f}},
        {{{1, 5, 6, 4}}, {{6, 4, 1, 5}}, {{1, 6, 4, 5}}, {{4, 1, 5, 6}}},
        false, 5, {"Quarter pulse", "Eighth pulse", "Push and hold", "Two pushes"}, 0.85f,
        {{"gl.pocketsynth", "Super Lead", 0.41f}, {"gl.pocketsynth", "Bell Keys", 0.75f}, {"gl.sawswarm", "Swarm Lead", 1.11f}},
        LeadMode::Melody, {"x_x_x___x_x_x___", "..x_x_x_x_x_x___", "x___x_x_x___x_x_"}, 69, 0.16f, 0.14f});
    // 7 Ambient
    v.push_back(Recipe{
        0.0f,
        {"gl.kick", "Lo-fi", 0.45f}, {"gl.snare", "Rim", 0.97f}, {"gl.hat", "Soft shaker", 0.9f}, {"gl.hat", "Open", 0.5f},
        {"gl.pads", "", 0.45f}, false, false,
        {"x---------------", "x-------x-------"},
        {"--------o-------", "----------------"},
        {"--o---o---o---o-", "o-.-o-.-o-.-o-.-", "----------------"},
        {"----------------"},
        {"-------l-------h", "--------------l-", "----------------"},
        {{"gl.additive", "Sub bass", 0.67f}, {"gl.analog", "Velvet Pad", 0.8f}},
        {"R_______________", "R_______R_______"},
        33,
        {{"gl.hexsynth", "Soft Pad", 0.46f}, {"gl.analog", "Velvet Pad", 0.52f}, {"gl.sawswarm", "Wide Pad", 1.22f}},
        {{{1, 4, 1, 4}}, {{1, 6, 4, 4}}, {{4, 1, 6, 5}}, {{1, 3, 4, 1}}},
        true, 5, {"Held"}, 1.0f,
        {{"gl.wavetable", "Glass Bell", 0.58f}, {"gl.plucker", "Long bell", 0.82f}, {"gl.fm", "Bell", 0.5f}},
        LeadMode::Arp, {"x___x___x___x___", "x_______x___x___", "x_x_____x_x_____"}, 72, 0.45f, 0.3f});
    return v;
  }();
  return r;
}

// ------------------------------------------------------------------ helpers ----
PluginState soundState(const char* type, const char* preset) {
  const PluginInfo* info = findPlugin(type);
  if (!info) return defaultPluginState(type);
  if (preset && *preset)
    for (size_t i = 0; i < info->presets.size(); ++i)
      if (info->presets[i].name == preset) return presetState(*info, i);
  return defaultPluginState(type);
}

void setP(PluginState& st, int index, float v) {
  if (index >= 0 && static_cast<size_t>(index) < st.params.size()) st.params[static_cast<size_t>(index)] = v;
}

int velOf(char c) {
  switch (c) {
    case 'x': case 'X': return 118;
    case 'o': case 'O': return 96;
    case '.': return 62;
    default: return 0;
  }
}

uint8_t clampVel(int v) { return static_cast<uint8_t>(std::clamp(v, 1, 127)); }

// Per-part generator seeds (style and part folded in).
uint64_t partSeed(const Settings& s, int part, uint64_t salt) {
  return mixHash(mixHash(mixHash(s.seeds[static_cast<size_t>(part)], static_cast<uint64_t>(s.style) + 1), static_cast<uint64_t>(part) + 11),
                 salt);
}

double moodReverb(Mood m) {
  switch (m) {
    case Mood::Dark: return 1.0;
    case Mood::Bright: return 0.7;
    case Mood::Dreamy: return 1.7;
    default: return 0.9;
  }
}

// ------------------------------------------------------------------- chords ----
struct Harmony {
  chord::ChordProgression prog;  // one chord per bar for the whole pattern
  std::vector<std::vector<int>> classes;  // pitch classes per bar
  std::vector<int> roots;                 // root pitch class per bar
};

Harmony makeHarmony(const Settings& s, const Recipe& r, int bars, Tick bar) {
  Rng rng(partSeed(s, kChords, 0xC0));
  Harmony h;
  chord::ChordProgression& p = h.prog;
  const bool sevenths = r.sevenths || s.mood == Mood::Dreamy;
  std::array<int, 4> degrees = r.progressions[static_cast<size_t>(rng.below(static_cast<int>(r.progressions.size())))];
  if (rng.chance(0.3)) {  // a fresh diatonic progression from the chord engine
    chord::ChordProgression g = chord::generate(s.key, s.scale, 4, bar, 0.0, static_cast<uint32_t>(rng.next() | 1u));
    for (size_t i = 0; i < 4; ++i) {
      const int d = chord::degreeOf(s.key, s.scale, g.chords[i].root);
      if (d > 0) degrees[i] = d;
    }
  }
  p.key = mod12(s.key);
  p.scale = s.scale;
  p.octave = r.chordOctave + (s.mood == Mood::Bright ? 1 : 0);
  p.chordLength = bar;
  for (int b = 0; b < bars; ++b) {
    int deg = degrees[static_cast<size_t>(b % 4)];
    // The second time round an 8-bar loop may answer with a different last chord.
    if (b == 7 && rng.chance(0.4)) deg = degrees[0] == 5 ? 1 : 5;
    chord::Chord c = chord::diatonicChord(p.key, p.scale, deg, sevenths);
    c.voicing = chord::Voicing::Smooth;
    c.length = bar;
    p.chords.push_back(c);
  }
  // Rhythm: one of the style's, busier ones with more energy.
  const int nr = static_cast<int>(r.chordRhythms.size());
  int ri = rng.below(nr);
  if (s.energy < 0.3) ri = 0;
  const char* rname = r.chordRhythms[static_cast<size_t>(ri)];
  if (s.mood == Mood::Dreamy) rname = "Held";
  p.perf.rhythm = 0;
  for (size_t k = 0; k < chord::rhythms().size(); ++k)
    if (std::string(chord::rhythms()[k].name) == rname) p.perf.rhythm = static_cast<int>(k);
  p.perf.cycle = bar;
  p.perf.gate = r.chordGate;
  p.perf.velocity = 92;
  for (size_t i = 0; i < p.chords.size(); ++i) {
    std::vector<int> cl;
    const auto& q = chord::qualities()[static_cast<size_t>(p.chords[i].quality)];
    for (int st : q.steps) cl.push_back(mod12(p.chords[i].root + st));
    h.classes.push_back(cl);
    h.roots.push_back(p.chords[i].root);
  }
  return h;
}

std::vector<Note> chordNotes(const Harmony& h, const Settings& s) {
  std::vector<Note> ns = chord::render(h.prog, 0, true, false);
  Rng rng(partSeed(s, kChords, 0xC1));
  for (auto& n : ns) n.velocity = std::clamp(n.velocity + rng.jitter(5), 1, 127);
  return ns;
}

// --------------------------------------------------------------------- drums ----
struct Drums {
  std::vector<uint8_t> kick, snare, hat, ohat;  // step rows (16 per bar)
  std::vector<Note> hatNotes, perc;             // note parts
  std::vector<uint8_t> hatL, ohatL;             // the light pattern (arrangement)
  std::vector<Note> hatNotesL, percL;
};

int percPitch(char c) {
  switch (c) {
    case 'r': case 'R': return 67;  // rim
    case 'l': case 'L': return 65;  // low tom
    case 'h': case 'H': return 66;  // high tom
    case 's': case 'S': return 63;  // shaker (soft hat pad)
    case 'c': case 'C': return 62;  // clap
    default: return -1;
  }
}

// Swing for note parts, the same shift the engine gives swung steps.
Tick swingShift(const Song& song, int step, Tick stepTicks) {
  return step % 2 == 1 ? static_cast<Tick>(song.swing * static_cast<float>(stepTicks) * 0.5f) : 0;
}

Drums makeDrums(const Settings& s, const Recipe& r, const Song& song, int bars) {
  Rng rng(partSeed(s, kDrums, 0xD0));
  const Tick step = song.barTicks() / 16;
  const double e = std::clamp(s.energy, 0.0, 1.0);
  auto pick = [&](const std::vector<const char*>& v) { return v[static_cast<size_t>(rng.below(static_cast<int>(v.size())))]; };
  const char* kT = pick(r.kickT);
  const char* sT = pick(r.snareT);
  const char* hT = pick(r.hatT);
  const char* oT = pick(r.ohatT);
  const char* pT = pick(r.percT);
  const double velScale = 0.82 + 0.18 * e;
  Drums d;
  const size_t n = static_cast<size_t>(16 * bars);
  d.kick.assign(n, 0), d.snare.assign(n, 0), d.hat.assign(n, 0), d.ohat.assign(n, 0);
  for (int b = 0; b < bars; ++b) {
    const bool fillBar = b % 4 == 3 && e >= 0.3 && rng.chance(0.35 + 0.5 * e);
    const int fillType = rng.below(3);
    for (int k = 0; k < 16; ++k) {
      const size_t i = static_cast<size_t>(b * 16 + k);
      auto vel = [&](char c) { return velOf(c) ? clampVel(static_cast<int>(velOf(c) * velScale) + rng.jitter(6)) : uint8_t(0); };
      d.kick[i] = vel(kT[k]);
      d.snare[i] = vel(sT[k]);
      char hc = hT[k];
      if (hc == '.' && e < 0.3) hc = '-';
      if (hc == '-' && e > 0.75 && rng.chance(0.22)) hc = '.';
      d.hat[i] = vel(hc);
      d.ohat[i] = e < 0.3 ? 0 : vel(oT[k]);
      if (d.ohat[i]) d.hat[i] = 0;  // the open hat takes the step
      const int pp = percPitch(pT[k]);
      if (pp >= 0 && e >= 0.2) {
        const bool acc = pT[k] >= 'A' && pT[k] <= 'Z';
        const int v = (pp == 63 ? 58 : acc ? 104 : 86) + rng.jitter(8);
        Note nt(static_cast<Tick>(i) * step + swingShift(song, k, step), step, pp, std::clamp(static_cast<int>(v * velScale), 1, 127));
        d.perc.push_back(nt);
      }
    }
    if (fillBar) {
      const size_t b0 = static_cast<size_t>(b * 16);
      if (fillType == 0) {  // snare run into the next bar
        for (int k = 12; k < 16; ++k) d.snare[b0 + static_cast<size_t>(k)] = clampVel(60 + (k - 12) * 16 + rng.jitter(4));
      } else if (fillType == 1) {  // tom fill on the perc pads
        static const int st[] = {10, 12, 13, 14, 15};
        static const int pi[] = {66, 66, 65, 66, 65};
        d.perc.erase(std::remove_if(d.perc.begin(), d.perc.end(), [&](const Note& x) { return x.start >= static_cast<Tick>(b0 + 10) * step && x.start < static_cast<Tick>(b0 + 16) * step; }),
                     d.perc.end());
        for (int k = 0; k < 5; ++k)
          d.perc.push_back(Note(static_cast<Tick>(b0 + static_cast<size_t>(st[k])) * step, step, pi[k], 90 + k * 6));
      } else {  // the kick drops out, the snare answers
        for (int k = 12; k < 16; ++k) d.kick[b0 + static_cast<size_t>(k)] = 0;
        d.snare[b0 + 14] = clampVel(84 + rng.jitter(6));
        d.snare[b0 + 15] = clampVel(104 + rng.jitter(6));
      }
    }
  }
  // Trap: hats as notes, with rolls.
  if (r.hatNotes) {
    for (size_t i = 0; i < n; ++i) {
      if (!d.hat[i]) continue;
      const Tick at = static_cast<Tick>(i) * step;
      const bool roll = i % 4 == 2 && rng.chance(0.12 + 0.4 * e);
      if (!roll) {
        d.hatNotes.push_back(Note(at, step, 60, d.hat[i]));
        continue;
      }
      const int kind = rng.below(3);  // 0 sixteenth triplets, 1 thirty-seconds, 2 sixteenths
      const int count = kind == 0 ? 3 : kind == 1 ? 4 : 2;
      const Tick span = 2 * step;
      for (int k = 0; k < count; ++k)
        d.hatNotes.push_back(Note(at + span * k / count, span / count, 60, std::clamp(66 + k * 12 + rng.jitter(4), 1, 127)));
    }
    std::fill(d.hat.begin(), d.hat.end(), 0);
  }
  // The light pattern: hats and perc only.
  d.hatL = d.hat, d.ohatL = d.ohat, d.hatNotesL = d.hatNotes, d.percL = d.perc;
  std::sort(d.perc.begin(), d.perc.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  std::sort(d.percL.begin(), d.percL.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return d;
}

// ---------------------------------------------------------------------- bass ----
int bassPitch(int pc, int lo) { return lo + mod12(pc - lo); }

std::vector<Note> makeBass(const Settings& s, const Recipe& r, const Harmony& h, const Song& song, int bars) {
  Rng rng(partSeed(s, kBass, 0xB0));
  const Tick step = song.barTicks() / 16;
  const double e = std::clamp(s.energy, 0.0, 1.0);
  int ti = rng.below(static_cast<int>(r.bassT.size()));
  if (e < 0.25) ti = 0;
  const std::string t = r.bassT[static_cast<size_t>(ti)];
  const int lo = r.bassLo + (s.mood == Mood::Dark ? -2 : 0);
  std::vector<Note> out;
  for (int b = 0; b < bars; ++b) {
    const size_t bi = static_cast<size_t>(b);
    const int root = bassPitch(h.roots[bi], lo);
    const auto& cl = h.classes[bi];
    int fifth = cl.size() >= 3 ? root + mod12(cl[2] - h.roots[bi]) : root + 7;
    if (fifth > lo + 16) fifth -= 12;
    const int nextRoot = bassPitch(h.roots[(bi + 1) % h.roots.size()], lo);
    int approach = nextRoot - 1;
    while (approach > nextRoot - 4 && !tools::inScale(approach, s.key, s.scale)) --approach;
    if (!tools::inScale(approach, s.key, s.scale)) approach = nextRoot;
    for (int k = 0; k < 16; ++k) {
      const char c = t[static_cast<size_t>(k)];
      int pitch = -1;
      if (c == 'R') pitch = root;
      else if (c == 'O') pitch = root + 12;
      else if (c == 'F') pitch = fifth;
      else if (c == 'A') pitch = approach;
      if (pitch < 0) continue;
      // Busy lines thin out at low energy (never the first note of the bar).
      if (k > 0 && e < 0.45 && c != 'A' && rng.chance(0.25 * (0.45 - e) / 0.45)) continue;
      int len = 1;
      while (k + len < 16 && t[static_cast<size_t>(k + len)] == '_') ++len;
      const Tick at = static_cast<Tick>(b * 16 + k) * step + swingShift(song, k, step);
      const Tick l = len > 1 ? len * step - step / 6 : step * 3 / 4;
      const int v = (k == 0 ? 112 : 98) + rng.jitter(6);
      out.push_back(Note(at, l, pitch, std::clamp(static_cast<int>(v * (0.85 + 0.15 * e)), 1, 127)));
    }
  }
  return out;
}

// ---------------------------------------------------------------------- lead ----
std::vector<int> scalePitches(const Settings& s, int lo, int hi) {
  std::vector<int> v;
  for (int p = lo; p <= hi; ++p)
    if (tools::inScale(p, s.key, s.scale)) v.push_back(p);
  return v;
}

bool inClasses(int pitch, const std::vector<int>& cl) { return std::find(cl.begin(), cl.end(), mod12(pitch)) != cl.end(); }

int nearestChordTone(int pitch, const std::vector<int>& cl, int lo, int hi) {
  for (int d = 0; d < 12; ++d) {
    if (pitch - d >= lo && inClasses(pitch - d, cl)) return pitch - d;
    if (pitch + d <= hi && inClasses(pitch + d, cl)) return pitch + d;
  }
  return pitch;
}

std::vector<Note> makeLead(const Settings& s, const Recipe& r, const Harmony& h, const Song& song, int bars) {
  Rng rng(partSeed(s, kLead, 0xA0));
  const Tick step = song.barTicks() / 16;
  const double e = std::clamp(s.energy, 0.0, 1.0);
  const int lo = r.leadLo + (s.mood == Mood::Bright ? 12 : 0) - (s.mood == Mood::Dark ? 2 : 0);
  const int hi = lo + 19;
  const std::vector<int> sc = scalePitches(s, lo, hi);
  const std::string rhythm = r.leadT[static_cast<size_t>(rng.below(static_cast<int>(r.leadT.size())))];
  std::vector<int> onsets;
  for (int k = 0; k < 16; ++k)
    if (rhythm[static_cast<size_t>(k)] == 'x') onsets.push_back(k);
  auto lengthAt = [&](int k) {
    int len = 1;
    while (k + len < 16 && rhythm[static_cast<size_t>(k + len)] == '_') ++len;
    return len;
  };
  std::vector<Note> out;
  if (r.leadMode == LeadMode::Arp) {
    const int order = rng.below(4);  // up, down, up-down, skip
    const int span = rng.chance(0.5) ? 12 : 16;
    for (int b = 0; b < bars; ++b) {
      std::vector<int> tones;
      for (int p = lo; p <= lo + span; ++p)
        if (inClasses(p, h.classes[static_cast<size_t>(b)])) tones.push_back(p);
      if (tones.empty()) continue;
      std::vector<int> seq = tones;
      if (order == 1) std::reverse(seq.begin(), seq.end());
      if (order == 2)
        for (int i = static_cast<int>(tones.size()) - 2; i > 0; --i) seq.push_back(tones[static_cast<size_t>(i)]);
      if (order == 3) {
        seq.clear();
        for (size_t i = 0; i < tones.size(); i += 2) seq.push_back(tones[i]);
        for (size_t i = 1; i < tones.size(); i += 2) seq.push_back(tones[i]);
      }
      int idx = 0;
      for (int k : onsets) {
        if (k > 0 && e < 0.4 && rng.chance(0.3 * (0.4 - e) / 0.4)) continue;
        const int pitch = seq[static_cast<size_t>(idx++ % static_cast<int>(seq.size()))];
        const int len = lengthAt(k);
        const Tick at = static_cast<Tick>(b * 16 + k) * step + swingShift(song, k, step);
        out.push_back(Note(at, std::max<Tick>(step / 2, len * step - step / 4), pitch, std::clamp(90 + (k % 4 == 0 ? 14 : 0) + rng.jitter(6), 1, 127)));
      }
    }
  } else {
    // A one-bar motif as scale steps, repeated on each chord (A A A' B).
    std::vector<int> moves;
    for (size_t i = 0; i < onsets.size(); ++i) moves.push_back(i == 0 ? 0 : rng.jitter(2));
    std::vector<int> movesB = moves;
    for (size_t i = movesB.size() / 2; i < movesB.size(); ++i) movesB[i] = rng.jitter(2);
    int anchor = sc[sc.size() / 2];
    for (int b = 0; b < bars; ++b) {
      const auto& cl = h.classes[static_cast<size_t>(b)];
      anchor = nearestChordTone(anchor, cl, lo, hi);
      const bool phraseEnd = b % 4 == 3;
      const auto& mv = phraseEnd ? movesB : moves;
      auto it = std::find(sc.begin(), sc.end(), anchor);
      int si = it == sc.end() ? static_cast<int>(sc.size() / 2) : static_cast<int>(it - sc.begin());
      for (size_t i = 0; i < onsets.size(); ++i) {
        const int k = onsets[i];
        si = std::clamp(si + mv[i], 0, static_cast<int>(sc.size()) - 1);
        int pitch = sc[static_cast<size_t>(si)];
        if (k % 4 == 0) pitch = nearestChordTone(pitch, cl, lo, hi);  // strong beats land on the chord
        const bool last = phraseEnd && i + 1 == onsets.size();
        if (last) pitch = nearestChordTone(pitch, {cl[0]}, lo, hi);
        if (i > 0 && !last && e < 0.45 && rng.chance(0.35 * (0.45 - e) / 0.45)) continue;
        int len = lengthAt(k);
        if (last) len = std::max(len, 16 - k);
        const Tick at = static_cast<Tick>(b * 16 + k) * step + swingShift(song, k, step);
        out.push_back(Note(at, std::max<Tick>(step / 2, len * step - step / 6), pitch, std::clamp(96 + (k % 4 == 0 ? 10 : 0) + rng.jitter(7), 1, 127)));
        auto f = std::find(sc.begin(), sc.end(), pitch);
        if (f != sc.end()) si = static_cast<int>(f - sc.begin());
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return out;
}

// --------------------------------------------------------------------- build ----
struct Built {
  ObjectId kick, snare, hat, ohat, perc, bass, chords, lead;
};

ObjectId addChannel(Song& s, const std::string& name, const Snd& snd, Rgb color, int insert) {
  Channel c;
  c.id = s.allocateId();
  c.name = name;
  c.color = color;
  c.plugin = soundState(snd.type, snd.preset);
  c.gain = snd.gain;
  c.mixerTrack = s.mixer[static_cast<size_t>(insert)].id;
  MixerTrack& m = s.mixer[static_cast<size_t>(insert)];
  m.name = name;
  m.color = color;
  s.channels.push_back(c);
  return c.id;
}

void setSlot(MixerTrack& m, int slot, PluginState st, float mix = 1.0f) {
  m.slots[static_cast<size_t>(slot)] = EffectSlot{std::move(st), true, mix};
}

ObjectId addPattern(Song& s, const std::string& name, int bars, Rgb color) {
  Pattern p;
  p.id = s.allocateId();
  p.name = name;
  p.bars = bars;
  p.stepsPerBar = 16;
  p.color = color;
  s.patterns.push_back(p);
  return p.id;
}

void addClip(Song& s, ObjectId pattern, int track, Tick start) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pattern;
  c.track = track;
  c.start = start;
  s.playlist.push_back(c);
}

constexpr Rgb kColDrums = 0xD8775A, kColBass = 0xA56CD8, kColChords = 0x7A6FE0, kColLead = 0x4FB8B0;

}  // namespace

// ------------------------------------------------------------------- public ----

const char* partName(int part) {
  static const char* const n[] = {"Drums", "Bass", "Chords", "Lead"};
  return part >= 0 && part < kPartCount ? n[part] : "";
}

const std::vector<StyleInfo>& styles() {
  static const std::vector<StyleInfo> v = {
      {"House", "Four on the floor, off-beat hats, stabbed chords and a plucked arpeggio.", 118, 128, 124, 9, 2},
      {"Techno", "Driving kick, rolling bass, sparse stabs and a hypnotic sequence.", 124, 140, 130, 9, 6},
      {"Hip hop", "Swung boom-bap drums, round bass, soft keys and a short motif.", 82, 98, 90, 2, 5},
      {"Trap", "Half-time snare, hat rolls, long sub bass and bell melodies.", 130, 160, 140, 1, 2},
      {"Lo-fi", "Lazy swing, dusty drums, seventh-chord keys and a gentle pluck.", 70, 90, 80, 5, 1},
      {"Drum and bass", "Two-step breaks at full speed under a saw bass and wide pads.", 168, 178, 174, 7, 2},
      {"Pop", "Straight beat, pulsing piano chords, eighth-note bass and a hook.", 96, 124, 110, 0, 1},
      {"Ambient", "Slow pads, drone bass, soft percussion and a floating bell arp.", 60, 90, 72, 4, 7},
  };
  return v;
}

const char* moodName(Mood m) {
  static const char* const n[] = {"Dark", "Warm", "Bright", "Dreamy"};
  const int i = static_cast<int>(m);
  return i >= 0 && i < static_cast<int>(Mood::Count) ? n[i] : "";
}

const char* formName(Form f) {
  static const char* const n[] = {"4-bar loop", "8-bar loop", "Short arrangement"};
  const int i = static_cast<int>(f);
  return i >= 0 && i < static_cast<int>(Form::Count) ? n[i] : "";
}

const std::vector<int>& scaleChoices() {
  // Major, natural minor, harmonic minor, dorian, phrygian, lydian, mixolydian.
  static const std::vector<int> v = {1, 2, 3, 5, 6, 7, 8};
  return v;
}

Settings styleDefaults(int style, uint32_t seed) {
  Settings s;
  s.style = std::clamp(style, 0, static_cast<int>(styles().size()) - 1);
  const StyleInfo& st = styles()[static_cast<size_t>(s.style)];
  s.tempo = st.tempoDefault;
  s.key = st.defaultKey;
  s.scale = st.defaultScale;
  for (int p = 0; p < kPartCount; ++p) s.seeds[static_cast<size_t>(p)] = static_cast<uint32_t>(mixHash(seed, static_cast<uint64_t>(p) + 1) & 0x7FFFFFFF) | 1u;
  return s;
}

double effectiveTempo(const Settings& s) {
  if (s.tempo > 0) return std::clamp(s.tempo, 10.0, 522.0);
  const int i = std::clamp(s.style, 0, static_cast<int>(styles().size()) - 1);
  return styles()[static_cast<size_t>(i)].tempoDefault;
}

void reroll(Settings& s, int part, uint32_t entropy) {
  for (int p = 0; p < kPartCount; ++p) {
    if ((part >= 0 && p != part) || s.locked[static_cast<size_t>(p)]) continue;
    uint32_t& sd = s.seeds[static_cast<size_t>(p)];
    sd = static_cast<uint32_t>(mixHash(mixHash(sd, entropy), static_cast<uint64_t>(p) + 101) & 0x7FFFFFFF) | 1u;
  }
}

std::vector<std::string> partChannels(int part) {
  switch (part) {
    case kDrums: return {"Kick", "Snare", "Clap", "Hat", "Open hat", "Perc"};
    case kBass: return {"Bass"};
    case kChords: return {"Chords"};
    case kLead: return {"Lead"};
    default: return {};
  }
}

std::vector<Sound> soundOptions(int style, int part) {
  const Recipe& r = recipes()[static_cast<size_t>(std::clamp(style, 0, static_cast<int>(recipes().size()) - 1))];
  std::vector<Sound> out;
  auto add = [&out](const Snd& x) { out.push_back(Sound{x.type, x.preset, x.gain}); };
  if (part == kDrums) {
    for (const Snd* x : {&r.kick, &r.snare, &r.hat, &r.ohat, &r.perc}) add(*x);
  } else {
    const std::vector<Snd>* src = part == kBass ? &r.bass : part == kChords ? &r.chords : part == kLead ? &r.lead : nullptr;
    if (src)
      for (const Snd& x : *src) add(x);
  }
  return out;
}

std::vector<Sound> chosenSounds(const Settings& in, int part) {
  Settings s = in;
  s.style = std::clamp(s.style, 0, static_cast<int>(recipes().size()) - 1);
  std::vector<Sound> all = soundOptions(s.style, part);
  if (part == kDrums || all.empty()) return all;
  Rng rng(partSeed(s, part, 0x50));
  return {all[static_cast<size_t>(rng.below(static_cast<int>(all.size())))]};
}

std::string describe(const Settings& s) {
  const int si = std::clamp(s.style, 0, static_cast<int>(styles().size()) - 1);
  const auto& sc = tools::scales();
  const int scale = std::clamp(s.scale, 0, static_cast<int>(sc.size()) - 1);
  char b[256];
  std::snprintf(b, sizeof b, "%s · %.0f BPM · %s %s · %s", styles()[static_cast<size_t>(si)].name, effectiveTempo(s),
                chord::noteName(mod12(s.key)).c_str(), sc[static_cast<size_t>(scale)].name, formName(s.form));
  return b;
}

Song generate(const Settings& in) {
  Settings s = in;
  s.style = std::clamp(s.style, 0, static_cast<int>(styles().size()) - 1);
  s.key = mod12(s.key);
  if (std::find(scaleChoices().begin(), scaleChoices().end(), s.scale) == scaleChoices().end()) s.scale = 2;
  s.energy = std::clamp(s.energy, 0.0, 1.0);
  if (static_cast<int>(s.mood) < 0 || s.mood >= Mood::Count) s.mood = Mood::Warm;
  if (static_cast<int>(s.form) < 0 || s.form >= Form::Count) s.form = Form::Loop4;
  const Recipe& r = recipes()[static_cast<size_t>(s.style)];
  const StyleInfo& st = styles()[static_cast<size_t>(s.style)];

  Song song = makeEmptySong();
  song.tempo = effectiveTempo(s);
  song.swing = r.swing;
  song.info.title = std::string(st.name) + " loop";
  song.info.genre = st.name;
  song.info.comments = "Made with the loop starter: " + describe(s);
  ensureMixer(song);
  const int bars = s.form == Form::Loop8 ? 8 : 4;
  const Tick bar = song.barTicks();

  // Sounds: the drum kit is the style's; bass, chords and lead are picked by their part's seed.
  const Sound bassS = chosenSounds(s, kBass)[0], chordS = chosenSounds(s, kChords)[0], leadS = chosenSounds(s, kLead)[0];
  const Snd bassSnd{bassS.type.c_str(), bassS.preset.c_str(), bassS.gain};
  const Snd chordSnd{chordS.type.c_str(), chordS.preset.c_str(), chordS.gain};
  const Snd leadSnd{leadS.type.c_str(), leadS.preset.c_str(), leadS.gain};

  Built c{};
  c.kick = addChannel(song, "Kick", r.kick, kColDrums, 1);
  c.snare = addChannel(song, r.clap ? "Clap" : "Snare", r.snare, 0xC9A43C, 2);
  c.hat = addChannel(song, "Hat", r.hat, 0x4FB8B0, 3);
  c.ohat = addChannel(song, "Open hat", r.ohat, 0x4FB8B0, 3);
  c.perc = addChannel(song, "Perc", r.perc, 0xC98A3C, 4);
  c.bass = addChannel(song, "Bass", bassSnd, kColBass, 5);
  c.chords = addChannel(song, "Chords", chordSnd, kColChords, 6);
  c.lead = addChannel(song, "Lead", leadSnd, kColLead, 7);
  song.mixer[3].name = "Hats";
  // Open hat: chokes the closed hat's tail (cut group 1).
  for (auto& ch : song.channels) {
    if (ch.id == c.ohat) ch.cutBy = 1, ch.pan = -0.12f;
    if (ch.id == c.hat) ch.cutGroup = 1, ch.pan = 0.12f;
    if (ch.id == c.perc) ch.pan = -0.2f;
    if (ch.id == c.lead) ch.pan = 0.08f;
  }

  // Mixer: buses, sends, a gentle master chain.
  MixerTrack& rev = song.mixer[8];
  MixerTrack& dly = song.mixer[9];
  rev.name = "Reverb";
  rev.color = 0x5A8FD8;
  dly.name = "Delay";
  dly.color = 0x5A8FD8;
  {
    PluginState rv = soundState("gl.reverb", s.style == 7 || s.mood == Mood::Dreamy ? "Hall" : "Plate");
    setP(rv, pp::RvbDry, 0.f);
    setP(rv, pp::RvbWet, 1.f);
    setP(rv, pp::RvbLowCut, 200.f);
    setSlot(rev, 0, rv);
    PluginState dl = soundState("gl.delay", "Dotted eighth");
    setP(dl, pp::DlyDry, 0.f);
    setP(dl, pp::DlyWet, 1.f);
    setP(dl, pp::DlyFeedback, s.mood == Mood::Dreamy ? 0.55f : 0.4f);
    setSlot(dly, 0, dl);
  }
  const float rvAmt = static_cast<float>(r.reverb * moodReverb(s.mood));
  const float dlAmt = static_cast<float>(r.delay * (s.mood == Mood::Dreamy ? 1.5 : 1.0));
  auto send = [&](int from, int to, float level) {
    if (level > 0.f) song.mixer[static_cast<size_t>(from)].sends.push_back(Send{song.mixer[static_cast<size_t>(to)].id, std::min(level, 1.0f), false, false});
  };
  send(2, 8, rvAmt * 0.6f);   // snare / clap
  send(4, 8, rvAmt * 0.5f);   // perc
  send(6, 8, rvAmt);          // chords
  send(7, 8, rvAmt);          // lead
  send(7, 9, dlAmt);          // lead delay
  send(6, 9, dlAmt * 0.4f);   // chords delay
  // Inserts.
  if (s.mood == Mood::Dark) {
    PluginState f = soundState("gl.filter", "Dark");
    setP(f, pp::FltCutoff, 2400.f);
    setSlot(song.mixer[6], 0, f);
    PluginState f2 = f;
    setP(f2, pp::FltCutoff, 3800.f);
    setSlot(song.mixer[7], 0, f2);
  } else if (s.mood == Mood::Dreamy) {
    setSlot(song.mixer[6], 0, soundState("gl.chorus", "Lush ensemble"), 0.6f);
  }
  if (s.style == 4) {  // lo-fi: crushed keys and drums
    PluginState bc = soundState("gl.bitcrush", "Lo-fi");
    setSlot(song.mixer[6], 1, bc, 0.5f);
    setSlot(song.mixer[2], 0, bc, 0.4f);
  }
  {
    PluginState cmp = soundState("gl.comp", "Gentle glue");
    setP(cmp, pp::CmpMakeup, 0.f);
    setSlot(song.mixer[0], 0, cmp);
    PluginState lim = soundState("gl.limiter", "Safety");
    setP(lim, pp::LimCeiling, -1.0f);
    setSlot(song.mixer[0], 1, lim);
  }

  // Parts.
  const Harmony h = makeHarmony(s, r, bars, bar);
  const Drums d = makeDrums(s, r, song, bars);
  const std::vector<Note> bass = makeBass(s, r, h, song, bars);
  const std::vector<Note> chords = chordNotes(h, s);
  const std::vector<Note> lead = makeLead(s, r, h, song, bars);

  Pattern& pd = song.patterns[0];
  pd.name = "Drums";
  pd.bars = bars;
  pd.color = kColDrums;
  const ObjectId pDrums = pd.id;
  auto fillDrums = [&](Pattern& p, bool light) {
    if (!light) {
      p.steps[c.kick] = d.kick;
      p.steps[c.snare] = d.snare;
    }
    if (!r.hatNotes) p.steps[c.hat] = light ? d.hatL : d.hat;
    else p.notes[c.hat] = light ? d.hatNotesL : d.hatNotes;
    p.steps[c.ohat] = light ? d.ohatL : d.ohat;
    if (!(light ? d.percL : d.perc).empty()) p.notes[c.perc] = light ? d.percL : d.perc;
  };
  fillDrums(song.patterns[0], false);
  const ObjectId pBass = addPattern(song, "Bass", bars, kColBass);
  song.pattern(pBass)->notes[c.bass] = bass;
  const ObjectId pChords = addPattern(song, "Chords", bars, kColChords);
  song.pattern(pChords)->notes[c.chords] = chords;
  const ObjectId pLead = addPattern(song, "Lead", bars, kColLead);
  song.pattern(pLead)->notes[c.lead] = lead;

  // Playlist.
  const char* trackNames[] = {"Drums", "Bass", "Chords", "Lead"};
  const Rgb trackCols[] = {kColDrums, kColBass, kColChords, kColLead};
  for (int t = 0; t < 4; ++t) {
    song.tracks[static_cast<size_t>(t)].name = trackNames[t];
    song.tracks[static_cast<size_t>(t)].color = trackCols[t];
  }
  Marker key;
  key.pos = 0;
  key.name = chord::noteName(s.key) + " " + tools::scales()[static_cast<size_t>(s.scale)].name;
  key.keyRoot = s.key;
  key.keyScale = s.scale;
  song.markers.push_back(key);
  if (s.form != Form::Arrangement) {
    addClip(song, pDrums, 0, 0);
    addClip(song, pBass, 1, 0);
    addClip(song, pChords, 2, 0);
    addClip(song, pLead, 3, 0);
  } else {
    const ObjectId pLight = addPattern(song, "Drums light", bars, kColDrums);
    fillDrums(*song.pattern(pLight), true);
    struct Section {
      const char* name;
      int blocks;  // 4-bar blocks
      int drums;   // 0 none, 1 light, 2 full
      bool bass, chords, lead;
    };
    static const Section sections[] = {{"Intro", 1, 1, false, true, false}, {"Groove", 2, 2, true, true, false},
                                       {"Main", 2, 2, true, true, true},    {"Break", 1, 0, false, true, true},
                                       {"Main", 2, 2, true, true, true},    {"Outro", 1, 1, true, true, false}};
    Tick at = 0;
    for (const Section& sec : sections) {
      Marker m;
      m.pos = at;
      m.name = sec.name;
      song.markers.push_back(m);
      for (int k = 0; k < sec.blocks; ++k) {
        if (sec.drums) addClip(song, sec.drums == 2 ? pDrums : pLight, 0, at);
        if (sec.bass) addClip(song, pBass, 1, at);
        if (sec.chords) addClip(song, pChords, 2, at);
        if (sec.lead) addClip(song, pLead, 3, at);
        at += bar * bars;
      }
    }
    // The key marker and the intro share position 0: keep the key marker first.
    std::stable_sort(song.markers.begin(), song.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
  }
  return song;
}

}  // namespace gl::loopstarter
