// Gridline - names for Objective-C classes made at run time (ObjcClass.h).
#if defined(__APPLE__)
#include "ObjcClass.h"
#include <cstdint>
#include <cstdio>

namespace gl::objc {

namespace {
// One byte per loaded image: its address tells this copy of the code from any other.
char gImageTag = 0;
}  // namespace

std::string uniqueName(const char* stem) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "_%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&gImageTag)));
  return std::string(stem) + buf;
}

}  // namespace gl::objc
#endif
