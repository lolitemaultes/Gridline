// Gridline — looping clips (Q-1, K-114).
//
// A pattern clip shows pattern ticks from its content offset on. When the
// clip loops (Clip::loop, per clip), a clip longer than its pattern repeats
// it: the content wraps at the pattern's loop length
// (Song::patternContentLength). The offset is where the first pass starts,
// so a clip can start mid-pattern and still wrap. Without looping the part
// past the pattern's end stays empty. Automation clips that loop repeat their
// curve every AutomationClip::loop ticks; audio clips loop their audio window
// (engine/ClipOps.h: audioLoopLength, audioLoopPasses).
//
// Projects saved before K-114 looped per pattern (Pattern::loop, FL's pattern
// loop mode): adoptOldClipLoops gives each of their clips the loop it played
// with. Pattern::loop stays as FL's pattern loop mode for FL interop
// (patternLoopMode).
#pragma once
#include <vector>
#include "Song.h"

namespace gl {

// One pass of the content inside a clip: `at` ticks from the clip start,
// `len` ticks long, showing content from `content` on (0 <= content < loop).
struct ClipPass {
  Tick at = 0, len = 0, content = 0;
  int index = 0;  // 0 = the first (possibly partial) pass
};

// Offset wrapped into [0, loop) (loop <= 0: unchanged).
Tick wrapLoopOffset(Tick offset, Tick loop);
// The passes of a window [offset, offset + len) over content that repeats
// every `loop` ticks (loop <= 0: a single pass).
std::vector<ClipPass> loopPasses(Tick offset, Tick len, Tick loop);

// The loop length of a pattern or automation clip's content (0 = it does not
// loop: Clip::loop off, an audio clip, a missing pattern, an automation clip
// without a loop length).
Tick clipLoopLength(const Song& s, const Clip& c);
// The clip's passes: several for a looping pattern clip longer than one pass,
// else one pass [0, len) from the clip's offset.
std::vector<ClipPass> clipPasses(const Song& s, const Clip& c);
// True when the clip repeats its content (a loop point lies inside it).
bool clipRepeats(const Song& s, const Clip& c);

// K-114: FL's pattern loop mode as the clips of `p` play it (FLP export):
// on when a clip of it that runs past the pattern's end loops, off when such a
// clip does not loop, else the pattern's stored mode (Pattern::loop).
bool patternLoopMode(const Song& s, const Pattern& p);
// True when a pattern clip runs past its pattern's content (offset + length
// beyond it; for a looping clip, its first pass's start + length).
bool clipPastContent(const Song& s, const Clip& c);
// K-114: clips of projects saved before per-clip loops get the loop they
// played with: a pattern clip of a looping pattern that ran past the pattern's
// end, an automation clip whose curve has a loop length. Every playlist
// (arrangements included).
void adoptOldClipLoops(Song& s);
// K-114: FL import: every pattern clip loops when its pattern's loop mode is
// on (FL loops per pattern).
void adoptPatternLoopModes(Song& s);

// Notes of a pattern clip's channel as they play inside the clip: ticks from
// the clip start, repeats unrolled, cut at the clip's end. Muted notes are
// skipped unless `keepMuted`.
std::vector<Note> clipChannelNotes(const Song& s, const Clip& c, const std::vector<Note>& patternNotes, bool keepMuted = false);

}  // namespace gl
