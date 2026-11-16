// Gridline — Standard MIDI File (SMF) reading and writing, and conversion
// between MIDI files and patterns (MI-03).
//
// Reading accepts format 0 and 1 (format 2 and SMPTE division are refused):
// notes, tempo map, time signatures, markers, track names, program changes,
// controllers and pitch bend. Writing produces format 1 (a conductor track +
// one track per channel) or format 0 (everything merged into one track).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {

struct MidiNote {
  int64_t start = 0, length = 0;  // in file ticks
  int pitch = 60, velocity = 100, channel = 0;
};

// Controller / pitch bend event. controller 0..127 = CC (value 0..127),
// kMidiPitchBend = pitch bend (value 0..16383, 8192 = centre).
inline constexpr int kMidiPitchBend = 128;
// K-99: channel pressure (value 0..127).
inline constexpr int kMidiChannelPressure = 129;
struct MidiCtl {
  int64_t tick = 0;
  int controller = 0;
  int value = 0;
  int channel = 0;
  bool operator==(const MidiCtl& o) const {
    return tick == o.tick && controller == o.controller && value == o.value && channel == o.channel;
  }
};

struct MidiTrack {
  std::string name;
  std::vector<MidiNote> notes;
  int program = -1;   // first program change (-1 = none)
  int channelProgram[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // first program per MIDI channel
  int channel = -1;   // MIDI channel of the track's first channel event (-1 = none)
  std::vector<MidiCtl> ctls;  // controllers and pitch bend, by tick
};

struct MidiTempo {
  int64_t tick = 0;
  double bpm = 120.0;
};
struct MidiTimeSig {
  int64_t tick = 0;
  int num = 4, den = 4;
};
struct MidiMarker {
  int64_t tick = 0;
  std::string text;
};

struct MidiFileData {
  int format = 1;        // as read (0 or 1); writeMidiFile takes the format separately
  int division = 96;     // ticks per quarter note
  double tempo = 120.0;  // first tempo event (BPM)
  int timeSigNum = 4, timeSigDen = 4;  // first time signature
  std::vector<MidiTempo> tempos;      // tempo map (sorted; empty = constant `tempo`)
  std::vector<MidiTimeSig> timeSigs;  // meter changes (sorted; empty = `timeSigNum/Den`)
  std::vector<MidiMarker> markers;
  std::vector<MidiTrack> tracks;  // tracks with notes or controllers (the conductor track is folded into the fields above)
};

bool readMidiFile(const std::string& path, MidiFileData& out, std::string* err = nullptr);
bool parseMidi(const std::vector<uint8_t>& bytes, MidiFileData& out, std::string* err = nullptr);
bool writeMidiFile(const std::string& path, const MidiFileData& data, std::string* err = nullptr, int format = 1);
std::vector<uint8_t> encodeMidi(const MidiFileData& data, int format = 1);

// Format 0 files (or tracks mixing MIDI channels): one track per MIDI channel.
// Tracks keep their name (with " ch N" added when a track was split).
// K-99: MPE tracks (every note on its own member channel) stay whole.
void splitTracksByChannel(MidiFileData& m);
// K-99: an MPE track: it announces an MPE zone (RPN 6), or its notes sit one at a time on
// member channels 2..16 with per-channel pitch bend, pressure or CC 74.
bool isMpeTrack(const MidiTrack& t);
// K-99: the per-note pitch bend range of an MPE track's member channels (the MPE default).
inline constexpr int kMpeBendRange = 48;

// ---- song → MIDI ----
struct MidiExportOptions {
  std::set<ObjectId> channels;  // channels to export (empty = all)
  bool tempoMap = true;         // tempo automation → tempo changes
  bool markers = true;          // song markers → marker events (song exports)
  bool automation = true;       // channel volume / pan automation → CC 7 / CC 10
  // K-99: MPE tracks (each note on its own MIDI channel 2..16 with its own pitch bend, channel
  // pressure and CC 74): 0 for the channels whose notes carry pressure or timbre lanes, 1 never,
  // 2 every channel.
  int mpe = 0;
};
// Notes of one pattern, one track per channel that has notes or steps.
MidiFileData patternToMidi(const Song& song, ObjectId pattern, const MidiExportOptions& o = {});
// The whole arrangement (playlist clips expanded), one track per channel.
MidiFileData songToMidi(const Song& song, const MidiExportOptions& o = {});
// The arrangement between song ticks [start, end) (time selection), shifted to start at 0.
MidiFileData songRangeToMidi(const Song& song, Tick start, Tick end, const MidiExportOptions& o = {});

// General MIDI program names (the public GM 1 sound set), 0..127.
const char* gmProgramName(int program);

}  // namespace gl
