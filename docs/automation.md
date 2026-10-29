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
- **In the channel rack.** A pattern's clips are rows below the channels, with the curve drawn
  across the step area. Click a row's name or its curve to open the clip; right-click it for
  the clip menu.
- **In the browser.** Current project > Automation lists the pattern clips as "in <pattern>";
  double-click one to edit it.
- **In the piano roll.** The event lane at the bottom of the piano roll still shows and edits a
  pattern clip's points (as steps), for the selected channel's controls.

The editor's title shows "in <pattern>" for a pattern's clip. Its side panel renames the clip
and has **Show the pattern**. A clip's menu (rack row, pattern menu) has **Edit in the
automation editor**, **Show in the piano roll**, **Rename**, **Colour**, **Move to the
playlist** (one playlist automation clip under every clip of the pattern, sounding the same) and
**Delete**. Every edit is one undo step; dragging a point is one step however long the drag is.

### Patterns with clips

- **Clone** and **Make unique** copy the pattern's clips; the copies are independent.
- Splitting, trimming, slipping and looping a pattern's playlist clip moves its automation with
  its notes.
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
