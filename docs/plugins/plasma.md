# Plasma (`gl.plasma`)

Plasma is a wavetable-centred hybrid synthesizer: three main oscillators that each play a
wavetable, a multisample, a sample, a granular cloud or a spectral resynthesis, plus a sub
oscillator and a noise layer, two filters, a 64-slot modulation matrix, three effect racks, an
arpeggiator and a clip player. It also opens presets made for Serum 2 (and Serum 1 `.fxp`
files) from your own folders and converts them into Plasma sounds, and it plays projects whose
Serum or Serum 2 is not installed (see "Stand-in for a missing Serum" below).

Plasma is in the plugin picker under Synths. Its code, factory wavetables, presets and artwork
are original. Opening a Serum preset reads your own preset and content files; nothing from
another product ships with Gridline.

## Layout

The editor follows the familiar arrangement of this kind of synth, so presets and tutorials made
for it translate directly:

- **Header**: OSC, MIX, FX, MATRIX and GLOBAL pages, the preset name with < > and its menu
  (Init, Factory, your library, Open preset, Browse), author and notes, MENU, undo / redo, the MAIN
  volume and the output meter. Undo / redo are Gridline's own (the same history as Ctrl+Z /
  Ctrl+Shift+Z, from the main window or Plasma's popped-out window): every gesture is one step, a
  knob drag, a wheel turn, one stroke on a shape or table, a rack change, a preset load.
- **Page** (top half): the selected page.
- **Lower panel** (OSC, MIX, FX and GLOBAL pages): eight macros, ENV 1-4, LFO 1-10 (six tabs in
  view, arrows for the rest), VELO and NOTE curves, and the voicing controls. The ARP button in
  the strip shows the arpeggiator here instead.
- **Strip** (bottom): pitch and mod wheels with the bend range, CLIP and ARP (power + editor),
  transpose, key, scale, swing, trigger octave, a playable keyboard and portamento (always /
  scaled, time and curve).

Every knob takes modulation: drag a source's handle (the small meter next to an envelope, LFO,
macro, VELO / NOTE tab) onto a knob, or right-click a knob and pick a source. The handle is a live
meter of the source's output for the newest note (or the global value for mono LFOs, macros and
other global sources): LFOs swing around the top centre, envelopes, velocity and note fill from
the lower left. Velocity and note keep the last note's value; a per-note LFO rests while no note
sounds. The ring around the knob shows the amount; Alt-drag on the ring changes it. The same context menu lists the knob's
slots (amount, bypass, remove) and offers automation and MIDI links.

## Oscillators (OSC page)

- **Wavetable**: 22 factory tables (each up to 256 frames, band-limited mip levels), your own
  `.wav` tables (single cycles or tables with a stated cycle length) and tables drawn in the
  editor. Position, two warp slots with 103 modes (sync, bend, PWM, asym, flip, mirror, remap
  curves 1-4, quantize, odd/even, filters, fourteen distortions, FM / PD / AM / RM from another
  oscillator, the sub, the noise or a filter, and spectral warps for the spectral oscillator),
  unison up to 16 voices with detune, blend, width, stack, table / warp spread and its detune
  curve, phase and random phase, octave / semitone / fine / coarse, pan and level.
- **Multisample**: plays an `.sfz` instrument (zones by key and velocity).
- **Sample**: start, end, loop modes and crossfade, slices, position and scan.
- **Granular**: density (free, tempo or grain count), length (free, tempo or percent), reverse
  probability, window shape and skew, randomness for offset, direction, pitch, length, pan, gain,
  window and warps, up to 64 grains, and how unison voices start their grains.
- **Spectral**: resynthesis of a sample (or the wavetable) with the spectral warp modes.
- **Sub**: six shapes, octave, coarse, phase, free-running or key-triggered, pan and level.
- **Noise**: 19 built-in noises or your own file, one-shot / keytrack, colour, start, random
  start, pitch, fine, pan and level.
- **Filters**: two filters with 106 types in groups (low-, high-, band-pass and notch slopes,
  ladder and analog models, combs, flangers, phasers, formant and vowel filters, multi-mode
  morphing and more), cutoff, resonance, drive, a type-dependent fourth knob (the X / Y boxes for
  morphing types), key tracking, pan, mix and level. Each oscillator, the sub and the noise picks
  the filters it feeds (the route button: F1, F2, F1+F2 with a balance, MAIN or DIRECT).

The wavetable editor (pencil on the display, or MENU) shows the harmonics and their phases on
top (drag to change), brushes on the left (freehand, line and ten cell shapes), the frame in the
middle with an X / Y snap grid and shift arrows, the table name, IMPORT / EXPORT, PROCESS
(single frame and whole table), MORPH (crossfade or spectral fill between the first and last
frame), the frame strip and a bar with the position, COPY / PASTE / REMOVE / SORT and a formula
field (`x` runs -1..1 across the cycle, `y` 0..1 across the table, `in` is the current sample).

## Mix, effects, matrix, global

- **MIX**: one column per source and filter: destination, filter balance, the two effect-bus
  sends, pan and a level fader; the filters' own outputs and the bus and main levels.
- **FX**: three racks (MAIN, BUS 1, BUS 2) with up to 24 modules: distortion (18 modes with an
  editable shaper), flanger, phaser, chorus, delay, compressor (single and multiband), reverb
  (several models), two-band EQ, filter (all filter types), hyper / dimension, frequency shifter,
  convolution (with your own impulse responses), utility, and splitters (low / high, low / mid /
  high, mid / side) that hold modules per band. Drag modules to reorder them; the rack menu
  copies, pastes and locks the effects so loading another preset keeps them.
- **MATRIX**: 64 slots: source, curve (editable, with rise / fall smoothing), amount, polarity,
  destination, live output, an auxiliary source with its own curve and invert, output scale and
  bypass. Expand the matrix over the lower panel to see more rows.
- **GLOBAL**: editor preferences (value bubbles, live display), MPE, voice control (per-voice
  sequences of pan, detune, cutoff, envelope and two modulation values, with random amounts),
  quality, tuning (`.tun` files) and preset information.

## Arpeggiator and clip player

- **ARP**: 12 pattern slots with a launch quantize and EDIT ALL; per slot: shape (up, down, as
  played, chords, random and more, or a drawn pattern of steps with velocity and accents), rate
  in tempo divisions or Hz with triplet and dotted, transpose (shift, range, direction and cycles
  per step), latch and thru, offset, repeats, gate and chance, retrigger (on launch, by rate, on a
  new note, on the first note) and velocity (decay towards a target). The keys of the trigger
  octave pick slots while you play.
- **CLIP**: 12 clips, each with a piano roll, a velocity lane and automation lanes for any
  parameter, length, keyboard span (off, mono, poly, offset), transpose, play modes (forward,
  reverse, pendulum, random variants, one shot, static), rate with BPM / triplet / dotted, launch
  quantize, retrigger, velocity trigger and note gate. RECORD takes the notes you play into the
  clip (OVERDUB keeps what is there, EXTEND lets the clip grow, the grid button quantizes), FOLD
  shows only the keys in use and the headphones button auditions notes as you place them.
- MIDI OUT sends the clip player's (or the clip and arp's) notes on to the channel's MIDI output.

## Presets

There is one way to open a preset, whatever made it: Plasma presets, Serum 2 presets
(`.SerumPreset`) and Serum 1 presets (`.fxp`) are recognised by their content and open the same
way from every place:

- **Open preset...** in the preset menu (the file dialog lists every supported type at once);
- the preset browser (double-click, Enter, or Up / Down to step through and audition);
- dragging a file (from the browser panel or the desktop) onto Plasma's window, or onto the
  channel rack for a new Plasma channel; double-clicking one in the browser panel;
- the plugin header's preset menu and anything else that loads a preset for Plasma.

The preset browser (the list icon next to the preset name) shows the factory presets, your
Gridline preset library for Plasma (SAVE AS stores the current sound there) and the **Serum
library**: the Serum 2 and Serum preset folders found on this computer (macOS, Windows, and Wine
prefixes on Linux), grouped by folder and bank, plus folders you add with ADD FOLDER. The library
is indexed in the background and kept in a cache, so it lists at once and refreshes when files
change (REFRESH rescans now). Typing in the search field while a Serum folder is selected searches
the whole library.

Opening a Serum preset maps oscillators, warps, unison, filters, envelopes, LFOs (including drawn
shapes), macros, the matrix, the effect racks, the arpeggiator and clips onto Plasma. Wavetables,
samples and impulse responses that a preset references are looked up in your Serum content
folders and the folders you added. When something is approximated or a file was not found, a
short notice says so; the full report stays with the sound: hover the small info mark next to the
preset name.

Serum 1 presets go through the same mapping: their 299 parameters, modulation matrix, effect
order, LFO shapes, wavetable and noise names and macro names are read from the preset. Serum 1's
Chaos sources, the Sync Window and Mirror warps and LFO trigger modes have no exact counterpart and
are listed in the report.

## Stand-in for a missing Serum

Installed plugins are always hosted as themselves: Plasma never replaces an installed Serum.
When a project (a Gridline project or an FL Studio project) uses Serum or Serum 2 and the plugin
is not installed, Plasma plays it instead:

- the plugin's saved state is read (Serum 2's VST3 or Audio Unit state, Serum 1's VST2 chunk) and
  mapped onto Plasma as when you open the preset;
- automation clips, pattern automation and controller links of the plugin's parameters move to the
  matching Plasma parameters, with their values converted. Parameters without a counterpart keep
  their automation aside;
- the original plugin and its saved state are kept in the project untouched. When the plugin is
  installed again, the project opens with it, its state and its automation as before. Exporting
  to FL Studio writes the original plugin;
- the plugin window shows a short note ("Serum 2 is not installed: Plasma plays its sound") with
  what is approximated on hover, and **Replace with Plasma** keeps Plasma for good (the missing
  plugin and its saved settings are then removed from the project).

## Performance

Plasma renders control changes on a fixed grid, so the sound does not depend on the buffer size.
Quality on the GLOBAL page sets the voice oversampling: Draft (none), High (2x, the default) or
Ultra (4x).
