# Your sound library

## Libraries already on this computer

The first time Gridline starts after this update, it checks whether a few well-known library
folders exist: the system's loop folders and your own loop folder on macOS, the shared sample
folders on Windows, and the system sample and drum kit folders on Linux. It only checks that these
folders are there; it does not list or read anything in them. If it finds any, it asks once
whether to add them to the browser. Say **Add** and they appear among the browser's folders; say
**Cancel** and Gridline does not ask again. You can add or remove browser folders at any time.

Gridline never looks into your personal folders (documents, music, downloads, desktop) unless you
add them to the browser yourself.

## Tempo, key and sound kind from the audio

While the browser indexes your folders, it listens to every audio file whose name and loop
metadata say nothing about it, in the background (never while you work in the interface):

- **Tempo**: the beat of a loop, settled by its length when it lasts a whole number of bars.
  One-shots have none.
- **Key**: the key and scale of tonal material (chords, melodies, bass lines). Drums have none.
- **Sound kind**: kick, snare, hat, cymbal, perc, bass, vocal, drums (a beat), tonal or fx.

What the file name or the file's own loop data says always wins over what is heard. The results
are cached with the index, so a file is analysed once, and again only when it changes.

## Filtering

The sound kinds show up as tags (in the tag chips of the browser's filter rows, and for `tag:`),
and the search field takes these filters beside the ones in [loop-files.md](loop-files.md):

| Filter | Finds |
| --- | --- |
| `sound:kick` | files that sound like a kick (`sound:snare,hat` finds either) |
| `scale:minor` | files in any minor key (`scale:major` for major) |
| `beats:8` | files 8 beats long at their tempo (`beats:4-16` for a range) |
| `bpm:120-130` | tempo, heard or named |
| `key:Am` | key |
