// Gridline - where every part of the app lives in each layout (K-112). See LayoutHomes.h.
#include "LayoutHomes.h"
#include <cstring>

namespace gl::layout {

const char* kindName(HomeKind k) {
  switch (k) {
    case HomeKind::Dock: return "dock";
    case HomeKind::Float: return "float";
    case HomeKind::Modal: return "modal";
    case HomeKind::Host: return "host";
    case HomeKind::Inside: return "inside";
    case HomeKind::Overlay: return "overlay";
    case HomeKind::Bar: return "bar";
    case HomeKind::Menu: return "menu";
    case HomeKind::Unchanged: return "unchanged";
    case HomeKind::Part: return "part";
  }
  return "?";
}

const std::vector<Item>& items() {
  static const std::vector<Item> table = {
#include "LayoutHomesTable.inc"
  };
  return table;
}

const Item* findItem(const std::string& id) {
  for (const Item& i : items())
    if (id == i.id) return &i;
  return nullptr;
}

const Item* findWindow(const std::string& stableName) {
  if (stableName.empty()) return nullptr;
  for (const Item& i : items())
    if (*i.window && stableName == i.window) return &i;
  // A window known by its id alone ("###autoedit") also answers to the id without the marker
  // (the stable name ui::stableWindowName gives).
  for (const Item& i : items())
    if (std::strncmp(i.window, "###", 3) == 0 && stableName == i.window + 3) return &i;
  return nullptr;
}

const Item* findPlugin(const std::string& type) {
  if (type.empty()) return nullptr;
  for (const Item& i : items())
    if (*i.plugin && type == i.plugin) return &i;
  return nullptr;
}

Home windowHome(const std::string& stableName, Preset p) {
  const int k = static_cast<int>(p);
  if (k < 0 || k >= kLayoutCount) return {};
  if (const Item* i = findWindow(stableName)) return i->home[k];
  return {};
}

std::string regionPrefix(Preset p) { return std::string(id(p)) + "."; }

}  // namespace gl::layout
