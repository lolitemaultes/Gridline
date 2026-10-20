// Gridline — "edison" pack: audio editor scripts (see edison_script.h and
// docs/scripting.md). Buffers are Lua userdata holding a length and the
// samples; every operation is a C loop charged to the VM's budget.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include "edison_internal.h"
#include "edison_script.h"
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;
using script::Vm;

namespace {

constexpr const char* kBufMeta = "gl.Buffer";
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Buf {
  int64_t n = 0;
  float* d = nullptr;
};

void charge(lua_State* L, int64_t samples, int perUnit = 16) {
  Vm::from(L)->charge(L, 1 + static_cast<uint64_t>(std::max<int64_t>(0, samples)) / static_cast<uint64_t>(perUnit));
}

Buf bufAt(void* p) {
  Buf b;
  std::memcpy(&b.n, p, sizeof b.n);
  b.d = reinterpret_cast<float*>(static_cast<char*>(p) + sizeof(int64_t));
  return b;
}
Buf checkBuf(lua_State* L, int arg) { return bufAt(luaL_checkudata(L, arg, kBufMeta)); }
bool isBuf(lua_State* L, int idx) { return luaL_testudata(L, idx, kBufMeta) != nullptr; }

// Pushes a new zeroed buffer of n samples.
Buf newBuf(lua_State* L, int64_t n) {
  if (n < 0 || n > AudioScript::kMaxFrames) luaL_error(L, "buffer length out of range (0..%I)", static_cast<lua_Integer>(AudioScript::kMaxFrames));
  charge(L, n, 64);
  void* p = lua_newuserdatauv(L, sizeof(int64_t) + static_cast<size_t>(n) * sizeof(float), 0);
  std::memcpy(p, &n, sizeof n);
  Buf b = bufAt(p);
  if (n > 0) std::memset(b.d, 0, static_cast<size_t>(n) * sizeof(float));
  luaL_setmetatable(L, kBufMeta);
  return b;
}
Buf pushCopy(lua_State* L, const float* src, int64_t n) {
  Buf b = newBuf(L, n);
  if (n > 0) std::memcpy(b.d, src, static_cast<size_t>(n) * sizeof(float));
  return b;
}

double num(lua_State* L, int arg) {
  double v = luaL_checknumber(L, arg);
  if (!std::isfinite(v)) luaL_argerror(L, arg, "number must be finite");
  return v;
}
double optNum(lua_State* L, int arg, double def) { return lua_isnoneornil(L, arg) ? def : num(L, arg); }
float clean(double v) {
  // Results stay finite (NaN / inf would poison the file).
  if (!std::isfinite(v)) return 0.f;
  return static_cast<float>(std::clamp(v, -1e6, 1e6));
}

// Optional 1-based inclusive range i..j at args (ai, ai + 1) -> [a, b).
struct Span {
  int64_t a = 0, b = 0;
  int64_t len() const { return b - a; }
};
Span span(lua_State* L, const Buf& buf, int ai) {
  Span s{0, buf.n};
  if (!lua_isnoneornil(L, ai)) {
    lua_Integer i = luaL_checkinteger(L, ai);
    if (i < 1 || i > buf.n + 1) luaL_argerror(L, ai, "start index out of range");
    s.a = i - 1;
  }
  if (!lua_isnoneornil(L, ai + 1)) {
    lua_Integer j = luaL_checkinteger(L, ai + 1);
    if (j < s.a || j > buf.n) luaL_argerror(L, ai + 1, "end index out of range");
    s.b = j;
  }
  return s;
}
int64_t indexArg(lua_State* L, int arg, const Buf& b) {
  lua_Integer i = luaL_checkinteger(L, arg);
  if (i < 1 || i > b.n) luaL_argerror(L, arg, "sample index out of range");
  return i - 1;
}

Curve curveArg(lua_State* L, int arg) {
  if (lua_isnoneornil(L, arg)) return Curve::Linear;
  std::string c = luaL_checkstring(L, arg);
  if (c == "linear") return Curve::Linear;
  if (c == "fast") return Curve::Fast;
  if (c == "slow") return Curve::Slow;
  if (c == "s") return Curve::SCurve;
  if (c == "power") return Curve::EqualPower;
  luaL_argerror(L, arg, "curve: linear, fast, slow, s or power");
  return Curve::Linear;
}
double rateArg(lua_State* L, int arg) {
  double r = num(L, arg);
  if (r < 1000 || r > 768000) luaL_argerror(L, arg, "sample rate 1000..768000");
  return r;
}

// ------------------------------------------------------------- basics ----
int b_len(lua_State* L) {
  lua_pushinteger(L, checkBuf(L, 1).n);
  return 1;
}
int b_get(lua_State* L) {
  Buf b = checkBuf(L, 1);
  lua_pushnumber(L, b.d[indexArg(L, 2, b)]);
  return 1;
}
int b_set(lua_State* L) {
  Buf b = checkBuf(L, 1);
  int64_t i = indexArg(L, 2, b);
  b.d[i] = clean(num(L, 3));
  return 0;
}
int b_copy(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  pushCopy(L, b.d + s.a, s.len());
  return 1;
}
int b_resized(lua_State* L) {
  Buf b = checkBuf(L, 1);
  lua_Integer n = luaL_checkinteger(L, 2);
  if (n < 0 || n > AudioScript::kMaxFrames) luaL_argerror(L, 2, "length out of range");
  charge(L, n);
  Buf o = newBuf(L, n);
  b = checkBuf(L, 1);
  std::memcpy(o.d, b.d, static_cast<size_t>(std::min<int64_t>(n, b.n)) * sizeof(float));
  return 1;
}
int b_totable(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  if (s.len() > (1 << 22)) luaL_error(L, "too many samples for a table (use buffer operations)");
  charge(L, s.len(), 1);
  lua_createtable(L, static_cast<int>(s.len()), 0);
  for (int64_t i = 0; i < s.len(); ++i) {
    lua_pushnumber(L, b.d[s.a + i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
int b_fill(lua_State* L) {
  Buf b = checkBuf(L, 1);
  float v = clean(num(L, 2));
  Span s = span(L, b, 3);
  charge(L, s.len());
  std::fill(b.d + s.a, b.d + s.b, v);
  return 0;
}
int b_scale(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double g = num(L, 2);
  Span s = span(L, b, 3);
  charge(L, s.len());
  for (int64_t i = s.a; i < s.b; ++i) b.d[i] = clean(b.d[i] * g);
  return 0;
}
int b_gainDb(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double g = std::pow(10.0, std::clamp(num(L, 2), -200.0, 60.0) / 20.0);
  Span s = span(L, b, 3);
  charge(L, s.len());
  for (int64_t i = s.a; i < s.b; ++i) b.d[i] = clean(b.d[i] * g);
  return 0;
}
int b_offset(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double v = num(L, 2);
  Span s = span(L, b, 3);
  charge(L, s.len());
  for (int64_t i = s.a; i < s.b; ++i) b.d[i] = clean(b.d[i] + v);
  return 0;
}
// b:add(other [, gain [, at]]): b[at + k] += other[1 + k] * gain.
int b_add(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Buf o = checkBuf(L, 2);
  double g = optNum(L, 3, 1.0);
  lua_Integer at = luaL_optinteger(L, 4, 1);
  if (at < 1 || at > b.n + 1) luaL_argerror(L, 4, "position out of range");
  const int64_t n = std::min<int64_t>(o.n, b.n - (at - 1));
  charge(L, n);
  for (int64_t k = 0; k < n; ++k) b.d[at - 1 + k] = clean(b.d[at - 1 + k] + o.d[k] * g);
  return 0;
}
int b_mul(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Buf o = checkBuf(L, 2);
  const int64_t n = std::min(o.n, b.n);
  charge(L, n);
  for (int64_t k = 0; k < n; ++k) b.d[k] = clean(static_cast<double>(b.d[k]) * o.d[k]);
  return 0;
}
// b:ramp(g0, g1 [, curve [, i, j]]): multiplies by a gain going from g0 to g1.
int b_ramp(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double g0 = num(L, 2), g1 = num(L, 3);
  Curve c = curveArg(L, 4);
  Span s = span(L, b, 5);
  charge(L, s.len(), 8);
  const int64_t n = s.len();
  for (int64_t k = 0; k < n; ++k) {
    double x = n > 1 ? static_cast<double>(k) / static_cast<double>(n - 1) : 1.0;
    double g = g0 + (g1 - g0) * curveGain(c, x);
    b.d[s.a + k] = clean(b.d[s.a + k] * g);
  }
  return 0;
}
int b_reverse(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  std::reverse(b.d + s.a, b.d + s.b);
  return 0;
}
int b_invert(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  for (int64_t i = s.a; i < s.b; ++i) b.d[i] = -b.d[i];
  return 0;
}
int b_abs(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  for (int64_t i = s.a; i < s.b; ++i) b.d[i] = std::fabs(b.d[i]);
  return 0;
}
int b_clip(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double lo = optNum(L, 2, -1.0), hi = optNum(L, 3, 1.0);
  if (hi < lo) std::swap(lo, hi);
  charge(L, b.n);
  for (int64_t i = 0; i < b.n; ++i) b.d[i] = static_cast<float>(std::clamp(static_cast<double>(b.d[i]), lo, hi));
  return 0;
}
int b_softclip(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double drive = std::clamp(optNum(L, 2, 1.0), 0.01, 100.0);
  const double norm = 1.0 / std::tanh(drive);
  charge(L, b.n, 4);
  for (int64_t i = 0; i < b.n; ++i) b.d[i] = clean(std::tanh(b.d[i] * drive) * norm);
  return 0;
}
int b_quantize(lua_State* L) {
  Buf b = checkBuf(L, 1);
  lua_Integer bits = luaL_checkinteger(L, 2);
  if (bits < 1 || bits > 24) luaL_argerror(L, 2, "bits 1..24");
  const double q = std::ldexp(1.0, static_cast<int>(bits) - 1);
  charge(L, b.n);
  for (int64_t i = 0; i < b.n; ++i) b.d[i] = clean(std::round(b.d[i] * q) / q);
  return 0;
}
int b_removeDc(lua_State* L) {
  Buf b = checkBuf(L, 1);
  charge(L, b.n, 8);
  double sum = 0;
  for (int64_t i = 0; i < b.n; ++i) sum += b.d[i];
  const double m = b.n > 0 ? sum / static_cast<double>(b.n) : 0.0;
  for (int64_t i = 0; i < b.n; ++i) b.d[i] = clean(b.d[i] - m);
  lua_pushnumber(L, m);
  return 1;
}

// ------------------------------------------------------------ analysis ----
int b_peak(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  float pk = 0;
  int64_t at = s.a;
  for (int64_t i = s.a; i < s.b; ++i)
    if (std::fabs(b.d[i]) > pk) pk = std::fabs(b.d[i]), at = i;
  lua_pushnumber(L, pk);
  if (s.len() > 0)
    lua_pushinteger(L, at + 1);
  else
    lua_pushnil(L);
  return 2;
}
int b_rms(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  double acc = 0;
  for (int64_t i = s.a; i < s.b; ++i) acc += static_cast<double>(b.d[i]) * b.d[i];
  lua_pushnumber(L, s.len() > 0 ? std::sqrt(acc / static_cast<double>(s.len())) : 0.0);
  return 1;
}
int b_sum(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  double acc = 0;
  for (int64_t i = s.a; i < s.b; ++i) acc += b.d[i];
  lua_pushnumber(L, acc);
  return 1;
}
int b_minmax(lua_State* L) {
  Buf b = checkBuf(L, 1);
  Span s = span(L, b, 2);
  charge(L, s.len());
  if (s.len() == 0) {
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
  }
  auto [mn, mx] = std::minmax_element(b.d + s.a, b.d + s.b);
  lua_pushnumber(L, *mn);
  lua_pushnumber(L, *mx);
  return 2;
}
// Indices i (1-based) where the signal crosses zero between i - 1 and i.
int b_zeroCrossings(lua_State* L) {
  Buf b = checkBuf(L, 1);
  std::string dir = luaL_optstring(L, 2, "any");
  if (dir != "any" && dir != "up" && dir != "down") luaL_argerror(L, 2, "any, up or down");
  lua_Integer max = luaL_optinteger(L, 3, 1 << 20);
  if (max < 0 || max > (1 << 22)) luaL_argerror(L, 3, "0..4194304");
  charge(L, b.n);
  lua_newtable(L);
  lua_Integer k = 0;
  for (int64_t i = 1; i < b.n && k < max; ++i) {
    const bool up = b.d[i - 1] < 0 && b.d[i] >= 0, down = b.d[i - 1] >= 0 && b.d[i] < 0;
    if ((dir == "any" && (up || down)) || (dir == "up" && up) || (dir == "down" && down)) {
      lua_pushinteger(L, i + 1);
      lua_rawseti(L, -2, ++k);
    }
  }
  charge(L, k, 1);
  return 1;
}
int b_nearestZero(lua_State* L) {
  Buf b = checkBuf(L, 1);
  int64_t i = indexArg(L, 2, b);
  lua_Integer radius = luaL_optinteger(L, 3, 4096);
  if (radius < 0 || radius > AudioScript::kMaxFrames) luaL_argerror(L, 3, "radius out of range");
  charge(L, radius * 2);
  auto crosses = [&](int64_t k) { return k >= 1 && k < b.n && ((b.d[k - 1] < 0) != (b.d[k] < 0)); };
  for (int64_t r = 0; r <= radius; ++r) {
    if (crosses(i - r)) {
      lua_pushinteger(L, i - r + 1);
      return 1;
    }
    if (crosses(i + r)) {
      lua_pushinteger(L, i + r + 1);
      return 1;
    }
  }
  lua_pushnil(L);
  return 1;
}
// Envelope follower of |x| (attack / release in ms) as a new buffer.
int b_envelope(lua_State* L) {
  Buf b = checkBuf(L, 1);
  double att = std::clamp(num(L, 2), 0.0, 10000.0), rel = std::clamp(num(L, 3), 0.0, 10000.0);
  double rate = rateArg(L, 4);
  charge(L, b.n, 4);
  Buf o = newBuf(L, b.n);
  b = checkBuf(L, 1);
  const double ca = att > 0 ? std::exp(-1.0 / (att * 0.001 * rate)) : 0.0;
  const double cr = rel > 0 ? std::exp(-1.0 / (rel * 0.001 * rate)) : 0.0;
  double e = 0;
  for (int64_t i = 0; i < b.n; ++i) {
    double x = std::fabs(b.d[i]);
    double c = x > e ? ca : cr;
    e = x + c * (e - x);
    o.d[i] = clean(e);
  }
  return 1;
}

// ----------------------------------------------------------- generators ----
// b:tone(shape, hz, rate [, amp [, phase]]) writes a wave; returns the end phase (cycles).
int b_tone(lua_State* L) {
  Buf b = checkBuf(L, 1);
  std::string shape = luaL_checkstring(L, 2);
  double hz = std::clamp(num(L, 3), 0.0, 400000.0);
  double rate = rateArg(L, 4);
  double amp = optNum(L, 5, 1.0);
  double ph = optNum(L, 6, 0.0);
  int kind = shape == "sine" ? 0 : shape == "square" ? 1 : shape == "saw" ? 2 : shape == "triangle" ? 3 : -1;
  if (kind < 0) luaL_argerror(L, 2, "sine, square, saw or triangle");
  charge(L, b.n, 4);
  ph -= std::floor(ph);
  const double inc = hz / rate;
  for (int64_t i = 0; i < b.n; ++i) {
    double v = 0;
    switch (kind) {
      case 0: v = std::sin(kTwoPi * ph); break;
      case 1: v = ph < 0.5 ? 1.0 : -1.0; break;
      case 2: v = 2.0 * ph - 1.0; break;
      default: v = ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph; break;
    }
    b.d[i] = clean(v * amp);
    ph += inc;
    ph -= std::floor(ph);
  }
  lua_pushnumber(L, ph);
  return 1;
}
// b:noise(kind [, amp [, seed]]) writes white, pink or brown noise (deterministic per seed).
int b_noise(lua_State* L) {
  Buf b = checkBuf(L, 1);
  std::string kind = luaL_optstring(L, 2, "white");
  double amp = optNum(L, 3, 1.0);
  lua_Integer seed = luaL_optinteger(L, 4, 1);
  int k = kind == "white" ? 0 : kind == "pink" ? 1 : kind == "brown" ? 2 : -1;
  if (k < 0) luaL_argerror(L, 2, "white, pink or brown");
  charge(L, b.n, 4);
  uint64_t s = static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
  auto next = [&s]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
  };
  double p0 = 0, p1 = 0, p2 = 0, br = 0;
  for (int64_t i = 0; i < b.n; ++i) {
    double w = next(), v = w;
    if (k == 1) {  // pink: three leaky integrators (Voss-like approximation)
      p0 = 0.99765 * p0 + w * 0.0990460;
      p1 = 0.96300 * p1 + w * 0.2965164;
      p2 = 0.57000 * p2 + w * 1.0526913;
      v = (p0 + p1 + p2 + w * 0.1848) * 0.25;
    } else if (k == 2) {  // brown: leaky integral of white
      br = 0.995 * br + w * 0.06;
      v = br * 2.5;
    }
    b.d[i] = clean(std::clamp(v, -1.0, 1.0) * amp);
  }
  return 0;
}

// -------------------------------------------------------------- filters ----
int onePole(lua_State* L, bool high) {
  Buf b = checkBuf(L, 1);
  double hz = num(L, 2), rate = rateArg(L, 3);
  hz = std::clamp(hz, 1.0, rate * 0.49);
  charge(L, b.n, 8);
  const double a = std::exp(-kTwoPi * hz / rate);
  double y = 0;
  for (int64_t i = 0; i < b.n; ++i) {
    y = b.d[i] * (1.0 - a) + y * a;
    b.d[i] = clean(high ? b.d[i] - y : y);
  }
  return 0;
}
int b_lowpass(lua_State* L) { return onePole(L, false); }
int b_highpass(lua_State* L) { return onePole(L, true); }
// b:biquad(kind, hz, q, gainDb, rate) — RBJ cookbook sections.
int b_biquad(lua_State* L) {
  Buf b = checkBuf(L, 1);
  std::string kind = luaL_checkstring(L, 2);
  double hz = num(L, 3), q = std::clamp(num(L, 4), 0.05, 50.0), db = std::clamp(num(L, 5), -48.0, 48.0);
  double rate = rateArg(L, 6);
  hz = std::clamp(hz, 5.0, rate * 0.49);
  const double A = std::pow(10.0, db / 40.0), w = kTwoPi * hz / rate, cw = std::cos(w), sw = std::sin(w), al = sw / (2 * q);
  double b0, b1, b2, a0, a1, a2;
  if (kind == "lowpass") {
    b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else if (kind == "highpass") {
    b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else if (kind == "bandpass") {
    b0 = al, b1 = 0, b2 = -al, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else if (kind == "notch") {
    b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else if (kind == "allpass") {
    b0 = 1 - al, b1 = -2 * cw, b2 = 1 + al, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else if (kind == "peak") {
    b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
  } else if (kind == "lowshelf" || kind == "highshelf") {
    const double sq = 2 * std::sqrt(A) * al, s = kind == "lowshelf" ? 1.0 : -1.0;
    b0 = A * ((A + 1) - s * (A - 1) * cw + sq);
    b1 = s * 2 * A * ((A - 1) - s * (A + 1) * cw);
    b2 = A * ((A + 1) - s * (A - 1) * cw - sq);
    a0 = (A + 1) + s * (A - 1) * cw + sq;
    a1 = -s * 2 * ((A - 1) + s * (A + 1) * cw);
    a2 = (A + 1) + s * (A - 1) * cw - sq;
  } else {
    return luaL_argerror(L, 2, "lowpass, highpass, bandpass, notch, allpass, peak, lowshelf or highshelf");
  }
  b0 /= a0, b1 /= a0, b2 /= a0, a1 /= a0, a2 /= a0;
  charge(L, b.n, 4);
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (int64_t i = 0; i < b.n; ++i) {
    double x = b.d[i];
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    if (!std::isfinite(y)) y = 0;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    b.d[i] = clean(y);
  }
  return 0;
}
// b:delay(samples, feedback, mix): feedback echo in place.
int b_delay(lua_State* L) {
  Buf b = checkBuf(L, 1);
  lua_Integer d = luaL_checkinteger(L, 2);
  if (d < 1 || d > b.n) luaL_argerror(L, 2, "delay 1..length samples");
  double fb = std::clamp(num(L, 3), -0.99, 0.99), mix = std::clamp(optNum(L, 4, 0.5), 0.0, 1.0);
  charge(L, b.n, 8);
  std::vector<float> ring(static_cast<size_t>(d), 0.f);
  size_t p = 0;
  for (int64_t i = 0; i < b.n; ++i) {
    const double x = b.d[i];
    const double w = x + fb * ring[p];
    ring[p] = clean(w);
    p = p + 1 == ring.size() ? 0 : p + 1;
    b.d[i] = clean(x * (1.0 - mix) + w * mix);
  }
  return 0;
}
// b:map(fn [, i, j]): b[k] = fn(b[k], k) — one Lua call per sample (slow; budgeted).
int b_map(lua_State* L) {
  Buf b = checkBuf(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  Span s = span(L, b, 3);
  for (int64_t i = s.a; i < s.b; ++i) {
    lua_pushvalue(L, 2);
    lua_pushnumber(L, b.d[i]);
    lua_pushinteger(L, i + 1);
    lua_call(L, 2, 1);
    if (!lua_isnumber(L, -1)) luaL_error(L, "map: the function must return a number (sample %I)", static_cast<lua_Integer>(i + 1));
    b.d[i] = clean(lua_tonumber(L, -1));
    lua_pop(L, 1);
    charge(L, 4, 1);
  }
  return 0;
}

int b_index(lua_State* L) {
  Buf b = checkBuf(L, 1);
  if (lua_type(L, 2) == LUA_TNUMBER) {
    lua_Integer i = 0;
    if (!lua_isinteger(L, 2) || (i = lua_tointeger(L, 2)) < 1 || i > b.n) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, b.d[i - 1]);
    return 1;
  }
  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));  // methods
  return 1;
}
int b_newindex(lua_State* L) {
  Buf b = checkBuf(L, 1);
  if (lua_type(L, 2) != LUA_TNUMBER || !lua_isinteger(L, 2)) return luaL_error(L, "buffers only take sample indices");
  int64_t i = indexArg(L, 2, b);
  b.d[i] = clean(num(L, 3));
  return 0;
}
int b_tostring(lua_State* L) {
  lua_pushfstring(L, "buffer(%I)", static_cast<lua_Integer>(checkBuf(L, 1).n));
  return 1;
}

// ------------------------------------------------------------- modules ----
int g_new(lua_State* L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  double v = optNum(L, 2, 0.0);
  Buf b = newBuf(L, n);
  if (v != 0) {
    charge(L, n);
    std::fill(b.d, b.d + b.n, clean(v));
  }
  return 1;
}
int g_fromTable(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
  if (n > (1 << 22)) luaL_argerror(L, 1, "too many values");
  Buf b = newBuf(L, n);
  charge(L, n, 1);
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 1, i);
    if (!lua_isnumber(L, -1)) luaL_error(L, "value %d is not a number", static_cast<int>(i));
    b.d[i - 1] = clean(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return 1;
}
int g_isBuffer(lua_State* L) {
  lua_pushboolean(L, isBuf(L, 1));
  return 1;
}
int d_dbToGain(lua_State* L) {
  lua_pushnumber(L, std::pow(10.0, std::clamp(num(L, 1), -400.0, 400.0) / 20.0));
  return 1;
}
int d_gainToDb(lua_State* L) {
  double g = std::fabs(num(L, 1));
  lua_pushnumber(L, g > 0 ? 20.0 * std::log10(g) : -std::numeric_limits<double>::infinity());
  return 1;
}
int d_noteToHz(lua_State* L) {
  lua_pushnumber(L, 440.0 * std::pow(2.0, (std::clamp(num(L, 1), -200.0, 400.0) - 69.0) / 12.0));
  return 1;
}
int d_hzToNote(lua_State* L) {
  double hz = num(L, 1);
  if (hz <= 0) luaL_argerror(L, 1, "frequency must be positive");
  lua_pushnumber(L, 69.0 + 12.0 * std::log2(hz / 440.0));
  return 1;
}

// audio:read(c, from, to): copy of the original file's frames [from, to) (0-based).
int a_read(lua_State* L) {
  const Audio* a = static_cast<const Audio*>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_Integer c = luaL_checkinteger(L, 1);
  if (c < 1 || c > a->channels) luaL_argerror(L, 1, "no such channel");
  lua_Integer from = luaL_checkinteger(L, 2), to = luaL_checkinteger(L, 3);
  from = std::clamp<lua_Integer>(from, 0, a->frames());
  to = std::clamp<lua_Integer>(to, from, a->frames());
  charge(L, to - from);
  pushCopy(L, a->ch(static_cast<int>(c - 1)) + from, to - from);
  return 1;
}

const char* kAudioPrelude = R"LUA(
do
  local read = _gl_read
  _gl_read = nil
  local isBuffer, error, type, tostring = buffer.isBuffer, error, type, tostring
  local A = {}
  A.__index = A
  A.__metatable = 'locked'
  function A:channel(c)
    local b = self._ch[c]
    if b == nil then error('no channel ' .. tostring(c), 2) end
    return b
  end
  function A:setChannel(c, b)
    if self._ch[c] == nil then error('no channel ' .. tostring(c), 2) end
    if not isBuffer(b) then error('setChannel needs a buffer', 2) end
    self._ch[c] = b
  end
  function A:buffers()
    local c = 0
    return function()
      c = c + 1
      if c <= self.channels then return c, self._ch[c] end
    end
  end
  function A:resize(n)
    if type(n) ~= 'number' or n < 0 or n ~= math.floor(n) then error('resize needs a frame count', 2) end
    for c = 1, self.channels do self._ch[c] = self._ch[c]:resized(n) end
    self.frames = n
  end
  function A:read(c, from, to) return read(c, from, to) end
  function A:addMarker(pos, name)
    if type(pos) ~= 'number' then error('addMarker needs a position', 2) end
    if #self._markers >= 10000 then error('too many markers', 2) end
    self._markers[#self._markers + 1] = { pos = pos, name = tostring(name or '') }
  end
  function A:select(a, b)
    if type(a) ~= 'number' or type(b) ~= 'number' then error('select needs two frame positions', 2) end
    self._sel = { a, b }
  end
  function _gl_audioMeta() return A end
end
)LUA";

void registerBuffer(lua_State* L) {
  static const luaL_Reg methods[] = {{"len", b_len},
                                     {"get", b_get},
                                     {"set", b_set},
                                     {"copy", b_copy},
                                     {"sub", b_copy},
                                     {"resized", b_resized},
                                     {"totable", b_totable},
                                     {"fill", b_fill},
                                     {"scale", b_scale},
                                     {"gainDb", b_gainDb},
                                     {"offset", b_offset},
                                     {"add", b_add},
                                     {"mul", b_mul},
                                     {"ramp", b_ramp},
                                     {"reverse", b_reverse},
                                     {"invert", b_invert},
                                     {"abs", b_abs},
                                     {"clip", b_clip},
                                     {"softclip", b_softclip},
                                     {"quantize", b_quantize},
                                     {"removeDc", b_removeDc},
                                     {"peak", b_peak},
                                     {"rms", b_rms},
                                     {"sum", b_sum},
                                     {"minmax", b_minmax},
                                     {"zeroCrossings", b_zeroCrossings},
                                     {"nearestZero", b_nearestZero},
                                     {"envelope", b_envelope},
                                     {"tone", b_tone},
                                     {"noise", b_noise},
                                     {"lowpass", b_lowpass},
                                     {"highpass", b_highpass},
                                     {"biquad", b_biquad},
                                     {"delay", b_delay},
                                     {"map", b_map},
                                     {nullptr, nullptr}};
  luaL_newmetatable(L, kBufMeta);
  lua_newtable(L);
  luaL_setfuncs(L, methods, 0);
  lua_pushcclosure(L, b_index, 1);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, b_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, b_len);
  lua_setfield(L, -2, "__len");
  lua_pushcfunction(L, b_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushstring(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);
  static const luaL_Reg g[] = {{"new", g_new}, {"fromTable", g_fromTable}, {"isBuffer", g_isBuffer}, {nullptr, nullptr}};
  lua_newtable(L);
  luaL_setfuncs(L, g, 0);
  lua_setglobal(L, "buffer");
  static const luaL_Reg d[] = {{"dbToGain", d_dbToGain}, {"gainToDb", d_gainToDb}, {"noteToHz", d_noteToHz}, {"hzToNote", d_hzToNote}, {nullptr, nullptr}};
  lua_newtable(L);
  luaL_setfuncs(L, d, 0);
  lua_setglobal(L, "dsp");
}

// Reads the result channels from the audio table (stack index t). On success
// the _ch table stays on the stack (it keeps the buffers alive); the caller
// copies the samples out before any other Lua call and pops it.
bool readChannels(lua_State* L, int t, int channels, std::vector<Buf>& out, std::string* err) {
  lua_getfield(L, t, "_ch");
  if (!lua_istable(L, -1)) {
    *err = "audio._ch was replaced";
    lua_pop(L, 1);
    return false;
  }
  for (int c = 1; c <= channels; ++c) {
    lua_rawgeti(L, -1, c);
    if (!isBuf(L, -1)) {
      *err = "channel " + std::to_string(c) + " is not a buffer";
      lua_pop(L, 2);
      return false;
    }
    out.push_back(bufAt(lua_touserdata(L, -1)));
    lua_pop(L, 1);  // the _ch table (left on the stack) keeps the buffer alive
  }
  for (const Buf& b : out)
    if (b.n != out[0].n) {
      *err = "the channel buffers differ in length (use audio:resize)";
      return false;
    }
  return true;
}

// A VM with the form helpers, buffers and the global `audio` for `ctx`
// (the audio table is also kept in the registry at *audioRef).
bool prepareVm(Vm& vm, const AudioScriptContext& ctx, Range tr, int* audioRef, std::string* err) {
  const Audio& src = ctx.content.a();
  const int channels = src.channels == 1 ? 1 : 2;
  lua_State* L = vm.L();
  if (!vm.ok() || !vm.exec(script::formPrelude(), "form", err)) return false;
  registerBuffer(L);
  lua_pushlightuserdata(L, const_cast<Audio*>(&src));
  lua_pushcclosure(L, a_read, 1);
  lua_setglobal(L, "_gl_read");
  if (!vm.exec(kAudioPrelude, "audio", err)) return false;
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, static_cast<lua_Integer>(ctx.seed));
  lua_call(L, 1, 0);
  lua_pop(L, 1);
  // The audio object (kept in the registry: the script may replace the global).
  if (!vm.protect(
          [&](lua_State* S) {
            lua_createtable(S, 0, 12);
            int t = lua_gettop(S);
            auto setI = [&](const char* k, lua_Integer v) {
              lua_pushinteger(S, v);
              lua_setfield(S, t, k);
            };
            lua_pushnumber(S, src.rate);
            lua_setfield(S, t, "rate");
            setI("channels", channels);
            setI("frames", tr.len());
            setI("start", tr.a);
            setI("fileFrames", src.frames());
            setI("cursor", ctx.cursor);
            setI("seed", ctx.seed);
            lua_pushboolean(S, !clampRange(ctx.content, ctx.sel).empty());
            lua_setfield(S, t, "selected");
            lua_createtable(S, channels, 0);
            for (int c = 0; c < channels; ++c) {
              pushCopy(S, tr.len() > 0 ? src.ch(c) + tr.a : nullptr, tr.len());
              lua_rawseti(S, -2, c + 1);
            }
            lua_setfield(S, t, "_ch");
            lua_newtable(S);
            lua_setfield(S, t, "_markers");
            lua_getglobal(S, "_gl_audioMeta");
            lua_call(S, 0, 1);
            lua_setmetatable(S, t);
            lua_pushnil(S);
            lua_setglobal(S, "_gl_audioMeta");
            lua_pushvalue(S, t);
            lua_setglobal(S, "audio");
            *audioRef = luaL_ref(S, LUA_REGISTRYINDEX);
          },
          err))
    return false;
  return true;
}

}  // namespace

void openAudioApi(Vm& vm) {
  if (vm.L()) registerBuffer(vm.L());
}

bool AudioScript::loadFile(const std::string& path, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << is.rdbuf();
  if (!loadSource(ss.str(), fs::path(path).stem().string(), err)) return false;
  path_ = path;
  return true;
}

bool AudioScript::loadSource(const std::string& source, const std::string& name, std::string* err) {
  source_.clear();
  path_.clear();
  header_ = script::parseHeader(source);
  name_ = header_.get("name", name);
  atCursor_ = header_.get("target") == "cursor";
  form_ = script::Form{};
  script::Limits lim;
  lim.instructions = 2'000'000;
  lim.seconds = 1.0;
  // Built against an empty editor, so top-level code may look at `audio`;
  // the work belongs in apply().
  Vm vm(lim);
  AudioScriptContext none;
  int ref = LUA_NOREF;
  if (!prepareVm(vm, none, {}, &ref, err)) return false;
  if (!vm.exec(source, name_, err)) return false;
  if (!script::buildForm(vm, form_, err)) return false;
  if (form_.description.empty()) form_.description = header_.get("description");
  source_ = source;
  return true;
}

Range AudioScript::targetRange(const AudioScriptContext& ctx) const {
  Range s = clampRange(ctx.content, ctx.sel);
  if (!s.empty()) return s;
  if (atCursor_ || ctx.content.frames() == 0) {
    const int64_t c = std::clamp<int64_t>(ctx.cursor, 0, ctx.content.frames());
    return {c, c};
  }
  return {0, ctx.content.frames()};
}

AudioScriptResult AudioScript::run(const AudioScriptContext& ctx, const std::vector<script::FormParam>& values,
                                   const script::Limits& limitsIn) const {
  AudioScriptResult r;
  r.content = ctx.content;
  if (source_.empty()) {
    r.error = "no script loaded";
    return r;
  }
  const Audio& src = ctx.content.a();
  const Range tr = targetRange(ctx);
  r.target = tr;
  const int channels = src.channels == 1 ? 1 : 2;
  // Room for the selection's buffers and a few working copies.
  script::Limits limits = limitsIn;
  const uint64_t bytes = static_cast<uint64_t>(tr.len()) * static_cast<uint64_t>(channels) * sizeof(float);
  limits.memoryBytes = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(limits.memoryBytes) + bytes * 6, uint64_t{3} << 30));
  Vm vm(limits);
  int audioRef = LUA_NOREF;
  if (!prepareVm(vm, ctx, tr, &audioRef, &r.error)) return r;
  bool ok = vm.exec(source_, name_, &r.error);
  if (ok && vm.hasFunction("apply"))
    ok = vm.protect(
        [&](lua_State* S) {
          lua_getglobal(S, "apply");
          lua_rawgeti(S, LUA_REGISTRYINDEX, audioRef);
          script::pushFormValues(S, form_, values);
          lua_call(S, 2, 0);
        },
        &r.error);
  if (!ok) {
    r.log = vm.log();
    r.limitHit = !vm.abortReason().empty();
    return r;
  }
  // Read back (protected: the tables may carry metamethods).
  Audio out;
  std::vector<std::pair<int64_t, std::string>> markers;
  std::pair<double, double> sel{0, 0};
  bool hasSel = false;
  std::string why;
  ok = vm.protect(
      [&](lua_State* S) {
        lua_rawgeti(S, LUA_REGISTRYINDEX, audioRef);
        int t = lua_gettop(S);
        std::vector<Buf> bufs;
        if (!readChannels(S, t, channels, bufs, &why)) return;
        const int64_t n = bufs[0].n;
        out = makeAudio(src.rate, channels, n);
        for (int c = 0; c < channels; ++c)
          if (n > 0) std::memcpy(out.ch(c), bufs[static_cast<size_t>(c)].d, static_cast<size_t>(n) * sizeof(float));
        fixMono(out);
        lua_pop(S, 1);  // _ch
        lua_getfield(S, t, "_markers");
        if (lua_istable(S, -1)) {
          const lua_Integer m = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(S, -1)), 10000);
          for (lua_Integer i = 1; i <= m; ++i) {
            lua_rawgeti(S, -1, i);
            if (lua_istable(S, -1)) {
              double p = script::fieldNumber(S, lua_gettop(S), "pos", 0);
              std::string nm = script::fieldString(S, lua_gettop(S), "name", "");
              if (nm.size() > 256) nm.resize(256);
              if (std::isfinite(p)) markers.emplace_back(std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::clamp(p, -1e15, 1e15))), 0, n), nm);
            }
            lua_pop(S, 1);
          }
        }
        lua_pop(S, 1);
        lua_getfield(S, t, "_sel");
        if (lua_istable(S, -1)) {
          lua_rawgeti(S, -1, 1);
          lua_rawgeti(S, -2, 2);
          if (lua_isnumber(S, -2) && lua_isnumber(S, -1)) sel = {lua_tonumber(S, -2), lua_tonumber(S, -1)}, hasSel = true;
          lua_pop(S, 2);
        }
        lua_pop(S, 1);
      },
      &r.error);
  r.log = vm.log();
  if (ok && !why.empty()) r.error = why, ok = false;
  if (!ok) {
    r.limitHit = !vm.abortReason().empty();
    return r;
  }
  const int64_t n = out.frames();
  bool same = n == tr.len();
  for (int c = 0; same && c < channels; ++c)
    same = n == 0 || std::memcmp(out.ch(c), src.ch(c) + tr.a, static_cast<size_t>(n) * sizeof(float)) == 0;
  Content next = ctx.content;
  if (!same) {
    if (n == tr.len()) {
      Audio full = src;
      for (int c = 0; c < 2; ++c) std::memcpy(full.ch(c) + tr.a, out.ch(c), static_cast<size_t>(n) * sizeof(float));
      next = withAudio(next, std::move(full));
    } else {
      next = replaceRange(next, tr, out);
    }
  }
  for (const auto& [p, nm] : markers) next = addMarker(next, tr.a + p, nm);
  r.changed = !same || !markers.empty();
  r.content = r.changed ? next : ctx.content;
  r.out = {tr.a, tr.a + n};
  if (hasSel && std::isfinite(sel.first) && std::isfinite(sel.second)) {
    // Relative to the target start; may reach outside it (not outside the file).
    const int64_t total = r.content.frames();
    auto at = [&](double v) { return std::clamp<int64_t>(tr.a + static_cast<int64_t>(std::llround(std::clamp(v, -1e15, 1e15))), 0, total); };
    r.sel = {at(std::min(sel.first, sel.second)), at(std::max(sel.first, sel.second))};
  } else {
    r.sel = clampRange(ctx.content, ctx.sel).empty() && !(tr.empty() && n > 0) ? Range{} : r.out;
  }
  r.preview = std::move(out);
  r.ok = true;
  return r;
}

}  // namespace gl::packs::edison
