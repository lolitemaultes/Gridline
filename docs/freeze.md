# Freezing, hibernating and resting

Two ways to take load off the computer while a project grows, both undone with one click, and a
third that works by itself: silent instruments rest (see [Resting](#resting)).

## Freeze

Freezing a channel or a mixer track renders its sound once into an audio file and unloads its
plugins. The file plays in their place, so the song sounds the same while the plugins use no
processing time and no memory.

- **A channel** is rendered with its instrument, note effects, envelopes and pitch, before its own
  volume and pan.
- **A mixer track** is rendered with everything that plays into it and its effects, before its
  fader. Channels and tracks that play only into it are unloaded too (they show at half strength).

What stays live: the volume, pan, mute and stereo controls, the track's EQ and every send, with
their automation. Automation of anything inside the freeze is in the render.

Freeze from the channel's menu (right click its name in the channel rack, then **Instrument and
routing**), from the rack menu for the selected channels (**Selected channels**), from a mixer
track's menu (**CPU**), or from a playlist track's menu when the track is linked to a channel or a
mixer track. A snowflake marks a frozen unit; a ring around it shows the
render's progress. **Unfreeze** loads the plugins again exactly as they were. Freeze, unfreeze and a
render asked for again are one undo step each.

The renders go to a **Frozen** folder next to the project file (with the settings while the project
is not saved yet) and are 32-bit float WAV files at the audio rate. Undo and redo and a freeze of the
same sound again reuse the file.

### Edits

- Opening the editor of a frozen plugin, or changing a frozen plugin from a menu (a preset, a
  replaced instrument, a pasted state), asks **Unfreeze to edit?**. Unfreezing loads the plugins and
  makes the change.
- Other edits the render depends on (notes, the tempo, automation inside it, a channel playing into
  a frozen track) make the freeze out of date. It then plays live and is rendered again a second
  after the last edit. With **Options > Refreeze out-of-date freezes** off, the old render keeps
  playing instead, marked with an amber dot, until you choose **Render the freeze again**.

### Where a freeze plays live

A render plays only in song mode. In pattern mode, in live mode, or when you play a frozen channel
on the keyboard, the unit loads its plugins and plays live, then unloads them again after a while
(see below). Exports render frozen units from their files when the file is at the export's rate.

Some units cannot be frozen, and the menu says why: the master, a track with an audio input, a
track with a send effect, a plugin that drives a link, a channel that plays other channels, has
extra outputs or sends MIDI out.

## Hibernate

Hibernating unloads a channel's or a mixer track's plugins without a render. A moon marks it. It
wakes by itself when it is needed:

- when its editor opens, or it is the selected channel;
- a few seconds before the song plays its notes (while stopped, when they are near the song
  position, so playback starts with it loaded);
- when you play it by hand, in pattern mode for the current pattern's channels, and in live mode.

A woken unit hibernates again 30 seconds after it was last needed (**Options > Woken plugins sleep
again after**; **Never** keeps it awake). A hibernating mixer track that has not woken is silent.

**Tools > Freeze and hibernate > Hibernate unused plugins** hibernates the channels without notes and the mixer
tracks nothing plays into; **Options > Hibernate unused plugins automatically** keeps doing so.
**Wake all** ends hibernation everywhere. Hibernation belongs to this computer: it is not an undo
step and is not shared with collaborators.

## Resting

Resting needs nothing from you: an instrument that has nothing to play stops using the processor
by itself, and starts again the moment it is needed. Nothing is unloaded and nothing changes in the
sound.

- **When it rests.** An instrument rests once it has been silent for a moment (half a second, or
  two seconds for a third-party plugin, whose tail only its sound can tell) with no note held and
  nothing sent to it. Its tail always ends first, so nothing is cut short and nothing clicks.
- **When it wakes.** A note, a MIDI message, the transport starting, stopping or jumping, or opening
  the instrument's own editor wakes it, on the same sample the note starts on.
- **Muted channels.** A muted channel rests half a second after its sound has faded, even while
  its notes go on. When you unmute it, it plays from its next note.
- **What never rests.** Effects on mixer tracks (they have their own rules for silence), and
  instruments that make sound without notes: an audio input, a turntable deck, a module player, an
  instrument running its own pattern, an instrument whose parameters automation or a controller
  link moves, or one whose MIDI output plays other channels. An instrument that is heard starting
  to sound with nothing sent to it is left running from then on.
- **Keep running when silent.** If a third-party instrument makes sound by itself (a drone, its own
  sequencer that follows the song) and goes quiet after a while, turn on **Keep running when
  silent** in the channel's menu (**Instrument and routing**). The choice is saved with the project.
- **Options > Rest silent instruments** turns resting off everywhere (on by default).

Exports and renders never leave anything out: they sound exactly as playback does.

## When the interface is idle

While you work, Gridline draws its windows at the display's refresh rate. When nothing has moved
for a second (no input, nothing playing, no sound, no edit), it draws about ten frames a second
until something happens again; a key, a click, a MIDI message or sound brings the full rate back
at once.

## Zipped projects

A zipped project includes the frozen audio. To make it smaller, turn on **File > Export > Leave out
frozen audio**: frozen units then go in unfrozen, and their plugins load when it opens.

## Shared projects

A freeze is part of the project: collaborators receive the render and play it too. Only the
computer that froze a unit renders it again by itself when it gets out of date; on the others it
plays live until then.
