# Meter changes

A song can change its time signature (its meter) as it goes: a verse in 4/4, a bridge in 7/8, an
ending in 6/8. Every meter change starts a new bar.

A meter is notation. It decides where bar and beat lines fall, how bars are numbered, where things
snap and how the metronome clicks. It never moves a clip or a note, never changes a length, and
never changes the sound: a song renders exactly the same with or without its meter changes.

## The meter at the start

The song's first meter is set in **Project settings** (Meter at the start). The meter display in
the top bar shows the meter at the song position; click it to edit that section's meter. Changing it keeps every
existing pattern in its old meter, so nothing you already wrote is cut into different bars. Tick
**Also change the meter of existing patterns** to move them to the new meter as well. Either way
it is one undo step.

## Adding a change

- Right-click the playlist ruler and choose **Add meter change here...**, or pick a meter from the
  **Add meter change** submenu.
- Press **Alt+Shift+T** to add a change at the song position.
- Once a song has a change, a thin **meter lane** shows above the ruler. Double-click it to add a
  change at the bar line under the pointer.

A new change goes on the bar line at or before the place you chose, and starts with the meter in
force there. Its editor opens so you can pick the meter right away.

## Editing a change

Click a change in the meter lane to open its editor:

- **Beats** and **Note**: the meter, such as 7 and 8 for 7/8. The common meters are one click away.
- **From bar**: where the change starts.
- **Name**: an optional label, shown as a marker in the ruler.
- **New patterns here use this meter**: new patterns made while the song position is in this
  section take its meter. Turn it off for a change that should only shape the playlist.
- **Remove** deletes the change.

Drag a change along the lane to move it to another bar line. Hold **Alt** while dragging to place
it anywhere. Right-click a change for **Edit meter...**, a **Meter** submenu, **Jump here** and
**Remove meter change**. Point at a change and press **Delete** to remove it. Every edit is one undo
step.

## Short bars

When a change sits inside a bar (placed with Alt, or brought in from a file), the bar before it
ends early. That short bar keeps its number and is drawn hatched in the lane; hover it to see how
many beats it has. The next bar starts at the change.

## What follows the meter

- The playlist grid, bar shading and bar numbers. Compound meters (6/8, 9/8, 12/8) also draw a
  line at each group of three eighths.
- Snapping, moving clips by whole bars, and the grid sizes **Bar** and **Beat**.
- The position display and the meter display in the top bar.
- The metronome and the count-in. Compound meters click every eighth, with a medium accent on each
  group of three and the strong accent on the bar.
- The bar position and meter sent to plugins.
- WAV export: each change becomes a cue point named by its meter ("6/8", or "Bridge (6/8)").

## Patterns and meters

A pattern follows the song's meter by default. Give it a meter of its own from **Meter** in the
pattern menus (the main **Patterns** menu, or right-click a pattern in the pattern selector or the
playlist's picker).
The piano roll and the channel rack show a pattern in its own meter.

New patterns get steps per bar that keep a step a sixteenth: 16 in 4/4, 14 in 7/8, 12 in 6/8.
Existing patterns keep their steps.

When a pattern clip starts in a section whose meter differs from the pattern's, its title shows the
pattern's meter. **Make pattern meter match here** in the clip's menu gives the pattern that
section's meter while keeping its length.

## Files

- Projects keep their meter changes. A project with no meter changes saves exactly as before.
- FL Studio projects: time signature markers come in as meter changes, and a pattern's own time
  signature as its meter; both go back out on export.
- MIDI files: import turns the file's meter changes into meter changes in the playlist. Tick
  **Split into patterns at meter changes** to get one pattern per meter section, each in its own
  meter, with its clip at the section start. MIDI export writes the meter changes, and a range that
  starts after a change begins in the meter in force there.

## Scripts and the MCP tools

Project scripts have `project.song.meterAt(tick)`, `barToTick(bar)`, `tickToBar(tick)`,
`addMeter(bar, beats, note)` and `removeMeter(bar)`, with bars counted from 1. The MCP tools report
the song's meter changes in `get_song` and take a `meter` for `edit_pattern` ("7/8", or "song" to
follow the song's meter).

## Shared projects

Meter changes reach everyone in a shared project. Everyone needs this version of Gridline: an
older collaborator is asked to update when joining.
