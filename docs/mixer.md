# Mixer: selecting, moving, resetting and scrolling

The mixer window has two views: **Mix** (the strips, described here) and **Stage**, where your tracks stand on a stage seen from where you sit. Switch with **Mix | Stage** at the left of the toolbar. See [stage-view.md](stage-view.md).

## Selecting tracks

| Do this | To |
| --- | --- |
| Click a track's name | Select that track only |
| Ctrl+click (Cmd+click on macOS) a name | Add it to the selection or take it out |
| Shift+click a name | Select every track from the focused one to this one |
| Ctrl+A (Cmd+A) with the mixer focused | Select every insert track |
| Ctrl+D (Cmd+D) with the mixer focused | Go back to just the focused track |
| Alt+L with the mixer focused | Select the channels routed to the focused track (in the channel rack) |
| Ctrl+Alt+L (Cmd+Alt+L) with the mixer focused | Select the tracks routed to the focused track |

Ctrl+A selects the insert tracks and leaves the master out, so a reset or a routing change on
everything never touches the master by accident. To include the master, Ctrl+click its name
afterwards. If the master was the focused track, the focus moves to the first insert. The
shortcuts act only while the mixer has the focus, so the playlist, piano roll and channel rack
keep their own, and all of them can be changed under Options > Keyboard shortcuts. **Edit > Select all**
and **Deselect** do the same while the mixer is the last editor you used.

## Moving tracks

Drag a track by its name plate to put it somewhere else in the mixer. If the track is one of
several selected tracks, they all go together and keep their order. While you drag, an orange
line shows where the tracks will land; let go to move them, or press Esc (or right-click) to
cancel. Dropping a track among the strips docked to the left or right docks it there too. The
master always stays first; every other track can move, the Stage's Room track included.

**Move left** and **Move right** in a track's right-click menu, or Alt+Left and Alt+Right with the
mixer focused, move the selected tracks one place.

Routing follows the tracks, not their numbers: channels, sends, sidechains, the Room, audio tracks
in the playlist, effects that send to another track (such as Mixer Send, band splitters and
transient splitters) or listen to one as their sidechain, automation of a send level, and MIDI,
OSC and internal controller links all keep pointing at the same tracks. The project sounds exactly
as it did. Tracks you never named show their new number (Insert 5 dragged to the front becomes
Insert 1). Each move is one undo step, is saved with the project, reaches collaborators, and an FL
Studio export writes the new order with the same routing.

## Resetting a control on several tracks

When the track you act on is one of several selected tracks, a reset covers all of them, in one
undo step:

- double-click a fader or knob (with a selection, this resets even when the track you clicked is
  already at its default, as long as another selected track is not);
- right-click it and choose **Reset selected tracks to default**.

This works for volume, pan, stereo separation and the strip EQ's gain, frequency and bandwidth. A
reset on a track outside the selection changes only that track. Sends and an effect's mix knob
belong to one route or slot and are reset one at a time.

The right-click menu of a track, and the mixer's own menu, have **Reset selected tracks**:

- **Volume, pan and stereo separation** together, or each by itself, or **Strip EQ**;
- **Everything but name, effects and routing** also clears mute, solo, polarity, swap, disable,
  latency and threading.

## Scrolling with the wheel

In the normal, wide mixer:

- over the track names and the empty parts of a strip, the wheel moves the mixer sideways, as a
  horizontal wheel, a sideways trackpad swipe or Shift+wheel does, at the same speed as the
  playlist;
- over a fader or knob, the wheel changes that control. A scroll already under way carries on
  when a fader slides under the pointer, so scrolling never changes a level by accident;
- when several rows of tracks do not fit the pane's height, the vertical wheel scrolls up and down
  and Shift+wheel goes sideways.

In the thin mixer, where the strips stack into rows, the wheel scrolls the rows as before.
Settings > Interface decides whether the wheel scrolls or adjusts over controls there.

## Adding and arranging effects

Click an empty effect slot (or a slot's arrow) to pick an effect. The picker starts with a search
field: type part of a name, a category or a format (VST3, CLAP and so on) and it lists the
matching effects, built-in and installed, best first; Enter loads the first one and Escape closes the picker. Small typing
mistakes still find the effect. Clear the field to browse the folders again. The instrument
pickers (adding or replacing an instrument) have the same field.

To change the order of the effects on a track, drag a slot onto another slot (hold Ctrl to copy
it instead), or turn the mouse wheel over a slot to move it up or down.
