// Gridline: installed plugins (VST3, VST2, CLAP, AU, LV2) in a collaborative project.
//
// A plugin instance is a JSON object of its channel or mixer track in the shared document (type,
// params, data, ident). Between this computer's song and the shared document:
//   * data (the plugin's own state): small ones inline; a bigger one is packed (base64 parts back to
//     bytes), compressed and kept as a project file Audio/<sha256>.glstate that travels like audio
//     files (chunked, paced, resumable, checksum verified); the document holds kPendingStatePrefix +
//     its name. Until it has arrived the song holds that name and the instance keeps what it has.
//   * params: in the order of the shared identity record's "pids" (the stable parameter ids of the
//     build that wrote the state); a computer whose build lists them differently maps them by id.
//   * ident: the shared record names who wrote the current state (name, vendor, version, pids); it is
//     replaced only together with a new state, so two builds never write it back and forth.
//   * a state written here writes every parameter with it (forceParamSets): the newest state wins
//     over older parameter edits, newer ones apply on top.
// A computer without the plugin keeps all of it as it came (no instance: silent) and passes it on.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/Png.h"
#include "../core/Project.h"
#include "../core/Zip.h"
#include "Session.h"

namespace fs = std::filesystem;

namespace gl::collab {

namespace {

constexpr size_t kPendingLen = 13;  // strlen(kPendingStatePrefix)
constexpr const char* kStateExt = ".glstate";

// ---- packing: the base64 parts of a state as bytes, so compression has something to work with ----
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string b64Encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// Canonical base64 only (the encoder above gives it back exactly); false otherwise.
bool b64Decode(const std::string& in, std::string& out) {
  if (in.size() % 4 != 0) return false;
  out.clear();
  out.reserve(in.size() / 4 * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + static_cast<size_t>(k)];
      if (c == '=' && i + 4 == in.size() && k >= 2) {
        v[k] = 0;
        ++pad;
      } else if (pad || (v[k] = b64Value(c)) < 0) {
        return false;
      }
    }
    const uint32_t x = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) | (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
    out += static_cast<char>((x >> 16) & 0xFF);
    if (pad < 2) out += static_cast<char>((x >> 8) & 0xFF);
    if (pad < 1) out += static_cast<char>(x & 0xFF);
  }
  return b64Encode(out) == in;
}

void put32(std::string& s, uint32_t v) {
  for (int k = 0; k < 4; ++k) s += static_cast<char>((v >> (8 * k)) & 0xFF);
}
bool get32(const std::string& s, size_t& at, uint32_t& v) {
  if (at + 4 > s.size()) return false;
  v = 0;
  for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[at + static_cast<size_t>(k)])) << (8 * k);
  at += 4;
  return true;
}

// "GLP1", the part count, then each ':'-separated part: kind (0 text, 1 base64 as bytes), length, bytes.
std::string packState(const std::string& data) {
  std::vector<std::string> parts;
  size_t at = 0;
  for (;;) {
    const size_t c = data.find(':', at);
    parts.push_back(data.substr(at, c == std::string::npos ? std::string::npos : c - at));
    if (c == std::string::npos) break;
    at = c + 1;
  }
  std::string out = "GLP1";
  put32(out, static_cast<uint32_t>(parts.size()));
  std::string bytes;
  for (const std::string& p : parts) {
    const bool b64 = p.size() >= 64 && b64Decode(p, bytes);
    out += static_cast<char>(b64 ? 1 : 0);
    const std::string& body = b64 ? bytes : p;
    put32(out, static_cast<uint32_t>(body.size()));
    out += body;
  }
  return out;
}

bool unpackState(const std::string& packed, std::string& data) {
  if (packed.compare(0, 4, "GLP1") != 0) return false;
  size_t at = 4;
  uint32_t n = 0;
  if (!get32(packed, at, n) || n > (1u << 20)) return false;
  data.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if (at >= packed.size()) return false;
    const char kind = packed[at++];
    uint32_t len = 0;
    if (!get32(packed, at, len) || at + len > packed.size()) return false;
    const std::string body = packed.substr(at, len);
    at += len;
    if (i) data += ':';
    data += kind == 1 ? b64Encode(body) : body;
  }
  return at == packed.size();
}

// The file: "GLS1", the packed size (u32), then the zlib stream.
std::string compressState(const std::string& data) {
  const std::string packed = packState(data);
  std::string chk;
  if (!unpackState(packed, chk) || chk != data) return {};  // (never: the packing is exact)
  const std::vector<uint8_t> z = png::deflateZlib(reinterpret_cast<const uint8_t*>(packed.data()), packed.size());
  std::string out = "GLS1";
  put32(out, static_cast<uint32_t>(packed.size()));
  out.append(reinterpret_cast<const char*>(z.data()), z.size());
  return out;
}

bool decompressState(const std::string& file, std::string& data) {
  if (file.size() < 14 || file.compare(0, 4, "GLS1") != 0) return false;
  size_t at = 4;
  uint32_t size = 0;
  if (!get32(file, at, size) || size > kMaxStateBytes + (1u << 20)) return false;
  std::vector<uint8_t> out;
  // zlib: two header bytes, the raw stream, four check bytes
  if (!inflateRaw(reinterpret_cast<const uint8_t*>(file.data()) + at + 2, file.size() - at - 6, out, size) || out.size() != size) return false;
  return unpackState(std::string(out.begin(), out.end()), data);
}

std::pair<size_t, size_t> stateKey(const std::string& d) { return {d.size(), std::hash<std::string>()(d)}; }

std::vector<uint32_t> idsOfJson(const Json* ident) {
  std::vector<uint32_t> v;
  const Json* a = ident && ident->is(Json::Type::Object) ? ident->find("pids") : nullptr;
  if (!a || !a->is(Json::Type::Array)) return v;
  v.reserve(a->array.size());
  for (const auto& e : a->array) v.push_back(e.is(Json::Type::Number) && e.number >= 0 ? static_cast<uint32_t>(e.number) : 0);
  return v;
}

const std::vector<uint32_t>* localIds(const std::string& type) {
  const PluginInfo* pi = findPlugin(type);
  return pi && !pi->hostParamIds.empty() && pi->hostParamIds.size() == pi->params.size() ? &pi->hostParamIds : nullptr;
}

std::string identText(const Json* ident, const char* key) {
  return ident && ident->is(Json::Type::Object) ? ident->text(key) : std::string();
}

// Every installed plugin state of a song (channel instruments and note processors, mixer slots).
void forEachHosted(const Song& s, const std::function<void(const PluginState&)>& fn) {
  for (const Channel& c : s.channels) {
    if (isHostedType(c.plugin.type)) fn(c.plugin);
    for (const PluginState& f : c.noteFx)
      if (isHostedType(f.type)) fn(f);
  }
  for (const MixerTrack& m : s.mixer)
    for (const auto& sl : m.slots)
      if (isHostedType(sl.plugin.type)) fn(sl.plugin);
}

}  // namespace

std::vector<std::string> pendingStates(const Song& s) {
  std::vector<std::string> v;
  forEachHosted(s, [&](const PluginState& p) {
    if (isPendingPluginData(p.data)) v.push_back(p.data);
  });
  return v;
}

std::string pluginLabel(const std::string& name, const std::string& vendor, const std::string& version) {
  std::string t = name;
  if (!vendor.empty()) t += " (" + vendor + (version.empty() ? std::string() : std::string(", ") + i18n::tr("version") + " " + version) + ")";
  else if (!version.empty()) t += " (" + std::string(i18n::tr("version")) + " " + version + ")";
  return t;
}

// ---------------------------------------------------------------- places in an element ----
std::vector<Session::PluginRef> Session::pluginsOf(Json& e) {
  std::vector<PluginRef> out;
  if (!e.is(Json::Type::Object)) return out;
  auto plug = [](Json* j) { return j && j->is(Json::Type::Object) && j->find("type") && j->find("params"); };
  if (Json* p = e.find("plugin") ? &e.object["plugin"] : nullptr; plug(p)) out.push_back({p, {"plugin"}});
  if (Json* nf = e.find("noteFx") ? &e.object["noteFx"] : nullptr; nf && nf->is(Json::Type::Array))
    for (size_t i = 0; i < nf->array.size(); ++i)
      if (plug(&nf->array[i])) out.push_back({&nf->array[i], {"noteFx", "[" + std::to_string(i) + "]"}});
  if (Json* sl = e.find("slots") ? &e.object["slots"] : nullptr; sl && sl->is(Json::Type::Array))
    for (size_t i = 0; i < sl->array.size(); ++i)
      if (Json* p = sl->array[i].find("plugin") ? &sl->array[i].object["plugin"] : nullptr; plug(p))
        out.push_back({p, {"slots", "[" + std::to_string(i) + "]", "plugin"}});
  return out;
}

namespace {
// The JSON at a place of an element ("[i]": list position).
const Json* jsonAt(const Json& e, const std::vector<std::string>& rel) {
  const Json* j = &e;
  for (const std::string& seg : rel) {
    if (!seg.empty() && seg[0] == '[') {
      const size_t i = static_cast<size_t>(std::strtoul(seg.c_str() + 1, nullptr, 10));
      if (!j->is(Json::Type::Array) || i >= j->array.size()) return nullptr;
      j = &j->array[i];
    } else {
      j = j->is(Json::Type::Object) ? j->find(seg) : nullptr;
      if (!j) return nullptr;
    }
  }
  return j;
}
}  // namespace

bool Session::replicaPath(const Path& elemPath, const std::vector<std::string>& rel, Path& out) const {
  out = elemPath;
  Stamp anc;
  for (size_t k = 0; k <= out.size(); ++k) {  // ancestors' clears decide what is visible
    Path pre(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(k));
    if (const Node* n = replica_.find(pre)) anc = std::max(anc, n->clearS);
  }
  for (const std::string& seg : rel) {
    if (!seg.empty() && seg[0] == '[') {
      const Node* n = replica_.find(out);
      if (!n || n->kind != Node::Kind::List) return false;
      const size_t i = static_cast<size_t>(std::strtoul(seg.c_str() + 1, nullptr, 10));
      const auto kids = visibleKids(*n, anc);
      if (i >= kids.size()) return false;
      anc = std::max(anc, n->clearS);
      out.push_back(*kids[i].first);
    } else {
      if (const Node* n = replica_.find(out)) anc = std::max(anc, n->clearS);
      out.push_back(seg);
    }
  }
  return replica_.visible(out);
}

// ------------------------------------------------------------------ states ----
std::string Session::stateToReplica(const std::string& data, bool* tooBig) {
  if (tooBig) *tooBig = false;
  if (data.size() <= kInlineStateBytes || isPendingPluginData(data)) return data;
  if (data.size() > kMaxStateBytes) {
    if (tooBig) *tooBig = true;
    return {};
  }
  const auto key = stateKey(data);
  std::error_code ec;
  if (auto it = stateRefs_.find(key); it != stateRefs_.end() && fs::is_regular_file(localPath(it->second), ec))
    return kPendingStatePrefix + it->second;
  const std::string file = compressState(data);
  if (file.empty()) return data;
  const std::string rel = importText(file, kStateExt);
  if (rel.empty()) return data;  // (the folder can't be written: it travels inside the edit)
  stateRefs_[key] = rel;
  return kPendingStatePrefix + rel;
}

std::string Session::stateToLocal(const std::string& shared) const {
  if (!isPendingPluginData(shared)) return shared;
  const std::string rel = shared.substr(kPendingLen);
  if (!safeMediaName(rel)) return shared;
  if (auto it = stateCache_.find(rel); it != stateCache_.end()) return it->second;
  std::string file, data;
  if (!readFileText(localPath(rel), file, kMaxStateFileBytes) || !decompressState(file, data)) return shared;  // still arriving
  if (stateCache_.size() >= 8) stateCache_.clear();
  stateCache_[rel] = data;
  stateRefs_[stateKey(data)] = rel;
  return data;
}

bool Session::stateLoading(const std::string& pendingData, float* progress) const {
  if (!isPendingPluginData(pendingData)) return false;
  const std::string rel = pendingData.substr(kPendingLen);
  if (progress) *progress = 0;
  auto it = incoming_.find(rel);
  if (it != incoming_.end() && progress && it->second.total) *progress = static_cast<float>(it->second.have) / static_cast<float>(it->second.total);
  return true;
}

// ------------------------------------------------------------- local -> shared ----
bool Session::pluginToReplica(Json& plug, const Json* shared) {
  const std::string type = plug.text("type");
  if (!isHostedType(type)) return false;
  const bool sameType = shared && shared->is(Json::Type::Object) && shared->text("type") == type;
  const std::string sharedData = sameType ? shared->text("data") : std::string();
  bool tooBig = false;
  std::string data = stateToReplica(plug.text("data"), &tooBig);
  if (tooBig) {
    data = sharedData;  // not shared: what the others have stays
    if (onNotice && tooBigNoticed_.insert(type).second) {
      const PluginInfo* pi = findPlugin(type);
      onNotice((pi ? pi->name : type) + ": " + i18n::tr("its settings are too large to share (over 64 MB), so collaborators keep the ones they have."), false);
    }
  }
  if (data.empty()) plug.object.erase("data");
  else plug.object["data"] = Json::makeString(data);
  const bool full = !sameType || data != sharedData;
  const Json* sharedIdent = sameType ? shared->find("ident") : nullptr;
  if (!full && sharedIdent && sharedIdent->is(Json::Type::Object)) {
    plug.object["ident"] = *sharedIdent;  // who wrote the state stays named
  } else if (const std::vector<uint32_t>* ids = localIds(type)) {
    Json& id = plug.object["ident"];
    if (!id.is(Json::Type::Object)) id = Json::makeObject();
    Json a = Json::makeArray();
    a.array.reserve(ids->size());
    for (uint32_t v : *ids) a.array.push_back(Json::makeNumber(v));
    id.object["pids"] = std::move(a);
  }
  // parameters in the shared order (the writer's), mapped by their stable ids
  const std::vector<uint32_t>* mine = localIds(type);
  const std::vector<uint32_t> theirs = idsOfJson(plug.find("ident"));
  Json* params = plug.find("params") ? &plug.object["params"] : nullptr;
  if (mine && !theirs.empty() && theirs != *mine && params && params->is(Json::Type::Array)) {
    Json out = Json::makeArray();
    const Json* sp = sameType ? shared->find("params") : nullptr;
    for (size_t j = 0; j < theirs.size(); ++j)
      out.array.push_back(sp && sp->is(Json::Type::Array) && j < sp->array.size() ? sp->array[j] : Json::makeNumber(0));
    for (size_t k = 0; k < mine->size() && k < params->array.size(); ++k) {
      auto it = std::find(theirs.begin(), theirs.end(), (*mine)[k]);
      if (it != theirs.end()) out.array[static_cast<size_t>(it - theirs.begin())] = params->array[k];
    }
    *params = std::move(out);
  }
  return full;
}

void Session::pluginsToReplica(Json& element, const Path& elemPath, FullStates* fullStates) {
  std::vector<PluginRef> plugs = pluginsOf(element);
  if (plugs.empty()) return;
  bool any = false;
  for (const PluginRef& r : plugs) any = any || isHostedType(r.plug->text("type"));
  if (!any) return;
  const Json shared = replica_.materialize(elemPath);
  for (const PluginRef& r : plugs) {
    const Json* sp = shared.is(Json::Type::Object) ? jsonAt(shared, r.rel) : nullptr;
    if (!pluginToReplica(*r.plug, sp) || !fullStates || !sp) continue;
    Path pp;
    std::vector<std::string> rel = r.rel;
    rel.push_back("params");
    if (const Json* p = r.plug->find("params"); p && replicaPath(elemPath, rel, pp)) fullStates->push_back({pp, *p});
  }
}

void Session::songPluginsToReplica(Json& song, FullStates* fullStates) {
  for (const char* kind : {"channels", "mixer"}) {
    Json* list = song.find(kind) ? &song.object[kind] : nullptr;
    if (!list || !list->is(Json::Type::Array)) continue;
    for (Json& e : list->array)
      if (const Json* id = e.find("id"); id && id->is(Json::Type::Number))
        pluginsToReplica(e, {"song", kind, "#" + std::to_string(static_cast<long long>(id->number))}, fullStates);
  }
}

void Session::forceParamSets(const Path& paramsPath, const Json& values, std::vector<Op>& ops, size_t opsFrom) {
  const Node* n = replica_.find(paramsPath);
  if (!n || n->kind != Node::Kind::List || !values.is(Json::Type::Array)) return;
  Stamp anc;
  for (size_t k = 0; k < paramsPath.size(); ++k)
    if (const Node* a = replica_.find(Path(paramsPath.begin(), paramsPath.begin() + static_cast<std::ptrdiff_t>(k)))) anc = std::max(anc, a->clearS);
  const auto kids = visibleKids(*n, anc);
  if (kids.size() != values.array.size()) return;  // the list itself changes: its diff covers it
  std::set<std::string> written;
  for (size_t i = opsFrom; i < ops.size(); ++i) {
    const Path& p = ops[i].path;
    if (p.size() < paramsPath.size() || !std::equal(paramsPath.begin(), paramsPath.end(), p.begin())) continue;
    if (p.size() == paramsPath.size()) return;  // replaced whole
    written.insert(p[paramsPath.size()]);
  }
  for (size_t i = 0; i < kids.size(); ++i) {
    const Node* kid = kids[i].second;
    if (written.count(*kids[i].first) || kid->kind != Node::Kind::Value || !values.array[i].is(Json::Type::Number)) continue;
    Op op;
    op.kind = OpKind::Set;
    op.t = hlc_.tick();
    op.path = paramsPath;
    op.path.push_back(*kids[i].first);
    op.value = values.array[i];
    op.prev = kid->val;
    ops.push_back(std::move(op));
  }
}

// ------------------------------------------------------------- shared -> local ----
void Session::pluginToLocal(Json& plug) const {
  const std::string type = plug.text("type");
  if (Json* d = plug.find("data") ? &plug.object["data"] : nullptr; d && d->is(Json::Type::String)) {
    d->str = stateToLocal(d->str);
    noteRemotePluginData(d->str);  // the instance that loads it takes the song's values (they are newer)
  }
  const std::vector<uint32_t>* mine = localIds(type);
  const std::vector<uint32_t> theirs = idsOfJson(plug.find("ident"));
  Json* params = plug.find("params") ? &plug.object["params"] : nullptr;
  if (!mine || theirs.empty() || theirs == *mine || !params || !params->is(Json::Type::Array)) return;
  const PluginInfo* pi = findPlugin(type);
  Json out = Json::makeArray();
  for (size_t k = 0; k < mine->size(); ++k) {
    auto it = std::find(theirs.begin(), theirs.end(), (*mine)[k]);
    const size_t j = it == theirs.end() ? ~size_t{0} : static_cast<size_t>(it - theirs.begin());
    out.array.push_back(j < params->array.size() ? params->array[j] : Json::makeNumber(pi && k < pi->params.size() ? pi->params[k].def : 0.0));
  }
  *params = std::move(out);
}

// ------------------------------------------------------------- who has what ----
bool Session::installedHere(const std::string& type, std::string& version) const {
  version.clear();
  if (pluginInstalledFn) return pluginInstalledFn(type, version);
  return findPlugin(type) != nullptr;
}

bool Session::pluginHere(const std::string& type) const {
  auto it = pluginsHere_.find(type);
  if (it != pluginsHere_.end()) return it->second.first;
  std::string v;
  return installedHere(type, v);
}

void Session::sendPlugins(SiteId to) {
  if (pluginsHere_.empty()) return;
  Message m;
  m.head.object["m"] = Json::makeString("plugs");
  Json have = Json::makeObject(), miss = Json::makeArray();
  for (const auto& [t, hv] : pluginsHere_) {
    if (hv.first) have.object[t] = Json::makeString(hv.second);
    else miss.array.push_back(Json::makeString(t));
  }
  m.head.object["have"] = std::move(have);
  m.head.object["miss"] = std::move(miss);
  send(to, kStreamControl, m);
}

void Session::checkPlugins() {
  lastPluginCheck_ = now_;
  if (!ready_) return;
  std::map<std::string, std::pair<bool, std::string>> now;
  std::map<std::string, std::string> label;
  forEachHosted(doc_.song(), [&](const PluginState& p) {
    if (now.count(p.type)) return;
    std::string v;
    const bool here = installedHere(p.type, v);
    now[p.type] = {here, v};
    if (!here && !missingNoticed_.count(p.type)) {
      Json id;
      if (!p.ident.empty()) jsonParse(p.ident, id);
      std::string name = identText(&id, "name");
      if (name.empty()) name = p.type;
      label[p.type] = pluginLabel(name, identText(&id, "vendor"), identText(&id, "version"));
    }
  });
  if (now == pluginsHere_) return;
  bool installedNow = false;
  for (const auto& [t, hv] : now) {
    // installed since, or another build of it now (a rescan): the shared parameters go into this
    // build's order and the engine loads the shared state
    auto was = pluginsHere_.find(t);
    if (was != pluginsHere_.end() && was->second != hv) installedNow = true;
    if (!hv.first && missingNoticed_.insert(t).second && onNotice)
      onNotice(label[t] + ": " + i18n::tr("this project uses it, but it is not installed on this computer. Install it and rescan your plugins to hear it; its settings are kept for everyone."), false);
  }
  pluginsHere_ = std::move(now);
  for (const auto& [site, pv] : peers_)
    if (pv.connected) sendPlugins(site);
  if (installedNow) {
    dirty_.full = true;
    remoteDirty_ = true;
  }
}

std::vector<PluginUse> Session::pluginUses() const {
  std::vector<PluginUse> out;
  std::map<std::string, size_t> at;
  forEachHosted(doc_.song(), [&](const PluginState& p) {
    auto it = at.find(p.type);
    if (it != at.end()) {
      ++out[it->second].instances;
      return;
    }
    at[p.type] = out.size();
    PluginUse u;
    u.type = p.type;
    u.instances = 1;
    Json id;
    if (!p.ident.empty()) jsonParse(p.ident, id);
    u.name = identText(&id, "name");
    u.vendor = identText(&id, "vendor");
    u.version = identText(&id, "version");
    if (u.name.empty())
      if (const PluginInfo* pi = findPlugin(p.type)) u.name = pi->name;
    if (u.name.empty()) u.name = p.type;
    if (auto h = pluginsHere_.find(p.type); h != pluginsHere_.end()) {
      u.here = h->second.first;
      u.hereVersion = h->second.second;
    } else {
      u.here = installedHere(p.type, u.hereVersion);
    }
    for (const auto& [site, pv] : peers_) {
      if (!pv.pluginsKnown || !isActive(site)) continue;
      auto f = pv.plugins.find(p.type);
      if (f == pv.plugins.end()) continue;
      if (f->second.first) u.versions[site] = f->second.second;
      else u.lacking.push_back(site);
    }
    out.push_back(std::move(u));
  });
  return out;
}

}  // namespace gl::collab
