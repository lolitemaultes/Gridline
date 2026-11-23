// Gridline - k078-flkey: Novation FLkey keyboards in the app (core/surfaces/FlKey.h).
// A keyboard's DAW port becomes a control surface on its own, found by its port name, or by the
// controller type picked in Settings > MIDI. Several keyboards at once; unplug, replug and Rescan
// devices are followed; quitting returns every keyboard to standalone.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "../core/Remote.h"

namespace gl {

class App;

// Controller types (id, label) for Settings > MIDI, one per model.
const std::vector<std::pair<std::string, std::string>>& flkeyControllerTypes();
bool isFlkeyType(const std::string& type);
// The model an input named so is taken for with the Automatic type (its DAW port), else empty.
std::string flkeyAutoName(const std::string& input);
// Per frame: builds, keeps and drops the surfaces; sends their feedback (throttled).
void flkeyTick(App& a);
// A message from a MIDI input: true when an FLkey surface took it.
bool flkeyMidi(App& a, const std::string& device, const MidiMessage& m);
bool flkeyOwns(const std::string& device);  // an FLkey surface runs this input (its DAW port)
// Before the app goes: every keyboard back to standalone.
void flkeyShutdown();
// Dev runs: one line per surface (model, port, output, pages, banks, pad colours).
std::vector<std::string> flkeyDescribe();

}  // namespace gl
