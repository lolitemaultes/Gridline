# Moving around the piano roll

The piano roll shows the notes of one channel in one pattern. These are the ways to move through
the notes and zoom in and out.

## Drawing notes

With the Draw tool, a click adds a note at the default length. **Shift+click and drag** adds the
note and sets its length as you drag (a Shift+click without a drag keeps the default length).
Drag a note to move it, drag its end to resize it, and right-click to delete it.

## Snap and grid

The snap choice (the magnet in the toolbar, or the piano roll menu's **Snap** submenu) sets what
notes land on when you draw, move and resize them. The same list is in the playlist, the
automation editor and every layout's note editor (in the Tracker's Roll page, **More** after the
five snap buttons):

- **Main** follows the playlist's snap. **Line** snaps to the lines you see (finer as you zoom
  in), **Cell** to the cell under the pointer, **None** to single ticks.
- **1/6, 1/4, 1/3 and 1/2 step**, and **Step**. A step is the pattern's own step, so in a pattern
  of 12 or 24 steps per bar Step follows those steps.
- **1/6, 1/4, 1/3 and 1/2 beat**, **Beat** and **Bar**.
- **Dotted step**, **Dotted 1/2 beat** and **Dotted beat** (one and a half times as long).
- **1/5 beat** and **1/7 beat** for quintuplets and septuplets. They do not divide a beat into
  whole ticks, so each line sits on the nearest tick and every fifth (seventh) line is exactly on
  a beat: the lines never drift.

- **Row grid** (only when the edited channel's row has a grid of its own, see
  [docs/channel-rack.md](channel-rack.md#polyrhythms-a-grid-and-a-length-for-each-row)): one step
  of that row, for example "Row grid: 5 per beat". The ruler then shows a bracket over each beat
  with the count, and the pattern's other rows of steps show as ghost notes.

A snap finer than a beat that the visible grid does not already show (triplets, dotted values,
quintuplets, a 12-step pattern's steps) draws its own lines, so you can see what notes snap to.
Hold Alt while dragging to place freely.

## Resizing many notes

Select notes and drag the end of any one of them: every selected note gets longer or shorter by
the same amount, as one undo step. A note never shrinks past one grid cell (or its own length, if
it was already shorter). The piano roll menu's **Note lengths** submenu does it in one click:
**Twice as long**, **Half as long**, **Longer by the snap grid**, **Shorter by the snap grid** and
**All as long as the snap grid**, and **Note lengths...** opens a tool that scales the lengths,
sets them all to one length or makes them longer or shorter by an amount, optionally with the new
ends on the pattern's steps. With nothing selected, these change every note.

## Moving notes in Smooth and Clips

In the Smooth and Clips layouts the piano roll starts with the Pointer (in Clips: draw mode off).
Press on a note and drag it: it follows the pointer the whole way, as many rows and bars as you
drag, and the selected notes go with it. Drag a note's right end to make it longer or shorter.
A drag on an empty spot selects the notes in a box, and Shift+click adds a note to the selection.
Each drag is one undo step. Alt while dragging frees the move from the snap.

Hold Cmd (Ctrl on Windows and Linux) as you press on a note and drag to copy it: the note, and the
other selected notes with it, stay where they are and the copies follow the pointer. Alt still
frees the copies from the snap. The copy is one undo step and reaches collaborators. Studio's
Select tool keeps Ctrl+drag for selecting with a box.

## The chord stamp

The stamp button sits right after the menu button at the left of the piano roll's toolbar (a
hand stamp over three stacked notes). It stays there however narrow the window gets; other
toolbar groups give way first.

- **Click it** to open the chord picker: your recent chords at the top, then every chord type in
  groups (triads; sixths and sevenths; extended chords; octaves), two chords built from the scale
  (when a scale is chosen in the toolbar), the inversion (root, 1st, 2nd, 3rd) and
  **Just the next chord**. Hover a chip for the chord's full name.
- **Pick a chord** and the stamp turns on with it: the Draw and Paint tools then place that chord,
  its root on the key you click (if another tool was chosen, Draw is chosen for you). Each placed
  chord is one undo step.
- While stamping, the button is lit and, on a wide piano roll, shows the chord's symbol beside it
  (for example `m7`, `maj7 /1` for a first inversion). Click the symbol, or right-click the stamp,
  to pick another chord.
- **Click the stamp again, or press Esc** over the piano roll, to turn it off.
- The piano roll menu's **Stamp** submenu has the same choices. **Alt+C** opens the picker from
  the keyboard (shortcut editor: **Chord stamp: pick a chord to stamp**).

The recent chords are kept between sessions (`piano.recentChords`, up to six).

## The scroll bars

- **The time bar** is the slim bar above the ruler. It shows the whole pattern: every note is a
  short mark at its time and pitch (the lowest note at the bottom of the bar, the highest at the
  top), selected notes are lit, and a thin line shows the playback position while the pattern
  plays. The lighter thumb is the part you see.
  - Drag the thumb to move through the pattern.
  - Click beside the thumb to jump a page that way.
  - Drag the left or right end of the thumb to zoom in or out. The other end stays where it is.
  - Double-click the bar to fit the whole pattern in the view.
  - The mouse wheel over the bar (and over the ruler) scrolls sideways.
- **The key bar** is the slim bar left of the keyboard. A small mark shows each pitch that has
  notes.
  - Drag the thumb to move up and down the keys, or click beside it to jump a page.
  - Drag the top or bottom end of the thumb to make the keys taller or shorter. The other end
    stays where it is.
  - Double-click the bar to bring the notes back into the middle of the view.

The bars work the same way as the playlist's.

## Wheel, trackpad and keys

| Do this | To |
| --- | --- |
| Wheel | Scroll up and down the keys |
| Shift+wheel, a sideways wheel or a two-finger swipe | Scroll through time |
| Ctrl+wheel (Cmd+wheel on macOS) | Zoom the time around the pointer |
| Alt+wheel (Option on macOS) | Make the keys taller or shorter around the pointer |
| Middle-button drag | Move the view in both directions |
| Page Up / Page Down | Zoom in / out |
| Home | Fit the pattern |

A wheel step glides to its place and a trackpad follows your fingers exactly. At the edges the
view stretches a little and springs back. You can turn these off in **Settings > Interface**
(Smooth scrolling, Elastic edges, Smooth zooming). They are also off when your system asks for
reduced motion.

The **auto scrolling** button beside the tools makes the view follow the playback position while
the pattern plays.

## The automation lane

The lane under the notes edits a pattern's automation with the piano roll's tools: draw values,
select, move, delete, copy and paste points, and thin out dense ones with **Simplify points...**.
The curve chooser under the lane's name sets whether drawing writes stepped, linear or smooth
values, and its menu sets the curve of the selected points; the automation clip editor shares
the same choice. Drag the bar above the lane to resize it. See
[Automation](automation.md#the-automation-lane-in-the-piano-roll) for every gesture and key.

## Vibrato

Each note can have its own vibrato. Pick the **Vibrato** tool (beside Pitch bend; **V** in the
Studio and Tracker layouts, the tool list in Clips and Smooth, and the MPE menu in Clips). With it
on, every note's vibrato shows as a violet wave on the note: its height is the depth and its
cycles are the rate. The shaded envelope shows the delay, the fade in and the fade out.

- **Drag up** on a note to set its depth (in cents; 100 cents is a semitone each way). **Ctrl**
  drags in fine steps, **Alt** snaps to 25 cent steps.
- **Drag sideways**, or turn the **wheel** over the note, to set the rate.
- Drag the **round handles** sideways to set the fade in and fade out; drag them up or down to
  bend the fade's curve. Drag the start of the envelope to delay the vibrato.
- **Right-click** a note for its shape (sine, triangle, soft square, saw up, saw down), the rate
  in Hz or synced to the beat, where it swings (around the pitch, only above, only below), a rate
  that speeds up or slows down over the note, presets (Gentle, Singer, Violin, Wide, Fast trill),
  copy and paste, a natural variation per note, and **Remove vibrato**.

A drag on a selected note sets every selected note's vibrato the same way. Each drag is one undo
step. The vibrato strip (preset, depth, rate, Sync) holds the vibrato a note starts with, and
changes the selected notes' vibrato too. It sits in the toolbar while the tool is on; where the
toolbar is narrow, a **Vibrato** button (or the tool's wave icon) opens it, in Smooth it is in
the inspector at the left, and in Tracker it is **Vibrato defaults** on the Roll page. The note
properties popup has a Vibrato section as well. In a cramped Studio toolbar the zoom
buttons keep their place: the tools at the end of the tool row fold into one **...** button, whose
list shows them with their keys in the **Tool** menu's order (the tool in use picked). Every tool
keeps its key and its place in the **Tool** menu.

Vibrato plays on top of the note's pitch bend line. Instruments that bend each note on its own
play each note's vibrato; an instrument without per-note pitch gets it as channel pitch bend,
which notes that overlap share (the tool's hint says which). MIDI export writes vibrato as pitch
bend (per note in MPE); the Tracker shows a vibrato note with `~` in its effect column.
