// Gridline - J-191: putting split stems into the playlist under the clip they came from.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/Song.h"

namespace gl {
struct Sample;
}

namespace gl::stems {

struct StemFile {
  std::string source;  // "vocals", "drums", ...
  std::string path;    // the stem's audio file (same rate and length as the clip's sample)
};

// One new audio clip per stem on new playlist tracks right below `clip`'s track (empty tracks
// further down when the playlist is full). Every new clip is a copy of the original (position,
// length, offset into the sample, stretch, pitch, gain, fades, reverse) playing its stem, and
// every new channel a copy of the clip's channel (routing, stretch mode, volume) with the stem
// file, so the stems line up with the source sample-accurately. `muteOriginal` mutes the
// source clip. Returns the new clip ids (empty when the clip is not an audio clip).
std::vector<ObjectId> placeStems(Song& s, ObjectId clip, const std::vector<StemFile>& stems, const std::string& baseName,
                                 bool muteOriginal);

// K-447: the part of an audio clip a split takes: a slice or trim of a longer file, the part of it
// inside a time selection, or the clip editor's selection. Only that part is separated, and its
// stems play exactly that part again, at the same place on the timeline.
struct StemPart {
  ObjectId clip = kNoId;
  Clip snapshot;              // the clip as it was when the part was taken
  Tick t0 = 0, t1 = 0;        // the part on the timeline (song ticks, inside the clip)
  int64_t heardFrom = 0, heardTo = 0;  // the frames of the clip's sound the part hears (file direction)
  int64_t from = 0, to = 0;   // what is split: those frames and the handles around them
  int64_t handle = 0;         // the handles' length on each side (stemHandleFrames), before the file's ends cut them
  int64_t frames = 0;         // the sound's length (frames)
  double rate = 0;            // and its sample rate
  bool reversed = false;      // the clip plays it backwards
  bool wholeClip = true;      // the part is the whole clip
  // What the clip plays: its file with the channel's sample processing (engine/ClipSource.h:
  // handles, precomputed effects, normalize, ...), so the stems add up to what is heard.
  std::shared_ptr<const Sample> sound;
  double seconds() const { return rate > 0 ? double(heardTo - heardFrom) / rate : 0.0; }  // what is heard
  double startSeconds() const { return rate > 0 ? double(from) / rate : 0.0; }
  double endSeconds() const { return rate > 0 ? double(to) / rate : 0.0; }
};

// The frames a stem keeps on each side of the part where the file has them, never heard: what
// playback reads around the part, so the stem clip plays what the clip plays also at its first and
// last frames. A clip played by resampling reads only the resampler's kernel around a position
// (kStemHandleFrames, 5.8 ms at 44.1 kHz). A clip whose pitch is kept apart from its speed (the
// Stretch and Rubber Band modes, warp markers, tempo sync, a pitch on a resampled clip) goes through
// the stretch engines, which start on 0.25 s of the audio before the clip and read on past its end:
// half a second of source, times the fastest speed the clip reads it at (kStemStretchHandleSeconds).
inline constexpr int64_t kStemHandleFrames = 256;
inline constexpr double kStemStretchHandleSeconds = 0.5;
// The handle length (frames of a `rate` sound) for audio clip `c` on channel `ch`.
int64_t stemHandleFrames(const Song& s, const Channel& ch, const Clip& c, double rate);

// The part of audio clip `clip` that plays in song ticks [t0, t1) (t1 <= t0: the whole clip),
// through its slip, loop passes, stretch, tempo, warp markers and reverse: from the earliest to the
// latest frame it hears (and the frame after it, which playback interpolates towards), with the
// handles around it. False when the clip is not an audio clip, its sound is not there, or the part
// hears none of it.
bool clipStemPart(const Song& s, ObjectId clip, Tick t0, Tick t1, StemPart& out);

// The stems of a part (each file holds frames [part.from, part.to) of the sound, separated) onto
// new tracks under the clip, as placeStems above, each new clip covering exactly the part: the
// clip's piece in [t0, t1) (cut as the split tool cuts), playing its stem with the clip's own
// settings (stretch, pitch, gain, fades, reverse, warp markers), its window moved to the stem
// file, so it plays sample for sample what the piece plays. Each new channel copies the clip's
// channel but plays its stem as it is (no start / end handles, precomputed effects or sample
// processing: the stem already holds them). `muteOriginal` mutes the piece of the original clip
// (cut out of it when the part is not the whole clip). A clip moved since the part was taken gets
// its stems where it is now.
std::vector<ObjectId> placeStems(Song& s, const StemPart& part, const std::vector<StemFile>& stems, const std::string& baseName,
                                 bool muteOriginal);

Rgb stemColor(const std::string& source);
// Display order: vocals, drums, bass, other, guitar, piano, then anything else.
int stemOrder(const std::string& source);

}  // namespace gl::stems
