// Gridline — project templates (see Templates.h).
#include "Templates.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "Project.h"
#include "ProjectFiles.h"

namespace gl {

namespace fs = std::filesystem;

namespace {

// K-121: templates are saved as "<Name>.gridline"; older ones named "<Name>.gridline.json" still list.
bool isTemplateFile(const fs::path& p) { return isGridlineProjectName(p.filename().u8string()); }
bool hasNewName(const fs::path& p) { return saveFormatOf(p.filename().u8string()) == SaveFormat::Gridline; }
std::string stemOf(const fs::path& p) { return projectStemOf(p.u8string()); }
std::string readDescription(const fs::path& txt) {
  std::ifstream is(txt, std::ios::binary);
  if (!is) return "";
  std::string out, line;
  while (std::getline(is, line) && out.size() < 2000) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      if (!out.empty()) break;  // first paragraph only
      continue;
    }
    if (!out.empty()) out += ' ';
    out += line;
  }
  return out;
}
std::string safeName(std::string n) {
  for (char& c : n)
    if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) c = '-';
  while (!n.empty() && (n.front() == ' ' || n.front() == '.')) n.erase(n.begin());
  while (!n.empty() && n.back() == ' ') n.pop_back();
  return n;
}

// Which root a template came from, and whether it has the new name: a later root replaces an earlier
// one; in one folder the new name wins over the older one (a template saved again after K-121).
struct Found {
  TemplateInfo info;
  size_t root = 0;
  bool newName = false;
};

void addFile(std::map<std::pair<std::string, std::string>, Found>& out, const fs::path& f, const std::string& category, bool user,
             size_t root) {
  TemplateInfo t;
  t.path = f.string();
  t.name = stemOf(f);
  t.category = category;
  t.user = user;
  t.description = readDescription(f.parent_path() / (t.name + ".txt"));
  const bool newName = hasNewName(f);
  auto it = out.find({t.category, t.name});
  if (it != out.end() && it->second.root == root && it->second.newName && !newName) return;
  out[{t.category, t.name}] = Found{t, root, newName};
}

}  // namespace

std::vector<TemplateInfo> scanTemplates(const std::vector<std::pair<std::string, bool>>& roots) {
  std::map<std::pair<std::string, std::string>, Found> found;
  for (size_t ri = 0; ri < roots.size(); ++ri) {
    const std::string& root = roots[ri].first;
    const bool user = roots[ri].second;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_directory(e2)) {
        std::string cat = it->path().filename().string();
        if (cat.empty() || cat[0] == '.') continue;
        for (fs::directory_iterator jt(it->path(), e2), jend; !e2 && jt != jend; jt.increment(e2))
          if (isTemplateFile(jt->path())) addFile(found, jt->path(), cat, user, ri);
      } else if (isTemplateFile(it->path())) {
        addFile(found, it->path(), user ? "User" : "Other", user, ri);
      }
    }
  }
  std::vector<TemplateInfo> out;
  for (auto& [k, t] : found) out.push_back(t.info);
  return out;
}

bool saveTemplate(const std::string& root, const std::string& category, const std::string& name, const std::string& description,
                  const Song& song, std::string* path, std::string* err) {
  std::string n = safeName(name), c = safeName(category);
  if (n.empty()) {
    if (err) *err = "a template needs a name";
    return false;
  }
  fs::path dir = c.empty() ? fs::path(root) : fs::path(root) / c;
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path file = dir / fs::u8path(n + kProjectExt);
  Song s = song;
  s.info.title = n;
  if (!saveProject(file.u8string(), s, err)) return false;
  fs::remove(dir / fs::u8path(n + kOldProjectExt), ec);  // (the same template under its older name is replaced)
  fs::path txt = dir / (n + ".txt");
  if (!description.empty()) {
    if (!writeFileAtomic(txt.string(), description + "\n", err)) return false;
  } else {
    fs::remove(txt, ec);
  }
  if (path) *path = file.string();
  return true;
}

bool loadTemplate(const std::string& path, Song& out, std::string* err) {
  Song s;
  if (!loadProject(path, s, err)) return false;
  s.info.title = "Untitled";
  s.info.created = 0;
  s.info.workTime = 0;
  out = std::move(s);
  return true;
}

std::string templateSummary(const Song& s) {
  size_t inserts = 0;
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    const MixerTrack& m = s.mixer[i];
    bool used = false;
    for (const auto& sl : m.slots) used = used || !sl.plugin.empty();
    for (const auto& c : s.channels) used = used || c.mixerTrack == m.id;
    if (used) ++inserts;
  }
  std::ostringstream os;
  os << s.channels.size() << (s.channels.size() == 1 ? " channel" : " channels") << " \xC2\xB7 " << s.patterns.size()
     << (s.patterns.size() == 1 ? " pattern" : " patterns") << " \xC2\xB7 " << inserts << " mixer "
     << (inserts == 1 ? "track" : "tracks") << " in use \xC2\xB7 " << static_cast<int>(std::lround(s.tempo)) << " BPM \xC2\xB7 "
     << s.timeSigNum << "/" << s.timeSigDen << " \xC2\xB7 " << s.ppq << " PPQ";
  return os.str();
}

}  // namespace gl
