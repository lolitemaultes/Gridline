# Recording audio

Gridline records the inputs of your audio device into the playlist. This page covers the input of a
mixer track (trim, polarity, metering and monitoring), pre-roll and post-roll around a punch range,
record safe, and loop recording into take lanes that you comp, flatten and share.

The record button's menu (right-click it, or **Options > Recording options**) holds the options
below; Settings > Audio holds the input device, the recording offset and the file format.

## Arming a track

A mixer track records when it has an input and is armed. Pick the input in the mixer's input panel
(select the track; the panel shows **Input**), then click the record dot at the bottom of its strip.
A track with no input gets the first input pair when you arm it. While the record button is on and
the transport plays, every armed track records into the playlist track linked to it.

## The Recording card

Clicking the record button on opens a small **Recording card** at the bottom right of the window.

- **Ready to record** lists the armed tracks with their input levels, how many minutes of recording
  the disk holds for them, and the mode (punch, when punching at the time selection). **Record and
  play** starts playback.
- **Recording** shows the time recorded, the mode and the pass, the take number, the disk time left
  and the levels, with **Stop** (the take is kept) and **Discard take** (stops and throws away what
  is being recorded).
- **H** (while the typing keyboard is off), or the fold button, folds the card to a small **REC** pill
  that shows the time; click the pill or press H again to bring the card back. The close button closes
  the card and leaves the record button on.

**Full options** on the card opens the **Record** window, with every recording setting on pages:

- **Ready**: a summary of what the next take does (mode, pre-roll, count-in, where loop passes go,
  armed tracks, file format and folder, disk time left), and when the record controls open and how
  they show.
- **Mode and punch**: punch at the time selection, the punch range, pre-roll and post-roll.
- **Count-in**: bars of count-in.
- **Takes**: loop passes as take lanes, drop silent takes, disarm after recording, naming.
- **Inputs and monitoring**: input only and low latency monitoring, and the input panel of every
  armed track or track with an input.
- **Notes**: recording from MIDI devices and the typing keyboard: whether notes and controller moves
  record, merge or replace on loop passes, a new take per pass, quantize (off, starts, or starts and
  lengths), and where controller moves go (automatic, pattern events or automation clips).
- **Files**: the file format, the folder and the rate.

Settings that would change a take in progress (the punch, the file format) wait until it stops.

**Recording options > Record controls** (or **Open** on the Record window's Ready page) sets when it opens: every time Record is clicked (the
default), the first time in each song, or never. Shift+click on the record button, and the record
key, record without opening it.

### Card, strip or the full window

The record controls can show three ways (**Show as** on the Record window's Ready page, or **Recording
options > Record controls**):

- **Card** (the default): the card at the bottom right. When a take starts, an open Record window folds
  away into the card.
- **Strip**: one row under the top bar with the same contents as the card: the time and the take, each
  armed track's name and level, the disk time left, **Stop**, **Discard take** (or **Record and
  play** when ready) and **Full options**. The views below move down to make room, so nothing is
  covered. H folds the strip to the REC pill as well.
- **This window**: the Record window stays open while recording. Its footer then shows the time,
  the take and the disk time left, with **Stop** and **Discard take**; the card steps aside while
  the window is open.

## Input: trim, polarity and the meter

Each mixer track's input has:

- **Trim**, from -24 to +24 dB. It applies before the input meter, before what you hear while
  monitoring and before the recording, so the take is written at the level you see.
- **Polarity** (the button beside Trim) turns the input upside down, for a microphone that faces the
  other way or a cable wired backwards.
- **The input meter** holds its peak for a moment and lights a clip lamp when the input reaches
  0 dBFS. Click the lamp to clear it.

Trim and polarity are saved in the song. In a shared session each person keeps their own, since the
inputs belong to each computer.

## Monitoring

**Monitor** in the input panel sets what an armed track lets you hear of its input:

- **On**: always the input.
- **Off**: never the input (for gear you hear another way).
- **Auto**: tape style. With a punch range, while recording, you hear the input inside the range and
  the playback around it, so you hear the old part right up to the punch in. Without a punch range,
  armed tracks hear their input.

Two options in **Recording options > Monitoring** apply to every armed track:

- **Input only monitoring** keeps the input on in Auto all the time.
- **Low latency monitoring** makes armed tracks skip their effects that add latency while they
  record, so the performer hears the input sooner. The effects come back when recording stops.

## Punch range, pre-roll and post-roll

**Punch in/out at the time selection** (in the record button's menu) records only inside the time
selection. With the record button armed and song mode on, the playlist ruler hatches the pre-roll
and post-roll around the range.

- **Pre-roll** (1 to 16 beats) starts playback that much before the punch in, so the performer hears
  the run-up. It applies to the first pass.
- **Post-roll** (1 to 16 beats) plays on that long past the punch out, then stops.

Set the punch range before you press Play.

### Quick punch

With **Quick punch** on (Record window > Mode and punch, or the record button's menu), you can punch
in and out while the song plays: press the record key (or the record button, or the card's **Punch in**
/ **Punch out** button) to start recording at that moment and press it again to stop, as often as you
like. Playback goes on, and every piece you record in one pass belongs to that pass (they don't open
take lanes). When the transport comes round to an earlier point (a loop), the next punch is the
next pass.

### Punch crossfades

Where a take starts or ends at a punch (the punch range or a quick punch), it crossfades with the
audio already on the track instead of cutting in: the old audio fades out while the take fades in,
over a short crossfade centred on the punch point, on whole ticks. **Crossfade** on the Mode and
punch page sets its length: Off, 5, **10** (the default), 20 or 50 ms. To have audio before the punch
for the fade, Gridline uses what the armed track heard just before it (see Capture while armed), so
the take reaches back half the crossfade. The old audio under the take is split and the middle part
is removed; undo brings it back. A take that starts when you press Play, or a loop pass that starts
at the loop start, has nothing before it and starts as it always did.

## Capture while armed

An armed track keeps what its input hears while the transport plays, even when Record is off, for
the last **Keep** seconds (Record window > Files: Off, 30 s, **1 min**, 2 min; the memory it takes
per armed track shows beside it). If you played something great without recording, **Recover the
last pass** (Record window > Mode and punch, or **Recording options > Capture while armed**) puts the newest stretch
the transport played on the playlist as audio clips, where it was played, latency compensated like
any take: one undo step. The audio is kept in memory only, never in the song.

## Start on the first note

With **On the first note** on (Record window > Count-in), pressing Play with the record button armed
waits: the transport starts with the first note you play on a MIDI keyboard or the typing keyboard,
and that note is recorded at the start. **Or a sound above** also lets an armed track's input start
it, on the audio block where the input first reaches the level (-40, -30, -20 or -12 dB; **Notes only**
turns it off). The card says **Waiting for the first note**, with **Start now** and **Cancel**;
Stop cancels too.

## Measuring the latency with a loopback cable

Recordings are moved earlier by the audio device's latency so they line up with what you heard.
Gridline estimates it from the device buffers; to measure the real round trip, open **Settings >
Audio > Measure...** (or the Record window's Inputs page, or the record button's menu):

1. Connect an output of your interface to one of its inputs with a cable.
2. Turn the speakers down: a short burst of noise plays through the outputs.
3. Pick the input the cable comes back on and press **Measure**.

Gridline plays the burst three times and finds it in what comes back, to the sample (an interface
that turns the polarity over is found as well). When at least two measurements agree, **Use this**
makes the round trip the latency for recordings (plus the song's own delay compensation and the
manual offset, as before). The measurement belongs to the devices, sample rate and buffer size it
was made with: change any of them and the estimate applies again until you measure again. **Back to
the estimate** forgets it.

## Record safe

A track that must not be recorded over can be made **record safe**: right-click its record button in
the mixer, or right-click **Arm** in the input panel. It shows a lock instead of the record dot and can't be
armed until record safe is off again (arming it says so; the input panel's **Safe** button turns it
off). Turning it on disarms the track. Record safe
is saved in the song, a safe track opens disarmed, and in a shared session record safe is the same for
everyone.

## Loop recording and take lanes

With a loop running (a song loop or a time selection) and the record button on, each pass of the loop
is a take. When you stop, every pass of a track lands on that track: the newest take plays, and the
track opens its **take lanes**, one lane per take, under the track's row.

- The track's own row plays the **comp**: the parts of the takes that are picked. The lanes show
  every take in full, the picked parts in the take's colour and the rest dimmed.
- **Drag across a lane** to play that take over the range you swiped (it snaps to the playlist's grid). The rest of the comp stays as it was. Where a take has no audio (a last pass that was
  stopped early), the comp there is left alone.
- **Click a lane** to pick that take for the section under the pointer (the part that plays there).
- **Click a lane's header** (Take 1, Take 2...) to play that take wherever it has audio.
- The header of a take that plays somewhere is lit with a dot.

Every comp edit is one undo step. Moving or deleting the comp in the track's row moves or deletes the
takes with it, so they stay lined up.

Right-click a lane or its header, or use **Takes** in the clip menu:

- **Flatten the comp** renders the parts that play into one audio clip (with short crossfades where
  one take meets the next) and removes the takes. The file goes to the **Recorded** folder next to the
  project. Undo brings the takes back.
- **Remove unused takes** removes the parts of the takes that are not in the comp; what plays stays.
- **Hide the take lanes**, or **Show take lanes** in the track's menu, folds the lanes away and back.

**Loop passes as take lanes** in Recording options is on by default. Turn it off to have the passes
stack on the tracks below instead, as before. A song saved before take lanes keeps its takes on
separate tracks; **Takes > Move the takes into lanes** in a take's clip menu puts them on one track as
lanes.

### Rating, soloing and auditioning takes

Each lane header has three small **rating dots** and an **S**:

- Click a dot to rate the take one to three dots; click the lit last dot again for no rating. A rating
  is one undo step, saved with the song and shared in a collaboration session, and every piece of the
  take keeps it when you comp.
- Click **S** to **solo** the take: only that take plays on the track, wherever it has audio, while the
  comp stays exactly as it is. Click it again to go back to the comp. Soloing is listening only: it is
  not saved, not undone and nobody else hears it.
- Right-click a lane for **Rating**, **Solo this take** and **Audition from its start**, which plays the
  take alone from where it starts; stopping the transport ends the audition and the song position goes
  back to where it was. (Turn Record off to audition.)

The **Takes** page of the Record window lists every track's takes in every layout (Clips and the
Tracker have no lanes): each take with its range, whether it plays in the comp, its rating, and
**Solo** and **Audition** buttons.

The takes, the comp and the lanes are saved with the song exactly as they are. In a shared session
they are part of the song: each take's audio file is shared with the others, and everyone sees and
can change the same comp. When two people change the same part of the comp, the newest change wins.

## Recording notes

### Arming instruments

With nothing armed, the notes you play go to the selected channel, as always. To play and record
other instruments, **arm** them: what you play goes to every armed instrument and records there.

- **Channel rack:** hover a channel and click the dot at the right end of its name. An armed channel
  keeps a red dot.
- **Playlist (Studio):** an instrument track shows the same dot at the right end of its lower line when
  you hover the header, and keeps it red while armed.
- **Tracker:** the dot sits in each track's header, before the column buttons.
- **Smooth:** the R button of an instrument track's header arms it (the same switch).
- **Clips** arms its MIDI tracks with their own arm buttons.

Alt+click arms only that one. Arming belongs to your computer: it is not saved, and in a shared session
everyone arms their own instruments.

### What records from MIDI

**Recording options > What to record** (or the Notes page of the Record window) chooses what a recording
takes from MIDI devices and the typing keyboard: **Notes**, **Automation (controller moves)**, and under
**Other messages**, **Pitch bend**, **Pressure (aftertouch)** and **Program changes**. Pitch bend and pressure of an MPE controller
record with each note; bend, pressure and program changes linked to a control record as automation.
What you play still sounds; only the recording leaves out what is off. The choice is yours: it is kept
in this computer's settings, so in a shared session each person has their own.

### Recover what I just played

Gridline keeps the notes you played in the last two minutes while nothing was recording. **Recording
options > Recover what I just played** (or the Notes page of the Record window) puts the last phrase
back as notes, one undo step:

- played while a pattern played, the notes go into that pattern where you heard them;
- played while the song played, they go into a new take clip at the bar where you played them, on the
  channel's instrument track (or a free track), just as if you had been recording;
- played while the transport was stopped, they go into the selected pattern from its start, timed as
  you played them.

In Clips the **Capture** button does the same into the clip grid.

### The record options menu

Right-click the record button (or **Options > Recording options**) for every record setting in groups:
what to record, punch and count-in (with pre-roll and post-roll), audio takes (file format, take
lanes, silent takes, disarming, names), monitoring, notes, and **Record controls** (when they open and
how they show). **Record window...** at the top opens the full window.
