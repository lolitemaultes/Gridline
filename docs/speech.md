# Speech

Gridline can speak. In the channel rack, open the add menu (the **+** under the channels, or
right-click an empty part of the rack) and choose **Speech...**.

1. **Text**: type (or paste) what it should say. A comma makes a short pause; a full stop,
   question mark or new line ends a phrase, and the voice falls at its end. Digits are spoken one
   by one. **Channel name** names the channel it adds (empty: the
   first words of the text).
2. **Voice**: pick one of the chips: Narrator, Deep, Bright, Child, Whisper and Singer, or one of the
   robots (Robot, Old robot, Toy robot).
3. **Shape** and **Character**: knobs, each with its value and unit under it.
   - **Pitch**: how high the voice starts (Hz).
   - **Speed**: how fast it talks.
   - **Intonation**: from flat to lively.
   - **Voice size**: a larger voice to the left, a smaller one to the right.
   - **Breath**: from clear to whispered.
   - **Vibrato**: a waver in the pitch (cents).
   - **Robot**: a flat pitch, a buzzier tone and ring modulation.
   - **Ring**: the ring modulation's rate for robot voices.

   Drag a knob, double-click it to go back to the voice's own value, or right-click it to reset it
   or type a value. **Reset to voice defaults** puts every knob back to the chosen voice.
4. **Preview** (the play button, or Enter) plays it and draws its waveform; press it again to stop.
   **Add channel** (Ctrl+Enter, Cmd+Enter on macOS) saves it as a WAV file in Gridline's `Speech`
   folder and adds a sampler channel that plays it. Escape closes the dialog. The last voice and its
   settings are remembered for next time.

The added channel is an ordinary sample: play it from the piano roll or steps, drag the file from
the browser to the playlist as an audio clip, open it in Loupe, slice it, and render it like any
other sound.

The voice is Gridline's own small formant synthesizer, with English spelling rules: it sounds
synthetic on purpose, which suits robot voices, vocoder effects and spoken hooks. Unusual names
may come out as they are spelled.

## Pitch per word

Put a number of semitones in brackets right after a word to raise or lower it: `up(1) down(-2)`.
A space before the bracket is fine (`up (1)`), and so are decimals and a plus sign (`(0.5)`,
`(+3)`). The marks are never spoken; they show in colour in the text box. A mark that is not a
number from -24 to 24 shows in the warning colour and is left out.

Underscores join words into one, said with no gap: `this_is_one_word(3)`.

**Style** decides the pitch line:

| Style | What it does |
| --- | --- |
| Natural | The voice's own rise and fall, with each word's mark added |
| Sing (one note) | Every word on the voice's pitch plus its mark, so the marks write a melody |
| Random | Each word a few semitones up or down |

A little tune in Sing style: `twin(0) kle(0) twin(7) kle(7) lit(9) tle(9) star(7)`.

The waveform marks where each word starts, and the saved file has a cue point (a slice) at every
word, labelled with the word. Load it into a slicer and it is cut into one slice per word. Put it
in the playlist as an audio clip and right-click it: **Chop > At the file's slices** (or **File's
slices** in the Chop and stutter window) cuts the clip into one piece per word, each starting
where its word starts, as one undo step. The same works for any file that marks its own slices.
The file's comment keeps the text and the voice it was said with.
