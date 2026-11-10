# Control voltage and gate

Gridline can talk to modular synths and other gear that runs on control voltages (CV): pitch,
gate, clock and modulation voltages travel over ordinary audio cables between the gear and your
audio interface.

- **Voltage Out** turns the notes of a channel (and any Gridline controller) into voltages on
  the interface's outputs: pitch, gate, velocity, clock, trigger, run, reset or a linkable value.
- **Voltage In** reads voltages from the interface's inputs and turns them into notes (pitch and
  gate) and into eight controller outputs that any parameter can follow, hosted plugins included.
- **Voltage calibration** checks the cables and the interface and tunes both directions.

## What you need: a DC-coupled interface

A gate or a held pitch is a steady voltage. Most audio interfaces block steady voltages on purpose
(they are *AC-coupled*): a held voltage fades to 0 V within a fraction of a second, so gates stop
and pitches drift back to the middle. CV needs a **DC-coupled** interface (or DC-coupled outputs
and inputs of a module made for this). The interface's manual says whether its outputs and inputs
are DC-coupled; the coupling check in the calibration window tells you for sure.

Other limits worth knowing:

| Limit | What it means |
|---|---|
| Voltage swing | An interface reaches only its own full scale. A line output may give about +/-2 V, which is only four octaves of 1 V/oct pitch. Set **Interface full scale** to the volts your interface gives at full digital level (the round trip and a meter tell you). |
| Latency | Voltages in and out are as late as the audio: the input buffer, the jitter buffer and the output buffer. Voltage In's notes play one audio block after the input, timed to the sample within it. |
| Modulation rate | Controller outputs drive parameters every 64 samples, with the link's smoothing. Audio-rate CV (FM, for example) is not supported. |
| Export | An export has no device input: Voltage In rests while Gridline renders. Voltage Out renders its voltages like any channel. |
| Speakers | Steady voltages can damage speakers. Never patch a CV output into a speaker or an amplifier. |
| System audio processing | Turn off any enhancement, loudness or effect processing the operating system adds to the device. |

## Voltage Out

Add **Voltage Out** from the plugin browser (Controller) to a channel and turn it on. Each side
(left and right of its output) has a source:

| Source | Voltage |
|---|---|
| Pitch | 1 V per octave (or 1.2 V), C4 at 0 V, with glide |
| Gate | High while a note is held |
| Velocity | The note's velocity across the range |
| Clock | Pulses at the chosen PPQ while playing |
| Trigger | A short pulse at each note |
| Value | The side's **Value** parameter across the range: right-click it and link any Gridline controller to it to drive the gear from an LFO, an envelope or Voltage In |
| Run | High while the transport plays |
| Reset | A pulse when playback starts or jumps |

The **Interface** section sets where the voltages go and how they are scaled:

- **Through the mixer** (the default): the voltages pass the channel's volume and pan, its mixer
  track and the master. Keep them at unity (0 dB, centred, no master effects or limiter) or the
  voltages change.
- **Direct to outputs**: the voltages skip the channel strip and the mixer and go straight to an
  extra output pair of the interface, untouched. This needs the ASIO driver on Windows: open
  **Settings > Audio**, add the pair under **Extra outputs** (any mixer track; Voltage Out adds to
  what that track sends there), then pick it as **Output pair**. On other systems direct mode
  stays on the mixer path for now, and the editor says so.
- **Interface full scale**: the volts at full digital level (10 V by default).
- **Trim** (mV) and **Fine** (%) per side, after the offset and scale wheels.
- **Use calibration**: applies this computer's calibration of the output (see below).

## Voltage In

Add **Voltage In** to a channel. It makes no sound. Open an input device first: **Settings > Audio
> Input & recording** (or the ASIO inputs on Windows). The inputs are numbered In 1, In 2, ... as
in the mixer's input selector.

- **Pitch** and **Gate** inputs play notes: the gate's rising edge starts a note at the pitch of
  that sample, the falling edge ends it. **Play into** picks the channel that plays them (it sets
  the MIDI ports of both channels; the channel settings show them too).
- **Gate threshold** and **hysteresis**: the gate opens at threshold + half the hysteresis and closes
  below threshold - half, so a noisy edge gives one note, not a burst.
- **Gate mode**: Gate (notes as long as the gate) or Trigger (notes of a fixed length).
- **Pitch**: 1 V/oct or 1.2 V/oct, the note at 0 V, and **Snap**: off (the exact pitch, played with
  its fraction of a semitone), to semitones, or to a scale and root.
- **Glide** slews the pitch; while the gate is held a new pitch glides the note, or starts a new note
  with **New note on pitch change** on. Instruments that cannot glide get pitch bend (hosted plugins)
  or a new note (built-in instruments).
- **Velocity**: fixed, or from CV A to D.
- **CV A to D**: four more inputs, each with its range (+/-5 V, +/-10 V, 0 to 10 V), trim and fine
  scale. Their outputs go from 0 at the bottom of the range to 1 at the top.

Every input's live voltage shows under its column.

### CV to any parameter

Right-click any knob (an instrument, an effect, a mixer control or a hosted plugin's parameter) and
choose **Link to controller**, then one of Voltage In's outputs: Pitch, Gate, CV A to D, Clock or
Velocity. The link's range, curve and smoothing work as for every controller.

### Clock in

Pick the **Clock** input and its pulses per quarter note, and optionally a **Run** input (high =
running). **Follow** sets what the clock drives: **Tempo** follows the clock's tempo, **+Run** also
starts the song from the beginning when Run goes high (or the clock starts) and stops it when Run
goes low (or the clock stops for half a second). The editor shows the tempo it reads.

## Voltage calibration

Open it with **Calibrate** in either editor. Pick a Voltage Out channel and side, a Voltage In
channel and a device input, and confirm the output goes to CV gear or back into an input, never to
speakers. Each check runs only when you press **Run**.

1. **Coupling check**: patch the output into the input. The output holds 5 V for 2.5 s while
   Gridline watches the input. If the voltage fades, the path is AC-coupled and gates and held
   pitches will not work on it. It also shows the input's offset and noise.
2. **Round trip**: patch the output into the input. Gridline steps from -2 V to +5 V and fits a
   line through what comes back. **Apply** stores it for that input, so Voltage In reads what
   Voltage Out sends.
3. **Tuner (V/oct)**: patch the output into an oscillator's pitch input and the oscillator's audio
   into the chosen input. Gridline sends 0, 1, 2 and 3 octaves, measures the pitch of each and shows
   how many cents each step is off, now and after calibration. **Apply** stores the correction for
   that output.

Calibration belongs to this computer: it is kept per audio device and channel in
`cv_calibration.json` in Gridline's settings folder. It is never saved in a project and never sent
to collaborators. The project keeps only each plugin's own trims, so a project opened on another
computer uses that computer's calibration. **Use calibration** in each editor turns it off for one
instance; **Forget** in the window deletes an entry.

## Safety

- Never patch CV outputs into speakers, monitors or amplifiers.
- Voltage Out starts switched off; a new instance sends nothing until you turn it on.
- The calibration window sends steady voltages: check the patching before pressing **Run**.
