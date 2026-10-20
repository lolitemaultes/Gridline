// Gridline — FLP writing primitives (see FlpWriter.h).
#include "FlpWriter.h"

namespace gl::flp {

Bytes utf16z(const std::string& s) {
  Bytes out;
  size_t i = 0;
  auto unit = [&](uint32_t u) {
    out.push_back(static_cast<uint8_t>(u & 0xFF));
    out.push_back(static_cast<uint8_t>(u >> 8));
  };
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    size_t n = 1;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 6 && i + 1 < s.size()) cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu), n = 2;
    else if ((c >> 4) == 14 && i + 2 < s.size())
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu), n = 3;
    else if ((c >> 3) == 30 && i + 3 < s.size())
      cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu),
      n = 4;
    i += n;
    if (cp >= 0x10000 && cp < 0x110000) {
      cp -= 0x10000;
      unit(0xD800 + (cp >> 10));
      unit(0xDC00 + (cp & 0x3FF));
    } else {
      unit(cp > 0xFFFF ? 0xFFFD : cp);
    }
  }
  unit(0);
  return out;
}

void EventWriter::value(uint8_t id, uint32_t v) {
  switch (eventWidth(id)) {
    case 1: put(id, {static_cast<uint8_t>(v)}); break;
    case 2: put(id, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}); break;
    case 4:
      put(id, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
      break;
    default: {
      ByteWriter w;
      w.u32(v);
      put(id, w.b);
    }
  }
}

void EventWriter::put(uint8_t id, const Bytes& payload) {
  RawEvent e;
  e.id = id;
  e.data = payload;
  ev_.push_back(std::move(e));
}

Bytes encodeEvents(const EventList& ev) {
  Bytes out;
  size_t total = 0;
  for (const auto& e : ev) total += e.data.size() + 6;
  out.reserve(total);
  for (const auto& e : ev) {
    out.push_back(e.id);
    if (e.id >= 192) {
      size_t n = e.data.size();
      do {
        uint8_t b = n & 0x7F;
        n >>= 7;
        if (n) b |= 0x80;
        out.push_back(b);
      } while (n);
    }
    out.insert(out.end(), e.data.begin(), e.data.end());
  }
  return out;
}

EventList decodeEvents(const uint8_t* d, size_t end) {
  EventList out;
  size_t p = 0;
  while (p < end) {
    RawEvent e;
    e.id = d[p++];
    size_t n;
    if (e.id < 64) {
      n = 1;
    } else if (e.id < 128) {
      n = 2;
    } else if (e.id < 192) {
      n = 4;
      if (e.id == 172 && p < end) n = d[p] == 1 ? 3 : d[p] == 0 ? 5 : 4;
    } else {
      n = 0;
      int shift = 0;
      while (p < end) {
        uint8_t b = d[p++];
        n |= static_cast<size_t>(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80) || shift > 28) break;
      }
    }
    if (n > end - p) break;
    e.data.assign(d + p, d + p + n);
    out.push_back(std::move(e));
    p += n;
  }
  return out;
}

EventList toRaw(const std::vector<Event>& evs) {
  EventList out;
  out.reserve(evs.size());
  for (const Event& e : evs) {
    RawEvent r;
    r.id = e.id;
    r.data.assign(e.data, e.data + e.size);
    out.push_back(std::move(r));
  }
  return out;
}

const RawEvent* findRaw(const EventList& ev, uint8_t id) {
  for (const auto& e : ev)
    if (e.id == id) return &e;
  return nullptr;
}

Bytes EventWriter::file(int16_t format, uint16_t channelCount, uint16_t ppq) const {
  Bytes body = encodeEvents(ev_);
  ByteWriter w;
  w.bytes("FLhd", 4);
  w.u32(6);
  w.u16(static_cast<uint16_t>(format));
  w.u16(channelCount);
  w.u16(ppq);
  w.bytes("FLdt", 4);
  w.u32(static_cast<uint32_t>(body.size()));
  w.bytes(body);
  return w.b;
}

}  // namespace gl::flp
