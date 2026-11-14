// Gridline - "Arrange by sound..." on the Stage (docs/stage-view.md): what each track is, and where
// that kind of part stands. The role comes, in this order, from:
//   1. what plays on the track: the instrument types routed to it (drum machines, slicers, keys,
//      synth presets by category, ...), which works even for a track called "Audio 3";
//   2. how it sounded: a short analysis of the track's own output while the song played (peak, the
//      spectral centre, the low band's share, onsets per second, sustain, stereo width);
//   3. the name, as a hint only (never over 1 or 2).
// A track it is unsure of is "?" and goes to the middle of the stage. Pure functions
// (tests/test_k188_stage.cpp); the analysis runs on the UI thread from the engine's display taps.
#pragma once
#include <string>
#include <vector>
#include "StageMap.h"

namespace gl::stage {

enum class Role : int {
  Unknown = 0, LeadVocal, BackingVocal, Kick, Bass, Snare, Clap, Hats, Perc, Drums, Keys, Guitar, Lead, Pad, Strings, Brass, Fx, Loop,
  Count
};
const char* roleName(Role r);  // English, marked for translation ("bass", "lead vocal", ...)

// The sound analysis: running sums over mono blocks (the engine's post-fader display tap).
struct Features {
  double seconds = 0;      // how much sound (above the floor) was heard
  double peak = 0;         // the loudest sample
  double centroid = 0;     // Hz: the spectral centre (from the slope of the signal)
  double lowShare = 0;     // energy below about 150 Hz, 0 .. 1
  double onsets = 0;       // onsets per second of sound
  double sustain = 0;      // mean level / peak level, 0 short .. 1 held
  double width = 0;        // 0 mono .. 1 wide (the track's stereo width; the display tap is mono)
  bool enough() const { return seconds >= 1.0 && peak > 1e-3; }
};
class FeatureAcc {
 public:
  void reset(double sampleRate);
  void add(const float* mono, int n);
  Features result() const;
  double sampleRate() const { return sr_; }

 private:
  double sr_ = 48000;
  double sumSq_ = 0, sumDiffSq_ = 0, sumLowSq_ = 0, prev_ = 0, low_ = 0;
  double peak_ = 0, envFast_ = 0, envSlow_ = 0, envSum_ = 0, envPeak_ = 0;
  long frames_ = 0, loud_ = 0, onsets_ = 0, since_ = 1L << 30;
  bool armed_ = true;
};

// What decided a role: 1 what plays, 2 how it sounded, 3 the name; 0 unsure.
struct Guess {
  Role role = Role::Unknown;
  int source = 0;
  std::string why;  // English, for the hover line ("low and steady")
};
// 1: from instrument types / preset words routed to the track ("" entries ignored).
Role roleFromPlays(const std::vector<std::string>& pluginTypes, const std::vector<std::string>& presetWords);
// 2: from the sound; Unknown when there was too little or it is unclear. `why` gets the reason.
Role roleFromSound(const Features& f, std::string* why);
// 3: from a name ("vox", "kick", "pad", ...).
Role roleFromName(const std::string& name);
Guess guessRole(const std::vector<std::string>& pluginTypes, const std::vector<std::string>& presetWords, const Features* f,
                const std::string& name);

// Where each role stands: spots for tracks in input (mixer) order; several tracks of one role take the
// next free slot for it. Unknown tracks stand in the middle at the middle distance.
std::vector<Pos> arrangeByRole(const std::vector<Role>& roles);

// The track icon (core/TrackIconSet.h) that stands for a role (0 for Unknown).
int roleIcon(Role r);

// Stage presets: a starting layout by role, with a room type and how far back the stage feels.
struct Preset {
  const char* name;   // English, marked for translation
  const char* hint;   // one line under the name
  int roomType;       // the room it plays in (core/StageRooms.h, never "No room")
  double strength;    // Song::stage.depthStrength
};
constexpr int kPresetCount = 6;
const Preset& preset(int k);
// Where each track stands in preset k (input order, never two on one point).
std::vector<Pos> arrangePreset(int k, const std::vector<Role>& roles);

}  // namespace gl::stage
