// Gridline — settings pages added by J-24 (ST-02..08): General (startup,
// default template, language), Files (folders, scripting), Debug (log and
// script console), plus extra panels for the Project, Backup and Interface
// pages (time-signature / author defaults, autosave and backup restore, hint
// timing). Drawn inside the settings window (Dialogs.cpp).
#pragma once

namespace gl {

class App;

void generalSettingsPage(App& a, float fieldW);
void fileSettingsPage(App& a, float fieldW);
void debugSettingsPage(App& a, float fieldW);
void projectDefaultsPanel(App& a);
void backupRestorePanel(App& a, float fieldW);
void hintSettingsPanel(App& a, float fieldW);

}  // namespace gl
