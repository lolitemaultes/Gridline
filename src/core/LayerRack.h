// Gridline — K-169: building split and layered sounds on a layer channel. Undoable edits of the
// layer's zones, children and macros, and layer presets (".gllayer" files): the layer with its
// children (instruments, note processors, strips), the children's own mixer inserts, the zones and
// the macro links, loaded back as new channels.
//
//   <preset root>/layers/<folder…>/<name>.gllayer
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "Json.h"
#include "Params.h"
#include "Presets.h"
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::layer {

// ---- edits (each one undo step; false when nothing changes) ----
// The child's zone: a full zone removes the entry. `mergeKey` merges drags into one step.
bool setZone(Document& d, ObjectId layer, const LayerZone& z, uint64_t mergeKey = 0);
// The zone a child plays (a full zone when it has none).
LayerZone zoneOf(const Channel& layer, ObjectId child);
// Equal key parts over the range the layer's notes use (else C4..B6), the outer parts reaching
// the keyboard ends; `fade` keys of crossfade between neighbours.
bool splitKeys(Document& d, ObjectId layer, int fade = 0);
// Equal velocity bands (soft children first).
bool splitVelocity(Document& d, ObjectId layer, int fade = 0);
bool resetZones(Document& d, ObjectId layer);  // every child: the whole keyboard, all velocities
bool setMode(Document& d, ObjectId layer, int mode);
bool addChild(Document& d, ObjectId layer, ObjectId child);
bool removeChild(Document& d, ObjectId layer, ObjectId child);
bool moveChild(Document& d, ObjectId layer, int from, int to);  // reorder (indices into Channel::layer)
bool renameMacro(Document& d, ObjectId layer, int macro, const std::string& name);
// Links parameter `target` to the macro (replacing any link that drives it). min / max: the range.
bool mapMacro(Document& d, ObjectId layer, int macro, ParamId target, float min = 0, float max = 1);
// The links a macro drives, in the song's order.
std::vector<const ModLink*> macroLinks(const Song& s, ObjectId layer, int macro);
// Parameters a macro can drive: the children's instruments, note processors, strips (volume, pan,
// pitch) and the effects of the inserts they play into alone.
std::vector<ParamId> macroTargets(const Song& s, ObjectId layer);

// ---- presets ----
std::string presetFolder(const std::string& presetRoot);  // <root>/layers
bool isPresetFile(const std::string& path);                 // ".gllayer"
std::vector<PresetEntry> listPresets(const std::string& presetRoot);
// The layer as a preset document. `leftOut` (optional) gets the names of children whose insert is
// shared with other channels (their effects are not saved).
Json presetToJson(const Song& s, ObjectId layer, std::vector<std::string>* leftOut = nullptr);
bool savePreset(const std::string& presetRoot, const Song& s, ObjectId layer, const std::string& relName,
                std::string* err = nullptr);
bool readPreset(const std::string& file, Json& out, std::string* err = nullptr);
// Adds the preset's channels (children first, then the layer) at channel index `at` (-1: the end),
// the saved inserts on free mixer tracks, and its macro links, as one undo step. Returns the new
// layer (kNoId on a bad document).
ObjectId loadPreset(Document& d, const Json& preset, int at = -1, std::string* err = nullptr);
// The built-in layers (name, document), made from Gridline's own instruments.
std::vector<std::pair<std::string, Json>> factoryPresets();

}  // namespace gl::layer
