// Gridline — settings pages added by J-24 (ST-02..08): General (startup,
// default template, language), Files (folders, scripting), Debug (log and
// script console), plus extra panels for the Project, Backup and Interface
// pages (time-signature / author defaults, autosave and backup restore, hint
// timing). Drawn inside the settings window (Dialogs.cpp).
#pragma once

namespace gl {

class App;
namespace ui {
enum class Icon;
}

void generalSettingsPage(App& a, float fieldW);
void fileSettingsPage(App& a, float fieldW);
void debugSettingsPage(App& a, float fieldW);
void projectDefaultsPanel(App& a);
// GitHub #74: Settings > Project: the colour new channels and patterns take; and, once a frame, that
// choice (with the selected channel's colour) handed to the document.
void newColorsPanel(App& a, float fieldW);
void publishNewColors(App& a);
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
// J-137 dev check (--settings-size WxH): the settings window at this size
// (logical px; still bounded by the window's minimum), for layout checks.
void setDevSettingsSize(float w, float h);
// K-93 (Dialogs.cpp): the settings pages (tab order), their English names, and one page's
// controls drawn into the current window (the command palette reads every setting from them).
int settingsPageCount();
const char* settingsPageNameEn(int page);
void drawSettingsPage(App& a, int page, float fieldW);
// K-256: the Layouts page (LayoutSettingsUi.cpp): each layout's control bar, panels, workspaces, pinned
// commands and switching; Studio's zone keys (K-340).
constexpr int kLayoutsSettingsTab = 13;
void layoutsSettingsPage(App& a, float fieldW);
// The layout the page shows next time (View > Window layout > Layout settings, the bar list's More).
void showLayoutsPageFor(int layout);

// K-325 (Dialogs.cpp): the Settings window's frame, shared with the other paged windows (the
// Record window), so they look the same in every layout.
enum class SettingsNav { Tabs, List, Icons };  // Studio and Tracker: tabs across the top; Clips: a list down the left; Smooth: icon tiles
SettingsNav settingsNav(App& a);
// The pages' navigation for the layout. A list continues on the same line (the page beside it);
// `reserveH` is the room the list leaves under it (a footer). Labels are English (translated here).
void settingsPageNav(App& a, const char* tabsId, const char* const* labelsEn, const ui::Icon* icons, int count, int* page,
                     const char* const* hints = nullptr, float reserveH = 0.0f);
// Smooth: the page's inset panel, from `barTop` (the cursor's y under the tiles) to `reserveH` above
// the window's content bottom. Call with the window's draw list split in two (channel 1 current).
void settingsPagePanel(float barTop, float reserveH);
// The page form's columns and field width (Live's value column, Smooth's right-aligned labels).
float settingsFormBegin(App& a);
void settingsFormEnd();

}  // namespace gl
