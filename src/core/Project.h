// Gridline — project file (JSON) <-> Song (PJ-01, PJ-02).
//
// Schema history (older files are migrated on load; saves always write the latest):
//   v1  index-based references, fixed 96 PPQ, title only           (Gridline 0.1)
//   v2  stable object IDs, per-project PPQ, song info, nextId       (Gridline 0.2)
//   v3  plugin states, mixer (slots, sends), playlist tracks, colours (Gridline 0.3)
//   v4  MIDI controller links                                       (Gridline 0.4)
//   v5  internal controller links, channel note processors, sidechain sends (Gridline 0.5)
#pragma once
#include <string>
#include "Json.h"
#include "Song.h"

namespace gl {

inline constexpr int kProjectSchemaVersion = 8;  // v7 (J-98): gl.bitcrush Bits law; v8 (J-104): Downsample -> Rate (Hz)
inline constexpr const char* kProjectFormatTag = "gridline-project";

bool loadProject(const std::string& path, Song& out, std::string* err = nullptr);
// Atomic: writes `<path>.tmp` then renames over `path`, so a crash never
// leaves a half-written project.
bool saveProject(const std::string& path, const Song& song, std::string* err = nullptr);
std::string projectToText(const Song& song);
// withFlOrigin = false leaves out Song::flOrigin (J-111: collaboration syncs it separately).
Json songToJson(const Song& song, bool withFlOrigin = true);
// Accepts any known schema version (migrating it) and validates references.
bool songFromJson(const Json& j, Song& out, std::string* err = nullptr);
// Upgrades a parsed project document to the latest schema in place.
bool migrateProjectJson(Json& j, std::string* err = nullptr);

// J-111: one element of a project document, its references checked against
// `s` (collaboration applies remote edits element by element). The same code
// a whole-project load uses. patternFromJson: false for bad dimensions.
// clipFromJson: 0 ok, 1 dropped (its audio / automation source is missing),
// -2 its pattern is missing, -3 bad position.
void channelFromJson(const Json& jc, Channel& c, const Song& s);
bool patternFromJson(const Json& jp, Pattern& p, const Song& s);
int clipFromJson(const Json& jc, Clip& c, const Song& s);
void automationClipFromJson(const Json& ja, AutomationClip& a);

// One plugin state as the project document stores it ("plugin" objects).
Json pluginStateToJson(const PluginState& p);
PluginState pluginStateFromJson(const Json& j);

// One mixer track (mixer track state files, J-12). FromJson fills everything
// but the id; sends keep the ids found in the JSON (callers remap them).
Json mixerTrackToJson(const MixerTrack& m);
void mixerTrackFromJson(const Json& j, MixerTrack& m);

// Writes `text` to `path` atomically (temp file + rename). Nothing is left
// behind on failure, and an existing read-only file is not replaced.
bool writeFileAtomic(const std::string& path, const std::string& text, std::string* err = nullptr);
// J-78: why writing `path` failed (errno), in words the user can act on:
// "cannot write Song.flp: no permission to write in /x (read-only folder or
// disk). Choose another folder."
std::string fileWriteError(const std::string& path, int errnum);
// False (with the reason) when `path` is a folder or an existing read-only file.
bool checkWritableTarget(const std::string& path, std::string* err = nullptr);

}  // namespace gl
