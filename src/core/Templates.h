// Gridline — project templates (TP-01). A template is an ordinary project
// file in a category folder, with an optional description next to it:
//
//   <root>/<Category>/<Name>.gridline   (K-121; older templates: <Name>.gridline.json)
//   <root>/<Category>/<Name>.txt          (first paragraph = preview text)
//
// Files directly in a root get the category "User" (user folder) or "Other".
// Gridline ships templates in assets/templates; "Save as template" writes to
// <config>/templates. A later root's template with the same category and name
// replaces an earlier one.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "Song.h"

namespace gl {

struct TemplateInfo {
  std::string path, name, category, description;
  bool user = false;
};

std::vector<TemplateInfo> scanTemplates(const std::vector<std::pair<std::string, bool>>& roots);
// File names are made safe ('/', '\\', ':' and control characters become '-').
bool saveTemplate(const std::string& root, const std::string& category, const std::string& name, const std::string& description,
                  const Song& song, std::string* path = nullptr, std::string* err = nullptr);
// Loads a template as a new, unsaved project (title "Untitled", no creation
// date or work time carried over).
bool loadTemplate(const std::string& path, Song& out, std::string* err = nullptr);
// Short summary of a template's content for the browser ("6 channels · 4 patterns · 140 BPM").
std::string templateSummary(const Song& song);

// The New project's starter kit (no template chosen): four drum voices on inserts 1-4 and a
// synth on insert 5, added to `base` (a song from makeEmptySong). K-306 (R6-10): the kit's
// voices are full-level instruments at unity gain, so hits that land together go past full
// scale; the master ends with the Limiter (last slot, as in the Beat starter template) so
// playback and the first export stay under 0 dBFS.
Song addStarterKit(Song base);

}  // namespace gl
