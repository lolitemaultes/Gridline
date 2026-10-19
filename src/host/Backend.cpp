#include "Backend.h"
#include <algorithm>

namespace gl::host {

// Vyukov bounded queue: each cell's sequence number says whether it is free
// for the producer at position p (seq == p) or holds data for the consumer
// (seq == p + 1).
HostEventQueue::HostEventQueue() : cells_(new Cell[kSize]) {
  for (size_t i = 0; i < kSize; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
}

bool HostEventQueue::push(const HostEvent& e) {
  size_t pos = head_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& c = cells_[pos & (kSize - 1)];
    size_t seq = c.seq.load(std::memory_order_acquire);
    intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
    if (dif == 0) {
      if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        c.ev = e;
        c.seq.store(pos + 1, std::memory_order_release);
        return true;
      }
    } else if (dif < 0) {
      return false;  // full
    } else {
      pos = head_.load(std::memory_order_relaxed);
    }
  }
}

bool HostEventQueue::pop(HostEvent& e) {
  size_t pos = tail_.load(std::memory_order_relaxed);
  Cell& c = cells_[pos & (kSize - 1)];
  size_t seq = c.seq.load(std::memory_order_acquire);
  if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0) return false;  // empty
  e = c.ev;
  c.seq.store(pos + kSize, std::memory_order_release);
  tail_.store(pos + 1, std::memory_order_relaxed);
  return true;
}

void Backend::indexParams() {
  const auto& ps = params();
  lookup_.clear();
  lookup_.reserve(ps.size());
  for (size_t i = 0; i < ps.size(); ++i) lookup_.push_back({ps[i].id, static_cast<int>(i)});
  std::sort(lookup_.begin(), lookup_.end());
}

int Backend::paramIndex(uint32_t id) const {
  auto it = std::lower_bound(lookup_.begin(), lookup_.end(), std::make_pair(id, -1));
  return it != lookup_.end() && it->first == id ? it->second : -1;
}

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64Encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

bool base64Decode(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (char ch : in) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

}  // namespace gl::host
