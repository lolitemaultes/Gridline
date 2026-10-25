# Changelog

All notable changes to Gridline. Versions follow `project(VERSION)` in `CMakeLists.txt`; the
release workflow publishes the section of the tagged version as the release notes.

## 0.7.0

### New: Seltmann, the automatic sample chopper (`gl.seltmann`)

- One simple flow: drop audio on Seltmann (from Finder or Explorer, Tracklib, the browser, the
  playlist or the audio editor, or on a Seltmann row in the channel rack; or "Open in Seltmann" in
  the browser), it analyses on its own, pick a layout card and it plays. A calm main view: one
  waveform with the bars and the slices by letter, the tempo with x2 / /2, the chop size (Auto,
  1 bar ... 1/16), the tempo mode, play, drag handles, Make loops and Send to project, the layout
  cards and the pads. Everything advanced (slice inspector, manual cuts, steps and the step
  language, grid tools, sound, loop list) is in a collapsible Edit drawer.
- Reads any material: breaks and drum loops (also with leading silence, a pickup, hits behind the
  beat or a ring-out tail), melodic loops, whole songs (sections and phrases), one-shots and
  ambient textures. Loops lock to their length (whole bars, exact tempo), the tempo and key in
  the file name are used when they are there, and the automatic chop size suits the material:
  breaks on their hits in eighths or sixteenths, songs in bars.
- No more tiny first slice: chops always start on bar 1, and no slice is shorter than half a step.
- 59 chop layouts in tabs (Flip, Phrase, Breaks, Micro, Creative, Mine), six of them new instant
  flips; every layout fits breaks, songs and one-shots alike and never refers to a missing slice.
- Plays as an instrument (slices on keys, the pattern on the octave below, locked to the song,
  following the project tempo by stretching or resampling); loops out as WAV files, drags into the
  playlist, rack or piano roll, or a pattern sent to the project.
- `gridline seltmann-analyse` prints what Seltmann hears in a set of files.

### Stop silences everything

- Stop now stops every sound, not only the song: notes from the piano roll and channel keyboards,
  typed keys, a MIDI keyboard (sustain pedal included), drum and Seltmann pads, the arpeggiator,
  the browser and audio editor previews, and installed plugins (note-offs plus All Notes Off) are
  released with their normal tails. Press Stop again to silence everything at once, delay and reverb
  tails included, with a short fade so nothing clicks; "Panic: stop all sound" is also in the Stop
  button's menu (Ctrl+H). Keys held when Gridline loses focus no longer get stuck.
- Plugins' own Play buttons follow Stop: Seltmann's pattern button (and Kerf's region play, the
  chord editor's preview) shows stopped as soon as Stop, Space, Stop twice, Tools > Stop all sound,
  a MIDI controller's Stop or a focus loss silences it, starts again from the first step with one
  click, and goes off by itself when the sound ends. Stop also ends the audio editor's playback in
  a mixer slot, Shuttle's free-running bar repeat and the effects inside Junction FX. Recordings,
  renders and loops dragged from the playlist's clip list now load into Seltmann (and onto channel
  rows) like browser audio files.

## 0.6.1

### Drawing, Convolver and stability

- Smooth drawing: strokes drawn into Cipher's bands, Morph EQ, Curve EQ, Band Delay, Facet's partials,
  Lattice harmonics and Keyboard Mod fill every band between mouse samples and are smoothed
  automatically (Alt draws raw). Each stroke is one undo step. Amount in Settings > Interface >
  Drawing, plus an on/off chip on each surface.
- Convolver: the IR EQ and the IR envelope are edited with points again, never painted. A click adds
  exactly one point (keep the button down to place it), dragging a point moves it with the pointer
  (Shift for fine moves) without adding any, double-click deletes a point, right-click opens a point
  menu (delete, straighten, reset), Alt-drag bends a segment. Every gesture is one undo step, and a
  drag released outside the graph ends cleanly. The editor is redesigned: a large EQ graph with a
  log-frequency grid, dB axis, the impulse's spectrum behind a filled curve and a hover readout; a
  waveform view with a time axis; a source area that takes dropped audio files; knobs grouped into
  Source, Shape, Space, Character and Output sections with value readouts.
- Convolver: new IR EQ and IR envelope curves are smooth. The curve runs through every point with no
  corners and never overshoots between points. One SMOOTH / STRAIGHT button above the graph switches
  the curve on view back to straight segments, which Alt-drag can bend. The switch is one undo step
  and keeps your points and bends. The audio follows the drawn curve exactly. Existing projects,
  presets and FL imports stay straight and sound the same. FL export writes a smooth curve as
  straight points that stay within 0.1 dB of it.
- Quitting while the background plugin scan was still running (first start, new or updated plugins)
  could crash on the way out ("mutex lock failed", a crash report on macOS). Quitting now stops the
  scan (the plugins not reached yet are scanned at the next start) and every other background job
  before Gridline closes; settings and projects are saved as before.

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
- The metronome icon ticks on every beat while the metronome is on and the song plays: it snaps
  between facing right and mirrored exactly on the beat, with no in-between frames, and switching the
  metronome on or off only shows its rest pose. A new "Interface animations" switch (Settings >
  Interface > Motion) turns decorative animations off and follows the system's reduced motion setting.
- Channel rack: dropping an audio file or an instrument from the browser (or a file from another
  application) onto a channel row replaces that channel's content, as FL Studio does. A sample
  channel loads the file; any other instrument becomes a sampler holding it; an instrument or
  instrument preset replaces the channel's instrument. The channel keeps its mixer track, volume,
  pan, steps and notes, and its name unless the name was automatic. The row shows "Replace <name>"
  while you drag. Dropping on the edge between two rows inserts a new channel there, and dropping
  under the rows adds one as before. Each drop is one undo step. A MIDI file dropped on a row fills
  that channel's notes. Mixer slots and strips now refuse instruments, and the playlist and rack
  refuse effects, with a reason. The playlist's clip source picker accepts drops. Loupe loads an
  audio file dropped on its waveform. A file from another application dropped on a plugin's sample
  slot no longer also opens as a new channel.

### Bitcrusher and Tideline

- Bitcrusher: redesigned around a Rate control in Hz. Rate sets the sample rate the signal is held
  at, from 50 Hz up to Off, on a logarithmic knob that reads "11.0 kHz" or "820 Hz". Any rate works,
  not only whole divisions of the audio sample rate, and sweeping or automating it is smooth, with no
  clicks or zipper noise. It replaces Downsample, which divided the audio sample rate by a whole number.
- Bitcrusher: Bits is the second control. It keeps its logarithmic knob, where 1 to 8 bits take 65%
  of the travel, and fractional depths ("5.6 bits"). New Anti-alias and Smoothing controls filter
  the signal before and after the hold. At 0% they let all the crunchy aliasing through; at 100%
  they give a clean, dull lo-fi sound. A new Dither control trades the bit-depth distortion for
  hiss. Mix, Gain, the filter, Squarize and Punch stay.
- Bitcrusher: a new editor puts Rate on a large knob with its readout. Bits sits beside it on a
  second knob, with the number of levels. A live display between them shows the held,
  quantised output over the input wave. Drag the display sideways to set the rate, or up and down
  to set the bits. New factory presets: Handheld 4-bit, Telephone, Sampler 12-bit and Crushed
  sweep.
- Projects, presets and effect chains saved by earlier versions keep their sound. Downsample N
  becomes the audio sample rate set in Settings divided by N (/4 at 44.1 kHz is 11.0 kHz). Automation, pattern
  automation and controller links on Downsample (project schema 8) and on Bits (schema 7) are
  moved to the new knobs when the project loads.
- Tideline: the Split slider's handle now stays under the pointer. It used to move 1.25 times
  as fast as the pointer and drifted after the pointer went past either end. Clicking no longer
  makes it jump, and Shift drags finer. All slider controls in the plugin editors now follow
  the pointer the same way.
- Tideline: the playhead, the output trace, the one-shot End marker and the custom curve points
  now line up with the curve when Shift or Duration are set. Points and the End marker no longer
  jump when you grab them off centre. Shift-dragging the curve wraps around the cycle.
- Tideline Listen plays only the part being ducked: the band in Lows / Highs, and the mid or
  side component when one is selected. Full band with Mid or Side used to ignore Listen. With
  Full band in Stereo everything is ducked, so the button is drawn faint and its tooltip explains
  why.

### Plugin editors

- One shared knob renderer for every built-in editor: anti-aliased rings, hover glow, shaded caps.
- Refined section headers, recessed value fields, gradient graph fills and hover readouts on the
  filter, maximizer, band delay and envelope editors.
- Shuttle: time and volume colour coding, cleaner slot cells and a readout while dragging a point.
- Shuttle: new layout. A header with the Hold pad, the two mix dials and the slots playing now; both
  slot banks with readable names; a large envelope editor with a labelled tool bar, axis labels and
  the smoothing dials beside it; slot settings underneath; an Options panel that also shows the quick
  effect, step gate, scratch and engine settings. Follows the light theme.

### Every parameter checked

- New `gridline param-audit` command and test: every parameter of every built-in plugin is checked
  for an audible effect, engine use, editor wiring, automation and save/load.
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

### Gridline's own sound data

- Hex Synth has its own 32 wave tables (classic shapes, tonal shapes, bit noise, formant and sync
  waves), generated by Gridline, so every wave works without FL Studio installed. Imported
  patches keep their wave numbers and get a Gridline wave of the same kind. Kick Sculptor's eight
  noise layers are synthesised too, so they also work without FL Studio.

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
