# Gridline on a Chromebook

Gridline runs on Chromebooks in ChromeOS's **Linux development environment**, the Debian system that
ChromeOS can run beside your apps. It is the same Linux build as on any other computer, on both
kinds of Chromebook:

| Chromebook | Processor | Package |
|---|---|---|
| Most Chromebooks and Chromebook Plus models | Intel or AMD | `Gridline-<version>-linux-amd64.deb` |
| Chromebooks with an ARM processor | MediaTek, Qualcomm | `Gridline-<version>-linux-arm64.deb` |

Every release is installed and started on Google's own Chromebook Linux images (the current one
and the older Debian 11 one), for both kinds, before it is published.

## 1. Turn on Linux

Open **Settings**, **About ChromeOS**, **Developers**, and next to **Linux development environment**
choose **Set up**. Give it at least 10 GB of disk; a few projects with samples need more. When it
is done a Terminal window opens.

ChromeOS sets up Debian 12 today; Chromebooks whose Linux was set up earlier may still have
Debian 11. Gridline installs on both. To check which one you have, type this in the Terminal:

    cat /etc/debian_version

On Debian 11, apt may need the archive for security updates first: see
[Debian 11](debian.md#debian-11) in the Debian guide. ChromeOS also offers an upgrade to Debian 12
under **Settings**, **About ChromeOS**, **Developers**, **Linux development environment**; back up
your Linux files first.

## 2. Install Gridline

Find out which package you need:

    dpkg --print-architecture

It prints `amd64` or `arm64`. Download the `.deb` with that word in its name from the
[latest release](https://github.com/lolitemaultes/Gridline/releases/latest), then move it from
**Downloads** to **Linux files** in the Files app. In the Terminal:

    sudo apt update
    sudo apt install ./Gridline-<version>-linux-amd64.deb

(with your version and `arm64` on an ARM Chromebook). Gridline then appears in the launcher, in
the **Linux apps** folder. To remove it: `sudo apt remove gridline`.

The AppImage works too. ChromeOS does not include the library AppImages use to mount themselves, so
either install it once with `sudo apt install libfuse2`, or start the AppImage with
`--appimage-extract-and-run`.

## Sound

Linux apps play through ChromeOS's own sound system. On a Chromebook a new Gridline installation
starts at **48000 Hz** with a **1024 frame** buffer (about 21 ms), the rate and buffer the
Chromebook's Linux sound system runs at. Smaller buffers do not lower the delay there and can
cause crackles. You can still change both in **Settings**, **Audio**.

To record, allow the microphone for Linux in **Settings**, **About ChromeOS**, **Developers**,
**Linux development environment** (**Allow Linux to access your microphone**). Bluetooth
headphones add a noticeable delay; use the speakers, wired headphones or a USB audio device for
playing live.

## Files

Linux apps see only **Linux files** at first. To open samples or projects from elsewhere (your
Downloads folder, Google Drive, a USB drive), right-click the folder in the Files app and choose
**Share with Linux**. Shared folders appear under `/mnt/chromeos` in Linux; in Gridline's file
dialogs the **Shared folders** button next to the home button goes there.

## MIDI keyboards and controllers

Connect the device, then in **Settings**, **About ChromeOS**, **Developers**, **Linux development
environment**, **Manage USB devices** turn it on for Linux, and choose **Rescan devices** in
Gridline's **Settings**, **MIDI**. Not every Chromebook passes MIDI devices to Linux; if yours does
not, the computer keyboard plays notes too (the typing keyboard).

## Plugins

Gridline hosts Linux VST3, CLAP, LV2 and VST plugins built for your Chromebook's processor: x86_64
plugins on Intel and AMD Chromebooks, aarch64 plugins on ARM ones. Windows and macOS plugins do not
run in the Linux environment. Gridline's own instruments and effects work everywhere.

## Graphics

Gridline draws with OpenGL. It uses the Chromebook's graphics where ChromeOS offers them to Linux
and also runs on software graphics, which costs more processor time. If the interface feels slow,
make the window smaller or lower the interface scale in **Settings**, **Interface**.

## Googlebook and other Android laptops

Googlebooks and other laptops with Google's Android-based laptop system have a **Terminal** app
with a Debian Linux environment, which Google currently calls a preview. Gridline's Linux packages
install there the same way, and Gridline opens in the Terminal app's display screen. Sound from
Linux apps is not dependable there yet, and USB devices (MIDI keyboards, audio interfaces) do not
reach Linux, so Gridline is not supported there for now. It recognises that environment, points
its file dialogs at the device's shared storage (`/mnt/shared`) and uses the same sound settings as
on a Chromebook.

## Troubleshooting

* Start Gridline from the Terminal with `gridline-app` to see its messages. The first lines say
  which system it recognised (`[system]`), the window system (`[video]`) and the graphics
  (`[graphics]`).
* `GRIDLINE_CHROMEOS=off gridline-app` starts Gridline without the Chromebook defaults and hints.
* No sound at all: check that the Chromebook's own volume is up and that another Linux app plays.
  Restarting the Linux environment (right-click the Terminal in the shelf, **Shut down Linux**)
  helps after the Chromebook has slept.
