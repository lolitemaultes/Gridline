// Gridline — "burn" a channel's note processors (arpeggiator, chords, note
// scripts, …) into plain notes (J-27): the chain runs offline over the notes
// as the engine would play them, and its output for the channel itself comes
// back as notes. Notes the chain sends to other channels are not included.
#pragma once
#include <string>
#include <vector>
#include "../core/Document.h"

namespace gl {

// The notes the channel's note processor chain produces from `notes` over
// [0, length) ticks (plus a short tail so held notes close). Without
// processors the notes come back unchanged.
std::vector<Note> burnNoteFx(const Song& song, ObjectId channel, const std::vector<Note>& notes, Tick length);

// Replaces the channel's notes in the pattern by the processed notes and
// removes the channel's note processors (one undo step). False when the
// channel has no processors or no notes. Steps go in as the piano roll shows
// them (pitch, fine pitch, shift, a lane's own grid), so they bake where they play.
bool burnNoteFxToPattern(Document& d, ObjectId pattern, ObjectId channel);

// K-438: instruments with a sequencer of their own (PluginInfo::sequencerParams; Acid Bass, Step
// Bass). True when the channel's instrument has one switched on.
bool sequencerOn(const Song& song, ObjectId channel);
// The notes the instrument's own sequencer plays over one pass of the pattern, as the engine plays
// them (the channel's notes and note processors drive it; channels feeding it by MIDI play too),
// read from the instrument's MIDI output. Pattern ticks; notes still sounding at the end close there.
// A sequencer in two parts (PluginInfo::seqParts, Plasma): Whole prints what it plays (the
// arpeggiator when it is on, else the clip player); the others print one part's notes only.
enum class SeqPart { Whole = 0, Arpeggiator, ClipPlayer };
std::vector<Note> printSequence(const Song& song, ObjectId pattern, ObjectId channel, SeqPart part = SeqPart::Whole);
// Both parts of a two-part sequencer are on (the print asks which one).
bool sequencerPartsOn(const Song& song, ObjectId channel);
// Writes them as the channel's notes in the pattern (its steps and old notes go), switches the
// sequencer off and removes the note processors that fed it (one undo step). False when the
// sequencer is off or played nothing. Printing the clip player of a two-part sequencer switches
// only the clip player off: its notes then go through the arpeggiator as before.
bool printSequenceToPattern(Document& d, ObjectId pattern, ObjectId channel, SeqPart part = SeqPart::Whole);

// K-438: an installed plugin's own MIDI output (arpeggiators, sequencers, chord plugins). The
// channel that plays what it sends: the first channel listening on its MIDI output port, else the
// plugin's own channel.
ObjectId midiOutputTarget(const Song& song, ObjectId channel);
// Puts recorded MIDI output in a new pattern ("<channel> MIDI output", as long as `like` or as the
// notes need), as notes of `target` (one undo step `label`). kNoId when `notes` is empty.
ObjectId writeMidiOutputPattern(Document& d, ObjectId like, ObjectId source, ObjectId target, const std::vector<Note>& notes,
                                const std::string& label);
// Renders one pass of `pattern` offline and writes what the channel's instrument sends on its MIDI
// output into a new pattern (writeMidiOutputPattern). The new pattern, or kNoId: nothing was sent.
ObjectId printMidiOutputToPattern(Document& d, ObjectId pattern, ObjectId channel);

}  // namespace gl
