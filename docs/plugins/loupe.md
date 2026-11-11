# Loupe: playing, selecting and scrubbing

Loupe is Gridline's audio editor. It opens in three ways:

- **In a mixer slot.** Add Loupe to an insert to record into it and play back through that insert.
- **On a channel's sample.** Right click a sampled channel in the channel rack and choose **Edit sample in Loupe**, or press **Edit in Loupe** in the channel's settings window, or right click an audio clip in the playlist and choose **Edit in Loupe**. Every edit comes back to the channel (one undo step each), so the channel and its clips play the edited version.
- **On a file from the browser.** Right click an audio file in the browser and choose **Edit in Loupe**. The file itself is never changed: your edits stay in Loupe until you drag them out, send them to the playlist or save them.

## Play position and playback

- Click the waveform or the ruler to set the play position. **Play** (or Space) starts from there.
- While Loupe plays, a click jumps playback to that point.
- With a selection, **Play** plays the selection. With **Loop** on (or L) it loops it, with no gap, wherever Loupe is open: in a mixer slot, on a channel's sample or on a file.
- Make a new selection while playing and Loupe plays it at once: playback carries on where it is when that is inside the new selection, otherwise it goes to the new selection's start. The old loop never keeps playing.
- With loop points set and **Loop** on, **Play** loops them, starting at the play position: from before the loop points it plays on into them and then loops them; from past them it plays on to the end. So a click while looping always jumps where you click.
- **Follow transport** (in a mixer slot) starts Loupe's playback with the song, from the play position.

## Selecting

- Drag in the waveform to select.
- Each edge of a selection has a small grip: drag an edge to move only that edge.
- Shift-click extends the selection to where you click (or selects from the play position when there is none yet).
- Double-click selects the region under the pointer, or the span between two markers.

## Scrubbing

Turn on **Scrub** (or hold Alt) and drag in the waveform, or drag along the ruler at any time: you hear the audio at the speed and in the direction you move, up to four times normal speed, and silence while the pointer rests. When you let go, the play position stays where the scrub ended, so you can find a spot by ear and then play, mark or cut from there.

## Following the playhead

The toggle on the right of the waveform (also **View > Auto-scroll**) makes the view follow playback. Your choice is remembered for every Loupe; its default is under **Settings > Interface > Loupe**.

## Taking the audio out

Drag the grip handle at the right end of the tool strip onto a playlist track (an audio clip where you drop it), onto the channel rack (a new sampler channel, or onto a channel to replace its sample) or into a sampler. With a selection, only the selection goes. While you drag, Loupe turns see-through, so you can drop onto the channel rack or the playlist even where Loupe's window covers them.
