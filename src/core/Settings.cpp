#include "Settings.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "DrawSmooth.h"
#include "Project.h"
#include "Platform.h"

namespace gl {

namespace fs = std::filesystem;

namespace {
Json b(bool v) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = v;
  return j;
}
}  // namespace

Settings::Settings() {
  defaults_ = {
      {"undo.maxLevels", Json::makeNumber(100)},
      {"backup.frequency", Json::makeNumber(3)},  // BackupFrequency (Backup.h)
      {"backup.maxCount", Json::makeNumber(100)},
      {"backup.location", Json::makeString("global")},  // "global" | "project"
      {"project.defaultPpq", Json::makeNumber(96)},
      {"project.defaultTempo", Json::makeNumber(140)},
      {"import.flData", Json::makeString("")},  // FL Studio factory data folder ("" = detect)
      {"audio.sampleRate", Json::makeNumber(44100)},
      {"audio.bufferFrames", Json::makeNumber(256)},
      {"audio.pdc", b(true)},                  // plugin delay compensation
      {"audio.threads", Json::makeNumber(0)},  // mixer processing threads (0 = automatic)
      // Audio input & recording (J-8).
      {"audio.inputDevice", Json::makeString("")},     // "" = none, "*" = system default
      {"audio.inputChannels", Json::makeNumber(2)},
      {"audio.inputOffsetMs", Json::makeNumber(0)},    // manual recording offset (+ = later)
      {"audio.recordBits", Json::makeNumber(32)},      // 32 = float, 24 / 16 = PCM
      {"master.targetLufs", Json::makeNumber(-14)},    // RN-09: master chain loudness target
      // J-31: recording extras, audio logger, piano roll extras, quantized jumps.
      {"record.autoUnarm", b(false)},                  // disarm the tracks after a recording
      {"record.ignoreSilent", b(false)},               // takes quieter than -60 dBFS are not kept
      {"record.naming", Json::makeNumber(0)},          // clip names: 0 insert name, 1 input name
      {"record.nameTracks", b(true)},                  // name empty playlist tracks after the take
      {"logger.enabled", b(false)},                    // audio logger keeps the last minutes
      {"logger.minutes", Json::makeNumber(2)},         // 1 / 2 / 5 / 10
      {"logger.source", Json::makeNumber(0)},          // AudioLogger::Source: 0 armed inputs, 1 master
      // J-79: playlist audio clip editing toggles (global, like FL's toolbar switches).
      {"playlist.stretchResize", b(false)},            // resizing audio clips time-stretches them
      {"playlist.showFades", b(true)},                 // fade + volume handles on audio clips
      {"playlist.zeroCross", b(false)},                // audio clip edits land on zero crossings
      {"piano.snapIncomingMidi", b(false)},            // played notes land on the highlighted scale
      {"piano.editableGhosts", b(false)},              // clicking a ghost note edits it in place
      {"piano.followKeyMarkers", b(true)},             // scale highlighting follows key markers
      {"transport.jumpQuantize", Json::makeNumber(0)}, // ops::jumpGrid mode (0 = jump at once)
      {"transport.rememberSeek", b(true)},             // J-49: Stop returns to the last set song position
      {"ui.scale", Json::makeNumber(1.0)},
      {"ui.scaleAuto", b(true)},             // J-91: ui.scale is multiplied by the desktop's scale (core/UiScale.h)
      {"ui.detachWindows", b(true)},         // J-113: windows can be dragged out of the main window
      {"ui.animations", b(true)},            // J-91: decorative interface animations (metronome tick, light fades, glides)
      {"ui.showHints", b(true)},
      {"ui.confirmAutoRevert", b(true)},     // J-77: unconfirmed display / audio changes go back after 15 s
      {"ui.autoScroll", b(true)},              // editors follow the song position while playing
      {"ui.smoothSnap", b(true)},              // J-37: snapped drags glide from cell to cell (drawing only)
      {"ui.smoothScroll", b(true)},            // J-67: wheel scrolling glides to its step (trackpads stay 1:1)
      {"ui.elasticEdges", b(true)},            // J-67: scrolling past an edge stretches a little and springs back
      {"ui.darkTheme", b(true)},
      // J-55: Settings > Theme (app/ThemeSettings.h).
      {"theme.mode", Json::makeString("")},          // "dark" | "light" | "system" ("" = ui.darkTheme)
      {"theme.accent", Json::makeString("")},        // "#RRGGBB" overrides ("" = the theme's role colour)
      {"theme.hot", Json::makeString("")},
      {"theme.led", Json::makeString("")},
      {"theme.playhead", Json::makeString("")},
      {"theme.contrast", Json::makeNumber(0)},       // -1 .. 1
      {"theme.brightness", Json::makeNumber(0)},     // -1 .. 1
      {"ui.mixerWheel", Json::makeNumber(2)},  // J-83: wheel over mixer controls: 0 adjust, 1 scroll the mixer, 2 auto (scroll when narrow)
      {"ui.mixerLayout", Json::makeNumber(1)},  // mixer strip width: 0 compact, 1 normal, 2 wide
      {"ui.layoutVersion", Json::makeNumber(0)},  // J-28: default workspace version the saved layout.ini was built with
      // J-24: general, file, project defaults, backup / autosave, hints, debug, scripting.
      {"general.startup", Json::makeString("template")},   // "template" | "empty" | "last"
      {"general.defaultTemplate", Json::makeString("")},   // "<Category>/<Name>" ("" = built-in starter kit)
      {"general.language", Json::makeString("en")},        // i18n code ("en", "pseudo", catalogue stem)
      {"keys.bindings", Json::makeString("")},             // J-55: shortcut overrides, JSON (keys::Keymap::serialize)
      {"file.scriptEditor", Json::makeString("")},         // command that opens a script ("" = system default)
      {"project.defaultNum", Json::makeNumber(4)},
      {"project.defaultDen", Json::makeNumber(4)},
      {"project.defaultAuthor", Json::makeString("")},
      {"project.defaultPatternBars", Json::makeNumber(1)},
      {"backup.autosave", b(false)},                       // also save the project file itself (saved projects only)
      {"backup.autosaveMinutes", Json::makeNumber(5)},
      {"ui.hintDelay", Json::makeNumber(0.5)},             // tooltip delay, seconds
      {"debug.logLevel", Json::makeNumber(2)},             // logging::Level: 0 errors … 3 verbose
      {"debug.logToFile", b(false)},                       // <config>/gridline.log
      {"scripts.enabled", b(true)},
      {"scripts.allowTransport", b(false)},  // J-57: project scripts may start / stop / move the transport
      {"scripts.maxSeconds", Json::makeNumber(2)},         // per script run / controller hook
      {"scripts.maxMillionInstructions", Json::makeNumber(20)},
      // SC-07: MCP server for AI assistants (Settings > Integrations); off until the user opts in.
      {"mcp.enabled", b(false)},                 // listen on 127.0.0.1 for assistants
      {"mcp.allowEdits", b(false)},              // assistants may change the project and write files
      {"mcp.port", Json::makeNumber(47390)},     // loopback port (0 = any free port)
      {"mcp.token", Json::makeString("")},       // bearer token ("" = generated on first start)
      {"mcp.exportFolder", Json::makeString("")},  // extra folder assistants may write renders / exports to
      // J-48: interface sounds (engine/UiSounds.h); per-event switches "sounds.<event>".
      {"sounds.enabled", b(true)},
      {"sounds.volume", Json::makeNumber(0.7)},
      {"sounds.startup", b(true)},
      {"sounds.open", b(true)},
      {"sounds.save", b(true)},
      {"sounds.export", b(true)},
      {"sounds.import", b(true)},
      {"sounds.task_done", b(true)},
      {"sounds.warning", b(false)},
      {"sounds.error", b(true)},
      {"sounds.record_start", b(false)},
      {"sounds.record_stop", b(false)},
      // J-89: update check (core/Update.h, docs/updates.md).
      {"update.auto", b(true)},                     // check GitHub for a newer release at startup (once a day)
      {"update.beta", b(false)},                    // include prereleases
      {"update.lastCheck", Json::makeNumber(0)},    // seconds since 1970 of the last check (0 = never)
      {"update.skipVersion", Json::makeString("")}, // "Skip this version"
      {"update.remindAfter", Json::makeNumber(0)},  // "Remind me later": not offered again before this time
      // J-111: collaboration (src/collab, docs/collaboration.md). The profile itself lives in <config>/gridline/collab.
      {"collab.stunHost", Json::makeString("stun.l.google.com")},  // address discovery ("" = same network only)
      {"collab.stunPort", Json::makeNumber(19302)},
      {"collab.turnHost", Json::makeString("")},    // optional relay server (password: secret store)
      {"collab.turnPort", Json::makeNumber(3478)},
      {"collab.turnUser", Json::makeString("")},
      {"collab.portMapping", b(true)},              // ask the router for a temporary port (UPnP-IGD / NAT-PMP / PCP)
      {"collab.showCursors", b(true)},              // draw collaborators' cursors
      {"collab.inviteMinutes", Json::makeNumber(60)},  // invite lifetime
      {"collab.latestVersion", Json::makeString("")},  // newest eligible release seen by the collaboration version check
      {"collab.latestCheckedAt", Json::makeNumber(0)},  // when (seconds since 1970)
  };
  // J-99: smooth freehand drawing (core/DrawSmooth.h): one amount, a switch per surface.
  defaults_["draw.smoothAmount"] = Json::makeNumber(drawsmooth::kDefaultAmount);
  for (const char* s : drawsmooth::kSurfaces) defaults_[std::string("draw.smooth.") + s] = b(true);
  values_ = defaults_;
}

std::string Settings::defaultPath() {
  // GRIDLINE_CONFIG_HOME (J-68): an isolated config root for automated runs
  // (screenshots, scripted runs) that does not touch other tools' XDG configuration;
  // else $XDG_CONFIG_HOME, %APPDATA% (Windows) or ~/.config (see core/Platform.h).
  return (fs::path(platform::configRoot()) / "gridline" / "settings.json").string();
}

bool Settings::load(const std::string& path, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return true;  // first run
  std::stringstream ss;
  ss << is.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) {
    if (err) *err = "settings file is not valid JSON: " + path;
    return false;
  }
  if (j.num("version", 1) > kVersion) {
    if (err) *err = "settings were written by a newer Gridline";
    return false;
  }
  if (const Json* v = j.find("values"); v && v->is(Json::Type::Object))
    for (const auto& [k, val] : v->object) {
      auto d = defaults_.find(k);
      if (d == defaults_.end() || d->second.type == val.type) values_[k] = val;
    }
  return true;
}

bool Settings::save(const std::string& path, std::string* err) const {
  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-settings");
  j.object["version"] = Json::makeNumber(kVersion);
  Json v = Json::makeObject();
  v.object = values_;
  j.object["values"] = std::move(v);
  return writeFileAtomic(path, jsonStringify(j) + "\n", err);
}

const Json* Settings::lookup(const std::string& key, Json::Type t) const {
  auto it = values_.find(key);
  if (it != values_.end() && it->second.type == t) return &it->second;
  auto d = defaults_.find(key);
  return d != defaults_.end() && d->second.type == t ? &d->second : nullptr;
}

bool Settings::getBool(const std::string& key) const {
  const Json* j = lookup(key, Json::Type::Bool);
  return j && j->boolean;
}
double Settings::getNumber(const std::string& key) const {
  const Json* j = lookup(key, Json::Type::Number);
  return j ? j->number : 0.0;
}
std::string Settings::getString(const std::string& key) const {
  const Json* j = lookup(key, Json::Type::String);
  return j ? j->str : std::string();
}

bool Settings::set(const std::string& key, bool v) {
  auto d = defaults_.find(key);
  if (d == defaults_.end() || d->second.type != Json::Type::Bool) return false;
  values_[key] = b(v);
  return true;
}
bool Settings::set(const std::string& key, double v) {
  auto d = defaults_.find(key);
  if (d == defaults_.end() || d->second.type != Json::Type::Number) return false;
  values_[key] = Json::makeNumber(v);
  return true;
}
bool Settings::set(const std::string& key, const std::string& v) {
  auto d = defaults_.find(key);
  if (d == defaults_.end() || d->second.type != Json::Type::String) return false;
  values_[key] = Json::makeString(v);
  return true;
}

}  // namespace gl
