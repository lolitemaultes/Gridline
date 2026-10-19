// Gridline — Time Gate slot banks: 36 time envelopes and 36 volume envelopes
// with per-slot playback options. Shared by the DSP, the editor and the FL import.
//
// Time envelopes: y = 1 plays live, y = 0 reads 8 beats (two 4-beat bars) back:
// read position = now - (1 - y) * 8 beats. Volume envelopes: y = gain 0..1.
// Points are menv points (x in beats, segment shape on the END point).
//
// Stored in PluginState::data. Current format (first line "GB2"), one line per
// edited slot:
//   T3 4 My name|0,1,0,0 2,1,0,0 2,0.75,0,0 4,0.75,0,2|@-1,5,0,0,0,0
//   bank T/V, slot 1..36, length (beats), name, then x,y,tension,mode per point,
//   then @trigSync,posSync,forceHold,link,keyHeld,oneShot (trigSync -1 = the
//   plugin's default; sync index 0 off, 1 1/4, 2 1/2, 3 1, 4 2, 5 4 beats).
// Data without the "GB2" line is the 0.5 format (tg::legacy) and is converted
// when read. Slots without a line use the factory content. parse() does not allocate.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include "MEnv.h"

namespace gl::tg {

namespace legacy {
// The 0.5 slot model (16 slots, x 0..1 of the slot length, time y = read-back
// fraction of the slot length, tension on the START point: f^(2^(-4t))).


inline constexpr int kSlots = 16;
inline constexpr int kMaxPts = 48;

struct Slot {
  int n = 0;
  float len = 4;  // beats
  float x[kMaxPts] = {}, y[kMaxPts] = {}, t[kMaxPts] = {};
  char name[24] = {};
  void clear() { n = 0; }
  bool add(float px, float py, float pt = 0) {
    if (n >= kMaxPts) return false;
    x[n] = px, y[n] = py, t[n] = pt;
    ++n;
    return true;
  }
  void setName(const char* s) {
    std::strncpy(name, s, sizeof name - 1);
    name[sizeof name - 1] = 0;
  }
};

struct Bank {
  Slot time[kSlots], vol[kSlots];
};

// Curve between two points with tension (-1..1): 0 = straight.
inline double tension(double f, double t) {
  if (std::fabs(t) < 1e-4) return f;
  double k = std::pow(2.0, -t * 4.0);
  return std::pow(f, k);
}

// Value of a slot at x in [0,1). Equal x values give a vertical step.
inline double eval(const Slot& s, double x) {
  if (s.n <= 0) return 0;
  if (x <= s.x[0]) return s.y[0];
  for (int i = 0; i + 1 < s.n; ++i) {
    if (x < s.x[i + 1]) {
      double w = s.x[i + 1] - s.x[i];
      if (w <= 1e-9) continue;
      double f = tension((x - s.x[i]) / w, s.t[i]);
      return s.y[i] + (s.y[i + 1] - s.y[i]) * f;
    }
  }
  return s.y[s.n - 1];
}

// Factory slots (original designs).
inline void factory(Bank& b) {
  for (int k = 0; k < kSlots; ++k) b.time[k].clear(), b.vol[k].clear();
  auto stairs = [](Slot& s, double group, double step, double start = 0.0) {  // repeat the first `step` of each `group` (bar fractions)
    s.add(0, 0);
    for (double g = 0; g < 1.0 - 1e-9; g += group)
      for (double st = 0; st < group - 1e-9; st += step) {
        double d = g >= start ? st : 0;
        s.add(static_cast<float>(g + st), static_cast<float>(d));
        s.add(static_cast<float>(std::min(1.0, g + st + step) - 1e-6), static_cast<float>(d));
      }
  };
  Slot* T = b.time;
  T[0].setName("Half speed"), T[0].add(0, 0), T[0].add(1, 0.5f);
  T[1].setName("Quarter speed"), T[1].add(0, 0), T[1].add(1, 0.75f);
  T[2].setName("Tape stop"), T[2].add(0, 0, 0.6f), T[2].add(1, 0.5f);
  T[3].setName("Repeat beat 1"), stairs(T[3], 1.0, 0.25);
  T[4].setName("Repeat 1/8"), stairs(T[4], 0.25, 0.125);
  T[5].setName("Repeat 1/16"), stairs(T[5], 0.25, 0.0625);
  T[6].setName("Stutter end"), stairs(T[6], 0.25, 0.03125, 0.75);
  T[7].setName("Reverse beats");
  for (int k = 0; k < 4; ++k) T[7].add(k * 0.25f, 0), T[7].add(k * 0.25f + 0.2499f, 0.5f);
  T[8].setName("Reverse half"), T[8].add(0, 0), T[8].add(0.4999f, 0), T[8].add(0.5f, 0), T[8].add(0.9999f, 1.0f);
  T[9].setName("Triplet repeat"), stairs(T[9], 0.25, 0.25 / 3);
  T[10].setName("Scratch");
  for (int k = 0; k < 8; ++k) T[10].add(k * 0.125f, k % 2 ? 0.12f : 0.0f, 0.4f);
  T[10].add(1, 0);
  T[11].setName("Slow second half"), T[11].add(0, 0), T[11].add(0.5f, 0), T[11].add(1, 0.25f);
  T[12].setName("Repeat 1/2 bar"), stairs(T[12], 1.0, 0.5);
  T[13].setName("Speed dip"), T[13].add(0, 0), T[13].add(0.5f, 0.2f, -0.5f), T[13].add(1, 0, 0.5f);
  T[14].setName("Tape start"), T[14].add(0, 0.4f, -0.6f), T[14].add(0.6f, 0), T[14].add(1, 0);
  T[15].setName("Dotted repeat"), stairs(T[15], 0.5, 0.1875);
  Slot* V = b.vol;
  auto gate = [](Slot& s, int steps, const char* pattern) {  // pattern of '1'/'0' per step
    for (int k = 0; k < steps; ++k) {
      float a = static_cast<float>(k) / steps, e = static_cast<float>(k + 1) / steps - 1e-4f;
      float v = pattern[k % std::strlen(pattern)] == '1' ? 1.f : 0.f;
      s.add(a, v), s.add(e, v);
    }
  };
  V[0].setName("Gate 1/8"), gate(V[0], 8 * 2, "10");
  V[1].setName("Gate 1/16"), gate(V[1], 16 * 2, "10");
  V[2].setName("Pump");
  for (int k = 0; k < 4; ++k) V[2].add(k * 0.25f, 0), V[2].add(k * 0.25f + 0.2499f, 1, -0.7f);
  V[3].setName("Trance 16ths"), gate(V[3], 16, "1011");
  V[4].setName("Fade in"), V[4].add(0, 0, 0.4f), V[4].add(1, 1);
  V[5].setName("Fade out"), V[5].add(0, 1, -0.4f), V[5].add(1, 0);
  V[6].setName("Off-beat"), gate(V[6], 8, "01");
  V[7].setName("Triplet gate"), gate(V[7], 12, "10");
  V[8].setName("Chop 3-3-2"), gate(V[8], 8, "10010010");
  V[9].setName("Swell");
  for (int k = 0; k < 4; ++k) V[9].add(k * 0.25f, 0.2f, 0.5f), V[9].add(k * 0.25f + 0.2499f, 1);
  V[10].setName("Half bar mute"), gate(V[10], 2, "10");
  V[11].setName("Tremolo 1/16");
  for (int k = 0; k <= 16; ++k) V[11].add(k / 16.0f, k % 2 ? 0.3f : 1.f);
  V[12].setName("Dotted gate"), gate(V[12], 16, "1001001001001010");
  V[13].setName("Last beat cut"), gate(V[13], 4, "1110");
  V[14].setName("Stutter fade");
  for (int k = 0; k < 16; ++k) V[14].add(k / 16.0f, 1 - k / 18.0f), V[14].add((k + 0.5f) / 16.0f, 0);
  V[15].setName("Accents"), gate(V[15], 16, "1000100010001000"), V[15].n = V[15].n;
  for (int k = 0; k < V[15].n; ++k) V[15].y[k] = V[15].y[k] > 0.5f ? 1.f : 0.45f;
}

// Overlays edited slots from data onto the bank.
inline void parse(const char* data, Bank& b) {  // 0.5 reader
  const char* p = data;
  while (p && *p) {
    while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
    if (!*p) break;
    char bank = *p++;
    char* end = nullptr;
    long idx = std::strtol(p, &end, 10);
    p = end;
    double len = std::strtod(p, &end);
    p = end;
    Slot* s = nullptr;
    if ((bank == 'T' || bank == 'V') && idx >= 1 && idx <= kSlots) s = bank == 'T' ? &b.time[idx - 1] : &b.vol[idx - 1];
    while (*p == ' ') ++p;
    char name[24] = {};
    int ni = 0;
    while (*p && *p != '|' && *p != '\n') {
      if (ni < 23) name[ni++] = *p;
      ++p;
    }
    if (s) {
      s->clear();
      s->len = static_cast<float>(len > 0.05 ? std::min(len, 16.0) : 4.0);
      s->setName(name);
    }
    if (*p == '|') ++p;
    while (*p && *p != '\n') {
      double x = std::strtod(p, &end);
      if (end == p) break;
      p = end;
      if (*p == ',') ++p;
      double y = std::strtod(p, &end);
      p = end;
      double t = 0;
      if (*p == ',') {
        ++p;
        t = std::strtod(p, &end);
        p = end;
      }
      if (s) s->add(static_cast<float>(std::fmin(1.0, std::fmax(0.0, x))), static_cast<float>(std::fmin(1.0, std::fmax(0.0, y))),
                    static_cast<float>(std::fmin(1.0, std::fmax(-1.0, t))));
      while (*p == ' ') ++p;
    }
  }
}


// Reads one 0.5 line into a slot (any slot index).
inline void parseOne(const char* line, Slot& out) {
  Bank b;
  for (int k = 0; k < kSlots; ++k) b.time[k].clear(), b.vol[k].clear();
  parse(line, b);
  long idx = std::strtol(line + 1, nullptr, 10);
  if (idx < 1 || idx > kSlots) return;
  out = line[0] == 'T' ? b.time[idx - 1] : b.vol[idx - 1];
}
}  // namespace legacy

inline constexpr int kSlots = 36;
inline constexpr int kMaxPts = 360;
inline constexpr double kTimeRange = 8.0;  // beats reachable by a time envelope
inline constexpr double kSyncBeats[6] = {0, 0.25, 0.5, 1, 2, 4};
enum ForceHold { ForceOff, ForceLast, ForceNext };

using Env = menv::Env<kMaxPts>;

struct Slot {
  Env env;
  float len = 4;     // envelope length (beats)
  char name[32] = {};
  int trig = -1;     // trigger sync index (-1 = plugin default)
  int pos = 5;       // position sync index (0 = restart on trigger, 5 = locked to the 4-beat bar)
  int force = ForceOff;
  bool link = false, keyHeld = false, oneShot = false;
  void setName(const char* s) {
    std::strncpy(name, s, sizeof name - 1);
    name[sizeof name - 1] = 0;
  }
  void prepare() { env.prepare(); }
  double eval(double beats) const { return env.eval(beats); }
  // Flat line at y over the slot length.
  void flat(float y) {
    env.clear();
    env.add(0, y);
    env.add(len, y, 0, menv::Hold);
  }
};

struct Bank {
  Slot time[kSlots], vol[kSlots];
};

// Delay (beats) of a time envelope value, and back.
inline double delayBeats(double y) { return (1.0 - std::clamp(y, 0.0, 1.0)) * kTimeRange; }
inline double timeY(double delay) { return 1.0 - std::clamp(delay / kTimeRange, 0.0, 1.0); }

// Legacy slot → current slot (time banks: read-back fraction of len → reference-style y).
inline void fromLegacy(const legacy::Slot& o, bool time, Slot& s) {
  s.env.clear();
  s.len = o.len;
  s.setName(o.name);
  for (int i = 0; i < o.n; ++i) {
    double y = time ? timeY(static_cast<double>(o.y[i]) * o.len) : o.y[i];
    // Old tension bent the segment that starts at a point; now it lives on the end point.
    float t = i > 0 ? std::clamp(o.t[i - 1] * 0.4f, -1.0f, 1.0f) : 0.f;
    s.env.add(static_cast<double>(o.x[i]) * o.len, static_cast<float>(y), t, menv::SingleCurve);
  }
  if (s.env.n == 0) s.flat(1.f);
  s.prepare();
}

// Factory slots (original designs): slots 1-16 keep the 0.5 designs, 17-36 are new.
inline void factory(Bank& b) {
  legacy::Bank lb;
  legacy::factory(lb);
  for (int k = 0; k < kSlots; ++k) {
    Slot& t = b.time[k];
    Slot& v = b.vol[k];
    t.env.clear(), v.env.clear();
    t.len = v.len = 4;
    t.name[0] = v.name[0] = 0;
    t.trig = v.trig = -1, t.pos = v.pos = 5, t.force = v.force = ForceOff;
    t.link = t.keyHeld = t.oneShot = v.link = v.keyHeld = v.oneShot = false;
    if (k < legacy::kSlots) {
      fromLegacy(lb.time[k], true, t);
      fromLegacy(lb.vol[k], false, v);
    } else {
      t.flat(1.f);
      v.flat(1.f);
    }
  }
  using namespace menv;
  auto T = [&](int k, const char* name) -> Env& {
    b.time[k].setName(name);
    b.time[k].env.clear();
    return b.time[k].env;
  };
  auto V = [&](int k, const char* name) -> Env& {
    b.vol[k].setName(name);
    b.vol[k].env.clear();
    return b.vol[k].env;
  };
  auto d = [](double beats) { return static_cast<float>(timeY(beats)); };
  // Repeats built from hold segments: replay the first `step` beats of every `group` beats.
  auto repeat = [&](Env& e, double group, double step) {
    for (double g = 0; g < 4 - 1e-9; g += group) {
      e.add(g, 1.f, 0, g == 0 ? SingleCurve : Hold);
      for (double s = step; s < group - 1e-9; s += step) e.add(g + s, d(s), 0, Hold);
    }
    e.add(4, e.y[e.n - 1], 0, Hold);
  };
  repeat(T(16, "Beat one x4"), 4, 1);
  repeat(T(17, "Loop 1/4 beat"), 1, 0.25);
  repeat(T(18, "Loop 1/3 beat"), 1, 1.0 / 3);
  repeat(T(19, "Loop half bar"), 4, 2);
  {  // stairs: a stuttering hold of 1/8 beats (32 steps over the bar)
    Env& e = T(20, "Stair stutter");
    e.add(0, 1.f), e.add(4, d(4.0 - 0.125), 0.425f, Stairs);
  }
  {  // smooth slow-down over the bar
    Env& e = T(21, "Wind down");
    e.add(0, 1.f), e.add(4, d(2.0), -0.35f, SingleCurve);
  }
  {
    Env& e = T(22, "Wind up");
    e.add(0, d(2.0)), e.add(4, 1.f, 0.35f, SingleCurve);
  }
  {  // back-and-forth scratch (sine)
    Env& e = T(23, "Rub");
    e.add(0, 1.f), e.add(4, d(0.5), -0.6f, Wave);
  }
  {  // every beat played backwards: the delay rises 2 beats per beat
    Env& e = T(24, "Backwards beats");
    for (int k = 0; k < 4; ++k) e.add(k, 1.f, 0, Hold), e.add(k + 1, d(2.0), 0, SingleCurve);
  }
  {  // octave down for half a bar, then live
    Env& e = T(25, "Drop half bar");
    e.add(0, 1.f), e.add(2, d(1.0)), e.add(2, 1.f, 0, Hold), e.add(4, 1.f, 0, Hold);
  }
  {
    Env& e = T(26, "Gate repeat 1/8");
    for (int k = 0; k < 8; ++k) e.add(k * 0.5, d(k % 2 ? 0.5 : 0.0), 0, Hold), e.add(k * 0.5 + 0.5, d(k % 2 ? 0.5 : 0.0), 0, Hold);
  }
  {  // wobble around a short delay
    Env& e = T(27, "Wobble");
    e.add(0, d(0.05)), e.add(4, d(0.12), 0.3f, Wave);
  }
  {  // stutter that tightens towards the end of the bar
    Env& e = T(28, "Roll in");
    e.add(0, 1.f), e.add(2, 1.f, 0, Hold), e.add(3, d(0.5), 0.6f, Stairs), e.add(4, d(0.25), 0.25f, Stairs);
  }
  // Volume additions.
  {
    Env& e = V(16, "Pulse 1/16");
    e.add(0, 1.f), e.add(4, 0.f, 0.51f, Pulse);
  }
  {
    Env& e = V(17, "Pulse 1/12");
    e.add(0, 1.f), e.add(4, 0.f, 0.455f, Pulse);
  }
  {
    Env& e = V(18, "Tremolo sine");
    e.add(0, 0.f), e.add(4, 1.f, -0.5f, Wave);
  }
  {
    Env& e = V(19, "Duck beats");
    for (int k = 0; k < 4; ++k) e.add(k, 0.f, 0, Hold), e.add(k + 0.75, 1.f, -0.6f, SingleCurve), e.add(k + 1, 1.f, 0, Hold);
  }
  {
    Env& e = V(20, "Steps up");
    e.add(0, 0.f), e.add(4, 1.f, 0.6f, Stairs);
  }
  {
    Env& e = V(21, "Swell smooth");
    e.add(0, 0.f), e.add(2, 1.f, 0, Smooth), e.add(4, 0.f, 0, Smooth);
  }
  {
    Env& e = V(22, "Half-sine pump");
    for (int k = 0; k < 4; ++k) e.add(k, 0.f, 0, Hold), e.add(k + 1, 1.f, 0.8f, HalfSine);
  }
  for (int k = 0; k < kSlots; ++k) b.time[k].prepare(), b.vol[k].prepare();
}

namespace detail {
inline const char* skipSpaces(const char* p) {
  while (*p == ' ') ++p;
  return p;
}
}  // namespace detail

// Overlays one current-format slot line (p at the bank letter); returns the end of the line.
inline const char* parseLine(const char* p, Bank& b) {
  char bank = *p++;
  char* end = nullptr;
  long idx = std::strtol(p, &end, 10);
  p = end;
  double len = std::strtod(p, &end);
  p = detail::skipSpaces(end);
  Slot* s = nullptr;
  if ((bank == 'T' || bank == 'V') && idx >= 1 && idx <= kSlots) s = bank == 'T' ? &b.time[idx - 1] : &b.vol[idx - 1];
  char name[32] = {};
  int ni = 0;
  while (*p && *p != '|' && *p != '\n') {
    if (ni < 31) name[ni++] = *p;
    ++p;
  }
  if (s) {
    s->env.clear();
    s->len = static_cast<float>(len > 0.05 ? std::min(len, 16.0) : 4.0);
    s->setName(name);
    s->trig = -1, s->pos = 5, s->force = ForceOff, s->link = s->keyHeld = s->oneShot = false;
  }
  if (*p == '|') ++p;
  while (*p && *p != '\n' && *p != '|') {
    double x = std::strtod(p, &end);
    if (end == p) break;
    p = end;
    double v[3] = {0, 0, 0};
    for (int k = 0; k < 3 && *p == ','; ++k) {
      ++p;
      v[k] = std::strtod(p, &end);
      p = end;
    }
    if (s)
      s->env.add(std::clamp(x, 0.0, 16.0), static_cast<float>(std::clamp(v[0], 0.0, 1.0)), static_cast<float>(std::clamp(v[1], -1.0, 1.0)),
                 static_cast<uint8_t>(std::clamp(static_cast<int>(v[2]), 0, menv::ModeCount - 1)));
    p = detail::skipSpaces(p);
  }
  if (*p == '|') {
    ++p;
    if (*p == '@') ++p;
    long o[6] = {-1, 5, 0, 0, 0, 0};
    for (int k = 0; k < 6; ++k) {
      long v = std::strtol(p, &end, 10);
      if (end == p) break;
      o[k] = v;
      p = end;
      if (*p == ',') ++p;
    }
    if (s) {
      s->trig = static_cast<int>(std::clamp(o[0], -1L, 5L));
      s->pos = static_cast<int>(std::clamp(o[1], 0L, 5L));
      s->force = static_cast<int>(std::clamp(o[2], 0L, 2L));
      s->link = o[3] != 0, s->keyHeld = o[4] != 0, s->oneShot = o[5] != 0;
    }
  }
  if (s) {
    if (s->env.n == 0) s->flat(1.f);
    s->prepare();
  }
  while (*p && *p != '\n') ++p;
  return p;
}

inline bool isCurrentFormat(const char* data) { return data && std::strncmp(data, "GB2", 3) == 0; }

// Overlays the edited slots from data onto the bank (either format). Does not allocate.
inline void parse(const char* data, Bank& b) {
  if (!data || !*data) return;
  if (!isCurrentFormat(data)) {  // 0.5 data: read each line with the legacy reader and convert
    const char* p = data;
    while (*p) {
      while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
      if (!*p) break;
      char bank = *p;
      long idx = std::strtol(p + 1, nullptr, 10);
      const char* e = p;
      while (*e && *e != '\n') ++e;
      if ((bank == 'T' || bank == 'V') && idx >= 1 && idx <= legacy::kSlots) {
        char line[4096];
        size_t n = std::min<size_t>(static_cast<size_t>(e - p), sizeof line - 1);
        std::memcpy(line, p, n);
        line[n] = 0;
        legacy::Slot ls;
        legacy::parseOne(line, ls);
        fromLegacy(ls, bank == 'T', bank == 'T' ? b.time[idx - 1] : b.vol[idx - 1]);
      }
      p = e;
    }
    return;
  }
  const char* p = data + 3;
  while (*p) {
    while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
    if (!*p) break;
    p = parseLine(p, b);
  }
}

// One slot as a current-format data line (UI thread).
inline std::string serialize(char bank, int idx, const Slot& s) {
  std::string out;
  char buf[96];
  std::snprintf(buf, sizeof buf, "%c%d %g %s|", bank, idx + 1, static_cast<double>(s.len), s.name);
  out += buf;
  for (int i = 0; i < s.env.n; ++i) {
    std::snprintf(buf, sizeof buf, "%s%.9g,%.7g,%.5g,%d", i ? " " : "", s.env.x[i], static_cast<double>(s.env.y[i]), static_cast<double>(s.env.t[i]),
                  static_cast<int>(s.env.mode[i]));
    out += buf;
  }
  std::snprintf(buf, sizeof buf, "|@%d,%d,%d,%d,%d,%d", s.trig, s.pos, s.force, s.link ? 1 : 0, s.keyHeld ? 1 : 0, s.oneShot ? 1 : 0);
  return out + buf;
}

// Current-format data equivalent to `data` (legacy lines converted).
inline std::string upgrade(const std::string& data) {
  if (isCurrentFormat(data.c_str())) return data;
  std::string out = "GB2\n";
  if (data.empty()) return out;
  auto bk = std::make_unique<Bank>();
  factory(*bk);
  parse(data.c_str(), *bk);
  size_t st = 0;
  while (st < data.size()) {  // re-emit exactly the slots the legacy data named
    size_t e = data.find('\n', st);
    std::string line = data.substr(st, e == std::string::npos ? std::string::npos : e - st);
    if (line.size() > 1 && (line[0] == 'T' || line[0] == 'V')) {
      long idx = std::strtol(line.c_str() + 1, nullptr, 10);
      if (idx >= 1 && idx <= legacy::kSlots)
        out += serialize(line[0], static_cast<int>(idx - 1), line[0] == 'T' ? bk->time[idx - 1] : bk->vol[idx - 1]) + "\n";
    }
    if (e == std::string::npos) break;
    st = e + 1;
  }
  return out;
}

namespace detail {
inline std::string without(const std::string& data, char bank, int idx) {
  std::string src = upgrade(data);
  std::string out = "GB2\n";
  char key[8];
  std::snprintf(key, sizeof key, "%c%d ", bank, idx + 1);
  size_t st = 4;
  while (st < src.size()) {
    size_t e = src.find('\n', st);
    std::string line = src.substr(st, e == std::string::npos ? std::string::npos : e - st);
    if (line.compare(0, std::strlen(key), key) != 0 && !line.empty()) out += line + "\n";
    if (e == std::string::npos) break;
    st = e + 1;
  }
  return out;
}
}  // namespace detail

// Replaces (or adds) one slot's line in a data string (result is current format).
inline std::string replaceSlot(const std::string& data, char bank, int idx, const Slot& s) {
  return detail::without(data, bank, idx) + serialize(bank, idx, s) + "\n";
}

// Removes one slot's line (back to its factory content).
inline std::string resetSlot(const std::string& data, char bank, int idx) { return detail::without(data, bank, idx); }

}  // namespace gl::tg
