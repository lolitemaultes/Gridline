// Gridline — J-78: notification cards (App::notify). How long a card stays
// up, and the text a click copies to the clipboard: the untranslated English
// message plus its details (file path, error code), so it can be pasted into
// a bug report whatever the interface language.
#pragma once
#include <string>

namespace gl::toast {

// Seconds a card stays visible; errors stay longer. Hovering keeps it up.
double lifetime(bool error);
// Seconds the "Copied" feedback shows after a click.
inline constexpr double kCopiedFeedback = 1.0;
// Clipboard text of a notification: "Error: <english>" (or just the message)
// and, when given and not already in the message, the details on the next line.
std::string clipboardText(const std::string& english, const std::string& details, bool error);

}  // namespace gl::toast
