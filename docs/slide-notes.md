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

## Instruments

Every built-in instrument follows slides note by note: synths, samplers, the multisampler,
keys, plucks and strings, the granular and spectral instruments, and drums whose sound has a
pitch: the kicks (the sweep kick and the sculpted kick too, so a long kick glides like an 808),
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
