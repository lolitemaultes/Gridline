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
#include <functional>
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
// K-112c (round 44): the mixer track a track plays through, for the Clips layout's strips: the linked
// one, else the one mixer track (not Main) every channel of its clips goes to (pattern clips: each
// channel with steps or notes; audio clips: their channel). nullptr when there is none or several.
const MixerTrack* playedMixer(const Song& s, int track);
// N45-05: when the clips play into several mixer tracks that all route into one bus (a drum kit's
// tracks into "Drum Bus"), that bus: the one fader, pan and sends that reach every sound of the track,
// as the reference's track with an instrument rack.
// Every mixer track the track's clips play into (Main included), in the order first met.
std::vector<ObjectId> playedMixers(const Song& s, int track);
// K-112c (C47): a mixer track's output: its first send that is not a sidechain (an insert sends to
// the master by default; a bus's members send there first). kNoId when it has none.
ObjectId outputOf(const MixerTrack& m);
// The return tracks, by mixer index in mixer order: the mixer tracks (not the master) that some track
// reaches by an aux send, a send other than its output. A bus fed only by outputs is not a return.
std::vector<size_t> returnTracks(const Song& s);
// Checkpoint 56 (N55-01): the track's output (its first send that is no sidechain or room send) goes to
// `to`; its other aux sends stay as they are. Checkpoint 57 (N56-01): an aux send to the new output
// steps out of the track's sends into Song::keptSends (its level and place kept), so it never plays
// (every send in `sends` plays, in every layout); when the output moves on, it goes back where it was.
// False (nothing changed) when `to` is the output already or the route would loop.
bool setOutput(Song& s, ObjectId from, ObjectId to);
// The send `from` keeps aside while it plays into `to` (Song::keptSends), nullptr when none.
const KeptSend* keptSend(const Song& s, ObjectId from, ObjectId to);
// Checkpoint 58 (N57-01): a kept send whose track no longer plays into its return (Studio, the Tracker,
// Smooth, a collaborator or a file routed the track elsewhere) goes back into the track's sends at its
// level and place, and the entry is dropped (also when the track or the return is gone, or the track
// has a send there again). Document runs it after every edit (in the same undo step), loading runs it,
// and a collaboration session runs it after a merge. False (nothing changed) when no entry was stale.
bool keptSendsStale(const Song& s);
bool restoreKeptSends(Song& s);
// The track's aux send to `to` (a plain send after its output), -1 when it has none.
int auxSendIndex(const MixerTrack& m, ObjectId to);
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
// The tracks in use: up to the last track with clips, a mode or a name, and never fewer than
// kShownTracks (a new set shows that many empty tracks). The Clips layout's arrangement rows and
// clip grid columns are exactly these, in the song's order (the empty tracks after them stay in
// the song, as Studio's playlist shows them).
inline constexpr int kShownTracks = 4;
int usedTrackCount(const Song& s);
// A new track right after the tracks in use (the track list grows when it has to), named
// "Track N" so it counts as in use. Returns its index (-1: the list is full).
int addUsedTrack(Song& s);
// K-112c (round 43): the starting tracks of a new project made in the Clips layout, as the
// reference's new set: two MIDI tracks (empty: they take pattern clips) then two audio tracks
// (each on a free mixer track), named "1 MIDI", "2 MIDI", "3 Audio" and "4 Audio". Part of the
// new project itself, so every view, Studio and collaborators show the same. Older projects are
// left as they are (they still show at least kShownTracks tracks).
// K-112c (round 45): and two returns, "Reverb" and "Delay" (shown as A Reverb, B Delay; Gridline's, fully
// wet, on the next free mixer tracks), which each audio track sends to at zero level.
void makeClipsStarterTracks(Song& s);
// K-112c (C44-16): a new return track: the first free mixer track, named `name`, with a send at zero
// level from every shown track's mixer track. Returns its id (kNoId: no free mixer track).
ObjectId addReturnTrack(Song& s, const std::string& name);
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
// Checkpoint 53 (N52-02): an audio track's clips play through its mixer track: each clip source only
// this track's clips use is routed there. Returns the channels rerouted (0 for other tracks).
int routeClipSources(Song& s, int track);
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
int addUsedTrack(Document& d, int count = 1);  // `count` new tracks in one undo step; returns the last one's index
// K-112c (round 45): the Clips Create menu's new tracks, right after the shown ones, one undo step
// each: an audio track on a free mixer track, or an empty MIDI track (it takes pattern clips),
// named "<number> Audio" / "<number> MIDI" as a new set's. Return the index (-1: none made).
// N45-08: `after` >= 0 puts it right after that track (else after the tracks in use); it takes the
// next colour of the Clips track colours.
int addAudioTrack(Document& d, int after = -1);
// Checkpoint 52 (N51-09): an unassigned track that got an audio clip (a file dropped past the tracks in
// Clips) becomes an audio track as Cmd+T makes: "N Audio", the next colour, a mixer track and the sends.
bool makeNamedAudioTrack(Song& s, int track);
int addMidiTrack(Document& d, int after = -1);
bool setArmed(Document& d, int track, bool armed);
bool setInput(Document& d, int track, int input, bool stereo);
bool setMonitor(Document& d, int track, InputMonitor m);
int assignMixerToNewTrack(Document& d, ObjectId mixer, std::string* why = nullptr);
bool unassignMixer(Document& d, ObjectId mixer);
int assignChannelToNewTrack(Document& d, ObjectId channel, std::string* why = nullptr);
bool unassignChannel(Document& d, ObjectId channel);

// ---- K-112c (checkpoint 51): the Clips layout's group tracks (Cmd+G) ----
// A group track is a group head (groupSize > 0) that is an audio track with no clips of its own: a
// bus its members play into, foldable, with no slots of its own (the clip grid shows the members'
// clips in its slots). Studio's playlist groups (a head that holds clips) are not group tracks.
bool isGroupTrack(const Song& s, int track);
// The group track holding `track` as a member (-1: none).
int groupTrackOf(const Song& s, int track);
// Groups the tracks (in use, none already grouped or a group): they are moved together (the first
// one's place), a group track named "<number> Group" in the first one's colour is put before them on
// a free mixer track, and each member's mixer track (an instrument playing into Main: the
// instrument) is routed into it instead of where it went. Returns the group track (-1: refused).
int groupTracks(Song& s, std::vector<int> tracks, std::string* why = nullptr);
// The group goes, its members stay where they are and play where the group played (Cmd+Shift+G).
bool ungroupTracks(Song& s, int group, std::string* why = nullptr);
int groupTracks(Document& d, const std::vector<int>& tracks, std::string* why = nullptr);  // one undo step, "group tracks"
bool ungroupTracks(Document& d, int group, std::string* why = nullptr);                  // one undo step, "ungroup tracks"
// Checkpoint 52: a track of a group plays into the group (its innermost one), as at grouping;
// makeAudioTrack and makeInstrumentTrack call it, so a member given its sound later joins too.
void routeIntoItsGroup(Song& s, int track);
// Checkpoint 52 (N51-06): each track in use that plays into a group it is not in plays into its own
// group (else Main); one in a group that plays into Main plays into the group. Run after a move.
// Returns the tracks rerouted.
int syncGroupRouting(Song& s);
int syncGroupRouting(Document& d);  // one undo step "group routing" (none when nothing changes)
// Tracks named by their place ("3 Audio") take their place's number again (N51-11: after a move).
void renumberTrackNames(Song& s);
// Checkpoint 53 (N52-06): a track renamed (every layout's rename): its mixer track (an audio or group
// track's, an instrument's own) and its instrument follow when they carried the track's old name, so
// the mixer, the members' Out and the rack show the new one. One undo step "rename playlist track";
// `extra` edits the track in the same step.
int followTrackName(Song& s, int track, const std::string& was);  // the names changed
bool renameTrack(Document& d, int track, const std::string& name, const std::function<void(PlaylistTrack&)>& extra = {});
// Checkpoint 52 (N51-03): Clips' Delete and Duplicate of tracks: a group brings all its tracks. Delete
// frees a group's mixer track and shortens the groups around; Duplicate puts the copies (clips too,
// audio tracks on new mixer tracks, a group copy with its tracks routed into it) right after the
// originals, in the originals' group. Return the count deleted (0: refused) / the first copy (-1).
// Checkpoint 53 (N52-10): Clips' automation lanes. A lane is a track holding automation clips only (no
// audio or instrument track, no group): the Arrangement shows it under its track, the clip grid shows
// no column for it. addAutomationLane makes one for a control of `owner` (a mixer track or channel)
// right under the track that control belongs to (a group's: after the group's tracks; none found:
// after the tracks in use), named `name`. Returns the lane (-1: the list is full).
bool isAutomationLane(const Song& s, int track);
std::vector<char> automationLanes(const Song& s, int count);  // the first `count` tracks' answers, in one pass
int addAutomationLane(Song& s, ObjectId owner, const std::string& name);
// Checkpoint 54 (N53-03): a lane belongs to the track its automation's control belongs to (the track
// whose instrument or mixer track that is): Clips draws it right under that track with no number and
// no activator, solo or arm, and moves and copies it with the track. addAutomationLane puts a new lane
// right under its track (a group's: under the group's title, inside the group). The storage is as
// before (a track holding the automation clips), so every layout reads the same project.
int laneOwner(const Song& s, int lane);  // -1: none (or not a lane)
// The track a control (a channel or mixer track) belongs to in Clips (-1: none).
int controlTrack(const Song& s, ObjectId owner);
// `tracks` with the lanes each one owns right under it (or right after its group), sorted.
std::vector<int> withLanes(const Song& s, const std::vector<int>& tracks);
// A drop place (before track `before`) moved past the lanes there, so nothing lands between a track
// and its lanes.
int pastLanes(const Song& s, int before);
// Checkpoint 54 (N53-04): an empty, unnamed track between tracks in use (rows another layout left
// empty, Studio's "Track 5"): Clips shows no row or column for it. The first `count` tracks' answers.
std::vector<char> gapTracks(const Song& s, int count);
// Checkpoint 54 (N53-08): a mixer track's zero sends to the return tracks it does not send to yet (as
// every Clips track has); and whether it lacks one.
void addReturnSendsTo(Song& s, ObjectId mixer);
bool missingReturnSends(const Song& s, ObjectId mixer);
// The numbers Clips shows its tracks by (1, 2, ... in order; lanes and gap tracks take none: 0).
std::vector<int> trackNumbers(const Song& s, int count);
// Checkpoint 55 (N54-04): the number Clips shows track `t` by (its place + 1 for a lane or a gap track).
int shownNumber(const Song& s, int t);
// Checkpoint 55 (N54-01): clip grid clips (the live area's) that loop although they are short unwarped
// samples: audio, under one beat, not following the tempo, not one-shots. Sets saved before checkpoint 54
// keep them looping (Clips makes new ones one-shots); Clips offers to make them play once.
std::vector<ObjectId> loopingShortSlotClips(const Song& s);
// Checkpoint 55 (N54-03): names made from an owner's name follow it. An automation named by its control
// ("3 Group › Volume") takes the control's name now ("2 Group › Volume") when only the owner's part
// differs, and the lanes showing that name with it. Returns the names changed (0: the song is unchanged).
int followAutomationNames(Song& s);
// Checkpoint 59 (N58-01): an automation take recorded into Clips' Arrangement (F9). `pts` are at
// arrangement ticks with parameter-normalized values (Hold points, as recorded). The take merges into the
// Arrangement clip of that control already under it (`tolerance`: the thinning), else becomes a new clip
// from the beat the take starts in, on the control's lane right under its track (a new lane when it has
// none). Never before the live area's end, so it never lands in the clip grid. Returns the automation
// clip's id (kNoId: nothing was written).
ObjectId recordArrangementAutomation(Song& s, ObjectId owner, uint32_t index, const std::string& name, std::vector<AutoPoint> pts,
                                     double tolerance = 0.004);
// Checkpoint 59 (N58-03): Clips' scenes (scene k: the live area's clips starting in [k * len, (k + 1) * len),
// `len` the scene length). insertScene: an empty scene under scene k (later scenes, the arrangement and
// its markers move down / along). deleteScene: scene k's clips go, the later scenes move up and the
// arrangement back. duplicateScene: a copy of scene k under it. Each keeps the scene names and colours
// with their scenes. The scene's name and colour (an empty name and colour 0: just its number).
void insertScene(Song& s, int k, Tick len);
bool deleteScene(Song& s, int k, Tick len);
bool duplicateScene(Song& s, int k, Tick len);
void setSceneName(Song& s, int k, const std::string& name);
void setSceneColor(Song& s, int k, Rgb color);
const SceneInfo* sceneInfo(const Song& s, int k);  // nullptr: nothing set
int deleteTracks(Song& s, const std::vector<int>& tracks, std::string* why = nullptr);
int duplicateTracks(Song& s, const std::vector<int>& tracks, std::string* why = nullptr);
int deleteTracks(Document& d, const std::vector<int>& tracks, std::string* why = nullptr);
int duplicateTracks(Document& d, const std::vector<int>& tracks, std::string* why = nullptr);

}  // namespace gl::trackmodes
