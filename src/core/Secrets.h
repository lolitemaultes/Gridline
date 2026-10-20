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
// Keychain on macOS unless GRIDLINE_SECRET_STORE=file; File elsewhere.
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

// The secret `name`, moved out of the plain setting `key` when that still
// holds one (the setting is cleared and saved to `settingsPath`), or made
// with `generate` when none exists. If the store cannot be written the value
// stays in (or goes to) the setting so nothing is lost. `stored` = it is in
// the store.
std::string migrate(Settings& settings, const std::string& key, const std::string& settingsPath, const Store& store,
                    const std::string& name, const std::function<std::string()>& generate, bool* stored = nullptr);

}  // namespace secrets
}  // namespace gl
