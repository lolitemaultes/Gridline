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

### Plugin window size and fit

Every built-in editor (effect slot windows and channel windows alike) is laid out at its design
size in a region exactly as wide as the editor (`PluginInfo::editorWidth`; the channel window at
least 600 px for its own rows), and that region is drawn at the plugin window size (Settings >
Interface, 75 % by default, 60 % to 150 %; or a plugin's own Editor size, 60 % to 200 %) as one picture (`src/app/EditorZoomUi.cpp`). The
window is the region at that size plus the window padding: no room is kept for a scrollbar or a
margin the editor does not draw, so the header's right edge and the editor's right edge agree.

If a window at the size asked for would not fit the work area, the editor is reduced until the
whole window fits (`edzoom::fitZoom`), down to 60 % (`edzoom::kFitMin`); only an editor still too
tall at 60 % scrolls. So a large instrument on a small screen, or at interface scale 200 %, is
never cut off at the bottom or the right. At exactly 100 % the editor is drawn straight into its
window (no magnified region). The test suite pins every editor at 100 % with
`GRIDLINE_PLUGIN_WINDOW_SIZE=1`; with it set, a large editor keeps 100 % and scrolls instead of
being reduced, so tests can click at design positions. The fit test clears it.

`--dev-input` commands: `edzoom <fxeditor|channelsettings>` prints the zoom, the design size and
the window, followed by an `edfit` line: what the editor drew against its window, in screen
pixels (clipped past the left, top, right and bottom edges; spare room inside the padding on the
left, right and bottom; the plugin's own drawing under the header: its width, its left offset and
the width it was given; and how far any row, the header included, was laid out past the region's
right edge). `fitall [frames]` opens every built-in plugin in turn and prints both for
each (`GRIDLINE_FIT_ONLY=<type,type>` limits it), with an optional second argument: a directory
for one screenshot per plugin. `tests/k147_plugin_fit_app.cmake` runs it at several sizes and
interface scales and fails on any clipped or spare edge.

Installed plugins (VST3, CLAP, VST2, AU, LV2) that show their own interface open in their own
plugin window (`src/app/PluginWindow.cpp`: the plugin's view, Gridline's control bar under it).
That window has no channel strip, so the slide never moves or resizes a native plugin view. A
hosted plugin shown in Gridline's own panel (generic editor, no interface, sandbox placeholder)
is drawn by Gridline in the channel window and slides like any built-in editor.

## Magnetic divisions on knobs

A knob or slider can carry soft magnets (`pui::nextMagnets(values)` before it is drawn, in the
parameter's own units; `core/KnobMagnet.h`). A drag passes its pointer through a curve: within a
capture zone of 7 drag pixels around a division the value holds on it for the inner 45 % of the
zone, then eases back to the pointer (continuous, never going back, the same curve both ways, so
it cannot flicker at the edge). Between divisions every value can be set and kept. Shift drags
finely without magnets; the mouse wheel steps from one division to the next; typed values and
automation are never snapped; a drag is one undo step. Ticks outside the ring (or beside a
slider's track) mark the divisions, brighter on the current one. Used on Tracer's synced Time
(1/64 to 1 bar, dotted and triplet), Retro Phaser's synced period and echo time, Bucket Chorus's
synced period, and the delay times of Halftone and Lattice when synced. Tests: `test_k147.cpp`
(the curve) and `tests/k147_magnet_app.cmake` (a real drag on Tracer).

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
for bridged plugins whose interface you need to click. Gridline telling the interface where its
window went (below) does not change this: the same clicks stay offset by the window's position on
9.22, 10.20 and 11.21. yabridge's issue 382 reports that Wine ignores such a message since 9.22,
and users there report other Wine builds and settings as workarounds. Gridline has tested none of
those and does not recommend them.

**What works on every Wine above:** scanning and loading, sound, saving and restoring state, typed
text and Space, and closing and reopening the interface ten times. If the Wine side dies, the
plugin shows as crashed and goes silent, other tracks play on and the project still saves.

**Fixed, measured:**

- The first click after a plugin window moves: the plugin's view sits in a child window of Gridline's
  hidden top-level window, and yabridge only updates its idea of the position when it is told about
  that child or when the pointer enters. Moving the window told it neither, so the pointer was
  still inside the old place and the click landed there. This happened in 36 of 36 runs with no
  window manager and in 4 of 36 with Openbox. Now, on Linux, `native::containerMoved` sends the
  plugin's container a ConfigureNotify when the window's position changes. Clicks were exact after
  each of three moves on Wine 9.0 and 9.21, in VST3, VST2 and CLAP, at scale 1 and 2 with Openbox
  and at scale 1 without a window manager, in process and in the sandbox (2 full runs, 3 repeats each).
- Run in sandbox shows the plugin's interface on Linux: the sandbox process puts its window inside
  Gridline's plugin window, under the same bar. VST3, VST2 and CLAP probes and the VST3 builds of
  Nekobi, Dexed and Surge XT open on every Wine above. On Wine 9.0 and 9.21 clicks, drags, keys,
  resizing and quitting behave as they do in process, with and without Openbox (3 repeats each).
  On 9.22 and later the clicks are off in the same way. Under XWayland it opens; input there is
  not measured.
- The bar of a plugin window follows Gridline's interface scale on X11: at system scale 2 its row
  is 56 pixels, in process and in the sandbox, with Openbox and under XWayland.

**Known problems measured:**

- Closing an interface right after it opened can end the Wine side with an X error (BadWindow on its
  own window) on Wine 9.0 and 9.21: 9 of 216 runs of the test that closes and opens ten times at
  once. Keeping Gridline's window for 1.5 s longer did not change it. The real plugins' single
  closes showed none.
- Dexed: closing its interface does not return within 30 s on any Wine above, in Gridline and in
  Carla. Gridline closes the interface of a bridged plugin on a helper thread and waits 300 ms; if
  that is not enough the window hides and Gridline goes on (it answered in about 0.4 s in 30 of 30
  closes). After 15 s the plugin counts as lost and shows as crashed, and quitting ends cleanly with
  code 0 (45 of 45). Its
  interface is black on Wine up to 10.20 in both hosts, with one exception in 24 runs, and draws on
  11.21.
- Surge XT: its interface draws and drags change its parameters, but no screenshot taken right
  after a drag showed any change, in Gridline (30 of 30) or in Carla (14 of 14). After moving the
  window the changes showed in 8 of 12 runs on Wine 9.0 and 9.21, and in none of 18 on later Wines.
- Nekobi's VST2 build reports its edits as 0 in both hosts; its VST3 build reports real values.

**Not covered:**

- With Gridline on the Wayland video driver the plugins scan and load, but the interface does not
  open. Run in sandbox has not been measured on that driver.
- XWayland has no clean result yet. XWayland on the headless Weston of the test machine crashes on a
  synthetic click, even on a plain X client with neither Gridline nor Wine, so mouse and keyboard
  there need a person to check.
