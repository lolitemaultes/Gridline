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
3. **Pick a layout.** Click a card: the layout is applied and starts playing at once.
4. **Play it and take it out.** Play the slices from the pads or your keyboard, drag the **LOOP**
   or **MIDI** handle into the project, click **Make loops** or **Send to project**.

## The main view

- **Top line**: what Seltmann heard (Break, Loop, Song, One-shot or Ambient), the file name, the
  number of bars, the length, the key and how it is chopped. **Load...** opens another file;
  **Edit** opens the drawer with the advanced controls.
- **Waveform**: the bars being chopped, with the bar numbers and the slices coloured by their
  letter. Click a slice to hear it. For songs, a slim strip above it shows the whole song with its
  sections: drag the box to pick other bars, drag its edges to resize it, or double-click a section
  to take it. The eye button hides the strip.
- **Key controls**:
  - **BPM** with a confidence line. **/2** and **x2** fix a tempo that was read at half or double
    speed. Double-click the number to type a tempo.
  - **Chop**: Auto, 1 bar, 1/2, 1/4, 1/8 or 1/16. Auto picks the size for the material: breaks are
    cut on their hits in sixteenths (one bar), eighths (up to four bars) or beats; melodic loops in
    beats, half bars or bars; songs in bars; one-shots at their transients; ambient textures in
    eight equal parts.
  - **Tempo**: Project follows the project tempo, Original plays at the sample's own tempo.
  - **Play**, the **LOOP** and **MIDI** drag handles, **Make loops** and **Send to project**.
- **Layouts**: big cards in six tabs (Flip, Phrase, Breaks, Micro, Creative, Mine) with a search
  field. Each card shows its steps as coloured letters or slice numbers and says what it does.
- **Pads**: one per slice, coloured by the letter that plays it. Click to hear it; drag the grip
  of the selected pad to take the slice out as audio, or drag a pad onto a step.

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

## The Edit drawer

**Edit** opens the advanced controls (closed by default):

- **Slice**: the selected slice's pitch, gain, fade in and out, start, length, choke group and
  reverse, its letter, and a drag handle (WAV).
- **Chop**: every chop mode (Grid with 8 bars down to 1/64 and triplets, Hits with a sensitivity,
  Sections, Equal, Grains, Manual), snapping (grid, hits, zero crossings) and zero-crossing cuts.
  While this tab is open you can cut by hand in the waveform: double-click to add a cut, drag a cut
  to move it, right-click a cut to remove it.
- **Steps**: the pattern's steps (wheel: another letter; right-click: reverse, stutter, pitch,
  gain, length, start, ties, rests; drop a pad on a step) and the step length. The **STEPS** field
  is the pattern as text: `A`..`Z` are letters, numbers are slices, `.` rests, `~` holds the
  previous step; options follow a colon: `r` reverse, `p+7` pitch, `g-6` gain dB, `l50` length %,
  `x4` stutter, `o25` start %. Example: `A B:r A C:x4`.
- **Grid**: tap tempo, nudge the grid by 1 or 10 ms, the meter (3/4, 4/4, 6/8), the key, how sure
  the analysis is about the tempo, bar 1 and the meter, and **Analyse again**. With this tab open,
  drag the bar-1 flag in the waveform to move the grid, or Alt+drag a bar line to stretch it.
- **Sound**: how the project tempo is followed (Stretch keeps the pitch, Resample changes speed
  and pitch like a record), the stretch type (Auto, Drums, Mono), what the keys play (slices or
  letters), pattern speed, lock to song, mono, the base note, and swing, gate, velocity, pitch,
  attack, decay, sustain, release, declick, cutoff, resonance, pan, volume and reverse for every
  slice.
- **Loops**: the pattern, the best phrases and every section, each with a play button and a drag
  handle.

## Playing it

Seltmann is an instrument. From the base note (C4 by default) the keys play the slices in order
(or the letters A, B, C...). The octave below plays the pattern: the lowest key at the original
pitch, the others transposed. Hold it to keep the pattern going; with Lock to song on it follows
the song position.

## Loops out

- **Make loops** renders the pattern, the best phrases and the sections as WAV loops at the
  project tempo into a "Seltmann loops" folder next to the project (shown in the browser). Loops
  are cut at whole bars with continuous seams.
- Drag **LOOP** (the pattern as audio), **MIDI** (its notes), a pad's grip or a row in the Loops
  tab onto the playlist (audio clip), the channel rack (sampler channel), a channel row or the
  piano roll.
- **Send to project** adds a pattern with the steps as notes on this channel and places it in the
  playlist.

## Saving and exporting

Projects keep the file, the beat grid, the region, the chop, the slices and the pattern, so nothing
is analysed again when a project opens. The main controls can be automated.

FL Studio export: FL Studio has no Seltmann. When you save as an FL Studio project, the channel is
rendered to audio like other Gridline instruments without an FL Studio counterpart, so the song
sounds the same.

## Command line

`gridline seltmann-analyse [--json] [--no-hints] [--presets] <audio files...>` prints what
Seltmann hears in each file (tempo, the file name's tempo, kind, chop, slice checks). It is useful
for checking a sample library.
