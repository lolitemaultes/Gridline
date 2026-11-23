// Gridline: the SoundFont reader shared by the Multisampler (a patch as editable zones) and the
// Patch Player (a patch played as the file describes it).
//   parseSf2 / loadSf2   RIFF sfbk: presets, instruments, generators, modulators, samples
//                        (16-bit, 24-bit through sm24, Vorbis-compressed .sf3 decoded at load)
//   sf2PresetRegions     one preset's zones with instrument values and preset offsets resolved,
//                        and its modulators (the default set, the instrument's, the preset's)
// UI thread or a background reader only; the audio thread reads the parsed file, never this code.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gl::packs::samplers {

// SoundFont 2 generator operators (all of them; a few are reserved or unused by the format).
enum Sf2Op : uint16_t {
  kStartOfs = 0, kEndOfs = 1, kLoopStartOfs = 2, kLoopEndOfs = 3, kStartCoarse = 4, kModLfoToPitch = 5, kVibLfoToPitch = 6,
  kModEnvToPitch = 7, kFilterFc = 8, kFilterQ = 9, kModLfoToFc = 10, kModEnvToFc = 11, kEndCoarse = 12, kModLfoToVol = 13,
  kChorusSend = 15, kReverbSend = 16, kPan = 17, kDelayModLfo = 21, kFreqModLfo = 22, kDelayVibLfo = 23, kFreqVibLfo = 24,
  kDelayModEnv = 25, kAttackModEnv = 26, kHoldModEnv = 27, kDecayModEnv = 28, kSustainModEnv = 29, kReleaseModEnv = 30,
  kKeyToModEnvHold = 31, kKeyToModEnvDecay = 32, kDelayVolEnv = 33, kAttackVolEnv = 34, kHoldVolEnv = 35, kDecayVolEnv = 36,
  kSustainVolEnv = 37, kReleaseVolEnv = 38, kKeyToVolEnvHold = 39, kKeyToVolEnvDecay = 40, kInstrument = 41, kKeyRange = 43,
  kVelRange = 44, kLoopStartCoarse = 45, kKeynum = 46, kVelocity = 47, kAttenuation = 48, kLoopEndCoarse = 50, kCoarseTune = 51,
  kFineTune = 52, kSampleId = 53, kSampleModes = 54, kScaleTuning = 56, kExclusive = 57, kRootKey = 58, kSf2GenCount = 61
};
// Value of every generator when no zone sets it.
int32_t sf2GenDefault(int op);
// True for generators a preset zone adds to the instrument's value (address offsets, sample
// modes, exclusive class, root key, keynum and velocity belong to instruments only).
bool sf2PresetAdds(int op);

struct Sf2Preset {
  std::string name;
  int bank = 0, program = 0;
  uint32_t bagStart = 0, bagEnd = 0;
};
struct Sf2SampleHeader {
  std::string name;
  uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0, rate = 44100;
  int originalPitch = 60, correction = 0;
  uint16_t link = 0;
  uint16_t type = 1;  // 1 mono, 2 right, 4 left, 8 linked; 0x8000 ROM
};
// A modulator record (SF2 section 8.2): a source (and an amount source) through their curves,
// times amount, added to the destination generator. Identical = same sources and destination.
struct Sf2Mod {
  uint16_t src = 0, dest = 0;
  int16_t amount = 0;
  uint16_t amtSrc = 0, trans = 0;
  bool sameAs(const Sf2Mod& o) const { return src == o.src && dest == o.dest && amtSrc == o.amtSrc; }
};
// The format's ten default modulators (velocity to level and cutoff, pressure and mod wheel to
// vibrato, volume, pan, expression, reverb and chorus controllers, pitch wheel).
const std::vector<Sf2Mod>& sf2DefaultMods();

struct Sf2File {
  std::string name;
  int versionMajor = 2, versionMinor = 1;
  bool sf3 = false;          // samples were Vorbis-compressed (decoded at load)
  int undecodable = 0;       // .sf3 samples that could not be decoded (silent)
  std::vector<Sf2Preset> presets;  // sorted by bank, program
  // pdta records (generators as {oper, amount}).
  struct Bag { uint16_t gen = 0, mod = 0; };
  struct Gen { uint16_t oper = 0; int16_t amount = 0; uint8_t lo = 0, hi = 0; };
  struct Inst { std::string name; uint16_t bag = 0; };
  std::vector<Bag> pbag, ibag;
  std::vector<Gen> pgen, igen;
  std::vector<Sf2Mod> pmod, imod;
  std::vector<Inst> inst;
  std::vector<Sf2SampleHeader> samples;
  std::vector<int16_t> data;   // smpl chunk (decoded for .sf3)
  std::vector<uint8_t> data24; // sm24 low bytes; empty for 16-bit files
  // One frame of sample data, -1..1 (24-bit when the file has sm24).
  float at(size_t i) const {
    if (i >= data.size()) return 0.f;
    if (data24.empty()) return static_cast<float>(data[i]) * (1.f / 32768.f);
    return static_cast<float>(static_cast<int32_t>(data[i]) * 256 + data24[i]) * (1.f / 8388608.f);
  }
};

// One sounding zone of a preset: the instrument zone's generators with the preset zone's offsets
// added, key and velocity ranges intersected, and the modulators that apply to it.
struct Sf2Region {
  std::array<int32_t, kSf2GenCount> gen{};
  int klo = 0, khi = 127, vlo = 0, vhi = 127;
  int sample = -1;     // index into Sf2File::samples
  int instrument = -1;
  std::vector<Sf2Mod> mods;
  bool contains(int key, int vel) const { return key >= klo && key <= khi && vel >= vlo && vel <= vhi; }
};

std::shared_ptr<const Sf2File> parseSf2(const uint8_t* bytes, size_t n, std::string* err = nullptr);
// Cached by path; reads the file now (a web or network drive is read in the background and gives
// nullptr until then). "builtin:<name>" (a built-in drum sound) gives a one-patch file of that
// sound over the whole keyboard, as the sample instruments play the same path.
std::shared_ptr<const Sf2File> loadSf2(const std::string& path, std::string* err = nullptr);
// Never reads on the calling thread: the cached file, or nullptr while a background read runs
// (the program is built again when it ends). `failed` tells a file that cannot be read.
std::shared_ptr<const Sf2File> requestSf2(const std::string& path, bool* failed = nullptr, std::string* err = nullptr);
// .sf2 or .sf3 (any case).
bool isSoundFontPath(const std::string& path);
// Zones of one preset (index into presets). ROM samples and empty ranges are left out.
std::vector<Sf2Region> sf2PresetRegions(const Sf2File& f, size_t preset);
// Index of the preset with this bank and program, -1 if none.
int sf2FindPreset(const Sf2File& f, int bank, int program);
// The preset to play for a bank and program the file may not have: the exact one, else the same
// program in bank 0 (drums: bank 128, program 0), else the nearest program in the bank, else the
// first preset. `exact` tells which.
int sf2NearestPreset(const Sf2File& f, int bank, int program, bool* exact = nullptr);

}  // namespace gl::packs::samplers
