// Gridline — K-218: the loop and sample libraries already on this computer (the system's loop
// folders, installed content packs, the common sample folders of Linux and Windows). Gridline
// only checks whether these known folders exist (it never lists or reads anything to find them),
// asks once before adding them to the browser, and remembers the answer.
#pragma once
#include <string>
#include <vector>

namespace gl::soundlib {

struct Library {
  std::string path;
  std::string name;  // shown in the prompt ("System loops", "Shared samples", ...)
};

// The known library folders for this platform, under `home` (the user's home folder) and the
// system's own folders; `systemRoot` prefixes the system folders (tests point both at fixture
// folders). Only folders that exist are returned, each once.
std::vector<Library> candidates(const std::string& home, const std::string& systemRoot = "");

// The ask-once state, kept in <configDir>/sound-libraries.json.
struct Answer {
  bool asked = false;
  std::vector<std::string> added;  // the folders the user said yes to
};
Answer loadAnswer(const std::string& configDir);
bool saveAnswer(const std::string& configDir, const Answer& a);

// The libraries to offer now: candidates that exist, when the user has not been asked yet and
// none of them is a browser folder already. Empty when there is nothing to ask.
std::vector<Library> toOffer(const std::string& configDir, const std::string& home, const std::string& systemRoot = "");

// Records the user's answer (the question is not asked again either way). The caller adds the
// accepted folders to the browser.
void answer(const std::string& configDir, const std::vector<Library>& libs, bool accept);

}  // namespace gl::soundlib
