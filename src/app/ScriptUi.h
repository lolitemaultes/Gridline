// Gridline — scripting in the app (J-24): the piano roll "Scripts" menu and
// script tool window (live preview, Accept / Cancel, one undo step), the
// controller-script runtime per MIDI device, and the script console.
#pragma once
#include <string>
#include <vector>
#include "../core/PianoRollScript.h"
#include "../core/Remote.h"

namespace gl {

class App;

// Folders: shipped <assets>/scripts/<kind>, user <config>/scripts/<kind>.
std::string userScriptsDir(const App& a, const char* kind);
std::string shippedScriptsDir(const App& a, const char* kind);

// ---- piano roll scripts ----
const std::vector<script::ScriptInfo>& pianoRollScripts(App& a, bool rescan = false);
void scriptsMenuItems(App& a);  // body of the piano roll's "Scripts" menu
void openPianoRollScript(App& a, const std::string& path);
const std::string& lastPianoRollScript();  // "" until a script was opened (J-27)
bool scriptToolOpen();
void drawScriptTool(App& a);  // per frame (from the piano roll)

// ---- controller scripts ----
// Controller types "script:<id>" in Settings → MIDI select a script.
const std::vector<script::ScriptInfo>& controllerScripts(App& a, bool rescan = false);
// Runs the device's script on a message; true when the script handled it.
bool controllerScriptMidi(App& a, const std::string& device, const MidiMessage& m);
// Per frame: loads / unloads device scripts, onIdle, onRefresh.
void controllerScriptsTick(App& a);
// Unloads every device script (onDeinit); called on reload and shutdown.
void shutdownControllerScripts();
// Status line for a device ("running", the error, …).
std::string controllerScriptStatus(const std::string& device);

// ---- console (Settings → Debug) ----
void drawScriptConsole(App& a, float height);

// Development (--script <id>): opens a piano roll script for screenshots.
void devOpenScript(App& a, const std::string& id);

}  // namespace gl
