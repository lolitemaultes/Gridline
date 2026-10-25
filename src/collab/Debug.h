// Gridline - J-111 collaboration: verbose network diagnostics on stderr, off
// unless GRIDLINE_COLLAB_DEBUG=1 (any value but "0"). Endpoint state changes,
// handshake packets sent and received, failed sends, links opened and closed,
// plus libjuice's own warnings. For tests between two computers:
//   GRIDLINE_COLLAB_DEBUG=1 gridline-app 2> collab.log
// Thread safe (one fprintf per line); lines start with "collab-debug <seconds>".
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace gl::collab {

inline bool debugOn() {
  static const bool on = [] {
    const char* v = std::getenv("GRIDLINE_COLLAB_DEBUG");
    return v && *v && !(v[0] == '0' && v[1] == 0);
  }();
  return on;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void debugf(const char* fmt, ...) {
  if (!debugOn()) return;
  static const auto t0 = std::chrono::steady_clock::now();
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "collab-debug %9.3f %s\n", t, buf);
}

}  // namespace gl::collab
