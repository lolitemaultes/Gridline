# Playlist: arranging tracks

## Selecting track headers

| Do this | To |
| --- | --- |
| Click a track's header | Select that track |
| Ctrl+click (Cmd+click on macOS) a header | Add it to the selection or take it out |
| Shift+click a header | Select every track from the last one you clicked to this one |

The header's own controls (the mute switch, the arm, input and monitoring buttons of an audio
track, the channel of an instrument track, the "+" and the fold arrow of a group) keep doing what
they did; a double-click on the name still renames the track.

## Moving tracks

Drag a track header up or down to move the track. If it is one of several selected headers, they
all go together and keep their order. You can also drag the "..." handle at the bottom left of a
header (a click there still opens the track menu). While you drag, an orange line shows where the
tracks will land, and the playlist scrolls when you reach its top or bottom edge; let go to move
them, or press Esc (or right-click) to cancel.

**Move up** and **Move down** in the track menu, or Alt+Up and Alt+Down with the playlist focused,
move the selected tracks one place.

Everything that belongs to a track travels with it: its clips, name, colour, height, mute, lock,
icon, live settings and, for audio and instrument tracks, the mixer track or channel it is linked
to. Tracks you never named show their new number.

Groups stay together:

- dragging a group's top track moves the whole group;
- a track dropped between the members of a group joins that group;
- a member dragged out of its group leaves it;
- one step up or down passes a folded group as a whole.

Each move is one undo step, is saved with the project and reaches collaborators. Moving tracks
changes only the arrangement you are working on.

## Rendering clips

Right-click a clip, or open an audio clip's header drop-down, and choose **Render clip to audio**. The clip is rendered on its own (its track's effects and its automation included, nothing else in the song) over exactly its length, and replaces itself with an audio clip on the same track, at the same start and of the same length, muted if it was muted. The new audio plays on a sampler channel named after the clip with "(rendered)", and the render is sample for sample what the clip played. **Render clip to file...** saves the same render as a WAV file instead.

With several clips selected the rows are **Render each clip to audio** (each clip becomes its own audio clip, all in one undo step) and **Render each clip to a file...**, which asks for a folder and writes one WAV file per clip, named after the clips ("Name", "Name 2" and so on), never overwriting a file already there.

## Silence

An audio clip's menu has **Silence** (in Clips, its rows are in the menu itself): **Trim silence at the ends** moves the clip's start and end in to its first and last sound, and **Remove silence** cuts the clip into its parts with sound and takes out the silences between them. The first part keeps the clip's identity; each cut gets a short fade, and the clip's own fades stay at its ends. With **Close the gaps** on, the parts are packed one after the other from the clip's start and the later clips on the track move left by the time taken out. A looping clip is left alone (turn its loop off first).

**Silence settings** (in the same menu, and in Loupe) holds the detection, shared by the playlist, Loupe and the instruments:

- **Automatic threshold**: the threshold follows the audio's noise floor; turn it off to set a **Threshold** in dB.
- **Shortest gap**: quieter stretches shorter than this stay (pauses inside a phrase).
- **Margin**: kept before and after each sound.
- **Fades**: the length of the fade at each cut.
- **Keep breaths**: short quiet sounds between louder phrases stay; off, they count as silence.
- **Close the gaps**: as above, for Remove silence in the playlist.

Short clicks in a silence count as silence. Every change is one undo step and reaches collaborators.

## Audio tools

An audio clip's menu has **Audio tools**, in every layout (Studio's playlist, the Clips arrangement and its clip slots, Smooth's regions and the Tracker's clip pages), with Loupe's tools for the clip:

- Repair: **Clean up...** (noise reduction), **Declick...**, **Declip...**, **Deverb...** and **Noise gate...**
- Sound: **Equalize...**, **Reverb...** and **Blur...**
- Level and direction: **Normalize...**, **Amplify...**, **Fade in...**, **Fade out...** and **Reverse**
- Time: **Drum loop stretch...**

Each opens the tool's window with the same settings and presets as in Loupe (see [Loupe's repair tools](plugins/loupe.md#repair-tools)), and its **Preview**: the result loops while you change the settings, **A** plays it, **B** the clip as it is and, for the repair tools, **C** only what the tool takes out. **Accept** runs the tool with its progress shown; **Stop** leaves the clip as it was. **Reverse** has nothing to set, so it opens no window: it turns the audio round at once (the selected clips with it, and only the clip editor's selection when there is one), and shows its progress with **Stop** only when it takes a moment. **Fade in** and **Fade out** keep their window for the fade's curve.

The tool works on what the clip plays: its own part of the sample (from its start to its end, after any slip), in the direction it plays. When the clip editor has a time selection on the clip, the window offers **Whole clip** or **Selection**, and Clean up can learn the noise from the selection.

Nothing is overwritten: the result is a new file in the project's **Edited audio** folder, and the clip plays it from then on. A clip that shares its sample with other clips gets a channel of its own, a copy named after the original with **(edited)** after it (Kick becomes Kick (edited); an edited clip processed again keeps that name), so the other clips keep the original; a clip alone on its channel keeps its channel and its settings. With several audio clips selected, the tool runs on each one with the same settings (each clip its own new file) and one progress bar. **Drum loop stretch** and **Reverb** change the length: the clip then plays the new audio whole and takes its new length (a loop at its new tempo, a reverb with its tail). Every run is one undo step, which brings the original audio back, and in a collaboration session it reaches the others as one edit with its new files.

Spectral editing needs Loupe's spectrogram: use **Send to Loupe** for that.

