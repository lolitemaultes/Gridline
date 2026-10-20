// Gridline — playlist track modes (PL-08): audio tracks and instrument tracks.
//
// An audio track is linked to one mixer track: the track header shows that
// mixer track's input, monitoring and record arm, and audio recorded through
// it is placed on the track. An instrument track is linked to one rack
// channel (a generator), which is routed to a mixer track of its own; the
// track holds pattern clips only, selecting it selects the channel (MIDI input
// plays it) and notes recorded in song mode for the channel land on it.
// A mixer track or channel is linked to at most one track per arrangement.
//
// The Song-level functions do the edit; the Document overloads wrap one in a
// single undo step. Every function returns false (with a reason) and leaves
// the song unchanged when the edit is not possible.
#pragma once
#include <string>
#include <vector>
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::trackmodes {

const char* modeName(TrackMode m);

// ---- queries (stale ids count as "not linked") ----
// Mode of a track (Unassigned when its links no longer exist).
TrackMode modeOf(const Song& s, int track);
// The mixer track an audio track records through / an instrument track plays
// into (nullptr when none).
const MixerTrack* linkedMixer(const Song& s, int track);
const Channel* linkedChannel(const Song& s, int track);
// Track of the current arrangement linked to a mixer track as an audio track
// (-1 = none), and the instrument track of a channel.
int audioTrackOf(const Song& s, ObjectId mixer);
int instrumentTrackOf(const Song& s, ObjectId channel);
// True when any track of any arrangement links the mixer track / channel.
bool mixerLinked(const Song& s, ObjectId mixer, int exceptTrack = -1);
bool channelLinked(const Song& s, ObjectId channel, int exceptTrack = -1);
// A mixer track that can become an audio track's: an insert (not the
// master), not linked to another track, no channel playing into it.
bool mixerLinkable(const Song& s, ObjectId mixer, std::string* why = nullptr, int exceptTrack = -1);
// A channel that can become an instrument track's: a generator (not an
// audio-clip or automation source, not a layer), not linked elsewhere.
bool channelLinkable(const Song& s, ObjectId channel, std::string* why = nullptr, int exceptTrack = -1);
// First unused insert (default state, nothing routed into it, not linked);
// kNoId when every insert is in use.
ObjectId freeMixer(const Song& s);
// First playlist track at or after `from` without clips and without a mode
// (-1 = none; the track list may need to grow, see makeRoom).
int freeTrack(const Song& s, int from = 0);
// Whether a clip of this kind may sit on the track: instrument tracks take
// pattern clips only.
bool clipAllowed(const Song& s, ClipKind kind, int track);
// Playlist track audio recorded through `mixer` goes to: its audio track,
// else -1.
int recordTrack(const Song& s, ObjectId mixer);
// The audio tracks whose mixer track is armed for recording.
std::vector<int> armedTracks(const Song& s);

// ---- edits on a Song ----
// Audio track linked to `mixer` (kNoId: the first free insert, a new insert
// when there is none). The mixer track takes the track's name / colour when
// it has none, and the reverse. A previous link of the track is dropped (its
// mixer track is disarmed).
bool makeAudioTrack(Song& s, int track, ObjectId mixer = kNoId, std::string* why = nullptr);
// Instrument track playing `channel`. A channel that plays into the master or
// shares its mixer track with other channels is moved to a free insert of
// its own (named and coloured after it).
bool makeInstrumentTrack(Song& s, int track, ObjectId channel, std::string* why = nullptr);
// Back to an ordinary track (an audio track's mixer track is disarmed).
bool unassign(Song& s, int track);
// Recording controls of an audio track (they edit the linked mixer track).
// Arming a mixer track without an input selects the first input pair.
bool setArmed(Song& s, int track, bool armed);
bool setInput(Song& s, int track, int input, bool stereo);
bool setMonitor(Song& s, int track, InputMonitor m);
// Mixer menu: an audio track for the mixer track on the first free playlist
// track (returns it, -1 = none); and dropping the link of every track.
int assignMixerToNewTrack(Song& s, ObjectId mixer, std::string* why = nullptr);
bool unassignMixer(Song& s, ObjectId mixer);
// Channel menu: the same for instrument tracks.
int assignChannelToNewTrack(Song& s, ObjectId channel, std::string* why = nullptr);
bool unassignChannel(Song& s, ObjectId channel);
// An audio clip of `channel` dropped on `track`: the track it lands on
// (instrument tracks pass it on to the next empty track that takes it) — on
// an audio track the channel is routed through the track's mixer track.
int placeAudioClip(Song& s, int track, ObjectId channel);
// Drops links to mixer tracks / channels that no longer exist (after deletes,
// on load).
int dropStaleLinks(Song& s);

// ---- the same as single undo steps ----
bool makeAudioTrack(Document& d, int track, ObjectId mixer = kNoId, std::string* why = nullptr);
bool makeInstrumentTrack(Document& d, int track, ObjectId channel, std::string* why = nullptr);
// A new channel with `pluginType` (added after the last channel) as the
// track's instrument. Returns the channel (kNoId on failure).
ObjectId newInstrumentTrack(Document& d, int track, const std::string& pluginType, std::string* why = nullptr);
bool unassign(Document& d, int track);
bool setArmed(Document& d, int track, bool armed);
bool setInput(Document& d, int track, int input, bool stereo);
bool setMonitor(Document& d, int track, InputMonitor m);
int assignMixerToNewTrack(Document& d, ObjectId mixer, std::string* why = nullptr);
bool unassignMixer(Document& d, ObjectId mixer);
int assignChannelToNewTrack(Document& d, ObjectId channel, std::string* why = nullptr);
bool unassignChannel(Document& d, ObjectId channel);

}  // namespace gl::trackmodes
