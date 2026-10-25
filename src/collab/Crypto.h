// Gridline - J-111 collaboration: cryptography (Monocypher wrappers).
// Identity keys (EdDSA signing + X25519 key exchange), hashing / key
// derivation (BLAKE2b), authenticated encryption (XChaCha20-Poly1305),
// randomness from the operating system, base64url for codes.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include "../core/Json.h"

namespace gl::collab::crypto {

using Key = std::array<uint8_t, 32>;

// Cryptographically secure random bytes from the system; false if unavailable.
bool randomBytes(void* out, size_t n);
Key randomKey();
uint64_t randomU64();

std::string toB64url(const std::string& bytes);
bool fromB64url(const std::string& text, std::string& out);
std::string toHex(const void* data, size_t n);
bool fromHex(const std::string& text, std::string& out);
std::string keyString(const Key& k);  // raw 32 bytes as std::string
bool keyFrom(const std::string& bytes, Key& out);

// BLAKE2b-256 of `data`; keyed with `key` when given.
Key hash(const std::string& data);
Key hashKeyed(const Key& key, const std::string& data);
// Key derivation: BLAKE2b keyed by `key` over label || 0 || data.
Key kdf(const Key& key, const std::string& label, const std::string& data = std::string());

// X25519 shared secret (hashed, so it is a uniform key).
Key dh(const Key& secret, const Key& pub);
Key dhPublic(const Key& secret);

// XChaCha20-Poly1305: returns ciphertext + 16-byte tag.
std::string seal(const Key& key, const std::string& nonce24, const std::string& ad, const std::string& plain);
bool open(const Key& key, const std::string& nonce24, const std::string& ad, const std::string& sealed, std::string& plain);
// A 24-byte nonce from a counter and a direction byte.
std::string counterNonce(uint64_t counter, uint8_t direction);

// A person's keys on this computer (<config>/collab/identity.json).
struct Identity {
  std::array<uint8_t, 64> signSecret{};
  Key signPublic{};
  Key dhSecret{};
  Key dhPublic{};
  bool valid = false;

  static Identity generate();
  uint64_t site() const;              // stable 64-bit id (never 0)
  std::string fingerprint() const;    // "ABCD-EF01-2345-6789-ABCD"
  Json toJson() const;                // includes the secrets: only for the identity file
  static bool fromJson(const Json& j, Identity& out);
};
std::string fingerprintOf(const Key& signPublic);
uint64_t siteOf(const Key& signPublic);

std::string sign(const Identity& id, const std::string& message);  // 64 bytes
bool verify(const Key& signPublic, const std::string& message, const std::string& signature);

// Constant-time comparison.
bool equal(const std::string& a, const std::string& b);

}  // namespace gl::collab::crypto
