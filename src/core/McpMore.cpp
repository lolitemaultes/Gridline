// Gridline - MCP tools, part two (K-281): generic access to every object of the project (inspect,
// patch), batches, discovery (help, find), the transport and selection, parameter paths, more
// editing tools (duplicate, notes, automation, mixer tracks and sends, Stage rooms,
// arrangements), and what only the running app can do (its commands, windows and settings).
//
// inspect / patch work on the project document itself (Project.h: the file format), so every
// field a project file can hold is reachable, including fields added later: an object is turned
// into its document form, changed there and read back with the same code that opens projects,
// then put in place as one Document edit. A field the file format does not know is reported as
// ignored, a value the loader corrects is reported as adjusted.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include "Arrangements.h"
#include "CommandPalette.h"
#include "Keymap.h"
#include "McpInternal.h"
#include "MenuOps.h"
#include "MixerOps.h"
#include "PluginBrand.h"
#include "Project.h"
#include "Settings.h"
#include "StagePick.h"
#include "StageRoomOps.h"

namespace gl::mcp::detail {

namespace {

// ============================================================ object paths ====
// "song", "channel/2", "pattern/1", "mixer/0", "track/3", "clip/<id>", "automation/1",
// "marker/1", "room/1", "arrangement/1", "link/1", "modlink/1", then optional keys / 1-based
// array positions inside the object ("channel/2/echo", "mixer/3/slots/1").
struct Kind {
  const char* name;
  const char* plural;   // the document's array ("" for song)
  const char* about;    // one line for inspect / help
  std::vector<std::pair<const char*, const char*>> fields;  // field -> meaning (help topic "objects")
};

const std::vector<Kind>& kinds() {
  static const std::vector<Kind> v = {
      {"song", "", "Song settings (the collections are their own paths: channel/N, pattern/N, mixer/N, ...)",
       {{"info", "title, author, genre, url, comments (strings), created, workTime (seconds)"},
        {"tempo", "BPM"},
        {"timeSigNum", "meter at the song start: numerator (meter changes later are markers)"},
        {"timeSigDen", "meter at the song start: denominator 2, 4, 8 or 16"},
        {"ppq", "ticks per beat (changing it rescales every position)"},
        {"swing", "global swing 0..1"},
        {"swingNotes", "swing moves piano-roll notes too (false: steps only)"},
        {"groove", "swing shape: grid, cycle, timing[], velocity[], accent, preset"},
        {"slidesByColour", "slide notes glide only notes of their own colour"},
        {"slidesVelocity", "slide notes also glide the velocity of the notes they slide (fade in, fade out, swell)"},
        {"masterPitch", "semitones"},
        {"autoCrossfades", "overlapping audio clips crossfade"},
        {"panLaw", "0 balance, 1 constant power, 2 Stage placement"},
        {"rackAutoLanes", "the rack's automation lanes for song and mixer parameters, kept across patterns: owner, index, name, color"},
        {"stage", "the first Stage room (use room/1)"},
        {"live", "areaEnd: clips starting before this tick are launched by hand"},
        {"channelGroups", "channel rack display group names"},
        {"patternGroups", "named pattern sets: name, patterns (pattern ids)"},
        {"flExportGridline", "how Gridline-only plugins are written in .flp exports: render, suite, suite+fallback"},
        {"trackReceipt", "Track Receipt fields (object)"}}},
      {"channel", "channels", "A channel rack channel: instrument, strip, routing and note settings",
       {{"name", "name"},
        {"color", "colour (\"#RRGGBB\" accepted)"},
        {"plugin", "instrument state: type, params[] (plugin units), data"},
        {"gain", "volume 0..1.25 (0.78 default)"},
        {"pan", "-1..1"},
        {"muted", "mute"},
        {"pitch", "semitones -24..24"},
        {"fine", "cents -200..200"},
        {"mixerTrack", "mixer track id it plays into (0 or absent: master)"},
        {"auxOuts", "mixer track ids of a multi-output instrument's extra outputs"},
        {"midiOutPort", "MIDI out port 0..255 of a hosted plugin (-1 none)"},
        {"midiInPort", "MIDI in port 0..255 (-1 none)"},
        {"noteFx", "note processors (plugin states, in order)"},
        {"swingMix", "share of the song swing 0..1"},
        {"timeShift", "beats 0..1 every note starts later"},
        {"polyphony", "maximum sounding notes (0 unlimited)"},
        {"mono", "one note at a time"},
        {"porta", "glide between notes"},
        {"portaTime", "glide time, seconds 0..2"},
        {"bendRange", "semitones a full pitch bend means to the instrument 1..48 (plugins without note tuning)"},
        {"expression", "per-note expression (MPE): pitch, pressure, timbre amounts"},
        {"cutGroup", "notes cut channels whose cutBy equals this (0 none)"},
        {"cutBy", "cut by notes of this group (0 none)"},
        {"echo", "note echo: count 0..10, feedback, time (beats), pan, pitch (cents), pingPong, modX, modY"},
        {"layer", "channel ids this layer plays"},
        {"layerMode", "0 all, 1 random, 2 cycle, 3 random without repeats"},
        {"layerZones", "per child: child id, keyLo, keyHi, fades, velLo, velHi, transpose, solo"},
        {"macros", "layer macro knobs: name, value 0..1"},
        {"group", "display group index (-1 none)"},
        {"keyLo", "lowest key that plays"},
        {"keyHi", "highest key that plays"},
        {"swingTruncate", "swung steps end at their unswung end"},
        {"automationLanes", "the rack's automation lanes for this channel's parameters, kept across patterns: index, name, color"},
        {"audioClip", "a playlist audio clip source"},
        {"icon", "track icon number"},
        {"flIcon", "icon number from an imported .flp, kept for export"},
        {"freeze", "frozen render (read only: freeze with the app's commands)"},
        {"hibernate", "plugins unloaded until needed"},
        {"keepRunning", "keeps running while silent (not rested)"}}},
      {"pattern", "patterns", "A pattern: steps and notes per channel (keyed by channel id), pattern automation, length",
       {{"name", "name"},
        {"color", "colour"},
        {"bars", "length in bars"},
        {"stepsPerBar", "step resolution"},
        {"meterNum", "own meter numerator (0: follows the song)"},
        {"meterDen", "own meter denominator"},
        {"loop", "a longer playlist clip repeats it"},
        {"steps", "channel id -> velocity per step (use set_steps)"},
        {"notes", "channel id -> notes in ticks (use set_notes / get_pattern: beats)"},
        {"stepProps", "channel id -> per-step pitch, pan, release, fine, modX, modY, shift"},
        {"stepLoop", "channel id -> lane length (loop) in the lane's own steps"},
        {"lanes", "channel id -> {grid: steps per bar of its own (absent: the pattern's), runOn: the lane length runs on through longer clips}"},
        {"automation", "pattern automation clips: owner, index, points [tick, value, tension, shape]"}}},
      {"mixer", "mixer", "A mixer track (mixer/0 is the master): strip, effects, sends, Stage place, recording",
       {{"name", "name"},
        {"color", "colour"},
        {"volume", "0..1.25 (0.8 unity)"},
        {"pan", "-1..1"},
        {"stereoSep", "-1 mono .. 1 wide"},
        {"depth", "Stage distance 0 front .. 1 back"},
        {"focus", "Stage focus -1 soft .. 1 present"},
        {"roomId", "the Stage room it stands in (room id)"},
        {"muted", "mute"},
        {"solo", "solo"},
        {"soloAlone", "solo without the tracks feeding it"},
        {"invertPolarity", "polarity"},
        {"swapLR", "swap left and right"},
        {"slots", "10 effect slots: plugin state, enabled, mix 0..1"},
        {"sends", "routes: to (mixer track id), level 0..1.25, sidechain, preFader, stageRoom"},
        {"eq", "strip EQ: gain (dB), freq (Hz), q per band (low, mid, high)"},
        {"input", "audio input channel (-1 none)"},
        {"inputStereo", "stereo input"},
        {"monitor", "input monitoring 0 auto, 1 on, 2 off"},
        {"armed", "armed for recording"},
        {"recPoint", "record from 0 input, 1 after effects, 2 after fader"},
        {"icon", "icon number"},
        {"disabled", "bypassed"},
        {"dock", "-1 left, 0 middle, 1 right"},
        {"latencyOffset", "manual latency in samples"},
        {"threaded", "may run on a worker thread"},
        {"group", "mixer group name"},
        {"separator", "gap before the strip"},
        {"locked", "locked strip"},
        {"freeze", "frozen render (read only)"},
        {"hibernate", "effects unloaded until needed"}}},
      {"track", "tracks", "A playlist track: name, colour, mute, height, live launching, folding, mode",
       {{"name", "name"},
        {"color", "colour"},
        {"muted", "mute"},
        {"height", "relative height"},
        {"launch", "live launch grid 0 now .. 6 clip length"},
        {"pad", "pad mode 0 trigger, 1 gate, 2 toggle"},
        {"follow", "follow action 0 loop, 1 once, 2 next, 3 next hold, 4 next stop, 5 random, 6 shuffle"},
        {"syncStart", "launch in phase on this grid (0 off)"},
        {"groupSize", "number of following tracks in its group"},
        {"folded", "group folded"},
        {"locked", "clips cannot be edited"},
        {"icon", "icon number"},
        {"mode", "0 unassigned, 1 audio, 2 instrument"},
        {"mixer", "linked mixer track id"},
        {"channel", "linked channel id"}}},
      {"clip", "playlist", "A playlist clip (addressed by its id): pattern, audio or automation, with its playback settings",
       {{"pattern", "pattern id (pattern clips)"},
        {"track", "playlist track, 0 = first"},
        {"start", "ticks"},
        {"length", "ticks (0: the content's length)"},
        {"kind", "0 pattern, 1 audio, 2 automation"},
        {"source", "audio: channel id; automation: automation clip id"},
        {"offset", "content ticks skipped at the start"},
        {"loop", "repeats its content"},
        {"loopStart", "repeated window start (ticks)"},
        {"loopLength", "repeated window length (ticks)"},
        {"muted", "mute"},
        {"audioStart", "audio: source seconds at the start"},
        {"audioEnd", "audio: source seconds where it stops"},
        {"fadeIn", "seconds"},
        {"fadeOut", "seconds"},
        {"gain", "linear gain"},
        {"pitch", "cents"},
        {"stretch", "time factor"},
        {"syncBpm", "follows the song tempo, natural speed at this tempo"},
        {"tempoLock", "locked to the tempo"},
        {"reverse", "plays backwards"},
        {"group", "clip group id"},
        {"warp", "warp markers"},
        {"playTranspose", "pattern clip: semitones applied as it plays"},
        {"playVelocity", "pattern clip: velocity factor"},
        {"playDelay", "pattern clip: delay"},
        {"playQuantize", "pattern clip: quantize"}}},
      {"automation", "automations", "An automation clip of the playlist: target parameter, range and points",
       {{"name", "name"},
        {"color", "colour"},
        {"owner", "target object id (with index: the parameter)"},
        {"index", "target parameter index"},
        {"min", "range start 0..1"},
        {"max", "range end 0..1"},
        {"loop", "repeat length in ticks (0 none)"},
        {"points", "[tick, value 0..1, tension -1..1, shape 0..12] each"},
        {"flCurves", "point shapes follow the exact curve laws"},
        {"events", "recorded events (step on their exact sample)"}}},
      {"marker", "markers", "A song marker: position, name, meter change, key, action",
       {{"pos", "ticks"},
        {"name", "name"},
        {"timeSigNum", "meter change numerator (0: plain marker)"},
        {"timeSigDen", "meter change denominator"},
        {"action", "0 none, 1 loop, 2 skip, 3 pause, 4 stop, 5 song loop"},
        {"keyRoot", "key marker root 0..11 (-1 none)"},
        {"keyScale", "key marker scale index"},
        {"meterForPatterns", "new patterns made in this meter section take its meter"}}},
      {"room", "stageRooms", "A Stage room (room/1 is the first): space, width, depth law",
       {{"name", "name"},
        {"color", "colour"},
        {"width", "stage width 0..1.5"},
        {"room", "the room's space track id"},
        {"depthStrength", "0 gentle .. 1 strong distance"},
        {"roomKind", "the space by name"},
        {"preDelay", "pre-delay follows distance"},
        {"early", "early reflections"},
        {"frontQuiet", "the front row sends less into the room"}}},
      {"arrangement", "arrangements", "A playlist arrangement (only its name here: switch with the arrangement tool)", {{"name", "name"}}},
      {"link", "links", "A MIDI / OSC controller link to a parameter or action",
       {{"cc", "controller, note or program number"},
        {"midiChannel", "0..15, -1 any"},
        {"owner", "target object id"},
        {"index", "target parameter index"},
        {"min", "normalized range start"},
        {"max", "normalized range end"},
        {"source", "0 CC, 1 note, 2 pitch bend, 3 channel pressure, 4 key pressure, 5 program, 6 OSC"},
        {"port", "input port (-1 any)"},
        {"curve", "mapping curve"},
        {"formula", "formula of input"},
        {"invert", "invert"},
        {"takeover", "0 jump, 1 pickup, 2 scale"},
        {"encoder", "0 absolute, 1..3 relative kinds"},
        {"sensitivity", "relative step factor"},
        {"button", "0 value, 1 momentary, 2 toggle, 3 increment"},
        {"action", "global action instead of a parameter (0 none)"},
        {"osc", "OSC address"}}},
      {"modlink", "modLinks", "An internal controller link: a controller plugin output drives a parameter",
       {{"sourceOwner", "channel or mixer track id of the controller"},
        {"sourceSlot", "-1 the channel's instrument, 0..9 a mixer slot"},
        {"sourceOutput", "controller output"},
        {"owner", "target object id"},
        {"index", "target parameter index"},
        {"min", "range start"},
        {"max", "range end"},
        {"curve", "mapping curve 0..9"},
        {"formula", "formula"},
        {"smoothMs", "smoothing"},
        {"enabled", "on"}}},
  };
  return v;
}

const Kind* findKind(const std::string& name) {
  for (const auto& k : kinds())
    if (name == k.name) return &k;
  return nullptr;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (!cur.empty()) v.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) v.push_back(cur);
  return v;
}

bool number(const std::string& s, long long& out) {
  if (s.empty() || s.size() > 18) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  out = std::stoll(s);
  return true;
}

const char* kPathHelp =
    "paths: song, channel/N, pattern/N, mixer/N (0 = master), track/N, clip/<clip id>, automation/N, marker/N, room/N, "
    "arrangement/N, link/N, modlink/N (numbers from 1 as on screen, except mixer and clip ids), optionally followed by keys "
    "inside the object (channel/2/echo)";

// Where an object lives in the project document.
struct Ref {
  const Kind* kind = nullptr;
  long long n = 0;     // the number in the path (clip: its id)
  size_t index = 0;    // position in the song's container
  ObjectId id = kNoId;
  std::vector<std::string> sub;  // keys inside the object
};

bool parseRef(const Song& s, const std::string& path, Ref& r, std::string& err) {
  const auto parts = split(path, '/');
  if (parts.empty()) {
    err = "empty path: " + std::string(kPathHelp);
    return false;
  }
  r.kind = findKind(parts[0]);
  if (!r.kind) {
    std::string p = parts[0];
    if (!p.empty() && p.back() == 's' && findKind(p.substr(0, p.size() - 1))) {
      err = "\"" + p + "\" is a list: name one, e.g. " + p.substr(0, p.size() - 1) + "/1 (" + kPathHelp + ")";
    } else {
      err = "unknown object \"" + p + "\": " + kPathHelp;
    }
    return false;
  }
  size_t next = 1;
  const std::string kn = r.kind->name;
  if (kn != "song") {
    if (parts.size() < 2 || !number(parts[1], r.n)) {
      err = kn + " needs a number: " + kn + "/1";
      return false;
    }
    next = 2;
    auto range = [&](size_t count, long long lo) {
      if (r.n < lo || r.n >= lo + static_cast<long long>(count)) {
        err = "no " + kn + " " + std::to_string(r.n) +
              (count == 0 ? " (there are none)" : " (" + std::to_string(lo) + ".." + std::to_string(lo + static_cast<long long>(count) - 1) + ")");
        return false;
      }
      r.index = static_cast<size_t>(r.n - lo);
      return true;
    };
    if (kn == "channel") {
      if (!range(s.channels.size(), 1)) return false;
      r.id = s.channels[r.index].id;
    } else if (kn == "pattern") {
      if (!range(s.patterns.size(), 1)) return false;
      r.id = s.patterns[r.index].id;
    } else if (kn == "mixer") {
      if (!range(s.mixer.size(), 0)) return false;
      r.id = s.mixer[r.index].id;
    } else if (kn == "track") {
      if (!range(s.tracks.size(), 1)) return false;
    } else if (kn == "clip") {
      const int i = s.clipIndex(static_cast<ObjectId>(r.n));
      if (i < 0) {
        err = "no clip with id " + std::to_string(r.n) + " (get_playlist lists the clip ids)";
        return false;
      }
      r.index = static_cast<size_t>(i);
      r.id = static_cast<ObjectId>(r.n);
    } else if (kn == "automation") {
      if (!range(s.automations.size(), 1)) return false;
      r.id = s.automations[r.index].id;
    } else if (kn == "marker") {
      if (!range(s.markers.size(), 1)) return false;
    } else if (kn == "room") {
      if (!range(1 + s.stageRooms.size(), 1)) return false;
    } else if (kn == "arrangement") {
      if (!range(static_cast<size_t>(std::max(1, arrangementCount(s))), 1)) return false;
    } else if (kn == "link") {
      if (!range(s.links.size(), 1)) return false;
    } else if (kn == "modlink") {
      if (!range(s.modLinks.size(), 1)) return false;
    }
  }
  r.sub.assign(parts.begin() + static_cast<long>(next), parts.end());
  return true;
}

// The object in the document (created when the document leaves it out: a first room, a single
// arrangement). nullptr when absent.
Json* locate(Json& doc, const Song& s, const Ref& r) {
  const std::string kn = r.kind->name;
  if (kn == "song") return &doc;
  if (kn == "room" && r.index == 0) {
    if (!doc.find("stage")) doc.object["stage"] = Json::makeObject();
    return &doc.object["stage"];
  }
  if (kn == "arrangement" && !doc.find("arrangements")) {
    Json a = Json::makeArray();
    a.array.push_back(jobj({{"name", jstr(arrangementName(s, 0))}}));
    doc.object["arrangements"] = a;
    doc.object["currentArrangement"] = jnum(0);
  }
  auto it = doc.object.find(r.kind->plural);
  if (it == doc.object.end() || !it->second.is(Json::Type::Array)) return nullptr;
  auto& arr = it->second.array;
  if (kn == "clip") {
    for (auto& c : arr)
      if (static_cast<long long>(c.num("id", -1)) == r.n) return &c;
    return nullptr;
  }
  const size_t i = kn == "room" ? r.index - 1 : r.index;
  return i < arr.size() ? &arr[i] : nullptr;
}

// Steps into the object along `sub` (keys, or 1-based positions in arrays). `create`: missing
// keys of objects become objects.
Json* descend(Json* j, const std::vector<std::string>& sub, bool create, std::string& err) {
  for (const auto& k : sub) {
    if (j->is(Json::Type::Object)) {
      auto it = j->object.find(k);
      if (it == j->object.end()) {
        if (!create) {
          err = "no field \"" + k + "\" here";
          return nullptr;
        }
        j = &j->object[k];
        continue;
      }
      j = &it->second;
    } else if (j->is(Json::Type::Array)) {
      long long n = 0;
      if (!number(k, n) || n < 1 || n > static_cast<long long>(j->array.size())) {
        err = "\"" + k + "\": give a position 1.." + std::to_string(j->array.size()) + " in this list";
        return nullptr;
      }
      j = &j->array[static_cast<size_t>(n - 1)];
    } else if (create && j->is(Json::Type::Null)) {
      *j = Json::makeObject();
      j = &j->object[k];
    } else {
      err = "\"" + k + "\": the value before it has no fields";
      return nullptr;
    }
  }
  return j;
}

// Keys of the song document that are collections (their own paths).
bool songCollection(const std::string& k) {
  static const std::set<std::string> v = {"channels", "patterns", "playlist", "tracks", "mixer", "automations", "markers",
                                          "links", "modLinks", "stageRooms", "arrangements", "stage", "flOrigin"};
  return v.count(k) > 0;
}

// Large lists inside an object read out as their size (the full object with full = true).
void shrink(Json& j, size_t maxItems, const std::string& where, Json& cut) {
  if (j.is(Json::Type::Array)) {
    if (j.array.size() > maxItems) {
      cut.array.push_back(jstr(where));
      j = jobj({{"items", jnum(static_cast<double>(j.array.size()))}, {"shown", jbool(false)}});
      return;
    }
    for (size_t i = 0; i < j.array.size(); ++i) shrink(j.array[i], maxItems, where + "/" + std::to_string(i + 1), cut);
  } else if (j.is(Json::Type::Object)) {
    for (auto& [k, v] : j.object) shrink(v, maxItems, where + "/" + k, cut);
  }
}

// "#RRGGBB" for every "color" key of a patch (the document stores numbers).
void colorsToNumbers(Json& j) {
  if (j.is(Json::Type::Object)) {
    for (auto& [k, v] : j.object) {
      Rgb c;
      if (k == "color" && v.is(Json::Type::String) && parseColor(v.str, c)) v = jnum(c);
      else colorsToNumbers(v);
    }
  } else if (j.is(Json::Type::Array)) {
    for (auto& v : j.array) colorsToNumbers(v);
  }
}

void mergePatch(Json& target, const Json& patch) {
  if (!patch.is(Json::Type::Object)) {
    target = patch;
    return;
  }
  if (!target.is(Json::Type::Object)) target = Json::makeObject();
  for (const auto& [k, v] : patch.object) {
    if (v.is(Json::Type::Null)) target.object.erase(k);
    else if (v.is(Json::Type::Object)) mergePatch(target.object[k], v);
    else target.object[k] = v;
  }
}

bool sameJson(const Json& a, const Json& b) {
  if (a.type != b.type) {
    if (a.is(Json::Type::Bool) && b.is(Json::Type::Number)) return (a.boolean ? 1.0 : 0.0) == b.number;
    if (b.is(Json::Type::Bool) && a.is(Json::Type::Number)) return (b.boolean ? 1.0 : 0.0) == a.number;
    return false;
  }
  switch (a.type) {
    case Json::Type::Null: return true;
    case Json::Type::Bool: return a.boolean == b.boolean;
    case Json::Type::Number: return std::fabs(a.number - b.number) <= 1e-6 * std::max(1.0, std::fabs(a.number));
    case Json::Type::String: return a.str == b.str;
    case Json::Type::Array:
      if (a.array.size() != b.array.size()) return false;
      for (size_t i = 0; i < a.array.size(); ++i)
        if (!sameJson(a.array[i], b.array[i])) return false;
      return true;
    case Json::Type::Object:
      for (const auto& [k, v] : a.object) {
        const Json* o = b.find(k);
        if (!o || !sameJson(v, *o)) return false;
      }
      return true;
  }
  return false;
}

// Compares what was asked with what the project now holds: ignored fields, adjusted values.
void verify(const Json& asked, const Json* now, const std::string& where, Json& ignored, Json& adjusted) {
  if (asked.is(Json::Type::Object) && (!now || now->is(Json::Type::Object))) {
    for (const auto& [k, v] : asked.object) {
      const Json* n = now ? now->find(k) : nullptr;
      const std::string w = where.empty() ? k : where + "/" + k;
      if (v.is(Json::Type::Null)) continue;
      if (!n) {
        // a default value is left out of the document: not an ignored field when it equals the default
        if (v.is(Json::Type::Bool) && !v.boolean) continue;
        if (v.is(Json::Type::Number) && v.number == 0) continue;
        if (v.is(Json::Type::String) && v.str.empty()) continue;
        if (v.is(Json::Type::Array) && v.array.empty()) continue;
        ignored.array.push_back(jstr(w));
        continue;
      }
      verify(v, n, w, ignored, adjusted);
    }
    return;
  }
  if (!now) {
    ignored.array.push_back(jstr(where));
    return;
  }
  if (!sameJson(asked, *now)) adjusted.array.push_back(jobj({{"field", jstr(where)}, {"value", *now}}));
}

// Puts the changed object of `ns` (read back from the patched document) in place: one Document edit.
bool transplant(Document& d, const Ref& r, const Song& ns, const Json& patchKeys, std::string& err) {
  const std::string kn = r.kind->name;
  const Song& s = d.song();
  const std::string label = "edit " + kn;
  if (kn == "channel") {
    const Channel& c = ns.channels.at(r.index);
    return d.editChannel(r.id, label, [&c](Channel& x) { x = c; });
  }
  if (kn == "pattern") {
    const Pattern& p = ns.patterns.at(r.index);
    return d.editPattern(r.id, label, [&p](Pattern& x) { x = p; });
  }
  if (kn == "mixer") {
    const MixerTrack& m = ns.mixer.at(r.index);
    return d.editMixerTrack(r.id, label, [&m](MixerTrack& x) { x = m; });
  }
  if (kn == "track") {
    const PlaylistTrack& t = ns.tracks.at(r.index);
    return d.editPlaylistTrack(static_cast<int>(r.index), label, [&t](PlaylistTrack& x) { x = t; });
  }
  if (kn == "clip") {
    const Clip* c = ns.clip(r.id);
    if (!c) {
      err = "the clip would be dropped (its pattern, audio or automation is missing)";
      return false;
    }
    const Clip copy = *c;
    return d.editClip(r.id, label, [&copy](Clip& x) { x = copy; });
  }
  if (kn == "automation") {
    const AutomationClip* a = ns.automation(r.id);
    if (!a) return false;
    const AutomationClip copy = *a;
    return d.editAutomation(r.id, label, [&copy](AutomationClip& x) { x = copy; });
  }
  if (kn == "marker") {
    d.editSong(label, [&ns](Song& x) { x.markers = ns.markers; });
    return true;
  }
  if (kn == "room") {
    d.editSong(label, [&ns](Song& x) {
      x.stage = ns.stage;
      x.stageRooms = ns.stageRooms;
    });
    return true;
  }
  if (kn == "arrangement") {
    const std::string name = arrangementName(ns, static_cast<int>(r.index));
    d.editSong(label, [&](Song& x) { renameArrangement(x, static_cast<int>(r.index), name); });
    return true;
  }
  if (kn == "link") {
    d.editSong(label, [&ns](Song& x) { x.links = ns.links; });
    return true;
  }
  if (kn == "modlink") {
    d.editSong(label, [&ns](Song& x) { x.modLinks = ns.modLinks; });
    return true;
  }
  // song settings
  if (patchKeys.find("ppq")) {
    if (!d.setPpq(ns.ppq)) {
      err = "ppq must be one of the supported resolutions (96, 192, 384, 768, 960 ...)";
      return false;
    }
  }
  (void)s;
  d.editSong("edit song settings", [&ns](Song& x) {
    x.info = ns.info;
    x.tempo = ns.tempo;
    x.timeSigNum = ns.timeSigNum;
    x.timeSigDen = ns.timeSigDen;
    x.swing = ns.swing;
    x.swingNotes = ns.swingNotes;
    x.groove = ns.groove;
    x.slidesAnyColour = ns.slidesAnyColour;
    x.slidesVelocity = ns.slidesVelocity;
    x.masterPitch = ns.masterPitch;
    x.autoCrossfades = ns.autoCrossfades;
    x.panLaw = ns.panLaw;
    x.live = ns.live;
    x.channelGroups = ns.channelGroups;
    x.patternGroups = ns.patternGroups;
    x.trackReceipt = ns.trackReceipt;
    x.flExportGridline = ns.flExportGridline;
  });
  return true;
}

// ================================================================= inspect ====
ToolResult tInspect(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  Ref r;
  std::string err;
  if (!parseRef(s, argStr(a, "path"), r, err)) return fail(err);
  Json doc = songToJson(s, false);
  Json* obj = locate(doc, s, r);
  if (!obj) return fail("nothing at " + argStr(a, "path"));
  Json* at = descend(obj, r.sub, false, err);
  if (!at) return fail(err + " (inspect " + std::string(r.kind->name) + (r.kind->plural[0] ? "/" + std::to_string(r.n) : "") + " lists the fields it has)");
  Json out = *at;
  if (std::string(r.kind->name) == "song" && r.sub.empty()) {
    Json counts = Json::makeObject();
    for (auto it = out.object.begin(); it != out.object.end();) {
      if (songCollection(it->first)) {
        if (it->second.is(Json::Type::Array)) counts.object[it->first] = jnum(static_cast<double>(it->second.array.size()));
        it = out.object.erase(it);
      } else {
        ++it;
      }
    }
    out.object.erase("format");
    out.object.erase("version");
    out.object["counts"] = counts;
  }
  Json cut = Json::makeArray();
  if (!argBool(a, "full")) shrink(out, static_cast<size_t>(argInt(a, "maxItems", 64)), argStr(a, "path"), cut);
  if (jsonCompact(out).size() > (2u << 20)) return fail("too large to read at once: inspect a deeper path");
  Json j = jobj({{"path", jstr(argStr(a, "path"))}, {"kind", jstr(r.kind->name)}, {"value", out}});
  if (r.id != kNoId) j.object["id"] = jnum(r.id);
  j.object["units"] = jstr("project file units: positions and lengths in ticks (" + std::to_string(s.ppq) +
                           " per beat), colours as 0xRRGGBB numbers (patch also takes \"#RRGGBB\"), other objects by their id");
  if (!cut.array.empty()) {
    j.object["shortened"] = cut;
    j.object["hint"] = jstr("long lists are shown as their size: inspect one of these paths, or pass full: true");
  }
  if (argBool(a, "fields")) {
    Json f = Json::makeObject();
    for (const auto& [k, d] : r.kind->fields) f.object[k] = jstr(d);
    j.object["fields"] = f;
  }
  return ok(j, "inspected " + argStr(a, "path"));
}

// =================================================================== patch ====
bool patchOne(Environment& env, const std::string& path, const Json& setIn, Json& report, std::string& err) {
  Document& d = env.doc;
  const Song& s = d.song();
  Ref r;
  if (!parseRef(s, path, r, err)) return false;
  Json set = setIn;
  if (!r.sub.empty()) {  // a value deeper in the object: wrap it so the merge sees the whole object
    Json w = set;
    for (auto it = r.sub.rbegin(); it != r.sub.rend(); ++it) {
      long long n;
      if (number(*it, n)) {
        err = "patch a list as a whole (give the list at its key): positions inside lists are read-only here";
        return false;
      }
      w = jobj({{*it, w}});
    }
    set = w;
  } else if (!set.is(Json::Type::Object)) {
    err = "set must be an object of fields, e.g. {\"name\": \"Lead\"} (inspect " + path + " shows the fields)";
    return false;
  }
  const std::string kn = r.kind->name;
  if (set.find("id")) {
    err = "an object's id cannot change";
    return false;
  }
  if (kn == "song")
    for (const auto& [k, v] : set.object) {
      if (songCollection(k)) {
        err = "\"" + k + "\" is a collection: patch its objects (" + kPathHelp + ")";
        return false;
      }
      if (k == "nextId" || k == "format" || k == "version" || k == "currentArrangement" || k == "clipLoops") {
        err = "\"" + k + "\" is managed by Gridline" + (k == "currentArrangement" ? ": use the arrangement tool" : "");
        return false;
      }
    }
  if (kn == "arrangement")
    for (const auto& [k, v] : set.object)
      if (k != "name") {
        err = "only an arrangement's name is set here: clips, tracks and markers of the current arrangement are clip/, track/ and marker/ paths";
        return false;
      }
  if ((kn == "channel" || kn == "mixer") && set.find("freeze")) {
    err = "freeze is made by rendering: use the app's freeze commands (list_commands query \"freeze\")";
    return false;
  }
  colorsToNumbers(set);
  Json doc = songToJson(s, false);
  Json* obj = locate(doc, s, r);
  if (!obj) {
    err = "nothing at " + path;
    return false;
  }
  mergePatch(*obj, set);
  Song ns;
  if (!songFromJson(doc, ns, &err)) {
    err = "that change does not make a valid project: " + err;
    return false;
  }
  if (!transplant(d, r, ns, set, err)) {
    if (err.empty()) err = "could not change " + path;
    return false;
  }
  // What the project holds now.
  Json after = songToJson(d.song(), false);
  Ref r2 = r;
  if (kn == "marker" || kn == "clip" || kn == "automation") {
    std::string ignore;
    parseRef(d.song(), path, r2, ignore);
  }
  Json* now = locate(after, d.song(), r2);
  Json ignored = Json::makeArray(), adjusted = Json::makeArray();
  verify(set, now, "", ignored, adjusted);
  if (set.is(Json::Type::Object) && !set.object.empty()) {  // every field unknown: say so instead of a no-op step
    size_t unknown = 0;
    std::string names;
    for (const auto& [k, v] : set.object)
      for (const auto& g : ignored.array)
        if (g.str == k) {
          ++unknown;
          names += (names.empty() ? "" : ", ") + k;
          break;
        }
    if (unknown == set.object.size()) {
      err = "no such field of a " + kn + ": " + names + " (help topic \"objects\" lists its fields)";
      return false;
    }
  }
  report = jobj({{"path", jstr(path)}});
  if (!ignored.array.empty()) {
    report.object["ignored"] = ignored;
    report.object["hint"] = jstr("ignored fields are not part of a " + kn + " (help topic \"objects\" lists them)");
  }
  if (!adjusted.array.empty()) report.object["adjusted"] = adjusted;
  return true;
}

ToolResult tPatch(Environment& env, const Json& a) {
  std::vector<std::pair<std::string, Json>> list;
  if (has(a, "path")) {
    if (!has(a, "set") && !has(a, "value")) return fail("give set (an object of fields) or value (with a path to one field)");
    list.push_back({argStr(a, "path"), has(a, "set") ? *a.find("set") : *a.find("value")});
  }
  if (const Json* ps = a.find("patches"))
    for (const auto& p : ps->array) {
      if (!p.find("path") || (!p.find("set") && !p.find("value"))) return fail("each patch needs path and set (or value)");
      list.push_back({p.text("path"), p.find("set") ? *p.find("set") : *p.find("value")});
    }
  if (list.empty()) return fail("nothing to change: give path and set, or patches");
  Json reports = Json::makeArray();
  bool anyIgnored = false;
  for (size_t i = 0; i < list.size(); ++i) {
    Json rep;
    std::string err;
    if (!patchOne(env, list[i].first, list[i].second, rep, err))
      return fail((list.size() > 1 ? "patch " + std::to_string(i + 1) + " (" + list[i].first + "): " : "") + err + " (nothing was changed)");
    anyIgnored = anyIgnored || rep.find("ignored");
    reports.array.push_back(std::move(rep));
  }
  Json j = jobj({{"changed", reports}});
  if (anyIgnored) j.object["warning"] = jstr("some fields were ignored (see changed[].ignored)");
  return ok(j, std::to_string(list.size()) + " object" + (list.size() == 1 ? "" : "s") + " patched (" + list[0].first + (list.size() > 1 ? ", ..." : "") + ")");
}

// =================================================================== batch ====
ToolResult tBatch(Environment& env, const Json& a) {
  const Json* calls = a.find("calls");
  Json results = Json::makeArray();
  const bool keepGoing = argBool(a, "continueOnError");
  size_t failed = 0;
  for (size_t i = 0; i < calls->array.size(); ++i) {
    const Json& c = calls->array[i];
    const std::string name = c.text("tool");
    const std::string where = "call " + std::to_string(i + 1) + " (" + name + ")";
    const Tool* t = findTool(name);
    if (!t) return fail(where + ": unknown tool (nothing was changed)");
    if (t->prepare || t->writesFiles || !t->grouped || name == "batch")
      return fail(where + ": " + name + " cannot run inside a batch (renders, exports, saves, undo, redo and batches run on their own)");
    Json args = c.find("arguments") && c.find("arguments")->is(Json::Type::Object) ? *c.find("arguments") : Json::makeObject();
    std::string verr;
    if (!validateArgs(t->inputSchema, args, verr)) return fail(where + ": " + verr + " (nothing was changed)");
    if (!t->readOnly && !env.allowEdits) return fail(env.editsOffMessage);
    ToolResult r;
    try {
      r = t->run(env, args);
    } catch (const std::exception& e) {
      r.error = std::string("internal error: ") + e.what();
    }
    if (!r.error.empty()) {
      if (!keepGoing) return fail(where + ": " + r.error + " (nothing was changed: the batch is one step)");
      ++failed;
      results.array.push_back(jobj({{"tool", jstr(name)}, {"error", jstr(r.error)}}));
      continue;
    }
    Json item = jobj({{"tool", jstr(name)}, {"result", r.data}});
    if (jsonCompact(r.data).size() > 4000) item.object["result"] = jobj({{"summary", jstr(r.summary)}});
    results.array.push_back(std::move(item));
  }
  Json j = jobj({{"results", results}, {"calls", jnum(static_cast<double>(calls->array.size()))}});
  if (failed) j.object["failed"] = jnum(static_cast<double>(failed));
  return ok(j, "batch of " + std::to_string(calls->array.size()) + " calls" + (failed ? " (" + std::to_string(failed) + " failed)" : ""));
}

// ==================================================================== find ====
ToolResult tFind(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  const std::string q = key(argStr(a, "query"));
  const size_t limit = static_cast<size_t>(argInt(a, "limit", 50));
  Json hits = Json::makeArray();
  size_t total = 0;
  auto add = [&](const std::string& path, const char* kind, const std::string& label, const std::string& detail) {
    if (q.empty() || (key(label).find(q) == std::string::npos && key(detail).find(q) == std::string::npos)) return;
    if (total++ >= limit) return;
    Json h = jobj({{"path", jstr(path)}, {"kind", jstr(kind)}, {"label", jstr(label)}});
    if (!detail.empty()) h.object["detail"] = jstr(detail);
    hits.array.push_back(std::move(h));
  };
  for (size_t i = 0; i < s.channels.size(); ++i)
    add("channel/" + std::to_string(i + 1), "channel", s.channels[i].name, pluginName(s.channels[i].plugin.type) + " " + s.channels[i].plugin.type);
  for (size_t i = 0; i < s.patterns.size(); ++i) add("pattern/" + std::to_string(i + 1), "pattern", s.patterns[i].name, "");
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    add("mixer/" + std::to_string(i), "mixer track", mixerTrackName(s, i), s.mixer[i].group);
    for (size_t k = 0; k < s.mixer[i].slots.size(); ++k)
      if (!s.mixer[i].slots[k].plugin.empty())
        add("mixer/" + std::to_string(i) + "/slots/" + std::to_string(k + 1), "effect", pluginName(s.mixer[i].slots[k].plugin.type),
            s.mixer[i].slots[k].plugin.type + " on " + mixerTrackName(s, i));
  }
  for (size_t i = 0; i < s.tracks.size(); ++i)
    if (!s.tracks[i].name.empty()) add("track/" + std::to_string(i + 1), "playlist track", s.tracks[i].name, "");
  for (size_t i = 0; i < s.automations.size(); ++i)
    add("automation/" + std::to_string(i + 1), "automation clip", s.automations[i].name, paramDisplayName(s, ParamId{s.automations[i].owner, s.automations[i].index}));
  for (size_t i = 0; i < s.markers.size(); ++i) add("marker/" + std::to_string(i + 1), "marker", s.markers[i].name, "");
  for (int i = 0; i < roomops::roomCount(s); ++i) add("room/" + std::to_string(i + 1), "stage room", roomops::roomName(s, i), "");
  for (int i = 0; i < arrangementCount(s); ++i) add("arrangement/" + std::to_string(i + 1), "arrangement", arrangementName(s, i), "");
  if (argBool(a, "includePlugins", true)) {
    for (const auto& p : pluginCatalog())
      if (!p.hidden) add("plugin:" + p.type, "plugin type", p.name, p.type + " " + p.category + " " + pluginKeywords(p.type));
    for (const PluginInfo* p : dynamicPlugins()) add("plugin:" + p->type, "plugin type", p->name, p->type + " " + p->category);
  }
  if (argBool(a, "includeCommands", true) && env.app.available())
    for (const auto& c : env.app.commands()) add(c.id, "app command", c.label, c.path + " " + c.hint);
  Json j = jobj({{"hits", hits}, {"total", jnum(static_cast<double>(total))}});
  if (total > hits.array.size()) j.object["truncated"] = jbool(true);
  if (q.empty()) j.object["hint"] = jstr("give a query");
  return ok(j, std::to_string(total) + " matches for \"" + argStr(a, "query") + "\"");
}

// ======================================================= parameter paths ====
// "song/tempo", "channel/2/volume", "channel/2/Cutoff" (a plugin parameter by name or number),
// "mixer/3/pan", "mixer/3/slot/1/mix", "mixer/3/slot/1/Decay".
bool paramArgsOfPath(const std::string& path, Json& args, std::string& err) {
  const auto parts = split(path, '/');
  args = Json::makeObject();
  auto rest = [&](size_t from) {
    std::string p;
    for (size_t i = from; i < parts.size(); ++i) p += (p.empty() ? "" : "/") + parts[i];
    return p;
  };
  long long n = 0, k = 0;
  const char* hint = "parameter paths: song/<param>, channel/N/<param>, mixer/N/<param>, mixer/N/slot/K/<param> (get_parameters lists the names)";
  if (parts.size() >= 2 && parts[0] == "song") {
    args.object["param"] = jstr(rest(1));
    return true;
  }
  if (parts.size() >= 3 && parts[0] == "channel" && number(parts[1], n)) {
    args.object["channel"] = jnum(static_cast<double>(n));
    args.object["param"] = jstr(rest(2));
    return true;
  }
  if (parts.size() >= 5 && parts[0] == "mixer" && number(parts[1], n) && parts[2] == "slot" && number(parts[3], k)) {
    args.object["mixer"] = jnum(static_cast<double>(n));
    args.object["slot"] = jnum(static_cast<double>(k));
    args.object["param"] = jstr(rest(4));
    return true;
  }
  if (parts.size() >= 3 && parts[0] == "mixer" && number(parts[1], n)) {
    args.object["mixer"] = jnum(static_cast<double>(n));
    args.object["param"] = jstr(rest(2));
    return true;
  }
  err = "\"" + path + "\" is not a parameter path: " + hint;
  return false;
}

ToolResult tSetParameters(Environment& env, const Json& a) {
  Document& d = env.doc;
  Json out = Json::makeArray();
  const Json* list = a.find("changes");
  for (size_t i = 0; i < list->array.size(); ++i) {
    const Json& c = list->array[i];
    const std::string path = c.text("path");
    Json args;
    std::string err;
    ParamId id;
    if (!paramArgsOfPath(path, args, err) || !resolveParam(d.song(), args, id, err))
      return fail("change " + std::to_string(i + 1) + " (" + path + "): " + err + " (nothing was changed)");
    auto desc = describeParam(d.song(), id);
    double v = c.num("value");
    if (const Json* vv = c.find("value"); vv && vv->is(Json::Type::Bool)) v = vv->boolean ? desc->max : desc->min;
    if (const Json* vv = c.find("value"); vv && vv->is(Json::Type::String)) {  // a choice by its name
      bool found = false;
      for (size_t k = 0; k < desc->choices.size() && !found; ++k)
        if (key(desc->choices[k]) == key(vv->str)) v = desc->min + static_cast<double>(k), found = true;
      if (!found) return fail("change " + std::to_string(i + 1) + " (" + path + "): \"" + vv->str + "\" is not one of its choices (get_parameters lists them)");
    }
    const Json* nz = c.find("normalized");
    if (nz && nz->is(Json::Type::Bool) && nz->boolean) v = desc->fromNormalized(std::clamp(v, 0.0, 1.0));
    if (!d.setParam(id, v, false)) return fail("change " + std::to_string(i + 1) + " (" + path + "): could not set it");
    Json pj = paramJson(d.song(), args.text("param"), id);
    pj.object["path"] = jstr(path);
    out.array.push_back(std::move(pj));
  }
  return ok(jobj({{"params", out}}), std::to_string(out.array.size()) + " parameters set");
}

ToolResult tGetValues(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  Json out = Json::makeArray();
  for (const auto& p : a.find("paths")->array) {
    Json args;
    std::string err;
    ParamId id;
    if (!paramArgsOfPath(p.str, args, err) || !resolveParam(s, args, id, err)) {
      out.array.push_back(jobj({{"path", jstr(p.str)}, {"error", jstr(err)}}));
      continue;
    }
    Json pj = paramJson(s, args.text("param"), id);
    pj.object["path"] = jstr(p.str);
    out.array.push_back(std::move(pj));
  }
  return ok(jobj({{"params", out}}), std::to_string(out.array.size()) + " parameters read");
}

// =============================================================== transport ====
Json transportJson(const Environment& env) {
  const Song& s = env.doc.song();
  const auto sel = env.host.timeSelection();
  Json j = jobj({{"playing", jbool(env.host.playing())},
                 {"recording", jbool(env.host.recording())},
                 {"songMode", jbool(env.host.songMode())},
                 {"position", beatsJ(s, env.host.position())}});
  const int p = s.patternIndex(env.host.currentPattern());
  if (p >= 0) j.object["pattern"] = jnum(p + 1);
  if (sel.second > sel.first) j.object["timeSelection"] = jobj({{"from", beatsJ(s, sel.first)}, {"to", beatsJ(s, sel.second)}});
  Json ch = Json::makeArray();
  for (ObjectId id : env.host.selectedChannels())
    if (s.channelIndex(id) >= 0) ch.array.push_back(jnum(s.channelIndex(id) + 1));
  j.object["selectedChannels"] = ch;
  if (s.mixerIndex(env.host.selectedMixer()) >= 0) j.object["selectedMixer"] = jnum(s.mixerIndex(env.host.selectedMixer()));
  Json cl = Json::makeArray();
  for (ObjectId id : env.host.selectedClips()) cl.array.push_back(jnum(id));
  j.object["selectedClips"] = cl;
  return j;
}

ToolResult tTransport(Environment& env, const Json& a) {
  const std::string action = argStr(a, "action", "status");
  if (action != "status" && !env.allowTransport) return fail("the transport cannot be controlled here");
  if (has(a, "position")) env.host.setPosition(std::max<Tick>(0, toTicks(env.doc.song(), argNum(a, "position"))));
  if (action == "play") env.host.play(true);
  else if (action == "stop") env.host.play(false);
  else if (action == "toggle") env.host.play(!env.host.playing());
  return ok(transportJson(env), action == "status" ? "transport" : "transport " + action);
}

ToolResult tSelect(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  std::string err;
  if (const Json* ch = a.find("channels")) {
    std::vector<ObjectId> ids;
    for (const auto& n : ch->array) {
      const Channel* c = channelAt(s, std::llround(n.number), err);
      if (!c) return fail(err);
      ids.push_back(c->id);
    }
    env.host.selectChannels(ids);
  }
  if (has(a, "pattern")) {
    const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
    if (!p) return fail(err);
    env.host.setCurrentPattern(p->id);
  }
  if (has(a, "mixer")) {
    const MixerTrack* m = mixerAt(s, argInt(a, "mixer"), err);
    if (!m) return fail(err);
    env.host.selectMixer(m->id);
  }
  if (const Json* cl = a.find("clips")) {
    std::vector<ObjectId> ids;
    for (const auto& n : cl->array) {
      const ObjectId id = static_cast<ObjectId>(std::llround(n.number));
      if (!s.clip(id)) return fail("no clip with id " + std::to_string(id));
      ids.push_back(id);
    }
    env.host.selectClips(ids);
  }
  if (const Json* ts = a.find("timeSelection")) {
    if (ts->is(Json::Type::Null)) env.host.setTimeSelection(0, 0);
    else env.host.setTimeSelection(toTicks(s, ts->num("from")), toTicks(s, ts->num("to")));
  }
  return ok(transportJson(env), "selection");
}

// =============================================================== duplicate ====
ToolResult tDuplicate(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  if (has(a, "channel")) {
    const Channel* c = channelAt(d.song(), argInt(a, "channel"), err);
    if (!c) return fail(err);
    const ObjectId id = d.cloneChannel(c->id);
    if (id == kNoId) return fail("could not duplicate the channel");
    return ok(jobj({{"channel", jnum(channelNumber(d.song(), id))}, {"path", jstr("channel/" + std::to_string(channelNumber(d.song(), id)))}}), "duplicated a channel");
  }
  if (has(a, "pattern")) {
    const Pattern* p = patternAt(d.song(), argInt(a, "pattern"), err);
    if (!p) return fail(err);
    const ObjectId id = d.clonePattern(p->id);
    if (id == kNoId) return fail("could not duplicate the pattern");
    return ok(jobj({{"pattern", jnum(patternNumber(d.song(), id))}}), "duplicated a pattern");
  }
  if (const Json* cl = a.find("clips")) {
    std::set<ObjectId> ids;
    for (const auto& n : cl->array) {
      const ObjectId id = static_cast<ObjectId>(std::llround(n.number));
      if (!d.song().clip(id)) return fail("no clip with id " + std::to_string(id) + " (get_playlist lists them)");
      ids.insert(id);
    }
    const auto made = ops::duplicateClips(d, ids);
    Json j = Json::makeArray();
    for (ObjectId id : made) j.array.push_back(jnum(id));
    return ok(jobj({{"clips", j}}), "duplicated " + std::to_string(made.size()) + " clips");
  }
  return fail("give channel, pattern or clips");
}

// =================================================================== notes ====
ToolResult tEditNotes(Environment& env, const Json& a) {
  Document& d = env.doc;
  const Song& s = d.song();
  std::string err;
  const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const Channel* c = channelAt(s, argInt(a, "channel"), err);
  if (!c) return fail(err);
  std::vector<Note> notes;
  if (auto it = p->notes.find(c->id); it != p->notes.end()) notes = it->second;
  if (notes.empty()) return fail("that channel has no notes in this pattern (set_notes writes some)");
  const Tick from = has(a, "from") ? toTicks(s, argNum(a, "from")) : std::numeric_limits<Tick>::min();
  const Tick to = has(a, "to") ? toTicks(s, argNum(a, "to")) : std::numeric_limits<Tick>::max();
  const int lo = static_cast<int>(argInt(a, "lowestPitch", 0)), hi = static_cast<int>(argInt(a, "highestPitch", 127));
  ops::NoteSel sel(notes.size(), false);
  size_t chosen = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (notes[i].start >= from && notes[i].start < to && notes[i].pitch >= lo && notes[i].pitch <= hi) sel[i] = true, ++chosen;
  if (!chosen) return fail("no note matches from / to / lowestPitch / highestPitch");
  const std::string op = argStr(a, "op");
  const double amount = argNum(a, "amount", 0);
  auto beats = [&](double def) { return std::max<Tick>(1, toTicks(s, has(a, "amount") ? amount : def)); };
  if (op == "quantize") ops::quantize(notes, sel, beats(0.25), argBool(a, "ends"));
  else if (op == "legato") ops::legato(notes, sel);
  else if (op == "transpose") ops::transpose(notes, sel, static_cast<int>(std::lround(amount)));
  else if (op == "nudge") ops::nudge(notes, sel, toTicks(s, amount));
  else if (op == "scale_velocity") ops::scaleVelocity(notes, sel, has(a, "amount") ? amount : 1.0);
  else if (op == "humanize") ops::humanize(notes, sel, static_cast<int>(argInt(a, "velocityRange", 10)), toTicks(s, has(a, "amount") ? amount : 0.02), static_cast<uint32_t>(argInt(a, "seed", 1)));
  else if (op == "strum") ops::strum(notes, sel, beats(1.0 / 32));
  else if (op == "arpeggiate") ops::arpeggiate(notes, sel, beats(0.25));
  else if (op == "chop") ops::chop(notes, sel, beats(0.25));
  else if (op == "glue") ops::glue(notes, sel);
  else if (op == "flip_vertical") ops::flipVertical(notes, sel);
  else if (op == "flip_horizontal") ops::flipHorizontal(notes, sel);
  else if (op == "duplicate") ops::duplicate(notes, sel, beats(1.0));
  else if (op == "delete") ops::deleteNotes(notes, sel);
  else return fail("unknown op \"" + op + "\"");
  const size_t n = notes.size();
  if (!d.setNotes(p->id, c->id, std::move(notes), op)) return fail("could not change the notes");
  return ok(jobj({{"op", jstr(op)}, {"notesChosen", jnum(static_cast<double>(chosen))}, {"notes", jnum(static_cast<double>(n))}}),
            op + " on " + std::to_string(chosen) + " notes");
}

// ============================================================== automation ====
const std::vector<std::string>& shapeNames() {
  static const std::vector<std::string> v = {"curve", "s_curve", "hold", "stairs", "smooth", "pulse", "wave",
                                             "curve2", "curve3", "s_curve2", "s_curve3", "half_sine", "smooth_stairs"};
  return v;
}

ToolResult tEditAutomation(Environment& env, const Json& a) {
  Document& d = env.doc;
  const long long n = argInt(a, "automation");
  if (n < 1 || n > static_cast<long long>(d.song().automations.size()))
    return fail("no automation clip " + std::to_string(n) + " (get_automation lists them)");
  const AutomationClip& cur = d.song().automations[static_cast<size_t>(n - 1)];
  AutomationClip ac = cur;
  if (has(a, "name")) ac.name = clipName(argStr(a, "name"));
  if (has(a, "min")) ac.min = static_cast<float>(std::clamp(argNum(a, "min"), 0.0, 1.0));
  if (has(a, "max")) ac.max = static_cast<float>(std::clamp(argNum(a, "max"), 0.0, 1.0));
  if (has(a, "loop")) ac.loop = std::max<Tick>(0, toTicks(d.song(), argNum(a, "loop")));
  if (has(a, "color")) {
    Rgb col;
    if (!parseColor(argStr(a, "color"), col)) return fail("color must be \"#RRGGBB\"");
    ac.color = col;
  }
  auto readPoints = [&](const Json& arr, std::vector<AutoPoint>& out, std::string& err) {
    for (const auto& p : arr.array) {
      AutoPoint x;
      x.pos = std::max<Tick>(0, toTicks(d.song(), p.num("position")));
      x.value = static_cast<float>(std::clamp(p.num("value"), 0.0, 1.0));
      x.tension = static_cast<float>(std::clamp(p.num("tension"), -1.0, 1.0));
      if (const Json* sh = p.find("shape")) {
        const auto& names = shapeNames();
        auto it = std::find(names.begin(), names.end(), sh->str);
        if (it == names.end()) {
          err = "unknown shape \"" + sh->str + "\"";
          return false;
        }
        x.shape = static_cast<AutoShape>(it - names.begin());
      }
      out.push_back(x);
    }
    return true;
  };
  std::string err;
  if (const Json* pts = a.find("points")) {
    ac.points.clear();
    if (!readPoints(*pts, ac.points, err)) return fail(err);
  }
  if (const Json* pts = a.find("addPoints"))
    if (!readPoints(*pts, ac.points, err)) return fail(err);
  if (has(a, "removeFrom") || has(a, "removeTo")) {
    const Tick f = toTicks(d.song(), argNum(a, "removeFrom", 0)), t = has(a, "removeTo") ? toTicks(d.song(), argNum(a, "removeTo")) : std::numeric_limits<Tick>::max();
    ac.points.erase(std::remove_if(ac.points.begin(), ac.points.end(), [&](const AutoPoint& p) { return p.pos >= f && p.pos < t; }), ac.points.end());
  }
  std::stable_sort(ac.points.begin(), ac.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  if (ac == cur) return fail("nothing to change: give name, color, min, max, loop, points, addPoints or removeFrom / removeTo");
  if (!d.editAutomation(cur.id, "edit automation", [&ac](AutomationClip& x) { x = ac; })) return fail("could not change the automation clip");
  return ok(jobj({{"automation", jnum(static_cast<double>(n))}, {"pointCount", jnum(static_cast<double>(ac.points.size()))}}), "edited automation clip " + std::to_string(n));
}

ToolResult tRemoveAutomation(Environment& env, const Json& a) {
  Document& d = env.doc;
  const long long n = argInt(a, "automation");
  if (n < 1 || n > static_cast<long long>(d.song().automations.size())) return fail("no automation clip " + std::to_string(n));
  const ObjectId id = d.song().automations[static_cast<size_t>(n - 1)].id;
  const std::string name = d.song().automations[static_cast<size_t>(n - 1)].name;
  d.editSong("delete automation clip", [id](Song& s) {
    s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(), [id](const AutomationClip& x) { return x.id == id; }), s.automations.end());
    auto gone = [id](const Clip& c) { return c.kind == ClipKind::Automation && c.source == id; };
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), gone), s.playlist.end());
    for (auto& arr : s.arrangements) arr.playlist.erase(std::remove_if(arr.playlist.begin(), arr.playlist.end(), gone), arr.playlist.end());
  });
  return ok(jobj({{"removed", jstr(name)}}), "removed automation clip \"" + name + "\"");
}

// =================================================================== mixer ====
ToolResult tAddMixerTrack(Environment& env, const Json& a) {
  Document& d = env.doc;
  if (d.song().mixer.size() > static_cast<size_t>(kMaxMixerInserts)) return fail("the mixer is full");
  const ObjectId id = d.addMixerTrack();
  if (id == kNoId) return fail("could not add a mixer track");
  if (has(a, "name")) {
    const std::string n = clipName(argStr(a, "name"));
    d.editMixerTrack(id, "rename mixer track", [&n](MixerTrack& t) { t.name = n; });
  }
  const int i = d.song().mixerIndex(id);
  return ok(mixerJson(d.song(), static_cast<size_t>(i)), "added mixer track " + std::to_string(i));
}

ToolResult tRemoveMixerTrack(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const MixerTrack* m = mixerAt(d.song(), argInt(a, "mixer"), err);
  if (!m) return fail(err);
  if (argInt(a, "mixer") == 0) return fail("the master cannot be removed");
  const std::string name = mixerTrackName(d.song(), static_cast<size_t>(argInt(a, "mixer")));
  if (!d.removeMixerTrack(m->id)) return fail("could not remove the mixer track");
  return ok(jobj({{"removed", jstr(name)}}), "removed mixer track \"" + name + "\"");
}

ToolResult tSetSend(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const MixerTrack* from = mixerAt(d.song(), argInt(a, "mixer"), err);
  if (!from) return fail(err);
  const MixerTrack* to = mixerAt(d.song(), argInt(a, "to"), err);
  if (!to) return fail(err);
  if (from->id == to->id) return fail("a track cannot send to itself");
  const ObjectId f = from->id, t = to->id;
  const double level = argNum(a, "level", 1.0);
  if (level > 0 && !canRoute(d.song(), f, t)) return fail("that send would make a loop (" + mixerTrackName(d.song(), static_cast<size_t>(argInt(a, "to"))) + " already feeds this track)");
  if (!d.setSend(f, t, static_cast<float>(level)) && level > 0) return fail("could not set the send");
  if (level > 0) {
    if (has(a, "sidechain")) d.setSendSidechain(f, t, argBool(a, "sidechain"));
    if (has(a, "preFader")) mixops::setSendPreFader(d, f, t, argBool(a, "preFader"));
    if (has(a, "stageRoom")) mixops::setSendRoom(d, f, t, argBool(a, "stageRoom"));
  }
  const int i = d.song().mixerIndex(f);
  return ok(mixerJson(d.song(), static_cast<size_t>(i)), level > 0 ? "send " + std::to_string(i) + " -> " + std::to_string(argInt(a, "to")) : "removed a send");
}

// ============================================================= Stage rooms ====
ToolResult tStageRoom(Environment& env, const Json& a) {
  Document& d = env.doc;
  const std::string action = argStr(a, "action");
  const int room = static_cast<int>(argInt(a, "room", 1)) - 1;
  auto checkRoom = [&](int i) { return i >= 0 && i < roomops::roomCount(d.song()); };
  if (action != "add" && !checkRoom(room)) return fail("no room " + std::to_string(room + 1) + " (1.." + std::to_string(roomops::roomCount(d.song())) + ")");
  Rgb col = 0;
  if (has(a, "color") && !parseColor(argStr(a, "color"), col)) return fail("color must be \"#RRGGBB\"");
  if (action == "add") {
    const int i = roomops::addRoom(d, std::max(0, room), nullptr, argStr(a, "name"), col);
    if (i < 0) return fail("could not add a room");
    return ok(jobj({{"room", jnum(i + 1)}, {"path", jstr("room/" + std::to_string(i + 1))}}), "added Stage room " + std::to_string(i + 1));
  }
  if (action == "rename") {
    if (!roomops::renameRoom(d, room, clipName(argStr(a, "name")))) return fail("could not rename the room");
  } else if (action == "color") {
    if (!roomops::setRoomColor(d, room, col)) return fail("could not colour the room");
  } else if (action == "move") {
    if (!roomops::moveRoom(d, room, static_cast<int>(argInt(a, "to", 1)) - 1)) return fail("could not move the room");
  } else if (action == "delete") {
    if (!roomops::deleteRoom(d, room)) return fail("only an empty room can be deleted, and never the last one: move its tracks to another room first (action move_tracks)");
  } else if (action == "move_tracks" || action == "find_tracks") {
    std::vector<ObjectId> ids;
    std::string err;
    if (const Json* t = a.find("tracks")) {
      for (const auto& n : t->array) {
        const MixerTrack* m = mixerAt(d.song(), std::llround(n.number), err);
        if (!m) {
          return fail(err);
        }
        ids.push_back(m->id);
      }
    }
    // Tracks from a marker area (core/StagePick.h): every clip playing from marker N to the next (or to marker_to).
    if (has(a, "marker")) {
      const int mk = static_cast<int>(argInt(a, "marker", 1)) - 1;
      if (mk < 0 || mk >= static_cast<int>(d.song().markers.size())) {
        return fail("no marker " + std::to_string(mk + 1) + " (the song has " + std::to_string(d.song().markers.size()) + ")");
      }
      const stagepick::Area ar = stagepick::betweenMarkers(d.song(), mk, has(a, "markerTo") ? static_cast<int>(argInt(a, "markerTo", 0)) - 1 : -1);
      stagepick::Options o;
      o.buses = !has(a, "buses") || argBool(a, "buses");
      o.muted = argBool(a, "muted");
      for (ObjectId id : stagepick::fromRange(d.song(), ar.start, ar.end, o).tracks) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
          ids.push_back(id);
        }
      }
      if (action == "move_tracks" && !has(a, "room")) {  // no room given: a new one named after the marker
        int to = -1;
        const int moved = stagepick::moveToRoom(d, ids, -1, stagepick::areaName(d.song(), ar), 0, &to);
        return ok(jobj({{"moved", jnum(moved)}, {"room", jnum(to + 1)}}), "moved " + std::to_string(moved) + " tracks to a new room " + std::to_string(to + 1));
      }
    }
    if (action == "find_tracks") {
      Json list = Json::makeArray();
      for (ObjectId id : ids) {
        const int i = d.song().mixerIndex(id);
        list.array.push_back(jobj({{"mixer", jnum(i)}, {"name", jstr(mixerTrackName(d.song(), static_cast<size_t>(i)))},
                                   {"room", jnum(roomops::trackRoom(d.song(), id) + 1)}}));
      }
      return ok(jobj({{"tracks", list}}), "found " + std::to_string(ids.size()) + " tracks");
    }
    if (ids.empty()) return fail("give tracks (mixer track numbers) or a marker whose area's tracks move into the room");
    const int moved = roomops::moveTracksToRoom(d, ids, room, argBool(a, "feeders"));
    return ok(jobj({{"moved", jnum(moved)}}), "moved " + std::to_string(moved) + " tracks to room " + std::to_string(room + 1));
  } else if (action == "set_space") {
    if (!mixops::setRoomType(d, static_cast<int>(argInt(a, "space", 0)), {}, room)) return fail("could not set the room's space");
  } else {
    return fail("unknown action");
  }
  return ok(jobj({{"room", jnum(room + 1)}, {"rooms", jnum(roomops::roomCount(d.song()))}}), action + " Stage room");
}

// ============================================================ arrangements ====
ToolResult tArrangement(Environment& env, const Json& a) {
  Document& d = env.doc;
  const std::string action = argStr(a, "action", "list");
  const int n = static_cast<int>(argInt(a, "arrangement", 1)) - 1;
  const int count = arrangementCount(d.song());
  if (action != "list" && action != "add" && (n < 0 || n >= count)) return fail("no arrangement " + std::to_string(n + 1) + " (1.." + std::to_string(count) + ")");
  bool okDone = true;
  if (action == "add") {
    const std::string name = has(a, "name") ? clipName(argStr(a, "name")) : uniqueArrangementName(d.song(), "Arrangement");
    const bool copy = argBool(a, "copyCurrent");
    d.editSong("add arrangement", [&](Song& s) { addArrangement(s, name, copy); });
  } else if (action == "select") {
    d.editSong("switch arrangement", [&](Song& s) { okDone = selectArrangement(s, n); });
  } else if (action == "rename") {
    const std::string name = clipName(argStr(a, "name"));
    d.editSong("rename arrangement", [&](Song& s) { okDone = renameArrangement(s, n, name); });
  } else if (action == "remove") {
    if (count <= 1) return fail("the only arrangement cannot be removed");
    d.editSong("delete arrangement", [&](Song& s) { okDone = removeArrangement(s, n); });
  } else if (action == "move") {
    const int to = static_cast<int>(argInt(a, "to", 1)) - 1;
    if (to < 0 || to >= count) return fail("no destination arrangement " + std::to_string(to + 1) + " (1.." + std::to_string(count) + ")");
    d.editSong("reorder arrangement", [&](Song& s) { okDone = moveArrangement(s, n, to); });
  } else if (action != "list") {
    return fail("unknown action");
  }
  if (!okDone) return fail("could not " + action + " the arrangement");
  Json list = Json::makeArray();
  for (int i = 0; i < arrangementCount(d.song()); ++i)
    list.array.push_back(jobj({{"arrangement", jnum(i + 1)}, {"name", jstr(arrangementName(d.song(), i))}, {"current", jbool(i == d.song().currentArrangement)}}));
  return ok(jobj({{"arrangements", list}}), action + " arrangement");
}

// ================================================================ the app ====
const char* kNeedsApp = "this needs the Gridline app: connect to the running app (Settings > Integrations) instead of the headless server";

// Commands an assistant may not run: they close or replace the open project.
bool blockedCommand(const std::string& id) {
  static const char* exact[] = {"cmd:file.new", "cmd:file.open", "cmd:file.quit", "cmd:file.close"};
  for (const char* e : exact)
    if (id == e) return true;
  static const char* prefixes[] = {"menu:File/New", "menu:File/Open", "menu:File/Revert", "menu:File/Join", "menu:File/Welcome", "menu:File/Quit", "menu:File/Exit"};
  for (const char* p : prefixes)
    if (id.compare(0, std::strlen(p), p) == 0) return true;
  return false;
}

ToolResult tListCommands(Environment& env, const Json& a) {
  std::vector<AppBridge::Command> rows;
  const bool app = env.app.available();
  if (app) {
    rows = env.app.commands();
  } else {
    for (const keys::Command& c : keys::commands()) {
      AppBridge::Command r;
      r.id = std::string("cmd:") + c.id;
      r.kind = "command";
      r.label = c.label;
      r.path = keys::contextName(c.context);
      r.enabled = false;
      r.hint = "needs the Gridline app";
      rows.push_back(std::move(r));
    }
  }
  std::vector<cmdpal::Entry> entries;
  for (const auto& r : rows) {
    cmdpal::Entry e;
    e.id = r.id;
    e.kind = r.kind == "window" ? cmdpal::Kind::Window : r.kind == "setting" ? cmdpal::Kind::Setting : r.kind == "plugin" ? cmdpal::Kind::Plugin : cmdpal::Kind::Command;
    e.label = r.label;
    e.path = r.path;
    e.shortcut = r.shortcut;
    e.alias = r.hint + " " + r.id;
    e.enabled = r.enabled;
    entries.push_back(std::move(e));
  }
  const std::string kind = argStr(a, "kind");
  int k = -1;
  if (kind == "command") k = static_cast<int>(cmdpal::Kind::Command);
  if (kind == "window") k = static_cast<int>(cmdpal::Kind::Window);
  if (kind == "setting") k = static_cast<int>(cmdpal::Kind::Setting);
  if (kind == "plugin") k = static_cast<int>(cmdpal::Kind::Plugin);
  const auto ranked = cmdpal::rank(entries, argStr(a, "query"), {}, k);
  const size_t offset = static_cast<size_t>(argInt(a, "offset", 0)), limit = static_cast<size_t>(argInt(a, "limit", 40));
  Json list = Json::makeArray();
  for (size_t i = offset; i < ranked.size() && list.array.size() < limit; ++i) {
    const auto& r = rows[static_cast<size_t>(ranked[i].index)];
    Json j = jobj({{"id", jstr(r.id)}, {"kind", jstr(r.kind)}, {"label", jstr(r.label)}});
    if (!r.path.empty()) j.object["path"] = jstr(r.path);
    if (!r.shortcut.empty()) j.object["shortcut"] = jstr(r.shortcut);
    if (!r.hint.empty()) j.object["hint"] = jstr(r.hint);
    if (!r.enabled) j.object["enabled"] = jbool(false);
    if (r.checkable) j.object["checked"] = jbool(r.checked);
    if (blockedCommand(r.id)) j.object["blocked"] = jbool(true);
    list.array.push_back(std::move(j));
  }
  Json j = jobj({{"commands", list}, {"total", jnum(static_cast<double>(ranked.size()))}});
  if (!app) j.object["note"] = jstr(kNeedsApp);
  if (ranked.size() > offset + list.array.size()) j.object["more"] = jstr("raise offset or narrow the query");
  return ok(j, std::to_string(ranked.size()) + " commands");
}

ToolResult tRunCommand(Environment& env, const Json& a) {
  if (!env.app.available()) return fail(kNeedsApp);
  const std::string id = argStr(a, "id");
  if (blockedCommand(id)) return fail("\"" + id + "\" would close or replace the open project: ask the user to do it in Gridline");
  bool found = false;
  AppBridge::Command row;
  for (const auto& r : env.app.commands())
    if (r.id == id) row = r, found = true;
  if (!found) return fail("no command \"" + id + "\" (list_commands with a query finds the ids)");
  if (!row.enabled) return fail("\"" + row.label + "\" cannot run now" + (row.hint.empty() ? "" : ": " + row.hint));
  bool queued = false;
  std::string err;
  if (!env.app.runCommand(id, queued, err)) return fail(err.empty() ? "could not run \"" + row.label + "\"" : err);
  Json j = jobj({{"id", jstr(id)}, {"label", jstr(row.label)}, {"ran", jbool(!queued)}});
  if (queued) j.object["queued"] = jstr("runs in the next frames (a keyboard command of an editor); check its effect with the read tools");
  return ok(j, "ran \"" + row.label + "\"");
}

ToolResult tWindows(Environment& env, const Json& a) {
  if (!env.app.available() || !env.app.windows) return fail(kNeedsApp);
  std::string err;
  if (has(a, "open") && !env.app.showWindow(argStr(a, "open"), true, err)) return fail(err);
  if (has(a, "close") && !env.app.showWindow(argStr(a, "close"), false, err)) return fail(err);
  Json list = Json::makeArray();
  for (const auto& w : env.app.windows()) list.array.push_back(jobj({{"name", jstr(w.name)}, {"open", jbool(w.open)}}));
  return ok(jobj({{"windows", list}}), has(a, "open") ? "opened " + argStr(a, "open") : has(a, "close") ? "closed " + argStr(a, "close") : "windows");
}

// (settings access: see the end of this file)

const char* typeName(Json::Type t) { return t == Json::Type::Bool ? "bool" : t == Json::Type::Number ? "number" : "string"; }

ToolResult tGetSettings(Environment& env, const Json& a) {
  Settings* st = env.app.settings;
  if (!st) return fail(kNeedsApp);
  const std::string q = key(argStr(a, "query"));
  const std::string prefix = argStr(a, "prefix");
  const size_t limit = static_cast<size_t>(argInt(a, "limit", 100));
  Json list = Json::makeArray();
  size_t total = 0;
  for (const auto& k : st->keys()) {
    if (!prefix.empty() && k.compare(0, prefix.size(), prefix) != 0) continue;
    if (!q.empty() && key(k).find(q) == std::string::npos) continue;
    if (total++ >= limit) continue;
    const SettingAccess acc = settingAccess(k);
    const Json* d = st->defaultValue(k);
    Json j = jobj({{"key", jstr(k)}, {"type", jstr(typeName(d->type))}});
    if (acc != SettingAccess::Hidden) {
      j.object["value"] = *st->value(k);
      j.object["default"] = *d;
    }
    j.object["access"] = jstr(acc == SettingAccess::Hidden ? "hidden" : acc == SettingAccess::ReadOnly ? "user only" : "read-write");
    list.array.push_back(std::move(j));
  }
  Json j = jobj({{"settings", list}, {"total", jnum(static_cast<double>(total))}});
  if (total > list.array.size()) j.object["more"] = jstr("narrow with prefix or query, or raise limit");
  return ok(j, std::to_string(total) + " settings");
}

ToolResult tSetSetting(Environment& env, const Json& a) {
  Settings* st = env.app.settings;
  if (!st) return fail(kNeedsApp);
  const std::string k = argStr(a, "key");
  const Json* d = st->defaultValue(k);
  if (!d) return fail("no setting \"" + k + "\" (get_settings with a query lists them)");
  if (settingAccess(k) != SettingAccess::ReadWrite)
    return fail("\"" + k + "\" is the user's to change (assistant access, scripts, network, files and secrets stay with the user): ask them to change it in Settings");
  const Json* v = a.find("value");
  bool done = false;
  if (d->is(Json::Type::Bool) && v->is(Json::Type::Bool)) done = st->set(k, v->boolean);
  else if (d->is(Json::Type::Number) && v->is(Json::Type::Number)) done = st->set(k, v->number);
  else if (d->is(Json::Type::String) && v->is(Json::Type::String)) done = st->set(k, v->str);
  if (!done) return fail("\"" + k + "\" takes a " + std::string(typeName(d->type)) + " value");
  if (env.app.settingsChanged) env.app.settingsChanged();
  ToolResult r = ok(jobj({{"key", jstr(k)}, {"value", *st->value(k)}}), k + " = " + dump(*st->value(k)));
  r.changed = true;
  return r;
}

// ==================================================================== help ====
// Examples per tool (help topic <tool>).
const std::vector<std::pair<const char*, std::vector<const char*>>>& examples() {
  static const std::vector<std::pair<const char*, std::vector<const char*>>> v = {
      {"get_song", {"{}"}},
      {"assistant_inbox", {"{}"}},
      {"assistant_say", {"{\"text\": \"I added a bass line to Pattern 2; accept it in the panel to keep it.\"}"}},
      {"get_proposals", {"{}"}},
      {"find", {"{\"query\": \"kick\"}", "{\"query\": \"reverb\", \"includeCommands\": false}"}},
      {"inspect", {"{\"path\": \"channel/2\"}", "{\"path\": \"mixer/3/sends\"}", "{\"path\": \"song\", \"fields\": true}", "{\"path\": \"clip/57\"}"}},
      {"patch",
       {"{\"path\": \"channel/2\", \"set\": {\"name\": \"Lead\", \"color\": \"#E0A040\", \"echo\": {\"count\": 3, \"time\": 0.5}}}",
        "{\"path\": \"track/4\", \"set\": {\"name\": \"Drums\", \"muted\": true}}",
        "{\"patches\": [{\"path\": \"mixer/1\", \"set\": {\"depth\": 0.4}}, {\"path\": \"room/1\", \"set\": {\"width\": 1.2}}]}",
        "{\"path\": \"channel/3/hibernate\", \"value\": true}"}},
      {"batch",
       {"{\"calls\": [{\"tool\": \"add_pattern\", \"arguments\": {\"name\": \"Verse\", \"bars\": 2}}, {\"tool\": \"set_steps\", \"arguments\": {\"pattern\": 2, "
        "\"channel\": 1, \"steps\": [100,0,0,0,100,0,0,0,100,0,0,0,100,0,0,0]}}, {\"tool\": \"add_clip\", \"arguments\": {\"pattern\": 2, \"track\": 1, \"start\": 0}}]}"}},
      {"set_parameters",
       {"{\"changes\": [{\"path\": \"song/tempo\", \"value\": 124}, {\"path\": \"channel/1/volume\", \"value\": 0.7}, {\"path\": \"mixer/2/slot/1/mix\", \"value\": 0.5}]}",
        "{\"changes\": [{\"path\": \"channel/2/Cutoff\", \"value\": 0.3, \"normalized\": true}]}"}},
      {"get_values", {"{\"paths\": [\"song/tempo\", \"mixer/0/volume\", \"channel/2/1\"]}"}},
      {"set_notes",
       {"{\"pattern\": 1, \"channel\": 2, \"notes\": [{\"start\": 0, \"length\": 1, \"pitch\": 48}, {\"start\": 1, \"length\": 0.5, \"pitch\": 55, "
        "\"velocity\": 90, \"bend\": [[0, 0], [0.5, 2]]}]}"}},
      {"edit_notes", {"{\"pattern\": 1, \"channel\": 2, \"op\": \"quantize\", \"amount\": 0.25}", "{\"pattern\": 1, \"channel\": 2, \"op\": \"transpose\", \"amount\": 12, \"lowestPitch\": 60}"}},
      {"set_send", {"{\"mixer\": 1, \"to\": 5, \"level\": 0.6}", "{\"mixer\": 2, \"to\": 3, \"level\": 1, \"sidechain\": true}", "{\"mixer\": 1, \"to\": 5, \"level\": 0}"}},
      {"list_commands", {"{\"query\": \"metronome\"}", "{\"kind\": \"window\"}"}},
      {"run_command", {"{\"id\": \"cmd:options.metronome\"}"}},
      {"stage_room", {"{\"action\": \"add\", \"name\": \"Strings hall\"}", "{\"action\": \"move_tracks\", \"room\": 2, \"tracks\": [4, 5]}",
                      "{\"action\": \"move_tracks\", \"marker\": 2}"}},
      {"arrangement", {"{\"action\": \"add\", \"name\": \"Radio edit\", \"copyCurrent\": true}", "{\"action\": \"select\", \"arrangement\": 2}"}},
      {"transport", {"{\"action\": \"play\", \"position\": 16}", "{\"action\": \"stop\"}"}},
      {"get_settings", {"{\"prefix\": \"ui.stage.\"}"}},
      {"set_setting", {"{\"key\": \"ui.smoothScroll\", \"value\": false}"}},
  };
  return v;
}

std::string overviewText() {
  return "Gridline over MCP: every editing call is one undo step in Gridline (\"AI: ...\"), a failing call changes nothing.\n"
         "Start with get_song (compact overview), then focused reads: list_channels, get_pattern, get_playlist, get_mixer, "
         "get_parameters, get_automation, find (search anything by name). inspect reads any object as the project file holds it, "
         "with every field; patch changes any field of any object (channel, pattern, mixer track, playlist track, clip, automation, "
         "marker, Stage room, arrangement, controller link, song settings). batch runs many calls as one undo step. "
         "set_parameters / get_values take parameter paths (song/tempo, channel/2/volume, mixer/3/slot/1/<param>). "
         "In the app, list_commands and run_command reach every menu item, keyboard command, window, settings page and plugin of "
         "the command palette; windows opens and closes windows; get_settings / set_setting read and change the user's settings "
         "(not the ones that decide what assistants, scripts, the network and files may do).\n"
         "Conventions: friendly tools use beats (quarter notes), numbers from 1 as on screen, mixer track 0 = master, plugin "
         "type ids (list_plugin_catalog); inspect / patch use the file's units (ticks, ids, colours as numbers or \"#RRGGBB\").\n"
         "help topics: overview, tools, paths, objects, <tool name>, <object kind> (channel, mixer, ...).";
}

ToolResult tHelp(Environment& env, const Json& a) {
  const std::string topic = argStr(a, "topic", "overview");
  if (topic == "overview" || topic.empty()) return ok(jobj({{"topic", jstr("overview")}, {"text", jstr(overviewText())}}), "help");
  if (topic == "paths")
    return ok(jobj({{"topic", jstr("paths")},
                    {"objects", jstr(kPathHelp)},
                    {"parameters", jstr("song/<param>, channel/N/<param>, mixer/N/<param>, mixer/N/slot/K/<param>; <param> is an API name "
                                        "(volume, pan, ...), a plugin parameter's number from 1 or its name")}}),
              "help paths");
  if (topic == "tools") {
    Json list = Json::makeArray();
    for (const auto& t : tools())
      list.array.push_back(jobj({{"name", jstr(t.name)}, {"title", jstr(t.title)}, {"edits", jbool(!t.readOnly)}}));
    return ok(jobj({{"topic", jstr("tools")}, {"tools", list}}), "help tools");
  }
  if (topic == "objects" || findKind(topic)) {
    Json list = Json::makeArray();
    for (const auto& k : kinds()) {
      if (topic != "objects" && topic != k.name) continue;
      Json f = Json::makeObject();
      for (const auto& [n, d] : k.fields) f.object[n] = jstr(d);
      list.array.push_back(jobj({{"kind", jstr(k.name)}, {"about", jstr(k.about)}, {"fields", f}}));
    }
    return ok(jobj({{"topic", jstr(topic)}, {"objects", list}, {"note", jstr("fields at their default are left out of inspect; patch sets them anyway")}}), "help objects");
  }
  if (const Tool* t = findTool(topic)) {
    Json ex = Json::makeArray();
    for (const auto& [n, list] : examples())
      if (topic == n)
        for (const char* e : list) {
          Json x;
          parse(e, x);
          ex.array.push_back(x);
        }
    Json j = jobj({{"topic", jstr(topic)}, {"description", jstr(t->description)}, {"inputSchema", t->inputSchema}, {"edits", jbool(!t->readOnly)}, {"examples", ex}});
    return ok(j, "help " + topic);
  }
  (void)env;
  return fail("no help topic \"" + topic + "\": overview, tools, paths, objects, a tool name or an object kind");
}

}  // namespace

// ============================================================== catalogue ====
void addMoreTools(std::vector<Tool>& v) {
  auto add = [&](const char* name, const char* title, const std::string& desc, Json schema, bool readOnly,
                 std::function<ToolResult(Environment&, const Json&)> fn) -> Tool& {
    Tool t;
    t.name = name;
    t.title = title;
    t.description = desc;
    t.inputSchema = std::move(schema);
    t.readOnly = readOnly;
    t.run = std::move(fn);
    v.push_back(std::move(t));
    return v.back();
  };
  auto ex = [](const char* tool) {
    for (const auto& [n, list] : examples())
      if (std::strcmp(n, tool) == 0 && !list.empty()) return std::string(" Example: ") + list[0];
    return std::string();
  };
  // K-92: the in-app assistant panel. The user types requests there; an assistant reads them,
  // answers in the panel, and at the Propose level sees whether its changes were accepted.
  add("assistant_inbox", "read the panel",
      "What the user typed in Gridline's AI assistant panel since you last asked (oldest first), and the permission level: "
      "\"read only\", \"propose\" (your changes wait for the user to accept them) or \"allow\". Poll it while you work "
      "with the user through the panel." + ex("assistant_inbox"),
      objSchema({{"all", boolP("Every line of the conversation, not only the unread ones")}}), true, [](Environment& env, const Json& args) {
        const Json* all = args.find("all");
        const bool every = all && all->is(Json::Type::Bool) && all->boolean;
        Json lines = Json::makeArray();
        uint64_t last = env.chatRead;
        for (const auto& c : env.chat) {
          if (!every && (!c.fromUser || c.id <= env.chatRead)) continue;
          lines.array.push_back(jobj({{"from", jstr(c.fromUser ? "user" : c.who)}, {"text", jstr(c.text)}}));
          if (c.fromUser) last = std::max(last, c.id);
        }
        env.chatRead = last;
        const char* level = !env.allowEdits ? "read only" : env.propose ? "propose" : "allow";
        return ok(jobj({{"messages", lines}, {"level", jstr(level)}, {"waitingProposals", jnum(static_cast<double>(env.proposals.size()))}}),
                  std::to_string(lines.array.size()) + " messages");
      });
  add("assistant_say", "answer in the panel",
      "Shows your text in Gridline's AI assistant panel, as your answer to the user (plain text, up to 4000 characters)." + ex("assistant_say"),
      objSchema({{"text", strP("What to tell the user", 4000)}}, {"text"}), true, [](Environment& env, const Json& args) {
        env.say(false, "assistant", args.text("text"));
        return ok(jobj({{"shown", jbool(true)}}), "said: " + args.text("text").substr(0, 60));
      });
  add("get_proposals", "proposed changes",
      "At the Propose level: your changes that still wait for the user (number, tool, what changes). A proposal that is gone was "
      "accepted or discarded." + ex("get_proposals"),
      objSchema({}), true, [](Environment& env, const Json&) {
        Json list = Json::makeArray();
        for (const auto& p : env.proposals) {
          Json lines = Json::makeArray();
          for (const auto& l : p.diff) lines.array.push_back(jstr(l));
          list.array.push_back(jobj({{"proposal", jnum(static_cast<double>(p.id))}, {"tool", jstr(p.tool)}, {"changes", lines}}));
        }
        return ok(jobj({{"proposals", list}, {"level", jstr(!env.allowEdits ? "read only" : env.propose ? "propose" : "allow")}}),
                  std::to_string(env.proposals.size()) + " waiting");
      });
  add("help", "help",
      "How to use Gridline's tools: an overview, the tool list, path syntax, the fields of every object kind, or one tool's "
      "description, schema and examples. Call it first when unsure.",
      objSchema({{"topic", strP("overview (default), tools, paths, objects, a tool name (\"patch\") or an object kind (\"channel\")", 64)}}), true, tHelp);
  add("find", "find anything",
      "Searches the project (channels, patterns, mixer tracks and their effects, playlist tracks, automation clips, markers, Stage "
      "rooms, arrangements), the plugin types and, in the app, its commands, by name. Returns paths for inspect / patch." +
          ex("find"),
      objSchema({{"query", strP("Text to look for (case and separators ignored)", 128)},
                 {"includePlugins", boolP("Also plugin types (default true)")},
                 {"includeCommands", boolP("Also the app's commands (default true)")},
                 {"limit", intP("Most hits (default 50)", 1, 500)}},
                {"query"}),
      true, tFind);
  add("inspect", "inspect an object",
      std::string("Reads any object of the project with every field it has, as the project file stores it (ticks, ids, colours as "
                  "numbers). ") +
          kPathHelp + ". Long lists show as their size unless full is true; fields: true adds what each field means." + ex("inspect"),
      objSchema({{"path", strP("Object path", 256)},
                 {"full", boolP("Show long lists in full")},
                 {"maxItems", intP("Lists longer than this show as their size (default 64)", 1, 100000)},
                 {"fields", boolP("Also describe the fields of this kind of object")}},
                {"path"}),
      true, tInspect);
  {
    Json patchItem = objSchema({{"path", strP("Object path", 256)}, {"set", prop("object", "Fields to change (null removes an optional field)")}, {"value", prop("string", "With a path to one field: its new value (any JSON type)")}}, {"path"});
    patchItem.object["properties"].object["value"] = jobj({{"type", jarr({jstr("string"), jstr("number"), jstr("boolean"), jstr("object"), jstr("array"), jstr("null")})}, {"description", jstr("With a path to one field: its new value (any JSON type)")}});
    Json value = jobj({{"type", jarr({jstr("string"), jstr("number"), jstr("boolean"), jstr("object"), jstr("array"), jstr("null")})}, {"description", jstr("With a path to one field (channel/2/hibernate): its new value, any JSON type")}});
    Json schema = objSchema({{"path", strP("Object path (see help topic paths)", 256)},
                             {"set", prop("object", "Fields to change, merged into the object: nested objects merge, lists and values replace, null removes")},
                             {"value", value},
                             {"patches", arrP("Several objects at once (one undo step)", patchItem, 200)}});
    add("patch", "change any object",
        std::string("Changes any fields of any object of the project in one undo step: the generic editor that reaches everything the "
                    "project file holds (channel note settings, echo, layers, MIDI ports, hibernate, mixer track strip, EQ, sends, "
                    "Stage depth and room, playlist track live settings, clip playback, markers, Stage rooms, controller links, song "
                    "settings). Units as inspect shows them. The reply lists ignored fields (not part of that object) and adjusted "
                    "values (corrected to the allowed range). ") +
            kPathHelp + "." + ex("patch"),
        schema, false, tPatch);
  }
  {
    Json call = objSchema({{"tool", strP("Tool name", 64)}, {"arguments", prop("object", "The tool's arguments")}}, {"tool"});
    add("batch", "batch of calls",
        "Runs up to 200 tool calls in order as one undo step. Without continueOnError the first failing call undoes the whole "
        "batch and says which call failed and why. Renders, exports, saves, undo and redo run on their own." +
            ex("batch"),
        objSchema({{"calls", arrP("The calls", call, 200)}, {"continueOnError", boolP("Keep going after a failing call (its changes are kept)")}}, {"calls"}), false,
        tBatch)
        .checksEdits = true;
  }
  {
    Json change = objSchema({{"path", strP("Parameter path: song/tempo, channel/2/volume, mixer/3/slot/1/Decay", 256)},
                             {"value", jobj({{"type", jarr({jstr("number"), jstr("boolean"), jstr("string")})}, {"description", jstr("Number in the parameter's units, true / false for toggles, or a choice's name")}})},
                             {"normalized", boolP("value is 0..1 of the range")}},
                            {"path", "value"});
    add("set_parameters", "set parameters",
        "Sets any number of parameters (song, channel strip and instrument, mixer track, effect slot) by path, as one undo step." + ex("set_parameters"),
        objSchema({{"changes", arrP("Parameter changes", change, 500)}}, {"changes"}), false, tSetParameters);
    add("get_values", "read parameters by path", "Reads parameters by path with value, display text, range and choices." + ex("get_values"),
        objSchema({{"paths", arrP("Parameter paths", strP("path", 256), 500)}}, {"paths"}), true, tGetValues);
  }
  add("transport", "play, stop, position",
      "Starts, stops or toggles playback, moves the song position (beats), and reports the transport, the current pattern and the "
      "selection." + ex("transport"),
      objSchema({{"action", enumP("status (default), play, stop or toggle", {"status", "play", "stop", "toggle"})},
                 {"position", numP("Move the song position here (beats)", 0, 1e7)}}),
      false, tTransport)
      .grouped = false;
  add("select", "select",
      "Sets what is selected in Gridline: channels, the current pattern, the mixer track, playlist clips, the playlist time "
      "selection (null clears it). Selection is not an undo step.",
      objSchema({{"channels", arrP("Channel numbers", intP("channel", 1, 999), 999)},
                 {"pattern", intP(kPatternD, 1, 999)},
                 {"mixer", intP(kMixerD, 0, 999)},
                 {"clips", arrP("Clip ids", intP("clip id", 1, 4294967295.0), 10000)},
                 {"timeSelection", jobj({{"type", jarr({jstr("object"), jstr("null")})}, {"description", jstr("{from, to} in beats, or null to clear")}})}}),
      false, tSelect)
      .grouped = false;
  add("duplicate", "duplicate",
      "Duplicates a channel (with its notes and steps in every pattern), a pattern, or playlist clips (placed right after them). One undo step.",
      objSchema({{"channel", intP(kChannelD, 1, 999)}, {"pattern", intP(kPatternD, 1, 999)}, {"clips", arrP("Clip ids", intP("clip id", 1, 4294967295.0), 10000)}}),
      false, tDuplicate);
  add("edit_notes", "edit notes",
      "Piano-roll operations on one channel's notes in a pattern, optionally only notes starting in from..to (beats) and within a "
      "pitch range: quantize (amount = grid in beats, ends), legato, transpose (semitones), nudge (beats), scale_velocity (factor), "
      "humanize (amount = timing in beats, velocityRange), strum / arpeggiate (beats per note), chop (beats), glue, flip_vertical, "
      "flip_horizontal, duplicate (grid in beats), delete. One undo step." +
          ex("edit_notes"),
      objSchema({{"pattern", intP(kPatternD, 1, 999)},
                 {"channel", intP(kChannelD, 1, 999)},
                 {"op", enumP("Operation", {"quantize", "legato", "transpose", "nudge", "scale_velocity", "humanize", "strum", "arpeggiate", "chop", "glue",
                                            "flip_vertical", "flip_horizontal", "duplicate", "delete"})},
                 {"amount", numP("The operation's amount (see the description)")},
                 {"ends", boolP("quantize: also the note ends")},
                 {"velocityRange", intP("humanize: velocity range", 0, 127)},
                 {"seed", intP("humanize: random seed", 0, 2147483647)},
                 {"from", numP("Only notes starting at or after this beat", 0)},
                 {"to", numP("Only notes starting before this beat", 0)},
                 {"lowestPitch", intP("Only notes at or above this pitch", 0, 127)},
                 {"highestPitch", intP("Only notes at or below this pitch", 0, 127)}},
                {"pattern", "channel", "op"}),
      false, tEditNotes);
  {
    Json pt = objSchema({{"position", numP("Beats from the clip start", 0, 1e7)},
                         {"value", numP("0..1 of the clip's range", 0, 1)},
                         {"tension", numP("Curve tension of the segment ending here", -1, 1)},
                         {"shape", enumP("Segment shape", shapeNames())}},
                        {"position", "value"});
    add("edit_automation", "edit automation",
        "Changes an automation clip: name, colour, range (min, max), loop length (beats), replaces its points or adds points (with "
        "curve shapes and tension), or removes the points in removeFrom..removeTo (beats). One undo step.",
        objSchema({{"automation", intP("Automation clip (1 = first, get_automation)", 1, 100000)},
                   {"name", strP("Name")},
                   {"color", strP("Colour \"#RRGGBB\"", 7)},
                   {"min", numP("Range start", 0, 1)},
                   {"max", numP("Range end", 0, 1)},
                   {"loop", numP("Repeat length in beats (0 none)", 0, 1e6)},
                   {"points", arrP("Replace every point", pt, 10000)},
                   {"addPoints", arrP("Points to add", pt, 10000)},
                   {"removeFrom", numP("Remove points from this beat", 0)},
                   {"removeTo", numP("... up to this beat", 0)}},
                  {"automation"}),
        false, tEditAutomation);
  }
  add("remove_automation", "remove automation", "Deletes an automation clip and its playlist placements. One undo step.",
      objSchema({{"automation", intP("Automation clip (1 = first)", 1, 100000)}}, {"automation"}), false, tRemoveAutomation)
      .destructive = true;
  add("add_mixer_track", "add mixer track", "Adds an insert track to the mixer (sending to the master), optionally named. One undo step.",
      objSchema({{"name", strP("Track name")}}), false, tAddMixerTrack);
  add("remove_mixer_track", "remove mixer track", "Removes an insert track; its channels and senders go to the master. One undo step.",
      objSchema({{"mixer", intP("Insert track (1..)", 1, 999)}}, {"mixer"}), false, tRemoveMixerTrack)
      .destructive = true;
  add("set_send", "set a send",
      "Adds or changes a send from one mixer track to another (level 0..1.25; 0 removes it), as sidechain only, pre-fader or a Stage "
      "room send. Loops are refused. One undo step." + ex("set_send"),
      objSchema({{"mixer", intP("From this mixer track", 0, 999)},
                 {"to", intP("To this mixer track", 0, 999)},
                 {"level", numP("Send level (default 1, 0 removes the send)", 0, 1.25)},
                 {"sidechain", boolP("Feeds the destination's sidechain input only")},
                 {"preFader", boolP("Taps before the fader")},
                 {"stageRoom", boolP("The track's Stage room send")}},
                {"mixer", "to"}),
      false, tSetSend);
  add("stage_room", "Stage rooms",
      "Stage view rooms: add (name, color), rename, color, move (to), delete (an empty room), move_tracks (mixer track numbers, "
      "feeders; or marker: the tracks of every clip playing from that marker to the next or to markerTo, into room, else a new room "
      "named after the marker), find_tracks (the same tracks, listed), set_space (space number, 0 = no room). Room settings are patch "
      "paths room/N. One undo step." + ex("stage_room"),
      objSchema({{"action", enumP("What to do", {"add", "rename", "color", "move", "delete", "move_tracks", "find_tracks", "set_space"})},
                 {"room", intP("Room (1 = first)", 1, 999)},
                 {"name", strP("Name")},
                 {"color", strP("Colour \"#RRGGBB\"", 7)},
                 {"to", intP("move: new position", 1, 999)},
                 {"tracks", arrP("move_tracks: mixer track numbers", intP("mixer track", 1, 999), 999)},
                 {"feeders", boolP("move_tracks: the tracks sending into them come too")},
                 {"marker", intP("move_tracks / find_tracks: the tracks of the clips playing from this marker (1 = first) to the next", 1, 100000)},
                 {"markerTo", intP("with marker: up to this marker instead of the next", 1, 100000)},
                 {"buses", boolP("with marker: the buses they send into come too (default true)")},
                 {"muted", boolP("with marker: muted clips count too")},
                 {"space", intP("set_space: the room's space (0 = no room)", 0, 64)}},
                {"action"}),
      false, tStageRoom);
  add("arrangement", "arrangements",
      "Lists the playlist arrangements, or adds (name, copyCurrent), selects, renames, moves (to) or removes one. One undo step." + ex("arrangement"),
      objSchema({{"action", enumP("list (default), add, select, rename, move, remove", {"list", "add", "select", "rename", "move", "remove"})},
                 {"arrangement", intP("Arrangement (1 = first)", 1, 999)},
                 {"to", intP("move: new position (1 = first)", 1, 999)},
                 {"name", strP("Name")},
                 {"copyCurrent", boolP("add: start as a copy of the current arrangement")}}),
      false, tArrangement);
  add("list_commands", "list app commands",
      "Everything Gridline's command palette offers: menu items, keyboard commands, windows, settings pages and plugins, ranked "
      "for the query, with their id, where they live, shortcut, on / off state and why they cannot run. Use the id with "
      "run_command. Without the app it lists the keyboard commands only." + ex("list_commands"),
      objSchema({{"query", strP("Search words (typos are tolerated)", 128)},
                 {"kind", enumP("Only this kind", {"command", "window", "setting", "plugin"})},
                 {"offset", intP("Skip this many", 0, 100000)},
                 {"limit", intP("Most rows (default 40)", 1, 500)}}),
      true, tListCommands);
  add("run_command", "run an app command",
      "Runs one command of the app (a menu item, keyboard command, window, settings page or plugin row from list_commands), as the "
      "user choosing it. Edits it makes are one undo step. Commands that close or replace the open project are refused." + ex("run_command"),
      objSchema({{"id", strP("Command id from list_commands", 512)}}, {"id"}), false, tRunCommand);
  add("windows", "windows", "Lists Gridline's windows and whether each is open; opens or closes one by name.",
      objSchema({{"open", strP("Open this window", 128)}, {"close", strP("Close this window", 128)}}), false, tWindows)
      .grouped = false;
  add("get_settings", "read settings",
      "The user's Gridline settings: key, type, value, default and access (read-write, user only, hidden). Filter by prefix "
      "(\"ui.stage.\") or query." + ex("get_settings"),
      objSchema({{"prefix", strP("Only keys starting with this", 128)}, {"query", strP("Only keys containing this", 128)}, {"limit", intP("Most rows (default 100)", 1, 2000)}}),
      true, tGetSettings);
  add("set_setting", "change a setting",
      "Changes one of the user's settings (saved at once). Settings that decide what assistants, scripts, the network and files may "
      "do, and secrets, stay with the user." + ex("set_setting"),
      objSchema({{"key", strP("Setting key", 128)}, {"value", jobj({{"type", jarr({jstr("boolean"), jstr("number"), jstr("string")})}, {"description", jstr("New value (bool, number or string, as the setting's type)")}})}},
                {"key", "value"}),
      false, tSetSetting)
      .grouped = false;
}

}  // namespace gl::mcp::detail

namespace gl::mcp {

// ============================================================== settings ====
// Who may touch a setting: secrets stay hidden, the switches that decide what assistants, scripts,
// the network and files may do stay with the user.
const std::vector<std::string>& settingAccessRules() {
  static const std::vector<std::string> v = {"mcp.", "osc.", "collab.", "scripts.", "update.", "suite.", "import.", "file.", "files.", "audio.inputDevice",
                                             "audio.jack.", "backup.location", "backup.autosave", "debug.logToFile", "logger.enabled", "general.startup", "keys.bindings"};
  return v;
}

SettingAccess settingAccess(const std::string& k) {
  auto has_ = [&](const char* part) { return k.find(part) != std::string::npos; };
  if (has_("token") || has_("password") || has_("secret")) return SettingAccess::Hidden;
  for (const auto& p : settingAccessRules())
    if (k.compare(0, p.size(), p) == 0) return SettingAccess::ReadOnly;
  return SettingAccess::ReadWrite;
}

}  // namespace gl::mcp
