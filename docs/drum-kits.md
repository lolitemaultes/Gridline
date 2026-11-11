# Drum kits

Gridline loads drum kits in the open `.h2drumkit` format onto Abacus, its drum pad sampler. Many
free kits are shared in this format, often under a Creative Commons licence.

## What you can load

| What | How it looks |
| --- | --- |
| A packed kit | one `.h2drumkit` file (a tar archive, usually compressed) |
| An unpacked kit | a folder with a `drumkit.xml` file and the kit's samples beside it |

Every version of the kit file loads: the current one, the 1.x one with a kit-wide list of sample
sets (components), the 0.9 one and the oldest one with a single sample per instrument.

## Loading a kit

- **Browser:** a `.h2drumkit` file and a kit folder's `drumkit.xml` show with a drum icon and
  the word KIT. Double-click one, or drag it onto the channel rack, the playlist or the window.
  Right click a kit folder and choose **Load drum kit** to load the folder itself.
- **Onto a channel:** drop a kit on a channel row or on an open Abacus window and that channel
  takes the kit. Its routing, volume and pan stay.
- **From the file manager:** drop the `.h2drumkit` file or the kit's folder onto Gridline.

The kit is read in the background, so even a large kit does not hold up the app, and the whole
load is one undo step. A packed kit is unpacked into Gridline's local cache, the same place
zipped projects go: the file itself is never changed and nothing is written beside it. Saving
the project keeps the samples it uses; **Export project data files into folder** or a zipped project copies
them next to the project.

## Previewing a kit

Click a kit in the browser. The info panel at the bottom shows how many instruments it has, its
licence and its author, and a pad for each instrument. Click a pad to hear that instrument (its
loudest sample). Dragging a pad drags the whole kit.

## The import report

After a kit loads, a short report names the kit, its author and its licence, and says what it
added and what could not be carried over. Keep the licence in mind when you share music made
with the kit: Creative Commons kits often ask you to credit the author.

## How a kit maps onto the pads

| In the kit | On Abacus |
| --- | --- |
| Instrument | a pad named after it; up to 32 pads per channel. A kit with more instruments goes on to a second and a third Abacus channel (96 instruments in all) |
| Note | instrument 1 plays on MIDI note 36, instrument 2 on note 37 and so on, as in the kit's own player, so drum MIDI files made for the kit play the right sounds |
| Velocity layers | velocity layers with the same velocity ranges (where ranges touch, the first layer takes the shared velocity) |
| Sample sets (components) | their layers stack and play together |
| Round robin, random | the pad's Cycle and Random layer modes |
| Volume, gain | pad level and layer levels (above full is set to full) |
| Pan, pitch, layer pitch | pad pan, pad tune and layer tune |
| Mute group | a cut group: the pads of a group cut each other off (up to eight groups) |
| Attack, decay, sustain, release | the pad's envelope |
| Stop note | one shot off: the pad stops when the note ends |
| Velocity on or off | the pad's velocity scaling |
| Mute, solo | the pad's mute and solo |

What does not come across, and is named in the report when a kit uses it: the instrument filter,
random pitch, more than 64 layers on one instrument and more than eight mute groups. A pad in a
cut group also cuts its own previous hit; in the kit's own player only the other instruments of
the group are cut. Missing samples are listed in the report and can be found again with
**File > Locate missing samples**.
