#pragma once
// K-86 part 2 (0.7.9): Collaboration chat panel, "Send to chat" prompt, and "View" + element flash.

#include "core/CollabChat.h"
#include "imgui.h"

#include <string>
#include <vector>

namespace gl {
class App;
struct Note;
struct ParamId;
}  // namespace gl

namespace gl::collabchat {

// Draws the docked collaboration Chat panel (collab sessions only) and the "Send to chat" prompt.
void draw(App& a);

// Per-frame housekeeping (hides Chat outside collab sessions, expires active note highlights).
void frame(App& a, double now);

// Right-click menu helper: does nothing outside collab sessions; in collab sessions appends
// an optional separator + "Send to chat..." which opens the compact comment prompt.
bool sendToChatMenuItem(App& a, const ChatAttachment& att, bool separatorBefore = true);

// Opens the compact "Send to chat" comment prompt targeting `att`.
void promptSendToChat(App& a, const ChatAttachment& att);

// Navigates to the element referenced by `att`, opens/focuses its window, selects/scrolls it
// into view, and briefly flashes the target (or shows a notice if it was deleted).
bool viewAttachment(App& a, const ChatAttachment& att);

// Returns 0..1 highlight strength when `n` in (patId, chId) is currently flashed by a Notes View action.
float noteFlash(const App& a, ObjectId patId, ObjectId chId, const Note& n, ImU32* color = nullptr);

// Dev/test commands handled under "collab chat-..." in collabui::devCommand.
bool devCommand(App& a, const std::vector<std::string>& args, int frame);

// Canonical flash keys shared by View navigation and UI element renderers.
std::string paramFlashKey(const ParamId& pid);
std::string slotFlashKey(ObjectId mixerId, int slot);
std::string pluginFlashKey(ObjectId ownerId, int slot);
std::string markerFlashKey(Tick pos);
std::string patternFlashKey(ObjectId patId);

}  // namespace gl::collabchat
