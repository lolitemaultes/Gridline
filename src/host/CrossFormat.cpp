// Gridline: a project's third-party plugin opening in another installed format (K-78). See CrossFormat.h.
#include "CrossFormat.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include "../core/Params.h"
#include "../core/PluginStandIn.h"
#include "../core/Project.h"
#include "../core/Song.h"
#include "HostedPlugins.h"
#include "PluginGroups.h"
#include "PluginPrefs.h"
#include "StateConvert.h"

namespace gl::host::xformat {

namespace {

constexpr size_t kMaxSavedParamNames = 4096;  // larger plugins rely on their state alone

// "<format>:<id>" for any plugin format a project can name, whether or not this platform hosts it (a
// project from a Mac names AUs on Linux and Windows too).
bool knownType(const std::string& type, std::string& format, std::string& uid) {
  const size_t c = type.find(':');
  if (c == std::string::npos || c == 0 || c + 1 >= type.size()) return false;
  format = type.substr(0, c);
  uid = type.substr(c + 1);
  return format == "vst3" || format == "vst2" || format == "au" || format == "clap" || format == "lv2";
}

const char* formatLabelOf(const std::string& type) {
  std::string f, u;
  if (!knownType(type, f, u)) return "plugin";
  if (f == "vst3") return "VST3";
  if (f == "vst2") return "VST2";
  if (f == "au") return "AU";
  if (f == "clap") return "CLAP";
  if (f == "lv2") return "LV2";
  return "plugin";
}

bool skipClass(const HostedClass& c) { return standin::hidden(c.type, c.name); }

bool hostedType(const std::string& type) {
  std::string f, u;
  return knownType(type, f, u);
}

// Registered types of the same plugin in other formats (identity aliases), cached per class set.
std::vector<std::string> aliasesOf(const HostedClass& c) {
  static std::mutex m;
  static uint64_t gen = ~uint64_t(0);
  static std::map<std::string, std::vector<std::string>> cache;
  const uint64_t g = hostedClassesGeneration();
  {
    std::lock_guard<std::mutex> lk(m);
    if (g != gen) {
      cache.clear();
      gen = g;
    }
    if (auto it = cache.find(c.type); it != cache.end()) return it->second;
  }
  std::vector<std::string> out;
  for (const auto& cd : ident::rank(ident::identityOf(c), hostedClasses(), {}, nullptr))
    if (cd.confidence >= ident::Confidence::Medium) out.push_back(cd.cls->type);
  std::sort(out.begin(), out.end());
  std::lock_guard<std::mutex> lk(m);
  cache[c.type] = out;
  return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Saved parameter k in 0..1.
double savedNormalized(const PluginState& saved, const ident::Identity& id, size_t k, const HostedClass& target, int t, bool byPosition) {
  const double v = k < saved.params.size() ? saved.params[k] : 0.0;
  if (byPosition) return target.toNormalized(static_cast<size_t>(t), v);
  const int steps = k < id.paramSteps.size() ? id.paramSteps[k] : 0;
  if (steps > 1) return std::clamp(v / steps, 0.0, 1.0);
  return std::clamp(v, 0.0, 1.0);
}

std::mutex checksMutex;
std::vector<std::string> checks;

// ---- the project document ----
struct Slot {
  Json* plugin;
  double owner;
  uint32_t base, end;
  std::string name;
};

std::vector<Slot> slots(Json& project) {
  std::vector<Slot> out;
  if (auto c = project.object.find("channels"); c != project.object.end() && c->second.is(Json::Type::Array))
    for (auto& ch : c->second.array)
      if (ch.is(Json::Type::Object) && ch.object.count("plugin"))
        out.push_back({&ch.object["plugin"], ch.num("id", 0), kChannelPluginBase, kNoteFxBase, ch.text("name")});
  if (auto m = project.object.find("mixer"); m != project.object.end() && m->second.is(Json::Type::Array))
    for (auto& tr : m->second.array) {
      if (!tr.is(Json::Type::Object)) continue;
      auto s = tr.object.find("slots");
      if (s == tr.object.end() || !s->second.is(Json::Type::Array)) continue;
      for (size_t i = 0; i < s->second.array.size(); ++i) {
        Json& sl = s->second.array[i];
        if (!sl.is(Json::Type::Object) || !sl.object.count("plugin")) continue;
        const uint32_t base = kSlotBase + static_cast<uint32_t>(i) * kSlotStride;
        out.push_back({&sl.object["plugin"], tr.num("id", 0), base, base + kSlotEnable, tr.text("name")});
      }
    }
  return out;
}

// Automation, links and pattern automation of a slot follow its parameters to their new indices.
// Items of parameters the new format does not have target nothing (the clip stays).
void remapAutomation(Json& project, const Slot& s, const std::vector<int>& map) {
  std::vector<Json*> items;
  for (const char* key : {"automations", "links", "modLinks"})
    if (auto a = project.object.find(key); a != project.object.end() && a->second.is(Json::Type::Array))
      for (auto& it : a->second.array)
        if (it.is(Json::Type::Object)) items.push_back(&it);
  if (auto pit = project.object.find("patterns"); pit != project.object.end() && pit->second.is(Json::Type::Array))
    for (auto& p : pit->second.array)
      if (p.is(Json::Type::Object))
        if (auto at = p.object.find("automation"); at != p.object.end() && at->second.is(Json::Type::Array))
          for (auto& it : at->second.array)
            if (it.is(Json::Type::Object)) items.push_back(&it);
  for (Json* it : items) {
    if (it->num("owner", -1) != s.owner) continue;
    const double idx = it->num("index", -1);
    if (idx < s.base || idx >= s.end) continue;
    const size_t k = static_cast<size_t>(idx - s.base);
    const int to = k < map.size() ? map[k] : -1;
    if (to >= 0) it->object["index"] = Json::makeNumber(s.base + static_cast<uint32_t>(to));
    else it->object["owner"] = Json::makeNumber(static_cast<double>(kNoId));
  }
}

void setLoad(Json& plugin, const ident::Identity& id, const LoadNote& n) {
  Json* ij = nullptr;
  if (auto it = plugin.object.find("ident"); it != plugin.object.end() && it->second.is(Json::Type::Object)) ij = &it->second;
  if (!ij) {
    plugin.object["ident"] = ident::toJson(id);
    ij = &plugin.object["ident"];
  }
  ij->object["load"] = toJson(n);
}

// Converts one saved state in place (`p` a plugin object). True when it was replaced.
bool resolvePlugin(Json& project, const Slot* s, Json& p, std::vector<std::string>* report, bool remap) {
  PluginState st = pluginStateFromJson(p);
  if (!hostedType(st.type) || installed(st.type)) return false;
  const ident::Identity id = savedIdentity(st, s ? s->name : std::string());
  const std::vector<ident::Candidate> ranked = candidatesFor(id);
  const ident::Candidate* pick = ident::autoPick(ranked);
  if (!pick) {
    // A stand-in's record of its plugin stays exactly as saved, and so does a plugin the stand-in step
    // is about to replace (its record becomes that stand-in's original).
    if (!remap || standin::familyOf(st.type, id.name, id.vendor) != standin::Family::None) return false;
    LoadNote n;
    n.from = st.type;
    n.name = id.name.empty() ? (s ? s->name : std::string()) : id.name;
    n.via = "missing";
    for (const auto& c : ranked) {
      if (n.suggest.size() >= 6) break;
      n.suggest.push_back(c.cls->type);
    }
    if (!ranked.empty()) n.confidence = ident::confidenceName(ranked.front().confidence);
    setLoad(p, id, n);
    if (report && !ranked.empty()) report->push_back(describe(n));
    return false;
  }
  Conversion cv;
  if (!convert(st, id, *pick->cls, pick->confidence, cv)) return false;
  Json nj = pluginStateToJson(cv.state);
  if (!remap) {  // a stand-in's record of its plugin: only the plugin, its values and its state change
    for (const char* k : {"type", "params", "data", "ident"}) {
      if (auto it = nj.object.find(k); it != nj.object.end()) p.object[k] = it->second;
      else p.object.erase(k);
    }
    if (report) report->push_back(describe(cv.note));
    return true;
  }
  if (auto o = p.object.find("original"); o != p.object.end()) nj.object["original"] = o->second;
  ident::Identity nid;
  if (auto ij = nj.object.find("ident"); ij != nj.object.end()) ident::fromJson(ij->second, nid);
  setLoad(nj, nid, cv.note);
  p = std::move(nj);
  if (remap && s) remapAutomation(project, *s, cv.indexMap);
  if (report) report->push_back(describe(cv.note));
  return true;
}

}  // namespace

// ---- load notes ------------------------------------------------------------------------------

Json toJson(const LoadNote& n) {
  Json j = Json::makeObject();
  j.object["from"] = Json::makeString(n.from);
  if (!n.to.empty()) j.object["to"] = Json::makeString(n.to);
  if (!n.name.empty()) j.object["name"] = Json::makeString(n.name);
  if (!n.confidence.empty()) j.object["confidence"] = Json::makeString(n.confidence);
  j.object["via"] = Json::makeString(n.via);
  if (!n.how.empty()) j.object["how"] = Json::makeString(n.how);
  j.object["matched"] = Json::makeNumber(n.matched);
  j.object["total"] = Json::makeNumber(n.total);
  if (!n.verify.empty()) {
    Json a = Json::makeArray();
    for (int k : n.verify) a.array.push_back(Json::makeNumber(k));
    j.object["verify"] = std::move(a);
  }
  if (!n.suggest.empty()) {
    Json a = Json::makeArray();
    for (const auto& t : n.suggest) a.array.push_back(Json::makeString(t));
    j.object["suggest"] = std::move(a);
  }
  return j;
}

bool fromJson(const Json& j, LoadNote& n) {
  n = LoadNote{};
  if (!j.is(Json::Type::Object)) return false;
  n.from = j.text("from");
  n.to = j.text("to");
  n.name = j.text("name");
  n.confidence = j.text("confidence");
  n.via = j.text("via");
  n.how = j.text("how");
  n.matched = static_cast<int>(j.num("matched", 0));
  n.total = static_cast<int>(j.num("total", 0));
  if (const Json* a = j.find("verify"); a && a->is(Json::Type::Array))
    for (const auto& e : a->array)
      if (e.is(Json::Type::Number) && e.number >= 0 && e.number < 1e6) n.verify.push_back(static_cast<int>(e.number));
  if (const Json* a = j.find("suggest"); a && a->is(Json::Type::Array))
    for (const auto& e : a->array)
      if (e.is(Json::Type::String)) n.suggest.push_back(e.str);
  return !n.from.empty();
}

bool loadNote(const PluginState& st, LoadNote& out) {
  if (st.ident.empty()) return false;
  Json j;
  if (!jsonParse(st.ident, j) || !j.is(Json::Type::Object)) return false;
  const Json* l = j.find("load");
  return l && fromJson(*l, out);
}

bool installed(const std::string& type) {
  auto c = hostedClass(type);
  return c && !standin::hidden(type, c->name);
}

// ---- identities --------------------------------------------------------------------------------

Json identityForSave(const PluginState& st) {
  if (!hostedType(st.type)) return Json{};
  ident::Identity stored;
  if (!st.ident.empty()) {
    Json j;
    if (jsonParse(st.ident, j)) ident::fromJson(j, stored);
  }
  ident::Identity id;
  if (auto c = hostedClass(st.type)) {
    id = ident::identityOf(*c);
    id.aliases = aliasesOf(*c);
    if (stored.type == st.type || contains(stored.aliases, st.type)) {  // what the record knew of the same plugin
      if (!stored.type.empty() && stored.type != st.type && !contains(id.aliases, stored.type)) id.aliases.push_back(stored.type);
      for (const auto& a : stored.aliases)
        if (a != st.type && !contains(id.aliases, a)) id.aliases.push_back(a);
    }
  } else if (stored.type == st.type) {
    id = stored;  // not installed here: the record stays as it came
  } else {
    id.type = st.type;
  }
  id.aliases.erase(std::remove(id.aliases.begin(), id.aliases.end(), st.type), id.aliases.end());
  if (id.paramNames.size() > kMaxSavedParamNames) {
    id.paramNames.clear();
    id.paramSteps.clear();
  }
  return ident::toJson(id);
}

ident::Identity savedIdentity(const PluginState& st, const std::string& hintName) {
  ident::Identity id;
  if (!st.ident.empty()) {
    Json j;
    if (jsonParse(st.ident, j) && ident::fromJson(j, id) && id.type == st.type) return id;
    id = ident::Identity{};
  }
  if (auto c = hostedClass(st.type)) return ident::identityOf(*c);
  id.type = st.type;
  id.name = hintName;  // a channel's name is often the plugin's (weak: never more than a Low match alone)
  return id;
}

std::vector<ident::Candidate> candidatesFor(const ident::Identity& id) {
  return ident::rank(id, hostedClasses(), formatOrder(pluginPrefs().preferredFormat), skipClass);
}

bool convert(const PluginState& saved, const ident::Identity& id, const HostedClass& target, ident::Confidence conf, Conversion& out) {
  out = Conversion{};
  out.state = defaultPluginState(target.type);
  if (out.state.type != target.type) return false;
  out.state.ins = saved.ins;
  LoadNote& n = out.note;
  n.from = saved.type;
  n.to = target.type;
  n.name = target.name;
  n.confidence = ident::confidenceName(conf);
  n.total = static_cast<int>(saved.params.size());
  // Parameters by name.
  const ident::ParamMap pm = ident::mapParams(id.paramNames, saved.params.size(), target);
  out.indexMap = pm.target;
  for (size_t k = 0; k < pm.target.size(); ++k) {
    const int t = pm.target[k];
    if (t < 0 || static_cast<size_t>(t) >= out.state.params.size()) continue;
    const double v = savedNormalized(saved, id, k, target, t, pm.byPosition);
    out.state.params[static_cast<size_t>(t)] = static_cast<float>(target.fromNormalized(static_cast<size_t>(t), v));
    n.verify.push_back(t);
  }
  n.matched = pm.matched;
  // The plugin's own state.
  xstate::Core core;
  std::string how;
  if (!saved.data.empty() && xstate::unwrap(saved.data, core)) {
    // The VST2 id a chunk belongs to, from the saved type when its wrapping did not say.
    std::string f, u;
    uint32_t v2 = 0;
    if (core.vst2Id == 0 && knownType(saved.type, f, u)) {
      if (f == "vst2" && u.size() == 8) core.vst2Id = static_cast<uint32_t>(std::strtoul(u.c_str(), nullptr, 16));
      else if (f == "vst3" && ident::vst2IdOfVst3Uid(u, v2)) core.vst2Id = v2;
    }
    if (core.framework == "juce") core.vst2Id = 0;  // (a JUCE state never goes into a VST2 program)
    out.state.data = xstate::rewrap(core, target.type, target.name, &how, static_cast<int>(target.params.size()));
  }
  if (!out.state.data.empty()) {
    n.via = "state";
    n.how = core.source + " -> " + how;
  } else {
    n.via = pm.matched > 0 ? "parameters" : "none";
    n.verify.clear();  // nothing to compare: the values are sent to the plugin as they are
  }
  // The identity keeps the saved type and its aliases, so the project can go back.
  ident::Identity nid = ident::identityOf(target);
  nid.aliases = aliasesOf(target);
  for (const std::string& a : std::vector<std::string>{saved.type})
    if (!contains(nid.aliases, a)) nid.aliases.push_back(a);
  for (const auto& a : id.aliases)
    if (a != target.type && !contains(nid.aliases, a)) nid.aliases.push_back(a);
  out.state.ident = jsonCompact(ident::toJson(nid));
  return true;
}

// ---- the project document ----------------------------------------------------------------------

void resolveProjectJson(Json& project, std::vector<std::string>* report) {
  if (!project.is(Json::Type::Object)) return;
  for (auto& s : slots(project)) {
    Json& p = *s.plugin;
    if (!p.is(Json::Type::Object)) continue;
    // A stand-in whose plugin is installed here in another format: its record moves to that format,
    // so the stand-in step restores it (PluginStandIn.h).
    if (auto o = p.object.find("original"); o != p.object.end() && o->second.is(Json::Type::Object)) {
      resolvePlugin(project, nullptr, o->second, report, false);
      continue;
    }
    resolvePlugin(project, &s, p, report, true);
  }
}

std::string describe(const LoadNote& n) {
  const std::string name = n.name.empty() ? n.from : n.name;
  if (n.via == "missing") {
    if (n.suggest.empty()) return name + " (" + formatLabelOf(n.from) + ") is not installed";
    return name + " (" + formatLabelOf(n.from) + ") is not installed; a possible match needs your confirmation in its plugin window";
  }
  std::string s = name + ": opened the " + formatLabelOf(n.to) + " version (the project used the " + formatLabelOf(n.from) + " version)";
  if (n.via == "state") s += "; its settings were carried over";
  else if (n.via == "parameters") s += "; " + std::to_string(n.matched) + " of " + std::to_string(n.total) + " parameters were carried over by name";
  else s += "; its settings could not be carried over";
  return s;
}

std::vector<std::string> summary(const Song& song) {
  std::vector<std::string> out;
  auto one = [&out](const PluginState& st) {
    LoadNote n;
    if (loadNote(st, n) && (n.via != "missing" || (!n.suggest.empty() && !installed(st.type)))) out.push_back(describe(n));
  };
  for (const auto& c : song.channels) one(c.plugin);
  for (const auto& m : song.mixer)
    for (const auto& sl : m.slots) one(sl.plugin);
  return out;
}

void postLoadCheck(const std::string& line) {
  std::lock_guard<std::mutex> lk(checksMutex);
  if (checks.size() < 256) checks.push_back(line);
}

std::vector<std::string> takeLoadChecks() {
  std::lock_guard<std::mutex> lk(checksMutex);
  std::vector<std::string> out;
  out.swap(checks);
  return out;
}

}  // namespace gl::host::xformat
