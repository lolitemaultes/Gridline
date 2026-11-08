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
  for (const char* e : {kOldProjectExt, ".json", kOldZippedProjectExt, kZippedProjectExt, kProjectExt, ".flp", ".zip"})
    if (endsWith(lname, e) && lname.size() > std::string(e).size()) return e;
  return "";
}

// A (lower-case) name ending in `ext` with something before it: ".gridline" alone is a hidden
// file or folder, no project.
bool namedWith(const std::string& lname, const char* ext) {
  const std::string e = ext;
  return lname.size() > e.size() && endsWith(lname, e);
}

}  // namespace

bool isGridlineProjectName(const std::string& path) {
  const std::string n = lowerName(path);
  return namedWith(n, kProjectExt) || namedWith(n, kOldProjectExt);
}

bool isGridlineZipName(const std::string& path) {
  const std::string n = lowerName(path);
  return namedWith(n, kZippedProjectExt) || namedWith(n, kOldZippedProjectExt);
}

OpenKind openKindOf(const std::string& path) {
  const std::string n = lowerName(path);
  if (endsWith(n, ".json") || namedWith(n, kProjectExt)) return OpenKind::GridlineProject;
  if (endsWith(n, ".flp")) return OpenKind::FlProject;
  if (endsWith(n, ".zip") || namedWith(n, kZippedProjectExt)) return OpenKind::ZippedProject;
  if (endsWith(n, ".xm") || endsWith(n, ".mod")) return OpenKind::TrackerModule;
  if (endsWith(n, ".mid") || endsWith(n, ".midi") || endsWith(n, ".rmi")) return OpenKind::Midi;
  if (isAudioFile(path)) return OpenKind::Audio;
  return OpenKind::Unknown;
}

bool opensAsProject(const std::string& path) {
  const OpenKind k = openKindOf(path);
  return k == OpenKind::GridlineProject || k == OpenKind::FlProject || k == OpenKind::ZippedProject || k == OpenKind::TrackerModule;
}

const std::vector<FileType>& openFileTypes() {
  static const std::vector<FileType> t = {
      {i18n::N_("All projects (.gridline, .gridlinez, .flp, .zip, .xm, .mod)"),
       {kProjectExt, kZippedProjectExt, ".json", ".flp", ".zip", ".xm", ".mod"}},
      {i18n::N_("Gridline project (.gridline, .gridline.json)"), {kProjectExt, ".json"}},
      {i18n::N_("FL Studio project (.flp)"), {".flp"}},
      {i18n::N_("Zipped project / FL Studio zipped loop package (.gridlinez, .zip)"), {kZippedProjectExt, ".zip"}},
      {i18n::N_("Tracker module (.xm, .mod)"), {".xm", ".mod"}},
  };
  return t;
}

const std::vector<FileType>& importMidiTypes() {
  static const std::vector<FileType> t = {{i18n::N_("MIDI file (.mid)"), {".mid", ".midi", ".rmi"}}};
  return t;
}

const std::vector<FileType>& importAudioTypes() {
  static const std::vector<FileType> t = {{i18n::N_("Audio file"), {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv", ".m4a", ".aac"}}};
  return t;
}

const std::vector<FileType>& saveFileTypes() {
  static const std::vector<FileType> t = {
      {i18n::N_("Gridline project (.gridline)"), {kProjectExt}},
      {i18n::N_("Gridline zipped project (.gridlinez)"), {kZippedProjectExt}},
      {i18n::N_("FL Studio project (.flp)"), {".flp"}},
      {i18n::N_("FL Studio zipped loop package (.zip)"), {".zip"}},
      {i18n::N_("Gridline project, older name (.gridline.json)"), {kOldProjectExt}},
      {i18n::N_("Gridline zipped project, older name (.gridline.zip)"), {kOldZippedProjectExt}},
  };
  return t;
}

SaveFormat saveFormatOf(const std::string& path) {
  const std::string n = lowerName(path);
  if (endsWith(n, ".flp")) return SaveFormat::Flp;
  if (endsWith(n, kZippedProjectExt)) return SaveFormat::GridlineZip;
  if (endsWith(n, kOldZippedProjectExt)) return SaveFormat::GridlineZipOld;
  if (endsWith(n, ".zip")) return SaveFormat::FlZip;
  if (endsWith(n, ".json")) return SaveFormat::GridlineJson;
  return SaveFormat::Gridline;
}

std::string withSaveExtension(const std::string& name, SaveFormat f) {
  std::string base = name;
  const std::string e = projectExt(lowerName(name));
  if (!e.empty()) base = name.substr(0, name.size() - e.size());
  switch (f) {
    case SaveFormat::Flp: return base + ".flp";
    case SaveFormat::FlZip: return base + ".zip";
    case SaveFormat::GridlineZip: return base + kZippedProjectExt;
    case SaveFormat::GridlineZipOld: return base + kOldZippedProjectExt;
    case SaveFormat::GridlineJson: return base + kOldProjectExt;
    case SaveFormat::Gridline:
    case SaveFormat::Count: break;
  }
  return base + kProjectExt;
}

std::string projectStemOf(const std::string& path) {
  const std::string file = fs::path(path).filename().string();
  const std::string e = projectExt(lowerName(path));
  if (!e.empty()) return file.substr(0, file.size() - e.size());
  return fs::path(path).stem().string();
}

std::string projectFileNameInZip(const std::string& zipPath, const std::string& stem) {
  return stem + (saveFormatOf(zipPath) == SaveFormat::GridlineZipOld ? kOldProjectExt : kProjectExt);
}

}  // namespace gl
