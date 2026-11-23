# Render to audio

Any part of a song that plays from notes can become audio: one channel's steps and notes in a pattern,
several rows of the channel rack, a whole pattern, one clip in the playlist, or a few notes picked in
the piano roll. The render is sample for sample what that part plays in the song.

## Where to find it

**Render to audio** is a submenu in:

- a channel's menu in the channel rack (the rack selection when the channel is part of it, else the
  channel alone), with **The whole pattern (every channel)...** at the end;
- a pattern clip's menu in the playlist, in every layout (in Smooth: a region's **Render** submenu);
- the piano roll's Edit menu, as **Render selected notes** (no notes selected: all of the channel's
  notes in the pattern);
- an instrument's menu in the Clips layout's device band, the track menu in Smooth, and the instrument
  list and song order menus in Tracker.

The submenu names what it renders, then lists quick destinations that use your last options, and
**Render options...**, which opens the render sheet.

## Destinations

| Destination | What you get |
| --- | --- |
| Audio clip (Smooth: audio region, Tracker: sample in the song) | A new channel playing the render, on a free track where the source plays. |
| Replace | A clip becomes an audio clip in its place; a pattern's clips all become audio; rows and notes give way to one sampler note. |
| Slicer channel | A Kerf channel with one slice per hit and notes that play the slices: in the same pattern for rows and notes, or in a new pattern placed where a clip or pattern plays. At most 64 slices. |
| One-shot sampler channel | A new sampler channel below, trimmed to the sound, with the first note's key as its root key. |
| Drum pad | The next free pad of the drum pad channel takes the render. |
| Loupe | The render opens in Loupe to edit (in Smooth, in the lower pane). |
| Library | Saved in the project's Rendered folder (shown in the browser), tagged as a loop or a one-shot (see below). |
| File | A WAV or FLAC file where you choose. |

Every destination that changes the song is one undo step. The notice that follows has **Undo**,
**Open in Loupe** and **Options...**.

## The render sheet

- **Source**: the part to render (the row, the selected rows, the whole pattern, the clip, the notes)
  and what it goes through: **Dry** (the instrument alone), **Channel effects** (with its note
  effects and echo, before the channel's volume and pan) or **Mixer track** (through its mixer tracks
  and their effects). Rows and notes default to Channel effects, patterns and clips to Mixer track.
  A live preview shows the waveform, the tail and where the slices will fall.
- **Length and tail**: the pattern, clip or notes once or several loops; the tail cut, left to ring
  (up to the longest tail you set) or wrapped to the start so a loop plays seamlessly; and one-shot
  trim (start at the first sound, end where it falls below -60 dB).
- **Slices and format**: slice at each step hit, each note, each beat or each transient; the sample
  rate and bit depth of a file you save; normalize the peak to -1 dB.
- **Mute the source after rendering**, and **Remember for the quick menu rows**, which makes these
  the options of the submenu's quick destinations.

Rendered files are kept in the project's own Rendered folder and are named after their content, so
rendering the same thing twice reuses the file. They are not added to your sound library.

### Loop or one-shot

A render to the library is tagged on its own: when it lasts a whole number of bars (to within a
millisecond) and ends on a bar line, it is a **loop**, which the browser plays in time with the song;
anything else (a tail left ringing, trimmed notes, a clip that starts off the bar line) is a
**one-shot**. With **Library** picked, the render sheet shows the tag as a chip under the options:
click **Loop** or **One-shot** to tag it the other way before you render. The quick menu row always
uses the automatic tag.

## Exporting a song in parts

The export window can write one file per mixer track (**Split mixer tracks**), per playlist track
(**Split playlist tracks**) or per track group. The parts are named with **Part names** (for
example `Song - 03 Snare`). With one kind of split on, the parts go in the folder you chose, next
to the full mix. With two or three kinds on, each kind goes in a folder of its own there (**Mixer
tracks**, **Playlist tracks**, **Track groups**; in Smooth **Channel strips** and **Tracks**, in
Clips **Tracks**), so a mixer track and a playlist track with the same name never replace each
other's file. A name that would still repeat in one folder (a part name pattern without the
number) gets ` 2`, ` 3` added.

## Export formats and loop files

The export window writes WAV (16 or 24 bit, or 32-bit float) and FLAC on its own. MP3, Ogg Vorbis,
AAC (.m4a) and Opus (.opus) are made by an encoder program installed on the computer (ffmpeg, or
lame, oggenc or opusenc), each with its own bitrate; the window tells you when a format's encoder
is missing. Opus files play back at 48 kHz.

With **Save tempo information (WAV)** on, a WAV carries the song's tempo, its length in beats and
its meter, so loop browsers and samplers play it in time. When the tail is set to **Wrap
remainder** (the tail wraps around to the start, so the file loops seamlessly), the WAV is also
marked as a loop and carries sampler loop points over the whole file, so a sampler that reads them
loops it as soon as it loads.

## In a collaboration session

The render runs on the computer of the person who asked for it. While it runs, the others see the
source's menu say **Rendering...** with that person's name. The result reaches everyone as one edit,
and the rendered file reaches each computer like recorded audio. Undo works for everyone.
