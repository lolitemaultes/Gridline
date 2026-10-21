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

The stroke is drawn at least 1.5 logical pixels wide (2 units at 18 px and up) and every icon is
grid fitted when rasterised: the glyph, then each of its paths, may shift by under half a pixel so
straight strokes land on whole pixels. No geometry is redrawn in code.
