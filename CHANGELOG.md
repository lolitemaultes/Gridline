# Changelog

All notable changes to Gridline. Versions follow `project(VERSION)` and `GRIDLINE_VERSION_PRE` in
`CMakeLists.txt`; the release workflow publishes `docs/releases/<version>.md` as the release notes, or this file's section of the tagged version when there is none.

## Unreleased

### Added

- **Voltage In: control voltages into Gridline.** A new controller reads pitch, gate, clock and four CV inputs from a DC-coupled audio interface. The pitch and gate play notes into any channel (pick it under **Play into**), timed to the sample, with gate threshold and hysteresis, 1 or 1.2 V/oct, snapping to semitones or a scale, glide, and legato or retrigger. Its eight outputs can drive any parameter through **Link to controller**, hosted plugins included, and a clock input can set the tempo and start and stop the song. See [docs/cv-gate.md](docs/cv-gate.md).
- **Voltage calibration.** A new window, opened with **Calibrate** in Voltage In or Voltage Out, checks whether the interface passes steady voltages, fits the round trip from an output back into an input, and tunes an output's volts per octave with a tuner. Calibration is kept on this computer for each audio device and channel, never in the project.
- **More from Voltage Out.** New sources (a linkable value, run and reset), the interface's full-scale voltage, fine trims per side, and a direct mode that sends the voltages straight to an extra output pair without the mixer (with the ASIO driver on Windows for now). Existing projects sound exactly as before.
- **Split and layered instruments.** A layer channel now gives each child a key zone and a velocity zone with crossfades, a transpose and a solo, and two more ways to share notes: **Cycle** (round robin) and **No repeat** (random, never the same child twice in a row). Its Plugin tab is a layer editor with a lane per child, zone bars to drag, **Split** and **Reset**, and eight macro knobs that each turn any controls of the children at once. Layers save as `.gllayer` presets with their instruments, effects, zones and macros, and four built-in ones are included; drag one from the browser onto the channel rack. See [docs/layers.md](docs/layers.md).
- **Slide notes on every instrument.** A slide note now glides the notes it meets on every built-in instrument, drums with a pitch included, and on plugins: the note moves smoothly to the slide's pitch over the slide's length, and a slide never starts a note of its own. Instruments that cannot glide a single note bend their sounding notes instead. In the piano roll, Shift+S turns slide mode on and off (also in the command palette), and a faint arrow shows which notes each slide moves. See [docs/slide-notes.md](docs/slide-notes.md).
- **Per-note pitch for plugins.** The **Processing** tab of a plugin's window has a **Per-note pitch** choice: Automatic (the plugin's own note tuning where it has one), MPE (each note on a channel of its own, so slides and bend lines move single notes on MPE instruments, with a bend range of 2 to 96 semitones), Channel pitch bend, or Off.
- **Slides in MIDI files.** Exporting a pattern with slide notes to a MIDI file writes them as pitch bends of the notes they glide.
- **Freeze channels and mixer tracks.** Right click a channel, a mixer track or a linked playlist track and choose **Freeze**: its sound is rendered once and its plugins unload, so it costs no processing time, while its volume, pan, sends and their automation stay live. A mixer track's freeze also unloads the channels and tracks that play only into it. **Unfreeze** brings back the plugins exactly as they were. A snowflake marks a frozen unit and shows the render's progress; an edit the render depends on plays it live and renders it again a second later (or, with the option off, keeps the old render with an amber mark), and opening a frozen plugin asks to unfreeze it first. Pattern mode, live mode and playing it on the keyboard load it live for the moment. Freezes are shared in collaborative projects and go into zipped projects unless **Leave out frozen audio** is on. See [docs/freeze.md](docs/freeze.md).
- **Hibernate plugins.** **Hibernate** unloads a channel's or mixer track's plugins on this computer without a render (marked with a moon); they load again by themselves when their editor opens, a few seconds before the song plays them, or when you play them, and unload again after 30 seconds unused. **Tools > Macros** can hibernate every unused plugin at once and wake everything again, and an option does it automatically.
- **Edit in Loupe from the browser.** Right click an audio file in the browser and choose **Edit in Loupe** to open it in Loupe on its own. The file itself is never changed: your edits stay in Loupe, ready to drag onto the playlist or the channel rack, send to the playlist or save as a new file.
- **Loupe's auto-scroll has its own default.** Settings > Interface has **Loupe: Follow the playhead while playing**. The auto-scroll toggle beside Loupe's waveform (and in its View menu) changes the same setting, so your choice is kept for every Loupe and the next session.
- **Edit in Loupe from the channel settings.** A sampler channel's settings window has an **Edit in Loupe** button beside **Load sample...**; every edit you make comes back to the channel, as with **Edit sample in Loupe** in the channel's menu.

- **REX2 and Apple Loops.** Gridline now opens sliced loops in the REX2 format (`.rx2`) and Apple Loops (`.caf`), and reads the loop information in AIFF and WAV loops. The browser shows a loop's own tempo, key, slices and tags (category, genre, descriptors) and finds them in searches. A REX2 loop dropped on the channel rack or the playlist becomes a Kerf channel cut at its own slices with a pattern that plays each slice on its beat, so it follows the song tempo without stretching (hold Ctrl on the playlist for a plain audio clip). A loop that states its tempo becomes a sampler or audio clip that follows the song tempo with its pitch kept; undo once to keep it as it is. REX2 audio is decoded by Gridline itself on every system. See [docs/loop-files.md](docs/loop-files.md).

### Changed

- **The channel rack sets automation apart.** When a pattern has automation clips, a thin line with a small "Automation" caption now sits between the instrument rows and the automation rows, so you can tell them apart at a glance. Drop a channel on that line to move it to the end of the instrument rows.
- **Adjust a selection in Loupe without starting again.** Each edge of the selection now has a small grip: drag an edge to move just that edge (the pointer shows it can be dragged). Shift-click extends the selection to where you click, or from the play position when there is no selection yet. A click that moves the mouse a pixel or two no longer leaves a tiny selection behind; it only sets the play position.
- **Loupe plays what you see.** A new selection made while Loupe plays is heard at once: the loop follows the new selection straight away (playback carries on where it is when that is inside the new selection, otherwise it goes to its start), instead of looping the old one until Play is pressed again. Clicking the waveform or the ruler while playing jumps there, and Play always starts from where you last clicked. A song transport that Loupe follows starts from there too. With loop points set and Loop on, Play starts at the play position when it is inside the loop.
- **Scrubbing in Loupe works everywhere.** With the Scrub button on, or holding Alt, dragging in the waveform plays the audio at the speed and in the direction you move, up to four times normal speed; dragging along the ruler scrubs too. It now also works when Loupe edits a channel's sample or a file from the browser, not only in a mixer slot, and the play position stays where you let go, so you can find a spot by ear and then play, mark or cut from there.

- **Loop a cut clip from where it was cut.** Holding Ctrl (Cmd on macOS) and dragging the edge of a pattern, audio or automation clip that shows only part of its content (it was trimmed at either end) now repeats the part the clip showed, whatever the cut point. Dragging the edge back to the cut point in the same drag gives back the plain clip exactly as it was, with no loop; the edge snaps to that point so it is easy to hit, and inside it the clip is simply shorter. With the left edge, dragging out loops the part and sets where its loop starts. Several selected clips loop together, each from its own cut point, and the whole drag is one undo step. FL Studio export writes such a loop as repeated clips of the cut length.
- **Recordings draw as they record.** Loupe now draws a take while it records, from its first moment, in its own waveform colours with a red line where the recording has got to; Seltmann's recorder draws its take the same way. While the recording still fits, it grows from left to right; once it reaches the right edge, the view keeps fitting the whole recording into its width, smoothly as it grows, so you always see all of it, even on an hour-long take. When recording stops, the finished take shows in the normal view. While Loupe records, the readout under the waveform shows the take's length and peak level.

- **Edit automation points in the piano roll.** The automation lane under the piano roll now works like the automation clip editor: select points with the Select tool or Ctrl+drag, move them, erase them with the Delete tool, copy, cut and paste them (also between the lane and the automation editor), quantize them and set their curve from a right-click menu. A small chip under the lane's name chooses whether drawing writes stepped, linear or smooth values; linear and smooth lines are just their two ends. **Simplify points...** thins dense automation, such as recorded controller moves, to a few linear or smooth points within a tolerance you choose, with a live preview. The lane can be resized by dragging the bar above it. See [docs/automation.md](docs/automation.md).
- **Recorded controller moves take far fewer points.** Moves recorded from a MIDI controller are thinned as they are written, within 0.4 % of what you played, as knob moves already were, so a take no longer holds one stepped point per message.
- **Main buttons no longer look pressed.** The main action of a window or dialog (Start in the render dialog, OK, Save, Create, Accept and the rest) is now a raised button filled with the theme's highlight colour, and it only sinks for the moment you click it. Buttons that switch something on show it as a lit inset, and a button you are holding down looks held, so the three are easy to tell apart in every control style and both themes. Mode switches in a few plugin editors that always looked switched on now look like ordinary buttons showing their value.

### Fixed

- **Glides on several instruments.** Slides and bend lines now reach the reference models of Tercet and Lattice, Step Bass no longer glides a note while its sequencer runs, DCO Synth and Plasma follow a slide in a straight line as other instruments do, and synths that read pitch bend themselves are no longer bent twice. The sweep and sculpted kicks, the clap, the modelled membrane drum, the pad sampler, the slicer, the slice instrument and the scratch sampler (with pitched keys) now glide with slides too, so long kicks slide too. MIDI Out now passes on pitch bend, channel pressure, key pressure and controllers.

- **Changes made in an installed plugin's own window now stick.** Turning a knob, dragging a slider, choosing from a menu or flipping a switch in the interface of a VST3 instrument or effect (also one running in the sandbox, and Windows plugins run through a bridge on Linux) could jump straight back to the value it had before: Gridline handed the plugin its previous value again in the moment before the project took the new one, so a drag kept being pulled back and a menu choice was undone. This showed most on Linux and Windows, where plugins are VST3 rather than Audio Units. The plugin's own change now stays, it is still one undo step per change, and automation, controller links and Gridline's own knobs still move the plugin's controls.
- **The song position's time format and Show played chords now stay as chosen.** Picking minutes and seconds, timecode, a timecode frame rate or bars and steps from the song position's right-click menu went straight back to bars and beats, and turning on Show played chords switched itself off again. The choice now sticks and is remembered the next time Gridline opens.
- **The main window tiles on tiling window managers (Linux and the BSDs).** Gridline's window opened floating on i3, sway, Hyprland, bspwm and other tiling setups, because it starts as the small fixed size loading window and the window manager kept treating it that way. Once loading is done the window is now handed to the window manager again as an ordinary resizable window, so it is tiled, placed and sized like any other application. The loading window and dialogs still float, as they should. On other desktops nothing changes.
- **Dragging audio out of Loupe onto the channel rack or the playlist.** Loupe's window usually covers much of the channel rack and the playlist, and letting go over it dropped the audio back into Loupe itself, so nothing seemed to happen. While you drag audio out, Loupe now turns see-through and lets the drop reach whatever is under it, and it no longer accepts its own audio.
- **Edit sample in Loupe on a Kerf channel with named regions.** Loupe opened such a channel's sample empty ("Cannot open"); it now opens the sample, and its edits come back to the channel with the region names kept.
- **Cut groups and mono work on the Sampler.** A channel set to be cut by another now stops when that channel plays, and mono now ends the previous note, also when the earlier note was a short step whose one-shot sample was still ringing (before, only notes still held down were cut, so most drum steps rang on). This works the same for notes played live from the computer keyboard, a MIDI keyboard or the pads, which used to ignore cut groups, mono and the voice limit altogether. A cut is exact to the sample and fades out over a few milliseconds, so it never clicks. The voice limit now counts ringing one-shots too. The built-in drum instruments also stop when cut, where before a cut had no effect on them.
- **Pitched samples cost far less processing time.** Every note the Sampler plays at another pitch than its root, or from a file at another sample rate than the project (a 44.1 kHz sample in a 48 kHz project), now takes about a quarter of the processing time it did, and notes pitched up about half, with exactly the same sound. Holding a few notes on a long sample could push the audio load to 100% on some computers, and one-shot notes played again and again, which each play to the end of the file, added up quickly. The drum sampler, the slicer, the granular instruments, the multisampler and the other sample players, and pitched or stretched audio clips, gain the same way.
- **Knobs follow every drag.** Knobs over whole numbers or choices no longer stick when you drag slowly or hold Shift for a fine drag: the piano roll tools (Randomize: Octave, Range, Max notes, Bars; Limit; Riff), the channel settings and envelope pages, the step tools, the chop window, the tracker and sampler editors and the slicer's Choke now move with the pointer from where you grabbed them and stay where you let go. Choice knobs no longer jump on the first pixel of a drag, the wheel moves whole-number knobs one step a notch (trackpad scrolling adds up), and a double-click on the piano roll tools' whole-number knobs puts them back on their default.
- **Large LV2 bundles load.** A bundle that holds hundreds of plugins in one folder used up gigabytes of memory within seconds while Gridline checked it, so its plugins never appeared and the check started again at every launch. Gridline now reads each plugin's own description files once and shares them, closes each plugin as soon as it has been checked, and looks through your other LV2 folders once instead of once per plugin, so the whole bundle is checked in about the memory of one plugin and is remembered afterwards. A plugin in such a bundle that will not start is now listed on its own with the failed plugins, and the rest of the bundle loads. A VST2 shell of many plugins is also checked one plugin at a time.
- **A plugin that misbehaves while it is scanned can no longer use up the computer's memory.** Gridline checks every installed plugin in a separate helper process the first time it starts, while you work. A plugin that got stuck in a loop while it loaded could make that helper, or a program the plugin started itself (such as a bridge for Windows plugins), grow without limit until the whole computer ran out of memory, often within seconds of opening the demo song. Each plugin's check now has a memory limit that scales with the computer's memory (a quarter of it, between 2 and 8 GB), and it is stopped early if the computer is close to running out of memory, so a check can no longer push the computer into swap. A plugin stopped this way is listed with the other plugins that failed, as "used too much memory while loading", and is skipped by later scans until you retry it or rescan everything. Programs a plugin starts during its check or in the sandbox are now always ended with it, never left running. Checking a CLAP file that holds many plugins also takes far less memory, as each of its plugins is closed once it has been described instead of all of them staying open together.
- **M4A samples have their exact length on Linux and the BSDs.** AAC files in .m4a decoded with FFmpeg no longer carry up to a tenth of a second of extra silence at the end, and no longer start late: Gridline now reads the start delay and the real length the file states and trims to the sample, as macOS and Windows do, so loops close cleanly and tempo fitting lines up.
- **The loop badge by the pointer.** The badge that shows a Ctrl (Cmd) edge drag will loop is now larger, so its loop arrows read as arrows instead of a small ring, sits below and to the right of the pointer, clear of the resize arrows, is drawn crisply at every interface size and stands out in both themes, and appears only where a drag of that edge really loops: not over a clip's body, not on the left edge of a clip at the very start of the song, and during a drag only while the clip loops. It appears and goes the moment the key is pressed or released. The loop mark in the title bar of a looping clip is larger and readable too.
- **A sampler's start and end handles now trim its audio clips.** Moving the START or END handle on the sample page changes what every playlist clip of that channel plays, in playback, renders, exports and bounces, and the clips' waveforms, lengths, loops, trims and slips follow. The handles can no longer be dragged past each other, the waveform at the top of the channel settings is now the one you drag them on, and the sample page no longer waits for a long file to load. The sample page's processing (normalize, remove DC, polarity, swap stereo and the fades over the region) and its precomputed effects now also reach the clips, as they do the notes.
- **Reversing an audio clip keeps its own audio.** A trimmed clip used to play another part of the file once reversed; it now plays the same audio backwards.
- **FL Studio projects with trimmed samples.** Audio clips on a channel whose sample start was moved in FL Studio now open at the right place, and export back the same way; warped clips are written to FL Studio as one stretched clip per section.

## 0.7.4

### Added

- **Chromebook support.** Gridline installs on Chromebooks in ChromeOS's Linux development environment, with the Linux `.deb` for Intel and AMD Chromebooks or for ARM ones (MediaTek, Qualcomm); every release is now installed and started on Google's own Chromebook Linux image before it is published. On a Chromebook a new installation starts at 48000 Hz with a 1024 frame buffer, which is what the Chromebook's Linux sound system runs at, the file dialogs get a **Shared folders** button and explain how to share folders with Linux, and the MIDI settings say how to give a USB MIDI device to Linux. See [docs/chromeos.md](docs/chromeos.md) for the install steps.
- **Debian support.** The Linux `.deb`, AppImage and `.tar.gz` now install and run on Debian 11, 12 and 13 and on Ubuntu 22.04 and later, for Intel, AMD and ARM computers, and on Chromebooks whose Linux is still Debian 11; every release is installed and started on each of them before it is published. See [docs/debian.md](docs/debian.md) for the install steps.
- **Keyboard layout for the typing keyboard.** Settings > MIDI and the typing keyboard menu now let you pick Standard (QWERTY), QWERTZ, AZERTY, Dvorak, Colemak or Follow the computer; the keys are named after that layout and still play by where they sit. Standard is the default.
- **Window layouts (preview).** Choose how the main window is arranged in Settings > Interface, in View > Window layout or from Layout on the welcome screen. Studio is the arrangement you know and stays the default. Live and Logic are marked as previews: they arrange the windows and bring their own shortcuts and menus, and more of each style follows in later updates. Every layout keeps your theme: the slate look, or the colours, accent, control style and interface size you chose, stay the same whichever layout you pick. Live puts the browser on the left, the playlist and the live pads as one main view (Tab switches between them) with the mixer under it, and the editors and channel settings along the bottom; the status bar gets Editor, Channel and Mixer toggles. Logic puts the tracks in the centre with the browser on the left, one pane at the bottom that the mixer and the editors share (opening one closes the other, and each keeps its own height), the undo history on the right, and a two-row top bar with the menus above the pane toggles, the transport, the position, tempo and metre display and the modes.
- Each layout can bring its own shortcuts and menus; two ticks in the chooser let you keep yours. Live uses single keys such as O for the metronome, M for the typing keyboard and Ctrl+D to duplicate. Logic uses single letters for the panes and the transport (B browser, X mixer, E editors, P piano roll, R record, K metronome, Return to the start), and its menus read File, Edit, Track, Patterns, Navigate, Record, Mix, View, Tools and Help, with the settings in the menu on the logo. On macOS the Logic shortcuts tell Command from Control. Every menu command stays reachable in every layout, and Settings > Shortcuts shows which shortcut set is in use; your changes to shortcuts are kept per set.
- Each layout remembers your own changes to it in its own file, and switching back restores them. The layout is your own setting: projects never store or change it, a project saved in one layout is the same file as in any other, collaborators can each use their own, and the welcome screen looks the same in every layout.
- **Projects open with whichever format of a plugin your computer has.** A project made with the Audio Unit of a plugin now opens on a computer that has only its VST3, VST2 or CLAP version (and the other way round), with that version and its settings. Gridline checks that it really is the same plugin: the plugin's ids where the formats share them, its name and maker, whether it is an instrument or an effect, its outputs and its parameter names. The plugin's own saved state moves into the other format where the plugin allows it; Gridline then reads the parameters back, and if they do not match, sets every parameter that has the same name instead. Automation follows its parameters. The open message lists every plugin that opened in another format and what was carried over. When the match is not certain, nothing is swapped: the plugin's window lists the possible matches and you pick one. Projects now remember every format each plugin was installed in, so a project can go back and forth between computers. Your preferred format in the plugin manager decides when several are installed. FL Studio projects use the same matching, and now carry the plugin's settings over to the other format too.
- **Gridline plugins open live in FL Studio.** When you export an FL Studio project, Gridline instruments and effects that FL Studio has no exact counterpart for (Seltmann, Plasma, Platter, Junction and the rest) can now be written as the matching Gridline Suite plugin with all their settings. With the Gridline Suite installed, FL Studio opens them live and editable instead of as rendered audio. Samples the plugins use travel inside their settings (up to 64 MB of files per plugin), so they play on the other computer too. Choose how in **Settings, Project, FL Studio export**: Gridline Suite plugins with muted audio copies (the default: each instrument also gets a muted copy of its channel playing it as audio, for computers without the Suite), Gridline Suite plugins only, or rendered to audio as before. **File, Export, FL Studio project** shows the choice and can change it for one export or for the project. The export report lists how each channel and effect went. Gridline reads these plugins back as its own when it opens the project again, with their exact settings, whether or not the Suite is installed. `gridline export-flp` and `export-flp-zip` take `--gridline-plugins fallback|suite|render`.
- **Tracker modules open as projects.** Open an .xm or .mod file like any project (File > Open, the browser, a drop on the window, the start screen or `gridline render`) and Gridline converts it: every instrument becomes a channel with the new Tracker Sampler, holding its samples, envelopes, key map, vibrato and fadeout; every note becomes a piano roll note; the order list becomes playlist clips, one per stretch of a pattern as the module plays it, so pattern breaks, jumps and loops play in order. Speed and tempo changes become tempo automation, the global volume automation of the master volume, and a song that loops gets a loop marker. Each note keeps the effect column of the rows it sounds through, so slides, vibrato, arpeggios, retriggers, note delays and the rest play as the module does, with the same pitch tables, volume ramps and interpolation. The converted project opens from the cache, the original file is never changed, and Save asks where to save the project.
- **Tracker Sampler**, a sampler instrument for tracker instruments: up to 16 samples on a key map, forward and ping-pong loops, volume and panning envelopes, fadeout and auto vibrato, a choice of interpolation (none, linear, cubic, sinc), linear or period-table pitch, and click removal. Notes you draw or play on it use its own ticks per row. It also plays as an ordinary sampler: drop audio files from the browser or the desktop onto it to add samples, then name, reorder or remove them and set each one's volume, pan, fine tune, transpose and root key. The selected sample shows its waveform, where you drag the sample start and end and the loop points, pick no loop, forward or ping-pong, and zoom and scroll. The volume and panning envelopes are editable (add, move and delete points, set the sustain point and loop, switch them on or off), fadeout and auto vibrato (shape, sweep, depth, rate) are knobs, and dragging across the key map gives those keys the selected sample. Played notes follow their fine pitch, pan and the pitch wheel, and stereo files play in stereo. A new Tracker Sampler stays silent until you drop a sample, pick one of its built-in sounds or choose one of its six presets. Imported modules sound exactly as before. The Tracker Sampler is also part of the Gridline Suite, for other DAWs.
- **More Tracker Sampler settings.** Name the instrument; keep six volume and six panning envelope shapes of your own in numbered slots (a click uses one, a right click keeps the current envelope there; a volume slot also keeps the fadeout and vibrato); play any sample at 8 or 16 bits; copy a sample into a free slot; add a sample from a file with Add file... at the end of the samples list, just as dropping the file adds it. A MIDI panel sends the notes the instrument plays to your MIDI output on a channel and program of your choice, sets how far the pitch wheel bends, and can mute the instrument's own sound.
- **Edit samples in the Tracker Sampler.** Drag across the waveform to mark a range, then use the edit menu (the button beside the zoom buttons, or a right click on the waveform): cut, copy, paste over the range or where you clicked, crop, delete or silence the range, reverse, fade in or out, a volume ramp from one level to another, normalise, remove DC offset, resample by semitones (the transpose follows, so the pitch stays), echo, mix another sample in, crossfade the loop, trim what follows the loop, select all and zoom to the range. Every edit can be undone; the edited audio is saved next to the project and travels with it, its zip and collaboration.
- **Hear a sample with one click.** Clicking a sample in the Tracker Sampler's list plays it once through the instrument at its own pitch; click it again or press Esc to stop, and a click on another one plays that instead.
- **The Tracker Sampler's key strip lights up** every key that sounds, from the piano roll, the song, the typing keyboard, MIDI and the strip itself; pressing a key on the strip plays it.
- `gridline import-module <file> -o <project>` converts a module into a project file and reports what it found.
- **M4A and AAC samples.** Audio files in .m4a (AAC, HE-AAC or Apple Lossless) and .aac now load everywhere samples do: the browser and its preview, channels, audio clips, drag and drop, the file dialogs, collaborative projects and FL Studio projects whose clips use them (they used to play as silence). On macOS and Windows the system's own decoder reads them; on Linux and the BSDs Gridline uses FFmpeg, the copy that comes with the package or the one installed on the computer. A file that cannot be decoded shows that it could not load, with the reason.
- **Double click to open.** Gridline projects, zipped Gridline projects and tracker modules (.xm, .mod) now open in Gridline from the file manager. On macOS a file opened while Gridline is running, or one that starts Gridline, now opens as a project (it used to be lost while starting, or dropped where the pointer was). The Linux packages add the zipped project type and list Gridline for tracker modules, and the Windows installer adds Gridline to Open with for projects and modules, on by default. Gridline never takes over another program's file types: where a type already has a default program, choose Gridline once with Open with. See [docs/opening-files.md](docs/opening-files.md).
- **Esc takes you back to the playlist.** Press Esc in the piano roll, the channel rack, the mixer, a plugin window, the automation editor or the audio editor and the playlist comes to the front, also where it shares a pane with other windows. Esc still closes menus, popups and dialogs, cancels drags, selections and text edits and clears a selection first, and only goes on to the playlist when it has nothing else to do. Change or remove the key under Back to the playlist in Settings > Shortcuts.
- **Plugin window size.** Settings > Interface has a new Plugin window size, from 75 % to 150 %, for every built-in instrument and effect window: text, knobs and spacing change together and the window follows. Each plugin window's menu still has Editor size, now from 75 % to 200 %, to give one plugin a size of its own; it is remembered for that plugin. Plugins you install keep their own size.

### Changed

- **Plugin windows are more compact.** Built-in instrument and effect windows now open at 85 %, closer to the size of the mixer and the channel rack, and no longer grow by themselves on large screens. To get the earlier sizes back, set Plugin window size to 100 % in Settings > Interface, and tick Open small plugin windows larger on big screens there for the earlier automatic size.
- **The channel rack is never taller than its channels.** A floating channel rack now stops growing when it is as tall as its header, its rows and the add button; it grows when you add a channel and shrinks when you delete one. The add button sits right under the last channel, also when the rack is docked in a taller panel, where the space below stays empty. A long list of channels still scrolls.
- **Projects now end in .gridline, zipped projects in .gridlinez.** macOS and Windows tell a file's type by its last extension only, so they saw a Gridline project as a plain JSON or zip file and opened it in another program. With a single extension of their own, Gridline projects open in Gridline by double click on every system, and the Windows installer now makes Gridline the program for them. Projects saved with the earlier names (.gridline.json, .gridline.zip) still open everywhere, Save keeps their name, and Save as can still write the earlier names for someone on an earlier Gridline. New saves, templates, backups, recovery copies, exported project folders and collaborative projects use the new names. See [docs/opening-files.md](docs/opening-files.md).
- **One Gridline at a time on Linux and Windows.** Opening a file while Gridline is running now opens it in that Gridline, as on macOS: it asks about unsaved changes first and comes to the front. Starting Gridline again from the menu brings the running window forward. To run several windows side by side, start Gridline with the environment variable `GRIDLINE_SINGLE_INSTANCE=0`.
- **A clearer layout chooser.** Layout on the welcome screen now opens its own page with large cards, each showing a true miniature of its layout (the control bar, browser, playlist with clips, mixer strips, editors and side panels, in your theme and accent colours); the layout in use has an accent ring and a check mark. The arrow keys move between the cards, Enter picks one and the window switches at once, and Esc goes back. Settings > Interface uses the same cards, with Matching shortcuts and Matching menus as switches with a short hint each and Reset this layout beside them. View > Window layout shows a small picture of each layout next to its name.
- **A new instrument opens straight away.** Adding an instrument opens its window at once, in front, ready to play: from the channel rack's add menu, Insert instrument below, Replace instrument, the plugin picker, the browser, a drop on the channel rack or the playlist, and the playlist's new instrument tracks. Installed plugins open their own window. Opening a project, undo and redo, a collaborator's changes and pasted or cloned channels open nothing.
- **One light per channel, before its steps.** Each channel rack row now has a single green light, between the channel's name and its steps: it is lit while the channel is on, dark while it is muted, and glows brighter while the channel plays. Click it to mute or unmute. Right-click it (or Ctrl+click) and only that channel plays; do it again and every channel goes back to how it was. Each is one undo step. Shift+click adds the channel to the selection or takes it out. The separate light at the start of the row is gone, so the knobs, the mixer track number and the name move a little to the left. Selected channels show a frame around their name.
- **Clips loop only when you ask.** Dragging the right edge of a clip now simply makes it longer: a pattern clip is empty past the end of its pattern (notes you add there later fill it), an audio clip shows more of its file and stops at the file's end, and an automation clip holds its last value. Hold Ctrl (Cmd on macOS) while you drag the edge to loop the clip instead: its content repeats as far as you drag, for pattern, audio and automation clips alike, and pressing or letting go of the key during the drag switches between the two. Ctrl with the left edge loops the clip and sets where its loop starts. Looping now belongs to each clip, so two clips of the same pattern can differ; the clip menu has a Loop switch, a looped clip dragged back to its own length stops looping, and looped clips show a small notch at each repeat. Projects made before this, and FL Studio projects, play exactly as before.
- **Menus are in a clearer, more consistent order.** The main menus and the right-click menus of channels, patterns, clips, playlist tracks, mixer tracks, effect slots and plugin windows now follow the same order: open, create, edit, properties, arrange, and removals last, so Delete is always at the bottom and Rename, Colour and Icon are always together. Options and Tools are split into sections (Playback, Recording, Keyboard and controllers, Editing; Composing, Audio and recording, Macros, Clean up). The two Recording options menus are now one, also on the record button; Plugin manager moved from File to Options. Every command and shortcut is unchanged. A few duplicate entries in the channel rack menu are listed once, and stray dividers at the top of the playlist View menu and in File inside another host are gone.
- **Piano roll notes are slightly see-through.** A note lying under or over another on the same key now shows through it, and where two notes overlap the colour gets denser, so an accidental double note is easy to spot. Note edges, names and marks stay sharp, in the dark and light themes and with any accent colour.

### Fixed

- **No dropout when you add a Junction during playback.** Junction and Junction FX now prepare the built-in instruments and effects inside them before they start playing, as other plugins do, so adding one while the song plays no longer risks a short dropout. Installed plugins inside a Junction are left alone until they play.
- **Download installer gets the .deb on Debian, Ubuntu and their relatives.** On Zorin OS, Linux Mint, Pop!_OS, elementary OS, Debian on a Chromebook and any other system that calls itself Ubuntu or Debian based, the update popup downloads the .deb package for your processor, also when you run Gridline from the AppImage. When the system description cannot be read, Gridline looks at how the system installs software, and a Gridline installed from our .deb keeps getting the .deb on systems we make no package for. The AppImage is offered only where no package fits. Versions before 0.7.0, its test versions included, download the AppImage on every Linux system, so if you are updating from one of those, install the .deb from the release page once.
- **A clip dragged back to the start of the playlist lands on the beat.** A clip, or a note in the piano roll, that sits slightly off the beat and is dragged past the start now stops there exactly, on the first beat, instead of keeping its small offset. Clips dragged together stop together, the first one on the beat.
- **Gridline starts on graphics that offer only older OpenGL.** Where OpenGL 3.0 is refused, as on some virtual machines and older drivers, Gridline now falls back to an OpenGL 3.0 compatibility context or to OpenGL 2.1 instead of failing to open its window, and says which one it uses when started from a terminal.
- **Idle Plasma instances no longer load the processor.** A Plasma that is not playing used to keep running its whole effect rack and its oversampling filters for good, so a project with many complex Plasma sounds (or Serum sounds opened in Plasma) could overload the processor with the transport stopped. Once its notes and effect tails have died away, Plasma now rests and costs next to nothing until the next note or a change to its settings wakes it, at once and with the same sound. It waits for the longest delay or pre-delay in its effects, so no echo is cut, and its silence lets the effects on its mixer track rest too. Exports are unchanged.
- **Scanning plugins no longer freezes Gridline.** The window stays responsive while plugins are scanned, however many you have. A few plugins are scanned at a time, so one that hangs or starts slowly no longer holds up the rest, and the scan shows which plugin it is on and, after a few seconds, how long that plugin has taken. Rescanning a folder no longer freezes the window while the folder is searched, folders that link back to themselves are searched only once, and the scan says when it is still looking for plugins. `gridline scan-plugins` prints its progress too.
- **Menus no longer flicker.** Menus, lists and hints opened in the main window now always stay inside it. Before, a menu that reached past the window's edge, such as the add instrument menu of a Channel Rack docked at the right, opened in small separate windows that came and went as you moved through it, and on some Linux desktops the whole window flickered each time. On Linux and other X11 desktops, menus, lists and hints of a window you have moved outside the main one also open inside that window wherever they fit, and Gridline's windows no longer ask the desktop to stop compositing them, which made some desktops flash the whole screen when a menu or plugin window appeared over a full-screen Gridline. On macOS and Windows, menus of a window you have moved outside the main one can still reach past that window's edge.
- **Large plugin collections stay smooth.** With thousands of plugins, the plugin manager, the browser's plugin list and the plugin picker no longer sort and redraw every plugin on every frame, so the window stays quick during a scan and after it. The plugin folders lists no longer check the disk on every frame, so a slow network folder cannot hold up the window.
- **Plugin files holding hundreds of plugins finish scanning.** A plugin file that holds many plugins (such as a shell of a whole bundle) was given up after a minute and listed as failed. The time limit now counts from the last plugin it described, so it finishes; a plugin that stops answering is still given up after a minute.
- VST3 folders are searched like the other formats: each folder once even when it links back to itself, plugin names in any letter case, and the search stops at once when you quit. Before, a folder that linked back to itself could list the same plugins hundreds of times.
- On Windows, 32-bit plugins in a 64-bit plugin folder and the helper files plugins install beside themselves are recognised without being loaded, so they no longer take a scan slot each, and a plugin that crashes while it is scanned no longer shows a Windows error window.
- A scanner process that cannot be stopped no longer holds up the rest of the scan, and a file name the system cannot read no longer ends the scan.
- The Live pads window, opened in the default layout, now opens in front of the channel rack, so its "No live area" note is no longer hidden behind it.
- In a song with many tempo changes, a note placed exactly on a sample no longer slips one sample late. The tempo map's rounding could push such a note to the next sample after a minute or so of tempo changes.
- **Exporting project bones over an earlier export can no longer damage it.** The new files are now written to a hidden folder beside the export first and replace the earlier ones only once every file was written, so a Cancel, a full disk or a file in the way leaves the earlier export exactly as it was.
- **A graphics driver restart at the start screen or in the crash report window is now handled as in the main window.** The window is drawn again where the driver allows it; where it does not, Gridline writes a graphics report (at start) or keeps the crash report for the next start and says so, instead of ending without a word.
- **Plugin windows come back after a graphics driver restart.** A plugin's own interface that was open is closed and opened again, so it draws again instead of staying black.
- **Track Receipt tells you when a calculation fails.** It used to note the error only in the log and show nothing; it now says that the receipt could not be calculated, with the reason, and that nothing was changed.
- **Work that stops moving on is reported.** When a render, a background file job or a render to a sampled instrument shows no progress for a long time, Gridline says so with the job's last step, and says again when it moves on, so a plugin or a disk that stopped responding no longer looks like a frozen window.
- **Pitch Shifter's Music engine in imported projects** now reads its grains one sample sooner and sums them into a soft clip, so denser grains play louder and sit one sample earlier against the dry signal. Projects made before keep their sound.
- **The vocoder's output enhancer in imported projects** no longer gets louder and brighter as its amount goes up; the amount now changes the sound only a little. Projects made before keep their sound.
- **Bucket Chorus and Retro Phaser start their sweep at the start phase when playback starts.** With the transport stopped the sweep runs from its beginning; the start phase applies when you press play.
- **Imported sampler channels stretched to a time follow the tempo.** In an FL Studio project, a sampler channel whose sample is stretched to a number of beats now keeps that length in beats when tempo automation changes the tempo: a stretch follows the tempo as it changes, and a channel that resamples plays each note at the rate the tempo gives when the note starts. Before, such a channel stayed fitted to the project tempo.
- **Pitch automation on imported channels plays smoothly and replaces the pitch knob.** Automation or a controller on the pitch knob of a channel in an FL Studio project now moves the pitch in fine steps instead of whole semitones, and it takes the knob's place instead of adding to it, on sampler channels and on audio clip channels alike; the channel's fine tune stays on top. Projects made in Gridline keep their whole-semitone steps, so they sound as before.
- **Automation of a missing plugin survives the FL Studio export.** A plugin that is not installed and plays through its built-in stand-in (such as the tape stop effect opening as Gridline Suite Mini Halt) now keeps its automation clips when the project is exported to FL Studio: they are written onto the plugin's own settings, and the exported project opens with them still linked, on a computer with the plugin and on one without. Missing VST3 effects of that kind are now written with their parameter list, so FL Studio links the clips to the right settings.
- **Instruments in a patch keep their notes when the patch's latency changes.** Switching an effect in a patch to a mode with more or less latency used to stop every note its instruments were holding; they now play on.
- **Reports follow automation.** A channel or mixer track that starts muted or at zero volume but is brought up by its automation now counts as heard in the track reports, and its plugins are listed as used; one that its automation keeps silent is still left out.
- **Replacing a plugin shows the busy indicator.** Loading a plugin or handing it its settings while you replace one now shows the busy indicator instead of looking frozen.
- **An idle string instrument holds far less memory.** A Twin String instrument that is not playing now keeps about 14 MB instead of about 56 MB; its string memory is only taken as voices play. It sounds exactly as before.
- **Plasma loads wavetables, samples, impulse responses and noise files that arrive late.** In a collaborative project, a file that reaches your computer after the project opened now loads into Plasma as soon as it arrives; before, the project had to be opened again.
- Settings > MIDI: the soft pedal's velocity slider no longer covers its own caption and value; both now sit beside it.

## 0.7.3

### Added

- **Track Receipt calculates each song's revenue share.** Choose the category on the license and the share follows from it and the measured length, in the window, the spreadsheet and the PDF: category A 10, 25 or 50%, B 5, 15 or 30%, C 2, 10 or 20%, for up to 2, 15 or 60 seconds. A share you type replaces the calculated one. Check the figures against your license.
- **Crash reports and project recovery.** If Gridline crashes or freezes, a window now says what happened and gives you a code to copy. The code tells developers exactly where it failed: version, system, graphics card, the failing thread and functions, and your last few actions. It holds no file paths and no project names, and Gridline never sends it anywhere: you choose where to share it. See [docs/crash-reports.md](docs/crash-reports.md).
- After a serious error Gridline cannot safely keep running, so it closes and offers to restart with a recovery copy of your project. On macOS, Linux and the BSDs that copy is normally saved at the moment of the crash; on Windows it is the last automatic one, from at most about 30 seconds earlier. Lesser errors (a failed load, export or plugin call) show the same window while Gridline keeps running.
- If the interface stops answering for 30 seconds, the window offers to keep waiting or to force a restart, and closes itself if Gridline comes back. Long work such as loading, exporting and saving does not trigger it, nor does a computer waking from sleep.
- **Tap tempo button** beside the BPM display in the top bar. Click on every beat: from the second tap the song tempo follows, a whole run of taps is one undo step, and a pause of over 2 seconds starts over. It shares its count with Tools > Tempo tapper and the controller action, can be linked to a controller from its right-click menu, and has a key, Ctrl+Shift+T, which Settings > Shortcuts can change.

### Changed

- **Projects on web and network drives open properly** (Google Drive, Dropbox, OneDrive, iCloud, rclone, gvfs, SMB, NFS): the file is read once, off the interface, with a progress bar in bytes, the speed, the time left and a Cancel that returns at once. If the drive stops answering the window says so; if the file is gone or cut short, it says why.
- A zipped project on such a drive unpacks to a cache on this computer (`gridline/unpacked` in your cache folder) and nothing is written to the drive. Opening the same archive again needs no read, and when the drive is offline the last copy opens with a note. Save asks where to put the project. The cache trims itself oldest first, down to 16 GB, and keeps anything a saved project points at.
- **Zipped projects keep their folders clean**: every zipped project, on a local disk too, now unpacks to the same cache instead of beside the archive, and Save asks where (a Gridline zipped project offers to replace its archive). Folders earlier versions unpacked beside an archive are left alone, and projects saved there keep working. When a project from a local disk closes its copy is deleted, unless it is open in another Gridline, a saved project uses it, or a recent backup or a crash recovery copy uses it; a copy from a web or network drive stays, for quick and offline opening. What a crash leaves behind is cleared in the background at the next start.
- **Clear cache**: Settings > Files shows the size of the unpacked project cache and clears everything nothing needs, then says what it kept and why.
- An archive with several projects asks which one to open. One named like the archive opens straight away, and copies in backup folders and macOS leftovers no longer count as projects.
- Gridline never writes to a drive on its own: autosave of a project on a web or network drive is off there, and timed backups stay on this computer even if they are set to the project folder. Save still writes there.
- The browser's search index no longer walks web or network drives, and Search folder in the missing samples window stays in the folder you pick there. Folders on a drive still browse one at a time.
- **Crest has a cleaner interface**: every knob, button and caption now sits on one grid with one knob size, in the same panels as the other flagship plugins, in both themes.
- Crest's display buttons stay readable when they are off, and the spectrum heatmap button has its own icon. It looked blank when off.
- Crest in new instances and FL imports now reacts differently while a control moves. A new band delay or master attack length starts from an empty delay, a new sustain window or detector starts fresh, a new curve stage count restarts its envelope, a new slope restarts the engine, and stereo separation moves in a short straight line, the same at every buffer size. Moving the delay or attack knob therefore makes a short gap in the sound. Saved projects and saved presets keep the behaviour they had, and sound the same when nothing moves.

### Fixed

- A Cancel during the last moment of saving a zipped project or exporting project data files now cancels too. It used to be ignored there, so the archive or the folder stayed. A Cancel after a sample or clip export has written its file no longer reports the export as cancelled.
- Whole-number knobs in the piano roll script window follow a slow drag. A knob such as a ratchet's Repeats only moved when the mouse moved fast, because each small step was rounded away; now every drag adds up. A double-click on one of these knobs goes back to the script's own value, where before it did nothing.
- A collaborative project you are working on from another computer no longer reads "is working on it now" with no name on the welcome screen. It says so in full.
- Soloing and unsoloing no longer freezes a large project. Bringing back a playlist track used to rebuild every clip's playback and throw away most of its finished renders (up to 7 seconds of frozen interface on a project with thousands of clips, then minutes of extra CPU); a muted clip now keeps them, and a toggle takes about 50 ms. Soloing a channel no longer does slow tempo work once per channel (over 2 seconds on the same project, now under 50 ms)
- Playback uses far less CPU on large arrangements. Channels and effects with nothing to play used to run all the time; sampler channels with no sound near, and EQs, delays and reverbs that have gone fully silent, now rest until sound reaches them. It applies to playback only: exports run everything and stay bit-identical, and modulated effects never rest. A project of 222 channels and 125 inserts went from about 39% to between 19% and 31% on the CPU meter, and the work done per block fell by about 40%
- Far fewer dropouts just after opening a project, while clips are still rendering in the background (from about 184 to 2 at a 512 sample buffer on a large project, from about 213 to 8 at 256). A clip's realtime stretch now does its start-up over the blocks before the clip and shares each of its steps out over several blocks, with the same output. Background renders use several low-priority threads and start with what is about to play (about four times faster on an 8 core machine)
- **Plugin interfaces open on Wayland desktops where they failed.** On systems whose SDL chooses Wayland by itself (Arch, CachyOS and other systems where SDL2 is sdl2-compat over SDL3), or with `SDL_VIDEODRIVER=wayland` set for the whole session, every plugin's Interface button said "its interface could not be attached". Plugin interfaces are X11 windows, so Gridline now runs through XWayland whenever it is available, and starts on Wayland only when there is none or when `GRIDLINE_VIDEO_DRIVER=wayland` asks for it. If an interface still cannot open, the message gives the real reason and repeated clicks no longer stack it.
- Platter no longer plays out of tune after a scratch with Sync on. When you let go, the deck was out of phase and ran up to 4% fast or slow until the beats lined up, sometimes for several seconds. It now falls back in phase at once with a short crossfade and plays at its right speed. In Deck to Deck sync, the following deck no longer chases a deck being scratched.
- Turntable no longer lurches when you scratch across the start of the sample. For a moment the record was told to go a whole turn away.
- **Clips and notes placed off the grid stay where you put them.** A click no longer moves or resizes them. Dragged, they now move by whole grid steps from where they are, one bar at a time with Bar snap for example, and keep their offset instead of jumping onto the grid. Hold Alt to drag freely. Resizing still lands on the grid.
- **The typing keyboard follows where keys sit, on any keyboard layout.** On Dvorak, AZERTY, QWERTZ and others the notes came out jumbled, some keys played nothing, and the octave and velocity keys moved, because Gridline read the character a key types. The bottom row now plays C D E F G A B whatever the layout, with plugin windows in focus too, and the hints, the menu and the keys list in Settings name the keys as your layout does.
- **Builds with assertions on start again.** A build without `NDEBUG`, such as the AUR source package (`CMAKE_BUILD_TYPE=None`), stopped at startup wherever detached windows are available, because the interface's own consistency check failed on the first frame. Release downloads were not affected.
- **A crash during long mixer edits.** Dragging a send level for a long time, or any fast run of edits, could crash Gridline if a block of audio was taking long at that moment, as when a bridged plugin stops answering for a while. The audio engine could free a compiled copy of the project that the block was still using.
- FL Studio projects: notes on sampler channels now play at the level the project's settings give them, up to about 6 dB quieter than before at usual channel volumes. The louder level pushed drums into the master limiter and made them sound distorted. Import the project again to get the new levels.
- Hybrid Synth's wave files and Halftone's image source no longer stay missing when the file was still loading as the project opened.
- The Convolver no longer stays silent after opening a project. If its impulse file was still loading when the project opened, the wet signal stayed off until you changed a setting; it now comes in as soon as the file has loaded.
- The Convolver in FL Studio projects now sounds as saved. Volume, pan and separation curves spread over the whole impulse instead of being read as seconds, so a reverb tail decays at the right time and level. Separation, pre-delay, stretch, gain at other sample rates and automation of its knobs also follow the stored values more closely.
- Stretched and pitched audio from files above or below the project's sample rate is much cleaner. The stretch sized its windows for the output rate instead of the file's own, so a 96 kHz bass at a 44.1 kHz output had far more noise between its notes' partials and an unsteady pitch. A file now sounds the same whatever its sample rate. Stretched clips and sampler notes from such files can sound different in existing projects, in the Rubber Band mode too.
- A low monophonic sound such as a bass line, slowed down in a playlist clip, now keeps its waveform and its attacks: its render repeats whole periods of the sound instead of going through the spectral stretch. Gridline decides this from the audio, per clip. Other sounds are not affected, and clips that play back faster stay on the spectral stretch.
- Two clips of one pattern back to back no longer lose the new note when a long note ends right where the same note starts again. Installed plugins of every format (VST3, CLAP, VST2, AU, LV2), the MIDI Out channel and the two built-in instruments that had the problem now end the old note first or keep the new one, in playback and in exports.
- FL projects: an audio clip on a channel stretched to a time more than four times longer or shorter than its sample now lasts that time and, when the channel resamples, is heard at the matching pitch.
- Audio clips from very high sample rate files (192 kHz, for example) pitched far up or read fast no longer play flat while the clip's final render is being prepared.
- Adding a built-in instrument or effect during playback no longer risks a dropout on its first block: some sized their buffers on the audio thread, and the slowest took longer than a whole block. They now do it before audio reaches them, and they sound exactly the same.
- FL Studio projects with a tempo automation clip now play at the right tempo. The importer used one fixed range for every tempo clip; it now reads the range each automation clip stores, for every kind of target, and FL export writes it back.
- Audio clips in imported FL Studio projects no longer stop before their end when the tempo is automated. Clips whose channel is stretched to a time and keeps its pitch now follow the automated tempo. On a channel that resamples each clip takes the speed that fits it at the tempo where it starts and holds that speed and pitch while it plays, so its audio reaches the clip's end and stays on the beat in every tempo section. The clip end FL works out from the project tempo no longer cuts the audio.
- FL projects: Limiter now keeps its 0.2 dB safety margin when the project says so. The stored setting was read the wrong way round, so most imported limiters ran 0.2 dB above their ceiling.
- Automatic crossfades in FL Studio projects with tempo automation are recognised as automatic, so they follow the overlap of their clips.
- AIFF samples now load.
- Browser moves and renames now follow file references that spell a path with different letter case, on Windows (including the drive letter) and on macOS volumes that ignore case. Linux and case-sensitive macOS volumes still treat them as different paths. Stored paths keep their spelling.
- Undoing a rename that only changes letter case now restores the old spelling on disk on those systems, and a file's sidecars now follow such a rename.
- Audio Units: fixed a race between edits made in a plugin's window and Gridline reading the plugin's parameter list again. It was found with a test tool and never seen crashing.
- **Saving a zipped project, exporting project data files or bones, and exporting a clip or sample as WAV no longer freeze the window.** They now run in the background with a progress bar and Cancel, and you can keep editing. Rendering mixer tracks to wave files and saving into a new folder work the same way. A Cancel or a failure leaves no partial file, and an earlier archive under the same name stays as it was. Changes you make while a project saves stay marked as unsaved.
- A copied effect chain keeps its sound after the zipped project it came from closes: the unpacked files it uses stay in the cache while the clipboard holds it, and pasting it into another project keeps them.
- **Gridline can now recover from a graphics driver reset.** On Windows and Linux, where the driver reports a reset, the window rebuilds its graphics and the project stays open as it was. If the graphics do not come back, or the driver keeps resetting, Gridline closes and offers to restart with a recovery copy of your project. On macOS the system does not report resets.
- The percentage of a render or export can now be read to the end. It used to sit on the bar's fill, which hid it.
- A loop dropped into Kerf no longer stays silent until the next edit on a busy computer.
- **Exporting a project with installed plugins no longer freezes the window.** Such a project used to render on the interface thread, so Gridline stopped responding until the end, with no progress and no Cancel, and Windows could mark the window as not responding and paint it over. It now renders in the background like any other project, so the progress bar, Cancel and quitting all work. Render to sampled instrument gets the same fix. The sound does not change.
- Exports no longer run at real-time priority. A long render took priority over the interface, the graphics driver and the rest of the computer. On Windows, live playback also leaves one processor free of real-time work.
- An export that runs out of memory or hits a file system error now reports an error instead of closing Gridline. The same goes for rendering to a sampled instrument, exporting a clip or sample as WAV, decoding a sample in the background and rendering a stretched clip.
- Cancel now stops MP3, Ogg and AAC encoding straight away, and removes the partial file. Before, it waited for the encoder to finish, and so did quitting.
- A full disk during a WAV or MP3 export is now reported as an error, and no cut-off file is left under the export's name.
- The render dialog no longer searches the disk for encoders and copies the whole project on every frame, which slowed big projects while it was open.
- Folio instruments no longer share one random number generator between threads, and the Crest editor reads the engine state safely while audio plays.
- A big project with thousands of audio clips uses about half the memory it did before. Each clip's standby stretch voice kept large buffers it almost never touches; they now take memory only if they are reached. The sound is unchanged.
- Zipped projects and FL Studio zipped loop packages over 4 GB now save correctly, and packing a large sample no longer needs its size in memory. Smaller archives are written exactly as before.
- SFZ maps that use #include or #define now play correctly, and packing follows them. Maps without those directives play as before. Include loops are skipped safely.
- In collaborative projects, an SFZ map's samples and a cabinet folder's files now reach every collaborator with the map or the folder, and copies made with Leave keep them.
- Zipped projects now carry each file's origin and source notes, so the Track Receipt still maps a derived file back to its original after unpacking. Files named inside third-party plugins that sit inside a patch are now packed and named in the notice too, and a third-party plugin's saved state is never edited, at any depth.
- A zipped project now holds every file the project uses. It used to leave out Abacus pads after the first and their layers, Multisampler zones and sound fonts, the other deck of a region model, wave programs, Plasma wavetables, samples, sample maps and impulses, and files used by a plugin inside a patch, while reporting nothing missing. An SFZ map travels with its samples, and a cabinet folder with its files. Whatever cannot be packed is now named in a notice, and the project inside no longer points at files on your computer. The same goes for Export project data files, project bones and the FL Studio zipped loop package.
- Files that exist or are not files at all no longer show up as missing samples: Plasma wavetables on disk, built-in sounds, empty pads, generated tones and the labels inside a plugin's state.
- Large zipped projects opened far too slowly anywhere, and used as much memory as the archive: the whole file was read again for every file inside it. Archives over 4 GB (ZIP64) and file names in the older zip encoding now open too.
- A slow or offline drive no longer freezes Gridline: at startup, on the welcome screen, in the open and save dialogs, in the browser, in the missing samples window, when previewing a sound from the drive, or when quitting during a read. A project on an offline drive also stays in Recent files instead of being dropped by Remove missing.
- FL project export: a Sweep Kick whose Seed sits between two whole numbers no longer takes about half a second of extra solving.
- Piano roll: right-clicking (or using the delete tool on) overlapping notes deletes only the top note. It used to take the note underneath as well while the button was held.
- Piano roll keyboard: the key under the pointer now shows exactly the accent colour chosen in Settings, a sounding key lights in the channel's real colour instead of a dulled one, and a highlighted white key no longer changes shade beside the black keys.
- **Linux, plugins through yabridge**: the first click after moving a plugin window now lands where
  you clicked (Wine 9.0 and 9.21). A plugin whose interface never returns from closing no longer
  freezes Gridline: its window goes, after 15 seconds the plugin shows as crashed, and quitting
  ends normally.
- **Linux, Run in sandbox**: the plugin's own interface now shows, inside Gridline's plugin window on
  X11, with clicks and keys working as in process.
- **Linux, X11**: the bar of a plugin window now follows the system scale.
- **FL Studio patches line up parallel paths**: imported patches with a plugin that has latency now
  line up parallel paths at every module's input, by the latency each plugin reports, and report the
  longest path to the host. The mixer compensates for it, and uneven parallel paths sum lined up.
  A module's level from FL Studio now scales its output too. Hand-wired
  graphs already worked this way. Projects you imported before keep their sound until you import them again.
- FL imports report the delay they really have, so the mixer and patches line them up: Shuttle reports its 15 samples while playing; Retro Phaser delays and reports 44 samples at every rate; Cipher delays and reports 4 ms while its enhancer is on, plus 16 samples when the imported state asks for them. Projects you imported before keep their timing until you import them again, and new instances don't change.
- Imported patches: an edit that takes a patch's latency from zero to non-zero, or back, no longer restarts the effects inside it. Delays and reverbs keep their tails.
- Pitch bend export to FL Studio projects: a bend over another sounding note of the same colour is no longer left out. The bent note and its slides move to a colour that's free for the note's whole length, so the other note keeps its pitch. The note reads back in that colour after opening the file, since the file has no place for the old one. A bend is still left out, and counted in the export report, when no colour is free, when the instrument reads note colours (hosted plugins, MIDI Out, the slicer, the FM synth, Junction, channels with note processors, Plasma in MPE mode), or when one of your own slide notes plays over the note or counts from it.
- Curves in that export now stay within 5 cents of the line, counting the 10 cent steps of FL Studio's fine pitch, for glides of 2 beats and up at any curve amount and range (they reached about 7.7 cents), and with fewer slides (at most 32 for one glide, 64 for one note's line). Glides that are short and sharp at the same time are limited by the project's ticks: at 96 ticks per beat a 2 octave glide with the sharpest curve is off by up to 12.5 cents over half a beat, and more below that. A step is one slide of one tick, the shortest the format has.
- Limiter: the release knobs reach 0 ms. New instances and FL imports use the new range; existing projects, presets and automation keep the old one.
- Cipher, legacy engine: FL imports reach the full band range (10 Hz up) and attacks up to 1 s. Existing projects and automation keep the old ranges.
- Parametric EQ: FL imports of the classic band model reach down to 10 Hz.
- Envelope curves: the Smooth and Double curve 3 shapes are more accurate. FL imports and newly picked shapes use them; saved curves keep their old shape and sound.
- Delay Bank: automating a very long time down to a very short one with a gate on no longer stalls the audio thread for a moment.
- Multi Delay: FL imports now play the gate in whole slots of its window within each audio block, and very short slots open and close correctly. New instances and existing projects keep their sound. Projects imported before keep theirs until you import them again.
- FL imports: the newest saved states of DCO Synth and Step Bass no longer fail to load, and Kerf picks up the transpose, detune, cutoff and resonance of an empty slicer.
- Sweep Kick has a second engine for FL imports and new instances, with a reworked start phase, pitch sweep, decays, key tracking, velocity and note-off fade. Imported envelope, LFO and filter settings now work through the channel's Envelope page. Existing projects and factory presets keep the first engine and their sound.
- Freq Shifter: FL imports and new instances use a new shift curve and new filter tunings. Existing projects keep their sound.
- Retro Phaser: the echo has a new mode with unfiltered taps, filters in the feedback path, a per-channel stereo balance and revised timing. FL imports and new instances use it; existing projects and factory presets keep the first echo.
- FL Studio automation now imports for Velocity Scale (all 36 controls), Colour Router (the 16
  group outputs), Note Sequencer (steps, rate, both transpose controls), Envelope Mod (base,
  envelope and LFO level of all four envelopes, it was only the first) and Band Delay (all 12
  main controls). Band Delay's old links reached controls its imported engine doesn't read, so
  they had no effect. Automation of a single band of Band Delay still doesn't import.

## 0.7.2

Released 2026-11-03. Everything that changed since 0.7.1. Projects from 0.7.1 open as they are, except that Plasma's Linear Fold, Sine Fold and Sine Shaper distortion modes sound different at high drive.

### Added

- **Crest, full round**: new instances run the engine that FL import uses, with six new
  presets. Oversampling (2x, 4x, 8x), curves of up to 64 points, all 13 point shapes (right-click a
  point). A monitor with scrolling peaks, curve level and gain for one band or all four, pause and
  speed; a spectrum and heatmap with range, tilt and smoothing; gain meters in the band tabs; dB,
  ms and Hz readouts on the knobs; a linear-phase button with its quality menu. Older projects and
  presets keep their engine and their sound. Automating a split with linear-phase filters no
  longer allocates memory on the audio thread.
- **Junction wires anything to anything**: Junction and Junction FX now open on a map of modules
  and cables. Drag from any port to wire audio, notes or parameters, in parallel, across sidechains
  and to several outputs; a cable that closes a loop becomes a feedback cable, drawn dashed, that
  arrives 256 samples later whatever the buffer size. Each cable has its own level, and a right
  click inserts an effect into it. Modules can be renamed, coloured, bypassed, muted, duplicated
  and disconnected; note processors can sit before instruments; installed plugins run inside with
  their own interface. Latency between parallel paths is compensated. The map zooms and pans
  smoothly, with box select, automatic arrange, a minimap and filters per cable kind. The Panel
  page holds the eight macros and any parameter put on it, with names, ranges and snapshots; its
  knobs automate as before. Six new patches show what it can do. On a wired graph, turning a knob
  or a cable's level updates the running patch without rebuilding it. Every edit is one undo step, and
  existing projects and presets keep their sound and data until you rewire them.
  FL Studio import runs more patches as wired (note processors, installed plugins, sidechains,
  extra outputs and mixer track outputs), and reports exactly what it leaves out; export writes
  hand-wired graphs back in FL Studio's patch format, with cable levels, layout and exposed
  parameters.
- **Track "+" in the playlist**: hover a track header and a small "+" appears beside its mute
  switch. Its menu gives the track an instrument (your starred instruments are listed right
  there, then every instrument, or a channel already in the rack) and on an empty track starts a
  first pattern for it on the track, named after the instrument and opened in the piano roll. On
  an instrument track it makes a new pattern at the song position (or the start of the view),
  changes the instrument or makes it an ordinary track again. Each action is one undo step. Turn
  it off in the playlist menu, View, Track + button.
- **Pitch bend on piano roll notes**: a Pitch bend tool beside the pencil (key N) shows a line on
  every note. Drag anywhere on a note or its line to bend it like an automation line: semitone
  steps, the value and the note it reaches shown beside the point. Double-click a point to remove
  just that point (the points before and after it stay as they were), double-click a glide or its
  ring to make it a straight ramp again, and double-click the note itself to reset its whole line;
  each is one undo step, even when the first click slipped a few pixels. Points
  follow the piano roll's snap (the grid in view, or the size you set) and whole semitones; Alt
  frees them, or with Snap off snaps them to the grid in view. Right-click a point for its curve
  menu: the glide into the point as straight, smooth, ease in, ease out or step (hold, then jump),
  each drawn beside its name, a curve amount, Reset the curve, and Remove point (Alt+right-click
  removes at once). Every shape and curve amount plays what the line shows: glides follow their
  curve within a few cents, a step jumps on its own sample, and the channel pitch bend sent to other
  plugins and MIDI Out, MIDI file export and FL Studio slide notes follow steep curves closely.
  Drag the small ring half way along a glide to bend it. The line shows the curve exactly as it
  plays. Note names stay readable when another note's line crosses them, and the line you drag
  stays on top. With several notes selected, a chord bends as one. Back on the pencil the lines
  stay visible, dimmed. Bends play per note on Gridline's instruments and on hosted plugins with
  per-note tuning, as channel pitch bend on other plugins and MIDI Out, travel with moves, copies,
  splits, chops and glue, and are written to and read from MIDI files. FL Studio export (.flp and
  zipped) writes each bend as slide notes on the note's channel and colour (curves as several
  shorter slides, fractions of a semitone as fine pitch), which play the same pitch; a bend over
  another sounding note of the same colour is left out, since a slide would glide that note too.
  The export report counts what was converted, approximated and left out.
- **Channel rack steps in the piano roll**: open a channel with steps in the piano roll and its
  steps are there as notes, one step long, with each step's key, velocity, pan, fine pitch,
  release, mod X and Y and shift. A small "Rack steps" tag in the corner says they still belong to
  the rack; looking changes nothing. Move, resize, delete or edit any of them (or draw a note
  beside them) and the channel's steps become piano roll notes in the same undo step, so one undo
  brings the rack row back exactly; the rack then shows the channel's note preview. The notes play
  exactly as the steps did, swing, swing truncation, shift and step loops included, until one is
  given another length. Piano roll tools, scripts and the chord progression tool see the steps
  too, and cancelling them leaves the steps as they were. "Steps to piano roll" in the rack menu
  makes the same notes, and "Piano roll to steps" turns them back. FL Studio export writes them as
  step notes, and step notes in imported .flp files now keep the swing of steps.
- **Plugin folders per format**: Settings, Plugins lists where VST3, VST2, CLAP, LV2 and AU plugins
  are searched. Add your own folders per format or for every format, switch folders or the default
  folders off, and rescan one folder or all. The Plugin Manager links to it.
- The `VST3_PATH` environment variable adds VST3 search folders, as `VST_PATH`, `CLAP_PATH` and
  `LV2_PATH` already did.
- **Drag audio out of Loupe**: the grip button in Loupe's toolbar drags the selection (or the whole
  file when nothing is selected) onto the playlist, where it lands as an audio clip at the drop
  position on the grid, or onto the channel rack and samplers. A dragged selection is saved as a
  new file in the project's Edited audio folder.
- **Organise files in the browser**: right-click a file or folder to rename it in place, make a new
  folder or move it to the trash (asked first). Drag files and folders onto a folder to move them.
  Channels and clips that use a renamed or moved file follow it, in one undo step, and its
  sidecar files move along. Only your own folders and Gridline's folders can be changed; elsewhere
  the menu says why not.
- **ASIO on Windows**: Settings, Audio has a driver type switch between the system audio (WASAPI,
  still the default) and ASIO. Pick any installed ASIO driver, its sample rate and buffer size from
  the sizes the driver allows, and open the driver's control panel. Device inputs feed the mixer's
  In 1 to In 16, the main output pair is selectable and other output pairs can each carry a mixer
  track. The driver's reported latency sets the recording offset, and resets the driver asks for
  (from its control panel, a new rate or buffer, lost sync) reopen it on their own; an unplugged
  device falls back to the system audio. See `docs/asio.md`.
- **Favourite plugins**: a star on each plugin in the mixer slot list, the channel and instrument
  menus and the browser's plugin database marks it as a favourite. Favourites come first in those
  lists and have their own group in the browser, for built-in and installed plugins alike.
- **Shift+click draws a note and sets its length**: in the piano roll, Shift+click with the draw
  tool places a note where you click and the drag that follows stretches it, in one undo step. A
  Shift+click without a drag keeps the default length.
- **Plugin windows have their own size**: the menu of every built-in effect and instrument window
  has an Editor size submenu (Automatic, 100 % to 200 %), and the corner grip of the window resizes
  the editor, keeping its proportions. The size is remembered per plugin and comes on top of the
  interface scale; text and controls stay sharp at every size. Installed plugins keep their own
  interface size.
- **Loupe can be pinned into the main layout**: Loupe's View menu pins it as a tab next to the
  playlist, below the playlist, on the left with the browser, on the right with the mixer, or as a
  floating panel, and its tab can be dragged to any other dock spot. While pinned, every sample or
  Loupe effect you open shows there. In a narrow column Loupe stacks its toolbar rows instead of
  shrinking them. The pinned panel is kept with the layout; "Unpin into its own window" (or closing
  the tab) puts Loupe back in a window of its own.
- **Stretch the playlist from its scroll bars**: drag either end of the horizontal scroll bar to
  zoom the time, or either end of the vertical scroll bar to zoom the track height. The other end
  stays where it is. The ends light up under the pointer. Zoom, Default track height in the
  playlist menu brings the tracks back to their normal height.

### Changed

- **Plasma's distortion curves are rewritten.** Most modes keep their level; Linear Fold, Sine
  Fold and Sine Shaper sound different at high drive.

- **Channel rack play lights.** The green lamp at the end of each channel row now lights while
  the channel plays and fades after, in place of the small light inside the name button (the name
  gets that room back). A selected channel's lamp keeps a green rim, and a click on the lamp still
  adds the channel to the selection or takes it out.

- **Double-clicking a playlist track header.** Double-click the track's name to rename it. On a
  track that plays an instrument, double-clicking anywhere else on its header shows that
  instrument's window. The header's own controls (the "+", the mute switch, the mode controls)
  keep their normal clicks, and the hint line says what a double-click does where the pointer is.

- **Overlapping playlist clips show what lies underneath.** Where a clip lies over another on the
  same track, the notes, waveform or automation curve of the clip beneath stay visible through it,
  a little dimmed (a clip further down a stack a step dimmer), while the top clip's own content and
  name stay clear. A click in the overlap still picks the top clip. The dark shadow a top clip cast
  onto the clip beneath is gone, the end of the clip beneath shows as a thin edge in its own colour
  instead of light dashes (the lit dashed outline stays while it is selected), and the loop points of
  a repeating pattern are a crisp, quiet seam, so the grid lines read the same through every clip,
  overlapped or not.
- **Turntable is a mixer effect now.** Put it on any mixer insert: the insert's audio passes through
  unchanged and the deck plays its sample on top, with the same platter, motor, hand and editor as
  before. FL Studio projects with its scratch plugin open with Turntable on the same mixer slot, with its
  settings, its sample and its automation, and export back the same way; a Turntable made in
  Gridline exports to FL Studio as that plugin too. The pickers offer this one Turntable. Projects from
  earlier versions open and sound as before: the 0.7.1 Turntable instrument shows as Turntable (old)
  and the earlier live-input effect as Turntable FX (old); neither is offered for new use. Platter
  stays a separate DJ deck instrument.
- **Small plugin editors open larger on large screens.** On a large workspace (for example a
  1440p screen at 130 %) an effect or instrument window that would take less than about half of the
  screen opens at 125 % or more; it never opens larger than fits. Pick 100 % in Editor size to keep
  the design size.
- **Alt+wheel zooms the playlist's track height around the pointer**: the track under the pointer
  and the spot inside it stay put, with tracks of any height, folded groups and automation lanes,
  and the zoom glides like the horizontal one. At the top and bottom of the song the view stays
  against the edge. The piano roll's Alt+wheel row height zooms the same way. The track height is
  remembered for the next start.
- Ctrl+wheel over the playlist's track headers zooms around the left edge of the grid instead of
  a point left of it.
- **Same starting settings as in imported projects**: a new compressor, flanger, time gate,
  frequency shifter, low-end enhancer, bass booster, overdrive, Pinch, spreader, Bucket Chorus,
  Retro Phaser, hall reverb, X-Y-Z Pad and Formula Mod now opens with the settings
  it gets when an FL Studio project brings it in at its defaults. Projects and presets you already
  have sound exactly as before.
- **Auto scrolling can be turned off**, for the playlist and for the pattern editors on their own.
  The playlist and the piano roll each have an auto scrolling button in their toolbar, and the
  setting is also in Options, in each window's View menu and in the channel rack menu. With it off
  the view stays where you put it while the song or pattern plays; scrolling by hand works either
  way. The piano roll now follows the pattern position while it plays, as the playlist already
  did; the channel rack's step view follows it as before. Both settings are remembered.
- **Slide notes glide only the notes of their own colour** (piano roll). A slide
  note no longer bends notes of other colours playing on the same channel, in FL Studio projects
  too. Gridline projects saved before this version whose
  slides met notes of other colours keep sliding every colour, so they sound as they did.
- **Looping clips show their loop points as small notches** in the clip's top and bottom edges,
  like the loop markers on regions in other arrangers, with a faint line through the clip. The
  repeats are no longer dimmed and the bright markers are gone, so a long loop reads calmer.
  The notches scale with the zoom and the interface size and disappear when the clip is zoomed
  too far out to show them.
- **The browser's PROJECT tab lists every audio file the project uses** (edited audio,
  recordings, renders, stems and slices), and the Edited audio folder now sits in that tab.
- The mixer slot list and the channel menus also list installed plugins, not only built-in ones.
- Gridline's id for Linux software centres is now `com.lolitemaultes.Gridline` (it was
  `org.gridline.Gridline`, which the metadata lists as replaced).
- In a Flatpak or snap, the update popup no longer offers to download a setup program: those
  installations are updated by their store.

### Fixed

- A bridged plugin whose other process dies (a Windows plugin through yabridge, say) no longer takes Gridline down: it shows as crashed and goes silent, other tracks play on and the project still saves. Reload it from its plugin window.
- Quitting no longer leaves an invisible process behind when a plugin never returns from closing: Gridline leaves after 20 seconds.
- Gridline as a plugin could crash the host as it quit or unloaded the plugin while the browser was
  still indexing files. The indexing now stops first.
- Linux: Space and Gridline's other shortcuts typed into a plugin's interface no longer get lost; the watch on the interface's windows was switching itself off every second time it was read.
- **FL import, mastering dynamics**: oversampling and curves of more than eight points now load and export;
  release times, the low band's all-pass, the master mid detector and two point shapes are
  reworked for imported settings.
- A lit key on the piano roll's keyboard now lights the whole white key, including the parts beside
  the black keys, and its colour blends into the key instead of covering it, so light channel
  colours stay soft. The key you click lights the same way.
- The playlist could draw garbage on some Windows graphics drivers when a busy project was
  scrolled: the ruler and track headers went blank and lines streaked across the clips. Large
  windows now draw correctly whatever OpenGL version the driver provides.
- Dragging along the piano roll's keyboard plays and lights only the key under the pointer, one
  key at a time; the key you left stops and no longer stays lit.
- FL Studio automation that lands halfway between two steps of a switch or a mode list now picks
  the step the stored value stands for (compressor type, limiter curves and similar controls).
- Enhance presets from FL Studio now import about 13 dB louder than before.
- Reverb settings imported from FL Studio projects and presets now load in full, including the
  oldest preset files without a version number, and the reverb's Full model plays those
  settings with a new voicing: single early reflections per side, new modulation and new level
  ramps when a level moves. A
  changed reverb that came from an old preset file exports in the current layout. Projects saved
  with earlier versions keep their sound: their Full model shows V1, new reverbs and new imports
  use V2 (switch in the corner of the room view). The Full model's knobs show seconds,
  milliseconds, kHz and percentages instead of raw steps.
- FL Studio automation of Multi Delay's unit controls (pans, levels, filters, times, feedback,
  stereo separation, offset and send to the next unit) now imports, and so does automation of
  Retro Phaser's and Bucket Chorus's switches.
- FL Studio vocoder settings now import: band range, formant, invert, attack, release, hold and
  the three levels, with their automation. They used to fall back to Gridline's defaults.
- Auto Pan's LFO imported from FL Studio now runs at the speed the imported setting gives, with
  a new LFO law. A slow setting used to run far too fast. New Auto Pans use the same LFO law; projects saved earlier keep their sound
  (their LFO law parameter stays at 1), and the factory presets move at about their former rates.
- FL Studio projects saved with current versions now import their Keyboard Mod settings, and its attack and release smoothing times follow the stored settings from 0 to 1000 ms.
- A Multi Delay unit set to the shortest step in an FL Studio project now sounds at zero delay,
  instead of a few samples late. Projects saved earlier keep their timing.
- New Multi Delay, Peak Follower and Pitch Shifter instances have revised starting settings that match
  imported units (the Pitch Shifter keeps its Hi-fi engine and no delay). The factory presets and saved
  projects are unchanged.
- The snare drum track icon is no longer cut off at the bottom. Three more icons that touched or
  crossed their box at small sizes (reverb, voice and the loop arrows on looping clips) are drawn
  inside it now.
- Right-clicking the header of a folder added to the browser opens its menu again.

- **The app icon** shows on every window (plugin windows and windows dragged out of the main one),
  in the Windows taskbar at any display scale, and on Linux in the dock and task switcher (X11 and
  Wayland). The Linux packages add a scalable icon, and the AppImage's own icon is sharp.
- The Linux and BSD tarballs come with `install.sh`, which adds Gridline to the desktop menu with
  its icon (and removes it again with `--uninstall`).
- Recording armed when Play is pressed captures from the transport's first moment, also when the
  interface is slow to draw a frame (a take could start late).


## 0.7.1

Released 2026-11-01. Everything that changed since 0.7.0. Projects from 0.7.0 open as they are.

### Highlights

- **Track Receipt** (Help, Track Receipt...) measures which parts of which original recordings a
  project actually plays, merged per song across stems, samplers and plugins, and writes a
  spreadsheet and a PDF for filling in sample licenses, for example on Tracklib.
- **Turntable**, a new instrument: load a sample and scratch it on a platter. FL Studio projects
  with its scratch plugin open with it, with their settings.
- **Seltmann records from an input**, and each step of its sequence can now have its own length
  and its own pitch.
- **Recording into the playlist** draws each take's clip and waveform live while you record.
- **Track icons**: 139 icons with a searchable picker, on mixer tracks, playlist tracks and
  channels, read from and written to FL Studio projects.
- **Plasma** reads and maps many more settings of imported Serum 2 presets.
- **Loupe**'s interface is redrawn, and smaller things: Stop clicked while stopped goes back to the
  start, a "+" for new patterns in the playlist, instruments dropped onto playlist tracks.
- **Fixed**: a crash on "Make unique" in a playlist clip's menu, Sampler notes in the Stretch and
  Rubber Band modes that played silence, scratching recorded in FL Studio projects timed exactly,
  and more below.

### Added

- Audio recorded into the playlist now appears while you record: each take is drawn as a growing
  clip on the track it will land on, from its exact start, with its waveform filling in as the
  audio arrives and a red outline and record head while it is live. Loop passes stack on the
  tracks below as they record, earlier passes shown muted, and several armed tracks record side by
  side. When recording stops the finished clip takes its place without a jump or redraw, and long
  takes stay smooth to draw at any zoom.
- Seltmann can record from an audio input: Record, next to Load on its start screen, opens a
  recorder with an input picker (stereo pairs or single inputs) and a live level meter, Record,
  Pause and Resume, Finish, Clear, a running time and the take drawn as it records. It works
  with the transport stopped or playing, and says so when no input device is set up, the device
  goes away or the sample rate changes. Load recording saves the take as a WAV in the project's
  Recorded folder and loads it like any file: it is analysed, chopped and can be undone. Back
  asks before throwing away a finished take.
- Track Receipt (Help, Track Receipt...): a helper for filling in sample licenses, for example on
  Tracklib. It lists every audio file the project uses, grouped by original song, and ticks the
  files named like Tracklib downloads. Calculate plays the project through offline and measures
  which parts of each original are actually heard, in the playlist and in every built-in sampler
  (loops and repeats count once, the same part from several stems counts once, chopped parts add
  up). Results show the seconds per song, the length tier, the parts used with timestamps and where
  each comes from. Muted parts are listed but not counted, files inside third-party plugins count
  as the whole file until you type the parts used, and a song over 60 s is flagged. Save Track
  Receipt writes a spreadsheet (Licenses, Usage, Release, Splits); View results saves a PDF and
  opens it. What you type (category, share, fee, release details, writers) is saved in the
  project. Files Gridline makes from others (edits, stems, stretched clips, slices, renders) now
  remember where they came from, so their uses map back to the original. Command line:
  `gridline track-receipt <project> --out <folder>`. A helper, not legal advice. Check the figures
  against your license on Tracklib.
- Clicking the Stop button with the transport already stopped, or double-clicking it, puts the
  song and pattern position back at the beginning (the first click still returns to the set
  position; Space is unchanged).
- Turntable, a new instrument: load a sample and scratch it on a platter. Drag the platter up and
  down or the waveform sideways to scratch (Ctrl for fine moves), with the platter's speed,
  acceleration, sensitivity and hold, four smoothing controls, whole extra turns and the motor
  (reverse, pause, forward). A new Turntable plays its own built-in sound until you load a sample
  or drop one anywhere on the editor, from the browser or the desktop. Notes can start the motor
  or jump to a cue point, and key tracking sets the speed. A large view gives the waveform more
  room. Five factory presets. FL Studio projects with its scratch plugin open with it: every
  setting, the sample, the timecode options, automation clips and recorded movements
  come along, and a scratcher without a sample stays silent. Saving to FL Studio writes it back.
  The previous scratch effect is still there as Turntable FX, so projects that use it sound as
  before. Turntable is part of the Gridline Suite too (119 plugins), so it also runs in other DAWs.
- The playlist's pattern list has a "+" button under the last pattern that adds a new pattern and
  selects it, named and undone like New pattern. When the list is longer than the panel the button
  stays at the bottom of the panel.
- Drag an instrument from the browser or the plugin picker onto a playlist track, on its header or
  its lane, to make that track an instrument track playing a new channel of it. The track lights
  up while you hover it. Dropped on an instrument track, the instrument replaces the track's
  instrument; an effect dropped on an instrument or audio track goes into its mixer track. Each
  drop is one undo step. Installed plugins are loaded as themselves.
- Seltmann: turn the mouse wheel over a block in the sequence to pitch that step by a semitone
  per notch (hold Shift for tenths); the region's other steps (in "A B C A", the first A) keep
  their pitch. The block's right-click Pitch menu pitches the step too, and also offers the
  region's pitch, for every step of it. A run of notches is one undo step, and each block shows
  the pitch it plays at.
- Track icons: 139 icons in 16 categories (drums, percussion, keys, synths, guitars and strings,
  brass and wind, vocals, samples, notes, effects, mixing, surround, gear, symbols, numbers and
  faces). The icon menu has a search field, a category list and the icon's name under the grid.
  Playlist tracks and channels take icons too (track menu and channel menu): the playlist shows
  the icon before the track's name at every track height, the channel rack on the name button
  when there is room. Changing an icon is one undo step.
- FL Studio projects open with their icons on mixer tracks, playlist tracks and channels, and
  saving to FL Studio writes them back, including icons chosen in Gridline. Projects imported
  earlier show their icons when opened again.

### Changed

- Seltmann: the sequence is now separate from the regions. Drag the right edge of a block in the
  sequence to change how long that step lasts (it snaps to the chop grid; Shift for finer steps, Alt
  for no snapping; double-click for its default length) without changing its region: a shorter step
  is cut and the next step starts right after it, a longer one plays its region out and then rests.
  Two steps of the same region, for example after Duplicate step, can have different lengths. Once a
  step has its own length, the whole sequence lasts whole bars: when the steps add up to less, it
  rests until the next bar line, so the loop stays in time (sequences you already have sound as
  before). Reset length in the block's menu gives a step its default length
  back. The cuts between regions are resized in the waveform.
- Mixer: a track's icon sits in its own corner of the name plate in every strip width and
  interface scale, so it no longer crowds the number or the name; the routing overview and the
  track panel show it larger.
- Loupe's interface is redrawn on Gridline's own sizes and laid out on one grid: every button of a
  row has one size and one face, and the transport, recording controls, menus and tool buttons
  share their centre lines. The menus sit on one line beside the tool buttons (grouped by what
  they do), the info boxes carry their captions inside them, and the info line shows the cursor or
  the selection and its length and says when Loupe is armed or recording. The bottom bar has a
  wave and spectrum switch, drop-down lists for snapping and the time format, icon zoom buttons
  and evenly spaced level readouts. An empty editor says how to get audio in (and no longer shows
  a stray time), and a running edit shows a progress plate. The tool dialogs group their settings
  in titled sections with one label column and keep Accept and Cancel on the right; their sliders
  show the value in a field beside the track, where the handle never covers it, and a
  double-click on it types one. At 150 and 200 % interface scale the editor shrinks to fit the
  window instead of scrolling sideways.
- Plasma reads and maps many more settings of imported Serum 2 presets:
  - Effects: the delay (times, synced range up to 16 beats, feedback, echo filter, Ping-Pong),
    compressor and limiter, reverb (all five models, the pre-delay on every type), distortion,
    convolution (including the impulse gain), chorus, phaser, flanger, frequency shifter (shift
    direction per side, delay, blur), the dimension stage of the hyper effect and the multiband
    compressor (per-band ratios, upward compression, crossovers and switches) now take these
    settings from the preset. Each gets a Law setting that imports switch on, so Plasma's own
    projects keep their sound. The reverb knob labelled as a pre-delay now reads TAIL.
  - Filters: the dirty ladder filter grows louder and brighter when driven with the cutoff open,
    in the voice and in the filter effect. A voice filter's drive now acts on its dry signal too,
    so a driven filter with its mix turned down keeps the drive's level and colour (most types;
    the ladders, formants, distorted combs and a few others keep a clean dry signal). Driven
    filters, combs, phasers, flangers, ring mods and sample and hold run through a new drive
    stage, and the comb, flanger and phaser types lower their dry signal as the resonance rises.
    The acid ladder, Wsp and DistComb 1 low-pass filters blend with the dry signal in a new way
    when their mix is turned down, and the DistComb 2 filters play at the level the preset sets.
    A modulation route that has only its aux curve drawn now reads that curve, so routes whose aux
    source works in reverse move the filter (or any other target) as far as the preset sets. The
    distortion effect's Overdrive mode now compresses at medium drive.
  - Modulation: LFO point modulation, routes to envelope times, curves on LFO segments, routes and
    envelopes, and tempo-synced LFO divisions are read from the preset.
  - Voice: sub oscillator shapes, granular density and length, sources routed past Env 1,
    envelope and LFO time scaling (100 % is neutral), the spectral oscillator's pitch and
    frequency limits, notes that take over a voice, and effect buses that feed each other.
  - Oscillators: the waveshaper warps (tube, sine shaper, stomp box, tape and the others, on every
    oscillator type), random start phase, routes to the main tuning, the spectral oscillator's
    stereo image from stereo samples, and the loudness of multisample unison are read from the
    preset.
  - Arpeggiator and clips: launched clips follow the song position, clips that span the keyboard
    play as static, one-shot or one copy per key, clip rates, launch quantize, note chance and the
    octave, semitone and velocity variations, the arpeggiator's dotted and triplet rates and the
    timing of its first step are read from the preset.
  - Spectral oscillator: imported presets keep the same loudness on every key instead of
    jumping between notes, and a spectral oscillator without a sample stays silent.

### Fixed

- Playlist: "Make unique" in a pattern clip's menu no longer closes Gridline when the pattern has
  piano roll notes, and the clip's Chop submenu is safe in the same way.
- Sampler: in the Stretch and Rubber Band time modes, a note no longer plays silence when the
  sample start (or, in reverse, the sample end) falls between two samples of the file. Depending
  on the file length this hit about half of all start and end settings, live and in exports.
- Runs of Gridline started with command-line options (automated checks, screenshots, layout
  audits) no longer save their window layout over yours; only a normal start saves it.
- Plasma: a noise oscillator that follows the keys now also follows the pitch wheel and the cents
  of the main tuning, like the other oscillators (a noise that does not follow the keys stays at
  one pitch, as before). The noise also gets a key range, with Fold, on the Oscillator Mapping
  panel. Imported presets keep the noise's key range and play the noise at the pitch the preset
  was made with, also for noise samples at other sample rates, for the fine tuning of a noise
  that does not follow the keys and for modulation of its pitch.
- FL Studio projects with control changes recorded into a pattern (such as recorded scratching)
  now play them on time: each change lands on its exact sample instead of the next 64-sample
  step, and the control keeps its own value until the first change of each pass instead of
  jumping to it at the start. An imported scratch performance now plays its movements exactly as
  they were recorded.
- Importing a link with "Browser only" in a collaborative project now adds the file to the shared
  project audio (as a channel, without a clip), so collaborators receive it too.
- Replacing a channel's instrument now renames the channel after the new instrument when it still
  had an automatic name (a name you typed stays), and the plugin window's preset selector starts
  at the new plugin's default instead of showing the old plugin's preset. Undo restores the old
  instrument, its name and the preset its selector showed. Replacing an effect resets its preset
  selector the same way.

## 0.7.0

Released 2026-10-31. Everything that changed since 0.6.1, including the four 0.7.0 betas. Projects
from 0.6.1 and from every 0.7.0 beta open as they are.

### Highlights

- **Make music together.** Collaborative projects let two or more people work on one project
  live, on the same network or across the internet, peer to peer and end-to-end encrypted, with
  no server and no account. Invite someone with a one-time code; channels, notes, the playlist,
  the mixer, automation, Gridline plugin settings and audio files sync in a fraction of a second,
  while everyone keeps their own windows and transport and sound stays on each computer.
  Collaborators' cursors and changes show in their colour, undo takes back only your own changes,
  work done while apart comes together through a Sync review, and the welcome screen's
  Collaborative view shows who is working on each shared project right now. See
  docs/collaboration.md.
- **The Gridline Suite.** Gridline's own instruments and effects run in any DAW: 118 plugins as
  VST3, CLAP and, on macOS, Audio Unit, each with the same sound and the same editor as inside
  Gridline, named and automatable parameters, the factory presets and a sidechain input where the
  effect uses one. Gridline Suite Mini adds 35 compact single-purpose effects. Every download
  carries both, and a new Gridline brings the installed copies to its own version. See
  docs/gridline-suite.md.
- **Platter.** Two DJ decks and a mixer on a channel: tempo and beat grid detection, sync, key
  lock, hot cues, loops, slip mode and a pitch fader, scratching that feels like a record, ready
  mappings for three DJ controllers (or your own MIDI learn), and performances recorded as
  automation clips that you can see, edit and play from anywhere.
- **Seltmann.** Drop in any audio and it finds the tempo, chops it into lettered regions and
  flips it with ready-made layouts. The sequence lane reorders, splits and joins regions, each
  region can be pitched right where it is, the sequence can start on any cell, and the result
  goes out as a loop or a pattern.
- **Plasma**, a wavetable hybrid synth with three oscillators, two filters, a 64-slot modulation
  matrix and three effect racks, in a calmer editor. It can also read your own Serum 2 and Serum
  presets (`.SerumPreset`, `.fxp`) and set up Plasma sounds from their settings, with the audio they carry.
- **Split stems** separates an audio clip or a file into vocals, drums, bass and other (or six
  stems with guitar and piano), on your computer, onto new tracks lined up with the clip.
- **Import from URL** brings the audio of a web page into the project as a lossless WAV clip; the
  packages carry the programs it needs.
- **Kerf**, the slicer, is laid out like a full region slicer, slices on the beat grid and has
  time, marker and region name settings. **Pixel Tone** plays your own pictures.
- **Audio clips follow the tempo**, a **welcome screen**, a **loading window** for big projects,
  **automation clips inside patterns**, **one smooth stretch and pitch engine** with Rubber Band
  as a third time mode, **smooth zooming and snapping**, **windows on every monitor**, **five
  control styles** and **sound themes**, one undo step per gesture in every plugin editor, a new
  plugin manager and a new demo song.
- **FL Studio projects** open with their whole arrangement, also from its 2026 version, with
  Slicex chops arriving in Kerf exactly and channel stretch times kept.

### Known limits

- Plasma does not read every setting an imported preset stores yet; more are read in each
  update.
- Collaborative projects use Gridline's own plugins only, and everyone in a session needs the
  same Gridline version.
- Half-time grooves may still read at half speed in Platter and Seltmann; the /2 and x2 buttons
  put them right.
- Notes played live on a Rubber Band instrument start the moment you play them, so their start-up
  work is not spread out ahead.

### Added since 0.7.0-beta.4

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

### Changed since 0.7.0-beta.4

- Plasma: changes that also affect existing projects:
  - Hyper uses new voice counts: 1, 2, 3, 5, 5 and 7 voices per side for unison 2 to 7,
    each a short swept delay of 1 to 10 samples plus about 10 per detune percent, summed through a
    low cut at 253 Hz; the dry part falls to 4/9 at full mix. Its sweep is slow at low rates (100
    s per cycle at rate 10, 1 s at 100). Projects that use Hyper sound different.
  - Hyper's voices each start at their own point of the sweep and run at their own speed, with
    different values on the left and the right, so a stereo spread is there even at zero detune.
  - The distorting filter types are reworked: the ring mods (an oscillator at the cutoff,
    and for the second type a second one set by VAR), the sample and hold types (a new value at
    twice the cutoff, resonance coarsens the steps), the first distorted comb type (its delay set
    by VAR, resonance gives back level), the screamer's feedback loop and the dirty ladder's VAR,
    which now lifts the lows below the cutoff. Projects that use these types sound different.
  - The X-Shaper is reworked: its curve shapes the input's size (the sign is kept), and
    the drive fades from the first curve to the second (a straight line when an imported preset
    has none) instead of doing nothing. Imported curves bend as the preset draws them. Projects
    with a drawn X-Shaper curve sound different.
  - The flanger sweeps over 3.5 ms at full depth (before: 4.5 ms). The comb and flanger filter
    types with a low cut or high cut in the loop are revised at mid VAR settings and keep
    their top end in check at high VAR.
  - The phaser sweeps upward from its resting notches, linearly in frequency, as far as 14 kHz at
    full depth (before: three octaves each way). The vintage reverb's tail ends at 16 kHz.
  - Imported presets: the Mg ladder filters keep their resonance under drive (the input clips
    before the resonance loop), and a bus output set to the direct out now skips the main effects.
  - The noise has a new Tone control: a filter that darkens or brightens the noise. It is off in
    older projects.
  - The delay effect's mix, feedback, echo filter and ping-pong now use new laws.
    Projects that use it sound different: at mid mix the dry signal is lower and the echo louder,
    feedback is weaker, and ping-pong repeats are louder.
  - Spectral oscillators play 25 dB louder. Their Low and High band limits now read as
    frequencies from 8 Hz to 22 kHz, no longer as fractions of the sample rate.
  - Arpeggiator patterns keep their step timing, so gaps between steps are rests, and a step plays
    for its full length times the gate.
  - The Geiger noise loops.
  - A new Unison Mix setting picks how unison voices share the stereo field: Even (as before) or
    Alternating, which imported presets use. Existing projects keep Even.
  - More effect laws are new. The EQ's Q and shelf slopes are new. The compressor's
    threshold, release, limiter (ratios from 100 up), makeup and latency compensation are new;
    projects that use it compress less at the same threshold. Each reverb type now has its own
    decay, tone, width, pre-delay and mix laws: a project with a reverb sounds different (the dry
    signal stays louder at mid mix, Hall, Vintage and Abyss are always wide, Space is mono). Imported
    presets fill effect values they leave unset with the defaults that preset format assumes.
  - A new Filter Drive Mode setting: Leveled (as before) keeps the level about even as the drive
    rises; Boost, which imported presets use, raises it. Existing projects keep Leveled.
  - Two new settings that imported presets use and existing projects keep off: Filter Key Centre
    (C4 as before, or G#1) and Multisample Level (Unity as before, or Mapped: the kit's own level,
    velocity and envelope).
  - Mono: releasing a key that a later key took over now forgets it, so a mono chord ends when
    its keys are released.
  - A new Envelope Curves setting: Gentle (as before) or Steep, which imported presets use.
- Plasma's editor has a cleaner, calmer look. It now uses Gridline's slate panels and text tones
  and follows the theme. It has one text scale with nothing below 8 px, values in the mono face, and
  one knob size. The pages sit in a segmented switch, and graphs sit in their own wells away from
  their fields. Outputs are named in words (MAIN, DIR, F1+2). Empty matrix rows show only their
  source and destination, the effects rack uses a single accent, and the clip editor has a real
  piano keyboard. The CLIP and ARP blocks in the bottom strip open their editors with a button.
  Every control works as before.
- Tempo display: drag or scroll the whole number to step whole BPM while the decimals stay
  (96.250 becomes 97.250, 98.250, ...); drag the decimals to fine-tune as before, or scroll them
  by 0.1 (Shift: 0.01). A short line under the digits shows which part a drag or scroll moves.
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

### Fixed since 0.7.0-beta.4

- The update popup on Linux downloads your distribution's own package: the `.deb` on Ubuntu, Debian
  and their derivatives (Mint, Pop!_OS, elementary and others), the `.rpm` on Fedora, RHEL, Rocky,
  AlmaLinux and openSUSE. Other distributions, and a processor a release has no package for, still
  get the AppImage. The distribution is read from `/etc/os-release`.
- Installing a new Gridline now always brings the Gridline Suite and Gridline Suite Mini to the same
  version. Copies in your plug-in folders are replaced at the first start, whatever the install
  answer in Settings, and a suite that is new in this version joins them; the Windows installer
  replaces the suite bundles whole, so no file of an older version stays behind. Settings, Files,
  Gridline Suite: "Install the Gridline Suite automatically" now only decides about installing where
  the suite is missing, and Remove keeps it removed.
- Platter's hot cue pads take fast taps. A pad held while Shift went down left a stopped deck
  playing on from the pad until the end of the track. A pad now always sends its release, never
  a release without its press, and Shift or the right button only clear when the click starts
  with them. A pad pressed while another one previews takes the preview over, so its release
  returns there and stops. A pad set right after a jump stores the place the jump went to, and a
  newly set pad is always saved with the project, even when it was set moments before saving.
- Plasma reads and maps more settings of imported Serum 2 presets:
  - Noise pitch and key tracking are read from the preset. The noise pitch knob is a playback rate
    covering 96 semitones above its centre and 384 below. With key tracking on it spans 128
    semitones, with the noise's own pitch at key 72 and its stored detune applied. Sample noises
    play 2 dB quieter than before.
  - White, pink and brown noise use the knob as a tone filter and play at the level the preset
    sets. Geiger keeps clicking on held notes.
  - Main tuning covers its full range of plus or minus 64 semitones, where before it moved by less
    than a cent. The oscillator Hz offset only applies in Ratio mode.
  - Spectral oscillators no longer go silent when the preset limits their frequency band. They
    also play at the level the preset sets, 25 dB louder than before.
  - Arpeggiator rates use the divisions the preset sets, and dotted rates play the next faster
    division dotted. Pattern steps keep their timing, so gaps between steps are rests. Pattern rows
    beyond the held notes repeat those notes instead of jumping octaves.
  - A clip player with no active clip passes the keys through instead of looping a clip.
  - The delay effect uses the times, mix, feedback and echo filter the preset sets, with new laws.
    Its ping-pong echoes no longer lose level, and high feedback no longer runs away.
  - Unison detune spreads the voices as wide as the preset sets, and the default detune of
    presets that do not store one is now applied. Voices go hard left and right in turn, with new
    blend and voice-count levels: a 3-voice unison at no blend plays 7 dB quieter than before, and
    at full blend its side voices are louder.
  - Filter drive raises the level, up to 24 dB at full drive, and clips about 8 dB later. Driven
    filters and filter effects play up to 19 dB louder than before.
  - The filter types in imported presets now map their settings more fully: the low, high, band,
    peak and notch types at 6 to 24 dB, the dual types and the three-way morph types take their
    resonance, level and VAR from the preset across cutoff, resonance and VAR, and the classic
    ladder low-pass types get back all the level the resonance takes at full VAR. The five ladder
    types (including the diode-style acid and EMS ladders) have a reworked cutoff, resonance,
    level and drive, and the A and dirty ladders self-oscillate at full resonance. The combs,
    flangers and phasers have a reworked feedback, mix and level (a comb at no resonance is now a
    clean level drop instead of a comb), and so do the gentle and morphing low/high types, the
    band reject (VAR sets the width of the rejected band), the ring modulator, the pole-zero type
    (now a straight wire) and the diffuser (no colouring). The filter stereo spread is reworked
    too: small around the centre and up to 32 semitones each way at the ends. This voicing comes
    with the Boost drive mode that imports set; Plasma's own projects keep the Leveled mode and
    sound as before.
  - More filter types in imported presets now map their settings: the allpass chain, the
    eight-comb bank, the pre-delayed comb, the reverb (with a reworked tail), the three scream
    types, the five EQ shelves and bell, the filtered 8-stage phasers, the flange-phase type, the
    DJ mixer (low-pass below the middle of the knob, high-pass above it) and the three formant
    types (five vowel bands whose positions follow cutoff and VAR). The cubic stereo spread now
    applies to every filter type, combs and phasers included.
  - Multisample kits play at the level the kit sets, about 9 dB louder than before. Velocity
    follows the kit's own velocity tracking (half tracking at half velocity: 6 dB down), and each
    kit's own envelope shapes its notes, with a half-second release when the kit names none. Imports use this; Plasma's own projects keep their level (new Multisample Level setting).
  - Multisample kits that were silent now play: kits whose sample folder is written without a
    trailing slash, and presets that carry the kit's map but not its audio (the samples are found
    below the library's multisample folder, also when the kit file has moved).
  - Filter key tracking in imported presets centres on G#1: at middle C the cutoff is 28
    semitones higher than before. Filter X and Y are no longer imported.
  - Mono presets no longer go silent after a chord. Releasing the keys of a mono chord left the
    note held, so later notes joined it without starting again.
  - Imported envelopes keep the curve shapes the preset sets. Curves bend harder towards the ends
    of the knob (at curve 90 a 0.3 s decay is now about 45 dB down halfway through), and settings
    a preset leaves unset take the defaults that preset format assumes (attack curve 60 on the
    first envelope and 40 on the others, decay and release curves 60, and a 0.5 ms attack on
    envelopes 2 to 4).
  - Wavetable oscillators in imported presets use a new Wavetable Law setting (Plasma's own
    projects keep the Classic law). The table position spreads the frames evenly and keeps each
    frame's DC offset (pulse frames play up to 9 dB louder than before). Tables step from frame to
    frame unless the table file asks for blended frames. FM from another oscillator is a fixed
    deviation in Hz at any pitch, and phase distortion has a new depth. Sync, bend, asymmetry,
    flip, quantize, even/odd and the low-pass and high-pass warps have new curves, and the tube
    and soft saturation warps are gentler (at half way the tube warp plays 4 dB quieter than
    before). Ring and amplitude modulation are deeper (their sidebands are 6 to 12 dB louder than
    before), and FM and phase distortion from the sub oscillator have a new depth.
  - The modulation matrix in imported presets uses a new Matrix Law setting (Plasma's own
    projects keep the Classic law). Bipolar routes swing half their amount each way, input curves
    bend more steeply (at curve -50 a route at half way gives 6 % of its amount), a macro moved by
    another route passes that on, and routes to bus sends and the utility width move them on a new
    knob curve. Routes to the voice amp add to its gain in a straight line and can push it past
    the knob's top or through zero (velocity at full amount lifts a centred amp by 9.5 dB, where it
    used to hit the 12 dB top already at half velocity). Route smoothing is no longer imported.
  - Wavetable oscillators in imported presets start at half a cycle, the default phase that
    preset format assumes, and a free-running oscillator starts there too. The mirror, quantize
    and self phase distortion warps are reworked (quantize at 0.7 was a plain saw before). A
    multisample is silent on keys and velocities no zone covers, where it used to play the nearest
    zone.
  - LFOs in imported presets use a new LFO Law setting (Plasma's own projects keep
    the Classic law). Rise and delay are times, in seconds on a free LFO and in beats on a synced
    one, and smoothing is a fixed time rather than a share
    of the LFO's cycle (at 50 % about 12 ms, where a 2 Hz LFO used to lose most of its swing).
  - An imported clip player that holds a looping clip across the keyboard stays on when the
    preset names no active clip.
  - A macro moved by another route passes that on whatever order the routes are in (before, a
    route into a macro in a later slot never reached the routes reading that macro). Routes to
    the compressor's gain move it on a new knob curve (a route at half lifts it by about 19 dB,
    where it gave 15).
  - An effect bus whose output is the other bus now feeds that bus, through both bus volumes (new
    Bus Chain setting; before, it went to the main output).
  - The voice filters' pad switch is imported (new Filter 1 Pad and Filter 2 Pad settings): a
    fully driven filter with pad on now plays 3 dB louder than before.
  - The distortion effect has new curves for every mode and drive, a new filter (a fade from
    low-pass through band-pass to high-pass, with its own resonance), new downsample rates, a new
    mix law and the level knob after the mix. Presets that leave the filter type unset now get a
    low-pass instead of a band-pass.
  - The multiband compressor takes its thresholds, ratios, upward compression, deadbands and band
    thresholds from the preset with new laws, catches a note at once and stops near 40 dB of gain
    change. Before, it was up to 12 dB louder on the default settings and pushed quiet parts up
    even with upward compression off.
  - The frequency shifter puts the down shift on the left and the up shift on the right by
    default, with new direction, width and mix laws. Its two shifts are now cleanly apart
    (before, each side carried both at the same level).
  - Effects that a preset leaves unset take the defaults that preset format assumes: shifter
    width and direction, phaser feedback, convolver mix and the chorus, phaser and flanger rates.
  - Chorus, flanger, phaser and convolver share a new mix law, so half mixes play up to 9 dB
    louder than before. Flanger and phaser feedback no longer turn negative at low settings, the
    flanger's shortest delay and the phaser's notch positions are revised, and the convolver's
    wet path has no delay (an empty impulse slot passes the sound unchanged).
  - Hyper with one voice passes the sound untouched, and more voices keep about the same level
    (four voices play up to 7 dB louder than before).
  - The reverb types in imported presets map their settings more fully. Imported presets now set
    each reverb type's tone, low cut and high cut from the preset's settings (the plate's low cut
    reaches far higher, the other types' cuts are gentle slopes), and the low, mid and high bands
    decay at the times the preset sets, per size (and on the hall per size and delay setting): a
    large plate no longer loses its highs early, and a small hall with a long delay rings in the
    lows. The wet level follows the preset's size (a large plate plays up to 10 dB quieter than
    before).
  - The utility's low cut is steeper (24 dB per octave instead of 12), and the utility and the
    filter effect use a new mix law (a half mix keeps more of the dry sound). A multiband compressor at a part mix no longer cuts deep notches around its
    crossover points.
  - Comb and flanger filters ring at a higher pitch than before at high cutoffs (most near
    7 kHz) and keep more of their highs. The filter effect's pad switch keeps the drive's gain and
    only removes its clipping (it used to switch the drive off, 16 dB quieter at drive 60).
- Import from URL: a missing downloader or converter is reported as missing on the BSDs too.
- Platter picks up very quick jog moves from a controller. A flick out and back in 30 ms now
  reaches about 90% of the hand's travel instead of 54% (in 15 ms: 80% instead of 33%), about
  7 ms behind the hand instead of 24 ms, and the record always ends where the hand put it. The
  smoothing adapts to the hand: quick moves are followed closely, slow drags on a coarse wheel
  stay as smooth as before, and Hand feel still sets how close it follows. The wheel's touch,
  its turns and the release now each play on the sample they arrived on, so a touch, a flick
  and a release in quick succession keep their order. Recorded passes keep the quick moves and
  play them back exactly.
- The clear button in the browser search field works again: it empties the field (also while you
  are typing in it) and leaves the cursor in the field.
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
  beats) now play at that length, so loops and one-shots line up with the beat grid. Resample channels change the pitch with the stretch, the other modes keep it. Sampler
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
- Plasma opens Serum 2 and Serum 1 presets with the audio they carry. Wavetables, samples,
  multisample kits, noises, impulse responses and microtunings stored inside a preset now play
  instead of a "file not found" stand-in, and they are saved with the project, so it plays the same
  on a computer without the preset. A table stored in the preset wins over a file of the same name.
  Files a preset only refers to are also found next to the preset (a pack's own folders). Levels
  follow the volume curve the preset format uses (oscillators, noise, sub, master, amp, direct
  and bus outputs play louder than before; sends to the effect buses are lower), the table
  position spans the whole table, oscillator pans follow the pan curve the preset format uses, filter drive keeps its driven level, and a wide unison no longer starts with a click.
- Notes with a fine pitch now keep their pitch in Kerf: a note tuned up or down (up to an octave
  either way) plays the chop of its own key at that pitch, so imported FL Studio projects that pitch
  Slicex chops this way play as written. Before, the tuning moved the note to another key, so it
  played a different chop or nothing. The channel and master pitch also tune the chops without changing which chop plays,
  and the same holds for drum pads and kit pieces. The channel's key range checks the written key.

### Added in 0.7.0-beta.4

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

### Fixed in 0.7.0-beta.4

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

### 0.7.0-beta.3 in short

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

### Added in 0.7.0-beta.3

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

### Changed in 0.7.0-beta.3

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

### Fixed in 0.7.0-beta.3

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

### Removed in 0.7.0-beta.3

- The "Song sketch" project template (and its Songwriting category).

### 0.7.0-beta.2 in short

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
- Reads Serum 2 presets (`.SerumPreset`) and Serum 1 `.fxp` files from your own folders and sets
  up Plasma sounds from their settings. There is one Open path for every preset type (the preset menu's Open
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

### New: collaborative projects

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
  application) onto a channel row replaces that channel's content. A sample
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
- Channel envelopes, LFOs and filter: imported projects keep the timing their settings store and
  new ones get smooth timing, in one system.
- Looping pattern clips, Shift+drag copies, an eye toggle for hidden items, a shared minimum
  inset for every editor with an automatic layout audit, effect reordering with the mouse wheel,
  an interactive reverb room display.
- Event lane editing follows the piano roll's rules (nearest stem, line tool).
- FL Studio project import keeps note velocities as MIDI velocities and finds samples that were
  moved next to the project.
- The version comes from one place (`project(VERSION)`), shown in the app, the command line
  and the plugins.
