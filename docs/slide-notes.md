# Slide notes

A slide note does not start a note of its own. It takes the notes of its channel that are
sounding when it starts and moves their pitch to the slide's pitch, in a straight line (in
semitones) over the slide's length. The notes keep sounding at the new pitch until they end or
the next slide moves them on.

## Writing slides

- In the piano roll, turn on **Slide** in the toolbar (or press Shift+S, also in the command
  palette as "Slide mode") and every note you draw is a slide note. Turn it off to draw ordinary
  notes again.
- To make existing notes slides, select them and use **Make slide notes** in the right-click
  menu, or tick **Slide** in the note's properties.
- Slide notes are drawn lighter, with a small arrow head at their end. A faint arrow runs from
  each note a slide will move up or down to the slide, so it is clear what it acts on.

## What a slide moves

- By default a slide moves only the notes of its own colour group, so two lines on one channel
  (a bass and a lead, say) slide on their own. Projects that slide every colour (older projects,
  and the setting for it) keep doing so.
- A slide moves notes that are sounding when it starts. With nothing sounding it is silent.
- Several slides in a row climb on from where the last one left the note.
- A note's own bend line and slides add up: the slide starts from wherever the bend line has
  taken the note.

## Velocity

A slide also takes the notes it moves from their velocity to the slide's own velocity, in a
straight line over the slide's length, at the same time as their pitch. A slide at the velocity
the note already has changes only its pitch.

- **Fade in**: draw a note at 0% velocity and a slide note at the velocity it should reach. The
  0% note is silent until the slide raises it. (A 0% note without a slide on it stays silent.)
- **Fade out**: a slide at 0% takes the note down to silence by the slide's end. The note stays
  silent until it ends.
- **Crescendo and decrescendo**: a louder or softer slide swells or softens the note.
- **Swells**: several slides in a row go on from where the last one left the note, so slides up
  and down in turn make a note swell and fade again and again. Slides of the same pitch as the
  note change only its level.

A slide that changes the velocity draws its fade inside the note (a line from the velocity it
starts from to its own, with a soft area under it) and in the velocity lane (the lane's bar for the
slide runs from where it starts to the slide's velocity at its end). Velocity goes down to 0% in
the piano roll's velocity lane, its velocity field and note properties, the Smooth event list and
the Tracker's volume column.

The slide's velocity acts on the note's level: a note taken from velocity 40 to 120 plays three
times as loud, and 0% is silence on every instrument (on the plain synth's reference velocity
curve, along that curve). Built-in instruments (the drum kit, the kick, snare, hat and
clap and Seltmann among them) fade each note on its own, so a slide never changes the other notes
still sounding. Known limit: a few fade their whole output instead, which also fades the other notes
sounding on that channel while the slide runs (the next note brings the level back): the tracker
instrument, the patch player, Flex, the turntable, Matrix FM with its reference engine on, Hex
Synth while its sequencer runs and the slicer on its slice-player engine. The step bass plays one
note at a time, so there is nothing else to fade. Installed plugins: VST3 plugins that list a
per-note volume note expression get the fade on each note; other plugins (CLAP, VST2, Audio
Units and VST3 plugins without it) have their whole output faded, which is the same as long as
the slid notes are the only ones playing on the channel. Behind note effects a slide does
nothing, as for its pitch.

**Slides glide velocity**, under **Slide and portamento** in the piano roll's tools menu, turns
this on or off for the project (one undo step, shared in a collaboration session). It is on for
new projects. A project saved before slides glided velocity, with slides that would change a
note's velocity, opens with it off, so it sounds as it did.

## Instruments

Every built-in instrument follows slides note by note: synths, samplers, the multisampler,
keys, plucks and strings, the granular and spectral instruments, and drums whose sound has a
pitch: the kicks (the sweep kick and the sculpted kick too, so a long kick glides),
a kick with key tracking on, the snare, the clap, the modelled membrane drum and the drum kit's
pitched pads. The pad sampler, the slicer and the slice instrument play the sample faster or
slower by the slide's interval, and the scratch sampler does when its keys are set to pitch.
Instruments that run their own sequence (Step Bass and Acid Bass while their sequencers play)
ignore slides while the sequence runs, as they ignore other notes, and so do the slices the
slice instrument's own pattern plays. Where keys pick a sound rather than a pitch (the scratch
sampler's moves, the spline scratcher, the membrane kit, the membrane drum's reference engine,
the turntable) a slide leaves the sound as it is.

An instrument that cannot glide one note on its own bends its whole sound instead, with the
channel's pitch bend, so the slide is still heard. Instruments that read pitch bend themselves
get it once, never twice.

## Plugins

The **Processing** tab of a plugin's window has a **Per-note pitch** choice:

- **Automatic**: CLAP and VST3 plugins get each note's own tuning (note expressions). Other
  plugins get the channel's pitch bend, within the bend range the plugin is set to (2 semitones
  is assumed).
- **MPE**: each note plays on a MIDI channel of its own (channels 2 to 16, the lower zone), and
  a slide or bend line moves that channel's pitch bend. Gridline tells the plugin the zone and
  the **MPE bend range** (2, 12, 24, 48 or 96 semitones; 48 is the MPE standard) when it starts,
  but set the same range in the plugin if it does not read it. Use this for MPE instruments.
- **Channel pitch bend**: the whole plugin is bent with the channel's pitch bend.
- **Off**: slides and bend lines leave the plugin's pitch alone.

## MIDI files

MIDI files have no slide notes. When a pattern is exported to a MIDI file, its slide notes are
written as pitch bends of the notes they move (the same ramp, from where the note was to the
slide's pitch over the slide's length), and the slide notes themselves are left out.
