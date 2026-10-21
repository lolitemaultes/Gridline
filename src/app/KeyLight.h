// Gridline - lit keys and pad flashes (J-77): per-channel light state read
// from the engine once per frame (see KeyLightState.h for the curves).
#pragma once
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"
#include "KeyLightState.h"

namespace gl {
class App;
}

namespace gl::keylight {

// The channel's lights this frame (nullptr: no such channel / instrument).
// Updated at most once per ImGui frame, however many views ask.
const ChannelLights* lights(App& a, ObjectId channel);
// Key light 0..1 of `key` on `channel` (0 when not sounding / faded).
float key(App& a, ObjectId channel, int key);
// Pad flash 0..1 of the pad played by `key` on `channel`.
float flash(App& a, ObjectId channel, int key);
// Brightest pad flash over every key (single-voice drums: any key strikes).
float flashAny(App& a, ObjectId channel);
// Draws a pad flash over [p0, p1]: a bright wash plus a rim, `level` 0..1.
void drawFlash(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float level, float rounding);

}  // namespace gl::keylight
