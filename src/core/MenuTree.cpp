// Gridline - the menu tree (K-112). See MenuTree.h.
#include "MenuTree.h"

namespace gl::menutree {

std::vector<std::string> Ref::rawPath() const {
  std::vector<std::string> v;
  for (size_t i = 1; i < path.size(); ++i) v.push_back(path[i]->raw);
  return v;
}

namespace {

void walk(const Node& n, int root, std::vector<const Node*>& path, std::vector<Ref>& out) {
  if (!n.submenu) {
    out.push_back({root, path, &n});
    return;
  }
  path.push_back(&n);
  for (const Node& c : n.children) walk(c, root, path, out);
  path.pop_back();
}

void count(const Node& n, int& items, int& subs) {
  if (!n.submenu) {
    ++items;
    return;
  }
  ++subs;
  for (const Node& c : n.children) count(c, items, subs);
}

}  // namespace

std::vector<Ref> items(const Tree& t) {
  std::vector<Ref> out;
  std::vector<const Node*> path;
  for (size_t i = 0; i < t.roots.size(); ++i) walk(t.roots[i], static_cast<int>(i), path, out);
  return out;
}

int countItems(const Tree& t) {
  int items = 0, subs = 0;
  for (const Node& r : t.roots) count(r, items, subs);
  return items;
}

int countSubmenus(const Tree& t) {
  int items = 0, subs = 0;
  for (const Node& r : t.roots) count(r, items, subs);
  return subs;
}

const Node* find(const Tree& t, int root, const std::vector<std::string>& rawPath, const std::string& rawLabel, int occ) {
  if (root < 0 || root >= static_cast<int>(t.roots.size())) return nullptr;
  const Node* at = &t.roots[static_cast<size_t>(root)];
  for (const std::string& r : rawPath) {
    const Node* next = nullptr;
    for (const Node& c : at->children)
      if (c.submenu && c.raw == r) {
        next = &c;
        break;
      }
    if (!next) return nullptr;
    at = next;
  }
  for (const Node& c : at->children)
    if (!c.submenu && c.raw == rawLabel && c.occ == occ) return &c;
  return nullptr;
}

std::vector<Ref> byCommand(const Tree& t, const std::string& command) {
  std::vector<Ref> out;
  if (command.empty()) return out;
  for (const Ref& r : items(t))
    if (r.item->command == command) out.push_back(r);
  return out;
}

std::string dump(const Tree& t) {
  std::string out;
  for (const Ref& r : items(t)) {
    std::string line;
    for (const Node* p : r.path) line += p->en + " > ";
    line += r.item->en;
    line += "\t" + r.item->shortcut + "\t" + r.item->command + "\t";
    line += r.item->enabled ? "e" : "-";
    line += r.item->checked ? "c" : "-";
    out += line + "\n";
  }
  return out;
}

}  // namespace gl::menutree
