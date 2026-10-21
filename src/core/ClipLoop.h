// Gridline — looping pattern clips (Q-1).
//
// A pattern clip shows pattern ticks from its content offset on. When the
// pattern loops (Pattern::loop, FL's pattern loop mode), a clip longer than
// the pattern repeats it: the content wraps at the pattern's loop length
// (Song::patternContentLength). The offset is where the first pass starts,
// so a clip can start mid-pattern and still wrap. Without looping the part
// past the pattern's end stays empty (FL's behaviour for patterns whose loop
// mode is off).
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

// The loop length of a pattern clip's content (0 = it does not loop: not a
// pattern clip, a missing pattern, or a pattern whose loop mode is off).
Tick clipLoopLength(const Song& s, const Clip& c);
// The clip's passes: several for a looping pattern clip longer than one pass,
// else one pass [0, len) from the clip's offset.
std::vector<ClipPass> clipPasses(const Song& s, const Clip& c);
// True when the clip repeats its content (a loop point lies inside it).
bool clipRepeats(const Song& s, const Clip& c);

// Notes of a pattern clip's channel as they play inside the clip: ticks from
// the clip start, repeats unrolled, cut at the clip's end. Muted notes are
// skipped unless `keepMuted`.
std::vector<Note> clipChannelNotes(const Song& s, const Clip& c, const std::vector<Note>& patternNotes, bool keepMuted = false);

}  // namespace gl
