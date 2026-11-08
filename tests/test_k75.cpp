// Gridline - K-75: the Linux environments of Chromebooks and of Android-based laptops. Detection
// from the files those environments create (fake system roots here), their audio defaults (a new
// installation starts at the sound server's 48 kHz / 1024 frames, a saved choice still wins), the
// hints, and the shading language for an OpenGL 2.x context (virtual machine graphics).
#include <filesystem>
#include <fstream>
#include <string>
#include "../src/core/ChromeOs.h"
#include "../src/core/GlslChoice.h"
#include "../src/core/Settings.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void expectS(bool ok, const std::string& what) { expect(ok, what.c_str()); }

void touch(const fs::path& p, const std::string& text = "") {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << text;
}

fs::path freshRoot(const fs::path& base, const char* name) {
  std::error_code ec;
  const fs::path r = base / name;
  fs::remove_all(r, ec);
  fs::create_directories(r, ec);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  std::error_code ec;
  const fs::path base = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path(ec) / "gridline_k75";
  fs::create_directories(base, ec);

  // ---- detection ----
  {
    const fs::path plain = freshRoot(base, "plain");
    touch(plain / "etc/debian_version", "12.13\n");
    expect(chromeos::detect(plain.string()) == chromeos::Kind::None, "a plain Debian system is not ChromeOS");
    expect(chromeos::describe(chromeos::Kind::None).empty(), "nothing to describe off ChromeOS");
    expect(chromeos::sharedFolder(chromeos::Kind::None).empty() && !*chromeos::shareHint(chromeos::Kind::None) &&
               !*chromeos::midiHint(chromeos::Kind::None),
           "no shared folder or hints off ChromeOS");

    const fs::path ms = freshRoot(base, "milestone");
    touch(ms / "dev/.cros_milestone", "150\n");
    expect(chromeos::detect(ms.string()) == chromeos::Kind::ChromeOs, "the milestone file marks ChromeOS");
    expect(chromeos::milestone(ms.string()) == 150, "the milestone is read");
    expectS(chromeos::describe(chromeos::Kind::ChromeOs, ms.string()) == "ChromeOS Linux environment (milestone 150)",
            "described with the milestone: " + chromeos::describe(chromeos::Kind::ChromeOs, ms.string()));

    const fs::path tools = freshRoot(base, "tools");
    fs::create_directories(tools / "opt/google/cros-containers", ec);
    expect(chromeos::detect(tools.string()) == chromeos::Kind::ChromeOs, "the mounted guest tools mark ChromeOS");
    expect(chromeos::milestone(tools.string()) == 0, "no milestone file: 0");
    expect(chromeos::describe(chromeos::Kind::ChromeOs, tools.string()) == "ChromeOS Linux environment", "described without a milestone");

    const fs::path pkg = freshRoot(base, "package");
    touch(pkg / "var/lib/dpkg/info/cros-guest-tools.list", "/usr/share/doc/cros-guest-tools\n");
    expect(chromeos::detect(pkg.string()) == chromeos::Kind::ChromeOs, "the guest tools package marks ChromeOS (an image not started yet)");

    const fs::path bad = freshRoot(base, "badmilestone");
    touch(bad / "dev/.cros_milestone", "garbage");
    expect(chromeos::milestone(bad.string()) == 0, "an unreadable milestone is 0");

    const fs::path avf = freshRoot(base, "android");
    touch(avf / "var/lib/dpkg/info/forwarder-guest.list", "/usr/bin/forwarder_guest\n");
    expect(chromeos::detect(avf.string()) == chromeos::Kind::AndroidTerminal, "the port forwarder package marks the Android terminal");
    expect(chromeos::describe(chromeos::Kind::AndroidTerminal) == "Linux Terminal on Android", "Android terminal described");
    expect(chromeos::sharedFolder(chromeos::Kind::AndroidTerminal) == "/mnt/shared", "Android shared storage folder");
    expect(chromeos::sharedFolder(chromeos::Kind::ChromeOs) == "/mnt/chromeos", "ChromeOS shared folders");
    expect(*chromeos::shareHint(chromeos::Kind::ChromeOs) && *chromeos::midiHint(chromeos::Kind::ChromeOs), "ChromeOS hints");

    expect(chromeos::detect((base / "does-not-exist").string()) == chromeos::Kind::None, "a missing root is not ChromeOS");
  }

  // ---- GRIDLINE_CHROMEOS ----
  {
    using K = chromeos::Kind;
    expect(chromeos::withOverride(K::ChromeOs, nullptr) == K::ChromeOs, "unset: detection stands");
    expect(chromeos::withOverride(K::ChromeOs, "off") == K::None && chromeos::withOverride(K::ChromeOs, "0") == K::None, "off turns it off");
    expect(chromeos::withOverride(K::None, "chromeos") == K::ChromeOs && chromeos::withOverride(K::None, "1") == K::ChromeOs, "forced on");
    expect(chromeos::withOverride(K::None, "android") == K::AndroidTerminal, "forced Android terminal");
    expect(chromeos::withOverride(K::AndroidTerminal, "something") == K::AndroidTerminal, "other values: detection stands");
  }

  // ---- audio defaults ----
  {
    const chromeos::AudioDefaults off = chromeos::audioDefaults(chromeos::Kind::None);
    expect(off.sampleRate == 44100 && off.bufferFrames == 256, "elsewhere: the usual defaults");
    const chromeos::AudioDefaults on = chromeos::audioDefaults(chromeos::Kind::ChromeOs);
    expect(on.sampleRate == 48000 && on.bufferFrames == 1024, "ChromeOS: the sound server's rate and period");

    Settings plain;
    expect(plain.getNumber("audio.sampleRate") == 44100 && plain.getNumber("audio.bufferFrames") == 256, "Settings defaults as before");
    chromeos::applyDefaults(plain, chromeos::Kind::None);
    expect(plain.getNumber("audio.bufferFrames") == 256, "no change off ChromeOS");

    // A new installation on a Chromebook (no settings file).
    Settings fresh;
    chromeos::applyDefaults(fresh, chromeos::Kind::ChromeOs);
    expect(fresh.load((base / "no-such-settings.json").string()), "a missing settings file loads");
    expect(fresh.getNumber("audio.sampleRate") == 48000 && fresh.getNumber("audio.bufferFrames") == 1024, "a new installation starts at 48 kHz / 1024");
    fresh.resetToDefaults();
    expect(fresh.getNumber("audio.bufferFrames") == 1024, "Reset to defaults keeps the ChromeOS defaults");

    // The user's saved choice wins.
    const fs::path saved = base / "saved" / "settings.json";
    {
      Settings s;
      s.set("audio.bufferFrames", 256);
      s.set("audio.sampleRate", 44100);
      expect(s.save(saved.string()), "settings saved");
    }
    Settings again;
    chromeos::applyDefaults(again, chromeos::Kind::ChromeOs);
    expect(again.load(saved.string()), "saved settings load");
    expect(again.getNumber("audio.bufferFrames") == 256 && again.getNumber("audio.sampleRate") == 44100, "a saved buffer and rate win");

    // A setting the user changed before the new default arrived stays.
    Settings changed;
    changed.set("audio.bufferFrames", 512);
    changed.setDefault("audio.bufferFrames", 1024);
    expect(changed.getNumber("audio.bufferFrames") == 512, "a changed value is not overwritten by a new default");
    expect(!changed.setDefault("no.such.key", 1) && !changed.setDefault("audio.backend", 1), "unknown keys and other types are refused");
  }

  // ---- shading language for the context ----
  {
    expect(std::string(glslForVersionString("4.3 (Core Profile) Mesa 22.3.6")) == "#version 130", "virgl 4.3 core: GLSL 1.30");
    expect(std::string(glslForVersionString("3.0 Mesa 22.3.6")) == "#version 130", "3.0 compatibility: GLSL 1.30");
    expect(std::string(glslForVersionString("2.1 Mesa 22.3.6")) == "#version 120", "2.1: GLSL 1.20");
    expect(std::string(glslForVersionString("2.1.2 NVIDIA 340")) == "#version 120", "2.1 old driver: GLSL 1.20");
    expect(std::string(glslForVersionString(nullptr)) == "#version 130", "unknown: GLSL 1.30 as before");
    expect(std::string(glslForVersionString("")) == "#version 130", "empty: GLSL 1.30 as before");
    expect(glMajorFromVersionString("OpenGL ES 3.2 Mesa") == 3, "OpenGL ES prefix skipped");
    expect(glMajorFromVersionString("10.1") == 10, "two-digit major");
  }

  fs::remove_all(base, ec);
  return finish();
}
