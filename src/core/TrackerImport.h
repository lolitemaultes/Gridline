// Gridline - tracker modules (K-76): a module (.xm, .mod) becomes a Gridline song.
//
// The module is played through once, tick by tick, with the same channel logic the Tracker
// Sampler runs (TrackerChannel.h), to learn where every voice starts, how long it sounds and the
// channel state it starts in. Then:
//  * every instrument becomes a channel playing "gl.tracker" with the instrument's samples (16-bit
//    WAV files the import writes) and its envelopes, key map, vibrato and fadeout;
//  * every voice becomes a piano-roll note on its instrument's channel, carrying the cells of its
//    tracker channel for the rows it sounds through (TrackerStream.h), so effects play as the
//    tracker played them;
//  * the order list becomes playlist clips: one clip per stretch of consecutive rows of a pattern
//    (a pattern break or loop starts a new one). One row is one step. A pattern that plays with
//    different notes in different places (a voice carried over from the pattern before, other
//    effect memories) gets one Gridline pattern per variant;
//  * speed and tempo become the song tempo and a tempo automation clip, the global volume an
//    automation clip on the master volume, a song that loops a loop marker.
#pragma once
#include <string>
#include <vector>
#include "Song.h"
#include "TrackerModule.h"

namespace gl::tracker {

struct ImportOptions {
  // Where the instruments' sample files are written ("" = nowhere: the instruments refer to the
  // files there would be, nothing is written).
  std::string sampleFolder;
  bool write = true;               // false: the files are there already (refer to them only)
  double maxSeconds = 4 * 3600.0;  // a song longer than this is cut (it never ends)
};

struct ImportReport {
  int instruments = 0, notes = 0, patterns = 0, variants = 0, clips = 0, rows = 0, samples = 0;
  int rowsPerBeat = 4;
  double seconds = 0;     // the song's length as the tracker plays it
  bool loops = false;     // the song loops back (a loop marker)
  std::vector<std::string> files;     // sample files written (relative to the sample folder)
  std::vector<std::string> warnings;  // what does not carry over exactly
};

// The sample file of an instrument's sample slot (relative to the sample folder).
std::string sampleFileName(int instrument, int slot);
// Writes every sample of the module into `folder` as 16-bit WAV files; `names` lists them.
bool writeSamples(const Module& m, const std::string& folder, std::vector<std::string>& names, std::string* err = nullptr);
// Builds a new song from the module, replacing `song`. Writes the sample files when
// `o.sampleFolder` is set. false + `err` when nothing could be built (or a file not written).
bool buildSong(const Module& m, const std::string& name, const ImportOptions& o, Song& song, ImportReport& rep, std::string* err = nullptr);
// Reads a module file's bytes and builds the song (parseModule + buildSong).
bool importModule(const std::vector<uint8_t>& bytes, const std::string& name, const ImportOptions& o, Song& song, ImportReport& rep,
                  std::string* err = nullptr);

}  // namespace gl::tracker
