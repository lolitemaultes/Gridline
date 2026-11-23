// Gridline - the control bar model of each layout (core/Toolbar.h): the defaults are the bars as they
// were, JSON round trips, unknown and repeated parts dropped, reordering inside a section, the narrow
// window order, pins (pinnable ids, no repeats, the palette's list), Smooth's older switches migrated.
#include <map>
#include <string>
#include "Check.h"
#include "core/Toolbar.h"

using namespace gl;
using layout::Preset;
namespace tb = toolbar;

namespace {

std::string ids(const tb::Bar& b) {
  std::string s;
  for (const tb::Part& p : b.parts) s += (s.empty() ? "" : ",") + p.id + (p.shown ? "" : "-");
  return s;
}

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
  return s;
}

}  // namespace

int main() {
  // ---- defaults: the bars of before ----
  expect(ids(tb::defaults(Preset::Studio)) == "menus,transport,pins,tempo,position,pattern,master,monitor,windows",
         "Studio: menus, transport, pins, tempo, position, pattern | master, monitor, windows");
  expect(ids(tb::defaults(Preset::Live)) ==
             "browser,tempo,meter,metronome,quantise,key,position,transport,recording,capture,sessionRecord,recordMenu,pins,back,follow,loop,punch,range,cpu,draw,typing,view",
         "Clips: the left, middle and right groups of before");
  expect(ids(tb::defaults(Preset::Logic)) == "panes,toStart-,nav,cycle,display,countIn,click,typing,pins,master,rightPanes",
         "Smooth: the go to start button off by default, as before");
  expect(ids(tb::defaults(Preset::Tracker)) == "pins", "Tracker: only the pinned commands (in the header's buttons)");
  {
    // Studio's narrow window order: monitor, master, (pins,) window buttons, position, pattern, menus, tempo.
    std::string order;
    for (const auto& g : tb::leaveOrder(Preset::Studio, tb::defaults(Preset::Studio))) order += (order.empty() ? "" : ";") + join(g);
    expect(order == "monitor;master;pins,windows;position;pattern;menus;tempo", "Studio: the groups leave a narrow window in the order of before");
    std::string clips;
    for (const auto& g : tb::leaveOrder(Preset::Live, tb::defaults(Preset::Live))) clips += (clips.empty() ? "" : ";") + join(g);
    expect(clips == "range;key;cpu,draw,typing;loop,punch", "Clips: loop range, key, the three switches together, then loop and punch");
  }
  // ---- JSON ----
  {
    for (Preset p : {Preset::Studio, Preset::Live, Preset::Logic, Preset::Tracker})
      expect(tb::toJson(p, tb::Model{tb::defaults(p), {}}).empty(), "the defaults with no pins save as nothing");
    tb::Model m;
    m.bar = tb::defaults(Preset::Studio);
    tb::setShown(m.bar, "monitor", false);
    tb::setAlways(m.bar, "windows", true);
    tb::step(Preset::Studio, m.bar, "pattern", -1);
    tb::pin(m.pins, tb::Pin{"cmd:edit.undo", "Undo", true, tb::Where::Bar});
    tb::pin(m.pins, tb::Pin{"menu:Tools/Quantize", "Quantize", false, tb::Where::Both});
    const std::string text = tb::toJson(Preset::Studio, m);
    const tb::Model back = tb::fromJson(Preset::Studio, text);
    expect(back == m, "a changed bar and its pins round-trip");
    expect(tb::toJson(Preset::Studio, back) == text, "the text is stable");
    expect(ids(back.bar) == "menus,transport,pins,tempo,pattern,position,master,monitor-,windows", "the order, the off part and the move are kept");
    expect(tb::find(back.bar, "windows")->always, "always shown is kept");
    expect(back.pins[0].icon && back.pins[0].where == tb::Where::Bar && back.pins[1].where == tb::Where::Both, "a pin's look and place are kept");
    expect(tb::fromJson(Preset::Studio, "{broken").bar == tb::defaults(Preset::Studio), "broken text gives the defaults");
    expect(tb::fromJson(Preset::Studio, "[]").pins.empty(), "text that is not an object gives the defaults");
  }
  // ---- unknown, repeated and missing parts ----
  {
    const tb::Bar b = tb::barFromJson(Preset::Studio,
                                      R"([{"id":"windows","on":true},{"id":"gone","on":true},{"id":"menus","on":false},{"id":"windows","on":false},{"id":"tempo"}])");
    expect(ids(b) == "menus-,transport,pins,tempo,position,pattern,master,monitor,windows", "unknown and repeated parts drop, missing ones come back in place");
    const tb::Bar c = tb::barFromJson(Preset::Studio, R"([{"id":"pattern"},{"id":"menus"},{"id":"master"}])");
    expect(ids(c) == "pattern,menus,transport,pins,tempo,position,master,monitor,windows", "missing parts go after their default neighbour");
    const tb::Bar d = tb::barFromJson(Preset::Studio, R"([{"id":"windows"},{"id":"menus"}])");
    expect(tb::sectionParts(Preset::Studio, d, "right").size() == 3 && d.parts.front().id == "menus", "a part stays in its own section");
    const tb::Bar e = tb::barFromJson(Preset::Logic, R"([{"id":"typing"},{"id":"display"},{"id":"panes"},{"id":"click"},{"id":"countIn"},{"id":"pins"}])");
    expect(ids(e) == "panes,toStart-,nav,cycle,display,typing,click,countIn,pins,master,rightPanes", "Smooth: the switches keep their new order, fixed parts their places");
  }
  // ---- moving ----
  {
    tb::Bar b = tb::defaults(Preset::Studio);
    expect(!tb::step(Preset::Studio, b, "menus", -1), "the first part of a section cannot move earlier");
    expect(tb::step(Preset::Studio, b, "menus", 1) && b.parts[0].id == "transport", "a part moves one place later");
    expect(!tb::step(Preset::Studio, b, "pattern", 1), "the last part of the left side does not cross into the right side");
    expect(tb::moveTo(Preset::Studio, b, "windows", 0) && tb::sectionParts(Preset::Studio, b, "right")[0]->id == "windows", "moveTo places a part in its section");
    tb::Bar s = tb::defaults(Preset::Logic);
    expect(!tb::step(Preset::Logic, s, "display", 1) && !tb::moveTo(Preset::Logic, s, "panes", 3), "Smooth's fixed parts do not move");
    expect(tb::step(Preset::Logic, s, "countIn", 1) && ids(s) == "panes,toStart-,nav,cycle,display,click,countIn,typing,pins,master,rightPanes", "Smooth's switches move");
    expect(tb::setShown(s, "master", false) && !tb::shown(s, "master") && !tb::setShown(s, "master", false), "switching a part off (once)");
  }
  // ---- drag and drop: a part lands where the insertion line showed ----
  {
    tb::Bar b = tb::defaults(Preset::Studio);  // left: menus transport pins tempo position pattern
    expect(tb::moveBeside(Preset::Studio, b, "menus", "position", false) && ids(b) == "transport,pins,tempo,menus,position,pattern,master,monitor,windows",
           "dragged down: right before the part under the line");
    expect(tb::moveBeside(Preset::Studio, b, "menus", "transport", true) && ids(b) == "transport,menus,pins,tempo,position,pattern,master,monitor,windows",
           "dragged up: right after the part above the line (a hidden part after it stays after it)");
    expect(tb::moveBeside(Preset::Studio, b, "menus", "pattern", true) && ids(b) == "transport,pins,tempo,position,pattern,menus,master,monitor,windows",
           "dropped after the last part of its group");
    expect(tb::moveBeside(Preset::Studio, b, "pattern", "", false) && ids(b) == "transport,pins,tempo,position,menus,pattern,master,monitor,windows",
           "no part under the line: the end of its group");
    expect(tb::moveBeside(Preset::Studio, b, "pattern", "transport", false) && ids(b) == "pattern,transport,pins,tempo,position,menus,master,monitor,windows",
           "dropped before the first part");
    expect(!tb::moveBeside(Preset::Studio, b, "pattern", "pattern", true) && !tb::moveBeside(Preset::Studio, b, "pattern", "transport", false) &&
               !tb::moveBeside(Preset::Studio, b, "transport", "pattern", true),
           "a drop where it already is changes nothing");
    expect(tb::moveBeside(Preset::Studio, b, "master", "menus", true) && ids(b) == "pattern,transport,pins,tempo,position,menus,monitor,windows,master",
           "a part never leaves its group: an anchor in another group puts it at its group's end");
    tb::Bar s = tb::defaults(Preset::Logic);
    expect(!tb::moveBeside(Preset::Logic, s, "display", "click", true) && !tb::moveBeside(Preset::Logic, s, "master", "rightPanes", true),
           "Smooth: fixed parts do not move");
    expect(tb::moveBeside(Preset::Logic, s, "typing", "countIn", false) && ids(s) == "panes,toStart-,nav,cycle,display,typing,countIn,click,pins,master,rightPanes",
           "Smooth: a switch dragged to the top of its group");
  }
  // ---- fitting a narrow window ----
  {
    const tb::Bar b = tb::defaults(Preset::Live);
    std::map<std::string, float> w;
    for (const tb::Part& p : b.parts) w[p.id] = 50;
    auto width = [&](const std::string& id) { return w[id]; };
    expect(tb::fit(Preset::Live, b, width, 5000).empty(), "a wide window keeps every part");
    expect(join(tb::fit(Preset::Live, b, width, 22 * 50 - 10)) == "range", "a little short: the loop range leaves first");
    expect(join(tb::fit(Preset::Live, b, width, 22 * 50 - 60)) == "range,key", "then the key and scale");
    expect(join(tb::fit(Preset::Live, b, width, 22 * 50 - 110)) == "range,key,cpu,draw,typing", "then the three switches together");
    tb::Bar keep = b;
    tb::setAlways(keep, "key", true);
    expect(join(tb::fit(Preset::Live, keep, width, 22 * 50 - 60)) == "range,cpu,draw,typing", "a part marked always shown stays");
    tb::Bar off = b;
    tb::setShown(off, "range", false);
    expect(join(tb::fit(Preset::Live, off, width, 21 * 50 - 10)) == "key", "a part already off does not count");
    expect(join(tb::fit(Preset::Live, b, width, 10)) == "range,key,cpu,draw,typing,loop,punch", "parts with fit level 0 never leave");
  }
  // ---- pins ----
  {
    expect(tb::pinnable("cmd:edit.undo") && tb::pinnable("menu:Tools/Quantize"), "commands and menu items can be pinned");
    expect(!tb::pinnable("") && !tb::pinnable("cmd:") && !tb::pinnable("set:3/Audio") && !tb::pinnable("win:chan:4") && !tb::pinnable("win:fx:2:1") && !tb::pinnable("plug:gl.trio") && tb::pinnable("win:Mixer"),
           "a row with no id, a setting, a channel or effect window or a plugin cannot be pinned (a window can)");
    std::vector<tb::Pin> pins;
    expect(tb::pin(pins, tb::Pin{"cmd:edit.undo", "Undo"}), "pin a command");
    expect(!tb::pin(pins, tb::Pin{"cmd:edit.undo", "Undo"}), "a command is pinned once");
    expect(!tb::pin(pins, tb::Pin{"", "Nothing"}), "a pinned command with no id is refused");
    expect(tb::pin(pins, tb::Pin{"menu:Tools/Quantize", "Quantize", false, tb::Where::Bar}, 0) && pins[0].command == "menu:Tools/Quantize", "pin at a place");
    expect(tb::pin(pins, tb::Pin{"cmd:view.mixer", "Mixer", false, tb::Where::Palette}), "a palette-only pin");
    expect(join(tb::paletteFirst(pins)) == "cmd:edit.undo,cmd:view.mixer", "the palette lists the pins shown there, in order");
    expect(tb::movePin(pins, 2, 0) && pins[0].command == "cmd:view.mixer", "pins reorder");
    expect(tb::unpin(pins, "cmd:edit.undo") && !tb::pinned(pins, "cmd:edit.undo") && !tb::unpin(pins, "cmd:edit.undo"), "unpin");
    for (int i = 0; i < 40; ++i) tb::pin(pins, tb::Pin{"cmd:x" + std::to_string(i), ""});
    expect(static_cast<int>(pins.size()) == tb::kMaxPins, "the pin list is capped");
    tb::cleanPins(pins, [](const std::string& c) { return c != "cmd:view.mixer"; });
    expect(!tb::pinned(pins, "cmd:view.mixer") && static_cast<int>(pins.size()) == tb::kMaxPins - 1, "a pin whose command is gone is skipped");
    const auto back = tb::pinsFromJson(R"([{"command":"cmd:a.b"},{"command":"cmd:a.b"},{"command":"set:1/x"},{"command":"menu:File/Save","where":"palette"}])");
    expect(back.size() == 2 && back[1].where == tb::Where::Palette, "pins read from text: repeated and unpinnable ones dropped");
  }
  // ---- Smooth's older switches ----
  {
    std::map<std::string, bool> set = {{"ui.cbar.master", false}, {"ui.cbar.toStart", true}};
    const tb::Bar b = tb::migrateSmooth([&](const std::string& k, bool& v) {
      const auto it = set.find(k);
      if (it == set.end()) return false;
      v = it->second;
      return true;
    });
    expect(!tb::shown(b, "master") && tb::shown(b, "toStart") && tb::shown(b, "nav") && tb::shown(b, "click"), "the switches a person set are kept, the rest default");
    const tb::Bar none = tb::migrateSmooth([](const std::string&, bool&) { return false; });
    expect(none == tb::defaults(Preset::Logic), "nothing set: the defaults");
    expect(tb::smoothLegacyKeys().size() == 7, "seven older switches");
  }
  expect(tb::settingsKey(Preset::Live) == "layout.live.toolbar", "settings key per layout");
  // ---- 0.7.8: parts moved from one side of the bar to another ----
  {
    auto side = [](const tb::Bar& b, Preset p, const char* sec) {
      std::string s;
      for (const tb::Part* x : tb::sectionParts(p, b, sec)) s += (s.empty() ? "" : ",") + x->id;
      return s;
    };
    tb::Bar b = tb::defaults(Preset::Studio);
    expect(tb::canMoveInto(Preset::Studio, "transport", "right") && tb::canMoveInto(Preset::Studio, "windows", "left") &&
               !tb::canMoveInto(Preset::Studio, "transport", "middle") && !tb::canMoveInto(Preset::Studio, "nope", "right"),
           "Studio: a part may stand on either side (and nowhere else)");
    expect(tb::moveInto(Preset::Studio, b, "pattern", "right", "monitor", false), "a left part dropped on the right side");
    expect(side(b, Preset::Studio, "left") == "menus,transport,pins,tempo,position" && side(b, Preset::Studio, "right") == "master,pattern,monitor,windows",
           ("it lands right before the part under the line, on that side (" + side(b, Preset::Studio, "right") + ")").c_str());
    expect(tb::sectionOf(Preset::Studio, b, "pattern") == "right" && tb::find(b, "pattern")->section == "right", "the part records its side");
    expect(tb::moveInto(Preset::Studio, b, "windows", "left", "menus", false) && side(b, Preset::Studio, "left") == "windows,menus,transport,pins,tempo,position",
           "a right part dropped before the first part of the left side");
    expect(tb::moveInto(Preset::Studio, b, "master", "left", "", false) && side(b, Preset::Studio, "left") == "windows,menus,transport,pins,tempo,position,master",
           "no anchor: the end of the other side");
    expect(tb::moveBeside(Preset::Studio, b, "pattern", "monitor", true) && side(b, Preset::Studio, "right") == "monitor,pattern",
           "a moved part then reorders inside its new side");
    expect(tb::step(Preset::Studio, b, "master", -1) && side(b, Preset::Studio, "left") == "windows,menus,transport,pins,tempo,master,position",
           "the up / down steps work inside its new side");
    // Saved and read back: the order and the sides; a part back on its own side names none.
    const tb::Model m{b, {}};
    const std::string text = tb::toJson(Preset::Studio, m);
    expect(text.find("\"section\":\"right\"") != std::string::npos && text.find("\"section\":\"left\"") != std::string::npos, "the saved bar names the sides");
    const tb::Model back = tb::fromJson(Preset::Studio, text);
    expect(back == m && side(back.bar, Preset::Studio, "right") == "monitor,pattern", "a bar with moved parts round-trips");
    tb::Bar c = back.bar;
    expect(tb::moveInto(Preset::Studio, c, "pattern", "left", "position", true) && tb::find(c, "pattern")->section.empty(), "moved back home: no side named");
    // Every part on one side: the other is empty, and a part can go back onto it.
    tb::Bar e = tb::defaults(Preset::Studio);
    for (const char* id : {"master", "monitor", "windows"}) expect(tb::moveInto(Preset::Studio, e, id, "left", "", false), (std::string("all to the left: ") + id).c_str());
    expect(side(e, Preset::Studio, "right").empty() && side(e, Preset::Studio, "left") == "menus,transport,pins,tempo,position,pattern,master,monitor,windows",
           "the right side can be left empty");
    expect(tb::moveInto(Preset::Studio, e, "menus", "right", "", false) && side(e, Preset::Studio, "right") == "menus", "a part dropped on an empty side");
    const tb::Bar e2 = tb::barFromJson(Preset::Studio, tb::barJson(e));
    expect(e2 == e, "an empty side round-trips");
    // Old saved bars (no sides) load as before; a side that is not allowed, or unknown, is ignored.
    const tb::Bar old = tb::barFromJson(Preset::Studio, R"([{"id":"pattern"},{"id":"menus"},{"id":"master"}])");
    expect(ids(old) == "pattern,menus,transport,pins,tempo,position,master,monitor,windows" && tb::find(old, "pattern")->section.empty(), "an older bar loads unchanged");
    const tb::Bar bad = tb::barFromJson(Preset::Studio, R"([{"id":"windows","section":"middle"},{"id":"menus","section":"left"}])");
    expect(bad == tb::defaults(Preset::Studio), ("an unknown side, or its own side named, gives its own side (" + ids(bad) + ")").c_str());
    // Reset: the defaults (no sides).
    expect(tb::defaults(Preset::Studio) == tb::barFromJson(Preset::Studio, ""), "Reset gives the bar as designed");
    // Clips: three free groups.
    tb::Bar l = tb::defaults(Preset::Live);
    expect(tb::moveInto(Preset::Live, l, "view", "left", "browser", true) && side(l, Preset::Live, "left") == "browser,view,tempo,meter,metronome,quantise,key",
           "Clips: the view switch to the left group");
    expect(tb::moveInto(Preset::Live, l, "browser", "middle", "position", false) && side(l, Preset::Live, "middle").rfind("browser,position,", 0) == 0,
           "Clips: into the middle group");
    expect(tb::moveInto(Preset::Live, l, "loop", "right", "", false) && side(l, Preset::Live, "right") == "cpu,draw,typing,loop", "Clips: out of the middle group");
    expect(tb::fromJson(Preset::Live, tb::toJson(Preset::Live, tb::Model{l, {}})).bar == l, "Clips: round-trips");
    // Smooth and the Tracker: no free sides; their parts keep their groups.
    tb::Bar s = tb::defaults(Preset::Logic);
    expect(!tb::canMoveInto(Preset::Logic, "click", "right") && !tb::moveInto(Preset::Logic, s, "click", "right", "", false) && s == tb::defaults(Preset::Logic),
           "Smooth: the switches stay in their group");
    expect(tb::moveInto(Preset::Logic, s, "click", "switches", "countIn", false) && ids(s) == "panes,toStart-,nav,cycle,display,click,countIn,typing,pins,master,rightPanes",
           "Smooth: a drop in its own group still moves it");
    tb::Bar t = tb::defaults(Preset::Tracker);
    expect(!tb::moveInto(Preset::Tracker, t, "pins", "header", "", false), "Tracker: nothing moves");
    expect(tb::barFromJson(Preset::Logic, R"([{"id":"click","section":"right"}])") == tb::defaults(Preset::Logic), "Smooth: a saved side is ignored");
  }
  return finish();
}
