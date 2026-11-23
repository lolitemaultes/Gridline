// Gridline - control surface profiles: the shared part for controllers that need a handshake (a SysEx
// message that puts them in a DAW or programmer mode, and one that gives them back their standalone
// mode), light their pads and buttons from Gridline's state, and have pages of their own. Mackie
// Control and HUI (MackieControl.h) are older, fixed protocols and stay on their own.
//
// A device profile (one file per device family in this folder) turns messages from the device into
// Gridline actions and asks for LED states; the pieces here keep what the device shows, send only what
// changed (a cap per round, so a page change never floods the port), and match Gridline's colours to a
// device palette.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gl::surfaces {

using Rgb = uint32_t;  // 0xRRGGBB, as Song colours
using Bytes = std::vector<uint8_t>;
using Out = std::function<void(const Bytes&)>;

// One LED as the device should show it.
enum class LedMode : uint8_t { Off = 0, Static, Flash, Pulse, Rgb };
struct Led {
  LedMode mode = LedMode::Off;
  uint8_t a = 0, b = 0;  // palette entries: Static / Pulse colour a; Flash between a and b
  Rgb rgb = 0;           // LedMode::Rgb
  bool operator==(const Led& o) const { return mode == o.mode && a == o.a && b == o.b && rgb == o.rgb; }
  bool operator!=(const Led& o) const { return !(*this == o); }
  static Led off() { return {}; }
  static Led solid(int c) { return c <= 0 ? Led{} : Led{LedMode::Static, static_cast<uint8_t>(c), 0, 0}; }
  static Led pulse(int c) { return c <= 0 ? Led{} : Led{LedMode::Pulse, static_cast<uint8_t>(c), 0, 0}; }
  static Led flash(int a, int b) { return Led{LedMode::Flash, static_cast<uint8_t>(a), static_cast<uint8_t>(b), 0}; }
  static Led exact(Rgb c) { return c == 0 ? Led{} : Led{LedMode::Rgb, 0, 0, c}; }
};

// The palette entry (from `first`) nearest to `c`, by a perceptual distance that keeps the hue: a
// saturated colour never matches a grey, and a dark one matches a dark entry.
int nearestColour(const std::array<Rgb, 128>& palette, Rgb c, int first = 1);
// `c` scaled towards black by `f` (0..1).
Rgb dim(Rgb c, float f);

// What the device shows now against what it should show, for up to 128 LEDs (numbered as the
// device numbers them). take() hands out the LEDs that changed, at most `max` per call; invalidate()
// forgets everything (the device was (re)connected), so every LED is sent again.
class LedCache {
 public:
  LedCache() {
    used_.fill(true);
    invalidate();
  }
  // Only these indices exist on the device (the others are never sent).
  void use(const std::array<bool, 128>& used) {
    used_ = used;
    invalidate();
  }
  void invalidate() {
    for (size_t i = 0; i < 128; ++i) known_[i] = !used_[i];
  }
  void want(int index, const Led& l) {
    if (index >= 0 && index < 128) want_[static_cast<size_t>(index)] = l;
  }
  const Led& wanted(int index) const { return want_[static_cast<size_t>(index & 127)]; }
  // The indices to send this round (each marked sent), in index order, at most `max`.
  std::vector<int> take(int max);
  int pending() const;  // LEDs that differ from what was sent

 private:
  std::array<Led, 128> want_{}, sent_{};
  std::array<bool, 128> known_{}, used_{};
};

}  // namespace gl::surfaces
