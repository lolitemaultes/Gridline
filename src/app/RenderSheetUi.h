// Gridline: render MIDI to audio (K-254), the interface: a "Render to audio" submenu on every rack
// row, note clip, region and note selection (quick destinations that use the last options, and
// Render options...), the render sheet (the source in words with a live preview and its slice
// points, eight destination cards, the length, tail and slice options), the background job with
// progress and Cancel, and the result notice (Undo, Open in Loupe, Options...). The same in every
// layout; the words follow the layout ("audio region" in Smooth, "sample" in Tracker).
// The engine is engine/SourceRender.h; each placement is one undo step. In a collaboration the
// render runs here, its file reaches the others like recorded audio, and they see the source as
// busy ("Rendering...") until it lands.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/Types.h"
#include "../engine/SourceRender.h"

namespace gl {
class App;
}

namespace gl::renderui {

// The rows a rack row's menu renders: the rack selection when it holds `channel`, else the row.
srcrender::Source rowsSource(const App& a, ObjectId pattern, ObjectId channel);
// The piano roll's selected notes (none selected: every note of the channel in the pattern).
srcrender::Source pianoNotesSource(const App& a);

// The body of a "Render to audio" submenu: the source in words, the quick destinations, Render
// options... and (rows) the whole pattern. True when a row was chosen.
bool menuBody(App& a, const srcrender::Source& src);
// The submenu itself (`label` null: "Render to audio"). While a collaborator renders the source it
// shows "Rendering... (name)" instead, disabled. True when a row was chosen.
bool menu(App& a, const srcrender::Source& src, const char* label = nullptr);

// The entry points: a pattern clip's (a region's) submenu (nothing for other clips), a channel's
// rows in the current pattern (the rack selection when it holds the channel), the piano roll's
// selected notes. True when a row was chosen.
bool clipMenu(App& a, ObjectId clip);
bool channelMenu(App& a, ObjectId channel);
bool notesMenu(App& a);

// Renders with the last options straight to `dest` (a background job, then one undo step).
void quick(App& a, const srcrender::Source& src, srcrender::Dest dest);
// Opens the render sheet for the source.
void openSheet(App& a, const srcrender::Source& src);
bool sheetOpen();
// Per frame (from drawDialogs): the sheet and its preview.
void draw(App& a);
// App quit: a preview still rendering stops and its worker is joined.
void shutdown();

// Collaboration: the name of the collaborator rendering this source now ("" = nobody), and what
// this computer renders (for its presence: 1 rows, 2 pattern, 3 clip; 0 = nothing).
std::string peerRendering(const App& a, const srcrender::Source& src);
void localBusy(uint32_t& kind, uint32_t& id);

// dev-input "render ..." (see RenderSheetUi.cpp). False when `cmd` is not this file's.
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace gl::renderui
