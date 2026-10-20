// Gridline — "models" pack, J-22 group "drums": parameter layouts shared by the
// catalogue, the runtimes, the editors and the FL import.
//   gl.membranepad  "Membrane Pad"   one physically-modelled drum (mallet,
//                                    membrane, LOW / SOF / MID / HIGH bands,
//                                    lo-fi, hold, pitch, 4 velocity routes),
//                                    keyboard-pitched.
//   gl.membranekit  "Membrane Kit"   16 channels of the same drum model with
//                                    volume / pan / mute / solo / key, master
//                                    level, swing, 8-band EQ, limiter and a
//                                    16-pattern step sequencer started by keys.
//   gl.kicksculpt   "Kick Sculptor"  sine-sweep kick with sub, drive, attack,
//                                    variation, click and a sampled noise layer.
//   gl.sweepkick    "Sweep Kick"     minimal pitch-sweep kick (max / min
//                                    frequency, sweep and amp decay, click,
//                                    distortion, random seed).
// Parameter indices follow the reference instruments' control tags so that
// automation maps 1:1.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::packs::models::dr {

// ------------------------------------------------------------ drum model
enum MdParam {
  MdMalletRetrig, MdRetrig, MdMalletAmp, MdMalletDecay, MdMalletNoise, MdMalletNoiseRp,
  MdMemDecay, MdMemCutoff, MdTension, MdPhase, MdMaterial, MdSize, MdShape,
  MdLowFreq, MdLowReso, MdSofFreq, MdSofDecay, MdSofLevel,
  MdMidFreq, MdMidLevel, MdMidRes, MdMidFreqMod, MdMidLevelMod, MdMidAttack,
  MdHighFreq, MdHighLevel, MdHighRes, MdLofi, MdHold, MdPitch,
  MdVelSrc1, MdVelSrc2, MdVelSrc3, MdVelSrc4, MdVelAmt1, MdVelAmt2, MdVelAmt3, MdVelAmt4,
  MdCount
};
// Velocity-route destinations (index 0 = none, k = model parameter k + 1 … see kVelDest).
int velDestCount();
const char* velDestName(int k);
// Model parameter a velocity route destination index drives (-1 = none).
int velDestParam(int k);

// ------------------------------------------------------------ Membrane Pad
// PdOutput: the stored output level after the model; PdEngine (J-33): 0 =
// Gridline's modal model (J-22), 1 = the reference comb-bank engine (drums_ref.h).
enum PadParam { PdModel = 0, PdOutput = MdCount, PdEngine, PdCount };

// ------------------------------------------------------------ Membrane Kit
inline constexpr int kKitPads = 16;
inline constexpr int kKitStride = 44;
enum KitPadField { KpVolume, KpPan, KpMute, KpSolo, KpKey, KpModel, KpSpare = KpModel + MdCount, KpCount };
static_assert(KpCount == kKitStride, "kit stride");
inline constexpr int kitPad(int pad, int field) { return pad * kKitStride + field; }
inline constexpr int kKitMaster = kKitPads * kKitStride;  // 704
enum KitMaster {
  KmReserved = kKitMaster, KmLevel, KmSwing, KmEqOn, KmEq1, KmEq2, KmEq3, KmEq4, KmEq5, KmEq6, KmEq7, KmEq8,
  KmLimiter, KmLimGain, KmLimRelease, KmReserved2, KmCount
};
inline constexpr int kKitPatterns = 16;
inline constexpr int kKitSteps = 64;  // cells per channel and pattern
// Kit pattern / name data (PluginState::data, text, see encodeKitData).
struct KitPattern {
  int length = 0;    // steps (0 = unused)
  int key = -1;      // MIDI key that starts it (-1 = none)
  uint8_t cell[kKitPads][kKitSteps] = {};  // bit 7 = step on, bits 0..6 = velocity
};
struct KitData {
  std::string kitName;
  std::string padName[kKitPads];
  KitPattern pattern[kKitPatterns];
  uint8_t link[32] = {};  // channel link flags (stored, shown; 16 used)
};
std::string encodeKitData(const KitData& d);
bool decodeKitData(const std::string& s, KitData& d);

// ------------------------------------------------------------ Kick Sculptor
enum KickParam {
  KsLevel, KsSampling, KsDuration, KsSub, KsDrive, KsAttack, KsBase, KsPeak, KsSlide,
  KsVarBase, KsVarPeak, KsVarSlide, KsVarPhase, KsClickCut, KsClickFreq, KsClickLevel,
  KsNoiseA, KsNoiseD, KsNoiseS, KsNoiseStart, KsNoisePitch, KsNoiseMix, KsNoiseFilter,
  KsNoiseSample,  // choice: the reference instrument's noise samples (read from its install)
  KsCount
};
int kickNoiseCount();
const char* kickNoiseName(int k);  // file stem, e.g. "Metallic"
// Absolute path of noise sample k inside the reference install ("" when absent).
std::string kickNoisePath(int k);
// Noise index of a stored path (by file name), -1 when unknown.
int kickNoiseIndex(const std::string& path);

// ------------------------------------------------------------ Sweep Kick
enum SweepParam { SkMaxFreq, SkMinFreq, SkFreqDecay, SkAmpDecay, SkClick, SkDist, SkSeed, SkCount };

}  // namespace gl::packs::models::dr
