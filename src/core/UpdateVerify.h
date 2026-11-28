// Gridline - automatic updates: is a downloaded release really ours?
//
// SHA-512 (FIPS 180-4), Ed25519 (RFC 8032, on Monocypher's curve arithmetic) and minisign
// signatures (https://jedisct1.github.io/minisign/), both the original "Ed" format (the signature
// covers the file) and the prehashed "ED" format (it covers the file's BLAKE2b-512). A release
// signs its SHA256SUMS file; every other file is checked against that list (Update.h).
// Pure functions, no I/O. See docs/updates.md.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::update {

class Sha512 {
 public:
  Sha512();
  void add(const void* data, size_t n);
  std::array<uint8_t, 64> finish();  // call once
 private:
  void block(const uint8_t* p);
  uint64_t h_[8];
  uint8_t buf_[128];
  size_t used_ = 0;
  uint64_t total_ = 0;
};
std::array<uint8_t, 64> sha512(const void* data, size_t n);
std::string hexOf(const uint8_t* p, size_t n);

// Base64 (RFC 4648, with padding). Decoding ignores nothing: false on any stray character.
std::string base64Encode(const std::vector<uint8_t>& in);
bool base64Decode(const std::string& in, std::vector<uint8_t>& out);

// ---- Ed25519 ----
// `seed` 32 bytes -> public key 32 bytes.
std::array<uint8_t, 32> ed25519PublicKey(const std::array<uint8_t, 32>& seed);
std::array<uint8_t, 64> ed25519Sign(const std::array<uint8_t, 32>& seed, const uint8_t* msg, size_t n);
bool ed25519Verify(const uint8_t sig[64], const uint8_t pub[32], const uint8_t* msg, size_t n);

// ---- minisign ----
struct MinisignKey {
  std::array<uint8_t, 8> keyId{};
  std::array<uint8_t, 32> pub{};
};
// A public key: the base64 line ("RWQ...") or the whole .pub file (comment line + base64 line).
bool parseMinisignKey(const std::string& text, MinisignKey& out);
std::string minisignKeyText(const MinisignKey& k);  // the base64 line

enum class SigResult {
  Ok,
  Malformed,      // not a minisign signature file
  WrongKey,       // signed with a key we do not trust (key id)
  BadSignature,   // the file was changed, or the signature was
  BadComment,     // the trusted comment was changed
  WrongFile,      // the trusted comment names another file (an old signature replayed)
};
const char* describe(SigResult r);  // English, for the log
struct SigInfo {
  std::string trustedComment;  // "timestamp:1764300000\tfile:Gridline-0.7.9-SHA256SUMS\thashed"
  bool prehashed = false;      // "ED"
};
// Checks `sigFile` (the .minisig text) for `message` against each trusted key. `expectedFile`
// (non-empty): the trusted comment must name exactly this file ("file:<name>").
SigResult verifyMinisign(const std::string& sigFile, const std::string& message, const std::vector<MinisignKey>& keys,
                         const std::string& expectedFile, SigInfo* info = nullptr);
// Makes a .minisig (tests, and `gridline-app --update-sign` for a local check). `prehashed`: "ED".
std::string signMinisign(const std::array<uint8_t, 32>& seed, const std::array<uint8_t, 8>& keyId, const std::string& message,
                         const std::string& trustedComment, bool prehashed);

// The release keys compiled into this build (core/UpdateKeys.h; empty until the project has one).
std::vector<MinisignKey> releaseKeys();

}  // namespace gl::update
