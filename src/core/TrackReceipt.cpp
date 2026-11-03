// Gridline - J-219: Track Receipt (see TrackReceipt.h).
#include "TrackReceipt.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include "Json.h"

namespace gl::receipt {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string stripExtension(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || name.size() - dot > 6) return name;
  for (size_t i = dot + 1; i < name.size(); ++i)
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) return name;
  return name.substr(0, dot);
}

const char* kTag = "[Clearance Required]";

}  // namespace

std::string baseName(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}

NameInfo parseName(const std::string& pathOrName) {
  NameInfo n;
  std::string name = stripExtension(baseName(pathOrName));
  const size_t tag = name.find(kTag);
  n.tracklib = tag != std::string::npos;
  // " - Part_N" after the tag (or at the end).
  {
    const size_t p = name.rfind("Part_");
    if (p != std::string::npos && p + 5 < name.size()) {
      bool digits = true;
      for (size_t i = p + 5; i < name.size(); ++i) digits = digits && std::isdigit(static_cast<unsigned char>(name[i]));
      if (digits && (tag == std::string::npos || p > tag)) {
        n.part = std::atoi(name.c_str() + p + 5);
        size_t cut = p;
        while (cut > 0 && (name[cut - 1] == ' ' || name[cut - 1] == '-')) --cut;
        name = name.substr(0, cut);
      }
    }
  }
  // "--<bpm>bpm-<Key>-": the last "--" followed by a number and "bpm".
  std::string head;
  bool bpmFound = false;
  for (size_t at = name.find("--"); at != std::string::npos; at = name.find("--", at + 1)) {
    size_t i = at + 2, j = i;
    while (j < name.size() && (std::isdigit(static_cast<unsigned char>(name[j])) || name[j] == '.')) ++j;
    if (j > i && name.compare(j, 3, "bpm") == 0) {
      head = name.substr(0, at);
      n.bpm = std::atof(name.substr(i, j - i).c_str());
      bpmFound = true;
      // key: between "bpm-" and the next "-" (or the tag)
      size_t k = j + 3;
      if (k < name.size() && name[k] == '-') ++k;
      size_t e = name.find('-', k);
      const size_t t = name.find('[', k);
      if (t != std::string::npos && (e == std::string::npos || t < e)) e = t;
      n.key = trim(name.substr(k, e == std::string::npos ? std::string::npos : e - k));
      if (!n.key.empty() && n.key.front() == '[') n.key.clear();
    }
  }
  if (!bpmFound) {
    if (n.tracklib) {
      head = name.substr(0, tag);
      while (!head.empty() && (head.back() == '-' || head.back() == ' ')) head.pop_back();
    } else {
      n.song = trim(name);
      n.title = n.song;
      return n;
    }
  }
  if (!n.tracklib && !bpmFound) return n;
  head = trim(head);
  // A loop cut from the song: "<Stem>--<start>-<end>" (seconds of the original).
  if (const size_t ld = head.rfind("--"); ld != std::string::npos) {
    const std::string r = head.substr(ld + 2);
    const size_t h = r.find('-');
    auto number = [](const std::string& s) {
      if (s.empty()) return false;
      for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
      return true;
    };
    if (h != std::string::npos && number(r.substr(0, h)) && number(r.substr(h + 1))) {
      const double a = std::atof(r.substr(0, h).c_str()), b = std::atof(r.substr(h + 1).c_str());
      if (b > a) {
        n.loopStart = a, n.loopEnd = b;
        head = trim(head.substr(0, ld));
      }
    }
  }
  const size_t dash = head.find_last_of('-');
  if (dash != std::string::npos && dash > 0) {
    n.stem = trim(head.substr(dash + 1));
    n.song = trim(head.substr(0, dash));
  } else {
    n.song = head;
  }
  const size_t first = n.song.find('-');
  if (first != std::string::npos && first > 0) {
    n.artist = trim(n.song.substr(0, first));
    n.title = trim(n.song.substr(first + 1));
    n.ambiguous = n.song.find('-', first + 1) != std::string::npos;
  } else {
    n.title = n.song;
  }
  return n;
}

NameInfo resplit(NameInfo n, const std::set<std::string>& knownArtists) {
  if (!n.ambiguous) return n;
  for (size_t at = n.song.find('-'); at != std::string::npos; at = n.song.find('-', at + 1)) {
    if (at == 0) continue;
    const std::string artist = trim(n.song.substr(0, at));
    if (knownArtists.count(lowerText(artist))) {
      n.artist = artist;
      n.title = trim(n.song.substr(at + 1));
      n.artistKnown = true;
      return n;
    }
  }
  return n;
}

std::string lowerText(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- ranges

Ranges unite(Ranges r, double join) {
  r.erase(std::remove_if(r.begin(), r.end(), [](const Range& x) { return !(x.b > x.a); }), r.end());
  std::sort(r.begin(), r.end(), [](const Range& x, const Range& y) { return x.a < y.a || (x.a == y.a && x.b < y.b); });
  Ranges out;
  for (const Range& x : r) {
    if (!out.empty() && x.a <= out.back().b + join)
      out.back().b = std::max(out.back().b, x.b);
    else
      out.push_back(x);
  }
  return out;
}

double total(const Ranges& r) {
  double t = 0;
  for (const Range& x : r) t += std::max(0.0, x.b - x.a);
  return t;
}

std::string timeText(double seconds) {
  if (!(seconds >= 0)) seconds = 0;
  const long long tenths = std::llround(seconds * 10.0);
  const long long m = tenths / 600, rest = tenths % 600;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld:%02lld.%lld", m, rest / 10, rest % 10);
  return buf;
}

std::string rangesText(const Ranges& r) {
  std::string s;
  for (const Range& x : r) {
    if (!s.empty()) s += ", ";
    s += timeText(x.a) + "-" + timeText(x.b);
  }
  return s;
}

std::string secondsText(double seconds) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", std::round(std::max(0.0, seconds) * 10.0) / 10.0);
  return buf;
}

namespace {
bool parseTime(const std::string& t0, double& out) {
  const std::string t = trim(t0);
  if (t.empty()) return false;
  double mins = 0;
  std::string secs = t;
  const size_t colon = t.find(':');
  if (colon != std::string::npos) {
    char* end = nullptr;
    const std::string m = t.substr(0, colon);
    mins = std::strtod(m.c_str(), &end);
    if (m.empty() || !end || *end) return false;
    secs = t.substr(colon + 1);
  }
  char* end = nullptr;
  const double s = std::strtod(secs.c_str(), &end);
  if (secs.empty() || !end || *end || s < 0 || mins < 0) return false;
  out = mins * 60 + s;
  return true;
}
}  // namespace

bool parseRanges(const std::string& text, Ranges& out) {
  out.clear();
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (item.empty()) continue;
    const size_t dash = item.find('-');
    double a = 0, b = 0;
    if (dash == std::string::npos || !parseTime(item.substr(0, dash), a) || !parseTime(item.substr(dash + 1), b) || b <= a) return false;
    out.push_back(Range{a, b});
  }
  out = unite(out);
  return true;
}

int tierFor(double seconds) {
  const double s = std::round(seconds * 10.0) / 10.0;
  if (s <= 2.0) return 2;
  if (s <= 15.0) return 15;
  if (s <= 60.0) return 60;
  return 0;
}

// ---- origin sidecar

std::string originPath(const std::string& file) { return file + ".origin.json"; }

bool writeOrigin(const std::string& file, const Origin& o) {
  Json j = Json::makeObject();
  j.object["gridlineOrigin"] = Json::makeNumber(1);
  j.object["source"] = Json::makeString(o.source);
  j.object["start"] = Json::makeNumber(o.start);
  j.object["end"] = Json::makeNumber(o.end);
  j.object["speed"] = Json::makeNumber(o.speed);
  Json b;
  b.type = Json::Type::Bool;
  b.boolean = o.reverse;
  j.object["reverse"] = b;
  b.boolean = o.rangeKnown;
  j.object["rangeKnown"] = b;
  j.object["kind"] = Json::makeString(o.kind);
  if (!o.sources.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : o.sources) a.array.push_back(Json::makeString(s));
    j.object["sources"] = a;
  }
  std::ofstream f(originPath(file), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << jsonStringify(j, 2) << "\n";
  return static_cast<bool>(f);
}

bool readOrigin(const std::string& file, Origin& o) {
  std::ifstream f(originPath(file), std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || j.type != Json::Type::Object || !j.find("gridlineOrigin")) return false;
  o = Origin{};
  o.source = j.text("source");
  o.start = j.num("start", 0);
  o.end = j.num("end", -1);
  o.speed = j.num("speed", 1);
  if (!(o.speed > 0)) o.speed = 1;
  if (const Json* v = j.find("reverse"); v && v->type == Json::Type::Bool) o.reverse = v->boolean;
  if (const Json* v = j.find("rangeKnown"); v && v->type == Json::Type::Bool) o.rangeKnown = v->boolean;
  o.kind = j.text("kind");
  if (const Json* a = j.find("sources"); a && a->type == Json::Type::Array)
    for (const auto& s : a->array)
      if (s.type == Json::Type::String) o.sources.push_back(s.str);
  // In a package the sources are named relative to the file (ProjectInterop, sidecarsFor).
  auto beside = [&](std::string& p) {
    const std::filesystem::path f = std::filesystem::u8path(p);
    if (!p.empty() && !f.is_absolute()) p = (std::filesystem::u8path(file).parent_path() / f).lexically_normal().u8string();
  };
  beside(o.source);
  for (auto& x : o.sources) beside(x);
  return !o.source.empty();
}

std::vector<Origin> originChain(const std::string& file) {
  std::vector<Origin> chain;
  std::set<std::string> seen{file};
  std::string at = file;
  Origin o;
  while (chain.size() < 8 && readOrigin(at, o)) {
    chain.push_back(o);
    if (!seen.insert(o.source).second) break;  // a loop
    at = o.source;
  }
  return chain;
}

bool mapThrough(const std::vector<Origin>& chain, double fileSeconds, Ranges& r) {
  double len = fileSeconds;
  for (const Origin& o : chain) {
    if (!o.rangeKnown) return false;
    if (len <= 0)
      for (const Range& x : r) len = std::max(len, x.b);
    const double end = o.end >= 0 ? o.end : o.start + len * o.speed;
    Ranges m;
    for (const Range& x : r) {
      Range y = o.reverse ? Range{end - x.b * o.speed, end - x.a * o.speed} : Range{o.start + x.a * o.speed, o.start + x.b * o.speed};
      y.a = std::clamp(y.a, o.start, end), y.b = std::clamp(y.b, o.start, end);
      if (y.b > y.a) m.push_back(y);
    }
    r = unite(m);
    len = 0;  // the next file's length: from its own range end
  }
  return true;
}

// ---- saved fields

bool Saved::empty() const {
  if (!(release == Release{}) || !writers.empty()) return false;
  for (const auto& [k, v] : songs)
    if (!(v == SongFields{})) return false;
  for (const auto& [k, v] : files)
    if (!(v == FileFields{})) return false;
  return true;
}

namespace {
void put(Json& j, const char* k, const std::string& v) {
  if (!v.empty()) j.object[k] = Json::makeString(v);
}
}  // namespace

std::string savedToJson(const Saved& s) {
  if (s.empty()) return "";
  Json j = Json::makeObject();
  j.object["v"] = Json::makeNumber(1);
  Json songs = Json::makeObject();
  for (const auto& [k, f] : s.songs) {
    if (f == SongFields{}) continue;
    Json o = Json::makeObject();
    put(o, "artist", f.artist), put(o, "title", f.title), put(o, "category", f.category), put(o, "share", f.share);
    put(o, "fee", f.fee), put(o, "tier", f.tier), put(o, "holders", f.holders);
    songs.object[k] = o;
  }
  if (!songs.object.empty()) j.object["songs"] = songs;
  Json files = Json::makeObject();
  for (const auto& [k, f] : s.files) {
    if (f == FileFields{}) continue;
    Json o = Json::makeObject();
    if (f.ticked >= 0) o.object["ticked"] = Json::makeNumber(f.ticked);
    put(o, "song", f.song), put(o, "ranges", f.ranges);
    files.object[k] = o;
  }
  if (!files.object.empty()) j.object["files"] = files;
  Json rel = Json::makeObject();
  put(rel, "title", s.release.title), put(rel, "performer", s.release.performer), put(rel, "licensee", s.release.licensee);
  put(rel, "isrc", s.release.isrc), put(rel, "date", s.release.date), put(rel, "distributor", s.release.distributor);
  if (!rel.object.empty()) j.object["release"] = rel;
  if (!s.writers.empty()) {
    Json a = Json::makeArray();
    for (const Writer& w : s.writers) {
      Json o = Json::makeObject();
      put(o, "name", w.name), put(o, "role", w.role), put(o, "pro", w.pro), put(o, "ipi", w.ipi), put(o, "share", w.share);
      a.array.push_back(o);
    }
    j.object["writers"] = a;
  }
  return jsonCompact(j);
}

Saved savedFromJson(const std::string& json) {
  Saved s;
  Json j;
  if (json.empty() || !jsonParse(json, j) || j.type != Json::Type::Object) return s;
  if (const Json* songs = j.find("songs"); songs && songs->type == Json::Type::Object)
    for (const auto& [k, o] : songs->object) {
      SongFields f;
      f.artist = o.text("artist"), f.title = o.text("title"), f.category = o.text("category"), f.share = o.text("share");
      f.fee = o.text("fee"), f.tier = o.text("tier"), f.holders = o.text("holders");
      s.songs[k] = f;
    }
  if (const Json* files = j.find("files"); files && files->type == Json::Type::Object)
    for (const auto& [k, o] : files->object) {
      FileFields f;
      f.ticked = std::clamp(jsonInt<int>(o.num("ticked", -1)), -1, 1);
      f.song = o.text("song"), f.ranges = o.text("ranges");
      s.files[k] = f;
    }
  if (const Json* r = j.find("release"); r && r->type == Json::Type::Object) {
    s.release.title = r->text("title"), s.release.performer = r->text("performer"), s.release.licensee = r->text("licensee");
    s.release.isrc = r->text("isrc"), s.release.date = r->text("date"), s.release.distributor = r->text("distributor");
  }
  if (const Json* a = j.find("writers"); a && a->type == Json::Type::Array)
    for (const auto& o : a->array) {
      Writer w;
      w.name = o.text("name"), w.role = o.text("role"), w.pro = o.text("pro"), w.ipi = o.text("ipi"), w.share = o.text("share");
      s.writers.push_back(w);
    }
  return s;
}

FileFields fileFields(const Saved& s, const std::string& path) {
  auto it = s.files.find(path);
  if (it != s.files.end()) return it->second;
  const std::string name = baseName(path);
  for (const auto& [k, f] : s.files)
    if (baseName(k) == name) return f;
  return FileFields{};
}

double parseShare(const std::string& s0) {
  std::string s = trim(s0);
  if (!s.empty() && s.back() == '%') s.pop_back();
  s = trim(s);
  for (char& c : s)
    if (c == ',') c = '.';
  if (s.empty()) return -1;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || *end || !(v >= 0) || v > 100) return -1;
  return v;
}

std::vector<std::string> reportDates(const std::string& releaseDate, int count) {
  std::vector<std::string> out;
  int y = 0, m = 0, d = 0;
  if (std::sscanf(releaseDate.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
    return out;
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  for (int k = 1; k <= count; ++k) {
    int mm = m - 1 + 6 * k, yy = y + mm / 12;
    mm %= 12;
    const bool leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
    const int dim = days[mm] + (mm == 1 && leap ? 1 : 0);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", yy, mm + 1, std::min(d, dim));
    out.push_back(buf);
  }
  return out;
}

std::string receiptFileName(const std::string& projectName) {
  return (projectName.empty() ? std::string("Untitled") : projectName) + "_trackreceipt.xml";
}
std::string resultsFileName(const std::string& projectName) {
  return (projectName.empty() ? std::string("Untitled") : projectName) + "_trackreceipt.pdf";
}

// ---- the report

namespace {

struct Piece {
  Range r;
  std::string from;
  bool estimated = false;
};

}  // namespace

Report build(const Measurement& m, const Saved& saved, const std::string& projectName) {
  Report rep;
  rep.project = projectName;
  rep.saved = saved;
  rep.mode = m.mode;
  rep.warnings = m.warnings;
  // Files.
  std::map<std::string, size_t> rowOf;
  for (const FileIn& f : m.files) {
    if (rowOf.count(f.path)) continue;
    FileRow r;
    r.path = f.path;
    r.name = baseName(f.path);
    r.info = parseName(f.path);
    r.seconds = f.seconds;
    r.exists = f.exists;
    r.derived = f.derived && !f.chain.empty();
    if (r.derived) r.origin = f.chain.back();
    rowOf[f.path] = rep.files.size();
    rep.files.push_back(r);
  }
  for (const Use& u : m.uses) {
    auto it = rowOf.find(u.file);
    if (it == rowOf.end()) continue;
    FileRow& r = rep.files[it->second];
    (u.muted ? r.muted : r.uses).push_back(u);
  }
  // Song of each file, tick state, mapped ranges.
  std::map<std::string, std::vector<std::pair<int, Piece>>> pieces;  // song key -> (timeline part, piece)
  std::map<std::string, std::vector<size_t>> songRows;
  for (size_t ri = 0; ri < rep.files.size(); ++ri) {
    FileRow& r = rep.files[ri];
    const FileIn* fin = nullptr;
    for (const FileIn& f : m.files)
      if (f.path == r.path) fin = &f;
    const FileFields ff = fileFields(saved, r.path);
    NameInfo src = r.info;  // the original's name (derived files: their source)
    if (r.derived) {
      src = parseName(r.origin.source);
      r.originNote = "derived from " + baseName(r.origin.source) + (r.origin.kind.empty() ? "" : " (" + r.origin.kind + ")");
    }
    r.autoTicked = src.tracklib;
    r.ticked = ff.ticked >= 0 ? ff.ticked == 1 : r.autoTicked;
    if (!ff.song.empty())
      r.song = ff.song;
    else if (src.tracklib)
      r.song = src.song;
    else if (r.derived)
      r.song = stripExtension(baseName(r.origin.source));
    else
      r.song = stripExtension(r.name);
    r.part = src.part;
    // Used ranges of this file (file seconds).
    Ranges manual;
    r.manual = !ff.ranges.empty() && parseRanges(ff.ranges, manual);
    Ranges all;
    std::vector<std::pair<Ranges, const Use*>> per;
    bool manualDone = false;
    for (const Use& u : r.uses) {
      Ranges ur;
      // Measured reads shorter than 10 ms are a voice's first block before it jumps to its
      // start (a deck cueing, a slice retrigger), not a part anyone hears: left out.
      for (const Range& x : unite(u.ranges))
        if (u.estimated || x.b - x.a >= kMinHeard) ur.push_back(x);
      if (u.estimated) {
        r.estimated = true;
        if (r.manual) {
          if (manualDone) continue;
          ur = manual;
          manualDone = true;
        }
      }
      for (const Range& x : ur) all.push_back(x);
      per.push_back({ur, &u});
    }
    if (r.manual && !manualDone && r.uses.empty()) {  // typed for a file nothing measured
      for (const Range& x : manual) all.push_back(x);
      per.push_back({manual, nullptr});
    }
    r.used = unite(all);
    r.usedSeconds = total(r.used);
    if (!r.ticked || r.used.empty()) {
      if (r.ticked) songRows[r.song].push_back(ri);  // a ticked file with nothing used still shows its song
      continue;
    }
    songRows[r.song].push_back(ri);
    const std::string label = r.info.stem.empty() ? r.name : r.info.stem;
    for (const auto& [ur, u] : per) {
      Ranges mapped = ur;
      int part = r.part;
      if (r.derived && fin) {
        if (!mapThrough(fin->chain, r.seconds, mapped)) {
          r.unknownRange = true;
          part = -1;
          mapped = ur;
        }
      }
      // A loop the store cut from the song: its seconds sit at loopStart in the original.
      const NameInfo& li = r.derived ? src : r.info;
      if (li.loopEnd > li.loopStart && part >= 0) {
        Ranges shifted;
        for (const Range& x : mapped) {
          const double a = std::min(li.loopEnd, li.loopStart + x.a), b = std::min(li.loopEnd, li.loopStart + x.b);
          if (b > a) shifted.push_back({a, b});
        }
        mapped = shifted;
      }
      for (const Range& x : mapped) {
        Piece p;
        p.r = x;
        p.from = (u ? u->where : std::string("typed by you"));
        if (u && u->bar0 > 0) {
          const int b0 = static_cast<int>(u->bar0), b1 = static_cast<int>(u->bar1);
          p.from += b1 > b0 ? ", bars " + std::to_string(b0) + "-" + std::to_string(b1) : ", bar " + std::to_string(b0);
        }
        p.from += " (" + label + ")";
        p.estimated = (u && u->estimated && !r.manual);
        pieces[r.song].push_back({part, p});
        // A render of several originals: each one's song gets the part (unknown range) too.
        if (r.unknownRange && ff.song.empty())
          for (const std::string& other : r.origin.sources) {
            const NameInfo on = parseName(other);
            if (!on.tracklib || on.song == r.song) continue;
            pieces[on.song].push_back({part, p});
            auto& rowsOf = songRows[on.song];
            if (std::find(rowsOf.begin(), rowsOf.end(), ri) == rowsOf.end()) rowsOf.push_back(ri);
          }
      }
    }
    if (r.unknownRange) r.originNote += ", range unknown";
  }
  // Artists the names make plain (one hyphen between artist and title): they settle ambiguous splits.
  std::set<std::string> knownArtists;
  for (const FileRow& r : rep.files) {
    for (const NameInfo& n : {r.info, r.derived ? parseName(r.origin.source) : NameInfo{}})
      if (n.tracklib && !n.ambiguous && !n.artist.empty()) knownArtists.insert(lowerText(n.artist));
  }
  for (const auto& [key, f] : saved.songs)
    if (!f.artist.empty()) knownArtists.insert(lowerText(f.artist));
  // Songs.
  for (auto& [key, rows] : songRows) {
    SongRow s;
    s.key = key;
    auto sf = saved.songs.find(key);
    if (sf != saved.songs.end()) s.fields = sf->second;
    std::set<std::string> stems, files;
    NameInfo first;
    bool haveFirst = false;
    for (size_t ri : rows) {
      const FileRow& r = rep.files[ri];
      NameInfo src = r.derived ? parseName(r.origin.source) : r.info;
      if (r.derived && src.song != key)  // a render of several originals: the one of this song
        for (const std::string& other : r.origin.sources)
          if (const NameInfo on = parseName(other); on.song == key) src = on;
      if (!haveFirst && (src.tracklib || !r.derived)) first = src, haveFirst = true;
      std::string st = r.info.tracklib ? r.info.stem : src.stem;
      if (r.derived) st = (st.empty() ? std::string("file") : st) + " (" + (r.origin.kind.empty() ? "derived" : r.origin.kind) + ")";
      if (st.empty()) st = r.name;
      if (!r.used.empty()) stems.insert(st);
      files.insert(r.path);
      for (const Use& u : r.uses) s.plays += u.plays;
      if (r.estimated && !r.manual)
        s.warnings.push_back(r.name + ": used inside a plugin Gridline cannot measure; the whole file is counted. Type the parts used if you know them.");
      if (r.unknownRange) s.warnings.push_back(r.name + ": " + r.originNote + "; counted on its own.");
      if (!r.exists) s.warnings.push_back(r.name + ": the file is missing on this computer.");
    }
    if (!haveFirst && !rows.empty()) first = rep.files[rows.front()].info;
    first = resplit(first, knownArtists);
    for (size_t ri : rows)
      if (!rep.files[ri].derived) {
        s.sourceName = rep.files[ri].name;
        break;
      }
    if (s.sourceName.empty() && !rows.empty()) s.sourceName = baseName(rep.files[rows.front()].origin.source);
    s.checkName = first.ambiguous && !first.artistKnown && s.fields.artist.empty() && s.fields.title.empty();
    s.artist = !s.fields.artist.empty() ? s.fields.artist : first.artist;
    s.title = !s.fields.title.empty() ? s.fields.title : (!first.title.empty() ? first.title : key);
    s.stems.assign(stems.begin(), stems.end());
    s.files.assign(files.begin(), files.end());
    // Timelines: union per part (stems of one song share a timeline).
    std::map<int, std::vector<Piece>> byPart;
    for (const auto& [part, p] : pieces[key]) byPart[part].push_back(p);
    for (auto& [part, ps] : byPart) {
      Timeline t;
      t.part = part;
      Ranges all;
      for (const Piece& p : ps) all.push_back(p.r);
      t.used = unite(all);
      t.seconds = total(t.used);
      for (const Range& x : t.used) {
        RangeFrom rf;
        rf.r = x;
        std::set<std::string> from;
        for (const Piece& p : ps)
          if (p.r.b > x.a && p.r.a < x.b) {
            from.insert(p.from);
            rf.estimated = rf.estimated || p.estimated;
          }
        rf.from.assign(from.begin(), from.end());
        t.parts.push_back(rf);
      }
      for (size_t ri : rows) {
        const FileRow& r = rep.files[ri];
        if (!r.derived && r.part == part) t.length = std::max(t.length, r.seconds);
      }
      for (const Range& x : t.used) t.length = std::max(t.length, x.b);
      s.timelines.push_back(t);
      s.seconds += t.seconds;
    }
    s.seconds = std::round(s.seconds * 10.0) / 10.0;
    s.tier = s.seconds > 0 ? tierFor(s.seconds) : 0;  // nothing played: no license needed
    s.over = s.seconds > kMaxLicensable;
    if (s.over) s.warnings.insert(s.warnings.begin(), "More than 60 s of this song is used: a sample license covers at most 60 s per song.");
    s.share = parseShare(s.fields.share);
    rep.songs.push_back(s);
  }
  std::sort(rep.songs.begin(), rep.songs.end(), [](const SongRow& a, const SongRow& b) {
    return a.artist != b.artist ? a.artist < b.artist : a.title < b.title;
  });
  rep.sharesComplete = !rep.songs.empty();
  for (const SongRow& s : rep.songs) {
    if (s.seconds <= 0) continue;
    if (s.share >= 0)
      rep.totalShare += s.share;
    else
      rep.sharesComplete = false;
  }
  if (rep.totalShare > 100) rep.warnings.push_back("The sample shares add up to more than 100%.");
  return rep;
}

}  // namespace gl::receipt
