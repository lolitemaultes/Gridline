// Gridline — "samplers" pack: shared declarations.
//   gl.multisampler  Multisampler    — key/velocity zone sampler with SF2 import
//   gl.scratch       Scratch Sampler — playback position follows drawn moves
// Zone maps and scratch moves live in PluginState::data as compact text; the
// UI and the engine parse it on the UI thread (prepareAux), never in render.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Sample.h"

namespace gl::packs::samplers {

// ---------------------------------------------------------------- Multisampler
enum MsParam {
  MsVolume, MsTranspose, MsFine, MsPolyphony, MsPlayMode, MsGlide, MsRoundRobin, MsVelAmp, MsInterp,
  MsAttack, MsHold, MsDecay, MsSustain, MsRelease,
  MsFilterType, MsCutoff, MsResonance, MsKeyTrack, MsVelCutoff, MsEnvAmount,
  MsModAttack, MsModDecay, MsModSustain, MsModRelease, MsModPitch,
  MsLfoShape, MsLfoRate, MsLfoDelay, MsLfoPitch, MsLfoCutoff, MsLfoAmp, MsLfoPan,
  MsFactorySet,
  MsCount
};
enum PlayMode { PlayPoly, PlayMono, PlayLegato };
enum RoundRobin { RrLayer, RrCycle, RrRandom, RrAvoid };
enum Interp { InterpLinear, InterpCubic, InterpSinc };
enum FilterType { FiltOff, FiltLow, FiltHigh, FiltBand, FiltNotch };
enum LfoShape { LfoSine, LfoTriangle, LfoSquare, LfoSaw, LfoRandom };
enum LoopMode { LoopOff, LoopOneShot, LoopForward, LoopSustain, LoopBounce };

constexpr int kMaxVoices = 64;
constexpr int kMaxGroups = 64;

struct Zone {
  std::string path;     // audio file, "sf2:<file>#<sample>" or "tone:<set>:<root>:<layer>"
  std::string name;     // display name (empty = from path)
  int lo = 0, hi = 127;    // key range
  int vlo = 0, vhi = 127;  // velocity range (0..127)
  int root = 60;
  float tune = 0;       // cents
  float keyTrack = 1;   // 1 = chromatic, 0 = fixed pitch
  float gain = 0;       // dB
  float pan = 0;        // -1..1
  int64_t start = 0, end = 0;            // frames; end 0 = sample end
  int64_t loopStart = 0, loopEnd = 0;    // frames; loopEnd 0 = sample end
  int loop = LoopOff;
  int64_t xfade = 0;    // loop crossfade, frames
  bool reverse = false;
  int group = 0;        // round-robin group (0 = always layered)
  int choke = 0;        // exclusive class: zones with the same id cut each other
  float cutoff = 0;     // per-zone low-pass, Hz (0 = open)
  bool ownEnv = false;  // zone amp envelope instead of the program's
  float attack = 0.002f, hold = 0, decay = 0.5f, sustain = 1, release = 0.3f;
  bool operator==(const Zone& o) const;
};

struct ZoneMap {
  std::vector<Zone> zones;
  std::string sf2Path;  // soundfont the zones came from (for the preset picker)
  int sf2Bank = -1, sf2Program = -1;
  std::string sf2Name;  // preset name
};

// Parses PluginState::data. Empty data gives the factory set's map; a bare
// audio path gives a single full-keyboard zone.
ZoneMap parseZoneMap(const std::string& data, int factorySet = 0);
std::string serializeZoneMap(const ZoneMap& m);
ZoneMap factoryZoneMap(int set);
const std::vector<std::string>& factorySetNames();
std::string zoneLabel(const Zone& z);
// Sample for a zone path (files via the sample cache, soundfont samples and
// factory tones via the pack's own caches). UI thread only.
SamplePtr resolveSample(const std::string& path);
// Root key from a file name ("Piano_C#3", "bass-60", "A4 soft"); -1 if none.
int noteFromName(const std::string& name);
// Key ranges from roots (splits halfway), velocity layers for equal roots.
void autoMap(std::vector<Zone>& zones);
// One key per zone from `base` upwards (root = key).
void mapAsDrumKit(std::vector<Zone>& zones, int base);
// New zone for a sample file, root from its name.
Zone zoneForFile(const std::string& path);

// ---------------------------------------------------------------- SoundFont 2
struct Sf2Preset {
  std::string name;
  int bank = 0, program = 0;
  uint32_t bagStart = 0, bagEnd = 0;
};
struct Sf2SampleHeader {
  std::string name;
  uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0, rate = 44100;
  int originalPitch = 60, correction = 0;
  uint16_t type = 1;
};
struct Sf2File {
  std::string name;
  std::vector<Sf2Preset> presets;  // sorted by bank, program
  // pdta records (generators as {oper, amount}).
  struct Bag { uint16_t gen = 0; };
  struct Gen { uint16_t oper = 0; int16_t amount = 0; uint8_t lo = 0, hi = 0; };
  struct Inst { std::string name; uint16_t bag = 0; };
  std::vector<Bag> pbag, ibag;
  std::vector<Gen> pgen, igen;
  std::vector<Inst> inst;
  std::vector<Sf2SampleHeader> samples;
  std::vector<int16_t> data;  // smpl chunk
};
std::shared_ptr<const Sf2File> parseSf2(const uint8_t* bytes, size_t n, std::string* err = nullptr);
// Cached by path.
std::shared_ptr<const Sf2File> loadSf2(const std::string& path, std::string* err = nullptr);
// Zones of one preset (index into presets), sample paths "sf2:<path>#<sample>".
std::vector<Zone> sf2PresetZones(const Sf2File& f, const std::string& path, size_t preset);
// Mono float sample for one SoundFont sample header.
SamplePtr sf2MakeSample(const Sf2File& f, size_t sampleIndex);
// Zone map for a soundfont preset (by index).
ZoneMap sf2PresetMap(const std::string& path, size_t preset, std::string* err = nullptr);

// ---------------------------------------------------------------- Scratch Sampler
enum ScParam {
  ScVolume, ScStart, ScEnd, ScLength, ScSync, ScKeyMode, ScRoot, ScPitchFollow, ScSmoothing, ScAttack, ScRelease,
  ScLoopMove, ScInterp, ScSpeed, ScMotion,
  ScCount
};
enum ScKeyMode { KeyMoves, KeyPitched };
// Position: the curve is the playhead. Speed: the curve is the platter speed
// (bottom = full reverse, middle = stopped, top = double speed forward).
enum ScMotion { MotionPosition, MotionSpeed };
enum Curve { CurveLinear, CurveSmooth, CurveEaseIn, CurveEaseOut, CurveHold };
constexpr int kMoves = 12;     // one move per key (C..B)
constexpr int kMuteSteps = 16; // mute lane resolution

struct ScratchPoint {
  float t = 0, y = 0;  // 0..1 time, 0..1 position (between Start and End)
  int curve = CurveSmooth;  // shape of the segment that starts here
};
struct ScratchMove {
  std::vector<ScratchPoint> pts;  // sorted by t; first at t=0, last at t=1
  uint32_t mutes = 0;             // bit k mutes step k of kMuteSteps
};
struct ScratchData {
  std::string path;  // sample (empty = built-in phrase)
  std::array<ScratchMove, kMoves> moves;
};
ScratchData parseScratch(const std::string& data);
std::string serializeScratch(const ScratchData& d);
ScratchMove factoryMove(int k);
const char* factoryMoveName(int k);
// Position 0..1 at time t (0..1).
float evalMove(const ScratchMove& m, float t);
// The built-in phrase used when no sample is loaded.
SamplePtr scratchDefaultSample();

}  // namespace gl::packs::samplers
