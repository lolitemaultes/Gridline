// Gridline — "modulation" pack: reference-family skin helpers for the pack's
// editors. Same look as app/PluginSkin (wheels, rail sliders, LED checks,
// tab selectors, sober drop-downs, digit boxes, sober panels), but addressed
// through EditTarget so they also serve note processors (which have no
// PluginTarget). Every bound control goes through pui::dragParamItem or
// Document::setParam, so undo / MIDI link / typed values keep working.
#pragma once
#include <string>
#include <vector>
#include "../../app/PluginSkin.h"
#include "modulation_widgets.h"

namespace gl::packs::modulation::sk {

// Palettes taken from the reference panels' colour roles.
// Controller family (peak / envelope / keyboard / formula controllers).
const skin::Skin& controller();        // orange values
const skin::Skin& controllerLfo();     // green values (LFO sections)
const skin::Skin& formula();           // white values on dark rails
const skin::Skin& xyPad();             // slate panel, red values on light caps
const skin::Skin& xyzPad();            // grey panel, ice sliders
const skin::Skin& voltage();           // blue panel family
const skin::Skin& send();              // routing (cyan level / violet pan)
const skin::Skin& surface();           // control surface (dark buttons)
const skin::Skin& patcher();           // patch host (slate, amber selection)
// Note processor family: slate panel, dark title strip, per-plugin value colour.
const skin::Skin& noteFx(uint32_t fill);

// Bound controls (screen coordinates). Return true on change.
bool wheel(const EditTarget& e, int param, ImVec2 tl, float size, const skin::Skin& k, int flags = skin::WheelBigLine, ImU32 fill = 0);
// Wheel in form units with its label underneath.
bool wheelL(const EditTarget& e, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k, const char* label,
            int flags = skin::WheelBigLine, ImU32 fill = 0);
bool vslider(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, ImU32 fill = 0);
bool hslider(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, ImU32 fill = 0);
// LED check box with a caption to the right (toggle parameter).
bool check(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* text);
// Push button lit while a toggle parameter is on.
bool toggle(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* text = nullptr);
// Tab selector over a choice parameter (cells across `cols` columns; labels default to the choices).
bool selector(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const std::vector<std::string>* labels = nullptr,
              int cols = 0);
// Sober drop-down over a choice parameter.
bool menuBox(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k);
// Digit box: drag / wheel steps, double-click types (any parameter); `text` overrides the shown value.
bool digi(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* text = nullptr);

// Opens a form of formW x formH reference units at the cursor: scaled so that
// `unitsAcross` units (default formW) fill the editor width, centred, panel
// painted. Finish with F.end(0, formH).
skin::Form openForm(const EditTarget& e, const skin::Skin& k, float formW, float formH, float unitsAcross = 0);

// Drawing helpers.
// Sober panel (rounded, well colour) with its caption at the top left (form units).
void panelBox(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption = nullptr);
// Note processor title strip: large faint class word + plugin name (form units).
// J-136: `type` (when given) draws the plugin's wordmark in place of `title`.
void titleStrip(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* title,
                const char* type = nullptr);
// Small text label in form units (left-aligned unless centred).
void text(ImDrawList* dl, const skin::Form& F, float x, float y, const skin::Skin& k, const char* s, float px = 8.0f, bool centre = false,
          ImU32 col = 0);
// Labelled value read-out ("PEAK 42.0%") in an LCD.
void readout(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, const char* label, float v01);
// Sober push button (unbound).
bool button(const char* id, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* text, bool on = false);

}  // namespace gl::packs::modulation::sk
