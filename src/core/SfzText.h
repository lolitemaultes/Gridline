// Gridline: the text of an SFZ map as a player reads it. Comments go, #include pulls in other files,
// #define names values used as $name further on. A map without those directives only loses its
// comments. Used by the player (Plasma) and by packing (ProjectInterop), so both read the same map.
#pragma once
#include <string>
#include <vector>

namespace gl::sfz {

struct Expanded {
  std::string text;                   // one flat map
  std::vector<std::string> included;  // files pulled in, in order
  std::vector<std::string> missing;   // #include files that do not exist
  bool looped = false;                // an #include led back to a file being read: that one was skipped
};

// An included file is looked for beside the map first, then beside the file that names it. Samples in
// included files stay relative to the map, as in other SFZ players. False when `path` cannot be read.
bool expand(const std::string& path, Expanded& out);

}  // namespace gl::sfz
