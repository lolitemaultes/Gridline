// Gridline — translation hook (see I18n.h).
#include "I18n.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include "Json.h"

namespace gl::i18n {

namespace fs = std::filesystem;

namespace {
struct State {
  std::string code = "en";
  std::map<std::string, std::string> catalogue;
  std::unordered_map<std::string, std::string> cache;  // stable storage for tr()
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

std::vector<Language> available(const std::string& dir) {
  std::vector<Language> out = {{"en", "English"}, {"pseudo", "Pseudo (layout test)"}};
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    std::ifstream is(it->path(), std::ios::binary);
    std::stringstream ss;
    ss << is.rdbuf();
    Json j;
    if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) continue;
    std::string code = it->path().stem().string();
    if (code == "en" || code == "pseudo") continue;
    out.push_back({code, j.text("name", code)});
  }
  return out;
}

bool setLanguage(const std::string& code, const std::string& dir, std::string* err) {
  State& s = st();
  s.cache.clear();
  s.catalogue.clear();
  s.code = "en";
  if (code.empty() || code == "en") return true;
  if (code == "pseudo") {
    s.code = code;
    return true;
  }
  std::ifstream is(fs::path(dir) / (code + ".json"), std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  Json j;
  if (!is || !jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) {
    if (err) *err = "language file missing or invalid: " + code;
    return false;
  }
  if (const Json* strs = j.find("strings"); strs && strs->is(Json::Type::Object))
    for (const auto& [k, v] : strs->object)
      if (v.is(Json::Type::String)) s.catalogue[k] = v.str;
  s.code = code;
  return true;
}

const std::string& language() { return st().code; }

const char* tr(const char* english) {
  State& s = st();
  if (s.code == "en" || !english) return english;
  auto it = s.cache.find(english);
  if (it != s.cache.end()) return it->second.c_str();
  std::string out;
  if (s.code == "pseudo") {
    out = pseudo(english);
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
