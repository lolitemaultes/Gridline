<p align="center">
  <img src="docs/images/banner.png" alt="Gridline" width="100%">
</p>

<p align="center">
  <img alt="Version 0.6.0" src="https://img.shields.io/badge/version-0.6.0-0B0B0B?style=flat-square&labelColor=1F1F1F">
  <img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-0B0B0B?style=flat-square&labelColor=1F1F1F">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-0B0B0B?style=flat-square&labelColor=1F1F1F">
  <img alt="Plugins" src="https://img.shields.io/badge/plugins-VST3%20%7C%20CLAP%20%7C%20VST2%20%7C%20AU%20%7C%20LV2-0B0B0B?style=flat-square&labelColor=1F1F1F">
</p>

<p align="center">
  <b>A free, open source music workstation built around the grid.</b><br>
  Channel rack, step sequencer, piano roll, playlist and mixer,<br>
  written from scratch in C++17 and built to run everywhere.
</p>

<br>

<p align="center">
  <img src="docs/images/workspace.png" alt="The Gridline workspace: browser, playlist arrangement and mixer" width="100%">
</p>

## Highlights

<table>
<tr>
<td width="50%" valign="top">

**Pattern workflow.** Build loops in the channel rack, arrange them as clips in the playlist and mix them on inserts. Patterns, audio and automation all live on the same tracks.

**128 built-in instruments and effects.** Synths from subtractive to six-operator FM, additive and resynthesis, physical models, samplers and slicers, drum machines, reverbs, delays, dynamics, EQs and modulators. Every one has its own editor.

**Real plugins, real interfaces.** VST3, CLAP, VST2 and AU instruments and effects open with their own interface, can run sandboxed in a separate process, and are checked by a built-in compatibility sweep.

</td>
<td width="50%" valign="top">

**A mixer that keeps time.** Plugin delay compensation, parallel processing on several cores, sends, sidechains, groups, stems and a per-track EQ. Offline renders are bit-identical whatever the buffer size.

**Recording and editing.** Audio input with latency compensation, takes and comping, an audio editor with spectral view, automation clips with curves and an LFO tool, MIDI learn on every control.

**Scripting and templates.** Sandboxed Lua scripts for the piano roll and for hardware controllers, project templates, and a background sample browser with tempo and key search.

</td>
</tr>
</table>

## A closer look

<table>
<tr>
<td width="50%"><img src="docs/images/piano-roll.png" alt="Piano roll"></td>
<td width="50%"><img src="docs/images/chord-editor.png" alt="Chord progression editor"></td>
</tr>
<tr>
<td><b>Piano roll.</b> Slides, per-note properties, ghost notes, scale highlighting and a full set of tools: quantize, chop, strum, arpeggiate, randomize and more.</td>
<td><b>Chord progressions.</b> Pick a key, generate or type a progression, shape the rhythm and voicing, and write it straight into the pattern.</td>
</tr>
<tr>
<td width="50%"><img src="docs/images/audio-editor.png" alt="Audio editor"></td>
<td width="50%"><img src="docs/images/automation.png" alt="Automation clip editor"></td>
</tr>
<tr>
<td><b>Audio editor.</b> Waveform and spectral views, markers and regions, time stretch and pitch shift, noise reduction, beat slicing and loudness readouts.</td>
<td><b>Automation.</b> Points with thirteen curve shapes, tension handles, an LFO tool, a shape library and recording straight from knob moves.</td>
</tr>
</table>

### Instruments and effects

<p align="center">
  <img src="docs/images/editors.png" alt="A selection of built-in plugin editors" width="100%">
</p>

<p align="center">
  <img src="docs/images/plugin-picker.png" alt="Plugin picker" width="80%"><br>
  <sub>The plugin picker shows every instrument and effect with a preview of its editor.</sub>
</p>

## Download

Every [release](https://github.com/lolitemaultes/Gridline/releases) has builds for:

| System | Files |
|---|---|
| macOS 11 or later (Apple Silicon and Intel) | `Gridline-<version>-macos-universal.dmg`: the app, the command line tool and the VST3 / CLAP / AU plugins |
| Windows 10 / 11, x64 and ARM64 | `...-windows-x64-setup.exe` (installer, optional plugins and file types), `...-windows-x64.zip`; the same for `arm64` |
| Linux x86_64 and aarch64 | `.AppImage` (runs on most distributions), `.deb` (Debian, Ubuntu), `.rpm` (Fedora, openSUSE), `.tar.gz` |
| FreeBSD, OpenBSD, NetBSD | `.tar.gz` built on each system |

`Gridline-<version>-SHA256SUMS` lists the checksums. The builds are not signed by Apple or Microsoft: on macOS open the app the first time with right-click > Open, on Windows choose "More info" > "Run anyway" in the SmartScreen prompt.

On Wayland Gridline runs natively through SDL; the interfaces of hosted plugins are X11 windows and open through XWayland.

## Build

Gridline needs CMake, a C++17 compiler and SDL2. Everything else (Dear ImGui, Lua, RtMidi, Signalsmith Stretch, the plugin interfaces and the audio decoders) is bundled in `third_party/`. The CI builds and tests every change on the systems below (`.github/workflows/ci.yml`); `tools/ci/install-deps.sh` installs the dependencies on each.

```sh
# macOS
brew install cmake sdl2

# Debian / Ubuntu
sudo apt install cmake libsdl2-dev libasound2-dev libjack-jackd2-dev libx11-dev

# Fedora: sudo dnf install cmake gcc-c++ SDL2-devel alsa-lib-devel libX11-devel mesa-libGL-devel
# Arch:   sudo pacman -S cmake sdl2 alsa-lib libx11 mesa
# FreeBSD: pkg install cmake ninja sdl2 libX11 sndio    OpenBSD: pkg_add cmake ninja sdl2

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 126 test suites
./build/gridline-app            # start the app
```

On Windows (Visual Studio 2022 or its Build Tools, from a developer command prompt), SDL2 is built from source:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRIDLINE_BUNDLED_SDL2=ON
cmake --build build
ctest --test-dir build
build\gridline-app.exe
```

Audio goes through SDL (Core Audio, WASAPI, ALSA / PulseAudio / PipeWire / JACK, sndio / OSS on the BSDs); MIDI devices through RtMidi (CoreMIDI, Windows Multimedia, ALSA, JACK) and sndio on the BSDs.

`./build.sh` is a quicker path for the core alone: it builds the command line tool, runs the core tests and renders the demo song.

## Command line

```sh
build/gridline info    demo/song.json                        # inspect a project
build/gridline render  demo/song.json -o out.wav --bits 24    # render to audio
build/gridline export-midi demo/song.json -o song.mid        # write a MIDI file
build/gridline import-midi song.mid -o song.json             # read a MIDI file
build/gridline zip     demo/song.json -o song.zip            # project plus samples
build/gridline export-flp-zip demo/song.json -o song.zip    # FL Studio zipped loop package
build/gridline plugin-check --all --json report.json         # plugin compatibility sweep
build/gridline bench   demo/song.json                        # load, save and render timings
```

## Plugin hosting

`src/host/` hosts third-party plugins through one interface, `host::Backend`.

| Format | Type id | Where Gridline looks |
|---|---|---|
| VST3 | `vst3:<class uid>` | `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3`; Windows `Common Files\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3`; Linux / BSD `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3` |
| CLAP | `clap:<plugin id>` | `.../Plug-Ins/CLAP`; Windows `Common Files\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`; Linux / BSD `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`; `$CLAP_PATH` |
| VST2 | `vst2:<unique id>` | `.../Plug-Ins/VST`; Windows `Program Files\VSTPlugins`, `Program Files\Steinberg\VSTPlugins`, `Common Files\VST2`; Linux / BSD `~/.vst`, `/usr/lib/vst`; `$VST_PATH` |
| AU (macOS) | `au:<type>/<subtype>/<manufacturer>` | the system component registry |
| LV2 | `lv2:<plugin URI>` | `~/Library/Audio/Plug-Ins/LV2`, `/Library/Audio/Plug-Ins/LV2`, `~/.lv2`, `/usr/local/lib/lv2`; on Linux `~/.lv2`, `/usr/local/lib/lv2`, `/usr/lib/lv2`; `$LV2_PATH` |

- **Opening a plugin** shows its own interface at the size it asks for, with a slim Gridline bar underneath for presets, bypass, sandbox, CPU and latency. The window follows the plugin when it resizes.
- **Scanning** happens in a child process, so a plugin that crashes or hangs is blocked until it changes instead of taking Gridline down.
- **The sandbox** runs a plugin in its own process. Audio and events travel through shared memory with a lock-free handshake. If the plugin crashes, Gridline keeps playing, the plugin is silenced and you can reload it.
- **What plugins get from the host:** transport and tempo, sample-accurate notes and parameter changes, sidechain input, MIDI output into other channels, offline render mode, latency reporting into delay compensation, CLAP parameter modulation and preset discovery.
- **LV2:** Gridline reads each bundle's Turtle files itself (no lilv). Control ports become parameters with their ranges, enumerations, toggles and logarithmic scales; MIDI travels in atom sequences both ways; plugins get `time:Position`, the worker, state (saved in the project), options with the block sizes, `lv2:enabled` bypass and `lv2:freeWheeling` during exports; the latency port feeds delay compensation; `lv2:Preset`s, also from other bundles, appear as the plugin's presets. Plugin interfaces open in the plugin window: CocoaUI on macOS, WindowsUI on Windows, X11UI on Linux and the BSDs, and external (kxstudio) interfaces in their own window. The Windows and X11 interface paths are built and unit-tested on CI but have not been used by hand yet; everything else is tested with in-tree test plugins on every system.
- **Compatibility sweep:** `gridline plugin-check` loads every scanned plugin at several sample rates and buffer sizes, feeds it silence, noise and notes, checks for invalid output, round-trips its state, sweeps its parameters and opens and closes its editor.

Interfaces: the VST 3 SDK interface headers (`third_party/vst3/pluginterfaces`, MIT), the CLAP headers (`third_party/clap`, MIT) and the LV2 specification headers (`third_party/lv2`, ISC). VST2 support uses Gridline's own description of the binary interface (`src/host/vst2/Vst2Abi.h`); no VST2 SDK code is included. No plugin binaries are shipped.

## Gridline as a plugin

The same build also produces Gridline as a plugin, so the whole workstation (channel rack, piano roll, playlist, mixer, every built-in instrument and effect) runs inside another DAW. Each bundle holds two plugins:

| Plugin | Kind | Audio | Notes |
|---|---|---|---|
| **Gridline** | instrument | stereo out, plus 15 stereo outputs carrying mixer inserts 1 to 15 | MIDI in |
| **Gridline FX** | effect | stereo in (enters mixer insert 1), sidechain in (inputs 3 and 4), stereo out | MIDI in (CLAP, VST3) |

```sh
cmake --build build --target gridline_clap gridline_vst3   # build/plugins/Gridline.clap, build/plugins/Gridline.vst3
cmake --build build --target gridline_au                   # macOS: build/plugins/Gridline.component (Audio Unit v2)
cmake --build build --target install-plugins               # copy the bundles into your user plugin folders
cmake --install build --component plugins --prefix /usr    # or install for a package (lib/clap, lib/vst3)
```

`install-plugins` copies to `~/Library/Audio/Plug-Ins/CLAP`, `.../VST3` and `.../Components` on macOS, and to `~/.clap` and `~/.vst3` on Linux. The build and the tests never install anything. Each bundle carries the app icon and Gridline's assets (fonts, sounds, templates, translations), so it works on its own.

- **Formats:** CLAP and VST3, both written against the interface headers only, and on macOS an Audio Unit (v2) written against Apple's AudioToolbox headers only. The Audio Units are `aumu GrIn GrLn` (Gridline) and `aufx GrFx GrLn` (Gridline FX); both pass `auval -strict`.
- **Transport:** with **Follow the host transport** on (the default), Gridline starts and stops with the host, takes its tempo and moves to its position: pattern mode plays the pattern at the host position within the pattern, song mode plays the arrangement at the host's song position. Turn it off and Gridline's own transport plays the pattern or the song. The Audio Unit reads the transport through the host callbacks.
- **MIDI input** plays the selected channel, or with **By MIDI channel**, MIDI channel 1 to 16 plays rack channel 1 to 16. Every format carries notes, controllers, channel pressure and pitch bend at their sample positions: CLAP and AU as MIDI messages, VST3 through MIDI controller mapping (each controller of each MIDI channel is a hidden parameter that Gridline turns back into MIDI), so the VST3 build plays exactly like the CLAP one.
- **Host parameters** are 16 macros. Right-click any control and choose **Link to host macro**, or press **Link** in the **Host link** panel to link the control you touched last. The host automates the macro and Gridline applies it to the linked control every 64 samples; the host shows the linked control's value, such as `62%` or `1.20 kHz`.
- **State** is the whole project: the project JSON with the paths of the samples it uses, the macro links and the plugin settings (for the Audio Unit, inside its ClassInfo). Samples stay where they are on disk.
- **Latency:** the project's delay compensation is reported to the host and updated when it changes.
- **Offline render** and **sample rate or buffer size changes** are supported; renders are identical from run to run.
- **Editor:** Gridline's interface in the host's plugin window, resizable, sharp on HiDPI screens (an NSView on macOS, also the Audio Unit's Cocoa view, an X11 child window on Linux).
- **Installed plugins inside Gridline:** the plugin loads the plugins the Gridline application scanned (VST3, CLAP, VST2, AU, LV2), and each opens its own interface, never a stand-in: a window owned by the host's plugin window (on macOS a child window that stays above it, on Linux a transient window), with the plugin's interface on top and Gridline's control bar under it (presets, bypass, sandbox, generic, CPU and latency, settings), as in the application. The windows hide with Gridline's editor and close with it; closing one gives the keyboard back to the host's window.

The **Host link** panel (a tab beside the browser) shows the transport, MIDI and output settings, the reported latency and the 16 macros. `gridline plugin-check Gridline` runs the compatibility sweep on the built bundles once they are in the plugin cache.

## FL Studio project compatibility

Gridline opens FL Studio projects (`.flp`), FL Studio zipped loop packages (`.zip`) and presets (`.fst`), so existing work can be brought across. **File, Open** (or drag and drop) opens `.flp` and `.zip` files like any project; saving one asks where to keep it and in which format, so the original is never overwritten unasked:

- channels, patterns, notes, playlist clips, automation, markers and mixer routing are imported;
- FL Studio's own instruments and effects are mapped to their Gridline equivalents with their settings;
- third-party plugins load as the installed plugin with their saved state, or stay as placeholders until the plugin is installed;
- **File, Import, FL Studio presets** converts the presets of a local FL Studio installation into Gridline's preset library on your computer.

Projects can also be saved back to `.flp` with **File, Export, FL Studio project** (or **File, Save as**), or as an FL Studio zipped loop package (the `.flp` plus every sample it uses, with relative sample paths) with **File, Export, FL Studio zipped loop package**: unchanged parts are written back exactly as they were imported, and FL Studio's own plugins are saved with their settings. Nothing from FL Studio is bundled with Gridline.

## Architecture

| Area | Where | What it does |
|---|---|---|
| Model | `src/core/Song`, `Types`, `Document`, `Undo` | Channels, patterns, clips and mixer with stable ids. Every edit is an undoable command. |
| Project files | `src/core/Project`, `Json` | Versioned JSON with migration from older versions, validation and atomic saves. |
| Engine | `src/engine/AudioEngine`, `Program`, `ProgramGraph` | Real-time engine with delay compensation and a parallel mixer graph. |
| Plugins | `src/packs/`, `src/engine/` | The built-in instruments and effects, one pack per family. |
| Hosting | `src/host/` | VST3, CLAP, VST2 and AU backends, scanning, sandbox and native editor windows. |
| Gridline as a plugin | `src/plugin/` | CLAP, VST3 and Audio Unit entry points, host transport, macros, state, the embedded editor and the windows of plugins hosted inside it. |
| Interface | `src/app/` | SDL2, OpenGL 3 and Dear ImGui. Every control and icon is drawn in code. |
| Import and export | `src/core/flp/`, `src/core/MidiFile`, `src/engine/Export` | Project import, MIDI, audio export and stems. |

Project files are JSON (`"format": "gridline-project"`). Older files are upgraded on load, and `gridline upgrade` rewrites them in the current schema.

## License

Gridline is free software, released under the GNU General Public License v3.0 (see `LICENSE`).
Bundled third-party libraries keep their own licences, all GPL-compatible: Dear ImGui (MIT), Lua 5.4 (MIT), RtMidi (MIT-style), Signalsmith Stretch and Signalsmith Linear (MIT, time stretching and pitch shifting), CLAP headers (MIT), VST3 plug-in interfaces (MIT), dr_libs and stb_vorbis (public domain or MIT-0), nanosvg (zlib). See each folder under `third_party/`.
The interface icons in `assets/icons/ui/` use geometry from Lucide (ISC) and Tabler Icons (MIT); see `assets/icons/ui/NOTICE.md`.

Gridline is an independent project and is not affiliated with or endorsed by Image-Line. "FL Studio" is a trademark of Image-Line Software and is mentioned only to describe file compatibility.

<sub>The README artwork is generated by `tools/make_readme_art.py` from Gridline's brand assets and screenshots of the app taken by `tools/readme_shots.sh`.</sub>
