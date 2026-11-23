// Gridline - the region shell of a layout (K-112). See LayoutShell.h.
#include "LayoutShell.h"
#include <algorithm>
#include <cmath>
#include "Json.h"
#include "LayoutHomes.h"

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
  // R09-06: the Library takes 14 % of the window (about 200 px at 1440, 270 at 1920), between 190 and
  // 300 px, and a drag may widen it to 420 px, as the reference's Library is sized; the lanes keep
  // most of a small window.
  RegionSize lib = share(0.14f, 190, 0.22f);
  lib.maxPx = 300, lib.dragMaxPx = 420;
  s.regions = {R("logic.library", "", Edge::Left, lib, false, false, TabStyle::None),
               R("logic.inspector", "", Edge::Left, px(230, 0.2f, 180), true, false, TabStyle::None),  // K-112D (audit E7): 105 px strips
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
// Checkpoint 58 (N57-05, N57-06, N57-15): the detail view grows with the window: 236 px at 1440 x 900 (a
// share of the height under the top bar), up to 320 px on a large screen, and at least 180 px.
RegionSize detailSize(float maxShare) {
  RegionSize s;
  s.share = 0.289f;
  s.minPx = 180.0f;
  s.maxPx = 320.0f;
  s.maxShare = maxShare;
  return s;
}
Shell live() {
  Shell s;
  s.preset = Preset::Live;
  s.centre = "live.main";
  s.regions = {R("live.browser", "", Edge::Left, px(292, 0.25f, 200)),
               // Checkpoint 55 (gap 9): a small corner (the reference's Info View), shown or hidden by the
               // status bar's info button, View > Info View or Shift+?.
               R("live.info", "live.browser", Edge::Bottom, px(128, 0.3f, 80), true, false, TabStyle::None, true),
               R("live.side", "", Edge::Right, px(300, 0.3f, 200), false, false, TabStyle::Rail),  // K-237: its tools on a tab rail
               // Checkpoint 57 (N56-08): the device band and the clip band are one detail view of one fixed
               // height (about 230 at 1440 x 900, as in the refs), so the main view keeps its place when the
               // selection moves between a clip and a device; a drag of either divider sizes both.
               // 0.7.8: one spot of one size: both take the same rule (the clip view had a larger share
               // on a short window, so the switch moved the main view's edge).
               R("live.device", "", Edge::Bottom, detailSize(0.4f), true, false, TabStyle::None, true),
               R("live.clip", "", Edge::Bottom, detailSize(0.4f), false, false, TabStyle::Dock, true),
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
  // K-106 cp14: 248 (was 232): the order list's five entries in the band's text size took the
  // height from the scope box's tools; the classic band is taller still for its window.
  s.regions = {R("tracker.header", "", Edge::Top, px(248, 0.35f, 120), true, false, TabStyle::Dock, true),
               R("tracker.side", "", Edge::Right, px(190, 0.25f, 150), false, false, TabStyle::Rail),  // K-237: its tools on a tab rail
               R("tracker.lower", "", Edge::Bottom, px(150, 0.3f, 100), false, false, TabStyle::Dock, true)};
  s.regions[0].keep = true;  // gap-1118: the header band is the Tracker's own (its boxes, the page strip)
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

bool hasPaneTabs(const Region& r) { return r.tabs == TabStyle::Segmented || r.tabs == TabStyle::Rail; }

std::string tabRegion(Preset p, const State& st, const std::string& window) {
  if (auto it = st.moved.find(window); it != st.moved.end()) return it->second;
  const Home h = windowHome(window, p);
  return (h.kind == HomeKind::Dock || h.kind == HomeKind::Part) ? h.region : "";
}

bool canMoveTab(Preset p, const std::string& window, const std::string& to) {
  const Home h = windowHome(window, p);
  if ((h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) return false;
  const Region* from = findRegion(p, h.region);
  const Region* dest = findRegion(p, to);
  return from && dest && hasPaneTabs(*from) && hasPaneTabs(*dest);
}

bool moveTab(Preset p, State& st, const std::string& window, const std::string& to) {
  if (!canMoveTab(p, window, to) || tabRegion(p, st, window) == to) return false;
  if (to == windowHome(window, p).region) st.moved.erase(window);
  else st.moved[window] = to;
  return true;
}

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
  // K-112c: one region of a group at a time. 0.7.8: always (the Clips clip view and device view share
  // one spot and no longer stack), so `alone` only matters to regions outside a group.
  (void)alone;
  if (on && self && *self->group)
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
  if (!st.tabs.empty()) {
    Json m = Json::makeObject();
    for (const auto& [k, v] : st.tabs) m.object[k] = Json::makeString(v);
    o.object["tabs"] = m;
  }
  if (!st.maximised.empty()) o.object["max"] = list(st.maximised);
  if (!st.moved.empty()) {
    Json m = Json::makeObject();
    for (const auto& [k, v] : st.moved) m.object[k] = Json::makeString(v);
    o.object["moved"] = m;
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
  if (const Json* m = o.find("tabs"); m && m->is(Json::Type::Object))
    for (const auto& [k, v] : m->object)
      if (known(k) && v.is(Json::Type::String) && !v.str.empty() && v.str.size() < 4096) st.tabs[k] = v.str;
  if (const Json* a = o.find("max"); a && a->is(Json::Type::Array))
    for (const Json& v : a->array)
      if (v.is(Json::Type::String) && known(v.str)) st.maximised.insert(v.str);
  if (const Json* m = o.find("moved"); m && m->is(Json::Type::Object))  // K-237: only moves the rules allow
    for (const auto& [k, v] : m->object)
      if (v.is(Json::Type::String) && canMoveTab(p, k, v.str) && v.str != windowHome(k, p).region) st.moved[k] = v.str;
  return st;
}

std::string stateKey(Preset p) { return std::string("layout.") + id(p) + ".state"; }

float regionExtent(const Region& r, float parent, const State& st, const std::string& member) {
  float v = r.size.px > 0 ? r.size.px : std::floor(parent * r.size.share);
  // K-297: where the person dragged it. Their size may go past the layout's own limit (which sizes the
  // default on small windows), up to 70 % of the parent, so a band can be pulled open to show a whole editor.
  bool dragged = false;
  if (auto it = st.regionSize.find(r.id); it != st.regionSize.end()) v = it->second, dragged = true;
  // rsz-1118: a member's own size is a dragged one too (Smooth's lower pane let go above its default
  // share came back smaller on the next rebuild).
  if (!member.empty()) {
    if (auto it = st.memberSize.find(std::string(r.id) + "|" + member); it != st.memberSize.end()) {
      v = it->second, dragged = true;
    }
  }
  if (dragged) {
    float lo = 0, hi = 0;
    dragRange(r, parent, lo, hi);
    v = std::clamp(v, lo, hi);
  } else {
    if (r.size.maxPx > 0) {
      v = std::min(v, r.size.maxPx);
    }
    if (r.size.maxShare > 0) {
      v = std::min(v, std::floor(parent * r.size.maxShare));
    }
    v = std::max(v, std::min(r.size.minPx, std::floor(parent * 0.5f)));
  }
  // K-237: a maximised pane takes most of its parent (the main area keeps the rest) until restored.
  if (st.maximised.count(r.id)) v = std::floor(parent * kMaximisedShare);
  return std::floor(std::clamp(v, 0.0f, std::max(0.0f, parent - 1.0f)));
}

void dragRange(const Region& r, float parent, float& lo, float& hi) {
  hi = std::max(0.0f, parent - 1.0f);
  if (r.size.dragMaxPx > 0) {
    hi = std::min(hi, r.size.dragMaxPx);
  }
  if (r.size.maxShare > 0) {
    hi = std::min(hi, std::floor(parent * std::max(r.size.maxShare, 0.7f)));
  }
  lo = std::min(std::min(r.size.minPx, std::floor(parent * 0.5f)), hi);
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
