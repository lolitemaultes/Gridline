// Gridline: the Patch Player (gl.patchplayer). Plays one patch of a SoundFont (.sf2 / .sf3) the
// way the file describes it: its generators and modulators, volume and modulation envelopes, two
// LFOs, a resonant low-pass, exclusive classes, stereo sample pairs and reverb and chorus sends.
// State: PluginState::data line 0 = the file; a second line "patch <bank> <program> <name>"
// remembers the patch's name for a missing file. Bank and Patch are parameters (automatable;
// MIDI program change and bank select switch the patch while playing).
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Generators.h"
#include "../../engine/Sample.h"
#include "sf2/Sf2File.h"

namespace gl {
struct Song;
}

namespace gl::packs::samplers {

enum PpParam {
  PpBank, PpPatch, PpVolume, PpPan, PpReverb, PpChorus, PpTranspose, PpFine, PpBendRange, PpGlide, PpPolyphony, PpMono,
  PpVelocity, PpCutoff, PpResonance, PpKeyTrack, PpEnvMode, PpAttackScale, PpDecayScale, PpSustainScale, PpReleaseScale,
  PpAttack, PpDecay, PpSustain, PpRelease, PpVibMode, PpVibDepth, PpVibRate, PpVibDelay, PpModWheel,
  PpCount
};
enum PpEnvMode { PpEnvPatch, PpEnvScale, PpEnvReplace };
enum PpVelCurve { PpVelNormal, PpVelSoft, PpVelHard, PpVelFixed };
enum PpVibMode { PpVibPatch, PpVibOwn };
constexpr int kPpMaxVoices = 128;
constexpr const char* kPatchPlayerType = "gl.patchplayer";

// The state's text.
struct PatchRef {
  std::string path;
  int bank = 0, program = 0;
  std::string name;  // remembered patch name (shown while the file is missing)
};
PatchRef parsePatchRef(const std::string& data);
std::string serializePatchRef(const PatchRef& r);
// The state for a patch of a file: data, Bank and Patch, and the patch's own reverb and chorus
// amounts as the starting sends. `st` keeps its other parameters.
void setPatch(PluginState& st, const std::string& path, int bank, int program, const std::string& name);
// The patch's own reverb and chorus amounts (0..1, the largest of its zones).
void patchSends(const Sf2File& f, size_t preset, float& reverb, float& chorus);

// Sound families for patch lists (General MIDI blocks for melodic banks, "Drum kits" for 128).
const char* patchFamily(int bank, int program);
const std::vector<std::string>& patchFamilies();

// What a patch is made of (the editor's summary and "From the file" line).
struct PatchInfo {
  int zones = 0, samples = 0, stereoPairs = 0, velocityLayers = 0;
  int keyLo = 127, keyHi = 0;
  bool loops = false, allLoop = true;
  size_t memory = 0;  // bytes of sample data the patch plays
  int ownMods = 0, defaultMods = 10;
  bool modEnvPitch = false, modEnvFilter = false, modLfo = false, vibLfo = false;
  int lfos = 0;
  int filterQcB = 0;  // largest filter Q (centibels)
  bool lowPass = false;
  int exclusiveClasses = 0;
  bool bits24 = false, compressed = false;
  float reverb = 0, chorus = 0;
  // Key ranges of the zones (for the keyboard strip), merged where they overlap.
  std::vector<std::pair<int, int>> ranges;
};
PatchInfo patchInfo(const Sf2File& f, size_t preset);

// Audio runtime data for the generator (built on the UI thread; tests use it directly).
std::shared_ptr<const Sample> buildPatchRuntime(const std::string& path);

// The file's patch nearest to bank / program (sf2NearestPreset) as the state's patch; false when
// the file cannot be read (the state then remembers the path, bank and program anyway).
bool setPatchNearest(PluginState& st, const std::string& path, int bank, int program);
// A MIDI file's track on a Patch Player: MIDI channel 10 plays the drum kits (bank 128), a track
// without a program change program 0.
bool setPatchForMidiTrack(PluginState& st, const std::string& path, int midiChannel, int bank, int program);

// Reverb and chorus: the player's outputs 2 and 3 carry the patch at the send levels. This routes
// them to two mixer tracks (unused ones, else new ones) holding Gridline's reverb and chorus,
// fully wet, named after the channel. False when `channel` is not a Patch Player. One edit.
bool routePatchSends(Song& s, ObjectId channel);
// The tracks the sends reach now (kNoId: not routed).
std::pair<ObjectId, ObjectId> patchSendTracks(const Song& s, ObjectId channel);

// The pack's entry points (patchplayer_dsp.cpp, patchplayer_info.cpp).
std::unique_ptr<Generator> makePatchPlayer();
bool preparePatchPlayerAux(const PluginState& state, ChannelAux& aux);
PluginInfo patchPlayerInfo();
// The preset index a running player plays now (-1 none, -2 not a Patch Player).
int patchPlayerPreset(const Generator* g);

}  // namespace gl::packs::samplers
