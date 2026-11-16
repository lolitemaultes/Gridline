// Gridline - window bodies any host can draw (K-112 rule R3: hosted bodies, not copies).
//
// The channel settings, the effect editor, the master chain and Loupe are each split once into
// a frame (their window) and a body (everything inside it). A layout part that shows one of
// them inside its own frame (a device band, an instrument page, a lower frame, a sheet) calls
// the body; the same code draws the same editor everywhere, with one undo history and one state.
//
// A host claims a body for the frame it draws it in. The body's own window then draws nothing
// on the next frame (its open flag means "this device is selected or expanded" there), and
// comes back as a window when no host claims it any more (another layout, the host closed).
// Studio has no hosts, so its windows are exactly as before.
#pragma once
#include <string>
#include "../core/Types.h"

namespace gl {
class App;

struct BodyFrame {
  float width = 600;       // the room the body has (logical px)
  bool zoom = false;       // run the editor zoom region (K-19); hosts bypass it and fit instead
  const char* host = "";   // the host's id, for reports ("Device chain")
  const char* zoomKey = "fxeditor";  // the zoom region's window key (K-112b: an extra effect window has its own)
};

// Inside a window or child of the host's own: the channel's header row, strip, pages and
// instrument editor (the channel settings window's content).
void drawChannelSettingsBody(App& a, ObjectId channel, const BodyFrame& f);
// One mixer slot's effect: its menu, header and editor (the effect editor window's content).
void drawEffectEditorBody(App& a, ObjectId mixerTrack, int slot, const BodyFrame& f);
// The master chain's toolbar, macros, readouts and effects (the Master chain window's content).
void drawMasterChainBody(App& a, const BodyFrame& f);
// Loupe's panel content (the pinned Loupe session, or its empty state) in `w` x `h`.
void drawLoupeBody(App& a, float w, float h);

namespace hostbody {
enum class Body : int { ChannelSettings = 0, EffectEditor, MasterChain, Loupe, Count };
const char* name(Body b);
// A host drew this body this frame: its window stays out of the way next frame.
void claim(Body b, const char* host);
// True when a host drew the body last frame or earlier this frame (the window draws nothing now).
bool claimed(Body b);
// Every body draw, by any host or window, with its target: the dev report checks no body is drawn
// twice in a frame ("layout bodies").
void drawn(Body b, ObjectId owner, int slot);
// Dev: last frame's body draws, "<body> <owner>/<slot> x<count>" separated by "; ".
std::string drawnReport();
// Dev: how often a body with this target was drawn so far this frame, and by which window.
int drawnNow(Body b, ObjectId owner, int slot, std::string* host);
const char* claimedBy(Body b);
// Call once per frame (before the windows draw): this frame's claims become "last frame's".
void frameBegin();
}  // namespace hostbody

}  // namespace gl
