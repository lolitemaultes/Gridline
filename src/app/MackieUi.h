// Gridline - K-214: Mackie Control / HUI control surfaces (MIDI device controller types).
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "../core/Remote.h"

namespace gl {

class App;

// Controller types (id, label) for Settings > MIDI.
const std::vector<std::pair<std::string, std::string>>& mackieControllerTypes();
bool isMackieType(const std::string& type);
// Per frame: builds the surface from the configured devices, feedback and meters.
void mackieTick(App& a);
// A message from a device of the surface: true when the surface took it.
bool mackieMidi(App& a, const std::string& device, const MidiMessage& m);
bool mackieOwns(const std::string& device);  // a unit of the surface is this input

}  // namespace gl
