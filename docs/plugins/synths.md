# Synths pack (`src/packs/synths/`)

Two subtractive/wavetable instruments that cover the classic synth family of a
pattern-based DAW. Both are original designs (code, names, presets, artwork);
the layout follows the usual grouping of such synths: voice strip, oscillators,
filter + envelopes, modulation, arpeggiator / effects.

| Type | Name | Covers the role of |
|---|---|---|
| `gl.analog` | Analog Synth | three-oscillator "simple synth", mini mono/poly synth, 3-osc subtractive synth with FX, supersaw-style synth (unison), classic poly with arpeggiator, Juno-style DCO poly (pulse + saw + sub, PWM, chorus), bass-guitar and FX-gun sounds (presets) |
| `gl.wavetable` | Isoline | wavetable / "growl" style synths, dual-oscillator synth with mod matrix and effects pages, preset-player style macro sounds |

## Analog Synth (`gl.analog`)

Signal flow per voice: 3 oscillators × up to 8 unison copies + sub + noise →
drive → multimode filter (stereo when unison is spread) → amp envelope →
voice sum → FX strip (drive → chorus → ping-pong delay → reverb) → pan.

- Oscillators: sine, triangle (polyBLAMP), saw and pulse with PWM (polyBLEP),
  noise; coarse ±36 st, fine ±100 ct, level, width; per-oscillator modulation
  by the previous oscillator (1←3, 2←1, 3←2): hard sync (BLEP-corrected), ring,
  phase FM, with depth.
- Sub oscillator (−1/−2 oct, sine/square), noise level.
- Filter: LP/BP/HP 12 and 24 dB, notch, peak; cutoff, resonance, drive, key
  tracking, envelope amount, velocity → cutoff. Drag on the graph for
  cutoff/resonance.
- Three ADSR envelopes (amp, filter, mod; mod env has a target + amount).
- Two LFOs: 7 shapes (sine, triangle, ramps, square, stepped and smooth
  random), free Hz or tempo division, retrigger, fade-in, target + amount.
- 4-slot mod matrix: LFO 1/2, mod/filter/amp env, velocity, key, random →
  pitch, osc pitches, pulse width, osc mod depth, cutoff, resonance, volume,
  pan, unison detune, LFO rates, noise.
- Voice: poly (1–32) with stealing (released voices first, then oldest), mono
  (retrigger) and legato; glide always / only on overlapping notes; unison
  1–8 with detune and stereo spread; velocity → volume / cutoff.
- Arpeggiator: up, down, up-down, random, as played; 1–4 octaves; tempo rate;
  gate.
- Presets (12): Velvet Pad, Rubber Bass, Glass Pluck, Trance Stack, Sync Scream,
  Ring Bells, Arp Bubbles, PWM Strings, Wobble Bass, Fingered Bass, Brass
  Section, Laser Zap.

## Isoline (`gl.wavetable`)

- Two wavetable oscillators. Ten built-in, procedurally generated tables
  (Analog morph, Pulse sweep, Vowels, Harmonic build, Sync sweep, FM sweep,
  Resonant sweep, Odd to even, Bit steps, Drawbars), 8 morph frames each,
  stored as 11 band-limited mip levels (no aliasing at high notes).
- Per oscillator: position, warp (bend ±, sync, mirror, crush, FM from the
  other oscillator) + amount, coarse/fine, level, pan, unison 1–16 with detune,
  blend (centre vs. side voices), stereo width, start phase / random phase.
- Sub (sine/triangle/square, −1/−2 oct) and coloured noise.
- Two filters (same modes as Analog) with drive and key tracking, serial or
  parallel.
- Four ADSR envelopes (Amp + 3 modulation), four LFOs (free/tempo, retrigger),
  an 8-slot modulation matrix (envelopes, LFOs, velocity, key, random, macros
  → pitches, positions, warps, levels, detune, cutoffs/resonances, sub, noise,
  volume, pan, LFO rates) and four macros.
- FX page (same strip as Analog) shares the panel with the matrix page.
- Presets (11): Hyper Saw, Formant Choir, Digital Keys, Resonant Drift, Growl
  Bass, Pulse Drift, Drawbar Organ, Glass Bell, Sub Pressure, Macro Morph
  Lead, Vowel Bass.

## Engineering notes

- No allocation on the audio thread: voices, scratch buffers are fixed arrays;
  delay/chorus/reverb buffers are sized for 192 kHz in the constructor;
  wavetables are built once (function-local static) when the first Isoline
  instance is created on the UI thread.
- Block-size invariance: modulation runs every 16 samples of each voice's own
  clock, free-running LFOs use a global phase advanced per sample, FX are per
  sample; tests compare 37/64/1000-sample blocks.
- Tests: `tests/packs/test_synths.cpp` (catalogue, every preset sounds, silence
  after release, extremes and random states stay finite, block-size
  invariance, voice stealing, glide/legato/mono fallback, aliasing at high
  notes, arpeggiator, FX tail, `normalizePluginState`, table band-limits, CPU
  report).
