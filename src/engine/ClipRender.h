// Gridline: playlist clips rendered to audio one at a time (K-250, K-251).
//
// One render for every way a single clip becomes audio: "Render clip to
// audio" (the clip replaced in place by an audio clip of its sound), "Render
// clip to file" and "Export clip as WAV" (a file), and the batch versions of
// both, where every selected clip gets its own render (never one merged file:
// that is Consolidate). The clip plays on its own over its own span: its
// channels and their mixer tracks (insert effects kept), every automation
// clip of the song, the master neutral (unity, no effects); the tail is cut
// at the clip's end. The result is sample for sample what the song makes over
// that span with only that clip playing (tests/test_k250_clip_render.cpp).
#pragma once
#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "Render.h"

namespace gl {

namespace receipt {
struct Origin;
}

// The song reduced to one clip: the clip unmuted, every other clip but the
// automation clips gone, every playlist track playing, the master neutral.
Song clipAloneSong(const Song& song, ObjectId clip);
// Renders clip `clip` alone over [its start, its end) (clipAloneSong).
// Pattern, audio and any other clip kind but automation; an error in
// RenderResult::error when there is nothing to render.
RenderResult renderClipAlone(const Song& song, ObjectId clip, int sampleRate, std::function<void(double)> progress = {},
                             const std::atomic<bool>* cancel = nullptr);
// The clips of `clips` a clip render takes (not automation), in playlist
// order: by start, then track.
std::vector<ObjectId> renderableClips(const Song& song, const std::set<ObjectId>& clips);
// A name for the clip's render: the audio clip's channel or the pattern's name.
std::string clipRenderName(const Song& song, ObjectId clip);
// File names (without folder or extension) for a batch of clips, one each:
// the clip's name, cleaned for a file name, numbered " 2", " 3", ... where
// names repeat in the batch or `taken` (names already in the folder) holds them.
std::vector<std::string> batchFileNames(const Song& song, const std::vector<ObjectId>& clips, const std::set<std::string>& taken = {});
// The Track Receipt note of a clip's render: the part of its file an audio
// clip played (seconds, speed, direction), or the files a pattern clip's
// instruments read. False when the render names no file.
bool clipRenderOrigin(const Song& song, ObjectId clip, int64_t frames, int sampleRate, receipt::Origin& out);

// K-250: clip `clip` replaced in place by an audio clip of its sound (one
// render written into `dir`, on a new sampler channel to the master: the
// render already holds its mixer track's effects). The new clip takes the
// clip's track, start, length and mute state. Returns its id (kNoId: nothing
// rendered).
ObjectId renderClipInPlace(Song& song, ObjectId clip, const std::string& dir, int sampleRate);
// K-251: every clip of `clips` replaced by its own render (renderClipInPlace
// each). Returns the new clip ids in playlist order.
std::vector<ObjectId> renderClipsInPlace(Song& song, const std::set<ObjectId>& clips, const std::string& dir, int sampleRate);

// K-251: one file per clip in folder `dir` (WAV, `bits` 16, 24 or 32 float),
// named by batchFileNames (an existing file is never overwritten: the name
// takes the next number). Returns the files written in playlist order; `err`
// holds the first failure. `progress` 0..1 over the whole batch.
struct ClipFile {
  ObjectId clip = kNoId;
  std::string path;
};
std::vector<ClipFile> renderClipsToFiles(const Song& song, const std::set<ObjectId>& clips, const std::string& dir, int sampleRate, int bits,
                                         std::string* err = nullptr, std::function<void(double)> progress = {},
                                         const std::atomic<bool>* cancel = nullptr);

}  // namespace gl
