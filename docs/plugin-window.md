# Plugin windows: the channel strip

A Gridline instrument opens in its channel window (`src/app/ChannelSettings.cpp`): a header row
(colour, name, instrument, mixer track), the channel strip, then the Plugin / Envelope / Misc
tabs with the instrument's editor. The channel strip holds the channel's volume, pan and pitch
knobs and a two-octave preview keyboard.

## Showing and hiding

- Hidden by default. The eye button at the right end of the header row shows and hides it. It is
  the mixer's eye (`ui::eyeIcon`, drawn by `ui::IconButton` exactly as `ui::EyeToggle` does),
  with its own tooltip: "Show the channel strip" / "Hide the channel strip".
- It never shows by itself. Every time a channel window opens it opens with the strip hidden:
  after a restart, when a project opens, when the window is closed and opened again, and when it
  is switched to another channel. Only a click on the eye shows it, for that window while it
  stays open. The choice is kept in memory only (`StripState` in `ChannelSettings.cpp`) and never
  saved: there is no setting for it. The old Settings key `ui.channelStrip` is dropped when an
  older settings file is read (`Settings::load`), so such files still load.
- Showing slides the strip down from under the header, hiding slides it back up: about 140 ms,
  cubic ease-out (`chstrip::Reveal` in `src/app/ChannelStrip.h`, timed by `ImGui::GetTime()`, so
  `--fixed-dt` makes it frame-exact). Reversing mid-way slides back from where it is. The strip
  is drawn in a clipped child area whose height is the shown fraction of the strip's full height
  (its measured content plus the gaps around it); the content is offset by the part not shown
  yet, so it slides rather than unrolls. Everything under the strip moves with the area's edge.
  With Interface animations off (`ui.animations`) or the system's reduced-motion preference, it
  switches at once.

## Window size

The channel window is auto-sized: its width is fixed by the instrument's editor width and its
height follows its content (up to the work area, then it scrolls). Hidden, the window is the
strip's height shorter; the minimum and default widths do not change. While the strip slides,
the window follows the content a frame later, docked inside the main window or popped out into
its own OS window (`src/app/Viewports.cpp` resizes that window like any auto-sized one).

Installed plugins (VST3, CLAP, VST2, AU, LV2) that show their own interface open in their own
plugin window (`src/app/PluginWindow.cpp`: the plugin's view, Gridline's control bar under it).
That window has no channel strip, so the slide never moves or resizes a native plugin view. A
hosted plugin shown in Gridline's own panel (generic editor, no interface, sandbox placeholder)
is drawn by Gridline in the channel window and slides like any built-in editor.

## Dev check and test

`--dev-input` command `strip [on|off] [frames]`: this frame's (and the next `frames` frames')
strip state to stdout: whether it is on, the shown fraction, whether it is sliding, the shown and
full heights, the eye button, the window, and (when drawn) the clipped area, the keyboard and the
volume knob. `strip on` / `strip off` first show or hide it at once (no slide) when the window
is next drawn: a test set-up for tests that click inside an editor laid out with the strip shown
(`plugin_undo_app`, `j112_play_stops_plugins`). `plugwin chan <index>`, `plugwin fx <mixer
index> <slot>` and `plugwin close` open and close plugin windows.

`tests/plugin_strip.cmake` (`plugin_strip_app`): hidden by default, the eye slides it in over
several frames, the window size is stable after the slide and taller by the strip's height,
hidden again after a restart (and not in settings.json), the keyboard plays and the volume knob
turns, hiding slides it back to the hidden size, and shown, closed and opened again it is hidden.

## Popped out

A plugin window pops out of the main window only with its pop-out button, and only while it
stays open: closed and opened again, after a restart or with another project it opens inside
the main window again (`docs/detachable-windows.md`, "Plugin windows").

## Windows plugins through yabridge

`.github/workflows/yabridge.yml` and `tests/yabridge/` run Gridline on a GitHub Linux machine
(Ubuntu 24.04, yabridge 5.1.1) with real windows and a real X server, and click, type and drag
through X. A probe plugin in VST3, VST2 and CLAP logs what reaches it, along with Surge XT, Dexed
and Nekobi. Carla runs the same probe and plugins as a control host, so a failure can be pinned on
the right side. These results are from one full run with three repeats of each cell.

**Mouse input depends on the Wine version.** The boundary is between Wine 9.21 and 9.22:

| Wine | Clicks in Gridline | Clicks in Carla |
|---|---|---|
| 9.0 (Ubuntu's package) | exact | exact |
| 9.21 Staging | exact | exact |
| 9.22 Staging | offset or lost | offset or lost |
| 10.20 Staging | offset or lost | offset or lost |
| 11.21 Staging | offset or lost | offset or lost |

On 9.22 and later the interface still shows and the keyboard still works, but a click lands offset
by the window's position on the screen or reaches nothing. Carla behaves the same way, and
yabridge 5.1.1 states that it supports Wine up to 9.21. Until this changes, use Wine 9.21 or older
for bridged plugins whose interface you need to click.

**What works on every Wine above:** scanning and loading, sound, saving and restoring state, typed
text and Space, and closing and reopening the interface ten times. If the Wine side dies, the
plugin shows as crashed and goes silent, other tracks play on and the project still saves.

**Known problems measured:**

- The first click after a plugin window moves for the first time can be lost or land in the wrong
  place on Wine 9.0 and 9.21. With no window manager this happened in every Gridline run (36 of
  36). With Openbox it happened in 4 of 36. Carla had none in 24. Later moves are fine. The cause
  is not found yet and this one looks like Gridline's.
- Dexed: closing its interface does not return within 30 s on any Wine above, in Gridline and in
  Carla, and Gridline stops answering while it waits. Quitting with it open still ends after 20 s
  (30 of 30). Its interface is black on Wine up to 10.20 in both hosts, with one exception in 24
  runs, and draws on 11.21.
- Surge XT: its interface draws and drags change its parameters, but no screenshot taken right
  after a drag showed any change, in Gridline (30 of 30) or in Carla (14 of 14). After moving the
  window the changes showed in 8 of 12 runs on Wine 9.0 and 9.21, and in none of 18 on later Wines.
- Nekobi's VST2 build reports its edits as 0 in both hosts; its VST3 build reports real values.

**Not covered:**

- Run in sandbox shows no plugin interface on Linux.
- With Gridline on the Wayland video driver the plugins scan and load, but the interface does not
  open.
- XWayland has no clean result yet. XWayland on the headless Weston of the test machine crashes on a
  synthetic click, even on a plain X client with neither Gridline nor Wine, so mouse and keyboard
  there need a person to check.
