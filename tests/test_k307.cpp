// Gridline - K-307: an export crashed with "status: File name too long" on a hosted reverb's
// state text ("!gui_reverb_ir_path " and a list of impulse names on lines). Strings from plugins
// and projects that cannot name a file are never asked about, and asking never throws.
#include <filesystem>
#include <string>
#include "Check.h"
#include "core/SafeFs.h"
#include "core/Song.h"
#include "engine/TrackReceiptPass.h"

using namespace gl;

namespace {

const char* kNames[] = {"01_Piano Hall.wav", "02_Concert Hall A.wav", "03_Concert Hall B.wav", "04_Chamber.wav",
                        "05_Cathedral.wav", "06_Plate.wav", "07_Large Room.wav", "08_Medium Room.wav",
                        "09_Drum Room.wav", "10_Vocal Booth.wav", "11_Spring.wav", "12_Small Studio.wav"};

// The string from the report, with real line breaks or with them written as "\n" (two characters).
std::string reported(const char* sep) {
  std::string s = "!gui_reverb_ir_path ";
  for (size_t k = 0; k < sizeof kNames / sizeof kNames[0]; ++k) s += (k ? sep : "") + std::string(kNames[k]);
  return s;
}

Song songWith(const std::string& type, const std::string& data) {
  Song s = makeEmptySong();
  MixerTrack m;
  m.id = 900;
  m.slots.resize(1);
  m.slots[0].plugin.type = type;
  m.slots[0].plugin.data = data;
  s.mixer.push_back(m);
  return s;
}

}  // namespace

int main() {
  const std::string lines = reported("\n"), escaped = reported("\\n");
  // What could be a file.
  expect(!safefs::plausiblePath(lines), "the reported text (lines) is no path");
  expect(!safefs::plausiblePath(std::string(300, 'a') + ".wav"), "a name longer than 255 is no path");
  expect(!safefs::plausiblePath(std::string(5000, '/')), "a string longer than PATH_MAX is no path");
  expect(!safefs::plausiblePath(""), "the empty string is no path");
  expect(safefs::plausiblePath("/home/user/Samples/kick.wav") && safefs::plausiblePath("C:\\Samples\\kick.wav"), "paths are paths");
  // Asking never throws, whatever the string.
  bool threw = false;
  try {
    const std::string longName = "/" + std::string(1000, 'x') + escaped;
    expect(!safefs::exists(lines) && !safefs::exists(escaped) && !safefs::isFile(escaped) && !safefs::isDir(longName) &&
               !safefs::exists(longName),
           "the reported text is not there");
    expect(!safefs::existsAt(std::filesystem::path(longName)), "nor a path built from it");
  } catch (...) {
    threw = true;
  }
  expect(!threw, "no file system question throws");
  // The export worker's Track Receipt pass on the reported plugin state (hosted and built-in).
  // (doubled: over 255 characters, the name limit, where the throwing call failed with "File name too long")
  const std::string twice = escaped + "\\n" + escaped;
  for (const std::string& data : {escaped, lines, twice, "vst3s1:" + escaped, std::string("C:\\") + twice}) {
    for (const char* type : {"vst3:0123456789ABCDEF", "clap:com.example.reverb", "gl.convolver"}) {
      const Song s = songWith(type, data);
      threw = false;
      size_t odd = 0;
      try {
        for (const auto& r : receipt::fileRefs(s))
          if (r.path.find('\n') != std::string::npos || r.path.size() > 4096 || !safefs::plausiblePath(r.path)) ++odd;
        receipt::scan(s, true);
        const std::string out = (std::filesystem::temp_directory_path() / "gridline_k307_render.wav").u8string();
        receipt::writeRenderOrigin(s, out);
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(out + ".origin.json"), ec);
      } catch (...) {
        threw = true;
      }
      expect(!threw && odd == 0, (std::string("the receipt pass takes no text for a file: ") + type).c_str());
    }
  }
  // Real file names inside a plugin's state are still found.
  const Song ok = songWith("vst3:0123456789ABCDEF", "ir=/home/user/IR/Plate.wav;x=1");
  bool found = false;
  for (const auto& r : receipt::fileRefs(ok)) found = found || r.path == "/home/user/IR/Plate.wav";
  expect(found, "a real path in a plugin state still counts");
  return finish();
}
