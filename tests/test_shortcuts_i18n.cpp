// Gridline - J-55: the central keymap (default map, conflicts, shadowing,
// rebinding, persistence through the settings, reset) and the translation
// layer (catalogue lookup and fallback, .po / JSON loading, pseudo locale,
// pointer stability) plus i18n coverage of the sources: every tr() / N_()
// literal is in assets/i18n/gridline.pot, and menus / buttons hold no
// hard-coded English (runs from the source directory).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "../src/core/I18n.h"
#include "../src/core/Keymap.h"
#include "../src/core/Settings.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
  std::ifstream is(p, std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

void writeFile(const fs::path& p, const std::string& s) {
  std::ofstream os(p, std::ios::binary);
  os << s;
}

bool has(const std::vector<const keys::Command*>& v, const char* id) {
  return std::any_of(v.begin(), v.end(), [&](const keys::Command* c) { return !std::strcmp(c->id, id); });
}

// ----------------------------------------------------------------- keymap ----
void keymapTests() {
  using keys::Chord;
  using keys::Key;
  // Chord text round trip.
  expect(keys::format(keys::parse("ctrl+shift+s")) == "Ctrl+Shift+S", "parse/format: modifier order and case");
  expect(keys::format(keys::parse("Shift+Alt+Ctrl+F4")) == "Ctrl+Alt+Shift+F4", "parse/format: Ctrl, Alt, Shift order");
  expect(keys::parse("Num +").key == Key::NumAdd && keys::parse("Ctrl+Num +").mods == keys::kCtrl, "parse: keypad keys with a plus in the name");
  expect(!keys::parse("Ctrl+Banana").valid() && keys::parse("Ctrl+Banana").mods == 0, "parse: unknown key is invalid");
  expect(keys::parse("Delete").key == Key::Delete && keys::parse("PageUp").key == Key::PageUp, "parse: long spellings");
  for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i) {
    Chord c{static_cast<Key>(i), static_cast<uint8_t>(i % 8)};
    if (keys::parse(keys::format(c)) != c) {
      expect(false, ("round trip of " + keys::format(c)).c_str());
      break;
    }
  }
  expect(true, "every key name round-trips");

  // The default map: FL's defaults, unique ids, no conflicts inside a context.
  keys::Keymap km;
  std::set<std::string> ids;
  bool unique = true, labelled = true;
  for (const auto& c : keys::commands()) {
    unique &= ids.insert(c.id).second;
    labelled &= c.label && *c.label && !c.defaults.empty();
  }
  expect(unique, "command ids are unique");
  expect(labelled, "every command has a label and a default chord");
  expect(keys::commands().size() >= 140, "the registry covers the app (menus, global keys, every editor)");
  const auto conflicts = km.allConflicts();
  for (const auto& c : conflicts) std::printf("# default conflict: %s\n", c.c_str());
  expect(conflicts.empty(), "default map has no same-context conflicts");
  expect(keys::format(km.bindings("file.save").front()) == "Ctrl+S", "default: Save is Ctrl+S");
  expect(km.bindings("edit.redo").size() == 2, "default: Redo has Ctrl+Y and Ctrl+Shift+Z");
  expect(keys::format(km.bindings("view.mixer").front()) == "F9", "default: Mixer is F9");
  expect(keys::format(km.bindings("piano.toolDraw").front()) == "P", "default: piano roll draw tool is P");
  expect(keys::format(km.bindings("playlist.toolSlip").front()) == "S", "default: playlist slip tool is S");
  expect(keys::format(km.bindings("rack.graphEditor").front()) == "Ctrl+K", "default: rack graph editor is Ctrl+K");
  expect(keys::format(km.bindings("mixer.solo").front()) == "S", "default: mixer solo is S");

  // Contexts: piano roll Ctrl+Q shadows the global Quit; same key in two editors is fine.
  const Chord ctrlQ = keys::parse("Ctrl+Q");
  expect(km.conflicts("file.quit", ctrlQ).empty(), "global Quit has no same-context conflict");
  expect(has(km.shadows("file.quit", ctrlQ), "piano.quickQuantize"), "global Quit is shadowed by the piano roll's Ctrl+Q");
  expect(has(km.shadows("piano.quickQuantize", ctrlQ), "file.quit"), "the shadow is reported from the editor side too");
  expect(km.shadows("playlist.toolSlip", keys::parse("S")).empty(), "editor to editor never shadows (playlist S vs mixer S)");
  expect(has(km.lookup(keys::Context::PianoRoll, ctrlQ), "piano.quickQuantize") && km.lookup(keys::Context::PianoRoll, ctrlQ).size() == 1,
         "lookup by context and chord");

  // Rebinding with conflict detection.
  const Chord ctrlS = keys::parse("Ctrl+S");
  expect(has(km.conflicts("file.open", ctrlS), "file.save"), "Ctrl+S for Open conflicts with Save");
  auto lost = km.assign("file.open", ctrlS);
  expect(lost.size() == 1 && lost[0] == "file.save", "assign reports the command that lost the chord");
  expect(km.bindings("file.save").empty() && km.bindings("file.open").size() == 1 && km.bindings("file.open")[0] == ctrlS,
         "assign moves the chord (Save unbound, Open = Ctrl+S)");
  expect(km.customized("file.save") && km.customized("file.open"), "both commands count as customized");
  expect(km.allConflicts().empty(), "no conflicts after a reassign");
  km.assign("edit.undo", keys::parse("Alt+Backspace"), false);
  expect(km.bindings("edit.undo").size() == 2, "assign without replace adds an alternative chord");
  expect(!km.set("no.such.command", {ctrlS}), "unknown ids are refused");
  km.set("view.mixer", {keys::parse("F9")});
  expect(!km.customized("view.mixer"), "setting the default chords is not an override");

  // Persistence through the settings (overrides only).
  const std::string json = km.serialize();
  expect(json.find("file.open") != std::string::npos && json.find("view.mixer") == std::string::npos, "serialize writes overrides only");
  Settings st;
  expect(st.getString("keys.bindings").empty(), "settings: no overrides by default");
  expect(st.set("keys.bindings", json), "settings: keys.bindings is a registered string");
  const fs::path dir = fs::temp_directory_path() / "gridline_j55_keys";
  fs::create_directories(dir);
  const std::string path = (dir / "settings.json").string();
  expect(st.save(path), "settings saved");
  Settings st2;
  expect(st2.load(path), "settings loaded");
  keys::Keymap km2;
  expect(km2.load(st2.getString("keys.bindings")), "keymap loads from the settings");
  expect(km2.bindings("file.open").size() == 1 && km2.bindings("file.open")[0] == ctrlS, "persisted: Open = Ctrl+S");
  expect(km2.bindings("file.save").empty() && km2.customized("file.save"), "persisted: Save stays unbound");
  expect(km2.bindings("edit.undo").size() == 2, "persisted: Undo keeps its alternative");
  expect(km2.serialize() == json, "load + serialize is stable");
  expect(km2.load("{\"file.new\": [\"Ctrl+Nonsense\", \"Alt+N\"], \"gone.command\": [\"F2\"], \"edit.history\": 5}"),
         "unknown ids, bad chords and wrong types are skipped");
  expect(km2.bindings("file.new").size() == 1 && keys::format(km2.bindings("file.new")[0]) == "Alt+N", "the valid chord survives");
  expect(!km2.load("not json"), "invalid JSON is reported");
  expect(!km2.anyCustomized() && keys::format(km2.bindings("file.save").front()) == "Ctrl+S", "invalid JSON leaves the defaults");

  // Reset.
  const uint32_t v0 = km.version();
  km.reset("file.save");
  expect(keys::format(km.bindings("file.save").front()) == "Ctrl+S" && km.version() != v0, "reset one command");
  expect(!km.allConflicts().empty(), "resetting Save while Open holds Ctrl+S is a visible conflict");
  km.resetAll();
  expect(!km.anyCustomized() && km.allConflicts().empty() && km.serialize().empty(), "reset all = the default map");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------- i18n ----
void i18nTests() {
  const fs::path dir = fs::temp_directory_path() / "gridline_j55_i18n";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  writeFile(dir / "de.po",
            "# test catalogue\nmsgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Language-Name: Deutsch\\n\"\n\n"
            "#: src/app/Menus.cpp\nmsgid \"Save\"\nmsgstr \"Speichern\"\n\n"
            "msgid \"Open recent\"\nmsgstr \"\"\n\n"
            "msgid \"%d notes copied\"\nmsgstr \"%d Noten kopiert\"\n\n"
            "msgid \"Say \\\"hi\\\"\"\nmsgstr \"Sag \\\"hallo\\\"\"\n\n"
            "msgid \"Long \"\n\"text\"\nmsgstr \"Langer \"\n\"Text\"\n\n"
            "msgid \"Playlist\"\nmsgstr \"Wiedergabeliste\"\n\n"
            "msgid \"Export failed\"\nmsgstr \"Export fehlgeschlagen\"\n\n"
            "msgid \"Saved\"\nmsgstr \"Gespeichert\"\n\n"
            "msgid \"Render cancelled\"\nmsgstr \"Rendern abgebrochen\"\n");
  writeFile(dir / "fr.json", "{ \"name\": \"Fran\xC3\xA7" "ais\", \"strings\": { \"Save\": \"Enregistrer\", \"Undo\": \"\" } }");
  writeFile(dir / "gridline.pot", "msgid \"Save\"\nmsgstr \"\"\n");
  writeFile(dir / "broken.json", "{ nope");

  auto langs = i18n::available(dir.string());
  auto name = [&](const char* code) {
    for (const auto& l : langs)
      if (l.code == code) return l.name;
    return std::string();
  };
  expect(langs.size() == 4, "available: English, Pseudo, de.po, fr.json (template and broken files skipped)");
  expect(name("en") == "English" && name("pseudo").find("Pseudo") == 0, "built-in languages");
  expect(name("de") == "Deutsch", "po: Language-Name from the header");
  expect(name("fr") == "Fran\xC3\xA7" "ais", "json: name field");

  expect(i18n::setLanguage("en", dir.string()) && i18n::language() == "en", "English");
  const char* save = "Save";
  expect(i18n::tr(save) == save, "English returns the same pointer (no copy)");

  expect(i18n::setLanguage("de", dir.string()), "load de.po");
  expect(!std::strcmp(i18n::tr("Save"), "Speichern"), "po lookup");
  expect(!std::strcmp(i18n::tr("Save##menu"), "Speichern##menu"), "##id suffix kept");
  expect(!std::strcmp(i18n::tr("Open recent"), "Open recent"), "empty msgstr falls back to English");
  expect(!std::strcmp(i18n::tr("Not in the catalogue"), "Not in the catalogue"), "missing string falls back to English");
  expect(!std::strcmp(i18n::tr("%d notes copied"), "%d Noten kopiert"), "printf format translated with its conversion");
  expect(!std::strcmp(i18n::tr("Say \"hi\""), "Sag \"hallo\""), "po escapes");
  expect(!std::strcmp(i18n::tr("Long text"), "Langer Text"), "po multi-line strings");
  const char* kept = i18n::tr("Save");
  // J-61: window titles keep a stable English id after "###" (ImGui hashes
  // only what follows "###", so layouts and lookups by name still match).
  expect(!std::strcmp(i18n::tr("Playlist###Playlist"), "Wiedergabeliste###Playlist"), "window title: translated label, stable ### id");
  // Notifications built at run time.
  expect(i18n::trMessage("Render cancelled") == "Rendern abgebrochen", "notify: whole message translated");
  expect(i18n::trMessage("Export failed: disk full") == "Export fehlgeschlagen: disk full", "notify: leading part before ': ' translated");
  expect(i18n::trMessage("Saved song.wav") == "Gespeichert song.wav", "notify: leading words translated");
  expect(i18n::trMessage("Something else entirely") == "Something else entirely", "notify: unknown text unchanged");

  expect(i18n::setLanguage("fr", dir.string()) && !std::strcmp(i18n::tr("Save"), "Enregistrer"), "json lookup");
  expect(!std::strcmp(i18n::tr("Undo"), "Undo"), "json: empty translation falls back");
  expect(!std::strcmp(kept, "Speichern"), "pointers from an earlier language stay valid");

  std::string err;
  expect(!i18n::setLanguage("xx", dir.string(), &err) && !err.empty() && i18n::language() == "en", "missing language: error, English");
  expect(!std::strcmp(i18n::tr("Save"), "Save"), "after a failed load the text is English");

  expect(i18n::setLanguage("pseudo", dir.string()), "pseudo locale");
  const std::string p = i18n::tr("Open project");
  expect(p.front() == '[' && p.find(']') != std::string::npos && p.size() > std::strlen("Open project") * 13 / 10, "pseudo: bracketed and ~30% longer");
  expect(p.find("\xC3\xB6") != std::string::npos, "pseudo: accented letters");
  const std::string fmt = i18n::tr("%d of %s##x");
  expect(fmt.find("%d") != std::string::npos && fmt.find("%s") != std::string::npos && fmt.size() > 3 && fmt.compare(fmt.size() - 3, 3, "##x") == 0,
         "pseudo: printf conversions and ids unchanged");
  expect(std::string(i18n::tr(i18n::tr("Mixer"))) == i18n::tr("Mixer"), "pseudo: translating twice changes nothing");
  expect(std::string(i18n::tr("##id")) == "##id", "pseudo: bare ids unchanged");
  expect(i18n::trMessage("Saved x.wav").front() == '[', "pseudo: notifications go through the translation layer");
  i18n::setLanguage("en", dir.string());
  expect(i18n::trMessage("Saved x.wav") == "Saved x.wav", "English: notifications unchanged");
  fs::remove_all(dir, ec);
}

// --------------------------------------------------------------- coverage ----
// C string literal body -> bytes.
std::string unescapeC(const std::string& b) {
  std::string out;
  for (size_t i = 0; i < b.size(); ++i) {
    if (b[i] != '\\' || i + 1 >= b.size()) {
      out += b[i];
      continue;
    }
    char n = b[++i];
    if (n == 'x') {
      size_t j = i + 1;
      int v = 0;
      while (j < b.size() && std::isxdigit(static_cast<unsigned char>(b[j]))) v = v * 16 + std::stoi(std::string(1, b[j++]), nullptr, 16);
      out += static_cast<char>(v & 0xFF);
      i = j - 1;
    } else if (n >= '0' && n <= '7') {
      int v = 0, k = 0;
      size_t j = i;
      while (j < b.size() && k < 3 && b[j] >= '0' && b[j] <= '7') v = v * 8 + (b[j++] - '0'), ++k;
      out += static_cast<char>(v & 0xFF);
      i = j - 1;
    } else {
      out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
    }
  }
  return out;
}

bool letters2(const std::string& s) {
  static const std::regex conv(R"(%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?[hlLzjt]*[diouxXeEfgGcspn%])");
  std::string t = std::regex_replace(s.substr(0, s.find("##")), conv, "");
  return std::count_if(t.begin(), t.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }) >= 2;
}

std::vector<fs::path> uiSources() {
  std::vector<fs::path> v;
  for (const auto& e : fs::directory_iterator("src/app"))
    if (e.path().extension() == ".cpp" || e.path().extension() == ".h") v.push_back(e.path());
  for (const auto& d : fs::directory_iterator("src/packs"))
    if (d.is_directory())
      for (const auto& e : fs::directory_iterator(d.path()))
        if (e.path().filename().string().find("_ui.cpp") != std::string::npos) v.push_back(e.path());
  v.push_back("src/core/Keymap.cpp");
  for (const auto& e : fs::directory_iterator("src/collab"))  // J-111
    if (e.path().extension() == ".cpp") v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}

void coverageTests() {
  if (!fs::exists("assets/i18n/gridline.pot") || !fs::exists("src/app/Menus.cpp")) {
    expect(false, "coverage: run from the gridline source directory (assets/i18n/gridline.pot)");
    return;
  }
  std::vector<std::pair<std::string, std::string>> entries;
  expect(i18n::parsePo(readFile("assets/i18n/gridline.pot"), entries), "template parses");
  // parsePo drops empty msgstr: read the msgids directly.
  std::set<std::string> ids;
  {
    std::istringstream in(readFile("assets/i18n/gridline.pot"));
    std::string line;
    while (std::getline(in, line))
      if (line.rfind("msgid \"", 0) == 0) {
        std::string body = line.substr(7, line.size() - 8), out;
        for (size_t i = 0; i < body.size(); ++i) {
          if (body[i] == '\\' && i + 1 < body.size()) {
            char n = body[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
          } else {
            out += body[i];
          }
        }
        if (!out.empty()) ids.insert(out);
      }
  }
  expect(ids.size() > 1500, "template holds the interface (menus, dialogs, settings, hints)");
  for (const char* must : {"File", "Save", "Open recent", "Keyboard shortcuts", "Follow system", "Reset all", "Piano roll", "Draw tool",
                           "Theme", "Shortcuts", "Display language"})
    expect(ids.count(must) == 1, (std::string("template contains \"") + must + "\"").c_str());

  // Every tr() / N_() literal is in the template (the template is up to date).
  const std::regex trCall(R"((?:\btr|\bN_)\(\s*"((?:[^"\\]|\\.)*)\"\s*\))");
  // Raw English in menus, buttons and lists (must go through tr()).
  const std::regex rawWidget(R"(ImGui::(MenuItem|BeginMenu|SeparatorText|Selectable|Checkbox|RadioButton|Button|SmallButton|TableSetupColumn|CollapsingHeader)\(\s*"((?:[^"\\]|\\.)*)\")");
  const std::regex rawTernary(R"(ImGui::(MenuItem|BeginMenu|SeparatorText|Selectable|Checkbox|RadioButton|Button|SmallButton)\([^;"]*\?\s*"((?:[^"\\]|\\.)*)\")");
  std::vector<std::string> missing, raw;
  size_t literals = 0;
  for (const auto& f : uiSources()) {
    std::istringstream in(readFile(f));
    std::string line;
    int no = 0;
    while (std::getline(in, line)) {
      ++no;
      const size_t cmt = line.find("//");
      const std::string code = cmt == std::string::npos ? line : line.substr(0, cmt);
      for (std::sregex_iterator it(code.begin(), code.end(), trCall), end; it != end; ++it) {
        std::string t = unescapeC((*it)[1].str());
        if (!letters2(t) || t.rfind("##", 0) == 0) continue;
        t = t.substr(0, t.find("##"));
        ++literals;
        if (!ids.count(t)) missing.push_back(f.string() + ":" + std::to_string(no) + " \"" + t + "\"");
      }
      for (const auto* re : {&rawWidget, &rawTernary})
        for (std::sregex_iterator it(code.begin(), code.end(), *re), end; it != end; ++it) {
          const std::string t = (*it)[2].str();
          if (t.rfind("##", 0) == 0 || !letters2(unescapeC(t))) continue;
          raw.push_back(f.string() + ":" + std::to_string(no) + " " + (*it)[1].str() + "(\"" + t + "\")");
        }
    }
  }
  for (size_t i = 0; i < missing.size() && i < 20; ++i) std::printf("# not in gridline.pot: %s\n", missing[i].c_str());
  if (!missing.empty()) std::printf("# run: python3 tools/i18n_extract.py\n");
  expect(literals > 1000, "the sources route their text through tr() / N_()");
  expect(missing.empty(), "every tr() / N_() literal is in assets/i18n/gridline.pot");

  // J-61: window titles, notifications and canvas labels reach the catalogue.
  for (const char* must : {"Playlist", "Mixer", "Channel rack", "Piano roll", "Browser", "Master chain", "Live pads", "Undo history",
                           "Settings", "Performance", "Render", "Render cancelled", "Export failed", "Empty slot", "Current project",
                           "Plugin database", "Samples", "No recent projects"})
    expect(ids.count(must) == 1, (std::string("in the template: ") + must).c_str());
  // No window shows a title bar with a hard-coded English title.
  std::vector<std::string> plainTitles;
  for (const auto& e : fs::directory_iterator("src/app")) {
    if (e.path().extension() != ".cpp") continue;
    std::istringstream in(readFile(e.path().string()));
    std::string line, prev;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);
    for (size_t i = 0; i < lines.size(); ++i) {
      static const std::regex begin(R"(ImGui::Begin\("[A-Za-z])");
      if (!std::regex_search(lines[i], begin)) continue;
      const std::string call = lines[i] + (i + 1 < lines.size() ? lines[i + 1] : "");
      if (call.find("NoDecoration") != std::string::npos || call.find("NoTitleBar") != std::string::npos) continue;
      plainTitles.push_back(e.path().filename().string() + ":" + std::to_string(i + 1));
    }
  }
  for (size_t i = 0; i < plainTitles.size() && i < 20; ++i) std::printf("# untranslated window title: %s\n", plainTitles[i].c_str());
  expect(plainTitles.empty(), "window titles go through ui::windowTitle / i18n::tr");
  for (size_t i = 0; i < raw.size() && i < 20; ++i) std::printf("# hard-coded UI text: %s\n", raw[i].c_str());
  expect(raw.empty(), "no hard-coded English in menus, buttons, checkboxes, lists (wrap in i18n::tr)");

  // Keymap labels and context names are extractable (N_ in Keymap.cpp).
  bool all = true;
  for (const auto& c : keys::commands()) all &= ids.count(c.label) == 1;
  for (int i = 0; i < static_cast<int>(keys::Context::Count); ++i) all &= ids.count(keys::contextName(static_cast<keys::Context>(i))) == 1;
  expect(all, "every command label and context name is in the template");
}

}  // namespace

int main() {
  keymapTests();
  i18nTests();
  coverageTests();
  return finish();
}
