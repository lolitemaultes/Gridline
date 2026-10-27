#include "PluginGroups.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include "HostedPlugins.h"
#include "PluginPrefs.h"

namespace gl::host {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// Category tokens: split at '|', ',' and ';', lower case, without the
// channel-layout and kind words every plugin carries.
std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
    std::string t = a == std::string::npos ? std::string() : lower(cur.substr(a, b - a + 1));
    cur.clear();
    static const char* const skip[] = {"stereo", "mono", "surround", "ambisonic", "fx", "audio-effect", "instrument", "plugin", "instrumentplugin"};
    if (t.empty()) return;
    for (const char* k : skip)
      if (t == k) return;
    out.push_back(t);
  };
  for (char c : s) {
    if (c == '|' || c == ',' || c == ';') flush();
    else cur += c;
  }
  flush();
  return out;
}

struct Rule {
  const char* category;
  std::vector<const char*> words;  // a word of 3 letters or fewer matches a token's start only
};
// Order matters: the first rule with a matching word wins.
const std::vector<Rule>& instrumentRules() {
  static const std::vector<Rule> r = {
      {"Drums", {"drum", "percussion", "beat"}},
      {"Keys", {"piano", "organ", "e-piano", "electric piano", "keyboard", "keys"}},
      {"Sampler", {"sampler", "sample", "rompler"}},
      {"Synth", {"synth", "oscillat", "wavetable", "generator"}},
  };
  return r;
}
const std::vector<Rule>& effectRules() {
  static const std::vector<Rule> r = {
      {"Analyzer", {"analyz", "analys", "meter", "scope", "metering"}},
      {"Restoration", {"restoration", "denois", "de-nois", "noise reduction", "declick"}},
      {"Reverb", {"reverb", "convol"}},
      {"Delay", {"delay", "echo"}},
      {"EQ", {"eq", "equaliz", "equalis"}},
      {"Filter", {"filter", "bandpass", "lowpass", "highpass", "band pass", "low pass", "high pass"}},
      {"Dynamics", {"dynamic", "compress", "limit", "gate", "expander", "transient", "de-esser", "deesser", "leveler"}},
      {"Distortion", {"distort", "saturat", "overdrive", "amp", "bitcrush", "crusher", "lofi", "lo-fi", "fuzz", "simulator"}},
      {"Modulation", {"modulat", "chorus", "flang", "phaser", "tremolo", "vibrato", "rotary", "ensemble"}},
      {"Pitch", {"pitch", "tuner", "tuning", "harmoniz"}},
      {"Spatial", {"spatial", "panner", "imager", "widen", "binaural"}},
      {"Spectral", {"vocoder", "spectral", "granular", "glitch", "freeze"}},
      {"MIDI", {"midi", "note-effect", "note effect", "arpeggiat"}},
      {"Mastering", {"mastering"}},
      {"Utility", {"utility", "tools", "tool", "gain", "mixing", "mixer", "converter", "routing"}},
  };
  return r;
}

bool wordMatches(const std::string& token, const char* word) {
  const std::string w(word);
  if (w.size() <= 3) return token.compare(0, w.size(), w) == 0;
  return token.find(w) != std::string::npos;
}

const char* matchRules(const std::vector<Rule>& rules, const std::vector<std::string>& toks) {
  for (const Rule& r : rules)
    for (const char* w : r.words)
      for (const std::string& t : toks)
        if (wordMatches(t, w)) return r.category;
  return nullptr;
}

// Words of a plugin name: split at spaces, punctuation and lower-to-upper
// case changes ("AUMatrixReverb" -> au matrix reverb). Only words of four or
// more letters count for names (short words are too ambiguous there).
std::vector<std::string> nameWords(const std::string& name) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    if (cur.size() >= 4) out.push_back(lower(cur));
    cur.clear();
  };
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (!alnum(c)) {
      flush();
      continue;
    }
    if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(name[i - 1]))) flush();
    cur += c;
  }
  flush();
  return out;
}

const std::vector<const char*>& defaultOrder() {
  static const std::vector<const char*> o = {"vst3", "clap", "au", "lv2", "vst2"};
  return o;
}

}  // namespace

std::string normaliseCategory(const std::string& subCategories, bool instrument, const std::string& name) {
  const auto& rules = instrument ? instrumentRules() : effectRules();
  if (const char* c = matchRules(rules, tokens(subCategories))) return c;
  if (!name.empty()) {
    const std::vector<std::string> words = nameWords(name);
    // Short rule words ("eq", "amp") never match name words.
    for (const Rule& r : rules)
      for (const char* w : r.words)
        if (std::string(w).size() > 3)
          for (const std::string& t : words)
            if (t.find(w) != std::string::npos) return r.category;
  }
  return instrument ? "Instrument" : "Effect";
}

const std::vector<const char*>& categoryNames() {
  static const std::vector<const char*> v = [] {
    std::vector<const char*> out;
    for (const Rule& r : instrumentRules()) out.push_back(r.category);
    out.push_back("Instrument");
    for (const Rule& r : effectRules()) out.push_back(r.category);
    out.push_back("Effect");
    return out;
  }();
  return v;
}

bool isGenericCategory(const std::string& c) { return c.empty() || c == "Instrument" || c == "Effect"; }

bool isOwnSuiteType(const std::string& type) {
  const size_t colon = type.find(':');
  if (colon == std::string::npos) return false;
  const std::string fmt = type.substr(0, colon), uid = type.substr(colon + 1);
  if (fmt == "clap") return uid.compare(0, 19, "org.gridline.suite.") == 0;
  if (fmt == "vst3") {
    std::string hex;
    for (char c : uid)
      if (std::isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return hex.size() == 32 && hex.compare(0, 24, "475249444C494E4553554954") == 0;  // "GRIDLINESUIT"
  }
  if (fmt == "au") {
    const size_t s2 = uid.find('/', uid.find('/') + 1);
    return s2 != std::string::npos && uid.find('/') != std::string::npos && uid.substr(s2 + 1) == "GrLn";
  }
  return false;
}

std::vector<std::string> formatOrder(const std::string& preferred) {
  std::vector<std::string> out;
  for (const char* f : defaultOrder())
    if (preferred == f) out.push_back(f);
  for (const char* f : defaultOrder())
    if (preferred != f) out.push_back(f);
  return out;
}

std::string groupNameKey(const std::string& name) {
  std::string n = lower(name);
  auto trim = [&n]() {
    while (!n.empty() && (n.back() == ' ' || n.back() == '-' || n.back() == '_')) n.pop_back();
  };
  trim();
  // Drop trailing format tags, one at a time: "Name (VST3)", "Name VST3", "Name-x64", "Name [AU]".
  static const char* const tags[] = {"vst3", "vst2", "vst", "clap", "au", "lv2", "x64", "64-bit", "64bit"};
  for (bool again = true; again;) {
    again = false;
    for (const char* tag : tags) {
      const std::string t(tag);
      for (const std::string& form : {" (" + t + ")", " [" + t + "]", " " + t, "-" + t, "_" + t}) {
        if (n.size() > form.size() && n.compare(n.size() - form.size(), form.size(), form) == 0) {
          n.resize(n.size() - form.size());
          trim();
          again = true;
          break;
        }
      }
      if (again) break;
    }
  }
  std::string out;
  for (char c : n)
    if (alnum(c)) out += c;
  return out.empty() ? lower(name) : out;
}

std::string groupVendorKey(const std::string& vendor) {
  std::string first;
  for (char c : vendor) {
    if (c == ' ' || c == ',' || c == '.') {
      if (!first.empty()) break;
      continue;
    }
    if (alnum(c)) first += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return first;
}

std::vector<PluginGroup> groupPlugins(const std::vector<GroupItem>& items, const std::string& preferredFormat,
                                      const std::set<std::string>& preferTypes) {
  const std::vector<std::string> order = formatOrder(preferredFormat);
  auto rank = [&order](const std::string& f) {
    auto it = std::find(order.begin(), order.end(), f);
    return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
  };
  struct Work {
    std::string nameKey, vendorKey;
    PluginGroup g;
  };
  std::vector<Work> work;
  std::map<std::string, std::vector<size_t>> byName;  // name key -> work indexes
  for (size_t i = 0; i < items.size(); ++i) {
    const GroupItem& it = items[i];
    const std::string nk = groupNameKey(it.name), vk = groupVendorKey(it.vendor);
    const bool own = isOwnSuiteType(it.type);
    size_t found = work.size();
    for (size_t w : byName[nk]) {
      const Work& x = work[w];
      if (x.g.ownSuite != own) continue;  // a Suite build never merges with a third-party plugin of the same name
      if (x.vendorKey == vk || x.vendorKey.empty() || vk.empty()) {
        found = w;
        break;
      }
    }
    if (found == work.size()) {
      work.push_back(Work{nk, vk, PluginGroup{}});
      work.back().g.ownSuite = own;
      byName[nk].push_back(found);
    }
    Work& w = work[found];
    if (w.vendorKey.empty()) w.vendorKey = vk;
    w.g.members.push_back(i);
  }
  std::vector<PluginGroup> out;
  out.reserve(work.size());
  for (Work& w : work) {
    PluginGroup& g = w.g;
    std::stable_sort(g.members.begin(), g.members.end(), [&](size_t a, size_t b) { return rank(items[a].format) < rank(items[b].format); });
    g.preferred = g.members.front();
    for (size_t m : g.members)
      if (preferTypes.count(items[m].type)) {
        g.preferred = m;
        g.userPicked = true;
        break;
      }
    if (g.userPicked) {  // the picked member leads the list
      std::stable_partition(g.members.begin(), g.members.end(), [&](size_t m) { return m == g.preferred; });
    }
    const GroupItem& p = items[g.preferred];
    g.name = p.name;
    g.instrument = p.instrument;
    for (size_t m : g.members)
      if (!items[m].vendor.empty()) {
        g.vendor = items[m].vendor;
        break;
      }
    if (!p.vendor.empty()) g.vendor = p.vendor;
    // The most specific category any format reports (preferred first), else from the name.
    for (size_t m : g.members) {
      const std::string c = normaliseCategory(items[m].subCategories, g.instrument);
      if (!isGenericCategory(c)) {
        g.category = c;
        break;
      }
    }
    if (g.category.empty()) g.category = normaliseCategory("", g.instrument, p.name);
    out.push_back(std::move(g));
  }
  return out;
}

// ---------------------------------------------------------------- snapshot

namespace {
struct SnapshotCache {
  std::mutex lock;
  std::shared_ptr<const GroupSnapshot> snap;
  uint64_t scanGen = ~uint64_t(0), prefsGen = ~uint64_t(0);
  PluginPrefs prefs;
};
SnapshotCache& snapCache() {
  static auto* c = new SnapshotCache;  // never destroyed (like the class map)
  return *c;
}
}  // namespace

std::shared_ptr<const GroupSnapshot> pluginGroups() {
  SnapshotCache& c = snapCache();
  const uint64_t sg = hostedClassesGeneration(), pg = pluginPrefsGeneration();
  std::lock_guard<std::mutex> g(c.lock);
  if (c.snap && sg == c.scanGen && pg == c.prefsGen) return c.snap;
  auto s = std::make_shared<GroupSnapshot>();
  const PluginPrefs prefs = pluginPrefs();
  for (const auto& cls : hostedClasses()) {
    if (prefs.disabledFormats.count(cls->format)) continue;
    s->classes.push_back(cls);
    s->items.push_back(GroupItem{cls->type, cls->format, cls->name, cls->vendor, cls->subCategories, cls->instrument});
  }
  // Stable order: by name, then format (hostedClasses() comes from a map keyed by type).
  std::vector<size_t> idx(s->items.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    const std::string x = lower(s->items[a].name), y = lower(s->items[b].name);
    return x != y ? x < y : s->items[a].type < s->items[b].type;
  });
  GroupSnapshot sorted;
  for (size_t i : idx) {
    sorted.classes.push_back(s->classes[i]);
    sorted.items.push_back(s->items[i]);
  }
  sorted.groups = groupPlugins(sorted.items, prefs.preferredFormat, prefs.preferTypes);
  for (size_t gi = 0; gi < sorted.groups.size(); ++gi)
    for (size_t m : sorted.groups[gi].members) sorted.groupOf[sorted.items[m].type] = gi;
  c.snap = std::make_shared<const GroupSnapshot>(std::move(sorted));
  c.scanGen = sg;
  c.prefsGen = pg;
  c.prefs = prefs;
  return c.snap;
}

bool listedInBrowser(const std::string& type) {
  if (type.find(':') == std::string::npos) return true;  // built-in
  if (isOwnSuiteType(type)) return false;
  auto snap = pluginGroups();
  bool all = false;
  {
    SnapshotCache& c = snapCache();
    std::lock_guard<std::mutex> g(c.lock);
    all = c.prefs.browserAllFormats;
  }
  if (all) return true;
  auto it = snap->groupOf.find(type);
  if (it == snap->groupOf.end()) return true;
  return snap->items[snap->groups[it->second].preferred].type == type;
}

}  // namespace gl::host
