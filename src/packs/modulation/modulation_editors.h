// Gridline — "modulation" pack: editor entry points shared by the pack's UI files.
#pragma once
#include "modulation_widgets.h"

namespace gl::packs::modulation {

// Controllers (modulation_ctl_ui.cpp).
void peakEditor(EditTarget& e);
void xyEditor(EditTarget& e);
void xyzEditor(EditTarget& e);
void formulaEditor(EditTarget& e);
void envEditor(EditTarget& e);
void envArtEditor(EditTarget& e);  // articulator model (modulation_envart_ui.cpp)
void keyEditor(EditTarget& e);
void cvEditor(EditTarget& e);
// Routing / surface / sidechain (modulation_route_ui.cpp).
void surfaceEditor(EditTarget& e);
void sendEditor(EditTarget& e);
void busEditor(EditTarget& e, bool send);
void duckEditor(EditTarget& e);
// Patch host (modulation_patcher_ui.cpp).
bool drawPatchHostEditor(EditTarget& e);

}  // namespace gl::packs::modulation
