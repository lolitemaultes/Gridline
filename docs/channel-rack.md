# Channel rack: cloning, hiding, audio clips, new patterns and polyrhythms

## Clone with or without the notes

Right-click a channel's name for these:

- **Clone** makes a copy of the channel (its instrument, settings and routing) with its steps and
  notes in every pattern.
- **Clone without notes** makes the same copy with no steps and no notes, ready for a new part.
  Shortcut: Alt+Shift+C. The rack's options menu has **Clone selected without notes** for several
  channels at once.
- **Clone with this pattern's notes** copies only the current pattern's steps and notes.

In the Tracker, the instrument menu has **Clone without notes** too. Each clone is one undo step.

In the Clips and Smooth layouts, right-click a track's header for **Duplicate without notes**: a
copy of the track right after it, with a copy of its instrument, its effects and its settings, and
no clips (so no notes). With several tracks selected, each is copied. A track of clips playing
other instruments gets a track of its own playing a copy of their sound. In Smooth, the **Track**
menu in the menu bar has it too, for the selected track.

## A channel's sequence in a new pattern

When a channel has lit steps or notes in the current pattern, its menu offers:

- **Copy to a new pattern**: a new pattern, right after the current one, holding just that
  channel's steps and notes, with the same length, steps and meter. The current pattern keeps
  them. The new pattern becomes the current one.
- **Move to a new pattern**: the same, and the current pattern no longer has them.

In the Tracker, the instrument menu has **Copy its notes to a new pattern**. Each is one undo step.

In the Clips and Smooth layouts, a track's header menu has **Copy to a new pattern** too (in
Smooth, the **Track** menu in the menu bar as well, for the selected track). It takes the track's
notes and steps from its selected clip (else its first clip of the current pattern, else its first
clip) into a new pattern right after that one, which becomes the current pattern, and puts a clip
of the new pattern on the track where that clip is, with its length and settings. The original
clip stays where it was:

- in Smooth the new region lies on the original region, in either **Drag** mode;
- in Clips a clip of the clip grid goes in the slot under the original (a slot holds one clip; when
  that slot is taken, or the original is in the last scene, an empty scene is put in under it, as
  **Duplicate** does); an arrangement clip lies on the original.

The original pattern keeps its notes, and other clips of it stay as they are. One undo step, and it
reaches collaborators.

## Hiding channels

Right-click a channel and choose **Hide from the rack** (or select several and use **Hide
selected** in the rack's options menu). A hidden channel keeps playing and keeps its steps and
notes; it is just not shown in the rack.

While channels are hidden, the rack's toolbar shows how many (for example **2 hidden**). Click it,
or open **Hidden channels** in the rack's options menu, to see them: click one to show it again,
or **Show all**. Hidden channels are saved with the song, are hidden for everyone in a shared
session, and hiding or showing is one undo step.

## Audio clips in the rack

Every audio clip in the playlist plays through a channel, and that channel is in the channel rack.
However the audio got there (dropped from the browser or from outside Gridline, recorded, rendered,
sent from Loupe, split into stems, chopped, or brought in with a project), its channel is listed.

- An audio clip channel is a row like any other sample channel's: its name button, mute, pan,
  volume and a row of steps. Click a step and it plays the channel's sample there, as on any
  sampler channel; the piano roll and the graph editor open from it as from any row. Its clips in
  the playlist play as before. This is the same in every layout that shows the rack.
- Rename it, route it to a mixer track, mute it or change its volume and pan as on any channel.
  **Delete** removes the channel and every clip that plays from it; one undo brings both back.
- The display filter at the left of the rack's toolbar has **Audio clips**, which shows only these
  channels.
- A new channel always comes into view: the rack scrolls to it, and if a display group is shown
  that would hide it, the rack shows all channels again.

**Select unused channels** does not pick audio clip channels: their clips play from them. A channel
from an older project needs nothing done to it: if a playlist audio clip plays from it, it is listed
as an audio clip channel, and it sounds exactly as before.

## Colours of new channels and patterns

New channels and patterns take the next colour of the palette, so neighbours differ. To change
that, open **Settings > Project > New channels and patterns**:

- **New channels**: **Varied colours** (the default) or **One colour**, with the colour beside it.
- **New patterns**: **Varied colours**, **One colour**, or **The channel's colour** (a new pattern
  takes the colour of the selected channel).

Only new channels and patterns follow this; the ones you have keep their colours (right-click one
to change it). It is a setting of this computer, not of the song.

## Polyrhythms: a grid and a length for each row

Each row of a pattern can have its own grid and its own length, so a hat plays 12 to the bar over
a 16-step kick, a shaker plays 5 to the beat, or a bell loops every 9 steps and drifts against the
rest until they meet again.

Turn on **Show row grids and lengths** in the rack's options menu. Each row then shows a chip with
its steps per bar (orange when the row has a grid of its own) and an orange handle at the end of
its steps. A pattern whose rows already have a grid or a length shows them without the setting.

**The grid.** Click a row's chip. Choose a count in **Steps per bar**, **Per beat** or **Per
pattern**, or type any count from 1 to 64 in **Other**. The steps play exactly where they fall in
the bar, also where that is between two ticks (5 or 7 to the bar), so a row of 5 stays in time
however long it plays. Under **The steps already set** choose what happens to steps already lit:

- **Keep them where they sound**: each step moves to the new step nearest to where it played (when
  two land on one, the louder stays).
- **Keep their order**: step 3 stays step 3.

**Back to the pattern grid** gives the row the pattern's steps again; **Every row like this** gives
every row of the pattern this row's grid. A row on its own grid is drawn stretched, so a bar is as
wide on every row.

**The length.** Drag a row's handle to the step where its loop should end (or right-click the chip,
or click the handle, for the length box). The steps after the handle show, dimmed, what the loop
plays there; clicking one of them sets the step it repeats. The length box has **Length** with -
and +, quick lengths (**1 beat**, **2 beats**, **3 beats**, **1 bar**, **From the notes**), and
what the row does **When the pattern starts again**:

- **Keep running**: the row runs on through a longer clip, so a loop of 7 steps over a 16-step
  pattern plays 7, 7, 7 ... across the bars. The rack shows when every row lines up again ("Every
  lane lines up again after 7 bars"). A new loop starts out this way.
- **Start again with the pattern**: the loop restarts at every pass of the pattern, as step loops
  did before.

A row keeps running through a pattern clip in the playlist that loops the whole pattern and through
a Clips slot from its launch. A clip that loops only part of the pattern restarts it at its own
loop, and playing the pattern on its own (pattern mode) plays its first pass each time round.

**Don't loop** removes the length. **Burn into the pattern** writes the row out as plain content:
a row on its own grid becomes notes at the exact places, a row that starts again writes out its
repeats, and a row that keeps running makes the pattern as long as it takes for every row to line
up again (at most 64 bars). Exporting to MIDI or an FL Studio project writes the rows out the same
way, since those files have no such rows.

Every change to a grid or a length is one undo step and is shared in a collaboration session. The
Steps page in the Tracker, the Clips clip view and the Smooth step editor show the same rows. In the
piano roll a row on its own grid adds the **Row grid** snap, a bracket over each beat with the count,
and the other rows' steps as ghost notes. In the Tracker's pattern editor a row on its own grid shows
its steps on the nearest row, marked with ~, and a track loop is drawn as a line with the rows after
it dimmed; **Ctrl+Alt+L** sets the loop of the cursor's track at the cursor row (again on that row
removes it).
