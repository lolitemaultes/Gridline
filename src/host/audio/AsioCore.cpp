// Gridline: platform-independent parts of the ASIO backend (K-6). See AsioCore.h.
#include "AsioCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gl::asio {

namespace {

// Layout of an integer format: container bytes, significant bits, byte order.
struct IntFormat {
  int bytes = 0, bits = 0;
  bool bigEndian = false;
};

bool intFormat(abi::SampleType t, IntFormat& f) {
  switch (t) {
    case abi::kInt16LSB: f = {2, 16, false}; return true;
    case abi::kInt24LSB: f = {3, 24, false}; return true;
    case abi::kInt32LSB: f = {4, 32, false}; return true;
    case abi::kInt32LSB16: f = {4, 16, false}; return true;
    case abi::kInt32LSB18: f = {4, 18, false}; return true;
    case abi::kInt32LSB20: f = {4, 20, false}; return true;
    case abi::kInt32LSB24: f = {4, 24, false}; return true;
    case abi::kInt16MSB: f = {2, 16, true}; return true;
    case abi::kInt24MSB: f = {3, 24, true}; return true;
    case abi::kInt32MSB: f = {4, 32, true}; return true;
    case abi::kInt32MSB16: f = {4, 16, true}; return true;
    case abi::kInt32MSB18: f = {4, 18, true}; return true;
    case abi::kInt32MSB20: f = {4, 20, true}; return true;
    case abi::kInt32MSB24: f = {4, 24, true}; return true;
    default: return false;
  }
}

// Reads a signed integer of `bytes` bytes (sign-extended from its container).
inline int32_t readInt(const unsigned char* p, int bytes, bool be) {
  uint32_t u = 0;
  for (int i = 0; i < bytes; ++i) {
    const unsigned b = p[be ? i : bytes - 1 - i];
    u = (u << 8) | b;
  }
  const int shift = 32 - 8 * bytes;  // sign-extend the container
  return static_cast<int32_t>(u << shift) >> shift;
}

inline void writeInt(unsigned char* p, int bytes, bool be, int32_t v) {
  auto u = static_cast<uint32_t>(v);
  for (int i = 0; i < bytes; ++i) {
    p[be ? bytes - 1 - i : i] = static_cast<unsigned char>(u & 0xffu);
    u >>= 8;
  }
}

constexpr bool hostLittleEndian() {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return false;
#else
  return true;
#endif
}

template <typename T>
inline T loadSwapped(const unsigned char* p, bool swap) {
  unsigned char b[sizeof(T)];
  for (size_t i = 0; i < sizeof(T); ++i) b[i] = p[swap ? sizeof(T) - 1 - i : i];
  T v;
  std::memcpy(&v, b, sizeof(T));
  return v;
}

template <typename T>
inline void storeSwapped(unsigned char* p, bool swap, T v) {
  unsigned char b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  for (size_t i = 0; i < sizeof(T); ++i) p[swap ? sizeof(T) - 1 - i : i] = b[i];
}

}  // namespace

bool formatSupported(abi::SampleType t) {
  IntFormat f;
  return intFormat(t, f) || t == abi::kFloat32LSB || t == abi::kFloat64LSB || t == abi::kFloat32MSB || t == abi::kFloat64MSB;
}

int formatBytes(abi::SampleType t) {
  IntFormat f;
  if (intFormat(t, f)) return f.bytes;
  if (t == abi::kFloat32LSB || t == abi::kFloat32MSB) return 4;
  if (t == abi::kFloat64LSB || t == abi::kFloat64MSB) return 8;
  return 0;
}

const char* formatName(abi::SampleType t) {
  switch (t) {
    case abi::kInt16LSB: return "16-bit integer";
    case abi::kInt24LSB: return "24-bit integer";
    case abi::kInt32LSB: return "32-bit integer";
    case abi::kInt32LSB16: return "16-bit in 32";
    case abi::kInt32LSB18: return "18-bit in 32";
    case abi::kInt32LSB20: return "20-bit in 32";
    case abi::kInt32LSB24: return "24-bit in 32";
    case abi::kFloat32LSB: return "32-bit float";
    case abi::kFloat64LSB: return "64-bit float";
    case abi::kInt16MSB: return "16-bit integer (big endian)";
    case abi::kInt24MSB: return "24-bit integer (big endian)";
    case abi::kInt32MSB: return "32-bit integer (big endian)";
    case abi::kFloat32MSB: return "32-bit float (big endian)";
    case abi::kFloat64MSB: return "64-bit float (big endian)";
    default: return "unsupported";
  }
}

void toFloat(const void* src, abi::SampleType t, float* dst, int n) {
  if (n <= 0) return;
  const auto* p = static_cast<const unsigned char*>(src);
  IntFormat f;
  if (intFormat(t, f)) {
    const double scale = 1.0 / std::ldexp(1.0, f.bits - 1);
    for (int i = 0; i < n; ++i) dst[i] = static_cast<float>(readInt(p + static_cast<size_t>(i) * static_cast<size_t>(f.bytes), f.bytes, f.bigEndian) * scale);
    return;
  }
  if (t == abi::kFloat32LSB || t == abi::kFloat32MSB) {
    const bool swap = (t == abi::kFloat32MSB) == hostLittleEndian();
    if (!swap) {
      std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    for (int i = 0; i < n; ++i) dst[i] = loadSwapped<float>(p + 4 * static_cast<size_t>(i), true);
    return;
  }
  if (t == abi::kFloat64LSB || t == abi::kFloat64MSB) {
    const bool swap = (t == abi::kFloat64MSB) == hostLittleEndian();
    for (int i = 0; i < n; ++i) dst[i] = static_cast<float>(loadSwapped<double>(p + 8 * static_cast<size_t>(i), swap));
    return;
  }
  std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
}

void fromFloat(const float* src, abi::SampleType t, void* dst, int n) {
  if (n <= 0) return;
  auto* p = static_cast<unsigned char*>(dst);
  IntFormat f;
  if (intFormat(t, f)) {
    const double full = std::ldexp(1.0, f.bits - 1);
    const double hi = full - 1.0;
    for (int i = 0; i < n; ++i) {
      double x = static_cast<double>(src[i]) * full;
      if (!(x == x)) x = 0;  // NaN
      x = std::clamp(x, -full, hi);
      writeInt(p + static_cast<size_t>(i) * static_cast<size_t>(f.bytes), f.bytes, f.bigEndian, static_cast<int32_t>(std::lrint(x)));
    }
    return;
  }
  if (t == abi::kFloat32LSB || t == abi::kFloat32MSB) {
    const bool swap = (t == abi::kFloat32MSB) == hostLittleEndian();
    if (!swap) {
      std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    for (int i = 0; i < n; ++i) storeSwapped<float>(p + 4 * static_cast<size_t>(i), true, src[i]);
    return;
  }
  if (t == abi::kFloat64LSB || t == abi::kFloat64MSB) {
    const bool swap = (t == abi::kFloat64MSB) == hostLittleEndian();
    for (int i = 0; i < n; ++i) storeSwapped<double>(p + 8 * static_cast<size_t>(i), swap, static_cast<double>(src[i]));
    return;
  }
  if (const int b = formatBytes(t); b > 0) std::memset(dst, 0, static_cast<size_t>(b) * static_cast<size_t>(n));
}

// ---- buffer sizes ----

bool bufferSizeValid(const BufferCaps& c, int frames) {
  if (frames <= 0) return false;
  if (frames == c.preferred) return true;
  if (frames < c.minSize || frames > c.maxSize) return false;
  if (c.granularity == 0) return frames == c.minSize || frames == c.maxSize;
  if (c.granularity < 0) return (frames & (frames - 1)) == 0;
  return (frames - c.minSize) % c.granularity == 0 || frames == c.maxSize;
}

std::vector<int> bufferSizeChoices(const BufferCaps& c, int maxCount) {
  std::vector<int> v;
  const int lo = std::max(1, c.minSize), hi = std::max(lo, c.maxSize);
  if (c.granularity < 0) {
    for (long long s = 1; s <= hi; s *= 2)
      if (s >= lo) v.push_back(static_cast<int>(s));
  } else if (c.granularity > 0) {
    for (long long s = lo; s <= hi; s += c.granularity) v.push_back(static_cast<int>(s));
    v.push_back(hi);
  } else {
    v.push_back(lo);
    v.push_back(hi);
  }
  if (c.preferred > 0) v.push_back(c.preferred);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  if (maxCount > 0 && static_cast<int>(v.size()) > maxCount) {
    // A fine step (granularity 1, 8, ...): keep the round sizes (powers of two, then multiples
    // of 32), the extremes and the preferred size.
    std::vector<int> keep;
    for (int s : v)
      if (s == lo || s == hi || s == c.preferred || (s & (s - 1)) == 0) keep.push_back(s);
    for (int step : {32, 64, 128, 256}) {
      if (static_cast<int>(keep.size()) >= maxCount) break;
      std::vector<int> more = keep;
      for (int s : v)
        if (s % step == 0 && std::find(more.begin(), more.end(), s) == more.end()) more.push_back(s);
      std::sort(more.begin(), more.end());
      if (static_cast<int>(more.size()) <= maxCount) {
        keep = more;
        break;
      }
    }
    v = keep;
    while (static_cast<int>(v.size()) > maxCount) {  // still too many: drop from the large end
      auto it = std::find_if(v.rbegin(), v.rend(), [&](int s) { return s != c.preferred && s != lo && s != hi; });
      if (it == v.rend()) break;
      v.erase(std::next(it).base());
    }
  }
  return v;
}

int chooseBufferSize(const BufferCaps& c, int wanted) {
  const int pref = c.preferred > 0 ? c.preferred : std::max(1, c.minSize);
  if (wanted <= 0 || wanted == pref) return pref;
  if (bufferSizeValid(c, wanted)) return wanted;
  // Candidates near `wanted`: the preferred size, the extremes and the nearest steps.
  const long long lo = std::max(1, c.minSize), hi = std::max<long long>(lo, c.maxSize);
  std::vector<long long> cand = {pref, lo, hi};
  if (c.granularity < 0) {
    for (long long s = 1; s <= hi; s *= 2)
      if (s >= lo) cand.push_back(s);
  } else if (c.granularity > 0) {
    const long long g = c.granularity;
    const long long k = (std::clamp<long long>(wanted, lo, hi) - lo) / g;
    for (long long s : {lo + k * g, lo + (k + 1) * g})
      if (s <= hi) cand.push_back(s);
  }
  long long best = pref, bestD = -1;
  for (long long s : cand) {
    if (!bufferSizeValid(c, static_cast<int>(s))) continue;
    const long long d = s > wanted ? s - wanted : wanted - s;
    if (bestD < 0 || d < bestD || (d == bestD && s > best)) best = s, bestD = d;  // ties: the larger (safer) size
  }
  return static_cast<int>(best);
}

// ---- sample rates ----

const std::vector<int>& candidateRates() {
  static const std::vector<int> r = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
  return r;
}

// ---- channels ----

std::vector<ExtraOut> parseExtraOuts(const std::string& s) {
  std::vector<ExtraOut> v;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    const std::string item = s.substr(i, j - i);
    int m = -1, o = -1;
    if (std::sscanf(item.c_str(), "%d:%d", &m, &o) == 2 && m >= 0 && o >= 0) v.push_back({m, o});
    i = j + 1;
  }
  return v;
}

std::string formatExtraOuts(const std::vector<ExtraOut>& v) {
  std::string s;
  for (const auto& e : v) {
    if (!s.empty()) s += ',';
    s += std::to_string(e.mixer) + ":" + std::to_string(e.firstOut);
  }
  return s;
}

int ChannelPlan::extraOut(int k) const {
  if (k < 0 || k >= static_cast<int>(extras.size())) return -1;
  const int mainCount = mainL < 0 ? 0 : mainR == mainL ? 1 : 2;
  return mainCount + 2 * k;
}

ChannelPlan planChannels(int numIn, int numOut, const ChannelRequest& r) {
  ChannelPlan p;
  numIn = std::max(0, numIn), numOut = std::max(0, numOut);
  if (numIn > 0 && r.inCount > 0) {
    const int first = std::clamp(r.inFirst, 0, numIn - 1);
    const int count = std::min({r.inCount, numIn - first, kMaxInputs});
    for (int k = 0; k < count; ++k) p.inputs.push_back(first + k);
  }
  if (numOut == 1) {
    p.mainL = p.mainR = 0;
    p.outputs.push_back(0);
  } else if (numOut >= 2) {
    const int first = std::clamp(r.outMain, 0, numOut - 2);
    p.mainL = first, p.mainR = first + 1;
    p.outputs.push_back(p.mainL);
    p.outputs.push_back(p.mainR);
    for (const auto& e : r.extras) {
      if (static_cast<int>(p.extras.size()) >= kMaxExtraOutputs) break;
      if (e.mixer < 0 || e.firstOut < 0 || e.firstOut + 1 >= numOut) continue;
      if (std::find(p.outputs.begin(), p.outputs.end(), e.firstOut) != p.outputs.end() ||
          std::find(p.outputs.begin(), p.outputs.end(), e.firstOut + 1) != p.outputs.end())
        continue;
      p.extras.push_back(e);
      p.outputs.push_back(e.firstOut);
      p.outputs.push_back(e.firstOut + 1);
    }
  }
  return p;
}

std::string pairLabel(const std::vector<std::string>& names, int first, bool input) {
  auto one = [&](int k) {
    if (k >= 0 && k < static_cast<int>(names.size()) && !names[static_cast<size_t>(k)].empty()) return names[static_cast<size_t>(k)];
    return std::string(input ? "In " : "Out ") + std::to_string(k + 1);
  };
  if (first + 1 >= static_cast<int>(names.size()) && !names.empty()) return one(first);
  return one(first) + " - " + one(first + 1);
}

}  // namespace gl::asio
