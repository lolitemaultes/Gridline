# Gridline on Debian and Ubuntu

Gridline's Linux `.deb` installs with apt on these systems:

| System | Versions |
|---|---|
| Debian | 11 (bullseye), 12 (bookworm), 13 (trixie) |
| Ubuntu | 22.04 and later |
| Chromebooks | ChromeOS's Linux development environment, Debian 11 or 12 ([docs/chromeos.md](chromeos.md)) |

There is one package per processor, for every system in the table:

| Processor | Package |
|---|---|
| Intel or AMD (x86_64) | `Gridline-<version>-linux-amd64.deb` |
| ARM (aarch64) | `Gridline-<version>-linux-arm64.deb` |

`dpkg --print-architecture` prints `amd64` or `arm64`. Every release is installed and started on
Debian 11, 12 and 13 and on Ubuntu 22.04 and 24.04, for both processors, before it is published.

## Install

Download the `.deb` from the [latest release](https://github.com/lolitemaultes/Gridline/releases/latest),
then in a terminal, in the folder you saved it to:

    sudo apt update
    sudo apt install ./Gridline-<version>-linux-amd64.deb

(with your version, and `arm64` on an ARM computer). The `./` matters: it tells apt to install
that file and to fetch what it needs (SDL2, ALSA, JACK's library) from your system's own
repositories. Gridline then appears in your applications menu; `gridline-app` starts it from a
terminal and `gridline` is the command line tool.

The package also installs Gridline as a VST3 and CLAP plugin for other programs, and the Gridline
Suite, in `/usr/lib/vst3` and `/usr/lib/clap`.

To update, install the newer `.deb` the same way. To remove Gridline:

    sudo apt remove gridline

Your projects and settings (`~/.config/gridline`) stay.

## Debian 11

Debian 11 is past the end of its long-term support, and its security updates have moved from the
usual mirrors to `archive.debian.org`. If `sudo apt install` stops with "404 Not Found" for a
`bullseye-security` file, change the `bullseye-security` line in `/etc/apt/sources.list` to:

    deb http://archive.debian.org/debian-security bullseye-security main

then run `sudo apt update` and the install again. Upgrading to Debian 12 or 13 is the better fix
when you can.

## The AppImage and the .tar.gz

The AppImage and the `.tar.gz` from the same release run on the same systems. The AppImage needs
FUSE 2 (`sudo apt install libfuse2`, or `libfuse2t64` on Debian 13 and Ubuntu 24.04), or start it
with `--appimage-extract-and-run`. The `.tar.gz` needs SDL2 and ALSA from the system
(`sudo apt install libsdl2-2.0-0 libasound2`, or `libasound2t64` on Debian 13 and Ubuntu 24.04); its
`install.sh` adds Gridline to your menus.

## Troubleshooting

* Start Gridline from a terminal with `gridline-app` to see its messages. The first lines say
  which window system (`[video]`) and graphics (`[graphics]`) it uses.
