# The Tracker Sampler's sample entries

A Tracker Sampler holds up to 16 samples, its **entries**, numbered 01 to 10 in hex as the
pattern numbers its instruments. Each entry is a sound with its own volume, pan, fine tune,
transpose, loop and start and end points; the instrument's key map says which entry each key plays.
The patterns name the instrument only, so the entries never change a pattern's instrument numbers.

## The entry list

In the Tracker layout the header band shows the current instrument's entries beside the instrument
list: all 16 rows, each with its number and name, and the empty ones with their number only. Marks at
the end of a row say it loops (L, or P for ping-pong), plays backwards (B), plays at 8 bits (8) or
that its file is missing (!). The wheel scrolls the list.

* **Click** an entry to pick it, empty or not. The Sample editor shows the entry picked; an empty
  one says so, with a **Load sample...** button and a place to drop a file.
* **Double-click** an entry to open the Sample editor on it.
* **Click its number** to mark it as the source sample (for the header's copy and exchange tools).
* **Ctrl+Alt+Up** and **Ctrl+Alt+Down** pick the entry above or below, empty ones too, in the
  pattern and in the Sample and Instrument editors. (Alt+Up and Alt+Down in the editors still step
  through the samples only.)
* **Right-click** an entry for its menu:
  * **Load into entry...** picks an audio file for it;
  * **Edit in the Sample editor**;
  * **Clear entry** empties it (its keys go to the nearest sample left);
  * **Duplicate to next free** copies it to the first empty entry;
  * **Copy to entry** copies it over another entry (that entry keeps its keys);
  * **Exchange with entry** swaps the two sounds (the keys stay where they are);
  * **Rename** names it in place (Enter keeps the name, Esc does not);
  * **Map all keys to this entry** makes every key play it.

## Loading into an entry

A sound goes into the entry picked from every way of loading one in the Tracker:

* the **Files** page (open the file, or double-click it);
* the **Browser**: double-click a sample, or drag it onto the Sample editor or onto an entry's row
  (a row takes the file whichever entry is picked);
* a file dragged from the desktop onto the Sample editor or an entry's row, or let go over the
  Tracker window;
* **Load...** and **Load sample...** in the Sample editor, and **Load into entry...** in the menu.

The first sample of an instrument plays on every key, in the entry it went into. A later one keeps
the key map as it is: give it keys in the Instrument editor's key map (drag across the piano), or
with **Map all keys to this entry**. Loading over a used entry replaces its sound and keeps its keys.

## Moving the loop

With a loop on (Forward or Ping-pong), its start and end are lines across the waveform, marked at the
top (start) and the bottom (end). Drag a line anywhere along it to move that end of the loop; hold
Alt to land on the nearest zero crossing. Drag the loop's strip at the top or the bottom of the
waveform, between its two lines, to move the whole loop with its length kept. Each drag is one undo
step.

## Sending a range to an entry

Drag across the waveform in the Sample editor to mark a range, then click **To entry** (beside the
cut, copy and crop buttons). Its menu offers:

* **Next free entry (NN)**: the first empty entry, with its number (off when all 16 are used);
* **Entry 01** to **Entry 10**: a picked entry. A used one shows its name and asks before it is
  replaced.

The entry then plays that range of the same file: nothing new is written to disk. It takes the
source's volume, pan, fine tune, transpose and bit depth, and its loop when the whole loop is inside
the range (otherwise it has no loop). It is named after the source with "part" added and is picked
at once. A new entry gets no keys (give it some in the key map, or with **All keys**); an entry it
replaces keeps its keys. Each send is one undo step and reaches everyone in a collaboration
session. The button is off, and says why, until a range is marked.

**Duplicate** and **All keys** in the same column copy the sample to the next free entry and make
every key play it.

The menu opens above the button. When the Sample editor is short of width (a small window with the
Browser beside it), the range and zoom buttons fold into **Zoom...** and the cut, copy and crop
buttons into **Edit...** (Copy and Paste stay as buttons while there is room), so every button
shows its whole name.

## Playing a sample backwards

**Backwards**, beside the loop type (No loop, Forward, Ping-pong) in the Sample editor and in the
Tracker Sampler's own window, plays the sample from its end to its start without changing the
audio. With a loop, the sound runs from the sample's end down into the loop and then plays the loop
backwards: a forward loop from its end to its start again and again, a ping-pong loop to and fro.
The part before the loop is never heard, as the part after it is not when the sample plays forwards.
So on a backwards sample the trims of the unheard part work from the other side: **Clean up**'s trim
after the loop starts the sample at its loop instead of ending it there, and **Cut after loop** and
**Minimise** in the Sample editor (**Trim after the loop** in the Tracker Sampler's own window) take
away the audio before the loop instead of after it. The sound does not change; each trim is one undo
step. A sample offset in a pattern counts from the end. The **Wave**, **Range** and **View**
buttons play backwards too. When the Sample editor is short of width, **Backwards** is the first row
of the **Process...** menu.

Each entry has its own setting, saved with the project; switching it is one undo step and reaches
everyone in a collaboration session. Projects saved before it play exactly as they did. Modules
have no such setting, so an imported module's samples play forwards.

The header's sample list marks a backwards sample with **B**, beside the loop's **L** (forward) or
**P** (ping-pong); hover the row to read the marks in words. The list in the Tracker Sampler's own
window says "backwards" beside the loop type. **Convert to Tracker Sampler** keeps a Sampler's
**Reverse** setting as **Backwards**: both play from the end and round the loop backwards.

**Reverse** (a separate button) still writes the audio backwards: use it when you want the reversed
audio itself, for example to edit it or save it as a file.

## The play position

While a sample sounds, a line in the Sample editor's waveform shows where each voice is. On a
ping-pong loop the line runs back on the backward pass, as the sound does. A sample played
**Backwards** has its line start at the end and run to the left. **Reverse** in the editor writes
the audio backwards, so a reversed sample plays (and its line runs) from left to right through the
reversed waveform.

## A Sampler's sample in the Sample editor

In the Tracker layout the **Sample** button (and a double-click on an audio clip that plays a
Sampler's sample) opens a Sampler in the Sample editor in the lower band. Two buttons where the
title stands pick its view, one click either way:

* **Sample editor**, the classic view (the default): the same waveform, range, loop handles, edits
  and play buttons as a Tracker Sampler's. The range, loop and cut points change the Sampler's own
  start, end and loop settings, and an edit rewrites its file as the Audio clip window's edits do.
  A Sampler plays one sample, so the controls for more entries (**To entry**, **Duplicate**,
  **All keys**) and **8-bit** are greyed out; **Convert to Tracker Sampler...** in the instrument
  list's right-click menu turns it into a Tracker Sampler with every control.
* **Advanced**: the Audio clip window's pages. The page list on its left picks **Waveform** or one
  of the window's tabs (Clip, Warp, Sample, Playback, Envelope, Filter, Sample FX, Channel, Note
  effects, MIDI and macros); each tab's modules sit beside **Output**, drawn in the Tracker's look.
  The controls are the Audio clip window's own: each change is one undo step, keeps its automation
  and link menus, and reaches everyone in a collaboration session. See
  [audio-clip-editor.md](audio-clip-editor.md) for what each module does.

Switching keeps the instrument, its sample and the selected range. The view picked last is
remembered as an app setting (not in the project), and the next Sampler or audio clip opens in it.
**Channel settings** and **Open plugin window** open a Sampler's settings in the floating Audio clip
window, as in the other layouts. A Tracker Sampler's sample entries, loop handles, **Backwards** and
**To entry** stay as described above.
