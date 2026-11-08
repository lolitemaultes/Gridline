// Gridline: K-75: the shading language version for the interface renderer, from the OpenGL version
// a context reports (glGetString(GL_VERSION): "4.3 (Core Profile) Mesa 22.3.6", "3.0 Mesa ...",
// "2.1 Mesa ..."). OpenGL 3.0 and later take GLSL 1.30; an OpenGL 2.x context (a virtual machine's
// graphics or a software renderer capped at 2.1) takes GLSL 1.20. Not used on macOS (always 1.50).
#pragma once
#include <cctype>

namespace gl {

// The major version at the start of a GL_VERSION string (skipping a leading "OpenGL ES "), 0 when none.
inline int glMajorFromVersionString(const char* v) {
  if (!v) return 0;
  while (*v && !std::isdigit(static_cast<unsigned char>(*v))) ++v;
  int major = 0;
  while (*v && std::isdigit(static_cast<unsigned char>(*v)) && major < 1000) major = major * 10 + (*v++ - '0');
  return major;
}

// An unreadable version keeps GLSL 1.30 (what every context got before).
inline const char* glslForVersionString(const char* v) {
  const int major = glMajorFromVersionString(v);
  return major == 1 || major == 2 ? "#version 120" : "#version 130";
}

}  // namespace gl
