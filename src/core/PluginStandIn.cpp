// Gridline: stand-ins for missing Serum / Kickstart instances (see PluginStandIn.h).
#include "PluginStandIn.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include "../host/HostedPlugins.h"
#include "../packs/plasma/plasma.h"
#include "../packs/tideline/tideline.h"
#include "Params.h"
#include "Project.h"
#include "Song.h"

namespace gl::standin {

namespace {

std::string fold(const std::string& s) {
  std::string o;
  for (unsigned char c : s)
    if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
  return o;
}

std::mutex& mu() {
  static std::mutex m;
  return m;
}
std::vector<std::string>& testHidden() {
  static std::vector<std::string> v;
  return v;
}

std::vector<std::string> hiddenEntries() {
  std::vector<std::string> out;
  if (const char* env = std::getenv("GRIDLINE_HIDE_PLUGINS")) {
    std::string cur;
    for (const char* p = env;; ++p) {
      if (*p == ',' || *p == 0) {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
        if (*p == 0) break;
      } else if (*p != ' ') {
        cur += *p;
      }
    }
  }
  std::lock_guard<std::mutex> lk(mu());
  out.insert(out.end(), testHidden().begin(), testHidden().end());
  return out;
}

thread_local int noResolve = 0;

bool isSerum(Family f) { return f == Family::Serum1 || f == Family::Serum2; }

Json parse(const std::string& text) {
  Json j;
  if (!jsonParse(text, j)) return Json{};
  return j;
}

// Automation reference of one item kind.
struct Ref {
  Json* item;
  std::string list;  // "automations", "links", "modLinks", "pattern:<id>"
};

// Every automation item of the project, by list.
std::vector<Ref> refs(Json& project) {
  std::vector<Ref> out;
  for (const char* key : {"automations", "links", "modLinks"})
    if (Json* a = project.object.count(key) ? &project.object[key] : nullptr; a && a->is(Json::Type::Array))
      for (auto& it : a->array)
        if (it.is(Json::Type::Object)) out.push_back({&it, key});
  if (auto pit = project.object.find("patterns"); pit != project.object.end() && pit->second.is(Json::Type::Array))
    for (auto& p : pit->second.array) {
      if (!p.is(Json::Type::Object)) continue;
      const std::string list = "pattern:" + std::to_string(static_cast<long long>(p.num("id", 0)));
      if (auto at = p.object.find("automation"); at != p.object.end() && at->second.is(Json::Type::Array))
        for (auto& it : at->second.array)
          if (it.is(Json::Type::Object)) out.push_back({&it, list});
    }
  return out;
}

Json* listOf(Json& project, const std::string& list) {
  if (list.compare(0, 8, "pattern:") == 0) {
    const double id = std::atof(list.c_str() + 8);
    auto pit = project.object.find("patterns");
    if (pit == project.object.end() || !pit->second.is(Json::Type::Array)) return nullptr;
    for (auto& p : pit->second.array)
      if (p.is(Json::Type::Object) && p.num("id", -1) == id) {
        Json& a = p.object["automation"];
        if (!a.is(Json::Type::Array)) a = Json::makeArray();
        return &a;
      }
    return nullptr;
  }
  Json& a = project.object[list];
  if (!a.is(Json::Type::Array)) a = Json::makeArray();
  return &a;
}

// One plugin of the project and the parameter index range it owns.
struct Slot {
  Json* plugin;
  double owner;
  uint32_t base, end;  // parameter indices [base, end) of the owner
  std::string name;    // channel name (report lines)
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

bool targets(const Json& item, const Slot& s, int& k) {
  if (item.num("owner", -1) != s.owner) return false;
  const double idx = item.num("index", -1);
  if (idx < s.base || idx >= s.end) return false;
  k = static_cast<int>(idx - s.base);
  return true;
}

// Rewrites an item onto parameter `index` through `fn` (normalized value -> normalized value).
void remapItem(Json& item, const std::string& list, uint32_t index, const std::function<float(float)>& fn) {
  item.object["index"] = Json::makeNumber(index);
  if (list == "automations" || list.compare(0, 8, "pattern:") == 0) {
    const double lo = item.num("min", 0), hi = item.num("max", 1);
    if (auto p = item.object.find("points"); p != item.object.end() && p->second.is(Json::Type::Array))
      for (auto& pt : p->second.array)
        if (pt.is(Json::Type::Array) && pt.array.size() >= 2 && pt.array[1].is(Json::Type::Number))
          pt.array[1].number = static_cast<float>(std::clamp(static_cast<double>(fn(static_cast<float>(lo + (hi - lo) * pt.array[1].number))), 0.0, 1.0));
    item.object.erase("min");
    item.object.erase("max");
  } else {
    const float lo = std::clamp(fn(static_cast<float>(item.num("min", 0))), 0.0f, 1.0f), hi = std::clamp(fn(static_cast<float>(item.num("max", 1))), 0.0f, 1.0f);
    item.object["min"] = Json::makeNumber(lo);
    item.object["max"] = Json::makeNumber(hi);
  }
}

PluginState stateOfJson(const Json& p) { return pluginStateFromJson(p); }

// Links of an original's parameters, cached per state (restores search all of them).
struct LinkCache {
  std::map<int, std::pair<bool, ParamLink>> links;
};
std::shared_ptr<LinkCache> cacheFor(const PluginState& orig) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<LinkCache>> all;
  const std::string key = orig.type + "|" + std::to_string(std::hash<std::string>{}(orig.data)) + "|" + std::to_string(orig.data.size());
  std::lock_guard<std::mutex> lk(m);
  auto& c = all[key];
  if (!c) {
    if (all.size() > 32) {
      all.clear();
      return cacheFor(orig);
    }
    c = std::make_shared<LinkCache>();
  }
  return c;
}

bool cachedLink(Family f, const PluginState& orig, int index, ParamLink& out) {
  auto c = cacheFor(orig);
  {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (auto it = c->links.find(index); it != c->links.end()) {
      out = it->second.second;
      return it->second.first;
    }
  }
  ParamLink l;
  bool ok = false;
  if (isSerum(f)) {
    ok = packs::plasma::serumParamLink(orig, index, l.standin, l.curve, l.title);
  } else if (f == Family::Kickstart1 || f == Family::Kickstart2) {
    ok = packs::tideline::kickstartParamLink(orig.type, index, l.standin, l.curve, l.title);
  }
  ok = ok && l.standin >= 0 && l.curve.size() >= 2;
  static std::mutex m2;
  std::lock_guard<std::mutex> lk(m2);
  c->links[index] = {ok, l};
  out = l;
  return ok;
}

int paramCount(Family f, const std::string& type) {
  if (isSerum(f)) return packs::plasma::serumParamCount(type);
  if (f == Family::Kickstart1 || f == Family::Kickstart2) return 12;
  return 0;
}

Json pluginJsonOf(const Original& o) {
  PluginState p;
  p.type = o.type;
  p.params = o.params;
  p.data = o.data;
  p.ins = o.ins;
  Json j = Json::makeObject();
  j.object["type"] = Json::makeString(p.type);
  Json params = Json::makeArray();
  for (float v : p.params) params.array.push_back(Json::makeNumber(v));
  j.object["params"] = params;
  if (!p.data.empty()) j.object["data"] = Json::makeString(p.data);
  if (!p.ins.empty()) j.object["ins"] = Json::makeString(p.ins);
  return j;
}

// Stand-in plugin -> original, with its automation items put back.
void restoreSlot(Json& project, Slot& s, const Original& o, std::vector<std::string>* report) {
  const Family f = familyOf(o.type, o.name, o.vendor);
  PluginState orig;
  orig.type = o.type;
  orig.params = o.params;
  orig.data = o.data;
  orig.ins = o.ins;
  const std::string standinType = s.plugin->text("type");
  *s.plugin = pluginJsonOf(o);
  // 1. recorded items: unchanged ones come back exactly, edited ones are inverted
  std::vector<Json*> done;
  for (const auto& it : o.items) {
    Json was = parse(it.was);
    if (!was.is(Json::Type::Object)) continue;
    Json* list = listOf(project, it.list);
    if (!list) continue;
    if (it.now.empty()) {  // set aside
      if (it.list == "automations") {
        const double id = was.num("id", -1);
        for (auto& a : list->array)
          if (a.is(Json::Type::Object) && a.num("id", -2) == id) {
            a = was;
            done.push_back(&a);
          }
      } else {
        list->array.push_back(was);
      }
      continue;
    }
    Json* found = nullptr;
    for (auto& a : list->array)
      if (a.is(Json::Type::Object) && jsonCompact(a) == it.now && std::find(done.begin(), done.end(), &a) == done.end()) {
        found = &a;
        break;
      }
    if (found) {
      *found = was;
      done.push_back(found);
      continue;
    }
    // edited since: same clip id (automations), else same target
    Json now = parse(it.now);
    for (auto& a : list->array) {
      if (!a.is(Json::Type::Object) || std::find(done.begin(), done.end(), &a) != done.end()) continue;
      const bool same = it.list == "automations" ? a.num("id", -1) == was.num("id", -2)
                                                 : a.num("owner", -1) == now.num("owner", -2) && a.num("index", -1) == now.num("index", -2);
      if (!same) continue;
      ParamLink l;
      const int k = static_cast<int>(was.num("index", 0) - s.base);
      if (cachedLink(f, orig, k, l)) remapItem(a, it.list, static_cast<uint32_t>(was.num("index", 0)), [&](float v) { return l.unmap(v); });
      else a.object["index"] = Json::makeNumber(was.num("index", 0));
      done.push_back(&a);
      break;
    }
  }
  // 2. other items on the stand-in's parameters (added since, or from an FL import): nearest original parameter
  std::vector<Ref> rs = refs(project);
  for (auto& r : rs) {
    if (std::find(done.begin(), done.end(), r.item) != done.end()) continue;
    int k;
    if (!targets(*r.item, s, k)) continue;
    const int n = paramCount(f, o.type);
    bool moved = false;
    for (int i = 0; i < n && !moved; ++i) {
      ParamLink l;
      if (!cachedLink(f, orig, i, l) || l.standin != k) continue;
      remapItem(*r.item, r.list, s.base + static_cast<uint32_t>(i), [&](float v) { return l.unmap(v); });
      moved = true;
    }
    if (!moved && r.list == "automations") r.item->object["owner"] = Json::makeNumber(kNoId);  // keeps the clip, targets nothing
    else if (!moved) r.item->object["owner"] = Json::makeNumber(-1);                           // dropped below
  }
  for (const char* key : {"links", "modLinks"})
    if (Json* a = listOf(project, key))
      a->array.erase(std::remove_if(a->array.begin(), a->array.end(), [](const Json& j) { return j.num("owner", 0) < 0; }), a->array.end());
  if (auto pit = project.object.find("patterns"); pit != project.object.end() && pit->second.is(Json::Type::Array))
    for (auto& p : pit->second.array)
      if (auto at = p.object.find("automation"); at != p.object.end() && at->second.is(Json::Type::Array))
        at->second.array.erase(std::remove_if(at->second.array.begin(), at->second.array.end(), [](const Json& j) { return j.num("owner", 0) < 0; }),
                               at->second.array.end());
  if (report) report->push_back(std::string(familyName(f)) + (s.name.empty() ? "" : " (" + s.name + ")") + ": the installed plugin replaces " + standinType);
}

// Missing original -> stand-in, automation items moved (recorded in the Original).
void convertSlot(Json& project, Slot& s, std::vector<std::string>* report) {
  PluginState orig = stateOfJson(*s.plugin);
  const Family f = familyOf(orig.type);
  PluginState st;
  if (!makeStandIn(orig, "", "", st)) return;
  Original o;
  fromText(st.original, o);
  o.name = familyName(f);
  std::vector<Ref> rs = refs(project);
  for (auto& r : rs) {
    int k;
    if (!targets(*r.item, s, k)) continue;
    Original::Item item;
    item.list = r.list;
    item.was = jsonCompact(*r.item);
    ParamLink l;
    if (cachedLink(f, orig, k, l)) {
      remapItem(*r.item, r.list, s.base + static_cast<uint32_t>(l.standin), [&](float v) { return l.map(v); });
      item.now = jsonCompact(*r.item);
    } else if (r.list == "automations") {
      r.item->object["owner"] = Json::makeNumber(kNoId);  // set aside: the clip stays, targets nothing
    } else {
      r.item->object["owner"] = Json::makeNumber(-1);
    }
    o.items.push_back(std::move(item));
  }
  for (const char* key : {"links", "modLinks"})
    if (Json* a = listOf(project, key))
      a->array.erase(std::remove_if(a->array.begin(), a->array.end(), [](const Json& j) { return j.num("owner", 0) < 0; }), a->array.end());
  if (auto pit = project.object.find("patterns"); pit != project.object.end() && pit->second.is(Json::Type::Array))
    for (auto& p : pit->second.array)
      if (auto at = p.object.find("automation"); at != p.object.end() && at->second.is(Json::Type::Array))
        at->second.array.erase(std::remove_if(at->second.array.begin(), at->second.array.end(), [](const Json& j) { return j.num("owner", 0) < 0; }),
                               at->second.array.end());
  st.original = toText(o);
  *s.plugin = pluginStateToJson(st);
  if (report) report->push_back(std::string(familyName(f)) + (s.name.empty() ? "" : " (" + s.name + ")") + " is not installed: opens as " + standInName(f));
}

}  // namespace

// ------------------------------------------------------------------ families
Family familyOf(const std::string& type, const std::string& name, const std::string& vendor) {
  static const std::pair<const char*, Family> ids[] = {
      {"vst3:56534558667350736572756D20320000", Family::Serum2},
      {"au:aumu/Xf2X/XFER", Family::Serum2},
      {"vst2:58667358", Family::Serum1},
      {"au:aumu/XfsX/XFER", Family::Serum1},
      {"vst3:ABCDEF019182FAEB43614E52434E4B32", Family::Kickstart2},
      {"vst2:434E4B32", Family::Kickstart2},
      {"au:aumf/CNK2/CaNR", Family::Kickstart2},
  };
  for (const auto& [id, f] : ids)
    if (type == id) return f;
  if (name.empty() || !isHostedType(type)) return Family::None;
  // By name + vendor (FL projects name the plugin; other builds have other ids).
  const std::string n = fold(name), v = fold(vendor);
  const bool xfer = v.empty() || v.find("xfer") != std::string::npos;
  const bool kick = v.empty() || v.find("sonicacademy") != std::string::npos || v.find("nickyromero") != std::string::npos ||
                    v.find("kickstart") != std::string::npos;
  if (xfer && (n == "serum2" || n == "serum2x64")) return Family::Serum2;
  if (xfer && (n == "serum" || n == "serumx64")) return Family::Serum1;
  if (kick && (n == "kickstart2" || n == "kickstart2x64")) return Family::Kickstart2;
  if (kick && (n == "kickstart" || n == "kickstartx64")) return Family::Kickstart1;
  return Family::None;
}

const char* familyName(Family f) {
  switch (f) {
    case Family::Serum1: return "Serum";
    case Family::Serum2: return "Serum 2";
    case Family::Kickstart1: return "Kickstart";
    case Family::Kickstart2: return "Kickstart 2";
    default: return "";
  }
}
const char* standInType(Family f) {
  if (isSerum(f)) return packs::plasma::kType;
  if (f == Family::Kickstart1 || f == Family::Kickstart2) return "gl.tideline";
  return "";
}
const char* standInName(Family f) {
  if (isSerum(f)) return "Plasma";
  if (f == Family::Kickstart1 || f == Family::Kickstart2) return "Tideline";
  return "";
}

void setHiddenPlugins(const std::vector<std::string>& entries) {
  std::lock_guard<std::mutex> lk(mu());
  testHidden() = entries;
}

bool hidden(const std::string& type, const std::string& name) {
  const auto entries = hiddenEntries();
  if (entries.empty()) return false;
  const Family f = familyOf(type, name);
  for (const auto& e : entries) {
    const std::string le = fold(e);
    if (e == type || (!name.empty() && le == fold(name))) return true;
    if (f == Family::None) continue;
    if (le == "all" || (le == "serum" && isSerum(f)) || (le == "kickstart" && !isSerum(f))) return true;
  }
  return false;
}

bool installed(const std::string& type) { return host::hostedClass(type) != nullptr && !hidden(type); }

// ------------------------------------------------------------------ the original record
std::string toText(const Original& o) {
  Json j = Json::makeObject();
  j.object["type"] = Json::makeString(o.type);
  if (!o.name.empty()) j.object["name"] = Json::makeString(o.name);
  if (!o.vendor.empty()) j.object["vendor"] = Json::makeString(o.vendor);
  Json params = Json::makeArray();
  for (float v : o.params) params.array.push_back(Json::makeNumber(v));
  j.object["params"] = params;
  if (!o.data.empty()) j.object["data"] = Json::makeString(o.data);
  if (!o.ins.empty()) j.object["ins"] = Json::makeString(o.ins);
  if (!o.format.empty()) j.object["format"] = Json::makeString(o.format);
  if (!o.notes.empty()) {
    Json n = Json::makeArray();
    for (const auto& s : o.notes) n.array.push_back(Json::makeString(s));
    j.object["notes"] = n;
  }
  if (!o.items.empty()) {
    Json a = Json::makeArray();
    for (const auto& it : o.items) {
      Json e = Json::makeObject();
      e.object["list"] = Json::makeString(it.list);
      if (!it.now.empty()) e.object["now"] = Json::makeString(it.now);
      e.object["was"] = Json::makeString(it.was);
      a.array.push_back(e);
    }
    j.object["items"] = a;
  }
  return jsonCompact(j);
}

bool fromText(const std::string& text, Original& o) {
  o = Original{};
  Json j;
  if (text.empty() || !jsonParse(text, j) || !j.is(Json::Type::Object)) return false;
  o.type = j.text("type");
  o.name = j.text("name");
  o.vendor = j.text("vendor");
  o.data = j.text("data");
  o.ins = j.text("ins");
  o.format = j.text("format");
  if (const Json* p = j.find("params"); p && p->is(Json::Type::Array))
    for (const auto& v : p->array) o.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
  if (const Json* n = j.find("notes"); n && n->is(Json::Type::Array))
    for (const auto& v : n->array)
      if (v.is(Json::Type::String)) o.notes.push_back(v.str);
  if (const Json* a = j.find("items"); a && a->is(Json::Type::Array))
    for (const auto& e : a->array)
      if (e.is(Json::Type::Object)) o.items.push_back({e.text("list"), e.text("now"), e.text("was")});
  return !o.type.empty();
}

// ------------------------------------------------------------------ conversion
bool convert(Family f, const PluginState& orig, Conversion& out) {
  out = Conversion{};
  if (isSerum(f)) {
    packs::plasma::ImportReport rep;
    const bool ok = packs::plasma::serumStateToPlasma(orig, out.state, rep);
    out.format = rep.format;
    out.error = rep.error;
    out.notes = rep.approximated;
    for (const auto& m : rep.missingFiles) out.notes.push_back("Missing file: " + m);
    return ok;
  }
  if (f == Family::Kickstart1 || f == Family::Kickstart2) {
    packs::tideline::KickstartReport rep;
    out.state = defaultPluginState("gl.tideline");
    const bool ok = packs::tideline::kickstartToTideline(orig.type, orig.data, orig.params, out.state.params, rep);
    normalizePluginState(out.state);
    out.format = rep.format;
    out.error = rep.error;
    out.notes = rep.notes;
    return ok;
  }
  out.error = "no stand-in for this plugin";
  return false;
}

float ParamLink::map(float n) const {
  if (curve.empty()) return n;
  if (curve.size() == 1) return curve[0];
  const double x = std::clamp(static_cast<double>(n), 0.0, 1.0) * static_cast<double>(curve.size() - 1);
  const size_t i = std::min(static_cast<size_t>(x), curve.size() - 2);
  const double t = x - static_cast<double>(i);
  return static_cast<float>(curve[i] + (curve[i + 1] - curve[i]) * t);
}

float ParamLink::unmap(float v) const {
  if (curve.size() < 2) return v;
  float best = 0, err = 1e9f;
  for (int i = 0; i <= 512; ++i) {
    const float n = static_cast<float>(i) / 512.0f;
    const float e = std::fabs(map(n) - v);
    if (e < err - 1e-7f) err = e, best = n;
  }
  return best;
}

bool paramLink(Family f, const PluginState& orig, int index, ParamLink& out) { return cachedLink(f, orig, index, out); }

int paramIndexOfId(Family f, const std::string& type, uint32_t id) {
  if (isSerum(f)) return packs::plasma::serumParamIndexOfId(type, id);
  if (f == Family::Kickstart1 || f == Family::Kickstart2) return packs::tideline::kickstartParamIndexOfId(type, id);
  return -1;
}

bool makeStandIn(const PluginState& orig, const std::string& name, const std::string& vendor, PluginState& out,
                 std::vector<std::string>* notes) {
  const Family f = familyOf(orig.type, name, vendor);
  if (f == Family::None) return false;
  Conversion c;
  if (!convert(f, orig, c)) return false;
  Original o;
  o.type = orig.type;
  o.name = name.empty() ? familyName(f) : name;
  o.vendor = vendor;
  o.params = orig.params;
  o.data = orig.data;
  o.ins = orig.ins;
  o.format = c.format;
  o.notes = c.notes;
  out = c.state;
  out.ins = orig.ins;
  out.original = toText(o);
  if (notes) *notes = c.notes;
  return true;
}

bool isStandIn(const PluginState& s) { return !s.original.empty(); }

PluginState originalState(const PluginState& s) {
  PluginState p;
  Original o;
  if (!fromText(s.original, o)) return p;
  p.type = o.type;
  p.params = o.params;
  p.data = o.data;
  p.ins = o.ins;
  return p;
}

std::string describe(const PluginState& s) {
  Original o;
  if (!fromText(s.original, o)) return "";
  const Family f = familyOf(o.type, o.name, o.vendor);
  const std::string n = f != Family::None ? familyName(f) : o.name;
  const std::string si = f != Family::None ? standInName(f) : s.type;
  return n + " is not installed: " + si + " plays its sound";
}

std::vector<std::string> notes(const PluginState& s) {
  Original o;
  fromText(s.original, o);
  return o.notes;
}

std::vector<std::string> summary(const Song& song) {
  std::vector<std::string> out;
  auto add = [&](const PluginState& p, const std::string& where) {
    Original o;
    if (p.original.empty() || !fromText(p.original, o)) return;
    const Family f = familyOf(o.type, o.name, o.vendor);
    out.push_back(std::string(f != Family::None ? familyName(f) : o.name.c_str()) + (where.empty() ? "" : " (" + where + ")") + " is not installed: " +
                  (f != Family::None ? standInName(f) : p.type.c_str()) + " plays it");
  };
  for (const auto& c : song.channels) add(c.plugin, c.name);
  for (const auto& m : song.mixer)
    for (const auto& sl : m.slots) add(sl.plugin, m.name);
  return out;
}

// ------------------------------------------------------------------ projects
void resolveProjectJson(Json& project, std::vector<std::string>* report) {
  if (!project.is(Json::Type::Object)) return;
  std::vector<Slot> ss = slots(project);
  for (auto& s : ss) {
    if (!s.plugin->is(Json::Type::Object)) continue;
    const std::string type = s.plugin->text("type");
    if (const Json* oj = s.plugin->find("original"); oj && oj->is(Json::Type::Object)) {
      Original o;
      if (fromText(jsonCompact(*oj), o) && installed(o.type)) restoreSlot(project, s, o, report);
      continue;
    }
    const Family f = familyOf(type);
    if (f == Family::None || installed(type)) continue;
    convertSlot(project, s, report);
  }
}

void restoreProjectJson(Json& project) {
  if (!project.is(Json::Type::Object)) return;
  std::vector<Slot> ss = slots(project);
  for (auto& s : ss) {
    if (!s.plugin->is(Json::Type::Object)) continue;
    if (const Json* oj = s.plugin->find("original"); oj && oj->is(Json::Type::Object)) {
      Original o;
      if (fromText(jsonCompact(*oj), o)) restoreSlot(project, s, o, nullptr);
    }
  }
}

void restoreAll(Song& song) {
  bool any = false;
  for (const auto& c : song.channels) any = any || isStandIn(c.plugin);
  for (const auto& m : song.mixer)
    for (const auto& sl : m.slots) any = any || isStandIn(sl.plugin);
  if (!any) return;
  Json j = songToJson(song);
  restoreProjectJson(j);
  NoResolve guard;
  Song out;
  if (songFromJson(j, out)) song = std::move(out);
}

NoResolve::NoResolve() { ++noResolve; }
NoResolve::~NoResolve() { --noResolve; }
bool resolving() { return noResolve > 0; }

}  // namespace gl::standin
