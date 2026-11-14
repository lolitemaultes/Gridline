# Chater (`gl.chater`)

Chater is a dreamy glitch, glitter and sparkle effect. It listens to the sound on its track, keeps
the last few bars in a rolling capture and, now and then, breaks gently into it: a soft stutter, a
reverse swell, a tape that sighs to a stop, a scrub, a jump back or a skipped beat. With soft edges
and Wash those breaks blur into the space instead of cutting. Around them it sprinkles fine pitched
grains, a shimmer tail, ringing crystal resonators and a lifted halo on the highs, then hazes,
damps and wobbles the sound like an old tape and sends it into a long, soft hall. Turn the Glitch
tile up (or pick a Glitch preset) and the breaks come back sharp, fast and digital. It is in the
plugin picker under Delay & reverb, next to Cloister (search for "glitch", "dreamy", "tape",
"shimmer" or "sparkle").

Chater is an effect: it does not sequence and it does not play from notes. Breaks fire by chance
on the song's tempo grid, on the hits in the input, or both. Every chance is worked out from the
**Seed** and the position in the song, so a section plays the same breaks every time you play it,
and a render or bounce sounds exactly like playback. The tape wobble, hiss and crackle follow the
song position too.

## The main page

- **Glitch tape** (top) shows the last four bars of the input. Each break is drawn over the part
  of the audio it replaced, in its own colour and with its name (a Repeat also shows how many
  slices it played). The lane under the waveform has one cell per beat: the number is the chance
  that a break fires in that beat, a `?` marks a beat where one may fire and a letter shows the
  break that did. The lane is read-only; Density, Grid and Lean change it.
- **Eight macro tiles**: turn the big knob on a tile, or click the chevron to open its page.

  | Macro | What it turns up |
  |---|---|
  | Sparkle | grain density and level, how far grains reach in pitch, shimmer feedback, halo twinkle |
  | Haze | Damp (down toward 2.5 kHz), warmth, soft break edges, Wash, room damping |
  | Memory | wow, flutter, tape age and ensemble, and above half a little hiss |
  | Drift | gentle breaks: reverse and tape odds, a falling pitch ramp, scrub, grain detune and wobble |
  | Shine | crystal ring and decay, halo lift and twinkle, a brighter tilt |
  | Glitch | the sharp side: more breaks, repeats, jumps and skips, finer cuts, gate, bits and rate; it also takes the soft edges and the wash away |
  | Space | room mix, decay and size, room shimmer above half, echo mix, feedback and diffusion, width and spray |
  | Mix | the dry and wet balance |

  Right-click a tile knob (or any knob) for its menu: type a value, reset, copy and paste, lock it
  from the dice, roll only that knob, link it to a modulation source and, for a macro, **What it
  drives**, which lists every control the macro moves and the range it moves it through.
- **Break odds**: when a break fires, these six faders set how likely each kind is.
- **Quick**: hold the capture (the breaks loop the held audio), freeze the space (the room holds
  its tail), let the input's hits fire breaks, pick a scale for the grains and change where in the
  bar the chance leans.

Every page and sheet has a close cross in its top right corner; Escape also steps back one level,
and the editor always opens on the main page.

## How breaks fire

- **Trigger** (Fracture page): **Grid** fires by chance on the tempo grid, **Hits** fires on the
  input's own hits (an onset detector) and **Both** does both.
- **Grid** sets the step (1/2 to 1/32, with triplets). **Density** is the chance on each step; the
  Drift and Glitch macros add to it. **Lean** moves the chance in the bar: evenly, toward
  downbeats, toward offbeats or toward the end of the bar. **Span** sets how many steps a break
  lasts and **Variation** how much each break is varied (division, direction, length).
- **Hit sensitivity**, **Hit chance** and, in Settings, **Hit gap** (the shortest time between two
  hits) tune the hit detector.
- **Seed** (Settings) picks a different, repeatable set of breaks. **Restart breaks on play**
  starts the pattern from the top each time the transport starts.

## Pages

- **Haze** (Haze and Memory tiles):
  - **Damp**: a gentle high cut on the wet sound (off at 20 kHz) and **Warmth**, a soft saturation.
  - **Soft breaks**: **Soft edges** rounds every break (longer crossfades, rounded repeat slices
    that fade and darken with each repeat, tapes that darken as they slow, reverses that swell
    in). **Wash** lets a break replace less of the beat and sends it into the room and the echo,
    so it smears into the space.
  - **Wow and flutter**: a slow pitch wobble with a slower random drift, its rate, a fast
    flutter and **Ensemble**, two detuned voices that widen the sound.
  - **Tape**: **Tape age** rolls off lows and highs and squashes the loud parts a little;
    **Hiss** and **Crackle** are quiet and only play while music does.
- **Fracture**: the capture (length, quantise, smoothing of the cut edges, hold), the break odds as
  rows, Repeat (division, acceleration, pitch ramp, gate), Tape and scrub (stop time, curve, scrub
  depth and rate) and Triggers.
- **Glitter**: the grain cloud (density, size, scatter, reverse, level and a window: Soft, Bell,
  Sharp, Click or Swell, which fades in slowly), the intervals a grain may take with reach and
  detune, a scale and root the grains snap to, and Shimmer (feedback up +7, +12 or +19
  semitones, tone and spread).
- **Colour**: Crystal (six tuned resonators in octaves, fifths, a triad, a sus or a ninth chord),
  Halo (lifts and twinkles the highs above a frequency, with smear and freeze) and Grit (bits,
  rate, raw, soft or clean conversion, jitter and drive).
- **Space**: Spray (width, pan scatter, micro delay and a slow orbit); Room, a long diffuse hall
  (size, decay up to 20 seconds, damping, **Shimmer** an octave and a fifth up inside the tail,
  **Mod** for a slow chorus in the tail, and **Freeze**, which holds the tail for as long as it is
  on); a tempo echo with ping-pong and **Diffuse**, which blurs each repeat a little more; and the
  last stage: low and high cut, tilt, duck, dry/wet, output and a safety limiter.
- **Motion** (top bar): two tempo sways (sine, triangle, ramp, steps or drift), an input follower
  and up to eight links from a sway, the follower or the breaks themselves to any continuous
  control.

## Presets and the dice

**Presets** opens 40 factory sounds in eight banks, one per kind of source, with search,
favourites and your own presets (**Save preset**). The arrows next to the preset name step
through them. **Init** is the plain dreamy starting point. Every preset was checked on a pad, a
voice, a piano, a plucked string and a drum loop.

**Pads**
- **Cloud Bed**: soft haze and a long hall under a slow pad.
- **Velvet Fog**: dark, damp warmth with gentle tape wow.
- **Polar Glow**: octave shimmer rising out of a long hall.
- **Slow Bloom**: reverse swells that bloom into the room.
- **Faded Choir**: worn ensemble voices and a quiet crystal ring.

**Vocals**
- **Whisper Halo**: airy halo and soft echoes around a voice.
- **Dream Double**: a wobbly tape double with a wide ensemble.
- **Angel Trail**: fifth and octave shimmer trailing each phrase.
- **Lullaby Tape**: warm worn tape with slow reverse swells.
- **Echo Diary**: diffuse ping-pong echoes that fade softly.

**Keys and piano**
- **Music Box Memory**: sparkling grains and a worn tape memory.
- **Felt Haze**: muted, warm and close, with gentle wow.
- **Glass Lullaby**: crystal rings in fifths over soft keys.
- **Old Upright**: tape age, flutter and a small dusty room.
- **Starlit Keys**: twinkling halo and glitter above each chord.

**Guitars and plucks**
- **Lake Shimmer**: plucks ripple into a shimmering hall.
- **Harp Mist**: soft repeats washed into a misty room.
- **Worn Strings**: old tape, flutter and a dark high cut.
- **Pluck Comets**: sparkle trails flying off each note.
- **Tremolo Dust**: a slow sway on the haze, fine glitter on top.

**Drums and loops**
- **Soft Shatter**: gentle repeats with soft edges on the beat.
- **Dusty Breaks**: worn tape, crackle and soft tape stops.
- **Tape Swell Loop**: reverse swells and slowdowns into a room.
- **Fairy Stutter**: light stutters that sparkle and wash away.
- **Moon Groove**: damp, wide and spacious, with rare soft breaks.

**Ambient and textures**
- **Frozen Lake**: a very long hall with drifting shimmer.
- **Aurora Field**: swaying halo and wide spray, no glitch.
- **Deep Nebula**: a vast, dark hall with octave shimmer.
- **Glass Rain**: pitched glitter falling through a long room.
- **Northern Hum**: ensemble and wow on a slow, dark wash.

**Lo-fi memories**
- **Cassette Sunday**: wow, flutter, hiss and a soft high cut.
- **Faded Polaroid**: washed-out tape tone and gentle breaks.
- **Attic Radio**: narrow, crackling and far away.
- **Summer Reel**: warm wow and slow tape stops at bar ends.
- **Home Video**: flutter, hiss and reverse swells.

**Glitch** (the sharp side)
- **Broken Clock**: repeats that speed up into each downbeat.
- **Shredder**: fine divisions, jumps and skips at high odds.
- **Signal Loss**: skips and crushed repeats, a dropped stream.
- **Arcade Sparks**: square-edged repeats at 8 bits.
- **Hiccup**: short stutters on the off-beats.

**Roll** opens the dice: pick how far a roll goes (Nudge, Shake or New sound) and what it may
change (macros, breaks, glitter, colour, space, motion). Locked knobs stay put; lock a macro with
the padlock on its tile, or any knob from its menu (or press L over it). The history steps back
and forward through earlier rolls. With **Dice keeps the vibe** on, rolls stay near the sound
of the current bank.

## Settings

- **Quality**: Light, Normal or High (High also oversamples Grit).
- **Halo engine**: Off, Low (about 12 ms of latency) or Full (about 23 ms, finer detail). Gridline
  compensates the latency automatically.
- **Capture up to**: how much audio the rolling capture keeps (2, 4 or 8 bars).
- **Tempo**: follow the song, or run free at its own BPM.
- **Visuals** (Full, Calm, Still) and **Knob drag** (circular, or up and down) apply to every
  Chater window.

## Notes

- With Mix at 0 % and no break firing, the sound passes through unchanged (delayed by the Halo
  latency, which the mixer compensates). Haze, Memory and the space only colour the wet sound.
- The dry and wet signals are mixed linearly, because breaks replace the beat rather than sit on
  top of it.
- Projects saved with the first version of Chater open with their old sound: the new Haze and
  Memory tiles start at 0, and the edges stay hard.
- The audio thread never allocates or locks; all buffers are fixed when the plugin is created.
  Silent stretches cost almost nothing.
