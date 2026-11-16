// Gridline - the region shell of a layout (K-112). See LayoutShell.h.
#include "LayoutShell.h"
#include <algorithm>
#include <cmath>
#include "Json.h"

namespace gl::layout {

namespace {

RegionSize px(float v, float maxShare = 0, float minPx = 0) {
  RegionSize s;
  s.px = v;
  s.maxShare = maxShare;
  s.minPx = minPx;
  return s;
}
RegionSize share(float v, float minPx, float maxShare) {
  RegionSize s;
  s.share = v;
  s.minPx = minPx;
  s.maxShare = maxShare;
  return s;
}

Region R(const char* id, const char* parent, Edge e, RegionSize sz, bool visible = true, bool exclusive = false,
         TabStyle tabs = TabStyle::Dock, bool fixed = false) {
  Region r;
  r.id = id;
  r.parent = parent;
  r.edge = e;
  r.size = sz;
  r.visibleByDefault = visible;
  r.exclusive = exclusive;
  r.tabs = tabs;
  r.fixed = fixed;
  return r;
}

Shell studio() {
  Shell s;
  s.preset = Preset::Studio;
  s.centre = "studio.main";
  s.regions = {R("studio.browser", "", Edge::Left, share(0.15f, 180, 0.35f)),
               R("studio.mixer", "", Edge::Right, px(338, 0.35f, 180))};
  return s;
}

// Logic: two left columns (library, inspector), the browser column on the right (one of several
// panels at a time), one lower pane under the tracks (editors or mixer, one at a time).
Shell logic() {
  Shell s;
  s.preset = Preset::Logic;
  s.centre = "logic.tracks";
  s.centreTabs = TabStyle::None;
  // Sizes at 1440x900 as the approved picture: Library 240, Inspector 200, right pane 300 (with the
  // quick help box along its lower edge), the lower pane half of the centre column.
  s.regions = {R("logic.library", "", Edge::Left, px(240, 0.2f, 180), false, false, TabStyle::None),
               R("logic.inspector", "", Edge::Left, px(200, 0.2f, 180), true, false, TabStyle::None),
               // K-112b8: the pads grid beside the tracks (hidden until shown; Tab or the tracks bar's grid switch)
               R("logic.grid", "", Edge::Left, px(360, 0.3f, 220), false, false, TabStyle::None),
               R("logic.right", "", Edge::Right, px(300, 0.3f, 220), false, true, TabStyle::Segmented),
               // R01-08: closed by default (Quick help is a floating window; the box shows when the Info box part is opened)
               R("logic.help", "logic.right", Edge::Bottom, px(120, 0.4f, 72), false, false, TabStyle::None, true),
               R("logic.lower", "", Edge::Bottom, share(0.5f, 160, 0.62f), false, true, TabStyle::Segmented)};  // K-112B cp40: closed until asked for
  return s;
}

// Live: the browser on the left with the info corner under it, a side panel on the right (closed),
// the device band (fixed height) or the clip band along the bottom, the mixer band under the main view.
// K-112c (round 45): a new set opens as the reference's does: the browser, the clip grid with its
// own mixer, and one detail band (the device band). The clip and device bands are one group: showing
// one hides the other, so the main view keeps its room.
Shell live() {
  Shell s;
  s.preset = Preset::Live;
  s.centre = "live.main";
  s.regions = {R("live.browser", "", Edge::Left, px(292, 0.25f, 200)),
               R("live.info", "live.browser", Edge::Bottom, px(188, 0.4f, 100), true, false, TabStyle::None, true),
               R("live.side", "", Edge::Right, px(300, 0.3f, 200), false),
               R("live.device", "", Edge::Bottom, px(188, 0.35f, 120), true, false, TabStyle::None, true),
               R("live.clip", "", Edge::Bottom, share(0.36f, 160, 0.45f), false),  // about 230 at 1440 x 900, as in the refs
               R("live.mixer", "", Edge::Bottom, share(0.32f, 150, 0.5f), false)};
  for (Region& r : s.regions)
    if (std::string(r.id) == "live.device" || std::string(r.id) == "live.clip") r.group = "live.detail";
  return s;
}

// Tracker: a header band of boxes on top, pages in the centre, a lower frame and a side panel (closed).
Shell tracker() {
  Shell s;
  s.preset = Preset::Tracker;
  s.centre = "tracker.pages";
  s.centreTabs = TabStyle::Pages;  // K-106: one page at a time, picked in the header band's page strip
  s.regions = {R("tracker.header", "", Edge::Top, px(232, 0.35f, 120), true, false, TabStyle::Dock, true),
               R("tracker.side", "", Edge::Right, px(190, 0.25f, 150), false),
               R("tracker.lower", "", Edge::Bottom, px(150, 0.3f, 100), false, false, TabStyle::Dock, true)};
  return s;
}

bool horizontal(Edge e) { return e == Edge::Left || e == Edge::Right; }

}  // namespace

const Shell& shell(Preset p) {
  static const Shell s[kLayoutCount] = {studio(), live(), logic(), tracker()};
  const int i = static_cast<int>(p);
  return s[i >= 0 && i < kLayoutCount ? i : 0];
}

const Region* findRegion(Preset p, const std::string& id) {
  for (const Region& r : shell(p).regions)
    if (id == r.id) return &r;
  return nullptr;
}

bool hasRegion(Preset p, const std::string& id) { return id == shell(p).centre || findRegion(p, id) != nullptr; }

bool visible(const Shell& sh, const State& st, const std::string& region) {
  if (region == sh.centre) return true;
  for (const Region& r : sh.regions)
    if (region == r.id) {
      if (*r.parent && !visible(sh, st, r.parent)) return false;
      return r.visibleByDefault ? !st.hidden.count(region) : st.shown.count(region) > 0;
    }
  return false;
}

void setVisible(const Shell& sh, State& st, const std::string& region, bool on, bool alone) {
  const Region* self = nullptr;
  for (const Region& r : sh.regions)
    if (region == r.id) self = &r;
  if (on && alone && self && *self->group)  // K-112c: one region of a group at a time (unless stacked)
    for (const Region& r : sh.regions)
      if (region != r.id && std::string(r.group) == self->group && visible(sh, st, r.id)) setVisible(sh, st, r.id, false);
  for (const Region& r : sh.regions)
    if (region == r.id) {
      if (r.visibleByDefault) {
        if (on) st.hidden.erase(region);
        else st.hidden.insert(region);
      } else {
        if (on) st.shown.insert(region);
        else st.shown.erase(region);
      }
    }
}

std::string toJson(const State& st) {
  if (st == State{}) return "";
  Json o = Json::makeObject();
  auto list = [](const std::set<std::string>& s) {
    Json a = Json::makeArray();
    for (const std::string& v : s) a.array.push_back(Json::makeString(v));
    return a;
  };
  if (!st.hidden.empty()) o.object["hidden"] = list(st.hidden);
  if (!st.shown.empty()) o.object["shown"] = list(st.shown);
  if (!st.member.empty()) {
    Json m = Json::makeObject();
    for (const auto& [k, v] : st.member) m.object[k] = Json::makeString(v);
    o.object["member"] = m;
  }
  if (!st.memberSize.empty()) {
    Json m = Json::makeObject();
    for (const auto& [k, v] : st.memberSize) m.object[k] = Json::makeNumber(std::round(static_cast<double>(v)));
    o.object["size"] = m;
  }
  if (!st.regionSize.empty()) {
    Json m = Json::makeObject();
    for (const auto& [k, v] : st.regionSize) m.object[k] = Json::makeNumber(std::round(static_cast<double>(v)));
    o.object["region"] = m;
  }
  return jsonCompact(o);
}

State fromJson(Preset p, const std::string& text) {
  State st;
  Json o;
  if (text.empty() || !jsonParse(text, o) || !o.is(Json::Type::Object)) return st;
  auto known = [p](const std::string& id) { return findRegion(p, id) != nullptr; };
  for (const char* key : {"hidden", "shown"})
    if (const Json* a = o.find(key); a && a->is(Json::Type::Array))
      for (const Json& v : a->array)
        if (v.is(Json::Type::String) && known(v.str)) (std::string(key) == "hidden" ? st.hidden : st.shown).insert(v.str);
  if (const Json* m = o.find("member"); m && m->is(Json::Type::Object))
    for (const auto& [k, v] : m->object)
      if (known(k) && v.is(Json::Type::String) && !v.str.empty()) st.member[k] = v.str;
  if (const Json* m = o.find("size"); m && m->is(Json::Type::Object))
    for (const auto& [k, v] : m->object) {
      const size_t bar = k.find('|');
      if (bar == std::string::npos || !known(k.substr(0, bar)) || !v.is(Json::Type::Number) || !std::isfinite(v.number)) continue;
      st.memberSize[k] = static_cast<float>(std::clamp(v.number, 40.0, 4000.0));
    }
  if (const Json* m = o.find("region"); m && m->is(Json::Type::Object))
    for (const auto& [k, v] : m->object)
      if (known(k) && v.is(Json::Type::Number) && std::isfinite(v.number)) st.regionSize[k] = static_cast<float>(std::clamp(v.number, 40.0, 4000.0));
  return st;
}

std::string stateKey(Preset p) { return std::string("layout.") + id(p) + ".state"; }

float regionExtent(const Region& r, float parent, const State& st, const std::string& member) {
  float v = r.size.px > 0 ? r.size.px : std::floor(parent * r.size.share);
  // K-297: where the person dragged it. Their size may go past the layout's own limit (which sizes the
  // default on small windows), up to 70 % of the parent, so a band can be pulled open to show a whole editor.
  bool dragged = false;
  if (auto it = st.regionSize.find(r.id); it != st.regionSize.end()) v = it->second, dragged = true;
  if (!member.empty())
    if (auto it = st.memberSize.find(std::string(r.id) + "|" + member); it != st.memberSize.end()) v = it->second;
  if (r.size.maxPx > 0 && !dragged) v = std::min(v, r.size.maxPx);
  if (r.size.maxShare > 0) v = std::min(v, std::floor(parent * (dragged ? std::max(r.size.maxShare, 0.7f) : r.size.maxShare)));
  v = std::max(v, std::min(r.size.minPx, std::floor(parent * 0.5f)));
  return std::floor(std::clamp(v, 0.0f, std::max(0.0f, parent - 1.0f)));
}

std::vector<Placed> solve(Preset p, float w, float h, const State& st) {
  const Shell& sh = shell(p);
  std::map<std::string, workspace::Rect> rest;  // what is left of each parent ("" = the whole area)
  rest[""] = {0, 0, std::max(w, 1.0f), std::max(h, 1.0f)};
  std::vector<Placed> out;
  for (const Region& r : sh.regions) {
    Placed pl;
    pl.id = r.id;
    pl.visible = visible(sh, st, r.id) && rest.count(r.parent);
    if (pl.visible) {
      workspace::Rect& pr = rest[r.parent];
      const bool hz = horizontal(r.edge);
      const std::string member = r.exclusive && st.member.count(r.id) ? st.member.at(r.id) : std::string();
      const float e = regionExtent(r, hz ? pr.w : pr.h, st, member);
      switch (r.edge) {
        case Edge::Left: pl.rect = {pr.x, pr.y, e, pr.h}; pr.x += e; pr.w -= e; break;
        case Edge::Right: pl.rect = {pr.x + pr.w - e, pr.y, e, pr.h}; pr.w -= e; break;
        case Edge::Top: pl.rect = {pr.x, pr.y, pr.w, e}; pr.y += e; pr.h -= e; break;
        case Edge::Bottom: pl.rect = {pr.x, pr.y + pr.h - e, pr.w, e}; pr.h -= e; break;
      }
      rest[r.id] = pl.rect;  // children split what is left of it
    }
    out.push_back(pl);
  }
  // A region with children ends up as what its children left.
  for (Placed& pl : out)
    if (pl.visible) pl.rect = rest[pl.id];
  out.push_back({sh.centre, rest[""], true});
  return out;
}

}  // namespace gl::layout
