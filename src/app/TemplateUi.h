// Gridline — template windows (TP-01): File > New from template... (a
// template browser with categories, preview text, project info and time
// settings) and File > Save as template... (name, category, description).
#pragma once

namespace gl {

class App;

void drawTemplateDialogs(App& a);  // per frame (from drawDialogs)

}  // namespace gl
