// Gridline — K-13: Settings > Plugins: the folders each plugin format is searched in
// (host/PluginFolders.h), scanning, and rescanning one folder.
#pragma once

namespace gl {

class App;

void pluginFoldersSettingsPage(App& a, float fieldW);
// Opens Settings on the Plugins page (the Plugin Manager's "Folders..." button).
void openPluginFolderSettings(App& a);
// Settings tab index of the Plugins page.
constexpr int kPluginSettingsTab = 12;

}  // namespace gl
