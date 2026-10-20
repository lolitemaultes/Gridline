// Gridline — browser content model (see BrowserIndex.h).
#include "BrowserIndex.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../engine/Sample.h"
#include "Json.h"
#include "Project.h"

namespace gl::browse {

namespace fs = std::filesystem;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

namespace {

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string extOf(const std::string& path) { return lower(fs::path(path).extension().string()); }

// Splits on anything but letters, digits (and '#' when keepSharp).
std::vector<std::string> tokens(const std::string& s, bool keepSharp) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || (keepSharp && c == '#')) {
      cur += c;
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool allDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

int pitchOfLetter(char c) {
  switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return -1;
  }
}

// "C#", "Bb", "Eb" + mode suffix. `mode` gets the suffix ("", "m", "min", …).
bool splitKeyToken(const std::string& t, int& pc, std::string& mode) {
  if (t.empty()) return false;
  pc = pitchOfLetter(t[0]);
  if (pc < 0) return false;
  size_t i = 1;
  if (i < t.size() && t[i] == '#') {
    pc = (pc + 1) % 12;
    ++i;
  } else if (i < t.size() && t[i] == 'b' && (i + 1 == t.size() || t[i + 1] == 'm' || t[i + 1] == 'M')) {
    // "Bb", "Ebm", "Abmaj" (a lower-case 'b' right after the letter is a flat)
    pc = (pc + 11) % 12;
    ++i;
  }
  mode = t.substr(i);
  return true;
}

double fileMtime(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  return static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

}  // namespace

// ------------------------------------------------------------------- kinds

Kind kindOf(const std::string& path) {
  const std::string e = extOf(path);
  if (isAudioFile(path)) return Kind::Audio;
  if (e == ".mid" || e == ".midi") return Kind::Midi;
  if (e == ".flp") return Kind::Flp;
  if (e == ".zip") return Kind::Project;  // J-52: zipped project / FL Studio zipped loop package
  if (e == ".fst") return Kind::Fst;
  if (e == ".fsc") return Kind::Score;
  if (e == ".json") {
    const std::string l = lower(fs::path(path).generic_string());
    if (endsWith(l, ".gridline.json")) return Kind::Project;
    if (l.find("/presets/chains/") != std::string::npos) return Kind::Chain;
    if (l.find("/presets/automation/") != std::string::npos) return Kind::Shape;
    if (l.find("/presets/") != std::string::npos) return Kind::Preset;
  }
  return Kind::Other;
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Audio: return "audio";
    case Kind::Midi: return "midi";
    case Kind::Project: return "project";
    case Kind::Flp: return "flp";
    case Kind::Fst: return "fst";
    case Kind::Preset: return "preset";
    case Kind::Chain: return "chain";
    case Kind::Shape: return "shape";
    case Kind::Score: return "score";
    case Kind::Folder: return "folder";
    case Kind::Other: break;
  }
  return "other";
}

bool kindFromName(const std::string& name, Kind& out) {
  static const std::map<std::string, Kind> names = {
      {"audio", Kind::Audio},   {"sample", Kind::Audio},  {"samples", Kind::Audio}, {"sound", Kind::Audio},
      {"wav", Kind::Audio},     {"flac", Kind::Audio},    {"mp3", Kind::Audio},     {"ogg", Kind::Audio},
      {"wv", Kind::Audio},      {"midi", Kind::Midi},     {"mid", Kind::Midi},      {"project", Kind::Project},
      {"projects", Kind::Project}, {"flp", Kind::Flp},    {"fst", Kind::Fst},       {"preset", Kind::Preset},
      {"presets", Kind::Preset}, {"chain", Kind::Chain},  {"chains", Kind::Chain},  {"shape", Kind::Shape},
      {"automation", Kind::Shape}, {"score", Kind::Score}, {"fsc", Kind::Score},
  };
  auto it = names.find(lower(name));
  if (it == names.end()) return false;
  out = it->second;
  return true;
}

// ----------------------------------------------------------- tempo and key

double bpmFromName(const std::string& name) {
  const std::string l = lower(fs::path(name).stem().string());
  const std::vector<std::string> t = tokens(l, false);
  auto inRange = [](double v) { return v >= 40 && v <= 300; };
  for (size_t i = 0; i < t.size(); ++i) {
    const std::string& s = t[i];
    // "120bpm"
    if (endsWith(s, "bpm") && s.size() > 3 && allDigits(s.substr(0, s.size() - 3))) {
      double v = std::atof(s.c_str());
      if (inRange(v)) return v;
    }
    // "bpm120"
    if (s.rfind("bpm", 0) == 0 && s.size() > 3 && allDigits(s.substr(3))) {
      double v = std::atof(s.c_str() + 3);
      if (inRange(v)) return v;
    }
    // "120 bpm" / "bpm 120"
    if (s == "bpm") {
      if (i > 0 && allDigits(t[i - 1]) && inRange(std::atof(t[i - 1].c_str()))) return std::atof(t[i - 1].c_str());
      if (i + 1 < t.size() && allDigits(t[i + 1]) && inRange(std::atof(t[i + 1].c_str()))) return std::atof(t[i + 1].c_str());
    }
  }
  // A bare number in loop names ("Funk loop 96", "groove_120").
  const bool loopish = l.find("loop") != std::string::npos || l.find("groove") != std::string::npos || l.find("beat") != std::string::npos;
  if (loopish)
    for (const auto& s : t)
      if (allDigits(s) && s.size() >= 2 && s.size() <= 3) {
        double v = std::atof(s.c_str());
        if (v >= 70 && v <= 180) return v;
      }
  return 0;
}

bool keyFromName(const std::string& name, int& pitchClass, bool& minor) {
  const std::string stem = fs::path(name).stem().string();
  const std::vector<std::string> t = tokens(stem, true);
  for (size_t i = 0; i < t.size(); ++i) {
    int pc;
    std::string mode;
    if (!splitKeyToken(t[i], pc, mode)) continue;
    const bool upperRoot = std::isupper(static_cast<unsigned char>(t[i][0])) != 0;
    const std::string lm = lower(mode);
    const bool accidental = t[i].size() > 1 && (t[i][1] == '#' || (t[i][1] == 'b' && t[i].size() - mode.size() == 2));
    std::string next = i + 1 < t.size() ? lower(t[i + 1]) : "";
    bool isMinor = false, ok = false;
    if (lm == "min" || lm == "minor") {
      isMinor = true, ok = true;
    } else if (lm == "maj" || lm == "major") {
      ok = true;
    } else if (mode == "m" && upperRoot) {  // "Am", "F#m"
      isMinor = true, ok = true;
    } else if (mode == "M" && upperRoot) {  // "CM"
      ok = true;
    } else if (mode.empty() && upperRoot) {
      if (next == "minor" || next == "min") {
        isMinor = true, ok = true;
      } else if (next == "major" || next == "maj") {
        ok = true;
      } else if (accidental) {  // "Bb", "F#"
        ok = true;
      }
    }
    if (!ok) continue;
    pitchClass = pc;
    minor = isMinor;
    return true;
  }
  return false;
}

std::string keyName(int pitchClass, bool minor) {
  static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (pitchClass < 0 || pitchClass > 11) return "";
  return std::string(names[pitchClass]) + (minor ? " minor" : " major");
}

bool parseKey(const std::string& text, int& pitchClass, int& minor) {
  std::string s;
  for (char c : text)
    if (c != ' ' && c != '_' && c != '-') s += c;
  if (s.empty()) return false;
  int pc = pitchOfLetter(s[0]);
  if (pc < 0) return false;
  size_t i = 1;
  if (i < s.size() && s[i] == '#') {
    pc = (pc + 1) % 12;
    ++i;
  } else if (i < s.size() && s[i] == 'b') {  // "bb", "eb", "Ebm": a 'b' after the root is a flat
    pc = (pc + 11) % 12;
    ++i;
  }
  const std::string rest = s.substr(std::min(i, s.size()));
  const std::string lr = lower(rest);
  if (rest.empty()) minor = -1;
  else if (rest == "m" || lr == "min" || lr == "minor") minor = 1;
  else if (rest == "M" || lr == "maj" || lr == "major") minor = 0;
  else return false;
  pitchClass = pc;
  return true;
}

double bpmFromLength(double seconds) {
  if (!(seconds >= 1.0) || seconds > 120) return 0;
  double best = 0, bestScore = 1e9;
  for (int bars : {1, 2, 4, 8, 16}) {
    const double bpm = 240.0 * bars / seconds;
    if (bpm < 70 || bpm > 180) continue;
    const double half = std::round(bpm * 2) / 2;
    if (std::fabs(bpm - half) > 0.02 * bpm / 100 + 0.015) continue;  // must land on a whole or half BPM
    const double score = std::fabs(half - 125.0) + (half != std::round(half) ? 30 : 0);
    if (score < bestScore) bestScore = score, best = half;
  }
  return best;
}

double wavAcidTempo(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  unsigned char h[12];
  if (!f.read(reinterpret_cast<char*>(h), 12) || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return 0;
  auto le32 = [](const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  for (int guard = 0; guard < 64; ++guard) {
    unsigned char c[8];
    if (!f.read(reinterpret_cast<char*>(c), 8)) break;
    const uint32_t n = le32(c + 4);
    if (std::memcmp(c, "acid", 4) == 0 && n >= 24) {
      unsigned char a[24];
      if (!f.read(reinterpret_cast<char*>(a), 24)) break;
      uint32_t bits = le32(a + 20);
      float tempo;
      std::memcpy(&tempo, &bits, sizeof tempo);
      if (!std::isfinite(tempo) || tempo < 20 || tempo > 400) return 0;
      double t = tempo;
      while (t < 70) t *= 2;
      while (t > 180) t /= 2;
      return std::round(t * 100) / 100;
    }
    f.seekg(static_cast<std::streamoff>(n + (n & 1)), std::ios::cur);
    if (!f) break;
  }
  return 0;
}

double syncRatio(double sampleBpm, double songBpm) {
  if (!(sampleBpm > 0) || !(songBpm > 0)) return 1.0;
  double r = songBpm / sampleBpm;
  const double hi = std::sqrt(2.0);
  for (int i = 0; i < 8 && r > hi; ++i) r /= 2;
  for (int i = 0; i < 8 && r < 1 / hi; ++i) r *= 2;
  return r;
}

std::vector<std::string> autoTags(const std::string& path) {
  static const std::vector<std::pair<const char*, const char*>> vocab = {
      {"kick", "kick"},   {"bd", "kick"},        {"snare", "snare"},  {"sd", "snare"},     {"rim", "snare"},
      {"hihat", "hat"},   {"hat", "hat"},        {"hh", "hat"},       {"clap", "clap"},    {"snap", "clap"},
      {"perc", "perc"},   {"shaker", "perc"},    {"conga", "perc"},   {"bongo", "perc"},   {"tom", "tom"},
      {"crash", "cymbal"}, {"ride", "cymbal"},   {"cymbal", "cymbal"}, {"808", "808"},     {"bass", "bass"},
      {"sub", "bass"},    {"vocal", "vocal"},    {"vox", "vocal"},    {"voice", "vocal"},  {"chant", "vocal"},
      {"fx", "fx"},       {"sfx", "fx"},         {"riser", "fx"},     {"impact", "fx"},    {"sweep", "fx"},
      {"noise", "fx"},    {"loop", "loop"},      {"loops", "loop"},   {"pad", "pad"},      {"lead", "lead"},
      {"pluck", "pluck"}, {"chord", "chord"},    {"synth", "synth"},  {"drum", "drums"},   {"drums", "drums"},
      {"piano", "keys"},  {"keys", "keys"},      {"rhodes", "keys"},  {"organ", "keys"},   {"guitar", "guitar"},
      {"string", "strings"}, {"strings", "strings"}, {"brass", "brass"}, {"horn", "brass"}, {"impulse", "ir"},
      {"impulses", "ir"}, {"reverb", "ir"},      {"oneshot", "oneshot"}, {"shot", "oneshot"}, {"break", "loop"},
  };
  fs::path p(path);
  std::string hay = p.stem().string();
  if (p.has_parent_path()) {
    hay += " " + p.parent_path().filename().string();
    if (p.parent_path().has_parent_path()) hay += " " + p.parent_path().parent_path().filename().string();
  }
  std::vector<std::string> out;
  for (const std::string& t : tokens(lower(hay), false)) {
    for (const auto& [kw, tag] : vocab) {
      const std::string k = kw;
      // Short keywords must match the whole token; longer ones may prefix ("kicks", "snares").
      const bool hit = k.size() <= 3 ? t == k || (t.size() == k.size() + 1 && t.compare(0, k.size(), k) == 0 && t.back() == 's')
                                     : t.compare(0, k.size(), k) == 0;
      if (hit && std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
    }
  }
  return out;
}

// ------------------------------------------------------------------- peaks

std::string encodePeaks(const std::vector<float>& peaks) {
  static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string s;
  s.reserve(peaks.size());
  for (float v : peaks) {
    float c = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    s += digits[static_cast<int>(std::lround(c * 35.0f))];
  }
  return s;
}

std::vector<float> decodePeaks(const std::string& s) {
  std::vector<float> out;
  out.reserve(s.size());
  for (char c : s) {
    int q = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 0;
    out.push_back(static_cast<float>(q) / 35.0f);
  }
  return out;
}

std::vector<float> computePeaks(const std::vector<float>& left, const std::vector<float>& right, int buckets) {
  std::vector<float> out(static_cast<size_t>(std::max(0, buckets)), 0.0f);
  const size_t n = left.size();
  if (n == 0 || buckets <= 0) return out;
  for (size_t i = 0; i < n; ++i) {
    size_t b = std::min(out.size() - 1, i * out.size() / n);
    float v = std::fabs(left[i]);
    if (i < right.size()) v = std::max(v, std::fabs(right[i]));
    if (std::isfinite(v)) out[b] = std::max(out[b], std::min(v, 1.0f));
  }
  return out;
}

// ------------------------------------------------------------------ search

Query parseQuery(const std::string& text) {
  Query q;
  std::vector<std::string> parts;
  {
    std::string cur;
    bool quoted = false;
    for (char c : text) {
      if (c == '"') {
        quoted = !quoted;
      } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
        if (!cur.empty()) parts.push_back(cur), cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) parts.push_back(cur);
  }
  auto range = [](const std::string& v, double& lo, double& hi, double tol) {
    if (v.empty()) return false;
    if (v[0] == '>' || v[0] == '<') {
      size_t o = v.size() > 1 && v[1] == '=' ? 2 : 1;
      double x = std::atof(v.c_str() + o);
      if (!(x > 0) && v.substr(o) != "0") return false;
      if (v[0] == '>') lo = x, hi = 1e9;
      else lo = 0, hi = x;
      return true;
    }
    size_t dash = v.find('-', 1);
    if (dash != std::string::npos) {
      lo = std::atof(v.substr(0, dash).c_str());
      hi = std::atof(v.substr(dash + 1).c_str());
      if (lo > hi) std::swap(lo, hi);
      return hi > 0;
    }
    double x = std::atof(v.c_str());
    if (!(x > 0)) return false;
    lo = x - tol;
    hi = x + tol;
    return true;
  };
  for (const std::string& p : parts) {
    size_t colon = p.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < p.size()) {
      const std::string key = lower(p.substr(0, colon));
      const std::string val = p.substr(colon + 1);
      if (key == "type" || key == "kind" || key == "is") {
        std::stringstream ss(lower(val));
        for (std::string v; std::getline(ss, v, ',');) {
          if (v == "plugin" || v == "plugins" || v == "instrument" || v == "instruments" || v == "generator" || v == "synth" ||
              v == "effect" || v == "effects" || v == "fx" || v == "notefx") {
            q.pluginKinds.push_back(v);
            continue;
          }
          Kind k;
          if (kindFromName(v, k)) {
            q.kinds.insert(k);
            if (k == Kind::Project) q.kinds.insert(Kind::Flp);
            if (k == Kind::Preset) q.kinds.insert(Kind::Fst);
          }
        }
        continue;
      }
      if (key == "tag" || key == "tags" || key == "#") {
        std::stringstream ss(lower(val));
        for (std::string v; std::getline(ss, v, ',');)
          if (!v.empty()) q.tags.push_back(v);
        continue;
      }
      if (key == "bpm" || key == "tempo") {
        double lo = 0, hi = 0;
        if (range(val, lo, hi, 1.0)) q.bpmLo = lo, q.bpmHi = hi;
        continue;
      }
      if (key == "key") {
        int pc, mn;
        if (parseKey(val, pc, mn)) q.key = pc, q.minor = mn;
        continue;
      }
      if (key == "len" || key == "length" || key == "dur") {
        double lo = 0, hi = 0;
        if (range(val, lo, hi, 0.25)) q.lenLo = lo, q.lenHi = hi;
        continue;
      }
    }
    if (!p.empty() && p[0] == '#' && p.size() > 1) {  // "#kick" = tag:kick
      q.tags.push_back(lower(p.substr(1)));
      continue;
    }
    q.words.push_back(lower(p));
  }
  return q;
}

namespace {
bool hasTag(const FileMeta& m, const std::string& tag, const std::vector<std::string>& autos) {
  for (const auto& t : m.tags)
    if (t == tag || t.rfind(tag, 0) == 0) return true;
  for (const auto& t : autos)
    if (t == tag || t.rfind(tag, 0) == 0) return true;
  return false;
}
}  // namespace

bool matches(const FileMeta& m, const Query& q) {
  if (!q.pluginKinds.empty()) return false;
  if (!q.kinds.empty() && !q.kinds.count(m.kind)) return false;
  if (q.bpmHi > 0 && !(m.bpm > 0 && m.bpm >= q.bpmLo && m.bpm <= q.bpmHi)) return false;
  if (q.key >= 0 && !(m.key == q.key && (q.minor < 0 || m.minor == (q.minor == 1)))) return false;
  if (q.lenHi > 0 && !(m.seconds > 0 && m.seconds >= q.lenLo && m.seconds <= q.lenHi)) return false;
  std::vector<std::string> autos;
  if (!q.tags.empty() || !q.words.empty()) autos = autoTags(m.path);
  for (const auto& t : q.tags)
    if (!hasTag(m, t, autos)) return false;
  if (!q.words.empty()) {
    fs::path p(m.path);
    std::string hay = lower(p.filename().string());
    if (p.has_parent_path()) {
      hay += " " + lower(p.parent_path().filename().string());
      if (p.parent_path().has_parent_path()) hay += " " + lower(p.parent_path().parent_path().filename().string());
    }
    for (const auto& t : m.tags) hay += " " + t;
    for (const auto& t : autos) hay += " " + t;
    for (const auto& w : q.words)
      if (hay.find(w) == std::string::npos) return false;
  }
  return true;
}

bool matchesPlugin(const PluginInfo& p, const Query& q, bool favourite) {
  if (!q.kinds.empty() || q.bpmHi > 0 || q.key >= 0 || q.lenHi > 0) return false;
  if (!q.pluginKinds.empty()) {
    bool ok = false;
    for (const auto& k : q.pluginKinds) {
      if (k == "plugin" || k == "plugins") ok = true;
      else if ((k == "instrument" || k == "instruments" || k == "generator" || k == "synth") && p.kind == PluginKind::Generator) ok = true;
      else if ((k == "effect" || k == "effects" || k == "fx") && p.kind == PluginKind::Effect) ok = true;
      else if (k == "notefx" && p.kind == PluginKind::NoteFx) ok = true;
    }
    if (!ok) return false;
  }
  const std::string cat = lower(p.category);
  for (const auto& t : q.tags) {
    const bool favTag = t == "fav" || t == "favourite" || t == "favorite" || t == "star" || t == "starred";
    if (favTag ? !favourite : cat.find(t) == std::string::npos) return false;
  }
  if (!q.words.empty()) {
    const std::string hay = lower(p.name) + " " + cat + " " + lower(p.type);
    for (const auto& w : q.words)
      if (hay.find(w) == std::string::npos) return false;
  }
  return true;
}

// ------------------------------------------------------------- preset tree

size_t PresetNode::count() const {
  size_t n = presets.size();
  for (const auto& f : folders) n += f.count();
  return n;
}

PresetNode buildPresetTree(const std::vector<PresetEntry>& entries) {
  PresetNode root;
  for (size_t i = 0; i < entries.size(); ++i) {
    PresetNode* node = &root;
    std::stringstream ss(entries[i].folder);
    for (std::string part; std::getline(ss, part, '/');) {
      if (part.empty()) continue;
      auto it = std::find_if(node->folders.begin(), node->folders.end(), [&](const PresetNode& n) { return n.name == part; });
      if (it == node->folders.end()) {
        node->folders.push_back(PresetNode{part, {}, {}});
        it = node->folders.end() - 1;
      }
      node = &*it;
    }
    node->presets.push_back(i);
  }
  // Sort folders and presets by name (case-insensitive), recursively.
  struct Sorter {
    const std::vector<PresetEntry>& e;
    void operator()(PresetNode& n) const {
      std::sort(n.folders.begin(), n.folders.end(), [](const PresetNode& a, const PresetNode& b) { return lower(a.name) < lower(b.name); });
      std::sort(n.presets.begin(), n.presets.end(), [this](size_t a, size_t b) { return lower(e[a].name) < lower(e[b].name); });
      for (auto& f : n.folders) (*this)(f);
    }
  };
  Sorter{entries}(root);
  return root;
}

std::vector<std::string> libraryTypes(const std::string& presetRoot) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(presetRoot, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    std::string n = it->path().filename().string();
    if (n.empty() || n[0] == '.' || n == "chains" || n == "automation") continue;
    out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ------------------------------------------------------------- drop table

DropAction dropAction(const DropItem& item, DropTarget target, const std::string& targetType, bool targetUsesSample) {
  switch (item.kind) {
    case Kind::Audio:
      switch (target) {
        case DropTarget::Rack:
        case DropTarget::Workspace: return DropAction::NewSamplerChannel;
        case DropTarget::Playlist: return DropAction::AudioClip;
        case DropTarget::MixerTrack:
        case DropTarget::MixerSlot: return DropAction::ConvolverIR;
        case DropTarget::PluginWindow: return targetUsesSample ? DropAction::LoadSample : DropAction::None;
        case DropTarget::PianoRoll: return DropAction::None;
      }
      break;
    case Kind::Midi:
      switch (target) {
        case DropTarget::PianoRoll: return DropAction::ImportMidiNotes;
        case DropTarget::Rack:
        case DropTarget::Playlist:
        case DropTarget::Workspace: return DropAction::ImportMidiChannels;
        default: return DropAction::None;
      }
    case Kind::Project:
    case Kind::Flp:
      if (target == DropTarget::Rack || target == DropTarget::Playlist || target == DropTarget::Workspace)
        return DropAction::OpenProject;  // J-52: FL Studio projects open as projects
      return DropAction::None;
    case Kind::Preset:
      if (target == DropTarget::PluginWindow) return !targetType.empty() && targetType == item.pluginType ? DropAction::ApplyPreset : DropAction::None;
      if (item.pluginKind == PluginKind::Generator)
        return target == DropTarget::Rack || target == DropTarget::Workspace ? DropAction::NewChannelFromPreset : DropAction::None;
      if (item.pluginKind == PluginKind::Effect)
        return target == DropTarget::MixerTrack || target == DropTarget::MixerSlot ? DropAction::InsertEffectPreset : DropAction::None;
      return DropAction::None;
    case Kind::Chain:
      return target == DropTarget::MixerTrack || target == DropTarget::MixerSlot ? DropAction::ApplyChain : DropAction::None;
    default: break;
  }
  return DropAction::None;
}

const char* dropActionLabel(DropAction a) {
  switch (a) {
    case DropAction::None: return "";
    case DropAction::NewSamplerChannel: return "New sampler channel";
    case DropAction::AudioClip: return "Audio clip";
    case DropAction::LoadSample: return "Load sample";
    case DropAction::ConvolverIR: return "Convolver with this impulse";
    case DropAction::ApplyPreset: return "Load preset";
    case DropAction::NewChannelFromPreset: return "New channel from preset";
    case DropAction::InsertEffectPreset: return "Insert effect with preset";
    case DropAction::ApplyChain: return "Load effect chain";
    case DropAction::ImportMidiChannels: return "Import MIDI (new channels)";
    case DropAction::ImportMidiNotes: return "Import MIDI notes";
    case DropAction::OpenProject: return "Open project";
  }
  return "";
}

// ----------------------------------------------------------------- indexer

// Bumped when the analysis changes (2: ACID tempo); older cached analyses are redone.
constexpr double kCacheVersion = 2;

Index::~Index() { stop(); }

void Index::setCacheFile(const std::string& file) {
  std::lock_guard<std::mutex> lk(mu_);
  cacheFile_ = file;
}

void Index::setRoots(const std::vector<std::pair<std::string, bool>>& roots) {
  std::lock_guard<std::mutex> lk(mu_);
  roots_ = roots;
}

void Index::start() {
  stop();
  std::vector<std::pair<std::string, bool>> roots;
  {
    std::lock_guard<std::mutex> lk(mu_);
    roots = roots_;
  }
  cancel_ = false;
  busy_ = true;
  done_ = 0;
  pending_ = 0;
  worker_ = std::thread([this, roots]() { run(roots); });
}

void Index::stop() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
  busy_ = false;
}

void Index::waitIdle() {
  if (worker_.joinable()) worker_.join();
  busy_ = false;
}

FileMeta Index::describe(const std::string& path, bool analyse) {
  FileMeta m;
  m.path = path;
  m.kind = kindOf(path);
  std::error_code ec;
  m.size = static_cast<uint64_t>(fs::file_size(path, ec));
  if (ec) m.size = 0;
  m.mtime = static_cast<int64_t>(fileMtime(path));
  const std::string name = fs::path(path).filename().string();
  if (m.kind == Kind::Audio) {
    m.bpm = bpmFromName(name);
    keyFromName(name, m.key, m.minor);
  }
  if (analyse && m.kind == Kind::Audio) {
    m.analysed = true;
    if (m.size > 0 && m.size < (96u << 20)) {
      if (SamplePtr s = loadSample(path)) {
        m.seconds = s->seconds();
        m.rate = s->sampleRate;
        m.channels = s->left == s->right ? 1 : 2;
        m.peaks = encodePeaks(computePeaks(s->left, s->right, 64));
        if (m.bpm <= 0) m.bpm = wavAcidTempo(path);
        if (m.bpm <= 0) {
          const auto tags = autoTags(path);
          if (std::find(tags.begin(), tags.end(), "loop") != tags.end()) m.bpm = bpmFromLength(m.seconds);
        }
      }
    }
  }
  return m;
}

bool Index::analyseNow(const std::string& path, FileMeta* out) {
  if (kindOf(path) != Kind::Audio) return false;
  FileMeta m = describe(path, true);
  m.tags = tagsOf(path);
  put(m);
  if (out) *out = m;
  return true;
}

void Index::put(const FileMeta& m) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    FileMeta c = m;
    if (auto t = tags_.find(m.path); t != tags_.end()) c.tags = t->second;
    entries_[m.path] = std::move(c);
  }
  ++gen_;
}

bool Index::lookup(const std::string& path, FileMeta& out) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(path);
  if (it == entries_.end()) return false;
  out = it->second;
  return true;
}

size_t Index::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.size();
}

std::vector<FileMeta> Index::search(const Query& q, size_t limit) const {
  std::vector<FileMeta> out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [p, m] : entries_) {
      if (!listable(m.kind) || !matches(m, q)) continue;
      out.push_back(m);
      if (out.size() >= limit * 4) break;  // enough to sort and cut
    }
  }
  std::sort(out.begin(), out.end(), [](const FileMeta& a, const FileMeta& b) {
    const std::string na = lower(fs::path(a.path).filename().string()), nb = lower(fs::path(b.path).filename().string());
    return na != nb ? na < nb : a.path < b.path;
  });
  if (out.size() > limit) out.resize(limit);
  return out;
}

void Index::setTags(const std::string& path, std::vector<std::string> tags) {
  for (auto& t : tags) {
    t = lower(t);
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == ','; }), t.end());
  }
  tags.erase(std::remove(tags.begin(), tags.end(), std::string()), tags.end());
  std::sort(tags.begin(), tags.end());
  tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (tags.empty()) tags_.erase(path);
    else tags_[path] = tags;
    auto it = entries_.find(path);
    if (it == entries_.end()) {
      FileMeta m;
      m.path = path;
      m.kind = kindOf(path);
      it = entries_.emplace(path, m).first;
    }
    it->second.tags = tags;
  }
  ++gen_;
  save();
}

std::vector<std::string> Index::tagsOf(const std::string& path) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = tags_.find(path);
  return it == tags_.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> Index::allTags() const {
  std::set<std::string> s;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [p, t] : tags_) s.insert(t.begin(), t.end());
  }
  return {s.begin(), s.end()};
}

bool Index::save() const {
  Json root = Json::makeObject();
  std::string file;
  {
    std::lock_guard<std::mutex> lk(mu_);
    file = cacheFile_;
    if (file.empty()) return false;
    root.object["format"] = Json::makeString("gridline-browser-index");
    root.object["version"] = Json::makeNumber(kCacheVersion);
    Json files = Json::makeObject();
    for (const auto& [p, m] : entries_) {
      if (!m.analysed) continue;
      Json e = Json::makeObject();
      e.object["s"] = Json::makeNumber(static_cast<double>(m.size));
      e.object["m"] = Json::makeNumber(static_cast<double>(m.mtime));
      if (m.seconds > 0) e.object["sec"] = Json::makeNumber(std::round(m.seconds * 1000) / 1000);
      if (m.rate > 0) e.object["rate"] = Json::makeNumber(m.rate);
      if (m.channels > 0) e.object["ch"] = Json::makeNumber(m.channels);
      if (m.bpm > 0) e.object["bpm"] = Json::makeNumber(m.bpm);
      if (m.key >= 0) e.object["key"] = Json::makeNumber(m.key + (m.minor ? 12 : 0));
      if (!m.peaks.empty()) e.object["peaks"] = Json::makeString(m.peaks);
      files.object[p] = std::move(e);
    }
    root.object["files"] = std::move(files);
    Json tags = Json::makeObject();
    for (const auto& [p, t] : tags_) {
      Json a = Json::makeArray();
      for (const auto& x : t) a.array.push_back(Json::makeString(x));
      tags.object[p] = std::move(a);
    }
    root.object["tags"] = std::move(tags);
  }
  std::error_code ec;
  fs::create_directories(fs::path(file).parent_path(), ec);
  return writeFileAtomic(file, jsonStringify(root, 0));
}

bool Index::load() {
  std::string file;
  {
    std::lock_guard<std::mutex> lk(mu_);
    file = cacheFile_;
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || j.text("format") != "gridline-browser-index") return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (const Json* t = j.find("tags"); t && t->is(Json::Type::Object))
    for (const auto& [p, a] : t->object) {
      std::vector<std::string> v;
      for (const auto& x : a.array)
        if (x.is(Json::Type::String)) v.push_back(x.str);
      if (!v.empty()) tags_[p] = v;
    }
  if (const Json* f = j.find("files"); f && f->is(Json::Type::Object) && j.num("version") >= kCacheVersion)  // older analyses are redone
    for (const auto& [p, e] : f->object) {
      FileMeta m;
      m.path = p;
      m.kind = kindOf(p);
      m.analysed = true;
      m.size = jsonInt<uint64_t>(e.num("s"));
      m.mtime = jsonInt<int64_t>(e.num("m"));
      m.seconds = e.num("sec");
      m.rate = e.num("rate");
      m.channels = jsonInt<int>(e.num("ch"));
      m.bpm = e.num("bpm");
      int k = jsonInt<int>(e.num("key", -1));
      if (k >= 0 && k < 24) m.key = k % 12, m.minor = k >= 12;
      m.peaks = e.text("peaks");
      if (auto t = tags_.find(p); t != tags_.end()) m.tags = t->second;
      entries_[p] = std::move(m);
    }
  for (const auto& [p, t] : tags_)
    if (!entries_.count(p)) {
      FileMeta m;
      m.path = p;
      m.kind = kindOf(p);
      m.tags = t;
      entries_[p] = std::move(m);
    }
  ++gen_;
  return true;
}

void Index::run(std::vector<std::pair<std::string, bool>> roots) {
  constexpr size_t kMaxFiles = 80000;
  struct Todo {
    std::string path;
    bool analyse;
  };
  std::vector<Todo> todo;
  std::set<std::string> seen;
  bool changed = false;
  // 1. Walk: register every listable file at once (search works right away).
  for (const auto& [root, analyse] : roots) {
    if (cancel_) break;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) continue;
    std::vector<FileMeta> batch;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (cancel_ || seen.size() >= kMaxFiles) break;
      const std::string name = it->path().filename().string();
      if (name.empty() || name[0] == '.') {
        std::error_code e2;
        if (it->is_directory(e2)) it.disable_recursion_pending();
        continue;
      }
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      const std::string p = it->path().string();
      const Kind k = kindOf(p);
      if (!listable(k) || !seen.insert(p).second) continue;
      const uint64_t size = static_cast<uint64_t>(it->file_size(e2));
      const int64_t mtime = static_cast<int64_t>(fileMtime(it->path()));
      FileMeta old;
      bool have = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto f = entries_.find(p);
        if (f != entries_.end()) old = f->second, have = true;
      }
      const bool fresh = have && old.size == size && old.mtime == mtime;
      if (fresh && (old.analysed || !(analyse && k == Kind::Audio))) continue;
      FileMeta m = describe(p, false);
      if (fresh) m = old;
      batch.push_back(m);
      changed = true;
      if (analyse && k == Kind::Audio) todo.push_back({p, true});
      if (batch.size() >= 256) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& b : batch) {
          if (auto t = tags_.find(b.path); t != tags_.end()) b.tags = t->second;
          entries_[b.path] = b;
        }
        batch.clear();
        ++gen_;
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& b : batch) {
      if (auto t = tags_.find(b.path); t != tags_.end()) b.tags = t->second;
      entries_[b.path] = b;
    }
    ++gen_;
  }
  // Files under a walked root that are gone.
  if (!cancel_) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = entries_.begin(); it != entries_.end();) {
      bool under = false;
      for (const auto& r : roots) {
        const std::string pre = (fs::path(r.first) / "").string();
        if (it->first.compare(0, pre.size(), pre) == 0) under = true;
      }
      if (under && !seen.count(it->first) && !tags_.count(it->first)) {
        it = entries_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    ++gen_;
  }
  // 2. Analyse new / changed audio.
  pending_ = todo.size();
  size_t sinceSave = 0;
  // J-51: stop() also interrupts the analysis of one long file (the decoders
  // check the flag between blocks): the entry is then not stored.
  DecodeCancelScope cancellable(&cancel_);
  for (const auto& t : todo) {
    if (cancel_) break;
    FileMeta m = describe(t.path, true);
    if (cancel_) break;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (auto tg = tags_.find(m.path); tg != tags_.end()) m.tags = tg->second;
      entries_[m.path] = std::move(m);
    }
    ++gen_;
    ++done_;
    if (++sinceSave >= 300) {
      save();
      sinceSave = 0;
    }
  }
  if (changed || !todo.empty()) save();
  busy_ = false;
}

}  // namespace gl::browse
