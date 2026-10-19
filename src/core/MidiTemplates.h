// Gridline — MIDI controller templates (J-17): what a known controller's
// buttons, knobs and faders do without any linking. Templates are JSON data
// files (assets/midi/controllers/*.json, plus user copies in the config
// folder that override them by id), matched to devices by name.
//
// File format:
//   { "name": "Device name", "match": ["substring", …] ("*" = any device),
//     "takeover": "jump" | "pickup" | "scale",
//     "mmc": true,                      // MIDI Machine Control sysex transport
//     "transport": { "play": {"cc": 41}, "stop": {"note": 93, "channel": 1}, … },
//     "knobs":  { "cc": [16, 17, …], "channel": 0, "encoder": "absolute" },
//     "faders": { "cc": [0, 1, …] } }
// Knobs drive the first N parameters of the focused plugin window ("focused
// window" mode); faders drive the volumes of mixer inserts 1..N. Transport
// keys: play, stop, record, songmode, metronome, countin, steprecord,
// looprecord, nextpattern, prevpattern, undo, redo, taptempo. Channels are
// 1..16 (0 or missing = any).
#pragma once
#include <string>
#include <vector>
#include "Remote.h"

namespace gl {

struct TemplateControl {
  RemoteSource source = RemoteSource::CC;
  int number = 0;   // CC / note number
  int channel = -1; // 0..15, -1 = any
  bool operator==(const TemplateControl& o) const {
    return source == o.source && number == o.number && channel == o.channel;
  }
};

struct ControllerTemplate {
  std::string id;    // file stem, unique
  std::string name;
  std::vector<std::string> match;  // case-insensitive name substrings; "*" = any device
  RemoteTakeover takeover = RemoteTakeover::Jump;
  RemoteEncoder knobEncoder = RemoteEncoder::Absolute;
  bool mmc = false;
  std::vector<std::pair<RemoteAction, TemplateControl>> transport;
  std::vector<TemplateControl> knobs;   // → focused plugin parameters 0..N-1
  std::vector<TemplateControl> faders;  // → mixer insert volumes 1..N
  std::string source;  // file it was loaded from ("" = built in)
};

bool parseControllerTemplate(const std::string& json, ControllerTemplate& out, std::string* err = nullptr);
std::string controllerTemplateJson(const ControllerTemplate& t);
// The fallback "Generic controller" (also shipped as generic.json).
ControllerTemplate genericTemplate();
// Loads every *.json in `dirs` in order; a later file with the same id replaces
// an earlier one (user folder after the shipped one). Always contains "generic".
std::vector<ControllerTemplate> loadControllerTemplates(const std::vector<std::string>& dirs,
                                                        std::vector<std::string>* errors = nullptr);
// Best template for a device: the longest matching substring wins, "*" only
// when nothing else matches. nullptr when no template matches at all.
const ControllerTemplate* matchControllerTemplate(const std::vector<ControllerTemplate>& all, const std::string& device);
const ControllerTemplate* findControllerTemplate(const std::vector<ControllerTemplate>& all, const std::string& id);

// What a message means for a template.
struct TemplateHit {
  enum Kind { None, Action, Knob, Fader } kind = None;
  RemoteAction action = RemoteAction::None;
  int index = 0;  // knob / fader number
};
TemplateHit templateLookup(const ControllerTemplate& t, const MidiMessage& m);
const char* templateActionKey(RemoteAction a);  // "play", "stop", …

// MIDI Machine Control (universal sysex F0 7F <id> 06 <cmd> F7) → action.
RemoteAction parseMmc(const std::vector<unsigned char>& sysex);

}  // namespace gl
