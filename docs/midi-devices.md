# MIDI devices

Gridline plays and records from MIDI keyboards and controllers, follows an external MIDI clock or
time code, and sends clock and notes to MIDI outputs. Devices are set up in **Settings > MIDI**.

| System  | MIDI service                          |
|---------|---------------------------------------|
| macOS   | CoreMIDI                              |
| Linux   | ALSA (and JACK when it is installed)  |
| Windows | Windows Multimedia                    |
| BSDs    | sndio                                 |

## Input devices

Every input that is found is listed with an **On** switch, a controller type (a template picked by
the device's name, a script, or none), a port number that links can filter on, and **Notes**
(whether the device plays notes or only drives controls). **Rescan devices** looks for devices
plugged in since Gridline started; new devices are switched on with the next free port number.

## Control surfaces: Mackie Control and HUI

Surfaces that speak the Mackie Control protocol (and the many that copy it in their "MC" or "MCU" mode) or HUI drive the mixer and transport. In **Settings > MIDI**, set the surface's input to one of the **Control surfaces** controller types, and enable its output of the same name: Gridline sends the fader positions, LEDs, rings, names, timecode and meters back to it.

| Controller type | What it is |
| --- | --- |
| Mackie Control (main unit) | eight strips, transport, jog wheel, displays |
| Mackie Control extender 1, 2, 3 | eight more strips each, to the right of the main unit (needs the main unit) |
| Mackie Control, one strip | one fader that follows the selected mixer insert |
| HUI | eight strips and transport over the HUI protocol |

- **Faders** set each insert's volume and follow it when it changes in Gridline (a fader you are holding is left alone).
- **Encoders** pan; push one to centre it. The LED ring shows the pan.
- **Rec, Solo, Mute, Select** per strip. Select also selects the insert in the mixer.
- **Bank** buttons move the strips by eight inserts, **Channel** buttons by one.
- **Play, Stop, Record, Rewind and Fast forward** (a bar back or on), **Cycle** (song / pattern mode), **Click** (metronome), **Undo** and the **jog wheel** (moves the play position a beat per step). The main unit's master fader sets the master volume.
- The **scribble strips** show each insert's name and the **timecode display** shows bars, beats and ticks.

Each move is one undo step, and in a shared session it reaches the other people like a mouse edit.

Keyboards whose transport buttons send Mackie notes can use the **Mackie Control transport** controller template instead, which only maps the transport buttons.

## Pedals

**Settings > MIDI > Pedals** sets what the pedals of a keyboard do:

- **Sustain holds notes (CC 64)** is on by default. While the sustain pedal is down, Gridline holds
  the notes you let go of until you lift the pedal, and also sends the pedal to the instrument.
  Gridline's own instruments have no pedal of their own, so this is what makes them sustain.
- Turn it off for instruments that model their own pedal, such as piano plugins. Notes then end
  when you lift the keys, the pedal still reaches the instrument, and the instrument decides how
  long the notes ring.
- **Sostenuto (CC 66)** holds only the notes that are down when you press it.
- **Soft (CC 67)** plays notes softer while it is down, by **Soft velocity**.

These settings belong to this computer, like the other MIDI settings, and are not saved in projects.

### Recording the pedal

When you record notes, the sustain pedal is recorded with them, as the channel's **Sustain pedal**
automation, and each note keeps the length you held its key. On playback the instrument gets the
pedal where you pressed and lifted it, and with **Sustain holds notes** on, Gridline holds the notes
just as it did while you played. With the switch off, the notes end where you lifted the keys and
the instrument hears the pedal. **Sustain pedal** can also be drawn, automated and linked like any
other control of a channel. Stopping playback lifts it.

## MIDI Out to other channels

A MIDI Out channel sends to the MIDI output device and also to every channel in the project that
listens on its **Port**: its notes, its controller knobs and their automation (CC, RPN, NRPN,
pressure and note knobs), program and bank changes, the pitch bend range, and the pitch bend,
pressure, controllers and pedals you play into it from a keyboard.

To make a channel listen, open its channel settings, choose the **Misc** tab and set **MIDI input**
to the same port number. Every instrument and controller channel has this setting, including
Envelope Mod and Keyboard Mod, so a MIDI Out channel's notes can trigger an envelope too. It starts
at **None**, and projects made before it open with every channel set to None.

- What a MIDI Out channel sends reaches its listeners in the same audio block, on the same sample,
  so a plugin plays the notes exactly in time. Knob automation arrives in the block where the
  automation moves the knob.
- Plugins with a MIDI output (arpeggiators, chord tools) use the same port numbers: set the
  plugin's output port in its window's settings page.
- A channel never hears its own MIDI. If ports would send MIDI round in a circle, through one
  channel or several, the link that closes the circle is left out and the listening channel's
  **MIDI input** says so.
- A frozen channel keeps the notes a MIDI Out channel sent it in its render.

## Hardware Insert

The **Hardware Insert** effect (in a mixer insert's effect slots) puts outboard gear in the mix: a compressor, a pedal, a tape machine.

- **Send** is the extra output pair of the audio device the track goes out of (the pairs after the main outputs). With the ASIO driver they are its extra outputs. With the system audio (macOS, and Linux with PulseAudio, PipeWire or ALSA) choose **Settings > Audio > Output channels** 4, 6 or 8: channels 3-4 are pair 1, 5-6 pair 2, 7-8 pair 3. For the return, set **Input channels** to 4 or 8 under Input and recording. **Return** is the input pair it comes back on; **Return mono** takes one input for both sides.
- **Send level**, **Return level** and **Mix** (dry against what comes back).
- **PING** sends a single click out of the send and counts the samples until it comes back on the return. That **round trip** is written into the insert (one undo step) and reported as its latency, so plugin delay compensation holds the rest of the mix back by the same amount and the gear stays in time. **Offset** trims it by hand. Ping again after changing the buffer size or the device.
- **External instruments:** set **Send** to **None** and the insert is an audio return. Put it on the mixer track of a [MIDI Out](#midi-out-to-other-channels) channel that plays a hardware synth, with the synth's audio on the return inputs, and set the round trip by hand (the synth's delay from note to sound). The synth then plays in time with the rest.

Without the outputs or inputs it names, and when exporting (outboard gear cannot run faster than real time), the insert passes the sound on unchanged. In a shared session the send, the return and the round trip belong to each person's own studio and are not shared.

## When MIDI devices are unavailable

Gridline asks the system's MIDI service for its devices in the background, so a service that is
slow, stuck or broken never holds up startup or the interface. If the service does not answer
within a few seconds, or refuses with an error, Gridline carries on without MIDI devices:

- audio, opening and saving projects, playback, recording audio, plugins and the typing keyboard
  (the computer keyboard as a piano) all work as usual;
- **Settings > MIDI** shows that MIDI devices are unavailable, the reason the service gave (or that
  it is not responding), and when Gridline will try again;
- Gridline tries again on its own, waiting a little longer each time (5, 15, 30 seconds, then
  every minute), and **Retry now** asks again straight away;
- as soon as the service answers, your devices open with their saved settings, without a restart.

A stuck MIDI service is usually cleared by restarting the computer. Gridline keeps checking in the
meantime, so you can go on working.

Developers can simulate both cases with the environment variable `GRIDLINE_TEST_MIDI`: `fail`
makes the service refuse, `hang` makes it never answer. The real MIDI service is not asked in
either case.

Hidden and scripted runs (`--hidden`, `--dev-input`, `--screenshot`: tests and tools) open no
MIDI devices at all, inputs or outputs, so a connected keyboard never reaches a test; tests send
MIDI through the dev commands (`tracker midi on|off <pitch>`). Setting `GRIDLINE_TEST_MIDI`
(`1` for the real devices) asks the system MIDI service again in such a run.
