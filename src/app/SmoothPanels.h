// Gridline — K-128: Smooth layout panels (Event List, Project/Track Notes, Score Editor).
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::smoothpanels {

// Smooth right-hand List Editors panel (Event | Marker | Tempo | Signature).
void drawEventList(App& a);

// Smooth right-hand Notepad panel (Project | Track tabs persisted to SongInfo::comments & Channel::notes).
void drawNotes(App& a);

// Smooth bottom Score Editor tab (Grand / Treble / Bass staff, vector notation, interactive editing).
void drawScore(App& a);

// Dev commands ("parts notes ...", "parts eventlist ...", "parts score ..."). Returns true if handled.
bool devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::smoothpanels
