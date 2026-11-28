# Scratching and nudging on Platter

Platter (`gl.platter`) treats each deck's platter like a turntable: the record follows your hand
while you hold it, and the motor takes it back when you let go. The mouse on the editor's
platter and a controller's jog wheel work the same way.

## The mouse on the platter

- **The record (the black part).** Press on it and drag round: the record turns with your hand.
  Clockwise plays forward, anti-clockwise plays backwards, holding still stops it. Let go and the
  motor brings it back up to speed (over the deck's **Start time**, at least a few hundredths of
  a second, so it never jumps). A synced deck is back on the beat at once, and a stopped deck's
  record stops where you let go of it. Let go
  of a record you are spinning fast (more than 2.5 times its speed) and it is thrown: it coasts
  and slows down by itself.
- **The rim (the dotted band round the edge).** Drag along it to nudge the speed for a moment:
  clockwise speeds the deck up, anti-clockwise slows it down, and the speed goes back to the
  pitch fader's once you stop. Use it to bring two decks into line by ear. On a stopped deck the
  rim turns the record a little, to find a spot. The rim lights while the pointer is on it.
- **The scroll wheel** over the platter nudges too, one small push per notch.
- **VINYL off.** Then the whole platter nudges, as a CD player's does, and never scratches.
- **The waveform.** Dragging a deck's waveform sideways scratches it as well.

## A controller's jog wheel

With a controller mapped (see **Controller...** in the Platter window), touching the top of the
jog wheel grabs the record (with VINYL on) and turning it scratches; its side nudges the speed.
The **Jog** knob on each deck sets how far the record turns for a turn of the wheel, and **JOG
TICKS / TURN** in the mapping drawer how many steps the wheel sends per turn. A wheel sends its
turns as numbers centred on 64, in two's complement, or as a sign bit and a size; the mapping
list shows which one a wheel is bound with, and clicking it picks the next one if the wheel turns
the wrong way or in jumps. **Hand feel** sets how closely the record follows the hand: tighter
follows quick scratches more closely, looser plays slow drags smoother.

## Why it stays smooth

A computer never hands over the hand's moves evenly: the mouse is read once a screen frame, in
whole pixels, and the editor gets round to each frame at a slightly different moment; a
controller's moves can be bunched together by its USB connection. Platter times each mouse move
by the start of the frame that read it (not the moment the editor was drawn), so moves made at
an even pace arrive evenly spaced. A pixel crossed and crossed back by the rounding of the
pointer's place is ignored, so a hand that holds still holds the record still. Each move is
played out over about the time the last moves took, so a quick scratch is followed within a
frame or two.

Measured with the mouse at 60 to 240 frames a second, uneven frames, controllers that bunch
their moves and audio buffers of 256 to 1024 samples (`tests/packs/test_platter_jog.cpp`): a
held record stays put, the record never turns where the hand did not, and a steady drag at
normal speed or faster plays a steady speed on a 60 or 120 Hz screen. Very slow drags, where the
mouse crosses a pixel only every few frames, and controllers that bunch their moves are still
heard as some unevenness.

Platter's scratch mechanics follow the approach of [Mixxx](https://github.com/mixxxdj/mixxx)
(GPL-2.0-or-later): its jog wheel filter, its mouse scratching (which reads the hand's moves at
a steady interval and assumes a late move rather than a stop) and its ramps back to the motor
were studied as a reference. No Mixxx code is included; Gridline's implementation is its own.
