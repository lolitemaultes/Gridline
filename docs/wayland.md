# Running on Wayland

Gridline runs on Wayland desktops (GNOME, KDE Plasma, Sway, Hyprland, Cosmic and others) in one of
two ways. You choose in **Settings > Interface > Window system**; the choice applies the next time
Gridline starts, and the line under it says which one is in use now.

## Automatic (the default)

On a Wayland desktop Gridline runs through **XWayland**, the X11 compatibility layer every major
Wayland desktop ships, whenever it is available. This is deliberate:

- **Plugin interfaces.** VST3, VST2 and LV2 plugins on Linux can only draw their interface inside an
  X11 window, and CLAP plugins almost always do too. Bridges for Windows plugins (yabridge) work the
  same way. Through XWayland a plugin's interface sits inside its Gridline plugin window, with
  Gridline's bar and settings under it, exactly as on an X11 desktop.
- **Detached windows.** Panels popped out into windows of their own need to know and set where those
  windows are on the screen. Wayland does not let a program do that, X11 does.

Gridline asks for X11 even when the desktop sets `SDL_VIDEODRIVER=wayland` for every program (some do,
for games). Without XWayland it runs on Wayland by itself.

## Native Wayland

Choose **Native Wayland** in the setting, or start Gridline with the environment variable
`GRIDLINE_VIDEO_DRIVER=wayland` (the variable wins over the setting; `GRIDLINE_VIDEO_DRIVER=x11` or
`auto` go the other way). Gridline's own windows are then Wayland windows:

- The interface is drawn at your display's scale by the compositor, sharp at 200 %; at fractional
  scales (125 %, 150 %) it depends on the SDL library of your system: SDL 3 (and SDL 2 provided
  through sdl2-compat, as on Arch and Fedora) draws at the exact scale, older SDL 2 draws at the next
  whole scale and the compositor shrinks it.
- Menus, lists, hints and dialogs stay inside the main window.
- **Plugin interfaces** open in a window of their own on XWayland, titled with the plugin's and the
  channel's names, and Gridline's bar for that plugin opens as a small window of its own next to it.
  Wayland does not let a program place or stack its windows, so the two are not joined together as
  on X11, and the desktop decides where they appear. Interfaces that always open their own window
  (LV2 plugins with an external interface) open as they are. Without XWayland at all, a plugin shows
  its parameters in Gridline's own panel and Gridline says why its interface cannot open.
- **Detached windows** are off: every panel stays inside the main window, where it can still float,
  dock and be resized. The pop-out button says why.
- Files dropped from a file manager land where you let go of them, and a second launch (opening a
  project from the file manager while Gridline runs) brings the running Gridline to the front with
  the activation token the desktop gave the launch, so desktops that refuse to let programs come
  forward by themselves let it through.

## Which should I use?

Keep **Automatic** if you use plugins with their own interfaces or detach panels onto other
monitors. Native Wayland suits sessions without XWayland, or working mainly with Gridline's own
instruments and effects.

## Notes

- On GNOME, native Wayland windows get their title bar from the libdecor library (package
  `libdecor-0-0` with its plugin, installed with GNOME on most distributions).
- The Flatpak runs through XWayland: it has the X11 socket only, so the Native Wayland setting
  has no effect there.
- Gridline's file dialogs are its own, the same in both modes.
- Dragging files from Gridline into other programs is not available on Linux yet, in either mode.
