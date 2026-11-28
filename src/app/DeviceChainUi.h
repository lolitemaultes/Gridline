// Gridline - K-404: Studio inline Device chain panel.
//
// Draws the selected channel's instrument followed by its mixer track's effect slots (including
// Junction / Junction FX) in a horizontal, height-snapped scrollable row. Built-in editors scale
// crisply to fit the panel height; installed plugins show a compact card with Show its window
// (native view embedding is 0.8.0). Collapsed and off-screen devices execute zero editor draw calls.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;

namespace devchainui {

// Draws the Studio inline Device chain inside the current window/child region.
void draw(App& a);

// Dev/test commands (`parts chain stats|collapse|expand|add|remove|move|bypass|scroll ...`).
// Returns true if the command was handled.
bool devCommand(App& a, const std::vector<std::string>& args);

}  // namespace devchainui
}  // namespace gl
