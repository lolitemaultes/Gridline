// Gridline — K-198 follow-up: one note selection over several channels of one pattern.
//
// The piano roll edits a target channel (where new notes go) and, beside it, the channels ticked
// in its channel picker. A selection may hold notes of any of them; moving, dragging, nudging,
// transposing, deleting, copying and pasting work on all of them together. Every write goes through
// stepnotes::write (Document ops, so collaboration stays safe). Callers wrap a whole edit in
// Document::beginGroup / endGroup so it is one undo step; drag frames pass a merge key.
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Document.h"

namespace gl::multinote {

// Channel -> note indices in that channel's piano roll list (stepnotes::view order).
using Selection = std::map<ObjectId, std::set<size_t>>;
// Channel -> its piano roll list.
using Lists = std::map<ObjectId, std::vector<Note>>;
struct ChannelNote {
  ObjectId channel = kNoId;
  Note note;
};

// Number of selected notes (indices past a list's end are not counted by the edits).
size_t count(const Selection& sel);
// Drops empty channel entries.
void prune(Selection& sel);
// The piano roll lists (stepnotes::view) of `channels` in pattern `pattern`.
Lists lists(const Song& s, ObjectId pattern, const std::set<ObjectId>& channels);
Lists lists(const Song& s, ObjectId pattern, const Selection& sel);

// The pitch step every selected note can take (clamped so all stay in 0..127).
int clampPitchDelta(const Lists& base, const Selection& sel, int dp);
// The earliest selected start (or `fallback` when nothing is selected).
Tick earliestStart(const Lists& base, const Selection& sel, Tick fallback);

// Writes each channel of `base` with `fn` applied to its selected notes. `copy`: the selected
// notes stay where they are and changed copies are added (the copies are the new selection).
// Returns the selection after the edit (indices in the sorted lists). Does not group.
Selection transform(Document& d, ObjectId pattern, const Lists& base, const Selection& sel,
                    const std::function<void(Note&)>& fn, bool copy, const std::string& label, uint64_t mergeKey = 0);
// Deletes the selected notes of every channel. Does not group.
void erase(Document& d, ObjectId pattern, const Lists& base, const Selection& sel, const std::string& label);
// The selected notes with their channels (in channel, then list order).
std::vector<ChannelNote> copy(const Lists& base, const Selection& sel);
// Adds `clip` (each note to its own channel, starts moved by `shift`); the pasted notes are the
// returned selection. Channels no longer in the song are skipped. Does not group.
Selection paste(Document& d, ObjectId pattern, const std::vector<ChannelNote>& clip, Tick shift, const std::string& label);
// How far a paste of `clip` moves it: just past its end, on `grid`.
Tick pasteShift(const std::vector<ChannelNote>& clip, Tick grid);

}  // namespace gl::multinote
