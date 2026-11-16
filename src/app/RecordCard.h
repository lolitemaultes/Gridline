// Gridline — K-123: the Recording card. Clicking the record button on opens a small card at the
// bottom right of the window: ready (the armed tracks with their input levels, Record and play)
// or recording (the time, the mode and pass, the take, disk time left, levels, Stop and Discard
// this take). H (with the typing keyboard off) or its fold button hides it to a small REC pill
// while recording; the pill opens it again. Settings: record.window (when the card opens: 0
// always, 1 the first time per song, 2 never), record.form (how they show: 0 the card, 1 a strip
// under the top bar, 2 the card, and the Record window stays open while recording). Shift+click on the record button and the record
// key never open it.
#pragma once
#include <string>
#include "App.h"

namespace gl::recordcard {

// The record button was clicked (after it switched): opens the card when record.window says so.
void recordClicked(App& a, bool shift);
void open(App& a);
// The Record window itself (Record window... in the record options menu).
void openWindow(App& a);
void close();
bool isOpen();
bool isHidden();  // folded to the REC pill
// Per frame: the card (or the pill), and its closing when the record button is off.
void draw(App& a);
// K-123: with record.form = strip, the record strip under the top bar at `top` (px below the work
// area's top). Returns its height (0 when it does not show); the dock space starts below it.
float drawStrip(App& a, float top);

}  // namespace gl::recordcard
