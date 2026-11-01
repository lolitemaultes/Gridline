// Gridline — J-211: the track icon set (see TrackIconSet.h).
//
// The FL numbers come from FL Studio's shipped content only: its icon font's
// glyph list (one glyph per icon number) and the icon numbers its own name
// presets and templates give to named tracks. Every drawing is Gridline's own.
#include "TrackIconSet.h"
#include <array>
#include "I18n.h"

namespace gl::trackicon {

namespace {
using i18n::N_;
// Picker order: by category. Ids 1..16 are the first set (kept as they were).
const Info kIcons[] = {
    // Drums
    {1, "track-kick", N_("Kick"), Drums, 5, "bass drum"},
    {2, "track-snare", N_("Snare"), Drums, 4, "drum"},
    {3, "track-hats", N_("Hi-hats"), Drums, 239, "hat cymbal"},
    {17, "track-cymbal", N_("Cymbal"), Drums, 6, "crash ride"},
    {18, "track-clap", N_("Clap"), Drums, 132, "hands"},
    {19, "track-snap", N_("Finger snap"), Drums, 131, "click"},
    {20, "track-drum-machine", N_("Drum machine"), Drums, 111, "beat box groove"},
    {21, "track-drum-pads", N_("Drum pads"), Drums, 123, "pads kit"},
    // Percussion
    {4, "track-percussion", N_("Shaker"), Percussion, 129, "percussion maracas"},
    {22, "track-bongos", N_("Hand drums"), Percussion, 7, "bongos congas toms percussion"},
    {23, "track-tambourine", N_("Tambourine"), Percussion, 128, "jingle"},
    {24, "track-cowbell", N_("Cowbell"), Percussion, 121, "bell"},
    {25, "track-triangle", N_("Triangle"), Percussion, 79, "metal"},
    {26, "track-gong", N_("Gong"), Percussion, 80, "tam"},
    {27, "track-chimes", N_("Wind chimes"), Percussion, 89, "bells tubes"},
    {28, "track-whistle", N_("Whistle"), Percussion, 112, "referee"},
    // Keys & mallets
    {6, "track-keys", N_("Keys"), Keys, 237, "keyboard"},
    {29, "track-piano", N_("Grand piano"), Keys, 12, "piano chords"},
    {30, "track-organ", N_("Organ"), Keys, 9, "pipes"},
    {31, "track-bell", N_("Bell"), Keys, 78, "chime"},
    {32, "track-glockenspiel", N_("Mallets"), Keys, 81, "glockenspiel xylophone vibraphone marimba"},
    // Synths & bass
    {7, "track-synth", N_("Synth"), Synths, 16, "saw lead"},
    {33, "track-pulse", N_("Square synth"), Synths, 17, "pulse lead"},
    {8, "track-pad", N_("Pad"), Synths, 86, "sustain ambient"},
    {34, "track-pluck", N_("Pluck"), Synths, 87, "stab"},
    {35, "track-arp", N_("Arpeggio"), Synths, 109, "arp sequence"},
    {5, "track-bass", N_("Bass"), Synths, 14, "sub low"},
    {36, "track-additive", N_("Additive"), Synths, 83, "harmonics"},
    {37, "track-subtractive", N_("Subtractive"), Synths, 84, "analogue filter"},
    {38, "track-fm", N_("FM"), Synths, 85, "frequency modulation operator"},
    {39, "track-wobble", N_("Wobble"), Synths, 122, "lfo dubstep growl"},
    {40, "track-prism", N_("Resynthesis"), Synths, 127, "spectral prism"},
    // Guitars & strings
    {9, "track-guitar", N_("Electric guitar"), Strings, 2, "guitar"},
    {41, "track-acoustic", N_("Acoustic guitar"), Strings, 1, "guitar"},
    {42, "track-banjo", N_("Banjo"), Strings, 90, "folk"},
    {43, "track-violin", N_("Violin"), Strings, 3, "strings orchestra cello viola"},
    {44, "track-harp", N_("Harp"), Strings, 11, "strings"},
    // Brass & wind
    {45, "track-trumpet", N_("Trumpet"), Wind, 8, "brass horn"},
    {46, "track-sax", N_("Saxophone"), Wind, 120, "sax brass"},
    {47, "track-flute", N_("Flute"), Wind, 91, "woodwind"},
    {48, "track-pan-pipes", N_("Pan pipes"), Wind, 10, "flute"},
    // Vocals
    {10, "track-vocal", N_("Microphone"), Vocals, 30, "vocal mic recording"},
    {49, "track-voice", N_("Voice"), Vocals, 20, "vocal speech singer"},
    {50, "track-voice-male", N_("Voice (low)"), Vocals, 21, "vocal male"},
    {51, "track-voice-female", N_("Voice (high)"), Vocals, 22, "vocal female"},
    {52, "track-vocoder", N_("Vocoder"), Vocals, 23, "robot talkbox"},
    // Samples & audio
    {53, "track-sample", N_("Sample"), Samples, 18, "one-shot hit"},
    {54, "track-sliced-loop", N_("Sliced loop"), Samples, 19, "chops break"},
    {55, "track-audio", N_("Audio"), Samples, 75, "waveform recording"},
    {56, "track-nature", N_("Nature"), Samples, 24, "foley field leaf"},
    {57, "track-film", N_("Film"), Samples, 25, "score movie cinematic"},
    {58, "track-vinyl", N_("Vinyl"), Samples, 88, "record lp"},
    {59, "track-disc", N_("Disc"), Samples, 32, "cd"},
    {60, "track-library", N_("Library"), Samples, 130, "books"},
    // Notation & patterns
    {61, "track-treble-clef", N_("Treble clef"), Notation, 13, "lead melody"},
    {62, "track-tuning-fork", N_("Tuning fork"), Notation, 15, "pitch tune"},
    {63, "track-score", N_("Score"), Notation, 70, "notes sheet hook"},
    {64, "track-melody", N_("Melody"), Notation, 71, "note pattern"},
    {65, "track-steps", N_("Steps"), Notation, 72, "sequencer"},
    {66, "track-automation", N_("Automation"), Notation, 73, "envelope curve"},
    {67, "track-control-link", N_("Control link"), Notation, 74, "automation link"},
    {68, "track-clock", N_("Clock"), Notation, 99, "time tempo"},
    {69, "track-midi", N_("MIDI"), Notation, 96, "connector din"},
    // Effects
    {11, "track-effects", N_("Effects"), Effects, 26, "fx spark"},
    {70, "track-plugin", N_("Plugin"), Effects, 27, "plug"},
    {12, "track-reverb", N_("Reverb"), Effects, 119, "space room"},
    {13, "track-delay", N_("Delay"), Effects, 0, "echo"},
    {71, "track-compressor", N_("Compressor"), Effects, 82, "dynamics squash"},
    {72, "track-eq", N_("Equaliser"), Effects, 117, "eq sliders"},
    {73, "track-low-pass", N_("Low pass"), Effects, 114, "filter"},
    {74, "track-high-pass", N_("High pass"), Effects, 115, "filter riser"},
    // Mixing & routing
    {14, "track-bus", N_("Bus"), Mixing, 40, "group submix"},
    {75, "track-master", N_("Master"), Mixing, 116, "crown"},
    {76, "track-input", N_("Input"), Mixing, 33, "in"},
    {16, "track-output", N_("Output"), Mixing, 34, "out speaker"},
    {77, "track-send", N_("Send"), Mixing, 118, "aux return"},
    {15, "track-sidechain", N_("Sidechain"), Mixing, 0, "key duck"},
    {78, "track-routing", N_("Routing"), Mixing, 39, "route"},
    {79, "track-split-two", N_("Split in two"), Mixing, 68, "branch"},
    {80, "track-split-three", N_("Split in three"), Mixing, 69, "branch bus"},
    {81, "track-link", N_("Link"), Mixing, 93, "chain sidechain"},
    {82, "track-jack", N_("Jack plug"), Mixing, 31, "cable"},
    {83, "track-front-layer", N_("Front layer"), Mixing, 76, "foreground"},
    {84, "track-back-layer", N_("Back layer"), Mixing, 77, "background"},
    // Surround & channels
    {85, "track-mono", N_("Mono"), Surround, 55, "mid"},
    {86, "track-stereo", N_("Stereo"), Surround, 56, "side wide"},
    {87, "track-left", N_("Left"), Surround, 57, "channel"},
    {88, "track-right", N_("Right"), Surround, 58, "channel"},
    {89, "track-quad-front", N_("Quad front"), Surround, 53, "surround"},
    {90, "track-quad-rear", N_("Quad rear"), Surround, 54, "surround"},
    {91, "track-surround-51-front", N_("5.1 front"), Surround, 46, "surround"},
    {92, "track-surround-51-centre", N_("5.1 centre"), Surround, 48, "surround center"},
    {93, "track-surround-51-rear", N_("5.1 rear"), Surround, 47, "surround"},
    {94, "track-surround-71-front", N_("7.1 front"), Surround, 49, "surround"},
    {95, "track-surround-71-centre", N_("7.1 centre"), Surround, 52, "surround center"},
    {96, "track-surround-71-side", N_("7.1 side"), Surround, 50, "surround"},
    {97, "track-surround-71-rear", N_("7.1 rear"), Surround, 51, "surround"},
    // Gear & control
    {98, "track-headphones", N_("Headphones"), Gear, 28, "monitor cue"},
    {99, "track-speaker", N_("Speaker"), Gear, 29, "monitor cabinet"},
    {100, "track-knob", N_("Knob"), Gear, 36, "control"},
    {101, "track-joystick", N_("Joystick"), Gear, 35, "controller"},
    {102, "track-touchscreen", N_("Touchscreen"), Gear, 124, "tablet"},
    {103, "track-battery", N_("Battery"), Gear, 105, "power"},
    {104, "track-gear", N_("Gear"), Gear, 104, "settings cog"},
    {105, "track-wrench", N_("Spanner"), Gear, 95, "wrench tool fix"},
    // Symbols
    {106, "track-info", N_("Info"), Symbols, 37, "about"},
    {107, "track-question", N_("Question"), Symbols, 41, "help"},
    {108, "track-warning", N_("Warning"), Symbols, 42, "exclamation alert"},
    {109, "track-eye", N_("Eye"), Symbols, 38, "visual look"},
    {110, "track-padlock", N_("Padlock"), Symbols, 92, "lock"},
    {111, "track-forbidden", N_("Forbidden"), Symbols, 101, "no stop"},
    {112, "track-pencil", N_("Pencil"), Symbols, 100, "edit write"},
    {113, "track-magnifier", N_("Magnifier"), Symbols, 102, "search zoom"},
    {114, "track-scissors", N_("Scissors"), Symbols, 103, "cut"},
    {115, "track-magnet", N_("Magnet"), Symbols, 107, "snap"},
    {116, "track-folder", N_("Folder"), Symbols, 110, "files"},
    {117, "track-project", N_("Pages"), Symbols, 113, "project documents"},
    {118, "track-package", N_("Box"), Symbols, 106, "package"},
    {119, "track-bandage", N_("Plaster"), Symbols, 94, "patch fix bandage"},
    {120, "track-brick", N_("Brick"), Symbols, 125, "building block"},
    {121, "track-cloud", N_("Cloud"), Symbols, 133, "online"},
    {122, "track-snowflake", N_("Snowflake"), Symbols, 134, "frozen freeze"},
    {123, "track-flame", N_("Flame"), Symbols, 126, "hot fire"},
    // Numbers
    {124, "track-digit-1", N_("One"), Numbers, 59, "1"},
    {125, "track-digit-2", N_("Two"), Numbers, 60, "2"},
    {126, "track-digit-3", N_("Three"), Numbers, 61, "3"},
    {127, "track-digit-4", N_("Four"), Numbers, 62, "4"},
    {128, "track-digit-5", N_("Five"), Numbers, 63, "5"},
    {129, "track-digit-6", N_("Six"), Numbers, 64, "6"},
    {130, "track-digit-7", N_("Seven"), Numbers, 65, "7"},
    {131, "track-digit-8", N_("Eight"), Numbers, 66, "8"},
    {132, "track-digit-9", N_("Nine"), Numbers, 67, "9"},
    {133, "track-digit-0", N_("Zero"), Numbers, 136, "0"},
    // Faces & fun
    {134, "track-happy", N_("Happy"), Faces, 97, "smile"},
    {135, "track-sad", N_("Sad"), Faces, 98, "frown"},
    {136, "track-goofy", N_("Goofy"), Faces, 108, "silly ugly"},
    {137, "track-thumbs-up", N_("Thumbs up"), Faces, 43, "good like"},
    {138, "track-thumbs-down", N_("Thumbs down"), Faces, 44, "bad dislike"},
    {139, "track-pixel-hero", N_("Pixel figure"), Faces, 45, "game retro"},
};
constexpr int kCount = static_cast<int>(sizeof(kIcons) / sizeof(kIcons[0]));
constexpr int kMaxId = 139;

// Icons with no FL number of their own export as the closest FL meaning.
int ownFl(const Info& i) {
  if (i.fl) return i.fl;
  if (i.id == 13) return 18;  // delay: FL's own name presets give delay slots this number
  if (i.id == 15) return 93;  // sidechain: FL templates mark sidechain tracks with the link
  return 0;
}

struct Index {
  std::array<const Info*, kMaxId + 1> byId{};
  std::array<int, 256> byFl{};
  Index() {
    for (const Info& i : kIcons) {
      byId[static_cast<size_t>(i.id)] = &i;
      if (i.fl > 0 && i.fl < 256) byFl[static_cast<size_t>(i.fl)] = i.id;
    }
  }
};
const Index& index() {
  static const Index x;
  return x;
}
}  // namespace

const Info* all(int* count) {
  if (count) *count = kCount;
  return kIcons;
}

const Info* info(int id) { return id >= 1 && id <= kMaxId ? index().byId[static_cast<size_t>(id)] : nullptr; }

int maxId() { return kMaxId; }

const char* categoryName(int category) {
  static const char* const kNames[] = {N_("Drums"),         N_("Percussion"),  N_("Keys & mallets"), N_("Synths & bass"),
                                       N_("Guitars & strings"), N_("Brass & wind"), N_("Vocals"),        N_("Samples & audio"),
                                       N_("Notes & patterns"), N_("Effects"),      N_("Mixing & routing"), N_("Surround"),
                                       N_("Gear & control"),  N_("Symbols"),      N_("Numbers"),       N_("Faces")};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == CategoryCount, "one name per category");
  return category >= 0 && category < CategoryCount ? kNames[category] : "";
}

int fromFl(int flNumber) { return flNumber > 0 && flNumber < 256 ? index().byFl[static_cast<size_t>(flNumber)] : 0; }

int toFl(int id) {
  const Info* i = info(id);
  return i ? ownFl(*i) : 0;
}

int exportFl(int icon, int flIcon) {
  if (icon <= 0 || !info(icon)) return flIcon;  // no Gridline icon: the FL number as it was (editing clears both)
  return fromFl(flIcon) == icon ? flIcon : toFl(icon);
}

}  // namespace gl::trackicon
