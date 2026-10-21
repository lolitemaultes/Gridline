# Signalsmith Linear 0.6.4 (vendored, subset)

Source: https://github.com/Signalsmith-Audio/linear (tag 0.6.4,
commit de55e6a50ffcf6f8f43f649692d94691c7025151). Licence: MIT (see LICENSE.txt).
Only `fft.h` and `stft.h` are vendored, unmodified: the STFT/FFT that
Signalsmith Stretch uses. No platform back ends (Accelerate / IPP / PFFFT /
xsimd) are used.
