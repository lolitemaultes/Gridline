// Gridline tests - K-232 / K-234: the channel rack's select buttons and the selection that
// follows across the rack, the mixer and the piano roll (core/RackSelect.h, core/SelectionFollow.h).
//  * a click on a select button adds the channel, again takes it out; the last one stays;
//  * right-click selects that channel only; Shift+click selects a run;
//  * the typing keyboard plays the followed channel first, then every other selected one;
//  * selecting a channel selects its mixer track; selecting a mixer track selects the channels
//    routed to it (the master and empty tracks leave the channels alone); no feedback loop.
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Keymap.h"
#include "core/RackSelect.h"
#include "core/SelectionFollow.h"
#include "core/Song.h"

using namespace gl;

int main() {
  // ---- K-232: select buttons
  const std::vector<ObjectId> order = {11, 12, 13, 14, 15};
  std::set<ObjectId> sel = {11};
  ObjectId primary = 11;
  rackselect::click(order, sel, primary, 13, rackselect::Click::Toggle);
  expect(sel == std::set<ObjectId>({11, 13}) && primary == 13, "a click adds the channel and the keyboard follows it");
  rackselect::click(order, sel, primary, 14, rackselect::Click::Toggle);
  expect(sel.size() == 3 && primary == 14, "several channels selected by clicking their buttons");
  rackselect::click(order, sel, primary, 14, rackselect::Click::Toggle);
  expect(sel == std::set<ObjectId>({11, 13}) && primary == 11, "a second click takes it out; the first selected one in rack order is followed");
  rackselect::click(order, sel, primary, 11, rackselect::Click::Toggle);
  rackselect::click(order, sel, primary, 13, rackselect::Click::Toggle);
  expect(sel == std::set<ObjectId>({13}) && primary == 13, "the last selected channel stays selected");
  rackselect::click(order, sel, primary, 15, rackselect::Click::Range);
  expect(sel == std::set<ObjectId>({13, 14, 15}) && primary == 15, "Shift+click selects the run");
  rackselect::click(order, sel, primary, 12, rackselect::Click::Only);
  expect(sel == std::set<ObjectId>({12}) && primary == 12, "right-click selects that channel only");
  rackselect::click(order, sel, primary, 99, rackselect::Click::Toggle);
  expect(sel == std::set<ObjectId>({12}), "an unknown channel changes nothing");
  sel = {15, 11, 13};
  expect(rackselect::playTargets(order, sel, 13) == std::vector<ObjectId>({13, 11, 15}), "the keyboard plays the followed channel, then the others in rack order");
  expect(rackselect::playTargets(order, {}, kNoId).empty(), "nothing selected: nothing plays");

  // ---- K-234: selection follows
  Song s = makeEmptySong();
  while (s.mixer.size() < 4) {
    MixerTrack t;
    t.id = s.nextId++;
    s.mixer.push_back(t);
  }
  const ObjectId m1 = s.mixer[1].id, m2 = s.mixer[2].id, m3 = s.mixer[3].id;
  s.channels.clear();
  for (int i = 0; i < 4; ++i) {
    Channel c;
    c.id = s.nextId++;
    c.mixerTrack = i < 2 ? m1 : i == 2 ? m2 : kNoId;
    s.channels.push_back(c);
  }
  const ObjectId c0 = s.channels[0].id, c1 = s.channels[1].id, c2 = s.channels[2].id, c3 = s.channels[3].id;
  selfollow::State st;
  std::set<ObjectId> chs = {c2};
  ObjectId ch = c2, mx = kNoId;
  selfollow::step(s, st, chs, ch, mx);
  expect(mx == m2, "selecting a channel selects its mixer track");
  expect(!selfollow::step(s, st, chs, ch, mx) && chs == std::set<ObjectId>({c2}), "and nothing else moves next frame (no loop)");
  mx = m1;
  selfollow::step(s, st, chs, ch, mx);
  expect(chs == std::set<ObjectId>({c0, c1}) && ch == c0, "selecting a mixer track selects the channels routed to it; the piano roll follows the first");
  expect(!selfollow::step(s, st, chs, ch, mx), "stable after that");
  mx = m3;
  selfollow::step(s, st, chs, ch, mx);
  expect(chs == std::set<ObjectId>({c0, c1}) && ch == c0, "a track no channel routes to leaves the channels as they are");
  mx = s.masterId();
  selfollow::step(s, st, chs, ch, mx);
  expect(chs == std::set<ObjectId>({c0, c1}), "the master leaves the channels as they are");
  ch = c3, chs = {c3};
  selfollow::step(s, st, chs, ch, mx);
  expect(mx == s.masterId(), "a channel on the master selects the master");
  // a channel change and a mixer change in the same frame: the channel wins
  ch = c1, chs = {c1}, mx = m2;
  selfollow::step(s, st, chs, ch, mx);
  expect(mx == m1 && chs == std::set<ObjectId>({c1}), "a channel picked in the same frame as a track: the channel wins");

  // ---- K-234: the mixer's "select routed" shortcuts are bound
  keys::Keymap km;
  bool bound = true;
  for (const char* id : {"mixer.selectRoutedChannels", "mixer.selectRoutedTo"}) {
    const auto& b = km.bindings(id);
    if (b.empty()) {
      bound = false;
      continue;
    }
    const auto hit = km.lookup(keys::Context::Mixer, b.front());
    if (hit.size() != 1 || std::string(hit.front()->id) != id) bound = false;
  }
  expect(bound, "Alt+L and Ctrl+Alt+L are bound to the mixer's select routed commands, with no clash");
  return finish();
}
