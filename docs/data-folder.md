# Data folder

Gridline keeps its settings, caches and downloaded data in three folders that your system
chooses (on Linux `~/.config/gridline`, `~/.cache/gridline` and `~/.local/share/gridline`).
Settings > Files > Data folder moves all three under one folder you pick, for example on an
external drive:

    <folder>/config/gridline
    <folder>/cache/gridline
    <folder>/data/gridline

## Moving your data

1. Type the full path of the folder in "Move to".
2. Press "Copy my data there and use it". The copy shows its progress and can be cancelled.
   Your files are copied, not moved: the old folders stay as they were.
3. When the copy is done, press "Keep" to keep the new folder, or "Revert" to go on using
   the folders you use now.

The new folder is used from the next start. "Use the default folders" goes back to the
system's folders, also from the next start; the files in the chosen folder are left there.

Gridline remembers the choice in a small file at the default place,
`<default config folder>/gridline/data-folder.txt`, so it finds the folder before it reads any
setting.

## A missing folder

If the chosen folder is not there when Gridline starts (an unplugged drive, for example),
Gridline does not create it. It uses the default folders for that run and says so; the
choice stays, so the folder is used again once it is back.

## Portable mode

A folder named `GridlineData` beside the program (on macOS, beside `Gridline.app`) is used
without any setting, so Gridline can run from a stick with its own settings and data.

## Environment variables

`GRIDLINE_CONFIG_HOME` turns the data folder setting off for that run (tests and scripts use
it). `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` and `XDG_DATA_HOME` each keep their own folder.
