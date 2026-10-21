// Gridline — FLP export: stand-ins for Gridline-only plugins (J-80).
//
// A Gridline built-in that has no FL plugin of its own (or whose instance runs
// a Gridline-own engine the FL plugin does not reproduce) is not written as a
// silent placeholder. Before the project is encoded, the export works on a copy
// of the song in which each such plugin is replaced by its stand-in:
//   - mapped: an FL plugin that is close (Gridline's chorus → Fruity Chorus,
//     delay → Fruity Delay 3, keys → FL Keys, distortion → Fruity Fast Dist,
//     …) with the settings translated to it as well as they carry;
//   - rendered: an instrument without a close FL plugin is played offline by
//     the render engine, note by note, at the notes (and audibly different
//     velocities) the song uses; the takes are written as WAV files in a folder
//     next to the exported project, and the channel becomes an FL sampler
//     channel (one used note) or a DirectWave program mapping the takes. The
//     channel keeps its notes, mixer routing, volume and pan;
//   - neutral: no FL plugin carries it and it does not shape the sound the way
//     a plugin slot can (documented reason per type).
// Hosted third-party plugins and plugins with an FL origin are never touched.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "../Song.h"
#include "FlpExport.h"

namespace gl::flp {

// What the export does with a Gridline built-in type (catalogue view, for the
// report and the tests).
enum class StandInKind {
  Native,    // an FL plugin of its own (encoders), nothing to do
  Mapped,    // written as a close FL plugin, settings translated
  Rendered,  // instrument rendered to samples (sampler channel / DirectWave)
  Neutral,   // no FL counterpart; neutral slot or nothing (reason given)
};
struct StandInPolicy {
  StandInKind kind = StandInKind::Native;
  std::string flName;  // Mapped: the FL plugin it becomes
  std::string reason;  // Neutral: why there is no stand-in
};
// The policy for an instance of `type` whose own encoding would be a
// placeholder (instances an FL plugin carries natively are Native).
StandInPolicy standInPolicy(const std::string& type);

// One replaced plugin.
struct StandIn {
  ObjectId owner = kNoId;  // channel or mixer track
  int slot = -1;           // -1: the channel's instrument
  std::string where;       // "channel 'Kick'", "insert 3 slot 2"
  std::string glType;      // the Gridline type replaced
  std::string action;      // "mapped", "rendered", "neutral", "unused" (no notes to render)
  std::string flName;      // FL plugin written ("" = sampler channel / none)
  std::string note;        // what was done / approximated
  std::vector<std::string> files;  // rendered samples
};

struct StandInOptions {
  // Folder the rendered samples go to (created when needed). Empty: nothing
  // is rendered (instruments that need it stay placeholders).
  std::string sampleDir;
  int sampleRate = 44100;
  std::function<void(double)> progress;  // 0..1
  const std::atomic<bool>* cancel = nullptr;
};

struct StandInResult {
  std::vector<StandIn> entries;
  std::vector<std::string> files;  // every sample written
  bool cancelled = false;
  std::string error;
  const StandIn* find(ObjectId owner, int slot) const {
    for (const auto& e : entries)
      if (e.owner == owner && e.slot == slot) return &e;
    return nullptr;
  }
};

// Replaces the Gridline-only plugins of `song` by their stand-ins (see above).
// `eo` gives the origin / encoder context. False when cancelled or a render
// failed (r.error); the song is then partly converted and must not be used.
bool applyStandIns(Song& song, const ExportOptions& eo, const StandInOptions& o, StandInResult& r);

// The notes (pitch, velocity 1..127) a channel plays anywhere in the song,
// with the longest note in seconds.
struct UsedNotes {
  std::map<int, std::vector<int>> velocities;  // pitch → sorted distinct velocities
  double longestSeconds = 0;
  bool empty() const { return velocities.empty(); }
};
UsedNotes usedNotes(const Song& song, ObjectId channel);

}  // namespace gl::flp
