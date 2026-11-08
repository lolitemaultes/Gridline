// Gridline — audio clip workflow (J-8): splitting / slicing, fit to tempo,
// normalize, stretch (realtime modes or rendered with the audio editor's
// stretch), consolidate a selection to audio and bounce a playlist track.
// Song-level operations (callers wrap them in Document::editSong for undo);
// the ones that write audio take the folder to write into.
#pragma once
#include <set>
#include <string>
#include <vector>
#include "../core/Automation.h"
#include "../core/Song.h"

namespace gl {

// Source seconds an audio clip plays at song tick t (follows the engine's
// mapping: resample, pitch-preserving stretch, tempo sync).
double clipSourceAt(const Song& song, const Clip& clip, const TempoMap& map, double tick);
// Natural length (ticks) of an audio clip's source window at the current settings.
Tick audioClipNaturalLength(const Song& song, const Clip& clip);

// K-114: looping audio clips (Clip::loop). The loop is the clip's audio window
// [audioStart, audioEnd or the sample's end); the clip's offset (ticks) is
// where its first pass starts inside it. audioLoopLength: ticks of one pass at
// the clip's start (0 = the clip does not loop or has no audio).
// audioLoopPasses: the passes as plain clips (same id, loop off, one window
// each: the first from the offset on), what the engine plays and FL export
// writes; a clip that does not loop is returned alone.
Tick audioLoopLength(const Song& song, const Clip& clip, const TempoMap& map);
std::vector<Clip> audioLoopPasses(const Song& song, const Clip& clip, const TempoMap& map);
// K-114: turning a clip's loop on or off (a copy; gestures and the clip menu).
// On: an audio clip shorter than its audio loops what it shows (its window
// ends at the clip's end). Off: an audio clip that started inside its loop
// starts its audio there. Automation curves need a loop length too
// (prepareAutomationLoops).
Clip withClipLoop(const Song& song, const TempoMap& map, Clip c, bool on);
// K-114: automation curves of the clips of `clips` listed in `sel` without a
// loop length get one: what those placements show (offset + length, the
// longest of them).
void prepareAutomationLoops(Song& song, const std::vector<Clip>& clips, const std::set<ObjectId>& sel);
// K-114: the loop of every listed clip on or off (withClipLoop, plus the
// automation loop lengths). False when nothing changed.
bool setClipsLoop(Song& song, const std::set<ObjectId>& clips, bool on);
// K-114: how long a looping clip plays before its content first repeats
// (from its loop start to the end of its pattern, audio window or curve loop);
// 0 when it does not loop.
Tick loopFirstPass(const Song& song, const Clip& clip, const TempoMap& map);
// K-114: a plain right-edge drag that brings a looping clip back to its own
// length (loopFirstPass) or shorter stops it looping, per clip of `sel` in
// `clips` (withClipLoop off). Returns how many stopped.
int unloopWithinFirstPass(const Song& song, std::vector<Clip>& clips, const std::set<ObjectId>& sel, const TempoMap& map);

// Splits an audio (or pattern) clip at song tick `at` (start < at < end).
// Returns the id of the right part (kNoId when not split).
ObjectId splitClipAt(Song& song, ObjectId clip, Tick at);
// Slices an audio clip at every grid line / at the transients of its audio.
// Returns the ids of all pieces (the original first).
std::vector<ObjectId> sliceClipAtGrid(Song& song, ObjectId clip, Tick grid);
std::vector<ObjectId> sliceClipAtTransients(Song& song, ObjectId clip, double sensitivity = 0.5);

// Fit to tempo: guesses the number of beats the audio spans at the song
// tempo, makes the clip follow the tempo and sets its length to that many
// beats. Returns the beat count (0 = not an audio clip).
int fitClipToTempo(Song& song, ObjectId clip);
// Clip gain so the source window peaks at `targetDb` dBFS. Returns the gain (0 = failed).
float normalizeClip(Song& song, ObjectId clip, double targetDb = 0.0);

// Realtime stretch: time factor (2 = twice as long); keepPitch selects the
// pitch-preserving mode of the clip's channel (else pitch follows speed;
// J-157: a channel on Rubber Band stays on it). The clip length follows.
bool setClipStretch(Song& song, ObjectId clip, double timeFactor, bool keepPitch);
// J-87: the same with an explicit time mode of the clip's channel (0 resample,
// 1 stretch, 7 Rubber Band; J-153: any other value is the stretch too).
bool setClipStretchMode(Song& song, ObjectId clip, double timeFactor, int mode);
// Rendered stretch through the audio editor's stretch (mode: 0 stretch, 1
// tape: pitch follows speed, 2 Rubber Band). Writes the result into `dir` on a new channel and points
// the clip at it (stretch reset to 1). Returns the file ("" = failed).
std::string renderClipStretch(Song& song, ObjectId clip, double timeFactor, double semitones, int mode,
                              const std::string& dir);

// Renders the given clips (through their channels and mixer tracks; master
// effects bypassed) over their time span into one WAV in `dir` and places it
// as a new audio clip at the span start on the first clip's track. With
// `replace`, the rendered clips are removed. Returns the new clip id.
ObjectId consolidateClips(Song& song, const std::set<ObjectId>& clips, const std::string& dir, double sampleRate,
                          bool replace = true);
// Renders the clips `keep` over [a, b) (song ticks; the clips should lie in
// the range) with a neutral master into a WAV in `dir` named after `name`.
// Returns the file ("" = nothing rendered).
std::string renderClipsToWav(const Song& song, const std::set<ObjectId>& keep, Tick a, Tick b, double sampleRate,
                             const std::string& dir, const std::string& name);
// Bounce (freeze) a playlist track: renders all of its clips to one audio clip
// placed on the first empty track below; the original clips are muted.
ObjectId bounceTrack(Song& song, int track, const std::string& dir, double sampleRate);

// J-193: tempo lock. A clip the user stretched keeps its bars when the tempo
// changes: it follows the tempo (syncBpm, set from its current geometry so
// locking never changes how it sounds at that moment) and is marked
// Clip::tempoLock (in Resample mode its pitch then follows its speed).
// Works on a copy (resize gestures); false when `c` is not a playable audio clip.
bool lockClipToTempo(const Song& song, Clip& c, const TempoMap& map);
// "Follow the project tempo" on (lock, as above) or off: the clip keeps its
// length, its audio and its current speed (the stretch takes over the speed
// the tempo gave it) and plays fixed seconds again. False when nothing changed.
bool setClipFollowsTempo(Song& song, ObjectId clip, bool follow);
// J-193: "Original length": the stretch back to 1 (the clip's audio kept,
// its length follows); a clip a stretch locked to the tempo plays fixed
// seconds again (older tempo-synced clips stay synced, as before).
bool resetClipStretch(Song& song, ObjectId clip);
// J-193: after a tempo change (`oldMap` -> `newMap`), every audio clip that
// does not follow the tempo keeps the audio it played (its source start and
// the cut where it ended: stored exactly in audioEnd) and its start; its
// length becomes that audio's duration at the new tempo. `before` holds the
// clips as they were (by id; clips changed since are left alone). Returns
// the number of clips changed.
int refitClipsToTempo(Song& song, const std::vector<Clip>& before, const TempoMap& oldMap, const TempoMap& newMap);

// Cleans a name for use in a file name.
std::string cleanFileName(const std::string& s);

}  // namespace gl
