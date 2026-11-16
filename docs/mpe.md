# MPE and per-note expression

An MPE controller (a keyboard or pad surface that tracks each finger) gives every note three
controls of its own: **pitch** (sliding the finger sideways), **pressure** (pressing harder) and
**timbre** (sliding up and down, sent as CC 74). Gridline's own instruments follow all three,
note by note.

## Playing an MPE controller

1. Open **Settings > MIDI** and tick **MPE controller** under the input settings.
2. Set **Note bend range** to the range your controller sends (48 semitones is the MPE
   standard and the default).

With MPE on, MIDI channel 1 is the master channel: its pitch bend, sustain and other
controllers act on the whole instrument, as before. Channels 2 to 16 each carry one note, and
their pitch bend, channel pressure and CC 74 move that note alone. A note starts with the
values its channel had just before it, as MPE controllers send them.

With MPE off, nothing changes: pitch bend, aftertouch and CC 74 reach the instrument as
ordinary MIDI.

Plugins get the controller's messages as they arrive and do MPE themselves; set them to MPE in
their own settings. For sending slides and bend lines to MPE plugins, see **Per-note pitch** in
[slide-notes.md](slide-notes.md).

## What each control does

Open the channel's settings (the gear on its rack row) and find **EXPRESSION**:

- **Note bend** turns per-note pitch on or off. On, each note bends by itself through the
  instrument's glide, so every instrument that follows slides follows MPE pitch too.
- **Pressure** and **Timbre** each choose what the control does:
  - **Instrument's choice** (the default): the instrument's own response. Most instruments
    raise the level with pressure (up to 6 dB) and brighten with timbre (a high shelf, up to
    9 dB either way from the centre). The Multisampler playing an SFZ instrument passes
    pressure to the instrument's channel aftertouch opcodes (`*_oncc129`, `*_chanaft`) and
    timbre to its CC 74 opcodes, as the instrument's author mapped them.
  - **Level** or **Brightness**: that response, whatever the instrument would do.
  - **Off**: the control does nothing.
- **Amount** scales pressure and timbre (100% by default).

The level and brightness responses act on each note by itself: pressing harder on one key of a
chord lifts that note alone, and the others keep their sound. A released note keeps its last
values while it fades, and a new note starts from its own values. Pitch is always per note.

A few instruments have no single voice per note to shape, and answer pressure and timbre as a
whole instead, following the held notes together (the strongest pressure and the timbre furthest
from its centre lead):

- the drum machines and kits, where a note picks a drum rather than playing a pitch;
- the decks and scratch players;
- Step Bass, whose one voice is shared by every key;
- Plasma, which has its own modulation for expression;
- Hex Synth while its sequencer plays, Lattice's reference engine, Kerf's Regions engine and the
  Preset Stand-in, which play notes inside an engine of their own.

## Expression saved in notes

A note can carry a **pressure lane** and a **timbre lane** next to its pitch bend line. They are
saved with the project, and moving, copying, resizing, splitting, chopping and gluing a note
keeps them in step with the note, as its bend line is. When the song plays, the instrument
follows them as it follows a controller. Notes without lanes play exactly as before, and
projects from earlier versions open unchanged.

Scripts can read and write them as `pressure` and `timbre` point lists on a note (see
[scripting.md](scripting.md)).

## Recording

With **MPE controller** ticked, recording keeps what each finger did with its note: the
member channel's pitch bend becomes the note's pitch bend line, its channel pressure the
note's pressure lane and its CC 74 the note's timbre lane, timed as you played them (a
quantized note takes its lanes with it). The lanes are thinned to the points that shape them,
at most 32 a lane, so a long performance stays light.

## Editing in the piano roll

Pick **Note pressure** or **Note timbre** in the lane under the notes (click its name, or turn
the wheel over it). Each note shows its line over its length, faint while it has none:

- **Drag** across the lane to paint the line into the notes you cross (only the selected notes
  while some are selected).
- **Right-drag** draws a straight line from where you pressed, across as many notes as it spans.
- **Alt+drag** takes the lane back to neutral where you drag.

Each drag is one undo step. Pitch is drawn with the **Pitch bend** tool, as before. Moving,
copying and resizing a note keep its lanes with it.

## Sending MPE out

- **Plugins.** Per-note pressure and timbre reach a hosted instrument as its format carries
  them: as CLAP note expressions (pressure and brightness), as VST3 poly pressure and the
  brightness note expression, or, for formats without note expressions, pressure as the
  note's poly aftertouch. Set the plugin's **Per-note pitch** to MPE and every note goes out
  on a channel of its own with its own pitch bend, channel pressure and CC 74, the MPE zone
  and bend range announced first.
- **MIDI Out.** Switch on **MPE out** under EXPRESSION in a MIDI Out channel's settings: channel 1 becomes the
  master channel, every note goes out on its own channel 2 to 16 with its own pitch bend (48
  semitones), channel pressure and CC 74, and slides and bend lines glide each note by itself.
  The zone and the bend range are announced when it is switched on.
- **MIDI files.** A channel whose notes carry pressure or timbre lanes is written as an MPE
  track: every note on its own member channel with its pitch bend, channel pressure and CC 74,
  the zone announced at the start. Other channels are written as before. Opening or dragging
  in an MPE file (one that announces an MPE zone, or whose notes sit one at a time on member
  channels with their own bends and pressure) keeps the track together and gives each note its
  bend line, pressure and timbre back.
