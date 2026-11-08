// Gridline: K-75: see ChromeOs.h.
#include "ChromeOs.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "Settings.h"

namespace gl::chromeos {

namespace fs = std::filesystem;

namespace {

bool exists(const std::string& root, const char* rel) {
  std::error_code ec;
  return fs::exists(fs::path(root) / rel, ec);
}

bool isWord(const char* env, const char* w) { return env && std::string(env) == w; }

}  // namespace

Kind detect(const std::string& root) {
  if (exists(root, "dev/.cros_milestone") || exists(root, "opt/google/cros-containers") ||
      exists(root, "var/lib/dpkg/info/cros-guest-tools.list"))
    return Kind::ChromeOs;
  if (exists(root, "var/lib/dpkg/info/forwarder-guest.list") || exists(root, "var/lib/dpkg/info/forwarder-guest-launcher.list"))
    return Kind::AndroidTerminal;
  return Kind::None;
}

Kind withOverride(Kind detected, const char* env) {
  if (isWord(env, "off") || isWord(env, "0")) return Kind::None;
  if (isWord(env, "chromeos") || isWord(env, "1")) return Kind::ChromeOs;
  if (isWord(env, "android")) return Kind::AndroidTerminal;
  return detected;
}

Kind current() {
  static const Kind k = [] {
#if defined(__linux__)
    return withOverride(detect("/"), std::getenv("GRIDLINE_CHROMEOS"));
#else
    return withOverride(Kind::None, std::getenv("GRIDLINE_CHROMEOS"));
#endif
  }();
  return k;
}

int milestone(const std::string& root) {
  std::ifstream is(fs::path(root) / "dev/.cros_milestone");
  int m = 0;
  if (is && (is >> m) && m > 0 && m < 100000) return m;
  return 0;
}

std::string describe(Kind k, const std::string& root) {
  switch (k) {
    case Kind::None: return std::string();
    case Kind::ChromeOs: {
      const int m = milestone(root);
      return m > 0 ? "ChromeOS Linux environment (milestone " + std::to_string(m) + ")" : std::string("ChromeOS Linux environment");
    }
    case Kind::AndroidTerminal: return "Linux Terminal on Android";
  }
  return std::string();
}

std::string sharedFolder(Kind k) {
  switch (k) {
    case Kind::None: return std::string();
    case Kind::ChromeOs: return "/mnt/chromeos";
    case Kind::AndroidTerminal: return "/mnt/shared";
  }
  return std::string();
}

const char* shareHint(Kind k) {
  switch (k) {
    case Kind::None: return "";
    case Kind::ChromeOs: return "To open files from your Chromebook, right-click a folder in the Files app and choose Share with Linux. Shared folders appear here.";
    case Kind::AndroidTerminal: return "Files in the device's shared storage appear here. Add more folders in the Terminal app's settings, under Shared folders.";
  }
  return "";
}

const char* midiHint(Kind k) {
  switch (k) {
    case Kind::None: return "";
    case Kind::ChromeOs:
      return "On a Chromebook, give a USB MIDI device to Linux in the ChromeOS settings (Linux development environment, Manage USB devices), "
             "then choose Rescan. Some Chromebooks do not pass MIDI devices to Linux.";
    case Kind::AndroidTerminal: return "The Linux Terminal on this device does not pass USB devices to Linux yet.";
  }
  return "";
}

AudioDefaults audioDefaults(Kind k) {
  AudioDefaults d;
  if (k != Kind::None) {
    d.sampleRate = 48000;
    d.bufferFrames = 1024;
  }
  return d;
}

void applyDefaults(Settings& s, Kind k) {
  if (k == Kind::None) return;
  const AudioDefaults d = audioDefaults(k);
  s.setDefault("audio.sampleRate", d.sampleRate);
  s.setDefault("audio.bufferFrames", d.bufferFrames);
}

}  // namespace gl::chromeos
