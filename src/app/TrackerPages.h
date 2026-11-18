// Gridline - the Tracker layout's Song, Steps and Mix pages (K-106). See TrackerPages.cpp.
#pragma once
#include <string>
#include <vector>

#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::trackerpages {

// The Tracker layout is on screen, built from its region shell: the Playlist, Channel rack and
// Mixer windows draw these pages instead of their own views.
bool active(const App& a);
// The frame the Tracker layout comes in (a switch, or the start): the shell docks its windows
// over this frame, so a window not docked yet skips drawing (the switch snapshot covers the
// frame); the hidden pages then ask for no icon sizes they will not show. Kept by the header band,
// which draws on every Tracker frame.
bool enteringFrame();
void song(App& a);    // the Playlist window's body
void steps(App& a);   // the Channel rack window's body
void mixer(App& a);   // the Mixer window's body
void pads(App& a);    // the Live pads window's body
// A page title bar in the Tracker look (the title left, `info` right in the monospaced face) at
// the window cursor, full width; the cursor moves below it. For the pages their own views draw (Roll).
void pageTitle(App& a, const char* title, const std::string& info);
// cp16: the Roll page's tool row in the band's look (in place of the piano roll's toolbar): Menu
// (the piano roll's whole menu), the tools, the snap, the instrument, zoom. The cursor moves below it.
void rollToolbar(App& a);
// cp16: the Audio page with no sample open (in place of the audio editor's empty state).
void audioEmpty(App& a, float w, float h);
// Dev (--dev-input "tracker order ..."), the Song page's edits as its keys and buttons make them:
//   dump · go <i> · ins [<i>] (the entry's pattern after it) · del <i> · move <i> <+1|-1> · set <i> <ptn>
//   append <ptn> · new · clone <ptn> · bars <ptn> <+1|-1> · loop <i0> <i1> | loop off · play <i>
// (patterns by number, entries from 0). "dump" prints the order: "order frame F: n N | ptn ptn ... | loop a-b | pos p".
void devOrder(App& a, const std::vector<std::string>& args, int frame);
// TRK-1118 (dev, "tracker fx ..."): the Mix page's effect menu: add <track> <type> · replace <track> <slot> <type> ·
// bypass <track> <slot> · remove <track> <slot> · open <track> <slot> · dump <track> (mixer tracks from 0, slots from 0).
// Prints "tracker fx frame F (op): track T slots k:type[(off)] ... | editor <track> <slot> | undo P \"label\"".
void devEffect(App& a, const std::vector<std::string>& args, int frame);
// cp17 (user): the Song page's sideways zoom (the Lanes view: the lane width; the arranger: its
// timeline): d > 0 in, d < 0 out, 0 fit (every lane, or the whole song). Settings, never the song.
void zoomSong(App& a, int d);
// Dev ("tracker songzoom in|out|fit|lanes|arranger|end|lane <n>|show"): prints the view as drawn.
void devSongZoom(App& a, const std::vector<std::string>& args, int frame);
// The clip picked on the Song page (its clip box edits it); view state, never saved.
extern ObjectId gSongClip;

}  // namespace gl::trackerpages
