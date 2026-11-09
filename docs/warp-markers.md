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

## FL Studio projects

FL Studio has no warp markers for playlist audio clips. When you export an FL Studio project, a
warped clip is written as what it plays: one clip per section, each stretched to its section's
length, grouped, with the clip's fades at its outer edges. On a channel that resamples, FL Studio
plays those sections at a changed pitch; the export report says so.
