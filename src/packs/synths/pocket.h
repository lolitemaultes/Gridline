// Gridline — "synths" pack, J-33 group "pocket": parameter layout of
//   gl.pocketsynth "Pocket Synth"  compact performance synth: ten oscillator
//                  kinds over one band-limited wavetable (saw pairs, pulse,
//                  square pairs, supersaw, bell, spreads), noise, a 4-pole
//                  ladder low-pass with its envelope, amp envelope, one
//                  tempo-locked LFO, poly / mono / lead voicing with glide, and
//                  an effect bus (drive + decimator, tempo delay, phaser,
//                  chorus).
// Indices 0..42 are the reference instrument's parameter indices, values its
// 0..1 positions (see FlpJ33.cpp); the laws are in pocket_dsp.cpp.
#pragma once

namespace gl::packs::synths::pocket {

enum PocketParam {
  PkVoiceMode, PkTranspose, PkSlide, PkWheelDest, PkWheelAmount, PkOscKind, PkOscMod, PkNoise,
  PkCutoff, PkPeak, PkKeyTrack, PkVelTrack, PkFltA, PkFltD, PkFltS, PkFltR, PkFltAmount,
  PkLfoShape, PkLfoDest, PkLfoRate, PkLfoAmount,
  PkAmpA, PkAmpD, PkAmpS, PkAmpR,
  PkDist, PkDrive, PkDecimate,
  PkDelay, PkDelayTime, PkDelayFeedback, PkDelayFilter, PkDelayMix,
  PkChorus, PkChorusRate, PkChorusDepth, PkChorusMix,
  PkPhaser, PkPhaserRate, PkPhaserWidth, PkPhaserFeedback, PkPhaserMix, PkPhaserFreq,
  PkCount
};
static_assert(PkCount == 43, "reference layout");

}  // namespace gl::packs::synths::pocket
