# Tideline (`gl.tideline`)

Tideline is a one-knob rhythmic volume shaper. It pulls the volume down and lets it swell back in
time with the song, which gives you the pumping sound of sidechain compression without routing a
kick into a compressor. You can also trigger it from a kick on another track or from MIDI notes.
It is in the plugin picker under Dynamics (search for "sidechain", "pump" or "ducking").

Tideline adds no latency, runs per sample (the result doesn't depend on the buffer size) and
smooths every gain change, so it doesn't click.

## Main controls

- **Amount** (the big dial) sets how deep the duck goes. At 0 % the sound passes untouched. At
  100 % the full curve applies, which goes down to silence on the beat for most shapes. The inner
  ring on the dial shows the live gain reduction.
- **Rate** (top strip) sets how often the curve repeats: 1/16, 1/8, 1/4 (default), 1/2, 1 bar or
  2 bars, plus triplet (1/8T, 1/4T) and dotted (1/8., 1/4.) values. With a one-shot trigger it
  sets how long the curve lasts.
- **Shapes** (thumbnail strip). There are 16 built-in curves, plus one you draw yourself:

  | Group | Shapes | Use |
  |---|---|---|
  | Pump | Pump, Snap, Tight, Classic, Round, Long, Breath, Glide | Classic sidechain pumping, from short and tight to long and smooth |
  | Sub | Sub Soft, Sub Deep | Bass lines: they stay low longer and fade back in slowly, which gives the kick more room |
  | Trim | Trim, Trim Fade | Use these on the kick itself: they let the hit through and then cut its tail |
  | Reverse | Inhale, Inhale Long | The volume is sucked down into the next beat |
  | Rhythm | Chop, Stutter | Two cuts or four quick pumps per cycle |
  | Custom | Custom | Drag the 8 points in the display. Curve tension bends the segments between them |

- **Display** shows the gain envelope over one cycle (bright line, with Amount applied) and the
  plain shape (faint line). While the plugin runs it also shows a moving playhead, the live gain
  point, the gain reduction in dB and the output level traced over the cycle.
  - Drag left or right on the curve to shorten or stretch the duck (**Duration**).
  - Shift-drag to move the curve against the beat (**Shift**).
  - The reset arrow sets Duration and Shift back to 0, and the custom points back to their
    defaults.
- **Shift buttons** under the display move the curve by 1/16 of a cycle (outer buttons) or by
  1/256 (inner buttons).

## Trigger

- **Tempo** (default) locks the curve to the song position. It stays in sync after jumps and loop
  wraps. When the transport is stopped, the sound passes through.
- **Audio** starts the curve whenever the detector hears a hit. The detector listens to the
  **trigger input**: the track's sidechain input (route a kick track to it), the plugin's own
  input, or a numbered input route. Its controls are:
  - **Threshold** sets the trigger level.
  - **Gap** is the shortest time between two triggers.
  - **Low cut** and **High cut** set the detector's band. The default is 30 to 200 Hz, which
    finds kicks.
- **MIDI** starts the curve on every new note from a MIDI Out channel's port. **Port** -1 means
  any port. **Note** -1 means any note.
- **Loop** (Audio and MIDI only) repeats the curve at the rate until the next trigger. With Loop
  off, each trigger plays the curve once. Drag the **end marker** in the display to choose where
  it stops: the gain holds the curve's value there until the next trigger.

## Extras

- **Band**: Full, Lows or Highs. With Lows, only the frequencies below the **Split** frequency
  (20 Hz to 5.12 kHz) are ducked and the rest passes. Highs does the opposite. The split is a
  4th-order crossover, so its level stays flat. The headphones button (**Listen**) plays only the
  part being ducked.
- **Stereo**: Stereo ducks both channels. Mid ducks only the centre. Side ducks only the sides,
  so a centred kick and bass stay put while wide pads pump.
- **Smoothing** (0 to 20 ms) softens the gain edges. 1 ms is enough to avoid clicks. Larger
  values make the drop gentler.

Every control can be automated and linked to a MIDI controller, and each one has the usual right-click
menu. The presets menu in the plugin header holds the 11 factory presets and your own presets.

## FL Studio export

When you export to an FL Studio project, Tideline becomes a Gross Beat volume envelope. The
envelope is the tempo-synced curve with Amount, Duration and Shift applied, spread over a
bar-locked slot. Audio and MIDI triggers, the band split and Mid / Side do not carry over, and
the export notes say so.
