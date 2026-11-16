// Gridline — K-89: DAWproject files (.dawproject, the open interchange format: a zip holding
// project.xml, metadata.xml, audio files and plugin states). Export maps the song's tempo and
// meter, channels (one instrument track each, with volume, pan, mute and the instrument),
// mixer inserts (tracks of their own with their effects, routing and sends), the arrangement's
// pattern clips (one note clip per channel in each, notes with their per-note pitch, pressure
// and timbre), audio clips (the files go into the archive), volume, pan and tempo automation
// and markers. Import builds a new song from the same. What cannot be carried either way is
// listed in an approximation report.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "Song.h"
#include "XmlDom.h"

namespace gl::dawproject {

struct Report {
  // One line per kind of loss ("3 note slides became plain notes"), in the order found.
  std::vector<std::string> lines;
  int approximations() const { return static_cast<int>(lines.size()); }
  std::string text() const;  // the lines, one per row ("" = nothing was lost)
};

// The archive's parts. `files` lists {name in the archive, source path} for audio, and
// `blobs` {name, bytes} for plugin states.
struct Parts {
  xml::Node project, metadata;
  std::vector<std::pair<std::string, std::string>> files;
  std::vector<std::pair<std::string, std::string>> blobs;
};
Parts toParts(const Song& song, Report* report = nullptr);
// `readBlob(name, bytes)` returns a stored file of the archive (plugin states); `audioPath(name)`
// where an audio file of the archive was put on disk.
struct Source {
  std::function<bool(const std::string&, std::string&)> readBlob;
  std::function<std::string(const std::string&)> audioPath;
};
bool fromParts(const xml::Node& project, const xml::Node* metadata, const Source& src, Song& out, Report* report = nullptr,
               std::string* err = nullptr);

// Whole files. Import puts the archive's audio files into `audioDir` (created when needed).
bool exportFile(const Song& song, const std::string& path, Report* report = nullptr, std::string* err = nullptr);
bool importFile(const std::string& path, const std::string& audioDir, Song& out, Report* report = nullptr, std::string* err = nullptr);
bool isDawProject(const std::string& path);  // by extension

}  // namespace gl::dawproject
