// Gridline — MIDI file import into a song (MI-03): tracks → channels, notes →
// one pattern, tempo map → tempo automation, time signatures → song meter +
// markers, track names / program changes → channel names, controllers →
// pattern automation. Song-level operations (callers wrap them in
// Document::editSong for undo).
#pragma once
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "MidiFile.h"
#include "Song.h"

namespace gl {

struct MidiImportOptions {
  std::set<int> midiChannels;      // MIDI channels (0..15) to import; empty = all
  std::set<int> tracks;            // track indexes (MidiFileData::tracks) to import; empty = all
  bool tempo = true;               // first tempo → song tempo; changes → tempo automation clip
  bool timeSignature = true;       // first meter → song; changes → meter markers
  bool markers = true;             // marker events → song markers
  bool programNames = true;        // unnamed tracks get their GM program name
  bool controllers = true;         // CC 7 / CC 10 → channel volume / pan pattern automation
  bool placeInPlaylist = true;     // a clip of the new pattern at the song start (first free track)
  std::string instrument = "gl.synth";     // plugin for melodic tracks
  std::string drumInstrument = "gl.drumkit";  // plugin for MIDI channel 10
};

struct MidiImportResult {
  ObjectId pattern = kNoId;
  std::vector<ObjectId> channels;
  int notes = 0;
  int automationLanes = 0;
  int tempoChanges = 0;
  int skippedControllers = 0;  // controller events without a Gridline target
  std::string error;
};

// Imports into a new pattern named `name` with one new channel per track.
MidiImportResult importMidiIntoSong(Song& song, const MidiFileData& midi, const std::string& name,
                                    const MidiImportOptions& o = {});

// Piano roll "import at cursor": the notes of `midi` (all tracks, or track
// `track`) into one channel of a pattern, starting at pattern tick `at`.
// Existing notes stay. Returns the number of notes added.
int insertMidiNotes(Song& song, ObjectId pattern, ObjectId channel, Tick at, const MidiFileData& midi, int track = -1);

// File ticks → song ticks.
inline Tick midiToSongTicks(int64_t t, int division, int ppq) {
  return static_cast<Tick>((static_cast<long double>(t) * ppq) / std::max(1, division) + 0.5L);
}

}  // namespace gl
