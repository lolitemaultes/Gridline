// Gridline — "tideline" pack: trigger sources in the project (J-165). What
// Tideline's Audio trigger hears (a sidechain route from a mixer track or a
// channel's track) and which channel its note trigger follows, as document
// edits (one undo step each) shared by the editor and the tests.
#pragma once
#include <string>
#include <vector>
#include "../../core/Document.h"

namespace gl::packs::tideline {

// Tracks with a sidechain-only send into `track` (what "Sidechain input" hears), in mixer order.
std::vector<ObjectId> sidechainKeys(const Song& s, ObjectId track);
// The mixer track the Audio trigger of a slot on `track` listens to for a Trigger input
// value `scIn` >= 1 (an input route, J-45); kNoId when there is none.
ObjectId routeSourceTrack(const Song& s, ObjectId track, int scIn);
// The mixer track a channel plays into (its own or the master).
ObjectId channelTrack(const Song& s, ObjectId channel);
// True when the channel can key `track` as it is (it plays into a track of its own
// that is neither the master nor `track` itself); otherwise linking moves it to a
// free mixer track first.
bool channelKeysDirectly(const Song& s, ObjectId channel, ObjectId track);

// Audio trigger of the Tideline in slot `slot` of mixer track `owner` listens to mixer
// track `from`: an existing send from it is used (sidechain-only: Trigger input =
// sidechain input; a mixed send: its input route), otherwise a sidechain-only send is
// added (it shows in the mixer's routing). Sets Trigger to Audio. One undo step.
// False (nothing changed) when the route would make a loop or a track is missing.
bool linkAudioTrack(Document& d, ObjectId owner, int slot, ObjectId from);
// The same from a channel's track. A channel that plays into the master or into
// `owner` itself is first moved to a free mixer track of its own.
bool linkAudioChannel(Document& d, ObjectId owner, int slot, ObjectId channel);
// Removes the sidechain-only send from `from` into `owner`.
bool unlinkAudioTrack(Document& d, ObjectId owner, ObjectId from);

// Note trigger follows channel `channel` (its notes and audio clip starts): Trigger
// MIDI, Note source Channel, the channel id in the state. One undo step.
bool linkNoteChannel(Document& d, ObjectId owner, int slot, ObjectId channel);
// Note trigger listens to MIDI Out ports (`port`: -1 any) or to the MIDI input
// (src = SrcPorts / SrcInput); forgets the channel. One undo step.
bool setNoteSource(Document& d, ObjectId owner, int slot, int src, int port = -1);

}  // namespace gl::packs::tideline
