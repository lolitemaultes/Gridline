// Gridline: an SFZ instrument as a player reads it (K-144). SfzText expands #include and #define;
// this reads the headers (<control> <global> <master> <group> <region> <curve> <effect> <sample>
// <midi>) with their inheritance, every opcode of SFZ v1, SFZ v2 and the ARIA extensions, and gives
// plain region structs the Multisampler plays. Opcodes Gridline does not know are listed in the
// report, never a load failure. UI thread only (the player prepares its data from this).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gl::sfz {

// ------------------------------------------------------------------ opcode registry
// How Gridline treats an opcode (the coverage table in docs/sfz.md is made from this).
enum class Support : uint8_t {
  Played,  // changes the sound as the format describes
  Mapped,  // taken over by a Gridline control or behaviour (the note says which)
  Shown,     // labels and images: shown in the editor, no sound of their own
  Reported,  // player-specific settings with no Gridline counterpart: read, and listed in the report
};
struct OpcodeInfo {
  const char* pattern;  // name with N / X / NN / NNN for numbers ("cutoff_onccN", "lfoN_freq")
  const char* family;   // docs grouping
  Support support;
  const char* note;     // how it is played or mapped (empty: as specified)
};
const std::vector<OpcodeInfo>& opcodeTable();
// The registry entry for an opcode as written in a file ("cutoff_oncc74", "lfo2_freq"); null if unknown.
const OpcodeInfo* findOpcode(const std::string& name);
// Header names Gridline reads (control global master group region curve effect sample midi).
bool knownHeader(const std::string& name);

// ------------------------------------------------------------------ model
// Extended controller numbers (as in the ARIA and sfizz players): CC 0..127, then these.
enum ExtCc : int {
  kCcPitchBend = 128, kCcChanAft = 129, kCcPolyAft = 130, kCcVelocity = 131, kCcRelVelocity = 132, kCcKey = 133,
  kCcGate = 134, kCcRandUni = 135, kCcRandBi = 136, kCcAlternate = 137, kCcKeyDelta = 140, kCcAbsKeyDelta = 141,
  kCcCount = 512,  // hdcc and plain cc numbers beyond 141 are free slots (set_hdccN, varNN)
};

// What a modulation moves.
enum class Target : uint8_t {
  Volume, Amplitude, Pan, Width, Position, Pitch, Cutoff, Resonance, FilGain, EqFreq, EqBw, EqGain,
  Offset, Delay, LoopStart, LoopLength, EgTime, LfoFreq, LfoDepth, LfoDepthAdd, NoiseLevel, NoiseStep, NoiseTone,
  Decim, Bitred, Rectify, RingMod, Drive, Oscillator, AmpegStage, Count
};
// One controller-driven modulation (opcode_onccN, _curveccN, _smoothccN, _stepccN).
struct CcMod {
  Target target = Target::Volume;
  int index = 0;  // filter 0/1, EQ band 0..2, LFO / EG number, EG stage
  int cc = 0;
  float depth = 0;
  int curve = 0;       // <curve> number (0 = linear 0..1, 1 = bipolar -1..1 ...)
  float smooth = 0;    // ms
  float step = 0;      // quantise steps of the depth (0 = off)
};

// Classic envelope (ampeg_, fileg_, pitcheg_) with the ARIA shapes.
struct ClassicEg {
  float delay = 0, start = 0, attack = 0, hold = 0, decay = 0, sustain = 100, release = 0, depth = 0;
  float attackShape = 0, decayShape = 0, releaseShape = 0;  // 0 = the format's default shape
  bool decayZero = true, releaseZero = false, dynamic = false;
  // vel2* (depth per velocity 0..1)
  float vel2delay = 0, vel2attack = 0, vel2hold = 0, vel2decay = 0, vel2sustain = 0, vel2release = 0, vel2depth = 0;
  // controller amounts per stage: delay start attack hold decay sustain release (seconds / %)
  struct Cc { int stage; int cc; float amount; int curve; };
  std::vector<Cc> cc;
  bool used = false;
};

// v2 flexible envelope (egN_).
struct FlexEg {
  struct Point { float time = 0, level = 0, shape = 0; };
  std::vector<Point> points;  // point 0 is the start level
  int sustain = 0, loop = -1, loopCount = 0;
  bool dynamic = false, ampeg = false;
  struct Dest { Target target; int index; float depth; };
  std::vector<Dest> dest;
  std::vector<CcMod> depthCc;  // egN_<target>_onccX: target index in CcMod.index = dest index
  std::vector<CcMod> pointCc;  // egN_timeX_onccY / levelX_onccY: index = point*2 (+1 level)
};

// LFO (v1 amplfo_ fillfo_ pitchlfo_, v2 lfoN_).
struct Lfo {
  float freq = 0, delay = 0, fade = 0, phase = 0, smooth = 0, offset = 0, ratio = 1, scale = 1;
  int count = 0;                // 0 = endless
  int wave = 1;                 // 0 tri 1 sine 2 pulse75 3 square 4 pulse25 5 pulse12.5 6 ramp up 7 ramp down, 12 S&H
  std::vector<int> subWaves;    // lfoN_waveX (sub oscillators beyond the first)
  std::vector<float> subOffsets, subRatios, subScales;
  std::vector<float> steps;     // lfoN_steps / stepX (a step sequencer wave)
  struct Dest { Target target; int index; float depth; };
  std::vector<Dest> dest;
  std::vector<CcMod> cc;        // freq / depth controllers (target LfoFreq or the dest's target with index = dest)
  bool v1 = false;
  bool used = false;
};

struct Filter {
  int type = -1;  // -1 = off; see filterTypeOf
  float cutoff = -1, resonance = 0, gain = 0, keytrack = 0, veltrack = 0, random = 0, resonanceRandom = 0;
  int keycenter = 60;
  int mode = 0;  // fil_mode (ARIA: 0 default)
};
struct Eq {
  float freq = 0, bw = 1, gain = 0, vel2freq = 0, vel2gain = 0;
  int type = 0;  // 0 peak, 1 low shelf, 2 high shelf
  bool dynamic = false, used = false;
};

enum class Trigger : uint8_t { Attack, Release, ReleaseKey, First, Legato };
enum class LoopMode : int8_t { Unset = -1, NoLoop, OneShot, Continuous, Sustain };
enum class LoopType : uint8_t { Forward, Backward, Alternate };
enum class OffMode : uint8_t { Fast, Normal, Time };
enum class Builtin : uint8_t { None, Sine, Triangle, Square, Saw, Noise, Silence };

struct Range { int cc; float lo, hi; };  // lo/hi in 0..127 (hd ranges scaled)

struct Region {
  // sound source
  std::string sample;       // as written
  std::string file;         // resolved path (empty for built-ins)
  Builtin builtin = Builtin::None;
  int64_t offset = 0, offsetRandom = 0, end = -1, count = 0;
  bool offsetPercent = false;
  LoopMode loopMode = LoopMode::Unset;
  LoopType loopType = LoopType::Forward;
  int64_t loopStart = -1, loopEnd = -1;
  float loopCrossfade = 0, loopTune = 0;
  int loopCount = 0, loopEndOffset = 0;
  bool reverse = false;
  std::vector<Range> reverseCc;
  float delay = 0, delayRandom = 0, delayBeats = 0, delayBeatsRandom = 0, stopBeats = 0;
  int64_t delaySamples = 0;
  float syncBeats = 0, syncOffset = 0, sampleFadeout = 0;
  std::string md5;
  bool waveguide = false;
  // wavetable oscillator (oscillator=on plays the sample as one cycle)
  bool oscillator = false;
  int oscMode = 0, oscMulti = 1, oscQuality = 1, oscTableSize = 0;
  float oscDetune = 0, oscModDepth = 0, oscPhase = 0;  // phase -1 = random
  int sampleQuality = -1;
  // region logic
  int lokey = 0, hikey = 127, lovel = 1, hivel = 127, lochan = 1, hichan = 16;
  int lobend = -8192, hibend = 8192, lochanaft = 0, hichanaft = 127, lopolyaft = 0, hipolyaft = 127;
  int loprog = 0, hiprog = 127;
  float lobpm = 0, hibpm = 500, lorand = 0, hirand = 1, lotimer = 0, hitimer = 1e9f;
  std::vector<Range> ccRange;     // loccN / hiccN (and hd)
  std::vector<Range> onCc;        // on_loccN / on_hiccN: the region starts when the controller enters the range
  std::vector<Range> stopCc;      // stop_loccN / stop_hiccN: its voices stop there
  int seqLength = 1, seqPosition = 1;
  Trigger trigger = Trigger::Attack;
  float rtDecay = 0;              // dB per second
  std::vector<std::pair<float, float>> rtDecayStages;  // rt_decayN / rt_decayN_time (ARIA)
  bool rtDead = false;
  // keyswitches
  int swLokey = 0, swHikey = 127, swLast = -1, swLolast = -1, swHilast = -1, swDown = -1, swUp = -1, swPrevious = -1;
  bool swVelPrevious = false;
  int swDefault = -1;
  std::string swLabel;
  // pedals
  int sustainCc = 64, sostenutoCc = 66;
  float sustainLo = 0.5f, sostenutoLo = 0.5f;
  bool sustainSw = true, sostenutoSw = true;
  // voices
  int64_t group = 0, offBy = 0;
  OffMode offMode = OffMode::Fast;
  float offTime = 0.006f;
  int offShape = 0, offCurve = -1;
  int polyphony = -1, notePolyphony = -1, polyphonyStealing = 0;
  bool noteSelfmask = true;
  int output = 0;
  // amplifier
  float volume = 0, amplitude = 100, pan = 0, width = 100, position = 0, panRandom = 0, positionRandom = 0;
  int positionKeycenter = 60;
  float positionKeytrack = 0, positionVeltrack = 0;
  int panKeycenter = 60;
  float panKeytrack = 0, panVeltrack = 0;
  int panLaw = 0;
  int ampKeycenter = 60;
  float ampKeytrack = 0, ampVeltrack = 100, ampVeltrackRandom = 0, ampRandom = 0;
  std::vector<std::pair<int, float>> velcurve;  // amp_velcurve_N points
  bool phaseInvert = false;
  float globalVolume = 0, masterVolume = 0, groupVolume = 0;
  float globalAmplitude = 100, masterAmplitude = 100, groupAmplitude = 100;
  // crossfades
  int xfinLokey = 0, xfinHikey = 0, xfoutLokey = 127, xfoutHikey = 127;
  int xfinLovel = 0, xfinHivel = 0, xfoutLovel = 127, xfoutHivel = 127;
  struct XfCc { int cc; float lo, hi; bool in; };
  std::vector<XfCc> xfCc;
  bool xfKeyPower = true, xfVelPower = true, xfCcPower = true;
  // pitch
  float tune = 0, globalTune = 0, masterTune = 0, groupTune = 0;
  int transpose = 0;
  int pitchKeycenter = 60;  // -2 = from the sample's own root note
  float pitchKeytrack = 100, pitchVeltrack = 0, pitchRandom = 0;
  float bendUp = 200, bendDown = -200, bendStep = 1, bendSmooth = 0, bendStepUp = 0, bendStepDown = 0;
  // filters and EQ
  std::array<Filter, 2> filter{};
  std::array<Eq, 3> eq{};
  // noise source (v2)
  float noiseLevel = -144, noiseStep = 0, noiseTone = 0, noiseFilter = 0;
  bool noiseStereo = false;
  // per-voice lofi (v2 targets)
  float decim = 0, bitred = 0, rectify = 0, ringmod = 0, drive = 0;
  // envelopes and LFOs
  ClassicEg ampeg, fileg, pitcheg;
  std::vector<FlexEg> egs;   // index = N - 1
  Lfo amplfo, fillfo, pitchlfo;
  std::vector<Lfo> lfos;     // index = N - 1
  std::vector<CcMod> cc;
  // velocity to modulation (v1 vel2 for filter / pitch EG handled in the EGs; these are the rest)
  float pitchVel = 0;
  // labels
  std::string globalLabel, masterLabel, groupLabel, regionLabel;
  // ARIA variables: varNN_mod (0 add / 1 mult) and targets
  struct Var { int index; int mod; std::vector<std::pair<int, float>> cc; std::vector<std::pair<Target, float>> dest; };
  std::vector<Var> vars;
  // effect sends (effect1..4, in %)
  std::array<float, 4> effectSend{};
  int headerLine = 0;  // line in the expanded text (for reports)
};

// <curve>: 128 points, interpolated between the given ones.
struct Curve {
  std::array<float, 128> v{};
  bool used = false;
};

// <effect>: a bus effect (type and its opcodes as written; the player maps them).
struct EffectDef {
  std::string bus = "main";  // main, aux1..aux8, fx1..fx4, midi
  std::string type;          // apan comp delay disto eq filter fverb gate limiter lofi phaser reverb static strings tdfir ...
  std::map<std::string, std::string> params;
  float directToMain = 100, fxToMain[4] = {0, 0, 0, 0}, fxToMix[4] = {0, 0, 0, 0};
  int dspOrder = 0;
};

struct Report {
  std::map<std::string, int> unknown;       // opcode (as written) -> uses
  std::map<std::string, int> unknownHeaders;
  std::map<std::string, int> reported;      // known opcodes Gridline has no counterpart for (Support::Reported)
  std::map<std::string, int> unhandled;     // known opcodes the reader did not take (a Gridline bug; tests keep it empty)
  std::vector<std::string> missingSamples;  // resolved paths that do not exist
  std::vector<std::string> missingIncludes;
  std::vector<std::string> included;
  std::vector<std::string> warnings;        // bad values, md5 mismatches, oversize maps
  int regions = 0, groups = 0, masters = 0, samples = 0;
};

struct Instrument {
  std::string path;  // the .sfz
  std::vector<Region> regions;
  std::array<Curve, 256> curves{};
  std::vector<EffectDef> effects;
  std::array<float, kCcCount> setCc{};    // initial controller values 0..1 (set_ccN, set_hdccN); -1 = unset
  std::map<int, std::string> ccLabels;    // label_ccN
  std::map<int, std::string> swLabels;    // keyswitch key -> sw_label
  int swDefault = -1;
  int swLokey = 128, swHikey = -1;        // keyswitch range used by the map (for the editor)
  std::string image;                      // <control> image= (shown in the editor)
  std::map<std::string, std::string> hints;  // hint_*
  int sampleQuality = -1, loadMode = 0;
  Report report;
};

// Reads an .sfz file. False only when the file cannot be read at all.
bool load(const std::string& path, Instrument& out);
// Reads SFZ text as if it were the file `path` (relative samples resolve beside it); tests use this.
bool loadText(const std::string& text, const std::string& path, Instrument& out);

// Helpers shared with the player.
int noteNumber(const std::string& v, bool* ok = nullptr);  // "60", "c4", "c#4", "db4" (c4 = 60)
int filterTypeOf(const std::string& name);                  // -1 if unknown
const char* filterTypeName(int type);
// Value of curve `c` at x (0..1); curves 0..6 are the format's built-ins when not defined.
float curveAt(const Instrument& ins, int c, float x);
// Plain sentence for the report line ("3 settings Gridline does not play: ..."), empty when none.
std::string reportLine(const Report& r);

}  // namespace gl::sfz
