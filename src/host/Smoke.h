// Gridline — `gridline plugin-smoke <format> <entry> [--sandbox]`: loads every
// class of a plugin entry through the same path the app uses (scan → hosted
// type → engine adapter), processes silence and a note / a test tone, sends
// MIDI controllers, lists programs and checks that the state survives
// save/restore. Prints a short report; exit code 0 when every class passed.
#pragma once
#include <string>

namespace gl::host {
[[noreturn]] void smokeMain(const std::string& format, const std::string& entry, bool sandbox = false);
}
