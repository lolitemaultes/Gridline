# Swing presets and grooves

Swing moves every second step a little later, so straight steps feel played. Gridline keeps the
song's swing as two things:

- the **amount**, the channel rack's Swing knob (0 to 100 %);
- the **shape**: which notes move, by how much and how loud they play.

A **swing preset** stores both under a name, so a feel can be saved, recalled and shared. Without a
preset the shape is the classic step swing, exactly as in earlier versions of Gridline.

## Choosing a preset

The preset menu sits right of the Swing knob in the channel rack's header and shows the preset in
use. When the rack is narrow it is the first thing to make room. Click it to see:

- **Factory**: the built-in presets;
- **My presets**: the presets you saved, with their folders as sub-menus;
- **Save current as preset...**, **Groove editor...**, **Import preset...**, **Export current...**
  and **Show in Finder** (Explorer, Files), which opens your swing preset folder;
- **Swing piano roll notes**: when it is off, only rack steps swing (as in projects opened from FL
  Studio files).

Picking a preset sets the Swing knob and the shape together, as one undo step. If you change
either one afterwards, the menu shows the preset's name with "(edited)".

The same presets are in:

- the **browser**, under Swing presets in the Plugins tab. Double-click one or drag it onto the
  rack's preset menu. Right-click a preset to export or delete it.
- **Tools > Swing presets**, so the command palette finds every preset by name.
- the piano roll's **Quantize** tool, as its groove templates.

## Factory presets

| Preset | What it does |
|---|---|
| Straight | No swing. |
| Gentle 16ths | Off-beat 16ths at 54 % of the pair. |
| Easy shuffle | Off-beat 16ths at 58 % of the pair. |
| Deep pocket | Off-beat 16ths at 62 %, a little softer. |
| Triplet 16ths | Off-beat 16ths on the triplet (two thirds of the pair). |
| Lilting 8ths | Off-beat 8ths at 58 % of the pair. |
| Triplet 8ths | Off-beat 8ths on the triplet, softer. |
| Behind the beat | Every 16th a little late, the off-beats the most. |
| Ahead of the beat | Every 16th a little early. |
| Loose hands | Eight 16ths of small human timing and dynamics. |
| Backbeat accents | No timing change; quiet off-beats, a strong beat 2 and 4. |
| Late hats | Late, quieter off-beat 16ths, as hi-hats often are. |

The percentage is where the off-beat lands between its beat and the next one: 50 % is straight,
about 67 % is a triplet feel, 75 % is the most the knob allows.

## The Groove editor

Open it from the preset menu or with **View > Groove editor**.

- **Grid**: what a cell is. *Step* is the pattern's step (16ths in a 16-step pattern, 32nds in a
  32-step one, as before). *8ths*, *16ths* and *32nds* stay the same note value whatever the
  pattern's steps.
- **Cycle**: how many cells repeat (2, 3, 4, 6, 8, 12 or 16). Two cells make a classic swing pair;
  16 sixteenths cover a whole bar. Cycles start again at every bar line; cells left over at the end
  of a bar stay straight.
- **Amount**: the Swing knob. It scales the timing lane: at 100 % a full bar in the lane moves a
  cell by half its length.
- **Accent**: how much of the velocity lane plays.
- **Timing** lane: drag up to play a cell later, down to play it earlier. Hold Alt for steps of
  2.5 %. Double-click resets the lane.
- **Velocity** lane: drag up for louder, down for quieter. Double-click clears the accents.
- The strip under the lanes shows one bar: the straight grid above, where each cell plays below.
- **From pattern notes** takes the groove from the selected channel's notes and steps in the
  current pattern (every channel when none is selected). **From audio clip** takes it from the
  transients of the audio clip selected in the playlist.
- **Save preset...** keeps the swing as one of your presets. Use a slash in the name to put it in a
  folder ("Drums/Late hats").
- **Reset to straight** goes back to no swing.

Every change is one undo step. Notes between two cell starts move in proportion, so 32nds inside a
swung 16th pair follow it. A channel's **Swing mix** (in its settings) scales both the timing and the
accents for that channel; truncated swing still ends swung steps on the grid.

## Grooves from audio

In the playlist, an audio clip's menu has two more items under **Warp markers**:

- **Swing to the song's groove** places warp markers on the clip's transients and moves each one to
  the nearest line of the song's swung grid, at the quantize strength set in the same menu. Undo
  takes it back in one step, and the markers can be edited like any others. See
  [warp markers](warp-markers.md).
- **Take its groove as the song's swing** measures the clip's transients (their timing and their
  level) and makes them the song's swing, then opens the Groove editor so you can save it.

Timing is measured from each cycle's first cell, so a clip that starts a little late is not taken
for a groove. A groove taken from audio, notes or a file makes a cell quieter, never silent: at
least a tenth of its velocity.

## Files

Your presets are small JSON files in the preset library, `presets/grooves` inside Gridline's
settings folder:

```json
{
  "format": "gridline-groove",
  "version": 1,
  "name": "Late hats",
  "grid": "1/16",
  "cycle": 4,
  "amount": 1,
  "timing": [0, 0.12, 0.02, 0.14],
  "velocity": [1, 0.7, 0.85, 0.7]
}
```

`timing` is a fraction of a cell at full amount (-0.5 to 0.5), `velocity` a factor per cell (0 to 2,
optional), `accent` (optional) the share of the velocity lane that plays. Gridline reads what it can
from newer versions of the format, pads short lanes and keeps every value in range.

**Import preset...** also accepts a MIDI file (its notes become a groove on 16ths) and a score file
(.fsc). Importing FL Studio's presets (in Settings, or with `gridline import-fl-presets`) converts
the quantize grooves of the FL Studio installed on this computer into swing presets under
"FL Studio/Quantization"; nothing of them is shipped with Gridline.

## Projects, collaboration and FL Studio

- The project stores the shape and the preset's name; a project without a preset saves exactly as
  before.
- In a shared project a preset, a groove edit and the Swing piano roll notes switch reach everyone.
- FL Studio has one swing amount for the whole project. An exported project gets the Swing knob's
  amount when the swing is a plain pair on the pattern's steps (or a 16th pair in 16-step patterns),
  which plays the same in FL Studio. A pair on another grid writes its amount; any other groove
  writes no swing. The export report says when a groove's timing or accents stay in Gridline only.
- Opening an FL Studio project gives the classic step swing at its amount, swinging steps only.

## Scripts and assistants

Lua project scripts: `project.song.swingPreset()`, `project.song.swingPresets()`,
`project.song.applySwingPreset(name)`, `project.song.groove()` and `project.song.setGroove(t)` (see
[scripting](scripting.md)). The assistant tools have `list_swing_presets`, and `set_song` takes
`swingPreset` and `groove` (see [MCP](mcp.md)).
