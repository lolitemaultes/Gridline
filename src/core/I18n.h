// Gridline — translation hook (I18N-01, first step). UI code wraps visible
// English text in tr("…"); the active language maps it. Built in: "en"
// (English) and "pseudo" (accented, bracketed, ~30% longer: shows untranslated
// and clipped text without a real translation). Further languages are JSON
// catalogues <dir>/<code>.json:
//   { "name": "Deutsch", "strings": { "Audio": "Audio", "Backup": "Sicherung", … } }
// Missing strings fall back to English. "##id" suffixes and printf
// conversions are never changed.
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
// Translated text; the pointer stays valid until the language changes.
const char* tr(const char* english);
std::string pseudo(const std::string& english);

}  // namespace gl::i18n
