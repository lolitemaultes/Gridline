// Gridline: cross-format plugin identity and the matcher (K-78). See PluginIdentity.h.
#include "PluginIdentity.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include "HostedPlugins.h"
#include "PluginGroups.h"

namespace gl::host::ident {

namespace {

std::string upperHex(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return o;
}

std::string hex8(uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

uint32_t byteSwap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

bool hexValue(const std::string& s, size_t at, size_t n, uint32_t& out) {
  if (at + n > s.size()) return false;
  uint32_t v = 0;
  for (size_t i = at; i < at + n; ++i) {
    const char c = s[i];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(d);
  }
  out = v;
  return true;
}

std::string formatOf(const std::string& type) {
  const size_t c = type.find(':');
  return c == std::string::npos ? std::string() : type.substr(0, c);
}
std::string uidOf(const std::string& type) {
  const size_t c = type.find(':');
  return c == std::string::npos ? std::string() : type.substr(c + 1);
}

bool vst2IdOfType(const std::string& type, uint32_t& id) {
  return formatOf(type) == "vst2" && uidOf(type).size() == 8 && hexValue(uidOf(type), 0, 8, id);
}

// The two keys contain one another (at least four characters): "sausagefattener" / "dadalifesausagefattener".
bool looseName(const std::string& a, const std::string& b) {
  if (a.size() < 4 || b.size() < 4) return false;
  return a.find(b) != std::string::npos || b.find(a) != std::string::npos;
}

// A name key without the vendor written in front of it ("Dada Life Sausage Fattener").
std::string nameKey(const std::string& name, const std::string& vendor) {
  std::string n = groupNameKey(name);
  std::string v;
  for (char c : vendor)
    if (std::isalnum(static_cast<unsigned char>(c))) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (v.size() >= 3 && n.size() > v.size() + 2 && n.compare(0, v.size(), v) == 0) n = n.substr(v.size());
  return n;
}

int formatRank(const std::vector<std::string>& order, const std::string& f) {
  auto it = std::find(order.begin(), order.end(), f);
  return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
}

}  // namespace

const char* confidenceName(Confidence c) {
  switch (c) {
    case Confidence::Exact: return "exact";
    case Confidence::High: return "high";
    case Confidence::Medium: return "medium";
    case Confidence::Low: return "low";
    case Confidence::None: break;
  }
  return "none";
}

Confidence confidenceOf(const std::string& n) {
  if (n == "exact") return Confidence::Exact;
  if (n == "high") return Confidence::High;
  if (n == "medium") return Confidence::Medium;
  if (n == "low") return Confidence::Low;
  return Confidence::None;
}

Identity identityOf(const HostedClass& c) {
  Identity id;
  id.type = c.type;
  id.name = c.name;
  id.vendor = c.vendor;
  id.version = c.version;
  id.instrument = c.instrument ? 1 : 0;
  id.inputs = c.inputChannels;
  id.outputs = c.outputChannels;
  bool anySteps = false;
  for (size_t k = 0; k < c.params.size(); ++k) {
    id.paramNames.push_back(c.params[k].title);
    const int steps = c.isChoice(k) ? c.params[k].stepCount : 0;
    id.paramSteps.push_back(steps);
    anySteps = anySteps || steps > 0;
  }
  if (!anySteps) id.paramSteps.clear();
  return id;
}

Json toJson(const Identity& id) {
  Json j = Json::makeObject();
  if (!id.type.empty()) j.object["type"] = Json::makeString(id.type);
  if (!id.name.empty()) j.object["name"] = Json::makeString(id.name);
  if (!id.vendor.empty()) j.object["vendor"] = Json::makeString(id.vendor);
  if (!id.version.empty()) j.object["version"] = Json::makeString(id.version);
  if (id.instrument >= 0) j.object["kind"] = Json::makeString(id.instrument ? "instrument" : "effect");
  if (id.inputs >= 0) j.object["inputs"] = Json::makeNumber(id.inputs);
  if (id.outputs >= 0) j.object["outputs"] = Json::makeNumber(id.outputs);
  if (!id.aliases.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : id.aliases) a.array.push_back(Json::makeString(s));
    j.object["aliases"] = std::move(a);
  }
  if (!id.paramNames.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : id.paramNames) a.array.push_back(Json::makeString(s));
    j.object["paramNames"] = std::move(a);
  }
  if (!id.paramSteps.empty()) {
    Json a = Json::makeArray();
    for (int s : id.paramSteps) a.array.push_back(Json::makeNumber(s));
    j.object["paramSteps"] = std::move(a);
  }
  return j;
}

bool fromJson(const Json& j, Identity& out) {
  out = Identity{};
  if (!j.is(Json::Type::Object)) return false;
  out.type = j.text("type");
  out.name = j.text("name");
  out.vendor = j.text("vendor");
  out.version = j.text("version");
  const std::string kind = j.text("kind");
  out.instrument = kind == "instrument" ? 1 : kind == "effect" ? 0 : -1;
  out.inputs = static_cast<int>(std::clamp(j.num("inputs", -1), -1.0, 256.0));
  out.outputs = static_cast<int>(std::clamp(j.num("outputs", -1), -1.0, 256.0));
  auto strings = [&j](const char* key, std::vector<std::string>& v) {
    if (const Json* a = j.find(key); a && a->is(Json::Type::Array))
      for (const auto& e : a->array)
        if (e.is(Json::Type::String)) v.push_back(e.str);
  };
  strings("aliases", out.aliases);
  strings("paramNames", out.paramNames);
  if (const Json* a = j.find("paramSteps"); a && a->is(Json::Type::Array))
    for (const auto& e : a->array) out.paramSteps.push_back(e.is(Json::Type::Number) ? static_cast<int>(std::clamp(e.number, 0.0, 1e6)) : 0);
  return !out.empty();
}

// ---- ids ----------------------------------------------------------------------------------------

bool parseFourCC(const std::string& s, uint32_t& out) {
  if (s.size() == 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return hexValue(s, 2, 8, out);
  if (s.size() != 4) return false;
  out = 0;
  for (char c : s) out = (out << 8) | static_cast<unsigned char>(c);
  return true;
}

bool parseAuKey(const std::string& key, uint32_t& type, uint32_t& subtype, uint32_t& manufacturer) {
  std::string k = key.compare(0, 3, "au:") == 0 ? key.substr(3) : key;
  const size_t a = k.find('/'), b = a == std::string::npos ? a : k.find('/', a + 1);
  if (b == std::string::npos) return false;
  return parseFourCC(k.substr(0, a), type) && parseFourCC(k.substr(a + 1, b - a - 1), subtype) && parseFourCC(k.substr(b + 1), manufacturer);
}

std::string vst2CompatibleVst3Uid(uint32_t vst2Id, const std::string& vst2Name) {
  std::string s = "565354" + hex8(vst2Id);  // "VST", then the id
  for (size_t i = 0; i < 9; ++i) {
    unsigned char c = i < vst2Name.size() ? static_cast<unsigned char>(vst2Name[i]) : 0;
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    char b[3];
    std::snprintf(b, sizeof b, "%02X", c);
    s += b;
  }
  return s;
}

bool vst2IdOfVst3Uid(const std::string& uid, uint32_t& vst2Id) {
  const std::string u = upperHex(uid);
  if (u.size() != 32 || u.compare(0, 6, "565354") != 0) return false;
  return hexValue(u, 6, 8, vst2Id);
}

std::string juceVst3Uid(uint32_t manufacturer, uint32_t plugin) { return "ABCDEF019182FAEB" + hex8(manufacturer) + hex8(plugin); }

bool juceCodesOfVst3Uid(const std::string& uid, uint32_t& manufacturer, uint32_t& plugin) {
  const std::string u = upperHex(uid);
  if (u.size() != 32 || u.compare(0, 16, "ABCDEF019182FAEB") != 0) return false;
  return hexValue(u, 16, 8, manufacturer) && hexValue(u, 24, 8, plugin);
}

int idRelation(const std::string& a, const std::string& nameA, const std::string& b, const std::string& nameB, std::string* why) {
  auto say = [why](const char* s, int v) {
    if (why) *why = s;
    return v;
  };
  if (a.empty() || b.empty()) return 0;
  if (a == b) return say("the same plugin id", 100);
  const std::string fa = formatOf(a), fb = formatOf(b);
  if (fa == fb) return 0;
  // Order the pair: (vst2|au, vst3) and (vst2, au).
  if (fa == "vst3" && (fb == "vst2" || fb == "au")) return idRelation(b, nameB, a, nameA, why);
  if (fa == "au" && fb == "vst2") return idRelation(b, nameB, a, nameA, why);
  uint32_t v2 = 0, t = 0, sub = 0, man = 0;
  if (fa == "vst2" && fb == "vst3" && vst2IdOfType(a, v2)) {
    uint32_t inUid = 0;
    if (!vst2IdOfVst3Uid(uidOf(b), inUid) || inUid != v2) return 0;
    const std::string u = upperHex(uidOf(b));
    if (u == vst2CompatibleVst3Uid(v2, nameA) || u == vst2CompatibleVst3Uid(v2, nameB))
      return say("the VST3 class id is the VST2-compatible id of this VST2 plugin (id and name)", 100);
    return say("the VST3 class id holds this VST2 plugin's id", 92);
  }
  if (fa == "au" && fb == "vst3" && parseAuKey(uidOf(a), t, sub, man)) {
    uint32_t m = 0, p = 0;
    if (juceCodesOfVst3Uid(uidOf(b), m, p) && m == man && p == sub)
      return say("the VST3 class id is built from the Audio Unit's manufacturer and plugin codes", 95);
    if (vst2IdOfVst3Uid(uidOf(b), p) && p == sub) return say("the VST3 class id holds the Audio Unit's plugin code", 60);
    // Some frameworks end the class id with the vendor and plugin codes, bytes reversed.
    if (hexValue(upperHex(uidOf(b)), 16, 8, m) && hexValue(upperHex(uidOf(b)), 24, 8, p) && uidOf(b).size() == 32 && m == byteSwap(man) && p == byteSwap(sub))
      return say("the VST3 class id ends with the Audio Unit's vendor and plugin codes", 90);
    return 0;
  }
  if (fa == "vst2" && fb == "au" && vst2IdOfType(a, v2) && parseAuKey(uidOf(b), t, sub, man) && v2 == sub)
    return say("the VST2 id is the Audio Unit's plugin code", 60);
  return 0;
}

// ---- the matcher --------------------------------------------------------------------------------

std::string paramKey(const std::string& title) {
  std::string k;
  for (char c : title)
    if (std::isalnum(static_cast<unsigned char>(c))) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return k;
}

ParamMap mapParams(const std::vector<std::string>& savedNames, size_t savedCount, const HostedClass& target) {
  ParamMap m;
  m.target.assign(savedCount, -1);
  if (savedNames.empty()) {
    if (savedCount == target.params.size() && savedCount > 0) {
      for (size_t k = 0; k < savedCount; ++k) m.target[k] = static_cast<int>(k);
      m.matched = static_cast<int>(savedCount);
      m.byPosition = true;
    }
    return m;
  }
  std::map<std::string, std::vector<int>> byKey;  // target parameters by name key, in order
  for (size_t k = 0; k < target.params.size(); ++k) byKey[paramKey(target.params[k].title)].push_back(static_cast<int>(k));
  std::map<std::string, size_t> used;
  for (size_t k = 0; k < savedCount && k < savedNames.size(); ++k) {
    const std::string key = paramKey(savedNames[k]);
    if (key.empty()) continue;
    auto it = byKey.find(key);
    if (it == byKey.end()) continue;
    size_t& n = used[key];
    if (n >= it->second.size()) continue;
    m.target[k] = it->second[n++];
    ++m.matched;
  }
  // Names cut short by a format (VST2 names have eight characters: "Intensit" / "Intensity"): one name
  // starts with the other, and only one unused parameter fits.
  std::vector<bool> used2(target.params.size(), false);
  for (int t : m.target)
    if (t >= 0) used2[static_cast<size_t>(t)] = true;
  for (size_t k = 0; k < savedCount && k < savedNames.size(); ++k) {
    if (m.target[k] >= 0) continue;
    const std::string key = paramKey(savedNames[k]);
    if (key.size() < 5) continue;
    int found = -1, count = 0;
    for (size_t t = 0; t < target.params.size(); ++t) {
      if (used2[t]) continue;
      const std::string tk = paramKey(target.params[t].title);
      if (tk.size() < 5) continue;
      if (tk.compare(0, key.size(), key) == 0 || key.compare(0, tk.size(), tk) == 0) found = static_cast<int>(t), ++count;
    }
    if (count != 1) continue;
    m.target[k] = found;
    used2[static_cast<size_t>(found)] = true;
    ++m.matched;
  }
  return m;
}

std::vector<Candidate> rank(const Identity& saved, const std::vector<std::shared_ptr<const HostedClass>>& classes,
                            const std::vector<std::string>& order, bool (*skip)(const HostedClass&)) {
  std::vector<Candidate> out;
  const std::string sName = nameKey(saved.name, saved.vendor), sVendor = groupVendorKey(saved.vendor);
  const bool savedTagged = paramKey(groupNameKey(saved.name)) != paramKey(saved.name);
  std::vector<std::string> ids;
  if (!saved.type.empty()) ids.push_back(saved.type);
  for (const auto& a : saved.aliases)
    if (!a.empty() && std::find(ids.begin(), ids.end(), a) == ids.end()) ids.push_back(a);
  for (const auto& c : classes) {
    if (!c || c->type == saved.type || (skip && skip(*c))) continue;
    if (isOwnSuiteType(c->type) != isOwnSuiteType(saved.type) && !saved.type.empty()) continue;
    Candidate cd;
    cd.cls = c;
    // Ids.
    int idScore = 0;
    std::string idWhy;
    for (size_t i = 0; i < ids.size(); ++i) {
      std::string w;
      int r = idRelation(ids[i], saved.name, c->type, c->name, &w);
      if (i > 0 && r >= 100) {
        r = 96;
        w = "recorded as the same plugin when the project was saved";
      }
      if (r > idScore) idScore = r, idWhy = w;
    }
    // Names and vendors.
    const std::string cName = nameKey(c->name, c->vendor), cVendor = groupVendorKey(c->vendor);
    const bool nameEq = !sName.empty() && sName == cName;
    const bool nameNear = !nameEq && !sName.empty() && looseName(sName, cName);
    const bool vendorKnown = !sVendor.empty() && !cVendor.empty();
    const bool vendorEq = vendorKnown && (sVendor == cVendor || looseName(paramKey(saved.vendor), paramKey(c->vendor)));
    if (vendorKnown && !vendorEq && idScore < 95) continue;  // another company's plugin is not this plugin
    if (saved.instrument >= 0 && (saved.instrument == 1) != c->instrument) continue;  // kinds must agree
    if (idScore == 0 && !nameEq && !nameNear) continue;
    int score = idScore;
    if (idScore > 0) cd.reasons.push_back(idWhy);
    if (nameEq) score += 40, cd.reasons.push_back("same name");
    else if (nameNear) score += 15, cd.reasons.push_back("similar name");
    // A saved name that carries a format tag ("Synth (CLAP)") names that very build: it goes first.
    cd.exactName = nameEq && savedTagged && paramKey(saved.name) == paramKey(c->name);
    if (vendorEq) score += 25, cd.reasons.push_back("same vendor");
    else if (!vendorKnown) cd.reasons.push_back("vendor not known");
    // Main outputs (a mono build is not the stereo plugin's twin, though it may stand for it).
    bool ioClash = false;
    if (saved.outputs > 0 && c->outputChannels > 0 && saved.outputs != c->outputChannels) {
      score -= 15;
      ioClash = true;
      cd.reasons.push_back("different number of outputs");
    }
    if (saved.inputs >= 0 && c->inputChannels >= 0 && (saved.inputs > 0) != (c->inputChannels > 0) && saved.instrument == 0) {
      score -= 15;
      ioClash = true;
      cd.reasons.push_back("different inputs");
    }
    // Parameter names.
    bool paramsAgree = false, paramsClash = false;
    if (!saved.paramNames.empty() && !c->params.empty()) {
      const ParamMap pm = mapParams(saved.paramNames, saved.paramNames.size(), *c);
      cd.paramsMatched = pm.matched;
      cd.paramsTotal = static_cast<int>(saved.paramNames.size());
      const double r = static_cast<double>(pm.matched) / static_cast<double>(saved.paramNames.size());
      if (saved.paramNames.size() >= 3 && r >= 0.8) score += 20, paramsAgree = true, cd.reasons.push_back("parameters match");
      else if (saved.paramNames.size() >= 4 && c->params.size() >= 4 && r < 0.3) score -= 40, paramsClash = true, cd.reasons.push_back("parameters differ");
    }
    cd.score = score;
    Confidence conf = Confidence::None;
    if (idScore >= 95 && !paramsClash) conf = Confidence::High;
    else if (idScore >= 90 && (nameEq || nameNear || vendorEq) && !paramsClash) conf = Confidence::High;
    else if (nameEq && vendorEq && paramsAgree && !ioClash) conf = Confidence::High;
    else if (nameEq && vendorEq && !paramsClash) conf = Confidence::Medium;
    else if (idScore >= 60 && nameEq && !paramsClash) conf = Confidence::Medium;
    else if (nameEq || (nameNear && vendorEq) || idScore >= 60) conf = Confidence::Low;
    if (conf == Confidence::None) continue;
    cd.confidence = conf;
    out.push_back(std::move(cd));
  }
  std::stable_sort(out.begin(), out.end(), [&order](const Candidate& x, const Candidate& y) {
    if (x.confidence != y.confidence) return x.confidence > y.confidence;
    if (x.exactName != y.exactName) return x.exactName;
    const int rx = formatRank(order, x.cls->format), ry = formatRank(order, y.cls->format);
    if (rx != ry) return rx < ry;
    return x.score > y.score;
  });
  return out;
}

const Candidate* autoPick(const std::vector<Candidate>& ranked) {
  if (ranked.empty() || ranked.front().confidence < Confidence::Medium) return nullptr;
  const Candidate& best = ranked.front();
  const std::string bn = nameKey(best.cls->name, best.cls->vendor), bv = groupVendorKey(best.cls->vendor);
  for (size_t i = 1; i < ranked.size(); ++i) {
    const Candidate& o = ranked[i];
    if (o.confidence < best.confidence) break;
    // The same product in yet another format is fine; another product just as good is a guess.
    if (nameKey(o.cls->name, o.cls->vendor) != bn || groupVendorKey(o.cls->vendor) != bv) return nullptr;
  }
  return &best;
}

}  // namespace gl::host::ident
