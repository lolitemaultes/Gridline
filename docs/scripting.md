# Scripting reference

Gridline runs Lua 5.4 scripts in a sandbox. There are five places a script can run:

| Kind | Where | Folder (shipped / yours) | Undo |
|---|---|---|---|
| Piano roll scripts | Piano roll > Scripts | `assets/scripts/pianoroll`, `<config>/scripts/pianoroll` | one step per Accept |
| Project scripts | Tools > Project scripts | `assets/scripts/project`, `<config>/scripts/project` | one step per run |
| Loupe (audio editor) scripts | Loupe > Tools > Scripts | `assets/scripts/audio`, `<config>/scripts/audio` | one step per Accept |
| Controller scripts | Settings > MIDI, controller type "Script: ..." | `assets/scripts/controllers`, `<config>/scripts/controllers` | controller moves merge |
| Console | Settings > Debug | `<config>/scripts/console` | one step per line |

This page covers what every script shares, then the project API (`project`) and the audio
editor API (`audio`, `buffer`, `dsp`). The piano roll `score` API and the controller hooks
are described at the top of `src/core/PianoRollScript.h` and `src/core/ControllerScript.h`.

## The sandbox

Scripts get the Lua base library without code loading, plus `string`, `table`, `math`,
`utf8` and `coroutine`. There is no `io`, `os`, `package`, `debug`, `require`, `load`,
`loadfile`, `dofile` or `string.dump`. `print` and `log` write to the script log (Settings >
Debug shows it). A read-only `files` table reads plain files in the script's own folder
(`files.read(name)`, `files.exists(name)`, `files.list()`); only the console may write.

Every run has limits (Settings > Files > Scripts):

* an instruction budget (default 20 million) and a time limit (default 2 s). Once a limit is
  reached the script stops and keeps raising errors, so `pcall` cannot swallow the stop;
* a memory cap for the Lua state (64 MB; audio scripts get extra room for their buffers);
* pattern matching refuses very long subjects and patterns, `table.move` is bounded, and
  metatables with `__gc` are refused (finalizers would run outside the limits).

Work done inside the C functions of the APIs counts too: every API call is charged against the
instruction budget, edits cost more than reads, and buffer operations cost a share of the
samples they touch. A script that loops forever over API calls or buffer operations stops just
like a plain Lua loop.

The API tables are read-only: assigning to `project.channels` or `project.mixer.setVolume`
raises an error, and their metatables are locked.

## Script files and forms

A script file starts with header comments:

```lua
-- name: Route drums to a bus
-- category: Mixer
-- description: What the script does, shown in menus and in the dialog.
-- author: You
```

Loupe scripts may also say `-- target: cursor` (see below).

A script can declare settings with `form(f)`. Gridline shows them as knobs, menus, check boxes
and text fields, and passes the values to `apply` by name:

```lua
function form(f)
  f:knob("Amount", 0.5, 0, 1, "hint text")      -- number
  f:knobInt("Bars", 4, 1, 64)                   -- whole number
  f:combo("Style", { "One", "Two" }, 1)         -- 1-based index of the choice
  f:check("Replace", true)                      -- boolean
  f:text("Name", "Drum bus")                    -- string
  f:group("Section title")                      -- following controls are grouped
  f:endGroup()
  f:description("Longer description")
end

function apply(p)            -- project scripts: apply(p)
  print(p["Amount"], p.Bars) -- audio scripts: apply(audio, p)
end
```

Without `apply`, the top-level code does the work. `math.random` is seeded per run, so a run
can be repeated exactly; "New variation" picks another seed.

## Project scripts

A project script edits the whole project through the global table `project`. All edits go
through the document's undo system, and one run is one undo step labelled `script: <name>`.
If the script raises an error (or hits a limit) part way, every edit it made is reverted and
no undo step is left behind. Selection changes are editor state and are not undo steps.

Numbering:

* channels, patterns, playlist tracks, automation clips, markers, effect slots and plugin
  parameters count from 1, in the order shown on screen;
* mixer tracks count from 0: 0 is the master, 1 and up are the inserts;
* playlist clips are named by their clip id (a number that stays the same while the script
  runs, even when clips are added or removed);
* times are ticks (`project.song.ppq()` ticks per quarter note).

Wrong arguments raise an error with a message (for example "no such channel"). Values are
clamped to the parameter ranges.

### project.song

| Function | Description |
|---|---|
| `tempo()`, `setTempo(bpm)` | song tempo |
| `ppq()` | ticks per quarter note |
| `bar()`, `beat()` | ticks per bar and per beat |
| `timeSignature()` | returns numerator, denominator |
| `setTimeSignature(num, den)` | num 1..16, den 2, 4, 8 or 16 |
| `swing()`, `setSwing(v)` | global swing 0..1 |
| `masterPitch()`, `setMasterPitch(semitones)` | master pitch |
| `length()` | end of the last playlist clip, in ticks |
| `info()` | table `{title, author, genre, url, comments}` |
| `setInfo(t)` | sets the fields present in `t` |

### project.channels

| Function | Description |
|---|---|
| `count()` | number of channels |
| `id(i)`, `index(id)` | stable object id of a channel and back (nil when gone) |
| `find(name)` | index of the first channel with that name, or nil |
| `name(i)`, `setName(i, s)` | channel name |
| `color(i)`, `setColor(i, 0xRRGGBB)` | channel colour |
| `volume(i)`, `setVolume(i, v)` | linear level 0..1.25 |
| `pan(i)`, `setPan(i, v)` | -1 (left) .. 1 (right) |
| `muted(i)`, `setMuted(i, b)` | mute |
| `pitch(i)`, `setPitch(i, semitones)` | channel pitch |
| `plugin(i)`, `pluginName(i)` | instrument type id (`gl.synth`) and display name (`Tercet`); scripts should test the type id, which never changes, not the display name |
| `setPlugin(i, type)` | replaces the instrument (instrument types only) |
| `sample(i)` | file name of the channel's sample, or nil (no folders are exposed) |
| `route(i)`, `setRoute(i, m)` | mixer track the channel plays through |
| `add([name [, type [, position]]])` | new channel (default type `gl.synth`), returns its index |
| `remove(i)`, `clone(i)`, `move(i, to)` | structure; `clone` returns the copy's index |
| `paramCount(i)` | number of instrument parameters |
| `param(i, k)`, `setParam(i, k, v)` | instrument parameter `k` in its own units; `k` may also be `"volume"`, `"pan"`, `"mute"` or `"pitch"` |
| `paramName(i, k)`, `paramRange(i, k)`, `paramText(i, k)` | name; min, max, default; formatted value |

### project.patterns

| Function | Description |
|---|---|
| `count()`, `id(p)`, `index(id)`, `find(name)` | as for channels |
| `name(p)`, `setName(p, s)`, `color(p)`, `setColor(p, rgb)` | pattern name and colour |
| `add([name [, bars [, stepsPerBar]]])` | new pattern, returns its index |
| `remove(p)` | removes the pattern and its playlist clips |
| `clone(p)` | copy placed after the original, returns its index |
| `length(p)` | content length in ticks |
| `bars(p)` | returns bars, steps per bar |
| `setBars(p, bars [, stepsPerBar])` | pattern length |
| `steps(p, ch)` | list of step velocities (0 = off) of channel `ch` |
| `setSteps(p, ch, list)` | numbers 0..127 or booleans (true = 100) |
| `notes(p, ch)` | list of note tables |
| `setNotes(p, ch, list)` | replaces the channel's notes in the pattern |
| `addNote(p, ch, note)` | adds one note |
| `channels(p)` | indices of the channels with notes or steps in the pattern |

Note tables: `{time, length, pitch (0..127), velocity (0..127), pan (-64..64), fine (cents),
release, modx, mody (0..255), color (0..15), slide, porta, muted}`. Missing fields take their
defaults.

### project.playlist

| Function | Description |
|---|---|
| `trackCount()`, `setTrackCount(n)` | number of playlist tracks (1..500; cannot drop tracks in use) |
| `trackName(t)`, `setTrackName(t, s)` | name ("Track 3" when unnamed) |
| `trackColor(t)`, `setTrackColor(t, rgb)` | colour |
| `trackMuted(t)`, `setTrackMuted(t, b)` | mute |
| `clips([t])` | clip tables of the whole playlist or of track `t`, sorted by start |
| `clip(id)` | one clip table, or nil |
| `addPatternClip(p, t, start [, length])` | returns the new clip id |
| `addAudioClip(ch, t, start [, length])` | audio clip of a sample channel (default length one bar) |
| `addAutomationClip(a, t, start [, length])` | places automation clip `a` |
| `removeClip(id)` | removes a clip |
| `moveClip(id, t, start)` | moves a clip |
| `setClipLength(id, ticks)` | 0 = the content's natural length |
| `setClip(id, t)` | sets `muted`, `offset`, and for audio clips `gain`, `pitch` (cents), `reverse`, `fadeIn`, `fadeOut` (seconds) |

Clip tables: `{id, kind ("pattern", "audio" or "automation"), track, start, length, offset,
muted, name}` plus `pattern` (index) for pattern clips, `channel` (index), `gain`, `pitch`,
`reverse`, `fadeIn`, `fadeOut` for audio clips and `automation` (index) for automation clips.

### project.automation

| Function | Description |
|---|---|
| `count()`, `id(a)` | automation clips of the arrangement |
| `name(a)`, `setName(a, s)` | name |
| `target(a)` | parameter reference (see `project.params`) or nil |
| `targetName(a)` | display name of the target (owner and parameter) |
| `points(a)` | list of `{time, value (0..1), tension (-1..1), shape}` |
| `setPoints(a, list)`, `addPoint(a, point)` | edit the curve (kept sorted by time) |
| `range(a)` | min, max of the clip (fractions of the parameter range) |
| `add(ref [, name [, points]])` | new automation clip for parameter `ref`, returns its index |
| `remove(a)` | removes the clip and its playlist placements |

Point values are normalized: use `project.params.toNormalized(ref, value)` to turn a
parameter value into a point value.

### project.mixer

| Function | Description |
|---|---|
| `count()` | master + inserts |
| `id(m)`, `name(m)`, `setName(m, s)`, `color(m)`, `setColor(m, rgb)` | track identity |
| `volume(m)`, `setVolume(m, v)`, `pan(m)`, `setPan(m, v)` | strip |
| `muted(m)`, `setMuted(m, b)`, `solo(m)`, `setSolo(m, b)` | mute and solo |
| `param(m, name)`, `setParam(m, name, v)` | `volume`, `pan`, `stereo`, `mute`, `eqLowGain`, `eqMidGain`, `eqHighGain`, `eqLowFreq`, `eqMidFreq`, `eqHighFreq`, `eqLowQ`, `eqMidQ`, `eqHighQ` |
| `add()`, `remove(m)` | new insert (returns its index); the master cannot be removed |
| `sends(m)` | list of `{to, level, sidechain}` |
| `setSend(from, to, level)` | adds or changes a send (level 0 removes it); returns false for a loop |
| `routeTo(from, to)` | sends only to `to` at full level (other sends removed) |
| `setSidechain(from, to, b)` | the send feeds the destination's sidechain input only |
| `channels(m)` | indices of the channels that play through track `m` |
| `slotCount()` | effect slots per track (10) |
| `slot(m, s)` | `{type, name, enabled, mix, params}` or nil for an empty slot |
| `setSlot(m, s, type)` | loads an effect (effect types only); no type clears the slot |
| `setSlotEnabled(m, s, b)`, `setSlotMix(m, s, v)` | slot switch and dry / wet |
| `slotParamCount(m, s)`, `slotParam(m, s, k)`, `setSlotParam(m, s, k, v)` | effect parameters |
| `slotParamName(m, s, k)`, `slotParamRange(m, s, k)`, `slotParamText(m, s, k)` | name; min, max, default; formatted value |

### project.params

A parameter reference is a number that names one parameter of the project. Build it, then
read, write or automate it:

| Function | Description |
|---|---|
| `song(name)` | `"tempo"`, `"swing"` or `"masterPitch"` |
| `channel(i, k)` | instrument parameter `k`, or `"volume"`, `"pan"`, `"mute"`, `"pitch"` |
| `mixer(m, name)` | a mixer strip parameter (names as in `project.mixer.param`) |
| `send(from, to)` | level of an existing send |
| `slot(m, s, k)`, `slotMix(m, s)` | effect parameter, slot dry / wet |
| `get(ref)`, `set(ref, v)` | value in the parameter's own units |
| `normalized(ref)`, `setNormalized(ref, n)` | value as 0..1 |
| `toNormalized(ref, v)`, `fromNormalized(ref, n)` | conversions |
| `name(ref)`, `range(ref)`, `text(ref)` | display name; min, max, default; formatted value |

### project.markers

| Function | Description |
|---|---|
| `count()`, `get(i)` | markers sorted by time: `{time, name, tsNum, tsDen, action}` |
| `add(t)` | adds a marker (returns its index); `tsNum` / `tsDen` > 0 make a time signature marker |
| `set(i, t)`, `remove(i)` | edit or remove |

### project.selection

Selection is editor state: changing it is not recorded in the undo history.

| Function | Description |
|---|---|
| `channels()`, `setChannels(list)` | selected channel indices |
| `clips()`, `setClips(list)` | selected playlist clip ids |
| `pattern()`, `setPattern(p)` | current pattern |
| `mixer()`, `setMixer(m)` | selected mixer track |
| `time()` | playlist time selection: start, end (nil when there is none) |
| `setTime(start, end)` | sets the time selection |

### project.transport

Scripts can always read the transport. Starting, stopping and moving playback needs the
setting "Project scripts may start, stop and move playback" (off by default); otherwise those
calls raise an error.

| Function | Description |
|---|---|
| `isPlaying()`, `isRecording()`, `songMode()`, `position()` | state (position in ticks) |
| `allowed()` | true when the script may control the transport |
| `play()`, `stop()`, `setPosition(ticks)` | control (when allowed) |

### Example

```lua
-- name: Mute empty tracks
-- category: Playlist
local P = project
function apply()
  local used = {}
  for _, c in ipairs(P.playlist.clips()) do used[c.track] = true end
  for t = 1, P.playlist.trackCount() do
    if not used[t] then P.playlist.setTrackMuted(t, true) end
  end
end
```

Shipped project scripts: `humanize_playlist.lua` (humanize a playlist selection),
`drum_pattern.lua` (build a drum pattern), `route_drums_to_bus.lua` (route all drums to a
bus), `rename_tracks_by_content.lua` (name tracks after their clips), `section_markers.lua`,
`tempo_ramp.lua` (tempo automation clip).

## Loupe (audio editor) scripts

Loupe scripts read and rewrite the sample data of the editor's selection. When nothing
is selected they work on the whole file; a script with the header `-- target: cursor` works at
the cursor instead (generators insert their audio there). The script dialog previews the
result while you change the settings (with a small budget while a control is held), shows the
processed audio, plays it with "Play preview", and Accept makes the change one undo step of
the editor.

Scripts work on whole blocks of samples with buffer operations implemented in C, so they never
need one Lua call per sample.

### The audio object

`apply(audio, p)` receives:

| Field / method | Description |
|---|---|
| `audio.rate` | sample rate |
| `audio.channels` | 1 or 2 |
| `audio.frames` | frames in the target (changes with `resize`) |
| `audio.start` | first frame of the target in the file |
| `audio.fileFrames` | frames in the whole file |
| `audio.selected` | true when the editor has a selection |
| `audio.cursor`, `audio.seed` | cursor frame, random seed of the run |
| `audio:channel(c)` | the buffer of channel `c` (1 = left, 2 = right); edit it in place |
| `audio:buffers()` | iterator: `for c, buf in audio:buffers() do ... end` |
| `audio:setChannel(c, buf)` | replaces channel `c` with another buffer |
| `audio:resize(n)` | changes the target length of every channel (pads with silence) |
| `audio:read(c, from, to)` | copy of the original file's frames `from` .. `to - 1` (0-based), also outside the target |
| `audio:addMarker(pos, name)` | marker at `pos` frames from the target start |
| `audio:select(a, b)` | selection after the script, in frames from the target start (may reach outside it) |

When the script ends, all channel buffers must have the same length. The target is replaced
by the buffers (a different length shortens or lengthens the file). Mono files stay mono.

### Buffers

Buffers hold 32-bit float samples. Sample indices count from 1, and optional ranges `i, j`
are inclusive (`buf:fill(0, 1, 100)` clears the first 100 samples). `#buf` is the length,
`buf[i]` reads and `buf[i] = v` writes a sample. Values stay finite: NaN and infinity are
refused or replaced.

| Constructor | Description |
|---|---|
| `buffer.new(n [, value])` | `n` samples |
| `buffer.fromTable(list)` | from a list of numbers |
| `buffer.isBuffer(x)` | type check |

| Method | Description |
|---|---|
| `len()`, `get(i)`, `set(i, v)` | basics |
| `copy([i, j])`, `sub(i, j)` | new buffer with a copy of the samples |
| `resized(n)` | new buffer of length `n` (copy, padded with silence) |
| `totable([i, j])` | list of numbers (up to 4 million) |
| `fill(v [, i, j])` | sets samples |
| `scale(g [, i, j])`, `gainDb(db [, i, j])`, `offset(v [, i, j])` | gain and offset |
| `add(other [, gain [, at]])` | mixes `other * gain` in, starting at sample `at` |
| `mul(other)` | multiplies sample by sample (ring modulation, envelopes) |
| `ramp(g0, g1 [, curve [, i, j]])` | multiplies by a gain moving from `g0` to `g1`; curves `"linear"`, `"fast"`, `"slow"`, `"s"`, `"power"` |
| `reverse([i, j])`, `invert([i, j])`, `abs([i, j])` | reorder and sign |
| `clip([lo, hi])`, `softclip([drive])`, `quantize(bits)` | limit, saturate, reduce bit depth |
| `removeDc()` | removes the mean, returns it |
| `peak([i, j])` | largest absolute value and its index |
| `rms([i, j])`, `sum([i, j])`, `minmax([i, j])` | measurements |
| `zeroCrossings([dir [, max]])` | indices where the signal crosses zero (`"up"`, `"down"` or `"any"`) |
| `nearestZero(i [, radius])` | index of the nearest crossing within `radius` samples, or nil |
| `envelope(attackMs, releaseMs, rate)` | new buffer with the level envelope |
| `tone(shape, hz, rate [, amp [, phase]])` | writes a `"sine"`, `"square"`, `"saw"` or `"triangle"` wave; returns the end phase |
| `noise([kind [, amp [, seed]]])` | writes `"white"`, `"pink"` or `"brown"` noise (same seed, same noise) |
| `lowpass(hz, rate)`, `highpass(hz, rate)` | one-pole filters |
| `biquad(kind, hz, q, gainDb, rate)` | `"lowpass"`, `"highpass"`, `"bandpass"`, `"notch"`, `"allpass"`, `"peak"`, `"lowshelf"`, `"highshelf"` |
| `delay(samples, feedback [, mix])` | feedback echo |
| `map(fn [, i, j])` | `buf[k] = fn(buf[k], k)`: one Lua call per sample, slow, for small ranges |

`dsp.dbToGain(db)`, `dsp.gainToDb(g)`, `dsp.noteToHz(note)` and `dsp.hzToNote(hz)` convert
units.

### Example

```lua
-- name: Gentle swell
-- category: Amplitude
function form(f) f:knob("Depth", 0.5, 0, 1) end
function apply(audio, p)
  for _, buf in audio:buffers() do
    buf:ramp(1 - p.Depth, 1, "s")
  end
end
```

Shipped audio scripts: `fade_in.lua`, `fade_out.lua`, `normalize.lua`, `reverse_smooth.lua`,
`tone_generator.lua`, `noise_generator.lua`, `zero_crossings.lua` (snap the selection to zero
crossings, mark them), `tremolo.lua`, `lofi.lua`, `stereo_width.lua`.

## Console

Settings > Debug has a Lua console with the controller modules (`transport`, `mixer`,
`channels`, `plugin`, `device`, `ui`), `files` (read and write in `<config>/scripts/console`)
and `project`. Typing an expression prints its value. The project edits of one console line
are one undo step; a line that fails leaves the project unchanged.
