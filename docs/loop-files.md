# Loop files: REX2 and Apple Loops

Gridline opens sliced loops in the REX2 format (`.rx2`) and tagged loops in the Apple Loops
format (`.caf`), as well as the loop information that AIFF and WAV loops carry. They show up in
the browser with the rest of your samples, preview there, and drop anywhere a sample goes.

| File | Name ends in | What Gridline reads |
| --- | --- | --- |
| REX2 sliced loop | `.rx2` | the audio, its slices, its length in bars and beats, its own tempo |
| Older sliced loop | `.rex`, `.rcy` | the audio (they are AIFF files); their slices are not read yet |
| Apple Loops tagged loop | `.caf` | the audio (AAC, Apple Lossless or uncompressed), the beat count, time signature, key, category, genre, descriptors and beat markers |
| AIFF loop | `.aif`, `.aiff` | the beat count, time signature and key it states |
| WAV loop | `.wav` | the tempo, beat count and root note it states |

## In the browser

A loop file shows its own tempo and key next to its length, and a sliced loop also shows how
many slices it has. Its tags (for example `#rex`, `#apple loop`, `#loop` or `#one-shot`, and an
Apple Loop's category, genre and descriptors) appear with the automatic tags and are searchable,
so `tag:drums bpm:118-122 key:Am` finds what you expect. A loop file's own tempo wins over a
tempo in its name; a one-shot (an Apple Loop without a beat count) has no tempo at all. With
tempo sync on, the preview plays at the song's tempo.

## Dropping a loop

- **A REX2 loop** dropped on the channel rack becomes a Kerf channel cut at the file's own slices,
  with a new pattern that plays every slice on its beat. Dropped on the playlist, it also adds a
  clip of that pattern where you drop it. The loop then follows the song tempo the way sliced
  loops are meant to: each slice starts on its beat and plays at its own speed, with no
  stretching. Hold Ctrl while dropping it on the playlist to get a plain audio clip of the loop
  instead.
- **A loop that states its tempo** (an Apple Loop with a beat count, an AIFF or WAV loop) dropped
  on the playlist becomes an audio clip that follows the song tempo with its pitch kept. Dropped
  on the channel rack, it becomes a sampler set to follow the song tempo from the loop's own
  tempo. A notice names that tempo; undo once to keep the sample as it is.
- **A loop file dropped on a Kerf channel** replaces its sample and cuts it at the file's own
  slices (a REX2 loop's slices, an Apple Loop's beat markers).

Muted slices stay muted, and a slice that stops before the next one starts keeps its end. A
slicer channel plays one slice per key: up to 64 slices use Kerf's slice settings, up to 128
use its region engine, and a file with more has its shortest neighbouring slices joined until
it fits (a notice says how many).

## Projects

Projects refer to loop files by path, like any sample. Zipped projects take them along, and FL
Studio projects whose slicer channels use REX2 loops play them.

## Notes

- Gridline decodes REX2 audio itself, on every system, written from a public-domain description
  of the format. It does not use, and does not need, any other program's library.
- Apple Loops in AAC or Apple Lossless are decoded by the system on macOS and Windows (where
  Windows cannot open the CAF container, Gridline rebuilds the AAC stream for it), and by FFmpeg
  on Linux and the BSDs, the same way as `.m4a` files. Uncompressed CAF files need nothing else.
- REX and Apple Loops are trademarks of their owners; Gridline is not affiliated with them.
