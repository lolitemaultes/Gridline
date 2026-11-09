# Command palette

The command palette is one search box for everything Gridline can do: every command, every menu
item, every window, every setting and every plugin. Open it, type a few letters, press Enter.

## Opening it

| Shortcut set | Opens the palette |
|---|---|
| Studio, Live | Ctrl+K (Cmd+K on macOS), or Ctrl+Shift+P |
| Logic | Ctrl+Shift+P (Cmd+Shift+P on macOS) |

It is also the first item of the **View** menu, and it can be given any other key in
Settings > Shortcuts ("Command palette").

- In the channel rack, Ctrl+K stays the rack's own shortcut for the graph editor (a window's own
  shortcut wins while that window has the focus), so use Ctrl+Shift+P there.
- The Logic set uses Ctrl+K (Cmd+K) for the typing keyboard, so in that set the palette keeps only
  Ctrl+Shift+P.
- If you give Ctrl+K to another command yourself, that command gets it and the palette keeps
  Ctrl+Shift+P.

The same shortcut, or Esc, closes it. Clicking outside it closes it too.

## What it finds

- **Commands and menu items.** The palette reads the menus of the layout in use, so it finds
  exactly what the menu bar offers, with where each item lives ("Options > Recording"), its
  shortcut and whether it is ticked. Items that cannot run right now are greyed, with the
  reason the menu gives. Shortcut-only commands (those with no menu item) are listed too.
- **Windows.** The mixer, playlist, piano roll, browser and every other panel, and each
  channel's and mixer effect's plugin window. Choosing one opens it and brings it to the front.
- **Settings.** Every row of every settings page. Choosing one opens Settings on that page and
  lights the row for a moment.
- **Plugins.** Every instrument and effect the plugin picker lists. Choosing one adds it the way
  the plugin picker does.

## Searching

- Words may come in any order and each must match; they can match the name or where it lives
  ("export midi" finds File > Export > Song as MIDI file).
- Names that start with what you typed come first, then names with a word starting with it,
  then initials ("pdc" finds Plugin delay compensation), then the letters anywhere.
- Small typos are forgiven ("metornome" finds Metronome).
- With the interface in another language, both the translated and the English name work.
- You can type a shortcut ("ctrl+s") to find what it does.

## Keys

| Key | Does |
|---|---|
| Up, Down, Page Up, Page Down | Move through the list |
| Enter | Run the selected row |
| Tab, Shift+Tab | Next or previous category (All, Commands, Windows, Settings, Plugins) |
| Esc | Close |

## Recent

With nothing typed, the rows you ran last are listed first, newest first (up to eight). The list
is your own setting, kept with your other settings; projects never store it.

## For developers

`src/app/CommandPaletteUi.h` offers `cmdpalette::commandList(app)` (every row: id, label, menu
path, shortcut, enabled) and `cmdpalette::run(app, id)`, for toolbars that pin commands. Ranking
and the recent list are in `src/core/CommandPalette.h`. The menus are read through small hooks in
the bundled Dear ImGui (`BeginMenu`, `EndMenu`, `MenuItem`), so new menu items show up in the
palette without any extra code.
