// Gridline — project interop. See ProjectInterop.h.
#include "ProjectInterop.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include "Json.h"
#include "MidiFile.h"
#include "PluginInfo.h"
#include "Presets.h"
#include "Project.h"
#include "Zip.h"

namespace fs = std::filesystem;

namespace gl {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void walkJson(Json& j, const std::function<void(std::string&)>& fn, bool& changed) {
  switch (j.type) {
    case Json::Type::String:
      if (looksLikeFilePath(j.str)) {
        std::string before = j.str;
        fn(j.str);
        changed = changed || j.str != before;
      }
      break;
    case Json::Type::Array:
      for (auto& v : j.array) walkJson(v, fn, changed);
      break;
    case Json::Type::Object:
      for (auto& [k, v] : j.object) walkJson(v, fn, changed);
      break;
    default: break;
  }
}

// One plugin state; returns true when a reference changed.
bool visitState(PluginState& st, const std::function<void(std::string&)>& fn) {
  if (st.data.empty()) return false;
  const char first = st.data.front();
  if (first == '{' || first == '[') {
    Json j;
    if (!jsonParse(st.data, j)) return false;
    bool changed = false;
    walkJson(j, fn, changed);
    if (changed) {
      st.data = jsonStringify(j, 0);
      while (!st.data.empty() && (st.data.back() == '\n')) st.data.pop_back();
    }
    return changed;
  }
  const PluginInfo* info = findPlugin(st.type);
  if (info && info->usesSample && st.data.find('\n') != std::string::npos) {
    // "<sample file>\n<model>" (J-41 slicer region model): the first line.
    const size_t nl = st.data.find('\n');
    std::string first = st.data.substr(0, nl);
    const std::string before = first;
    if (!first.empty()) fn(first);
    if (first == before) return false;
    st.data = first + st.data.substr(nl);
    return true;
  }
  if ((info && info->usesSample) || looksLikeFilePath(st.data)) {
    std::string before = st.data;
    fn(st.data);
    return st.data != before;
  }
  return false;
}

bool sameContent(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
  std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
  std::vector<char> ba(1 << 16), bb(1 << 16);
  while (x && y) {
    x.read(ba.data(), static_cast<std::streamsize>(ba.size()));
    y.read(bb.data(), static_cast<std::streamsize>(bb.size()));
    if (x.gcount() != y.gcount() || !std::equal(ba.begin(), ba.begin() + x.gcount(), bb.begin())) return false;
  }
  return true;
}

bool existsFile(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::path(p).is_absolute() && fs::is_regular_file(p, ec);
}

}  // namespace

bool looksLikeFilePath(const std::string& s) {
  if (s.size() < 5 || s.size() > 4096 || s.find('\n') != std::string::npos) return false;
  static const std::set<std::string> kExt = {".wav", ".wave", ".flac", ".ogg", ".oga", ".mp3", ".aif", ".aiff", ".aifc", ".wv",
                                             ".w64", ".rf64", ".caf", ".m4a", ".mid", ".midi", ".sf2", ".sfz", ".png", ".jpg",
                                             ".jpeg", ".bmp", ".rx2", ".ir", ".fst", ".zip"};
  const size_t dot = s.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return false;
  return kExt.count(lower(s.substr(dot))) > 0;
}

int forEachFileRef(Song& song, const std::function<void(std::string& path)>& fn) {
  int changed = 0;
  for (auto& c : song.channels) {
    changed += visitState(c.plugin, fn) ? 1 : 0;
    for (auto& nf : c.noteFx) changed += visitState(nf, fn) ? 1 : 0;
  }
  for (auto& m : song.mixer)
    for (auto& sl : m.slots) changed += visitState(sl.plugin, fn) ? 1 : 0;
  return changed;
}

std::vector<std::string> referencedFiles(const Song& song) {
  Song copy = song;
  std::vector<std::string> out;
  std::set<std::string> seen;
  forEachFileRef(copy, [&](std::string& p) {
    if (!p.empty() && seen.insert(p).second) out.push_back(p);
  });
  return out;
}

std::vector<std::string> missingFiles(const Song& song) {
  std::vector<std::string> out;
  for (const auto& p : referencedFiles(song))
    if (!existsFile(p)) out.push_back(p);
  return out;
}

int resolveRelativePaths(Song& song, const std::string& baseDir) {
  if (baseDir.empty()) return 0;
  return forEachFileRef(song, [&](std::string& p) {
    if (p.empty() || fs::path(p).is_absolute()) return;
    std::error_code ec;
    const fs::path abs = (fs::path(baseDir) / fs::path(p)).lexically_normal();
    if (fs::is_regular_file(abs, ec)) p = abs.string();
  });
}

CollectReport collectFiles(Song& song, const std::string& folder, const std::string& relativeTo) {
  CollectReport rep;
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) {
    rep.error = "cannot create " + folder;
    return rep;
  }
  std::set<std::string> missing;
  std::map<std::string, std::string> done;  // source → destination (absolute)
  std::set<std::string> usedNames;
  forEachFileRef(song, [&](std::string& p) {
    if (!existsFile(p)) {
      if (!p.empty() && missing.insert(p).second) rep.missing.push_back(p);
      return;
    }
    auto it = done.find(p);
    if (it == done.end()) {
      const fs::path src(p);
      const std::string stem = src.stem().string(), ext = src.extension().string();
      fs::path dst;
      for (int k = 1;; ++k) {
        dst = fs::path(folder) / (k == 1 ? stem + ext : stem + " (" + std::to_string(k) + ")" + ext);
        const std::string key = lower(dst.string());
        if (usedNames.count(key)) continue;
        if (!fs::exists(dst, ec)) break;
        if (fs::equivalent(dst, src, ec) || sameContent(dst, src)) break;
      }
      usedNames.insert(lower(dst.string()));
      if (fs::exists(dst, ec)) {
        ++rep.reused;
      } else if (fs::copy_file(src, dst, ec)) {
        ++rep.copied;
      } else {
        rep.error = "cannot copy " + p + ": " + ec.message();
        return;
      }
      it = done.emplace(p, dst.lexically_normal().string()).first;
      rep.moved[p] = it->second;
    }
    p = relativeTo.empty() ? it->second : fs::path(it->second).lexically_relative(relativeTo).generic_string();
  });
  return rep;
}

CollectReport exportProjectDataFiles(const Song& song, const std::string& folder, const std::string& name) {
  Song copy = song;
  CollectReport rep = collectFiles(copy, (fs::path(folder) / "Samples").string(), folder);
  if (!rep.error.empty()) return rep;
  std::string err;
  if (!saveProject((fs::path(folder) / (name + ".gridline.json")).string(), copy, &err)) rep.error = err;
  return rep;
}

BonesReport exportProjectBones(const Song& song, const std::string& folder, const std::string& name) {
  BonesReport rep;
  rep.files = exportProjectDataFiles(song, folder, name);
  if (!rep.files.error.empty()) return rep;
  std::error_code ec;
  const fs::path midiDir = fs::path(folder) / "MIDI";
  fs::create_directories(midiDir, ec);
  int k = 0;
  for (const auto& p : song.patterns) {
    ++k;
    MidiFileData m = patternToMidi(song, p.id);
    if (m.tracks.empty()) continue;
    char num[32];
    std::snprintf(num, sizeof num, "%02d ", k);
    if (writeMidiFile((midiDir / (num + p.name + ".mid")).string(), m)) ++rep.midiFiles;
  }
  if (!song.playlist.empty()) {
    MidiFileData m = songToMidi(song);
    if (!m.tracks.empty() && writeMidiFile((midiDir / (name + " (song).mid")).string(), m)) ++rep.midiFiles;
  }
  // Instrument states and effect chains, with references to the collected files.
  Song copy = song;
  relinkFiles(copy, rep.files.moved);
  const std::string presets = (fs::path(folder) / "Presets").string();
  for (const auto& c : copy.channels)
    if (!c.plugin.type.empty() && savePreset(presets, c.plugin, c.name, "project bones: " + name)) ++rep.presets;
  for (size_t i = 0; i < copy.mixer.size(); ++i) {
    const auto& m = copy.mixer[i];
    ChainPreset chain;
    bool any = false;
    for (const auto& sl : m.slots) {
      chain.slots.push_back(sl.plugin);
      chain.mix.push_back(sl.mix);
      any = any || !sl.plugin.type.empty();
    }
    if (any && saveChain(presets, chain, mixerTrackName(copy, i))) ++rep.chains;
  }
  return rep;
}

CollectReport exportZippedProject(const Song& song, const std::string& zipPath, const std::string& name) {
  CollectReport rep;
  Song copy = song;
  std::map<std::string, std::string> entry;  // source → archive name
  std::set<std::string> names;
  std::set<std::string> missing;
  forEachFileRef(copy, [&](std::string& p) {
    if (!existsFile(p)) {
      if (!p.empty() && missing.insert(p).second) rep.missing.push_back(p);
      return;
    }
    auto it = entry.find(p);
    if (it == entry.end()) {
      const fs::path src(p);
      std::string n;
      for (int k = 1;; ++k) {
        n = "Samples/" + (k == 1 ? src.filename().string() : src.stem().string() + " (" + std::to_string(k) + ")" + src.extension().string());
        if (names.insert(lower(n)).second) break;
      }
      it = entry.emplace(p, n).first;
      rep.moved[p] = n;
    }
    p = it->second;
  });
  ZipWriter z;
  std::string err;
  if (!z.open(zipPath, &err)) {
    rep.error = err;
    return rep;
  }
  const std::string text = projectToText(copy);
  if (!z.add(name + ".gridline.json", std::vector<uint8_t>(text.begin(), text.end()))) {
    rep.error = z.close(&err) ? "cannot write " + zipPath : err;  // J-78: a failed entry is reported (the target stays as it was)
    return rep;
  }
  for (const auto& [src, n] : entry) {
    if (!z.addFile(n, src, &err)) {
      rep.error = err;
      return rep;
    }
    ++rep.copied;
  }
  if (!z.close(&err)) rep.error = err;
  return rep;
}

bool unpackZippedProject(const std::string& zipPath, const std::string& dir, std::string& projectFile, std::string* err) {
  std::vector<std::string> files;
  if (!extractZip(zipPath, dir, &files, err)) return false;
  projectFile.clear();
  std::string flp, json;
  for (const auto& f : files) {
    const std::string l = lower(f);
    if (l.size() > 14 && l.compare(l.size() - 14, 14, ".gridline.json") == 0 && projectFile.empty()) projectFile = f;
    if (l.size() > 5 && l.compare(l.size() - 5, 5, ".json") == 0 && json.empty()) json = f;
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".flp") == 0 && flp.empty()) flp = f;
  }
  if (projectFile.empty()) projectFile = !json.empty() ? json : flp;
  if (projectFile.empty()) {
    if (err) *err = "the archive holds no project file";
    return false;
  }
  return true;
}

RelinkReport findReplacements(const std::vector<std::string>& missing, const std::string& folder, const RelinkOptions& o) {
  RelinkReport rep;
  // Index: lower-case file name → paths; lower-case stem → paths.
  std::map<std::string, std::vector<fs::path>> byName, byStem;
  std::error_code ec;
  auto add = [&](const fs::path& p) {
    byName[lower(p.filename().string())].push_back(p);
    byStem[lower(p.stem().string())].push_back(p);
    ++rep.indexed;
  };
  if (o.recursive) {
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && rep.indexed < o.maxFiles; it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2)) add(it->path());
    }
  } else {
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2)) add(it->path());
    }
  }
  for (const auto& m : missing) {
    // FL / Windows paths use backslashes.
    std::string norm = m;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    const fs::path mp(norm);
    std::vector<fs::path> cands;
    if (auto it = byName.find(lower(mp.filename().string())); it != byName.end()) cands = it->second;
    if (cands.empty() && o.matchStem)
      if (auto it = byStem.find(lower(mp.stem().string())); it != byStem.end())
        for (const auto& c : it->second)
          if (looksLikeFilePath(c.string())) cands.push_back(c);
    if (cands.empty()) {
      rep.notFound.push_back(m);
      continue;
    }
    // Rank: parent folder names in common (from the file upwards), then size hint.
    auto score = [&](const fs::path& c) {
      int s = 0;
      auto a = mp.parent_path(), b = c.parent_path();
      while (!a.empty() && !b.empty() && lower(a.filename().string()) == lower(b.filename().string()) && a.has_filename()) {
        ++s;
        a = a.parent_path();
        b = b.parent_path();
      }
      s *= 4;
      if (auto it = o.sizeHints.find(m); it != o.sizeHints.end()) {
        std::error_code e3;
        if (fs::file_size(c, e3) == it->second) s += 2;
      }
      if (lower(c.extension().string()) == lower(mp.extension().string())) s += 1;
      return s;
    };
    std::stable_sort(cands.begin(), cands.end(), [&](const fs::path& x, const fs::path& y) { return score(x) > score(y); });
    rep.found[m] = cands[0].string();
  }
  return rep;
}

int relinkFiles(Song& song, const std::map<std::string, std::string>& map) {
  if (map.empty()) return 0;
  return forEachFileRef(song, [&](std::string& p) {
    auto it = map.find(p);
    if (it != map.end()) p = it->second;
  });
}

}  // namespace gl
