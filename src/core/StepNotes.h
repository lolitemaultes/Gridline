// Gridline — K-22: channel rack steps in the piano roll.
//
// A pattern keeps a channel's rack steps (Pattern::steps / stepProps) apart from its piano roll
// notes. The piano roll shows both: the steps as step notes (Note::step, one step long, every
// step parameter carried over). Viewing changes nothing. The first edit made in the piano roll
// moves the channel's steps into its notes, in the same undo step as the edit, and the rack then
// shows the channel's note preview. Step notes play exactly as the steps did (engine/Program.cpp).
#pragma once
#include <string>
#include <vector>
#include "Document.h"
#include "NoteTools.h"

namespace gl::stepnotes {

// The channel's steps that play (inside the pattern's length) as step notes.
std::vector<Note> fromSteps(const Song& s, const Pattern& p, ObjectId channel);
// True when the channel has a step that plays.
bool hasSteps(const Song& s, const Pattern& p, ObjectId channel);
// What the piano roll shows and edits for the channel: its notes and its step notes, sorted as
// Document::setNotes sorts them (at one start and pitch a step comes first, as it plays first).
std::vector<Note> view(const Song& s, const Pattern& p, ObjectId channel);
// Writes the piano roll's list for the channel. While the channel still has steps and `notes`
// differs from view(), one edit writes the notes and removes the steps (one undo step; `mergeKey`
// folds a drag's frames into it, as Document::setNotes does). An unchanged list writes nothing.
bool write(Document& d, ObjectId pattern, ObjectId channel, std::vector<Note> notes, const std::string& label,
           uint64_t mergeKey = 0);
// True when the note plays as a rack step in pattern `p` (a step note one step long).
bool playsAsStep(const Song& s, const Pattern& p, const Note& n);

// A channel's content in a pattern, for tools that cancel back to where they started.
struct Snapshot {
  ObjectId pattern = kNoId, channel = kNoId;
  std::vector<Note> notes;
  tools::StepRow row;
  tools::StepPropRow props;
  bool hadRow = false, hadProps = false;
};
Snapshot capture(const Song& s, ObjectId pattern, ObjectId channel);
// Puts the snapshot's notes and steps back (one edit, merged under `mergeKey`).
bool restore(Document& d, const Snapshot& snap, const std::string& label, uint64_t mergeKey = 0);

}  // namespace gl::stepnotes
