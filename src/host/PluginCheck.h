// Gridline — `gridline plugin-check`: a compatibility sweep over hosted plugins.
//
//   gridline plugin-check [--all | <type | name>…] [--json <report.json>]
//                         [--timeout <seconds>] [--no-editor] [--format <id>]
//
// Every class runs in its own child process (`plugin-check --child …`) so a
// crash or a hang only fails that plugin. The child talks to the plugin
// through host::Backend exactly like the engine does — processing on a
// separate audio thread, UI calls on the main thread — and checks:
// instantiation; prepare at 44.1 / 48 / 96 kHz; blocks of 32…2048 frames and
// random sizes; silence, noise and MIDI notes; NaN / infinity / denormals and
// CPU spikes; state save → restore → state and output bit-compare; a sweep
// of every parameter to 0, 0.5 and 1; program change; bypass; offline mode;
// latency and tail; opening, idling, closing and reopening the editor in a
// window that is never shown; closing the instance. Each check is pass,
// warn, fail or skip; the parent prints a table and writes a JSON report.
#pragma once

namespace gl::host {

// Both modes; returns the process exit code (0: no plugin failed).
int pluginCheckMain(int argc, char** argv);

}  // namespace gl::host
