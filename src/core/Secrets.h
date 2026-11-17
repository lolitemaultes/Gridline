// Gridline — secrets kept out of settings.json (J-61): the MCP server token.
//
// macOS: the login Keychain (Security framework, generic password, service
// "Gridline"). Elsewhere (and when GRIDLINE_SECRET_STORE=file, which the
// tests set): one file per secret in <config>/gridline/secrets/, the folder
// 0700 and the file 0600, written atomically.
#pragma once
#include <functional>
#include <string>

namespace gl {
class Settings;

namespace secrets {

enum class Backend { Keychain, File };
// Keychain on macOS unless GRIDLINE_SECRET_STORE=file or GRIDLINE_CONFIG_HOME is set, or (K-327, macOS)
// HOME names another folder than the account's home (GRIDLINE_SECRET_STORE=keychain overrides the
// latter two); File elsewhere.
Backend defaultBackend();
const char* backendName(Backend b);  // "macOS Keychain" / "private file"

struct Store {
  Backend backend = defaultBackend();
  std::string dir;  // File backend: folder of the secret files ("" = <config>/gridline/secrets)
  std::string service = "Gridline";  // Keychain service name
  std::string folder() const;
  bool get(const std::string& name, std::string& value) const;  // false: none stored / unreadable
  bool set(const std::string& name, const std::string& value, std::string* err = nullptr) const;
  bool remove(const std::string& name) const;
};

// K-327: while a NoAccess lives (the command palette's and the menu tree's item walks, which run
// menu and settings page code only to collect rows), no secret is read, written or removed: get /
// set / remove fail at once without touching the Keychain, Credential Manager or secret files, and
// migrate() does nothing. A Keychain read can wait on a macOS password prompt, and a walk must
// never block or change anything. Nests; per thread.
struct NoAccess {
  NoAccess();
  ~NoAccess();
  NoAccess(const NoAccess&) = delete;
  NoAccess& operator=(const NoAccess&) = delete;
};
bool accessBlocked();
int blockedAttempts();  // how many calls a NoAccess turned away so far (tests)

// The secret `name`, moved out of the plain setting `key` when that still
// holds one (the setting is cleared and saved to `settingsPath`), or made
// with `generate` when none exists. If the store cannot be written the value
// stays in (or goes to) the setting so nothing is lost. `stored` = it is in
// the store.
std::string migrate(Settings& settings, const std::string& key, const std::string& settingsPath, const Store& store,
                    const std::string& name, const std::function<std::string()>& generate, bool* stored = nullptr);

}  // namespace secrets
}  // namespace gl
