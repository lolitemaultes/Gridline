// Gridline - J-89: update check logic (see Update.h).
#include "Update.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include "Json.h"
#include "Settings.h"

namespace gl::update {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ versions

namespace {

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

int toInt(const std::string& s) {
  long v = 0;
  for (char c : s) {
    v = v * 10 + (c - '0');
    if (v > 1000000000L) return 1000000000;
  }
  return static_cast<int>(v);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

Version parseVersion(const std::string& text) {
  Version v;
  std::string s = trim(text);
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
  if (const size_t plus = s.find('+'); plus != std::string::npos) s.erase(plus);
  if (const size_t dash = s.find('-'); dash != std::string::npos) {
    v.pre = s.substr(dash + 1);
    s.erase(dash);
    if (v.pre.empty()) return v;
    for (const auto& id : split(v.pre, '.'))
      if (id.empty()) return v;
  }
  const auto parts = split(s, '.');
  if (parts.empty() || parts.size() > 3) return v;
  for (const auto& p : parts)
    if (!allDigits(p)) return v;
  v.major = toInt(parts[0]);
  v.minor = parts.size() > 1 ? toInt(parts[1]) : 0;
  v.patch = parts.size() > 2 ? toInt(parts[2]) : 0;
  v.valid = true;
  return v;
}

int compare(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
  const auto x = split(a.pre, '.'), y = split(b.pre, '.');
  for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
    const bool nx = allDigits(x[i]), ny = allDigits(y[i]);
    if (nx && ny) {
      const int p = toInt(x[i]), q = toInt(y[i]);
      if (p != q) return p < q ? -1 : 1;
    } else if (nx != ny) {
      return nx ? -1 : 1;  // numeric identifiers sort first
    } else if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }
  if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
  return 0;
}

std::string toString(const Version& v) {
  std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
  if (!v.pre.empty()) s += "-" + v.pre;
  return s;
}

// ------------------------------------------------------------------ releases

bool parseRelease(const Json& j, Release& out) {
  if (!j.is(Json::Type::Object) || !j.find("tag_name")) return false;
  out = Release{};
  out.tag = j.text("tag_name");
  out.name = j.text("name");
  out.htmlUrl = j.text("html_url");
  out.body = j.text("body");
  out.published = j.text("published_at");
  if (const Json* d = j.find("draft"); d && d->is(Json::Type::Bool)) out.draft = d->boolean;
  if (const Json* p = j.find("prerelease"); p && p->is(Json::Type::Bool)) out.prerelease = p->boolean;
  if (const Json* as = j.find("assets"); as && as->is(Json::Type::Array))
    for (const Json& a : as->array) {
      if (!a.is(Json::Type::Object)) continue;
      Asset x;
      x.name = a.text("name");
      x.url = a.text("browser_download_url");
      x.size = jsonInt<int64_t>(a.num("size"));
      if (!x.name.empty() && !x.url.empty()) out.assets.push_back(std::move(x));
    }
  out.version = parseVersion(out.tag);
  return true;
}

bool parseReleases(const std::string& json, std::vector<Release>& out, std::string* err) {
  out.clear();
  Json j;
  if (!jsonParse(json, j)) {
    if (err) *err = "the answer is not valid JSON";
    return false;
  }
  Release r;
  if (j.is(Json::Type::Array)) {
    for (const Json& e : j.array)
      if (parseRelease(e, r)) out.push_back(r);
    return true;
  }
  if (parseRelease(j, r)) {
    out.push_back(r);
    return true;
  }
  if (err) *err = j.is(Json::Type::Object) && !j.text("message").empty() ? "GitHub: " + j.text("message") : "the answer is not a release";
  return false;
}

const Release* pickRelease(const std::vector<Release>& releases, bool beta) {
  const Release* best = nullptr;
  for (const Release& r : releases) {
    if (r.draft || !r.version.valid) continue;
    if (!beta && (r.prerelease || !r.version.pre.empty())) continue;
    if (!best || compare(r.version, best->version) > 0) best = &r;
  }
  return best;
}

// ------------------------------------------------------------------ downloads

Target currentTarget() {
  Target t;
#if defined(__APPLE__)
  t.os = "macos";
#elif defined(_WIN32)
  t.os = "windows";
#elif defined(__linux__)
  t.os = "linux";
#elif defined(__FreeBSD__)
  t.os = "freebsd";
#elif defined(__OpenBSD__)
  t.os = "openbsd";
#elif defined(__NetBSD__)
  t.os = "netbsd";
#else
  t.os = "unknown";
#endif
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
  t.arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  t.arch = "aarch64";
#else
  t.arch = "unknown";
#endif
#if defined(__linux__)
  std::ifstream f("/etc/os-release", std::ios::binary);
  if (!f) f.open("/usr/lib/os-release", std::ios::binary);
  if (f) t.package = linuxPackage(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
#endif
  return t;
}

std::string linuxPackage(const std::string& osRelease) {
  std::vector<std::string> ids;  // ID first, then ID_LIKE's words
  size_t a = 0;
  while (a < osRelease.size()) {
    size_t b = osRelease.find('\n', a);
    if (b == std::string::npos) b = osRelease.size();
    std::string line = osRelease.substr(a, b - a);
    a = b + 1;
    const bool id = line.rfind("ID=", 0) == 0, like = line.rfind("ID_LIKE=", 0) == 0;
    if (!id && !like) continue;
    std::string v = lower(line.substr(line.find('=') + 1));
    for (char& c : v)
      if (c == '"' || c == '\'' || c == '\r' || c == '\t') c = ' ';
    size_t p = 0;
    while (p < v.size()) {
      while (p < v.size() && v[p] == ' ') ++p;
      size_t q = v.find(' ', p);
      if (q == std::string::npos) q = v.size();
      if (q > p) ids.insert(id ? ids.begin() : ids.end(), v.substr(p, q - p));
      p = q;
    }
  }
  static const char* const kDeb[] = {"debian", "ubuntu", "linuxmint", "pop", "elementary", "zorin", "kali", "raspbian", "neon", "deepin", "mx"};
  static const char* const kRpm[] = {"fedora", "rhel", "centos", "rocky", "almalinux", "ol", "nobara", "ultramarine",
                                     "suse", "opensuse", "opensuse-leap", "opensuse-tumbleweed", "sles", "mageia", "openmandriva"};
  for (const std::string& i : ids) {
    for (const char* d : kDeb)
      if (i == d) return "deb";
    for (const char* d : kRpm)
      if (i == d) return "rpm";
  }
  return "";
}

namespace {
// The asset named "Gridline-<version><suffix>" (any spelling of the version as a fallback).
const Asset* assetWithSuffix(const Release& r, std::string suffix) {
  suffix = lower(suffix);
  const std::string v = r.version.valid ? toString(r.version) : std::string();
  const Asset* loose = nullptr;
  for (const Asset& a : r.assets) {
    const std::string n = lower(a.name);
    if (n.rfind("gridline-", 0) != 0 || !endsWith(n, suffix)) continue;
    if (!v.empty() && n == "gridline-" + lower(v) + suffix) return &a;
    if (!loose) loose = &a;  // version spelled differently in the file name
  }
  return loose;
}
}  // namespace

const Asset* pickAsset(const Release& r, const Target& t) {
  if (t.os == "macos") return assetWithSuffix(r, "-macos-universal.dmg");
  if (t.os == "windows" && (t.arch == "x86_64" || t.arch == "aarch64"))
    return assetWithSuffix(r, std::string("-windows-") + (t.arch == "aarch64" ? "arm64" : "x64") + "-setup.exe");
  if (t.os == "linux" && (t.arch == "x86_64" || t.arch == "aarch64")) {
    // The distribution's own package where we ship one for it; the AppImage everywhere else.
    const Asset* pkg = nullptr;
    if (t.package == "deb") pkg = assetWithSuffix(r, std::string("-linux-") + (t.arch == "aarch64" ? "arm64" : "amd64") + ".deb");
    if (t.package == "rpm") pkg = assetWithSuffix(r, "-linux-" + t.arch + ".rpm");
    return pkg ? pkg : assetWithSuffix(r, "-linux-" + t.arch + ".appimage");
  }
  if ((t.os == "freebsd" || t.os == "openbsd" || t.os == "netbsd") && t.arch != "unknown") return assetWithSuffix(r, "-" + t.os + "-" + t.arch + ".tar.gz");
  return nullptr;
}

const Asset* checksumAsset(const Release& r) {
  for (const Asset& a : r.assets)
    if (endsWith(lower(a.name), "sha256sums") || endsWith(lower(a.name), "sha256sums.txt")) return &a;
  return nullptr;
}

std::string checksumFor(const std::string& sums, const std::string& fileName) {
  size_t a = 0;
  while (a < sums.size()) {
    size_t b = sums.find('\n', a);
    if (b == std::string::npos) b = sums.size();
    std::string line = trim(sums.substr(a, b - a));
    a = b + 1;
    if (line.size() < 66) continue;
    std::string hex = lower(line.substr(0, 64));
    if (hex.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
    if (!std::isspace(static_cast<unsigned char>(line[64]))) continue;
    std::string name = trim(line.substr(65));
    if (!name.empty() && name[0] == '*') name.erase(0, 1);  // binary mode marker
    if (name == fileName || fs::u8path(name).filename().u8string() == fileName) return hex;
  }
  return {};
}

// ------------------------------------------------------------------ SHA-256

namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, buf_{} {}

void Sha256::block(const unsigned char* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::add(const void* data, size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  total_ += n;
  while (n > 0) {
    const size_t take = std::min(n, sizeof buf_ - used_);
    std::memcpy(buf_ + used_, p, take);
    used_ += take, p += take, n -= take;
    if (used_ == sizeof buf_) {
      block(buf_);
      used_ = 0;
    }
  }
}

std::string Sha256::hex() {
  const uint64_t bits = total_ * 8;
  const unsigned char one = 0x80, zero = 0;
  add(&one, 1);
  while (used_ != 56) add(&zero, 1);
  unsigned char len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
  add(len, 8);
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint32_t v : h_)
    for (int s = 28; s >= 0; s -= 4) out += digits[(v >> s) & 15u];
  return out;
}

std::string sha256Hex(const std::string& data) {
  Sha256 s;
  s.add(data.data(), data.size());
  return s.hex();
}

std::string sha256File(const std::string& path) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) return {};
  Sha256 s;
  std::vector<char> buf(1 << 16);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (in.gcount() > 0) s.add(buf.data(), static_cast<size_t>(in.gcount()));
  }
  if (in.bad()) return {};
  return s.hex();
}

// ------------------------------------------------------------------ release notes

namespace {

void eraseAll(std::string& s, const std::string& what) {
  for (size_t p; (p = s.find(what)) != std::string::npos;) s.erase(p, what.size());
}

void replaceAll(std::string& s, const std::string& what, const std::string& with) {
  for (size_t p = 0; (p = s.find(what, p)) != std::string::npos; p += with.size()) s.replace(p, what.size(), with);
}

// Inline Markdown of one line: images out, links as their text, tags and markers out.
std::string inlineText(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size();) {
    const char c = in[i];
    if ((c == '!' && i + 1 < in.size() && in[i + 1] == '[') || c == '[') {
      const bool image = c == '!';
      const size_t open = image ? i + 1 : i;
      const size_t close = in.find(']', open);
      if (close != std::string::npos && close + 1 < in.size() && in[close + 1] == '(') {
        const size_t end = in.find(')', close);
        if (end != std::string::npos) {
          if (!image) out += inlineText(in.substr(open + 1, close - open - 1));
          i = end + 1;
          continue;
        }
      }
    }
    if (c == '<' && i + 1 < in.size() && (std::isalpha(static_cast<unsigned char>(in[i + 1])) || in[i + 1] == '/')) {
      const size_t end = in.find('>', i);
      if (end != std::string::npos) {  // an HTML tag (<img>, <br>, <details>): dropped
        i = end + 1;
        continue;
      }
    }
    if (c == '`') {
      ++i;
      continue;
    }
    if ((c == '*' || c == '~') && i + 1 < in.size() && in[i + 1] == c) {
      i += 2;
      continue;
    }
    if (c == '_' && i + 1 < in.size() && in[i + 1] == '_') {
      i += 2;
      continue;
    }
    if (c == '*') {  // single emphasis marker
      ++i;
      continue;
    }
    out += c;
    ++i;
  }
  replaceAll(out, "&amp;", "&");
  replaceAll(out, "&lt;", "<");
  replaceAll(out, "&gt;", ">");
  replaceAll(out, "&quot;", "\"");
  replaceAll(out, "&#39;", "'");
  replaceAll(out, "&nbsp;", " ");
  return out;
}

}  // namespace

std::string notesExcerpt(const std::string& markdown, size_t maxChars) {
  std::string md = markdown;
  eraseAll(md, "\r");
  for (size_t a; (a = md.find("<!--")) != std::string::npos;) {  // comments may span lines
    const size_t b = md.find("-->", a);
    md.erase(a, b == std::string::npos ? std::string::npos : b + 3 - a);
  }
  std::vector<std::string> lines;
  bool fence = false;
  for (const std::string& raw : split(md, '\n')) {
    std::string t = trim(raw);
    if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) {
      fence = !fence;
      continue;
    }
    if (fence) {
      lines.push_back("  " + raw);
      continue;
    }
    if (t == "---" || t == "***" || t == "___" || (t.size() >= 3 && t.find_first_not_of("-=") == std::string::npos)) {
      lines.emplace_back();
      continue;
    }
    size_t indent = raw.find_first_not_of(" \t");
    if (indent == std::string::npos) indent = 0;
    std::string lead(std::min<size_t>(indent / 2, 3) * 2, ' ');
    while (!t.empty() && t[0] == '>') t = trim(t.substr(1));  // quote
    if (!t.empty() && t[0] == '#') {                       // heading
      const size_t text = t.find_first_not_of('#');
      t = text == std::string::npos ? std::string() : trim(t.substr(text));
      lead.clear();
    } else if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
      t = "\xE2\x80\xA2 " + trim(t.substr(2));  // bullet
      if (t.rfind("\xE2\x80\xA2 [ ] ", 0) == 0 || t.rfind("\xE2\x80\xA2 [x] ", 0) == 0) t = "\xE2\x80\xA2 " + t.substr(8);
    }
    t = inlineText(t);
    // An image-only line leaves an empty bullet / nothing.
    if (trim(t).empty() || trim(t) == "\xE2\x80\xA2") {
      lines.emplace_back();
      continue;
    }
    lines.push_back(lead + t);
  }
  std::string out;
  bool blank = true;  // no blank lines at the start, at most one in a row
  for (const std::string& l : lines) {
    if (trim(l).empty()) {
      if (!blank) out += "\n";
      blank = true;
      continue;
    }
    out += l + "\n";
    blank = false;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  if (out.size() > maxChars) {
    size_t cut = out.rfind('\n', maxChars);
    if (cut == std::string::npos || cut < maxChars / 2) {
      cut = maxChars;
      while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;  // UTF-8 boundary
    }
    out.erase(cut);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    out += "\n\xE2\x80\xA6";
  }
  return out;
}

// ------------------------------------------------------------------ the check

std::string userAgent() { return std::string("Gridline/") + GRIDLINE_VERSION; }

CheckResult check(const Fetcher& fetch, const std::string& currentVersion, bool beta) {
  CheckResult r;
  r.current = currentVersion;
  const Version cur = parseVersion(currentVersion);
  if (!cur.valid) {
    r.detail = "the running version (" + currentVersion + ") is not a version number";
    return r;
  }
  HttpResult res;
  try {
    res = fetch(beta ? kListUrl : kLatestUrl);
  } catch (...) {
    r.detail = "the request failed";
    return r;
  }
  if (res.offline || (res.status == 0 && !res.error.empty())) {
    r.status = res.offline ? Status::Offline : Status::Failed;
    r.detail = res.error.empty() ? "no connection" : res.error;
    return r;
  }
  if (res.status == 403 || res.status == 429) {
    r.status = Status::RateLimited;
    r.detail = "HTTP " + std::to_string(res.status);
    return r;
  }
  if (res.status == 404) {  // /releases/latest of a repository without releases
    r.status = Status::NoRelease;
    return r;
  }
  if (res.status != 200) {
    r.detail = res.status ? "HTTP " + std::to_string(res.status) : "no answer";
    return r;
  }
  std::vector<Release> all;
  std::string err;
  if (!parseReleases(res.body, all, &err)) {
    r.detail = err;
    return r;
  }
  const Release* best = pickRelease(all, beta);
  if (!best) {
    r.status = Status::NoRelease;
    return r;
  }
  if (compare(best->version, cur) > 0) {
    r.status = Status::Available;
    r.release = *best;
    if (r.release.htmlUrl.rfind("https://", 0) != 0) r.release.htmlUrl = std::string(kReleasesPage) + "/tag/" + best->tag;
  } else {
    r.status = Status::UpToDate;
  }
  return r;
}

std::string describe(const CheckResult& r) {
  switch (r.status) {
    case Status::UpToDate: return "You're up to date (" + r.current + ")";
    case Status::Available:
      return "Gridline " + toString(r.release.version) + " is available (you have " + r.current + "): " + r.release.htmlUrl;
    case Status::NoRelease: return "No release found on GitHub; you're up to date (" + r.current + ")";
    case Status::Offline: return "Could not reach GitHub (" + r.detail + ")";
    case Status::RateLimited: return "GitHub is limiting requests right now (" + r.detail + "); try again later";
    case Status::Disabled: return "Update checks are turned off (GRIDLINE_NO_UPDATE_CHECK, CI or --no-update-check)";
    case Status::Failed: break;
  }
  return "Update check failed: " + (r.detail.empty() ? std::string("unknown error") : r.detail);
}

// ------------------------------------------------------------------ schedule

Prefs loadPrefs(const Settings& s) {
  Prefs p;
  p.autoCheck = s.getBool("update.auto");
  p.beta = s.getBool("update.beta");
  p.lastCheck = static_cast<int64_t>(s.getNumber("update.lastCheck"));
  p.skipVersion = s.getString("update.skipVersion");
  p.remindAfter = static_cast<int64_t>(s.getNumber("update.remindAfter"));
  return p;
}

void savePrefs(Settings& s, const Prefs& p) {
  s.set("update.auto", p.autoCheck);
  s.set("update.beta", p.beta);
  s.set("update.lastCheck", static_cast<double>(p.lastCheck));
  s.set("update.skipVersion", p.skipVersion);
  s.set("update.remindAfter", static_cast<double>(p.remindAfter));
}

bool disabledByEnvironment() {
  auto on = [](const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
  };
  return std::getenv("GRIDLINE_NO_UPDATE_CHECK") != nullptr || on("CI");
}

bool dueAtStartup(const Prefs& p, int64_t now, bool disabled) {
  if (disabled || !p.autoCheck) return false;
  if (p.lastCheck <= 0 || now < p.lastCheck) return true;  // never, or the clock was set back
  return now - p.lastCheck >= kCheckInterval;
}

bool shouldOffer(const Prefs& p, const Release& r, int64_t now) {
  if (!r.version.valid) return false;
  if (!p.skipVersion.empty() && parseVersion(p.skipVersion).valid && compare(parseVersion(p.skipVersion), r.version) == 0) return false;
  return now >= p.remindAfter;
}

void remindLater(Prefs& p, int64_t now) { p.remindAfter = now + kRemindDelay; }

void skipVersion(Prefs& p, const Release& r) {
  p.skipVersion = toString(r.version);
  p.remindAfter = 0;
}

// ------------------------------------------------------------------ installer download

namespace {
fs::path freeName(const fs::path& folder, const std::string& name) {
  const fs::path base = fs::u8path(name);
  std::string stem = base.stem().u8string(), ext = base.extension().u8string();
  if (lower(name).size() > 7 && endsWith(lower(name), ".tar.gz")) stem = name.substr(0, name.size() - 7), ext = name.substr(name.size() - 7);
  fs::path p = folder / base;
  std::error_code ec;
  for (int k = 1; fs::exists(p, ec) && k < 1000; ++k) p = folder / fs::u8path(stem + " (" + std::to_string(k) + ")" + ext);
  return p;
}
}  // namespace

InstallerResult downloadInstaller(const Fetcher& fetch, const Downloader& download, const Release& r, const Asset& asset,
                                  const std::string& folder, const std::function<void(int64_t, int64_t)>& progress,
                                  const std::atomic<bool>* cancel) {
  InstallerResult out;
  const Asset* sums = checksumAsset(r);
  if (!sums) {
    out.error = "the release has no SHA256SUMS file";
    return out;
  }
  const HttpResult sr = fetch(sums->url);
  if (sr.status != 200) {
    out.error = sr.offline ? "could not reach GitHub" : "could not read " + sums->name + (sr.status ? " (HTTP " + std::to_string(sr.status) + ")" : "");
    return out;
  }
  const std::string want = checksumFor(sr.body, asset.name);
  if (want.empty()) {
    out.error = asset.name + " is not listed in " + sums->name;
    return out;
  }
  std::error_code ec;
  const fs::path dir = fs::u8path(folder);
  fs::create_directories(dir, ec);
  // Downloaded before (same file, intact): nothing to fetch.
  if (const fs::path same = dir / fs::u8path(asset.name); fs::is_regular_file(same, ec) && sha256File(same.u8string()) == want) {
    out.ok = true;
    out.path = same.u8string();
    return out;
  }
  const fs::path dest = freeName(dir, asset.name);
  const fs::path part = fs::u8path(dest.u8string() + ".part");
  std::string err;
  if (!download(asset.url, part.u8string(), progress, cancel, &err)) {
    fs::remove(part, ec);
    out.error = cancel && cancel->load() ? "cancelled" : err.empty() ? "the download failed" : err;
    return out;
  }
  const std::string got = sha256File(part.u8string());
  if (got != want) {
    fs::remove(part, ec);
    out.error = "the downloaded file does not match its SHA-256 checksum";
    return out;
  }
  fs::rename(part, dest, ec);
  if (ec) {
    fs::remove(part, ec);
    out.error = "could not save " + dest.u8string();
    return out;
  }
  if (endsWith(lower(asset.name), ".appimage"))  // ready to run from the file manager
    fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
  out.ok = true;
  out.path = dest.u8string();
  return out;
}

// ------------------------------------------------------------------ fakes (--fake-update, tests)

std::string fakeAssetBytes(const std::string& name) {
  std::string s;
  s.reserve(48 * 1024);
  for (int i = 0; s.size() < 48 * 1024; ++i) s += "Gridline test download " + name + " block " + std::to_string(i) + "\n";
  return s;
}

Release fakeRelease(const std::string& version) {
  Release r;
  r.version = parseVersion(version);
  const std::string v = r.version.valid ? toString(r.version) : version;
  r.tag = "v" + v;
  r.name = "Gridline " + v;
  r.htmlUrl = std::string(kReleasesPage) + "/tag/" + r.tag;
  r.published = "2026-11-02T10:00:00Z";
  r.prerelease = !r.version.pre.empty();
  r.body =
      "![Gridline " + v + "](https://github.com/lolitemaultes/Gridline/raw/main/docs/images/hero.png)\n\n"
      "## What's new\r\n\n"
      "- **Faster project loading**: large projects open in about half the time.\n"
      "- The mixer keeps its scroll position per project.\n"
      "- Release notes are shown when an update is offered ([details](https://github.com/lolitemaultes/Gridline/blob/main/docs/updates.md)).\n\n"
      "## Fixes\n\n"
      "* Piano roll: ghost notes no longer flicker while scrolling.\n"
      "* `gridline render` keeps the project's tempo automation.\n"
      "* Windows: the installer keeps the chosen plugin folder.\n\n"
      "<!-- assets are attached below -->\n"
      "<img src=\"https://example.invalid/badge.svg\" alt=\"build\">\n";
  const char* suffixes[] = {"-macos-universal.dmg", "-windows-x64-setup.exe", "-windows-arm64-setup.exe", "-linux-x86_64.AppImage",
                            "-linux-aarch64.AppImage", "-linux-amd64.deb", "-linux-arm64.deb", "-linux-x86_64.rpm", "-linux-aarch64.rpm", "-linux-x86_64.tar.gz", "-freebsd-x86_64.tar.gz", "-openbsd-x86_64.tar.gz",
                            "-netbsd-x86_64.tar.gz"};
  for (const char* s : suffixes) {
    Asset a;
    a.name = "Gridline-" + v + s;
    a.url = "fake://" + a.name;
    a.size = static_cast<int64_t>(fakeAssetBytes(a.name).size());
    r.assets.push_back(a);
  }
  Asset sums;
  sums.name = "Gridline-" + v + "-SHA256SUMS";
  sums.url = "fake://" + sums.name;
  r.assets.push_back(sums);
  r.assets.back().size = static_cast<int64_t>(fakeSums(r).size());
  return r;
}

std::string fakeSums(const Release& r) {
  std::string s;
  for (const Asset& a : r.assets)
    if (!endsWith(lower(a.name), "sha256sums")) s += sha256Hex(fakeAssetBytes(a.name)) + "  " + a.name + "\n";
  return s;
}

std::string releaseJson(const Release& r) {
  Json j = Json::makeObject();
  j.object["tag_name"] = Json::makeString(r.tag);
  j.object["name"] = Json::makeString(r.name);
  j.object["html_url"] = Json::makeString(r.htmlUrl);
  j.object["body"] = Json::makeString(r.body);
  j.object["published_at"] = Json::makeString(r.published);
  Json d;
  d.type = Json::Type::Bool;
  d.boolean = r.draft;
  j.object["draft"] = d;
  d.boolean = r.prerelease;
  j.object["prerelease"] = d;
  Json as = Json::makeArray();
  for (const Asset& a : r.assets) {
    Json o = Json::makeObject();
    o.object["name"] = Json::makeString(a.name);
    o.object["browser_download_url"] = Json::makeString(a.url);
    o.object["size"] = Json::makeNumber(static_cast<double>(a.size));
    as.array.push_back(std::move(o));
  }
  j.object["assets"] = std::move(as);
  return jsonStringify(j);
}

Fetcher fakeFetcher(const Release& r) {
  return [r](const std::string& url) {
    HttpResult h;
    if (url == kLatestUrl) {
      h.status = 200;
      h.body = releaseJson(r);
    } else if (url == kListUrl) {
      h.status = 200;
      h.body = "[" + releaseJson(r) + "]";
    } else if (const Asset* s = checksumAsset(r); s && url == s->url) {
      h.status = 200;
      h.body = fakeSums(r);
    } else {
      h.status = 404;
    }
    return h;
  };
}

Downloader fakeDownloader() {
  return [](const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
            const std::atomic<bool>* cancel, std::string* err) {
    if (url.rfind("fake://", 0) != 0) {
      if (err) *err = "not a fake URL";
      return false;
    }
    const std::string bytes = fakeAssetBytes(url.substr(7));
    std::ofstream out(fs::u8path(path), std::ios::binary);
    const size_t step = bytes.size() / 8 + 1;
    for (size_t at = 0; at < bytes.size(); at += step) {
      if (cancel && cancel->load()) return false;
      out.write(bytes.data() + at, static_cast<std::streamsize>(std::min(step, bytes.size() - at)));
      if (progress) progress(static_cast<int64_t>(std::min(at + step, bytes.size())), static_cast<int64_t>(bytes.size()));
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return static_cast<bool>(out);
  };
}

}  // namespace gl::update
