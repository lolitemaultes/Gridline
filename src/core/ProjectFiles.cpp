// Gridline — project files (J-52). See ProjectFiles.h.
#include "ProjectFiles.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "../engine/Sample.h"
#include "I18n.h"

namespace fs = std::filesystem;

namespace gl {

namespace {

std::string lowerName(const std::string& path) {
  std::string s = fs::path(path).filename().string();
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Longest project extension of a (lower-case) file name, "" when none.
std::string projectExt(const std::string& lname) {
  for (const char* e : {".gridline.json", ".json", ".gridline.zip", ".flp", ".zip"})
    if (endsWith(lname, e) && lname.size() > std::string(e).size()) return e;
  return "";
}

}  // namespace

OpenKind openKindOf(const std::string& path) {
  const std::string n = lowerName(path);
  if (endsWith(n, ".json")) return OpenKind::GridlineProject;
  if (endsWith(n, ".flp")) return OpenKind::FlProject;
  if (endsWith(n, ".zip")) return OpenKind::ZippedProject;
  if (endsWith(n, ".mid") || endsWith(n, ".midi") || endsWith(n, ".rmi")) return OpenKind::Midi;
  if (isAudioFile(path)) return OpenKind::Audio;
  return OpenKind::Unknown;
}

bool opensAsProject(const std::string& path) {
  const OpenKind k = openKindOf(path);
  return k == OpenKind::GridlineProject || k == OpenKind::FlProject || k == OpenKind::ZippedProject;
}

const std::vector<FileType>& openFileTypes() {
  static const std::vector<FileType> t = {
      {i18n::N_("All projects (.gridline.json, .flp, .zip)"), {".json", ".flp", ".zip"}},
      {i18n::N_("Gridline project (.gridline.json)"), {".json"}},
      {i18n::N_("FL Studio project (.flp)"), {".flp"}},
      {i18n::N_("Zipped project / FL Studio zipped loop package (.zip)"), {".zip"}},
  };
  return t;
}

const std::vector<FileType>& importMidiTypes() {
  static const std::vector<FileType> t = {{i18n::N_("MIDI file (.mid)"), {".mid", ".midi", ".rmi"}}};
  return t;
}

const std::vector<FileType>& importAudioTypes() {
  static const std::vector<FileType> t = {{i18n::N_("Audio file"), {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"}}};
  return t;
}

const std::vector<FileType>& saveFileTypes() {
  static const std::vector<FileType> t = {
      {i18n::N_("Gridline project (.gridline.json)"), {".json"}},
      {i18n::N_("Gridline zipped project (.gridline.zip)"), {".gridline.zip"}},
      {i18n::N_("FL Studio project (.flp)"), {".flp"}},
      {i18n::N_("FL Studio zipped loop package (.zip)"), {".zip"}},
  };
  return t;
}

SaveFormat saveFormatOf(const std::string& path) {
  const std::string n = lowerName(path);
  if (endsWith(n, ".flp")) return SaveFormat::Flp;
  if (endsWith(n, ".gridline.zip")) return SaveFormat::GridlineZip;
  if (endsWith(n, ".zip")) return SaveFormat::FlZip;
  return SaveFormat::Gridline;
}

std::string withSaveExtension(const std::string& name, SaveFormat f) {
  std::string base = name;
  const std::string e = projectExt(lowerName(name));
  if (!e.empty()) base = name.substr(0, name.size() - e.size());
  switch (f) {
    case SaveFormat::Flp: return base + ".flp";
    case SaveFormat::FlZip: return base + ".zip";
    case SaveFormat::GridlineZip: return base + ".gridline.zip";
    case SaveFormat::Gridline:
    case SaveFormat::Count: break;
  }
  return base + ".gridline.json";
}

std::string projectStemOf(const std::string& path) {
  const std::string file = fs::path(path).filename().string();
  const std::string e = projectExt(lowerName(path));
  if (!e.empty()) return file.substr(0, file.size() - e.size());
  return fs::path(path).stem().string();
}

}  // namespace gl
