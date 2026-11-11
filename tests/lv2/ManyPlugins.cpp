// Gridline — a test LV2 binary holding many plugins (built in-tree for the big bundle scan test;
// never installed). Written against the LV2 specification headers only.
//   urn:gridline:lv2many:<n>, n = 0 .. kCount - 1: a stereo gain (ports 0, 1 audio in; 2, 3 audio
//   out; 4 gain; 5 latency, a control output). Every instance holds a large buffer of its own
//   (kInstanceMegabytes, filled so it is resident memory), like a real plugin's analysers and
//   delay lines. Plugin 7 refuses to start. The bundle (manifest and one .ttl a plugin) is
//   written by the test, which may describe more control ports than these: they are ignored.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include "lv2/core/lv2.h"

namespace {

constexpr uint32_t kCount = 256;
constexpr size_t kInstanceMegabytes = 24;
constexpr uint32_t kRefuses = 7;

struct Many {
  const float* in[2] = {nullptr, nullptr};
  float* out[2] = {nullptr, nullptr};
  const float* gain = nullptr;
  float* latency = nullptr;
  uint64_t* buffer = nullptr;
  size_t words = 0;
};

LV2_Handle instantiate(const LV2_Descriptor* d, double, const char*, const LV2_Feature* const*) {
  const unsigned long n = std::strtoul(d->URI + std::strlen("urn:gridline:lv2many:"), nullptr, 10);
  if (n == kRefuses) return nullptr;
  auto* m = new (std::nothrow) Many;
  if (!m) return nullptr;
  m->words = kInstanceMegabytes * 1024 * 1024 / sizeof(uint64_t);
  m->buffer = static_cast<uint64_t*>(std::malloc(m->words * sizeof(uint64_t)));
  if (!m->buffer) {
    delete m;
    return nullptr;
  }
  // Noise, so a memory compressor cannot shrink it: it counts as the instance's own memory.
  uint64_t x = 88172645463325252ull + n;
  for (size_t i = 0; i < m->words; ++i) {
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    m->buffer[i] = x;
  }
  return m;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
  auto* m = static_cast<Many*>(h);
  switch (port) {
    case 0: m->in[0] = static_cast<const float*>(data); break;
    case 1: m->in[1] = static_cast<const float*>(data); break;
    case 2: m->out[0] = static_cast<float*>(data); break;
    case 3: m->out[1] = static_cast<float*>(data); break;
    case 4: m->gain = static_cast<const float*>(data); break;
    case 5: m->latency = static_cast<float*>(data); break;
    default: break;  // described control ports this binary does not use
  }
}

void run(LV2_Handle h, uint32_t n) {
  auto* m = static_cast<Many*>(h);
  const float g = m->gain ? *m->gain : 1.0f;
  for (int c = 0; c < 2; ++c)
    if (m->in[c] && m->out[c])
      for (uint32_t i = 0; i < n; ++i) m->out[c][i] = m->in[c][i] * g;
  if (m->latency) *m->latency = 0;
}

void cleanup(LV2_Handle h) {
  auto* m = static_cast<Many*>(h);
  std::free(m->buffer);
  delete m;
}

char gUris[kCount][40];
LV2_Descriptor gDescs[kCount];

}  // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < kCount; ++i) {
      std::snprintf(gUris[i], sizeof gUris[i], "urn:gridline:lv2many:%u", i);
      gDescs[i] = LV2_Descriptor{gUris[i], instantiate, connect, nullptr, run, nullptr, cleanup, nullptr};
    }
  });
  return index < kCount ? &gDescs[index] : nullptr;
}
