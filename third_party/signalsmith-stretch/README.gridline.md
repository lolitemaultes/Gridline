# Signalsmith Stretch 1.3.2 (vendored)

Source: https://github.com/Signalsmith-Audio/signalsmith-stretch
(commit a670068d9aeb64913331d5cc29337b19a457a7df). Licence: MIT (see LICENSE.txt).
Only `signalsmith-stretch.h` is vendored, unchanged from upstream (J-153
removed the local changes earlier versions carried: a formant energy bound
and an onset phase reset).

It needs
`signalsmith-linear/stft.h` (../signalsmith-linear, MIT), found through the
`third_party` include directory. Gridline builds it with the portable built-in
FFT (no SIGNALSMITH_USE_* platform flags), so it compiles on every target.

Gridline's wrapper (offline streaming renders, clip renders, realtime voices)
is src/engine/TimeStretch.{h,cpp}.
