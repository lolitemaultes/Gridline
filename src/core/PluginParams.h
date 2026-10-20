// Gridline — parameter indices of the built-in plugins. The catalogue
// (PluginInfo.cpp), the DSP (engine/) and the editors (app/) all use these.
#pragma once

namespace gl::pp {

// ---------------------------------------------------------------- generators
// "gl.synth" — three-oscillator subtractive synth
enum Synth {
  SynOsc1Shape, SynOsc1Level,
  SynOsc2Shape, SynOsc2Level, SynOsc2Coarse, SynOsc2Fine,
  SynOsc3Shape, SynOsc3Level, SynOsc3Coarse, SynOsc3Fine,
  SynNoise,
  SynAttack, SynDecay, SynSustain, SynRelease,
  SynCutoff, SynResonance, SynFilterEnv, SynFilterDecay,
  SynGlide, SynVoices, SynVolume,
  // 0.5: osc 1 tuning, per-osc pan / invert / stereo phase / stereo detune, phase randomness,
  // osc 3 mode (AM etc.), osc 2 → osc 1 modulation, filter type, velocity → cutoff, LFO
  SynOsc1Coarse, SynOsc1Fine,
  SynOsc1Pan, SynOsc2Pan, SynOsc3Pan,
  SynOsc1Invert, SynOsc2Invert, SynOsc3Invert,
  SynOsc1Phase, SynOsc2Phase, SynOsc3Phase,
  SynOsc1Detune, SynOsc2Detune, SynOsc3Detune,
  SynPhaseRand, SynOsc3Mode, SynOsc2Mod,
  SynFilterType, SynVelCutoff, SynKeyTrack,
  SynLfoTarget, SynLfoShape, SynLfoRate, SynLfoAmount,
  // J-2: band-limited ("high quality") oscillators; off = the classic naive shapes.
  // Shape 7 = custom: one cycle read from the channel sample (PluginState::data).
  SynHQ,
  SynCount
};
enum SynOsc3ModeChoice { SynModeNormal, SynModeAM, SynModeRing, SynModeFM, SynModePM };
// "gl.sampler" — sample player
enum Sampler {
  SmpRoot, SmpFine, SmpAttack, SmpDecay, SmpSustain, SmpRelease,
  SmpStart, SmpReverse, SmpLoop, SmpOneShot, SmpVolume,
  // 0.5: region + loop points, sample processing, declick, time stretch, envelope delay/hold,
  // filter + filter envelope, LFO, pan, velocity, polyphony
  SmpEnd, SmpLoopStart, SmpLoopEnd, SmpLoopMode,
  SmpNormalize, SmpPolarity, SmpRemoveDc, SmpSwapStereo, SmpFadeIn, SmpFadeOut, SmpLoopXfade,
  SmpDeclick, SmpStretchMode, SmpPitchShift, SmpTimeStretch,
  SmpEnvDelay, SmpEnvHold,
  SmpFilterType, SmpCutoff, SmpReso, SmpFiltEnv, SmpFiltAttack, SmpFiltDecay, SmpFiltSustain, SmpFiltRelease,
  SmpVelCutoff, SmpKeyCutoff,
  SmpLfoTarget, SmpLfoShape, SmpLfoRate, SmpLfoAmount, SmpLfoDelay,
  SmpPan, SmpVelVolume, SmpVoices, SmpGlide,
  // J-23: precomputed sample effects (applied once to the sample data when
  // it loads, in the reference sampler's order: stereo delay, then per frame
  // time warp, ring modulation, low pass, ramps, boost and clip, then tilt,
  // stereo fade, reverb). Neutral defaults leave the sample untouched.
  SmpPreRampIn, SmpPreRampOut, SmpPreBoost, SmpPreClip, SmpPreTilt, SmpPreCutoff, SmpPreReso,
  SmpPreRingMix, SmpPreRingFreq, SmpPreStereoDelay, SmpPreWarp, SmpPreReverbMix, SmpPreReverbType, SmpPreFadeStereo,
  SmpCount
};
// "gl.kick"
enum Kick {
  KickPitch, KickEndPitch, KickSweep, KickDecay, KickClick, KickDrive, KickVolume,
  KickKeyTrack, KickSub, KickClickTone, KickClickLen, KickAttack, KickTone, KickVariation, KickNoise, KickNoiseDecay, KickSweepCurve,  // 0.5
  KickCount
};
// "gl.snare"
enum Snare {
  SnTone, SnSnappy, SnDecay, SnNoiseDecay, SnTune, SnVolume,
  SnSweep, SnSweepTime, SnNoiseBand, SnNoiseWidth, SnRing, SnHarmonics, SnClick, SnTilt, SnPan, SnVelTone,  // 0.5
  SnCount
};
// "gl.hat"
enum Hat {
  HatDecay, HatTone, HatOpen, HatVolume,
  HatMetal, HatBase, HatWidth, HatChoke, HatAttack, HatClick, HatPan,  // 0.5
  HatCount
};
// "gl.clap"
enum Clap {
  ClapDecay, ClapSpread, ClapTone, ClapVolume,
  ClapHits, ClapHitDecay, ClapFalloff, ClapWidth, ClapStereo, ClapHumanize, ClapPan,  // 0.5
  ClapCount
};
// "gl.pluck" — plucked string (Karplus-Strong)
enum Pluck { PlkDecay, PlkDamping, PlkBrightness, PlkPosition, PlkWidth, PlkRelease, PlkVolume, PlkNormalize, PlkGate, PlkCount };

// "gl.pads" — 16-pad drum sampler. Per pad: tune, level, pan, cut group, reverse.
enum PadField { PadTune, PadLevel, PadPan, PadCut, PadReverse, PadFields };
inline constexpr int kPads = 16;
inline constexpr int padParam(int pad, int field) { return pad * PadFields + field; }
inline constexpr int PadsBaseNote = kPads * PadFields;  // note of pad 1
inline constexpr int PadsVelocity = PadsBaseNote + 1;   // velocity → level amount
inline constexpr int PadsVolume = PadsBaseNote + 2;
// 0.5: per-pad extension block, then per-layer block (4 layers per pad).
enum PadExtField {
  PadNote, PadCutBy, PadMute, PadSolo, PadVelScale, PadLayerMode,
  PadAttack, PadHold, PadDecay, PadSustain, PadRelease, PadOneShot, PadStart,
  PadExtFields
};
inline constexpr int PadExtBase = PadsBaseNote + 3;
inline constexpr int padExt(int pad, int field) { return PadExtBase + pad * PadExtFields + field; }
inline constexpr int kPadLayers = 4;
enum PadLayerField { LayEnabled, LayVelLow, LayVelHigh, LayLevel, LayPan, LayTune, LayReverse, LayStart, LayEnd, LayFields };
inline constexpr int PadLayerBase = PadExtBase + kPads * PadExtFields;
inline constexpr int padLayer(int pad, int layer, int field) { return PadLayerBase + (pad * kPadLayers + layer) * LayFields + field; }
enum PadLayerMode { LayerStack, LayerCycle, LayerCycleBack, LayerRandom, LayerRandomNoRepeat };
// Aux sample index of a pad layer (layer 0 keeps the pad's own index).
inline constexpr int padLayerSample(int pad, int layer) { return layer == 0 ? pad : kPads + pad * (kPadLayers - 1) + (layer - 1); }
// J-2: bank B (pads 17..32) — one block per pad after bank A with the same
// fields (pad, extension, 4 layers). Use padP / padE / padL for any of the 32
// pads. Layers 5..64 of a pad, pad names / colours and the volume / pan
// envelopes live in PluginState::data (see Slicing.h).
inline constexpr int kPadsAll = 32;
inline constexpr int PadsBankBBase = PadLayerBase + kPads * kPadLayers * LayFields;
inline constexpr int kPadBlockB = PadFields + PadExtFields + kPadLayers * LayFields;
inline constexpr int padP(int pad, int field) { return pad < kPads ? padParam(pad, field) : PadsBankBBase + (pad - kPads) * kPadBlockB + field; }
inline constexpr int padE(int pad, int field) {
  return pad < kPads ? padExt(pad, field) : PadsBankBBase + (pad - kPads) * kPadBlockB + PadFields + field;
}
inline constexpr int padL(int pad, int layer, int field) {
  return pad < kPads ? padLayer(pad, layer, field) : PadsBankBBase + (pad - kPads) * kPadBlockB + PadFields + PadExtFields + layer * LayFields + field;
}
inline constexpr int PadsCount = PadsBankBBase + kPads * kPadBlockB;
inline constexpr int kPadMaxLayers = 64;  // J-33: shipped kits use up to 64 layers on a pad
// Aux sample slot of any pad / layer (bank A layers 1..4 keep their old slots).
inline constexpr int padLayerSampleX(int pad, int layer) {
  return pad < kPads && layer < kPadLayers ? padLayerSample(pad, layer) : kPads * kPadLayers + pad * kPadMaxLayers + layer;
}
inline constexpr int kPadSampleSlots = kPads * kPadLayers + kPadsAll * kPadMaxLayers;
// "gl.fm" — four-operator FM synth. Per operator: ratio, fine, level, A, D, S, R, velocity.
enum FmOpField { FmRatio, FmFine, FmLevel, FmAttack, FmDecay, FmSustain, FmRelease, FmVel, FmOpFields };
inline constexpr int kFmOps = 4;
inline constexpr int fmParam(int op, int field) { return op * FmOpFields + field; }
inline constexpr int FmAlgorithm = kFmOps * FmOpFields;
inline constexpr int FmFeedback = FmAlgorithm + 1;
inline constexpr int FmVoices = FmAlgorithm + 2;
inline constexpr int FmVolume = FmAlgorithm + 3;
// 0.5: global octave, LFO / vibrato, glide, pitch envelope; per-operator extension block.
inline constexpr int FmOctave = FmAlgorithm + 4, FmLfoRate = FmAlgorithm + 5, FmVibrato = FmAlgorithm + 6, FmLfoIndex = FmAlgorithm + 7;
inline constexpr int FmGlide = FmAlgorithm + 8, FmPitchEnv = FmAlgorithm + 9;
enum FmExtField { FmWave, FmFixed, FmOffset, FmThru, FmPan, FmPhase, FmKeyScale, FmInit, FmExtFields };
inline constexpr int FmExtBase = FmAlgorithm + 10;
inline constexpr int fmExt(int op, int field) { return FmExtBase + op * FmExtFields + field; }
inline constexpr int FmCount = FmExtBase + kFmOps * FmExtFields;
// "gl.acid" — monophonic bass with accent and slide
enum Acid {
  AcWave, AcCutoff, AcReso, AcEnvMod, AcDecay, AcAccent, AcSlide, AcDrive, AcVolume,
  // 0.5: tuning, wave blend, pulse width + PWM, high-pass, accent decay, key follow, clicks, drive tone,
  // accent threshold, then the 16-step sequencer
  AcTuning, AcBlend, AcVintage, AcPulseWidth, AcPwmRate, AcPwmDepth, AcHighPass, AcAccentDecay, AcKeyFollow,
  AcClicks, AcDriveTone, AcAccentThresh,
  AcSeqOn, AcSeqLength, AcSeqShuffle, AcSeqGate,
  AcSteps,  // first step field
  AcCountBase = AcSteps
};
enum AcStepField { AcStepNote, AcStepOctave, AcStepAccent, AcStepSlide, AcStepGate, AcStepFields };
inline constexpr int kAcSteps = 16;
inline constexpr int acStep(int step, int field) { return AcSteps + step * AcStepFields + field; }
inline constexpr int AcCount = AcSteps + kAcSteps * AcStepFields;
// "gl.keys" — electric piano
enum Keys {
  KyDecay, KyRelease, KyHardness, KyTreble, KyMuffle, KyTremRate, KyTremDepth, KyStereo, KyDetune, KyDrive, KyVelocity, KyVolume,
  KyVelHardness, KyVelMuffle, KyTune, KyStretch, KyModMode, KyModel, KyVelCurve, KyKeyPan,  // 0.5
  KyCount
};
// "gl.slicer" — beat slicer (slices mapped to keys from the base note)
enum Slicer {
  SlMode, SlSensitivity, SlBaseNote, SlPitch, SlAttack, SlRelease, SlReverse, SlVolume,
  // 0.5: global playback options, tempo fit, filter, then manual markers and per-slice settings
  SlFine, SlPlayToEnd, SlHold, SlMono, SlFadeIn, SlFadeOut, SlDeclick, SlFitTempo, SlLoopBeats,
  SlFilterType, SlCutoff, SlReso, SlPan, SlVelocity, SlChromatic, SlMinGap,
  SlMarkers,  // first marker
  SlBase = SlMarkers
};
inline constexpr int kMaxSlices = 64;
inline constexpr int SlModeManual = 5;  // SlMode choice: slice at the manual markers
inline constexpr int slMarker(int k) { return SlMarkers + k; }  // position 0..1 of slice k start, -1 = unused
enum SliceField { SliceGain, SlicePan, SlicePitch, SliceReverse, SliceCutoff, SliceStart, SliceMute, SliceFields };
inline constexpr int SliceBase = SlMarkers + kMaxSlices;
inline constexpr int slParam(int slice, int field) { return SliceBase + slice * SliceFields + field; }
// J-2 (region parity): second per-slice block — end point (-1 = up to the next
// marker), resonance offset, start delay, cut group (0 none), key (-1 = base
// note + index) — then global modulation X / Y (cutoff / resonance offsets).
enum SliceField2 { SliceEnd, SliceReso, SliceDelay, SliceCut, SliceKey, SliceFields2 };
inline constexpr int Slice2Base = SliceBase + kMaxSlices * SliceFields;
inline constexpr int slParam2(int slice, int field) { return Slice2Base + slice * SliceFields2 + field; }
inline constexpr int SlModX = Slice2Base + kMaxSlices * SliceFields2;
inline constexpr int SlModY = SlModX + 1;
inline constexpr int SlCount = SlModY + 1;
// "gl.granular"
enum Granular {
  GrPosition, GrSpread, GrSize, GrDensity, GrPitchRand, GrStereo, GrAttack, GrRelease, GrFreeze, GrVolume,
  GrSpeed, GrShape, GrLoop, GrKeyMode, GrBaseNote, GrTempoSync, GrWobble, GrWobbleRate, GrJitter, GrTransientHold, GrPitch,  // 0.5
  GrCount
};
// "gl.midiout"
enum MidiOut {
  MoChannel, MoProgram, MoVelocity,
  MoPort, MoBankMsb, MoBankLsb, MoBendRange, MoVelCurve,  // 0.5
  MoKnobs
};
enum MoKnobField { MoKnobValue, MoKnobType, MoKnobNumber, MoKnobMin, MoKnobMax, MoKnobChannel, MoKnobFields };
inline constexpr int kMoKnobs = 72;  // J-33: the reference keeps up to 72 controllers (values 0x22..0x142)
inline constexpr int moKnob(int k, int field) { return MoKnobs + k * MoKnobFields + field; }
inline constexpr int MoCount = MoKnobs + kMoKnobs * MoKnobFields;

// ------------------------------------------------------------------- effects
// "gl.eq" — seven-band parametric EQ. Per band: type, freq, gain, Q, slope.
enum EqBandField { EqType, EqFreq, EqGain, EqQ, EqSlope, EqFields };
inline constexpr int kEqBands = 7;
inline constexpr int eqParam(int band, int field) { return band * EqFields + field; }
inline constexpr int EqMain = kEqBands * EqFields;  // output level (dB)
// 0.5 additions (appended): per band order + enable, listen (band solo), spectrum display.
enum EqExtField { EqOrder, EqEnabled, EqExtFields };
inline constexpr int EqExtBase = EqMain + 1;
inline constexpr int eqExt(int band, int field) { return EqExtBase + band * EqExtFields + field; }
inline constexpr int EqListen = EqExtBase + kEqBands * EqExtFields;  // 0 = off, 1..7 = hear that band only
inline constexpr int EqSpectrum = EqListen + 1;                     // spectrum display: off / output / output + hold
// κ1 additions (appended): the reference EQ's band model. Per band a width
// (0..1, type-dependent: bandwidth for band/notch/peak, slope for cuts and
// shelves) and a unified order (7 choices). When EqModel = 1 (all new
// instances) the band design follows these; legacy states (EqModel = 0) are
// migrated on load (normalizePluginState) and then use them too.
enum EqFlField { EqWidth, EqFlOrder, EqFlFields };
inline constexpr int EqFlBase = EqSpectrum + 1;
inline constexpr int eqFl(int band, int field) { return EqFlBase + band * EqFlFields + field; }
inline constexpr int EqHQ = EqFlBase + kEqBands * EqFlFields;  // 0 off, 1 high quality (decramped, matched response)
inline constexpr int EqTokens = EqHQ + 1;                         // show band tokens on the graph
inline constexpr int EqSoloShift = EqTokens + 1;                  // band solo on Shift: off / adaptive / band pass / inherit width
inline constexpr int EqMonitor = EqSoloShift + 1;                 // graph monitor: off / input / output
inline constexpr int EqHistRange = EqMonitor + 1;                 // histogram floor: -60 / -90 / -120 dB / matched
inline constexpr int EqPivot = EqHistRange + 1;                   // histogram tilt: 0 / 1.5 / 3 / 4.5 / 6 dB/oct
inline constexpr int EqSmoothing = EqPivot + 1;                   // histogram time smoothing: fastest .. slowest
inline constexpr int EqHeatmap = EqSmoothing + 1;                 // heat map: off / top / bottom / full
inline constexpr int EqModel = EqHeatmap + 1;                     // 0 legacy (pre-κ1), 1 reference model, 2 classic (J-26)
inline constexpr int EqLinear = EqModel + 1;                      // linear phase: off / lowest .. highest (FIR 512 .. 16384 taps)
inline constexpr int EqPhaseView = EqLinear + 1;                  // graph: draw the phase response ("phase rotation")
inline constexpr int EqHistAvg = EqPhaseView + 1;                 // histogram: long-term average mode
// J-26: classic band model (EqModel = 2, the original v1 EQ): per band EqFreq (Hz), EqGain (dB),
// eqFl(b, EqWidth) = width knob (Q / slope law per type), RBJ designs at the host rate; output gain
// is linear (EqMainLin, 0 = silence) instead of EqMain.
inline constexpr int EqMainLin = EqHistAvg + 1;
inline constexpr int EqCount = EqMainLin + 1;
inline constexpr int eqLinearTaps(int choice) { return choice <= 0 ? 0 : 256 << choice; }
// EqFlOrder choices, steepest first (the reference stores -3..3 = index - 3).
enum EqFlOrderChoice { EqSteep8, EqSteep6, EqSteep4, EqOrd2, EqGentle4, EqGentle6, EqGentle8 };
enum EqBandType { EqOff, EqLowCut, EqBandPass, EqHighCut, EqNotch, EqLowShelf, EqPeak, EqHighShelf };
// EqSlope choices: 0..3 = 12/24/36/48 dB/oct, 4 = 6 dB/oct (appended).
inline constexpr int EqSlope6 = 4;
// EqOrder choices: gentle, normal, steep x2, x3, x4 (peak/shelf/band types).
enum EqOrderChoice { EqOrderGentle, EqOrderNormal, EqOrderSteep2, EqOrderSteep3, EqOrderSteep4 };

// "gl.filter"
enum Filter {
  FltMode, FltCutoff, FltReso, FltMix,
  FltSlope, FltGain, FltBlend, FltLPLevel, FltBPLevel, FltHPLevel, FltDcBlock,  // 0.5
  // J-26: classic multi-tap state-variable model (FltModel = 1, Fruity Filter reference): knob
  // positions for cutoff / resonance, FltLP/BP/HPLevel = level knobs, FltSlope = second stage,
  // FltDcBlock = DC remover (engine/FlFilterRef.h).
  FltModel, FltCutK, FltResK,
  FltCount
};
// FltMode choices: 0 LP, 1 BP, 2 HP, 3 notch, then (0.5) 4 low shelf, 5 peak, 6 high shelf.
// "gl.comp"
enum Comp {
  CmpThreshold, CmpRatio, CmpAttack, CmpRelease, CmpKnee, CmpMakeup,
  // 0.5: detector, character, lookahead, parallel mix, output trim, detector high-pass
  CmpDetector, CmpCharacter, CmpLookahead, CmpMix, CmpOutput, CmpKeyHpf,
  // κ2: fast program-dependent release ("/R" types), output safety limiter (0 dBFS, 64-sample
  // look-ahead), detector input (own / sidechain)
  CmpFastRelease, CmpLimiter, CmpSidechain,
  CmpCount
};
// "gl.limiter"
enum Limiter {
  LimGain, LimCeiling, LimRelease,
  // 0.5: limiter envelope, saturation, RMS compressor stage, gate stage, options
  LimAttack, LimHold, LimRelCurve, LimAtkCurve, LimSaturation,
  LimCompThresh, LimCompRatio, LimCompKnee, LimCompAttack, LimCompRelease, LimCompRms, LimCompCurve,
  LimGateThresh, LimGateFloor, LimGateRelease, LimSafety, LimView,
  LimSidechain,  // 0.5 FL import: compressor detector follows the track's sidechain input
  LimCompShape,  // κ2: compressor curve shape -1..1 (0 = straight; > 0 bends down, < 0 bends up)
  LimCount
};
// "gl.gate"
enum Gate {
  GtThreshold, GtAttack, GtHold, GtRelease, GtRange,
  GtHysteresis, GtLookahead, GtKeyHpf, GtKeyLpf, GtFlip,  // 0.5
  GtCount
};
// "gl.delay"
enum Delay {
  DlySync, DlyTime, DlyTimeMs, DlyFeedback, DlyMode, DlyOffset, DlyCutoff, DlyLowCut,
  DlyModRate, DlyModDepth, DlySaturation, DlyDry, DlyWet,
  // 0.5: resonant feedback filter, diffusion, loop distortion, lo-fi, time glide, modulation, output
  DlyFilterType, DlyResonance, DlyDiffusion, DlyDiffSpread,
  DlyDistType, DlyDistThresh, DlyDistHardness, DlyDistSymmetry, DlyBits, DlyRateReduce,
  DlyKeepPitch, DlyTimeSmooth, DlyModCutoff, DlyStereoSpread, DlyTone, DlyInput, DlyInputPan,
  // κ1: reference delay model (DlyModel = 1 for new instances): stereo type,
  // synced time in 1/192-beat ticks, R/L time offset, spread, modulation rate,
  // feedback filter mode + cutoff, rate reduction. The input runs through the
  // feedback chain (filter, crush, distortion, diffusion) like the echoes.
  DlyModel, DlyType3, DlyTicks, DlyTimeR, DlySpread3, DlyModRate3, DlyFilter3, DlyCutoff3, DlyRate3,
  // J-26: classic tempo delay (DlyModel = 2, engine/ClassicDelay.h): DlyTicks = time; own tempo
  // (0 = host), whole-sixteenth rounding, feedback filter coefficient (× 44.1 kHz / sr when scaled),
  // linear feedback, output mode, input-side offset (ms, < 0 = left later), input gain + fold pan, dry.
  DlyClsStepRound, DlyClsBpm, DlyClsCoef, DlyClsScaleSr, DlyClsFb, DlyClsMode, DlyClsOffset, DlyClsIn, DlyClsPan, DlyClsDry,
  // J-35: stereo feedback delay reference (DlyModel = 3, engine/Delay3Ref.h): the reference's 26
  // controls, normalised 0..1, in its own order (d3ref::Param).
  DlyD3Raw0,
  DlyCount = DlyD3Raw0 + 26
};
// DlyMode choices: normal, ping-pong, invert, then (0.5) mono, off.
// "gl.reverb"
enum Reverb {
  RvbPreDelay, RvbSize, RvbDecay, RvbDamp, RvbLowCut, RvbHighCut, RvbWidth, RvbEarly, RvbDry, RvbWet,
  // 0.5: decay in seconds (RvbDecay is legacy), diffusion, bass multiplier, modulation, input mode,
  // tempo pre-delay, colour tilt, freeze
  RvbDecayTime, RvbDiffusion, RvbBassMult, RvbCrossover, RvbModDepth, RvbModRate, RvbInputMode, RvbPreSync, RvbColor, RvbFreeze,
  RvbDampHz,  // FL import: damping as a cutoff frequency (>= 22 kHz = off)
  // J-26: network model: 0 hall (comb network), 1 plate (Reeverb v1 reference: mono input,
  // low cut <= 20 Hz = off, high cut / damping >= 22100 Hz = off, size steps of 1 %, fixed 500 Hz bass crossover)
  RvbModel,
  // J-35: model 2 = the full reference reverb (FullReverbRef.h), driven by the standalone
  // reverb's own integer controls (low cut 19..3000 Hz (19 = off), high cut / damping 5..221
  // ×100 Hz (221 = off), pre-delay 0..384 (1/384 s or 1/192 beat), room 1..100, diffusion 0..100,
  // decay 1..200 ×0.1 s, bass 20..300 %, crossover 25..2000 Hz, separation -64..64, dry / ER /
  // wet 0..160, mod speed / depth 0..2048); RvbPreSync and RvbInputMode (mid / side) shared.
  RvbFLowCut, RvbFHighCut, RvbFPredelay, RvbFRoom, RvbFDiffusion, RvbFDecay, RvbFDamp, RvbFBass, RvbFCross, RvbFSep,
  RvbFDry, RvbFEr, RvbFWet, RvbFModSpeed, RvbFModDepth,
  RvbCount
};
// "gl.chorus"
enum Chorus {
  ChoRate, ChoDepth, ChoDelay, ChoMix,
  ChoVoices, ChoRate2, ChoRate3, ChoStereo, ChoShape, ChoCrossMode, ChoCrossFreq, ChoWetOnly, ChoFeedback,  // 0.5
  // κ3: per-LFO waves, absolute sweep depth, reference engine (3 voices, unipolar sweep, one-pole split)
  ChoWave1, ChoWave2, ChoWave3, ChoDepthMs, ChoClassic,
  ChoCount
};
// "gl.flanger"
enum Flanger {
  FlgDelay, FlgDepth, FlgRate, FlgFeedback, FlgMix,
  FlgPhase, FlgDamp, FlgShape, FlgInvertWet, FlgCross,  // 0.5
  // J-26 reference engine (FlgModel = 1; pre-model states keep the legacy engine): knob positions
  // (delay / depth / rate curves in engine/ClassicModFx.h), damp coefficient, feedback amount and
  // sign, separate dry / wet / cross gains (invert wet flips wet and cross). FlgPhase (degrees) and
  // FlgShape are shared.
  FlgModel, FlgDelayN, FlgDepthN, FlgRateN, FlgDampN, FlgFeedN, FlgInvFb, FlgDryN, FlgWetN, FlgCrossN,
  FlgCount
};
// "gl.phaser"
enum Phaser {
  PhsStages, PhsRate, PhsDepth, PhsCenter, PhsFeedback, PhsMix,
  PhsRangeMode, PhsMinFreq, PhsMaxFreq, PhsStereo, PhsOutGain, PhsShape, PhsManual,  // 0.5
  // J-26 reference engine (PhsModel = 1; pre-model states keep the legacy engine): knob positions
  // for sweep rate (+ small / large range), all-pass depths (coefficients), stereo, feedback,
  // dry fraction, output gain (2 n · √(1 − fb²)); stage count 3..23.
  PhsModel, PhsSweepN, PhsRangeLarge, PhsDepthMin, PhsDepthMax, PhsStereoN, PhsStagesN, PhsFeedbackN, PhsDryN, PhsGainN,
  PhsCount
};
// "gl.dist"
enum Dist {
  DstMode, DstDrive, DstTone, DstMix, DstOutput,
  DstPreBand, DstPreAmount, DstColor, DstOversample, DstDcRemove, DstThreshold, DstExtreme, DstSoftOut,  // 0.5
  DstCount
};
// "gl.bitcrush"
enum Bitcrush {
  BcBits, BcDownsample, BcMix,
  BcFilterOn, BcFilterType, BcFilterFreq, BcFilterReso, BcFilterPos, BcOutput, BcPunch, BcPreserve, BcImpact, BcSquarize,  // 0.5
  BcCount
};
// "gl.clipper"
enum Clipper { ClpThreshold, ClpPostGain, ClpCeilingMode, ClpOversample, ClpInput, ClpMix, ClpCount };
// "gl.stereo"
enum Stereo {
  StWidth, StMid, StSide, StHaas, StPan,
  StLL, StRR, StRL, StLR, StInvert, StDephase, StFxPos, StMonoBelow, StVolume,  // 0.5: matrix, invert, dephase, mono bass, volume
  StSep,  // κ3: stereo separation -1 (wider) .. 1 (merged); reference engine: s = 0.75 x StSep
  // κ3 reference engine: volume knob (0..1.25 through 0.1 (11^x - 1)), phase-offset knob -1..1
  // (delay (1001^|k| - 1) / 2 ms, negative = left), separation matrix, pan without fold-in.
  StVolK, StPhaseK, StClassic,
  StCount
};
// "gl.transient"
enum Transient {
  TrAttack, TrSustain, TrOutput,
  TrAttackSpeed, TrReleaseSpeed, TrSplit, TrBalance, TrDrive, TrMix,  // 0.5
  TrEnabled,  // κ2: effect switch (the reference's on/off button)
  TrCount
};
// "gl.multiband" — three-band compressor
enum Multiband {
  MbLowXover, MbHighXover,
  MbLowThresh, MbLowRatio, MbLowGain,
  MbMidThresh, MbMidRatio, MbMidGain,
  MbHighThresh, MbHighRatio, MbHighGain,
  MbAttack, MbRelease,
  // 0.5: per band time scale, knee and state; output limiter and gain
  MbLowTime, MbMidTime, MbHighTime, MbLowKnee, MbMidKnee, MbHighKnee, MbLowState, MbMidState, MbHighState,
  MbLimiter, MbOutput,
  // κ2 (reference engine): per band attack / release / knee hardness, the mid
  // band's own edges, edge mode and band filter type
  MbLowAtk, MbMidAtk, MbHighAtk, MbLowRel, MbMidRel, MbHighRel, MbLowHard, MbMidHard, MbHighHard,
  MbMidLowEdge, MbMidHighEdge, MbEdgeMode, MbFilterType,
  MbCount
};
// MbEdgeMode: 0 linked (mid band edges = the two crossovers), 1 independent.
// MbFilterType: 0 Butterworth 24 dB/oct (IIR), 1 linear phase (FIR), 2 Linkwitz-Riley (flat sum).
// MbLowState..: 0 compress, 1 bypass (uncompressed), 2 mute.
// "gl.analyzer" (pass-through, display only)
enum Analyzer { AnzMode, AnzGain, AnzStereo, AnzPeaks, AnzRange, AnzSlope, AnzSmoothing, AnzHold, AnzCount };
// AnzMode choices: spectrum, oscilloscope, both, then (0.5) sonogram, meter, stereo.
// "gl.geq" — seven-band graphic EQ
inline constexpr int kGeqBands = 7;
inline constexpr double kGeqFreqs[kGeqBands] = {63, 160, 400, 1000, 2500, 6300, 16000};
inline constexpr int GeqOutput = kGeqBands;
inline constexpr int GeqLayout = kGeqBands + 1;     // 0.5: band centre table (wide / classic)
inline constexpr int GeqBandwidth = kGeqBands + 2;  // peak bandwidth in octaves
inline constexpr int GeqMix = kGeqBands + 3;        // -1..1 (negative inverts the EQ difference)
inline constexpr int GeqCount = kGeqBands + 4;
inline constexpr double kGeqFreqsClassic[kGeqBands] = {63, 250, 500, 1500, 3000, 5000, 8000};
// "gl.waveshaper" — 17-point transfer curve (input -1..1 → output)
inline constexpr int kShapePoints = 17;
inline constexpr int WsPreGain = kShapePoints, WsPostGain = kShapePoints + 1, WsMix = kShapePoints + 2, WsBipolar = kShapePoints + 3;
inline constexpr int WsCenter = kShapePoints + 4, WsOversample = kShapePoints + 5, WsInterp = kShapePoints + 6;  // 0.5
// κ3: reference gain knobs (positions: pre 0.5..1.5, post 0..1), engine switch, HQ, smoothing.
// With a "WS1" curve in PluginState::data the free points replace the 17 points.
inline constexpr int WsPreAmp = kShapePoints + 7, WsPost = kShapePoints + 8, WsClassic = kShapePoints + 9;
inline constexpr int WsHq = kShapePoints + 10, WsSmooth = kShapePoints + 11;
inline constexpr int WsCount = kShapePoints + 12;
// "gl.autopan"
enum AutoPan {
  ApShape, ApSync, ApRate, ApRateHz, ApPan, ApVolume, ApPhase, ApStaticPan, ApStaticVol,
  // κ3 reference engine: one LFO target (off / pan / volume) and amount, speed knob position,
  // volume knob through 0.1 (11^x - 1), pan law with the far side folded in.
  ApTarget, ApAmount, ApSpeedK, ApClassic,
  ApCount
};
// "gl.shaper" — synced volume + filter envelopes (16 points each)
inline constexpr int kShaperPoints = 16;
inline constexpr int ShVol = 0;                         // 16 volume points 0..1
inline constexpr int ShCut = kShaperPoints;             // 16 cutoff points 0..1
inline constexpr int ShLength = kShaperPoints * 2;      // envelope length choice
inline constexpr int ShFilter = ShLength + 1, ShCutMin = ShLength + 2, ShCutMax = ShLength + 3, ShReso = ShLength + 4;
inline constexpr int ShSmooth = ShLength + 5, ShMix = ShLength + 6;
// 0.5 additions
inline constexpr int ShTension = ShLength + 7, ShCutDepth = ShLength + 8;
inline constexpr int ShPan = ShLength + 9;  // 16 pan points -1..1
inline constexpr int ShDrive = ShPan + kShaperPoints, ShDriveMix = ShDrive + 1, ShSlope = ShDrive + 2, ShShelfGain = ShDrive + 3;
inline constexpr int ShFollow = ShDrive + 4, ShFollowAtt = ShDrive + 5, ShFollowRel = ShDrive + 6;
inline constexpr int ShLfoAmt = ShDrive + 7, ShLfoRate = ShDrive + 8, ShLfoShape = ShDrive + 9;
inline constexpr int ShInput = ShDrive + 10, ShOutput = ShDrive + 11;
inline constexpr int ShCount = ShDrive + 12;
// ShFilter choices: 0 off, 1 LP, 2 BP, 3 HP, then (0.5) 4 notch, 5 low shelf, 6 peak, 7 high shelf, 8 all-pass.
// "gl.timegate" — stutter / half-speed / tape-stop / reverse + 16-step gate
inline constexpr int kGateSteps = 16;
inline constexpr int TgMode = 0, TgGateOn = 1, TgGate = 2;  // TgGate .. TgGate+15
inline constexpr int TgSmooth = TgGate + kGateSteps, TgMix = TgSmooth + 1;
// 0.5: slot banks (envelopes live in PluginState::data), mixes, volume smoothing, hold, sync, declick.
inline constexpr int kTgSlots = 36;
inline constexpr int TgTimeSlot = TgSmooth + 2, TgVolSlot = TgSmooth + 3, TgTimeMix = TgSmooth + 4, TgVolMix = TgSmooth + 5;
inline constexpr int TgVolAttack = TgSmooth + 6, TgVolRelease = TgSmooth + 7, TgTension = TgSmooth + 8, TgHold = TgSmooth + 9;
inline constexpr int TgTrigSync = TgSmooth + 10, TgDeclick = TgSmooth + 11, TgFreeRun = TgSmooth + 12;
// κ3: 36-slot banks (kTgSlots), scratch offset, DC removal, attack compensation, HQ resampling, MIDI slot triggers.
inline constexpr int TgScratch = TgSmooth + 13, TgDcRemove = TgSmooth + 14, TgCompensate = TgSmooth + 15, TgHq = TgSmooth + 16;
inline constexpr int TgMidiTrigger = TgSmooth + 17;
// J-42 reference engine (GrossBeatRef.h): model switch and the stored attack / release / scratch.
inline constexpr int TgModel = TgSmooth + 18, TgRAttack = TgSmooth + 19, TgRRelease = TgSmooth + 20, TgRScratch = TgSmooth + 21;
inline constexpr int TgCount = TgSmooth + 22;
// "gl.maximizer" — three-band upward/downward compression + saturating ceiling
enum Maximizer {
  MxLowFreq, MxHighFreq, MxLowGain, MxMidGain, MxHighGain, MxDrive, MxRelease, MxCeiling, MxLowCut,
  // 0.5: per band compressors (low, mid, high), crossover slope, lookahead, band mix, master envelope, width, amount
  MxLowThresh, MxMidThresh, MxHighThresh, MxLowRatio, MxMidRatio, MxHighRatio,
  MxLowAttack, MxMidAttack, MxHighAttack, MxLowRel, MxMidRel, MxHighRel,
  MxLowMode, MxMidMode, MxHighMode, MxLowWidth, MxMidWidth, MxHighWidth, MxLowSat, MxMidSat, MxHighSat,
  MxSlope, MxLookahead, MxBandMix, MxAttack, MxRelease2, MxRelCurve, MxWidth, MxAmount, MxSolo, MxStage,
  MxCount
};
// "gl.freqshift"
enum FreqShift {
  FsShift, FsFeedback, FsStereo, FsMix,
  FsType, FsRange, FsShiftWide, FsShiftBeats, FsWidth, FsStartPhase, FsShapeL, FsShapeR,  // 0.5
  // J-26 reference model (FsModel = 1; pre-model states keep the legacy engine): knob positions
  // 0..1 for shift (0.5 = none; law by type / range), dry-wet, L/R phase, feedback (0.5 = none),
  // start phase (0.5 = 0°). FsShapeL/R = sideband blend, FsWidth = stereo; FsRange 0 narrow
  // (±200 Hz), 1 wide (±20 kHz), 2 tempo.
  FsModel, FsFreqPos, FsMixRef, FsLrPhase, FsFeedbackRef, FsStartRef,
  FsCount
};
// "gl.lowlift"
enum LowLift {
  LlCrossover, LlLowGain, LlHarmonics, LlHighGain, LlLowCut, LlMix,
  LlDrive, LlSlope, LlInvertHarm, LlDiffuse, LlDiffuseAmt, LlLowOn, LlHighOn, LlMultiband, LlBoost, LlBoostFreq,  // 0.5
  // J-26 reference engine (LlModel = 1; pre-model states keep the legacy engine, engine/LowLifterRef.h):
  // enable, look-ahead, FL-unit gains (low / high ≤ -120 dB = off), harmonic slope, pre boost,
  // crossover / low cut (20..120 Hz), diffuser coefficient (±0.99). Shared: LlMix, LlDiffuse (mix),
  // LlInvertHarm, LlMultiband, LlHighOn, LlLowOn.
  LlModel, LlOn, LlLookahead, LlLowDb, LlSlopeDb, LlPreDb, LlHighDb, LlXoverHz, LlLowCutHz, LlDiffCoef,
  LlCount
};
// "gl.tuner" (pass-through, pitch display)
enum Tuner { TnReference, TnReactivity, TnBassMode, TnDisplay, TnAverage /* J-42: display averaging */, TnCount };
// "gl.utility"
enum Utility {
  UtlGain, UtlPan, UtlWidth, UtlInvert, UtlSwap,
  UtlInvertMode, UtlMute, UtlMuteChannel, UtlDcRemove, UtlPanLaw, UtlMono,  // 0.5
  UtlCount
};

// Delay time choices (beats) for "gl.delay" DlyTime.
inline constexpr double kDelayBeats[] = {0.125, 1.0 / 6, 0.1875, 0.25, 1.0 / 3, 0.375, 0.5, 2.0 / 3, 0.75, 1.0, 1.5, 2.0,
                                        0.0625, 4.0 / 3, 3.0, 4.0};  // 0.5 appended: 1/64, 1/2T, 1/2., 1 bar
inline constexpr int kDelayBeatCount = 16;

}  // namespace gl::pp
