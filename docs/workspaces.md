# Workspaces and pane tabs

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
roll). Ctrl+Alt+Page Down and Ctrl+Alt+Page Up step to the next and previous workspace.

Workspaces are saved per person in your settings (`layout.<id>.workspaces`), and by default never in
a project. A workspace remembers which windows are open and, in the region
layouts, the pane sizes, tabs and which window each pane shows; Gridline rebuilds the layout from
that, so switching never loads an old window file. Settings and other dialogs are not part of a
workspace.

### A copy in the project

**View > Workspaces > Keep a copy in the project** stores every layout's workspaces in the project
as well, refreshed each time you save. Use it to take your arrangements to another computer, or to
hand them to someone who opens the project. When the project opens, a layout takes the copy only
where the person opening it has no workspaces of their own there (one or none); their own
workspaces always win. Turn the item off to stop writing the copy. Projects saved without it, and
projects from older versions, open exactly as before.
