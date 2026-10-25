# Plasma (`gl.plasma`)

Plasma is a wavetable-centred hybrid synthesizer: three main oscillators that each play a
wavetable, a multisample, a sample, a granular cloud or a spectral resynthesis, plus a sub
oscillator and a noise layer, two filters, a 64-slot modulation matrix, three effect racks, an
arpeggiator and a clip player. It also opens presets made for Serum 2 (and Serum 1 `.fxp`
files) from your own folders and converts them into Plasma sounds.

Plasma is in the plugin picker under Synths. Its code, factory wavetables, presets and artwork
are original. Import reads your own preset and content files when you open them; nothing from
another product ships with Gridline.

## Layout

The editor follows the familiar arrangement of this kind of synth, so presets and tutorials made
for it translate directly:

- **Header**: OSC, MIX, FX, MATRIX and GLOBAL pages, the preset name with < > and its menu
  (Init, Factory, your library, Import, Browse), author and notes, MENU, undo / redo, the MAIN
  volume and the output meter.
- **Page** (top half): the selected page.
- **Lower panel** (OSC, MIX, FX and GLOBAL pages): eight macros, ENV 1-4, LFO 1-10 (six tabs in
  view, arrows for the rest), VELO and NOTE curves, and the voicing controls. The ARP button in
  the strip shows the arpeggiator here instead.
- **Strip** (bottom): pitch and mod wheels with the bend range, CLIP and ARP (power + editor),
  transpose, key, scale, swing, trigger octave, a playable keyboard and portamento (always /
  scaled, time and curve).

Every knob takes modulation: drag a source's handle (the small circle next to an envelope, LFO,
macro, VELO / NOTE tab) onto a knob, or right-click a knob and pick a source. The ring around the
knob shows the amount; Alt-drag on the ring changes it. The same context menu lists the knob's
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

## Presets and import

The preset browser (the list icon next to the preset name) shows the factory presets, your
Gridline preset library for Plasma (SAVE AS stores the current sound there) and the Serum 2 /
Serum preset folders it finds on this computer. Double-click or Enter loads; Up / Down step
through the list and load as you go.

Importing a `.SerumPreset` or `.fxp` maps oscillators, warps, unison, filters, envelopes, LFOs
(including drawn shapes), macros, the matrix, the effect racks, the arpeggiator and clips onto
Plasma. Wavetables, samples and impulse responses that a preset references are looked up in your
Serum content folders. The import report lists what was approximated and any files that were not
found. A project or FL Studio project that hosts the real plugin keeps hosting it; Plasma never
replaces it on its own.

## Performance

Plasma renders control changes on a fixed grid, so the sound does not depend on the buffer size.
Quality on the GLOBAL page sets the voice oversampling: Draft (none), High (2x, the default) or
Ultra (4x).
