# JACK and PipeWire on Linux

On Linux Gridline plays through the system audio by default (SDL2 over PipeWire, PulseAudio or
ALSA). You can also switch to the **JACK / PipeWire** driver in **Settings > Audio** to expose
named audio input and output ports directly in the audio graph, so other applications can be
connected into Gridline's audio tracks and tracks can be routed out to other applications.

The JACK / PipeWire driver is opt-in: the system audio remains the default and stays available if
no JACK or PipeWire server is running.

## Requirements

Gridline loads `libjack.so.0` at runtime and does not require JACK development packages. On modern
Linux distributions running PipeWire, install the distribution's PipeWire JACK package:

| Distribution | Package |
|---|---|
| Debian / Ubuntu | `sudo apt install pipewire-jack` |
| Fedora | `sudo dnf install pipewire-jack-audio-connection-kit` |
| Arch Linux | `sudo pacman -S pipewire-jack` |

On Debian and Ubuntu, `pipewire-jack` installs its library in
`/usr/lib/<arch>-linux-gnu/pipewire-0.3/jack/libjack.so.0`. Gridline checks that directory
automatically when PipeWire is running, so you do not need to launch Gridline through `pw-jack`.
If you run a traditional JACK2 or JACK1 server (`jackd`), Gridline connects to it through the
system `libjack.so.0`.

## Choosing JACK / PipeWire

1. Open **Settings > Audio**.
2. Set **Driver type** to **JACK / PipeWire**. The change applies immediately and asks you to keep
   or revert it.
3. Under **JACK / PIPEWIRE PORTS**, choose how many named input ports (0, 2, 4, 8 or 16; default 2)
   and extra stereo output pairs (0 to 15; default 0) Gridline registers.

If no server is running or `libjack.so.0` is not installed, Gridline shows the exact reason in
**Settings > Audio** and falls back cleanly to the system audio.

## Ports, aliases and track input menus

When active, Gridline registers one client named **Gridline** (or `Gridline-01` if a second copy is
open):

- **Inputs:** `In 1`, `In 2`, ..., up to `In 16`. Each port matches the numbering used in every
  audio track's input menu (`In 1 - In 2` is the stereo pair of `In 1` and `In 2`). When a mixer
  track uses an input port, Gridline updates that port's alias and pretty name to include the track
  name (for example, `In 1 (Vocal)`).
- **Main outputs:** `Out 1` (`Main L`) and `Out 2` (`Main R`).
- **Extra output pairs:** `Out 3` and up (`Pair 1 L`, `Pair 1 R`, ...). Hardware Insert sends and
  Voltage Out write to these pairs directly, and you can also assign any mixer track (post-fader)
  to a pair under **Extra outputs**.

Audio track input menus list what each input port is
currently connected to (or `not connected` when nothing is patched to it).

## Connecting ports

You can wire ports in either of two ways:

- **In a PipeWire or JACK patchbay:** connect any other application's outputs to `Gridline:In 1`
  and `Gridline:In 2`, and Gridline's outputs to your speakers or another application.
- **Inside Gridline:** open **Settings > Audio > JACK / PIPEWIRE PORTS** and click **Connect...**
  next to any input or output port to connect or disconnect external audio ports without leaving
  Gridline.

Auto-connect switches apply only to projects that have no saved connections of their own:

- **Connect outputs to the speakers** (default on): connects `Out 1` and `Out 2` to the first two
  physical playback ports.
- **Connect inputs to the system inputs** (default off): kept off by default to avoid microphone
  feedback.

## Saving and restoring connections

With **Restore connections with the project** on (the default), Gridline snapshots the connections
of its ports when you save a project. When you reopen the project, Gridline reconnects every saved
link whose target port is present, and also reconnects automatically if the other application is
started after Gridline. Turn this option off if you use a separate session manager or patchbay that
manages connections for you.

Port connections belong to each computer and are never shared in collaborative projects.

## Sample rate, buffer size and latency

- **Sample rate:** Gridline follows the server's sample rate. If the server changes rate while
  running, Gridline reopens its client at the new rate and restores its port connections.
- **Buffer size:** **Server default** follows the graph's period size (PipeWire quantum) without
  modifying server settings. Choosing an explicit size requests that period from PipeWire (via
  `PIPEWIRE_LATENCY`, unless already set in your environment) or sets it on JACK. Dynamic quantum
  changes (such as `PIPEWIRE_QUANTUM` or another client requesting a smaller period) take effect
  without reopening or reallocating.
- **Latency and recording offset:** Gridline reports plugin delay compensation on its playback
  ports and uses the server's reported capture and playback latencies as the automatic recording
  offset. For network audio sources whose transport delay is unknown to the audio server, adjust
  the manual offset in **Settings > Audio > Recording offset** (or run **Measure...**).

## When the server stops

If the JACK or PipeWire server stops or restarts while Gridline is running, Gridline notifies you
and falls back to the system audio so playback can continue. With **Return when the server is
back** on (the default), Gridline checks in the background and automatically switches back to JACK
and restores your connections once the server is available again. You can also click **Reconnect**
at any time.

## Advanced settings

- `audio.jack.clientName`: client name registered with the server (default `Gridline`).
- `audio.jack.library`: explicit path to a `libjack.so.0` library file, overriding automatic
  detection.

## For developers

Set `GRIDLINE_FAKE_JACK=1` on any platform (including macOS) to run Gridline against an in-process
simulated JACK / PipeWire server with physical ports and a built-in `Tone` test peer client, so
the UI, port patching, input menus and recording flow can be tested without a Linux audio daemon.
