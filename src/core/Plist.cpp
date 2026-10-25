// Gridline: minimal property list reader (see Plist.h).
#include "Plist.h"
#include <cstdint>
#include <vector>
#include "../host/Backend.h"

namespace gl::plist {

namespace {

struct Bin {
  const uint8_t* p = nullptr;
  size_t n = 0;
  int offSize = 0, refSize = 0;
  uint64_t count = 0, table = 0;

  uint64_t be(size_t at, int size) const {
    uint64_t v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | p[at + static_cast<size_t>(i)];
    return v;
  }
  bool offsetOf(uint64_t obj, size_t& at) const {
    if (obj >= count) return false;
    const uint64_t pos = table + obj * static_cast<uint64_t>(offSize);
    if (pos + static_cast<uint64_t>(offSize) > n) return false;
    at = static_cast<size_t>(be(static_cast<size_t>(pos), offSize));
    return at < n;
  }
  // Length of an object (low nibble, or a following int object); `at` moves past it.
  bool length(size_t& at, uint64_t& len) const {
    const uint8_t m = p[at];
    len = m & 0x0F;
    ++at;
    if (len != 0x0F) return true;
    if (at >= n || (p[at] & 0xF0) != 0x10) return false;
    const int sz = 1 << (p[at] & 0x0F);
    if (sz > 8 || at + 1 + static_cast<size_t>(sz) > n) return false;
    len = be(at + 1, sz);
    at += 1 + static_cast<size_t>(sz);
    return true;
  }
  // String or data object -> bytes (UTF-16 strings: ASCII range only).
  bool bytes(uint64_t obj, std::string& out, bool& isString) const {
    size_t at;
    if (!offsetOf(obj, at)) return false;
    const uint8_t type = p[at] >> 4;
    uint64_t len;
    if (!length(at, len)) return false;
    if (type == 0x4 || type == 0x5) {
      if (len > n - at) return false;
      out.assign(reinterpret_cast<const char*>(p + at), static_cast<size_t>(len));
      isString = type == 0x5;
      return true;
    }
    if (type == 0x6) {
      if (len > (n - at) / 2) return false;
      out.clear();
      for (uint64_t i = 0; i < len; ++i) {
        const uint16_t c = static_cast<uint16_t>(p[at + 2 * i] << 8 | p[at + 2 * i + 1]);
        out += c < 0x80 ? static_cast<char>(c) : '?';
      }
      isString = true;
      return true;
    }
    return false;
  }
};

bool binaryValue(const std::string& s, const std::string& key, std::string& out) {
  Bin b;
  b.p = reinterpret_cast<const uint8_t*>(s.data());
  b.n = s.size();
  if (b.n < 8 + 32) return false;
  const size_t t = b.n - 32;
  b.offSize = b.p[t + 6];
  b.refSize = b.p[t + 7];
  b.count = b.be(t + 8, 8);
  const uint64_t top = b.be(t + 16, 8);
  b.table = b.be(t + 24, 8);
  if (b.offSize < 1 || b.offSize > 8 || b.refSize < 1 || b.refSize > 8 || b.table >= b.n || b.count > b.n) return false;
  size_t at;
  if (!b.offsetOf(top, at) || (b.p[at] >> 4) != 0xD) return false;
  uint64_t entries;
  if (!b.length(at, entries)) return false;
  const uint64_t rs = static_cast<uint64_t>(b.refSize);
  if (entries > (b.n - at) / (2 * rs)) return false;
  for (uint64_t i = 0; i < entries; ++i) {
    std::string k;
    bool isString = false;
    if (!b.bytes(b.be(at + static_cast<size_t>(i * rs), b.refSize), k, isString) || !isString || k != key) continue;
    return b.bytes(b.be(at + static_cast<size_t>((entries + i) * rs), b.refSize), out, isString);
  }
  return false;
}

std::string unescape(std::string s) {
  static const std::pair<const char*, char> ents[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
  for (const auto& [e, c] : ents)
    for (size_t at = s.find(e); at != std::string::npos; at = s.find(e, at + 1)) s.replace(at, std::char_traits<char>::length(e), 1, c);
  return s;
}

bool xmlValue(const std::string& s, const std::string& key, std::string& out) {
  // Top-level dictionary: the first <dict>, keys at depth 1 only.
  size_t at = s.find("<dict>");
  if (at == std::string::npos) return false;
  at += 6;
  int depth = 1;
  while (at < s.size() && depth > 0) {
    const size_t lt = s.find('<', at);
    if (lt == std::string::npos) return false;
    if (s.compare(lt, 6, "<dict>") == 0 || s.compare(lt, 7, "<array>") == 0) {
      ++depth;
      at = lt + 1;
      continue;
    }
    if (s.compare(lt, 7, "</dict>") == 0 || s.compare(lt, 8, "</array>") == 0) {
      --depth;
      at = lt + 1;
      continue;
    }
    if (depth == 1 && s.compare(lt, 5, "<key>") == 0) {
      const size_t e = s.find("</key>", lt);
      if (e == std::string::npos) return false;
      const std::string k = unescape(s.substr(lt + 5, e - lt - 5));
      at = e + 6;
      if (k != key) continue;
      const size_t v = s.find('<', at);
      if (v == std::string::npos) return false;
      for (const char* tag : {"data", "string"}) {
        const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        if (s.compare(v, open.size(), open) != 0) continue;
        const size_t ve = s.find(close, v);
        if (ve == std::string::npos) return false;
        std::string text = s.substr(v + open.size(), ve - v - open.size());
        if (tag[0] == 's') {
          out = unescape(text);
          return true;
        }
        std::string clean;
        for (char c : text)
          if (c != ' ' && c != '\n' && c != '\r' && c != '\t') clean += c;
        return host::base64Decode(clean, out);
      }
      return false;
    }
    at = lt + 1;
  }
  return false;
}

}  // namespace

bool topValue(const std::string& bytes, const std::string& key, std::string& out) {
  if (bytes.compare(0, 8, "bplist00") == 0) return binaryValue(bytes, key, out);
  if (bytes.find("<plist") != std::string::npos) return xmlValue(bytes, key, out);
  return false;
}

}  // namespace gl::plist
