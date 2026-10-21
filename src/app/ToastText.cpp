// Gridline — J-78: notification card text. See ToastText.h.
#include "ToastText.h"

namespace gl::toast {

double lifetime(bool error) { return error ? 9.0 : 3.5; }

std::string clipboardText(const std::string& english, const std::string& details, bool error) {
  std::string s = error ? "Error: " + english : english;
  if (!details.empty() && english.find(details) == std::string::npos) s += "\n" + details;
  return s;
}

}  // namespace gl::toast
