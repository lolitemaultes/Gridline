# Quiet windows (tests and tools)

Many tests start the real app and open real windows: plugin interfaces of the in-tree CLAP, VST3
and LV2 test plugins, the plugin sandbox's own window, the crash report dialog. Quiet windows let
those tests run while you work: nothing they open appears on your screen, and neither the app
nor any process it starts becomes the active app, so the keyboard and the window in front stay
yours. The windows are still made, sized, placed, parented, shown, hidden, given the keys and
drawn exactly as in a normal run, so the tests check the same code.

## When it is on

- `GRIDLINE_QUIET_WINDOWS` is set to anything but `0`.
- Every `--hidden` run of `gridline-app` sets it for itself and for the processes it starts (the
  plugin sandbox, the plugin scanner, the crash report dialog), unless it is already `0`.
- `ctest` sets it for every test. Configure with `-DGRIDLINE_TEST_QUIET_WINDOWS=OFF` to watch the
  windows of a test run again, or run a single app command with `GRIDLINE_QUIET_WINDOWS=0`.

A plain start of Gridline never has it, and then none of it does anything.

## What it does

**macOS.** The app's activation policy is "prohibited": no Dock icon, no menu bar, no entry in
the app switcher, and it cannot become the active app. SDL's own activation at start-up is off
(`SDL_MAC_BACKGROUND_APP`), and every request to activate an app made inside a quiet process is
dropped. Each window is made fully transparent and lets the mouse through the moment it is ordered
in, wherever it is placed; its frame, level, parent window and visibility are what the code asked
for. Code that reads the window's alpha or mouse setting gets back what it set. The key window is
kept by the app itself: making a window key (or showing it the way SDL does) moves the keys to it
inside the app, with the same notifications SDL and window delegates get in a normal run, and
closing the key window passes them to the window in front.

Pictures of plugin windows taken through the window system (the `.editor.png` that `--screenshot`
writes for an open plugin interface, and the dev-input command `stackshot`) come out transparent in
a quiet run; set `GRIDLINE_QUIET_WINDOWS=0` for those. The app's own `--screenshot` and `shot`
frames are drawn by the app and are not affected.

**Linux and Windows.** Windows are shown without being activated (SDL's
`SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN`), and the app makes each window it shows fully transparent.
On X11 transparency needs a compositing window manager, so on a desktop session run the tests on a
virtual display as well (`xvfb-run ctest`), as CI does.

## Checking it

The dev-input command `quietcheck` prints one line:

```
quietcheck frame 32: quiet 1 guards 1 policy prohibited active 0 became-active 0 own-activations 0 blocked-activations 2 quieted 1 on-screen 1 visible 0 key "Synth"
```

`on-screen` counts the windows of the app and the processes it started that the window server has
ordered in, `visible` the ones of them that could be seen on a display (it must be 0), `active`,
`became-active` and `own-activations` whether the app or one of its processes was ever the active
app, and `key` names the window that has the keys inside the app. `GRIDLINE_QUIET_WINDOWS_LOG=<file>`
appends a line to the file for every window a quiet process kept off the screen and every
activation request it dropped.

The tests `quiet_windows_mac` and `quiet_windows_app` fail if a window of a quiet run can be seen on
a display or a test process becomes the active app, and check that a normal run is left as it is.
