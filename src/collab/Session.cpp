// Gridline - J-111 collaboration: session. See Session.h and DESIGN notes in docs/collaboration.md.
#include "Session.h"
#include "LocalState.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/PluginInfo.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../core/Update.h"
#include "../engine/Sample.h"

namespace fs = std::filesystem;

namespace gl::collab {

namespace {

constexpr const char* kDir = ".gridline-collab";


double nowMsOf(const std::function<uint64_t()>& f) {
  if (f) return static_cast<double>(f());
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string lowerExt(const std::string& p) {
  std::string e = fs::path(p).extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (e.size() > 12) e = e.substr(0, 12);
  return e;
}

bool isHexName(const std::string& stem) {
  if (stem.size() != 64) return false;
  for (char c : stem)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// A safe "Audio/<sha256>.<ext>" name (no folders, no dots tricks).
bool safeAudioName(const std::string& rel) {
  if (rel.rfind("Audio/", 0) != 0) return false;
  const std::string f = rel.substr(6);
  if (f.empty() || f.find('/') != std::string::npos || f.find('\\') != std::string::npos || f.find("..") != std::string::npos)
    return false;
  const size_t dot = f.find('.');
  return isHexName(dot == std::string::npos ? f : f.substr(0, dot));
}

Json memberJson(const Member& m) {
  Json j = Json::makeObject();
  j.object["sign"] = Json::makeString(crypto::toHex(m.signPublic.data(), 32));
  j.object["dh"] = Json::makeString(crypto::toHex(m.dhPublic.data(), 32));
  j.object["slot"] = Json::makeNumber(m.slot);
  j.object["name"] = Json::makeString(m.name);
  j.object["by"] = Json::makeString(siteHex(m.by));
  j.object["joined"] = Json::makeNumber(static_cast<double>(m.joinedMs));
  return j;
}

bool memberFrom(const std::string& key, const Json& j, Member& m) {
  m.site = siteFromHex(key);
  std::string s, d;
  if (!crypto::fromHex(j.text("sign"), s) || !crypto::keyFrom(s, m.signPublic)) return false;
  if (crypto::fromHex(j.text("dh"), d)) crypto::keyFrom(d, m.dhPublic);
  if (crypto::siteOf(m.signPublic) != m.site) return false;  // the key decides the id
  m.slot = static_cast<int>(j.num("slot", 0));
  m.name = j.text("name");
  m.by = siteFromHex(j.text("by"));
  m.joinedMs = static_cast<uint64_t>(j.num("joined", 0));
  return true;
}

// Plugin objects inside a project JSON (they have "type" and "params").
void forEachPluginJson(Json& j, const std::function<void(Json&)>& fn, int depth = 0) {
  if (depth > 40) return;
  if (j.is(Json::Type::Object)) {
    const Json* t = j.find("type");
    const Json* p = j.find("params");
    if (t && t->is(Json::Type::String) && p && p->is(Json::Type::Array)) fn(j);
    for (auto& [k, v] : j.object) forEachPluginJson(v, fn, depth + 1);
  } else if (j.is(Json::Type::Array)) {
    for (auto& v : j.array) forEachPluginJson(v, fn, depth + 1);
  }
}

Json floToJson(const FlOrigin& f) {
  Json j = Json::makeObject();
  for (const auto& [k, v] : f.blobs) j.object[k] = Json::makeString(crypto::toB64url(v));
  return j;
}

FlOrigin floFromJson(const Json& j) {
  FlOrigin f;
  if (!j.is(Json::Type::Object)) return f;
  for (const auto& [k, v] : j.object) {
    std::string bytes;
    if (v.is(Json::Type::String) && crypto::fromB64url(v.str, bytes)) f.blobs[k] = std::move(bytes);
  }
  return f;
}

std::string readCollabFileText(const std::string& folder) {
  std::string t;
  readFileText((fs::path(folder) / kDir / "collab.json").string(), t, 1 << 20);
  return t;
}

// ---- element JSON (J-111 incremental diff / apply): songToJson of a song
// holding only that element, so the format is exactly a project file's.
const char* const kLists[] = {"channels", "patterns", "playlist", "mixer", "automations"};
bool isList(const std::string& k) {
  for (const char* l : kLists)
    if (k == l) return true;
  return false;
}
const char* const kScalars[] = {"tempo", "swing", "masterPitch"};
bool isScalar(const std::string& k) {
  for (const char* l : kScalars)
    if (k == l) return true;
  return false;
}

Song skeleton(const Song& cur) {
  Song t;
  t.tracks.clear();
  t.ppq = cur.ppq;
  return t;
}

Json listJsonOf(const Song& cur, const std::string& kind, int only) {
  Song t = skeleton(cur);
  auto pick = [only](auto& dst, const auto& src) {
    if (only >= 0) dst.push_back(src[static_cast<size_t>(only)]);
    else dst = src;
  };
  if (kind == "channels") pick(t.channels, cur.channels);
  else if (kind == "patterns") pick(t.patterns, cur.patterns);
  else if (kind == "playlist") pick(t.playlist, cur.playlist);
  else if (kind == "mixer") pick(t.mixer, cur.mixer);
  else if (kind == "automations") pick(t.automations, cur.automations);
  Json j = songToJson(t, false);
  const Json* l = j.find(kind);
  return l ? *l : Json();  // Null: the key is not written (an empty optional list)
}

// Everything but the five lists and flOrigin (small: compared and diffed whole).
Song restOf(const Song& c) {
  Song t;
  t.info = c.info;
  t.ppq = c.ppq;
  t.tempo = c.tempo;
  t.timeSigNum = c.timeSigNum;
  t.timeSigDen = c.timeSigDen;
  t.swing = c.swing;
  t.masterPitch = c.masterPitch;
  t.nextId = c.nextId;
  t.tracks = c.tracks;
  t.links = c.links;
  t.modLinks = c.modLinks;
  t.markers = c.markers;
  t.channelGroups = c.channelGroups;
  t.patternGroups = c.patternGroups;
  t.live = c.live;
  t.arrangements = c.arrangements;
  t.currentArrangement = c.currentArrangement;
  t.autoCrossfades = c.autoCrossfades;
  return t;
}

bool restEqual(const Song& a, const Song& b) {
  return a.info == b.info && a.ppq == b.ppq && a.tempo == b.tempo && a.timeSigNum == b.timeSigNum && a.timeSigDen == b.timeSigDen &&
         a.swing == b.swing && a.masterPitch == b.masterPitch && a.nextId == b.nextId && a.tracks == b.tracks && a.links == b.links &&
         a.modLinks == b.modLinks && a.markers == b.markers && a.channelGroups == b.channelGroups && a.patternGroups == b.patternGroups &&
         a.live == b.live && a.arrangements == b.arrangements && a.currentArrangement == b.currentArrangement &&
         a.autoCrossfades == b.autoCrossfades;
}

template <typename T>
std::vector<ObjectId> idsOf(const std::vector<T>& v) {
  std::vector<ObjectId> out;
  out.reserve(v.size());
  for (const auto& x : v) out.push_back(x.id);
  return out;
}

bool hostedIn(const Json& j, std::string* which) {
  bool bad = false;
  Json copy = j;
  forEachPluginJson(copy, [&](Json& plug) {
    const std::string t = plug.text("type");
    if (isHostedType(t)) {
      bad = true;
      if (which && which->empty()) {
        const PluginInfo* pi = findPlugin(t);
        *which = pi ? pi->name : t;
      }
    }
  });
  return bad;
}

// this computer's session fields survive a collaborator's change of the element (LocalState.h)
void keepLocal(Channel& e, const Channel& o) { keepChannelSession(e, o); }
void keepLocal(MixerTrack& e, const MixerTrack& o) { keepMixerSession(e, o); }
void keepLocal(Clip& e, const Clip& o) { keepClipSession(e, o); }
void keepLocal(Pattern&, const Pattern&) {}
void keepLocal(AutomationClip&, const AutomationClip&) {}

Song withoutOrigin(const Song& s) {
  Song c = s;
  c.flOrigin = FlOrigin();
  return c;
}

}  // namespace

// ---------------------------------------------------------------- repair ----
void repairProjectJson(Json& j) {
  if (!j.is(Json::Type::Object)) return;
  auto clampNum = [](Json& o, const char* key, double lo, double hi, double dflt) {
    Json* v = o.is(Json::Type::Object) ? &o.object[key] : nullptr;
    if (!v) return;
    if (!v->is(Json::Type::Number)) *v = Json::makeNumber(dflt);
    v->number = std::clamp(v->number, lo, hi);
  };
  if (Json* p = j.find("ppq") ? &j.object["ppq"] : nullptr; p && (!p->is(Json::Type::Number) || !isValidPpq(static_cast<int>(p->number))))
    *p = Json::makeNumber(kDefaultPpq);
  if (j.find("tempo")) clampNum(j, "tempo", 10.0, 522.0, 140.0);
  if (j.find("timeSigNum")) clampNum(j, "timeSigNum", 1, 64, 4);
  if (j.find("timeSigDen")) clampNum(j, "timeSigDen", 1, 64, 4);
  std::set<double> ids;
  auto uniqueList = [&](Json* arr) {
    if (!arr || !arr->is(Json::Type::Array)) return;
    std::vector<Json> keep;
    for (auto& e : arr->array) {
      const double id = e.num("id", 0);
      if (id < kFirstObjectId || !ids.insert(id).second) continue;  // duplicate id after a merge: the later one goes
      keep.push_back(std::move(e));
    }
    arr->array = std::move(keep);
  };
  Json* mixer = j.find("mixer") ? &j.object["mixer"] : nullptr;
  uniqueList(mixer);
  if (mixer && mixer->is(Json::Type::Array)) {
    std::set<double> mids;
    for (const auto& m : mixer->array) mids.insert(m.num("id", 0));
    std::map<double, std::vector<double>> edges;  // accepted sends
    std::function<bool(double, double, std::set<double>&)> reaches = [&](double a, double b, std::set<double>& seen) {
      if (a == b) return true;
      if (!seen.insert(a).second) return false;
      for (double n : edges[a])
        if (reaches(n, b, seen)) return true;
      return false;
    };
    for (auto& m : mixer->array) {
      Json* sends = m.find("sends") ? &m.object["sends"] : nullptr;
      if (!sends || !sends->is(Json::Type::Array)) continue;
      const double from = m.num("id", 0);
      std::vector<Json> keep;
      std::set<double> seenTo;
      for (auto& sd : sends->array) {
        const double to = sd.num("to", 0);
        std::set<double> seen;
        if (!mids.count(to) || to == from || seenTo.count(to) || reaches(to, from, seen)) continue;
        seenTo.insert(to);
        edges[from].push_back(to);
        keep.push_back(std::move(sd));
      }
      sends->array = std::move(keep);
    }
  }
  uniqueList(j.find("channels") ? &j.object["channels"] : nullptr);
  uniqueList(j.find("patterns") ? &j.object["patterns"] : nullptr);
  uniqueList(j.find("automations") ? &j.object["automations"] : nullptr);
  std::set<double> pats, autos, chans;
  if (const Json* a = j.find("patterns"); a && a->is(Json::Type::Array))
    for (const auto& p : a->array) pats.insert(p.num("id", 0));
  if (const Json* a = j.find("automations"); a && a->is(Json::Type::Array))
    for (const auto& p : a->array) autos.insert(p.num("id", 0));
  if (const Json* a = j.find("channels"); a && a->is(Json::Type::Array))
    for (const auto& p : a->array) chans.insert(p.num("id", 0));
  if (Json* a = j.find("patterns") ? &j.object["patterns"] : nullptr; a && a->is(Json::Type::Array))
    for (auto& p : a->array) {
      clampNum(p, "stepsPerBar", 1, kMaxStepsPerBar, 16);
      clampNum(p, "bars", 1, kMaxPatternBars, 1);
    }
  auto fixClips = [&](Json* arr) {
    if (!arr || !arr->is(Json::Type::Array)) return;
    std::vector<Json> keep;
    for (auto& c : arr->array) {
      const double id = c.num("id", 0);
      if (id < kFirstObjectId || !ids.insert(id).second) continue;
      const std::string kind = c.text("kind");
      if (kind.empty() && !pats.count(c.num("pattern", 0))) continue;  // its pattern was deleted meanwhile
      if (kind == "audio" && !chans.count(c.num("source", 0))) continue;
      if (kind == "automation" && !autos.count(c.num("source", 0))) continue;
      clampNum(c, "track", 0, kMaxPlaylistTracks - 1, 0);
      if (c.num("start", 0) < 0) c.object["start"] = Json::makeNumber(0);
      if (c.num("length", 0) < 0) c.object["length"] = Json::makeNumber(0);
      keep.push_back(std::move(c));
    }
    arr->array = std::move(keep);
  };
  fixClips(j.find("playlist") ? &j.object["playlist"] : nullptr);
  if (Json* arrs = j.find("arrangements") ? &j.object["arrangements"] : nullptr; arrs && arrs->is(Json::Type::Array))
    for (auto& a : arrs->array)
      if (a.is(Json::Type::Object) && a.find("playlist")) fixClips(&a.object["playlist"]);
}

std::vector<std::pair<std::string, std::string>> foreignPlugins(const Json& song) {
  std::vector<std::pair<std::string, std::string>> out;
  auto check = [&](const Json* plug, const std::string& where) {
    if (!plug || !plug->is(Json::Type::Object)) return;
    const std::string type = plug->text("type");
    if (type.empty()) return;
    if (isHostedType(type)) {
      std::string label = type;
      if (const PluginInfo* pi = findPlugin(type)) label = pi->name;
      out.emplace_back(label, where);
    }
  };
  if (const Json* chans = song.find("channels"); chans && chans->is(Json::Type::Array))
    for (const auto& c : chans->array) {
      const std::string where = i18n::tr("Channel") + std::string(" '") + c.text("name") + "'";
      check(c.find("plugin"), where);
      if (const Json* nf = c.find("noteFx"); nf && nf->is(Json::Type::Array))
        for (const auto& f : nf->array) check(&f, where + " " + i18n::tr("note processor"));
    }
  if (const Json* mx = song.find("mixer"); mx && mx->is(Json::Type::Array)) {
    int idx = 0;
    for (const auto& m : mx->array) {
      std::string name = m.text("name");
      if (name.empty()) name = idx == 0 ? i18n::tr("Master") : i18n::tr("Insert") + std::string(" ") + std::to_string(idx);
      if (const Json* sl = m.find("slots"); sl && sl->is(Json::Type::Array))
        for (size_t s = 0; s < sl->array.size(); ++s)
          check(sl->array[s].find("plugin"), i18n::tr("Mixer") + std::string(" '") + name + "' " + i18n::tr("slot") + " " +
                                                 std::to_string(s + 1));
      ++idx;
    }
  }
  return out;
}

// ------------------------------------------------------------ the name ----
namespace {

// One code point of UTF-8 at `i` (advanced); false for a malformed sequence (skipped by one byte).
bool nextCodePoint(const std::string& s, size_t& i, uint32_t& cp, size_t& len) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  if (len == 0 || i + len > s.size()) {
    ++i;
    return false;
  }
  cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (size_t k = 1; k < len; ++k) {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) {
      ++i;
      return false;
    }
    cp = (cp << 6) | (d & 0x3Fu);
  }
  static const uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {  // overlong, out of range, surrogates
    ++i;
    return false;
  }
  i += len;
  return true;
}

bool isNameSpace(uint32_t cp) {
  return cp <= 0x20 || cp == 0x7F || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// Invisible characters that only confuse a name: C1 controls, zero-width and direction controls, BOM, specials.
bool isNameInvisible(uint32_t cp) {
  return (cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF0 && cp <= 0xFFFF);
}

bool reservedDeviceName(std::string base) {
  for (char& c : base) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$") return true;
  return base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base[3] >= '0' && base[3] <= '9';
}

bool hasProjectExt(const std::string& f) {
  const std::string ext = kProjectFileExt;
  return f.size() > ext.size() && f.compare(f.size() - ext.size(), ext.size(), ext) == 0;
}

// A plain file name (no folders, no "..") as stored in collab.json.
bool plainFileName(const std::string& f) {
  return !f.empty() && f.find('/') == std::string::npos && f.find('\\') == std::string::npos && f != "." && f != ".." && hasProjectExt(f);
}

}  // namespace

std::string cleanProjectName(const std::string& raw) {
  std::string out;
  bool space = true;  // (leading spaces go)
  size_t cps = 0, i = 0;
  while (i < raw.size() && cps < kMaxProjectNameChars) {
    const size_t at = i;
    uint32_t cp = 0;
    size_t len = 0;
    if (!nextCodePoint(raw, i, cp, len)) continue;  // malformed UTF-8 is dropped
    if (isNameInvisible(cp)) continue;
    if (isNameSpace(cp)) {
      if (!space) {
        out += ' ';
        ++cps;
      }
      space = true;
      continue;
    }
    out.append(raw, at, len);
    ++cps;
    space = false;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool validProjectName(const std::string& name) { return !name.empty() && cleanProjectName(name) == name; }

std::string projectFileStem(const std::string& name) {
  const std::string clean = cleanProjectName(name);
  std::string out;
  for (char ch : clean) {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool bad = c < 0x20 || c == 0x7F || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                     ch == '>' || ch == '|';
    out += bad ? '-' : ch;
  }
  auto trim = [](std::string& t) {
    size_t a = 0;
    while (a < t.size() && (t[a] == '.' || t[a] == ' ')) ++a;  // no hidden files, no ".."
    t.erase(0, a);
    while (!t.empty() && (t.back() == '.' || t.back() == ' ')) t.pop_back();  // Windows drops these
  };
  trim(out);
  constexpr size_t kMaxStemBytes = 120;  // + " (99).gridline.json" stays far below 255 bytes
  if (out.size() > kMaxStemBytes) {
    size_t cut = kMaxStemBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    trim(out);
  }
  if (out.empty()) return "Project";
  const size_t dot = out.find('.');
  if (reservedDeviceName(out.substr(0, dot))) out.insert(dot == std::string::npos ? out.size() : dot, "_");
  return out;
}

std::string Session::renameProjectFileIn(const std::string& folder, const std::string& from, const std::string& stemIn, std::string* err) {
  const fs::path dir(folder);
  const std::string stem = stemIn.empty() ? std::string("Project") : stemIn;
  std::error_code ec;
  const fs::path cur = dir / from;
  const bool have = plainFileName(from) && fs::is_regular_file(cur, ec);
  for (int k = 1; k <= 99; ++k) {
    const std::string cand = stem + (k == 1 ? std::string() : " (" + std::to_string(k) + ")") + kProjectFileExt;
    if (cand == from) return from;
    const fs::path target = dir / cand;
    if (fs::exists(target, ec)) {
      if (have && fs::equivalent(cur, target, ec) && !ec) {
        // the same file under another spelling (a case-insensitive or normalising file system):
        // through a temporary name, so the new spelling is really written
        const fs::path tmp = dir / ".gridline-rename.tmp";
        fs::remove(tmp, ec);
        fs::rename(cur, tmp, ec);
        if (!ec) {
          fs::rename(tmp, target, ec);
          if (!ec) return cand;
          std::error_code back;
          fs::rename(tmp, cur, back);
        }
        if (err) *err = ec.message();
        return from;
      }
      continue;  // another file has that name: never replaced
    }
    if (!have) return cand;  // nothing written yet: the name is simply taken
    fs::rename(cur, target, ec);  // atomic on one file system
    if (ec) {
      if (err) *err = ec.message();  // e.g. the file is open in another program (Windows)
      return from;
    }
    return cand;
  }
  if (err) *err = "no free file name";
  return from;
}

// ------------------------------------------------------------- creation ----
Session::Session(Document& doc, Config cfg) : doc_(doc), cfg_(std::move(cfg)) {
  me_ = cfg_.identity.site();
  if (cfg_.wallMs) hlc_.wallMs = cfg_.wallMs;
}

Session::~Session() {
  if (!stopped_) saveNow();
  detach();
  log_.close();
}

bool Session::isCollabFolder(const std::string& folder) {
  if (folder.empty()) return false;
  const std::string t = readCollabFileText(folder);
  Json j;
  if (t.empty() || !jsonParse(t, j)) return false;
  return j.text("format") == "gridline-collab" && j.num("left", 0) == 0;
}

std::string Session::folderOfProjectFile(const std::string& projectFile) {
  if (projectFile.empty()) return {};
  const std::string dir = fs::path(projectFile).parent_path().string();
  return isCollabFolder(dir) ? dir : std::string();
}

std::string Session::projectIdOf(const std::string& folder) {
  Json j;
  if (!jsonParse(readCollabFileText(folder), j)) return {};
  return j.text("id");
}

std::string Session::projectNameOf(const std::string& folder) {
  Json j;
  if (!jsonParse(readCollabFileText(folder), j)) return {};
  return j.text("name");
}

std::string Session::projectFileOf(const std::string& folder) {
  const fs::path dir(folder);
  Json cj;
  jsonParse(readCollabFileText(folder), cj);
  std::error_code ec;
  auto isFile = [&](const std::string& f) { return plainFileName(f) && fs::is_regular_file(dir / f, ec); };
  const std::string rec = cj.text("file"), name = cj.text("name");
  if (isFile(rec)) return (dir / rec).string();
  if (!name.empty()) {
    if (const std::string f = projectFileStem(name) + kProjectFileExt; isFile(f)) return (dir / f).string();
    if (const std::string f = name + kProjectFileExt; isFile(f)) return (dir / f).string();  // projects made before J-111f
  }
  // renamed by hand, or a rename that a crash interrupted before collab.json was written:
  // the folder's project file is the one Gridline wrote last
  std::vector<fs::path> found;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string f = e.path().filename().string();
    if (f.empty() || f[0] == '.' || !hasProjectExt(f) || !e.is_regular_file(ec)) continue;
    found.push_back(e.path());
  }
  std::sort(found.begin(), found.end());
  if (found.size() > 1) {
    std::string written;
    readFileText((dir / kDir / "written.txt").string(), written, 1 << 10);
    for (const auto& f : found) {
      std::string text;
      if (!written.empty() && readFileText(f.string(), text) && update::sha256Hex(text) == written) return f.string();
    }
    return std::max_element(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
             return fs::last_write_time(a, ec) < fs::last_write_time(b, ec);
           })->string();
  }
  if (!found.empty()) return found.front().string();
  return (dir / (plainFileName(rec) ? rec : projectFileStem(name.empty() ? std::string("Project") : name) + kProjectFileExt)).string();
}

std::string Session::minAppVersionOf(const std::string& folder) {
  Json j;
  if (!jsonParse(readCollabFileText(folder), j)) return {};
  return j.text("minAppVersion");
}

std::string Session::localPath(const std::string& rel) const {
  return (fs::path(cfg_.folder) / fs::path(rel)).lexically_normal().string();
}

std::string Session::projectFile() const { return (fs::path(cfg_.folder) / fileName_).string(); }

uint64_t Session::wallNowMs() const { return static_cast<uint64_t>(nowMsOf(cfg_.wallMs)); }

std::string Session::importFile(const std::string& absPath) {
  std::error_code ec;
  if (!fs::is_regular_file(absPath, ec)) return {};
  const std::string h = update::sha256File(absPath);
  if (h.empty()) return {};
  const std::string name = h + lowerExt(absPath);
  const fs::path dst = fs::path(cfg_.folder) / "Audio" / name;
  fs::create_directories(dst.parent_path(), ec);
  if (!fs::exists(dst, ec)) {
    const fs::path tmp = dst.string() + ".tmp";
    fs::copy_file(absPath, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return {};
    fs::rename(tmp, dst, ec);
    if (ec) return {};
  }
  return "Audio/" + name;
}

std::unique_ptr<Session> Session::create(Document& doc, Config cfg, std::string* err) {
  if (!cfg.identity.valid) {
    if (err) *err = "no identity";
    return nullptr;
  }
  std::error_code ec;
  fs::create_directories(fs::path(cfg.folder) / kDir, ec);
  fs::create_directories(fs::path(cfg.folder) / "Audio", ec);
  if (ec) {
    if (err) *err = i18n::tr("Cannot create the project folder") + std::string(": ") + cfg.folder;
    return nullptr;
  }
  cfg.name = cleanProjectName(cfg.name);
  if (cfg.name.empty()) cfg.name = "Project";
  std::unique_ptr<Session> s(new Session(doc, std::move(cfg)));
  s->creator_ = s->me_;
  s->creatorSign_ = s->cfg_.identity.signPublic;
  s->projectId_ = crypto::toHex(crypto::randomKey().data(), 16);
  // a file of that name already in the folder is left alone (the project takes "<name> (2)")
  s->fileName_ = renameProjectFileIn(s->cfg_.folder, std::string(), projectFileStem(s->cfg_.name));
  if (!s->bootstrap(err)) return nullptr;
  s->shownName_ = s->projectName();
  s->nameReady_ = true;
  return s;
}

bool Session::bootstrap(std::string* err) {
  // collect the project's files into Audio/ (content addressed)
  std::map<std::string, std::string> moved;
  Song copy = doc_.song();
  forEachFileRef(copy, [&](std::string& p) {
    if (p.empty() || !fs::path(p).is_absolute()) return;
    auto it = moved.find(p);
    if (it == moved.end()) {
      const std::string rel = importFile(p);
      it = moved.emplace(p, rel.empty() ? p : localPath(rel)).first;
    }
    p = it->second;
  });
  if (!moved.empty()) doc_.patchSong([&](Song& s) { forEachFileRef(s, [&](std::string& p) {
      auto it = moved.find(p);
      if (it != moved.end()) p = it->second;
    }); });
  // collab.json
  Json cj = Json::makeObject();
  cj.object["format"] = Json::makeString("gridline-collab");
  cj.object["version"] = Json::makeNumber(1);
  cj.object["id"] = Json::makeString(projectId_);
  cj.object["name"] = Json::makeString(cfg_.name);
  cj.object["file"] = Json::makeString(fileName_);
  cj.object["creator"] = Json::makeString(siteHex(creator_));
  cj.object["creatorSign"] = Json::makeString(crypto::toHex(creatorSign_.data(), 32));
  cj.object["minAppVersion"] = Json::makeString(cfg_.appVersion);
  if (!writeFileDurable((fs::path(cfg_.folder) / kDir / "collab.json").string(), jsonStringify(cj, 2), err)) return false;
  Json snap;
  std::vector<std::string> recs;
  if (!log_.open((fs::path(cfg_.folder) / kDir).string(), snap, recs, err)) return false;
  // genesis
  Member me;
  me.site = me_;
  me.signPublic = cfg_.identity.signPublic;
  me.dhPublic = cfg_.identity.dhPublic;
  me.slot = 0;
  me.name = cleanName(cfg_.profile.name);
  me.by = me_;
  me.joinedMs = wallNowMs();
  Json meta = Json::makeObject();
  Json proj = Json::makeObject();
  proj.object["id"] = Json::makeString(projectId_);
  proj.object["name"] = Json::makeString(cfg_.name);
  proj.object["creator"] = Json::makeString(siteHex(creator_));
  meta.object["project"] = std::move(proj);
  Json members = Json::makeObject();
  members.object[siteHex(me_)] = memberJson(me);
  meta.object["members"] = std::move(members);
  meta.object["admin"] = Json::makeString(siteHex(me_));
  Json versions = Json::makeObject();
  versions.object[siteHex(me_)] = Json::makeString(cfg_.appVersion);
  meta.object["versions"] = std::move(versions);
  meta.object["resolutions"] = Json::makeObject();
  std::vector<Op> ops(3);
  ops[0].kind = OpKind::Ins;
  ops[0].t = hlc_.tick();
  ops[0].path = {"meta"};
  ops[0].value = std::move(meta);
  ops[1].kind = OpKind::Ins;
  ops[1].t = hlc_.tick();
  ops[1].path = {"song"};
  ops[1].value = localSongJson();
  ops[2].kind = OpKind::Ins;
  ops[2].t = hlc_.tick();
  ops[2].path = {"flo"};
  ops[2].value = floToJson(doc_.song().flOrigin);
  floShadow_ = doc_.song().flOrigin;
  floCache_ = floShadow_;
  floDirty_ = false;
  ready_ = true;
  if (!makeLocalBatch(std::move(ops), false, false)) {
    if (err) *err = "cannot start the collaboration log";
    return false;
  }
  doc_.patchSong([&](Song& s) {
    s.idSlot = 0;
    s.idStride = kIdStride;
  });
  shadow_ = withoutOrigin(doc_.song());
  shadowValid_ = true;
  attach();
  writeProjectFile();
  saveNow();
  return true;
}

std::unique_ptr<Session> Session::open(Document& doc, Config cfg, std::string* err) {
  Json cj;
  if (!jsonParse(readCollabFileText(cfg.folder), cj) || cj.text("format") != "gridline-collab") {
    if (err) *err = i18n::tr("This folder is not a collaborative project.");
    return nullptr;
  }
  std::unique_ptr<Session> s(new Session(doc, std::move(cfg)));
  s->projectId_ = cj.text("id");
  if (s->cfg_.name.empty()) s->cfg_.name = cj.text("name");
  s->cfg_.name = cleanProjectName(s->cfg_.name);
  s->fileName_ = fs::path(projectFileOf(s->cfg_.folder)).filename().string();  // wherever it is, whatever it is called
  s->creator_ = siteFromHex(cj.text("creator"));
  std::string cs;
  if (!crypto::fromHex(cj.text("creatorSign"), cs) || !crypto::keyFrom(cs, s->creatorSign_)) {
    if (err) *err = "collab.json: creator key missing";
    return nullptr;
  }
  if (!s->loadFromDisk(err)) return nullptr;
  s->shownName_ = s->projectName();
  s->nameReady_ = s->ready_;
  s->syncFileName();  // renamed by a sync whose file rename did not happen (the file was open elsewhere)
  s->writeCollabJson();
  return s;
}

std::unique_ptr<Session> Session::join(Document& doc, Config cfg, const Ticket& t, std::string* err) {
  std::error_code ec;
  fs::create_directories(fs::path(cfg.folder) / kDir, ec);
  fs::create_directories(fs::path(cfg.folder) / "Audio", ec);
  if (ec) {
    if (err) *err = i18n::tr("Cannot create the project folder") + std::string(": ") + cfg.folder;
    return nullptr;
  }
  std::unique_ptr<Session> s(new Session(doc, std::move(cfg)));
  s->projectId_ = t.projectId;
  s->creator_ = t.creator;
  s->creatorSign_ = t.creatorSign;
  if (s->cfg_.name.empty()) s->cfg_.name = t.projectName;
  // the invite's name until the first sync brings the project's own (the file follows it then)
  s->cfg_.name = cleanProjectName(s->cfg_.name);
  if (s->cfg_.name.empty()) s->cfg_.name = "Project";
  s->fileName_ = renameProjectFileIn(s->cfg_.folder, std::string(), projectFileStem(s->cfg_.name));
  s->shownName_ = s->cfg_.name;
  Json cj = Json::makeObject();
  cj.object["format"] = Json::makeString("gridline-collab");
  cj.object["version"] = Json::makeNumber(1);
  cj.object["id"] = Json::makeString(t.projectId);
  cj.object["name"] = Json::makeString(s->cfg_.name);
  cj.object["file"] = Json::makeString(s->fileName_);
  cj.object["creator"] = Json::makeString(siteHex(t.creator));
  cj.object["creatorSign"] = Json::makeString(crypto::toHex(t.creatorSign.data(), 32));
  cj.object["minAppVersion"] = Json::makeString(s->cfg_.appVersion);
  if (!writeFileDurable((fs::path(s->cfg_.folder) / kDir / "collab.json").string(), jsonStringify(cj, 2), err)) return nullptr;
  Json snap;
  std::vector<std::string> recs;
  if (!s->log_.open((fs::path(s->cfg_.folder) / kDir).string(), snap, recs, err)) return nullptr;
  doc.reset(makeEmptySong());
  s->attach();
  return s;
}

bool Session::loadFromDisk(std::string* err) {
  Json snap;
  std::vector<std::string> recs;
  if (!log_.open((fs::path(cfg_.folder) / kDir).string(), snap, recs, err)) return false;
  if (snap.is(Json::Type::Object)) {
    if (const Json* r = snap.find("replica"); !r || !replica_.fromJson(*r)) {
      if (err) *err = i18n::tr("The collaboration snapshot is damaged.");
      return false;
    }
    vv_ = vvFromJson(snap.find("vv") ? *snap.find("vv") : Json());
    if (const Json* c = snap.find("conflicts")) conflicts_.fromJson(*c);
    hlc_.restore(siteFromHex(snap.text("hlc")));
    if (const Json* h = snap.find("honoured"); h && h->is(Json::Type::Object))
      for (const auto& [site, q] : h->object) honouredTop_[siteFromHex(site)] = static_cast<uint64_t>(q.number);
    if (const Json* k = snap.find("known"); k && k->is(Json::Type::Object))
      for (const auto& [site, v] : k->object) peers_[siteFromHex(site)].known = vvFromJson(v);
    for (auto& [site, p] : peers_) p.site = site;
  }
  mySeq_ = vv_.count(me_) ? vv_[me_] : 0;
  for (const auto& text : recs) {
    Batch b;
    if (!Batch::parse(text, b)) continue;
    batches_[b.site][b.seq] = text;
    if (b.site == me_) mySeq_ = std::max(mySeq_, b.seq);
    if (b.seq > (vv_.count(b.site) ? vv_[b.site] : 0)) pending_[b.site][b.seq] = std::move(b);
  }
  // records are re-applied without being written again
  replaying_ = true;
  refreshMembership();
  drainPending();
  replaying_ = false;
  refreshMembership();
  // a removal replayed after batches it cuts off: replay again with every removal known
  if (rebuildPending_) rebuild();
  {
    std::string t;
    Json c;
    if (readFileText((fs::path(cfg_.folder) / kDir / "contact.json").string(), t, 4096) && jsonParse(t, c))
      lastContact_ = static_cast<uint64_t>(c.num("last", 0));
  }
  floDirty_ = true;
  updateReady();
  // the plain project file may have been edited without collaboration since
  Json song = replica_.materialize({"song"});
  refsToLocal(song);
  repairProjectJson(song);
  Song s;
  std::string e;
  if (!song.is(Json::Type::Object) || !songFromJson(song, s, &e)) {
    if (err) *err = i18n::tr("The collaborative project could not be read") + std::string(": ") + e;
    return false;
  }
  floCache_ = floFromJson(replica_.materialize({"flo"}));
  floShadow_ = floCache_;
  floDirty_ = false;
  s.flOrigin = floCache_;
  const Member* m = member(me_);
  s.idSlot = m ? static_cast<uint32_t>(m->slot) : 0;
  s.idStride = kIdStride;
  doc_.reset(std::move(s));
  shadow_ = withoutOrigin(doc_.song());
  shadowValid_ = true;
  remoteDirty_ = false;
  attach();
  // External edits of <name>.gridline.json (an older Gridline, a text editor): they become local changes.
  std::string written, cur;
  readFileText((fs::path(cfg_.folder) / kDir / "written.txt").string(), written, 1 << 10);
  if (readFileText(projectFile(), cur) && !written.empty() && update::sha256Hex(cur) != written) {
    Song ext;
    if (loadProject(projectFile(), ext, &e)) {
      ext.idSlot = doc_.song().idSlot;
      ext.idStride = kIdStride;
      ext.flOrigin = doc_.song().flOrigin;
      doc_.patchSong([&](Song& x) { x = std::move(ext); });
      flushLocal();
      if (onNotice) onNotice(i18n::tr("Changes made to the project file outside a collaboration session were added."), false);
    }
  }
  return true;
}

// ---------------------------------------------------------------- attach ----
void Session::attach() {
  if (attached_) return;
  attached_ = true;
  doc_.historyHook = [this](int dir) { return historyStep(dir); };
  doc_.beforeChange = [this] { flushLocal(); };
  lastRev_ = doc_.revision();
  lastPush_ = doc_.pushCount();
  lastSerial_ = doc_.history().stateSerial();
}

void Session::detach() {
  if (!attached_) return;
  attached_ = false;
  doc_.historyHook = nullptr;
  doc_.beforeChange = nullptr;
  doc_.patchSong([](Song& s) {
    s.idSlot = 0;
    s.idStride = 1;
  });
}

// ---------------------------------------------------------- local edits ----
// Local changes since the last flush, element by element: only lists, elements
// and fields that differ from the shadow (the replica as a song) are turned
// into JSON and diffed. False with `bad` set: an external plugin was added.
bool Session::diffIncremental(std::vector<Op>& ops, std::string* bad) {
  const Song& cur = doc_.song();
  auto next = [this] { return hlc_.tick(); };
  if (!shadowValid_) {
    Json song = localSongJson();
    if (!policyAllows(song, bad)) return false;
    replica_.diff({"song"}, song, me_, next, ops);
    shadow_ = withoutOrigin(doc_.song());
    shadowValid_ = true;
    return true;
  }
  std::vector<std::pair<Path, Json>> work;  // diffs to run once every part passed the plugin check
  std::vector<std::function<void()>> commit;
  auto keyed = [&](const std::string& kind, const auto& oldList, const auto& nowList, auto& shadowList) {
    if (idsOf(oldList) == idsOf(nowList)) {
      for (size_t i = 0; i < nowList.size(); ++i) {
        if (oldList[i] == nowList[i]) continue;
        Json j = listJsonOf(cur, kind, static_cast<int>(i));
        if (!j.is(Json::Type::Array) || j.array.empty()) continue;
        Json e = std::move(j.array[0]);
        refsToReplica(e);
        maskElementJson(kind, e);
        work.push_back({{"song", kind, "#" + std::to_string(nowList[i].id)}, std::move(e)});
        commit.push_back([&shadowList, &nowList, i] { shadowList[i] = nowList[i]; });
      }
    } else {
      Json j = listJsonOf(cur, kind, -1);
      if (!j.is(Json::Type::Null)) {
        refsToReplica(j);
        for (auto& e : j.array) maskElementJson(kind, e);
      }
      work.push_back({{"song", kind}, std::move(j)});
      commit.push_back([&shadowList, &nowList] { shadowList = nowList; });
    }
  };
  keyed("channels", shadow_.channels, cur.channels, shadow_.channels);
  keyed("patterns", shadow_.patterns, cur.patterns, shadow_.patterns);
  keyed("playlist", shadow_.playlist, cur.playlist, shadow_.playlist);
  keyed("mixer", shadow_.mixer, cur.mixer, shadow_.mixer);
  keyed("automations", shadow_.automations, cur.automations, shadow_.automations);
  const bool restChanged = !restEqual(shadow_, cur);
  Json rest;
  if (restChanged) {
    rest = songToJson(restOf(cur), false);
    for (const char* l : kLists) rest.object.erase(l);
    maskSessionJson(rest);
  }
  for (const auto& [path, j] : work)
    if (hostedIn(j, bad)) return false;
  for (const auto& [path, j] : work) {
    if (j.is(Json::Type::Null)) {  // an optional list that became empty
      if (replica_.visible(path)) {
        Op op;
        op.kind = OpKind::Del;
        op.t = next();
        op.path = path;
        ops.push_back(std::move(op));
      }
      continue;
    }
    replica_.diff(path, j, me_, next, ops);
  }
  if (restChanged) {
    if (const Node* sn = replica_.find({"song"}))
      for (const auto& [k, n] : visibleKids(*sn, Stamp{}))
        if (!isList(*k) && !rest.find(*k)) {
          Op op;
          op.kind = OpKind::Del;
          op.t = next();
          op.path = {"song", *k};
          ops.push_back(std::move(op));
        }
    for (const auto& [k, v] : rest.object) replica_.diff({"song", k}, v, me_, next, ops);
    Song r = restOf(cur);
    shadow_.info = r.info;
    shadow_.ppq = r.ppq;
    shadow_.tempo = r.tempo;
    shadow_.timeSigNum = r.timeSigNum;
    shadow_.timeSigDen = r.timeSigDen;
    shadow_.swing = r.swing;
    shadow_.masterPitch = r.masterPitch;
    shadow_.nextId = r.nextId;
    shadow_.tracks = r.tracks;
    shadow_.links = r.links;
    shadow_.modLinks = r.modLinks;
    shadow_.markers = r.markers;
    shadow_.channelGroups = r.channelGroups;
    shadow_.patternGroups = r.patternGroups;
    shadow_.live = r.live;
    shadow_.arrangements = r.arrangements;
    shadow_.currentArrangement = r.currentArrangement;
    shadow_.autoCrossfades = r.autoCrossfades;
  }
  for (auto& c : commit) c();
  return true;
}

Json Session::localSongJson() {
  Json j = songToJson(doc_.song(), false);
  refsToReplica(j);
  maskSessionJson(j);  // solo, record arm, plugin listen / hold toggles ... stay on this computer
  return j;
}

void Session::refsToReplica(Json& song) {
  std::map<std::string, std::string> fix;  // doc path -> new doc path (imports)
  const std::string audioDir = (fs::path(cfg_.folder) / "Audio").lexically_normal().generic_string() + "/";
  forEachPluginJson(song, [&](Json& plug) {
    Json* data = plug.find("data") ? &plug.object["data"] : nullptr;
    if (!data || !data->is(Json::Type::String) || data->str.empty()) return;
    PluginState st;
    st.type = plug.text("type");
    st.data = data->str;
    if (forEachStateFileRef(st, [&](std::string& p) {
          if (p.empty() || !fs::path(p).is_absolute()) return;
          const std::string g = fs::path(p).lexically_normal().generic_string();
          if (g.rfind(audioDir, 0) == 0) {
            p = "Audio/" + g.substr(audioDir.size());
            return;
          }
          auto it = fix.find(p);
          if (it != fix.end()) {
            p = "Audio/" + fs::path(it->second).filename().string();
            return;
          }
          const std::string rel = importFile(p);  // a new file from outside: it joins the project
          if (rel.empty()) return;
          fix[p] = localPath(rel);
          p = rel;
        }))
      data->str = st.data;
  });
  if (!fix.empty())
    doc_.patchSong([&](Song& s) {
      forEachFileRef(s, [&](std::string& p) {
        auto it = fix.find(p);
        if (it != fix.end()) p = it->second;
      });
    });
}

void Session::refsToLocal(Json& song) const {
  forEachPluginJson(song, [&](Json& plug) {
    Json* data = plug.find("data") ? &plug.object["data"] : nullptr;
    if (!data || !data->is(Json::Type::String) || data->str.empty()) return;
    PluginState st;
    st.type = plug.text("type");
    st.data = data->str;
    if (forEachStateFileRef(st, [&](std::string& p) {
          if (p.rfind("Audio/", 0) == 0 && safeAudioName(p)) p = localPath(p);
        }))
      data->str = st.data;
  });
}

bool Session::policyAllows(const Json& song, std::string* offending) const {
  const auto bad = foreignPlugins(song);
  if (bad.empty()) return true;
  if (offending) *offending = bad.front().first + " (" + bad.front().second + ")";
  return false;
}

void Session::flushLocal() {
  if (!attached_ || stopped_) return;
  if (doc_.revision() == lastRev_) return;
  if (!ready_ || previewing_) {
    lastRev_ = doc_.revision();
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Op> ops;
  std::string bad;
  auto next = [this] { return hlc_.tick(); };
  if (!diffIncremental(ops, &bad)) {
    if (!bad.empty()) {
      if (onNotice)
        onNotice(i18n::tr("External plugins can't be used in a collaborative project") + std::string(": ") + bad, true);
      dirty_.full = true;
      remoteDirty_ = true;
      lastRev_ = doc_.revision();
      lastPush_ = doc_.pushCount();
      lastSerial_ = doc_.history().stateSerial();
      materialize();
      return;
    }
  }
  if (!(doc_.song().flOrigin == floShadow_)) {
    replica_.diff({"flo"}, floToJson(doc_.song().flOrigin), me_, next, ops);
    floShadow_ = doc_.song().flOrigin;
    floCache_ = floShadow_;
  }
  const uint64_t serial = doc_.history().stateSerial(), push = doc_.pushCount();
  const bool newStep = push != lastPush_;
  const bool merged = !newStep && serial != lastSerial_;
  if (merged) {
    auto it = steps_.find(lastSerial_);
    if (it != steps_.end() && lastSerial_ != serial) {
      steps_[serial] = std::move(it->second);
      steps_.erase(lastSerial_);
    }
  }
  if (newStep) steps_.erase(serial);
  lastPush_ = push;
  lastSerial_ = serial;
  lastRev_ = doc_.revision();
  lastFlush_ = now_;
  if (!ops.empty()) {
    if (gesture_)
      for (const auto& op : ops) gestureObjects_.insert(objectOf(op.path));
    makeLocalBatch(std::move(ops), true, newStep || merged);
  }
  lastFlushMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Session::makeLocalBatch(std::vector<Op> ops, bool undoable, bool record) {
  (void)undoable;
  if (ops.empty()) return true;
  Batch b;
  b.site = me_;
  b.seq = mySeq_ + 1;
  b.deps = vv_;
  b.deps[me_] = mySeq_;
  b.name = cleanName(cfg_.profile.name);
  std::vector<Json> before;
  if (record) {
    before.reserve(ops.size());
    for (const auto& op : ops)
      before.push_back(op.kind == OpKind::Del || op.kind == OpKind::Ins ? replica_.materialize(op.path) : Json());
  }
  b.ops = std::move(ops);
  b.signWith(cfg_.identity);
  if (!tryApply(b, true)) return false;
  if (record) recordStep(b.ops, before);
  broadcastBatch(batches_[me_][b.seq], 0);
  return true;
}

void Session::recordStep(const std::vector<Op>& ops, const std::vector<Json>& before) {
  StepRec& rec = steps_[lastSerial_];
  for (size_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    const int cls = op.kind == OpKind::Set ? 0 : op.kind == OpKind::Mov ? 1 : 2;
    const std::string key = joinPath(op.path) + "#" + std::to_string(cls);
    auto it = rec.index.find(key);
    if (it == rec.index.end()) {
      UndoGroup g;
      g.path = op.path;
      g.cls = cls;
      if (cls == 0) g.backVal = op.prev;
      if (cls == 1) g.backPos = op.prev.str;
      if (cls == 2) {
        const bool wasThere = i < before.size() && !before[i].is(Json::Type::Null);
        g.backPres = op.kind == OpKind::Del || (op.kind == OpKind::Ins && wasThere);
        if (wasThere) g.backContent = before[i];
      }
      it = rec.index.emplace(key, rec.groups.size()).first;
      rec.groups.push_back(std::move(g));
    }
    UndoGroup& g = rec.groups[it->second];
    if (cls == 0) g.fwdVal = op.value;
    if (cls == 1) g.fwdPos = op.pos;
    if (cls == 2) {
      g.fwdPres = op.kind != OpKind::Del;
      if (op.kind == OpKind::Ins) g.fwdVal = op.value;  // content to re-insert on redo
    }
    g.cur = Stamp{op.t, me_};
  }
  // bounded memory: the history keeps 100 steps; records of dropped steps go
  while (steps_.size() > 256) steps_.erase(steps_.begin());
}

bool Session::historyStep(int dir) {
  flushLocal();
  const uint64_t before = doc_.history().stateSerial();
  if (!doc_.stepHistory(dir)) return false;
  const uint64_t key = dir < 0 ? before : doc_.history().stateSerial();
  lastSerial_ = doc_.history().stateSerial();
  lastPush_ = doc_.pushCount();
  lastRev_ = doc_.revision();
  auto it = steps_.find(key);
  if (it == steps_.end() || previewing_) return true;
  StepRec& rec = it->second;
  std::vector<Op> ops;
  std::vector<size_t> owner;
  auto emit = [&](size_t gi, Op op) {
    op.t = hlc_.tick();
    ops.push_back(std::move(op));
    owner.push_back(gi);
  };
  const size_t n = rec.groups.size();
  for (size_t k = 0; k < n; ++k) {
    const size_t gi = dir < 0 ? n - 1 - k : k;
    UndoGroup& g = rec.groups[gi];
    const Node* node = replica_.find(g.path);
    Op op;
    op.path = g.path;
    if (g.cls == 0) {
      if (!node || node->valS != g.cur) continue;  // someone changed it since: theirs stays
      op.kind = OpKind::Set;
      op.value = dir < 0 ? g.backVal : g.fwdVal;
      op.prev = node->val;
      if (op.value.is(Json::Type::Object) || op.value.is(Json::Type::Array)) continue;
    } else if (g.cls == 1) {
      if (!node || node->posS != g.cur) continue;
      op.kind = OpKind::Mov;
      op.pos = dir < 0 ? g.backPos : g.fwdPos;
      op.prev = Json::makeString(node->pos);
      if (op.pos.empty()) continue;
    } else {
      if (node && node->presS != g.cur) continue;
      const bool target = dir < 0 ? g.backPres : g.fwdPres;
      if (target) {
        const Json& content = dir < 0 ? g.backContent : g.fwdVal;
        const bool replace = dir < 0 ? !g.backContent.is(Json::Type::Null) && g.fwdPres : !g.fwdVal.is(Json::Type::Null);
        if ((!node || replace) && !content.is(Json::Type::Null)) {
          op.kind = OpKind::Ins;
          op.value = content;
          if (node) op.pos = node->pos;
        } else if (node) {
          op.kind = OpKind::Undel;
        } else {
          continue;
        }
      } else {
        if (!node) continue;
        op.kind = OpKind::Del;
      }
    }
    emit(gi, std::move(op));
  }
  if (ops.empty()) return true;
  std::vector<Stamp> stamps;
  for (const auto& op : ops) stamps.push_back(Stamp{op.t, me_});
  if (!makeLocalBatch(std::move(ops), false, false)) return true;
  for (size_t i = 0; i < owner.size(); ++i) rec.groups[owner[i]].cur = stamps[i];
  remoteDirty_ = true;
  dirty_.full = true;
  materialize();
  return true;
}

// ---------------------------------------------------------------- remote ----
SiteId Session::admin() const {
  const Json a = replica_.materialize({"meta", "admin"});
  return a.is(Json::Type::String) ? siteFromHex(a.str) : creator_;
}

std::vector<Member> Session::members() const {
  std::vector<Member> out;
  const Json m = replica_.materialize({"meta", "members"});
  if (!m.is(Json::Type::Object)) return out;
  for (const auto& [k, v] : m.object) {
    Member x;
    if (memberFrom(k, v, x)) out.push_back(std::move(x));
  }
  std::sort(out.begin(), out.end(), [](const Member& a, const Member& b) { return a.joinedMs != b.joinedMs ? a.joinedMs < b.joinedMs : a.site < b.site; });
  return out;
}

const Member* Session::member(SiteId s) const {
  static thread_local Member cache;
  const Json v = replica_.materialize({"meta", "members", siteHex(s)});
  if (!v.is(Json::Type::Object) || !memberFrom(siteHex(s), v, cache)) return nullptr;
  return &cache;
}

std::string Session::projectName() const {
  const Json n = replica_.materialize({"meta", "project", "name"});
  if (n.is(Json::Type::String))
    if (std::string c = cleanProjectName(n.str); !c.empty()) return c;
  // projects without a shared name (none was ever written): collab.json / the invite, then the file
  if (!cfg_.name.empty()) return cfg_.name;
  if (hasProjectExt(fileName_)) return fileName_.substr(0, fileName_.size() - std::string(kProjectFileExt).size());
  return "Project";
}

std::string Session::minAppVersion() const {
  const Json v = replica_.materialize({"meta", "versions"});
  update::Version best;
  std::string bestText;
  if (v.is(Json::Type::Object))
    for (const auto& [k, x] : v.object) {
      const update::Version pv = update::parseVersion(x.str);
      if (pv.valid && (!best.valid || update::compare(pv, best) > 0)) {
        best = pv;
        bestText = update::toString(pv);
      }
    }
  return bestText;
}

PeerView* Session::peer(SiteId s) {
  auto it = peers_.find(s);
  return it == peers_.end() ? nullptr : &it->second;
}

size_t Session::pendingBatches() const {
  size_t n = 0;
  for (const auto& [s, m] : pending_) n += m.size();
  return n;
}

bool Session::authorized(const Batch& b, std::string* why) const {
  crypto::Key key{};
  bool have = false;
  if (const Member* m = member(b.site)) {
    key = m->signPublic;
    have = true;
  } else if (b.site == creator_ && b.seq == 1) {
    key = creatorSign_;
    have = true;
  }
  if (!have) {
    if (why) *why = "unknown";
    return false;
  }
  if (!b.verify(key)) {
    if (why) *why = "signature";
    return false;
  }
  return true;
}

bool Session::heldByGesture(const Batch& b) const {
  if (!gesture_ || gestureObjects_.empty()) return false;
  for (const auto& op : b.ops)
    if (gestureObjects_.count(objectOf(op.path))) return true;
  return false;
}

bool Session::tryApply(const Batch& b, bool local) {
  const uint64_t have = vv_.count(b.site) ? vv_[b.site] : 0;
  if (b.seq != have + 1) return false;
  if (!vvCovers(vv_, b.deps, b.site)) return false;
  if (!local) {
    std::string why;
    if (!authorized(b, &why)) return false;
  }
  const SiteId adminNow = admin();
  const bool genesis = b.site == creator_ && b.seq == 1;
  hlc_.observe(b.lastT());
  // J-111e: a removed member's batches after the cut count no more (the batch
  // still advances the version vector, so later batches of others that depend
  // on it apply, and it is relayed like any other: it simply changes nothing)
  const bool counts = genesis || honoured(b);
  if (!counts) logging::info("collab", "batch " + std::to_string(b.seq) + " of removed member " + siteHex(b.site) + " dropped");
  for (const auto& op : b.ops) {
    if (!counts) break;
    if (op.path.empty()) continue;
    const std::string& top = op.path[0];
    if (top == "meta" && !genesis) {
      if (!metaAllowed(b, op, adminNow)) continue;
    } else if (top != "meta" && top != "song" && top != "flo") {
      continue;
    }
    replica_.apply(op, b.site);
    if (top == "song") {
      remoteDirty_ = remoteDirty_ || !local;
      // what changed, for the incremental apply: elements of the five lists, a few scalars, else everything
      if (op.path.size() >= 3 && isList(op.path[1]) && !(op.path.size() == 3 && op.kind != OpKind::Set))
        dirty_.elems[op.path[1]].insert(op.path[2]);
      else if (op.path.size() == 2 && isScalar(op.path[1]) && op.kind == OpKind::Set)
        dirty_.scalars.insert(op.path[1]);
      else
        dirty_.full = true;
      if (!local) {
        const std::string obj = objectOf(op.path);
        if (!obj.empty()) {
          Path p = splitPath(obj);
          if (p.size() >= 3) flashes_.push_back(Flash{p[1] + "/" + p[2], b.site, now_});
        }
      }
    } else if (top == "flo") {
      floDirty_ = true;
      remoteDirty_ = remoteDirty_ || !local;
    } else {
      metaDirty_ = true;
    }
  }
  vv_[b.site] = b.seq;
  if (b.site == me_) mySeq_ = b.seq;
  std::string text = b.text();
  if (!replaying_) log_.append(text);
  batches_[b.site][b.seq] = std::move(text);
  opCount_ += b.ops.size();
  const size_t openBefore = conflicts_.openCount();
  if (counts) {
    honouredTop_[b.site] = std::max(honouredTop_[b.site], b.seq);
    conflicts_.onBatch(b, std::make_shared<const VersionVector>(b.deps));
  }
  if (metaDirty_) {
    refreshMembership();
    std::set<std::string> refs;
    const Json res = replica_.materialize({"meta", "resolutions"});
    if (res.is(Json::Type::Object))
      for (const auto& [k, r] : res.object)
        if (const Json* list = r.find("refs"); list && list->is(Json::Type::Array))
          for (const auto& x : list->array) refs.insert(x.str);
    conflicts_.setResolved(std::move(refs));
    metaDirty_ = false;
    updateReady();
  }
  if (!local && conflicts_.openCount() > openBefore && isAdmin()) reviewRequest_ = true;
  if (!local) {
    if (Hlc::msOf(b.lastT()) > wallNowMs() + 5000) peers_[b.site].clockApprox = true;
  }
  projectDirtyAt_ = projectDirtyAt_ < 0 ? now_ : projectDirtyAt_;
  return true;
}

void Session::updateReady() {
  if (ready_) return;
  if (replica_.visible({"song"}) && member(me_)) {
    ready_ = true;
    remoteDirty_ = true;
  }
}

void Session::drainPending() {
  if (pending_.empty()) return;
  // local changes are diffed against the replica as it was when they were
  // made: flush them before any remote op changes the replica
  if (!replaying_ && doc_.revision() != lastRev_) flushLocal();
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto& [site, m] : pending_) {
      for (;;) {
        const uint64_t want = (vv_.count(site) ? vv_[site] : 0) + 1;
        // drop what we already have
        while (!m.empty() && m.begin()->first < want) m.erase(m.begin());
        auto it = m.find(want);
        if (it == m.end()) break;
        if (heldByGesture(it->second)) break;
        std::string why;
        if (!vvCovers(vv_, it->second.deps, site)) break;
        if (!authorized(it->second, &why)) {
          if (why == "signature") {
            logging::warning("collab", "dropped a batch with a bad signature from " + siteHex(site));
            m.erase(it);
          }
          break;
        }
        Batch b = std::move(it->second);
        m.erase(it);
        if (!tryApply(b, false)) break;
        progress = true;
        for (auto& [ps, pv] : peers_)
          if (pv.syncTotal > pv.syncDone && pv.connected) ++pv.syncDone;
        if (!replaying_) broadcastBatch(batches_[b.site][b.seq], b.site);
      }
    }
  }
  for (auto it = pending_.begin(); it != pending_.end();) it = it->second.empty() ? pending_.erase(it) : std::next(it);
}

void Session::broadcastBatch(const std::string& text, SiteId except) {
  if (!sendFn) return;
  // only to connected peers that do not have it yet (relay / store-and-forward)
  std::vector<SiteId> to;
  for (auto& [s, p] : peers_) {
    if (!p.connected || s == except) continue;
    const MemberState st = memberState(s);
    if (st == MemberState::Removed || st == MemberState::Left) continue;
    to.push_back(s);
  }
  if (to.empty()) return;
  Message m;
  m.head.object["m"] = Json::makeString("batches");
  Json list = Json::makeArray();
  list.array.push_back(Json::makeString(text));
  m.head.object["b"] = std::move(list);
  const std::string wire = m.encode();
  Batch b;
  const bool parsed = Batch::parse(text, b);
  for (SiteId s : to) {
    sendFn(s, kStreamControl, wire);
    // the link is reliable: count it as seen (their next version vector confirms it)
    if (parsed) {
      uint64_t& k = peers_[s].seen[b.site];
      if (k + 1 == b.seq) k = b.seq;
    }
  }
}

void Session::handleBatches(SiteId from, const Json& list) {
  if (!list.is(Json::Type::Array)) return;
  for (const auto& t : list.array) {
    if (!t.is(Json::Type::String)) continue;
    Batch b;
    if (!Batch::parse(t.str, b)) continue;
    const uint64_t have = vv_.count(b.site) ? vv_[b.site] : 0;
    if (b.seq <= have) continue;
    if (pending_[b.site].size() > 100000) continue;
    auto& pv = peers_[from];
    if (b.seq > (pv.seen.count(b.site) ? pv.seen[b.site] : 0)) pv.seen[b.site] = b.seq;
    pending_[b.site].emplace(b.seq, std::move(b));
  }
  drainPending();
}

bool Session::buildFullSong(Song& s, std::string* err) {
  Json song = replica_.materialize({"song"});
  if (!song.is(Json::Type::Object)) return false;
  refsToLocal(song);
  repairProjectJson(song);
  if (!songFromJson(song, s, err)) return false;
  keepSessionState(s, doc_.song());  // this computer's solo, arm, listen toggles ... stay as they are
  if (floDirty_) {
    floCache_ = floFromJson(replica_.materialize({"flo"}));
    floDirty_ = false;
  }
  s.flOrigin = floCache_;
  const Member* m = member(me_);
  s.idSlot = m ? static_cast<uint32_t>(m->slot) : 0;
  s.idStride = kIdStride;
  s.nextId = std::max(s.nextId, doc_.song().nextId);  // keep ids we may already have handed out locally
  return true;
}

bool Session::applyIncremental() {
  if (dirty_.full || !shadowValid_ || floDirty_) return false;
  bool ok = true;
  Song& sh = shadow_;
  doc_.patchSong([&](Song& s) {
    for (const auto& k : dirty_.scalars) {
      const Json v = replica_.materialize({"song", k});
      if (!v.is(Json::Type::Number)) {
        ok = false;
        return;
      }
      if (k == "tempo") s.tempo = sh.tempo = std::clamp(v.number, 10.0, 522.0);
      if (k == "swing") s.swing = sh.swing = static_cast<float>(std::clamp(v.number, 0.0, 1.0));
      if (k == "masterPitch") s.masterPitch = sh.masterPitch = static_cast<float>(std::clamp(v.number, -12.0, 12.0));
    }
    for (const auto& [kind, ids] : dirty_.elems)
      for (const auto& id : ids) {
        Json j = replica_.materialize({"song", kind, id});
        if (!j.is(Json::Type::Object) || id.size() < 2 || id[0] != '#') {
          ok = false;
          return;
        }
        refsToLocal(j);
        const ObjectId oid = static_cast<ObjectId>(std::strtoul(id.c_str() + 1, nullptr, 10));
        auto place = [&](auto& list, auto& shadowList, auto&& build) {
          for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == oid) {
              auto e = list[i];
              e = {};
              e.id = oid;
              if (!build(e)) break;
              keepLocal(e, list[i]);
              list[i] = e;
              for (auto& x : shadowList)
                if (x.id == oid) x = e;
              return true;
            }
          return false;
        };
        bool done = false;
        if (kind == "channels") {
          done = place(s.channels, sh.channels, [&](Channel& c) {
            channelFromJson(j, c, s);
            std::vector<ObjectId> kids;  // the whole-project reader's layer / group checks
            for (ObjectId k : c.layer)
              if (k != c.id && s.channel(k) && std::find(kids.begin(), kids.end(), k) == kids.end()) kids.push_back(k);
            c.layer = std::move(kids);
            if (c.group >= static_cast<int>(s.channelGroups.size())) c.group = -1;
            return true;
          });
        } else if (kind == "patterns") {
          done = place(s.patterns, sh.patterns, [&](Pattern& p) { return patternFromJson(j, p, s); });
        } else if (kind == "playlist") {
          done = place(s.playlist, sh.playlist, [&](Clip& c) { return clipFromJson(j, c, s) == 0; });
        } else if (kind == "mixer") {
          done = place(s.mixer, sh.mixer, [&](MixerTrack& m) {
            mixerTrackFromJson(j, m);
            // sends: existing targets, no cycles (checked against the open song)
            std::vector<Send> keep;
            const int idx = s.mixerIndex(oid);
            if (idx < 0) return false;
            const std::vector<Send> saved = s.mixer[static_cast<size_t>(idx)].sends;
            s.mixer[static_cast<size_t>(idx)].sends.clear();
            for (const auto& sd : m.sends)
              if (sd.to != oid && s.mixerTrack(sd.to) && canRoute(s, oid, sd.to)) {
                keep.push_back(sd);
                s.mixer[static_cast<size_t>(idx)].sends.push_back(sd);
              }
            s.mixer[static_cast<size_t>(idx)].sends = saved;
            m.sends = std::move(keep);
            return true;
          });
        } else if (kind == "automations") {
          done = place(s.automations, sh.automations, [&](AutomationClip& a) {
            automationClipFromJson(j, a);
            return true;
          });
        }
        if (!done) {
          ok = false;
          return;
        }
      }
  });
  return ok;
}

void Session::materialize() {
  if (previewing_ || !ready_) return;
  if (doc_.revision() != lastRev_) flushLocal();
  const auto t0 = std::chrono::steady_clock::now();
  if (applyIncremental()) {
    ++incApplies_;
    if (verifyAt_ < 0) verifyAt_ = now_ + 1.0;  // a whole-project check once things are quiet
  } else {
    ++fullApplies_;
    Song s;
    std::string err;
    if (!buildFullSong(s, &err)) {
      logging::error("collab", "merged project does not load: " + err);
      remoteDirty_ = false;
      dirty_ = Dirty();
      return;
    }
    floShadow_ = s.flOrigin;
    shadow_ = withoutOrigin(s);
    shadowValid_ = true;
    doc_.replaceSong(std::move(s));
    verifyAt_ = -1;
  }
  dirty_ = Dirty();
  lastRev_ = doc_.revision();
  remoteDirty_ = false;
  lastApplyMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!flashes_.empty() && onRemoteChange) onRemoteChange(flashes_);
  flashes_.clear();
  lastFileScan_ = -1;  // look for new files now
}

void Session::verifyNow() {
  verifyAt_ = -1;
  if (!ready_ || previewing_) return;
  if (doc_.revision() != lastRev_) flushLocal();
  Song s;
  if (!buildFullSong(s)) return;
  Song cur = doc_.song();
  s.nextId = cur.nextId;
  if (s == cur) return;
  // the element-by-element apply differed from the whole project (references
  // changed meanwhile): the whole project wins, like on every other peer
  logging::info("collab", "incremental apply corrected by a whole-project check");
  ++corrections_;
  floShadow_ = s.flOrigin;
  shadow_ = withoutOrigin(s);
  doc_.replaceSong(std::move(s));
  lastRev_ = doc_.revision();
}

// ------------------------------------------------------------- messages ----
void Session::send(SiteId to, int stream, const Message& m) {
  if (sendFn) sendFn(to, stream, m.encode());
}

void Session::sendHello(SiteId to) {
  Message m;
  m.head.object["m"] = Json::makeString("hello");
  m.head.object["vv"] = vvToJson(vv_);
  m.head.object["ready"] = Json::makeNumber(ready_ ? 1 : 0);
  m.head.object["profile"] = profileWire(cfg_.profile);
  m.head.object["project"] = Json::makeString(projectId_);
  send(to, kStreamControl, m);
}

void Session::peerUp(SiteId peer) {
  if (stopped_) return;
  const MemberState st = memberState(peer);
  if (st == MemberState::Removed || st == MemberState::Left) return;  // never synced with again
  auto& p = peers_[peer];
  p.site = peer;
  p.connected = true;
  sendHello(peer);
}

void Session::peerDown(SiteId peer) {
  auto it = peers_.find(peer);
  if (it == peers_.end()) return;
  it->second.connected = false;
  it->second.presenceAt = -1;
  outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& o) { return o.to == peer; }), outgoing_.end());
  for (auto& [n, inc] : incoming_)
    if (inc.from == peer) inc.askedAt = -1;
}

void Session::setRtt(SiteId peer, double ms) { peers_[peer].rttMs = ms; }

int Session::admitMember(const Member& in) {
  if (openRemoval(in.site)) return -1;  // removed: only an admin's re-invite brings them back (readmit)
  if (const Member* have = member(in.site)) {
    const int slot = have->slot;
    if (left_.count(in.site)) {  // they left and are invited again: a member again
      Op op;
      op.kind = OpKind::Del;
      op.t = hlc_.tick();
      op.path = {"meta", "left:" + siteHex(in.site)};
      makeLocalBatch({op}, false, false);
    }
    return slot;
  }
  std::set<int> used;
  for (const auto& m : members()) used.insert(m.slot);
  int slot = 1;
  while (used.count(slot) && slot < static_cast<int>(kIdStride) - 1) ++slot;
  Member m = in;
  m.slot = slot;
  m.by = me_;
  m.joinedMs = wallNowMs();
  Op op;
  op.kind = OpKind::Ins;
  op.t = hlc_.tick();
  op.path = {"meta", "members", siteHex(m.site)};
  op.value = memberJson(m);
  makeLocalBatch({op}, false, false);
  return slot;
}

void Session::sendMissing(SiteId to, const VersionVector& theirs) {
  std::vector<std::pair<uint64_t, const std::string*>> list;
  bool gap = false;
  for (const auto& [site, m] : batches_) {
    const uint64_t has = theirs.count(site) ? theirs.at(site) : 0;
    const uint64_t mine = vv_.count(site) ? vv_.at(site) : 0;
    if (mine <= has) continue;
    if (m.empty() || m.begin()->first > has + 1) gap = true;  // compacted away: they need the snapshot
    for (auto it = m.upper_bound(has); it != m.end(); ++it) list.emplace_back(0, &it->second);
    // reliable link: counted as seen (their next version vector confirms it)
    if (!m.empty() && m.rbegin()->first > has && m.begin()->first <= has + 1) peers_[to].seen[site] = m.rbegin()->first;
  }
  for (const auto& [site, q] : vv_) {
    const uint64_t has = theirs.count(site) ? theirs.at(site) : 0;
    if (q > has && !batches_.count(site)) gap = true;
  }
  if (gap) {
    sendSnapshot(to);
    return;
  }
  if (list.empty()) return;
  Message begin;
  begin.head.object["m"] = Json::makeString("sync");
  begin.head.object["count"] = Json::makeNumber(static_cast<double>(list.size()));
  send(to, kStreamControl, begin);
  // in chunks of about 256 KB
  Message m;
  size_t bytes = 0;
  auto flush = [&] {
    if (!m.head.find("b")) return;
    send(to, kStreamBulk, m);
    m = Message();
    bytes = 0;
  };
  for (const auto& [k, text] : list) {
    if (!m.head.find("b")) {
      m.head.object["m"] = Json::makeString("batches");
      m.head.object["b"] = Json::makeArray();
    }
    m.head.object["b"].array.push_back(Json::makeString(*text));
    bytes += text->size();
    if (bytes > 256 * 1024) flush();
  }
  flush();
}

Json Session::snapshotJson() const {
  Json s = Json::makeObject();
  s.object["format"] = Json::makeString("gridline-collab-snapshot");
  s.object["project"] = Json::makeString(projectId_);
  s.object["replica"] = replica_.toJson();
  s.object["vv"] = vvToJson(vv_);
  s.object["conflicts"] = conflicts_.toJson();
  s.object["hlc"] = Json::makeString(siteHex(hlc_.last()));
  Json known = Json::makeObject();
  for (const auto& [site, p] : peers_) known.object[siteHex(site)] = vvToJson(p.known);
  s.object["known"] = std::move(known);
  Json hon = Json::makeObject();
  for (const auto& [site, q] : honouredTop_) hon.object[siteHex(site)] = Json::makeNumber(static_cast<double>(q));
  s.object["honoured"] = std::move(hon);
  return s;
}

void Session::sendSnapshot(SiteId to) {
  if (tainted_ || rebuildPending_) return;  // ours still holds edits a removal cut off: someone else sends it
  Message m;
  m.head.object["m"] = Json::makeString("snap");
  m.bin = jsonCompact(snapshotJson());
  send(to, kStreamBulk, m);
  peers_[to].seen = vv_;  // reliable link: everything we have is on its way
}

void Session::applySnapshot(const Message& m) {
  Json s;
  if (!jsonParse(m.bin, s) || s.text("project") != projectId_) return;
  const VersionVector theirs = vvFromJson(s.find("vv") ? *s.find("vv") : Json());
  // nothing new (a clean snapshot replaces ours while it holds edits a removal cut off)
  bool cleans = false;
  if (tainted_ && m.head.num("clean", 0) != 0) {
    cleans = true;
    const Json hon = s.find("honoured") ? *s.find("honoured") : Json();
    for (const auto& [site, upTo] : rebuildFor_)
      if (static_cast<uint64_t>(hon.num(siteHex(site), 0)) > upTo) cleans = false;
  }
  if (vvCovers(vv_, theirs) && !cleans) return;
  if (doc_.revision() != lastRev_) flushLocal();
  // our own batches that the snapshot does not include are re-applied on top
  std::vector<Batch> mine;
  for (const auto& [site, m2] : batches_)
    for (const auto& [seq, text] : m2) {
      const uint64_t has = theirs.count(site) ? theirs.at(site) : 0;
      if (seq > has) {
        Batch b;
        if (Batch::parse(text, b)) mine.push_back(std::move(b));
      }
    }
  if (!s.find("replica") || !replica_.fromJson(*s.find("replica"))) return;
  vv_ = theirs;
  honouredTop_.clear();
  if (const Json* h = s.find("honoured"); h && h->is(Json::Type::Object))
    for (const auto& [site, q] : h->object) honouredTop_[siteFromHex(site)] = static_cast<uint64_t>(q.number);
  if (cleans) {
    tainted_ = false;
    rebuildFor_.clear();
    ++rebuilds_;
  }
  refreshMembership();  // their removals decide what of ours still counts
  if (const Json* c = s.find("conflicts")) conflicts_.fromJson(*c);
  hlc_.observe(siteFromHex(s.text("hlc")));
  // the log restarts from this snapshot
  std::vector<std::string> keep;
  for (const auto& b : mine) keep.push_back(b.text());
  batches_.clear();
  log_.compact(snapshotJson(), {});
  for (auto& b : mine) pending_[b.site][b.seq] = std::move(b);
  mySeq_ = std::max(mySeq_, vv_.count(me_) ? vv_[me_] : 0);
  metaDirty_ = true;
  floDirty_ = true;
  remoteDirty_ = true;
  dirty_.full = true;
  {
    std::set<std::string> refs;
    const Json res = replica_.materialize({"meta", "resolutions"});
    if (res.is(Json::Type::Object))
      for (const auto& [k, r] : res.object)
        if (const Json* list = r.find("refs"); list && list->is(Json::Type::Array))
          for (const auto& x : list->array) refs.insert(x.str);
    conflicts_.setResolved(std::move(refs));
  }
  updateReady();
  drainPending();
  if (conflicts_.openCount() > 0 && isAdmin()) reviewRequest_ = true;
}

void Session::receive(SiteId from, int stream, const std::string& wire) {
  (void)stream;
  if (stopped_) return;
  const MemberState fromState = memberState(from);
  if (fromState == MemberState::Removed || fromState == MemberState::Left) return;  // J-111e: not synced with any more
  Message m;
  if (!Message::decode(wire, m)) return;
  const std::string type = m.type();
  auto& pv = peers_[from];
  pv.site = from;
  pv.lastSyncMs = wallNowMs();
  lastContact_ = pv.lastSyncMs;
  if (type == "hello") {
    if (m.head.text("project") != projectId_) return;
    pv.known = vvFromJson(m.head.find("vv") ? *m.head.find("vv") : Json());
    for (const auto& [k, q] : pv.known) pv.seen[k] = std::max(pv.seen[k], q);
    if (const Json* p = m.head.find("profile")) profileFromWire(*p, pv.profile);
    if (const Member* mem = member(from)) pv.fingerprint = mem->fingerprint();
    pv.avatarAskedAt = -1;
    askAvatar(from, pv);  // J-111g: their picture, by the hash the profile names
    sendMissing(from, pv.known);
    // they may have what we lack: ask by sending ours (hello is symmetric)
  } else if (type == "batches") {
    if (const Json* b = m.head.find("b")) handleBatches(from, *b);
  } else if (type == "sync") {
    pv.syncTotal = static_cast<size_t>(m.head.num("count", 0));
    pv.syncDone = 0;
  } else if (type == "snap") {
    applySnapshot(m);
  } else if (type == "snapreq") {
    // a collaborator's replica holds edits a removal cut off: ours is clean if we know those removals
    bool ok = !tainted_ && !rebuildPending_;
    if (const Json* need = m.head.find("need"); ok && need && need->is(Json::Type::Object))
      for (const auto& [site, upTo] : need->object) {
        bool have = false;
        auto it = windows_.find(siteFromHex(site));
        if (it != windows_.end())
          for (const auto& w : it->second) have = have || w.upTo == static_cast<uint64_t>(upTo.number);
        ok = ok && have;
      }
    if (ok) {
      Message r;
      r.head.object["m"] = Json::makeString("snap");
      r.head.object["clean"] = Json::makeNumber(1);
      r.bin = jsonCompact(snapshotJson());
      send(from, kStreamBulk, r);
    }
  } else if (type == "vv") {
    pv.known = vvFromJson(m.head.find("vv") ? *m.head.find("vv") : Json());
    for (const auto& [k, q] : pv.known) pv.seen[k] = std::max(pv.seen[k], q);
  } else if (type == "pres") {
    Presence p;
    p.window = m.head.text("w");
    p.x = static_cast<float>(m.head.num("x", 0));
    p.y = static_cast<float>(m.head.num("y", 0));
    auto u32 = [&](const char* k) {
      const double v = m.head.num(k, 0);
      return v >= 0 && v < 4294967296.0 ? static_cast<uint32_t>(v) : 0u;
    };
    p.kind = static_cast<int>(u32("k") & 0xFF);
    p.a = u32("a");
    p.b = u32("b");
    p.cx = static_cast<float>(m.head.num("cx", 0));
    p.cy = static_cast<float>(m.head.num("cy", 0));
    p.plugin = u32("po");
    if (p.plugin) {
      const double ps = m.head.num("ps", -1);
      p.pluginSlot = ps >= 0 && ps < 64 ? static_cast<int>(ps) : -1;
    }
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.cx) || !std::isfinite(p.cy)) p = Presence();
    p.playing = m.head.num("pl", 0) != 0;
    p.songTick = m.head.num("st", 0);
    pv.presence = p;
    pv.presenceAt = now_;
  } else if (type == "prof") {
    if (const Json* p = m.head.find("profile")) {
      Profile np;
      if (profileFromWire(*p, np)) {
        pv.profile = np;
        if (!np.avatar.empty()) avatars_[np.avatarHash()] = np.avatar;
        pv.avatarAskedAt = -1;
        askAvatar(from, pv);  // J-111g: a new picture: fetch it now
      }
    }
  } else if (type == "avreq") {
    if (m.head.text("h") == cfg_.profile.avatarHash() && !cfg_.profile.avatar.empty()) {
      Message r;
      r.head.object["m"] = Json::makeString("av");
      r.head.object["h"] = Json::makeString(cfg_.profile.avatarHash());
      r.bin = cfg_.profile.avatar;
      send(from, kStreamBulk, r);
    }
  } else if (type == "av") {
    if (m.bin.size() <= kMaxAvatarBytes) {
      Profile tmp;
      tmp.avatar = m.bin;
      if (tmp.avatarHash() == m.head.text("h")) {
        avatars_[tmp.avatarHash()] = m.bin;
        if (pv.profile.avatar.empty() && pv.profile.avatarRef == tmp.avatarHash()) pv.profile.avatar = m.bin;  // J-111g
      }
    }
  } else if (type == "fwant") {
    onFileWant(from, m);
  } else if (type == "fdata") {
    onFileData(from, m);
  } else if (type == "fnone") {
    auto it = incoming_.find(m.head.text("f"));
    if (it != incoming_.end() && it->second.from == from) it->second.askedAt = now_ - 9.0;  // ask someone else soon
  } else if (type == "notice") {
    if (const Json* n = m.head.find("n")) learnNotice(*n);
  } else if (type == "bye") {
    pv.connected = false;
    if (m.head.num("stop", 0) != 0 && from == creator_ && onNotice)
      onNotice(i18n::tr("The project owner stopped sharing this project. Your copy stays on this computer."), false);
  }
}

void Session::setProfile(const Profile& p) {
  cfg_.profile = p;
  Message m;
  m.head.object["m"] = Json::makeString("prof");
  m.head.object["profile"] = profileWire(p);
  for (auto& [s, pv] : peers_)
    if (pv.connected) send(s, kStreamControl, m);
}

// J-111g: profile messages name the picture by its hash; the bytes (up to 64 KB,
// more than one datagram) come on the reliable bulk stream when asked for. Until
// J-111g nobody asked, so collaborators' pictures never arrived. Asked when a
// profile names a picture not here, again every few seconds until it arrives.
void Session::askAvatar(SiteId to, PeerView& pv) {
  if (!pv.profile.avatar.empty() || pv.profile.avatarRef.empty()) return;  // here already (or no picture)
  const std::string& h = pv.profile.avatarRef;
  if (h.empty() || !pv.connected) return;
  if (const auto it = avatars_.find(h); it != avatars_.end()) {
    if (pv.profile.avatar.empty()) pv.profile.avatar = it->second;
    return;
  }
  if (pv.avatarAskedAt >= 0 && now_ - pv.avatarAskedAt < kAvatarRetrySec) return;
  pv.avatarAskedAt = now_;
  Message r;
  r.head.object["m"] = Json::makeString("avreq");
  r.head.object["h"] = Json::makeString(h);
  send(to, kStreamControl, r);
}

void Session::addAvatar(const std::string& bytes) {
  if (bytes.empty() || bytes.size() > kMaxAvatarBytes) return;
  Profile tmp;
  tmp.avatar = bytes;
  avatars_[tmp.avatarHash()] = bytes;
}

std::string Session::avatarFor(const std::string& hash) const {
  if (hash.empty()) return {};
  if (hash == cfg_.profile.avatarHash()) return cfg_.profile.avatar;
  auto it = avatars_.find(hash);
  return it == avatars_.end() ? std::string() : it->second;
}

void Session::sendPresence(double now) {
  if (now - lastPresence_ < kPresenceSec) return;
  if (localPresence_ == sentPresence_) return;
  lastPresence_ = now;
  sentPresence_ = localPresence_;
  Message m;
  m.head.object["m"] = Json::makeString("pres");
  m.head.object["w"] = Json::makeString(localPresence_.window);
  m.head.object["x"] = Json::makeNumber(localPresence_.x);
  m.head.object["y"] = Json::makeNumber(localPresence_.y);
  if (localPresence_.kind) {  // J-111d: the anchor (older peers read x, y only)
    m.head.object["k"] = Json::makeNumber(localPresence_.kind);
    m.head.object["a"] = Json::makeNumber(localPresence_.a);
    m.head.object["b"] = Json::makeNumber(localPresence_.b);
    m.head.object["cx"] = Json::makeNumber(localPresence_.cx);
    m.head.object["cy"] = Json::makeNumber(localPresence_.cy);
  }
  if (localPresence_.plugin) {  // J-111g: in a plugin editor (older peers ignore it)
    m.head.object["po"] = Json::makeNumber(localPresence_.plugin);
    m.head.object["ps"] = Json::makeNumber(localPresence_.pluginSlot);
  }
  if (localPresence_.playing) m.head.object["pl"] = Json::makeNumber(1);
  m.head.object["st"] = Json::makeNumber(localPresence_.songTick);
  for (auto& [s, pv] : peers_)
    if (pv.connected) send(s, kStreamUnreliable, m);
}

// ---------------------------------------------------------------- frame ----
void Session::frame(double now) {
  now_ = now;
  if (exitPending_) finishExit();
  if (stopped_) return;
  const bool gestureEnded = wasGesture_ && !gesture_;
  if (doc_.revision() != lastRev_) {
    const bool due = doc_.pushCount() != lastPush_ || lastFlush_ < 0 || now - lastFlush_ >= kGestureFlushSec || gestureEnded ||
                     !gesture_;
    if (due) flushLocal();
  }
  if (gestureEnded) gestureObjects_.clear();
  wasGesture_ = gesture_;
  drainPending();
  if (exitPending_) {
    finishExit();
    return;
  }
  if (rebuildPending_) rebuild();
  if (tainted_ && (snapAskedAt_ < 0 || now - snapAskedAt_ >= 5.0)) requestCleanSnapshot();
  if (remoteDirty_) materialize();
  if (lastContact_ && (contactSavedAt_ < 0 || now - contactSavedAt_ >= 60.0)) {
    contactSavedAt_ = now;
    Json c = Json::makeObject();
    c.object["last"] = Json::makeNumber(static_cast<double>(lastContact_));
    writeFileDurable((fs::path(cfg_.folder) / kDir / "contact.json").string(), jsonCompact(c));
  }
  checkName();  // a rename (live, caught up, from a snapshot, an undo, a review choice)
  if (verifyAt_ >= 0 && now >= verifyAt_ && !gesture_ && pending_.empty()) verifyNow();
  // files
  if (lastFileScan_ < 0 || now - lastFileScan_ >= 1.0) {
    lastFileScan_ = now;
    scanFiles();
  }
  requestFiles();
  pumpFileSends();
  sendPresence(now);
  for (auto& [site, pv] : peers_) askAvatar(site, pv);  // J-111g: pictures still missing
  // version vectors: stability (compaction, conflict index) and "unseen" counts
  if (now - lastVvSend_ >= 5.0) {
    lastVvSend_ = now;
    Message m;
    m.head.object["m"] = Json::makeString("vv");
    m.head.object["vv"] = vvToJson(vv_);
    for (auto& [s, pv] : peers_)
      if (pv.connected) send(s, kStreamControl, m);
    conflicts_.prune([this](SiteId site, uint64_t seq) { return isStable(Batch{site, seq, {}, {}, {}, {}}); });
  }
  for (auto& [s, pv] : peers_) {
    size_t unseen = 0;
    if (const auto it = batches_.find(me_); it != batches_.end()) {
      const uint64_t has = pv.seen.count(me_) ? pv.seen[me_] : 0;
      unseen = static_cast<size_t>(std::distance(it->second.upper_bound(has), it->second.end()));
    }
    pv.unseen = unseen;
    // caught up: counted, or (batches that came live before the count) we hold all they reported
    bool haveAll = pending_.empty();
    for (const auto& [site, q] : pv.known)
      if (haveAll && (vv_.count(site) ? vv_.at(site) : 0) < q) haveAll = false;
    if (pv.connected && pv.syncTotal && (pv.syncDone >= pv.syncTotal || haveAll)) pv.syncTotal = pv.syncDone = 0;
  }
  if (cfg_.durable && now - lastLogSync_ >= kLogSyncSec) {
    lastLogSync_ = now;
    log_.sync();
  }
  if (projectDirtyAt_ >= 0 && now - projectDirtyAt_ >= kProjectWriteSec) writeProjectFile();
  maybeCompact();
}

bool Session::isStable(const Batch& b) const {
  // removed and departed members are not waited for (their queues are purged)
  for (const auto& m : activeMembers()) {
    if (m.site == me_) continue;
    auto it = peers_.find(m.site);
    if (it == peers_.end()) return false;
    const VersionVector& k = it->second.known;
    const uint64_t seen = k.count(b.site) ? k.at(b.site) : 0;
    if (seen < b.seq) return false;
    const uint64_t theirOwn = k.count(m.site) ? k.at(m.site) : 0;
    const uint64_t mine = vv_.count(m.site) ? vv_.at(m.site) : 0;
    if (mine < theirOwn) return false;
  }
  return true;
}

void Session::maybeCompact() {
  static thread_local double last = -1;
  if (last >= 0 && now_ - last < 10.0 && now_ >= last) return;
  last = now_;
  if (log_.bytes() > kCompactBytes || log_.records() > kCompactRecords) compactNow();
}

bool Session::compactNow(std::string* err) {
  std::vector<std::string> keep;
  for (auto& [site, m] : batches_)
    for (auto it = m.begin(); it != m.end();) {
      if (isStable(Batch{site, it->first, {}, {}, {}, {}})) {
        it = m.erase(it);
      } else {
        keep.push_back(it->second);
        ++it;
      }
    }
  // tombstones: only when everyone is fully in sync, and older than a day
  bool allSynced = true;
  for (const auto& mem : activeMembers())
    if (mem.site != me_) {
      auto it = peers_.find(mem.site);
      if (it == peers_.end() || !vvCovers(it->second.known, vv_)) allSynced = false;
    }
  if (allSynced) replica_.collectTombstones(Stamp{Hlc::fromMs(wallNowMs() - 24ull * 3600 * 1000), 0});
  return log_.compact(snapshotJson(), keep, err);
}

void Session::writeProjectFile() {
  projectDirtyAt_ = -1;
  if (!ready_) return;
  Song copy = doc_.song();
  copy.idSlot = 0;
  copy.idStride = 1;
  const std::string base = fs::path(cfg_.folder).lexically_normal().generic_string() + "/";
  forEachFileRef(copy, [&](std::string& p) {
    const std::string g = fs::path(p).lexically_normal().generic_string();
    if (g.rfind(base, 0) == 0) p = g.substr(base.size());
  });
  const std::string text = projectToText(copy);
  syncFileName();  // first the file gets its name, then the content
  std::string err;
  if (writeFileAtomic(projectFile(), text, &err))
    writeFileDurable((fs::path(cfg_.folder) / kDir / "written.txt").string(), update::sha256Hex(text));
  writeCollabJson();
}

void Session::writeCollabJson() {
  // collab.json keeps the highest version that ever wrote here, the name (for lists of projects
  // that are not open) and which file in the folder is the project
  Json cj;
  if (!jsonParse(readCollabFileText(cfg_.folder), cj) || !cj.is(Json::Type::Object)) return;
  std::string minV = minAppVersion();
  const update::Version cur = update::parseVersion(cfg_.appVersion), have = update::parseVersion(cj.text("minAppVersion"));
  if (cur.valid && (!have.valid || update::compare(cur, have) > 0)) minV = update::toString(cur);
  else if (have.valid) minV = update::toString(have);
  const std::string name = ready_ ? projectName() : cj.text("name");
  if (cj.text("minAppVersion") != minV || cj.text("name") != name || cj.text("file") != fileName_) {
    cj.object["minAppVersion"] = Json::makeString(minV);
    cj.object["name"] = Json::makeString(name);
    cj.object["file"] = Json::makeString(fileName_);
    writeFileDurable((fs::path(cfg_.folder) / kDir / "collab.json").string(), jsonStringify(cj, 2));
  }
}

void Session::syncFileName() {
  if (!ready_ || stopped_) return;
  if (fileRetryAt_ >= 0 && now_ < fileRetryAt_) return;
  std::string err;
  const std::string got = renameProjectFileIn(cfg_.folder, fileName_, projectFileStem(projectName()), &err);
  if (!err.empty()) {
    if (fileRetryAt_ < 0) logging::warning("collab", "cannot rename the project file " + fileName_ + ": " + err);
    fileRetryAt_ = now_ + 30;  // tried again later (it may be open in another program)
  } else {
    fileRetryAt_ = -1;
  }
  if (got == fileName_) return;
  logging::info("collab", "project file renamed: " + fileName_ + " -> " + got);
  fileName_ = got;
  writeCollabJson();
}

void Session::checkName() {
  if (replaying_) return;
  const std::string n = projectName();
  if (n == shownName_ && nameReady_ == ready_) return;
  const std::string before = shownName_;
  // a join's first sync brings the name the project already had: not a rename
  const bool announce = nameReady_ && ready_ && n != before;
  shownName_ = n;
  nameReady_ = ready_;
  if (!ready_ || stopped_) return;
  syncFileName();
  writeCollabJson();
  if (announce && onRenamed) {
    const Node* node = replica_.find({"meta", "project", "name"});
    onRenamed(before, n, node ? node->valS.site : 0);
  }
}

bool Session::rename(const std::string& raw, std::string* err) {
  if (stopped_) {
    if (err) *err = i18n::tr("This project no longer syncs, so it can't be renamed for everyone.");
    return false;
  }
  if (!ready_) {
    if (err) *err = i18n::tr("The project hasn't arrived yet.");
    return false;
  }
  const std::string name = cleanProjectName(raw);
  if (name.empty()) {
    if (err) *err = i18n::tr("A project needs a name.");
    return false;
  }
  const Json cur = replica_.materialize({"meta", "project", "name"});
  if (cur.is(Json::Type::String) && cur.str == name) return true;  // the same name: nothing to send
  if (previewing_) endPreview();
  flushLocal();  // what was changed before is its own step
  // a history step of its own: undo / redo write the name back (another last-writer-wins write,
  // which leaves a newer rename by someone else alone, like every other undo)
  doc_.recordMark("Rename project");
  flushLocal();
  Op op;
  op.kind = OpKind::Set;
  op.t = hlc_.tick();
  op.path = {"meta", "project", "name"};
  op.value = Json::makeString(name);
  op.prev = cur.is(Json::Type::String) ? cur : Json();
  if (!makeLocalBatch({op}, true, true)) {
    if (err) *err = "cannot write the collaboration log";
    return false;
  }
  checkName();
  return true;
}

void Session::saveNow() {
  if (stopped_) return;
  flushLocal();
  log_.sync();
  writeProjectFile();
}

void Session::leave(bool stopSharing) {
  if (stopped_) return;
  saveNow();
  Message m;
  m.head.object["m"] = Json::makeString("bye");
  m.head.object["stop"] = Json::makeNumber(stopSharing ? 1 : 0);
  for (auto& [s, pv] : peers_)
    if (pv.connected) send(s, kStreamControl, m);
  Json cj;
  if (jsonParse(readCollabFileText(cfg_.folder), cj)) {
    cj.object["left"] = Json::makeNumber(1);
    writeFileDurable((fs::path(cfg_.folder) / kDir / "collab.json").string(), jsonStringify(cj, 2));
  }
  detach();
  stopped_ = true;
}

// ------------------------------------------------------------- conflicts ----
bool Session::takeReviewRequest() {
  const bool r = reviewRequest_;
  reviewRequest_ = false;
  return r;
}

bool Session::resolve(const std::string& object, SiteId keepSite, std::string* err) {
  if (!isAdmin()) {
    if (err) *err = i18n::tr("Only the project admin can resolve conflicts.");
    return false;
  }
  if (previewing_) endPreview();
  flushLocal();
  auto it = conflicts_.all().find(object);
  if (it == conflicts_.all().end()) {
    if (err) *err = "no such conflict";
    return false;
  }
  std::vector<Op> ops = conflicts_.keepOps(object, keepSite, replica_, [this] { return hlc_.tick(); });
  bool anyOpen = false;
  for (const auto& o : it->second.ops)
    if (!conflicts_.isResolved(o)) anyOpen = true;
  Json refs = Json::makeArray();
  for (const auto& o : it->second.ops)
    if (!anyOpen || !conflicts_.isResolved(o)) refs.array.push_back(Json::makeString(o.ref()));
  Op rec;
  rec.kind = OpKind::Ins;
  rec.t = hlc_.tick();
  rec.path = {"meta", "resolutions", siteHex(me_) + "-" + siteHex(rec.t)};
  Json r = Json::makeObject();
  r.object["o"] = Json::makeString(object);
  r.object["keep"] = Json::makeString(siteHex(keepSite));
  r.object["refs"] = std::move(refs);
  r.object["at"] = Json::makeNumber(static_cast<double>(wallNowMs()));
  rec.value = std::move(r);
  ops.push_back(std::move(rec));
  if (!makeLocalBatch(std::move(ops), false, false)) return false;
  remoteDirty_ = true;
  dirty_.full = true;
  materialize();
  return true;
}

bool Session::preview(const std::string& object, SiteId keepSite) {
  if (conflicts_.all().find(object) == conflicts_.all().end()) return false;
  flushLocal();
  Replica tmp;
  if (!tmp.fromJson(replica_.toJson())) return false;
  uint64_t t = hlc_.last();
  const auto ops = conflicts_.keepOps(object, keepSite, replica_, [&t] { return ++t; });
  for (const auto& op : ops) tmp.apply(op, me_);
  Json song = tmp.materialize({"song"});
  refsToLocal(song);
  repairProjectJson(song);
  Song s;
  if (!songFromJson(song, s)) return false;
  keepSessionState(s, doc_.song());
  s.flOrigin = floCache_;
  s.idSlot = doc_.song().idSlot;
  s.idStride = kIdStride;
  previewing_ = true;
  doc_.replaceSong(std::move(s));
  lastRev_ = doc_.revision();
  return true;
}

void Session::endPreview() {
  if (!previewing_) return;
  previewing_ = false;
  remoteDirty_ = true;
  dirty_.full = true;
  lastRev_ = doc_.revision();
  materialize();
}

bool Session::transferAdmin(SiteId to) {
  if (!isAdmin() || !isActive(to) || stopped_) return false;
  Op op;
  op.kind = OpKind::Set;
  op.t = hlc_.tick();
  op.path = {"meta", "admin"};
  op.value = Json::makeString(siteHex(to));
  op.prev = Json::makeString(siteHex(me_));
  return makeLocalBatch({op}, false, false);
}

// ------------------------------------------------------------ membership ----
// J-111e. Records live as flat keys under meta (each written once, by its one
// allowed author, so concurrent writers never clear each other's records):
//   meta/"removal:<site>:<id>"  admin: {by, byName, name, reason, at, upTo}
//   meta/"readd:<site>:<id>"    admin: {by, seq, at} (the batch that re-added them)
//   meta/"left:<site>"          the member themselves: {at, name}; deleted by whoever invites them back
//   meta/"ended"                admin: {by, byName, at} (stop sharing)
// Only the admin's signature is honoured for removals: the admin role is a chain
// of signed transfers, each made by the admin of its moment, and batches apply in
// causal order, so "the author is the admin" means the same on every peer.

void audioRefsIn(const std::string& t, std::set<std::string>& out) {
  size_t at = 0;
  while ((at = t.find("Audio/", at)) != std::string::npos) {
    at += 6;
    if (at + 64 > t.size()) break;
    bool hex = true;
    for (size_t i = 0; i < 64 && hex; ++i) {
      const char c = t[at + i];
      hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }
    if (!hex) continue;
    size_t end = at + 64;
    if (end < t.size() && t[end] == '.') {
      size_t e = end + 1;
      while (e < t.size() && e - end <= 12 && std::isalnum(static_cast<unsigned char>(t[e]))) ++e;
      end = e;
    }
    out.insert("Audio/" + t.substr(at, end - at));
    at = end;
  }
}

bool Session::honoured(const Batch& b) const {
  auto it = windows_.find(b.site);
  if (it == windows_.end()) return true;
  for (const Window& w : it->second) {
    if (b.seq <= w.upTo) continue;
    // after a re-add, their batches made after seeing it count again
    if (w.readd) {
      auto d = b.deps.find(w.readdBy);
      if (d != b.deps.end() && d->second >= w.readdSeq) continue;
    }
    return false;
  }
  return true;
}

bool Session::metaAllowed(const Batch& b, const Op& op, SiteId adminNow) const {
  if (op.path.size() < 2) return false;
  const std::string& sub = op.path[1];
  if (sub == "admin") {
    if (b.site != adminNow) return false;
    // never to someone removed (the admin's own earlier ops decide that: deterministic)
    return !(op.kind == OpKind::Set && op.value.is(Json::Type::String) && openRemoval(siteFromHex(op.value.str)));
  }
  if (sub == "resolutions") return b.site == adminNow;
  if (sub == "project") {
    // J-111f: the name: any member, a valid name (or none: back to the file name); the rest of it: the admin
    if (op.path.size() == 3 && op.path[2] == "name")
      return op.kind == OpKind::Set && (op.value.is(Json::Type::Null) || (op.value.is(Json::Type::String) && validProjectName(op.value.str)));
    return b.site == adminNow;
  }
  if (sub == "versions") return op.path.size() >= 3 && siteFromHex(op.path[2]) == b.site;
  if (sub == "members") {
    if (op.path.size() < 3) return false;
    if (op.path.size() == 3 && op.kind == OpKind::Del) return false;  // removal is a record, never an erase
    // a removed member's record is the admin's to touch (no re-admission through someone else's invite)
    return !(openRemoval(siteFromHex(op.path[2])) && b.site != adminNow);
  }
  if (sub.rfind("removal:", 0) == 0) {
    const size_t c = sub.find(':', 8);
    return op.path.size() == 2 && op.kind == OpKind::Ins && b.site == adminNow && c != std::string::npos &&
           siteFromHex(sub.substr(8, c - 8)) != b.site;
  }
  if (sub.rfind("readd:", 0) == 0)
    return op.path.size() == 2 && op.kind == OpKind::Ins && b.site == adminNow && siteFromHex(op.value.text("by")) == b.site &&
           static_cast<uint64_t>(op.value.num("seq", 0)) == b.seq;
  if (sub.rfind("left:", 0) == 0) {
    if (op.path.size() != 2) return false;
    if (op.kind == OpKind::Ins) return siteFromHex(sub.substr(5)) == b.site;  // only they leave
    return op.kind == OpKind::Del;                                          // invited back by a member
  }
  if (sub == "ended") return op.path.size() == 2 && op.kind == OpKind::Ins && b.site == adminNow;
  return true;
}

MemberState Session::memberState(SiteId s) const {
  if (!member(s)) return MemberState::Unknown;
  if (openRemoval(s)) return MemberState::Removed;
  if (left_.count(s)) return MemberState::Left;
  return MemberState::Active;
}

std::vector<Member> Session::activeMembers() const {
  std::vector<Member> out;
  for (auto& m : members())
    if (!openRemoval(m.site) && !left_.count(m.site)) out.push_back(m);
  return out;
}

std::vector<Removal> Session::removals() const {
  std::vector<Removal> out;
  for (const auto& [site, list] : removals_) out.insert(out.end(), list.begin(), list.end());
  std::sort(out.begin(), out.end(), [](const Removal& a, const Removal& b) { return a.at != b.at ? a.at < b.at : a.id < b.id; });
  return out;
}

const Removal* Session::openRemoval(SiteId s) const {
  auto it = removals_.find(s);
  if (it == removals_.end()) return nullptr;
  for (auto r = it->second.rbegin(); r != it->second.rend(); ++r)
    if (!r->readded) return &*r;
  return nullptr;
}

void Session::refreshMembership() {
  const Json meta = replica_.materialize({"meta"});
  std::map<SiteId, std::vector<Removal>> rem;
  std::map<std::string, const Json*> readds;
  std::set<SiteId> left;
  bool ended = false;
  EndInfo end;
  if (meta.is(Json::Type::Object))
    for (const auto& [k, v] : meta.object) {
      if (k.rfind("removal:", 0) == 0) {
        const size_t c = k.find(':', 8);
        if (c == std::string::npos) continue;
        Removal r;
        r.site = siteFromHex(k.substr(8, c - 8));
        r.id = k.substr(c + 1);
        r.by = siteFromHex(v.text("by"));
        r.byName = v.text("byName");
        r.name = v.text("name");
        r.reason = v.text("reason");
        r.at = static_cast<uint64_t>(v.num("at", 0));
        r.upTo = static_cast<uint64_t>(v.num("upTo", 0));
        rem[r.site].push_back(r);
      } else if (k.rfind("readd:", 0) == 0) {
        readds[k.substr(6)] = &v;
      } else if (k.rfind("left:", 0) == 0) {
        left.insert(siteFromHex(k.substr(5)));
      } else if (k == "ended" && v.is(Json::Type::Object)) {
        ended = true;
        end.by = siteFromHex(v.text("by"));
        end.byName = v.text("byName");
        end.at = static_cast<uint64_t>(v.num("at", 0));
      }
    }
  for (auto& [site, list] : rem) {
    for (auto& r : list) {
      auto it = readds.find(siteHex(site) + ":" + r.id);
      if (it == readds.end()) continue;
      r.readded = true;
      r.readdBy = siteFromHex(it->second->text("by"));
      r.readdSeq = static_cast<uint64_t>(it->second->num("seq", 0));
      r.readdAt = static_cast<uint64_t>(it->second->num("at", 0));
    }
    std::sort(list.begin(), list.end(), [](const Removal& a, const Removal& b) { return a.at != b.at ? a.at < b.at : a.id < b.id; });
  }
  removals_ = std::move(rem);
  left_ = std::move(left);
  if (!windowsFrozen_) {
    windows_.clear();
    for (const auto& [site, list] : removals_)
      for (const auto& r : list) windows_[site].push_back(Window{r.upTo, r.readded, r.readdBy, r.readdSeq});
  }
  for (const auto& [site, list] : removals_)
    for (const auto& r : list) {
      if (seenRemovals_.insert(r.id).second && !r.readded) {
        if (site == me_) {
          if (!cfg_.expectReadmit && exit_ == ExitKind::None) {
            myRemoval_ = r;
            exit_ = ExitKind::Removed;
            exitPending_ = true;
          }
        } else {
          // what we applied of theirs after the cut goes again (replayed without it)
          if (!windowsFrozen_ && honouredTop_.count(site) && honouredTop_[site] > r.upTo) {
            rebuildFor_[site] = r.upTo;
            rebuildPending_ = true;
          }
          // never synced with again: no link, no files to or from them. If they are
          // online they are told first (with the admin's signed record), so their
          // open session shows it at once.
          auto pv = peers_.find(site);
          if (pv != peers_.end() && pv->second.connected && !replaying_ && !windowsFrozen_) {
            Message n;
            n.head.object["m"] = Json::makeString("notice");
            n.head.object["n"] = noticeFor(site);
            send(site, kStreamControl, n);
          }
          if (pv != peers_.end()) {
            pv->second.connected = false;
            pv->second.presenceAt = -1;
          }
          const SiteId gone = site;
          outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(), [gone](const Outgoing& o) { return o.to == gone; }), outgoing_.end());
          for (auto& [n, inc] : incoming_)
            if (inc.from == gone) inc.askedAt = -1;
        }
      }
      if (r.readded && seenReadds_.insert(r.id).second && site == me_ && cfg_.expectReadmit) {
        cfg_.expectReadmit = false;
        // what we made while removed was dropped everywhere: our copy follows
        if (!windowsFrozen_ && honouredTop_.count(me_) && honouredTop_[me_] > r.upTo) {
          rebuildFor_[me_] = r.upTo;
          rebuildPending_ = true;
        }
      }
    }
  // departed members: nothing goes to them any more
  for (SiteId s : left_) {
    auto pv = peers_.find(s);
    if (pv != peers_.end()) pv->second.connected = false;
  }
  if (ended && !ended_) {
    ended_ = true;
    end_ = end;
    if (end.by != me_ && exit_ == ExitKind::None) {
      exit_ = ExitKind::Ended;
      exitPending_ = true;
    }
  }
}

void Session::writeCollabFlag(const char* key, const Json& value) {
  Json cj;
  if (!jsonParse(readCollabFileText(cfg_.folder), cj) || !cj.is(Json::Type::Object)) return;
  if (value.is(Json::Type::Null)) cj.object.erase(key);
  else cj.object[key] = value;
  writeFileDurable((fs::path(cfg_.folder) / kDir / "collab.json").string(), jsonStringify(cj, 2));
}

void Session::finishExit() {
  exitPending_ = false;
  if (stopped_ || exit_ == ExitKind::None) return;
  // the local work is safe: the log and the plain project file are current
  log_.sync();
  writeProjectFile();
  Json info = Json::makeObject();
  if (exit_ == ExitKind::Removed) {
    info.object["by"] = Json::makeString(siteHex(myRemoval_.by));
    info.object["byName"] = Json::makeString(myRemoval_.byName);
    info.object["at"] = Json::makeNumber(static_cast<double>(myRemoval_.at));
    info.object["reason"] = Json::makeString(myRemoval_.reason);
    writeCollabFlag("removed", info);
  } else {
    info.object["by"] = Json::makeString(siteHex(end_.by));
    info.object["byName"] = Json::makeString(end_.byName);
    info.object["at"] = Json::makeNumber(static_cast<double>(end_.at));
    writeCollabFlag("ended", info);
  }
  logging::info("collab", exit_ == ExitKind::Removed ? "removed from the project: the session stops" : "the admin stopped sharing: the session stops");
  detach();
  stopped_ = true;
  for (auto& [site, pv] : peers_) pv.connected = false;
}

std::string Session::batchHolding(SiteId author, const Path& path) const {
  auto it = batches_.find(author);
  if (it == batches_.end()) return {};
  for (auto b = it->second.rbegin(); b != it->second.rend(); ++b) {
    if (b->second.find(path.back()) == std::string::npos) continue;
    Batch x;
    if (!Batch::parse(b->second, x)) continue;
    for (const auto& op : x.ops)
      if (op.path == path) return b->second;
  }
  return {};
}

Json Session::noticeFor(SiteId peer) const {
  Json n = Json::makeObject();
  n.object["project"] = Json::makeString(projectId_);
  n.object["name"] = Json::makeString(projectName());
  if (ended_) {
    n.object["reason"] = Json::makeString("ended");
    n.object["by"] = Json::makeString(siteHex(end_.by));
    n.object["byName"] = Json::makeString(end_.byName);
    n.object["at"] = Json::makeNumber(static_cast<double>(end_.at));
    const std::string b = batchHolding(end_.by, {"meta", "ended"});
    if (!b.empty() && b.size() < 16384) n.object["batch"] = Json::makeString(b);
    return n;
  }
  const Removal* r = openRemoval(peer);
  if (!r) return Json();
  n.object["reason"] = Json::makeString("removed");
  n.object["who"] = Json::makeString(siteHex(peer));
  n.object["by"] = Json::makeString(siteHex(r->by));
  n.object["byName"] = Json::makeString(r->byName);
  n.object["at"] = Json::makeNumber(static_cast<double>(r->at));
  n.object["why"] = Json::makeString(r->reason);
  n.object["upTo"] = Json::makeNumber(static_cast<double>(r->upTo));
  n.object["id"] = Json::makeString(r->id);
  const std::string b = batchHolding(r->by, {"meta", "removal:" + siteHex(peer) + ":" + r->id});
  if (!b.empty() && b.size() < 16384) n.object["batch"] = Json::makeString(b);
  return n;
}

bool Session::learnNotice(const Json& n) {
  if (!n.is(Json::Type::Object) || n.text("project") != projectId_ || exit_ != ExitKind::None) return false;
  const std::string reason = n.text("reason");
  if (reason != "removed" && reason != "ended") return false;
  if (reason == "removed" && siteFromHex(n.text("who")) != me_) return false;
  const SiteId by = siteFromHex(n.text("by"));
  // the signed proof, when they still hold it: it must verify and say what the notice says
  const std::string bt = n.text("batch");
  if (!bt.empty()) {
    Batch b;
    if (!Batch::parse(bt, b) || b.site != by) return false;
    // (an admin who joined after we were last online is not known here: the refusal stands anyway)
    const Member* m = member(by);
    if (m || by == creator_) {
      if (!b.verify(m ? m->signPublic : creatorSign_)) return false;
      const Path want = reason == "ended" ? Path{"meta", "ended"} : Path{"meta", "removal:" + siteHex(me_) + ":" + n.text("id")};
      bool found = false;
      for (const auto& op : b.ops) found = found || op.path == want;
      if (!found) return false;
    }
  }
  if (reason == "removed") {
    myRemoval_ = Removal();
    myRemoval_.site = me_;
    myRemoval_.by = by;
    myRemoval_.byName = cleanName(n.text("byName"));
    myRemoval_.reason = n.text("why").substr(0, 300);
    myRemoval_.at = static_cast<uint64_t>(n.num("at", 0));
    myRemoval_.upTo = static_cast<uint64_t>(n.num("upTo", 0));
    myRemoval_.id = n.text("id");
    exit_ = ExitKind::Removed;
  } else {
    end_.by = by;
    end_.byName = cleanName(n.text("byName"));
    end_.at = static_cast<uint64_t>(n.num("at", 0));
    ended_ = true;
    exit_ = ExitKind::Ended;
  }
  exitPending_ = true;
  return true;
}

bool Session::removeMember(SiteId s, const std::string& reason, std::string* err) {
  auto fail = [&](const char* t) {
    if (err) *err = i18n::tr(t);
    return false;
  };
  if (stopped_) return fail("The project is not shared any more.");
  if (!isAdmin()) return fail("Only the project admin can remove people.");
  if (s == me_) return fail("You can't remove yourself. Use Leave project instead.");
  const Member* m = member(s);
  if (!m) return fail("They are not a member of this project.");
  if (openRemoval(s)) return fail("They were already removed.");
  std::string name = m->name;
  if (auto pv = peers_.find(s); pv != peers_.end() && !pv->second.profile.name.empty()) name = pv->second.profile.name;
  flushLocal();
  Op op;
  op.kind = OpKind::Ins;
  op.t = hlc_.tick();
  const std::string id = siteHex(me_) + "-" + siteHex(op.t);
  op.path = {"meta", "removal:" + siteHex(s) + ":" + id};
  Json v = Json::makeObject();
  v.object["by"] = Json::makeString(siteHex(me_));
  v.object["byName"] = Json::makeString(cleanName(cfg_.profile.name));
  v.object["name"] = Json::makeString(cleanName(name));
  std::string why = reason;
  for (char& c : why)
    if (static_cast<unsigned char>(c) < 0x20 && c != '\n') c = ' ';
  v.object["reason"] = Json::makeString(why.substr(0, 300));
  v.object["at"] = Json::makeNumber(static_cast<double>(wallNowMs()));
  // the cut: what we hold of theirs stays; anything later is dropped everywhere
  v.object["upTo"] = Json::makeNumber(static_cast<double>(vv_.count(s) ? vv_.at(s) : 0));
  op.value = std::move(v);
  return makeLocalBatch({op}, false, false);
}

bool Session::readmit(SiteId s) {
  if (stopped_ || !isAdmin()) return false;
  const Removal* r = openRemoval(s);
  if (!r) return true;
  const std::string id = r->id;
  flushLocal();
  Op op;
  op.kind = OpKind::Ins;
  op.t = hlc_.tick();
  op.path = {"meta", "readd:" + siteHex(s) + ":" + id};
  Json v = Json::makeObject();
  v.object["by"] = Json::makeString(siteHex(me_));
  v.object["seq"] = Json::makeNumber(static_cast<double>(mySeq_ + 1));  // the batch this record travels in
  v.object["at"] = Json::makeNumber(static_cast<double>(wallNowMs()));
  op.value = std::move(v);
  return makeLocalBatch({op}, false, false);
}

size_t Session::unackedBatches() const {
  uint64_t best = 0;
  bool anyone = false;
  for (const auto& m : activeMembers()) {
    if (m.site == me_) continue;
    anyone = true;
    auto it = peers_.find(m.site);
    if (it == peers_.end()) continue;
    auto k = it->second.known.find(me_);
    if (k != it->second.known.end()) best = std::max(best, k->second);
  }
  if (!anyone) return 0;
  return mySeq_ > best ? static_cast<size_t>(mySeq_ - best) : 0;
}

bool Session::announceLeave() {
  if (stopped_) return false;
  flushLocal();
  Op op;
  op.kind = OpKind::Ins;
  op.t = hlc_.tick();
  op.path = {"meta", "left:" + siteHex(me_)};
  Json v = Json::makeObject();
  v.object["at"] = Json::makeNumber(static_cast<double>(wallNowMs()));
  v.object["name"] = Json::makeString(cleanName(cfg_.profile.name));
  op.value = std::move(v);
  return makeLocalBatch({op}, false, false);
}

bool Session::stopSharing() {
  if (stopped_ || !isAdmin()) return false;
  flushLocal();
  Op op;
  op.kind = OpKind::Ins;
  op.t = hlc_.tick();
  op.path = {"meta", "ended"};
  Json v = Json::makeObject();
  v.object["by"] = Json::makeString(siteHex(me_));
  v.object["byName"] = Json::makeString(cleanName(cfg_.profile.name));
  v.object["at"] = Json::makeNumber(static_cast<double>(wallNowMs()));
  op.value = std::move(v);
  return makeLocalBatch({op}, false, false);
}

void Session::requestCleanSnapshot() {
  snapAskedAt_ = now_;
  Message m;
  m.head.object["m"] = Json::makeString("snapreq");
  Json need = Json::makeObject();
  for (const auto& [site, upTo] : rebuildFor_) need.object[siteHex(site)] = Json::makeNumber(static_cast<double>(upTo));
  m.head.object["need"] = std::move(need);
  for (auto& [s, pv] : peers_)
    if (pv.connected) send(s, kStreamControl, m);
}

// A removal cut off batches we had applied: the replica is rebuilt from the
// snapshot on disk with every removal known (their batches then change
// nothing), so we converge with everyone who never applied them. When the
// snapshot itself already holds them, a clean snapshot is asked for instead.
void Session::rebuild() {
  rebuildPending_ = false;
  if (rebuildFor_.empty() || stopped_) return;
  if (attached_) flushLocal();
  Json snap;
  {
    std::string t;
    if (readFileText((fs::path(cfg_.folder) / kDir / "snapshot.json").string(), t)) jsonParse(t, snap);
  }
  std::map<SiteId, uint64_t> hon;
  if (snap.is(Json::Type::Object))
    if (const Json* h = snap.find("honoured"); h && h->is(Json::Type::Object))
      for (const auto& [site, q] : h->object) hon[siteFromHex(site)] = static_cast<uint64_t>(q.number);
  bool clean = true;
  for (const auto& [site, upTo] : rebuildFor_)
    if (hon.count(site) && hon[site] > upTo) clean = false;
  if (snap.is(Json::Type::Object) && !snap.find("honoured")) {
    // an older snapshot: clean when it holds none of their batches past the cut
    const VersionVector svv = vvFromJson(snap.find("vv") ? *snap.find("vv") : Json());
    for (const auto& [site, upTo] : rebuildFor_)
      if (svv.count(site) && svv.at(site) > upTo) clean = false;
  }
  if (!clean) {
    logging::info("collab", "a removal cut off edits already in the snapshot: asking a collaborator for a clean one");
    tainted_ = true;
    requestCleanSnapshot();
    return;
  }
  // every batch we hold, replayed on the snapshot with the removals frozen in
  std::vector<Batch> held;
  for (const auto& [site, m] : batches_)
    for (const auto& [seq, text] : m) {
      Batch b;
      if (Batch::parse(text, b)) held.push_back(std::move(b));
    }
  if (snap.is(Json::Type::Object) && snap.find("replica")) {
    if (!replica_.fromJson(*snap.find("replica"))) return;
    vv_ = vvFromJson(snap.find("vv") ? *snap.find("vv") : Json());
    if (const Json* c = snap.find("conflicts")) conflicts_.fromJson(*c);
    else conflicts_.clear();
  } else {
    replica_.clear();
    vv_.clear();
    conflicts_.clear();
  }
  honouredTop_ = hon;
  for (auto& b : held) {
    const uint64_t have = vv_.count(b.site) ? vv_[b.site] : 0;
    if (b.seq > have) pending_[b.site][b.seq] = std::move(b);
  }
  const bool g = gesture_;
  gesture_ = false;
  windowsFrozen_ = true;
  replaying_ = true;
  drainPending();
  replaying_ = false;
  windowsFrozen_ = false;
  gesture_ = g;
  refreshMembership();
  {
    std::set<std::string> refs;
    const Json res = replica_.materialize({"meta", "resolutions"});
    if (res.is(Json::Type::Object))
      for (const auto& [k, r] : res.object)
        if (const Json* list = r.find("refs"); list && list->is(Json::Type::Array))
          for (const auto& x : list->array) refs.insert(x.str);
    conflicts_.setResolved(std::move(refs));
  }
  rebuildFor_.clear();
  ++rebuilds_;
  floDirty_ = true;
  remoteDirty_ = true;
  dirty_.full = true;
  compactNow();  // the snapshot on disk is clean from now on
  logging::info("collab", "replica rebuilt without a removed member's late edits");
}

std::vector<std::pair<std::string, uint64_t>> Session::unusedAudio() const {
  std::set<std::string> refs;
  // the project, everything the replica still holds (deleted content too: undo
  // and restores bring it back), every batch held, undo / redo records, the review
  audioRefsIn(jsonCompact(replica_.toJson()), refs);
  audioRefsIn(jsonCompact(songToJson(doc_.song(), false)), refs);
  for (const auto& [site, m] : batches_)
    for (const auto& [seq, text] : m) audioRefsIn(text, refs);
  for (const auto& [site, m] : pending_)
    for (const auto& [seq, b] : m) audioRefsIn(b.text(), refs);
  for (const auto& [serial, rec] : steps_)
    for (const auto& g : rec.groups) {
      audioRefsIn(jsonCompact(g.backVal), refs);
      audioRefsIn(jsonCompact(g.fwdVal), refs);
      audioRefsIn(jsonCompact(g.backContent), refs);
    }
  audioRefsIn(jsonCompact(conflicts_.toJson()), refs);
  for (const auto& [name, inc] : incoming_) refs.insert(name);
  std::vector<std::pair<std::string, uint64_t>> out;
  std::error_code ec;
  const fs::path dir = fs::path(cfg_.folder) / "Audio";
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string rel = "Audio/" + e.path().filename().string();
    if (refs.count(rel)) continue;
    out.emplace_back(rel, static_cast<uint64_t>(e.file_size(ec)));
  }
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t Session::cleanUnusedAudio() {
  uint64_t freed = 0;
  for (const auto& [rel, size] : unusedAudio()) {
    std::error_code ec;
    if (fs::remove(localPath(rel), ec)) freed += size;
  }
  return freed;
}

// ----------------------------------------------------------------- files ----
std::vector<FileProgress> Session::transfers() const {
  std::vector<FileProgress> out;
  for (const auto& [n, inc] : incoming_) out.push_back(FileProgress{n, inc.have, inc.total, inc.failed});
  return out;
}

bool Session::fileLoading(const std::string& absPath, float* progress) const {
  const std::string base = (fs::path(cfg_.folder) / "Audio").lexically_normal().generic_string() + "/";
  const std::string g = fs::path(absPath).lexically_normal().generic_string();
  if (g.rfind(base, 0) != 0) return false;
  auto it = incoming_.find("Audio/" + g.substr(base.size()));
  if (it == incoming_.end()) return false;
  if (progress) *progress = it->second.total ? static_cast<float>(it->second.have) / static_cast<float>(it->second.total) : 0.0f;
  return true;
}

void Session::scanFiles() {
  if (!ready_) return;
  const std::string base = (fs::path(cfg_.folder) / "Audio").lexically_normal().generic_string() + "/";
  std::set<std::string> wanted;
  Song copy = doc_.song();
  forEachFileRef(copy, [&](std::string& p) {
    const std::string g = fs::path(p).lexically_normal().generic_string();
    if (g.rfind(base, 0) != 0) return;
    const std::string rel = "Audio/" + g.substr(base.size());
    if (!safeAudioName(rel)) return;
    std::error_code ec;
    if (!fs::exists(localPath(rel), ec)) wanted.insert(rel);
  });
  for (const auto& w : wanted)
    if (!incoming_.count(w)) {
      Incoming inc;
      std::error_code ec;
      const fs::path part = fs::path(cfg_.folder) / kDir / "partial" / (fs::path(w).filename().string() + ".part");
      if (fs::exists(part, ec)) inc.have = fs::file_size(part, ec);
      incoming_[w] = inc;
    }
  for (auto it = incoming_.begin(); it != incoming_.end();)
    it = wanted.count(it->first) || it->second.verifying ? std::next(it) : incoming_.erase(it);
  // partial downloads nobody needs any more, older than 7 days
  std::error_code ec;
  const fs::path pdir = fs::path(cfg_.folder) / kDir / "partial";
  if (fs::exists(pdir, ec))
    for (const auto& e : fs::directory_iterator(pdir, ec)) {
      const std::string rel = "Audio/" + e.path().stem().string();
      if (incoming_.count(rel)) continue;
      const auto age = fs::file_time_type::clock::now() - fs::last_write_time(e.path(), ec);
      if (!ec && age > std::chrono::hours(24 * 7)) fs::remove(e.path(), ec);
    }
}

void Session::requestFiles() {
  for (auto& [name, inc] : incoming_) {
    if (inc.failed || inc.verifying) continue;
    if (inc.askedAt >= 0 && now_ - inc.askedAt < 10.0) continue;
    // round robin over connected peers
    std::vector<SiteId> conn;
    for (const auto& [s, pv] : peers_)
      if (pv.connected) conn.push_back(s);
    if (conn.empty()) continue;
    size_t k = 0;
    for (size_t i = 0; i < conn.size(); ++i)
      if (conn[i] == inc.from) k = (i + 1) % conn.size();
    inc.from = conn[k];
    inc.askedAt = now_;
    Message m;
    m.head.object["m"] = Json::makeString("fwant");
    m.head.object["f"] = Json::makeString(name);
    m.head.object["off"] = Json::makeNumber(static_cast<double>(inc.have));
    send(inc.from, kStreamBulk, m);
  }
}

void Session::onFileWant(SiteId from, const Message& m) {
  const std::string name = m.head.text("f");
  if (!safeAudioName(name)) return;
  std::error_code ec;
  const std::string p = localPath(name);
  if (!fs::is_regular_file(p, ec)) {
    Message r;
    r.head.object["m"] = Json::makeString("fnone");
    r.head.object["f"] = Json::makeString(name);
    send(from, kStreamBulk, r);
    return;
  }
  Outgoing o;
  o.to = from;
  o.name = name;
  o.size = fs::file_size(p, ec);
  o.offset = std::min<uint64_t>(static_cast<uint64_t>(std::max(0.0, m.head.num("off", 0))), o.size);
  // one transfer per (peer, file)
  for (auto& x : outgoing_)
    if (x.to == from && x.name == name) {
      x.offset = o.offset;
      return;
    }
  outgoing_.push_back(o);
}

void Session::pumpFileSends() {
  size_t budget = fileBudget;  // per frame
  while (!outgoing_.empty() && budget > 0) {
    const size_t chunkSize = std::min(kFileChunk, budget);
    Outgoing& o = outgoing_.front();
    if (bulkQueuedFn && bulkQueuedFn(o.to) > (1u << 20)) {
      // this peer's link is full: try the next transfer
      if (outgoing_.size() > 1) {
        outgoing_.push_back(o);
        outgoing_.pop_front();
      }
      break;
    }
    std::ifstream f(localPath(o.name), std::ios::binary);
    if (!f) {
      outgoing_.pop_front();
      continue;
    }
    f.seekg(static_cast<std::streamoff>(o.offset));
    std::string chunk(chunkSize, '\0');
    f.read(&chunk[0], static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<size_t>(f.gcount()));
    Message m;
    m.head.object["m"] = Json::makeString("fdata");
    m.head.object["f"] = Json::makeString(o.name);
    m.head.object["off"] = Json::makeNumber(static_cast<double>(o.offset));
    m.head.object["total"] = Json::makeNumber(static_cast<double>(o.size));
    m.bin = std::move(chunk);
    const size_t n = m.bin.size();
    send(o.to, kStreamBulk, m);
    o.offset += n;
    budget = budget > n ? budget - n : 0;
    if (n == 0 || o.offset >= o.size) outgoing_.pop_front();
  }
}

void Session::onFileData(SiteId from, const Message& m) {
  const std::string name = m.head.text("f");
  auto it = incoming_.find(name);
  if (it == incoming_.end() || !safeAudioName(name)) return;
  Incoming& inc = it->second;
  if (inc.verifying) return;
  const uint64_t off = static_cast<uint64_t>(std::max(0.0, m.head.num("off", 0)));
  inc.total = static_cast<uint64_t>(std::max(0.0, m.head.num("total", 0)));
  inc.from = from;
  inc.askedAt = now_;
  std::error_code ec;
  const fs::path pdir = fs::path(cfg_.folder) / kDir / "partial";
  fs::create_directories(pdir, ec);
  const fs::path part = pdir / (fs::path(name).filename().string() + ".part");
  const uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
  if (off != have) {
    inc.have = have;
    return;  // out of step (a resend from an older request): the next request resumes at `have`
  }
  {
    std::ofstream f(part, std::ios::binary | std::ios::app);
    f.write(m.bin.data(), static_cast<std::streamsize>(m.bin.size()));
  }
  inc.have = have + m.bin.size();
  if (inc.total > 0 && inc.have >= inc.total) finishFile(name);
}

void Session::finishFile(const std::string& name) {
  auto it = incoming_.find(name);
  if (it == incoming_.end()) return;
  Incoming& inc = it->second;
  std::error_code ec;
  const fs::path part = fs::path(cfg_.folder) / kDir / "partial" / (fs::path(name).filename().string() + ".part");
  const std::string stem = fs::path(name).filename().string().substr(0, 64);
  const std::string h = update::sha256File(part.string());
  if (h != stem || (inc.total && fs::file_size(part, ec) != inc.total)) {
    fs::remove(part, ec);
    inc.have = 0;
    inc.askedAt = -1;
    if (++inc.tries >= 3) {
      inc.failed = true;
      // a notice, never an alert sound: it comes from a collaborator's file, not from what you did
      if (onNotice) onNotice(i18n::tr("A file of the project arrived damaged three times and was not used") + std::string(": ") + name, false);
    }
    return;
  }
  const std::string dst = localPath(name);
  fs::create_directories(fs::path(dst).parent_path(), ec);
  fs::rename(part, dst, ec);
  if (ec) {
    fs::copy_file(part, dst, fs::copy_options::overwrite_existing, ec);
    fs::remove(part, ec);
  }
  incoming_.erase(it);
  sampleCache().forget(dst);
}

}  // namespace gl::collab
