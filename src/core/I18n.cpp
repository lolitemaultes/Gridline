// Gridline - translation hook (see I18n.h).
#include "I18n.h"
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <sstream>
#include <unordered_map>
#include "Json.h"

namespace gl::i18n {

namespace fs = std::filesystem;

namespace {
using Cache = std::unordered_map<std::string, std::string>;
struct State {
  std::string code = "en";
  std::map<std::string, std::string> catalogue;
  Cache cache;               // stable storage for tr()
  std::list<Cache> retired;  // earlier languages' texts: pointers handed out stay valid
};
State& st() {
  static State s;
  return s;
}

const char* accent(char c) {
  switch (c) {
    case 'a': return "\xC3\xA0";
    case 'e': return "\xC3\xA9";
    case 'i': return "\xC3\xAE";
    case 'o': return "\xC3\xB6";
    case 'u': return "\xC3\xBA";
    case 'c': return "\xC3\xA7";
    case 'n': return "\xC3\xB1";
    case 'y': return "\xC3\xBD";
    case 'A': return "\xC3\x80";
    case 'E': return "\xC3\x89";
    case 'I': return "\xC3\x8E";
    case 'O': return "\xC3\x96";
    case 'U': return "\xC3\x9A";
    case 'C': return "\xC3\x87";
    case 'N': return "\xC3\x91";
    default: return nullptr;
  }
}

std::string readFile(const fs::path& p, bool* ok = nullptr) {
  std::ifstream is(p, std::ios::binary);
  if (ok) *ok = static_cast<bool>(is);
  std::stringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

// One PO string literal ("..." with C escapes) starting at s[i] == '"'.
bool poLiteral(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '"') return false;
  for (++i; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"') {
      ++i;
      return true;
    }
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case '\\': out += '\\'; break;
      case '"': out += '"'; break;
      default: out += s[i]; break;
    }
  }
  return false;
}
}  // namespace

std::string pseudo(const std::string& s) {
  size_t idPos = s.find("##");
  std::string text = s.substr(0, idPos), id = idPos == std::string::npos ? "" : s.substr(idPos);
  if (text.empty()) return s;
  std::string out = "[";
  size_t letters = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '%') {  // copy a printf conversion unchanged
      out += c;
      while (++i < text.size()) {
        out += text[i];
        if (std::string("diouxXeEfgGcspn%").find(text[i]) != std::string::npos) break;
      }
      continue;
    }
    if (const char* a = accent(c))
      out += a;
    else
      out += c;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
  }
  out += std::string((letters + 2) / 3, '~');  // ~30% longer
  out += "]";
  return out + id;
}

bool parsePo(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string* languageName) {
  std::istringstream in(text);
  std::string line, id, str, *cur = nullptr;
  bool haveEntry = false, ok = true;
  auto flush = [&]() {
    if (haveEntry) {
      if (id.empty()) {  // header: "Language-Name: Deutsch\n"
        if (languageName) {
          size_t p = str.find("Language-Name:");
          if (p != std::string::npos) {
            size_t e = str.find('\n', p);
            std::string v = str.substr(p + 14, e == std::string::npos ? std::string::npos : e - p - 14);
            while (!v.empty() && (v.front() == ' ')) v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
            *languageName = v;
          }
        }
      } else if (!str.empty()) {
        out.emplace_back(id, str);
      }
    }
    id.clear(), str.clear(), cur = nullptr, haveEntry = false;
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') continue;
    if (line.compare(i, 6, "msgid ") == 0) {
      flush();
      haveEntry = true;
      cur = &id;
      i += 6;
    } else if (line.compare(i, 7, "msgstr ") == 0) {
      cur = &str;
      i += 7;
    } else if (line[i] != '"' || !cur) {
      continue;  // msgctxt / plural forms: not used by Gridline
    }
    i = line.find('"', i);
    if (i == std::string::npos || !poLiteral(line, i, *cur)) ok = false;
  }
  flush();
  return ok;
}

std::vector<Language> available(const std::string& dir) {
  std::vector<Language> out = {{"en", "English"}, {"pseudo", "Pseudo (layout test)"}};
  std::error_code ec;
  std::map<std::string, std::string> found;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto ext = it->path().extension().string();
    const std::string code = it->path().stem().string();
    if (code == "en" || code == "pseudo" || found.count(code)) continue;
    if (ext == ".json") {
      Json j;
      if (!jsonParse(readFile(it->path()), j) || !j.is(Json::Type::Object)) continue;
      found[code] = j.text("name", code);
    } else if (ext == ".po") {
      std::vector<std::pair<std::string, std::string>> entries;
      std::string name;
      if (!parsePo(readFile(it->path()), entries, &name)) continue;
      found[code] = name.empty() ? code : name;
    }
  }
  for (const auto& [code, name] : found) out.push_back({code, name});
  return out;
}

bool setLanguage(const std::string& code, const std::string& dir, std::string* err) {
  State& s = st();
  if (!s.cache.empty()) s.retired.push_back(std::move(s.cache));  // node storage moves: old pointers stay valid
  s.cache = Cache();
  s.catalogue.clear();
  s.code = "en";
  if (code.empty() || code == "en") return true;
  if (code == "pseudo") {
    s.code = code;
    return true;
  }
  bool ok = false;
  std::string text = readFile(fs::path(dir) / (code + ".po"), &ok);
  if (ok) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (!parsePo(text, entries)) {
      if (err) *err = "language file invalid: " + code + ".po";
      return false;
    }
    for (auto& [k, v] : entries) s.catalogue[k] = v;
  } else {
    text = readFile(fs::path(dir) / (code + ".json"), &ok);
    Json j;
    if (!ok || !jsonParse(text, j) || !j.is(Json::Type::Object)) {
      if (err) *err = "language file missing or invalid: " + code;
      return false;
    }
    if (const Json* strs = j.find("strings"); strs && strs->is(Json::Type::Object))
      for (const auto& [k, v] : strs->object)
        if (v.is(Json::Type::String) && !v.str.empty()) s.catalogue[k] = v.str;
  }
  s.code = code;
  return true;
}

const std::string& language() { return st().code; }

const char* tr(const char* english) {
  State& s = st();
  if (s.code == "en" || !english || !*english) return english;
  auto it = s.cache.find(english);
  if (it != s.cache.end()) return it->second.c_str();
  std::string out;
  if (s.code == "pseudo") {
    // Already pseudo text (a caller translated before a widget did): unchanged.
    const std::string e = english;
    const size_t idPos = e.find("##");
    const std::string text = e.substr(0, idPos);
    out = text.size() >= 2 && text.front() == '[' && text.find(']') != std::string::npos ? e : pseudo(e);
  } else {
    std::string key = english;
    size_t idPos = key.find("##");
    std::string text = key.substr(0, idPos), id = idPos == std::string::npos ? "" : key.substr(idPos);
    auto c = s.catalogue.find(text);
    out = c != s.catalogue.end() ? c->second + id : key;
  }
  return s.cache.emplace(english, std::move(out)).first->second.c_str();
}

}  // namespace gl::i18n
