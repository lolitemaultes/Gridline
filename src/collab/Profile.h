// Gridline - J-111 collaboration: the user's collaboration profile (app level,
// per user, never stored in a project): display name, optional contact email
// (shared only when the user allows it, only inside the encrypted channel),
// profile picture (sanitised: square, 256 px, re-encoded JPEG without
// metadata, at most 64 KB), collaboration colour; plus the identity keys.
// Files: <config>/collab/profile.json, avatar.jpg, identity.json, peers/.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Crypto.h"

namespace gl::collab {

inline constexpr size_t kMaxNameChars = 40;
inline constexpr size_t kMaxAvatarBytes = 64 * 1024;
inline constexpr int kAvatarSize = 256;
inline constexpr size_t kMaxAvatarInputBytes = 20u << 20;  // files larger than this are refused
inline constexpr int kMaxAvatarInputPixels = 12000;        // per side

struct Profile {
  std::string name;
  std::string email;
  bool shareEmail = false;
  uint32_t color = 0;        // 0xRRGGBB; 0 = derived from the identity
  std::string avatar;        // JPEG bytes ("" = initials)
  // J-111g: the hash a collaborator announced while the bytes are not here yet
  // (profile messages carry the hash; the bytes come separately, see Session).
  std::string avatarRef;
  // BLAKE2b hex of the bytes; without bytes the announced hash ("" when none).
  std::string avatarHash() const;
};

// Display name: trimmed, control characters removed, at most kMaxNameChars
// characters (UTF-8 aware). Returns "" when nothing is left.
std::string cleanName(const std::string& raw);
// Plain address check: one '@', a dot in the domain, no spaces / control
// characters, at most 254 bytes. Empty is valid (no email).
bool validEmail(const std::string& email);
// The colour a person gets by default (from their signing key): a vivid hue.
uint32_t defaultColor(const crypto::Key& signPublic);
// Initials for the fallback avatar ("Sam Lee" -> "SL", "alex" -> "A").
std::string initials(const std::string& name);

// Decodes PNG / JPEG `bytes`, centre-crops to a square, scales to kAvatarSize
// and re-encodes as JPEG (lowering the quality, then the size, until it fits
// kMaxAvatarBytes). Refuses malformed, too large or non-image input.
bool sanitizeAvatar(const std::string& bytes, std::string& jpegOut, std::string* err = nullptr);
// RGBA pixels of an avatar for display (false when it does not decode).
bool decodeAvatar(const std::string& jpeg, int& w, int& h, std::vector<uint8_t>& rgba);

// What travels to collaborators (handshake / profile message): never the email
// unless shareEmail; the avatar by hash (fetched separately).
Json profileWire(const Profile& p, bool withAvatarBytes = false);
bool profileFromWire(const Json& j, Profile& out);

// <config>/collab: identity + profile + cache of other people's profiles.
class ProfileStore {
 public:
  explicit ProfileStore(std::string dir = std::string());
  const std::string& dir() const { return dir_; }
  // Loads (creating the identity on first use).
  bool load(std::string* err = nullptr);
  bool save(std::string* err = nullptr) const;
  const crypto::Identity& identity() const { return id_; }
  bool resetIdentity(std::string* err = nullptr);
  Profile& profile() { return profile_; }
  const Profile& profile() const { return profile_; }
  uint32_t color() const;  // profile colour or the default one
  bool ready() const { return !cleanName(profile_.name).empty(); }
  // Other people, by signing-key fingerprint.
  void remember(const std::string& fingerprint, const Profile& p);
  const Profile* known(const std::string& fingerprint) const;

 private:
  std::string dir_;
  crypto::Identity id_;
  Profile profile_;
  mutable std::map<std::string, Profile> peers_;
};

}  // namespace gl::collab
