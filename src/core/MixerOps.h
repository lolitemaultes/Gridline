// Gridline — mixer track operations behind the mixer's track, routing, slot
// and layout menus (J-12): routing modes with cycle explanations, submixes,
// pre-fader / sidechain sends, mono / disable / latency / threading switches,
// groups, docking, icons, effect moves between tracks, track state files and
// per-track stem renders. Every edit is one undo step.
#pragma once
#include <set>
#include <string>
#include <vector>
#include "Document.h"
#include "Song.h"

namespace gl::mixops {

// ---------------------------------------------------------------- routing ----
// Why `from` can't send to `to` ("" = it can): itself, the master's output,
// or the loop it would close, spelled out ("… would loop: Insert 5 → Insert 3 → Insert 2").
std::string routeProblem(const Song& s, ObjectId from, ObjectId to);
// Existing route chain to → … → from that a send from → to would close into
// a loop (empty when there is none).
std::vector<ObjectId> cyclePath(const Song& s, ObjectId from, ObjectId to);

enum class RouteMode {
  Send,           // add a normal send (keeps the others)
  Only,           // the destination becomes the only route
  Sidechain,      // add a sidechain-only route (feeds the destination's sidechain input)
  SidechainOnly,  // sidechain to the destination and drop the other sidechain routes
};
// Routes every source to every destination (pairs with source == destination
// are skipped). One undo step. Returns how many routes were made; `err`
// receives the first refusal.
int route(Document& d, const std::vector<ObjectId>& from, const std::vector<ObjectId>& to, RouteMode mode,
          std::string* err = nullptr);
// New track after the last of `tracks`, which then route only to it; the
// submix sends to the master. Returns its id (kNoId when nothing was routed).
ObjectId createSubmix(Document& d, const std::vector<ObjectId>& tracks, const std::string& name);
// The tracks route to the master only again. Returns how many changed.
int resetRouting(Document& d, const std::vector<ObjectId>& tracks);
// Removes every send into `dest` (never the master: returns 0); sources left
// without a route go to the master. Returns how many tracks changed.
int unrouteIncoming(Document& d, ObjectId dest);
// Pre-fader (after effects and EQ, before volume / pan) or post-fader send.
bool setSendPreFader(Document& d, ObjectId from, ObjectId to, bool pre);
// Destinations of a track's sends / tracks that send to it, in mixer order.
std::vector<ObjectId> routedFrom(const Song& s, ObjectId track);
std::vector<ObjectId> routedTo(const Song& s, ObjectId track);
// Send level arriving at `to` from `from` for a signal of gain 1 at `from`'s
// input: the post-fader send includes the source's fader and pan law
// (left channel), a pre-fader send does not. 0 when not routed.
double sendGain(const Song& s, ObjectId from, ObjectId to);

// Routing overview: column per mixer index (sources left, master right) by
// longest path, and every route.
struct RouteEdge {
  int from = 0, to = 0;  // mixer indices
  float level = 1;
  bool sidechain = false, preFader = false;
};
struct RouteLayout {
  std::vector<int> column;  // per mixer index
  int columns = 1;
  std::vector<RouteEdge> edges;
};
RouteLayout routeLayout(const Song& s);

// ------------------------------------------------------------------ strip ----
inline bool isMono(const MixerTrack& t) { return t.stereoSep <= -1.0f; }
// Mono: stereo separation fully merged; stereo: separation back to 0.
bool setMono(Document& d, const std::vector<ObjectId>& tracks, bool mono);
bool setSwap(Document& d, const std::vector<ObjectId>& tracks, bool swap);
bool setPolarity(Document& d, const std::vector<ObjectId>& tracks, bool invert);
// Disabled tracks pass their input straight through (no effects, EQ, fader).
bool setDisabled(Document& d, const std::vector<ObjectId>& tracks, bool disabled);
bool setThreaded(Document& d, const std::vector<ObjectId>& tracks, bool threaded);
// Manual latency (samples, clamped to 0..kMaxManualLatency).
bool setLatencyOffset(Document& d, ObjectId track, int samples);
int resetManualLatency(Document& d);  // returns how many tracks changed
int latencyFromMs(double ms, double sampleRate);
int latencyFromBeats(double beats, double tempo, double sampleRate);
// Volume, pan, separation, mute, solo, polarity, swap, disable, latency,
// threading and strip EQ back to defaults (name, colour, effects, routing stay).
int resetTracks(Document& d, const std::vector<ObjectId>& tracks);

// -------------------------------------------------- layout, groups, looks ----
bool setDock(Document& d, const std::vector<ObjectId>& tracks, MixerDock dock);
bool setIcon(Document& d, const std::vector<ObjectId>& tracks, int icon);
bool setColor(Document& d, const std::vector<ObjectId>& tracks, Rgb color);
// Blends from the first to the last track's colour (mixer order, >= 3 tracks).
bool gradientColor(Document& d, const std::vector<ObjectId>& tracks);
// Puts the tracks in a group ("" removes them from their group).
bool setGroup(Document& d, const std::vector<ObjectId>& tracks, const std::string& group);
std::vector<std::string> groupNames(const Song& s);  // in mixer order of first member
std::vector<ObjectId> groupMembers(const Song& s, const std::string& group);
// A group name not used yet ("Group 1", "Group 2", …).
std::string newGroupName(const Song& s);
// Strip order on screen: master, left-docked, middle, right-docked inserts
// (mixer indices).
std::vector<size_t> displayOrder(const Song& s);
// Sorts track ids into mixer order (unknown ids dropped).
std::vector<ObjectId> inMixerOrder(const Song& s, const std::set<ObjectId>& tracks);

// ----------------------------------------------------------------- tracks ----
// Inserts `count` tracks at `index` (1..size). Returns their ids.
std::vector<ObjectId> insertTracks(Document& d, int index, int count);
// Adds tracks at the end until there are `inserts` insert tracks. Returns how many were added.
int growTo(Document& d, int inserts);

// ------------------------------------------------------------------ slots ----
// Moves (or copies) an effect to another slot, on the same or another track.
// The target slot must be free, or the effects from it down shift by one
// (refused when the last slot is taken). Controller links follow a moved
// effect. One undo step.
bool moveSlotTo(Document& d, ObjectId fromTrack, int fromSlot, ObjectId toTrack, int toSlot, bool copy = false);
// Q-8: moves the effect in `slot` by `steps` slots (negative = up), like the
// reference's mouse wheel over an effect slot: it trades places with the
// slots it passes (empty ones included), clamped to the slot range. Mix,
// enable and controller links travel with it (Document::moveSlot); sends,
// sidechains and delay compensation belong to the track and stay. Returns
// the slot it ends in (== slot when nothing moved). One undo step.
int stepSlot(Document& d, ObjectId track, int slot, int steps);

// ------------------------------------------------------------ track state ----
// Mixer track state file: everything but identity and routing (name, colour,
// icon, levels, switches, effects, EQ, latency, threading).
inline constexpr const char* kTrackStateTag = "gridline-mixer-track";
inline constexpr const char* kTrackStateExt = ".gltrack";
std::string trackStateText(const MixerTrack& t);
bool applyTrackState(Document& d, ObjectId track, const std::string& text, std::string* err = nullptr);
bool saveTrackState(const std::string& path, const MixerTrack& t, std::string* err = nullptr);
bool loadTrackState(Document& d, ObjectId track, const std::string& path, std::string* err = nullptr);

// ------------------------------------------------------------------ stems ----
// A copy of the song whose master output is `track`'s output: the track sends
// only to a neutral master, tracks feeding it (sends, sidechains, send
// effects) keep working, everything else is muted. The master's own stem is
// the song itself.
Song stemSong(const Song& s, ObjectId track);
struct StemJob {
  ObjectId track = kNoId;
  std::string name;  // display name
  std::string file;  // dir/base_NN_name.wav (unique, filesystem-safe)
};
std::vector<StemJob> stemJobs(const Song& s, const std::vector<ObjectId>& tracks, const std::string& dir,
                              const std::string& base);

}  // namespace gl::mixops
