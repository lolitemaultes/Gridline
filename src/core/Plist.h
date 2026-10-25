// Gridline: minimal property list reader (binary "bplist00" and XML) for
// plugin states saved as Audio Unit class info. Only what state decoders
// need: the data / string values of the top-level dictionary.
#pragma once
#include <string>

namespace gl::plist {

// Value of `key` in the top-level dictionary of `bytes` (a data or string
// value; data is returned as raw bytes). False when absent or not a plist.
bool topValue(const std::string& bytes, const std::string& key, std::string& out);

}  // namespace gl::plist
