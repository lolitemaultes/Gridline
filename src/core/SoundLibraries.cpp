// Gridline — K-218: known sound library folders. See SoundLibraries.h.
#include "SoundLibraries.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include "Json.h"
#include "Project.h"

namespace gl::soundlib {

namespace fs = std::filesystem;

namespace {

struct Known {
  const char* path;
  bool underHome;
  const char* name;
};

const std::vector<Known>& known() {
  static const std::vector<Known> k = {
#if defined(__APPLE__)
      {"Library/Audio/Apple Loops", false, "System loops"},
      {"Library/Audio/Apple Loops", true, "Your loops"},
      {"Users/Shared/Samples", false, "Shared samples"},
#elif defined(_WIN32)
      {"Users/Public/Music/Samples", false, "Shared samples"},
      {"Users/Public/Documents/Samples", false, "Shared samples"},
      {"ProgramData/Gridline/Sounds", false, "Installed sound packs"},
#else
      {"usr/share/sounds/samples", false, "System samples"},
      {"usr/share/hydrogen/data/drumkits", false, "Drum kit samples"},
      {"usr/local/share/samples", false, "Local samples"},
      {"usr/share/gridline/sounds", false, "Installed sound packs"},
      {".local/share/gridline/sounds", true, "Your sound packs"},
#endif
  };
  return k;
}

std::string norm(const std::string& p) {
  std::string s = fs::path(p).lexically_normal().string();
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return s;
}

std::string answerFile(const std::string& configDir) { return (fs::path(configDir) / "sound-libraries.json").string(); }

}  // namespace

std::vector<Library> candidates(const std::string& home, const std::string& systemRoot) {
  std::vector<Library> out;
  std::set<std::string> seen;
#if defined(_WIN32)
  const std::string sys = systemRoot.empty() ? std::string("C:/") : systemRoot;
#else
  const std::string sys = systemRoot.empty() ? std::string("/") : systemRoot;
#endif
  for (const Known& k : known()) {
    if (k.underHome && home.empty()) continue;
    const std::string p = norm((fs::path(k.underHome ? home : sys) / k.path).string());
    std::error_code ec;
    if (!fs::is_directory(p, ec) || !seen.insert(p).second) continue;
    out.push_back({p, k.name});
  }
  return out;
}

Answer loadAnswer(const std::string& configDir) {
  Answer a;
  std::ifstream in(answerFile(configDir), std::ios::binary);
  if (!in) return a;
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j)) return a;
  a.asked = j.num("asked") > 0;
  if (const Json* f = j.find("added"); f && f->is(Json::Type::Array))
    for (const auto& x : f->array)
      if (x.is(Json::Type::String)) a.added.push_back(x.str);
  return a;
}

bool saveAnswer(const std::string& configDir, const Answer& a) {
  Json j = Json::makeObject();
  j.object["asked"] = Json::makeNumber(a.asked ? 1 : 0);
  Json f = Json::makeArray();
  for (const auto& p : a.added) f.array.push_back(Json::makeString(p));
  j.object["added"] = std::move(f);
  std::error_code ec;
  fs::create_directories(configDir, ec);
  return writeFileAtomic(answerFile(configDir), jsonStringify(j, 2));
}

std::vector<Library> toOffer(const std::string& configDir, const std::string& home, const std::string& systemRoot) {
  if (loadAnswer(configDir).asked) return {};
  std::set<std::string> have;
  std::ifstream in(fs::path(configDir) / "browser-folders.txt");
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) have.insert(norm(l));
  std::vector<Library> out;
  for (Library& l : candidates(home, systemRoot))
    if (!have.count(l.path)) out.push_back(std::move(l));
  return out;
}

void answer(const std::string& configDir, const std::vector<Library>& libs, bool accept) {
  Answer a = loadAnswer(configDir);
  a.asked = true;
  if (accept)
    for (const Library& l : libs)
      if (std::find(a.added.begin(), a.added.end(), l.path) == a.added.end()) a.added.push_back(l.path);
  saveAnswer(configDir, a);
}

}  // namespace gl::soundlib
