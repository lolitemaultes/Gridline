// Gridline — "models" pack: parameter layouts shared by the catalogue, the
// runtimes and the editors.
//   gl.additive   Additive      — partial-bank synth (timbre editor, partial filter,
//                                 phaser, pluck, prism, blur, unison, morph)
//   gl.imagesynth Image Synth   — plays a picture as a spectrogram
//   gl.string     String Model  — waveguide strings (pluck / pick / hammer / bow),
//                                 body resonator, strummer, amp
//   gl.drumkit    Drum Model    — 16 modal drum voices (membrane / bar / plate / bell)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"

namespace gl::packs::models {

// Shared effect block (chorus, delay, reverb) used by Additive and String Model:
// six consecutive parameters starting at the plugin's Fx base index.
enum FxParam { FxChorus, FxDelay, FxDelayTime, FxDelayFeedback, FxReverb, FxRoomSize, FxCount };

// ------------------------------------------------------------------ Additive
inline constexpr int kAdDrawn = 32;    // drawable partial levels (timbre A)
inline constexpr int kAdMask = 12;     // repeating harmonic mask
inline constexpr int kAdMaxOsc = 256;  // partial oscillators per voice (incl. unison copies)
enum AdditiveParam {
  AdSpectrumA, AdSpectrumB, AdMorph, AdMorphEnv,
  AdPartials, AdTilt, AdOddEven, AdSub, AdStretch, AdScatter, AdSeed, AdPhaseRand, AdBlur, AdMaskMix,
  AdFltType, AdFltCutoff, AdFltWidth, AdFltReso, AdFltKey,
  AdFEnvAmt, AdFEnvAtt, AdFEnvDec, AdFEnvSus, AdFEnvRel,
  AdPhMix, AdPhDensity, AdPhOffset, AdPhSpeed,
  AdPluck, AdPluckTime,
  AdUniOrder, AdUniDetune, AdUniSpread,
  AdAttack, AdDecay, AdSustain, AdRelease,
  AdLfoShape, AdLfoRate, AdLfoPitch, AdLfoCutoff, AdLfoPhaser, AdLfoMorph,
  AdGlide, AdVoices, AdVolume,
  AdFx,                         // FxCount params
  AdDrawnBase = AdFx + FxCount,  // kAdDrawn params
  AdMaskBase = AdDrawnBase + kAdDrawn,
  AdPitch = AdMaskBase + kAdMask,  // appended: global transpose (semitones, fractional)
  AdFine,  // appended after AdPitch: fine tuning (cents)
  // ---- J-4: the full subtractive-additive set (appended; older states take the defaults,
  // which reproduce the previous sound).
  AdTeqBase,                      // kAdTeq timbre EQ bands (-1..1 = ±18 dB, octave bands of the series)
  AdSub3 = AdTeqBase + 6,         // sub-harmonic at f0/3 (AdSub = f0/2)
  AdSub4,                         // sub-harmonic at f0/4
  AdProtect,                      // low-partial protection (keeps the fundamental through filter / phaser)
  AdMaskLfo,                      // LFO → mask mix
  AdTremolo,                      // stereo tremolo depth (LFO)
  AdGrit,                         // partial pitch jitter (-1..1: negative = static, positive = moving)
  AdDetune,                       // partial detuning: cents at the 16th partial, scaled with log2(k)
  AdVibrato,                      // built-in pitch vibrato depth (0..1 = ±1 semitone, 5.5 Hz)
  AdVelAtt, AdVelRel, AdVelVol, AdVelFEnv, AdVelPluck, AdVelHarm, AdVelPorta, AdVelUni,  // velocity routings
  AdPluckMask, AdPluckAlt, AdPluckRel,  // pluck options: masked / alternate / only on release
  AdPlayMode,                     // AdPlay
  AdFEnvAttSlope, AdFEnvDecSlope, AdFEnvAdaptive,
  AdFltScale,                     // 0 = width in octaves, 1 = width scales with frequency (linear Hz)
  AdResType, AdResOsc, AdResOffset, AdResScale, AdResAdaptive, AdResTrack,
  AdUniPhase, AdUniVar, AdUniType,
  AdPhType, AdPhScale, AdPhWidthLfo, AdPhKey,
  AdHzMix, AdHzWidth, AdHzStrength, AdHzType, AdHzPos, AdHzWidthLfo,
  AdLfoAttack, AdLfoGlobal, AdLfoTempo,
  AdChType, AdDelType, AdRevType, AdCompType, AdComp,
  AdCount
};
inline constexpr int kAdTeq = 6;
// Spectra (timbre A / B). The second group are the subtractive-additive
// "colours" (saw family with phase and harmonic variations); Custom plays the
// harmonic table in PluginState::data (see parseAdditiveData).
enum AdSpectrum {
  SpSaw, SpSquare, SpTriangle, SpSine, SpPulse, SpOrgan, SpVowel, SpGlass, SpChoir, SpRandom,
  SpBright, SpBrighter, SpDephased, SpDephased2, SpSqSaw, SpSqSaw2, SpNotched, SpDouble, SpTriple, SpTriple2, SpCustom,
  SpCount
};
// Partial filter shapes (harmonic-domain: gain of each partial against its
// distance from the cutoff, in widths).
enum AdFilterType {
  AfOff, AfLowPass, AfBandPass, AfBandStop, AfHighPass,
  AfSoftLp, AfSoftBp, AfSoftBs, AfSoftHp,
  AfHill, AfSlope, AfHybrid, AfSnake, AfFin, AfLeaky, AfComb, AfBandComb, AfFlatBp,
  AfCount
};
// Resonance shapes (added around cutoff + offset).
enum AdResType { ArPeak, ArSpring, ArPedestal, ArHat, ArBump, ArCone, ArWell, ArHole, ArNoise, ArCount };
// Phaser comb shapes.
enum AdPhaserType { ApClassic, ApTriangle, ApEggs, ApDeep, ApDeeper, ApNotch, ApTwins, ApCascade, ApBox, ApFreq, ApCount };
// Harmonizer kinds: add partials an octave (+0 / +1 / +2 harmonic) or one harmonic (+0 / +1) above each.
enum AdHarmType { AhOct, AhOct1, AhOct2, AhHarm, AhHarm1, AhCount };
enum AdHarmPos { AhPreFilter, AhPrePhaser, AhPrePluck, AhFinal };
// Voice modes: Auto = mono legato while glide > 0 (older states), else poly.
enum AdPlay { ApAuto, ApPoly, ApPolyPorta, ApMono, ApMonoPorta };
// LFO shapes (AdLfoShape).
enum AdLfoKind {
  AlSine, AlTriangle, AlSaw, AlSquare, AlRandom,
  AlSmoothSaw, AlSmoothPulse, AlStairs, AlSickle, AlMultiSine,
  AlFiltEnv, AlFiltFreq, AlVoiceRand, AlVelocity, AlPitch,
  AlCount
};
enum AdChorus { AcClassic, AcThick, AcDeep, AcFlanger, AcAltFlanger, AcStereo, AcMod, AcSurround, AcVibe };
enum AdDelay { AdlClassic, AdlPingPong, AdlStereo };
enum AdReverb { AvClassic, AvHall, AvCathedral, AvVenue, AvDrumRoom, AvStudio };
enum AdComp { AkOff, AkLimit, AkWarm, AkHeat, AkDrive };

// Additive drawn data (PluginState::data, text):
//   add 1
//   c <n> <level>…   custom timbre: n harmonic levels (0..1), then
//   p <n> <phase>…   their phases (0..1 of a cycle)
inline constexpr int kAdCustomMax = 512;
struct AdditiveData {
  std::vector<float> level, phase;  // empty = none
};
bool parseAdditiveData(const std::string& text, AdditiveData& out);
std::string serializeAdditiveData(const AdditiveData& d);
// Harmonic levels / phases of a single-cycle waveform (n samples) via a DFT.
AdditiveData analyzeCycle(const float* x, int n, int harmonics = 256);

// Everything the partial-gain model needs, read from a state (UI and DSP share it).
struct AdditiveModel {
  int specA = 0, specB = 0, partials = 128, fltType = 1, seed = 1;
  float morph = 0, tilt = 0, oddEven = 0, sub = 0, stretch = 0, scatter = 0, maskMix = 0;
  float cutoff = 4000, width = 2, reso = 0, keyTrack = 0.5f;
  float phMix = 0, phDensity = 2, phOffset = 0;
  float pluck = 0, pluckTime = 1;
  float drawn[kAdDrawn] = {};
  float mask[kAdMask] = {};
  // J-4 additions.
  float teq[kAdTeq] = {};
  float detune = 0, protect = 0;
  int resType = ArPeak, phType = ApClassic, fltScale = 0, phScale = 0, resScale = 0;
  double resWidth = 1.0, f0ref = 261.63;  // resonance table width (octaves per 1024 entries); harmonic-axis phaser reference
  float resOffset = 0, resOsc = 0;
  int hzType = AhOct, hzPos = AhPreFilter;
  float hzMix = 0, hzWidth = 0.5f, hzStrength = 0;
  bool pluckMasked = false, pluckAlt = false;
  const AdditiveData* custom = nullptr;  // SpCustom table (not owned; may be null)
  void read(const PluginState& s);
  // Frequency ratio of partial k (1-based) to the fundamental (prism / scatter / detune).
  double ratio(int k) const;
  // Timbre level of partial k (morph = 0..1 between A and B), before filtering.
  float timbre(int k, float morphNow) const;
  // Start phase (0..1) of partial k for the current spectra (dephased colours, custom table).
  float phase(int k, float morphNow) const;
  // Filter gain at `hz` for a cutoff (already tracked/modulated) in Hz; `resoNow`
  // < 0 uses the stored resonance, `widthMul` scales the width (adaptive modes).
  float filter(double hz, double cutoffHz, float resoNow = -1, double widthMul = 1, bool withRes = true) const;
  // Phaser gain at `hz` with offset (0..1 wraps), density multiplier for LFO width modulation.
  float phaser(double hz, double offset, double densityMul = 1) const;
  // Pluck decay factor of a partial at frequency hz, t seconds after the note.
  float pluckGain(double hz, double f0, double t, double timeMul = 1) const;
  // Harmonizer: extra level partial k receives from the harmonizer (relative to
  // the source level `lvl` of the partial it copies), given the width.
  // Harmonizer: adds copies of lvl[0..n) moved up the series, in place (tmp: n floats of scratch).
  void harmonize(float* lvl, int n, double widthNow, float* tmp) const;
  // Timbre EQ gain at log2(partial ratio).
  double teqGain(double oct) const;
  // Protection gain (0..1 blend toward 1) for partial k.
  float protectGain(int k, float g) const;
};
// Level of partial k (1-based) for a built-in spectrum at seed `seed`.
float spectrumLevel(int spectrum, int k, int seed, const AdditiveData* custom = nullptr);
// Start phase (0..1) of partial k for a spectrum.
float spectrumPhase(int spectrum, int k, const AdditiveData* custom = nullptr);
// Harmonic-domain shape helpers shared with Harmor-class engines.
//   filterShapeT / filterShape: gain over the shape's table position t (0..1) /
//     at `oct` octaves from the cutoff with the table spanning widthOct per 1024 entries.
//   resonanceShapeT / resonanceShape: resonance value (scaled; may be negative), 0 outside.
//   phaserShape: comb value (0..2) at comb phase ph; gain = mix·value + (1 - mix).
float filterShapeT(int shape, double t);
float filterShape(int shape, double oct, double widthOct);
float resonanceShapeT(int type, double t, uint32_t seed = 0, int k = 0);
float resonanceShape(int type, double oct, double widthOct, uint32_t seed = 0, int k = 0);
float phaserShape(int shape, double ph);

// ------------------------------------------------------------------ Image Synth
inline constexpr int kImgMaxRows = 128;
inline constexpr int kImgMaxCols = 512;
inline constexpr int kCanvasW = 64, kCanvasH = 32;
enum ImageParam {
  ImSource, ImPattern, ImLow, ImHigh, ImScale, ImPixel, ImStart, ImLoop, ImReverse, ImGrainy, ImWiden, ImUseBlue,
  ImKeyTrack, ImGamma, ImAttack, ImRelease, ImVoices, ImVolume, ImCount
};
enum ImageSourceKind { SrcPattern, SrcFile, SrcCanvas };
// An image as frequency rows × time columns; row 0 = top = highest frequency.
struct Image {
  int w = 0, h = 0;
  std::vector<float> r, g, b;  // w*h each, row-major, 0..1
  float at(const std::vector<float>& c, int x, int y) const { return c[static_cast<size_t>(y * w + x)]; }
};
using ImagePtr = std::shared_ptr<const Image>;
inline constexpr int kImgPatterns = 8;
// Built-in procedural pictures (generated once, cached).
ImagePtr builtinImage(int pattern);
// Loads BMP (24/32-bit uncompressed), PPM/PGM (P5/P6) or TGA (uncompressed);
// downscaled to at most kImgMaxRows × kImgMaxCols. Cached by path. nullptr on failure.
ImagePtr loadImageFile(const std::string& path, std::string* err = nullptr);
// Canvas (kCanvasW × kCanvasH greyscale, 0..15 per pixel) <-> hex text.
std::vector<uint8_t> decodeCanvas(const std::string& hex);
std::string encodeCanvas(const std::vector<uint8_t>& px);  // all blank → "0"
std::vector<uint8_t> starterCanvas();                      // shown while the canvas text is ""
ImagePtr canvasImage(const std::string& hex);
// PluginState::data for Image Synth: "<file path>\n<canvas hex>".
std::string imageDataPath(const std::string& data);
std::string imageDataCanvas(const std::string& data);
std::string makeImageData(const std::string& path, const std::string& canvas);
// The image a state currently plays (pattern / file / canvas); never null.
ImagePtr imageFor(const PluginState& s);
// Row frequency for the image synth (row 0 = top).
double imageRowHz(int row, int rows, int scale, double low, double high, double f0Ratio);

// ------------------------------------------------------------------ String Model
enum StringParam {
  StExciter, StExcPos, StExcHard, StExcNoise, StBowForce,
  StDecay, StDamp, StStiffness, StPickup, StSaturation,
  St2Ratio, St2Level, StDetune,
  StBody, StBodyMix, StBodySize,
  StLoCut, StHiCut, StHiCutEnv, StHiCutDec,
  StAttack, StSustain, StRelease,
  StStrum, StStrumTime, StStrumDir,
  StDrive, StTone, StCab,
  StVibrato, StVibRate, StSpread, StVoices, StVolume,
  StFx,
  StCount = StFx + FxCount
};
enum StExciterKind { ExPluck, ExPick, ExHammer, ExBow, ExBreath };
inline constexpr int kBodyModes = 6;
inline constexpr int kBodies = 8;
struct BodyMode {
  float hz, gain, q;
};
// Body resonance tables (index 0 = none).
const BodyMode* bodyModes(int body);
inline constexpr int kStrumMax = 6;
// Semitone offsets of a strum chord (count returned), chord 0 = off (single note).
int strumChord(int chord, int* semis);
double stringRatio(int choice);

// ------------------------------------------------------------------ Drum Model
inline constexpr int kDrumPads = 16;
inline constexpr int kDrumModes = 12;
enum DrumPadParam {
  DpLevel, DpPan, DpTune, DpMute,
  DpShape, DpFreq, DpSpread, DpDecay, DpMaterial, DpPosition,
  DpBend, DpBendTime,
  DpMallet, DpClick, DpNoise, DpNoiseDecay, DpNoiseTone, DpWires,
  DpChoke, DpDrive,
  DpCount
};
enum DrumMaster { DmBaseNote, DmVelocity, DmVolume, DmLow, DmMid, DmHigh, DmLimiter, DmLofi, DmCount };
inline constexpr int kDrumMasterBase = 0;
inline constexpr int drumPad(int pad, int p) { return DmCount + pad * DpCount + p; }
// Appended (0.5 import): key mode — 0 = one pad per key from the base note,
// 1 = every key plays pad 1, transposed by (key - base note) semitones.
inline constexpr int DmKeyMode = DmCount + kDrumPads * DpCount;
inline constexpr int kDrumParamCount = DmKeyMode + 1;
enum DrumShape { DsMembrane, DsTom, DsBar, DsPlate, DsBell, DsTabla, DsCount };
// Mode frequency ratios / base gains of a shape (kDrumModes each).
const float* drumRatios(int shape);
const float* drumGains(int shape);
// Default kit pad names (UI only).
const char* drumPadName(int pad);

}  // namespace gl::packs::models
