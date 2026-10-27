# Signalsmith Stretch 1.3.2 (vendored)

Source: https://github.com/Signalsmith-Audio/signalsmith-stretch
(commit a670068d9aeb64913331d5cc29337b19a457a7df). Licence: MIT (see LICENSE.txt).
Only `signalsmith-stretch.h` is vendored. Two local changes:

- J-140, marked "Gridline (J-140)": the formant energy ratio is bounded to +12 dB.
- J-145, marked "Gridline (J-145)": an optional onset phase reset (the public
  member `onsetPhaseReset`, 0 by default, which keeps the original behaviour).
  When it is above 0, a bin whose input energy grows by more than that factor
  from one block to the next takes the input's phase, in the first pass and in
  the vertical phase step, instead of the predicted one. Gridline sets it to 4
  on the engine that stretches the harmonic part of the onset modes, so a new
  note played at the original speed stays in phase with the input (and with
  the attack copied around it).

It needs
`signalsmith-linear/stft.h` (../signalsmith-linear, MIT), found through the
`third_party` include directory. Gridline builds it with the portable built-in
FFT (no SIGNALSMITH_USE_* platform flags), so it compiles on every target.

Gridline's wrapper (modes, offline exact-length rendering, realtime voices) is
src/engine/TimeStretch.{h,cpp}.
