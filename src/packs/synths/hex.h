// Gridline — "synths" pack, J-33 group "hex": parameter layout of
//   gl.hexsynth "Hex Synth"  six additive-wavetable oscillators per voice
//                            (128 harmonics, band-limited per pitch) in a
//                            6 × 6 phase-modulation matrix, each with its own
//                            init → attack → decay → sustain → release
//                            envelope, unison stacks, a 4-pole low / band /
//                            high-pass filter with its envelope, two LFOs, an
//                            8-slot modulation matrix, a chord-following step
//                            sequencer, drive, two effect slots (delay, chorus,
//                            plate reverb, flanger, phaser, lo-fi) and an
//                            8-band EQ.
// Indices 0..339 are the reference instrument's parameter indices (values as
// the reference stores them: 0..1 positions, a few raw integers from 294 on)
// so that recorded automation maps one-to-one (see FlpJ33Hex.cpp). The value
// laws live in hex_laws.h, shared by the runtime, the editor and the decoder.
#pragma once
#include <cstdint>
#include <string>

namespace gl::packs::synths::hex {

inline constexpr int kOscs = 6;
inline constexpr int kSlots = 8;       // wave slots: oscillators 1..6, LFO 1, LFO 2
inline constexpr int kHarmonics = 128;
inline constexpr int kModSlots = 8;
inline constexpr int kSeqSteps = 64;
inline constexpr int kEqBands = 8;
inline constexpr int kAutomatable = 294;  // parameters 0..293 take modulation / controller links
inline constexpr int kWaves = 33;         // wave selector 0 (custom) .. 32
inline constexpr int kModSources = 7 + 128;

enum HexParam {
  HxGain, HxDrive, HxTranspose, HxAmpA, HxAmpD, HxAmpS, HxAmpR, HxVelCurve, HxUniDetune, HxUniPan, HxGlide,
  HxEnvToPitch, HxLfo2ToPitch, HxLfo1Speed, HxLfo2Speed, HxCutoff, HxReso, HxKeyTrack, HxVelTrack, HxFltA, HxFltD,
  HxFltS, HxFltR, HxFltEnvAmt, HxFltLfo1,
  HxOscOn = 25,      // + osc (6 each from here)
  HxCoarse = 31,
  HxFine = 37,
  HxShift = 43,
  HxFreeRun = 49,
  HxPhase = 55,
  HxVelSens = 61,
  HxInit = 67,
  HxOscA = 73,
  HxOscD = 79,
  HxOscS = 85,
  HxOscR = 91,
  HxPan = 97,
  HxMix = 103,
  HxFm = 109,        // + 6 * (destination osc) + source osc
  HxLfo1Osc = 145,   // + osc
  HxLfo2Osc = 151,   // + osc
  HxSeqPlay = 157, HxSeqSwing, HxSeqPingPong, HxSeqRandom, HxSeqDual, HxSeqSteps,
  HxSeqStep = 163,   // + step (64)
  HxEqOn = 227,
  HxEqBand = 228,    // + band (8)
  HxFxOn = 236,      // + slot (2); every effect parameter below: + slot
  HxFlDelay = 238, HxFlFeedback = 240, HxFlDepth = 242, HxFlRate = 244, HxFlMix = 246, HxFlInvFeedback = 248,
  HxFlInvMix = 250,
  HxChDepth = 252, HxChRate = 254, HxChMix = 256,
  HxRvMix = 258, HxRvDecay = 260, HxRvDamp = 262, HxRvHighCut = 264,
  HxDlTime = 266, HxDlFeedback = 268, HxDlRate = 270, HxDlDepth = 272, HxDlBlur = 274, HxDlMix = 276,
  HxLfBits = 278, HxLfRate = 280, HxLfFilter = 282,
  HxPhFreq = 284, HxPhRate = 286, HxPhDepth = 288, HxPhFeedback = 290, HxPhMix = 292,
  HxUnused = 294,
  HxBendRange = 295,  // 0..1 position: int(12 x) semitones
  HxLfo1Sync = 296,   // raw 0..9
  HxLfo1Free = 297,   // raw 0 / 1
  HxLfo2Sync = 298,
  HxLfo2Free = 299,
  HxFilterType = 300,  // raw 0 off, 1 low, 2 band, 3 high
  HxVoices = 301,      // raw 0 = mono, 1..32
  HxUnison = 302,      // raw n → min(n + 1, 8) copies
  HxGlideMode = 303,   // raw 0..3
  HxFxSync = 304,      // + slot, raw 0..9
  HxWave = 306,        // + slot (8), raw 0..32
  HxFxType = 314,      // + slot, raw 0 delay, 1 chorus, 2 reverb, 3 flanger, 4 phaser, 5 lo-fi
  HxModSource = 316,   // + slot (8), raw code
  HxModTarget = 324,   // + slot, raw target + 1 (0 = none)
  HxModAmount = 332,   // + slot, 0..1 position
  HxCount = 340
};
static_assert(HxFltLfo1 == 24 && HxSeqSteps == 162 && HxPhMix == 292 && HxCount == 340, "reference layout");

enum FxKind { FxDelay, FxChorus, FxReverb, FxFlanger, FxPhaser, FxLoFi, FxKinds };
enum FilterKind { FtOff, FtLow, FtBand, FtHigh };

// PluginState::data of gl.hexsynth: custom wave spectra (per wave slot: 128
// harmonic amplitudes and 128 phases in cycles), the MIDI controller link of
// every automatable parameter (-1 = none) and the program name. Text lines:
//   "W<slot> a0 … a127 | p0 … p127", "L <param> <cc> …", "N <name>".
struct HexData {
  float amp[kSlots][kHarmonics] = {};
  float phase[kSlots][kHarmonics] = {};
  int8_t link[kAutomatable];
  std::string name;
  HexData() {
    for (auto& l : link) l = -1;
  }
};
std::string formatHexData(const HexData& d);
bool parseHexData(const std::string& text, HexData& d);

}  // namespace gl::packs::synths::hex
