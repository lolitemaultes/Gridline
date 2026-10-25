# Detachable windows (J-113, J-114, J-116: detached with the button only, live resize; J-117: elastic edges inside)

Gridline's panels (mixer, channel rack, playlist, piano roll, browser, automation and event
editors, undo history, …) and Gridline's own plugin editors can live in OS windows of their own,
on any monitor. The code is `src/app/Viewports.*`, `src/app/LiveResize.*`,
`src/core/WindowPlacement.h`, `src/core/LiveResize.h` and small hooks in `main.cpp`,
`DialogLayout.cpp` and `Shell.cpp`.

## The model (J-116)

- **Only the title-bar button detaches.** Every dockable panel and Gridline editor has a pop-out
  button; that is the only way out of the main window.
- **Dragging is exactly as without detaching.** A window dragged by its title bar or tab docks
  (dock targets, centre = tab, sides = split), Shift keeps it floating, and it stays a part of the
  main window wherever the pointer goes, including past the edge (J-117: held by the elastic
  boundary below, never more than a few dozen pixels out). Releasing it outside does nothing
  special: it springs back inside the main window. (J-114's "detach on drop" is gone.)
- **A detached window is an ordinary OS window**, decorated by the OS: its OS title bar moves it,
  its edges and corner resize it (never below the panel's minimum size, `style.WindowMinSize`
  enforced as the OS minimum and by ImGui's constraints; fixed-size editors, `AlwaysAutoResize`,
  are created non-resizable). Its contents re-lay out and redraw while it is being resized (live
  resize, below). Gridline never moves it; ImGui only reads where the OS put it.
- **Moving never docks.** Moving a detached window, by its OS title bar or by Gridline's own title
  bar or tab, never docks it, even when it is released over the main window. Gridline's own title
  bar does not move a detached window at all (ImGui's move is dropped every frame; clicking it
  still focuses the window); the panel name stays visible there.
- **The ways back in**: the dock-back button ("Dock back into Gridline"), the window's OS close
  button (below), View > Layout > "Bring all windows back into Gridline", Reset layout, and turning
  "Allow windows outside the main window" off.
- **Closing a detached window docks it back.** The OS close button (macOS red button, Windows /
  Linux X, Cmd+W while it has the focus: SDL's Window menu "Close" is `performClose:`) docks the
  panel back to where it was, exactly like the dock-back button; the panel stays open. Gridline's
  own close button (the X in its title bar, a tab's X or a dock node's X) does the same. Closing
  the main window quits Gridline as before.
- **"Where it was"** is remembered continuously while a panel is inside: the dock node it was in,
  else the split node it came from with its side and share (a panel that was the only window of
  a split is docked back on the same side at the same share), else the central node; a window
  that floated inside comes back to that floating place.

### The pop-out / dock-back button

Every dockable panel and Gridline editor shows one small monochrome button in its title bar
(floating) or at the right of its tab bar (docked): a window frame with an arrow leaving it
("Pop out into its own window") or, once detached, coming into it ("Dock back into Gridline").
Tooltips go through `tr()`.

- Inside, a click pops the panel out: at its last place outside (`layout.ini`, recovered onto a
  visible monitor), else with its current size beside the main window (right, else left, at the
  panel's height), else centred on another monitor, else (the main window fills the only
  monitor) offset inside the main window's monitor.
- Outside, the same button docks it back where it was.
- Hidden when "Allow windows outside the main window" is off or detaching is unavailable
  (native Wayland, the plugin build).
- Docked panels use ImGui's dock-node window-menu slot (`style.WindowMenuButtonPosition` is set
  to Right only while the dock nodes update, so nothing else changes): Gridline replaces the
  menu (`ImGuiContext::DockNodeWindowMenuHandler`) by the toggle and draws its icon over the slot.
  Floating windows get an extra title-bar button left of the close button, placed from the title
  bar as ImGui laid it out and drew it this frame (`laidOutTitleBar`: `InnerRect` less the
  decorations, which `SetWindowPos` never moves), not from `window->Pos`.

### J-117: the button that came away from the title bar

Dragging the floating channel rack past the main window's edge drew its pop-out button up to
~60 px to the right of its close button, over whatever was beside it. The rack had a clamp of its
own (`shell::keepFloatingInView`, keep 160 px of it inside) that ran *after* the rack's `Begin()`:
ImGui had already drawn the title bar and close button where the pointer put the window, then the
clamp moved `window->Pos`, and the button, drawn at the end of the frame from `TitleBarRect()`
(`Pos`-based), landed where the clamp had put the window. The next frame ImGui moved the window
back under the pointer, so the drawn window never followed the clamp while the button always did.
Fixed twice over: the late clamp is gone (the elastic boundary below replaces it, before layout),
and the button is placed from where the title bar was actually laid out.

## Inside the main window: the elastic boundary (J-117)

Floating windows inside the main window stay in its work area (between the top bar and the
status bar) with an elastic ("rubber") edge, the feel of J-67's elastic overscroll:

- **Which windows**: every floating panel or editor (anything that docks or pops out: the channel
  rack, the mixer or piano roll undocked, Gridline's plugin editors), a floating dock node holding
  several of them, and the J-91 tracked windows (dialogs, channel settings, ...). Not popups,
  menus, tooltips, combo lists, docked windows, fixed (`NoMove`) windows, Gridline's chrome, nor
  detached windows (the OS moves those). The same with "Allow windows outside the main window" on
  or off.
- **While dragged** past an edge, the part of the drag past it is damped by J-67's rubber band,
  `rubber(s) = M·a / (a + M)` with `a = 0.55·s` and `M = 48 px`: 100 px past gives ~26 px, 400 px
  ~39 px, never 48. It is recomputed from the pointer every frame (ImGui puts the window under the
  pointer, Gridline damps the overshoot), so nothing accumulates and moving back unwinds it 1:1.
  Dock targets follow the pointer, not the window, so docking (also on the targets by the main
  window's edges) works as before; the J-113 late-create filter still keeps the dragged window
  from getting an OS window of its own.
- **Let go** (or when the work area shrinks under a window: the main window resized), it springs
  back: each frame leaves `exp(-dt / 0.055 s)` of the distance (J-67's spring-back constant), so
  the motion is the same at any frame rate; the last half pixel snaps (40 px out: ~0.25 s). The
  spring keeps its own unrounded position (ImGui truncates window positions; a spring stepping
  from those could stall a pixel short).
- **Larger than the work area**: its top-left corner (title bar and left side) is kept in instead.
- **Just opened** (an editor, a dialog, a window restored by a layout) outside the area, or put
  there by code (moved or resized: a layout applied, the default rack placement, a dialog sizing
  itself to its content): placed inside at once, before it
  is drawn, no animation. Only a window let go past the edge, or left outside by a shrinking work
  area, springs.
- **Elastic edges off** (Settings > Interface "Elastic edges", or the system asks for reduced
  motion): a hard edge, no stretch, no spring.
- **Where**: the maths is `src/core/WindowRubber.h` (pure, `tests/test_j117.cpp`); the per-window
  state and the choice of windows are in `ui::ClampTrackedWindows` (`src/app/DialogLayout.cpp`).
  It runs inside `ImGui::NewFrame()`, right after ImGui moved the dragged window and before
  anything is laid out, through `ImGuiContext::WindowsMovedHook` (`GRIDLINE PATCH (J-117)`):
  ImGui lays floating dock nodes out inside `NewFrame()` itself, and a node moved after that
  draws its tab bar a frame behind where it is. (`ClampTrackedWindows()` after `NewFrame()`
  installs the hook and does the work itself the first frame.)
- **Plugin interfaces**: installed plugins' own (VST3 / CLAP / AU / LV2) interfaces open in OS
  windows of their own (`PluginWindow.cpp`), not inside an ImGui window of the main window; they
  are not affected and nothing follows an ImGui window.

## Live resize (J-116)

While a window is resized, some window systems run a modal loop inside the event pump: the main
loop is blocked there, nothing is drawn, and the OS shows the window's last frame stretched until
the mouse is let go. Gridline draws from SDL's event watch instead (`src/app/LiveResize.*`, the
gate in `src/core/LiveResize.h`, unit-tested in `j113_window_placement`):

- `main.cpp`'s frame (tick, interface, render, swap of the main and every detached window) is one
  function; the main loop calls it after each event pump, and `SDL_AddEventWatch` calls it from
  inside the pump for a `SIZE_CHANGED` / `RESIZED` / `EXPOSED` / `MOVED` event of a Gridline window
  (main or detached, main thread only) when the gate says so:
  - never from inside a frame (re-entrancy: the frame resizes OS windows itself, which reports
    events synchronously) and never outside the main loop's pump;
  - only while the pump is blocked: the platform reports a modal size / move loop
    (`host::native::inModalSizeLoop`), or the pump has not returned for 50 ms;
  - a size event that changes the window's size from the size it was last drawn at draws at once;
    other events (the platform's live-resize timers, a modal move, SDL's `SIZE_CHANGED` + `RESIZED`
    pair and the view's `EXPOSED` for one step) only if 14 ms passed since the last frame began
    and 5 ms since it ended: one frame per size step, ~60 frames a second while the window is held
    still. (The second condition matters when a frame takes longer than 14 ms, on a loaded
    machine or with software GL: the step's own later events then arrive as its frame ends, and
    each drew another frame of the same size.)
- The ImGui backend sees the event before the frame (it acts on `RESIZED` only, so `SIZE_CHANGED`
  is passed to it as `RESIZED`): a detached viewport reads its new size, the main window reads
  `SDL_GetWindowSize` / `SDL_GL_GetDrawableSize` as every frame does, so the frame uses the new
  framebuffer size and the Retina density of each window.
- A frame drawn from the watch skips the J-66 frame-clock wait. Audio is untouched (the audio
  thread never waits for the interface; the frame only does what a main-loop frame does).
- `GRIDLINE_NO_LIVE_RESIZE=1` turns the watch off (the behaviour before J-116, for comparisons).

| Platform | What blocks (SDL 2.30 / 2.32 source) | What Gridline does |
|---|---|---|
| macOS (Cocoa) | A mouse-down on a window edge enters AppKit's live-resize tracking inside `[NSApp sendEvent:]`, inside `SDL_PumpEvents`, until the release. SDL sends `MOVED` + `RESIZED` (+ `SIZE_CHANGED`) from `windowDidResize:`, `EXPOSED` from `updateLayer` and, in 2.32, from a 60 Hz live-resize timer. | `inModalSizeLoop` = some window `inLiveResize`: one frame per step from the watch. Verified by running (real AppKit live resize of the main and a detached window). |
| Windows | `DefWindowProc`'s modal size / move loop (`WM_ENTERSIZEMOVE` … `WM_EXITSIZEMOVE`) inside `DispatchMessage`. SDL sends `RESIZED` from `WM_WINDOWPOSCHANGED` and `EXPOSED` from a 10 ms `WM_TIMER` it starts on `WM_ENTERSIZEMOVE`. | `inModalSizeLoop` = `GetGUIThreadInfo` `GUI_INMOVESIZE`: one frame per size step, and every other timer tick while moving / held still. Verified from the source and the shared gate test (no Windows machine here). |
| Linux / BSD X11 | Nothing: `ConfigureNotify` arrives in the pump (`RESIZED` / `MOVED`), which returns. SDL2 has no `_NET_WM_SYNC_REQUEST` support, so the window manager does not wait for a frame of the new size either. | The gate never draws from the watch (pump not blocked, no modal loop): the main loop draws the latest size right after the pump, one frame per loop, no double frames. Verified from the source and the gate test. |
| Linux Wayland | Nothing: `xdg_toplevel` / libdecor configure events are handled in the pump (`Wayland_HandleResize`: `RESIZED` + `EXPOSED`, then `ack_configure`); the compositor shows the new size when the next frame is committed. | Same as X11 (detaching is unavailable there; the main window). Verified from the source. |

## Why J-113's live tear-off broke on macOS (investigation, J-114)

J-113 let ImGui tear a window off *while dragging*: the frame a dragged window stopped fitting the
main window, ImGui created its OS window, and from then on moved that OS window under the pointer.
Driven on a real (non-hidden) Mac with synthetic Cocoa mouse events dispatched to the main window
(`nsmouse` dev command, `[NSWindow sendEvent:]`), logging SDL's events, SDL's keyboard / mouse
focus and the real cursor (`SDL_GetGlobalMouseState`) every frame (`GRIDLINE_VP_TRACE=1`):

1. The mixer's tab dragged right from (1129, 64): at (1200, 140), *pointer still inside the main
   window*, the window's rect left the main window and ImGui created its viewport.
2. The SDL2 backend creates the viewport of a window being moved with no "no activation" flag,
   so `SDL_ShowWindow` **activated** the new window: main window `FOCUS_LOST` + `LEAVE`, new
   window `FOCUS_GAINED`.
3. On Cocoa, SDL ties mouse capture to the keyboard-focus window. AppKit kept delivering the drag
   to the main window (the drag started there), which was no longer captured: motion past its
   edge was clamped (x 1439) and then dropped (1500, 1550, 1600 never arrived).
4. The release arrived with `windowID 0` (no mouse focus); the ImGui backend ignores it. ImGui
   still thought the button was held: the mixer stayed `g.MovingWindow` for the rest of the run,
   its OS window stuck at OS x 1651 while the pointer was at 1840, keyboard focus on the new
   window. To the user: "it still treats it as the same window".
5. Hidden test runs never show or focus windows and the scripted pointer bypasses SDL, so the
   J-113 tests could not see it.

J-114 therefore never created an OS window during a drag; J-116 never creates one because of a
drag at all: the platform window appears only from the button, or at start-up from `layout.ini`,
and an OS window is never moved by ImGui while the mouse is down.

## Mechanism

Approach (a) of the J-113 brief: ImGui multi-viewports stay (`ImGuiConfigFlags_ViewportsEnable`,
the vendored `imgui_impl_sdl2` + `imgui_impl_opengl3`), but a platform window is created only when
a window is detached.

- **Detached = a window class flag.** A detached window carries
  `ImGuiViewportFlags_NoAutoMerge` in `window->WindowClass.ViewportFlagsOverrideSet` (all windows
  of a floating dock node). ImGui then keeps it in a viewport of its own
  (`GetWindowAlwaysWantOwnViewport`) and never merges it back by itself. Docking back clears it;
  a panel docked into the main window never carries it (or it would tear off the moment it is
  next undocked). Windows restored detached from `layout.ini` get it again on their first frame.
- **No tear-off by dragging.** One small patch in the vendored ImGui (`GRIDLINE PATCH (J-113)`,
  `ImGuiContext::ViewportLateCreateFilter`; J-116 still needs it, unchanged): a window that stops
  fitting the main window gets its own OS window only if it is a popup / menu / tooltip, or if it
  was opened outside on purpose (centre outside the main window within the frames it takes to
  appear and centre itself: a dialog opened from a detached window). Dragged, released or grown
  windows stay in the main window.
- **No ImGui moves of detached windows.** Before and after `NewFrame`, a `g.MovingWindow` whose
  root is detached is dropped (the title bar's id stays active until the release; nothing happens
  then). So a detached window shows no dock targets and never docks by being dragged.
- **Close.** The SDL2 backend turns `SDL_WINDOWEVENT_CLOSE` of a viewport's window into
  `viewport->PlatformRequestClose`; ImGui would close the window (`*p_open = false`) in `Begin`.
  `beginFrame` (before `NewFrame`) clears it for a detached panel or dock node and docks it back
  instead; other viewports (a dialog over a detached window) close as ImGui does. Gridline's own X
  goes through `g.CloseRequestFilter` (a Gridline patch to ImGui at the three close buttons: title
  bar, docked tab, dock node): for a detached panel it queues the same dock-back and ImGui does not
  close it.
- **Docking back** queues `DockContextQueueDock` on the remembered node (or the remembered split
  node with side and ratio, or the central node of `GridlineDock`); a window that floated inside
  is moved back there and put into the main viewport at once (ImGui's own merge waits until no
  widget is active and no other OS window overlaps).
- **Coordinates.** Gridline lays out in logical pixels (`window size / uiScale`), so the
  platform callbacks are wrapped and ImGui's coordinate space is "OS coordinates relative to the
  main window's top-left, divided by the interface scale":

      imgui = (os - mainWindowOs) / uiScale        os = mainWindowOs + imgui * uiScale

  The main viewport stays at (0, 0) with the size it always had, so everything inside the main
  window (layout, dev-input coordinates, screenshots) is unchanged. When the main window moves,
  detached windows re-read their OS position (nothing moves on screen).
- **Rendering.** `UpdatePlatformWindows()` + `RenderPlatformWindowsDefault()` after the main
  window is rendered: each detached window has the backend's own GL context shared with the main
  one (textures shared), swap interval 0 (the main window paces the frame); the main context is
  made current again before the main window swaps. `viewports` dev dump: `windows-ms` is the time
  spent on detached windows per frame.

## Windows, ownership, macOS choice

- Decorated (`io.ConfigViewportsNoDecoration = false`) and resizable OS windows; popups, menus
  and tooltips stay undecorated (ImGui). `ConfigViewportsNoTaskBarIcon = true`: one taskbar entry.
- Windows: owned by the main window (`GWLP_HWNDPARENT`: above it, minimised with it). X11:
  transient for the main window. Both via `host::native::keepAbove`.
- **macOS: ordinary windows of the Gridline app, kept above the main window by ordering**, not
  AppKit child windows. Child windows are dragged along whenever the main window moves (a panel
  put on another monitor would follow the main window around) and AppKit constrains them with the
  parent. Instead, each time the main window comes forward Gridline orders every detached window
  just above it without activating it (`host::native::orderAbove`, `orderWindow:NSWindowAbove
  relativeTo:`). They come forward with the app, Cmd-` cycles them, moving the main window leaves
  them where they are.
- Closing the main window quits (detached windows close with it, the layout is saved: they reopen
  detached next time only if they were left detached). Minimising it hides them; restoring shows
  them again without stealing the focus.

## Input

- Keys: SDL key events carry the detached window's id; Gridline's shortcuts (Space / the J-112
  play rule, undo, save, …), the typing keyboard and text fields work from every window.
- Drag and drop: ImGui payloads are global (browser → rack, J-97 drop-to-replace, playlist
  drags across windows). Files from other applications arrive through `SDL_DROPFILE` on any
  Gridline window; the macOS native drop target (hover highlight, promises, links) stays on the
  main window.
- J-109 focus rule: live keys are released only when **the whole application** loses the focus:
  a focus loss is acted on only if no Gridline window gains it within 80 ms.

## Persistence

- `layout.ini` in the user config folder (never in the project, never synchronised): ImGui's
  windows (a detached window's viewport position and size, ImGui units) plus
  `[GridlineViewports]` sections:
  - `[Main]` `MainOs=x,y` `Scale=s`: the main window's OS position and the interface scale the
    coordinates are relative to; at start-up they are re-anchored to the current main window.
  - `[Home:<window>]` `Dock=0xnode,0xparent,dir,share` or `Float=x,y,w,h`: where it docks back.
  - `[Out:<window>]` `Rect=x,y,w,h` (OS units): its last place outside, for "Pop out".
- Monitor loss: a window whose title bar would not be on a current monitor is moved onto the
  monitor it overlaps most (or the nearest) and fitted inside it (`place::recoverOnScreen`).
- Settings > Interface > "Allow windows outside the main window" (`ui.detachWindows`, on by
  default). Off: every window is brought back, the pop-out buttons are hidden and viewports are
  disabled, exactly as before J-113; started with it off, windows saved outside open inside.
- `layout.ini` is read once the main window has its final position (J-113 fix: it used to be
  written but never read). The dev layout options (`--show`, `--hide`) start from the default
  layout as before.

## Platforms

| Platform | Detaching | Live resize | Notes |
|---|---|---|---|
| macOS (Cocoa) | yes | event watch (AppKit live resize) | points; per-window Retina density; kept above the main window by ordering |
| Windows | yes | event watch (modal size / move loop) | owned windows; one logical scale |
| Linux / BSD X11 | yes | main loop (not blocked) | transient-for the main window, skip-taskbar |
| Linux Wayland (native) | **no** | main loop (not blocked) | SDL2 cannot place windows globally there; the backend offers no viewports |

Wayland: Gridline does not force XWayland. SDL2 already picks X11 when both exist; when the user
runs the native Wayland driver (`SDL_VIDEODRIVER=wayland`) detaching is unavailable and Settings
says so ("Start Gridline with SDL_VIDEODRIVER=x11 to use it through XWayland"). The plugin build
(Gridline inside a host) has no detached windows (stubs in `src/plugin/HostedWindows.cpp`).

## Tests and dev tooling

- `--hidden`: viewports stay on (the real code path), platform windows are created hidden, never
  shown or focused; their positions are kept virtually. Off-screen runs (`--screenshot` with
  `--size`) see two virtual monitors (the main window and one to its right), so runs are identical
  on every machine. `vpshot <window> <file>` renders one detached window to a PPM.
- Dev-input commands: `detach <window> x y [w h]` (popped out to exactly that place: a test
  set-up), `toggle | popout | dockback <window>` (the title-bar button), `titlebtn <window>` (where
  the button is, its state), `osmove <window> x y` (the OS moves it), `osresize <window> w h`,
  `osclose <window>` (its OS close button: `SDL_WINDOWEVENT_CLOSE`), `closebtn <window>` (where its
  own title-bar X is), `liveresize <window> w h
  [steps]` (resized in steps while the pump is blocked: the platform's size / expose events from
  inside the pump with the modal loop forced on; prints each step's size, the size it was drawn at
  and the frames drawn), `livestats` (frames drawn from the watch: count, time per frame, interval,
  size), `dock <window> <target>`, `gather`, `detaching on|off`, `viewports` (dump),
  `focusmove <window>|main`, `focuslost [window]`, `sdlkeyin <window> <key> down|up`,
  `dialogfrom <window> about|info`, `vpshot <window> <file>`; on macOS `nsmouse move|down|drag|up
  x y` (a real Cocoa mouse event dispatched to the main window, main-window logical px),
  `nsresize <window>|main dx dy [steps]` (its bottom-right corner dragged through AppKit's own live
  resize: a real modal loop; prints the OS size before / after and the frames drawn meanwhile) and
  `nsclose <window>` (`performClose:`, the red button). `GRIDLINE_VP_TRACE=1` logs SDL mouse /
  window events, focus, the real cursor and every viewport each frame.
- `j113_window_placement` (unit): recovery onto monitors, re-anchoring, reachable title bars,
  pop-out placement (beside, other monitor, offset), split ratios, the `layout.ini` lines, and the
  live-resize gate (one frame per step, none inside a frame or outside the pump, none where the
  pump is not blocked, timer frames while held still).
- `j116_detach_app` (real app, hidden; replaces `j114_detach_app`): the mixer dragged past the
  edge and released outside stays in the main window; the button pops it out (decorated,
  resizable, beside the main window, the panel's size); OS resize (and minimum size); OS move over
  the main window and its own title bar dragged there: no move, no docking; the dock-back button;
  popped out again at the last place outside; `osclose` docks the mixer back where it was and a
  floating channel rack back where it floated, both still open, Gridline still running; live
  resize: 6 steps, each drawn exactly once at its own size; a fixed-size editor; "Bring all
  windows back"; no button with detaching off.
- `j113_detach_app` (real app, hidden): Space and the typing keyboard from a detached window,
  focus moving between Gridline windows keeps held keys, focus lost to another application
  releases them; J-97 drops into a detached rack; dialogs over the window they come from;
  `layout.ini` persistence (including Home / Out), re-anchoring, recovery, the setting off.
- Both app tests report `SKIP: <reason>` where windows cannot be created outside the main one.
- `j117_window_rubber` (unit): the resting range (a window larger than the area keeps its
  top-left in), the damped drag (1:1 inside, monotonic, always less out than the pointer, bounded
  by 48 px), the spring (no overshoot, the same duration at 30 / 60 / 144 / 240 fps, one 50 ms step
  = five 10 ms steps), hard edges with elastic edges off. `dialog_layout` checks the same on a
  headless ImGui window dragged against the top bar.
- `j117_rubber_app` (real app, hidden, 16 ms frames): the rack dragged far past each edge is out
  by less than 48 px, its pop-out button stays beside its close button (`closebtn` also prints
  where the window was laid out and where it is at the end of the frame: equal), three frames
  after the release it is still springing, by frame +40 it rests on the edge; `--resize 1100x700`
  brings it back inside; docking by drag on the dock space's outer right target and on the
  browser's centre; the mixer dragged out by its tab past the edge stays bounded and attached; a
  floating dock node of two tabs past the edge is bounded with its tab bar laid out where it is
  (the `viewports` dump prints `laid x y` per window) and its pop-out button in place.

## Known limitations

- Wayland native driver: no detaching.
- Windows / X11: one logical interface scale for all windows (the main window's).
- macOS: a detached window is put back above the main window whenever the main window comes
  forward; clicking the main window while a detached one overlaps it covers it only until then.
- The native macOS drop highlight / file promises work on the main window only.

## Manual checklist (macOS; ideally two monitors)

1. Default layout. Drag the Mixer tab past the right edge of the main window and let go outside:
   it stays a part of the main window (it follows only a little way past the edge, with
   resistance, and springs back inside when let go); no new window appears. Docking and Shift
   behave exactly as before. Drag the channel rack hard against each edge: its pop-out button
   stays next to its X.
2. Click the mixer's pop-out button (right of its tab bar; tooltip "Pop out into its own
   window"): it opens beside the main window (or where it was last outside), with an OS title bar.
3. Resize it by its edges and corner: the mixer re-lays out and redraws continuously while you
   drag (no stretched picture), and cannot get smaller than the panel allows. Resize the main
   window the same way: it redraws continuously too. Open a channel's settings, pop it out: its
   window cannot be resized.
4. Move it by its OS title bar over the main window and let go: it stays a separate window. Drag
   its own Gridline title bar (the "Mixer" bar inside): it does not move and does not dock.
5. Click its red close button (or press Cmd+W while it is focused): it docks back where it was and
   stays open. Pop it out again and click the X in its own Gridline title bar: the same. Pop it out again: it opens where it last was outside. Its "Dock back into
   Gridline" button docks it back the same way.
6. With the mixer and the rack detached: Space starts / stops playback from each; Cmd+Z / Cmd+S
   work; typing in a text field types. Typing keyboard on: hold Z, click between the main window
   and the detached rack: the note keeps sounding; switch to another app: it stops.
7. Drag a sample from the browser onto a detached rack row: it replaces the row's sound (J-97).
8. Click the main window: the detached windows stay above it. Move the main window: they stay
   where they are. Minimise the main window: they hide; restore: they come back.
9. Quit with windows detached (closing the main window), restart: they reopen where they were.
   Unplug the second monitor and restart: they open fully visible on the remaining one.
10. View > Layout > Bring all windows back into Gridline: everything docks back.
11. Settings > Interface: turn "Allow windows outside the main window" off: everything is inside,
    no pop-out buttons, dragging past the edge is elastic as with it on.
