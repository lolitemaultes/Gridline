# Changelog

All notable changes to Gridline. Versions follow `project(VERSION)` in `CMakeLists.txt`; the
release workflow publishes the section of the tagged version as the release notes.

## Unreleased

### New: Plasma, a wavetable hybrid synth (`gl.plasma`)

- Three oscillators (wavetable, multisample, sample, granular or spectral) with two warp slots
  each (103 modes, among them FM / PD / AM / RM between sources and spectral warps), unison up to
  16 voices, a sub oscillator, a noise layer (19 noises or your own file) and two filters with 106
  types. Each source picks the filter(s) it feeds or goes straight to the effects.
- Modulation: 4 envelopes, 10 LFOs (drawn shapes with loop points, path and chaos modes, sample
  and hold), 8 macros, velocity and note curves, MPE and more as sources; a 64-slot matrix with
  curves, smoothing and an auxiliary source. Drag a source onto any knob, or use the knob's menu.
  Each source's drag handle on the ENV, LFO, VELO / NOTE tabs and the macros is a small live
  meter of its current output.
- Three effect racks with up to 24 modules (distortion, flanger, phaser, chorus, delay,
  compressor, reverb, EQ, filter, hyper / dimension, frequency shifter, convolution, utility and
  band splitters), an arpeggiator with 12 pattern slots and a clip player with 12 clips (piano
  roll, automation lanes, recording).
- A wavetable editor: harmonics and phases, brushes, snap grid, processing, morphing between
  frames, formulas, WAV import and export.
- Opens Serum 2 presets (`.SerumPreset`) and Serum 1 `.fxp` files from your own folders and turns
  them into Plasma sounds. There is one Open path for every preset type (the preset menu's Open
  preset..., the browser, drag and drop, the plugin header's preset menu); what was approximated
  or not found stays with the sound behind an info mark next to the preset name. The browser's
  Serum library lists the Serum 2 and Serum preset folders found on this computer (and folders you
  add), indexed in the background, grouped by bank and searchable as a whole. Projects that host
  the original plugin keep hosting it. See `docs/plugins/plasma.md`.

### Stand-ins for a missing Serum or Kickstart

- A project (Gridline or FL Studio) whose Serum or Serum 2 is not installed plays it with Plasma;
  a missing Kickstart or Kickstart 2 plays with Tideline. The plugin's saved state is mapped onto
  the stand-in, automation and controller links follow, and the original plugin with its state is
  kept untouched: once it is installed, the project opens with it again. Installed plugins are
  never substituted. The plugin window notes the stand-in, and Replace with Plasma / Tideline
  keeps it for good. See `docs/plugins/plasma.md` and `docs/plugins/tideline.md`.

### Plugin windows: the channel strip hides

- The strip at the top of a plugin's channel window (volume, pan, pitch and the preview keyboard)
  is now hidden by default, which leaves more room for the plugin's editor. The eye button at the
  right of the window's header row (the same eye the mixer uses to show and hide a plugin's
  window) shows it: it slides down in about 140 ms and slides back up when you hide it again. The
  knobs and the keyboard work exactly as before while it is shown.
- The choice is global and remembered (Settings key `ui.channelStrip`): every plugin window opens
  with the strip shown or hidden the way you left it. With Interface animations off, or when the
  system asks for reduced motion, it switches at once.
- The window grows and shrinks with the strip, docked or popped out into its own window. Installed
  plugins (VST3, CLAP, AU, ...) that open their own interface do so in a window without this strip,
  so their view never moves.

### Undo in plugin editors: one step per gesture

- Ctrl+Z (Cmd+Z on macOS) undoes, and Ctrl+Shift+Z / Ctrl+Y redo, what you did in any Gridline
  plugin editor, also in a plugin window popped out of the main window and while the keyboard is
  in that window. Text fields keep their own undo while you type in them.
- Every gesture is one undo step, whatever the editor: a knob or slider drag, a turn of the mouse
  wheel over a control (the steps of one turn), a typed value, a menu pick, one stroke on a drawn
  shape (Plasma's LFO shapes and curves, wavetable editor, clip and arp editors), an effect added,
  removed or moved in a rack, a preset loaded. Before, two strokes on the same shape could merge
  into one step, one stroke could leave two steps (a parameter and the shape apart), and each
  wheel step was a step of its own.
- External plugins (VST3, CLAP, AU, VST2): an edit gesture in the plugin's own interface and the
  plugin state it reports right after are one undo step, and the plugin following an undo or
  redo no longer adds a step that threw the redo history away.
- In a collaborative project undo still takes back only your own changes, plugin edits included.

### Collaboration: rename in place

- The Collaboration panel no longer has a Rename... button: the project's name at the top is the
  field. A faint box shows around it on hover; click it to edit, then Enter or clicking anywhere
  else renames the project for everyone, and Escape keeps the old name.

### New: windows outside the main window

- Every panel (mixer, channel rack, playlist, piano roll, browser, ...) and every Gridline plugin
  editor has a small pop-out button in its title bar or tab bar: "Pop out into its own window". It
  opens where it was last outside, or beside the main window with the panel's size. The button is
  the only way to detach: dragging a window by its title bar or tab works exactly as before
  (docking, dock targets, Shift keeps it floating), even past the edge of the main window.
- A detached window is an ordinary window of your system: move it by its title bar, resize it by
  its edges and corner (never smaller than the panel allows; fixed-size editors stay fixed). Its
  contents follow the resize live, on every system. Moving it over the main window never docks it.
- To bring it back: its "Dock back into Gridline" button, or its own close button (the red button
  on macOS, X on Windows and Linux, Cmd+W): either docks it back where it was, it stays open.
- Space, shortcuts, the typing keyboard, text fields and drag and drop (browser items, files from
  other applications, J-97 drop-to-replace on the channel rack) work in detached windows. Moving
  between Gridline's windows no longer releases held keys; switching to another application still
  does.
- Detached windows reopen where they were, on the same monitor (saved with your window layout,
  never in the project); a window left on a monitor that is gone comes back on one that is there.
  View > Bring all windows back into Gridline returns them all; minimising the main window hides
  them. Dialogs open over the window they were opened from.
- Settings > Interface > "Allow windows outside the main window" (on by default). Off: everything
  stays inside, exactly as before. Not available with the native Wayland driver (XWayland works)
  or inside a plugin host.
- Plugin editor windows (VST3, CLAP, AU, ...) reopen on the monitor you left them on.
- Fixed: the window layout (panel sizes, docking, the rack's position) was saved but never
  loaded; it now comes back at the next start.
- Fixed: resizing the main window on macOS and Windows showed its last picture stretched until the
  mouse was let go; it now redraws continuously while you resize.
  On a slow or busy computer each step of a resize no longer draws the same picture several times
  over.

### Windows inside the main window stay inside it

- Floating windows inside Gridline (the channel rack, the mixer or piano roll when undocked, a
  group of tabs floating together, plugin editors, dialogs) can no longer be dragged off the edge
  and lost. Dragged past an edge of the work area (the top bar, the status bar, the sides) they
  follow with growing resistance, a little way only, and spring back inside when let go, the same
  elastic feel as scrolling past the end of a list. A window larger than the work area keeps its
  title bar and left side in reach. Making the main window smaller brings the windows back inside
  it. Docking by dragging works as before, also on the targets by the main window's edges. With
  "Elastic edges" off (Settings > Interface, or reduced motion) the edge is a hard stop. Windows
  outside the main window (popped out) are not affected.
- Fixed: the pop-out button of the channel rack came away from its title bar (drawn up to ~60 px
  to the right of the close button) while the rack was dragged past the edge of the main window.

### Playlist: paint the clip you clicked, clearer crossfades

- Clicking a clip in the playlist now makes that clip, as it is, what the draw and paint tools
  place. A resized or trimmed audio clip paints with its length, its start in the sample, its fades
  and fade curves, gain, pitch, stretch, tempo sync and reverse; pattern and automation clips keep
  their length and offset. Painted copies are new clips: not muted, in no group. Before, a click
  with the draw tool placed the whole sample at its full length.
- Picking a pattern, sample or automation in the picker (or the source menu) still places it at
  its natural length, with default settings.
- Where two audio clips on a track crossfade, the overlap now shows both clips blended the way
  they play: the outgoing clip's waveform fades out as the incoming one fades in, and the colour of
  the body and title bar moves from one clip to the other, so the pair reads as one cell with no
  edge or outline in the middle. The two fade curves are drawn as faint lines. A selected clip
  still shows where it starts and ends. Only the drawing changed; the crossfade sounds as before.

### New: collaborative projects (preview, J-111)

- Work on one project together, live, on the same network or across the internet, peer to peer:
  no server and no account. Each person keeps their own windows and transport; channels, notes,
  the playlist, the mixer, automation, Gridline plugin settings and audio files sync in a fraction
  of a second. See docs/collaboration.md.
- Invite with a one-time code that expires (15 minutes to 24 hours); the invitee pastes it in File >
  Join collaborative project... and chooses where the project folder goes. A reply code covers
  routers that need both sides to open the connection. Members reconnect by themselves, through
  each other, or with a reconnect code.
- Connections: direct (same network, IPv6), hole punching (ICE with STUN), automatic router port
  opening (UPnP-IGD, NAT-PMP, PCP; can be turned off), through another collaborator, or an optional
  TURN relay. "Test my connection" reports the router's behaviour, port opening and IPv6.
- End-to-end encrypted (X25519, XChaCha20-Poly1305), changes signed by their authors (EdDSA),
  safety codes and fingerprints to compare. Nothing goes anywhere but to invited people and the
  STUN / TURN server you configure.
- The project saves itself continuously (a crash-safe log with compaction) and stays a normal
  project folder; audio files are stored and sent once, by content, with a silent "Loading"
  placeholder until they have arrived and been checked.
- Colour-coded cursors in the windows you both have open, collaborators in the top bar,
  remote changes flash in their author's colour, "Follow their playhead".
- Collaborators' cursors (J-111d) point at the same thing on every screen whatever the window size,
  layout, interface scale, scroll or zoom: a playlist track and time, a piano roll key and time, a
  channel rack row, a mixer strip, or the button, knob or step under the pointer. They draw on top
  of every window, floating window, popup and menu, fade out quickly where that spot isn't shown
  (a closed or hidden window, another tab or pattern, scrolled away) and back in where it is. The
  cursor is a plain pointer arrow in the collaborator's colour with a thin white rim and a soft
  shadow, sharp at any scale; the name tag is gone (the colour says who it is).
- Collaborators' cursors and pictures (J-111g): the arrow is the size of your own mouse pointer
  (macOS, Linux and Windows pointer size settings) with softer, rounded corners and a thinner
  outline; it no longer blinks out and in while someone moves between windows (it waits a moment
  before fading, and moves to another window without fading); a plugin editor you have open hides
  the cursors of people who are not in that same plugin. Collaborators' profile pictures now
  actually arrive (they were announced but never sent), update when changed, and are kept for
  offline members and reopened projects (also when you join and someone's greeting reaches you
  before their membership record: their picture was shown but never kept).
- Sound stays on each computer: playback, held keys, previews, plugins playing on their own,
  recording, solo, record arm, input monitoring and plugins' listening switches are never shared,
  and a collaborator's edits never make sound on yours.
- Undo undoes only your own changes. Offline work is normal: changes travel the next time two
  computers that have them are online together, also through a third collaborator. Changes made to
  the same thing while apart go to the admin's Sync review (who, when, before and after, each side's
  change log, preview, keep, restore later).
- Settings > Collaboration: display name, optional contact email (shared only if you allow it),
  profile picture (cropped, made small and stripped of metadata), colour, identity fingerprint,
  connection settings.
- Only Gridline's own plugins in a collaborative project: projects with VST3, VST2, CLAP, AU or LV2
  plugins can't become collaborative (each one is listed), and such plugins are hidden and refused
  in a collaborative project.
- Collaboration needs the newest Gridline: behind the newest release, the buttons are greyed out
  with an Update button, and collaborative projects ask for the update before they open. Peers on
  different versions or builds are refused politely when they connect.
- The Collaboration panel, Join window and Sync review follow the Settings window's layout: section
  panels, a label column, short captions, lit primary and red destructive buttons. People are listed
  with their picture, "you" / "admin" badges, a status lamp (online direct / relayed, connecting,
  offline with the last sync) and Reconnect / Invite again for those offline; invites show their
  state (preparing, the code with a countdown, joined with a large safety code, expired); long
  folders are cut in the middle with the full path on hover; the Join window shows its steps.
- Removing someone (J-111e): the admin removes a member from the "..." menu in People, with an
  optional note. The removal is signed and checked on every computer: they can't connect, be
  relayed or join with old invites any more, and anything they change after the removal is dropped
  everywhere, while their earlier work stays (with their name in Sync review). With the project
  open they see "You were removed from ... by ..." with Keep a private copy / Close project
  (editing stops, playback doesn't); offline, they learn it the next time they connect. Removed
  (n) in People shows who, when and why, with Re-invite for the admin. The collaboration protocol
  is now version 2.
- What happens to the files (J-111e): Leave asks whether to keep the folder as a normal project or
  move it to the Trash, with sizes, and offers Wait for sync when some changes aren't confirmed
  yet. Stop sharing reaches everyone (also later, when someone offline reconnects) and is never
  undone by an old copy. Your collaborative projects marks removed, stopped and missing projects
  (Keep a private copy, Locate, Forget); a copied folder can't sync next to the original. This
  project shows the folder (Show in Finder / Files), its size and history, and Clean up unused
  audio; after 30 days without anyone it offers to make the project a normal one. Ending a project
  deletes the other members' cached details on this computer. docs/collaboration.md: "Removing
  someone" and "What happens to the files".
- `gridline collab-run` runs a collaborative session without a window (scripts and tests).
- Collaborative projects have a shared name (J-111f). "Make this project collaborative..." asks for it
  first (prefilled from the open project; any language, at most 80 characters). Any member renames
  it from the Collaboration panel's header, Project info or Project settings > Collaboration
  ("Rename..."). Online collaborators see the new name at once (window title, panel, top bar, a
  notice "Sam renamed the project to ..."); the others get it when they catch up. Renames made
  while apart on both sides go to the Sync review like other conflicting changes. A rename is one
  undo step. Each computer's project file is renamed to match (made safe as a file name; another
  file is never replaced; case-only renames work on macOS and Windows); folders keep their names
  unless you choose "Also rename the project folder on this computer". Exports, Recent files and
  Recent collaborative projects use the new name, and projects with the same name are told apart by
  their folder. A project file renamed by hand is still found.
- Sharing and renaming in place (J-111h): the Collaboration panel's "Share this project" section
  holds the name (prefilled, checked as you type) and the folder (next to the project file by
  default, Choose...) with a caption of what happens, and "Make collaborative": no separate name
  window any more. Rename... turns the panel's title, or the Shared name row of Project info and
  Project settings, into a name field with Rename / Cancel and the "Also rename the project folder
  on this computer" option under it. Fixed: a dialog that sizes itself to its content (the old
  name window) shrank by a pixel every frame for over a second after it opened: sections reserved
  one pixel less than their width.
- Collaborators' cursors go behind your windows (J-111h): every window you have open in front of
  the one a cursor points into covers it (Settings, Project info, the Collaboration panel, dialogs,
  the floating channel rack, mixer or piano roll, popups and menus), cut at the window's edge, not
  faded; in that same window it is drawn on top. Behind a modal it is dimmed with everything else.
- New third-party code: libjuice 1.7.4 and libplum 0.6.0 (MPL-2.0), Monocypher 4.0.2 (BSD-2-Clause
  or CC0), stb_image 2.30 and stb_image_write 1.16 (public domain or MIT); see third_party/NOTICE.md.

## 0.7.0

### New: Seltmann, the automatic sample chopper (`gl.seltmann`)

- One simple flow: drop audio on Seltmann (from Finder or Explorer, Tracklib, the browser, the
  playlist or the audio editor, or on a Seltmann row in the channel rack; or "Open in Seltmann" in
  the browser), it analyses on its own, pick a layout card and it plays. A calm main view: one
  waveform with the bars and the slices by letter, the tempo with x2 / /2, the chop size (Auto,
  1 bar ... 1/16), the tempo mode, play, drag handles, Make loops and Send to project, the layout
  cards and the pads. Everything advanced (slice inspector, manual cuts, steps and the step
  language, grid tools, sound, loop list) is in a collapsible Edit drawer.
- Reads any material: breaks and drum loops (also with leading silence, a pickup, hits behind the
  beat or a ring-out tail), melodic loops, whole songs (sections and phrases), one-shots and
  ambient textures. Loops lock to their length (whole bars, exact tempo), the tempo and key in
  the file name are used when they are there, and the automatic chop size suits the material:
  breaks on their hits in eighths or sixteenths, songs in bars.
- No more tiny first slice: chops always start on bar 1, and no slice is shorter than half a step.
- 59 chop layouts in tabs (Flip, Phrase, Breaks, Micro, Creative, Mine), six of them new instant
  flips; every layout fits breaks, songs and one-shots alike and never refers to a missing slice.
- Plays as an instrument (slices on keys, the pattern on the octave below, locked to the song,
  following the project tempo by stretching or resampling); loops out as WAV files, drags into the
  playlist, rack or piano roll, or a pattern sent to the project.
- `gridline seltmann-analyse` prints what Seltmann hears in a set of files.

### Stop silences everything

- Stop now stops every sound, not only the song: notes from the piano roll and channel keyboards,
  typed keys, a MIDI keyboard (sustain pedal included), drum and Seltmann pads, the arpeggiator,
  the browser and audio editor previews, and installed plugins (note-offs plus All Notes Off) are
  released with their normal tails. Press Stop again to silence everything at once, delay and reverb
  tails included, with a short fade so nothing clicks; "Panic: stop all sound" is also in the Stop
  button's menu (Ctrl+H). Keys held when Gridline loses focus no longer get stuck.
- Plugins' own Play buttons follow Stop: Seltmann's pattern button (and Kerf's region play, the
  chord editor's preview) shows stopped as soon as Stop, Space, Stop twice, Tools > Stop all sound,
  a MIDI controller's Stop or a focus loss silences it, starts again from the first step with one
  click, and goes off by itself when the sound ends. Stop also ends the audio editor's playback in
  a mixer slot, Shuttle's free-running bar repeat and the effects inside Junction FX. Recordings,
  renders and loops dragged from the playlist's clip list now load into Seltmann (and onto channel
  rows) like browser audio files.
- Play stops a plugin playing on its own first: with the pattern or song stopped, pressing Play
  (Space, the Play button, a MIDI controller's or remote Play / Pause) while Seltmann's pattern,
  Kerf's region play, the chord editor's preview, Loupe's Play (in a mixer slot or on its own),
  Shuttle's free-running held bar or one of them inside Junction FX is sounding stops that playback
  (with its normal release, tails ring on) and leaves Gridline stopped; the next Play plays the
  pattern or song as usual. This works whichever window has the focus; the hint line says what was
  stopped and the Play button's tooltip says so beforehand. Keys you hold, the browser's sample
  preview (it plays on over the song) and effect tails do not count, and starting the song any other
  way (the playlist's play tool, a launched live clip, external sync) still lets the plugin play along.

## 0.6.1

### Drawing, Convolver and stability

- Smooth drawing: strokes drawn into Cipher's bands, Morph EQ, Curve EQ, Band Delay, Facet's partials,
  Lattice harmonics and Keyboard Mod fill every band between mouse samples and are smoothed
  automatically (Alt draws raw). Each stroke is one undo step. Amount in Settings > Interface >
  Drawing, plus an on/off chip on each surface.
- Convolver: the IR EQ and the IR envelope are edited with points again, never painted. A click adds
  exactly one point (keep the button down to place it), dragging a point moves it with the pointer
  (Shift for fine moves) without adding any, double-click deletes a point, right-click opens a point
  menu (delete, straighten, reset), Alt-drag bends a segment. Every gesture is one undo step, and a
  drag released outside the graph ends cleanly. The editor is redesigned: a large EQ graph with a
  log-frequency grid, dB axis, the impulse's spectrum behind a filled curve and a hover readout; a
  waveform view with a time axis; a source area that takes dropped audio files; knobs grouped into
  Source, Shape, Space, Character and Output sections with value readouts.
- Convolver: new IR EQ and IR envelope curves are smooth. The curve runs through every point with no
  corners and never overshoots between points. One SMOOTH / STRAIGHT button above the graph switches
  the curve on view back to straight segments, which Alt-drag can bend. The switch is one undo step
  and keeps your points and bends. The audio follows the drawn curve exactly. Existing projects,
  presets and FL imports stay straight and sound the same. FL export writes a smooth curve as
  straight points that stay within 0.1 dB of it.
- Quitting while the background plugin scan was still running (first start, new or updated plugins)
  could crash on the way out ("mutex lock failed", a crash report on macOS). Quitting now stops the
  scan (the plugins not reached yet are scanned at the next start) and every other background job
  before Gridline closes; settings and projects are saved as before.

### Fixes from issue #1

- Linux and BSD: the automatic interface scale now follows the desktop's scale setting (Xft.dpi,
  GDK_SCALE, Qt and Wayland scaling) instead of the panel's physical pixel density, so a 1920x1080
  laptop at 100% opens at 100%. A new option, Settings > Interface "Follow the system's display
  scale", can be turned off to set the size yourself.
- Auto Pan: the pad's live dot used the wrong volume scale and could sit outside the pad. The dot and
  the setting ring now share one mapping, stay inside the pad and follow the pointer. The X-Y Pad
  output dot is clamped the same way.
- Floating windows dragged against the top no longer leave an empty band above their content.
- Linux: Space, F1 to F12 and Ctrl/Alt shortcuts reach Gridline while a hosted plugin's own window
  has focus. Windows uses the same rule.
- The play, stop and record buttons act on press.
- The metronome icon ticks on every beat while the metronome is on and the song plays: it snaps
  between facing right and mirrored exactly on the beat, with no in-between frames, and switching the
  metronome on or off only shows its rest pose. A new "Interface animations" switch (Settings >
  Interface > Motion) turns decorative animations off and follows the system's reduced motion setting.
- Channel rack: dropping an audio file or an instrument from the browser (or a file from another
  application) onto a channel row replaces that channel's content, as FL Studio does. A sample
  channel loads the file; any other instrument becomes a sampler holding it; an instrument or
  instrument preset replaces the channel's instrument. The channel keeps its mixer track, volume,
  pan, steps and notes, and its name unless the name was automatic. The row shows "Replace <name>"
  while you drag. Dropping on the edge between two rows inserts a new channel there, and dropping
  under the rows adds one as before. Each drop is one undo step. A MIDI file dropped on a row fills
  that channel's notes. Mixer slots and strips now refuse instruments, and the playlist and rack
  refuse effects, with a reason. The playlist's clip source picker accepts drops. Loupe loads an
  audio file dropped on its waveform. A file from another application dropped on a plugin's sample
  slot no longer also opens as a new channel.

### Bitcrusher and Tideline

- Bitcrusher: redesigned around a Rate control in Hz. Rate sets the sample rate the signal is held
  at, from 50 Hz up to Off, on a logarithmic knob that reads "11.0 kHz" or "820 Hz". Any rate works,
  not only whole divisions of the audio sample rate, and sweeping or automating it is smooth, with no
  clicks or zipper noise. It replaces Downsample, which divided the audio sample rate by a whole number.
- Bitcrusher: Bits is the second control. It keeps its logarithmic knob, where 1 to 8 bits take 65%
  of the travel, and fractional depths ("5.6 bits"). New Anti-alias and Smoothing controls filter
  the signal before and after the hold. At 0% they let all the crunchy aliasing through; at 100%
  they give a clean, dull lo-fi sound. A new Dither control trades the bit-depth distortion for
  hiss. Mix, Gain, the filter, Squarize and Punch stay.
- Bitcrusher: a new editor puts Rate on a large knob with its readout. Bits sits beside it on a
  second knob, with the number of levels. A live display between them shows the held,
  quantised output over the input wave. Drag the display sideways to set the rate, or up and down
  to set the bits. New factory presets: Handheld 4-bit, Telephone, Sampler 12-bit and Crushed
  sweep.
- Projects, presets and effect chains saved by earlier versions keep their sound. Downsample N
  becomes the audio sample rate set in Settings divided by N (/4 at 44.1 kHz is 11.0 kHz). Automation, pattern
  automation and controller links on Downsample (project schema 8) and on Bits (schema 7) are
  moved to the new knobs when the project loads.
- Tideline: the Split slider's handle now stays under the pointer. It used to move 1.25 times
  as fast as the pointer and drifted after the pointer went past either end. Clicking no longer
  makes it jump, and Shift drags finer. All slider controls in the plugin editors now follow
  the pointer the same way.
- Tideline: the playhead, the output trace, the one-shot End marker and the custom curve points
  now line up with the curve when Shift or Duration are set. Points and the End marker no longer
  jump when you grab them off centre. Shift-dragging the curve wraps around the cycle.
- Tideline Listen plays only the part being ducked: the band in Lows / Highs, and the mid or
  side component when one is selected. Full band with Mid or Side used to ignore Listen. With
  Full band in Stereo everything is ducked, so the button is drawn faint and its tooltip explains
  why.

### Plugin editors

- One shared knob renderer for every built-in editor: anti-aliased rings, hover glow, shaded caps.
- Refined section headers, recessed value fields, gradient graph fills and hover readouts on the
  filter, maximizer, band delay and envelope editors.
- Shuttle: time and volume colour coding, cleaner slot cells and a readout while dragging a point.
- Shuttle: new layout. A header with the Hold pad, the two mix dials and the slots playing now; both
  slot banks with readable names; a large envelope editor with a labelled tool bar, axis labels and
  the smoothing dials beside it; slot settings underneath; an Options panel that also shows the quick
  effect, step gate, scratch and engine settings. Follows the light theme.

### Every parameter checked

- New `gridline param-audit` command and test: every parameter of every built-in plugin is checked
  for an audible effect, engine use, editor wiring, automation and save/load.
- Fixed crashes or runaway output in Crest, Enhance, Lattice ring-mod loops, Octa Filter ALT
  resonance, and Isoline when an oscillator was switched on during playback.
- Wired parameters the engines ignored: pitch bend range (Hybrid, DCO and Hex synths), Hex Synth
  modulation sources, Pocket Synth mod wheel, MIDI Out bend range and pass-through, Spectra Morph
  chorus rate, Grain Scanner volume and transient hold, Kerf minimum slice gap, Pitch Shifter Hi-fi
  tempo sync, Alloy and Halftone chorus, Pixel Tone rows, Pedal Rack boost and more.
- Saw Swarm and Twin VA glide fixes; Note Sequencer ties hold their note.
- Missing editor controls added (Facet fine tune, Folio program controls); percentage readouts that
  showed values like "10000%" fixed.
- Abacus and the sampler no longer misbehave when a sample's start is set to its very end; the
  same kind of fix for the slicer's slice ends, plugin event timing and a few editor labels.

### Gridline's own sound data

- Hex Synth has its own 32 wave tables (classic shapes, tonal shapes, bit noise, formant and sync
  waves), generated by Gridline, so every wave works without FL Studio installed. Imported
  patches keep their wave numbers and get a Gridline wave of the same kind. Kick Sculptor's eight
  noise layers are synthesised too, so they also work without FL Studio.

### Plugins renamed

Gridline's built-in plugins now follow one naming system. Flagships carry their own one-word
names, a family accent and a wordmark in their editor's title bar; everyday studio tools keep
plain descriptive names (Compressor, Parametric EQ, Arpeggiator). Type ids (`gl.*`) are unchanged,
so projects, presets, scripts and FL Studio imports keep working; former names stay searchable in
the plugin picker, the browser and the MCP catalogue (`list_plugin_catalog`), except where a former
name was also the name of an FL Studio plugin. Existing projects keep the channel names they
were saved with.

| Before | Now | Type | Tagline |
|---|---|---|---|
| Trio Synth | **Tercet** | `gl.synth` | Three oscillators, one clean line. |
| Drum Pads | **Abacus** | `gl.pads` | Sixteen pads, counted out on a grid. |
| Slicer | **Kerf** | `gl.slicer` | Cuts loops into slices along the beat. |
| Wavetable | **Isoline** | `gl.wavetable` | Wavetables scanned like contour lines. |
| Matrix FM | **Lattice** | `gl.matrixfm` | Six FM operators wired through one modulation grid. |
| Wave Program | **Folio** | `gl.waveprogram` | Zones, layers and loops bound into one instrument. |
| Additive | **Facet** | `gl.additive` | Additive synthesis, one harmonic facet at a time. |
| Resynth | **Halftone** | `gl.harmor` | Resynthesis that prints sound as fields of partials. |
| Patch Host | **Junction** | `gl.patcher` | Patch plugins together into one instrument. |
| Patch Host FX | **Junction FX** | `gl.patcherfx` | Patch plugins together into one effect. |
| Delay | **Tracer** | `gl.delay` | Echoes that trace the beat, from tape to digital. |
| Hall | **Cloister** | `gl.hall` | Long algorithmic halls with a quiet floor. |
| Time Gate | **Shuttle** | `gl.timegate` | Redraws time and volume on the grid. |
| Mastering Dynamics | **Crest** | `gl.master` | Multiband loudness for the master. |
| Vocoder | **Cipher** | `gl.vocoder` | Speaks any sound through a voice. |
| Pitch Corrector | **Plumb** | `gl.pitcher` | Pulls every note true to the scale. |
| Scope | **Graticule** | `gl.scope` | Scope, spectrum and phase on one measured screen. |
| Audio Editor | **Loupe** | `gl.edison` | The close-up audio editor: waveform, spectrum, markers. |
| Analog | **Analog Synth** | `gl.analog` | tool |
| Groove Synth | **Hybrid Synth** | `gl.gms` | tool |
| Plucker | **Pluck Model** | `gl.plucker` | tool |
| Envelope Controller | **Envelope Mod** | `gl.envctl` | tool |
| Keyboard Controller | **Keyboard Mod** | `gl.keyctl` | tool |
| Formula Controller | **Formula Mod** | `gl.formulactl` | tool |
| Control Surface | **Control Deck** | `gl.surface` | tool |
| Soft Clipper | **Clipper** | `gl.clipper` | tool |
| Stereo Shaper | **Stereo Width** | `gl.stereo` | tool |
| Low Lifter | **Sub Focus** | `gl.lowlift` | tool |
| Scratcher | **Turntable** | `gl.scratcher` | tool |
| Spreader | **Stereo Spread** | `gl.spread` | tool |
| Hit Looper | **Hit Repeater** | `gl.hitloop` | tool |
| Amp Rack | **Pedal Rack** | `gl.amprack` | tool |
| Squeeze | **Pinch** | `gl.squeeze` | tool |
| Pocket Rack | **Pocket FX** | `gl.pocketrack` | tool |
| Key Mapper | **Key Remap** | `gl.keymap` | tool |
| Key Splitter | **Key Zones** | `gl.split` | tool |
| Level Scaler | **Velocity Scale** | `gl.velscale` | tool |
| Free Filter | **Biquad Filter** | `gl.freefilter` | tool |
| Spline Scratcher | **Spline Scratch** | `gl.splinescratch` | tool |

- Editor title strips and the audio editor's window take their titles from the catalogue.
- The FL Studio import report lists every plugin as "FL plugin → Gridline plugin"; the export
  report names the Gridline side by its display name and type.

### New plugin

- New effect: Tideline (`gl.tideline`, Dynamics). It is a rhythmic volume ducker with one big Amount
  dial, 16 pump, sub, trim, reverse and rhythm curves plus a drawable curve, and rates from 1/16
  to 2 bars (triplet and dotted included). It can follow the tempo or be triggered by audio (track
  sidechain) or MIDI, in loop or one-shot mode with an end marker. Duration and shift are
  adjustable, it can duck only the lows or highs, or only the mid or side, and it adds no latency.
  It exports to FL Studio as a Gross Beat volume envelope. See `docs/plugins/tideline.md`.

## 0.6.0

### Pitch and time stretching

- New stretch engine (Signalsmith Stretch, MIT) with a band-limited sinc resampler for pitch:
  pitch shifts land within a hundredth of a cent, drum hits stay sharp and the level no longer
  flutters. Used by playlist audio clips, the sampler, the slicers, the audio editor and the
  pitch shifter.
- Stretch modes: Resample, Auto, Mono / vocal, Transient and Formant keep. They are read from
  and written to FL Studio projects.
- Resample mode and sample playback are band-limited, so pitching up no longer aliases.
- The pitch shifter has a new Hi-fi engine.


### Platforms

- Gridline now builds, tests and ships on Windows (x64 and ARM64), Linux (x86_64 and aarch64),
  macOS (Intel and Apple Silicon, one universal app), FreeBSD, OpenBSD and NetBSD. Every
  platform is built and tested on each change.
- Windows: audio through SDL (WASAPI), MIDI devices through Windows Multimedia, plugin hosting
  (VST3, CLAP, VST2, LV2) with the plugins' own interfaces in Gridline's plugin windows, the
  plugin sandbox (separate process, shared memory, pipes), drag and drop from Explorer and
  browsers with the drop target shown while hovering, the MCP token in the Windows Credential
  Manager, settings in `%APPDATA%\gridline`.
- Gridline as a plugin on Windows: `Gridline.vst3` and `Gridline.clap` with their own editor.
- Linux: the X11 plugin windows and plugin editor are compiled and tested on every
  distribution in the matrix (Ubuntu, Debian, Fedora, Arch, openSUSE, Alpine with musl).
  Wayland sessions run Gridline natively through SDL; plugin interfaces, which are X11
  windows, open through XWayland.
- FreeBSD, OpenBSD, NetBSD: audio through SDL (sndio / OSS), MIDI devices through sndio,
  plugin interfaces on X11.
- Number parsing and formatting in project files no longer depends on the system locale.

### Releases

- Downloads for every platform: a macOS disk image (universal app, plugins, command line tool),
  Windows installers and zip files, Linux AppImages, `.deb`, `.rpm` and `.tar.gz`, BSD
  `.tar.gz` archives, all with SHA-256 checksums.
- Automatic update check: at most once a day Gridline looks for a newer release on GitHub and
  offers to download it, remind you later or skip that version. Turn it off in Settings, General;
  Help, Check for updates checks right away.
- `gridline --version` prints the version.

## 0.5.1

- FL Studio project export: files written by Gridline open in FL Studio 2026, with the effects
  of each mixer insert in their slots and complete states for the formula controller, MIDI out
  and the audio editor.
- Song loop marker: one marker loops the song between itself and the next one; marker actions
  run inside the audio engine, sample accurate.
- The channel rack keeps its volume and pan knobs with long patterns, and the step sequencer
  scrolls sideways.
- Thin mixer: the mouse wheel scrolls the strips instead of changing a fader.
- The playlist's clip source picker can delete audio and automation clip sources.
- Editing tools: right-click deletes only the topmost item, audio clips show a volume line,
  zero-crossing snapping and time stretching have toggles with keys.
- Notifications can be copied and dismissed without clicking through to the playlist.
- Edited audio is saved to a neutral "Edited audio" folder that the browser lists.
- FL Studio project export carries Gridline's own instruments and effects with their sound.
- Interface scale, theme, language, layout reset and audio device changes ask to Keep or
  Revert, and revert by themselves after 15 seconds.
- Piano roll keys light up for sounding notes, drum machine pads flash when hit, and the tool
  strip icons are 16 px.

## 0.5.0

- Drag and drop from other applications: files, file promises, web links and audio data from
  file managers, browsers and sample stores, with the drop target shown under the pointer.
- New interface icons drawn from SVG geometry at the exact screen size.
- Channel envelopes, LFOs and filter: exact FL Studio timing for imported projects and smooth
  timing for new ones, in one system.
- Looping pattern clips, Shift+drag copies, an eye toggle for hidden items, a shared minimum
  inset for every editor with an automatic layout audit, effect reordering with the mouse wheel,
  an interactive reverb room display.
- Event lane editing follows the piano roll's rules (nearest stem, line tool).
- FL Studio project import keeps note velocities as MIDI velocities and finds samples that were
  moved next to the project.
- The version comes from one place (`project(VERSION)`), shown in the app, the command line
  and the plugins.
