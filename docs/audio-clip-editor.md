# The Audio clip window

The Audio clip window is one place for an audio clip and the sampler channel that plays it: the
clip's own settings, its sample's settings (shared by every clip of the sample and by the notes
the channel plays), the channel's note settings, and the audio tools. It replaced the separate
audio clip editor and the sampler channel's settings window.

It opens from:

- a **double-click** on an audio clip, and the clip menu's **Edit clip...** (on the **Clip** tab);
- **Channel settings** of a sampler channel, wherever you open them (the channel rack's button or
  double-click, a clip's or a track's menu): on the **Playback** tab, with the channel's clip (the
  one selected, else its first). A sampler channel that no clip plays opens the same window without
  the clip's tabs, its whole sample in place of the clip's waveform.

The window remembers the tab you were on.

## Main and Advanced

The window (Studio's, and the window the other layouts open for a sampler's settings) has two
views, picked with **Main | Advanced** in its header. Gridline remembers the one you used last.

- **Main** is one calm page with the settings the sampler's own small window had, on its four
  pages (Sample, Envelope and filter, Misc, Sample FX): the header, the whole sample with its blue
  start and end handles (drop a file on it to replace the sample, or use **Replace sample...**),
  then the groups in that window's order: **Playback**, **Time stretching** and **Processing**;
  **Volume envelope** and **Filter**; **LFO**, **Root and voice** and **Channel**; **FX shape**,
  **FX tone** and **FX space**. Each knob shows its name above and its value below. It fits the
  window at its normal size without scrolling; in a small window the page scrolls. The clip's own
  settings (gain, fades, warp and the rest) are on Advanced.
- **Advanced** is the full window described below: the tools row, the clip's waveform editor and
  the tabs, with the clip's fades and warp, the channel's note settings, note effects, MIDI and
  macros, Edit audio and the analysis.

Both views change the same settings: a knob moved on one shows on the other, and each change is one
undo step. Switching keeps the clip, its time selection and the tab you were on.

Where it shows:

- **Studio:** a window of its own.
- **Clips:** the band at the bottom shows it in the band's own way, as cards side by side like the
  device chain's, all inside the band (nothing opens over the clip grid or the arrangement). The
  clip band shows the selected audio clip; the device band shows a sampler channel in its card the
  same way (with the clip chosen in the arrangement, else its first clip, else the channel alone).
  From the left:
  - the **head card**: the colour, the name, the previous and next preset and the menu in its
    title; the file, its rate and length, its warp markers, how many clips share the sample and
    how many more are selected; the mixer track (and a free one), **Open in Loupe**, and the
    channel's **Volume**, **Pan** and **Pitch**;
  - the **sample display**: the tools (pointer, warp, slip, fade, slice, pencil, scrub, zoom),
    **Snap**, the preview and its loop, the view menu (**Wave** or **Spectrum**, **Bars** or
    **Time**, the snap grid, **Follow**), the live analysis and **Edit audio...**, over the
    waveform and its **SAMPLE** strip (without a clip: the whole sample with its handles and loop);
  - a card per module, in the tabs' order: for a clip of the clip grid **Launch** and **Follow
    action** first, then **Clip**, **Fades**, **Warp**, then the sample's (**Sample**, **Sample
    warp**, **Playback**, **Voice**, **Envelope**, **Filter**, **LFO**, **FX shape**, **FX tone**,
    **FX space**) and the channel's (**Channel**, **Polyphony**, **Echo**, **Layer and MIDI**,
    **Note processors**, **Macros**). Each card has every control of its module, fitted to the
    band's height (the display shows when the band is tall enough for it).

  The cards scroll sideways inside the band when they do not all fit (the mouse wheel over them,
  or the scroll bar under them). Click a card's arrow, or double-click its title, to fold it to a
  narrow strip; click the strip to open it again. **Channel settings** of a sampler select its
  card and show it there, with that tab's first card in view; a tab asked for does the same. Asked for in a window (the device's window
  button), the settings open as this window. Nothing opens above the band: other instruments and
  effects open in their own windows, as in Studio.
- **Smooth:** in Smooth's own places. The inspector's **Region** card has the region's own
  settings as rows, under its name, mute, loop, position and length: **Clip** (gain, semitones,
  cents, normalize gain, reverse, the loop's start; takes and **Reset loop start** in its menu),
  **Fades** (shapes, lengths, curves, **Auto crossfade**) and **Warp** (follow tempo, fit to tempo,
  half, twice, original, stretch, speed, the warp markers and their menu, the stretch engine).
  Double-clicking an audio region opens the card. The lower pane's **Audio** tab is the audio
  editor: a local inspector on its left (the region and its channel, the menu and the presets at
  its head with the mixer track box under the name, then a section for each of the sample's and
  the channel's modules, **Output** last;
  click a section's name to fold or unfold it) and the tools and the waveform on its right.
- **Tracker:** the Sample editor in the lower band holds this window, drawn in the Tracker's own
  look (its bevelled panels, buttons, knobs and colours). Every way in opens it there: a
  double-click on an audio clip, **Edit clip...**, a Sampler's **Channel settings** and **Open
  plugin window**, the **Audio** page and Ctrl+Alt+8, and Ctrl+Alt+E on a Sampler. The list on its
  left picks a page: **Waveform** (the tools row and the waveform with the SAMPLE strip under it),
  then one page for each tab, with the tab's modules and **Output** at the right and the SAMPLE
  strip above them when the band has the room (short bands show the modules compact). **Play**
  and **Edit audio...** sit under the list on the settings pages; the mixer track box (with the
  free track's button), the preset arrows and **Menu...** sit in its title bar. **Alt+P** plays the clip, and again stops it; Esc or **Exit** closes the
  page. A Tracker Sampler keeps its own Sample editor (see
  [tracker-sampler.md](tracker-sampler.md)).

**Settings > Interface > Double-click an audio clip** chooses what a double-click opens: **Clip**
(this window on the Clip tab), **Playback** (on the Playback tab) or **Loupe**.

## The header

The clip's (or channel's) colour and name, its warp markers, the file (its rate, mono or stereo
and length) and how many clips share the sample. With several audio clips selected it says how
many more are selected. On the right: the mixer track box, where a plugin window's header has it
(it shows the track the channel plays through; click it for the mixer's tracks, **Route to a free
mixer track** first; each pick is one undo step) and the free track's button beside it, the
sampler's presets (previous and next when the header is narrow) and the menu (the sampler's
presets, load and save, copy and paste, randomize, reset, replace the instrument, rename, clone,
colour, delete, the piano roll, **Show in the channel rack**, **Make this clip's sample unique**
and the **Audio tools** submenu). The channel's volume, pan, pitch and **Open in Loupe** are in the
**Output** module (see below).

## The waveform

The waveform shows the clip as it plays: warped, looped and at its gain, inside the rest of its
sample (dimmed). The mouse wheel scrolls, Ctrl+wheel zooms at the pointer.

Under it, the **SAMPLE** strip shows the whole file: the blue handles set where the sample starts
and ends (for every clip of it and the notes it plays), the orange bar shows the notes' loop when
it is on, and the orange box is the part in view (drag it to move the view). Drop an audio file on
the waveform or the strip to replace the sample.

The tool row above the waveform:

- **Pointer:** drag the orange edges to trim the clip, the body with Ctrl+Shift to slip its audio,
  the white squares to set the fades, the dots in the fades to bend their curves and the dashed
  line to set the clip's volume. Double-click a fade's dot or the volume line to reset it. Drag
  anywhere else to select time; right-click for the selection's menu.
- **Warp:** the clip's warp markers, as in the playlist's Warp tool (see
  [warp-markers.md](warp-markers.md)). W in Studio, Alt+W in the other layouts.
- **Slip, Fade, Slice, Pencil** (draws the volume line), **Scrub** (hear the audio under the
  pointer) and **Zoom** (click zooms in, right-click out).

**Wave / Spectrum** shows the waveform or the spectrum (Shift+W), **Bars / Time** the ruler in bars
or minutes and seconds. The snap button and list set where edges, fades and markers land; hold Alt
while dragging to place them freely. The play button plays the clip alone (through its channel and
effects), with or without a loop. **Follow** keeps the song position in view while the song plays.

Further right, the live analysis of the clip (or of the time selection, or of the whole sample
without a clip): peak, RMS, loudness, true peak, tempo, pitch, DC and length, as many as fit.
**Edit audio...** at the right end opens the audio operations (see below). Without a clip the row
has just the analysis and **Edit audio...**.

Drag the line above the tabs to give the waveform or the settings more room; drag it low and the
modules show just their knobs (double-click the line: as they were). The place is remembered.

## The tabs of modules

Under the waveform the settings are in tabs, grouped under **This clip**, **The sample** and **The
channel**: **Clip**, **Warp**, **Sample**, **Playback**, **Envelope**, **Filter**, **Sample FX**,
**Channel**, **Note effects** and **MIDI and macros**. Click a tab to show its modules, side by
side and sharing the width. In a 1440 x 900 window each tab fits in one row; when the room is
narrower the modules wrap to a second row, and only when the pane is too short for that do they
scroll up and down (never sideways). In a narrow bar the tabs that do not fit are under **More**
at its right.

Each module has its title, a word for what it changes (**clip**, **sample**, **rendered** or
**channel**, in blue, orange or green) and, where it has rarer options, a **More options** menu at
its right. Every knob has its name above it and its value below it. In a short pane the modules
show just their knobs, so the waveform keeps its room; drag the line above the tabs to give them
back their full height.

**Output** stays pinned at the right of every tab: which mixer track the channel plays through,
**Open in Loupe** and the channel's **Volume**, **Pan** and **Pitch**, always in view (in a narrow
pane it sits after the tab's modules instead). The mixer track box itself is in the header.

Right-click any of the sampler's or the channel's controls (a knob, a switch, a segmented choice
or a value field) for the same menu every plugin control has: **Reset to default**, **Type a
value...**, the automation items, **Modulate** and the controller link. The clip's own settings
are not parameters and have no such menu.

The tabs and their modules:

**This clip** (blue)

- **Clip** tab: **Launch** (Clips, a clip of the clip grid: its launch settings and follow
  actions), **Clip** and **Fades**.
- **Clip:** **Gain**, **Semitones** and **Cents**, **Normalize gain**, **Reverse** (this clip),
  **Mute**, **Loop** and the loop's start. Its menu has **Reset loop start** and the takes (the
  take, its rating and **Show take lanes**). With several audio clips selected, a change moves
  every selected clip by the same amount, as one undo step.
- **Fades:** each fade's shape (**Smooth** or **Straight**), length and curve; **Auto crossfade**
  in its title.
- **Warp:** **Follow tempo**, **Fit to tempo**, **Half**, **Twice**, **Original**, **Stretch** and
  **Speed** (the speed changes its pitch too, like a record), and the warp markers (**Add at
  transients**, **Quantize to grid** with its **Strength**, **Clear**). Its menu has **Swing to the
  song's groove**, **Take its groove as the swing** and **New markers snap to transients**. Its
  orange part is the sample's stretch engine: the engine, the tempo the sample follows from,
  **Pitch shift** and **Time stretch**.
- **Warp** tab: **Warp** and the sample's **Sample warp** (below).

**The sample** (orange: a change here changes every clip of the sample and the notes the channel
plays)

- **Sample** tab: **Sample** (without a clip, also **Stretch engine** and **Sample warp**).
  **Playback** tab: **Playback** and **Voice**. **Envelope** tab: **Envelope** and **LFO**.
  **Filter** tab: **Filter**. **Sample FX** tab: **FX shape**, **FX tone** and **FX space**.

- **Sample:** the file, the sample's start and end (the blue handles), **Remove DC**, **Invert
  polarity**, **Swap stereo**, **Fade in** and **Fade out**. Its menu has **Replace sample...** and
  **Make this clip's sample unique**.
- **Stretch engine** (without a clip): the engine, the tempo the sample follows from, **Pitch
  shift** and **Time stretch**.
- **Sample warp:** the sample's own warp markers, which stretch the sample its notes play
  (right-click for the Warp menu, see [warp-markers.md](warp-markers.md)).
- **Playback:** **Loop** (forward or ping-pong), **One shot**, **Reverse sample** (its notes and
  every clip of it), **Loop start**, **Loop end**, **Crossfade** and **Declick**.
- **Voice:** the root note, a keyboard to hear the sample at each note (right-click a key to make
  it the root note), **Root**, **Fine**, **Pan**, **Voices**, **Glide** and **Volume**.
- **Envelope:** the notes' volume envelope (delay, attack, hold, decay, sustain, release,
  velocity).
- **Filter:** the type, cutoff, resonance, envelope amount, key and velocity, and the filter's own
  envelope.
- **LFO:** what it moves (volume, pan, cutoff or pitch), its shape, speed, amount and delay.
- **FX shape**, **FX tone** and **FX space:** effects rendered once into the sample: ramps, time
  warp, boost, stereo delay and stereo fade; filter, tilt and ring modulation; reverb (room or
  hall).

**The channel** (green)

- **Channel** tab: **Channel** and **Polyphony**. **Note effects** tab: **Echo** and **Note
  processors**. **MIDI and macros** tab: **Layer and MIDI** and **Macros**.

- **Channel:** how the channel plays its notes: tuning and time, and expression.
- **Polyphony:** voices, glide, mono and porta, and cut groups.
- **Echo:** the channel's note echoes.
- **Layer and MIDI:** layer children and the layer mode, display group and MIDI input.
- **Note processors** and **Macros.**

## Edit audio and analysis

**Edit audio...** in the tool row has Loupe's operations on the clip's audio, or on the time
selection (on the whole sample without a clip): cut, trim, silence, fades, **Normalize the file**,
amplify, **Reverse the file**, invert, gate, denoise, declick, stretch and pitch, tune to a note,
resample, equalize, reverb, blur, mono, stereo and swap. Each writes a new file next to the project
and keeps the old one; the clip plays the new file, as one undo step. Its **Send and split** submenu
has **Split stems...**, **Chop**, **To sampler**, **To slicer**, **Export WAV** and **Consolidate**.

The analysis readout in the tool row measures the clip (or the selection, or the whole sample
without a clip). In the Clips band it is in the sample display's tool row, and in Smooth in the audio
editor's tool row.

### Reverse and Normalize

There are three ways to reverse, each saying what it changes: **Reverse** (Clip module) plays
this clip backwards; **Reverse sample** (Playback module) plays the sample backwards for its notes
and every clip of it; **Reverse the file** (Edit audio...) writes a reversed copy of the audio.

**Normalize gain** (Clip module) sets the clip's gain so it peaks at 0 dB without changing the
file; **Normalize the file** (Edit audio...) writes a normalized copy. A project that turned on the
sampler's older Normalize switch keeps playing the same; the Sample module says so and its menu
offers **Make it a normalized file**, which writes the normalized copy and turns the switch off in
one undo step.

Every change in the window is one undo step and reaches collaborators.
