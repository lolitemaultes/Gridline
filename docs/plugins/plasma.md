# Plasma (`gl.plasma`)

Plasma is a wavetable-centred hybrid synthesizer: three main oscillators that each play a
wavetable, a multisample, a sample, a granular cloud or a spectral resynthesis, plus a sub
oscillator and a noise layer, two filters, a 64-slot modulation matrix, three effect racks, an
arpeggiator and a clip player. It can also read presets made for Serum 2 (and Serum 1 `.fxp`
files) from your own folders and set up a Plasma sound from their settings, and it plays projects whose
Serum or Serum 2 is not installed (see "Stand-in for a missing Serum" below).

Plasma is in the plugin picker under Synths. Its code, factory wavetables, presets and artwork
are original. Opening a Serum preset reads your own preset and content files; nothing from
another product ships with Gridline. Plasma is an independent synth with its own engine: an
opened preset is Plasma playing the settings it read, not a reproduction of the other synth's
sound. Gridline is not affiliated with or endorsed by Xfer Records; "Serum" is a trademark of
Xfer Records and is named only to describe the preset files Plasma can read.

## Layout

The editor has five tabs: **Synth**, **Effects**, **Links**, **Play** and **Setup**. Every setting
has its home in one of them. On Synth and Effects, a second switch shows the same sound as
**Panels** (blocks) or **Patch** (boxes and wires, Tab). **Simple | Full** sets how much each block
shows: Simple keeps the main controls and folds the rest behind the block's More button; Full shows
every control in place. The choice is remembered.

- **Top bar**: Library (right-click for Init, the factory sounds and Open file), Save as, the
  tabs (Links shows how many links are in use), Panels | Patch, Simple | Full, a note when the
  sound came from another format, undo / redo, the output level and meter, with the sound's name,
  its < > arrows and its sound list beside Plasma's name. The plugin window's own header above
  keeps only its menu, A / B and copy (Plasma's bar has the name and presets). Undo / redo are Gridline's own (the
  same history as Ctrl+Z / Ctrl+Shift+Z): every gesture is one step.
- **Synth**: the sound as blocks. The top row holds the sound in signal order: Layer 1-3, Root,
  Texture, Filter 1 and 2, then the Amp (Envelope 1). Under it, **Modulation**: the LFOs with
  their shapes at a readable size, Envelopes 2-4, the Macros (right-click a knob to rename it),
  Key and touch, and Chance and voice. Each block has a title bar with its on light (or, for a
  motion source, its link dot), its name and its menu (the dots at the right); a folded block
  shows its picture in the title bar. The menu opens the advanced panel (or double-click the
  block), turns it on or off (or Shift-click its name), resets it, copies and pastes its settings
  to another block of the same kind (Ctrl+C / Ctrl+V with the pointer over the block), loads and
  saves a layer's wavetable, shows every setting in place, folds it, shows it in the Patch view,
  says what it does (?) and removes it from its section (Delete). Drag a block's name
  to move it within its row: the order on screen is the editor's own and never changes the sound.
  Each layer's table display is edited in place: drag across it to move through the table, switch
  2D / 3D, read the morphs and their amounts on it, click the table map beside it, step through
  tables with the arrows, Edit opens the table editor.
- **Parts sidebar** (right on Synth and Effects; fold it, or drag its edge to resize it):
  **Modules** lists each kind of block with how many are in use (Layers 2 of 3, LFOs 1 of 10):
  drag one into the synth, or click it, to add the next free one; a full family is greyed out.
  Removing a block (x in its title bar) switches its slot off and returns its settings to the
  start, as one undo step. **Library** lists the factory tables, LFO shapes and sounds, with a
  search: drag a table onto a layer to load it (or onto empty space for a new layer), a shape
  onto an LFO, click a sound to load it. On the Effects tab the sidebar lists the effect types.
- **Library** (top bar): a full page with Tables, Shapes and Sounds. Tables and shapes show as
  cards with their pictures, grouped as Factory, your own folder and the installed library, with
  filters, a search and Load into a chosen layer or LFO; Import copies a WAV into your table folder
  and Show in folder opens it. Sounds lists the factory, saved and installed sounds with a
  preview of the open one, Play a note and Load sound.
- **Effects**: three lanes, Main, Send A and Send B. Each effect is a block with its live
  picture, its mix and main dials (every control with More or Full). Drag an effect by its name
  to reorder it or move it to another lane; drag one in from the sidebar.
- **Links**: every link, grouped by source, by target or in slot order.
- **Play**: Voices (notes at once, stealing, glide, bend range, transpose, fine tune), Key and
  touch, the Arpeggiator, Phrases and Key zones, as pages under the tab.
- **Setup**: sound quality, tuning, time scaling, the editor, sound info and compatibility.
- **Advanced panels**: a block's advanced panel grows from it over the Synth tab, with Back at the
  top left and the other members of its family at the top right: the LFO editor (the shape across
  the whole width, Speed, Each note, Feel and Moves cards), the envelope editor (quick shapes,
  Time / Beats, the graph, Times, Levels, Curves and Moves cards), Routing and the table editor.
  Advanced, under the cards, opens the less used settings.
- **Play bar** (bottom): Poly / Mono / Legato and the notes at once, glide, transpose, the
  Arpeggiator and Phrases switches, More... and Keyboard. The keyboard drawer (Alt+K) slides up
  over the bottom of any tab: bend and mod wheels (the bend wheel springs back to the centre),
  the bend range (click it to edit), the trigger octave and a playable keyboard.

### Patch

The Patch view draws the sound as boxes and wires, left to right: what plays the notes (Voices,
Arpeggiator, Phrases), the five sources, the two filters, the amp, the effects of the main chain
and both sends, and the output. Motion sources in use sit in a row below.

- **Sound wires** (thick, orange) are where each part's sound goes; a source feeding both filters
  shows two wires whose thickness is the balance. **Send wires** are dashed. **Motion wires** are
  dotted in the source's family colour, one per link, with the depth on a label in the middle.
- Drag from a box's right edge onto another box to route its sound. Only the places the sound can
  go light up; anything else is refused with the reason. Drop on empty space for a search menu
  (where it can go, or an effect to add). Alt+click a wire to take it away.
- Drag a motion box's dot onto any control or pin to link it. Drag a depth label sideways to set
  the depth (Shift for fine), click it to edit the link on the right, Alt+click to remove it.
  The toolbar counts the links in use (64 at most).
- Click a box to edit it in the inspector on the right (the same controls as the columns). The
  arrow on a box lists all its settings, each with a pin. Right-click a box to turn it on or off,
  open it in the panels or remove an effect; right-click the background to show a motion source
  or add an effect. Delete removes the selected effect or link, or turns a box off.
- Hovering a box lights its whole path. Show chips hide sound, motion or effects; Tidy puts the
  boxes back in order; Fit (F) fits the whole patch; the wheel zooms. Box places and zoom belong
  to the editor and are not saved in the sound.

Every control takes modulation, three ways: drag a source's grip onto a control; click a grip (or
choose Link mode in the dock) and then drag on any control to set the link's depth directly; or
right-click a control and choose Link from. A linked control shows a band in the source's family
colour along its edge; Alt-drag changes the newest link's depth. The same menu lists the
control's links (depth, bypass, remove) and offers automation and MIDI links. The Links view
lists all 64 links with their curves.

## Layers

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

## Routing, effects, links, setup

- **Routing**: one row per source and filter: destination, filter balance, the two send
  levels, pan and level; the filters' own outputs and the send and main levels.
- **Effects**: three chains listed down the left (Main chain, Send A, Send B) with up to 24 effects: distortion (18 modes with an
  editable shaper), flanger, phaser, chorus, delay, compressor (single and multiband), reverb
  (several models), two-band EQ, filter (all filter types), hyper / dimension, frequency shifter,
  convolution (with your own impulse responses), utility, and splitters (low / high, low / mid /
  high, mid / side) that hold effects per band. Drag a row to reorder it or move it to another
  chain; the chain's ... menu copies, pastes and empties it, and Keep on preset load keeps the
  effects when another sound loads. The selected effect fills the rest of the stage: its picture
  and main dials, then its other settings.
- **Links**: 64 links: source, curve (editable, with rise / fall smoothing), amount, polarity,
  destination, live output, an auxiliary source with its own curve and invert, output scale and
  bypass. Select a link to edit its curves in full size.
- **Setup**: editor preferences (value bubbles, live display), MPE, voice control (per-voice
  sequences of pan, detune, cutoff, envelope and two modulation values, with random amounts),
  quality, tuning (`.tun` files) and preset information.

## Arpeggiator and phrases

- **Arpeggiator**: 12 patterns with a switch-on grid and Edit every pattern; per slot: shape (up, down, as
  played, chords, random and more, or a drawn pattern of steps with velocity and accents), rate
  in tempo divisions or Hz with triplet and dotted, transpose (shift, range, direction and cycles
  per step), latch and thru, offset, repeats, gate and chance, retrigger (on launch, by rate, on a
  new note, on the first note) and velocity (decay towards a target). The keys of the trigger
  octave pick slots while you play.
- **Phrases**: 12 phrases, each with a piano roll, a velocity lane and automation lanes for any
  parameter, length, keyboard span (off, mono, poly, offset), transpose, play modes (forward,
  reverse, pendulum, random variants, one shot, static), rate with BPM / triplet / dotted, launch
  quantize, restart, velocity start and note gate. Record takes the notes you play into the
  phrase (Layer keeps what is there, Extend lets the phrase grow, the grid button quantizes), Fold
  shows only the keys in use and the headphones button auditions notes as you place them.
- Output to MIDI (Setup) sends the clip player's (or the clip and arp's) notes on to the channel's MIDI output.

## Presets

There is one way to open a preset, whatever made it: Plasma presets, Serum 2 presets
(`.SerumPreset`) and Serum 1 presets (`.fxp`) are recognised by their content and open the same
way from every place:

- **Open preset...** in the preset menu (the file dialog lists every supported type at once);
- the Library (double-click, Enter, or Up / Down to step through and audition);
- dragging a file (from the browser panel or the desktop) onto Plasma's window, or onto the
  channel rack for a new Plasma channel; double-clicking one in the browser panel;
- the plugin header's preset menu and anything else that loads a preset for Plasma.

The Library (the button next to the sound's name) shows the factory sounds, your Gridline
preset library for Plasma (Save as stores the current sound there) and the **imported folders**: the Serum 2 and Serum preset folders found on this computer (macOS, Windows, and Wine
prefixes on Linux), grouped by folder and bank, plus folders you add with Add a folder. Each folder found reads "Presets 1", "Presets 2" and so
on; right-click one to rename it or to show it in the file manager. The library
is indexed in the background and kept in a cache, so it lists at once and refreshes when files
change (Read again rescans now). Typing in the search field while a Serum folder is selected searches
the whole library.

Opening a Serum preset reads the preset's settings and maps oscillators, warps, unison, filters, envelopes, LFOs (including drawn
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
- the plugin window shows a short note ("Serum 2 is not installed: opens as Plasma") with
  what is approximated on hover, and **Replace with Plasma** keeps Plasma for good (the missing
  plugin and its saved settings are then removed from the project).

## FL Studio export

FL Studio has no Plasma of its own. Unless you choose **Rendered to audio** in **Settings, Project,
FL Studio export**, a Plasma channel goes into an FL Studio project as Plasma from the Gridline
Suite with all its settings, and FL Studio opens it live and editable where the Gridline Suite is
installed. With the default choice a muted copy of the channel plays it rendered to audio, for
computers without the Suite; with **Gridline Suite plugins only** there is no copy. **Rendered to
audio** renders the channel to samples. Automation of Plasma's parameters is kept in every case
where the plugin is written. (A Plasma that stands in for a missing Serum is exported as that
plugin, as described above.)

## Performance

Plasma renders control changes on a fixed grid, so the sound does not depend on the buffer size.
Quality in Setup sets the voice oversampling: Draft (none), High (2x, the default) or
Ultra (4x).

Parts that are not in use cost nothing: a layer, Root, Texture or Filter 2 that is off is
skipped, an LFO, envelope, macro, chance or key source with no link only keeps its clock running
(so it is in step the moment you link it), empty effect slots and unused sends are skipped, and
unison and oversampling cost only when they are used. A one-layer sound uses a fraction of the
processing time of a sound with everything on, and switching a part on is click-free.
