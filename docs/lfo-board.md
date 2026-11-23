# Moving any knob with an LFO

The **LFO board** holds the song's LFOs, and its **Chaos** and **Random** sources (see
[Chaos and Random](#chaos-and-random)): up to 32 of them together, each able to move any number of
knobs anywhere in the project. A built-in or installed plugin's parameter, a channel or mixer control, an
effect's mix, a macro (which then moves everything it drives) and even another LFO's own settings can
all be moved.

Open it with **Alt+F9** or **View > LFO board** in every layout:

- **Studio**: a tab beside the Mixer. In the narrow mixer column it shows a compact list; **Pop out**
  opens the full board in a window of its own, and **Dock** puts it back beside the mixer.
- **Clips**: a tab in the mixer band. The LFO you pick shows with all its settings in the device band.
- **Smooth**: an **LFO board** segment in the lower pane.
- **Tracker**: the **LFO** page, after Mix. As with the other page keys, Alt+F9 shows the page (pressed
  again it stays); **Exit** or Esc closes the board and goes back to the pattern.

## The cards

Each LFO is a card, four across on a wide screen and one column in a narrow space:

- The header: the on lamp (off: every target keeps its own value), the LFO's number and name
  (double-click to rename), its shape, the round **handle** and the **...** menu.
- The wave, as it plays, with a dot at the current position.
- Eight shapes: sine, triangle, saw up, saw down, square, random hold, random glide and a drawn shape.
- **Hz** or **Tempo**, the rate (note values with dotted and triplet ones, or 0.01 to 40 Hz), and
  when it restarts: **Free** (runs freely), **Play** (locked to the song: the same place in the song is
  always the same place in the wave) or **Note** (restarts on every note of a channel).
- Knobs for **Rate**, **Phase**, **Depth**, **Offset** and **Smooth**. These are ordinary knobs: they
  can be automated, linked to a macro or moved by another LFO.
- **Targets**: a chip for each knob the LFO moves, with how far. Click a chip for its settings; **+ Add
  target** lists every control in the project, with a search, to pick from.

The dashed **New LFO** card (or **+ New LFO**) adds a sine at 1/4, locked to the song. The toolbar
filters the cards (**All**, **Moving now**, **No targets**), finds an LFO or a target by name, sorts
them and switches between **Cards** and **Compact**.

## Linking a knob

Three ways, each one undo step:

- Drag an LFO's **handle** onto any knob. Every knob that can take it shows a ring while you drag, and
  the tooltip says what the drop will do ("Drop: LFO 1 moves Resonance").
- Right-click any knob and choose **Modulate > LFO**: pick an LFO, **New LFO**, or a new LFO from a
  preset. (**Modulate > Chaos** and **Modulate > Random** do the same for the other sources.)
- **+ Add target** on a card: a search over every control, the ones you touched lately, or **Pick on
  screen** (the next knob you click anywhere becomes the target; Esc cancels).

A new target swings **around the knob** by 25 percent: the knob stays where you set it and the LFO
moves it up and down from there. Its settings:

- **Moves**: **Around the knob** (by the **Amount**, minus 100 to plus 100 percent) or **Between two
  values** (the LFO sweeps from one value to another and the knob's own setting is ignored).
- **Direction**: both ways, up only or down only.
- **Invert**, the **Curve** the movement follows, and **Smoothing**.
- **Show the knob** opens the window with the control, **Disable** stops this LFO moving it (the link
  stays), **Remove target** removes the link.

Hold **Alt** and drag a knob an LFO moves to change the amount without opening anything. A knob an LFO
moves shows its swing as a dotted arc and its live value as a dot; **Show on knobs** on the board's
toolbar turns these marks off.

## The Modulate menu

Every knob's right-click menu has one **Modulate** submenu with every source that can move it, always
in this order:

| Row | What it holds |
|---|---|
| **LFO**, **Chaos**, **Random** | the board's sources of that kind (a check on each one moving the knob; click to link it or unlink it), then **New LFO**, **New chaos** or **New random** (a new source moving the knob, one undo step) and, for LFOs, **New LFO from a preset** |
| **Macro** | the channel's or mixer track's macros (and a layer's), **New macro**, **Unlink from macro** |
| **Controller plugin** | the outputs of the project's controller plugins (greyed out when there are none) |
| *source* **settings** | one row per source moving the knob now, with its amount: the same settings as its chip on the board |
| **Remove all modulation** | removes every source's link to the knob in one undo step |
| **Open the LFO board** | Alt+F9 |

A submenu with a source moving the knob has a dot after its name. Rows that have nothing for the
control are left out (a song setting such as the tempo takes no modulation; a macro knob is not
linked to its own set). The same menu is on plugin parameter lists, an installed plugin's parameters
and **Tools > Last moved control**; the MIDI link rows stay at the end of the knob menu, after it.

## Chaos and Random

Two more kinds of source sit on the board beside the LFOs. They move knobs exactly as an LFO does:
the same handle to drag, the same targets with their amount, direction, invert, curve and
smoothing, the same marks on a moved knob, Alt-drag, undo and saving. Add one with **+ Chaos** or
**+ Random** on the toolbar (in the **...** menu when the board is narrow), the dashed new card, or
a knob's **Modulate > Chaos** or **Modulate > Random**. They are numbered by kind: Chaos 1,
Random 1.

**Chaos** is a smooth movement that never repeats. Its card shows the walk round where it is now.

- **Rate**: in Hz or note values (**Hz** / **Tempo**): about one bend of the line per cycle.
- **Character**: calm (one slow bend at a time) to wild (faster layers on top and sudden turns).
- **Depth**, **Smooth** and **Offset**, as on an LFO.
- **Free**, **Play** (locked to the song), **Note** (starts again on every note of a channel) or
  **Bar** (starts again at every bar, so each bar moves the same way).
- **+-** or **+**: both ways round the knob, or up only.
- The **seed**: drag it or double-click to type one; **New seed** in the card's menu.

How it works: at every whole cycle of the rate the seed gives a repeatable random value, and a smooth
curve joins them (a smoothed random walk). The character adds up to three faster, weaker layers of
the same kind and lets a slow layer push the walk ahead and hold it back.

**Random** holds a random value until the next trigger. Its card shows the steps.

- The trigger: **Tempo** (a new value every note value of the rate, locked to the song), **Hz** (at a
  rate in times a second), **Note** (every note of a channel, or of every channel; the button under
  it picks) or **Roll** (only when you press **Roll**).
- **Depth**, **Glide** (slides from one value to the next instead of jumping) and **Offset**.
- **Even** or **Middle**: every value as likely as any other, or values near the middle more often.
- **Steps**: snap every value to 2 to 16 even steps across the range (the card draws them).
- **+-** or **+**, and the seed, as for Chaos.

Both are repeatable: the same seed from the same place in the song plays the same movement every
time, so an export comes out the same on every run and every computer. Nothing random is kept on one
computer only: in a shared session the sources, their seeds and every **Roll** reach everyone (a roll
is one undo step). A Chaos or Random source that moves no knob costs nothing while the song plays or
rests.

## LFOs, macros and other LFOs

An LFO can move a macro, and everything the macro drives moves with it in the same moment. An LFO can
also move another LFO's depth, rate or phase. Gridline refuses a link that would make a loop (an LFO
moving a macro that moves the same LFO's depth, for instance) and says which links would close it.

## Presets and more settings

The card's **...** menu renames, turns off, duplicates, colours and reorders an LFO (or a Chaos or
Random source, with its seed and, for Random, Roll, the spread and the steps), applies one of
the presets (Dotted wobble, Quarter pump, Slow drift, Sixteenth gate, Random steps, Long swell) and has
the settings that are not on the card: **Up only** (the knob is the bottom of the swing), the pulse
width of the square, the steps per cycle and new values of the random shapes, a **Fade in** after each
restart and the channel whose notes restart a **Note** LFO. **Edit in a window** shows the LFO with
all its settings, every target's settings and, for the drawn shape, an editor: click to add a point,
drag to move it, right-click to remove it.

## Saving and sharing

LFOs, Chaos and Random sources are saved with the project. A project saved before the LFO board opens
and plays exactly as it did. A version of Gridline that does not know a kind of source leaves that
source and its links out rather than playing it as something else. In a shared session every change to an LFO or a target is one undo step and reaches the others
at once.

## From an automation clip

The automation editor's **LFO tool** draws an LFO into a clip as points. **Make a live LFO instead**
makes a song LFO with the same shape and speed that sweeps the clip's parameter between the same two
values as it plays, and opens the board; the clip itself is left as it is.
