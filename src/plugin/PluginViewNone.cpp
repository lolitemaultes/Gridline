// Gridline — plugin build without an editor (PH-09): no window system
// headers were found at build time. The plugins work; hosts show their
// generic parameter view for the macros.
#include "PluginView.h"

namespace gl::plugin {

bool viewSupported(WindowApi) { return false; }
WindowApi nativeWindowApi() { return WindowApi::X11; }
std::unique_ptr<View> createView(Instance&) { return nullptr; }
std::unique_ptr<HostWindow> createHostWindow(View&, const std::string&, const host::layout::WindowLayout&, bool, bool, Surface&, const int*, int) {
  return nullptr;
}
GlBinding currentGl() { return {}; }
void restoreGl(const GlBinding&) {}

}  // namespace gl::plugin
