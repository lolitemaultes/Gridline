# Recording Platter performances

Platter (`gl.platter`) records what you play on its two decks while Gridline records: the
mixer, both decks, cues, loops, beat jumps and scratches. By default a pass becomes ordinary
pattern data you can see in the piano roll and the playlist and edit like anything else.
Played back without edits, it is exactly the pass you played.

## Recording a pass

1. The setup's record button (in the Platter window) shows **Record armed**. A new setup is
   armed; click the button to switch it to **Record off** and back.
2. Next to it, two buttons pick what a pass becomes: **Automation** (the default) or
   **Take**.
3. Press Gridline's Record and play, in pattern mode or song mode. While Platter captures, its
   button reads **Recording**.
4. Stop. The pass is written into the pattern as one undo step ("Record a deck performance").

Where it goes:

- **Pattern mode.** The pass goes into the selected pattern when that pattern is empty. If it
  already holds a pass of this setup, Gridline makes a new pattern named after the channel
  (for example "Platter take 2"), selects it and records there. The earlier pattern is never
  changed: a second pass never merges into the first.
- **Song mode.** The same rule applies, and a new pattern is placed in the playlist where its
  pass started.
- **Loop recording.** When the transport loops while you record, every loop pass is a pass of
  its own: the first one fills the pattern and each next one goes into a new pattern. A pass
  ends exactly on the sample the loop wraps and the next one starts on that same sample.
- **Punch in and out** (song mode, a time selection in the playlist, "Punch in/out at the time
  selection" on in the Record button's right-click menu): the pass starts on the first beat
  of the selection and ends exactly where the selection ends.

## What a pass becomes

| You did | In the pattern |
|---|---|
| Moved a fader, knob or EQ, the crossfader, the pitch fader, the filter | An automation lane on that control (open it in the piano roll's event lane, for example "A EQ low") |
| Pressed a button (play, cue, hot cues, clearing a hot cue, loops, beat jumps, back to start, reverse, pitch bend) | A note on that button's key, as long as you held it. The piano roll names these keys ("A Play", "B Hot cue 3") |
| Scratched with the mouse | A note on "A Mouse hand" while you held the record, and the "A mouse travel" lane for where the hand moved it |
| Scratched or nudged with a jog wheel | The "A wheel travel" lane (the travel the jog gave the record) and "A nudge" for rim nudges; touching the jog top is a note on "A Wheel touch" |
| Searched or seeked | The "A record position" lane |
| (always) | A "Performance start" note at the start of the pass |

Deck B has the same lanes and keys with a "B" in front.

The **Performance start** note puts both decks back the way they were when the pass started:
the tracks' positions, speeds, loops, cues and every control. A control that has no lane in
the pattern keeps its value from the pass until you turn it (or automation elsewhere moves
it), so a later pass that ends with different settings does not change this one. Leave the
note at the front of the pattern. Without it, a replay continues from whatever state the
decks are in.

The travel lanes (mouse and wheel) wrap: a hand that keeps turning the record past one turn
starts again at the other edge of the lane, so a scratch fills the lane's height.

## Editing

All of it is regular pattern data, so the usual tools work: move or stretch the notes, delete
a hot cue press, draw over a lane, copy the pattern, put it elsewhere in the playlist.
Editing changes the sound. For example, drawing a new curve on "A mouse travel" changes the
scratch, and moving a "B Play" note moves the moment deck B starts.

Notes and points keep their exact position between grid lines, so an unedited pass lines up
with the sample it was played on. Quantizing snaps them to the grid as usual.

## Exact playback

An unedited pass plays back exactly as it was played: every control has the same value on the
same sample, and the audio is the same sample for sample. This holds for any audio buffer
size, including a different one from the one it was recorded with, in pattern and song mode,
and with key lock on. The positions between grid lines are saved with the project, synced in
collaboration sessions and kept when you copy or move the data.

Cases where playback is not the same as the pass:

- The project tempo changed after recording, or tempo automation runs during the pass. The
  pass is stored in beats, so it plays at the new tempo.
- Playback starts in the middle of a pass, after its Performance start note. The decks are
  not reset, so they go on from their current state.
- A loop drawn in Platter's track view while a pass is recorded. That edit is not part of the
  pass.

## Takes

With **Take** selected, a pass becomes a single take note that replays it as played, the way
Platter recorded passes before. Takes follow the same new pattern rule. Takes recorded
earlier keep playing, whichever of the two is selected.
