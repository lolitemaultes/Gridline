# Moving around the piano roll

The piano roll shows the notes of one channel in one pattern. These are the ways to move through
the notes and zoom in and out.

## Drawing notes

With the Draw tool, a click adds a note at the default length. **Shift+click and drag** adds the
note and sets its length as you drag (a Shift+click without a drag keeps the default length).
Drag a note to move it, drag its end to resize it, and right-click to delete it.

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
