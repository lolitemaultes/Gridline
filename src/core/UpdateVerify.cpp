// Gridline - automatic updates: SHA-512, Ed25519 and minisign checks (see UpdateVerify.h).
#include "UpdateVerify.h"
#include <cstring>
#include <cstdlib>
#include "UpdateKeys.h"
#include "monocypher.h"

namespace gl::update {

// ------------------------------------------------------------------ SHA-512

namespace {
constexpr uint64_t kK512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
}  // namespace

Sha512::Sha512()
    : h_{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
         0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL},
      buf_{} {}

void Sha512::block(const uint8_t* p) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v = (v << 8) | p[i * 8 + b];
    w[i] = v;
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 80; ++i) {
    const uint64_t t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + kK512[i] + w[i];
    const uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha512::add(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  total_ += n;
  while (n > 0) {
    const size_t take = n < sizeof buf_ - used_ ? n : sizeof buf_ - used_;
    std::memcpy(buf_ + used_, p, take);
    used_ += take, p += take, n -= take;
    if (used_ == sizeof buf_) {
      block(buf_);
      used_ = 0;
    }
  }
}

std::array<uint8_t, 64> Sha512::finish() {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  add(&one, 1);
  while (used_ != 112) add(&zero, 1);
  uint8_t len[16] = {};
  for (int i = 0; i < 8; ++i) len[8 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  add(len, 16);
  std::array<uint8_t, 64> out{};
  for (int i = 0; i < 8; ++i)
    for (int b = 0; b < 8; ++b) out[static_cast<size_t>(i * 8 + b)] = static_cast<uint8_t>(h_[i] >> (56 - 8 * b));
  return out;
}

std::array<uint8_t, 64> sha512(const void* data, size_t n) {
  Sha512 s;
  s.add(data, n);
  return s.finish();
}

std::string hexOf(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
  return s;
}

// ------------------------------------------------------------------ base64

std::string base64Encode(const std::vector<uint8_t>& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
    out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(in[i]) << 16;
    out += t[v >> 18], out += t[(v >> 12) & 63], out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
  }
  return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
  out.clear();
  if (in.size() % 4 != 0) return false;
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + static_cast<size_t>(k)];
      if (c == '=') {
        if (i + 4 != in.size() || k < 2) return false;  // padding only at the very end
        v[k] = 0;
        ++pad;
      } else {
        if (pad) return false;
        v[k] = val(c);
        if (v[k] < 0) return false;
      }
    }
    const uint32_t x = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    out.push_back(static_cast<uint8_t>(x >> 16));
    if (pad < 2) out.push_back(static_cast<uint8_t>(x >> 8));
    if (pad < 1) out.push_back(static_cast<uint8_t>(x));
  }
  return true;
}

// ------------------------------------------------------------------ Ed25519

namespace {

// h = SHA-512(a || b || c) mod L
void hashReduce(uint8_t out[32], const uint8_t* a, size_t na, const uint8_t* b, size_t nb, const uint8_t* c, size_t nc) {
  Sha512 s;
  s.add(a, na);
  s.add(b, nb);
  s.add(c, nc);
  std::array<uint8_t, 64> h = s.finish();
  crypto_eddsa_reduce(out, h.data());
}

}  // namespace

std::array<uint8_t, 32> ed25519PublicKey(const std::array<uint8_t, 32>& seed) {
  std::array<uint8_t, 64> az = sha512(seed.data(), seed.size());
  uint8_t a[32];
  crypto_eddsa_trim_scalar(a, az.data());
  std::array<uint8_t, 32> pub{};
  crypto_eddsa_scalarbase(pub.data(), a);
  crypto_wipe(a, sizeof a);
  crypto_wipe(az.data(), az.size());
  return pub;
}

std::array<uint8_t, 64> ed25519Sign(const std::array<uint8_t, 32>& seed, const uint8_t* msg, size_t n) {
  std::array<uint8_t, 64> az = sha512(seed.data(), seed.size());
  uint8_t a[32], r[32], h[32];
  crypto_eddsa_trim_scalar(a, az.data());
  const std::array<uint8_t, 32> pub = ed25519PublicKey(seed);
  hashReduce(r, az.data() + 32, 32, msg, n, nullptr, 0);
  std::array<uint8_t, 64> sig{};
  crypto_eddsa_scalarbase(sig.data(), r);
  hashReduce(h, sig.data(), 32, pub.data(), 32, msg, n);
  crypto_eddsa_mul_add(sig.data() + 32, h, a, r);  // S = h * a + r (mod L)
  crypto_wipe(a, sizeof a);
  crypto_wipe(r, sizeof r);
  crypto_wipe(az.data(), az.size());
  return sig;
}

bool ed25519Verify(const uint8_t sig[64], const uint8_t pub[32], const uint8_t* msg, size_t n) {
  uint8_t h[32];
  hashReduce(h, sig, 32, pub, 32, msg, n);
  return crypto_eddsa_check_equation(sig, pub, h) == 0;
}

// ------------------------------------------------------------------ minisign

namespace {

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= text.size()) {
    size_t b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    std::string l = text.substr(a, b - a);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
    a = b + 1;
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

std::string trimSpaces(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

}  // namespace

bool parseMinisignKey(const std::string& text, MinisignKey& out) {
  for (const std::string& raw : lines(text)) {
    const std::string l = trimSpaces(raw);
    if (l.empty() || l.rfind("untrusted comment:", 0) == 0) continue;
    std::vector<uint8_t> b;
    if (!base64Decode(l, b) || b.size() != 42 || b[0] != 'E' || b[1] != 'd') return false;
    std::memcpy(out.keyId.data(), b.data() + 2, 8);
    std::memcpy(out.pub.data(), b.data() + 10, 32);
    return true;
  }
  return false;
}

std::string minisignKeyText(const MinisignKey& k) {
  std::vector<uint8_t> b = {'E', 'd'};
  b.insert(b.end(), k.keyId.begin(), k.keyId.end());
  b.insert(b.end(), k.pub.begin(), k.pub.end());
  return base64Encode(b);
}

const char* describe(SigResult r) {
  switch (r) {
    case SigResult::Ok: return "signature good";
    case SigResult::Malformed: return "the signature file is damaged";
    case SigResult::WrongKey: return "signed with a key this build does not trust";
    case SigResult::BadSignature: return "the signature does not match the file";
    case SigResult::BadComment: return "the signature's trusted comment was changed";
    case SigResult::WrongFile: return "the signature belongs to another file";
  }
  return "?";
}

SigResult verifyMinisign(const std::string& sigFile, const std::string& message, const std::vector<MinisignKey>& keys,
                         const std::string& expectedFile, SigInfo* info) {
  const std::vector<std::string> ls = lines(sigFile);
  if (ls.size() < 4 || ls[0].rfind("untrusted comment:", 0) != 0) return SigResult::Malformed;
  static const std::string kTrusted = "trusted comment: ";
  if (ls[2].rfind(kTrusted, 0) != 0) return SigResult::Malformed;
  std::vector<uint8_t> sig, global;
  if (!base64Decode(trimSpaces(ls[1]), sig) || sig.size() != 74) return SigResult::Malformed;
  if (!base64Decode(trimSpaces(ls[3]), global) || global.size() != 64) return SigResult::Malformed;
  const bool prehashed = sig[0] == 'E' && sig[1] == 'D';
  if (!prehashed && !(sig[0] == 'E' && sig[1] == 'd')) return SigResult::Malformed;
  const MinisignKey* key = nullptr;
  for (const MinisignKey& k : keys)
    if (std::memcmp(k.keyId.data(), sig.data() + 2, 8) == 0) key = &k;
  if (!key) return SigResult::WrongKey;
  const uint8_t* s = sig.data() + 10;
  bool ok;
  if (prehashed) {
    uint8_t h[64];
    crypto_blake2b(h, 64, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    ok = ed25519Verify(s, key->pub.data(), h, 64);
  } else {
    ok = ed25519Verify(s, key->pub.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size());
  }
  if (!ok) return SigResult::BadSignature;
  const std::string comment = ls[2].substr(kTrusted.size());
  std::vector<uint8_t> both(s, s + 64);
  both.insert(both.end(), comment.begin(), comment.end());
  if (!ed25519Verify(global.data(), key->pub.data(), both.data(), both.size())) return SigResult::BadComment;
  if (info) {
    info->trustedComment = comment;
    info->prehashed = prehashed;
  }
  if (!expectedFile.empty()) {
    // minisign writes "timestamp:<n>\tfile:<name>[\thashed]"; the name is the signed file's.
    bool named = false;
    size_t a = 0;
    while (a <= comment.size()) {
      size_t b = comment.find('\t', a);
      if (b == std::string::npos) b = comment.size();
      if (comment.compare(a, b - a, "file:" + expectedFile) == 0) named = true;
      a = b + 1;
    }
    if (!named) return SigResult::WrongFile;
  }
  return SigResult::Ok;
}

std::string signMinisign(const std::array<uint8_t, 32>& seed, const std::array<uint8_t, 8>& keyId, const std::string& message,
                         const std::string& trustedComment, bool prehashed) {
  std::array<uint8_t, 64> s{};
  if (prehashed) {
    uint8_t h[64];
    crypto_blake2b(h, 64, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    s = ed25519Sign(seed, h, 64);
  } else {
    s = ed25519Sign(seed, reinterpret_cast<const uint8_t*>(message.data()), message.size());
  }
  std::vector<uint8_t> sig = {'E', static_cast<uint8_t>(prehashed ? 'D' : 'd')};
  sig.insert(sig.end(), keyId.begin(), keyId.end());
  sig.insert(sig.end(), s.begin(), s.end());
  std::vector<uint8_t> both(s.begin(), s.end());
  both.insert(both.end(), trustedComment.begin(), trustedComment.end());
  const std::array<uint8_t, 64> g = ed25519Sign(seed, both.data(), both.size());
  return "untrusted comment: signature from minisign secret key\n" + base64Encode(sig) + "\ntrusted comment: " + trustedComment +
         "\n" + base64Encode(std::vector<uint8_t>(g.begin(), g.end())) + "\n";
}

std::vector<MinisignKey> releaseKeys() {
  std::vector<MinisignKey> out;
  for (const char* k : kReleaseKeys) {
    if (!k) break;
    MinisignKey m;
    if (parseMinisignKey(k, m)) out.push_back(m);
  }
  return out;
}

}  // namespace gl::update
