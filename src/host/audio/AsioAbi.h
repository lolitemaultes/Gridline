// Gridline: the binary interface an ASIO driver exposes, declared for interoperability (K-6).
//
// Licence (decided for K-6, details in docs/asio.md): Steinberg's ASIO SDK 2.3.4 (2025-10-15) is
// dual licensed, the proprietary Steinberg ASIO License or the GNU GPL version 3 (LICENSE.txt of
// the SDK, https://www.steinberg.net/developers/). Gridline is GPL-3.0-or-later, so it uses the
// SDK's interface under the GPLv3 option. No SDK file is vendored or copied: this header is
// Gridline's own minimal declaration of the driver ABI (the COM interface's method order, the
// structures passed to it and the numeric constants), written for interoperability. The build
// needs nothing from Steinberg. "ASIO" is a trademark of Steinberg Media Technologies GmbH and is
// used only to name the driver standard; no ASIO logo is shown.
#pragma once
#include <cstdint>

namespace gl::asio::abi {

// The driver ABI uses C `long` (32 bits on Windows, LLP64). Other platforms only run the
// simulated driver, where any 32-bit integer works.
#ifdef _WIN32
using Long = long;
using ULong = unsigned long;
#else
using Long = int32_t;
using ULong = uint32_t;
#endif
using Bool = Long;
using Error = Long;
using SampleType = Long;
using SampleRate = double;

// Error codes.
inline constexpr Error kOk = 0;
inline constexpr Error kSuccess = 0x3f4847a0;  // success of future() calls
inline constexpr Error kNotPresent = -1000;
inline constexpr Error kHwMalfunction = -999;
inline constexpr Error kInvalidParameter = -998;
inline constexpr Error kInvalidMode = -997;
inline constexpr Error kSpNotAdvancing = -996;
inline constexpr Error kNoClock = -995;
inline constexpr Error kNoMemory = -994;

// Sample formats (the LSB ones are what Windows drivers use).
enum : Long {
  kInt16MSB = 0,
  kInt24MSB = 1,
  kInt32MSB = 2,
  kFloat32MSB = 3,
  kFloat64MSB = 4,
  kInt32MSB16 = 8,
  kInt32MSB18 = 9,
  kInt32MSB20 = 10,
  kInt32MSB24 = 11,
  kInt16LSB = 16,
  kInt24LSB = 17,
  kInt32LSB = 18,
  kFloat32LSB = 19,
  kFloat64LSB = 20,
  kInt32LSB16 = 24,
  kInt32LSB18 = 25,
  kInt32LSB20 = 26,
  kInt32LSB24 = 27,
};

// Host message selectors (asioMessage).
enum : Long {
  kSelectorSupported = 1,
  kEngineVersion = 2,
  kResetRequest = 3,
  kBufferSizeChange = 4,
  kResyncRequest = 5,
  kLatenciesChanged = 6,
  kSupportsTimeInfo = 7,
  kSupportsTimeCode = 8,
  kOverload = 15,
};

#pragma pack(push, 4)
struct Samples64 {  // 64-bit sample position / nanosecond stamp as two halves
  ULong hi, lo;
};

struct ChannelInfo {
  Long channel;    // in: channel index
  Bool isInput;    // in
  Bool isActive;   // out
  Long channelGroup;
  SampleType type;
  char name[32];
};

struct BufferInfo {
  Bool isInput;      // in
  Long channelNum;   // in
  void* buffers[2];  // out: the two halves of the double buffer
};

struct Time;  // only passed through (bufferSwitchTimeInfo is never asked for)

struct Callbacks {
  void (*bufferSwitch)(Long doubleBufferIndex, Bool directProcess);
  void (*sampleRateDidChange)(SampleRate rate);
  Long (*asioMessage)(Long selector, Long value, void* message, double* opt);
  Time* (*bufferSwitchTimeInfo)(Time* params, Long doubleBufferIndex, Bool directProcess);
};
#pragma pack(pop)

}  // namespace gl::asio::abi
