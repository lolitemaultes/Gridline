// Gridline — "seltmann" pack: the Ribbon editor's pure pieces (see seltmann_editor.h). Core library:
// no UI, so the tests reach every rule here.
#include "seltmann_editor.h"
#include "../../core/I18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

namespace gl::packs::seltmann {

// ------------------------------------------------------------------ heights
int heightChoice(const std::string& name) {
  if (name == "compact") return HeightCompact;
  if (name == "standard") return HeightStandard;
  if (name == "full") return HeightFull;
  return -1;
}

const char* heightChoiceName(int choice) {
  switch (choice) {
    case HeightCompact: return "compact";
    case HeightStandard: return "standard";
    case HeightFull: return "full";
    default: return "auto";
  }
}

int heightClassFor(float hostH, int choice) {
  if (choice >= 0 && choice < HeightCount) return choice;
  if (hostH <= 0) return HeightFull;  // floating: the whole editor
  if (hostH < kCompactBelow) return HeightCompact;
  if (hostH < kStandardBelow) return HeightStandard;
  return HeightFull;
}

// ------------------------------------------------------------------ layouts
int resolveSlots(Model& m) {
  const int n = static_cast<int>(m.slices.size());
  int changed = 0;
  for (Step& s : m.pattern.steps) {
    if (!s.slot || s.src == kRest) continue;
    const int k = stepSlice(m.pattern, s, n);
    if (k < 0) continue;  // an unassigned slot stays as it is (it plays nothing either way)
    s.slot = false;
    s.src = k;
    ++changed;
  }
  return changed;
}

Model previewModel(const Model& m, const Sample* s, const Preset& p, uint32_t seed, std::vector<std::pair<int, float>>* params) {
  Model out = m;
  auto pr = applyPreset(out, s, p, seed);
  resolveSlots(out);
  if (params) *params = std::move(pr);
  return out;
}

std::vector<int> stepRegions(const Model& m, int maxSteps) {
  std::vector<int> v;
  const int n = static_cast<int>(m.slices.size());
  for (const Step& s : m.pattern.steps) {
    if (static_cast<int>(v.size()) >= maxSteps) break;
    v.push_back(s.src == kRest ? -1 : stepSlice(m.pattern, s, n));
  }
  return v;
}

bool LayoutPrefs::starred(const std::string& name) const { return std::find(stars.begin(), stars.end(), name) != stars.end(); }

void LayoutPrefs::toggleStar(const std::string& name) {
  auto it = std::find(stars.begin(), stars.end(), name);
  if (it != stars.end()) stars.erase(it);
  else if (!name.empty()) stars.push_back(name);
}

void LayoutPrefs::used(const std::string& name) {
  if (name.empty()) return;
  recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
  recent.insert(recent.begin(), name);
  if (recent.size() > kRecent) recent.resize(kRecent);
}

std::string layoutPrefsToText(const LayoutPrefs& p) {
  std::string out = "seltmann layouts 1\n";
  for (const auto& s : p.stars) out += "star " + s + "\n";
  for (const auto& r : p.recent) out += "recent " + r + "\n";
  return out;
}

LayoutPrefs layoutPrefsFromText(const std::string& text) {
  LayoutPrefs p;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("star ", 0) == 0 && line.size() > 5 && !p.starred(line.substr(5))) p.stars.push_back(line.substr(5));
    else if (line.rfind("recent ", 0) == 0 && line.size() > 7 && p.recent.size() < LayoutPrefs::kRecent) p.recent.push_back(line.substr(7));
  }
  return p;
}

bool saveLayoutPrefs(const std::string& file, const LayoutPrefs& p) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(file).parent_path(), ec);
  const std::string tmp = file + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << layoutPrefsToText(p);
    if (!o) return false;
  }
  fs::rename(tmp, file, ec);
  return !ec;
}

LayoutPrefs loadLayoutPrefs(const std::string& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return layoutPrefsFromText(ss.str());
}

// ------------------------------------------------------------------ key names
std::vector<std::string> keyNamesFor(int baseNote, int regions) {
  std::vector<std::string> v(128);
  baseNote = std::clamp(baseNote, 0, 127);
  for (int k = 0; k < regions && baseNote + k < 128; ++k) v[static_cast<size_t>(baseNote + k)] = regionName(k);
  for (int k = 0; k < 12; ++k) {
    const int key = baseNote - 12 + k;
    if (key < 0) continue;
    v[static_cast<size_t>(key)] = k == 0 ? std::string("Pattern") : "Pattern +" + std::to_string(k);
  }
  return v;
}

std::vector<std::string> instanceKeyNames(const PluginState& s) {
  if (s.type != kType) return {};
  const int base = s.params.size() > SmBaseNote ? static_cast<int>(std::lround(s.params[SmBaseNote])) : 60;
  const bool slots = s.params.size() > SmKeyMap && std::lround(s.params[SmKeyMap]) == KeySlots;
  // Parsing a model is not free: the last few answers are kept by the state's data and the two parameters.
  struct Entry {
    size_t data = 0;
    int base = 0;
    bool slots = false;
    std::vector<std::string> names;
  };
  static std::mutex mu;
  static std::vector<Entry> cache;
  const size_t h = std::hash<std::string>{}(s.data);
  {
    std::lock_guard<std::mutex> lk(mu);
    for (const Entry& e : cache)
      if (e.data == h && e.base == base && e.slots == slots) return e.names;
  }
  std::string path;
  Model m;
  std::vector<std::string> names;
  if (parseData(s.data, path, m) && !m.slices.empty()) {
    int regions = static_cast<int>(m.slices.size());
    if (slots) {  // keys play the slot letters: each key is named after the region its letter plays
      names.assign(128, std::string());
      const std::vector<std::string> pat = keyNamesFor(base, 0);
      for (int k = 0; k < 128; ++k) names[static_cast<size_t>(k)] = pat[static_cast<size_t>(k)];
      for (int k = 0; k < static_cast<int>(m.pattern.slots.size()) && base + k < 128; ++k) {
        const int r = m.pattern.slots[static_cast<size_t>(k)];
        if (r >= 0 && r < regions) names[static_cast<size_t>(base + k)] = regionName(r);
      }
    } else {
      names = keyNamesFor(base, regions);
    }
  }
  std::lock_guard<std::mutex> lk(mu);
  cache.push_back({h, base, slots, names});
  if (cache.size() > 8) cache.erase(cache.begin());
  return names;
}

// ------------------------------------------------------------------ linked send
uint64_t notesHash(const std::vector<Note>& notes) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](int64_t v) {
    for (int i = 0; i < 8; ++i) h = (h ^ static_cast<uint64_t>((v >> (i * 8)) & 0xFF)) * 1099511628211ull;
  };
  mix(static_cast<int64_t>(notes.size()));
  for (const Note& n : notes) {
    mix(n.start), mix(n.length), mix(n.pitch), mix(n.velocity), mix(n.pan), mix(n.fine), mix(n.release), mix(n.modX), mix(n.modY);
    mix((n.muted ? 1 : 0) | (n.slide ? 2 : 0) | (n.porta ? 4 : 0));
  }
  return h == 0 ? 1 : h;  // (0 means "no link written")
}

void writePatternNotes(const Song& s, gl::Pattern& p, ObjectId channel, const Model& m, const std::vector<float>& params, int cycles) {
  auto prm = [&](int k, float d) { return k < static_cast<int>(params.size()) ? params[static_cast<size_t>(k)] : d; };
  const int base = std::clamp(static_cast<int>(std::lround(prm(SmBaseNote, 60))), 0, 127);
  const int sp = std::clamp(static_cast<int>(std::lround(prm(SmSpeed, SpeedNormal))), 0, 2);
  const double speed = sp == SpeedHalf ? 0.5 : sp == SpeedDouble ? 2.0 : 1.0;
  const auto notes = patternNotes(m, base, prm(SmSwing, 0), prm(SmGate, 1));
  const double cycleB = cycleBeats(m) / speed;
  cycles = std::clamp(cycles, 1, 64);
  const double beatsPerBar = std::max<double>(1, static_cast<double>(s.barTicks()) / std::max(1, s.ppq));
  p.bars = std::clamp(static_cast<int>(std::ceil(cycleB * cycles / beatsPerBar - 1e-9)), 1, kMaxPatternBars);
  auto& row = p.notes[channel];
  row.clear();
  for (int c = 0; c < cycles; ++c)
    for (const auto& n : notes) {
      Note nn;
      nn.start = static_cast<Tick>(std::llround((c * cycleB + n.start / speed) * s.ppq));
      nn.length = std::max<Tick>(1, static_cast<Tick>(std::llround(n.length / speed * s.ppq)));
      nn.pitch = n.key;
      nn.velocity = n.velocity;
      nn.modX = n.modX;
      nn.modY = n.modY;
      row.push_back(nn);
    }
}

LinkState linkState(const gl::Pattern& p, ObjectId channel) {
  if (p.linkChannel == kNoId || p.linkChannel != channel) return LinkNone;
  auto it = p.notes.find(channel);
  const uint64_t h = notesHash(it == p.notes.end() ? std::vector<Note>{} : it->second);
  return h == p.linkHash ? LinkFollowing : LinkEdited;
}

bool refreshLinked(const Song& s, gl::Pattern& p, ObjectId channel, const Model& m, const std::vector<float>& params, bool relink) {
  const LinkState st = linkState(p, channel);
  if (st == LinkNone || (st == LinkEdited && !relink)) return false;
  gl::Pattern np = p;
  writePatternNotes(s, np, channel, m, params, std::max(1, p.linkCycles));
  np.linkHash = notesHash(np.notes[channel]);
  if (np == p) return false;
  p = std::move(np);
  return true;
}

// ------------------------------------------------------------------ takes
Model keepChopOnTake(const Model& m0, int64_t takeFrames, double takeRate) {
  Model m = m0;
  if (takeFrames <= 0 || takeRate <= 0) return m;
  // A take at another rate: positions in time stay (frames scale with the rate).
  const double k = m0.srcRate > 0 ? takeRate / m0.srcRate : 1.0;
  auto sc = [&](int64_t f) { return static_cast<int64_t>(std::llround(static_cast<double>(f) * k)); };
  if (std::fabs(k - 1.0) > 1e-12) {
    for (double& b : m.grid.beats) b *= k;
    m.regionStart = sc(m.regionStart), m.regionEnd = sc(m.regionEnd);
    for (Slice& s : m.slices) s.start = sc(s.start), s.end = sc(s.end);
    m.grid.contentStart = sc(m.grid.contentStart), m.grid.contentEnd = sc(m.grid.contentEnd);
  }
  m.srcFrames = takeFrames;
  m.srcRate = takeRate;
  m.grid.sampleRate = takeRate;
  m.grid.frames = takeFrames;
  m.grid.contentEnd = std::min<int64_t>(m.grid.contentEnd, takeFrames);
  m.regionEnd = std::min<int64_t>(m.regionEnd, takeFrames);
  if (m.regionStart >= m.regionEnd) {  // the region lies past the take: the whole take
    m.regionStart = 0;
    m.regionEnd = takeFrames;
  }
  std::vector<Slice> kept;
  for (Slice s : m.slices) {
    if (s.start >= m.regionEnd) break;
    s.start = std::max(s.start, m.regionStart);
    s.end = std::min(s.end, m.regionEnd);
    if (s.end > s.start) kept.push_back(s);
  }
  if (kept.empty()) {
    Slice s;
    s.start = m.regionStart;
    s.end = m.regionEnd;
    kept.push_back(s);
  }
  m.slices = std::move(kept);
  foldSteps(m);
  return m;
}

// ------------------------------------------------------------------ editor keys
keys::Chord editorChord(int key) {
  using keys::Key;
  switch (key) {
    case KeyPrevRegion: return {Key::Left, 0};
    case KeyNextRegion: return {Key::Right, 0};
    case KeyPlayRegion: return {Key::Enter, 0};
    case KeyPlayPattern: return {Key::Enter, keys::kShift};
    case KeySplit: return {Key::S, 0};
    case KeyJoin: return {Key::J, 0};
    case KeyReverseStep: return {Key::R, 0};
    case KeyChop1: return {Key::D1, 0};
    case KeyChop2: return {Key::D2, 0};
    case KeyChop3: return {Key::D3, 0};
    case KeyChop4: return {Key::D4, 0};
    case KeyChop5: return {Key::D5, 0};
    case KeyChop6: return {Key::D6, 0};
    case KeyToolPlay: return {Key::Escape, 0};
    default: return {};
  }
}

const char* editorKeyLabel(int key) {
  static const char* const n[KeyCount] = {i18n::N_("Previous region"), i18n::N_("Next region"), i18n::N_("Play the region"),
                                          i18n::N_("Play the pattern"), i18n::N_("Split the region in two"), i18n::N_("Join with the previous region"),
                                          i18n::N_("Play the step backwards"), i18n::N_("Chop: automatic"), i18n::N_("Chop: 1 bar"),
                                          i18n::N_("Chop: 1/2"), i18n::N_("Chop: 1/4"), i18n::N_("Chop: 1/8"), i18n::N_("Chop: 1/16"),
                                          i18n::N_("Back to the Play tool")};
  return key >= 0 && key < KeyCount ? n[key] : "";
}

bool editorKeyFree(const keys::Chord& c) {
  if (!c.valid()) return false;
  for (int p = 0; p < static_cast<int>(keys::Profile::Count); ++p)
    for (bool sep : {false, true})
      for (const auto& [id, chords] : keys::profileDefaults(static_cast<keys::Profile>(p), sep)) {
        const keys::Command* cmd = keys::findCommand(id);
        if (!cmd || cmd->context != keys::Context::Global) continue;
        for (const keys::Chord& ch : chords)
          if (ch == c) return false;
      }
  return true;
}

}  // namespace gl::packs::seltmann
