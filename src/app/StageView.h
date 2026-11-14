// Gridline - the Stage: the mixer window's second view (docs/stage-view.md). The tracks stand on a
// perspective stage seen from the listener: left / right is the pan, front / back the distance, the
// flat oval under a token its stereo width. Dragging a track onto another (or onto a group) puts it
// on that exact spot, and tracks that share a spot sit together in a tray above it; tokens never
// rest on top of each other. Under the stage a control panel shows what is selected (one track,
// several, or the whole stage), with Spread out and Arrange by sound. Every edit goes through the
// mixer parameters, so the strips, undo, automation and collaborators see the same change.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
struct Song;
namespace roomops {
struct RoomTemplate;
}
namespace stageui {

// The mixer window shows the Stage (App::showStage, the user setting ui.mixerView).
bool active(App& a);
// Switches the mixer window's view (opens the mixer when it is closed).
void setView(App& a, bool stage);
void toggle(App& a);
// Each frame, before the windows: keeps App::showStage and the setting in step, opens the mixer
// when the Stage is asked for elsewhere (View menu, palette, keys).
void sync(App& a);
// The Mix | Stage switch (both toolbars).
void viewSwitch(App& a);
// The Stage's toolbar and body inside the mixer window (instead of the strips).
void drawBody(App& a);
// Dev check ("stage" in --dev-input): every token, group, the selection and undo position, to stdout.
// With arguments it acts instead (screenshots, app tests): "select 3,5" (mixer indexes), "none",
// "preview spread|sound|preset <k>", "apply", "room <type>", "solo <i>", "open presets|room|fx",
// K-270: "roomsolo <room> [only]" (its headphones), "listen off", "roomdelete <room>", "rooms open|close",
// "rooms width <px>" (the rooms column; under 150 the slim rail).
void devCommand(App& a, int frame, const std::vector<std::string>& args = {});
// K-270: soloed rooms (each room's headphones): mutes, in the song the engine plays, every track not in a
// soloed room or downstream of one. Only this computer; the project is untouched.
void listenFilter(App& a, Song& s);
// K-224: the room the Stage shows (core/StageRoomOps.h index; the Mix view's room filter follows it).
int currentRoom(App& a);
// K-224: a room template on the room shown, or a new room from it (shown, its name being edited).
void applyRoomTemplate(App& a, const roomops::RoomTemplate& t);
void newRoomFrom(App& a, const roomops::RoomTemplate& t);
// K-224: asks before deleting one of the user's own room templates (the app's confirm prompt).
void confirmDeleteTemplate(App& a, const roomops::RoomTemplate& t);

}  // namespace stageui
}  // namespace gl
