// Gridline — "models" pack, J-22 group "metal": parameter layouts and drawn
// data shared by the catalogue, the runtimes, the editors and the importer.
//   gl.alloy        Alloy          — metallic additive timbre generator: seeded
//                                    harmonic timbres (two seeds + morph), pre-decay /
//                                    decay / release per harmonic with timbre EQ
//                                    curves, 13 articulators, filter, unison, chorus,
//                                    delay, reverb, 3-band mix EQ.
//   gl.spectramorph Spectra Morph  — plays four resynthesised harmonic programs
//                                    (spectral frames over time) morphed on X / Y.
//   gl.grainscan    Grain Scanner  — grain player on the channel sample: grain
//                                    attack / hold / spacing, wave spacing (speed),
//                                    wobble, randomness, stereo, transient mode.
//   gl.splinescratch Spline Scratcher — per-key drawn position / volume splines and
//                                    mutes over the channel sample (scratching).
// Parameters of the engines are stored in the reference control order (index =
// reference tag) and in the reference control units, so imported states and
// automation map 1:1.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "harmor.h"

namespace gl::packs::models::metal {

// =================================================================== Alloy
enum AlloyParam {
  // reference control tags 0..48 (stored units)
  AlVolume,        // 0..256 (128 = unity)
  AlCoarse,        // -24..24 st
  AlFine,          // -50..50 ct
  AlModX, AlModY,  // 0..512
  AlPreDecay,      // 0..1000
  AlDecay,         // 0..10000 ms
  AlRelease,       // 0..10000 ms
  AlFullness,      // -1000..1000
  AlMorph,         // 0..1000 (seed 1 → seed 2 / randomness)
  AlSeed1, AlSeed2,  // 0..9999
  AlUniOrder,      // 1..9
  AlUniPan,        // 0..128
  AlUniVol,        // 16..128
  AlUniPitch,      // 0..256
  AlUniPhase,      // 0..128
  AlCutoff,        // 0..256
  AlReso,          // 0..256
  AlChOrder,       // 0..9 (0 = off)
  AlChDepth, AlChSpeed,  // 0..1024
  AlChDelay, AlChSpread,  // 0..128
  AlChCross, AlChMix,     // -128..128
  AlDlFeedback,    // 0..127
  AlDlTime,        // 0..384 (ticks: 96 per beat when tempo, else 1/96 s… see dsp)
  AlDlOffset,      // -512..512
  AlDlVol,         // -128..128
  AlDlMode,        // 0..3 feedback mode
  AlRvLowCut,      // 19..3000 Hz
  AlRvHighCut,     // 5..221 (×100 Hz)
  AlRvPredelay,    // 0..384
  AlRvRoom,        // 1..100
  AlRvDiffusion,   // 0..100
  AlRvDecay,       // 1..200 (×0.1 s)
  AlRvDamp,        // 5..221
  AlRvColor,       // 0..4
  AlRvWet,         // -128..128
  AlEqLevel1, AlEqLevel2, AlEqLevel3,  // -1800..1800 (×0.01 dB)
  AlEqFreq1, AlEqFreq2, AlEqFreq3,     // 0..65536
  AlEqWidth1, AlEqWidth2, AlEqWidth3,  // 0..65536
  // ---- untagged controls and options (stored in the state's option block)
  AlDust,          // 0..1000 magic dust (enhancer)
  AlRichness,      // 1..11
  AlComb,          // 0..7 harmonic comb
  AlFltType,       // filter type
  AlFltMode,       // filter mode / order
  AlFltFlat,       // alternate resonance / bandwidth / mode
  AlDeclick, AlAltDecay,
  AlRvOn, AlRvTempo, AlDlOn, AlDlTempo, AlModSmooth,
  AlEqType1, AlEqType2, AlEqType3,  // 0 low shelf, 1 peak, 2 high shelf
  AlPorta, AlPortaVar, AlPortaLimit,  // portamento on, variable time, limit (0 none .. 6)
  AlKeyPitch, AlKeySeed, AlOneVoicePerKey,
  AlVelVol, AlVelAttTime, AlVelAttLen,
  AlQuality,       // 0 draft .. 4
  AlRelLink,       // link timbre release to volume decay
  AlNatScale, AlNatWeight, AlAutoLoud,
  // articulator knobs of the five articulation targets (reference automation
  // indices 49..88): envelope attack / decay / release scales (128 = 1x),
  // sustain offset (-128..128), LFO speed (0..65536), tension / skew / width (-128..128)
  AlArtKnobs,
  AlCount = AlArtKnobs + 5 * 8
};
enum AlloyKnob { AkAttack, AkDecay, AkSustain, AkRelease, AkLfoSpeed, AkLfoTension, AkLfoSkew, AkLfoWidth, AkCount };
inline constexpr int alKnob(int art, int k) { return AlArtKnobs + art * AkCount + k; }

enum AlloyArt {
  AaVolume, AaCutoff, AaReso, AaPitch, AaUniPitch,  // articulation targets
  AaUniPhase, AaPreDecay, AaDecay, AaRelease, AaFullness, AaMorph, AaRotation, AaEqShift,  // mapping targets
  AaCount
};
enum AlloyEq { AeForced, AeImpulse, AeDecay, AeRelease, AeCount };
enum AlloyMap { AmKey, AmVel, AmX, AmY, AmCount };
inline const char* const kAlloyArtNames[AaCount] = {"Volume", "Filter frequency", "Filter resonance", "Pitch", "Unison pitch",
                                                   "Unison phase", "Timbre pre-decay", "Timbre decay time", "Timbre release time",
                                                   "Timbre fullness", "Timbre morphing", "Timbre rotation", "Timbre EQ shifting"};
inline const char* const kAlloyEqNames[AeCount] = {"Forced harmonics", "Impulse EQ", "Decay EQ", "Release EQ"};
// Articulators 0..4 have an envelope and an LFO; all have key / velocity / X / Y maps.
inline constexpr int kAlloyEnvArts = 5;

struct AlloyArtData {
  bool bipolar = false;
  hm::Curve env, lfo;
  float att = 1, dec = 1, sus = 0, rel = 1;  // envelope time scales / sustain offset
  float lfoSpeed = 1, lfoTension = 0, lfoSkew = 0, lfoPw = 0, lfoPhase = 0;
  std::array<hm::Curve, AmCount> maps;  // empty = neutral
};
struct AlloyData {
  std::array<AlloyArtData, AaCount> arts;
  std::array<hm::Curve, AeCount> eq;  // empty = neutral (x 0..1 across the harmonics)
  std::string info;                   // patch notes
  AlloyData();
};
bool alloyArtBipolar(int art);
float alloyEqNeutral(int eq);
bool parseAlloy(const std::string& text, AlloyData& out);
std::string serializeAlloy(const AlloyData& d);

// ============================================================ Spectra Morph
inline constexpr int kSmPrograms = 4;      // the four X / Y corners
inline constexpr int kSmMaxPartials = 256;
struct SmFrame {
  float level = 1;          // frame gain
  int hold = 1;             // duration in analysis steps
  std::vector<float> amp;   // harmonic amplitudes (index 0 = fundamental)
};
struct SmZone {
  int key = 60;             // analysis root key of this zone
  float gain = 1;
  std::vector<SmFrame> frames;
  int loopStart = -1, loopEnd = -1;  // sustain loop (analysis steps), -1 = none
};
struct SmProgram {
  std::vector<SmZone> zones;  // empty = silent corner
};
struct SmData {
  std::array<SmProgram, kSmPrograms> prog;
  std::string name;
};
inline constexpr float kSmStepSec = 0.01f;  // one analysis step
bool parseMorph(const std::string& text, SmData& out);
SmData factoryMorph();  // built-in corner programs of a fresh instance
std::string serializeMorph(const SmData& d);

enum SpectraMorphParam {
  SmVolume, SmTranspose, SmFine, SmGlide, SmVelSens,
  SmMorphX, SmMorphY, SmMorphRate, SmMorphMode,  // XY position, motion rate, mode (static / circle / path)
  SmSpeed, SmLoop,                               // frame playback speed, loop mode
  SmAttack, SmDecay, SmSustain, SmRelease,
  SmNoise, SmNoiseColor,
  SmChorusOn, SmChorusDepth, SmChorusRate, SmDelayOn, SmDelayTime, SmDelayFb, SmReverbOn, SmReverbDecay, SmReverbMix,
  SmDrive, SmVoices,
  SmCount
};

// ============================================================ Grain Scanner
enum GrainScanParam {
  GsAttack,       // 50..500
  GsHold,         // 0..500
  GsGrainSpace,   // 125..1000
  GsWaveSpace,    // -1000..1000
  GsFxDepth,      // 0..384
  GsFxSpeed,      // 4..200
  GsRandom,       // 0..256
  GsStereo,       // 0..64
  GsLoop,         // toggle
  GsHoldOn,       // toggle (hold sound)
  GsStart,        // 0..4096
  GsTransHold,    // 0..500 (tags 0..11 are the automatable controls)
  GsTransSource,  // transient source (stored after the tags)
  GsTrigger,      // key to: pitch / percent / step / transient
  GsTempoLock,    // toggle
  // channel envelope (the reference instrument uses its channel's volume envelope)
  GsEnvOn, GsEnvAttack, GsEnvHold, GsEnvDecay, GsEnvSustain, GsEnvRelease,
  GsVolume, GsRoot,
  GsCount
};

// ======================================================== Spline Scratcher
inline constexpr int kSsKeys = 120;
enum SsCurve { SsHalfCosine, SsLinear, SsBezier, SsTension, SsQuarterSine, SsQuarterCosine, SsCurveCount };
struct SsPoint {
  float x = 0, y = 0;   // time 0..1 over the patch length, value 0..1
  float tension = 0.5f;
  uint8_t curve = SsLinear;  // segment shape towards the next point
  bool locked = false;
};
struct SsPatch {
  bool used = false;            // edited / stored patch
  float beats = 4;              // length in beats
  float speed = 1, attack = 1, release = 1;  // per-patch scales (reference floats)
  float start = 0, end = 1;     // sample window 0..1
  std::string name;
  std::vector<std::pair<bool, float>> mutes;  // (sound on, x) toggles
  std::vector<SsPoint> pos, vol;
};
struct SsData {
  std::string path;
  int current = 60;  // patch shown in the editor
  std::array<SsPatch, kSsKeys> keys;
};
SsPatch ssDefaultPatch();
bool parseScratchSpl(const std::string& text, SsData& out);
std::string serializeScratchSpl(const SsData& d);
float ssEval(const std::vector<SsPoint>& pts, float x);
bool ssSoundOn(const SsPatch& p, float x);

enum SplineScratchParam {
  SsSpeed,        // tag 5: 0..1024 (512 = 1×)
  SsAttack,       // tag 6: 0..100
  SsRelease,      // tag 7: 0..5000
  SsStartWave,    // tag 8: 0..10000
  SsEndWave,      // tag 9: 0..10000
  SsSplineMode,   // tag 17: position / speed
  SsVolume, SsContinuous,
  SsCount
};

}  // namespace gl::packs::models::metal
