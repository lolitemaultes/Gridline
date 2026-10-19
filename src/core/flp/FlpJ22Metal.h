// Gridline — FLP import, J-22 metal group: decoded states exposed for the tests.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "../../packs/models/metal.h"

namespace gl::flp::j22 {

// Metallic timbre generator / preset player state (version + zlib record).
struct AlloyDecoded {
  int version = 0;
  int params[49] = {};
  uint8_t opt[64] = {};  // option block: object offsets 0x200..0x23F
  uint8_t extra3[3] = {};
  int eqTypes[3] = {0, 1, 2};
  packs::models::metal::AlloyData data;
  int knobs[5][8] = {};
  bool consumedAll = false;  // the record walked to its exact end
  size_t inflated = 0, consumed = 0;
};
bool decodeAlloyState(const uint8_t* p, size_t n, AlloyDecoded& out, bool presetPlayer);

// Spline scratcher state: header, sample path, per-key patches, trailer.
// `consumed` receives the bytes walked (the whole state when complete).
bool decodeScratchState(const uint8_t* p, size_t n, packs::models::metal::SsData& out, size_t* consumed);

// Spectral morpher state (plugin state with "SSLF" header, or a .mrp preset file).
bool decodeMorphState(const uint8_t* p, size_t n, packs::models::metal::SmData& out, size_t* consumed);

}  // namespace gl::flp::j22
