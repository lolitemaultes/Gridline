// Gridline — pieces shared by the hosted plugin wrapper pages (HostedUi.cpp)
// and the native plugin windows (PluginWindow.cpp). App-internal.
#pragma once
#include <string>
#include "PluginUI.h"

namespace gl {
class App;
namespace host {
class HostedInstance;
}

std::string hostedTargetName(App& a, const PluginTarget& t);

namespace hostedui {

// The plugin's programs, then its library presets.
int presetCount(host::HostedInstance* hi);
std::string presetName(host::HostedInstance* hi, int index);
bool selectPreset(App& a, const PluginTarget& t, host::HostedInstance* hi, int index);
// Preset last picked for this target (-1: none yet).
int& presetIndex(const PluginTarget& t);
// Processing load (refreshed four times a second).
void cpuLoad(App& a, const PluginTarget& t, host::HostedInstance* hi, float& average, float& peak);
// Generic parameter editor instead of the plugin's interface, for every
// instance of `type` (switches open windows over).
void setGenericEditor(App& a, std::string type, bool on);
// One wrapper page: 0 parameters, 1 processing, 2 settings.
void drawPage(App& a, const PluginTarget& t, int page, float width);
// Per frame (pumpHostedPlugins): editor idle, sandboxed editor windows
// followed, closed windows dropped.
void pluginWindowsIdle(App& a);
// Opens the plugin's window again once its new instance runs (sandbox or
// structural option switched).
void reopenLater(const PluginTarget& t);

}  // namespace hostedui
}  // namespace gl
