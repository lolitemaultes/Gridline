// Gridline — preset library on disk (see Presets.h).
#include "Presets.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "AutomationEdit.h"
#include "Json.h"
#include "Project.h"

namespace gl {

namespace fs = std::filesystem;

namespace {

std::vector<PresetEntry> listDir(const fs::path& base) {
  std::vector<PresetEntry> out;
  std::error_code ec;
  if (!fs::is_directory(base, ec)) return out;
  for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
    PresetEntry e;
    e.name = it->path().stem().string();
    e.folder = fs::relative(it->path().parent_path(), base, ec).generic_string();
    if (e.folder == ".") e.folder.clear();
    e.file = it->path().string();
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) {
    // Top-level (user) presets first, then folders alphabetically.
    if (a.folder.empty() != b.folder.empty()) return a.folder.empty();
    return a.folder != b.folder ? a.folder < b.folder : a.name < b.name;
  });
  return out;
}

bool readJson(const std::string& file, Json& j) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  return jsonParse(ss.str(), j);
}

Json stateJson(const PluginState& st) {
  Json j = Json::makeObject();
  j.object["type"] = Json::makeString(st.type);
  Json p = Json::makeArray();
  for (float v : st.params) p.array.push_back(Json::makeNumber(v));
  j.object["params"] = std::move(p);
  if (!st.data.empty()) j.object["data"] = Json::makeString(st.data);
  return j;
}

PluginState stateFrom(const Json& j) {
  PluginState s = defaultPluginState(j.text("type"));
  if (const Json* p = j.find("params"); p && p->is(Json::Type::Array))
    for (size_t i = 0; i < p->array.size() && i < s.params.size(); ++i)
      if (p->array[i].is(Json::Type::Number)) s.params[i] = static_cast<float>(p->array[i].number);
  s.data = j.text("data");
  normalizePluginState(s);
  return s;
}

}  // namespace

std::string presetRoot(const std::string& configDir) { return (fs::path(configDir) / "presets").string(); }

std::string safePresetName(const std::string& name) {
  std::string s;
  for (unsigned char ch : name) {
    bool ok = std::isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '.' || ch == '(' || ch == ')' || ch == '&' ||
              ch == '#' || ch == '+' || ch == ',' || ch >= 0x80;
    s += ok ? static_cast<char>(ch) : '_';
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s.empty() ? "Preset" : s;
}

std::vector<PresetEntry> listPresets(const std::string& root, const std::string& type) {
  return listDir(fs::path(root) / type);
}

bool loadPreset(const std::string& file, const std::string& type, PluginState& out) {
  Json j;
  if (!readJson(file, j) || j.text("type") != type) return false;
  out = stateFrom(j);
  return true;
}

bool savePreset(const std::string& root, const PluginState& st, const std::string& relName, const std::string& source, bool exact) {
  fs::path file = fs::path(root) / st.type / (relName + ".json");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  Json j = stateJson(st);
  j.object["format"] = Json::makeString("gridline-preset");
  if (!source.empty()) j.object["source"] = Json::makeString(source);
  if (!exact) {
    Json t;
    t.type = Json::Type::Bool;
    t.boolean = true;
    j.object["approximate"] = t;
  }
  return writeFileAtomic(file.string(), jsonStringify(j) + "\n");
}

std::vector<PresetEntry> listChains(const std::string& root) { return listDir(fs::path(root) / "chains"); }

bool loadChain(const std::string& file, ChainPreset& out) {
  Json j;
  if (!readJson(file, j) || j.text("format") != "gridline-chain") return false;
  out = ChainPreset{};
  out.source = j.text("source");
  if (const Json* a = j.find("slots"); a && a->is(Json::Type::Array))
    for (const auto& s : a->array) {
      out.slots.push_back(s.text("type").empty() ? PluginState{} : stateFrom(s));
      out.mix.push_back(static_cast<float>(s.num("mix", 1.0)));
    }
  return true;
}

bool saveChain(const std::string& root, const ChainPreset& chain, const std::string& relName) {
  fs::path file = fs::path(root) / "chains" / (relName + ".json");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-chain");
  if (!chain.source.empty()) j.object["source"] = Json::makeString(chain.source);
  Json a = Json::makeArray();
  for (size_t i = 0; i < chain.slots.size(); ++i) {
    Json s = chain.slots[i].type.empty() ? Json::makeObject() : stateJson(chain.slots[i]);
    s.object["mix"] = Json::makeNumber(i < chain.mix.size() ? chain.mix[i] : 1.0f);
    a.array.push_back(std::move(s));
  }
  j.object["slots"] = std::move(a);
  return writeFileAtomic(file.string(), jsonStringify(j) + "\n");
}

std::vector<PresetEntry> listAutomationShapes(const std::string& root) { return listDir(fs::path(root) / "automation"); }

bool loadAutomationShape(const std::string& file, int ppq, AutomationShape& out) {
  Json j;
  if (!readJson(file, j) || j.text("format") != "gridline-automation-shape") return false;
  out = AutomationShape{};
  out.source = j.text("source");
  const double q = std::max(1, ppq);
  if (const Json* a = j.find("points"); a && a->is(Json::Type::Array))
    for (const auto& jp : a->array) {
      if (!jp.is(Json::Type::Array) || jp.array.size() < 2) continue;
      auto at = [&](size_t i, double d) { return i < jp.array.size() && jp.array[i].is(Json::Type::Number) ? jp.array[i].number : d; };
      AutoPoint p;
      p.pos = std::max<Tick>(0, static_cast<Tick>(std::llround(at(0, 0) * q)));
      p.value = static_cast<float>(std::clamp(at(1, 0.5), 0.0, 1.0));
      p.tension = static_cast<float>(std::clamp(at(2, 0), -1.0, 1.0));
      if (jp.array.size() > 3 && jp.array[3].is(Json::Type::String)) p.shape = autoedit::shapeFromKey(jp.array[3].str);
      out.points.push_back(p);
    }
  std::stable_sort(out.points.begin(), out.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  if (!out.points.empty()) {
    Tick base = out.points.front().pos;
    for (auto& p : out.points) p.pos -= base;
  }
  return !out.points.empty();
}

bool saveAutomationShape(const std::string& root, const AutomationShape& shape, int ppq, const std::string& relName) {
  if (shape.points.empty()) return false;
  fs::path file = fs::path(root) / "automation" / (relName + ".json");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-automation-shape");
  if (!shape.source.empty()) j.object["source"] = Json::makeString(shape.source);
  const double q = std::max(1, ppq);
  const Tick base = shape.points.front().pos;
  Json a = Json::makeArray();
  for (const auto& p : shape.points) {
    Json jp = Json::makeArray();  // [beats, value, tension, curve]
    jp.array.push_back(Json::makeNumber(static_cast<double>(p.pos - base) / q));
    jp.array.push_back(Json::makeNumber(p.value));
    jp.array.push_back(Json::makeNumber(p.tension));
    jp.array.push_back(Json::makeString(autoedit::shapeKey(p.shape)));
    a.array.push_back(std::move(jp));
  }
  j.object["points"] = std::move(a);
  return writeFileAtomic(file.string(), jsonStringify(j) + "\n");
}

}  // namespace gl
