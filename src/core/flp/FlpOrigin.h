// Gridline — FL origin: the original events of imported FL objects, kept in
// Song::flOrigin for the FL exporter (J-34).
//
// Keys (ObjectIds are Gridline ids):
//   proj              project events (header, settings, window state, …)
//   links             MIDI remote-control links (226) as they were imported
//   modlinks          internal controller links (227 with an output source)
//   ch/<channel>      a rack channel's events (FL channel types 0–4)
//   auto/<clip>       an automation channel's events + its target links (227)
//   pat/<pattern>     a pattern's events
//   arr/<index>       an arrangement's events except its playlist (233)
//   clip/<clip>       the playlist record (233) a clip came from
//   mx/<track>        a mixer insert's events (+ its 225 parameter records)
//   mxcur             FL's "current" insert (not imported)
//   mxfl/<index>      an FL insert the import trimmed (unused)
//   mxp               225 records that belong to no insert
//   plug/<owner>/<slot>  a plugin's events (slot -1 = the channel's instrument)
// Every list may carry pseudo events Gridline adds (ids FL does not use):
//   kFlIndex     u32 the object's FL index (channel, pattern, insert, arrangement)
//   kFingerprint the Gridline object's fingerprint right after the import:
//                the exporter writes an object back verbatim while its
//                current fingerprint is the same
//   kGlType      the Gridline plugin type the plugin was imported as
#pragma once
#include <map>
#include <string>
#include "../Song.h"
#include "FlpWriter.h"

namespace gl::flp {

inline constexpr uint8_t kFlIndex = 250;      // pseudo event: u32 FL index
inline constexpr uint8_t kFingerprint = 249;  // pseudo event: u64 fingerprint
inline constexpr uint8_t kGlType = 248;       // pseudo event: UTF-8 Gridline type

std::string originKey(const char* kind, ObjectId id);
std::string pluginKey(ObjectId owner, int slot);

// FL index of each Gridline object, as imported or as being exported.
struct IndexMap {
  std::map<ObjectId, int> channel;     // rack channel / audio-clip channel → FL channel index
  std::map<ObjectId, int> automation;  // automation clip → FL channel index
  std::map<ObjectId, int> pattern;     // pattern → FL pattern index (1-based)
  std::map<ObjectId, int> insert;      // mixer track → FL insert index (0 = master)
  int ch(ObjectId id) const {
    auto it = channel.find(id);
    return it == channel.end() ? -1 : it->second;
  }
  int aut(ObjectId id) const {
    auto it = automation.find(id);
    return it == automation.end() ? -1 : it->second;
  }
  int pat(ObjectId id) const {
    auto it = pattern.find(id);
    return it == pattern.end() ? -1 : it->second;
  }
  int ins(ObjectId id) const {
    auto it = insert.find(id);
    return it == insert.end() ? -1 : it->second;
  }
  // FL owner of a parameter target: channel → its index, mixer track →
  // 0x10000 + insert, song → -2, else -1.
  int owner(ObjectId id) const;
};

// Fingerprints of the parts of the song the exporter writes per object. They
// include references as FL indices (so renumbering counts as a change).
uint64_t fingerprintChannel(const Channel& c, const IndexMap& m);
uint64_t fingerprintPlugin(const PluginState& p);
uint64_t fingerprintPattern(const Pattern& p, const Song& s, const IndexMap& m);
uint64_t fingerprintInsert(const MixerTrack& t, const IndexMap& m);
uint64_t fingerprintAutomation(const AutomationClip& a, const IndexMap& m);
uint64_t fingerprintClip(const Clip& c, const IndexMap& m);
uint64_t fingerprintArrangement(const std::vector<PlaylistTrack>& tracks, const std::vector<Marker>& markers, const LiveSettings& live,
                                const std::string& name);
uint64_t fingerprintProject(const Song& s);
uint64_t fingerprintLinks(const Song& s, const IndexMap& m);
uint64_t fingerprintModLinks(const Song& s, const IndexMap& m);

// Reads an origin entry (empty list when missing).
EventList originEvents(const Song& s, const std::string& key);
// Helpers on an origin list.
int originIndex(const EventList& ev);           // kFlIndex value, -1 when none
bool originFingerprint(const EventList& ev, uint64_t& out);
std::string originGlType(const EventList& ev);
// The list without Gridline's pseudo events.
EventList stripPseudo(const EventList& ev);

struct Context;
// Called at the end of importFlp: fills song.flOrigin from the import context.
void captureOrigin(Context& c);

}  // namespace gl::flp
