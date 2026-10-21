// Gridline — groups and loops (J-31): pattern groups, note and clip groups,
// per-channel step loops, layer key splits and truncated swing.
//
// Song-level edits go through Document (one undo step each); note-list
// helpers are pure functions like the other piano roll tools.
#pragma once
#include <set>
#include <string>
#include <vector>
#include "Document.h"
#include "MenuOps.h"

namespace gl::tools {

// ---------------------------------------------------------------- note groups ----
// Puts the selected notes (at least two) into a new group; returns its number (0 = none made).
int groupNotes(std::vector<Note>& notes, const ops::NoteSel& sel);
// Takes the selected notes (all when none selected) out of their groups; returns how many changed.
int ungroupNotes(std::vector<Note>& notes, const ops::NoteSel& sel);
// Adds every note that shares a group with a selected note to the selection.
// Returns the number of notes added.
int selectGroups(const std::vector<Note>& notes, ops::NoteSel& sel);
// Q-3 (Shift+drag): appends a copy of every selected note (every property
// kept; grouped notes form new groups, one per original group) and makes the
// copies the selection. Returns, per original index, the copy's index (-1 =
// not copied).
std::vector<int> cloneNotes(std::vector<Note>& notes, std::vector<bool>& sel);

}  // namespace gl::tools

namespace gl::ops {

// ------------------------------------------------------------ pattern groups ----
int patternGroupOf(const Song& s, ObjectId pattern);  // group index, -1 = none
// The group's patterns that still exist, in pattern list order.
std::vector<ObjectId> patternsInGroup(const Song& s, int group);
// A new group holding the patterns (they leave their other groups). Returns its index, -1 = nothing to group.
int groupPatterns(Document& d, const std::vector<ObjectId>& patterns, const std::string& name);
bool renamePatternGroup(Document& d, int group, const std::string& name);
// Takes the patterns out of their groups; empty groups are removed. Returns how many left a group.
int ungroupPatterns(Document& d, const std::vector<ObjectId>& patterns);
bool deletePatternGroup(Document& d, int group);

// --------------------------------------------------------------- clip groups ----
// Groups the clips (at least two); returns the group id (kNoId = none made).
ObjectId groupClips(Document& d, const std::set<ObjectId>& clips);
int ungroupClips(Document& d, const std::set<ObjectId>& clips);
// The selection plus every clip that shares a group with a selected clip.
std::set<ObjectId> expandClipGroups(const Song& s, const std::set<ObjectId>& clips);

// ----------------------------------------------------------------- step loops ----
int stepLoop(const Pattern& p, ObjectId channel);  // 0 = no loop
// Sets the loop length (steps; 0 = no loop) of each channel in the pattern.
bool setStepLoop(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int steps);
// Steps the channel's content reaches (last step set, or the end of its last note, in steps).
int contentSteps(const Song& s, const Pattern& p, ObjectId channel);
// Loops each channel at its content rounded up to `unit` steps (a beat, a bar);
// channels whose content fills the pattern are not looped. Returns how many loop.
int loopToContent(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int unit);
// Writes the repeats into the pattern (steps and notes) and removes the loop.
int burnStepLoops(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels);
// The step row as it plays with the loop (pure; used by the burn and the rack view).
std::vector<uint8_t> loopedRow(const std::vector<uint8_t>& row, int loop, int total);

// --------------------------------------------------------- layer key splits ----
bool setKeyRange(Document& d, ObjectId channel, int lo, int hi);
// Splits the keyboard between the layer's children in order: the range the
// layer's notes use (else C4..B6) is cut into equal parts; the outer parts
// reach the keyboard ends.
bool splitLayerChildren(Document& d, ObjectId layer);
bool resetLayerChildren(Document& d, ObjectId layer);  // every child: the whole keyboard

// ----------------------------------------------------------------- alt solo ----
// Solos the tracks on their own (the tracks feeding them stay silent); the
// same tracks again: everything back on. Returns false when nothing is valid.
bool altSoloMixerTracks(Document& d, const std::vector<ObjectId>& tracks);

// ------------------------------------------------------------ truncated swing ----
// 0 off, 1 on, 2 toggle. Returns how many channels changed.
int setSwingTruncate(Document& d, const std::vector<ObjectId>& channels, int mode);

}  // namespace gl::ops
