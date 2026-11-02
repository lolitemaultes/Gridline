# ASIO on Windows

On Windows Gridline plays through the system audio by default (WASAPI, with DirectSound as a
fallback). Audio interfaces that come with an ASIO driver can also be used through that driver,
for lower latency and for every input and output of the interface. ASIO is optional: the system
audio stays available and stays the default.

## Choosing ASIO

1. Install the ASIO driver from your interface's maker (Gridline lists the drivers registered
   under `HKEY_LOCAL_MACHINE\SOFTWARE\ASIO`). A 64-bit Gridline needs a 64-bit driver.
2. Open **Settings > Audio** and set **Driver type** to **ASIO**.
3. Pick the driver under **ASIO driver**. The change applies at once and asks you to keep it or go
   back, as other audio device changes do.

| Setting | What it does |
|---|---|
| ASIO driver | Every installed driver. Switching drivers works while Gridline runs. |
| Control panel | Opens the driver's own settings window (buffer, clock source, routing, as the driver offers). |
| Reconnect | Opens the driver again, for example after plugging the interface back in. |
| Sample rate | The rates the device supports. Gridline's engine runs at the device's rate. |
| Buffer size | The sizes the driver allows (its minimum, maximum and step, for example 32 to 512). **Driver's preferred size** follows whatever the driver's control panel is set to. |
| Latency | Input and output latency as the driver reports it. |
| Inputs from | The first device input that becomes the mixer's **In 1**. **None** uses the system input device instead (Input & recording). |
| Input channels | How many device inputs Gridline opens (up to 16): they appear as In 1, In 2, ... in each mixer track's input selector. |
| Main output | The output pair the master plays on. |
| Extra outputs | Any other output pair can carry one mixer track (after its fader), for outboard effects or separate headphone mixes. |

## Recording offset

The input and output latency the driver reports is the automatic recording offset: recorded audio
is moved earlier by that much plus the plugin delay compensation, so takes line up with what you
heard. **Settings > Audio > Recording offset** adds a manual correction on top.

## When the driver asks for a reset

Drivers ask the host to restart when something changes in their control panel (buffer size,
sample rate, clock source) or when they lose sync. Gridline reopens the driver on its own and
keeps the new values: a buffer or rate set in the driver's panel replaces the one chosen in
Gridline. If the device stops delivering audio (unplugged, powered off), Gridline says so and
switches to the system audio; choose the driver again, or press **Reconnect**, once the device
is back.

## Sample formats

Gridline converts the common driver formats without extra latency: 16-, 24- and 32-bit integer,
32-bit integer with 16, 18, 20 or 24 significant bits, and 32- and 64-bit float. DSD formats are
not supported.

## Licence

The ASIO interface comes from Steinberg's ASIO SDK, which is dual licensed under the Steinberg
ASIO License and the GNU General Public License version 3 (ASIO SDK 2.3.4, October 2025). Gridline,
licensed under the GPL version 3 or later, uses it under the GPLv3 option. No SDK file is included:
`src/host/audio/AsioAbi.h` declares the driver interface in Gridline's own words, for
interoperability. ASIO is a trademark of Steinberg Media Technologies GmbH.

## For developers

`GRIDLINE_FAKE_ASIO=1` lists two simulated drivers on any platform (an 8 x 8 interface and a
2 x 2 one) so the backend and its settings can be tried without Windows or hardware. The simulated
inputs carry a test ramp, never a microphone. The code is in `src/host/audio/` (`AsioCore` sample
formats, buffer sizes and channels; `AsioHost` the driver session and the simulated driver;
`AsioWin` the registry and COM loading) and `src/app/AsioUi.cpp`; the tests are
`tests/test_asio.cpp` (every platform) and `tests/test_asio_win.cpp` (Windows).
