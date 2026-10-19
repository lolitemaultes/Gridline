// Gridline — "models" pack: "Resynth" (gl.harmor), a two-part additive /
// resynthesis synth.
//
// Each part (A, B) is a bank of up to 256 sine partials per voice built from
// two timbre curves (blended), shaped over frequency by two harmonic-domain
// filters with resonance, a phaser, a pluck, harmonic blur / time blur,
// prism, a harmonizer, clipping and protection, sub-harmonics, tremolo,
// vibrato, unison (up to 9) and an image layer (a picture or a resynthesised
// audio file played as time × partial gains with per-partial pitch offsets).
// Every part has 43 articulators (envelope + LFO + 9 mapping curves:
// key, velocity, mod X / Y / Z, random, unison index, held index, X speed)
// and 26 shaping curves over the harmonic / frequency axis. A global block
// (mod X / Y / Z, legato / portamento, part mix, volumes, pitch) and an
// effects chain (distortion, chorus, delay, reverb, compressor, EQ curve)
// follow. Drawn data (curves, images) lives in PluginState::data (text).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gl::packs::models::hm {

inline constexpr int kParts = 2;
inline constexpr int kArts = 43;
inline constexpr int kMaps = 9;
inline constexpr int kShapes = 26;
inline constexpr int kHarm = 256;  // partial table length

// Articulation targets (storage order).
enum Art {
  ArPan, ArVol, ArFxMix, ArTimbreMix, ArF1Freq, ArF2Freq, ArF1Width, ArF2Width, ArF1Res, ArF2Res, ArF1ResWidth, ArF2ResWidth,
  ArF1ResOfs, ArF2ResOfs, ArFMix, ArPluck, ArPhMix, ArPhWidth, ArPhOfs, ArHzMix, ArHzWidth, ArClip, ArBlur, ArPrism, ArPitch,
  ArVibrato, ArUniPitch, ArImgTime, ArImgSpeed, ArImgFormant, ArImgFreqScale, ArF1Env, ArF2Env, ArGAttack, ArGAttScale,
  ArGDecScale, ArGSusOfs, ArGRelScale, ArGLfoAmt, ArGLfoSpeed, ArGLfoPhase, ArUniPhase, ArPredelay
};
enum MapKind { MkKey, MkVel, MkX, MkY, MkZ, MkRand, MkUni, MkHeld, MkXSpeed };
// Shaping curves (x = harmonic index or frequency, see shapeAxisHz).
enum Shape {
  ShLocalEq, ShPhase, ShTimbre1, ShTimbre2, ShRandom, ShProtect, ShClip, ShPrism, ShUniPitch, ShFormant, ShFilter1, ShFilter2,
  ShRes1, ShRes2, ShPluck, ShPhaser, ShHarmonizer, ShGlobalEq, ShMaskF1, ShMaskF2, ShMaskRes1, ShMaskRes2, ShMaskPhaser,
  ShMaskHarm, ShMaskBlur, ShMaskImage
};
inline const char* const kArtNames[kArts] = {
    "Pan", "Volume", "FX / dry", "Timbre mix", "Filter 1 freq", "Filter 2 freq", "Filter 1 width", "Filter 2 width",
    "Filter 1 reso", "Filter 2 reso", "F1 reso width", "F2 reso width", "F1 reso offset", "F2 reso offset", "Filter mix",
    "Pluck", "Phaser mix", "Phaser width", "Phaser offset", "Harmonizer mix", "Harmonizer width", "Clip threshold", "Blur",
    "Prism", "Pitch", "Vibrato", "Unison pitch", "Image time", "Image speed", "Image formant", "Image freq scale",
    "Filter 1 env", "Filter 2 env", "Env attack", "Env att scale", "Env dec scale", "Env sus offset", "Env rel scale",
    "LFO amount", "LFO speed", "LFO phase", "Unison phase", "Predelay"};
inline const char* const kMapNames[kMaps] = {"KEY", "VEL", "X", "Y", "Z", "RAND", "UNISON", "HELD", "X SPEED"};
inline const char* const kShapeNames[kShapes] = {
    "Local EQ", "Harmonic phase", "Timbre 1", "Timbre 2", "Randomness", "Protection", "Clipping", "Prism", "Unison pitch",
    "Formant", "Filter 1 shape", "Filter 2 shape", "Reso 1 shape", "Reso 2 shape", "Pluck", "Phaser shape", "Harmonizer",
    "Global EQ", "Mask filter 1", "Mask filter 2", "Mask reso 1", "Mask reso 2", "Mask phaser", "Mask harmonizer",
    "Mask blur", "Mask image"};

// Part parameters (stride PfCount; part p starts at partBase(p)).
enum PartField {
  PfEnabled, PfPhase, PfPhaseRand, PfTimbreMix, PfBlend, PfSub1, PfSub3, PfSub4, PfSubMode, PfProtect, PfClip, PfClipMode,
  PfFxMix, PfVolume, PfEnvVolume, PfAutoGain, PfVelAttTime, PfVelAttLen, PfVelRelLen,
  PfTremolo, PfTremHz, PfTremGap,
  PfBlurMix, PfTBlurAtt, PfTBlurDec, PfHBlur, PfHBlurLo, PfHBlurHi,
  PfPrism, PfPrismMul,
  PfHzMix, PfHzWidth, PfHzStrength, PfHzShift, PfHzShiftStep, PfHzGap, PfHzGapStep,
  PfUniOrder, PfUniType, PfUniAlt, PfUniPan, PfUniPitch, PfUniPhase,
  PfFreqMul, PfFreqDiv, PfDetMul, PfDetDiv, PfPitchArt, PfVibrato, PfVibHz, PfPortaTime, PfPortaLimit, PfPortaLog, PfPortaVar,
  PfF1Type, PfF1ResType, PfF1Cut, PfF1Width, PfF1Key, PfF1Env, PfF1Res, PfF1ResWidth, PfF1ResOfs, PfF1Osc, PfF1Adapt, PfF1ResAdapt,
  PfF2Type, PfF2ResType, PfF2Cut, PfF2Width, PfF2Key, PfF2Env, PfF2Res, PfF2ResWidth, PfF2ResOfs, PfF2Osc, PfF2Adapt, PfF2ResAdapt,
  PfFMix, PfPluck,
  PfPhType, PfPhMix, PfPhWidth, PfPhOfs, PfPhMotion, PfPhKey, PfPhScale,
  PfImgGainMix, PfImgGainScale, PfImgTime, PfImgTimeSmooth, PfImgSpeed, PfImgSpeedOct, PfImgSharp, PfImgFreqMode, PfImgFreqScale,
  PfImgFormant, PfImgFormantMix, PfImgLoop, PfPredelay, PfRelMul,
  PfCount
};
inline constexpr int kFltStride = PfF2Type - PfF1Type;
enum GlobalParam {
  GmModX = 2 * PfCount, GmModY, GmModZ, GmLegato, GmPorta, GmVelPorta, GmLegatoLimit, GmStrumMode, GmStrumTime, GmStrumTension,
  GmPartMix, GmVolume, GmPostVolume, GmVelVol, GmPitch, GmLfoAmt, GmMono, GmVoices,
  // effects
  FxDistType, FxDistDrive, FxDistAsym, FxDistOut, FxDistMix, FxDistTone,
  FxChOrder, FxChDepth, FxChSpeed, FxChDelay, FxChSpread, FxChCross, FxChMix,
  FxDlOn, FxDlMode, FxDlFeedback, FxDlTime, FxDlOffset, FxDlVol, FxDlPan, FxDlLowCut, FxDlHighCut, FxDlDamp,
  FxRvOn, FxRvLowCut, FxRvHighCut, FxRvPredelay, FxRvRoom, FxRvDiffusion, FxRvDecay, FxRvDamp, FxRvColor, FxRvMix,
  FxCompType, FxCompAmount, FxCompLow, FxCompMid, FxCompHigh, FxEqMix,
  HmParamCount
};
inline constexpr int partBase(int p) { return p * PfCount; }
inline constexpr int pf(int p, int f) { return p * PfCount + f; }
inline constexpr int flt(int p, int k, int f) { return p * PfCount + f + k * kFltStride; }  // f = PfF1… field

// Harmonic-domain filter shapes (same set as Additive's AdFilterType, then 2 custom curves).
inline constexpr int kFltCustom1 = 18, kFltCustom2 = 19, kFltShapes = 20;
inline constexpr int kResCustom1 = 10, kResCustom2 = 11, kResShapes = 12;  // 0..8 Additive's, 9 lowpass

// ------------------------------------------------------------------ data
struct Pt {
  float dt = 0, y = 0, tension = 0;
  uint8_t kind = 0;  // segment curve: 0 single, 1 double, 2 hold, 3 stairs, 4 smooth stairs, 5 pulse, 6 wave, 9 half sine, 10 smooth
};
struct Curve {
  std::vector<Pt> pts;
  int sustain = -1, loopStart = -1, decay = -1;
  bool enabled = false, tempo = false, global = false;
  bool empty() const { return pts.empty(); }
};
struct ArtData {
  bool bipolar = false;  // neutral value 0.5 (offsets) instead of 1 (factors)
  Curve env, lfo;
  float att = 1, dec = 1, sus = 0, rel = 1;            // envelope time scales / sustain offset
  float lfoSpeed = 0.5f, lfoSkew = 0, lfoPw = 0, lfoPhase = 0;  // cycles per second (per beat when tempo)
  float smooth = 0;                                    // output smoothing, seconds
  std::array<Curve, kMaps> maps;                       // empty = neutral
};
struct ImageData {
  int cols = 0, rows = 0, loop = 1;  // loop: 0 one-shot, 1 loop, 2 ping-pong
  float colSec = 0.01f;              // seconds per column at speed 1
  std::vector<uint8_t> gain;         // cols × rows, column-major
  std::vector<int8_t> freq;          // cols × rows or empty: pitch offset ×1/64 octave
  std::string source;                // audio / picture file it came from (re-analysed when gain is empty)
  bool empty() const { return gain.empty(); }
};
struct PartData {
  std::array<ArtData, kArts> arts;
  std::array<Curve, kShapes> shapes;  // empty = neutral
  ImageData image;
};
struct PatchData {
  std::array<PartData, kParts> part;
  PatchData();
};

// Neutral curve value of an articulator / shape.
float neutralShape(int shape);
bool artBipolarDefault(int art);
// Parses data text ("hmr 1"); false when it is not Resynth data.
bool parsePatch(const std::string& text, PatchData& out);
std::string serializePatch(const PatchData& d);

// Curve helpers. Value at x (cumulative dt) with segment kinds; `cyclic` wraps x.
double curveAt(const Curve& c, double x);
double curveLength(const Curve& c);
// Shaping-curve value for harmonic k (1-based) of `count` (x = (k-1)/(count-1)).
double shapeAtHarmonic(const Curve& c, int k, int count, float neutral);
// Frequency axis of the EQ curves: x 0..1 ↔ 20 Hz .. 20 kHz (log).
double shapeAtHz(const Curve& c, double hz, float neutral);

// Image building (UI thread). Audio: estimates the pitch and tracks `rows`
// harmonics every `hopSec`. Picture: rows = image rows (top = highest partial).
bool analyzeAudio(const std::vector<float>& mono, double sampleRate, int rows, double hopSec, ImageData& out, double* f0 = nullptr);
bool imageFromPicture(const std::string& path, int rows, ImageData& out);

}  // namespace gl::packs::models::hm
