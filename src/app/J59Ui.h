// Gridline — J-59 windows: the loop starter (GEN-01, File > New from loop
// starter, also offered in the new-project browser) and render to sampled
// instrument (RN-08, channel menu and Tools menu).
#pragma once
#include "../core/Types.h"

namespace gl {

class App;

namespace j59 {

void openLoopStarter(App& a);
// Creates the loop starter's project now (its window open; dev checks).
void createFromLoopStarter(App& a, int style);
// The render window for `channel`; it offers the piano roll's selected notes
// when they belong to that channel.
void openRenderSampler(App& a, ObjectId channel);
void drawDialogs(App& a);  // per frame (from drawDialogs): both windows
void drawLoopStarter(App& a);
void drawRenderSampler(App& a);
// J-103 app quit: a running render to sampler is cancelled and its worker joined. Idempotent.
void shutdownRenderSampler();

}  // namespace j59
}  // namespace gl
