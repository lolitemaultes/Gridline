# Changelog

All notable changes to Gridline. Versions follow `project(VERSION)` and `GRIDLINE_VERSION_PRE` in
`CMakeLists.txt`; the release workflow publishes the section of the tagged version as the release notes.

## Unreleased

### Added

- Route to a free mixer track, one click. The channel window of every channel (an audio clip, a
  sampler, a Gridline instrument or an installed plugin) has a new button next to its mixer track
  list: it routes the channel to the lowest mixer track nothing uses yet (no channel, effect, send,
  playlist track link, recording arm or input), adds a new track when none is left, names and
  colours the track after the channel and selects it, all in one undo step. A channel that already
  has a track of its own keeps it and only renames it. The same action is the first entry of the
  mixer track list, of the channel rack's mixer track menu, and a button in an installed
  instrument's own window bar. Free-track routing (Ctrl+L and the rack menus) now also leaves alone
  tracks used by extra plugin outputs, audio tracks, armed tracks and audio inputs, and says when
  the mixer is full.
- Import from URL. File > Import > Import from URL... (Ctrl+Shift+U) brings the audio of a web
  page into the project: paste a link (a link on the clipboard is filled in already), Fetch shows
  the title, uploader, length and a playlist's items, and Import downloads the best audio and saves
  it as a lossless 32-bit float WAV in the project's Imported folder, placed as a clip on a new
  track at the playhead (one undo step) or only listed in the browser. A range can be kept instead
  of the whole item, and a playlist imports its first item or chosen items. The popup shows the
  progress of each step, Cancel stops at once, and errors (sign-in needed, private, copy protected,
  unsupported, offline) are named plainly. The title, uploader, length and link stay with the file
  and show in the clip's properties. The release packages carry the downloader (yt-dlp) and the
  converter (FFmpeg, audio only, LGPL); builds without them set them up the first time, checked
  against published checksums, and yt-dlp keeps itself up to date at most once a day (Settings >
  File, can be turned off). Import only media you have the rights to use: Gridline does not sign
  in to sites or get around copy protection. See docs/import-url.md.
- Split stems. Right-click an audio clip (or open its title menu) and pick Split stems... to
  separate it into vocals, drums, bass and other; the six-stem model adds guitar and piano. Choose
  the stems to keep and where they go: new tracks right under the clip, lined up with it and with
  the clip's settings (one undo step, the original clip muted if you like), or files in the
  project's Stems folder, which the browser lists. The split runs in the background on this
  computer with progress, time left and Cancel. Samples in the browser and the audio editor's File
  menu have Split stems... too. The separation is Gridline's own implementation of the published
  Hybrid Transformer Demucs models (Standard, High quality, Six stems); a model is downloaded from
  its official address the first time it is used, after its licence note is shown, and checked
  against its SHA-256 (Import model file... takes a copy you already have). Nothing is sent
  anywhere. On the command line: `gridline stems <audio> --model htdemucs --out <dir>`. See
  docs/stems.md.
- Kerf slices on the beat grid. The SLICES card has a beat grid choice next to hits, 4, 8, 16, 32
  and by hand, with a field for the slice size: 1 bar, 1/2 bar, 1/4, 1/8, 1/16, 1/32 or the
  triplets 1/4 T, 1/8 T and 1/16 T (also in the Regions menu). Kerf finds the loop's tempo, its
  first downbeat, its meter and how its timing moves, the same way Seltmann does, so the cuts land
  on the beats even when the loop starts with a pickup, drifts or is in 3/4. The meter can be set
  by hand (2/4, 3/4, 4/4, 5/4, 6/8). Cuts move onto the nearest hit and then the nearest zero
  crossing (both can be turned off), and the grid can follow the loop's length in beats instead
  (Fit tempo and the beat count) when the detected beat is not wanted. The beat is found in the
  background and remembered per file; while that runs the field shows a busy line and the loop is
  cut by its length. The bars and beats ruler, the grid lines and marker snapping follow the same
  grid. Settings are saved with the project; FL Studio project export writes the cuts as markers.
- Play lights. Every channel rack row has a small light bar at the right end of its name button
  that lights up when a note starts on the channel (from pattern steps, the piano roll, a MIDI
  keyboard, the typing keyboard or a preview), stays lit while the channel sounds (audio clips and
  installed plugins too, following the output level) and fades out smoothly after; muted channels
  stay dark. Kerf lights the regions that are sounding: the region in the wave view, its flag and
  its pad on the keyboard light up (several at once when notes overlap), a thin line shows where
  each voice is reading, and the light fades after the note ends. A region that plays only for a
  moment still blinks.
- Kerf time and marker settings. The ruler under the wave view shows seconds or bars and beats
  (click it, or use the View menu); bars and beats follow the loop's length in beats. Faint beat grid
  lines (bars stronger than beats) show the snap size and can be hidden in the View menu. Markers
  snap to a bar, half bar, beat, 1/2, 1/3, 1/4, 1/6 or 1/8 beat, to zero crossings, or to the grid
  and then the nearest zero crossing, when dragged, added with a double-click or with the add
  buttons; hold Alt while dragging to place a marker freely. Regions can be named (right-click a
  flag, or the name field of the region panel) and the name shows on the flag; a marker's exact
  position can be typed in seconds, samples or bars:beats:ticks, and the arrow keys over the wave
  view (or the nudge buttons and menu items) move it one snap step. Snap, ruler, grid and names
  are saved with the project, and every edit is one undo step.
- Pixel Tone plays your own pictures. "Load picture..." in its editor opens PNG, JPEG, BMP, GIF
  (first frame), TGA, PSD and HDR files, and a picture dragged from the desktop or from the
  browser onto the picture, the plugin window or a Pixel Tone channel in the channel rack loads
  there (the picture lights up while you hover). Photos are reduced to the "Max rows" height with
  their proportions kept (at most 1024 columns and 262144 pixels, so large photos stay quick and
  projects small), transparent parts play as silence, and decoding runs in the background. Each
  load is one undo step and is saved with the project. The editor shows the file name and sizes,
  and can reverse, flip, invert or reset the picture. Files that are not pictures are refused with
  a message and leave the picture as it was. The browser now lists pictures. WebP is not supported.
- Dev scripts: `dialogok <file>` presses OK with that file on the file dialog that is open.
- Seltmann: pitch each region right where it is. Every pad and every region in the waveform shows a
  small pitch readout (pitched ones always, the others under the pointer): drag it up or down for
  semitones (Shift for finer steps), turn the mouse wheel over a pad or a region, double-click the
  readout to go back to 0, or right-click for a Pitch menu (a semitone or an octave up or down,
  reset). Each edit is one undo step and plays at once.
- Seltmann: start the sequence on any cell. Right-click a sequence block, a pad or a region and pick
  "Start the sequence here": it plays from that cell on and the cells before move to the end, so
  the loop keeps its length. "Start here, leave out the steps before" drops them instead. A START
  flag in the waveform shows where the sequence begins; drag it onto another region to start there.
  Saved with the project, one undo step, and the pattern sent to the project follows the new order.
- Seltmann: resize regions directly again. Hover the cut between two regions in the waveform and drag
  it: both neighbours resize (the chop's snap applies, hold Alt to place it freely) and the cuts are
  kept by hand. While the sequence plays every region in order, the edges between its blocks drag
  the same cuts and the steps follow the new lengths. A click on a region still plays it.

### Fixed
- Import from URL: a missing downloader or converter is reported as missing on the BSDs too.
- Platter picks up very quick jog moves from a controller. A flick out and back in 30 ms now
  reaches about 90% of the hand's travel instead of 54% (in 15 ms: 80% instead of 33%), about
  7 ms behind the hand instead of 24 ms, and the record always ends where the hand put it. The
  smoothing adapts to the hand: quick moves are followed closely, slow drags on a coarse wheel
  stay as smooth as before, and Hand feel still sets how close it follows. The wheel's touch,
  its turns and the release now each play on the sample they arrived on, so a touch, a flick
  and a release in quick succession keep their order. Recorded passes keep the quick moves and
  play them back exactly.
- The clear button in the browser search field works again: it empties the field (also while you are typing in it) and leaves the cursor in the field.

- Dragging the volume or fades of several selected audio clips no longer loads the CPU and makes
  the interface lag. Volume, fade and fade curve changes now go straight to the playing song
  instead of preparing the whole song again on every frame of the drag (with 24 stretched clips a
  frame of the drag took about 50 ms longer before, and 70 ms while playing; now no longer than
  any other frame).
- Turning a pitch knob or field of audio clips (the clip's pitch, the channel's pitch, fine tune and
  time knobs) is smooth again. While you turn it the clips play through the realtime stretch, which
  follows the knob at once without restarting the sound, and the high quality render of the new
  pitch starts once you let go. Before, every step of the knob started a new render of every clip
  (the last one could arrive many seconds late), prepared new playback voices for every clip on
  each frame (up to a second per frame with Rubber Band) and restarted the sound.

- Audio clips no longer shrink or get cut off when the tempo changes. A clip you stretch (by
  dragging an edge in stretch mode, typing a Time value or using Fit to tempo) now follows the
  project tempo: it keeps its bars and plays faster or slower with the tempo, also through tempo
  automation. With a time mode that keeps the pitch the pitch stays; in Resample mode the pitch
  follows the speed, like a record. A clip you never stretched keeps the audio it was cut to and
  plays it at the same speed: its border on the playlist grows or shrinks with the tempo, so no
  audio is cut off and no silent tail appears (one undo step with the tempo change; clips after it
  stay where they are). "Follow the project tempo" in the clip menu and in the clip settings turns
  this on or off for any audio clip; turning it off keeps the clip's current speed. Projects open
  unchanged, and clips that already followed the tempo keep their pitch as before. FL Studio
  project export writes a clip that follows the tempo as the stretch it plays at the song tempo.
- Smooth snapping no longer trails behind a fast drag: when the pointer crosses cells faster than a
  glide can land, the drawn clip or note speeds up and stays close to the pointer (about a sixth of a
  cell behind at one cell per frame, was over a cell); slow single steps glide as before.
- Smooth snapping can be seen again. Dragged clips, notes, selection edges, automation points and
  cut lines did glide between grid lines, but the glide covered three quarters of each step in the
  first frame and was over in about 50 ms, so it looked like a jump. It now takes about 140 ms to
  settle and still starts moving at once. Switching smooth snapping on in the middle of a drag
  applies to the next step instead of the next drag.
- FL Studio projects: audio clips whose channel stretches its sample to a set time (a number of
  beats) now play at that length, so loops and one-shots line up with the beat grid as in FL
  Studio. Resample channels change the pitch with the stretch, the other modes keep it. Sampler
  channels with a set time are stretched the same way. Saving back to FL Studio keeps the time.
- Slicex channels from FL Studio projects now open in Kerf with all their chops when the loop's
  stored file path is 255 characters or longer (common in projects that moved between computers):
  every region comes across in place, plays on the same key, and exports back unchanged. Loops
  with hundreds of regions keep every region, with keys from the bottom of the keyboard up.
- Plasma's preset library: clicking a folder of the Serum library now shows its presets. The
  library is a folder tree at any depth (arrows open and close folders, each shows its preset
  count), a folder lists its own presets and its sub-folders', the search looks inside the selected
  folder, the column heads sort the list, the tree scrolls without moving the window, and the
  selected preset stays selected while you search or sort.
- Notes with a fine pitch now keep their pitch in Kerf: a note tuned up or down (up to an octave
  either way) plays the chop of its own key at that pitch, so imported FL Studio projects that pitch
  Slicex chops this way play as written. Before, the tuning moved the note to another key, so it
  played a different chop or nothing. The channel and master pitch also tune the chops without changing which chop plays,
  and the same holds for drum pads and kit pieces. The channel's key range checks the written key.

### Changed
- Tempo display: drag or scroll the whole number to step whole BPM while the decimals stay (96.250 becomes 97.250, 98.250, ...); drag the decimals to fine-tune as before, or scroll them by 0.1 (Shift: 0.01). A short line under the digits shows which part a drag or scroll moves.

- Zooming glides. Wheel notches, the zoom keys and buttons, the zoom tool, zoom to fit, zoom to the
  selection and the zoom menus ease the zoom over about a tenth of a second in the playlist, the
  piano roll and the automation editor, with the point under the pointer (or the zoom's anchor)
  staying in place on every frame. Quick notches add up and end at exactly the same zoom as before.
  Zooming with a trackpad still follows the fingers directly. Settings > Interface > Motion has a
  new "Smooth zooming" switch (on by default; off with Interface animations or when the system asks
  for reduced motion).
- Dev scripts: `setting <key> on|off` changes a switch the way the Settings dialog does.
- The playlist's slip tool works on a selection: dragging inside a selected clip slips the content of
  every selected clip by the same amount (one undo step); an unselected clip still slips alone.
- Kerf's editor has a cleaner layout. The top band is split into three cards: the level and
  tuning sliders, MODULATION (the X / Y pad with its two wheels) and PLAYBACK. In PLAYBACK one
  menu sets how a region plays (gated or one shot, the region alone or on to the end of the
  loop), with fit to tempo, beats, mono and declick in two rows under it. The region panel has a
  REGION card (selector, key and cut, then AMP, FILTER, PITCH and START rows) and a SLICES card
  (hits, 4, 8, 16, 32 or by hand, and the filter). The deck bar keeps its buttons in one row with
  captions over the loop's details. The wave view has an overview on top and a row of switches
  for snapping, the region's name and its position. The keyboard is two rows of pads. Controls
  sit in even rows, nothing overlaps, and text keeps a readable size at 1280 x 800 and at 150%.
- Kerf's editor is laid out band by band like a classic region slicer and finished in Gridline's
  own style. The view beside the filter has tabs: AMP (envelope and a velocity curve), REGION
  (the selected region's audio) and DETECT (slicing method, the loop with its cuts, sensitivity and
  gap). The deck bar gains a loop button for region playback, File, Sample, Edit, Tools, Regions,
  View, Snap, Select and Zoom menus, undo and redo, a button that writes the regions into the
  current pattern as notes, and a handle to drag the loop out. The wave view draws peaks and RMS in
  layers, highlights the selected and hovered regions, has a time ruler, zooms smoothly (Shift +
  wheel scrolls) and, when empty, shows a drop zone that also opens the file picker. Markers can
  snap to zero crossings or sixteenths. A display selector hides the wave editor or the region
  panel (or both), a button hides the keyboard, an options menu holds key macros and playback
  settings, and right-clicking a key assigns the current region to it. The editor now scales down
  to fit short windows (such as 1280 x 800) instead of being cut off at the bottom.
- A song-mode Platter pass is one new pattern holding all its automation clips ("Platter take
  1"), placed once on the playlist where you played it and as long as the pass, instead of a
  stack of separate clips on many tracks. The curves show on that clip, a double-click opens one
  in the automation editor, and moving, trimming, cutting or copying the clip takes the whole
  pass along; it still replays exactly and plays from any point. Platter's new Song choice
  (Pattern or Clips) keeps the separate playlist clips for those who want them, and projects
  with passes recorded that way play as before.
- Kerf's window is laid out like a full region slicer: a larger wave view with numbered (and
  named) region flags, zoom and a scroller across the whole loop; a region list to pick any
  region; a deck bar with load, slicing and view menus, tool buttons and the loop's details; a
  view of the selected region; and controls that do nothing for the current engine are dimmed.

## 0.7.0-beta.4

This is a beta version: Gridline offers it only to people who tick Include beta versions in the update settings.

### New since 0.7.0-beta.3

- A loading window for projects: a progress bar that counts every sample and plugin, a live log
  with the time each one took, warnings that stay on a card afterwards, and Cancel to go back to
  the project you had open. Gridline no longer freezes while a big project opens.
- Patterns can hold automation clips: they play wherever the pattern plays, are edited like any
  other automation clip, and show as curves on the pattern's clips in the playlist.
- Platter passes are recorded as named automation clips (the record's position, the wheel, play,
  loops and every control you used), in song mode and in pattern mode, so a pass plays from
  wherever playback starts and can be cut, trimmed and moved.
- Tideline follows a source you pick in its window: a mixer track or a channel for audio
  ducking, with the sidechain made for you and up to 10 ms look-ahead, or a channel's notes and
  clip starts, the MIDI input or MIDI Out ports, all sample-accurate.
- Platter decks have /2 and x2 buttons, and Platter and Seltmann read drum and bass at its real
  tempo (174, not 116 or 87).
- Fixes: changing an audio clip's pitch keeps its length and fill, Rubber Band notes and clips
  start without crackles, FL Studio 2026 projects import with their whole playlist, Slicex chops
  arrive in Kerf exactly, plugins such as Serum 2 export to the FL Studio format exactly, and a
  song-mode Platter pass is no longer split at the song end.

### New

- Opening a project no longer freezes Gridline. The open dialog, menu or welcome screen closes
  right away and a loading window shows the project's name, a progress bar that counts every
  sample and plugin, what is loading at the moment (which plugin is starting, 3 of 9) and a log
  of everything loaded with its time, so a slow plugin is easy to spot. Missing samples, plugins
  that are not installed and stand-ins are listed as warnings and stay on a notification card
  afterwards. Cancel (or Esc) goes back to the project you had open, exactly as it was. Quick
  projects open without the window flashing up, and a project that cannot be read says why in
  the same window. This works the same for Gridline projects, FL Studio projects, zipped
  projects and collaborative projects.
- Platter passes are recorded as automation clips: in song mode each pass becomes a grouped set
  of clips in the playlist, one per control you used, named like "Platter A wheel", with the
  record's position, play, the wheel and its touch, the mouse, loops, reverse and pitch bend as
  curves you can see and edit. No piano roll notes any more, so a pass plays from wherever
  playback starts: cut its clips, trim their start or move them and the decks pick the pass up
  there. Unedited, a pass still replays exactly as played, at any buffer size. Passes recorded
  as notes earlier keep playing as before.
- Tideline follows a source you pick in its own window. In Audio mode, choose a mixer track or a
  channel (also an audio clip channel) from the new Source list: Tideline makes the sidechain
  route for you, and it shows in the mixer. A channel that plays into the master or into
  Tideline's own track gets a track of its own first. A new Look-ahead control (up to 10 ms)
  lets the duck land right on the hit; the project stays in time.
- Tideline's MIDI mode can follow a channel: every note of its patterns and the playlist, notes
  you play live on it, and each start of its audio clips, exactly on the sample, with no
  detection. It can also follow the MIDI input (your controller, or the host's MIDI in plugin
  builds) or MIDI Out ports as before. A note filter takes one note or a range.
- Tideline's MIDI triggers are now sample-accurate (they used to fire at the start of the
  audio block).
- Patterns can hold automation clips. A clip in a pattern plays wherever the pattern plays and
  is edited in the automation editor like any other: points, curve shapes, the LFO tool, a
  name and a colour. Add one from a control's right-click menu ("Create automation clip in
  pattern"), from the channel rack's or the pattern's menu, or move or copy a playlist
  automation clip into the pattern. The pattern's clips in the playlist draw the curves over
  the notes (the View menu can show notes only or automation only); double-click a curve to
  edit it. The channel rack lists a pattern's clips as rows below the channels, and the
  browser lists them under Current project. You no longer have to open the piano roll's event
  lane to change a pattern's pitch wheel or volume. Clips come along when a pattern is cloned
  or made unique, and they are saved, synced in collaboration, exported to FL Studio projects
  and to MIDI (volume and pan). Pattern lanes from older projects open as clips and sound
  exactly the same.
- Platter's pattern-mode passes are written as named, coloured automation clips of the pattern
  ("Platter A wheel"), the same as in song mode, so the wheel shows moving on the pattern's
  playlist clips. Unedited passes still replay exactly as played.

### Fixed

- A Platter performance recorded in song mode is no longer cut into pieces at the song end. On a short or empty
  song the playlist looped back to the start while you were still playing the decks, and every loop ended the pass
  and began a new one, so one performance landed as several sets of clips. Recording in song mode without a time
  selection now plays on past the song end, as audio recording already did, and the whole performance is one pass.
  With a time selection the selection still loops and punches in and out as before, now also when no audio track is
  armed.
- Platter and Seltmann read drum and bass at its real tempo again. A 174 BPM track used to show about 116 BPM (two
  thirds of the tempo, with a grid that drifted off the beats) or 87 BPM; the tempo analysis now checks that the beat
  it picks fits every hit of the groove and recognises the snare backbeat at fast tempos, so drum and bass from 160 to
  185 BPM, breaks and boom bap read at their own tempo and their beat grids sit on the hits. Half-time grooves may
  still read at half speed. Platter decks get /2 and x2 buttons next to the load button, like Seltmann's, for when the
  analysis hears half or double time. Tracks analysed before are analysed again.
- Rubber Band notes and clips no longer cause crackles when they start. Rubber Band has to work through a long
  stretch of audio before a note's first sound, and Gridline did all of that in the one audio block where the note
  started, so a chord, or a few clips starting on the same beat, could take several blocks' worth of CPU at once
  (worst at 96 kHz and small buffers). Notes in patterns and the playlist and audio clips on the playlist now do
  that work in small pieces during the blocks before they play, and sound exactly as before. Notes played live
  still start the moment you play them. Two smaller fixes come with it: a few pitches no longer make the audio
  thread allocate memory the first time they play, and a sampler note now sounds the same whichever note played on
  its voice before.
- Changing the pitch of an audio clip on the playlist no longer changes how its audio fits the clip. In every time
  mode the clip keeps its length and its audio still runs from the clip start to exactly the clip end: no silent gap
  after pitching up, nothing cut off after pitching down. In Resample mode a stretched clip still sounds resampled
  (twice as long plays an octave lower) and the pitch moves on top of that. Clips in projects made before this fix
  fill their clip again too. Imported FL Studio projects keep their original timing.
- Switching an audio clip's time mode no longer resizes the clip, and a trimmed clip stays trimmed when its time
  factor changes.
- A resampled, stretched audio clip on a channel with its pitch knob turned now renders at the same pitch it plays at.
- FL Studio projects saved by its 2026 version open with their whole playlist arrangement again: pattern clips and
  audio clips land on their tracks with their lengths, offsets, fades and gain. Before, the arrangement came in empty.
  If a project ever stores its playlist in a layout Gridline does not know, the clips are still read wherever the
  layout allows, and the import report says which arrangement lost what instead of dropping it quietly. Such
  projects also save back to the FL Studio format with their clips.
- Slicex channels in imported FL Studio projects now open in Kerf with exactly the chops they were saved with: every
  region in its saved order, on the key it was assigned (including keys moved away from the usual C5 upwards layout),
  with its own start, end and name, even when two regions share a start. Kerf's editor shows these regions as they
  play, region starts can be dragged, and markers no longer land one sample early. Exporting the project back to the
  FL Studio format keeps the chops, including keys, starts and ends changed in Gridline.
- Third-party plugins such as Serum 2 now export to the FL Studio format exactly. A plugin that came from an FL Studio
  project keeps all of its original plugin host settings, and only its sound settings are updated to what the plugin
  holds now. A plugin that was missing and played by a stand-in is written back as the original plugin, still as an
  instrument and with its name, instead of being rebuilt as an effect without one. The command line export and round
  trip now know which plugins are installed, as the app does.

### New in 0.7.0-beta.3

- Platter, a two-deck DJ instrument on a channel: decks A and B with a mixer and crossfader,
  tempo and beat grid detection, sync, key lock, hot cues, loops and a pitch fader, ready
  mappings for the Pioneer DDJ-200, Hercules DJControl Inpulse 200 and Numark Mixtrack Pro FX
  (or your own MIDI learn), and scratching that feels like a record, built on the model Mixxx uses.
- Platter performances are recorded as pattern data you can see and edit: every control you move
  becomes an automation lane, every button press (play, cue, hot cues, loops, beat jumps) a note
  on a named key, and scratches lanes for the mouse hand, the jog wheel and nudges. Unedited, a
  recording replays exactly as played, at any buffer size. A second pass never records over the
  first: it goes into a new pattern. Record as: Take keeps the one-note takes.
- Drag audio onto Platter's deck A or deck B to load it there, and Platter's EQ and master knobs
  have 0 dB at 12 o'clock, so a mixer set by eye no longer sounds far too quiet.
- A welcome screen: recent projects as tiles with a sketch of each arrangement, favourites,
  templates and the demo song, and a Collaborative view that shows who is working on each shared
  project right now.
- One smooth stretch and pitch engine for all audio, with far fewer glitches and the same sound
  live and in renders, and Rubber Band as a third time mode for clips, the sampler and the audio
  editor.
- The plugin manager is redesigned: one row per plugin with its formats, clean categories, a
  preferred format and a details panel; the browser lists each plugin once.
- Playlist: changes to one of several selected audio clips (level, fades, pitch) apply to all of
  them by the same amount.
- Seltmann: a sequence lane to reorder, split and join regions, in a shorter editor.
- Fixes: big plugin windows stay inside the main window and scroll when they do not fit, the OUT
  meter in the top bar is a real master output meter, and meters fall more calmly.

### One smooth stretch and pitch engine for all audio

- One smooth stretch and pitch engine for all audio: audio clips (rendered or played in
  realtime, tempo-synced or following tempo changes), the sampler, slices, the audio editor,
  the Pitch Shifter, Platter and the other pitch and time effects all use the same continuous
  stretch. Every picker now offers two choices: Stretch (time and pitch apart) and Resample
  (pitch and speed together, like a turntable).
- Clips can use Rubber Band for stretching and pitch: a third choice in the time mode pickers
  (the clip menu and clip properties, the sampler, the audio editor), next to Stretch and
  Resample. It uses the Rubber Band Library's R3 engine, which keeps drums and transients
  sharper and costs more CPU, in renders and in realtime playback alike. Projects keep the
  choice; FL Studio project export writes it as the stretch.
- Stretched and shifted audio stays smooth from x0.5 to x4 and up to an octave either way:
  far fewer clicks and glitches (the worst realtime case drops from dozens per second to a few),
  no crackle or hop-rate ripple, and bass notes no longer drift or warble near the original
  speed.
- The noise left around steady notes after a stretch or shift is 15 to 20 dB lower, and a bass
  line under drum hits holds its level instead of dipping.
- Realtime playback (sampler notes, clips while the channel pitch moves, Platter) sounds the
  same as a render and is the same for any audio buffer size; pitch changes glide instead of
  stepping.
- Pitched material no longer folds back as aliasing (high tones an octave up drop from full
  level to below -80 dB).
- Projects saved with the older Auto, Mono, Transient, Formant, Voice or Beats modes open with
  Stretch.
- Drum hits keep the silence ahead of them: nothing of a stretched or shifted hit sounds before
  it (pre-echo below -60 dB at every stretch and shift). Hits stretched far (x4) or shifted
  down an octave sound a little softer than the original (3 to 4 dB at the very attack).
- The Pitch Shifter's Hi-fi engine uses a shorter block for live use (latency about 0.24 s,
  covered by delay compensation) and no longer offers formant keeping.

### Platter scratching

- Scratching on Platter is rebuilt on the model Mixxx uses, so it feels like a record with
  some weight instead of a buzzing follower. A jog wheel sets the record's speed from how fast
  its ticks arrive, smoothed the way each controller's Mixxx mapping does it (DDJ-200 and
  Inpulse 200 alike, Mixtrack Pro FX snappier). Strokes come out as clean sweeps with no
  zipper noise, about 30 ms behind the hand.
- Mouse scratching works like Mixxx's: drag the record round and it follows smoothly. Let go
  faster than 2.5x and it is thrown: it coasts and slows down. Let go slower and it goes
  straight back to the motor's speed.
- Touching a playing record catches it smoothly and letting go ramps it back up to speed,
  instead of a hard stop and a jump. At low speeds the level fades the way vinyl does.
- Jog ticks are placed at the moment they arrived, so slow drags on a coarse jog wheel play as
  one tone instead of a stutter, and a record held still by the hand is silent.
- New per-deck Jog knob (next to Hand) sets how far the record moves for each jog tick, from
  0.25x to 2x. The default is 0.8x, a little less sensitive than before. Projects saved
  earlier load with the default.
- Hand feel sets how heavy the record feels: loose is smoother and slower to follow, tight
  follows the hand about twice as fast. The middle setting is Mixxx's own feel.

### Platter performances recorded as automation

- Platter now records what you play as pattern data you can see and edit, instead of only as
  a take. Every control you move (faders, EQ, filter, pitch, the crossfader) becomes an
  automation lane, every button press (play, cue, hot cues, loops, beat jumps) becomes a note
  on a named key in the piano roll, and scratches become lanes for the mouse hand, the jog
  wheel and rim nudges. A "Performance start" note puts both decks back where they were.
- Played back without edits, a recorded performance is exactly what you played: the same
  control values on the same sample and the same audio, at any audio buffer size. Edit the
  notes and lanes and the sound changes with them.
- Recording a second pass never records over the first: if the selected pattern already holds
  a performance, Gridline makes a new pattern (for example "Platter take 2"), selects it and
  records there (in song mode it is placed where the pass started). Each pass is one undo
  step.
- Loop recording makes one performance per loop pass, each in its own pattern, and punch in
  and out works in song mode.
- The Platter window has a choice between Automation (the default) and Take. Takes still work,
  follow the same new pattern rule, and takes recorded earlier keep playing.
- The piano roll shows the names of an instrument's named keys on its notes and keys.

### New

- Collaborative projects can be favourites too: the heart on a tile in the welcome screen's
  Collaborative view (or F, or the tile menu) marks it, and the heart badge takes the mark off.
- Platter, a new instrument in the Gridline Suite: a whole two-deck DJ setup on a channel, deck
  A and deck B with a mixer between them. Drop a track on each deck and it finds the tempo and
  beat grid and plays in time with the project or with the other deck (the padlock frees it to
  its own tempo), with key lock, a platter you scratch with the mouse or a jog wheel, stacked
  three-band colour waveforms with beat grids and a phase meter, cue and play, eight hot cues,
  loops and beat jumps, and a pitch fader. The mixer has trim, three-band EQ with kills and a
  filter per channel, channel faders with level meters and a curve, a crossfader with assign,
  curve, cut and reverse, and the master level with its meter. On first open it asks whether to
  use a DJ controller: ready mappings for the Pioneer DDJ-200, Hercules Inpulse 200 and Numark
  Mixtrack Pro FX map both decks and the mixer at once (with LEDs that follow each deck), or no
  controller and your own MIDI learn mapping per deck and for the mixer. A one-deck layout is
  there too. Mappings are saved with the project and as presets. While Gridline records, what
  you play is kept as a take: a note in the pattern that replays both decks, the scratches and
  the mixer moves exactly. Every control can be automated. The older Turntable effect is
  unchanged.
- A welcome screen, Gridline's new home: recent projects in a large grid, each tile with a
  sketch of the project's arrangement (or its pattern, channels or mixer), when it was last
  opened, its tempo and length. The first tile starts a new project. Open a project from disk,
  start from a template or the demo song, search, and tidy the list (remove a project, remove
  missing ones, show a project in its folder) from the same place. Fully keyboard driven.
- A click on the Gridline logo now shows the welcome screen, with the open project marked and a
  Back to project button; a right click still opens the main menu. Opening another project from
  there asks about unsaved changes first.
- Gridline starts on the welcome screen. The first launch still opens the demo song, and
  projects opened from files, the command line or a drop go straight in. Settings > General >
  On startup: Show the welcome screen turns it off.
- Sketches are cached in the settings folder and made when a project is saved or opened, so the
  welcome screen opens at once; the recent list keeps up to 24 projects.
- Favourite projects on the welcome screen: the heart button on a project's tile (or its menu,
  or the F key) marks it, and a heart badge shows on its tile. Favourites are never pushed off
  the recent list by newer projects and stay when the list is cleared.
- A Collaborative view on the welcome screen: join with a code, start a new collaborative
  project or share the open one, and every collaborative project on this computer as a tile with
  its members' pictures, your role, the Sync review or unsent changes waiting, missing folders,
  needed updates and, above all, who is working on it right now. Presence is checked directly
  with your collaborators, encrypted and only while the view is shown, with no server; a silent
  computer shows as not reachable, never as offline. The collaboration settings are right there
  too, with a new Show that I am online to collaborators option.

### Changed

- The plugin manager is redesigned. Each plugin is one row with the formats it is installed in
  (VST3, CLAP, AU, VST2, LV2) shown as tags, a clean category (Synth, Drums, Reverb, Analyzer and
  so on) instead of raw format strings, a star for favourites and a sortable table. Choose a
  preferred format, or pick one per plugin; the browser and the plugin picker now list each
  plugin once, in that format (a setting lists every format again). Gridline Suite builds are
  kept in a collapsed group and no longer appear in the browser, since the same plugins are
  built in. A details panel shows every format with its version and location, scan problems
  with retry, the sandbox switch, a category override and Add to project. The view tabs show
  counts, the search can be cleared, the folder list shows the home folder as ~, marks missing
  folders and can remove your own, Add folder opens a folder chooser, failed plugins can be
  retried one by one, and the arrow keys move through the list. Favourites and sandbox
  settings carry over unchanged.
- Playlist: with several audio clips selected, changing one clip's level, fades, fade curves or
  pitch changes every selected clip by the same amount. A +2 dB drag adds 2 dB to each clip, a
  longer fade-in lengthens each clip's own fade-in by the same time, and clips keep their
  differences. Each clip stops at its own limits while the others keep moving. One drag is one
  undo step. Reset sets every selected clip back to the default; a typed value goes to the
  clicked clip and moves the others by the same difference.
- Playlist: a right-click on a clip's title strip deletes the clip like a right-click on its body
  (right-drag deletes across clips from there too). Only the clip's name and its small menu arrow
  open the clip menu, and the name highlights where a right-click opens it.
- Seltmann: a new Sequence lane under the waveform. After a drop the regions play in order,
  A B C D, and each region keeps its letter in the waveform, the pads and the lane. Drag a block
  to put the sequence in your own order (Alt+Left and Alt+Right move the selected one).
  Right-click a block, a region in the waveform or a pad to split the region into 2 to 8 equal
  parts: the first part keeps its letter and the later letters move on, so splitting B in
  A B C D gives A B C D E. Delete joins a region to its neighbour (the first one to the next)
  without ever reaching past the chopped region. Splits and joins keep the loop sounding the
  same, every edit is one undo step, and projects saved earlier open and play as before.
- Seltmann: the editor follows Gridline's look: panel title strips, segmented selectors and the
  control styles, with the controls row lined up on one baseline.
- Seltmann is a quarter shorter (the editor body went from 674 to 506 px at 100% scale) and
  reads in the order of the work: a header with the file, its facts and the tempo; the waveform
  with the sequence right under it on the same time axis; one action bar to play, choose the
  chop and take the result out; then the layouts and the pads side by side, the pads in two rows
  of eight. The whole editor now fits a 1280 x 800 screen and a 1440 x 960 one at 125% scale.
  Region colours are calmer: eight muted hues that follow the letters, quiet tinted pads and
  steps, and only the playing region lights up in full. The Edit drawer takes only the height
  its tab needs.

### Fixed

- Big plugin windows (Platter, Seltmann, Plasma, Edison and the other wide editors) no longer pop
  out of Gridline into a separate window by themselves. This happened when a plugin window was
  last used on a larger Gridline window (for example maximised) and then opened in a smaller one.
  Plugin windows now always open inside the main window; they leave it only with the pop-out
  button.
- A plugin window larger than the space between the top bar and the status bar is now capped to
  that space and its content scrolls, down and sideways, instead of running past the edge of the
  main window where part of it could not be reached. With room to spare, windows keep their
  natural size. Small screens and large interface scales (125 %, 150 %) benefit most.
- Platter fits short windows better: the waveforms, platters and pads get more compact before the
  window has to scroll, and it follows a narrow main window more closely.
- Dragging audio onto Platter now loads it on the deck you drop it on: anywhere on deck A or deck
  B, its waveform or its deck panel. Deck B's waveform used to send the track to deck A. Files from
  the browser or the desktop and audio clips from the playlist's clip list all work, and only the
  deck under the pointer lights up while you drag, with its name in the hint.
- Platter no longer sounds far too quiet when its mixer is set by eye. The EQ and master knobs now
  have 0 dB at 12 o'clock, like a DJ mixer: turning them to the centre used to cut each EQ band by
  10 dB and the master by 17 dB, up to 27 dB in all. Turning them right still boosts up to +6 dB,
  and fully left still kills the band or mutes the master. A track on a deck at the default
  settings plays at the file's own level, the same as an audio clip; saved projects keep their
  sound.
- The automation clip editor no longer runs under the status bar on small screens, and the Save as
  template dialog no longer grows past the main window when the templates folder has a long path.
- Welcome screen: the menu button on a project tile now opens the tile's menu, in the
  Collaborative view too.
- The OUT meter in the top bar is now a real master output meter: it shows the master track after
  its effects and fader (the same level as the mixer's master meter), on the same dB scale, with a
  steady fall that does not depend on the frame rate, a short peak hold per channel and a clip lamp
  that lights only above 0 dBFS. It used to hold on to the loudest moment ever played, so it stayed
  lit after the music stopped, and it also counted the metronome click.
- Meters no longer show a lit peak marker at the bottom of the scale in silence; the mixer meters
  fall a little more calmly and hold each channel's peak on its own.

### Removed

- The "Song sketch" project template (and its Songwriting category).

### New in 0.7.0-beta.2

- The Gridline Suite: Gridline's instruments and effects as VST3, CLAP and Audio Unit plugins
  for other DAWs, plus Gridline Suite Mini, 35 compact effects.
- Control styles: Slate (the new default), Graphite, Hairline, Lumen and Classic, chosen in
  Settings > Theme, and sound themes for the interface sounds (Graphite by default).
- Every built-in plugin shows its own wordmark, in its window header and in its own editor.
- Knobs: double-click resets to the default, or opens the value box when the knob is already
  there; the right-click menu starts with Reset to default.
- Swing that feels right: smooth at every setting, and piano roll notes swing as well as steps.
- A new demo song, "Gridline", which opens once on the first launch of a new install.
- Installers in Gridline's design on macOS and Windows, and AppStream metadata on Linux.
- Fixes: faders centred on their tracks in every style, highlights and outlines no longer cut
  off at the edge of their panel, a tidier Theme page, quieter song markers in the playlist, and
  the details below.

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

### New: the Gridline Suite, Gridline's plugins in any DAW

- Every built-in instrument and effect that works on its own is now also a plugin for other music
  programs, in one bundle per format: `Gridline Suite.vst3` and `Gridline Suite.clap` on every
  system and `Gridline Suite.component` (Audio Unit) on macOS. Each plugin has its own name,
  vendor Gridline, a category, its automatable parameters with value text, factory presets, a
  sidechain input where the effect uses one, latency reporting, its resizable Gridline editor, and
  the same state as inside Gridline. The suite is installed with the application: by the Windows
  installer and the Linux packages, and on macOS, with the AppImage and the archives Gridline
  offers to copy it into your plug-in folders and keeps it up to date (Settings, Files, Gridline
  Suite, or `gridline suite install`). See `docs/gridline-suite.md`.

### Sound themes for the interface sounds

- The interface sounds were redesigned and come in five themes: Graphite (warm wood, the
  default), Prism (struck crystal), Tine (electric piano), Signal (clean electronics) and
  Classic (the earlier glass sounds). Pick one in Settings > Theme > Sounds, where each theme
  and each sound has a preview button; the switch takes effect at once, no restart needed. All
  themes share one motif and the same loudness per event, stay at least 3 dB below full scale,
  and the volume and per-sound switches apply to every theme.

### New: Gridline Suite Mini, 35 compact effects

- A second suite of small single-purpose effects, each one plain word: Trim, Breadth, Skew,
  Weave (utility); Bands, Sieve, Rungs, Scorch, Vowel, Tines, Chime (tone); Press, Ceiling,
  Shear, Pack, Span, Latch, Punch (dynamics); Burn, Bend, Warp, Crumble, Clang (drive); Slide,
  Interval (pitch); Double, Crowd, Jet, Orbit (motion); Echo, Volley, Vault, Rewind, Halt,
  Pulse (time). They sit under Gridline Suite Mini in the effect picker.
- Every editor is the same compact card: a title strip with the family colour, a display that
  explains the effect (response curve over the live spectrum, transfer curve, gain reduction,
  delay taps, decay, speed curve, a vowel pad, 16 clickable gate steps), one knob grid and one
  option row. 2 to 6 factory presets each.
- Press, Latch and Clang can follow the track's sidechain input. Ceiling (2 ms look-ahead),
  Pack (10 ms), Shear, Burn, Bend and Warp report their latency for delay compensation.
- Gridline Suite Mini is also a plugin bundle for other music programs: `Gridline Suite Mini.vst3`,
  `.clap` and `.component` (macOS), installed with the Gridline Suite.
- A project that uses one of the Kilohearts single effects that is not installed on this computer
  opens with its Gridline Suite Mini counterpart in the slot (Compressor as Press, Delay as Echo,
  and so on for all 35). The saved values carry over where the settings mean the same thing, the
  rest starts from the Mini defaults, and automation follows. Opening the project says which
  plugin is missing and what plays instead, and the slot shows a marker. The original plugin and
  its own state are kept untouched in the project, so a computer that has it installed hosts the
  real plugin again. An installed plugin is always hosted as itself.

### Every plugin has its own wordmark

- Every built-in instrument and effect now shows a designed text logo at the top left of its
  editor in place of the plain name: a tile in the plugin's family colour with its own small
  pictogram cut out of it (an arcade for Cloister, a plumb bob for Plumb, formant bars for
  Cipher), then the name in the Gridline logo lettering. Gridline Suite and Gridline Suite Mini
  have suite logos of their own. Hover a wordmark to see the name and tagline.
- The wordmarks are drawn from vector sources at the exact pixel size of your interface scale and
  display, so they stay sharp at 100 %, 150 % and 200 %. They take their colours from the theme,
  so they also read well on the light theme.
- The plugins' own interfaces show the wordmark too: Plasma's header, and every editor that
  printed its name as plain text in its title strip or name plate (Isoline, Kerf, Abacus, Keys,
  Tercet, Lattice, Convolver, Stereo Width, Clipper, Plumb, Loupe, the note processors and more).
  The name stays as text only where a plugin has no wordmark.

### Knobs: double-click resets, then types

- Double-click any knob or slider in a built-in plugin to put it back to its default value, as
  one undo step. When it already is at its default, the double-click opens the box where you type
  a value instead.
- Right-click a knob or slider: Reset to default is now the first item of its menu, greyed out
  when the value already is the default. The other items (type a value, automation, links, MIDI
  control) stay. Ctrl+click still resets.
- This works the same in every built-in editor, Plasma's own knobs, its arpeggiator knobs (which
  now reset to their real defaults), faders, the Seltmann slice knobs and the X / Y pads included.

### New: control styles (Settings > Theme > Controls)

- Choose how knobs, faders, buttons, meters, fields and displays are drawn, in the main window and
  every plugin editor: Slate (the default: the studio slate look with one consistent finish),
  Graphite (machined graphite with recessed Paper light, the material of the app icon), Hairline
  (flat lines with one accent), Lumen (dark glass, values drawn as light) or Classic (the look
  before control styles). Each card in the picker is a live preview; a new style applies at once,
  open plugin windows included, and can be kept or reverted like the other theme settings.
- Styles change drawing only: sizes, layouts and click areas stay the same. They follow dark or
  light mode, your accent and role colours, and each plugin's own palette where the style uses one.
- The Theme page is now grouped as Appearance, Controls and Sounds.
- Fixed on the Theme page: the selection ring of the first and last style card was cut off; the
  style previews now show the real knob, fader and meter of each style at their real size,
  centred in the card, so a wide window no longer spreads them apart; the cards wrap to fewer
  columns on a narrow page; long names and descriptions end with an ellipsis instead of being
  cut; the sound theme and sound rows keep their Preview and Play buttons inside the page; the
  colour preview's toolbar no longer overlaps itself on a narrow page; in the light theme the
  style previews show the light surfaces the plugin windows really use; and the Keep or Revert
  prompt for a style change names both styles and previews each one.
- Fixed: the outline of the playing step in the channel rack was cut off on its left edge in
  the first column. Selection, focus and highlight outlines on rack steps, mixer strips, playlist
  clips and fields are now drawn inside their control in every style, so a neighbouring control
  or a pane edge can no longer cover or cut them.
- Mixer strip meters follow the control style too, like every other meter.
- Fixed: fader caps sat to the side of their track. In Slate the cap was 3 px left of the slot
  (mixer strips, the Theme page preview, plugin faders), and in Graphite, Hairline and Lumen the
  whole fader sat 4 px left of its place. In every style the cap, the slot, the value fill, the
  ruler and the unity mark now share one centre line, on whole pixels, at every interface scale;
  the ruler shows on both sides of the slot, and the cap lines up exactly with the tick of its
  value. Sliders in plugin editors are centred on their slot the same way.

### Playlist: quieter song markers

- A song marker (Intro, Verse, Chorus and so on) no longer draws a bright line through the whole
  ruler. Its tick is now the left edge of the marker's label, only as tall as the label, in a
  soft shade of the theme's text colour (the loop marker keeps its lit colour).

### New demo song: "Gridline"

- Demo song (File > New from template, or Help > Load demo song) now opens "Gridline", a 4:25
  atmospheric liquid drum & bass track at 174 BPM in D-flat major, made only with the built-in
  instruments, effects and automation (no samples). It has eight sections, each a named marker:
  Intro, Verse, Chorus, Verse 2, Bridge, Breakdown, Chorus 2 and Outro. The lead is a felted
  wooden mallet built from the Graphite interface sound and plays its motif. Underneath are a
  moving reese over a mono sub, a synthesised rain bed with distant thunder, and buses with
  sidechains. In the second chorus the felt keys answer the mallet's hook. Automation lanes shape
  every section.
- It is mixed and mastered to -10 LUFS with a true peak of -1.2 dBTP. The song is compiled into
  the programs, so it opens instantly on every system and package without looking for files,
  and it renders the same every time.
- The demo song opens by itself once, on the first launch of a new install. Later launches
  follow Settings > General > On startup.
- `gridline demo out.json` writes the same song. `tools/make_demo_song.py` is the script that
  composes it, and test `j135_demo_song` checks its markers, loudness and true peak.
- The earlier 16-bar house demo is gone from the programs. The tests keep it as their own test
  song (`tests/fixtures/`), and the command line examples in the README now start from
  `gridline demo song.json`.

### Swing that feels right

- Song swing now moves piano roll notes as well as channel rack steps. Inside every pair of
  steps, counted from the bar line, the off-beat moves later by the swing amount and everything
  between moves in proportion, so 16ths swing like steps while 32nds and triplets bend with them.
  Notes keep their written positions in the piano roll; only playback swings. Channel swing mix
  and truncated swing apply to notes too. Imported projects whose source swung steps only keep
  doing so; right-click the Swing label in the channel rack to choose.
- The swing knob is smooth: swung steps and notes are placed between ticks and played to the
  sample, so every percent of the knob is heard, at any PPQ. Before, small amounts did nothing
  and the knob moved in coarse jumps.
- The loop starter now writes its parts straight and leaves the swing to the song setting.
  MIDI export writes straight notes, as before.
- Note Sequencer and Arpeggiator steps follow the song grid while the song plays: a key pressed
  a little late or early still swings in time, and after the song loops the steps land on the
  grid again. Turning the swing knob takes effect from the next step.
- Note Sequencer: swing pairs restart with each pass of the pattern, so odd lengths keep the
  first step straight. A new Velocity choice: Step x played (new default) scales each step's
  velocity by how hard the key is played; Step only plays the step velocity as before. Projects
  saved earlier keep Step only. A step at velocity 0 rests.
- Arpeggiator: a fixed velocity of 0 rests instead of sending silent notes, and Accent first in
  Chord mode accents one chord per cycle instead of every chord.
- Drum sequencer steps no longer drift from the tempo, odd-length patterns stay on the grid with
  swing, and the Plasma arpeggiator now swings on the song grid when it follows the song.
  Plasma's clip player no longer skips the first notes of a loop, and the Drum Model's driven
  pads now respond to velocity.

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
- It never shows by itself: a plugin window always opens with the strip hidden (after a restart,
  with a project, and when you close the window and open it again). The eye shows it for that
  window while it stays open; nothing of it is saved. With Interface animations off, or when the
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

### Tidier layouts in plugin editors and panels

- Knobs in a row now share one centre line and their labels one baseline, labels sit centred
  under their knobs, and captions beside menus and value boxes sit on the field's own text line,
  in the built-in plugin editors and the app panels, at 100, 150 and 200 % UI scale. Text is
  placed to the nearest pixel instead of being cut down, so centred labels no longer lean left.
- Fixed overlaps and cut-off text: Plasma's sub oscillator shapes ran into the phase box,
  Halftone's FX rail ran under the clip menu, Saw Swarm's matrix knobs and SYNC button touched
  their neighbours, Retro Phaser's dB scale labels collided, the Maximizer's axis value sat under
  its IN caption, and a note in Crest and Band Splitter's last frequency label were cut off. The channel rack's pattern length buttons sit on one line again.
- A layout check now runs in the test suite: it measures every editor's rows, labels, gaps,
  baselines, overlaps and cut-off text and fails when a change makes any of them worse.

### Every icon is an SVG

- The icons that were still drawn as shapes in code or as text characters are now SVG icons from
  the same set as the rest of the interface: ImGui's tree, combo and menu arrows, check marks,
  close crosses and step buttons; the mixer track icons and plugin category glyphs; the track
  header arm, input and monitor glyphs; playlist clip, loop and fold glyphs; the plugin window
  header's gear, power and preset arrows; the pop out / dock in button; Edison's toolbar; Plasma's
  arrows, gear, pencil, power, save, browse, close, more, undo, redo and audition buttons; and the
  "<", ">", "+", "-", "?", "i" and "=" captions on plugin editor buttons. 88 new icons are drawn
  for Gridline on the same 24-unit grid (`assets/icons/ui/NOTICE.md`).
- Each one is fitted into the pixels the old glyph covered, so nothing grows and no layout or
  click area moves; colours, hover, lit and disabled states and both themes are as before. A
  caption too small for its icon (a 2 px minus) stays text.

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
  other applications, drop-to-replace on the channel rack) work in detached windows. Moving
  between Gridline's windows no longer releases held keys; switching to another application still
  does.
- Detached panels reopen where they were, on the same monitor (saved with your window layout,
  never in the project); a window left on a monitor that is gone comes back on one that is there.
- Gridline's plugin windows (a channel's window, an effect editor) are outside the main window
  only while they stay open after you popped them out. Opened again, after a restart, with a
  project, or after you closed one that was popped out, they always open inside the main window;
  the pop-out button puts them back where they last were outside.
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

### New: collaborative projects (preview)

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
- Collaborators' cursors point at the same thing on every screen whatever the window size,
  layout, interface scale, scroll or zoom: a playlist track and time, a piano roll key and time, a
  channel rack row, a mixer strip, or the button, knob or step under the pointer. They draw on top
  of every window, floating window, popup and menu, fade out quickly where that spot isn't shown
  (a closed or hidden window, another tab or pattern, scrolled away) and back in where it is. The
  cursor is a plain pointer arrow in the collaborator's colour with a thin white rim and a soft
  shadow, sharp at any scale; the name tag is gone (the colour says who it is).
- Collaborators' cursors and pictures: the arrow is the size of your own mouse pointer
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
- Removing someone: the admin removes a member from the "..." menu in People, with an
  optional note. The removal is signed and checked on every computer: they can't connect, be
  relayed or join with old invites any more, and anything they change after the removal is dropped
  everywhere, while their earlier work stays (with their name in Sync review). With the project
  open they see "You were removed from ... by ..." with Keep a private copy / Close project
  (editing stops, playback doesn't); offline, they learn it the next time they connect. Removed
  (n) in People shows who, when and why, with Re-invite for the admin. The collaboration protocol
  is now version 2.
- What happens to the files: Leave asks whether to keep the folder as a normal project or
  move it to the Trash, with sizes, and offers Wait for sync when some changes aren't confirmed
  yet. Stop sharing reaches everyone (also later, when someone offline reconnects) and is never
  undone by an old copy. Your collaborative projects marks removed, stopped and missing projects
  (Keep a private copy, Locate, Forget); a copied folder can't sync next to the original. This
  project shows the folder (Show in Finder / Files), its size and history, and Clean up unused
  audio; after 30 days without anyone it offers to make the project a normal one. Ending a project
  deletes the other members' cached details on this computer. docs/collaboration.md: "Removing
  someone" and "What happens to the files".
- `gridline collab-run` runs a collaborative session without a window (scripts and tests).
- Collaborative projects have a shared name. "Make this project collaborative..." asks for it
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
- Sharing and renaming in place: the Collaboration panel's "Share this project" section
  holds the name (prefilled, checked as you type) and the folder (next to the project file by
  default, Choose...) with a caption of what happens, and "Make collaborative": no separate name
  window any more. Rename... turns the panel's title, or the Shared name row of Project info and
  Project settings, into a name field with Rename / Cancel and the "Also rename the project folder
  on this computer" option under it. Fixed: a dialog that sizes itself to its content (the old
  name window) shrank by a pixel every frame for over a second after it opened: sections reserved
  one pixel less than their width.
- Collaborators' cursors go behind your windows: every window you have open in front of
  the one a cursor points into covers it (Settings, Project info, the Collaboration panel, dialogs,
  the floating channel rack, mixer or piano roll, popups and menus), cut at the window's edge, not
  faded; in that same window it is drawn on top. Behind a modal it is dimmed with everything else.
- New third-party code: libjuice 1.7.4 and libplum 0.6.0 (MPL-2.0), Monocypher 4.0.2 (BSD-2-Clause
  or CC0), stb_image 2.30 and stb_image_write 1.16 (public domain or MIT); see third_party/NOTICE.md.

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

### Installers in Gridline's design

- The macOS disk image opens as a designed window: a dark Gridline header, Gridline and the
  Applications folder side by side with a drag hint between them, the plugins, command line tool
  and read me in a row below, a Retina background and Gridline's own disk icon.
- The Windows setup is dark, with Gridline artwork for every display scale, a welcome page, the
  license shown as information, a Gridline setup icon and clearer wording.
- Linux packages carry AppStream metadata (description, screenshots, releases), so software
  centres such as GNOME Software and KDE Discover show Gridline properly.
- The read me files in every download start with a short Gridline header.
- macOS: the small Gridline icon in the system's password prompts (for example when Gridline
  reads the access token of its AI assistant connection) is sharp instead of noisy.

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
