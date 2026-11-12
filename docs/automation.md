# Automation

Any control in Gridline (a knob, a fader, a channel's volume or pan, a plugin parameter, the
tempo) can follow a curve. Curves live in automation clips, and a clip lives in one of two
places:

- **In the playlist.** The clip sits on a playlist track like any other clip and plays where it
  is placed. Use this for changes that belong to the song: a filter that opens over the
  breakdown, a fade out at the end.
- **In a pattern.** The clip belongs to the pattern and plays wherever the pattern plays: every
  clip of that pattern in the playlist, the pattern played on its own in pattern mode, and
  live launches. Use this for movement that is part of the loop itself: a wobble on the bass,
  a pitch wheel bend, a pan that swings every bar.

Both kinds are edited in the same automation editor, with the same points, curve shapes,
tension handles, LFO tool, shape library and recording.

## Getting started

1. Right-click the control you want to move (a knob or a fader in a channel, the mixer or a
   built-in plugin's editor) and pick **Create automation clip**. A clip four bars long appears
   on the playlist (over the selected time when there is one, or at the playback position in
   song mode), holding the control's current value, and opens in the automation editor.
2. In the editor, click to add a point and drag points to shape the curve. Drag the small ring
   between two points to bend the line, and pick a shape from the side panel to fill the clip
   or the selected part with it.
3. Move, stretch or copy the clip on the playlist like any other clip. Wherever it sits, the
   control follows its curve; outside it, the control keeps the last value it was given.

To turn automation off without deleting it, mute the clip (the playlist's Mute tool, or mute
its playlist track). A muted clip leaves the control alone and plays again when unmuted.

## Automation in a pattern

### Adding a clip

- **From a control.** Right-click a knob or fader and pick **Create automation clip in pattern
  "name"**. In pattern mode this is the first choice; **Create automation clip** still makes a
  playlist clip. The new clip holds the control's current value over the pattern's length and
  opens in the editor.
- **From the channel rack.** The rack's menu has **Automation clips in this pattern** >
  **Add automation clip for**, listing the selected channel's controls, the song's and its
  mixer track's.
- **From the pattern menu.** Right-click a pattern in the playlist's pattern list and open
  **Automation clips**. (A pattern clip's own menu in the playlist lists the pattern's clips
  under **Automation in this pattern**.)
- **From the playlist.** Right-click a playlist automation clip and pick **Move into pattern
  "name"** or **Copy into pattern "name"**. The part the clip plays (its offset, length and
  loop) goes into the pattern at the same place, and the song sounds the same. Dragging an
  automation clip from the playlist's clip list onto the channel rack copies it into the
  pattern too.

A pattern holds one clip per control. A clip that would add a second one is refused with a
message.

### Seeing and editing

- **On the playlist.** A pattern's clips show its curves over the notes, each in its own colour
  (clips without a colour of their own get one of a few colours in turn). The curves follow the
  clip's offset, trim and loop passes and stay readable at every zoom and track height. Hover a
  curve to see its name; **double-click a curve** to open that clip in the editor. The
  playlist's View menu has **Pattern clips show**: **Notes and automation** (the default),
  **Notes only** or **Automation only**.
- **In the channel rack.** A pattern's clips are rows below the channels, set apart from them by
  a thin line with a small "Automation" caption, with the curve drawn across the step area. Click a row's name or its curve to open the clip; right-click it for
  the clip menu.
- **In the browser.** Current project > Automation lists the pattern clips as "in <pattern>";
  double-click one to edit it.
- **In the piano roll.** The lane at the bottom of the piano roll shows and edits a pattern
  clip's points for the selected channel's controls (choose one from the menu at the lane's
  name). See [The automation lane in the piano roll](#the-automation-lane-in-the-piano-roll).

The editor's title shows "in <pattern>" for a pattern's clip. Its side panel renames the clip
and has **Show the pattern**. A clip's menu (rack row, pattern menu) has **Edit in the
automation editor**, **Show in the piano roll**, **Rename**, **Colour**, **Move to the
playlist** (one playlist automation clip under every clip of the pattern, sounding the same) and
**Delete**. Every edit is one undo step; dragging a point is one step however long the drag is.

### The automation lane in the piano roll

The lane works like the automation clip editor, with the piano roll's tools:

- **Draw and Paint** draw values as you drag; **right-drag** draws a straight line. The chip
  under the lane's name chooses the curve they draw: **Stepped** (a value on every grid step),
  **Linear** or **Smooth**. A Linear or Smooth stroke keeps only the few points its shape needs,
  and a Linear or Smooth line is just its two ends.
- **Select** (or Ctrl+drag with any tool) drags a box around points; Shift+click adds or removes
  one. Drag a selected point to move the selection (Shift: one direction only, Alt: no snap).
- **Delete** erases the points you drag over. The last point always stays, so the lane keeps
  its name, colour and range; use **Delete this lane** in the lane's menu to remove it.
- **Right-click a point** for its menu: delete, its curve (Stepped, Linear, Smooth or any other
  curve), reset tension, copy, simplify, open in the automation editor. The chip's menu also
  sets the curve of the selected points.
- After a click in the lane, the editing keys act on its points: Delete, Ctrl+A, Ctrl+D,
  Ctrl+C, Ctrl+X, Ctrl+V (pastes over the copied span after the selection, or at the playhead
  while the pattern plays) and Q (quantize). A click in the notes gives the keys back to them.
- **Simplify points...** (the chip's menu, the lane's menu or a point's menu) thins dense
  points, such as recorded controller moves, to a few Linear or Smooth points within a
  tolerance you set, with a live preview; it works on the selection, or on the whole lane when
  nothing is selected. The automation clip editor's **Thin out points...** has the same choice.
- Drag the bar above the lane to make it taller or shorter; double-click it for the automatic
  height. The height is remembered.

Points copied in the lane paste in the automation clip editor and the other way round. Recorded
controller moves are thinned as they are written (within 0.4 %, which you cannot hear), so a
take holds a few points instead of one per message.

### Patterns with clips

- **Clone** and **Make unique** copy the pattern's clips; the copies are independent.
- Splitting, trimming, slipping and looping a pattern's playlist clip moves its automation with
  its notes.
- A playlist clip that shows only part of its pattern or curve (it was cut at either end) loops
  that part when you hold Ctrl (Cmd on macOS) and drag its edge out: the part it showed repeats,
  automation included. Drag the edge back to where it was cut and it is the plain clip again.
- Projects save every field (name, colour, range, loop, curve law). Patterns from projects made
  before pattern automation clips (their plain event lanes) open as unnamed clips with the
  whole range of the control and play exactly as before; a project that only has such lanes
  saves them the way older versions of Gridline read them.
- In a collaboration session the clips sync with the pattern.
- FL Studio export writes a pattern's clips as the pattern's events (curves become steps a 32nd
  note apart; the control's range is applied); import brings them back as pattern clips.
  MIDI export writes volume and pan clips as CC 7 and CC 10.

## Recorded passes

Platter's passes are written as named pattern clips, for example "Platter A wheel", coloured by
deck. In pattern mode they go into the pass's pattern; in song mode each pass gets a new pattern
holding all its clips, placed once on the playlist where you played it, so the curves show on
that one clip and a double-click on a curve opens it in the editor. Platter can write song-mode
passes as separate playlist clips instead (see
[plugins/platter-recording.md](plugins/platter-recording.md)).

When playback starts inside a pattern clip that holds automation, its clips take effect from
that point, as they do when the clip is trimmed, moved, cut or copied.
