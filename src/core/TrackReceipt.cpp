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
#include "ReceiptHashes.h"
#include "ReceiptOrigin.h"

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

double shareRate(const std::string& category, int tier) {
  const int row = tier == 2 ? 0 : tier == 15 ? 1 : tier == 60 ? 2 : -1;
  const int col = category == "A" ? 0 : category == "B" ? 1 : category == "C" ? 2 : -1;
  if (row < 0 || col < 0) return -1;
  static const double rates[3][3] = {{10, 5, 2}, {25, 15, 10}, {50, 30, 20}};
  return rates[row][col];
}

// (The origin sidecars are in ReceiptOrigin.cpp.)

// ---- saved fields

bool Saved::empty() const {
  if (!(release == Release{}) || !writers.empty() || !unknown.empty()) return false;
  if (!names.empty() || !origins.empty() || !heardFloor.empty() || !snapshots.empty()) return false;
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

// K-322: the known keys of each part of the saved object (everything else is kept as it was).
const char* const kSongKeys[] = {"artist", "title", "category", "share", "fee", "tier", "holders", "basis"};
const char* const kFileKeys[] = {"ticked", "song", "ranges", "seconds", "uses"};
const char* const kReleaseKeys[] = {"title", "performer", "licensee", "isrc", "date", "distributor"};

// Takes the known keys out of each entry of the object `key` of `j` (an entry left empty goes,
// and so does the object when nothing is left).
template <size_t N>
void dropKnownEntries(Json& j, const char* key, const char* const (&known)[N]) {
  auto it = j.object.find(key);
  if (it == j.object.end() || it->second.type != Json::Type::Object) {
    return;
  }
  for (auto e = it->second.object.begin(); e != it->second.object.end();) {
    if (e->second.type == Json::Type::Object) {
      for (const char* k : known) {
        e->second.object.erase(k);
      }
    }
    if (e->second.type == Json::Type::Object && e->second.object.empty()) {
      e = it->second.object.erase(e);
    } else {
      ++e;
    }
  }
  if (it->second.object.empty()) {
    j.object.erase(it);
  }
}

// The object `key` of `j` (made an object when it is not one).
Json& objectAt(Json& j, const std::string& key) {
  Json& o = j.object[key];
  if (o.type != Json::Type::Object) {
    o = Json::makeObject();
  }
  return o;
}
}  // namespace

std::string savedToJson(const Saved& s) {
  if (s.empty()) return "";
  Json j = Json::makeObject();
  if (!s.unknown.empty() && (!jsonParse(s.unknown, j) || j.type != Json::Type::Object)) {
    j = Json::makeObject();
  }
  if (!j.find("v")) {
    j.object["v"] = Json::makeNumber(1);
  }
  for (const auto& [k, f] : s.songs) {
    if (f == SongFields{}) continue;  // (its unknown keys, if any, are in j already)
    Json& o = objectAt(objectAt(j, "songs"), k);
    put(o, "artist", f.artist), put(o, "title", f.title), put(o, "category", f.category), put(o, "share", f.share);
    put(o, "fee", f.fee), put(o, "tier", f.tier), put(o, "holders", f.holders), put(o, "basis", f.basis);
  }
  for (const auto& [k, f] : s.files) {
    if (f == FileFields{}) continue;
    Json& o = objectAt(objectAt(j, "files"), k);
    if (f.ticked >= 0) o.object["ticked"] = Json::makeNumber(f.ticked);
    put(o, "song", f.song), put(o, "ranges", f.ranges), put(o, "seconds", f.seconds);
    if (!f.uses.empty()) {
      Json& u = objectAt(o, "uses");
      for (const auto& [key, text] : f.uses) u.object[key] = Json::makeString(text);
    }
  }
  // K-321: the project's trail and the newer settings.
  if (!s.names.empty()) {
    Json& o = objectAt(j, "names");
    for (const auto& [h, name] : s.names) o.object[h] = Json::makeString(name);
  }
  if (!s.origins.empty()) {
    Json& o = objectAt(j, "origins");
    for (const auto& [h, text] : s.origins) {
      Json v;
      if (jsonParse(text, v) && v.type == Json::Type::Object) o.object[h] = v;
    }
  }
  if (!s.heardFloor.empty()) put(objectAt(j, "heard"), "floor", s.heardFloor);
  if (!s.snapshots.empty()) {
    Json a = Json::makeArray();
    for (const std::string& text : s.snapshots) {
      Json v;
      if (jsonParse(text, v)) a.array.push_back(v);
    }
    j.object["snapshots"] = a;
  }
  if (!(s.release == Release{})) {
    Json& rel = objectAt(j, "release");
    put(rel, "title", s.release.title), put(rel, "performer", s.release.performer), put(rel, "licensee", s.release.licensee);
    put(rel, "isrc", s.release.isrc), put(rel, "date", s.release.date), put(rel, "distributor", s.release.distributor);
  }
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
      f.fee = o.text("fee"), f.tier = o.text("tier"), f.holders = o.text("holders"), f.basis = o.text("basis");
      s.songs[k] = f;
    }
  if (const Json* files = j.find("files"); files && files->type == Json::Type::Object)
    for (const auto& [k, o] : files->object) {
      FileFields f;
      f.ticked = std::clamp(jsonInt<int>(o.num("ticked", -1)), -1, 1);
      f.song = o.text("song"), f.ranges = o.text("ranges"), f.seconds = o.text("seconds");
      if (const Json* u = o.find("uses"); u && u->type == Json::Type::Object)
        for (const auto& [key, v] : u->object)
          if (v.type == Json::Type::String) f.uses[key] = v.str;
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
  // K-321: the project's trail and the newer settings.
  if (const Json* o = j.find("names"); o && o->type == Json::Type::Object)
    for (const auto& [h, v] : o->object)
      if (v.type == Json::Type::String) s.names[h] = v.str;
  if (const Json* o = j.find("origins"); o && o->type == Json::Type::Object)
    for (const auto& [h, v] : o->object)
      if (v.type == Json::Type::Object) s.origins[h] = jsonCompact(v);
  if (const Json* h = j.find("heard"); h && h->type == Json::Type::Object) s.heardFloor = h->text("floor");
  if (const Json* a = j.find("snapshots"); a && a->type == Json::Type::Array)
    for (const Json& v : a->array) s.snapshots.push_back(jsonCompact(v));
  // K-322: everything else is kept as it is (a newer Gridline's keys survive an edit here).
  Json rest = j;
  // (K-321: the known entries of the trail tables.)
  for (const char* table : {"names", "origins"}) {
    auto t = rest.object.find(table);
    if (t == rest.object.end() || t->second.type != Json::Type::Object) continue;
    const Json::Type want = std::string(table) == "names" ? Json::Type::String : Json::Type::Object;
    for (auto e = t->second.object.begin(); e != t->second.object.end();)
      e = e->second.type == want ? t->second.object.erase(e) : std::next(e);
    if (t->second.object.empty()) rest.object.erase(t);
  }
  if (auto h = rest.object.find("heard"); h != rest.object.end() && h->second.type == Json::Type::Object) {
    if (const Json* fl = h->second.find("floor"); fl && fl->type == Json::Type::String) h->second.object.erase("floor");
    if (h->second.object.empty()) rest.object.erase(h);
  }
  if (const Json* a = rest.find("snapshots"); a && a->type == Json::Type::Array) rest.object.erase("snapshots");
  dropKnownEntries(rest, "songs", kSongKeys);
  dropKnownEntries(rest, "files", kFileKeys);
  if (auto r = rest.object.find("release"); r != rest.object.end() && r->second.type == Json::Type::Object) {
    for (const char* k : kReleaseKeys) {
      r->second.object.erase(k);
    }
    if (r->second.object.empty()) {
      rest.object.erase(r);
    }
  }
  if (const Json* a = rest.find("writers"); a && a->type == Json::Type::Array) {
    rest.object.erase("writers");
  }
  if (const Json* v = rest.find("v"); v && v->type == Json::Type::Number && v->number == 1) {
    rest.object.erase("v");  // what savedToJson writes when there is none
  }
  if (!rest.object.empty()) {
    s.unknown = jsonCompact(rest);
  }
  return s;
}

FileFields fileFields(const Saved& s, const std::string& path, const std::string& hash) {
  auto it = s.files.find(path);
  if (it != s.files.end()) return it->second;
  if (!hash.empty())
    if (auto h = s.files.find("sha256:" + hash); h != s.files.end()) return h->second;
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
  // K-321: names from the project's table, for files whose own name says nothing (renamed, or a
  // collab copy named by its hash).
  std::map<std::string, std::string> hashOfPath;
  for (const FileIn& f : m.files)
    if (!f.hash.empty()) hashOfPath[f.path] = f.hash;
  bool renamed = false;
  const auto nameOf = [&](const std::string& path) {
    NameInfo n = parseName(path);
    renamed = false;
    if (n.tracklib || saved.names.empty()) return n;
    std::string h = path.rfind("sha256:", 0) == 0 ? path.substr(7) : std::string();
    if (h.empty())
      if (auto it = hashOfPath.find(path); it != hashOfPath.end()) h = it->second;
    if (h.empty()) h = hashFromName(path);
    if (auto it = saved.names.find(h); !h.empty() && it != saved.names.end()) {
      NameInfo t = parseName(it->second);
      if (t.tracklib) return renamed = true, t;
    }
    return n;
  };
  // Files.
  std::map<std::string, size_t> rowOf;
  for (const FileIn& f : m.files) {
    if (rowOf.count(f.path)) continue;
    FileRow r;
    r.path = f.path;
    r.name = baseName(f.path);
    r.info = nameOf(f.path);
    if (renamed) r.originNote = "named from the project (file renamed)";
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
    const FileFields ff = fileFields(saved, r.path, fin ? fin->hash : std::string());
    NameInfo src = r.info;  // the original's name (derived files: their source)
    if (r.derived) {
      src = nameOf(r.origin.source);
      r.originNote = "derived from " + baseName(r.origin.source) + (r.origin.kind.empty() ? "" : " (" + r.origin.kind + ")");
    }
    // K-321: the trail of version 2 maps (several originals at once, each with its parts).
    std::map<std::string, OriginV2> trail;
    if (r.derived && fin)
      for (const auto& [file, text] : fin->trail) {
        Json j;
        OriginV2 o;
        if (jsonParse(text, j) && originFromJson(j, o)) trail[file] = o;
      }
    const bool traced = !trail.empty();
    const Lookup trailLk = traced ? trailLookup(trail) : Lookup{};
    r.autoTicked = src.tracklib;
    if (traced && !src.tracklib)  // ticked when any original it traces to is a Tracklib file
      for (const auto& [k, st] : trace(r.path, {{0, std::max(1e9, r.seconds)}}, r.seconds, trailLk)) r.autoTicked = r.autoTicked || nameOf(k).tracklib;
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
      // start (a deck cueing, a slice retrigger), not a part anyone hears: left out. K-321: the
      // tap flags those itself (measure() left them out), so its other short reads stay.
      for (const Range& x : unite(u.ranges))
        if (u.estimated || u.measured || x.b - x.a >= kMinHeard) ur.push_back(x);
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
    const std::string label = r.info.stem.empty() ? r.name : r.info.stem;
    if (traced) {
      // K-321: each use's parts on every original it traces to, each on that original's song
      // and part (the user's song for the file, when set, takes them all).
      bool any = false;
      for (const auto& [ur, u] : per) {
        std::vector<std::string> warns;
        for (const auto& [orig, st] : trace(r.path, ur, r.seconds, trailLk, &warns)) {
          const NameInfo on = nameOf(orig);
          const std::string song = !ff.song.empty() ? ff.song : on.tracklib ? on.song : stripExtension(baseName(orig));
          int part = st.unknown ? -1 : on.part;
          if (st.unknown) r.unknownRange = true;
          Ranges mapped = st.ranges;
          if (on.loopEnd > on.loopStart && part >= 0) {
            Ranges shifted;
            for (const Range& x : mapped) {
              const double a = std::min(on.loopEnd, on.loopStart + x.a), b = std::min(on.loopEnd, on.loopStart + x.b);
              if (b > a) shifted.push_back({a, b});
            }
            mapped = shifted;
          }
          auto& rowsOf = songRows[song];
          if (std::find(rowsOf.begin(), rowsOf.end(), ri) == rowsOf.end()) rowsOf.push_back(ri);
          for (const Range& x : mapped) {
            Piece pc;
            pc.r = x;
            pc.from = (u ? u->where : std::string("typed by you"));
            if (u && u->bar0 > 0) {
              const int b0 = static_cast<int>(u->bar0), b1 = static_cast<int>(u->bar1);
              pc.from += b1 > b0 ? ", bars " + std::to_string(b0) + "-" + std::to_string(b1) : ", bar " + std::to_string(b0);
            }
            pc.from += " (" + label + ")";
            pc.estimated = (u && u->estimated && !r.manual);
            pieces[song].push_back({part, pc});
            any = true;
          }
        }
        for (const std::string& w : warns)
          if (std::find(rep.warnings.begin(), rep.warnings.end(), w) == rep.warnings.end()) rep.warnings.push_back(w);
      }
      if (!any) songRows[r.song].push_back(ri);
      if (r.unknownRange) r.originNote += ", range unknown";
      continue;
    }
    songRows[r.song].push_back(ri);
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
            const NameInfo on = nameOf(other);
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
    for (const NameInfo& n : {r.info, r.derived ? nameOf(r.origin.source) : NameInfo{}})
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
      NameInfo src = r.derived ? nameOf(r.origin.source) : r.info;
      if (r.derived && src.song != key)  // a render of several originals: the one of this song
        for (const std::string& other : r.origin.sources)
          if (const NameInfo on = nameOf(other); on.song == key) src = on;
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
    if (!s.over) s.autoShare = shareRate(s.fields.category, s.tier);
    s.share = trim(s.fields.share).empty() ? s.autoShare : parseShare(s.fields.share);
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
