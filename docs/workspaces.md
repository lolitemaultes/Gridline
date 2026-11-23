# Workspaces, pane tabs, control bars and pinned commands

Gridline keeps two kinds of tabs apart:

* **Pane tabs**: the tabs of a layout's side and lower panes: along the top of the Smooth layout's
  right pane and lower pane, and down a narrow rail at the edge of the side panel in the Clips and
  Tracker layouts. They pick which window a pane shows.
* **Workspaces**: named arrangements of a whole layout (which windows are open, the pane sizes and
  the pane tabs), switched with one click or one key.

Free-floating and docked windows keep their own tab bars as described in `detachable-windows.md`.

## Pane tabs

* **Click** a tab to show its window in the pane. Pressing the window's own key again closes the
  pane, as before.
* **Drag** a tab along the strip to reorder it, or onto another pane's tabs to move its window
  there (in Smooth between the right pane and the lower pane). The tab joins the end of that pane and
  its window shows there; drag it back, or use **Move to** in its menu, to return it. The main area
  never takes a pane tab, so it can never be lost. Where you moved a tab is kept per layout, also
  across a restart.
* **Right-click** a tab for **Move to** another pane, **Pin tab** (a pinned tab stays first, is marked with a small ring and
  cannot be closed by accident), **Move left**, **Move right**, **Close tab**, **Reopen closed tab**,
  **Maximise pane** and **Close pane**.
* **Middle-click** closes a tab. The pinned tabs and the last tab of a pane do not close.
* **Overflow**: tabs that do not fit show as "+N" at the end of the strip; click it for a menu of
  them. Pinned tabs and the tab in use always stay in view.
* **Maximise and close**: while the pointer is over the strip, two buttons at its right end
  maximise the pane (it takes most of the window, the main area keeps the rest; again to restore)
  and close it. A double-click on an empty part of the strip also maximises or restores.

Keys (change them in Settings > Keyboard shortcuts):

| Command | Default key |
|---|---|
| Next tab in the pane | Ctrl+Page Down |
| Previous tab in the pane | Ctrl+Page Up |
| Reopen the last closed pane tab | Ctrl+Alt+T |

The order, the pins and the maximised panes are kept per layout in your settings
(`layout.<id>.state`), never in a project. Closed tabs come back with **Reopen closed tab**, with
the window's own key or menu entry, or at the next start. **View > Reset window layout** restores
the layout as designed.

### The side panel rail (Clips and Tracker)

The side panel of the Clips and Tracker layouts (View > Panels) shows its tools one at a time, with
a rail of tabs down its outer edge, one per open tool, read from top to bottom. The tool in front
has a bar on the edge toward the panel.

* **Click** a tab to bring its tool to the front. Next and previous tab (Ctrl+Page Down, Ctrl+Page
  Up) step through them.
* **Drag** a tab up or down the rail to reorder it. **Middle-click** closes the tool.
* **Right-click** for **Pin tab**, **Move up**, **Move down**, **Close tab**, **Reopen closed tab**
  and **Close pane**. Tabs that do not fit show as "+N" at the bottom.

The panel is closed by default, so these layouts look as before until you open it.

In the Tracker, the Browser and Performance open in this side panel, beside the pattern. The header
band at the top (the order list, tempo and rows, scopes, instruments, the buttons and the page tabs)
always stays in place: no window opens over it. Close the Browser from its tab, with **View >
Windows and panels > Browser** (**View > Browser** with the Studio menus) or with its key, and the
panel closes with it.

In the Tracker, plugins open in floating windows inside Gridline, as in Studio: a double-click in
the instrument list, **Channel settings**, **Open plugin window**, the Instrument button for an
instrument without a Tracker screen, a mixer effect, the Master chain and an installed plugin's own
editor. The lower band under the pattern shows only the Tracker's own screens: a Tracker Sampler's
Sample and Instrument editors, a Sampler's Sample editor, Seltmann's Chop and Instrument screens,
the plugin picker and the automation clip editor.

### The detail area (Clips)

Under the clip grid or the arrangement, the Clips layout has one detail area. It shows one of two
things at a time, always in the same place and at the same size:

* the **clip band**: the selected clip's editor (the piano roll for a MIDI clip, the audio clip's
  settings and waveform for an audio clip);
* the **device band**: the selected track's instrument and its effects.

What you click decides which one shows. Clicking a clip, in the clip grid or the arrangement, shows
the clip band; clicking a track (its title in the clip grid, its header in the arrangement, an empty
slot or an empty part of its lane, or Main or a return track) shows the device band. This also works
when the clip or track was already selected. Double-clicking a clip shows the clip band and gives it
the keyboard.

**Shift+Tab** switches between the two and gives the keyboard to the one shown. The **Clip** and
**Devices** buttons at the right end of the status bar do the same and stay lit for the one shown;
clicking the lit button hides the detail area, and Shift+Tab (or either button) brings it back.
**Navigate > Clip band** and **Navigate > Device band** work the same way, and **Navigate > Clip and
device bands** (Ctrl+Alt+L) hides or shows the area. Plain Tab is not used for this in Clips: it switches between
the clip grid and the arrangement.

Which one was shown is kept for the layout, and also with each Set you save: opening that Set again
shows the same one.

The Tracker's page tabs (Pattern, Song, Roll, Steps, Mix, Stage, LFO, Pads, Audio) show one page at
a time under the band; the View menu's keys pick them too (Ctrl+Alt+5 Mix, Ctrl+Alt+G Stage). The
[Stage page](stage-view.md#in-the-tracker) is the mixer's Stage view in the Tracker's look.

## Workspaces

Each layout starts with one workspace (today's arrangement), and nothing about the layout looks
different until you add a second one. Then a slim strip under the top bar lists them:

* The pill at the left names the layout the workspaces belong to. Each layout has its own list.
* **Click** a workspace (or press its key) to switch. What you changed in the workspace you leave is
  kept with it.
* **+** adds a workspace from the current arrangement. **View > Workspaces > New workspace** does
  the same, and **Add starter workspaces** adds a ready-made set for the layout (Studio: Arrange,
  Mix, Patterns, Edit; Clips: Session, Arrange, Devices; Smooth: Tracks, Mix, Edit; Tracker:
  Patterns, Instruments, Samples).
* **Double-click** a workspace to rename it; **drag** to reorder.
* **Right-click** for **Rename**, **Duplicate**, **Move left**, **Move right**, **Save current
  arrangement**, **Revert to saved**, **Save workspace changes automatically** (on by default) and
  **Close workspace**. After a close, **Undo** shows in the strip for a few seconds. The last
  workspace cannot be closed.
* With automatic saving off, a dot on a workspace means its arrangement changed since it was saved.

Keys: Ctrl+1 to Ctrl+9 switch to the first nine workspaces where the shortcut set leaves those keys
free (in the Smooth set Ctrl+2, Ctrl+3 and Ctrl+4 stay the mixer, the channel rack and the piano
roll). The Clips set uses Alt+1 to Alt+9, and the Tracker set Ctrl+Alt+Shift+1 to Ctrl+Alt+Shift+9
(Ctrl and a digit is its edit step). Ctrl+Alt+Page Down and Ctrl+Alt+Page Up step to the next and
previous workspace.

In Studio a workspace keeps the whole arrangement of its panes: where each window is docked, its tab
order, the tab in front, the size of every split, and the windows that float with their places.
Switching rebuilds it.

### The workspace cards

Click the layout's pill at the left end of the strip (or choose **View > Workspaces > Show all as
cards**) for a drawer of cards, one per workspace. Each card shows a small picture of its
arrangement, its key and what it keeps. **Switch** goes to it; **...** turns the card over to choose
what the workspace keeps:

* **Its windows**: the windows open in it (off: switching leaves the windows as they are).
* **Its pane sizes**: the size of each pane (off: the sizes stay as they are).
* **Its own control bar**: a bar of its own instead of the layout's (it starts as a copy of the
  layout's).
* **Its own pinned commands**: pins of its own instead of the layout's.

**Customise its bar...** opens the bar list for that workspace. **Save as now** keeps what is on
screen as the workspace's arrangement, **Revert** goes back to the one it was saved with, and
**Done** turns the card back. **Starter sets** adds the layout's starter workspaces, and **New
workspace** makes one from the current arrangement.

Workspaces are saved per person in your settings (`layout.<id>.workspaces`), and by default never in
a project. A workspace remembers which windows are open and, in the region
layouts, the pane sizes, tabs and which window each pane shows; Gridline rebuilds the layout from
that, so switching never loads an old window file. Settings and other dialogs are not part of a
workspace.

## Customising a control bar

Right-click an empty bit of the control bar, in any of the four layouts, for the bar list:

* Each part of the bar has a switch: turn it off and the bar leaves it out.
* Drag a part up or down, by its name or its grip, to move it within its group. A line shows where it
  will land; let go below the last part to put it at the end. The bar changes at once. The control
  bar page in **Settings > Layouts** works the same way, and keeps its arrow buttons.
* In Studio (left and right side) and Clips (left, middle and right group) a part can also be dragged
  onto another side: drag it over that side's heading or among its parts. The side lights up, the
  line shows where the part lands and the chip under the pointer names the side. On the bar the right
  side stays flush with the window's right edge and the left side with the left, with the usual
  spacing. A side you emptied stays in the list, so you can drag a part back onto it. Smooth's bar
  keeps its pane buttons and master in place, so only its switches move, among themselves; the
  Tracker's menu row has no parts to move.
* Right-click a part and choose **Always shown** to keep it when the window is too narrow for the
  whole bar. Otherwise parts leave a narrow window in a set order (the Layouts settings page lists
  it), the same order the bar always used.
* Type in the search field to find a part, or any command: click a command to pin it.
* **Reset** puts the bar back as it was designed. Your pinned commands stay.

**More layout settings...** at the bottom opens Settings > Layouts.

## Pinned commands

A pinned command is a button on the control bar and a row at the top of the command palette. To pin
one, open the command palette (Ctrl+K), select a command, menu item or window and press
**Ctrl+Enter**, or click the star at the end of its row. Press Ctrl+Enter again to unpin it.
Settings and plugins cannot be pinned.

With nothing typed, the palette lists the pinned commands first (under **Pinned**), then the recent
ones.

Right-click a pinned button for its menu: show its name or only its icon, show it on the bar and in
the palette or on one of them, move it left or right, or unpin it. In the Tracker the pinned
commands are a column of buttons in the header (up to ten; every pin is also in the palette).

## Settings > Layouts

The Layouts page in Settings is the full editor. Choose a layout at the top; the page shows:

* **Control bar**: every part in its group with its switch, its order and **Always shown**, the
  order in which parts leave a narrow window, and **Reset the bar**.
* **Panels**: for the Clips, Smooth and Tracker layouts, the pane each window lives in. Choosing
  another pane is the same as dragging the window's tab there; **Back home** returns it. The main
  area keeps its own view. In Studio the windows dock freely, so you arrange them by dragging their
  tabs.
* **Workspaces**: every workspace with its key, how many windows it opens and what it keeps, with
  **Duplicate** and **Close**. **New from the current arrangement**, **Add starter workspaces** and
  **Import from the project copy** add more.
* **Pinned commands**: each pin with where it shows (the bar and the palette, the bar only or the
  palette only), its label or icon, and its order. Type a command's name to pin it.
* **Switching**: show the workspace strip always, once a layout has two workspaces (the default), or
  never. The keys and View > Workspaces switch whatever you choose.
* **Zone keys** (Studio only): see below.

Your bars and pins are saved per person in your settings (`layout.<id>.toolbar`), never in a
project, and never sent to the people you collaborate with.

## Zone keys (Studio)

Zone keys jump between the parts of the Studio layout without the mouse. Turn them on in Settings >
Layouts > Studio (they are off at first). Then hold **Tab** and press a digit:

| Keys | Goes to |
|---|---|
| Tab+1 | The playlist |
| Tab+2 | The piano roll |
| Tab+3 | The mixer; 3 again for the channel rack, and again to swap back |
| Tab+4 to Tab+9 | Nothing at first: choose a window, or two, for each |

A jump opens the window if it is closed and gives it the keyboard; it never closes it. **Tab**
pressed and let go on its own goes back to the zone you were in before, so two presses swap between
your last two zones. Opening the browser does not count as a zone you were in.

While Tab is held the digit is taken by the jump: it never plays a note with the typing keyboard
on. Zone keys do nothing while you type in a text field, while a menu, popup, dialog, the command
palette or the welcome screen is open, while a plugin's own window has the keyboard, or while the
Stage view has it (Tab moves between its tracks there). With zone keys on, Tab no longer moves into
the next text field of a window (it still moves between fields while you are typing in one).

The Clips and Tracker key sets give plain Tab its own work (switching the view, the next track), so
the setting is greyed while one of them is chosen.

### A copy in the project

**View > Workspaces > Keep a copy in the project** stores every layout's workspaces in the project
as well, refreshed each time you save. Use it to take your arrangements to another computer, or to
hand them to someone who opens the project. When the project opens, a layout takes the copy only
where the person opening it has no workspaces of their own there (one or none); their own
workspaces always win (**Import from the project copy** on the Layouts page takes the copy's anyway).
Turn the item off to stop writing the copy. Projects saved without it, and
projects from older versions, open exactly as before.
