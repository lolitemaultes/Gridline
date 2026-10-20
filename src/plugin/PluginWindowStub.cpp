// Gridline — plugin build (PH-09) replacement for app/PluginWindow.cpp.
// Inside another application Gridline has no top-level windows of its own
// (no SDL): installed plugins hosted by the Gridline plugin show Gridline's
// panel (presets, parameters, processing, settings) in the workspace. Their
// own interfaces open in the standalone application.
#include <string>
#include "../app/App.h"
#include "../app/HostedUi.h"
#include "../app/HostedUiPages.h"
#include "../core/I18n.h"

namespace gl {

namespace {
bool panelShowing(App& a, const PluginTarget& t) {
  return t.isChannel() ? a.settingsChannel == t.owner : a.fxEditor == std::make_pair(t.owner, t.slot);
}
void hidePanel(App& a, const PluginTarget& t) {
  if (t.isChannel()) {
    if (a.settingsChannel == t.owner) a.settingsChannel = kNoId;
  } else if (a.fxEditor == std::make_pair(t.owner, t.slot)) {
    a.fxEditor = {kNoId, -1};
  }
}
}  // namespace

void setPluginWindowLog(bool) {}
void setPluginWindowPanel(int) {}
void setPluginWindowDevAction(const std::string&) {}
void setPluginWindowDevResize(int, int) {}
bool hostedEditorOpen(const PluginTarget&) { return false; }
void closeHostedEditor(App&, const PluginTarget&) {}
void closeAllHostedEditors(App&) {}
bool openHostedEditor(App& a, const PluginTarget&) {
  a.notify(i18n::tr("Plugin interfaces open in the Gridline application; here the plugin shows its parameters."));
  return false;
}
bool hostedEditorEvent(App&, const SDL_Event&) { return false; }
void renderHostedEditors(App&) {}
bool screenshotHostedEditor(const PluginTarget&, const std::string&) { return false; }
void hostedui::reopenLater(const PluginTarget&) {}
void hostedui::pluginWindowsIdle(App&) {}
void routePluginWindows(App&) {}
bool pluginWindowShown(App& a, const PluginTarget& t) { return panelShowing(a, t); }
void togglePluginWindow(App& a, const PluginTarget& t) {
  if (panelShowing(a, t)) {
    hidePanel(a, t);
    return;
  }
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
}
void showHostedPanel(App& a, const PluginTarget& t) {
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
}

}  // namespace gl
