# Rubber Band Library 4.0.0 (vendored)

Source: https://github.com/breakfastquay/rubberband, tag v4.0.0
(commit 1d95888bec3ae0a17c0c4af791810d5a63f6bc35). Copyright Particular
Programs Ltd. Licence: GPL-2.0-or-later (see COPYING); Gridline uses it under
the GPL-3.0, as the "or later" allows. README.md is upstream's.

Gridline offers it as the "Rubber Band" time mode (J-157), next to its own
stretch (Signalsmith Stretch) and Resample. It uses the R3 ("finer") engine:
the offline engine (study, then process) for clip renders and offline
processing, the realtime engine for sampler voices and clips whose pitch moves.
The wrapper is src/engine/TimeStretch.{h,cpp}.

## What is vendored

- `rubberband/` (the public headers), `src/` (common, faster, finer and the
  three API sources) and `single/RubberBandSingle.cpp`, COPYING, README.md.
- Left out, because Gridline does not build them: src/ext (speex, kissfft,
  pommier, getopt), src/jni, src/test, src/common/VectorOpsComplex.cpp, the
  command line program (main), the LADSPA / LV2 / Vamp plugins, the .NET and
  Java bindings, the Meson build and the documentation.

## How it is built

CMakeLists.txt builds `single/RubberBandSingle.cpp` (the whole library in one
translation unit) as the static library `gridline_rubberband`, warnings off for
that file only. That build needs no threads (no multi-threaded R2 option),
resamples with its built-in BQResampler and uses its built-in FFT; on macOS it
uses vDSP from the Accelerate framework, which CMake links there. No other
library is required, so it builds on every Gridline target.

## Local changes (marked "Gridline patch (J-157)")

1. `src/common/BQResampler.cpp`, `resampleInterleaved`: after `reset()` the
   resampler starts from silence, as a new one does. Upstream carried the old
   stream's history (its samples and fill) into the restarted stream, which
   delayed a restarted realtime pitch shift by several milliseconds (about
   6 ms at +12 semitones and 15 ms at -12 in Gridline's tests) and smeared its
   start.
2. `src/common/BQResampler.cpp`, constructor: an often-changing ratio (the
   realtime engine's pitch) reserves its phase table for the largest
   denominator it can pick (96000), so a later ratio change never allocates.
3. `src/finer/BinClassifier.h`, `reset()`: the lag queue's entries are cleared
   in place instead of freed and allocated again, so `reset()` allocates
   nothing.

Changes 2 and 3 let a prepared realtime voice restart (reset, start pad,
pre-roll) and bend its pitch and speed on the audio thread without any heap
allocation. Gridline also warms each realtime stretcher up at the extremes of
its pitch range when it prepares it, so the resampler's buffers reach their
largest size before playback.
