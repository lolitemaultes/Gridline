// Gridline — K-107: the Speech dialog (channel rack > Add > Speech...): text, a voice and its
// settings, a preview, and Add, which renders the speech to a file in the Speech folder and
// adds a sampler channel playing it.
#pragma once
#include <string>

namespace gl {
class App;
namespace speechui {

void open(App& a);
void draw(App& a);  // per frame (Dialogs)
// (dev) speech [<style> ["<text>"]] [preview]: opens the dialog with that style and text, previewing when asked.
void devSet(const std::string& style, const std::string& text, bool preview);

}  // namespace speechui
}  // namespace gl
