// Gridline — J-27 menu parity sweep: the menu entries added to the main menu
// and the context menus (operations in core/MenuParity, engine/NoteFxBurn).
// Each function draws a block of items inside an open menu.
#pragma once
#include <string>
#include <vector>
#include "../core/Params.h"
#include "../core/Remote.h"
#include "../core/Types.h"

namespace gl {

class App;

namespace menuparity {

// ---- main menu ----
void fileItems(App& a);     // File: save to a new project folder
void exportItems(App& a);   // File › Export: render again with the last settings
void addItems(App& a);      // Add: automation clip for the last tweaked control
void viewItems(App& a);     // View: project picker (browser), played chord hint
void optionsItems(App& a);  // Options: MIDI master sync output
void toolsItems(App& a);    // Tools: last tweaked controls, score logger, macros

// ---- context menus ----
void channelItems(App& a, ObjectId channel);                    // channel menu
void rackItems(App& a, const std::vector<ObjectId>& selection);  // channel rack menu
void pianoEditItems(App& a);                                    // piano roll › Edit
void pianoToolItems(App& a);                                    // piano roll › Tools
void pianoSelectItems(App& a);                                  // piano roll › Select
void pianoViewItems(App& a);                                    // piano roll › View
void knobItems(App& a, ParamId pid);                            // knob menu (tempo presets)

// ---- top bar ----
std::string positionText(App& a, double tick);  // song position in the chosen time format
void timeFormatMenu(App& a);                    // right-click on the song position
void stopMenuItems(App& a);                     // right-click on the stop button
void stopPressed(App& a);                       // the stop button (remember seek time; J-109: twice = panic)
void panicPressed(App& a);                      // J-109: stop and silence everything now
void transportTick(App& a);                     // per frame: playback start position

// ---- MIDI input ----
void logMidi(App& a, const MidiMessage& m);  // score logger

}  // namespace menuparity
}  // namespace gl
