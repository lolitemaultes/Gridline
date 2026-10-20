// Gridline — AI assistants in the app (SC-07): the opt-in local MCP server
// (Settings > Integrations), its status lamp, and the activity log of what
// assistants did. The server listens on 127.0.0.1 only, needs the token shown
// in the settings, and is polled from the UI loop, so every tool call runs on
// the thread that owns the document (one undo step per editing call).
#pragma once

struct ImDrawList;

namespace gl {
class App;
namespace mcpui {

// Per frame: starts / stops the server as the settings say, answers requests.
void tick(App& a);
// Stops the server and removes the endpoint file (app shutdown).
void shutdown();
// Settings > Integrations.
void settingsPage(App& a, float fieldW);
// Tools > AI assistant activity.
void showActivity();
void drawActivityWindow(App& a);
// Status bar lamp right of the other lamps; moves rx left by its width.
void statusLamp(App& a, ImDrawList* dl, float& rx, float ty, float fs);

}  // namespace mcpui
}  // namespace gl
