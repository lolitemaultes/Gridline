// Gridline - J-111 collaboration: cryptography. See Crypto.h.
#include "Crypto.h"
#include <cctype>
#include <cstring>
#include <fstream>
#include "monocypher.h"
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#else
#include <stdlib.h>
#endif

namespace gl::collab::crypto {

bool randomBytes(void* out, size_t n) {
  if (n == 0) return true;
#if defined(_WIN32)
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
  auto* p = static_cast<uint8_t*>(out);
  size_t got = 0;
  while (got < n) {
    const ssize_t r = getrandom(p + got, n - got, 0);
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  if (got == n) return true;
  std::ifstream f("/dev/urandom", std::ios::binary);
  f.read(reinterpret_cast<char*>(p), static_cast<std::streamsize>(n));
  return static_cast<size_t>(f.gcount()) == n;
#else
  arc4random_buf(out, n);
  return true;
#endif
}

Key randomKey() {
  Key k{};
  randomBytes(k.data(), k.size());
  return k;
}

uint64_t randomU64() {
  uint64_t v = 0;
  randomBytes(&v, sizeof v);
  return v;
}

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}

std::string toB64url(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  const auto* s = reinterpret_cast<const unsigned char*>(in.data());
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t(s[i]) << 16) | (uint32_t(s[i + 1]) << 8) | s[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(s[i]) << 16;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t(s[i]) << 16) | (uint32_t(s[i + 1]) << 8);
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
  }
  return out;
}

bool fromB64url(const std::string& text, std::string& out) {
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = 26 + (c - 'a');
    else if (c >= '0' && c <= '9') v = 52 + (c - '0');
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

std::string toHex(const void* data, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

bool fromHex(const std::string& text, std::string& out) {
  if (text.size() % 2) return false;
  out.clear();
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
  };
  for (size_t i = 0; i < text.size(); i += 2) {
    const int a = val(text[i]), b = val(text[i + 1]);
    if (a < 0 || b < 0) return false;
    out.push_back(static_cast<char>(a * 16 + b));
  }
  return true;
}

std::string keyString(const Key& k) { return std::string(reinterpret_cast<const char*>(k.data()), k.size()); }

bool keyFrom(const std::string& bytes, Key& out) {
  if (bytes.size() != 32) return false;
  std::memcpy(out.data(), bytes.data(), 32);
  return true;
}

Key hash(const std::string& data) {
  Key k{};
  crypto_blake2b(k.data(), 32, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return k;
}

Key hashKeyed(const Key& key, const std::string& data) {
  Key k{};
  crypto_blake2b_keyed(k.data(), 32, key.data(), 32, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return k;
}

Key kdf(const Key& key, const std::string& label, const std::string& data) {
  std::string m = label;
  m.push_back('\0');
  m += data;
  return hashKeyed(key, m);
}

Key dh(const Key& secret, const Key& pub) {
  Key raw{};
  crypto_x25519(raw.data(), secret.data(), pub.data());
  Key k = hash(keyString(raw));
  crypto_wipe(raw.data(), raw.size());
  return k;
}

Key dhPublic(const Key& secret) {
  Key p{};
  crypto_x25519_public_key(p.data(), secret.data());
  return p;
}

std::string seal(const Key& key, const std::string& nonce24, const std::string& ad, const std::string& plain) {
  std::string out(plain.size() + 16, '\0');
  auto* o = reinterpret_cast<uint8_t*>(&out[0]);
  crypto_aead_lock(o, o + plain.size(), key.data(), reinterpret_cast<const uint8_t*>(nonce24.data()),
                   reinterpret_cast<const uint8_t*>(ad.data()), ad.size(), reinterpret_cast<const uint8_t*>(plain.data()),
                   plain.size());
  return out;
}

bool open(const Key& key, const std::string& nonce24, const std::string& ad, const std::string& sealed, std::string& plain) {
  if (sealed.size() < 16 || nonce24.size() != 24) return false;
  const size_t n = sealed.size() - 16;
  plain.assign(n, '\0');
  const auto* c = reinterpret_cast<const uint8_t*>(sealed.data());
  return crypto_aead_unlock(reinterpret_cast<uint8_t*>(n ? &plain[0] : nullptr), c + n, key.data(),
                            reinterpret_cast<const uint8_t*>(nonce24.data()), reinterpret_cast<const uint8_t*>(ad.data()),
                            ad.size(), c, n) == 0;
}

std::string counterNonce(uint64_t counter, uint8_t direction) {
  std::string n(24, '\0');
  for (int i = 0; i < 8; ++i) n[static_cast<size_t>(i)] = static_cast<char>((counter >> (8 * i)) & 0xFF);
  n[8] = static_cast<char>(direction);
  return n;
}

Identity Identity::generate() {
  Identity id;
  uint8_t seed[32];
  if (!randomBytes(seed, sizeof seed)) return id;
  crypto_eddsa_key_pair(id.signSecret.data(), id.signPublic.data(), seed);  // wipes the seed
  if (!randomBytes(id.dhSecret.data(), 32)) return id;
  id.dhPublic = crypto::dhPublic(id.dhSecret);
  id.valid = true;
  return id;
}

uint64_t siteOf(const Key& signPublic) {
  const Key h = hash("gridline-site" + keyString(signPublic));
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | h[static_cast<size_t>(i)];
  return v ? v : 1;
}

std::string fingerprintOf(const Key& signPublic) {
  const Key h = hash("gridline-fingerprint" + keyString(signPublic));
  const std::string hx = toHex(h.data(), 10);
  std::string out;
  for (size_t i = 0; i < hx.size(); ++i) {
    if (i && i % 4 == 0) out += '-';
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(hx[i])));
  }
  return out;
}

uint64_t Identity::site() const { return siteOf(signPublic); }
std::string Identity::fingerprint() const { return fingerprintOf(signPublic); }

Json Identity::toJson() const {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-identity");
  j.object["signSecret"] = Json::makeString(toHex(signSecret.data(), signSecret.size()));
  j.object["signPublic"] = Json::makeString(toHex(signPublic.data(), 32));
  j.object["dhSecret"] = Json::makeString(toHex(dhSecret.data(), 32));
  j.object["dhPublic"] = Json::makeString(toHex(dhPublic.data(), 32));
  return j;
}

bool Identity::fromJson(const Json& j, Identity& out) {
  std::string ss, sp, ds, dp;
  if (!fromHex(j.text("signSecret"), ss) || ss.size() != 64 || !fromHex(j.text("signPublic"), sp) || sp.size() != 32 ||
      !fromHex(j.text("dhSecret"), ds) || ds.size() != 32 || !fromHex(j.text("dhPublic"), dp) || dp.size() != 32)
    return false;
  std::memcpy(out.signSecret.data(), ss.data(), 64);
  std::memcpy(out.signPublic.data(), sp.data(), 32);
  std::memcpy(out.dhSecret.data(), ds.data(), 32);
  std::memcpy(out.dhPublic.data(), dp.data(), 32);
  if (crypto::dhPublic(out.dhSecret) != out.dhPublic) return false;
  out.valid = true;
  return true;
}

std::string sign(const Identity& id, const std::string& message) {
  std::string sig(64, '\0');
  crypto_eddsa_sign(reinterpret_cast<uint8_t*>(&sig[0]), id.signSecret.data(), reinterpret_cast<const uint8_t*>(message.data()),
                    message.size());
  return sig;
}

bool verify(const Key& signPublic, const std::string& message, const std::string& signature) {
  if (signature.size() != 64) return false;
  return crypto_eddsa_check(reinterpret_cast<const uint8_t*>(signature.data()), signPublic.data(),
                            reinterpret_cast<const uint8_t*>(message.data()), message.size()) == 0;
}

bool equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

}  // namespace gl::collab::crypto
