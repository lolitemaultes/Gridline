// Gridline - translation hook (I18N-01). UI code passes visible English text
// through tr("..."); the active language maps it. The shared widgets
// (ui::TextButton, ui::Hint, ui::BeginSection, ui::Label, ui::Tooltip,
// ui::TabStrip, ui::Segmented, icon-button hints, form labels) translate
// their text themselves, so callers hand them plain English.
//
// Built in: "en" (English) and "pseudo" (accented, bracketed, ~30% longer:
// shows untranslated and clipped text without a real translation). Further
// languages are catalogues in assets/i18n, either gettext
//   <code>.po    msgid "Backup" / msgstr "Sicherung"   ("Language-Name:" in the header names it)
// or JSON
//   <code>.json  { "name": "Deutsch", "strings": { "Backup": "Sicherung", ... } }
// The template with every translatable string is assets/i18n/gridline.pot,
// produced by tools/i18n_extract.py. Missing or empty translations fall back
// to English; "##id" suffixes and printf conversions are never changed.
#pragma once
#include <string>
#include <vector>

namespace gl::i18n {

struct Language {
  std::string code, name;
};

std::vector<Language> available(const std::string& dir);
bool setLanguage(const std::string& code, const std::string& dir, std::string* err = nullptr);
const std::string& language();
// Translated text. The pointer stays valid for the whole run (earlier
// languages' texts are kept), so a language change mid-frame is safe.
const char* tr(const char* english);
std::string pseudo(const std::string& english);

// Marks a literal for the extractor without translating it here (arrays of
// labels handed to widgets that translate, command tables, ...).
constexpr const char* N_(const char* english) { return english; }

// Catalogue parsing (exposed for tests): msgid -> msgstr, empty msgstr dropped.
bool parsePo(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string* languageName = nullptr);

}  // namespace gl::i18n
