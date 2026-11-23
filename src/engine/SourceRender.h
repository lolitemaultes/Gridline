// Gridline: render MIDI to audio (K-254). One engine call renders a part of the project into a
// buffer, and one place step lands that buffer in the project:
//
//   source   what is rendered:
//              Channel  the steps and notes of one or more channel rows of a pattern (a rack sequence)
//              Pattern  a whole pattern, every channel
//              Clip     one playlist pattern clip, with its own span, offset and loops (the K-250 render)
//              Notes    a few notes of one channel (a one-shot)
//   through  how much of the channel's chain is in the render:
//              Dry        the instrument alone (no note effects, no channel echo), straight to the master
//              ChannelFx  the instrument with its note effects and channel echo, as a freeze renders it
//              Mixer      through the channel's mixer tracks and their effects (the master stays neutral)
//
// A render is a normal export of the reduced song (sourceSong) over the source's span, so it is
// sample for sample what an export of that span makes with only the source playing (tests/test_k254_source.cpp).
// One channel through Dry or ChannelFx is rendered at unity and centre, before its volume and pan,
// exactly as a freeze of it (engine/Freeze.h renderSong); `keepsStrip` then says that the result
// should play through the source's own volume, pan and mixer track to sound the same.
//
// place() puts a written render into the song (callers wrap it in one Document::editSong, so each
// destination is one undo step): an audio clip on a free track, the source replaced in place, a new
// slicer channel (Kerf) cut at hits, notes, beats or transients, a one-shot sampler channel with its
// root key, or the next free pad of a drum pad channel. Loupe, the library and a file are the app's
// (they write or open the file and leave the song alone).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/AudioBuffer.h"
#include "../core/Song.h"
#include "Render.h"

namespace gl::srcrender {

enum class Kind : int { Channel = 0, Pattern, Clip, Notes, Count };
enum class Through : int { Dry = 0, ChannelFx, Mixer, Count };

struct Source {
  Kind kind = Kind::Channel;
  ObjectId pattern = kNoId;           // Channel, Pattern, Notes
  std::vector<ObjectId> channels;     // Channel: the rows (rack order); Notes: the one channel
  ObjectId clip = kNoId;              // Clip
  std::vector<int> notes;             // Notes: indices into the pattern's notes of channels[0]
  bool operator==(const Source& o) const {
    return kind == o.kind && pattern == o.pattern && channels == o.channels && clip == o.clip && notes == o.notes;
  }
};
inline Source channelSource(ObjectId pattern, ObjectId channel) { return Source{Kind::Channel, pattern, {channel}, kNoId, {}}; }
inline Source rowsSource(ObjectId pattern, std::vector<ObjectId> channels) { return Source{Kind::Channel, pattern, std::move(channels), kNoId, {}}; }
inline Source patternSource(ObjectId pattern) { return Source{Kind::Pattern, pattern, {}, kNoId, {}}; }
inline Source clipSource(ObjectId clip) { return Source{Kind::Clip, kNoId, {}, clip, {}}; }
inline Source notesSource(ObjectId pattern, ObjectId channel, std::vector<int> notes) {
  return Source{Kind::Notes, pattern, {channel}, kNoId, std::move(notes)};
}

// What a source renders through by default: one channel and notes through their channel's own
// effects (as a freeze), patterns and clips through the mixer (as a clip render).
Through defaultThrough(Kind k);
// Notes render as one-shots: the tail rings out and the ends are trimmed.
inline bool isOneShot(const Source& s) { return s.kind == Kind::Notes; }

struct Options {
  Through through = Through::ChannelFx;
  int loops = 1;                       // the span plays this many times (1..64)
  TailMode tail = TailMode::Cut;
  double maxTailSeconds = 4.0;         // Leave / Wrap
  bool oneShotTrim = false;            // start at the first sound, end where it falls under trimEndDb
  double trimEndDb = -60.0;
  int sampleRate = 44100;
  int blockSize = 512;
  std::function<void(double)> progress;
  const std::atomic<bool>* cancel = nullptr;
};
// The options a source starts with (its default Through; one-shots ring out and are trimmed).
Options defaultOptions(const Source& s);

// One onset in the render: where a step, note or beat starts.
struct Hit {
  Tick tick = 0;      // from the start of the span (the render's own ticks)
  int64_t frame = 0;  // in the rendered audio (after any trim)
};

struct Output {
  AudioBuffer audio;                 // stereo
  int sampleRate = 44100;
  int64_t bodyFrames = 0;            // the span (x loops) before the tail, after any trim
  int64_t trimmedFrom = 0;           // frames cut from the start by the one-shot trim
  Tick start = 0;                    // where the span starts: song ticks (Clip) or pattern ticks
  Tick length = 0;                   // the span in ticks (x loops)
  bool songTicks = false;            // start / length / hit ticks count on the song's timeline (Clip)
  double tempo = 120;                // the tempo at the span's start
  int ppq = 96;
  std::vector<Hit> hits;             // step and note onsets, one per step of the grid they fall in
  std::vector<Hit> notes;            // every distinct note onset
  std::vector<Hit> beats;            // every beat
  int rootNote = 60;                 // the note a sampler plays it back at its own pitch
  bool oneShot = false;              // tagged as a one-shot (else a loop) in the file
  bool keepsStrip = false;           // pre-fader: play it through the source channel's volume, pan and mixer track
  ObjectId stripOf = kNoId;          // that channel
  std::string name;                  // "Kick sequence (Break A)", "Reese one-shot C2", "Break A clip 3"
  std::string error;                 // non-empty: nothing rendered
  bool cancelled = false;
};

// The source can be rendered (it exists and holds steps or notes); a reason when not.
bool renderable(const Song& s, const Source& src, std::string* why = nullptr);
// The song a render plays: only the source sounds, through `through`, the master neutral.
Song sourceSong(const Song& s, const Source& src, Through through);
// The export job a render runs on sourceSong (scope, span, loops, tail).
RenderJob sourceJob(const Song& s, const Source& src, const Options& o);
// Renders the source. Sample for sample renderAudio(sourceSong, sourceJob), then the one-shot trim.
Output render(const Song& s, const Source& src, const Options& o);

// Trims a one-shot: the start moves to the first sample over -90 dBFS, the end to the last over
// `endDb` (a short fade into the cut). Returns the frames cut from the start.
int64_t oneShotTrim(AudioBuffer& a, double endDb = -60.0);

// Names.
std::string sourceName(const Song& s, const Source& src);
// How many hits (steps of the grid with a step or note starting in them) one pass of the source has.
int sourceHitCount(const Song& s, const Source& src);
// The span the source covers (ticks) and where (a pattern clip's song start, else the pattern's
// first clip in the playlist, else 0): where an audio clip of it lands.
Tick sourceSpan(const Song& s, const Source& src);
Tick sourceSongStart(const Song& s, const Source& src);

// ---- place ----

enum class Dest : int { AudioClip = 0, Replace, Slicer, OneShot, DrumPad, Loupe, Library, File, Count };
enum class SliceAt : int { Hits = 0, Notes, Beats, Transients, Count };
// Song edits happen for these; Loupe, Library and File only write or open the file.
inline bool editsSong(Dest d) { return d == Dest::AudioClip || d == Dest::Replace || d == Dest::Slicer || d == Dest::OneShot || d == Dest::DrumPad; }

struct PlaceOptions {
  Dest dest = Dest::AudioClip;
  SliceAt sliceAt = SliceAt::Hits;
  double sensitivity = 0.5;     // transients
  bool muteSource = false;      // Slicer, OneShot, AudioClip, DrumPad: the source falls silent (rows muted, notes muted, clips muted)
  ObjectId drumChannel = kNoId; // DrumPad: this drum pad channel (kNoId: the first in the rack)
};

struct Placed {
  ObjectId channel = kNoId, clip = kNoId, pattern = kNoId;
  std::vector<ObjectId> clips;  // Replace of a pattern: one per pattern clip
  int slices = 0;
  int pad = -1;                 // DrumPad: 0-based
  std::string error;
};

// Slice starts in frames of the rendered audio (the first is 0), at most 64.
std::vector<int64_t> slicePoints(const Output& out, SliceAt at, double sensitivity = 0.5);
// The drum pad channel a render would go to and its next free pad (-1 when none).
ObjectId drumPadChannel(const Song& s, ObjectId preferred = kNoId);
int freePad(const Song& s, ObjectId drumChannel);

// The library's automatic tag: a render is a loop when its audio lasts a whole number of bars
// (within 1 ms, at the span's tempo) and ends on a bar line of the song; anything else is a one-shot.
bool tagsAsLoop(const Song& s, const Output& out);
// The same guess before the render exists (the sheet's chip while its preview renders): the span
// times its loops in whole bars, starting on a bar, with no tail left ringing and no one-shot trim.
bool expectsLoop(const Song& s, const Source& src, const Options& o);

// Writes the render into `dir` as a 32-bit float WAV named after it (content-hashed, so a render
// made twice is one file), with a one-shot or loop tag, its tempo and beats. "" on failure. A file
// made before with the other tag is written again with this one (the audio is the same).
std::string writeRender(const Output& out, const std::string& dir, std::string* err = nullptr);
// Puts the render (written at `path`) into the song. Nothing changes for Loupe, Library and File.
Placed place(Song& s, const Source& src, const Output& out, const std::string& path, const PlaceOptions& o);

const char* destName(Dest d);       // untranslated (marked for translation)
const char* sliceAtName(SliceAt a);
const char* throughName(Through t);

}  // namespace gl::srcrender
