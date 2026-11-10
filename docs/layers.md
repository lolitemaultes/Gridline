# Layers: splits, stacks and macros

A layer channel plays its notes on the channels it holds (its children). With zones, one layer
becomes a split or a stacked instrument: bass on the low keys and keys above, a soft sound under a
hard one, a wide stack that only opens up at the top, or round robin hits that change on every note.

## Making a layer

- Select some channels in the channel rack, right click and choose **Create layer from selected**.
- Or drag a layer preset (a `.gllayer` file) from the browser onto the channel rack. Its
  instruments, their effects, the zones and the macros arrive as new channels in one undo step.

Open the layer's window to edit it. The Plugin tab shows the layer editor.

## The layer editor

**Lanes.** Every child has a lane: a grip to drag it up or down, its colour, its name and
instrument, a light that shows it playing, **M** (mute) and **S** (solo within the layer), its
level, pan and transpose. Drop an instrument from the browser or a channel from the rack on the
last row, or use **Add child**, to add one. Right click a lane to open, duplicate or remove the
child.

**Zones.** The bar on each lane is the part of the keyboard (**Keys**) or the velocity range
(**Velocity**) the child plays.

- Drag the bar to move it, its ends to resize it, and the small squares on its top corners to fade
  it in and out. Where two zones fade into each other, the notes crossfade: each child plays the
  note softer the further it sits into its fade.
- Double-click a bar to give the child the whole range again.
- **Split** shares the keyboard (or the velocities) equally among the children, with a short
  crossfade. **Reset** gives every child the whole keyboard and every velocity again.

**Mode.** How the children share a note:

| Mode | What plays |
| --- | --- |
| All | every child whose zones hold the note |
| Random | one of them, picked at random |
| Cycle | one of them, the next one each note |
| No repeat | one of them at random, never the same one twice in a row |

Notes from the playlist and the piano roll pick the same children on every play, so a render sounds
like what you heard.

## Macros

The layer has eight macro knobs. Each can turn any number of controls of its children at once:
their instrument's controls, their note processors, their level, pan and pitch, and the effects of
a mixer track only that child plays into.

- Press **Map** under a knob and move a control, or pick one from the knob's list.
- Select a macro to see its targets. Each target has its own **from** and **to** range, and can
  be removed.
- Double-click a macro's name to rename it.
- Macros can be automated and linked to a controller like any other knob.

## Layer presets

**Save layer...** writes the layer, its children, their own mixer effects, the zones and the macros
to a `.gllayer` file in the layers folder of your presets. **Load layer** adds a saved layer or a
built-in one to the project. A child that shares its mixer track with other channels is saved
without that track's effects; the editor tells you which ones.

Layer presets show in the browser too: drag one onto the channel rack or the workspace.

## Exported `.flp` projects

When a project is exported as an `.flp` file, a layer is written with its children, but the zones,
the Cycle and No repeat modes and the macros are not: the export lists them in its report. Layers
in imported `.flp` files play exactly as before.
