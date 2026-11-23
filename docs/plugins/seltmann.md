# Seltmann (`gl.seltmann`)

Seltmann chops samples for you. Drop anything on it: a break, a drum loop, a melodic loop, a
whole song, a vocal, a one-shot or an ambient texture. It works out what the sound is, its tempo,
its bars and where bar 1 starts, and chops it at a size that suits it. Pick a layout such as
A B A C, play it in time with your project, and drag the loop out.

It is in the plugin picker under Sampler. Search for "chop", "chopper", "flip", "loop maker",
"microsampling", "beat slicer" or "bpm".

## One flow

1. **Drop audio.** An empty Seltmann is one big drop zone. Drop a file from Finder or Explorer,
   from Tracklib or another sample store, from Gridline's browser, the playlist or the audio
   editor, or click **Load audio...**. Dropping audio on a Seltmann channel row in the channel
   rack loads it into that Seltmann too. In the browser, right-click an audio file and choose
   **Open in Seltmann** for a new Seltmann channel with it.
2. **It analyses on its own.** A progress bar shows the steps. Short loops take a moment; songs
   of 5 to 10 minutes take a second or two and the window stays usable. Results are cached.
3. **Pick a layout.** Hold the mouse on a card to hear it on your chop; click it to keep it.
4. **Play it and take it out.** Play the regions from the keys or your keyboard, drag the **LOOP**
   or **MIDI** handle into the project, click **Make loops** or **Send to project**.

## The editor

The waveform and the sequence share one line: the blocks under the waveform are the steps of the
sequence, in playing order, each under the bars it plays. Everything else opens as a card next to
what you clicked and closes with Esc, its close button or a click outside.

- **Header**: what Seltmann heard (Break, Loop, Song, One-shot or Ambient), the file name, the
  number of bars, the length, the regions, how it is chopped and the key. On the right: the
  **BPM** with **/2** and **x2** to fix a tempo read at half or double speed (double-click the
  number to type one) and **Tap**; **Follow**; the **Sound**, **Grid** and **Loops** cards;
  **Load...**; **Record take**; and the height.
- **Follow** sets how the project tempo is followed: **Stretch** keeps the pitch, **Resample**
  changes speed and pitch together like a record, **Off** plays at the sample's own tempo.
  With Stretch, a tempo ramp or a tempo you drag keeps every slice in tune as it plays (and in an
  export); right after a jump to a new tempo the first slice can play sped up or slowed down for a
  moment while the new tempo is prepared.
- **Waveform**: the bars being chopped with their numbers and the regions. For songs, a strip
  above it shows the whole song with its sections: drag the box to pick other bars, drag its edges
  to resize it, double-click a section to take it, or click a section button under the sequence.
  The eye button hides the strip.
- **Sequence**: one block per step. Click a block for its step card, drag it to move it, drag its
  end to make it longer or shorter, drop a region key on it to change what it plays. **Steps**
  opens the step card for the whole sequence and **Reset** puts the regions back in order.
- **Blank steps**: right-click a block and choose **Remove from the sequence** to leave a blank in
  its place: the blank is silent and keeps the step's length, so the sequence keeps its length and
  the later steps stay where they are. A blank shows as an empty tile with a dashed edge and a
  plus. Right-click it and pick a region under **Fill with**, drag a region's pad onto it, or click
  it and pick one under **Plays** in its card. **Delete step** in the same menu takes a step out
  instead, and the later steps move up. Each is one undo step.
- **Action row**: **Play**; **Chop** (Auto, 1 bar, 1/2, 1/4, 1/8, 1/16, Hits) with **More...**
  for every chop mode; the **Tool**; the **LOOP** and **MIDI** drag handles, **Make loops** and
  **Send to project**.
- **Hint line**: what the tool does, and the editor's own keys.

### Tools

What a click in the waveform does:

- **Play**: click a region to hear it and open its card. The wheel changes its pitch, and dragging
  a cut resizes the regions on both sides.
- **Cut**: double-click to add a cut, drag a cut to move it, right-click a cut to remove it.
- **Grid**: drag the bar-1 flag to move the grid, or Alt+drag a bar line to stretch it.

### Heights

The editor fits the room it has. **Auto** picks the height from the room: in its own window it is
Full; in a layout's pane or band it is Compact below about 330 px, Standard below about 460 px
and Full above. You can also choose a height; it is remembered for you, not saved in the project.

- **Compact**: one row (play, the file, the tempo, a short chop choice, the layout, the drag
  handles and a menu with everything else), the waveform and the sequence.
- **Standard**: the header, the waveform and sequence, the action row, the hint line and a strip
  of layout cards.
- **Full**: the layout library on the left, and the region keys under the action row.

### Cards

- **Region**: the clicked region's pitch, gain, fade in and out, start, length, choke group,
  reverse, a drag handle (WAV), **Split** into 2, 3, 4 or 8, **Join** with the one before and
  **Start here**.
- **Step**: one step's pitch, gain, start, length and stutter, what it plays, reverse, longer,
  shorter, copy and delete. **Region / This step** switches between the region and the step.
- **Sound**: what the keys play, the pattern speed, lock to song, mono, the base note, and swing,
  gate, velocity, pitch, attack, decay, sustain, release, declick, cutoff, resonance, pan, volume
  and reverse for every region.
- **Grid**: tap tempo, nudge the grid by 1 or 10 ms, the meter (3/4, 4/4, 6/8), the key, how sure
  the analysis is about the tempo, bar 1 and the meter, and **Analyse again**.
- **Loops**: the pattern, the best phrases and every section, each with a play button and a drag
  handle.
- **Chop** (More...): every chop mode (Grid with 8 bars down to 1/64 and triplets, Hits with a
  sensitivity, Sections, Equal, Grains, Manual), snapping (grid, hits, zero crossings) and
  zero-crossing cuts.
- **Steps**: the step length and the number of steps, **Reset order**, and the sequence as text:
  `A`..`Z` are letters, numbers are regions, `.` rests, `~` holds the previous step; options follow
  a colon: `r` reverse, `p+7` pitch, `g-6` gain dB, `l50` length %, `x4` stutter, `o25` start %.
  Example: `A B:r A C:x4`. The letters are resolved to regions when you apply it.
- **Takes**: the takes recorded into this instance; pick one to use it.

### Keys

These keys work while the editor has the focus, and only where no key set uses them:

| Key | Does |
|---|---|
| Left, Right | The previous or next region (and hear it) |
| S | Split the region in two |
| J | Join the region with the one before |
| 1 to 6 | Chop: Auto, 1 bar, 1/2, 1/4, 1/8, 1/16 |

Playing the region, playing the pattern, playing a step backwards and going back to the Play tool
are under **Region and keys** in the region menu (right-click a region) and in Compact's menu, because their keys
belong to other commands.
When the computer keyboard plays notes, the keys it plays are left to it.

## How the analysis reads your audio

- **Tempo** comes from where hits and notes start in eight frequency bands, the strongest repeating
  period, and a check of whether every beat or every other beat is strong (half or double tempo).
  The beats are followed through the song, allowing for drift.
- **The file name helps**: a tempo in the name ("92 BPM", "92bpm", "--110.00bpm-", the number in
  sample-pack names such as "KMRBI_GHHS_80_drum_loop", or a folder such as "Cool 165.5") picks
  the octave and, when the name says "bpm", is trusted over an unsure reading. A key in the name
  ("Am", "F# min", "D# Maj") is read too. Numbers that are indexes ("Drumloop 62", "Track 41")
  are not tempos.
- **Loops lock to their length**: a file shorter than 45 seconds that holds a whole number of
  beats gets a grid fitted exactly to it. The tempo is beats x 60 / length, choosing the beat
  count nearest the heard (or named) tempo, so a 2-bar loop is exactly 2 bars and every slice
  has the same length. The fit is used only when the beats Seltmann hears agree with it.
- **Leading silence and pickups**: the chop starts at bar 1, after any silence. A pickup hit
  before bar 1 is left outside, and bar 1 snaps to the first strong downbeat.
- **No slivers**: the chop always starts on a bar line. No slice is shorter than half a step (or
  40 ms for hit chops); smaller fragments join their neighbour. So slice A is never a tiny piece of
  the start.

## Layouts

59 layouts, plus your own:

| Tab | Layouts |
|---|---|
| Flip | Instant Flip, Chop and Bounce, Soul Stab, Halftime Flip, Reverse Hook, Stutter Drop |
| Phrase | Verse Turn (A B A C), Triple Lift (A A A B), See-Saw (A B A B), Pairs (A A B B), Straight Run (A B C D), Mirror (A B B A), Homecoming (A A B A), Round Trip (A B C A), Long Story, Call and Reply; One, Two, Four and Eight Bar Loop, Half Bar Loop, Beat Loop, Reverse Tail, Fading Repeat |
| Breaks | Original Order, Kick Swap, Second Bar Flip, Backbeat Push, Two-Step Shuffle, Broken Groove, Ghost Notes, Rewind Break, Lazy Swing, Heavy Shuffle; Stutter Fill, Snare Riser, Turnaround Roll, Reverse Swell, Drop Gap, Slow Down, Echo Tail, Triplet Fill |
| Micro | Grain Pulse, Micro Stutter, Grain Cloud, Micro Melody, Micro Arp, Pitched Crumbs, Glitch Cuts, Tiny Bounce |
| Creative | Key Scatter, Shuffle Deck, Backwards Mirror, Octave Jumper, Half-Time Drop, Double-Time Run, Skipping Record, Vinyl Spin, Gated Chops |

Layouts work on any material. When the sound is shorter than the layout's bars, the layout keeps
its number of slices with a finer chop: a 4-bar phrase layout on a 2-bar loop uses half bars, a
2-bar break layout on a 1-bar break uses sixteenths. A sound shorter than a bar is cut into as
many equal parts as the layout needs. Steps never point at a slice that is not there. Breaks keep
their played feel: the cuts land on the hits.

Generated layouts (Key Scatter, Shuffle Deck, Micro Melody, Micro Arp, Grain Cloud, Skipping
Record) roll a new take when you click their card again. Under **Mine**, name and save the
current chop and pattern as your own layout.

At Standard height the layouts are a strip of cards under the editor; at Full height they are a
list on the left, grouped by tab with a search field; at Compact height they are a menu.

- **Hear before you keep**: hold the mouse on a card (or the play button of a row) and the layout
  plays on your chop as a loop; let go and your sequence is back as it was. Nothing changes until
  you click. Right-click the layouts and choose **Play on hover** to hear them while the pointer
  rests on a card instead.
- **Starred** and **Recent**: click the star of a layout to keep it under Starred. The last three
  you used are under Recent. Both are remembered for you, not saved in the project.
- **Custom (yours)**: when you pick a layout after changing the sequence by hand, your sequence is
  kept under Recent as Custom (yours), so you can go back to it.
- Applying a layout is one undo step.
- Picking a layout while the sequence plays changes it at its next step, in time and at the right
  pitch from the first note, also with **Follow: Stretch** at a tempo other than the loop's.

## Playing it

Seltmann is an instrument. From the base note (C4 by default) the keys play the slices in order
(or the letters A, B, C...). The octave below plays the pattern: the lowest key at the original
pitch, the others transposed. Hold it to keep the pattern going; with Lock to song on it follows
the song position.

## Loops out

- **Make loops** renders the pattern, the best phrases and the sections as WAV loops at the
  project tempo into a "Seltmann loops" folder next to the project (shown in the browser). Loops
  are cut at whole bars with continuous seams.
- Drag **LOOP** (the pattern as audio), **MIDI** (its notes), a region's **WAV** handle or a row
  in the Loops card onto the playlist (audio clip), the channel rack (sampler channel), a channel
  row or the piano roll.
- **Send to project** adds a pattern with the steps as notes on this channel and places it in the
  playlist. The pattern stays linked: when you change the sequence, the chop or the tempo, its
  notes follow, in the same undo step. Once you edit its notes in the piano roll it stops
  following and the editor shows **edited** with **Relink**, which makes it follow again (your
  edits go). In the piano roll the keys of a Seltmann channel are named after its regions.

## Record a take

**Record take** in the header records from your audio input into this Seltmann, and an empty
Seltmann offers it next to Load. Into a loaded instance a take can **Keep the chop** (the regions
and the sequence stay, at the same tempo) or be **analysed fresh**. Each take is kept in the
project's Recorded folder and listed in the **Takes** card, so you can go back to an earlier one.

## In the layouts

- **Studio**: the editor is the instrument window, at Full height.
- **Clips**: opening the channel opens the editor in the instrument window, as in Studio; its card in the device band keeps its main knobs, and the card's window button opens or closes the window.
- **Smooth**: Seltmann has a tab in the lower pane. Opening a Seltmann channel shows it there; with
  several Seltmann channels, the chips at the top pick which one it shows.
- **Tracker**: Seltmann has screens of its own in the lower band, drawn like the rest of the
  Tracker. See [In the Tracker](#in-the-tracker). The editor above still opens from the
  instrument's **Open plugin window**.

### In the Tracker

With a Seltmann as the current instrument, the header's **Sample** button reads **Chop**.

- **Chop** (the header's Chop button, or **Ctrl+Alt+E**) shows the waveform with its lettered
  regions, each with the key that plays it. The sequence sits beside it as rows (step, region,
  reverse, pitch, length, options). Under them are the band's buttons: Stop and Pattern, the chop
  sizes, the Play, Cut and Grid tools, Split, Join, Duplicate, Delete, Start here, Reset order, the
  region's or the step's values, the LOOP and MIDI drags, Send to project, Make loops, Load,
  Record take, the tempo with /2, x2 and Tap, Follow, and the way out. A short band shows two rows
  of buttons and keeps the rest under **More...**, which also opens every value, the chop modes,
  the steps, loops, grid and takes in a box.
- **Instrument** (**Ctrl+Alt+I**) shows Sound (follow, speed, keys, lock, mono, reverse and every
  knob as a stepper), the Layouts list with its groups (Apply, Hear while held, Star, New take,
  Save as..., Delete), Grid (tempo, /2, x2, Tap, nudge, meter, key, how sure, Analyse again,
  Bar 1 here, the takes) and the key map on a piano along the bottom.
- The **Chop** page, beside the Tracker's other pages while the project has a Seltmann, has
  everything on one page: the layout library, the waveform with the sequence on the same time
  line, the region's values and edits, the band's buttons, the key map, the sequence rows, the
  takes and the loops to drag out.

Every edit there is the same edit as in the editor above: one undo step, the same saved project
and the same for everyone in a collaboration session. Pattern cells of a Seltmann track show the
region each note plays after the note (P and +1 to +11 for the pattern octave).

Keys of the Chop screen: Left and Right pick the region (heard), Up and Down the step, Shift+Up
and Down move the step, Tab switches the values between the region and the step, Enter hears it,
Shift+Enter or Alt+P plays the sequence, Alt+Q stops, Insert splits, Backspace joins, Delete
deletes the step, Alt+1 to Alt+7 set the chop size, Alt+E picks the next tool, Alt+R reverses,
Alt+D duplicates the step and Alt+Up and Down change the pitch. Keys of the Instrument screen: Up
and Down pick a layout, Left, Right and Tab the group, Enter applies it, Alt+P hears it, Alt+F
stars it and Alt+Up and Down move the base note. On both, Esc closes an open box and then the
screen, and the note keys play the regions. The Tracker help lists them under **Editor keys**.

## Saving and exporting

Projects keep the file, the beat grid, the region, the chop, the slices and the pattern, so nothing
is analysed again when a project opens. The main controls can be automated.

FL Studio export: FL Studio has no Seltmann of its own. How the channel goes into an FL Studio
project follows **Settings, Project, FL Studio export** (the export window can change it for one
export, or for the project):

- **Gridline Suite plugins, with muted audio copies** (the default): the channel holds Seltmann
  from the Gridline Suite with all its settings, and the sample travels inside the plugin's
  settings (up to 64 MB of files per plugin). With the Gridline Suite installed, FL Studio opens it
  live and editable. A muted copy of the channel, right after it, plays the same notes rendered to
  audio: on a computer without the Suite, unmute the copy.
- **Gridline Suite plugins only**: the same, without the audio copy.
- **Rendered to audio**: the channel is rendered to audio like other Gridline instruments without
  an FL Studio counterpart, so the song sounds the same, but it is no longer Seltmann.

A sample larger than the limit is not carried: it is copied into the sample folder next to the
project, where you can load it into the plugin again. Opening the project in Gridline again gives
back Seltmann with its exact settings.

## Command line

`gridline seltmann-analyse [--json] [--no-hints] [--presets] <audio files...>` prints what
Seltmann hears in each file (tempo, the file name's tempo, kind, chop, slice checks). It is useful
for checking a sample library.
