# Interface icons: sources and licences

Gridline's interface icons are SVG files on a 24-unit grid with a 2-unit round-cap, round-join
stroke. They are embedded at build time (`cmake/EmbedIcons.cmake`) and rasterised by nanosvg
(`third_party/nanosvg`, zlib licence) into a texture atlas at the exact device-pixel size
(`src/app/icons/`). Which icon draws which control: `src/app/icons/IconNames.cpp`.

Only the icons Gridline uses are included. Each file starts with a comment naming its source.

## Lucide v1.60.0, ISC licence (`LICENSE.lucide.txt`)

Unmodified geometry: audio-waveform, chart-gantt, chevron-down, chevron-right, circle-help, copy,
ellipsis, eraser, eye, eye-off, file, file-plus, folder, folder-open, folder-tree, grid-3x3,
keyboard, layout-grid, list-music, lock, menu, minus, move-horizontal, paintbrush-vertical, pencil, plug, plus,
redo-2, repeat, save, scissors, search, settings, sliders-vertical, square-dashed-mouse-pointer,
stamp, step-forward, trash-2, undo-2, volume-x, x, zoom-in.

Customised for Gridline (transport glyphs, filled and optically matched):

- `play-filled`: Lucide "play", filled.
- `pause-filled`: Lucide "pause", filled, bars shortened to 16 units.
- `stop-filled`: Lucide "square", filled, 14-unit square.
- `record-filled`: Lucide "circle", filled, radius 7.

## Tabler Icons v3.49.0, MIT licence (`LICENSE.tabler.txt`)

Unmodified geometry (the invisible 24 x 24 frame path removed): magnet, metronome, piano (outline);
caret-up, caret-down, caret-left, caret-right (filled).

## Gridline originals (GPL-3.0, same grid and stroke rules)

- `slip`: clip edges with the content arrow sliding between them (slip tool).
- `detune`: a note with fine-pitch up / down chevrons (detune tool).
- `fade-handles`: a level rising, holding and falling over the volume line (fade / volume handles toggle).
- `zero-crossing`: one wave cycle crossing its centre line, the crossing marked (zero-crossing toggle).
- `stairs`: a level climbing in flat steps (Shuttle envelope step editing).
- `link-slots`: two open chain halves joined by a bar (Shuttle linked volume slot).
- `curve-smooth`: a rounded curve through three points (Convolver smooth curve mode).
- `curve-straight`: straight segments joining three points (Convolver straight curve mode).

### Replacing hand-drawn glyphs

These take the place of glyphs that were drawn in code. Each is drawn on the old glyph's own
geometry and is fitted into the old glyph's ink box when drawn (`icons::drawFit` / `drawGlyph`),
so it is never larger than what it replaced. `chevron-left` and `chevron-up` are originals that
mirror the Lucide chevrons; the ImGui arrow, check, close and step glyphs draw the carets, `check`,
`x`, `minus` and `plus` through a hook (`GlyphHook`, a Gridline patch in `third_party/imgui`).

- `blur`: two rings, the outer one fainter (blur).
- `cat-controller`: plugin category: a knob with its range arc (controllers).
- `cat-delay`: plugin category: decaying taps (delays).
- `cat-distortion`: plugin category: a clipped sine (distortion).
- `cat-drums`: plugin category: four drum pads (drums).
- `cat-dynamics`: plugin category: a transfer curve with a knee (dynamics).
- `cat-eq`: plugin category: an EQ bell with its handle (equalisers).
- `cat-filter`: plugin category: a low-pass curve (filters).
- `cat-midi`: plugin category: two beamed notes (MIDI).
- `cat-modulation`: plugin category: two detuned sines (modulation).
- `cat-pitch`: plugin category: an up and a down triangle (pitch).
- `cat-sampler`: plugin category: a waveform drawn as bars (samplers).
- `cat-spectral`: plugin category: spectrum bars (spectral and visual).
- `cat-stereo`: plugin category: two overlapping circles (stereo).
- `cat-synth`: plugin category: a saw wave (synths).
- `cat-utility`: plugin category: two fader tracks with caps (utility).
- `check`: a check mark (done, selected).
- `chevron-left`: an open chevron pointing left (previous, step left).
- `chevron-up`: an open chevron pointing up (step up, fold).
- `circles-apart`: two circles side by side (wider stereo).
- `circles-merged`: two overlapping circles (narrower stereo).
- `clip-automation`: a ramp through four points (automation clips).
- `clip-pattern`: three step bars (pattern clips).
- `denoise`: one strong bar among quiet ones (denoise).
- `dice-two`: a rounded square with two dots (layer trigger mode).
- `display-bar`: a single level bar (display option).
- `display-bars`: three rising bars (display option).
- `display-dots`: three dots (display option).
- `display-envelope`: an envelope line (display option).
- `display-levels`: two level lines (display option).
- `display-line`: a jagged level line (display option).
- `display-peaks`: five short peak bars (display option).
- `edison-loop`: a loop arrow (loop playback).
- `edison-loop-set`: a loop arrow round a cross (set the loop from the selection).
- `envelope-points`: an envelope line through its points (envelope).
- `eq-curve`: an EQ curve, a dip then a peak (equaliser).
- `fade-in`: a level ramp rising over its base line (fade in).
- `fade-out`: a level ramp falling over its base line (fade out).
- `flag`: a pennant on a pole (add a marker).
- `follow`: two linked rings (follow the playhead).
- `grip-dots`: two columns of three dots (drag handle).
- `headphones`: a headband over two ear cups (audition).
- `heart`: a heart outline (favourite project).
- `heart-filled`: a heart, filled (favourite project set).
- `info`: a lower-case i on its own (information).
- `input-jack`: a jack plug: a ring with a stem (audio input).
- `list-bullets`: four bulleted lines (browse a list).
- `loop-arrows`: two arrows chasing round a loop (a clip that repeats).
- `normalize`: a sound raised to two level lines (normalise).
- `plus-minus`: a plus over a minus (bipolar).
- `power`: a power symbol: an open ring with a stem (on / bypass).
- `question`: a question mark on its own (help).
- `reverb-tail`: bars dying away (reverb).
- `ring`: an empty ring (the rim of a round arm button).
- `scroll-to`: a play triangle against a stop line (auto scroll).
- `scrub`: bars under a pointer (scrub).
- `send-out`: a box with an arrow leaving it (send to).
- `sine`: one sine cycle over its centre line (phase invert).
- `slash`: a single diagonal stroke (none, default).
- `slice-wave`: decaying hits between slice lines (slice).
- `slide-arrow`: a dot sliding along a line to an arrow head (slide points).
- `snowflake`: three crossing strokes (freeze).
- `speaker`: a loudspeaker (monitoring off).
- `speaker-small`: a small loudspeaker with one wave (sound on).
- `speaker-small-off`: a small loudspeaker with a cross (sound off).
- `speaker-waves`: a loudspeaker with two sound waves (monitoring on).
- `star`: a five-pointed star outline (favourite).
- `star-filled`: a five-pointed star, filled (favourite set).
- `store-tray`: an arrow down into a tray (store in the spare state).
- `stretch`: arrows pulling apart round two lines (time stretch).
- `swap-arrows`: two opposing arrows (flip with the spare state).
- `track-bass`: mixer track icon: one deep wave (bass).
- `track-bus`: mixer track icon: three lines merging into one (bus).
- `track-delay`: mixer track icon: four fading echoes (delay).
- `track-effects`: mixer track icon: a four-point spark (effects).
- `track-guitar`: mixer track icon: a guitar body, sound hole and neck (guitar).
- `track-hats`: mixer track icon: two cymbals on a stand (hats).
- `track-keys`: mixer track icon: three white keys and two black keys (keys).
- `track-kick`: mixer track icon: a drum head with a beater dot (kick).
- `track-output`: mixer track icon: a loudspeaker with two waves (output).
- `track-pad`: mixer track icon: two soft layered waves (pad).
- `track-percussion`: mixer track icon: a shaker with seeds (percussion).
- `track-reverb`: mixer track icon: a source with expanding arcs (reverb).
- `track-sidechain`: mixer track icon: an arrow into a key bracket (sidechain).
- `track-snare`: mixer track icon: a shallow drum (snare).
- `track-synth`: mixer track icon: a saw wave (synth).
- `track-vocal`: mixer track icon: a microphone on a stand (vocal).
- `trim`: a sound between two cut lines (trim).
- `window-dockin`: a window frame with an arrow coming into it at the upper right (dock back in).
- `window-popout`: a window frame with an arrow leaving it at the upper right (pop out).

The stroke is drawn at least 1.5 logical pixels wide (2 units at 18 px and up) and every icon is
grid fitted when rasterised: the glyph, then each of its paths, may shift by under half a pixel so
straight strokes land on whole pixels. No geometry is redrawn in code.
