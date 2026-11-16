// Gridline - K-237: pane tab strips (core/PaneTabs.h): order, pins, close and reopen, cycling,
// overflow fitting and the saved text form.
#include <string>
#include <vector>
#include "Check.h"
#include "core/LayoutShell.h"
#include "core/PaneTabs.h"

using namespace gl::panetabs;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }
std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
  return s;
}
}  // namespace

int main() {
  Strip s;
  sync(s, {"Mixer", "Piano roll", "Channel rack"});
  expect(join(s.order) == "Mixer,Piano roll,Channel rack" && s.active == "Mixer", "sync: members in order, the first active");
  s.active = "Piano roll";
  sync(s, {"Channel rack", "Piano roll", "Automation"});
  expect(join(s.order) == "Piano roll,Channel rack,Automation", "sync keeps the order, drops gone tabs, adds new ones at the end");
  expect(s.active == "Piano roll", "sync keeps the active tab");
  sync(s, {"Channel rack", "Automation"});
  expect(s.active == "Channel rack", "the active tab gone: the first becomes active");

  // move and pins
  s = Strip{};
  sync(s, {"A", "B", "C", "D"});
  expect(move(s, "D", 0) && join(s.order) == "D,A,B,C", "move to the front");
  expect(!move(s, "D", 0), "a move to the same place does nothing");
  expect(!move(s, "Z", 1), "an unknown tab does not move");
  expect(setPinned(s, "C", true) && join(s.order) == "C,D,A,B", "pinning moves it to the pinned group");
  expect(setPinned(s, "B", true) && join(s.order) == "C,B,D,A", "a second pin goes after the first");
  expect(move(s, "A", 0) && join(s.order) == "C,B,A,D", "an unpinned tab cannot pass the pinned ones");
  expect(move(s, "C", 9) && join(s.order) == "B,C,A,D", "a pinned tab stays among the pinned");
  expect(setPinned(s, "C", false) && join(s.order) == "B,C,A,D" && !s.pinned.count("C"), "unpinning: first after the pinned");
  expect(!setPinned(s, "C", false), "unpinning an unpinned tab does nothing");

  // close and reopen
  History h;
  s.active = "A";
  expect(!close(s, "B", "smooth.lower", h), "a pinned tab cannot be closed");
  expect(close(s, "A", "smooth.lower", h) && join(s.order) == "B,C,D" && s.active == "D", "closing the active tab: its right neighbour");
  expect(close(s, "D", "smooth.lower", h) && s.active == "C", "... or the left one at the end");
  Strip one;
  sync(one, {"Only"});
  expect(!close(one, "Only", "x", h), "the last tab stays");
  Closed c = h.pop();
  expect(c.window == "D" && c.region == "smooth.lower" && c.index == 2, "the history gives the last closed tab first");
  reopen(s, c);
  expect(join(s.order) == "B,C,D" && s.active == "D", "reopen puts it back where it was, active");
  c = h.pop();
  reopen(s, c);
  expect(join(s.order) == "B,C,A,D" && s.active == "A", "and the one before");
  expect(h.empty() && h.pop().window.empty(), "an empty history gives nothing");
  for (int i = 0; i < 30; ++i) h.push({"r", "W" + std::to_string(i), 0});
  expect(h.items().size() == History::kMax && h.items().front().window == "W10", "the history keeps the last 20");
  h.push({"r", "W15", 3});
  expect(h.items().size() == History::kMax && h.items().back().window == "W15" && h.items().back().index == 3, "a window is in the history once");
  h.forget("W15");
  expect(h.items().size() == History::kMax - 1, "forget drops a window");

  // cycle
  s.active = "D";
  expect(cycle(s, 1) == "B" && cycle(s, -1) == "A", "cycling wraps both ways");

  // fit
  Strip f;
  sync(f, {"A", "B", "C", "D", "E"});
  const std::vector<float> w = {100, 100, 100, 100, 100};
  expect(fit(f, w, 500, 30).size() == 5, "all tabs fit: no overflow");
  auto shown = fit(f, w, 330, 30);
  expect(shown == std::vector<int>({0, 1, 2}), "three fit beside the overflow button");
  f.active = "E";
  shown = fit(f, w, 330, 30);
  expect(shown == std::vector<int>({0, 1, 4}), "the active tab always shows");
  setPinned(f, "D", true);  // order D,A,B,C,E
  shown = fit(f, w, 330, 30);
  expect(shown.size() == 3 && shown[0] == 0 && shown.back() == 4, "pinned tabs always show");

  // text form
  Strip t;
  sync(t, {"Mixer", "Piano roll", "Channel rack"});
  setPinned(t, "Channel rack", true);
  t.active = "Piano roll";
  const std::string txt = save(t);
  expect(txt == "Channel rack|Mixer|Piano roll;pinned=Channel rack;active=Piano roll", "saved text: " + txt);
  expect(load(txt) == t, "load reads back what save wrote");
  const Strip bad = load("A|A|B;pinned=Z|B;active=Q;junk");
  expect(join(bad.order) == "B,A" && bad.pinned.size() == 1 && bad.active == "B", "load drops duplicates, unknown pins and a lost active tab");
  expect(load("").order.empty(), "empty text: an empty strip");

  // the pane state in the layout's region state (layout.<id>.state)
  using gl::layout::Preset;
  gl::layout::State st;
  st.tabs["logic.lower"] = txt;
  st.maximised.insert("logic.lower");
  const std::string js = gl::layout::toJson(st);
  expect(gl::layout::fromJson(Preset::Logic, js) == st, "tabs and maximised panes survive the JSON round trip: " + js);
  const gl::layout::State dropped = gl::layout::fromJson(Preset::Logic, R"({"tabs":{"nowhere":"A|B","logic.lower":7},"max":["nowhere"]})");
  expect(dropped == gl::layout::State{}, "unknown regions and broken entries are dropped");
  expect(gl::layout::toJson(gl::layout::State{}).empty(), "the default state is still empty text");
  const gl::layout::Region* lower = gl::layout::findRegion(Preset::Logic, "logic.lower");
  expect(lower != nullptr, "the Smooth lower pane exists");
  if (lower) {
    gl::layout::State mx;
    mx.maximised.insert("logic.lower");
    mx.shown.insert("logic.lower");
    const float normal = gl::layout::regionExtent(*lower, 800, gl::layout::State{});
    const float big = gl::layout::regionExtent(*lower, 800, mx);
    expect(big > normal && big <= 800 * gl::layout::kMaximisedShare && big < 799, "a maximised pane grows but leaves the main area room");
    bool ok = true;
    for (const auto& pl : gl::layout::solve(Preset::Logic, 1200, 800, mx))
      if (pl.id == gl::layout::shell(Preset::Logic).centre) ok = pl.rect.h >= 800 * (1 - gl::layout::kMaximisedShare) - 1 && pl.rect.w > 0;
    expect(ok, "the main area keeps its share beside a maximised pane");
  }
  return finish();
}
