// Gridline: drum kits in the drumkit.xml / .h2drumkit format (see HydrogenKit.h). Own reader,
// written from the format's published schema; no code of the format's own player is used.
#include "HydrogenKit.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include "../engine/Slicing.h"
#include "I18n.h"
#include "PluginParams.h"
#include "Zip.h"

namespace gl::hydrogen {

namespace fs = std::filesystem;
namespace pp = gl::pp;

namespace {

// ------------------------------------------------------------------ a small XML tree
// Elements, their text and children; attributes are not needed by the kit format and are skipped.
// Namespace prefixes are dropped from names.
struct Node {
  std::string name, text;
  std::vector<std::unique_ptr<Node>> kids;
  const Node* child(const char* n) const {
    for (const auto& k : kids)
      if (k->name == n) return k.get();
    return nullptr;
  }
  std::vector<const Node*> all(const char* n) const {
    std::vector<const Node*> out;
    for (const auto& k : kids)
      if (k->name == n) out.push_back(k.get());
    return out;
  }
};

void appendUtf8(std::string& s, unsigned long cp) {
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Character data with its entities resolved.
std::string unescape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out += in[i];
      continue;
    }
    const size_t semi = in.find(';', i);
    if (semi == std::string::npos || semi - i > 12) {
      out += in[i];
      continue;
    }
    const std::string e = in.substr(i + 1, semi - i - 1);
    if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "amp") out += '&';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else if (e.size() > 1 && e[0] == '#') {
      const bool hex = e[1] == 'x' || e[1] == 'X';
      const unsigned long cp = std::strtoul(e.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
      appendUtf8(out, cp);
    } else {
      out += in.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

class XmlReader {
 public:
  explicit XmlReader(const std::string& s) : s_(s) {}
  std::unique_ptr<Node> root(std::string* err) {
    skipMisc();
    if (p_ >= s_.size() || s_[p_] != '<') return fail(err, "no element");
    auto n = element(0);
    if (!n) return fail(err, error_.empty() ? "broken XML" : error_);
    return n;
  }

 private:
  const std::string& s_;
  size_t p_ = 0;
  std::string error_;

  std::unique_ptr<Node> fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return nullptr;
  }
  bool starts(const char* t) const { return s_.compare(p_, std::strlen(t), t) == 0; }
  bool skipPast(const char* t) {
    const size_t e = s_.find(t, p_);
    if (e == std::string::npos) return false;
    p_ = e + std::strlen(t);
    return true;
  }
  // Declarations, comments, processing instructions, a DOCTYPE and blanks before the root.
  void skipMisc() {
    for (;;) {
      while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
      if (p_ + 2 < s_.size() && s_[p_] == '\xEF' && s_[p_ + 1] == '\xBB' && s_[p_ + 2] == '\xBF') {
        p_ += 3;  // a byte order mark
        continue;
      }
      if (starts("<?")) {
        if (!skipPast("?>")) p_ = s_.size();
      } else if (starts("<!--")) {
        if (!skipPast("-->")) p_ = s_.size();
      } else if (starts("<!")) {
        if (!skipPast(">")) p_ = s_.size();
      } else {
        return;
      }
    }
  }
  static std::string localName(const std::string& n) {
    const size_t c = n.rfind(':');
    return c == std::string::npos ? n : n.substr(c + 1);
  }
  std::unique_ptr<Node> element(int depth) {
    if (depth > 64) {
      error_ = "the XML nests too deeply";
      return nullptr;
    }
    ++p_;  // '<'
    const size_t nameStart = p_;
    while (p_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[p_])) && s_[p_] != '>' && s_[p_] != '/') ++p_;
    auto n = std::make_unique<Node>();
    n->name = localName(s_.substr(nameStart, p_ - nameStart));
    // Attributes: skipped, quotes respected.
    char quote = 0;
    while (p_ < s_.size()) {
      const char c = s_[p_];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>' || c == '/') {
        break;
      }
      ++p_;
    }
    if (p_ >= s_.size()) return nullptr;
    if (s_[p_] == '/') {  // <empty/>
      if (!skipPast(">")) return nullptr;
      return n;
    }
    ++p_;  // '>'
    std::string raw;
    for (;;) {
      if (p_ >= s_.size()) {
        error_ = "the XML ends inside <" + n->name + ">";
        return nullptr;
      }
      if (s_[p_] != '<') {
        const size_t e = s_.find('<', p_);
        raw += s_.substr(p_, e == std::string::npos ? std::string::npos : e - p_);
        p_ = e == std::string::npos ? s_.size() : e;
        continue;
      }
      if (starts("<!--")) {
        if (!skipPast("-->")) return nullptr;
      } else if (starts("<![CDATA[")) {
        p_ += 9;
        const size_t e = s_.find("]]>", p_);
        if (e == std::string::npos) return nullptr;
        n->text += unescape(raw);
        raw.clear();
        n->text += s_.substr(p_, e - p_);
        p_ = e + 3;
      } else if (starts("<?")) {
        if (!skipPast("?>")) return nullptr;
      } else if (starts("</")) {
        if (!skipPast(">")) return nullptr;
        n->text += unescape(raw);
        return n;
      } else {
        auto k = element(depth + 1);
        if (!k) return nullptr;
        n->kids.push_back(std::move(k));
      }
    }
  }
};

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string textOf(const Node& n, const char* key, const std::string& def = "") {
  const Node* c = n.child(key);
  return c ? trim(c->text) : def;
}
float floatOf(const Node& n, const char* key, float def) {
  const Node* c = n.child(key);
  if (!c) return def;
  const std::string t = trim(c->text);
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  return end != t.c_str() && std::isfinite(v) ? static_cast<float>(v) : def;
}
int intOf(const Node& n, const char* key, int def) {
  const Node* c = n.child(key);
  if (!c) return def;
  const std::string t = trim(c->text);
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  return end != t.c_str() && std::isfinite(v) ? static_cast<int>(std::clamp(v, -2e9, 2e9)) : def;
}
bool boolOf(const Node& n, const char* key, bool def) {
  const Node* c = n.child(key);
  if (!c) return def;
  std::string t = trim(c->text);
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (t == "true" || t == "1" || t == "yes") return true;
  if (t == "false" || t == "0" || t == "no") return false;
  return def;
}

Select selectOf(const std::string& t, Select def) {
  if (t == "VELOCITY") return Select::Velocity;
  if (t == "ROUND_ROBIN") return Select::RoundRobin;
  if (t == "RANDOM") return Select::Random;
  return def;
}

Layer readLayer(const Node& l) {
  Layer o;
  o.file = textOf(l, "filename");
  o.min = floatOf(l, "min", 0);
  o.max = floatOf(l, "max", 1);
  o.gain = floatOf(l, "gain", 1);
  o.pitch = floatOf(l, "pitch", 0);
  o.muted = boolOf(l, "isMuted", false);
  return o;
}

// The pan a pair of side levels gives: the louder side stays at 1 and the other is turned down.
float panFromSides(float l, float r) {
  l = std::clamp(l, 0.0f, 1.0f), r = std::clamp(r, 0.0f, 1.0f);
  if (l <= 0 && r <= 0) return 0;
  if (l >= r) return r / l - 1.0f;
  return 1.0f - l / r;
}

Instrument readInstrument(const Node& n, int index) {
  Instrument in;
  in.id = intOf(n, "id", index);
  in.name = textOf(n, "name");
  if (in.name.empty()) in.name = "Instrument " + std::to_string(index + 1);
  in.volume = floatOf(n, "volume", 1);
  in.gain = floatOf(n, "gain", 1);
  in.muted = boolOf(n, "isMuted", false);
  in.soloed = boolOf(n, "isSoloed", false);
  if (n.child("pan")) in.pan = std::clamp(floatOf(n, "pan", 0), -1.0f, 1.0f);
  else if (n.child("pan_L") && n.child("pan_R")) in.pan = panFromSides(floatOf(n, "pan_L", 1), floatOf(n, "pan_R", 1));
  in.pitch = floatOf(n, "pitchOffset", 0);
  in.randomPitch = std::clamp(floatOf(n, "randomPitchFactor", 0), 0.0f, 1.0f);
  in.applyVelocity = boolOf(n, "applyVelocity", true);
  in.stopNote = boolOf(n, "isStopNote", boolOf(n, "stop_note", false));
  in.filterActive = boolOf(n, "filterActive", false);
  in.cutoff = std::clamp(floatOf(n, "filterCutoff", 1), 0.0f, 1.0f);
  in.resonance = std::clamp(floatOf(n, "filterResonance", 0), 0.0f, 1.0f);
  in.attack = std::max(0, intOf(n, "Attack", 0));
  in.decay = std::max(0, intOf(n, "Decay", 0));
  in.sustain = std::clamp(floatOf(n, "Sustain", 1), 0.0f, 1.0f);
  in.release = std::max(0, intOf(n, "Release", 1000));
  in.muteGroup = intOf(n, "muteGroup", -1);
  in.midiOutNote = intOf(n, "midiOutNote", -1);
  const Select instSelect = selectOf(textOf(n, "sampleSelectionAlgo"), Select::Velocity);
  const auto comps = n.all("instrumentComponent");
  if (!comps.empty()) {
    for (const Node* c : comps) {
      Component co;
      co.id = intOf(*c, "component_id", static_cast<int>(in.components.size()));
      co.name = textOf(*c, "name");
      co.gain = floatOf(*c, "gain", 1);
      co.muted = boolOf(*c, "isMuted", false);
      co.select = selectOf(textOf(*c, "sampleSelectionAlgo"), instSelect);
      for (const Node* l : c->all("layer")) co.layers.push_back(readLayer(*l));
      in.components.push_back(std::move(co));
    }
  } else {
    Component co;  // 0.9: layers inside the instrument; older: one file named on it
    co.select = instSelect;
    for (const Node* l : n.all("layer")) co.layers.push_back(readLayer(*l));
    if (co.layers.empty() && n.child("filename")) {
      Layer l;
      l.file = textOf(n, "filename");
      co.layers.push_back(l);
    }
    in.components.push_back(std::move(co));
  }
  return in;
}

std::string lowerStr(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// ------------------------------------------------------------------ tar
uint64_t tarNumber(const uint8_t* f, size_t n) {
  if (n > 0 && (f[0] & 0x80)) {  // base-256 (GNU, large sizes)
    uint64_t v = f[0] & 0x7F;
    for (size_t i = 1; i < n; ++i) v = (v << 8) | f[i];
    return v;
  }
  uint64_t v = 0;
  size_t i = 0;
  while (i < n && (f[i] == ' ' || f[i] == 0)) ++i;
  for (; i < n && f[i] >= '0' && f[i] <= '7'; ++i) v = v * 8 + static_cast<uint64_t>(f[i] - '0');
  return v;
}

std::string tarString(const uint8_t* f, size_t n) {
  size_t k = 0;
  while (k < n && f[k]) ++k;
  return std::string(reinterpret_cast<const char*>(f), k);
}

// A name that stays inside the folder: relative, no "..", no drive or root.
bool safeName(const std::string& name) {
  if (name.empty() || name[0] == '/' || name[0] == '\\') return false;
  if (name.size() > 1 && name[1] == ':') return false;
  size_t st = 0;
  while (st <= name.size()) {
    size_t e = name.find_first_of("/\\", st);
    const std::string part = name.substr(st, e == std::string::npos ? std::string::npos : e - st);
    if (part == "..") return false;
    if (e == std::string::npos) break;
    st = e + 1;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------ the kit file

bool parseDrumkitXml(const std::string& xml, Kit& out, std::string* err) {
  out = Kit{};
  XmlReader r(xml);
  std::string e;
  auto root = r.root(&e);
  if (!root) {
    if (err) *err = e;
    return false;
  }
  if (root->name != "drumkit_info") {
    if (err) *err = "not a drum kit file";
    return false;
  }
  const Node& k = *root;
  out.formatVersion = intOf(k, "formatVersion", 0);
  out.name = textOf(k, "name");
  out.author = textOf(k, "author");
  out.info = textOf(k, "info");
  out.license = textOf(k, "license");
  out.image = textOf(k, "image");
  out.imageLicense = textOf(k, "imageLicense");
  if (const Node* tags = k.child("tags"))
    for (const Node* t : tags->all("tag"))
      if (!trim(t->text).empty()) out.tags.push_back(trim(t->text));
  if (const Node* cl = k.child("componentList"))
    for (const Node* c : cl->all("drumkitComponent")) out.kitComponents.push_back({intOf(*c, "id", 0), textOf(*c, "name"), floatOf(*c, "volume", 1)});
  const Node* list = k.child("instrumentList");
  if (!list) {
    if (err) *err = "the kit lists no instruments";
    return false;
  }
  for (const Node* n : list->all("instrument")) out.instruments.push_back(readInstrument(*n, static_cast<int>(out.instruments.size())));
  if (out.instruments.empty()) {
    if (err) *err = "the kit lists no instruments";
    return false;
  }
  return true;
}

std::string kitXmlIn(const std::string& folderOrFile) {
  std::error_code ec;
  const fs::path p = fs::u8path(folderOrFile);
  if (fs::is_regular_file(p, ec)) return lowerStr(p.filename().u8string()) == "drumkit.xml" ? p.u8string() : std::string();
  if (!fs::is_directory(p, ec)) return {};
  if (fs::is_regular_file(p / "drumkit.xml", ec)) return (p / "drumkit.xml").u8string();
  // An unpacked archive: the kit is in the one folder inside.
  fs::path only;
  int dirs = 0;
  for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_directory(e2) && it->path().filename().u8string().rfind('.', 0) != 0) only = it->path(), ++dirs;
  }
  if (dirs == 1 && fs::is_regular_file(only / "drumkit.xml", ec)) return (only / "drumkit.xml").u8string();
  return {};
}

bool isKitFolder(const std::string& dir) {
  std::error_code ec;
  return fs::is_regular_file(fs::u8path(dir) / "drumkit.xml", ec);
}

bool readKit(const std::string& folderOrFile, Kit& out, std::string* err) {
  const std::string xml = kitXmlIn(folderOrFile);
  if (xml.empty()) {
    if (err) *err = "no drumkit.xml in this folder";
    return false;
  }
  std::ifstream in(fs::u8path(xml), std::ios::binary);
  if (!in) {
    if (err) *err = "cannot read " + xml;
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!parseDrumkitXml(text, out, err)) return false;
  out.folder = fs::u8path(xml).parent_path().u8string();
  return true;
}

std::string samplePath(const Kit& kit, const std::string& file) {
  if (file.empty()) return {};
  std::string f = file;
  std::replace(f.begin(), f.end(), '\\', '/');
  fs::path p = fs::u8path(f);
  std::error_code ec;
  if (!p.is_absolute() && !kit.folder.empty()) p = fs::u8path(kit.folder) / p;
  if (fs::is_regular_file(p, ec)) return p.u8string();
  // Not there: the same name in the kit folder (absolute paths of another computer), in any case,
  // then as .flac.
  const fs::path dir = kit.folder.empty() ? p.parent_path() : fs::u8path(kit.folder);
  const std::string want = lowerStr(fs::u8path(f).filename().u8string());
  const std::string wantFlac = lowerStr(fs::u8path(f).stem().u8string()) + ".flac";
  fs::path flac;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = lowerStr(it->path().filename().u8string());
    if (n == want) return it->path().u8string();
    if (n == wantFlac) flac = it->path();
  }
  if (!flac.empty()) return flac.u8string();
  return p.u8string();
}

// ------------------------------------------------------------------ the packed kit

bool isKitArchive(const std::string& path) { return lowerStr(fs::u8path(path).extension().u8string()) == ".h2drumkit"; }

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* err) {
  out.clear();
  if (in.size() < 2 || in[0] != 0x1F || in[1] != 0x8B) {
    out = in;  // a plain tar
    return true;
  }
  auto fail = [err](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (in.size() < 18 || in[2] != 8) return fail("unknown compression");
  const uint8_t flags = in[3];
  size_t p = 10;
  if (flags & 4) {  // extra field
    if (p + 2 > in.size()) return fail("damaged archive");
    p += 2 + (in[p] | (in[p + 1] << 8));
  }
  if (flags & 8)  // file name
    while (p < in.size() && in[p++]) {
    }
  if (flags & 16)  // comment
    while (p < in.size() && in[p++]) {
    }
  if (flags & 2) p += 2;  // header CRC
  if (p + 8 > in.size()) return fail("damaged archive");
  const size_t tail = in.size() - 8;
  const uint32_t crc = in[tail] | (in[tail + 1] << 8) | (in[tail + 2] << 16) | (static_cast<uint32_t>(in[tail + 3]) << 24);
  const uint32_t isize = in[tail + 4] | (in[tail + 5] << 8) | (in[tail + 6] << 16) | (static_cast<uint32_t>(in[tail + 7]) << 24);
  if (!inflateRaw(in.data() + p, tail - p, out, isize)) return fail("damaged archive");
  if (static_cast<uint32_t>(out.size()) != isize || crc32(out.data(), out.size()) != crc) return fail("damaged archive (checksum)");
  return true;
}

bool listTar(const std::vector<uint8_t>& tar, std::vector<TarEntry>& out, std::string* err) {
  out.clear();
  size_t p = 0;
  std::string longName;
  while (p + 512 <= tar.size()) {
    const uint8_t* h = tar.data() + p;
    bool zero = true;
    for (int i = 0; i < 512 && zero; ++i) zero = h[i] == 0;
    if (zero) break;  // end of archive
    // Header checksum: the bytes summed with the checksum field read as blanks.
    uint64_t sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? 32u : h[i];
    if (sum != tarNumber(h + 148, 8)) {
      if (err) *err = "not a kit archive";
      return false;
    }
    const uint64_t size = tarNumber(h + 124, 12);
    const char type = static_cast<char>(h[156]);
    const size_t data = p + 512;
    if (size > tar.size() - data) {
      if (err) *err = "the archive is cut short";
      return false;
    }
    std::string name = tarString(h, 100);
    if (std::memcmp(h + 257, "ustar", 5) == 0) {
      const std::string prefix = tarString(h + 345, 155);
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (!longName.empty()) name = longName, longName.clear();
    if (type == 'L') {  // the next entry's long name
      longName = tarString(tar.data() + data, static_cast<size_t>(size));
    } else if (type == 'x') {  // pax records: "<len> path=<name>\n"
      const std::string rec(reinterpret_cast<const char*>(tar.data() + data), static_cast<size_t>(size));
      size_t q = 0;
      while (q < rec.size()) {
        const size_t sp = rec.find(' ', q);
        const long len = std::atol(rec.c_str() + q);
        if (sp == std::string::npos || len <= 0 || q + static_cast<size_t>(len) > rec.size()) break;
        const std::string kv = rec.substr(sp + 1, q + static_cast<size_t>(len) - sp - 2);
        if (kv.rfind("path=", 0) == 0) longName = kv.substr(5);
        q += static_cast<size_t>(len);
      }
    } else if (type == '0' || type == 0 || type == '7' || type == '5') {
      while (name.rfind("./", 0) == 0) name.erase(0, 2);
      const bool dir = type == '5' || (!name.empty() && name.back() == '/');
      while (!name.empty() && name.back() == '/') name.pop_back();
      if (!name.empty()) out.push_back({name, static_cast<uint64_t>(data), size, dir});
    }
    p = data + static_cast<size_t>((size + 511) / 512 * 512);
  }
  return true;
}

bool unpackKit(const std::vector<uint8_t>& archive, const std::string& folder, std::vector<std::string>& written, std::string* err) {
  std::vector<uint8_t> tar;
  if (!gunzip(archive, tar, err)) return false;
  std::vector<TarEntry> entries;
  if (!listTar(tar, entries, err)) return false;
  if (entries.empty()) {
    if (err) *err = "the archive is empty";
    return false;
  }
  for (const auto& e : entries)
    if (!safeName(e.name)) {
      if (err) *err = "the archive names a file outside its folder: " + e.name;
      return false;
    }
  const fs::path root = fs::u8path(folder);
  std::error_code ec;
  fs::create_directories(root, ec);
  for (const auto& e : entries) {
    const fs::path target = root / fs::u8path(e.name);
    if (e.dir) {
      fs::create_directories(target, ec);
      continue;
    }
    fs::create_directories(target.parent_path(), ec);
    std::ofstream o(target, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(tar.data() + e.offset), static_cast<std::streamsize>(e.size));
    if (!o) {
      if (err) *err = "cannot write " + target.u8string();
      return false;
    }
    written.push_back(e.name);
  }
  return true;
}

// ------------------------------------------------------------------ the kit as Abacus channels

namespace {

struct PadLayer {
  std::string path;
  int lo = 0, hi = 127;
  float level = 1, tune = 0;
};

// The velocities 1..127 of a component's layers. Choosing by velocity, the first layer whose range holds
// a velocity plays it (ranges that touch would otherwise both play); round robin and random choose among
// every layer whose range holds it, so those keep their ranges.
void velocityRanges(const Component& c, std::vector<std::pair<int, int>>& out) {
  out.assign(c.layers.size(), {1, 0});
  const float eps = 1e-4f;
  if (c.select != Select::Velocity) {
    for (size_t i = 0; i < c.layers.size(); ++i) {
      const Layer& l = c.layers[i];
      int lo = static_cast<int>(std::ceil(std::min(l.min, l.max) * 127.0f - eps));
      int hi = static_cast<int>(std::floor(std::max(l.min, l.max) * 127.0f + eps));
      lo = std::clamp(lo, 0, 127), hi = std::clamp(hi, 0, 127);
      if (lo > hi) lo = hi = std::clamp(static_cast<int>(std::lround(l.min * 127.0f)), 0, 127);
      if (lo <= 1) lo = 0;  // the softest hits too
      out[i] = {lo, hi};
    }
    return;
  }
  for (int v = 1; v <= 127; ++v) {
    const float x = static_cast<float>(v) / 127.0f;
    for (size_t i = 0; i < c.layers.size(); ++i) {
      const Layer& l = c.layers[i];
      if (l.muted || l.file.empty()) continue;
      if (x + eps >= std::min(l.min, l.max) && x - eps <= std::max(l.min, l.max)) {
        auto& r = out[i];
        if (r.first > r.second) r = {v, v};
        else r.second = v;
        break;
      }
    }
  }
  for (auto& r : out)
    if (r.first == 1) r.first = 0;  // velocity 0 with the softest layer
}

float kitComponentVolume(const Kit& kit, int id) {
  for (const auto& c : kit.kitComponents)
    if (c.id == id) return c.volume;
  return 1;
}

std::string joinNames(const std::vector<std::string>& v, size_t most = 6) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < most; ++i) s += (i ? ", " : "") + v[i];
  if (v.size() > most) s += ", +" + std::to_string(v.size() - most);
  return s;
}

void setParam(PluginState& st, int idx, float v) {
  if (idx >= 0 && static_cast<size_t>(idx) < st.params.size()) st.params[static_cast<size_t>(idx)] = v;
}

}  // namespace

PadsImport kitToPads(const Kit& kit) {
  PadsImport r;
  const int total = static_cast<int>(kit.instruments.size());
  const int perChannel = pp::kPadsAll;
  const int usable = std::min(total, perChannel * kMaxChannels);
  r.instruments = usable;
  if (total > usable)
    r.notes.push_back({i18n::N_("Only the first %s instruments were added (the keyboard ends there)"), std::to_string(usable) + " / " + std::to_string(total)});
  // Mute groups: each group the kit uses gets a cut group (eight per channel).
  std::vector<std::string> clipped, filtered, randomPitch, tooManyLayers, rrMixed, groupsLost;
  std::set<std::string> missing;
  const std::string kitName = kit.name.empty() ? std::string("Drum kit") : kit.name;
  for (int ch = 0; ch * perChannel < usable; ++ch) {
    PluginState st = defaultPluginState("gl.pads");
    st.data.clear();
    setParam(st, pp::PadsBaseNote, static_cast<float>(kFirstNote + ch * perChannel));
    setParam(st, pp::PadsVelocity, 1.0f);  // the velocity scales the level fully, as in the kit's player
    std::map<int, int> groupOf;
    for (int k = 0; k < perChannel; ++k) {
      const int idx = ch * perChannel + k;
      if (idx >= usable) {
        if (k < pp::kPads) st.data = setPadSamplePath(st.data, k, 0, "none");
        setParam(st, pp::padL(k, 0, pp::LayEnabled), 0);
        continue;
      }
      const Instrument& in = kit.instruments[static_cast<size_t>(idx)];
      // Layers of every component, each with its velocities.
      std::vector<PadLayer> layers;
      Select select = Select::Velocity;
      bool selectSet = false, mixed = false;
      for (const Component& c : in.components) {
        if (c.muted) continue;
        std::vector<std::pair<int, int>> ranges;
        velocityRanges(c, ranges);
        const float compGain = c.gain * kitComponentVolume(kit, c.id);
        bool any = false;
        for (size_t i = 0; i < c.layers.size(); ++i) {
          const Layer& l = c.layers[i];
          if (l.muted || l.file.empty() || ranges[i].first > ranges[i].second) continue;
          PadLayer pl;
          pl.path = samplePath(kit, l.file);
          std::error_code ec;
          if (!fs::is_regular_file(fs::u8path(pl.path), ec)) missing.insert(l.file);
          pl.lo = ranges[i].first, pl.hi = ranges[i].second;
          const float lv = compGain * l.gain;
          if (lv > 1.0001f) clipped.push_back(in.name);
          pl.level = std::clamp(lv, 0.0f, 1.0f);
          pl.tune = std::clamp(l.pitch, -24.0f, 24.0f);
          layers.push_back(pl);
          any = true;
        }
        if (any) {
          if (selectSet && c.select != select) mixed = true;
          if (!selectSet || select == Select::Velocity) select = c.select;
          selectSet = true;
        }
      }
      int componentsUsed = 0;
      for (const Component& c : in.components) componentsUsed += !c.muted && !c.layers.empty();
      if (select != Select::Velocity && (componentsUsed > 1 || mixed)) rrMixed.push_back(in.name);
      if (static_cast<int>(layers.size()) > pp::kPadMaxLayers) {
        tooManyLayers.push_back(in.name);
        layers.resize(static_cast<size_t>(pp::kPadMaxLayers));
      }
      r.layers += static_cast<int>(layers.size());
      // The layers: 1..4 in parameters, the rest in data lines.
      for (int l = 0; l < pp::kPadLayers; ++l) setParam(st, pp::padL(k, l, pp::LayEnabled), 0);
      if (layers.empty() && k < pp::kPads) st.data = setPadSamplePath(st.data, k, 0, "none");
      for (size_t l = 0; l < layers.size(); ++l) {
        const PadLayer& pl = layers[l];
        const int li = static_cast<int>(l);
        st.data = setPadSamplePath(st.data, k, li, pl.path);
        if (li < pp::kPadLayers) {
          setParam(st, pp::padL(k, li, pp::LayEnabled), 1);
          setParam(st, pp::padL(k, li, pp::LayVelLow), static_cast<float>(pl.lo));
          setParam(st, pp::padL(k, li, pp::LayVelHigh), static_cast<float>(pl.hi));
          setParam(st, pp::padL(k, li, pp::LayLevel), pl.level);
          setParam(st, pp::padL(k, li, pp::LayTune), pl.tune);
        } else {
          PadLayerX x;
          x.lo = pl.lo, x.hi = pl.hi, x.level = pl.level, x.tune = pl.tune;
          st.data = padDataSet(st.data, "S" + std::to_string(k + 1) + "." + std::to_string(li + 1), formatPadLayer(x));
        }
      }
      st.data = padDataSet(st.data, "N" + std::to_string(k + 1), in.name);
      // The pad.
      const float level = in.volume * in.gain;
      if (level > 1.0001f) clipped.push_back(in.name);
      setParam(st, pp::padP(k, pp::PadLevel), std::clamp(level, 0.0f, 1.0f));
      setParam(st, pp::padP(k, pp::PadPan), std::clamp(in.pan, -1.0f, 1.0f));
      setParam(st, pp::padP(k, pp::PadTune), std::clamp(in.pitch, -24.0f, 24.0f));
      setParam(st, pp::padE(k, pp::PadMute), in.muted ? 1.0f : 0.0f);
      setParam(st, pp::padE(k, pp::PadSolo), in.soloed ? 1.0f : 0.0f);
      setParam(st, pp::padE(k, pp::PadVelScale), in.applyVelocity ? 1.0f : 0.0f);
      setParam(st, pp::padE(k, pp::PadOneShot), in.stopNote ? 0.0f : 1.0f);
      setParam(st, pp::padE(k, pp::PadLayerMode),
               static_cast<float>(select == Select::RoundRobin ? pp::LayerCycle : select == Select::Random ? pp::LayerRandom : pp::LayerStack));
      // Envelope: frames to seconds. A flat envelope (no decay to a lower sustain) keeps the pad's own.
      setParam(st, pp::padE(k, pp::PadAttack), std::clamp(static_cast<float>(in.attack / kFrameRate), 0.0f, 2.0f));
      if (in.sustain < 0.999f) {
        setParam(st, pp::padE(k, pp::PadHold), 0);
        setParam(st, pp::padE(k, pp::PadDecay), std::clamp(static_cast<float>(in.decay / kFrameRate), 0.005f, 10.0f));
        setParam(st, pp::padE(k, pp::PadSustain), in.sustain);
      }
      setParam(st, pp::padE(k, pp::PadRelease), std::clamp(static_cast<float>(in.release / kFrameRate), 0.002f, 5.0f));
      // Mute group: a cut group the pad both belongs to and is cut by.
      if (in.muteGroup >= 0) {
        auto it = groupOf.find(in.muteGroup);
        if (it == groupOf.end()) {
          const int g = static_cast<int>(groupOf.size()) + 1;
          it = groupOf.emplace(in.muteGroup, g <= 8 ? g : 0).first;
        }
        if (it->second > 0) {
          setParam(st, pp::padP(k, pp::PadCut), static_cast<float>(it->second));
          setParam(st, pp::padE(k, pp::PadCutBy), static_cast<float>(it->second));
        } else {
          groupsLost.push_back(in.name);
        }
      } else {
        setParam(st, pp::padP(k, pp::PadCut), 0);
        setParam(st, pp::padE(k, pp::PadCutBy), 0);
      }
      if (in.filterActive && in.cutoff < 0.999f) filtered.push_back(in.name);
      if (in.randomPitch > 0.0001f) randomPitch.push_back(in.name);
    }
    r.channels.push_back(std::move(st));
    r.names.push_back(ch == 0 ? kitName : kitName + " " + std::to_string(ch + 1));
  }
  auto dedupe = [](std::vector<std::string>& v) {
    std::vector<std::string> o;
    for (auto& s : v)
      if (std::find(o.begin(), o.end(), s) == o.end()) o.push_back(s);
    v = o;
  };
  for (auto* v : {&clipped, &filtered, &randomPitch, &tooManyLayers, &rrMixed, &groupsLost}) dedupe(*v);
  r.missing.assign(missing.begin(), missing.end());
  if (!r.missing.empty())
    r.notes.push_back({i18n::N_("Samples that are not there: %s"), std::to_string(r.missing.size()) + " (" + joinNames(r.missing, 4) + ")"});
  if (!clipped.empty()) r.notes.push_back({i18n::N_("Levels above full were set to full: %s"), joinNames(clipped)});
  if (!filtered.empty()) r.notes.push_back({i18n::N_("Their filter is not carried over: %s"), joinNames(filtered)});
  if (!randomPitch.empty()) r.notes.push_back({i18n::N_("Their random pitch is not carried over: %s"), joinNames(randomPitch)});
  if (!tooManyLayers.empty()) r.notes.push_back({i18n::N_("Only the first 64 layers play: %s"), joinNames(tooManyLayers)});
  if (!rrMixed.empty()) r.notes.push_back({i18n::N_("Round robin over several sample sets plays one layer at a time: %s"), joinNames(rrMixed)});
  if (!groupsLost.empty()) r.notes.push_back({i18n::N_("More than eight mute groups, these have none: %s"), joinNames(groupsLost)});
  return r;
}

std::vector<std::pair<const char*, std::string>> creditLines(const Kit& kit) {
  std::vector<std::pair<const char*, std::string>> out;
  if (!kit.author.empty()) out.emplace_back(i18n::N_("Author"), kit.author);
  if (!kit.license.empty()) out.emplace_back(i18n::N_("Licence"), kit.license);
  if (!kit.imageLicense.empty() && kit.imageLicense != kit.license && !kit.image.empty()) out.emplace_back(i18n::N_("Picture licence"), kit.imageLicense);
  return out;
}

}  // namespace gl::hydrogen
