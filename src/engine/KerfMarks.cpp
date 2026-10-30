// Gridline — Kerf's time and marker settings (see KerfMarks.h).
#include "KerfMarks.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace gl::kerfmarks {

namespace {
constexpr const char* kNamesTag = "kerf-names";

float mono(const Sample& s, int64_t i) { return (s.left[static_cast<size_t>(i)] + s.right[static_cast<size_t>(i)]) * 0.5f; }
// A zero crossing between frames s - 1 and s (s is where it is placed).
bool crossesAt(const Sample& smp, int64_t s) {
  const float x0 = mono(smp, s - 1), x1 = mono(smp, s);
  return (x0 <= 0 && x1 > 0) || (x0 >= 0 && x1 < 0) || x1 == 0;
}
int64_t clampFrame(const Sample* smp, int64_t f) {
  const int64_t n = smp ? static_cast<int64_t>(smp->frames()) : 0;
  return n > 0 ? std::clamp<int64_t>(f, 0, n - 1) : 0;
}
int64_t reachOf(const Sample& smp, double seconds) { return std::max<int64_t>(1, static_cast<int64_t>(smp.sampleRate * seconds)); }
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
}  // namespace

double stepBeats(int snap) {
  switch (snap) {
    case SnapBar: return kBeatsPerBar;
    case SnapHalfBar: return kBeatsPerBar / 2.0;
    case SnapBeat: return 1.0;
    case SnapHalfBeat: return 0.5;
    case SnapThirdBeat: return 1.0 / 3.0;
    case SnapQuarterBeat: return 0.25;
    case SnapSixthBeat: return 1.0 / 6.0;
    case SnapEighthBeat: return 0.125;
    default: return 0.0;
  }
}
bool isGrid(int snap) { return stepBeats(snap) > 0; }

double loopBeats(double seconds, bool fitted, double fittedBeats) {
  if (fitted) return std::max(1.0, fittedBeats);
  if (!(seconds > 0)) return 4.0;
  double beats = 1;
  while (beats * 60.0 / seconds < 80 && beats < 256) beats *= 2;
  return beats;
}

double framesPerBeat(double frames, double beats) { return frames > 0 && beats > 0 ? frames / beats : 0.0; }

int64_t nearestZero(const Sample& smp, int64_t f, int64_t reach) {
  const int64_t n = static_cast<int64_t>(smp.frames());
  for (int64_t d = 0; d <= reach; ++d)
    for (int64_t s : {f - d, f + d}) {
      if (s <= 0 || s >= n) continue;
      if (crossesAt(smp, s)) return s;
    }
  return f;
}

int64_t snapFrame(const Sample* smp, int64_t f, int snap, bool thenZero, double fpb) {
  if (!smp || smp->frames() == 0) return f;
  f = clampFrame(smp, f);
  if (snap == SnapOff) return f;
  if (isGrid(snap) && fpb > 0) {
    const double step = stepBeats(snap) * fpb;
    f = clampFrame(smp, std::llround(std::round(static_cast<double>(f) / step) * step));
    if (!thenZero) return f;
  }
  return nearestZero(*smp, f, reachOf(*smp, 0.005));
}

int64_t nudgeFrame(const Sample* smp, int64_t f, int dir, int snap, bool thenZero, double fpb) {
  if (!smp || smp->frames() == 0) return f;
  dir = dir < 0 ? -1 : 1;
  f = clampFrame(smp, f);
  const int64_t n = static_cast<int64_t>(smp->frames());
  if (isGrid(snap) && fpb > 0) {
    const double step = stepBeats(snap) * fpb;
    // The grid line index of f (a frame within half a frame of a line is on it).
    const double pos = static_cast<double>(f) / step;
    double line = dir > 0 ? std::floor(pos + 0.5 / step + 1e-9) + 1 : std::ceil(pos - 0.5 / step - 1e-9) - 1;
    int64_t to = clampFrame(smp, std::llround(line * step));
    if (thenZero) {
      const int64_t z = nearestZero(*smp, to, reachOf(*smp, 0.005));
      if ((dir > 0 && z > f) || (dir < 0 && z < f)) to = z;
    }
    return to;
  }
  if (snap == SnapZero) {
    const int64_t reach = reachOf(*smp, 0.05);
    for (int64_t s = f + dir; s > 0 && s < n && std::llabs(s - f) <= reach; s += dir)
      if (crossesAt(*smp, s)) return s;
  }
  return clampFrame(smp, f + dir * reachOf(*smp, 0.001));
}

std::string barsBeats(double frame, double fpb, bool ticks) {
  if (!(fpb > 0)) fpb = 1;
  // Whole ticks first (rounded), so 1.9999 beats shows as the next beat, not :095.
  const int64_t allTicks = std::llround(std::max(0.0, frame) / fpb * kTicksPerBeat);
  const int64_t beatsIn = allTicks / kTicksPerBeat, tick = allTicks % kTicksPerBeat;
  const int64_t bar = beatsIn / kBeatsPerBar + 1, beat = beatsIn % kBeatsPerBar + 1;
  char b[48];
  if (ticks) std::snprintf(b, sizeof b, "%lld:%lld:%03lld", static_cast<long long>(bar), static_cast<long long>(beat), static_cast<long long>(tick));
  else std::snprintf(b, sizeof b, "%lld:%lld", static_cast<long long>(bar), static_cast<long long>(beat));
  return b;
}

std::string formatPosition(double frame, int unit, double rate, double fpb) {
  char b[48];
  if (unit == UnitSamples) {
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(frame)));
    return b;
  }
  if (unit == UnitBars) return barsBeats(frame, fpb, true);
  std::snprintf(b, sizeof b, "%.3f s", rate > 0 ? frame / rate : 0.0);
  return b;
}

bool parsePosition(const std::string& text, int unit, double rate, double fpb, double& frame) {
  std::string s = trim(text);
  if (s.empty()) return false;
  if (unit == UnitSeconds) {
    if (s.size() > 1 && (s.back() == 's' || s.back() == 'S')) s = trim(s.substr(0, s.size() - 1));
    std::replace(s.begin(), s.end(), ',', '.');
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != 0 || !std::isfinite(v) || v < 0 || !(rate > 0)) return false;
    frame = v * rate;
    return true;
  }
  if (unit == UnitSamples) {
    for (char c : s)
      if (c < '0' || c > '9') return false;
    if (s.size() > 15) return false;
    frame = static_cast<double>(std::strtoll(s.c_str(), nullptr, 10));
    return true;
  }
  if (!(fpb > 0)) return false;
  double part[3] = {1, 1, 0};
  int n = 0;
  size_t at = 0;
  for (;;) {
    size_t e = s.find_first_of(":.", at);
    const bool last = e == std::string::npos;
    if (last) e = s.size();
    const std::string tok = trim(s.substr(at, e - at));
    if (n >= 3 || tok.empty() || tok.size() > 9) return false;
    for (char c : tok)
      if (c < '0' || c > '9') return false;
    part[n++] = std::strtod(tok.c_str(), nullptr);
    if (last) break;
    at = e + 1;
  }
  if (part[0] < 1 || part[1] < 1 || part[1] > kBeatsPerBar || part[2] >= kTicksPerBeat) return false;
  frame = ((part[0] - 1) * kBeatsPerBar + (part[1] - 1) + part[2] / kTicksPerBeat) * fpb;
  return true;
}

double rulerStepBeats(double pxPerBeat, double minPx) {
  static const double steps[] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256};
  for (double s : steps)
    if (s * pxPerBeat >= minPx) return s;
  return steps[sizeof steps / sizeof steps[0] - 1];
}

double rulerStepSeconds(double pxPerSecond, double minPx) {
  static const double steps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60, 120, 300};
  for (double s : steps)
    if (s * pxPerSecond >= minPx) return s;
  return steps[sizeof steps / sizeof steps[0] - 1];
}

bool hasNames(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl != std::string::npos && data.compare(nl + 1, std::char_traits<char>::length(kNamesTag), kNamesTag) == 0 &&
         (data.size() == nl + 1 + std::char_traits<char>::length(kNamesTag) || data[nl + 1 + std::char_traits<char>::length(kNamesTag)] == '\n');
}

std::string pathOf(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? data : data.substr(0, nl);
}

std::map<int, std::string> namesOf(const std::string& data) {
  std::map<int, std::string> out;
  if (!hasNames(data)) return out;
  std::istringstream in(data);
  std::string line;
  std::getline(in, line);  // path
  std::getline(in, line);  // tag
  while (std::getline(in, line)) {
    const size_t sp = line.find(' ');
    if (sp == std::string::npos || sp == 0 || sp > 3) continue;
    bool digits = true;
    for (size_t i = 0; i < sp; ++i) digits = digits && line[i] >= '0' && line[i] <= '9';
    if (!digits) continue;
    const int slot = std::atoi(line.substr(0, sp).c_str());
    const std::string nm = cleanName(line.substr(sp + 1));
    if (slot >= 0 && slot < 256 && !nm.empty()) out[slot] = nm;
  }
  return out;
}

std::string withNames(const std::string& path, const std::map<int, std::string>& names) {
  std::string out = pathOf(path), body;
  for (const auto& [slot, nm] : names) {
    const std::string c = cleanName(nm);
    if (slot < 0 || slot >= 256 || c.empty()) continue;
    body += "\n" + std::to_string(slot) + " " + c;
  }
  return body.empty() ? out : out + "\n" + kNamesTag + body;
}

std::string cleanName(const std::string& s) {
  std::string o;
  for (char c : s) o += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
  o = trim(o);
  if (o.size() > 64) {
    size_t cut = 64;
    while (cut > 0 && (static_cast<unsigned char>(o[cut]) & 0xC0) == 0x80) --cut;  // not inside a UTF-8 sequence
    o = trim(o.substr(0, cut));
  }
  return o;
}

}  // namespace gl::kerfmarks
