// Gridline - K-237: named workspaces of a layout. See Workspaces.h.
#include "Workspaces.h"
#include <algorithm>
#include <cctype>
#include "Json.h"

namespace gl::workspaces {

using layout::Preset;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool taken(const Set& s, const std::string& name, int except = -1) {
  const std::string l = lower(name);
  for (size_t i = 0; i < s.items.size(); ++i)
    if (static_cast<int>(i) != except && lower(s.items[i].name) == l) return true;
  return false;
}

void clampActive(Set& s) {
  if (s.items.empty()) s.active = 0;
  else s.active = std::clamp(s.active, 0, static_cast<int>(s.items.size()) - 1);
}

Workspace starter(const char* name, std::set<std::string> open) {
  Workspace w;
  w.name = name;
  w.saved.open = std::move(open);
  w.current = w.saved;
  return w;
}

const char* firstName(Preset p) {
  switch (p) {
    case Preset::Live: return "Session";
    case Preset::Logic: return "Tracks";
    case Preset::Tracker: return "Patterns";
    default: return "Arrange";
  }
}

}  // namespace

Set initial(Preset p, const Arrangement& now) {
  Set s;
  Workspace w;
  w.name = firstName(p);
  w.saved = w.current = now;
  s.items.push_back(w);
  return s;
}

std::vector<Workspace> starters(Preset p) {
  switch (p) {
    case Preset::Live:
      return {starter("Session", {"Live pads", "Browser"}), starter("Arrange", {"Playlist", "Browser"}),
              starter("Devices", {"Live pads", "Browser", "Mixer"})};
    case Preset::Logic:
      return {starter("Tracks", {"Playlist", "Browser"}), starter("Mix", {"Playlist", "Mixer"}), starter("Edit", {"Playlist", "Piano roll"})};
    case Preset::Tracker:
      return {starter("Patterns", {"Pattern editor"}), starter("Instruments", {"Channel settings"}), starter("Samples", {"Audio editor"})};
    default:
      return {starter("Arrange", {"Browser", "Playlist", "Channel rack"}), starter("Mix", {"Browser", "Playlist", "Mixer"}),
              starter("Patterns", {"Browser", "Playlist", "Channel rack", "Piano roll"}), starter("Edit", {"Playlist", "Piano roll"})};
  }
}

std::string cleanName(const std::string& name) {
  std::string out;
  for (char c : name)
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
  const size_t a = out.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  out = out.substr(a, out.find_last_not_of(' ') - a + 1);
  if (out.size() > static_cast<size_t>(kMaxName)) {
    size_t cut = static_cast<size_t>(kMaxName);
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;  // never split a UTF-8 character
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

std::string freeName(const Set& s, const std::string& base) {
  for (int n = static_cast<int>(s.items.size()) + 1;; ++n) {
    const std::string name = base + " " + std::to_string(n);
    if (!taken(s, name)) return name;
  }
}

int add(Set& s, const std::string& name, const Arrangement& a) {
  if (static_cast<int>(s.items.size()) >= kMaxWorkspaces) return -1;
  Workspace w;
  w.name = cleanName(name);
  if (w.name.empty() || taken(s, w.name)) w.name = freeName(s, w.name.empty() ? "Workspace" : w.name);
  w.saved = w.current = a;
  const int at = s.items.empty() ? 0 : std::clamp(s.active + 1, 0, static_cast<int>(s.items.size()));
  s.items.insert(s.items.begin() + at, w);
  s.active = at;
  return at;
}

bool rename(Set& s, int i, const std::string& name) {
  if (i < 0 || i >= static_cast<int>(s.items.size())) return false;
  const std::string n = cleanName(name);
  if (n.empty() || taken(s, n, i)) return false;
  s.items[static_cast<size_t>(i)].name = n;
  return true;
}

bool remove(Set& s, int i, Workspace* removed) {
  if (i < 0 || i >= static_cast<int>(s.items.size()) || s.items.size() <= 1) return false;
  if (removed) *removed = s.items[static_cast<size_t>(i)];
  s.items.erase(s.items.begin() + i);
  if (s.active > i) --s.active;
  clampActive(s);
  return true;
}

void restore(Set& s, const Workspace& w, int index) {
  if (static_cast<int>(s.items.size()) >= kMaxWorkspaces) return;
  Workspace v = w;
  if (taken(s, v.name)) v.name = freeName(s, v.name);
  const int at = std::clamp(index, 0, static_cast<int>(s.items.size()));
  s.items.insert(s.items.begin() + at, v);
  s.active = at;
}

int duplicate(Set& s, int i) {
  if (i < 0 || i >= static_cast<int>(s.items.size()) || static_cast<int>(s.items.size()) >= kMaxWorkspaces) return -1;
  Workspace w = s.items[static_cast<size_t>(i)];
  std::string base = cleanName(w.name + " copy");
  w.name = taken(s, base) ? freeName(s, base) : base;
  s.items.insert(s.items.begin() + i + 1, w);
  s.active = i + 1;
  return i + 1;
}

bool move(Set& s, int from, int to) {
  const int n = static_cast<int>(s.items.size());
  if (from < 0 || from >= n) return false;
  to = std::clamp(to, 0, n - 1);
  if (to == from) return false;
  const Workspace w = s.items[static_cast<size_t>(from)];
  const std::string activeName = s.items[static_cast<size_t>(s.active)].name;
  s.items.erase(s.items.begin() + from);
  s.items.insert(s.items.begin() + to, w);
  for (int k = 0; k < n; ++k)
    if (s.items[static_cast<size_t>(k)].name == activeName) s.active = k;
  return true;
}

void record(Set& s, const Arrangement& now) {
  if (s.items.empty()) return;
  clampActive(s);
  Workspace& w = s.items[static_cast<size_t>(s.active)];
  w.current = now;
  if (s.autoSave) w.saved = now;
}

const Arrangement* select(Set& s, int i, const Arrangement& now) {
  if (i < 0 || i >= static_cast<int>(s.items.size()) || i == s.active) return nullptr;
  record(s, now);
  s.active = i;
  return &s.items[static_cast<size_t>(i)].current;
}

void save(Set& s) {
  if (s.items.empty()) return;
  clampActive(s);
  Workspace& w = s.items[static_cast<size_t>(s.active)];
  w.saved = w.current;
}

const Arrangement* revert(Set& s) {
  if (s.items.empty()) return nullptr;
  clampActive(s);
  Workspace& w = s.items[static_cast<size_t>(s.active)];
  w.current = w.saved;
  return &w.current;
}

namespace {
Json arrangementJson(const Arrangement& a) {
  Json o = Json::makeObject();
  Json open = Json::makeArray();
  for (const std::string& w : a.open) open.array.push_back(Json::makeString(w));
  o.object["open"] = open;
  if (!a.regionState.empty()) o.object["state"] = Json::makeString(a.regionState);
  if (!a.dock.empty()) o.object["dock"] = Json::makeString(a.dock);
  return o;
}
Arrangement arrangementFrom(const Json* o) {
  Arrangement a;
  if (!o || !o->is(Json::Type::Object)) return a;
  if (const Json* open = o->find("open"); open && open->is(Json::Type::Array))
    for (const Json& v : open->array)
      if (v.is(Json::Type::String) && !v.str.empty() && v.str.size() < 200) a.open.insert(v.str);
  if (const Json* st = o->find("state"); st && st->is(Json::Type::String) && st->str.size() < 65536) a.regionState = st->str;
  if (const Json* d = o->find("dock"); d && d->is(Json::Type::String) && d->str.size() < 65536) a.dock = d->str;
  return a;
}
}  // namespace

std::string toJson(const Set& s) {
  Json o = Json::makeObject();
  Json items = Json::makeArray();
  for (const Workspace& w : s.items) {
    Json j = Json::makeObject();
    j.object["name"] = Json::makeString(w.name);
    j.object["saved"] = arrangementJson(w.saved);
    if (w.changed()) j.object["current"] = arrangementJson(w.current);
    auto flag = [&j](const char* key, bool v) {
      Json b;
      b.type = Json::Type::Bool;
      b.boolean = v;
      j.object[key] = b;
    };
    if (!w.keepWindows) flag("keepWindows", false);
    if (!w.keepPanes) flag("keepPanes", false);
    if (w.hasOwnBar()) j.object["ownBar"] = Json::makeString(w.ownBar);
    if (w.hasOwnPins()) j.object["ownPins"] = Json::makeString(w.ownPins);
    items.array.push_back(j);
  }
  o.object["items"] = items;
  o.object["active"] = Json::makeNumber(s.active);
  if (!s.autoSave) {
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = false;
    o.object["autoSave"] = b;
  }
  return jsonCompact(o);
}

Set fromJson(const std::string& text) {
  Set s;
  Json o;
  if (text.empty() || !jsonParse(text, o) || !o.is(Json::Type::Object)) return s;
  if (const Json* items = o.find("items"); items && items->is(Json::Type::Array))
    for (const Json& j : items->array) {
      if (!j.is(Json::Type::Object) || static_cast<int>(s.items.size()) >= kMaxWorkspaces) continue;
      Workspace w;
      if (const Json* n = j.find("name"); n && n->is(Json::Type::String)) w.name = cleanName(n->str);
      if (w.name.empty() || taken(s, w.name)) w.name = freeName(s, w.name.empty() ? "Workspace" : w.name);
      w.saved = arrangementFrom(j.find("saved"));
      const Json* cur = j.find("current");
      w.current = cur ? arrangementFrom(cur) : w.saved;
      if (const Json* k = j.find("keepWindows"); k && k->is(Json::Type::Bool)) w.keepWindows = k->boolean;
      if (const Json* k = j.find("keepPanes"); k && k->is(Json::Type::Bool)) w.keepPanes = k->boolean;
      if (const Json* b = j.find("ownBar"); b && b->is(Json::Type::String) && b->str.size() < 65536) w.ownBar = b->str;
      if (const Json* b = j.find("ownPins"); b && b->is(Json::Type::String) && b->str.size() < 65536) w.ownPins = b->str;
      s.items.push_back(w);
    }
  if (const Json* a = o.find("active"); a && a->is(Json::Type::Number) && a->number >= 0 && a->number < 1000) s.active = static_cast<int>(a->number);
  if (const Json* a = o.find("autoSave"); a && a->is(Json::Type::Bool)) s.autoSave = a->boolean;
  clampActive(s);
  if (s.autoSave)
    for (Workspace& w : s.items) w.saved = w.current;
  return s;
}

std::string settingsKey(Preset p) { return std::string("layout.") + layout::id(p) + ".workspaces"; }

const Workspace* activeWorkspace(const Set& s) {
  if (s.items.empty()) return nullptr;
  return &s.items[static_cast<size_t>(std::clamp(s.active, 0, static_cast<int>(s.items.size()) - 1))];
}

Workspace* activeWorkspace(Set& s) {
  if (s.items.empty()) return nullptr;
  clampActive(s);
  return &s.items[static_cast<size_t>(s.active)];
}

}  // namespace gl::workspaces
