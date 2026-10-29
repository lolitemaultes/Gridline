# Tideline (`gl.tideline`)

Tideline is a one-knob rhythmic volume shaper. It pulls the volume down and lets it swell back in
time with the song, which gives you the pumping sound of sidechain compression without routing a
kick into a compressor. You can also trigger it from a kick on another track or from MIDI notes.
It is in the plugin picker under Dynamics (search for "sidechain", "pump" or "ducking").

Tideline adds no latency (unless you turn on the Audio trigger's look-ahead), runs per sample (the
result doesn't depend on the buffer size) and smooths every gain change, so it doesn't click.

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
- **Audio** starts the curve whenever the detector hears a hit. Pick what it listens to in the
  **Source** list under the knobs:
  - a **mixer track** (for example "Kick"): Tideline adds a sidechain route from that track into
    its own track. The route is an ordinary mixer route, so you see it in the mixer and can remove
    it there or with **Remove** in the same list.
  - a **channel**, including an audio clip channel: Tideline uses the channel's mixer track. A
    channel that plays into the master, or into Tideline's own track, first gets a mixer track
    of its own (a kick should not be ducked by itself).
  - **Own input**: the detector hears the sound Tideline is ducking.
  If the track already sends its audio into Tideline's track, that send is used as it is. Routes
  follow the tracks, not their numbers, so renaming or reordering tracks keeps them. With no
  route at all, the detector hears Tideline's own input, and the list says so.
  The detector's controls are:
  - **Threshold** sets the trigger level.
  - **Gap** is the shortest time between two triggers.
  - **Low cut** and **High cut** set the detector's band. The default is 30 to 200 Hz, which
    finds kicks.
  - **Look-ahead** (0 to 10 ms, default 0) delays the ducked sound so the duck starts right on
    the hit instead of a moment after it. Gridline's delay compensation keeps the rest of the
    project in time. At 0, Tideline adds no latency.
- **MIDI** starts the curve on notes, exactly on their sample. Pick the notes in the **Source**
  list:
  - a **channel**: every note it plays (steps, piano roll, pattern clips in the playlist, notes
    you play live on it, a note processor's output) and every start of its audio clips. Clip
    starts trigger exactly on the grid with no detection, which is handy for a kick sample placed
    as audio clips. A muted channel still triggers, so a silent "ghost kick" channel works.
  - **MIDI input (controllers)**: every note that comes in from your MIDI controller. In the plugin
    builds (VST3, CLAP, AU) this is the host's MIDI input.
  - **Any MIDI Out port** or **One MIDI Out port**: notes MIDI Out channels play on a port; the
    **Port** knob picks the port.
  The **Note** and **To** knobs filter the notes: Note -1 takes any note, Note alone takes one
  note, Note and To take the range between them. Audio clip starts always count.
  The picked channel is stored with the project and follows the channel when you rename or move it.
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

## Stand-in for a missing Kickstart

Installed plugins are always hosted as themselves. When a project (a Gridline project or an FL
Studio project) uses Kickstart or Kickstart 2 and the plugin is not installed, Tideline plays it
instead and the original plugin with its saved state stays in the project untouched; once the
plugin is installed, the project opens with it again. The plugin window shows a short note with
**Replace with Tideline** to keep Tideline for good.

What is read from Kickstart 2's saved state:

- the loop length (1/8, 1/4, 1/2 or a bar) becomes the Rate;
- the selected curve is redrawn with Tideline's eight custom points (Shape: Custom). This is an
  approximation: Kickstart's curve segments are not reproduced exactly;
- Mix becomes Amount; the band switch and solo become Band (Lows) and Listen, with the band
  frequency on Tideline's 20 Hz to 5.12 kHz scale (approximate);
- the one-shot end of the curve becomes One-shot end; trigger smoothing becomes 1 ms Smoothing;
- MIDI triggering listens to any note.

Automation and controller links of Loop Length, Band On, Band Frequency and Mix follow to Rate,
Band, Split and Amount; the curve selection has no single counterpart and its automation is kept
aside. Kickstart 1 is read the same way when its state has the same layout, otherwise only its
parameters are used and Tideline's Pump shape plays.

## FL Studio export

When you export to an FL Studio project, Tideline becomes a Gross Beat volume envelope. The
envelope is the tempo-synced curve with Amount, Duration and Shift applied, spread over a
bar-locked slot. Audio and MIDI triggers, the band split and Mid / Side do not carry over, and
the export notes say so.
