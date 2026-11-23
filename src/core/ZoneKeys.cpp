// Gridline - zone keys for the Studio layout. See ZoneKeys.h.
#include "ZoneKeys.h"
#include <algorithm>

namespace gl::zones {

Action Layer::tabDown(bool repeat) {
  if (repeat && held_) return Action{Action::None, 0, true};
  held_ = true;
  used_ = false;
  return Action{Action::None, 0, true};
}

Action Layer::digit(int n) {
  if (!held_ || n < 1 || n > kZones) return {};
  used_ = true;
  return Action{Action::Zone, n, true};
}

Action Layer::tabUp() {
  if (!held_) return {};
  held_ = false;
  if (used_) return Action{Action::None, 0, true};
  return Action{Action::Back, 0, true};
}

void Layer::cancel() {
  held_ = false;
  used_ = true;
}

Zones defaults() {
  Zones z;
  z[0].windows = {"Playlist", ""};
  z[1].windows = {"Piano roll", ""};
  z[2].windows = {"Mixer", "Channel rack"};
  return z;
}

std::string toText(const Zones& z) {
  std::string out;
  for (int i = 0; i < kZones; ++i) {
    const Zone& zone = z[static_cast<size_t>(i)];
    if (zone.empty()) continue;
    if (!out.empty()) out += ';';
    out += std::to_string(i + 1) + "=" + zone.windows[0];
    if (!zone.windows[1].empty()) out += "|" + zone.windows[1];
  }
  return out;
}

namespace {
bool cleanName(const std::string& s) {
  if (s.size() > 120) return false;
  for (char c : s)
    if (c == ';' || c == '|' || c == '=' || static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}
}  // namespace

Zones fromText(const std::string& text) {
  if (text.empty()) return defaults();
  Zones z;
  size_t at = 0;
  while (at <= text.size()) {
    const size_t end = std::min(text.find(';', at), text.size());
    const std::string part = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string num = part.substr(0, eq);
    if (num.size() != 1 || num[0] < '1' || num[0] > '9') continue;
    const int n = num[0] - '0';
    std::string a = part.substr(eq + 1), b;
    if (const size_t bar = a.find('|'); bar != std::string::npos) b = a.substr(bar + 1), a = a.substr(0, bar);
    if (!cleanName(a) || !cleanName(b)) continue;
    if (a.empty()) a.swap(b);
    if (a == b) b.clear();
    z[static_cast<size_t>(n - 1)].windows = {a, b};
  }
  return z;
}

int pick(const Zone& z, bool alreadyThere, int here) {
  if (z.empty()) return -1;
  if (z.count() == 1) return z.windows[0].empty() ? 1 : 0;
  if (!alreadyThere) return 0;
  return here == 0 ? 1 : 0;
}

void History::visit(int zone) {
  if (zone <= 0 || zone == cur_) return;
  prev_ = cur_;
  cur_ = zone;
}

int History::back() {
  if (prev_ <= 0) return 0;
  const int to = prev_;
  prev_ = cur_;
  cur_ = to;
  return to;
}

}  // namespace gl::zones
