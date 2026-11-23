// Gridline - the Tracker layout's lower-band editors (K-106 cp18, K-316 plan sections 3, 5, 6).
//
// The lower band under the pattern (region tracker.lower, the Device chain's place in the other
// layouts) shows one screen at a time for the current instrument, as the classic tracker swaps its
// bottom band:
//   * a Tracker Sampler (gl.tracker): the Sample editor (waveform, range, loop pins, the edits, the
//     play buttons; cp20: right-drag draws with a pencil, one undo step) or the Instrument editor (cp19: the volume and panning envelopes with their
//     presets, the selected sample's volume / pan / fine tune / relative note, the fadeout and the
//     auto vibrato, the key map on a piano; the plugin's own editor no longer stands in for it;
//     cp20: a play-position line on each envelope while notes sound, the "Extra" page with the
//     instrument's MIDI out and playback settings, fewer and wider piano keys in short bands);
//   * any other instrument: the Channel frame (volume, pan, pitch, fine, mute, output, key range,
//     Open plugin window, and for the plain Sampler Convert to Tracker Sampler...).
// Every edit goes through the Tracker Sampler's session ops (packs/tracker/tracker_session.h) or
// the Document calls the rack and the channel settings use: the same undo steps, the same saved
// bytes in every layout, the same collaboration ops. Which screen shows, the view, the range and
// the play note are view state, never saved.
#pragma once
#include <string>
#include <vector>

#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::trackereditors {

enum class Editor { Instrument = 0, Sample = 1 };

// Opens the screen for `channel` (it becomes the current instrument). K-391: only the Tracker's own
// screens show in the band (a Tracker Sampler's Sample and Instrument editors, a Sampler's Sample
// editor, Seltmann's Chop and Instrument screens); for any other `e` the instrument's editor floats
// in its window instead (openPluginWindow), as in Studio. `slot` >= 0: that sample selected.
// `keys`: the screen takes the keyboard (opened by its key); a click leaves the keys where they were.
void open(App& a, Editor e, ObjectId channel, int slot = -1, bool keys = false);
// K-391: the band shows a Tracker screen for the channel settings' channel (they do not float).
bool bandHolds(App& a);
// K-391: another instrument picked while the band shows a screen: the same screen for it when it has
// one, else the band closes (nothing floats up from a pick).
void follow(App& a, ObjectId channel);
// K-391: Convert to Tracker Sampler... for a Sampler with a file (the instrument list's menu): its
// confirm, then the new Tracker Sampler's Sample editor. One undo step.
void askConvert(App& a, ObjectId channel);
// The screen asked for again while it shows: closed; otherwise opened (the current instrument).
void toggle(App& a, Editor e, bool keys = false);
// Closed (Exit, Esc, Cmd/Ctrl+W, the lit button, Ctrl+Alt+X): the keys go back to the pattern.
void close(App& a);
// The lower band shows a Tracker screen (any of them).
bool shown(App& a);
// The screen asked for last (the band may show the Channel frame for another kind of instrument).
Editor wanted();
// The header's buttons light: `e` shows now for the current instrument.
bool showing(App& a, Editor e);
// The Sample button and Ctrl+Alt+E reach a Sample editor for this channel (a Tracker Sampler; 0.7.8:
// a Sampler too, whose Sample editor holds the Audio clip window: ClipEditor.h, trackerpage).
bool hasSampleEditor(App& a, ObjectId channel);
// 0.7.8: the band shows a Sampler's Sample editor (the Audio clip window's page) now.
bool showsClipForm(App& a);

// "Open plugin window" (the instrument menu, a double-click, Channel settings, the Instrument button
// for an instrument with no Tracker screen): the plugin's own editor in its own floating window,
// never restyled (an installed plugin's own interface; a built-in's channel settings window; a
// Sampler's Audio clip window), as in Studio.
void openPluginWindow(App& a, ObjectId channel);
// K-391: in the Tracker the channel settings float (they are open and the band is not holding them).
bool pluginWindowOpen(App& a);
// cp22: true while the Tracker's band holds the channel settings' channel for its own screen (or
// none is open): the shared channel settings window (and an installed plugin's own window routed
// from them) stays closed then (no frame of it, not even one).
bool ownsChannelWindow(App& a);
// cp22 (dev): once a frame, before the channel settings window: counts the frames a plugin or channel
// window showed in the Tracker that "Open plugin window" did not ask for ("tracker editors windows").
void watchWindows(App& a, bool blocked);

// The Device chain part's body in the Tracker: draws the screen for the instrument its chain shows.
// False when the Tracker does not show its own screen there (the caller draws the chain).
bool host(App& a, ObjectId channel);
// The height the shown screen wants (0: none of ours; the band then measures the chain's body).
float need(App& a);
// Once a frame from the header band: a preview whose screen is gone ends.
void tend(App& a);
// cp24: the Channel frame folds to its title and first row. "tracker.frameFold" holds the person's
// choice (0: by the window, 1: folded, 2: whole). By the window, the header band says each frame how
// much height the frame may have while the pattern keeps eight rows under its cursor (`room`); the
// frame folds when its whole height does not fit there.
void setFrameRoom(float room);
// The whole Channel frame's height, the band's padding included (once it has been drawn).
float frameWholeNeed();
bool frameFolded(App& a);
// The fold button: folded <-> whole.
void toggleFrameFold(App& a);

// Keys: the screen had the keyboard on the last frame (its window focused).
bool hasKeys();
// main.cpp's SDL key events, before the pattern's: true when the screen takes the key.
bool sdlKey(App& a, int scancode, int keycode, int mods, bool down, bool repeat);
// Cmd+W reaching Gridline as the window's close request (macOS): the screen closes (true).
bool closeRequest(App& a);

// Dev (--dev-input "tracker editors ..."):
//   open sample|instrument|frame [channel index or name] [slot] · close · focus · plugin [channel index or name] · load <file>
//   dump · geometry · range <a> <b> · pin <0..3> <frame> · view <start> <len>
//   zoom in|out|all|range|at <frame> <in|out> · scroll <frames>
//   op <cut|copy|paste|crop|delete|silence|reverse|normalise|fadein|fadeout|fixdc|minimise|selectall|clearrange>
//   loop off|forward|pingpong · bits 8|16 · note <n> · play wave|range|view|stop · step <+1|-1>
//   modal volume|resample|echo|mix|loopfade [values...] (applies) · remove · loopstart <d> · looplen <d>
//   frame volume|pan|pitch|fine|mute|output|keylo|keyhi <value> · convert
//   cp19, the Instrument editor: ins dump · ins at key <0..95> · ins at env vol|pan <tick> <level> (the pointer there)
//   ins env vol|pan add <x> <y>|move <i> <x> <y>|end|delete <i>|addafter <i>|sustain <i>|loop <a> <b>|flag on|sustain|loop|preset <k>|sel <i>
//   ins keys <lo> <hi> · ins everykey · ins away · ins set volume|pan|fine|rel|fadeout|rate|depth|sweep|shape <v>
//   cp20: pencil <frame> <value> <frame> <value> ... (a right-drag's stroke) · ins extra on|off (the Extra page)
//   · ins extra set interp|amiga|rpb|ticks|level|ramp|sound <value> (as its controls set them);
//   "ins dump" also prints envlines <vol> <pan> (the play-position lines drawn on the envelopes)
void devCommand(App& a, const std::vector<std::string>& args, int frame);
// cp19: the pointer a dev command asked for ("ins at ..."), taken once by main's scripted input
// (which keeps its own pointer between commands). False: none asked.
bool takeDevPointer(float* x, float* y);
// TRK-1118 (dev): the instrument list's right-click menu notes each row's place by its English label as it
// draws it; "tracker editors menu at <label>" puts the pointer on that row (then "sdldown 0" / "sdlup 0"),
// so a script finds a row by what it says, not where it was.
void devMenuRow(const char* label, float x0, float y0, float x1, float y1);

}  // namespace gl::trackereditors
