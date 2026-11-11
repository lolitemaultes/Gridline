// Gridline - drum kits (.h2drumkit, drumkit.xml and their folders) loaded as drum pad channels
// (core/HydrogenKit.h). A kit is read on a worker: a packed one unpacks into the local cache, the
// way zipped projects do, and the song changes in one undo step once it is read. The import report
// names the kit, its author and licence and what could not be carried over. The browser shows a
// kit's facts and its instruments, each one played with a click.
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::drumkitui {

// Loads the kit at `path`. `replace`: the drum pad channel whose kit it replaces (kNoId: new
// channels, inserted at channel index `insertAt`, < 0: after the selected channel).
void load(App& a, const std::string& path, ObjectId replace = kNoId, int insertAt = -1);
bool loading();
// Per frame (from drawDialogs): applies a load that finished, draws the import report.
void draw(App& a);

// What the browser shows of a kit, read on a worker. nullptr while it is read.
struct Summary {
  bool ok = false;
  std::string error;
  std::string name, author, license;
  std::vector<std::string> instruments;  // names in pad order
  std::vector<std::string> previews;     // a sample of each (the loudest layer), "" when none
  int layers = 0;
};
const Summary* summary(const std::string& path);

// dev-input "drumkit load <path>" / "drumkit report": loads like a drop, prints the report.
bool devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::drumkitui
