// Gridline: K-47: which OpenGL context the interface's own textures belong to. A driver reset takes
// every texture with the old context (GlRecovery.h); whatever keeps a texture name compares this
// number and makes its texture again when it changed.
#pragma once
#include <cstdint>

namespace gl::glepoch {

inline uint64_t& counter() {
  static uint64_t n = 0;
  return n;
}
inline uint64_t current() { return counter(); }
inline void next() { ++counter(); }

}  // namespace gl::glepoch
