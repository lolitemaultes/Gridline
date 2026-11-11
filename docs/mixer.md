# Mixer: selecting tracks, resetting and scrolling

## Selecting tracks

| Do this | To |
| --- | --- |
| Click a track's name | Select that track only |
| Ctrl+click (Cmd+click on macOS) a name | Add it to the selection or take it out |
| Shift+click a name | Select every track from the focused one to this one |
| Ctrl+A (Cmd+A) with the mixer focused | Select every insert track |
| Ctrl+D (Cmd+D) with the mixer focused | Go back to just the focused track |

Ctrl+A selects the insert tracks and leaves the master out, so a reset or a routing change on
everything never touches the master by accident. To include the master, Ctrl+click its name
afterwards. If the master was the focused track, the focus moves to the first insert. The
shortcuts act only while the mixer has the focus, so the playlist, piano roll and channel rack
keep their own, and both can be changed under Options > Keyboard shortcuts. **Edit > Select all**
and **Deselect** do the same while the mixer is the last editor you used.

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
