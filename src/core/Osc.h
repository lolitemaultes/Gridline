// Gridline: Open Sound Control 1.0 codec (Gridline's own, no third-party code). Messages and
// bundles with the types i f s b (required) and h d t T F N I S c r m (optional ones), time tags,
// address pattern matching (? * [] {}) and SLIP framing for OSC over TCP. Pure logic: no I/O, so
// the server (OscServer.h), the address space (OscControl.h) and the tests share it.
//
// Parsing never trusts a length in the packet: every size is checked against the bytes left, the
// nesting of bundles and the number of messages per packet are capped, so a malformed or hostile
// packet costs at most a few times its own size in memory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::osc {

// One argument. `type` is its OSC type tag character.
struct Arg {
  char type = 'N';
  int64_t i = 0;              // i h c r m (as an integer), t (the time tag), T F (1 / 0)
  double f = 0;               // f d
  std::string s;              // s S
  std::vector<uint8_t> blob;  // b

  static Arg Int(int32_t v);
  static Arg Int64(int64_t v);
  static Arg Float(float v);
  static Arg Double(double v);
  static Arg Str(std::string v);
  static Arg Blob(std::vector<uint8_t> v);
  static Arg Bool(bool v);
  static Arg Nil();
  static Arg Impulse();
  static Arg Time(uint64_t t);

  // i h f d T F c: a number (T = 1, F = 0, c = the character code).
  bool numeric() const;
  double number() const;  // 0 when not numeric
  bool text() const { return type == 's' || type == 'S'; }
  bool operator==(const Arg& o) const;
};

struct Message {
  std::string address;
  std::vector<Arg> args;
  bool operator==(const Message& o) const { return address == o.address && args == o.args; }
  // ",ifs" style type tag string (with the comma).
  std::string typeTags() const;
};

// A message from a packet with the time tag of its innermost bundle (kImmediately outside bundles).
struct Timed {
  uint64_t time = 1;
  Message msg;
};

constexpr uint64_t kImmediately = 1;  // the time tag "now" (OSC 1.0)
constexpr size_t kMaxPacket = 65536;  // larger packets are refused (and a UDP datagram is at most ~64 KiB)
constexpr int kMaxDepth = 8;          // nested bundles
constexpr size_t kMaxMessages = 1024; // messages per packet

// Parses one packet (a message or a bundle). False with a short reason when malformed; `out`
// then keeps nothing from this packet.
bool parse(const uint8_t* data, size_t size, std::vector<Timed>& out, std::string* err = nullptr);
inline bool parse(const std::vector<uint8_t>& p, std::vector<Timed>& out, std::string* err = nullptr) {
  return parse(p.data(), p.size(), out, err);
}

// Encoding. A message's address must start with '/'; arguments of unknown types are written as N.
std::vector<uint8_t> encode(const Message& m);
// A bundle of messages (each element is one encoded message).
std::vector<uint8_t> encodeBundle(uint64_t time, const std::vector<Message>& msgs);
// A bundle whose elements are already encoded packets (messages or bundles).
std::vector<uint8_t> encodeBundlePackets(uint64_t time, const std::vector<std::vector<uint8_t>>& packets);

// Address pattern matching (OSC 1.0): '?' one character, '*' any run of characters, "[a-z]" and
// "[!abc]" one character of (or not of) a set, "{foo,bar}" one of the words. No wildcard crosses
// a '/'. True when `pattern` matches the whole `address`.
bool match(const std::string& pattern, const std::string& address);
// True when the text holds any pattern character.
bool isPattern(const std::string& s);
// A valid address or pattern: starts with '/', printable ASCII, no space or '#' and no ',' outside braces.
bool validAddress(const std::string& s);

// Time tags: 32.32 fixed point seconds since 1900-01-01 (NTP). Unix time in seconds both ways.
uint64_t timeFromUnix(double seconds);
double unixFromTime(uint64_t t);
uint64_t timeNow();

// SLIP framing (RFC 1055, double END as OSC 1.1 suggests) for OSC over TCP.
constexpr uint8_t kSlipEnd = 0xC0, kSlipEsc = 0xDB, kSlipEscEnd = 0xDC, kSlipEscEsc = 0xDD;
std::vector<uint8_t> slipEncode(const std::vector<uint8_t>& packet);
// Reassembles packets from a byte stream. A frame longer than `maxFrame` is dropped whole (counted
// in `overflows`); the decoder then waits for the next END.
class SlipDecoder {
 public:
  explicit SlipDecoder(size_t maxFrame = kMaxPacket) : max_(maxFrame) {}
  // Feeds bytes; every complete frame is appended to `frames`.
  void feed(const uint8_t* data, size_t n, std::vector<std::vector<uint8_t>>& frames);
  size_t overflows() const { return overflows_; }
  void reset() {
    cur_.clear();
    esc_ = skip_ = false;
  }

 private:
  size_t max_;
  std::vector<uint8_t> cur_;
  bool esc_ = false, skip_ = false;
  size_t overflows_ = 0;
};

// "/mixer/1/volume ,f 0.5" style line for logs and the activity monitor (arguments shortened).
std::string describe(const Message& m, size_t maxLen = 120);

}  // namespace gl::osc
