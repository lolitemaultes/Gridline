// Gridline: running inside the Linux environment of a Chromebook (ChromeOS's Linux development
// environment, a Debian container in a virtual machine) or of the Linux Terminal on Android-based
// laptops (a Debian virtual machine). Both are ordinary Debian systems to Gridline; what differs is
// what is sensible by default and what to tell the user:
//
//   * Audio reaches the speakers through a sound server in the container that runs at 48 kHz with
//     a fixed period of 1024 frames (ChromeOS's own configuration). A smaller buffer only adds the
//     risk of drop-outs, and 44.1 kHz adds resampling, so a new installation starts at 48 kHz and
//     1024 frames there. A user's own choice always wins (these are only the defaults).
//   * Files from the host are only visible once shared: ChromeOS puts shared folders below
//     /mnt/chromeos, the Android terminal puts the device's shared storage at /mnt/shared.
//   * USB MIDI devices reach Linux only on some Chromebooks.
//
// Detection looks only at files the environments create; nothing is read from the user's files.
//   ChromeOS:  /dev/.cros_milestone (the ChromeOS milestone, written into the container),
//              /opt/google/cros-containers (the guest tools, mounted into the container),
//              the cros-guest-tools package (installed in every image Google ships).
//   Android:   the forwarder-guest package (the terminal's port forwarder, in every image).
// GRIDLINE_CHROMEOS=off turns the detection off; =chromeos / =android forces it (support, tests).
#pragma once
#include <string>

namespace gl {
class Settings;
}

namespace gl::chromeos {

enum class Kind { None, ChromeOs, AndroidTerminal };

// Looks below `root` ("/" for the real system; a test folder in the tests).
Kind detect(const std::string& root);
// The running system (Linux only; None elsewhere), worked out once. GRIDLINE_CHROMEOS applies.
Kind current();
// GRIDLINE_CHROMEOS: "off" / "0" -> None, "chromeos" / "1" -> ChromeOs, "android" -> AndroidTerminal,
// anything else (or unset) -> `detected`.
Kind withOverride(Kind detected, const char* env);

// For the log and the About text: "ChromeOS Linux environment (milestone 150)", "Linux Terminal on Android".
std::string describe(Kind k, const std::string& root = "/");
// The ChromeOS milestone from /dev/.cros_milestone, 0 when unknown.
int milestone(const std::string& root = "/");

// Where the files shared from the host appear ("" when not on such a system).
std::string sharedFolder(Kind k);
// A one-line hint for the file dialog: how to make host files visible ("" when not on such a system).
const char* shareHint(Kind k);
// A one-line hint for the MIDI page when no device is found ("" when not on such a system).
const char* midiHint(Kind k);

struct AudioDefaults {
  int sampleRate = 44100;
  int bufferFrames = 256;
};
AudioDefaults audioDefaults(Kind k);
// Makes the defaults of `s` the ones for `k` (call before Settings::load: saved values still win).
void applyDefaults(Settings& s, Kind k);

}  // namespace gl::chromeos
