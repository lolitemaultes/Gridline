// Gridline — K-107: the Speech dialog (channel rack > Add > Speech...): text, a voice and its
// settings, a preview, and Add, which renders the speech to a file in the Speech folder and
// adds a sampler channel playing it.
#pragma once

namespace gl {
class App;
namespace speechui {

void open(App& a);
void draw(App& a);  // per frame (Dialogs)

}  // namespace speechui
}  // namespace gl
