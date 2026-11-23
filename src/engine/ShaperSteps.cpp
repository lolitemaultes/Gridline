// Gridline — the Shaper's step bank text (see ShaperSteps.h).
#include "ShaperSteps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gl::shsteps {

void clear(Bank& b) {
  for (int p = 0; p < kPatterns; ++p)
    for (int l = 0; l < kLanes; ++l) {
      for (int k = 0; k < kSteps; ++k) b.val[p][l][k] = laneDefault(l), b.chance[p][l][k] = 100, b.hits[p][l][k] = 1;
      b.anyChance[p][l] = b.anyHits[p][l] = false;
    }
  for (auto& c : b.chain) c = -1;
  b.chainLen = 0;
}

void finish(Bank& b) {
  for (int p = 0; p < kPatterns; ++p)
    for (int l = 0; l < kLanes; ++l) {
      bool c = false, h = false;
      for (int k = 0; k < kSteps; ++k) c = c || b.chance[p][l][k] < 100, h = h || b.hits[p][l][k] > 1;
      b.anyChance[p][l] = c, b.anyHits[p][l] = h;
    }
  b.chainLen = 0;
  for (int k = 0; k < kChain; ++k)
    if (b.chain[k] >= 0) b.chainLen = k + 1;
}

namespace {

// Allocation-free reading: one signed integer, skipping spaces and tabs (not line ends).
bool readInt(const char*& s, long& out) {
  while (*s == ' ' || *s == '\t') ++s;
  bool neg = false;
  if (*s == '-' || *s == '+') neg = *s++ == '-';
  if (*s < '0' || *s > '9') return false;
  long v = 0;
  while (*s >= '0' && *s <= '9') {
    if (v < 100000000) v = v * 10 + (*s - '0');
    ++s;
  }
  out = neg ? -v : v;
  return true;
}

}  // namespace

void parse(const char* text, Bank& b) {
  clear(b);
  if (!text) return;
  const char* s = text;
  while (*s) {
    const char kind = *s;
    const char* line = s + 1;
    long p = 0, l = 0, at = 0, v = 0;
    if ((kind == 'v' || kind == 'q' || kind == 'r') && readInt(line, p) && readInt(line, l) && readInt(line, at) && p >= 0 && p < kPatterns && l >= 0 &&
        l < kLanes) {
      for (long k = at; readInt(line, v); ++k) {
        if (k < 0 || k >= kSteps) continue;
        if (kind == 'v') b.val[p][l][k] = std::clamp(static_cast<float>(v) / 10000.f, l == 2 ? -1.f : 0.f, 1.f);
        else if (kind == 'q') b.chance[p][l][k] = static_cast<uint8_t>(std::clamp(v, 0L, 100L));
        else b.hits[p][l][k] = static_cast<uint8_t>(std::clamp(v, 1L, 4L));
      }
    } else if (kind == 'c') {
      for (int k = 0; readInt(line, v); ++k)
        if (k < kChain) b.chain[k] = static_cast<int8_t>(v >= 1 && v <= kPatterns ? v - 1 : -1);
    }
    while (*s && *s != '\n') ++s;
    if (*s == '\n') ++s;
  }
  finish(b);
}

std::string write(const Bank& b) {
  std::string out;
  char buf[48];
  auto row = [&](char kind, int p, int l, int from, auto get, auto isDefault) {
    int last = -1;
    for (int k = from; k < kSteps; ++k)
      if (!isDefault(k)) last = k;
    if (last < 0) return;
    int first = from;
    while (first < last && isDefault(first)) ++first;
    std::snprintf(buf, sizeof buf, "%c %d %d %d", kind, p, l, first);
    out += buf;
    for (int k = first; k <= last; ++k) std::snprintf(buf, sizeof buf, " %ld", get(k)), out += buf;
    out += '\n';
  };
  for (int p = 0; p < kPatterns; ++p)
    for (int l = 0; l < kLanes; ++l) {
      const float d = laneDefault(l);
      row('v', p, l, p == 0 ? pp::kShaperPoints : 0, [&](int k) { return std::lround(b.val[p][l][k] * 10000.0); },
          [&](int k) { return std::lround(b.val[p][l][k] * 10000.0) == std::lround(d * 10000.0); });
      row('q', p, l, 0, [&](int k) { return static_cast<long>(b.chance[p][l][k]); }, [&](int k) { return b.chance[p][l][k] >= 100; });
      row('r', p, l, 0, [&](int k) { return static_cast<long>(b.hits[p][l][k]); }, [&](int k) { return b.hits[p][l][k] <= 1; });
    }
  int len = 0;
  for (int k = 0; k < kChain; ++k)
    if (b.chain[k] >= 0) len = k + 1;
  if (len > 0) {
    out += 'c';
    for (int k = 0; k < len; ++k) std::snprintf(buf, sizeof buf, " %d", b.chain[k] + 1), out += buf;
    out += '\n';
  }
  return out.empty() ? out : "SH1\n" + out;
}

}  // namespace gl::shsteps
