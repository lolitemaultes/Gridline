// Gridline — K-99: MPE controller input for recording (see MpeInput.h).
#include "MpeInput.h"
#include <algorithm>

namespace gl {

void MpeInput::reset() {
  for (int c = 0; c < 16; ++c) key_[c] = -1, bend_[c] = 0, pressure_[c] = 0, timbre_[c] = 0.5f;
}

int MpeInput::feed(uint8_t status, uint8_t d1, uint8_t d2, Change* out, int max) {
  const int kind = status & 0xF0, ch = status & 0x0F;
  if (ch == 0 || !out || max <= 0) return 0;  // the master channel acts on the whole instrument
  int n = 0;
  auto put = [&](ExprDim d, float v) {
    if (n < max && key_[ch] >= 0) out[n++] = Change{ch, key_[ch], d, v};
  };
  switch (kind) {
    case 0x90:
      if (d2 > 0) {
        key_[ch] = d1 & 0x7F;
        if (bend_[ch] != 0) put(ExprDim::Pitch, bend_[ch]);
        if (pressure_[ch] != 0) put(ExprDim::Pressure, pressure_[ch]);
        if (timbre_[ch] != 0.5f) put(ExprDim::Timbre, timbre_[ch]);
        return n;
      }
      [[fallthrough]];
    case 0x80:
      if (key_[ch] == (d1 & 0x7F)) key_[ch] = -1;
      return 0;
    case 0xE0: {
      const int v = (d1 & 0x7F) | ((d2 & 0x7F) << 7);
      bend_[ch] = static_cast<float>(v - 8192) / 8192.0f * std::max(1.0f, bendRange);
      put(ExprDim::Pitch, bend_[ch]);
      return n;
    }
    case 0xD0:
      pressure_[ch] = static_cast<float>(d1 & 0x7F) / 127.0f;
      put(ExprDim::Pressure, pressure_[ch]);
      return n;
    case 0xB0:
      if (d1 != 74) return 0;
      timbre_[ch] = static_cast<float>(d2 & 0x7F) / 127.0f;
      put(ExprDim::Timbre, timbre_[ch]);
      return n;
    default: return 0;
  }
}

}  // namespace gl
