// Gridline: platform-independent parts of the ASIO backend (K-6): sample format conversion,
// buffer size choice, the channel plan (which device channels Gridline opens and where they go)
// and the sample rates offered. Tested on every platform (tests/test_asio.cpp).
// Licence notes: AsioAbi.h and docs/asio.md.
#pragma once
#include <string>
#include <vector>

#include "AsioAbi.h"

namespace gl::asio {

// ---- sample formats ----
bool formatSupported(abi::SampleType t);  // the converters below handle it
int formatBytes(abi::SampleType t);       // bytes per sample (0 = unknown)
const char* formatName(abi::SampleType t);
// n samples of one channel. No allocation; unsupported formats read as silence / write zeros
// (when the size is known). Out-of-range floats are clipped for the integer formats.
void toFloat(const void* src, abi::SampleType t, float* dst, int n);
void fromFloat(const float* src, abi::SampleType t, void* dst, int n);

// ---- buffer sizes ----
struct BufferCaps {
  int minSize = 0, maxSize = 0, preferred = 0;
  int granularity = 0;  // -1 = powers of two, 0 = only `preferred`, > 0 = step from minSize
};
bool bufferSizeValid(const BufferCaps& c, int frames);
// Sizes offered in the settings (sorted, at most `maxCount`; the preferred size is always one).
std::vector<int> bufferSizeChoices(const BufferCaps& c, int maxCount = 24);
// The size to open: `wanted` <= 0 = the driver's preferred size; otherwise the closest valid size.
int chooseBufferSize(const BufferCaps& c, int wanted);

// ---- sample rates ----
const std::vector<int>& candidateRates();  // asked from the driver with canSampleRate

// ---- channels ----
inline constexpr int kMaxInputs = 16;       // the engine's device inputs (kMaxInputChannels)
inline constexpr int kMaxExtraOutputs = 15;  // extra output pairs fed by mixer tracks

struct ExtraOut {
  int mixer = -1;      // mixer track index
  int firstOut = -1;   // first device output of the pair (the pair is firstOut, firstOut + 1)
  bool operator==(const ExtraOut& o) const { return mixer == o.mixer && firstOut == o.firstOut; }
};
// Settings text "mixer:firstOut,mixer:firstOut" (both 0-based).
std::vector<ExtraOut> parseExtraOuts(const std::string& s);
std::string formatExtraOuts(const std::vector<ExtraOut>& v);

struct ChannelRequest {
  int inFirst = 0, inCount = 2;  // device inputs In 1.. of the mixer (inCount 0 = none)
  int outMain = 0;               // first output of the main stereo pair
  std::vector<ExtraOut> extras;
};
// What gets opened, after clamping a request to the device's channel counts.
struct ChannelPlan {
  std::vector<int> inputs;   // device input index for engine input 0, 1, ...
  int mainL = -1, mainR = -1;  // device outputs of the main pair (mainR == mainL: mono device)
  std::vector<ExtraOut> extras;  // valid, not overlapping the main pair or each other
  // Device outputs opened, in order: main L, (main R), then each extra pair's two outputs.
  std::vector<int> outputs;
  int extraOut(int k) const;  // index into `outputs` of extra pair k's left channel
};
ChannelPlan planChannels(int numIn, int numOut, const ChannelRequest& r);

// Label of a pair of device channels: "Out 3 - Out 4" (or the driver's names when given).
std::string pairLabel(const std::vector<std::string>& names, int first, bool input);

}  // namespace gl::asio
