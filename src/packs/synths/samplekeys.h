// Gridline — "synths" pack, J-33 group "samplekeys": Sample Keys
// (gl.samplekeys), a sampled keyboard that plays the reference keyboard
// instrument's sound sets in place from the installed reference content (never
// bundled): ".wti" set descriptions (key groups, velocity layers, loops) over
// raw 16-bit data files or WAV files (PCM or Ogg Vorbis in WAV).
// Parameters: the 15 knobs as the reference's raw control values (tag order);
// PluginState::data = the sound set name (unknown names play the default set,
// as the reference does).
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace gl::packs::synths::skeys {

enum SkParam {
  SkDecay, SkRelease, SkHardness, SkVelHardness, SkMuffle, SkVelMuffle, SkVelSens, SkStereo, SkTune, SkDetune,
  SkStretch, SkTreble, SkPanTrem, SkLfoRate, SkOverdrive, SkCount
};
// Raw control range of each knob.
inline constexpr int kSkMin[SkCount] = {-1000, -1000, -1000, -1000, 0, 0, -1000, 0, -1000, 0, -500, -1000, -1000, 0, 0};
inline constexpr int kSkMax[SkCount] = {1000, 1000, 1000, 1000, 1000, 2000, 1000, 2000, 1000, 500, 500, 1000, 1000, 65536, 1000};
inline constexpr int kSkDefault[SkCount] = {0, 0, 0, -1000, 784, 502, -248, 1000, 0, 123, 0, 0, 0, 0, 0};

struct KeySample {
  int root = 60, high = 999;
  size_t start = 0;  // first frame in the packed buffer
  int last = 0;      // last frame (relative, inclusive)
  int loop = 0;      // loop length (frames)
  bool loops = false;
};

struct KeySet {
  std::string name;
  double rate = 44100;
  double velocityFactor = 1;
  bool releaseHigh = false;
  int channels = 1;
  int layers = 1;
  int threshold[128] = {};
  std::vector<KeySample> samples;  // key group g, layer l at g·layers + l
  std::vector<int16_t> data;       // interleaved when stereo; one guard frame after each sample
};

// Sound set names found in the reference install (the default set first); empty when absent.
const std::vector<std::string>& keySetNames();
// Loads (cached) the set of that name, or the default set when the name is unknown; null when none is installed.
std::shared_ptr<const KeySet> keySet(const std::string& name);
// True when a set of exactly this name is installed.
bool keySetInstalled(const std::string& name);

}  // namespace gl::packs::synths::skeys
