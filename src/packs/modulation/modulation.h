// Gridline — "modulation" pack: controllers, note processors, patch host,
// sidechain and send utilities. Parameter indices shared by the catalogue
// (modulation_info.cpp), the runtimes (*_dsp.cpp) and the editors (modulation_ui.cpp).
#pragma once
#include <string>
#include <vector>

namespace gl::packs::modulation {

// ------------------------------------------------------------- controllers
// "gl.peakctl" — Peak Follower (mixer effect). Outputs (J-42: the reference's
// order): Peak + LFO (clamped sum), Peak, LFO.
// J-42 reference model (PkModel = 1): the stored integers PkR* drive a block
// peak (instant attack) with a linear per-tick decay, a tempo-locked 32-bit
// phase LFO, the exponential amount / tension laws and a 1/8-per-tick ramp.
enum PeakCtl {
  PkBase, PkAmount, PkTension, PkAttack, PkDecay,
  LfBase, LfAmount, LfTension, LfShape, LfSync, LfRate, LfSyncRate, LfPhase,
  PkRamp, PkMute,
  PkModel,                                    // 0 legacy follower, 1 reference (older states: legacy)
  PkRBase, PkRAmount, PkRTension, PkRDecay,   // 0..65536, −256..256, −128..128, 5..128
  LfRBase, LfRAmount, LfRTension, LfRSpeed,   // 0..65536, −256..256, −128..128, 200..65536
  LfRPhase,                                   // 0..65535
  PkCount
};
// "gl.xyctl" — X-Y Pad (mixer effect). Outputs: X, Y.
enum XyCtl { XyX, XyY, XyXMin, XyXMax, XyXTension, XyYMin, XyYMax, XyYTension, XySmooth, XyInvertY, XyCount };
// "gl.xyzctl" — X-Y-Z Pad (mixer effect). Outputs: X, Y, Z.
enum XyzCtl { XzX, XzY, XzZ, XzXMin, XzXMax, XzYMin, XzYMax, XzZMin, XzZMax, XzSmooth, XzCount };
// "gl.formulactl" — Formula Controller (mixer effect). data = formula. Output: Out.
enum FormulaCtl { FmA, FmB, FmC, FmSmooth, FmCount };
// "gl.envctl" — Envelope Controller (instrument). Four note-triggered
// articulators (envelope + LFO), one output each.
inline constexpr int kEnvArts = 4;
inline constexpr int kEnvPoints = 6;
enum EnvArtField {
  EaBase, EaEnvAmt, EaLfoAmt, EaMode, EaNumPoints, EaSustain, EaLoop,
  EaLfoShape, EaLfoSync, EaLfoRate, EaLfoSyncRate, EaLfoDelay,
  EaKeyLow, EaKeyHigh, EaVelAmt, EaRetrig,
  EaPoints,  // then kEnvPoints x (time, level)
  EaFields = EaPoints + kEnvPoints * 2
};
inline constexpr int envParam(int art, int field) { return art * EaFields + field; }
inline constexpr int envPointTime(int art, int pt) { return art * EaFields + EaPoints + pt * 2; }
inline constexpr int envPointLevel(int art, int pt) { return art * EaFields + EaPoints + pt * 2 + 1; }
inline constexpr int EcContinuous = kEnvArts * EaFields;
inline constexpr int EcCount = EcContinuous + 1;
// "gl.keyctl" — Keyboard Controller (instrument). data = 128 key values.
// Outputs: Value, Velocity, Gate, Note.
enum KeyCtl { KcAttack, KcRelease, KcNoteOff, KcNoteOffValue, KcCount };
// "gl.cvctl" — Voltage Out (instrument): DC control voltages on the audio
// outputs from the notes it plays. Outputs: Pitch, Gate, Velocity.
enum CvCtl {
  CvEnable, CvLeftSrc, CvRightSrc, CvLeftOfs, CvRightOfs, CvLeftScale, CvRightScale, CvLeftBipolar, CvRightBipolar,
  CvPitchScale, CvRange, CvClockPpq, CvPulse, CvGlide,
  CvCount
};
enum CvSource { CvsOff, CvsPitch, CvsGate, CvsVelocity, CvsClock, CvsTrigger };
// "gl.surface" — Control Surface (mixer effect): 16 user controls; data =
// one "type label" line per control. Outputs: the 16 control values.
inline constexpr int kSurfaceControls = 16;
inline constexpr int SfSmooth = kSurfaceControls;
inline constexpr int SfCount = kSurfaceControls + 1;
enum SurfaceKind { SkKnob, SkSlider, SkButton, SkToggle, SkPadX, SkPadY };

// --------------------------------------------------------- note processors
// "gl.arp" — Arpeggiator.
enum Arp { ArDirection, ArRange, ArRate, ArGate, ArRepeat, ArSwing, ArVelMode, ArVelocity, ArLatch, ArTranspose, ArCount };
enum ArpDirection { AdUp, AdDown, AdUpDown, AdDownUp, AdRandom, AdOrder, AdChord };
// "gl.chord" — Chord Generator.
enum Chord { ChType, ChInversion, ChSpread, ChStrum, ChStrumDir, ChVelSpread, ChTranspose, ChBass, ChCount };
// "gl.notesq" — Note Sequencer (16-step grid over held notes).
inline constexpr int kSeqSteps = 16;
enum SeqStepField { SqOn, SqNote, SqOctave, SqVelocity, SqGate, SqTie, SqFields };
inline constexpr int seqParam(int step, int field) { return step * SqFields + field; }
inline constexpr int SqLength = kSeqSteps * SqFields;
inline constexpr int SqRate = SqLength + 1, SqSwing = SqLength + 2, SqTranspose = SqLength + 3, SqThru = SqLength + 4;
inline constexpr int SqCount = SqLength + 5;
// "gl.keymap" — Key Mapper (per pitch class offset, scale snap, transpose).
inline constexpr int KmMap = 0;  // 12 offsets (-13 = muted .. +12)
enum KeyMap { KmRoot = 12, KmScale, KmSnap, KmTranspose, KmOctave, KmMulti, KmCount };
// "gl.split" — Key Splitter: four zones, each routed to this or another channel.
inline constexpr int kSplitZones = 4;
enum SplitField { SpOn, SpLow, SpHigh, SpTranspose, SpVelLow, SpVelHigh, SpFields };
inline constexpr int splitParam(int zone, int field) { return zone * SpFields + field; }
inline constexpr int SpMode = kSplitZones * SpFields;  // first match / all matches
inline constexpr int SpCount = SpMode + 1;
// "gl.velscale" — Level Scaler (velocity / pitch curve + humanize).
enum VelScale { VsTarget, VsCenter, VsMultiply, VsOffset, VsTension, VsHumanize, VsHumanMode, VsMin, VsMax, VsCount };
// "gl.colormap" — Colour Router: colour group → output (this or another channel).
inline constexpr int kColorGroups = 16;
inline constexpr int CmMap = 0;  // 16 x target (0 = this channel, 1..8 = target slot, 9 = mute)
enum ColorMap { CmSource = kColorGroups, CmCount };
enum ColorSource { CsNoteColour, CsVelocity, CsPitchClass, CsOctave };
// "gl.notescript" — Note Script: per-note formulas (data = 3 lines: pitch,
// velocity, gate). Knobs a, b, c are formula variables.
enum NoteScript { NsA, NsB, NsC, NsEnable, NsCount };

// --------------------------------------------------------------- utilities
// "gl.patcher" / "gl.patcherfx" — Patch Host. data = JSON graph; 8 macros.
inline constexpr int kMacros = 8;
inline constexpr int PhMix = kMacros, PhGain = kMacros + 1, PhCount = kMacros + 2;
inline constexpr int kPatchNodes = 8;
// "gl.send" — Mixer Send (send from this point of the chain to a track).
enum MixSend { SnTarget, SnLevel, SnPan, SnDry, SnCount };
// "gl.bussend" / "gl.busreturn" — named audio buses.
enum BusSend { BsBus, BsLevel, BsDry, BsCount };
enum BusReturn { BrBus, BrLevel, BrDry, BrCount };
// "gl.scduck" — Sidechain Ducker (compressor keyed by the track's sidechain input).
enum ScDuck { SdThreshold, SdRatio, SdAttack, SdRelease, SdDepth, SdSource, SdListen, SdCount };

// Tempo-synced rates (note values), shared by LFOs, the arpeggiator and the sequencer.
inline constexpr int kRateCount = 12;
inline constexpr const char* kRateNames[kRateCount] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
                                                        "1/4 T", "1/8 T", "1/16 T", "1/4 D", "1/8 D", "1/16 D"};
inline constexpr double kRateBeats[kRateCount] = {4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
inline double rateBeats(double v) {
  int i = static_cast<int>(v + 0.5);
  return kRateBeats[i < 0 ? 0 : (i >= kRateCount ? kRateCount - 1 : i)];
}

// Chord shapes (semitones from the root), shared by the chord generator and its editor.
struct ChordShape {
  const char* name;
  int notes[6];
  int count;
};
const std::vector<ChordShape>& chordShapes();
// Scale masks (bit i = pitch class i in the scale), shared by key mapper / editors.
struct ScaleDef {
  const char* name;
  unsigned mask;
};
const std::vector<ScaleDef>& scales();

// Default formula of the formula controller.
inline constexpr const char* kDefaultFormula = "a";

}  // namespace gl::packs::modulation
