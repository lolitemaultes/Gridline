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
// J-55 (SettingsKeysTheme.cpp): keyboard shortcut editor, theme page, and the
// read-only shortcut list of Help > Keyboard shortcuts.
void shortcutSettingsPage(App& a, float fieldW);
void themeSettingsPage(App& a, float fieldW);
// J-129: the Theme page's Sounds group (Appearance / Controls / Sounds): the
// interface sounds and the sound theme (J-133).
void themeSoundsGroup(App& a, float fieldW);
void shortcutReference(App& a);

}  // namespace gl
