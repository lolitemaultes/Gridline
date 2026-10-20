// Gridline — chord tools (J-31): the one chord vocabulary shared by the piano
// roll chord stamps (tools::chords), chord names (ops::chordName), the Tools
// menu progressions (ops::progressionNotes) and the chord progression editor.
//
// A progression is a list of chords, each with its own root, quality, length,
// inversion, voicing, bass and extension settings, rendered to notes through
// a performance (rhythm, strum, gate). Everything is pure data in, notes out,
// so the editor window, the menus and the tests share it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Types.h"

namespace gl::chord {

// ------------------------------------------------------------- qualities ----
struct Quality {
  const char* name;          // "Minor 7th"
  const char* symbol;        // "m7" (as written after the root: "Am7")
  std::vector<int> steps;    // semitones above the root, ascending
};
// Index 0..20 are the piano roll's chord stamps (same order as before J-31).
const std::vector<Quality>& qualities();
int findQuality(const std::string& symbol);  // -1 = unknown
// Quality whose pitch classes (relative to the root) are exactly `pcs`; -1 = none.
int qualityOfClasses(const std::vector<int>& pcs);
std::string noteName(int pitchClass, bool flats = false);  // "C", "C#" / "Db"

// ---------------------------------------------------------- scale degrees ----
// Semitone offset of a 0-based scale degree (degree 7 = the octave above).
// Scales with fewer than seven notes fall back to the major / minor scale.
int degreeOffset(int scale, int degree);
// The diatonic chord on 1-based `degree` of the key: stacked thirds of the
// scale (triad, or seventh chord when `sevenths`).
struct Chord;
Chord diatonicChord(int key, int scale, int degree, bool sevenths = false);
// Degree (1..7) of a root in the key, 0 = not in the scale.
int degreeOf(int key, int scale, int rootPc);

// ---------------------------------------------------------------- chords ----
enum class Voicing : uint8_t {
  Close = 0,  // the chord stacked inside one octave
  Open,       // the second note from the bottom one octave up
  Drop2,      // the second note from the top one octave down
  Spread,     // root, fifth, then the rest an octave up
  Smooth,     // the inversion closest to the previous chord (voice leading)
  Count
};
const char* voicingName(Voicing v);

struct Chord {
  int root = 0;          // pitch class 0..11
  int quality = 0;       // qualities() index
  Tick length = 0;       // ticks (0 = ChordProgression::chordLength)
  int inversion = 0;     // > 0: lowest notes up an octave; < 0: highest notes down
  Voicing voicing = Voicing::Close;
  int octave = 0;        // octaves relative to the progression's octave
  int extra = 0;         // -1 = one note left out; 1..3 = extension notes added
  bool main = true;      // the chord notes play
  bool bass = true;      // the bass note plays
  int bassOctave = 0;    // bass: octaves relative to its default (one below the chord)
  int fixedBass = -1;    // bass pitch class (slash chord); -1 = the root
  bool locked = false;   // regenerating leaves it alone
  bool linked = true;    // its notes are replaced when the progression is written again
  int color = 0;         // note colour group of its notes
  bool operator==(const Chord& o) const {
    return root == o.root && quality == o.quality && length == o.length && inversion == o.inversion &&
           voicing == o.voicing && octave == o.octave && extra == o.extra && main == o.main && bass == o.bass &&
           bassOctave == o.bassOctave && fixedBass == o.fixedBass && locked == o.locked && linked == o.linked &&
           color == o.color;
  }
};

// Absolute name ("Am7", "C/E" for a fixed bass) / name relative to the key
// ("vi7", "bVII"); `roman` names are built from the chord's degree and quality.
std::string chordName(const Chord& c, bool flats = false);
std::string romanName(const Chord& c, int key, int scale);
// Parses a chord written as a name ("F#m7", "Bb", "C/G", "Ebmaj7") or a
// numeral of the key ("IV", "vi7", "bVII", "ii°", "V7"). False if unknown.
bool parseChord(const std::string& text, int key, int scale, Chord& out);

// The chord a set of played pitches forms (lowest pitch = bass); false when
// no quality matches. The inversion is set so the pitches' order is kept and
// a bass outside the chord becomes a fixed bass.
bool detect(const std::vector<int>& pitches, Chord& out);

// ------------------------------------------------------------ performance ----
// One hit of a rhythm pattern: when (fraction of the cycle), how long, which notes.
struct Hit {
  enum Part : uint8_t { All = 0, Main, Bass, Arp };  // Arp: one chord note, `index` from the bottom (cycled)
  double at = 0, len = 1;
  Part part = All;
  int index = 0;
  double velocity = 1.0;
};
struct Rhythm {
  const char* name;
  std::vector<Hit> hits;
};
const std::vector<Rhythm>& rhythms();  // index 0 = held chords

struct Performance {
  int rhythm = 0;          // rhythms() index
  Tick cycle = 0;          // length of one rhythm cycle (0 = one bar); restarts on every chord
  Tick strum = 0;          // start offset of the top note of each chord (ticks)
  bool strumDown = false;  // top note first
  double gate = 1.0;       // 0.05..1 note length factor
  int velocity = 96;
  bool operator==(const Performance& o) const {
    return rhythm == o.rhythm && cycle == o.cycle && strum == o.strum && strumDown == o.strumDown && gate == o.gate &&
           velocity == o.velocity;
  }
};

// ------------------------------------------------------------ progression ----
struct ChordProgression {
  int key = 0;               // 0..11
  int scale = 1;             // tools::scales() index (0 = chromatic: treated as major)
  int octave = 5;            // octave of the chord roots (12 * octave + root; C5 = 60)
  Tick chordLength = 384;    // default chord length (ticks)
  std::vector<Chord> chords;
  Performance perf;
  bool operator==(const ChordProgression& o) const {
    return key == o.key && scale == o.scale && octave == o.octave && chordLength == o.chordLength && chords == o.chords &&
           perf == o.perf;
  }
};

Tick chordLength(const ChordProgression& p, size_t i);
Tick chordStart(const ChordProgression& p, size_t i);  // ticks from the progression start
Tick totalLength(const ChordProgression& p);
// Index of the chord sounding at tick `t` (from the progression start); -1 outside.
int chordAt(const ChordProgression& p, Tick t);

// Pitches of chord i: main notes (ascending, voiced) and the bass (-1 = none).
struct Voiced {
  std::vector<int> main;
  int bass = -1;
};
Voiced voice(const ChordProgression& p, size_t i);
// Pitch classes chord i contains (main notes, extensions and the bass).
std::vector<int> chordClasses(const ChordProgression& p, size_t i);

// The progression as notes from `start` (sorted by start, pitch). With
// `withMain` / `withBass` false those notes are left out everywhere.
std::vector<Note> render(const ChordProgression& p, Tick start = 0, bool withMain = true, bool withBass = true);
// Notes of one chord (preview / its part of render()).
std::vector<Note> renderChord(const ChordProgression& p, size_t i, Tick start = 0, bool withMain = true, bool withBass = true);

// What happens to notes already in the pattern under the progression.
enum class NonChordMode : uint8_t {
  ReplaceAll = 0,   // removed
  ReplaceClashing,  // removed when their pitch class is not in the chord above them
  Keep,             // left alone
  Snap,             // moved to the nearest chord tone
  Count
};
const char* nonChordModeName(NonChordMode m);
struct WriteOpts {
  Tick start = 0;
  NonChordMode mode = NonChordMode::ReplaceAll;
  bool withMain = true, withBass = true;
  // Notes the previous write produced: linked chords replace theirs.
  std::vector<Note> previous;
};
// The pattern's notes after writing the progression; `written` receives the
// notes the progression added (for the next write's `previous`).
std::vector<Note> write(const std::vector<Note>& existing, const ChordProgression& p, const WriteOpts& o,
                        std::vector<Note>* written = nullptr);
// Moves notes under the progression to the nearest tone of the chord above them.
int snapNotes(std::vector<Note>& notes, const ChordProgression& p, Tick start);

// -------------------------------------------------------------- editing ----
// Every edit keeps the progression valid (indices clamped, lengths > 0).
void invert(Chord& c, int dir);                 // +1 up, -1 down (limited to the note count)
void transpose(Chord& c, int semitones);        // root moves; whole octaves move `octave`
void setRoot(Chord& c, int pitchClass);         // keeps the octave: C..B inside the same octave
bool swapChords(ChordProgression& p, size_t i, size_t j);
// Inserts a chord leading into chord i + 1 (its dominant seventh), taking the
// second half of chord i (at least one beat is left to each). Returns its index or -1.
int addPassingChord(ChordProgression& p, size_t i, Tick beat);
// Appends / inserts a chord (the key's tonic, or a copy of `like`) at `at` (-1 = end).
size_t addChord(ChordProgression& p, int at = -1);
size_t addChord(ChordProgression& p, const Chord& like, int at);
bool removeChord(ChordProgression& p, size_t i);
// Other chords that fit where chord i is: shared notes with it, diatonic or
// borrowed, sorted by the number of shared notes (most first). Not chord i itself.
std::vector<Chord> alternatives(const ChordProgression& p, size_t i, size_t max = 12);
// New chords for the unlocked chords in `which` (all when empty). `adventure`
// 0..1: 0 keeps to common diatonic moves, 1 also borrows and surprises.
void regenerate(ChordProgression& p, const std::vector<size_t>& which, double adventure, uint32_t seed);
// A fresh progression of `count` chords in the key.
ChordProgression generate(int key, int scale, int count, Tick chordLength, double adventure, uint32_t seed);
// The chords existing notes form: one chord per `chordLength` window from
// `start` (the notes sounding in the window). A window without a chord keeps
// the previous chord (the first: the key's tonic).
ChordProgression analyze(const std::vector<Note>& notes, int key, int scale, Tick start, Tick chordLength, int count);
// Fits the chords' lengths so the whole progression is `total` ticks long (proportions kept).
void rescale(ChordProgression& p, Tick total, Tick grid);

// ------------------------------------------------------------------ text ----
// "C - Am - F - G", "I V vi IV", "Dm7, G7, Cmaj7": chords separated by
// commas, dashes, spaces or bars. Unknown words are skipped (counted in `bad`).
bool parseProgression(const std::string& text, int key, int scale, Tick chordLength, ChordProgression& out, int* bad = nullptr);
std::string progressionText(const ChordProgression& p, bool roman);

struct Preset {
  const char* name;
  const char* text;  // numerals of the key (parseProgression)
  bool minor;        // natural minor key
};
const std::vector<Preset>& presets();

}  // namespace gl::chord
