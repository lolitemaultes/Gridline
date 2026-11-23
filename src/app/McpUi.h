// Gridline — AI assistants in the app (SC-07): the opt-in local MCP server
// (Settings > Integrations), its status lamp, and the activity log of what
// assistants did. The server listens on 127.0.0.1 only, needs the token shown
// in the settings, and is polled from the UI loop, so every tool call runs on
// the thread that owns the document (one undo step per editing call).
#pragma once
#include <string>
#include <vector>

struct ImDrawList;

namespace gl {
class App;
namespace mcpui {

// Per frame: starts / stops the server as the settings say, answers requests.
void tick(App& a);
// K-142 (0.7.8 follow-up): whether the server needs the interface to keep taking turns while nothing
// is on screen: starting (or trying again), a job under way (its progress streamed in turns). A client
// connecting, its request and its reply wake the sleeping interface themselves (HttpServer::watch).
bool needsTurns(const App& a);
// K-281 (dev input): "mcp call <tool> <json>" runs a tool call and prints its reply.
bool devCommand(App& a, const std::vector<std::string>& args);
// Stops the server and removes the endpoint file (app shutdown).
void shutdown();
// Settings > Integrations.
void settingsPage(App& a, float fieldW);
// Tools > AI assistant activity.
void showActivity();
void hideActivity();  // K-112 (dev): the layout inventory closes it again
void drawActivityWindow(App& a);
// Status bar lamp right of the other lamps; moves rx left by its width.
void statusLamp(App& a, ImDrawList* dl, float& rx, float ty, float fs);

}  // namespace mcpui
}  // namespace gl
