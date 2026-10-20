// Gridline — secrets kept out of settings.json (see Secrets.h).
#include "Secrets.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "Log.h"
#include "Settings.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace gl::secrets {

namespace fs = std::filesystem;

Backend defaultBackend() {
#ifdef __APPLE__
  const char* e = std::getenv("GRIDLINE_SECRET_STORE");
  if (e && std::string(e) == "file") return Backend::File;
  return Backend::Keychain;
#else
  return Backend::File;
#endif
}

const char* backendName(Backend b) { return b == Backend::Keychain ? "macOS Keychain" : "private file"; }

namespace {

bool validName(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
  return n[0] != '.';
}

#ifdef __APPLE__
struct CfRelease {
  CFTypeRef r;
  ~CfRelease() {
    if (r) CFRelease(r);
  }
};
CFStringRef cfStr(const std::string& s) { return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false); }
CFMutableDictionaryRef query(const std::string& service, const std::string& account) {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef sv = cfStr(service), ac = cfStr(account);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, sv);
  CFDictionarySetValue(q, kSecAttrAccount, ac);
  CFRelease(sv);
  CFRelease(ac);
  return q;
}
#endif

}  // namespace

std::string Store::folder() const {
  if (!dir.empty()) return dir;
  return (fs::path(Settings::defaultPath()).parent_path() / "secrets").string();
}

bool Store::get(const std::string& name, std::string& value) const {
  if (!validName(name)) return false;
#ifdef __APPLE__
  if (backend == Backend::Keychain) {
    CFMutableDictionaryRef q = query(service, name);
    CfRelease qr{q};
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef out = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &out);
    CfRelease outr{out};
    if (st != errSecSuccess || !out || CFGetTypeID(out) != CFDataGetTypeID()) return false;
    const auto* d = static_cast<CFDataRef>(out);
    value.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(d)), static_cast<size_t>(CFDataGetLength(d)));
    return true;
  }
#endif
  const fs::path p = fs::path(folder()) / name;
  const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat sb{};
  if (::fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > 65536) {
    ::close(fd);
    return false;
  }
  if ((sb.st_mode & 077) != 0) ::fchmod(fd, 0600);  // tighten a file someone loosened
  std::string v(static_cast<size_t>(sb.st_size), '\0');
  const ssize_t n = v.empty() ? 0 : ::read(fd, v.data(), v.size());
  ::close(fd);
  if (n < 0) return false;
  v.resize(static_cast<size_t>(n));
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
  value = v;
  return true;
}

bool Store::set(const std::string& name, const std::string& value, std::string* err) const {
  if (!validName(name)) {
    if (err) *err = "invalid secret name";
    return false;
  }
#ifdef __APPLE__
  if (backend == Backend::Keychain) {
    CFMutableDictionaryRef q = query(service, name);
    CfRelease qr{q};
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(value.data()), static_cast<CFIndex>(value.size()));
    CfRelease dr{data};
    CFMutableDictionaryRef upd = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CfRelease ur{upd};
    CFDictionarySetValue(upd, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, upd);
    if (st == errSecItemNotFound) {
      CFDictionarySetValue(q, kSecValueData, data);
      CFStringRef label = cfStr("Gridline " + name);
      CFDictionarySetValue(q, kSecAttrLabel, label);
      CFRelease(label);
      CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
      st = SecItemAdd(q, nullptr);
    }
    if (st != errSecSuccess) {
      if (err) *err = "Keychain error " + std::to_string(static_cast<int>(st));
      return false;
    }
    return true;
  }
#endif
  std::error_code ec;
  const fs::path d = folder();
  fs::create_directories(d, ec);
  ::chmod(d.c_str(), 0700);
  const fs::path tmp = d / ("." + name + ".tmp");
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    if (err) *err = "cannot write " + tmp.string() + ": " + std::strerror(errno);
    return false;
  }
  ::fchmod(fd, 0600);
  const std::string text = value + "\n";
  const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) {
    fs::remove(tmp, ec);
    if (err) *err = "cannot write " + tmp.string();
    return false;
  }
  fs::rename(tmp, d / name, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (err) *err = "cannot replace " + (d / name).string();
    return false;
  }
  return true;
}

bool Store::remove(const std::string& name) const {
  if (!validName(name)) return false;
#ifdef __APPLE__
  if (backend == Backend::Keychain) {
    CFMutableDictionaryRef q = query(service, name);
    CfRelease qr{q};
    return SecItemDelete(q) == errSecSuccess;
  }
#endif
  std::error_code ec;
  return fs::remove(fs::path(folder()) / name, ec);
}

std::string migrate(Settings& settings, const std::string& key, const std::string& settingsPath, const Store& store,
                    const std::string& name, const std::function<std::string()>& generate, bool* stored) {
  const std::string legacy = settings.getString(key);
  std::string value;
  bool have = store.get(name, value) && !value.empty();
  bool inStore = have;
  if (!have) {
    value = !legacy.empty() ? legacy : (generate ? generate() : std::string());
    std::string err;
    inStore = !value.empty() && store.set(name, value, &err);
    if (!inStore && !value.empty()) logging::warning("settings", "cannot keep " + name + " in the " + backendName(store.backend) + ": " + err);
  }
  if (inStore && !legacy.empty()) {  // moved: no copy left in settings.json
    settings.set(key, std::string());
    settings.save(settingsPath);
    logging::info("settings", name + " moved from settings.json to the " + std::string(backendName(store.backend)));
  } else if (!inStore && legacy != value && !value.empty()) {  // no store: keep it where it was
    settings.set(key, value);
    settings.save(settingsPath);
  }
  if (stored) *stored = inStore;
  return value;
}

}  // namespace gl::secrets
