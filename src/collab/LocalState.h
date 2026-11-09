// Gridline - J-111 collaboration: what stays on one computer.
//
// The replicated document holds the PROJECT (content). Everything about what
// is sounding, playing, recording or being looked at on one computer is
// SESSION state: it never becomes an op and never reaches a collaborator.
//
// Not in the Document at all (so it can never be synced): the transport (play
// / stop, pattern or song mode, position, record mode, metronome, count-in,
// loop recording), live and held notes (typed keys, piano-roll keys, pads, the
// MIDI keyboard and sustain), the browser's preview, plugins playing on their
// own (Seltmann's pattern, Kerf's region, the chord preview, Loupe's Play,
// Shuttle's free run: AudioEngine::pluginPlayback), Stop / panic, recording in
// progress and the audio input device, windows, selection, scroll, zoom, the
// pattern shown, editor view state.
//
// In the Song but local (masked here: written as their defaults when the
// project goes into the replica, kept as they are on this computer when a
// collaborator's change comes in):
//  - mixer track solo and "solo alone" (a listening aid), record arm, input
//    monitoring, the audio input channel(s) and the record point;
//  - a playlist clip's "launched" flag (a live-mode clip playing right now);
//  - playlist track folding and height (view);
//  - plugin parameters that are performance gestures or listening aids, not
//    sound settings (isSessionParam: Solo / Listen / Delta / Hold / Freeze /
//    Latch / Running toggles of Gridline's own plugins).
// Mute IS shared (it is part of the mix).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "../core/Json.h"
#include "../core/Song.h"

namespace gl {
class AudioEngine;
}

namespace gl::collab {

// A plugin parameter that stays on this computer (see above).
bool isSessionParam(const std::string& type, size_t index);
// Every such parameter of Gridline's plugins, "type: name" (docs, tests).
std::vector<std::string> sessionParamList();

// Outgoing project JSON (a whole song, or one element of a list): session
// fields written as their defaults.
void maskSessionJson(Json& song);
// A digest of the shared part of a song (session state masked, Audio/ paths
// relative, id counter ignored): equal on every computer once they converged.
std::string sharedSongDigest(const Song& song);
void maskElementJson(const std::string& list, Json& element);
void maskPluginJson(Json& anything);  // every plugin state found inside

// Incoming: keep this computer's session fields (by object id / track index).
void keepSessionState(Song& incoming, const Song& current);
// K-141: a hash of every setting keepSessionState carries (what stays on this computer): a step
// that changed it is undone and redone here, since it never reaches the shared project.
uint64_t sessionStateHash(const Song& s);
// K-141: each of those settings by itself, keyed by the object it belongs to (a channel, mixer
// track or clip id, a track index) and, for a plugin parameter, the plugin's type: what one step
// changed is kept as these values before and after, and undone / redone where the objects still
// are (never by replaying the step's command on a song it was not made on).
struct SessionKey {
  enum Kind : uint8_t { ChannelPlugin, ChannelNoteFx, MixerSlot, MixerField, ClipLaunched, TrackFolded, TrackHeight };
  uint8_t kind = 0;
  uint64_t id = 0;    // object id (track index for the track fields)
  int32_t a = 0;      // note FX / slot index, or the mixer field
  int32_t b = 0;      // parameter index
  uint64_t type = 0;  // hash of the plugin type (plugin parameters)
  bool operator<(const SessionKey& o) const;
  bool operator==(const SessionKey& o) const;
};
struct SessionChange {
  SessionKey key;
  double before = 0, after = 0;
};
using SessionValues = std::vector<std::pair<SessionKey, double>>;  // sorted by key
SessionValues sessionValues(const Song& s);
// The settings found in both whose values differ (objects made or removed in between are no
// change of a setting).
std::vector<SessionChange> sessionChanges(const SessionValues& from, const SessionValues& to);
// Sets one value where its object (and plugin type) still is; false when it is gone.
bool applySessionValue(Song& s, const SessionKey& key, double value);
void keepChannelSession(Channel& incoming, const Channel& current);
void keepMixerSession(MixerTrack& incoming, const MixerTrack& current);
void keepClipSession(Clip& incoming, const Clip& current);

// A collaborator's change reached the open song: a plugin playing on its own
// here whose channel / slot was removed, or whose instrument, sample or model
// changed, stops with a normal release (its Play button goes off through the
// stop epoch). Call once after each frame that applied remote changes.
struct OwnPlaySig {
  int kind = 0;
  ObjectId id = kNoId;
  int slot = -1;
  std::string type;
  uint64_t dataHash = 0;
};
OwnPlaySig ownPlaySignature(const AudioEngine& e, const Song& s);
bool stopOwnPlaybackIfChanged(AudioEngine& e, const Song& s, const OwnPlaySig& before);

}  // namespace gl::collab
