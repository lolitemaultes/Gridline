# Right-click menus

Gridline's right-click menus are short enough to fit on a 1440x900 screen without scrolling. Each
section of a menu keeps its most used commands at the top level; the rest of the section sits in a
submenu named after it. Every command keeps its keyboard shortcut, and the command palette
(**View > Command palette...**) still finds everything in the menu bar's menus.

Commands that can never apply to what you clicked are left out instead of greyed: the sample
commands on a channel that plays no sample, **Disarm selected tracks** while nothing is armed, and
only the one of **Make an instrument track for it** / **Drop its instrument track link** (and the
mixer's **Make an audio track for it** / **Drop its audio track link**) that applies. Commands that
apply but cannot run right now, such as **Paste** with an empty clipboard, stay greyed.

## Where things are

| Menu | Submenu | What it holds |
|---|---|---|
| Channel (right-click a channel's name) | **Fill** | Every step, every 2, 4 or 8 steps, the offbeat fills, Advanced fill |
| | **Steps** | Shift, reverse, invert, randomize, insert or delete a step, repeat the first bar, bake note effects, steps to piano roll and back, step loop, truncate swung steps, reset step parameters |
| | **Instrument and routing** | Instrument track link, Freeze, Hibernate, render to a sampled instrument, export the sample, open the sample in a new channel, key range, layer zones |
| | **Properties** | Random colour, icon, cut itself, zip, a layer's channels |
| Channel rack menu | **Selected channels** | New patterns from the selection, layers, swing mix, declicking, step loops, truncated swing, cut itself, Freeze, Hibernate, their mixer tracks |
| | **All channels** | Audio clip channels, the pattern's automation clips |
| Mixer track | **File** | Open and save the track state, insert a track from a state file |
| | **Stereo and polarity** | Mono or stereo, swap left and right, reverse polarity, stereo separation |
| | **Routing** | Channel routing, track routing, select related tracks, audio track link, unroute, reset routing |
| | **Group** | Create, add to, remove from and colour a group |
| | **CPU** | Freeze, Hibernate, threaded processing, track latency |
| Playlist track | **Properties** | Random colour, icon, name after its first clip, size, reset name, colour and icon |
| | **Group** | Hold the next tracks, group with the track above, colour, collapse, fold, ungroup |
| | **Clips** | Send the current pattern's clips here, merge, mute or unmute all clips |
| Audio clip | **Audio** | Follow the project tempo, fit to tempo, normalize, make unique as sample, pitch a semitone up or down |
| Tools (menu bar) | **Composing** | Piano roll selection, the last piano roll script, the note clipboard as a MIDI file, project scripts |
| | **Macros** | Route every channel to its own track, select unused channels, lock or unlock every track, prepare the live area, audio clip channels, declicking |
| | **Freeze and hibernate** | Unfreeze all, hibernate unused plugins, wake all |
| | **Clean up** | Trim unused mixer tracks, delete unused automation clips and patterns |
