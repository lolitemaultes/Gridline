# Gridline

A clean-room, dependency-free C++17 DAW core targeting **alternative operating systems**.
It reimplements a familiar *workflow model*, like the channel rack, step sequencer,
piano roll, playlist arrangement, mixer.

## Build

```sh
./build.sh                 # macOS/Linux quick build + tests + demo render
# or with CMake (Linux release):
cmake -B build && cmake --build build -j
ctest --test-dir build
```

## Use

```sh
build/gridline info    demo/song.json          # inspect a project
build/gridline render  demo/song.json -o out.wav --sr 44100 --bits 24
build/gridline upgrade old.json -o new.json    # rewrite in the current schema
```

## Architecture

| Module | Role | Status |
|---|---|---|
| `core/Types`, `core/Song` | model: channels, patterns, clips with stable `ObjectId`s; per-project PPQ (24–960) | ✅ M1 |
| `core/Document`, `core/Undo` | the only way to edit: every change is an undoable `Command` (merging, groups, level limit, history) | ✅ M1 |
| `core/Params` | `ParamId` addressing, descriptors, smoother, lock-free param bus | ✅ M1 |
| `core/Project` | JSON schema v2 with migration from v1, validation, atomic save | ✅ M1 |
| `core/Backup`, `core/Settings` | timed backups with rotation; XDG settings store | ✅ M1 |
| `core/Synth` + `CreateGridlinePlugInstance` | built-in generators behind a plain C factory | ✅ M0 |
| `core/Engine`, `core/Mixer` | offline render: playlist → events → voices → strips → master | ✅ M0 |
| `core/WavWriter`, `core/Json`, `cli` | WAV export, JSON, command line | ✅ M0 |
| Realtime engine (PipeWire/JACK/ALSA) | M2 | ⬜ |
| UI toolkit & shell | M3 | ⬜ |

### Plugin hosting (VST3, CLAP, VST2, AU)

`src/host/` hosts third-party plugins through one interface (`host::Backend`):

| Format | Type id | Folders | State in `PluginState::data` |
|---|---|---|---|
| VST3 | `vst3:<class uid>` | `~/Library/Audio/Plug-Ins/VST3`, `/Library/…/VST3`; Linux `~/.vst3`, `/usr/lib/vst3` | `vst3s1:<component>:<controller>` |
| CLAP | `clap:<plugin id>` | `…/Plug-Ins/CLAP`; Linux `~/.clap`, `/usr/lib/clap`; `$CLAP_PATH` | `clap1:<state>` |
| VST2 | `vst2:<unique id, 8 hex>` | `…/Plug-Ins/VST`; Linux `~/.vst`, `/usr/lib/vst`; `$VST_PATH` | `vst2c1:<chunk>` or `vst2p1:<parameters>` |
| AU (macOS) | `au:<type>/<subtype>/<manufacturer>` | the system's component registry | `au1:<ClassInfo plist>` |

Every plugin is scanned in a child process (`gridline --scan-plugin <format> <entry>`;
crashes/hangs are blacklisted until the plugin changes) into `<config>/plugin-cache.json`.
Parameters are ordinary `ParamId`s (automation, MIDI learn, controller links); MIDI
controllers, pitch bend and pressure from a keyboard reach the selected instrument
(VST3 through `IMidiMapping`, the others natively); the plugin's own programs appear as
its factory presets; extra outputs of multi-output instruments can be sent to mixer
tracks (plugin window → Outputs). Editors open in their own window (Cocoa on macOS;
X11 embedding on Linux).

**Sandbox.** A plugin set to *Run in sandbox* (plugin window or Options → Plugin
manager) runs in a child process (`gridline --plugin-sandbox …`): audio, parameter
changes and notes go through shared memory with a lock-free request/done handshake (the
audio thread waits a few block lengths at most), everything else through a socket. If the
plugin crashes, Gridline keeps running, the plugin is silenced, its last saved state stays
in the project and the plugin window offers *Reload plugin*. Its editor opens in the
child's own window (macOS). `GRIDLINE_SANDBOX_ALL=1` sandboxes every plugin (testing).

**What plugins get from the host.** Transport for every format (tempo, song position,
bar start and number, time signature, loop range, playing / recording, MIDI clock
distance, system time); sample-accurate notes and parameter changes (plus VST3 / CLAP
note expressions: tuning for glides, pan and brightness from piano-roll note properties);
the mixer's sidechain sends on the plugin's sidechain bus; offline mode during exports
(VST3 `kOffline`, CLAP `render`, AU `OfflineRender`, VST2 offline process level);
latency changes (restart → re-activation → delay compensation rebuilds); CLAP parameter
modulation (Processing → *Links modulate parameters*: the plugin keeps the song's value,
links and automation arrive as `PARAM_MOD`), CLAP preset discovery + preset load (the
wrapper's preset browser lists programs and library presets), plugin bypass (VST3 / CLAP
bypass parameters, VST2 soft bypass, AU `BypassEffect`). Notes and MIDI a plugin sends
(arpeggiators, chord tools; VST3 event output, CLAP note ports, VST2 `processEvents`,
AU MIDI output callback) play every channel whose **MIDI input port** matches the
plugin channel's **MIDI output port** (Settings page; *Detect* shows what it sends).

**Plugin window (wrapper).** Header: plugin editor, generic editor switch, sandbox,
preset browser (programs + library), CPU (average / peak) and latency readouts. Pages:
*Parameters* (searchable generic editor), *Processing* (fixed size buffers — the plugin
always processes N samples, N more samples of compensated latency; process maximum size
buffers — one call per engine block with notes placed inside it, off: the block is cut at
notes; notify about rendering mode; links as modulation (CLAP); run in sandbox) and
*Settings* (MIDI ports and detection, sidechain sources, extra outputs, plugin facts).
Processing options are stored per plugin type in `plugin-manager.json`.

**Compatibility sweep.** `gridline plugin-check --all [--json report.json]` runs every
scanned class in its own child process through the same Backend code the engine uses
(audio on its own thread): instantiate; 44.1 / 48 / 96 kHz with blocks of 32…2048 and
random sizes; silence, noise and notes; NaN / infinity / denormals / CPU spikes; state
save → restore → state and output bit-compare; every parameter to 0 / 0.5 / 1; program
change; bypass; offline mode; latency / tail; open, idle, close and reopen the editor in a
window that is never shown; close. Prints a table, writes the JSON report.

**Plugin manager** (Options → Plugin manager): scan / rescan, formats on or off, extra
search folders, the plugin list with favourites, user categories and the sandbox switch,
failed plugins with their reason and *Retry*.

**FL Studio import.** "Fruity Wrapper" instances in `.flp` / `.fst` files are matched to
installed plugins by VST3 class id, VST2 unique id or AU description (then by name), and
their state is restored (VST2 chunk or parameter list, VST3 component + controller state,
AU ClassInfo). FL's own effects wrapped as VST plugins in old projects become Gridline's
built-in equivalents; anything else that is not installed is kept as a placeholder with
its state (it loads once the plugin is installed) and listed in the import report.

```sh
build/gridline scan-plugins [--force]                 # fill the plugin cache now (the app also scans at startup)
build/gridline list-plugins [vst3|clap|vst2|au]       # what the cache knows
build/gridline plugin-smoke <format> <entry> [--sandbox]   # load, play, MIDI, programs, save/restore every class
build/gridline plugin-check --all --json report.json  # compatibility sweep (one child process per class)
build/gridline flp-plugins <file.flp>...              # wrapped plugins in FL files and what they map to here
```

Interfaces: Steinberg's VST 3 SDK interface headers (`third_party/vst3/pluginterfaces`, MIT,
v3.8.1, unmodified) and the CLAP headers (`third_party/clap`, MIT, 1.2.2, unmodified). VST2
uses Gridline's own clean-room description of the binary interface
(`src/host/vst2/Vst2Abi.h`, written from public format notes; no Steinberg VST2 SDK code).
No plugin binaries are shipped; the tests use tiny in-tree plugins (`tests/vst3`,
`tests/clap`, `tests/vst2`) and, on macOS, Apple's built-in Audio Units.

Project files are JSON (`"format": "gridline-project"`, `"version": 2`). Older files
are migrated on load. Pattern clips sit on playlist tracks, and step rows and piano-roll
notes coexist per channel.

## Roadmap

Status: `../TASKS.md`. Full checklist: `../FEATURES.md`. Specification: `../FL_STUDIO_STRUCTURE.md`.

## License

Gridline is free software, released under the GNU General Public License v3.0 (see `LICENSE`).
Bundled third-party libraries keep their own licences (all GPL-compatible): Dear ImGui (MIT),
Lua 5.4 (MIT), RtMidi (MIT-style), CLAP headers (MIT), VST3 plug-in interfaces (MIT), dr_libs and stb_vorbis
(public domain / MIT-0) — see each folder under `third_party/`.

Gridline is an independent project and is not affiliated with or endorsed by Image-Line.
"FL Studio" is a trademark of Image-Line Software; it is mentioned only to describe file-format
compatibility.
