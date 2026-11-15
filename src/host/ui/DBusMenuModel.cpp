// Gridline - the menus as a "dbusmenu" layout (K-112). See DBusMenuModel.h.
#include "DBusMenuModel.h"

namespace gl::host::dbusmenu {

std::string validUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0) {
      ++i;
      continue;
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    int len = 0;
    uint32_t cp = 0, min = 0;
    if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07, min = 0x10000;
    bool ok = len > 0 && i + static_cast<size_t>(len) <= n;
    for (int k = 1; ok && k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
      ok = (cc & 0xC0) == 0x80;
      cp = (cp << 6) | (cc & 0x3F);
    }
    ok = ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    if (ok) {
      out.append(s, i, static_cast<size_t>(len));
      i += static_cast<size_t>(len);
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return out;
}

std::string escapeMnemonic(const std::string& label) {
  std::string out;
  out.reserve(label.size());
  for (char c : label) {
    if (c == '_') out += '_';
    out += c;
  }
  return out;
}

std::vector<std::vector<std::string>> shortcutKeys(const std::string& shortcut) {
  if (shortcut.empty()) return {};
  std::vector<std::string> keys;
  std::string part;
  auto flush = [&] {
    if (part.empty()) return;
    if (part == "Ctrl" || part == "Control" || part == "Cmd") part = "Control";
    else if (part == "Alt" || part == "Option") part = "Alt";
    else if (part == "Shift") part = "Shift";
    else if (part == "Super" || part == "Win" || part == "Meta") part = "Super";
    keys.push_back(part);
    part.clear();
  };
  for (size_t i = 0; i < shortcut.size(); ++i) {
    const char c = shortcut[i];
    // "+" separates keys, unless it is the key itself ("Ctrl++", a trailing "+").
    if (c == '+' && !part.empty()) flush();
    else if (c == ' ') continue;
    else part += c;
  }
  flush();
  if (keys.empty()) return {};
  return {keys};
}

int Model::idFor(const std::string& key) {
  auto it = ids_.find(key);
  if (it != ids_.end()) return it->second;
  const int id = nextId_++;
  ids_[key] = id;
  return id;
}

void Model::add(const nativemenu::Item& it, int parent, const std::string& parentKey, std::map<std::string, int>& seen) {
  Entry e;
  e.parent = parent;
  e.separator = it.label.empty();
  // The key: the path and the occurrence among siblings of the same label (separators by place).
  const std::string base = parentKey + "\x1f" + (e.separator ? std::string("\x01sep") : it.label);
  const int n = seen[base]++;
  const std::string key = base + "\x1e" + std::to_string(n);
  e.id = idFor(key);
  if (!e.separator) {
    e.label = escapeMnemonic(validUtf8(it.label));
    e.enabled = it.enabled;
    e.checkable = it.checkable || it.checked;
    e.checked = it.checked;
    e.submenu = !it.children.empty();
    e.shortcut = shortcutKeys(validUtf8(it.shortcut));
    e.appId = it.id;
  }
  const size_t at = entries_.size();
  entries_.push_back(e);
  entries_[static_cast<size_t>(byId_[parent])].children.push_back(e.id);
  byId_[e.id] = at;
  std::map<std::string, int> childSeen;
  for (const nativemenu::Item& c : it.children) add(c, e.id, key, childSeen);
}

namespace {
bool same(const Entry& a, const Entry& b) {
  return a.id == b.id && a.parent == b.parent && a.label == b.label && a.enabled == b.enabled && a.separator == b.separator &&
         a.checkable == b.checkable && a.checked == b.checked && a.submenu == b.submenu && a.shortcut == b.shortcut && a.appId == b.appId &&
         a.children == b.children;
}
}  // namespace

bool Model::set(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus) {
  std::vector<Entry> old = std::move(entries_);
  entries_.clear();
  byId_.clear();
  Entry root;
  root.id = 0;
  root.submenu = true;
  entries_.push_back(root);
  byId_[0] = 0;
  std::map<std::string, int> seen;
  if (!appMenu.empty()) {
    nativemenu::Item app;
    app.label = "Gridline";
    app.children = appMenu;
    add(app, 0, "", seen);
  }
  for (const nativemenu::Item& m : menus) add(m, 0, "", seen);
  bool changed = old.size() != entries_.size();
  for (size_t i = 0; !changed && i < old.size(); ++i) changed = !same(old[i], entries_[i]);
  if (changed) ++revision_;
  return changed;
}

const Entry* Model::entry(int id) const {
  auto it = byId_.find(id);
  return it == byId_.end() ? nullptr : &entries_[it->second];
}

std::string Model::appIdOf(int id) const {
  const Entry* e = entry(id);
  return e && !e->submenu && !e->separator ? e->appId : std::string();
}

}  // namespace gl::host::dbusmenu
