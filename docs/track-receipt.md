# Track Receipt: songs and marker areas

**Help > Track Receipt...** lists the parts of original recordings a project uses, one entry per
original song, so you can license each sample. This page explains how files are grouped into
songs and how the receipt splits the arrangement at its markers.

## One song, many files

Files named like sample store downloads (`Artist-Title-Stem--120.00bpm-Am-[Clearance Required].wav`)
are grouped by their artist and title, as before.

Other files often carry the song inside other words, for example
`VOCALS_86BPM_GMinor_VBV3_bless_ya_life.wav`. Gridline reads the song from the name by leaving out
the words that describe the file rather than the song:

| Left out | Examples |
| --- | --- |
| Tempo | `86BPM`, `98.00bpm`, `bpm 98` |
| Key | `GMinor`, `Dmaj`, `C#min`, `G minor`, `C sharp minor`; short keys such as `Bm` or `F#` only next to a tempo or the word `key` |
| Stem or section | `vocals`, `vox`, `acapella`, `drums`, `bass`, `keys`, `master`, `instrumental`, `chorus`, `hook`, ... |
| Take or version | `take 2`, `v3`, `alt` |
| Part number | `part 2`, `pt1` (the part gets its own timeline, as `Part_2` does) |
| Store note | `clearance required`, `royalty free`, `copy` |
| Pack code | up to four letters and up to three digits: `VBV3`, `SP404` |

The words that are left, in their order, are the song. Files whose songs read the same are counted
as one song: the same part taken from two stems counts once. Everything else keeps them apart, so
only identical song names with different extra words are grouped:

- `VOCALS_86BPM_GMinor_VBV3_bless_ya_life.wav` and `DRUMS_86BPM_GMinor_VBV3_bless_ya_life.wav` are
  one song, **bless ya life**.
- `VOCALS_98BPM_DMajor_VBV3_mike_ransom_world_wide_master__98.00bpm_bm__clearance_required_.wav` is
  the song **mike ransom world wide**: a different song, listed on its own.
- `Bless Ya Life 2` and `Bless Ya Life`, `Some Song Remix` and `Some Song`, `01 Morning Light` and
  `02 Morning Light` stay apart: numbers and words such as remix are part of the song.

When fewer than two words are left (and no single word of five letters or more), the file has no
song name of its own and stays on its own, named by the whole file name. A name that says
`clearance required` in any spelling is ticked for you.

Point at a group's note on the files page to see the song name of each file and the words that were
left out.

### Fixing a group by hand

On the files page:

- **Group... > Count with** counts this song with another one (for two names of one song that
  Gridline could not match).
- **Group... > Undo merges into this song** separates the songs counted with it again.
- **Group... > Keep these files apart** makes each file of the group a song of its own.
- **Song...** on a file links that one file to a song, keeps it **On its own**, or sets it back to
  **Automatic**.

Each change is one undo step, is saved in the project and reaches everyone in a collaboration
session. Fields typed in projects from before this version (category, share, fee, rights holders)
stay with their song.

## By marker area

When the arrangement has markers, the results also show **By marker area**: each area runs from a
marker to the next one (the last to the song end), and a stretch before the first marker is an area
of its own. For each area the receipt lists every original song used there with:

- **Seconds used**: the seconds of the original used inside the area, counted as if the area were a
  song of its own (loops and repeats count once, the same part from several stems counts once), and
  the tier to choose for it.
- **Sounds for**: how long the song plays in the area.
- the files of that song used in the area, each with its own seconds.

A sample used in two areas counts in each, so three songs made in one project and separated by
markers each get their own figures. Reads that sound past the song end count in the last area.
When Gridline cannot tell exactly when a sample plays (inside an installed plugin, or parts you
typed by hand), it is counted in every area it may play in and marked so.

Without markers the receipt is one section for the whole song, as before. The areas are also in the
saved spreadsheet (an **Areas** sheet) and in the results PDF.
