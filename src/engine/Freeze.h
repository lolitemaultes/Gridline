// Gridline - K-87: freezing and hibernating channels and mixer tracks.
//
// A freeze renders a unit's sound once into a file and unloads its plugins; the file plays in
// their place until the unit is unfrozen:
//   channel      the instrument, its note processors and channel envelopes, before the channel's
//                volume and pan (which stay live), with its notes in the current arrangement;
//   mixer track  everything that feeds it through its effects, before its fader (volume, pan,
//                stereo separation, strip EQ, mute and every send stay live). Channels and tracks
//                that play only into a frozen track ("covered") are unloaded too.
// Hibernate unloads a unit's plugins without a render; the app wakes it when it is needed.
//
// Nothing here edits the unit's own state: Channel::freeze / MixerTrack::freeze hold the render
// beside it, and the song the engine plays is a "play view" (playView) in which unloaded plugins
// are left out. The fingerprint of what a render played tells when a freeze is out of date.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include "../core/Song.h"
#include "Program.h"
#include "Render.h"

namespace gl::freeze {

// A channel or a mixer track.
struct Unit {
  bool mixer = false;
  ObjectId id = kNoId;
  bool operator<(const Unit& o) const { return mixer != o.mixer ? mixer < o.mixer : id < o.id; }
  bool operator==(const Unit& o) const { return mixer == o.mixer && id == o.id; }
};
inline Unit channelUnit(ObjectId id) { return {false, id}; }
inline Unit mixerUnit(ObjectId id) { return {true, id}; }

const FreezeRecord* record(const Song& s, Unit u);
bool frozen(const Song& s, Unit u);
bool hibernating(const Song& s, Unit u);
// Display name ("Lead", "Insert 3").
std::string unitName(const Song& s, Unit u);

// Why the unit cannot be frozen / hibernate ("" = it can).
std::string whyNotFreeze(const Song& s, Unit u);
std::string whyNotHibernate(const Song& s, Unit u);

// Channels and mixer tracks that play only into `track` (directly or through each other), so a
// freeze of `track` holds all of their sound. `track` itself is not in the sets.
struct Cover {
  std::set<ObjectId> channels, mixers;
};
Cover covered(const Song& s, ObjectId track);
// The frozen mixer track whose freeze covers the unit (kNoId = none).
ObjectId coveringTrack(const Song& s, Unit u);

// The song a freeze render plays: only what the unit's sound depends on, the unit's live strip
// neutral (unity, centre, no sends) and routed straight to a neutral master.
Song renderSong(const Song& s, Unit u);
// Before the unit's own strip automation is taken out (renderSong does that).
Song renderSongWithStrip(const Song& s, Unit u);
// The plugins a hibernating or frozen unit loads when it wakes (ProgramBuilder::preload).
std::vector<ProgramBuilder::PreloadItem> wakeItems(const Song& s, Unit u);
// What a render of the unit would play (renderSong without names, colours and other display
// state), as a SHA-256 hex string. A frozen unit whose record holds another value is out of date.
std::string fingerprint(const Song& s, Unit u);
// The song length a freeze renders (ticks, from the song start); 0 = nothing to render.
Tick renderEnd(const Song& s);

// ---- what plays how (the app's program builds) ----
struct Plan {
  std::map<ObjectId, std::string> channelFiles, mixerFiles;  // frozen units playing their file
  std::set<ObjectId> unloadChannels, unloadMixers;            // plugins left out (frozen, covered, asleep)
  std::set<ObjectId> asleepMixers;                            // hibernating mixer tracks (silent)
  bool empty() const { return unloadChannels.empty() && unloadMixers.empty(); }
  bool operator==(const Plan& o) const {
    return channelFiles == o.channelFiles && mixerFiles == o.mixerFiles && unloadChannels == o.unloadChannels &&
           unloadMixers == o.unloadMixers && asleepMixers == o.asleepMixers;
  }
};
// `live`: frozen units that play live now (out of date, file missing, pattern mode, played by
// hand); `awake`: hibernating units awake now.
Plan makePlan(const Song& s, const std::set<Unit>& live = {}, const std::set<Unit>& awake = {});
// The song with the plan's plugins left out: an unloaded channel holds a silent sampler, an
// unloaded mixer track empty slots, sends from live tracks into a frozen track are dropped (they
// are in its render) and a sleeping mixer track is muted.
Song playView(const Song& s, const Plan& p);

// Up to date: frozen, with a record whose fingerprint is the unit's now.
bool upToDate(const Song& s, Unit u);
// The song a render or an export plays: frozen units that are up to date and whose file loads
// play their render (decoded here, blocking, into `out`); the rest plays live. `live` gets the
// frozen units left live.
Song exportView(const Song& s, FrozenAudio& out, std::set<Unit>* live = nullptr);

// ---- render ----
struct RenderControl {
  std::function<void(double)> progress;     // 0..1 (rendering thread)
  const std::atomic<bool>* cancel = nullptr;
};
// Renders the unit into `<dir>/<name> <fingerprint>.wav` (32-bit float, `sampleRate`) and fills
// `out` (not the song). An existing file with the same name is reused (undo / redo, refreeze of
// the same content). False with a reason when there is nothing to render or the write failed.
bool render(const Song& s, Unit u, int sampleRate, const std::string& dir, FreezeRecord& out, std::string* err = nullptr,
            const RenderControl* ctl = nullptr);

// ---- edits on a Song (the app wraps them in one undo step each) ----
bool setRecord(Song& s, Unit u, const FreezeRecord& r);  // freeze with a finished render
bool unfreeze(Song& s, Unit u);
bool setHibernate(Song& s, Unit u, bool on);

}  // namespace gl::freeze
