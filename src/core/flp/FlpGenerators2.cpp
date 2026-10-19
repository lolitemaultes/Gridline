// Gridline — FLP import: second-wave instrument decoders (plugins used across FL's
// supplied projects beyond the first three demos). Must not override names
// already registered by the first wave. Shared helpers for the group files.
#include <algorithm>
#include <memory>
#include "FlpGen2.h"

namespace gl::flp {

namespace gen2 {

std::string Blob::ascii(size_t at, size_t maxLen) const {
  std::string s;
  for (size_t i = at; i < n && s.size() < maxLen && p[i]; ++i) {
    uint8_t c = p[i];
    if (c < 0x80) {
      s += static_cast<char>(c);
    } else {
      s += static_cast<char>(0xC0 | (c >> 6));
      s += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return s;
}

std::string Blob::utf16(size_t at, size_t chars) const {
  std::string s;
  for (size_t k = 0; k < chars && has(at + 2 * k, 2); ++k) {
    uint32_t cu = u16(at + 2 * k);
    if (cu == 0) break;
    if (cu >= 0xD800 && cu < 0xDC00 && k + 1 < chars) {
      uint32_t lo = u16(at + 2 * (k + 1));
      cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
      ++k;
    }
    if (cu < 0x80) {
      s += static_cast<char>(cu);
    } else if (cu < 0x800) {
      s += static_cast<char>(0xC0 | (cu >> 6));
      s += static_cast<char>(0x80 | (cu & 0x3F));
    } else if (cu < 0x10000) {
      s += static_cast<char>(0xE0 | (cu >> 12));
      s += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cu & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (cu >> 18));
      s += static_cast<char>(0x80 | ((cu >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cu & 0x3F));
    }
  }
  return s;
}

size_t Blob::find(const std::string& pattern, size_t from) const {
  if (pattern.empty() || from >= n || pattern.size() > n) return std::string::npos;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(pattern.data());
  for (size_t i = from; i + pattern.size() <= n; ++i)
    if (std::memcmp(p + i, b, pattern.size()) == 0) return i;
  return std::string::npos;
}

Blob stateBlob(const std::vector<Event>& events) {
  const Event* e = find(events, 213);
  return e ? Blob(e->data, e->size) : Blob();
}

Blob wrapperBlob(const std::vector<Event>& events) {
  const Event* e = find(events, 212);
  return e ? Blob(e->data, e->size) : Blob();
}

void begin(PluginMapping& m, const char* glType) {
  m.glType = glType;
  m.state = defaultPluginState(glType);
}

void set(PluginMapping& m, int glParam, double value) {
  if (glParam >= 0 && static_cast<size_t>(glParam) < m.state.params.size()) m.state.params[static_cast<size_t>(glParam)] = static_cast<float>(value);
}

void linkParams(PluginMapping& m, std::vector<Link> links, std::vector<RawRange> ranges) {
  setRawRanges(m, std::move(ranges));
  auto table = std::make_shared<std::map<int, Link>>();
  for (auto& l : links) (*table)[l.fl] = std::move(l);
  std::string type = m.glType;
  m.param = [table](int fl) {
    auto it = table->find(fl);
    return it == table->end() ? -1 : it->second.gl;
  };
  m.value = [table, type](int fl, double norm) {
    auto it = table->find(fl);
    if (it == table->end()) return norm;
    if (it->second.conv) return it->second.conv(norm);
    const PluginInfo* info = findPlugin(type);
    if (!info || it->second.gl < 0 || static_cast<size_t>(it->second.gl) >= info->params.size()) return norm;
    return info->params[static_cast<size_t>(it->second.gl)].fromNormalized(norm);
  };
}

}  // namespace gen2

void registerGeneratorDecoders2(std::map<std::string, PluginDecoder>& d) {
  gen2::registerSlicers(d);
  gen2::registerPads(d);
  gen2::registerSynthsA(d);
  gen2::registerSynthsB(d);
  gen2::registerControllers(d);
}

}  // namespace gl::flp
