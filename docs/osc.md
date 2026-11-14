# OSC (Open Sound Control)

Gridline can receive OSC messages and send feedback back, so a touch surface on a phone or tablet,
another music program, a modular patch or a lighting desk can play and mix a project.

## Switching it on

**Settings > MIDI > OSC (Open Sound Control)**:

| Setting | What it does |
|---|---|
| Receive OSC messages | Starts the OSC server. Off by default. |
| Port | The port Gridline listens on. 9000 by default; 0 picks any free port. |
| Accept messages from other devices on the network | Off: only programs on this computer can send (Gridline listens on 127.0.0.1). On: Gridline listens on every network and phones, tablets and other computers can send too. |
| Allowed hosts | With the network on: the IPv4 addresses or ranges (`192.168.1.20`, `10.0.0.0/8`) allowed to send, separated by commas. Empty: any host. This computer is always allowed. Packets from other hosts are refused and counted. |
| Also accept OSC over TCP | For senders that use TCP, on the same port. Each connection is framed with SLIP (OSC 1.1) or with a 32-bit length before each packet (OSC 1.0); Gridline tells them apart by the first byte. |

The section shows whether the server runs (or why it does not, such as a port in use by another
program), how many messages arrived and how many were malformed, refused or dropped, and the last
messages with what Gridline did with them. **Test** sends a message to Gridline's own port and
tells you whether it arrived.

Your firewall may ask whether Gridline can accept connections the first time you switch the
network on.

## Feedback

**Send the transport and linked values back** sends messages to a host and port of your choice
(the surface's receive port), or, with port 0, to the address and port the last message came
from. Gridline sends:

- `/transport/play`, `/transport/record` (1 or 0) and `/transport/tempo` (BPM) when they change;
- `/transport/position/bars`, `/transport/position/beats` and `/transport/position/seconds`, at most
  10 times a second while the position moves;
- `/pattern/current` (the pattern number) when it changes;
- the position of every OSC link's control when its parameter changes in Gridline (the range,
  curve and invert of the link are undone, so the surface's fader lands where it would have to be);
- the value of every fixed address (below) the surface has sent before, when it changes.

Values are sent at most 30 times a second, and nothing is sent while nothing changes. A value the
surface has just set is not sent back to it. `/gridline/ping` is answered with `/gridline/pong`.

## The address space

Numbers count the way Gridline shows them: mixer track 0 is the master and 1 onwards are the
inserts; channels, patterns, markers, effect slots and plugin parameters count from 1.

Values: continuous controls take the control's position, 0 to 1 (a float or an integer; true is 1
and false is 0). On and off addresses take 1 or 0, and toggle when the message has no value.
Buttons (stop, next, toggles) act on a press: 1, true, an impulse or no value; 0 (the release of a
push button) does nothing.

Address patterns work as OSC 1.0 describes them: `/mixer/[1-4]/mute 1` mutes inserts 1 to 4 and
`/mixer/*/volume 0.8` sets every track.

### Transport

| Address | Arguments | |
|---|---|---|
| `/transport/play` | none or 1; 0 | Plays; 0 pauses. |
| `/transport/playpause` | press | Plays, or pauses while playing. |
| `/transport/pause` | press | Pauses where it is. |
| `/transport/stop` | press | Stops (as the Stop button). |
| `/transport/record` | 1 or 0; none toggles | Arms or disarms recording. |
| `/transport/record/toggle` | press | Flips record on a press. |
| `/transport/metronome` | press | Switches the metronome. |
| `/transport/tempo` | BPM | 10 to 522. |
| `/transport/position` or `/transport/position/bars` | bar | Bar 1 is the start; 2.5 is the middle of bar 2. Follows meter changes. |
| `/transport/position/beats` | beats | Quarter notes from the start. |
| `/transport/position/seconds` | seconds | At the song tempo. |
| `/transport/loop` | first bar, last bar | Loops the song from the start of the first bar to the end of the last one (the playlist's time selection). |
| `/transport/loop/off` or `/transport/loop 0` | | Clears the loop range. |
| `/transport/mode` | `"song"` or `"pattern"`, or 1 / 0; none toggles | Song or pattern mode. |

### Mixer

| Address | Arguments | |
|---|---|---|
| `/mixer/<n>/volume` | 0..1 | Fader position (1 is the top of the fader). `master` may stand for 0. |
| `/mixer/<n>/pan` | 0..1 | 0.5 is the centre. |
| `/mixer/<n>/stereo` | 0..1 | Stereo separation, 0.5 is the default. |
| `/mixer/<n>/mute` | 1 or 0; none toggles | |
| `/mixer/<n>/mute/toggle` | press | |
| `/mixer/<n>/solo` | 1 or 0; none toggles | |
| `/mixer/<n>/solo/toggle` | press | |
| `/mixer/<n>/send/<to>` | 0..1 | Send level from track n to track `to`; the send is made when there is none. |
| `/mixer/<n>/slot/<s>/bypass` | 1 or 0; none toggles | 1 bypasses effect slot s (its switch off). |
| `/mixer/<n>/slot/<s>/enabled` | 1 or 0 | The slot's switch itself. |
| `/mixer/<n>/slot/<s>/mix` | 0..1 | The slot's dry / wet mix. |

### Channels

| Address | Arguments | |
|---|---|---|
| `/channel/<n>/volume` | 0..1 | |
| `/channel/<n>/pan` | 0..1 | 0.5 is the centre. |
| `/channel/<n>/pitch` | 0..1 | 0.5 is no change (the range is two octaves either way). |
| `/channel/<n>/mute` | 1 or 0; none toggles | |
| `/channel/<n>/mute/toggle` | press | |
| `/channel/<n>/note` | pitch, velocity | Plays a note: pitch 0..127 (60 is C5); velocity 1..127 as an integer, or 0..1 as a float; 0 releases it. Without a velocity, 100. |
| `/channel/<n>/noteoff` | pitch | Releases a note. |

`selected` may stand for `<n>`: `/channel/selected/note 60 100` plays the selected channel.
Notes OSC still holds when the server stops are released. Notes record like notes from a MIDI
keyboard.

### Plugin parameters

| Address | Arguments | |
|---|---|---|
| `/param/mixer/<n>/<slot>/<param>` | 0..1 | A parameter of the effect in slot `<slot>` (1..10) of mixer track n. |
| `/param/channel/<n>/<slot>/<param>` | 0..1 | Slot 0 (or `instrument`) is the channel's instrument; 1..8 are its note effects. |

`<param>` is the parameter's number (from 1, in the order the plugin lists them) or its name.
Names ignore case, spaces and punctuation: `/param/channel/2/0/cutoff_freq` finds "Cutoff freq".

### Patterns and markers

| Address | Arguments | |
|---|---|---|
| `/pattern/select` | number, or name | Selects a pattern. |
| `/pattern/next`, `/pattern/previous` | press | |
| `/marker/jump` | number, or name | Jumps to a song marker (switching to song mode). Markers count in song order; meter and key markers are left out. |
| `/marker/next`, `/marker/previous` | press | The marker after or before the position. |

### Stage

`/stage/...` is kept for the Stage view: messages there are accepted and shown as "reserved" in
the activity list, and do nothing yet.

### Gridline

`/gridline/ping` is answered with `/gridline/pong` when feedback is on. Other `/gridline/...`
messages are accepted and ignored (the Test button uses them).

## Linking any control

Right-click a knob, fader or transport button and choose **Link to controller...**, then move the
control on your surface: the first address that arrives is learned (the dialog's Source becomes
**OSC address**). You can also pick **OSC address** as the source and type the address, or a
pattern such as `/1/fader*`. The link uses the first number of each message and has the same
options as a MIDI link: range, curve or formula, invert, pick up or scale takeover, relative steps
(the value is then a signed number of steps) and button modes (hold, toggle, step up). Links are
saved with the project and listed in **Settings > MIDI > Controller links**; **Multilink to
controllers** takes OSC addresses too.

A message whose address is linked runs the link instead of the fixed address space.

## Bundles and time tags

Bundles are taken apart, nested ones too. A bundle timed for later waits until its time (on the
computer's clock); "immediately" and past times act at once. A packet holds at most 1024 messages
and at most 8 levels of bundles; larger or malformed packets are refused and counted.

## Supported types

`i` `f` `s` `b` (OSC 1.0) and `h` `d` `t` `T` `F` `N` `I` `S` `c` `r` `m`, and OSC 1.1 arrays (their
items are read as plain arguments). Numbers of any of the numeric types work wherever a value is
expected; a string that holds a number does too.

## Notes

- Changes made over OSC are undoable; a run of changes to one control without a pause of 0.4 s
  is one undo step.
- Messages are applied on Gridline's interface thread, the same way as MIDI controller links, and
  reach the audio engine on its usual path; nothing is done on the audio thread.
- OSC listens on IPv4. Use `127.0.0.1` rather than `localhost` on systems where `localhost` means
  the IPv6 address.
