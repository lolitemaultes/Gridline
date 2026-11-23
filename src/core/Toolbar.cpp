// Gridline - the control bar of each layout as a model. See Toolbar.h.
#include "Toolbar.h"
#include <algorithm>
#include <map>
#include <set>
#include "I18n.h"
#include "Json.h"

namespace gl::toolbar {

using layout::Preset;

namespace {

// Studio: the left side (from the mark) and the right side (to the window's edge). The fit levels give
// the order the groups always left a narrow window in: the output meter, the master controls, the window
// buttons, the position, the pattern picker, the menus, then the tempo; the transport stays.
const std::vector<PartInfo> kStudio = {
    {"menus", i18n::N_("Menus"), "left", 7, true, true},
    {"transport", i18n::N_("Song or pattern, play, stop and record"), "left", 0, true, true},
    {kPins, i18n::N_("Pinned commands"), "left", 4, true, true},  // (after the meter and the master: a pin a person chose stays a while)
    {"tempo", i18n::N_("Tempo and tap"), "left", 8, true, true},
    {"position", i18n::N_("Position"), "left", 5, true, true},
    {"pattern", i18n::N_("Pattern picker"), "left", 6, true, true},
    {"master", i18n::N_("Master volume, metronome and typing keyboard"), "right", 3, true, true},
    {"monitor", i18n::N_("Output meter and CPU"), "right", 2, true, true},
    {"windows", i18n::N_("Window buttons"), "right", 4, true, true},
};
const std::vector<SectionInfo> kStudioSections = {{"left", i18n::N_("Left side"), true}, {"right", i18n::N_("Right side"), true}};

// Clips: three groups, the middle one centred between the others. On a narrow window the loop range
// goes first, then the key and scale, then the CPU, draw and typing switches, then loop and punch.
const std::vector<PartInfo> kClips = {
    {"browser", i18n::N_("Browser button"), "left", 0, true, true},
    {"tempo", i18n::N_("Tap and tempo"), "left", 0, true, true},
    {"meter", i18n::N_("Meter"), "left", 0, true, true},
    {"metronome", i18n::N_("Metronome"), "left", 0, true, true},
    {"quantise", i18n::N_("Launch quantisation"), "left", 0, true, true},
    {"key", i18n::N_("Key and scale"), "left", 2, true, true},
    {"position", i18n::N_("Position"), "middle", 0, true, true},
    {"transport", i18n::N_("Play, stop and record"), "middle", 0, true, true},
    {"recording", i18n::N_("Overdub and automation arm"), "middle", 0, true, true},
    {"capture", i18n::N_("Capture"), "middle", 0, true, true},
    {"sessionRecord", i18n::N_("Session record"), "middle", 0, true, true},
    {"recordMenu", i18n::N_("Recording options"), "middle", 0, true, true},
    {kPins, i18n::N_("Pinned commands"), "middle", 0, true, true},
    {"back", i18n::N_("Back to arrangement"), "middle", 0, true, true},
    {"follow", i18n::N_("Follow"), "middle", 0, true, true},
    {"loop", i18n::N_("Loop"), "middle", 4, true, true},
    {"punch", i18n::N_("Punch in and out"), "middle", 4, true, true},
    {"range", i18n::N_("Loop range"), "middle", 1, true, true},
    {"cpu", i18n::N_("CPU"), "right", 3, true, true},
    {"draw", i18n::N_("Draw mode"), "right", 3, true, true},
    {"typing", i18n::N_("Typing keyboard"), "right", 3, true, true},
    {"view", i18n::N_("Grid and arrangement switch"), "right", 0, true, true},
};
const std::vector<SectionInfo> kClipsSections = {{"left", i18n::N_("Left"), true}, {"middle", i18n::N_("Middle"), true}, {"right", i18n::N_("Right"), true}};

// Smooth: the pane buttons and the right group keep their places; the transport's extra buttons can be
// switched off; the switches after the display can be reordered. Its own fit makes room in steps (the
// start and bar buttons, a compact master pill, a narrower display, the master, the switches).
const std::vector<PartInfo> kSmooth = {
    {"panes", i18n::N_("Pane buttons"), "left", 0, false, true},
    {"toStart", i18n::N_("Go to the start"), "transport", 1, false, false},
    {"nav", i18n::N_("Rewind and forward"), "transport", 1, false, true},
    {"cycle", i18n::N_("Cycle"), "transport", 0, false, true},
    {"display", i18n::N_("Display"), "display", 0, false, true},
    {"countIn", i18n::N_("Count-in"), "switches", 4, true, true},
    {"click", i18n::N_("Click"), "switches", 4, true, true},
    {"typing", i18n::N_("Typing keyboard"), "switches", 4, true, true},
    {kPins, i18n::N_("Pinned commands"), "switches", 0, true, true},
    {"master", i18n::N_("Master volume"), "right", 3, false, true},
    {"rightPanes", i18n::N_("Right pane buttons"), "right", 0, false, true},
};
const std::vector<SectionInfo> kSmoothSections = {
    {"left", i18n::N_("Left")}, {"transport", i18n::N_("Transport")}, {"display", i18n::N_("Display")}, {"switches", i18n::N_("Switches")}, {"right", i18n::N_("Right")}};

// The Tracker: its menu row has no free room; pinned commands go into the header's button grid.
const std::vector<PartInfo> kTracker = {{kPins, i18n::N_("Pinned commands"), "header", 0, false, true}};
const std::vector<SectionInfo> kTrackerSections = {{"header", i18n::N_("Header buttons")}};

Json boolJson(bool v) {
  Json b;
  b.type = Json::Type::Bool;
  b.boolean = v;
  return b;
}

bool boolOf(const Json& o, const char* key, bool dflt) {
  const Json* v = o.find(key);
  return v && v->is(Json::Type::Bool) ? v->boolean : dflt;
}

}  // namespace

const std::vector<PartInfo>& parts(Preset p) {
  switch (p) {
    case Preset::Live: return kClips;
    case Preset::Logic: return kSmooth;
    case Preset::Tracker: return kTracker;
    default: return kStudio;
  }
}

const std::vector<SectionInfo>& sections(Preset p) {
  switch (p) {
    case Preset::Live: return kClipsSections;
    case Preset::Logic: return kSmoothSections;
    case Preset::Tracker: return kTrackerSections;
    default: return kStudioSections;
  }
}

const PartInfo* part(Preset p, const std::string& id) {
  for (const PartInfo& i : parts(p))
    if (id == i.id) return &i;
  return nullptr;
}

namespace {
const SectionInfo* sectionInfo(Preset p, const std::string& id) {
  for (const SectionInfo& s : sections(p))
    if (id == s.id) return &s;
  return nullptr;
}
// The section a part stands in (its own when the side it names is not one it may stand in).
std::string effSection(Preset p, const Part& x) {
  const PartInfo* i = part(p, x.id);
  if (!i) return "";
  if (!x.section.empty() && x.section != i->section && canMoveInto(p, x.id, x.section)) return x.section;
  return i->section;
}
}  // namespace

bool canMoveInto(Preset p, const std::string& id, const std::string& section) {
  const PartInfo* i = part(p, id);
  if (!i) return false;
  if (section == i->section) return true;
  const SectionInfo* from = sectionInfo(p, i->section);
  const SectionInfo* to = sectionInfo(p, section);
  return i->movable && from && to && from->open && to->open;
}

std::string sectionOf(Preset p, const Bar& b, const std::string& id) {
  const Part* x = find(b, id);
  if (x) return effSection(p, *x);
  const PartInfo* i = part(p, id);
  return i ? i->section : "";
}

const char* whereName(Where w) {
  switch (w) {
    case Where::Bar: return i18n::N_("Bar only");
    case Where::Palette: return i18n::N_("Palette only");
    default: return i18n::N_("Bar and palette");
  }
}

Bar defaults(Preset p) {
  Bar b;
  for (const PartInfo& i : parts(p)) b.parts.push_back(Part{i.id, i.shown, false, {}});
  return b;
}

void normalise(Preset p, Bar& b) {
  std::map<std::string, Part> stored;
  std::vector<std::string> order;
  for (const Part& x : b.parts)
    if (part(p, x.id) && !stored.count(x.id)) {
      Part y = x;
      y.section = effSection(p, y);
      if (y.section == part(p, y.id)->section) y.section.clear();  // (its own side: not named)
      stored[x.id] = y;
      order.push_back(x.id);
    }
  auto standsIn = [&](const std::string& id) {
    const auto it = stored.find(id);
    return it == stored.end() || it->second.section.empty() ? std::string(part(p, id)->section) : it->second.section;
  };
  Bar out;
  for (const SectionInfo& sec : sections(p)) {
    std::vector<std::string> def;  // the section's own parts in default order
    for (const PartInfo& i : parts(p))
      if (sec.id == std::string(i.section)) def.push_back(i.id);
    std::vector<std::string> got;  // stored order of the movable ones standing in it (its own and moved in)
    for (const std::string& id : order)
      if (part(p, id)->movable && standsIn(id) == sec.id) got.push_back(id);
    // Missing movable parts: after their default predecessor (else first).
    for (size_t k = 0; k < def.size(); ++k) {
      if (!part(p, def[k])->movable || stored.count(def[k])) continue;
      size_t at = 0;
      for (size_t j = k; j-- > 0;) {
        const auto it = std::find(got.begin(), got.end(), def[j]);
        if (it != got.end()) {
          at = static_cast<size_t>(it - got.begin()) + 1;
          break;
        }
      }
      got.insert(got.begin() + static_cast<std::ptrdiff_t>(at), def[k]);
    }
    // Parts that cannot move: back at their default index.
    for (size_t k = 0; k < def.size(); ++k)
      if (!part(p, def[k])->movable) got.insert(got.begin() + static_cast<std::ptrdiff_t>(std::min(k, got.size())), def[k]);
    for (const std::string& id : got) {
      const auto it = stored.find(id);
      Part x = it != stored.end() ? it->second : Part{id, part(p, id)->shown, false, {}};
      if (!part(p, id)->movable) x.section.clear();
      out.parts.push_back(x);
    }
  }
  b = out;
}

std::vector<const Part*> sectionParts(Preset p, const Bar& b, const std::string& section) {
  std::vector<const Part*> out;
  for (const Part& x : b.parts)
    if (part(p, x.id) && section == effSection(p, x)) out.push_back(&x);
  return out;
}

const Part* find(const Bar& b, const std::string& id) {
  for (const Part& x : b.parts)
    if (x.id == id) return &x;
  return nullptr;
}

bool shown(const Bar& b, const std::string& id) {
  const Part* x = find(b, id);
  return x && x->shown;
}

bool setShown(Bar& b, const std::string& id, bool on) {
  for (Part& x : b.parts)
    if (x.id == id) {
      if (x.shown == on) return false;
      x.shown = on;
      return true;
    }
  return false;
}

bool setAlways(Bar& b, const std::string& id, bool on) {
  for (Part& x : b.parts)
    if (x.id == id) {
      if (x.always == on) return false;
      x.always = on;
      return true;
    }
  return false;
}

bool moveTo(Preset p, Bar& b, const std::string& id, int to) {
  const PartInfo* info = part(p, id);
  if (!info || !info->movable) return false;
  const std::string sec = sectionOf(p, b, id);
  // The movable parts of its section, as indices into b.parts.
  std::vector<size_t> slots;
  int from = -1;
  for (size_t i = 0; i < b.parts.size(); ++i) {
    const PartInfo* pi = part(p, b.parts[i].id);
    if (!pi || effSection(p, b.parts[i]) != sec || !pi->movable) continue;
    if (b.parts[i].id == id) from = static_cast<int>(slots.size());
    slots.push_back(i);
  }
  if (from < 0) return false;
  to = std::clamp(to, 0, static_cast<int>(slots.size()) - 1);
  if (to == from) return false;
  std::vector<Part> moving;
  for (size_t s : slots) moving.push_back(b.parts[s]);
  const Part m = moving[static_cast<size_t>(from)];
  moving.erase(moving.begin() + from);
  moving.insert(moving.begin() + to, m);
  for (size_t k = 0; k < slots.size(); ++k) b.parts[slots[k]] = moving[k];
  return true;
}

bool moveBeside(Preset p, Bar& b, const std::string& id, const std::string& anchor, bool after) {
  const PartInfo* info = part(p, id);
  if (!info || !info->movable) return false;
  const std::string sec = sectionOf(p, b, id);
  std::vector<std::string> order;  // the movable parts of its section, in order
  for (const Part& x : b.parts)
    if (const PartInfo* pi = part(p, x.id); pi && pi->movable && effSection(p, x) == sec) order.push_back(x.id);
  const auto self = std::find(order.begin(), order.end(), id);
  if (self == order.end()) return false;
  if (anchor == id) return false;
  order.erase(self);  // (moveTo's index counts the others)
  const auto at = std::find(order.begin(), order.end(), anchor);
  const int to = at == order.end() ? static_cast<int>(order.size()) : static_cast<int>(at - order.begin()) + (after ? 1 : 0);
  return moveTo(p, b, id, to);
}

bool moveInto(Preset p, Bar& b, const std::string& id, const std::string& section, const std::string& anchor, bool after) {
  const PartInfo* info = part(p, id);
  if (!info || !info->movable || !find(b, id)) return false;
  const std::string from = sectionOf(p, b, id);
  if (section == from) return moveBeside(p, b, id, anchor, after);
  if (!canMoveInto(p, id, section)) return false;
  const auto self = std::find_if(b.parts.begin(), b.parts.end(), [&](const Part& x) { return x.id == id; });
  Part m = *self;
  m.section = section == info->section ? std::string() : section;
  b.parts.erase(self);
  // Where it goes in the list: beside the anchor in that section, else after the section's last part,
  // else before the first part of a later section (else at the end).
  std::ptrdiff_t at = -1, last = -1;
  for (size_t i = 0; i < b.parts.size(); ++i) {
    if (effSection(p, b.parts[i]) != section) continue;
    last = static_cast<std::ptrdiff_t>(i);
    if (!anchor.empty() && b.parts[i].id == anchor) at = static_cast<std::ptrdiff_t>(i) + (after ? 1 : 0);
  }
  if (at < 0 && last >= 0) at = last + 1;
  if (at < 0) {
    bool later = false;
    for (const SectionInfo& s : sections(p)) {
      if (later)
        for (size_t i = 0; i < b.parts.size() && at < 0; ++i)
          if (effSection(p, b.parts[i]) == s.id) at = static_cast<std::ptrdiff_t>(i);
      if (s.id == section) later = true;
    }
  }
  if (at < 0) at = static_cast<std::ptrdiff_t>(b.parts.size());
  b.parts.insert(b.parts.begin() + at, m);
  normalise(p, b);
  return true;
}

bool step(Preset p, Bar& b, const std::string& id, int dir) {
  const PartInfo* info = part(p, id);
  if (!info || !info->movable || dir == 0) return false;
  const std::string sec = sectionOf(p, b, id);
  int at = 0;
  for (const Part& x : b.parts) {
    const PartInfo* pi = part(p, x.id);
    if (!pi || effSection(p, x) != sec || !pi->movable) continue;
    if (x.id == id) break;
    ++at;
  }
  return moveTo(p, b, id, at + (dir < 0 ? -1 : 1));
}

std::vector<std::vector<std::string>> leaveOrder(Preset p, const Bar& b) {
  std::map<int, std::vector<std::string>> byLevel;
  for (const Part& x : b.parts) {
    const PartInfo* i = part(p, x.id);
    if (!i || !x.shown || x.always || i->fitLevel <= 0) continue;
    byLevel[i->fitLevel].push_back(x.id);
  }
  std::vector<std::vector<std::string>> out;
  for (auto& [lvl, ids] : byLevel) out.push_back(ids);
  return out;
}

std::vector<std::string> fit(Preset p, const Bar& b, const std::function<float(const std::string&)>& width, float room, float fixed) {
  std::set<std::string> gone;
  auto total = [&] {
    float t = fixed;
    for (const Part& x : b.parts)
      if (x.shown && !gone.count(x.id)) t += width(x.id);
    return t;
  };
  std::vector<std::string> out;
  for (const auto& group : leaveOrder(p, b)) {
    if (total() <= room) break;
    for (const std::string& id : group) {
      gone.insert(id);
      out.push_back(id);
    }
  }
  return out;
}

// ---------------------------------------------------------------- pins ----

bool pinnable(const std::string& command) {
  if (command.size() <= 4 || command.size() > kMaxCommand) return false;
  if (command.find('\n') != std::string::npos) return false;
  if (command.compare(0, 4, "win:") == 0) return command.compare(0, 9, "win:chan:") != 0 && command.compare(0, 7, "win:fx:") != 0;  // a project's own channel or effect windows: no
  return command.compare(0, 4, "cmd:") == 0 || (command.size() > 5 && command.compare(0, 5, "menu:") == 0);
}

bool pinned(const std::vector<Pin>& pins, const std::string& command) {
  for (const Pin& p : pins)
    if (p.command == command) return true;
  return false;
}

bool pin(std::vector<Pin>& pins, const Pin& p, int at) {
  if (!pinnable(p.command) || pinned(pins, p.command) || static_cast<int>(pins.size()) >= kMaxPins) return false;
  Pin q = p;
  if (q.label.size() > 120) q.label.resize(120);
  if (at < 0 || at > static_cast<int>(pins.size())) pins.push_back(q);
  else pins.insert(pins.begin() + at, q);
  return true;
}

bool unpin(std::vector<Pin>& pins, const std::string& command) {
  const auto it = std::find_if(pins.begin(), pins.end(), [&](const Pin& p) { return p.command == command; });
  if (it == pins.end()) return false;
  pins.erase(it);
  return true;
}

bool movePin(std::vector<Pin>& pins, int from, int to) {
  const int n = static_cast<int>(pins.size());
  if (from < 0 || from >= n) return false;
  to = std::clamp(to, 0, n - 1);
  if (to == from) return false;
  const Pin p = pins[static_cast<size_t>(from)];
  pins.erase(pins.begin() + from);
  pins.insert(pins.begin() + to, p);
  return true;
}

void cleanPins(std::vector<Pin>& pins, const std::function<bool(const std::string&)>& exists) {
  std::vector<Pin> out;
  for (const Pin& p : pins) {
    if (!pinnable(p.command) || pinned(out, p.command) || static_cast<int>(out.size()) >= kMaxPins) continue;
    if (exists && !exists(p.command)) continue;
    out.push_back(p);
  }
  pins = out;
}

std::vector<std::string> paletteFirst(const std::vector<Pin>& pins) {
  std::vector<std::string> out;
  for (const Pin& p : pins)
    if (p.inPalette()) out.push_back(p.command);
  return out;
}

// ---------------------------------------------------------------- JSON ----

namespace {
Json barValue(const Bar& b) {
  Json a = Json::makeArray();
  for (const Part& x : b.parts) {
    Json o = Json::makeObject();
    o.object["id"] = Json::makeString(x.id);
    o.object["on"] = boolJson(x.shown);
    if (x.always) o.object["always"] = boolJson(true);
    if (!x.section.empty()) o.object["section"] = Json::makeString(x.section);  // moved to another side
    a.array.push_back(o);
  }
  return a;
}

Bar barFrom(Preset p, const Json* a) {
  Bar b;
  if (!a || !a->is(Json::Type::Array)) return defaults(p);
  for (const Json& o : a->array) {
    if (!o.is(Json::Type::Object)) continue;
    Part x;
    x.id = o.text("id");
    const PartInfo* i = part(p, x.id);
    if (!i) continue;
    x.shown = boolOf(o, "on", i->shown);
    x.always = boolOf(o, "always", false);
    x.section = o.text("section");  // (older bars: none, its own side)
    b.parts.push_back(x);
  }
  normalise(p, b);
  return b;
}

Json pinsValue(const std::vector<Pin>& pins) {
  Json a = Json::makeArray();
  for (const Pin& p : pins) {
    Json o = Json::makeObject();
    o.object["command"] = Json::makeString(p.command);
    if (!p.label.empty()) o.object["label"] = Json::makeString(p.label);
    if (p.icon) o.object["icon"] = boolJson(true);
    if (p.where != Where::Both) o.object["where"] = Json::makeString(p.where == Where::Bar ? "bar" : "palette");
    a.array.push_back(o);
  }
  return a;
}

std::vector<Pin> pinsFrom(const Json* a) {
  std::vector<Pin> pins;
  if (!a || !a->is(Json::Type::Array)) return pins;
  for (const Json& o : a->array) {
    if (!o.is(Json::Type::Object)) continue;
    Pin p;
    p.command = o.text("command");
    p.label = o.text("label");
    p.icon = boolOf(o, "icon", false);
    const std::string w = o.text("where");
    p.where = w == "bar" ? Where::Bar : w == "palette" ? Where::Palette : Where::Both;
    pin(pins, p);
  }
  return pins;
}
}  // namespace

std::string barJson(const Bar& b) { return jsonCompact(barValue(b)); }

Bar barFromJson(Preset p, const std::string& text) {
  Json o;
  if (text.empty() || text.size() > 65536 || !jsonParse(text, o)) return defaults(p);
  return barFrom(p, &o);
}

std::string pinsJson(const std::vector<Pin>& pins) { return jsonCompact(pinsValue(pins)); }

std::vector<Pin> pinsFromJson(const std::string& text) {
  Json o;
  if (text.empty() || text.size() > 65536 || !jsonParse(text, o)) return {};
  return pinsFrom(&o);
}

std::string toJson(Preset p, const Model& m) {
  const bool plainBar = m.bar == defaults(p);
  if (plainBar && m.pins.empty()) return "";
  Json o = Json::makeObject();
  if (!plainBar) o.object["parts"] = barValue(m.bar);
  if (!m.pins.empty()) o.object["pins"] = pinsValue(m.pins);
  return jsonCompact(o);
}

Model fromJson(Preset p, const std::string& text) {
  Model m;
  m.bar = defaults(p);
  Json o;
  if (text.empty() || text.size() > 65536 || !jsonParse(text, o) || !o.is(Json::Type::Object)) return m;
  if (const Json* a = o.find("parts")) m.bar = barFrom(p, a);
  m.pins = pinsFrom(o.find("pins"));
  return m;
}

std::string settingsKey(Preset p) { return std::string("layout.") + layout::id(p) + ".toolbar"; }

const std::vector<std::string>& smoothLegacyKeys() {
  static const std::vector<std::string> k = {"ui.cbar.nav", "ui.cbar.toStart", "ui.cbar.cycle", "ui.cbar.countIn",
                                             "ui.cbar.click", "ui.cbar.typing", "ui.cbar.master"};
  return k;
}

Bar migrateSmooth(const std::function<bool(const std::string& key, bool& value)>& value) {
  Bar b = defaults(Preset::Logic);
  for (const std::string& key : smoothLegacyKeys()) {
    bool v = false;
    if (value(key, v)) setShown(b, key.substr(std::string("ui.cbar.").size()), v);
  }
  return b;
}

}  // namespace gl::toolbar
