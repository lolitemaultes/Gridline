// Gridline - J-111 collaboration: profile. See Profile.h.
#include "Profile.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../core/Platform.h"
#include "../core/Png.h"
#include "Log.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace gl::collab {

std::string Profile::avatarHash() const {
  if (avatar.empty()) return avatarRef;
  const crypto::Key h = crypto::hash(avatar);
  return crypto::toHex(h.data(), 16);
}

std::string cleanName(const std::string& raw) {
  // drop control characters (ASCII and C1), collapse runs of spaces
  std::string s;
  for (size_t i = 0; i < raw.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c < 0x20 || c == 0x7F) {
      s += ' ';
      continue;
    }
    if (c == 0xC2 && i + 1 < raw.size()) {
      const unsigned char d = static_cast<unsigned char>(raw[i + 1]);
      if (d >= 0x80 && d <= 0x9F) {
        ++i;
        continue;
      }
    }
    s += static_cast<char>(c);
  }
  std::string out;
  bool space = true;
  for (char c : s) {
    if (c == ' ') {
      if (!space) out += ' ';
      space = true;
    } else {
      out += c;
      space = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  // at most kMaxNameChars code points
  size_t cps = 0, cut = out.size();
  for (size_t i = 0; i < out.size(); ++i) {
    if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80) {
      if (cps == kMaxNameChars) {
        cut = i;
        break;
      }
      ++cps;
    }
  }
  out.resize(cut);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool validEmail(const std::string& e) {
  if (e.empty()) return true;
  if (e.size() > 254) return false;
  const size_t at = e.find('@');
  if (at == std::string::npos || at == 0 || e.find('@', at + 1) != std::string::npos) return false;
  const std::string domain = e.substr(at + 1);
  const size_t dot = domain.find('.');
  if (dot == std::string::npos || dot == 0 || domain.back() == '.' || domain.find("..") != std::string::npos) return false;
  for (char c : e) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7F || c == ',' || c == ';' || c == '<' || c == '>' || c == '"' || c == '(' || c == ')') return false;
  }
  return true;
}

uint32_t defaultColor(const crypto::Key& signPublic) {
  const crypto::Key h = crypto::hash("gridline-colour" + crypto::keyString(signPublic));
  const double hue = (h[0] | (h[1] << 8)) / 65536.0 * 360.0;
  // HSV with s = 0.62, v = 0.92: vivid but readable on dark and light panels
  const double s = 0.62, v = 0.92, c = v * s;
  const double hp = hue / 60.0, x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1));
  double r = 0, g = 0, b = 0;
  if (hp < 1) r = c, g = x;
  else if (hp < 2) r = x, g = c;
  else if (hp < 3) g = c, b = x;
  else if (hp < 4) g = x, b = c;
  else if (hp < 5) r = x, b = c;
  else r = c, b = x;
  const double m = v - c;
  auto ch = [&](double q) { return static_cast<uint32_t>(std::lround((q + m) * 255.0)) & 0xFF; };
  return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

std::string initials(const std::string& name) {
  const std::string n = cleanName(name);
  std::string out;
  bool start = true;
  for (size_t i = 0; i < n.size() && out.size() < 8; ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    if (c == ' ') {
      start = true;
      continue;
    }
    if (start && c < 0x80 && !std::isalnum(c)) continue;  // "(studio B)": the B, not the bracket
    if (start) {
      // one whole UTF-8 character
      size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
      std::string chr = n.substr(i, len);
      if (len == 1) chr[0] = static_cast<char>(std::toupper(c));
      out += chr;
      if (out.size() >= 2 && (static_cast<unsigned char>(out[0]) < 0x80)) {
        // two letters are enough
      }
      start = false;
    }
  }
  // keep the first and last initial
  std::vector<std::string> parts;
  for (size_t i = 0; i < out.size();) {
    const unsigned char c = static_cast<unsigned char>(out[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    parts.push_back(out.substr(i, len));
    i += len;
  }
  if (parts.empty()) return "?";
  if (parts.size() == 1) return parts[0];
  return parts.front() + parts.back();
}

namespace {

void appendBytes(void* ctx, void* data, int size) {
  static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

}  // namespace

bool sanitizeAvatar(const std::string& bytes, std::string& jpegOut, std::string* err) {
  jpegOut.clear();
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (bytes.empty()) return fail("empty file");
  if (bytes.size() > kMaxAvatarInputBytes) return fail("the picture file is too large (20 MB at most)");
  int w = 0, h = 0, comp = 0;
  const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
  if (!stbi_info_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &comp)) return fail("not a PNG or JPEG picture");
  if (w <= 0 || h <= 0 || w > kMaxAvatarInputPixels || h > kMaxAvatarInputPixels) return fail("the picture is too large (12000 pixels a side at most)");
  stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &comp, 3);
  if (!px) return fail("the picture could not be read");
  // centre crop to a square
  const int side = std::min(w, h);
  const int x0 = (w - side) / 2, y0 = (h - side) / 2;
  png::Image sq;
  sq.width = side;
  sq.height = side;
  sq.channels = 3;
  sq.pixels.resize(static_cast<size_t>(side) * static_cast<size_t>(side) * 3);
  for (int y = 0; y < side; ++y)
    std::copy(px + (static_cast<size_t>(y0 + y) * static_cast<size_t>(w) + static_cast<size_t>(x0)) * 3,
              px + (static_cast<size_t>(y0 + y) * static_cast<size_t>(w) + static_cast<size_t>(x0 + side)) * 3,
              sq.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * static_cast<size_t>(side) * 3));
  stbi_image_free(px);
  for (int size : {kAvatarSize, 192, 128, 96}) {
    const png::Image img = side > size ? png::resize(sq, size, size) : sq;
    for (int q : {88, 78, 66, 52, 40}) {
      std::string out;
      if (!stbi_write_jpg_to_func(appendBytes, &out, img.width, img.height, 3, img.pixels.data(), q)) return fail("the picture could not be encoded");
      if (out.size() <= kMaxAvatarBytes) {
        jpegOut = std::move(out);
        return true;
      }
    }
  }
  return fail("the picture could not be made small enough");
}

bool decodeAvatar(const std::string& jpeg, int& w, int& h, std::vector<uint8_t>& rgba) {
  if (jpeg.empty() || jpeg.size() > kMaxAvatarBytes * 2) return false;
  int comp = 0;
  stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(jpeg.data()), static_cast<int>(jpeg.size()), &w, &h, &comp, 4);
  if (!px) return false;
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
    stbi_image_free(px);
    return false;
  }
  rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  stbi_image_free(px);
  return true;
}

Json profileWire(const Profile& p, bool withAvatarBytes) {
  Json j = Json::makeObject();
  j.object["name"] = Json::makeString(cleanName(p.name));
  j.object["color"] = Json::makeNumber(p.color & 0xFFFFFF);
  if (p.shareEmail && validEmail(p.email) && !p.email.empty()) j.object["email"] = Json::makeString(p.email);
  if (const std::string h = p.avatarHash(); !h.empty()) {
    j.object["avatar"] = Json::makeString(h);
    if (withAvatarBytes && !p.avatar.empty()) j.object["avatarData"] = Json::makeString(crypto::toB64url(p.avatar));
  }
  return j;
}

bool profileFromWire(const Json& j, Profile& out) {
  if (!j.is(Json::Type::Object)) return false;
  Profile p;
  p.name = cleanName(j.text("name"));
  p.color = static_cast<uint32_t>(std::clamp(j.num("color", 0), 0.0, 16777215.0));
  const std::string e = j.text("email");
  if (validEmail(e)) {
    p.email = e;
    p.shareEmail = !e.empty();
  }
  if (const Json* d = j.find("avatarData"); d && d->is(Json::Type::String)) {
    std::string bytes;
    if (crypto::fromB64url(d->str, bytes) && bytes.size() <= kMaxAvatarBytes) p.avatar = std::move(bytes);
  }
  const std::string keepAvatar = out.avatar;
  const std::string hash = j.text("avatar");
  if (p.avatar.empty() && !hash.empty() && out.avatarHash() == hash) p.avatar = keepAvatar;
  // J-111g: the announced hash stays with the profile until the bytes are here
  // (before, it was dropped, so nobody could look the picture up by it)
  if (p.avatar.empty() && hash.size() == 32 && hash.find_first_not_of("0123456789abcdef") == std::string::npos) p.avatarRef = hash;
  out = std::move(p);
  return true;
}

// ---------------------------------------------------------------- store ----
ProfileStore::ProfileStore(std::string dir) : dir_(std::move(dir)) {
  if (dir_.empty()) dir_ = (fs::path(platform::configRoot()) / "gridline" / "collab").string();
}

bool ProfileStore::load(std::string* err) {
  std::error_code ec;
  fs::create_directories(fs::path(dir_), ec);
  std::string text;
  Json j;
  const std::string idPath = (fs::path(dir_) / "identity.json").string();
  if (readFileText(idPath, text) && jsonParse(text, j) && crypto::Identity::fromJson(j, id_)) {
  } else {
    id_ = crypto::Identity::generate();
    if (!id_.valid || !platform::writePrivateFile(idPath, jsonStringify(id_.toJson(), 2))) {
      if (err) *err = "cannot create the identity in " + dir_;
      return false;
    }
  }
  profile_ = Profile();
  if (readFileText((fs::path(dir_) / "profile.json").string(), text) && jsonParse(text, j)) {
    profile_.name = cleanName(j.text("name"));
    const std::string e = j.text("email");
    if (validEmail(e)) profile_.email = e;
    profile_.shareEmail = j.num("shareEmail", 0) != 0;
    profile_.color = static_cast<uint32_t>(std::clamp(j.num("color", 0), 0.0, 16777215.0));
  }
  std::string av;
  if (readFileText((fs::path(dir_) / "avatar.jpg").string(), av, kMaxAvatarBytes) && !av.empty()) profile_.avatar = av;
  return true;
}

bool ProfileStore::save(std::string* err) const {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-collab-profile");
  j.object["name"] = Json::makeString(cleanName(profile_.name));
  if (validEmail(profile_.email)) j.object["email"] = Json::makeString(profile_.email);
  j.object["shareEmail"] = Json::makeNumber(profile_.shareEmail ? 1 : 0);
  j.object["color"] = Json::makeNumber(profile_.color);
  std::error_code ec;
  fs::create_directories(fs::path(dir_), ec);
  if (!platform::writePrivateFile((fs::path(dir_) / "profile.json").string(), jsonStringify(j, 2))) {
    if (err) *err = "cannot write the profile";
    return false;
  }
  const fs::path av = fs::path(dir_) / "avatar.jpg";
  if (profile_.avatar.empty())
    fs::remove(av, ec);
  else
    writeFileDurable(av.string(), profile_.avatar, err);
  return true;
}

bool ProfileStore::resetIdentity(std::string* err) {
  crypto::Identity n = crypto::Identity::generate();
  if (!n.valid || !platform::writePrivateFile((fs::path(dir_) / "identity.json").string(), jsonStringify(n.toJson(), 2))) {
    if (err) *err = "cannot create a new identity";
    return false;
  }
  id_ = n;
  return true;
}

uint32_t ProfileStore::color() const { return profile_.color ? profile_.color : defaultColor(id_.signPublic); }

void ProfileStore::remember(const std::string& fingerprint, const Profile& p) {
  if (fingerprint.empty() || fingerprint.find('/') != std::string::npos || fingerprint.find('.') != std::string::npos) return;
  Profile keep = p;
  keep.email.clear();  // other people's emails are shown live, never cached on disk
  keep.shareEmail = false;
  peers_[fingerprint] = keep;
  std::error_code ec;
  const fs::path dir = fs::path(dir_) / "peers";
  fs::create_directories(dir, ec);
  Json j = profileWire(keep, true);
  writeFileDurable((dir / (fingerprint + ".json")).string(), jsonCompact(j));
}

const Profile* ProfileStore::known(const std::string& fingerprint) const {
  auto it = peers_.find(fingerprint);
  if (it != peers_.end()) return &it->second;
  std::string text;
  Json j;
  if (fingerprint.empty() || fingerprint.find('/') != std::string::npos) return nullptr;
  if (!readFileText((fs::path(dir_) / "peers" / (fingerprint + ".json")).string(), text, 256 * 1024) || !jsonParse(text, j)) return nullptr;
  Profile p;
  if (!profileFromWire(j, p)) return nullptr;
  return &(peers_[fingerprint] = p);
}

}  // namespace gl::collab
