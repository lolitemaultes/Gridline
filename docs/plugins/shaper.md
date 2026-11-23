# Shaper (`gl.shaper`)

Shaper is a rhythmic gate and filter shaper. Three lanes of steps, **Volume**, **Cutoff** and
**Pan**, play in time with the song and move the level, a filter's cutoff and the stereo position.
It is in the plugin picker under Filter.

## Units

- **IN:** input gain, and **Band** (Full, Lows, Highs) with its **Split** frequency: with Lows or
  Highs only that part of the sound is shaped and the rest passes untouched.
- **FILTER:** the filter type (off, low pass, band pass, high pass, notch, shelves, peak, all pass),
  its range (**Min** and **Max**), resonance, shelf gain, how far the cutoff lane moves it
  (**Env amt**) and the slope.
- **DRIVE** and **OUT:** saturation, output gain and the dry and wet mix.
- **MODULATION:** an envelope follower and a synced LFO on the cutoff, and the smoothing of the
  lanes (short smoothing gives hard gates, long smoothing soft swells).

## Lanes

Pick a lane with **Volume**, **Cutoff** or **Pan** and drag across the steps to draw it.

- **Length** sets how long 16 steps last (1/4 bar to 4 bars) for lanes whose step size is Auto.
- **Curve** bends the line between steps (0: hard steps).
- **Steps** is the lane's own length, 1 to 64 steps. Lanes of different lengths run against each
  other (a 3-step volume lane against a 16-step cutoff lane, and so on). The view shows 16, 32, 48
  or 64 steps; the steps a lane skips are shaded.
- **Step** is the lane's step size: Auto (Length over 16 steps), or 1/32, 1/16, 1/8, 1/4, 1/2 and
  1 bar, each also as a triplet (T).
- **Swing** delays every second step of the lane: at 100% it starts three quarters of the way
  through the pair of steps.

### Level, Chance and Repeats

The buttons under the lane choose what you draw:

- **Level:** the step's level (volume, cutoff or pan).
- **Chance:** how likely the step is to play, 0 to 100%. A step that does not play leaves the gate
  shut (volume), the filter open (cutoff) or the sound in the centre (pan). The dice are fixed by the
  step's place in the song, so playing a passage again or exporting it always gives the same result,
  while each pass of the lane rolls its own. In the Level view a step with a chance below 100% is
  fainter and has a dot over it.
- **Repeats:** 1 to 4 hits in the step. Each hit holds the step's level for its first half and
  rests for the second half, which gives ratchets and rolls. In the Level view the hits show as
  splits in the step.

## Patterns

The Shaper holds 8 patterns, each with its own steps, chances and repeats in every lane (the lane
lengths, step sizes and swing are shared). The **PATTERNS** panel picks the pattern shown and
played; **Copy**, **Paste** and **Clear** work on the whole pattern.

Which pattern plays:

- **The Pattern control:** the buttons 1 to 8 set the **Pattern** parameter, which you can automate
  or link to a macro or controller like any other.
- **The chain:** turn **Chain** on and click the 16 slots to fill them with patterns (or roll the
  wheel over a slot). Each slot plays for one Length, in order, and the chain loops. An empty slot
  plays the chosen pattern.
- **Notes:** under **PATTERN NOTES** choose **MIDI Out ports** (notes from MIDI Out channels, on one
  **Port** or any) or **MIDI input** (your controller). The note set in **From** picks pattern 1 and the
  seven notes above it pick patterns 2 to 8, from the note's own sample. A note's pattern stays until
  another note, a change of the Pattern control or a jump of the song position.

Projects made before these controls play exactly as before: Auto step size, no swing, every chance
at 100%, one hit per step and pattern 1.
