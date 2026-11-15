# AI assistants (MCP)

Gridline includes a Model Context Protocol (MCP) server, so AI assistants such as Claude Desktop
and Claude Code can look at a project, edit it, render it and save it. There are two ways to use
it:

| Mode | How it starts | Works on |
|---|---|---|
| In the app | Settings > Integrations > "Allow AI assistants to control Gridline" | the project open in Gridline, live |
| Headless | `gridline mcp <project>` started by the assistant | a project file, without the app |

Both speak the same protocol and offer the same tools, resources and prompts.

## Safety

* The server in the app is **off by default**. Turning it on starts a listener on `127.0.0.1`
  only: other computers cannot reach it. It never binds to another address.
* Every request to the app must carry the token shown in Settings > Integrations
  (`Authorization: Bearer <token>`). "New" makes a new token; clients with the old one are
  refused. Requests that name another host or come from a web page on another origin are refused
  as well. The token is kept in the macOS Keychain (item "Gridline mcp-token") or, on Linux, in
  `secrets/mcp-token` in Gridline's settings folder (readable by you only, mode 0600), not in
  `settings.json`; a token from an older version is moved there on first use.
* A second switch, "Allow assistants to change the project and write files", is also off by
  default. While it is off, assistants can read the project and measure loudness, but every tool
  that edits, renders to a file, exports or saves answers with an explanation instead.
* Every editing call is **one undo step** labelled `AI: <action>` in Edit > Undo history, so you
  can undo any assistant change with Ctrl+Z (or the "Undo last assistant change" button). A call
  that fails changes nothing.
* Tools > AI assistant activity lists every call (time, assistant, action, result). The `AI` lamp
  in the status bar is lit while an assistant is connected and turns orange while it works.
* Files are written only inside the project's folder and the export folder you choose in
  Settings > Integrations. Paths that leave those folders (`..`, links pointing elsewhere) are refused, and
  each tool only writes its own file types (`.wav`, `.mid`, `.flp`, `.zip`, `.json`).
* There is no shell access. Lua scripts run in the same sandbox as Tools > Composing > Project scripts (no
  `io`, `os`, file or network access) with the time and instruction limits of Settings > Files.
* Calls are rate limited (bursts of 60, then 10 per second) and request sizes are bounded
  (8 MB per message, 100 000 notes per call, 256 KB per script).

## Connect Claude Code

Turn on Settings > Integrations > "Allow AI assistants to control Gridline" (and "Allow assistants
to change the project and write files" if the assistant should edit). The page shows a ready
command with your port and token; copy it with the Copy button:

```sh
claude mcp add --transport http gridline http://127.0.0.1:47390/mcp \
  --header "Authorization: Bearer <token from Settings > Integrations>"
```

To keep the token out of a shared configuration, put it in an environment variable and use a
project `.mcp.json`:

```json
{
  "mcpServers": {
    "gridline": {
      "type": "http",
      "url": "http://127.0.0.1:47390/mcp",
      "headers": { "Authorization": "Bearer ${GRIDLINE_MCP_TOKEN}" }
    }
  }
}
```

Instead of HTTP you can also use the stdio bridge, which reads the port and token from the file
the app writes while the server runs (see below):

```sh
claude mcp add gridline -- /path/to/gridline mcp --attach
```

To work on a project file without the app (for example on a build server):

```sh
claude mcp add gridline-song -- /path/to/gridline mcp "/path/to/My song.gridline" --export-dir "/path/to/Renders"
```

## Connect Claude Desktop

Claude Desktop starts servers as programs, so it uses the stdio bridge. Open Claude Desktop's
settings, Developer > Edit Config, and add Gridline to `claude_desktop_config.json`
(`~/Library/Application Support/Claude/claude_desktop_config.json` on macOS,
`%APPDATA%\Claude\claude_desktop_config.json` on Windows):

```json
{
  "mcpServers": {
    "gridline": {
      "command": "/path/to/gridline",
      "args": ["mcp", "--attach"]
    }
  }
}
```

`/path/to/gridline` is the command-line program that sits next to the app (`gridline`, not
`gridline-app`); Settings > Integrations shows its full path and copies this configuration.
Restart Claude Desktop afterwards. The bridge connects to whichever Gridline is running; if
Gridline is closed or the switch is off, tool calls answer with an error saying so. If Gridline
is restarted, the bridge opens a new session by itself.

For a headless server on one project instead, use
`"args": ["mcp", "/path/to/My song.gridline"]`.

## Headless server

```
gridline mcp <project.gridline|.flp> [--export-dir DIR] [--read-only] [--fl-data DIR]
gridline mcp --attach [--endpoint FILE]
```

* The project is loaded once; edits stay in memory until `save_project`. A `.gridline`
  path that does not exist yet starts a new project that `save_project` writes. An FL Studio
  `.flp` is imported; save it with `save_project` and a `.gridline` path (`.gridline.json`, the older name, works too).
* `--export-dir` adds a folder for renders and exports (relative paths go there).
* `--read-only` refuses every editing and file-writing tool.
* The protocol uses stdout; everything else the program prints goes to stderr, including one
  line per tool call. Unsaved changes are discarded when the client disconnects.

## Conventions

* Positions and lengths are in **beats** (quarter notes). Bar `n` starts at
  `(n - 1) * beatsPerBar` (`get_song` reports `beatsPerBar` and the PPQ).
* Channels, patterns, playlist tracks, markers, effect slots and automation clips are numbered
  from 1, as on screen. Mixer track 0 is the master, 1 and up are the inserts.
* Playlist clips are named by their id (`get_playlist`); ids stay valid while the clip exists.
* Parameters are named the way the scripting API names them: `volume`, `pan`, `mute`, `pitch`
  for channels; `volume`, `pan`, `stereo`, `mute`, `eqLowGain` and so on for mixer tracks; `mix`
  and `enabled` for effect slots; `tempo`, `swing`, `masterPitch` for the song. Plugin
  parameters are numbered from 1 or named by their display name. `get_parameters` lists them
  with their ranges; `set_parameter` takes plugin units, or 0..1 with `normalized: true`.
* Plugin types are ids such as `gl.synth` or `gl.reverb` (`list_plugin_catalog`). Type ids never
  change; display names do (0.6.1 gave the flagships names such as Tercet for `gl.synth`), and
  `list_plugin_catalog` finds a plugin by its former name too (`query: "time gate"`).

## Tools

An assistant that is unsure where to start calls `help`: it gives an overview, the tool list, the
path syntax, the fields of every kind of object and, for one tool, its schema and examples.
`get_song` is the compact summary of the whole project (with Stage rooms, arrangements and the
transport), and `find` looks up anything by name.

General tools that reach everything:

| Tool | What it does |
|---|---|
| `help` | overview, tools, paths, object fields, one tool with its schema and examples |
| `find` | finds channels, patterns, mixer tracks and effects, playlist tracks, automation clips, markers, Stage rooms, arrangements, plugin types and app commands by name, with their paths |
| `inspect` | reads any object with every field the project file holds (see Paths below) |
| `patch` | changes any fields of any object, or of several objects, as one undo step |
| `batch` | runs up to 200 tool calls in order as one undo step; the first failing call undoes the whole batch |
| `get_values`, `set_parameters` | read or set any number of parameters by path, as one undo step |
| `list_commands`, `run_command` | everything the command palette offers (menu items, keyboard commands, windows, settings pages, plugins), and running one of them (in the app) |
| `windows` | lists Gridline's windows and opens or closes one (in the app) |
| `get_settings`, `set_setting` | the user's settings, with their access (in the app) |
| `transport`, `select` | play, stop, the song position; what is selected (neither is an undo step) |

Reading (always allowed):

| Tool | What it does |
|---|---|
| `get_song` | song settings, length, counts, short lists, unsaved state |
| `list_channels` | channel rack: instrument, volume, pan, mute, routing, note counts |
| `list_patterns` | patterns with length and which channels have data |
| `get_pattern` | notes and step rows of a pattern |
| `get_playlist` | playlist clips and tracks in use |
| `get_mixer` | mixer tracks, effects per slot, sends |
| `get_parameters` | parameters of the song, a channel, a mixer track or an effect |
| `get_automation` | automation clips and their points |
| `get_markers` | song markers |
| `list_plugin_catalog` | built-in instruments, effects, note processors (and scanned plugins) |
| `list_presets` | factory and user presets of a plugin type |
| `list_swing_presets` | swing presets (factory and the user's), the one in use and the song's groove |
| `get_history` | latest undo steps |
| `analyze_loudness` | renders in memory and reports LUFS, peak, RMS, clipping |

Editing (one undo step each; needs "Allow assistants to change the project"):

| Tool | What it does |
|---|---|
| `set_song` | tempo, time signature, swing, a swing preset or groove, master pitch, title and other song info |
| `add_channel`, `edit_channel`, `remove_channel` | channels, instruments, presets, routing |
| `add_pattern`, `edit_pattern`, `remove_pattern` | patterns |
| `set_notes` | piano-roll notes of a channel in a pattern (replace or add) |
| `set_steps` | step-sequencer row of a channel in a pattern |
| `add_clip`, `edit_clip`, `remove_clip` | pattern clips on the playlist |
| `add_automation` | an automation clip for any parameter, optionally placed |
| `add_plugin`, `remove_plugin` | effects in mixer slots, with presets |
| `set_parameter` | any parameter |
| `edit_mixer_track` | name, volume, pan, mute, solo, routing |
| `add_marker`, `remove_marker` | song markers and time signature changes |
| `duplicate` | a channel (with its notes and steps), a pattern, or playlist clips |
| `edit_notes` | quantize, legato, transpose, nudge, velocity, humanize, strum, arpeggiate, chop, glue, flip, duplicate or delete notes, optionally in a time and pitch range |
| `edit_automation`, `remove_automation` | an automation clip's name, colour, range, loop and points (with curve shapes and tension) |
| `add_mixer_track`, `remove_mixer_track` | insert tracks |
| `set_send` | sends between mixer tracks: level, sidechain only, pre-fader, Stage room send |
| `stage_room` | Stage rooms: add, rename, colour, move, delete, move tracks into a room, the room's space |
| `arrangement` | playlist arrangements: list, add, select, rename, remove |
| `run_script` | a Lua project script (see `gridline://docs/scripting`) |
| `undo`, `redo` | undo or redo the latest assistant step (never the user's own edits) |

Files (need the same switch; only inside the project and export folders):

| Tool | What it does |
|---|---|
| `render_audio` | song or pattern to a 16 or 24-bit WAV, with its loudness |
| `export_project` | MIDI (song or one pattern), FL Studio `.flp`, FL Studio zipped loop package; `gridline_plugins` (`fallback`, `suite`, `render`) says how Gridline plugins without an FL Studio counterpart are written |
| `save_project` | save, or save as a new `.gridline` |

Jobs (always allowed):

| Tool | What it does |
|---|---|
| `get_job` | state (`running`, `done`, `failed`, `cancelled`), progress 0..1 and the result of a job |
| `cancel_job` | stops a running job; nothing is written |

`set_notes` takes every note property: start, length, pitch, velocity, pan, release, fine pitch,
mod X and Y, colour, group, slide and portamento, and the per-note bend, pressure and timbre
curves (points as `[time in beats, value]` or `{time, value, curve, tension}`). `get_pattern`
reads them back the same way.

### Paths

`inspect` and `patch` name objects by path. Numbers count from 1 as on screen:

| Path | Object |
|---|---|
| `song` | song settings (tempo, time signature, swing, info, master pitch and so on) |
| `channel/N` | a channel of the rack, with its instrument, note settings, echo, layers, MIDI ports |
| `pattern/N` | a pattern (its notes are in `pattern/N/notes`) |
| `mixer/N` | a mixer track (0 = master): strip, EQ, effect slots, sends, Stage depth and room |
| `track/N` | a playlist track: name, colour, height, live settings |
| `clip/<id>` | a playlist clip by its id (`get_playlist`) |
| `automation/N`, `marker/N` | automation clips and markers |
| `room/N` | a Stage room (`room/1` is the first) |
| `arrangement/N` | a playlist arrangement (its name) |
| `link/N`, `modlink/N` | controller links and modulation links |

A path can go on into a field (`channel/2/echo/count`); `patch` then takes `value` instead of
`set`. `patch` merges `set` into the object: nested objects merge, lists and plain values replace,
`null` removes an optional field, and colours can be written as `"#RRGGBB"`. The reply lists
fields that are not part of the object (ignored) and values that were corrected to their range
(adjusted). Ids, the lists that make up the song and a frozen state cannot be patched; use the
dedicated tools or the app's commands for those. `help` with topic `objects` describes the fields
of every kind of object.

Parameter paths for `get_values` and `set_parameters`: `song/tempo`, `channel/2/volume`,
`channel/2/Cutoff` (an instrument parameter by name or number), `mixer/3/pan`,
`mixer/3/slot/1/Decay` (an effect parameter). Values are in the parameter's units, `true` or
`false` for switches, or a choice's name; with `normalized: true` they are 0..1 of the range.

### Batches

```json
{"calls": [
  {"tool": "add_pattern", "arguments": {"name": "Verse"}},
  {"tool": "set_steps", "arguments": {"pattern": 3, "channel": 1, "steps": [100, 0, 0, 0]}},
  {"tool": "patch", "arguments": {"path": "channel/1", "set": {"name": "Kick"}}}
]}
```

The whole batch is one undo step. Without `continueOnError` a failing call undoes everything the
batch did and the error says which call failed and why. Renders, exports, saves, undo and redo
cannot be part of a batch.

### App commands, windows and settings

In the app, `list_commands` returns every row of the command palette with its id, where it lives
in the menus, its shortcut, whether it is on and, when it cannot run now, why. `run_command` runs
one row as if the user chose it, so anything the menus can do is reachable, and its edits are one
undo step. Commands that close, replace or reopen the project (new, open, revert, quit and the
like) are refused: ask the user to do those. Keyboard commands of an editor run once that editor
has the focus; the reply says so.

`get_settings` lists every setting with its type, value, default and access. `set_setting`
changes a setting and saves it. The settings that decide what assistants, scripts, the network,
updates and files may do stay with the user (access `user only`), and secrets such as tokens are
hidden. The headless server has no app, so these tools answer that the app is needed, and
`list_commands` lists the keyboard commands only.

### Errors that help

An unknown argument names the arguments the tool takes, an unknown tool points to `tools/list`
and `help`, an object number out of range gives the range, and a wrong parameter path explains the
path syntax.

### Renders and exports run in the background

`analyze_loudness`, `render_audio` and `export_project` render from a snapshot of the song taken
when the call arrives, on a worker thread: Gridline keeps responding, shows an "Assistant job"
window with the progress and a Cancel button, and later edits do not change the render. By
default the call waits and returns the result when the job is done. A client that sends a
`progressToken` in the request's `_meta` (and accepts `text/event-stream`) receives
`notifications/progress` while it waits; the stdio bridge passes them on. With `"wait": false`
the call returns at once with `{"job": N, "state": "running"}`; poll `get_job` with that number
and stop it with `cancel_job`. Up to four jobs run at once. Projects with hosted third-party
plugins render on the main thread (their offline instances need it), so the window waits for
those, as with File > Export. The headless server (`gridline mcp`) always works inside the call.

## Resources and prompts

| URI | Content |
|---|---|
| `gridline://project/summary` | the project summary (JSON) |
| `gridline://plugins/catalog` | every built-in plugin with parameters and presets (JSON) |
| `gridline://docs/scripting` | the scripting reference (`docs/scripting.md`) |

Prompts: `make_beat` (argument `style`, optional `bars`) and `review_mix`.

## Protocol details

* JSON-RPC 2.0, MCP revisions `2025-06-18`, `2025-03-26` and `2024-11-05` (the server answers
  `initialize` with the client's revision when it knows it, otherwise with `2025-06-18`).
* Methods: `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`,
  `resources/read`, `resources/templates/list`, `prompts/list`, `prompts/get`; notifications
  `notifications/initialized` and `notifications/cancelled`. Batches are accepted.
* Errors: `-32700` parse error, `-32600` invalid request (also any request before
  `initialize`), `-32601` unknown method, `-32602` unknown tool or arguments that do not match
  the tool's input schema, `-32002` unknown resource, `-32000` rate limit. A tool that runs but
  fails (no such pattern, path refused, script error) returns a result with `isError: true` and
  the reason as text.
* Tool results carry the data as JSON text and, from `2025-06-18` on, as `structuredContent`.
* HTTP (in the app): `POST /mcp` with `Content-Length`, answered with `application/json`;
  a waiting job call with a `progressToken` is answered as `text/event-stream` (progress
  notifications, then the reply) when the request accepts it. `GET` answers 405. `initialize` returns an `Mcp-Session-Id` header that
  later requests send back; `DELETE /mcp` ends the session. Missing or wrong token: 401;
  foreign `Host` or `Origin`: 403; unknown session: 404; body over 8 MB: 413.
* The bridge finds the app through `mcp-endpoint.json` in Gridline's settings folder
  (`$XDG_CONFIG_HOME/gridline` or `~/.config/gridline`). The app writes it with owner-only
  permissions while the server runs and removes it when the server stops.

## Troubleshooting

* "Gridline is not running, or Allow AI assistants to control Gridline is off": start Gridline
  and turn the switch on in Settings > Integrations.
* "Changes are switched off": turn on "Allow assistants to change the project and write files".
* "no folder is available for files": save the project once, or choose an export folder.
* Port in use: set another port in Settings > Integrations (0 picks a free one; the bridge still
  finds it, a direct HTTP configuration must be updated).
* Settings > Debug shows the log; assistant connections and edits are logged under `mcp`.
