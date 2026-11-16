# Game controller and Steam Deck

Gridline runs in Steam Deck Game mode, where there is no physical keyboard. The Deck's own
controls (and any game controller) reach the main commands, and the command pad gives large
tiles for the rest. Nothing in the layouts changes: the pad is drawn only while it is open.

## When it is on

**Settings > General > Game controller > Controller** decides (settings key `input.gamepad`):

| Value | What happens |
| --- | --- |
| `auto` (default) | On in Game mode (Steam on the Deck or a gamescope session), and anywhere as soon as a controller button is pressed |
| `on` | Always on |
| `off` | Controllers are ignored |

In Game mode a note appears once: "Controller: press the Menu button for the command pad".

## Buttons

| Button | Alone | With L1 held |
| --- | --- | --- |
| A | Play / stop | Record on / off |
| B | Escape (closes a dialog or menu) | Go to the start |
| X | Undo | Redo |
| Y | Open the command pad | Save |
| D-pad left / right | Previous / next window | Previous / next pattern |
| D-pad up / down | Previous / next channel | Back / forward one bar |
| Menu | Open or close the command pad | |
| View | Command palette | |
| R1 | Note entry on / off | |
| L3 (left stick click) | Typing keyboard on / off | |
| R3 (right stick click) | Steam's on-screen keyboard | |

The windows the D-pad steps through are the playlist, channel rack, piano roll, mixer and
browser. A window that is already open comes to the front; it never closes. The trackpads and
the touch screen move the pointer as usual.

While a menu, a pop-up, a dialog or the command palette is in front, the D-pad moves in it as
the arrow keys do, A picks (Enter) and B closes it (Escape).

**Settings > Shortcuts** lists the buttons: with a controller in use, the **Controller** button
at the top of the page shows them in place of the key list (in Game mode they show first).

## Note entry

R1 (or the command pad's Note entry tile) turns note entry on: the D-pad and the face buttons
play eight notes of the scale on the selected channel, as the typing keyboard does, so they
record and step record the same way.

| Button | Note |
| --- | --- |
| D-pad left, down, right, up | 1, 2, 3, 4 |
| X, A, B, Y | 5, 6, 7, 8 (the octave) |
| L1 held | a semitone higher |
| Left stick up / down | octave up / down |

The scale is C major from middle C. Pick the channel with D-pad up and down before turning note
entry on. A small strip at the bottom of the window shows the octave and the channel while it is
on. Menu, View and the stick clicks keep their jobs; R1 turns note entry off.

## The command pad

Menu or Y opens it in the lower half of the screen, so the transport stays in view:

| | | | | |
| --- | --- | --- | --- | --- |
| Play / stop | Record | To start | Undo | Redo |
| Save | Playlist | Piano roll | Channel rack | Mixer |
| Browser | Note entry | Typing keyboard | Command palette | On-screen keyboard |

The D-pad or the left stick moves the lit tile, A runs it, and B or Menu closes the pad. You can
also tap a tile. Undo and Redo keep the pad open so you can press them again; every other tile
closes it.

The command palette lists every command by name, and the on-screen keyboard types into it.

## Plugin windows

Game mode shows one window, so Gridline's own plugin windows open as tabs beside the main view
(next to the playlist) instead of floating over it. **Settings > General > Game controller >
Plugin windows** chooses: Automatic (docked in Game mode, floating elsewhere), Always docked, or
Floating (settings key `input.dockPlugins`: `auto`, `on`, `off`). Installed plugins keep their
own windows.
