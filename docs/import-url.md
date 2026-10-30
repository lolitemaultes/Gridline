# Import from URL

**File > Import > Import from URL...** (Ctrl+Shift+U) brings the audio of a web page (a video or
music page on most sites, or a playlist) into the project as a lossless WAV file.

Import only media you have the rights to use. Gridline does not sign in to sites, does not read
your browser's cookies and does not get around copy protection (DRM): such links stop with a
clear message.

## Steps

1. **Paste the link.** The field is focused when the popup opens; if the clipboard holds a link it
   is filled in already. **Paste** takes the link from the clipboard.
2. **Fetch** reads the link: title, uploader or site, length, and for a playlist its items.
3. Choose where the audio goes:
   * **Clip on a new track at the playhead** (the default): a sampler channel plus an audio clip
     on the first free playlist track, at the playhead.
   * **Browser only**: the file is saved and listed, nothing is placed.
4. **Only part** keeps a range (minutes:seconds, like `1:30` to `2:05`); the rest is never
   written.
5. Playlists: the first item, or **chosen items** (with All / None).
6. **Import** downloads the best audio the site offers and converts it once, without loss, to a
   32-bit float WAV at the source's sample rate (levels above 0 dBFS are kept). All placed clips
   are one undo step.

The popup shows the progress of each step (reading the link, downloading the best audio,
converting to WAV) with the sizes and the item number of a playlist. **Cancel** stops at once:
the programs Gridline started are ended, together with any programs they started, and the
unfinished files are deleted. A step that hangs is stopped after a time limit (reading the link:
90 seconds; downloading or converting: two minutes without progress).

## Where the files go

* A saved project: `<project folder>/Imported/`.
* A project that was never saved: the `Imported` folder in Gridline's settings folder.

The browser lists the folder under **Imported**. Each file gets a `<file>.source.json` beside it
with the title, uploader, length, the link and the date; the clip's properties show them (with
**Copy link**). File names come from the title, made safe for every system; a name that is taken
gets a number.

## The programs it runs

Gridline runs two separate programs for this:
[yt-dlp](https://github.com/yt-dlp/yt-dlp) (reads the page, downloads the audio; public domain)
and [FFmpeg](https://ffmpeg.org) (converts it; LGPL). Gridline uses the first copy it finds:

1. The copy that came with Gridline: the release packages carry both, in
   `Gridline.app/Contents/Resources/tools` (macOS), `tools\` beside `gridline-app.exe` (Windows)
   and `share/gridline/tools` (Linux AppImage, `.deb`, `.rpm` and `.tar.gz`). Their licences and
   sources are listed in `tools/LICENSES` and `tools/SOURCES.txt`.
2. The copy Gridline installed (in `gridline/tools` in the system's application data folder).
3. The system's copy (on the `PATH`, or where Homebrew, pkgsrc, WinGet, Scoop or Chocolatey put it).

yt-dlp prefers the copy Gridline installed over the one that came with it, so updates win. With
the environment variables `GRIDLINE_YTDLP` and `GRIDLINE_FFMPEG` a program of your own is used
instead (nothing else is tried).

### First use without the programs

Builds without them (a self-built Gridline, the BSD packages, a package with the folder removed)
set them up the first time the popup is used: it shows "Setting up the downloader (once)..." with
progress and then goes on with the import.

* yt-dlp comes from yt-dlp's own newest release on GitHub and is checked against that release's
  `SHA2-256SUMS` and by running it.
* FFmpeg comes from Gridline's own releases (`Gridline-<version>-media-tools-<system>.zip`, built
  by the release workflow from the official source) and is checked against the release's
  `SHA256SUMS`.

Nothing is used that does not match its checksum. On FreeBSD, OpenBSD and NetBSD (no ready-made
programs for these systems) Gridline uses the system's yt-dlp and ffmpeg; when they are missing
the popup names the packages to install (for example `pkg install yt-dlp ffmpeg`).

### Updates

Sites change often, so yt-dlp is kept up to date: when the popup is used, Gridline checks in the
background at most once a day for a newer yt-dlp release, checks it the same way, and only then
replaces its copy in one step (the previous copy is kept and used again if anything fails). The
copy that came with Gridline is never changed; the update goes into Gridline's own folder.
**Settings > File > Import from URL** turns this off ("Keep yt-dlp up to date automatically"),
shows which copy of each program is used, and has **Update yt-dlp now**. A copy given by
`GRIDLINE_YTDLP` or the system's copy is never updated by Gridline.

## For packagers

`tools/release/fetch-media-tools.py --target <target> --out <dir> [--zip <file>]` makes the
`tools` folder: it downloads yt-dlp's release binary and FFmpeg's source tarball, checks both
against `tools/release/media-tools.json`, builds `ffmpeg` and `ffprobe` audio only without GPL or
nonfree parts (for Windows with the pinned llvm-mingw toolchain), and writes the licences and
`SOURCES.txt`. CMake installs such a folder with `-DGRIDLINE_MEDIA_TOOLS_DIR=<dir>`; the macOS
disk image takes it from `GRIDLINE_MEDIA_TOOLS` (`tools/release/package-macos.sh`). It runs only
in the release workflow; normal CI and builds never download anything.
