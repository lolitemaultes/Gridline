// Gridline — FL Studio project (.flp) writing: little-endian byte builder,
// event writer (BYTE / WORD / DWORD / DATA events, FLhd + FLdt container) and
// the "FL origin" event lists the importer keeps for the exporter.
//
// Container (analysis/reference/format-flp.md):
//   "FLhd" u32 6 | i16 format | u16 channel count | u16 PPQ
//   "FLdt" u32 size | events
//   event := u8 id, payload; id 0–63 one byte, 64–127 two, 128–191 four,
//            192–255 LEB128 size + bytes (text: UTF-16LE + NUL).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "Flp.h"

namespace gl::flp {

using Bytes = std::vector<uint8_t>;

// Little-endian builder / patcher over a byte vector.
struct ByteWriter {
  Bytes b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
  void u16(uint32_t v) {
    u8(v & 0xFF);
    u8((v >> 8) & 0xFF);
  }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xFF);
  }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v));
    u32(static_cast<uint32_t>(v >> 32));
  }
  void f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u32(u);
  }
  void f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    u64(u);
  }
  void bytes(const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
  }
  void bytes(const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
  void zeros(size_t n) { b.insert(b.end(), n, 0); }
  size_t size() const { return b.size(); }
};

// In-place little-endian writes into a fixed buffer (no-ops out of range).
inline void putU8(Bytes& b, size_t at, uint32_t v) {
  if (at < b.size()) b[at] = static_cast<uint8_t>(v);
}
inline void putU16(Bytes& b, size_t at, uint32_t v) {
  if (at + 2 <= b.size()) b[at] = static_cast<uint8_t>(v), b[at + 1] = static_cast<uint8_t>(v >> 8);
}
inline void putU32(Bytes& b, size_t at, uint32_t v) {
  if (at + 4 <= b.size())
    for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}
inline void putI32(Bytes& b, size_t at, int32_t v) { putU32(b, at, static_cast<uint32_t>(v)); }
inline void putF32(Bytes& b, size_t at, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  putU32(b, at, u);
}
inline void putF64(Bytes& b, size_t at, double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  putU32(b, at, static_cast<uint32_t>(u));
  putU32(b, at + 4, static_cast<uint32_t>(u >> 32));
}
inline uint32_t getU32(const Bytes& b, size_t at) {
  if (at + 4 > b.size()) return 0;
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16) |
         (static_cast<uint32_t>(b[at + 3]) << 24);
}
inline uint16_t getU16(const Bytes& b, size_t at) {
  return at + 2 <= b.size() ? static_cast<uint16_t>(b[at] | (b[at + 1] << 8)) : 0;
}
inline float getF32(const Bytes& b, size_t at) {
  uint32_t u = getU32(b, at);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline double getF64(const Bytes& b, size_t at) {
  if (at + 8 > b.size()) return 0;
  uint64_t u = static_cast<uint64_t>(getU32(b, at)) | (static_cast<uint64_t>(getU32(b, at + 4)) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// UTF-8 → UTF-16LE with a terminating NUL (FL 11.5+ text events).
Bytes utf16z(const std::string& utf8);
// Size class of an event id: 1, 2, 4 bytes, or 0 = DATA (varint size).
inline int eventWidth(uint8_t id) { return id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0; }

// One event as written (id + payload). Payload sizes of BYTE / WORD / DWORD
// events are fixed except id 172 (FL 25.2.3+: a size selector byte first).
struct RawEvent {
  uint8_t id = 0;
  Bytes data;
  uint32_t value() const {
    uint32_t v = 0;
    for (size_t i = 0; i < data.size() && i < 4; ++i) v |= static_cast<uint32_t>(data[i]) << (8 * i);
    return v;
  }
  // A view the importer's helpers understand (valid while this RawEvent lives).
  Event view() const {
    Event e;
    e.id = id;
    e.data = data.data();
    e.size = static_cast<uint32_t>(data.size());
    return e;
  }
  bool operator==(const RawEvent& o) const { return id == o.id && data == o.data; }
};
using EventList = std::vector<RawEvent>;
// A BYTE / WORD / DWORD event with the payload size of its id's class.
inline RawEvent scalarEvent(uint8_t id, uint32_t v) {
  RawEvent e;
  e.id = id;
  const int n = id < 64 ? 1 : id < 128 ? 2 : 4;
  for (int i = 0; i < n; ++i) e.data.push_back(static_cast<uint8_t>(v >> (8 * i)));
  return e;
}

// Event stream builder.
class EventWriter {
 public:
  // Scalar events: the payload size always follows the id's class (0–63
  // byte, 64–127 word, 128–191 dword), whichever of these is called.
  void byte(uint8_t id, uint32_t v) { value(id, v); }
  void word(uint8_t id, uint32_t v) { value(id, v); }
  void dword(uint8_t id, uint32_t v) { value(id, v); }
  // Scalar event of the id's size class.
  void value(uint8_t id, uint32_t v);
  void data(uint8_t id, const Bytes& b) { put(id, b); }
  void text(uint8_t id, const std::string& utf8) { put(id, utf16z(utf8)); }
  void ascii(uint8_t id, const std::string& s) {  // 8-bit text + NUL (event 199)
    Bytes b(s.begin(), s.end());
    b.push_back(0);
    put(id, b);
  }
  void raw(const RawEvent& e) { put(e.id, e.data); }
  void put(uint8_t id, const Bytes& payload);
  const EventList& events() const { return ev_; }
  EventList& events() { return ev_; }
  // FLhd + FLdt file image.
  Bytes file(int16_t format, uint16_t channelCount, uint16_t ppq) const;

 private:
  EventList ev_;
};

// Encodes an event list in the FLP event encoding (also the FL-origin blob
// format) and back. decodeEvents stops at a truncated tail.
Bytes encodeEvents(const EventList& ev);
EventList decodeEvents(const uint8_t* p, size_t n);
inline EventList decodeEvents(const std::string& s) {
  return decodeEvents(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline EventList decodeEvents(const Bytes& b) { return decodeEvents(b.data(), b.size()); }
inline std::string eventsBlob(const EventList& ev) {
  Bytes b = encodeEvents(ev);
  return std::string(b.begin(), b.end());
}
EventList toRaw(const std::vector<Event>& evs);

// First event with `id` (nullptr if none).
const RawEvent* findRaw(const EventList& ev, uint8_t id);

}  // namespace gl::flp
