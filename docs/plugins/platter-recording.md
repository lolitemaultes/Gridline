# Recording Platter performances

Platter (`gl.platter`) records what you play on its two decks while Gridline records: the
mixer, both decks, cues, loops, beat jumps and scratches. By default a pass becomes plain
automation, one curve per control you used: the record's position, play, the wheel, the mouse,
loops and every fader and knob, kept together in one pattern. You see the wheel moving in the
playlist, you can cut, trim, move and copy the pass, and playback can start anywhere in it.
Played back without edits, it is exactly the pass you played.

## Recording a pass

1. The setup's record button (in the Platter window) shows **Record armed**. A new setup is
   armed; click the button to switch it to **Record off** and back.
2. Next to it, two buttons pick what a pass becomes: **Automation** (the default) or
   **Take**. With Automation, the **Song** buttons pick what a song-mode pass becomes on the
   playlist: **Pattern** (the default) or **Clips**.
3. Press Gridline's Record and play, in pattern mode or song mode. While Platter captures, its
   button reads **Recording**.
4. Stop. The pass is written as one undo step ("Record a deck performance").

Where it goes:

- **Song mode.** Each pass becomes one new pattern named after the channel (for example
  "Platter take 1") that holds the pass's automation clips, one per control. The pattern is
  placed once on the playlist, where the pass started and exactly as long as the pass, on the
  setup's instrument track when it is free there, else on the first free track. The clips are
  named after the channel and the control, for example "Platter A wheel" or "Platter
  Crossfader", and coloured by deck. Their curves show on the pattern's playlist clip; double-click
  one to open it in the automation editor (see
  [automation in a pattern](../automation.md#automation-in-a-pattern)). Move, trim, cut or copy
  the clip and the whole pass goes with it. A second pass gets a pattern of its own ("Platter
  take 2") on its own track: it never records over the first.
- **Song mode, Clips.** With **Clips** selected, a pass becomes a set of automation clips in the
  playlist instead, one per control, starting where the pass started, on free tracks next to
  each other and joined in one clip group (so they select, move and cut together). Use it when
  you want to move or edit one control's clip on its own in the playlist.
- **Pattern mode.** The pass goes into the selected pattern as automation clips of the
  pattern, when that pattern is empty. They have the same names, colours and zoomed position
  range as song mode's clips ("Platter A wheel", "Platter A position"), and they play wherever
  the pattern plays. If the pattern already holds a pass of this setup, Gridline makes a new
  pattern named after the channel (for example "Platter take 2"), selects it and records there.
  Put the pattern in the playlist and its clips show the curves (the wheel moving) over it;
  the channel rack lists them as rows below the channels. Click one to edit it (see
  [automation in a pattern](../automation.md#automation-in-a-pattern)).
- **Loop recording.** When the transport loops while you record, every loop pass is a pass of
  its own (a new pattern, or a new set of clips). A pass ends exactly on the sample the loop
  wraps and the next one starts on that same sample.
- **Punch in and out** (song mode, a time selection in the playlist, "Punch in/out at the time
  selection" on in the Record button's right-click menu): the pass starts on the first beat
  of the selection and ends exactly where the selection ends.

## What a pass becomes

| You did | Its clip or lane |
|---|---|
| Played the record (anything that moved it) | "A position": where the read head is, on every step, with jumps (cues, hot cues, loops wrapping, beat jumps, seeks) as straight steps |
| Pressed play or stop | "A play": on or off, with the motor's start and stop |
| Touched the jog top / turned the jog | "A wheel touch" (on while your hand is on it) and "A wheel" (the wheel's travel, wrapping at one turn) |
| Held the record with the mouse | "A mouse hand" (on while you held it) and "A mouse travel" |
| Nudged with the jog rim | "A nudge" |
| A loop | "A loop" (on or off), "A loop in" and "A loop out" |
| Reverse, pitch bend | "A reverse", "A pitch bend" |
| Moved a fader, knob or EQ, the crossfader, the pitch fader, the filter | A clip on that control, for example "A EQ low" |

Deck B has the same set with a "B" in front. A pass only writes the clips for what it used, and
the state ones (play, touch, hand, loop, reverse, bend) also when they were on at the start.
Nothing goes into the piano roll: there are no notes to keep in place.

## Playing it from anywhere

The position, play, touch, hand, loop, reverse and bend clips hold the decks' state, not
moments in time. While one of a deck's clips plays, that clip drives the deck: start playback
on the first beat of the pass, in the middle of a scratch or anywhere after it, and the deck is
put where the pass says, playing or stopped, held or free, looping or not, and goes on from
there. The same happens when you cut the clips in two, trim their start, move them later or
copy them elsewhere: wherever they play, the pass plays.

- From the pass's first step the decks come back exactly as they were when it started (the
  tracks' positions, speeds, loops and every control), and the replay is exact.
- From anywhere else the deck is picked up on the pass's own step (within a step, about 0.7 ms)
  with a short crossfade, then plays on from the lanes. On a steady part it matches the pass
  within a few milliseconds; started inside a scratch, the scratch goes on from the lanes and
  matches the pass at the latest once the hand lets go.
- A control without a clip of its own keeps the value it had in the pass until you turn it.
- Outside the pass's clips the decks are free again: your hands and controller play them.
- While a pass drives a deck, the deck ignores its own buttons, jog, mouse and seek; the mixer
  controls without a clip stay yours.

## Editing

All of it is regular automation, so the usual tools work: draw over a curve, move points,
delete a clip, cut, trim, move and copy clips. Editing changes the sound from the edit on:

- Draw a jump into "A position" and the record jumps there; delete "A mouse hand" and the
  mouse scratch is gone; flatten "A EQ low" and the kill is gone.
- When the position curve and the record part ways by more than a few frames (because you
  edited it), the record goes where the curve says.

Points keep their exact position between grid lines and their exact value, so an unedited pass
lines up with the sample it was played on. The position clip is zoomed on the record's travel.

## Exact playback

An unedited pass played from its start plays back exactly as it was played: the audio is the
same sample for sample. This holds for any audio buffer size, including a different one from
the one it was recorded with, in pattern and song mode, with key lock on, and also after the
clips are cut in two or moved to another place. The exact values are saved with the project,
synced in collaboration sessions and kept when you copy or move the clips.

Cases where playback is not the same as the pass:

- The project tempo changed after recording, or tempo automation runs during the pass. The
  pass is stored in beats, so it plays at the new tempo.
- Playback starts in the middle of a pass: the deck is picked up within a step and a short
  crossfade, so the first moments differ slightly (see above). Sync's phase lock and key lock
  start fresh there.
- A loop drawn in Platter's track view while a pass is recorded. That edit is not part of the
  pass.

## Passes recorded earlier

Passes recorded by earlier versions (notes on the performance keys with a Performance start
note, in a pattern) keep playing exactly as before. Two of their lanes show new names: the
record position lane is now "A seek" and the wheel travel lane "A wheel".

Song-mode passes recorded as separate playlist clips (before passes became one pattern, or
with **Clips** selected) keep playing exactly as before, also in a project that has passes in
both forms: each pass plays where it is placed, and another pass's clips on the same controls
do not reach into it.

Pattern-mode passes recorded before patterns held named automation clips keep playing exactly
as before too: their lanes open as unnamed clips of the pattern (shown by the control's name,
for example "Platter > A wheel") with the control's whole range and the curves they were
recorded with. Nothing is converted, so the sound cannot change; rename or recolour them like
any other clip.

## Takes

With **Take** selected, a pass becomes a single take note that replays it as played. Takes
follow the same new pattern rule. Takes recorded earlier keep playing, whichever of the two is
selected.
