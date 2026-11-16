// Gridline - K-237: pane tab strips (see PaneTabs.h).
#include "PaneTabs.h"
#include <algorithm>

namespace gl::panetabs {

namespace {
int indexOf(const Strip& s, const std::string& name) {
  auto it = std::find(s.order.begin(), s.order.end(), name);
  return it == s.order.end() ? -1 : static_cast<int>(it - s.order.begin());
}
int pinnedCount(const Strip& s) {
  int n = 0;
  for (const auto& t : s.order) n += s.pinned.count(t) ? 1 : 0;
  return n;
}
// Pinned tabs first, each group in its own order.
void pinsFirst(Strip& s) {
  std::stable_partition(s.order.begin(), s.order.end(), [&](const std::string& t) { return s.pinned.count(t) > 0; });
}
}  // namespace

void sync(Strip& s, const std::vector<std::string>& members) {
  std::vector<std::string> out;
  for (const auto& t : s.order)
    if (std::find(members.begin(), members.end(), t) != members.end() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  for (const auto& m : members)
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  s.order = out;
  for (auto it = s.pinned.begin(); it != s.pinned.end();) it = indexOf(s, *it) < 0 ? s.pinned.erase(it) : std::next(it);
  pinsFirst(s);
  if (indexOf(s, s.active) < 0) s.active = s.order.empty() ? std::string() : s.order.front();
}

bool move(Strip& s, const std::string& name, int to) {
  const int from = indexOf(s, name);
  if (from < 0) return false;
  const int pins = pinnedCount(s);
  const bool pin = s.pinned.count(name) > 0;
  const int lo = pin ? 0 : pins, hi = pin ? pins - 1 : static_cast<int>(s.order.size()) - 1;
  to = std::clamp(to, lo, hi);
  if (to == from) return false;
  s.order.erase(s.order.begin() + from);
  s.order.insert(s.order.begin() + to, name);
  return true;
}

bool setPinned(Strip& s, const std::string& name, bool pin) {
  if (indexOf(s, name) < 0 || (s.pinned.count(name) > 0) == pin) return false;
  const int from = indexOf(s, name);
  s.order.erase(s.order.begin() + from);
  if (pin) {
    const int pins = pinnedCount(s);
    s.pinned.insert(name);
    s.order.insert(s.order.begin() + pins, name);
  } else {
    s.pinned.erase(name);
    s.order.insert(s.order.begin() + pinnedCount(s), name);
  }
  return true;
}

void History::push(const Closed& c) {
  forget(c.window);
  items_.push_back(c);
  if (items_.size() > kMax) items_.erase(items_.begin());
}
Closed History::pop() {
  if (items_.empty()) return {};
  Closed c = items_.back();
  items_.pop_back();
  return c;
}
void History::forget(const std::string& window) {
  items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Closed& c) { return c.window == window; }), items_.end());
}

bool close(Strip& s, const std::string& name, const std::string& region, History& h) {
  const int i = indexOf(s, name);
  if (i < 0 || s.pinned.count(name) || s.order.size() <= 1) return false;
  s.order.erase(s.order.begin() + i);
  if (s.active == name) s.active = s.order[static_cast<size_t>(std::min(i, static_cast<int>(s.order.size()) - 1))];
  h.push({region, name, i});
  return true;
}

void reopen(Strip& s, const Closed& c) {
  if (c.window.empty()) return;
  if (indexOf(s, c.window) < 0) {
    const int at = std::clamp(c.index, pinnedCount(s), static_cast<int>(s.order.size()));
    s.order.insert(s.order.begin() + at, c.window);
  }
  s.active = c.window;
}

std::string cycle(const Strip& s, int dir) {
  if (s.order.empty()) return "";
  const int n = static_cast<int>(s.order.size());
  const int i = indexOf(s, s.active);
  if (i < 0) return s.order.front();
  return s.order[static_cast<size_t>(((i + dir) % n + n) % n)];
}

std::vector<int> fit(const Strip& s, const std::vector<float>& widths, float avail, float overflowW) {
  const int n = static_cast<int>(std::min(widths.size(), s.order.size()));
  std::vector<int> all;
  float total = 0;
  for (int i = 0; i < n; ++i) all.push_back(i), total += widths[static_cast<size_t>(i)];
  if (total <= avail) return all;
  // Keep pinned tabs and the active one, then fill in strip order.
  float room = avail - overflowW;
  std::vector<bool> keep(static_cast<size_t>(n), false);
  for (int i = 0; i < n; ++i) {
    const std::string& t = s.order[static_cast<size_t>(i)];
    if (s.pinned.count(t) || t == s.active) keep[static_cast<size_t>(i)] = true, room -= widths[static_cast<size_t>(i)];
  }
  for (int i = 0; i < n; ++i) {
    if (keep[static_cast<size_t>(i)]) continue;
    if (widths[static_cast<size_t>(i)] <= room) keep[static_cast<size_t>(i)] = true, room -= widths[static_cast<size_t>(i)];
    else break;  // the strip keeps its order: the rest go to the overflow menu
  }
  std::vector<int> out;
  for (int i = 0; i < n; ++i)
    if (keep[static_cast<size_t>(i)]) out.push_back(i);
  return out;
}

std::string save(const Strip& s) {
  std::string o;
  for (const auto& t : s.order) o += (o.empty() ? "" : "|") + t;
  std::string p;
  for (const auto& t : s.order)
    if (s.pinned.count(t)) p += (p.empty() ? "" : "|") + t;
  return o + ";pinned=" + p + ";active=" + s.active;
}

Strip load(const std::string& text) {
  Strip s;
  auto split = [](const std::string& v, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= v.size()) {
      const size_t b = v.find(sep, a);
      const std::string part = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
      if (!part.empty()) out.push_back(part);
      if (b == std::string::npos) break;
      a = b + 1;
    }
    return out;
  };
  const auto fields = split(text, ';');
  for (size_t i = 0; i < fields.size(); ++i) {
    const std::string& f = fields[i];
    if (f.rfind("pinned=", 0) == 0) {
      for (const auto& t : split(f.substr(7), '|')) s.pinned.insert(t);
    } else if (f.rfind("active=", 0) == 0) {
      s.active = f.substr(7);
    } else if (i == 0) {
      for (const auto& t : split(f, '|'))
        if (std::find(s.order.begin(), s.order.end(), t) == s.order.end()) s.order.push_back(t);
    }
  }
  for (auto it = s.pinned.begin(); it != s.pinned.end();) it = indexOf(s, *it) < 0 ? s.pinned.erase(it) : std::next(it);
  pinsFirst(s);
  if (indexOf(s, s.active) < 0) s.active = s.order.empty() ? std::string() : s.order.front();
  return s;
}

}  // namespace gl::panetabs
