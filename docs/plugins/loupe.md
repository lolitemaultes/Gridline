# Loupe: playing, selecting and scrubbing

Loupe is Gridline's audio editor. It opens in three ways:

- **In a mixer slot.** Add Loupe to an insert to record into it and play back through that insert.
- **On a channel's sample.** Right click a sampled channel in the channel rack and choose **Edit sample in Loupe**, or press **Edit in Loupe** in the channel's settings window, or open an audio clip's menu in the playlist (its header's drop-down or a right click) and choose **Send to Loupe**. Every edit comes back to the channel (one undo step each), so the channel and its clips play the edited version.
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

## Silence

The Edit menu has **Trim silence at the ends** (also the trim button in the toolbar) and **Remove silence** side by side. The first takes the silence off the start and the end of the audio; the second also cuts out every silence inside it, with a short fade at each join. Markers and regions move with the audio. **Silence settings** sets the threshold (automatic from the noise floor, or a level), the shortest gap that counts as a silence, the margin kept around each sound, the length of the fades and whether quiet breaths stay; the same settings serve the playlist and the instruments. Each is one undo step.

## Repair tools

The **Tools > Repair** menu has the tools that fix a recording: **Clean up (denoise)**, **Declick**, **Declip** and **Deverb**. **Tools > Time and pitch** has **Drum loop stretch**. Each one works on the selection, or on the whole file when nothing is selected, and each is one undo step.

Every repair dialog has a **Preview** section. Press **Preview** to hear the result loop before you apply it; while it plays, change the settings and the preview follows a moment after you let go. **A: Processed** and **B: Original** switch between the result and the audio as it is now, at the same place, so you can compare. **C: Removed** (on the repair tools) plays only what the tool takes out: it should hold the noise, clicks or reverb and none of the sound you want to keep, so it is the quickest way to hear whether a setting goes too far. The small view shows the original (dim) under the result. The preview plays up to 12 seconds of the selection, from the cursor when it is inside it. Every tool dialog with settings (Equalize, Reverb, Blur, Noise gate, Amplify, Normalize, Envelope and Spectral brush too) has the same preview.

On a long file, **Accept** shows how far the edit has got, and **Stop** ends it with the audio left as it was. The busy plate over the waveform shows the same percentage.

The same tools (all but the spectral brush) are on an audio clip's menu in the playlist, under **Audio tools**: they work on the part the clip plays and give the clip a new file of its own. See [the playlist's Audio tools](../playlist.md#audio-tools).

### Clean up (denoise)

Takes steady noise out: hiss, room tone, hum, fan noise. For the best result, select a part with only noise, open **Clean up** and press **Learn from the selection**; then select what you want to clean (or nothing, for the whole file). With **Adaptive**, or with no profile learned, Loupe follows the noise floor as it changes, so it works on noise that rises and falls too.

**Reduction** is how far the noise is turned down. **Sensitivity** decides how much of the quiet sound counts as noise: raise it if noise is left, lower it if soft parts of the sound go dull. **Smoothing** steadies the result over time and frequency, which keeps it natural and free of the "watery" sound of heavy noise removal; less smoothing follows quick sounds more closely. **Character** tells Loupe what the noise is like: **Broadband** for hiss and room, **Tonal** for hum and whine. Drum hits and other quick sounds are handled with finer timing, so they stay sharp. **Light**, **Medium** and **Strong** are good starting points.

### Declick

Takes out clicks, pops and vinyl crackle, and digital clicks from bad edits or dropouts. Loupe looks for short sounds that do not follow from the audio around them, then rebuilds each one from the audio on both sides. The clicks it finds are marked in red in the dialog's overview and on the waveform, and the note says how many. **Sensitivity** finds fainter clicks when raised; **Longest click** is the longest disturbance it will treat as a click, so drum hits and other real sounds are left alone. **Clicks**, **Crackle** and **Heavy** set both for common cases.

### Noise gate

Silences the audio between sounds. It opens when the level rises above **Threshold** and closes only when it falls **Hysteresis** below it, so it does not flutter on a sound that hovers near the threshold. **Attack**, **Hold** and **Release** shape how it opens, stays open and closes; **Floor** is how far it turns the audio down when closed.

### Deverb

Takes the room's reverb out of a recording: the tail that rings on after each sound, and the wash under sustained sounds. Loupe measures the room's reverb time from the places where a sound stops and the room rings on (**Measure**, on by default; the note under it says what it found, for low and high frequencies too). If the audio has no clear decays, or the result does not sound right, turn **Measure** off and set the **Reverb time** by ear: the time the room takes to fall silent.

**Early** keeps the early reflections, the first 50 ms of the room, so the sound keeps a sense of space and only the late tail comes out. Turn it off to dry the sound out further. **Amount** sets how much comes out; **Gentle**, **Medium** and **Strong** are good starting points. Moderate amounts stay natural; very high amounts can thin out long held notes.

### Declip

Rebuilds peaks that were cut flat by clipping. With **Auto**, Loupe finds the level the audio was clipped at (runs of equal samples at the peak). Turn **Auto** off to set a **Clip level** by hand, for audio that was clipped and then converted, where the flat tops are no longer exactly flat. The dialog shows how many clipped regions it found and marks them in red, in its own overview and on the waveform behind it.

Each clipped run is redrawn from the audio around it, so the peak comes back the way it would have been. The rebuilt peaks rise above the old clip level, so **Gain** (on by default) lowers the level just enough to fit them under the **Ceiling**.

### Drum loop stretch

Changes the tempo of a drum loop and keeps its hits sharp. Select the loop (or use the whole file). Loupe finds the hits and the loop's tempo (**Detected**; set **Loop tempo** by hand if it is wrong), and **New tempo** starts at the song's tempo. **Length** is the loop's length in bars at the new tempo; it starts at the loop's own length, so the loop stays the same number of bars. **Song tempo**, **Half time** and **Double time** set the new tempo in one click.

Every hit lands exactly on its place at the new tempo, the result is exactly the new length, and its end runs on into its start, so it loops with no click. **Between hits** chooses what happens to the sound between the hits: **Stretch the sound** stretches it to fill the new time, and **Slices with fades** plays each slice as it is and fades it out before the next hit (the classic sliced sound, with gaps when slowing down). **Attack** is how much of each hit is kept exactly as it was. **Sensitivity** decides how quiet a hit can be and still count; the hits it finds are marked in the dialog's overview and on the waveform.

## The spectrogram

**SPECTRUM** in the bottom bar (or **View > Spectrogram**, Shift+W) shows the audio as a spectrogram: time across, frequency up (30 Hz at the bottom to the top of the audio's range, on a musical scale), level as colour from the background through green and amber to white. It is drawn for the pixels of your screen: when you zoom or scroll, the picture you had is stretched for a moment and then redrawn sharp for the new view.

Low notes are analysed with long windows and high ones with short windows, so a bass line shows each note as a thin line while hi-hats and clicks stay sharp in time. Tones a semitone apart are told apart from about 100 Hz upwards. Both channels count, so sound that is only on one side, or in opposite phase on the two sides, still shows. A steady tone reads its true level: a full-scale sine is 0 dB.

Point at the spectrogram to read the time, the frequency, its note and the level there in the hint line.

The **View** menu has two settings for it, kept for your next session:

- **Spectrogram range**: how far below 0 dB the colours reach (60, 80, 100 or 120 dB). A smaller range hides quiet detail and gives loud material more contrast; 120 dB shows the noise floor of a 16-bit recording.
- **Spectrogram resolution**: **Sharper in time** (half-length windows, for drums and speech), **Balanced**, or **Sharper in frequency** (double-length windows, for tuning, hum and close notes).
