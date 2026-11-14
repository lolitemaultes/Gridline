// Gridline: Open Sound Control 1.0 codec (see Osc.h).
#include "Osc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gl::osc {

// ------------------------------------------------------------------ arguments ----

Arg Arg::Int(int32_t v) {
  Arg a;
  a.type = 'i';
  a.i = v;
  return a;
}
Arg Arg::Int64(int64_t v) {
  Arg a;
  a.type = 'h';
  a.i = v;
  return a;
}
Arg Arg::Float(float v) {
  Arg a;
  a.type = 'f';
  a.f = v;
  return a;
}
Arg Arg::Double(double v) {
  Arg a;
  a.type = 'd';
  a.f = v;
  return a;
}
Arg Arg::Str(std::string v) {
  Arg a;
  a.type = 's';
  a.s = std::move(v);
  return a;
}
Arg Arg::Blob(std::vector<uint8_t> v) {
  Arg a;
  a.type = 'b';
  a.blob = std::move(v);
  return a;
}
Arg Arg::Bool(bool v) {
  Arg a;
  a.type = v ? 'T' : 'F';
  a.i = v ? 1 : 0;
  return a;
}
Arg Arg::Nil() { return Arg{}; }
Arg Arg::Impulse() {
  Arg a;
  a.type = 'I';
  return a;
}
Arg Arg::Time(uint64_t t) {
  Arg a;
  a.type = 't';
  a.i = static_cast<int64_t>(t);
  return a;
}

bool Arg::numeric() const {
  switch (type) {
    case 'i':
    case 'h':
    case 'f':
    case 'd':
    case 'T':
    case 'F':
    case 'c': return true;
    default: return false;
  }
}

double Arg::number() const {
  switch (type) {
    case 'i':
    case 'h':
    case 'c': return static_cast<double>(i);
    case 'f':
    case 'd': return f;
    case 'T': return 1.0;
    default: return 0.0;
  }
}

bool Arg::operator==(const Arg& o) const {
  if (type != o.type) return false;
  switch (type) {
    case 'f':
    case 'd': return f == o.f || (f != f && o.f != o.f);  // NaN equals NaN here (round trips)
    case 's':
    case 'S': return s == o.s;
    case 'b': return blob == o.blob;
    default: return i == o.i;
  }
}

std::string Message::typeTags() const {
  std::string t = ",";
  for (const auto& a : args) t += a.type;
  return t;
}

// ------------------------------------------------------------------ reading ----

namespace {

struct Reader {
  Reader(const uint8_t* data, size_t size, size_t at = 0) : p(data), n(size), pos(at) {}
  const uint8_t* p;
  size_t n, pos;
  size_t left() const { return n - pos; }
  bool u32(uint32_t& v) {
    if (left() < 4) return false;
    v = (uint32_t(p[pos]) << 24) | (uint32_t(p[pos + 1]) << 16) | (uint32_t(p[pos + 2]) << 8) | uint32_t(p[pos + 3]);
    pos += 4;
    return true;
  }
  bool u64(uint64_t& v) {
    uint32_t hi = 0, lo = 0;
    if (left() < 8 || !u32(hi) || !u32(lo)) return false;
    v = (uint64_t(hi) << 32) | lo;
    return true;
  }
  // OSC-string: characters, a NUL, NULs up to a multiple of 4.
  bool str(std::string& s) {
    const void* z = std::memchr(p + pos, 0, left());
    if (!z) return false;
    const size_t len = static_cast<size_t>(static_cast<const uint8_t*>(z) - (p + pos));
    const size_t padded = (len + 4) & ~size_t(3);
    if (padded > left()) return false;
    s.assign(reinterpret_cast<const char*>(p + pos), len);
    pos += padded;
    return true;
  }
};

struct Budget {
  size_t messages = 0, args = 0;
};
constexpr size_t kMaxArgsPerPacket = 8192;

bool fail(std::string* err, const char* why) {
  if (err) *err = why;
  return false;
}

bool parseMessage(Reader& r, uint64_t time, std::vector<Timed>& out, Budget& b, std::string* err) {
  Timed t;
  t.time = time;
  if (!r.str(t.msg.address)) return fail(err, "address is not terminated");
  if (t.msg.address.empty() || t.msg.address[0] != '/') return fail(err, "address does not start with '/'");
  if (++b.messages > kMaxMessages) return fail(err, "too many messages in one packet");
  if (r.left() == 0) {  // no type tag string (very old senders): no arguments
    out.push_back(std::move(t));
    return true;
  }
  std::string tags;
  if (!r.str(tags)) return fail(err, "type tags are not terminated");
  if (tags.empty() || tags[0] != ',') return fail(err, "type tags do not start with ','");
  int depth = 0;
  for (size_t k = 1; k < tags.size(); ++k) {
    const char c = tags[k];
    if (c == '[') {  // OSC 1.1 arrays: their elements are read as plain arguments
      ++depth;
      continue;
    }
    if (c == ']') {
      if (--depth < 0) return fail(err, "unbalanced ']' in type tags");
      continue;
    }
    if (++b.args > kMaxArgsPerPacket) return fail(err, "too many arguments");
    Arg a;
    a.type = c;
    uint32_t u = 0;
    uint64_t w = 0;
    switch (c) {
      case 'i':
      case 'c':
      case 'r':
      case 'm':
        if (!r.u32(u)) return fail(err, "truncated 32-bit argument");
        a.i = c == 'i' || c == 'c' ? static_cast<int64_t>(static_cast<int32_t>(u)) : static_cast<int64_t>(u);
        break;
      case 'f': {
        if (!r.u32(u)) return fail(err, "truncated float");
        float f;
        std::memcpy(&f, &u, 4);
        a.f = f;
        break;
      }
      case 'h':
      case 't':
        if (!r.u64(w)) return fail(err, "truncated 64-bit argument");
        a.i = static_cast<int64_t>(w);
        break;
      case 'd': {
        if (!r.u64(w)) return fail(err, "truncated double");
        double d;
        std::memcpy(&d, &w, 8);
        a.f = d;
        break;
      }
      case 's':
      case 'S':
        if (!r.str(a.s)) return fail(err, "string argument is not terminated");
        break;
      case 'b': {
        if (!r.u32(u)) return fail(err, "truncated blob size");
        const size_t len = u, padded = (len + 3) & ~size_t(3);
        if (u > r.left() || padded > r.left()) return fail(err, "blob larger than the packet");
        a.blob.assign(r.p + r.pos, r.p + r.pos + len);
        r.pos += padded;
        break;
      }
      case 'T': a.i = 1; break;
      case 'F':
      case 'N':
      case 'I': break;
      default: return fail(err, "unknown type tag");
    }
    t.msg.args.push_back(std::move(a));
  }
  if (depth != 0) return fail(err, "unbalanced '[' in type tags");
  out.push_back(std::move(t));
  return true;
}

bool parseElement(const uint8_t* p, size_t n, uint64_t time, int depth, std::vector<Timed>& out, Budget& b, std::string* err) {
  if (n == 0) return fail(err, "empty packet");
  if (n % 4 != 0) return fail(err, "size is not a multiple of 4");
  if (p[0] == '/') {
    Reader r{p, n};
    if (!parseMessage(r, time, out, b, err)) return false;
    if (r.left() != 0) return fail(err, "trailing bytes after the arguments");
    return true;
  }
  if (p[0] != '#') return fail(err, "neither a message nor a bundle");
  if (depth >= kMaxDepth) return fail(err, "bundles nested too deep");
  if (n < 16 || std::memcmp(p, "#bundle\0", 8) != 0) return fail(err, "bad bundle header");
  Reader r{p, n, 8};
  uint64_t tt = 0;
  r.u64(tt);
  // An element's own tag applies; "immediately" inside a timed bundle keeps the outer time.
  const uint64_t when = tt == kImmediately ? time : tt;
  while (r.left() > 0) {
    uint32_t size = 0;
    if (!r.u32(size)) return fail(err, "truncated bundle element size");
    if (size > r.left()) return fail(err, "bundle element larger than the bundle");
    if (!parseElement(p + r.pos, size, when, depth + 1, out, b, err)) return false;
    r.pos += size;
  }
  return true;
}

// ------------------------------------------------------------------ writing ----

void putU32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 24));
  o.push_back(static_cast<uint8_t>(v >> 16));
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}
void putU64(std::vector<uint8_t>& o, uint64_t v) {
  putU32(o, static_cast<uint32_t>(v >> 32));
  putU32(o, static_cast<uint32_t>(v));
}
void putStr(std::vector<uint8_t>& o, const std::string& s) {
  const size_t len = std::min(s.size(), std::strlen(s.c_str()));  // stops at an embedded NUL
  o.insert(o.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(len));
  const size_t pad = 4 - (len % 4);
  o.insert(o.end(), pad, 0);
}

}  // namespace

bool parse(const uint8_t* data, size_t size, std::vector<Timed>& out, std::string* err) {
  if (!data || size == 0) return fail(err, "empty packet");
  if (size > kMaxPacket) return fail(err, "packet too large");
  const size_t before = out.size();
  Budget b;
  if (!parseElement(data, size, kImmediately, 0, out, b, err)) {
    out.resize(before);
    return false;
  }
  return true;
}

std::vector<uint8_t> encode(const Message& m) {
  std::vector<uint8_t> o;
  putStr(o, m.address.empty() ? std::string("/") : m.address);
  std::string tags = ",";
  for (const auto& a : m.args) {
    switch (a.type) {
      case 'i':
      case 'h':
      case 'f':
      case 'd':
      case 's':
      case 'S':
      case 'b':
      case 't':
      case 'c':
      case 'r':
      case 'm':
      case 'T':
      case 'F':
      case 'N':
      case 'I': tags += a.type; break;
      default: tags += 'N'; break;
    }
  }
  putStr(o, tags);
  for (size_t k = 0; k < m.args.size(); ++k) {
    const Arg& a = m.args[k];
    switch (tags[k + 1]) {
      case 'i':
      case 'c':
      case 'r':
      case 'm': putU32(o, static_cast<uint32_t>(a.i)); break;
      case 'h':
      case 't': putU64(o, static_cast<uint64_t>(a.i)); break;
      case 'f': {
        const float f = static_cast<float>(a.f);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        putU32(o, u);
        break;
      }
      case 'd': {
        uint64_t u;
        std::memcpy(&u, &a.f, 8);
        putU64(o, u);
        break;
      }
      case 's':
      case 'S': putStr(o, a.s); break;
      case 'b':
        putU32(o, static_cast<uint32_t>(a.blob.size()));
        o.insert(o.end(), a.blob.begin(), a.blob.end());
        o.insert(o.end(), (4 - a.blob.size() % 4) % 4, 0);
        break;
      default: break;
    }
  }
  return o;
}

std::vector<uint8_t> encodeBundlePackets(uint64_t time, const std::vector<std::vector<uint8_t>>& packets) {
  std::vector<uint8_t> o = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
  putU64(o, time);
  for (const auto& p : packets) {
    putU32(o, static_cast<uint32_t>(p.size()));
    o.insert(o.end(), p.begin(), p.end());
  }
  return o;
}

std::vector<uint8_t> encodeBundle(uint64_t time, const std::vector<Message>& msgs) {
  std::vector<std::vector<uint8_t>> ps;
  ps.reserve(msgs.size());
  for (const auto& m : msgs) ps.push_back(encode(m));
  return encodeBundlePackets(time, ps);
}

// ------------------------------------------------------------------ patterns ----

namespace {

// One element of a compiled pattern segment.
struct Tok {
  enum Kind { Lit, Any, Star, Set, Alt } kind = Lit;
  char c = 0;
  bool negate = false;
  std::string set;                // Set: the characters (ranges expanded)
  std::vector<std::string> alts;  // Alt: the words
};

std::vector<Tok> compile(const std::string& p) {
  std::vector<Tok> t;
  for (size_t i = 0; i < p.size(); ++i) {
    const char c = p[i];
    Tok k;
    if (c == '?') {
      k.kind = Tok::Any;
    } else if (c == '*') {
      if (!t.empty() && t.back().kind == Tok::Star) continue;  // "**" is "*"
      k.kind = Tok::Star;
    } else if (c == '[') {
      const size_t e = p.find(']', i + 1);
      if (e == std::string::npos) {
        k.c = c;
      } else {
        k.kind = Tok::Set;
        size_t j = i + 1;
        if (j < e && p[j] == '!') k.negate = true, ++j;
        for (; j < e; ++j) {
          if (j + 2 < e && p[j + 1] == '-') {
            const unsigned char a = static_cast<unsigned char>(p[j]), b = static_cast<unsigned char>(p[j + 2]);
            for (unsigned v = std::min(a, b); v <= std::max(a, b); ++v) k.set += static_cast<char>(v);
            j += 2;
          } else {
            k.set += p[j];
          }
        }
        i = e;
      }
    } else if (c == '{') {
      const size_t e = p.find('}', i + 1);
      if (e == std::string::npos) {
        k.c = c;
      } else {
        k.kind = Tok::Alt;
        size_t s = i + 1;
        for (;;) {
          const size_t comma = p.find(',', s);
          if (comma == std::string::npos || comma > e) {
            k.alts.push_back(p.substr(s, e - s));
            break;
          }
          k.alts.push_back(p.substr(s, comma - s));
          s = comma + 1;
        }
        i = e;
      }
    } else {
      k.c = c;
    }
    t.push_back(std::move(k));
  }
  return t;
}

// Whole-segment match by dynamic programming (no recursion: hostile patterns cannot blow the stack).
bool matchSegment(const std::string& pat, const std::string& a) {
  if (!isPattern(pat)) return pat == a;
  const std::vector<Tok> t = compile(pat);
  const size_t T = t.size(), L = a.size();
  if ((T + 1) * (L + 1) > (size_t(4) << 20)) return false;  // absurd sizes: no match
  std::vector<uint8_t> dp((T + 1) * (L + 1), 0);
  auto at = [&](size_t ti, size_t ai) -> uint8_t& { return dp[ti * (L + 1) + ai]; };
  at(T, L) = 1;
  for (size_t ti = T; ti-- > 0;) {
    const Tok& k = t[ti];
    for (size_t ai = L + 1; ai-- > 0;) {
      bool ok = false;
      switch (k.kind) {
        case Tok::Lit: ok = ai < L && a[ai] == k.c && at(ti + 1, ai + 1); break;
        case Tok::Any: ok = ai < L && at(ti + 1, ai + 1); break;
        case Tok::Star: ok = at(ti + 1, ai) || (ai < L && at(ti, ai + 1)); break;
        case Tok::Set:
          ok = ai < L && ((k.set.find(a[ai]) != std::string::npos) != k.negate) && at(ti + 1, ai + 1);
          break;
        case Tok::Alt:
          for (const auto& w : k.alts)
            if (ai + w.size() <= L && a.compare(ai, w.size(), w) == 0 && at(ti + 1, ai + w.size())) {
              ok = true;
              break;
            }
          break;
      }
      at(ti, ai) = ok ? 1 : 0;
    }
  }
  return at(0, 0) != 0;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i <= s.size()) {
    const size_t j = s.find('/', i);
    if (j == std::string::npos) {
      v.push_back(s.substr(i));
      break;
    }
    v.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return v;
}

}  // namespace

bool isPattern(const std::string& s) { return s.find_first_of("?*[]{}") != std::string::npos; }

bool validAddress(const std::string& s) {
  if (s.empty() || s[0] != '/' || s.size() > 1024) return false;
  int brace = 0;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x21 || u > 0x7E || c == '#') return false;
    if (c == '{') ++brace;
    if (c == '}') --brace;
    if (c == ',' && brace <= 0) return false;
  }
  return true;
}

bool match(const std::string& pattern, const std::string& address) {
  if (!isPattern(pattern)) return pattern == address;
  const auto ps = split(pattern), as = split(address);
  if (ps.size() != as.size()) return false;
  for (size_t i = 0; i < ps.size(); ++i)
    if (!matchSegment(ps[i], as[i])) return false;
  return true;
}

// ------------------------------------------------------------------ time ----

namespace {
constexpr double kNtpToUnix = 2208988800.0;  // seconds from 1900-01-01 to 1970-01-01
}

uint64_t timeFromUnix(double seconds) {
  double s = seconds + kNtpToUnix;
  if (!(s > 0)) return 0;
  if (s >= 4294967296.0) s = 4294967295.999;
  const double whole = std::floor(s);
  const uint64_t frac = static_cast<uint64_t>((s - whole) * 4294967296.0);
  return (static_cast<uint64_t>(whole) << 32) | std::min<uint64_t>(frac, 0xFFFFFFFFull);
}

double unixFromTime(uint64_t t) {
  return static_cast<double>(t >> 32) + static_cast<double>(t & 0xFFFFFFFFull) / 4294967296.0 - kNtpToUnix;
}

uint64_t timeNow() {
  const auto d = std::chrono::system_clock::now().time_since_epoch();
  return timeFromUnix(std::chrono::duration<double>(d).count());
}

// ------------------------------------------------------------------ SLIP ----

std::vector<uint8_t> slipEncode(const std::vector<uint8_t>& packet) {
  std::vector<uint8_t> o;
  o.reserve(packet.size() + 8);
  o.push_back(kSlipEnd);
  for (uint8_t b : packet) {
    if (b == kSlipEnd) {
      o.push_back(kSlipEsc);
      o.push_back(kSlipEscEnd);
    } else if (b == kSlipEsc) {
      o.push_back(kSlipEsc);
      o.push_back(kSlipEscEsc);
    } else {
      o.push_back(b);
    }
  }
  o.push_back(kSlipEnd);
  return o;
}

void SlipDecoder::feed(const uint8_t* data, size_t n, std::vector<std::vector<uint8_t>>& frames) {
  for (size_t k = 0; k < n; ++k) {
    uint8_t b = data[k];
    if (b == kSlipEnd) {
      if (!skip_ && !cur_.empty()) frames.push_back(cur_);
      cur_.clear();
      esc_ = skip_ = false;
      continue;
    }
    if (skip_) continue;
    if (esc_) {
      esc_ = false;
      if (b == kSlipEscEnd) b = kSlipEnd;
      else if (b == kSlipEscEsc) b = kSlipEsc;
    } else if (b == kSlipEsc) {
      esc_ = true;
      continue;
    }
    if (cur_.size() >= max_) {
      cur_.clear();
      skip_ = true;
      ++overflows_;
      continue;
    }
    cur_.push_back(b);
  }
}

// ------------------------------------------------------------------ text ----

std::string describe(const Message& m, size_t maxLen) {
  std::string s = m.address;
  if (!m.args.empty()) s += " " + m.typeTags();
  for (const auto& a : m.args) {
    if (s.size() >= maxLen) break;
    char b[64];
    switch (a.type) {
      case 'f':
      case 'd': std::snprintf(b, sizeof b, " %g", a.f); break;
      case 'i':
      case 'h':
      case 'c':
      case 'r':
      case 'm': std::snprintf(b, sizeof b, " %lld", static_cast<long long>(a.i)); break;
      case 's':
      case 'S': std::snprintf(b, sizeof b, " \"%.40s\"", a.s.c_str()); break;
      case 'b': std::snprintf(b, sizeof b, " <%zu bytes>", a.blob.size()); break;
      case 'T': std::snprintf(b, sizeof b, " true"); break;
      case 'F': std::snprintf(b, sizeof b, " false"); break;
      case 'I': std::snprintf(b, sizeof b, " impulse"); break;
      case 't': std::snprintf(b, sizeof b, " t%llu", static_cast<unsigned long long>(a.i)); break;
      default: std::snprintf(b, sizeof b, " nil"); break;
    }
    s += b;
  }
  if (s.size() > maxLen) s = s.substr(0, maxLen - 3) + "...";
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20) c = '?';
  return s;
}

}  // namespace gl::osc
