// Gridline — K-123: Recording card texts. See RecordInfo.h.
#include "RecordInfo.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gl::recinfo {

std::string modeLine(bool recording, bool punch, bool loop, int pass) {
  std::string m = punch ? "punch" : loop ? "loop" : "";
  if (!recording) return m;
  const std::string p = "pass " + std::to_string(std::max(1, pass));
  return m.empty() ? p : m + ", " + p;
}

double minutesLeft(double freeBytes, double sampleRate, int tracks, int bytesPerSample) {
  const double perSec = std::max(1.0, sampleRate) * 2.0 * std::max(1, bytesPerSample) * std::max(1, tracks);
  return std::max(0.0, freeBytes) / perSec / 60.0;
}

std::string durationText(double minutes) {
  if (!(minutes >= 1.0)) return "under a minute";
  const long m = static_cast<long>(std::floor(minutes));
  if (m < 60) return std::to_string(m) + " min";
  return std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min";
}

std::string clock(double seconds) {
  const double s = std::max(0.0, seconds);
  const long tenths = static_cast<long>(std::floor(s * 10.0 + 1e-9));
  const long h = tenths / 36000, m = (tenths / 600) % 60, sec = (tenths / 10) % 60, t = tenths % 10;
  char b[32];
  if (h > 0) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld.%ld", h, m, sec, t);
  else std::snprintf(b, sizeof b, "%02ld:%02ld.%ld", m, sec, t);
  return b;
}

View view(int form, bool open, bool hidden, bool windowOpen, bool recording, bool started) {
  View v;
  if (form < kCard || form > kKeepWindow) form = kCard;
  v.window = windowOpen && !(started && recording && form != kKeepWindow);
  if (!open) return v;
  if (hidden) {
    v.pill = true;
    return v;
  }
  if (form == kStrip) v.strip = true;
  else v.card = !(form == kKeepWindow && v.window);
  return v;
}

}  // namespace gl::recinfo
