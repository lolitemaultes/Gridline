// Gridline: a seeded stress run of the running app (--dev-input "stress ..."). One or more random
// actions per frame, through the same calls the editors, menus and transport use: rapid edits,
// undo and redo, adding and removing channels, inserts, patterns and clips, play and stop, editors
// opening and closing, renders starting and cancelling, projects being replaced, and random mouse
// and key input over the panels. The same seed replays the same action list; each action is printed
// before it runs, so the last line of a crashed run names what it was doing.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
namespace devstress {

// stress <seed> <frames> <actions per frame> <work folder> [groups] [project to reopen]
// groups: letters of the action groups to use (default "mceptrui"):
//   m model edits, c channels, e effects and inserts, p patterns and clips, t transport,
//   r renders, u windows and editors, i mouse and keys. Only when asked for: l project reopen,
//   x renders without any cancel (each one runs to its end, encoder included, while the edits go on)
void start(const std::vector<std::string>& args);
// Each frame, before the scripted input of that frame.
void frame(App& a);
bool active();

}  // namespace devstress
}  // namespace gl
