// Gridline — marker areas. See MarkerAreas.h.
#include "MarkerAreas.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include "../core/I18n.h"
#include "../core/MarkerAreas.h"
#include "Export.h"

namespace fs = std::filesystem;

namespace gl {

namespace {

// Where the arrangement starts: the Session's slot clips sit before the live area's end.
Tick arrangementStart(const Song& song) { return std::max<Tick>(0, song.live.areaEnd); }

bool inArrangement(const Clip& c, Tick zone) { return c.start >= zone; }

bool sounds(const Song& song, const Clip& c) {
  if (c.muted || c.kind == ClipKind::Automation) return false;
  if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) return false;
  return song.clipLength(c) > 0;
}

// The song as one area renders it: the Session's slot clips gone, and only the clips that overlap
// the area (automation clips all stay: they set the state the area starts in). Nothing outside the
// area plays in a range render anyway; with them gone, the splits and the Track Receipt's sidecar
// list only what the area holds.
Song areaSong(const Song& song, const MarkerArea& a) {
  Song s = song;
  const Tick zone = arrangementStart(song);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                  [&](const Clip& c) {
                                    if (!inArrangement(c, zone)) return true;
                                    if (c.kind == ClipKind::Automation) return false;
                                    return !(c.start < a.end && c.start + song.clipLength(c) > a.start);
                                  }),
                   s.playlist.end());
  if (zone > 0) s.live.areaEnd = 0;  // (positions stay as they are: the area's ticks are the song's)
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Windows refuses these as file names, with any extension ("con.wav").
bool reservedName(const std::string& stem) {
  std::string b = lower(stem.substr(0, stem.find('.')));
  while (!b.empty() && b.back() == ' ') b.pop_back();
  static const std::set<std::string> names = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                              "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  return names.count(b) > 0;
}

// At most `max` bytes, never cutting a UTF-8 character.
std::string clip(std::string s, size_t max) {
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  return s;
}

}  // namespace

std::vector<MarkerArea> markerAreas(const Song& song) {
  std::vector<MarkerArea> out;
  const Tick zone = arrangementStart(song);
  Tick end = zone;
  for (const Clip& c : song.playlist)
    if (inArrangement(c, zone)) end = std::max(end, c.start + song.clipLength(c));
  // The arrangement's area markers (the Session's, before `zone`, are not), cut by the one helper
  // the Track Receipt uses too (core/MarkerAreas.h). `idx` maps them back to Song::markers.
  std::vector<Marker> marks;
  std::vector<int> idx;
  for (size_t i = 0; i < song.markers.size(); ++i)
    if (markers::isAreaMarker(song.markers[i]) && song.markers[i].pos >= zone) marks.push_back(song.markers[i]), idx.push_back(static_cast<int>(i));
  if (marks.empty()) return out;
  auto content = [&](Tick a, Tick b) {
    for (const Clip& c : song.playlist)
      if (inArrangement(c, zone) && sounds(song, c) && c.start < b && c.start + song.clipLength(c) > a) return true;
    return false;
  };
  auto start = [&](Tick b) {  // the stretch before the first marker: an area only when a clip sounds there
    if (b > zone && content(zone, b)) {
      MarkerArea a;
      a.start = zone;
      a.end = b;
      a.name = i18n::tr("Start");
      a.marker = -1;
      a.hasContent = true;
      out.push_back(a);
    }
  };
  const std::vector<markers::Area> cut = markers::areas(marks, zone, end);
  // (the song ends at its last clip: a marker there or after it opens nothing; with every marker
  // there, the song before them is the one area)
  if (cut.empty()) start(end);
  for (const markers::Area& c : cut) {
    if (c.marker < 0) {
      start(c.end);
      continue;
    }
    MarkerArea a;
    a.start = c.start;
    a.end = c.end;
    a.marker = idx[static_cast<size_t>(c.marker)];
    if (c.name.empty()) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("Marker %d"), a.marker + 1);
      a.name = b;
    } else {
      a.name = c.name;
    }
    a.hasContent = content(a.start, a.end);
    out.push_back(a);
  }
  return out;
}

std::vector<std::string> markerAreaFileNames(const std::vector<MarkerArea>& areas, const std::string& pattern, const std::string& project, int sampleRate) {
  std::vector<std::string> out;
  std::set<std::string> taken;
  const std::string pat = pattern.empty() ? std::string(kMarkerAreaPattern) : pattern;
  for (size_t i = 0; i < areas.size(); ++i) {
    char num[16];
    std::snprintf(num, sizeof num, "%02d", static_cast<int>(i + 1));
    const std::map<char, std::string> tok = {{'P', project.empty() ? std::string("Untitled") : project}, {'M', areas[i].name}, {'N', num},
                                             {'R', std::to_string(sampleRate)}};
    std::string stem = clip(expandNamePattern(pat, tok), 180);  // (room for " 99" and the longest extension)
    stem = safeFileName(stem);
    if (reservedName(stem)) stem += "_";
    std::string n = stem;
    for (int k = 2; taken.count(lower(n)); ++k) n = stem + " " + std::to_string(k);
    taken.insert(lower(n));
    out.push_back(n);
  }
  return out;
}

ExportOutcome runMarkerAreaExport(const Song& song, const ExportSettings& s, const std::vector<MarkerArea>& areas, const std::vector<std::string>& names,
                                  const std::function<void(double, const std::string&)>& progress, const std::atomic<bool>* cancel,
                                  std::vector<MarkerAreaResult>* perArea) {
  ExportOutcome out;
  if (perArea) perArea->clear();
  const size_t n = std::min(areas.size(), names.size());
  if (n == 0) {
    out.errors.push_back("no marker areas to export");
    return out;
  }
  const bool split = s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups;
  bool first = true;
  for (size_t i = 0; i < n; ++i) {
    if (cancel && cancel->load()) {
      out.cancelled = true;
      break;
    }
    const MarkerArea& area = areas[i];
    ExportSettings a = s;
    a.render.scope = RenderScope::Range;
    a.render.start = area.start;
    a.render.end = area.end;
    a.baseName = names[i];
    if (split) a.folder = (fs::path(s.folder) / names[i]).string();  // one folder per area
    const std::string what = "Area " + std::to_string(i + 1) + " of " + std::to_string(n) + " (" + area.name + ")";
    const Song played = areaSong(song, area);
    ExportOutcome o = runExport(played, a, [&](double p, const std::string& st) {
      if (progress) progress((static_cast<double>(i) + std::clamp(p, 0.0, 1.0)) / static_cast<double>(n), what + ": " + st);
    }, cancel);
    MarkerAreaResult r;
    r.name = names[i];
    if (o.cancelled) {  // the area under way goes; the finished ones stay
      std::error_code ec;
      for (const auto& f : o.files) fs::remove(f, ec);
      if (split) {
        for (const auto& f : o.files) fs::remove(fs::path(f).parent_path(), ec);  // a split kind's own subfolder, once empty
        fs::remove(a.folder, ec);  // (only when empty)
      }
      out.cancelled = true;
      if (perArea) perArea->push_back(r);
      break;
    }
    r.files = o.files;
    for (const auto& e : o.errors) r.errors.push_back(names[i] + ": " + e);
    r.done = true;
    out.files.insert(out.files.end(), r.files.begin(), r.files.end());
    out.errors.insert(out.errors.end(), r.errors.begin(), r.errors.end());
    if (first && o.frames > 0) {
      out.frames = o.frames;
      out.peak = o.peak;
      out.lufs = o.lufs;
      out.ceilingLimited = o.ceilingLimited;
      first = false;
    }
    if (perArea) perArea->push_back(std::move(r));
  }
  if (progress && !out.cancelled) progress(1.0, "Done");
  return out;
}

}  // namespace gl
