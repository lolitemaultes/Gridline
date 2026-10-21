// Gridline — settings store (ST-01). One versioned JSON file in the XDG config
// directory; typed accessors with registered defaults.
#pragma once
#include <map>
#include <string>
#include "Json.h"

namespace gl {

class Settings {
 public:
  static constexpr int kVersion = 1;

  Settings();  // registers defaults (see Settings.cpp)

  // $GRIDLINE_CONFIG_HOME/gridline/settings.json, else $XDG_CONFIG_HOME/gridline/..., else ~/.config/gridline/....
  static std::string defaultPath();
  // Missing file is not an error (defaults stay). Unknown keys are kept.
  bool load(const std::string& path, std::string* err = nullptr);
  bool save(const std::string& path, std::string* err = nullptr) const;  // atomic

  bool getBool(const std::string& key) const;
  double getNumber(const std::string& key) const;
  std::string getString(const std::string& key) const;
  // Setters reject keys without a registered default of the same type.
  bool set(const std::string& key, bool v);
  bool set(const std::string& key, double v);
  bool set(const std::string& key, int v) { return set(key, static_cast<double>(v)); }
  bool set(const std::string& key, const std::string& v);
  bool set(const std::string& key, const char* v) { return set(key, std::string(v)); }
  void resetToDefaults() { values_ = defaults_; }

 private:
  const Json* lookup(const std::string& key, Json::Type t) const;
  std::map<std::string, Json> defaults_;
  std::map<std::string, Json> values_;
};

}  // namespace gl
