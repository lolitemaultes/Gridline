# Plugin windows: the channel strip

A Gridline instrument opens in its channel window (`src/app/ChannelSettings.cpp`): a header row
(colour, name, instrument, mixer track), the channel strip, then the Plugin / Envelope / Misc
tabs with the instrument's editor (a sampler channel opens the Audio clip window instead, see
[audio-clip-editor.md](audio-clip-editor.md)). The channel strip holds the channel's volume, pan and pitch
knobs and a two-octave preview keyboard. Hold a key and slide to play the keys in turn, one at a
time, as on the piano roll's keyboard and every built-in instrument keyboard (`src/app/KeyGlide.h`);
letting go, Escape, leaving the window, switching apps or closing the window stops the key.

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

A plugin whose editor is designed for 100 % (`PluginInfo::editorFullSize`; Plasma) opens at
100 % instead, whatever the global size, unless a size was picked for it in its window menu; its
menu's first Editor size entry reads "Full size, fitted to the screen". On a 1920 x 1080 screen
Plasma's window then fits at 100 %, because the channel window's own rows are kept short: the
plugin header (menu, presets, A/B) sits on the tab row after the tabs whenever it fits there (its
width as drawn last time; otherwise, and for the sampler, it keeps its own row), and a channel
without note processors shows one slim row (title, Add note processor, a hint) instead of the
note processor panel, which appears once one is added.

The window's own parts around the editor: the header row (menu, wordmark or the installed plugin's
name on the row's middle, presets, A/B, and for effects the bypass button and mix knob, which end at
the window's right edge, on a row of their own when the window is too narrow for them), the editor,
and the resize corner, drawn in the window's padding beside the editor's corner. When the window's
own rows need more width than the editor (a small effect's header, the channel window's rows), the
editor sits in the middle of the window. A preset loaded from the window menu (Presets, Load preset
from file, Reset to default) shows in the preset box, and previous and next go on from it.

Closed and opened again in the same session, a plugin window comes back where it was (its top left
corner; its size follows the editor). A window that was popped out opens inside the main window,
centred, as it does the first time.

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
each once its window has kept its size for two frames (at most `frames` frames, 12 by default),
with the frames and milliseconds it took (`GRIDLINE_FIT_ONLY=<type,type>` limits it). An
optional second argument names a directory for one screenshot per plugin. After the last one it
prints `edfit done` and quits the app, so a run never waits on its frame count.
Without screenshots the sweep only lays the windows out and draws nothing, since the fit
report needs the layout and not the pixels.
`hdrmix` prints where the last effect header drew its mix knob against its window's right edge,
`hdrpreset` the header's preset name and its place in the list, and `edzoom` also prints an
`edregion` line: the editor region's width and the room left and right of it in its window.
`tests/k286_plugin_wrapper_app.cmake` checks the header's right edge, the centred editor, a menu
preset in the header, bypass and its undo, reopening in place and the editor's keyboard focus.
`tests/k147_plugin_fit_app.cmake` runs it at several sizes and interface scales and fails on any
clipped or spare edge. Each run has its own time limit and reports how far it got, so the test
fails within minutes instead of running on; it closes the main windows (playlist, browser, piano
roll, mixer, channel rack), whose frames cost the most on software-rendered machines.

Installed plugins (VST3, CLAP, VST2, AU, LV2) that show their own interface open inside Gridline
by default, in the channel window's Plugin page or the effect window, at the interface's own size
(see "Installed plugins inside Gridline" below); the strip's slide moves the interface down with the
page. In a window of their own (the setting, or the plugin's own choice) they open in their own
plugin window (`src/app/PluginWindow.cpp`: the plugin's view, Gridline's control bar under it),
which has no channel strip, so the slide never moves or resizes a native plugin view there. A
hosted plugin shown in Gridline's own panel (generic editor, no interface, sandbox placeholder)
is drawn by Gridline in the channel window and slides like any built-in editor.

## Installed plugins inside Gridline

An installed plugin's own interface sits inside Gridline, as a built-in editor does: in its plugin
window inside the main window (the channel window's Plugin page for an instrument, the effect window
for an effect), under that window's header (the window menu, the plugin's name, presets, A/B, and for
effects the bypass button and mix knob). Settings > Interface, **Open installed plugins**, chooses
**Inside Gridline** (the default) or **In their own windows** (each interface in a window of its own
above Gridline, with Gridline's bar under it, as before 0.7.8).

- **Everywhere the window goes.** The interface moves with its window, is cut by its pane's edges
  when the pane is smaller (scrolling the pane scrolls it), hides with a hidden tab or a closed or
  collapsed pane, and closes with the window. In Studio the window floats inside the main window; in
  Clips it is the device's plugin window; in Smooth it is the effect or instrument in the lower pane
  (or its own window there); in the Tracker it is the window **Open plugin window** opens.
- **Gridline stays in front.** Menus, lists, tooltips, drag previews, notices and any Gridline window
  over the interface are drawn in front of it: on macOS and Windows the part they cover is cut out of
  the interface while they are there; on Linux the interface hides while one is over it (a window
  across a whole edge only trims it). A dialog hides it until the dialog closes.
- **The keyboard.** Click the interface and it has the keyboard: keys it does not use (Space, the
  shortcuts, the typing keyboard) reach Gridline as they do from a window of its own, and a text field
  of the interface keeps every key. Clicking Gridline's part of the window gives the keyboard back.
  In the Smooth layout the arrow keys stay with the plugin (in a window of its own too): they do not
  select another region or track in the tracks area.
  A click on the interface brings its window to the front of Gridline's windows.
- **Popped out and back.** The window's pop-out button puts the whole window (header and interface)
  in a window of its own; its dock-back button brings it back. The interface opens again in its new
  window (it is the plugin's own view: the plugin builds it again there), so it keeps its state but
  may take a moment.
- **One plugin in a window of its own.** The window menu's **Open in a window of its own** keeps this
  plugin in a window of its own from now on (whatever the setting), and the button with the arrow
  into a window on that window's bar brings it back inside. The choice is kept per plugin in the
  plugin settings (`plugin-manager.json`, `options.<type>.window`: 1 inside, 2 its own window; none
  follows the setting). **Parameters and settings...** in the window menu shows Gridline's panel for
  the plugin (every parameter, processing, sandbox, MIDI, outputs); its **Interface** button goes back.
- **What stays in its own window.** A plugin running in the sandbox on macOS shows its interface from
  the sandbox process, which cannot sit inside Gridline's window: it opens in a window of its own and
  says so once. So does an interface that opens a window of its own (some LV2 interfaces), and every
  interface while Gridline runs on Wayland ([wayland.md](wayland.md)). On Linux a sandboxed interface
  sits inside like any other.
- **Resizing.** A plugin that resizes itself (its own size control or corner) resizes its window.
  Gridline's window is not dragged larger around the interface; use the plugin's own size control,
  or put it in a window of its own to size it from the window's edges.

How it works (`src/app/PluginWindow.cpp`, `hostedui::drawDockedInterface` and
`placeDockedInterfaces`): the pane reserves the interface's size in its layout and notes where it drew
it this frame; after `ImGui::Render()` the plugin's container (`native::createDockedContainer`: a
clipping view with the container inside it, in the window of the pane's viewport) is moved to that
rectangle, cut to what the pane shows (`layout::dockedShow`) and given holes where Gridline's windows
drawn after the pane, and every popup, menu and tooltip, cover it. The window system is asked to move
it only when something changed, so dragging a window does not flicker. A pane that did not draw it
this frame hides it; a pane that no longer shows the plugin closes it. Keys: on macOS SDL sees the
main window's keys even when the interface has them, so while the interface has the keyboard those
are dropped (`hostedui::dockedKeyEvent`), and the keys it passes on come back through its container,
marked so they are not dropped again.

| Platform | Inside Gridline | Holes for menus | Checked |
|---|---|---|---|
| macOS | yes (a view in the window's content view) | yes (a layer mask) | yes; sandboxed interfaces keep their own window |
| Windows | yes (a child window) | yes (a window region) | not yet on a Windows machine |
| Linux X11 | yes (a child window) | no: hides while covered | not yet on a Linux machine |
| Linux Wayland | no | | their own window on XWayland |

Dev: `plugwin info` prints `inside 1 host ... visible x,y wxh holes n` for an interface inside;
`plugwin place inside|own` is the menu's and the bar's choice for the open effect (else channel);
`plugwin inst <channel> <type>` puts an instrument on a channel and opens it; `plugwin dockkey <kind>
[space]` gives the keyboard to a test view in the interface (0 passes keys on, 1 uses them, 2 a text
field); `plugwin winshot <png>` captures the main window with the interfaces in it (macOS; with
`GRIDLINE_DEV_SHOW_MAIN=1` a hidden run shows its main window for it). `GRIDLINE_PLUGIN_WINDOWS=own`
or `inside` overrides the setting (tests). Test: `tests/plugin_dock_app.cmake`; the tests of
installed plugins' own windows run with `own`.

## Typing a value

Right-click any knob, slider or parameter in a Gridline plugin editor (in the app, and in the
plugins as VST3, CLAP and AU) and choose **Type a value...** (a double-click on a control that sits
at its default opens the same box). The box shows the value with its unit; type the new one the
way you would say it:

| Type | For |
| --- | --- |
| `2.5k`, `2.5 kHz`, `440` | frequencies |
| `-6`, `-6 dB` | levels and gains |
| `250 ms`, `0.5 s` | times |
| `1/4`, `1/8 dotted`, `1/16 t`, `2 bars`, `1.5 beats` | delay times (at the song tempo when the time is free) |
| `50%` | percent controls; on other controls, that far along the travel |
| `C4`, `A#3`, `Bb3` | notes |
| `50% left`, `30 R`, `centre` | pan |
| `4:1` | ratios |
| `x1.5` | multipliers |
| a name, `on`, `off` | choices and switches |

Enter applies the value as one undo step, Escape cancels, and a value beyond the range stops at
its end. A text that is no value for that control leaves the box open with a note.

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

## Macro knobs

Every channel and every mixer track has eight macro knobs. A channel's are on the **Misc** page of
its window (in the Tracker, **Open plugin window** on the Channel frame shows that window); a
mixer track's are under the editor of each of its effects (**TRACK MACROS**: click the line to
open or fold it; it opens by itself while one of the track's macros is in use). One macro can move
many controls at once, each over a range of its own.

- **Link a control.** Right-click any control of a plugin (a Gridline knob, a row of an installed
  plugin's parameter list) and choose **Link to macro**: **New macro** takes the next macro that
  drives nothing yet, or pick one of the eight. A channel inside a layer also lists the layer's
  macros. For a control on a plugin's own interface, move it and use **Tools > Last moved
  control > Link to macro**, or press **Map** under a macro and choose **Learn**, then move the
  control.
- **Map** under a macro also lists the plugin's controls (with a search box): click one to link or
  unlink it.
- **Range and curve.** Click a macro (or its name) to see what it drives below the knobs. Each line
  has its own **from** (where the control sits with the macro at 0) and **to** (with the macro at
  its top; a lower value than **from** turns it the other way), a mapping curve and a button that
  unlinks it. A new link starts at the control's current value, so the sound does not change until
  the macro moves. **Controller link settings** on the control's right-click menu also sets the
  smoothing.
- **Names.** Double-click a macro's name, or right-click it, to rename it; the right-click menu
  also unlinks every control at once.
- A macro is itself a control: automate it, link it to a MIDI control, or let another macro drive
  it (a macro cannot drive itself, also not through others). Every macro in use is listed in the
  channel rack's **Add automation clip for** menu and in the browser under **Current project >
  Automation** (double-click one for a new automation clip).
- Macros, their names, values and links are saved with the project, every change is one undo step,
  and they are shared in a collaboration session.

## Time knobs

Every time control of Gridline's plugins has gentle magnets at the values people aim for: round
milliseconds and seconds (1, 2, 5, 10, 20, 50 ...) on times, note values (1/64 to bars, dotted
and triplet) on lengths in beats, steps or ticks, and on slow rates in Hz the note values at the
song's tempo (at 120 BPM a 1/4 is 2 Hz). Between the magnets the knob moves freely; Shift drags
finely without them; the mouse wheel steps from one to the next. The readout names the value
("1/8 dotted (321 ms)", "1/4 (2.00 Hz)", "250 ms"), and **Type a value...** takes note values on
these controls too. Installed plugins keep their own knobs.

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

## Several windows open

Click anywhere on a plugin window, its editor, a knob or its keyboard included, with any mouse
button, and it comes in front of the other windows and takes the keyboard; so does an installed
plugin's interface inside Gridline. Installed plugins' own windows do the same among themselves (see `docs/detachable-windows.md`, "Input"). They stay
above the main window, so a Gridline panel inside the main window cannot come in front of them.
A plugin that shows its interface from its sandbox process on macOS raises its own window there.

## In the Clips layout

Every instrument and effect editor opens in its own floating window, as in Studio, wherever it is
opened from (a mixer slot, the channel rack, a menu, a device's window button or a double-click on a
device's picture). The device band shows only the devices' cards: the fold arrow, the main knobs and
the window button. A sampler channel's settings are the exception: they show in its card (see
[the audio clip editor](audio-clip-editor.md)).

The Clips layout handles installed plugins' own windows in its own way (with **Open installed
plugins: In their own windows**, or for a plugin kept in a window of its own; inside Gridline the
interface is in the device's plugin window, as a built-in editor is). Three settings in
Settings > Interface, under "Plugin windows (Clips)", control it, and all three are on at first:

- **Open a plugin's window when it is put on a track.** Only for a plugin you add by hand, one at a time (a cloned channel and a copied effect count too). Opening a project, loading a drum kit, scripts, AI tools, an action that adds several plugins at once, undo, redo and a collaborator's additions never open a window.
- **Several plugin windows at once.** Turn this off and opening one plugin window closes the others.
- **Show only the selected track's plugin windows.** Selecting another track hides the other tracks'
  windows and brings back the new track's own. A hidden window stays open and keeps running, and it
  comes back where it was.

Ctrl+Alt+P hides every plugin window, and pressing it again shows them. The plugin's window button
in the device band also brings a hidden window back. In the other layouts the command is in
Settings > Shortcuts without a key. Interfaces shown from the plugin sandbox's own window cannot be
hidden this way. The Studio layout keeps its plugin windows as before.

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

- With Gridline on the Wayland video driver (Window system: Native Wayland) the interface opens in
  a window of its own on XWayland ([wayland.md](wayland.md)); bridged Windows plugins have not been
  measured in that window.
- XWayland has no clean result yet. XWayland on the headless Weston of the test machine crashes on a
  synthetic click, even on a plain X client with neither Gridline nor Wine, so mouse and keyboard
  there need a person to check.
