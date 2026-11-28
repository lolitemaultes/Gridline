// Gridline - K-435: a Tracker Sampler's sounds out of the project (the Tracker's instrument list menu):
// load one of the library's Tracker Sampler presets, save the instrument as a preset (its samples
// carried with it, core/Presets.h), save every Tracker Sampler of the song as presets at once (an
// imported module's sounds into the library), and write the instrument's samples as WAV files.
// Loading a preset is one undo step (and a collaboration op, as every plugin state change); saving
// and exporting write files only and change nothing in the project.
#pragma once
#include <string>
#include <vector>

#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::trackerfiles {

// The menu rows for `channel` (drawn inside the instrument list's open menu; nothing for an
// instrument that is not a Tracker Sampler, except "Save every instrument as a preset").
void menuItems(App& a, ObjectId channel);
// The library folder the song's presets go to ("<song title>", or "Tracker instruments").
std::string songPresetFolder(App& a);
// The work behind the rows (also the dev commands'). Each returns how many it saved or wrote.
bool savePreset(App& a, ObjectId channel, const std::string& name);
int saveAllPresets(App& a);
bool loadPreset(App& a, ObjectId channel, const std::string& file);
int exportSamples(App& a, ObjectId channel, const std::string& path);

// Dev (--dev-input "tracker editors files ..."): save <channel> <name> · all · load <channel> <name> ·
// export <channel> <path> · dump (each Tracker Sampler: its playable samples and its files).
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::trackerfiles
