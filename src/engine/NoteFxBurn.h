// Gridline — "burn" a channel's note processors (arpeggiator, chords, note
// scripts, …) into plain notes (J-27): the chain runs offline over the notes
// as the engine would play them, and its output for the channel itself comes
// back as notes. Notes the chain sends to other channels are not included.
#pragma once
#include <vector>
#include "../core/Document.h"

namespace gl {

// The notes the channel's note processor chain produces from `notes` over
// [0, length) ticks (plus a short tail so held notes close). Without
// processors the notes come back unchanged.
std::vector<Note> burnNoteFx(const Song& song, ObjectId channel, const std::vector<Note>& notes, Tick length);

// Replaces the channel's notes in the pattern by the processed notes and
// removes the channel's note processors (one undo step). False when the
// channel has no processors or no notes.
bool burnNoteFxToPattern(Document& d, ObjectId pattern, ObjectId channel);

}  // namespace gl
