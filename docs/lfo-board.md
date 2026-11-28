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

## The cards and the inspector

Each source is a calm card: its colour, on lamp, number and name (double-click to rename), its wave
as it plays, a one-line summary (shape, rate and restart) and how many targets it moves. The round
**handle** and the **...** menu show when you point at a card or select it. Click a card to select it:
the **inspector** beside the cards shows everything about it:

- The wave, large, with a dot at the current position (for the drawn shape: the shape editor, see
  [Drawing a shape](#drawing-a-shape)).
- Eight shapes: sine, triangle, saw up, saw down, square, random hold, random glide and **Draw**.
  **Edit this shape** turns the shape shown into points you can change.
- **Hz** or **Tempo**, the rate (note values with dotted and triplet ones, or 0.01 to 40 Hz), and
  when it restarts: **Free** (runs freely), **Play** (locked to the song: the same place in the song is
  always the same place in the wave) or **Note** (restarts on every note of a channel).
- Knobs for **Rate**, **Phase**, **Depth**, **Offset** and **Smooth**, in the source's colour. These
  are ordinary knobs: they can be automated, linked to a macro or moved by another source.
- **Targets**: a row for each knob the source moves, with its owner, the control and the amount as a
  bar. Click a row for its settings; **+ Add target** lists every control in the project, with a
  search, to pick from.
- **Presets**: load a preset into the source, or save it as one (see [LFO presets](#lfo-presets)).

Where the board is short (Smooth's lower pane), the inspector lays out in two columns: the picture
and the shapes on the left, the rate, knobs and targets on the right. Where it is narrow, the cards
fill the width without an inspector; select a card and use **Edit in a window**.

**+ New LFO** adds a sine at 1/4, locked to the song; the arrow beside it adds **Chaos**, **Random**
or a **New LFO from a preset**. With no sources yet the board shows the same three choices. The
toolbar filters the cards (**All**, **Moving now**, **No targets**), finds a source or a target by
name, sorts them and switches between **Cards** and **Compact** (one row each; the list scrolls a row
at a time).

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

## Drawing a shape

Pick **Draw** in the shapes, or **Edit this shape** to start from the shape you have (a sine, a saw,
the random steps and so on become points that play the same). The inspector's large wave becomes
the editor:

- **Points**: click to add a point, drag a point to move it, drag the small diamond in the middle of a
  line to bend it, right-click a point to remove it.
- **Pencil**: draw the shape by hand. With a grid, every cell you cross becomes a step at the height
  you drew; without one, the line follows your hand.
- **Grid**: off, or 4, 8, 16, 32 or 64 steps across the cycle. Points snap to it.
- **Line**, **Smooth** and **Step** set the selected point: a straight line to the next point (bent by
  its curve), a smooth curve flat at both points, or a hold until the next point.
- **Flip** mirrors the shape left to right, **Invert** turns it upside down and **Clear** leaves a flat
  line at the middle to draw on.

Each stroke, point move or button is one undo step. The shape is saved with the project and, in a
shared session, reaches the others as one change when you let go of the mouse. A shape holds up to
128 points.

## LFO presets

A preset keeps a whole source: its shape (a drawn one too), rate, restart and knobs. It never keeps
the targets, so loading one into a source changes how it moves and leaves what it moves alone.

- **Load**: the card's **...** menu (**Load a preset**), the inspector's **Presets** button, or the
  Browser's **LFO presets** (double-click loads one into the selected LFO, or makes a new one when none
  is selected; right-click for **New LFO from it** and **Load into the selected LFO**).
- **New**: **New LFO from a preset** beside **+ New LFO**, in the board's **...** menu and in every
  knob's **Modulate > LFO** menu.
- **Save**: **Save as a preset...** in the card's menu or under **Presets**. A name with a `/` makes a
  folder. Your presets go to the `lfos` folder of your preset folder (see
  [the data folder](data-folder.md)) and show under **My presets** next to the factory ones.

The factory presets are Slow swell, Wobble 1/4, Dotted wobble, Triplet tremolo, Gate 1/8, Ramp up bar,
Random steps, Drift, and three drawn ones: Stutter gate, Sidechain pump and Bounce 1/2.

The Browser lists **LFO presets** with the other presets in Studio, Smooth and the Tracker, and as a
place of its own in Clips.

## More settings

The card's **...** menu renames, turns off, duplicates, colours and reorders a source (or a Chaos or
Random source, with its seed and, for Random, Roll, the spread and the steps), loads and saves presets
and has the settings that are not on the card: **Up only** (the knob is the bottom of the swing), the
pulse width of the square, the steps per cycle and new values of the random shapes, a **Fade in**
after each restart and the channel whose notes restart a **Note** LFO. **Edit in a window** shows the
selected source in a window of its own, the same as the inspector.

## Saving and sharing

LFOs, Chaos and Random sources are saved with the project. A project saved before the LFO board opens
and plays exactly as it did. A version of Gridline that does not know a kind of source leaves that
source and its links out rather than playing it as something else. In a shared session every change to an LFO or a target is one undo step and reaches the others
at once.

## From an automation clip

The automation editor's **LFO tool** draws an LFO into a clip as points. **Make a live LFO instead**
makes a song LFO with the same shape and speed that sweeps the clip's parameter between the same two
values as it plays, and opens the board; the clip itself is left as it is.
