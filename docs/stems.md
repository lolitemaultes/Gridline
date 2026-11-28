# Split stems

Gridline can separate a recording into its parts: vocals, drums, bass and other (everything
else), or with the six-stem model also guitar and piano. The separation runs on your computer, in
the background, and nothing is sent anywhere.

## Where to find it

* **Playlist**: right-click an audio clip, or open the menu on its title strip, and choose
  **Split stems...** (Studio, Clips and Smooth).
* **Audio clip window**: **Edit audio... > Send and split > Split stems...**, also in the Clips band
  and in Smooth's audio editor.
* **Browser**: right-click an audio file and choose **Split stems...**. The results appear under
  **Stems** in the browser.
* **Loupe**: **File > Split stems...** splits the sound that is open (with your edits).

## What is split

Only the audio you chose is separated, so a short part takes far less time than the whole song:

* **A clip** splits the part of its file it plays: a clip you sliced, trimmed or cut out of a whole
  song with the split or slice tool splits that part, not the song. A looping clip splits its loop
  once.
* **A time selection** in the playlist (or the Marquee tool's area in Smooth) that covers only part
  of the clip splits the clip's part inside it.
* **The Audio clip window** splits its selection when there is one, as its other tools do.
* **Loupe** splits its selection when there is one, else the whole sound.
* **The browser** splits the whole file.

The window's first line names the part and where it lies in the file. The clip's sound is split as
you hear it from its channel: the channel's start and end handles, precomputed effects and sample
processing (normalize, fades, polarity and the like) are in the stems, so the stem channels play
their files as they are. The clip's own settings (stretch, pitch, gain, fades, reverse, warp
markers) stay on the stem clips, which play the stems exactly as the clip plays its part.

## The window

| Part | What it does |
|---|---|
| Model | **Standard** (vocals, drums, bass, other; the fastest), **High quality** (one tuned model per stem, about four times slower, a little cleaner) or **Six stems** (adds guitar and piano; piano is rough). Each shows whether it is downloaded or how much there is to download. |
| Download | Shown while the chosen model is not on this computer: the licence note, **Download** with a progress bar and Cancel, and **Import model file...** for a copy you already have. |
| Stems | Tick the stems to keep. |
| Result | For a clip: **New tracks under the clip, lined up with it** or **Files only, in the Stems folder**, and **Mute the original clip**. For part of a clip: **lined up with the part** and **Mute that part of the original clip**, which cuts the clip at the part's edges and mutes the piece between them. |
| Split | Starts the split. The window shows the progress and the time left; **Cancel** (or Esc) stops it. You can keep working while it runs. |

New tracks are inserted right under the clip's track, one per stem (vocals, drums, bass, other,
guitar, piano), named and coloured after the stem. Each stem clip sits exactly where the part is,
with the same length, stretch, pitch, gain, fades, reverse setting and warp markers, and plays on
its own new channel copied from the clip's channel; its sample offset points at the same audio in
the stem file, so the stems play sample for sample in time with the clip. The whole split is one
undo step, and in a collaboration session the stems, their files and the undo reach everyone. When
the playlist already has its maximum number of tracks, the stems go on empty tracks further down.

The stem files hold the part only, with a few milliseconds of the file around it that playback
reads between samples. They are 32-bit float WAV files at the original sample rate, named
`<name> - Vocals.wav` and so on, in the `Stems` folder next to the project (for a project that is
not saved yet, in Gridline's settings folder). Existing files are never overwritten.

## Models

| Model | Stems | Download | Speed |
|---|---|---|---|
| Standard (`htdemucs`) | vocals, drums, bass, other | 84 MB | about the length of the audio or less on an 8-core computer (3 minutes: 2.5 to 3 minutes on a busy 8-core Intel i7) |
| High quality (`htdemucs_ft`) | vocals, drums, bass, other | 337 MB (4 files) | about four times longer than Standard |
| Six stems (`htdemucs_6s`) | adds guitar, piano | 55 MB | like Standard |

The models are the Hybrid Transformer Demucs models published by Meta AI Research. Gridline runs
them with its own C++ code (no Python, no extra libraries); its output matches the reference
implementation to within rounding.

**Licence.** The Demucs code is MIT-licensed. The trained model files are provided by their
authors for research use and are not part of Gridline. Gridline downloads them only when you ask,
from their official address (`https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/`), checks
each file against its SHA-256 checksum and keeps them in Gridline's data folder:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\gridline\stem-models` |
| macOS, Linux, BSD | `~/.local/share/gridline/stem-models` (or `$XDG_DATA_HOME/gridline/stem-models`) |

`GRIDLINE_STEM_MODELS` sets another folder. A download that is interrupted keeps the files that
finished and continues with the rest next time. On a computer without internet access, copy the
`.th` files there, or use **Import model file...**, which recognises a file by its size and
checksum. Remove a model with a right-click on it in the window, with `gridline stems --remove <model>`, or by
deleting its files.

## Command line

    gridline stems <audio> [--model htdemucs|htdemucs_ft|htdemucs_6s] [--out <dir>] [--stems vocals,drums]
                           [--name <base name>] [--threads <n>] [--download] [--from <seconds>] [--to <seconds>]
    gridline stems --list               # models, installed or not, and the models folder
    gridline stems --download <model>   # download a model (shows the licence note first)
    gridline stems --import <file>      # install a model file you already have
    gridline stems --remove <model>

`--from` and `--to` split only that part of the file (either one alone: from that point to the end,
or from the start to that point). Without `--out` the stems are written into a folder `<input name> stems` next to the input. `stems` never downloads unless you pass
`--download`; it reports a missing model instead. The written files are printed one per line
(`<stem>\t<path>`).

## Notes

* Separation works best on full mixes of pop, rock, electronic and hip-hop music. Unusual
  instruments may land in the wrong stem or in "other", and quiet parts can leak between stems.
* Mono files give mono stems; any sample rate works (the model runs at 44.1 kHz and the stems are
  converted back).
* Memory: about 2 to 3 GB while a split runs.
