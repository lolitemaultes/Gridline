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
  as well.
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
* There is no shell access. Lua scripts run in the same sandbox as Tools > Project scripts (no
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
claude mcp add gridline-song -- /path/to/gridline mcp "/path/to/My song.gridline.json" --export-dir "/path/to/Renders"
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
`"args": ["mcp", "/path/to/My song.gridline.json"]`.

## Headless server

```
gridline mcp <project.gridline.json|.flp> [--export-dir DIR] [--read-only] [--fl-data DIR]
gridline mcp --attach [--endpoint FILE]
```

* The project is loaded once; edits stay in memory until `save_project`. A `.gridline.json`
  path that does not exist yet starts a new project that `save_project` writes. An FL Studio
  `.flp` is imported; save it with `save_project` and a `.gridline.json` path.
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
* Plugin types are ids such as `gl.synth` or `gl.reverb` (`list_plugin_catalog`).

## Tools

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
| `get_history` | latest undo steps |
| `analyze_loudness` | renders in memory and reports LUFS, peak, RMS, clipping |

Editing (one undo step each; needs "Allow assistants to change the project"):

| Tool | What it does |
|---|---|
| `set_song` | tempo, time signature, swing, master pitch, title and other song info |
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
| `run_script` | a Lua project script (see `gridline://docs/scripting`) |
| `undo`, `redo` | undo or redo the latest assistant step (never the user's own edits) |

Files (need the same switch; only inside the project and export folders):

| Tool | What it does |
|---|---|
| `render_audio` | song or pattern to a 16 or 24-bit WAV, with its loudness |
| `export_project` | MIDI (song or one pattern), FL Studio `.flp`, FL Studio zipped loop package |
| `save_project` | save, or save as a new `.gridline.json` |

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
* HTTP (in the app): `POST /mcp` with `Content-Length`, answered with `application/json`
  (no event streams; `GET` answers 405). `initialize` returns an `Mcp-Session-Id` header that
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
