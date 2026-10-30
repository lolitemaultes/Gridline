// Gridline - J-191: putting split stems into the playlist under the clip they came from.
#pragma once
#include <string>
#include <vector>
#include "../../core/Song.h"

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

Rgb stemColor(const std::string& source);
// Display order: vocals, drums, bass, other, guitar, piano, then anything else.
int stemOrder(const std::string& source);

}  // namespace gl::stems
