// Gridline — K-151: swing presets and grooves (see Groove.h).
#include "Groove.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Json.h"
#include "MidiFile.h"
#include "Presets.h"
#include "Project.h"
#include "flp/Flp.h"

namespace gl::groove {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ grids ----

const char* gridKey(int grid) {
  static const char* const k[] = {"step", "1/8", "1/16", "1/32"};
  return grid >= 0 && grid < kGridCount ? k[grid] : k[0];
}

int gridFromKey(const std::string& key) {
  for (int g = 0; g < kGridCount; ++g)
    if (key == gridKey(g)) return g;
  return kGridStep;
}

const char* gridName(int grid) {
  static const char* const n[] = {"Pattern step", "8ths", "16ths", "32nds"};
  return grid >= 0 && grid < kGridCount ? n[grid] : n[0];
}

bool validCycle(int cycle) {
  for (int c : kCycles)
    if (c == cycle) return true;
  return false;
}

namespace {
float finiteOr(float v, float d) { return std::isfinite(v) ? v : d; }
}  // namespace

void normalize(GrooveShape& s) {
  if (s.grid < 0 || s.grid >= kGridCount) s.grid = kGridStep;
  if (!validCycle(s.cycle)) {
    int best = 2;
    for (int c : kCycles)
      if (c <= std::max(2, s.cycle)) best = c;
    s.cycle = best;
  }
  const size_t n = static_cast<size_t>(s.cycle);
  s.timing.resize(n, 0.0f);
  for (float& t : s.timing) t = std::clamp(finiteOr(t, 0.0f), -0.5f, 0.5f);
  if (!s.velocity.empty()) {
    s.velocity.resize(n, 1.0f);
    for (float& v : s.velocity) v = std::clamp(finiteOr(v, 1.0f), 0.0f, 2.0f);
  }
}

void normalize(SongGroove& g) {
  normalize(g.shape);
  g.accent = std::clamp(finiteOr(g.accent, 1.0f), 0.0f, 1.0f);
  if (g.preset.size() > 200) g.preset.resize(200);
}

bool isPairSwing(const GrooveShape& s) {
  return s.cycle == 2 && s.timing.size() == 2 && s.timing[0] == 0.0f && s.timing[1] == 0.5f && s.velocity.empty();
}

double cellTicks(const Song& song, const Pattern* p, int grid) {
  const double q = static_cast<double>(song.ppq);
  switch (grid) {
    case kGrid8th: return q / 2.0;
    case kGrid16th: return q / 4.0;
    case kGrid32nd: return q / 8.0;
    default: return p ? static_cast<double>(song.ticksPerStep(*p)) : q / 4.0;
  }
}

// ------------------------------------------------------------------- warp ----

Warp makeWarp(const Song& song, const Pattern* p, const GrooveShape& shapeIn, double amount, double accent) {
  Warp w;
  GrooveShape shape = shapeIn;
  normalize(shape);
  const double cell = cellTicks(song, p, shape.grid);
  if (!(cell > 0)) return w;
  w.cell = cell;
  w.cycle = shape.cycle;
  // The step grid keeps the old swing's bar exactly (whole steps); note grids use the song's bar.
  if (shape.grid == kGridStep && p) w.bar = static_cast<double>(song.ticksPerStep(*p)) * static_cast<double>(p->stepsPerBar);
  else w.bar = static_cast<double>(p ? song.patternBarTicks(*p) : song.barTicks());  // K-179: the pattern's own bar
  if (!(w.bar > 0)) return w;
  amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.0;
  accent = std::isfinite(accent) ? std::clamp(accent, 0.0, 1.0) : 0.0;
  bool moves = false;
  for (int k = 0; k < w.cycle; ++k) {
    // cell * (k + offset): for the classic pair this is exactly the old swing's off-beat start.
    w.anchor[k] = cell * (static_cast<double>(k) + static_cast<double>(shape.timing[static_cast<size_t>(k)]) * amount);
    moves = moves || w.anchor[k] != cell * static_cast<double>(k);
  }
  const double cyc = cell * static_cast<double>(w.cycle);
  w.tailEnd = cyc;
  w.anchor[w.cycle] = cyc + w.anchor[0];  // the next cycle's first cell (moved with it)
  w.active = amount > 0 && moves;
  for (int k = 0; k < w.cycle; ++k) {
    float f = 1.0f;
    if (!shape.velocity.empty()) f = static_cast<float>(1.0 + (static_cast<double>(shape.velocity[static_cast<size_t>(k)]) - 1.0) * accent);
    w.vel[k] = f;
    w.accents = w.accents || f != 1.0f;
  }
  return w;
}

double Warp::map(double x) const {
  if (!active) return x;
  const double cyc = cell * static_cast<double>(cycle);
  const double barStart = std::floor(x / bar) * bar;
  const double u = x - barStart;
  const double cs = std::floor(u / cyc) * cyc;
  if (cs + cyc > bar + 1e-9) return x;  // a partial cycle at the bar's end stays straight
  const double v = u - cs;
  int k = 0;
  while (k + 1 < cycle && v >= static_cast<double>(k + 1) * cell) ++k;
  // The cycle's end: the next cycle's moved first cell, unless the next cycle is straight.
  const bool nextStraight = cs + 2.0 * cyc > bar + 1e-9 && std::fabs(bar - (cs + cyc)) > 1e-9;
  const double a0 = anchor[k];
  const double a1 = k + 1 < cycle ? anchor[k + 1] : (nextStraight ? tailEnd : anchor[cycle]);
  const double w = a0 + (v - static_cast<double>(k) * cell) * ((a1 - a0) / cell);
  return barStart + cs + w;
}

float Warp::velocityAt(double x) const {
  if (!accents) return 1.0f;
  const double cyc = cell * static_cast<double>(cycle);
  const double barStart = std::floor(x / bar) * bar;
  const double u = x - barStart;
  const double cs = std::floor(u / cyc) * cyc;
  if (cs + cyc > bar + 1e-9) return 1.0f;
  // The cell the note belongs to: the nearest cell start (a note played a little early keeps its accent).
  const double v = u - cs;
  int k = static_cast<int>(std::floor(v / cell + 0.5));
  k = ((k % cycle) + cycle) % cycle;
  return vel[k];
}

// ---------------------------------------------------------------- presets ----

namespace {

Preset factory(const char* name, int grid, int cycle, float amount, std::vector<float> timing, std::vector<float> velocity = {}) {
  Preset p;
  p.name = name;
  p.shape.grid = grid;
  p.shape.cycle = cycle;
  p.shape.timing = std::move(timing);
  p.shape.velocity = std::move(velocity);
  p.amount = amount;
  p.factory = true;
  normalize(p.shape);
  return p;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

const std::vector<Preset>& factoryPresets() {
  // Original presets. The amount is the share of a half cell the off-beat moves for a pair
  // (0.32: the off-beat at 58 % of the pair, 2/3: a triplet feel).
  static const std::vector<Preset> list = {
      factory("Straight", kGridStep, 2, 0.0f, {0.0f, 0.5f}),
      factory("Gentle 16ths", kGrid16th, 2, 0.16f, {0.0f, 0.5f}),
      factory("Easy shuffle", kGrid16th, 2, 0.32f, {0.0f, 0.5f}),
      factory("Deep pocket", kGrid16th, 2, 0.48f, {0.0f, 0.5f}, {1.0f, 0.9f}),
      factory("Triplet 16ths", kGrid16th, 2, 2.0f / 3.0f, {0.0f, 0.5f}),
      factory("Lilting 8ths", kGrid8th, 2, 0.32f, {0.0f, 0.5f}),
      factory("Triplet 8ths", kGrid8th, 2, 2.0f / 3.0f, {0.0f, 0.5f}, {1.0f, 0.8f}),
      factory("Behind the beat", kGrid16th, 4, 1.0f, {0.03f, 0.08f, 0.04f, 0.10f}),
      factory("Ahead of the beat", kGrid16th, 4, 1.0f, {-0.03f, -0.07f, -0.04f, -0.08f}),
      factory("Loose hands", kGrid16th, 8, 1.0f, {0.0f, 0.09f, -0.04f, 0.14f, 0.02f, 0.07f, -0.05f, 0.12f},
              {1.0f, 0.82f, 0.9f, 0.78f, 0.96f, 0.8f, 0.88f, 0.76f}),
      factory("Backbeat accents", kGrid16th, 8, 1.0f, {0, 0, 0, 0, 0, 0, 0, 0}, {0.85f, 0.7f, 0.85f, 0.7f, 1.0f, 0.7f, 0.85f, 0.7f}),
      factory("Late hats", kGrid16th, 4, 1.0f, {0.0f, 0.12f, 0.02f, 0.14f}, {1.0f, 0.7f, 0.85f, 0.7f}),
  };
  return list;
}

const Preset* findFactory(const std::string& name) {
  const std::string n = lower(name);
  // Names the piano roll's quantize grooves had before they joined the preset library.
  static const std::pair<const char*, const char*> older[] = {
      {"light swing", "Gentle 16ths"}, {"heavy swing", "Deep pocket"}, {"triplet shuffle", "Triplet 16ths"}};
  std::string want = n;
  for (const auto& [o, cur] : older)
    if (n == o) want = lower(cur);
  for (const Preset& p : factoryPresets())
    if (lower(p.name) == want) return &p;
  return nullptr;
}

std::string grooveFolder(const std::string& presetRoot) { return (fs::path(presetRoot) / "grooves").string(); }

std::vector<Preset> listUserPresets(const std::string& presetRoot) {
  std::vector<Preset> out;
  for (const PresetEntry& e : listGrooves(presetRoot)) {
    Preset p;
    if (!loadPresetFile(e.file, p)) continue;
    p.name = e.name;  // the file name is the preset's name in the library
    p.folder = e.folder;
    p.file = e.file;
    p.factory = false;
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<Preset> allPresets(const std::string& presetRoot) {
  std::vector<Preset> out = factoryPresets();
  for (Preset& p : listUserPresets(presetRoot)) out.push_back(std::move(p));
  return out;
}

bool findPreset(const std::string& presetRoot, const std::string& name, Preset& out) {
  if (name.empty()) return false;
  const std::string n = lower(name);
  if (!presetRoot.empty())
    for (Preset& p : listUserPresets(presetRoot)) {
      const std::string full = p.folder.empty() ? p.name : p.folder + "/" + p.name;
      if (lower(full) == n || lower(p.name) == n) {
        out = std::move(p);
        return true;
      }
    }
  if (const Preset* f = findFactory(name)) {
    out = *f;
    return true;
  }
  return false;
}

std::string toJsonText(const Preset& pIn) {
  Preset p = pIn;
  normalize(p.shape);
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-groove");
  j.object["version"] = Json::makeNumber(kFormatVersion);
  j.object["name"] = Json::makeString(p.name);
  j.object["grid"] = Json::makeString(gridKey(p.shape.grid));
  j.object["cycle"] = Json::makeNumber(p.shape.cycle);
  j.object["amount"] = Json::makeNumber(std::round(static_cast<double>(std::clamp(p.amount, 0.0f, 1.0f)) * 1e6) / 1e6);
  auto arr = [](const std::vector<float>& v) {
    Json a = Json::makeArray();
    for (float x : v) a.array.push_back(Json::makeNumber(std::round(static_cast<double>(x) * 1e6) / 1e6));
    return a;
  };
  j.object["timing"] = arr(p.shape.timing);
  if (!p.shape.velocity.empty()) j.object["velocity"] = arr(p.shape.velocity);
  if (p.accent != 1.0f) j.object["accent"] = Json::makeNumber(std::clamp(static_cast<double>(p.accent), 0.0, 1.0));
  if (!p.source.empty()) j.object["source"] = Json::makeString(p.source);
  return jsonStringify(j) + "\n";
}

bool fromJsonText(const std::string& text, Preset& out, std::string* err) {
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "not a JSON file";
    return false;
  }
  if (j.text("format") != "gridline-groove") {
    if (err) *err = "not a swing preset";
    return false;
  }
  const Json* tm = j.find("timing");
  if (!tm || !tm->is(Json::Type::Array)) {
    if (err) *err = "the swing preset has no timing";
    return false;
  }
  Preset p;
  p.name = j.text("name");
  p.source = j.text("source");
  p.shape.grid = gridFromKey(j.text("grid", "step"));
  p.shape.cycle = jsonInt<int>(j.num("cycle", static_cast<double>(tm->array.size())));
  auto floats = [](const Json& a, float dflt) {
    std::vector<float> v;
    for (const Json& x : a.array) v.push_back(x.is(Json::Type::Number) ? static_cast<float>(x.number) : dflt);
    return v;
  };
  p.shape.timing = floats(*tm, 0.0f);
  if (const Json* v = j.find("velocity"); v && v->is(Json::Type::Array) && !v->array.empty()) p.shape.velocity = floats(*v, 1.0f);
  const double am = j.num("amount", 1.0);
  p.amount = static_cast<float>(std::isfinite(am) ? std::clamp(am, 0.0, 1.0) : 1.0);
  const double ac = j.num("accent", 1.0);
  p.accent = static_cast<float>(std::isfinite(ac) ? std::clamp(ac, 0.0, 1.0) : 1.0);
  normalize(p.shape);
  out = std::move(p);
  return true;
}

bool loadPresetFile(const std::string& file, Preset& out, std::string* err) {
  std::ifstream in(fs::u8path(file), std::ios::binary);
  if (!in) {
    if (err) *err = "cannot read " + file;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  if (ss.str().size() > (1u << 20)) {
    if (err) *err = "the file is too large for a swing preset";
    return false;
  }
  if (!fromJsonText(ss.str(), out, err)) return false;
  if (out.name.empty()) out.name = fs::u8path(file).stem().u8string();
  return true;
}

bool savePresetFile(const std::string& file, const Preset& p, std::string* err) {
  std::error_code ec;
  const fs::path f = fs::u8path(file);
  if (f.has_parent_path()) fs::create_directories(f.parent_path(), ec);
  return writeFileAtomic(file, toJsonText(p), err);
}

bool saveUserPreset(const std::string& presetRoot, const Preset& p, const std::string& relName, std::string* path, std::string* err) {
  fs::path f = fs::u8path(grooveFolder(presetRoot));
  std::string part, rel = relName.empty() ? p.name : relName;
  std::vector<std::string> parts;
  for (char c : rel + "/") {
    if (c == '/' || c == '\\') {
      if (!part.empty() && part != "." && part != "..") parts.push_back(safePresetName(part));
      part.clear();
    } else {
      part += c;
    }
  }
  if (parts.empty()) parts.push_back(safePresetName(p.name));
  for (size_t i = 0; i + 1 < parts.size(); ++i) f /= fs::u8path(parts[i]);
  f /= fs::u8path(parts.back() + ".json");
  Preset q = p;
  q.name = parts.back();
  if (!savePresetFile(f.u8string(), q, err)) return false;
  if (path) *path = f.u8string();
  return true;
}

bool deleteUserPreset(const Preset& p) {
  if (p.factory || p.file.empty()) return false;
  std::error_code ec;
  return fs::remove(fs::u8path(p.file), ec);
}

Preset fromSong(const Song& song, const std::string& name) {
  Preset p;
  p.name = name;
  p.shape = song.groove.shape;
  normalize(p.shape);
  p.amount = std::clamp(song.swing, 0.0f, 1.0f);
  p.accent = song.groove.accent;
  return p;
}

void apply(Song& song, const Preset& p) {
  GrooveShape s = p.shape;
  normalize(s);
  song.swing = std::clamp(std::isfinite(p.amount) ? p.amount : 0.0f, 0.0f, 1.0f);
  song.groove.shape = s;
  song.groove.accent = std::clamp(std::isfinite(p.accent) ? p.accent : 1.0f, 0.0f, 1.0f);
  song.groove.preset = p.folder.empty() ? p.name : p.folder + "/" + p.name;
}

bool matches(const Song& song, const Preset& p) {
  GrooveShape s = p.shape;
  normalize(s);
  return song.swing == std::clamp(p.amount, 0.0f, 1.0f) && song.groove.shape == s && song.groove.accent == p.accent;
}

const Preset* findIn(const std::vector<Preset>& presets, const std::string& name) {
  if (name.empty()) return nullptr;
  const std::string n = lower(name);
  for (const Preset& p : presets)  // the user's first (they win over a factory preset of the same name)
    if (!p.factory && lower(p.folder.empty() ? p.name : p.folder + "/" + p.name) == n) return &p;
  for (const Preset& p : presets)
    if (p.factory && lower(p.name) == n) return &p;
  return findFactory(name);
}

std::string chipLabel(const Song& song, const std::vector<Preset>& presets) {
  const SongGroove& g = song.groove;
  if (!g.preset.empty()) {
    const Preset* p = findIn(presets, g.preset);
    return p && !matches(song, *p) ? g.preset + " (edited)" : g.preset;
  }
  if (g.shape == GrooveShape{}) return song.swing > 0 ? "Step swing" : "Straight";
  return "Custom groove";
}

std::string feelText(const GrooveShape& s, double amount) {
  char buf[160];
  const std::string grid = s.grid == kGridStep ? "pattern steps" : gridName(s.grid);
  if (isPairSwing(s)) {
    if (!(amount > 0)) return "Straight " + grid;
    std::snprintf(buf, sizeof buf, "%s, off-beat at %.0f %% of the pair", grid.c_str(), 50.0 + 25.0 * std::clamp(amount, 0.0, 1.0));
    std::string t = buf;
    if (t[0] >= 'a' && t[0] <= 'z') t[0] = static_cast<char>(t[0] - 'a' + 'A');
    return t;
  }
  std::snprintf(buf, sizeof buf, "%d-cell groove on %s%s", s.cycle, grid.c_str(), s.velocity.empty() ? "" : ", with accents");
  return buf;
}

// ------------------------------------------------------------- extraction ----

std::vector<Onset> onsetsOfNotes(const std::vector<Note>& notes) {
  std::vector<Onset> out;
  out.reserve(notes.size());
  for (const Note& n : notes)
    if (!n.muted) out.push_back({static_cast<double>(n.start) + static_cast<double>(n.frac), static_cast<float>(n.velocity)});
  return out;
}

Preset extract(const std::vector<Onset>& onsets, double cell, double bar, int grid, int cycle, const std::string& name) {
  Preset p;
  p.name = name;
  p.shape.grid = grid;
  p.shape.cycle = validCycle(cycle) ? cycle : 2;
  const size_t n = static_cast<size_t>(p.shape.cycle);
  std::vector<double> sum(n, 0.0), lvl(n, 0.0);
  std::vector<int> cnt(n, 0);
  if (!(cell > 0) || !(bar > 0)) {
    p.shape.timing.assign(n, 0.0f);
    return p;
  }
  const double cyc = cell * static_cast<double>(n);
  for (const Onset& o : onsets) {
    if (!std::isfinite(o.tick)) continue;
    const double barStart = std::floor(o.tick / bar) * bar;
    const double u = o.tick - barStart;
    double line = std::floor(u / cell + 0.5);  // nearest cell start
    double off = (u - line * cell) / cell;
    if (line * cell >= bar - 1e-9) line = 0;  // early for the next bar's first cell
    const double cs = std::floor(line * cell / cyc) * cyc;
    if (cs + cyc > bar + 1e-9) continue;  // partial cycle at the bar's end: not part of the groove
    if (std::fabs(off) > 0.5) continue;
    const size_t k = static_cast<size_t>(static_cast<long long>(line) % static_cast<long long>(n));
    sum[k] += off;
    lvl[k] += std::max(0.0f, o.level);
    ++cnt[k];
  }
  // A shorter cycle when the longer one repeats (cells with no onset match anything).
  int use = static_cast<int>(n);
  for (int c : kCycles) {
    if (c >= use || static_cast<int>(n) % c != 0) continue;
    bool same = true;
    for (size_t k = 0; k < n && same; ++k) {
      const size_t r = k % static_cast<size_t>(c);
      for (size_t m = r; m < n && same; m += static_cast<size_t>(c)) {
        if (!cnt[k] || !cnt[m]) continue;
        same = std::fabs(sum[k] / cnt[k] - sum[m] / cnt[m]) < 0.02;
      }
    }
    if (same) {
      use = c;
      break;
    }
  }
  std::vector<double> t(static_cast<size_t>(use), 0.0), lv(static_cast<size_t>(use), 0.0);
  std::vector<int> tc(static_cast<size_t>(use), 0);
  for (size_t k = 0; k < n; ++k) {
    const size_t r = k % static_cast<size_t>(use);
    t[r] += sum[k];
    lv[r] += lvl[k];
    tc[r] += cnt[k];
  }
  for (size_t r = 0; r < t.size(); ++r)
    if (tc[r]) t[r] /= tc[r], lv[r] /= tc[r];
  // Relative to the downbeat: a clip that starts a little late (or a detector's delay) is not a groove.
  if (tc[0]) {
    const double base = t[0];
    for (size_t r = 0; r < t.size(); ++r)
      if (tc[r]) t[r] = std::clamp(t[r] - base, -0.5, 0.5);
  }
  double big = 0, loud = 0;
  size_t bigAt = 0;
  for (size_t r = 0; r < t.size(); ++r) {
    if (std::fabs(t[r]) > big) big = std::fabs(t[r]), bigAt = r;
    loud = std::max(loud, lv[r]);
  }
  p.shape.cycle = use;
  p.shape.timing.assign(static_cast<size_t>(use), 0.0f);
  if (big > 1e-6) {
    // Scale so the largest offset is half a cell; the amount carries the size.
    for (size_t r = 0; r < t.size(); ++r) p.shape.timing[r] = static_cast<float>(t[r] * 0.5 / big);
    p.shape.timing[bigAt] = t[bigAt] < 0 ? -0.5f : 0.5f;
    p.amount = static_cast<float>(std::min(1.0, big * 2.0));
  }
  if (loud > 0) {
    std::vector<float> v(static_cast<size_t>(use), 1.0f);
    bool varies = false;
    for (size_t r = 0; r < v.size(); ++r) {
      if (!tc[r]) continue;
      // (A taken groove makes a note quieter, never silent: at least a tenth of its velocity.)
      v[r] = static_cast<float>(std::max(0.1, std::round(lv[r] / loud * 1000.0) / 1000.0));
      varies = varies || v[r] < 0.97f;
    }
    if (varies) p.shape.velocity = v;
  }
  normalize(p.shape);
  return p;
}

// --------------------------------------------------------------- FL Studio ----

double flSwing(const Song& song, bool* exact) {
  const SongGroove& g = song.groove;
  const bool accents = !g.shape.velocity.empty() && g.accent > 0;
  bool ok = !accents;
  double v = 0;
  if (!(song.swing > 0)) {
    v = 0;
  } else if (isPairSwing(g.shape)) {
    v = song.swing;
    if (g.shape.grid != kGridStep) {
      // Exact only when every pattern's step is the groove's cell.
      const double cell = cellTicks(song, nullptr, g.shape.grid);
      for (const Pattern& p : song.patterns) ok = ok && std::fabs(static_cast<double>(song.ticksPerStep(p)) - cell) < 1e-9;
    }
  } else {
    bool moves = false;
    for (float t : g.shape.timing) moves = moves || t != 0.0f;
    ok = ok && !moves;
    v = 0;
  }
  if (exact) *exact = ok;
  return std::clamp(v, 0.0, 1.0);
}

bool fromScoreFile(const std::string& file, Preset& out, std::string* err) {
  flp::File f;
  if (!flp::readFile(file, f, err)) return false;
  std::vector<Onset> on;
  for (const flp::Event& e : f.events) {
    if (e.id != 224) continue;
    for (uint32_t o = 0; o + 24 <= e.size; o += 24) on.push_back({static_cast<double>(e.u32(o)), static_cast<float>(e.u8(o + 21))});
  }
  if (on.empty()) {
    if (err) *err = "the score has no notes";
    return false;
  }
  const std::string stem = fs::u8path(file).stem().u8string();
  const int grid = stem.rfind("8_", 0) == 0 ? kGrid8th : kGrid16th;
  const double ppq = f.ppq > 0 ? f.ppq : 96;
  const double cell = grid == kGrid8th ? ppq / 2.0 : ppq / 4.0;
  out = extract(on, cell, ppq * 4.0, grid, grid == kGrid8th ? 8 : 16, stem);
  out.source = "score file " + fs::u8path(file).filename().u8string();
  return true;
}

bool fromMidiFile(const std::string& file, int grid, Preset& out, std::string* err) {
  MidiFileData m;
  if (!readMidiFile(file, m, err)) return false;
  std::vector<Onset> on;
  for (const MidiTrack& t : m.tracks)
    for (const MidiNote& n : t.notes) on.push_back({static_cast<double>(n.start), static_cast<float>(n.velocity)});
  if (on.empty()) {
    if (err) *err = "the MIDI file has no notes";
    return false;
  }
  if (grid == kGridStep) grid = kGrid16th;
  const double q = std::max(1, m.division);
  const double cell = grid == kGrid8th ? q / 2.0 : grid == kGrid32nd ? q / 8.0 : q / 4.0;
  const double bar = q * 4.0 * std::max(1, m.timeSigNum) / std::max(1, m.timeSigDen);
  const int cells = static_cast<int>(std::lround(bar / cell));
  int cycle = 2;
  for (int c : kCycles)
    if (c <= cells && cells % c == 0) cycle = c;
  out = extract(on, cell, bar, grid, cycle, fs::u8path(file).stem().u8string());
  out.source = "MIDI file " + fs::u8path(file).filename().u8string();
  return true;
}

bool importFile(const std::string& file, Preset& out, std::string* err) {
  std::string ext = fs::u8path(file).extension().u8string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".mid" || ext == ".midi") return fromMidiFile(file, kGrid16th, out, err);
  if (ext == ".fsc") return fromScoreFile(file, out, err);
  return loadPresetFile(file, out, err);
}

}  // namespace gl::groove
