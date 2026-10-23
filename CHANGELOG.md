# Changelog

All notable changes to Gridline. Versions follow `project(VERSION)` in `CMakeLists.txt`; the
release workflow publishes the section of the tagged version as the release notes.

## 0.6.1

### Fixes from issue #1

- Linux and BSD: the automatic interface scale now follows the desktop's scale setting (Xft.dpi,
  GDK_SCALE, Qt and Wayland scaling) instead of the panel's physical pixel density, so a 1920x1080
  laptop at 100% opens at 100%. A new option, Settings > Interface "Follow the system's display
  scale", can be turned off to set the size yourself.
- Auto Pan: the pad's live dot used the wrong volume scale and could sit outside the pad. The dot and
  the setting ring now share one mapping, stay inside the pad and follow the pointer. The X-Y Pad
  output dot is clamped the same way.
- Floating windows dragged against the top no longer leave an empty band above their content.
- Linux: Space, F1 to F12 and Ctrl/Alt shortcuts reach Gridline while a hosted plugin's own window
  has focus. Windows uses the same rule.
- The play, stop and record buttons act on press.
- The metronome icon swings on every beat while the metronome is on and the song plays. A new
  "Interface animations" switch (Settings > Interface > Motion) turns decorative animations off and
  follows the system's reduced motion setting.

### Plugin editors

- One shared knob renderer for every built-in editor: anti-aliased rings, hover glow, shaded caps.
- Refined section headers, recessed value fields, gradient graph fills and hover readouts on the
  filter, maximizer, band delay and envelope editors.
- Shuttle: time and volume colour coding, cleaner slot cells and a readout while dragging a point.

### Every parameter checked

- New `gridline param-audit` command and test: every parameter of every built-in plugin is checked
  for an audible effect, engine use, editor wiring, automation and save/load.
- Bitcrusher: Downsample now defaults to 1, so the Bits control is audible all the way to 24 bits
  (the old /4 default masked every depth above about 6). The 8-bit preset keeps its /4.
- Fixed crashes or runaway output in Crest, Enhance, Lattice ring-mod loops, Octa Filter ALT
  resonance, and Isoline when an oscillator was switched on during playback.
- Wired parameters the engines ignored: pitch bend range (Hybrid, DCO and Hex synths), Hex Synth
  modulation sources, Pocket Synth mod wheel, MIDI Out bend range and pass-through, Spectra Morph
  chorus rate, Grain Scanner volume and transient hold, Kerf minimum slice gap, Pitch Shifter Hi-fi
  tempo sync, Alloy and Halftone chorus, Pixel Tone rows, Pedal Rack boost and more.
- Saw Swarm and Twin VA glide fixes; Note Sequencer ties hold their note.
- Missing editor controls added (Facet fine tune, Folio program controls); percentage readouts that
  showed values like "10000%" fixed.
- Abacus and the sampler no longer misbehave when a sample's start is set to its very end; the
  same kind of fix for the slicer's slice ends, plugin event timing and a few editor labels.

### Plugins renamed

Gridline's built-in plugins now follow one naming system. Flagships carry their own one-word
names, a family accent and a wordmark in their editor's title bar; everyday studio tools keep
plain descriptive names (Compressor, Parametric EQ, Arpeggiator). Type ids (`gl.*`) are unchanged,
so projects, presets, scripts and FL Studio imports keep working; former names stay searchable in
the plugin picker, the browser and the MCP catalogue (`list_plugin_catalog`), except where a former
name was also the name of an FL Studio plugin. Existing projects keep the channel names they
were saved with.

| Before | Now | Type | Tagline |
|---|---|---|---|
| Trio Synth | **Tercet** | `gl.synth` | Three oscillators, one clean line. |
| Drum Pads | **Abacus** | `gl.pads` | Sixteen pads, counted out on a grid. |
| Slicer | **Kerf** | `gl.slicer` | Cuts loops into slices along the beat. |
| Wavetable | **Isoline** | `gl.wavetable` | Wavetables scanned like contour lines. |
| Matrix FM | **Lattice** | `gl.matrixfm` | Six FM operators wired through one modulation grid. |
| Wave Program | **Folio** | `gl.waveprogram` | Zones, layers and loops bound into one instrument. |
| Additive | **Facet** | `gl.additive` | Additive synthesis, one harmonic facet at a time. |
| Resynth | **Halftone** | `gl.harmor` | Resynthesis that prints sound as fields of partials. |
| Patch Host | **Junction** | `gl.patcher` | Patch plugins together into one instrument. |
| Patch Host FX | **Junction FX** | `gl.patcherfx` | Patch plugins together into one effect. |
| Delay | **Tracer** | `gl.delay` | Echoes that trace the beat, from tape to digital. |
| Hall | **Cloister** | `gl.hall` | Long algorithmic halls with a quiet floor. |
| Time Gate | **Shuttle** | `gl.timegate` | Redraws time and volume on the grid. |
| Mastering Dynamics | **Crest** | `gl.master` | Multiband loudness for the master. |
| Vocoder | **Cipher** | `gl.vocoder` | Speaks any sound through a voice. |
| Pitch Corrector | **Plumb** | `gl.pitcher` | Pulls every note true to the scale. |
| Scope | **Graticule** | `gl.scope` | Scope, spectrum and phase on one measured screen. |
| Audio Editor | **Loupe** | `gl.edison` | The close-up audio editor: waveform, spectrum, markers. |
| Analog | **Analog Synth** | `gl.analog` | tool |
| Groove Synth | **Hybrid Synth** | `gl.gms` | tool |
| Plucker | **Pluck Model** | `gl.plucker` | tool |
| Envelope Controller | **Envelope Mod** | `gl.envctl` | tool |
| Keyboard Controller | **Keyboard Mod** | `gl.keyctl` | tool |
| Formula Controller | **Formula Mod** | `gl.formulactl` | tool |
| Control Surface | **Control Deck** | `gl.surface` | tool |
| Soft Clipper | **Clipper** | `gl.clipper` | tool |
| Stereo Shaper | **Stereo Width** | `gl.stereo` | tool |
| Low Lifter | **Sub Focus** | `gl.lowlift` | tool |
| Scratcher | **Turntable** | `gl.scratcher` | tool |
| Spreader | **Stereo Spread** | `gl.spread` | tool |
| Hit Looper | **Hit Repeater** | `gl.hitloop` | tool |
| Amp Rack | **Pedal Rack** | `gl.amprack` | tool |
| Squeeze | **Pinch** | `gl.squeeze` | tool |
| Pocket Rack | **Pocket FX** | `gl.pocketrack` | tool |
| Key Mapper | **Key Remap** | `gl.keymap` | tool |
| Key Splitter | **Key Zones** | `gl.split` | tool |
| Level Scaler | **Velocity Scale** | `gl.velscale` | tool |
| Free Filter | **Biquad Filter** | `gl.freefilter` | tool |
| Spline Scratcher | **Spline Scratch** | `gl.splinescratch` | tool |

- Editor title strips and the audio editor's window take their titles from the catalogue.
- The FL Studio import report lists every plugin as "FL plugin → Gridline plugin"; the export
  report names the Gridline side by its display name and type.

### New plugin

- New effect: Tideline (`gl.tideline`, Dynamics). It is a rhythmic volume ducker with one big Amount
  dial, 16 pump, sub, trim, reverse and rhythm curves plus a drawable curve, and rates from 1/16
  to 2 bars (triplet and dotted included). It can follow the tempo or be triggered by audio (track
  sidechain) or MIDI, in loop or one-shot mode with an end marker. Duration and shift are
  adjustable, it can duck only the lows or highs, or only the mid or side, and it adds no latency.
  It exports to FL Studio as a Gross Beat volume envelope. See `docs/plugins/tideline.md`.

## 0.6.0

### Pitch and time stretching

- New stretch engine (Signalsmith Stretch, MIT) with a band-limited sinc resampler for pitch:
  pitch shifts land within a hundredth of a cent, drum hits stay sharp and the level no longer
  flutters. Used by playlist audio clips, the sampler, the slicers, the audio editor and the
  pitch shifter.
- Stretch modes: Resample, Auto, Mono / vocal, Transient and Formant keep. They are read from
  and written to FL Studio projects.
- Resample mode and sample playback are band-limited, so pitching up no longer aliases.
- The pitch shifter has a new Hi-fi engine.


### Platforms

- Gridline now builds, tests and ships on Windows (x64 and ARM64), Linux (x86_64 and aarch64),
  macOS (Intel and Apple Silicon, one universal app), FreeBSD, OpenBSD and NetBSD. Every
  platform is built and tested on each change.
- Windows: audio through SDL (WASAPI), MIDI devices through Windows Multimedia, plugin hosting
  (VST3, CLAP, VST2, LV2) with the plugins' own interfaces in Gridline's plugin windows, the
  plugin sandbox (separate process, shared memory, pipes), drag and drop from Explorer and
  browsers with the drop target shown while hovering, the MCP token in the Windows Credential
  Manager, settings in `%APPDATA%\gridline`.
- Gridline as a plugin on Windows: `Gridline.vst3` and `Gridline.clap` with their own editor.
- Linux: the X11 plugin windows and plugin editor are compiled and tested on every
  distribution in the matrix (Ubuntu, Debian, Fedora, Arch, openSUSE, Alpine with musl).
  Wayland sessions run Gridline natively through SDL; plugin interfaces, which are X11
  windows, open through XWayland.
- FreeBSD, OpenBSD, NetBSD: audio through SDL (sndio / OSS), MIDI devices through sndio,
  plugin interfaces on X11.
- Number parsing and formatting in project files no longer depends on the system locale.

### Releases

- Downloads for every platform: a macOS disk image (universal app, plugins, command line tool),
  Windows installers and zip files, Linux AppImages, `.deb`, `.rpm` and `.tar.gz`, BSD
  `.tar.gz` archives, all with SHA-256 checksums.
- Automatic update check: at most once a day Gridline looks for a newer release on GitHub and
  offers to download it, remind you later or skip that version. Turn it off in Settings, General;
  Help, Check for updates checks right away.
- `gridline --version` prints the version.

## 0.5.1

- FL Studio project export: files written by Gridline open in FL Studio 2026, with the effects
  of each mixer insert in their slots and complete states for the formula controller, MIDI out
  and the audio editor.
- Song loop marker: one marker loops the song between itself and the next one; marker actions
  run inside the audio engine, sample accurate.
- The channel rack keeps its volume and pan knobs with long patterns, and the step sequencer
  scrolls sideways.
- Thin mixer: the mouse wheel scrolls the strips instead of changing a fader.
- The playlist's clip source picker can delete audio and automation clip sources.
- Editing tools: right-click deletes only the topmost item, audio clips show a volume line,
  zero-crossing snapping and time stretching have toggles with keys.
- Notifications can be copied and dismissed without clicking through to the playlist.
- Edited audio is saved to a neutral "Edited audio" folder that the browser lists.
- FL Studio project export carries Gridline's own instruments and effects with their sound.
- Interface scale, theme, language, layout reset and audio device changes ask to Keep or
  Revert, and revert by themselves after 15 seconds.
- Piano roll keys light up for sounding notes, drum machine pads flash when hit, and the tool
  strip icons are 16 px.

## 0.5.0

- Drag and drop from other applications: files, file promises, web links and audio data from
  file managers, browsers and sample stores, with the drop target shown under the pointer.
- New interface icons drawn from SVG geometry at the exact screen size.
- Channel envelopes, LFOs and filter: exact FL Studio timing for imported projects and smooth
  timing for new ones, in one system.
- Looping pattern clips, Shift+drag copies, an eye toggle for hidden items, a shared minimum
  inset for every editor with an automatic layout audit, effect reordering with the mouse wheel,
  an interactive reverb room display.
- Event lane editing follows the piano roll's rules (nearest stem, line tool).
- FL Studio project import keeps note velocities as MIDI velocities and finds samples that were
  moved next to the project.
- The version comes from one place (`project(VERSION)`), shown in the app, the command line
  and the plugins.
