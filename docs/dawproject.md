# DAWproject files

DAWproject (`.dawproject`) is an open file format for moving a song between music programs.
Gridline reads and writes it.

- **Export:** **File > Export > DAWproject (.dawproject)...**
- **Import:** **File > Import > DAWproject file...**, or open or drop a `.dawproject` file. It
  opens as a new project (Gridline asks first when the open project has unsaved changes).

After either, Gridline lists what it could not carry over exactly.

## What is carried

| Gridline | DAWproject |
| --- | --- |
| Tempo, meter, title, artist, genre, comments | Transport and metadata |
| Each channel: its instrument, note effects, volume, pan and mute | An instrument track |
| Each mixer track in use: its effects, volume, pan, mute, solo, routing and sends | An audio track (one fed by sends becomes an effect track) |
| Each pattern clip in the arrangement | A note clip on the track of each channel that plays in it, with its start, length, offset, loop and mute |
| Notes, with velocity, release velocity and their pitch bend, pressure and timbre lines | Notes with per-note pitch, pressure and timbre |
| Channel rack steps | Notes |
| Audio clips, with their start in the file and fades | Audio clips; the files go into the archive |
| Volume, pan and tempo automation | Automation points |
| Markers | Markers |

Gridline's own instruments and effects are stored with their full settings, so a file Gridline
wrote opens in Gridline exactly. Other programs see their names and use their own devices
instead. Installed plugins (VST3, CLAP, VST2, Audio Units) are named by their plugin ID, so another
program loads the same plugin, starting from its default sound.

On import, tracks that others play into, group tracks and effect tracks become mixer tracks;
the other tracks become channels. Clips that sit together on several tracks and share a name
become one pattern holding every track's notes, and identical clips share their pattern. A track
with its own effects gets a mixer track of its own. The audio files are copied into Gridline's
`Imported audio` folder.

## What is not carried

These are left out or simplified, and the list after an export or import says which ones
the song had:

- slide and portamento notes (plain notes), notes' pan, fine pitch and mod X / Y, muted notes;
- automation of other parameters, automation inside patterns, curve tensions (straight lines);
- audio clips' stretch, pitch, reverse, warp markers and gain;
- channel pitch, the song's swing, the Stage, sidechain routes, effect mix amounts;
- the live area, other arrangements, meter and key markers (plain markers);
- on import: scenes, other programs' device settings, notes' MIDI channels, and effects past the
  tenth on a track.
