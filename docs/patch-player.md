# Patch Player

The Patch Player plays SF2 files (`.sf2`, and the compressed `.sf3`) the way the file
describes them: every zone of a patch with its key and velocity ranges, tuning, loops, volume
and modulation envelopes, both vibrato and modulation LFOs, its filter, the file's modulators and
the standard default ones, exclusive classes (an open hi-hat cut by a closed one) and stereo
sample pairs. It answers MIDI program change and bank select, so a whole General MIDI file plays
on it with every part on the right sound.

The project keeps a reference to the SF2 file itself, never a copy.

## Opening an SF2 file

- In the browser, an `.sf2` or `.sf3` file opens like a folder. Inside it the patches are listed
  by family (Piano, Organ, Guitar, Strings and so on, and Drum kits), each with its bank and
  program number. Click a patch to hear it; double-click it (or drag it onto the channel rack) for
  a new Patch Player channel playing it, or drop it on a Patch Player channel to change its patch.
  While a Patch Player window is open, a double-click plays the patch there.
  Dragging the file itself makes a channel playing its first patch.
- **Add > Patch Player** (under Samplers) makes an empty one; **Choose an SF2 file...** in its
  window opens a file.
- In the Smooth layout the Library lists the patches of the SF2 files you used lately, each under
  its kind of sound (Keys, Bass, Leads, Drum kit and so on), and the inspector shows the patch
  a track plays. In the Clips layout the device band shows the patch, level, sends and filter.

## The window

The header always shows the file (with its size and how many patches and banks it has) and the
patch. The arrows beside the patch step through the file's patches; a click on the patch name
lists them all with a search field. Two tabs sit beside them.

### Patches

- **Banks** on the left, with how many patches each has, then two lists of your own:
  **Starred** (click the star on any row) and **Recent** (the patches you chose last, from any
  file). These lists belong to you, not to the project, and are not shared in a collaboration
  session. **Other files...** opens another SF2 file.
- **Search** finds a patch by name, by number (`000:048` or just `48`) or by family.
- The family chips show only the families of the bank you are looking at; click one to list just
  that family.
- Each row shows the bank and program number, the name, the family and how many zones the patch
  has. Click a row to play that patch on the channel; Up and Down step through the list.
- On the right: what the patch is made of. Its zones, samples (and stereo pairs), the keys it
  covers, velocity layers, loops, the memory its samples take, and the reverb and chorus amounts
  the patch asks for.
- The keyboard along the bottom shows the patch's zones as coloured bars over the keys. Click or
  drag on it to play.

### Sound

- **Output**: Volume, Pan, and the **Reverb** and **Chorus** sends. The sends start at the amounts
  the patch asks for (see below).
- **Pitch**: Transpose, Fine, Bend range (the pitch wheel's range; MIDI files can set it too) and
  Glide.
- **Voices**: Poly or Mono, Polyphony (up to 128 voices) and how much Velocity changes the level.
- **Filter**: Cutoff and Resonance move the patch's own filter up or down; Key track opens it
  further on higher notes.
- **Volume envelope**: **From the patch** plays the patch's envelopes as they are, **Scale**
  makes its attack, decay, sustain and release longer or shorter (the patch's curve is drawn
  dashed behind yours), **Replace** sets them outright.
- **Vibrato**: from the patch, or your own Depth, Rate and Delay. **Mod wheel** sets how far the
  modulation wheel adds vibrato.
- **From the file** sums up what the patch itself asks for: its own modulators, modulation
  envelope and LFO targets, filter, exclusive classes and sample format. Gridline plays all of it.

Every knob can be automated and linked to a MIDI control. **Patch** and **Bank** are parameters
too: automate them, or let program change and bank select messages change them. Their first
setting, **Chosen**, plays the patch chosen in the window.

## Reverb and chorus

An SF2 patch can ask for reverb and chorus, but the file holds no effect. The Patch Player
gives each of them an output of its own (outputs 2 and 3 of the channel) carrying the patch at
the send level. On the Sound tab, **Sends to mixer** takes two unused mixer tracks (or makes new
ones), puts Gridline's reverb on one and its chorus on the other, and routes the sends to them in one
step. Leave the sends at 0 and nothing changes the dry sound.

MIDI controllers 91 (reverb) and 93 (chorus) add to the send levels, as a MIDI file expects.

## Program change and bank select

- A program change (from a MIDI keyboard or controller, or a MIDI Out channel) changes
  the patch for the next notes. Notes already playing keep their patch.
- Bank select (controller 0, and 32) chooses the bank for the next program change.
- When the file has no patch at that number, the Patch Player plays the nearest one: the same
  program in bank 0, then the nearest patch of the same family. A drum kit the file lacks plays
  kit 0.
- A program change with no bank select before it stays in the bank of the patch chosen in the
  window, so on a drum kit channel it picks another kit.

### In the Tracker

A note can carry a program change: in the Tracker, type **I** and two digits in the note's effect
field (00 to 7F, the program number in hex). As the note starts, the instrument switches to that
program, and the notes after it keep it until another program change. **Delete** on the effect
takes it off. The program change is saved with the project, plays in renders and exports, is
written to MIDI files as a program change before the note, and is shared in a collaboration
session like any note edit. Other instruments that answer program change (installed plugins, MIDI
Out) get it too.

The Tracker's effect letters: **I** program change, **G** slide, **P** portamento, **T** fine
tune, **X** and **Y** modulation and **R** release velocity. A note with more than one shows the
first of them in this order (a **+** marks the others), so a program change is always the one shown. The key help (Effect letters) lists
them with an example of each.

Modules (`.xm`) have no program change command, so none comes in from a module, and Gridline does
not write modules.

## MIDI files

When you import a MIDI file, the import window offers **A Patch Player and this file** as the
instrument. Each track then gets its own Patch Player on the chosen SF2 file, set to the
program the track starts with; channel 10 tracks get the drum kit. The choice is remembered for
the next import.

## Missing files

When a project's SF2 file is not where it was saved, the channel stays silent but remembers its
patch, and the window says the file is missing. **Find the file...** searches for it the same way
as for missing samples, and **Relink** works as for any sample. In a collaboration session the
file is sent to the other people like a sample.

## Compressed SF2 files (.sf3)

`.sf3` files hold their samples compressed. Gridline decompresses them when the file opens,
which takes a moment for a large file; after that they play exactly like an `.sf2` file. A sample
that cannot be decompressed plays as silence and the window says how many there are.

## Imported projects

Projects imported from other programs open their SF2 players as Patch Players on the same
file. The patch the player was saved on cannot be read from the project, so it starts on the
file's first patch and the import report says so: pick the patch again in the window. Projects
saved before keep the Multisampler they were saved with.

The Multisampler can still read `.sf2` files into its zones, as before.
