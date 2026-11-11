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
