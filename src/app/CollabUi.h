// Gridline - J-111: collaboration in the app. The collaboration panel
// (members, invite, reconnect, leave), Join collaborative project, the sync
// review for the admin, collaborators in the top bar, remote cursors, flashes
// of remote edits, loading placeholders, the Collaboration settings page
// (profile, picture, colour, identity, connection, version status) and the
// version gate. Logic: src/collab (Controller, Session, Hub). Docs:
// docs/collaboration.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
namespace collab {
class Controller;
class Session;
}
}

namespace gl::collabui {

// Per frame before drawing (network, remote edits, gesture, presence, saving).
void frame(App& a, double now);
// Windows and dialogs (after the other windows).
void draw(App& a);
// Right after ImGui::NewFrame: what the pointers are over is collected while the
// windows draw (J-111d, CursorAnchor.h).
void beginFrame(App& a);
// This user's pointer anchor (sent to the others), and collaborators' cursors on
// top of everything (call last, after every window, popup and menu).
void drawCursors(App& a);
// Top bar: collaborator chips right to left from `rx`; returns the new right edge.
float topBar(App& a, ImDrawList* dl, float rx, float top, float height);
// Settings > Collaboration.
void settingsPage(App& a, float fieldW);
// Project settings: the Collaboration section.
void projectSection(App& a);
// J-111f: "Shared name  <name>  Rename..." as a form row (Project info), labels ending at `labelX`;
// nothing when no collaborative project is open.
void projectNameRow(App& a, float labelX);
// J-111f: the open collaborative project's shared name ("" otherwise): the window title.
std::string projectTitle(const App& a);
// File menu entries.
void fileMenu(App& a);

// J-111e: how "Show in Finder / Files" opens a folder (main.cpp: SDL_OpenURL with a file:// URL).
void setOpenUrl(std::function<bool(const std::string&)> f);
// The collaboration controller (made on first use; nullptr in plugin builds).
collab::Controller* controller(App& a);
collab::Session* session(App& a);
bool active(const App& a);
// Opening a project file: a collaborative project opens with its session (or
// asks for an update first). Returns true when it handled the file.
bool openIfCollab(App& a, const std::string& projectFile, bool* opened = nullptr);
// A collaborative project folder (or its .gridline-collab folder, or a file in
// it): its project file; "" otherwise. File > Open, drops and recent files
// open a collaborative project through this.
std::string projectFileIn(const std::string& path);
// Before the open project is replaced (new / open): the session closes.
void closeSession(App& a);
// Plugins of hosted formats are hidden and refused in a collaborative project.
bool pluginAllowed(const App& a, const std::string& type);
// The file of a sample / IR is still arriving from a collaborator.
bool fileLoading(App& a, const std::string& path, float* progress = nullptr);
// A collaborator just changed `object` ("channels/#5", "playlist/#40", "mixer/#3"):
// 0..1 highlight strength and the collaborator's colour.
float flash(App& a, const std::string& object, ImU32* color = nullptr);
// Striped "loading" overlay for a placeholder (clip or slot).
void drawLoading(ImDrawList* dl, ImVec2 a, ImVec2 b, float progress, bool compact);
// Opens the collaboration panel / join dialog.
void openPanel(App& a);
void openJoin(App& a);
// Scripted runs (--dev-input "<frame> collab <command> <args>", see main.cpp) and,
// per frame in such runs, their pending actions and waitfor signals.
// Returns true when the command moved this user's pointer to *pointer ("collab point").
bool devCommand(App& a, const std::vector<std::string>& args, int frame, ImVec2* pointer = nullptr);
void devFrame(App& a);
// Dev / screenshots (--show collab-demo): a collaborative session with a simulated
// collaborator in this process (in-memory network): their cursor, edits, an audio
// file still arriving, conflicting offline edits for the review.
void startDemo(App& a, const std::string& folder, bool quietReview = false);
// J-111g: the system pointer arrow's height in logical units (host/ui/NativeView.h
// pointerSize over the interface scale): collaborators' cursors are drawn this size.
void setPointerSize(float logicalH, const std::string& source);

}  // namespace gl::collabui
