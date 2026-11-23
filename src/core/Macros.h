// Gridline - K-245: macro knobs for every plugin.
//
// Every channel and every mixer track owns eight macro knobs (LayerMacro, Song.h): the channel's on
// its window's Misc page, the mixer track's in the windows of its effects. A right-click on any plugin
// parameter (built-in or installed) links it to one of them: the link is an internal controller link
// (ModLink with sourceSlot kLayerMacroSlot, sourceOutput = the macro), so each link has its own range
// (start and end, either way round), mapping curve and smoothing, is saved with the project, undone in
// one step and synced to collaborators like any other link. A macro is itself a parameter
// ({owner, kLayerMacroBase + k}): it can be automated, linked to a MIDI control or to a controller,
// and an assigned macro shows in every automation list (K-246).
//
// The layer editor's macros (K-169) are the same knobs on a layer channel.
#pragma once
#include <string>
#include <vector>
#include "Params.h"

namespace gl {
class Document;
}

namespace gl::macro {

// True when `pid` is a macro knob (any owner).
bool isMacroParam(ParamId pid);
// The macro knob k of a channel or mixer track (nullptr: no such owner or k out of range).
const LayerMacro* find(const Song& s, ObjectId owner, int k);
LayerMacro* find(Song& s, ObjectId owner, int k);
// True when `owner` is a channel or a mixer track (it has macro knobs).
bool hasMacros(const Song& s, ObjectId owner);
// "Macro 3" or the name given to it.
std::string name(const Song& s, ObjectId owner, int k);
// The macro set a parameter's own wrapper shows: the channel for a channel parameter (its instrument,
// note processors, strip), the mixer track for an effect or strip parameter. kNoId for song parameters.
ObjectId ownerFor(const Song& s, ParamId target);
// The links macro k of `owner` drives, in the song's order.
std::vector<const ModLink*> links(const Song& s, ObjectId owner, int k);
// The macro driving `target` (false: none). owner / k receive it.
bool linkedMacro(const Song& s, ParamId target, ObjectId* owner, int* k);
// Assigned: the macro drives at least one parameter or has a name of its own.
bool assigned(const Song& s, ObjectId owner, int k);
// Every assigned macro of the song (channels in rack order, then mixer tracks), as parameters (K-246).
std::vector<ParamId> assignedMacros(const Song& s);
// The first macro of `owner` that drives nothing (-1: all eight are in use).
int firstFree(const Song& s, ObjectId owner);
// True when linking `target` to macro k of `owner` would make a loop (a macro driving itself,
// directly or through other macros).
bool wouldLoop(const Song& s, ObjectId owner, int k, ParamId target);
// The range a new link starts with: from the parameter's current value (normalised) up to the top
// (down to the bottom when it already sits at the top), so a macro at 0 leaves the sound as it was.
void defaultRange(const Song& s, ParamId target, float& min, float& max);

// Links `target` to macro k of `owner` (replacing any controller or macro link that drives it), with
// the default range. One undo step. False: not a parameter, not a macro, or it would loop.
bool link(Document& d, ParamId target, ObjectId owner, int k);
// Removes the macro link that drives `target` (false: it has none).
bool unlink(Document& d, ParamId target);
// Removes every link of macro k (one undo step).
bool clear(Document& d, ObjectId owner, int k);
// Names macro k (up to 24 characters; empty: "Macro N"). One undo step.
bool rename(Document& d, ObjectId owner, int k, const std::string& name);

}  // namespace gl::macro
