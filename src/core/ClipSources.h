// Gridline — J-82: playlist clip sources (patterns, audio clip channels,
// automation clips) as a list: which ones the arrangement uses, and deleting
// them together with every clip of them (all arrangements) as one undo step.
// Deleting an audio clip source removes its channel from the project; the
// sample file on disk is never touched. Tested in tests/test_clip_sources.cpp.
#pragma once
#include <vector>
#include "Document.h"

namespace gl::ops {

// A channel listed as an audio clip source: a sampler with a sample (FL's
// audio clip channels import as such).
bool isAudioClipSource(const Channel& ch);

// Steps or notes of the channel in any pattern (a sampler that also plays
// as an instrument).
bool channelHasPatternData(const Song& s, ObjectId channel);

// Number of playlist clips of the source in every arrangement.
int clipSourceUses(const Song& s, ClipKind kind, ObjectId source);

// Sources of `kind` no clip in any arrangement uses (patterns: every pattern
// without a clip; audio: audio clip channels only).
std::vector<ObjectId> unusedClipSources(const Song& s, ClipKind kind);

// Deletes the sources and every clip of them as one undo step ("delete clip
// sources"). Audio: the channel goes, with its automation links, MIDI links,
// pattern data and layer references (Document::removeChannel); its mixer
// track stays. Patterns: the last pattern of the project is kept. Returns how
// many sources were deleted.
int deleteClipSources(Document& d, ClipKind kind, const std::vector<ObjectId>& sources);

}  // namespace gl::ops
