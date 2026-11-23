# Novation FLkey keyboards

Gridline drives every Novation FLkey keyboard as a full controller: the pads step-sequence the
selected channel, the knobs and faders move plugins and the mixer, the buttons run the transport,
and the screen shows what you move.

| Model | Pads | Knobs | Faders | Screen |
| --- | --- | --- | --- | --- |
| FLkey Mini | 16 | 8 pots | none | none |
| FLkey 37 | 16 | 8 pots | none | LCD |
| FLkey 49, FLkey 61 | 16 | 8 pots | 8 and the master, 8 fader buttons, Solo | LCD |
| FLkey 2 Mini 25 | 16 | 8 encoders | none | OLED |
| FLkey 2 37 | 16 | 8 encoders | none | OLED |
| FLkey 2 49, FLkey 2 61 | 16 | 8 encoders | 8 and the master, 8 fader buttons, Solo | OLED |

## Setting up

Plug the keyboard in. Each FLkey shows two ports: its **MIDI** port (the keys, wheels and pedal)
and its **DAW** port (pads, knobs, faders and buttons). Gridline finds the DAW port by its name
and takes it over on its own; the MIDI port plays notes like any keyboard.

If Gridline does not find it (a renamed port, or a name without the model size), open
**Settings > MIDI**, select the keyboard's **DAW** input and pick its model under
**Keyboards with a DAW port** in the controller type list. Turn the input on.

When Gridline quits, or the controller type is set to something else, the keyboard goes back to
working on its own.

Several FLkeys can be used at once; each one has its own pages and banks. Unplugging and plugging
in again, or **Rescan devices**, is picked up while Gridline runs.

## Pads

Hold **Shift** and press **Page ◄ ►** to switch the pad page (on the first FLkey Mini, which has no
Page buttons: **Shift** and **Channel Rack ▲ ▼**). The screen shows the page name.

```
 Steps page (the selected channel, Kick, 16 steps; step 7 playing)

  [ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ][ 8 ]     lit: the channel's colour
  [ 9 ][10 ][11 ][12 ][13 ][14 ][15 ][16 ]     dim: empty   white: playing
```

- **Steps**: the selected channel's steps in the current pattern, in the channel's colour, the
  playing step white. Tap an empty pad to set its step, tap a lit pad to clear it. **Page ◄ ►**
  shows the next or previous 16 steps of a longer pattern. Hold one or more step pads and turn the
  knobs to edit those steps: pitch, velocity, release, fine pitch, pan, mod X, mod Y and shift
  (knobs 1 to 8). Each edit is one undo step.
- **Channels**: the bottom row shows eight channels of the rack in their colours, the selected one
  white. A pad selects its channel and plays it. **Page ◄ ►** moves by eight channels.
  **Channel Rack ▲ ▼** moves the channels shown by eight on the first FLkey, and by one on the FLkey 2;
  the selected channel stays.
- **Patterns**: the song's patterns in their colours, the current one white. A pad selects its
  pattern; the dim pad after the last one adds a new pattern. **Channel Rack ▲ ▼** shows the
  previous or next eight patterns. On the FLkey 2, **Shift and Page ▼** clones the selected pattern.
- **Instrument**: the pads play the selected channel, chromatic from C5 on the bottom left, or the
  song's key over two rows when the song has a key. **Page ◄ ►** changes the octave.

When the keyboard's own pad modes are chosen (its drum or chord pads, custom modes), the pads
belong to the keyboard and Gridline leaves them alone.

## Knobs

The knob mode is chosen on the keyboard (Shift and the pads, as its guide shows); Gridline follows
it.

| Knob mode on the keyboard | In Gridline |
| --- | --- |
| Plugin (FLkey 2: Plugin) | the focused plugin's parameters, eight at a time; after the last bank, the channel's eight macro knobs |
| Volume / Pan (FLkey 2: Mixer) | volume or pan of eight mixer inserts |
| Channel volume / pan | volume or pan of eight channels of the rack |
| Transport (FLkey 2) | knob 1 moves the play position, knob 5 jumps between markers, knob 8 sets the tempo (Shift: fine) |
| Custom | left to the keyboard |

**Presets.** On the first FLkey 37, 49 and 61, **Preset ▲ ▼** load the previous or next preset of
the selected channel's instrument. On the FLkey 2, hold **Shift** and press **Channel Rack ▲ ▼** (the
buttons are labelled Preset under them). Either way it works as the arrows in the plugin window do,
for built-in and other plugins alike, and the screen shows the preset's name. Each one is an undo step
and reaches everyone in a collaboration session.

**Knob banks.** The FLkey 2's encoder bank buttons page the encoders through the plugin's parameters,
or switch between volume and pan in the mixer and channel modes. On the first FLkey, **Shift and
Preset ▲ ▼** do the same.

The first FLkey's pots pick the value up: after a page change, a pot does nothing until it passes
the value it controls, so nothing jumps. The FLkey 2's endless encoders are told the value instead.

## Faders (49 and 61)

Faders 1 to 8 set the volume of eight mixer inserts (or eight channels, in the keyboard's channel
fader mode); the ninth sets the master. They pick the value up like the pots. The **fader
buttons** mute their insert and light in its colour while it plays. Press **Solo** and they solo
instead; press **Solo** twice quickly to unmute and unsolo everything.

## Buttons

| Button | In Gridline |
| --- | --- |
| Play, Stop, Record | play or pause, stop, record |
| Loop (FLkey 2) | loop recording |
| Score Log | puts the notes played in the last five minutes into the current pattern |
| Quantise (FLkey 2: Shift and Score Log) | snaps the selected channel's notes to the step grid |
| Metronome | metronome on or off |
| Undo, Redo (FLkey 2: Shift and Undo) | undo, redo |
| Pattern / Song (FLkey 2) | pattern or song mode |
| Channel Rack ▲ ▼ | selects the previous or next channel (Channels and Patterns pages: moves what the pads show) |
| Shift and Channel Rack ▲ ▼ (FLkey 2) | the previous or next preset of the selected instrument |
| Preset ▲ ▼ (first FLkey 37, 49, 61) | the previous or next preset of the selected instrument |
| Mixer ◄ ► | FLkey 2: moves the knobs and faders by eight inserts (with Shift: by one). First FLkey: by eight on the Channels page, by one on the other pages. Selects the insert |

## The screen

Turning a knob or moving a fader shows its name and value. The rest of the time the screen shows
the current pattern, the pad page and the selected channel. When the song has a key, the
keyboard's **Scale** setting follows it (root and scale type), so scale mode on the keys plays in
the song's key.

## Layouts

The keyboard works the same in every layout: Studio, Clips, Smooth and Tracker all have the
channel rack, mixer and patterns it drives.

## Collaboration and undo

Every edit made from the keyboard is one undo step and reaches the other people in a shared
session like a mouse edit.

## When something does not match

Novation documents what each FLkey control is for, but not the MIDI numbers its DAW port uses.
Gridline uses the numbers Novation publishes for its closely related keyboards. If a button does
something else on your keyboard:

1. Open **Settings > Debug**, set the log **Detail** to **Verbose**, and press the button. The log
   shows what the keyboard sent (for example `flkey: FLkey 49 DAW Out in: B0 66 7F`).
2. Put a `controllers/flkey.json` file in Gridline's settings folder with the corrected number,
   for that model (or `"*"` for all of them):

```json
{
  "flkey-49": {
    "buttons": { "metronome": 75, "undo": 76 },
    "knobCc": 21,
    "faderCc": 53
  }
}
```

Button names: `play`, `stop`, `record`, `loop`, `scoreLog`, `quantize`, `metronome`, `undo`,
`redo`, `patternSong`, `shift`, `channelUp`, `channelDown`, `mixerLeft`, `mixerRight`, `pageLeft`,
`pageRight`, `knobBankUp`, `knobBankDown`, `solo`. Other keys: `sysexId`, `padTop`, `padBottom`,
`knobCc`, `faderCc`, `faderButtonCc`, `transportCc`, `modeChannel`, `padModeCc`, `knobModeCc`,
`faderModeCc`, `padDawValue`, `padModes`, `knobModes`, `faderModes`, `scale`. Model names:
`flkey-mini`, `flkey-37`, `flkey-49`, `flkey-61`, `flkey2-mini`, `flkey2-37`, `flkey2-49`,
`flkey2-61`. Restart Gridline (or unplug the keyboard and plug it in again) to load the file.

## Linux and Windows

On Linux (ALSA) and Windows the keyboard's ports are found the same way (on Windows the DAW port
is called MIDIIN2 / MIDIOUT2). These two have not been tried with the hardware yet.
