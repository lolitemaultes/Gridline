// Gridline — project interop. See ProjectInterop.h.
#include "ProjectInterop.h"
#include "I18n.h"
#include "Platform.h"
#include "ProjectArchive.h"
#include "ProjectFiles.h"
#include "RemoteFs.h"
#include "SfzText.h"
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
#include "../engine/TrackReceiptPass.h"

namespace fs = std::filesystem;

namespace gl {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- what a value in a plugin state refers to --------------------------------------------------
//
// Every plugin type keeps its files in its own format; visitState knows them all. A reference is
// handed to the caller without its wrapping ("sf2:<bank>#<n>" and "file:<path>" arrive as the file)
// and put back with it, so a caller only ever sees and changes a path. Values that name no file
// (generated tones, the built-in kit, media embedded in the state) never reach the caller.

enum class RefKind { File, Folder };
using RefFn = std::function<void(std::string&, RefKind)>;

bool visitState(PluginState& st, const RefFn& fn);

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool isVirtualRef(const std::string& s) {
  return s.empty() || s == "none" || s == "-" || startsWith(s, "tone:") || startsWith(s, "builtin:") || startsWith(s, "media:") ||
         startsWith(s, "factory:");
}

// One reference; returns true when it changed.
bool visitRef(std::string& ref, const RefFn& fn, RefKind kind = RefKind::File) {
  if (isVirtualRef(ref)) return false;
  std::string head, tail, path = ref;
  if (startsWith(ref, "file:")) {
    head = "file:";
    path = ref.substr(5);
  } else if (startsWith(ref, "sf2:")) {  // "sf2:<bank>#<sample>": the bank is the file
    const size_t hash = ref.rfind('#');
    head = "sf2:";
    path = hash == std::string::npos ? ref.substr(4) : ref.substr(4, hash - 4);
    if (hash != std::string::npos) tail = ref.substr(hash);
  }
  if (path.empty()) return false;
  const std::string before = path;
  fn(path, kind);
  if (path == before || path.find('\n') != std::string::npos) return false;
  ref = head + path + tail;
  return true;
}

// Calls `edit` for each line; the data is only rebuilt when a line changed, so nothing else moves.
template <class Edit>
bool editLines(std::string& data, Edit edit) {
  std::string out;
  bool changed = false;
  size_t index = 0;
  for (size_t at = 0;; ++index) {
    const size_t nl = data.find('\n', at);
    std::string line = data.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    changed = edit(line, index) || changed;
    out += line;
    if (nl == std::string::npos) break;
    out += '\n';
    at = nl + 1;
  }
  if (changed) data = std::move(out);
  return changed;
}

// The reference after a marker in a line: "F <path>", "L3.2 <path>".
bool visitAfter(std::string& line, size_t from, const RefFn& fn, RefKind kind = RefKind::File) {
  std::string ref = line.substr(from);
  if (!visitRef(ref, fn, kind)) return false;
  line.replace(from, std::string::npos, ref);
  return true;
}

bool visitAfterBar(std::string& line, const RefFn& fn) {  // zone maps: the first " | " (names are encoded)
  const size_t bar = line.find(" | ");
  return bar != std::string::npos && visitAfter(line, bar + 3, fn);
}

// ---- JSON states ----

using Edits = std::map<std::string, std::string>;  // string value -> its new value

void visitJsonString(Json& v, const RefFn& fn, Edits& edits) {
  const std::string before = v.str;
  if (visitRef(v.str, fn)) edits[before] = v.str;
}

// Plasma: the references are named fields ("table" only as "file:..."); names of tables and presets
// are not files, and "media" holds what is embedded.
void walkPlasma(Json& j, const RefFn& fn, Edits& edits) {
  if (j.type == Json::Type::Array) {
    for (auto& v : j.array) walkPlasma(v, fn, edits);
  } else if (j.type == Json::Type::Object) {
    for (auto& [k, v] : j.object) {
      if (k == "media") continue;
      if (v.type == Json::Type::String) {
        if (k == "sample" || k == "sfz" || k == "noiseFile" || k == "ir" || (k == "table" && startsWith(v.str, "file:")))
          visitJsonString(v, fn, edits);
      } else {
        walkPlasma(v, fn, edits);
      }
    }
  }
}

// An object with "type" and "data" is a plugin state inside this one (patcher nodes): it follows the
// rules of its own type. `strings`: other string values that look like a file are references too
// (states whose format is not known here); a patcher's are labels.
void walkJson(Json& j, const RefFn& fn, Edits& edits, bool strings) {
  switch (j.type) {
    case Json::Type::String:
      if (strings && looksLikeFilePath(j.str)) visitJsonString(j, fn, edits);
      break;
    case Json::Type::Array:
      for (auto& v : j.array) walkJson(v, fn, edits, strings);
      break;
    case Json::Type::Object: {
      const Json* type = j.find("type");
      auto data = j.object.find("data");
      const bool nested = type && type->type == Json::Type::String && data != j.object.end() && data->second.type == Json::Type::String;
      if (nested) {
        PluginState inner;
        inner.type = type->str;
        inner.data = data->second.str;
        const std::string before = inner.data;
        if (visitState(inner, fn)) edits[before] = inner.data;
      }
      for (auto& [k, v] : j.object)
        if (!(nested && (k == "data" || k == "type"))) walkJson(v, fn, edits, strings);
      break;
    }
    default: break;
  }
}

std::string jsonStringToken(const std::string& s) {
  std::string t = jsonStringify(Json::makeString(s), 0);
  while (!t.empty() && t.back() == '\n') t.pop_back();
  return t;
}

// Replaces string values in JSON text and leaves every other byte alone: key order matters to some
// formats (a patch starts with "v"), and what did not change stays the same.
std::string replaceJsonStrings(const std::string& text, const Edits& edits) {
  size_t shortest = std::string::npos, longest = 0;
  for (const auto& [from, to] : edits) {
    shortest = std::min(shortest, from.size());
    longest = std::max(longest, from.size());
  }
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] != '"') {
      out += text[i++];
      continue;
    }
    size_t e = i + 1;
    while (e < text.size() && text[e] != '"') e += text[e] == '\\' ? 2 : 1;
    if (e >= text.size()) {
      out.append(text, i, std::string::npos);
      break;
    }
    const size_t tokenSize = e + 1 - i;
    size_t after = e + 1;
    while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) ++after;
    const bool isKey = after < text.size() && text[after] == ':';
    bool replaced = false;
    if (!isKey && tokenSize >= shortest + 2 && tokenSize <= longest * 6 + 2) {
      Json v;
      if (jsonParse(text.substr(i, tokenSize), v) && v.type == Json::Type::String) {
        if (auto it = edits.find(v.str); it != edits.end()) {
          out += jsonStringToken(it->second);
          replaced = true;
        }
      }
    }
    if (!replaced) out.append(text, i, tokenSize);
    i = e + 1;
  }
  return out;
}

bool visitJsonState(PluginState& st, const RefFn& fn) {
  Json j;
  if (!jsonParse(st.data, j)) return false;
  Edits edits;
  if (st.type == "gl.plasma") {
    walkPlasma(j, fn, edits);
  } else if (st.type == "gl.edison") {  // "path" only: its "name" is a label, "flExtra" a blob
    if (auto it = j.object.find("path"); it != j.object.end() && it->second.type == Json::Type::String) visitJsonString(it->second, fn, edits);
  } else {
    walkJson(j, fn, edits, st.type != "gl.patcher" && st.type != "gl.patcherfx");
  }
  if (edits.empty()) return false;
  st.data = replaceJsonStrings(st.data, edits);
  return true;
}

// ---- text states ----

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A region model's "file <hex>" lines (engine/SlicexEngine: one per deck, "-" when empty).
bool visitHexLine(std::string& line, size_t from, const RefFn& fn) {
  const std::string hex = line.substr(from);
  if (hex.empty() || hex.size() % 2 != 0) return false;
  std::string ref;
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int a = hexDigit(hex[i]), b = hexDigit(hex[i + 1]);
    if (a < 0 || b < 0) return false;
    ref += static_cast<char>(a * 16 + b);
  }
  if (!visitRef(ref, fn)) return false;
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (char c : ref) {
    const auto u = static_cast<unsigned char>(c);
    out += kHex[u >> 4];
    out += kHex[u & 15];
  }
  line.replace(from, std::string::npos, out);
  return true;
}

bool visitTextState(PluginState& st, const RefFn& fn) {
  const std::string& t = st.type;
  if (t == "gl.pads") {
    // One line per pad of bank A, then keyed lines; "L<pad>.<layer> <path>" hold the other layers.
    return editLines(st.data, [&](std::string& l, size_t i) {
      if (i < 16) return visitRef(l, fn);
      if (l.size() > 2 && l[0] == 'L' && std::isdigit(static_cast<unsigned char>(l[1])))
        if (const size_t sp = l.find(' '); sp != std::string::npos) return visitAfter(l, sp + 1, fn);
      return false;
    });
  }
  if (t == "gl.multisampler") {
    const bool zones = startsWith(st.data, "#gl-zones") || startsWith(st.data, "z ") || startsWith(st.data, "sfz ") || st.data.find("\nz ") != std::string::npos;
    if (!zones) {  // a bare path (a file dropped on the channel): its first line that is not blank
      bool done = false;
      return editLines(st.data, [&](std::string& l, size_t) {
        if (done || l.empty()) return false;
        done = true;
        return visitRef(l, fn);
      });
    }
    return editLines(st.data, [&](std::string& l, size_t) { return (startsWith(l, "z ") || startsWith(l, "sf2 ") || startsWith(l, "sfz ")) && visitAfterBar(l, fn); });  // K-144: an SFZ instrument
  }
  if (t == "gl.waveprogram")
    return editLines(st.data, [&](std::string& l, size_t) { return startsWith(l, "F ") && visitAfter(l, 2, fn); });
  if (t == "gl.gms")
    return editLines(st.data, [&](std::string& l, size_t) { return l.size() > 4 && startsWith(l, "w ") && l[3] == ' ' && visitAfter(l, 4, fn); });
  if (t == "gl.harmor" && startsWith(st.data, "hmr ")) {  // "x <part> <source>": an audio file or a picture
    return editLines(st.data, [&](std::string& l, size_t) {
      if (!startsWith(l, "x ")) return false;
      const size_t sp = l.find(' ', 2);
      return sp != std::string::npos && visitAfter(l, sp + 1, fn);
    });
  }
  if (t == "gl.amprack")  // "cabdir=<folder>": the cabinet files are found by name inside it
    return editLines(st.data, [&](std::string& l, size_t) { return startsWith(l, "cabdir=") && visitAfter(l, 7, fn, RefKind::Folder); });
  if (t == "gl.imagesynth")  // the picture, then the canvas
    return editLines(st.data, [&](std::string& l, size_t i) { return i == 0 && visitRef(l, fn); });
  if (t == "gl.slicer") {
    // The sample, and in a region model "file <hex>" per deck (the first deck names the same file).
    return editLines(st.data, [&](std::string& l, size_t i) {
      if (i == 0) return visitRef(l, fn);
      return startsWith(l, "file ") && visitHexLine(l, 5, fn);
    });
  }
  const PluginInfo* info = findPlugin(t);
  if (info && info->usesSample) {
    // "<sample file>\n<model>" (J-41); every later line "file <path>" of the models that refer to
    // more files (J-151: a DJ setup's second deck and the tracks its takes were played on).
    return editLines(st.data, [&](std::string& l, size_t i) {
      if (i == 0) return visitRef(l, fn);
      return startsWith(l, "file ") && visitAfter(l, 5, fn);
    });
  }
  // Other states hold a path only as all they are.
  return looksLikeFilePath(st.data) && visitRef(st.data, fn);
}

// One plugin state; returns true when a reference changed.
bool visitState(PluginState& st, const RefFn& fn) {
  if (st.data.empty() || isHostedType(st.type)) return false;  // a hosted plugin's state is its own data: never read as ours, never edited
  const char first = st.data.front();
  return first == '{' || first == '[' ? visitJsonState(st, fn) : visitTextState(st, fn);
}

int visitSong(Song& song, const RefFn& fn) {
  int changed = 0;
  for (auto& c : song.channels) {
    changed += visitState(c.plugin, fn) ? 1 : 0;
    for (auto& nf : c.noteFx) changed += visitState(nf, fn) ? 1 : 0;
    if (c.freeze.frozen()) changed += visitState(c.freeze.render, fn) ? 1 : 0;  // K-87: the frozen audio
  }
  for (auto& m : song.mixer) {
    for (auto& sl : m.slots) changed += visitState(sl.plugin, fn) ? 1 : 0;
    if (m.freeze.frozen()) changed += visitState(m.freeze.render, fn) ? 1 : 0;
  }
  return changed;
}

RefFn filesOnly(const std::function<void(std::string&)>& fn) {
  return [&fn](std::string& p, RefKind k) {
    if (k == RefKind::File) fn(p);
  };
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

// A path from a project: never throws (on Windows a name that is not valid UTF-8 cannot become a path).
fs::path pathOf(const std::string& p) {
  try {
    return fs::u8path(p);
  } catch (const std::exception&) {
    return {};
  }
}

bool existsFile(const std::string& p) {
  std::error_code ec;
  const fs::path f = pathOf(p);
  return f.is_absolute() && fs::is_regular_file(f, ec);
}

// ---- packing: what goes beside a project ---------------------------------------------------------

std::string fileNameOf(const std::string& p) { return fs::u8path(p).filename().u8string(); }

std::string numberedName(const std::string& fileName, int k) {
  if (k <= 1) return fileName;
  const fs::path f = fs::u8path(fileName);
  return f.stem().u8string() + " (" + std::to_string(k) + ")" + f.extension().u8string();
}

bool isSfz(const std::string& p) { return lower(fs::u8path(p).extension().u8string()) == ".sfz"; }

bool existsFolder(const std::string& p) {
  std::error_code ec;
  const fs::path f = pathOf(p);
  return f.is_absolute() && (fs::is_directory(f, ec) || (isFolderManifest(p) && fs::is_regular_file(f, ec)));
}

// The name a folder reference gets in a package.
std::string folderNameOf(const std::string& p) {
  if (!isFolderManifest(p)) return fileNameOf(p);
  return "Cabinet " + pathOf(p).stem().u8string().substr(0, 8);
}

// Files that third-party plugins name inside their own saved state. Gridline cannot change what such
// a state says, so these files travel with the project and the report names them.
std::vector<std::string> insidePluginFiles(const Song& song) {
  std::set<std::string> seen;
  std::vector<std::string> out;
  auto addFrom = [&](const std::string& data) {
    for (const auto& p : receipt::chunkFileNames(data))
      if (existsFile(p) && seen.insert(p).second) out.push_back(p);
  };
  // Patches hold plugin states of their own, at any depth.
  std::function<void(const Json&, int)> walk;
  std::function<void(const PluginState&, int)> scanAt = [&](const PluginState& st, int depth) {
    if (st.empty() || depth > 16) return;
    if (isHostedType(st.type)) addFrom(st.data);
    if (!st.original.empty()) addFrom(st.original);
    if (!st.data.empty() && st.data.front() == '{') {
      Json j;
      if (jsonParse(st.data, j)) walk(j, depth);
    }
  };
  walk = [&](const Json& j, int depth) {
    if (j.type == Json::Type::Array) {
      for (const auto& v : j.array) walk(v, depth);
    } else if (j.type == Json::Type::Object) {
      const Json* type = j.find("type");
      const Json* data = j.find("data");
      if (type && data && type->type == Json::Type::String && data->type == Json::Type::String) {
        PluginState inner;
        inner.type = type->str;
        inner.data = data->str;
        scanAt(inner, depth + 1);
      }
      for (const auto& [k, v] : j.object)
        if (v.type == Json::Type::Object || v.type == Json::Type::Array) walk(v, depth);
    }
  };
  auto scan = [&](const PluginState& st) { scanAt(st, 0); };
  for (const auto& c : song.channels) {
    scan(c.plugin);
    for (const auto& nf : c.noteFx) scan(nf);
  }
  for (const auto& m : song.mixer)
    for (const auto& sl : m.slots) scan(sl.plugin);
  return out;
}

// The saved Track Receipt fields go by file: new names for moved files, a bare name for any other
// (a project never keeps a path it no longer refers to; files match by name too).
void relinkReceipt(Song& song, const std::map<std::string, std::string>& moved) {
  if (song.trackReceipt.empty()) return;
  Json j;
  if (!jsonParse(song.trackReceipt, j)) return;
  auto files = j.object.find("files");
  if (files == j.object.end() || files->second.type != Json::Type::Object) return;
  std::map<std::string, Json> next;
  bool changed = false;
  for (auto& [k, v] : files->second.object) {
    std::string key = k;
    if (auto it = moved.find(k); it != moved.end()) key = it->second;
    else if (pathOf(k).is_absolute()) key = fileNameOf(k);
    changed = changed || key != k;
    next[key] = v;
  }
  if (!changed) return;
  files->second.object = std::move(next);
  song.trackReceipt = jsonStringify(j, 0);
  while (!song.trackReceipt.empty() && song.trackReceipt.back() == '\n') song.trackReceipt.pop_back();
}

void addAbsolutePaths(const std::string& s, std::set<std::string>& seen, std::vector<std::string>& out) {
  for (size_t at = 0; at < s.size(); ++at) {
    const char c = s[at];
    const bool drive = at + 2 < s.size() && std::isalpha(static_cast<unsigned char>(c)) && s[at + 1] == ':' && (s[at + 2] == '\\' || s[at + 2] == '/');
    if (c != '/' && !drive) continue;
    const char before = at ? s[at - 1] : ' ';
    if (!(before == ' ' || before == ':' || before == '=' || before == '"' || before == '|' || before == '\n')) continue;
    size_t end = s.find_first_of("\n\"", at);
    if (end == std::string::npos) end = s.size();
    if (end - at > 4096) continue;
    std::string cand = s.substr(at, end - at);
    if (!existsFile(cand)) {  // "sf2:<bank>#<n>"
      const size_t hash = cand.rfind('#');
      if (hash == std::string::npos) continue;
      cand.resize(hash);
      if (!existsFile(cand)) continue;
    }
    if (seen.insert(cand).second) out.push_back(cand);
    at = end;
  }
}

void walkAbsolutePaths(const Json& j, std::set<std::string>& seen, std::vector<std::string>& out) {
  if (j.type == Json::Type::String) {
    addAbsolutePaths(j.str, seen, out);
  } else if (j.type == Json::Type::Array) {
    for (const auto& v : j.array) walkAbsolutePaths(v, seen, out);
  } else if (j.type == Json::Type::Object) {
    for (const auto& [k, v] : j.object) walkAbsolutePaths(v, seen, out);
  }
}

const char* const kCopyingStatus = i18n::N_("Copying the project's files");

// K-47: how far an export is, in bytes of the files it carries.
class ExportMeter {
 public:
  ExportMeter(const ExportControl* c, const char* status) : c_(c), status_(status) {}
  bool on() const { return c_ != nullptr; }
  bool stopped() const { return c_ && c_->cancelled(); }
  void addTotal(const fs::path& file) {
    if (!c_) return;
    std::error_code ec;
    const uint64_t n = fs::file_size(file, ec);
    if (!ec) total_ += n;
  }
  void advance(uint64_t bytes) {
    done_ += bytes;
    if (!c_ || !c_->progress) return;
    // The last part of the bar is what follows the files (the project itself, the archive's index).
    c_->progress(total_ ? 0.95 * std::min(1.0, static_cast<double>(done_) / static_cast<double>(total_)) : 0.0, status_);
  }
  void say(double done, const char* status) const {
    if (c_ && c_->progress) c_->progress(done, status);
  }

 private:
  const ExportControl* c_;
  std::string status_;
  uint64_t total_ = 0, done_ = 0;
};

// Under a temporary name, so nothing cut off ever stands under the real one. False: failed
// (rep.error) or stopped (rep.cancelled).
bool copyInChunks(const fs::path& src, const fs::path& dst, CollectReport& rep, ExportMeter& meter) {
  const fs::path tmp = fs::u8path(dst.u8string() + ".part");
  std::error_code ec;
  auto drop = [&] { fs::remove(tmp, ec); };
  const std::string cannotWrite = "cannot copy " + src.u8string() + ": could not write " + dst.u8string() + " (is the disk full?)";
  {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
      out.close();
      drop();
      rep.error = in ? cannotWrite : "cannot copy " + src.u8string();
      return false;
    }
    std::vector<char> buf(1u << 20);
    while (in) {
      if (meter.stopped()) {
        rep.cancelled = true;
        out.close();
        drop();
        return false;
      }
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize got = in.gcount();
      if (got > 0) out.write(buf.data(), got);
      if (!out || in.bad()) {
        out.close();
        drop();
        rep.error = cannotWrite;
        return false;
      }
      meter.advance(static_cast<uint64_t>(got));
    }
    out.close();
    if (!out) {
      drop();
      rep.error = cannotWrite;
      return false;
    }
  }
  fs::permissions(tmp, fs::status(src, ec).permissions(), ec);
  ec.clear();
  std::string why;
  if (!platform::replaceFile(tmp.u8string(), dst.u8string(), &why)) {
    drop();
    rep.error = "cannot copy " + src.u8string() + ": " + why;
    return false;
  }
  return true;
}

// Copies `src` to `dst` unless the same content is there. False + the reason on failure.
bool copyInto(const fs::path& src, const fs::path& dst, CollectReport& rep, ExportMeter& meter) {
  std::error_code ec;
  if (meter.stopped()) {
    rep.cancelled = true;
    return false;
  }
  const bool there = fs::exists(dst, ec);
  if (there && (fs::equivalent(dst, src, ec) || sameContent(dst, src))) {
    ++rep.reused;
    if (meter.on()) {
      const uint64_t n = fs::file_size(src, ec);
      if (!ec) meter.advance(n);
    }
    return true;
  }
  fs::create_directories(dst.parent_path(), ec);
  if (meter.on()) {
    if (!copyInChunks(src, dst, rep, meter)) return false;
  } else if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
    rep.error = "cannot copy " + src.u8string() + ": " + ec.message();
    return false;
  }
  if (!there) rep.created.push_back(dst.u8string());
  ++rep.copied;
  return true;
}

// A subfolder of `folder` named after `name`, free of files other than what `fits` accepts.
fs::path freeFolder(const fs::path& folder, const std::string& name, std::set<std::string>& usedNames,
                    const std::function<bool(const fs::path&)>& fits) {
  fs::path dir;
  std::error_code ec;
  for (int k = 1;; ++k) {
    dir = folder / fs::u8path(k == 1 ? name : name + " (" + std::to_string(k) + ")");
    if (usedNames.count(lower(dir.u8string()))) continue;
    if (!fs::exists(dir, ec) || fits(dir)) break;
  }
  usedNames.insert(lower(dir.u8string()));
  return dir;
}

}  // namespace

bool looksLikeFilePath(const std::string& s) {
  if (s.size() < 5 || s.size() > 4096 || s.find('\n') != std::string::npos) return false;
  static const std::set<std::string> kExt = {".wav", ".wave", ".flac", ".ogg", ".oga", ".mp3", ".aif", ".aiff", ".aifc", ".wv",
                                             ".w64", ".rf64", ".caf", ".m4a", ".mid", ".midi", ".sf2", ".sfz", ".png", ".jpg",
                                             ".jpeg", ".bmp", ".rx2", ".rex", ".rcy", ".aac", ".ir", ".fst", ".zip"};
  const size_t dot = s.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return false;
  return kExt.count(lower(s.substr(dot))) > 0;
}

bool forEachStateFileRef(PluginState& state, const std::function<void(std::string& path)>& fn) { return visitState(state, filesOnly(fn)); }

int forEachFileRef(Song& song, const std::function<void(std::string& path)>& fn) { return visitSong(song, filesOnly(fn)); }

int forEachRef(Song& song, const std::function<void(std::string& path, bool folder)>& fn) {
  return visitSong(song, [&fn](std::string& p, RefKind k) { fn(p, k == RefKind::Folder); });
}

bool forEachStateRef(PluginState& state, const std::function<void(std::string& path, bool folder)>& fn) {
  return visitState(state, [&fn](std::string& p, RefKind k) { fn(p, k == RefKind::Folder); });
}

bool isFolderManifest(const std::string& path) {
  static const std::string ext = kFolderManifestExt;
  return path.size() > ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

std::string folderManifestText(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string t = "gridline-folder 1\n";
  for (const auto& [name, stored] : files) t += name + "\t" + stored + "\n";
  return t;
}

std::vector<std::pair<std::string, std::string>> folderRefFiles(const std::string& ref) {
  std::vector<std::pair<std::string, std::string>> out;
  std::error_code ec;
  const fs::path dir = pathOf(ref);
  if (isFolderManifest(ref)) {
    std::ifstream f(dir, std::ios::binary);
    std::string line;
    if (!f || !std::getline(f, line) || line != "gridline-folder 1") return out;
    while (std::getline(f, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      const fs::path stored = pathOf(line.substr(tab + 1)), name = pathOf(line.substr(0, tab));
      if (stored.has_parent_path() || name.has_parent_path() || stored.empty() || name.empty()) continue;  // names only, nothing outside
      out.emplace_back((dir.parent_path() / stored).u8string(), name.u8string());
    }
    return out;
  }
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::error_code e2;
    if ((e.path().extension() == ".wav" || e.path().extension() == ".wv") && e.is_regular_file(e2))
      out.emplace_back(e.path().u8string(), e.path().filename().u8string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> bundleMembers(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  if (isFolderManifest(path)) {
    for (const auto& [f, name] : folderRefFiles(path)) out.push_back(fileNameOf(f));
  } else if (isSfz(path) && fs::is_regular_file(pathOf(path), ec)) {
    // Every name a map gives, whether it is there yet or not: a map in a shared project names its
    // samples beside it, and they arrive after it.
    const SfzBundle b = sfzBundle(path);
    for (const auto& [src, member] : b.members) out.push_back(fileNameOf(src));
    for (const auto& m : b.missing) out.push_back(fileNameOf(m));
  }
  return out;
}

SfzBundle sfzBundle(const std::string& path, const std::function<std::string(const std::string& source)>& memberName) {
  SfzBundle out;
  sfz::Expanded expanded;
  if (!sfz::expand(path, expanded)) return out;
  std::string text = std::move(expanded.text);
  out.missing = std::move(expanded.missing);
  const fs::path dir = fs::u8path(path).parent_path();
  std::string defaultPath;
  std::map<std::string, std::string> nameOf;  // source -> member name
  std::set<std::string> used = {lower(fileNameOf(path))};
  auto slashes = [](std::string v) {
    std::replace(v.begin(), v.end(), '\\', '/');
    return v;
  };
  // Opcodes are read the way the player does (plasma_wt_dsp.cpp, parseSfz).
  size_t i = 0;
  while (i < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    if (text[i] == '<') {
      const size_t e = text.find('>', i);
      if (e == std::string::npos) break;
      i = e + 1;
      continue;
    }
    const size_t eq = text.find('=', i);
    if (eq == std::string::npos) break;
    const std::string key = text.substr(i, eq - i);
    size_t ve = eq + 1;
    if (key == "sample" || key == "default_path" || key == "image" || key == "tdfir_impulse") {  // K-144: pictures and impulses travel too
      const size_t next = text.find_first_of("<\n\r", ve);
      ve = next == std::string::npos ? text.size() : next;
      const size_t eq2 = text.find('=', eq + 1);
      if (eq2 != std::string::npos && eq2 < ve) {
        const size_t sp = text.rfind(' ', eq2);
        if (sp != std::string::npos && sp > eq) ve = sp;
      }
      size_t vEnd = ve;
      while (vEnd > eq + 1 && std::isspace(static_cast<unsigned char>(text[vEnd - 1]))) --vEnd;
      const std::string value = slashes(text.substr(eq + 1, vEnd - eq - 1));
      if (key == "default_path") {
        defaultPath = value;
        if (!defaultPath.empty() && defaultPath.back() != '/') defaultPath += '/';
        text.erase(eq + 1, vEnd - eq - 1);  // the members sit beside the map
        ve = eq + 1 + (ve - vEnd);
      } else if (!value.empty() && value[0] != '*') {  // ("*sine", "*noise": generated)
        const fs::path v = fs::u8path(value);
        const std::string src = (v.is_absolute() ? v : dir / fs::u8path(defaultPath + value)).lexically_normal().u8string();
        std::error_code ec;
        std::string member;
        if (!fs::is_regular_file(fs::u8path(src), ec)) {
          out.missing.push_back(src);
        } else if (auto it = nameOf.find(src); it != nameOf.end()) {
          member = it->second;
        } else {
          if (memberName) {
            member = memberName(src);
          } else {
            for (int k = 1;; ++k) {
              member = numberedName(fileNameOf(src), k);
              if (used.insert(lower(member)).second) break;
            }
          }
          nameOf[src] = member;
          out.members.emplace_back(src, member);
        }
        if (!member.empty()) {
          text.replace(eq + 1, vEnd - eq - 1, member);
          ve = eq + 1 + member.size() + (ve - vEnd);
        }
      }
    } else {
      while (ve < text.size() && !std::isspace(static_cast<unsigned char>(text[ve])) && text[ve] != '<') ++ve;
    }
    i = ve;
  }
  out.text = std::move(text);
  out.ok = true;
  return out;
}

std::map<std::string, std::string> sidecarsFor(const std::map<std::string, std::string>& files,
                                               const std::function<std::string(const std::string& fileName)>& freeName) {
  std::map<std::string, std::string> out;
  std::map<std::string, std::string> known = files;
  std::vector<std::pair<std::string, std::string>> queue(files.begin(), files.end());
  auto readAll = [](const std::string& p, std::string& text) {
    std::ifstream f(pathOf(p), std::ios::binary);
    if (!f) return false;
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
  };
  for (size_t q = 0; q < queue.size(); ++q) {
    const std::string src = queue[q].first, target = queue[q].second;
    std::string text;
    if (readAll(src + ".source.json", text)) out[target + ".source.json"] = text;  // where it came from: no paths in it
    if (!readAll(src + ".origin.json", text)) continue;
    Json j;
    if (!jsonParse(text, j) || j.type != Json::Type::Object) {
      out[target + ".origin.json"] = text;
      continue;
    }
    const fs::path fromDir = pathOf(target).parent_path();
    // A source that travels: its new place, relative to this file. One that does not but has an
    // origin of its own: only that sidecar travels, so the chain holds. Any other: its name.
    auto follow = [&](std::string& ref) {
      fs::path abs = pathOf(ref);
      if (abs.empty()) return;
      if (!abs.is_absolute()) abs = pathOf(src).parent_path() / abs;
      const std::string key = abs.lexically_normal().u8string();
      auto it = known.find(key);
      if (it == known.end() && existsFile(key + ".origin.json")) {
        it = known.emplace(key, freeName(fileNameOf(key))).first;
        queue.emplace_back(key, it->second);
      }
      ref = it != known.end() ? pathOf(it->second).lexically_relative(fromDir).generic_u8string() : fileNameOf(key);
    };
    if (auto s = j.object.find("source"); s != j.object.end() && s->second.type == Json::Type::String) follow(s->second.str);
    if (auto a = j.object.find("sources"); a != j.object.end() && a->second.type == Json::Type::Array)
      for (auto& v : a->second.array)
        if (v.type == Json::Type::String) follow(v.str);
    out[target + ".origin.json"] = jsonStringify(j, 2) + "\n";
  }
  return out;
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

std::vector<std::string> missingFiles(const Song& song) { return missingAmong(referencedFiles(song)); }

std::vector<std::string> missingAmong(const std::vector<std::string>& refs) {
  std::vector<std::string> out;
  for (const auto& p : refs)
    if (!existsFile(p)) out.push_back(p);
  return out;
}

int resolveRelativePaths(Song& song, const std::string& baseDir) {
  if (baseDir.empty()) return 0;
  return visitSong(song, [&](std::string& p, RefKind kind) {
    const fs::path rel = pathOf(p);
    if (rel.empty() || rel.is_absolute()) return;
    std::error_code ec;
    const fs::path abs = (pathOf(baseDir) / rel).lexically_normal();
    if (kind == RefKind::File ? fs::is_regular_file(abs, ec) : (fs::is_directory(abs, ec) || (isFolderManifest(p) && fs::is_regular_file(abs, ec)))) p = abs.u8string();
  });
}

void undoCollect(const CollectReport& report, const std::string& folder) {
  const fs::path top = fs::u8path(folder).lexically_normal();
  std::error_code ec;
  for (const auto& f : report.created) {
    fs::path p = fs::u8path(f).lexically_normal();
    fs::remove(p, ec);
    // Folders it leaves empty, up to the top. (Removing a folder that holds anything fails: it stays.)
    for (p = p.parent_path(); p != top && !p.empty() && p != p.parent_path() && p.u8string().rfind(top.u8string(), 0) == 0; p = p.parent_path())
      if (!fs::remove(p, ec)) break;
  }
}

CollectReport collectFiles(Song& song, const std::string& folder, const std::string& relativeTo, const ExportControl* control) {
  CollectReport rep;
  std::error_code ec;
  const fs::path root = fs::u8path(folder);
  fs::create_directories(root, ec);
  if (ec) {
    rep.error = "cannot create " + folder;
    return rep;
  }
  ExportMeter meter(control, kCopyingStatus);
  if (meter.on()) {  // how much there is to copy, for the progress bar
    std::set<std::string> counted;
    visitSong(song, [&](std::string& p, RefKind kind) {
      if (!counted.insert(p).second) return;
      if (kind == RefKind::Folder) {
        if (existsFolder(p))
          for (const auto& [f, member] : folderRefFiles(p)) meter.addTotal(fs::u8path(f));
      } else if (existsFile(p)) {
        if (const SfzBundle sfz = isSfz(p) ? sfzBundle(p) : SfzBundle{}; sfz.ok) {
          for (const auto& [src, member] : sfz.members) meter.addTotal(fs::u8path(src));
        } else {
          meter.addTotal(fs::u8path(p));
        }
      }
    });
  }
  std::set<std::string> missing;
  auto addMissing = [&](const std::string& p) {
    if (!p.empty() && missing.insert(p).second) rep.missing.push_back(p);
  };
  std::map<std::string, std::string> done;  // source → destination (absolute)
  std::map<std::string, std::string> carried;  // every file copied: source → destination
  std::set<std::string> usedNames;
  auto refTo = [&](const std::string& dst) {
    return relativeTo.empty() ? dst : fs::u8path(dst).lexically_relative(fs::u8path(relativeTo)).generic_u8string();
  };
  visitSong(song, [&](std::string& p, RefKind kind) {
    if (!rep.error.empty() || rep.cancelled) return;
    if (kind == RefKind::Folder ? !existsFolder(p) : !existsFile(p)) {
      addMissing(p);
      return;
    }
    auto it = done.find(p);
    if (it == done.end()) {
      fs::path dst;
      if (kind == RefKind::Folder) {
        const auto files = folderRefFiles(p);
        dst = freeFolder(root, folderNameOf(p), usedNames, [&](const fs::path& d) {
          return !files.empty() && fs::exists(d / fs::u8path(files[0].second), ec) && sameContent(d / fs::u8path(files[0].second), fs::u8path(files[0].first));
        });
        for (const auto& [f, name] : files) {
          if (!existsFile(f)) {
            addMissing(f);
          } else if (!copyInto(fs::u8path(f), dst / fs::u8path(name), rep, meter)) {
            return;
          } else {
            carried[f] = (dst / fs::u8path(name)).lexically_normal().u8string();
          }
        }
      } else if (const SfzBundle sfz = isSfz(p) ? sfzBundle(p) : SfzBundle{}; sfz.ok) {
        for (const auto& m : sfz.missing) addMissing(m);
        const std::string name = fileNameOf(p);
        auto holds = [&](const fs::path& d) {
          std::ifstream x(d / fs::u8path(name), std::ios::binary);
          return x && std::string((std::istreambuf_iterator<char>(x)), std::istreambuf_iterator<char>()) == sfz.text;
        };
        const fs::path dir = freeFolder(root, fs::u8path(p).stem().u8string(), usedNames, holds);
        for (const auto& [src, member] : sfz.members) {
          if (!copyInto(fs::u8path(src), dir / fs::u8path(member), rep, meter)) return;
          carried[src] = (dir / fs::u8path(member)).lexically_normal().u8string();
        }
        dst = dir / fs::u8path(name);
        fs::create_directories(dir, ec);
        if (!holds(dir)) {
          if (!fs::exists(dst, ec)) rep.created.push_back(dst.u8string());
          std::ofstream o(dst, std::ios::binary | std::ios::trunc);
          o << sfz.text;
          o.close();
          if (!o) {
            rep.error = "cannot write " + dst.u8string();
            return;
          }
          ++rep.copied;
        } else {
          ++rep.reused;
        }
      } else {
        const fs::path src = fs::u8path(p);
        for (int k = 1;; ++k) {
          dst = root / fs::u8path(numberedName(fileNameOf(p), k));
          const std::string key = lower(dst.u8string());
          if (usedNames.count(key)) continue;
          if (!fs::exists(dst, ec)) break;
          if (fs::equivalent(dst, src, ec) || sameContent(dst, src)) break;
        }
        usedNames.insert(lower(dst.u8string()));
        if (!copyInto(src, dst, rep, meter)) return;
      }
      it = done.emplace(p, dst.lexically_normal().u8string()).first;
      rep.moved[p] = it->second;
      if (kind == RefKind::File) carried[p] = it->second;
    }
    p = refTo(it->second);
  });
  if (!rep.error.empty() || rep.cancelled) {
    if (meter.on()) undoCollect(rep, folder);
    return rep;
  }
  // Sidecars beside their files (a file collected onto itself keeps its own).
  for (auto c = carried.begin(); c != carried.end();)
    c = c->first == c->second ? carried.erase(c) : std::next(c);
  const auto sidecars = sidecarsFor(carried, [&](const std::string& fileName) {
    fs::path to;
    for (int k = 1;; ++k) {
      to = root / fs::u8path(numberedName(fileName, k));
      if (usedNames.insert(lower(to.u8string())).second && !fs::exists(to, ec)) return to.lexically_normal().u8string();
    }
  });
  for (const auto& [to, text] : sidecars) {
    if (!fs::exists(fs::u8path(to), ec)) rep.created.push_back(to);
    std::ofstream o(fs::u8path(to), std::ios::binary | std::ios::trunc);
    o << text;
    o.close();
    if (!o) {
      rep.error = "cannot write " + to;
      if (meter.on()) undoCollect(rep, folder);
      return rep;
    }
  }
  return rep;
}

std::vector<std::string> absolutePathsInProject(const std::string& projectText) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  Json j;
  if (jsonParse(projectText, j)) walkAbsolutePaths(j, seen, out);
  return out;
}

namespace {

// The folder of an export: what a run that does not finish takes away again.
struct ExportFolder {
  fs::path folder;
  bool existed = false;
  explicit ExportFolder(const std::string& path) : folder(fs::u8path(path)) {
    std::error_code ec;
    existed = fs::exists(folder, ec);
  }
  void undo(const CollectReport& rep) const {
    std::error_code ec;
    if (!existed) fs::remove_all(folder, ec);  // this run made it: all of it goes
    else undoCollect(rep, folder.u8string());
  }
};

// `stage`: the project file goes there instead (K-133: project bones move their files into place together).
CollectReport writeDataFiles(const Song& song, const std::string& folder, const std::string& name, const ExportControl* control,
                             const fs::path* stage = nullptr) {
  Song copy = song;
  CollectReport rep = collectFiles(copy, (fs::u8path(folder) / "Samples").u8string(), folder, control);
  if (!rep.error.empty() || rep.cancelled) return rep;
  std::map<std::string, std::string> keys;
  for (const auto& [from, to] : rep.moved) keys[from] = fs::u8path(to).lexically_relative(fs::u8path(folder)).generic_u8string();
  relinkReceipt(copy, keys);
  // Third-party plugins' own references: copied too, and named in the report.
  rep.insidePlugins = insidePluginFiles(copy);
  ExportMeter meter(control, kCopyingStatus);
  for (const auto& p : rep.insidePlugins) meter.addTotal(fs::u8path(p));
  for (const auto& p : rep.insidePlugins) {
    fs::path to;
    std::error_code ec;
    for (int k = 1;; ++k) {
      to = fs::u8path(folder) / "Samples" / "Inside plugins" / fs::u8path(numberedName(fileNameOf(p), k));
      if (!fs::exists(to, ec) || sameContent(to, fs::u8path(p))) break;
    }
    if (!copyInto(fs::u8path(p), to, rep, meter)) return rep;
  }
  if (meter.stopped()) {
    rep.cancelled = true;
    return rep;
  }
  meter.say(0.96, i18n::N_("Writing the project"));
  if (meter.stopped()) {  // (a Cancel during the last step is a Cancel too)
    rep.cancelled = true;
    return rep;
  }
  const fs::path project = (stage ? *stage : fs::u8path(folder)) / fs::u8path(name + kProjectExt);  // K-121: the new name
  std::error_code ec;
  if (stage) fs::create_directories(*stage, ec);
  const bool projectThere = fs::exists(project, ec);
  std::string err;
  if (!saveProject(project.u8string(), copy, &err)) {
    rep.error = err;
  } else {
    if (!projectThere && !stage) rep.created.push_back(project.u8string());
    rep.unpacked = absolutePathsInProject(projectToText(copy));
  }
  return rep;
}

}  // namespace

CollectReport exportProjectDataFiles(const Song& song, const std::string& folder, const std::string& name, const ExportControl* control) {
  const ExportFolder target(folder);
  CollectReport rep = writeDataFiles(song, folder, name, control);
  if (!rep.error.empty() || rep.cancelled) target.undo(rep);
  return rep;
}

// K-133: the files of project bones that replace an earlier export's (the project, MIDI files,
// presets, effect chains) are written into a staging folder first, and moved into place together
// once every one of them was written. A run that fails or is cancelled replaces nothing.
namespace {
constexpr const char* kBonesStage = ".gridline-bones-staging";

// Moves every file under `stage` to the same place under `folder`. False (with the error) when one
// cannot be moved.
bool commitStage(const fs::path& stage, const fs::path& folder, CollectReport& rep) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(stage, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path());
  if (ec) {
    rep.error = "cannot read the written files in " + stage.u8string();
    return false;
  }
  std::sort(files.begin(), files.end());
  // First every place is checked (a folder where a file goes, a folder that cannot be made), so a
  // move that cannot happen is found before anything was moved.
  for (const fs::path& f : files) {
    const fs::path to = folder / f.lexically_relative(stage);
    fs::create_directories(to.parent_path(), ec);
    if (!fs::is_directory(to.parent_path(), ec) || (fs::exists(to, ec) && !fs::is_regular_file(to, ec))) {
      rep.error = "cannot write " + to.u8string() + ": something else is in its place";
      return false;
    }
  }
  for (const fs::path& f : files) {
    const fs::path to = folder / f.lexically_relative(stage);
    const bool there = fs::exists(to, ec);
    std::string err;
    if (!platform::replaceFile(f.u8string(), to.u8string(), &err)) {
      rep.error = err.empty() ? "cannot move " + to.u8string() + " into place" : err;
      return false;
    }
    if (!there) rep.created.push_back(to.u8string());
  }
  fs::remove_all(stage, ec);
  return true;
}
}  // namespace

BonesReport exportProjectBones(const Song& song, const std::string& folder, const std::string& name, const ExportControl* control) {
  BonesReport rep;
  const ExportFolder target(folder);
  const fs::path midiDir = fs::u8path(folder) / "MIDI", presetDir = fs::u8path(folder) / "Presets";
  const fs::path stage = fs::u8path(folder) / kBonesStage, stageMidi = stage / "MIDI", stagePresets = stage / "Presets";
  std::error_code ec;
  fs::remove_all(stage, ec);  // (what a run that was killed left)
  const bool midiThere = fs::exists(midiDir, ec), presetsThere = fs::exists(presetDir, ec);
  rep.files = writeDataFiles(song, folder, name, control, &stage);
  // A run that does not finish: what it added goes, and nothing of an earlier export in the same
  // place was replaced (what would replace it is still in the staging folder).
  auto stop = [&](const std::string& why) {
    rep.files.error = why;
    fs::remove_all(stage, ec);
    if (!midiThere) fs::remove_all(midiDir, ec);
    if (!presetsThere) fs::remove_all(presetDir, ec);
    target.undo(rep.files);
    return rep;
  };
  if (!rep.files.error.empty() || rep.files.cancelled) return stop(rep.files.error);
  const ExportMeter meter(control, "");
  auto stopped = [&] {
    if (!meter.stopped()) return false;
    rep.files.cancelled = true;
    return true;
  };
  meter.say(0.97, i18n::N_("Writing MIDI files and presets"));
  // A file where a folder goes: the move at the end could not put anything there.
  for (const fs::path& d : {midiDir, presetDir})
    if (fs::exists(d, ec) && !fs::is_directory(d, ec)) return stop("cannot write into " + d.u8string() + ": a file is in the way");
  fs::create_directories(stageMidi, ec);
  auto writeMidi = [&](const std::string& fileName, const MidiFileData& m) {
    std::string safe = fileName;
    for (char& c : safe)
      if (c == '/' || c == '\\' || c == ':') c = '_';
    return writeMidiFile((stageMidi / fs::u8path(safe)).u8string(), m);
  };
  int k = 0;
  for (const auto& p : song.patterns) {
    ++k;
    if (stopped()) return stop("");
    MidiFileData m = patternToMidi(song, p.id);
    if (m.tracks.empty()) continue;
    char num[32];
    std::snprintf(num, sizeof num, "%02d ", k);
    if (!writeMidi(num + p.name + ".mid", m)) return stop("cannot write the MIDI file of " + p.name + " in " + midiDir.u8string());
    ++rep.midiFiles;
  }
  if (!song.playlist.empty()) {
    MidiFileData m = songToMidi(song);
    if (!m.tracks.empty()) {
      if (!writeMidi(name + " (song).mid", m)) return stop("cannot write the song's MIDI file in " + midiDir.u8string());
      ++rep.midiFiles;
    }
  }
  // Instrument states and effect chains, with references to the collected files.
  Song copy = song;
  relinkFiles(copy, rep.files.moved);
  const std::string presets = stagePresets.u8string();
  for (const auto& c : copy.channels) {
    if (stopped()) return stop("");
    if (c.plugin.type.empty()) continue;
    if (!savePreset(presets, c.plugin, c.name, "project bones: " + name)) return stop("cannot write the preset of " + c.name + " in " + presetDir.u8string());
    ++rep.presets;
  }
  for (size_t i = 0; i < copy.mixer.size(); ++i) {
    if (stopped()) return stop("");
    const auto& m = copy.mixer[i];
    ChainPreset chain;
    bool any = false;
    for (const auto& sl : m.slots) {
      chain.slots.push_back(sl.plugin);
      chain.mix.push_back(sl.mix);
      any = any || !sl.plugin.type.empty();
    }
    if (!any) continue;
    if (!saveChain(presets, chain, mixerTrackName(copy, i))) return stop("cannot write the effect chain of " + mixerTrackName(copy, i) + " in " + presetDir.u8string());
    ++rep.chains;
  }
  if (stopped()) return stop("");
  // Every file was written: now they replace the earlier export's.
  if (!commitStage(stage, fs::u8path(folder), rep.files)) return stop(rep.files.error);
  fs::create_directories(midiDir, ec);  // (a project without notes still gets its folder, as before)
  return rep;
}

CollectReport exportZippedProject(const Song& song, const std::string& zipPath, const std::string& name, const ExportControl* control) {
  CollectReport rep;
  Song copy = song;
  std::map<std::string, std::string> entry;  // source → archive name (a file, a folder or a map)
  std::vector<std::pair<std::string, std::string>> extra;  // more files to pack: source → archive name
  std::vector<std::pair<std::string, std::string>> generated;  // archive name → text
  std::set<std::string> names;
  std::set<std::string> missing;
  auto addMissing = [&](const std::string& p) {
    if (!p.empty() && missing.insert(p).second) rep.missing.push_back(p);
  };
  // A name under Samples/ that nothing else has. Case-insensitive: the archive may be unpacked on a
  // file system that does not tell names apart by case.
  auto uniqueEntry = [&](const std::string& fileName) {
    std::string n;
    for (int k = 1;; ++k) {
      n = "Samples/" + numberedName(fileName, k);
      if (names.insert(lower(n)).second) return n;
    }
  };
  visitSong(copy, [&](std::string& p, RefKind kind) {
    if (kind == RefKind::Folder ? !existsFolder(p) : !existsFile(p)) {
      addMissing(p);
      return;
    }
    auto it = entry.find(p);
    if (it == entry.end()) {
      std::string n;
      if (kind == RefKind::Folder) {
        n = uniqueEntry(folderNameOf(p));
        for (const auto& [f, name] : folderRefFiles(p)) {
          if (existsFile(f)) extra.emplace_back(f, n + "/" + name);
          else addMissing(f);
        }
      } else if (const SfzBundle sfz = isSfz(p) ? sfzBundle(p) : SfzBundle{}; sfz.ok) {
        // The map and its samples in a folder of their own.
        const std::string dir = uniqueEntry(fs::u8path(p).stem().u8string());
        for (const auto& m : sfz.missing) addMissing(m);
        for (const auto& [src, member] : sfz.members) extra.emplace_back(src, dir + "/" + member);
        n = dir + "/" + fileNameOf(p);
        generated.emplace_back(n, sfz.text);
      } else {
        n = uniqueEntry(fileNameOf(p));
        extra.emplace_back(p, n);
      }
      it = entry.emplace(p, n).first;
      rep.moved[p] = n;
    }
    p = it->second;
  });
  relinkReceipt(copy, rep.moved);
  // Third-party plugins' own references: packed too, and named in the report.
  rep.insidePlugins = insidePluginFiles(copy);
  for (const auto& p : rep.insidePlugins) {
    std::string n;
    for (int k = 1;; ++k) {
      n = "Samples/Inside plugins/" + numberedName(fileNameOf(p), k);
      if (names.insert(lower(n)).second) break;
    }
    extra.emplace_back(p, n);
  }
  // Sidecars beside their files.
  {
    std::map<std::string, std::string> carried;
    for (const auto& [src, n] : extra) carried.emplace(src, n);
    for (const auto& [n, t] : sidecarsFor(carried, uniqueEntry)) generated.emplace_back(n, t);
  }
  ExportMeter meter(control, i18n::N_("Packing the project's files"));
  for (const auto& [src, n] : extra) meter.addTotal(fs::u8path(src));
  ZipWriter z;
  if (meter.on())
    z.setChunkHook([&meter](uint64_t bytes) {  // (a stopped writer never replaces the target)
      if (meter.stopped()) return false;
      meter.advance(bytes);
      return true;
    });
  std::string err;
  if (!z.open(zipPath, &err)) {
    rep.error = err;
    return rep;
  }
  const std::string text = projectToText(copy);
  if (!z.add(projectFileNameInZip(zipPath, name), std::vector<uint8_t>(text.begin(), text.end()))) {  // K-121
    rep.error = z.close(&err) ? "cannot write " + zipPath : err;  // J-78: a failed entry is reported (the target stays as it was)
    return rep;
  }
  for (const auto& [n, t] : generated) {
    if (!z.add(n, std::vector<uint8_t>(t.begin(), t.end()))) {
      rep.error = z.close(&err) ? "cannot write " + zipPath : err;
      return rep;
    }
    ++rep.copied;
  }
  for (const auto& [src, n] : extra) {
    if (meter.stopped() || !z.addFile(n, src, &err)) {
      if (meter.stopped()) rep.cancelled = true;
      else rep.error = err;
      return rep;
    }
    ++rep.copied;
  }
  if (meter.stopped()) {  // (the temporary archive goes with the writer)
    rep.cancelled = true;
    return rep;
  }
  meter.say(0.98, i18n::N_("Finishing the archive"));
  if (meter.stopped()) {  // (a Cancel during the last step is a Cancel too)
    rep.cancelled = true;
    return rep;
  }
  if (!z.close(&err)) rep.error = err;
  // Nothing may stay behind unnoticed: whatever the walk above does not know is named here.
  rep.unpacked = absolutePathsInProject(text);
  return rep;
}

bool unpackZippedProject(const std::string& zipPath, const std::string& dir, std::string& projectFile, std::string* err) {
  std::string oe;
  auto src = remotefs::openFile(zipPath, &oe);
  if (!src) {
    if (err) *err = "cannot open " + zipPath;
    return false;
  }
  std::vector<ZipEntry> all, entries;
  if (!readZipDirectory(*src, all, err)) return false;
  for (auto& e : all)
    if (!archive::isJunkEntry(e.name)) entries.push_back(std::move(e));
  std::vector<std::string> files;
  if (!extractZipEntries(*src, entries, dir, &files, {}, err)) return false;
  projectFile.clear();
  const archive::ProjectPick pick = archive::pickProject(entries, projectStemOf(zipPath));
  if (pick.names.empty()) {
    if (err) *err = "the archive holds no project file";
    return false;
  }
  std::error_code ec;
  projectFile = (fs::weakly_canonical(fs::u8path(dir), ec) / fs::u8path(pick.names.front())).u8string();
  return true;
}

RelinkReport findReplacements(const std::vector<std::string>& missing, const std::string& folder, const RelinkOptions& o) {
  RelinkReport rep;
  // Index: lower-case file name → paths; lower-case stem → paths.
  std::map<std::string, std::vector<fs::path>> byName, byStem;
  std::error_code ec;
  const bool recursive = o.recursive && !remotefs::isRemote(folder);  // K-42: a web drive is never walked
  rep.shallow = o.recursive && !recursive;
  auto add = [&](const fs::path& p) {
    byName[lower(p.filename().string())].push_back(p);
    byStem[lower(p.stem().string())].push_back(p);
    ++rep.indexed;
  };
  if (recursive) {
    remotefs::countWalk();
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && rep.indexed < o.maxFiles && !(o.cancel && o.cancel->load(std::memory_order_relaxed)); it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2)) add(it->path());
    }
  } else {
    remotefs::countWalk();
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
  return visitSong(song, [&](std::string& p, RefKind) {
    auto it = map.find(p);
    if (it != map.end()) p = it->second;
  });
}

}  // namespace gl
