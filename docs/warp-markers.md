# Warp markers

Warp markers pin points of an audio clip's audio to the timeline. Between two markers the audio
stretches evenly so that it spans the distance between them, with its pitch kept. Before the first
marker and after the last one the clip plays at its own speed.

## The Warp tool

Pick the Warp tool in the playlist toolbar or press W.

- **Click** an audio clip to add a marker at that point of its audio. With **New markers snap to
  transients** on (the default, in the clip menu under Warp markers) the marker lands on the
  nearest transient within 40 ms.
- **Drag** a marker to move its audio to another place on the timeline. The sections on both sides
  stretch to fit, and the waveform on both sides follows as you drag. The marker keeps its place
  under the pointer (grabbing it never makes it jump) and lands on the nearest grid line, gliding
  between lines when smooth snapping is on, as a dragged clip does; hold **Alt** to drag off the
  grid. The first drag on a clip pins its start with a marker, so the clip's first sample stays at
  its left edge. While the song plays, the audio follows the marker without restarting; the higher
  quality render is made in the background once you let go and fades in when it is ready.
- **Shift+drag** a marker to keep it where it is and slide the audio under it instead.
- **Double click** or **right click** a marker to remove it. A right click elsewhere deletes clips,
  as with every editing tool.
- **Esc** or a right click during a drag cancels it.

Each section plays between an eighth and eight times as fast as the clip, and two markers stay at
least 2 ms apart. A drag stops at those limits, so markers never cross.

Markers show as faint lines in every tool, so a warped clip is easy to recognise. In the Warp tool
they show as handles, and a section that plays faster or slower is tinted and shows its length
against the original (200% is twice as long).

## Audio quantize

The audio clip menu (right click the clip's title) has **Warp markers**:

- **Add at transients** places a marker on every transient of the clip. Nothing moves yet.
- **Quantize to the grid** places markers on the transients and moves them toward the playlist's
  grid. **Quantize strength** sets how far: 100% lands them on the grid, 50% halfway.
- **Clear warp markers** removes every marker; the clip plays at its own speed again.

## How warped clips play

A warped clip always plays its sections through the time stretch with its pitch kept, whatever the
channel's time mode, and only that clip: other clips on the same channel play as they were. The
clip's own pitch, stretch, tempo sync, trim, slip, loop and fades still apply and act on the warped
audio. Reversing a clip keeps its markers on the same audio. Playback, renders, exports and bounces
play the same sound.

Stretching a whole warped clip (resize with stretch, the stretch amount, fit to tempo, following
the tempo) works on its warped length: the clip's timeline scales as a whole and every marker
stays on its audio. A clip that keeps its seconds when the tempo changes keeps its warp too, and a
rendered stretch takes its markers along into the new file.

## Saving, undo and collaboration

Markers are saved with the clip in the project file. Each drag is one undo step. In a collaborative
project each marker is merged on its own, so two people can add or move different markers of a
clip at the same time.

## Warp mode in every waveform

The same teal **Warp** button sits in every view that shows a sample's waveform: the Audio clip
window (its waveform and its **Sample warp** module) and Kerf. In Smooth it is called **Flex**.
The key toggles it while the pointer is over the waveform: **W** in Studio, **Alt+W** in Clips,
Smooth and Tracker. Both can be changed in Settings > Keys, under Waveform.

- **Audio clip window.** The Warp tool edits the clip's own markers on the window's waveform, with
  the same clicks, drags and keys as the playlist's Warp tool.
- **Sampler.** With warp mode on, markers on the **Sample warp** module's waveform stretch the
  sample around them, and every note plays the warped sample. Turn warp mode off to hear the
  sample as it is; the markers are kept. The right click menu's **Warp** submenu has Add at
  transients, Quantize to the grid (on the loop's beats), its strength, **New markers snap to
  transients**, **Set the loop's BPM** (type it or Detect it) and Clear markers.
- **Kerf.** The markers sit in a slim lane under the wave, so they never collide with the slice
  flags. Kerf slices the warped audio, so the slices fall on the warped beats, and **Slice at warp
  markers** (in the Regions menu, and in the lane's Warp menu) cuts a slice at every marker.
  Clicks in the lane only touch the markers: they never select, add or remove a region.

- **Loupe.** Warp is an edit there: the markers sit in a lane at the bottom of the waveform, and
  **Apply warp to a new file** in the lane's right click menu writes the warped audio, as one step of
  Loupe's undo. Adding, moving and removing a marker are each one step of Loupe's undo too, while
  they are the last thing you did. Markers you have not applied yet are saved with the project, on
  the channel whose sample Loupe edits (or the Loupe effect in a mixer track): they are there when you
  open the project again, the song's undo takes them back as well, and in a collaboration session the
  others see them. A file opened in Loupe on its own keeps its markers until Loupe closes.

The warped sample is made once when the markers change, never while notes play, and each computer
of a collaboration session makes its own from the markers. Each drag is one undo step.

Loupe's spectrogram moved from W to **Shift+W**, and the Tracker sample editor's Wave preview from
Alt+W to **Alt+P**.

## FL Studio projects

FL Studio has no warp markers for playlist audio clips. When you export an FL Studio project, a
warped clip is written as what it plays: one clip per section, each stretched to its section's
length, grouped, with the clip's fades at its outer edges. On a channel that resamples, FL Studio
plays those sections at a changed pitch; the export report says so.
