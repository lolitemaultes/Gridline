// Gridline — clean-room DAW core. Types & timeline constants.
// Reimplements the *workflow model* of a pattern-based DAW (patterns, channel
// rack, playlist clips, per-project PPQ) from scratch. No Image-Line code or assets.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gl {

using Tick = int64_t;

// Every model object (channel, pattern, clip, …) has an ID that is unique within
// its project and never reused, so commands and links survive reordering/deletion.
using ObjectId = uint32_t;
inline constexpr ObjectId kNoId = 0;
inline constexpr ObjectId kSongId = 1;      // the song itself (owner of song params)
inline constexpr ObjectId kFirstObjectId = 2;

// Time resolution is chosen per project (CORE-01). The selectable set matches
// what users of pattern DAWs expect; 96 is the default.
inline constexpr int kDefaultPpq = 96;
inline constexpr std::array<int, 11> kPpqChoices = {24, 48, 72, 96, 120, 144, 168, 192, 384, 768, 960};
inline bool isValidPpq(int ppq) {
  for (int p : kPpqChoices)
    if (p == ppq) return true;
  return false;
}

// Rescales a tick value between resolutions, rounding to the nearest tick.
inline Tick rescaleTick(Tick t, int fromPpq, int toPpq) {
  if (fromPpq == toPpq) return t;
  long double v = static_cast<long double>(t) * toPpq / fromPpq;
  return static_cast<Tick>(std::llround(v));
}

// Per-note property ranges (the piano roll's property lane edits these).
inline constexpr int kNotePanMax = 64;        // pan -64 (left) .. 64 (right), 0 = centre
inline constexpr int kNoteFineMax = 1200;     // fine pitch, cents -1200 .. 1200
inline constexpr int kNoteModMax = 255;       // mod X / mod Y 0..255, 128 = neutral
inline constexpr int kNoteModCentre = 128;
inline constexpr int kNoteReleaseDefault = 64;  // release velocity 0..127
inline constexpr int kNoteColors = 16;

// One piano-roll note.
struct Note {
  Tick start = 0;               // ticks from pattern/clip start
  Tick length = kDefaultPpq;    // ticks
  int pitch = 60;               // MIDI note number (60 = middle C)
  int velocity = 100;           // 0..127
  // Per-note properties. Defaults are neutral, so notes without them play as
  // before and old projects load unchanged.
  int pan = 0;                          // -64..64
  int fine = 0;                         // cents -1200..1200 (added to the pitch)
  int release = kNoteReleaseDefault;    // release velocity 0..127
  int modX = kNoteModCentre;            // instrument-defined modulation 0..255 (sampler: cutoff)
  int modY = kNoteModCentre;            // instrument-defined modulation 0..255 (sampler: resonance)
  int color = 0;                        // colour group 0..15
  bool slide = false;  // glides the channel's sounding notes to this pitch over its length (no new note)
  bool porta = false;  // starts at the previous note's pitch and glides to its own (channel porta time)
  bool muted = false;  // J-11: kept in the pattern but not played (mute tool)

  // {start, length, pitch, velocity} keeps working everywhere (without
  // missing-initializer warnings for the property fields).
  Note() = default;
  Note(Tick start_, Tick length_, int pitch_, int velocity_)
      : start(start_), length(length_), pitch(pitch_), velocity(velocity_) {}

  bool hasProps() const {
    return pan != 0 || fine != 0 || release != kNoteReleaseDefault || modX != kNoteModCentre || modY != kNoteModCentre ||
           color != 0 || slide || porta || muted;
  }
  bool operator==(const Note& o) const {
    return start == o.start && length == o.length && pitch == o.pitch && velocity == o.velocity && pan == o.pan &&
           fine == o.fine && release == o.release && modX == o.modX && modY == o.modY && color == o.color &&
           slide == o.slide && porta == o.porta && muted == o.muted;
  }
};

// What a playlist clip plays (arrangement).
enum class ClipKind : uint8_t {
  Pattern = 0,     // a pattern (Clip::pattern)
  Audio = 1,       // a sample, played through a channel's strip and mixer insert (Clip::source = Channel::id)
  Automation = 2,  // an automation curve (Clip::source = AutomationClip::id)
};

// One arrangement clip: a pattern, audio or automation clip placed on a playlist track.
struct Clip {
  ObjectId id = kNoId;
  ObjectId pattern = kNoId;  // Pattern::id
  int track = 0;             // playlist track lane
  Tick start = 0;            // ticks from song start
  Tick length = 0;           // ticks (0 = pattern natural length)
  // --- arrangement: clip kinds, offsets, mute, audio clip settings ---
  ClipKind kind = ClipKind::Pattern;
  ObjectId source = kNoId;   // Audio: Channel::id (sample + strip + insert); Automation: AutomationClip::id
  Tick offset = 0;           // pattern / automation clips: content ticks skipped at the clip start
  bool muted = false;
  // Audio clips (source times in seconds of the sample).
  double audioStart = 0;     // source position at the clip start
  double audioEnd = 0;       // source position where playback stops (0 = sample end)
  double fadeIn = 0, fadeOut = 0;  // fade lengths, seconds of playback time
  float gain = 1.0f;         // linear clip gain
  float pitch = 0.0f;        // cents, added to the channel's pitch shift
  double stretch = 1.0;      // time factor (2 = plays half as fast)
  double syncBpm = 0;        // > 0: follows the song tempo, natural speed at this tempo
  bool reverse = false;
  // Recording takes (J-8): clips recorded in the same loop-recording session
  // share a take group; `take` is the pass number (0 = first). kNoId = none.
  ObjectId takeGroup = kNoId;
  int take = 0;

  bool operator==(const Clip& o) const {
    return id == o.id && pattern == o.pattern && track == o.track && start == o.start && length == o.length &&
           kind == o.kind && source == o.source && offset == o.offset && muted == o.muted && audioStart == o.audioStart &&
           audioEnd == o.audioEnd && fadeIn == o.fadeIn && fadeOut == o.fadeOut && gain == o.gain && pitch == o.pitch &&
           stretch == o.stretch && syncBpm == o.syncBpm && reverse == o.reverse && takeGroup == o.takeGroup &&
           take == o.take;
  }
};

inline double midiToFreq(int note) {
  // Equal temperament, A4 = 440 Hz at MIDI 69.
  return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

}  // namespace gl
